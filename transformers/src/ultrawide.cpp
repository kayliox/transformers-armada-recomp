// Ultrawide (ultrawide.h has the long form, with where every address comes from).

#include "ultrawide.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_settings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <tuple>
#include <vector>
#include "game_version.h" // (TF_GA: the code addresses that differ in the PAL release)

namespace ultrawide
{
    namespace
    {
        constexpr float kGameWide = 16.0f / 9.0f; // the game's own widescreen shape: the data's base
        constexpr float kMaxShape = 2.4f;
        constexpr uint32_t kWidescreenByte = 0x006CF004u; // the renderer's copy of the WIDESCREEN option
        constexpr uint32_t kFourThirds = 0x006CB1FCu;     // the 4/3 the pixel aspect is multiplied by in widescreen
        constexpr uint32_t kFourThirdsBits = 0x3FAAAAABu;
        constexpr uint32_t kDisplayModeAspect = 0x0070F490u; // the display mode's pixel aspect (its info at 0x70F480, +0x10)

        constexpr uint32_t kGetFieldOfView = 0x001F2840u;     // vlCamera::GetFieldOfView: degrees in $f0
        constexpr uint32_t kGetTanHalfFov = 0x001F2830u;      // vlCamera::GetTanHalfFieldOfView: tan(fov/2) in $f0
        constexpr uint32_t kGetDisplayModeInfo = 0x0025DD50u; // vlRenderDevicePS2::GetDisplayModeInfo (info by value through a0)
        constexpr uint32_t kCameraSpaceFrustum = 0x001F4B00u; // vlCameraSpaceFrustum
        constexpr uint32_t kBuildFrustum = 0x0020AB70u;       // vlSceneDisplayUtil::BuildFrustum

        // The calls (jal / jalr addresses) whose result is widened, and the callers that decide it
        // for the calls made inside the two frustum builders.
        constexpr uint32_t kFovCalls[] = {0x0020C7E4u, 0x0020B570u, TF_GA(0x00358F38u), TF_GA(0x003542C8u), 0x001BFB04u,
                                          0x002B8B2Cu, 0x002B8E34u, 0x002B8E50u, 0x002B8EA0u};
        // Both frustum builders take their HORIZONTAL half-angle from the field of view and the
        // vertical one from it times the display mode's pixel aspect (vlCameraSpaceFrustum: +8/+C
        // and +10/+14; BuildFrustum: +A0 tan and +A4 aspect), as the projection does (0x20CA00).
        // In widescreen the game keeps the pixel aspect divided by the shape (frame() below), so a
        // frustum is widened like the projection: its field of view, the aspect left as stored (the
        // vertical then stays the same). Widening the aspect instead (until 2026-10-22) only made
        // the culling frustums taller: they culled at 16:9 width, and at 21:9 objects at the
        // screen's sides went missing (measured: the scene traversal's plane at 22.5 degrees with
        // the picture showing 29).
        constexpr uint32_t kFovInCameraFrustum = 0x001F4B38u;
        constexpr uint32_t kCameraFrustumForFov[] = {0x002B8A1Cu,  // taRenderingInfo (effects, particles)
                                                     0x002191A4u,  // the scene traversal (culling)
                                                     TF_GA(0x0042A868u)}; // the eco code (culling)
        constexpr uint32_t kCameraFrustumForCulling[] = {0x002191A4u, TF_GA(0x0042A868u)};
        constexpr uint32_t kFovInBuildFrustum = 0x0020AC28u;
        constexpr uint32_t kBuildFrustumForFov[] = {0x00270618u, 0x00273F64u, TF_GA(0x004368C4u), TF_GA(0x00564E4Cu), // water, sky, eco globe, rain
                                                    0x002B3FD0u, 0x002B40B0u, TF_GA(0x005BAFA8u),              // in-view tests
                                                    TF_GA(0x004296F4u), TF_GA(0x0042E42Cu)};                          // the eco code (culling)
        constexpr uint32_t kBuildFrustumForCulling[] = {TF_GA(0x004296F4u), TF_GA(0x0042E42Cu)}; // the eco code's (trees, plants)
        constexpr uint32_t kTanCall = TF_GA(0x0042E3BCu);                                  // ecoQuadtree::Render
        constexpr uint32_t kAspectInBuildFrustum = 0x0020AC04u;
        constexpr uint32_t kAspectInCameraFrustum = 0x001F4BB0u;

        template <size_t N>
        bool contains(const uint32_t (&list)[N], uint32_t value)
        {
            return std::find(std::begin(list), std::end(list), value) != std::end(list);
        }

        std::atomic<float> g_chosen{0.0f}; // TF_ASPECT (0: the window's shape, the default)
        std::atomic<float> g_fovFactor{1.0f};   // TF_FOV: the view's tan(fov/2) times this (1: the game's)
        std::atomic<bool> g_inHq{false};        // the HQ: no TF_FOV there (setInHq) unless g_hqFov
        // TODO 96, TF_HQ_FOV ("Field of view in the HQ"): the HQ's scene at the Field of view setting too (on), or at the
        // game's own as before (off, the default: item 52 kept the ring band's framing that way).
        std::atomic<bool> g_hqFov{false};
        // The HQ's menu panels (TODO 53): Options, Controller, Audio and the like are taScreenInGMenu
        // screens the HQ renders onto a hologram panel in its 3D scene (renderToSurface), drawn at a
        // fixed size in the middle - about 0.7 of the picture's height. "Larger" (TF_HQ_PANELS, the
        // default) zooms the HQ's camera in while one is up, so the panel - its box, bars and text
        // together, all one picture on the panel - fills about 0.9 of the height, at every window
        // shape (Hor+: the height is the picture's measure); eased in and out over a fifth of a second.
        constexpr float kPanelZoom = 0.78f; // the view's tan(fov/2) times this while a panel is up
        std::atomic<bool> g_panelsLarger{true};
        std::atomic<float> g_panelZoom{1.0f}; // now (notePanel eases it)
        float fovFactor()
        {
            const float zoom = g_panelZoom.load(std::memory_order_relaxed);
            float fov = g_fovFactor.load(std::memory_order_relaxed);
            if (g_inHq.load(std::memory_order_relaxed))
            {
                // In the HQ: the game's own (off), or the setting's - given back to the game's own as a menu panel's
                // zoom comes in, so a panel fills the picture as it does with the switch off (its share of the way in).
                const float panel = std::clamp((1.0f - zoom) / (1.0f - kPanelZoom), 0.0f, 1.0f);
                fov = g_hqFov.load(std::memory_order_relaxed) ? fov + (1.0f - fov) * panel : 1.0f;
            }
            return fov * zoom;
        }
        std::atomic<float> g_cullMargin{1.15f}; // TF_CULL_MARGIN: the culling frustums' extra width
        std::atomic<int> g_windowW{0}, g_windowH{0};
        std::atomic<float> g_k{1.0f}; // shape / 16:9 while the game draws wide, else 1 (the game thread's frame())

        PS2Runtime::RecompiledFunction g_fov = nullptr, g_tan = nullptr, g_displayInfo = nullptr, g_cameraFrustum = nullptr,
                                       g_buildFrustum = nullptr;
        // The call each frustum builder was last entered from (its jal's address).
        uint32_t g_cameraFrustumFrom = 0u, g_buildFrustumFrom = 0u;

        float parseShape(const char *text)
        {
            if (!text || !*text)
                return kGameWide;
            const std::string s = text;
            if (s == "screen" || s == "window")
                return 0.0f;
            float w = 0.0f, h = 0.0f;
            if (std::sscanf(s.c_str(), "%f:%f", &w, &h) == 2 && w > 0.0f && h > 0.0f)
                return w / h;
            const float ratio = static_cast<float>(std::atof(s.c_str()));
            return ratio > 0.0f ? ratio : kGameWide;
        }

        float wideK() { return g_k.load(std::memory_order_relaxed); }

        // Testing aids. TF_ULTRAWIDE_CALLERS=1: each distinct caller of the hooked functions, once,
        // with whether it is widened (the list to look through for culling the shape misses).
        // TF_UW_WIDEN=addr,addr,...: more calls (jal addresses, hex) to widen: a GetFieldOfView or
        // GetTanHalfFieldOfView call, or a frustum builder's call (its aspect then widened).
        std::vector<uint32_t> g_extra;
        bool extra(uint32_t call) { return std::find(g_extra.begin(), g_extra.end(), call) != g_extra.end(); }
        void noteCaller(const char *what, uint32_t call, uint32_t from, bool widened)
        {
            static const bool on = []()
            {
                const char *env = std::getenv("TF_ULTRAWIDE_CALLERS");
                return env && *env && *env != '0';
            }();
            if (!on || wideK() == 1.0f)
                return;
            static std::set<std::tuple<std::string, uint32_t, uint32_t>> seen;
            if (seen.insert({what, call, from}).second)
                std::fprintf(stderr, "[ultrawide] caller %s at %08x (frustum from %08x): %s\n", what, call, from,
                             widened ? "widened" : "NOT widened");
        }

        void fovHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t call = GPR_U32(ctx, 31) - 8u;
            g_fov(rdram, ctx, runtime); // (a leaf: it returns)
            const uint32_t from = call == kFovInCameraFrustum ? g_cameraFrustumFrom : (call == kFovInBuildFrustum ? g_buildFrustumFrom : 0u);
            const bool inCamera = call == kFovInCameraFrustum, inBuild = call == kFovInBuildFrustum;
            const bool widen = contains(kFovCalls, call) || extra(call) || (from && extra(from)) ||
                               (inCamera && contains(kCameraFrustumForFov, from)) || (inBuild && contains(kBuildFrustumForFov, from));
            // The culling frustums of the scene traversal and the eco code get a margin on top (its
            // vertical too, through the aspect: see the lists): the picture shown is often a moment
            // between two frames (in-between pictures), turned further than the frame the scene was
            // culled for, so while the camera turns objects just past the edge would go missing at
            // the screen's sides. TF_CULL_MARGIN (1.15; 1 = none).
            const bool culling = (inCamera && contains(kCameraFrustumForCulling, from)) || (inBuild && contains(kBuildFrustumForCulling, from));
            const float k = wideK() * fovFactor() *
                            (culling ? g_cullMargin.load(std::memory_order_relaxed) : 1.0f);
            noteCaller("GetFieldOfView", call, from, widen);
            if (!widen || k == 1.0f)
                return;
            constexpr float kRad = 3.14159265358979f / 180.0f;
            const float half = ctx->f[0] * 0.5f * kRad;
            if (half > 0.0f && half < 1.55f)
                ctx->f[0] = 2.0f * std::atan(k * std::tan(half)) / kRad;
        }

        void tanHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t call = GPR_U32(ctx, 31) - 8u;
            g_tan(rdram, ctx, runtime); // (a leaf)
            const float k = wideK() * fovFactor();
            const bool widen = call == kTanCall || extra(call);
            noteCaller("GetTanHalfFieldOfView", call, 0u, widen);
            if (k != 1.0f && widen)
                ctx->f[0] *= k;
        }

        void displayInfoHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t returnAddress = GPR_U32(ctx, 31);
            const uint32_t call = returnAddress - 8u;
            const uint32_t info = GPR_U32(ctx, 4);
            g_displayInfo(rdram, ctx, runtime);
            if (ctx->pc != returnAddress)
                return; // it stopped at its copy loop's back edge for the scheduler: this one call keeps the aspect
            // The aspect is no longer widened anywhere (the frustums take the field of view: see the
            // lists); kept for TF_UW_WIDEN experiments and the caller log.
            const uint32_t from = call == kAspectInCameraFrustum ? g_cameraFrustumFrom : (call == kAspectInBuildFrustum ? g_buildFrustumFrom : 0u);
            const bool scale = extra(call) || (from && extra(from));
            noteCaller("GetDisplayModeInfo", call, from, scale);
            const float k = wideK();
            if (k == 1.0f || !scale)
                return;
            if (uint8_t *p = getMemPtr(rdram, info + 0x10u))
            {
                float aspect;
                std::memcpy(&aspect, p, sizeof(aspect));
                aspect *= k;
                std::memcpy(p, &aspect, sizeof(aspect));
            }
        }

        void cameraFrustumHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t from = GPR_U32(ctx, 31) - 8u;
            const uint32_t frustum = GPR_U32(ctx, 4);
            g_cameraFrustumFrom = from;
            g_cameraFrustum(rdram, ctx, runtime);
            // TF_ULTRAWIDE_TRACE: the scene traversal's frustum as built (its two planes' sin/cos:
            // +8/+C from the field of view, +10/+14 from it and the aspect), now and then.
            static const bool trace = std::getenv("TF_ULTRAWIDE_TRACE") != nullptr;
            static uint64_t calls = 0u;
            if (trace && from == 0x002191A4u && (calls++ % 600u) == 0u)
                if (const uint8_t *p = getConstMemPtr(rdram, frustum + 8u))
                {
                    float v[4];
                    std::memcpy(v, p, sizeof(v));
                    constexpr float kDeg = 180.0f / 3.14159265f;
                    std::fprintf(stderr, "[ultrawide]   scene frustum: half-angle A %.2f deg, B %.2f deg\n",
                                 std::atan2(v[0], v[1]) * kDeg, std::atan2(v[2], v[3]) * kDeg);
                }
        }

        void buildFrustumHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_buildFrustumFrom = GPR_U32(ctx, 31) - 8u;
            g_buildFrustum(rdram, ctx, runtime);
        }

        template <class F>
        void hook(PS2Runtime &runtime, uint32_t address, PS2Runtime::RecompiledFunction &original, F fn)
        {
            original = runtime.lookupFunction(address);
            if (original)
                runtime.registerFunction(address, fn);
            else
                std::fprintf(stderr, "[ultrawide] no function at %08x: not hooked\n", address);
        }
    }

    void install(PS2Runtime &runtime)
    {
        if (const char *aspect = std::getenv("TF_ASPECT"); aspect && *aspect)
            g_chosen.store(parseShape(aspect)); // (else the screen's shape: settings.ini may still choose)
        if (const char *fov = std::getenv("TF_FOV"); fov && *fov)
            g_fovFactor.store(std::clamp(static_cast<float>(std::atof(fov)), 0.5f, 2.0f));
        for (const char *p = std::getenv("TF_UW_WIDEN"); p && *p;)
        {
            char *end = nullptr;
            const unsigned long value = std::strtoul(p, &end, 16);
            if (end == p)
                break;
            g_extra.push_back(static_cast<uint32_t>(value));
            p = *end == ',' ? end + 1 : end;
        }
        if (const char *hq = std::getenv("TF_HQ_FOV"); hq && *hq)
            g_hqFov.store(std::strcmp(hq, "0") != 0);
        if (const char *margin = std::getenv("TF_CULL_MARGIN"); margin && *margin)
            g_cullMargin.store(std::clamp(static_cast<float>(std::atof(margin)), 0.5f, 2.0f)); // (below 1: a test that the margin takes)
        hook(runtime, kGetFieldOfView, g_fov, fovHook);
        hook(runtime, kGetTanHalfFov, g_tan, tanHook);
        hook(runtime, kGetDisplayModeInfo, g_displayInfo, displayInfoHook);
        hook(runtime, kCameraSpaceFrustum, g_cameraFrustum, cameraFrustumHook);
        hook(runtime, kBuildFrustum, g_buildFrustum, buildFrustumHook);
    }

    void setInHq(bool inHq) { g_inHq.store(inHq, std::memory_order_relaxed); }

    void notePanel(bool shown)
    {
        using clock = std::chrono::steady_clock;
        static clock::time_point last = clock::now();
        const clock::time_point now = clock::now();
        const float dt = std::min(0.1f, std::chrono::duration<float>(now - last).count());
        last = now;
        const bool on = shown && g_inHq.load(std::memory_order_relaxed) && g_panelsLarger.load(std::memory_order_relaxed);
        const float target = on ? kPanelZoom : 1.0f;
        float zoom = g_panelZoom.load(std::memory_order_relaxed);
        const float step = (1.0f - kPanelZoom) * dt / 0.2f; // (the whole way in a fifth of a second)
        zoom = zoom < target ? std::min(target, zoom + step) : std::max(target, zoom - step);
        g_panelZoom.store(zoom, std::memory_order_relaxed);
    }

    void noteWindow(int width, int height)
    {
        g_windowW.store(width, std::memory_order_relaxed);
        g_windowH.store(height, std::memory_order_relaxed);
    }

    float shape()
    {
        float s = g_chosen.load(std::memory_order_relaxed);
        if (s == 0.0f)
        {
            const int w = g_windowW.load(std::memory_order_relaxed), h = g_windowH.load(std::memory_order_relaxed);
            s = h > 0 ? static_cast<float>(w) / static_cast<float>(h) : kGameWide;
        }
        return std::clamp(s, kGameWide, kMaxShape);
    }

    void frame(uint8_t *rdram)
    {
        const uint8_t *wide = getConstMemPtr(rdram, kWidescreenByte);
        const float k = (wide && *wide != 0u) ? shape() / kGameWide : 1.0f;
        g_k.store(k, std::memory_order_relaxed);

        // The 4/3 constant: written while it holds the original or what was written last. The game
        // reads it only when it works out a display mode (ConvertDisplayModeIndexToInfo, 0x25E180:
        // the TV's pixel aspect x 4/3 in widescreen, the mode's pixel aspect divided by that), at a
        // level's start or a change of the option.
        static uint32_t lastWritten = kFourThirdsBits;
        static float constantK = 1.0f; // the k the constant holds
        uint8_t *p = getMemPtr(rdram, kFourThirds);
        if (!p)
            return;
        uint32_t current;
        std::memcpy(&current, p, sizeof(current));
        const bool ours = current == kFourThirdsBits || current == lastWritten;

        // So the display mode's pixel aspect (0x70F490) is kept at the shape too: a mode worked out
        // since the last frame holds the constant's k; a change of shape since then is made there
        // at once, so it shows without a new level.
        static uint32_t infoWritten = 0u;
        static float infoK = 1.0f; // the k the stored pixel aspect holds
        if (wide && *wide != 0u && ours)
        {
            if (uint8_t *info = getMemPtr(rdram, kDisplayModeAspect))
            {
                uint32_t bits;
                std::memcpy(&bits, info, sizeof(bits));
                if (bits != infoWritten)
                    infoK = constantK; // (the game worked the mode out again)
                if (infoK != k)
                {
                    float aspect;
                    std::memcpy(&aspect, &bits, sizeof(aspect));
                    aspect = aspect * infoK / k;
                    std::memcpy(&bits, &aspect, sizeof(bits));
                    std::memcpy(info, &bits, sizeof(bits));
                    infoK = k;
                }
                infoWritten = bits;
            }
        }

        if (!ours)
            return;
        float value = (4.0f / 3.0f) * k;
        uint32_t bits = kFourThirdsBits;
        if (k > 1.0f)
            std::memcpy(&bits, &value, sizeof(bits));
        if (bits != current)
            std::memcpy(p, &bits, sizeof(bits));
        lastWritten = bits;
        constantK = k > 1.0f ? k : 1.0f;
    }

    void addSettings()
    {
        ps2_settings::Setting s;
        s.key = "TF_ASPECT";
        s.section = "Graphics";
        s.label = "Picture shape";
        s.order = 31;
        s.help = "With Widescreen on: the game's own 16:9, or wider (hor+: more at the sides, the HUD at the screen's "
                 "edges). 'The screen's' follows the window, up to 2.4:1. Movies stay 4:3.";
        s.choices = {{"16:9", "16:9 (the game's)"}, {"21:9", "21:9"}, {"screen", "The screen's"}, {"2.4", "2.4:1 (the widest)"}};
        s.defaultValue = "screen"; // (the window's shape: a 16:9 window stays 16:9, an ultrawide one fills)
        s.apply = [](const std::string &value)
        {
            g_chosen.store(parseShape(value.c_str()));
            return true; // (in effect at the game's next frame)
        };
        ps2_settings::add(s);

        // The field of view: the view's tan(fov/2) at the picture's calls (projection and the
        // culling frustums, the same calls the shape widens; the camera's own value, which its
        // code reads and writes back each frame, stays the game's).
        ps2_settings::Setting f;
        f.key = "TF_FOV";
        f.section = "Graphics";
        f.label = "Field of view";
        f.order = 32;
        f.help = "How much the view takes in, in every camera (the aiming view's zoom keeps its share). "
                 "Normal is the game's.";
        f.choices = {{"0.9", "Narrower (90%)"}, {"1", "Normal"},       {"1.1", "Wider (110%)"}, {"1.2", "Wider (120%)"},
                     {"1.3", "Wider (130%)"},   {"1.4", "Widest (140%)"}};
        f.defaultValue = "1";
        f.apply = [](const std::string &value)
        {
            const float factor = static_cast<float>(std::atof(value.c_str()));
            g_fovFactor.store(factor > 0.0f ? std::clamp(factor, 0.5f, 2.0f) : 1.0f);
            return true;
        };
        ps2_settings::add(f);

        // TODO 96: the Field of view in the HQ as well (off: the game's own there, as before).
        ps2_settings::Setting h;
        h.key = "TF_HQ_FOV";
        h.section = "Graphics";
        h.label = "Field of view in the HQ";
        h.order = 32; // (right after Field of view: the same place, added after it)
        h.help = "On: the HQ's scene uses the Field of view setting too. Off: the HQ keeps the game's own field of view "
                 "(the ring and its band framed as on the console). The menu panels fill the picture the same either way.";
        h.choices = {{"0", "Off"}, {"1", "On"}};
        h.defaultValue = "0";
        h.apply = [](const std::string &value)
        {
            g_hqFov.store(value == "1");
            return true;
        };
        ps2_settings::add(h);

        // The HQ's menu panels (TODO 53, notePanel).
        if (const char *env = std::getenv("TF_HQ_PANELS"); env && *env)
            g_panelsLarger.store(std::strcmp(env, "original") != 0);
        ps2_settings::Setting p;
        p.key = "TF_HQ_PANELS";
        p.section = "Display";
        p.label = "HQ menu panels";
        p.order = 33;
        p.help = "The HQ's hologram panels (Options, Controller, Audio...): larger, the view closing in on the panel "
                 "while it is up, or at the original size.";
        p.choices = {{"larger", "Larger"}, {"original", "Original size"}};
        p.defaultValue = "larger";
        p.apply = [](const std::string &value)
        {
            g_panelsLarger.store(value != "original");
            return true;
        };
        ps2_settings::add(p);
    }
}
