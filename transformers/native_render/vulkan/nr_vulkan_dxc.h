// nr_vulkan_dxc.h - HLSL to SPIR-V with DXC's library (dxcompiler.dll beside the game, loaded at run time), for the
// Vulkan backend: the Direct3D 12 backend's HLSL as it is, compiled at start as D3DCompile compiles it there.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace nr::vulkan::dxc
{
    // The compiler loaded (once); false: none (why says what is missing).
    bool available(std::string &why);
    // `source` (size bytes, HLSL) compiled for `entry` at `profile` (vs_6_0, ps_6_0, gs_6_0) with the defines, as SPIR-V
    // for Vulkan 1.1 with Direct3D's buffer layout and the backend's bindings (b0.. at 0, t0.. at 10, s0.. at 20). False:
    // errors holds the compiler's message. Any thread (one compile at a time).
    bool compile(const char *source, size_t size, const char *entry, const char *profile,
                 const std::vector<std::pair<std::string, std::string>> &defines, std::vector<uint32_t> &spirv, std::string &errors);
}
