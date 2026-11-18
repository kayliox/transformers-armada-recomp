// nr_core S12: the recomp's "core" overlay host (nr_overlay_host.h). Glue, used only under the developer switch TF_NATIVE_SWAPCHAIN=1
// (nr_devswap.cpp): the core's swap chain presents into a CHILD window over the game window, which keeps all the input, so the
// ImGui overlays keep rlImGui's whole platform side (its setup, fonts, style, raylib input read once a picture - the F1 menu's own
// workarounds for that stay on: feedsInput false) and rlImGui still draws them into the GL window (shown whenever the child hides:
// the start's black, a movie read from GS memory, the emulated drawing). The only change: each frame, after ImGui::Render() and
// before rlImGui's renderer answers ImGui's texture requests, the module nr_core_imgui copies it (Textures::Shared: the textures
// stay rlImGui's, the module keeps copies of their pixels), and the devswap presents each picture with the frames copied since -
// drawn by the module's Direct3D 12 renderer into the core's back buffer over the picture.
// nr_core S13a: the window's raylib overlays too (the frame counter, the notices, the settings panel, the small crosshair, the
// distance meter, the objective arrow, the REC mark, HD movies - whatever the window draws after its picture): the runtime draws
// them into layers (ps2_host_present::setOverlayLayers) - the core's shared layer textures (nr_core::layer), imported into GL -
// and draws each layer into the GL window as it closes (premultiplied: the GL window looks as before); each closed layer goes
// into the module's frames at its place (nr_core_imgui::insert(nr_core::layerOverlay)), so the core draws the same layers between
// the same ImGui frames, in the GL window's order. TF_NATIVE_SWAPCHAIN_LAYERS=0: not done (S12: the raylib overlays in the GL
// window alone).
#include "nr_overlay_host.h"

#if defined(PS2X_ENABLE_DEBUG_UI) && defined(_WIN32)

#include "nr_devswap.h"
#include "nr_glshare.h"
#include "overlay/nr_core_imgui.h"

#include "imgui.h"
#include "imgui_impl_raylib.h"
#include "rlImGui.h"
#include "runtime/ps2_host_present.h"
#include "runtime/ps2_overlay.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace nr::overlay_host
{
    namespace
    {
        // The time since the overlay's last frame, as ps2_overlay.cpp's rlImGui host measures it (TODO 404: the steady clock,
        // clamped to [1/1000, 1/10] s, the first frame 1/60).
        float frameSeconds()
        {
            using Clock = std::chrono::steady_clock;
            static Clock::time_point last{};
            const Clock::time_point now = Clock::now();
            const bool first = last == Clock::time_point{};
            const float dt = first ? 1.0f / 60.0f : std::chrono::duration<float>(now - last).count();
            last = now;
            return std::clamp(dt, 0.001f, 0.1f);
        }

        // ---- S13a: the raylib overlays' layers (the window's thread) ----
        // A picture's slices (the runtime's: before, between and after its ImGui frames), each drawn into one of kCopies layers in
        // turn (the core waits for the GPU to be done with a layer before it is drawn into again).
        constexpr int kSlices = 8, kCopies = 3;
        static_assert(kSlices * kCopies <= nr_core::kLayers, "the core's layers");
        struct Imported
        {
            unsigned texture = 0u, memory = 0u;
            uint64_t generation = 0;
        };
        Imported g_imported[nr_core::kLayers];
        int g_turn[kSlices] = {};
        int g_sliceLayer[kSlices] = {-1, -1, -1, -1, -1, -1, -1, -1};
        bool g_layersDrawn = false;
        unsigned g_layerFails = 0;

        unsigned int layerTexture(int slice, int width, int height)
        {
            if (slice < 0 || slice >= kSlices || width <= 0 || height <= 0 || !nr::glshare::available())
                return 0u;
            g_turn[slice] = (g_turn[slice] + 1) % kCopies;
            const int index = slice * kCopies + g_turn[slice];
            nr_core::Layer layer;
            if (!nr_core::layer(index, static_cast<uint32_t>(width), static_cast<uint32_t>(height), layer))
                return 0u;
            Imported &im = g_imported[index];
            if (im.texture == 0u || im.generation != layer.generation)
            {
                if (im.texture != 0u)
                    nr::glshare::release(im.texture, im.memory);
                im = Imported{};
                im.texture = nr::glshare::import(layer.handle, layer.size, layer.width, layer.height, im.memory);
                im.generation = layer.generation;
                if (im.texture == 0u)
                {
                    if (++g_layerFails <= 3u)
                        std::fprintf(stderr, "[native render] TF_NATIVE_SWAPCHAIN: overlay layer %d (%dx%d) not imported into GL - the "
                                             "window's raylib overlays not in the core's picture this time\n", index, width, height);
                    return 0u;
                }
            }
            g_sliceLayer[slice] = index;
            return im.texture;
        }

        void layerClosed(int slice)
        {
            if (slice < 0 || slice >= kSlices || g_sliceLayer[slice] < 0)
                return;
            nr_core_imgui::insert(nr_core::layerOverlay(g_sliceLayer[slice], true)); // (GL's rows bottom up)
            g_sliceLayer[slice] = -1;
            g_layersDrawn = true;
        }
    }

    bool takeLayersDrawn()
    {
        const bool drawn = g_layersDrawn;
        g_layersDrawn = false;
        return drawn;
    }

    bool install()
    {
        if (const char *v = std::getenv("TF_NO_OVERLAY"); v && *v == '1')
            return false;
        ps2_overlay::Host h;
        h.name = "rlImGui + nr_core";
        h.setup = [](bool darkTheme) { rlImGuiSetup(darkTheme); };
        h.shutdown = [] {
            nr_core_imgui::drop();
            nr_core_imgui::forgetTextures();
            rlImGuiShutdown();
        };
        h.begin = [] { rlImGuiBeginDelta(frameSeconds()); };
        h.end = [] {
            // (rlImGuiEnd, with the copy for the core between ImGui's render and rlImGui's drawing; S13a: the raylib overlays drawn
            // so far closed into their layer first - under this frame in the GL window and in the core alike - and the next layer
            // opened after it)
            ImGui::Render();
            ps2_host_present::overlayLayerBreak();
            nr_core_imgui::capture(ImGui::GetDrawData(), nr_core_imgui::Textures::Shared);
            ImGui_ImplRaylib_RenderDrawData(ImGui::GetDrawData());
            ps2_host_present::overlayLayerResume();
        };
        h.feedsInput = false;
        ps2_overlay::setHost(h);
        devswap::setOverlaySource(&nr_core_imgui::take);
        std::fprintf(stderr, "[native render] TF_NATIVE_SWAPCHAIN: overlay host %s\n", ps2_overlay::hostName());
        // (S13a) The raylib overlays into layers the core draws too (TF_NATIVE_SWAPCHAIN_LAYERS=0: not).
        if (const char *v = std::getenv("TF_NATIVE_SWAPCHAIN_LAYERS"); !(v && *v == '0'))
        {
            ps2_host_present::OverlayLayers layers;
            layers.texture = &layerTexture;
            layers.closed = &layerClosed;
            ps2_host_present::setOverlayLayers(layers);
            std::fprintf(stderr, "[native render] TF_NATIVE_SWAPCHAIN: the window's raylib overlays drawn into layers the core draws too\n");
        }
        return true;
    }
}

#else

namespace nr::overlay_host
{
    bool install() { return false; }
    bool takeLayersDrawn() { return false; }
}

#endif
