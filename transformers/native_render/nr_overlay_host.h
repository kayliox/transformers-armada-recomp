// nr_core S12: the recomp's "core" overlay host (nr_overlay_host.cpp) - under TF_NATIVE_SWAPCHAIN=1 the window's ImGui overlays (the
// F1 menu, the runtime's debugger) drawn into the core's swap chain picture too.
#pragma once

namespace nr::overlay_host
{
    // From the window's thread, at the first picture the core's swap chain shows (nr_present.cpp devDraw): rlImGui's host
    // replaced by the same one plus the core's drawing (nr_core_imgui), and the devswap's overlay source set. False: not done
    // (no debug UI built in, not Windows, or TF_NO_OVERLAY=1) - the overlays stay the GL window's alone, as before.
    bool install();
    // (S13a) Whether a layer of the window's raylib overlays was handed to the core since the last call (the window's thread):
    // GL must then be done drawing before the core's present reads it.
    bool takeLayersDrawn();
}
