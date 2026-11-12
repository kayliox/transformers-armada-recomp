// bake_spirv.cpp - the build's SPIR-V bake for the Vulkan backend (nr_vulkan_shaders.h): CMake builds this program and
// runs it after the HLSL changes - `tf_vulkan_bake <dxcompiler.dll> <nr_spirv_baked.cpp>` - so the game carries its
// SPIR-V and never needs a shader compiler (Direct3D 12's counterpart: build.ps1's --bake-shaders, nr_d3d.cpp).
// Every shader the backend makes at start (nr_vulkan.cpp's makeShaders: keep the two lists alike - a shader missing
// here is reported by the backend at start as not built in) and every pixel shader variant psv:: may ask for.
// Exit code 0: the table written; 1: a shader failed or the file could not be written (the build stops).
#include "nr_hlsl_sources.h" // (generated from nr_d3d.cpp: extract_hlsl.cmake)
#include "nr_vulkan_dxc.h"
#include "nr_vulkan_shaders.h"

#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace
{
    using namespace nr::vulkan;

    // SMAA.hlsl's text as string pieces, as nr_d3d.cpp and nr_vulkan.cpp include it (between kSmaaPrelude and kSmaaEntries).
    const char *const kSmaaSource[] = {
#include "ext/smaa/smaa_hlsl.inc"
    };

    struct Bake
    {
        std::map<uint64_t, std::vector<uint32_t>> table;
        int failed = 0, compiled = 0;

        void add(const char *source, size_t size, const char *entry, const char *profile, const shaders::Defines &defines = {})
        {
            const uint64_t key = shaders::keyOf(source, size, entry, profile, defines);
            if (table.count(key)) // (variants with the same defines: one shader)
                return;
            std::vector<uint32_t> spirv;
            std::string errors;
            if (!dxc::compile(source, size, entry, profile, defines, spirv, errors))
            {
                std::string what;
                for (const auto &[name, value] : defines)
                    what += " " + name + "=" + value;
                std::fprintf(stderr, "bake_spirv: %s (%s%s): %s\n", entry, profile, what.c_str(), errors.empty() ? "failed" : errors.c_str());
                ++failed;
                return;
            }
            ++compiled;
            table.emplace(key, std::move(spirv));
        }
    };

    bool writeTable(const char *path, const std::map<uint64_t, std::vector<uint32_t>> &table, size_t &bytes)
    {
        std::string text = "// Generated at build time by native_render/vulkan/bake_spirv.cpp (DXC): the Vulkan backend's shaders as\n"
                           "// SPIR-V, read by nr_vulkan_shaders.h's find(). Not kept in the repository; do not edit.\n"
                           "#include \"nr_vulkan_shaders.h\"\n\nnamespace nr::vulkan::shaders\n{\n";
        char buf[96];
        size_t index = 0;
        bytes = 0;
        for (const auto &[key, words] : table)
        {
            std::snprintf(buf, sizeof(buf), "    static const uint32_t kWords%zu[] = {", index++);
            text += buf;
            for (size_t i = 0; i < words.size(); ++i)
            {
                std::snprintf(buf, sizeof(buf), "%s0x%08x,", (i % 12u) == 0u ? "\n        " : "", static_cast<unsigned>(words[i]));
                text += buf;
            }
            text += "};\n";
            bytes += words.size() * 4u;
        }
        text += "    const Baked kBaked[] = { // (external: declared so in nr_vulkan_shaders.h)\n";
        index = 0;
        for (const auto &[key, words] : table)
        {
            std::snprintf(buf, sizeof(buf), "        {0x%016llxull, kWords%zu, %zuu},\n", static_cast<unsigned long long>(key), index++, words.size());
            text += buf;
        }
        text += "    };\n";
        std::snprintf(buf, sizeof(buf), "    const size_t kBakedCount = %zu;\n}\n", table.size());
        text += buf;
        FILE *f = std::fopen(path, "wb");
        if (!f)
            return false;
        const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
        return std::fclose(f) == 0 && ok;
    }
}

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        std::fprintf(stderr, "usage: tf_vulkan_bake <dxcompiler.dll> <nr_spirv_baked.cpp>\n");
        return 1;
    }
    const auto t0 = std::chrono::steady_clock::now();
    std::string why;
    if (!dxc::load(argv[1], why))
    {
        std::fprintf(stderr, "bake_spirv: %s\n", why.c_str());
        return 1;
    }
    Bake b;
    using namespace hlsl;
    const size_t kS = sizeof(kShader) - 1u;
    // The main shader's stages and the pixel shader's variants (bit 0 set: the 128 odd values, 64 different define sets).
    b.add(kShader, kS, "vs", "vs_6_0");
    b.add(kShader, kS, "ps", "ps_6_0");
    b.add(kShader, kS, "gsClip", "gs_6_0");
    for (uint32_t v = 1u; v < 256u; v += 2u)
        b.add(kShader, kS, "ps", "ps_6_0", shaders::variantDefines(v));
    // The effects: FXAA, the probe, the sparks, the bloom, the tone mapping, the local contrast and SMAA 1x.
    b.add(kAaShader, sizeof(kAaShader) - 1u, "aaVs", "vs_6_0");
    b.add(kAaShader, sizeof(kAaShader) - 1u, "aaPs", "ps_6_0");
    b.add(kProbeShader, sizeof(kProbeShader) - 1u, "probeVs", "vs_6_0");
    b.add(kProbeShader, sizeof(kProbeShader) - 1u, "probePs", "ps_6_0");
    b.add(kCloudShader, sizeof(kCloudShader) - 1u, "cloudVs", "vs_6_0");
    b.add(kCloudShader, sizeof(kCloudShader) - 1u, "cloudPs", "ps_6_0");
    b.add(kBloomShader, sizeof(kBloomShader) - 1u, "bloomVs", "vs_6_0");
    b.add(kBloomShader, sizeof(kBloomShader) - 1u, "bloomDownPs", "ps_6_0");
    b.add(kBloomShader, sizeof(kBloomShader) - 1u, "bloomUpPs", "ps_6_0");
    b.add(kToneShader, sizeof(kToneShader) - 1u, "toneVs", "vs_6_0");
    b.add(kToneShader, sizeof(kToneShader) - 1u, "tonePs", "ps_6_0");
    b.add(kClarityShader, sizeof(kClarityShader) - 1u, "clarityVs", "vs_6_0");
    b.add(kClarityShader, sizeof(kClarityShader) - 1u, "clarityPs", "ps_6_0");
    {
        // (SMAA 1x: its prelude - our porting macros, the "high" preset - SMAA.hlsl, its entries: D3D12's text)
        std::string smaa(kSmaaPrelude, sizeof(kSmaaPrelude) - 1u);
        for (const char *piece : kSmaaSource)
            smaa += piece;
        smaa.append(kSmaaEntries, sizeof(kSmaaEntries) - 1u);
        const char *const names[6] = {"smaaEdgeVs", "smaaEdgePs", "smaaWeightVs", "smaaWeightPs", "smaaBlendVs", "smaaBlendPs"};
        for (int k = 0; k < 6; ++k)
            b.add(smaa.c_str(), smaa.size(), names[k], (k & 1) ? "ps_6_0" : "vs_6_0");
    }
    if (b.failed)
    {
        std::fprintf(stderr, "bake_spirv: %d shader(s) failed to compile (see above)\n", b.failed);
        return 1;
    }
    size_t bytes = 0;
    if (!writeTable(argv[2], b.table, bytes))
    {
        std::fprintf(stderr, "bake_spirv: cannot write %s\n", argv[2]);
        return 1;
    }
    std::printf("SPIR-V: %zu shaders built in (%zu KB), compiled in %.1f s\n", b.table.size(), bytes / 1024u,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return 0;
}
