// nr_core S12: the core's ImGui overlays (the F1 menu, the runtime's debugger) - an OPTIONAL module beside the core, for the hosts
// that draw ImGui over the core's pictures (the recomp under TF_NATIVE_SWAPCHAIN=1, the native decomp build). The core itself knows
// nothing of ImGui: it only calls an overlay at each present (nr_core.h Overlay, OverlayTarget). This module turns ImGui frames
// into such overlays, with a small Direct3D 12 renderer of its own (not imgui_impl_dx12: the overlay may run on the core's drawing
// thread while the ImGui context is in use on the host's thread, so it draws COPIES of the frames; and in the recomp another
// renderer - rlImGui's GL - owns ImGui's textures, so it keeps copies of their pixels instead of taking them over).
//
// Two parts:
// - The renderer, for any ImGui context: after ImGui::Render(), capture(ImGui::GetDrawData(), ...) copies the frame; take() hands
//   the frames captured since the last take() over as ONE overlay (drawn in the order captured: e.g. the menu's frame, then the
//   debugger's) for the next present (PresentRequest::overlay / FrameInfo::overlay).
// - A whole ImGui host for a window whose messages the core forwards (nr_core::WindowEvents::message - the core's own window, or a
//   host's window it watches): the context, ImGui's own Win32 input (imgui_impl_win32: mouse, wheel, keys, WM_CHAR text with
//   surrogate pairs, focus, the IME's composition window placed at the text cursor), the frame's start and end. The recomp does not
//   use this part (its overlays keep rlImGui's input on the GL window: nr_overlay_host.cpp).
//
// Built only with the debug UI's ImGui (PS2X_ENABLE_DEBUG_UI) on Windows; links imgui, d3d12, d3dcompiler.
#pragma once

#include "../nr_core.h"

#include <cstdint>

struct ImDrawData;

namespace nr_core_imgui
{
    // Who answers ImGui's texture requests (ImTextureData: create / update / destroy).
    enum class Textures
    {
        Shared, // another renderer of the same context does (rlImGui's): capture() before it, the module reads their status
        Owned   // the module is the context's only renderer: capture() answers them (Status OK / Destroyed, TexID)
    };
    // The frame just rendered (ImGui::Render()) copied for the next overlay: its draw lists and the textures' changes (pixels copied).
    // On the ImGui context's thread. A draw of a texture that is not one of ImGui's own (an application's ImTextureID) is left out.
    void capture(const ImDrawData *data, Textures textures);
    // (S13a) Another overlay (e.g. a layer of the host's own drawing, nr_core::layerOverlay) drawn between the frames captured
    // so far and the next: handed over by the same take(), in its place in their order (any thread).
    void insert(nr_core::Overlay overlay);
    // The frames captured since the last take(), as one overlay (empty when none: nothing drawn). Any thread.
    nr_core::Overlay take();
    // The frames captured so far, dropped (none drawn).
    void drop();
    // The copies of ImGui's textures let go (at the next overlay): the context they came from is gone (its texture numbers are
    // its own - a new context counts from 0 again).
    void forgetTextures();
    // Sampling of ImGui's textures: nearest (the default: rlImGui's GL textures, raylib's point filter) or bilinear.
    void setLinearSampling(bool linear);
    // The GPU objects let go - when no present can run any more (the last present done; the module may be used again after).
    void release();

    // What the module drew so far (tests): overlays drawn, draw calls, texture uploads, draws left out (no texture).
    struct Stats
    {
        uint64_t overlays = 0, draws = 0, uploads = 0, skipped = 0;
    };
    Stats stats();

    // ---- the whole host (a window whose messages the core forwards) ----
    // ImGui's context made (once) with its dark or light style and ImGui's default font at `fontPixels`, ImGui's Win32 input on
    // `window` (an HWND; null: nr_core::window(), taken at the first begin() when there is none yet). Sets WindowEvents::message
    // when the host has not set one (else the host calls onWindowMessage from its own).
    void setup(bool darkTheme, void *window = nullptr, float fontPixels = 13.0f);
    void shutdown();
    // The window's messages queued since the last frame handed to ImGui's Win32 input (on this thread: the ImGui context's), then
    // ImGui's Win32 frame start and ImGui::NewFrame.
    void begin();
    // ImGui::Render() and capture(..., Textures::Owned).
    void end();
    // The host feeds ImGui's input itself (ps2_overlay::Host::feedsInput): true.
    bool feedsInput();
    // A window message for ImGui's input (what setup() puts in WindowEvents::message): queued for the next begin(); answers
    // WM_SETCURSOR in the window's client area itself with the cursor ImGui last asked for (true: handled).
    bool onWindowMessage(void *window, uint32_t message, uint64_t wParam, int64_t lParam);
}
