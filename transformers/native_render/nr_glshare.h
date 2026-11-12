// The live picture without a CPU copy: the renderer's picture is a Direct3D 12 texture shared with
// the window's OpenGL (EXT_memory_object_win32: the same video memory read by both, no readback, no
// upload). Taken where the driver offers it; otherwise the CPU hand-over stays (nr_present.cpp).
#pragma once

#include <cstdint>

namespace nr::glshare
{
    // On the window's thread, its GL context current: whether the driver can import D3D12 textures.
    bool available();

    // A shared D3D12 texture (its NT handle, allocation size, pixels) as a GL texture; 0 if refused.
    // The memory object is returned for release().
    unsigned import(void *handle, uint64_t size, uint32_t width, uint32_t height, unsigned &memory);

    void release(unsigned texture, unsigned memory);

    // What the window's GL reads from an imported texture (its context current): the brightest red, green or blue
    // and the largest alpha over all its pixels. All 0: nothing reached GL - Intel's driver imports the shared
    // texture without an error and reads zeros (a black window on the N100 / UHD Graphics, 2026-11-10), where a
    // picture the renderer made, even a black one, is opaque. False when it cannot be read.
    bool readBack(unsigned texture, uint32_t width, uint32_t height, unsigned &maxColour, unsigned &maxAlpha);
}

namespace nr::d3d
{
    // How many shared textures the renderer may make (N9: the paced hand-over keeps pictures waiting for their moment
    // beside the one shown). The window imports every one of them (TODO 98: it took only the first 8 while the
    // renderer used up to 24 - a picture in 8..23 was never shown, and the texture still on the screen was left
    // unprotected and drawn into, so the screen showed later and earlier pictures by turns).
    constexpr int kSharedPictures = 24;

    // A finished picture in one of the renderer's shared textures (the GPU done with it).
    struct SharedPicture
    {
        void *handle = nullptr;  // the texture's NT handle (the renderer's; not to be closed)
        uint64_t size = 0;       // its allocation size
        uint32_t width = 0, height = 0;
        int index = -1;          // which of the shared textures
        uint64_t generation = 0; // made again (another size): imported again
        bool has3D = false;
        // (N9, the pacing log and the paced hand-over) The frame's VBlank (the moment it shows: its Frame::dueNs) and
        // when the picture was ready - the GPU done with it - on the steady clock, in ns.
        uint64_t tick = 0;
        int64_t dueNs = 0, readyNs = 0;
        uint32_t stamp = 0; // (TF_NATIVE_STAMP: the number stamped into it)
        uint64_t turnId = 0; // (N9: the picture's camera turn, its shown yaw for the pacing log - view_turn.h)
    };

    // The window can read shared textures (from its first frame on; until then, the CPU hand-over).
    void setSharedPictures(bool on);
    // The newest picture since serial, if it is a shared one.
    bool latestShared(SharedPicture &out, uint64_t &serial);
    // The shared texture the window shows now (not drawn into while it is shown, nor the two it showed before - the
    // window's GL may still be reading those for a frame or two after it has moved on).
    void sharedHeld(int index);
    // (N9) Waits until a picture newer than `serial` is handed over, or until `untilNs` (steady clock): true when one
    // is. `lastNs`: when the newest was handed over (0: none yet). The window's pacer (nr_present.cpp).
    bool waitPicture(uint64_t serial, int64_t untilNs, int64_t &lastNs);
    // (N9) How the pictures are handed over (TF_PACING): 2 paced - each at its frame's moment plus a delay the GPU's
    // times need (the default); 1 as soon as the GPU is done with it; 0 at the next frame's start, as before N9.
    int pacingMode();
}
