// The parts of drawing a frame that do not depend on the graphics API (roadmap P3, step S2; docs/RENDERER_BACKEND.md):
// the GS state's translation into the GPU's blend, depth test and colour writes (as nr_backend.h's neutral types, which
// each backend turns into its own), TEX1's filtering constants, a mesh's box and its triangles' runs against the
// clipping planes, and the layout of the constants the shaders read. Shared by every backend.
#pragma once

#include "nr_backend.h"
#include "nr_types.h"

#include <cstdint>
#include <vector>

namespace nr::draw
{
    // The GS's ALPHA register as the GPU's blend: (A - B) * C + D, A/B/D one of Cs, Cd, 0 and C one of As, Ad, FIX. The
    // weights of Cs and Cd become the source and destination factors; a product the GPU's factors (stopping at 1.0)
    // cannot make - Cs * As or Cs * FIX, the GS's going to 2.0 - is left to the pixel shader (premultiply). `abe` off:
    // no blending. TF_NATIVE_PREMULTIPLY=0: the product by the GPU's factor, as before.
    gfx::BlendState blendOf(uint64_t alpha, bool abe);

    // The colour channels FBMSK leaves written (a channel whose mask byte is not all ones).
    uint32_t writeMaskOf(uint32_t fbmsk);

    // The GS's ZTST as the depth test.
    gfx::Compare compareOfZtst(uint32_t ztst);

    // TEX1 (LCM bit 0, MXL 2-4, MMAG 5, MMIN 6-8, L 19-20, K 32-43: signed, 7.4 fixed point) as the pixel shader's sampleGs
    // takes it; `sharpShift` moves the distance-based levels (LCM 0) toward the finer ones (the sharp-textures setting);
    // `smoothMagnified` (lod.x 2, the smooth-textures setting) has a texture magnified on screen take MMAG's filter even
    // where the GS's distance rule picks MMIN (issue #17).
    // TF_NATIVE_TEX1=0: left as they are (the anisotropic filter over the whole mip chain, as before).
    void tex1Constants(uint64_t tex1, float sharpShift, bool smoothMagnified, float *lod, float *lodf);

    // The box of a mesh's vertices (the core's clipping: a mesh wholly inside needs none).
    void boundsOf(const Mesh &mesh, float lo[3], float hi[3]);
    // Whether a box lies inside all six of the clipping matrix's planes (|x|, |y|, |z| <= w): then 0x0480 cuts nothing
    // and the draw needs no clipping (the planes are linear: its eight corners decide).
    bool boxInside(const float lo[3], const float hi[3], const float (*clip)[4]);

    // A clipping draw's triangles in runs, in their own order (TODO 62): 0 wholly inside all six planes (drawn without
    // the clipping shader, its output for them the same: the triangle as it is, screen-linear colour), 1 crossing one
    // (through the shader), 2 wholly outside one (not drawn, as the shader drops it). Near a plane (within a hair of the
    // GPU's own rounding) a triangle counts as crossing, so the shader decides it as before. False: more runs than worth
    // the switches.
    struct ClipRun
    {
        uint32_t first = 0, count = 0;
        int kind = 0;
    };
    bool clipRunsOf(const Mesh &mesh, const float (*clip)[4], std::vector<ClipRun> &runs);

    // A draw's constants (the shaders' b0).
    struct Constants
    {
        float rows[16];
        float screen[4];
        float misc[4];
        float flags[4];
        float test[4];
        float mode[4];
        float p[24][4];
        float texScale[4];
        float squeeze[4];
        float wrapS[4], wrapT[4];
        float dest[4];
        float lod[4], lodf[4];
        float clipRows[16];
        float clipOn[4];
        float standIn[4];    // (w: alpha + 2; 0 = none - the shader's standIn)
        float down[4];       // (x: the supersampled copy's four taps)
        float clipRowsB[16]; // (N9 stage 3: clipRows in the frame before's pose)
        float pose[4];       // (N9 stage 3: x the camera's turn applies, y the vertices' frame-before stream)
        float lightsB[12];   // (N9 stage 3: p[8..10] of 0x21f0 in the frame before's pose)
        float clipRowsM[16]; // (TODO 101 a: clipRows halfway between the two poses, along the turn - nr_passes.cpp midPose)
    };
    // (N9 stage 3) The picture's constants (the shaders' b1): where it lies between the frame before's pose and this
    // frame's, the camera's turn.
    struct PictureConstants
    {
        float picture[4];
        float turn[16];
        float turnClip[16];
    };
}
