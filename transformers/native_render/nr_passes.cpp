// A draw as the passes the GPU draws (README.md): what its VU1 routines compute, as modes and
// parameters for the HLSL (nr_d3d.cpp), each from the routine's own data block (docs/RENDERER.md;
// src/vu1_native.cpp has every routine bit for bit). Plain C++: no GPU here.
//
// Colour modes (GS units, 128 = 1.0): 0 the vertex's; 1 constant (0x13b0: q0); 2 scaled (0x2150:
// times q0, at most 255); 3 scaled and offset (0x1c20: times q0 plus q1, 0 to 255); 4 the
// environment pass's (0x21b0: q4, the alpha bit 7 of the vertex's); 5 one light (0x1ac0/0x1c20 of
// the lit images: q2 * q1 * (max(N.L, 0) + L.w), L = q0; alpha q2.w * 128); 6 two lights and a
// highlight (0x21f0, the lights and eye taken into the mesh's space here, as the routine does);
// 7 the lit metal's reflection (0x2800, context 1); 8 its lit base (context 2); 9 an instance's
// (0x1ac0 of bec5: the vertex's times the instance's tint).
// ST modes: 0 the mesh's; 1 the position through q0-q3 (0x21b0); 2 a 2x3 matrix (0x1ac0: scrolling);
// 3 the sphere map of the view reflected about the normal (0x2800); ... 9 S = N.V, T the mesh's (0x1e70 of 6b20).
// Alpha modes: 0 the colour's; 1 the distance from a plane (0x3660), 0 to 128.
#include "nr_types.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>

namespace nr
{
    namespace
    {
        constexpr int kSmoothTrails = 2; // ("smooth" and unset: the look chosen for the default - TODO 305 (7))
        std::atomic<int> g_motionTrails{-1}; // (-1: not read yet - TF_MOTION_TRAILS at the first draw)
    }
    void setMotionTrails(int mode)
    {
        g_motionTrails.store(mode < 0 || mode > 3 ? kSmoothTrails : mode, std::memory_order_relaxed); // (-1: "smooth")
    }
    int motionTrails()
    {
        int m = g_motionTrails.load(std::memory_order_relaxed);
        if (m < 0)
        {
            const char *v = std::getenv("TF_MOTION_TRAILS");
            const std::string s = v ? v : "";
            m = s == "original" ? 0 : s == "soft" ? 1 : s == "ghost" ? 2 : s == "off" ? 3 : kSmoothTrails;
            g_motionTrails.store(m, std::memory_order_relaxed);
        }
        return m;
    }

    namespace
    {
        void copy(float *dst, const Qword &q) { std::memcpy(dst, q.v, 16u); }

        // A 3x3 inverse of the screen transform's linear part (its rows: the coefficients of x, y,
        // z), applied to a vector; `point`: the offset (row 3) taken off first.
        void toMesh(const float *m, const float *v, bool point, float *out)
        {
            const float a[3] = {m[0], m[1], m[2]}, b[3] = {m[4], m[5], m[6]}, c[3] = {m[8], m[9], m[10]};
            float d[3] = {v[0], v[1], v[2]};
            if (point)
                for (int i = 0; i < 3; ++i)
                    d[i] -= m[12 + i];
            // screen = a * x + b * y + c * z: the matrix with columns a, b, c.
            const float det = a[0] * (b[1] * c[2] - b[2] * c[1]) - b[0] * (a[1] * c[2] - a[2] * c[1]) + c[0] * (a[1] * b[2] - a[2] * b[1]);
            if (std::fabs(det) < 1e-30f)
            {
                out[0] = out[1] = out[2] = 0.0f;
                return;
            }
            const float r = 1.0f / det;
            // Cramer: x = det(d, b, c) / det, y = det(a, d, c) / det, z = det(a, b, d) / det.
            auto det3 = [](const float *p, const float *q, const float *s) {
                return p[0] * (q[1] * s[2] - q[2] * s[1]) - q[0] * (p[1] * s[2] - p[2] * s[1]) + s[0] * (p[1] * q[2] - p[2] * q[1]);
            };
            out[0] = det3(d, b, c) * r;
            out[1] = det3(a, d, c) * r;
            out[2] = det3(a, b, d) * r;
        }

        void normalize(float *v)
        {
            const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (l > 0.0f)
                v[0] /= l, v[1] /= l, v[2] /= l;
        }

        Pass basePass(const Draw &d)
        {
            Pass p;
            std::memcpy(p.matrix, d.matrix, sizeof(p.matrix));
            std::memcpy(p.matrixB, d.hasB ? d.matrixB : d.matrix, sizeof(p.matrixB)); // (N9 stage 3: the frame before's pose)
            p.turned = d.turned;
            p.sceneMotion = d.sceneMotion;
            p.texture = d.texture;
            p.fogPalette = d.fogPalette;
            p.topPalette = d.topPalette;
            p.tex0 = d.tex0;
            p.alpha = d.alpha;
            p.test = d.test;
            p.zbuf = d.zbuf;
            p.prim = d.mesh ? d.mesh->prim : 0u;
            p.fbmsk = d.fbmsk;
            p.clamp = d.clamp;
            p.tex1 = d.tex1;
            // The core's clipping (src/vu1_native.cpp, docs/RENDERER.md 0x0480): with q21.x above 0 (an
            // integer) 0x1410 hands the strip to 0x0480, which clips each triangle - Sutherland-Hodgman
            // against -z, +z, -x, +x, -y, +y of the position through its q0-q3 (CLIP against w). The GPU's
            // own clipping alone kept everything from w ~ 0.03 on, where the PS2's near plane is w ~ 0.5:
            // a mesh whose corners straddle the camera came out as a screen-filling smear (Antarctica's
            // blue additive light cone at 9900, the whole picture about 20 bluer). TF_NATIVE_CLIP=0: off.
            static const bool clipOff = std::getenv("TF_NATIVE_CLIP") && *std::getenv("TF_NATIVE_CLIP") == '0';
            int32_t q21x = 0;
            std::memcpy(&q21x, &d.shader.q21[0], 4);
            if (!clipOff && d.mesh && !d.mesh->immediate && !d.mesh->sprites && q21x > 0)
            {
                bool core = false;
                for (uint32_t k = 0; k < d.shader.count; ++k)
                {
                    if (d.shader.routine[k] == Routine::Instances)
                        break; // (each instance its own matrix: not the clipping matrix's)
                    core = core || d.shader.address[k] == 0x1410u || d.shader.address[k] == 0x2800u || d.shader.address[k] == 0x2EC0u;
                    if (core && d.shader.address[k] == 0x0480u && d.shader.first[k] + 4u <= d.shader.data.size())
                    {
                        for (int r = 0; r < 4; ++r)
                            std::memcpy(p.clip[r], d.shader.data[d.shader.first[k] + r].v, 16u);
                        if (d.hasClipB)
                            std::memcpy(p.clipB, d.clipB, sizeof(p.clipB));
                        else
                            std::memcpy(p.clipB, p.clip, sizeof(p.clipB));
                        p.clips = true;
                        break;
                    }
                }
            }
            return p;
        }
    }

    namespace
    {
    void buildPassesOf(const Draw &d, std::vector<Pass> &out)
    {
        const ShaderState &sh = d.shader;
        // The lit metal: reflection (context 1), then the lit base (context 2), the GS's order.
        if (const Qword *lighting = sh.blockOf(Routine::LitMeshPrepare))
            if (const Qword *output = sh.blockOf(Routine::LitMeshDraw))
            {
                Pass reflection = basePass(d);
                reflection.mode[0] = 7.0f;
                // (TODO 305 (4)) The sphere map's coordinates as the GS takes them: 0x2800 sends S, T = (0.5 + 0.5 E.xy /
                // (Ez + 1)) * Q and Q' = (Ez + 1) * Q, the GS dividing S and T by Q' at each pixel - shader mode 9. Divided
                // at the vertex (mode 3, TF_NATIVE_REFLECTQ=0: as before), a vertex whose reflection points nearly away
                // from the eye (Ez ~ -1) gave its triangle coordinates far out on the map: single triangles of the
                // reflective characters flashed bright as they moved.
                static const bool vertexDivide = std::getenv("TF_NATIVE_REFLECTQ") && *std::getenv("TF_NATIVE_REFLECTQ") == '0';
                reflection.mode[1] = vertexDivide ? 3.0f : 9.0f;
                for (int q = 0; q < 11; ++q)
                    copy(reflection.p[q], lighting[q]);
                copy(reflection.p[11], output[16]);
                Pass base = reflection;
                base.mode[0] = 8.0f;
                base.mode[1] = 0.0f;
                base.texture = d.texture2;
                base.topPalette = nullptr; // (context 1's)
                base.tex0 = d.tex02;
                base.tex1 = d.tex12;
                base.alpha = d.alpha2;
                base.test = d.test2;
                base.prim = d.mesh ? d.mesh->prim2 : 0u;
                out.push_back(reflection);
                out.push_back(base);
                return;
            }
        // The water (0x1df0 then 0x2cc0, image e147: the rivers): the height moved by the waves, two
        // copies of the strip as the routine sends them - context 1 the ST moved by the current (q4.xy)
        // with the vertex's colour, context 2 the sphere map of the reflected view with the colour
        // times q21 (src/vu1_native.cpp drawWater). TF_NATIVE_WATER=0: drawn as a plain mesh.
        if (const Qword *waves = sh.blockOf(Routine::WaterPrepare))
            if (sh.blockOf(Routine::WaterDraw))
            {
                static const bool off = std::getenv("TF_NATIVE_WATER") && *std::getenv("TF_NATIVE_WATER") == '0';
                if (!off)
                {
                    Pass base = basePass(d);
                    base.mode[0] = 14.0f;
                    base.mode[1] = 4.0f;
                    base.mode[2] = 2.0f;
                    for (int q = 0; q < 5; ++q)
                        copy(base.p[q], waves[q]);
                    copy(base.p[5], waves[21]);
                    Pass reflection = base;
                    reflection.mode[0] = 15.0f;
                    reflection.mode[1] = 5.0f;
                    reflection.texture = d.texture2;
                    reflection.topPalette = nullptr;
                    reflection.tex0 = d.tex02;
                    reflection.tex1 = d.tex12;
                    reflection.alpha = d.alpha2;
                    reflection.test = d.test2;
                    reflection.clamp = d.clamp2;
                    reflection.prim = d.mesh ? d.mesh->prim2 : 0u;
                    out.push_back(base);
                    if (reflection.prim)
                        out.push_back(reflection);
                    return;
                }
            }
        // Cybertron's bubble (image 6b20's 0x2030 then 0x25e0, before 0x1410; batch type 4): the
        // vertex moved by two waves (q4: amplitude, frequency x2; q5, q6: direction and phase - the
        // water's cubic), its normal by the same displacement; ST the sphere map of the view (from the
        // eye q8) reflected about it; colour q10, alpha q11.w * max(N.L, 0) + q11.z with L = q9 (the
        // unmoved normal) - 0 in most frames, so it is not seen (it drew as a white chrome dome, the
        // colour and alpha 1, ALPHA 0x48 adding it in). The blocks are adjacent: 0x2030's q9-q11 are
        // 0x25e0's q0-q2.
        // (TODO 305) The tractor beam's ball (image 6b20: 0x1e70 then 0x25e0, before 0x1410): ST mode 10, S the normal dotted
        // with the direction to the eye (q0 of 0x1e70, in the mesh's space) plus q0.w, T the mesh's. 0x25e0 then writes
        // its q1 as the colour and as the alpha q2.w * max(R . q0, 0) + q2.z, R the register 0x2030 leaves the moved
        // normal in - here 0x1e70's q1 as integers (FTOI0), read as floats: denormals, so 0 - each value FTOI0 to the
        // packet's low eight bits (tools/vu1_trace.py on captured batches; the packets' alpha is 0: with ALPHA 0x48,
        // Cs * As + Cd, the ball adds nothing - on the PS2 it is not seen).
        if (const Qword *ramp = sh.blockOf(Routine::ViewRamp))
        {
            Pass p = basePass(d);
            p.mode[1] = 10.0f; // (ST mode 10: 9 is the lit metal's per-pixel Q, TODO 305 (4))
            copy(p.p[8], ramp[0]);
            if (const Qword *shade = sh.blockOf(Routine::MembraneShade))
            {
                auto ftoi0 = [](float v) -> int32_t {
                    if (!(v == v))
                        return 0;
                    return v >= 2147483520.0f ? INT32_MAX : v <= -2147483648.0f ? INT32_MIN : static_cast<int32_t>(v);
                };
                auto vuFloat = [](int32_t bits) { // (an integer register read by an FMAC: denormals are 0)
                    float f = 0.0f;
                    std::memcpy(&f, &bits, 4u);
                    if (std::isinf(f) || std::isnan(f))
                        return std::copysign(3.402823466e38f, f); // (exponent 255: +-FLT_MAX)
                    return std::fpclassify(f) == FP_SUBNORMAL ? 0.0f : f;
                };
                float rl = 0.0f;
                for (int k = 0; k < 3; ++k)
                    rl += vuFloat(ftoi0(ramp[1].v[k])) * shade[0].v[k];
                const int32_t alpha = ftoi0(shade[2].v[3] * std::max(rl, 0.0f) + shade[2].v[2]);
                p.mode[0] = 1.0f;
                for (int k = 0; k < 3; ++k)
                    p.p[0][k] = static_cast<float>(ftoi0(shade[1].v[k]) & 0xFF);
                p.p[0][3] = static_cast<float>(alpha & 0xFF);
            }
            out.push_back(p);
            return;
        }
        if (const Qword *wave = sh.blockOf(Routine::MembraneWave))
            if (sh.blockOf(Routine::MembraneShade))
            {
                Pass p = basePass(d);
                p.mode[0] = 17.0f;
                p.mode[1] = 6.0f;
                p.mode[2] = 3.0f;
                for (int q = 0; q < 12; ++q)
                    copy(p.p[q], wave[q]);
                out.push_back(p);
                return;
            }
        // 0x1e70 (image 83f1; the Starship's walls): ST a projection - the vertex moved along its normal
        // by q4.w through q0-q3, 0.5 + 0.5 * (x, y) / z - of the light pattern the frame drew into a
        // 256 x 256 target; colour q5 * (max(N.L, 0) + L.w), L = q6, at most 255, alpha q5.w.
        if (const Qword *light = sh.blockOf(Routine::ProjectedLight))
        {
            Pass p = basePass(d);
            p.mode[0] = 18.0f;
            p.mode[1] = 7.0f;
            for (int q = 0; q < 7; ++q)
                copy(p.p[q], light[q]);
            out.push_back(p);
            return;
        }
        // 0x2920 (image e147): ST the view (from the eye q4) reflected about the normal, through the
        // screen transform's axes, 0.5 + 0.5 * E.xy / |E|; where the vertex faces the eye, colour
        // q5 * (max(N.L * q7.y, 0) + q7.x) (L = q6) and alpha q5.w * ((1 - N.V)^2 * q7.z + q7.w), each at
        // most 255; facing away, 16 for both.
        if (const Qword *rim = sh.blockOf(Routine::RimReflection))
        {
            Pass p = basePass(d);
            p.mode[0] = 19.0f;
            p.mode[1] = 8.0f;
            for (int q = 0; q < 8; ++q)
                copy(p.p[q], rim[q]);
            out.push_back(p);
            return;
        }
        // 0x2220 (image 83f1, then 0x1410; batch type 4): the dash's after-image (TODO 69) - each vertex
        // whose normal faces q0 (q0 . N above 0; the routine's min(max(q0 . N, 0) * 1e8, 1)) moved by q0,
        // its colour q2 there and q1 elsewhere (the vertex's own colour replaced: 128 grey, alpha 0 at the
        // moved end), ST the mesh's; drawn with the TEX0_1 / ALPHA_1 / TEST_1 the list writes for it
        // (the character's texture, the usual blend: nr_capture.cpp expandFrame). TF_NATIVE_SMEAR=0:
        // drawn as a plain mesh.
        //
        // (TODO 305 (7)) Its look, TF_MOTION_TRAILS (F1 Graphics, "Motion trails"). The routine moves each vertex facing
        // q0 the whole way and the rest not at all (taShader_MotionBlur: q0 the creature's velocity x -1/15 s, clamped),
        // so a triangle with corners on both sides stretches into a long hard sliver - the flyers' "two of them".
        // Original (0): as the PS2. Soft push (1): each vertex moved by how squarely it faces q0 (its cosine, eased), so
        // the shape tapers instead of tearing. Ghost blur (2): the mesh drawn kGhosts times along q0, each a step further
        // and fainter - the trail as a soft blur of the body. Off (3): no trail. p3: x the mode (0 / 1 / 2), y a ghost's
        // place along q0 (0..1), z its share of the alpha.
        if (const Qword *smear = sh.blockOf(Routine::Smear))
        {
            static const bool off = std::getenv("TF_NATIVE_SMEAR") && *std::getenv("TF_NATIVE_SMEAR") == '0';
            if (!off)
            {
                const int look = motionTrails();
                if (look == 3)
                    return;
                Pass p = basePass(d);
                p.mode[0] = 20.0f;
                p.mode[2] = 4.0f;
                for (int q = 0; q < 3; ++q)
                    copy(p.p[q], smear[q]);
                p.p[3][2] = 1.0f;
                if (look == 2)
                {
                    // (copies close enough together to read as one blur - 6 left a staircase of plates on the flyers,
                    // whose trail is long; each copy's result alpha times 3/N x (1 - t)^2, applied in the pixel shader -
                    // a share on the vertex alpha changed nothing on the flyers (their alpha comes from the texture) - the
                    // faintest at the far end. TF_MOTION_TRAIL_GHOSTS: the number, 2..48.)
                    static const int kGhosts = [] {
                        const char *v = std::getenv("TF_MOTION_TRAIL_GHOSTS");
                        return v && *v ? std::clamp(std::atoi(v), 2, 48) : 24;
                    }();
                    // (each copy's texture blurred over this many texels - TF_MOTION_TRAIL_BLUR, 0 sharp)
                    static const float kBlur = [] {
                        const char *v = std::getenv("TF_MOTION_TRAIL_BLUR");
                        return v && *v ? std::clamp(static_cast<float>(std::atof(v)), 0.0f, 16.0f) : 3.0f;
                    }();
                    p.p[3][3] = kBlur;
                    // (the trail's length, a share of the game's q0 - the full length by default. TF_MOTION_TRAIL_LENGTH)
                    static const float kLength = [] {
                        const char *v = std::getenv("TF_MOTION_TRAIL_LENGTH");
                        return v && *v ? std::clamp(static_cast<float>(std::atof(v)), 0.05f, 1.0f) : 1.0f;
                    }();
                    p.p[3][0] = 2.0f;
                    for (int k = kGhosts; k >= 1; --k) // (farthest first: the nearer ones over them)
                    {
                        const float t = static_cast<float>(k) / static_cast<float>(kGhosts + 1);
                        p.p[3][1] = t * kLength;
                        p.p[3][2] = 3.0f * (1.0f - t) * (1.0f - t) / static_cast<float>(kGhosts); // (the faintest at the far end)
                        out.push_back(p);
                    }
                    return;
                }
                p.p[3][0] = look == 1 ? 1.0f : 0.0f;
                out.push_back(p);
                return;
            }
        }
        // (TODO 162) Cybertron's city (image a31d's 0x22c0): one face of each building (the vertex shader's mode.z 5,
        // the mesh's four corners a building - nr_capture.cpp cityBlocksOf). p1: the face (q10.w, an integer: 0 the
        // side along x, 1 along y, 2 the roof); p2: the eye in the buildings' space (q13); p3, p4: the colour's
        // factors for the side facing -x / -y (and the roof) and +x / +y (q10, q11); p5: the texture's repeats per
        // unit of half size and height, and the base per unit of z (q12); p6: the sides' foot colour (q16, integers).
        if (const Qword *city = sh.blockOf(Routine::CityBlocks); city && d.mesh && !d.mesh->sprites && d.mesh->vertices.size() % 4u == 0u &&
                                                                  static_cast<size_t>(city - sh.data.data()) + 17u <= sh.data.size())
        {
            Pass p = basePass(d);
            p.mode[2] = 5.0f;
            int32_t face = 0;
            std::memcpy(&face, &city[10].v[3], 4u);
            p.p[1][0] = static_cast<float>(face);
            copy(p.p[2], city[13]);
            copy(p.p[3], city[10]);
            copy(p.p[4], city[11]);
            copy(p.p[5], city[12]);
            for (int l = 0; l < 4; ++l)
            {
                int32_t c = 0;
                std::memcpy(&c, &city[16].v[l], 4u);
                p.p[6][l] = static_cast<float>(c);
            }
            out.push_back(p);
            return;
        }
        // Instances: the mesh at each instance's place (its own field A matrix), tinted.
        if (const Qword *block = sh.blockOf(Routine::Instances))
        {
            static const bool off = std::getenv("TF_NATIVE_NOINSTANCES") != nullptr;
            if (off)
                return;
            const int32_t count = static_cast<int32_t>(sh.data.size()) - static_cast<int32_t>(block - sh.data.data());
            const int32_t instances = std::max(0, (count - 13) / 5);
            for (int32_t i = 0; i < instances; ++i)
            {
                Pass p = basePass(d);
                for (int r = 0; r < 4; ++r)
                    std::memcpy(p.matrix + 4 * r, block[14 + 5 * i + r].v, 16u);
                if (d.instB.size() >= 16u * static_cast<size_t>(i + 1))
                    std::memcpy(p.matrixB, d.instB.data() + 16 * i, sizeof(p.matrixB));
                else
                    std::memcpy(p.matrixB, p.matrix, sizeof(p.matrixB));
                p.mode[0] = 9.0f;
                copy(p.p[0], block[13 + 5 * i]);
                copy(p.p[1], block[10]); // the guard band (q10 * position + q11 * w, CLIP against its w)
                copy(p.p[2], block[11]);
                out.push_back(p);
            }
            return;
        }
        Pass p = basePass(d);
        for (uint32_t k = 0; k < sh.count; ++k)
        {
            const Qword *b = sh.data.data() + sh.first[k];
            switch (sh.routine[k])
            {
            case Routine::ConstantColour:
                p.mode[0] = 1.0f;
                copy(p.p[0], b[0]);
                break;
            case Routine::ScaleColours:
                p.mode[0] = 2.0f;
                copy(p.p[0], b[0]);
                break;
            case Routine::ScaleAndOffsetColours:
                p.mode[0] = 3.0f;
                copy(p.p[0], b[0]);
                copy(p.p[1], b[1]);
                break;
            case Routine::MapCoordinates:
                p.mode[0] = 4.0f;
                p.mode[1] = 1.0f;
                for (int q = 0; q < 4; ++q)
                    copy(p.p[12 + q], b[q]);
                copy(p.p[0], b[4]);
                break;
            case Routine::Projection:
            {
                // 0x1df0 (image 2011, docs/RENDERER.md; src/vu1_native.cpp projectVertices): a texture
                // projected onto the mesh - ST from the position through q0-q3 (the projector's box),
                // the colour q8, its alpha q8.w * 128 where the vertex's alpha has bit 7. The shadow
                // receivers: the player's shadow texture laid on the ground round him (with the mesh's
                // own UVs it landed wherever those fell inside the texture - a black silhouette on the
                // ground beside the player at Amazon's start). TF_NATIVE_PROJECTION=0: as before.
                static const bool off = std::getenv("TF_NATIVE_PROJECTION") && *std::getenv("TF_NATIVE_PROJECTION") == '0';
                if (off)
                    break;
                p.mode[0] = 13.0f;
                p.mode[1] = 1.0f;
                for (int q = 0; q < 4; ++q)
                    copy(p.p[12 + q], b[q]);
                copy(p.p[0], b[8]);
                break;
            }
            case Routine::TransformTexture:
                p.mode[1] = 2.0f;
                for (int q = 0; q < 3; ++q)
                    copy(p.p[12 + q], b[q]);
                break;
            case Routine::ElectricShock:
                // (TODO 290) taShader_ElectricShock (image 83f1's 0x1ac0): each vertex pushed out by q0.z along its
                // direction from q2 (the creature's centre), ST that direction's y, z plus q0.xy, the colour q1 as
                // integers (FTOI0). The game moves q0.xy and draws q1's brightness anew every frame of the effect.
                p.mode[0] = 21.0f;
                p.mode[2] = 6.0f;
                copy(p.p[0], b[1]);
                copy(p.p[1], b[0]);
                copy(p.p[2], b[2]);
                break;
            case Routine::Sprites:
                p.mode[3] = 1.0f;
                copy(p.p[18], b[10]);
                copy(p.p[19], b[11]);
                copy(p.p[20], b[14]);
                copy(p.p[21], b[15]);
                copy(p.p[22], b[18]);
                break;
            case Routine::FieldSprites:
                // 0x2cf0 (src/vu1_native.cpp drawFieldSprites): the corners the centre less q5 and plus
                // q8 times size * Q (each at most q0.zw), the texture whole (ST q1 and q4).
                p.mode[3] = 1.0f;
                copy(p.p[18], b[0]);
                copy(p.p[19], b[1]);
                copy(p.p[20], b[4]);
                copy(p.p[21], b[5]);
                copy(p.p[22], b[8]);
                break;
            case Routine::FadeByPlane:
                p.mode[2] = 1.0f;
                copy(p.p[16], b[0]);
                break;
            case Routine::LightNormals:
                p.mode[0] = 5.0f;
                for (int q = 0; q < 3; ++q)
                    copy(p.p[q], b[q]);
                break;
            case Routine::LightWithHighlight:
            {
                p.mode[0] = 6.0f;
                for (int q = 0; q < 7; ++q)
                    copy(p.p[q], b[q]);
                {
                    // (TODO 108 S2, a measuring aid: TF_NATIVE_LIGHTLOG=1 - at most every 2 s, a lit mesh's two lights (q5 = L1,
                    // q1 = L2), its eye (q0) and its matrix: where the game's light comes from, for the contact shadows.)
                    static const bool lightLog = std::getenv("TF_NATIVE_LIGHTLOG") && *std::getenv("TF_NATIVE_LIGHTLOG") == '1';
                    static int64_t lastNs = 0;
                    const int64_t nowNs = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                                   std::chrono::steady_clock::now().time_since_epoch())
                                                                   .count());
                    if (lightLog && nowNs - lastNs > 2000000000ll)
                    {
                        lastNs = nowNs;
                        const float *m = d.matrix;
                        std::fprintf(stderr,
                                     "[native render] light: L1 %.3f %.3f %.3f %.3f L2 %.3f %.3f %.3f %.3f eye %.1f %.1f %.1f | matrix %.3f %.3f %.3f %.3f / %.3f "
                                     "%.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.1f %.1f %.1f %.1f\n",
                                     b[5].v[0], b[5].v[1], b[5].v[2], b[5].v[3], b[1].v[0], b[1].v[1], b[1].v[2], b[1].v[3], b[0].v[0], b[0].v[1],
                                     b[0].v[2], m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
                    }
                }
                // The lights (q5 = L1, q1 = L2) and the eye (q0) in the mesh's space.
                toMesh(d.matrix, b[5].v, false, p.p[8]);
                normalize(p.p[8]);
                toMesh(d.matrix, b[1].v, false, p.p[9]);
                normalize(p.p[9]);
                toMesh(d.matrix, b[0].v, true, p.p[10]);
                // (N9 stage 3: the same in the frame before's pose - the pictures between frames blend them)
                {
                    const float *mb = d.hasB ? d.matrixB : d.matrix;
                    toMesh(mb, b[5].v, false, p.lightsB[0]);
                    normalize(p.lightsB[0]);
                    toMesh(mb, b[1].v, false, p.lightsB[1]);
                    normalize(p.lightsB[1]);
                    toMesh(mb, b[0].v, true, p.lightsB[2]);
                }
                break;
            }
            default:
                break;
            }
        }
        out.push_back(p);
    }
    }

    bool midPose(const float *b, const float *a, float *m)
    {
        if (std::memcmp(a, b, 64u) == 0)
        {
            std::memcpy(m, a, 64u);
            return true;
        }
        for (int i = 0; i < 16; ++i)
            m[i] = 0.5f * (a[i] + b[i]); // (the average: the origin's row always, the axes' where no turn is found)
        double B[3][4], A[3][4];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                B[r][c] = b[4 * r + c], A[r][c] = a[4 * r + c];
        // The turn between the poses as it acts on the model's axes: with B = R_b P and A = R_a P (P the camera's, the same
        // for both), Q = A pinv(B) = R_a R_b^-1 - P need not be known. pinv(B) = B^T (B B^T)^-1.
        double G[3][3];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
            {
                double v = 0.0;
                for (int c = 0; c < 4; ++c)
                    v += B[i][c] * B[j][c];
                G[i][j] = v;
            }
        const auto det3 = [](const double M[3][3]) {
            return M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
                   M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
        };
        const auto inv3 = [&](const double M[3][3], double R[3][3]) {
            const double d = det3(M);
            if (!(std::fabs(d) > 0.0))
                return false;
            R[0][0] = (M[1][1] * M[2][2] - M[1][2] * M[2][1]) / d;
            R[0][1] = (M[0][2] * M[2][1] - M[0][1] * M[2][2]) / d;
            R[0][2] = (M[0][1] * M[1][2] - M[0][2] * M[1][1]) / d;
            R[1][0] = (M[1][2] * M[2][0] - M[1][0] * M[2][2]) / d;
            R[1][1] = (M[0][0] * M[2][2] - M[0][2] * M[2][0]) / d;
            R[1][2] = (M[0][2] * M[1][0] - M[0][0] * M[1][2]) / d;
            R[2][0] = (M[1][0] * M[2][1] - M[1][1] * M[2][0]) / d;
            R[2][1] = (M[0][1] * M[2][0] - M[0][0] * M[2][1]) / d;
            R[2][2] = (M[0][0] * M[1][1] - M[0][1] * M[1][0]) / d;
            return true;
        };
        const double trace = G[0][0] + G[1][1] + G[2][2];
        double Gi[3][3];
        if (!(trace > 0.0) || std::fabs(det3(G)) < 1e-9 * trace * trace * trace || !inv3(G, Gi))
            return true; // (an axis scaled away, or none: the average)
        double Q[3][3];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
            {
                double v = 0.0;
                for (int k = 0; k < 3; ++k)
                {
                    double ab = 0.0; // (A B^T)[i][k]
                    for (int c = 0; c < 4; ++c)
                        ab += A[i][c] * B[k][c];
                    v += ab * Gi[k][j];
                }
                Q[i][j] = v;
            }
        if (!(det3(Q) > 0.0))
            return true; // (a mirror between the poses: no turn - the average)
        // Its rotation: the polar decomposition Q = U S, U by Newton's iteration U <- (U + U^-T) / 2.
        double U[3][3];
        std::memcpy(U, Q, sizeof(U));
        for (int it = 0; it < 12; ++it)
        {
            double Ui[3][3];
            if (!inv3(U, Ui))
                return true;
            double change = 0.0;
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                {
                    const double v = 0.5 * (U[i][j] + Ui[j][i]);
                    change = std::max(change, std::fabs(v - U[i][j]));
                    U[i][j] = v;
                }
            if (change < 1e-12)
                break;
        }
        const double cosAngle = std::clamp(0.5 * (U[0][0] + U[1][1] + U[2][2] - 1.0), -1.0, 1.0);
        if (cosAngle < 0.5)
            return false; // (over 60 degrees in one frame: no pose of the same thing)
        // Half the turn: the quaternion's (w >= cos 30 here), its angle halved.
        const double w = 0.5 * std::sqrt(std::max(0.0, 1.0 + U[0][0] + U[1][1] + U[2][2]));
        double qx = (U[2][1] - U[1][2]) / (4.0 * w), qy = (U[0][2] - U[2][0]) / (4.0 * w), qz = (U[1][0] - U[0][1]) / (4.0 * w), qw = w;
        qw += 1.0;
        const double n = std::sqrt(qw * qw + qx * qx + qy * qy + qz * qz);
        qw /= n, qx /= n, qy /= n, qz /= n;
        const double H[3][3] = {{1.0 - 2.0 * (qy * qy + qz * qz), 2.0 * (qx * qy - qw * qz), 2.0 * (qx * qz + qw * qy)},
                                {2.0 * (qx * qy + qw * qz), 1.0 - 2.0 * (qx * qx + qz * qz), 2.0 * (qy * qz - qw * qx)},
                                {2.0 * (qx * qz - qw * qy), 2.0 * (qy * qz + qw * qx), 1.0 - 2.0 * (qx * qx + qy * qy)}};
        // Its stretch S = U^T Q halfway (a scale changing: next to never), so the half turn's matrix is H (I + S) / 2.
        double S[3][3], Qh[3][3];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
            {
                double v = 0.0;
                for (int k = 0; k < 3; ++k)
                    v += U[k][i] * Q[k][j];
                S[i][j] = v;
            }
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
            {
                double v = 0.0;
                for (int k = 0; k < 3; ++k)
                    v += H[i][k] * (0.5 * ((k == j ? 1.0 : 0.0) + S[k][j]));
                Qh[i][j] = v;
            }
        // The axes halfway: Qh B, plus half of what Q B leaves of A (a camera's projection that changed).
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
            {
                double turned = 0.0, through = 0.0;
                for (int k = 0; k < 3; ++k)
                    turned += Qh[r][k] * B[k][c], through += Q[r][k] * B[k][c];
                m[4 * r + c] = static_cast<float>(turned + 0.5 * (A[r][c] - through));
            }
        return true;
    }

    void buildPasses(const Draw &d, std::vector<Pass> &out)
    {
        const size_t first = out.size();
        buildPassesOf(d, out);
        // (TODO 101 a) Each pass's pose halfway between its two, along the turn: the GPU's curve through the three (nr_d3d.cpp
        // `posed`). A turn of over 60 degrees in a frame is no pose: drawn as it is now in every picture. TF_POSES_LINEAR=1:
        // the average, the straight blend as before (an A/B).
        static const bool linear = std::getenv("TF_POSES_LINEAR") && *std::getenv("TF_POSES_LINEAR") == '1';
        for (size_t i = first; i < out.size(); ++i)
        {
            Pass &p = out[i];
            bool posed = true;
            if (linear)
                for (int k = 0; k < 16; ++k)
                    p.matrixM[k] = 0.5f * (p.matrix[k] + p.matrixB[k]);
            else
                posed = midPose(p.matrixB, p.matrix, p.matrixM);
            if (posed && p.clips)
            {
                if (linear)
                    for (int r = 0; r < 4; ++r)
                        for (int c = 0; c < 4; ++c)
                            p.clipM[r][c] = 0.5f * (p.clip[r][c] + p.clipB[r][c]);
                else
                    posed = midPose(&p.clipB[0][0], &p.clip[0][0], &p.clipM[0][0]);
            }
            if (!posed)
            {
                std::memcpy(p.matrixB, p.matrix, sizeof(p.matrixB));
                std::memcpy(p.matrixM, p.matrix, sizeof(p.matrixM));
                std::memcpy(p.clipB, p.clip, sizeof(p.clipB));
                std::memcpy(p.clipM, p.clip, sizeof(p.clipM));
                for (int l = 0; l < 3; ++l)
                    std::memcpy(p.lightsB[l], p.p[8 + l], sizeof(p.lightsB[l]));
            }
            else if (!p.clips)
                std::memcpy(p.clipM, p.clip, sizeof(p.clipM));
        }
    }
}
