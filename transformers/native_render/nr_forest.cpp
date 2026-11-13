// The forest's 3D trees drawn natively (TODO 287, lane F). See nr_forest.h.
//
// Phase 0: TF_NATIVE_FOREST=measure. Two wrappers, each calling the function registered before it (the recompiled
// one or another wrapper - TF_FOREST_SPLIT's timers, ecoTimed):
//   - 0x1FFC70 (vlMeshSurface::vfunc_03, a mesh surface's render: RenderGeometrySet and the capture): a call whose
//     return address lies in the forest's code (0x428000-0x434000) is a tree's submit - counted, timed and kept by its
//     caller and by the surface (a0) it draws, the key a native clone's template would have;
//   - 0x429510 (ecoPromotionList::Render, the 3D trees' drawing): its fresh calls counted (the forest runs).
// The caller is read from ra, not from a depth kept by wrapping the drawing: a checkpoint often unwinds the drawing
// mid-run and the scheduler resumes it at its label - straight into the recompiled function, past any wrapper - so a
// depth would miss the submits after the first unwind (the first measure, 2026-11-13: ~0 submits seen). A mesh
// surface's render that is itself unwound and resumed counts only its time up to the unwind. All on the game thread.

#include "nr_forest.h"

#include "game_version.h" // (TF_GA: the code addresses that differ in the PAL release)
#include "ps2_runtime.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace nr::forest
{
    namespace
    {
        constexpr uint32_t kMeshSurfaceRender = 0x001FFC70u; // (the same in both releases)
        uint32_t g_instanceDraw = 0u;                        // (TF_GA'd at install)
        uint32_t g_codeLo = 0u, g_codeHi = 0u;               // (the forest's code, TF_GA'd at install)

        int g_mode = 0; // 0 off, 1 measure
        PS2Runtime::RecompiledFunction g_origDraw = nullptr, g_origSurface = nullptr;

        // The window's counts.
        using Clock = std::chrono::steady_clock;
        Clock::time_point g_windowStart{};
        std::chrono::seconds g_window{30}; // (TF_NATIVE_FOREST_SECONDS: the report's window)
        uint64_t g_runs = 0u;              // (0x429510's fresh calls)
        uint64_t g_submits = 0u;           // (0x1FFC70 calls from the forest's code)
        uint64_t g_repeats = 0u;           // (of them: the surface met before in the window)
        double g_submitUs = 0.0;
        struct Caller
        {
            uint64_t calls = 0u;
            double us = 0.0;
        };
        std::unordered_map<uint32_t, Caller> g_callers; // (by return address)
        std::unordered_set<uint32_t> g_surfaces;

        uint32_t reg32(const R5900Context *ctx, int n)
        {
            uint32_t v;
            std::memcpy(&v, &ctx->r[n], sizeof(v));
            return v;
        }

        void report()
        {
            const double secs = std::chrono::duration<double>(Clock::now() - g_windowStart).count();
            if (secs <= 0.0)
                return;
            const double runs = static_cast<double>(g_runs ? g_runs : 1u);
            std::fprintf(stderr,
                         "[forest:native] %.0f s: %llu forest runs (%.1f/s); the trees' submits %.3f ms a run (%.1f ms a second), "
                         "%.1f a run; %zu distinct surfaces; %.1f%% of submits met before in the window (the clonable ceiling)\n",
                         secs, static_cast<unsigned long long>(g_runs), g_runs / secs, g_submitUs / runs / 1000.0, g_submitUs / secs / 1000.0,
                         static_cast<double>(g_submits) / runs, g_surfaces.size(),
                         g_submits ? 100.0 * static_cast<double>(g_repeats) / static_cast<double>(g_submits) : 0.0);
            std::vector<std::pair<uint32_t, Caller>> callers(g_callers.begin(), g_callers.end());
            std::sort(callers.begin(), callers.end(), [](const auto &a, const auto &b) { return a.second.us > b.second.us; });
            for (size_t i = 0; i < callers.size() && i < 6u; ++i)
                std::fprintf(stderr, "[forest:native]   from %08x: %.1f calls a run, %.3f ms a run\n", callers[i].first,
                             static_cast<double>(callers[i].second.calls) / runs, callers[i].second.us / runs / 1000.0);
            g_runs = g_submits = g_repeats = 0u;
            g_submitUs = 0.0;
            g_callers.clear();
            g_surfaces.clear();
            g_windowStart = Clock::now();
        }

        void instanceDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == g_instanceDraw)
            {
                ++g_runs;
                if (Clock::now() - g_windowStart >= g_window)
                    report();
            }
            g_origDraw(rdram, ctx, runtime);
        }

        void surfaceRender(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = reg32(ctx, 31);
            if (ctx->pc != kMeshSurfaceRender || ra < g_codeLo || ra >= g_codeHi)
            {
                g_origSurface(rdram, ctx, runtime);
                return;
            }
            ++g_submits;
            if (!g_surfaces.insert(reg32(ctx, 4)).second)
                ++g_repeats;
            const auto start = Clock::now();
            g_origSurface(rdram, ctx, runtime);
            const double us = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
            g_submitUs += us;
            Caller &c = g_callers[ra];
            ++c.calls;
            c.us += us;
        }
    }

    bool enabled() { return g_mode != 0; }

    void install(PS2Runtime &runtime)
    {
        const char *v = std::getenv("TF_NATIVE_FOREST");
        if (!v || !*v || std::strcmp(v, "0") == 0)
            return;
        if (std::strcmp(v, "measure") != 0)
        {
            std::fprintf(stderr, "[forest:native] TF_NATIVE_FOREST=%s: only 'measure' exists yet (phase 0) - off\n", v);
            return;
        }
        g_instanceDraw = TF_GA(0x00429510u);
        g_codeLo = TF_GA(0x00428000u);
        g_codeHi = TF_GA(0x00434000u);
        g_origDraw = runtime.lookupFunction(g_instanceDraw);
        g_origSurface = runtime.lookupFunction(kMeshSurfaceRender);
        if (!g_origDraw || !g_origSurface)
        {
            std::fprintf(stderr, "[forest:native] a forest function is missing - off\n");
            return;
        }
        runtime.registerFunction(g_instanceDraw, instanceDraw);
        runtime.registerFunction(kMeshSurfaceRender, surfaceRender);
        if (const char *w = std::getenv("TF_NATIVE_FOREST_SECONDS"); w && std::atoi(w) > 0)
            g_window = std::chrono::seconds(std::atoi(w));
        g_mode = 1;
        g_windowStart = Clock::now();
        std::fprintf(stderr, "[forest:native] measure: the forest's submits timed (every %lld s, the originals still drawn)\n",
                     static_cast<long long>(g_window.count()));
    }
}
