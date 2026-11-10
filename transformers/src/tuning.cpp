// Tuning (tuning.h): the game's speed.
//
// Every system of a level steps by the level's frame period (taGame +0x5C, set when the level
// begins: 1/R at R frames a second). The game clock's own scale (+0x0C) changes nothing a player sees
// (measured: the same distance walked at scale 0.25 and 1). So game speed changes what a level begun
// at another rate would have (the port's convertLevel, overrides.cpp): the frame period, the clock's
// step (GameClock_SetStep, 0x41E820: step +0x08, frames since +0x10, the time at the step +0x14, the
// time going on from the next frame's) and the physics' gravity a step (0x6EC698 = the gravity a
// second at 0x6CBA1C times the period). The pictures keep their rate; the game moves on faster or
// slower in them. 0 freezes the game (photo mode).
//
// Decomp equivalent: the frame period times the speed, where the level sets it, and its two
// followers (clock step, gravity step) with it.

#include "tuning.h"

#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_settings.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace tuning
{
    namespace
    {
        constexpr uint32_t kGame = 0x006CF2B8u, kGamePeriod = 0x5Cu;
        constexpr uint32_t kGameClock = 0x006CF8A4u;
        constexpr uint32_t kGravityStep = 0x006EC698u, kGravityPerSecond = 0x006CBA1Cu;
        constexpr uint32_t kCreatureGravity = 0x006CF9B4u;
        std::atomic<float> g_speed{1.0f};
        std::atomic<float> g_gravity{1.0f}; // the gravity's factor (the gravity step only)
        float g_writtenGravity = 1.0f;
        float g_base = 0.0f;    // the level's own period
        float g_written = 0.0f; // the period last written (0: none)
        uint32_t g_game = 0u;
        constexpr float kFrozen = 1.0e-4f; // the speed that stands for "stopped" (photo mode's 0)
        std::atomic<bool> g_frozenState{false};

        float loadFloat(const uint8_t *rdram, uint32_t a)
        {
            float v = 0.0f;
            if (const uint8_t *p = getConstMemPtr(rdram, a))
                std::memcpy(&v, p, sizeof(v));
            return v;
        }

        void storeFloat(uint8_t *rdram, uint32_t a, float v)
        {
            if (uint8_t *p = getMemPtr(rdram, a))
                std::memcpy(p, &v, sizeof(v));
        }
    }

    float speed() { return g_speed.load(); }
    float gravity() { return g_gravity.load(); }
    void setSpeed(float s) { g_speed.store(std::clamp(s, 0.0f, 4.0f)); }
    void setGravity(float g) { g_gravity.store(std::clamp(g, 0.1f, 4.0f)); }
    bool takeFrozenState() { return g_frozenState.exchange(false); }

    // TODO 269: a state saved while the game was stopped (F6 in photo mode, or the restart that applies a
    // setting) holds the stopped period: the level's own times kFrozen. The level's own rates give periods
    // of 1/480 to 1/25 s and the Game speed setting goes down to a tenth at most, so a period of 1e-5 or
    // less can only be that. Left as it is, the port reads the state as made at no PC rate (the level then
    // runs at the console's 30) and takes the stopped period as the level's own: stopped for good. Here,
    // right after the load and before anything reads it, the period, the clock's step and the gravity step
    // are put back as the level had them. TF_TUNING_FROZENFIX=0: not done (tests).
    bool thawLoadedState(uint8_t *rdram)
    {
        static const bool repair = !(std::getenv("TF_TUNING_FROZENFIX") && *std::getenv("TF_TUNING_FROZENFIX") == '0');
        uint32_t game = 0u, clock = 0u;
        std::memcpy(&game, rdram + kGame, sizeof(game));
        std::memcpy(&clock, rdram + kGameClock, sizeof(clock));
        if (!repair || !game || !clock)
            return false;
        const float period = loadFloat(rdram, game + kGamePeriod);
        const float own = period / kFrozen;
        if (!(period > 0.0f && period <= 1.0e-5f && own > 0.0005f && own < 0.1f))
            return false;
        storeFloat(rdram, game + kGamePeriod, own);
        const float time = loadFloat(rdram, clock + 0x04u), step = loadFloat(rdram, clock + 0x08u);
        const float scale = loadFloat(rdram, clock + 0x0Cu);
        storeFloat(rdram, clock + 0x08u, own);
        if (uint8_t *p = getMemPtr(rdram, clock + 0x10u))
            std::memset(p, 0, 4);
        storeFloat(rdram, clock + 0x14u, time + step * scale);
        storeFloat(rdram, kGravityStep, loadFloat(rdram, kGravityPerSecond) * own);
        g_frozenState.store(true);
        std::fprintf(stderr, "[tuning] the state was saved with the game stopped (period %.7f): the level's own %.6f put back\n", period, own);
        return true;
    }

    void frame(uint8_t *rdram, bool inLevel)
    {
        uint32_t game = 0u, clock = 0u;
        std::memcpy(&game, rdram + kGame, sizeof(game));
        std::memcpy(&clock, rdram + kGameClock, sizeof(clock));
        if (!inLevel || !game || !clock)
        {
            g_game = 0u;
            return;
        }
        const float period = loadFloat(rdram, game + kGamePeriod);
        if (game != g_game || (period != g_written && period > 0.0f))
        {
            // (a new level, or the port set a new rate: that is the level's own period now)
            g_game = game;
            g_base = period;
            g_written = 0.0f;
            g_writtenGravity = 1.0f; // (the level set its own gravity step)
            static float logged = 0.0f;
            if (g_base != logged)
                std::fprintf(stderr, "[tuning] level period %.6f\n", logged = g_base);
        }
        if (!(g_base > 0.0005f && g_base < 0.1f))
            return;
        const float want = g_base * std::max(g_speed.load(std::memory_order_relaxed), 1.0e-4f);
        const float current = g_written != 0.0f ? g_written : g_base;
        const float gravity = g_gravity.load(std::memory_order_relaxed);
        {
            // The creatures' own gravity (0x6CF9B4: a jump's and a fall's, 0x470F70), the game's value
            // taken again whenever it is not the one written here.
            static float base = 0.0f, written = 0.0f;
            const float now = loadFloat(rdram, kCreatureGravity);
            if (now != written)
                base = now;
            if (base != 0.0f && now != base * gravity)
                storeFloat(rdram, kCreatureGravity, base * gravity);
            written = base * gravity;
        }
        if (want == current && gravity == g_writtenGravity)
            return;
        // (the gravity a step: the game's gravity a second x the period, and our factor)
        storeFloat(rdram, kGravityStep, loadFloat(rdram, kGravityPerSecond) * want * gravity);
        g_writtenGravity = gravity;
        if (want == current)
            return;
        storeFloat(rdram, game + kGamePeriod, want);
        // The clock: SetStep (the step, from the next frame on, its time going on from now)
        const float time = loadFloat(rdram, clock + 0x04u), step = loadFloat(rdram, clock + 0x08u);
        const float scale = loadFloat(rdram, clock + 0x0Cu);
        storeFloat(rdram, clock + 0x08u, want);
        if (uint8_t *p = getMemPtr(rdram, clock + 0x10u))
            std::memset(p, 0, 4);
        storeFloat(rdram, clock + 0x14u, time + step * scale);
        g_written = want;
    }
}
