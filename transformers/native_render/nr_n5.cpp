// N5 (docs/N5_PLAN.md): the render device native - no PS2 display lists, VIF1 or VU1 packets. This file holds N5's
// own parts, outside the capture (nr_capture.cpp, the renderer's), so the stages stay apart from it.
//
// N5.0, the census (TF_N5_CENSUS=<seconds>, 10 if just set): who writes the display list. Every function of the render
// device's vtable (vlRenderDevicePS2, 0x006B4450, 85 slots) and the PS2 helpers and effects that write packets (the
// table below) is wrapped: the packet pointer D_70000D30 read before and after, so each function's bytes are known -
// exclusive of the wrapped functions it calls (their own rows) - and how often it ran. Bytes that appear between two
// wrapped calls at the top level were written by code that is not wrapped: they are put to the return address of the
// next wrapped call (inside the function that wrote them, as a rule). Per list (kick to kick, BeginDisplayListDmaTransfer 0x0025D2B0): the list's
// size, the share of it each writer made. Every <seconds>: "[n5:census]" lines. Changes nothing the game does.
// With TF_N5_CENSUS_REGS=1 also which GS registers each function's own bytes write (A+D and REGLIST in VIF DIRECT) and,
// apart, those of the packets its DMA tags point at (REF / REFS / CALL) - where the register stream's writes come from.
//
// N5.3, the device's functions native (TF_N5_DEVICE: on by default; =0 off; =check), first group: SetGSRegister and
// ApplyStateBlock (nr_capture.cpp, beside its wrappers of them) and vlTextureChange::UseTexture (here): no packet, what
// they set handed to the renderer (nr::deviceRegister), the recompiled originals kept for the GS and as the oracle.
// Second group (here): SetHardwareRenderEnvironment, with the render-state slots it calls (GetRenderState, and
// SetRenderState's GS-context switch). All of them only while the frames are deferred (nr::framesDeferred).
//
// N5.2, no VIF1 work (TF_N5: on by default with the native renderer; =0 off; =check). While the native renderer draws
// alone (ps2_host_display::gsNativeAlone) and no screen of the game's own is up (outsideFrames, the warp tunnel), the
// display list is still built - the native renderer reads it from main memory - and still sent through VIF1, for what
// the GS keeps (the uploads, the registers) and what the EE sees (MARK, the interrupts, the DMA's end); but VIF1 leaves
// out what only VU1's programs use (PS2Recomp patch 0074, ps2vif1::setLite): no UNPACK into VU1 data memory, no program
// started. =check: VIF1 as before, and beside it the VU1 data memory such transfers would have left (the EE's own
// writes only): every EE read of VU1 data memory compared with it ("[n5:check]").
#include "nr_n5.h"
#include "game_version.h" // (TF_GA: the code addresses that differ in the PAL release)

#include "nr_types.h"
#include "nr_vif.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_host_display.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_vif1_thread.h"
#include "runtime/ps2_vu1_native.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
void tfLogCallStack(const char *label); // (profiler.cpp: the host stack, guest function names in it)
#endif

namespace nr::n5
{
    namespace
    {
        constexpr uint32_t kPacketPointer = 0x70000D30u; // D_70000D30: where the next DMA words of the list go
        constexpr uint32_t kDeviceVtable = 0x006B4450u;  // vlRenderDevicePS2's (85 slots)
        constexpr uint32_t kDeviceSlots = 85u;
        constexpr uint32_t kKick = 0x0025D2B0u;          // BeginDisplayListDmaTransfer: closes the list, kicks it, starts the other buffer
        constexpr uint32_t kJump = 16u * 1024u * 1024u;  // (a pointer move larger than this: the other buffer - not bytes written)

        uint32_t word(const uint8_t *rdram, uint32_t addr)
        {
            uint32_t v = 0u;
            if (const uint32_t ram = addr & ~0x20000000u; ram <= PS2_RAM_SIZE - 4u && rdram)
            {
                std::memcpy(&v, rdram + ram, 4);
                return v;
            }
            if (const uint8_t *p = getConstMemPtr(rdram, addr))
                std::memcpy(&v, p, 4);
            return v;
        }

        // The PS2 helpers and effects that write packets (docs/N5_PLAN.md §4), beside the device's slots.
        struct Named
        {
            uint32_t address;
            const char *name;
        };
        constexpr Named kHelpers[] = {
            {0x00225D60u, "SetGSRegister"},
            {0x00225C50u, "SetGSRegisterPair"},
            {0x00225DE0u, "func_00225DE0 (GS register)"},
            {0x00225C20u, "ReportGSRegisterChange"},
            {0x00221DB0u, "LoadMicrocodeContext"},
            {0x00221CC0u, "SetupContextTransferPacket"},
            {0x00221BD0u, "LoadClipContext"},
            {0x00221E70u, "SetupSonyCameraContext"},
            {0x00221D40u, "UpdateModelToDeviceClipSpace"},
            {0x00225300u, "microcode Upload(block)"},
            {0x00225610u, "microcode Upload(index)"},
            {0x00225250u, "microcode Flush(index)"},
            {0x002601B0u, "SetAlphaEnv"},
            {0x0025D2B0u, "BeginDisplayListDmaTransfer"},
            {0x00221240u, "PrimitiveStart"},
            {0x00254A70u, "PrimitiveStartSpriteSet"},
            {0x00220D40u, "PrimitiveEndSpriteSet"},
            {0x002210D0u, "PrimitiveAddSpriteVertex (from memory)"},
            {0x00220F60u, "PrimitiveAddSpriteVertex"},
            {0x00254990u, "PrimitiveStartTriStripSet"},
            {0x00220D30u, "PrimitiveStartTriStrip"},
            {0x00220350u, "PrimitiveEndTriStripSet"},
            {0x00220A10u, "PrimitiveAddTriStripVertex (from memory)"},
            {0x002206D0u, "PrimitiveAddTriStripVertex"},
            {0x002202E0u, "PrimitiveReRender"},
            {0x00256AC0u, "RenderLinePrimitives"},
            {0x00267060u, "texture level upload"},
            {0x0023C9F0u, "texture group load"},
            {0x0023C340u, "vlTextureChange::UseTexture"},
            {0x0023C660u, "vlTextureChange::ContextChange"},
            {0x0023CAE0u, "func_0023CAE0 (texture change)"},
            {0x0023AB70u, "shader list flush"},
            {0x0023AA90u, "shader dirty contexts"},
            {0x0023A9A0u, "SetShaderBlock (manager)"},
            {0x00262000u, "vlShader_TransformClip LoadContext"},
            {0x00274680u, "DualTransformInstancer"},
            {0x00264DB0u, "SetHardwareRenderEnvironment"},
            {0x00265370u, "SetupRenderEnvironment"},
            {0x00263120u, "SetHardwareDisplayEnvironment"},
            {0x0026F790u, "grass grid packet"},
            {0x00253F00u, "vlxParticleManager"},
            {0x0024FC50u, "particle trail emitter"},
            {TF_GA(0x003711A0u), "taParticleManager"},
            {TF_GA(0x004B8CC0u), "CreatureEffects RenderNode"},
            {TF_GA(0x004B97A0u), "CreatureEffects func_004B97A0"},
            {TF_GA(0x004B7B60u), "CreatureEffects func_004B7B60"},
            {0x00232E60u, "vlxText2D char"},
            {0x00206A50u, "BlendSurfaceRect"},
            {0x00204530u, "ApplyBlurEffects"},
            {0x002642F0u, "CopyGreenToAlpha"},
            {TF_GA(0x004E19A0u), "snow func_004E19A0"},
            {TF_GA(0x004E1B10u), "snow func_004E1B10"},
            {0x00272010u, "water func_00272010"},
            {0x00272260u, "water func_00272260"},
            {TF_GA(0x004C59B0u), "RenderShadowTexture"},
            {TF_GA(0x003536B0u), "AllocateCaster"},
            {0x0025FD40u, "func_0025FD40"},
            {0x0025FD60u, "func_0025FD60"},
            {0x002261A0u, "func_002261A0"},
            {0x00226540u, "func_00226540"},
        };

        constexpr int kMax = 192;
        struct Wrapped
        {
            uint32_t address = 0u;
            char name[64] = {};
            PS2Runtime::RecompiledFunction original = nullptr;
            uint64_t calls = 0u, bytes = 0u, inclusive = 0u; // (since the last report)
            uint64_t regs[256] = {}; // (TF_N5_CENSUS_REGS: the GS registers its own bytes write, A+D or REGLIST; +128: through the packets its DMA tags reference)
        };
        Wrapped g_wrapped[kMax];
        int g_count = 0;

        struct State
        {
            int depth = 0;
            std::vector<uint64_t> childBytes; // per depth: the bytes the wrapped calls under it made
            std::vector<std::vector<std::pair<uint32_t, uint32_t>>> childRanges; // (TF_N5_CENSUS_REGS: per depth, the calls under it)
            uint32_t lastTop = 0u;            // the packet pointer when the last top-level wrapped call ended
            bool haveLastTop = false;
            uint32_t frameStart = 0u;
            bool haveFrameStart = false;
            uint64_t frames = 0u, frameBytes = 0u, frameMax = 0u, gaps = 0u;
            std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>> gapByRa; // ra -> bytes, times
            uint64_t gapRegs[256] = {};
            std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
        } g_state;

        uint32_t seconds()
        {
            static const uint32_t value = []() -> uint32_t {
                const char *e = std::getenv("TF_N5_CENSUS");
                if (!e || !*e || *e == '0')
                    return 0u;
                const int n = std::atoi(e);
                return n > 1 ? static_cast<uint32_t>(n) : 10u;
            }();
            return value;
        }

        bool censusRegs()
        {
            static const bool on = std::getenv("TF_N5_CENSUS_REGS") && *std::getenv("TF_N5_CENSUS_REGS") != '0';
            return on;
        }

        // (TF_N5_CENSUS_REGS) The GS registers written in [from, to) of the list, the ranges in `skip` left out: every
        // VIF DIRECT / DIRECTHL in a qword's upper half (a DMA tag's VIF codes) and its GIF packets - PACKED A+D entries and
        // REGLIST registers - counted into regs.
        // (TF_N5_CENSUS_REGS) A GIF packet's register writes in [at, end): PACKED A+D entries and REGLIST registers.
        void countGif(const uint8_t *rdram, uint32_t at, uint32_t end, uint64_t *regs)
        {
            while (at + 16u <= end)
            {
                const uint32_t t0 = word(rdram, at), t1 = word(rdram, at + 4u), r0 = word(rdram, at + 8u), r1 = word(rdram, at + 12u);
                const uint32_t nloop = t0 & 0x7FFFu, flg = (t1 >> 26) & 3u;
                uint32_t nreg = (t1 >> 28) & 0xFu;
                nreg = nreg ? nreg : 16u;
                const uint64_t regList = static_cast<uint64_t>(r0) | static_cast<uint64_t>(r1) << 32;
                at += 16u;
                if (flg == 0u) // PACKED
                    for (uint32_t l = 0; l < nloop && at + 16u <= end; ++l)
                        for (uint32_t r = 0; r < nreg && at + 16u <= end; ++r, at += 16u)
                        {
                            if (((regList >> (4u * r)) & 0xFu) == 0xEu)
                                ++regs[word(rdram, at + 8u) & 0x7Fu];
                        }
                else if (flg == 1u) // REGLIST: 64-bit register values, two a qword
                {
                    const uint32_t count = nloop * nreg;
                    for (uint32_t i = 0; i < count; ++i)
                    {
                        const uint32_t reg = static_cast<uint32_t>((regList >> (4u * (i % nreg))) & 0xFu);
                        if (reg != 0xFu && reg != 0xEu)
                            ++regs[reg];
                    }
                    at += 8u * ((count + 1u) & ~1u);
                }
                else // IMAGE: its data
                    at += 16u * nloop;
            }
        }

        // The VIF DIRECT / DIRECTHL in a qword's upper half (a DMA tag's VIF codes): its qword count, 0 if none.
        uint32_t directIn(const uint8_t *rdram, uint32_t q)
        {
            uint32_t direct = 0u;
            for (int k = 2; k < 4; ++k)
            {
                const uint32_t code = word(rdram, q + 4u * static_cast<uint32_t>(k));
                const uint32_t cmd = (code >> 24) & 0x7Fu;
                if (cmd == 0x50u || cmd == 0x51u)
                    direct = (code & 0xFFFFu) ? (code & 0xFFFFu) : 65536u;
            }
            return direct;
        }

        // A DMA chain from `tag` (a CALL's): each tag's GIF data counted, until its RET / END (or 32 tags).
        void countChain(const uint8_t *rdram, uint32_t tag, uint64_t *regs)
        {
            for (int guard = 0; guard < 32; ++guard)
            {
                const uint32_t lo = word(rdram, tag), addr = word(rdram, tag + 4u) & 0x7FFFFFF0u;
                const uint32_t qwc = lo & 0xFFFFu, id = (lo >> 28) & 7u;
                const uint32_t data = (id == 3u || id == 4u || id == 0u) ? addr : tag + 16u;
                if (const uint32_t direct = directIn(rdram, tag))
                    countGif(rdram, data, data + 16u * std::min(direct, qwc), regs);
                if (id == 1u || id == 5u)
                    tag = tag + 16u + 16u * qwc; // (cnt; a nested call: its data skipped)
                else if (id == 2u)
                    tag = addr;
                else if (id == 3u || id == 4u)
                    tag += 16u;
                else
                    return; // (ret, refe, end)
            }
        }

        // (TF_N5_CENSUS_REGS) The GS registers written in [from, to) of the list, the ranges in `skip` left out: every
        // VIF DIRECT / DIRECTHL in a qword's upper half (a DMA tag's VIF codes) and its GIF packets - PACKED A+D entries and
        // REGLIST registers - counted into regs; the packets a REF / REFS / CALL tag points at into regs + 128.
        void countRegisters(const uint8_t *rdram, uint32_t from, uint32_t to, const std::vector<std::pair<uint32_t, uint32_t>> &skip, uint64_t *regs)
        {
            from &= ~15u;
            auto skipped = [&](uint32_t a) {
                for (const auto &r : skip)
                    if (a >= (r.first & ~15u) && a < r.second)
                        return true;
                return false;
            };
            for (uint32_t q = from; q + 16u <= to; q += 16u)
            {
                if (skipped(q))
                    continue;
                const uint32_t lo = word(rdram, q), addr = word(rdram, q + 4u) & 0x7FFFFFF0u;
                const uint32_t qwc = lo & 0xFFFFu, id = (lo >> 28) & 7u;
                const bool tagLike = (lo & 0x0FFF0000u) == 0u && addr < PS2_RAM_SIZE; // (a DMA tag: PCE/ID/IRQ bits only above the qword count)
                if (tagLike && id == 5u && addr)
                {
                    countChain(rdram, addr, regs + 128);
                    continue;
                }
                const uint32_t direct = directIn(rdram, q);
                if (!direct)
                    continue;
                if (tagLike && (id == 3u || id == 4u) && addr)
                {
                    countGif(rdram, addr, addr + 16u * std::min(direct, qwc ? qwc : direct), regs + 128);
                    continue;
                }
                const uint32_t at = q + 16u, end = std::min(to, at + 16u * direct);
                countGif(rdram, at, end, regs);
                q = std::max(q, end - 16u);
            }
        }

        void report()
        {
            State &s = g_state;
            const auto now = std::chrono::steady_clock::now();
            const double secs = std::chrono::duration<double>(now - s.last).count();
            s.last = now;
            const double frames = s.frames ? static_cast<double>(s.frames) : 1.0;
            uint64_t attributed = 0u;
            std::vector<Wrapped *> rows;
            for (int i = 0; i < g_count; ++i)
                if (g_wrapped[i].calls)
                {
                    rows.push_back(&g_wrapped[i]);
                    attributed += g_wrapped[i].bytes;
                }
            std::sort(rows.begin(), rows.end(), [](const Wrapped *a, const Wrapped *b) { return a->bytes != b->bytes ? a->bytes > b->bytes : a->calls > b->calls; });
            uint64_t gapBytes = 0u;
            for (const auto &kv : s.gapByRa)
                gapBytes += kv.second.first;
            std::fprintf(stderr, "[n5:census] %.1f s, %llu lists: a list %.1f KB (largest %.1f KB); of it by wrapped functions %.1f KB, between them (not wrapped) %.1f KB\n",
                         secs, static_cast<unsigned long long>(s.frames), s.frameBytes / 1024.0 / frames, s.frameMax / 1024.0, attributed / 1024.0 / frames,
                         gapBytes / 1024.0 / frames);
            for (size_t i = 0; i < rows.size() && i < 40u; ++i)
            {
                const Wrapped &w = *rows[i];
                std::fprintf(stderr, "[n5:census]   %08x %-40s %10.1f calls a list, %8.2f KB a list (with the wrapped under it %.2f KB)\n", w.address, w.name,
                             w.calls / frames, w.bytes / 1024.0 / frames, w.inclusive / 1024.0 / frames);
                if (censusRegs())
                {
                    std::string regs;
                    for (uint32_t r = 0x06u; r < 0x80u; ++r)
                        if (w.regs[r] && r != 0x0Cu && r != 0x0Du)
                        {
                            char e[32];
                            std::snprintf(e, sizeof(e), " %02x:%.1f", r, w.regs[r] / frames);
                            regs += e;
                        }
                    if (!regs.empty())
                        std::fprintf(stderr, "[n5:census]     GS registers it writes, a list:%s\n", regs.c_str());
                    std::string refs;
                    for (uint32_t r = 0x06u; r < 0x80u; ++r)
                        if (w.regs[128u + r] && r != 0x0Cu && r != 0x0Du)
                        {
                            char e[32];
                            std::snprintf(e, sizeof(e), " %02x:%.1f", r, w.regs[128u + r] / frames);
                            refs += e;
                        }
                    if (!refs.empty())
                        std::fprintf(stderr, "[n5:census]     GS registers through the packets its DMA tags point at, a list:%s\n", refs.c_str());
                }
            }
            std::vector<std::pair<uint32_t, std::pair<uint64_t, uint64_t>>> gaps(s.gapByRa.begin(), s.gapByRa.end());
            std::sort(gaps.begin(), gaps.end(), [](const auto &a, const auto &b) { return a.second.first > b.second.first; });
            for (size_t i = 0; i < gaps.size() && i < 16u; ++i)
                std::fprintf(stderr, "[n5:census]   not wrapped, before a wrapped call returning to %08x: %8.2f KB a list (%.1f times)\n", gaps[i].first,
                             gaps[i].second.first / 1024.0 / frames, gaps[i].second.second / frames);
            for (int i = 0; i < g_count; ++i)
            {
                g_wrapped[i].calls = g_wrapped[i].bytes = g_wrapped[i].inclusive = 0u;
                std::memset(g_wrapped[i].regs, 0, sizeof(g_wrapped[i].regs));
            }
            if (censusRegs())
            {
                std::string regs;
                for (uint32_t r = 0x06u; r < 0x80u; ++r)
                    if (s.gapRegs[r] && r != 0x0Cu && r != 0x0Du)
                    {
                        char e[32];
                        std::snprintf(e, sizeof(e), " %02x:%.1f", r, s.gapRegs[r] / frames);
                        regs += e;
                    }
                std::fprintf(stderr, "[n5:census]   GS registers written by code not wrapped, a list:%s\n", regs.c_str());
                std::memset(s.gapRegs, 0, sizeof(s.gapRegs));
            }
            s.gapByRa.clear();
            s.frames = s.frameBytes = s.frameMax = 0u;
        }

        template <int I> void wrapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = g_state;
            Wrapped &w = g_wrapped[I];
            const uint32_t before = word(rdram, kPacketPointer);
            if (s.depth == 0)
            {
                // Bytes since the last top-level wrapped call: written by code not wrapped - put to this call's return address.
                if (s.haveLastTop && before > s.lastTop && before - s.lastTop < kJump)
                {
                    auto &g = s.gapByRa[getRegU32(ctx, 31)];
                    g.first += before - s.lastTop;
                    ++g.second;
                    if (censusRegs())
                        countRegisters(rdram, s.lastTop, before, {}, s.gapRegs);
                }
            }
            if (w.address == kKick)
            {
                // A list ends here (at any depth: BeginFrame calls it): from the last kick's new buffer to now.
                if (s.haveFrameStart && before >= s.frameStart && before - s.frameStart < kJump)
                {
                    const uint64_t size = before - s.frameStart;
                    s.frameBytes += size;
                    s.frameMax = std::max<uint64_t>(s.frameMax, size);
                    ++s.frames;
                }
                if (seconds() && std::chrono::steady_clock::now() - s.last >= std::chrono::seconds(seconds()))
                    report();
            }
            if (s.childBytes.size() <= static_cast<size_t>(s.depth))
                s.childBytes.resize(static_cast<size_t>(s.depth) + 1u);
            s.childBytes[static_cast<size_t>(s.depth)] = 0u;
            if (censusRegs())
            {
                if (s.childRanges.size() <= static_cast<size_t>(s.depth))
                    s.childRanges.resize(static_cast<size_t>(s.depth) + 1u);
                s.childRanges[static_cast<size_t>(s.depth)].clear();
            }
            ++s.depth;
            w.original(rdram, ctx, runtime);
            --s.depth;
            const uint32_t after = word(rdram, kPacketPointer);
            ++w.calls;
            uint64_t made = 0u;
            if (w.address == kKick)
            {
                // (the pointer moved to the other buffer: no bytes; the next list starts there)
                s.frameStart = after;
                s.haveFrameStart = true;
                s.lastTop = after;
                s.haveLastTop = s.depth == 0 ? true : s.haveLastTop;
            }
            else if (after >= before && after - before < kJump)
                made = after - before;
            const uint64_t under = s.childBytes[static_cast<size_t>(s.depth)];
            if (censusRegs() && made)
            {
                countRegisters(rdram, before, after, s.childRanges[static_cast<size_t>(s.depth)], w.regs);
                if (s.depth > 0)
                    s.childRanges[static_cast<size_t>(s.depth) - 1u].push_back({before, after});
            }
            w.inclusive += made;
            w.bytes += made > under ? made - under : 0u;
            if (s.depth > 0)
                s.childBytes[static_cast<size_t>(s.depth) - 1u] += made;
            else
            {
                s.lastTop = after;
                s.haveLastTop = true;
            }
        }

        template <int... I> constexpr std::array<PS2Runtime::RecompiledFunction, sizeof...(I)> table(std::integer_sequence<int, I...>)
        {
            return {wrapped<I>...};
        }

        bool add(PS2Runtime &runtime, uint32_t address, const char *name)
        {
            static const auto fns = table(std::make_integer_sequence<int, kMax>{});
            if (g_count >= kMax || address < 0x00100000u || address >= 0x00700000u)
                return false;
            for (int i = 0; i < g_count; ++i)
                if (g_wrapped[i].address == address)
                    return false;
            const PS2Runtime::RecompiledFunction original = runtime.lookupFunction(address);
            if (!original)
                return false;
            Wrapped &w = g_wrapped[g_count];
            w.address = address;
            std::snprintf(w.name, sizeof(w.name), "%s", name);
            w.original = original;
            runtime.registerFunction(address, fns[static_cast<size_t>(g_count)]);
            ++g_count;
            return true;
        }
    }

    // ---- N5.2: VIF1 without the work only VU1 uses ----
    namespace
    {
        enum class Mode : uint8_t
        {
            Off,
            On,
            Check,
        };

        Mode mode()
        {
            static const Mode selected = []() {
                const char *e = std::getenv("TF_N5");
                if (e && *e == '0')
                    return Mode::Off;
                if (e && std::strcmp(e, "check") == 0)
                    return Mode::Check;
                return Mode::On;
            }();
            return selected;
        }

        // Whether a transfer may go lite now: the native renderer draws alone, the game's frames are on (not the warp
        // tunnel or a loader screen of its own: their VU1 programs are the picture's - nr_warp.cpp, outsideFrames).
        bool liteWanted() { return ps2_host_display::gsNativeAlone() && !nr::outsideFrames() && !nr::warp::active(); }

        bool litePredicate()
        {
            static bool before = false;
            const bool now = liteWanted();
            if (now != before)
            {
                before = now;
                static int told = 0;
                if (told++ < 12)
                    std::fprintf(stderr, "[n5] VIF1 %s (TF_N5)\n", now ? "lite: no UNPACK into VU1 memory, no programs started" : "as before");
            }
            return now;
        }

        // The scan of each transfer on the EE's thread (setLiteScan, PS2Recomp patch 0005's): while lite, its walk of the
        // GIF data (only for the readbacks ahead of the queue) is left out, and the rest - the walk of the codes for the
        // interrupts and MARK, ~0.2-0.8 ms of the game's thread a frame in Deep Amazon - is done on the VIF1 thread as the
        // transfer comes up. TF_N5_SCAN=ee: the scan stays on the EE's thread (the GIF walk left out); =0: as before.
        int scanMode() // (the answer while lite: 3 on the VIF1 thread, 1 on the EE's, 0 as before)
        {
            static const int selected = []() {
                const char *e = std::getenv("TF_N5_SCAN");
                if (e && *e == '0')
                    return 0;
                if (e && std::strcmp(e, "ee") == 0)
                    return 1;
                return 3;
            }();
            return selected;
        }
        bool scanLiteAllowed() { return scanMode() != 0; }
        int liteScanOn() { return liteWanted() ? scanMode() : 0; }
        int liteScanCheck() { return liteWanted() ? 2 : 0; } // (walked as before; the readbacks met then are counted)

        // =check: VU1 data memory as lite transfers would have left it - the EE's writes only - beside the real one.
        struct Check
        {
            std::mutex mutex;
            const uint8_t *vu1 = nullptr;
            std::vector<uint8_t> lite = std::vector<uint8_t>(0x4000u, 0u);
            bool tracking = false;
            uint32_t pendingOffset = 0u, pendingBytes = 0u; // (an EE store seen, its bytes copied at the next access)
            uint64_t loads = 0u, stores = 0u, differing = 0u, differingBytes = 0u;
            int told = 0;
            std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
        } g_check;

        // (=check) On the VIF1 thread, as each transfer starts: where lite transfers would begin, both worlds are alike
        // (the VU1 data memory copied as it is then); where they would end, nothing more to compare. Never lite itself.
        bool checkPredicate()
        {
            Check &c = g_check;
            const bool want = liteWanted();
            std::lock_guard<std::mutex> lock(c.mutex);
            if (want != c.tracking)
            {
                c.tracking = want;
                c.pendingBytes = 0u;
                if (want && c.vu1)
                    std::memcpy(c.lite.data(), c.vu1, 0x4000u);
            }
            return false;
        }

        void observe(uint32_t offset, uint32_t bytes, bool write)
        {
            Check &c = g_check;
            std::lock_guard<std::mutex> lock(c.mutex);
            if (!c.vu1 || offset >= 0x4000u)
                return;
            bytes = std::min<uint32_t>(bytes, 0x4000u - offset);
            if (c.pendingBytes)
            {
                std::memcpy(c.lite.data() + c.pendingOffset, c.vu1 + c.pendingOffset, c.pendingBytes);
                c.pendingBytes = 0u;
            }
            if (!c.tracking)
                return;
            if (write)
            {
                ++c.stores;
                c.pendingOffset = offset;
                c.pendingBytes = bytes;
            }
            else
            {
                ++c.loads;
                uint32_t diff = 0u;
                for (uint32_t k = 0; k < bytes; ++k)
                    diff += c.lite[offset + k] != c.vu1[offset + k] ? 1u : 0u;
                if (diff)
                {
                    ++c.differing;
                    c.differingBytes += diff;
                    if (c.told < 40)
                    {
#if defined(_WIN32)
                        if (c.told < 4)
                            tfLogCallStack("n5:check reader"); // (who reads it: the guest functions on the stack)
#endif
                        ++c.told;
                        std::fprintf(stderr, "[n5:check] the EE reads VU1 data 0x%08x (%u bytes): %u bytes there would differ with lite transfers\n",
                                     0x1100C000u + offset, bytes, diff);
                    }
                }
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - c.last >= std::chrono::seconds(10))
            {
                c.last = now;
                std::fprintf(stderr, "[n5:check] 10 s: the EE's VU1 data accesses while lite would be on: %llu loads, %llu stores; %llu loads of bytes that would differ (%llu bytes)\n",
                             static_cast<unsigned long long>(c.loads), static_cast<unsigned long long>(c.stores),
                             static_cast<unsigned long long>(c.differing), static_cast<unsigned long long>(c.differingBytes));
                c.loads = c.stores = c.differing = c.differingBytes = 0u;
            }
        }
    }

    // ---- N5.3: the device's functions native ----
    namespace
    {
        constexpr const char *kDeviceNames[] = {"setgsregister", "applystateblock", "usetexture", "renderenvironment"};
        constexpr int kDeviceCount = static_cast<int>(Device::Count);

        // TF_N5_DEVICE: 0 off, 1 on (the default), 2 =check.
        int deviceMode()
        {
            static const int mode = [] {
                const char *e = std::getenv("TF_N5_DEVICE");
                if (e && *e == '0')
                    return 0;
                if (e && std::strcmp(e, "check") == 0)
                    return 2;
                return 1;
            }();
            return mode;
        }

        bool deviceOff(Device f)
        {
            static const std::array<bool, kDeviceCount> off = [] {
                std::array<bool, kDeviceCount> o{};
                const char *e = std::getenv("TF_N5_DEVICE_OFF");
                for (int i = 0; e && i < kDeviceCount; ++i)
                    o[static_cast<size_t>(i)] = std::strstr(e, kDeviceNames[i]) != nullptr;
                return o;
            }();
            return off[static_cast<size_t>(f)];
        }

        struct DeviceCheck
        {
            std::mutex mutex;
            uint64_t calls[kDeviceCount] = {}, differing[kDeviceCount] = {};
            int told = 0;
            std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
        } g_deviceCheck;

        void deviceReport()
        {
            DeviceCheck &c = g_deviceCheck;
            const auto now = std::chrono::steady_clock::now();
            if (now - c.last < std::chrono::seconds(10))
                return;
            c.last = now;
            std::string line;
            for (int i = 0; i < kDeviceCount; ++i)
                if (c.calls[i])
                {
                    char e[96];
                    std::snprintf(e, sizeof(e), "%s%s %llu calls, %llu differing", line.empty() ? "" : "; ", kDeviceNames[i],
                                  static_cast<unsigned long long>(c.calls[i]), static_cast<unsigned long long>(c.differing[i]));
                    line += e;
                    c.calls[i] = c.differing[i] = 0u;
                }
            if (!line.empty())
                std::fprintf(stderr, "[n5:device] 10 s, native against the original: %s\n", line.c_str());
        }
    }

    namespace
    {
        // ---- vlTextureChange::UseTexture (0x0023C340; a0 the TEX register cache, a1 the texture, a2 the context 0/1) ----
        // Native: TEX0 (6 + context), MIPTBP1/2 (0x34 + context) and, when the alpha expansion changes, TEXA (0x3B) - as
        // the original computes them (its helpers too: vlTexturePS2::GetGSFormat 0x00267C90, the TEX0 maker 0x00267D30,
        // the levels 0x00267C80 / 0x00267CE0, its virtuals 0x28 and 0x34 - the palette count, the active palette) - into
        // the GS shadow (ReportGSRegisterChange's) and to the renderer, the cache's state as the original leaves it; no
        // packet. Only for vlTexturePS2's own vtable (0x006B4760): any other texture class goes to the original.
        constexpr uint32_t kUseTexture = 0x0023C340u;
        constexpr uint32_t kTextureVtable = 0x006B4760u;
        constexpr uint32_t kGsShadow = 0x70000060u;
        PS2Runtime::RecompiledFunction g_useTexture = nullptr;

        uint64_t sext32(uint32_t v) { return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v))); }
        uint32_t lhu(const uint8_t *rdram, uint32_t addr) { return (word(rdram, addr & ~3u) >> (8u * (addr & 2u))) & 0xFFFFu; }
        uint32_t clampedLevel(const uint8_t *rdram, uint32_t tex, int32_t i) // (levels[i], i clamped below the count)
        {
            const int32_t count = static_cast<int32_t>(word(rdram, tex + 0x94u));
            if (!(i < count))
                i = count - 1;
            return word(rdram, tex + 0x98u + 4u * static_cast<uint32_t>(i));
        }
        int32_t gsFormat(const uint8_t *rdram, uint32_t tex) // 0x00267C90
        {
            const uint32_t info = word(rdram, clampedLevel(rdram, tex, static_cast<int32_t>(word(rdram, tex + 0x90u))) + 0x24u);
            return info ? static_cast<int32_t>(lhu(rdram, info + 2u)) : -1;
        }
        uint32_t mipBlock(const uint8_t *rdram, uint32_t tex, int32_t n) // 0x00267CE0: a mip's VRAM block
        {
            const uint32_t info = word(rdram, clampedLevel(rdram, tex, static_cast<int32_t>(word(rdram, tex + 0x90u)) + n) + 0x24u);
            return lhu(rdram, info) + word(rdram, tex + 0x58u + 4u * static_cast<uint32_t>(n));
        }
        uint64_t makeTex0(const uint8_t *rdram, uint32_t tex, uint32_t tfx) // 0x00267D30
        {
            const int32_t v1 = static_cast<int32_t>(word(rdram, tex + 0x90u)), v0 = static_cast<int32_t>(word(rdram, tex + 0x94u));
            auto formatAt = [&]() {
                const uint32_t info = word(rdram, clampedLevel(rdram, tex, v1) + 0x24u);
                return info ? static_cast<int32_t>(lhu(rdram, info + 2u)) : -1;
            };
            const int32_t psm = formatAt();
            const uint32_t rowWidth = word(rdram, word(rdram, tex + 0x98u + 4u * static_cast<uint32_t>(v1)) + 0x18u); // (levels[v1], not clamped)
            int32_t tbw;
            if (psm == 0x13 || psm == 0x14) // T8 / T4: a 128-pixel column pair
            {
                const int32_t a2 = static_cast<int32_t>(rowWidth) + 0x7F;
                tbw = ((a2 < 0 ? a2 + 0x7F : a2) >> 7) << 1;
            }
            else
            {
                const int32_t a0 = static_cast<int32_t>(rowWidth) + 0x3F;
                tbw = (a0 < 0 ? a0 + 0x3F : a0) >> 6;
            }
            const uint32_t tw = word(rdram, tex + 0x48u), th = word(rdram, tex + 0x4Cu);
            const uint32_t tbp = lhu(rdram, word(rdram, clampedLevel(rdram, tex, v1) + 0x24u)) + word(rdram, tex + 0x58u);
            const int32_t psm2 = formatAt();
            uint64_t r = (sext32(static_cast<uint32_t>(psm2)) << 20) | (sext32(static_cast<uint32_t>(tbw)) << 14) | sext32(tbp);
            r |= sext32(tw) << 26;
            r |= sext32(th) << 30;
            r |= 4ull << 32; // TCC
            r |= sext32(tfx) << 35;
            const uint32_t flags = word(rdram, tex + 0x30u);
            if (flags & 0x10u) // indexed: the CLUT
            {
                const uint32_t csaFlag = (flags & 0x20u) ? 1u : 0u;
                const uint32_t active = word(rdram, tex + 0xE0u); // (virtual 0x34: the active palette)
                uint32_t cbp = 0u;
                if (word(rdram, tex + 0x30u) & 0x10u)
                    cbp = lhu(rdram, word(rdram, word(rdram, tex + 0x98u + 4u * active) + 0x24u));
                r |= (sext32(cbp) << 37) | (sext32(csaFlag * 15u) << 56) | (0x20000000ull << 32);
            }
            return r;
        }

        struct UseTextureResult
        {
            uint32_t count = 0;
            uint32_t reg[3] = {};
            uint64_t value[3] = {};
            uint32_t state = 0; // the cache's alpha-expansion state (+0) after
            uint32_t cycles = 0; // the EE cycles the original's path takes (deviceReturn)
        };

        bool computeUseTexture(const uint8_t *rdram, uint32_t self, uint32_t tex, uint32_t context, UseTextureResult &out)
        {
            if (!tex || word(rdram, tex) != kTextureVtable)
                return false;
            uint32_t state = word(rdram, self);
            int32_t mips = static_cast<int32_t>(word(rdram, tex + 0x54u)) - 1;
            const int32_t format = gsFormat(rdram, tex);
            bool texa = false;
            uint32_t ta0 = 0u;
            if (format == 2 || format == 10)
            {
                if (state == 1u)
                    texa = true, state = 0u, ta0 = 0u;
            }
            else if (format == 1 && state == 0u)
                texa = true, state = 1u, ta0 = 0x80u;
            out.count = 0;
            out.reg[out.count] = 6u + context, out.value[out.count++] = makeTex0(rdram, tex, word(rdram, self + 4u * context + 0x10u));
            uint64_t miptbp = 0u;
            if (mips != 0)
            {
                if (!(mips < 4))
                    mips = 3;
                for (int32_t level = 1; !(mips < level); ++level)
                {
                    const uint32_t palettes = word(rdram, tex + 0x90u); // (virtual 0x28: the palette count)
                    const uint32_t mip = word(rdram, tex + 0x98u + 4u * (static_cast<uint32_t>(level) + palettes));
                    const int32_t rw = static_cast<int32_t>(word(rdram, mip + 0x18u));
                    int64_t tbw = (rw < 0 ? rw + 0x3F : rw) >> 6;
                    if (tbw == 0)
                        tbw = 1;
                    const uint64_t entry = (static_cast<uint64_t>(tbw) << 14) | sext32(mipBlock(rdram, tex, level));
                    miptbp |= entry << ((static_cast<uint32_t>(level - 1) * 20u) & 63u);
                }
            }
            out.reg[out.count] = 0x34u + context, out.value[out.count++] = miptbp;
            if (texa)
                out.reg[out.count] = 0x3Bu, out.value[out.count++] = static_cast<uint64_t>(ta0) | (0x80ull << 32);
            out.state = state;
            // (the original's safe points: its calls - GetGSFormat, the TEX0 maker with its palette virtual for a palette
            // texture, the shadow stores - and per mip level the loop's back edge, the palette-count virtual and the two
            // level helpers; measured against the original in TF_N5_DEVICE=check)
            const uint32_t levels = mips > 0 ? static_cast<uint32_t>(mips) : 0u;
            out.cycles = 32u + ((word(rdram, tex + 0x30u) & 0x10u) ? 8u : 0u) + 56u * levels + (texa ? 8u : 0u);
            return true;
        }

        void store32(uint8_t *rdram, uint32_t addr, uint32_t value)
        {
            if (uint8_t *at = getMemPtr(rdram, addr))
                std::memcpy(at, &value, 4u);
        }
        void store64(uint8_t *rdram, uint32_t addr, uint64_t value)
        {
            if (uint8_t *at = getMemPtr(rdram, addr))
                std::memcpy(at, &value, 8u);
        }
        uint64_t load64(const uint8_t *rdram, uint32_t addr)
        {
            return static_cast<uint64_t>(word(rdram, addr)) | static_cast<uint64_t>(word(rdram, addr + 4u)) << 32;
        }

        void useTexture(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t self = getRegU32(ctx, 4), tex = getRegU32(ctx, 5), context = getRegU32(ctx, 6) & 1u;
            UseTextureResult r;
            if (!nr::framesDeferred() || !deviceNative(Device::UseTexture) || !computeUseTexture(rdram, self, tex, context, r))
            {
                g_useTexture(rdram, ctx, runtime);
                return;
            }
            if (deviceCheck())
            {
                // The original first - into the list, its state and shadow writes kept to compare - then all of it put back.
                const uint32_t pointer = word(rdram, kPacketPointer);
                const uint32_t savedState = word(rdram, self), savedTex = word(rdram, self + 4u * context + 4u);
                const uint64_t saved[3] = {load64(rdram, kGsShadow + 8u * (6u + context)), load64(rdram, kGsShadow + 8u * (0x34u + context)),
                                           load64(rdram, kGsShadow + 8u * 0x3Bu)};
                const uint32_t ret = getRegU32(ctx, 31);
                const uint32_t count0 = ctx->cop0_count;
                g_useTexture(rdram, ctx, runtime);
                const uint32_t spent = ctx->cop0_count - count0; // (the EE clock the original moved: deviceReturn's charge)
                if (ctx->pc != ret)
                {
                    // (It stopped part-way - a VBlank due at one of its calls, TODO 118: entered again later where it stopped,
                    // it finishes on its own. This call is the original's, not compared.)
                    static int told = 4;
                    if (told > 0)
                    {
                        --told;
                        std::fprintf(stderr, "[n5:device] usetexture: the original stopped part-way - this call left to it\n");
                    }
                    return;
                }
                const uint32_t after = word(rdram, kPacketPointer);
                if (after != pointer + 32u + 16u * r.count)
                    deviceMismatch(Device::UseTexture, "bytes written", 32u + 16u * r.count, after - pointer);
                else
                    for (uint32_t i = 0; i < r.count; ++i)
                    {
                        const uint32_t at = pointer + 32u + 16u * i;
                        if ((word(rdram, at + 8u) & 0x7Fu) != r.reg[i])
                            deviceMismatch(Device::UseTexture, "a register", r.reg[i], word(rdram, at + 8u));
                        else if (load64(rdram, at) != r.value[i])
                            deviceMismatch(Device::UseTexture, r.reg[i] == 0x3Bu ? "TEXA" : r.reg[i] >= 0x34u ? "MIPTBP" : "TEX0", r.value[i], load64(rdram, at));
                        else if (load64(rdram, kGsShadow + 8u * r.reg[i]) != r.value[i])
                            deviceMismatch(Device::UseTexture, "the shadow", r.value[i], load64(rdram, kGsShadow + 8u * r.reg[i]));
                    }
                if (word(rdram, self) != r.state)
                    deviceMismatch(Device::UseTexture, "the cache's state", r.state, word(rdram, self));
                if (!deviceCyclesMatch(runtime, r.cycles, spent))
                    deviceMismatch(Device::UseTexture, "EE cycles", r.cycles, spent);
                store32(rdram, kPacketPointer, pointer);
                store32(rdram, self, savedState);
                store32(rdram, self + 4u * context + 4u, savedTex);
                store64(rdram, kGsShadow + 8u * (6u + context), saved[0]);
                store64(rdram, kGsShadow + 8u * (0x34u + context), saved[1]);
                store64(rdram, kGsShadow + 8u * 0x3Bu, saved[2]);
                deviceChecked(Device::UseTexture);
            }
            store32(rdram, self + 4u * context + 4u, tex);
            store32(rdram, self, r.state);
            for (uint32_t i = 0; i < r.count; ++i)
            {
                store64(rdram, kGsShadow + 8u * r.reg[i], r.value[i]);
                nr::deviceRegister(rdram, r.reg[i], r.value[i]);
            }
            deviceReturn(ctx, runtime, deviceCheck() ? 0u : r.cycles); // (checked: the original ran, its cycles counted)
        }

        // ---- The render-state slots (vlRenderDevicePS2 slot 48 GetRenderState 0x00257F80, slot 44 SetRenderState
        // 0x00258690) - as far as SetHardwareRenderEnvironment uses them: the state table D_70000B80 (8 bytes a state, a
        // word a context when the state's byte in D_70000D10 says it is per context - D_70000680 the current context) and,
        // for state 0x10 (the GS context), the context's switch: D_70000680, and when that context holds a special texture
        // (the device's byte +0x40 + context) and is not the one last switched to (+0x68), vlTextureChange::ContextChange
        // (0x0023C660) - the context's TEX0 written again from the shadow (SetGSRegister's work: native here). ----
        constexpr uint32_t kGetRenderState = 0x00257F80u, kSetRenderState = 0x00258690u;
        constexpr uint32_t kRenderStates = 0x70000B80u, kPerContext = 0x70000D10u, kContext = 0x70000680u;
        uint32_t byteAt(const uint8_t *rdram, uint32_t addr) { return (word(rdram, addr & ~3u) >> (8u * (addr & 3u))) & 0xFFu; }
        uint32_t renderStateAt(const uint8_t *rdram, uint32_t state)
        {
            const uint32_t context = byteAt(rdram, kPerContext + state) ? word(rdram, kContext) : 0u;
            return kRenderStates + 8u * state + 4u * context;
        }
        uint32_t getRenderState(const uint8_t *rdram, uint32_t state) { return word(rdram, renderStateAt(rdram, state)); }

        constexpr int kEnvRegs = 11;
        struct EnvRegisters
        {
            uint32_t reg[kEnvRegs];
            uint64_t value[kEnvRegs];
            uint32_t switches = 0; // (the context switches' TEX0s - ContextChange's SetGSRegister)
            uint32_t switchReg[3];
            uint64_t switchValue[3];
        };

        void setContextState(uint8_t *rdram, uint32_t device, uint32_t context, EnvRegisters &out) // SetRenderState (device, 0x10, context)
        {
            const uint32_t at = renderStateAt(rdram, 0x10u);
            const uint32_t was = word(rdram, at);
            store32(rdram, at, context);
            if (was == context)
                return;
            store32(rdram, kContext, context);
            if (byteAt(rdram, device + 0x40u + context) == 0u || word(rdram, device + 0x68u) == context)
                return;
            store32(rdram, device + 0x68u, context);
            static int told = 4;
            if (told > 0)
            {
                --told;
                std::fprintf(stderr, "[n5:device] renderenvironment: a GS context switch with a special texture - TEX0_%u again from the shadow\n", context + 1u);
            }
            const uint32_t reg = 6u + context;
            const uint64_t tex0 = load64(rdram, kGsShadow + 8u * reg);
            nr::deviceRegister(rdram, reg, tex0);
            if (out.switches < 3u)
                out.switchReg[out.switches] = reg, out.switchValue[out.switches++] = tex0;
        }

        // ---- SetHardwareRenderEnvironment (0x00264DB0; a0 the environment, or null: back to the one saved) ----
        // Native: the environment made the current one (0x00713540: 16 bytes, then per context 128 at 0x00713550 /
        // 0x007135D0 - the one replaced saved at 0x00713650 / 0x00713660 / 0x007136E0, which a null environment puts back)
        // and its registers - FRAME_1/2 (their FBMSK from render state 0xF, of each context: the GS context switched to 0,
        // 1 and back for it), ZBUF_1/2, XYOFFSET_1/2 (OFX, OFY from the floats at gp-0x4204/-0x4200, + 8, to integers),
        // SCISSOR_1/2, PRMODE 0x48, PRMODECONT (the byte at 0x006F853C), TEXFLUSH - into the shadow (not TEXFLUSH) and to
        // the renderer; no packet (the original's: a DMA CNT + VIF DIRECT of a GIF A+D packet of the 11). D_70000040 /
        // D_70000050 (the packet's two header words' places) set as the original leaves them.
        constexpr uint32_t kRenderEnvironment = 0x00264DB0u;
        constexpr uint32_t kEnvCurrent = 0x00713540u, kEnvSaved = 0x00713650u, kEnvBytes = 0x110u; // (16 + 128 + 128)
        constexpr uint32_t kHeaderTag = 0x70000040u, kHeaderGif = 0x70000050u;
        constexpr uint32_t kPrmodeCont = 0x006F853Cu;
        PS2Runtime::RecompiledFunction g_renderEnvironment = nullptr;

        void copyWords(uint8_t *rdram, uint32_t to, uint32_t from, uint32_t bytes)
        {
            for (uint32_t i = 0; i < bytes; i += 4u)
                store32(rdram, to + i, word(rdram, from + i));
        }

        // The device the environment's render-state calls go to (*(gp - 0x4760)) - when it is vlRenderDevicePS2 with its
        // own slots there (else: the original).
        uint32_t environmentDevice(const uint8_t *rdram, uint32_t gp)
        {
            const uint32_t device = word(rdram, gp - 0x4760u);
            const uint32_t vtable = device ? word(rdram, device) : 0u;
            if (!vtable || word(rdram, vtable + 0xC8u) != kGetRenderState || word(rdram, vtable + 0xB8u) != kSetRenderState)
                return 0u;
            return device;
        }

        void renderEnvironmentNative(uint8_t *rdram, uint32_t env, uint32_t device, uint32_t gp, EnvRegisters &out)
        {
            uint32_t s1 = env;
            if (env)
            {
                copyWords(rdram, kEnvSaved, kEnvCurrent, kEnvBytes);
                copyWords(rdram, kEnvCurrent, env, kEnvBytes);
            }
            else
            {
                s1 = kEnvSaved;
                copyWords(rdram, kEnvCurrent, kEnvSaved, kEnvBytes);
            }
            const uint32_t pointer = word(rdram, kPacketPointer);
            store32(rdram, kHeaderTag, pointer);
            store32(rdram, kHeaderGif, pointer + 0x10u);
            uint64_t frame1 = load64(rdram, s1 + 0x10u), frame2 = load64(rdram, s1 + 0x90u);
            const uint32_t context = getRenderState(rdram, 0x10u);
            setContextState(rdram, device, 0u, out);
            frame1 = (frame1 & 0xFFFFFFFFull) | (static_cast<uint64_t>(~getRenderState(rdram, 0xFu)) << 32);
            setContextState(rdram, device, 1u, out);
            frame2 = (frame2 & 0xFFFFFFFFull) | (static_cast<uint64_t>(~getRenderState(rdram, 0xFu)) << 32);
            setContextState(rdram, device, context, out);
            float fx, fy;
            const uint32_t wx = word(rdram, gp - 0x4204u), wy = word(rdram, gp - 0x4200u);
            std::memcpy(&fx, &wx, 4u);
            std::memcpy(&fy, &wy, 4u);
            const uint64_t ofx = static_cast<uint16_t>(FPU_CVT_W_S(FPU_ADD_S(8.0f, fx)));
            const uint64_t ofy = static_cast<uint16_t>(FPU_CVT_W_S(FPU_ADD_S(8.0f, fy)));
            constexpr uint64_t kOffsets = 0x0000FFFF0000FFFFull;
            const uint64_t xy1 = (load64(rdram, s1 + 0x30u) & ~kOffsets) | ofx | (ofy << 32);
            const uint64_t xy2 = (load64(rdram, s1 + 0xB0u) & ~kOffsets) | ofx | (ofy << 32);
            const uint64_t values[kEnvRegs] = {frame1, load64(rdram, s1 + 0x20u), xy1, load64(rdram, s1 + 0x40u),
                                               frame2, load64(rdram, s1 + 0xA0u), xy2, load64(rdram, s1 + 0xC0u),
                                               0x48u, byteAt(rdram, kPrmodeCont), 0u};
            constexpr uint32_t regs[kEnvRegs] = {0x4Cu, 0x4Eu, 0x18u, 0x40u, 0x4Du, 0x4Fu, 0x19u, 0x41u, 0x1Bu, 0x1Au, 0x3Fu};
            for (int i = 0; i < kEnvRegs; ++i)
            {
                out.reg[i] = regs[i], out.value[i] = values[i];
                if (regs[i] != 0x3Fu) // (TEXFLUSH: not kept in the shadow)
                    store64(rdram, kGsShadow + 8u * regs[i], values[i]);
                nr::deviceRegister(rdram, regs[i], values[i]);
            }
        }

        uint32_t environmentCycles(uint32_t env, const EnvRegisters &r)
        {
            return (env ? 896u : 448u) + 6u * 8u + 8u * 24u + 2u * 8u + 8u * r.switches;
        }

        void renderEnvironment(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t env = getRegU32(ctx, 4), gp = getRegU32(ctx, 28);
            const uint32_t device = nr::framesDeferred() && deviceNative(Device::RenderEnvironment) ? environmentDevice(rdram, gp) : 0u;
            if (!device)
            {
                g_renderEnvironment(rdram, ctx, runtime);
                return;
            }
            const uint32_t pointer = word(rdram, kPacketPointer);
            if (deviceCheck())
            {
                // The original first - into the list - with everything it changes kept to compare, then put back; then the
                // native one, compared against it.
                std::array<uint32_t, 2u * kEnvBytes / 4u> env0{}, envOriginal{};
                std::array<uint32_t, 0x100u> shadow0{}, shadowOriginal{}; // (the GS shadow: 128 registers)
                auto keep = [&](std::array<uint32_t, 2u * kEnvBytes / 4u> &e, std::array<uint32_t, 0x100u> &s) {
                    for (uint32_t i = 0; i < e.size(); ++i)
                        e[i] = word(rdram, kEnvCurrent + 4u * i);
                    for (uint32_t i = 0; i < s.size(); ++i)
                        s[i] = word(rdram, kGsShadow + 4u * i);
                };
                keep(env0, shadow0);
                const uint32_t states0[2] = {word(rdram, kRenderStates + 0x80u), word(rdram, kRenderStates + 0x84u)};
                const uint32_t context0 = word(rdram, kContext), switched0 = word(rdram, device + 0x68u);
                const uint32_t header0[2] = {word(rdram, kHeaderTag), word(rdram, kHeaderGif)};
                const uint32_t ret = getRegU32(ctx, 31);
                const uint32_t count0 = ctx->cop0_count;
                g_renderEnvironment(rdram, ctx, runtime);
                const uint32_t spent = ctx->cop0_count - count0;
                if (ctx->pc != ret)
                {
                    static int told = 4;
                    if (told > 0)
                    {
                        --told;
                        std::fprintf(stderr, "[n5:device] renderenvironment: the original stopped part-way - this call left to it\n");
                    }
                    return;
                }
                keep(envOriginal, shadowOriginal);
                const uint32_t statesOriginal[2] = {word(rdram, kRenderStates + 0x80u), word(rdram, kRenderStates + 0x84u)};
                const uint32_t contextOriginal = word(rdram, kContext), switchedOriginal = word(rdram, device + 0x68u);
                const uint32_t headerOriginal[2] = {word(rdram, kHeaderTag), word(rdram, kHeaderGif)};
                const uint32_t after = word(rdram, kPacketPointer);
                std::array<uint64_t, 2u * (kEnvRegs + 2 + 9)> list{};
                const uint32_t qwords = (after - pointer) / 16u;
                const bool fits = after > pointer && after - pointer <= 16u * (kEnvRegs + 2u + 9u);
                if (fits)
                    for (uint32_t i = 0; i < 2u * qwords; ++i)
                        list[i] = load64(rdram, pointer + 8u * i);
                // (put back)
                for (uint32_t i = 0; i < env0.size(); ++i)
                    store32(rdram, kEnvCurrent + 4u * i, env0[i]);
                for (uint32_t i = 0; i < shadow0.size(); ++i)
                    store32(rdram, kGsShadow + 4u * i, shadow0[i]);
                store32(rdram, kRenderStates + 0x80u, states0[0]);
                store32(rdram, kRenderStates + 0x84u, states0[1]);
                store32(rdram, kContext, context0);
                store32(rdram, device + 0x68u, switched0);
                store32(rdram, kHeaderTag, header0[0]);
                store32(rdram, kHeaderGif, header0[1]);
                store32(rdram, kPacketPointer, pointer);
                EnvRegisters r;
                renderEnvironmentNative(rdram, env, device, gp, r);
                const uint32_t inserted = 3u * r.switches; // (the context switches' packets - DMA CNT, GIF tag, A+D)
                if (!fits || qwords != kEnvRegs + 2u + inserted)
                    deviceMismatch(Device::RenderEnvironment, "bytes written", 16u * (kEnvRegs + 2u + inserted), after - pointer);
                else
                {
                    for (uint32_t k = 0; k < r.switches; ++k)
                    {
                        const uint32_t at = 2u * (2u + 3u * k + 2u); // (the switch packet's A+D qword)
                        if ((list[at + 1u] & 0x7Fu) != r.switchReg[k] || list[at] != r.switchValue[k])
                            deviceMismatch(Device::RenderEnvironment, "a context switch's TEX0", r.switchValue[k], list[at]);
                    }
                    if (inserted)
                    {
                        // (the A+D after them: compared as if they were not there)
                        for (uint32_t i = 4u; i + 6u * r.switches < 2u * qwords; ++i)
                            list[i] = list[i + 6u * r.switches];
                        static int told = 4;
                        if (told > 0)
                        {
                            --told;
                            std::fprintf(stderr, "[n5:device] renderenvironment: the original's DIRECT holds %u context switch packet(s) - "
                                                 "the GS takes their DMA tag and GIF tag as A+D writes (PRIM); native: the TEX0s only\n", r.switches);
                        }
                    }
                    const uint64_t n = kEnvRegs + 1u + inserted; // (the GIF tag, and the A+D qwords - with the switches' packets)
                    if (list[0] != (n | 0x10000000ull) || list[1] != ((((n | 0x50000000ull) << 32)) | 0x13000000ull))
                        deviceMismatch(Device::RenderEnvironment, "the DMA tag / VIF code", n | 0x10000000ull, list[0]);
                    else if (list[2] != ((n - 1u) | 0x8000ull | (0x10000000ull << 32)) || list[3] != 0xEu)
                        deviceMismatch(Device::RenderEnvironment, "the GIF tag", (n - 1u) | 0x8000ull | (0x10000000ull << 32), list[2]);
                    for (int i = 0; i < kEnvRegs; ++i)
                    {
                        if ((list[5u + 2u * i] & 0x7Fu) != r.reg[i])
                            deviceMismatch(Device::RenderEnvironment, "a register", r.reg[i], list[5u + 2u * i]);
                        else if (list[4u + 2u * i] != r.value[i])
                            deviceMismatch(Device::RenderEnvironment, r.reg[i] >= 0x4Cu ? "FRAME / ZBUF" : r.reg[i] >= 0x40u ? "SCISSOR" : r.reg[i] >= 0x1Au ? "PRMODE(CONT) / TEXFLUSH" : "XYOFFSET",
                                           r.value[i], list[4u + 2u * i]);
                    }
                }
                std::array<uint32_t, 2u * kEnvBytes / 4u> envNative{};
                std::array<uint32_t, 0x100u> shadowNative{};
                keep(envNative, shadowNative);
                for (uint32_t i = 0; i < envNative.size(); ++i)
                    if (envNative[i] != envOriginal[i])
                    {
                        deviceMismatch(Device::RenderEnvironment, "the environments' copies", kEnvCurrent + 4u * i, envOriginal[i]);
                        break;
                    }
                for (uint32_t i = 0; i < shadowNative.size(); ++i)
                    if (shadowNative[i] != shadowOriginal[i])
                    {
                        deviceMismatch(Device::RenderEnvironment, "the shadow (register)", i / 2u, shadowOriginal[i]);
                        break;
                    }
                if (word(rdram, kRenderStates + 0x80u) != statesOriginal[0] || word(rdram, kRenderStates + 0x84u) != statesOriginal[1] ||
                    word(rdram, kContext) != contextOriginal || word(rdram, device + 0x68u) != switchedOriginal)
                    deviceMismatch(Device::RenderEnvironment, "the GS context's state", word(rdram, kContext), contextOriginal);
                if (word(rdram, kHeaderTag) != headerOriginal[0] || word(rdram, kHeaderGif) != headerOriginal[1])
                    deviceMismatch(Device::RenderEnvironment, "D_70000040 / 50", word(rdram, kHeaderTag), headerOriginal[0]);
                if (const uint32_t predicted = environmentCycles(env, r); !deviceCyclesMatch(runtime, predicted, spent))
                    deviceMismatch(Device::RenderEnvironment, "EE cycles", predicted, spent);
                deviceChecked(Device::RenderEnvironment);
            }
            else
            {
                EnvRegisters r;
                renderEnvironmentNative(rdram, env, device, gp, r);
                static const bool pad = std::getenv("TF_N5_ENV_PAD") && *std::getenv("TF_N5_ENV_PAD") == '1';
                if (pad)
                {
                    // (debug: the list as long as the original's - a DMA CNT of 12 qwords of VIF NOPs where its packet was)
                    store64(rdram, pointer, (kEnvRegs + 1u) | 0x10000000ull);
                    store64(rdram, pointer + 8u, 0u);
                    for (uint32_t i = 1; i <= kEnvRegs + 1u; ++i)
                        store64(rdram, pointer + 16u * i, 0u), store64(rdram, pointer + 16u * i + 8u, 0u);
                    store32(rdram, kPacketPointer, pointer + 16u * (kEnvRegs + 2u));
                }
                // (the original's safe points: its four copy loops - two for a null environment - 7 back edges each, its 6
                // render-state calls, the 8 register helpers with their two calls each, the two shadow stores, and per
                // context switch ContextChange's call - its tail jump to SetGSRegister is free, as measured)
                deviceReturn(ctx, runtime, environmentCycles(env, r));
                return;
            }
            ctx->pc = getRegU32(ctx, 31); // (returned: the caller's dispatch checks pc came back to it)
        }
    }

    bool deviceNative(Device f)
    {
        return deviceMode() != 0 && !deviceOff(f) && liteWanted();
    }

    bool deviceCheck() { return deviceMode() == 2; }

    bool deviceCyclesMatch(PS2Runtime *runtime, uint32_t charged, uint32_t spent)
    {
        const EeScheduler *s = runtime ? runtime->eeSchedulerFast() : nullptr;
        const uint32_t scale = s ? s->currentCycleScale256() : 256u;
        const uint64_t expected = scale < 256u ? (static_cast<uint64_t>(charged) * scale) >> 8 : charged;
        return spent + 1u >= expected && spent <= expected + 1u;
    }

    void deviceReturn(R5900Context *ctx, PS2Runtime *runtime, uint32_t cycles)
    {
        static const bool charge = !(std::getenv("TF_N5_CYCLES") && *std::getenv("TF_N5_CYCLES") == '0');
        ctx->pc = getRegU32(ctx, 31); // (returned: the caller's dispatch checks pc came back to it)
        if (charge && cycles && runtime)
            (void)runtime->eeCheckpointDue(cycles);
    }

    void deviceChecked(Device f)
    {
        std::lock_guard<std::mutex> lock(g_deviceCheck.mutex);
        ++g_deviceCheck.calls[static_cast<int>(f)];
        deviceReport();
    }

    void deviceMismatch(Device f, const char *what, uint64_t native, uint64_t original)
    {
        std::lock_guard<std::mutex> lock(g_deviceCheck.mutex);
        ++g_deviceCheck.differing[static_cast<int>(f)];
        if (g_deviceCheck.told < 40)
        {
            ++g_deviceCheck.told;
            std::fprintf(stderr, "[n5:device] %s: %s - native %016llx, original %016llx\n", kDeviceNames[static_cast<int>(f)], what,
                         static_cast<unsigned long long>(native), static_cast<unsigned long long>(original));
        }
    }

    // ---- N5 step 0 / TODO 167: the GS read-backs the game asks for, and who asks ----
    // TF_N5_READBACKS=<s>: TransferRenderSurfaceToMemory 0x2556B0 (with the sun test's own surface and host copy told
    // apart - nr_occlusion.cpp answers those natively), its caller 0x255470 (the surfaces' copy-to-memory, through the
    // thunks 0x263100 / 0x263D90) and sceGsExecStoreImage 0x11E088 wrapped: per function and caller (ra) the calls and
    // their time, every <s> seconds; the first 4 StoreImages with the host stack (guest names in it).
    namespace
    {
        constexpr uint32_t kRbTransfer = 0x002556B0u, kRbSurfaceCopy = 0x00255470u, kRbStoreImage = 0x0011E088u;
        constexpr uint32_t kRbOcclusionBuffer = 0x006CEC5Cu, kRbHostCopy = 0x006CEC68u;
        PS2Runtime::RecompiledFunction g_rbTransfer = nullptr, g_rbSurfaceCopy = nullptr, g_rbStoreImage = nullptr;
        struct RbEntry
        {
            uint64_t calls = 0u;
            double ms = 0.0, maxMs = 0.0;
            uint32_t surface = 0u, host = 0u;
        };
        std::mutex g_rbMutex;
        std::map<std::tuple<uint32_t, uint32_t, int>, RbEntry> g_rb; // (function, ra, the sun's surface: 1 / host copy differs: 2 / other: 0)
        uint32_t g_rbSeconds = 0u;
        std::chrono::steady_clock::time_point g_rbLast = std::chrono::steady_clock::now();
        int g_rbStacks = 4;

        void rbReport(uint8_t *rdram, PS2Runtime *runtime)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now - g_rbLast < std::chrono::seconds(g_rbSeconds))
                return;
            g_rbLast = now;
            std::fprintf(stderr, "[n5:readbacks] tick %llu, %s; occlusion surface %08x host copy %08x:%s\n",
                         static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()),
                         ps2_host_display::emulatedDrawing() ? "emulated drawing on" : "native only", word(rdram, kRbOcclusionBuffer),
                         word(rdram, kRbHostCopy), g_rb.empty() ? " none" : "");
            for (const auto &[key, e] : g_rb)
            {
                const auto [fn, ra, kind] = key;
                std::fprintf(stderr, "[n5:readbacks]   %08x from ra %08x%s: %llu calls, %.2f ms (%.3f max), surface %08x host %08x\n", fn, ra,
                             kind == 1 ? " (the sun's)" : (kind == 2 ? " (the sun's surface, another host)" : ""),
                             static_cast<unsigned long long>(e.calls), e.ms, e.maxMs, e.surface, e.host);
            }
            g_rb.clear();
        }

        void rbCall(PS2Runtime::RecompiledFunction original, uint32_t fn, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = getRegU32(ctx, 31), surface = getRegU32(ctx, 4), host = getRegU32(ctx, 6);
            int kind = 0;
            if (fn == kRbTransfer && surface && surface == word(rdram, kRbOcclusionBuffer))
                kind = host == word(rdram, kRbHostCopy) ? 1 : 2;
            if (fn == kRbStoreImage && g_rbStacks > 0)
            {
                --g_rbStacks;
                std::fprintf(stderr, "[n5:readbacks] sceGsExecStoreImage from ra %08x (tick %llu, %s):\n", ra,
                             static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()),
                             ps2_host_display::emulatedDrawing() ? "emulated drawing on" : "native only");
#if defined(_WIN32)
                tfLogCallStack("[n5:readbacks]");
#endif
            }
            const auto start = std::chrono::steady_clock::now();
            original(rdram, ctx, runtime);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            std::lock_guard<std::mutex> lock(g_rbMutex);
            RbEntry &e = g_rb[{fn, ra, kind}];
            ++e.calls;
            e.ms += ms;
            e.maxMs = std::max(e.maxMs, ms);
            e.surface = surface;
            e.host = host;
            rbReport(rdram, runtime);
        }

        void rbTransfer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { rbCall(g_rbTransfer, kRbTransfer, rdram, ctx, runtime); }
        void rbSurfaceCopy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { rbCall(g_rbSurfaceCopy, kRbSurfaceCopy, rdram, ctx, runtime); }
        void rbStoreImage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { rbCall(g_rbStoreImage, kRbStoreImage, rdram, ctx, runtime); }

        // ---- N5 step 0: the geometry sets' packets (what RenderGeometrySet's DMA CALLs add to the kick) ----
        // TF_N5_GEOMCENSUS=<s>: RenderGeometrySet 0x260A70 wrapped; each set's packet (set + 0x14), the first time it is
        // met, followed command by command: its bytes and what it does to VU1 that is not the mesh - UNPACKs at a fixed
        // address (not TOPS-relative) and where, programs started and where, the registers it sets. Every <s> seconds:
        // the calls that added a CALL and the bytes they point the kick at a frame, and the packets' totals.
        constexpr uint32_t kRenderGeometrySet = 0x00260A70u;
        PS2Runtime::RecompiledFunction g_gcRender = nullptr;
        uint32_t g_gcSeconds = 0u;
        struct GcPacket
        {
            uint32_t bytes = 0u;
        };
        struct GcStats
        {
            std::unordered_map<uint32_t, GcPacket> packets; // (by packet address)
            uint64_t calls = 0u, added = 0u, addedBytes = 0u, frames = 0u;
            uint64_t opcodes[128] = {};
            std::map<uint32_t, uint64_t> fixedUnpacks; // (qword address / 16 -> UNPACKs writing there, by first qword)
            uint64_t topsUnpacks = 0u, unpackBytes = 0u, otherBytes = 0u, failed = 0u;
            std::map<uint32_t, uint64_t> programs; // (MSCAL / MSCALF address; ~0u: MSCNT)
            uint64_t lastTick = 0u;
            std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
        } g_gc;

        // VU1 data memory's qwords as UNPACKs write them: bit 0 a geometry set's packet (fixed address), 1 the same
        // TOPS-relative, 2 the list itself (fixed), 3 the list TOPS-relative (a TOPS-relative one marked from 0: BASE /
        // OFFSET unknown here).
        uint8_t g_cover[1024] = {};
        uint64_t g_inlineMasked = 0u, g_inlineUnpacks = 0u, g_inlineState[128] = {};

        void markUnpack(uint32_t imm, uint32_t num, uint32_t cl, uint32_t wl, uint8_t fixedBit, uint8_t topsBit)
        {
            const uint32_t addr = imm & 0x3FFu, count = num ? num : 256u;
            const uint8_t bit = (imm & 0x8000u) ? topsBit : fixedBit;
            for (uint32_t i = 0; i < count; ++i)
                g_cover[(cl >= wl ? addr + (i / wl) * cl + i % wl : addr + i) & 0x3FFu] |= bit;
        }

        void gcAnalyse(const uint8_t *rdram, uint32_t packet, GcPacket &p)
        {
            static std::vector<uint32_t> words;
            words.clear();
            if (!vif::gather(rdram, packet, words, nullptr))
            {
                ++g_gc.failed;
                return;
            }
            p.bytes = static_cast<uint32_t>(words.size() * 4u);
            uint32_t cl = 1u, wl = 1u;
            for (size_t i = 0; i < words.size();)
            {
                const uint32_t cmd = words[i];
                const uint32_t op = (cmd >> 24) & 0x7Fu, num = (cmd >> 16) & 0xFFu, imm = cmd & 0xFFFFu;
                ++g_gc.opcodes[op];
                size_t len = 1u;
                if ((op & 0x60u) == 0x60u)
                {
                    static constexpr uint8_t kVectorBytes[16] = {4, 2, 1, 2, 8, 4, 2, 4, 12, 6, 3, 6, 16, 8, 4, 2};
                    uint32_t vectors = num ? num : 256u;
                    if (cl < wl)
                        vectors = (vectors / wl) * cl + std::min(vectors % wl, cl);
                    const uint32_t bytes = (vectors * kVectorBytes[op & 0xFu] + 3u) & ~3u;
                    len += bytes / 4u;
                    g_gc.unpackBytes += bytes;
                    if ((imm & 0x8000u) != 0u)
                        ++g_gc.topsUnpacks;
                    else
                        ++g_gc.fixedUnpacks[(imm & 0x3FFu) / 16u * 16u];
                    markUnpack(imm, num, cl, wl, 1u, 2u);
                }
                else
                {
                    switch (op)
                    {
                    case 0x01u: // STCYCL
                        cl = std::max(1u, imm & 0xFFu), wl = std::max(1u, (imm >> 8) & 0xFFu);
                        break;
                    case 0x14u: // MSCAL
                    case 0x15u: // MSCALF
                        ++g_gc.programs[imm * 8u];
                        break;
                    case 0x17u: // MSCNT
                        ++g_gc.programs[~0u];
                        break;
                    case 0x20u: // STMASK
                        len += 1u;
                        break;
                    case 0x30u: // STROW
                    case 0x31u: // STCOL
                        len += 4u;
                        break;
                    case 0x4Au: // MPG
                        len += 2u * (num ? num : 256u);
                        break;
                    case 0x50u: // DIRECT
                    case 0x51u: // DIRECTHL
                        len += 4u * (imm ? imm : 65536u);
                        break;
                    default:
                        break;
                    }
                    g_gc.otherBytes += 4u * (len - 1u);
                }
                i += len;
            }
        }

        void gcRender(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t set = getRegU32(ctx, 5), ra = getRegU32(ctx, 31);
            const uint32_t before = word(rdram, kPacketPointer);
            g_gcRender(rdram, ctx, runtime);
            ++g_gc.calls;
            if (ctx->pc != ra || !set) // (stopped part-way at a VBlank: not counted)
                return;
            const uint32_t after = word(rdram, kPacketPointer);
            if (after == before || after - before > kJump)
                return; // (culled: nothing added)
            const uint32_t packet = word(rdram, set + 0x14u);
            auto [it, fresh] = g_gc.packets.try_emplace(packet);
            if (fresh)
                gcAnalyse(rdram, packet, it->second);
            ++g_gc.added;
            g_gc.addedBytes += it->second.bytes;
            const uint64_t tick = runtime->eeScheduler().currentVSyncTick();
            const auto now = std::chrono::steady_clock::now();
            if (now - g_gc.last < std::chrono::seconds(g_gcSeconds))
                return;
            const uint64_t ticks = tick - g_gc.lastTick;
            std::fprintf(stderr, "[n5:geometry] %llu ticks: %llu calls, %llu added a CALL - %.0f KB of packets pointed at a tick; "
                                 "%zu packets so far (%llu not followed): UNPACK %.1f MB (%llu TOPS-relative, fixed:",
                         static_cast<unsigned long long>(ticks), static_cast<unsigned long long>(g_gc.calls),
                         static_cast<unsigned long long>(g_gc.added), ticks ? g_gc.addedBytes / 1024.0 / static_cast<double>(ticks) : 0.0,
                         g_gc.packets.size(), static_cast<unsigned long long>(g_gc.failed), g_gc.unpackBytes / 1048576.0,
                         static_cast<unsigned long long>(g_gc.topsUnpacks));
            for (const auto &[q, n] : g_gc.fixedUnpacks)
                std::fprintf(stderr, " q%03x+ %llu", q, static_cast<unsigned long long>(n));
            std::fprintf(stderr, "), other data %.1f KB; programs:", g_gc.otherBytes / 1024.0);
            for (const auto &[a, n] : g_gc.programs)
                if (a == ~0u)
                    std::fprintf(stderr, " MSCNT %llu", static_cast<unsigned long long>(n));
                else
                    std::fprintf(stderr, " %04x %llu", a, static_cast<unsigned long long>(n));
            std::fprintf(stderr, "; codes:");
            for (uint32_t op = 0; op < 128u; ++op)
                if (g_gc.opcodes[op] && (op & 0x60u) != 0x60u)
                    std::fprintf(stderr, " %02x %llu", op, static_cast<unsigned long long>(g_gc.opcodes[op]));
            std::fprintf(stderr, "\n");
            g_gc.calls = g_gc.added = g_gc.addedBytes = 0u;
            g_gc.lastTick = tick;
            g_gc.last = now;
        }

        // ---- N5 step 0: what the kick sends, by where it points ----
        // TF_N5_KICKCENSUS=<s>: after each kick (0x25D2B0) the list's chain is walked from where the frame's list began:
        // the bytes in the list itself, and the bytes each CALL / REF points at, by its target (the geometry sets'
        // packets, as TF_N5_GEOMCENSUS met them, told apart). Every <s> seconds: the bytes a frame, and the 16 targets
        // that send the most.
        PS2Runtime::RecompiledFunction g_kcKick = nullptr;
        uint32_t g_kcSeconds = 0u;
        struct KcTarget
        {
            uint64_t bytes = 0u, uses = 0u;
            uint32_t id = 0u, size = 0u;
        };
        // (step 2's question: is a large CALL's target built once or every frame?) Its words hashed each time it is called
        // (targets over 16 KB that are not geometry sets' packets), the changes counted - kept across the reports.
        struct KcChange
        {
            uint64_t hash = 0u, calls = 0u, changed = 0u;
        };
        std::unordered_map<uint32_t, KcChange> g_kcChanges;

        void kcHash(const uint8_t *rdram, uint32_t target)
        {
            static std::vector<uint32_t> words;
            words.clear();
            if (!vif::gather(rdram, target, words, nullptr))
                return;
            uint64_t h = 1469598103934665603ull ^ words.size();
            for (uint32_t w : words)
                h = (h ^ w) * 1099511628211ull;
            KcChange &c = g_kcChanges[target];
            if (c.calls != 0u && c.hash != h)
                ++c.changed;
            c.hash = h;
            ++c.calls;
        }
        struct KcStats
        {
            uint32_t start = 0u;
            bool haveStart = false;
            uint64_t frames = 0u, inlineBytes = 0u, calledBytes = 0u, refBytes = 0u, geometryBytes = 0u, tags = 0u, broken = 0u;
            std::unordered_map<uint32_t, KcTarget> targets;
            std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
        } g_kc;

        // The bytes a chain sends from `tag` to its RET / END (depth: CALLs inside it).
        uint64_t kcWalk(const uint8_t *rdram, uint32_t tag, int depth, uint64_t &tags)
        {
            uint64_t bytes = 0u;
            uint32_t ret[2] = {0u, 0u};
            int sp = 0;
            for (int guard = 0; guard < (1 << 20); ++guard)
            {
                const uint32_t lo = word(rdram, tag), hi = word(rdram, tag + 4u);
                const uint32_t qwc = lo & 0xFFFFu, id = (lo >> 28) & 7u, addr = hi & 0x7FFFFFF0u;
                ++tags;
                bytes += 8u + 16u * qwc; // (the tag's upper half, TTE, and its data)
                switch (id)
                {
                case 0: // REFE
                    return bytes;
                case 1: // CNT
                    tag += 16u + 16u * qwc;
                    break;
                case 2: // NEXT
                    tag = addr;
                    break;
                case 3: // REF
                case 4: // REFS
                    if (depth == 0)
                    {
                        KcTarget &t = g_kc.targets[addr];
                        t.bytes += 16u * qwc, ++t.uses, t.id = id, t.size = 16u * qwc;
                        g_kc.refBytes += 16u * qwc;
                    }
                    tag += 16u;
                    break;
                case 5: // CALL
                    if (sp >= 2)
                        return bytes;
                    ret[sp++] = tag + 16u + 16u * qwc;
                    tag = addr;
                    break;
                case 6: // RET
                    if (sp == 0)
                        return bytes;
                    tag = ret[--sp];
                    break;
                default: // END
                    return bytes;
                }
                if (depth == 0 && sp == 1 && (id == 5))
                {
                    // (a top-level CALL: what it sends is walked as its own chain and put to its target)
                    --sp;
                    uint64_t subTags = 0u;
                    const uint64_t sent = kcWalk(rdram, addr, 1, subTags);
                    tags += subTags;
                    KcTarget &t = g_kc.targets[addr];
                    t.bytes += sent, ++t.uses, t.id = 5u, t.size = static_cast<uint32_t>(sent);
                    g_kc.calledBytes += sent;
                    if (g_gc.packets.count(addr))
                        g_kc.geometryBytes += sent;
                    else if (sent > 16384u)
                        kcHash(rdram, addr);
                    tag = ret[0];
                }
            }
            ++g_kc.broken;
            return bytes;
        }

        void kcKick(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = getRegU32(ctx, 31);
            g_kcKick(rdram, ctx, runtime);
            if (ctx->pc != ra)
                return;
            const uint32_t after = word(rdram, kPacketPointer);
            if (g_kc.haveStart)
            {
                uint64_t tags = 0u;
                const uint64_t refsBefore = g_kc.refBytes;
                const uint64_t all = kcWalk(rdram, g_kc.start, 0, tags); // (the list's own level: its words and the REFs' data)
                g_kc.inlineBytes += all - std::min<uint64_t>(all, g_kc.refBytes - refsBefore);
                g_kc.tags += tags;
                ++g_kc.frames;
                // (the list's own level, its CALLs not followed: where its UNPACKs write, and the VIF state it sets)
                static std::vector<uint32_t> words;
                words.clear();
                if (vif::gather(rdram, g_kc.start, words, nullptr, [](uint32_t) { return true; }))
                {
                    uint32_t cl = 1u, wl = 1u;
                    for (size_t i = 0; i < words.size();)
                    {
                        const uint32_t cmd = words[i], op = (cmd >> 24) & 0x7Fu, num = (cmd >> 16) & 0xFFu, imm = cmd & 0xFFFFu;
                        size_t len = 1u;
                        if ((op & 0x60u) == 0x60u)
                        {
                            static constexpr uint8_t kVectorBytes[16] = {4, 2, 1, 2, 8, 4, 2, 4, 12, 6, 3, 6, 16, 8, 4, 2};
                            uint32_t vectors = num ? num : 256u;
                            if (cl < wl)
                                vectors = (vectors / wl) * cl + std::min(vectors % wl, cl);
                            len += ((vectors * kVectorBytes[op & 0xFu] + 3u) & ~3u) / 4u;
                            markUnpack(imm, num, cl, wl, 4u, 8u);
                            ++g_inlineUnpacks;
                            if (op & 0x10u)
                                ++g_inlineMasked;
                        }
                        else
                        {
                            ++g_inlineState[op];
                            if (op == 0x01u)
                                cl = std::max(1u, imm & 0xFFu), wl = std::max(1u, (imm >> 8) & 0xFFu);
                            else if (op == 0x20u)
                                len += 1u;
                            else if (op == 0x30u || op == 0x31u)
                                len += 4u;
                            else if (op == 0x4Au)
                                len += 2u * (num ? num : 256u);
                            else if (op == 0x50u || op == 0x51u)
                                len += 4u * (imm ? imm : 65536u);
                        }
                        i += len;
                    }
                }
            }
            g_kc.start = after;
            g_kc.haveStart = true;
            const auto now = std::chrono::steady_clock::now();
            if (now - g_kc.last < std::chrono::seconds(g_kcSeconds) || g_kc.frames == 0u)
                return;
            const double f = static_cast<double>(g_kc.frames);
            std::fprintf(stderr, "[n5:kick] %llu frames: a frame %.0f KB in the list, %.0f KB called (%.0f KB of it geometry sets), %.0f KB referenced; "
                                 "%.0f tags; %llu chains not followed to their end. The most:",
                         static_cast<unsigned long long>(g_kc.frames), g_kc.inlineBytes / 1024.0 / f, g_kc.calledBytes / 1024.0 / f,
                         g_kc.geometryBytes / 1024.0 / f, g_kc.refBytes / 1024.0 / f, g_kc.tags / f, static_cast<unsigned long long>(g_kc.broken));
            std::vector<std::pair<uint32_t, KcTarget>> top(g_kc.targets.begin(), g_kc.targets.end());
            std::sort(top.begin(), top.end(), [](const auto &a, const auto &b) { return a.second.bytes > b.second.bytes; });
            for (size_t i = 0; i < top.size() && i < 16u; ++i)
            {
                if (const auto c = g_kcChanges.find(top[i].first); c != g_kcChanges.end())
                    std::fprintf(stderr, " [changed %llu of %llu calls]", static_cast<unsigned long long>(c->second.changed),
                                 static_cast<unsigned long long>(c->second.calls));
                std::fprintf(stderr, " %s %08x %.0f KB/frame (%.1f a frame, %u B each%s)", top[i].second.id == 5u ? "CALL" : "REF", top[i].first,
                             top[i].second.bytes / 1024.0 / f, top[i].second.uses / f, top[i].second.size,
                             g_gc.packets.count(top[i].first) ? ", a geometry set's" : "");
            }
            std::fprintf(stderr, "; %zu targets\n", top.size());
            // (where the geometry sets' packets and the list itself write VU1 data memory: ranges of qwords by who writes them)
            std::fprintf(stderr, "[n5:kick] VU1 qwords written (g = geometry sets, l = the list, t = TOPS-relative, from 0):");
            for (uint32_t q = 0; q < 1024u;)
            {
                const uint8_t c = g_cover[q];
                uint32_t e = q;
                while (e + 1u < 1024u && g_cover[e + 1u] == c)
                    ++e;
                if (c)
                    std::fprintf(stderr, " %03x-%03x:%s%s%s%s", q, e, (c & 1u) ? "g" : "", (c & 2u) ? "gt" : "", (c & 4u) ? "l" : "", (c & 8u) ? "lt" : "");
                q = e + 1u;
            }
            std::fprintf(stderr, " | the list: %llu UNPACKs (%llu masked); its codes:", static_cast<unsigned long long>(g_inlineUnpacks),
                         static_cast<unsigned long long>(g_inlineMasked));
            for (uint32_t op = 0; op < 128u; ++op)
                if (g_inlineState[op])
                    std::fprintf(stderr, " %02x %llu", op, static_cast<unsigned long long>(g_inlineState[op]));
            std::fprintf(stderr, "\n");
            const uint32_t start = g_kc.start;
            g_kc = KcStats{};
            g_kc.start = start;
            g_kc.haveStart = true;
            g_kc.last = now;
        }

        // ---- N5 step 1: the geometry sets' packets summarized at the kick (PS2Recomp's setChainSummaries) ----
        // A geometry set's packet (set + 0x14, built at load) is called by the list once per draw (~1,400 times a frame
        // in Deep Amazon, ~5.5 MB): while VIF1 is lite its UNPACKs do nothing, so the kick's copy sends, instead of the
        // call, the packet's VIF codes without its UNPACKs - made once per packet and kept, checked at each use against
        // the packet's first tag (16 bytes) and dropped when a level loads. The packets are learnt from RenderGeometrySet
        // (its set's packet). A packet is summarized only when the whole of it can be followed here: CNT / NEXT / REF /
        // REFS tags ending in its RET, no interrupt bit on a tag, its STCYCL before its first UNPACK, and the codes ending
        // exactly with its data. Every code but UNPACK stays in the summary with its data - DIRECT's GIF data and MPG's
        // microcode too (the large buffers hold them): VIF1 and the GS get what the whole call gave them, less the UNPACKs. TF_N5_SUMMARY=0: off (the calls followed, as before); =check: the chain sent
        // whole, the summarized copy checked beside it by VIF1 (PS2Recomp's check), and each summary made again at each use
        // and compared with the kept one.
        constexpr uint32_t kSmRenderGeometrySet = 0x00260A70u;
        PS2Runtime::RecompiledFunction g_smRender = nullptr;
        struct Summary
        {
            bool built = false, ok = false;
            uint32_t tag[4] = {};
            uint32_t whole = 0u;
            std::vector<uint8_t> bytes;
            // (step 2 part 1: a large buffer's - its tags' places and words for the change check, its stream's hash, how
            // many times it has been seen the same, its uses since it was last read whole)
            std::vector<uint32_t> tagsAt, tagWords;
            uint64_t hash = 0u;
            uint32_t same = 0u, seen = 0u, uses = 0u, nextTag = 0u;
            bool promoted = false, given = false; // (given up: changing, too small, or not summarizable)
        };
        std::unordered_map<uint32_t, Summary> g_summaries; // (by packet address; the EE's thread only)
        // (TODO 94) A direct-mapped look-aside in front of g_summaries: both its users (the render hook and the kick's
        // summary) ask it for every geometry set, a hash of the packet address each time (~1% of the game thread). A
        // slot holds a packet and its summary (the map's nodes stay put until dropSummaries clears both).
        constexpr uint32_t kSmCacheSlots = 4096u;
        uint32_t g_smCacheKey[kSmCacheSlots] = {};
        Summary *g_smCachePtr[kSmCacheSlots] = {};
        uint32_t smCacheSlot(uint32_t packet) { return (packet >> 4) & (kSmCacheSlots - 1u); }
        Summary *smFind(uint32_t packet)
        {
            const uint32_t i = smCacheSlot(packet);
            if (g_smCacheKey[i] == packet && g_smCachePtr[i])
                return g_smCachePtr[i];
            const auto it = g_summaries.find(packet);
            if (it == g_summaries.end())
                return nullptr;
            g_smCacheKey[i] = packet;
            g_smCachePtr[i] = &it->second;
            return &it->second;
        }
        int g_smMode = 0;                                  // (0 off, 1 on, 2 checked)
        struct SmStats
        {
            uint64_t built = 0u, refused = 0u, rebuilt = 0u, stale = 0u, dropped = 0u;
            int told = 0;
        } g_sm;

        int summaryMode()
        {
            static const int selected = []() {
                const char *e = std::getenv("TF_N5_SUMMARY");
                if (e && *e == '0')
                    return 0;
                return (e && std::strcmp(e, "check") == 0) ? 2 : 1;
            }();
            return selected;
        }

        // What a call of the packet sends VIF1 (each tag's upper half, then its data), and whether it can be summarized.
        int g_smRefusal = 0; // (smBuild's last refusal: 1 the tags, 2 an interrupt bit, 3 CYCLE unknown, 4 MARK/MPG/DIRECT, 5 misread)

        bool smStream(const uint8_t *rdram, uint32_t packet, std::vector<uint32_t> &out, std::vector<uint32_t> *tagsAt = nullptr,
                      int maxTags = 256, size_t maxWords = size_t(1) << 16)
        {
            out.clear();
            if (tagsAt)
                tagsAt->clear();
            uint32_t tag = packet;
            for (int guard = 0; guard < maxTags; ++guard)
            {
                if (tagsAt)
                    tagsAt->push_back(tag);
                const uint32_t lo = word(rdram, tag), hi = word(rdram, tag + 4u);
                const uint32_t qwc = lo & 0xFFFFu, id = (lo >> 28) & 7u, addr = hi & 0x7FFFFFF0u;
                if ((lo >> 31) != 0u)
                {
                    g_smRefusal = 2;
                    return false; // (an interrupt bit: it may end the chain at the kick - TIE)
                }
                out.push_back(word(rdram, tag + 8u));
                out.push_back(word(rdram, tag + 12u));
                const uint32_t data = (id == 3u || id == 4u) ? addr : tag + 16u;
                for (uint32_t i = 0; i < qwc * 4u; ++i)
                    out.push_back(word(rdram, data + 4u * i));
                if (out.size() > maxWords)
                    return false;
                switch (id)
                {
                case 1u: tag = tag + 16u + 16u * qwc; break; // CNT
                case 2u: tag = addr; break;                   // NEXT
                case 3u: case 4u: tag = tag + 16u; break;     // REF / REFS
                case 6u: return true;                         // RET: the call's end
                default: g_smRefusal = 1; return false;       // (END / REFE / a CALL of its own: not summarized)
                }
            }
            return false;
        }

        bool smBuild(const uint8_t *rdram, uint32_t packet, Summary &s, bool large = false)
        {
            static std::vector<uint32_t> words;
            s.built = true;
            s.ok = false;
            s.bytes.clear();
            for (uint32_t i = 0; i < 4u; ++i)
                s.tag[i] = word(rdram, packet + 4u * i);
            if (!smStream(rdram, packet, words, large ? &s.tagsAt : nullptr, large ? (1 << 16) : 256,
                          large ? (size_t(4) << 20) : (size_t(1) << 16)))
                return false;
            s.whole = static_cast<uint32_t>(words.size() * 4u);
            if (large)
            {
                uint64_t h = 1469598103934665603ull ^ words.size();
                for (uint32_t w : words)
                    h = (h ^ w) * 1099511628211ull;
                s.hash = h;
                s.tagWords.resize(s.tagsAt.size() * 4u);
                for (size_t t = 0; t < s.tagsAt.size(); ++t)
                    for (uint32_t i = 0; i < 4u; ++i)
                        s.tagWords[4u * t + i] = word(rdram, s.tagsAt[t] + 4u * i);
            }
            std::vector<uint32_t> kept;
            kept.reserve(64u);
            bool cycleSet = false;
            uint32_t cl = 1u, wl = 1u;
            size_t i = 0;
            while (i < words.size())
            {
                const uint32_t cmd = words[i];
                const uint32_t op = (cmd >> 24) & 0x7Fu, num = (cmd >> 16) & 0xFFu, imm = cmd & 0xFFFFu;
                if ((op & 0x60u) == 0x60u)
                {
                    if (!cycleSet)
                    {
                        g_smRefusal = 3;
                        return false; // (its length would depend on the list's CYCLE)
                    }
                    static constexpr uint8_t kVectorBytes[16] = {4, 2, 1, 2, 8, 4, 2, 4, 12, 6, 3, 6, 16, 8, 4, 2};
                    uint32_t vectors = num ? num : 256u;
                    if (cl < wl)
                        vectors = (vectors / wl) * cl + std::min(vectors % wl, cl);
                    i += 1u + ((vectors * kVectorBytes[op & 0xFu] + 3u) & ~3u) / 4u;
                    continue; // (left out: code and data)
                }
                size_t len = 1u;
                switch (op)
                {
                case 0x01u: // STCYCL
                    cycleSet = true;
                    cl = std::max(1u, imm & 0xFFu), wl = std::max(1u, (imm >> 8) & 0xFFu);
                    break;
                case 0x20u: len = 2u; break;             // STMASK
                case 0x30u: case 0x31u: len = 5u; break; // STROW, STCOL
                case 0x4Au: // MPG: kept, with its microcode
                    len = 1u + 2u * (num == 0u ? 256u : num);
                    break;
                case 0x50u: // DIRECT / DIRECTHL: kept, with their GIF data (the GS gets what it got)
                case 0x51u:
                    len = 1u + 4u * (imm == 0u ? 65536u : imm);
                    break;
                default:
                    break;
                }
                if (i + len > words.size())
                    return false;
                kept.insert(kept.end(), words.begin() + static_cast<std::ptrdiff_t>(i), words.begin() + static_cast<std::ptrdiff_t>(i + len));
                i += len;
            }
            if (i != words.size())
            {
                g_smRefusal = 5;
                return false; // (the codes did not end with the packet: misread)
            }
            s.bytes.resize(kept.size() * 4u);
            std::memcpy(s.bytes.data(), kept.data(), s.bytes.size());
            s.ok = true;
            return true;
        }

        // (step 2 part 1) A CALL's target that is not a geometry set's packet: the large buffers a level builds once and calls
        // once a frame (~2 MB a frame in Deep Amazon, 74-645 KB each). Read whole the first times it is met; summarized from
        // the 5th time it has read the same (its words' hash), given up if it changes (8 times read differently), is under
        // 16 KB or cannot be summarized. Once summarized, its change check at each use: its first tag and 8 of its tags in
        // turn (their 16 bytes against the kept ones), and every 128th use read whole again and compared (an in-place change
        // behind the tags shows within ~1 s); dropped with the rest when a level loads or the HQ is entered or left.
        // OFF by default (TF_N5_SUMMARY_LARGE=1): measured 2026-11-06 (test PC, Deep Amazon) these buffers are nearly all
        // DIRECT GIF data and microcode, which stay in the summary - 8 of 11 summarized, 0 changed, the check clean, but the
        // kick's copy only ~0.1 of its ~2.9 MB a frame smaller.
        std::unordered_map<uint32_t, Summary> g_large;
        struct LargeStats
        {
            uint64_t promoted = 0u, givenUp = 0u, changedTags = 0u, changedWhole = 0u, rechecked = 0u;
            uint64_t small = 0u, unstable = 0u, refused = 0u, refusedBytes = 0u; // (why given up)
            std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
        } g_ls;
        std::map<uint32_t, std::pair<int, uint32_t>> g_largeRefused; // (target -> refusal, its tags' bytes as far as read)

        bool largeOn()
        {
            static const bool on = std::getenv("TF_N5_SUMMARY_LARGE") && *std::getenv("TF_N5_SUMMARY_LARGE") == '1';
            return on;
        }

        bool smLarge(const uint8_t *rdram, uint32_t target, const uint8_t *&bytes, uint32_t &size, uint32_t &wholeBytes)
        {
            if (!largeOn())
                return false;
            if (const auto now = std::chrono::steady_clock::now(); now - g_ls.last >= std::chrono::seconds(30))
            {
                g_ls.last = now;
                std::fprintf(stderr, "[n5:summary] large buffers: %zu met, %llu summarized, %llu given up (%llu under 16 KB, %llu changing, %llu not "
                                     "summarizable), %llu changed at their tags, %llu behind them (%llu whole reads)",
                             g_large.size(), static_cast<unsigned long long>(g_ls.promoted), static_cast<unsigned long long>(g_ls.givenUp),
                             static_cast<unsigned long long>(g_ls.small), static_cast<unsigned long long>(g_ls.unstable),
                             static_cast<unsigned long long>(g_ls.refused), static_cast<unsigned long long>(g_ls.changedTags),
                             static_cast<unsigned long long>(g_ls.changedWhole), static_cast<unsigned long long>(g_ls.rechecked));
                int n = 0;
                for (const auto &[t, r] : g_largeRefused)
                    if (n++ < 12)
                        std::fprintf(stderr, " %08x:%d(%u B)", t, r.first, r.second);
                std::fprintf(stderr, "\n");
            }
            Summary &s = g_large[target];
            if (s.given)
                return false;
            if (!s.promoted)
            {
                const uint64_t before = s.hash;
                const bool had = s.built;
                g_smRefusal = 0;
                const bool built = smBuild(rdram, target, s, true);
                if (!built || s.whole < 16384u)
                {
                    s.given = true;
                    ++g_ls.givenUp;
                    if (built)
                        ++g_ls.small;
                    else
                    {
                        ++g_ls.refused;
                        g_largeRefused[target] = {g_smRefusal, s.whole};
                    }
                    s.bytes.clear(), s.tagsAt.clear(), s.tagWords.clear();
                    return false;
                }
                ++s.seen;
                s.same = had && s.hash == before ? s.same + 1u : 0u;
                if (s.seen - s.same > 8u)
                {
                    s.given = true; // (built anew every frame or often: not a buffer to keep)
                    ++g_ls.givenUp;
                    ++g_ls.unstable;
                    s.bytes.clear(), s.tagsAt.clear(), s.tagWords.clear();
                    return false;
                }
                if (s.same < 4u)
                    return false;
                s.promoted = true;
                ++g_ls.promoted;
            }
            else
            {
                // (the change check: the first tag, 8 tags in turn; every 128th use, or every use with the check, read whole)
                bool changed = false;
                const size_t tags = s.tagsAt.size();
                for (uint32_t k = 0; k < 9u && !changed && tags; ++k)
                {
                    const size_t t = k == 0u ? 0u : (s.nextTag++ % tags);
                    for (uint32_t i = 0; i < 4u; ++i)
                        if (word(rdram, s.tagsAt[t] + 4u * i) != s.tagWords[4u * t + i])
                        {
                            changed = true;
                            break;
                        }
                }
                if (changed)
                    ++g_ls.changedTags;
                else if (g_smMode == 2 || ++s.uses % 128u == 0u)
                {
                    ++g_ls.rechecked;
                    const uint64_t kept = s.hash;
                    Summary again;
                    if (!smBuild(rdram, target, again, true) || again.hash != kept)
                    {
                        changed = true;
                        ++g_ls.changedWhole;
                        if (g_sm.told++ < 8)
                            std::fprintf(stderr, "[n5:summary] large buffer %08x changed behind its tags - read whole again\n", target);
                    }
                }
                if (changed)
                {
                    // (back to being read whole: summarized again only once it has read the same 4 more times)
                    s = Summary{};
                    return false;
                }
            }
            if (!s.ok)
                return false;
            bytes = s.bytes.data();
            size = static_cast<uint32_t>(s.bytes.size());
            wholeBytes = s.whole;
            return true;
        }

        bool smSummary(const uint8_t *rdram, uint32_t target, const uint8_t *&bytes, uint32_t &size, uint32_t &wholeBytes)
        {
            Summary *found = smFind(target);
            if (!found)
                return smLarge(rdram, target, bytes, size, wholeBytes);
            Summary &s = *found;
            bool fresh = !s.built;
            if (!fresh)
                for (uint32_t i = 0; i < 4u; ++i)
                    if (word(rdram, target + 4u * i) != s.tag[i])
                    {
                        fresh = true;
                        ++g_sm.rebuilt;
                        break;
                    }
            if (fresh)
            {
                if (smBuild(rdram, target, s))
                    ++g_sm.built;
                else
                    ++g_sm.refused;
            }
            else if (g_smMode == 2 && s.ok)
            {
                // (the check: made again now and compared - a packet changed behind its first tag)
                Summary again;
                smBuild(rdram, target, again);
                if (!again.ok || again.bytes != s.bytes || again.whole != s.whole)
                {
                    ++g_sm.stale;
                    if (g_sm.told++ < 8)
                        std::fprintf(stderr, "[n5:summary] packet %08x changed behind its first tag (%u -> %u bytes) - made again\n", target,
                                     static_cast<uint32_t>(s.bytes.size()), static_cast<uint32_t>(again.bytes.size()));
                    s = std::move(again);
                }
            }
            if (!s.ok)
                return false;
            bytes = s.bytes.data();
            size = static_cast<uint32_t>(s.bytes.size());
            wholeBytes = s.whole;
            return true;
        }

        bool smLite() { return liteWanted(); }

        void smRender(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kSmRenderGeometrySet) // (not a call stopped part-way and entered again)
                if (const uint32_t set = getRegU32(ctx, 5))
                    if (const uint32_t packet = word(rdram, set + 0x14u); packet && !smFind(packet))
                        g_summaries.try_emplace(packet);
            g_smRender(rdram, ctx, runtime);
        }

        void installSummaries(PS2Runtime &runtime)
        {
            g_smMode = summaryMode();
            if (g_smMode == 0 || !(g_smRender = runtime.lookupFunction(kSmRenderGeometrySet)))
                return;
            runtime.registerFunction(kSmRenderGeometrySet, smRender);
            ps2vif1::setChainSummaries(&smLite, &smSummary, g_smMode == 2);
            std::fprintf(stderr, "[n5] N5 step 1: the geometry sets' packets summarized at the kick while lite%s (TF_N5_SUMMARY=0: off, =check)\n",
                         g_smMode == 2 ? " - checked: sent whole, the summaries compared" : "");
        }

        // (TODO 169, TF_NATIVE_LATLOG) The pad read (scePadRead 0x13A898) timed: nr_thread.cpp's latency log takes the
        // last one before each frame is handed over - the input that frame acts on.
        constexpr uint32_t kPadRead = 0x0013A898u;
        PS2Runtime::RecompiledFunction g_padRead = nullptr;
        std::atomic<int64_t> g_lastInputNs{0};

        void padRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_lastInputNs.store(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(),
                                std::memory_order_relaxed);
            g_padRead(rdram, ctx, runtime);
        }

        void installReadbacks(PS2Runtime &runtime)
        {
            if (std::getenv("TF_NATIVE_LATLOG") && (g_padRead = runtime.lookupFunction(kPadRead)) != nullptr)
                runtime.registerFunction(kPadRead, padRead);
            if (const char *kc = std::getenv("TF_N5_KICKCENSUS"))
                g_kcSeconds = static_cast<uint32_t>(std::max(0, std::atoi(kc)));
            if (g_kcSeconds != 0u && (g_kcKick = runtime.lookupFunction(kKick)) != nullptr)
            {
                runtime.registerFunction(kKick, kcKick);
                std::fprintf(stderr, "[n5:kick] on: the kick's chain walked every frame, counted every %u s (TF_N5_KICKCENSUS)\n", g_kcSeconds);
            }
            if (const char *gc = std::getenv("TF_N5_GEOMCENSUS"))
                g_gcSeconds = static_cast<uint32_t>(std::max(0, std::atoi(gc)));
            if (g_gcSeconds != 0u && (g_gcRender = runtime.lookupFunction(kRenderGeometrySet)) != nullptr)
            {
                runtime.registerFunction(kRenderGeometrySet, gcRender);
                std::fprintf(stderr, "[n5:geometry] on: the geometry sets' packets counted every %u s (TF_N5_GEOMCENSUS)\n", g_gcSeconds);
            }
            const char *e = std::getenv("TF_N5_READBACKS");
            g_rbSeconds = e ? static_cast<uint32_t>(std::max(0, std::atoi(e))) : 0u;
            if (g_rbSeconds == 0u)
                return;
            g_rbTransfer = runtime.lookupFunction(kRbTransfer);
            g_rbSurfaceCopy = runtime.lookupFunction(kRbSurfaceCopy);
            g_rbStoreImage = runtime.lookupFunction(kRbStoreImage);
            if (!g_rbTransfer || !g_rbSurfaceCopy || !g_rbStoreImage)
            {
                std::fprintf(stderr, "[n5:readbacks] a function to wrap is missing - not counting\n");
                return;
            }
            runtime.registerFunction(kRbTransfer, rbTransfer);
            runtime.registerFunction(kRbSurfaceCopy, rbSurfaceCopy);
            runtime.registerFunction(kRbStoreImage, rbStoreImage);
            std::fprintf(stderr, "[n5:readbacks] on: the GS read-backs counted every %u s (TF_N5_READBACKS)\n", g_rbSeconds);
        }
    }

    int64_t lastInputNs() { return g_lastInputNs.load(std::memory_order_relaxed); }

    void dropSummaries()
    {
        if (g_summaries.empty() && g_large.empty())
            return;
        std::fprintf(stderr, "[n5:summary] a level loaded: %zu packets' summaries dropped (%llu made, %llu not summarizable, %llu made again, "
                             "%llu changed behind their first tag)\n",
                     g_summaries.size(), static_cast<unsigned long long>(g_sm.built), static_cast<unsigned long long>(g_sm.refused),
                     static_cast<unsigned long long>(g_sm.rebuilt), static_cast<unsigned long long>(g_sm.stale));
        std::fprintf(stderr, "[n5:summary] ... and %zu large buffers met (%llu summarized, %llu given up, %llu changed at their tags, "
                             "%llu changed behind them in %llu whole reads)\n",
                     g_large.size(), static_cast<unsigned long long>(g_ls.promoted), static_cast<unsigned long long>(g_ls.givenUp),
                     static_cast<unsigned long long>(g_ls.changedTags), static_cast<unsigned long long>(g_ls.changedWhole),
                     static_cast<unsigned long long>(g_ls.rechecked));
        g_summaries.clear();
        std::fill(std::begin(g_smCacheKey), std::end(g_smCacheKey), 0u);
        std::fill(std::begin(g_smCachePtr), std::end(g_smCachePtr), nullptr);
        g_large.clear();
        g_sm = SmStats{};
        g_ls = LargeStats{};
    }

    void install(PS2Runtime &runtime)
    {
        installReadbacks(runtime);
        // N5.3 (with the native renderer only: deviceNative needs it drawing alone)
        if (deviceMode() != 0)
        {
            g_useTexture = runtime.lookupFunction(kUseTexture);
            if (g_useTexture)
                runtime.registerFunction(kUseTexture, useTexture);
            g_renderEnvironment = runtime.lookupFunction(kRenderEnvironment);
            if (g_renderEnvironment)
                runtime.registerFunction(kRenderEnvironment, renderEnvironment);
            std::fprintf(stderr, "[n5] N5.3: the device's SetGSRegister, ApplyStateBlock, UseTexture and SetHardwareRenderEnvironment native (TF_N5_DEVICE=0: off, =check: checked)\n");
        }
        // N5.2 (with the native renderer only: gsNativeAlone is never true without it)
        if (mode() == Mode::On)
        {
            ps2vif1::setLite(&litePredicate);
            if (scanLiteAllowed())
                ps2vif1::setLiteScan(&liteScanOn);
            installSummaries(runtime);
            std::fprintf(stderr, "[n5] N5.2: VIF1 lite while the native renderer draws alone, its scan %s (TF_N5=0: off, =check: checked; TF_N5_SCAN=ee / 0)\n",
                         scanMode() == 3 ? "on the VIF1 thread" : (scanMode() == 1 ? "on the EE's thread without the GIF walk" : "as before"));
        }
        else if (mode() == Mode::Check)
        {
            g_check.vu1 = runtime.memory().getVU1Data();
            if (ps2vu1native::dataObserver())
                std::fprintf(stderr, "[n5:check] another VU1 data observer is set (TF_NATIVE_NOVU1=check?): not checking\n");
            else
            {
                ps2vu1native::setDataObserver(&observe);
                ps2vif1::setLite(&checkPredicate);
                ps2vif1::setLiteScan(&liteScanCheck);
                std::fprintf(stderr, "[n5:check] N5.2 checked: the EE's reads of VU1 data memory against what lite transfers would leave\n");
            }
        }
        if (seconds() == 0u)
            return;
        // (after the capture's own wrappers: these wrap them in turn, so the bytes of a hooked function are its original's)
        const uint8_t *rdram = runtime.memory().getRDRAM();
        int slots = 0;
        for (uint32_t i = 0; i < kDeviceSlots; ++i)
        {
            char name[48];
            if (i < 2u)
                continue; // (the vtable's type_info and offset words)
            std::snprintf(name, sizeof(name), "device slot %u", i - 2u);
            if (add(runtime, word(rdram, kDeviceVtable + 4u * i), name))
                ++slots;
        }
        int helpers = 0;
        for (const Named &n : kHelpers)
            if (add(runtime, n.address, n.name))
                ++helpers;
        std::fprintf(stderr, "[n5:census] on: %d device slots and %d helpers wrapped, a report every %u s (TF_N5_CENSUS)\n", slots, helpers, seconds());
    }

    bool liteNow() { return mode() == Mode::On && liteWanted(); }
}
