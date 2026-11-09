// The native renderer's entry point (README.md): wraps the game's render calls when
// TF_NATIVE_RENDER is set; does nothing otherwise.
#pragma once
#include <cstddef>
#include <cstdint>

class PS2Runtime;

namespace nr
{
    void install(PS2Runtime &runtime);
    // The PAL release: the device, the live picture and what the game draws outside its frames, no render call wrapped.
    void installDisplayOnly(PS2Runtime &runtime);
    // Registered before capture starts. Sampled on the EE thread for each new
    // frame; queued frames retain their own choice. Null/failed proofs use normal policy.
    void setFrameTexturePolicy(bool (*policy)(const uint8_t *, size_t));
    // Call on the EE thread (or while it is stopped) after a reset/state load.
    // Retains the callback, but clears the current capture's policy snapshot.
    void resetFrameTexturePolicy();
    // Capture proven interframe native program uploads; default null does nothing.
    // Set/reset on the EE thread or while it is stopped.
    void setInterframeMicrocodePolicy(bool (*policy)(const uint8_t *, size_t));
    void resetInterframeMicrocodeCapture();
    // N8 (TODO 139): how far the grass reaches, times the factor (1: the game's; nr_grass.cpp). From any thread.
    void setGrassReach(float factor);
    // N8 item 5: the distance-based mip levels shifted toward the finer ones, in levels (-1: matched to the picture's
    // height, 0: the PS2's; nr_d3d.cpp). From any thread.
    void setTextureSharpness(float shift);
    // (TODO 165 / 161) The frames the drawing left out because it was behind, since the start (nr_thread.cpp): the rate
    // governor (overrides.cpp) takes a lasting share of them as the PC unable to hold the rate.
    unsigned long long framesLeftOutBehind();
    // (TODO 55) The game has moved to another place - a level loaded, the HQ entered or left (overrides.cpp): the
    // textures not drawn since are let go of a little later (nr_capture.cpp evictTextures). From the game's thread.
    void placeChanged();
    // (TODO 150) Direct3D 12 still being made on its thread (begun at install, seconds of host time): until it is
    // ready the GS draws (the emulated drawing on), and the game's render device runs its own code where the native
    // one stands in later - a play recording's start and its replay wait for it (playrec.cpp).
    bool rendererStarting();
    // `TransformersRecomp --bake-shaders <file>` (build.ps1, after linking): the renderer's shaders compiled and written
    // as the table built into the next link (nr_d3d.cpp). 0: unchanged, 2: written anew, 1: failed.
    int bakeShaders(const char *path);
}
