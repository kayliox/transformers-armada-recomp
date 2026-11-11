// Frame timing: the rates the game aims at.

#include "timing.h"

#include "game_version.h" // (TF_GA: the code addresses that differ in the PAL release)
#include "port.h"

namespace tf::timing
{
    namespace
    {
        // The standard the rates are for: a PAL console in its 60 Hz mode counts as NTSC.
        // (Inline in each of the three functions below in the original.)
        // PC: NTSC in every mode. The VBlanks are the PC's, 60 a second (the runtime's), and the
        // PAL release in its 50 Hz mode timed for 50 ran 20% fast: its game clock 11.8 s in 10.1 s.
        // So the game is timed as in its own 60 Hz mode, its frame rate unchanged; the 50 Hz
        // mode keeps its picture (TvStandard below).
        int32_t RateStandard()
        {
            int32_t standard = tvStandard;
            if (pal60 != 0 && standard == 1)
                standard = 0;
            return standard == 1 ? 0 : standard;
        }

        // PC: a rate of the console's (59.94) at the PC port's level rate R: 59.94 x R / 60, a
        // frame and a VBlank every 1/R s. As it is at 60, and outside such a level (rate 0).
        f32 AtRate(f32 consoleRate, int rate)
        {
            if (rate == 0 || rate == 60)
                return consoleRate;
            return consoleRate * f32(static_cast<float>(rate) / 60.0f);
        }

        // The return address of the period's call in a car's reset (0x31F6D0).
        constexpr uint32_t kCarResetReturn = 0x0031F7E0u;
    }

    f32 LevelFrameRate(int rate)
    {
        return AtRate(frameRates[RateStandard()], rate);
    }

    f32 LevelFramePeriod(int rate)
    {
        return f32(1.0f) / LevelFrameRate(rate);
    }

    f32 ConsoleFramePeriod()
    {
        f32 rate = frameRates[RateStandard()];
        rate = rate / 2.0f;
        return f32(1.0f) / rate;
    }

    // 0x32E840: the TV's field rate.
    f32 TvFieldRate()
    {
        // PC: in a level at the PC port's rate, a field (a VBlank) every 1/R s - every 1/(2R) s
        // with in-between pictures.
        return AtRate(tvFieldRates[RateStandard()], port::vblankRate());
    }

    // 0x32E8B0: the frame rate the game aims at - the TV's field rate, or half of it.
    f32 TargetFrameRate()
    {
        f32 rate = frameRates[RateStandard()];
        if (halfRate != 0)
            rate = rate / 2.0f;
        // PC: in a level at the PC port's rate, R frames a second - of one field, or of two with
        // in-between pictures (whose half rate is on).
        if (port::levelRate() != 0)
            return LevelFrameRate(port::levelRate());
        return rate;
    }

    // 0x32E910: the seconds a frame at that rate.
    f32 FramePeriod()
    {
        // PC: a car's reset keeps the period of 30 in a level at the PC port's rate. Its wheels
        // and tyres take the step from it (+0x34), and the cars step every 30th of a second
        // at every rate (overrides.cpp, "The cars").
        if (port::levelRate() != 0 && decomp::guest().gpr(31) == kCarResetReturn)
            return ConsoleFramePeriod();
        f32 rate = frameRates[RateStandard()];
        if (halfRate != 0)
            rate = rate / 2.0f;
        if (port::levelRate() != 0)
            return LevelFramePeriod(port::levelRate()); // (PC: as TargetFrameRate)
        return f32(1.0f) / rate;
    }

    // 0x32E8A0: frames of two TV fields or of one. The game asks for two when it makes its
    // modes: the shipped game runs at 30.
    void SetHalfRate(uint8_t on)
    {
        // PC: remembered for when the level ends; a level at the PC port's rate keeps one
        // field a frame, or two with in-between pictures.
        port::noteGameHalfRate(on);
        halfRate = port::levelRate() != 0 ? uint8_t(port::inBetweenPictures() ? 1 : 0) : on;
    }

    // 0x202FF0: the governor's target rates; the main loop sets [0] when it starts.
    void SetTargetRate(int32_t index, f32 rate)
    {
        targetRates[index] = rate;
    }

    // 0x106710: whether the game holds itself at 30 (from boot until a level has been set
    // up, and while one loads).
    int32_t HoldsAtThirty()
    {
        return holdAtThirty;
    }

    // 0x202FE0 (j 0x106710): the same, asked by the governor.
    int32_t GovernorHoldsAtThirty()
    {
        // PC: a level at the PC port's rate is not held at 30.
        return port::levelRate() != 0 ? 0 : static_cast<int32_t>(holdAtThirty);
    }

    // 0x32E800: the TV standard - 0 NTSC, 1 PAL (a PAL console in its 60 Hz mode: 0).
    int32_t TvStandard()
    {
        // PC: the places that time the game by it see the 60 Hz mode (RateStandard): a PAL
        // console's speed-up of a frame's movement (0x3C9E80, x1.2 at 50 Hz), the in-level movies'
        // standard (0x3199E0, kept with the rate it reads next) and the films, "p" at 25
        // pictures a second or "n" at 29.97 with the field rate (0x4CDAE0) - the n films, timed
        // in the runtime's NTSC fields (MPEG.cpp) as in the 60 Hz mode. Every other caller sets
        // the picture up (the display's offsets and height, the menus' text) and sees the mode.
        // (Return addresses: the USA release's TV standard is always 0.)
        const uint32_t caller = decomp::guest().gpr(31);
        if (caller == TF_GA(0x3C9F94u) || caller == 0x319AACu || caller == TF_GA(0x4CDB50u))
            return RateStandard();
        int32_t standard = tvStandard;
        if (pal60 != 0 && standard == 1)
            standard = 0;
        return standard;
    }

    DECOMP_FUNCTION(0x32E800, TvStandard, "timing");
    DECOMP_FUNCTION(0x32E840, TvFieldRate, "timing");
    DECOMP_FUNCTION(0x32E8B0, TargetFrameRate, "timing");
    DECOMP_FUNCTION(0x32E910, FramePeriod, "timing");
    DECOMP_FUNCTION(0x32E8A0, SetHalfRate, "timing");
    DECOMP_FUNCTION(0x202FF0, SetTargetRate, "timing");
    DECOMP_FUNCTION(0x106710, HoldsAtThirty, "timing");
    DECOMP_FUNCTION(0x202FE0, GovernorHoldsAtThirty, "timing");
}
