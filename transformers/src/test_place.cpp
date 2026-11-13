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

        // ---- TF_PD_FORCE_RAGDOLL (a testing aid, test_place.h) ----
        // 0x28E260, the transformer actor's update, has the developers' ragdoll test: with
        // debugRagdollMode set (0x277B50 on the actor's +0x1A8 answers its value) and the input commands
        // 0x3B (the input's vtable +0x10, 0x34B1C0) and 0x26 (+0x18, 0x34B0C0) both on, it leaves the current
        // state and its motion shaders and enters state 10, the ragdoll (Collapse: nothing more; Grenade,
        // Rocket: a push). The aid answers those three calls - only from there, by their return addresses -
        // as if the mode were set and both commands on, once at each listed tick; everything else the
        // game does itself. Unset: nothing is registered.
        constexpr uint32_t kRagdollMode = TF_GA(0x00277B50u), kCommandA = TF_GA(0x0034B1C0u), kCommandB = TF_GA(0x0034B0C0u), kActorUpdate = TF_GA(0x0028E260u);
        constexpr uint32_t kModeReturn = 0x0028E51Cu, kCommandAReturn = 0x0028E540u, kCommandBReturn = 0x0028E564u;
        struct ForcedRagdoll
        {
            uint64_t tick = 0u;
            uint32_t mode = 1u; // 1 Collapse, 2 Grenade, 3 Rocket
            bool death = false; // "death": the player's health to 0 as he goes down
            bool done = false;
        };
        std::vector<ForcedRagdoll> g_ragdolls;
        ForcedRagdoll *g_armed = nullptr; // between the mode answer and the second command in one update
        PS2Runtime::RecompiledFunction g_previousMode = nullptr, g_previousA = nullptr, g_previousB = nullptr, g_previousUpdate = nullptr;

        void parseRagdolls()
        {
            // "tick[:mode][,tick[:mode]...]", mode collapse|grenade|rocket or 1..3 (default collapse), or death (collapse, health 0).
            const char *env = std::getenv("TF_PD_FORCE_RAGDOLL");
            for (const char *p = env; p && *p;)
            {
                ForcedRagdoll r;
                char *end = nullptr;
                r.tick = std::strtoull(p, &end, 10);
                if (end == p || r.tick == 0u)
                    break;
                if (*end == ':')
                {
                    const char *m = end + 1;
                    if (!std::strncmp(m, "death", 5)) { r.death = true; end = const_cast<char *>(m + 5); }
                    else if (!std::strncmp(m, "grenade", 7)) { r.mode = 2u; end = const_cast<char *>(m + 7); }
                    else if (!std::strncmp(m, "rocket", 6)) { r.mode = 3u; end = const_cast<char *>(m + 6); }
                    else if (!std::strncmp(m, "collapse", 8)) { r.mode = 1u; end = const_cast<char *>(m + 8); }
                    else { r.mode = static_cast<uint32_t>(std::strtoul(m, &end, 10)); if (r.mode < 1u || r.mode > 3u) r.mode = 1u; }
                }
                g_ragdolls.push_back(r);
                if (*end != ',')
                    break;
                p = end + 1;
            }
        }

        void answer(R5900Context *ctx, uint32_t value)
        {
            SET_GPR_U32(ctx, 2, value);
            ctx->pc = getRegU32(ctx, 31);
        }

        void ragdollMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_armed = nullptr;
            if (getRegU32(ctx, 31) == kModeReturn)
            {
                const uint64_t tick = tf::port::testTickNow();
                for (ForcedRagdoll &r : g_ragdolls)
                    if (!r.done && tick >= r.tick)
                    {
                        g_armed = &r;
                        answer(ctx, r.mode);
                        return;
                    }
            }
            g_previousMode(rdram, ctx, runtime);
        }

        void commandA(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_armed && getRegU32(ctx, 31) == kCommandAReturn)
                return answer(ctx, 1u);
            g_previousA(rdram, ctx, runtime);
        }

        void commandB(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_armed && getRegU32(ctx, 31) == kCommandBReturn)
            {
                g_armed->done = true;
                if (g_armed->death)
                {
                    // The player's health (the float the HUD shows) to 0: the game's own death follows.
                    const float zero = 0.0f;
                    if (uint8_t *h = getMemPtr(rdram, 0x00716FB4u))
                        std::memcpy(h, &zero, sizeof(zero));
                }
                std::fprintf(stderr, "[tf:ragdoll] tick %llu: the player knocked down (TF_PD_FORCE_RAGDOLL, mode %u)\n",
                             static_cast<unsigned long long>(tf::port::testTickNow()), g_armed->mode);
                g_armed = nullptr;
                return answer(ctx, 1u);
            }
            g_previousB(rdram, ctx, runtime);
        }

        // TF_PD_STATE_LOG=1 (or with TF_PD_FORCE_RAGDOLL): a line whenever a transformer actor's state (+0xF8)
        // differs from the one at its previous update - read as each update starts, so even a one-update ragdoll
        // is listed with its tick.
        struct SeenState
        {
            uint32_t actor = 0u, state = 0xFFFFFFFFu;
        };
        SeenState g_seen[4];

        // TF_PD_FORCE_STATE="tick:s<state>|e5|e21,..." (a testing aid, off when unset): at that tick the player's actor
        // goes to that state of its table (taTransformerActor::SetState, 0x28D000), or takes the flag that starts
        // creature effect 5 (a hit's flash: +0x172) or 21 (the water's hurt: +0x173), as the game's own code sets them.
        struct ForcedState
        {
            uint64_t tick = 0u;
            int state = -1, flag = 0;
            bool done = false;
        };
        std::vector<ForcedState> g_forced;
        PS2Runtime::RecompiledFunction g_setStateFn = nullptr;
        constexpr uint32_t kSetStateFn = 0x0028D000u;
        void parseForced()
        {
            for (const char *p = std::getenv("TF_PD_FORCE_STATE"); p && *p;)
            {
                ForcedState f;
                char *end = nullptr;
                f.tick = std::strtoull(p, &end, 10);
                if (end == p || *end != ':' || (end[1] != 's' && end[1] != 'e'))
                    break;
                const bool flag = end[1] == 'e';
                const int n = static_cast<int>(std::strtol(end + 2, &end, 10));
                if (flag)
                    f.flag = n == 21 ? 0x173 : 0x172;
                else
                    f.state = n;
                g_forced.push_back(f);
                if (*end != ',')
                    break;
                p = end + 1;
            }
        }
        // The player's actor (taPlayerManager's first player, its creature's +0x54), else 0.
        uint32_t playerActor(uint8_t *rdram)
        {
            const uint32_t manager = word(rdram, 0x006CF498u);
            if (!manager)
                return 0u;
            const uint32_t cursor = word(rdram, manager + 8u);
            if (!cursor || cursor == manager + 4u)
                return 0u;
            const uint32_t player = word(rdram, cursor + 8u);
            const uint32_t creature = player ? word(rdram, player + 8u) : 0u;
            return creature ? word(rdram, creature + 0x54u) : 0u;
        }
        void applyForced(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t actor)
        {
            const uint64_t tick = tf::port::testTickNow();
            for (ForcedState &f : g_forced)
                if (!f.done && tick >= f.tick && actor == playerActor(rdram))
                {
                    f.done = true;
                    std::fprintf(stderr, "[tf:state] tick %llu: TF_PD_FORCE_STATE %s %d\n", static_cast<unsigned long long>(tick), f.flag ? "effect flag" : "state", f.flag ? f.flag : f.state);
                    if (f.flag)
                    {
                        if (uint8_t *b = getMemPtr(rdram, actor + static_cast<uint32_t>(f.flag)))
                            *b = 1u;
                    }
                    else if (g_setStateFn)
                    {
                        decltype(ctx->r) regs;
                        decltype(ctx->f) floats;
                        std::memcpy(regs, ctx->r, sizeof(regs));
                        std::memcpy(floats, ctx->f, sizeof(floats));
                        const uint32_t pc = ctx->pc;
                        SET_GPR_U32(ctx, 4, actor);
                        SET_GPR_U32(ctx, 5, static_cast<uint32_t>(f.state));
                        ctx->pc = kSetStateFn;
                        g_setStateFn(rdram, ctx, runtime);
                        std::memcpy(ctx->r, regs, sizeof(regs));
                        std::memcpy(ctx->f, floats, sizeof(floats));
                        ctx->pc = pc;
                    }
                    return;
                }
        }

        void actorUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t actor = getRegU32(ctx, 4), state = word(rdram, actor + 0xF8u);
            if (!g_forced.empty())
                applyForced(rdram, ctx, runtime, actor);
            SeenState *slot = nullptr;
            for (SeenState &s : g_seen)
                if (s.actor == actor) { slot = &s; break; }
            if (!slot)
                for (SeenState &s : g_seen)
                    if (!s.actor) { slot = &s; break; }
            if (!slot)
                slot = &g_seen[0], *slot = {};
            if (slot->actor != actor || slot->state != state)
            {
                std::fprintf(stderr, "[tf:state] tick %llu: actor %08x state %d -> %u%s\n",
                             static_cast<unsigned long long>(tf::port::testTickNow()), actor,
                             slot->actor == actor ? static_cast<int>(slot->state) : -1, state,
                             state == 10u ? " (ragdoll)" : (slot->state == 10u && slot->actor == actor ? " (up from the ragdoll)" : ""));
                slot->actor = actor;
                slot->state = state;
            }
            g_previousUpdate(rdram, ctx, runtime);
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
        parseRagdolls();
        parseForced();
        // (Plain wrappers, no added guest code: registered only when asked for.)
        if (!g_ragdolls.empty() && runtime.hasFunction(kRagdollMode) && runtime.hasFunction(kCommandA) && runtime.hasFunction(kCommandB))
        {
            g_previousMode = runtime.lookupFunction(kRagdollMode);
            g_previousA = runtime.lookupFunction(kCommandA);
            g_previousB = runtime.lookupFunction(kCommandB);
            if (g_previousMode && g_previousA && g_previousB)
            {
                runtime.registerFunction(kRagdollMode, ragdollMode);
                runtime.registerFunction(kCommandA, commandA);
                runtime.registerFunction(kCommandB, commandB);
                std::fprintf(stderr, "[tf:ragdoll] TF_PD_FORCE_RAGDOLL: %zu knockdown(s) scheduled\n", g_ragdolls.size());
            }
        }
        const char *stateLog = std::getenv("TF_PD_STATE_LOG");
        if (!g_forced.empty() && runtime.hasFunction(kSetStateFn))
            g_setStateFn = runtime.lookupFunction(kSetStateFn);
        if (((stateLog && *stateLog == '1') || !g_ragdolls.empty() || !g_forced.empty()) && runtime.hasFunction(kActorUpdate) &&
            (g_previousUpdate = runtime.lookupFunction(kActorUpdate)) != nullptr)
            runtime.registerFunction(kActorUpdate, actorUpdate);
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
