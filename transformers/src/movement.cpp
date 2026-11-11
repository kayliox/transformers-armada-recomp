// The player's movement on a modern controller (docs/TODO.md item 41).
//
// "Walks instead of running, sometimes, with the stick fully pushed": the walk and run states ask
// PickDesiredSpeed (taTransformerStateMoveOnGround, 0x00476700) for the speed to reach - the record's
// walk speed (+0x120) once the move vector (context +0x190) is not zero, its run speed (+0x124) only
// when that vector is longer than 0.99 (gp-0x7904 = 0x6CB7EC). The vector comes from the left stick
// through taTransformerData::Update (0x0047DCxx): the stick's length past the record's dead zone
// (+0x100) rescaled to 0..1 and CUBED (0x0047DD30). So the run needs the stick at 99.7% of its travel
// or more: a DualShock 2's square gate gives that in every direction (its corners reach 1.4), a
// modern pad's round gate only where it is calibrated to the very edge. Measured (TF_INPUT, run41
// on the other PC): the stick at 97% -> 0.887, at 99% -> 0.931, a diagonal 0.70/0.70 -> 0.930 - all
// walk for good; 100% -> 1.000 runs. The user's DualSense (native, Bluetooth) falls short in some
// directions, hence "letting the stick go or changing direction a few times brings the run back".
//
// Here: PickDesiredSpeed as the game has it, with the run past 0.85 (the stick at about 95% of its
// travel) instead of 0.99; walking with the stick part way pushed is unchanged. TF_RUN_THRESHOLD
// overrides 0.85 (0.99 = the PS2's). Decomp equivalent: the constant at 0x6CB7EC, or the same test in
// taTransformerStateMoveOnGround::PickDesiredSpeed.

#include "movement.h"
#include "runtime/ps2_stick_response.h"
#include <chrono>

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_memory.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "game_version.h" // (TF_GA: the code addresses that differ in the PAL release)

namespace movement
{
    namespace
    {
        constexpr uint32_t kPickDesiredSpeed = TF_GA(0x00476700u);
        constexpr uint32_t kRecords = 0x00717430u + 0x60u; // func_0034F2B0's database, func_004A57D0's records
        constexpr uint32_t kRecordSize = 0x240u;            // (record = kRecords + id * 0x240, id = context +0x18)
        float g_runThreshold = 0.85f;

        bool valid(uint32_t a) { return a >= 0x00100000u && a < PS2_RAM_SIZE && !(a & 3u); }
        float fl(const uint8_t *rdram, uint32_t a)
        {
            float v = 0.0f;
            if (valid(a))
                if (const uint8_t *p = getConstMemPtr(rdram, a))
                    std::memcpy(&v, p, sizeof(v));
            return v;
        }
        uint16_t half(const uint8_t *rdram, uint32_t a)
        {
            uint16_t v = 0u;
            if (a >= 0x00100000u && a < PS2_RAM_SIZE && !(a & 1u))
                if (const uint8_t *p = getConstMemPtr(rdram, a))
                    std::memcpy(&v, p, sizeof(v));
            return v;
        }
        uint32_t word32(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v = 0u;
            if (valid(a))
                if (const uint8_t *p = getConstMemPtr(rdram, a))
                    std::memcpy(&v, p, sizeof(v));
            return v;
        }

        // PickDesiredSpeed(state a0, context a1) -> f0.
        void pickDesiredSpeed(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t context = getRegU32(ctx, 5);
            const uint32_t record = kRecords + kRecordSize * half(rdram, context + 0x18u);
            const float x = fl(rdram, context + 0x190u), y = fl(rdram, context + 0x194u), z = fl(rdram, context + 0x198u);
            const float length = std::sqrt(x * x + y * y + z * z), scale = fl(rdram, context + 0x1FCu);
            float speed = 0.0f;
            if (length > 0.0f)
                speed = scale * fl(rdram, record + 0x120u);
            if (length > g_runThreshold)
                speed = scale * fl(rdram, record + 0x124u);
            ctx->f[0] = speed;
            ctx->pc = getRegU32(ctx, 31);
        }
    }

    namespace
    {
        // TF_SCREEN_LOG=1 (tests driven without a picture): each menu screen the game goes to -
        // taUISystemBase::GoToScreen (0x318090) and taUIInGame::GoToScreen (0x315E00), the name in
        // a1 (a std::string: size +4, characters at the pointer +8).
        constexpr uint32_t kGoToScreen = 0x00318090u, kGoToScreenInGame = 0x00315E00u;
        PS2Runtime::RecompiledFunction g_goToScreen = nullptr, g_goToScreenInGame = nullptr;
        void logScreen(const uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const char *which)
        {
            const uint32_t s = getRegU32(ctx, 5);
            uint32_t size = 0u, data = 0u;
            if (valid(s + 4u) && valid(s + 8u))
            {
                std::memcpy(&size, getConstMemPtr(rdram, s + 4u), 4);
                std::memcpy(&data, getConstMemPtr(rdram, s + 8u), 4);
            }
            std::string name;
            for (uint32_t i = 0; i < size && i < 64u && data >= 0x00100000u && data + i < PS2_RAM_SIZE; ++i)
                name.push_back(static_cast<char>(*getConstMemPtr(rdram, data + i)));
            std::fprintf(stderr, "[screen] vblank %llu %s '%s'\n",
                         static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()), which, name.c_str());
        }
        void goToScreen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kGoToScreen)
                logScreen(rdram, ctx, runtime, "front end");
            g_goToScreen(rdram, ctx, runtime);
        }
        void goToScreenInGame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kGoToScreenInGame)
                logScreen(rdram, ctx, runtime, "in game");
            g_goToScreenInGame(rdram, ctx, runtime);
        }
    }

    namespace
    {
        // ---- The car's wheels after each step (docs/TODO.md item 40, the car that does not move) ----
        // Each frame the game steps the cars (0x3231D0: reCar::Update 0x319D70 per car - its wheels'
        // forces from their ground contacts) and then places them (0x3230D0: per car reCar::
        // PostCollisionUpdate 0x319D10 -> reCarPhysics::PostCollisionUpdate 0x31EF10, which
        // GENERATES THE WHEELS' GROUND CONTACTS (GenerateWheelStates 0x31D360) for the next step,
        // then copies the body's place to the vehicle). At the PC rate the port steps the cars every
        // 30th of a second (overrides.cpp, "The cars") and, in the frames between, moves each body on
        // for the picture instead of calling the placing - and it also skips it on the frame of a
        // step whenever the step falls inside the frame (a rate that is not a multiple of 30: 100,
        // 90, 50, 45...) or the count to the next step was carried over a change of rate (the rate
        // ladder: 120 -> 60 turns a count of 30 into 15, and from then on every step falls inside a
        // frame). Then the wheels' contacts are never generated again: the car keeps the ones it
        // had (frozen, often "in the air"), its tyres get no grip, it slides like a sled or sits
        // still with the pedal down - the stuck car. Measured with the user's save (Hot Shot and
        // Optimus alike) and with new games: the runs whose rate had left a multiple of 30 had no
        // wheel down for minutes; the wheels' heights stopped changing while the car moved.
        //
        // Here: on a frame on which the cars were stepped (reCar::Update ran) the game's own placing
        // runs (the wheels' contacts made from the stepped bodies, as on the console after every
        // step); on the frames between, the port's moving-on as before. The port's placing is
        // wrapped from the first car update on (it is registered after this module installs).
        constexpr uint32_t kCarUpdate = 0x00319D70u, kCarsPlace = 0x003230D0u;
        PS2Runtime::RecompiledFunction g_carUpdate = nullptr, g_gamePlace = nullptr, g_portPlace = nullptr, g_placeTaken = nullptr;
        bool g_steppedThisFrame = false, g_placeWrapped = false;
        uint64_t g_placesMade = 0u;

        void carsPlace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kCarsPlace)
            {
                g_placeTaken = g_steppedThisFrame || !g_portPlace ? g_gamePlace : g_portPlace;
                if (g_steppedThisFrame && g_placesMade++ == 0u && std::getenv("TF_CAR_TRACE"))
                    std::fprintf(stderr, "[movement] the cars' wheels made after a step (the game's placing)\n");
                g_steppedThisFrame = false;
            }
            g_placeTaken(rdram, ctx, runtime);
        }

        void carUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kCarUpdate)
            {
                g_steppedThisFrame = true;
                if (!g_placeWrapped)
                {
                    g_placeWrapped = true;
                    g_portPlace = runtime->lookupFunction(kCarsPlace); // (the port's, or the game's at 30)
                    if (g_portPlace && g_portPlace != carsPlace)
                        runtime->registerFunction(kCarsPlace, carsPlace);
                }
            }
            g_carUpdate(rdram, ctx, runtime);
        }
    }

    namespace
    {
        // ---- "Planet gravity everywhere" (the game's cheat forcePlanetaryGravity; docs/TODO.md item 79) ----
        // The level's planetary-gravity test (0x4C7EF0: the environment's HasPlanetaryGravity +0xAA, or the
        // cheat) is read by taGame::Initialise (game +0xC1) and taTransformerActor::InitAnimation (context
        // +0x21B, the top speed +0x224 = 20000). Gravity then pulls toward the planet's centre - Cybertron's
        // globe, at the world's origin. Outside Cybertron there is no planet: measured in Amazon (TF_CFG_
        // forcePlanetaryGravity=1, run pg79_1) the Autobot is pulled sideways toward the origin, the camera
        // turns with "down" (the horizon upright), and he is lost off the map at once ("Stasis lock - energon
        // depleted", vblank 10457, with god mode on). On Cybertron the level has it anyway. So the cheat
        // cannot work anywhere it would change something: the test here answers the level's own value only.
        // TF_PLANET_GRAVITY_ANYWHERE=1: the cheat as the game has it (to see the breakage).
        constexpr uint32_t kPlanetGravity = TF_GA(0x004C7EF0u);
        void planetGravity(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t environment = getRegU32(ctx, 4);
            uint8_t own = 0u;
            if (environment >= 0x00100000u && environment < PS2_RAM_SIZE)
                if (const uint8_t *p = getConstMemPtr(rdram, environment + 0xAAu))
                    own = *p;
            setReturnU32(ctx, own != 0u ? 1u : 0u);
            ctx->pc = getRegU32(ctx, 31);
        }
    }

    namespace
    {
        // The controller getter identifies the human player without scanning memory. The camera's
        // script changes later, and also calls a mounted jet bike a vehicle. Only the transformer's
        // car/transform states select this curve; the bike keeps robot response (as in carstick2).
        constexpr uint32_t kControllerGetter = TF_GA(0x0034B040u), kAxisGetter = 0x00197DC0u, kReadPad = 0x0013A898u;
        PS2Runtime::RecompiledFunction g_controllerGetter = nullptr, g_axisGetter = nullptr, g_readPad = nullptr;
        uint32_t g_controller = 0u, g_axisX = 0u, g_axisY = 0u;
        std::chrono::steady_clock::time_point g_controllerSeen{};
        bool span(uint32_t a, uint32_t n) { return a >= 0x100000u && n <= 0x2000000u && a <= 0x2000000u - n; }
        uint32_t playerActor(const uint8_t *r, uint32_t ctl)
        {
            if (!span(ctl, 12) || word32(r, ctl) != 0x6B8000u) return 0;
            const auto creature = word32(r, ctl + 8);
            if (!span(creature, 0x58)) return 0;
            const auto binding = word32(r, creature + 0x38), actor = word32(r, creature + 0x54);
            if (!span(binding, 4) || word32(r, binding) != 0 || !span(actor, 0xFC)) return 0;
            const auto context = word32(r, actor + 0x7C);
            if (!span(context, 0xD8)) return 0;
            return word32(r, context) == actor && word32(r, context + 4) == creature &&
                   word32(r, context + 0xD4) == ctl ? actor : 0;
        }
        void controllerGetter(uint8_t *r, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kControllerGetter && playerActor(r, getRegU32(ctx, 4)))
            {
                g_controller = getRegU32(ctx, 4);
                g_controllerSeen = std::chrono::steady_clock::now();
            }
            g_controllerGetter(r, ctx, runtime);
        }
        void axisGetter(uint8_t *r, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kAxisGetter)
            {
                const auto a = getRegU32(ctx, 4);
                if (span(a, 0x38) && word32(r, a + 0xC) == 0x6B14A0u)
                {
                    const auto mask = word32(r, a + 0x10);
                    if (mask == 0x20000u) g_axisX = a;
                    if (mask == 0x40000u) g_axisY = a;
                }
            }
            g_axisGetter(r, ctx, runtime);
        }
        bool liveAxis(const uint8_t *r, uint32_t a, uint32_t mask, float &dz)
        {
            if (!span(a, 0x38) || word32(r, a + 0xC) != 0x6B14A0u || word32(r, a + 0x10) != mask) return false;
            dz = fl(r, a + 0x20);
            return std::isfinite(dz) && dz >= 0.0f && dz < 0.9f;
        }
        void responsePadRead(uint8_t *r, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kReadPad && getRegU32(ctx, 4) == 0u && getRegU32(ctx, 5) == 0u)
            {
                int mode = 0;
                float dx = 0.2f, dy = 0.2f;
                const auto actor = playerActor(r, g_controller);
                const bool fresh = std::chrono::steady_clock::now() - g_controllerSeen < std::chrono::milliseconds(500);
                if (actor && fresh && !*getConstMemPtr(r, g_controller + 4) &&
                    liveAxis(r, g_axisX, 0x20000u, dx) && liveAxis(r, g_axisY, 0x40000u, dy) && dx == dy)
                {
                    const auto index = word32(r, actor + 0xF8);
                    const auto state = index < 25 ? word32(r, actor + 0x80 + 4 * index) : 0;
                    const auto vt = span(state, 0x1C) ? word32(r, state + 0x18) : 0;
                    if (vt == 0x6BFA20u) mode = 1;
                    if (vt == 0x6BFE80u) mode = 2;
                    if (vt == 0x6BFED0u) mode = 3;
                }
                ps2_stick_response::sampleVehicle(mode, dx);
            }
            g_readPad(r, ctx, runtime);
        }
        void installStickResponse(PS2Runtime &runtime)
        {
            g_controllerGetter = runtime.lookupFunction(kControllerGetter);
            g_axisGetter = runtime.lookupFunction(kAxisGetter);
            g_readPad = runtime.lookupFunction(kReadPad);
            if (g_controllerGetter && g_axisGetter && g_readPad)
            {
                runtime.registerFunction(kControllerGetter, controllerGetter);
                runtime.registerFunction(kAxisGetter, axisGetter);
                runtime.registerFunction(kReadPad, responsePadRead);
            }
        }
    }

    void install(PS2Runtime &runtime)
    {
        installStickResponse(runtime);
        if (const char *v = std::getenv("TF_PLANET_GRAVITY_ANYWHERE"); !(v && *v == '1'))
            if (runtime.lookupFunction(kPlanetGravity))
                runtime.registerFunction(kPlanetGravity, planetGravity);
        // (TF_CAR_WHEELS=0: the port's placing alone, as before - for comparisons)
        if (const char *v = std::getenv("TF_CAR_WHEELS"); !(v && *v == '0'))
        {
            g_gamePlace = runtime.lookupFunction(kCarsPlace);
            if (g_gamePlace && (g_carUpdate = runtime.lookupFunction(kCarUpdate)) != nullptr)
                runtime.registerFunction(kCarUpdate, carUpdate);
        }
        if (const char *v = std::getenv("TF_SCREEN_LOG"); v && *v == '1')
        {
            if ((g_goToScreen = runtime.lookupFunction(kGoToScreen)) != nullptr)
                runtime.registerFunction(kGoToScreen, goToScreen);
            if ((g_goToScreenInGame = runtime.lookupFunction(kGoToScreenInGame)) != nullptr)
                runtime.registerFunction(kGoToScreenInGame, goToScreenInGame);
        }
        if (const char *v = std::getenv("TF_RUN_THRESHOLD"); v && *v)
            g_runThreshold = static_cast<float>(std::atof(v));
        if (runtime.lookupFunction(kPickDesiredSpeed))
            runtime.registerFunction(kPickDesiredSpeed, pickDesiredSpeed);
        else
            std::fprintf(stderr, "[movement] nothing at %08x to replace\n", kPickDesiredSpeed);
    }
}
