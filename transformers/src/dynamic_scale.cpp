// (TODO 295) The dynamic render scale. See dynamic_scale.h.
//
// The UHD 620's cost is a cliff, not a slope (lane A's ThinkPad profile, Deep Amazon at cap 60: 67% = 18.8-22 ms a
// frame, 50% = 3.3 ms - ~5.7x for 1.8x the pixels), so the steps are lopsided:
//   - down fast: two judged seconds in a row over budget -> straight to the first level that fitted the last time it was
//     drawn (its p90 under 85% of the budget then), else the floor - not one level a step;
//   - up slowly: the current level's p90 under 70% of the budget for 10 s in a row, then one level up - and never back to a
//     level that failed (left downwards) within the last 60 s, so it cannot swing across the cliff.

#include "dynamic_scale.h"

#include "nr_frame.h"
#include "nr_install.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace dynamic_scale
{
    namespace
    {
        using Clock = std::chrono::steady_clock;
        constexpr int kLevels[] = {67, 63, 58, 54, 50};
        constexpr int kLevelCount = static_cast<int>(sizeof(kLevels) / sizeof(kLevels[0]));
        constexpr double kOverShare = 1.00;  // (over the budget: a judged second that missed)
        constexpr double kFitShare = 0.85;   // (a level that fitted: its p90 under this share)
        constexpr double kUpShare = 0.70;    // (the way up: under this share at the current level)
        constexpr int kDownSeconds = 2, kUpSeconds = 10;
        constexpr auto kFailMemory = std::chrono::seconds(60);

        int g_level = -1; // (kLevels' index; -1 before the first tick)
        int g_overSeconds = 0, g_upSeconds = 0;
        unsigned g_frames = 0;
        double g_levelP90[kLevelCount] = {}; // (each level's last judged p90 while drawn; 0: not drawn yet)
        Clock::time_point g_failedAt[kLevelCount] = {};
        Clock::time_point g_lastJudge{};

        void change(int next, double gpu, double budget, const char *why)
        {
            std::fprintf(stderr, "[dynscale] render scale %d%% -> %d%% (%s: the GPU %.2f ms a frame at the 90th percentile, budget %.2f ms)\n",
                         kLevels[g_level], kLevels[next], why, gpu, budget);
            g_level = next;
            g_overSeconds = g_upSeconds = 0;
            nr::frame::setRenderScaleAuto(kLevels[g_level], g_level > 0); // (below the start: the 480-line floor waived)
        }
    }

    bool active() { return nr::frame::scaleAuto(); }

    bool canGiveWay() { return nr::frame::scaleAuto() && g_level >= 0 && g_level + 1 < kLevelCount; }

    void tick(int cap)
    {
        if (!nr::frame::scaleAuto())
            return;
        ++g_frames;
        const auto now = Clock::now();
        if (g_level < 0)
        {
            // (The level nearest the scale it starts at: 67 from the low-end default)
            const int start = nr::frame::renderScale();
            g_level = 0;
            for (int i = 0; i < kLevelCount; ++i)
                if (std::abs(kLevels[i] - start) < std::abs(kLevels[g_level] - start))
                    g_level = i;
            g_lastJudge = now;
            g_frames = 0;
            return;
        }
        if (now - g_lastJudge < std::chrono::seconds(1))
            return;
        const unsigned frames = std::clamp(g_frames, 8u, 1024u);
        g_frames = 0;
        g_lastJudge = now;
        const double gpu = nr::gpuFrameMs(0.90, frames);
        if (gpu <= 0.0)
            return; // (no timestamps yet: a menu, a load)
        const int target = cap <= 0 ? 60 : std::min(cap, 60);
        const double budget = 1000.0 / target;
        g_levelP90[g_level] = gpu;
        if (gpu > kOverShare * budget)
        {
            g_upSeconds = 0;
            if (++g_overSeconds < kDownSeconds || g_level + 1 >= kLevelCount)
                return;
            g_failedAt[g_level] = now;
            int next = kLevelCount - 1; // (the floor, unless a level between fitted when it was drawn)
            for (int i = g_level + 1; i < kLevelCount; ++i)
                if (g_levelP90[i] > 0.0 && g_levelP90[i] < kFitShare * budget)
                {
                    next = i;
                    break;
                }
            change(next, gpu, budget, "over budget");
            return;
        }
        g_overSeconds = 0;
        if (gpu >= kUpShare * budget || g_level == 0)
        {
            g_upSeconds = 0;
            return;
        }
        if (++g_upSeconds < kUpSeconds)
            return;
        const int up = g_level - 1;
        if (g_failedAt[up] != Clock::time_point{} && now - g_failedAt[up] < kFailMemory)
            return; // (it failed there less than a minute ago: the cliff)
        change(up, gpu, budget, "room");
    }
}
