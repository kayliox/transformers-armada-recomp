// The sound nodes at the PC frame rates.
//
// The sound list's update (0x27BD50, once a game frame, called from 0x2807D4) runs two pools of
// sound nodes through the same small state machine, counted in FRAMES:
//   - 35 world-sound nodes (taSoundNode, array at [gp-0x3E94], 0x40 apart, state at +0x28) and
//   - 10 more (array at [gp-0x3E90], 0x20 apart, state at +0x04).
// State 1 -> 2 -> 3 -> 4 waits three frames for the IOP to start the voice. In state 4 a voice
// the device reports finished goes to 5; from 5 the node counts one a frame and is stopped and
// freed at 25 (`slti 0x19` at 0x27BEE4, 0x27C0B4 and 0x27C128), i.e. 20 frames after the sound
// ended; a voice playing again sends it back to 4. Sounds the game plays again when their node is
// free (loops made of one-shots) start again that much later.
//
// At the console's 30 frames a second that is 0.1 s and 0.67 s; at R frames a second the times
// shrink by 30/R (at 60 the nodes were measured freed ~0.37 s sooner than at 30, so the repeating
// sounds came round 1.6 times as often).
//
// Here the counts go at 30 a second at any level rate. On the frames between two console frames
// the update still runs in full (it starts, checks and stops what it should), but before it each
// count it would move on is set one back, so its +1 leaves it where it was: 2..3 and 6..24 one
// back, 5 to 4 (4 is "playing, check": a finished voice goes to 5 again, a playing one stays at 4,
// as from 5). State 1 cannot be held (0 is a free node), so the start delay is one frame plus two
// held ones: 0.083 s at 60, 0.075 s at 120, close to the console's 0.1 s. The adjustment is made
// BEFORE the call only: the recompiled update often suspends part way (it waits on the IOP) and
// returns to the caller before it is done, so its results cannot be looked at afterwards.
//
// Decomp equivalent: in 0x27BD50 advance the two counts by a time accumulator (or compare with
// 5 + 20*R/30 and step the start delay every R/30 frames).
//
// TF_SOUND_NODES=0 leaves the console's behaviour (overrides.cpp); TF_SOUND_NODES_TRACE=1 logs.

#include "pool_growth.h"
#include "sound_nodes.h"

#include "port.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "game_version.h" // (TF_GA: the code addresses that differ in the PAL release)

namespace sound_nodes
{
    namespace
    {
        constexpr uint32_t kUpdate = 0x0027BD50u;
        struct Pool
        {
            int32_t gpOffset;
            uint32_t count, stride, state;
        };
        constexpr Pool kPools[] = {{-0x3E94, pool_growth::kSoundEmitters, 0x40u, 0x28u}, {-0x3E90, 10u, 0x20u, 0x04u}};

        PS2Runtime::RecompiledFunction g_update = nullptr;
        double g_owed = 0.0; // console frames not counted yet
        uint64_t g_calls = 0u, g_held = 0u;

        uint32_t loadWord(const uint8_t *rdram, uint32_t address)
        {
            uint32_t value = 0u;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&value, p, sizeof(value));
            return value;
        }

        void storeWord(uint8_t *rdram, uint32_t address, uint32_t value)
        {
            if (uint8_t *p = getMemPtr(rdram, address))
                std::memcpy(p, &value, sizeof(value));
        }

        // Puts back the counts the update is about to move on (see above).
        void holdCounts(uint8_t *rdram, uint32_t gp)
        {
            for (const Pool &pool : kPools)
            {
                const uint32_t base = loadWord(rdram, gp + static_cast<uint32_t>(pool.gpOffset));
                if (!base)
                    continue;
                for (uint32_t i = 0; i < pool.count; ++i)
                {
                    const uint32_t at = base + i * pool.stride + pool.state;
                    const uint32_t state = loadWord(rdram, at);
                    if (state == 2u || state == 3u || (state >= 5u && state <= 24u))
                    {
                        storeWord(rdram, at, state - 1u);
                        ++g_held;
                    }
                }
            }
        }

        void update(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            static const bool trace = std::getenv("TF_SOUND_NODES_TRACE") != nullptr;
            const int rate = tf::port::levelRate();
            if (trace && (g_calls % 300u) == 0u)
                std::fprintf(stderr, "[soundnodes] call %llu rate %d counts held %llu\n",
                             static_cast<unsigned long long>(g_calls), rate, static_cast<unsigned long long>(g_held));
            ++g_calls;
            if (rate <= 30)
                g_owed = 0.0; // (the console's rate: every frame counts)
            else
            {
                g_owed += 30.0 / static_cast<double>(rate);
                if (g_owed >= 1.0)
                    g_owed -= 1.0; // a console frame: everything counts
                else
                    holdCounts(rdram, getRegU32(ctx, 28));
            }
            g_update(rdram, ctx, runtime);
        }
    }

    // The wind generator (taWindGenerator's update, 0x381CB0, once a frame per generator) slides its
    // wind sound's pitch and counts its gusts in frames:
    //   +0x38 pitch += +0x34 every call (0x381DE4), +0x34 re-picked at each gust and turned round at
    //         the bounds +0x54 / +0x58;
    //   +0x40 frames since the gust, a new gust when it passes +0x3C (a random count from
    //         [+0x4C, +0x50), 0x381FD0..0x382028), +1 at the end (0x382064);
    //   +0x78 frames to wait (down one a call, 0x381D70) before playing the sound again while it has
    //         no voice (+0x6C == 0).
    // At R frames a second the pitch swept R/30 times as fast and the gusts came R/30 times as
    // often. Here, before each call: the pitch step is scaled to 30/R (+0x38 -= +0x34 * (1 - 30/R):
    // the add comes before the gust's new +0x34 in the same call, so it is exact), and on the
    // frames between two console frames +0x40 is set one back and +0x78 one up so the call leaves
    // them where they were. (Its volume fade, 0x381A80, is already one of the port's rate sites.)
    // Decomp: in 0x381CB0 multiply the pitch step by dt*30 and count the two in time.
    namespace
    {
        constexpr uint32_t kWindUpdate = TF_GA(0x00381CB0u);
        PS2Runtime::RecompiledFunction g_windUpdate = nullptr;
        struct WindOwed
        {
            uint32_t generator;
            double owed;
        };
        WindOwed g_wind[8] = {}; // (one slot per generator: a level has very few)

        double &windOwed(uint32_t generator)
        {
            for (WindOwed &w : g_wind)
                if (w.generator == generator)
                    return w.owed;
            for (WindOwed &w : g_wind)
                if (w.generator == 0u)
                {
                    w = {generator, 0.0};
                    return w.owed;
                }
            g_wind[0] = {generator, 0.0};
            return g_wind[0].owed;
        }

        float loadFloat(const uint8_t *rdram, uint32_t address)
        {
            const uint32_t bits = loadWord(rdram, address);
            float value;
            std::memcpy(&value, &bits, sizeof(value));
            return value;
        }

        void storeFloat(uint8_t *rdram, uint32_t address, float value)
        {
            uint32_t bits;
            std::memcpy(&bits, &value, sizeof(bits));
            storeWord(rdram, address, bits);
        }

        void windUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const int rate = tf::port::levelRate();
            const uint32_t wind = getRegU32(ctx, 4);
            if (rate > 30 && wind != 0u)
            {
                const double share = 30.0 / static_cast<double>(rate);
                storeFloat(rdram, wind + 0x38u,
                           loadFloat(rdram, wind + 0x38u) - loadFloat(rdram, wind + 0x34u) * static_cast<float>(1.0 - share));
                double &owed = windOwed(wind);
                owed += share;
                if (owed >= 1.0)
                    owed -= 1.0; // a console frame: the counts move on
                else
                {
                    storeWord(rdram, wind + 0x40u, loadWord(rdram, wind + 0x40u) - 1u);
                    if (loadWord(rdram, wind + 0x6Cu) == 0u)
                        storeWord(rdram, wind + 0x78u, loadWord(rdram, wind + 0x78u) + 1u);
                }
                static const bool trace = std::getenv("TF_SOUND_NODES_TRACE") != nullptr;
                static uint64_t calls = 0u;
                if (trace && (calls++ % 600u) == 0u)
                    std::fprintf(stderr, "[wind] %08x pitch %.4f step %.5f gust %d/%d\n", wind, loadFloat(rdram, wind + 0x38u),
                                 loadFloat(rdram, wind + 0x34u), static_cast<int>(loadWord(rdram, wind + 0x40u)),
                                 static_cast<int>(loadWord(rdram, wind + 0x3Cu)));
            }
            g_windUpdate(rdram, ctx, runtime);
        }
    }

    void stateLoaded()
    {
        g_owed = 0.0;
        for (WindOwed &w : g_wind)
            w = {};
    }

    void install(PS2Runtime &runtime)
    {
        if ((g_update = runtime.lookupFunction(kUpdate)) != nullptr)
            runtime.registerFunction(kUpdate, update);
        if ((g_windUpdate = runtime.lookupFunction(kWindUpdate)) != nullptr)
            runtime.registerFunction(kWindUpdate, windUpdate);
    }
}
