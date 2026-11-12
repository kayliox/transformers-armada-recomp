#pragma once
// (TODO 299) The temporal upscaler (FSR 3 / FSR 4 through the AMD FidelityFX SDK): its settings and what the renderer
// gives it. docs/TEMPORAL_UPSCALER.md (the private repo's branch docs/temporal-upscaler) has the plan and the findings.
//
// Step 1 a: the motion. Each draw's frame before's pose comes from the in-between pictures' matching (nr_thread.cpp
// poses::prepare / assign: the draw's matrix the frame before, for a mesh made anew each frame its vertices then); the
// vertex shader puts the vertex where it is and where it was, and the motion view shows the difference.
//
//   TF_TAA_DEBUG=<scale>  the motion view: the scene's 3D drawn as its motion since the frame before (grey none, red the
//                         x, green the y, in clip space times <scale>; 1 = 20). The poses' matching runs for every frame.

#include <cstdint>

namespace nr::taa
{
    // The motion view's scale (0: off).
    float debugView();
    // The temporal upscaler on (the F1 setting Upscaler: FSR 3 / 4 - TF_NATIVE_UPSCALER=fsr - or TF_TAA=1, read at the start;
    // only where the SDK's DLLs are beside the game): the picture at the window's lines, the GS targets at the render scale,
    // the scene upscaled into the picture's resolution by the FidelityFX SDK - at render scale 100%, its anti-aliasing
    // alone (native AA). Off again for the rest of the run when the SDK can't run (markFailed).
    bool upscalerOn();
    // The SDK's two DLLs are next to the game (the F1 setting's choice is offered only then).
    bool sdkPresent();
    // The upscaler could not run on this device (no context, a dispatch refused): off from the next frame.
    void markFailed();
    // The scene's camera as FSR wants it, fitted from the game's own projection each frame (nr_frame.cpp): the depth target
    // holds 16 x depthScale x z / w of each draw's matrix, linear in 1 / w - near and far in w's units, the vertical field of
    // view, w's units in metres (1 PS2 unit = 1 m). Defaults: no fit yet.
    struct Camera
    {
        float nearZ = 1.0f, farZ = 1.0e6f;
        bool infinite = true;
        float fovY = 1.0f;
        float metersPerUnit = 1.0f;
    };
    // The frame's draws want their frame before's pose (the motion view, the upscaler).
    bool wantsMotion();
    // The projection's jitter for frame `n` (pixels of the render size, -0.5..0.5): FSR's Halton (2, 3) sequence, its phase
    // count 8 x (output / render)^2.
    void jitter(uint64_t n, float renderWidth, float outputWidth, float &x, float &y);
    // The dispatch (D3D12): the colour, depth (R32 typeless), motion and reactive mask (R8, may be null) of the render size
    // into `output` (UAV state), on `commandList`; the inputs in NON_PIXEL_SHADER_RESOURCE. The context made again when the
    // sizes or the depth's kind change. False: none.
    bool upscale(void *device, void *commandList, void *colour, void *depth, void *motion, void *reactive, void *output, unsigned renderWidth,
                 unsigned renderHeight, unsigned outputWidth, unsigned outputHeight, float jitterX, float jitterY, float frameMs, bool reset,
                 float sharpness, float motionScaleX, float motionScaleY, const Camera &camera);
    // (TF_TAA_PROBE=1) The FidelityFX SDK on this device (an ID3D12Device): its upscaler versions and the one a context
    // gets, logged. The SDK's DLLs come next to the game with the CMake option TF_FSR_SDK.
    void probe(void *device);
}
