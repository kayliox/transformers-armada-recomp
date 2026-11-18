// nr_core S9c: the developer switch TF_NATIVE_SWAPCHAIN=1 (nr_devswap.cpp) - the native pictures shown through the core's swap
// chain in a child window over the game window. Called by nr_present.cpp's picture drawer (the window's thread); no raylib here.
#pragma once

#include "nr_core.h"

#include <cstdint>

namespace nr::devswap
{
    // TF_NATIVE_SWAPCHAIN=1 (read once; Windows only).
    bool on();
    // The shot tick due for a picture of `pictureTick` (TF_NATIVE_SWAPCHAIN_SHOT), 0: none - each listed tick once.
    uint64_t shotDue(uint64_t pictureTick);
    // Shared picture `picture` (-1: none - the child hidden, the GL window's own drawing shows) presented through the core into
    // the child window over `parent` (an HWND) at the GL drawer's rects; true when presented (S12: or kept for flush()).
    bool show(void *parent, int picture, const float src[4], const float dst[4], int upscaler, float sharpness, int sync, const char *shot);

    // (nr_core S12) The window's ImGui overlays drawn through the core too (nr_overlay_host.cpp sets the source: the overlays the
    // ImGui frames since the last call make). With a source, show() keeps its picture and flush() - called once the window's
    // overlays of that picture are drawn - presents it with them: the same picture's menu, as the GL window shows it. Without
    // one (the default), show() presents at once, as before.
    using OverlaySource = nr_core::Overlay (*)();
    void setOverlaySource(OverlaySource source);
    // The kept picture presented, with the overlays drawn since (none kept: the overlays dropped). From the window's thread.
    void flush();
    // The shot tick (TF_NATIVE_SWAPCHAIN_SHOT) the kept picture will save at flush(), 0: none - the GL twin is read before it.
    uint64_t keptShotTick();
    // (Tests) The next present saved here too (BMP): TF_MENU_SHOT's twin of the core's window.
    void shotNext(const char *path);
}
