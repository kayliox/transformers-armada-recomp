// Placing the player (test_place.h).
//
// taTransformerData::Update (0x0047D9A0, context in a0) runs in the states' updates (walking,
// standing, the turret, ...). Wrapped: the original, then, when a placement is due, the actor's
// SetPosition (its vtable +0x80, as the turret state uses it) and the view's heading (context
// +0x11C). Added code (guest_code.h): registered at every start whether TF_PLACE is set or not.

#include "test_place.h"

#include "guest_code.h"
#include "port.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_memory.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "game_version.h" // (TF_GA: the code addresses that differ in the PAL release)

namespace test_place
{
    namespace
    {
        constexpr uint32_t kDataUpdate = TF_GA(0x0047D9A0u);
        constexpr uint32_t kSetPositionSlot = 0x80u, kContextHeading = 0x11Cu;

        struct Place
        {
            uint64_t tick = 0u;
            float position[3] = {};
            float heading = 0.0f;
            bool turn = false, done = false;
            bool after = false; // "+N": the tick counts from the placement before (see parse)
        };
        std::vector<Place> g_places;
        uint64_t g_lastPlaced = 0u; // the tick of the last placement, or of the level's first update

        // The state block: the caller's ra, the context, the position handed to SetPosition.
        enum : uint32_t
        {
            sRa = 0x00, sContext = 0x04, sPos = 0x10, kSize = 0x20
        };
        uint32_t g_state = 0u, g_fn = 0u, g_original = 0u;

        uint32_t word(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v = 0u;
            if (const uint8_t *p = getConstMemPtr(rdram, a))
                std::memcpy(&v, p, sizeof(v));
            return v;
        }

        void setWord(uint8_t *rdram, uint32_t a, uint32_t v)
        {
            if (uint8_t *p = getMemPtr(rdram, a))
                std::memcpy(p, &v, sizeof(v));
        }

        void setFloat(uint8_t *rdram, uint32_t a, float f)
        {
            uint32_t v;
            std::memcpy(&v, &f, sizeof(v));
            setWord(rdram, a, v);
        }

        void parse()
        {
            const char *env = std::getenv("TF_PLACE");
            for (const char *p = env; p && *p;)
            {
                Place place;
                char *end = nullptr;
                // "+N": N ticks after the placement before it (the first: after the level's first update) -
                // the level's own time, whatever the menus and the load took (TODO 106's scripted runs).
                place.after = *p == '+';
                place.tick = std::strtoull(place.after ? p + 1 : p, &end, 10);
                if (end == p || *end != ':')
                    break;
                for (int i = 0; i < 3; ++i)
                {
                    place.position[i] = std::strtof(end + 1, &end);
                    if (i < 2 && *end != ',')
                        return;
                }
                if (*end == ',')
                {
                    place.heading = std::strtof(end + 1, &end) * 3.14159265f / 180.0f;
                    place.turn = true;
                }
                g_places.push_back(place);
                if (*end != ';')
                    break;
                p = end + 1;
            }
        }

        void updateFn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            uint32_t pc = ctx->pc;
            if (pc == kDataUpdate)
            {
                setWord(rdram, g_state + sRa, getRegU32(ctx, 31));
                setWord(rdram, g_state + sContext, getRegU32(ctx, 4));
                if (!guest_code::call(rdram, ctx, runtime, g_original, g_fn + 4u))
                    return;
                pc = ctx->pc;
            }
            if (pc == g_fn + 4u)
            {
                const uint64_t tick = tf::port::testTickNow(); // (the testing aids' tick: 60 a second of game time at every rate)
                const uint32_t context = word(rdram, g_state + sContext);
                const uint32_t actor = context ? word(rdram, context) : 0u;
                if (actor && g_lastPlaced == 0u)
                    g_lastPlaced = tick;
                for (Place &place : g_places)
                {
                    if (place.done || !actor)
                        continue;
                    if (place.after ? tick < g_lastPlaced + place.tick : tick < place.tick)
                        break; // (in order: a later one waits for this one)
                    place.done = true;
                    g_lastPlaced = tick;
                    for (int i = 0; i < 3; ++i)
                        setFloat(rdram, g_state + sPos + 4u * i, place.position[i]);
                    if (place.turn)
                        setFloat(rdram, context + kContextHeading, place.heading);
                    std::fprintf(stderr, "[tf:place] tick %llu: the player to (%.1f, %.1f, %.1f)%s\n",
                                 static_cast<unsigned long long>(tick), place.position[0], place.position[1], place.position[2],
                                 place.turn ? ", the view turned" : "");
                    SET_GPR_U32(ctx, 4, actor);
                    SET_GPR_U32(ctx, 5, g_state + sPos);
                    if (!guest_code::call(rdram, ctx, runtime, word(rdram, word(rdram, actor) + kSetPositionSlot), g_fn + 8u))
                        return;
                    pc = ctx->pc;
                    break;
                }
                if (pc == g_fn + 4u)
                    pc = g_fn + 8u;
            }
            if (pc == g_fn + 8u)
                ctx->pc = word(rdram, g_state + sRa);
        }
    }

    void install(PS2Runtime &runtime)
    {
        parse();
        g_state = guest_code::addState("test placement", kSize);
        g_fn = guest_code::addResumable(runtime, "taTransformerData::Update (+ TF_PLACE)", updateFn, 2);
        if (PS2Runtime::RecompiledFunction original = runtime.lookupFunction(kDataUpdate); original && !g_places.empty())
        {
            // (the name the unused one has below: the names are part of a save state's layout key, and with another
            // name here no state made without TF_PLACE loaded in a run with it - "a different memory layout")
            g_original = guest_code::addFunction(runtime, "taTransformerData::Update (original, unused)", original);
            runtime.registerFunction(kDataUpdate, updateFn);
        }
        else
            guest_code::addFunction(runtime, "taTransformerData::Update (original, unused)", updateFn); // (the same addresses either way)
    }
}
