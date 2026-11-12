// Frame timing: the EE's clock and a level's clock.

#include "timing.h"

#include "runtime/ee_scheduler.h"

#include <cstdio>
#include <cstdlib>
#include "game_version.h" // (TF_GA: the code addresses that differ in the PAL release)

namespace tf::timing
{
    // 0x108780: the time in seconds by the EE's own clock, COP0 Count (which the scheduler
    // advances with the cycles the game's code is charged), summed into 64 bits.
    f32 ClockSeconds()
    {
        const uint32_t count = decomp::guest().cop0Count();
        const uint64_t cycles = clockCycles + static_cast<uint64_t>(count - clockLastCount);
        clockLastCount = count;
        clockCycles = cycles;
        // (TF_CLOCK_LOG=<vblank>, a test aid - TODO 150: each reading from that VBlank on, its caller and the count; two runs
        // that should play the same part where the EE's clock does, long before their random seeds do)
        static const unsigned long long clockLog = std::getenv("TF_CLOCK_LOG") ? std::strtoull(std::getenv("TF_CLOCK_LOG"), nullptr, 10) : 0ull;
        if (clockLog && decomp::guest().runtime()->eeScheduler().currentVSyncTick() >= clockLog)
        {
            static int budget = 40000;
            if (budget-- > 0)
                std::fprintf(stderr, "[tf:clock] %llu ra %08x count %08x cycles %llu\n",
                             static_cast<unsigned long long>(decomp::guest().runtime()->eeScheduler().currentVSyncTick()),
                             decomp::guest().gpr(31), count, static_cast<unsigned long long>(cycles));
        }
        // As a float: the low half halved (so that it converts as a positive integer) and
        // doubled again, plus the high half times 2^32.
        const f32 low = f32(2.0f) * f32::fromInt(static_cast<int32_t>(static_cast<uint32_t>(cycles) >> 1));
        const f32 high = f32::fromInt(static_cast<int32_t>(cycles >> 32)) * (f32(65536.0f) * f32(65536.0f));
        return secondsPerCycle * (low + high);
    }

    // 0x41E8F0: a level's clock, at 0 - and the current one from now on.
    GameClock GameClock_Construct(GameClock self)
    {
        currentGameClock = self.address;
        self.time() = 0.0f;
        self.step() = 0.0f;
        self.scale() = 1.0f;
        self.frames() = 0;
        self.framesSinceStep() = 0;
        self.timeAtStep() = 0.0f;
        return self;
    }

    // 0x41E820: the seconds a frame from now on.
    void GameClock_SetStep(GameClock self, f32 step)
    {
        self.timeAtStep() = self.time();
        self.step() = step;
        self.framesSinceStep() = 0;
    }

    // 0x41E840: once a frame. The time is worked out from the frames since the step was set
    // (no error builds up), and lags a frame: the first advance after setting the step
    // leaves it where it was.
    void GameClock_Advance(GameClock self)
    {
        self.time() = self.timeAtStep() + f32::fromInt(self.framesSinceStep()) * (self.step() * self.scale());
        self.frames() = self.frames() + 1;
        self.framesSinceStep() = self.framesSinceStep() + 1;
    }

    DECOMP_FUNCTION(0x108780, ClockSeconds, "timing");
    DECOMP_FUNCTION(TF_GA(0x41E8F0), GameClock_Construct, "timing");
    DECOMP_FUNCTION(TF_GA(0x41E820), GameClock_SetStep, "timing");
    DECOMP_FUNCTION(TF_GA(0x41E840), GameClock_Advance, "timing");
}
