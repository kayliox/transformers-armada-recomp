// nr_policy.cpp - the video-memory policy (nr_policy.h, TODO 94): the quality preset and the large images' cap and budget
// from the backend's video memory figure, one set of rules for Direct3D 12 and Vulkan.
#include "nr_policy.h"
#include "nr_backend.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>

namespace nr::policy
{
    namespace
    {
        // The video memory, MB: TF_NATIVE_VRAM_MB (the low-end policy tried on a larger card), else the backend's figure
        // (0: none known); nothing: not known yet (the Vulkan device's GPU not chosen).
        std::optional<uint64_t> memoryMB()
        {
            static const std::optional<uint64_t> pretended = []() -> std::optional<uint64_t> {
                if (const char *e = std::getenv("TF_NATIVE_VRAM_MB"); e && *e)
                    return std::strtoull(e, nullptr, 10);
                return std::nullopt;
            }();
            return pretended ? pretended : backend().adapterMemoryMB();
        }

        // A value fixed once: the first one given once its inputs are known, its log line written under the same lock
        // (as a function-local static's first use would; Direct3D 12's values were those until this file).
        template <class T>
        struct Kept
        {
            std::atomic<bool> fixed{false};
            T value{};
            std::mutex mutex;
        };

        template <class T, class Log>
        T keep(Kept<T> &k, T value, bool known, Log log)
        {
            if (!known)
                return value; // (not kept: asked again later)
            std::lock_guard<std::mutex> lock(k.mutex);
            if (!k.fixed.load(std::memory_order_relaxed))
            {
                k.value = value;
                log(value);
                k.fixed.store(true, std::memory_order_release);
            }
            return k.value;
        }
    }

    int qualityPreset()
    {
        static Kept<int> kept;
        if (kept.fixed.load(std::memory_order_acquire))
            return kept.value;
        const char *e = std::getenv("TF_NATIVE_QUALITY");
        const std::string v = e ? e : "";
        int level = v == "low" ? 0 : v == "medium" ? 1 : v == "high" ? 2 : v == "ultra" ? 3 : -1;
        const bool chosen = level >= 0;
        const std::optional<uint64_t> known = memoryMB();
        const uint64_t mb = known.value_or(0u);
        if (!chosen)
            level = mb == 0u ? 2 : mb < 3000u ? 0 : mb < 5000u ? 1 : mb < 10000u ? 2 : 3;
        return keep(kept, level, chosen || known.has_value(), [&](int q) {
            static const char *kNames[4] = {"Low", "Medium", "High", "Ultra"};
            std::fprintf(stderr, "[native render] quality %s (%s; %llu MB of video memory)\n", kNames[q], chosen ? "chosen" : "from the graphics card",
                         static_cast<unsigned long long>(mb));
        });
    }

    uint32_t anisotropy()
    {
        static const uint32_t kPresetAniso[4] = {4u, 8u, 16u, 16u};
        const char *e = std::getenv("TF_NATIVE_ANISO");
        return e ? static_cast<uint32_t>(std::clamp(std::atoi(e), 1, 16)) : kPresetAniso[qualityPreset()];
    }

    uint32_t largeImageCap()
    {
        static Kept<uint32_t> kept;
        if (kept.fixed.load(std::memory_order_acquire))
            return kept.value;
        const char *e = std::getenv("TF_HIRES_MAX");
        const bool set = e && *e;
        const std::optional<uint64_t> known = memoryMB();
        const uint64_t mb = known.value_or(0u);
        static const uint32_t kPresetCap[4] = {1024u, 2048u, 2048u, 0u};
        const uint32_t byMemory = mb == 0u ? 0u : mb < 3000u ? 1024u : mb < 5000u ? 2048u : 0u, byPreset = kPresetCap[qualityPreset()];
        const uint32_t c = set ? static_cast<uint32_t>(std::max(0, std::atoi(e))) : !byMemory ? byPreset : !byPreset ? byMemory : std::min(byMemory, byPreset);
        return keep(kept, c, set || known.has_value(), [&](uint32_t cap) {
            std::fprintf(stderr, "[native render] large images: %llu MB of video memory -> %s%s\n", static_cast<unsigned long long>(mb),
                         cap ? ("at most " + std::to_string(cap) + " texels a side").c_str() : "as made", set ? " (TF_HIRES_MAX)" : "");
        });
    }

    // The budget of the rules alone (kept once its inputs are known).
    static uint64_t fixedBudget()
    {
        static Kept<uint64_t> kept;
        if (kept.fixed.load(std::memory_order_acquire))
            return kept.value;
        if (const char *e = std::getenv("TF_HIRES_BUDGET_MB"); e && *e)
            return keep(kept, static_cast<uint64_t>(std::strtoull(e, nullptr, 10)) << 20, true, [](uint64_t) {});
        const std::optional<uint64_t> known = memoryMB();
        const uint64_t mb = known.value_or(0u);
        const uint64_t budget = !mb ? UINT64_MAX : std::max<uint64_t>(384u, std::min<uint64_t>(mb > 1536u ? mb - 1536u : 0u, mb * 3u / 4u)) << 20;
        return keep(kept, budget, known.has_value(), [](uint64_t) {});
    }

    uint64_t largeImageBudget()
    {
        // (Issue #20) The fixed budget, held to the room the card has now (the backend's: Direct3D 12 asks Windows);
        // TF_HIRES_BUDGET_MB is taken as it is.
        static const bool given = std::getenv("TF_HIRES_BUDGET_MB") && *std::getenv("TF_HIRES_BUDGET_MB");
        const uint64_t budget = fixedBudget();
        return given || budget == UINT64_MAX ? budget : backend().largeImageRoom(budget);
    }
}
