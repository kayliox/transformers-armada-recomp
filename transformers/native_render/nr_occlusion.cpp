// The sun's visibility test answered from the native renderer's own depth (roadmap N3a).
//
// The game: vlVisibilityTestNode::Test (0x247E40), when the sun is on the screen, copies the depth around its place (a
// 2 x 2 rect of the 512 x 448 scene) into the 64 x 64 "OcclusionBuffer" (CopySurfaceRect 0x2078C0: a vlDepthBufferPS2,
// PSMZ24, at D_006CEC5C; page 384) and counts the pixels at or behind the sun (0x2559D0: Z24 <= its threshold) among the
// first D_006CD9CC of the buffer's host copy D_006CEC68 - 1 in this game: the copy's first pixel decides. The visibility,
// count / D_006CD9CC, is node + 0x64 (0x247E20 reads it); the sun (0x409AD0) smooths it into D_006CF870 and its +0x8C,
// and its draw (0x409610) leaves out the lens flare and the glare's colour while that is 0. BeginFrame (0x25BE20, at
// 0x25C14C) reads the host copy back from GS memory each frame (0x262D70 -> 0x2556B0 -> 0x255A90 ->
// sceGsExecStoreImage): the EE waits there until the GS threads are past whatever can still write page 384 (PS2Recomp's
// read-back ahead) - in levels that upload textures there (Deep Amazon) several milliseconds a frame with the emulated
// drawing on.
// Measured in this port (2026-11-01, Amazon and Deep Amazon, both renderers): the copy never reaches GS memory - no draw
// has page 384 as its frame - so the read-back returns whatever texture sits there and the count, of one texel, came out
// 0 every frame: the flare and the glare's colour were never drawn, with either renderer.
//
// Here: the copy's rect is noted (game thread), the native renderer reads the 3D target's depth over it into a 64 x 64
// buffer as it draws that frame (nr_d3d.cpp: the occlusion probe), and once the GPU has drawn it the values come back
// as Z24 (depth * 2^28). The read-back 0x2556B0 for that surface, while the native renderer draws alone, writes the
// newest of them into the host copy and does no GS transfer: no wait, never on the GPU either (one frame late, two at
// most - as the console's own, which reads the frame before's copy). With the emulated drawing on (the compare mode,
// menus, a device lost) the GS transfer runs as before.
//
// TF_NATIVE_OCCLUSION=0: off (the GS transfer always, no probe). TF_NATIVE_OCCSHOT=<n> (debug): native shots of the first
// <n> frames the test copies in (the sun on the screen), 40 ticks apart at least (runs/native_<tick>.bmp).
// TF_NATIVE_OCCLOG=<ticks between lines> (debug): the test's last copy (rect, threshold, count, visibility), the host
// copy's Z range, the time the EE spent in 0x2556B0, and the count the newest native answer gives for the same threshold.
#include "nr_occlusion.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_host_display.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace nr
{
    namespace occlusion
    {
        namespace
        {
            constexpr uint32_t kTest = 0x00247E40u;            // vlVisibilityTestNode::Test (node, scene)
            constexpr uint32_t kCopySurfaceRect = 0x002078C0u; // (source surface, destination surface, source rect, destination rect)
            constexpr uint32_t kCount = 0x002559D0u;           // (threshold) -> pixels of the host copy with Z24 <= it
            constexpr uint32_t kTransfer = 0x002556B0u;        // (surface, GS address, host, convert-to): the read-back
            constexpr uint32_t kOcclusionBuffer = 0x006CEC5Cu; // D_006CEC5C (gp - 0x4494): the surface
            constexpr uint32_t kHostCopy = 0x006CEC68u;        // D_006CEC68 (gp - 0x4488): its 64 x 64 host copy (4 bytes a pixel)
            constexpr uint32_t kPixels = 4096u;
            constexpr uint32_t kPixelCount = 0x006CD9CCu;      // D_006CD9CC (gp - 0x5724): the pixels 0x2559D0 counts over
            constexpr uint32_t kScreenYScale = 0x006CD82Cu;    // D_006CD82C: Test's screen-y scale (0.5 dual-field, else 1.0)

            PS2Runtime::RecompiledFunction g_test = nullptr, g_copy = nullptr, g_count = nullptr, g_transfer = nullptr;
            bool g_on = false;
            uint32_t g_log = 0u, g_shotsLeft = 0u;

            // The copies made (game thread -> render thread): the tick each was made at and its rect.
            struct Copy
            {
                uint64_t tick = 0u;
                float rect[4] = {};
            };
            std::mutex g_copyMutex;
            std::array<Copy, 16> g_copies{};
            uint64_t g_copiesMade = 0u;
            uint64_t g_lastProbed = 0u; // (render thread: the frame drawn before's tick)

            // The answers (render thread -> game thread): the newest frame's Z24, and its tick.
            std::mutex g_answerMutex;
            std::array<uint32_t, kPixels> g_answer{};
            uint64_t g_answerTick = 0u, g_answers = 0u;

            // (TF_NATIVE_OCCLOG) what the game thread saw since the last line.
            struct Seen
            {
                uint64_t tests = 0u, copies = 0u, counts = 0u, transfers = 0u, native = 0u;
                double transferUs = 0.0, transferMaxUs = 0.0;
                float rect[4] = {}, dest[4] = {};
                uint32_t source = 0u, sourceW = 0u, sourceH = 0u, node = 0u, threshold = 0u, count = 0u;
                uint8_t onScreen = 0u;
                float visibility = -1.0f, fraction = -1.0f;
            } g_seen;

            uint32_t word(const uint8_t *rdram, uint32_t addr)
            {
                const uint8_t *p = getConstMemPtr(rdram, addr);
                uint32_t v = 0u;
                if (p)
                    std::memcpy(&v, p, 4);
                return v;
            }
            float fl(const uint8_t *rdram, uint32_t addr)
            {
                const uint32_t w = word(rdram, addr);
                float f;
                std::memcpy(&f, &w, 4);
                return f;
            }

            // (TODO 307 (n)) The sun's visibility from the fraction of the probe's samples that see it. The game counts
            // the host copy's first pixel only (D_006CD9CC is 1), so its visibility is 0 or 1 and, with a leaf or a
            // screen edge crossing the sun, toggles; its smoothing (UpdateVisibility: + 0.3 of the difference a frame,
            // D_006CB648, not scaled for the frame rate) eases that over about 3 frames at 30 fps, and hardly at all at
            // 60 and above: the glare popped on and off. Here, while the native answer feeds the host copy, the
            // visibility Test leaves in node + 0x64 is replaced by the share of the 64 x 64 samples at or behind the sun
            // (the same threshold), eased in time (TF_SUN_EASE seconds, default 0.08 - about the PS2's own 0.3 a frame
            // at 30 fps): a leaf passing over the sun fades the glare instead of switching it. (The test's area is small -
            // a 2 x 2 rect of the 512 x 448 scene - so a leaf mostly covers all of it or none: the easing does most of
            // the work, the share gives the in-between values at a covering edge.) TF_SUN_SAMPLES=0: the PS2's
            // single-point test as before.
            bool g_samples = true;
            float g_easeSeconds = 0.08f;
            bool g_counted = false;   // (Test called the count this time)
            uint32_t g_threshold = 0u; // (the threshold it counted with)
            struct Eased
            {
                uint32_t node = 0u;
                uint64_t tick = 0u;
                float value = 0.0f;
            };
            std::array<Eased, 4> g_eased{};

            void sampleVisibility(uint8_t *rdram, PS2Runtime *runtime, uint32_t node)
            {
                uint32_t under = 0u;
                {
                    std::lock_guard<std::mutex> lock(g_answerMutex);
                    if (!g_answers)
                        return; // (no native answer yet: the game's own)
                    for (uint32_t z : g_answer)
                        under += z <= g_threshold ? 1u : 0u;
                }
                const float target = static_cast<float>(under) / static_cast<float>(kPixels);
                Eased *e = nullptr;
                for (Eased &c : g_eased)
                    if (c.node == node)
                        e = &c;
                if (!e)
                {
                    e = &g_eased[0];
                    for (Eased &c : g_eased)
                        if (c.tick < e->tick)
                            e = &c;
                    *e = Eased{};
                    e->node = node;
                }
                const uint64_t tick = runtime->eeScheduler().currentVSyncTick();
                if (!e->tick || tick < e->tick || tick - e->tick > 30u || g_easeSeconds <= 0.0f)
                    e->value = target; // (first sight, or back after half a second away: no fade in from an old value)
                else if (tick > e->tick)
                {
                    const float dt = static_cast<float>(tick - e->tick) / 59.94f;
                    e->value += (target - e->value) * (1.0f - std::exp(-dt / g_easeSeconds));
                }
                e->tick = tick;
                if (uint8_t *p = getMemPtr(rdram, node + 0x64u))
                    std::memcpy(p, &e->value, 4);
                g_seen.fraction = target;
            }

            void test(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
            {
                const uint32_t node = getRegU32(ctx, 4);
                g_counted = false;
                g_test(rdram, ctx, runtime);
                if (g_samples && g_on && g_counted && !ps2_host_display::emulatedDrawing())
                    sampleVisibility(rdram, runtime, node);
                ++g_seen.tests;
                g_seen.node = node;
                if (const uint8_t *p = getConstMemPtr(rdram, node + 0x70u))
                    g_seen.onScreen = *p;
                g_seen.visibility = fl(rdram, node + 0x64u);
            }

            void copy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
            {
                const uint32_t source = getRegU32(ctx, 4), dest = getRegU32(ctx, 5), rect = getRegU32(ctx, 6), at = getRegU32(ctx, 7);
                if (dest && dest == word(rdram, kOcclusionBuffer))
                {
                    Copy c;
                    c.tick = runtime->eeScheduler().currentVSyncTick();
                    static uint64_t nextShot = 0u;
                    if (g_shotsLeft && c.tick >= nextShot)
                    {
                        --g_shotsLeft;
                        nextShot = c.tick + 40u;
                        requestShot(c.tick);
                    }
                    for (uint32_t i = 0; i < 4u; ++i)
                        c.rect[i] = fl(rdram, rect + 4u * i);
                    // (TODO 141) The rect's rows: Test scales the projected y by D_006CD82C - 0.5 in the dual-field mode (the
                    // picture's 448 lines as one field's 224 rows), 1.0 otherwise - and this port's frames are not dual-field,
                    // so the rect comes in the picture's 448 lines while the depth it reads holds the scene once, in field
                    // A's 224 rows (the native renderer draws the 3D once, field A's). The probe read row 2y: a sun in the
                    // picture's upper half was tested against whatever lay twice as far down (Hot Shot, the ground) and
                    // vanished, one in the lower half against field B's rows that no 3D draws into (always "visible", even
                    // behind a tree). Halved here for the native probe (the GS copy keeps the game's rect).
                    // TF_NATIVE_OCCFIELD=0: as before.
                    static const bool fieldRows = !(std::getenv("TF_NATIVE_OCCFIELD") && *std::getenv("TF_NATIVE_OCCFIELD") == '0');
                    if (fieldRows && fl(rdram, kScreenYScale) > 0.75f)
                    {
                        c.rect[1] *= 0.5f;
                        c.rect[3] *= 0.5f;
                    }
                    {
                        std::lock_guard<std::mutex> lock(g_copyMutex);
                        g_copies[g_copiesMade++ % g_copies.size()] = c;
                    }
                    if (g_log)
                    {
                        ++g_seen.copies;
                        g_seen.source = source;
                        g_seen.sourceW = word(rdram, source + 0x28u);
                        g_seen.sourceH = word(rdram, source + 0x2Cu);
                        for (uint32_t i = 0; i < 4u; ++i)
                        {
                            g_seen.rect[i] = c.rect[i];
                            g_seen.dest[i] = fl(rdram, at + 4u * i);
                        }
                    }
                }
                g_copy(rdram, ctx, runtime);
            }

            void count(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
            {
                const uint32_t threshold = getRegU32(ctx, 4);
                g_count(rdram, ctx, runtime);
                g_counted = true;
                g_threshold = threshold;
                ++g_seen.counts;
                g_seen.threshold = threshold;
                g_seen.count = getRegU32(ctx, 2);
            }

            void logLine(uint8_t *rdram, PS2Runtime *runtime, uint32_t host, bool gsTransfer)
            {
                const uint64_t tick = runtime->eeScheduler().currentVSyncTick();
                static uint64_t next = 0u;
                if (tick < next)
                    return;
                next = tick + g_log;
                uint32_t lo = 0xFFFFFFu, hi = 0u, zeros = 0u, under = 0u;
                uint64_t sum = 0u;
                const Seen &s = g_seen;
                for (uint32_t i = 0; i < kPixels; ++i)
                {
                    const uint32_t z = word(rdram, host + 4u * i) & 0xFFFFFFu;
                    lo = std::min(lo, z), hi = std::max(hi, z), sum += z, zeros += z == 0u ? 1u : 0u, under += z <= s.threshold ? 1u : 0u;
                }
                // (the native answer beside the GS's: the count its newest frame gives for the same threshold)
                uint32_t nativeUnder = 0u, nlo = 0xFFFFFFu, nhi = 0u;
                uint64_t answerTick = 0u, answers = 0u;
                {
                    std::lock_guard<std::mutex> lock(g_answerMutex);
                    answerTick = g_answerTick;
                    answers = g_answers;
                    for (uint32_t z : g_answer)
                        nativeUnder += z <= s.threshold ? 1u : 0u, nlo = std::min(nlo, z), nhi = std::max(nhi, z);
                }
                std::fprintf(stderr,
                             "[occlusion] tick %llu (%s, %s): %llu tests, %llu copies, %llu counts, %llu transfers, %llu answered natively, %.1f us avg %.1f max | "
                             "last: node %08x on screen %u, copy from %08x (%ux%u) rect %.1f,%.1f-%.1f,%.1f to %.0f,%.0f-%.0f,%.0f, threshold %06x "
                             "count %u of %d, visibility %.3f (samples %.3f) | host z %06x..%06x mean %06llx (%u zero, %u under the threshold) | native frame %llu (%llu answers): "
                             "z %06x..%06x, %u under\n",
                             static_cast<unsigned long long>(tick), ps2_host_display::emulatedDrawing() ? "emulated drawing on" : "native only",
                             gsTransfer ? "GS transfer" : "native answer", static_cast<unsigned long long>(s.tests),
                             static_cast<unsigned long long>(s.copies), static_cast<unsigned long long>(s.counts),
                             static_cast<unsigned long long>(s.transfers), static_cast<unsigned long long>(s.native),
                             (s.transfers + s.native) ? s.transferUs / static_cast<double>(s.transfers + s.native) : 0.0, s.transferMaxUs, s.node, s.onScreen, s.source, s.sourceW, s.sourceH, s.rect[0],
                             s.rect[1], s.rect[2], s.rect[3], s.dest[0], s.dest[1], s.dest[2], s.dest[3], s.threshold, s.count,
                             static_cast<int32_t>(word(rdram, kPixelCount)), s.visibility, s.fraction, lo, hi,
                             static_cast<unsigned long long>(sum / kPixels), zeros, under, static_cast<unsigned long long>(answerTick),
                             static_cast<unsigned long long>(answers), nlo, nhi, nativeUnder);
                g_seen.tests = g_seen.copies = g_seen.counts = g_seen.transfers = g_seen.native = 0u;
                g_seen.transferUs = g_seen.transferMaxUs = 0.0;
            }

            void transfer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
            {
                const uint32_t surface = getRegU32(ctx, 4), host = getRegU32(ctx, 6);
                const bool ours = surface && surface == word(rdram, kOcclusionBuffer) && host == word(rdram, kHostCopy) &&
                                  getConstMemPtr(rdram, host) && getConstMemPtr(rdram, host + 4u * kPixels - 4u);
                if (ours && g_on && !ps2_host_display::emulatedDrawing())
                {
                    // The native renderer draws alone: the newest native answer into the host copy (none yet: left as it
                    // is), no GS transfer. (Returns as the function does: nothing in v0 - its callers read none.)
                    const auto start = std::chrono::steady_clock::now();
                    {
                        std::lock_guard<std::mutex> lock(g_answerMutex);
                        // (only a newer answer than the one written last: most frames none; one copy when the host
                        // copy lies in one piece of guest memory, as it does)
                        static uint64_t written = 0u;
                        if (g_answers != written)
                        {
                            written = g_answers;
                            uint8_t *first = getMemPtr(rdram, host), *last = getMemPtr(rdram, host + 4u * kPixels - 1u);
                            if (first && last == first + 4u * kPixels - 1u)
                                std::memcpy(first, g_answer.data(), 4u * kPixels);
                            else
                                for (uint32_t i = 0; i < kPixels; ++i)
                                    std::memcpy(getMemPtr(rdram, host + 4u * i), &g_answer[i], 4);
                        }
                    }
                    ctx->pc = getRegU32(ctx, 31);
                    if (g_log)
                    {
                        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
                        ++g_seen.native;
                        g_seen.transferUs += us;
                        g_seen.transferMaxUs = std::max(g_seen.transferMaxUs, us);
                        logLine(rdram, runtime, host, false);
                    }
                    return;
                }
                if (!ours || !g_log)
                {
                    g_transfer(rdram, ctx, runtime);
                    return;
                }
                const auto start = std::chrono::steady_clock::now();
                g_transfer(rdram, ctx, runtime);
                const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
                ++g_seen.transfers;
                g_seen.transferUs += us;
                g_seen.transferMaxUs = std::max(g_seen.transferMaxUs, us);
                logLine(rdram, runtime, host, true);
            }

            PS2Runtime::RecompiledFunction wrap(PS2Runtime &runtime, uint32_t address, PS2Runtime::RecompiledFunction fn)
            {
                PS2Runtime::RecompiledFunction original = runtime.lookupFunction(address);
                runtime.registerFunction(address, fn);
                return original;
            }
        }

        void install(PS2Runtime &runtime)
        {
            const char *on = std::getenv("TF_NATIVE_OCCLUSION");
            g_on = !(on && *on == '0');
            if (const char *log = std::getenv("TF_NATIVE_OCCLOG"))
                g_log = static_cast<uint32_t>(std::max(0, std::atoi(log)));
            if (const char *shots = std::getenv("TF_NATIVE_OCCSHOT"))
                g_shotsLeft = static_cast<uint32_t>(std::max(0, std::atoi(shots)));
            if (const char *samples = std::getenv("TF_SUN_SAMPLES"))
                g_samples = *samples != '0';
            if (const char *ease = std::getenv("TF_SUN_EASE"))
                g_easeSeconds = std::max(0.0f, static_cast<float>(std::atof(ease)));
            if (!g_on && !g_log && !g_shotsLeft)
                return;
            for (uint32_t a : {kTest, kCopySurfaceRect, kCount, kTransfer})
                if (!runtime.lookupFunction(a))
                {
                    std::fprintf(stderr, "[occlusion] nothing at %08x to wrap - the sun's test left to the GS\n", a);
                    g_on = false;
                    g_log = 0u;
                    return;
                }
            g_copy = wrap(runtime, kCopySurfaceRect, copy);
            g_transfer = wrap(runtime, kTransfer, transfer);
            if (g_log || (g_on && g_samples))
            {
                g_test = wrap(runtime, kTest, test);
                g_count = wrap(runtime, kCount, count);
            }
        }

        bool probeFor(uint64_t tick, float rect[4])
        {
            if (!g_on && !g_log)
                return false;
            std::lock_guard<std::mutex> lock(g_copyMutex);
            const Copy *best = nullptr;
            for (const Copy &c : g_copies)
                if (c.tick && c.tick <= tick && c.tick > g_lastProbed && (!best || c.tick >= best->tick))
                    best = &c;
            g_lastProbed = tick;
            if (!best)
                return false;
            std::memcpy(rect, best->rect, sizeof(best->rect));
            return true;
        }

        void deliver(uint64_t tick, const float *depth)
        {
            std::array<uint32_t, kPixels> z;
            for (uint32_t i = 0; i < kPixels; ++i)
            {
                const double v = std::isfinite(depth[i]) ? std::max(0.0, static_cast<double>(depth[i])) * 268435456.0 : 0.0;
                z[i] = static_cast<uint32_t>(std::min(v, 16777215.0)); // (the depth is GS Z * 16 / 2^32: Z24 back)
            }
            std::lock_guard<std::mutex> lock(g_answerMutex);
            if (tick < g_answerTick)
                return; // (an older frame's, finished later)
            g_answer = z;
            g_answerTick = tick;
            ++g_answers;
        }
    }
}
