// nr_vulkan_dxc.h - HLSL to SPIR-V with DXC's library, for the build's SPIR-V bake (bake_spirv.cpp): the Direct3D 12
// backend's HLSL as it is, compiled as D3DCompile compiles it there. Not part of the game: it runs only at build time.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace nr::vulkan::dxc
{
    // DXC's library loaded from that file (dxcompiler.dll of the release the build fetched); false: why says what failed.
    bool load(const char *library, std::string &why);
    // `source` (size bytes, HLSL) compiled for `entry` at `profile` (vs_6_0, ps_6_0, gs_6_0) with the defines and
    // nr_vulkan_shaders.h's switches. False: errors holds the compiler's message. One compile at a time.
    bool compile(const char *source, size_t size, const char *entry, const char *profile,
                 const std::vector<std::pair<std::string, std::string>> &defines, std::vector<uint32_t> &spirv, std::string &errors);
}
