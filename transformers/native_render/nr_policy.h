// nr_policy.h - the native renderer's video-memory policy (TODO 94), the same rules for every backend: the quality
// preset, the anisotropy it sets, and the large (HD) images' longest side and video memory budget. A backend gives only
// its video memory figure (Backend::adapterMemoryMB, nr_backend.h: Direct3D 12 its adapter's dedicated memory from
// DXGI, Vulkan its largest device-local heap); TF_NATIVE_VRAM_MB=<n> pretends another size. Each value is fixed - and
// its log line written - the first time it is asked for once that figure is known (Direct3D 12: at once; Vulkan: once
// its GPU is chosen; before that a value is worked out from no figure and not kept). Any thread.
#pragma once

#include <cstdint>

namespace nr::policy
{
    // The quality preset (TF_NATIVE_QUALITY = low / medium / high / ultra; unset or auto: from the video memory - under
    // ~3 GB low, ~5 GB medium, ~10 GB high, else ultra): 0..3. What it sets - anisotropy 4 / 8 / 16 / 16, the large
    // images at most 1024 / 2048 / 2048 / as made a side (and never above what the video memory allows), the core's
    // clipping by the GPU's own clipper on Low and Medium (nr_frame.cpp) - yields to its own variable when that is set
    // (TF_NATIVE_ANISO, TF_HIRES_MAX, TF_NATIVE_CLIPGS).
    int qualityPreset();

    // The textures' anisotropy (TF_NATIVE_ANISO=<1..16>; else the quality preset's: 4, 8, 16, 16).
    uint32_t anisotropy();

    // The longest side a large image is used at (0: as made; TF_HIRES_MAX=<n> sets it): on a card of under ~5 GB 2048
    // (the level of its chain of that size: 4096 images a quarter of their memory), under ~3 GB 1024, and never above
    // the quality preset's.
    uint32_t largeImageCap();

    // The bytes the large images may hold on the GPU before the ones drawn least recently are let go of
    // (TF_HIRES_BUDGET_MB=<n> sets it): the video memory less 1.5 GB (the frame's targets, the game's textures, the
    // meshes), at most three quarters of it, at least 384 MB. UINT64_MAX: no limit (the video memory not known).
    uint64_t largeImageBudget();
}
