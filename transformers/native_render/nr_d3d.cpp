// nr_d3d.cpp - the native renderer's Direct3D 12 backend: the device interface (gfx::Device, nr_device.h) implemented
// on Direct3D 12, the backend's entry (d3d::render: the device up and not lost, then the frame builder of nr_frame.cpp),
// the pictures' pacing and their hand-over to the window (nr_present.cpp, nr_glshare.cpp), and the shaders (HLSL, below).
// docs/RENDERER_BACKEND.md describes what a second backend has to provide.
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
#include "nr_frame.h"
#include "nr_glshare.h"
#include "runtime/ps2_thread_placement.h"

#if defined(_WIN32)
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
#else
#include "nr_vkd3d.h" // (Linux: Direct3D 12 through vkd3d)
#endif

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
}

namespace nr::d3d
{
    using namespace nr::frame; // (the frame builder's counters, settings and layout constants: nr_frame.h)
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
    // (TODO 290) A stunned creature's crackling shell (mode.z 6: taShader_ElectricShock, image 83f1's 0x1ac0): the vertex
    // pushed out by p1.z along its direction from the centre p2 (as the routine's RSQRT: none from the centre itself),
    // ST that direction's y and z plus the effect's offset p1.xy.
    float2 shockSt = float2(0.0, 0.0);
    if ((int)mode.z == 6)
    {
        const float3 d = i.pos - p[2].xyz;
        const float l2 = dot(d, d);
        const float3 dn = l2 > 0.0 ? d * rsqrt(l2) : float3(0.0, 0.0, 0.0);
        ipos = i.pos + dn * p[1].z;
        shockSt = dn.yz + p[1].xy;
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

)"
                              R"(    // (TODO 290: the string split here - MSVC's limit of one string literal)
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
    else if (cm == 21)
        c = trunc(P0);                                                    // (TODO 290) the electric shock: q1, FTOI0
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
    if ((int)mode.z == 6)
        st = shockSt;
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
// level the GS picks. lod.x 2 (the smooth-textures setting): MMAG's filter also where the level is magnified
// on screen, whatever the distance says.
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
    // (Issue #17, lod.x 2: the smooth-textures setting) The GS takes MMIN by distance alone, where it counts the
    // texture as shrunk; on a tall picture the same draw can show each texel over many pixels (the lit meshes' skin,
    // 0xa0: MMIN nearest - blocks at 1440 lines). Where the level drawn is magnified on screen (d < level), the
    // draw's MMAG filter applies instead, as the GS would choose for a magnified texture: a draw that asked for hard
    // texels when magnified (MMAG nearest) keeps them.
    const bool screenMag = lod.x > 1.5 && d < level;
    const bool smooth = base > 0.0 || ((mag || screenMag) ? lodf.y > 0.5 : lodf.z > 0.5);
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
        // (TODO 108) The modern bloom (TF_NATIVE_BLOOM, off by default): the picture shrunk by halves with a 13-tap filter (the
        // first step takes the game's threshold off), then each level enlarged by two with a 3 x 3 tent and added into the one
        // above, the last into the picture. A full-screen triangle; s4 = linear, clamped.
        const char kBloomShader[] = R"(
cbuffer Bloom : register(b0)
{
    float4 bloomSize;  // 1 / the target's width, height; 1 / the source's width, height
    float4 bloomParam; // down: the threshold rgb, w > 0: subtract it; up: the gain rgb
    float4 bloomProfile[2]; // up: a weight profile down the target (8 values, top to bottom; bloomShape.x of them: 0 none)
    float4 bloomShape;
};
Texture2D<float4> bloomIn : register(t0);
SamplerState bloomLinear : register(s4);

float4 bloomVs(uint id : SV_VertexID) : SV_Position
{
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float3 tap(float2 uv) { return bloomIn.SampleLevel(bloomLinear, uv, 0).rgb; }

float4 bloomDownPs(float4 pos : SV_Position) : SV_Target
{
    const float2 uv = pos.xy * bloomSize.xy, s = bloomSize.zw;
    // (13 taps: a box of four around the centre at half weight, four boxes on the corners at an eighth each)
    const float3 a = tap(uv + s * float2(-2.0, -2.0)), b = tap(uv + s * float2(0.0, -2.0)), c = tap(uv + s * float2(2.0, -2.0));
    const float3 d = tap(uv + s * float2(-1.0, -1.0)), e = tap(uv + s * float2(1.0, -1.0));
    const float3 f = tap(uv + s * float2(-2.0, 0.0)), g = tap(uv), h = tap(uv + s * float2(2.0, 0.0));
    const float3 i = tap(uv + s * float2(-1.0, 1.0)), j = tap(uv + s * float2(1.0, 1.0));
    const float3 k = tap(uv + s * float2(-2.0, 2.0)), l = tap(uv + s * float2(0.0, 2.0)), m = tap(uv + s * float2(2.0, 2.0));
    float3 o = (d + e + i + j) * 0.125 + (a + b + f + g) * 0.03125 + (b + c + g + h) * 0.03125 + (f + g + k + l) * 0.03125 +
               (g + h + l + m) * 0.03125;
    if (bloomParam.w > 0.0)
        o = max(o - bloomParam.rgb, 0.0);
    return float4(o, 1.0);
}

float4 bloomUpPs(float4 pos : SV_Position) : SV_Target
{
    const float2 uv = pos.xy * bloomSize.xy, s = bloomSize.zw;
    float3 o = tap(uv) * 4.0;
    o += (tap(uv + s * float2(-1.0, 0.0)) + tap(uv + s * float2(1.0, 0.0)) + tap(uv + s * float2(0.0, -1.0)) + tap(uv + s * float2(0.0, 1.0))) * 2.0;
    o += tap(uv + s * float2(-1.0, -1.0)) + tap(uv + s * float2(1.0, -1.0)) + tap(uv + s * float2(-1.0, 1.0)) + tap(uv + s * float2(1.0, 1.0));
    float w = 1.0;
    if (bloomShape.x > 0.5)
    {
        // (the profile: its values at the centres of equal bands down the target, linear between them)
        const float n = bloomShape.x, at = clamp(uv.y * n - 0.5, 0.0, n - 1.0);
        const uint i = (uint)floor(at), j = min(i + 1, (uint)n - 1);
        const float a = bloomProfile[i >> 2][i & 3], b = bloomProfile[j >> 2][j & 3];
        w = lerp(a, b, at - floor(at));
    }
    return float4(o * (bloomParam.rgb / 16.0) * w, 0.0);
}
)";

        // (TODO 108) The filmic tone mapping (TF_NATIVE_TONEMAP, off by default): the picture's colours (display-referred, as the
        // game drew them) taken back to linear and spread out to a pseudo-HDR range (an inverse curve: 1.0 goes to 1 / (1 - k)),
        // then through ACES's fitted curve at an exposure that keeps a dark grey (sRGB 0.30) where it was, back to sRGB - blended with the
        // picture as it was by `strength`. A full-screen triangle, before the HUD.
        const char kToneShader[] = R"(
cbuffer Tone : register(b0)
{
    float4 toneParam; // x exposure, y k, z strength, w saturation added (0: none)
};
Texture2D<float4> toneIn : register(t0);

float4 toneVs(uint id : SV_VertexID) : SV_Position
{
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float3 toLinear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float3 toSrgb(float3 c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055; }
float3 aces(float3 x) { return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14)); }

float4 tonePs(float4 pos : SV_Position) : SV_Target
{
    const float3 c = toneIn.Load(int3(pos.xy, 0)).rgb;
    const float3 lin = toLinear(c);
    const float3 hdr = lin / max(1.0 - toneParam.y * lin, 1.0 - toneParam.y);
    const float3 mapped = toSrgb(aces(hdr * toneParam.x));
    // (the curve faded out in the deepest shadows - deeper, never crushed - then the colour's saturation raised a little,
    // since a contrast curve dulls it)
    float3 o = lerp(c, mapped, toneParam.z * smoothstep(0.0, 0.25, c));
    const float l = dot(o, float3(0.2126, 0.7152, 0.0722));
    o = saturate(l + (o - l) * (1.0 + toneParam.w));
    return float4(o, 1.0);
}
)";

        // (TODO 108 (3) C1) The local contrast (TF_NATIVE_CLARITY, off by default): the picture's luminance lifted against a wide
        // blur of itself - k times the difference added to each channel (the colours kept), the difference held to +-0.2 so
        // hard edges get no halo. A full-screen triangle, before the HUD; s4 = linear, clamped.
        const char kClarityShader[] = R"(
cbuffer Clarity : register(b0)
{
    float4 clarityParam; // xy 1 / the target's width, height; z k
};
Texture2D<float4> clarityIn : register(t0);
Texture2D<float4> clarityBlur : register(t1);
SamplerState clarityLinear : register(s4);

float4 clarityVs(uint id : SV_VertexID) : SV_Position
{
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 clarityPs(float4 pos : SV_Position) : SV_Target
{
    const float3 c = clarityIn.Load(int3(pos.xy, 0)).rgb;
    const float3 b = clarityBlur.SampleLevel(clarityLinear, pos.xy * clarityParam.xy, 0).rgb;
    const float3 w = float3(0.2126, 0.7152, 0.0722);
    const float d = clamp(dot(c, w) - dot(b, w), -0.2, 0.2);
    return float4(saturate(c + d * clarityParam.z), 1.0);
}
)";

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
        // (their timestamps a slot: each picture's start and end, then the frame's list's start - written by the list itself
        // and copied out after each picture: on the RX 5700 XT a timestamp in the small list before the frame's came out
        // after the picture's end)
        constexpr UINT kPicStride = 2u * kMaxPictures + 1u;
        constexpr UINT kPictureUp = 256u;
        constexpr UINT kPredicateAt = 248u; // (the predicate's 8 bytes in a picture's upload entry)
        static_assert(sizeof(PictureConstants) <= kPredicateAt, "the picture's constants overlap its predicate");
        constexpr UINT64 kConstantRing = 32ull << 20;                         // a frame's passes' constants
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
            ID3D12Resource *constants = nullptr; // an upload heap, mapped for good
            uint8_t *constantsMapped = nullptr;
            ID3D12Resource *instances = nullptr; // (upload heap: each pass's matrix and q0, the vertex shader's per-instance stream)
            uint8_t *instancesMapped = nullptr;
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
#if defined(__linux__)
            // (N9: the CPU hand-over) The pictures' readbacks, apart from the frame slots: a picture the window
            // still reads (or whose copy the GPU still makes) is not written into, so a slot coming round again
            // costs no picture. They are also the window's queue: each published picture keeps its readback until
            // the window takes it (oldest first - a newer publication never replaces one waiting), so a present
            // delayed by a refresh costs no picture. Four: the frames in flight fill at most one (the frame before
            // this one - the one before that is finished by beginSlot), and two presents' worth of pictures fit
            // beside it even when the window misses a refresh.
            static constexpr int kHandovers = 4;
            struct Handover
            {
                ID3D12Resource *resource = nullptr; // (readback: mapped once, read by the window where the GPU wrote it)
                void *mapped = nullptr;
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
                UINT w = 0, h = 0;
                std::atomic<bool> held{false}; // (published and not given back: the window may read it - pictureShown)
                bool filled = false;           // (its copy recorded, not published yet - render thread)
                bool taken = false;            // (the window took it: its ack is coming - with g_published.mutex)
                uint64_t serial = 0;           // (the publication it carries: oldest first - with g_published.mutex)
                d3d::SharedPicture meta;       // (its tick, moment, ready time, turn and size - with g_published.mutex)
            };
            Handover handovers[kHandovers];
            int handoverNext = 0;
            int slotHandover[kFrames] = {-1, -1}; // (the readback the frame's picture is copied into)
#else
            ID3D12Resource *slotReadback[kFrames] = {};
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT slotFootprint[kFrames] = {};
            UINT slotReadbackW[kFrames] = {}, slotReadbackH[kFrames] = {};
            bool slotPublish[kFrames] = {};
#endif
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
            ID3D12PipelineState *bloomDownPipeline = nullptr, *bloomUpPipeline = nullptr; // (TODO 108: the modern bloom's)
            ID3D12PipelineState *tonePipeline = nullptr;                                 // (TODO 108: the tone mapping's)
            ID3D12PipelineState *clarityPipeline = nullptr;                              // (TODO 108: the local contrast's)
            UINT64 timestampHz = 0;
            // (P3 S3) The device's time stamps (gfx timestamp(): kTimestampsPerFrame a slot, resolved at closeFrame) and
            // statistics (pipelineStats(): made on the first ask, kStatEntriesPerFrame a slot), each with its readback.
            ID3D12QueryHeap *stampQueries = nullptr;
            ID3D12Resource *stampBack = nullptr;
            UINT stampsUsed[kFrames] = {};
            ID3D12QueryHeap *statHeap = nullptr;
            ID3D12Resource *statBack = nullptr;
            bool statTried = false;
            UINT statsUsed[kFrames] = {}; // (the last entry ended + 1)
            // (P3 S3) The device's read-backs (gfx readTarget), by id (entry 0 empty): a readback buffer each, kept for the
            // slot's later frames once released (made free again at the slot's next beginFrame - its frame then done).
            struct DeviceReadback
            {
                ID3D12Resource *buffer = nullptr;
                UINT64 size = 0;
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
                gfx::Format format = gfx::Format::RGBA8;
                void *mapped = nullptr;
                UINT slot = 0;
                bool live = false;
            };
            std::vector<DeviceReadback> readbacks = std::vector<DeviceReadback>(1);
            std::vector<gfx::ReadbackId> readbackFree[kFrames], readbackReleased[kFrames];
            // SMAA 1x (kSmaaEntries, TF_NATIVE_AA=2): its three pipelines, its edges (R8G8) and blend-weights (RGBA8)
            // targets at the picture's size - their render-target views and shader views kept between frames, in the
            // shader's state between passes - and its two lookup textures (uploaded at its first frame). Render-target
            // place 0 and slot 0 are never handed out, so 0 = none.
            ID3D12PipelineState *smaaEdgePipe = nullptr, *smaaWeightPipe = nullptr, *smaaBlendPipe = nullptr;
            ID3D12Resource *smaaArea = nullptr, *smaaSearch = nullptr;
            UINT smaaAreaSlot = 0, smaaSearchSlot = 0;
            bool smaaLookupUploaded = false;
            // The sun's occlusion probe (kProbeShader, roadmap N3a): its pipeline, its 64 x 64 target and render-target view,
            // a readback of 16 KB a slot, and whether the slot's frame took one (and its tick).
            ID3D12PipelineState *cloudPipeline = nullptr; // (TODO 27: the warp tunnel's sparks - kCloudShader, drawCloud)
            ID3D12PipelineState *probePipeline = nullptr;
        } g;

#if defined(__linux__)
        // The latest picture, handed to the window's thread (nr_present.cpp). The CPU hand-over's pictures are in
        // State::handovers (published ones stay there, oldest first, until the window takes them); this is only the
        // serial and the moment of the newest, for the pacer's wait and its 100 ms rule.
#else
        // The latest picture, handed to the window's thread (nr_present.cpp).
#endif
        struct Published
        {
            std::mutex mutex;
#if !defined(__linux__)
            std::vector<uint32_t> pixels;
#endif
            uint32_t width = 0, height = 0;
            uint64_t serial = 0;
            bool has3D = false;
            d3d::SharedPicture shared; // (when the newest picture is a shared texture: index >= 0)
            std::condition_variable cv; // (N9) a new picture handed over (the window's pacer waits on it)
            int64_t lastNs = 0;          // when the newest was
            uint32_t pending = 0u;       // (N9) shared textures given to the pacer, not handed over yet (pickShared leaves them)
        } g_published;
#if defined(__linux__)
        uint64_t g_handoverSkips = 0; // (N9: CPU pictures not read back - every readback the window's; a profile window)
#endif
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

            // The shaders built into the executable: build.ps1 runs `TransformersRecomp --bake-shaders <file>` after
            // linking (bakeShaders below: every shader this file compiles, through compile()), and builds the table
            // it writes in, so no start compiles them - the first one on a PC included (the grass's took ~15 s on an
            // N100). CMake starts with an empty table; a key it lacks goes on to the cache and the compiler as before.
            struct BakedShader
            {
                uint64_t key;
                const unsigned char *bytes;
                size_t size;
            };
#include "nr_shaders_baked.inc"
            std::atomic<uint32_t> g_bakedHits{0};
            bool g_baking = false; // (--bake-shaders: everything compiled afresh and collected, nothing written to the cache)
            std::vector<std::pair<uint64_t, std::vector<uint8_t>>> g_bakedOut;

            ID3DBlob *loadBaked(uint64_t key)
            {
                for (const BakedShader &b : kBakedShaders)
                {
                    if (b.key != key || b.size < 4u)
                        continue;
                    ID3DBlob *blob = nullptr;
                    if (FAILED(D3DCreateBlob(b.size, &blob)))
                        return nullptr;
                    std::memcpy(blob->GetBufferPointer(), b.bytes, b.size);
                    ++g_bakedHits;
                    return blob;
                }
                return nullptr;
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
            if (!cache::g_baking)
            {
                if (ID3DBlob *hit = cache::loadBaked(key))
                    return hit;
                if (ID3DBlob *hit = cache::loadShader(key))
                    return hit;
            }
            ID3DBlob *code = nullptr, *errors = nullptr;
            const HRESULT hr = D3DCompile(source, size, "nr_shader", defines, nullptr, entry, target, flags, 0, &code, &errors);
            if (FAILED(hr))
                std::fprintf(stderr, "[native render] shader %s: %s\n", entry,
                             errors ? static_cast<const char *>(errors->GetBufferPointer()) : "failed");
            release(errors);
            if (code && cache::g_baking)
            {
                const auto *p = static_cast<const uint8_t *>(code->GetBufferPointer());
                cache::g_bakedOut.emplace_back(key, std::vector<uint8_t>(p, p + code->GetBufferSize()));
            }
            else if (code)
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

        // (N9) the pictures' times for the pacer and the stats lines
        std::atomic<double> g_gpuPictureMs{0.0};      // (N9 stage 2: a picture's time on the GPU, smoothed - the pictures' cap)
        std::atomic<double> g_gpuExtraMs{0.0};        // (N9 stage 3: an extra picture's, drawn again from the frame's recording)
        std::atomic<double> g_cpuPictureMs{0.0};      // (N9 stage 3: the render thread's time a picture once the frame is recorded)
        double g_replayWaitMs = 0.0;                  // (d3d553: the last render call's waits for an extra picture's turn; render thread)
        std::atomic<double> g_slotWaitAll{0.0};       // (N9 stage 3: the render thread's waits for a frame slot, all of them - the GPU behind)

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
            // (A target made as the picture - TargetDesc::shared - names the old surfaces now: the frame builder makes it
            // again, seeing the picture's new size. Told: one holding a shader view of the old picture.)
            for (size_t id = 1; id < g.devTargets.size(); ++id)
                if (const State::DeviceTarget &d = g.devTargets[id]; d.picture && d.srv && d.srvOf)
                    std::fprintf(stderr, "[native render] picture made %ux%u while target %zu held a view of the old one (slot %u)\n", width, height, id,
                                 d.srv);
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

        ID3D12Resource *g_pcBack = nullptr; // (TODO 99 check: the picture constants read back on a saved frame) // (TODO 99: clipping triangles left out on the CPU; whole passes left out)
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
#if defined(__linux__)
                        std::fprintf(stderr, "[native render] grass on the GPU: shader %s%s failed\n", entries[k], g_exact ? " (divisions in double)" : "");
                        if (!g_exact)
                            return false;
                        // (a compiler without doubles - vkd3d's on Linux: the float divisions rather than the CPU's grass)
                        std::fprintf(stderr, "[native render] grass on the GPU: its divisions in float instead\n");
                        for (int j = 0; j < k; ++j)
                            release(g_pso[j]);
                        g_exact = false;
                        k = -1;
                        continue;
#else
                        std::fprintf(stderr, "[native render] grass on the GPU: shader %s failed\n", entries[k]);
                        return false;
#endif
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

        std::atomic<bool> g_deviceLost{false}; // the device removed (a GPU hang, TDR): nothing more is drawn

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
                ps2_thread_placement::place(ps2_thread_placement::Role::Helper); // (PS2X_THREAD_PLACEMENT: the HD texture uploads)
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

#if defined(__linux__)
        // (N9: the CPU hand-over) A readback the picture can be copied into and the window can upload from - mapped
        // once, so the window reads the pixels where the GPU wrote them (no copy of the picture on the render side).
        bool handoverMake(int i)
        {
            State::Handover &ho = g.handovers[i];
            D3D12_RESOURCE_DESC td{};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = g.width;
            td.Height = g.height;
            td.DepthOrArraySize = 1;
            td.MipLevels = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            UINT64 total = 0;
            g.device->GetCopyableFootprints(&td, 0, 1, 0, &ho.fp, nullptr, nullptr, &total);
            const D3D12_HEAP_PROPERTIES back = heap(D3D12_HEAP_TYPE_READBACK);
            const D3D12_RESOURCE_DESC rb = bufferDesc(total);
            if (FAILED(g.device->CreateCommittedResource(&back, D3D12_HEAP_FLAG_NONE, &rb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&ho.resource))))
                return false;
            void *p = nullptr;
            if (FAILED(ho.resource->Map(0, nullptr, &p)))
            {
                release(ho.resource);
                return false;
            }
            ho.mapped = p;
            ho.w = g.width;
            ho.h = g.height;
            return true;
        }

        // A readback made for the picture's size and marked as this frame's copy (render thread).
        int handoverUse(int i)
        {
            State::Handover &ho = g.handovers[i];
            if (ho.resource && (ho.w != g.width || ho.h != g.height))
            {
                // (the picture's size changed: freed here only once the window has let go of it)
                ho.resource->Unmap(0, nullptr);
                g.uploads.push_back(ho.resource); // (released once its last use is done)
                ho.resource = nullptr;
                ho.mapped = nullptr;
            }
            if (!ho.resource && !handoverMake(i))
                return -1;
            g.handoverNext = (i + 1) % State::kHandovers;
            ho.filled = true;
            return i;
        }

        // A readback for this frame's picture: a free one, else the oldest picture the window has not taken (the
        // window is a whole ring behind - that one is let go of unseen, the newest picture here kept).
        int handoverFor()
        {
            for (int k = 0; k < State::kHandovers; ++k)
            {
                const int i = (g.handoverNext + k) % State::kHandovers;
                if (g.handovers[i].filled || g.handovers[i].held.load(std::memory_order_acquire))
                    continue;
                if (const int picked = handoverUse(i); picked >= 0)
                    return picked;
            }
            int oldest = -1;
            {
                std::lock_guard<std::mutex> lock(g_published.mutex);
                for (int i = 0; i < State::kHandovers; ++i)
                {
                    const State::Handover &ho = g.handovers[i];
                    if (ho.filled || ho.taken || !ho.held.load(std::memory_order_relaxed))
                        continue;
                    if (oldest < 0 || ho.serial < g.handovers[oldest].serial)
                        oldest = i;
                }
                if (oldest < 0)
                    return -1; // (every readback is the window's - this picture is not read back: counted at the call)
                g.handovers[oldest].held.store(false, std::memory_order_release); // (never to be shown: let go)
            }
            return handoverUse(oldest);
        }
#else
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
#endif

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
                    CreateDirectoryA("runs/stream", nullptr);
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
                    std::snprintf(path, sizeof(path), "runs/stream/s_%06llu_%llu.bmp", static_cast<unsigned long long>(j.seq),
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

        std::function<void(uint32_t)> g_frameDone; // (gfx setFrameDone: the frame builder's, frameDone below)

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
                                nr::noteGpuFrame(ms); // (TODO 290 fps: the rate governor's way up)
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
            // (P3 S3: the frame builder's part - the sun's probe to the game, its profile's time stamps: frameDone. Not for the
            // slot being recorded - a wait for the GPU in the middle of a frame: its frame before was handed over when it began)
            if (g_frameDone && !(g.journalOn && slot == g.slot))
                g_frameDone(slot);
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
#if defined(__linux__)
            if (const int readback = g.slotHandover[slot]; readback >= 0)
            {
                g.slotHandover[slot] = -1;
                State::Handover &ho = g.handovers[readback];
                ho.filled = false;
                const int64_t readyNs =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                static int told = 3;
                if (told > 0)
                {
                    --told;
                    std::fprintf(stderr, "[native render] picture %ux%u handed over (readback %d)\n", ho.w, ho.h, readback);
                }
                {
                    std::lock_guard<std::mutex> lock(g_published.mutex);
                    // (it joins the queue at its newest end: earlier pictures keep their readbacks until the window
                    //  takes them - a newer publication never replaces one that is waiting, so a present delayed by
                    //  a refresh costs no picture)
                    ho.meta = d3d::SharedPicture{};
                    ho.meta.width = ho.w;
                    ho.meta.height = ho.h;
                    ho.meta.has3D = g.slotHas3D[slot];
                    ho.meta.tick = g.slotTick[slot];
                    ho.meta.dueNs = g.slotDueNs[slot];
                    ho.meta.readyNs = readyNs;
                    ho.meta.turnId = g.slotFence[slot];
                    ho.serial = ++g_published.serial;
                    ho.taken = false;
                    ho.held.store(true, std::memory_order_release); // (from here the window may take and read it)
                    g_published.shared.index = -1; // (no shared texture: latestShared must not hand this one out)
                    g_published.has3D = g.slotHas3D[slot];
                    g_published.width = ho.w;
                    g_published.height = ho.h;
                    g_published.lastNs = readyNs;
                    g_published.cv.notify_all();
                }
            }
#else
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
                        // (when it came, and the window woken for it - waitPicture: the CPU hand-over's pictures are paced
                        // as the shared ones are; without it the window, paced since its shared pictures, saw a new picture on
                        // every pass and drew the same one ~190 times a second)
                        g_published.lastNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                    }
                    g_published.cv.notify_all();
                    const D3D12_RANGE written{0, 0};
                    g.slotReadback[slot]->Unmap(0, &written);
                }
            }
            g.slotPublish[slot] = false;
#endif
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
#if defined(_WIN32) // (vkd3d has neither the debug layer nor DRED)
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
#endif
        if (const HRESULT made = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.device)); FAILED(made))
        {
            // (With the debug layer on, a fullscreen window's start makes the device fail with
            // DXGI_ERROR_DEVICE_RESET, 887a0007 - debug runs take PS2X_FULLSCREEN=0.)
            std::fprintf(stderr, "[native render] no Direct3D 12 device (%08lx)%s\n", static_cast<unsigned long>(made),
                         debugging ? " - with the debug layer, run windowed (PS2X_FULLSCREEN=0)" : "");
            return false;
        }
#if defined(_WIN32)
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
#endif
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
        std::fprintf(stderr, "[native render] shaders: %.0f ms (%u built in, %u from the shader cache, %u compiled and kept)\n",
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - shadersStart).count(), cache::g_bakedHits.load(),
                     cache::g_shaderHits.load(), cache::g_shaderMade.load());
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
            // (TODO 108) The modern bloom's two pipelines: down (written) and up (added to the target times the blend factor, the
            // target's alpha kept). None: the
            // game's own glow is drawn (told once).
            {
                ID3DBlob *bv = compile("bloomVs", "vs_5_1", kBloomShader, sizeof(kBloomShader) - 1),
                         *bd = compile("bloomDownPs", "ps_5_1", kBloomShader, sizeof(kBloomShader) - 1),
                         *bu = compile("bloomUpPs", "ps_5_1", kBloomShader, sizeof(kBloomShader) - 1);
                for (int k = 0; k < 2 && bv && bd && bu; ++k)
                {
                    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
                    pd.pRootSignature = g.root;
                    pd.VS = {bv->GetBufferPointer(), bv->GetBufferSize()};
                    ID3DBlob *ps = k == 0 ? bd : bu;
                    pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
                    D3D12_RENDER_TARGET_BLEND_DESC &rt = pd.BlendState.RenderTarget[0];
                    rt.BlendEnable = k == 1 ? TRUE : FALSE;
                    rt.SrcBlend = rt.SrcBlendAlpha = D3D12_BLEND_ONE;
                    rt.DestBlend = rt.DestBlendAlpha = k == 1 ? D3D12_BLEND_BLEND_FACTOR : D3D12_BLEND_ZERO; // (up: + the target x the factor)
                    rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                    rt.LogicOp = D3D12_LOGIC_OP_NOOP;
                    rt.RenderTargetWriteMask = k == 1 ? D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE
                                                      : D3D12_COLOR_WRITE_ENABLE_ALL;
                    pd.SampleMask = 0xFFFFFFFFu;
                    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
                    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                    pd.RasterizerState.DepthClipEnable = TRUE;
                    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                    pd.NumRenderTargets = 1;
                    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
                    pd.SampleDesc.Count = 1;
                    ID3D12PipelineState *&out = k == 0 ? g.bloomDownPipeline : g.bloomUpPipeline;
                    if (FAILED(g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&out))))
                        out = nullptr;
                }
                release(bv);
                release(bd);
                release(bu);
                if (!g.bloomDownPipeline || !g.bloomUpPipeline)
                    std::fprintf(stderr, "[native render] the modern bloom could not be made: the game's glow is drawn\n");
            }
            // (TODO 108 C1) The local contrast's pipeline: written, the picture's alpha kept (none: the picture as drawn, told once).
            {
                ID3DBlob *cv = compile("clarityVs", "vs_5_1", kClarityShader, sizeof(kClarityShader) - 1),
                         *cp = compile("clarityPs", "ps_5_1", kClarityShader, sizeof(kClarityShader) - 1);
                if (cv && cp)
                {
                    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
                    pd.pRootSignature = g.root;
                    pd.VS = {cv->GetBufferPointer(), cv->GetBufferSize()};
                    pd.PS = {cp->GetBufferPointer(), cp->GetBufferSize()};
                    D3D12_RENDER_TARGET_BLEND_DESC &rt = pd.BlendState.RenderTarget[0];
                    rt.SrcBlend = rt.SrcBlendAlpha = D3D12_BLEND_ONE;
                    rt.DestBlend = rt.DestBlendAlpha = D3D12_BLEND_ZERO;
                    rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                    rt.LogicOp = D3D12_LOGIC_OP_NOOP;
                    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
                    pd.SampleMask = 0xFFFFFFFFu;
                    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
                    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                    pd.RasterizerState.DepthClipEnable = TRUE;
                    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                    pd.NumRenderTargets = 1;
                    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
                    pd.SampleDesc.Count = 1;
                    if (FAILED(g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g.clarityPipeline))))
                        g.clarityPipeline = nullptr;
                }
                release(cv);
                release(cp);
                if (!g.clarityPipeline)
                    std::fprintf(stderr, "[native render] the local contrast could not be made: the picture is drawn as it is\n");
            }
            // (TODO 108) The tone mapping's pipeline: written, the picture's alpha kept (none: the picture as drawn, told once).
            {
                ID3DBlob *tv = compile("toneVs", "vs_5_1", kToneShader, sizeof(kToneShader) - 1),
                         *tp = compile("tonePs", "ps_5_1", kToneShader, sizeof(kToneShader) - 1);
                if (tv && tp)
                {
                    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
                    pd.pRootSignature = g.root;
                    pd.VS = {tv->GetBufferPointer(), tv->GetBufferSize()};
                    pd.PS = {tp->GetBufferPointer(), tp->GetBufferSize()};
                    D3D12_RENDER_TARGET_BLEND_DESC &rt = pd.BlendState.RenderTarget[0];
                    rt.SrcBlend = rt.SrcBlendAlpha = D3D12_BLEND_ONE;
                    rt.DestBlend = rt.DestBlendAlpha = D3D12_BLEND_ZERO;
                    rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                    rt.LogicOp = D3D12_LOGIC_OP_NOOP;
                    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
                    pd.SampleMask = 0xFFFFFFFFu;
                    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
                    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                    pd.RasterizerState.DepthClipEnable = TRUE;
                    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                    pd.NumRenderTargets = 1;
                    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
                    pd.SampleDesc.Count = 1;
                    if (FAILED(g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g.tonePipeline))))
                        g.tonePipeline = nullptr;
                }
                release(tv);
                release(tp);
                if (!g.tonePipeline)
                    std::fprintf(stderr, "[native render] the tone mapping could not be made: the picture is drawn as it is\n");
            }
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
                       lookup(SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, DXGI_FORMAT_R8_UNORM, g.smaaSearch, g.smaaSearchSlot);
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
                if (pv && pp)
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
                    if (FAILED(g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g.probePipeline))))
                        g.probePipeline = nullptr;
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
            // (P3 S3: gfx timestamp()'s heap - the frame builder's stamps, the profile's marks and the anti-aliasing's two)
            D3D12_QUERY_HEAP_DESC qh{};
            qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            qh.Count = gfx::kTimestampsPerFrame * State::kFrames;
            const D3D12_HEAP_PROPERTIES back = heap(D3D12_HEAP_TYPE_READBACK);
            const D3D12_RESOURCE_DESC tb = bufferDesc(sizeof(UINT64) * gfx::kTimestampsPerFrame * State::kFrames);
            if (FAILED(g.queue->GetTimestampFrequency(&g.timestampHz)) || FAILED(g.device->CreateQueryHeap(&qh, IID_PPV_ARGS(&g.stampQueries))) ||
                FAILED(g.device->CreateCommittedResource(&back, D3D12_HEAP_FLAG_NONE, &tb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g.stampBack))))
            {
                release(g.stampQueries);
                release(g.stampBack);
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

    }
    uint64_t videoMemory(bool budget);
    uint32_t adapterVendor();
    namespace
    {
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
            uint32_t textureSlotsFree() override { return static_cast<uint32_t>(g.freeSlots.size()); }
            uint64_t textureBytes(gfx::TextureId id) override { return id < g.devTextures.size() ? g.devTextures[id].bytes : 0u; }

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

            // --- frame life ---
            // (The slot's frame before is done when this runs - beginSlot waited for it: its read-backs free again.)
            // (The picture's surfaces, the slot waited for - its frame-done call made in it - and the per-frame work that
            // records nothing.)
            bool prepareFrame(uint32_t pictureWidth, uint32_t pictureHeight) override
            {
                if (!makeTargets(pictureWidth, pictureHeight))
                    return false;
                beginSlot();
                psv::collect();
                plib::takeWarmed();
                plib::maybeSave();
                g_texRingAt = 0; // (the slot's part of the staging ring: its last frame finished)
                return true;
            }
            bool beginFrame(uint64_t) override
            {
                takeUploaded();
                const UINT s = g.slot;
                g.readbackFree[s].insert(g.readbackFree[s].end(), g.readbackReleased[s].begin(), g.readbackReleased[s].end());
                g.readbackReleased[s].clear();
                g.allocators[s]->Reset();
                g.picAllocators[s]->Reset();
                g.list = g.frameLists[s]; // (d3d553: the slot's own - the other slot's may still have pictures to submit)
                g.list->Reset(g.allocators[s], g.pipeline);
                // (N9 stage 3: the frame's list may be drawn more than once - every state it moves a resource through noted)
                g.journal.clear();
                g.journalOrder.clear();
                g.journalOn = true;
                g.stampsUsed[s] = 0;
                g.statsUsed[s] = 0;
                // (the whole frame's time on the GPU: each picture's own two timestamps - N9 stage 3, finishSlot; this the
                // frame's list's start, copied out after each picture)
                if (g.picQueries && g.picTimes)
                    g.list->EndQuery(g.picQueries, D3D12_QUERY_TYPE_TIMESTAMP, s * kPicStride + 2u * kMaxPictures);
                // The bindings every draw uses: the root signature, the picture's constants (b1), the descriptor heap; the
                // white texture (slot 0) made the first time.
                g.list->SetGraphicsRootSignature(g.root);
                g.list->SetGraphicsRootConstantBufferView(4, g.pictureCB->GetGPUVirtualAddress() + 256ull * s); // (N9 stage 3: the picture's)
                g.list->SetDescriptorHeaps(1, &g.srvHeap);
                if (!g.white)
                {
                    const uint32_t white = 0xFFFFFFFFu;
                    g.white = makeTexture(1u, 1u, &white, 0u);
                }
                g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                {
                    // (TODO 94: the draws' constants read from video memory - TF_NATIVE_CBVRAM=alt: by turns, a profile window each)
                    static const bool cbAlt = std::getenv("TF_NATIVE_CBVRAM") && *std::getenv("TF_NATIVE_CBVRAM") == 'a';
                    g_cbVram = g.constantsV && g.preList && !(cbAlt && (g_profWindow & 1u));
                }
                beginRings();
                // (TODO 94: the draw state last set in the list - a scissor, blend factor or pipeline the same as the draw
                // before's is not set again. On AMD each is a context register: set, it starts a new context for the draws
                // after, and a run of small draws each starting one waits on the GPU's few contexts. TF_NATIVE_STATEFILTER=0:
                // every draw sets them, as before; =alt: by turns.)
                static const char *stateFilterEnv = std::getenv("TF_NATIVE_STATEFILTER");
                beginDraws(speedupOn(stateFilterEnv));
                return true;
            }
            uint32_t frameSlot() override { return g.slot; }
            // (The recording's end: its time stamps and statistics resolved, the list closed.)
            bool closeFrame()
            {
                const UINT s = g.slot;
                if (g.stampQueries && g.stampBack && g.stampsUsed[s])
                    g.list->ResolveQueryData(g.stampQueries, D3D12_QUERY_TYPE_TIMESTAMP, s * gfx::kTimestampsPerFrame, g.stampsUsed[s], g.stampBack,
                                             static_cast<UINT64>(s) * gfx::kTimestampsPerFrame * 8ull);
                if (g.statHeap && g.statBack && g.statsUsed[s])
                    g.list->ResolveQueryData(g.statHeap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, s * gfx::kStatEntriesPerFrame, g.statsUsed[s], g.statBack,
                                             static_cast<UINT64>(s) * gfx::kStatEntriesPerFrame * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS));
                const HRESULT closed = g.list->Close();
                g.journalOn = false;
                if (FAILED(closed))
                {
                    static int told = 5;
                    if (told-- > 0)
                        std::fprintf(stderr, "[native render] the frame's list did not close (%08lx)\n", static_cast<unsigned long>(closed));
                    return false;
                }
                return true;
            }
            void setFrameDone(std::function<void(uint32_t)> done) override { g_frameDone = std::move(done); }
            // (The recording closed, then run once for each picture - each with a small list before it (its constants, the
            // predicate) and one after it (its time stamp, its copy to the window, its shot) - then what the frame let go of,
            // and with waitForGpu the slot finished at once.)
            bool endFrame(const gfx::FrameEnd &end) override
            {
                const bool closed = closeFrame();
                const Frame &in = *end.frame;
                // ---- N9 stage 3: the frame's pictures, each the frame's list run again ----------------------------------------
                // The list above is the whole frame, recorded once; each picture runs it with its own constants (where it lies
                // between the frame before's pose and this frame's, the camera's turn: PictureConstants, the shader's b1, written
                // by a small list before it) and a small list after it (its copy into the shared texture shown, its timestamps).
                // Before an extra picture the resources the list moved are set back to the states it began with. A frame of one
                // picture (Frame::pictures empty) is drawn the same way, once, at this frame's pose.
                const std::vector<gfx::PictureOut> &pictures = end.pictures;
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
                const auto picturesStart = std::chrono::steady_clock::now();
                const bool waitNow = end.waitForGpu;
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
                    const gfx::PictureOut &pic = pictures[k];
                    const bool last = k + 1u == pictureCount;
                    uint8_t *const picEntry = g.pictureUpMapped + (static_cast<size_t>(g.slot) * kMaxPictures + k) * kPictureUp;
                    {
                        // (its constants as the frame builder wrote them - PictureConstants, nr_draw.h)
                        uint8_t *entry = picEntry;
                        std::memset(entry, 0, sizeof(PictureConstants));
                        std::memcpy(entry, pic.constants.data(), std::min<size_t>(pic.constants.size(), sizeof(PictureConstants)));
                        const uint64_t skip = executed > 0 ? 1u : 0u; // (the predicate: an extra picture leaves out what it can)
                        std::memcpy(entry + kPredicateAt, &skip, 8u);
                    }
                    // (the shared texture this picture is copied into: not one shown or waiting for its moment)
                    int sharedIndex = g.live && g_shareOn.load() == 1 && !in.dropped ? pickShared() : -1; // (TODO 161 a: a frame not shown)
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
                    if (g.live && g_shareOn.load() == 1 && !in.dropped)
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
                        g.slotHas3D[g.slot] = in.has3D || in.shown2D; // (a menu shown natively: TODO 88)
                        g.slotTick[g.slot] = in.tick;
                        g.slotDueNs[g.slot] = pic.dueNs;
                    }
#if defined(__linux__)
                    else if (last && g.live && g_shareOn.load() != 1 && !in.dropped)
                    {
                        const int handover = handoverFor();
                        g_handoverSkips += handover < 0 ? 1u : 0u; // (told in the profile window: pictures not read back)
                        if (handover >= 0)
                        {
                            barrier(tail, g.colour, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
                            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
                            dst.pResource = g.handovers[handover].resource;
                            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                            dst.PlacedFootprint = g.handovers[handover].fp;
                            src.pResource = g.colour;
                            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                            tail->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                            barrier(tail, g.colour, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                            g.slotHandover[g.slot] = handover;
                            g.slotHas3D[g.slot] = in.has3D || in.shown2D; // (a menu shown natively: TODO 88)
                            g.slotTick[g.slot] = in.tick;
                            g.slotDueNs[g.slot] = pic.dueNs;
                        }
                    }
#else
                    else if (last && g.live && g_shareOn.load() != 1 && !in.dropped && slotReadbackFor(g.slot))
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
                        g.slotHas3D[g.slot] = in.has3D || in.shown2D; // (a menu shown natively: TODO 88)
                    }
#endif
                    if (last && !in.dropped && stream::wanted(in.tick) && stream::readbackFor(g.slot))
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
                        stream::tick[g.slot] = in.tick;
                    }
                    if (!pic.shot.empty() || (last && !end.shot.empty() && !ownShots))
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
                        if (sigPc && !end.shot.empty() && last)
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
                    // its constants just before it is submitted (PictureOut::beforeSubmit, the frame builder's): an extra picture's on
                    // the submitting thread, which may wait for the moment it is read at; the frame's own here, without waiting. Its id
                    // for the pacing log: its fence value.)
                    std::function<void()> turnFill;
                    if (pic.beforeSubmit)
                        turnFill = [before = pic.beforeSubmit, picEntry, id = g.fenceValue + 1u, extra = executed > 0]() {
                            const double waited = before(picEntry, id, extra);
                            if (waited > 0.0)
                                replay::g_turnWaitMs.store(replay::g_turnWaitMs.load(std::memory_order_relaxed) + waited, std::memory_order_relaxed);
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
                            const auto waitStart = std::chrono::steady_clock::now();
                            waitFence(g.fenceValue); // (the picture before: the frame's list and the restore list free to run again)
                            replayWaitMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitStart).count();
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
                        shown.tick = in.tick;
                        shown.dueNs = pic.dueNs;
                        shown.stamp = pictureStamp;
                        shown.turnId = g.fenceValue; // (N9: its camera turn's shown yaw, view_turn.h)
                        g.slotPaced[g.slot] = true;
                        pace::submit(g.fenceValue, shown, in.periodNs, in.vblanksOnRefresh, in.pictureNs, in.refreshNs);
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
                    const double ms = (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - picturesStart).count() - replayWaitMs) / executed;
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
                if (const HRESULT removed = g.device->GetDeviceRemovedReason(); FAILED(removed))
                {
                    g_deviceLost.store(true, std::memory_order_relaxed);
                    static bool told = false;
                    if (!told)
                    {
                        told = true;
                        std::fprintf(stderr, "[native render] the Direct3D 12 device was removed (%08lx) at a frame of %zu draws - the native renderer stops, the emulated one draws\n",
                                     static_cast<unsigned long>(removed), in.draws.size());
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
                // What the frame let go of: released when its slot comes round (its fence passed).
                for (ID3D12Resource *u : g.uploads)
                    if (u)
                        g.slotReleases[g.slot].push_back(u);
                g.uploads.clear();
                if (!end.shot.empty() && !ownShots)
                    saveBmp(end.shot);
                if (waitNow)
                    finishSlot(g.slot);
                if (g_pcBack && !end.shot.empty())
                {
                    void *p = nullptr;
                    if (SUCCEEDED(g_pcBack->Map(0, nullptr, &p)))
                    {
                        const float *pc = static_cast<const float *>(p);
                        std::fprintf(stderr, "[native render] picture constants (GPU): picture %g %g %g %g, turn diag %g %g %g %g; pictures %zu, s %g\n", pc[0], pc[1],
                                     pc[2], pc[3], pc[4], pc[9], pc[14], pc[19], in.pictures.size(), in.pictures.empty() ? 1.0f : in.pictures.back().s);
                        const D3D12_RANGE none{0, 0};
                        g_pcBack->Unmap(0, &none);
                    }
                    g_pcBack = nullptr;
                }
                return closed;
            }
            // --- the GPU grass (grassgpu: the device's compute pass) ---
            void growGrass(const Frame &frame) override { grassgpu::record(frame); }
            bool grassMesh(uint64_t groundMeshId) override { return grassgpu::meshFor(groundMeshId) != nullptr; }
            bool effect(gfx::Effect e) override
            {
                switch (e)
                {
                case gfx::Effect::None: return true;
                case gfx::Effect::Fxaa: return g.aaPipeline != nullptr;
                case gfx::Effect::SmaaEdges: return g.smaaEdgePipe != nullptr;
                case gfx::Effect::SmaaWeights: return g.smaaWeightPipe != nullptr;
                case gfx::Effect::SmaaBlend: return g.smaaBlendPipe != nullptr;
                case gfx::Effect::Probe: return g.probePipeline != nullptr;
                case gfx::Effect::Cloud: return g.cloudPipeline != nullptr;
                case gfx::Effect::BloomDown: return g.bloomDownPipeline != nullptr;
                case gfx::Effect::BloomUp: return g.bloomUpPipeline != nullptr;
                case gfx::Effect::ToneMap: return g.tonePipeline != nullptr;
                case gfx::Effect::LocalContrast: return g.clarityPipeline != nullptr;
                }
                return false;
            }
            void report(gfx::Report what) override;
            uint32_t vendorId() override { return d3d::adapterVendor(); }
            uint64_t videoMemory(bool budget) override { return d3d::videoMemory(budget); }
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
                    case gfx::Effect::Probe: pso = g.probePipeline; break;
                    case gfx::Effect::BloomDown: pso = g.bloomDownPipeline; break;
                    case gfx::Effect::BloomUp: pso = g.bloomUpPipeline; break;
                    case gfx::Effect::ToneMap: pso = g.tonePipeline; break;
                    case gfx::Effect::LocalContrast: pso = g.clarityPipeline; break;
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
            // --- time stamps (the slot's part of the heap, resolved at closeFrame) ---
            bool timestamps() override { return g.stampQueries && g.stampBack && g.timestampHz; }
            gfx::QueryId timestamp() override
            {
                UINT &n = g.stampsUsed[g.slot];
                if (!g.stampQueries || n >= gfx::kTimestampsPerFrame)
                    return 0;
                g.list->EndQuery(g.stampQueries, D3D12_QUERY_TYPE_TIMESTAMP, g.slot * gfx::kTimestampsPerFrame + n);
                return ++n;
            }
            bool readTimestamps(uint32_t slot, std::vector<uint64_t> &ticks, uint64_t &ticksPerSecond) override
            {
                ticks.clear();
                ticksPerSecond = g.timestampHz;
                const UINT n = slot < State::kFrames ? std::min(g.stampsUsed[slot], gfx::kTimestampsPerFrame) : 0u;
                if (!n || !g.stampBack || !g.timestampHz)
                    return false;
                void *p = nullptr;
                const SIZE_T at = static_cast<SIZE_T>(slot) * gfx::kTimestampsPerFrame * 8u;
                const D3D12_RANGE r{at, at + n * 8u};
                if (FAILED(g.stampBack->Map(0, &r, &p)))
                    return false;
                const UINT64 *t = reinterpret_cast<const UINT64 *>(static_cast<const uint8_t *>(p) + at);
                ticks.assign(t, t + n);
                const D3D12_RANGE none{0, 0};
                g.stampBack->Unmap(0, &none);
                return true;
            }
            // --- pipeline statistics (profile and debug only: the heap made on the first ask) ---
            bool pipelineStats() override
            {
                if (!g.statTried)
                {
                    g.statTried = true;
                    D3D12_QUERY_HEAP_DESC sh{};
                    sh.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
                    sh.Count = gfx::kStatEntriesPerFrame * State::kFrames;
                    const D3D12_HEAP_PROPERTIES back = heap(D3D12_HEAP_TYPE_READBACK);
                    const D3D12_RESOURCE_DESC sb = bufferDesc(sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS) * gfx::kStatEntriesPerFrame * State::kFrames);
                    if (FAILED(g.device->CreateQueryHeap(&sh, IID_PPV_ARGS(&g.statHeap))) ||
                        FAILED(g.device->CreateCommittedResource(&back, D3D12_HEAP_FLAG_NONE, &sb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g.statBack))))
                    {
                        release(g.statHeap);
                        release(g.statBack);
                    }
                }
                return g.statHeap && g.statBack;
            }
            void beginStats(uint32_t entry) override
            {
                if (g.statHeap && entry < gfx::kStatEntriesPerFrame)
                    g.list->BeginQuery(g.statHeap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, g.slot * gfx::kStatEntriesPerFrame + entry);
            }
            void endStats(uint32_t entry) override
            {
                if (!g.statHeap || entry >= gfx::kStatEntriesPerFrame)
                    return;
                g.list->EndQuery(g.statHeap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, g.slot * gfx::kStatEntriesPerFrame + entry);
                g.statsUsed[g.slot] = std::max(g.statsUsed[g.slot], entry + 1u);
            }
            bool readStats(uint32_t slot, std::vector<gfx::PipelineStats> &out) override
            {
                out.clear();
                const UINT n = slot < State::kFrames ? g.statsUsed[slot] : 0u;
                if (!n || !g.statBack)
                    return false;
                void *p = nullptr;
                const SIZE_T at = static_cast<SIZE_T>(slot) * gfx::kStatEntriesPerFrame * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
                const D3D12_RANGE r{at, at + n * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS)};
                if (FAILED(g.statBack->Map(0, &r, &p)))
                    return false;
                const auto *st = reinterpret_cast<const D3D12_QUERY_DATA_PIPELINE_STATISTICS *>(static_cast<const uint8_t *>(p) + at);
                out.resize(n);
                for (UINT i = 0; i < n; ++i)
                    out[i] = {st[i].IAVertices,    st[i].IAPrimitives, st[i].VSInvocations, st[i].GSInvocations,
                              st[i].GSPrimitives,  st[i].CInvocations, st[i].CPrimitives,   st[i].PSInvocations};
                const D3D12_RANGE none{0, 0};
                g.statBack->Unmap(0, &none);
                return true;
            }
            // --- predication: the slot's 8 bytes of the predicate buffer, 1 in an extra picture (its pro list writes it) ---
            bool predication() override { return g.predicate != nullptr; }
            void predicate(bool skip) override
            {
                g.list->SetPredication(skip ? g.predicate : nullptr, skip ? 8ull * g.slot : 0ull,
                                       skip ? D3D12_PREDICATION_OP_NOT_EQUAL_ZERO : D3D12_PREDICATION_OP_EQUAL_ZERO);
            }
            // --- read-backs ---
            gfx::ReadbackId readTarget(gfx::TargetId id) override
            {
                State::DeviceTarget &t = dt(id);
                if (!id || !t.colour)
                    return 0;
                const D3D12_RESOURCE_DESC td = t.colour->GetDesc();
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
                UINT64 total = 0;
                g.device->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
                const UINT s = g.slot;
                gfx::ReadbackId r = 0;
                // (one of the slot's own, its last frame done, big enough - the probe's: the same one every other frame)
                std::vector<gfx::ReadbackId> &pool = g.readbackFree[s];
                for (size_t i = 0; i < pool.size(); ++i)
                    if (g.readbacks[pool[i]].size >= total)
                    {
                        r = pool[i];
                        pool.erase(pool.begin() + static_cast<std::ptrdiff_t>(i));
                        break;
                    }
                if (!r)
                {
                    ID3D12Resource *buffer = nullptr;
                    const D3D12_HEAP_PROPERTIES back = heap(D3D12_HEAP_TYPE_READBACK);
                    const D3D12_RESOURCE_DESC rb = bufferDesc(total);
                    if (FAILED(g.device->CreateCommittedResource(&back, D3D12_HEAP_FLAG_NONE, &rb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer))))
                        return 0;
                    r = static_cast<gfx::ReadbackId>(g.readbacks.size());
                    g.readbacks.emplace_back();
                    g.readbacks[r].buffer = buffer;
                    g.readbacks[r].size = total;
                }
                State::DeviceReadback &rb = g.readbacks[r];
                rb.fp = fp;
                rb.format = td.Format == DXGI_FORMAT_R32_FLOAT        ? gfx::Format::R32Float
                            : td.Format == DXGI_FORMAT_B8G8R8A8_UNORM ? gfx::Format::BGRA8
                            : td.Format == DXGI_FORMAT_R8G8_UNORM     ? gfx::Format::RG8
                            : td.Format == DXGI_FORMAT_R8_UNORM       ? gfx::Format::R8
                                                                      : gfx::Format::RGBA8;
                rb.slot = s;
                rb.live = true;
                const D3D12_RESOURCE_STATES was = t.state;
                stateOf(t, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
                dst.pResource = rb.buffer;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint = fp;
                src.pResource = t.colour;
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                stateOf(t, was);
                return r;
            }
            bool readback(gfx::ReadbackId id, gfx::Readback &out) override
            {
                if (!id || id >= g.readbacks.size() || !g.readbacks[id].live)
                    return false;
                State::DeviceReadback &rb = g.readbacks[id];
                if (!rb.mapped)
                {
                    const D3D12_RANGE all{0, static_cast<SIZE_T>(rb.size)};
                    if (FAILED(rb.buffer->Map(0, &all, &rb.mapped)))
                    {
                        rb.mapped = nullptr;
                        return false;
                    }
                }
                out.data = static_cast<const uint8_t *>(rb.mapped) + rb.fp.Offset;
                out.width = static_cast<uint32_t>(rb.fp.Footprint.Width);
                out.height = rb.fp.Footprint.Height;
                out.rowBytes = rb.fp.Footprint.RowPitch;
                out.format = rb.format;
                return true;
            }
            void releaseReadback(gfx::ReadbackId id) override
            {
                if (!id || id >= g.readbacks.size() || !g.readbacks[id].live)
                    return;
                State::DeviceReadback &rb = g.readbacks[id];
                if (rb.mapped)
                {
                    const D3D12_RANGE none{0, 0};
                    rb.buffer->Unmap(0, &none);
                    rb.mapped = nullptr;
                }
                rb.live = false;
                g.readbackReleased[rb.slot].push_back(id);
            }

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

        // The device's own report lines (gfx::Report): a saved frame's rings; every profile window (300 frames) its uploads,
        // its targets' and textures' video memory, its counters - the frame builder prints its own lines around them.
        void D3d12Device::report(gfx::Report what)
        {
            if (what == gfx::Report::Rings)
            {
                // (TODO 99 check: the frame's rings - used of their room - and the views rewritten under a frame's use so far)
                std::fprintf(stderr, "[native render] rings: constants %llu of %llu, instances %u of %u, vertices %llu of %llu KB; slot %u; views rewritten %llu\n",
                             static_cast<unsigned long long>((g.ringConstantsAt - g.slot * kConstantRing) / kConstantStride),
                             static_cast<unsigned long long>(kConstantRing / kConstantStride), g.ringInstanceAt - g.slot * kInstanceRing, kInstanceRing,
                             static_cast<unsigned long long>((g.dynamicUsed - g.slot * kDynamicRing) >> 10),
                             static_cast<unsigned long long>(kDynamicRing >> 10), g.slot, static_cast<unsigned long long>(g_srvRewrites));
                return;
            }
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
                size_t targets = 0;
                for (size_t id = 1; id < g.devTargets.size(); ++id)
                {
                    const State::DeviceTarget &tg = g.devTargets[id];
                    if (!tg.colour || tg.picture)
                        continue;
                    ++targets;
                    UINT64 b = 0;
                    for (ID3D12Resource *r : {tg.colour, tg.depth, tg.destCopy})
                        if (r)
                        {
                            const D3D12_RESOURCE_DESC rd = r->GetDesc();
                            b += g.device->GetResourceAllocationInfo(0, 1, &rd).SizeInBytes;
                        }
                    targetBytes += b;
                    bigTargets += b > (16ull << 20) ? 1u : 0u;
                }
                std::fprintf(stderr, "[native render] profile: targets %zu (%u over 16 MB), %llu MB; texture pool %llu MB\n", targets, bigTargets,
                             static_cast<unsigned long long>(targetBytes >> 20), static_cast<unsigned long long>(pool::heaps.size() * (pool::kHeap >> 20)));
                const double pf = 300.0; // (the profile window's frames)
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
#if defined(__linux__)
            std::fprintf(stderr, "[native render] profile: texture pool %zu heaps, %llu MB used, %llu placed, %llu not (committed); staging ring %llu uses, %llu overflows; %llu pictures not read back (no readback free)\n",
                         pool::heaps.size(), static_cast<unsigned long long>(pool::usedBytes() >> 20), static_cast<unsigned long long>(pool::made),
                         static_cast<unsigned long long>(pool::fallbacks), static_cast<unsigned long long>(g_texRingUses),
                         static_cast<unsigned long long>(g_texRingOverflows), static_cast<unsigned long long>(g_handoverSkips));
#else
            std::fprintf(stderr, "[native render] profile: texture pool %zu heaps, %llu MB used, %llu placed, %llu not (committed); staging ring %llu uses, %llu overflows\n",
                         pool::heaps.size(), static_cast<unsigned long long>(pool::usedBytes() >> 20), static_cast<unsigned long long>(pool::made),
                         static_cast<unsigned long long>(pool::fallbacks), static_cast<unsigned long long>(g_texRingUses),
                         static_cast<unsigned long long>(g_texRingOverflows));
#endif
            pool::made = pool::fallbacks = g_texRingUses = g_texRingOverflows = 0;
#if defined(__linux__)
            g_handoverSkips = 0;
#endif
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

    }

    // The device's video memory in use (or its budget), from its adapter.
    void preUploadImpl(const std::shared_ptr<const Texture> &texture) { frame::preUpload(texture); }

#if defined(_WIN32)
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
#else
    uint64_t videoMemory(bool) { return 0; } // (no DXGI budget through vkd3d: the profile line shows 0)

    // The graphics card as Vulkan tells it (nr_vkd3d.h), once the device is made (0: not known yet).
    namespace adapter
    {
        std::mutex g_mutex;
        bool g_known = false;
        uint32_t g_vendor = 0;
        uint64_t g_memoryMB = 0;
        void look()
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            std::string name;
            if (!g_known && nr::vk::describe(g.device, g_vendor, g_memoryMB, name))
            {
                g_known = true;
                std::fprintf(stderr, "[native render] Vulkan through vkd3d: %s (vendor %04x, %llu MB of video memory)\n", name.c_str(),
                             static_cast<unsigned>(g_vendor), static_cast<unsigned long long>(g_memoryMB));
            }
        }
    }

    uint32_t adapterVendor()
    {
        adapter::look();
        return adapter::g_vendor;
    }

    uint64_t adapterMemoryMB()
    {
        if (const char *e = std::getenv("TF_NATIVE_VRAM_MB"); e && *e)
            return std::strtoull(e, nullptr, 10);
        adapter::look();
        return adapter::g_memoryMB;
    }
#endif

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
        static const uint64_t budget = []() -> uint64_t { // (one return type where uint64_t is unsigned long)
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

#if defined(__linux__)
    void setWindowSize(int width, int height)
    {
        g_windowWidth.store(width, std::memory_order_relaxed);
        g_windowLines.store(height, std::memory_order_relaxed);
    }
#else
    void setWindowLines(int lines)
    {
        g_windowLines.store(lines, std::memory_order_relaxed);
    }
#endif

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

#if defined(__linux__)
    bool latestPicture(const uint8_t *&pixels, uint32_t &rowPitch, int &index, uint64_t &serial, d3d::SharedPicture &meta)
    {
        std::lock_guard<std::mutex> lock(g_published.mutex);
        // (The oldest published picture the window has not taken - not the newest: one that arrived while the
        // window was presenting waits its turn instead of being replaced, and each is shown exactly once.)
        int oldest = -1;
        for (int i = 0; i < State::kHandovers; ++i)
        {
            const State::Handover &ho = g.handovers[i];
            if (ho.taken || ho.serial <= serial || !ho.held.load(std::memory_order_relaxed))
                continue;
            if (oldest < 0 || ho.serial < g.handovers[oldest].serial)
                oldest = i;
        }
        if (oldest < 0)
            return false;
        State::Handover &ho = g.handovers[oldest];
        pixels = static_cast<const uint8_t *>(ho.mapped) + ho.fp.Offset;
        rowPitch = ho.fp.Footprint.RowPitch;
        index = oldest;
        meta = ho.meta;
        serial = ho.serial;
        ho.taken = true; // (pictureShown's ack is coming: the readback is not freed or drawn into under it)
        return true;
    }

    void pictureShown(int index)
    {
        if (index < 0 || index >= State::kHandovers)
            return;
        g.handovers[index].held.store(false, std::memory_order_release);
    }
#else
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
#endif

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

}

namespace nr::d3d
{
    // The backend's frame (nr_types.h): the device up and not lost, then the frame builder's render (frame::render).
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
        frame::render(input, shotPath);
    }

    // --bake-shaders <file> (build.ps1, after linking): every shader this file compiles - the same sources, entry
    // points, targets, defines and flags as at run time, so the same keys - written as the built-in table
    // (nr_shaders_baked.inc, read by cache::loadBaked), and only when it differs: an unchanged table rebuilds nothing.
    // No device is needed (D3DCompile only). A shader added later and not listed here is compiled at run time as before.
    // Returns 0: the table is as it was, 2: written anew, 1: a shader failed.
    int bakeShaders(const char *path)
    {
        cache::g_baking = true;
        bool ok = true;
        const auto take = [&ok](ID3DBlob *code) {
            ok = ok && code != nullptr;
            release(code);
        };
        // The main shader: its stages, the depth-test debug pair (TF_NATIVE_ZWALT) and every pixel-shader variant
        // (psv::compileVariant: bit 0 set, the six PSV_ switches; equal define sets give the same key, kept once).
        for (const char *entry : {"vs"})
            take(compile(entry, "vs_5_1"));
        for (const char *entry : {"ps", "psTiny", "psInterp"})
            take(compile(entry, "ps_5_1"));
        take(compile("gsClip", "gs_5_1"));
        for (uint32_t v = 1u; v < 256u; v += 2u)
            take(psv::compileVariant(v));
        // The grass's compute shaders, with the double-precision divisions and without (grassgpu::init picks by the GPU).
        {
            const D3D_SHADER_MACRO exact[] = {{"EXACT_DIV", "1"}, {nullptr, nullptr}};
            for (const char *entry : {"CsCount", "CsScan", "CsWrite"})
                for (const D3D_SHADER_MACRO *defines : {static_cast<const D3D_SHADER_MACRO *>(exact), static_cast<const D3D_SHADER_MACRO *>(nullptr)})
                    take(compile(entry, "cs_5_0", grassgpu::kCs, sizeof(grassgpu::kCs) - 1, defines, D3DCOMPILE_IEEE_STRICTNESS));
        }
        // FXAA, SMAA 1x (its source put together as at start), the probe and the clouds.
        take(compile("aaVs", "vs_5_1", kAaShader, sizeof(kAaShader) - 1));
        take(compile("aaPs", "ps_5_1", kAaShader, sizeof(kAaShader) - 1));
        // (TODO 108) The modern bloom's and the tone mapping's (added after this list was first written).
        take(compile("bloomVs", "vs_5_1", kBloomShader, sizeof(kBloomShader) - 1));
        take(compile("bloomDownPs", "ps_5_1", kBloomShader, sizeof(kBloomShader) - 1));
        take(compile("bloomUpPs", "ps_5_1", kBloomShader, sizeof(kBloomShader) - 1));
        take(compile("toneVs", "vs_5_1", kToneShader, sizeof(kToneShader) - 1));
        take(compile("tonePs", "ps_5_1", kToneShader, sizeof(kToneShader) - 1));
        {
            std::string src(kSmaaPrelude, sizeof(kSmaaPrelude) - 1);
            for (const char *piece : kSmaaSource)
                src += piece;
            src.append(kSmaaEntries, sizeof(kSmaaEntries) - 1);
            const char *const names[6] = {"smaaEdgeVs", "smaaEdgePs", "smaaWeightVs", "smaaWeightPs", "smaaBlendVs", "smaaBlendPs"};
            for (int k = 0; k < 6; ++k)
                take(compile(names[k], (k & 1) ? "ps_5_1" : "vs_5_1", src.c_str(), src.size()));
        }
        take(compile("probeVs", "vs_5_1", kProbeShader, sizeof(kProbeShader) - 1));
        take(compile("probePs", "ps_5_1", kProbeShader, sizeof(kProbeShader) - 1));
        take(compile("cloudVs", "vs_5_1", kCloudShader, sizeof(kCloudShader) - 1));
        take(compile("cloudPs", "ps_5_1", kCloudShader, sizeof(kCloudShader) - 1));
        if (!ok)
        {
            std::fprintf(stderr, "[native render] --bake-shaders: a shader failed to compile (see above); the table is left as it was\n");
            return 1;
        }

        std::map<uint64_t, std::vector<uint8_t>> table(cache::g_bakedOut.begin(), cache::g_bakedOut.end());
        std::string text = "// Generated by TransformersRecomp --bake-shaders (build.ps1): the native renderer's shaders, compiled\n"
                           "// (nr_d3d.cpp, cache::loadBaked). Not kept in the repository; do not edit.\n";
        size_t index = 0, bytes = 0;
        char buf[64];
        for (const auto &[key, code] : table)
        {
            std::snprintf(buf, sizeof(buf), "static const unsigned char kBaked%zu[] = {", index++);
            text += buf;
            for (size_t i = 0; i < code.size(); ++i)
            {
                std::snprintf(buf, sizeof(buf), "%s%u,", (i % 40u) == 0u ? "\n" : "", static_cast<unsigned>(code[i]));
                text += buf;
            }
            text += "};\n";
            bytes += code.size();
        }
        text += "static const BakedShader kBakedShaders[] = {\n";
        index = 0;
        for (const auto &[key, code] : table)
        {
            std::snprintf(buf, sizeof(buf), "    {0x%016llxull, kBaked%zu, sizeof(kBaked%zu)},\n", static_cast<unsigned long long>(key), index, index);
            text += buf;
            ++index;
        }
        text += "};\n";

        std::string old;
        if (FILE *f = std::fopen(path, "rb"))
        {
            char chunk[65536];
            for (size_t n; (n = std::fread(chunk, 1, sizeof(chunk), f)) > 0;)
                old.append(chunk, n);
            std::fclose(f);
        }
        if (old == text)
        {
            std::printf("shaders: %zu built in, unchanged\n", table.size());
            return 0;
        }
        FILE *f = std::fopen(path, "wb");
        if (!f || std::fwrite(text.data(), 1, text.size(), f) != text.size())
        {
            if (f)
                std::fclose(f);
            std::fprintf(stderr, "[native render] --bake-shaders: cannot write %s\n", path);
            return 1;
        }
        std::fclose(f);
        std::printf("shaders: %zu built in (%zu KB), written to %s\n", table.size(), bytes / 1024u, path);
        return 2;
    }
}

// (nr_types.h's preUpload: the capture's textures made ahead, to this file's queue.)
namespace nr
{
    void preUpload(const std::shared_ptr<const Texture> &texture) { backend().preUpload(texture); }
    bool rendererStarting() { return backend().startingNow(); }
    int bakeShaders(const char *path) { return d3d::bakeShaders(path); } // (the build's step: Direct3D 12's shaders)
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
#if defined(__linux__)
            bool latestPicture(const uint8_t *&pixels, uint32_t &rowPitch, int &index, uint64_t &serial, d3d::SharedPicture &meta) override
            {
                return d3d::latestPicture(pixels, rowPitch, index, serial, meta);
            }
            void pictureShown(int index) override { d3d::pictureShown(index); }
#else
            bool latestPicture(std::vector<uint32_t> &pixels, uint32_t &width, uint32_t &height, uint64_t &serial, bool &has3D) override
            {
                return d3d::latestPicture(pixels, width, height, serial, has3D);
            }
#endif
            void setSharedPictures(bool on) override { d3d::setSharedPictures(on); }
            bool latestShared(d3d::SharedPicture &out, uint64_t &serial) override { return d3d::latestShared(out, serial); }
            void sharedHeld(int index) override { d3d::sharedHeld(index); }
            bool waitPicture(uint64_t serial, int64_t untilNs, int64_t &lastNs) override { return d3d::waitPicture(serial, untilNs, lastNs); }
            int pacingMode() override { return d3d::pacingMode(); }
#if defined(__linux__)
            void setWindowSize(int width, int height) override { d3d::setWindowSize(width, height); }
#else
            void setWindowLines(int lines) override { d3d::setWindowLines(lines); }
#endif
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

    // (The backend in use is chosen in nr_backend.cpp: TF_GFX_API.)
    Backend &d3d12Backend()
    {
        static D3d12Backend b;
        return b;
    }
}

namespace nr::gfx
{
    // (P3 S3) The Direct3D 12 device (the one in use is chosen in nr_backend.cpp: TF_GFX_API).
    Device &deviceD3d12() { return nr::d3d::d3d12Device(); }
}
