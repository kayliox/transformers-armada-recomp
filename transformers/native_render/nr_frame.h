// nr_frame.h - what the native renderer's frame builder (nr_frame.cpp) and its device (nr_d3d.cpp today, any backend's
// gfx::Device) share besides the device interface (nr_device.h): the frame builder's entry, the layout constants of the
// per-draw data, a few settings, and the profile's counters (TF_NATIVE_PROFILE) - plain numbers, no graphics API.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "nr_draw.h" // draw::Constants
#include "nr_taa.h"  // (TODO 299) taa::upscalerOn: FSR 3 / 4's anti-aliasing and sharpening in place of ours

namespace nr
{
    struct Frame;
    struct Texture;
}

namespace nr::frame
{
    // The frame builder's frame: the draw list turned into the device's calls, its pictures handed to the device's
    // endFrame. Called by the backend's render() on the render thread once its device is up.
    void render(const Frame &input, const std::string &shotPath);
    // A game texture the capture made ahead (d3d::preUploadImpl): handed to the device's uploader at the next frame's start.
    void preUpload(const std::shared_ptr<const Texture> &texture);

    // ---- The layout of the per-draw data the frame builder writes into the device's rings ----
    inline constexpr uint32_t kConstantStride = (sizeof(draw::Constants) + 255u) & ~255u; // a draw's constants (CBV alignment)
    inline constexpr uint32_t kInstanceStride = 208u; // a pass's instance entry: its matrix, q0, the frame before's pose, halfway (IROW0-3, IPZERO, IROWB0-3, IROWM0-3)
    inline constexpr uint32_t kVertexBStride = 20u;   // (N9 stage 3) a vertex's frame-before place, colour and size (POSB, COLORB, PSIZEB)
    inline constexpr uint32_t kMaxPictures = 24u;     // a frame's pictures from one recording (N9 stage 3)

    // ---- Settings (the settings panel changes them live; the d3d:: setters write them) ----
    inline std::atomic<int> g_aa{-1};             // the anti-aliasing (TF_NATIVE_AA): -1 not read yet, 0 off, 1 FXAA, 2 SMAA
    inline std::atomic<int> g_ssaa{-1};           // supersampling: -1 not read yet, then 0/1
    inline std::atomic<int> g_bloom{-1};          // (TODO 108) the modern bloom (TF_NATIVE_BLOOM): -1 not read yet, 0 the game's glow, 1 on
    inline std::atomic<int> g_speedupsForced{-1}; // (TF_NATIVE_SHOTAB: the speed-ups that keep the picture forced on or off)
    inline std::atomic<int> g_windowLines{0};     // the window's height (Linux: setWindowSize; Windows: setWindowLines); 0 = not known yet
    inline std::atomic<int> g_windowWidth{0};     // the window's width (Linux: setWindowSize); 0 = not known yet
    inline uint32_t g_profWindow = 0;             // TF_NATIVE_PROFILE's windows so far (the "=alt" switches turn on it)
    // (TODO 94: the speed-ups that keep the picture, each TF_NATIVE_<name>=0 off, =alt by turns (a profile window
    // each), unset on; TF_NATIVE_SHOTAB's forcing over them.)
    inline bool speedupOn(const char *env)
    {
        const int forced = g_speedupsForced.load(std::memory_order_relaxed);
        if (forced >= 0)
            return forced != 0;
        return !(env && (*env == '0' || (*env == 'a' && (g_profWindow & 1u))));
    }
    // (TF_NATIVE_AA: 0 off, 2 or "smaa" SMAA 1x, anything else - unset too - FXAA.)
    inline int aaMode()
    {
        int v = g_aa.load(std::memory_order_relaxed);
        if (v < 0)
        {
            const char *e = std::getenv("TF_NATIVE_AA");
            int expected = -1;
            g_aa.compare_exchange_strong(expected, e && *e == '0' ? 0 : e && (*e == '2' || *e == 's' || *e == 'S') ? 2 : 1);
            v = g_aa.load(std::memory_order_relaxed);
        }
        return v;
    }
    // (TODO 299: while FSR 3 / 4 runs, its own anti-aliasing replaces the edge filter)
    inline bool aaOn() { return aaMode() != 0 && !taa::upscalerOn(); }
    // (TODO 108, TF_NATIVE_BLOOM: 1 the modern bloom in place of the game's glow; unset or 0 the game's own, the faithful look;
    // 2 neither - the glow left out, to measure what the game's adds.)
    inline int bloomMode()
    {
        int v = g_bloom.load(std::memory_order_relaxed);
        if (v < 0)
        {
            const char *e = std::getenv("TF_NATIVE_BLOOM");
            int expected = -1;
            g_bloom.compare_exchange_strong(expected, e && *e == '1' ? 1 : e && *e == '2' ? 2 : 0); // (2, a checking aid: no glow at all)
            v = g_bloom.load(std::memory_order_relaxed);
        }
        return v;
    }
    inline void setBloom(int mode) { g_bloom.store(mode ? 1 : 0, std::memory_order_relaxed); }
    // (TODO 108 (2), TF_NATIVE_TONEMAP: 1 the filmic tone mapping over the 3D; unset or 0 none, the faithful look.)
    inline std::atomic<int> g_tone{-1};
    inline int toneMode()
    {
        int v = g_tone.load(std::memory_order_relaxed);
        if (v < 0)
        {
            const char *e = std::getenv("TF_NATIVE_TONEMAP");
            int expected = -1;
            g_tone.compare_exchange_strong(expected, e && *e == '1' ? 1 : 0);
            v = g_tone.load(std::memory_order_relaxed);
        }
        return v;
    }
    inline void setTone(int mode) { g_tone.store(mode ? 1 : 0, std::memory_order_relaxed); }
    // (TODO 108) The two looks' strengths, the F1 sliders: the bloom's 0..2 (1 = the game's glow matched; above, brighter and
    // wider - the threshold lowered too), TF_NATIVE_BLOOM_STRENGTH; the tone mapping's 0..1 (1 = the whole curve),
    // TF_NATIVE_TONE_STRENGTH. Read from the environment at first use, then set live.
    inline constexpr float kBloomStrengthDefault = 1.5f, kToneStrengthDefault = 0.6f;
    inline std::atomic<float> g_bloomStrength{-1.0f}, g_toneStrength{-1.0f};
    inline float strengthOf(std::atomic<float> &v, const char *env, float fallback, float most)
    {
        float s = v.load(std::memory_order_relaxed);
        if (s < 0.0f)
        {
            const char *e = std::getenv(env);
            float expected = -1.0f;
            v.compare_exchange_strong(expected, e && *e ? std::clamp(static_cast<float>(std::atof(e)), 0.0f, most) : fallback);
            s = v.load(std::memory_order_relaxed);
        }
        return s;
    }
    inline float bloomStrength() { return strengthOf(g_bloomStrength, "TF_NATIVE_BLOOM_STRENGTH", kBloomStrengthDefault, 2.0f); }
    inline float toneStrength() { return strengthOf(g_toneStrength, "TF_NATIVE_TONE_STRENGTH", kToneStrengthDefault, 1.0f); }
    inline void setBloomStrength(float s) { g_bloomStrength.store(std::clamp(s, 0.0f, 2.0f), std::memory_order_relaxed); }
    inline void setToneStrength(float s) { g_toneStrength.store(std::clamp(s, 0.0f, 1.0f), std::memory_order_relaxed); }
    // (TODO 108 (3) C1, TF_NATIVE_CLARITY: 1 the local contrast over the 3D; unset or 0 none. Its strength, the F1 slider,
    // 0..1: TF_NATIVE_CLARITY_STRENGTH.)
    inline std::atomic<int> g_clarity{-1};
    inline int clarityMode()
    {
        int v = g_clarity.load(std::memory_order_relaxed);
        if (v < 0)
        {
            const char *e = std::getenv("TF_NATIVE_CLARITY");
            int expected = -1;
            g_clarity.compare_exchange_strong(expected, e && *e == '1' ? 1 : 0);
            v = g_clarity.load(std::memory_order_relaxed);
        }
        return v;
    }
    inline void setClarity(int mode) { g_clarity.store(mode ? 1 : 0, std::memory_order_relaxed); }
    inline constexpr float kClarityStrengthDefault = 0.5f;
    inline std::atomic<float> g_clarityStrength{-1.0f};
    inline float clarityStrength() { return strengthOf(g_clarityStrength, "TF_NATIVE_CLARITY_STRENGTH", kClarityStrengthDefault, 1.0f); }
    inline void setClarityStrength(float s) { g_clarityStrength.store(std::clamp(s, 0.0f, 1.0f), std::memory_order_relaxed); }
    // The render scale (TF_NATIVE_RENDER_SCALE, the F1 setting, 25..100, default 100): the picture drawn with that share of
    // the window's lines and shown enlarged to it - for graphics cards that cannot fill the screen's pixels at the game's
    // rate. TF_NATIVE_HEIGHT, when set, still fixes the lines. Read from the environment at first use, then set live.
    inline std::atomic<int> g_renderScale{-1};
    inline int renderScale()
    {
        int v = g_renderScale.load(std::memory_order_relaxed);
        if (v < 0)
        {
            const char *e = std::getenv("TF_NATIVE_RENDER_SCALE");
            int expected = -1;
            g_renderScale.compare_exchange_strong(expected, e && *e ? std::clamp(std::atoi(e), 25, 100) : 100);
            v = g_renderScale.load(std::memory_order_relaxed);
        }
        return v;
    }
    // (TODO 295) The fewest lines a picture under render scale 100% is drawn with (TF_NATIVE_MIN_LINES; 0, the default: no
    // floor): built-in graphics start at 67% with 480 (graphics_driver::applyLowEndDefaults - the PS2 shows 448), so a small
    // window is never softer than the console's. Only while that scale is the default: the player's own choice of a render
    // scale (setRenderScale, from the setting) takes it as it is.
    inline std::atomic<int> g_minLines{-1};
    inline int minLines()
    {
        int v = g_minLines.load(std::memory_order_relaxed);
        if (v < 0)
        {
            const char *e = std::getenv("TF_NATIVE_MIN_LINES");
            int expected = -1;
            g_minLines.compare_exchange_strong(expected, e && *e ? std::clamp(std::atoi(e), 0, 4320) : 0);
            v = g_minLines.load(std::memory_order_relaxed);
        }
        return v;
    }
    inline void setRenderScale(int percent)
    {
        g_renderScale.store(std::clamp(percent, 25, 100), std::memory_order_relaxed);
        g_minLines.store(0, std::memory_order_relaxed);
    }
    // (TODO 299) The render scale as FSR 3 / 4's presets mean it: Native AA 1, Quality 1 / 1.5, Balanced 1 / 1.7, Performance
    // 1 / 2, Ultra Performance 1 / 3 - the settings' 100, 67, 58, 50 and 33 - the share exactly (AMD's ratios); any other percent
    // as it is.
    inline float renderRatio(int percent)
    {
        switch (percent)
        {
        case 67: return 1.0f / 1.5f;
        case 58: return 1.0f / 1.7f;
        case 50: return 0.5f;
        case 33: return 1.0f / 3.0f;
        default: return static_cast<float>(percent) / 100.0f;
        }
    }
    // The sharpening (TF_NATIVE_SHARPEN, the F1 setting, live): 0 off, 1 auto (the default: only below render scale 100%,
    // where the picture is enlarged and comes out soft), 2 on. Drawn by the FXAA pass (TF_NATIVE_AA's default), as the
    // amount CAS takes: 0 none - the pass's result as before.
    inline std::atomic<int> g_sharpen{-1};
    inline int sharpenMode()
    {
        int v = g_sharpen.load(std::memory_order_relaxed);
        if (v < 0)
        {
            const char *e = std::getenv("TF_NATIVE_SHARPEN");
            int expected = -1;
            g_sharpen.compare_exchange_strong(expected, !e || !*e || std::strcmp(e, "auto") == 0 ? 1 : *e == '0' ? 0 : 2);
            v = g_sharpen.load(std::memory_order_relaxed);
        }
        return v;
    }
    inline void setSharpen(int mode) { g_sharpen.store(std::clamp(mode, 0, 2), std::memory_order_relaxed); }
    // (TODO 295) The window's upscaler (TF_NATIVE_UPSCALER, the F1 setting, live): 0 bilinear (the window's stretch, as
    // before; the default until the CPU-path pacing is fixed and FSR's cost re-measured - your call, 2026-11-12), 1 auto
    // (FSR 1 whenever the picture is smaller than the window), 2 FSR 1. nr_present.cpp draws
    // it (fsr::draw) only when the picture is enlarged - at 100% nothing changes.
    inline std::atomic<int> g_upscaler{-1};
    inline int upscalerMode()
    {
        int v = g_upscaler.load(std::memory_order_relaxed);
        if (v < 0)
        {
            const char *e = std::getenv("TF_NATIVE_UPSCALER");
            int expected = -1;
            // (fsr: FSR 3 / 4, the temporal upscaler - nr_taa.h; the picture is then at the window's lines, FSR 1 never needed)
            g_upscaler.compare_exchange_strong(expected, !e || !*e || std::strcmp(e, "bilinear") == 0 || std::strcmp(e, "fsr") == 0 || *e == '0' ? 0
                                                         : std::strcmp(e, "auto") == 0                                                  ? 1
                                                                                                                                         : 2);
            v = g_upscaler.load(std::memory_order_relaxed);
        }
        return v;
    }
    inline void setUpscaler(int mode) { g_upscaler.store(std::clamp(mode, 0, 2), std::memory_order_relaxed); }
    inline bool fsrOn() { return upscalerMode() != 0; }
    inline float sharpenAmount()
    {
        // (TODO 295: while FSR 1 enlarges the picture its RCAS sharpens it - PR #23's CAS here would sharpen it twice; TODO 299:
        // FSR 3 / 4 sharpens with its own too - taaSharpness)
        if ((fsrOn() && renderScale() < 100) || taa::upscalerOn())
            return 0.0f;
        const int mode = sharpenMode();
        return mode == 2 || (mode == 1 && renderScale() < 100) ? 0.5f : 0.0f;
    }

    // ---- The game textures' mip chains (the frame builder's, and the D3D12 device's own uploads) ----
    void halveLevel(const uint32_t *src, uint32_t pw, uint32_t ph, uint32_t *dst);
    void mipChain(uint32_t width, uint32_t height, const uint32_t *pixels, const std::vector<std::vector<uint32_t>> *made,
                  std::vector<const uint32_t *> &mips, std::vector<std::vector<uint32_t>> &computed);

    // ---- The profile's counters (TF_NATIVE_PROFILE), counted by both sides, printed and reset every window ----
    // (per-frame counts of what may serialise the GPU's work: barriers, pipeline changes, the switches between pipelines
    // with and without the clipping shader, target changes, draws, target copies - TODO 94)
    inline uint64_t g_cntBarriers = 0, g_cntPso = 0, g_cntGsSwitch = 0, g_cntTargets = 0, g_cntDraws = 0, g_cntIndices = 0, g_cntGsIndices = 0;
    inline uint64_t g_cntCopies = 0, g_cntScissors = 0, g_cntFactors = 0;
    inline uint64_t g_picturesMade = 0;                 // the picture's surfaces made (again)
    inline double g_frameGpu = 0.0, g_slotWaitMs = 0.0; // the whole frame on the GPU; the render thread waiting for a frame slot
    inline uint32_t g_frameGpuFrames = 0;
    inline double g_releaseMs = 0.0;                    // this frame's releases on the render thread (the slow frames' line)
    inline double g_mkMs[5] = {};                       // a texture's making: mip chain, texture, staging, copy-in, record (ms)
    inline uint64_t g_mkTexels = 0;
    inline std::atomic<uint64_t> g_largeBytes{0};       // the large images' video memory in all (largeImageBytes)
}
