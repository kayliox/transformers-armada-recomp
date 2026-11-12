// The native renderer's entry point (README.md): wraps the game's render calls when
// TF_NATIVE_RENDER is set; does nothing otherwise.
#pragma once
#include <cstddef>
#include <cstdint>

class PS2Runtime;

namespace nr
{
    void install(PS2Runtime &runtime);
    // (Milestone 3) Before the window is made, on the main thread: with TF_GFX_API=vulkan under SDL3's window
    // (PS2X_PLATFORM=sdl3) the Vulkan backend presents the game's window itself (nr_backend.cpp).
    void beforeWindow();
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
    // Issue #17: a texture magnified on screen filtered by the draw's MMAG even where the GS's distance rule picks MMIN
    // (true, the default), or the PS2's choice (false; nr_d3d.cpp). From any thread.
    void setTextureSmoothing(bool on);
    // (TODO 165 / 161) The frames the drawing left out because it was behind, since the start (nr_thread.cpp): the rate
    // governor (overrides.cpp) takes a lasting share of them as the PC unable to hold the rate.
    unsigned long long framesLeftOutBehind();
// (TODO 290 fps) The drawing's time a frame (ms: the slower of its reading and its drawing, the GPU's waits included) at
// that share of the last `frames` frames drawn (0.95: the 95th percentile); 0 while fewer than 8 were drawn. And the
// GPU's own time a frame (its timestamps), the same way. The rate governor's way up checks a higher rate fits both.
double drawnFrameMs(double share, unsigned frames);
double gpuFrameMs(double share, unsigned frames);
// (TODO 291) The shown pictures' 1% low over the last `seconds` (the rate of the slowest 1% of their intervals, the same
// as the pacing line's): 0 while fewer than 100 were shown, or with no native picture live.
double pictureLowRate(double seconds);
    // (TODO 55) The game has moved to another place - a level loaded, the HQ entered or left (overrides.cpp): the
    // textures not drawn since are let go of a little later (nr_capture.cpp evictTextures). From the game's thread.
    void placeChanged();
    // (TODO 150) Direct3D 12 still being made on its thread (begun at install, seconds of host time): until it is
    // ready the GS draws (the emulated drawing on), and the game's render device runs its own code where the native
    // one stands in later - a play recording's start and its replay wait for it (playrec.cpp).
    bool rendererStarting();
    // (TODO 298) The HD set's index (nr_hires.cpp: a walk of thousands of files, ~0.2 s) made on a thread of its own at
    // the start (main, once the settings are read), while the window opens - not on the game's first frames.
    void prepareHires();
    // `TransformersRecomp --bake-shaders <file>` (build.ps1, after linking): the renderer's shaders compiled and written
    // as the table built into the next link (nr_d3d.cpp). 0: unchanged, 2: written anew, 1: failed.
    int bakeShaders(const char *path);
}
