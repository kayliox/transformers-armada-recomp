// nr_vulkan_shaders.h - the Vulkan backend's shaders as SPIR-V built into the game. The build compiles them from the
// Direct3D 12 backend's HLSL (bake_spirv.cpp, a program of the build's own that CMake runs with DXC's release: every
// shader the backend makes and every pixel shader variant) into a table of SPIR-V words (nr_spirv_baked.cpp, generated),
// so the game needs no shader compiler at run time and no dxcompiler.dll beside it. Shared by the bake and the backend:
// DXC's switches, the variants' defines and the key that finds a shader in the table.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace nr::vulkan::shaders
{
    using Defines = std::vector<std::pair<std::string, std::string>>;

    // DXC's switches for every shader (the bake adds -E <entry>, -T <profile> and the defines). SPIR-V with Direct3D's
    // constant buffer layout, so the C++ structs stay as they are; the Direct3D 12 root signature's registers as one
    // descriptor set: b0, b1 at 0, 1; t0-t2 at 10-12; s0-s5 at 20-25 (Y is flipped by the viewport).
    // -HV 2018: the language as D3DCompile reads it (HLSL 2021's ternary on vectors refused the tone mapping's).
    // ddx / ddy as the coarse derivatives: D3DCompile's ddx is ddx_coarse, SPIR-V's plain derivative leaves the choice to
    // the driver - the texture levels the shader picks itself (sampleGs) came out otherwise in fine detail.
    // Vulkan 1.3: `discard` becomes OpDemoteToHelperInvocation - the pixel stays in its 2 x 2 quad as a helper, as
    // Direct3D's discard does, so its neighbours' derivatives (the texture's level) stay right; 1.1's OpKill ended it, and
    // the alpha-tested foliage's neighbours took other levels.
    inline constexpr const char *kCompilerArgs[] = {"-spirv", "-HV", "2018", "-D", "ddx=ddx_coarse", "-D", "ddy=ddy_coarse",
                                                    "-fspv-target-env=vulkan1.3", "-fvk-use-dx-layout",
                                                    "-fvk-b-shift", "0", "0", "-fvk-t-shift", "10", "0",
                                                    "-fvk-s-shift", "20", "0", "-fvk-u-shift", "30", "0", "-O3"};

    inline uint64_t fnv(uint64_t h, const void *data, size_t size)
    {
        const auto *p = static_cast<const unsigned char *>(data);
        for (size_t i = 0; i < size; ++i)
            h = (h ^ p[i]) * 1099511628211ull;
        return h;
    }

    // A shader's key in the table: FNV-1a over DXC's switches, the HLSL, the entry point, the profile and the defines.
    // The bake keys what it compiles and the backend what it asks for from the same text, so a shader whose HLSL or
    // switches changed since the table was made is not found (the backend says so) rather than taken stale.
    inline uint64_t keyOf(const char *source, size_t size, const char *entry, const char *profile, const Defines &defines)
    {
        uint64_t h = fnv(1469598103934665603ull, "nr vulkan spirv 1", 17);
        for (const char *a : kCompilerArgs)
            h = fnv(h, a, std::strlen(a) + 1u);
        h = fnv(h, source, size);
        h = fnv(h, entry, std::strlen(entry) + 1u);
        h = fnv(h, profile, std::strlen(profile) + 1u);
        for (const auto &[name, value] : defines)
        {
            h = fnv(h, name.c_str(), name.size() + 1u);
            h = fnv(h, value.c_str(), value.size() + 1u);
        }
        return h;
    }

    // The pixel shader variant v's defines (nr_d3d.cpp's psv::compileVariant): bit 0 set (a variant), bits 1-2 PSV_TEX
    // (0 / 1), 3 PSV_LOD, 4 PSV_CLAMP, 5 PSV_ATEST, 6 PSV_DATE, 7 PSV_CULL. The 128 odd values give 64 different sets.
    inline Defines variantDefines(uint32_t v)
    {
        static const char *const names[6] = {"PSV_TEX", "PSV_LOD", "PSV_CLAMP", "PSV_ATEST", "PSV_DATE", "PSV_CULL"};
        const bool on[6] = {((v >> 1) & 3u) != 0u, ((v >> 3) & 1u) != 0u, ((v >> 4) & 1u) != 0u,
                            ((v >> 5) & 1u) != 0u, ((v >> 6) & 1u) != 0u, ((v >> 7) & 1u) != 0u};
        Defines d;
        for (int k = 0; k < 6; ++k)
            d.emplace_back(names[k], on[k] ? "1" : "0");
        return d;
    }

    // A shader in the table: its key and its SPIR-V.
    struct Baked
    {
        uint64_t key;
        const uint32_t *words;
        uint32_t count;
    };
    // The table the build made (nr_spirv_baked.cpp), sorted by key; kBakedCount 0 when this build has none.
    extern const Baked kBaked[];
    extern const size_t kBakedCount;

    inline const Baked *find(uint64_t key)
    {
        size_t lo = 0, hi = kBakedCount;
        while (lo < hi)
        {
            const size_t mid = (lo + hi) / 2u;
            if (kBaked[mid].key < key)
                lo = mid + 1u;
            else
                hi = mid;
        }
        return lo < kBakedCount && kBaked[lo].key == key ? &kBaked[lo] : nullptr;
    }
}
