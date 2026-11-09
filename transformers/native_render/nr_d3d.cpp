// The native renderer's GPU side, Direct3D 12 (README.md; D3D12 for ray tracing, the upscalers'
// frame generation and the rest of the PC headroom). Milestone 1: the captured geometry sets in
// place, coloured by their vertex colours (or a colour per mesh), drawn into a picture of its own
// and saved as a BMP on request, to compare with the emulated GS picture of the same VBlank.
//
// The transform is VU1 routine 0x1410's (docs/RENDERER.md): screen = row0 * x + row1 * y +
// row2 * z + row3; X and Y / w are GS coordinates (the pixel, 12.4 fixed point there), Z * 16 / w the
// GS depth (larger = nearer: the game tests GEQUAL, so the depth buffer is cleared to 0 and tested
// GREATER_EQUAL).
//
// TF_NATIVE_DEBUG=1: the D3D12 debug layer (messages in the debugger output).
#include "nr_types.h"
#include "nr_backend.h"
#include "nr_draw.h"
#include "nr_device.h"
#include "nr_glshare.h"
#include "nr_occlusion.h"
#include "view_turn.h"
#include "runtime/ps2_host_display.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <string>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <map>
#include <array>
#include <unordered_set>
#include <vector>

// SMAA 1x's precomputed lookup textures (ext/smaa, MIT - its LICENSE.txt): areaTexBytes (R8G8 160 x 560),
// searchTexBytes (R8 64 x 16).
#include "ext/smaa/AreaTex.h"
#include "ext/smaa/SearchTex.h"

namespace nr
{
    std::atomic<uint64_t> g_pipelinesMadeAll{0}; // (TODO 112, TF_HITCH_MS: pipelines made, both threads)
    uint64_t pipelinesMade() { return g_pipelinesMadeAll.load(std::memory_order_relaxed); }
    // N8 item 5 (sharp textures): the mip levels' shift toward the finer ones, in levels: -1 matched to the picture's height
    // (log2(lines / 448): the GS's distance-based choice made for the PS2's 448 lines), 0 the PS2's, above 0 that many.
    std::atomic<float> g_texSharp{-1.0f};
    void setTextureSharpness(float shift) { g_texSharp.store(shift, std::memory_order_relaxed); }
}

namespace nr::d3d
{
    namespace
    {
        template <typename T> void release(T *&p)
        {
            if (p)
                p->Release();
            p = nullptr;
        }

        // The routines VU1 ran for the draw (nr_passes.cpp: the modes, the parameters from their data
        // blocks), done per vertex here (docs/RENDERER.md; src/vu1_native.cpp bit for bit), in the GS's
        // units (colours 0..255, 128 = 1.0) until the output.
        const char kShader[] = R"(
cbuffer Draw : register(b0)
{
    float4 row0, row1, row2, row3; // model to GS screen (VU1's vf01-vf04)
    float4 screen;                 // GS origin x, y; 2 / width, 2 / height
    float4 misc;                   // depth scale, has vertex colours, the mesh's debug colour (r, g)
    float4 flags;                  // x: textured, y: TFX, z: TCC, w: 24-bit (alpha from TEXA, not memory)
    float4 test;                   // TEST: x ATE, y ATST, z AREF, w AFAIL
    float4 mode;                   // x colour, y ST, z alpha (nr_passes.cpp)
    float4 p[24];                  // the routines' parameters
    float4 texScale;               // xy: texel coordinates to the sampled render target's (1 for textures); z: STQ (Q in size); w: depth fog
    float4 squeeze;                // the 2D narrowed for widescreen: x pivot, y factor (1 = none), z the buffer's left, w its width
    float4 wrapS, wrapT;           // CLAMP per axis: x mode (0 repeat, 1 clamp, 2 region clamp), y min, z max (texels)
    float4 dest;                   // x: TEST's DATE - 0 off, 1 pass where the frame's alpha is under 0x80 (DATM 0), 2 at or over it;
                                   // y: the colour times 1 As, 2 FIX (z) here, the blend's source factor one (blendOf); w: untextured (TME off)
    float4 lod;                    // TEX1 (the GS's filter and mip choice): x on, y K (levels), z 2^L, w MXL
    float4 lodf;                   // x LCM (the level K alone), y MMAG linear, z MMIN's texels linear, w MMIN's levels (0 none, 1 nearest, 2 linear)
    float4 clipRows[4];            // 0x0480's clipping matrix (the position through it, |x|, |y|, |z| <= w kept)
    float4 clipOn;                 // x: the core clips (q21.x above 0); y: the colour screen-linear (the GS's Gouraud)
    float4 standIn;                // a flat colour drawn in place of the texture (w = its alpha + 2; under 1.5: none) - a game
                                   // texture still on its way to the GPU (TODO 76: made on the uploader, not in this frame)
    float4 down;                   // x: a copy from a supersampled target into the picture - four taps over the pixel's footprint
    float4 clipRowsB[4];           // (N9 stage 3) clipRows in the frame before's pose
    float4 pose;                   // (N9 stage 3) x: the camera's turn of the picture applies (the scene's 3D); y: the vertices blended
    float4 lightsB[3];             // (N9 stage 3) p[8..10] of 0x21f0 (the lights, the eye in the mesh's space) in the frame before's pose
    float4 clipRowsM[4];           // (TODO 101 a) clipRows halfway between the two poses, along the turn (nr_passes.cpp midPose)
};
// (N9 stage 3) The picture: one frame's recording drawn once for each picture between frames, these its own - where it lies
// between the frame before's pose and this frame's (each matrix blended so), and the camera's turn since the frame's input.
cbuffer Picture : register(b1)
{
    float4 picture;  // x: 0 the frame before's pose .. 1 this frame's
    float4 turn[4];  // the turn as GS screen space (row vectors: s' = s turn)
    float4 turnClip[4]; // the same in the clipping matrix's space
};
Texture2D tex : register(t0);
Texture2D palette : register(t1);      // the depth fog's 256 entries; for a DATE draw, a copy of the target (its alpha)
Texture2D<float> depthTex : register(t2); // the target's own depth (GS Z * 16 / 2^32)
SamplerState smp : register(s0);       // (anisotropic, the mip chain: the GS's level 0 with its footprint held, and TEX1 off)
SamplerState smpLinear : register(s1); // (TEX1: a level the GS picks, its texels linear)
SamplerState smpPoint : register(s2);  // (TEX1: nearest texels)
SamplerState smpAlt : register(s3);    // (TF_NATIVE_ANISO_ALT: the A/B anisotropy, clipOn.w)
struct VsIn { float3 pos : POSITION; float4 colour : COLOR; float2 uv : TEXCOORD; float3 normal : NORMAL; float size : PSIZE;
             // (Per instance, TODO 94: the pass's matrix and q0 - what an instance pass has of its own - from a stream of
             // their own, so that an instanced draw's passes are one draw. Every draw takes them from there.)
             float4 iRow0 : IROW0; float4 iRow1 : IROW1; float4 iRow2 : IROW2; float4 iRow3 : IROW3; float4 iP0 : IPZERO;
             // (N9 stage 3: the matrix in the frame before's pose)
             float4 iRowB0 : IROWB0; float4 iRowB1 : IROWB1; float4 iRowB2 : IROWB2; float4 iRowB3 : IROWB3;
             // (TODO 101 a: the matrix halfway between the two poses, along the turn)
             float4 iRowM0 : IROWM0; float4 iRowM1 : IROWM1; float4 iRowM2 : IROWM2; float4 iRowM3 : IROWM3;
             // (N9 stage 3: a mesh made anew each frame - its vertex the frame before, pose.y)
             float3 posB : POSB; float4 colourB : COLORB; float sizeB : PSIZEB; };
// (The colour linear on the screen, as the GS's Gouraud shading steps it: no perspective correction -
// that is for the texture's S, T, Q alone; perspective-correct, a large triangle seen at a slant takes
// its corners' colours in other proportions. But a triangle the GPU clips (the near plane, the core's
// planes) gets new corners whose screen-linear colour is extrapolated from behind the eye: such a one
// keeps the perspective-correct colour - linear in clip space, which is what 0x0480 gives its new
// corners. `whole` is 1 at every pixel of a triangle with all three corners inside, under 1 elsewhere.)
struct VsOut { float4 pos : SV_Position; noperspective float4 colour : COLOR0; float4 colourP : COLOR1; float whole : WHOLE; float2 uv : TEXCOORD; float cull : CULL; float q : QDIV; float4 box : BOXSIDES; float boxz : BOXZ; noperspective float gsq : GSQ;
               float4 clipA : SV_ClipDistance0; float2 clipB : SV_ClipDistance1; };

float3 lightsOf(float3 n, float3 l1, float3 l2, float3 c1, float3 c2, float scale, float ambient, float3 material)
{
    float nl1 = max(dot(n, l1), 0.0), nl2 = max(dot(n, l2), 0.0);
    return clamp(material * ((c1 * nl1 + c2 * nl2) * scale + ambient) * 128.0, 0.0, 255.0);
}

// The water's wave (0x1df0 of image e147): the cubic s - s^3 of s = 2 frac(p + 100000) - 1, the
// fraction as the routine takes it (x - ITOF0(FTOI0(x)): towards zero).
float waveOf(float p)
{
    float x = p + 100000.0;
    float s = 2.0 * (x - trunc(x)) - 1.0;
    return s - s * s * s;
}

)"
                              R"(// (N9 stage 3) A matrix row at the picture's moment: the frame before's pose b blended to this frame's a - this frame's
// exactly at 1 (and where the draw does not move: b = a).
float4 posed(float4 b, float4 a)
{
    return picture.x >= 1.0 ? a : b + (a - b) * picture.x;
}
// (TODO 101 a) The same through the pose halfway m (the axes turned by half the turn, nr_passes.cpp midPose): the curve
// through the three - at 0 b, at 1 a, at a half m - so an axis that turns keeps its length (a straight blend of the
// rows shortens it: the parts of a model shrank in the pictures between frames, up to 78% at the fixed 120). A row
// that is the average (the origin's, a pose that does not turn) gives the straight blend.
float4 posed3(float4 b, float4 m, float4 a)
{
    const float t = picture.x;
    return t >= 1.0 ? a : b * ((1.0 - t) * (1.0 - 2.0 * t)) + m * (4.0 * t * (1.0 - t)) + a * (t * (2.0 * t - 1.0));
}

VsOut vs(VsIn i)
{
    VsOut o;
    if (pose.y > 0.5 && picture.x < 1.0)
    {
        // (the vertex blended from the frame before's: the grass's sway, the beams' turning - N9 stage 3)
        i.pos = i.posB + (i.pos - i.posB) * picture.x;
        i.colour = i.colourB + (i.colour - i.colourB) * picture.x;
        i.size = i.sizeB + (i.size - i.sizeB) * picture.x;
    }
    float4 row0 = posed3(i.iRowB0, i.iRowM0, i.iRow0), row1 = posed3(i.iRowB1, i.iRowM1, i.iRow1), row2 = posed3(i.iRowB2, i.iRowM2, i.iRow2),
           row3 = posed3(i.iRowB3, i.iRowM3, i.iRow3);
    if (pose.x > 0.5)
    {
        // (the camera's turn since the frame's input, on the screen: N9 stage 3)
        const float4x4 t = float4x4(turn[0], turn[1], turn[2], turn[3]);
        row0 = mul(row0, t);
        row1 = mul(row1, t);
        row2 = mul(row2, t);
        row3 = mul(row3, t);
    }
    const float4 P0 = i.iP0; // (the cbuffer's, per instance)
    // The water (mode.z 2: 0x1df0 then 0x2cc0, docs/RENDERER.md): the height moved by two waves -
    // phases (x, y by the wave's x, y, plus its w) times its speed (p1.y, p1.w), heights p2.z, p3.z -
    // and the sphere map of the eye's direction (p0, from the unmoved position) reflected about the
    // normal the waves' slopes make (the cubic a quarter of a period on, times p1.x; its z p1.z).
    float3 ipos = i.pos;
    float2 waterMap = float2(0.0, 0.0);
    if ((int)mode.z == 2)
    {
        float ph1 = (i.pos.y * p[2].y + i.pos.x * p[2].x + p[2].w) * p[1].y;
        float ph2 = (p[3].w + (i.pos.y * p[3].y + i.pos.x * p[3].x)) * p[1].w;
        ipos.z = i.pos.z + waveOf(ph1) * p[2].z + waveOf(ph2) * p[3].z;
        float d1 = waveOf(ph1 - 0.75) * p[2].z, d2 = waveOf(ph2 - 0.75) * p[3].z;
        float wnx = (p[2].x * d1 + p[3].x * d2) * p[1].x, wny = (p[2].y * d1 + p[3].y * d2) * p[1].x, wnz = p[1].z;
        float3 ve = normalize(P0.xyz - i.pos);
        float wnv = wnx * ve.x + wny * ve.y + wnz * ve.z;
        float k = 0.5 / ((2.0 * wnz * wnv - ve.z) + 1.0);
        waterMap = float2((2.0 * wnx * wnv - ve.x) * k + 0.5, (2.0 * wny * wnv - ve.y) * k + 0.5);
    }
    // Cybertron's bubble (mode.z 3: image 6b20's 0x2030): the vertex moved by two waves (q4 amplitude
    // and frequency of each, q5 / q6 direction and phase - the water's cubic), the normal by the same.
    float3 mnormal = i.normal;
    if ((int)mode.z == 3)
    {
        float3 d = p[5].xyz * (waveOf((dot(i.pos, p[5].xyz) + p[5].w) * p[4].y) * p[4].x) +
                   p[6].xyz * (waveOf((dot(i.pos, p[6].xyz) + p[6].w) * p[4].w) * p[4].z);
        ipos = i.pos + d;
        mnormal = normalize(i.normal + d);
    }
    // The dash's after-image (mode.z 4: image 83f1's 0x2220): the vertices whose normal faces q0
    // moved by it - min(max(q0 . N, 0) * 1e8, 1), a step - the rest left where they are.
    float smearT = 0.0;
    if ((int)mode.z == 4)
    {
        smearT = saturate(dot(P0.xyz, i.normal) * 1e8);
        ipos = i.pos + P0.xyz * smearT;
    }
    // (TODO 162) Cybertron's city (mode.z 5: image a31d's 0x22c0, traced on captured batches): a building a vertex -
    // the centre its position, the half sizes its ST, the height its colour's alpha times 10, the foot z times p5.w -
    // and a corner of the face the batch draws (p1.x) in size (0-3: the strip's order). A side is the one facing the eye
    // (p2): the -x / -y side when the eye is below the centre along it, its colour times p3, else the + side, times p4;
    // the top corners take that colour, alpha 128, the feet p6. The roof all p3. ST the texture's repeats (p5: per
    // unit of half size along the face, per unit of height; at least one), the strip's first corner at 0, 0.
    float2 citySt = float2(0.0, 0.0);
    float4 cityColour = float4(0.0, 0.0, 0.0, 0.0);
    if ((int)mode.z == 5)
    {
        const float4 vcol = round(i.colour * 255.0);
        const float h = vcol.a * 10.0, sx = i.uv.x, sy = i.uv.y;
        const float base = i.pos.z * p[5].w, top = base + h;
        const int face = (int)p[1].x, k = (int)(i.size + 0.5);
        const float along = (k & 1) ? 1.0 : -1.0;
        const bool upper = k < 2;
        if (face == 0 || face == 1)
        {
            const float t = (face == 0 ? p[2].y - i.pos.y : p[2].x - i.pos.x) < 0.0 ? 1.0 : -1.0;
            ipos = face == 0 ? float3(i.pos.x + sx * along, i.pos.y - sy * t, upper ? top : base)
                             : float3(i.pos.x - sx * t, i.pos.y - sy * along, upper ? top : base);
            citySt = float2((k & 1) ? max(trunc((face == 0 ? sx * p[5].x : sy * p[5].y)), 1.0) : 0.0, upper ? 0.0 : max(trunc(h * p[5].z), 1.0));
            cityColour = upper ? float4(trunc(vcol.rgb * (t > 0.0 ? p[3].rgb : p[4].rgb)), 128.0) : p[6];
        }
        else if (face == 2)
        {
            ipos = float3(i.pos.x + sx * along, i.pos.y + (upper ? sy : -sy), top);
            citySt = float2((k & 1) ? max(trunc(sx * p[5].x), 1.0) : 0.0, upper ? 0.0 : max(trunc(sy * p[5].y), 1.0));
            cityColour = float4(trunc(vcol.rgb * p[3].rgb), 128.0);
        }
    }
    float4 s = row0 * ipos.x + row1 * ipos.y + row2 * ipos.z + row3;
    float w = s.w;
    float2 gs = s.xy / w;
    if (squeeze.y < 0.9999)
    {
        // (The vertices at the 4:3 picture's very edges stay: a banner drawn from the edge keeps
        // its end at the wider picture's.)
        float edge = squeeze.w * 0.005;
        if (gs.x > squeeze.z + edge && gs.x < squeeze.z + squeeze.w - edge)
            gs.x = squeeze.x + (gs.x - squeeze.x) * squeeze.y;
        s.xy = gs * w;
    }
    float2 ndc = float2((gs.x - screen.x) * screen.z - 1.0, 1.0 - (gs.y - screen.y) * screen.w);
    // Depth stays linear in clip space (GS Z = z / w * 16, larger nearer), so vertices behind the
    // eye are clipped by the hardware, as VU1's 0x0480 clips them against the near plane.
    o.pos = float4(ndc * w, s.z * 16.0 * misc.x, w);
    // The core's clipping (0x0480, q21.x above 0): the triangle kept where the position through the
    // clipping matrix has |x|, |y|, |z| <= w - planes linear in the position, so the GPU's clip distances
    // cut exactly where 0x0480's Sutherland-Hodgman does (its near plane: w ~ 0.5, not the GPU's ~0.03).
    o.clipA = float4(1.0, 1.0, 1.0, 1.0);
    o.clipB = float2(1.0, 1.0);
    if (clipOn.x > 0.5 && mode.w < 0.5)
    {
        float4 k = posed3(clipRowsB[0], clipRowsM[0], clipRows[0]) * ipos.x + posed3(clipRowsB[1], clipRowsM[1], clipRows[1]) * ipos.y +
                   posed3(clipRowsB[2], clipRowsM[2], clipRows[2]) * ipos.z + posed3(clipRowsB[3], clipRowsM[3], clipRows[3]);
        if (pose.x > 0.5)
            k = mul(k, float4x4(turnClip[0], turnClip[1], turnClip[2], turnClip[3]));
        o.clipA = float4(k.w - k.x, k.w + k.x, k.w - k.y, k.w + k.y);
        o.clipB = float2(k.w - k.z, k.w + k.z);
    }
    if (mode.w > 0.5)
    {
        // A sprite (0x22a0): its corner on the screen, the centre less q15 * size * Q or plus
        // q18 * size * Q (each at most q10.zw), its ST from q11 to q14.
        float q = 1.0 / max(w, 1e-6);
        float2 lo = min(p[21].xy * i.size * q, p[18].zw), hi = min(p[22].xy * i.size * q, p[18].zw);
        float2 corner = clamp(gs, 1000.0, 3000.0) + lerp(-lo, hi, i.uv);
        float2 cn = float2((corner.x - screen.x) * screen.z - 1.0, 1.0 - (corner.y - screen.y) * screen.w);
        o.pos = w > 0.0 ? float4(cn, min(s.z * q, 1.6e7) * 16.0 * misc.x, 1.0) : float4(0.0, 0.0, -1.0, 1.0);
    }
    // (TODO 100) A pass drawn again over what an earlier pass laid down - depth-tested GEQUAL, writing no depth: the
    // lightmaps, the characters' shadow receivers - nearer by a share pose.z of its depth. On the GS both passes come
    // from the same VU1 sums and land on the same Z; on the GPU a triangle clipped one way in one pass and another way
    // in the next can land a few bits off, and the second pass then fails in places from one frame to the next.
    o.pos.z *= 1.0 + pose.z;

    float4 vc = misc.y > 0.5 ? round(i.colour * 255.0) : float4(128.0, 128.0, 128.0, 128.0);
    float3 n = i.normal;
    float3 v = normalize(p[4].xyz - i.pos);   // (the lit metal: the eye in q4)
    float nv = dot(n, v);
    float4 c = vc;
    int cm = (int)mode.x;
    if (cm == 1)
        c = P0;                                                         // 0x13b0
    else if (cm == 2)
        c = min(vc * P0, 255.0);                                        // 0x2150
    else if (cm == 3)
        c = clamp(vc * P0 + p[1], 0.0, 255.0);                          // 0x1c20
    else if (cm == 4)
        c = float4(P0.rgb, ((int)vc.a & 128) ? 128.0 : 0.0);            // 0x21b0
    else if (cm == 5)
        c = float4(min(p[2].rgb * p[1].rgb * (max(dot(n, P0.xyz), 0.0) + P0.w), 255.0), p[2].w * 128.0); // one light
    else if (cm == 6)
    {
        // 0x21f0: L1 = p8, L2 = p9, the eye p10 (in the mesh's space) - at the picture's moment (N9 stage 3: from the frame
        // before's pose, the lights at unit length as each pose's are).
        const float3 l1 = picture.x >= 1.0 ? p[8].xyz : normalize(posed(lightsB[0], p[8]).xyz);
        const float3 l2 = picture.x >= 1.0 ? p[9].xyz : normalize(posed(lightsB[1], p[9]).xyz);
        const float3 eye = posed(lightsB[2], p[10]).xyz;
        float3 ve = normalize(eye - i.pos);
        float3 r = 2.0 * dot(n, ve) * n - ve;
        float rl = max(dot(r, l2), 0.0);
        c = float4(lightsOf(n, l1, l2, p[6].rgb, p[2].rgb, p[1].w, p[5].w, p[3].rgb * p[2].rgb), p[4].w * pow(rl, 8.0));
    }
    else if (cm == 7)
        c = float4((max(0.0, 255.0 * p[8].x * ((1.0 - nv) * p[8].y))).xxx, p[11].w * 128.0); // the reflection
    else if (cm == 8)
        c = float4(lightsOf(n, p[5].xyz, p[9].xyz, p[6].rgb, p[10].rgb, p[5].w, p[9].w, p[7].rgb * p[6].rgb), p[11].w * 128.0); // the lit base
    else if (cm == 9)
        c = vc * P0;                                                    // an instance's tint
    else if (cm == 10)
        c = float4(255.0, 0.0, 255.0, 128.0);                             // (debug: sprites in magenta)
    else if (cm == 13)
        c = float4(P0.rgb, ((int)vc.a & 128) ? 128.0 * P0.w : 0.0);   // 0x1df0: the projector's colour (alpha q8.w * bit 7)
    else if (cm == 15)
        c = min(trunc(vc * p[5]), 255.0);                                 // the water's reflection: the colour times q21
    else if (cm == 17)
        c = float4(trunc(p[10].rgb), clamp(trunc(p[11].w * max(dot(i.normal, p[9].xyz), 0.0) + p[11].z), 0.0, 255.0)); // the bubble: q10, alpha N.L
    else if (cm == 18)
        c = float4(trunc(min(p[5].rgb * (max(dot(i.normal, p[6].xyz), 0.0) + p[6].w), 255.0)), trunc(p[5].w)); // 0x1e70: one light
    else if (cm == 19)
    {
        // 0x2920: facing the eye (q4), q5 lit and alpha by (1 - N.V)^2; facing away, 16.
        float nv4 = dot(i.normal, normalize(p[4].xyz - i.pos));
        c = nv4 < 0.0 ? float4(16.0, 16.0, 16.0, 16.0)
                      : float4(trunc(min(p[5].rgb * (max(dot(i.normal, p[6].xyz * p[7].y), 0.0) + p[7].x), 255.0)),
                               trunc(min(p[5].w * ((1.0 - nv4) * (1.0 - nv4) * p[7].z + p[7].w), 255.0)));
    }
    else if (cm == 20)
        c = trunc(lerp(p[1], p[2], smearT));                              // 0x2220: q1, or q2 where moved
    if ((int)mode.z == 5)
        c = cityColour;                                                   // (TODO 162) 0x22c0: the city's face
    // (cm 14, the water's base: the vertex's colour as it is.)
    // Instances (0x1ac0): a triangle with a corner outside the guard band is not drawn (CLIP of
    // q10 * position + q11 * w against its own w) - the pixel shader drops any pixel of it.
    o.cull = 0.0;
    // 0x1df0: a triangle whose three vertices are all outside one side of the projector's box (ST x, y
    // from 0 to 1, z above -1) is not drawn - per side 1 outside, -1e6 inside: all three outside, and
    // only then, leaves it above 0 over the whole triangle.
    o.box = float4(-1e6, -1e6, -1e6, -1e6);
    o.boxz = -1e6;
    if (cm == 13)
    {
        float3 b = (p[12] * i.pos.x + p[13] * i.pos.y + p[14] * i.pos.z + p[15]).xyz;
        o.box = float4(b.x > 1.0 ? 1.0 : -1e6, b.x < 0.0 ? 1.0 : -1e6, b.y > 1.0 ? 1.0 : -1e6, b.y < 0.0 ? 1.0 : -1e6);
        o.boxz = b.z < -1.0 ? 1.0 : -1e6;
    }
    if (cm == 9)
    {
        float4 guard = p[1] * s + p[2] * s.w;
        o.cull = any(abs(guard.xyz) > abs(guard.w)) ? 1.0 : 0.0;
    }
    if ((int)mode.z == 1)
        c.a = clamp(dot(p[16].xyz, i.pos) + p[16].w, 0.0, 128.0);         // 0x3660
    o.colour = c / 128.0;
    o.colourP = o.colour;
    {
        const bool inside = o.pos.w > 0.0 && o.pos.z >= 0.0 && o.pos.z <= o.pos.w && all(o.clipA >= 0.0) && all(o.clipB >= 0.0);
        o.whole = inside ? 1.0 : 0.0;
    }
    if (clipOn.z > 1.5)
    {
        // (A clipping draw's triangle known to lie inside all its planes, drawn without the clipping
        // shader: as that shader passes it on - whole, no distances.)
        o.whole = 1.0;
        o.clipA = float4(1.0, 1.0, 1.0, 1.0);
        o.clipB = float2(1.0, 1.0);
    }

    float2 st = mode.w > 0.5 ? lerp(p[19].xy, p[20].xy, i.uv) : i.uv;
    // The GS's Q at the vertex (TEX1's level: log2(1 / Q)): VU1's 1 / w, linear across the screen;
    // the lit metal's reflection sends (Rz + 1) * Q; a 2D draw its own Q.
    o.gsq = 1.0 / max(abs(w), 1e-12);
    int sm = (int)mode.y;
    if (sm == 1)
        st = (p[12] * i.pos.x + p[13] * i.pos.y + p[14] * i.pos.z + p[15]).xy;
    else if (sm == 2)
        st = (p[12] * i.uv.x + p[13] * i.uv.y + p[14]).xy;
    else if (sm == 3)
    {
        float3 r = 2.0 * nv * n - v;
        float3 e = normalize(P0.xyz * r.x + p[1].xyz * r.y + p[2].xyz * r.z);
        st = 0.5 + e.xy * 0.5 / (e.z + 1.0);
        o.gsq *= max(e.z + 1.0, 1e-6);
    }
    else if (sm == 4)
        st = i.uv + p[4].xy; // the water's base: ST moved by the current (q4.xy)
    else if (sm == 5)
        st = waterMap;       // the water's reflection: the sphere map
    else if (sm == 6)
    {
        // The bubble's: the view (from the eye q8, to the moved vertex) reflected about the moved
        // normal, 0.5 + R.xy * 0.5 / |R + (0, 0, 1)|.
        float3 ve = normalize(p[8].xyz - ipos);
        float3 r = 2.0 * dot(mnormal, ve) * mnormal - ve;
        st = 0.5 + r.xy * 0.5 / sqrt(r.x * r.x + r.y * r.y + (r.z + 1.0) * (r.z + 1.0));
    }
    else if (sm == 7)
    {
        // 0x1e70: the vertex moved along its normal by q4.w, through q0-q3, projected.
        float3 m = i.pos + i.normal * p[4].w;
        float4 k = P0 * m.x + p[1] * m.y + p[2] * m.z + p[3];
        st = 0.5 + 0.5 * k.xy / k.z;
    }
    else if (sm == 8)
    {
        // 0x2920: the reflected view through the screen transform's axes.
        float3 ve = normalize(p[4].xyz - i.pos);
        float3 r = 2.0 * dot(i.normal, ve) * i.normal - ve;
        float3 e = row0.xyz * r.x + row1.xyz * r.y + row2.xyz * r.z;
        st = 0.5 + 0.5 * e.xy / max(length(e), 1e-20);
    }
    if ((int)mode.z == 5)
        st = citySt;
    o.uv = st * texScale.xy;
    o.q = texScale.z > 0.5 ? i.size : 1.0; // (S, T, Q interpolated, divided per pixel)
    if (texScale.z > 0.5)
        o.gsq = i.size;
    return o;
}

)"
                              R"(// The core's clipping done as 0x0480 does it (a draw that clips: clipOn.x): each triangle cut by the six
// planes of its clipping matrix (the distances in clipA / clipB), the new corners at o + (i - o) * d_o /
// (d_o - d_i) with every value linear in clip space - their colours as the VU gives them - and the
// polygon then drawn with its colour screen-linear (whole = 1), as the GS steps the clipped polygon VU1
// sends it. (Left to the GPU's clipper, a triangle reaching behind the eye could only take the
// perspective-correct colour over all of it.)
float clipDistanceOf(VsOut v, int k)
{
    return k == 0 ? v.clipA.x : k == 1 ? v.clipA.y : k == 2 ? v.clipA.z : k == 3 ? v.clipA.w : k == 4 ? v.clipB.x : v.clipB.y;
}

VsOut lerpOut(VsOut a, VsOut b, float t)
{
    VsOut o;
    o.pos = lerp(a.pos, b.pos, t);
    o.colour = lerp(a.colourP, b.colourP, t);
    o.colourP = o.colour;
    o.whole = 1.0;
    o.uv = lerp(a.uv, b.uv, t);
    o.cull = lerp(a.cull, b.cull, t);
    o.q = lerp(a.q, b.q, t);
    o.box = lerp(a.box, b.box, t);
    o.boxz = lerp(a.boxz, b.boxz, t);
    o.gsq = lerp(a.gsq, b.gsq, t);
    o.clipA = lerp(a.clipA, b.clipA, t);
    o.clipB = lerp(a.clipB, b.clipB, t);
    return o;
}

[maxvertexcount(9)]
void gsClip(triangle VsOut tri[3], inout TriangleStream<VsOut> stream)
{
    // (Most triangles of a mesh that clips lie wholly inside every plane, or wholly outside one: those
    // leave at once - the same output the polygon loop below gives them - without its arrays of nine
    // corners, which spill to memory and made this shader ~3 ms of a 2592x1080 frame on an RX 5700 XT.)
    bool inside = clipOn.z > 0.5;
    if (inside)
    {
        [unroll] for (int e = 0; e < 6; ++e)
        {
            const float d0 = clipDistanceOf(tri[0], e), d1 = clipDistanceOf(tri[1], e), d2 = clipDistanceOf(tri[2], e);
            if (d0 < 0.0 && d1 < 0.0 && d2 < 0.0)
                return;
            inside = inside && d0 >= 0.0 && d1 >= 0.0 && d2 >= 0.0;
        }
    }
    if (inside)
    {
        [unroll] for (int s = 0; s < 3; ++s)
        {
            VsOut v = tri[s];
            v.whole = 1.0;
            v.clipA = float4(1.0, 1.0, 1.0, 1.0);
            v.clipB = float2(1.0, 1.0);
            stream.Append(v);
        }
        return;
    }
    VsOut poly[9];
    VsOut next[9];
    [unroll] for (int z = 0; z < 9; ++z)
    {
        poly[z] = tri[0];
        next[z] = tri[0];
    }
    uint n = 3;
    poly[0] = tri[0];
    poly[1] = tri[1];
    poly[2] = tri[2];
    [unroll] for (int k = 0; k < 6; ++k)
    {
        uint m = 0;
        for (uint e = 0; e < n; ++e)
        {
            const VsOut a = poly[e], b = poly[e + 1 < n ? e + 1 : 0];
            const float da = clipDistanceOf(a, k), db = clipDistanceOf(b, k);
            if (da >= 0.0 && m < 9)
                next[m++] = a;
            if ((da >= 0.0) != (db >= 0.0) && m < 9)
                next[m++] = lerpOut(a, b, da / (da - db));
        }
        n = m;
        for (uint c = 0; c < n; ++c)
            poly[c] = next[c];
        if (n < 3)
            return;
    }
    // (As a strip: 0, 1, n-1, 2, n-2, ... - the polygon is convex. Its corners inside every plane now:
    // the distances out of the GPU's way, the colour screen-linear.)
    // (HLSL's ?: evaluates both sides: the corner's index worked out without side effects in it.)
    for (uint s = 0; s < n; ++s)
    {
        const uint at = s == 0 ? 0 : (s & 1) ? (s + 1) / 2 : n - s / 2;
        VsOut v = poly[at];
        v.whole = 1.0;
        v.clipA = float4(1.0, 1.0, 1.0, 1.0);
        v.clipB = float2(1.0, 1.0);
        stream.Append(v);
    }
}

)"
                              R"(// The texture as the GS samples it (TEX1, lod.x on): the level LOD = log2(1 / Q) * 2^L + K (or K alone,
// LCM), at most MXL; at LOD <= 0 the MMAG filter on level 0, above it MMIN's - level 0, the nearest level
// or between the two. The remaster's large images (s times the GS's size, base = log2 s) are read at the
// GS's level plus up to `base` levels finer where the screen shows that detail (their own footprint):
// never coarser than the GS's image, never a level the GS would not blur to. Level 0 linear through the
// anisotropic filter with its footprint held to the GS image's size (no level past it), the rest at the
// level the GS picks.
// The pixel shader's variants (TODO 94): what a draw never uses left out when it is compiled - the same arithmetic on
// the paths kept, far fewer registers (the one shader for every draw ran at a fraction of the GPU's pixel rate). Set by
// psVariantOf on the C++ side; unset, every path is in and picked at run time, as before.
// PSV_TEX: 0 untextured, 1 a game texture (sampleGs, or its stand-in), 3 anything (the reads of render targets too).
// PSV_LOD: 0 TEX1 off (the anisotropic sample), 1 TEX1 on, 2 either. PSV_CLAMP, PSV_ATEST, PSV_DATE, PSV_CULL: the
// CLAMP, the alpha test, the destination alpha test, an instance's guard band / the projector's box - 0 never, 1 maybe.
#ifndef PSV_TEX
#define PSV_TEX 3
#endif
#ifndef PSV_LOD
#define PSV_LOD 2
#endif
#ifndef PSV_CLAMP
#define PSV_CLAMP 1
#endif
#ifndef PSV_ATEST
#define PSV_ATEST 1
#endif
#ifndef PSV_DATE
#define PSV_DATE 1
#endif
#ifndef PSV_CULL
#define PSV_CULL 1
#endif
float4 sampleGs(float2 uv, float2 uvx, float2 uvy, float gsq)
{
#if PSV_LOD != 1
    if (PSV_LOD == 0 || lod.x < 0.5)
        return clipOn.w > 0.5 ? tex.Sample(smpAlt, uv) : tex.Sample(smp, uv);
#endif
#if PSV_LOD != 0
    float tw, th, levels;
    tex.GetDimensions(0, tw, th, levels);
    const float2 size = float2(tw, th);
    const float base = max(log2(tw / max(wrapS.w, 1.0)), 0.0);
    const float2 ax = uvx * size, ay = uvy * size;
    const float d = 0.5 * log2(max(max(dot(ax, ax), dot(ay, ay)), 1e-20));
    const float g = lodf.x > 0.5 ? lod.y : log2(1.0 / max(abs(gsq), 1e-30)) * lod.z + lod.y;
    const bool mag = g <= 0.0;
    float level = 0.0;
    if (!mag && lodf.w > 0.5)
    {
        level = min(g, lod.w);
        if (lodf.w < 1.5)
            level = floor(level + 0.5);
    }
    const bool smooth = base > 0.0 || (mag ? lodf.y > 0.5 : lodf.z > 0.5);
    if (level <= 0.0 && smooth)
    {
        const float k = exp2(min(base - d, 0.0));
        return clipOn.w > 0.5 ? tex.SampleGrad(smpAlt, uv, uvx * k, uvy * k) : tex.SampleGrad(smp, uv, uvx * k, uvy * k);
    }
    const float n = level + clamp(d, 0.0, base);
    return smooth ? tex.SampleLevel(smpLinear, uv, n) : tex.SampleLevel(smpPoint, uv, n);
#endif
}

// A supersampled target read into the picture (the supersampling setting): the average of four bilinear taps a
// quarter of the picture's pixel from its centre - the whole footprint (1.5-2 texels a pixel) weighed, where one
// bilinear tap at 1.5x would skip texels. (Held inside the texture: the sampler wraps.)
float4 downsample(float2 uv, float2 uvx, float2 uvy)
{
    float tw, th;
    tex.GetDimensions(tw, th);
    const float2 lo = 0.5 / float2(tw, th), hi = 1.0 - lo;
    const float2 a = 0.25 * (uvx + uvy), b = 0.25 * (uvx - uvy);
    return 0.25 * (tex.SampleLevel(smpLinear, clamp(uv + a, lo, hi), 0.0) + tex.SampleLevel(smpLinear, clamp(uv - a, lo, hi), 0.0) +
                   tex.SampleLevel(smpLinear, clamp(uv + b, lo, hi), 0.0) + tex.SampleLevel(smpLinear, clamp(uv - b, lo, hi), 0.0));
}

// (N10) A shrink of the picture into an effect's buffer at the GS's own size (the glow's 128 x 128: 27 x 11 of the
// picture's pixels a texel at 3440 x 1440, where the GS shrank 5 x 3.5): the average over the pixel's whole footprint,
// up to 16 x 16 bilinear taps (each 2 x 2 texels) - one tap took a few of the picture's pixels and skipped the rest, and
// the glow's bright spots shimmered as the camera moved. The chain after it, at the GS's size, as before.
float4 boxdown(float2 uv, float2 uvx, float2 uvy)
{
    float tw, th;
    tex.GetDimensions(tw, th);
    const float2 lo = 0.5 / float2(tw, th), hi = 1.0 - lo;
    const float2 span = max(abs(uvx), abs(uvy));
    const int2 n = clamp((int2)ceil(span * float2(tw, th) * 0.5), int2(1, 1), int2(16, 16));
    float4 s = float4(0.0, 0.0, 0.0, 0.0);
    [loop] for (int y = 0; y < n.y; ++y)
        [loop] for (int x = 0; x < n.x; ++x)
        {
            const float2 o = (float2(x, y) + 0.5) / (float2)n - 0.5;
            s += tex.SampleLevel(smpLinear, clamp(uv + o * span, lo, hi), 0.0);
        }
    return s / (float)(n.x * n.y);
}

// The GS's texture function (TEX0 TFX, TCC) and alpha test (TEST ATE, ATST, AREF, AFAIL), in its
// units: 1.0 = 128 for colours and alphas alike.
// (TODO 94, measuring aids - TF_NATIVE_ZWALT 128 / 256: a pixel shader that does nothing, and one that reads every
// interpolant and nothing else.)
float4 psTiny() : SV_Target
{
    return float4(0.5, 0.5, 0.5, 1.0);
}
float4 psInterp(VsOut i) : SV_Target
{
    return saturate(i.colour + i.colourP + float4(i.uv, i.q + i.gsq, i.whole + i.cull) + i.box + i.boxz + i.clipA + float4(i.clipB, 0.0, 0.0)) * 0.1;
}
float4 ps(VsOut i) : SV_Target
{
    if (down.y > 0.5)
        return float4(0.5, 0.5, 0.5, 1.0); // (TF_NATIVE_FLATPS: a measuring aid)
#if PSV_CULL
    if (i.cull > 0.0001 || any(i.box > 0.0001) || i.boxz > 0.0001)
        discard;
#endif
    i.uv /= i.q;
    const float2 uvx = ddx(i.uv), uvy = ddy(i.uv); // (before the CLAMP: the footprint for TEX1's level)
#if PSV_CLAMP
    if ((wrapS.x > 0.5 || wrapT.x > 0.5) && texScale.w < 1.5)
    {
        // The GS's CLAMP: a coordinate held to the texture's edge texels (or to a region's), where
        // the sampler would wrap round - the mirrored halves of an icon meet without a seam.
        // (TODO 125: by the pixel's footprint where that is larger than a texel - a large image drawn
        // smaller than its size is filtered from a smaller level, over several texels: held only half a
        // texel from the edge, the filter still reached round to the far edge, and the HUD's Mini-Con
        // icons, their mirrored halves meeting at u = 1, showed thin lines along their quads' edges.
        // Magnified - every texture at the PS2's size - the footprint is under a texel: as before.)
        float tw, th;
        tex.GetDimensions(tw, th);
        const float2 hold = max(float2(0.5 / tw, 0.5 / th), max(abs(uvx), abs(uvy)));
        if (wrapS.x > 1.5)
            i.uv.x = clamp(i.uv.x, (wrapS.y + 0.5) / wrapS.w, (wrapS.z + 0.5) / wrapS.w);
        else if (wrapS.x > 0.5)
            i.uv.x = clamp(i.uv.x, hold.x, 1.0 - hold.x);
        if (wrapT.x > 1.5)
            i.uv.y = clamp(i.uv.y, (wrapT.y + 0.5) / wrapT.w, (wrapT.z + 0.5) / wrapT.w);
        else if (wrapT.x > 0.5)
            i.uv.y = clamp(i.uv.y, hold.y, 1.0 - hold.y);
    }
#endif
#if PSV_TEX == 3
    if ((int)mode.x == 11)
        return float4(tex.Sample(smp, i.uv).rgb, 1.0); // (debug: the raw sample)
    if ((int)mode.x == 12)
        return float4(frac(i.uv), 0.0, 1.0);           // (debug: the coordinates)
    // (Untextured - PRIM's TME off, dest.w: the GS writes the vertex's colour as it is, 0x80 a half;
    // the colour here is in units of 0x80, so untextured draws came out twice as bright - the glow's
    // threshold, a constant subtracted, among them. TF_NATIVE_UNTEXTURED=0: as before.)
    float4 t = flags.x > 0.5 ? (standIn.w > 1.5 ? float4(standIn.rgb, standIn.w - 2.0) : down.x > 1.5 ? boxdown(i.uv, uvx, uvy) : down.x > 0.5 ? downsample(i.uv, uvx, uvy) : sampleGs(i.uv, uvx, uvy, i.gsq)) : dest.w > 0.5 ? float4(128.0 / 255.0, 128.0 / 255.0, 128.0 / 255.0, 1.0) : float4(1.0, 1.0, 1.0, 1.0);
#elif PSV_TEX == 1
    float4 t = standIn.w > 1.5 ? float4(standIn.rgb, standIn.w - 2.0) : sampleGs(i.uv, uvx, uvy, i.gsq);
#else
    float4 t = dest.w > 0.5 ? float4(128.0 / 255.0, 128.0 / 255.0, 128.0 / 255.0, 1.0) : float4(1.0, 1.0, 1.0, 1.0);
#endif
#if PSV_TEX == 3
    if (texScale.w > 1.5)
    {
        // An 8-bit-in-the-top-byte read (T8H) of a render target this frame drew (the shadow
        // textures): the game's 16-bit copy moves each pixel's green into its top byte, so the index
        // is the target's green - through the palette, filtered between four texels, the texture's
        // own CLAMP / REPEAT applied in its own extent (the part of the target it covers).
        float tw, th;
        tex.GetDimensions(tw, th);
        float2 extent = texScale.xy * float2(tw, th); // the texture's size in the target's pixels
        float2 pos = i.uv * float2(tw, th) - 0.5;
        int2 p0 = (int2)floor(pos);
        float2 f = pos - (float2)p0;
        int2 n = max(int2(extent + 0.5), int2(1, 1));
        float4 c[4];
        [unroll] for (int k = 0; k < 4; ++k)
        {
            int2 q = p0 + int2(k & 1, k >> 1);
            q.x = wrapS.x > 0.5 ? clamp(q.x, 0, n.x - 1) : ((q.x % n.x) + n.x) % n.x;
            q.y = wrapT.x > 0.5 ? clamp(q.y, 0, n.y - 1) : ((q.y % n.y) + n.y) % n.y;
            uint index = (uint)round(saturate(tex.Load(int3(q, 0)).g) * 255.0);
            c[k] = palette.Load(int3(index, 0, 0));
        }
        t = lerp(lerp(c[0], c[1], f.x), lerp(c[2], c[3], f.x), f.y);
    }
    else if (texScale.w > 0.5)
    {
        // The depth fog: the game shuffles bits 8-15 of each pixel's Z24 into its top byte and reads
        // that (T8H) through a palette; here from the depth itself, at the same pixel.
        float d = depthTex.Load(int3(i.pos.xy, 0));
        uint z = (uint)min(d * 268435456.0, 16777215.0); // (the depth is GS Z * 16 / 2^32: Z24 back)
        t = palette.Load(int3((z >> 8) & 255u, 0, 0));
        if ((int)mode.x == 16)
            return float4(((z >> 8) & 255u) / 255.0, saturate(d * 4294967296.0 / 65536.0), saturate(d * 268435456.0 / 65536.0), 0.0); // (debug: TF_NATIVE_FOGDEBUG; alpha 0: the fog's blend keeps the source)
    }
#endif
    if (flags.w > 0.5)
        t.a = 1.0; // (PSMCT24 / PSMZ24: TEXA's TA0, 0x80 - a frame buffer read as 24-bit has no alpha of its own)
    float4 v = (clipOn.y > 0.5 && i.whole > 0.99999) ? i.colour : i.colourP;
    int tfx = (int)flags.y;
    bool tcc = flags.z > 0.5;
    float4 c;
    if (tfx == 1)
        c = float4(t.rgb, tcc ? t.a : v.a);                          // DECAL
    else if (tfx == 0)
        c = float4(t.rgb * v.rgb, tcc ? t.a * v.a : v.a);            // MODULATE
    else
        // HIGHLIGHT(2): the GS adds the vertex alpha to the colour as it is - 0x80 is 128 of 255 - and the alpha here is in units
        // of 0x80 (TODO 61: it added 255, twice the GS's: a lit mesh's alpha 0x80 made it solid white).
        c = float4(t.rgb * v.rgb + v.a * (128.0 / 255.0), tcc ? (tfx == 2 ? t.a + v.a : t.a) : v.a);
#if PSV_ATEST
    if (test.x > 0.5)
    {
        float as = c.a * 128.0;
        int atst = (int)test.y;
        bool passed = atst == 1 || (atst == 2 && as < test.z) || (atst == 3 && as <= test.z) || (atst == 4 && abs(as - test.z) < 0.5) ||
                      (atst == 5 && as >= test.z) || (atst == 6 && as > test.z) || (atst == 7 && abs(as - test.z) >= 0.5);
        if (!passed && (int)test.w == 0)
            discard;
        if (passed && (int)test.w == 4)
            discard; // (AFAIL RGB_ONLY's second pass: the failing pixels, their colour without alpha)
    }
#endif
#if PSV_DATE
    if (dest.x > 0.5)
    {
        // The GS's destination alpha test: the frame's alpha bit 7 (0x80 is 1.0 here, 0x7F 0.996).
        bool high = palette.Load(int3(i.pos.xy, 0)).a > 0.998;
        if (high != (dest.x > 1.5))
            discard;
    }
#endif
    float3 rgb = saturate(c.rgb);
    if (dest.y > 0.5)
        rgb = saturate(rgb * (dest.y > 1.5 ? dest.z : min(c.a, 255.0 / 128.0))); // (Cs * As or Cs * FIX: blendOf's premultiply)
    return float4(rgb, saturate(c.a));
}
)";

        // The sun's occlusion probe (roadmap N3a, nr_occlusion.cpp): the 3D target's depth over the rect the game's
        // visibility test copied, point-sampled into a 64 x 64 target (R32_FLOAT) as its CopySurfaceRect samples it into
        // the OcclusionBuffer - read back once the frame is done. rect: its corner in the target's pixels and the step a
        // probe pixel; size: the target's pixels (t2: its depth, as the fog's pass reads it).
        const char kProbeShader[] = R"(
cbuffer Probe : register(b0)
{
    float4 rect;
    float4 size;
};
Texture2D<float> depthTex : register(t2);
float4 probeVs(uint id : SV_VertexID) : SV_Position
{
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
float probePs(float4 pos : SV_Position) : SV_Target
{
    float2 s = rect.xy + (floor(pos.xy) + 0.5) * rect.zw;
    int2 q = clamp(int2(floor(s)), int2(0, 0), int2(size.xy) - 1);
    return depthTex.Load(int3(q, 0));
}
)";

        // (TODO 27) The warp tunnel's sparks (nr_warp.cpp, drawCloud): each point of a set an instance - its place through
        // the set's rows to the GS's screen (kept on the eye's side, s.w < 0, as VU1 keeps it), narrowed about the centre for
        // Hor+, and a soft dot there (a Gaussian, its radius in the picture's pixels) added to the picture.
        const char kCloudShader[] = R"(
cbuffer Cloud : register(b0)
{
    float4 rows[4];  // local -> GS: s = x rows[0] + y rows[1] + z rows[2] + rows[3]
    float4 screen;   // the picture's GS origin x, y (less half a pixel), 2 / its GS width, 2 / its GS height
    float4 colour;   // added at a dot's centre
    float4 shape;    // a dot's half size in clip units (3 sigma) x, y; Hor+'s narrowing; the GS's centre x
};
struct CloudOut
{
    float4 pos : SV_Position;
    float2 d : TEXCOORD0;
};
CloudOut cloudVs(float3 p : POSITION, uint id : SV_VertexID)
{
    CloudOut o;
    const float2 corner = float2((id & 1) ? 1.0 : -1.0, (id & 2) ? -1.0 : 1.0);
    o.d = corner * 3.0;
    const float4 s = rows[0] * p.x + rows[1] * p.y + rows[2] * p.z + rows[3];
    if (!(s.w < 0.0))
    {
        o.pos = float4(4.0, 4.0, 0.5, 1.0); // (behind the eye: none)
        return o;
    }
    float2 gs = s.xy / s.w;
    gs.x = shape.w + (gs.x - shape.w) * shape.z;
    const float2 c = float2((gs.x - screen.x) * screen.z - 1.0, 1.0 - (gs.y - screen.y) * screen.w);
    o.pos = float4(c + corner * shape.xy, 0.5, 1.0);
    return o;
}
float4 cloudPs(CloudOut i) : SV_Target
{
    return float4(colour.rgb * exp(-0.5 * dot(i.d, i.d)), 0.0);
}
)";

        // The anti-aliasing pass (TF_NATIVE_AA): FXAA 3.11's "quality" algorithm (preset 39: 12
        // search steps), over the picture as it is once the 3D is in it and before the 2D over it -
        // the HUD, menus and text stay as drawn. A full-screen triangle; the picture copied into
        // aaSource first (t0), the result drawn back into the picture with its alpha kept (the 2D
        // after may blend with it). Luma from the colour (the picture's alpha is not luma).
        const char kAaShader[] = R"(
cbuffer Aa : register(b0)
{
    float4 aaFrame; // 1 / width, 1 / height, width, height
    float4 aaTune;  // x: sub-pixel amount, y: edge threshold, z: its minimum
};
Texture2D<float4> aaSource : register(t0);
SamplerState aaLinear : register(s1);

struct AaOut
{
    float4 pos : SV_Position;
};

AaOut aaVs(uint id : SV_VertexID)
{
    AaOut o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.pos = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float aaLuma(float3 c)
{
    return dot(c, float3(0.299, 0.587, 0.114));
}

float aaLumaOff(int2 c, int2 off)
{
    return aaLuma(aaSource.Load(int3(clamp(c + off, int2(0, 0), int2(aaFrame.zw) - 1), 0)).rgb);
}

float4 aaTap(float2 p)
{
    return aaSource.SampleLevel(aaLinear, clamp(p, 0.5 * aaFrame.xy, 1.0 - 0.5 * aaFrame.xy), 0.0);
}

static const float kAaStep[12] = {1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0};

float4 aaPs(AaOut i) : SV_Target
{
    const float2 rcp = aaFrame.xy;
    const int2 c = int2(i.pos.xy);
    const float4 rgbyM = aaSource.Load(int3(c, 0));
    const float lumaM = aaLuma(rgbyM.rgb);
    float lumaS = aaLumaOff(c, int2(0, 1));
    float lumaE = aaLumaOff(c, int2(1, 0));
    float lumaN = aaLumaOff(c, int2(0, -1));
    float lumaW = aaLumaOff(c, int2(-1, 0));
    const float rangeMax = max(max(lumaN, lumaW), max(lumaE, max(lumaS, lumaM)));
    const float rangeMin = min(min(lumaN, lumaW), min(lumaE, min(lumaS, lumaM)));
    const float range = rangeMax - rangeMin;
    if (range < max(aaTune.z, rangeMax * aaTune.y))
        return rgbyM; // (no edge here)
    const float lumaNW = aaLumaOff(c, int2(-1, -1)), lumaSE = aaLumaOff(c, int2(1, 1));
    const float lumaNE = aaLumaOff(c, int2(1, -1)), lumaSW = aaLumaOff(c, int2(-1, 1));
    const float lumaNS = lumaN + lumaS, lumaWE = lumaW + lumaE;
    const float lumaNESE = lumaNE + lumaSE, lumaNWNE = lumaNW + lumaNE;
    const float lumaNWSW = lumaNW + lumaSW, lumaSWSE = lumaSW + lumaSE;
    const float edgeHorz = abs(-2.0 * lumaW + lumaNWSW) + abs(-2.0 * lumaM + lumaNS) * 2.0 + abs(-2.0 * lumaE + lumaNESE);
    const float edgeVert = abs(-2.0 * lumaS + lumaSWSE) + abs(-2.0 * lumaM + lumaWE) * 2.0 + abs(-2.0 * lumaN + lumaNWNE);
    const bool horzSpan = edgeHorz >= edgeVert;
    // (Sub-pixel aliasing: how far the centre stands out of its 3x3 neighbourhood's average.)
    const float subpixB = ((lumaNS + lumaWE) * 2.0 + lumaNWSW + lumaNESE) * (1.0 / 12.0) - lumaM;
    const float subpixC = saturate(abs(subpixB) / range);
    if (!horzSpan)
    {
        lumaN = lumaW;
        lumaS = lumaE;
    }
    float lengthSign = horzSpan ? rcp.y : rcp.x;
    const float gradientN = lumaN - lumaM, gradientS = lumaS - lumaM;
    const bool pairN = abs(gradientN) >= abs(gradientS);
    const float gradient = max(abs(gradientN), abs(gradientS));
    if (pairN)
        lengthSign = -lengthSign;
    const float lumaNN = pairN ? lumaN + lumaM : lumaS + lumaM;
    // The edge's two ends searched along it, half a pixel over (between the pixel and its pair).
    float2 posM = i.pos.xy * rcp;
    float2 posB = posM;
    const float2 offNP = horzSpan ? float2(rcp.x, 0.0) : float2(0.0, rcp.y);
    if (horzSpan)
        posB.y += lengthSign * 0.5;
    else
        posB.x += lengthSign * 0.5;
    float2 posN = posB - offNP * kAaStep[0];
    float2 posP = posB + offNP * kAaStep[0];
    const float gradientScaled = gradient * 0.25;
    const bool lumaMLTZero = lumaM - lumaNN * 0.5 < 0.0;
    float lumaEndN = aaLuma(aaTap(posN).rgb) - lumaNN * 0.5;
    float lumaEndP = aaLuma(aaTap(posP).rgb) - lumaNN * 0.5;
    bool doneN = abs(lumaEndN) >= gradientScaled;
    bool doneP = abs(lumaEndP) >= gradientScaled;
    if (!doneN)
        posN -= offNP * kAaStep[1];
    if (!doneP)
        posP += offNP * kAaStep[1];
    [unroll] for (int k = 2; k < 12; ++k)
    {
        if (doneN && doneP)
            break;
        if (!doneN)
            lumaEndN = aaLuma(aaTap(posN).rgb) - lumaNN * 0.5;
        if (!doneP)
            lumaEndP = aaLuma(aaTap(posP).rgb) - lumaNN * 0.5;
        doneN = abs(lumaEndN) >= gradientScaled;
        doneP = abs(lumaEndP) >= gradientScaled;
        if (!doneN)
            posN -= offNP * kAaStep[k];
        if (!doneP)
            posP += offNP * kAaStep[k];
    }
    const float dstN = horzSpan ? posM.x - posN.x : posM.y - posN.y;
    const float dstP = horzSpan ? posP.x - posM.x : posP.y - posM.y;
    const bool goodSpanN = (lumaEndN < 0.0) != lumaMLTZero;
    const bool goodSpanP = (lumaEndP < 0.0) != lumaMLTZero;
    const bool directionN = dstN < dstP;
    const float pixelOffset = 0.5 - min(dstN, dstP) / (dstN + dstP);
    const float pixelOffsetGood = (directionN ? goodSpanN : goodSpanP) ? pixelOffset : 0.0;
    const float subpixF = (-2.0 * subpixC + 3.0) * subpixC * subpixC;
    const float pixelOffsetSubpix = max(pixelOffsetGood, subpixF * subpixF * aaTune.x);
    if (horzSpan)
        posM.y += pixelOffsetSubpix * lengthSign;
    else
        posM.x += pixelOffsetSubpix * lengthSign;
    return float4(aaTap(posM).rgb, rgbyM.a);
}
)";

        // SMAA 1x (TF_NATIVE_AA=2, TODO 64 (c)): the reference SMAA.hlsl (ext/smaa, MIT - its LICENSE.txt), its porting
        // macros ours (SMAA_CUSTOM_SL: the effect-file samplers of its HLSL_4 path are not D3D12's), its "high" preset.
        // Three full-screen passes, the same constant (b0: 1 / width, 1 / height, width, height) and the same picture
        // copy (aaSource) as FXAA's: luma edges into an R8G8 target, the blend weights from them and the two lookup
        // textures into an RGBA8 one, then the picture blended with its neighbours from those, its alpha kept.
        const char kSmaaPrelude[] = R"(
cbuffer Smaa : register(b0)
{
    float4 smaaMetrics;
};
#define SMAA_RT_METRICS smaaMetrics
#define SMAA_CUSTOM_SL 1
#define SMAA_PRESET_HIGH 1
SamplerState smaaLinear : register(s4);
SamplerState smaaPoint : register(s5);
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(smaaLinear, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(smaaPoint, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(smaaLinear, coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(smaaLinear, coord)
#define SMAASamplePoint(tex, coord) tex.Sample(smaaPoint, coord)
#define SMAASampleOffset(tex, coord, offset) tex.Sample(smaaLinear, coord, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
#define SMAATexture2DMS2(tex) Texture2DMS<float4, 2> tex
#define SMAALoad(tex, pos, sample) tex.Load(pos, sample)
#define SMAAGather(tex, coord) tex.Gather(smaaLinear, coord, 0)
)";
        const char *const kSmaaSource[] = {
#include "ext/smaa/smaa_hlsl.inc"
        };
        const char kSmaaEntries[] = R"(
Texture2D smaaT0 : register(t0);
Texture2D smaaT1 : register(t1);
Texture2D smaaT2 : register(t2);

float2 smaaCorner(uint id, out float4 pos)
{
    float2 t = float2((id << 1) & 2, id & 2);
    pos = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return t;
}

struct SmaaEdgeOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float4 off[3] : TEXCOORD1;
};
SmaaEdgeOut smaaEdgeVs(uint id : SV_VertexID)
{
    SmaaEdgeOut o;
    o.uv = smaaCorner(id, o.pos);
    SMAAEdgeDetectionVS(o.uv, o.off);
    return o;
}
float2 smaaEdgePs(SmaaEdgeOut i) : SV_Target
{
    return SMAALumaEdgeDetectionPS(i.uv, i.off, smaaT0);
}

struct SmaaWeightOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float2 pix : TEXCOORD1;
    float4 off[3] : TEXCOORD2;
};
SmaaWeightOut smaaWeightVs(uint id : SV_VertexID)
{
    SmaaWeightOut o;
    o.uv = smaaCorner(id, o.pos);
    SMAABlendingWeightCalculationVS(o.uv, o.pix, o.off);
    return o;
}
float4 smaaWeightPs(SmaaWeightOut i) : SV_Target
{
    return SMAABlendingWeightCalculationPS(i.uv, i.pix, i.off, smaaT0, smaaT1, smaaT2, float4(0.0, 0.0, 0.0, 0.0));
}

struct SmaaBlendOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float4 off : TEXCOORD1;
};
SmaaBlendOut smaaBlendVs(uint id : SV_VertexID)
{
    SmaaBlendOut o;
    o.uv = smaaCorner(id, o.pos);
    SMAANeighborhoodBlendingVS(o.uv, o.off);
    return o;
}
float4 smaaBlendPs(SmaaBlendOut i) : SV_Target
{
    const float4 c = SMAANeighborhoodBlendingPS(i.uv, i.off, smaaT0, smaaT1);
    return float4(c.rgb, smaaT0.Load(int3(int2(i.pos.xy), 0)).a); // (the picture's alpha kept, as FXAA's)
}
)";

        using Constants = draw::Constants; // (the layout the shaders read: nr_draw.h)
        using PictureConstants = draw::PictureConstants; // (nr_draw.h; in video memory, a slot's own 256 bytes, written before each picture by a list of its own from the upload ring - kPictureUp a picture)
        constexpr UINT kMaxPictures = 24u; // (a frame's pictures from one recording: N9 stage 3; State's lists' 24)
        // (their timestamps a slot: each picture's start and end, then the frame's list's start - written by the list itself
        // and copied out after each picture: on the RX 5700 XT a timestamp in the small list before the frame's came out
        // after the picture's end)
        constexpr UINT kPicStride = 2u * kMaxPictures + 1u;
        constexpr UINT kPictureUp = 256u;
        constexpr UINT kPredicateAt = 248u; // (the predicate's 8 bytes in a picture's upload entry)
        static_assert(sizeof(PictureConstants) <= kPredicateAt, "the picture's constants overlap its predicate");
        constexpr UINT kConstantStride = (sizeof(Constants) + 255u) & ~255u; // (CBV alignment)
        constexpr UINT64 kConstantRing = 32ull << 20;                         // a frame's passes' constants
        constexpr UINT kInstanceStride = 208u;                                 // a pass's matrix, q0, the matrix's frame-before pose, halfway (IROW0-3, IPZERO, IROWB0-3, IROWM0-3)
        constexpr UINT kInstanceRing = 32768u;                                 // a frame's passes' (entries)

        struct GpuMesh
        {
            std::weak_ptr<const Mesh> mesh;
            uint32_t meshId = 0; // (P3 S3: the device's mesh - its buffers; 0: the frame's ring, or the grass's own)
            gfx::RingSpan ringVertices, ringIndices; // (a frame's own mesh: where it went in the Vertices ring)
            // (Until sub-step 3 binds through the device: the views it was made with.)
            D3D12_VERTEX_BUFFER_VIEW vbv{};
            D3D12_INDEX_BUFFER_VIEW ibv{};
            D3D12_VERTEX_BUFFER_VIEW vbvV{}; // (the video memory copy's, TODO 94 - meshVram)
            D3D12_INDEX_BUFFER_VIEW ibvV{};
            UINT count = 0;
            float lo[3] = {}, hi[3] = {}; // the vertices' box (the core's clipping: a mesh wholly inside needs none)
            bool bounded = false;
            bool indirect = false; // (grassgpu: drawn by the GPU's arguments at argsAt - its count is the GPU's)
            UINT64 argsAt = 0;
        };

        // (the box, the inside test and the clip runs: nr_draw.h)
        void boundsOf(const Mesh &mesh, GpuMesh &m) { draw::boundsOf(mesh, m.lo, m.hi); }
        bool boxInside(const GpuMesh &m, const float (*clip)[4]) { return draw::boxInside(m.lo, m.hi, clip); }
        using draw::ClipRun;
        using draw::clipRunsOf;

        struct GpuTexture
        {
            std::weak_ptr<const Texture> texture;
            gfx::TextureId id = 0; // (P3 S3: the device's texture; 0 the white one)
            UINT slot = 0; // its view in the shader-visible heap (the device's; kept here until sub-step 3 binds by id)
            UINT64 largeBytes = 0; // a large image's video memory (g_largeBytes, TODO 94); 0: a game texture
        };
        // The large images' video memory in all (TODO 94: largeImageBytes - the capture's eviction reads it).
        std::atomic<uint64_t> g_largeBytes{0};

        constexpr UINT kSlots = 8192; // shader-visible texture descriptors (slot 0: white)

        struct State
        {
            ID3D12Device *device = nullptr;
            ID3D12CommandQueue *queue = nullptr;
            ID3D12CommandAllocator *allocator = nullptr;
            ID3D12GraphicsCommandList *list = nullptr;
            ID3D12Fence *fence = nullptr;
            HANDLE fenceEvent = nullptr;
            UINT64 fenceValue = 0;
            ID3D12RootSignature *root = nullptr;
            ID3D12PipelineState *pipeline = nullptr;
            ID3D12DescriptorHeap *rtvHeap = nullptr, *dsvHeap = nullptr;
            ID3D12Resource *colour = nullptr, *depth = nullptr, *readback = nullptr;
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            UINT width = 0, height = 0;
            std::unordered_map<uint64_t, GpuMesh> meshes;
            std::unordered_map<uint64_t, GpuTexture> textures;
            ID3D12DescriptorHeap *srvHeap = nullptr;
            UINT srvSize = 0;
            std::vector<UINT> freeSlots;
            ID3D12Resource *white = nullptr;
            std::vector<ID3D12Resource *> uploads; // staging buffers, released once the GPU is done
            ID3DBlob *vsCode = nullptr, *psCode = nullptr, *gsCode = nullptr;
            ID3DBlob *psTiny = nullptr, *psInterp = nullptr; // (TF_NATIVE_ZWALT 128 / 256: measuring aids)
            std::unordered_map<uint64_t, ID3D12PipelineState *> pipelines; // by pipelineKey
            uint64_t pipelinesChanged = 0; // (counted up whenever an entry is added or replaced: the device's last-asked shortcut)
            ID3D12Resource *dynamic = nullptr; // this frame's meshes (the immediate draws), mapped for good
            uint8_t *dynamicMapped = nullptr;
            UINT64 dynamicUsed = 0;
            GpuMesh dynamicMesh;
            ID3D12Resource *constants = nullptr; // an upload heap, mapped for good
            uint8_t *constantsMapped = nullptr;
            ID3D12Resource *instances = nullptr; // (upload heap: each pass's matrix and q0, the vertex shader's per-instance stream)
            uint8_t *instancesMapped = nullptr;
            std::vector<Pass> passes;
            // The GS frame buffers the frame draws into, as render targets (by FBP; the picture's is
            // the main one: g.colour / g.depth, descriptor slot 0).
            // (P3 S3: the frame builder's side - which GS buffer, its GS size; the surfaces behind it are the
            // device's, named by `id` - see DeviceTarget below and nr_device.h.)
            struct Target
            {
                gfx::TargetId id = 0;         // the device's target (0: none - not made, or made in vain)
                UINT w = 0, h = 0;            // pixels
                float gsW = 0.0f, gsH = 0.0f; // GS pixels it covers
                bool main = false;
            };
            std::unordered_map<uint32_t, Target> targets;
            // (P3 S3) The D3D12 device's targets, by gfx::TargetId (entry 0 stays empty: "none"). A deque, so a
            // reference to one stays good while others are made.
            struct DeviceTarget
            {
                ID3D12Resource *colour = nullptr, *depth = nullptr;
                UINT rtv = 0, srv = 0;
                ID3D12Resource *srvOf = nullptr; // the resource the SRV slot was written for
                UINT depthSrv = 0;               // the depth's shader view (the depth fog)
                ID3D12Resource *destCopy = nullptr; // a copy of the colour for TEST's DATE (its alpha), and its view
                UINT destSrv = 0;
                D3D12_RESOURCE_STATES destState = D3D12_RESOURCE_STATE_COPY_DEST;
                D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
                D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_RENDER_TARGET;
                DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM; // (TargetDesc::colour: the GS targets' BGRA8, the effects' own)
                bool withDepth = true;                           // (TargetDesc::depth)
                UINT w = 0, h = 0;
                bool picture = false; // (the picture's own surfaces, g.colour / g.depth through view place 0)
            };
            std::deque<DeviceTarget> devTargets = std::deque<DeviceTarget>(1);
            std::vector<gfx::TargetId> freeTargetIds;
            // (P3 S3) The D3D12 device's meshes, by id (entry 0 empty): a resident mesh's buffer (vertices then indices,
            // an upload heap) and, TF_NATIVE_MESHVRAM, its video memory copy.
            struct DeviceMesh
            {
                ID3D12Resource *buffer = nullptr, *vram = nullptr;
                D3D12_VERTEX_BUFFER_VIEW vbv{}, vbvV{};
                D3D12_INDEX_BUFFER_VIEW ibv{}, ibvV{};
            };
            std::deque<DeviceMesh> devMeshes = std::deque<DeviceMesh>(1);
            std::vector<uint32_t> freeMeshIds;
            // (P3 S3) The D3D12 device's textures, by gfx::TextureId (entry 0: the white one, slot 0): made on this frame
            // (uploadTexture, its levels' texels taken once the last level is given) or by the uploader (uploadTextureLater:
            // made, copied on the copy queue, its view written at a frame's start). `generation` tells a finished upload
            // from an older one of a reused id.
            struct DeviceTexture
            {
                ID3D12Resource *resource = nullptr;
                UINT slot = 0;
                uint32_t width = 0, height = 0, levels = 0;
                gfx::Format format = gfx::Format::RGBA8;
                std::vector<const void *> given; // (uploadTexture's levels until the last one)
                uint64_t bytes = 0;              // (its video memory)
                uint32_t generation = 0;
                bool ready = false;
            };
            std::deque<DeviceTexture> devTextures = std::deque<DeviceTexture>(1);
            std::vector<gfx::TextureId> freeTextureIds;
            // (P3 S3) The rings' places this frame: the constants' byte offset and the instance entries' index in their
            // buffers (both from the slot's start); the vertices' is dynamicUsed.
            UINT64 ringConstantsAt = 0;
            UINT ringInstanceAt = 0;
            UINT rtvSize = 0, dsvSize = 0, nextTarget = 1;
            std::vector<UINT> freeRtvs; // (render-target view places given back: a target that became the picture's)
            UINT scaleX = 2, scaleY = 2; // the targets' pixels a GS pixel
            UINT scaleYField = 2;        // (N10) a field buffer's rows a GS row: its field fills the picture's height
            bool failed = false;
            bool live = false; // (TF_NATIVE_LIVE, read in prepare(): the settings may set it after start-up)
            // Two frames in flight: while the GPU draws one, the game makes the next. Each frame its
            // own allocator, halves of the rings, readback, and what it lets go of (resources and
            // descriptor slots, given back once its fence is passed - an earlier frame may still
            // be reading them).
            static constexpr UINT kFrames = 2;
            // (TODO 94) The draws' constants in video memory: the frame's part of the upload ring copied into
            // constantsV by a list of its own run just before the frame's (preList); null: read from the upload heap.
            ID3D12Resource *constantsV = nullptr;
            bool constantsVReadable = false; // (its state: VERTEX_AND_CONSTANT_BUFFER, else COMMON / COPY_DEST)
            ID3D12CommandAllocator *preAllocators[kFrames] = {};
            ID3D12GraphicsCommandList *preList = nullptr;
            ID3D12CommandAllocator *allocators[kFrames] = {};
            // (d3d553) The frame's list a slot (`list` points at the slot's): a frame's extra pictures may still be waiting
            // for their turn (replay::) while the next frame is recorded into the other.
            ID3D12GraphicsCommandList *frameLists[kFrames] = {};
            // (N9 stage 3) A frame's pictures from one recording: the frame's list (`list`) run once for each, with a list
            // before it (the picture's constants into pictureCB, its first timestamp) and one after (its last timestamp,
            // its copy into the shared texture shown) - and, before an extra picture, one setting every resource back to
            // the state the frame's list began with (restoreList: its barriers assume them). Their allocator a slot.
            ID3D12CommandAllocator *picAllocators[kFrames] = {};
            // (a list object each for every picture of a slot's frame: one object recorded again before the GPU ran its
            // last recording drew the RX 5700 XT's first picture with the second's commands - its timestamps showed it)
            ID3D12GraphicsCommandList *proLists[kFrames][24] = {}, *tailLists[kFrames][24] = {}, *restoreLists[kFrames] = {};
            ID3D12Resource *pictureCB = nullptr; // (video memory: 256 bytes a slot, the shader's b1)
            ID3D12Resource *predicate = nullptr; // (video memory: 8 bytes a slot - 1 in an extra picture: its skipped draws)
            D3D12_RESOURCE_STATES predicateState = D3D12_RESOURCE_STATE_COPY_DEST;
            D3D12_RESOURCE_STATES pictureCBState = D3D12_RESOURCE_STATE_COPY_DEST;
            ID3D12Resource *pictureUp = nullptr; // (upload: kMaxPictures entries a slot)
            uint8_t *pictureUpMapped = nullptr;
            ID3D12QueryHeap *picQueries = nullptr; // (two timestamps a picture, kMaxPictures a slot)
            ID3D12Resource *picTimes = nullptr;
            UINT slotPictures[kFrames] = {}; // (the pictures the slot's frame drew: their times read when it is done)
            // (the frame's list being recorded: each resource's state when the list first moved it, and last)
            bool journalOn = false;
            std::unordered_map<ID3D12Resource *, std::pair<D3D12_RESOURCE_STATES, D3D12_RESOURCE_STATES>> journal;
            std::vector<ID3D12Resource *> journalOrder;
            UINT64 slotFence[kFrames] = {};
            std::vector<ID3D12Resource *> slotReleases[kFrames];
            std::vector<UINT> slotFreeSlots[kFrames];
            ID3D12Resource *slotReadback[kFrames] = {};
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT slotFootprint[kFrames] = {};
            UINT slotReadbackW[kFrames] = {}, slotReadbackH[kFrames] = {};
            bool slotPublish[kFrames] = {};
            bool slotHas3D[kFrames] = {};
            uint64_t slotTick[kFrames] = {}; // (N9) the frame's VBlank and its moment (Frame::tick, dueNs), for the hand-over
            int64_t slotDueNs[kFrames] = {};
            // The live picture shared with the window's GL (nr_glshare.h): four textures in turn, so
            // one is shown, one waits, one is being drawn and one is spare; the picture is copied
            // into one on the GPU and handed over when its frame's fence has passed.
            // (N9: up to 24, made as they are first needed - the paced hand-over keeps the pictures waiting for their
            // moment too, beside the one shown and the newest; with pictures between frames about a frame and a half of
            // them wait: 12 at 240 a second with the menus' 30 frames.)
            static constexpr int kShared = d3d::kSharedPictures;
            struct Shared
            {
                ID3D12Resource *resource = nullptr;
                HANDLE handle = nullptr;
                UINT w = 0, h = 0;
                UINT64 size = 0;
                uint64_t generation = 0;
            };
            Shared shared[kShared];
            int slotShared[kFrames] = {-1, -1};
            bool slotPaced[kFrames] = {}; // (N9) the frame's picture handed over by the pacer (pace::), not by finishSlot
            int lastShared = -1; // the newest handed over
            uint64_t frameIndex = 0;
            UINT slot = 0;
            // The anti-aliasing pass (kAaShader): its pipeline, the picture's copy it reads (and that
            // copy's shader view), its GPU time per frame (TF_NATIVE_PROFILE: timestamps a slot).
            ID3D12PipelineState *aaPipeline = nullptr;
            ID3D12Resource *aaSource = nullptr;
            UINT aaSlot = 0, aaW = 0, aaH = 0;
            D3D12_RESOURCE_STATES aaState = D3D12_RESOURCE_STATE_COPY_DEST;
            ID3D12QueryHeap *aaQueries = nullptr;
            ID3D12Resource *aaTimes = nullptr; // (readback: two timestamps a slot)
            UINT64 timestampHz = 0;
            bool aaTimed[kFrames] = {};
            bool frameTimed[kFrames] = {}; // (TF_NATIVE_PROFILE: the whole frame's two timestamps, after the anti-aliasing's)
            // SMAA 1x (kSmaaEntries, TF_NATIVE_AA=2): its three pipelines, its edges (R8G8) and blend-weights (RGBA8)
            // targets at the picture's size - their render-target views and shader views kept between frames, in the
            // shader's state between passes - and its two lookup textures (uploaded at its first frame). Render-target
            // place 0 and slot 0 are never handed out, so 0 = none.
            ID3D12PipelineState *smaaEdgePipe = nullptr, *smaaWeightPipe = nullptr, *smaaBlendPipe = nullptr;
            ID3D12Resource *smaaEdges = nullptr, *smaaWeights = nullptr, *smaaArea = nullptr, *smaaSearch = nullptr;
            UINT smaaEdgesRtv = 0, smaaWeightsRtv = 0, smaaEdgesSlot = 0, smaaWeightsSlot = 0, smaaAreaSlot = 0, smaaSearchSlot = 0;
            UINT smaaW = 0, smaaH = 0;
            bool smaaLookupUploaded = false;
            // The sun's occlusion probe (kProbeShader, roadmap N3a): its pipeline, its 64 x 64 target and render-target view,
            // a readback of 16 KB a slot, and whether the slot's frame took one (and its tick).
            ID3D12PipelineState *cloudPipeline = nullptr; // (TODO 27: the warp tunnel's sparks - kCloudShader, drawCloud)
            ID3D12PipelineState *probePipeline = nullptr;
            ID3D12Resource *probeTarget = nullptr, *probeReadback = nullptr;
            UINT probeRtv = 0;
            bool probeTaken[kFrames] = {};
            uint64_t probeTick[kFrames] = {};
            // (TF_NATIVE_PASSPROFILE: a timestamp after every draw, each interval put to the draw's kind.)
            static constexpr UINT kPassMarks = 8192u;
            ID3D12QueryHeap *passQueries = nullptr;
            ID3D12Resource *passTimes = nullptr;
            // (TF_NATIVE_DRAWCOST with TF_NATIVE_DRAWSTATS=1: each mark's pipeline statistics too - pixels shaded, triangles
            // rasterised; the queries slow those windows down, so their times are not the frame's.)
            ID3D12QueryHeap *statQueries = nullptr;
            ID3D12Resource *statData = nullptr;
            bool statOpen = false;
            bool statsTaken[kFrames] = {}; // (the slot's frame resolved its statistics)
            std::vector<uint8_t> passKinds[kFrames];
            struct PassNote
            {
                uint32_t draw = 0, indices = 0, texW = 0, texH = 0, mode = 0;
                uint64_t alpha = 0, tex0 = 0;
                // (TF_NATIVE_DRAWCOST, TODO 94: the draw's state - zte | func << 1 | zwrite << 4 | blend << 5 | mask << 6 |
                // main << 10 | immediate << 11 | grass << 12 | sprites << 13 | fog << 14 | source << 15 - its target, its
                // screen area in GS pixels (the triangles' sum, before clipping), its mesh.)
                uint32_t bits = 0, verts = 0, barriers = 0, copies = 0;
                float area = 0.0f;
                const void *target = nullptr;
                uint64_t mesh = 0;
            };
            std::vector<PassNote> passNotes[kFrames]; // (beside each mark: the draw it ended)
        } g;

        // The latest picture, handed to the window's thread (nr_present.cpp).
        struct Published
        {
            std::mutex mutex;
            std::vector<uint32_t> pixels;
            uint32_t width = 0, height = 0;
            uint64_t serial = 0;
            bool has3D = false;
            d3d::SharedPicture shared; // (when the newest picture is a shared texture: index >= 0)
            std::condition_variable cv; // (N9) a new picture handed over (the window's pacer waits on it)
            int64_t lastNs = 0;          // when the newest was
            uint32_t pending = 0u;       // (N9) shared textures given to the pacer, not handed over yet (pickShared leaves them)
        } g_published;
        std::atomic<int> g_shareOn{0};    // the window: 0 not asked yet, 1 reads shared textures, -1 cannot
        std::atomic<int> g_sharedHeld{-1}; // the shared texture the window shows
        // (TODO 98) ... and the two it showed before it: the window's GL may still read them after it has moved on (its
        // draws queued for a frame or two), so they are not drawn into until two more pictures have been shown.
        std::atomic<int> g_sharedHeldBefore[2] = {-1, -1};
        void holdShown(int index)
        {
            const int held = g_sharedHeld.load();
            if (index == held)
                return;
            g_sharedHeldBefore[1] = g_sharedHeldBefore[0].load();
            g_sharedHeldBefore[0] = held;
            g_sharedHeld = index;
        }

        // (TODO 164 stage 3) The shaders and pipelines kept on disk across runs, beside the player's settings (the folder
        // settings.ini is in - PS2X_SETTINGS_FILE's, else the game's working folder - under "shader cache";
        // TF_SHADER_CACHE=<folder> another, =0 none). Each compiled shader by the hash of its source, entry point, target
        // and defines (a changed shader is a new name - nothing stale is taken); the pipelines in a D3D12 pipeline library
        // (pipelines.bin), which the driver refuses when it or the graphics card changed - it is then made anew.
        namespace cache
        {
            std::filesystem::path g_dir;
            bool g_dirKnown = false;
            std::mutex g_dirMutex;
            std::atomic<uint32_t> g_shaderHits{0}, g_shaderMade{0};

            const std::filesystem::path &dir()
            {
                std::lock_guard<std::mutex> lock(g_dirMutex);
                if (!g_dirKnown)
                {
                    g_dirKnown = true;
                    const char *e = std::getenv("TF_SHADER_CACHE");
                    std::error_code ec;
                    if (e && e[0] == '0' && !e[1])
                        g_dir.clear();
                    else if (e && *e)
                        g_dir = std::filesystem::path(e);
                    else
                    {
                        const char *settings = std::getenv("PS2X_SETTINGS_FILE");
                        std::filesystem::path base =
                            settings && *settings && std::string(settings) != "0" ? std::filesystem::absolute(settings, ec).parent_path()
                                                                                   : std::filesystem::current_path(ec);
                        g_dir = base / "shader cache";
                    }
                    if (!g_dir.empty())
                    {
                        std::filesystem::create_directories(g_dir, ec);
                        if (ec)
                        {
                            std::fprintf(stderr, "[native render] shader cache: %s cannot be made - none kept\n", g_dir.string().c_str());
                            g_dir.clear();
                        }
                    }
                }
                return g_dir;
            }

            uint64_t fnv(uint64_t h, const void *p, size_t n)
            {
                const auto *b = static_cast<const uint8_t *>(p);
                for (size_t i = 0; i < n; ++i)
                    h = (h ^ b[i]) * 1099511628211ull;
                return h;
            }

            bool readFile(const std::filesystem::path &path, std::vector<uint8_t> &out)
            {
                FILE *f = _wfopen(path.wstring().c_str(), L"rb");
                if (!f)
                    return false;
                std::fseek(f, 0, SEEK_END);
                const long size = std::ftell(f);
                std::fseek(f, 0, SEEK_SET);
                bool ok = size > 0;
                if (ok)
                {
                    out.resize(static_cast<size_t>(size));
                    ok = std::fread(out.data(), 1u, out.size(), f) == out.size();
                }
                std::fclose(f);
                return ok;
            }

            // (written beside and renamed over: a run ending part-way leaves the old file, never half of one)
            void writeFile(const std::filesystem::path &path, const void *data, size_t size)
            {
                std::filesystem::path tmp = path;
                tmp += L".tmp" + std::to_wstring(GetCurrentThreadId());
                FILE *f = _wfopen(tmp.wstring().c_str(), L"wb");
                if (!f)
                    return;
                const bool ok = std::fwrite(data, 1u, size, f) == size;
                std::fclose(f);
                std::error_code ec;
                if (ok)
                    std::filesystem::rename(tmp, path, ec);
                if (!ok || ec)
                    std::filesystem::remove(tmp, ec);
            }

            // A compiled shader from the cache, or null.
            ID3DBlob *loadShader(uint64_t key)
            {
                if (dir().empty())
                    return nullptr;
                char name[40];
                std::snprintf(name, sizeof(name), "%016llx.dxbc", static_cast<unsigned long long>(key));
                std::vector<uint8_t> bytes;
                if (!readFile(dir() / name, bytes) || bytes.size() < 4u || std::memcmp(bytes.data(), "DXBC", 4) != 0)
                    return nullptr;
                ID3DBlob *blob = nullptr;
                if (FAILED(D3DCreateBlob(bytes.size(), &blob)))
                    return nullptr;
                std::memcpy(blob->GetBufferPointer(), bytes.data(), bytes.size());
                ++g_shaderHits;
                return blob;
            }

            void storeShader(uint64_t key, ID3DBlob *blob)
            {
                if (dir().empty() || !blob)
                    return;
                char name[40];
                std::snprintf(name, sizeof(name), "%016llx.dxbc", static_cast<unsigned long long>(key));
                writeFile(dir() / name, blob->GetBufferPointer(), blob->GetBufferSize());
                ++g_shaderMade;
            }
        }

        ID3DBlob *compile(const char *entry, const char *target, const char *source = kShader, size_t size = sizeof(kShader) - 1,
                          const D3D_SHADER_MACRO *defines = nullptr, UINT extraFlags = 0)
        {
            // (TODO 164 stage 3: from the shader cache when it has this source, entry point, target, defines and flags)
            const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | extraFlags;
            uint64_t key = cache::fnv(1469598103934665603ull, "nr shader cache 1", 17);
            const int compiler = D3D_COMPILER_VERSION;
            key = cache::fnv(key, &compiler, sizeof compiler);
            key = cache::fnv(key, &flags, sizeof flags);
            key = cache::fnv(key, source, size);
            key = cache::fnv(key, entry, std::strlen(entry) + 1u);
            key = cache::fnv(key, target, std::strlen(target) + 1u);
            for (const D3D_SHADER_MACRO *d = defines; d && d->Name; ++d)
            {
                key = cache::fnv(key, d->Name, std::strlen(d->Name) + 1u);
                if (d->Definition)
                    key = cache::fnv(key, d->Definition, std::strlen(d->Definition) + 1u);
            }
            if (ID3DBlob *hit = cache::loadShader(key))
                return hit;
            ID3DBlob *code = nullptr, *errors = nullptr;
            const HRESULT hr = D3DCompile(source, size, "nr_shader", defines, nullptr, entry, target, flags, 0, &code, &errors);
            if (FAILED(hr))
                std::fprintf(stderr, "[native render] shader %s: %s\n", entry,
                             errors ? static_cast<const char *>(errors->GetBufferPointer()) : "failed");
            release(errors);
            if (code)
                cache::storeShader(key, code);
            return code;
        }

        // The fence waited for in steps of a second, the device's state looked at between them: a
        // removed device (a GPU hang) ends the wait - the frame's end tells it and stops the renderer -
        // rather than the render thread waiting for ever (and the game's thread behind it).
        void waitFence(UINT64 value)
        {
            if (g.fence->GetCompletedValue() >= value)
                return;
            g.fence->SetEventOnCompletion(value, g.fenceEvent);
            const auto begun = std::chrono::steady_clock::now();
            bool told = false;
            while (WaitForSingleObject(g.fenceEvent, 1000) == WAIT_TIMEOUT)
            {
                if (FAILED(g.device->GetDeviceRemovedReason()))
                    return;
                const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - begun).count();
                if (!told && s > 3.0)
                {
                    told = true;
                    std::fprintf(stderr, "[native render] warning: the GPU has not finished a frame in %.0f s (fence %llu, done %llu)\n", s,
                                 static_cast<unsigned long long>(value), static_cast<unsigned long long>(g.fence->GetCompletedValue()));
                }
            }
        }

        // (d3d553) A frame's extra pictures, submitted by a thread of their own. D3D12 forbids submitting a command list
        // again while an earlier submission of it may still run (the driver may patch a list as it is submitted; the
        // debug layer's error 553, COMMAND_LIST_SYNC): an extra picture runs the frame's list (and the restore list)
        // again, so it may go only once the picture before it has finished. This thread waits for that and submits it,
        // while the render thread goes on recording the next frame (into the other slot's list) - the GPU kept busy as
        // before. Every other use of the queue first waits for its jobs to be done (drain), so the queue's order stays
        // the render thread's. The fence values are given out by the render thread when it hands a picture over.
        namespace replay
        {
            struct Job
            {
                ID3D12CommandList *lists[5] = {};
                UINT count = 0;
                UINT64 after = 0;  // (submitted once the fence has passed this)
                UINT64 signal = 0; // (then the fence set to this)
                std::function<void()> before; // (N9: the picture's camera turn read just before it is submitted - view_turn.h)
            };
            std::mutex g_mutex;
            std::condition_variable g_cv, g_idle;
            std::deque<Job> g_jobs;
            bool g_busy = false, g_started = false;
            // (N9: the time the jobs waited for their pictures' moments - the camera's turn read then, view_turn.h - so far:
            // a drain that waits on it is waiting for the turn's moment, not for the GPU)
            std::atomic<double> g_turnWaitMs{0.0};
            HANDLE g_event = nullptr;

            void run()
            {
                for (;;)
                {
                    Job job;
                    {
                        std::unique_lock<std::mutex> lock(g_mutex);
                        g_cv.wait(lock, [] { return !g_jobs.empty(); });
                        job = g_jobs.front();
                        g_jobs.pop_front();
                        g_busy = true;
                    }
                    if (g.fence->GetCompletedValue() < job.after)
                    {
                        g.fence->SetEventOnCompletion(job.after, g_event);
                        // (a removed device ends the wait: the render thread tells it at the frame's end)
                        while (WaitForSingleObject(g_event, 1000) == WAIT_TIMEOUT && SUCCEEDED(g.device->GetDeviceRemovedReason()))
                        {
                        }
                    }
                    if (job.before)
                        job.before();
                    g.queue->ExecuteCommandLists(job.count, job.lists);
                    g.queue->Signal(g.fence, job.signal);
                    {
                        std::lock_guard<std::mutex> lock(g_mutex);
                        g_busy = false;
                        if (g_jobs.empty())
                            g_idle.notify_all();
                    }
                }
            }

            void push(const Job &job)
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (!g_started)
                {
                    g_started = true;
                    g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                    std::thread(run).detach();
                }
                g_jobs.push_back(job);
                g_cv.notify_one();
            }

            // Every job submitted (before the render thread uses the queue itself); the time it waited (ms) - of it the part a
            // job spent waiting for its picture's moment in `turnMs` (N9's turn; not the GPU's).
            double drain(double *turnMs = nullptr)
            {
                if (turnMs)
                    *turnMs = 0.0;
                std::unique_lock<std::mutex> lock(g_mutex);
                if (g_jobs.empty() && !g_busy)
                    return 0.0;
                const double turn0 = g_turnWaitMs.load(std::memory_order_relaxed);
                const auto t0 = std::chrono::steady_clock::now();
                g_idle.wait(lock, [] { return g_jobs.empty() && !g_busy; });
                const double waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                const double turn = std::clamp(g_turnWaitMs.load(std::memory_order_relaxed) - turn0, 0.0, waited);
                if (turnMs)
                    *turnMs = turn;
                return waited - turn;
            }
        }

        void waitForGpu()
        {
            replay::drain(); // (d3d553: the queue's order - the extra pictures handed over first)
            const UINT64 value = ++g.fenceValue;
            g.queue->Signal(g.fence, value);
            waitFence(value);
        }

        D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type)
        {
            D3D12_HEAP_PROPERTIES h{};
            h.Type = type;
            return h;
        }

        D3D12_RESOURCE_DESC bufferDesc(UINT64 size)
        {
            D3D12_RESOURCE_DESC d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            d.Width = size;
            d.Height = 1;
            d.DepthOrArraySize = 1;
            d.MipLevels = 1;
            d.SampleDesc.Count = 1;
            d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            return d;
        }

        // (TF_NATIVE_PROFILE's per-frame counts of what may serialise the GPU's work: barriers, pipeline changes, the
        // switches between pipelines with and without the clipping shader, target changes, draws - TODO 94.)
        uint64_t g_cntBarriers = 0, g_cntPso = 0, g_cntGsSwitch = 0, g_cntTargets = 0, g_cntDraws = 0, g_cntIndices = 0, g_cntGsIndices = 0;
        uint64_t g_cntCopies = 0, g_markBarriers = 0, g_markCopies = 0;
        uint64_t g_cntScissors = 0, g_cntFactors = 0; // (scissors and blend factors set) // (TF_NATIVE_DRAWCOST: target copies; the counts at the last mark)
        void transition(ID3D12Resource *resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
        {
            ++g_cntBarriers;
            if (g.journalOn)
            {
                // (N9 stage 3: the frame's list drawn more than once - its resources set back between)
                const auto ins = g.journal.emplace(resource, std::make_pair(from, to));
                if (ins.second)
                    g.journalOrder.push_back(resource);
                else
                    ins.first->second.second = to;
            }
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = resource;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = from;
            b.Transition.StateAfter = to;
            g.list->ResourceBarrier(1, &b);
        }

        void waitIdle(); // (below: every frame in flight done)

        // The draws' constants' base address this frame: video memory (constantsV) or the upload ring (TODO 94;
        // TF_NATIVE_CBVRAM=alt: by turns, a profile window each). Set by render() at the frame's start.
        bool g_cbVram = false;
        D3D12_GPU_VIRTUAL_ADDRESS cbBase() { return (g_cbVram ? g.constantsV : g.constants)->GetGPUVirtualAddress(); }

        uint64_t g_targetsMade = 0, g_picturesMade = 0;

        // The anti-aliasing pass (TF_NATIVE_AA): -1 not read yet, 0 off, 1 on (the settings panel
        // changes it live). Its cost (TF_NATIVE_PROFILE): the render thread's recording, the GPU's.
        std::atomic<int> g_aa{-1};
        std::atomic<int> g_ssaa{-1}; // (supersampling: -1 not read yet, then 0/1 - setSupersampling)
        double g_aaCpu = 0.0, g_aaGpu = 0.0;
        double g_frameGpu = 0.0, g_slotWaitMs = 0.0; // (TF_NATIVE_PROFILE: the whole frame on the GPU; the render thread waiting for a frame slot)
        std::atomic<double> g_gpuPictureMs{0.0};      // (N9 stage 2: a picture's time on the GPU, smoothed - the pictures' cap)
        std::atomic<double> g_gpuExtraMs{0.0};        // (N9 stage 3: an extra picture's, drawn again from the frame's recording)
        std::atomic<double> g_cpuPictureMs{0.0};      // (N9 stage 3: the render thread's time a picture once the frame is recorded)
        double g_replayWaitMs = 0.0;                  // (d3d553: the last render call's waits for an extra picture's turn; render thread)
        std::atomic<double> g_slotWaitAll{0.0};       // (N9 stage 3: the render thread's waits for a frame slot, all of them - the GPU behind)
        uint32_t g_frameGpuFrames = 0;
        uint32_t g_profWindow = 0; // (TF_NATIVE_PROFILE's windows so far: TF_NATIVE_CLIPFAST=alt turns on it)
        uint64_t g_clipRuns[3] = {}; // (TF_NATIVE_PROFILE: a clipping draw's runs drawn - inside, crossing - and left out)
        // (TF_NATIVE_PASSPROFILE: GPU ms by kind of draw, summed over the frames timed.)
        constexpr int kPassKinds = 10;
        const char *const kPassKindNames[kPassKinds] = {"3D opaque", "3D blended", "grass/drift", "2D", "small targets (effects, shadows)", "depth fog",
                                                        "end of frame (AA, copies)", "setup (clears)", "3D upscaled texture", "3D through the clipping shader"};
        double g_passGpu[kPassKinds] = {};
        uint32_t g_passFrames = 0;
        std::vector<std::pair<double, std::string>> g_passTop; // (the last frame read: its costliest draws)
        // (TF_NATIVE_DRAWCOST=<window>[,<window>...], TODO 94: each mark's GPU ms summed over the window's frames with the
        // same count of marks, printed with the draw's state at the window's end - which draws cost, and next to what.)
        std::vector<double> g_drawCostSum;
        std::vector<uint32_t> g_drawCostN;
        std::vector<double> g_drawPixels, g_drawPrims; // (pipeline statistics: PS invocations, rasterised triangles, summed)
        uint32_t g_drawStatFrames = 0;
        std::vector<std::pair<uint8_t, State::PassNote>> g_drawCostNote;
        // (TODO 94: the speed-ups that keep the picture, each TF_NATIVE_<name>=0 off, =alt by turns (a profile window
        // each), unset on; TF_NATIVE_SHOTAB's forcing over them.)
        std::atomic<int> g_speedupsForced{-1};
        bool speedupOn(const char *env)
        {
            const int forced = g_speedupsForced.load(std::memory_order_relaxed);
            if (forced >= 0)
                return forced != 0;
            return !(env && (*env == '0' || (*env == 'a' && (g_profWindow & 1u))));
        }

        bool drawCostWindow(uint32_t w)
        {
            static const char *env = std::getenv("TF_NATIVE_DRAWCOST");
            if (!env || !*env)
                return false;
            for (const char *k = env; *k;)
            {
                char *end = nullptr;
                const unsigned long v = std::strtoul(k, &end, 10);
                if (end == k)
                    break;
                if (v == w)
                    return true;
                k = *end == ',' ? end + 1 : end;
            }
            return false;
        }
        uint32_t g_aaFrames = 0, g_aaGpuFrames = 0, g_smaaFrames = 0;
        // (TF_NATIVE_AA: 0 off, 2 or "smaa" SMAA 1x, anything else - unset too - FXAA.)
        int aaMode()
        {
            int v = g_aa.load(std::memory_order_relaxed);
            if (v < 0)
            {
                const char *e = std::getenv("TF_NATIVE_AA");
                int expected = -1;
                g_aa.compare_exchange_strong(expected, e && *e == '0' ? 0 : e && (*e == '2' || *e == 's' || *e == 'S') ? 2 : 1);
                v = g_aa.load(std::memory_order_relaxed);
            }
            return v;
        }
        bool aaOn()
        {
            return aaMode() != 0;
        }

        bool makeTargets(UINT width, UINT height)
        {
            if (width == g.width && height == g.height && g.colour)
                return true;
            ++g_picturesMade;
            if (g.colour)
                waitIdle(); // (the picture in use by a frame in flight)
            if (std::getenv("TF_NATIVE_TARGETLOG"))
                std::fprintf(stderr, "[native render] picture made %ux%u (was %ux%u)\n", width, height, g.width, g.height);
            // The old picture let go of with the frame (not here), and the main target's entry told it
            // is gone: released at once, the new picture could come back at the old one's address, the
            // main target would then keep its entry - and its shader view of the released texture
            // (srvFor compares the resource's address) - and a draw reading the picture made the GPU
            // read freed memory.
            g.uploads.push_back(g.colour);
            g.uploads.push_back(g.depth);
            g.uploads.push_back(g.readback);
            g.colour = g.depth = g.readback = nullptr;
            for (auto &t : g.targets)
                if (t.second.main && t.second.id)
                {
                    // (Told: the state the old code turned into a view of freed memory - a main target
                    // kept across the remake, with a shader view of the old picture written.)
                    const State::DeviceTarget &d = g.devTargets[t.second.id];
                    if (d.srv && d.srvOf)
                        std::fprintf(stderr, "[native render] picture made %ux%u while target %04x held a view of the old one (slot %u)\n",
                                     width, height, t.first, d.srv);
                    gfx::device().releaseTarget(t.second.id); // (render() makes the entry again)
                    t.second.id = 0;
                }
            D3D12_RESOURCE_DESC td{};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = width;
            td.Height = height;
            td.DepthOrArraySize = 1;
            td.MipLevels = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // (RGBA: as the window's GL texture takes it)
            td.SampleDesc.Count = 1;
            td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE clear{};
            clear.Format = td.Format;
            clear.Color[0] = 0.05f;
            clear.Color[1] = 0.05f;
            clear.Color[2] = 0.08f;
            clear.Color[3] = 1.0f;
            const D3D12_HEAP_PROPERTIES gpu = heap(D3D12_HEAP_TYPE_DEFAULT);
            if (FAILED(g.device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                                         IID_PPV_ARGS(&g.colour))))
                return false;
            td.Format = DXGI_FORMAT_D32_FLOAT;
            td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            D3D12_CLEAR_VALUE depthClear{};
            depthClear.Format = td.Format;
            depthClear.DepthStencil.Depth = 0.0f;
            if (FAILED(g.device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                         &depthClear, IID_PPV_ARGS(&g.depth))))
                return false;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.Flags = D3D12_RESOURCE_FLAG_NONE;
            UINT64 total = 0;
            g.device->GetCopyableFootprints(&td, 0, 1, 0, &g.footprint, nullptr, nullptr, &total);
            const D3D12_HEAP_PROPERTIES back = heap(D3D12_HEAP_TYPE_READBACK);
            const D3D12_RESOURCE_DESC rb = bufferDesc(total);
            if (FAILED(g.device->CreateCommittedResource(&back, D3D12_HEAP_FLAG_NONE, &rb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&g.readback))))
                return false;
            g.device->CreateRenderTargetView(g.colour, nullptr, g.rtvHeap->GetCPUDescriptorHandleForHeapStart());
            g.device->CreateDepthStencilView(g.depth, nullptr, g.dsvHeap->GetCPUDescriptorHandleForHeapStart());
            g.width = width;
            g.height = height;
            return true;
        }

        // A mesh's vertices and indices in one buffer (an upload heap for now: written once, read by
        // the GPU in place; a default-heap copy comes with the live picture).
        constexpr UINT64 kDynamicRing = 64ull << 20;
        constexpr UINT kVertexBStride = 20u; // (N9 stage 3: a vertex's frame-before place, colour and size - POSB, COLORB, PSIZEB)

        uint64_t g_clipCulled[2] = {};
        ID3D12Resource *g_pcBack = nullptr; // (TODO 99 check: the picture constants read back on a saved frame) // (TODO 99: clipping triangles left out on the CPU; whole passes left out)
        uint64_t g_ringFull = 0; // (TODO 99 check: draws or meshes left out because a frame's ring was full)
        uint64_t g_meshesMade = 0, g_meshVertices = 0, g_ringMeshes = 0, g_pictureReads = 0; // (TF_NATIVE_PROFILE: buffers made, their vertices; meshes in the frame's ring)
        // ---- grassgpu: the grass's blades grown on the GPU -----------------------------------------------------------
        // nr_grass.cpp's grassBlades as a compute pass, recorded when the frame's list begins into a list of its own that runs
        // once, before the frame's first picture (the frame's list itself runs again for each picture between frames: the pass
        // in it was run that many times - 2.5 ms a picture on the RX 5700 XT). Every ground of the frame given to the GPU (its
        // triangles, each with the area the CPU works out - in the CPU's own sqrt - and its ground's block); 44 threads a
        // triangle (the shader's kSegs: its own packets, one a thread, and its quarters' stretches) count their blades, the
        // counts are summed into each one's place, then written there as the CPU's quads (4 vertices a blade, nr::Vertex's
        // layout); each ground's draw an indirect one of its stretch (6 indices a blade from a fixed index list, its first
        // vertex from the GPU's arguments). The render thread makes no blades and uploads none. The divisions in double where
        // the GPU has it (the CPU's float division rounded the same), every sum and product unfused: the CPU's blades to the
        // bit (TF_GRASS_GPU_CHECK compares). Its GPU time is in the pass's 10-second line.
        namespace grassgpu
        {
            const char kCs[] = R"HLSL(
// (grassgpu) nr_grass.cpp's grassBlades on the GPU: the same packets of the same blades, in the same order. A ground
// triangle's tufts are a tree - the triangle, its quarters while a corner has more than eight packets to give, down to
// four levels - drawn in its depth-first order; here 44 threads share each triangle (kSegs): 24 for its own packets (three
// corners, eight packets at most each - nearly all the blades are there), then its four quarters, and the sixteen
// quarters' quarters with everything below them, each thread a stretch of that order.
// Every thread first only counts its stretch's blades (CsCount), then, once the counts are summed into each stretch's
// place (CsScan, one group), writes them there (CsWrite). A node's corners are made from the triangle's by the same
// midpoints the CPU takes, step by step - no stack. Every sum and product as the CPU makes it (precise: none fused) and
// every division in double, rounded to float (EXACT_DIV) - so the blades are the CPU's to the bit.
struct Tri
{
    float3 p0; uint c0;
    float3 p1; uint c1;
    float3 p2; uint c2;
    float area2;
    uint ground;
};
struct Ground
{
    float4 m0, m1, m2, m3;  // q3-q6: field A's matrix
    float4 b0, b1, b2, q23; // q0, q1: the frustum test; q2: the blade's offset (x) and height (y); q23: the density
    float4 pts[32];         // q24-q55: the points' table, two points a qword
    float thin;             // q23.w over the grass's reach
    uint firstTri, triCount, pad;
};
struct Vtx
{
    float x, y, z;
    uint colour;
    float u, v, nx, ny, nz, size;
};
StructuredBuffer<Tri> tris : register(t0);
StructuredBuffer<Ground> grounds : register(t1);
RWStructuredBuffer<uint> counts : register(u0);
RWStructuredBuffer<Vtx> blades : register(u1);
RWStructuredBuffer<uint> args : register(u2);
cbuffer Totals : register(b0)
{
    uint triCount;
    uint groundCount;
    uint capacity;
    uint unused;
};

static const uint kRootPackets = 24u; // (the triangle's own packets: three corners, eight at most each)
static const uint kSegs = 44u;        // (a triangle's threads: its own 24 packets, then 20 stretches below it)

#ifdef EXACT_DIV
float dv(float a, float b)
{
    return (float)((double)a / (double)b);
}
#else
float dv(float a, float b)
{
    return a / b;
}
#endif

struct Corner
{
    float4 p;
    float3 rgb;
    float density;
};

struct Node
{
    Corner t0, t1, t2;
    float area2, sub;
};

struct GSmall
{
    float4 m0, m1, m2, m3, b0, b1, b2, q23;
    float thin;
};

uint outsideOf(GSmall g, uint gi, float4 p)
{
    uint bits = 0u;
    precise float w = abs(p.w);
    precise float t0 = g.b0.x * p.x + g.b1.x * p.w;
    precise float t1 = g.b0.y * p.y + g.b1.y * p.w;
    precise float t2 = g.b0.z * p.z + g.b1.z * p.w;
    if (t0 > w) bits |= 1u;
    if (t0 < -w) bits |= 2u;
    if (t1 > w) bits |= 4u;
    if (t1 < -w) bits |= 8u;
    if (t2 > w) bits |= 16u;
    if (t2 < -w) bits |= 32u;
    return bits;
}

Corner lerp3(Corner c, Corner a, Corner b, float u, float v)
{
    Corner r;
    precise float4 p = c.p + u * (a.p - c.p) + v * (b.p - c.p);
    precise float3 rgb = c.rgb + u * (a.rgb - c.rgb) + v * (b.rgb - c.rgb);
    r.p = p;
    r.rgb = rgb;
    r.density = c.density;
    return r;
}

Corner mid(Corner a, Corner b)
{
    Corner r;
    precise float4 p = 0.5f * (a.p + b.p);
    precise float3 rgb = 0.5f * (a.rgb + b.rgb);
    precise float d = 0.5f * (a.density + b.density);
    r.p = p;
    r.rgb = rgb;
    r.density = d;
    return r;
}

Corner cornerOf(GSmall g, uint gi, float3 x, uint colour)
{
    Corner c;
    precise float4 p = g.m0 * x.x + g.m1 * x.y + g.m2 * x.z + g.m3;
    c.p = p;
    c.rgb = float3((float)(colour & 0xFFu), (float)((colour >> 8) & 0xFFu), (float)((colour >> 16) & 0xFFu));
    c.density = dv((float)((colour >> 24) & 0x7Fu), 127.0f);
    return c;
}

// A node's quarter `q` - in the CPU's order {m12 m01 m02} {t0 m01 m02} {m01 t1 m12} {m02 m12 t2}.
Node quarter(Node it, uint q)
{
    const Corner m01 = mid(it.t0, it.t1), m12 = mid(it.t1, it.t2), m02 = mid(it.t0, it.t2);
    Node r;
    precise float a = it.area2 * 0.25f;
    r.area2 = a;
    r.sub = 2.0f;
    r.t0 = m12; r.t1 = m01; r.t2 = m02;
    if (q == 1u) { r.t0 = it.t0; r.t1 = m01; r.t2 = m02; }
    else if (q == 2u) { r.t0 = m01; r.t1 = it.t1; r.t2 = m12; }
    else if (q == 3u) { r.t0 = m02; r.t1 = m12; r.t2 = it.t2; }
    return r;
}

// A packet of eight blades (nr_grass.cpp's packet): how many are drawn; written from `at` when `write`.
uint packet(GSmall g, uint gi, Corner c, Corner a, Corner b, int index, float height, bool write, uint at)
{
    uint n = 0u;
    const float dx = g.b2.x;
    [loop] for (int k = 0; k < 8; ++k)
    {
        const float4 q = grounds[gi].pts[4 * index + (k >> 1)];
        precise float uu = q.x + q.z;
        const float u = (k & 1) ? uu : q.x;
        const float v = (k & 1) ? q.w : q.y;
        const Corner base = lerp3(c, a, b, u, v);
        precise float tx = base.p.x + dx;
        precise float ty = base.p.y + height;
        const float4 tip = float4(tx, ty, base.p.z, base.p.w);
        if (abs(base.p.w) < 1e-6f || abs(tip.w) < 1e-6f || (outsideOf(g, gi, base.p) & outsideOf(g, gi, tip)) != 0u)
            continue;
        if (write && at + n < capacity)
        {
            const float x1 = dv(base.p.x, base.p.w), y1 = dv(base.p.y, base.p.w);
            const float x2 = dv(tip.x, tip.w), y2 = dv(tip.y, tip.w), z = dv(tip.z, tip.w);
            const uint ks = (uint)(k & 6);
            const float s1u = (ks == 2u || ks == 4u) ? 1.0f : 0.0f; // kSt: {0,0} {1,0} {1,1} {0,1} at 0, 2, 4, 6
            const float s1v = (ks == 4u || ks == 6u) ? 1.0f : 0.0f;
            Vtx o;
            o.colour = (uint)clamp(base.rgb.x, 0.0f, 255.0f) | (uint)clamp(base.rgb.y, 0.0f, 255.0f) << 8 |
                       (uint)clamp(base.rgb.z, 0.0f, 255.0f) << 16 | 0x7Fu << 24;
            o.z = z;
            o.nx = 0.0f;
            o.ny = 0.0f;
            o.nz = 0.0f;
            o.size = 0.0f;
            const uint first = 4u * (at + n);
            [unroll] for (uint i = 0u; i < 4u; ++i)
            {
                o.x = (i & 1u) ? x2 : x1;
                o.y = (i >> 1) ? y2 : y1;
                o.u = (i & 1u) ? 0.5f : s1u;
                o.v = (i >> 1) ? 0.5f : s1v;
                blades[first + i] = o;
            }
        }
        ++n;
    }
    return n;
}

// A node's blades (`packets`: false = only whether it splits): its count times two, plus one when its quarters come next.
uint node(GSmall g, uint gi, Node it, bool packets, bool write, uint at)
{
    uint n = 0u;
    bool split = false;
    if ((outsideOf(g, gi, it.t0.p) & outsideOf(g, gi, it.t1.p) & outsideOf(g, gi, it.t2.p)) == 0u)
    {
        [loop] for (int k = 0; k < 3; ++k)
        {
            // (the routine's corner `k` as C, with A and B: the third first, then the others in turn)
            Corner c = it.t1, a = it.t2, b = it.t0;
            if (k == 0) { c = it.t2; a = it.t0; b = it.t1; }
            else if (k == 1) { c = it.t0; a = it.t1; b = it.t2; }
            precise float dens = g.q23.z * c.density * it.area2;
            precise float far = 1.0f + g.thin * max(c.p.w - g.q23.x, 0.0f);
            precise float nn = dv(trunc(dens), far);
            precise float n2 = nn - it.sub;
            const float capped = min(n2, 8.0f);
            const int count = (int)trunc(max(capped, 0.0f));
            precise float excess = n2 - capped;
            precise float fr = n2 - trunc(n2);
            const float frac = excess == 0.0f ? fr : 0.0f;
            if (excess != 0.0f)
                split = true;
            if (packets)
            {
                const float height = g.b2.y;
                [loop] for (int p = 0; p < count; ++p)
                    n += packet(g, gi, c, a, b, p, height, write, at + n);
                if (frac > 0.1f && count < 8)
                {
                    precise float h = height * frac;
                    n += packet(g, gi, c, a, b, count, h, write, at + n);
                }
            }
        }
    }
    return 2u * n + (split ? 1u : 0u);
}

// One of the triangle's own packets: corner k's packet p (the routine's count of them, then the part-packet).
uint rootPacket(GSmall g, uint gi, Node it, uint k, uint p, bool write, uint at)
{
    uint n = 0u;
    if ((outsideOf(g, gi, it.t0.p) & outsideOf(g, gi, it.t1.p) & outsideOf(g, gi, it.t2.p)) == 0u)
    {
        Corner c = it.t1, a = it.t2, b = it.t0;
        if (k == 0u) { c = it.t2; a = it.t0; b = it.t1; }
        else if (k == 1u) { c = it.t0; a = it.t1; b = it.t2; }
        precise float dens = g.q23.z * c.density * it.area2;
        precise float far = 1.0f + g.thin * max(c.p.w - g.q23.x, 0.0f);
        precise float nn = dv(trunc(dens), far);
        precise float n2 = nn - it.sub;
        const float capped = min(n2, 8.0f);
        const int count = (int)trunc(max(capped, 0.0f));
        precise float excess = n2 - capped;
        precise float fr = n2 - trunc(n2);
        const float frac = excess == 0.0f ? fr : 0.0f;
        const float height = g.b2.y;
        if ((int)p < count)
            n = packet(g, gi, c, a, b, (int)p, height, write, at);
        else if ((int)p == count && frac > 0.1f && count < 8)
        {
            precise float h = height * frac;
            n = packet(g, gi, c, a, b, count, h, write, at);
        }
    }
    return n;
}

// A thread's blades: `seg` below kRootPackets one of the triangle's own packets; then kRootPackets + 5c its quarter c,
// kRootPackets + 5c + 1 + j that quarter's quarter j with all below it.
uint stretch(uint tri, uint seg, bool write, uint at)
{
    const Tri t = tris[tri];
    const uint gi = t.ground;
    GSmall g; // (the ground's small fields in registers; its points' table read from the buffer, a point a blade)
    g.m0 = grounds[gi].m0; g.m1 = grounds[gi].m1; g.m2 = grounds[gi].m2; g.m3 = grounds[gi].m3;
    g.b0 = grounds[gi].b0; g.b1 = grounds[gi].b1; g.b2 = grounds[gi].b2; g.q23 = grounds[gi].q23;
    g.thin = grounds[gi].thin;
    Node r;
    r.t0 = cornerOf(g, gi, t.p0, t.c0);
    r.t1 = cornerOf(g, gi, t.p1, t.c1);
    r.t2 = cornerOf(g, gi, t.p2, t.c2);
    r.area2 = t.area2;
    r.sub = 0.0f;
    uint n = 0u;
    if (seg < kRootPackets)
        n = rootPacket(g, gi, r, seg / 8u, seg % 8u, write, at);
    else if ((node(g, gi, r, false, false, 0u) & 1u) != 0u)
    {
        const uint c = (seg - kRootPackets) / 5u, j = (seg - kRootPackets) % 5u;
        const Node q = quarter(r, c);
        if (j == 0u)
            n = node(g, gi, q, true, write, at) >> 1;
        else if ((node(g, gi, q, false, false, 0u) & 1u) != 0u)
        {
            const Node q2 = quarter(q, j - 1u);
            const uint r2 = node(g, gi, q2, true, write, at);
            n = r2 >> 1;
            if ((r2 & 1u) != 0u)
            {
                [loop] for (uint a = 0u; a < 4u; ++a)
                {
                    const Node q3 = quarter(q2, a);
                    const uint r3 = node(g, gi, q3, true, write, at + n);
                    n += r3 >> 1;
                    if ((r3 & 1u) == 0u)
                        continue;
                    [loop] for (uint b = 0u; b < 4u; ++b) // (the fourth level splits no further)
                        n += node(g, gi, quarter(q3, b), true, write, at + n) >> 1;
                }
            }
        }
    }
    return n;
}

[numthreads(64, 1, 1)]
void CsCount(uint3 id : SV_DispatchThreadID)
{
    if (id.x < triCount * kSegs)
        counts[id.x] = stretch(id.x / kSegs, id.x % kSegs, false, 0u);
}

groupshared uint g_sum[1024];

[numthreads(1024, 1, 1)]
void CsScan(uint3 tid : SV_GroupThreadID)
{
    const uint segCount = triCount * kSegs;
    uint carry = 0u;
    for (uint start = 0u; start < segCount; start += 1024u)
    {
        const uint i = start + tid.x;
        const uint v = i < segCount ? counts[i] : 0u;
        g_sum[tid.x] = v;
        GroupMemoryBarrierWithGroupSync();
        for (uint off = 1u; off < 1024u; off <<= 1)
        {
            const uint add = tid.x >= off ? g_sum[tid.x - off] : 0u;
            GroupMemoryBarrierWithGroupSync();
            g_sum[tid.x] += add;
            GroupMemoryBarrierWithGroupSync();
        }
        if (i < segCount)
            counts[i] = carry + g_sum[tid.x] - v;
        const uint chunk = g_sum[1023];
        GroupMemoryBarrierWithGroupSync();
        carry += chunk;
    }
    AllMemoryBarrierWithGroupSync();
    if (tid.x == 0u)
        counts[segCount] = carry;
    AllMemoryBarrierWithGroupSync();
    for (uint gi = tid.x; gi < groundCount; gi += 1024u)
    {
        const uint first = grounds[gi].firstTri, count = grounds[gi].triCount;
        const uint a = first * kSegs, b = (first + count) * kSegs;
        const uint s = a < segCount ? counts[a] : carry;
        const uint e = b < segCount ? counts[b] : carry;
        const uint s2 = min(s, capacity), e2 = min(e, capacity);
        args[gi * 5u + 0u] = 6u * (e2 - s2);
        args[gi * 5u + 1u] = 1u;
        args[gi * 5u + 2u] = 0u;
        args[gi * 5u + 3u] = 4u * s2;
        args[gi * 5u + 4u] = 0u;
    }
}

[numthreads(64, 1, 1)]
void CsWrite(uint3 id : SV_DispatchThreadID)
{
    if (id.x < triCount * kSegs)
        stretch(id.x / kSegs, id.x % kSegs, true, counts[id.x]);
}
)HLSL";
            constexpr UINT kTriCap = 65536u, kGroundCap = 1024u, kBladeCap = 262144u;
            constexpr UINT kSegs = 44u; // (the shader's threads a triangle: kSegs there)
            constexpr UINT kTriBytes = 56u, kGroundBytes = 656u, kArgBytes = 20u;
            struct TriRec
            {
                float p0[3];
                uint32_t c0;
                float p1[3];
                uint32_t c1;
                float p2[3];
                uint32_t c2;
                float area2;
                uint32_t ground;
            };
            static_assert(sizeof(TriRec) == kTriBytes, "the triangle's layout");
            struct GroundRec
            {
                float m[16];
                float b0[4], b1[4], b2[4], q23[4];
                float pts[32][4];
                float thin;
                uint32_t firstTri, triCount, pad;
            };
            static_assert(sizeof(GroundRec) == kGroundBytes, "the ground's layout");

            struct Slot
            {
                ID3D12Resource *up = nullptr; // (the triangles, then the grounds: written here, read by the pass)
                uint8_t *upMapped = nullptr;
                ID3D12Resource *counts = nullptr, *blades = nullptr, *args = nullptr;
                // (the pass in a list of its own, run once a frame before its first picture - the frame's list is run again
                // for every picture between frames, and the blades are the frame's: grown once. Its buffers start and end it
                // in COMMON, where a buffer is anyway after each ExecuteCommandLists.)
                ID3D12CommandAllocator *alloc = nullptr;
                ID3D12GraphicsCommandList *list = nullptr;
                bool pending = false;
                ID3D12Resource *readback = nullptr; // (TF_GRASS_GPU_CHECK: the blades and the arguments copied out)
                std::vector<std::shared_ptr<const Mesh>> checks; // (this frame's CPU blades, by ground)
                std::vector<uint32_t> triCounts; // (and its triangles)
                bool checking = false;
            };
            ID3D12RootSignature *g_root = nullptr;
            ID3D12PipelineState *g_pso[3] = {};
            ID3D12CommandSignature *g_sig = nullptr;
            ID3D12Resource *g_indices = nullptr; // (0 1 2 2 1 3, 4 5 6 6 5 7 ...: a blade's quad)
            Slot g_slots[State::kFrames];
            std::atomic<bool> g_ready{false};
            bool g_exact = false;
            std::unordered_map<uint64_t, GpuMesh> g_meshes; // (this frame's grounds' draws, by their mesh's id)
            uint64_t g_frames = 0, g_grounds = 0, g_tris = 0, g_dropped = 0;
            // (the pass's own time on the GPU: a timestamp before and after it, per slot - read when the slot comes round again)
            ID3D12QueryHeap *g_timeHeap = nullptr;
            ID3D12Resource *g_timeBack = nullptr;
            bool g_timed[State::kFrames] = {};
            double g_gpuMs = 0.0, g_gpuMsMax = 0.0;
            uint64_t g_gpuTimed = 0;

            bool init()
            {
                // The pass's root: its counts (b0), the triangles and grounds (t0, t1), the counts, blades and arguments (u0-u2).
                D3D12_ROOT_PARAMETER p[6] = {};
                p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
                p[0].Constants.ShaderRegister = 0;
                p[0].Constants.Num32BitValues = 4;
                for (int k = 1; k < 6; ++k)
                {
                    p[k].ParameterType = k <= 2 ? D3D12_ROOT_PARAMETER_TYPE_SRV : D3D12_ROOT_PARAMETER_TYPE_UAV;
                    p[k].Descriptor.ShaderRegister = k <= 2 ? static_cast<UINT>(k - 1) : static_cast<UINT>(k - 3);
                }
                for (auto &q : p)
                    q.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
                D3D12_ROOT_SIGNATURE_DESC rsd{};
                rsd.NumParameters = 6;
                rsd.pParameters = p;
                ID3DBlob *blob = nullptr, *errors = nullptr;
                const bool rootOk = SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) &&
                                    SUCCEEDED(g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_root)));
                release(blob);
                release(errors);
                if (!rootOk)
                    return false;
                // (the divisions in double where the GPU can: every NVIDIA and AMD card of the target's - else float, a
                // division a few units in the last place off)
                D3D12_FEATURE_DATA_D3D12_OPTIONS opts{};
                g_exact = SUCCEEDED(g.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opts, sizeof(opts))) && opts.DoublePrecisionFloatShaderOps;
                if (std::getenv("TF_GRASS_GPU_FLOATDIV"))
                    g_exact = false;
                const D3D_SHADER_MACRO exact[] = {{"EXACT_DIV", "1"}, {nullptr, nullptr}};
                const char *entries[3] = {"CsCount", "CsScan", "CsWrite"};
                for (int k = 0; k < 3; ++k)
                {
                    // (from the shader cache like the others: compiled here at every start they took ~15 s of it on an
                    // Intel UHD - N100 - with the optimizer at level 3 and IEEE strictness)
                    ID3DBlob *code = compile(entries[k], "cs_5_0", kCs, sizeof(kCs) - 1, g_exact ? exact : nullptr,
                                             D3DCOMPILE_IEEE_STRICTNESS);
                    if (!code)
                    {
                        std::fprintf(stderr, "[native render] grass on the GPU: shader %s failed\n", entries[k]);
                        return false;
                    }
                    D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
                    cd.pRootSignature = g_root;
                    cd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
                    const HRESULT hp = g.device->CreateComputePipelineState(&cd, IID_PPV_ARGS(&g_pso[k]));
                    release(code);
                    if (FAILED(hp))
                        return false;
                }
                D3D12_INDIRECT_ARGUMENT_DESC ad{};
                ad.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
                D3D12_COMMAND_SIGNATURE_DESC sd{};
                sd.ByteStride = kArgBytes;
                sd.NumArgumentDescs = 1;
                sd.pArgumentDescs = &ad;
                if (FAILED(g.device->CreateCommandSignature(&sd, nullptr, IID_PPV_ARGS(&g_sig))))
                    return false;
                const D3D12_HEAP_PROPERTIES up = heap(D3D12_HEAP_TYPE_UPLOAD), gpu = heap(D3D12_HEAP_TYPE_DEFAULT), back = heap(D3D12_HEAP_TYPE_READBACK);
                {
                    const D3D12_RESOURCE_DESC d = bufferDesc(static_cast<UINT64>(kBladeCap) * 6u * 4u);
                    if (FAILED(g.device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_indices))))
                        return false;
                    void *m = nullptr;
                    const D3D12_RANGE none{0, 0};
                    if (FAILED(g_indices->Map(0, &none, &m)))
                        return false;
                    uint32_t *ix = static_cast<uint32_t *>(m);
                    for (uint32_t b = 0; b < kBladeCap; ++b)
                    {
                        const uint32_t q[6] = {0u, 1u, 2u, 2u, 1u, 3u};
                        for (int k = 0; k < 6; ++k)
                            ix[6u * b + static_cast<uint32_t>(k)] = 4u * b + q[k];
                    }
                    g_indices->Unmap(0, nullptr);
                }
                for (Slot &s : g_slots)
                {
                    const D3D12_RESOURCE_DESC ud = bufferDesc(static_cast<UINT64>(kTriCap) * kTriBytes + static_cast<UINT64>(kGroundCap) * kGroundBytes);
                    D3D12_RESOURCE_DESC cd = bufferDesc((static_cast<UINT64>(kTriCap) * kSegs + 1ull) * 4u), bd = bufferDesc(static_cast<UINT64>(kBladeCap) * 4u * sizeof(Vertex)),
                                        ad2 = bufferDesc(static_cast<UINT64>(kGroundCap) * kArgBytes);
                    cd.Flags = bd.Flags = ad2.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                    const D3D12_RESOURCE_DESC rd = bufferDesc(static_cast<UINT64>(kBladeCap) * 4u * sizeof(Vertex) + static_cast<UINT64>(kGroundCap) * kArgBytes);
                    void *m = nullptr;
                    const D3D12_RANGE none{0, 0};
                    if (FAILED(g.device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &ud, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&s.up))) ||
                        FAILED(s.up->Map(0, &none, &m)) ||
                        FAILED(g.device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &cd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&s.counts))) ||
                        FAILED(g.device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&s.blades))) ||
                        FAILED(g.device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &ad2, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&s.args))))
                        return false;
                    s.upMapped = static_cast<uint8_t *>(m);
                    if (FAILED(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.alloc))) ||
                        FAILED(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.alloc, nullptr, IID_PPV_ARGS(&s.list))))
                        return false;
                    s.list->Close();
                    if (std::getenv("TF_GRASS_GPU_CHECK") &&
                        FAILED(g.device->CreateCommittedResource(&back, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s.readback))))
                        s.readback = nullptr;
                }
                if (g.timestampHz)
                {
                    D3D12_QUERY_HEAP_DESC th{};
                    th.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
                    th.Count = 2u * State::kFrames;
                    const D3D12_RESOURCE_DESC tb = bufferDesc(16ull * State::kFrames);
                    if (FAILED(g.device->CreateQueryHeap(&th, IID_PPV_ARGS(&g_timeHeap))) ||
                        FAILED(g.device->CreateCommittedResource(&back, D3D12_HEAP_FLAG_NONE, &tb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_timeBack))))
                    {
                        release(g_timeHeap);
                        release(g_timeBack);
                    }
                }
                std::fprintf(stderr, "[native render] grass on the GPU: ready (its divisions %s; room for %u blades, %u triangles a frame)\n",
                             g_exact ? "in double, as the CPU's" : "in float", kBladeCap, kTriCap);
                return true;
            }

            void move(ID3D12GraphicsCommandList *l, ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
            {
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = r;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = from;
                b.Transition.StateAfter = to;
                l->ResourceBarrier(1, &b);
            }

            // The frame's grounds grown: at the start of its list, before any draw.
            void record(const Frame &input)
            {
                g_meshes.clear();
                if (!g_ready.load(std::memory_order_relaxed))
                    return;
                Slot &s = g_slots[g.slot];
                s.checks.clear();
                s.checking = false;
                s.pending = false;
                if (g_timed[g.slot] && g_timeBack)
                {
                    // (this slot's last pass: its list long done - the slot's allocator was reset for this one)
                    g_timed[g.slot] = false;
                    void *p = nullptr;
                    const D3D12_RANGE r{16u * g.slot, 16u * g.slot + 16u}, none{0, 0};
                    if (SUCCEEDED(g_timeBack->Map(0, &r, &p)))
                    {
                        uint64_t t[2];
                        std::memcpy(t, static_cast<uint8_t *>(p) + 16u * g.slot, 16u);
                        g_timeBack->Unmap(0, &none);
                        if (t[1] > t[0])
                        {
                            const double ms = static_cast<double>(t[1] - t[0]) * 1000.0 / static_cast<double>(g.timestampHz);
                            g_gpuMs += ms;
                            g_gpuMsMax = std::max(g_gpuMsMax, ms);
                            ++g_gpuTimed;
                        }
                    }
                }
                TriRec *tris = reinterpret_cast<TriRec *>(s.upMapped);
                GroundRec *grounds = reinterpret_cast<GroundRec *>(s.upMapped + static_cast<size_t>(kTriCap) * kTriBytes);
                uint32_t triCount = 0, groundCount = 0;
                for (const Draw &d : input.draws)
                {
                    if (!d.mesh || !d.mesh->gpuGrass || g_meshes.count(d.mesh->id))
                        continue;
                    const GpuGrass &gg = *d.mesh->gpuGrass;
                    const Mesh &ground = *gg.ground;
                    const uint32_t tris3 = static_cast<uint32_t>(ground.indices.size() / 3u);
                    if (groundCount >= kGroundCap || triCount + tris3 > kTriCap)
                    {
                        ++g_dropped;
                        continue; // (not drawn: more grass than the pass's room - counted in its line)
                    }
                    GroundRec &gr = grounds[groundCount];
                    std::memcpy(gr.m, gg.block[3].v, 64u);
                    std::memcpy(gr.b0, gg.block[0].v, 16u);
                    std::memcpy(gr.b1, gg.block[1].v, 16u);
                    std::memcpy(gr.b2, gg.block[2].v, 16u);
                    std::memcpy(gr.q23, gg.block[23].v, 16u);
                    std::memcpy(gr.pts, gg.block[24].v, sizeof(gr.pts));
                    gr.thin = gg.thinning;
                    gr.firstTri = triCount;
                    gr.pad = 0u;
                    uint32_t made = 0;
                    for (size_t i = 0; i + 2 < ground.indices.size(); i += 3)
                    {
                        const uint32_t i0 = ground.indices[i], i1 = ground.indices[i + 1], i2 = ground.indices[i + 2];
                        if (i0 >= ground.vertices.size() || i1 >= ground.vertices.size() || i2 >= ground.vertices.size())
                            continue;
                        const Vertex &v0 = ground.vertices[i0], &v1 = ground.vertices[i1], &v2 = ground.vertices[i2];
                        // (nr_grass.cpp's area, in its own steps)
                        const float ax = v0.x - v2.x, ay = v0.y - v2.y, az = v0.z - v2.z;
                        const float bx = v1.x - v2.x, by = v1.y - v2.y, bz = v1.z - v2.z;
                        const float cx = ay * bz - az * by, cy = az * bx - ax * bz, cz = ax * by - ay * bx;
                        TriRec &t = tris[triCount + made];
                        t.p0[0] = v0.x, t.p0[1] = v0.y, t.p0[2] = v0.z, t.c0 = v0.colour;
                        t.p1[0] = v1.x, t.p1[1] = v1.y, t.p1[2] = v1.z, t.c1 = v1.colour;
                        t.p2[0] = v2.x, t.p2[1] = v2.y, t.p2[2] = v2.z, t.c2 = v2.colour;
                        t.area2 = std::sqrt(cx * cx + cy * cy + cz * cz);
                        t.ground = groundCount;
                        ++made;
                    }
                    gr.triCount = made;
                    triCount += made;
                    GpuMesh m;
                    m.vbv = {s.blades->GetGPUVirtualAddress(), static_cast<UINT>(static_cast<UINT64>(kBladeCap) * 4u * sizeof(Vertex)), sizeof(Vertex)};
                    m.ibv = {g_indices->GetGPUVirtualAddress(), kBladeCap * 6u * 4u, DXGI_FORMAT_R32_UINT};
                    m.count = 0;
                    m.indirect = true;
                    m.argsAt = static_cast<UINT64>(groundCount) * kArgBytes;
                    g_meshes[d.mesh->id] = m;
                    if (gg.cpu)
                    {
                        s.checks.resize(groundCount + 1u);
                        s.checks[groundCount] = gg.cpu;
                        s.triCounts.resize(groundCount + 1u);
                        s.triCounts[groundCount] = made;
                        s.checking = s.readback != nullptr;
                    }
                    ++groundCount;
                }
                if (groundCount == 0u)
                    return;
                ++g_frames;
                g_grounds += groundCount;
                g_tris += triCount;
                if (FAILED(s.alloc->Reset()) || FAILED(s.list->Reset(s.alloc, nullptr)))
                {
                    g_meshes.clear();
                    return;
                }
                ID3D12GraphicsCommandList *l = s.list;
                if (g_timeHeap)
                    l->EndQuery(g_timeHeap, D3D12_QUERY_TYPE_TIMESTAMP, 2u * g.slot);
                constexpr D3D12_RESOURCE_STATES kCommon = D3D12_RESOURCE_STATE_COMMON, kUav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                kCopy = D3D12_RESOURCE_STATE_COPY_SOURCE;
                move(l, s.counts, kCommon, kUav);
                move(l, s.blades, kCommon, kUav);
                move(l, s.args, kCommon, kUav);
                l->SetComputeRootSignature(g_root);
                const uint32_t totals[4] = {triCount, groundCount, kBladeCap, 0u};
                l->SetComputeRoot32BitConstants(0, 4, totals, 0);
                l->SetComputeRootShaderResourceView(1, s.up->GetGPUVirtualAddress());
                l->SetComputeRootShaderResourceView(2, s.up->GetGPUVirtualAddress() + static_cast<UINT64>(kTriCap) * kTriBytes);
                l->SetComputeRootUnorderedAccessView(3, s.counts->GetGPUVirtualAddress());
                l->SetComputeRootUnorderedAccessView(4, s.blades->GetGPUVirtualAddress());
                l->SetComputeRootUnorderedAccessView(5, s.args->GetGPUVirtualAddress());
                D3D12_RESOURCE_BARRIER uav{};
                uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                l->SetPipelineState(g_pso[0]);
                l->Dispatch((triCount * kSegs + 63u) / 64u, 1, 1);
                uav.UAV.pResource = s.counts;
                l->ResourceBarrier(1, &uav);
                l->SetPipelineState(g_pso[1]);
                l->Dispatch(1, 1, 1);
                l->ResourceBarrier(1, &uav);
                l->SetPipelineState(g_pso[2]);
                l->Dispatch((triCount * kSegs + 63u) / 64u, 1, 1);
                move(l, s.counts, kUav, kCommon);
                if (s.checking)
                {
                    move(l, s.blades, kUav, kCopy);
                    move(l, s.args, kUav, kCopy);
                    const UINT64 bladeBytes = static_cast<UINT64>(kBladeCap) * 4u * sizeof(Vertex);
                    l->CopyBufferRegion(s.readback, 0, s.blades, 0, bladeBytes);
                    l->CopyBufferRegion(s.readback, bladeBytes, s.args, 0, static_cast<UINT64>(groundCount) * kArgBytes);
                    move(l, s.blades, kCopy, kCommon);
                    move(l, s.args, kCopy, kCommon);
                }
                else
                {
                    move(l, s.blades, kUav, kCommon);
                    move(l, s.args, kUav, kCommon);
                }
                if (g_timeHeap)
                {
                    l->EndQuery(g_timeHeap, D3D12_QUERY_TYPE_TIMESTAMP, 2u * g.slot + 1u);
                    l->ResolveQueryData(g_timeHeap, D3D12_QUERY_TYPE_TIMESTAMP, 2u * g.slot, 2u, g_timeBack, 16ull * g.slot);
                    g_timed[g.slot] = true;
                }
                if (FAILED(l->Close()))
                {
                    g_meshes.clear();
                    return;
                }
                s.pending = true;
                // The frame's list (every picture's run of it) reads them: from COMMON - after the pass, or decayed after the
                // picture before - each run. (Plain barriers, not the journal's: there is nothing to set back.)
                move(g.list, s.blades, kCommon, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
                move(g.list, s.args, kCommon, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
                static auto since = std::chrono::steady_clock::now();
                if (std::chrono::steady_clock::now() - since > std::chrono::seconds(10))
                {
                    std::fprintf(stderr, "[native render] grass on the GPU: %llu frames, %.1f grounds and %.0f triangles a frame; the pass %.3f ms of GPU time a frame, %.3f at worst%s\n",
                                 static_cast<unsigned long long>(g_frames), static_cast<double>(g_grounds) / std::max<uint64_t>(1u, g_frames),
                                 static_cast<double>(g_tris) / std::max<uint64_t>(1u, g_frames), g_gpuMs / static_cast<double>(std::max<uint64_t>(1u, g_gpuTimed)),
                                 g_gpuMsMax, g_dropped ? " - some grounds left out: no room" : "");
                    g_frames = g_grounds = g_tris = g_dropped = g_gpuTimed = 0;
                    g_gpuMs = g_gpuMsMax = 0.0;
                    since = std::chrono::steady_clock::now();
                }
            }

            // The frame's pass, once: for the first picture's submission (null: none this frame, or taken already).
            ID3D12CommandList *takeList()
            {
                Slot &s = g_slots[g.slot];
                if (!s.pending)
                    return nullptr;
                s.pending = false;
                return s.list;
            }

            GpuMesh *meshFor(const Mesh &mesh)
            {
                const auto it = g_meshes.find(mesh.id);
                return it == g_meshes.end() ? nullptr : &it->second;
            }
            GpuMesh *meshFor(uint64_t id)
            {
                const auto it = g_meshes.find(id);
                return it == g_meshes.end() ? nullptr : &it->second;
            }

            // A ground's blades drawn: its stretch of the frame's blades, by the GPU's arguments - the pass's own instance
            // entry (its matrix: the identity) the view's first.
            void draw(const GpuMesh &m, const D3D12_VERTEX_BUFFER_VIEW &instView, UINT firstInst)
            {
                D3D12_VERTEX_BUFFER_VIEW iv = instView;
                const UINT64 skip = static_cast<UINT64>(firstInst) * iv.StrideInBytes;
                iv.BufferLocation += skip;
                iv.SizeInBytes = iv.SizeInBytes > skip ? static_cast<UINT>(iv.SizeInBytes - skip) : 0u;
                g.list->IASetVertexBuffers(1, 1, &iv);
                g.list->ExecuteIndirect(g_sig, 1, g_slots[g.slot].args, m.argsAt, nullptr, 0);
            }

            // (TF_GRASS_GPU_CHECK) A checked frame's blades, once the GPU is done with it, against the CPU's - each ground's
            // count, and its vertices to the bit.
            void check(UINT slot)
            {
                Slot &s = g_slots[slot];
                if (!s.checking)
                    return;
                s.checking = false;
                const UINT64 bladeBytes = static_cast<UINT64>(kBladeCap) * 4u * sizeof(Vertex);
                void *m = nullptr;
                const D3D12_RANGE all{0, static_cast<SIZE_T>(bladeBytes + static_cast<UINT64>(s.checks.size()) * kArgBytes)};
                if (FAILED(s.readback->Map(0, &all, &m)))
                    return;
                const Vertex *v = static_cast<const Vertex *>(m);
                const uint32_t *args = reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(m) + bladeBytes);
                size_t grounds = 0, blades = 0, countOff = 0, verticesOff = 0, bitsOff = 0;
                double worst = 0.0;
                for (size_t gi = 0; gi < s.checks.size(); ++gi)
                {
                    const Mesh *cpu = s.checks[gi].get();
                    if (!cpu)
                        continue;
                    ++grounds;
                    const uint32_t n = args[5u * gi] / 6u, first = args[5u * gi + 3u];
                    const size_t want = cpu->vertices.size() / 4u;
                    blades += want;
                    if (n != want)
                    {
                        if (++countOff <= 3u)
                            std::fprintf(stderr, "[native render] grass on the GPU, checked: ground %zu (%u triangles) - %u blades from %u, the CPU's %zu\n", gi,
                                         gi < s.triCounts.size() ? s.triCounts[gi] : 0u, n, first, want);
                        continue;
                    }
                    for (size_t k = 0; k < cpu->vertices.size(); ++k)
                    {
                        const Vertex &a = cpu->vertices[k], &b = v[first + k];
                        if (std::memcmp(&a, &b, sizeof(Vertex)) != 0)
                        {
                            ++bitsOff;
                            const double d = std::max({std::fabs(static_cast<double>(a.x) - b.x), std::fabs(static_cast<double>(a.y) - b.y),
                                                       std::fabs(static_cast<double>(a.z) - b.z) * 1e4, std::fabs(static_cast<double>(a.u) - b.u),
                                                       std::fabs(static_cast<double>(a.v) - b.v)});
                            worst = std::max(worst, d);
                            if (a.colour != b.colour || d > 1e-3)
                                ++verticesOff;
                        }
                    }
                }
                const D3D12_RANGE none{0, 0};
                s.readback->Unmap(0, &none);
                std::fprintf(stderr, "[native render] grass on the GPU, checked: %zu grounds, %zu blades; %zu grounds with another count, %zu vertices not the "
                                     "CPU's to the bit, %zu off by more than 1e-3 (GS pixels; z x 1e4) or in colour; at most %.3g\n",
                             grounds, blades, countOff, bitsOff, verticesOff, worst);
            }
        }

        GpuMesh *upload(const std::shared_ptr<const Mesh> &mesh)
        {
            if (mesh->gpuGrass)
                return grassgpu::meshFor(*mesh); // (grassgpu: grown on the GPU at the frame's start)
            // A small mesh not resident goes into the frame's ring too: the meshes made anew every
            // frame (a buffer of their own each - CreateCommittedResource, about a third of a ms -
            // cost the carrier's second map 37 ms a frame: 110 of them, six vertices each). Copying
            // a few hundred bytes a frame costs nothing. TF_NATIVE_SMALLRING=0: buffers of their own.
            static const bool smallRing = !(std::getenv("TF_NATIVE_SMALLRING") && *std::getenv("TF_NATIVE_SMALLRING") == '0');
            const bool tiny = smallRing && mesh->vertices.size() <= 256u && g.meshes.find(mesh->id) == g.meshes.end();
            if (mesh->immediate || mesh->primitive || mesh->ocean || tiny) // (this frame's alone: in the frame's ring - a buffer of its own each frame cost the ocean's patches 12 ms)
            {
                const UINT64 vb = mesh->vertices.size() * sizeof(Vertex), ib = mesh->indices.size() * sizeof(uint32_t);
                gfx::RingSpan span;
                if (!g.dynamic || vb == 0 || !gfx::device().ringAlloc(gfx::RingKind::Vertices, static_cast<uint32_t>(vb + ib), 16u, span))
                {
                    g_ringFull += g.dynamic && vb != 0 ? 1u : 0u; // (TODO 99 check: the frame's ring full - the mesh not drawn)
                    return nullptr;
                }
                std::memcpy(span.cpu, mesh->vertices.data(), vb);
                std::memcpy(static_cast<uint8_t *>(span.cpu) + vb, mesh->indices.data(), ib);
                const D3D12_GPU_VIRTUAL_ADDRESS base = span.gpu;
                g.dynamicMesh.ringVertices = span;
                g.dynamicMesh.ringVertices.size = static_cast<uint32_t>(vb);
                g.dynamicMesh.ringIndices = span;
                g.dynamicMesh.ringIndices.cpu = static_cast<uint8_t *>(span.cpu) + vb;
                g.dynamicMesh.ringIndices.gpu = span.gpu + vb;
                g.dynamicMesh.ringIndices.size = static_cast<uint32_t>(ib);
                g.dynamicMesh.vbv = {base, static_cast<UINT>(vb), sizeof(Vertex)};
                g.dynamicMesh.ibv = {base + vb, static_cast<UINT>(ib), DXGI_FORMAT_R32_UINT};
                g.dynamicMesh.count = static_cast<UINT>(mesh->indices.size());
                g.dynamicMesh.bounded = false; // (its box made when a pass needs it)
                ++g_ringMeshes;
                return &g.dynamicMesh;
            }
            auto it = g.meshes.find(mesh->id);
            if (it != g.meshes.end())
                return &it->second;
            if (mesh->vertices.empty() || mesh->indices.empty())
                return nullptr;
            const UINT64 vertexBytes = mesh->vertices.size() * sizeof(Vertex);
            const UINT64 indexBytes = mesh->indices.size() * sizeof(uint32_t);
            ++g_meshesMade;
            g_meshVertices += mesh->vertices.size();
            static int meshLog = std::getenv("TF_NATIVE_MESHLOG") ? std::atoi(std::getenv("TF_NATIVE_MESHLOG")) : 0;
            if (meshLog > 0 && g.frameIndex > 600u)
            {
                --meshLog;
                std::fprintf(stderr, "[native render] mesh buffer: id %llu source %08x, %zu vertices %zu indices, type %u sprites %d ocean %d prim %x\n",
                             static_cast<unsigned long long>(mesh->id), mesh->source, mesh->vertices.size(), mesh->indices.size(), mesh->type,
                             mesh->sprites, mesh->ocean, mesh->prim);
            }
            GpuMesh m;
            m.meshId = gfx::device().createMesh(mesh->vertices.data(), static_cast<uint32_t>(vertexBytes), mesh->indices.data(),
                                                static_cast<uint32_t>(indexBytes));
            if (!m.meshId)
                return nullptr;
            {
                const State::DeviceMesh &d = g.devMeshes[m.meshId];
                m.vbv = d.vbv;
                m.ibv = d.ibv;
                m.vbvV = d.vbvV;
                m.ibvV = d.ibvV;
            }
            m.mesh = mesh;
            m.count = static_cast<UINT>(mesh->indices.size());
            boundsOf(*mesh, m);
            m.bounded = true;
            return &g.meshes.emplace(mesh->id, m).first->second;
        }

        // (TODO 204) The ground's blend layers softened in the mesh: a layer - ALPHA (Cs - Cd) As + Cd, Z not written, a
        // mesh of the game's - drawn with its vertices' colour (its baked shading; TF_NATIVE_LAYERSOFT_ALPHA=1: the alpha that fades it in too)
        // smoothed over their neighbours: the straight edges where the shading bent at a triangle's side spread over the
        // triangles round them. Worked out once per mesh (kept while the mesh lives); the mesh's border - where the next
        // chunk of ground goes on - left as it is, so the chunks still meet. The textures stay as they are; nothing more
        // for the GPU. TF_NATIVE_LAYERSOFT=<passes> (default 2; 0: as the PS2 draws them; each pass moves a vertex half way to its
        // neighbours' mean).
        namespace layersoft
        {
            int passes()
            {
                static const int v = [] {
                    const char *e = std::getenv("TF_NATIVE_LAYERSOFT");
                    return e && *e ? std::max(0, std::atoi(e)) : 2;
                }();
                return v;
            }
            // The alpha smoothed too (TF_NATIVE_LAYERSOFT_ALPHA=1), or the colour alone (the default: the layers' alphas are
            // mostly all or nothing a vertex - smoothed, a layer's small patches fade away).
            bool alpha()
            {
                static const bool v = std::getenv("TF_NATIVE_LAYERSOFT_ALPHA") && *std::getenv("TF_NATIVE_LAYERSOFT_ALPHA") == '1';
                return v;
            }
            // A draw that is one of them.
            bool isLayer(const Draw &draw)
            {
                return passes() > 0 && draw.mesh && !draw.mesh->immediate && !draw.mesh->sprites && !draw.mesh->gpuGrass &&
                       (draw.mesh->prim & 0x40u) != 0u && draw.mesh->prim2 == 0u && (draw.alpha & 0xFFu) == 0x44u && ((draw.zbuf >> 32) & 1u) != 0u &&
                       !draw.mesh->vertices.empty() && !draw.mesh->indices.empty();
            }
            // The vertices with their colours smoothed: the same positions welded into one point, its neighbours those it
            // shares a triangle with; a point on an edge only one triangle has (the mesh's border) kept.
            std::vector<Vertex> smooth(const Mesh &mesh, int passes)
            {
                std::vector<Vertex> out = mesh.vertices;
                const size_t n = mesh.vertices.size();
                struct Key
                {
                    uint32_t x, y, z;
                    bool operator==(const Key &o) const { return x == o.x && y == o.y && z == o.z; }
                };
                struct KeyHash
                {
                    size_t operator()(const Key &k) const { return (static_cast<size_t>(k.x) * 73856093u) ^ (static_cast<size_t>(k.y) * 19349663u) ^ (static_cast<size_t>(k.z) * 83492791u); }
                };
                std::unordered_map<Key, uint32_t, KeyHash> welded;
                welded.reserve(n);
                std::vector<uint32_t> point(n);
                for (size_t i = 0; i < n; ++i)
                {
                    Key k;
                    std::memcpy(&k.x, &mesh.vertices[i].x, 4);
                    std::memcpy(&k.y, &mesh.vertices[i].y, 4);
                    std::memcpy(&k.z, &mesh.vertices[i].z, 4);
                    point[i] = welded.emplace(k, static_cast<uint32_t>(welded.size())).first->second;
                }
                const size_t points = welded.size();
                // The edges (by their points), how many triangles each, and each point's neighbours.
                std::unordered_map<uint64_t, uint32_t> edges;
                std::vector<std::vector<uint32_t>> adjacent(points);
                for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3)
                {
                    uint32_t c[3];
                    bool ok = true;
                    for (int k = 0; k < 3; ++k)
                    {
                        if (mesh.indices[t + k] >= n)
                            ok = false;
                        else
                            c[k] = point[mesh.indices[t + k]];
                    }
                    if (!ok || c[0] == c[1] || c[1] == c[2] || c[0] == c[2])
                        continue;
                    for (int k = 0; k < 3; ++k)
                    {
                        const uint32_t a = c[k], b = c[(k + 1) % 3];
                        const uint64_t e = a < b ? (static_cast<uint64_t>(a) << 32 | b) : (static_cast<uint64_t>(b) << 32 | a);
                        if (edges[e]++ == 0u)
                        {
                            adjacent[a].push_back(b);
                            adjacent[b].push_back(a);
                        }
                    }
                }
                std::vector<char> border(points, 0);
                for (const auto &[e, count] : edges)
                    if (count == 1u)
                        border[static_cast<uint32_t>(e >> 32)] = border[static_cast<uint32_t>(e)] = 1;
                // Each point's colour (its vertices' mean), smoothed; each vertex moved by its point's change.
                std::vector<std::array<float, 4>> colour(points, {0.0f, 0.0f, 0.0f, 0.0f});
                std::vector<uint32_t> users(points, 0u);
                for (size_t i = 0; i < n; ++i)
                {
                    const uint32_t c = mesh.vertices[i].colour;
                    for (int k = 0; k < 4; ++k)
                        colour[point[i]][k] += static_cast<float>((c >> (8 * k)) & 0xFFu);
                    ++users[point[i]];
                }
                for (size_t q = 0; q < points; ++q)
                    for (int k = 0; k < 4; ++k)
                        colour[q][k] /= static_cast<float>(std::max(users[q], 1u));
                std::vector<std::array<float, 4>> now = colour, next(points);
                for (int pass = 0; pass < passes; ++pass)
                {
                    for (size_t q = 0; q < points; ++q)
                    {
                        next[q] = now[q];
                        if (border[q] || adjacent[q].empty())
                            continue;
                        std::array<float, 4> mean{0.0f, 0.0f, 0.0f, 0.0f};
                        for (const uint32_t o : adjacent[q])
                            for (int k = 0; k < 4; ++k)
                                mean[k] += now[o][k];
                        for (int k = 0; k < (alpha() ? 4 : 3); ++k)
                            next[q][k] = 0.5f * now[q][k] + 0.5f * mean[k] / static_cast<float>(adjacent[q].size());
                    }
                    now.swap(next);
                }
                for (size_t i = 0; i < n; ++i)
                {
                    const uint32_t c = mesh.vertices[i].colour;
                    uint32_t r = 0u;
                    for (int k = 0; k < 4; ++k)
                    {
                        const float v = static_cast<float>((c >> (8 * k)) & 0xFFu) + now[point[i]][k] - colour[point[i]][k];
                        r |= static_cast<uint32_t>(std::clamp(std::lround(v), 0l, 255l)) << (8 * k);
                    }
                    out[i].colour = r;
                }
                return out;
            }
            struct Entry
            {
                std::weak_ptr<const Mesh> mesh;
                std::vector<Vertex> vertices;
                GpuMesh gpu;
            };
            std::unordered_map<uint64_t, Entry> g_cache;
            uint64_t g_made = 0, g_drawn = 0; // (the profile)
            // A mesh's softened vertices (worked out the first time it is seen).
            Entry &entry(const std::shared_ptr<const Mesh> &mesh)
            {
                Entry &e = g_cache[mesh->id];
                if (e.mesh.lock() != mesh)
                {
                    if (e.gpu.meshId)
                        gfx::device().releaseMesh(e.gpu.meshId);
                    e = Entry{};
                    e.mesh = mesh;
                    e.vertices = smooth(*mesh, passes());
                    ++g_made;
                }
                return e;
            }
        }

        // A ground layer's mesh with its softened vertices (layersoft): kept on the GPU as upload keeps the game's meshes,
        // or - one made anew each frame, or small - in the frame's ring.
        GpuMesh *uploadSoft(const std::shared_ptr<const Mesh> &mesh)
        {
            layersoft::Entry &e = layersoft::entry(mesh);
            ++layersoft::g_drawn;
            static const bool smallRing = !(std::getenv("TF_NATIVE_SMALLRING") && *std::getenv("TF_NATIVE_SMALLRING") == '0');
            const bool tiny = smallRing && mesh->vertices.size() <= 256u && g.meshes.find(mesh->id) == g.meshes.end();
            if (mesh->primitive || mesh->ocean || tiny)
            {
                const UINT64 vb = e.vertices.size() * sizeof(Vertex), ib = mesh->indices.size() * sizeof(uint32_t);
                gfx::RingSpan span;
                if (!g.dynamic || vb == 0 || !gfx::device().ringAlloc(gfx::RingKind::Vertices, static_cast<uint32_t>(vb + ib), 16u, span))
                {
                    g_ringFull += g.dynamic && vb != 0 ? 1u : 0u;
                    return nullptr;
                }
                std::memcpy(span.cpu, e.vertices.data(), vb);
                std::memcpy(static_cast<uint8_t *>(span.cpu) + vb, mesh->indices.data(), ib);
                const D3D12_GPU_VIRTUAL_ADDRESS base = span.gpu;
                g.dynamicMesh.ringVertices = span;
                g.dynamicMesh.ringVertices.size = static_cast<uint32_t>(vb);
                g.dynamicMesh.ringIndices = span;
                g.dynamicMesh.ringIndices.cpu = static_cast<uint8_t *>(span.cpu) + vb;
                g.dynamicMesh.ringIndices.gpu = span.gpu + vb;
                g.dynamicMesh.ringIndices.size = static_cast<uint32_t>(ib);
                g.dynamicMesh.vbv = {base, static_cast<UINT>(vb), sizeof(Vertex)};
                g.dynamicMesh.ibv = {base + vb, static_cast<UINT>(ib), DXGI_FORMAT_R32_UINT};
                g.dynamicMesh.count = static_cast<UINT>(mesh->indices.size());
                g.dynamicMesh.bounded = false;
                ++g_ringMeshes;
                return &g.dynamicMesh;
            }
            if (!e.gpu.meshId)
            {
                const UINT64 vertexBytes = e.vertices.size() * sizeof(Vertex), indexBytes = mesh->indices.size() * sizeof(uint32_t);
                e.gpu.meshId = gfx::device().createMesh(e.vertices.data(), static_cast<uint32_t>(vertexBytes), mesh->indices.data(),
                                                        static_cast<uint32_t>(indexBytes));
                if (!e.gpu.meshId)
                    return nullptr;
                const State::DeviceMesh &d = g.devMeshes[e.gpu.meshId];
                e.gpu.vbv = d.vbv;
                e.gpu.ibv = d.ibv;
                e.gpu.vbvV = d.vbvV;
                e.gpu.ibvV = d.ibvV;
                e.gpu.mesh = mesh;
                e.gpu.count = static_cast<UINT>(mesh->indices.size());
                boundsOf(*mesh, e.gpu);
                e.gpu.bounded = true;
            }
            return &e.gpu;
        }

        // The meshes and textures whose owners are gone: let go of once this frame's fence is passed.
        void prune()
        {
            for (auto it = g.textures.begin(); it != g.textures.end();)
                if (it->second.texture.expired())
                {
                    g_largeBytes -= it->second.largeBytes;
                    gfx::device().releaseTexture(it->second.id);
                    it = g.textures.erase(it);
                }
                else
                    ++it;
            for (auto it = g.meshes.begin(); it != g.meshes.end();)
                if (it->second.mesh.expired())
                {
                    gfx::device().releaseMesh(it->second.meshId);
                    it = g.meshes.erase(it);
                }
                else
                    ++it;
            for (auto it = layersoft::g_cache.begin(); it != layersoft::g_cache.end();)
                if (it->second.mesh.expired())
                {
                    if (it->second.gpu.meshId)
                        gfx::device().releaseMesh(it->second.gpu.meshId);
                    it = layersoft::g_cache.erase(it);
                }
                else
                    ++it;
        }

        // The GS's blend, ((A - B) * C >> 7) + D (ALPHA: A, B, D of Cs, Cd, 0; C of As, Ad, FIX),
        // as D3D's src * srcFactor (op) dst * dstFactor: the coefficients of Cs and Cd worked out
        // (0, 1, C, -C or 1 - C; 1 + C, which D3D cannot weigh, is taken as 1). FIX is the blend
        // factor. ABE off: no blending.
        struct BlendKey
        {
            D3D12_BLEND src = D3D12_BLEND_ONE, dst = D3D12_BLEND_ZERO;
            D3D12_BLEND_OP op = D3D12_BLEND_OP_ADD;
            bool enabled = false;
            // Cs * As or Cs * FIX (ALPHA's A = Cs, B = 0): the product made in the pixel shader
            // (1 As, 2 FIX), the blend's source factor one. The GS's factor goes to 2.0 (0xFF), the
            // GPU's source alpha and blend factor stop at 1.0 - the additive glows (the warp beams,
            // the engines' glows: ALPHA 0x48, vertex or texture alpha over 0x80) came out at half
            // their brightness or less. (The GS clamps Cs * As to 255 before adding Cd: the shader's
            // saturated product is the same.)
            int premultiply = 0;
        };

        // TEX1 (LCM bit 0, MXL 2-4, MMAG 5, MMIN 6-8, L 19-20, K 32-43: signed, 7.4 fixed point) as the
        // pixel shader's sampleGs takes it. TF_NATIVE_TEX1=0: the anisotropic filter over the whole mip
        // chain, as before. (N8 item 5) The GS picks the level from the distance (Q) alone, K set for the PS2's
        // 448 lines: on a taller picture a level's texels are drawn over more pixels, the texture soft at a
        // distance it was sharp at on the PS2. The distance-based levels (LCM 0) are shifted by g_texSharp toward
        // the finer ones (the sharp-textures setting): log2(lines / 448), 1.27 levels at 1080 lines - the texels a
        // pixel the PS2's. A level K alone (LCM 1) is the game's own choice and stays.
        float texSharpShift()
        {
            const float s = g_texSharp.load(std::memory_order_relaxed);
            if (s >= 0.0f)
                return s;
            return g.height > 448u ? std::log2(static_cast<float>(g.height) / 448.0f) : 0.0f;
        }

        void tex1Constants(uint64_t tex1, float *lod, float *lodf) { draw::tex1Constants(tex1, texSharpShift(), lod, lodf); } // (nr_draw.h)

        // (the GS's ALPHA as the blend: nr_draw.h, in neutral terms - here Direct3D 12's)
        D3D12_BLEND blendOf(gfx::Blend b)
        {
            switch (b)
            {
            case gfx::Blend::Zero: return D3D12_BLEND_ZERO;
            case gfx::Blend::One: return D3D12_BLEND_ONE;
            case gfx::Blend::SrcAlpha: return D3D12_BLEND_SRC_ALPHA;
            case gfx::Blend::InvSrcAlpha: return D3D12_BLEND_INV_SRC_ALPHA;
            case gfx::Blend::DestAlpha: return D3D12_BLEND_DEST_ALPHA;
            case gfx::Blend::InvDestAlpha: return D3D12_BLEND_INV_DEST_ALPHA;
            case gfx::Blend::Factor: return D3D12_BLEND_BLEND_FACTOR;
            case gfx::Blend::InvFactor: return D3D12_BLEND_INV_BLEND_FACTOR;
            }
            return D3D12_BLEND_ONE;
        }
        BlendKey blendOf(uint64_t alpha, bool abe)
        {
            const gfx::BlendState n = draw::blendOf(alpha, abe);
            BlendKey k;
            k.src = blendOf(n.src);
            k.dst = blendOf(n.dst);
            k.op = n.op == gfx::BlendOp::RevSubtract ? D3D12_BLEND_OP_REV_SUBTRACT : n.op == gfx::BlendOp::Subtract ? D3D12_BLEND_OP_SUBTRACT : D3D12_BLEND_OP_ADD;
            k.enabled = n.enabled;
            k.premultiply = n.premultiply;
            return k;
        }
        D3D12_COMPARISON_FUNC compareOf(gfx::Compare c)
        {
            return c == gfx::Compare::Never ? D3D12_COMPARISON_FUNC_NEVER : c == gfx::Compare::Always ? D3D12_COMPARISON_FUNC_ALWAYS
                   : c == gfx::Compare::GreaterEqual ? D3D12_COMPARISON_FUNC_GREATER_EQUAL : D3D12_COMPARISON_FUNC_GREATER;
        }

        uint64_t pipelineKey(const BlendKey &b, bool depthTest, D3D12_COMPARISON_FUNC func, bool depthWrite, uint32_t writeMask, bool picture,
                             bool clip, uint32_t variant = 0u, uint32_t aid = 0u)
        {
            return static_cast<uint64_t>(static_cast<uint32_t>(b.src) | static_cast<uint32_t>(b.dst) << 5 | static_cast<uint32_t>(b.op) << 10 |
                                         static_cast<uint32_t>(b.enabled) << 13 | static_cast<uint32_t>(depthTest) << 14 |
                                         static_cast<uint32_t>(func) << 15 | static_cast<uint32_t>(depthWrite) << 19 | writeMask << 20 |
                                         static_cast<uint32_t>(picture) << 24 | static_cast<uint32_t>(clip) << 25 | aid << 27) |
                   static_cast<uint64_t>(variant) << 32;
        }

        // A pipeline's description (pipelineFor, and the variants' thread): the pixel shader given.
        void pipelineDesc(D3D12_GRAPHICS_PIPELINE_STATE_DESC &pd, const BlendKey &b, bool depthTest, D3D12_COMPARISON_FUNC func, bool depthWrite,
                          uint32_t writeMask, bool picture, bool clip, D3D12_SHADER_BYTECODE ps)
        {
            static const D3D12_INPUT_ELEMENT_DESC elements[] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, x), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(Vertex, colour), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, u), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, nx), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"PSIZE", 0, DXGI_FORMAT_R32_FLOAT, 0, offsetof(Vertex, size), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"IROW", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
                {"IROW", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
                {"IROW", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
                {"IROW", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
                {"IPZERO", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 64, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
                {"IROWB", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 80, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
                {"IROWB", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 96, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
                {"IROWB", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 112, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
                {"IROWB", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 128, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
                {"IROWM", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 144, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
                {"IROWM", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 160, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
                {"IROWM", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 176, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
                {"IROWM", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 192, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
                {"POSB", 0, DXGI_FORMAT_R32G32B32_FLOAT, 2, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"COLORB", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 2, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"PSIZEB", 0, DXGI_FORMAT_R32_FLOAT, 2, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            };
            pd = D3D12_GRAPHICS_PIPELINE_STATE_DESC{};
            pd.pRootSignature = g.root;
            pd.VS = {g.vsCode->GetBufferPointer(), g.vsCode->GetBufferSize()};
            pd.PS = ps;
            if (clip)
                pd.GS = {g.gsCode->GetBufferPointer(), g.gsCode->GetBufferSize()};
            D3D12_RENDER_TARGET_BLEND_DESC &rt = pd.BlendState.RenderTarget[0];
            rt.BlendEnable = b.enabled;
            rt.SrcBlend = b.src;
            rt.DestBlend = b.dst;
            rt.BlendOp = b.op;
            rt.SrcBlendAlpha = D3D12_BLEND_ONE;
            rt.DestBlendAlpha = D3D12_BLEND_ZERO;
            rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            rt.RenderTargetWriteMask = static_cast<UINT8>(writeMask);
            pd.SampleMask = 0xFFFFFFFFu;
            pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // (the GS draws both sides; the game culls on the CPU and in VU1)
            // (TODO 99, a test: TF_NATIVE_DEPTHCLIP=0 - depth clamped instead of clipped: the GS clips nothing by its Z)
            static const bool depthClipOff = std::getenv("TF_NATIVE_DEPTHCLIP") && *std::getenv("TF_NATIVE_DEPTHCLIP") == '0';
            pd.RasterizerState.DepthClipEnable = depthClipOff ? FALSE : TRUE;
            pd.DepthStencilState.DepthEnable = depthTest || depthWrite;
            pd.DepthStencilState.DepthWriteMask = depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
            pd.DepthStencilState.DepthFunc = depthTest ? func : D3D12_COMPARISON_FUNC_ALWAYS;
            pd.InputLayout = {elements, static_cast<UINT>(sizeof(elements) / sizeof(elements[0]))};
            pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pd.NumRenderTargets = 1;
            pd.RTVFormats[0] = picture ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
            pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
            pd.SampleDesc.Count = 1;
        }

        // (TODO 164 stage 3) The pipelines kept across runs: a D3D12 pipeline library on disk (cache::dir()/pipelines.bin),
        // each pipeline by its key and its shaders' bytes; the keys this machine has used (pipelines.txt) made ahead at the
        // next start, on a thread of their own (warm). A library the driver refuses - another driver or graphics card,
        // or a damaged file - is made anew. TF_PIPELINE_LIBRARY=0: none.
        namespace plib
        {
            std::mutex g_mutex;
            ID3D12PipelineLibrary *g_lib = nullptr;
            std::vector<uint8_t> g_libBytes; // (the library reads its pipelines from here: kept while it lives)
            std::unordered_set<uint64_t> g_keys; // (every key made or loaded: pipelines.txt)
            bool g_dirty = false;
            std::chrono::steady_clock::time_point g_changedAt{};
            std::atomic<uint32_t> g_loaded{0}, g_made{0}, g_warmed{0};
            std::vector<std::pair<uint64_t, ID3D12PipelineState *>> g_warmDone; // (render thread takes them: takeWarmed)

            void open()
            {
                static const bool off = std::getenv("TF_PIPELINE_LIBRARY") && *std::getenv("TF_PIPELINE_LIBRARY") == '0';
                if (off || cache::dir().empty())
                    return;
                ID3D12Device1 *device1 = nullptr;
                if (FAILED(g.device->QueryInterface(IID_PPV_ARGS(&device1))) || !device1)
                    return;
                const std::filesystem::path file = cache::dir() / "pipelines.bin";
                const bool had = cache::readFile(file, g_libBytes);
                HRESULT hr = E_FAIL;
                if (had)
                    hr = device1->CreatePipelineLibrary(g_libBytes.data(), g_libBytes.size(), IID_PPV_ARGS(&g_lib));
                if (!had || FAILED(hr))
                {
                    if (had)
                        std::fprintf(stderr, "[native render] pipeline library: the one on disk refused (%s, 0x%08lx) - made anew\n",
                                     hr == D3D12_ERROR_DRIVER_VERSION_MISMATCH ? "another driver"
                                     : hr == D3D12_ERROR_ADAPTER_NOT_FOUND      ? "another graphics card"
                                                                                : "damaged or old",
                                     static_cast<unsigned long>(hr));
                    g_libBytes.clear();
                    g_lib = nullptr;
                    if (FAILED(device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&g_lib))))
                        g_lib = nullptr;
                }
                device1->Release();
                if (!g_lib)
                    return;
                // (the keys used before: made ahead by warm)
                std::vector<uint8_t> text;
                if (cache::readFile(cache::dir() / "pipelines.txt", text))
                {
                    std::string s(text.begin(), text.end());
                    size_t at = 0;
                    while (at < s.size())
                    {
                        const size_t end = s.find('\n', at);
                        const std::string line = s.substr(at, end == std::string::npos ? std::string::npos : end - at);
                        if (!line.empty())
                            g_keys.insert(std::strtoull(line.c_str(), nullptr, 16));
                        if (end == std::string::npos)
                            break;
                        at = end + 1;
                    }
                }
                std::fprintf(stderr, "[native render] pipeline library: %s, %zu pipelines known (%s)\n", had && g_libBytes.size() ? "loaded" : "new",
                             g_keys.size(), cache::dir().string().c_str());
            }

            // The pipeline for a key: from the library, else made (and stored).
            ID3D12PipelineState *make(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &pd, uint64_t key)
            {
                ID3D12PipelineState *pso = nullptr;
                wchar_t name[64] = {};
                if (g_lib)
                {
                    uint64_t h = 1469598103934665603ull;
                    h = cache::fnv(h, pd.VS.pShaderBytecode, pd.VS.BytecodeLength);
                    h = cache::fnv(h, pd.PS.pShaderBytecode, pd.PS.BytecodeLength);
                    if (pd.GS.pShaderBytecode)
                        h = cache::fnv(h, pd.GS.pShaderBytecode, pd.GS.BytecodeLength);
                    std::swprintf(name, 64, L"%016llx-%016llx", static_cast<unsigned long long>(key), static_cast<unsigned long long>(h));
                    std::lock_guard<std::mutex> lock(g_mutex);
                    if (SUCCEEDED(g_lib->LoadGraphicsPipeline(name, &pd, IID_PPV_ARGS(&pso))) && pso)
                    {
                        ++g_loaded;
                        if (g_keys.insert(key).second)
                            g_dirty = true, g_changedAt = std::chrono::steady_clock::now();
                        return pso;
                    }
                    pso = nullptr;
                }
                if (FAILED(g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso))))
                    return nullptr;
                ++g_made;
                if (g_lib)
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    if (SUCCEEDED(g_lib->StorePipeline(name, pso)))
                    {
                        g_keys.insert(key);
                        g_dirty = true;
                        g_changedAt = std::chrono::steady_clock::now();
                    }
                }
                return pso;
            }

            // (render thread, the frame's end) The library and the keys written once nothing new came for 2 s: serialized
            // here (a copy), written on a thread of its own.
            void maybeSave()
            {
                std::vector<uint8_t> bytes;
                std::string keys;
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    if (!g_lib || !g_dirty || std::chrono::steady_clock::now() - g_changedAt < std::chrono::seconds(2))
                        return;
                    g_dirty = false;
                    const SIZE_T size = g_lib->GetSerializedSize();
                    bytes.resize(size);
                    if (!size || FAILED(g_lib->Serialize(bytes.data(), size)))
                        return;
                    char line[24];
                    for (const uint64_t k : g_keys)
                    {
                        std::snprintf(line, sizeof(line), "%016llx\n", static_cast<unsigned long long>(k));
                        keys += line;
                    }
                }
                std::thread([b = std::move(bytes), k = std::move(keys)] {
                    cache::writeFile(cache::dir() / "pipelines.bin", b.data(), b.size());
                    cache::writeFile(cache::dir() / "pipelines.txt", k.data(), k.size());
                }).detach();
            }
        }

        // The pixel shader's variants (TODO 94; the shader's PSV_ switches): compiled, and their pipelines made, on a
        // thread of their own as draws ask for them - until one is there the draw takes the whole shader, which draws
        // the same pixels. (D3DCompile and the device are free-threaded; the pipelines map is the render thread's,
        // the finished ones handed over at the frame's start.)
        namespace psv
        {
            struct Job
            {
                uint64_t key = 0;
                BlendKey b;
                bool depthTest = false, depthWrite = false, picture = false, clip = false;
                D3D12_COMPARISON_FUNC func = D3D12_COMPARISON_FUNC_ALWAYS;
                uint32_t writeMask = 0u, variant = 0u;
            };
            std::mutex mutex;
            std::condition_variable wake;
            std::deque<Job> jobs;
            std::vector<std::pair<uint64_t, ID3D12PipelineState *>> done;
            std::unordered_set<uint64_t> asked; // (render thread)
            bool started = false;
            std::atomic<uint32_t> made{0}, failed{0};

            // The variant's switches: bit 0 set (a variant), bits 1-2 PSV_TEX (0 / 1), 3 PSV_LOD, 4 PSV_CLAMP, 5 PSV_ATEST,
            // 6 PSV_DATE, 7 PSV_CULL.
            ID3DBlob *compileVariant(uint32_t v)
            {
                static const char *names[] = {"PSV_TEX", "PSV_LOD", "PSV_CLAMP", "PSV_ATEST", "PSV_DATE", "PSV_CULL"};
                const char *values[6] = {(v >> 1) & 3u ? "1" : "0", (v >> 3) & 1u ? "1" : "0", (v >> 4) & 1u ? "1" : "0",
                                         (v >> 5) & 1u ? "1" : "0", (v >> 6) & 1u ? "1" : "0", (v >> 7) & 1u ? "1" : "0"};
                D3D_SHADER_MACRO defines[7] = {};
                for (int k = 0; k < 6; ++k)
                    defines[k] = {names[k], values[k]};
                defines[6] = {nullptr, nullptr};
                return compile("ps", "ps_5_1", kShader, sizeof(kShader) - 1, defines);
            }

            void worker()
            {
                std::unordered_map<uint32_t, ID3DBlob *> blobs;
                for (;;)
                {
                    Job j;
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        wake.wait(lock, [] { return !jobs.empty(); });
                        j = jobs.front();
                        jobs.pop_front();
                    }
                    auto it = blobs.find(j.variant);
                    if (it == blobs.end())
                        it = blobs.emplace(j.variant, compileVariant(j.variant)).first;
                    ID3D12PipelineState *pso = nullptr;
                    if (it->second)
                    {
                        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
                        pipelineDesc(pd, j.b, j.depthTest, j.func, j.depthWrite, j.writeMask, j.picture, j.clip,
                                     D3D12_SHADER_BYTECODE{it->second->GetBufferPointer(), it->second->GetBufferSize()});
                        pso = plib::make(pd, j.key);
                        nr::g_pipelinesMadeAll.fetch_add(1u, std::memory_order_relaxed);
                    }
                    ++(pso ? made : failed);
                    std::lock_guard<std::mutex> lock(mutex);
                    done.emplace_back(j.key, pso);
                }
            }

            // The variant a draw's constants ask for (0: the whole shader - a read of a render target, a debug mode).
            uint32_t psVariantOf(const Constants &c)
            {
                const int mx = static_cast<int>(c.mode[0]);
                if (mx == 10 || mx == 11 || mx == 12 || mx == 16 || c.down[1] > 0.5f)
                    return 0u;
                uint32_t tex = 0u, lodOn = 0u, clampOn = 0u;
                if (c.flags[0] > 0.5f)
                {
                    if (c.down[0] > 0.5f || c.texScale[3] > 0.5f)
                        return 0u;
                    tex = 1u;
                    lodOn = c.lod[0] > 0.5f ? 1u : 0u;
                    clampOn = c.wrapS[0] > 0.5f || c.wrapT[0] > 0.5f ? 1u : 0u;
                }
                else if (c.texScale[3] > 0.5f)
                    return 0u;
                // (The alpha test discards where it fails and AFAIL keeps nothing (0), or where it passes in AFAIL
                // RGB_ONLY's second pass (4); an ALWAYS test in the first case never does.)
                const bool atest = c.test[0] > 0.5f && ((c.test[3] == 0.0f && c.test[1] != 1.0f) || c.test[3] == 4.0f);
                const bool date = c.dest[0] > 0.5f;
                const bool cull = mx == 9 || mx == 13;
                return 1u | tex << 1 | lodOn << 3 | clampOn << 4 | (atest ? 1u : 0u) << 5 | (date ? 1u : 0u) << 6 | (cull ? 1u : 0u) << 7;
            }

            // (Render thread, the frame's start: the finished pipelines into the map - a failed one as the whole shader's.)
            void collect()
            {
                std::vector<std::pair<uint64_t, ID3D12PipelineState *>> got;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    got.swap(done);
                }
                for (auto &[key, pso] : got)
                    g.pipelines[key] = pso;
                g.pipelinesChanged += got.size();
            }
        }

        namespace plib
        {
            // (TODO 164 stage 3) The keys this machine used before, made ahead from the library on a thread of their own at
            // the start: base pipelines with the whole shader, variants compiled (from the shader cache) - handed to the
            // render thread at the frame's start (takeWarmed), where a key it made itself meanwhile keeps its own.
            void warm()
            {
                if (!g_lib)
                    return;
                std::vector<uint64_t> keys;
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    keys.assign(g_keys.begin(), g_keys.end());
                }
                std::thread([keys] {
                    const auto start = std::chrono::steady_clock::now();
                    std::unordered_map<uint32_t, ID3DBlob *> blobs;
                    size_t made = 0;
                    for (const uint64_t key : keys)
                    {
                        const uint32_t low = static_cast<uint32_t>(key), variant = static_cast<uint32_t>(key >> 32);
                        BlendKey b;
                        b.src = static_cast<D3D12_BLEND>(low & 31u);
                        b.dst = static_cast<D3D12_BLEND>((low >> 5) & 31u);
                        b.op = static_cast<D3D12_BLEND_OP>((low >> 10) & 7u);
                        b.enabled = ((low >> 13) & 1u) != 0u;
                        const bool depthTest = ((low >> 14) & 1u) != 0u, depthWrite = ((low >> 19) & 1u) != 0u;
                        const auto func = static_cast<D3D12_COMPARISON_FUNC>((low >> 15) & 15u);
                        const uint32_t writeMask = (low >> 20) & 15u, aid = (low >> 27) & 31u;
                        const bool picture = ((low >> 24) & 1u) != 0u, clip = ((low >> 25) & 1u) != 0u;
                        if (clip && !g.gsCode)
                            continue;
                        ID3DBlob *ps = nullptr;
                        if (variant)
                        {
                            auto it = blobs.find(variant);
                            if (it == blobs.end())
                                it = blobs.emplace(variant, psv::compileVariant(variant)).first;
                            ps = it->second;
                        }
                        else
                            ps = aid == 1u ? g.psTiny : aid == 2u ? g.psInterp : aid == 0u ? g.psCode : nullptr;
                        if (!ps)
                            continue;
                        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
                        pipelineDesc(pd, b, depthTest, func, depthWrite, writeMask, picture, clip, D3D12_SHADER_BYTECODE{ps->GetBufferPointer(), ps->GetBufferSize()});
                        if (ID3D12PipelineState *pso = make(pd, key))
                        {
                            ++made;
                            std::lock_guard<std::mutex> lock(g_mutex);
                            g_warmDone.emplace_back(key, pso);
                        }
                    }
                    g_warmed.store(static_cast<uint32_t>(made));
                    std::fprintf(stderr, "[native render] pipeline library: %zu of %zu pipelines made ahead in %.0f ms (%u from the library, %u made; shaders: %u from the cache, %u compiled)\n",
                                 made, keys.size(), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(),
                                 g_loaded.load(), g_made.load(), cache::g_shaderHits.load(), cache::g_shaderMade.load());
                }).detach();
            }

            void takeWarmed()
            {
                std::vector<std::pair<uint64_t, ID3D12PipelineState *>> got;
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    got.swap(g_warmDone);
                }
                g.pipelinesChanged += got.size();
                for (auto &[key, pso] : got)
                {
                    auto [it, fresh] = g.pipelines.try_emplace(key, pso);
                    if (!fresh)
                    {
                        if (!it->second)
                            it->second = pso; // (a variant that had failed here: the warmed one)
                        else
                            pso->Release();
                    }
                }
            }
        }

        // FBMSK as the colour channels written: a channel whose 8 bits are all masked is left alone.
        // (FBMSK's channels: nr_draw.h - the neutral bits are Direct3D 12's)
        static_assert(gfx::kWriteR == D3D12_COLOR_WRITE_ENABLE_RED && gfx::kWriteG == D3D12_COLOR_WRITE_ENABLE_GREEN &&
                          gfx::kWriteB == D3D12_COLOR_WRITE_ENABLE_BLUE && gfx::kWriteA == D3D12_COLOR_WRITE_ENABLE_ALPHA,
                      "the write mask bits");
        uint32_t writeMaskOf(uint32_t fbmsk) { return draw::writeMaskOf(fbmsk); }

        // (picture: drawn into the picture itself, RGBA as the window's GL takes it - the GS buffers'
        // targets are BGRA. A pipeline must name the format it draws into: one made for BGRA drawing
        // into the RGBA picture is the debug layer's error 613, undefined on the GPU.)
        ID3D12PipelineState *pipelineFor(const BlendKey &b, bool depthTest, D3D12_COMPARISON_FUNC func, bool depthWrite,
                                         uint32_t writeMask = D3D12_COLOR_WRITE_ENABLE_ALL, bool picture = false, bool clip = false,
                                         uint32_t variant = 0u, uint32_t aid = 0u)
        {
            clip = clip && g.gsCode; // (the clipping geometry shader: the core's clipping, gsClip)
            if ((aid == 1u && !g.psTiny) || (aid == 2u && !g.psInterp))
                aid = 0u;
            if (aid)
                variant = 0u;
            const uint64_t key = pipelineKey(b, depthTest, func, depthWrite, writeMask, picture, clip, variant, aid);
            auto it = g.pipelines.find(key);
            if (it != g.pipelines.end())
                return it->second ? it->second : pipelineFor(b, depthTest, func, depthWrite, writeMask, picture, clip, 0u, aid);
            if (variant)
            {
                // (A variant not made yet: asked of the variants' thread once; the whole shader meanwhile.)
                if (psv::asked.insert(key).second)
                {
                    if (!psv::started)
                    {
                        psv::started = true;
                        std::thread(psv::worker).detach();
                    }
                    psv::Job j;
                    j.key = key, j.b = b, j.depthTest = depthTest, j.depthWrite = depthWrite, j.picture = picture, j.clip = clip, j.func = func;
                    j.writeMask = writeMask, j.variant = variant;
                    {
                        std::lock_guard<std::mutex> lock(psv::mutex);
                        psv::jobs.push_back(j);
                    }
                    psv::wake.notify_one();
                }
                return pipelineFor(b, depthTest, func, depthWrite, writeMask, picture, clip, 0u, aid);
            }
            D3D12_SHADER_BYTECODE ps{g.psCode->GetBufferPointer(), g.psCode->GetBufferSize()};
            if (aid == 1u)
                ps = {g.psTiny->GetBufferPointer(), g.psTiny->GetBufferSize()};
            else if (aid == 2u)
                ps = {g.psInterp->GetBufferPointer(), g.psInterp->GetBufferSize()};
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
            pipelineDesc(pd, b, depthTest, func, depthWrite, writeMask, picture, clip, ps);
            ID3D12PipelineState *pso = plib::make(pd, key); // (TODO 164: from the pipeline library when it has it)
            nr::g_pipelinesMadeAll.fetch_add(1u, std::memory_order_relaxed);
            g.pipelines[key] = pso;
            ++g.pipelinesChanged;
            return pso;
        }

        // (TODO 99, a check: the frame each shader-visible descriptor slot was last bound in - a view written into a slot
        // that this frame or the one in flight has bound is read by the GPU as the new view, not the one the draw was
        // recorded with: the descriptors are read when the list runs. Counted and the first few logged.)
        uint64_t g_slotBound[kSlots] = {};
        // (TODO 99, a check: TF_NATIVE_PSTATS=1 - on a saved frame, each pass's pipeline statistics (what the GPU's stages took
        // in and let through: triangles into and out of the clipper, pixels shaded) into runs\pstats_<tick>.txt)
        constexpr UINT kPStats = 8192u;
        ID3D12QueryHeap *g_pstatHeap = nullptr;
        ID3D12Resource *g_pstatBack = nullptr;
        struct PStatNote
        {
            uint32_t draw = 0, pass = 0, source = 0;
        };
        std::vector<PStatNote> g_pstatNotes;
        uint64_t g_srvRewrites = 0;
        void srvWritten(UINT slot, const char *what)
        {
            if (slot == 0u || slot >= kSlots || !g_slotBound[slot] || g_slotBound[slot] + State::kFrames <= g.frameIndex)
                return;
            ++g_srvRewrites;
            static int told = 20;
            if (told > 0)
            {
                --told;
                std::fprintf(stderr, "[native render] TODO 99 check: %s view written into slot %u, bound %s\n", what, slot,
                             g_slotBound[slot] == g.frameIndex ? "earlier this frame" : "by the frame before (maybe in flight)");
            }
        }

        D3D12_GPU_DESCRIPTOR_HANDLE slotHandle(UINT slot)
        {
            if (slot < kSlots)
                g_slotBound[slot] = g.frameIndex;
            D3D12_GPU_DESCRIPTOR_HANDLE h = g.srvHeap->GetGPUDescriptorHandleForHeapStart();
            h.ptr += static_cast<UINT64>(slot) * g.srvSize;
            return h;
        }

        // A texture's pixels into a default-heap texture: the copy recorded in the frame's list, the
        // staging buffer kept until the GPU has run it.
        // The game textures' video memory (TODO 76/77): placed resources in 64 MB heaps of our own, the ranges
        // handed out first-fit and given back when the frame that let a texture go has finished (finishSlot). Made
        // as committed resources each (and a committed staging buffer each), an HQ screen change's ~40 new
        // textures cost 1-2 ms of driver time apiece - 35-90 ms frames on the test PC, the more so with the
        // video memory over its budget. TF_NATIVE_TEXPOOL=0: committed, as before. (Render thread only.)
        namespace pool
        {
            constexpr UINT64 kHeap = 64ull << 20, kLargest = 16ull << 20;
            struct Heap
            {
                ID3D12Heap *heap = nullptr;
                std::map<UINT64, UINT64> free; // offset -> size
                UINT64 used = 0;
            };
            std::vector<Heap> heaps;
            struct Range
            {
                size_t heap = 0;
                UINT64 offset = 0, size = 0;
            };
            std::unordered_map<ID3D12Resource *, Range> placed;
            uint64_t made = 0, fallbacks = 0, sparesUsed = 0;
            // The next heap made ahead on a thread of its own once the pool runs low (a CreateHeap on the render
            // thread took 28 ms on the test PC with its video memory over budget).
            std::atomic<ID3D12Heap *> spare{nullptr};
            std::atomic<bool> sparePending{false};

            D3D12_HEAP_DESC heapDesc()
            {
                D3D12_HEAP_DESC hd{};
                hd.SizeInBytes = kHeap;
                hd.Properties = heap(D3D12_HEAP_TYPE_DEFAULT);
                hd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
                hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
                return hd;
            }

            void askSpare()
            {
                if (spare.load() || sparePending.exchange(true))
                    return;
                std::thread([] {
                    const D3D12_HEAP_DESC hd = heapDesc();
                    ID3D12Heap *h = nullptr;
                    if (SUCCEEDED(g.device->CreateHeap(&hd, IID_PPV_ARGS(&h))))
                        spare.store(h);
                    sparePending.store(false);
                }).detach();
            }

            bool enabled()
            {
                static const bool on = !(std::getenv("TF_NATIVE_TEXPOOL") && *std::getenv("TF_NATIVE_TEXPOOL") == '0');
                return on;
            }

            // A range of `size` at `align` in a heap (a new heap made when none has room).
            bool take(UINT64 size, UINT64 align, Range &out)
            {
                for (int pass = 0; pass < 2; ++pass)
                {
                    for (size_t h = 0; h < heaps.size(); ++h)
                        for (auto it = heaps[h].free.begin(); it != heaps[h].free.end(); ++it)
                        {
                            const UINT64 start = (it->first + align - 1u) & ~(align - 1u), end = it->first + it->second;
                            if (start + size > end)
                                continue;
                            const UINT64 freeStart = it->first;
                            heaps[h].free.erase(it);
                            if (start > freeStart)
                                heaps[h].free[freeStart] = start - freeStart;
                            if (start + size < end)
                                heaps[h].free[start + size] = end - (start + size);
                            heaps[h].used += size;
                            out = Range{h, start, size};
                            return true;
                        }
                    if (pass == 1)
                        break;
                    Heap fresh;
                    if ((fresh.heap = spare.exchange(nullptr)) != nullptr)
                        ++sparesUsed;
                    else
                    {
                        const D3D12_HEAP_DESC hd = heapDesc();
                        if (FAILED(g.device->CreateHeap(&hd, IID_PPV_ARGS(&fresh.heap))))
                            return false;
                    }
                    fresh.free[0] = kHeap;
                    heaps.push_back(std::move(fresh));
                }
                return false;
            }

            void give(const Range &r)
            {
                Heap &h = heaps[r.heap];
                h.used -= r.size;
                auto it = h.free.emplace(r.offset, r.size).first;
                if (auto next = std::next(it); next != h.free.end() && it->first + it->second == next->first)
                {
                    it->second += next->second;
                    h.free.erase(next);
                }
                if (it != h.free.begin())
                    if (auto prev = std::prev(it); prev->first + prev->second == it->first)
                    {
                        prev->second += it->second;
                        h.free.erase(it);
                    }
            }

            // A texture placed in a heap (COPY_DEST); null: not pooled (too large, the pool off or full).
            ID3D12Resource *place(D3D12_RESOURCE_DESC td)
            {
                if (!enabled())
                    return nullptr;
                td.Alignment = D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;
                D3D12_RESOURCE_ALLOCATION_INFO info = g.device->GetResourceAllocationInfo(0, 1, &td);
                if (info.Alignment != D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT)
                {
                    td.Alignment = 0;
                    info = g.device->GetResourceAllocationInfo(0, 1, &td);
                }
                if (info.SizeInBytes == UINT64_MAX || info.SizeInBytes > kLargest)
                    return nullptr;
                Range r;
                if (!take(info.SizeInBytes, info.Alignment, r))
                {
                    ++fallbacks;
                    return nullptr;
                }
                ID3D12Resource *resource = nullptr;
                if (FAILED(g.device->CreatePlacedResource(heaps[r.heap].heap, r.offset, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                          IID_PPV_ARGS(&resource))))
                {
                    give(r);
                    ++fallbacks;
                    return nullptr;
                }
                placed.emplace(resource, r);
                ++made;
                // (Under 24 MB free in all: the next heap asked for now.)
                UINT64 room = 0;
                for (const Heap &h : heaps)
                    room += kHeap - h.used;
                if (room < (24ull << 20))
                    askSpare();
                return resource;
            }

            // A resource let go of once its frame is done: its range back to the heap (before its Release -
            // nothing reads it any more; a texture placed over it is written whole by its copies first).
            void forget(ID3D12Resource *resource)
            {
                if (auto it = placed.find(resource); it != placed.end())
                {
                    give(it->second);
                    placed.erase(it);
                }
            }

            UINT64 usedBytes()
            {
                UINT64 n = 0;
                for (const Heap &h : heaps)
                    n += h.used;
                return n;
            }
        }

        // The game textures' staging (TODO 76/77): a ring in one upload buffer, a part a frame slot (the slot's
        // fence passed before it is written again), in place of a committed upload buffer per texture. A texture
        // that does not fit in what is left of the frame's part takes a buffer of its own, as before.
        constexpr UINT64 kTexRing = 24ull << 20;
        ID3D12Resource *g_texRing = nullptr;
        uint8_t *g_texRingMapped = nullptr;
        UINT64 g_texRingAt = 0; // (this frame's next free byte, from the slot's part's start)
        uint64_t g_texRingUses = 0, g_texRingOverflows = 0;

        // A mip level from the one above: each texel the rounded mean of its 2x2 (per channel, (sum + 2) / 4; an odd
        // edge's last row or column repeated) - two channels at a time in 16-bit lanes (TODO 76: the per-channel loop
        // was most of a new game texture's cost once its video memory came from the pool).
        void halveLevel(const uint32_t *src, UINT pw, UINT ph, uint32_t *dst)
        {
            const UINT w = std::max(1u, pw / 2u), h = std::max(1u, ph / 2u);
            constexpr uint32_t m = 0x00FF00FFu, r = 0x00020002u;
            for (UINT y = 0; y < h; ++y)
            {
                const uint32_t *r0 = src + static_cast<size_t>(std::min(ph - 1u, 2u * y)) * pw;
                const uint32_t *r1 = src + static_cast<size_t>(std::min(ph - 1u, 2u * y + 1u)) * pw;
                uint32_t *out = dst + static_cast<size_t>(y) * w;
                for (UINT x = 0; x < w; ++x)
                {
                    const UINT x0 = std::min(pw - 1u, 2u * x), x1 = std::min(pw - 1u, 2u * x + 1u);
                    const uint32_t a = r0[x0], b = r0[x1], c = r1[x0], d = r1[x1];
                    const uint32_t lo = (a & m) + (b & m) + (c & m) + (d & m) + r;
                    const uint32_t hi = ((a >> 8) & m) + ((b >> 8) & m) + ((c >> 8) & m) + ((d >> 8) & m) + r;
                    out[x] = ((lo >> 2) & m) | (((hi >> 2) & m) << 8);
                }
            }
        }

        // (makeTexture's parts, ms, summed: the mip chain built, the default-heap texture made, the staging
        // buffer made and mapped, the texels copied into it, the copies recorded with the view - the slow
        // frame line's breakdown, TODO 76.)
        double g_mkMs[5] = {};
        uint64_t g_mkTexels = 0;
        // The mip chain (the frame builder's), each level the one before box-filtered (2x2, per channel) - or as made
        // ahead (the large images: off the game's thread). (The levels read in place: a 4K image is not copied on the
        // game's thread; `computed` holds the ones made here.)
        void mipChain(UINT width, UINT height, const uint32_t *pixels, const std::vector<std::vector<uint32_t>> *made,
                      std::vector<const uint32_t *> &mips, std::vector<std::vector<uint32_t>> &computed)
        {
            const auto t0 = std::chrono::steady_clock::now();
            mips.assign(1, pixels);
            computed.clear();
            computed.reserve(16);
            std::vector<std::pair<UINT, UINT>> sizes(1, {width, height});
            if (made && !made->empty())
                for (const auto &level : *made)
                {
                    const UINT w = std::max(1u, sizes.back().first / 2u), h = std::max(1u, sizes.back().second / 2u);
                    if (level.size() != static_cast<size_t>(w) * h)
                        break;
                    mips.push_back(level.data());
                    sizes.push_back({w, h});
                }
            while (sizes.back().first > 1u || sizes.back().second > 1u)
            {
                const UINT pw = sizes.back().first, ph = sizes.back().second;
                const UINT w = std::max(1u, pw / 2u), h = std::max(1u, ph / 2u);
                std::vector<uint32_t> dst(static_cast<size_t>(w) * h);
                halveLevel(mips.back(), pw, ph, dst.data());
                computed.push_back(std::move(dst));
                mips.push_back(computed.back().data());
                sizes.push_back({w, h});
            }
            g_mkMs[0] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        }

        // (The device's) An RGBA8 texture from its levels (each half the one before), copied on this frame's list through
        // the staging ring, with its view at `slot`.
        ID3D12Resource *makeTextureLevels(UINT width, UINT height, const std::vector<const uint32_t *> &mips, UINT slot)
        {
            auto mkT = std::chrono::steady_clock::now();
            auto mkLap = [&mkT](int part) {
                const auto now = std::chrono::steady_clock::now();
                g_mkMs[part] += std::chrono::duration<double, std::milli>(now - mkT).count();
                mkT = now;
            };
            g_mkTexels += static_cast<uint64_t>(width) * height;
            std::vector<std::pair<UINT, UINT>> sizes(1, {width, height});
            while (sizes.size() < mips.size())
                sizes.push_back({std::max(1u, sizes.back().first / 2u), std::max(1u, sizes.back().second / 2u)});
            const UINT levels = static_cast<UINT>(mips.size());
            D3D12_RESOURCE_DESC td{};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = width;
            td.Height = height;
            td.DepthOrArraySize = 1;
            td.MipLevels = static_cast<UINT16>(levels);
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            const D3D12_HEAP_PROPERTIES gpu = heap(D3D12_HEAP_TYPE_DEFAULT);
            ID3D12Resource *resource = pool::place(td);
            if (!resource && FAILED(g.device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                      IID_PPV_ARGS(&resource))))
                return nullptr;
            mkLap(1);
            std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fps(levels);
            UINT64 total = 0;
            g.device->GetCopyableFootprints(&td, 0, levels, 0, fps.data(), nullptr, nullptr, &total);
            // (The staging: the frame's part of the ring if it fits, else a buffer of its own.)
            if (pool::enabled() && !g_texRing)
            {
                const D3D12_HEAP_PROPERTIES upRing = heap(D3D12_HEAP_TYPE_UPLOAD);
                const D3D12_RESOURCE_DESC rd = bufferDesc(kTexRing * State::kFrames);
                const D3D12_RANGE none{0, 0};
                void *mapped = nullptr;
                if (SUCCEEDED(g.device->CreateCommittedResource(&upRing, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                                IID_PPV_ARGS(&g_texRing))) &&
                    SUCCEEDED(g_texRing->Map(0, &none, &mapped)))
                    g_texRingMapped = static_cast<uint8_t *>(mapped);
                else
                    release(g_texRing);
            }
            ID3D12Resource *staging = nullptr;
            void *p = nullptr;
            const UINT64 ringStart = (g_texRingAt + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1u) & ~static_cast<UINT64>(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1u);
            if (g_texRingMapped && ringStart + total <= kTexRing)
            {
                const UINT64 base = static_cast<UINT64>(g.slot) * kTexRing + ringStart;
                for (auto &f : fps)
                    f.Offset += base;
                p = g_texRingMapped; // (the footprints' offsets are from the buffer's start)
                g_texRingAt = ringStart + total;
                ++g_texRingUses;
            }
            else
            {
                if (g_texRingMapped)
                    ++g_texRingOverflows;
                const D3D12_HEAP_PROPERTIES up = heap(D3D12_HEAP_TYPE_UPLOAD);
                const D3D12_RESOURCE_DESC bd = bufferDesc(total);
                const D3D12_RANGE none{0, 0};
                if (FAILED(g.device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                             IID_PPV_ARGS(&staging))) ||
                    FAILED(staging->Map(0, &none, &p)))
                {
                    release(staging);
                    pool::forget(resource);
                    release(resource);
                    return nullptr;
                }
            }
            mkLap(2);
            for (UINT m = 0; m < levels; ++m)
            {
                for (UINT y = 0; y < sizes[m].second; ++y)
                    std::memcpy(static_cast<uint8_t *>(p) + fps[m].Offset + static_cast<size_t>(y) * fps[m].Footprint.RowPitch,
                                mips[m] + static_cast<size_t>(y) * sizes[m].first, sizes[m].first * 4u);
            }
            if (staging)
                staging->Unmap(0, nullptr);
            mkLap(3);
            for (UINT m = 0; m < levels; ++m)
            {
                D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
                dst.pResource = resource;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.SubresourceIndex = m;
                src.pResource = staging ? staging : g_texRing;
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint = fps[m];
                g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            }
            transition(resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            if (staging)
                g.uploads.push_back(staging);
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = td.Format;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = levels;
            D3D12_CPU_DESCRIPTOR_HANDLE h = g.srvHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += static_cast<SIZE_T>(slot) * g.srvSize;
            srvWritten(slot, "a texture's");
            g.device->CreateShaderResourceView(resource, &sd, h);
            mkLap(4);
            return resource;
        }

        // The chain made and the texture with it (the white one, slot 0).
        ID3D12Resource *makeTexture(UINT width, UINT height, const uint32_t *pixels, UINT slot,
                                    const std::vector<std::vector<uint32_t>> *made = nullptr)
        {
            std::vector<const uint32_t *> mips;
            std::vector<std::vector<uint32_t>> computed;
            mipChain(width, height, pixels, made, mips, computed);
            return makeTextureLevels(width, height, mips, slot);
        }

        uint64_t g_made = 0, g_madeTexels = 0, g_refused = 0;
        // The new GPU textures a frame may make (TF_NATIVE_TEXCAP=<textures>[,<Mtexels>], default 128
        // and 96): past them a texture waits for a later frame - its draws left out meanwhile - so that
        // nothing (a key changing every frame, a level's load) can flood the GPU with uploads in one
        // frame. Reset by render() each frame.
        uint64_t g_frameMade = 0, g_frameTexels = 0, g_frameDeferred = 0, g_deferredSince = 0;
        // The game textures' creation budget a frame (TODO 76/77, TF_NATIVE_TEXBUDGET=<ms>, default 3; 0 = none): an
        // HQ screen change drew ~50 new game textures in one frame, each made here (60-75 ms frames, the lag and
        // black frames you saw). Past the budget a game texture goes to the uploader (its copy queue) and its draws
        // that frame take a stand-in - the white slot with the texture's average colour in the pass's constants -
        // until it is on the GPU (a frame or a few later): nothing left out (TODO 71), nothing white (TODO 61).
        double g_frameMakeMs = 0.0;
        uint64_t g_frameStandIns = 0, g_standInDraws = 0, g_standInFrames = 0, g_standInTextures = 0;
        GpuTexture g_standIn;           // (slot 0: the white texture - its colour in g_standInColour)
        float g_standInColour[4] = {};
        std::unordered_map<uint64_t, std::array<float, 4>> g_averages; // (a texture's average colour, by id)
        const double kTexBudgetMs = [] {
            const char *e = std::getenv("TF_NATIVE_TEXBUDGET");
            return e && *e ? std::atof(e) : 3.0;
        }();
        const std::pair<uint64_t, uint64_t> kTexCap = [] {
            const char *e = std::getenv("TF_NATIVE_TEXCAP");
            char *end = nullptr;
            const uint64_t n = e ? std::strtoull(e, &end, 10) : 128u;
            const uint64_t m = e && end && *end == ',' ? std::strtoull(end + 1, nullptr, 10) : 96u;
            return std::pair<uint64_t, uint64_t>{std::max<uint64_t>(n, 1u), std::max<uint64_t>(m, 1u) << 20};
        }();
        std::atomic<bool> g_deviceLost{false}; // the device removed (a GPU hang, TDR): nothing more is drawn
        std::atomic<int> g_windowLines{0};     // the window's height (setWindowLines); 0 = not known yet

        // The large images' uploads, off the render thread (2026-10-28, TODO 59). A level's ~1,000
        // upscaled 2K images arrive from the loader in its first seconds; made on the render thread
        // under the frame cap (128 textures / 96 Mtexels), each a default-heap texture, a staging
        // buffer and ~21 MB of texels copied into it, frames took 100-140 ms for ~15 s and the game
        // and the sound fell behind. Now a thread of its own makes them: the texture, the staging
        // buffer filled, the copy run on a COPY queue (the texture made in COMMON: promoted to
        // COPY_DEST there, decayed back once done, promoted to PIXEL_SHADER_RESOURCE by the frames
        // that sample it), its fence waited for on that thread. The render thread only gives a done
        // one its descriptor slot (takeUploaded, once a frame); until then the game's own texture
        // (Texture::fallback) is drawn. TF_NATIVE_UPLOADER=0: as before (on the render thread, capped).
        namespace up
        {
            struct Job
            {
                std::weak_ptr<const Texture> texture;
                uint64_t id = 0;     // (P3 S3: the device's texture id)
                uint32_t generation = 0;
                bool urgent = false; // (a game texture, not a large image)
            };
            struct Done
            {
                uint64_t id = 0;
                uint32_t generation = 0;
                std::weak_ptr<const Texture> texture;
                ID3D12Resource *resource = nullptr; // (null: not made - gone before its turn, or failed)
                UINT levels = 0;
                bool urgent = false;
            };
            std::mutex mutex;
            std::condition_variable cv;
            std::deque<Job> jobs;
            std::deque<Job> urgent; // (the game's own textures: taken before any large image)
            std::vector<Done> done;
            std::atomic<int> state{0}; // 0 not started, 1 running, -1 off (failed or the device lost)
            std::atomic<uint64_t> made{0}, texels{0}, bytes{0}, failed{0};
            std::atomic<double> busyMs{0.0};
            // (The thread's own.)
            ID3D12CommandQueue *queue = nullptr;
            ID3D12CommandAllocator *allocator = nullptr;
            ID3D12GraphicsCommandList *list = nullptr;
            ID3D12Fence *fence = nullptr;
            HANDLE event = nullptr;
            UINT64 value = 0;

            bool enabled()
            {
                static const bool on = !(std::getenv("TF_NATIVE_UPLOADER") && *std::getenv("TF_NATIVE_UPLOADER") == '0');
                return on;
            }

            // A texture's levels in a default-heap texture of its own (COMMON), its copies recorded. RGBA8 (the
            // chain's missing levels box-filtered here), or a large image's BC7 blocks (TODO 94) as they are.
            ID3D12Resource *record(const Texture &t, std::vector<ID3D12Resource *> &staging, UINT &levelsOut, UINT64 &bytesOut)
            {
                const bool bc = t.format == 1u;
                std::vector<const uint8_t *> mips;
                std::vector<std::pair<UINT, UINT>> sizes;
                std::vector<std::vector<uint32_t>> computed;
                if (bc)
                {
                    for (size_t l = 0; l < t.blocks.size(); ++l)
                    {
                        const UINT w = std::max(1u, t.width >> l), h = std::max(1u, t.height >> l);
                        if (t.blocks[l].size() != static_cast<size_t>((w + 3u) / 4u) * ((h + 3u) / 4u) * 16u)
                            break;
                        mips.push_back(t.blocks[l].data());
                        sizes.push_back({w, h});
                    }
                    if (mips.empty())
                        return nullptr;
                }
                else
                {
                    mips.push_back(reinterpret_cast<const uint8_t *>(t.pixels.data()));
                    sizes.push_back({t.width, t.height});
                    for (const auto &level : t.mips)
                    {
                        const UINT w = std::max(1u, sizes.back().first / 2u), h = std::max(1u, sizes.back().second / 2u);
                        if (level.size() != static_cast<size_t>(w) * h)
                            break;
                        mips.push_back(reinterpret_cast<const uint8_t *>(level.data()));
                        sizes.push_back({w, h});
                    }
                    // (The rest of the chain box-filtered here, on this thread: the game's own textures
                    // come with few levels or none - makeTexture's loop.)
                    computed.reserve(16);
                    while (sizes.back().first > 1u || sizes.back().second > 1u)
                    {
                        const UINT pw = sizes.back().first, ph = sizes.back().second;
                        const UINT w = std::max(1u, pw / 2u), h = std::max(1u, ph / 2u);
                        std::vector<uint32_t> dst(static_cast<size_t>(w) * h);
                        halveLevel(reinterpret_cast<const uint32_t *>(mips.back()), pw, ph, dst.data());
                        computed.push_back(std::move(dst));
                        mips.push_back(reinterpret_cast<const uint8_t *>(computed.back().data()));
                        sizes.push_back({w, h});
                    }
                }
                const UINT levels = static_cast<UINT>(mips.size());
                D3D12_RESOURCE_DESC td{};
                td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                td.Width = t.width;
                td.Height = t.height;
                td.DepthOrArraySize = 1;
                td.MipLevels = static_cast<UINT16>(levels);
                td.Format = bc ? DXGI_FORMAT_BC7_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
                td.SampleDesc.Count = 1;
                const D3D12_HEAP_PROPERTIES gpu = heap(D3D12_HEAP_TYPE_DEFAULT);
                ID3D12Resource *resource = nullptr;
                if (FAILED(g.device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                             IID_PPV_ARGS(&resource))))
                    return nullptr;
                std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fps(levels);
                std::vector<UINT> rows(levels);
                std::vector<UINT64> rowBytes(levels);
                UINT64 total = 0;
                g.device->GetCopyableFootprints(&td, 0, levels, 0, fps.data(), rows.data(), rowBytes.data(), &total);
                const D3D12_HEAP_PROPERTIES upHeap = heap(D3D12_HEAP_TYPE_UPLOAD);
                const D3D12_RESOURCE_DESC bd = bufferDesc(total);
                ID3D12Resource *buffer = nullptr;
                void *p = nullptr;
                const D3D12_RANGE none{0, 0};
                if (FAILED(g.device->CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                             IID_PPV_ARGS(&buffer))) ||
                    FAILED(buffer->Map(0, &none, &p)))
                {
                    release(buffer);
                    release(resource);
                    return nullptr;
                }
                // (A row: a line of texels, or of 4 x 4 blocks.)
                for (UINT m = 0; m < levels; ++m)
                {
                    const size_t pitch = bc ? static_cast<size_t>((sizes[m].first + 3u) / 4u) * 16u : static_cast<size_t>(sizes[m].first) * 4u;
                    for (UINT y = 0; y < rows[m]; ++y)
                        std::memcpy(static_cast<uint8_t *>(p) + fps[m].Offset + static_cast<size_t>(y) * fps[m].Footprint.RowPitch,
                                    mips[m] + static_cast<size_t>(y) * pitch, pitch);
                }
                buffer->Unmap(0, nullptr);
                for (UINT m = 0; m < levels; ++m)
                {
                    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
                    dst.pResource = resource;
                    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    dst.SubresourceIndex = m;
                    src.pResource = buffer;
                    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    src.PlacedFootprint = fps[m];
                    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                }
                staging.push_back(buffer);
                levelsOut = levels;
                bytesOut = total;
                return resource;
            }

            // (A copy queue's fence, waited for in steps with the device looked at: a removed device
            // ends the wait.)
            bool wait(UINT64 v)
            {
                while (fence->GetCompletedValue() < v)
                {
                    fence->SetEventOnCompletion(v, event);
                    if (WaitForSingleObject(event, 1000) == WAIT_OBJECT_0)
                        continue;
                    if (g_deviceLost.load(std::memory_order_relaxed) || FAILED(g.device->GetDeviceRemovedReason()))
                        return false;
                }
                return true;
            }

            void work()
            {
                // (At most this much staging in one copy batch: the batch's copies run, the staging is
                // let go of, the next taken - the upload heap never holds a level's worth at once.)
                // (48 MB, was 160: the game's own textures - stand-ins drawn meanwhile, TODO 76 - wait behind at most
                // one such batch of large images.)
                constexpr UINT64 kBatchBytes = 48ull << 20;
                for (;;)
                {
                    std::vector<Job> batch;
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        cv.wait(lock, [] { return !jobs.empty() || !urgent.empty(); });
                        // (The game's own textures first, many to a batch - small; then large images.)
                        while (!urgent.empty() && batch.size() < 256u)
                        {
                            batch.push_back(std::move(urgent.front()));
                            urgent.pop_front();
                        }
                        while (!jobs.empty() && batch.size() < 64u)
                        {
                            batch.push_back(std::move(jobs.front()));
                            jobs.pop_front();
                        }
                    }
                    const auto t0 = std::chrono::steady_clock::now();
                    allocator->Reset();
                    list->Reset(allocator, nullptr);
                    std::vector<ID3D12Resource *> staging;
                    std::vector<Done> recorded;
                    std::vector<std::shared_ptr<const Texture>> held;
                    UINT64 batchBytes = 0;
                    size_t taken = 0;
                    for (; taken < batch.size(); ++taken)
                    {
                        if (batchBytes >= kBatchBytes)
                            break;
                        Done d;
                        d.id = batch[taken].id;
                        d.generation = batch[taken].generation;
                        d.urgent = batch[taken].urgent;
                        d.texture = batch[taken].texture;
                        std::shared_ptr<const Texture> t = batch[taken].texture.lock();
                        if (t && (t->format == 1u ? !t->blocks.empty() : !t->pixels.empty() && t->pixels.size() == static_cast<size_t>(t->width) * t->height))
                        {
                            UINT64 b = 0;
                            d.resource = record(*t, staging, d.levels, b);
                            batchBytes += b;
                            if (!d.resource)
                                ++failed;
                            else
                                held.push_back(t);
                        }
                        recorded.push_back(d);
                    }
                    if (taken < batch.size())
                    {
                        // (Over the batch's staging: the rest back to the front of the queue.)
                        std::lock_guard<std::mutex> lock(mutex);
                        for (size_t k = batch.size(); k-- > taken;)
                            (batch[k].urgent ? urgent : jobs).push_front(std::move(batch[k]));
                    }
                    list->Close();
                    bool ok = true;
                    if (!staging.empty())
                    {
                        ID3D12CommandList *lists[] = {list};
                        queue->ExecuteCommandLists(1, lists);
                        queue->Signal(fence, ++value);
                        ok = wait(value);
                    }
                    for (ID3D12Resource *s : staging)
                        s->Release();
                    if (!ok)
                    {
                        for (Done &d : recorded)
                            release(d.resource);
                        state.store(-1);
                        std::fprintf(stderr, "[native render] uploader: the device was lost - large images no longer uploaded\n");
                        return;
                    }
                    // The texels let go of once on the GPU (TODO 50: a level's thousand 2K images held
                    // ~20 GB of RAM for good) - nothing reads a large image's texels after its upload.
                    for (const std::shared_ptr<const Texture> &t : held)
                    {
                        Texture &own = const_cast<Texture &>(*t);
                        made.fetch_add(1);
                        texels.fetch_add(own.format == 1u ? static_cast<size_t>(own.width) * own.height : own.pixels.size());
                        if (!own.upscaled)
                            continue; // (a game texture's texels stay: its large image's loader and the palettes read them)
                        std::vector<uint32_t>().swap(own.pixels);
                        std::vector<std::vector<uint32_t>>().swap(own.mips);
                        std::vector<std::vector<uint8_t>>().swap(own.blocks);
                    }
                    bytes.fetch_add(batchBytes);
                    held.clear();
                    busyMs.store(busyMs.load() + std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
                    std::lock_guard<std::mutex> lock(mutex);
                    for (Done &d : recorded)
                        done.push_back(d);
                }
            }

            // A device texture's upload asked of it (render thread). False: no uploader (off, or it could not start).
            bool ask(const std::shared_ptr<const Texture> &t, uint64_t id, uint32_t generation, bool urgentOne)
            {
                if (!enabled() || state.load() < 0 || g_deviceLost.load(std::memory_order_relaxed))
                    return false;
                if (state.load() == 0)
                {
                    D3D12_COMMAND_QUEUE_DESC qd{};
                    qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
                    if (FAILED(g.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) ||
                        FAILED(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&allocator))) ||
                        FAILED(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, allocator, nullptr, IID_PPV_ARGS(&list))) ||
                        FAILED(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) ||
                        !(event = CreateEventW(nullptr, FALSE, FALSE, nullptr)))
                    {
                        std::fprintf(stderr, "[native render] uploader: no copy queue - large images made on the render thread\n");
                        state.store(-1);
                        return false;
                    }
                    list->Close();
                    state.store(1);
                    std::thread(work).detach();
                    std::fprintf(stderr, "[native render] uploader: large images uploaded on a copy queue of their own\n");
                }
                std::lock_guard<std::mutex> lock(mutex);
                (urgentOne ? urgent : jobs).push_back(Job{t, id, generation, urgentOne});
                cv.notify_one();
                return true;
            }
        }

        // (P3 S3) The frame builder's textures asked of the device's later upload (uploadTextureLater), by the texture's
        // id: taken into g.textures once ready (takeLater, the frame's start).
        struct LaterAsk
        {
            gfx::TextureId id = 0;
            std::weak_ptr<const Texture> texture;
        };
        std::unordered_map<uint64_t, LaterAsk> g_laterAsked;

        // Asked once per texture (render thread). False: the device uploads nothing later (off, or it could not start).
        bool askLater(const std::shared_ptr<const Texture> &t, bool urgentOne = false)
        {
            if (g_laterAsked.count(t->id))
                return true;
            gfx::Device &dev = gfx::device();
            const gfx::TextureId id = dev.createTexture(t->width, t->height, t->format == 1u ? gfx::Format::BC7 : gfx::Format::RGBA8, 0u);
            if (!id)
                return false;
            if (!dev.uploadTextureLater(id, t, urgentOne))
            {
                dev.releaseTexture(id);
                return false;
            }
            g_laterAsked[t->id] = LaterAsk{id, t};
            return true;
        }

        // A stand-in for a game texture on the uploader's way (g_standIn): its average colour, from at most ~4,096 of
        // its texels (each alpha-weighted, so a cut-out's clear texels do not darken it), kept by id.
        GpuTexture *standInFor(const Texture &texture)
        {
            if (g_averages.size() > 8192u)
                g_averages.clear(); // (a movie's frames: a new texture each, most never asked for again)
            auto [it, fresh] = g_averages.try_emplace(texture.id);
            if (fresh)
            {
                const size_t n = texture.pixels.size(), step = std::max<size_t>(1u, n / 4096u);
                double sum[3] = {}, alpha = 0.0, weight = 0.0;
                size_t count = 0;
                for (size_t i = 0; i < n; i += step, ++count)
                {
                    const uint32_t c = texture.pixels[i];
                    const double a = ((c >> 24) & 0xFFu) / 255.0;
                    for (int k = 0; k < 3; ++k)
                        sum[k] += a * ((c >> (8 * k)) & 0xFFu) / 255.0;
                    alpha += a;
                    weight += a;
                }
                for (int k = 0; k < 3; ++k)
                    it->second[k] = weight > 0.0 ? static_cast<float>(sum[k] / weight) : 0.0f;
                it->second[3] = count ? static_cast<float>(alpha / count) : 1.0f;
                ++g_standInTextures;
            }
            std::memcpy(g_standInColour, it->second.data(), sizeof(g_standInColour));
            g_standIn.slot = 0u;
            ++g_frameStandIns;
            return &g_standIn;
        }

        GpuTexture *textureFor(const std::shared_ptr<const Texture> &texture, bool standInOk = false)
        {
            // (TODO 177) A texture converted on the pool whose content has a large image loaded already: that one, from
            // the conversion's own frame (the game's texture for that frame was the HQ's low-res flash, the prompts'
            // PS2 symbols). The frame waited for the conversion (waitFrameTextures), so `done` holds here.
            if (texture->pending && texture->pending->large && texture->pending->done.load(std::memory_order_acquire))
                return textureFor(texture->pending->large, standInOk);
            // (TODO 119: a font atlas's key-prompt twin while the keyboard and mouse are the device last used -
            // the prompts follow the device from one picture to the next)
            if (texture->keys && keyPromptsShown())
                return textureFor(texture->keys, standInOk);
            if (texture->xbox && xboxPromptsShown())
                return textureFor(texture->xbox, standInOk);
            auto it = g.textures.find(texture->id);
            if (it != g.textures.end())
                return &it->second;
            // (A large image: made by the uploader, the game's own texture drawn until it is done.)
            if (texture->upscaled && askLater(texture))
                return texture->fallback ? textureFor(texture->fallback, standInOk) : nullptr;
            // (A compressed large image needs the uploader - its game texture drawn when it is off.)
            if (texture->format != 0u)
                return texture->fallback ? textureFor(texture->fallback, standInOk) : nullptr;
            // (A game texture already on the uploader's way: its stand-in until it is done - or, with the frame's
            // budget not spent, made here after all: a stand-in lasts a frame or two, not the uploader's queue.
            // The uploader's copy, when it comes, is let go of - takeUploaded.)
            if (standInOk && !texture->upscaled && g_laterAsked.count(texture->id) && !texture->pixels.empty() &&
                !(kTexBudgetMs > 0.0 && g_frameMakeMs < kTexBudgetMs))
                return standInFor(*texture);
            // (Over the frame's creation budget: to the uploader, the stand-in drawn meanwhile.)
            if (standInOk && !texture->upscaled && kTexBudgetMs > 0.0 && g_frameMakeMs >= kTexBudgetMs &&
                texture->pixels.size() == static_cast<size_t>(texture->width) * texture->height && !texture->pixels.empty() &&
                askLater(texture, true))
                return standInFor(*texture);
            // (A game texture is made here, when first drawn with: it has nothing to stand in for it, and
            // a draw left out while its texture was on the uploader's way dropped whole models for a frame
            // - the HQ's Autobots gone at every menu change, TODO 71. TF_NATIVE_UPLOADGAME=1: on the
            // uploader, its draws left out the frame or two the copy takes - for tests only.)
            static const bool gameOn = std::getenv("TF_NATIVE_UPLOADGAME") && *std::getenv("TF_NATIVE_UPLOADGAME") == '1';
            if (gameOn && !texture->upscaled && texture->pixels.size() > 256u && askLater(texture, true))
                return nullptr;
            if (g.freeSlots.empty() || texture->pixels.size() != static_cast<size_t>(texture->width) * texture->height)
            {
                ++g_refused;
                return nullptr;
            }
            if (g_frameMade > 0 && (g_frameMade >= kTexCap.first || g_frameTexels + texture->pixels.size() > kTexCap.second))
            {
                ++g_frameDeferred;
                return nullptr;
            }
            ++g_frameMade; // (an attempt counts against the cap, made or not)
            g_frameTexels += texture->pixels.size();
            static int madeLog = std::getenv("TF_NATIVE_MADELOG") ? std::atoi(std::getenv("TF_NATIVE_MADELOG")) : 0;
            if (madeLog > 0 && g_made > 2000u)
            {
                --madeLog;
                std::fprintf(stderr, "[native render] gpu texture made: id %llu %ux%u use %ld, %zu resident, slots free %zu\n",
                             static_cast<unsigned long long>(texture->id), texture->width, texture->height, texture.use_count(), g.textures.size(),
                             g.freeSlots.size());
            }
            GpuTexture t;
            const auto makeStart = std::chrono::steady_clock::now();
            gfx::Device &dev = gfx::device();
            {
                std::vector<const uint32_t *> mips;
                std::vector<std::vector<uint32_t>> computed;
                mipChain(texture->width, texture->height, texture->pixels.data(), &texture->mips, mips, computed);
                t.id = dev.createTexture(texture->width, texture->height, gfx::Format::RGBA8, static_cast<uint32_t>(mips.size()));
                bool made = t.id != 0;
                for (size_t l = 0; made && l < mips.size(); ++l)
                    made = dev.uploadTexture(t.id, static_cast<uint32_t>(l), mips[l], std::max(1u, texture->width >> l) * 4u);
                if (made)
                    made = dev.textureReady(t.id);
                if (!made && t.id)
                {
                    dev.releaseTexture(t.id);
                    t.id = 0;
                }
            }
            g_frameMakeMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - makeStart).count();
            if (!t.id)
            {
                ++g_refused; // (its upload failed: refused, not made)
                return nullptr;
            }
            ++g_made;
            g_madeTexels += texture->pixels.size();
            t.slot = g.devTextures[t.id].slot;
            t.texture = texture;
            // A large image's texels let go of once on the GPU (2026-10-28, TODO 50): kept, a level's
            // thousand 2K images held ~20 GB of RAM for good. Nothing reads them after the upload (the
            // GPU copy lives exactly as long as this texture - prune()), and these are this thread's
            // alone by now (the loader handed it over; the game's side never reads an upscaled one).
            if (texture->upscaled)
            {
                t.largeBytes = g.devTextures[t.id].bytes;
                g_largeBytes += t.largeBytes;
                Texture &own = const_cast<Texture &>(*texture);
                std::vector<uint32_t>().swap(own.pixels);
                std::vector<std::vector<uint32_t>>().swap(own.mips);
            }
            return &g.textures.emplace(texture->id, t).first->second;
        }

        // (The device's) The uploader's finished uploads (once a frame, render thread): each given a descriptor slot and its
        // view; one whose texture is gone, or whose id was let go of meanwhile, released. A failed one is asked again while
        // its texture lives.
        void takeUploaded()
        {
            std::vector<up::Done> ready;
            {
                std::lock_guard<std::mutex> lock(up::mutex);
                ready.swap(up::done);
            }
            std::vector<up::Done> later;
            std::vector<up::Job> again;
            for (up::Done &d : ready)
            {
                State::DeviceTexture *r = d.id < g.devTextures.size() ? &g.devTextures[d.id] : nullptr;
                const bool current = r && r->generation == d.generation && !r->ready;
                std::shared_ptr<const Texture> t = d.texture.lock();
                if (!d.resource || !t || !current)
                {
                    release(d.resource); // (never used by this queue: released at once)
                    if (current && t)
                        again.push_back(up::Job{t, d.id, d.generation, d.urgent}); // (failed: asked again)
                    continue;
                }
                if (g.freeSlots.empty())
                {
                    later.push_back(d);
                    continue;
                }
                r->slot = g.freeSlots.back();
                g.freeSlots.pop_back();
                r->resource = d.resource;
                r->levels = d.levels;
                const D3D12_RESOURCE_DESC rd = d.resource->GetDesc();
                r->bytes = g.device->GetResourceAllocationInfo(0, 1, &rd).SizeInBytes;
                D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.Format = rd.Format; // (RGBA8, or a large image's BC7)
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sd.Texture2D.MipLevels = d.levels;
                D3D12_CPU_DESCRIPTOR_HANDLE h = g.srvHeap->GetCPUDescriptorHandleForHeapStart();
                h.ptr += static_cast<SIZE_T>(r->slot) * g.srvSize;
                srvWritten(r->slot, "an uploaded image's");
                g.device->CreateShaderResourceView(d.resource, &sd, h);
                r->ready = true;
            }
            if (!later.empty() || !again.empty())
            {
                std::lock_guard<std::mutex> lock(up::mutex);
                up::done.insert(up::done.end(), later.begin(), later.end());
                for (up::Job &j : again)
                    (j.urgent ? up::urgent : up::jobs).push_back(std::move(j));
                if (!again.empty())
                    up::cv.notify_one();
            }
        }

        // (The frame builder's) The textures asked of the device's later upload that are ready: taken into the GPU
        // textures, the game's texture each stood in for let go of; one whose texture is gone, or made on this thread
        // meanwhile, let go of.
        void takeLater()
        {
            gfx::Device &dev = gfx::device();
            for (auto it = g_laterAsked.begin(); it != g_laterAsked.end();)
            {
                std::shared_ptr<const Texture> t = it->second.texture.lock();
                if (!t || g.textures.count(it->first))
                {
                    dev.releaseTexture(it->second.id);
                    it = g_laterAsked.erase(it);
                    continue;
                }
                if (!dev.textureReady(it->second.id))
                {
                    ++it;
                    continue;
                }
                GpuTexture gt;
                gt.id = it->second.id;
                gt.slot = g.devTextures[gt.id].slot;
                gt.texture = t;
                if (t->upscaled)
                {
                    gt.largeBytes = g.devTextures[gt.id].bytes;
                    g_largeBytes += gt.largeBytes;
                }
                g.textures.emplace(it->first, gt);
                g_averages.erase(it->first);
                ++g_made;
                // (The game's texture no longer stands in: let go of - render thread only reads it.)
                const_cast<Texture &>(*t).fallback.reset();
                it = g_laterAsked.erase(it);
            }
        }

        // A slot's readback of the picture, made (again) for the picture's size.
        bool slotReadbackFor(UINT slot)
        {
            if (g.slotReadback[slot] && g.slotReadbackW[slot] == g.width && g.slotReadbackH[slot] == g.height)
                return true;
            if (g.slotReadback[slot])
                g.uploads.push_back(g.slotReadback[slot]); // (released once its last use is done)
            g.slotReadback[slot] = nullptr;
            D3D12_RESOURCE_DESC td{};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = g.width;
            td.Height = g.height;
            td.DepthOrArraySize = 1;
            td.MipLevels = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            UINT64 total = 0;
            g.device->GetCopyableFootprints(&td, 0, 1, 0, &g.slotFootprint[slot], nullptr, nullptr, &total);
            const D3D12_HEAP_PROPERTIES back = heap(D3D12_HEAP_TYPE_READBACK);
            const D3D12_RESOURCE_DESC rb = bufferDesc(total);
            if (FAILED(g.device->CreateCommittedResource(&back, D3D12_HEAP_FLAG_NONE, &rb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&g.slotReadback[slot]))))
                return false;
            g.slotReadbackW[slot] = g.width;
            g.slotReadbackH[slot] = g.height;
            return true;
        }

        // A shared texture of the picture's size (made again when it changes: a new generation, which
        // the window imports again).
        bool sharedFor(int index)
        {
            State::Shared &s = g.shared[index];
            if (s.resource && s.w == g.width && s.h == g.height)
                return true;
            if (s.resource)
                g.uploads.push_back(s.resource); // (released once its last use is done)
            if (s.handle)
                CloseHandle(s.handle);
            s = State::Shared{0, nullptr, 0u, 0u, 0u, s.generation};
            D3D12_RESOURCE_DESC td{};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = g.width;
            td.Height = g.height;
            td.DepthOrArraySize = 1;
            td.MipLevels = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            const D3D12_HEAP_PROPERTIES hp = heap(D3D12_HEAP_TYPE_DEFAULT);
            if (FAILED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &td, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                         IID_PPV_ARGS(&s.resource))) ||
                FAILED(g.device->CreateSharedHandle(s.resource, nullptr, GENERIC_ALL, nullptr, &s.handle)))
            {
                std::fprintf(stderr, "[native render] shared picture %ux%u not made: the CPU hand-over stays\n", g.width, g.height);
                release(s.resource);
                s.handle = nullptr;
                g_shareOn = -1;
                return false;
            }
            s.size = g.device->GetResourceAllocationInfo(0, 1, &td).SizeInBytes;
            s.w = g.width;
            s.h = g.height;
            ++s.generation;
            return true;
        }

        // The shared texture this frame draws into: not the one shown, not the newest handed over,
        // not the other frame's in flight.
        int pickShared()
        {
            // (Under the hand-over's lock: latestShared marks the texture held as it hands it over, so
            // the one the window is about to take is never picked between its taking and its marking.)
            std::lock_guard<std::mutex> lock(g_published.mutex);
            const int held = g_sharedHeld.load();
            for (int k = 0; k < State::kShared; ++k)
            {
                bool busy = k == held || k == g_sharedHeldBefore[0].load() || k == g_sharedHeldBefore[1].load() || k == g.lastShared ||
                            ((g_published.pending >> k) & 1u) != 0u;
                for (UINT f = 0; f < State::kFrames; ++f)
                    busy = busy || (f != g.slot && g.slotShared[f] == k);
                if (!busy)
                    return k;
            }
            return -1;
        }

        // ---- N9: the paced hand-over (TF_PACING) -----------------------------------------------------------
        // Each picture handed to the window at its frame's moment (Frame::dueNs, the VBlank it was handed over at
        // on the host's schedule) plus a delay - the same for every picture, so that they reach the screen as
        // evenly as the frames' moments are spaced, whatever the render thread's and the GPU's times do from one
        // frame to the next. Before (TF_PACING=0) a picture went over when the render thread began the frame after
        // next and found its GPU work done (finishSlot), and the window showed it at the first VBlank after that:
        // its place among the VBlanks moved with the game's and the render thread's times, a picture up one
        // VBlank, two or three - the judder of TODO 95. The delay follows the pictures' readiness (the GPU done,
        // after the frame's moment): the 98th percentile of the last 2 s plus 1 ms, raised at once by a late one
        // (not by a stall of over four frames: a load, a hitch, which no delay hides) and lowered slowly. When
        // the VBlanks are timed by the screen's refresh (a clock of its own, a whole number of refreshes a VBlank:
        // Frame::vblanksOnRefresh) the delay is a whole number of VBlanks, so each picture meets its refresh. A thread of its own waits for each picture's fence (the GPU's queue finishes them
        // in order) and then for its time. TF_PACING=ready hands each over as soon as the GPU is done with it.
        namespace pace
        {
            struct Item
            {
                UINT64 fence = 0;
                d3d::SharedPicture picture;
                int64_t periodNs = 0;
                int64_t quantumNs = 0; // (N9 stage 2: whole refreshes on a screen of fixed refresh - the picture's period)
                int64_t refreshNs = 0; // (TODO 169: the screen's refresh, the delay's step on a fixed refresh)
                bool onRefresh = false;
                int64_t readyNs = 0; // (stamped once its fence is seen passed)
            };
            std::mutex g_mutex;
            std::condition_variable g_cv;
            std::deque<Item> g_queue;
            bool g_started = false;
            double g_delayMs = -1.0; // (-1: none yet)
            std::atomic<double> g_delayShown{0.0}; // (the same, for other threads: N9 stage 3's tries)
            std::deque<std::pair<int64_t, double>> g_lateness; // (when, ms after the frame's moment)

            int64_t now()
            {
                return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            }

            // The fences passed now stamped with this moment (they finish in order: the front ones).
            void stampPassed(UINT64 done, int64_t t)
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                for (Item &it : g_queue)
                {
                    if (it.fence > done)
                        break;
                    if (it.readyNs == 0)
                        it.readyNs = t;
                }
            }

            // The delay after the frame's moment for this picture (ms), from its readiness.
            double delayFor(double lateMs, double periodMs, int64_t t)
            {
                if (lateMs > 4.0 * periodMs)
                    return g_delayMs < 0.0 ? 0.0 : g_delayMs; // (a stall: shown when ready, the delay kept)
                g_lateness.emplace_back(t, lateMs);
                while (!g_lateness.empty() && t - g_lateness.front().first > 2000000000ll)
                    g_lateness.pop_front();
                std::vector<double> v;
                v.reserve(g_lateness.size());
                for (const auto &l : g_lateness)
                    v.push_back(l.second);
                const size_t at = std::min(v.size() - 1u, static_cast<size_t>(0.98 * static_cast<double>(v.size())));
                std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(at), v.end());
                const double wanted = v[at] + 1.0;
                // (TODO 169) Down by 2% of what is over the wanted delay a picture, 0.05 to 0.25 ms (TF_PACING_DOWN=<ms>:
                // a fixed step; 0.05 was the old one). The fixed 0.05 ms took 7-10 s to give back a delay raised by a load or
                // a rate change (the other PC at 80: shown 50 ms after the moment for the first 8 s, 17 ms after), and a busy
                // PC's late pictures kept it high for good; a step of at most 0.25 ms moves a picture by 2% of a 12.5 ms
                // interval - not seen.
                static const double fixedDown = std::getenv("TF_PACING_DOWN") ? std::atof(std::getenv("TF_PACING_DOWN")) : 0.0;
                if (g_delayMs < 0.0 || lateMs + 1.0 > g_delayMs)
                    g_delayMs = std::max(g_delayMs, lateMs + 1.0); // (up at once)
                else if (wanted < g_delayMs - 0.5)
                {
                    const double step = fixedDown > 0.0 ? fixedDown : std::clamp(0.02 * (g_delayMs - wanted), 0.05, 0.25);
                    g_delayMs = std::max(wanted, g_delayMs - step);
                }
                g_delayMs = std::min(g_delayMs, 4.0 * periodMs);
                g_delayShown.store(g_delayMs, std::memory_order_relaxed);
                return g_delayMs;
            }

            void handOver(const Item &it, int64_t readyNs)
            {
                std::lock_guard<std::mutex> lock(g_published.mutex);
                g_published.shared = it.picture;
                g_published.shared.readyNs = readyNs;
                g_published.has3D = it.picture.has3D;
                g_published.width = it.picture.width;
                g_published.height = it.picture.height;
                ++g_published.serial;
                g_published.lastNs = now();
                g_published.pending &= ~(1u << it.picture.index);
                g.lastShared = it.picture.index;
                g_published.cv.notify_all();
            }

            void run()
            {
                HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                const bool paced = d3d::pacingMode() == 2;
                for (;;)
                {
                    Item it;
                    {
                        std::unique_lock<std::mutex> lock(g_mutex);
                        g_cv.wait(lock, [] { return !g_queue.empty(); });
                        it = g_queue.front();
                    }
                    // Its fence (the GPU done with the picture).
                    if (g.fence->GetCompletedValue() < it.fence)
                    {
                        g.fence->SetEventOnCompletion(it.fence, event);
                        while (WaitForSingleObject(event, 200) == WAIT_TIMEOUT)
                            if (g_deviceLost.load(std::memory_order_relaxed) || FAILED(g.device->GetDeviceRemovedReason()))
                                break;
                    }
                    const int64_t seen = now();
                    stampPassed(g.fence->GetCompletedValue(), seen);
                    {
                        std::lock_guard<std::mutex> lock(g_mutex);
                        it.readyNs = g_queue.front().readyNs ? g_queue.front().readyNs : seen;
                    }
                    // Its time: the frame's moment plus the delay (on a fixed refresh, whole VBlanks).
                    if (paced && it.picture.dueNs != 0)
                    {
                        const double periodMs = it.periodNs > 0 ? static_cast<double>(it.periodNs) / 1.0e6 : 16.667;
                        double delay = delayFor(static_cast<double>(it.readyNs - it.picture.dueNs) / 1.0e6, periodMs, it.readyNs);
                        if (it.onRefresh)
                        {
                            // (TODO 169) The delay a whole number of the screen's REFRESHES, not of the picture's period. The
                            // frames' moments are on refreshes (a whole number of them a picture), so any whole number of
                            // refreshes after the moment still meets a refresh and keeps the pictures exactly as far apart
                            // as before. Rounding up to whole picture periods put a picture ready 15-17 ms after its moment
                            // at a cap of 60 on a 180 Hz screen a whole period later (shown 34 ms after the moment where
                            // 23 would do); at 30, 33 ms later. TF_PACING_QUANTUM=picture: whole picture periods (before).
                            static const bool byPicture = std::getenv("TF_PACING_QUANTUM") && std::string(std::getenv("TF_PACING_QUANTUM")) == "picture";
                            const double picture = it.quantumNs > 0 ? static_cast<double>(it.quantumNs) / 1.0e6 : periodMs;
                            const double refresh = static_cast<double>(it.refreshNs) / 1.0e6;
                            const double q = !byPicture && refresh > 1.0 && refresh <= picture + 0.01 ? refresh : picture;
                            delay = std::ceil(delay / q - 1.0e-3) * q;
                        }
                        const int64_t at = it.picture.dueNs + static_cast<int64_t>(delay * 1.0e6);
                        // (meanwhile the later pictures' fences are stamped as they pass, to the half millisecond)
                        for (int64_t t = now(); t < at; t = now())
                        {
                            std::this_thread::sleep_for(std::chrono::nanoseconds(std::min<int64_t>(at - t, 500000)));
                            stampPassed(g.fence->GetCompletedValue(), now());
                        }
                    }
                    handOver(it, it.readyNs);
                    {
                        std::lock_guard<std::mutex> lock(g_mutex);
                        g_queue.pop_front();
                    }
                }
            }

            // The frame's picture (in shared texture `picture.index`, done once `fence` passes) to be handed over.
            void submit(UINT64 fence, const d3d::SharedPicture &picture, int64_t periodNs, bool onRefresh, int64_t quantumNs,
                        int64_t refreshNs)
            {
                {
                    std::lock_guard<std::mutex> lock(g_published.mutex);
                    g_published.pending |= 1u << picture.index;
                }
                std::lock_guard<std::mutex> lock(g_mutex);
                if (!g_started)
                {
                    g_started = true;
                    std::thread(run).detach();
                }
                Item it;
                it.fence = fence;
                it.picture = picture;
                it.periodNs = periodNs;
                it.onRefresh = onRefresh;
                it.quantumNs = quantumNs;
                it.refreshNs = refreshNs;
                g_queue.push_back(it);
                g_cv.notify_one();
            }
        }

        // Resources released on a thread of their own (finishSlot: a long list at once).
        double g_releaseMs = 0.0; // (this frame's releases on the render thread: the slow frames' line)
        void releaseLater(std::vector<ID3D12Resource *> &&list)
        {
            static std::mutex mutex;
            static std::condition_variable cv;
            static std::deque<std::vector<ID3D12Resource *>> queue;
            static bool started = false;
            std::lock_guard<std::mutex> lock(mutex);
            if (!started)
            {
                started = true;
                std::thread([] {
                    for (;;)
                    {
                        std::vector<ID3D12Resource *> batch;
                        {
                            std::unique_lock<std::mutex> l(mutex);
                            cv.wait(l, [] { return !queue.empty(); });
                            batch = std::move(queue.front());
                            queue.pop_front();
                        }
                        for (ID3D12Resource *r : batch)
                            r->Release();
                    }
                }).detach();
            }
            queue.push_back(std::move(list));
            cv.notify_one();
        }

        // A finished slot: its picture handed to the window, what it let go of released.
        // (TODO 118, a checking aid: TF_NATIVE_STREAM=<from>-<to> - every frame handed over between those VBlank ticks saved
        // WITHOUT waiting for the GPU. A shot (TF_NATIVE_SHOT) waits for its frame to finish, so frames never overlap on the
        // GPU while shots are taken - which would hide anything that depends on that. Here the picture is copied into a
        // readback of its frame slot, read when the slot comes round, and shrunk (TF_NATIVE_STREAM_SHRINK, default 3) and
        // written by a thread of its own -> runs\stream\s_<sequence>_<tick>.bmp. Frames the writer cannot keep up with are
        // left out and counted; the sequence numbers show where.)
        namespace stream
        {
            uint64_t from = 0, to = 0;
            UINT shrink = 3;
            bool enabled()
            {
                static const bool on = [] {
                    const char *e = std::getenv("TF_NATIVE_STREAM");
                    if (!e || !*e)
                        return false;
                    char *end = nullptr;
                    from = std::strtoull(e, &end, 10);
                    to = end && *end == '-' ? std::strtoull(end + 1, nullptr, 10) : from;
                    if (const char *k = std::getenv("TF_NATIVE_STREAM_SHRINK"))
                        shrink = static_cast<UINT>(std::max(1, std::atoi(k)));
                    CreateDirectoryA("runs\\stream", nullptr);
                    std::fprintf(stderr, "[native render] stream: every frame from tick %llu to %llu saved, shrunk %u times\n",
                                 static_cast<unsigned long long>(from), static_cast<unsigned long long>(to), shrink);
                    return to >= from;
                }();
                return on;
            }
            bool wanted(uint64_t tick) { return enabled() && tick >= from && tick <= to; }
            ID3D12Resource *rb[State::kFrames] = {};
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[State::kFrames] = {};
            UINT rw[State::kFrames] = {}, rh[State::kFrames] = {};
            uint64_t tick[State::kFrames] = {};
            bool pending[State::kFrames] = {};
            struct Job
            {
                uint64_t seq = 0, tick = 0;
                UINT w = 0, h = 0;
                std::vector<uint8_t> rgba;
            };
            std::mutex mutex;
            std::condition_variable wake;
            std::deque<Job> jobs;
            bool started = false;
            uint64_t seq = 0, dropped = 0;
            void writer()
            {
                for (;;)
                {
                    Job j;
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        wake.wait(lock, [] { return !jobs.empty(); });
                        j = std::move(jobs.front());
                        jobs.pop_front();
                    }
                    const UINT k = shrink, ow = j.w / k, oh = j.h / k, rowBytes = (ow * 3u + 3u) & ~3u;
                    std::vector<uint8_t> out(static_cast<size_t>(rowBytes) * oh);
                    for (UINT y = 0; y < oh; ++y)
                        for (UINT x = 0; x < ow; ++x)
                        {
                            UINT sum[3] = {};
                            for (UINT dy = 0; dy < k; ++dy)
                            {
                                const uint8_t *px = j.rgba.data() + (static_cast<size_t>(y * k + dy) * j.w + x * k) * 4u;
                                for (UINT dx = 0; dx < k; ++dx, px += 4)
                                    sum[0] += px[0], sum[1] += px[1], sum[2] += px[2];
                            }
                            uint8_t *o = out.data() + static_cast<size_t>(oh - 1u - y) * rowBytes + x * 3u; // (bottom up, BGR)
                            o[0] = static_cast<uint8_t>(sum[2] / (k * k));
                            o[1] = static_cast<uint8_t>(sum[1] / (k * k));
                            o[2] = static_cast<uint8_t>(sum[0] / (k * k));
                        }
                    char path[96];
                    std::snprintf(path, sizeof(path), "runs\\stream\\s_%06llu_%llu.bmp", static_cast<unsigned long long>(j.seq),
                                  static_cast<unsigned long long>(j.tick));
                    if (FILE *f = std::fopen(path, "wb"))
                    {
                        uint8_t header[54] = {'B', 'M'};
                        auto put32 = [&](int at, uint32_t v) { std::memcpy(header + at, &v, 4); };
                        put32(2, 54u + static_cast<uint32_t>(out.size()));
                        put32(10, 54u);
                        put32(14, 40u);
                        put32(18, ow);
                        put32(22, oh);
                        header[26] = 1;
                        header[28] = 24;
                        put32(34, static_cast<uint32_t>(out.size()));
                        std::fwrite(header, 1, sizeof(header), f);
                        std::fwrite(out.data(), 1, out.size(), f);
                        std::fclose(f);
                    }
                }
            }
            bool readbackFor(UINT slot)
            {
                if (rb[slot] && rw[slot] == g.width && rh[slot] == g.height)
                    return true;
                if (rb[slot])
                    g.uploads.push_back(rb[slot]); // (released once its last use is done)
                rb[slot] = nullptr;
                D3D12_RESOURCE_DESC td{};
                td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                td.Width = g.width;
                td.Height = g.height;
                td.DepthOrArraySize = 1;
                td.MipLevels = 1;
                td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                td.SampleDesc.Count = 1;
                UINT64 total = 0;
                g.device->GetCopyableFootprints(&td, 0, 1, 0, &fp[slot], nullptr, nullptr, &total);
                const D3D12_HEAP_PROPERTIES back = heap(D3D12_HEAP_TYPE_READBACK);
                const D3D12_RESOURCE_DESC b = bufferDesc(total);
                if (FAILED(g.device->CreateCommittedResource(&back, D3D12_HEAP_FLAG_NONE, &b, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                             IID_PPV_ARGS(&rb[slot]))))
                    return false;
                rw[slot] = g.width;
                rh[slot] = g.height;
                return true;
            }
            // (the slot's frame is done: its picture to the writer)
            void finish(UINT slot)
            {
                if (!pending[slot] || !rb[slot])
                {
                    pending[slot] = false;
                    return;
                }
                pending[slot] = false;
                const UINT w = rw[slot], h = rh[slot];
                const D3D12_RANGE all{0, static_cast<SIZE_T>(fp[slot].Offset + static_cast<UINT64>(fp[slot].Footprint.RowPitch) * (h - 1u) + w * 4u)};
                void *p = nullptr;
                const uint64_t n = seq++;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (jobs.size() >= 48u)
                    {
                        if (++dropped % 50u == 1u)
                            std::fprintf(stderr, "[native render] stream: the writer is behind - %llu frames left out\n",
                                         static_cast<unsigned long long>(dropped));
                        return;
                    }
                }
                if (FAILED(rb[slot]->Map(0, &all, &p)))
                    return;
                Job j;
                j.seq = n, j.tick = tick[slot], j.w = w, j.h = h;
                j.rgba.resize(static_cast<size_t>(w) * h * 4u);
                for (UINT y = 0; y < h; ++y)
                    std::memcpy(j.rgba.data() + static_cast<size_t>(y) * w * 4u,
                                static_cast<const uint8_t *>(p) + fp[slot].Offset + static_cast<size_t>(y) * fp[slot].Footprint.RowPitch, w * 4u);
                const D3D12_RANGE none{0, 0};
                rb[slot]->Unmap(0, &none);
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (!started)
                    {
                        started = true;
                        std::thread(writer).detach();
                    }
                    jobs.push_back(std::move(j));
                }
                wake.notify_one();
            }
        }

        void finishSlot(UINT slot)
        {
            if (const UINT n = std::min(g.slotPictures[slot], kMaxPictures); n && g.picTimes && g.timestampHz)
            {
                // (N9 stage 3: the frame's pictures on the GPU - the first, drawn as the frame's own, and the extra ones
                // drawn again from its recording - for the pictures' cap and the profile's whole-frame time)
                void *p = nullptr;
                const SIZE_T at = static_cast<SIZE_T>(slot) * kPicStride * 8u;
                const D3D12_RANGE r{at, at + n * 16u};
                if (SUCCEEDED(g.picTimes->Map(0, &r, &p)))
                {
                    const UINT64 *t = reinterpret_cast<const UINT64 *>(static_cast<const uint8_t *>(p) + at);
                    auto smooth = [](std::atomic<double> &v, double ms) {
                        const double was = v.load(std::memory_order_relaxed);
                        v.store(was <= 0.0 ? ms : (ms > was ? 0.8 * was + 0.2 * ms : 0.97 * was + 0.03 * ms), std::memory_order_relaxed);
                    };
                    double extra = 0.0;
                    UINT extras = 0;
                    {
                        // (TF_N9_TSLOG=<n>: the first n slots' raw picture timestamps - a checking aid)
                        static int tsLog = std::getenv("TF_N9_TSLOG") ? std::atoi(std::getenv("TF_N9_TSLOG")) : 0;
                        if (tsLog > 0 && n > 1u)
                        {
                            --tsLog;
                            std::fprintf(stderr, "[n9] slot %u, %u pictures:", slot, n);
                            for (UINT k = 0; k < n; ++k)
                                std::fprintf(stderr, " %llu-%llu", static_cast<unsigned long long>(t[2 * k]), static_cast<unsigned long long>(t[2 * k + 1]));
                            std::fputc('\n', stderr);
                        }
                    }
                    for (UINT k = 0; k < n; ++k)
                        if (t[2 * k + 1] > t[2 * k])
                        {
                            const double ms = static_cast<double>(t[2 * k + 1] - t[2 * k]) * 1000.0 / static_cast<double>(g.timestampHz);
                            if (k == 0)
                            {
                                smooth(g_gpuPictureMs, ms);
                                g_frameGpu += ms;
                                ++g_frameGpuFrames;
                            }
                            else
                                extra += ms, ++extras;
                        }
                    if (extras)
                        smooth(g_gpuExtraMs, extra / extras);
                    const D3D12_RANGE none{0, 0};
                    g.picTimes->Unmap(0, &none);
                }
            }
            g.slotPictures[slot] = 0;
            if (g.probeTaken[slot])
            {
                // (The sun's occlusion probe of the slot's frame, its GPU work done: to the game's read-back, roadmap N3a.)
                g.probeTaken[slot] = false;
                void *p = nullptr;
                const D3D12_RANGE r{slot * 16384u, slot * 16384u + 16384u};
                if (g.probeReadback && SUCCEEDED(g.probeReadback->Map(0, &r, &p)))
                {
                    occlusion::deliver(g.probeTick[slot], reinterpret_cast<const float *>(static_cast<const uint8_t *>(p) + slot * 16384u));
                    const D3D12_RANGE none{0, 0};
                    g.probeReadback->Unmap(0, &none);
                }
            }
            if (g.aaTimed[slot])
            {
                // (The anti-aliasing pass's two timestamps, TF_NATIVE_PROFILE.)
                g.aaTimed[slot] = false;
                void *p = nullptr;
                const D3D12_RANGE r{slot * 16u, slot * 16u + 16u};
                if (g.aaTimes && g.timestampHz && SUCCEEDED(g.aaTimes->Map(0, &r, &p)))
                {
                    UINT64 t[2];
                    std::memcpy(t, static_cast<const uint8_t *>(p) + slot * 16u, sizeof(t));
                    if (t[1] > t[0])
                    {
                        g_aaGpu += static_cast<double>(t[1] - t[0]) * 1000.0 / static_cast<double>(g.timestampHz);
                        ++g_aaGpuFrames;
                    }
                    const D3D12_RANGE none{0, 0};
                    g.aaTimes->Unmap(0, &none);
                }
            }
            if (!g.passKinds[slot].empty())
            {
                const UINT n = static_cast<UINT>(g.passKinds[slot].size());
                void *p = nullptr;
                const UINT at = slot * State::kPassMarks * 8u;
                const D3D12_RANGE r{at, at + n * 8u};
                if (g.passTimes && g.timestampHz && SUCCEEDED(g.passTimes->Map(0, &r, &p)))
                {
                    const UINT64 *t = reinterpret_cast<const UINT64 *>(static_cast<const uint8_t *>(p) + at);
                    g_passTop.clear();
                    if (drawCostWindow(g_profWindow))
                    {
                        // (The first frame's count of marks sets the window's; frames with another count are left out.)
                        if (g_drawCostSum.empty())
                        {
                            g_drawCostSum.assign(n, 0.0);
                            g_drawCostN.assign(n, 0u);
                        }
                        if (g_drawCostSum.size() == n)
                        {
                            for (UINT i = 1; i < n; ++i)
                                if (t[i] >= t[i - 1])
                                {
                                    g_drawCostSum[i] += static_cast<double>(t[i] - t[i - 1]) * 1000.0 / static_cast<double>(g.timestampHz);
                                    ++g_drawCostN[i];
                                }
                            ++g_drawCostN[0];
                            if (g.statData && g.statsTaken[slot])
                            {
                                void *sp = nullptr;
                                const SIZE_T at = static_cast<SIZE_T>(slot) * State::kPassMarks * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
                                const D3D12_RANGE sr{at, at + n * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS)};
                                if (SUCCEEDED(g.statData->Map(0, &sr, &sp)))
                                {
                                    const auto *st = reinterpret_cast<const D3D12_QUERY_DATA_PIPELINE_STATISTICS *>(static_cast<const uint8_t *>(sp) + at);
                                    g_drawPixels.resize(n, 0.0);
                                    g_drawPrims.resize(n, 0.0);
                                    for (UINT i = 0; i < n; ++i)
                                    {
                                        g_drawPixels[i] += static_cast<double>(st[i].PSInvocations);
                                        g_drawPrims[i] += static_cast<double>(st[i].CPrimitives);
                                    }
                                    ++g_drawStatFrames;
                                    const D3D12_RANGE none{0, 0};
                                    g.statData->Unmap(0, &none);
                                }
                            }
                            g_drawCostNote.resize(n);
                            for (UINT i = 0; i < n && i < g.passNotes[slot].size(); ++i)
                                g_drawCostNote[i] = {g.passKinds[slot][i], g.passNotes[slot][i]};
                        }
                    }
                    for (UINT i = 1; i < n; ++i)
                        if (t[i] >= t[i - 1] && g.passKinds[slot][i] < kPassKinds)
                        {
                            const double ms = static_cast<double>(t[i] - t[i - 1]) * 1000.0 / static_cast<double>(g.timestampHz);
                            g_passGpu[g.passKinds[slot][i]] += ms;
                            if (ms > 0.05 && i < g.passNotes[slot].size())
                            {
                                const auto &nt = g.passNotes[slot][i];
                                char text[200];
                                std::snprintf(text, sizeof(text), "draw %u %s: %u indices, texture %ux%u, tex0 %016llx alpha %llx, colour mode %u", nt.draw,
                                              kPassKindNames[g.passKinds[slot][i]], nt.indices, nt.texW, nt.texH, static_cast<unsigned long long>(nt.tex0),
                                              static_cast<unsigned long long>(nt.alpha), nt.mode);
                                g_passTop.emplace_back(ms, text);
                            }
                        }
                    ++g_passFrames;
                    const D3D12_RANGE none{0, 0};
                    g.passTimes->Unmap(0, &none);
                }
                g.passKinds[slot].clear();
                g.passNotes[slot].clear();
            }
            if (g.frameTimed[slot])
            {
                g.frameTimed[slot] = false;
                void *p = nullptr;
                const UINT at = (2u * State::kFrames + slot * 2u) * 8u;
                const D3D12_RANGE r{at, at + 16u};
                if (g.aaTimes && g.timestampHz && SUCCEEDED(g.aaTimes->Map(0, &r, &p)))
                {
                    UINT64 t[2];
                    std::memcpy(t, static_cast<const uint8_t *>(p) + at, sizeof(t));
                    if (t[1] > t[0])
                    {
                        const double ms = static_cast<double>(t[1] - t[0]) * 1000.0 / static_cast<double>(g.timestampHz);
                        g_frameGpu += ms;
                        ++g_frameGpuFrames;
                        const double was = g_gpuPictureMs.load(std::memory_order_relaxed);
                        g_gpuPictureMs.store(was <= 0.0 ? ms : (ms > was ? 0.8 * was + 0.2 * ms : 0.97 * was + 0.03 * ms), std::memory_order_relaxed);
                    }
                    const D3D12_RANGE none{0, 0};
                    g.aaTimes->Unmap(0, &none);
                }
            }
            if (const int k = g.slotShared[slot]; k >= 0 && !g.slotPaced[slot])
            {
                const State::Shared &s = g.shared[k];
                std::lock_guard<std::mutex> lock(g_published.mutex);
                g_published.shared = d3d::SharedPicture{s.handle, s.size, s.w, s.h, k, s.generation, g.slotHas3D[slot]};
                g_published.shared.tick = g.slotTick[slot];
                g_published.shared.dueNs = g.slotDueNs[slot];
                g_published.shared.readyNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                g_published.has3D = g.slotHas3D[slot];
                g_published.width = s.w;
                g_published.height = s.h;
                ++g_published.serial;
                g_published.lastNs = g_published.shared.readyNs;
                g.lastShared = k;
                g_published.cv.notify_all();
            }
            g.slotShared[slot] = -1;
            g.slotPaced[slot] = false;
            if (g.slotPublish[slot] && g.slotReadback[slot])
            {
                const UINT w = g.slotReadbackW[slot], h = g.slotReadbackH[slot];
                const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &fp = g.slotFootprint[slot];
                void *p = nullptr;
                const D3D12_RANGE all{0, static_cast<SIZE_T>(fp.Offset + static_cast<UINT64>(fp.Footprint.RowPitch) * (h - 1u) + w * 4u)}; // (the last row unpadded)
                const HRESULT mapped = g.slotReadback[slot]->Map(0, &all, &p);
                static int told = 3;
                if (told > 0)
                {
                    --told;
                    std::fprintf(stderr, "[native render] picture %ux%u handed over from slot %u (map %08lx)\n", w, h, slot,
                                 static_cast<unsigned long>(mapped));
                }
                if (SUCCEEDED(mapped))
                {
                    // Copied out (the readback is the next frame's) into a buffer of the game's side,
                    // then swapped in under the lock: no copy is made while it is held.
                    static std::vector<uint32_t> back;
                    back.resize(static_cast<size_t>(w) * h);
                    for (UINT y = 0; y < h; ++y)
                        std::memcpy(back.data() + static_cast<size_t>(y) * w,
                                    static_cast<const uint8_t *>(p) + fp.Offset + static_cast<size_t>(y) * fp.Footprint.RowPitch, w * 4u);
                    {
                        std::lock_guard<std::mutex> lock(g_published.mutex);
                        g_published.pixels.swap(back);
                        g_published.shared.index = -1;
                        g_published.has3D = g.slotHas3D[slot];
                        g_published.width = w;
                        g_published.height = h;
                        ++g_published.serial;
                    }
                    const D3D12_RANGE written{0, 0};
                    g.slotReadback[slot]->Unmap(0, &written);
                }
            }
            g.slotPublish[slot] = false;
            stream::finish(slot);
            // (Many at once - a level's or the HQ's textures let go of when it ends, a thousand and more -
            // released on a thread of their own: each Release of a texture costs the driver a fraction
            // of a millisecond, and the frame that freed the HQ's took 237 ms on the test PC, TODO 59.)
            const auto releaseStart = std::chrono::steady_clock::now();
            for (ID3D12Resource *r : g.slotReleases[slot])
                pool::forget(r);
            // (Every frame's on that thread now, not only batches over 32: with the video memory over its budget
            // a single Release took ~2 ms on the test PC - 70-110 ms of releases in the HQ's frames, TODO 76.)
            if (!g.slotReleases[slot].empty())
                releaseLater(std::move(g.slotReleases[slot]));
            g.slotReleases[slot].clear();
            g_releaseMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - releaseStart).count();
            g.freeSlots.insert(g.freeSlots.end(), g.slotFreeSlots[slot].begin(), g.slotFreeSlots[slot].end());
            g.slotFreeSlots[slot].clear();
        }

        // The next frame's slot: the frame it last held is waited for (two frames back), and an
        // other finished one handed over without waiting.
        void beginSlot()
        {
            g.slot = static_cast<UINT>(g.frameIndex++ % State::kFrames);
            const UINT64 done = g.fence->GetCompletedValue();
            for (UINT f = 0; f < State::kFrames; ++f)
                if (f != g.slot && g.slotFence[f] && done >= g.slotFence[f])
                    finishSlot(f);
            if (g.slotFence[g.slot] && done < g.slotFence[g.slot])
            {
                const auto t0 = std::chrono::steady_clock::now();
                waitFence(g.slotFence[g.slot]);
                const double waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                g_slotWaitMs += waited;
                g_slotWaitAll.store(g_slotWaitAll.load(std::memory_order_relaxed) + waited, std::memory_order_relaxed);
            }
            finishSlot(g.slot);
        }

        // Every frame done (before a resource in use is replaced).
        void waitIdle()
        {
            waitForGpu();
            for (UINT f = 0; f < State::kFrames; ++f)
                finishSlot(f);
        }

        void saveBmp(const std::string &path)
        {
            void *p = nullptr;
            const D3D12_RANGE all{0, static_cast<SIZE_T>(g.footprint.Offset + static_cast<UINT64>(g.footprint.Footprint.RowPitch) * (g.height - 1u) + g.width * 4u)}; // (the last row unpadded)
            if (const HRESULT hr = g.readback->Map(0, &all, &p); FAILED(hr))
            {
                std::fprintf(stderr, "[native render] picture not saved: the readback did not map (%08lx)\n", static_cast<unsigned long>(hr));
                return;
            }
            FILE *f = std::fopen(path.c_str(), "wb");
            if (!f)
                std::fprintf(stderr, "[native render] picture not saved: %s did not open\n", path.c_str());
            if (f)
            {
                const uint32_t rowBytes = g.width * 4u, size = rowBytes * g.height;
                uint8_t header[54] = {'B', 'M'};
                auto put32 = [&](int at, uint32_t v) { std::memcpy(header + at, &v, 4); };
                put32(2, 54u + size);
                put32(10, 54u);
                put32(14, 40u);
                put32(18, g.width);
                put32(22, g.height);
                header[26] = 1;
                header[28] = 32;
                put32(34, size);
                std::fwrite(header, 1, sizeof(header), f);
                std::vector<uint8_t> row(rowBytes);
                for (UINT y = g.height; y-- > 0;) // (BMP rows bottom up, BGRA)
                {
                    std::memcpy(row.data(), static_cast<const uint8_t *>(p) + g.footprint.Offset + static_cast<size_t>(y) * g.footprint.Footprint.RowPitch,
                                rowBytes);
                    for (uint32_t x = 0; x < rowBytes; x += 4u)
                        std::swap(row[x], row[x + 2u]);
                    std::fwrite(row.data(), 1, rowBytes, f);
                }
                std::fclose(f);
                std::fprintf(stderr, "[native render] picture saved: %s\n", path.c_str());
            }
            const D3D12_RANGE written{0, 0};
            g.readback->Unmap(0, &written);
        }
    }

    bool start()
    {
        if (g.device || g.failed)
            return g.device != nullptr;
        g.failed = true;
        // TF_NATIVE_DEBUG=1: the debug layer, its messages on stderr; =2 adds GPU-based validation.
        const char *debug = std::getenv("TF_NATIVE_DEBUG");
        const bool debugging = debug && (*debug == '1' || *debug == '2');
        if (debugging)
        {
            ID3D12Debug *layer = nullptr;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&layer))))
            {
                layer->EnableDebugLayer();
                ID3D12Debug1 *layer1 = nullptr;
                const bool gbv = *debug == '2' && SUCCEEDED(layer->QueryInterface(IID_PPV_ARGS(&layer1)));
                if (gbv)
                    layer1->SetEnableGPUBasedValidation(TRUE);
                release(layer1);
                std::fprintf(stderr, "[native render] the D3D12 debug layer is on%s\n", gbv ? ", with GPU-based validation" : "");
            }
            release(layer);
        }
        // TF_NATIVE_DRED=1: device-removed breadcrumbs and page faults (no debug layer needed).
        if (std::getenv("TF_NATIVE_DRED"))
        {
            ID3D12DeviceRemovedExtendedDataSettings *dred = nullptr;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred))))
            {
                dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            }
            release(dred);
        }
        if (const HRESULT made = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.device)); FAILED(made))
        {
            // (With the debug layer on, a fullscreen window's start makes the device fail with
            // DXGI_ERROR_DEVICE_RESET, 887a0007 - debug runs take PS2X_FULLSCREEN=0.)
            std::fprintf(stderr, "[native render] no Direct3D 12 device (%08lx)%s\n", static_cast<unsigned long>(made),
                         debugging ? " - with the debug layer, run windowed (PS2X_FULLSCREEN=0)" : "");
            return false;
        }
        // (TODO 163, with TF_NATIVE_DRED) A watch on the device every 50 ms: a GPU fault the OpenGL driver ends the process
        // over within seconds (the test PC's NVIDIA driver: nvoglv64 abort()) is told with DRED's last operations and
        // the faulting address before that - the renderer's own check (a frame's end) never came to it.
        if (std::getenv("TF_NATIVE_DRED"))
            std::thread([] {
                for (;;)
                {
                    Sleep(50);
                    const HRESULT removed = g.device->GetDeviceRemovedReason();
                    if (SUCCEEDED(removed))
                        continue;
                    std::fprintf(stderr, "[native render] DRED watch: the device was removed (%08lx)\n", static_cast<unsigned long>(removed));
                    ID3D12DeviceRemovedExtendedData *dred = nullptr;
                    if (SUCCEEDED(g.device->QueryInterface(IID_PPV_ARGS(&dred))))
                    {
                        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs{};
                        if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&crumbs)))
                            for (const D3D12_AUTO_BREADCRUMB_NODE *n = crumbs.pHeadAutoBreadcrumbNode; n; n = n->pNext)
                            {
                                const UINT done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0u;
                                if (done >= n->BreadcrumbCount)
                                    continue; // (a list the GPU finished)
                                std::fprintf(stderr, "[native render]   DRED: list %p (%ls) on queue %p (%ls): %u ops, %u done\n",
                                             static_cast<const void *>(n->pCommandList), n->pCommandListDebugNameW ? n->pCommandListDebugNameW : L"",
                                             static_cast<const void *>(n->pCommandQueue), n->pCommandQueueDebugNameW ? n->pCommandQueueDebugNameW : L"",
                                             n->BreadcrumbCount, done);
                                for (UINT k = (done > 8u ? done - 8u : 0u); k < n->BreadcrumbCount && k < done + 4u; ++k)
                                    std::fprintf(stderr, "[native render]     %s op %u: %d\n", k == done ? ">>" : "  ", k, static_cast<int>(n->pCommandHistory[k]));
                            }
                        D3D12_DRED_PAGE_FAULT_OUTPUT fault{};
                        if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&fault)))
                        {
                            std::fprintf(stderr, "[native render]   DRED: page fault at %llx\n", static_cast<unsigned long long>(fault.PageFaultVA));
                            for (const D3D12_DRED_ALLOCATION_NODE *a = fault.pHeadExistingAllocationNode; a; a = a->pNext)
                                std::fprintf(stderr, "[native render]     near a live allocation of type %d '%ls'\n", static_cast<int>(a->AllocationType),
                                             a->ObjectNameW ? a->ObjectNameW : L"");
                            for (const D3D12_DRED_ALLOCATION_NODE *a = fault.pHeadRecentFreedAllocationNode; a; a = a->pNext)
                                std::fprintf(stderr, "[native render]     near a freed allocation of type %d '%ls'\n", static_cast<int>(a->AllocationType),
                                             a->ObjectNameW ? a->ObjectNameW : L"");
                        }
                        dred->Release();
                    }
                    std::fflush(stderr);
                    return;
                }
            }).detach();
        if (debugging)
        {
            ID3D12InfoQueue1 *queue = nullptr;
            if (SUCCEEDED(g.device->QueryInterface(IID_PPV_ARGS(&queue))))
            {
                DWORD cookie = 0;
                // (Each message id told its first three times, then its 10th, 100th, 1000th...: one
                // message repeated every draw no longer hides the rarer ones behind a total cap.)
                queue->RegisterMessageCallback(
                    [](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id, LPCSTR text, void *) {
                        if (severity > D3D12_MESSAGE_SEVERITY_WARNING)
                            return;
                        static std::mutex mutex;
                        static std::unordered_map<int, uint64_t> seen;
                        uint64_t n = 0;
                        {
                            std::lock_guard<std::mutex> lock(mutex);
                            n = ++seen[static_cast<int>(id)];
                        }
                        bool power = n >= 10u;
                        for (uint64_t k = n; power && k > 1u; k /= 10u)
                            power = k % 10u == 0u;
                        if (n <= 3u || power) // (then at 10, 100, 1000... with the count)
                            std::fprintf(stderr, "[native render] d3d12 %s %d (#%llu): %s\n",
                                         severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ? "CORRUPTION"
                                         : severity <= D3D12_MESSAGE_SEVERITY_ERROR    ? "error"
                                                                                       : "warning",
                                         static_cast<int>(id), static_cast<unsigned long long>(n), text);
                    },
                    D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
                // (kept: the callback lives as long as the device)
            }
            else
                std::fprintf(stderr, "[native render] no debug message queue (the debug layer is not installed?)\n");
        }
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(g.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue))) ||
            FAILED(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.allocator))) ||
            FAILED(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.allocator, nullptr, IID_PPV_ARGS(&g.list))) ||
            FAILED(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence))))
        {
            std::fprintf(stderr, "[native render] Direct3D 12 queue objects could not be made\n");
            return false;
        }
        g.list->Close();
        g.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g.allocators[0] = g.allocator;
        g.frameLists[0] = g.list;
        for (UINT f = 1; f < State::kFrames; ++f)
        {
            if (FAILED(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.allocators[f]))) ||
                FAILED(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.allocators[f], nullptr, IID_PPV_ARGS(&g.frameLists[f]))))
                return false;
            g.frameLists[f]->Close();
        }
        // (N9 stage 3: the pictures' own small lists - their allocators a slot)
        for (UINT f = 0; f < State::kFrames; ++f)
            if (FAILED(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.picAllocators[f]))))
                return false;
        for (UINT f = 0; f < State::kFrames; ++f)
        {
            std::vector<ID3D12GraphicsCommandList **> lists{&g.restoreLists[f]};
            for (UINT k = 0; k < kMaxPictures; ++k)
                lists.push_back(&g.proLists[f][k]), lists.push_back(&g.tailLists[f][k]);
            for (ID3D12GraphicsCommandList **l : lists)
            {
                if (FAILED(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.picAllocators[f], nullptr, IID_PPV_ARGS(l))))
                    return false;
                (*l)->Close();
            }
        }

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.NumDescriptors = 64; // (slot 0: the picture; the render targets after it)
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.rtvHeap));
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hd.NumDescriptors = 128; // (each target's view, then its read-only one at +64)
        g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.dsvHeap));
        hd.NumDescriptors = 64;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kSlots;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.srvHeap));
        g.srvSize = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        g.rtvSize = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        g.dsvSize = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        for (UINT i = kSlots; i-- > 1;)
            g.freeSlots.push_back(i);

        // The root signature: the draw's constants as root constants (b0).
        D3D12_ROOT_PARAMETER params[5]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &range;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        for (int k = 0; k < 2; ++k)
        {
            ranges[k].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            ranges[k].NumDescriptors = 1;
            ranges[k].BaseShaderRegister = 1u + static_cast<UINT>(k); // t1 the palette, t2 the depth
            params[2 + k].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[2 + k].DescriptorTable.NumDescriptorRanges = 1;
            params[2 + k].DescriptorTable.pDescriptorRanges = &ranges[k];
            params[2 + k].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        }
        // (Bilinear, wrapping: the GS's CLAMP and filter registers come with the render state.)
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_ANISOTROPIC; // (16x, with the mip chains: the upscaled textures stay sharp at an angle)
        // (TF_NATIVE_ANISO=<1..16>: another anisotropy; else the quality preset's - 4, 8, 16, 16.)
        {
            static const UINT kPresetAniso[4] = {4u, 8u, 16u, 16u};
            sampler.MaxAnisotropy = std::getenv("TF_NATIVE_ANISO") ? static_cast<UINT>(std::clamp(std::atoi(std::getenv("TF_NATIVE_ANISO")), 1, 16))
                                                                   : kPresetAniso[qualityPreset()];
        }
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        // (TEX1: a level the GS picks, read with its texels linear, s1, or nearest, s2.)
        D3D12_STATIC_SAMPLER_DESC samplers[6] = {sampler, sampler, sampler, sampler, sampler, sampler};
        // (s4, s5: SMAA's - linear and point, clamped, no mips: kSmaaPrelude.)
        for (int k = 4; k < 6; ++k)
        {
            samplers[k].Filter = k == 4 ? D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_POINT;
            samplers[k].MaxAnisotropy = 1;
            samplers[k].AddressU = samplers[k].AddressV = samplers[k].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplers[k].MaxLOD = 0.0f;
            samplers[k].ShaderRegister = static_cast<UINT>(k);
        }
        // (s3: the anisotropic sampler at TF_NATIVE_ANISO_ALT=<n>'s anisotropy, taken by turns with s0's in
        // the profile's windows - an A/B of the anisotropy's cost in one run.)
        samplers[3].MaxAnisotropy = std::getenv("TF_NATIVE_ANISO_ALT") ? static_cast<UINT>(std::clamp(std::atoi(std::getenv("TF_NATIVE_ANISO_ALT")), 1, 16)) : 16u;
        samplers[3].ShaderRegister = 3;
        samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[1].MaxAnisotropy = 1;
        samplers[1].ShaderRegister = 1;
        samplers[2].Filter = D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR;
        samplers[2].MaxAnisotropy = 1;
        samplers[2].ShaderRegister = 2;
        D3D12_ROOT_SIGNATURE_DESC rsd{};
        // (N9 stage 3: b1, the picture's constants - PictureConstants)
        params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[4].Descriptor.ShaderRegister = 1;
        params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rsd.NumParameters = 5;
        rsd.pParameters = params;
        rsd.NumStaticSamplers = 6;
        rsd.pStaticSamplers = samplers;
        rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ID3DBlob *rootBlob = nullptr, *rootErrors = nullptr;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rootBlob, &rootErrors)) ||
            FAILED(g.device->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(), IID_PPV_ARGS(&g.root))))
        {
            std::fprintf(stderr, "[native render] root signature: %s\n",
                         rootErrors ? static_cast<const char *>(rootErrors->GetBufferPointer()) : "failed");
            release(rootBlob);
            release(rootErrors);
            return false;
        }
        release(rootBlob);
        release(rootErrors);

        const auto shadersStart = std::chrono::steady_clock::now(); // (TODO 164: the start's shaders, timed)
        ID3DBlob *vsCode = compile("vs", "vs_5_1"), *psCode = compile("ps", "ps_5_1");
        if (!vsCode || !psCode)
        {
            release(vsCode);
            release(psCode);
            return false;
        }
        g.vsCode = vsCode;
        g.psCode = psCode;
        if (std::getenv("TF_NATIVE_ZWALT"))
        {
            g.psTiny = compile("psTiny", "ps_5_1");
            g.psInterp = compile("psInterp", "ps_5_1");
        }
        g.gsCode = compile("gsClip", "gs_5_1"); // (none: the GPU's clipper alone, as before)
        std::fprintf(stderr, "[native render] shaders: %.0f ms (%u from the shader cache, %u compiled and kept)\n",
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - shadersStart).count(), cache::g_shaderHits.load(),
                     cache::g_shaderMade.load());
        plib::open(); // (TODO 164: the pipelines kept on disk, the ones used before made ahead)
        plib::warm();
        {
            const D3D12_HEAP_PROPERTIES up = heap(D3D12_HEAP_TYPE_UPLOAD);
            const D3D12_RESOURCE_DESC bd = bufferDesc(kConstantRing * State::kFrames);
            void *mapped = nullptr;
            const D3D12_RANGE none{0, 0};
            if (FAILED(g.device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&g.constants))) ||
                FAILED(g.constants->Map(0, &none, &mapped)))
            {
                std::fprintf(stderr, "[native render] no constant buffer\n");
                return false;
            }
            g.constantsMapped = static_cast<uint8_t *>(mapped);
            const D3D12_RESOURCE_DESC ibd = bufferDesc(static_cast<UINT64>(kInstanceRing) * kInstanceStride * State::kFrames);
            void *imapped = nullptr;
            if (FAILED(g.device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &ibd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&g.instances))) ||
                FAILED(g.instances->Map(0, &none, &imapped)))
            {
                std::fprintf(stderr, "[native render] no instance buffer\n");
                return false;
            }
            g.instancesMapped = static_cast<uint8_t *>(imapped);
            // (Their copy in video memory, TODO 94 - an experiment, OFF: measured by turns, no difference on the RX 5700 XT
            // (8.75 / 8.88 ms). TF_NATIVE_CBVRAM=1: on; =alt: by turns.)
            if (std::getenv("TF_NATIVE_CBVRAM") && (*std::getenv("TF_NATIVE_CBVRAM") == '1' || *std::getenv("TF_NATIVE_CBVRAM") == 'a'))
            {
                const D3D12_HEAP_PROPERTIES gpu = heap(D3D12_HEAP_TYPE_DEFAULT);
                bool ok = SUCCEEDED(g.device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                                      IID_PPV_ARGS(&g.constantsV)));
                for (UINT f = 0; ok && f < State::kFrames; ++f)
                    ok = SUCCEEDED(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.preAllocators[f])));
                ok = ok && SUCCEEDED(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.preAllocators[0], nullptr, IID_PPV_ARGS(&g.preList)));
                if (ok)
                    g.preList->Close();
                else
                    release(g.constantsV);
            }
            const D3D12_RESOURCE_DESC dd = bufferDesc(kDynamicRing * State::kFrames);
            if (FAILED(g.device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&g.dynamic))) ||
                FAILED(g.dynamic->Map(0, &none, &mapped)))
                return false;
            g.dynamicMapped = static_cast<uint8_t *>(mapped);
            // (N9 stage 3: the pictures' constants - video memory, a slot's 256 bytes, and their upload ring)
            const D3D12_HEAP_PROPERTIES gpu = heap(D3D12_HEAP_TYPE_DEFAULT);
            const D3D12_RESOURCE_DESC pcb = bufferDesc(256ull * State::kFrames);
            const D3D12_RESOURCE_DESC pub = bufferDesc(static_cast<UINT64>(kPictureUp) * kMaxPictures * State::kFrames);
            if (FAILED(g.device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &pcb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&g.pictureCB))) ||
                FAILED(g.device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &pub, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&g.pictureUp))) ||
                FAILED(g.pictureUp->Map(0, &none, &mapped)))
            {
                std::fprintf(stderr, "[native render] no picture constants\n");
                return false;
            }
            g.pictureUpMapped = static_cast<uint8_t *>(mapped);
            g.pictureCBState = D3D12_RESOURCE_STATE_COPY_DEST;
            const D3D12_RESOURCE_DESC prd = bufferDesc(8ull * State::kFrames);
            if (FAILED(g.device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &prd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&g.predicate))))
                g.predicate = nullptr; // (every picture draws everything)
            g.predicateState = D3D12_RESOURCE_STATE_COPY_DEST;
        }
        g.pipeline = pipelineFor(BlendKey{}, true, D3D12_COMPARISON_FUNC_GREATER_EQUAL, true);
        const HRESULT hr = g.pipeline ? S_OK : E_FAIL;
        if (FAILED(hr) || !g.rtvHeap || !g.dsvHeap)
        {
            std::fprintf(stderr, "[native render] pipeline state could not be made (%08lx)\n", static_cast<unsigned long>(hr));
            return false;
        }
        // The anti-aliasing pass's pipeline (none: the picture as drawn, told once).
        {
            ID3DBlob *aaVs = compile("aaVs", "vs_5_1", kAaShader, sizeof(kAaShader) - 1), *aaPs = compile("aaPs", "ps_5_1", kAaShader, sizeof(kAaShader) - 1);
            if (aaVs && aaPs)
            {
                D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
                pd.pRootSignature = g.root;
                pd.VS = {aaVs->GetBufferPointer(), aaVs->GetBufferSize()};
                pd.PS = {aaPs->GetBufferPointer(), aaPs->GetBufferSize()};
                D3D12_RENDER_TARGET_BLEND_DESC &rt = pd.BlendState.RenderTarget[0];
                rt.SrcBlend = rt.SrcBlendAlpha = D3D12_BLEND_ONE;
                rt.DestBlend = rt.DestBlendAlpha = D3D12_BLEND_ZERO;
                rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                rt.LogicOp = D3D12_LOGIC_OP_NOOP;
                rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
                pd.SampleMask = 0xFFFFFFFFu;
                pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
                pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                pd.RasterizerState.DepthClipEnable = TRUE;
                pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                pd.NumRenderTargets = 1;
                pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; // (the picture's)
                pd.SampleDesc.Count = 1;
                if (FAILED(g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g.aaPipeline))))
                    g.aaPipeline = nullptr;
            }
            release(aaVs);
            release(aaPs);
            if (!g.aaPipeline)
                std::fprintf(stderr, "[native render] the anti-aliasing pass could not be made: the picture is shown as drawn\n");
            // SMAA 1x's pipelines, lookup textures and their views, two render-target places and four slots kept for it
            // (none: TF_NATIVE_AA=2 draws FXAA, told once).
            {
                std::string src(kSmaaPrelude, sizeof(kSmaaPrelude) - 1);
                for (const char *piece : kSmaaSource)
                    src += piece;
                src.append(kSmaaEntries, sizeof(kSmaaEntries) - 1);
                const char *const names[6] = {"smaaEdgeVs", "smaaEdgePs", "smaaWeightVs", "smaaWeightPs", "smaaBlendVs", "smaaBlendPs"};
                ID3DBlob *code[6] = {};
                bool made = true;
                for (int k = 0; k < 6; ++k)
                {
                    code[k] = compile(names[k], (k & 1) ? "ps_5_1" : "vs_5_1", src.c_str(), src.size());
                    made = made && code[k];
                }
                const DXGI_FORMAT formats[3] = {DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM};
                ID3D12PipelineState **pipes[3] = {&g.smaaEdgePipe, &g.smaaWeightPipe, &g.smaaBlendPipe};
                for (int p = 0; made && p < 3; ++p)
                {
                    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
                    pd.pRootSignature = g.root;
                    pd.VS = {code[2 * p]->GetBufferPointer(), code[2 * p]->GetBufferSize()};
                    pd.PS = {code[2 * p + 1]->GetBufferPointer(), code[2 * p + 1]->GetBufferSize()};
                    D3D12_RENDER_TARGET_BLEND_DESC &rt = pd.BlendState.RenderTarget[0];
                    rt.SrcBlend = rt.SrcBlendAlpha = D3D12_BLEND_ONE;
                    rt.DestBlend = rt.DestBlendAlpha = D3D12_BLEND_ZERO;
                    rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                    rt.LogicOp = D3D12_LOGIC_OP_NOOP;
                    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
                    pd.SampleMask = 0xFFFFFFFFu;
                    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
                    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                    pd.RasterizerState.DepthClipEnable = TRUE;
                    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                    pd.NumRenderTargets = 1;
                    pd.RTVFormats[0] = formats[p];
                    pd.SampleDesc.Count = 1;
                    if (FAILED(g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(pipes[p]))))
                    {
                        *pipes[p] = nullptr;
                        made = false;
                    }
                }
                for (ID3DBlob *&c : code)
                    release(c);
                auto takeRtv = []() -> UINT {
                    if (!g.freeRtvs.empty())
                    {
                        const UINT r = g.freeRtvs.back();
                        g.freeRtvs.pop_back();
                        return r;
                    }
                    return g.nextTarget < 64u ? g.nextTarget++ : 0u;
                };
                auto takeSlot = []() -> UINT {
                    if (g.freeSlots.empty())
                        return 0u;
                    const UINT s = g.freeSlots.back();
                    g.freeSlots.pop_back();
                    return s;
                };
                const D3D12_HEAP_PROPERTIES gpu = heap(D3D12_HEAP_TYPE_DEFAULT);
                auto lookup = [&](UINT w, UINT h, DXGI_FORMAT f, ID3D12Resource *&res, UINT &slot) {
                    D3D12_RESOURCE_DESC td{};
                    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                    td.Width = w;
                    td.Height = h;
                    td.DepthOrArraySize = 1;
                    td.MipLevels = 1;
                    td.Format = f;
                    td.SampleDesc.Count = 1;
                    if (FAILED(g.device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                 IID_PPV_ARGS(&res))))
                    {
                        res = nullptr;
                        return false;
                    }
                    if (!(slot = takeSlot()))
                        return false;
                    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
                    sd.Format = f;
                    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    sd.Texture2D.MipLevels = 1;
                    D3D12_CPU_DESCRIPTOR_HANDLE hd = g.srvHeap->GetCPUDescriptorHandleForHeapStart();
                    hd.ptr += static_cast<SIZE_T>(slot) * g.srvSize;
                    srvWritten(slot, "SMAA's lookup texture's");
                    g.device->CreateShaderResourceView(res, &sd, hd);
                    return true;
                };
                made = made && lookup(AREATEX_WIDTH, AREATEX_HEIGHT, DXGI_FORMAT_R8G8_UNORM, g.smaaArea, g.smaaAreaSlot) &&
                       lookup(SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, DXGI_FORMAT_R8_UNORM, g.smaaSearch, g.smaaSearchSlot) &&
                       (g.smaaEdgesSlot = takeSlot()) != 0u && (g.smaaWeightsSlot = takeSlot()) != 0u && (g.smaaEdgesRtv = takeRtv()) != 0u &&
                       (g.smaaWeightsRtv = takeRtv()) != 0u;
                if (!made)
                {
                    // (Its places and slots stay taken: a few of 64 and of thousands, once.)
                    release(g.smaaEdgePipe);
                    release(g.smaaWeightPipe);
                    release(g.smaaBlendPipe);
                    release(g.smaaArea);
                    release(g.smaaSearch);
                    std::fprintf(stderr, "[native render] SMAA could not be made: TF_NATIVE_AA=2 draws FXAA\n");
                }
            }
            // The sun's occlusion probe (roadmap N3a; none: the game's read-back of the GS stays).
            {
                ID3DBlob *pv = compile("probeVs", "vs_5_1", kProbeShader, sizeof(kProbeShader) - 1),
                         *pp = compile("probePs", "ps_5_1", kProbeShader, sizeof(kProbeShader) - 1);
                if (pv && pp && (!g.freeRtvs.empty() || g.nextTarget < 64u))
                {
                    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
                    pd.pRootSignature = g.root;
                    pd.VS = {pv->GetBufferPointer(), pv->GetBufferSize()};
                    pd.PS = {pp->GetBufferPointer(), pp->GetBufferSize()};
                    D3D12_RENDER_TARGET_BLEND_DESC &rt = pd.BlendState.RenderTarget[0];
                    rt.SrcBlend = rt.SrcBlendAlpha = D3D12_BLEND_ONE;
                    rt.DestBlend = rt.DestBlendAlpha = D3D12_BLEND_ZERO;
                    rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                    rt.LogicOp = D3D12_LOGIC_OP_NOOP;
                    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
                    pd.SampleMask = 0xFFFFFFFFu;
                    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
                    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                    pd.RasterizerState.DepthClipEnable = TRUE;
                    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                    pd.NumRenderTargets = 1;
                    pd.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
                    pd.SampleDesc.Count = 1;
                    D3D12_RESOURCE_DESC td{};
                    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                    td.Width = 64u;
                    td.Height = 64u;
                    td.DepthOrArraySize = 1;
                    td.MipLevels = 1;
                    td.Format = DXGI_FORMAT_R32_FLOAT;
                    td.SampleDesc.Count = 1;
                    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                    D3D12_CLEAR_VALUE clear{};
                    clear.Format = td.Format;
                    const D3D12_HEAP_PROPERTIES gpu = heap(D3D12_HEAP_TYPE_DEFAULT), back = heap(D3D12_HEAP_TYPE_READBACK);
                    const D3D12_RESOURCE_DESC rb = bufferDesc(16384ull * State::kFrames);
                    if (FAILED(g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g.probePipeline))) ||
                        FAILED(g.device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                                                 IID_PPV_ARGS(&g.probeTarget))) ||
                        FAILED(g.device->CreateCommittedResource(&back, D3D12_HEAP_FLAG_NONE, &rb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                 IID_PPV_ARGS(&g.probeReadback))))
                    {
                        release(g.probePipeline);
                        release(g.probeTarget);
                        release(g.probeReadback);
                    }
                    else
                    {
                        if (!g.freeRtvs.empty())
                        {
                            g.probeRtv = g.freeRtvs.back();
                            g.freeRtvs.pop_back();
                        }
                        else
                            g.probeRtv = g.nextTarget++;
                        D3D12_CPU_DESCRIPTOR_HANDLE h = g.rtvHeap->GetCPUDescriptorHandleForHeapStart();
                        h.ptr += static_cast<SIZE_T>(g.probeRtv) * g.rtvSize;
                        g.device->CreateRenderTargetView(g.probeTarget, nullptr, h);
                    }
                }
                release(pv);
                release(pp);
                if (!g.probePipeline)
                    std::fprintf(stderr, "[native render] the sun's occlusion probe could not be made: the game reads the GS's\n");
            }
            // (TODO 27) The warp tunnel's sparks: the points of a set as instances (their x y z the per-instance stream), four
            // corners each (a strip), added to the picture - its colour only (none: they are left out, told once).
            {
                ID3DBlob *cv = compile("cloudVs", "vs_5_1", kCloudShader, sizeof(kCloudShader) - 1),
                         *cp = compile("cloudPs", "ps_5_1", kCloudShader, sizeof(kCloudShader) - 1);
                if (cv && cp)
                {
                    static const D3D12_INPUT_ELEMENT_DESC cloudElements[] = {
                        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, x), D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1}};
                    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
                    pd.pRootSignature = g.root;
                    pd.VS = {cv->GetBufferPointer(), cv->GetBufferSize()};
                    pd.PS = {cp->GetBufferPointer(), cp->GetBufferSize()};
                    pd.InputLayout = {cloudElements, 1u};
                    D3D12_RENDER_TARGET_BLEND_DESC &rt = pd.BlendState.RenderTarget[0];
                    rt.BlendEnable = TRUE;
                    rt.SrcBlend = rt.SrcBlendAlpha = D3D12_BLEND_ONE;
                    rt.DestBlend = rt.DestBlendAlpha = D3D12_BLEND_ONE;
                    rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                    rt.LogicOp = D3D12_LOGIC_OP_NOOP;
                    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
                    pd.SampleMask = 0xFFFFFFFFu;
                    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
                    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                    pd.RasterizerState.DepthClipEnable = TRUE;
                    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                    pd.NumRenderTargets = 1;
                    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; // (the picture's)
                    pd.SampleDesc.Count = 1;
                    if (FAILED(g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g.cloudPipeline))))
                        g.cloudPipeline = nullptr;
                }
                release(cv);
                release(cp);
                if (!g.cloudPipeline)
                    std::fprintf(stderr, "[native render] the warp tunnel's sparks could not be made: they are left out\n");
            }
            D3D12_QUERY_HEAP_DESC qh{};
            qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            qh.Count = 4u * State::kFrames; // (the anti-aliasing's two a slot, then the whole frame's two)
            const D3D12_HEAP_PROPERTIES back = heap(D3D12_HEAP_TYPE_READBACK);
            const D3D12_RESOURCE_DESC tb = bufferDesc(sizeof(UINT64) * 4u * State::kFrames);
            if (FAILED(g.queue->GetTimestampFrequency(&g.timestampHz)) || FAILED(g.device->CreateQueryHeap(&qh, IID_PPV_ARGS(&g.aaQueries))) ||
                FAILED(g.device->CreateCommittedResource(&back, D3D12_HEAP_FLAG_NONE, &tb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g.aaTimes))))
            {
                release(g.aaQueries);
                release(g.aaTimes);
            }
            if (g.timestampHz)
            {
                // (N9 stage 3: each picture's two timestamps - the pictures' cap)
                D3D12_QUERY_HEAP_DESC ph{};
                ph.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
                ph.Count = kPicStride * State::kFrames;
                const D3D12_RESOURCE_DESC pb = bufferDesc(sizeof(UINT64) * kPicStride * State::kFrames);
                if (FAILED(g.device->CreateQueryHeap(&ph, IID_PPV_ARGS(&g.picQueries))) ||
                    FAILED(g.device->CreateCommittedResource(&back, D3D12_HEAP_FLAG_NONE, &pb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g.picTimes))))
                {
                    release(g.picQueries);
                    release(g.picTimes);
                }
            }
            if (std::getenv("TF_NATIVE_PASSPROFILE") && g.timestampHz)
            {
                D3D12_QUERY_HEAP_DESC ph{};
                ph.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
                ph.Count = State::kPassMarks * State::kFrames;
                const D3D12_RESOURCE_DESC pb = bufferDesc(sizeof(UINT64) * State::kPassMarks * State::kFrames);
                if (FAILED(g.device->CreateQueryHeap(&ph, IID_PPV_ARGS(&g.passQueries))) ||
                    FAILED(g.device->CreateCommittedResource(&back, D3D12_HEAP_FLAG_NONE, &pb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g.passTimes))))
                {
                    release(g.passQueries);
                    release(g.passTimes);
                }
                if (g.passQueries && std::getenv("TF_NATIVE_DRAWCOST") && std::getenv("TF_NATIVE_DRAWSTATS"))
                {
                    D3D12_QUERY_HEAP_DESC sh{};
                    sh.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
                    sh.Count = State::kPassMarks * State::kFrames;
                    const D3D12_RESOURCE_DESC sb = bufferDesc(sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS) * State::kPassMarks * State::kFrames);
                    if (FAILED(g.device->CreateQueryHeap(&sh, IID_PPV_ARGS(&g.statQueries))) ||
                        FAILED(g.device->CreateCommittedResource(&back, D3D12_HEAP_FLAG_NONE, &sb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g.statData))))
                    {
                        release(g.statQueries);
                        release(g.statData);
                    }
                }
            }
        }
        // (grassgpu: the grass's compute pass - TF_GRASS_GPU=0 keeps the grass on the CPU)
        if (!(std::getenv("TF_GRASS_GPU") && *std::getenv("TF_GRASS_GPU") == '0'))
            grassgpu::g_ready.store(grassgpu::init(), std::memory_order_relaxed);
        g.failed = false;
        std::fprintf(stderr, "[native render] Direct3D 12 ready\n");
        return true;
    }

    namespace
    {
        D3D12_CPU_DESCRIPTOR_HANDLE rtvAt(UINT slot)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE h = g.rtvHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += static_cast<SIZE_T>(slot) * g.rtvSize;
            return h;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE dsvAt(UINT slot)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE h = g.dsvHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += static_cast<SIZE_T>(slot) * g.dsvSize;
            return h;
        }

        // ---- (P3 S3) The D3D12 device's targets: their surfaces, views and states (nr_device.h's targets part) ----
        State::DeviceTarget &dt(gfx::TargetId id) { return g.devTargets[id]; }
        State::DeviceTarget &dt(const State::Target &t) { return g.devTargets[t.id]; }

        void srvFor(State::DeviceTarget &t)
        {
            if (!t.srv)
            {
                if (g.freeSlots.empty())
                    return;
                t.srv = g.freeSlots.back();
                g.freeSlots.pop_back();
                t.srvOf = nullptr;
            }
            if (t.srvOf == t.colour)
                return;
            // (Written again whenever the target's resource is: a grown target is a new resource, and
            // a view of the old one would read memory already released - the GPU hangs on it.)
            t.srvOf = t.colour;
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = t.colour->GetDesc().Format;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE h = g.srvHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += static_cast<SIZE_T>(t.srv) * g.srvSize;
            srvWritten(t.srv, "a target's");
            g.device->CreateShaderResourceView(t.colour, &sd, h);
        }

        // A render target's surfaces (BGRA colour and a readable depth), cleared to transparent black: new, or a
        // grown one's again (same views' places).
        bool makeSurfaces(State::DeviceTarget &t, UINT w, UINT h)
        {
            if (!t.rtv && g.freeRtvs.empty() && g.nextTarget >= 64u)
                return false;
            t.w = w;
            t.h = h;
            D3D12_RESOURCE_DESC td{};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = t.w;
            td.Height = t.h;
            td.DepthOrArraySize = 1;
            td.MipLevels = 1;
            td.Format = t.format;
            td.SampleDesc.Count = 1;
            td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE clear{};
            clear.Format = td.Format;
            const D3D12_HEAP_PROPERTIES gpu = heap(D3D12_HEAP_TYPE_DEFAULT);
            t.srvOf = nullptr; // (its shader view written again for the new resource, whatever its address)
            if (FAILED(g.device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                                         IID_PPV_ARGS(&t.colour))))
                return false;
            t.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
            if (t.withDepth)
            {
                // (Typeless, so the depth fog can read it: a depth view to draw with, a read-only one and
                // a shader view for the fog's pass.)
                td.Format = DXGI_FORMAT_R32_TYPELESS;
                td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
                D3D12_CLEAR_VALUE depthClear{};
                depthClear.Format = DXGI_FORMAT_D32_FLOAT;
                if (FAILED(g.device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear,
                                                             IID_PPV_ARGS(&t.depth))))
                {
                    // (Not drawn into at all: with its colour alone, its depth views - a grown target's -
                    // would still name the old depth, released with this frame.)
                    release(t.colour);
                    t.depth = nullptr;
                    return false;
                }
                t.depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            }
            if (!t.rtv)
            {
                // (A grown target keeps its views' places. The views are CPU descriptors, read when a
                // list is recorded: a place given back is used again at once.)
                if (!g.freeRtvs.empty())
                {
                    t.rtv = g.freeRtvs.back();
                    g.freeRtvs.pop_back();
                }
                else
                    t.rtv = g.nextTarget++;
            }
            g.device->CreateRenderTargetView(t.colour, nullptr, rtvAt(t.rtv));
            if (t.withDepth)
            {
                D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
                dv.Format = DXGI_FORMAT_D32_FLOAT;
                dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                g.device->CreateDepthStencilView(t.depth, &dv, dsvAt(t.rtv));
                dv.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
                g.device->CreateDepthStencilView(t.depth, &dv, dsvAt(t.rtv + 64u));
                if (!t.depthSrv && !g.freeSlots.empty())
                {
                    t.depthSrv = g.freeSlots.back();
                    g.freeSlots.pop_back();
                }
                if (t.depthSrv)
                {
                    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
                    sd.Format = DXGI_FORMAT_R32_FLOAT;
                    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    sd.Texture2D.MipLevels = 1;
                    D3D12_CPU_DESCRIPTOR_HANDLE h = g.srvHeap->GetCPUDescriptorHandleForHeapStart();
                    h.ptr += static_cast<SIZE_T>(t.depthSrv) * g.srvSize;
                    srvWritten(t.depthSrv, "a target's depth");
                    g.device->CreateShaderResourceView(t.depth, &sd, h);
                }
            }
            const float black[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            g.list->ClearRenderTargetView(rtvAt(t.rtv), black, 0, nullptr);
            // (Only a target with depth: one made without has no depth view - its place may still name the depth of a
            // target released before, freed once the GPU is done with it; clearing through that crashed in AMD's
            // driver, amdxc64.dll, when the HQ's picture size changed and the anti-aliasing's copy was made again.)
            if (t.withDepth && t.depth)
                g.list->ClearDepthStencilView(dsvAt(t.rtv), D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
            t.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
            return true;
        }

        void depthStateOf(State::DeviceTarget &t, D3D12_RESOURCE_STATES to)
        {
            if (!t.depth || t.depthState == to)
                return;
            transition(t.depth, t.depthState, to);
            t.depthState = to;
        }

        void stateOf(State::DeviceTarget &t, D3D12_RESOURCE_STATES to)
        {
            if (t.state != to)
            {
                transition(t.colour, t.state, to);
                t.state = to;
            }
        }

        // TEST's DATE (the GS's destination alpha test): the target's colour as it is now, copied for
        // the draw to read its alpha (a target cannot be read while drawn into). Its view's slot, or 0.
        UINT destCopyOf(State::DeviceTarget &t)
        {
            if (!t.colour)
                return 0u;
            const D3D12_RESOURCE_DESC cd = t.colour->GetDesc();
            if (!t.destCopy || t.destCopy->GetDesc().Width != cd.Width || t.destCopy->GetDesc().Height != cd.Height)
            {
                if (t.destCopy)
                    g.uploads.push_back(t.destCopy); // (released once the GPU is done)
                t.destCopy = nullptr;
                if (!t.destSrv)
                {
                    if (g.freeSlots.empty())
                        return 0u;
                    t.destSrv = g.freeSlots.back();
                    g.freeSlots.pop_back();
                }
                D3D12_RESOURCE_DESC td = cd;
                td.Flags = D3D12_RESOURCE_FLAG_NONE;
                const D3D12_HEAP_PROPERTIES gpu = heap(D3D12_HEAP_TYPE_DEFAULT);
                if (FAILED(g.device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                             IID_PPV_ARGS(&t.destCopy))))
                    return 0u;
                t.destState = D3D12_RESOURCE_STATE_COPY_DEST;
                D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.Format = td.Format;
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sd.Texture2D.MipLevels = 1;
                D3D12_CPU_DESCRIPTOR_HANDLE h = g.srvHeap->GetCPUDescriptorHandleForHeapStart();
                h.ptr += static_cast<SIZE_T>(t.destSrv) * g.srvSize;
                srvWritten(t.destSrv, "a target copy's");
                g.device->CreateShaderResourceView(t.destCopy, &sd, h);
            }
            ++g_cntCopies;
            const D3D12_RESOURCE_STATES was = t.state;
            stateOf(t, D3D12_RESOURCE_STATE_COPY_SOURCE);
            if (t.destState != D3D12_RESOURCE_STATE_COPY_DEST)
                transition(t.destCopy, t.destState, D3D12_RESOURCE_STATE_COPY_DEST);
            g.list->CopyResource(t.destCopy, t.colour);
            transition(t.destCopy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            t.destState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            stateOf(t, was);
            return t.destSrv;
        }

        D3D12_RESOURCE_STATES stateFor(gfx::UseFor how)
        {
            switch (how)
            {
            case gfx::UseFor::Draw: return D3D12_RESOURCE_STATE_RENDER_TARGET;
            case gfx::UseFor::Sample: return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            case gfx::UseFor::CopySource: return D3D12_RESOURCE_STATE_COPY_SOURCE;
            case gfx::UseFor::CopyDest: return D3D12_RESOURCE_STATE_COPY_DEST;
            }
            return D3D12_RESOURCE_STATE_COMMON;
        }

        // (P3 S3) The D3D12 implementation of nr::gfx::Device. Sub-step 1: the targets; the rest of the interface is
        // still called directly in render() and is routed through here in the next sub-steps (its calls here are
        // placeholders, not used yet).
        bool smaaLookups(); // (below: SMAA's tables uploaded once)

        // A target's colour format.
        DXGI_FORMAT formatOf(gfx::Format f)
        {
            switch (f)
            {
            case gfx::Format::RGBA8: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case gfx::Format::RG8: return DXGI_FORMAT_R8G8_UNORM;
            case gfx::Format::R8: return DXGI_FORMAT_R8_UNORM;
            case gfx::Format::R32Float: return DXGI_FORMAT_R32_FLOAT;
            default: return DXGI_FORMAT_B8G8R8A8_UNORM;
            }
        }

        class D3d12Device final : public gfx::Device
        {
        public:
            // --- targets ---
            gfx::TargetId createTarget(const gfx::TargetDesc &d) override
            {
                gfx::TargetId id;
                if (!g.freeTargetIds.empty())
                {
                    id = g.freeTargetIds.back();
                    g.freeTargetIds.pop_back();
                }
                else
                {
                    id = static_cast<gfx::TargetId>(g.devTargets.size());
                    g.devTargets.emplace_back();
                }
                State::DeviceTarget &t = g.devTargets[id];
                t = State::DeviceTarget{};
                if (d.shared)
                {
                    // The picture: its own surfaces (made with the window's size, makeTargets), view place 0.
                    t.picture = true;
                    t.colour = g.colour;
                    t.depth = g.depth;
                    t.rtv = 0;
                    t.w = d.width;
                    t.h = d.height;
                    return id;
                }
                t.format = formatOf(d.colour);
                t.withDepth = d.depth;
                if (!makeSurfaces(t, d.width, d.height))
                {
                    t = State::DeviceTarget{};
                    g.freeTargetIds.push_back(id);
                    return 0;
                }
                return id;
            }

            bool resizeTarget(gfx::TargetId id, const gfx::TargetDesc &d) override
            {
                State::DeviceTarget &t = dt(id);
                if (t.colour)
                {
                    waitIdle(); // (its views are rewritten: no frame may be reading them)
                    g.uploads.push_back(t.colour); // (released once the GPU is done)
                    g.uploads.push_back(t.depth);
                    t.colour = t.depth = nullptr;
                }
                t.format = formatOf(d.colour);
                t.withDepth = d.depth;
                return makeSurfaces(t, d.width, d.height);
            }

            void releaseTarget(gfx::TargetId id) override
            {
                if (!id)
                    return;
                State::DeviceTarget &t = dt(id);
                if (!t.picture)
                {
                    if (t.colour)
                        g.uploads.push_back(t.colour); // (a buffer of its own: released once the GPU is done)
                    if (t.depth)
                        g.uploads.push_back(t.depth);
                    if (t.rtv)
                        g.freeRtvs.push_back(t.rtv);
                }
                // (Its views' slots given back once this frame's fence has passed - the frame before
                // may still read them - and its copy for DATE with them; given back at once, a texture
                // made this frame rewrote the slot under the frame in flight.)
                for (const UINT s : {t.srv, t.depthSrv, t.destSrv})
                    if (s)
                        g.slotFreeSlots[g.slot].push_back(s);
                if (t.destCopy)
                    g.uploads.push_back(t.destCopy);
                t = State::DeviceTarget{};
                g.freeTargetIds.push_back(id);
            }

            void clearColour(gfx::TargetId id, const float rgba[4]) override { g.list->ClearRenderTargetView(rtvAt(dt(id).rtv), rgba, 0, nullptr); }
            void clearDepth(gfx::TargetId id, float value) override
            {
                const State::DeviceTarget &t = dt(id);
                if (t.depth) // (a target made without depth: none to clear)
                    g.list->ClearDepthStencilView(dsvAt(t.rtv), D3D12_CLEAR_FLAG_DEPTH, value, 0, 0, nullptr);
            }
            void use(gfx::TargetId id, gfx::UseFor how) override { stateOf(dt(id), stateFor(how)); }
            void useDepth(gfx::TargetId id, gfx::DepthUse how) override
            {
                depthStateOf(dt(id), how == gfx::DepthUse::Write ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                                                                  : D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
            // (A target's view or snapshot as a TextureId: its descriptor slot with kViewBit - the textures' ids are their table's.)
            static constexpr gfx::TextureId kViewBit = 0x80000000u;
            static gfx::TextureId asView(UINT slot) { return slot ? (slot | kViewBit) : 0u; }
            static UINT slotOf(gfx::TextureId id) { return (id & kViewBit) ? (id & ~kViewBit) : g.devTextures[id].slot; }
            gfx::TextureId view(gfx::TargetId id, bool depth) override
            {
                State::DeviceTarget &t = dt(id);
                if (depth)
                    return asView(t.depthSrv);
                if (t.colour)
                    srvFor(t);
                return asView(t.srv);
            }
            gfx::TextureId snapshot(gfx::TargetId id) override { return asView(destCopyOf(dt(id))); }

            // --- textures (0: the white one, slot 0) ---
            gfx::TextureId createTexture(uint32_t width, uint32_t height, gfx::Format format, uint32_t levels) override
            {
                gfx::TextureId id;
                if (!g.freeTextureIds.empty())
                {
                    id = g.freeTextureIds.back();
                    g.freeTextureIds.pop_back();
                }
                else
                {
                    id = static_cast<gfx::TextureId>(g.devTextures.size());
                    g.devTextures.emplace_back();
                }
                State::DeviceTexture &t = g.devTextures[id];
                const uint32_t generation = t.generation + 1u;
                t = State::DeviceTexture{};
                t.generation = generation;
                t.width = width;
                t.height = height;
                t.levels = levels;
                t.format = format;
                return id;
            }

            // A level's texels (RGBA8 rows of rowBytes; this frame's list). The texture is made once its last level is given:
            // the levels' texels must stay where they are until then.
            bool uploadTexture(gfx::TextureId id, uint32_t level, const void *texels, uint32_t rowBytes) override
            {
                State::DeviceTexture &t = g.devTextures[id];
                if (t.ready || t.format != gfx::Format::RGBA8 || level >= t.levels || rowBytes != std::max(1u, t.width >> level) * 4u)
                    return false;
                if (t.given.size() < t.levels)
                    t.given.resize(t.levels, nullptr);
                t.given[level] = texels;
                for (const void *p : t.given)
                    if (!p)
                        return true; // (more levels to come)
                std::vector<const uint32_t *> mips;
                for (const void *p : t.given)
                    mips.push_back(static_cast<const uint32_t *>(p));
                std::vector<const void *>().swap(t.given);
                if (g.freeSlots.empty())
                    return false;
                const UINT slot = g.freeSlots.back();
                t.resource = makeTextureLevels(t.width, t.height, mips, slot);
                if (!t.resource)
                    return false;
                g.freeSlots.pop_back();
                t.slot = slot;
                const D3D12_RESOURCE_DESC rd = t.resource->GetDesc();
                t.bytes = g.device->GetResourceAllocationInfo(0, 1, &rd).SizeInBytes;
                t.ready = true;
                return true;
            }

            bool uploadTextureLater(gfx::TextureId id, std::weak_ptr<const Texture> source, bool urgent) override
            {
                std::shared_ptr<const Texture> t = source.lock();
                return t && up::ask(t, id, g.devTextures[id].generation, urgent);
            }

            bool textureReady(gfx::TextureId id) override { return id == 0 || g.devTextures[id].ready; }

            void releaseTexture(gfx::TextureId id) override
            {
                if (!id)
                    return;
                State::DeviceTexture &t = g.devTextures[id];
                if (t.resource)
                    g.uploads.push_back(t.resource); // (let go of once this frame's fence is passed)
                if (t.slot)
                    g.slotFreeSlots[g.slot].push_back(t.slot);
                const uint32_t generation = t.generation + 1u; // (an upload still on its way finds another generation)
                t = State::DeviceTexture{};
                t.generation = generation;
                g.freeTextureIds.push_back(id);
            }

            // --- meshes ---
            uint32_t createMesh(const void *vertices, uint32_t vertexBytes, const void *indices, uint32_t indexBytes) override
            {
                const D3D12_HEAP_PROPERTIES up = heap(D3D12_HEAP_TYPE_UPLOAD);
                const D3D12_RESOURCE_DESC bd = bufferDesc(static_cast<UINT64>(vertexBytes) + indexBytes);
                State::DeviceMesh m;
                if (FAILED(g.device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                             IID_PPV_ARGS(&m.buffer))))
                    return 0;
                void *p = nullptr;
                const D3D12_RANGE none{0, 0};
                if (FAILED(m.buffer->Map(0, &none, &p)))
                {
                    release(m.buffer);
                    return 0;
                }
                std::memcpy(p, vertices, vertexBytes);
                std::memcpy(static_cast<uint8_t *>(p) + vertexBytes, indices, indexBytes);
                m.buffer->Unmap(0, nullptr);
                m.vbv = {m.buffer->GetGPUVirtualAddress(), vertexBytes, sizeof(Vertex)};
                m.ibv = {m.buffer->GetGPUVirtualAddress() + vertexBytes, indexBytes, DXGI_FORMAT_R32_UINT};
                // (TODO 94, an experiment, OFF: the mesh copied into video memory - the GPU's frame cost the same at 960 x 540
                // as at 1920 x 1080 (8.2 ms on the RX 5700 XT), so the vertices read over the bus were a suspect; measured by
                // turns, no difference (8.14 / 8.08 ms). TF_NATIVE_MESHVRAM=1: on; =alt: by turns, a profile window each.)
                static const char *meshVramEnv = std::getenv("TF_NATIVE_MESHVRAM");
                if (meshVramEnv && (*meshVramEnv == '1' || *meshVramEnv == 'a'))
                {
                    const D3D12_HEAP_PROPERTIES gpu = heap(D3D12_HEAP_TYPE_DEFAULT);
                    if (SUCCEEDED(g.device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                                    IID_PPV_ARGS(&m.vram))))
                    {
                        g.list->CopyBufferRegion(m.vram, 0, m.buffer, 0, static_cast<UINT64>(vertexBytes) + indexBytes);
                        transition(m.vram, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER);
                        m.vbvV = {m.vram->GetGPUVirtualAddress(), vertexBytes, sizeof(Vertex)};
                        m.ibvV = {m.vram->GetGPUVirtualAddress() + vertexBytes, indexBytes, DXGI_FORMAT_R32_UINT};
                        if (!(meshVramEnv && *meshVramEnv == 'a'))
                        {
                            // (The upload heap's copy no longer read: let go of with this frame.)
                            g.uploads.push_back(m.buffer);
                            m.buffer = nullptr;
                            m.vbv = m.vbvV;
                            m.ibv = m.ibvV;
                        }
                    }
                }
                uint32_t id;
                if (!g.freeMeshIds.empty())
                {
                    id = g.freeMeshIds.back();
                    g.freeMeshIds.pop_back();
                }
                else
                {
                    id = static_cast<uint32_t>(g.devMeshes.size());
                    g.devMeshes.emplace_back();
                }
                g.devMeshes[id] = m;
                return id;
            }

            void releaseMesh(uint32_t id) override
            {
                if (!id)
                    return;
                State::DeviceMesh &m = g.devMeshes[id];
                g.uploads.push_back(m.buffer); // (let go of once this frame's fence is passed)
                if (m.vram)
                    g.uploads.push_back(m.vram);
                m = State::DeviceMesh{};
                g.freeMeshIds.push_back(id);
            }

            // --- the rings (a frame slot's part each; begun again by beginRings) ---
            bool ringAlloc(gfx::RingKind kind, uint32_t size, uint32_t align, gfx::RingSpan &out) override
            {
                align = std::max(align, 1u);
                switch (kind)
                {
                case gfx::RingKind::Constants:
                {
                    const UINT64 at = (g.ringConstantsAt + align - 1u) / align * align;
                    if (!g.constants || at + size > (g.slot + 1ull) * kConstantRing)
                        return false;
                    out.cpu = g.constantsMapped + at;
                    out.gpu = cbBase() + at;
                    out.size = size;
                    out.first = 0;
                    g.ringConstantsAt = at + size;
                    return true;
                }
                case gfx::RingKind::Instances:
                {
                    // (Entries of kInstanceStride bytes: `first` is the entry's index, the instance stream's.)
                    const UINT entries = (size + kInstanceStride - 1u) / kInstanceStride;
                    if (!g.instances || g.ringInstanceAt + entries > (g.slot + 1u) * kInstanceRing)
                        return false;
                    out.cpu = g.instancesMapped + static_cast<size_t>(g.ringInstanceAt) * kInstanceStride;
                    out.gpu = g.instances->GetGPUVirtualAddress() + static_cast<UINT64>(g.ringInstanceAt) * kInstanceStride;
                    out.size = entries * kInstanceStride;
                    out.first = g.ringInstanceAt;
                    g.ringInstanceAt += entries;
                    return true;
                }
                case gfx::RingKind::Vertices:
                {
                    const UINT64 at = (g.dynamicUsed + align - 1u) / align * align;
                    if (!g.dynamic || at + size + 16u > (g.slot + 1ull) * kDynamicRing)
                        return false;
                    out.cpu = g.dynamicMapped + at;
                    out.gpu = g.dynamic->GetGPUVirtualAddress() + at;
                    out.size = size;
                    out.first = 0;
                    g.dynamicUsed = at + size;
                    return true;
                }
                }
                return false;
            }

            // (The frame's start: the slot's part of each ring from its beginning. Into beginFrame in sub-step 3.)
            void beginRings()
            {
                g.ringConstantsAt = static_cast<UINT64>(g.slot) * kConstantRing;
                g.ringInstanceAt = g.slot * kInstanceRing;
                g.dynamicUsed = static_cast<UINT64>(g.slot) * kDynamicRing;
            }

            // --- the rest: routed in sub-steps 2-6 (placeholders until then) ---
            double waitFrameSlot() override { return 0.0; }
            bool beginFrame(uint64_t) override { return false; }
            void beginExtraPictures() override {}
            void endFrame(uint32_t) override {}
            // --- state (the redundant sets left out: TF_NATIVE_STATEFILTER, TODO 94 - on AMD each is a context register) ---
            // (The draws' start of a frame: the palette's and the depth's tables at the white slot, every filter forgotten.)
            void beginDraws(bool stateFilter)
            {
                filter = stateFilter;
                stripsSet = false; // (the frame's start set triangle lists)
                g.list->SetGraphicsRootDescriptorTable(2, slotHandle(0u));
                g.list->SetGraphicsRootDescriptorTable(3, slotHandle(0u));
                forget();
                bound[1] = bound[2] = 0u;
            }
            // (Anything recorded around the device - the anti-aliasing, the probe, the sparks - sets its own state: a target
            // bound again forgets what was set.)
            void forget()
            {
                lastPso = nullptr;
                scissorKnown = false;
                lastFactor = -1.0f;
                bound[0] = bound[1] = bound[2] = kUnknown;
                iaDirty = true;
            }

            void setTarget(gfx::TargetId id, bool withDepth) override
            {
                State::DeviceTarget &t = dt(id);
                const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvAt(t.rtv);
                // (Depth read by the shader - the fog's pass: the read-only view.)
                const bool readOnly = t.depthState == (D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvAt(readOnly ? t.rtv + 64u : t.rtv);
                // (A target made without depth: drawn with none, whatever is asked - its depth view's place is not its own.)
                g.list->OMSetRenderTargets(1, &rtv, FALSE, withDepth && t.depth ? &dsv : nullptr);
                forget();
            }
            void setViewport(float x, float y, float w, float h) override
            {
                const D3D12_VIEWPORT vp{x, y, w, h, 0.0f, 1.0f};
                g.list->RSSetViewports(1, &vp);
            }
            void setScissor(const gfx::Rect &r) override
            {
                const D3D12_RECT d{r.x0, r.y0, r.x1, r.y1};
                if (!filter || !scissorKnown || d.left != lastScissor.left || d.top != lastScissor.top || d.right != lastScissor.right ||
                    d.bottom != lastScissor.bottom)
                {
                    g.list->RSSetScissorRects(1, &d);
                    ++g_cntScissors;
                    lastScissor = d;
                    scissorKnown = true;
                }
            }
            bool setPipeline(const gfx::PipelineDesc &p) override
            {
                const bool strips = p.effect == gfx::Effect::Cloud;
                if (strips != stripsSet)
                {
                    g.list->IASetPrimitiveTopology(strips ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    stripsSet = strips;
                }
                if (p.effect != gfx::Effect::None)
                {
                    ID3D12PipelineState *pso = nullptr;
                    switch (p.effect)
                    {
                    case gfx::Effect::Fxaa: pso = g.aaPipeline; break;
                    case gfx::Effect::SmaaEdges: pso = g.smaaEdgePipe; break;
                    case gfx::Effect::SmaaWeights: pso = smaaLookups() ? g.smaaWeightPipe : nullptr; break;
                    case gfx::Effect::SmaaBlend: pso = g.smaaBlendPipe; break;
                    case gfx::Effect::Cloud: pso = g.cloudPipeline; break;
                    default: break;
                    }
                    if (!pso)
                        return false;
                    if (!filter || pso != lastPso)
                    {
                        g.list->SetPipelineState(pso);
                        lastPso = pso;
                    }
                    if (p.effect == gfx::Effect::SmaaWeights)
                    {
                        // (Its own tables: SMAA's area and search textures.)
                        g.list->SetGraphicsRootDescriptorTable(2, slotHandle(g.smaaAreaSlot));
                        g.list->SetGraphicsRootDescriptorTable(3, slotHandle(g.smaaSearchSlot));
                        bound[1] = bound[2] = kUnknown;
                    }
                    lastDesc = p;
                    lastPipelines = g.pipelinesChanged;
                    return true;
                }
                // (The same description as the last one asked, and no pipeline made since: that one, still set.)
                if (filter && lastPso && lastPipelines == g.pipelinesChanged && sameDesc(p, lastDesc))
                    return true;
                BlendKey k;
                k.src = blendOf(p.blend.src);
                k.dst = blendOf(p.blend.dst);
                k.op = p.blend.op == gfx::BlendOp::RevSubtract ? D3D12_BLEND_OP_REV_SUBTRACT
                       : p.blend.op == gfx::BlendOp::Subtract  ? D3D12_BLEND_OP_SUBTRACT
                                                              : D3D12_BLEND_OP_ADD;
                k.enabled = p.blend.enabled;
                k.premultiply = p.blend.premultiply;
                ID3D12PipelineState *pso = pipelineFor(k, p.depthTest, compareOf(p.depthFunc), p.depthWrite, p.writeMask, p.picture, p.clip,
                                                       p.variant, p.alphaTest);
                if (!pso)
                    return false;
                if (!filter || pso != lastPso)
                {
                    g.list->SetPipelineState(pso);
                    lastPso = pso;
                }
                lastDesc = p;
                lastPipelines = g.pipelinesChanged;
                return true;
            }
            void setBlendFactor(const float rgba[4]) override
            {
                if (!filter || rgba[0] != lastFactor)
                {
                    g.list->OMSetBlendFactor(rgba);
                    ++g_cntFactors;
                    lastFactor = rgba[0];
                }
            }
            // (t0 the draw's texture, set every draw; t1 the palette / the target's copy, t2 the depth: set when they change.)
            void bindTextures(gfx::TextureId t0, gfx::TextureId t1, gfx::TextureId t2) override
            {
                g.list->SetGraphicsRootDescriptorTable(1, slotHandle(slotOf(t0)));
                bound[0] = t0;
                if (t1 != gfx::kKeepTexture && t1 != bound[1])
                {
                    g.list->SetGraphicsRootDescriptorTable(2, slotHandle(slotOf(t1)));
                    bound[1] = t1;
                }
                if (t2 != gfx::kKeepTexture && t2 != bound[2])
                {
                    g.list->SetGraphicsRootDescriptorTable(3, slotHandle(slotOf(t2)));
                    bound[2] = t2;
                }
            }
            void bindDrawConstants(const gfx::RingSpan &c) override { g.list->SetGraphicsRootConstantBufferView(0, c.gpu); }
            void bindPictureConstants(const gfx::RingSpan &) override {}

            // --- geometry: stream 0 (a mesh, or ring vertices), the instance stream (1, the Instances ring), the frame
            // before's vertices (2), the indices - set on the list at the next draw ---
            void setVertices(const gfx::RingSpan &v, uint32_t stride) override
            {
                vb = {v.gpu, v.size, stride};
                iaDirty = true;
            }
            void setIndices(const gfx::RingSpan &i) override
            {
                ib = {i.gpu, i.size, DXGI_FORMAT_R32_UINT};
                iaDirty = true;
            }
            void setMesh(uint32_t meshId) override
            {
                const State::DeviceMesh &m = g.devMeshes[meshId];
                // (TF_NATIVE_MESHVRAM=alt: the upload heap's copy in every other profile window.)
                const bool upHeap = m.buffer && m.vram && (g_profWindow & 1u);
                vb = upHeap ? m.vbv : (m.vbvV.SizeInBytes ? m.vbvV : m.vbv);
                ib = upHeap ? m.ibv : (m.ibvV.SizeInBytes ? m.ibvV : m.ibv);
                iaDirty = true;
            }
            void setPreviousVertices(const gfx::RingSpan &v) override
            {
                // (none: a zero stride's one entry at the ring's start - read by nothing, pose.y off)
                vbB = v.size ? D3D12_VERTEX_BUFFER_VIEW{v.gpu, v.size, kVertexBStride}
                             : D3D12_VERTEX_BUFFER_VIEW{g.dynamic ? g.dynamic->GetGPUVirtualAddress() + static_cast<UINT64>(g.slot) * kDynamicRing : 0ull,
                                                        kVertexBStride, 0u};
                iaDirty = true;
            }
            void draw(uint32_t vertices, uint32_t first, uint32_t instances, uint32_t firstInstance) override
            {
                flushIa();
                g.list->DrawInstanced(vertices, instances, first, firstInstance);
            }
            void drawIndexed(uint32_t indices, uint32_t first, uint32_t instances, uint32_t firstInstance) override
            {
                flushIa();
                g.list->DrawIndexedInstanced(indices, instances, first, 0, firstInstance);
            }
            void drawIndirect(uint64_t groundMeshId, uint32_t firstInstance) override
            {
                GpuMesh *m = grassgpu::meshFor(groundMeshId);
                if (!m)
                    return;
                vb = m->vbvV.SizeInBytes ? m->vbvV : m->vbv;
                ib = m->ibvV.SizeInBytes ? m->ibvV : m->ibv;
                iaDirty = true;
                flushIa();
                grassgpu::draw(*m, instanceView(), firstInstance); // (grassgpu: its count the GPU's)
                iaDirty = true; // (its instance stream moved to the pass's entry)
            }
            void dispatchGrass(const gfx::RingSpan &, uint32_t) override {}
            void copy(gfx::TargetId, const gfx::Rect &, gfx::TargetId, const gfx::Rect &) override {}
            void copyWhole(gfx::TargetId dst, gfx::TargetId src) override
            {
                State::DeviceTarget &d = dt(dst), &s = dt(src);
                if (!d.colour || !s.colour)
                    return;
                stateOf(s, D3D12_RESOURCE_STATE_COPY_SOURCE);
                stateOf(d, D3D12_RESOURCE_STATE_COPY_DEST);
                g.list->CopyResource(d.colour, s.colour);
                ++g_cntCopies;
            }
            gfx::QueryId timestamp() override { return 0; }
            void beginOcclusion(gfx::QueryId) override {}
            void endOcclusion(gfx::QueryId) override {}
            void predicate(gfx::QueryId, bool) override {}
            bool readQueries(uint64_t, double &) override { return false; }
            void publish(gfx::TargetId, uint32_t, uint32_t) override {}

        private:
            static bool sameDesc(const gfx::PipelineDesc &a, const gfx::PipelineDesc &b)
            {
                return a.blend.src == b.blend.src && a.blend.dst == b.blend.dst && a.blend.op == b.blend.op && a.blend.enabled == b.blend.enabled &&
                       a.blend.premultiply == b.blend.premultiply && a.depthTest == b.depthTest && a.depthFunc == b.depthFunc &&
                       a.depthWrite == b.depthWrite && a.writeMask == b.writeMask && a.picture == b.picture && a.clip == b.clip &&
                       a.variant == b.variant && a.alphaTest == b.alphaTest && a.effect == b.effect;
            }
            static D3D12_VERTEX_BUFFER_VIEW instanceView()
            {
                return {g.instances->GetGPUVirtualAddress(), kInstanceRing * kInstanceStride * State::kFrames, kInstanceStride};
            }
            void flushIa()
            {
                if (!iaDirty)
                    return;
                const D3D12_VERTEX_BUFFER_VIEW views[3] = {vb, instanceView(), vbB};
                g.list->IASetVertexBuffers(0, 3, views);
                g.list->IASetIndexBuffer(&ib);
                iaDirty = false;
            }
            bool filter = true;
            ID3D12PipelineState *lastPso = nullptr;
            gfx::PipelineDesc lastDesc;
            uint64_t lastPipelines = 0;
            bool scissorKnown = false;
            D3D12_RECT lastScissor{};
            float lastFactor = -1.0f;
            static constexpr gfx::TextureId kUnknown = ~1u; // (set around the device: bound again at the next ask)
            gfx::TextureId bound[3] = {kUnknown, kUnknown, kUnknown};
            D3D12_VERTEX_BUFFER_VIEW vb{}, vbB{};
            D3D12_INDEX_BUFFER_VIEW ib{};
            bool iaDirty = true;
            bool stripsSet = false; // (the sparks' triangle strips set, not lists)
        };

        D3d12Device &d3d12Device()
        {
            static D3d12Device d;
            return d;
        }

        // ---- The frame builder's side (nr_frame.cpp from sub-step 3) ----
        // A render target for a GS frame buffer (twice its GS size), cleared to transparent black; a grown one
        // keeps its device target (new surfaces, the same views' places).
        bool makeTarget(State::Target &t, float gsW, float gsH, UINT sx = 0u, UINT sy = 0u)
        {
            ++g_targetsMade;
            sx = sx ? sx : g.scaleX;
            sy = sy ? sy : g.scaleY;
            if (std::getenv("TF_NATIVE_TARGETLOG"))
                std::fprintf(stderr, "[native render] target made %gx%g at %ux%u\n", gsW, gsH, sx, sy);
            gfx::TargetDesc d;
            d.width = static_cast<uint32_t>(gsW) * sx;
            d.height = static_cast<uint32_t>(gsH) * sy;
            d.colour = gfx::Format::BGRA8;
            gfx::Device &dev = gfx::device();
            bool made;
            if (t.id)
            {
                made = dev.resizeTarget(t.id, d);
                if (!made)
                {
                    dev.releaseTarget(t.id);
                    t.id = 0;
                }
            }
            else
                made = (t.id = dev.createTarget(d)) != 0;
            if (!made)
                return false;
            t.gsW = gsW;
            t.gsH = gsH;
            t.w = d.width;
            t.h = d.height;
            return true;
        }

        // A frame buffer's identity: its base and width (the same pages are another buffer at
        // another width - the 3D field and, later in the frame, the glare's small buffers).
        uint32_t targetKey(uint32_t frame) { return (frame & 0x1FFu) | ((frame >> 16) & 0x3Fu) << 9; }
        bool depthFormat(uint32_t frame) { return ((frame >> 24) & 0x30u) == 0x30u; } // (PSMZ*: a depth buffer written as colour)

        // The anti-aliasing pass's copy of the picture (made again when the picture's size changes:
        // makeTargets has waited for the frames in flight then, so its view's slot is free to rewrite).
        bool aaSourceFor()
        {
            if (g.aaSource && g.aaW == g.width && g.aaH == g.height)
                return true;
            if (g.aaSource)
                g.uploads.push_back(g.aaSource); // (released once the GPU is done)
            g.aaSource = nullptr;
            if (!g.aaSlot)
            {
                if (g.freeSlots.empty())
                    return false;
                g.aaSlot = g.freeSlots.back();
                g.freeSlots.pop_back();
            }
            D3D12_RESOURCE_DESC td = g.colour->GetDesc();
            td.Flags = D3D12_RESOURCE_FLAG_NONE;
            const D3D12_HEAP_PROPERTIES gpu = heap(D3D12_HEAP_TYPE_DEFAULT);
            if (FAILED(g.device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&g.aaSource))))
                return false;
            g.aaState = D3D12_RESOURCE_STATE_COPY_DEST;
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = td.Format;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE h = g.srvHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += static_cast<SIZE_T>(g.aaSlot) * g.srvSize;
            srvWritten(g.aaSlot, "the anti-aliasing source's");
            g.device->CreateShaderResourceView(g.aaSource, &sd, h);
            g.aaW = g.width;
            g.aaH = g.height;
            return true;
        }

        // SMAA's edges and blend-weights targets at the picture's size (after aaSourceFor: made again when it changes,
        // as aaSource is - the frames in flight waited for then - into the same views' places). False: SMAA not made.
        bool smaaTargetsFor()
        {
            if (!g.smaaEdgePipe || !g.smaaWeightPipe || !g.smaaBlendPipe || !g.smaaArea || !g.smaaSearch)
                return false;
            if (g.smaaEdges && g.smaaWeights && g.smaaW == g.aaW && g.smaaH == g.aaH)
                return true;
            if (g.smaaEdges)
                g.uploads.push_back(g.smaaEdges); // (released once the GPU is done)
            if (g.smaaWeights)
                g.uploads.push_back(g.smaaWeights);
            g.smaaEdges = g.smaaWeights = nullptr;
            const DXGI_FORMAT formats[2] = {DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM};
            ID3D12Resource **made[2] = {&g.smaaEdges, &g.smaaWeights};
            const UINT rtvs[2] = {g.smaaEdgesRtv, g.smaaWeightsRtv}, slots[2] = {g.smaaEdgesSlot, g.smaaWeightsSlot};
            const D3D12_HEAP_PROPERTIES gpu = heap(D3D12_HEAP_TYPE_DEFAULT);
            for (int k = 0; k < 2; ++k)
            {
                D3D12_RESOURCE_DESC td{};
                td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                td.Width = g.aaW;
                td.Height = g.aaH;
                td.DepthOrArraySize = 1;
                td.MipLevels = 1;
                td.Format = formats[k];
                td.SampleDesc.Count = 1;
                td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                D3D12_CLEAR_VALUE clear{};
                clear.Format = formats[k];
                if (FAILED(g.device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
                                                             IID_PPV_ARGS(made[k]))))
                {
                    *made[k] = nullptr;
                    release(g.smaaEdges);
                    release(g.smaaWeights);
                    return false;
                }
                g.device->CreateRenderTargetView(*made[k], nullptr, rtvAt(rtvs[k]));
                D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.Format = formats[k];
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sd.Texture2D.MipLevels = 1;
                D3D12_CPU_DESCRIPTOR_HANDLE h = g.srvHeap->GetCPUDescriptorHandleForHeapStart();
                h.ptr += static_cast<SIZE_T>(slots[k]) * g.srvSize;
                srvWritten(slots[k], k ? "SMAA's blend weights'" : "SMAA's edges'");
                g.device->CreateShaderResourceView(*made[k], &sd, h);
            }
            g.smaaW = g.aaW;
            g.smaaH = g.aaH;
            return true;
        }

        // SMAA's two lookup textures copied into video memory, recorded into the frame's list once (both staging
        // buffers made first: either both copies are recorded or none). False: not yet.
        bool smaaLookups()
        {
            if (g.smaaLookupUploaded)
                return true;
            ID3D12Resource *const dsts[2] = {g.smaaArea, g.smaaSearch};
            const unsigned char *const data[2] = {areaTexBytes, searchTexBytes};
            const UINT pitch[2] = {AREATEX_PITCH, SEARCHTEX_PITCH}, rows[2] = {AREATEX_HEIGHT, SEARCHTEX_HEIGHT};
            ID3D12Resource *staging[2] = {};
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[2]{};
            const D3D12_HEAP_PROPERTIES up = heap(D3D12_HEAP_TYPE_UPLOAD);
            const D3D12_RANGE none{0, 0};
            for (int k = 0; k < 2; ++k)
            {
                const D3D12_RESOURCE_DESC td = dsts[k]->GetDesc();
                UINT64 total = 0;
                g.device->GetCopyableFootprints(&td, 0, 1, 0, &fp[k], nullptr, nullptr, &total);
                const D3D12_RESOURCE_DESC bd = bufferDesc(total);
                void *p = nullptr;
                if (FAILED(g.device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                             IID_PPV_ARGS(&staging[k]))) ||
                    FAILED(staging[k]->Map(0, &none, &p)))
                {
                    release(staging[0]);
                    release(staging[1]);
                    return false;
                }
                for (UINT y = 0; y < rows[k]; ++y)
                    std::memcpy(static_cast<uint8_t *>(p) + fp[k].Offset + static_cast<size_t>(y) * fp[k].Footprint.RowPitch,
                                data[k] + static_cast<size_t>(y) * pitch[k], pitch[k]);
                staging[k]->Unmap(0, nullptr);
            }
            for (int k = 0; k < 2; ++k)
            {
                D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
                dst.pResource = dsts[k];
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.SubresourceIndex = 0;
                src.pResource = staging[k];
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint = fp[k];
                g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                transition(dsts[k], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                g.uploads.push_back(staging[k]); // (released once the GPU is done)
            }
            g.smaaLookupUploaded = true;
            return true;
        }

        // The anti-aliasing pass recorded: the picture copied into aaSource, FXAA - or SMAA 1x's three passes
        // (TF_NATIVE_AA=2; FXAA when SMAA could not be made) - drawn from it back into the picture. Leaves the
        // picture bound as the render target without its depth, the pass's pipeline and its texture tables (SMAA's:
        // the palette's and the depth's set back to slot 0, as at the frame's start): the caller binds its own
        // again. False: not run.
        // (The frame builder's) The anti-aliasing pass's targets: the picture's copy it reads, and SMAA's edges and blend
        // weights - the picture's size, made again when it changes.
        namespace aa
        {
            gfx::TargetId source = 0, edges = 0, weights = 0;
            bool ensure(gfx::TargetId &t, uint32_t w, uint32_t h, gfx::Format f)
            {
                gfx::Device &dev = gfx::device();
                gfx::TargetDesc d;
                d.width = w;
                d.height = h;
                d.colour = f;
                d.depth = false;
                if (t && !dev.resizeTarget(t, d))
                {
                    dev.releaseTarget(t);
                    t = 0;
                }
                if (!t)
                    t = dev.createTarget(d);
                return t != 0;
            }
            uint32_t sourceW = 0, sourceH = 0, smaaW = 0, smaaH = 0;
            bool sourceFor(uint32_t w, uint32_t h)
            {
                if (source && sourceW == w && sourceH == h)
                    return true;
                sourceW = sourceH = 0;
                if (!ensure(source, w, h, gfx::Format::RGBA8))
                    return false;
                sourceW = w;
                sourceH = h;
                return true;
            }
            bool smaaFor(uint32_t w, uint32_t h)
            {
                if (edges && weights && smaaW == w && smaaH == h)
                    return true;
                smaaW = smaaH = 0;
                if (!ensure(edges, w, h, gfx::Format::RG8) || !ensure(weights, w, h, gfx::Format::RGBA8))
                    return false;
                smaaW = w;
                smaaH = h;
                return true;
            }
        }

        bool runAa(State::Target &picture)
        {
            gfx::Device &dev = gfx::device();
            gfx::RingSpan cs; // (its constants: one entry, whichever pass runs)
            if (!g.aaPipeline || !picture.id || !dt(picture).colour || !aa::sourceFor(g.width, g.height) ||
                !dev.ringAlloc(gfx::RingKind::Constants, kConstantStride, 256u, cs))
                return false;
            g.aaW = g.width;
            g.aaH = g.height;
            gfx::PipelineDesc smaaWeights;
            smaaWeights.effect = gfx::Effect::SmaaWeights;
            const bool smaa = aaMode() == 2 && g.smaaEdgePipe && g.smaaBlendPipe && aa::smaaFor(g.aaW, g.aaH) && dev.setPipeline(smaaWeights);
            static const bool profiling = std::getenv("TF_NATIVE_PROFILE") != nullptr;
            const bool timed = profiling && g.aaQueries && g.aaTimes;
            if (timed)
                g.list->EndQuery(g.aaQueries, D3D12_QUERY_TYPE_TIMESTAMP, g.slot * 2u);
            dev.copyWhole(aa::source, picture.id);
            dev.use(aa::source, gfx::UseFor::Sample);
            dev.use(picture.id, gfx::UseFor::Draw);
            const gfx::TextureId sourceView = dev.view(aa::source);
            const gfx::Rect all{0, 0, static_cast<int32_t>(g.aaW), static_cast<int32_t>(g.aaH)};
            auto into = [&](gfx::TargetId t) {
                dev.setTarget(t, false);
                dev.setViewport(0.0f, 0.0f, static_cast<float>(g.aaW), static_cast<float>(g.aaH));
                dev.setScissor(all);
            };
            if (smaa)
            {
                // (b0: 1 / width, 1 / height, width, height - SMAA_RT_METRICS.)
                const float m[4] = {1.0f / static_cast<float>(g.aaW), 1.0f / static_cast<float>(g.aaH), static_cast<float>(g.aaW),
                                    static_cast<float>(g.aaH)};
                std::memcpy(cs.cpu, m, sizeof(m));
                gfx::PipelineDesc pd;
                // 1. Luma edges (the pixels without an edge discarded: the target cleared first).
                dev.use(aa::edges, gfx::UseFor::Draw);
                const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                dev.clearColour(aa::edges, zero);
                into(aa::edges);
                pd.effect = gfx::Effect::SmaaEdges;
                dev.setPipeline(pd);
                dev.bindDrawConstants(cs);
                dev.bindTextures(sourceView, gfx::kKeepTexture, gfx::kKeepTexture);
                dev.draw(3u, 0u);
                // 2. The blend weights (every pixel written).
                dev.use(aa::edges, gfx::UseFor::Sample);
                dev.use(aa::weights, gfx::UseFor::Draw);
                into(aa::weights);
                dev.setPipeline(smaaWeights);
                dev.bindDrawConstants(cs);
                dev.bindTextures(dev.view(aa::edges), gfx::kKeepTexture, gfx::kKeepTexture);
                dev.draw(3u, 0u);
                // 3. The picture blended with its neighbours, back into it; the tables bound as at the frame's start.
                dev.use(aa::weights, gfx::UseFor::Sample);
                into(picture.id);
                pd.effect = gfx::Effect::SmaaBlend;
                dev.setPipeline(pd);
                dev.bindDrawConstants(cs);
                dev.bindTextures(sourceView, dev.view(aa::weights), gfx::kKeepTexture);
                dev.draw(3u, 0u);
                dev.bindTextures(sourceView, 0u, 0u);
                ++g_smaaFrames;
                if (timed)
                {
                    g.list->EndQuery(g.aaQueries, D3D12_QUERY_TYPE_TIMESTAMP, g.slot * 2u + 1u);
                    g.list->ResolveQueryData(g.aaQueries, D3D12_QUERY_TYPE_TIMESTAMP, g.slot * 2u, 2u, g.aaTimes, g.slot * 16ull);
                    g.aaTimed[g.slot] = true;
                }
                return true;
            }
            into(picture.id);
            gfx::PipelineDesc pd;
            pd.effect = gfx::Effect::Fxaa;
            dev.setPipeline(pd);
            // (Tuning, for tests: the sub-pixel amount - FXAA's own default 0.75 blurs fine texture
            // detail; 0.25 (TODO 64, 2026-11-03: was 0.5) keeps the grass and leaves sharper with the edges as
            // smooth - the edge threshold and its minimum.)
            static const float tune[3] = {
                std::getenv("TF_NATIVE_AA_SUBPIX") ? static_cast<float>(std::atof(std::getenv("TF_NATIVE_AA_SUBPIX"))) : 0.25f,
                std::getenv("TF_NATIVE_AA_EDGE") ? static_cast<float>(std::atof(std::getenv("TF_NATIVE_AA_EDGE"))) : 0.125f,
                std::getenv("TF_NATIVE_AA_EDGEMIN") ? static_cast<float>(std::atof(std::getenv("TF_NATIVE_AA_EDGEMIN"))) : 0.0625f};
            const float k[8] = {1.0f / static_cast<float>(g.aaW), 1.0f / static_cast<float>(g.aaH), static_cast<float>(g.aaW),
                                static_cast<float>(g.aaH), tune[0], tune[1], tune[2], 0.0f};
            std::memcpy(cs.cpu, k, sizeof(k));
            dev.bindDrawConstants(cs);
            dev.bindTextures(sourceView, gfx::kKeepTexture, gfx::kKeepTexture);
            dev.draw(3u, 0u);
            if (timed)
            {
                g.list->EndQuery(g.aaQueries, D3D12_QUERY_TYPE_TIMESTAMP, g.slot * 2u + 1u);
                g.list->ResolveQueryData(g.aaQueries, D3D12_QUERY_TYPE_TIMESTAMP, g.slot * 2u, 2u, g.aaTimes, g.slot * 16ull);
                g.aaTimed[g.slot] = true;
            }
            return true;
        }
    }

    namespace
    {
        // The sun's occlusion probe (roadmap N3a) recorded: the depth of `scene` - the target the frame's 3D went into -
        // over `rect` (its GS pixels: x0, y0, x1, y1) point-sampled into the 64 x 64 probe target and copied into the
        // slot's readback (finishSlot hands it to nr_occlusion). Leaves the probe target bound and the scene's depth
        // readable: the caller binds its own again. False: not run.
        bool runProbe(State::Target &scene, const float rect[4], uint64_t tick)
        {
            gfx::Device &dev = gfx::device();
            const gfx::TextureId sceneDepth = scene.id ? dev.view(scene.id, true) : 0u;
            gfx::RingSpan cs;
            if (!g.probePipeline || !scene.id || !sceneDepth || scene.gsW <= 0.0f || scene.gsH <= 0.0f ||
                !dev.ringAlloc(gfx::RingKind::Constants, kConstantStride, 256u, cs))
                return false;
            dev.useDepth(scene.id, gfx::DepthUse::ReadSample);
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = g.rtvHeap->GetCPUDescriptorHandleForHeapStart();
            rtv.ptr += static_cast<SIZE_T>(g.probeRtv) * g.rtvSize;
            g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            const D3D12_VIEWPORT vp{0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f};
            g.list->RSSetViewports(1, &vp);
            const D3D12_RECT all{0, 0, 64, 64};
            g.list->RSSetScissorRects(1, &all);
            g.list->SetPipelineState(g.probePipeline);
            const float sx = static_cast<float>(scene.w) / scene.gsW, sy = static_cast<float>(scene.h) / scene.gsH;
            const float k[8] = {rect[0] * sx, rect[1] * sy, (rect[2] - rect[0]) * sx / 64.0f, (rect[3] - rect[1]) * sy / 64.0f,
                                static_cast<float>(scene.w), static_cast<float>(scene.h), 0.0f, 0.0f};
            std::memcpy(cs.cpu, k, sizeof(k));
            g.list->SetGraphicsRootConstantBufferView(0, cs.gpu);
            g.list->SetGraphicsRootDescriptorTable(3, slotHandle(D3d12Device::slotOf(sceneDepth)));
            g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g.list->DrawInstanced(3, 1, 0, 0);
            transition(g.probeTarget, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
            src.pResource = g.probeTarget;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.pResource = g.probeReadback;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Offset = static_cast<UINT64>(g.slot) * 16384u;
            dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_FLOAT, 64u, 64u, 1u, 256u};
            g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            transition(g.probeTarget, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            g.probeTaken[g.slot] = true;
            g.probeTick[g.slot] = tick;
            return true;
        }
    }

    namespace
    {
        // (TODO 27) The warp tunnel's sparks (Draw::cloud, nr_warp.cpp): each set's first `count` points drawn as instances of
        // a four-corner strip through kCloudShader, added to the picture. A dot's Gaussian radius: PointCloud::sigma times the
        // geometric mean of the PS2 pixel's size at the picture's height (H / 480 across - the 4:3 picture's 640 pixels -, H /
        // 224 down - a line of the 224-line field the tunnel draws, two of the shown frame's), its centre at 1 / (2 pi sigma^2)
        // of the colour: a dot adds what the PS2's one-pixel point added, spread smooth. Leaves the target bound without depth
        // and the topology a list: the caller binds its own again. False: not drawn (no pipeline, nothing to draw, the ring full).
        bool drawCloud(const Draw &draw, State::Target &tg)
        {
            if (!g.cloudPipeline || !draw.cloud || !tg.id || tg.gsW <= 0.0f || tg.gsH <= 0.0f)
                return false;
            const PointCloud &pc = *draw.cloud;
            gfx::Device &dev = gfx::device();
            dev.use(tg.id, gfx::UseFor::Draw);
            dev.setTarget(tg.id, false);
            dev.setViewport(0.0f, 0.0f, static_cast<float>(tg.w), static_cast<float>(tg.h));
            const float sx = static_cast<float>(tg.w) / tg.gsW, sy = static_cast<float>(tg.h) / tg.gsH;
            gfx::Rect r{static_cast<int32_t>(draw.scissor[0] * sx), static_cast<int32_t>(draw.scissor[1] * sy),
                        static_cast<int32_t>((draw.scissor[2] + 1.0f) * sx), static_cast<int32_t>((draw.scissor[3] + 1.0f) * sy)};
            r.x1 = std::min<int32_t>(r.x1, static_cast<int32_t>(tg.w));
            r.y1 = std::min<int32_t>(r.y1, static_cast<int32_t>(tg.h));
            dev.setScissor(r);
            gfx::PipelineDesc pd;
            pd.effect = gfx::Effect::Cloud;
            if (!dev.setPipeline(pd))
                return false;
            const float h = static_cast<float>(tg.h);
            const float sigmaUnit = std::max(0.05f, pc.sigma);
            const float sigma = sigmaUnit * std::sqrt((h / 480.0f) * (h / 224.0f)); // (in the picture's pixels)
            const float peak = 1.0f / (6.2831853f * sigmaUnit * sigmaUnit);
            bool drawn = false;
            for (const PointCloud::Set &set : pc.sets)
            {
                if (!set.points || set.count == 0u)
                    continue;
                GpuMesh *m = upload(set.points);
                gfx::RingSpan cs;
                if (!m || !gfx::device().ringAlloc(gfx::RingKind::Constants, kConstantStride, 256u, cs))
                    continue;
                float k[28] = {};
                std::memcpy(k, set.rows, sizeof(set.rows));
                k[16] = draw.originX - 0.5f;
                k[17] = draw.originY - 0.5f;
                k[18] = 2.0f / tg.gsW;
                k[19] = 2.0f / tg.gsH;
                for (int c = 0; c < 3; ++c)
                    k[20 + c] = set.colour[c] * peak;
                k[24] = 3.0f * sigma * 2.0f / static_cast<float>(tg.w);
                k[25] = 3.0f * sigma * 2.0f / h;
                k[26] = pc.squeeze;
                k[27] = 2048.0f;
                std::memcpy(cs.cpu, k, sizeof(k));
                dev.bindDrawConstants(cs);
                if (m->meshId)
                    dev.setMesh(m->meshId);
                else
                    dev.setVertices(m->ringVertices, sizeof(Vertex));
                const UINT count = std::min<UINT>(set.count, static_cast<UINT>(set.points->vertices.size()));
                dev.draw(4u, 0u, count, 0u);
                drawn = true;
            }
            return drawn; // (the triangle lists set again with the next pipeline)
        }
    }

    // The device's video memory in use (or its budget), from its adapter.
    // Game textures made ahead (the capture's prefetch, TODO 76/77): handed to the uploader at the next
    // frame's start, so a screen's textures are on the GPU before a frame draws with them - made on the
    // render thread when first drawn, an HQ screen change made ~50 of them in one frame (60-180 ms).
    namespace
    {
        std::mutex g_preUploadMutex;
        std::vector<std::weak_ptr<const Texture>> g_preUploads;
    }
    void preUploadImpl(const std::shared_ptr<const Texture> &texture)
    {
        std::lock_guard<std::mutex> lock(g_preUploadMutex);
        g_preUploads.push_back(texture);
    }

    uint64_t videoMemory(bool budget)
    {
        static IDXGIAdapter3 *adapter = nullptr;
        if (!adapter && g.device)
        {
            IDXGIFactory4 *factory = nullptr;
            if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            {
                factory->EnumAdapterByLuid(g.device->GetAdapterLuid(), IID_PPV_ARGS(&adapter));
                factory->Release();
            }
        }
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (!adapter || FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
            return 0;
        return budget ? info.Budget : info.CurrentUsage;
    }

    // The default adapter's dedicated video memory, MB (TODO 94; TF_NATIVE_VRAM_MB=<n> pretends another size -
    // the low-end policy tried on a larger card); 0: not known. (The device is made on that adapter.)
    // The graphics card's maker (PCI vendor id: 0x1002 AMD, 0x10DE NVIDIA, 0x8086 Intel; 0: not known).
    uint32_t adapterVendor()
    {
        static const uint32_t vendor = []() -> uint32_t {
            uint32_t out = 0;
            IDXGIFactory1 *factory = nullptr;
            if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            {
                IDXGIAdapter1 *adapter = nullptr;
                if (SUCCEEDED(factory->EnumAdapters1(0, &adapter)))
                {
                    DXGI_ADAPTER_DESC1 desc{};
                    if (SUCCEEDED(adapter->GetDesc1(&desc)))
                        out = desc.VendorId;
                    adapter->Release();
                }
                factory->Release();
            }
            return out;
        }();
        return vendor;
    }

    uint64_t adapterMemoryMB()
    {
        static const uint64_t mb = []() -> uint64_t {
            if (const char *e = std::getenv("TF_NATIVE_VRAM_MB"); e && *e)
                return std::strtoull(e, nullptr, 10);
            uint64_t out = 0;
            IDXGIFactory1 *factory = nullptr;
            if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            {
                IDXGIAdapter1 *adapter = nullptr;
                if (SUCCEEDED(factory->EnumAdapters1(0, &adapter)))
                {
                    DXGI_ADAPTER_DESC1 desc{};
                    if (SUCCEEDED(adapter->GetDesc1(&desc)))
                        out = static_cast<uint64_t>(desc.DedicatedVideoMemory) >> 20;
                    adapter->Release();
                }
                factory->Release();
            }
            return out;
        }();
        return mb;
    }

    // The large images' policy (TODO 94): on a card of under ~5 GB a large image is used at 2048 at most (the
    // level of its chain of that size: 4096 images a quarter of their memory), under ~3 GB at 1024; above, as
    // made. They may hold the video memory less 1.5 GB (the frame's targets, the game's textures, the
    // meshes; at most three quarters, at least 384 MB) before the least recently drawn are let go of.
    // The quality preset (TODO 94; nr_types.h): what it sets - anisotropy 4 / 8 / 16 / 16, the large images at most
    // 1024 / 2048 / 2048 / as made a side (and never above what the video memory allows), the core's clipping by the
    // GPU's own clipper on Low and Medium (the clipping shader's exact colour on clipped triangles on High and Ultra:
    // 1.1 ms of the RX 5700 XT's 8 ms at 1080p, the difference invisible on the shots compared).
    int qualityPreset()
    {
        static const int q = [] {
            const char *e = std::getenv("TF_NATIVE_QUALITY");
            const std::string v = e ? e : "";
            int level = v == "low" ? 0 : v == "medium" ? 1 : v == "high" ? 2 : v == "ultra" ? 3 : -1;
            const bool chosen = level >= 0;
            if (!chosen)
            {
                const uint64_t mb = adapterMemoryMB();
                level = mb == 0u ? 2 : mb < 3000u ? 0 : mb < 5000u ? 1 : mb < 10000u ? 2 : 3;
            }
            static const char *kNames[4] = {"Low", "Medium", "High", "Ultra"};
            std::fprintf(stderr, "[native render] quality %s (%s; %llu MB of video memory)\n", kNames[level], chosen ? "chosen" : "from the graphics card",
                         static_cast<unsigned long long>(adapterMemoryMB()));
            return level;
        }();
        return q;
    }

    uint32_t largeImageCap()
    {
        static const uint32_t cap = [] {
            const char *e = std::getenv("TF_HIRES_MAX");
            const uint64_t mb = adapterMemoryMB();
            static const uint32_t kPresetCap[4] = {1024u, 2048u, 2048u, 0u};
            const uint32_t byMemory = mb == 0u ? 0u : mb < 3000u ? 1024u : mb < 5000u ? 2048u : 0u, byPreset = kPresetCap[qualityPreset()];
            const uint32_t c = e && *e ? static_cast<uint32_t>(std::max(0, std::atoi(e)))
                                       : !byMemory ? byPreset : !byPreset ? byMemory : std::min(byMemory, byPreset);
            std::fprintf(stderr, "[native render] large images: %llu MB of video memory -> %s%s\n", static_cast<unsigned long long>(mb),
                         c ? ("at most " + std::to_string(c) + " texels a side").c_str() : "as made", e && *e ? " (TF_HIRES_MAX)" : "");
            return c;
        }();
        return cap;
    }

    uint64_t largeImageBudget()
    {
        static const uint64_t budget = [] {
            if (const char *e = std::getenv("TF_HIRES_BUDGET_MB"); e && *e)
                return std::strtoull(e, nullptr, 10) << 20;
            const uint64_t mb = adapterMemoryMB();
            if (!mb)
                return UINT64_MAX;
            return std::max<uint64_t>(384u, std::min<uint64_t>(mb > 1536u ? mb - 1536u : 0u, mb * 3u / 4u)) << 20;
        }();
        return budget;
    }

    uint64_t largeImageBytes() { return g_largeBytes.load(std::memory_order_relaxed); }

    // (Profiling: where a frame's time goes, printed every 300 frames under TF_NATIVE_PROFILE.)
    namespace prof
    {
        using clock = std::chrono::steady_clock;
        double setup = 0, meshes = 0, textures = 0, record = 0, wait = 0;
        uint32_t frames = 0;
        double since(clock::time_point &t)
        {
            const auto now = clock::now();
            const double ms = std::chrono::duration<double, std::milli>(now - t).count();
            t = now;
            return ms;
        }
    }

    void setWindowLines(int lines)
    {
        g_windowLines.store(lines, std::memory_order_relaxed);
    }

    void setSpeedupsForced(int on)
    {
        g_speedupsForced.store(on, std::memory_order_relaxed);
    }

    void setSupersampling(bool on)
    {
        g_ssaa.store(on ? 1 : 0, std::memory_order_relaxed);
    }

    void setAntiAliasing(int mode)
    {
        g_aa.store(std::clamp(mode, 0, 2), std::memory_order_relaxed);
    }

    bool latestPicture(std::vector<uint32_t> &pixels, uint32_t &width, uint32_t &height, uint64_t &serial, bool &has3D)
    {
        std::lock_guard<std::mutex> lock(g_published.mutex);
        if (g_published.serial == serial || g_published.pixels.empty())
            return false;
        pixels.swap(g_published.pixels); // (swapped, not copied: the game's side refills the other)
        has3D = g_published.has3D;
        width = g_published.width;
        height = g_published.height;
        serial = g_published.serial;
        return true;
    }

    void setSharedPictures(bool on)
    {
        g_shareOn = on ? 1 : -1;
    }

    bool latestShared(SharedPicture &out, uint64_t &serial)
    {
        std::lock_guard<std::mutex> lock(g_published.mutex);
        if (g_published.serial == serial || g_published.shared.index < 0)
            return false;
        out = g_published.shared;
        serial = g_published.serial;
        holdShown(out.index); // (held from now: see pickShared)
        return true;
    }

    double gpuPictureMs() { return g_gpuPictureMs.load(std::memory_order_relaxed); }
    double gpuExtraMs() { return g_gpuExtraMs.load(std::memory_order_relaxed); }
    double cpuPictureMs() { return g_cpuPictureMs.load(std::memory_order_relaxed); }
    double replayWaitMs() { return g_replayWaitMs; }
    double slotWaitMs() { return g_slotWaitAll.load(std::memory_order_relaxed); }
    double pacerDelayMs() { return pace::g_delayShown.load(std::memory_order_relaxed); }

    bool waitPicture(uint64_t serial, int64_t untilNs, int64_t &lastNs)
    {
        std::unique_lock<std::mutex> lock(g_published.mutex);
        const auto until = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(untilNs));
        g_published.cv.wait_until(lock, until, [serial] { return g_published.serial != serial; });
        lastNs = g_published.lastNs;
        return g_published.serial != serial;
    }

    int pacingMode()
    {
        static const int mode = [] {
            const char *v = std::getenv("TF_PACING");
            if (!v || !*v)
                return 2;
            if (std::strcmp(v, "ready") == 0)
                return 1;
            return (*v == '0' || std::strcmp(v, "old") == 0) ? 0 : 2;
        }();
        return mode;
    }

    void sharedHeld(int index)
    {
        std::lock_guard<std::mutex> lock(g_published.mutex);
        holdShown(index);
    }

    namespace
    {
        std::atomic<int> g_starting{0}; // 0 not begun, 1 on its thread, 2 done
    }

    void prepare()
    {
        int expected = 0;
        if (!g_starting.compare_exchange_strong(expected, 1))
            return;
        g.live = std::getenv("TF_NATIVE_LIVE") && *std::getenv("TF_NATIVE_LIVE") == '1';
        std::thread([] {
            const auto t0 = std::chrono::steady_clock::now();
            start();
            std::fprintf(stderr, "[native render] Direct3D 12 made ready in %.0f ms (on its own thread)\n",
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            g_starting.store(2, std::memory_order_release);
        }).detach();
    }

    bool deviceLost() { return g_deviceLost.load(std::memory_order_relaxed); }

    bool grassGpuReady() { return grassgpu::g_ready.load(std::memory_order_relaxed) && !g_deviceLost.load(std::memory_order_relaxed); }

    bool startingNow() { return g_starting.load(std::memory_order_acquire) == 1; }

    bool drawsPictures()
    {
        return g_starting.load(std::memory_order_acquire) == 2 && !g.failed && !g_deviceLost.load(std::memory_order_relaxed);
    }

    double waitForFrameSlot()
    {
        // (TODO 169 a) The slot the next frame's beginSlot takes, and the frame it last held: waited for here instead, before
        // the drawing thread takes a frame - so that when the GPU holds the renderer back, the frame taken is the newest
        // read by the time the GPU can start it, not the one read when the thread came free.
        if (!drawsPictures() || !g.fence)
            return 0.0;
        const UINT next = static_cast<UINT>(g.frameIndex % State::kFrames);
        const UINT64 value = g.slotFence[next];
        if (value == 0u || g.fence->GetCompletedValue() >= value)
            return 0.0;
        const auto t0 = std::chrono::steady_clock::now();
        waitFence(value);
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }

    void render(const Frame &input, const std::string &shotPath)
    {
        g_replayWaitMs = 0.0; // (d3d553: this call's waits for an extra picture's turn - replayWaitMs)
        // (Still being made on its thread: this frame is not drawn.)
        if (g_starting.load(std::memory_order_acquire) == 1)
            return;
        // (The device removed: no more GPU work - every resource made on it fails, and the frames went
        // on re-making the frame's textures, some 8,000 a window, until the machine went down.)
        if (g_deviceLost.load(std::memory_order_relaxed))
            return;
        if (!start())
            return;
        // (A checking aid: TF_NATIVE_REMOVEDEVICE=<VBlank> - the device removed for real (ID3D12Device5::RemoveDevice) at the
        // first frame from that tick, as a GPU hang would: the fallback to the emulated drawing tried out.)
        {
            static uint64_t removeAt = std::getenv("TF_NATIVE_REMOVEDEVICE") ? std::strtoull(std::getenv("TF_NATIVE_REMOVEDEVICE"), nullptr, 10) : 0u;
            if (removeAt && input.tick >= removeAt)
            {
                removeAt = 0u;
                ID3D12Device5 *device5 = nullptr;
                if (SUCCEEDED(g.device->QueryInterface(IID_PPV_ARGS(&device5))))
                {
                    std::fprintf(stderr, "[native render] TF_NATIVE_REMOVEDEVICE: removing the device at tick %llu\n", static_cast<unsigned long long>(input.tick));
                    device5->RemoveDevice();
                    device5->Release();
                }
            }
        }
        // (The texture cap's frame: what the last one put off, told now and then.)
        if (g_frameDeferred)
        {
            if (g_deferredSince == 0u)
                std::fprintf(stderr, "[native render] warning: %llu texture uses put off to later frames (more than %llu new textures or %llu Mtexels in a frame)\n",
                             static_cast<unsigned long long>(g_frameDeferred), static_cast<unsigned long long>(kTexCap.first),
                             static_cast<unsigned long long>(kTexCap.second >> 20));
            g_deferredSince = (g_deferredSince + 1u) % 300u;
        }
        g_frameMade = g_frameTexels = g_frameDeferred = 0;
        g_frameMakeMs = 0.0;
        if (g_frameStandIns)
            ++g_standInFrames, g_standInDraws += g_frameStandIns;
        g_frameStandIns = 0;
        auto profT = prof::clock::now();
        // (A slow frame's parts, told: TODO 59's hunt.)
        const auto renderStart = profT;
        const double partsBefore[5] = {prof::setup, prof::meshes, prof::textures, prof::record, prof::wait};
        const uint64_t madeBefore = g_made, meshesBefore = g_meshesMade;
        const double slotWaitBefore = g_slotWaitMs;
        for (double &v : g_mkMs)
            v = 0.0;
        g_mkTexels = 0;
        g_releaseMs = 0.0;
        // The picture is the frame buffer: of the targets drawn into, the one with the largest
        // screen (the shadow and effect textures are smaller).
        uint32_t mainFbp = 0;
        float mainW = 512.0f, mainH = 224.0f;
        if (input.loose && !input.draws.empty())
        {
            // (N3b: a frame the game drew outside its frames - a load screen: the buffer its last draw went to. The
            // warp tunnel's display shows two circuits over each other, one a buffer it only clears.)
            const uint32_t target = input.draws.back().target;
            mainFbp = targetKey(target);
            mainW = static_cast<float>(std::max(1u, (target >> 16) & 0x3Fu) * 64u);
            // (its lines: what the draws into it cover - the tunnel's 224-line fields fill the picture)
            mainH = 0.0f;
            for (const Draw &d : input.draws)
                if (targetKey(d.target) == mainFbp)
                    mainH = std::max(mainH, std::min(1024.0f, d.scissor[3] + 1.0f));
            if (mainH < 1.0f)
                mainH = 448.0f;
        }
        else if (input.dispfb[0] && (input.pmode & 1u))
        {
            // Circuit 1's buffer (DISPFB1: FBP, FBW) and the lines it shows (DISPLAY1's DH).
            const uint32_t fbp = static_cast<uint32_t>(input.dispfb[0]) & 0x1FFu, fbw = static_cast<uint32_t>(input.dispfb[0] >> 9) & 0x3Fu;
            mainFbp = fbp | fbw << 9;
            mainW = static_cast<float>(std::max(1u, fbw) * 64u);
            mainH = static_cast<float>(((input.display[0] >> 44) & 0x7FFu) + 1u);
            // Double buffering: the circuit shows the last frame's buffer while this one is drawn
            // into the other - the other of the two DISPFB has alternated between.
            static uint32_t pair[2] = {};
            if (pair[0] != mainFbp)
                pair[1] = pair[0], pair[0] = mainFbp;
            bool drawnInto = false;
            for (const Draw &d : input.draws)
                if (targetKey(d.target) == mainFbp)
                {
                    drawnInto = true;
                    break;
                }
            if (!drawnInto && pair[1])
                mainFbp = pair[1];
        }
        else
        {
            float area = 0.0f;
            for (const Draw &d : input.draws)
                if (d.width * d.height > area)
                    area = d.width * d.height, mainFbp = targetKey(d.target), mainW = d.width, mainH = d.height;
        }
        if (const char *forced = std::getenv("TF_NATIVE_PICTURE")) // (debug: another target as the picture, its key in hex)
        {
            mainFbp = static_cast<uint32_t>(std::strtoul(forced, nullptr, 16));
            mainW = 512.0f;
            mainH = 224.0f;
        }
        {
            // ((N3b) A frame of the game's 2D that draws nothing into the buffer shown - the frames between the warp gate's
            // zoom and the load's tunnel - not drawn: the window keeps the picture before, as the GS's display kept its
            // buffer (drawn, the empty picture's dark grey showed for a frame). TF_NATIVE_EMPTYFRAMES=1: drawn, as before.)
            static const bool emptyDrawn = std::getenv("TF_NATIVE_EMPTYFRAMES") && *std::getenv("TF_NATIVE_EMPTYFRAMES") == '1';
            // (TODO 120: the scene's first frame after such a load - the GS shows black there: the picture before kept, the
            // tunnel's last, black - not the 2D over the empty picture's dark grey)
            if (!emptyDrawn && input.firstAfterLoad && shotPath.empty())
                return;
            if (!emptyDrawn && !input.loose && !input.has3D && shotPath.empty())
            {
                bool shown = false;
                for (const Draw &d : input.draws)
                    if (targetKey(d.target) == mainFbp)
                    {
                        shown = true;
                        break;
                    }
                if (!shown)
                    return;
            }
        }
        // The picture is the PC's, not the GS's: the field (512x224, shown 4:3) maps to all of it.
        // As many lines as the window has (2026-10-28, item 64: a fixed 1080 was stretched to the
        // 3440x1440 screen, x1.33 - the blur), as wide as the picture shows on the screen (the
        // game's widescreen picture is anamorphic: a wider window gets a wider picture, not a
        // stretch). TF_NATIVE_HEIGHT=<n> fixes the lines (above the window's = supersampling).
        static const int linesFixed = [] {
            const char *v = std::getenv("TF_NATIVE_HEIGHT");
            return v && *v ? std::atoi(v) : 0;
        }();
        const int window = g_windowLines.load(std::memory_order_relaxed);
        // (TODO 255) A movie shown as a band of the picture's rows (ps2_host_display::setPictureBand): the picture as
        // many lines taller as the band is short of it, so the band itself still has the window's lines.
        float bandTop = 0.0f, bandBottom = 1.0f;
        ps2_host_display::pictureBand(bandTop, bandBottom);
        const float bandLines = linesFixed > 0 ? 1.0f : 1.0f / std::max(0.25f, bandBottom - bandTop);
        const UINT linesWanted = static_cast<UINT>(
            std::clamp(static_cast<int>(std::lround((linesFixed > 0 ? linesFixed : window > 0 ? window : 1080) * bandLines)), 240, 4320));
        const float shape = input.displayAspect > 0.5f ? input.displayAspect : 4.0f / 3.0f;
        const UINT height = linesWanted, width = (static_cast<UINT>(std::lround(linesWanted * shape)) + 7u) & ~7u;
        // The GS buffers' targets at a scale to match (the 3D field is copied into the picture:
        // drawn at the picture's own resolution, not enlarged).
        // Supersampling (the setting, TF_NATIVE_SSAA; off by default): the GS buffers' targets at 1.5 times the
        // picture's pixels in each direction, read into the picture through four taps a pixel (downsample).
        if (g_ssaa.load(std::memory_order_relaxed) < 0)
        {
            const char *e = std::getenv("TF_NATIVE_SSAA");
            int expected = -1;
            g_ssaa.compare_exchange_strong(expected, e && *e == '1' ? 1 : 0); // (unset: off)
        }
        // (TF_NATIVE_SSAA=alt: on in every other TF_NATIVE_PROFILE window - an in-run A/B of its cost, scratchpad abpar.py.)
        static const bool ssaaAlt = std::getenv("TF_NATIVE_SSAA") && *std::getenv("TF_NATIVE_SSAA") == 'a';
        const bool ssaa = ssaaAlt ? (g_profWindow & 1u) != 0u : g_ssaa.load(std::memory_order_relaxed) > 0;
        const UINT ssW = ssaa ? (width * 3u + 1u) / 2u : width, ssH = ssaa ? (height * 3u + 1u) / 2u : height;
        g.scaleX = std::max(2u, (ssW + 511u) / 512u);
        g.scaleY = std::max(2u, (ssH + 447u) / 448u);
        // (Roadmap N10) The scene buffer's 3D is drawn once, into field A's 224 rows, and copied over the whole
        // picture: at the picture's scale (lines / 448) that field held half the picture's rows, enlarged x2 down by
        // the copy - every edge's steps two lines tall, the 3D softer up and down than across. A buffer a field copy
        // reads (SCANMSK) gets twice the rows: the field at the picture's own resolution, copied 1:1. (Kept under
        // 8192 rows.) TF_NATIVE_FIELDRES=0: as before.
        static const bool fieldResOff = std::getenv("TF_NATIVE_FIELDRES") && *std::getenv("TF_NATIVE_FIELDRES") == '0';
        g.scaleYField = fieldResOff ? g.scaleY : std::max(g.scaleY, std::min(18u, (ssH + 223u) / 224u));
        if (!makeTargets(width, height))
            return;
        beginSlot();
        psv::collect();
        plib::takeWarmed();
        plib::maybeSave();
        if (!g_pstatHeap && std::getenv("TF_NATIVE_PSTATS") && *std::getenv("TF_NATIVE_PSTATS") == '1')
        {
            D3D12_QUERY_HEAP_DESC qd{};
            qd.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
            qd.Count = kPStats;
            const D3D12_HEAP_PROPERTIES back = heap(D3D12_HEAP_TYPE_READBACK);
            const D3D12_RESOURCE_DESC rb = bufferDesc(sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS) * kPStats);
            if (FAILED(g.device->CreateQueryHeap(&qd, IID_PPV_ARGS(&g_pstatHeap))) ||
                FAILED(g.device->CreateCommittedResource(&back, D3D12_HEAP_FLAG_NONE, &rb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&g_pstatBack))))
                release(g_pstatHeap);
        }
        g_texRingAt = 0; // (the slot's part of the staging ring: its last frame finished)
        prune();
        takeUploaded();
        takeLater();
        {
            // (preUpload's textures to the uploader - none drawn yet, so nothing waits for them.)
            std::vector<std::weak_ptr<const Texture>> pre;
            {
                std::lock_guard<std::mutex> lock(g_preUploadMutex);
                pre.swap(g_preUploads);
            }
            for (const auto &w : pre)
                if (auto t = w.lock(); t && !g.textures.count(t->id) && t->pixels.size() == static_cast<size_t>(t->width) * t->height)
                    askLater(t);
        }
        g.allocators[g.slot]->Reset();
        g.picAllocators[g.slot]->Reset();
        g.list = g.frameLists[g.slot]; // (d3d553: the slot's own - the other slot's may still have pictures to submit)
        g.list->Reset(g.allocators[g.slot], g.pipeline);
        // (N9 stage 3: the frame's list may be drawn more than once - every state it moves a resource through noted)
        g.journal.clear();
        g.journalOrder.clear();
        g.journalOn = true;
        grassgpu::record(input); // (grassgpu: the frame's grass - its own list, run before the first picture; the reads here)
        // (the whole frame's time on the GPU: now each picture's own two timestamps - N9 stage 3, finishSlot)
        const bool frameTimed = false;
        if (g.picQueries && g.picTimes)
            g.list->EndQuery(g.picQueries, D3D12_QUERY_TYPE_TIMESTAMP, g.slot * kPicStride + 2u * kMaxPictures); // (each picture's start)
        if (frameTimed)
            g.list->EndQuery(g.aaQueries, D3D12_QUERY_TYPE_TIMESTAMP, 2u * State::kFrames + g.slot * 2u);
        // (TF_NATIVE_PASSPROFILE: a timestamp now and after each draw; each interval is the next mark's kind.)
        const bool statsNow = g.statQueries && drawCostWindow(g_profWindow);
        g.statOpen = false;
        auto passMark = [&](uint8_t kind) {
            if (g.passQueries && g.passKinds[g.slot].size() < State::kPassMarks)
            {
                if (statsNow)
                {
                    const UINT q = g.slot * State::kPassMarks + static_cast<UINT>(g.passKinds[g.slot].size());
                    if (!g.statOpen)
                        g.list->BeginQuery(g.statQueries, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, q);
                    g.list->EndQuery(g.statQueries, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, q);
                    g.statOpen = false;
                }
                g.list->EndQuery(g.passQueries, D3D12_QUERY_TYPE_TIMESTAMP, g.slot * State::kPassMarks + static_cast<UINT>(g.passKinds[g.slot].size()));
                g.passKinds[g.slot].push_back(kind);
                g.passNotes[g.slot].emplace_back();
            }
        };
        g.passKinds[g.slot].clear();
        g.passNotes[g.slot].clear();
        passMark(255u);
        // The main target: the picture's resources.
        gfx::Device &dev = gfx::device();
        for (auto it = g.targets.begin(); it != g.targets.end();)
            if (it->second.main && it->first != mainFbp)
            {
                dev.releaseTarget(it->second.id); // (the double buffer flips every frame: its view's slot goes back)
                it = g.targets.erase(it);
            }
            else
                ++it;
        {
            State::Target &m = g.targets[mainFbp];
            if (!m.main || !m.id)
            {
                // (A buffer of its own until now - or the picture before a remake: let go of, its views' slots given
                // back once this frame's fence has passed; the picture draws through the device's picture target.)
                dev.releaseTarget(m.id);
                m = State::Target{};
                m.main = true;
                gfx::TargetDesc pd;
                pd.width = width;
                pd.height = height;
                pd.colour = gfx::Format::RGBA8;
                pd.shared = true;
                m.id = dev.createTarget(pd);
                m.w = width;
                m.h = height;
            }
            m.gsW = mainW;
            m.gsH = mainH;
        }
        // The other targets: their GS size from their FBW and the draws' scissors.
        {
            std::unordered_map<uint32_t, std::pair<float, float>> extent;
            for (const Draw &d : input.draws)
            {
                const uint32_t fbp = targetKey(d.target);
                if (fbp == mainFbp || depthFormat(d.target))
                    continue;
                auto &e = extent[fbp];
                const float fbw = static_cast<float>(std::max(1u, (d.target >> 16) & 0x3Fu) * 64u);
                e.first = std::max(e.first, std::min(fbw, d.scissor[2] + 1.0f));
                e.second = std::max(e.second, std::min(1024.0f, d.scissor[3] + 1.0f));
            }
            // A buffer the frame only draws 2D into from other buffers (or untextured) - an effect's
            // working buffer, the glow's 128 x 128 ping-pong - is kept at the GS's own resolution:
            // its passes blur by shrinking it (a pyramid to 8 x 8 texels, then four enlargements),
            // and at the picture's scale each step kept five times the detail, so the glow came out
            // as sharp-edged shapes instead of the GS's soft wash (TF_NATIVE_EFFECTSCALE=1: scaled).
            static const bool effectsScaled = std::getenv("TF_NATIVE_EFFECTSCALE") && *std::getenv("TF_NATIVE_EFFECTSCALE") == '1';
            std::unordered_map<uint32_t, int> effect; // 1: an effect's buffer so far, -1: not
            if (!effectsScaled)
            {
                std::unordered_set<uint32_t> keys;
                for (const Draw &d : input.draws)
                    keys.insert(targetKey(d.target));
                keys.insert(mainFbp);
                for (const Draw &d : input.draws)
                {
                    const uint32_t key = targetKey(d.target);
                    if (key == mainFbp || depthFormat(d.target))
                        continue;
                    int &e = effect[key];
                    if (e < 0)
                        continue;
                    const uint32_t tbp = static_cast<uint32_t>(d.tex0) & 0x3FFFu, tbw = static_cast<uint32_t>(d.tex0 >> 14) & 0x3Fu;
                    const bool fromTarget = (tbp % 32u) == 0u && keys.count((tbp / 32u) | tbw << 9) != 0u;
                    if (!d.mesh || !d.mesh->immediate || ((d.mesh->prim & 0x10u) && !fromTarget))
                        e = -1;
                    else if (fromTarget && (d.mesh->prim & 0x10u))
                        e = 1;
                }
            }
            // The shadow textures - a target of at most 256 x 256 that a draw of the frame reads as 8 bits
            // in the top byte (T8H / T4HL / T4HH: the player's shadow, the eco shadow map) - are kept at
            // the GS's resolution too: the shadow scene softens its 128 x 128 by copies of it onto itself
            // half a texel apart (ResampleSurface, bilinear), which at the picture's scale moved it by
            // part of a GS texel instead of blurring it - the receivers drew a hard-edged silhouette
            // where the GS lays a soft one (Pacific Island's cliff). TF_NATIVE_SHADOWSCALE=1: scaled.
            static const bool shadowsScaled = std::getenv("TF_NATIVE_SHADOWSCALE") && *std::getenv("TF_NATIVE_SHADOWSCALE") == '1';
            std::unordered_set<uint32_t> topRead;
            if (!shadowsScaled)
                for (const Draw &d : input.draws)
                {
                    const uint32_t psm = static_cast<uint32_t>(d.tex0 >> 20) & 0x3Fu, tbp = static_cast<uint32_t>(d.tex0) & 0x3FFFu;
                    if ((psm == 0x1Bu || psm == 0x24u || psm == 0x2Cu) && (tbp % 32u) == 0u && d.mesh && (d.mesh->prim & 0x10u))
                        topRead.insert((tbp / 32u) | (static_cast<uint32_t>(d.tex0 >> 14) & 0x3Fu) << 9);
                }
            // (N10) The field buffers: read by a field copy (SCANMSK) and holding the frame's 3D. Kept once found (a
            // frame without its copies - a load's - must not remake the buffer at the other size and back).
            static std::unordered_set<uint32_t> fieldRead;
            if (g.scaleYField != g.scaleY)
            {
                std::unordered_set<uint32_t> with3D;
                for (const Draw &d : input.draws)
                    if (d.mesh && !d.mesh->immediate && !depthFormat(d.target))
                        with3D.insert(targetKey(d.target));
                for (const Draw &d : input.draws)
                {
                    const uint32_t tbp = static_cast<uint32_t>(d.tex0) & 0x3FFFu;
                    const uint32_t key = (tbp / 32u) | (static_cast<uint32_t>(d.tex0 >> 14) & 0x3Fu) << 9;
                    if (d.scanmsk && d.mesh && (d.mesh->prim & 0x10u) && (tbp % 32u) == 0u && with3D.count(key))
                        fieldRead.insert(key);
                }
            }
            for (auto &e : extent)
            {
                State::Target &t = g.targets[e.first];
                const auto ef = effect.find(e.first);
                const bool unit = (ef != effect.end() && ef->second > 0) ||
                                  (topRead.count(e.first) && e.second.first <= 256.0f && e.second.second <= 256.0f);
                const UINT sx = unit ? 1u : g.scaleX, sy = unit ? 1u : fieldRead.count(e.first) ? g.scaleYField : g.scaleY;
                if (t.id && t.gsW >= e.second.first && t.gsH >= e.second.second && t.w == static_cast<UINT>(t.gsW) * sx &&
                    t.h == static_cast<UINT>(t.gsH) * sy)
                    continue;
                makeTarget(t, std::max(e.second.first, t.gsW), std::max(e.second.second, t.gsH), sx, sy);
            }
        }
        // Depth starts each frame cleared (the game clears it with a draw into the depth buffer's
        // pages as a colour target - a PSMZ frame - which the native targets have no need of).
        for (auto &t : g.targets)
            if (t.second.id && !t.second.main)
            {
                dev.useDepth(t.second.id, gfx::DepthUse::Write);
                dev.clearDepth(t.second.id, 0.0f);
            }
        State::Target &picture = g.targets[mainFbp];
        dev.use(picture.id, gfx::UseFor::Draw);
        const float clear[4] = {0.05f, 0.05f, 0.08f, 1.0f};
        dev.clearColour(picture.id, clear);
        dev.clearDepth(picture.id, 0.0f);
        g.list->SetGraphicsRootSignature(g.root);
        g.list->SetGraphicsRootConstantBufferView(4, g.pictureCB->GetGPUVirtualAddress() + 256ull * g.slot); // (N9 stage 3: the picture's)
        g.list->SetDescriptorHeaps(1, &g.srvHeap);
        if (!g.white)
        {
            const uint32_t white = 0xFFFFFFFFu;
            g.white = makeTexture(1u, 1u, &white, 0u);
        }
        g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        prof::setup += prof::since(profT);
        if (!shotPath.empty() && std::getenv("TF_NATIVE_ORDER"))
        {
            // (Debug: the frame's draws as runs of targets, with their first draw's kind.)
            uint32_t runKey = ~0u;
            size_t runStart = 0, runCount = 0;
            auto flush = [&]() {
                if (runCount)
                    std::fprintf(stderr, "[native render] order: draws %zu..%zu (%zu) into %04x, origin %g,%g scissor %g..%g\n", runStart, runStart + runCount - 1, runCount, runKey, input.draws[runStart].originX, input.draws[runStart].originY, input.draws[runStart].scissor[1], input.draws[runStart].scissor[3]);
            };
            for (size_t k = 0; k < input.draws.size(); ++k)
            {
                const uint32_t key = depthFormat(input.draws[k].target) ? 0xFFFFu : targetKey(input.draws[k].target);
                if (key != runKey)
                {
                    flush();
                    runKey = key;
                    runStart = k;
                    runCount = 0;
                }
                ++runCount;
                const Draw &d = input.draws[k];
                if (d.mesh && !d.mesh->immediate && !d.mesh->vertices.empty() && (k % 60u) == 0u)
                {
                    const Vertex &v = d.mesh->vertices[0];
                    const float *r = d.matrix;
                    float sc[4];
                    for (int q = 0; q < 4; ++q)
                        sc[q] = r[q] * v.x + r[4 + q] * v.y + r[8 + q] * v.z + r[12 + q];
                    std::fprintf(stderr, "[native render] order: draw %zu (3D) vertex 0 at gs %g,%g w %g; scissor %g..%g, origin %g\n", k, sc[0] / sc[3],
                                 sc[1] / sc[3], sc[3], d.scissor[1], d.scissor[3], d.originY);
                }
                if (d.mesh && d.mesh->immediate && d.mesh->prim == 0x14u)
                    std::fprintf(stderr, "[native render] order: draw %zu is a copy (tex0 %016llx scanmsk %u, %zu vertices) into %04x\n", k,
                                 static_cast<unsigned long long>(d.tex0), d.scanmsk, d.mesh->vertices.size(), key);
            }
            flush();
        }
        if (!shotPath.empty() && std::getenv("TF_NATIVE_FRAMES"))
        {
            // (Debug: every FRAME / FBMSK the frame drew with: draws, first and last index.)
            struct Use
            {
                size_t count = 0, first = 0, last = 0;
                bool immediate = false;
            };
            std::unordered_map<uint64_t, Use> uses;
            for (size_t k = 0; k < input.draws.size(); ++k)
            {
                const Draw &d = input.draws[k];
                Use &u = uses[static_cast<uint64_t>(d.target) | static_cast<uint64_t>(d.fbmsk) << 32];
                if (!u.count)
                    u.first = k;
                ++u.count;
                u.last = k;
                u.immediate |= d.mesh && d.mesh->immediate;
            }
            for (auto &u : uses)
                std::fprintf(stderr, "[native render] frame register %08x (FBP %03x FBW %u PSM %02x) fbmsk %08x: %zu draws %zu..%zu%s\n",
                             static_cast<uint32_t>(u.first), static_cast<uint32_t>(u.first) & 0x1FFu, (static_cast<uint32_t>(u.first) >> 16) & 0x3Fu,
                             (static_cast<uint32_t>(u.first) >> 24) & 0x3Fu, static_cast<uint32_t>(u.first >> 32), u.second.count, u.second.first,
                             u.second.last, u.second.immediate ? " (2D among them)" : "");
        }
        if (!shotPath.empty() && std::getenv("TF_NATIVE_FOGLOG"))
        {
            // (Debug: the depth fog's draws - into a depth-format frame (the shuffle), or sampling an
            // 8-bit-in-the-top-byte texture - with their geometry.)
            for (size_t k = 0; k < input.draws.size(); ++k)
            {
                const Draw &d = input.draws[k];
                const uint32_t tpsm = static_cast<uint32_t>(d.tex0 >> 20) & 0x3Fu;
                const bool shuffle = depthFormat(d.target), fog = (d.mesh && (d.mesh->prim & 0x10u)) && (tpsm == 0x1Bu || tpsm == 0x24u || tpsm == 0x2Cu);
                if (!shuffle && !fog)
                    continue;
                float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
                if (d.mesh)
                    for (const Vertex &v : d.mesh->vertices)
                    {
                        const float q = d.mesh->projective && v.size != 0.0f ? v.size : 1.0f;
                        x0 = std::min(x0, v.x), x1 = std::max(x1, v.x), y0 = std::min(y0, v.y), y1 = std::max(y1, v.y);
                        u0 = std::min(u0, v.u / q), u1 = std::max(u1, v.u / q), v0 = std::min(v0, v.v / q), v1 = std::max(v1, v.v / q);
                    }
                std::fprintf(stderr, "[native render] fog draw %zu: %s frame %08x fbmsk %08x prim %x tex0 %016llx (psm %x) alpha %llx test %llx zbuf %llx, %zu verts, xy %g..%g %g..%g uv %g..%g %g..%g, texture %d, origin %g %g\n",
                             k, shuffle ? "SHUFFLE" : "FOG", d.target, d.fbmsk, d.mesh ? d.mesh->prim : 0u, static_cast<unsigned long long>(d.tex0), tpsm,
                             static_cast<unsigned long long>(d.alpha), static_cast<unsigned long long>(d.test), static_cast<unsigned long long>(d.zbuf),
                             d.mesh ? d.mesh->vertices.size() : 0u, x0, x1, y0, y1, u0, u1, v0, v1, d.texture != nullptr, d.originX, d.originY);
                if (d.fogPalette && d.fogPalette->pixels.size() >= 256u && d.mesh)
                {
                    // (The palette's colour and alphas at a few entries, and the quads' Z, GS units.)
                    float z0 = 1e30f, z1 = -1e30f;
                    for (const Vertex &v : d.mesh->vertices)
                        z0 = std::min(z0, v.z), z1 = std::max(z1, v.z);
                    const auto &px = d.fogPalette->pixels;
                    std::fprintf(stderr, "[native render]   fog palette %08x, alpha 0 %u 1 %u 8 %u 16 %u 32 %u 64 %u 128 %u 255 %u; quad z %g..%g\n", px[1],
                                 px[0] >> 24, px[1] >> 24, px[8] >> 24, px[16] >> 24, px[32] >> 24, px[64] >> 24, px[128] >> 24, px[255] >> 24, z0, z1);
                }
            }
        }
        if (const char *range = shotPath.empty() ? nullptr : std::getenv("TF_NATIVE_DRAWLOG"))
        {
            // (Debug: TF_NATIVE_DRAWLOG=a-b: the saved frame's draws a to b with their state.)
            const size_t a = std::strtoull(range, nullptr, 10);
            const char *dash = std::strchr(range, '-');
            const size_t b = std::min(dash ? static_cast<size_t>(std::strtoull(dash + 1, nullptr, 10)) : a, input.draws.size() - 1u);
            for (size_t k = a; k <= b && k < input.draws.size(); ++k)
            {
                const Draw &d = input.draws[k];
                float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
                uint32_t c0 = 0u;
                if (d.mesh)
                    for (const Vertex &v : d.mesh->vertices)
                        x0 = std::min(x0, v.x), x1 = std::max(x1, v.x), y0 = std::min(y0, v.y), y1 = std::max(y1, v.y), c0 = v.colour;
                float sx0 = 1e9f, sx1 = -1e9f, sy0 = 1e9f, sy1 = -1e9f; // (the GS screen the matrix puts them on, in front)
                if (d.mesh && !d.mesh->immediate)
                    for (const Vertex &v : d.mesh->vertices)
                    {
                        const float *m = d.matrix;
                        const float w = m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15];
                        if (w <= 0.0f)
                            continue;
                        const float gx = (m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12]) / w - d.originX,
                                    gy = (m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13]) / w - d.originY;
                        sx0 = std::min(sx0, gx), sx1 = std::max(sx1, gx), sy0 = std::min(sy0, gy), sy1 = std::max(sy1, gy);
                    }
                if (d.mesh && !d.mesh->immediate)
                    std::fprintf(stderr, "[native render]   screen %g..%g %g..%g\n", sx0, sx1, sy0, sy1);
                static const bool everyVertex = std::getenv("TF_NATIVE_DRAWVERTS") != nullptr; // (every vertex, not only small meshes')
                if (d.mesh && d.mesh->immediate && (d.mesh->vertices.size() <= 8u || everyVertex))
                    for (const Vertex &v : d.mesh->vertices)
                        std::fprintf(stderr, "[native render]     2D v %g %g %g c %08x uv %g %g q %g\n", v.x, v.y, v.z, v.colour, v.u, v.v, v.size);
                if (d.mesh && !d.mesh->immediate && (d.mesh->vertices.size() <= 8u || everyVertex))
                    for (const Vertex &v : d.mesh->vertices)
                    {
                        const float *m = d.matrix;
                        const float w = m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15];
                        std::fprintf(stderr, "[native render]     v %g %g %g c %08x uv %g %g size %g n %g %g %g -> %g %g w %g\n", v.x, v.y, v.z, v.colour, v.u, v.v, v.size, v.nx, v.ny, v.nz,
                                     (m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12]) / w - d.originX, (m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13]) / w - d.originY, w);
                    }
                std::fprintf(stderr, "[native render] draw %zu: frame %08x fbmsk %08x prim %x%s tex0 %016llx alpha %llx test %llx zbuf %llx, %zu verts (%s), xy %g..%g %g..%g, colour %08x, texture %d\n",
                             k, d.target, d.fbmsk, d.mesh ? d.mesh->prim : 0u, d.mesh && d.mesh->immediate ? " 2D" : "",
                             static_cast<unsigned long long>(d.tex0), static_cast<unsigned long long>(d.alpha), static_cast<unsigned long long>(d.test),
                             static_cast<unsigned long long>(d.zbuf), d.mesh ? d.mesh->vertices.size() : 0u,
                             d.mesh && d.mesh->sprites ? (d.mesh->primitive ? "primitive sprites" : "sprites") : d.mesh && d.mesh->primitive ? "primitive" : "mesh",
                             x0, x1, y0, y1, c0, d.texture != nullptr);
                static const bool drawVerts = std::getenv("TF_NATIVE_DRAWVERTS") != nullptr;
                if (drawVerts && d.mesh && !d.mesh->immediate)
                    std::fprintf(stderr, "[native render]   clamp %llx fromList %d scissor %g,%g..%g,%g\n", static_cast<unsigned long long>(d.clamp), d.fromList ? 1 : 0,
                                 d.scissor[0], d.scissor[1], d.scissor[2], d.scissor[3]);
                if (drawVerts && d.mesh && d.mesh->immediate)
                {
                    // (Debug: a 2D draw's screen, CLAMP and every vertex - xy in GS pixels, uv in texels.)
                    const float tw = static_cast<float>(1u << ((d.tex0 >> 26) & 0xFu)), th = static_cast<float>(1u << ((d.tex0 >> 30) & 0xFu));
                    std::fprintf(stderr, "[native render]   origin %g,%g scissor %g,%g..%g,%g clamp %llx scanmsk %u projective %d\n", d.originX, d.originY,
                                 d.scissor[0], d.scissor[1], d.scissor[2], d.scissor[3], static_cast<unsigned long long>(d.clamp), d.scanmsk,
                                 d.mesh->projective ? 1 : 0);
                    for (const Vertex &v : d.mesh->vertices)
                    {
                        const float q = d.mesh->projective && v.size != 0.0f ? v.size : 1.0f;
                        std::fprintf(stderr, "[native render]     %g,%g uv %g,%g c %08x z %g\n", v.x - d.originX, v.y - d.originY, v.u / q * tw, v.v / q * th, v.colour, v.z);
                    }
                }
                if (d.shader.count)
                {
                    std::string r;
                    char part[24];
                    for (uint32_t s = 0; s < d.shader.count; ++s)
                    {
                        std::snprintf(part, sizeof(part), " %04x(%d)", d.shader.address[s], static_cast<int>(d.shader.routine[s]));
                        r += part;
                    }
                    std::fprintf(stderr, "[native render]   routines%s (type %x, v0 size %g, q15 %g %g; prim2 %x)\n", r.c_str(),
                                 d.mesh ? d.mesh->type : 0u, d.mesh && !d.mesh->vertices.empty() ? d.mesh->vertices[0].size : 0.0f,
                                 d.shader.first[0] + 16u <= d.shader.data.size() ? d.shader.data[d.shader.first[0] + 15u].v[0] : 0.0f,
                                 d.shader.first[0] + 16u <= d.shader.data.size() ? d.shader.data[d.shader.first[0] + 15u].v[1] : 0.0f,
                                 d.mesh ? d.mesh->prim2 : 0u);
                    for (uint32_t s = 0; s < d.shader.count; ++s)
                        if (d.shader.routine[s] != Routine::Unknown && d.shader.routine[s] != Routine::Sprites && d.shader.first[s] + 3u <= d.shader.data.size())
                        {
                            const Qword *q = d.shader.data.data() + d.shader.first[s];
                            std::fprintf(stderr, "[native render]   %04x q0 %g %g %g %g q1 %g %g %g %g q2 %g %g %g %g (type %u normals %d colours %d)\n",
                                         d.shader.address[s], q[0].v[0], q[0].v[1], q[0].v[2], q[0].v[3], q[1].v[0], q[1].v[1], q[1].v[2], q[1].v[3],
                                         q[2].v[0], q[2].v[1], q[2].v[2], q[2].v[3], d.mesh ? d.mesh->type : 0u, d.mesh && d.mesh->hasNormal ? 1 : 0,
                                         d.mesh && d.mesh->hasColour ? 1 : 0);
                            if (d.shader.routine[s] == Routine::Projection && d.shader.first[s] + 9u <= d.shader.data.size())
                                std::fprintf(stderr, "[native render]     q3 %g %g %g %g q4 %g %g %g %g q8 %g %g %g %g\n", q[3].v[0], q[3].v[1], q[3].v[2], q[3].v[3],
                                             q[4].v[0], q[4].v[1], q[4].v[2], q[4].v[3], q[8].v[0], q[8].v[1], q[8].v[2], q[8].v[3]);
                        }
                }
                static const bool clipLog = std::getenv("TF_NATIVE_CLIPLOG") != nullptr;
                if (clipLog && d.mesh && !d.mesh->immediate)
                {
                    // (Debug: TF_NATIVE_CLIPLOG with the DRAWLOG: the core's q21 and 0x0480's clipping matrix,
                    // and each vertex through the screen matrix and the clipping one.)
                    const Qword *clipBlock = nullptr;
                    for (uint32_t s = 0; s < d.shader.count; ++s)
                        if (d.shader.address[s] == 0x0480u && d.shader.first[s] + 4u <= d.shader.data.size())
                            clipBlock = d.shader.data.data() + d.shader.first[s];
                    std::fprintf(stderr, "[native render]   q21 %g %g %g %g clip block %s\n", d.shader.q21[0], d.shader.q21[1], d.shader.q21[2],
                                 d.shader.q21[3], clipBlock ? "" : "none");
                    if (clipBlock)
                        for (int r = 0; r < 4; ++r)
                            std::fprintf(stderr, "[native render]     clip q%d %g %g %g %g\n", r, clipBlock[r].v[0], clipBlock[r].v[1], clipBlock[r].v[2], clipBlock[r].v[3]);
                    for (size_t vi = 0; vi < d.mesh->vertices.size() && vi < 24u; ++vi)
                    {
                        const Vertex &v = d.mesh->vertices[vi];
                        const float *m = d.matrix;
                        float sc[4], cc[4] = {};
                        for (int k = 0; k < 4; ++k)
                        {
                            sc[k] = m[k] * v.x + m[4 + k] * v.y + m[8 + k] * v.z + m[12 + k];
                            if (clipBlock)
                                cc[k] = clipBlock[0].v[k] * v.x + clipBlock[1].v[k] * v.y + clipBlock[2].v[k] * v.z + clipBlock[3].v[k];
                        }
                        std::fprintf(stderr, "[native render]     v%zu screen %g %g z %g w %g | clip %g %g %g %g\n", vi, sc[0] / sc[3], sc[1] / sc[3], sc[2], sc[3],
                                     cc[0], cc[1], cc[2], cc[3]);
                    }
                }
                if (d.mesh && d.mesh->sprites && d.shader.count)
                {
                    const Qword *q = d.shader.data.data() + d.shader.first[0];
                    std::fprintf(stderr, "[native render]   q10 %g %g %g %g q11 %g %g %g q14 %g %g %g q15 %g %g q18 %g %g, v0 size %g\n", q[10].v[0], q[10].v[1],
                                 q[10].v[2], q[10].v[3], q[11].v[0], q[11].v[1], q[11].v[2], q[14].v[0], q[14].v[1], q[14].v[2], q[15].v[0], q[15].v[1],
                                 q[18].v[0], q[18].v[1], d.mesh->vertices.empty() ? 0.0f : d.mesh->vertices[0].size);
                }
            }
        }
        if (!shotPath.empty() && std::getenv("TF_NATIVE_TEX1LOG"))
        {
            // (Debug: the saved frame's TEX1 values - the GS's filter and mip choice - over its textured
            // draws: 3D or 2D, context 1 and 2, how many, the first draw, how many of them upscaled.)
            struct Use
            {
                size_t count = 0, first = 0, upscaled = 0;
            };
            std::unordered_map<uint64_t, Use> uses;
            // (TF_NATIVE_TEXLIST=1: each textured 3D draw's textures - guest texture, size, large or not.)
            static const bool texList = std::getenv("TF_NATIVE_TEXLIST") != nullptr;
            if (texList)
                for (size_t k = 0; k < input.draws.size(); ++k)
                {
                    const Draw &d = input.draws[k];
                    if (!d.mesh || d.mesh->immediate || !(d.mesh->prim & 0x10u))
                        continue;
                    std::fprintf(stderr, "[native render] texlist draw %zu: ctx1 %08x %ux%u%s", k, d.texture ? d.texture->source : 0u,
                                 d.texture ? d.texture->width : 0u, d.texture ? d.texture->height : 0u, d.texture && d.texture->upscaled ? " large" : "");
                    if (d.mesh->prim2)
                        std::fprintf(stderr, " | ctx2 %08x %ux%u%s", d.texture2 ? d.texture2->source : 0u, d.texture2 ? d.texture2->width : 0u,
                                     d.texture2 ? d.texture2->height : 0u, d.texture2 && d.texture2->upscaled ? " large" : "");
                    std::fprintf(stderr, " (tex1 %llx)\n", static_cast<unsigned long long>(d.tex1));
                }
            for (size_t k = 0; k < input.draws.size(); ++k)
            {
                const Draw &d = input.draws[k];
                if (!d.mesh || !(d.mesh->prim & 0x10u))
                    continue;
                const uint64_t key = (d.tex1 & 0x00000FFF001FFFFFull) ^ (d.mesh->immediate ? 1ull << 63 : 0ull) ^ (d.fromList ? 1ull << 62 : 0ull);
                Use &u = uses[key];
                if (!u.count)
                    u.first = k;
                ++u.count;
                u.upscaled += d.texture && d.texture->upscaled ? 1u : 0u;
                if (d.mesh->prim2)
                {
                    Use &u2 = uses[(d.tex12 & 0x00000FFF001FFFFFull) ^ (1ull << 61)];
                    if (!u2.count)
                        u2.first = k;
                    ++u2.count;
                    u2.upscaled += d.texture2 && d.texture2->upscaled ? 1u : 0u;
                }
            }
            for (auto &u : uses)
            {
                const uint64_t t = u.first & 0x00000FFF001FFFFFull;
                std::fprintf(stderr, "[native render] tex1 %s%s: %016llx (LCM %u MXL %u MMAG %u MMIN %u L %u K %d/16): %zu draws from %zu, %zu upscaled\n",
                             (u.first >> 63) & 1u ? "2D" : "3D", (u.first >> 61) & 1u ? " ctx2" : (u.first >> 62) & 1u ? " list" : "",
                             static_cast<unsigned long long>(t), static_cast<unsigned>(t & 1u), static_cast<unsigned>((t >> 2) & 7u),
                             static_cast<unsigned>((t >> 5) & 1u), static_cast<unsigned>((t >> 6) & 7u), static_cast<unsigned>((t >> 19) & 3u),
                             static_cast<int>(static_cast<int32_t>(static_cast<uint32_t>(t >> 32) << 20) >> 20), u.second.count, u.second.first,
                             u.second.upscaled);
            }
        }
        static int areaLog = std::getenv("TF_NATIVE_AREALOG") ? std::atoi(std::getenv("TF_NATIVE_AREALOG")) : 0;
        if (areaLog > 0)
        {
            // (Debug: the immediate draws covering the most screen, in GS pixels.)
            --areaLog;
            std::vector<std::pair<double, const Draw *>> big;
            for (const Draw &d : input.draws)
            {
                if (!d.mesh || !d.mesh->immediate)
                    continue;
                double area = 0.0;
                const auto &v = d.mesh->vertices;
                const auto &ix = d.mesh->indices;
                for (size_t k = 0; k + 2 < ix.size(); k += 3)
                {
                    if (ix[k] >= v.size() || ix[k + 1] >= v.size() || ix[k + 2] >= v.size())
                    {
                        area += 1e18;
                        continue;
                    }
                    const Vertex &a0 = v[ix[k]], &a1 = v[ix[k + 1]], &a2 = v[ix[k + 2]];
                    area += std::abs((a1.x - a0.x) * (a2.y - a0.y) - (a2.x - a0.x) * (a1.y - a0.y)) * 0.5;
                }
                big.push_back({area, &d});
            }
            std::sort(big.begin(), big.end(), [](auto &l, auto &r) { return l.first > r.first; });
            for (size_t k = 0; k < big.size() && k < 5; ++k)
            {
                const Draw &d = *big[k].second;
                const Vertex &v0 = d.mesh->vertices[0];
                std::fprintf(stderr, "[native render] big immediate: area %.3g px, %zu vertices %zu indices, prim %x target %08x tex0 %016llx v0 %g,%g\n",
                             big[k].first, d.mesh->vertices.size(), d.mesh->indices.size(), d.mesh->prim, d.target,
                             static_cast<unsigned long long>(d.tex0), v0.x, v0.y);
            }
        }
        // The rows of each target the frame's 3D drew (its scissors), and where the lowest-starting
        // scissor begins. The scene buffer holds a frame's two fields (field A rows 0-223, field B -
        // the in-between, or with one picture a frame a field of one pixel - rows 224-447), woven
        // into the display buffer by two copies (SCANMSK 3 from A's rows, then 2 from B's). The 3D
        // here is drawn once, with field A's matrix and screen: the PC's picture is a whole frame,
        // so a copy whose field was never drawn here is left out and the other fills every line -
        // as the PS2 path does with one field shown (ps2_host_display::setShownField). A field
        // counts as drawn only by a draw whose scissor starts in it: the overlays drawn after the
        // scene across the whole buffer at field A's offset (the carrier's light shafts, scissor
        // 0-447) reach field B's rows without drawing that field, and must not pick its copy.
        std::unordered_map<uint32_t, float> drawnRows, fieldTop;
        for (const Draw &d : input.draws)
            if (d.mesh && !d.mesh->immediate && !depthFormat(d.target))
            {
                const uint32_t key = targetKey(d.target);
                float &rows = drawnRows[key];
                static const bool rowsLog = std::getenv("TF_NATIVE_ROWSLOG") != nullptr;
                if (rowsLog && !shotPath.empty() && d.scissor[3] + 1.0f > 224.5f)
                    std::fprintf(stderr, "[native render] rows: draw %zu into %04x scissor %g..%g origin %g, primitive %d fromList %d sprites %d type %u prim %x routines %u (%04x), %zu vertices\n",
                                 static_cast<size_t>(&d - input.draws.data()), key, d.scissor[1], d.scissor[3], d.originY,
                                 d.mesh->primitive, d.fromList, d.mesh->sprites, d.mesh->type, d.mesh->prim, d.shader.count,
                                 d.shader.count ? d.shader.address[0] : 0u, d.mesh->vertices.size());
                rows = std::max(rows, d.scissor[3] + 1.0f);
                if (d.scissor[3] >= d.scissor[1])
                {
                    float &top = fieldTop[key];
                    top = std::max(top, d.scissor[1]);
                }
            }
        // The 2D narrowed for a widescreen picture, as the PS2 path's front end does it (the HUD,
        // menus and text are drawn for 4:3; on the wider picture each group of them keeps its shape,
        // narrowed about the middle or - in the outer quarter, not touching the edge - about that
        // edge). A draw qualifies going to a displayed buffer with the depth test off or ALWAYS, a
        // scissor the buffer's width, no texture read from a frame buffer, not spanning the width.
        struct Squeeze
        {
            float pivot, factor, left, width;
            float clipLeft = -1.0f, clipRight = -1.0f; // (TODO 123: text held to where the 4:3 picture lands, in the buffer's pixels)
            bool edgesStay = true; // (TODO 128: vertices at the 4:3 picture's very edges kept at the wider picture's)
        };
        std::unordered_map<const Draw *, Squeeze> squeezes;
        if (input.overlaySqueeze < 0.9999f)
        {
            struct Placed
            {
                float x0, x1, y0, y1;
                int anchor;
                uint32_t tex = 0;  // (TODO 122: the texture's page, ~0u untextured - letters join a line above or below only in one atlas)
                int kept = 0;      // (TODO 122: the same place the frame before, pinned to that edge: 1 left, 2 right)
                const Draw *draw = nullptr; // (TODO 128: whose element)
            };
            // (TODO 122: the full-width boxes of the frame - the comm calls' panel, the autosave notice's - left out above,
            // their rows kept: what lies inside one in the top or bottom part is that box's content.)
            struct Bar
            {
                float y0, y1;
            };
            std::vector<Bar> bars;
            static const bool hudSticky = !(std::getenv("TF_NATIVE_HUDSTICKY") && *std::getenv("TF_NATIVE_HUDSTICKY") == '0');
            static std::vector<Placed> before; // the frame before's elements
            // (TODO 122: the HUD's corner pieces remembered - a piece at the same place, of the same texture, that was
            // pinned to an edge stays pinned while it still lies in that corner, whatever it is grouped with: the HUD
            // under a menu sliding out, a panel fading, the autosave notice - the HUD came back at its 4:3 place for
            // as long as such a thing overlapped it. Forgotten after kPinFrames frames unseen.)
            struct Pin
            {
                int edge;
                uint64_t seen;
            };
            static std::unordered_map<uint64_t, Pin> pins;
            static uint64_t pinFrame = 0;
            constexpr uint64_t kPinFrames = 72000u; // (5 minutes at 240 pictures a second: the HUD hidden through a cutscene and a menu)
            ++pinFrame;
            auto pinKey = [](const Placed &e) {
                uint64_t k = 0xCBF29CE484222325ull;
                for (const float f : {e.x0, e.x1, e.y0, e.y1})
                {
                    uint32_t b;
                    std::memcpy(&b, &f, 4);
                    k = (k ^ b) * 0x100000001B3ull;
                }
                return (k ^ e.tex) * 0x100000001B3ull;
            };
            std::vector<Placed> now;
            std::vector<std::pair<const Draw *, uint32_t>> textPieces; // (TODO 123: the textured 2D draws and their pages)
            std::unordered_map<uint32_t, uint32_t> letters;
            float squeezeLeft = 0.0f, squeezeWidth = 1.0f; // (the frame's 2D buffer: its offset and width, the groups' anchors)
            float squeezeTop = 0.0f, squeezeHeight = 448.0f;
            bool squeezeSet = false;
            // Where a group goes: narrowed about the edge of the 4:3 picture's outer quarter it lies in
            // (its middle there, not reaching the picture's middle, not touching the edge), else about the
            // middle - the front end's rule (gs_frontend.cpp anchorByPlace). And the HUD's corners
            // (2026-10-28, TODO 7 again): a group in the top or bottom part of the picture lying wholly in
            // its left or right half, not touching the edge, keeps to that edge too - the Mini-Con ring and
            // its highlight outline (middle at ~30% of the 4:3 width) went in from the bottom-left edge
            // with the front end's rule, in both renderers. TF_NATIVE_HUDCORNERS=0: the front end's rule.
            static const bool hudCorners = !(std::getenv("TF_NATIVE_HUDCORNERS") && *std::getenv("TF_NATIVE_HUDCORNERS") == '0');
            auto anchorOf = [&](float x0, float x1, float y0, float y1, float left, float width, float top, float height, bool group = false) {
                const float middle = (x0 + x1) * 0.5f;
                const bool leftHalf = x0 >= left + width * 0.01f && x1 < left + width * 0.5f;
                const bool rightHalf = x1 <= left + width * 0.99f && x0 > left + width * 0.5f;
                if (leftHalf && middle < left + width * 0.25f)
                    return 1;
                if (rightHalf && middle > left + width * 0.75f)
                    return 2;
                // (In those bands even a group reaching past the 4:3 edge: the ring's highlight outline
                // runs off the left edge - the front end's rule took the group for a banner drawn from the
                // edge and narrowed it about the middle.)
                // (The levels' HUD only: on the front end's screens - 2D alone - the Extras screen's scrolling line of text
                // at the bottom was pulled apart at the middle, its left half to the left edge and its right half to the
                // right, a gap between them the emulated GS's menus do not have. N9 stage 3, 2026-10-30.)
                const bool corner = hudCorners && input.has3D && height > 0.0f && (y0 >= top + height * 0.6f || y1 <= top + height * 0.4f);
                // (TODO 122, Safeguard: a group in one half reaching into the bottom 40% from no higher than the middle is a
                // bottom corner too - Safeguard's red swoosh rises from the Mini-Con ring to 58% of the picture's height (y 259
                // of 448), and the ring with it was taken for something in the middle: narrowed about the middle at its 4:3
                // place, the swoosh's end left at the picture's edge - the long red outline stretched back to it (TODO 7's).
                // Loaded with Safeguard on, the ring was never pinned before to be remembered. Only in one half: the autosave
                // notice's centred paragraph, 18..448 x 248..418, stays about the middle.
                // (TODO 138: a group's whole extent only - not each piece. A piece of a panel sliding in or out (its places
                // new every frame, so no anchor carried from the frame before) took its own: the Mini-Con panel's bottom-left
                // fill pieces, 246..278 high, came out pinned to the left edge, and the pieces and letters overlapping them
                // with them, while the panel was narrowed about the middle - two squares of its fill off to the right, the
                // start of each line of text at the screen's edge.)
                const bool low = group && hudCorners && input.has3D && height > 0.0f && y0 >= top + height * 0.5f && y1 >= top + height * 0.6f;
                if ((corner || low) && x1 < left + width * 0.5f)
                    return 1;
                if ((corner || low) && x0 > left + width * 0.5f)
                    return 2;
                // (TODO 122: the content of a full-width box in the top or bottom part starting in its left quarter - a comm
                // call's lines of text - keeps to the left edge whatever its length: Red Alert's lines took the left edge while
                // short, and went back to their 4:3 place once a line reached past the middle; the first Mini-Con's three long
                // lines never left it. What starts further in - the "Open" prompt in its bar - stays about the middle.
                // (TODO 130: and so does a line centred in its bar - as far from the left edge as from the right: the HQ's
                // "[X] View Mission Sites  [<][>] Change Menu", 91..420 of 512, starts in the left quarter too and went to the
                // left edge - its middle at a third of a 21:9 screen. A comm call's lines start at 32 and end at 376 at most.)
                const bool centred = std::fabs((x0 - left) - (left + width - x1)) <= width * 0.06f;
                if (corner && hudSticky && x0 < left + width * 0.25f && !centred)
                    for (const Bar &b : bars)
                        if ((b.y1 <= top + height * 0.4f || b.y0 >= top + height * 0.6f) && y0 >= b.y0 - 1.0f && y1 <= b.y1 + 1.0f)
                            return 1;
                return 0;
            };
            auto overlapping = [&](const Placed &a, const Placed &b) {
                const float ha = a.y1 - a.y0, hb = b.y1 - b.y0;
                // (TODO 122: lines of text in one atlas only - the autosave notice's last line, a few rows above the Mini-Con
                // ring, joined the ring's group, which then reached the picture's middle: the ring went to its 4:3 place.)
                const bool letters = ha > 0.0f && hb > 0.0f && std::max(ha, hb) <= 40.0f && std::max(ha, hb) <= 1.5f * std::min(ha, hb) &&
                                     (!hudSticky || !hudCorners || !input.has3D || a.tex == b.tex);
                const float gap = letters ? 1.25f * std::min(ha, hb) : 0.0f;
                return a.x0 <= b.x1 && a.x1 >= b.x0 && a.y0 <= b.y1 + gap && a.y1 + gap >= b.y0;
            };
            const uint32_t shown[2] = {static_cast<uint32_t>(input.dispfb[0]) & 0x1FFu, static_cast<uint32_t>(input.dispfb[1]) & 0x1FFu};
            const uint32_t mainBase = mainFbp & 0x1FFu;
            for (const Draw &d : input.draws)
            {
                if (!d.mesh || !d.mesh->immediate || d.mesh->vertices.empty() || depthFormat(d.target))
                    continue;
                const uint32_t fbp = d.target & 0x1FFu, fbw = (d.target >> 16) & 0x3Fu;
                // (A displayed buffer at its own width: a texture drawn into the same pages at another
                // width - the shadow texture in the back buffer's - is not the 2D.)
                const uint32_t shownFbw[2] = {static_cast<uint32_t>(input.dispfb[0] >> 9) & 0x3Fu, static_cast<uint32_t>(input.dispfb[1] >> 9) & 0x3Fu};
                if (!(fbp == mainBase && fbw == (mainFbp >> 9)) && !(fbp == shown[0] && fbw == shownFbw[0]) && !(fbp == shown[1] && fbw == shownFbw[1]))
                    continue;
                // (TF_NATIVE_GROUPLOG=2: a displayed buffer's 2D draws left unnarrowed, and why - drawn the full width)
                static const bool leftLog = std::getenv("TF_NATIVE_GROUPLOG") && *std::getenv("TF_NATIVE_GROUPLOG") == '2';
                auto leftOut = [&](const char *why) {
                    if (!leftLog || shotPath.empty())
                        return;
                    float lx0 = 1e9f, lx1 = -1e9f, ly0 = 1e9f, ly1 = -1e9f;
                    for (const Vertex &v : d.mesh->vertices)
                        lx0 = std::min(lx0, v.x), lx1 = std::max(lx1, v.x), ly0 = std::min(ly0, v.y), ly1 = std::max(ly1, v.y);
                    std::fprintf(stderr, "[native render] 2D left unnarrowed (%s): draw %zu, %zu vertices, prim %x tex0 %016llx test %llx scissor %g,%g..%g,%g, %g..%g x %g..%g\n", why,
                                 static_cast<size_t>(&d - input.draws.data()), d.mesh->vertices.size(), d.mesh->prim, static_cast<unsigned long long>(d.tex0),
                                 static_cast<unsigned long long>(d.test), d.scissor[0], d.scissor[1], d.scissor[2], d.scissor[3], lx0, lx1, ly0, ly1);
                };
                const bool zte = ((d.test >> 16) & 1u) != 0u;
                if (zte && ((d.test >> 17) & 3u) != 1u)
                {
                    leftOut("depth test");
                    continue;
                }
                const float width = static_cast<float>(fbw * 64u);
                if (width <= 0.0f || d.scissor[0] != 0.0f || d.scissor[2] + 1.0f < width)
                {
                    leftOut("scissor");
                    continue;
                }
                if (d.mesh->prim & 0x10u)
                {
                    const uint32_t tbp = static_cast<uint32_t>(d.tex0) & 0x3FFFu;
                    auto inside = [&](uint32_t base) { return tbp >= base * 32u && tbp < base * 32u + fbw * 512u; };
                    if (inside(fbp) || inside(static_cast<uint32_t>(d.zbuf) & 0x1FFu) || inside(shown[0]) || inside(shown[1]))
                    {
                        leftOut("frame read");
                        continue;
                    }
                }
                Placed e{1e9f, -1e9f, 1e9f, -1e9f, -1};
                for (const Vertex &v : d.mesh->vertices)
                {
                    e.x0 = std::min(e.x0, v.x);
                    e.x1 = std::max(e.x1, v.x);
                    e.y0 = std::min(e.y0, v.y);
                    e.y1 = std::max(e.y1, v.y);
                }
                const float left = d.originX;
                if (e.x0 <= left + width * 0.02f && e.x1 >= left + width * 0.98f)
                {
                    if (e.y1 > e.y0 && bars.size() < 64u)
                        bars.push_back({e.y0, e.y1});
                    continue;
                }
                e.tex = (d.mesh->prim & 0x10u) ? static_cast<uint32_t>(d.tex0) & 0x3FFFu : ~0u;
                if (!squeezeSet)
                    squeezeSet = true, squeezeLeft = left, squeezeWidth = width, squeezeTop = d.originY + d.scissor[1],
                    squeezeHeight = d.scissor[3] + 1.0f - d.scissor[1];
                const float area = (e.x1 - e.x0) * (e.y1 - e.y0);
                if (area > 0.0f && now.size() < before.size() && before[now.size()].x0 == e.x0 && before[now.size()].x1 == e.x1 &&
                    before[now.size()].y0 == e.y0 && before[now.size()].y1 == e.y1)
                    e.anchor = before[now.size()].anchor;
                else if (area > 0.0f)
                {
                    float best = 0.5f;
                    for (const Placed &p : before)
                    {
                        const float w = std::min(e.x1, p.x1) - std::max(e.x0, p.x0), h = std::min(e.y1, p.y1) - std::max(e.y0, p.y0);
                        if (w <= 0.0f || h <= 0.0f)
                            continue;
                        const float shared = w * h, ratio = shared / (area + (p.x1 - p.x0) * (p.y1 - p.y0) - shared);
                        if (ratio >= best)
                            best = ratio, e.anchor = p.anchor;
                    }
                    if (e.anchor < 0)
                        for (size_t i = now.size(); i-- > 0u;)
                            if (overlapping(e, now[i]))
                            {
                                e.anchor = now[i].anchor;
                                break;
                            }
                }
                if (hudSticky && area > 0.0f)
                    for (const Placed &p : before)
                        if (p.x0 == e.x0 && p.x1 == e.x1 && p.y0 == e.y0 && p.y1 == e.y1 && (p.anchor == 1 || p.anchor == 2))
                        {
                            e.kept = p.anchor;
                            break;
                        }
                if (e.anchor < 0)
                    e.anchor = anchorOf(e.x0, e.x1, e.y0, e.y1, left, width, d.originY + d.scissor[1], d.scissor[3] + 1.0f - d.scissor[1]);
                if (hudSticky && hudCorners && input.has3D && area > 0.0f && e.anchor == 0)
                    if (const auto pin = pins.find(pinKey(e)); pin != pins.end() && pinFrame - pin->second.seen < kPinFrames &&
                                                               anchorOf(e.x0, e.x1, e.y0, e.y1, left, width, d.originY + d.scissor[1],
                                                                        d.scissor[3] + 1.0f - d.scissor[1]) == pin->second.edge)
                        e.anchor = pin->second.edge;
                e.draw = &d;
                if (area > 0.0f && now.size() < 1024u)
                    now.push_back(e);
                const float pivot = e.anchor == 1 ? left : e.anchor == 2 ? left + width : left + width * 0.5f;
                if (leftLog && !shotPath.empty())
                    std::fprintf(stderr, "[native render] 2D narrowed: draw %zu anchor %d kept %d, %g..%g x %g..%g, tex0 %016llx\n", static_cast<size_t>(&d - input.draws.data()), e.anchor,
                                 e.kept, e.x0, e.x1, e.y0, e.y1, static_cast<unsigned long long>(d.tex0));
                squeezes[&d] = Squeeze{pivot, input.overlaySqueeze, left, width};
                if (e.tex != ~0u)
                {
                    // (TODO 123: the text pages - a texture page drawn as many letter-sized pieces this frame)
                    const bool letter = e.x1 - e.x0 <= 24.0f && e.y1 - e.y0 <= 24.0f;
                    textPieces.push_back({&d, e.tex});
                    if (letter)
                        ++letters[e.tex];
                }
            }
            // The text drawn outside the 4:3 picture (TODO 123): the PS2 shows only its 512 pixels across, and the game
            // draws what lies beyond them carelessly - a description sliding in from the left, a line revealed letter by
            // letter, the Extras ticker's ends: letters still off the screen are drawn squashed or stretched into long
            // strips (the Mini-Con panel's lines start 352 pixels left of the picture, each line's first piece a strip
            // reaching to the visible text). Narrowed about the middle for a wider picture, that part lands in the margins
            // left and right of the 4:3 picture: long white streaks until the text settled. Each draw of a text page is
            // held to where the 4:3 picture lands under its own narrowing - nothing the PS2 shows is cut, the panels
            // around the text still reach into the margins. TF_NATIVE_TEXTCLIP=0: drawn whole, as before.
            static const bool textClip = !(std::getenv("TF_NATIVE_TEXTCLIP") && *std::getenv("TF_NATIVE_TEXTCLIP") == '0');
            if (textClip)
                for (const auto &[dp, page] : textPieces)
                {
                    const auto c = letters.find(page);
                    if (c == letters.end() || c->second < 8u)
                        continue;
                    Squeeze &q = squeezes[dp];
                    q.clipLeft = q.pivot + (q.left - q.pivot) * q.factor - q.left;
                    q.clipRight = q.pivot + (q.left + q.width - q.pivot) * q.factor - q.left;
                }
            // (The groups: elements overlapping one another, transitively, take the anchor of the
            // group's whole extent by its place - the frame before's list, as the front end keeps it
            // (gs_frontend.cpp closeOverlayFrame). Before 2026-10-28 an element took the anchor of
            // the first one it overlapped, decided by that one's own extent: the Mini-Con ring's
            // pieces, each in the outer quarter, and its highlight outline reaching to the middle
            // went their own ways - the ring narrowed about the middle, in from the bottom-left
            // edge (TODO 7). The group's extent: the emulated picture's anchors.)
            static const bool oldGroups = std::getenv("TF_NATIVE_OLDGROUPS") && *std::getenv("TF_NATIVE_OLDGROUPS") == '1'; // (debug: as before)
            if (oldGroups)
            {
                for (size_t i = 0; i < now.size(); ++i)
                    for (size_t j = 0; j < i; ++j)
                        if (overlapping(now[i], now[j]))
                            now[i].anchor = now[j].anchor;
                before = std::move(now);
            }
            else
            {
                const size_t n = now.size();
                std::vector<uint32_t> group(n);
                for (size_t i = 0; i < n; ++i)
                    group[i] = static_cast<uint32_t>(i);
                const auto root = [&](uint32_t i) {
                    while (group[i] != i)
                        i = group[i] = group[group[i]];
                    return i;
                };
                for (size_t i = 1; i < n; ++i)
                    for (size_t j = 0; j < i; ++j)
                        if (overlapping(now[i], now[j]))
                        {
                            const uint32_t a = root(static_cast<uint32_t>(i)), b = root(static_cast<uint32_t>(j));
                            if (a != b)
                                group[std::max(a, b)] = std::min(a, b);
                        }
                std::vector<Placed> out = now;
                for (size_t i = 0; i < n; ++i)
                {
                    Placed &r = out[root(static_cast<uint32_t>(i))];
                    r.x0 = std::min(r.x0, now[i].x0);
                    r.x1 = std::max(r.x1, now[i].x1);
                    r.y0 = std::min(r.y0, now[i].y0);
                    r.y1 = std::max(r.y1, now[i].y1);
                }
                const float left = squeezeLeft, width = squeezeWidth;
                // (TODO 122: a group holding pieces that stood still, pinned to an edge the frame before, keeps that edge while
                // it stays in the top or bottom part or in that half - a comm call's next line reaching past the middle, a
                // Mini-Con icon flying into the ring with its trail do not take the HUD's corner back to its 4:3 place.)
                std::vector<int> keptOf(n, 0);
                for (size_t i = 0; i < n; ++i)
                    if (now[i].kept)
                    {
                        int &k = keptOf[root(static_cast<uint32_t>(i))];
                        k = k == 0 || k == now[i].kept ? now[i].kept : -1;
                    }
                for (size_t i = 0; i < n; ++i)
                    if (root(static_cast<uint32_t>(i)) == i)
                    {
                        out[i].anchor = anchorOf(out[i].x0, out[i].x1, out[i].y0, out[i].y1, left, width, squeezeTop, squeezeHeight, true);
                        const bool band = squeezeHeight > 0.0f && (out[i].y0 >= squeezeTop + squeezeHeight * 0.6f || out[i].y1 <= squeezeTop + squeezeHeight * 0.4f);
                        const bool half = keptOf[i] == 1 ? out[i].x1 < left + width * 0.5f : out[i].x0 > left + width * 0.5f;
                        if (out[i].anchor == 0 && hudSticky && hudCorners && input.has3D && keptOf[i] > 0 && (band || half))
                            out[i].anchor = keptOf[i];
                    }
                // (TODO 128: a group reaching both edges of the 4:3 picture - the HQ's More Info card, its two panels from the
                // left edge to the right - is a box the PS2 showed whole, its frame at the screen's edges. The vertices at the
                // very edges stay at the wider picture's (a banner drawn from the edge keeps its end there), so its outermost
                // frame pieces, 32 pixels each, were stretched from the narrowed card out to the screen's edges: the right
                // panel's edge drawn as wide light-blue bands, the left panel reaching the screen's left edge. Such a group is
                // narrowed whole. TF_NATIVE_CARDEDGES=0: stretched, as before.)
                static const bool cardEdges = !(std::getenv("TF_NATIVE_CARDEDGES") && *std::getenv("TF_NATIVE_CARDEDGES") == '0');
                if (cardEdges)
                    for (size_t i = 0; i < n; ++i)
                    {
                        const Placed &r = out[root(static_cast<uint32_t>(i))];
                        if (r.x0 <= left + width * 0.005f && r.x1 >= left + width * 0.995f && now[i].draw)
                            if (const auto sq = squeezes.find(now[i].draw); sq != squeezes.end())
                            {
                                // (TODO 131: except a piece that runs on past the edge - its texture not ended where the 4:3
                                // picture is: the card's left panel and its title bar, drawn from the left edge with the
                                // texture taken from inside (u 0.01-0.02, the panel's fill) as if they went on off the PS2's
                                // screen. Narrowed with the card they ended in a hard cut at the 4:3 picture's edge; they
                                // still reach the wide screen's edge, as they did before. A piece whose texture ends at the
                                // edge - the right panel's frame, u 1 - is the card's end and is narrowed with it.)
                                bool runsOn = false;
                                const Draw &dd = *now[i].draw;
                                if (dd.mesh && (dd.mesh->prim & 0x10u))
                                {
                                    float px0 = 1e9f, px1 = -1e9f, pu0 = 1e9f, pu1 = -1e9f;
                                    bool atEdge = false;
                                    for (const Vertex &v : dd.mesh->vertices)
                                    {
                                        // (the texture coordinate: s/q for the GIF's STQ - the card's sprites)
                                        const float u = dd.mesh->projective && v.size != 0.0f ? v.u / v.size : v.u;
                                        const bool edgeV = std::fabs(v.x - left) < 0.75f || std::fabs(v.x - (left + width)) < 0.75f;
                                        atEdge = atEdge || edgeV;
                                        if (edgeV && u > 0.005f && u < 0.995f)
                                            runsOn = true;
                                        px0 = std::min(px0, v.x), px1 = std::max(px1, v.x), pu0 = std::min(pu0, u), pu1 = std::max(pu1, u);
                                    }
                                    // (TODO 145: and a piece whose texture is stretched across it - more than twice its texels: a
                                    // menu bar's body, the right half of a 64-texel bar drawn 400 pixels wide to the PS2 screen's
                                    // edge (the Stasis Lock menu's buttons, the pause OPTIONS screen's value panels), which the TV
                                    // cut off there. Narrowed with the menu they ended flat at the 4:3 picture's edge; now they run
                                    // on to the wide screen's edge. The card's frame pieces are drawn at their size (32 pixels for
                                    // 32 texels) and are its ends.)
                                    // (TODO 166: or squashed into it - under three quarters of its texels: the pause OPTIONS screen's
                                    // Music and SFX rows end in the value panels' box, its right half (32 texels) squeezed into the 20
                                    // pixels left to the PS2 screen's edge, as if the box went on past it. Narrowed with the menu it was
                                    // a thin sliver at the 4:3 picture's edge while the rows below ran on to the wide screen's.)
                                    const float texels = (pu1 - pu0) * static_cast<float>(1u << ((dd.tex0 >> 26) & 0xFu));
                                    static const bool squashedOn = !(std::getenv("TF_NATIVE_SQUASHEDEDGE") && *std::getenv("TF_NATIVE_SQUASHEDEDGE") == '0');
                                    if (atEdge && texels > 0.0f && ((px1 - px0) > 2.0f * texels || (squashedOn && (px1 - px0) < 0.75f * texels)))
                                        runsOn = true;
                                }
                                // (TODO 145 debug: TF_NATIVE_EDGELOG - each edge piece's vertices at the edges: x, u, v, and runsOn)
                                static const bool edgeLog = std::getenv("TF_NATIVE_EDGELOG") != nullptr;
                                if (edgeLog && !shotPath.empty() && dd.mesh)
                                {
                                    std::string line;
                                    char vb[96];
                                    for (const Vertex &v : dd.mesh->vertices)
                                    {
                                        const float u = dd.mesh->projective && v.size != 0.0f ? v.u / v.size : v.u;
                                        const float vv = dd.mesh->projective && v.size != 0.0f ? v.v / v.size : v.v;
                                        std::snprintf(vb, sizeof vb, " (%.1f,%.1f u%.4f v%.4f)", v.x - left, v.y - squeezeTop, u, vv);
                                        line += vb;
                                    }
                                    std::fprintf(stderr, "[native render] edge piece draw %zu prim %x tex0 %016llx runsOn %d:%s\n", static_cast<size_t>(now[i].draw - input.draws.data()),
                                                 dd.mesh->prim, static_cast<unsigned long long>(dd.tex0), runsOn ? 1 : 0, line.substr(0, 700).c_str());
                                }
                                if (!runsOn)
                                    sq->second.edgesStay = false;
                            }
                    }
                static const bool groupLog = std::getenv("TF_NATIVE_GROUPLOG") != nullptr;
                if (groupLog)
                {
                    // (TODO 122: the groups and their anchors, a line each time they change)
                    static std::string lastSig;
                    std::string sig;
                    char buf[200];
                    for (size_t i = 0; i < n; ++i)
                        if (root(static_cast<uint32_t>(i)) == i)
                        {
                            size_t members = 0;
                            int kept = 0;
                            for (size_t j = 0; j < n; ++j)
                                if (root(static_cast<uint32_t>(j)) == i)
                                    ++members, kept += now[j].kept != 0;
                            std::snprintf(buf, sizeof buf, " [%.0f..%.0f x %.0f..%.0f a%d n%zu k%d t%x]", out[i].x0 - left, out[i].x1 - left,
                                          out[i].y0 - squeezeTop, out[i].y1 - squeezeTop, out[i].anchor, members, kept, now[i].tex);
                            sig += buf;
                        }
                    for (const Bar &b : bars)
                    {
                        std::snprintf(buf, sizeof buf, " bar(%.0f..%.0f)", b.y0 - squeezeTop, b.y1 - squeezeTop);
                        sig += buf;
                    }
                    if (sig != lastSig)
                        std::fprintf(stderr, "[native render] groups at tick %llu (width %.0f, rows %.0f):%s\n", static_cast<unsigned long long>(input.tick),
                                     width, squeezeHeight, sig.c_str());
                    lastSig = sig;
                }
                for (size_t i = 0; i < n; ++i)
                {
                    out[i].anchor = out[root(static_cast<uint32_t>(i))].anchor;
                    out[i].x0 = now[i].x0;
                    out[i].x1 = now[i].x1;
                    out[i].y0 = now[i].y0;
                    out[i].y1 = now[i].y1;
                }
                if (hudSticky && hudCorners && input.has3D)
                {
                    for (size_t i = 0; i < n; ++i)
                    {
                        // (each piece's own place: still in its corner - the piece, not its group)
                        const int own = anchorOf(now[i].x0, now[i].x1, now[i].y0, now[i].y1, left, width, squeezeTop, squeezeHeight);
                        if (own != 1 && own != 2)
                            continue;
                        const uint64_t key = pinKey(now[i]);
                        if (out[i].anchor == own)
                            pins[key] = Pin{own, pinFrame};
                        else if (const auto pin = pins.find(key); out[i].anchor == 0 && pin != pins.end() && pin->second.edge == own &&
                                                                  pinFrame - pin->second.seen < kPinFrames)
                        {
                            out[i].anchor = own;
                            pin->second.seen = pinFrame;
                        }
                    }
                    if (pins.size() > 4096u)
                        for (auto it = pins.begin(); it != pins.end();)
                            it = pinFrame - it->second.seen >= kPinFrames ? pins.erase(it) : std::next(it);
                }
                // (TODO 136 / 138: the anchors decided here, the group's - kept edge and pins included - are this frame's too.
                // They went to the next frame only (the list it matches its pieces against), and this frame drew each piece
                // by its own: an element's first picture, and every picture of a panel sliding in or out (its pieces' places
                // new each frame, nothing matched), took the anchor of whichever piece came first - the comm call's
                // "(X) Confirm (O) Repeat" line and its "Respond" pinned left by the button symbol at its start, then centred
                // the picture after; the Mini-Con panel's first letters and two squares of its fill pinned left while the
                // panel was narrowed about the middle. TF_NATIVE_GROUPNOW=0: the next frame only, as before.)
                static const bool groupNow = !(std::getenv("TF_NATIVE_GROUPNOW") && *std::getenv("TF_NATIVE_GROUPNOW") == '0');
                if (groupNow)
                    for (size_t i = 0; i < out.size(); ++i)
                        if (now[i].draw)
                            if (const auto sq = squeezes.find(now[i].draw); sq != squeezes.end())
                            {
                                Squeeze &q = sq->second;
                                const float pivot = out[i].anchor == 1 ? q.left : out[i].anchor == 2 ? q.left + q.width : q.left + q.width * 0.5f;
                                if (pivot == q.pivot)
                                    continue;
                                const bool clipped = q.clipRight > q.clipLeft;
                                q.pivot = pivot;
                                if (clipped)
                                {
                                    q.clipLeft = q.pivot + (q.left - q.pivot) * q.factor - q.left;
                                    q.clipRight = q.pivot + (q.left + q.width - q.pivot) * q.factor - q.left;
                                }
                            }
                before = std::move(out);
            }
        }
        {
            static const bool cbAlt = std::getenv("TF_NATIVE_CBVRAM") && *std::getenv("TF_NATIVE_CBVRAM") == 'a';
            g_cbVram = g.constantsV && g.preList && !(cbAlt && (g_profWindow & 1u));
        }
        d3d12Device().beginRings();
        State::Target *current = nullptr;
        // (TODO 94: the draw state last set in the list - a scissor, blend factor or pipeline the same as the draw
        // before's is not set again. On AMD each is a context register: set, it starts a new context for the draws
        // after, and a run of small draws each starting one waits on the GPU's few contexts. TF_NATIVE_STATEFILTER=0:
        // every draw sets them, as before; =alt: by turns.)
        static const char *stateFilterEnv = std::getenv("TF_NATIVE_STATEFILTER");
        const bool stateFilter = speedupOn(stateFilterEnv);
        bool currentFog = false;
        // The anti-aliasing pass (TF_NATIVE_AA): run once the picture holds the frame's 3D - drawn
        // into it, or copied in from a target holding it (the scene buffer's field copies, the glow
        // made from the picture) - just before the first draw into the picture that carries none:
        // the HUD, menus and text are drawn after it, as sharp as before. At the frame's end if no
        // such draw came; never on a picture with no 3D (the front end's menus).
        const bool aaWanted = g.aaPipeline && aaOn();
        bool aaDone = false, picture3D = false;
        size_t aaAt = 0;
        std::unordered_set<const State::Target *> holds3D;
        // The sun's occlusion probe (roadmap N3a): the rect the game's visibility test copied this frame, run where its copy
        // into the OcclusionBuffer (a PSMZ24 frame at page 384, left out below) comes in the frame, from the largest target
        // the 3D went into so far - or at the frame's end if no such copy came.
        float probeRect[4] = {};
        bool probeWanted = occlusion::probeFor(input.tick, probeRect), probeDone = false;
        State::Target *probeScene = nullptr;
        static const bool probeLog = std::getenv("TF_NATIVE_OCCLOG") != nullptr;
        auto probe = [&](bool atCopy) {
            if (!probeWanted || probeDone || !probeScene)
                return;
            probeDone = true;
            const bool ran = runProbe(*probeScene, probeRect, input.tick);
            current = nullptr; // (the next draw's target, depth, viewport and pipeline bound again)
            static int told = 8;
            if (probeLog && told > 0)
            {
                --told;
                std::fprintf(stderr, "[native render] occlusion probe of frame %llu %s: rect %.1f,%.1f-%.1f,%.1f of a %.0fx%.0f target (%ux%u pixels, depth view %u)%s\n",
                             static_cast<unsigned long long>(input.tick), atCopy ? "at its copy" : "at the frame's end", probeRect[0], probeRect[1],
                             probeRect[2], probeRect[3], probeScene->gsW, probeScene->gsH, probeScene->w, probeScene->h, gfx::device().view(probeScene->id, true),
                             ran ? "" : " - not run");
            }
        };
        d3d12Device().beginDraws(stateFilter);
        size_t drawn = 0, fromTargets = 0, left = 0, topReads = 0;
        struct AfterSave
        {
            size_t index = 0;
            ID3D12Resource *readback = nullptr;
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
            UINT w = 0, h = 0;
            bool split = false; // (TF_NATIVE_SPLIT's: the picture, kept small in runs\split_<tick>.bin)
        };
        std::vector<AfterSave> afterSaves;
        // (TODO 99, a check: TF_NATIVE_SPLIT=<n> - on a saved frame, the picture after every n-th draw (the first draw to get
        // there), each a 160 x 90 grey image in runs\split_<tick>.bin: where in the frame a picture goes wrong)
        static const size_t splitStep = std::getenv("TF_NATIVE_SPLIT") ? static_cast<size_t>(std::atoi(std::getenv("TF_NATIVE_SPLIT"))) : 0u;
        size_t nextSplit = splitStep;
        static const std::vector<size_t> afterList = [] {
            std::vector<size_t> out;
            for (const char *e = std::getenv("TF_NATIVE_SAVE_AFTER"); e && *e;)
            {
                char *end = nullptr;
                const size_t v = std::strtoull(e, &end, 10);
                if (end == e)
                    break;
                out.push_back(v);
                e = *end == ',' ? end + 1 : end;
            }
            return out;
        }();
        std::unordered_set<uint32_t> drawnTargets; // (the targets drawn into so far this frame)
        // (Debug: TF_NATIVE_SKIP=a-b[,c-d]: the frame's draws a to b left out, to find which draw makes what.)
        static const std::vector<std::pair<size_t, size_t>> skips = [] {
            std::vector<std::pair<size_t, size_t>> out;
            if (const char *e = std::getenv("TF_NATIVE_SKIP"))
            {
                std::string s(e);
                size_t at = 0;
                while (at < s.size())
                {
                    size_t comma = s.find(',', at);
                    if (comma == std::string::npos)
                        comma = s.size();
                    const std::string part = s.substr(at, comma - at);
                    const size_t dash = part.find('-');
                    const size_t a = std::strtoull(part.c_str(), nullptr, 10);
                    out.emplace_back(a, dash == std::string::npos ? a : std::strtoull(part.c_str() + dash + 1, nullptr, 10));
                    at = comma + 1;
                }
            }
            return out;
        }();
        // (N9 stage 3) What an extra picture of the frame can leave out: the targets no picture's pose reaches - none of
        // their draws moves between frames (Draw::hasB, turned) and none reads a target that does - drawn the same in every
        // picture (the player's shadow, reflections, a HUD buffer): drawn by the frame's first picture alone, kept for the
        // rest. Only where every read of the target comes after its last write in the frame (a target read before it is
        // drawn - last frame's, double buffered - would be read as this frame's in the extra pictures). Its draw calls run
        // under predication: the picture's value (`predicate`, 1 in an extra picture) skips them; the barriers, copies and
        // clears still run, so the resources' states are the same either way.
        std::unordered_set<uint32_t> skippable;
        {
            // (OFF by default, TF_POSES_SKIP=1 on: on the RX 5700 XT predicated draws hung the GPU - a device hung in Amazon
            // every time, 2026-10-30, even with only the draw calls predicated - and it saved ~0.7 ms of an extra picture's
            // ~6 there. A list per stretch of draws, the skipped stretches' barriers kept, is the way if it is wanted.)
            static const bool skipOn = std::getenv("TF_POSES_SKIP") && *std::getenv("TF_POSES_SKIP") == '1';
            if (input.pictures.size() > 1u && skipOn && g.predicate)
            {
                auto readKey = [](const Draw &d, uint32_t &key) {
                    if (!d.mesh || !(d.mesh->prim & 0x10u))
                        return false;
                    const uint32_t tbp = static_cast<uint32_t>(d.tex0) & 0x3FFFu, tbw = static_cast<uint32_t>(d.tex0 >> 14) & 0x3Fu;
                    if ((tbp % 32u) != 0u)
                        return false;
                    key = (tbp / 32u) | tbw << 9;
                    return true;
                };
                std::unordered_set<uint32_t> dependent{mainFbp};
                for (bool changed = true; changed;)
                {
                    changed = false;
                    for (const Draw &d : input.draws)
                    {
                        const uint32_t t = targetKey(d.target);
                        if (depthFormat(d.target) || dependent.count(t))
                            continue;
                        uint32_t r = 0;
                        if (d.hasB || d.turned || d.hasClipB || !d.instB.empty() || (readKey(d, r) && dependent.count(r)))
                        {
                            dependent.insert(t);
                            changed = true;
                        }
                    }
                }
                std::unordered_map<uint32_t, size_t> lastWrite, firstRead;
                for (size_t i = 0; i < input.draws.size(); ++i)
                {
                    const Draw &d = input.draws[i];
                    if (depthFormat(d.target))
                        continue;
                    uint32_t r = 0;
                    if (readKey(d, r) && !firstRead.count(r))
                        firstRead[r] = i;
                    lastWrite[targetKey(d.target)] = i;
                }
                for (const auto &[t, w] : lastWrite)
                {
                    const auto r = firstRead.find(t);
                    if (!dependent.count(t) && (r == firstRead.end() || r->second > w))
                        skippable.insert(t);
                }
            }
        }
        bool predicated = false;
        auto predicate = [&](bool on) {
            if (on != predicated)
            {
                g.list->SetPredication(on ? g.predicate : nullptr, on ? 8ull * g.slot : 0ull, on ? D3D12_PREDICATION_OP_NOT_EQUAL_ZERO : D3D12_PREDICATION_OP_EQUAL_ZERO);
                predicated = on;
            }
        };
        passMark(7u);
        for (const Draw &draw : input.draws)
        {
            if (depthFormat(draw.target))
            {
                if (probeWanted && !probeDone && (draw.target & 0x1FFu) == 384u)
                    probe(true); // (the OcclusionBuffer's copy: page 384)
                continue;
            }
            // (TODO 161 a: a frame not shown - its draws into the picture and into the targets the newer frame draws left
            // out; a render-to-texture of its own still drawn)
            if (input.dropped && !input.droppedDrawn)
            {
                const uint32_t key = targetKey(draw.target);
                if (key == mainFbp || std::find(input.superseded.begin(), input.superseded.end(), key) != input.superseded.end())
                    continue;
            }
            // (predication around the draw calls alone: a copy or clear under it hung the RX 5700 XT - a device hung,
            // 2026-10-30 - so the copies a skipped target's draws make for themselves still run)
            const bool skipDraw = !skippable.empty() && skippable.count(targetKey(draw.target)) != 0u;
            if (!skips.empty())
            {
                const size_t index = static_cast<size_t>(&draw - input.draws.data());
                bool skip = false;
                for (const auto &r : skips)
                    skip = skip || (index >= r.first && index <= r.second);
                if (skip)
                    continue;
            }
            // (Debug: TF_NATIVE_SKIPROUTINE=<hex>: the draws that ran that VU1 routine left out.)
            static const uint32_t skipRoutine = std::getenv("TF_NATIVE_SKIPROUTINE") ? static_cast<uint32_t>(std::strtoul(std::getenv("TF_NATIVE_SKIPROUTINE"), nullptr, 16)) : 0u;
            if (skipRoutine)
            {
                bool has = false;
                for (uint32_t s = 0; s < draw.shader.count; ++s)
                    has = has || draw.shader.address[s] == skipRoutine;
                if (has)
                    continue;
            }
            static const uint32_t tbpLog = std::getenv("TF_NATIVE_TBPLOG") ? static_cast<uint32_t>(std::strtoul(std::getenv("TF_NATIVE_TBPLOG"), nullptr, 16)) : 0u;
            if (tbpLog && !shotPath.empty() && (static_cast<uint32_t>(draw.tex0) & 0x3FFFu) == tbpLog && draw.mesh)
                std::fprintf(stderr, "[native render] tbp draw %zu: target %08x prim %x immediate %d, %zu vertices, texture %d alpha %llx\n",
                             static_cast<size_t>(&draw - input.draws.data()), draw.target, draw.mesh->prim, draw.mesh->immediate,
                             draw.mesh->vertices.size(), draw.texture != nullptr, static_cast<unsigned long long>(draw.alpha));
            auto found = g.targets.find(targetKey(draw.target));
            if (found == g.targets.end() || !found->second.id)
                continue;
            State::Target &tg = found->second;
            if (draw.cloud)
            {
                // (TODO 27: the warp tunnel's sparks - their own pipeline; the next draw binds its target, state and
                // pipeline again)
                drawCloud(draw, tg);
                current = nullptr; // (its own pipeline and state: the next draw binds its target, and with it all, again)
                continue;
            }
            prof::record += prof::since(profT);
            // (TODO 204: a ground blend layer with its colours softened - layersoft.)
            const bool softLayer = layersoft::isLayer(draw);
            GpuMesh *m = draw.mesh ? (softLayer ? uploadSoft(draw.mesh) : upload(draw.mesh)) : nullptr;
            // (N9 stage 3: the frame before's vertices of a mesh made anew each frame, a stream of their own - place, colour,
            // size - in the frame's ring; the rest read a zero stride's one entry)
            gfx::RingSpan vertexB; // (empty: none)
            bool hasVertexB = false;
            if (m && draw.meshB && draw.meshB->vertices.size() == draw.mesh->vertices.size() && g.dynamic)
            {
                const UINT64 bytes = draw.meshB->vertices.size() * kVertexBStride;
                gfx::RingSpan vbSpan;
                if (dev.ringAlloc(gfx::RingKind::Vertices, static_cast<uint32_t>(bytes), 16u, vbSpan))
                {
                    std::vector<uint8_t> packed(static_cast<size_t>(bytes));
                    const std::vector<Vertex> &verticesB = softLayer ? layersoft::entry(draw.meshB).vertices : draw.meshB->vertices;
                    for (size_t k = 0; k < verticesB.size(); ++k)
                    {
                        const Vertex &v = verticesB[k];
                        std::memcpy(packed.data() + k * kVertexBStride, &v.x, 12u);
                        std::memcpy(packed.data() + k * kVertexBStride + 12u, &v.colour, 4u);
                        std::memcpy(packed.data() + k * kVertexBStride + 16u, &v.size, 4u);
                    }
                    std::memcpy(vbSpan.cpu, packed.data(), packed.size());
                    vertexB = vbSpan;
                    hasVertexB = true;
                }
            }
            prof::meshes += prof::since(profT);
            if (!m)
                continue;
            g.passes.clear();
            buildPasses(draw, g.passes);
            size_t passesDone = 0; // (the passes an instanced draw drew already)
            for (size_t passIndex = 0; passIndex < g.passes.size(); ++passIndex)
            {
                const Pass &pass = g.passes[passIndex];
                if (passIndex < passesDone)
                    continue;
                // The texture: the game's, else a render target this frame drew (its pages), else
                // none (a 2D draw sampling memory nobody drew is left out).
                prof::record += prof::since(profT);
                GpuTexture *t = pass.texture ? textureFor(pass.texture, !input.synchronousGameTextures) : nullptr;
                prof::textures += prof::since(profT);
                const bool standIn = t == &g_standIn; // (its colour in g_standInColour: read before another textureFor)
                float standInColour[4];
                std::memcpy(standInColour, g_standInColour, sizeof(standInColour));
                State::Target *source = nullptr;
                UINT selfSlot = 0u; // (a target sampling itself: its copy's view)
                static const bool selfReadOff = std::getenv("TF_NATIVE_SELFREAD") && *std::getenv("TF_NATIVE_SELFREAD") == '0'; // (debug: left out, as before)
                const bool tme = (pass.prim & 0x10u) != 0u;
                // The depth fog (its palette and the target's own depth; not on the picture itself).
                const gfx::TextureId tgDepth = dev.view(tg.id, true); // (the target's depth as a texture; 0: none - the picture's)
                const bool fog = pass.fogPalette && !tg.main && tgDepth;
                GpuTexture *fogPalette = fog ? textureFor(pass.fogPalette) : nullptr;
                static const bool fogLog = std::getenv("TF_NATIVE_FOGLOG") != nullptr;
                if (fogLog && !shotPath.empty() && pass.fogPalette)
                    std::fprintf(stderr, "[native render] fog pass of draw %zu into %04x: fog %d (main %d, depth view %u), palette %d, test %llx zbuf %llx\n",
                                 static_cast<size_t>(&draw - input.draws.data()), found->first, fog ? 1 : 0, tg.main ? 1 : 0, tgDepth,
                                 fogPalette != nullptr, static_cast<unsigned long long>(pass.test), static_cast<unsigned long long>(pass.zbuf));
                if (pass.fogPalette && !fogPalette)
                {
                    ++left;
                    continue;
                }
                // A T8H read of pages a target of this frame drew (the shadow textures): that
                // target's green through the palette (topPalette), not the converted GS memory.
                State::Target *topSource = nullptr;
                GpuTexture *topPalette = nullptr;
                if (pass.topPalette && tme && !fog)
                {
                    const uint32_t tbp = static_cast<uint32_t>(pass.tex0) & 0x3FFFu, tbw = static_cast<uint32_t>(pass.tex0 >> 14) & 0x3Fu;
                    const uint32_t key = (tbp / 32u) | tbw << 9;
                    auto rt = (tbp % 32u) == 0u && drawnTargets.count(key) ? g.targets.find(key) : g.targets.end();
                    if (rt != g.targets.end() && rt->second.id && &rt->second != &tg)
                        if ((topPalette = textureFor(pass.topPalette)) != nullptr)
                        {
                            topSource = source = &rt->second;
                            t = nullptr;
                            if (!topReads && !shotPath.empty() && std::getenv("TF_NATIVE_TOPLOG") && pass.topPalette->pixels.size() >= 256u)
                            {
                                const auto &px = pass.topPalette->pixels;
                                std::fprintf(stderr, "[native render] top-byte read of %04x (tex0 %016llx, alpha %llx): palette 0 %08x 16 %08x 32 %08x 64 %08x 96 %08x 128 %08x 160 %08x 192 %08x 255 %08x\n",
                                             key, static_cast<unsigned long long>(pass.tex0), static_cast<unsigned long long>(pass.alpha), px[0], px[16], px[32],
                                             px[64], px[96], px[128], px[160], px[192], px[255]);
                            }
                            ++topReads;
                        }
                    // The lens filters (taLensFilter: Antarctica's grey-blue grade, the sensors' tints):
                    // the scene's own green, moved into its top byte by CopyGreenToAlpha, read as T8H
                    // through the filter's palette and blended over the scene itself - from a copy of the
                    // target as it is now. (Left to the converted GS memory, the grade read a stale
                    // picture's green: Antarctica bluer and brighter than the GS draws it.)
                    static const bool selfTopOff = std::getenv("TF_NATIVE_LENSFILTER") && *std::getenv("TF_NATIVE_LENSFILTER") == '0';
                    if (!topSource && !selfTopOff && !selfReadOff && rt != g.targets.end() && &rt->second == &tg && tg.id && !tg.main)
                        if ((topPalette = textureFor(pass.topPalette)) != nullptr && (selfSlot = dev.snapshot(tg.id)) != 0u)
                        {
                            topSource = source = &tg;
                            t = nullptr;
                            ++topReads;
                        }
                }
                static const std::pair<uint32_t, uint32_t> targetDraws = [] {
                    const char *e = std::getenv("TF_NATIVE_TARGETDRAWS");
                    char *end = nullptr;
                    const uint32_t a = e ? static_cast<uint32_t>(std::strtoul(e, &end, 16)) : 0u;
                    const uint32_t b = e && end && *end == ',' ? static_cast<uint32_t>(std::strtoul(end + 1, nullptr, 16)) : a;
                    return std::make_pair(a, b);
                }();
                if (targetDraws.first && !shotPath.empty() && (targetDraws.first == 0xFFFFFFFFu || found->first == targetDraws.first || found->first == targetDraws.second) &&
                    !draw.mesh->vertices.empty())
                    std::fprintf(stderr, "[native render] draw %zu into %04x: prim %x tex0 %016llx clamp %llx texture %d (game %d) top %d alpha %llx test %llx fbmsk %08x frame %08x zbuf %llx, %zu vertices, v0 %g,%g c %08x uv %g,%g\n",
                                 static_cast<size_t>(&draw - input.draws.data()), found->first, pass.prim, static_cast<unsigned long long>(pass.tex0), static_cast<unsigned long long>(pass.clamp),
                                 t == nullptr ? 0 : standIn ? 2 : 1, pass.texture != nullptr, topSource != nullptr, static_cast<unsigned long long>(pass.alpha),
                                 static_cast<unsigned long long>(pass.test), pass.fbmsk, draw.target, static_cast<unsigned long long>(pass.zbuf),
                                 draw.mesh->vertices.size(), draw.mesh->vertices[0].x,
                                 draw.mesh->vertices[0].y, draw.mesh->vertices[0].colour, draw.mesh->vertices[0].u, draw.mesh->vertices[0].v);
                if (targetDraws.first && !shotPath.empty() && (targetDraws.first == 0xFFFFFFFFu || found->first == targetDraws.first || found->first == targetDraws.second) && pass.texture)
                    std::fprintf(stderr, "[native render]   texture id %llu %ux%u upscaled %d format %u, gpu slot %u, stand-in colour %.2f %.2f %.2f %.2f\n",
                                 static_cast<unsigned long long>(pass.texture->id), pass.texture->width, pass.texture->height, pass.texture->upscaled ? 1 : 0,
                                 pass.texture->format, t ? t->slot : 9999u, standInColour[0], standInColour[1], standInColour[2], standInColour[3]);
                if (!t && tme && !fog && !topSource)
                {
                    const uint32_t tbp = static_cast<uint32_t>(pass.tex0) & 0x3FFFu, tbw = static_cast<uint32_t>(pass.tex0 >> 14) & 0x3Fu;
                    auto rt = (tbp % 32u) == 0u ? g.targets.find((tbp / 32u) | tbw << 9) : g.targets.end();
                    if (rt != g.targets.end() && rt->second.id && &rt->second != &tg)
                        source = &rt->second;
                    else if (rt != g.targets.end() && rt->second.id && !tg.main && !selfReadOff && ((draw.target >> 24) & 0x3Fu) <= 1u &&
                             ((pass.tex0 >> 20) & 0x3Fu) == ((draw.target >> 24) & 0x3Fu) && (selfSlot = dev.snapshot(tg.id)) != 0u)
                        source = &tg; // (the target read while drawn into - the glow's channel shifts: from a copy of it as it is now)
                    else if (draw.mesh->immediate || draw.mesh->primitive)
                    {
                        // (Debug: TF_NATIVE_LEFTLOG=<n>: the saved frame's first n draws left out for want of a texture.)
                        static int leftLog = std::getenv("TF_NATIVE_LEFTLOG") ? std::atoi(std::getenv("TF_NATIVE_LEFTLOG")) : 0;
                        if (leftLog > 0 && !shotPath.empty())
                        {
                            --leftLog;
                            std::fprintf(stderr, "[native render] left out: draw %zu %s prim %x tex0 %016llx (tbp %04x psm %x) alpha %llx target %08x, %zu vertices, game texture %d\n",
                                         static_cast<size_t>(&draw - input.draws.data()), draw.mesh->immediate ? "2D" : "primitive", pass.prim,
                                         static_cast<unsigned long long>(pass.tex0), static_cast<uint32_t>(pass.tex0) & 0x3FFFu,
                                         static_cast<uint32_t>(pass.tex0 >> 20) & 0x3Fu, static_cast<unsigned long long>(pass.alpha), draw.target,
                                         draw.mesh->vertices.size(), pass.texture != nullptr);
                        }
                        ++left;
                        continue;
                    }
                }
                {
                    // (3D: a geometry set, an instance, a microcode primitive - anything not a 2D GIF draw.)
                    const bool carries3D = !draw.mesh->immediate || (source && holds3D.count(source));
                    if (aaWanted && !aaDone && tg.main && picture3D && !carries3D)
                    {
                        auto aaT = prof::clock::now();
                        if (runAa(tg))
                        {
                            aaAt = static_cast<size_t>(&draw - input.draws.data());
                            current = nullptr; // (the draw's target, depth and viewport bound again)
                        }
                        aaDone = true;
                        g_aaCpu += prof::since(aaT);
                        ++g_aaFrames;
                    }
                    if (carries3D)
                    {
                        holds3D.insert(&tg);
                        picture3D = picture3D || tg.main;
                    }
                    if (!draw.mesh->immediate && tgDepth && (!probeScene || tg.gsW * tg.gsH > probeScene->gsW * probeScene->gsH))
                        probeScene = &tg;
                }
                if (&tg != current || fog != currentFog)
                {
                    dev.use(tg.id, gfx::UseFor::Draw);
                    // (The depth read by the fog's pass: its read-only view, the shader's state too.)
                    if (!tg.main)
                        dev.useDepth(tg.id, fog ? gfx::DepthUse::ReadSample : gfx::DepthUse::Write);
                    // (Anything run between draws - the anti-aliasing - sets its own: all set again after a target change.)
                    dev.setTarget(tg.id, true);
                    ++g_cntTargets;
                    dev.setViewport(0.0f, 0.0f, static_cast<float>(tg.w), static_cast<float>(tg.h));
                    current = &tg;
                    currentFog = fog;
                }
                {
                    // The draw's scissor, in the target's pixels.
                    const float sx = static_cast<float>(tg.w) / tg.gsW, sy = static_cast<float>(tg.h) / tg.gsH;
                    gfx::Rect r{static_cast<int32_t>(draw.scissor[0] * sx), static_cast<int32_t>(draw.scissor[1] * sy),
                                static_cast<int32_t>((draw.scissor[2] + 1.0f) * sx), static_cast<int32_t>((draw.scissor[3] + 1.0f) * sy)};
                    r.x1 = std::min<int32_t>(r.x1, static_cast<int32_t>(tg.w));
                    r.y1 = std::min<int32_t>(r.y1, static_cast<int32_t>(tg.h));
                    if (const auto sq = squeezes.find(&draw); sq != squeezes.end() && sq->second.clipRight > sq->second.clipLeft)
                    {
                        // (TODO 123: a text page's draw held to where the 4:3 picture lands)
                        r.x0 = std::max<int32_t>(r.x0, static_cast<int32_t>(std::floor(sq->second.clipLeft * sx)));
                        r.x1 = std::min<int32_t>(r.x1, static_cast<int32_t>(std::ceil(sq->second.clipRight * sx)));
                        r.x1 = std::max(r.x1, r.x0);
                    }
                    dev.setScissor(r);
                }
                static const bool copyLog = std::getenv("TF_NATIVE_COPYLOG") != nullptr;
                if (copyLog && !shotPath.empty() && tg.main && !draw.mesh->vertices.empty())
                {
                    const Vertex &v0 = draw.mesh->vertices[0], &vl = draw.mesh->vertices.back();
                    std::fprintf(stderr, "[native render] main draw: prim %x tex0 %016llx texture %d source %d (%.0fx%.0f) alpha %llx fbmsk %08x v %zu: (%g,%g uv %g,%g) .. (%g,%g uv %g,%g) origin %g,%g colour %08x %08x\n",
                                 pass.prim, static_cast<unsigned long long>(pass.tex0), t != nullptr, source != nullptr, source ? source->gsW : 0.0f,
                                 source ? source->gsH : 0.0f, static_cast<unsigned long long>(pass.alpha), pass.fbmsk, draw.mesh->vertices.size(),
                                 v0.x, v0.y, v0.u, v0.v, vl.x, vl.y, vl.u, vl.v, draw.originX, draw.originY, v0.colour, vl.colour);
                    if (((source && pass.prim == 0x14u) || (pass.prim == 0x5bu && draw.mesh->vertices.size() == 12u)) && std::getenv("TF_NATIVE_COPYVERTS"))
                        for (const Vertex &v : draw.mesh->vertices)
                            std::fprintf(stderr, "[native render]   copy vertex %g,%g uv %g,%g q %g\n", v.x, v.y, v.u, v.v, v.size);
                }
                // A texture the draw has but that is not on the GPU this frame (put off by the
                // per-frame upload cap, or refused): left out until it is - drawn now it took the
                // white of slot 0 (2026-10-28: solid white enemies and debris, items 55/61). Draws
                // with no texture at all keep the white, as before.
                if (tme && pass.texture && !t && !source && !topSource && !selfSlot && !fog)
                {
                    ++left;
                    continue;
                }
                static const bool whiteLog = std::getenv("TF_NATIVE_WHITELOG") != nullptr;
                if (whiteLog && !shotPath.empty() && tme && !t && !source && !draw.mesh->immediate)
                    std::fprintf(stderr, "[native render] untextured: mesh %llu type %u prim %x, %zu vertices, tex0 %016llx, draw texture %d, target %04x, shader %u routines first %04x\n",
                                 static_cast<unsigned long long>(draw.mesh->id), draw.mesh->type, pass.prim, draw.mesh->vertices.size(),
                                 static_cast<unsigned long long>(pass.tex0), pass.texture != nullptr, targetKey(draw.target), draw.shader.count,
                                 draw.shader.count ? draw.shader.address[0] : 0u);
                if (source && draw.scanmsk && !topSource)
                {
                    const uint32_t key = ((static_cast<uint32_t>(pass.tex0) & 0x3FFFu) / 32u) | (static_cast<uint32_t>(pass.tex0 >> 14) & 0x3Fu) << 9;
                    auto drawn = drawnRows.find(key);
                    float lowest = 1e9f;
                    for (const Vertex &v : draw.mesh->vertices)
                        lowest = std::min(lowest, draw.mesh->projective && v.size != 0.0f ? v.v / v.size : v.v);
                    const float firstRow = lowest * static_cast<float>(1u << ((pass.tex0 >> 30) & 0xFu));
                    const auto top = fieldTop.find(key);
                    const bool fieldDrawn = firstRow < 0.5f || (top != fieldTop.end() && top->second >= firstRow - 0.5f);
                    if (drawn != drawnRows.end() && (firstRow >= drawn->second - 0.5f || !fieldDrawn))
                    {
                        ++left;
                        continue; // (the field this copy weaves in was not drawn)
                    }
                }
                gfx::TextureId sourceSlot = 0u;
                if (source && !selfSlot)
                {
                    if (source->main)
                        ++g_pictureReads; // (TF_NATIVE_PROFILE: passes reading the picture itself)
                    sourceSlot = dev.view(source->id);
                    dev.use(source->id, gfx::UseFor::Sample);
                }
                if (source)
                    ++fromTargets;
                // (Made on the stack and copied into the ring whole: the ring is an upload heap - write-combined,
                // uncached - and every read of a field back from it, a `-=` or a comparison, cost about a
                // microsecond: the half-pixel move below on every pass was 0.8 ms of the frame's recording.)
                Constants c{};
                std::memcpy(c.rows, pass.matrix, sizeof(c.rows));
                c.screen[0] = draw.originX;
                c.screen[1] = draw.originY;
                // The GS covers the pixels whose integer coordinates the primitive holds - pixel n shows
                // its sample at n over [n, n+1) - the GPU those whose centres: the primitive is moved by
                // half a GS pixel so that a target pixel takes the sample of the GS pixel it lies in. At the
                // GS's own resolution (an effect's buffer) the game's -0.5..127.5 copies lost their last
                // row and column without it; at the picture's scale every target was half a GS pixel up and
                // left of the GS's - the copies of the 3D field (-0.5..511.5) left the picture's last two
                // or three columns at the clear colour (a dark strip down the right edge of every menu and
                // level), and a target read back (the copies' +0.5 texel) came out a further half pixel off.
                // TF_NATIVE_HALFPIXEL=0: at the GS's resolution only, as before.
                static const bool halfPixelUnitOnly = std::getenv("TF_NATIVE_HALFPIXEL") && *std::getenv("TF_NATIVE_HALFPIXEL") == '0';
                if (!halfPixelUnitOnly || (tg.w == static_cast<UINT>(tg.gsW) && tg.h == static_cast<UINT>(tg.gsH)))
                {
                    c.screen[0] -= 0.5f;
                    c.screen[1] -= 0.5f;
                }
                c.screen[2] = 2.0f / tg.gsW;
                c.screen[3] = 2.0f / tg.gsH;
                c.misc[0] = input.depthScale;
                c.misc[1] = draw.mesh->hasColour ? 1.0f : 0.0f;
                const uint64_t h = draw.mesh->id * 0x9E3779B97F4A7C15ull;
                c.misc[2] = 0.3f + 0.6f * static_cast<float>((h >> 8) & 0xFF) / 255.0f;
                c.misc[3] = 0.3f + 0.6f * static_cast<float>((h >> 16) & 0xFF) / 255.0f;
                c.texScale[0] = c.texScale[1] = 1.0f;
                c.texScale[2] = draw.mesh->projective ? 1.0f : 0.0f;
                c.texScale[3] = fog ? 1.0f : topSource ? 2.0f : 0.0f;
                c.squeeze[1] = 1.0f;
                {
                    // CLAMP: WMS 0-1, WMT 2-3, MINU 4-13, MAXU 14-23, MINV 24-33, MAXV 34-43 (region
                    // bounds in the GS texture's texels; region repeat, 3, is left as repeat).
                    const uint32_t wms = static_cast<uint32_t>(pass.clamp) & 3u, wmt = static_cast<uint32_t>(pass.clamp >> 2) & 3u;
                    c.wrapS[0] = wms == 3u ? 0.0f : static_cast<float>(wms);
                    c.wrapT[0] = wmt == 3u ? 0.0f : static_cast<float>(wmt);
                    c.wrapS[1] = static_cast<float>((pass.clamp >> 4) & 0x3FFu);
                    c.wrapS[2] = static_cast<float>((pass.clamp >> 14) & 0x3FFu);
                    c.wrapS[3] = static_cast<float>(1u << ((pass.tex0 >> 26) & 0xFu));
                    c.wrapT[1] = static_cast<float>((pass.clamp >> 24) & 0x3FFu);
                    c.wrapT[2] = static_cast<float>((pass.clamp >> 34) & 0x3FFu);
                    c.wrapT[3] = static_cast<float>(1u << ((pass.tex0 >> 30) & 0xFu));
                    // (TODO 126) A 2D draw whose coordinates stay inside its texture - u and v within 0..1, nothing repeated -
                    // held to the texture's edge like a CLAMP: the GS at its own size never samples past the last texel's
                    // centre there, but at a higher resolution the pixels nearest the edge do, and the filter wrapped round to
                    // the far side - a dark line through every Extras gallery picture (drawn as a strip 512 texels wide and a
                    // column beside it: the strip's right edge took in its left border). Only small meshes are looked at (the
                    // tunnel's points are not). TF_NATIVE_EDGEHOLD=0: wrapped, as before.
                    static const bool edgeHoldOff = std::getenv("TF_NATIVE_EDGEHOLD") && *std::getenv("TF_NATIVE_EDGEHOLD") == '0';
                    if (!edgeHoldOff && !source && draw.mesh->immediate && draw.mesh->vertices.size() <= 4096u && (c.wrapS[0] == 0.0f || c.wrapT[0] == 0.0f))
                    {
                        // (Grass grown on the GPU has no vertices here; its blades' coordinates are 0..1 by construction -
                        // corners of kSt and the 0.5 centre, q 1 - so the loop finds nothing and both are held, as the CPU
                        // path holds every ground of up to 1024 blades.)
                        float u0 = 1e30f, u1 = -1e30f, v0 = 1e30f, v1 = -1e30f;
                        for (const Vertex &v : draw.mesh->vertices)
                        {
                            const float q = v.size != 0.0f ? v.size : 1.0f;
                            u0 = std::min(u0, v.u / q), u1 = std::max(u1, v.u / q), v0 = std::min(v0, v.v / q), v1 = std::max(v1, v.v / q);
                        }
                        const float eps = 1.0e-4f;
                        if (c.wrapS[0] == 0.0f && u0 >= -eps && u1 <= 1.0f + eps)
                            c.wrapS[0] = 1.0f;
                        if (c.wrapT[0] == 0.0f && v0 >= -eps && v1 <= 1.0f + eps)
                            c.wrapT[0] = 1.0f;
                    }
                    static const bool targetClampOff = std::getenv("TF_NATIVE_TARGETCLAMP") && *std::getenv("TF_NATIVE_TARGETCLAMP") == '0'; // (debug: wrapped, as before)
                    if (source && !topSource && targetClampOff)
                        c.wrapS[0] = c.wrapT[0] = 0.0f;
                    else if (source && !topSource)
                    {
                        // A render target: its coordinates are in its own extent (texScale), so the
                        // clamp's bounds are too - CLAMP to its edge texels, a region to the region's
                        // (in GS texels, which the target's extent counts). Left as the sampler's wrap,
                        // the glow's warp pulled texels in from the far side of its buffer (the warp
                        // gate's swirl, the lasers' and the dropships' glows drawn elsewhere, tinted).
                        // (The edge is the texture's - TW, TH - or the target's where it is smaller: a
                        // target grown by another frame's larger scissor has rows no draw wrote.)
                        const float edgeS = std::min(c.wrapS[3], source->gsW) - 1.0f, edgeT = std::min(c.wrapT[3], source->gsH) - 1.0f;
                        if (c.wrapS[0] == 1.0f)
                            c.wrapS[0] = 2.0f, c.wrapS[1] = 0.0f, c.wrapS[2] = edgeS;
                        if (c.wrapT[0] == 1.0f)
                            c.wrapT[0] = 2.0f, c.wrapT[1] = 0.0f, c.wrapT[2] = edgeT;
                        c.wrapS[2] = std::min(c.wrapS[2], edgeS);
                        c.wrapT[2] = std::min(c.wrapT[2], edgeT);
                        c.wrapS[3] = source->gsW;
                        c.wrapT[3] = source->gsH;
                    }
                }
                static const bool pinnedEdges = []() { const char *e = std::getenv("TF_NATIVE_PINNEDEDGES"); return !(e && *e == '0'); }();
                if (const auto sq = squeezes.find(&draw); sq != squeezes.end())
                {
                    c.squeeze[0] = sq->second.pivot;
                    c.squeeze[1] = sq->second.factor;
                    c.squeeze[2] = sq->second.left;
                    c.squeeze[3] = sq->second.width;
                    // (TODO 122: the pivot and the edges moved with the picture's half-pixel move above - the screen's
                    // origin is half a GS pixel left of originX, the pivot was not: what was narrowed sat
                    // 0.5 x (1 - factor) / factor of a GS pixel right of its 4:3 place, 0.4 at 21:9 - 1.5 pixels at 1440
                    // rows, measured on the Mini-Con ring against the game's 4:3 view.)
                    if (pinnedEdges)
                        c.squeeze[0] += c.screen[0] - draw.originX, c.squeeze[2] += c.screen[0] - draw.originX;
                    if (!sq->second.edgesStay)
                        c.squeeze[2] -= 1.0e6f, c.squeeze[3] += 2.0e6f; // (no vertex counts as at the edge: all narrowed)
                    // (TODO 122, Safeguard / TODO 7: a draw pinned to an edge is narrowed whole beyond that edge too. The
                    // vertices kept at the wider picture's edges are all those outside the 4:3 picture's edge band - also
                    // the ones past the edge: the Mini-Con ring's texture quads start at x -24 and Safeguard's red sweep
                    // fan at -12.5, and those vertices stayed at the wide picture's place while the rest of the ring was
                    // narrowed: its left segments stretched wider and paler, the red rim spread out from the segments as a
                    // smeared arc - about the middle, the long stretched outline to the screen's edge (TODO 7). Pinned to
                    // that edge the pivot is the edge itself, so a vertex at it stays where it was either way.)
                    // (The shader's edge band is 0.5% of z/w's width: the far edge's bound is kept where it was -
                    // the left one moved out by D for the left edge, the right one for the right edge.)
                    else if (pinnedEdges && sq->second.pivot == sq->second.left)
                        c.squeeze[2] -= 1.0e5f, c.squeeze[3] += 1.0e5f / 0.995f; // (the left edge: no vertex kept there)
                    else if (pinnedEdges && sq->second.pivot == sq->second.left + sq->second.width)
                        c.squeeze[2] -= 0.005f * 1.0e5f, c.squeeze[3] += 1.0e5f; // (the right edge)
                }
                if (source)
                {
                    c.texScale[0] = static_cast<float>(1u << ((pass.tex0 >> 26) & 0xFu)) / source->gsW;
                    c.texScale[1] = static_cast<float>(1u << ((pass.tex0 >> 30) & 0xFu)) / source->gsH;
                }
                c.flags[0] = (t || source || fog) ? 1.0f : 0.0f;
                if (t && !source && !fog && !topSource)
                    tex1Constants(pass.tex1, c.lod, c.lodf);
                // (A copy from a supersampled target into the picture: its four taps.)
                if (ssaa && source && !topSource && !selfSlot && !fog && tg.main && source->w > static_cast<UINT>(source->gsW))
                    c.down[0] = 1.0f;
                // (N10, TF_NATIVE_BOXDOWN=1, off by default) A shrink of the picture itself into a target at the GS's own
                // size (the glow's first copy): the average over the footprint (the shader's boxdown). Measured (other PC,
                // 3440x1440): the glow changes by at most 9 levels in Deep Amazon (its 8 x 8 pyramid already averages the
                // single taps away), no shimmer to take out on Amazon, Deep Amazon or the HQ, and +0.2-0.3 ms of the GPU a
                // frame - so off; kept for an A/B.
                {
                    static const bool boxOn = std::getenv("TF_NATIVE_BOXDOWN") && *std::getenv("TF_NATIVE_BOXDOWN") == '1';
                    if (boxOn && source && source->main && !topSource && !selfSlot && !fog && c.down[0] < 0.5f && tg.w == static_cast<UINT>(tg.gsW) &&
                        tg.h == static_cast<UINT>(tg.gsH) && (source->w >= static_cast<UINT>(source->gsW) * 2u || source->h >= static_cast<UINT>(source->gsH) * 2u))
                        c.down[0] = 2.0f;
                }
                // (TODO 94, a measuring aid: TF_NATIVE_FLATPS=alt - every draw's pixels a flat grey in every other profile
                // window: what the pixel shader's work costs, against the rest of a draw.)
                {
                    static const bool flatAlt = std::getenv("TF_NATIVE_FLATPS") != nullptr;
                    c.down[1] = flatAlt && (g_profWindow & 1u) ? 1.0f : 0.0f;
                }
                if (standIn && t && !source && !fog && !topSource && !selfSlot)
                {
                    std::memcpy(c.standIn, standInColour, sizeof(c.standIn));
                    c.standIn[3] += 2.0f;
                }

                c.flags[1] = static_cast<float>((pass.tex0 >> 35) & 3u);
                c.flags[2] = static_cast<float>((pass.tex0 >> 34) & 1u);
                {
                    const uint32_t psm = static_cast<uint32_t>(pass.tex0 >> 20) & 0x3Fu;
                    c.flags[3] = psm == 0x01u || psm == 0x31u ? 1.0f : 0.0f;
                }
                c.test[0] = static_cast<float>(pass.test & 1u);
                c.test[1] = static_cast<float>((pass.test >> 1) & 7u);
                c.test[2] = static_cast<float>((pass.test >> 4) & 0xFFu);
                c.test[3] = static_cast<float>((pass.test >> 12) & 3u);
                std::memcpy(c.mode, pass.mode, sizeof(c.mode));
                static const bool fogDebug = std::getenv("TF_NATIVE_FOGDEBUG") != nullptr; // (debug: the fog pass shows its index, no depth test)
                if (fogDebug && fog)
                    c.mode[0] = 16.0f;
                static const bool spriteDebug = std::getenv("TF_NATIVE_SPRITEDEBUG") != nullptr;
                static int spriteDebugLog = 6;
                if (spriteDebug && pass.mode[3] > 0.5f && spriteDebugLog > 0 && !draw.mesh->vertices.empty())
                {
                    --spriteDebugLog;
                    const Vertex &v = draw.mesh->vertices[0];
                    const float *r = pass.matrix;
                    float sc[4];
                    for (int k = 0; k < 4; ++k)
                        sc[k] = r[k] * v.x + r[4 + k] * v.y + r[8 + k] * v.z + r[12 + k];
                    std::fprintf(stderr, "[native render] sprite pass: mesh sprites %d, %zu vertices, v0 %g %g %g size %g uv %g %g -> gs %g %g w %g, lo %g %g\n",
                                 draw.mesh->sprites, draw.mesh->vertices.size(), v.x, v.y, v.z, v.size, v.u, v.v, sc[0] / sc[3], sc[1] / sc[3], sc[3],
                                 pass.p[21][0] * v.size / sc[3], pass.p[21][1] * v.size / sc[3]);
                }
                if (spriteDebug && pass.mode[3] > 0.5f)
                    c.mode[0] = 10.0f, c.flags[0] = 0.0f;
                static const bool copyDebug = std::getenv("TF_NATIVE_COPYDEBUG") != nullptr;
                if (copyDebug && source && pass.prim == 0x14u)
                {
                    static const int how = std::atoi(std::getenv("TF_NATIVE_COPYDEBUG"));
                    if (how >= 2)
                        c.mode[0] = how == 2 ? 11.0f : 12.0f;
                    else
                        c.mode[0] = 10.0f, c.flags[0] = 0.0f;
                    if (!shotPath.empty())
                        std::fprintf(stderr, "[native render] copy pass: test %llx zbuf %llx alpha %llx into %04x (%s), source %ux%u gs %gx%g, texScale %g %g, mode %g %g %g %g, screen %g %g %g %g\n",
                                     static_cast<unsigned long long>(pass.test), static_cast<unsigned long long>(pass.zbuf), static_cast<unsigned long long>(pass.alpha),
                                     found->first, tg.main ? "the picture" : "a target", source->w, source->h, source->gsW, source->gsH, c.texScale[0], c.texScale[1],
                                     c.mode[0], c.mode[1], c.mode[2], c.mode[3], c.screen[0], c.screen[1], c.screen[2], c.screen[3]);
                }
                std::memcpy(c.p, pass.p, sizeof(c.p));
                if (pass.mode[0] == 6.0f)
                    std::memcpy(c.lightsB, pass.lightsB, sizeof(c.lightsB));
                else
                    std::memcpy(c.lightsB, pass.p[8], sizeof(c.lightsB));
                // (The core's clipping only where it cuts: a mesh whose box lies inside all six planes is drawn as
                // it is - no clip distances, no clipping shader. The waves' moved vertices keep it.)
                bool clips = pass.clips;
                if (clips && pass.mode[2] < 0.5f)
                {
                    if (!m->bounded)
                    {
                        boundsOf(*draw.mesh, *m);
                        m->bounded = true;
                    }
                    // (N9 stage 3: inside in this frame's pose and the frame before's - the pictures between lie between)
                    clips = !boxInside(*m, pass.clip) || !boxInside(*m, pass.clipB) || !boxInside(*m, pass.clipM);
                }
                if (pass.turned)
                    c.pose[0] = 1.0f; // (N9 stage 3: the camera's turn of each picture)
                if (hasVertexB)
                    c.pose[1] = 1.0f; // (N9 stage 3: the vertices blended from the frame before's)
                if (clips)
                {
                    std::memcpy(c.clipRows, pass.clip, sizeof(c.clipRows));
                    std::memcpy(c.clipRowsB, pass.clipB, sizeof(c.clipRowsB));
                    std::memcpy(c.clipRowsM, pass.clipM, sizeof(c.clipRowsM));
                    c.clipOn[0] = 1.0f;
                    // (The clipping shader's early way out for triangles wholly inside / outside; TF_NATIVE_CLIPFAST=0: off;
                    // =alt: on and off by turns, a profile window (TF_NATIVE_PROFILE, 300 frames) each - an A/B in one run.)
                    static const char *clipFast = std::getenv("TF_NATIVE_CLIPFAST");
                    const bool fastOn = !(clipFast && *clipFast == '0') && !(clipFast && *clipFast == 'a' && (g_profWindow & 1u));
                    c.clipOn[2] = fastOn ? 1.0f : 0.0f;
                }
                // (The vertex colour screen-linear, as the GS's Gouraud shading - see VsOut. TF_NATIVE_AFFINE=0:
                // perspective-correct, as before.)
                static const bool affineOff = std::getenv("TF_NATIVE_AFFINE") && *std::getenv("TF_NATIVE_AFFINE") == '0';
                c.clipOn[1] = affineOff ? 0.0f : 1.0f;
                static const bool anisoAlt = std::getenv("TF_NATIVE_ANISO_ALT") != nullptr;
                c.clipOn[3] = anisoAlt && (g_profWindow & 1u) ? 1.0f : 0.0f;
                // TEST's DATE: the frame's alpha read from a copy of the target taken now (DATM: which
                // side of 0x80 passes).
                c.dest[0] = c.dest[1] = c.dest[2] = c.dest[3] = 0.0f;
                gfx::TextureId destSlot = 0u;
                if (((pass.test >> 14) & 1u) != 0u && !tg.main && !fog && !topSource && (destSlot = dev.snapshot(tg.id)) != 0u)
                    c.dest[0] = ((pass.test >> 15) & 1u) != 0u ? 2.0f : 1.0f;
                // Depth: TEST's ZTE / ZTST (0 never, 1 always, 2 GEQUAL, 3 GREATER), ZBUF's ZMSK.
                const bool zte = ((pass.test >> 16) & 1u) != 0u && !(fogDebug && fog);
                const uint32_t ztst = static_cast<uint32_t>(pass.test >> 17) & 3u;
                const gfx::Compare func = draw::compareOfZtst(ztst);
                const bool zwrite = ((pass.zbuf >> 32) & 1u) == 0u;
                {
                    // (TODO 100: the depth-equal passes a little nearer - the shader's pose.z, a share of the depth: 1e-4, a ten-
                    // thousandth of the distance (5 mm at 50 m). TF_NATIVE_ZNUDGE=<share>, 0: off as before. Measured on the test PC
                    // in Amazon: Optimus' shadow missing or cut in about 1 frame of 5; 2e-6 still left cut ones.)
                    static const float zNudge = std::getenv("TF_NATIVE_ZNUDGE") ? static_cast<float>(std::atof(std::getenv("TF_NATIVE_ZNUDGE"))) : 1.0e-4f;
                    if (zNudge != 0.0f && zte && ztst == 2u && !zwrite)
                        c.pose[2] = zNudge;
                }
                const gfx::BlendState blend = draw::blendOf(pass.alpha, (pass.prim & 0x40u) != 0u);
                c.dest[1] = static_cast<float>(blend.premultiply);
                c.dest[2] = static_cast<float>((pass.alpha >> 32) & 0xFFu) / 128.0f;
                // (The GS-exact untextured colour: the glow's threshold - a constant subtracted from the
                // shrunk picture - takes off what the GS's does, not twice that. TF_NATIVE_UNTEXTURED=0:
                // as before, twice as bright.)
                static const bool untexturedHalf = !(std::getenv("TF_NATIVE_UNTEXTURED") && *std::getenv("TF_NATIVE_UNTEXTURED") == '0');
                c.dest[3] = untexturedHalf && (pass.prim & 0x10u) == 0u ? 1.0f : 0.0f;
                uint32_t writeMask = writeMaskOf(pass.fbmsk);
                {
                    // A 24-bit frame (FRAME's PSM CT24, or Z24 written as colour) has no alpha of
                    // its own: the GS leaves the top byte as it was. The 3D field is drawn CT24 but
                    // for the frame's alpha clear and the shadow receivers (CT32), whose alpha the
                    // grass reads back as Ad (ALPHA (0 - Cs) * Ad + Cs) - written by every draw,
                    // it was the last mesh's alpha there and the grass came out dark.
                    const uint32_t fpsm = (draw.target >> 24) & 0x3Fu;
                    if (fpsm == 0x01u || fpsm == 0x31u)
                        writeMask &= ~static_cast<uint32_t>(gfx::kWriteA);
                }
                // (Nothing written: the draw only touches what is masked - unless it writes depth. An
                // opened warp gate's portal is a disc drawn with FBMSK ffffffff and Z written (TEST ZTE,
                // GEQUAL; ZBUF's ZMSK off), a depth plug: what is drawn after it behind the portal - the
                // forest - fails the depth test and the frame's clear stays there, under the additive
                // swirl. Left out, the forest showed through the portal, tinted (2026-10-28, TODO 65).
                // TF_NATIVE_DEPTHONLY=0: left out, as before.)
                static const bool depthOnlyOff = std::getenv("TF_NATIVE_DEPTHONLY") && *std::getenv("TF_NATIVE_DEPTHONLY") == '0';
                if (writeMask == 0u && (depthOnlyOff || !zte || !zwrite))
                    continue;
                // TEST's AFAIL RGB_ONLY (3): a pixel failing the alpha test still writes its colour,
                // not its alpha (nor Z) - drawn as two passes: the passing pixels as they are, then the
                // failing ones without alpha. (The shadow receivers write the shadow's strength into
                // the frame's alpha where it passes; elsewhere the alpha the receivers before left.)
                static const bool afailOff = std::getenv("TF_NATIVE_AFAIL") && *std::getenv("TF_NATIVE_AFAIL") == '0'; // (debug: as before)
                const bool rgbOnlyFail = !afailOff && c.test[0] > 0.5f && c.test[3] == 3.0f && (writeMask & gfx::kWriteA) != 0u;
                if (rgbOnlyFail)
                    c.test[3] = 0.0f;
                // (TF_NATIVE_CLIPGS=0: the GPU's clipper with the planes; =alt: by turns, a profile window each.)
                // (Unset: the quality preset's - the GPU's clipper on Low and Medium, TODO 94.)
                static const char *clipGsEnv = std::getenv("TF_NATIVE_CLIPGS");
                // (N9 stage 3: and on every preset on an AMD card - there the clipping shader, a geometry shader, cost 4.4 ms
                // of the RX 5700 XT's 8 a picture in Amazon, against 0.4 of the GTX 1080's 3: the extra pictures between frames
                // had no room. TF_NATIVE_CLIPGS=1 keeps it.)
                static const bool clipGsPresetOff = qualityPreset() < 2 || adapterVendor() == 0x1002u;
                const bool clipGsOff = clipGsEnv && *clipGsEnv ? (*clipGsEnv == '0' || (*clipGsEnv == 'a' && (g_profWindow & 1u))) : clipGsPresetOff;
                const bool clipGs = clips && !clipGsOff;
                // (TODO 94, a measuring aid: TF_NATIVE_ZWALT=<mode>,<mode>,... - profile window w takes mode w % n: a mask of the
                // classes of depth-write-off draws made to write depth (1 those reading a target, 2 sprites, 4 2D, 8 the other 3D;
                // 16 the draws with no depth test at all - the picture goes black: nothing after them passes); 128 a pixel shader
                // doing nothing, 256 one reading only the interpolants, 512 the shader variants without the alpha test; 0 the game's.)
                uint32_t zwMode = 0u;
                {
                    static std::vector<uint32_t> modes = [] {
                        std::vector<uint32_t> v;
                        if (const char *e = std::getenv("TF_NATIVE_ZWALT"))
                            for (const char *k = e; *k;)
                            {
                                char *end = nullptr;
                                const unsigned long m = std::strtoul(k, &end, 10);
                                if (end == k)
                                    break;
                                v.push_back(static_cast<uint32_t>(m));
                                k = *end == ',' ? end + 1 : end;
                            }
                        return v;
                    }();
                    if (!modes.empty())
                        zwMode = modes[g_profWindow % modes.size()];
                }
                bool zwriteNow = zwrite;
                if (zwMode & 15u)
                {
                    const uint32_t cls = source ? 1u : (draw.mesh && draw.mesh->sprites) ? 2u : (draw.mesh && draw.mesh->immediate) ? 4u : 8u;
                    if (!zwrite && zte && (zwMode & cls))
                        zwriteNow = true;
                }
                if ((zwMode & 16u) && !zwrite && !zte)
                    zwriteNow = true; // (16: the draws without a depth test - the full-screen 2D passes - written at their depth)
                const bool canDiscard = (c.test[0] > 0.5f && c.test[1] != 1.0f) || c.dest[0] > 0.5f || c.mode[0] == 9.0f || c.mode[0] == 13.0f ||
                                        rgbOnlyFail;
                // The pixel shader's variant (TODO 94): the draw's paths alone compiled in (psVariantOf).
                // TF_NATIVE_PSVARIANTS=0: the whole shader for every draw, as before; =alt: by turns, a profile window each.
                static const char *psvEnv = std::getenv("TF_NATIVE_PSVARIANTS");
                const bool psvOn = speedupOn(psvEnv);
                uint32_t psVar = psvOn ? psv::psVariantOf(c) : 0u;
                if ((zwMode & 512u) && psVar)
                    psVar &= ~(1u << 5); // (TF_NATIVE_ZWALT 512, a measuring aid: the alpha test left out - what it costs)
                const uint32_t psAid = (zwMode & 128u) ? 1u : (zwMode & 256u) ? 2u : 0u;
                // (The pipeline: the draw's state - or, a draw with none of its own, the default one: blend off, depth test
                // GEQUAL and written, all channels.)
                auto pipeOf = [](const gfx::BlendState &b, bool depthTest, gfx::Compare f, bool depthWrite, uint32_t mask, bool picture, bool clip,
                                 uint32_t variant = 0u, uint32_t aid = 0u) {
                    gfx::PipelineDesc p;
                    p.blend = b;
                    p.depthTest = depthTest;
                    p.depthFunc = f;
                    p.depthWrite = depthWrite;
                    p.writeMask = mask;
                    p.picture = picture;
                    p.clip = clip;
                    p.variant = variant;
                    p.alphaTest = aid;
                    return p;
                };
                const gfx::PipelineDesc defaultPipe = pipeOf(gfx::BlendState{}, true, gfx::Compare::GreaterEqual, true, gfx::kWriteAll, false, false);
                gfx::PipelineDesc pipe = pass.test || pass.alpha || writeMask != gfx::kWriteAll || tg.main || clipGs || zwriteNow != zwrite || psVar || psAid
                                             ? pipeOf(blend, zte, func, zwriteNow, writeMask, tg.main, clipGs && !psAid, psVar, psAid)
                                             : defaultPipe;
                {
                    // (TODO 94, a measuring aid: TF_NATIVE_ONEPSO=alt - every draw on the default pipeline in every other
                    // profile window, its own blend/depth/clipping state ignored: what the pipeline changes cost.)
                    static const char *onePso = std::getenv("TF_NATIVE_ONEPSO");
                    if (onePso && (g_profWindow & 1u) && !tg.main)
                    {
                        // (=b: the blend alone made the same (src alpha), =z: the depth state alone (test GEQUAL, write),
                        // =g: the clipping shader alone left out; else one pipeline for all.)
                        gfx::BlendState bk = blend;
                        bk.enabled = true, bk.src = gfx::Blend::SrcAlpha, bk.dst = gfx::Blend::InvSrcAlpha, bk.op = gfx::BlendOp::Add;
                        const gfx::Compare ge = gfx::Compare::GreaterEqual;
                        if (*onePso == 'O')
                            pipe = pipeOf(blend, zte, func, false, writeMask, false, false);
                        else if (*onePso == 'W')
                            pipe = pipeOf(blend, zte, func, true, writeMask, false, false);
                        else if (*onePso == 'F')
                            pipe = pipeOf(blend, true, ge, zwrite, writeMask, false, false);
                        else if (*onePso == 'B')
                            pipe = pipeOf(bk, zte, func, zwrite, writeMask, false, false);
                        else if (*onePso == 'Z')
                            pipe = pipeOf(blend, true, ge, true, writeMask, false, false);
                        else if (*onePso == 'm')
                            pipe = pipeOf(blend, zte, func, zwrite, gfx::kWriteAll, false, clipGs);
                        else if (*onePso == 'y')
                            pipe = pipeOf(bk, true, ge, true, writeMask, false, false);
                        else if (*onePso == 'n')
                            pipe = pipeOf(gfx::BlendState{}, zte, func, zwrite, writeMask, false, clipGs);
                        else if (*onePso == 'x')
                            pipe = pipeOf(bk, true, ge, true, writeMask, false, clipGs);
                        else if (*onePso == 'b')
                            pipe = pipeOf(bk, zte, func, zwrite, writeMask, false, clipGs);
                        else if (*onePso == 'z')
                            pipe = pipeOf(blend, true, ge, true, writeMask, false, clipGs);
                        else if (*onePso == 'g')
                            pipe = pipeOf(blend, zte, func, zwrite, writeMask, false, false);
                        else
                            pipe = defaultPipe;
                    }
                }
                if (!dev.setPipeline(pipe))
                {
                    static const char *onePsoOn = std::getenv("TF_NATIVE_ONEPSO");
                    if (!(onePsoOn && (g_profWindow & 1u) && !tg.main) || !dev.setPipeline(defaultPipe))
                    {
                        ++left; // (no pipeline: the draw is left out, not drawn with the last one's state and format)
                        continue;
                    }
                }
                // (TODO 94, a measuring aid: TF_NATIVE_PSOLOG=1 - on a saved frame, each draw's pipeline and what made it.)
                if (!shotPath.empty() && std::getenv("TF_NATIVE_PSOLOG"))
                    std::fprintf(stderr, "[native render] pso %u: variant %x blend %d %d %d %d z %d %d %d mask %x main %d clip %d mesh %llu passes %zu tgt %p imm %d\n",
                                 static_cast<unsigned>(&draw - input.draws.data()), pipe.variant, blend.enabled ? 1 : 0, static_cast<int>(blend.src),
                                 static_cast<int>(blend.dst), static_cast<int>(blend.op), zte ? 1 : 0, static_cast<int>(func), zwrite ? 1 : 0, writeMask,
                                 tg.main ? 1 : 0, clipGs ? 1 : 0, draw.mesh ? static_cast<unsigned long long>(draw.mesh->id) : 0ull, g.passes.size(),
                                 static_cast<void *>(&tg), draw.mesh && draw.mesh->immediate ? 1 : 0);
                {
                    static gfx::PipelineDesc lastPipe;
                    static bool lastGs = false;
                    const bool samePipe = lastPipe.blend.src == pipe.blend.src && lastPipe.blend.dst == pipe.blend.dst && lastPipe.blend.op == pipe.blend.op &&
                                          lastPipe.blend.enabled == pipe.blend.enabled && lastPipe.depthTest == pipe.depthTest &&
                                          lastPipe.depthFunc == pipe.depthFunc && lastPipe.depthWrite == pipe.depthWrite && lastPipe.writeMask == pipe.writeMask &&
                                          lastPipe.picture == pipe.picture && lastPipe.clip == pipe.clip && lastPipe.variant == pipe.variant &&
                                          lastPipe.alphaTest == pipe.alphaTest;
                    g_cntPso += samePipe ? 0u : 1u;
                    g_cntGsSwitch += clipGs != lastGs ? 1u : 0u;
                    lastPipe = pipe;
                    lastGs = clipGs;
                    ++g_cntDraws;
                }
                const float fix = static_cast<float>((pass.alpha >> 32) & 0xFFu) / 128.0f;
                {
                    const float factor[4] = {fix, fix, fix, fix};
                    dev.setBlendFactor(factor);
                }
                // (t0 the draw's texture; t1 the fog's palette, else TEST's DATE copy, else the top-byte read's palette; t2 the
                // fog's depth. Unused, t1 and t2 are left at what they were bound to: the shader reads them only for those.)
                dev.bindTextures(selfSlot ? selfSlot : source ? sourceSlot : t ? t->id : 0u,
                                 fog ? fogPalette->id : destSlot ? destSlot : topSource ? topPalette->id : gfx::kKeepTexture,
                                 fog ? tgDepth : gfx::kKeepTexture);
                // (The rings: the pass's constants, then its instance entries. Full - the frame's ring room spent - the
                // draw's passes from here are left out, as the check at a pass's start left them before.)
                gfx::RingSpan cSpan;
                if (!dev.ringAlloc(gfx::RingKind::Constants, kConstantStride, 256u, cSpan))
                {
                    ++g_ringFull;
                    break;
                }
                std::memcpy(cSpan.cpu, &c, sizeof(c));
                dev.bindDrawConstants(cSpan);
                // The pass's matrix and q0 into the instance stream - and, for an instanced draw (TODO 94: the forest's,
                // 6-10 passes of one mesh each, a third of a level's draws), every following instance pass's, drawn as one
                // draw: the same mesh, constants, texture and state, the instances in their order (the GPU keeps it).
                // TF_NATIVE_INSTANCING=0: a draw each, as before; =alt: by turns.
                UINT firstInst = 0u;
                UINT instCount = 1u;
                {
                    bool haveFirst = false;
                    auto entry = [&](const float *rows, const float *rowsB, const float *rowsM, const float *q0) {
                        // (one write of the whole entry: the ring is write-combined)
                        float e[52];
                        std::memcpy(e, rows, 64u);
                        std::memcpy(e + 16, q0, 16u);
                        std::memcpy(e + 20, rowsB, 64u);
                        std::memcpy(e + 36, rowsM, 64u);
                        gfx::RingSpan iSpan;
                        if (!dev.ringAlloc(gfx::RingKind::Instances, kInstanceStride, 16u, iSpan))
                            return false;
                        std::memcpy(iSpan.cpu, e, sizeof(e));
                        if (!haveFirst)
                            firstInst = iSpan.first; // (the instance stream's entry: draw's first instance)
                        haveFirst = true;
                        return true;
                    };
                    if (!entry(c.rows, pass.matrixB, pass.matrixM, c.p[0]))
                    {
                        ++g_ringFull;
                        break;
                    }
                    static const char *instEnv = std::getenv("TF_NATIVE_INSTANCING");
                    const bool instOn = speedupOn(instEnv);
                    if (instOn && pass.mode[0] == 9.0f && !rgbOnlyFail && !clipGs)
                        for (size_t k = passIndex + 1; k < g.passes.size() && g.passes[k].mode[0] == 9.0f; ++k)
                        {
                            if (!entry(g.passes[k].matrix, g.passes[k].matrixB, g.passes[k].matrixM, g.passes[k].p[0]))
                                break;
                            ++instCount;
                            passesDone = k + 1;
                        }
                }
                // (TODO 99) A clipping draw's triangles wholly outside one of its planes (the clipping matrix's |x|, |y|, |z| <= w,
                // in this frame's pose and the frame before's - the pictures between lie between) are left out here, on the CPU,
                // as 0x0480 leaves them out: the rest drawn from a list of their own in the frame's ring. Left to the GPU - the clip
                // distances on the RX 5700 XT's path, where the clipping shader is off - such triangles of a mesh reaching behind the
                // camera came through at some views: 36 of a near ground set's 84, all outside, drawn over 9,555 pixels for one
                // picture. A TEST, OFF (TF_NATIVE_CLIPCULL=1: on): it did not stop the flashes on either worker (2026-11-01).
                UINT drawCount = m->count;
                int cullKept = -1; // (TODO 99 check: -1 the CPU cull not tried, else the triangles it kept)
                bool keptView = false;
                gfx::RingSpan keptSpan;
                {
                    static const bool clipCullOff = !(std::getenv("TF_NATIVE_CLIPCULL") && *std::getenv("TF_NATIVE_CLIPCULL") == '1');
                    // (TF_NATIVE_BEHINDCULL=1, a test with it: a draw the core does not clip - its triangles with a corner at or
                    // behind the eye (w <= 0 through its own matrix, either pose) left out, as a GS-bound routine without 0x0480
                    // cannot draw them)
                    static const bool behindCullOn = std::getenv("TF_NATIVE_BEHINDCULL") && *std::getenv("TF_NATIVE_BEHINDCULL") == '1';
                    const bool clipping = c.clipOn[0] > 0.5f;
                    if (((!clipCullOff && clipping) || (behindCullOn && !clipping && !draw.mesh->immediate)) && // (the pictures' camera turn: identity in every picture today)
                        pass.mode[2] < 0.5f && pass.mode[3] < 0.5f && draw.mesh && draw.mesh->indices.size() == m->count && g.dynamic &&
                        instCount == 1u)
                    {
                        const auto &vx = draw.mesh->vertices;
                        const auto &ix = draw.mesh->indices;
                        static thread_local std::vector<uint8_t> outside;
                        static thread_local std::vector<uint32_t> kept;
                        outside.resize(vx.size());
                        for (size_t v = 0; v < vx.size() && !clipping; ++v)
                        {
                            uint8_t behind = 0u;
                            for (const float *rows : {static_cast<const float *>(c.rows), static_cast<const float *>(pass.matrixB), static_cast<const float *>(pass.matrixM)})
                                behind |= rows[3] * vx[v].x + rows[7] * vx[v].y + rows[11] * vx[v].z + rows[15] <= 0.0f ? 0x40u : 0u;
                            outside[v] = behind;
                        }
                        for (size_t v = 0; v < vx.size() && clipping; ++v)
                        {
                            uint8_t both = 0x3Fu;
                            for (const float *rows : {c.clipRows, c.clipRowsB, c.clipRowsM})
                            {
                                float k[4];
                                for (int r = 0; r < 4; ++r)
                                    k[r] = rows[r] * vx[v].x + rows[4 + r] * vx[v].y + rows[8 + r] * vx[v].z + rows[12 + r];
                                const float d[6] = {k[3] - k[0], k[3] + k[0], k[3] - k[1], k[3] + k[1], k[3] - k[2], k[3] + k[2]};
                                uint8_t o = 0u;
                                for (int e = 0; e < 6; ++e)
                                    o |= static_cast<uint8_t>((d[e] < 0.0f ? 1u : 0u) << e);
                                both &= o;
                            }
                            outside[v] = both;
                        }
                        kept.clear();
                        bool bad = false;
                        for (size_t q = 0; q + 2 < ix.size(); q += 3)
                        {
                            if (ix[q] >= vx.size() || ix[q + 1] >= vx.size() || ix[q + 2] >= vx.size())
                            {
                                bad = true;
                                break;
                            }
                            if ((outside[ix[q]] & outside[ix[q + 1]] & outside[ix[q + 2]] & 0x3Fu) == 0u &&
                                ((outside[ix[q]] | outside[ix[q + 1]] | outside[ix[q + 2]]) & 0x40u) == 0u)
                                kept.insert(kept.end(), {ix[q], ix[q + 1], ix[q + 2]});
                        }
                        cullKept = bad ? -2 : static_cast<int>(kept.size() / 3u);
                        if (!bad && kept.size() < ix.size())
                        {
                            if (kept.empty())
                            {
                                ++g_clipCulled[1];
                                continue; // (the whole pass outside: nothing to draw)
                            }
                            const UINT64 bytes = kept.size() * sizeof(uint32_t);
                            if (dev.ringAlloc(gfx::RingKind::Vertices, static_cast<uint32_t>(bytes), 16u, keptSpan))
                            {
                                std::memcpy(keptSpan.cpu, kept.data(), static_cast<size_t>(bytes));
                                keptView = true;
                                drawCount = static_cast<UINT>(kept.size());
                                g_clipCulled[0] += (ix.size() - kept.size()) / 3u;
                            }
                        }
                    }
                }
                // The geometry: the mesh (a resident one, or the frame's own in the ring; the grass's at its draw), the
                // clipped draw's kept triangles in place of its indices, the frame before's vertices.
                if (m->meshId)
                    dev.setMesh(m->meshId);
                else if (!m->indirect)
                {
                    dev.setVertices(m->ringVertices, sizeof(Vertex));
                    dev.setIndices(m->ringIndices);
                }
                if (keptView)
                    dev.setIndices(keptSpan);
                dev.setPreviousVertices(vertexB);
                // (TODO 62, an experiment, OFF: a clipping draw's triangles wholly inside its planes drawn
                // without the clipping shader, in runs that keep the mesh's order. Measured by turns in one
                // run on the RX 5700 XT it made the frame 4.5 ms SLOWER (14.9 vs 10.4 ms: ~77 switches a
                // frame between pipelines with and without a geometry shader), so the shader's own early
                // way out (clipOn.z) is what is kept. TF_NATIVE_CLIPSPLIT=1: on; =alt: by turns.)
                static const char *clipSplit = std::getenv("TF_NATIVE_CLIPSPLIT");
                const bool splitOn = clipSplit && (*clipSplit == '1' || (*clipSplit == 'a' && !(g_profWindow & 1u)));
                static thread_local std::vector<ClipRun> runs;
                gfx::RingSpan insideSpan; // (the split's constants for the runs wholly inside)
                bool split = clipGs && splitOn && !keptView && !rgbOnlyFail && pass.mode[2] < 0.5f && pass.mode[3] < 0.5f && draw.mesh &&
                             draw.mesh->indices.size() == m->count && dev.ringAlloc(gfx::RingKind::Constants, kConstantStride, 256u, insideSpan);
                const gfx::PipelineDesc pipeInside = pipeOf(blend, zte, func, zwrite, writeMask, tg.main, false, psVar);
                if (split) // (the inside runs' pipeline made now - and the draw's own set again)
                    split = dev.setPipeline(pipeInside) && dev.setPipeline(pipe) && clipRunsOf(*draw.mesh, pass.clip, runs);
                // (TODO 94, a measuring aid: TF_NATIVE_SKIPKIND=<kind>|all[a] - the draws of that kind (the pass
                // profile's numbers: 0 opaque, 1 blended, 2 grass, 3 2D, 4 small targets, 5 fog, 8 upscaled, 9
                // clipping) not drawn, their state still set; with 'a' in every other profile window: their
                // marginal cost, which the per-draw timestamps do not give where the GPU is not pixel-bound.)
                bool skipThis = false;
                {
                    static const char *skipEnv = std::getenv("TF_NATIVE_SKIPKIND");
                    if (skipEnv && *skipEnv)
                    {
                        const bool grassyK = draw.shader.blockOf(Routine::Grass) || draw.shader.blockOf(Routine::Drift);
                        const bool immediateK = draw.mesh && draw.mesh->immediate;
                        const int kindNow = fog ? 5 : (!tg.main && tg.w < 1024u) ? 4 : grassyK ? 2 : immediateK ? 3 : clipGs ? 9 : (t && pass.texture && pass.texture->upscaled) ? 8 : blend.enabled ? 1 : 0;
                        const bool all = skipEnv[0] == 'a' && skipEnv[1] == 'l';
                        const bool alt = std::strchr(skipEnv + (all ? 3 : 1), 'a') != nullptr;
                        skipThis = (all || std::atoi(skipEnv) == kindNow) && (!alt || (g_profWindow & 1u));
                    }
                }
                {
                    // (TODO 99, a check: TF_NATIVE_SIG=1 - on a saved frame, each pass drawn as a line in runs\sig_<tick>.txt: the
                    // mesh, a sum of its vertices, the texture and its view's slot, the instance entries, the matrix's last row;
                    // and a resident mesh's GPU copy compared with the mesh - a line "STALE" where they differ)
                    static const bool sigOn = std::getenv("TF_NATIVE_SIG") && *std::getenv("TF_NATIVE_SIG") == '1';
                    static FILE *sigF = nullptr;
                    static uint64_t sigTick = ~0ull;
                    if (sigOn && !shotPath.empty())
                    {
                        if (sigTick != input.tick)
                        {
                            if (sigF)
                                std::fclose(sigF);
                            sigTick = input.tick;
                            sigF = std::fopen(("runs\\sig_" + std::to_string(input.tick) + ".txt").c_str(), "w");
                        }
                        if (sigF && draw.mesh)
                        {
                            double vs = 0.0;
                            for (const Vertex &v : draw.mesh->vertices)
                                vs += v.x * 1.0 + v.y * 3.0 + v.z * 7.0 + v.u * 11.0 + v.v * 13.0 + (v.colour & 0xFFFFu);
                            const char *stale = "";
                            if (m != &g.dynamicMesh && g.devMeshes[m->meshId].buffer && !draw.mesh->immediate)
                            {
                                void *mp = nullptr;
                                const D3D12_RANGE rr{0, draw.mesh->vertices.size() * sizeof(Vertex)};
                                if (SUCCEEDED(g.devMeshes[m->meshId].buffer->Map(0, &rr, &mp)))
                                {
                                    if (std::memcmp(mp, draw.mesh->vertices.data(), rr.End) != 0 || m->count != draw.mesh->indices.size())
                                        stale = " STALE";
                                    const D3D12_RANGE none{0, 0};
                                    g.devMeshes[m->meshId].buffer->Unmap(0, &none);
                                }
                            }
                            // (where its vertices land: GS x, y and the depth, at the pass's matrix - the first instance's)
                            float zlo = 1e30f, zhi = -1e30f, xlo = 1e30f, xhi = -1e30f, ylo = 1e30f, yhi = -1e30f, wlo = 1e30f;
                            uint32_t inClip = 0u, behind = 0u; // (vertices inside the clipping matrix's six planes; behind the eye)
                            for (const Vertex &v : draw.mesh->vertices)
                            {
                                {
                                    float k[4];
                                    for (int r = 0; r < 4; ++r)
                                        k[r] = c.clipRows[r] * v.x + c.clipRows[4 + r] * v.y + c.clipRows[8 + r] * v.z + c.clipRows[12 + r];
                                    inClip += (std::fabs(k[0]) <= k[3] && std::fabs(k[1]) <= k[3] && std::fabs(k[2]) <= k[3]) ? 1u : 0u;
                                    const float w = c.rows[3] * v.x + c.rows[7] * v.y + c.rows[11] * v.z + c.rows[15];
                                    wlo = std::min(wlo, w);
                                    behind += w <= 0.0f ? 1u : 0u;
                                }
                                float q[4];
                                for (int k = 0; k < 4; ++k)
                                    q[k] = c.rows[k] * v.x + c.rows[4 + k] * v.y + c.rows[8 + k] * v.z + c.rows[12 + k];
                                if (q[3] <= 0.0f)
                                    continue;
                                const float z = q[2] / q[3] * 16.0f * c.misc[0], x = q[0] / q[3], y = q[1] / q[3];
                                zlo = std::min(zlo, z), zhi = std::max(zhi, z), xlo = std::min(xlo, x), xhi = std::max(xhi, x);
                                ylo = std::min(ylo, y), yhi = std::max(yhi, y);
                            }
                            // (the clipping shader's work, done here as gsClip does it, in floats: per triangle the six distances
                            // from the clipping matrix, the wholly-outside test, the polygon cut by each plane - its corners then
                            // through the draw's own matrix: those left with w <= 0, and the polygons' area on the GS screen)
                            uint32_t eTris = 0u, eEarly = 0u, eInside = 0u, ePoly = 0u, eBadW = 0u;
                            double eArea = 0.0;
                            if (c.clipOn[0] > 0.5f && draw.mesh->indices.size() >= 3u)
                            {
                                struct P { float p[3]; float d[6]; };
                                auto make = [&](const Vertex &v) {
                                    P o{};
                                    o.p[0] = v.x, o.p[1] = v.y, o.p[2] = v.z;
                                    float k[4];
                                    for (int r = 0; r < 4; ++r)
                                        k[r] = c.clipRows[r] * v.x + c.clipRows[4 + r] * v.y + c.clipRows[8 + r] * v.z + c.clipRows[12 + r];
                                    o.d[0] = k[3] - k[0], o.d[1] = k[3] + k[0], o.d[2] = k[3] - k[1], o.d[3] = k[3] + k[1], o.d[4] = k[3] - k[2], o.d[5] = k[3] + k[2];
                                    return o;
                                };
                                const auto &vx = draw.mesh->vertices;
                                const auto &ix = draw.mesh->indices;
                                for (size_t q = 0; q + 2 < ix.size(); q += 3)
                                {
                                    if (ix[q] >= vx.size() || ix[q + 1] >= vx.size() || ix[q + 2] >= vx.size())
                                        continue;
                                    ++eTris;
                                    P poly[12], next[12];
                                    poly[0] = make(vx[ix[q]]), poly[1] = make(vx[ix[q + 1]]), poly[2] = make(vx[ix[q + 2]]);
                                    bool out = false, in = true;
                                    for (int e = 0; e < 6; ++e)
                                    {
                                        out = out || (poly[0].d[e] < 0.0f && poly[1].d[e] < 0.0f && poly[2].d[e] < 0.0f);
                                        in = in && poly[0].d[e] >= 0.0f && poly[1].d[e] >= 0.0f && poly[2].d[e] >= 0.0f;
                                    }
                                    if (out)
                                    {
                                        ++eEarly;
                                        continue;
                                    }
                                    eInside += in ? 1u : 0u;
                                    uint32_t n = 3;
                                    for (int k = 0; k < 6 && n >= 3; ++k)
                                    {
                                        uint32_t m2 = 0;
                                        for (uint32_t e = 0; e < n; ++e)
                                        {
                                            const P &a = poly[e], &b = poly[e + 1 < n ? e + 1 : 0];
                                            const float da = a.d[k], db = b.d[k];
                                            if (da >= 0.0f && m2 < 9)
                                                next[m2++] = a;
                                            if ((da >= 0.0f) != (db >= 0.0f) && m2 < 9)
                                            {
                                                const float tt = da / (da - db);
                                                P o{};
                                                for (int j = 0; j < 3; ++j)
                                                    o.p[j] = a.p[j] + (b.p[j] - a.p[j]) * tt;
                                                for (int j = 0; j < 6; ++j)
                                                    o.d[j] = a.d[j] + (b.d[j] - a.d[j]) * tt;
                                                next[m2++] = o;
                                            }
                                        }
                                        n = m2;
                                        for (uint32_t cc = 0; cc < n; ++cc)
                                            poly[cc] = next[cc];
                                    }
                                    if (n < 3)
                                        continue;
                                    ++ePoly;
                                    double sx[12], sy[12];
                                    for (uint32_t cc = 0; cc < n; ++cc)
                                    {
                                        float sq[4];
                                        for (int k = 0; k < 4; ++k)
                                            sq[k] = c.rows[k] * poly[cc].p[0] + c.rows[4 + k] * poly[cc].p[1] + c.rows[8 + k] * poly[cc].p[2] + c.rows[12 + k];
                                        if (sq[3] <= 1e-6f)
                                            ++eBadW;
                                        sx[cc] = sq[3] > 1e-6f ? sq[0] / sq[3] : 0.0;
                                        sy[cc] = sq[3] > 1e-6f ? sq[1] / sq[3] : 0.0;
                                    }
                                    double ar = 0.0;
                                    for (uint32_t cc = 0; cc < n; ++cc)
                                        ar += sx[cc] * sy[(cc + 1) % n] - sx[(cc + 1) % n] * sy[cc];
                                    eArea += std::fabs(ar) * 0.5;
                                }
                            }
                            std::fprintf(sigF, "%zu.%zu set %08x id %llu v %zu i %u sum %.6g tex %llu slot %d si %d inst %u+%u clip %d gs %d t %g %g %g %g%s z %.5g %.5g x %.0f %.0f y %.0f %.0f test %llx zbuf %llx alpha %llx in %u behind %u wmin %.4g emu %u %u %u %u %u %.0f cull %d bdiff %g mdiff %g cm %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g\n",
                                         static_cast<size_t>(&draw - input.draws.data()), passIndex, draw.mesh->source,
                                         static_cast<unsigned long long>(draw.mesh->id), draw.mesh->vertices.size(), m->count, vs,
                                         static_cast<unsigned long long>(pass.texture ? pass.texture->id : 0u), t ? static_cast<int>(t->slot) : -1,
                                         standIn ? 1 : 0, firstInst - g.slot * kInstanceRing, instCount, clips ? 1 : 0, clipGs ? 1 : 0,
                                         c.rows[12], c.rows[13], c.rows[14], c.rows[15], stale, zlo, zhi, xlo, xhi, ylo, yhi,
                                         static_cast<unsigned long long>(pass.test), static_cast<unsigned long long>(pass.zbuf),
                                         static_cast<unsigned long long>(pass.alpha), inClip, behind, wlo, eTris, eEarly, eInside, ePoly, eBadW, eArea, cullKept,
                                         [&] { float m2 = 0.0f; for (int q = 0; q < 16; ++q) m2 = std::max(m2, std::fabs(c.clipRowsB[q] - c.clipRows[q])); return m2; }(),
                                         [&] { float m2 = 0.0f; for (int q = 0; q < 16; ++q) m2 = std::max(m2, std::fabs(pass.matrixB[q] - c.rows[q])); return m2; }(), c.clipRows[0], c.clipRows[1], c.clipRows[2], c.clipRows[3], c.clipRows[4], c.clipRows[5], c.clipRows[6], c.clipRows[7], c.clipRows[8], c.clipRows[9], c.clipRows[10], c.clipRows[11], c.clipRows[12], c.clipRows[13], c.clipRows[14], c.clipRows[15]);
                        }
                    }
                }
                g_cntIndices += m->count;
                g_cntGsIndices += clipGs ? m->count : 0u;
                if (statsNow && !g.statOpen && g.passKinds[g.slot].size() < State::kPassMarks)
                {
                    g.list->BeginQuery(g.statQueries, D3D12_QUERY_TYPE_PIPELINE_STATISTICS,
                                       g.slot * State::kPassMarks + static_cast<UINT>(g.passKinds[g.slot].size()));
                    g.statOpen = true;
                }
                static const bool pstatOn = std::getenv("TF_NATIVE_PSTATS") && *std::getenv("TF_NATIVE_PSTATS") == '1';
                const bool pstatHere = pstatOn && !shotPath.empty() && !statsNow && g_pstatHeap && g_pstatNotes.size() < kPStats;
                if (pstatHere)
                {
                    g.list->BeginQuery(g_pstatHeap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, static_cast<UINT>(g_pstatNotes.size()));
                    g_pstatNotes.push_back(PStatNote{static_cast<uint32_t>(&draw - input.draws.data()), static_cast<uint32_t>(passIndex),
                                                     draw.mesh ? draw.mesh->source : 0u});
                }
                if (!split)
                {
                    if (!skipThis)
                    {
                        predicate(skipDraw);
                        if (m->indirect)
                            dev.drawIndirect(draw.mesh->id, firstInst); // (grassgpu: its count the GPU's)
                        else
                            dev.drawIndexed(drawCount, 0u, instCount, firstInst);
                        predicate(false);
                    }
                }
                else
                {
                    Constants ci = c;
                    ci.clipOn[2] = 2.0f;
                    std::memcpy(insideSpan.cpu, &ci, sizeof(ci));
                    int bound = 1; // (the pipeline and constants set: the clipping shader's)
                    for (const ClipRun &r : runs)
                    {
                        if (r.kind == 2)
                            continue;
                        if (r.kind != bound)
                        {
                            dev.setPipeline(r.kind == 0 ? pipeInside : pipe);
                            dev.bindDrawConstants(r.kind == 0 ? insideSpan : cSpan); // (the crossing runs: c's, written above)
                            bound = r.kind;
                        }
                        predicate(skipDraw);
                        dev.drawIndexed(r.count, r.first, 1u, firstInst);
                        predicate(false);
                        ++g_clipRuns[r.kind];
                    }
                }
                if (rgbOnlyFail && (writeMask & ~static_cast<uint32_t>(gfx::kWriteA)) != 0u)
                {
                    Constants c2 = c;
                    c2.test[3] = 4.0f; // (the failing pixels only)
                    gfx::RingSpan c2Span;
                    if (dev.setPipeline(pipeOf(blend, zte, func, false, writeMask & ~static_cast<uint32_t>(gfx::kWriteA), tg.main, clipGs, psVar)) &&
                        dev.ringAlloc(gfx::RingKind::Constants, kConstantStride, 256u, c2Span))
                    {
                        std::memcpy(c2Span.cpu, &c2, sizeof(c2));
                        dev.bindDrawConstants(c2Span);
                        if (!skipThis)
                        {
                            predicate(skipDraw);
                            if (m->indirect)
                                dev.drawIndirect(draw.mesh->id, firstInst);
                            else
                                dev.drawIndexed(drawCount, 0u, instCount, firstInst);
                            predicate(false);
                        }
                    }
                }
                if (pstatHere)
                    g.list->EndQuery(g_pstatHeap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, static_cast<UINT>(g_pstatNotes.size() - 1u));
                ++drawn;
                drawnTargets.insert(found->first);
                if (g.passQueries)
                {
                    const bool grassy = draw.shader.blockOf(Routine::Grass) || draw.shader.blockOf(Routine::Drift);
                    const bool immediate = draw.mesh && draw.mesh->immediate;
                    State::PassNote nt;
                    nt.draw = static_cast<uint32_t>(&draw - input.draws.data());
                    nt.indices = m->count;
                    nt.texW = pass.texture ? pass.texture->width : 0u;
                    nt.texH = pass.texture ? pass.texture->height : 0u;
                    nt.mode = static_cast<uint32_t>(pass.mode[0]);
                    nt.alpha = pass.alpha;
                    nt.tex0 = pass.tex0;
                    if (drawCostWindow(g_profWindow))
                    {
                        nt.bits = (zte ? 1u : 0u) | static_cast<uint32_t>(func) << 1 | (zwrite ? 1u : 0u) << 4 | (blend.enabled ? 1u : 0u) << 5 |
                                  (writeMask & 15u) << 6 | (tg.main ? 1u : 0u) << 10 | (immediate ? 1u : 0u) << 11 | (grassy ? 1u : 0u) << 12 |
                                  (draw.mesh && draw.mesh->sprites ? 1u : 0u) << 13 | (fog ? 1u : 0u) << 14 | (source ? 1u : 0u) << 15 |
                                  (psVar ? 1u : 0u) << 16 | (canDiscard ? 1u : 0u) << 17 | (destSlot ? 1u : 0u) << 18 | (selfSlot ? 1u : 0u) << 19;
                        nt.barriers = static_cast<uint32_t>(g_cntBarriers - g_markBarriers);
                        nt.copies = static_cast<uint32_t>(g_cntCopies - g_markCopies);
                        nt.verts = draw.mesh ? static_cast<uint32_t>(draw.mesh->vertices.size()) : 0u;
                        nt.target = &tg;
                        nt.mesh = draw.mesh ? draw.mesh->id : 0ull;
                        // (The triangles' screen area in GS pixels: projected by the pass's matrix, those with a vertex behind
                        // the eye left out; sprites left out; the water's moved vertices as their unmoved positions.)
                        if (draw.mesh && !draw.mesh->sprites && draw.mesh->indices.size() >= 3u)
                        {
                            const auto &vs = draw.mesh->vertices;
                            const auto &ix = draw.mesh->indices;
                            const float *r = pass.matrix;
                            double area = 0.0;
                            for (size_t k = 0; k + 2 < ix.size(); k += 3)
                            {
                                float sx[3] = {}, sy[3] = {};
                                bool ok = true;
                                for (int j = 0; j < 3 && ok; ++j)
                                {
                                    if (ix[k + j] >= vs.size())
                                    {
                                        ok = false;
                                        break;
                                    }
                                    const Vertex &v = vs[ix[k + j]];
                                    if (draw.mesh->immediate)
                                    {
                                        sx[j] = v.x;
                                        sy[j] = v.y;
                                        continue;
                                    }
                                    const float w = r[3] * v.x + r[7] * v.y + r[11] * v.z + r[15];
                                    if (w <= 1e-6f)
                                        ok = false;
                                    else
                                    {
                                        sx[j] = (r[0] * v.x + r[4] * v.y + r[8] * v.z + r[12]) / w;
                                        sy[j] = (r[1] * v.x + r[5] * v.y + r[9] * v.z + r[13]) / w;
                                    }
                                }
                                if (ok)
                                    area += 0.5 * std::fabs(static_cast<double>((sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0])));
                            }
                            nt.area = static_cast<float>(std::min(area, 1e12));
                        }
                    }
                    passMark(fog ? 5u : (!tg.main && tg.w < 1024u) ? 4u : grassy ? 2u : immediate ? 3u : clipGs ? 9u : (t && pass.texture && pass.texture->upscaled) ? 8u : blend.enabled ? 1u : 0u);
                    g_markBarriers = g_cntBarriers;
                    g_markCopies = g_cntCopies;
                    if (!g.passNotes[g.slot].empty())
                        g.passNotes[g.slot].back() = nt;
                }
            }
            if (splitStep && !shotPath.empty() && tg.w >= 512u && static_cast<size_t>(&draw - input.draws.data()) >= nextSplit)
            {
                nextSplit = static_cast<size_t>(&draw - input.draws.data()) + splitStep;
                AfterSave sv;
                sv.index = static_cast<size_t>(&draw - input.draws.data());
                sv.w = tg.w;
                sv.h = tg.h;
                sv.split = true;
                D3D12_RESOURCE_DESC td = dt(tg).colour->GetDesc();
                UINT64 total = 0;
                g.device->GetCopyableFootprints(&td, 0, 1, 0, &sv.fp, nullptr, nullptr, &total);
                const D3D12_HEAP_PROPERTIES back = heap(D3D12_HEAP_TYPE_READBACK);
                const D3D12_RESOURCE_DESC rb = bufferDesc(total);
                if (SUCCEEDED(g.device->CreateCommittedResource(&back, D3D12_HEAP_FLAG_NONE, &rb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                IID_PPV_ARGS(&sv.readback))))
                {
                    const D3D12_RESOURCE_STATES was = dt(tg).state;
                    stateOf(dt(tg), D3D12_RESOURCE_STATE_COPY_SOURCE);
                    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
                    dst.pResource = sv.readback;
                    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    dst.PlacedFootprint = sv.fp;
                    src.pResource = dt(tg).colour;
                    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                    stateOf(dt(tg), was);
                    afterSaves.push_back(sv);
                }
            }
            if (!afterList.empty() && !shotPath.empty())
            {
                // (Debug: TF_NATIVE_SAVE_AFTER=<draw>[,<draw>]: the target a draw drew into, as it is after it.)
                const size_t index = static_cast<size_t>(&draw - input.draws.data());
                if (std::find(afterList.begin(), afterList.end(), index) != afterList.end() && !tg.main)
                {
                    AfterSave s;
                    s.index = index;
                    s.w = tg.w;
                    s.h = tg.h;
                    D3D12_RESOURCE_DESC td = dt(tg).colour->GetDesc();
                    UINT64 total = 0;
                    g.device->GetCopyableFootprints(&td, 0, 1, 0, &s.fp, nullptr, nullptr, &total);
                    const D3D12_HEAP_PROPERTIES back = heap(D3D12_HEAP_TYPE_READBACK);
                    const D3D12_RESOURCE_DESC rb = bufferDesc(total);
                    if (SUCCEEDED(g.device->CreateCommittedResource(&back, D3D12_HEAP_FLAG_NONE, &rb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                    IID_PPV_ARGS(&s.readback))))
                    {
                        const D3D12_RESOURCE_STATES was = dt(tg).state;
                        stateOf(dt(tg), D3D12_RESOURCE_STATE_COPY_SOURCE);
                        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
                        dst.pResource = s.readback;
                        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                        dst.PlacedFootprint = s.fp;
                        src.pResource = dt(tg).colour;
                        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                        g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                        stateOf(dt(tg), was);
                        afterSaves.push_back(s);
                    }
                }
            }
        }
        predicate(false);
        // (Debug: a target saved beside the picture, TF_NATIVE_SAVE_TARGET=<key in hex>.)
        ID3D12Resource *savedReadback = nullptr;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT savedFp{};
        State::Target *saved = nullptr;
        if (const char *key = std::getenv("TF_NATIVE_SAVE_TARGET"); key && !shotPath.empty())
        {
            // (Keys separated by commas: the first this frame drew into - the depth buffer's pages,
            // and the shadow texture in them, alternate between frames.)
            auto it = g.targets.end();
            for (const char *k = key; *k && it == g.targets.end();)
            {
                char *end = nullptr;
                const uint32_t want = static_cast<uint32_t>(std::strtoul(k, &end, 16));
                if (end == k)
                    break;
                if (drawnTargets.count(want) || !std::strchr(key, ','))
                    it = g.targets.find(want);
                k = *end == ',' ? end + 1 : end;
            }
            if (it != g.targets.end() && it->second.id && !it->second.main)
            {
                saved = &it->second;
                D3D12_RESOURCE_DESC td = dt(*saved).colour->GetDesc();
                UINT64 total = 0;
                g.device->GetCopyableFootprints(&td, 0, 1, 0, &savedFp, nullptr, nullptr, &total);
                const D3D12_HEAP_PROPERTIES back = heap(D3D12_HEAP_TYPE_READBACK);
                const D3D12_RESOURCE_DESC rb = bufferDesc(total);
                if (SUCCEEDED(g.device->CreateCommittedResource(&back, D3D12_HEAP_FLAG_NONE, &rb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                IID_PPV_ARGS(&savedReadback))))
                {
                    dev.use(saved->id, gfx::UseFor::CopySource);
                    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
                    dst.pResource = savedReadback;
                    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    dst.PlacedFootprint = savedFp;
                    src.pResource = dt(*saved).colour;
                    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                }
            }
        }
        if (!shotPath.empty())
            std::fprintf(stderr, "[native render] picture: target %03x (%.0fx%.0f), %zu targets, %zu passes drawn, %zu from targets (%zu top-byte reads), %zu left out\n",
                     mainFbp, mainW, mainH, g.targets.size(), drawn, fromTargets, topReads, left);
        if (!shotPath.empty())
            // (TODO 99 check: the frame's rings - used of their room - what was left out for want of room so far, and the
            // views rewritten under a frame's use so far)
            std::fprintf(stderr, "[native render] rings: constants %llu of %llu, instances %u of %u, vertices %llu of %llu KB; slot %u; full %llu, views rewritten %llu; clipping triangles left out %llu, passes %llu\n",
                         static_cast<unsigned long long>((g.ringConstantsAt - g.slot * kConstantRing) / kConstantStride),
                         static_cast<unsigned long long>(kConstantRing / kConstantStride), g.ringInstanceAt - g.slot * kInstanceRing, kInstanceRing,
                         static_cast<unsigned long long>((g.dynamicUsed - g.slot * kDynamicRing) >> 10),
                         static_cast<unsigned long long>(kDynamicRing >> 10), g.slot, static_cast<unsigned long long>(g_ringFull),
                         static_cast<unsigned long long>(g_srvRewrites), static_cast<unsigned long long>(g_clipCulled[0]),
                         static_cast<unsigned long long>(g_clipCulled[1]));
        probe(false);
        if (aaWanted && !aaDone && picture3D)
        {
            // (No 2D over the 3D this frame: the anti-aliasing pass over the whole picture now.)
            auto aaT = prof::clock::now();
            if (runAa(picture))
                aaAt = input.draws.size();
            aaDone = true;
            g_aaCpu += prof::since(aaT);
            ++g_aaFrames;
        }
        if (!shotPath.empty())
        {
            if (aaDone && aaAt)
                std::fprintf(stderr, "[native render] anti-aliasing: applied before draw %zu of %zu%s\n", aaAt, input.draws.size(),
                             aaAt == input.draws.size() ? " (at the frame's end: no 2D over the 3D)" : "");
            else
                std::fprintf(stderr, "[native render] anti-aliasing: not applied (%s)\n",
                             !aaOn() ? "off" : !g.aaPipeline ? "no pipeline" : picture3D ? "the pass failed" : "no 3D in the picture");
        }
        // (Debug: TF_NATIVE_AASAVE=1: on a saved frame, the picture as it was before the pass too -
        // <shot>_noaa.bmp, without the 2D drawn after it.)
        ID3D12Resource *aaReadback = nullptr;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT aaFp{};
        if (aaDone && aaAt && !shotPath.empty() && std::getenv("TF_NATIVE_AASAVE") && aa::source)
        {
            State::DeviceTarget &aaSrc = dt(aa::source);
            D3D12_RESOURCE_DESC td = aaSrc.colour->GetDesc();
            UINT64 total = 0;
            g.device->GetCopyableFootprints(&td, 0, 1, 0, &aaFp, nullptr, nullptr, &total);
            const D3D12_HEAP_PROPERTIES back = heap(D3D12_HEAP_TYPE_READBACK);
            const D3D12_RESOURCE_DESC rb = bufferDesc(total);
            if (SUCCEEDED(g.device->CreateCommittedResource(&back, D3D12_HEAP_FLAG_NONE, &rb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                            IID_PPV_ARGS(&aaReadback))))
            {
                const D3D12_RESOURCE_STATES was = aaSrc.state;
                stateOf(aaSrc, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
                dst.pResource = aaReadback;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint = aaFp;
                src.pResource = aaSrc.colour;
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                stateOf(aaSrc, was);
            }
        }
        dev.use(picture.id, gfx::UseFor::Draw);
        passMark(6u);
        if (g.passQueries && !g.passKinds[g.slot].empty())
            g.list->ResolveQueryData(g.passQueries, D3D12_QUERY_TYPE_TIMESTAMP, g.slot * State::kPassMarks, static_cast<UINT>(g.passKinds[g.slot].size()),
                                     g.passTimes, static_cast<UINT64>(g.slot) * State::kPassMarks * 8ull);
        g.statsTaken[g.slot] = statsNow && !g.passKinds[g.slot].empty();
        if (statsNow && !g.passKinds[g.slot].empty())
            g.list->ResolveQueryData(g.statQueries, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, g.slot * State::kPassMarks,
                                     static_cast<UINT>(g.passKinds[g.slot].size()), g.statData,
                                     static_cast<UINT64>(g.slot) * State::kPassMarks * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS));
        if (g_pstatHeap && !g_pstatNotes.empty())
            g.list->ResolveQueryData(g_pstatHeap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, static_cast<UINT>(g_pstatNotes.size()), g_pstatBack, 0);
        if (const HRESULT closed = g.list->Close(); FAILED(closed))
        {
            static int told = 5;
            if (told-- > 0)
                std::fprintf(stderr, "[native render] the frame's list did not close (%08lx), %zu targets skipped in extra pictures\n",
                             static_cast<unsigned long>(closed), skippable.size());
        }
        g.journalOn = false;
        prof::record += prof::since(profT);
        // ---- N9 stage 3: the frame's pictures, each the frame's list run again ----------------------------------------
        // The list above is the whole frame, recorded once; each picture runs it with its own constants (where it lies
        // between the frame before's pose and this frame's, the camera's turn: PictureConstants, the shader's b1, written
        // by a small list before it) and a small list after it (its copy into the shared texture shown, its timestamps).
        // Before an extra picture the resources the list moved are set back to the states it began with. A frame of one
        // picture (Frame::pictures empty) is drawn the same way, once, at this frame's pose.
        std::vector<Frame::Picture> onePicture;
        if (input.pictures.empty())
        {
            onePicture.emplace_back();
            onePicture.back().dueNs = input.dueNs;
        }
        // (TODO 99, a check: TF_NATIVE_TWICE=1 - a saved frame of one picture has its list run twice, the same picture each
        // time, both saved (<shot>_r0.bmp, then the shot itself): a picture that differs between the two runs of one
        // recording is the GPU's doing, one wrong in both is the recording's.)
        static const bool twice = std::getenv("TF_NATIVE_TWICE") && *std::getenv("TF_NATIVE_TWICE") == '1';
        if (twice && !shotPath.empty() && shotPath.size() > 4u && input.pictures.size() <= 1u)
        {
            if (!input.pictures.empty())
                onePicture = input.pictures;
            onePicture.back().shot = shotPath.substr(0, shotPath.size() - 4u) + "_r0.bmp";
            onePicture.push_back(onePicture.back());
            onePicture.back().shot = shotPath;
        }
        const std::vector<Frame::Picture> &pictures = !onePicture.empty() ? onePicture : input.pictures;
        const UINT pictureCount = static_cast<UINT>(std::min<size_t>(pictures.size(), kMaxPictures));
        bool ownShots = false;
        for (UINT k = 0; k < pictureCount; ++k)
            ownShots = ownShots || !pictures[k].shot.empty();
        auto barrier = [](ID3D12GraphicsCommandList *l, ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
            if (from == to)
                return;
            ++g_cntBarriers;
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = r;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = from;
            b.Transition.StateAfter = to;
            l->ResourceBarrier(1, &b);
        };
        if (pictureCount > 1u)
        {
            g.restoreLists[g.slot]->Reset(g.picAllocators[g.slot], nullptr);
            for (ID3D12Resource *r : g.journalOrder)
            {
                const auto &states = g.journal[r];
                barrier(g.restoreLists[g.slot], r, states.second, states.first);
            }
            g.restoreLists[g.slot]->Close();
        }
        const bool timed = g.picQueries && g.picTimes;
        UINT executed = 0; // (the pictures drawn: their timestamps' places)
        // D3D12 forbids submitting a command list again while an earlier submission of it may still run on the GPU (the
        // driver may patch a list as it is submitted; the debug layer's error 553, COMMAND_LIST_SYNC). An extra picture runs
        // the frame's list (and the restore list) again, so it is submitted only once the picture before it has finished:
        // by replay:: (above), so the render thread does not wait for it. Found by Jenvery (home repo e24c20b): 100+
        // errors 553 in a 3-minute run. TF_NATIVE_SERIAL_REPLAY=0: back to back, as before; =wait: the render thread
        // waits for each itself (that wait left out of its times - cpuPictureMs, nr_thread.cpp's frame base via
        // replayWaitMs: it is the GPU's). A frame saving a picture or waiting for the GPU anyway does that too.
        static const int replayMode = [] {
            const char *v = std::getenv("TF_NATIVE_SERIAL_REPLAY");
            return !v ? 2 : *v == '0' ? 0 : std::strcmp(v, "wait") == 0 ? 1 : 2;
        }();
        double replayWaitMs = 0.0;
        UINT replayQueued = 0; // (extra pictures handed to replay::)
        const auto picturesStart = prof::clock::now();
        const bool waitNow = !shotPath.empty() || savedReadback != nullptr || !afterSaves.empty() || std::getenv("TF_NATIVE_SYNC") != nullptr;
        const bool replayThread = replayMode == 2 && !waitNow && !ownShots;
        {
            // (the frame before's extra pictures all submitted before this frame's: the queue's order. Its wait is the GPU
            // behind, as a wait for a frame slot is - counted with those, the pictures' cap sees it.)
            double turnWaited = 0.0;
            const double waited = replay::drain(&turnWaited);
            replayWaitMs += turnWaited; // (N9: the frame before's turn pictures waiting for their moments - not the GPU)
            g_slotWaitMs += waited;
            g_slotWaitAll.store(g_slotWaitAll.load(std::memory_order_relaxed) + waited, std::memory_order_relaxed);
        }
        for (UINT k = 0; k < pictureCount; ++k)
        {
            const Frame::Picture &pic = pictures[k];
            const bool last = k + 1u == pictureCount;
            uint8_t *const picEntry = g.pictureUpMapped + (static_cast<size_t>(g.slot) * kMaxPictures + k) * kPictureUp;
            {
                PictureConstants pc{};
                pc.picture[0] = pic.s;
                for (int i = 0; i < 4; ++i)
                    pc.turn[5 * i] = pc.turnClip[5 * i] = 1.0f;
                uint8_t *entry = picEntry;
                std::memcpy(entry, &pc, sizeof(pc));
                const uint64_t skip = executed > 0 ? 1u : 0u; // (the predicate: an extra picture leaves out what it can)
                std::memcpy(entry + kPredicateAt, &skip, 8u);
            }
            // (the shared texture this picture is copied into: not one shown or waiting for its moment)
            int sharedIndex = g.live && g_shareOn.load() == 1 && !input.dropped ? pickShared() : -1; // (TODO 161 a: a frame not shown)
            if (sharedIndex >= 0 && !sharedFor(sharedIndex))
                sharedIndex = -1;
            {
                // (TODO 98: how many shared textures the pictures need - logged as the most in use grows)
                static int most = 0;
                if (sharedIndex + 1 > most)
                {
                    most = sharedIndex + 1;
                    if (most % 8 == 0)
                        std::fprintf(stderr, "[native render] shared textures: %d in use at most\n", most);
                }
            }
            if (g.live && g_shareOn.load() == 1 && !input.dropped)
            {
                // (N9: pictures drawn but not shown - every shared texture held: shown, waiting for its moment, the newest)
                static uint64_t lost = 0, drawnPictures = 0;
                static auto since = std::chrono::steady_clock::now();
                ++drawnPictures;
                lost += sharedIndex < 0;
                if (std::chrono::steady_clock::now() - since > std::chrono::seconds(10))
                {
                    if (lost)
                        std::fprintf(stderr, "[native render] %llu of %llu pictures had no shared texture free (not shown)\n",
                                     static_cast<unsigned long long>(lost), static_cast<unsigned long long>(drawnPictures));
                    lost = drawnPictures = 0;
                    since = std::chrono::steady_clock::now();
                }
            }
            // An extra picture with nowhere to go is not drawn (the frame's own, the last, always is: the shots, the
            // CPU hand-over, the states the next frame's recording starts from).
            if (sharedIndex < 0 && !last && g.live && g_shareOn.load() == 1 && pic.shot.empty())
                continue;
            // Before it: its constants, its first timestamp.
            ID3D12GraphicsCommandList *pro = g.proLists[g.slot][k];
            pro->Reset(g.picAllocators[g.slot], nullptr);
            // (A buffer's state decays to COMMON at the end of every ExecuteCommandLists: each picture begins from there.)
            barrier(pro, g.pictureCB, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            pro->CopyBufferRegion(g.pictureCB, 256ull * g.slot, g.pictureUp, (static_cast<UINT64>(g.slot) * kMaxPictures + k) * kPictureUp,
                                  sizeof(PictureConstants));
            barrier(pro, g.pictureCB, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
            g.pictureCBState = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
            if (g.predicate)
            {
                barrier(pro, g.predicate, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                pro->CopyBufferRegion(g.predicate, 8ull * g.slot, g.pictureUp,
                                      (static_cast<UINT64>(g.slot) * kMaxPictures + k) * kPictureUp + kPredicateAt, 8u);
                barrier(pro, g.predicate, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PREDICATION);
                g.predicateState = D3D12_RESOURCE_STATE_PREDICATION;
            }
            pro->Close();
            // After it: its last timestamp, the live picture's copy into a shared texture on the GPU when the window reads
            // them (no readback, no upload), else read back for the CPU hand-over; a saved picture's readback.
            ID3D12GraphicsCommandList *tail = g.tailLists[g.slot][k];
            tail->Reset(g.picAllocators[g.slot], nullptr);
            if (timed)
            {
                // (the picture's start - the frame's list's own timestamp, the same query each run - and its end, copied out)
                const UINT base = g.slot * kPicStride;
                tail->EndQuery(g.picQueries, D3D12_QUERY_TYPE_TIMESTAMP, base + 2u * executed + 1u);
                tail->ResolveQueryData(g.picQueries, D3D12_QUERY_TYPE_TIMESTAMP, base + 2u * kMaxPictures, 1u, g.picTimes, (base + 2u * executed) * 8ull);
                tail->ResolveQueryData(g.picQueries, D3D12_QUERY_TYPE_TIMESTAMP, base + 2u * executed + 1u, 1u, g.picTimes,
                                       (base + 2u * executed + 1u) * 8ull);
            }
            uint32_t pictureStamp = 0;
            {
                // (TF_NATIVE_STAMP=1, a checking aid: each picture's number, 16 bits, as squares in its top-left and bottom-left
                // corners - a red marker, then each bit white or black - read from what the window presented by
                // PS2X_PRESENT_CAPTURE: the two rows differ in a picture mixed of two, the numbers go back in one out of order)
                static const bool stampOn = std::getenv("TF_NATIVE_STAMP") && *std::getenv("TF_NATIVE_STAMP") == '1';
                static uint32_t stampNumber = 0;
                pictureStamp = stampOn ? (stampNumber + 1u) & 0xffffu : 0u;
                if (stampOn)
                {
                    const D3D12_RESOURCE_DESC cd = g.colour->GetDesc();
                    const LONG side = 16, h = static_cast<LONG>(cd.Height);
                    const uint32_t number = ++stampNumber & 0xffffu;
                    std::vector<D3D12_RECT> red, on, off;
                    for (const LONG y : {side, h - 2 * side})
                    {
                        red.push_back(D3D12_RECT{side, y, 2 * side, y + side});
                        for (int b = 0; b < 16; ++b)
                            (((number >> b) & 1u) ? on : off).push_back(D3D12_RECT{side * (b + 2), y, side * (b + 3), y + side});
                    }
                    const float redC[4] = {1.0f, 0.0f, 0.0f, 1.0f}, onC[4] = {1.0f, 1.0f, 1.0f, 1.0f}, offC[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                    tail->ClearRenderTargetView(rtvAt(0), redC, static_cast<UINT>(red.size()), red.data());
                    if (!on.empty())
                        tail->ClearRenderTargetView(rtvAt(0), onC, static_cast<UINT>(on.size()), on.data());
                    if (!off.empty())
                        tail->ClearRenderTargetView(rtvAt(0), offC, static_cast<UINT>(off.size()), off.data());
                }
            }
            if (sharedIndex >= 0)
            {
                ID3D12Resource *dstTexture = g.shared[sharedIndex].resource;
                barrier(tail, dstTexture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                barrier(tail, g.colour, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
                dst.pResource = dstTexture;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                src.pResource = g.colour;
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                tail->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                barrier(tail, g.colour, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                barrier(tail, dstTexture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON); // (as the window's GL reads it)
                g.slotShared[g.slot] = sharedIndex;
                g.slotHas3D[g.slot] = input.has3D || input.shown2D; // (a menu shown natively: TODO 88)
                g.slotTick[g.slot] = input.tick;
                g.slotDueNs[g.slot] = pic.dueNs;
            }
            else if (last && g.live && g_shareOn.load() != 1 && !input.dropped && slotReadbackFor(g.slot))
            {
                barrier(tail, g.colour, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
                dst.pResource = g.slotReadback[g.slot];
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint = g.slotFootprint[g.slot];
                src.pResource = g.colour;
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                tail->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                barrier(tail, g.colour, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                g.slotPublish[g.slot] = true;
                g.slotHas3D[g.slot] = input.has3D || input.shown2D; // (a menu shown natively: TODO 88)
            }
            if (last && !input.dropped && stream::wanted(input.tick) && stream::readbackFor(g.slot))
            {
                barrier(tail, g.colour, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
                dst.pResource = stream::rb[g.slot];
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint = stream::fp[g.slot];
                src.pResource = g.colour;
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                tail->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                barrier(tail, g.colour, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                stream::pending[g.slot] = true;
                stream::tick[g.slot] = input.tick;
            }
            if (!pic.shot.empty() || (last && !shotPath.empty() && !ownShots))
            {
                barrier(tail, g.colour, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
                dst.pResource = g.readback;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint = g.footprint;
                src.pResource = g.colour;
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                src.SubresourceIndex = 0;
                tail->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                barrier(tail, g.colour, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            }
            {
                // (TODO 99 check, with TF_NATIVE_SIG=1: the picture constants as the GPU holds them for this picture - picture.x)
                static const bool sigPc = std::getenv("TF_NATIVE_SIG") && *std::getenv("TF_NATIVE_SIG") == '1';
                static ID3D12Resource *pcBack = nullptr;
                if (sigPc && !shotPath.empty() && last)
                {
                    if (!pcBack)
                    {
                        const D3D12_HEAP_PROPERTIES back = heap(D3D12_HEAP_TYPE_READBACK);
                        const D3D12_RESOURCE_DESC rb = bufferDesc(256u);
                        if (FAILED(g.device->CreateCommittedResource(&back, D3D12_HEAP_FLAG_NONE, &rb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                     IID_PPV_ARGS(&pcBack))))
                            pcBack = nullptr;
                    }
                    if (pcBack)
                    {
                        barrier(tail, g.pictureCB, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_COPY_SOURCE);
                        tail->CopyBufferRegion(pcBack, 0, g.pictureCB, 256ull * g.slot, 256u);
                        barrier(tail, g.pictureCB, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
                        g_pcBack = pcBack;
                    }
                }
            }
            tail->Close();
            ID3D12CommandList *lists[6] = {};
            UINT listCount = 0;
            if (executed > 0)
                lists[listCount++] = g.restoreLists[g.slot]; // (the frame's list ran: its resources set back)
            if (executed == 0 && g_cbVram)
            {
                // (The constants this frame wrote, into video memory before the frame's list reads them: TODO 94.)
                const UINT64 from = static_cast<UINT64>(g.slot) * kConstantRing;
                g.preAllocators[g.slot]->Reset();
                g.preList->Reset(g.preAllocators[g.slot], nullptr);
                barrier(g.preList, g.constantsV, g.constantsVReadable ? D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER : D3D12_RESOURCE_STATE_COMMON,
                        D3D12_RESOURCE_STATE_COPY_DEST);
                if (g.ringConstantsAt > from)
                    g.preList->CopyBufferRegion(g.constantsV, from, g.constants, from, g.ringConstantsAt - from);
                barrier(g.preList, g.constantsV, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
                g.constantsVReadable = true;
                g.preList->Close();
                lists[listCount++] = g.preList;
            }
            if (executed == 0)
                if (ID3D12CommandList *grass = grassgpu::takeList())
                    lists[listCount++] = grass; // (grassgpu: the frame's blades, grown once before its first picture)
            lists[listCount++] = pro;
            lists[listCount++] = g.list;
            lists[listCount++] = tail;
            // (N9: the camera's turn of this picture - the mouse and stick since the frame's read, view_turn.h - written into
            // its constants just before it is submitted: an extra picture's on the submitting thread, which may wait for the
            // moment it is read at; the frame's own here, without waiting. Its id for the pacing log: its fence value.)
            std::function<void()> turnFill;
            if (input.turnCamera && input.turnMain >= 0)
                turnFill = [picEntry, cam = input.turnCamera, main = input.turnMain, frameDue = input.dueNs, span = input.periodNs, due = pic.dueNs, s = pic.s, id = g.fenceValue + 1u,
                            wait = executed > 0 && input.turnWaits ? 25.0 : 0.0, set = input.turnLeadSet]() {
                    view_turn::PictureTurn t;
                    const auto t0 = std::chrono::steady_clock::now();
                    view_turn::pictureTurn(*cam, main, frameDue, span, due, s, wait, set, t);
                    if (wait > 0.0)
                        replay::g_turnWaitMs.store(replay::g_turnWaitMs.load(std::memory_order_relaxed) +
                                                       std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(),
                                                   std::memory_order_relaxed);
                    std::memcpy(picEntry + offsetof(PictureConstants, turn), t.screen, sizeof(t.screen));
                    std::memcpy(picEntry + offsetof(PictureConstants, turnClip), t.clip, sizeof(t.clip));
                    view_turn::notePictureYaw(id, t.valid, t.yawShown, t.sampleNs);
                };
            if (executed > 0 && replayThread)
            {
                // (submitted by replay:: once the picture before has finished; its fence value given now)
                replay::Job job;
                std::copy(lists, lists + listCount, job.lists);
                job.count = listCount;
                job.after = g.fenceValue;
                job.signal = g.fenceValue + 1u;
                job.before = std::move(turnFill);
                replay::push(job);
                ++replayQueued;
            }
            else
            {
                if (executed > 0 && replayMode != 0 && g.fence->GetCompletedValue() < g.fenceValue)
                {
                    const auto waitStart = prof::clock::now();
                    waitFence(g.fenceValue); // (the picture before: the frame's list and the restore list free to run again)
                    replayWaitMs += std::chrono::duration<double, std::milli>(prof::clock::now() - waitStart).count();
                }
                if (turnFill)
                    turnFill();
                g.queue->ExecuteCommandLists(listCount, lists);
                g.queue->Signal(g.fence, g.fenceValue + 1u);
            }
            g.slotFence[g.slot] = ++g.fenceValue;
            ++executed;
            if (timed)
                g.slotPictures[g.slot] = executed;
            if (sharedIndex >= 0 && d3d::pacingMode() != 0)
            {
                // (N9: handed over by the pacer, at its moment - not by finishSlot)
                const State::Shared &sh = g.shared[sharedIndex];
                d3d::SharedPicture shown{sh.handle, sh.size, sh.w, sh.h, sharedIndex, sh.generation, g.slotHas3D[g.slot]};
                shown.tick = input.tick;
                shown.dueNs = pic.dueNs;
                shown.stamp = pictureStamp;
                shown.turnId = g.fenceValue; // (N9: its camera turn's shown yaw, view_turn.h)
                g.slotPaced[g.slot] = true;
                pace::submit(g.fenceValue, shown, input.periodNs, input.vblanksOnRefresh, input.pictureNs, input.refreshNs);
            }
            if (!pic.shot.empty())
            {
                waitFence(g.fenceValue);
                saveBmp(pic.shot);
            }
        }
        {
            // (d3d553: how often a frame's list was run again, how many of those waited, how long - every 10 s when any did)
            static uint64_t extras = 0, waits = 0, queued = 0;
            static double waitedMs = 0.0;
            static auto since = std::chrono::steady_clock::now();
            g_replayWaitMs = replayWaitMs;
            extras += executed > 1u ? executed - 1u : 0u;
            waits += replayWaitMs > 0.0;
            queued += replayQueued;
            waitedMs += replayWaitMs;
            if (std::chrono::steady_clock::now() - since > std::chrono::seconds(10))
            {
                if (extras)
                    std::fprintf(stderr, "[native render] extra pictures (the frame's list run again): %llu in 10 s, %s; %llu handed to the submitting thread, "
                                 "%llu frames waited themselves (%.1f ms)\n",
                                 static_cast<unsigned long long>(extras),
                                 replayMode == 0 ? "back to back" : "each after the one before", static_cast<unsigned long long>(queued),
                                 static_cast<unsigned long long>(waits), waitedMs);
                extras = waits = queued = 0;
                waitedMs = 0.0;
                since = std::chrono::steady_clock::now();
            }
        }
        if (executed > 1u && !waitNow)
        {
            const double ms = (std::chrono::duration<double, std::milli>(prof::clock::now() - picturesStart).count() - replayWaitMs) / executed;
            const double was = g_cpuPictureMs.load(std::memory_order_relaxed);
            g_cpuPictureMs.store(was <= 0.0 ? ms : (ms > was ? 0.8 * was + 0.2 * ms : 0.97 * was + 0.03 * ms), std::memory_order_relaxed);
        }
        if (waitNow)
            waitFence(g.fenceValue);
        if (grassgpu::g_slots[g.slot].checking)
        {
            // (TF_GRASS_GPU_CHECK: this frame's blades read back once the GPU is done - a checking aid, it waits)
            replay::drain();
            waitFence(g.fenceValue);
            grassgpu::check(g.slot);
        }
        prof::wait += prof::since(profT);
        if (const HRESULT removed = g.device->GetDeviceRemovedReason(); FAILED(removed))
        {
            g_deviceLost.store(true, std::memory_order_relaxed);
            static bool told = false;
            if (!told)
            {
                told = true;
                std::fprintf(stderr, "[native render] the Direct3D 12 device was removed (%08lx) at a frame of %zu draws - the native renderer stops, the emulated one draws\n",
                             static_cast<unsigned long>(removed), input.draws.size());
                ID3D12DeviceRemovedExtendedData *dred = nullptr;
                if (SUCCEEDED(g.device->QueryInterface(IID_PPV_ARGS(&dred))))
                {
                    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs{};
                    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&crumbs)))
                        for (const D3D12_AUTO_BREADCRUMB_NODE *n = crumbs.pHeadAutoBreadcrumbNode; n; n = n->pNext)
                        {
                            const UINT done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0u;
                            std::fprintf(stderr, "[native render]   breadcrumbs: list %p, %u ops, %u done\n", static_cast<const void *>(n->pCommandList),
                                         n->BreadcrumbCount, done);
                            for (UINT k = (done > 6u ? done - 6u : 0u); k < n->BreadcrumbCount && k < done + 6u; ++k)
                                std::fprintf(stderr, "[native render]     %s op %u: %d\n", k == done ? ">>" : "  ", k,
                                             static_cast<int>(n->pCommandHistory[k]));
                        }
                    D3D12_DRED_PAGE_FAULT_OUTPUT fault{};
                    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&fault)))
                    {
                        std::fprintf(stderr, "[native render]   page fault at %llx\n", static_cast<unsigned long long>(fault.PageFaultVA));
                        for (const D3D12_DRED_ALLOCATION_NODE *a = fault.pHeadExistingAllocationNode; a; a = a->pNext)
                            std::fprintf(stderr, "[native render]     near a live allocation of type %d\n", static_cast<int>(a->AllocationType));
                        for (const D3D12_DRED_ALLOCATION_NODE *a = fault.pHeadRecentFreedAllocationNode; a; a = a->pNext)
                            std::fprintf(stderr, "[native render]     near a freed allocation of type %d\n", static_cast<int>(a->AllocationType));
                    }
                    release(dred);
                }
            }
        }
        static const double slowMs = std::getenv("TF_NATIVE_SLOWMS") ? std::atof(std::getenv("TF_NATIVE_SLOWMS")) : 50.0;
        if (const double ms = std::chrono::duration<double, std::milli>(prof::clock::now() - renderStart).count(); ms > slowMs)
        {
            static int told = 0;
            if (told < (slowMs < 50.0 ? 400 : 40))
            {
                ++told;
                std::fprintf(stderr, "[native render] slow frame's parts: setup %.1f meshes %.1f textures %.1f record %.1f gpu wait %.1f (releases %.1f) ms; %llu mesh buffers and %llu textures made\n",
                             prof::setup - partsBefore[0], prof::meshes - partsBefore[1], prof::textures - partsBefore[2], prof::record - partsBefore[3],
                             prof::wait - partsBefore[4], g_releaseMs, static_cast<unsigned long long>(g_meshesMade - meshesBefore),
                             static_cast<unsigned long long>(g_made - madeBefore));
                std::fprintf(stderr, "[native render]   of its setup: %.1f ms waiting for a frame slot (the GPU two frames back)\n", g_slotWaitMs - slotWaitBefore);
                if (g_frameStandIns)
                    std::fprintf(stderr, "[native render]   stand-ins drawn %llu times (textures on the uploader's way)\n", static_cast<unsigned long long>(g_frameStandIns));
                if (g_mkTexels)
                    std::fprintf(stderr, "[native render]   its textures made (%llu Ktexels): mips %.1f texture %.1f staging %.1f copy-in %.1f record %.1f ms\n",
                                 static_cast<unsigned long long>(g_mkTexels >> 10), g_mkMs[0], g_mkMs[1], g_mkMs[2], g_mkMs[3], g_mkMs[4]);
            }
        }
        static const bool profile = std::getenv("TF_NATIVE_PROFILE") != nullptr;
        if (profile && ++prof::frames == 300u)
        {
            std::fprintf(stderr, "[native render] profile (ms a frame): setup %.2f meshes %.2f textures %.2f record %.2f gpu %.2f; %llu textures made (%llu Mtexels), %llu refused, %zu resident, %zu free slots; targets made %llu, pictures %llu; video memory %llu of %llu MB\n",
                         prof::setup / 300, prof::meshes / 300, prof::textures / 300, prof::record / 300, prof::wait / 300,
                         static_cast<unsigned long long>(g_made), static_cast<unsigned long long>(g_madeTexels >> 20),
                         static_cast<unsigned long long>(g_refused), g.textures.size(), g.freeSlots.size(),
                         static_cast<unsigned long long>(g_targetsMade), static_cast<unsigned long long>(g_picturesMade),
                         static_cast<unsigned long long>(videoMemory(false) >> 20), static_cast<unsigned long long>(videoMemory(true) >> 20));
            std::fprintf(stderr, "[native render] profile: mesh buffers made %.1f a frame (%.0f vertices), %.1f meshes in the frame's ring, %zu resident; %.1f passes a frame read the picture\n",
                         g_meshesMade / 300.0, g_meshVertices / 300.0, g_ringMeshes / 300.0, g.meshes.size(), g_pictureReads / 300.0);
            std::fprintf(stderr, "[native render] profile: anti-aliasing in %u of 300 frames (SMAA in %u), %.3f ms of the render thread and %.3f ms of the GPU a frame it ran (%ux%u)\n",
                         g_aaFrames, g_smaaFrames, g_aaFrames ? g_aaCpu / g_aaFrames : 0.0, g_aaGpuFrames ? g_aaGpu / g_aaGpuFrames : 0.0, g.aaW, g.aaH);
            if (up::state.load() != 0)
            {
                size_t queued = 0;
                {
                    std::lock_guard<std::mutex> lock(up::mutex);
                    queued = up::jobs.size() + up::urgent.size();
                }
                std::fprintf(stderr, "[native render] profile: uploader %llu large images made (%llu Mtexels, %llu MB staged), %zu waiting, %llu failed; its thread busy %.0f ms in all\n",
                             static_cast<unsigned long long>(up::made.load()), static_cast<unsigned long long>(up::texels.load() >> 20),
                             static_cast<unsigned long long>(up::bytes.load() >> 20), queued, static_cast<unsigned long long>(up::failed.load()),
                             up::busyMs.load());
                // (And the targets': colour, depth, DATE copy - TODO 94's video memory split.)
                UINT64 targetBytes = 0;
                UINT bigTargets = 0;
                for (auto &[key, tg] : g.targets)
                {
                    UINT64 b = 0;
                    for (ID3D12Resource *r : {dt(tg).colour, dt(tg).depth, dt(tg).destCopy})
                        if (r)
                        {
                            const D3D12_RESOURCE_DESC rd = r->GetDesc();
                            b += g.device->GetResourceAllocationInfo(0, 1, &rd).SizeInBytes;
                        }
                    targetBytes += b;
                    bigTargets += b > (16ull << 20) ? 1u : 0u;
                }
                std::fprintf(stderr, "[native render] profile: targets %zu (%u over 16 MB), %llu MB; texture pool %llu MB\n", g.targets.size(), bigTargets,
                             static_cast<unsigned long long>(targetBytes >> 20), static_cast<unsigned long long>(pool::heaps.size() * (pool::kHeap >> 20)));
                const double pf = std::max(1.0, static_cast<double>(prof::frames));
                std::fprintf(stderr, "[native render] profile: a frame: %.0f passes drawn, %.0f pipeline changes (%.0f to or from the clipping shader), %.0f target changes, %.0f barriers; %.0f indices (%.0f through the clipping shader); %.0f scissors and %.0f blend factors set\n",
                             g_cntDraws / pf, g_cntPso / pf, g_cntGsSwitch / pf, g_cntTargets / pf, g_cntBarriers / pf, g_cntIndices / pf,
                             g_cntGsIndices / pf, g_cntScissors / pf, g_cntFactors / pf);
                g_cntDraws = g_cntPso = g_cntGsSwitch = g_cntTargets = g_cntBarriers = g_cntIndices = g_cntGsIndices = 0;
                g_cntScissors = g_cntFactors = 0;
                std::fprintf(stderr, "[native render] profile: large images on the GPU %llu MB (budget %llu MB, cap %u); %s\n",
                             static_cast<unsigned long long>(g_largeBytes.load() >> 20),
                             static_cast<unsigned long long>(largeImageBudget() == UINT64_MAX ? 0u : largeImageBudget() >> 20), largeImageCap(),
                             largeImageStats().c_str());
            }
            std::fprintf(stderr, "[native render] profile: the whole frame %.2f ms on the GPU (%u frames timed); the render thread waited %.2f ms a frame for a frame slot (in setup); window %u\n",
                         g_frameGpuFrames ? g_frameGpu / g_frameGpuFrames : 0.0, g_frameGpuFrames, g_slotWaitMs / 300.0, g_profWindow);
            std::fprintf(stderr, "[native render] profile: texture pool %zu heaps, %llu MB used, %llu placed, %llu not (committed); staging ring %llu uses, %llu overflows\n",
                         pool::heaps.size(), static_cast<unsigned long long>(pool::usedBytes() >> 20), static_cast<unsigned long long>(pool::made),
                         static_cast<unsigned long long>(pool::fallbacks), static_cast<unsigned long long>(g_texRingUses),
                         static_cast<unsigned long long>(g_texRingOverflows));
            pool::made = pool::fallbacks = g_texRingUses = g_texRingOverflows = 0;
            std::fprintf(stderr, "[native render] profile: stand-ins (game textures on the uploader's way) drawn %llu times in %llu frames, %llu textures\n",
                         static_cast<unsigned long long>(g_standInDraws), static_cast<unsigned long long>(g_standInFrames),
                         static_cast<unsigned long long>(g_standInTextures));
            g_standInDraws = g_standInFrames = g_standInTextures = 0;
            std::fprintf(stderr, "[native render] profile: clipping draws' runs a frame: %.1f inside (no clipping shader), %.1f through it\n",
                         g_clipRuns[0] / 300.0, g_clipRuns[1] / 300.0);
            g_clipRuns[0] = g_clipRuns[1] = g_clipRuns[2] = 0;
            ++g_profWindow;
            if (g_passFrames)
            {
                std::fprintf(stderr, "[native render] profile: GPU ms a frame by kind of draw (%u frames):", g_passFrames);
                for (int k = 0; k < kPassKinds; ++k)
                    std::fprintf(stderr, " %s %.2f%s", kPassKindNames[k], g_passGpu[k] / g_passFrames, k + 1 < kPassKinds ? "," : "");
                std::fputc('\n', stderr);
                std::sort(g_passTop.begin(), g_passTop.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
                for (size_t i = 0; i < g_passTop.size() && i < 12u; ++i)
                    std::fprintf(stderr, "[native render]   %.2f ms: %s\n", g_passTop[i].first, g_passTop[i].second.c_str());
                for (double &v : g_passGpu)
                    v = 0.0;
                g_passFrames = 0;
                if (!g_drawCostSum.empty())
                {
                    // One line a mark: window, mark, kind, ms (the window's average), the draw's state.
                    std::map<const void *, int> targetNo;
                    for (size_t i = 1; i < g_drawCostSum.size() && i < g_drawCostNote.size(); ++i)
                    {
                        const auto &[kind, nt] = g_drawCostNote[i];
                        const auto ins = targetNo.emplace(nt.target, static_cast<int>(targetNo.size()));
                        const double sf = g_drawStatFrames ? static_cast<double>(g_drawStatFrames) : 1.0;
                        std::fprintf(stderr, "[native render] cost w%u m%zu k%u %.4f d%u i%u v%u b%x t%d a%.0f tx%ux%u tex0 %016llx al %llx mode %u mesh %llu bar %u cp %u px %.0f tri %.0f\n",
                                     g_profWindow - 1u, i, kind, g_drawCostN[i] ? g_drawCostSum[i] / g_drawCostN[i] : -1.0, nt.draw, nt.indices, nt.verts, nt.bits,
                                     ins.first->second, nt.area, nt.texW, nt.texH, static_cast<unsigned long long>(nt.tex0),
                                     static_cast<unsigned long long>(nt.alpha), nt.mode, static_cast<unsigned long long>(nt.mesh), nt.barriers, nt.copies,
                                     i < g_drawPixels.size() ? g_drawPixels[i] / sf : -1.0, i < g_drawPrims.size() ? g_drawPrims[i] / sf : -1.0);
                    }
                    std::fprintf(stderr, "[native render] cost w%u: %u frames\n", g_profWindow - 1u, g_drawCostN[0]);
                    g_drawCostSum.clear();
                    g_drawCostN.clear();
                    g_drawCostNote.clear();
                    g_drawPixels.clear();
                    g_drawPrims.clear();
                    g_drawStatFrames = 0;
                }
            }
            g_frameGpu = g_slotWaitMs = 0.0;
            g_frameGpuFrames = 0;
            g_aaFrames = g_aaGpuFrames = g_smaaFrames = 0;
            g_aaCpu = g_aaGpu = 0.0;
            g_meshesMade = g_meshVertices = g_ringMeshes = g_pictureReads = 0;
            g_made = g_madeTexels = g_refused = 0;
            prof::setup = prof::meshes = prof::textures = prof::record = prof::wait = 0;
            prof::frames = 0;
        }
        // What the frame let go of: released when its slot comes round (its fence passed).
        for (ID3D12Resource *u : g.uploads)
            if (u)
                g.slotReleases[g.slot].push_back(u);
        g.uploads.clear();
        if (!shotPath.empty() && !ownShots)
            saveBmp(shotPath);
        if (aaReadback)
        {
            void *p = nullptr;
            if (SUCCEEDED(aaReadback->Map(0, nullptr, &p)))
            {
                const std::string path = shotPath.substr(0, shotPath.size() - 4) + "_noaa.bmp";
                if (FILE *f = std::fopen(path.c_str(), "wb"))
                {
                    const UINT w = g.aaW, h = g.aaH;
                    const uint32_t rowBytes = w * 4u, size = rowBytes * h;
                    uint8_t header[54] = {'B', 'M'};
                    auto put32 = [&](int at, uint32_t v) { std::memcpy(header + at, &v, 4); };
                    put32(2, 54u + size);
                    put32(10, 54u);
                    put32(14, 40u);
                    put32(18, w);
                    put32(22, h);
                    header[26] = 1;
                    header[28] = 32;
                    put32(34, size);
                    std::fwrite(header, 1, sizeof(header), f);
                    std::vector<uint8_t> row(rowBytes);
                    for (UINT y = h; y-- > 0;) // (the picture is RGBA: a BMP's rows are BGRA, bottom up)
                    {
                        std::memcpy(row.data(), static_cast<const uint8_t *>(p) + aaFp.Offset + static_cast<size_t>(y) * aaFp.Footprint.RowPitch, rowBytes);
                        for (uint32_t x = 0; x < rowBytes; x += 4u)
                            std::swap(row[x], row[x + 2u]);
                        std::fwrite(row.data(), 1, rowBytes, f);
                    }
                    std::fclose(f);
                }
                aaReadback->Unmap(0, nullptr);
            }
            aaReadback->Release();
        }
        if (waitNow)
            finishSlot(g.slot);
        if (savedReadback)
        {
            void *p = nullptr;
            if (SUCCEEDED(savedReadback->Map(0, nullptr, &p)))
            {
                const std::string path = shotPath.substr(0, shotPath.size() - 4) + "_target.bmp";
                if (FILE *f = std::fopen(path.c_str(), "wb"))
                {
                    const UINT w = saved->w, h = saved->h;
                    const uint32_t rowBytes = w * 4u, size = rowBytes * h;
                    uint8_t header[54] = {'B', 'M'};
                    auto put32 = [&](int at, uint32_t v) { std::memcpy(header + at, &v, 4); };
                    put32(2, 54u + size);
                    put32(10, 54u);
                    put32(14, 40u);
                    put32(18, w);
                    put32(22, h);
                    header[26] = 1;
                    header[28] = 32;
                    put32(34, size);
                    std::fwrite(header, 1, sizeof(header), f);
                    for (UINT y = h; y-- > 0;)
                        std::fwrite(static_cast<const uint8_t *>(p) + savedFp.Offset + static_cast<size_t>(y) * savedFp.Footprint.RowPitch, 1, rowBytes, f);
                    std::fclose(f);
                }
                savedReadback->Unmap(0, nullptr);
            }
            savedReadback->Release();
        }
        if (g_pcBack && !shotPath.empty())
        {
            void *p = nullptr;
            if (SUCCEEDED(g_pcBack->Map(0, nullptr, &p)))
            {
                const float *pc = static_cast<const float *>(p);
                std::fprintf(stderr, "[native render] picture constants (GPU): picture %g %g %g %g, turn diag %g %g %g %g; pictures %zu, s %g\n", pc[0], pc[1], pc[2],
                             pc[3], pc[4], pc[9], pc[14], pc[19], input.pictures.size(), input.pictures.empty() ? 1.0f : input.pictures.back().s);
                const D3D12_RANGE none{0, 0};
                g_pcBack->Unmap(0, &none);
            }
            g_pcBack = nullptr;
        }
        if (!g_pstatNotes.empty() && !shotPath.empty())
        {
            void *p = nullptr;
            const std::string path = shotPath.substr(0, shotPath.rfind('\\') + 1) + "pstats_" + std::to_string(input.tick) + ".txt";
            if (SUCCEEDED(g_pstatBack->Map(0, nullptr, &p)))
            {
                if (FILE *f = std::fopen(path.c_str(), "w"))
                {
                    const auto *st = static_cast<const D3D12_QUERY_DATA_PIPELINE_STATISTICS *>(p);
                    for (size_t k = 0; k < g_pstatNotes.size(); ++k)
                        std::fprintf(f, "%u.%u set %08x ia %llu vs %llu gs %llu gsout %llu cin %llu cout %llu ps %llu\n", g_pstatNotes[k].draw,
                                     g_pstatNotes[k].pass, g_pstatNotes[k].source, static_cast<unsigned long long>(st[k].IAPrimitives),
                                     static_cast<unsigned long long>(st[k].VSInvocations), static_cast<unsigned long long>(st[k].GSInvocations),
                                     static_cast<unsigned long long>(st[k].GSPrimitives), static_cast<unsigned long long>(st[k].CInvocations),
                                     static_cast<unsigned long long>(st[k].CPrimitives), static_cast<unsigned long long>(st[k].PSInvocations));
                    std::fclose(f);
                }
                const D3D12_RANGE none{0, 0};
                g_pstatBack->Unmap(0, &none);
            }
        }
        g_pstatNotes.clear();
        if (!afterSaves.empty() && afterSaves.front().split)
        {
            // (TF_NATIVE_SPLIT: the frame's small grey pictures - u32 count, then per picture u32 draw index and 160 x 90 bytes)
            const std::string path = shotPath.substr(0, shotPath.rfind('\\') + 1) + "split_" + std::to_string(input.tick) + ".bin";
            if (FILE *f = std::fopen(path.c_str(), "wb"))
            {
                uint32_t n = 0;
                for (const AfterSave &s : afterSaves)
                    n += s.split ? 1u : 0u;
                std::fwrite(&n, 4, 1, f);
                std::vector<uint8_t> thumb(160u * 90u);
                for (AfterSave &s : afterSaves)
                {
                    if (!s.split)
                        continue;
                    void *p = nullptr;
                    std::fill(thumb.begin(), thumb.end(), uint8_t{0});
                    if (SUCCEEDED(s.readback->Map(0, nullptr, &p)))
                    {
                        for (UINT y = 0; y < 90u; ++y)
                            for (UINT x = 0; x < 160u; ++x)
                            {
                                uint32_t sum = 0, cnt = 0;
                                const UINT y0 = y * s.h / 90u, y1 = std::max(y0 + 1u, (y + 1u) * s.h / 90u);
                                const UINT x0 = x * s.w / 160u, x1 = std::max(x0 + 1u, (x + 1u) * s.w / 160u);
                                for (UINT yy = y0; yy < y1; yy += 2u)
                                    for (UINT xx = x0; xx < x1; xx += 2u)
                                    {
                                        const uint8_t *px = static_cast<const uint8_t *>(p) + s.fp.Offset + static_cast<size_t>(yy) * s.fp.Footprint.RowPitch + xx * 4u;
                                        sum += (px[0] + 2u * px[1] + px[2]) / 4u;
                                        ++cnt;
                                    }
                                thumb[y * 160u + x] = static_cast<uint8_t>(cnt ? sum / cnt : 0u);
                            }
                        s.readback->Unmap(0, nullptr);
                    }
                    const uint32_t index = static_cast<uint32_t>(s.index);
                    std::fwrite(&index, 4, 1, f);
                    std::fwrite(thumb.data(), 1, thumb.size(), f);
                }
                std::fclose(f);
            }
            for (AfterSave &s : afterSaves)
                if (s.split)
                {
                    s.readback->Release();
                    s.readback = nullptr;
                }
            afterSaves.erase(std::remove_if(afterSaves.begin(), afterSaves.end(), [](const AfterSave &s) { return s.split; }), afterSaves.end());
        }
        for (AfterSave &s : afterSaves)
        {
            void *p = nullptr;
            if (SUCCEEDED(s.readback->Map(0, nullptr, &p)))
            {
                // (The targets are BGRA8: a 32-bit BMP's own order.)
                const std::string path = shotPath.substr(0, shotPath.size() - 4) + "_after" + std::to_string(s.index) + ".bmp";
                if (FILE *f = std::fopen(path.c_str(), "wb"))
                {
                    const uint32_t rowBytes = s.w * 4u, size = rowBytes * s.h;
                    uint8_t header[54] = {'B', 'M'};
                    auto put32 = [&](int at, uint32_t v) { std::memcpy(header + at, &v, 4); };
                    put32(2, 54u + size);
                    put32(10, 54u);
                    put32(14, 40u);
                    put32(18, s.w);
                    put32(22, s.h);
                    header[26] = 1;
                    header[28] = 32;
                    put32(34, size);
                    std::fwrite(header, 1, sizeof(header), f);
                    for (UINT y = s.h; y-- > 0;)
                        std::fwrite(static_cast<const uint8_t *>(p) + s.fp.Offset + static_cast<size_t>(y) * s.fp.Footprint.RowPitch, 1, rowBytes, f);
                    std::fclose(f);
                }
                s.readback->Unmap(0, nullptr);
            }
            s.readback->Release();
        }
    }
}

// (nr_types.h's preUpload: the capture's textures made ahead, to this file's queue.)
namespace nr
{
    void preUpload(const std::shared_ptr<const Texture> &texture) { d3d::preUploadImpl(texture); }
    bool rendererStarting() { return d3d::startingNow(); }
}

// (Roadmap P3, step S1) The Direct3D 12 backend as nr_backend.h's interface: today's nr::d3d calls behind it.
namespace nr
{
    namespace
    {
        class D3d12Backend final : public Backend
        {
        public:
            void prepare() override { d3d::prepare(); }
            bool start() override { return d3d::start(); }
            bool startingNow() override { return d3d::startingNow(); }
            bool drawsPictures() override { return d3d::drawsPictures(); }
            bool deviceLost() override { return d3d::deviceLost(); }
            bool grassGpuReady() override { return d3d::grassGpuReady(); }
            double waitForFrameSlot() override { return d3d::waitForFrameSlot(); }
            void render(const Frame &frame, const std::string &shotPath) override { d3d::render(frame, shotPath); }
            void preUpload(const std::shared_ptr<const Texture> &texture) override { d3d::preUploadImpl(texture); }
            bool latestPicture(std::vector<uint32_t> &pixels, uint32_t &width, uint32_t &height, uint64_t &serial, bool &has3D) override
            {
                return d3d::latestPicture(pixels, width, height, serial, has3D);
            }
            void setSharedPictures(bool on) override { d3d::setSharedPictures(on); }
            bool latestShared(d3d::SharedPicture &out, uint64_t &serial) override { return d3d::latestShared(out, serial); }
            void sharedHeld(int index) override { d3d::sharedHeld(index); }
            bool waitPicture(uint64_t serial, int64_t untilNs, int64_t &lastNs) override { return d3d::waitPicture(serial, untilNs, lastNs); }
            int pacingMode() override { return d3d::pacingMode(); }
            void setWindowLines(int lines) override { d3d::setWindowLines(lines); }
            void setAntiAliasing(int mode) override { d3d::setAntiAliasing(mode); }
            void setSupersampling(bool on) override { d3d::setSupersampling(on); }
            void setSpeedupsForced(int on) override { d3d::setSpeedupsForced(on); }
            uint32_t largeImageCap() override { return d3d::largeImageCap(); }
            uint64_t largeImageBudget() override { return d3d::largeImageBudget(); }
            uint64_t largeImageBytes() override { return d3d::largeImageBytes(); }
            uint64_t adapterMemoryMB() override { return d3d::adapterMemoryMB(); }
            int qualityPreset() override { return d3d::qualityPreset(); }
            double gpuPictureMs() override { return d3d::gpuPictureMs(); }
            double gpuExtraMs() override { return d3d::gpuExtraMs(); }
            double cpuPictureMs() override { return d3d::cpuPictureMs(); }
            double replayWaitMs() override { return d3d::replayWaitMs(); }
            double slotWaitMs() override { return d3d::slotWaitMs(); }
            double pacerDelayMs() override { return d3d::pacerDelayMs(); }
        };
    }

    Backend &backend()
    {
        static D3d12Backend b;
        return b;
    }
}

namespace nr::gfx
{
    // (P3 S3) The device in use: Direct3D 12 today.
    Device &device() { return nr::d3d::d3d12Device(); }
}
