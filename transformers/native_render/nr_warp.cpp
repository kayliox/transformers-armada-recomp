// The warp tunnel of a load, native (TODO 27; README.md section 15). The loader's blue tunnel - the streaked walls, the
// clouds of sparks flying out of it, the bar growing along the bottom, the white flash it opens with and the fade to
// black it ends with - is a VU1 program of its own (0x606B00, 0x1420 bytes; its data 0x604900, 0x21F0), started by the
// EE at every VBlank while a level loads (taWarpTransitionEffect, 0x535280; the VBlank hook 0x26E990 runs it). On the
// PS2 it draws each field of an interlaced 640 x 224 picture once per VBlank. Run by the port, its packets were drawn
// as a load screen (nr_capture.cpp looseGif): 900,000 vertices a picture made on the processor, 11-25 pictures a second
// during a load, the 4:3 picture stretched across a wide screen, every spark one PS2 pixel.
//
// Here the program is left out (vu1Run: the run is the hook's, nothing sent) and what it computes is done natively,
// from the real clock, at the screen's rate:
// - The walls: three passes (an additive textured pair and a subtractive one: qw42-53 - colour, scroll, TEX0, ALPHA) of
//   four bands of a cone (qw54-57: radius and depth at each end) round the 37 points of qw58-94, the textures
//   (3 x 64 x 64, 4-bit, CLUTs from the program's own upload packet, qw115-540) scrolled by the frame count. A fifth band
//   nearer the eye (z -1 to -2, the cone continued) fills the corners of a picture wider than 16:9.
// - The sparks: 20 emitters (qw400 + 27 k: six affine maps, colour, position, clock). Each one's points are an iterated
//   function system's orbit (the chaos game: from the origin, each point the last through one of the six maps, chosen by
//   VU1's random register - its sequence the same for every emitter), the first min(30000 c^2, 32000) + 10 drawn; the
//   emitter turns about the view axis (0.2 c + 0.004 F) and flies from z -10 towards the eye (+11 c), the clock c going
//   up 0.004 a frame; at 1 a new emitter (six random maps: a rotation of three axis turns, a scale, a move; a colour of
//   eight; a place). The points are made once an emitter (a mesh on the GPU); every picture only moves the emitters.
// - The bar (qw95-102: growing from the middle to the loader's progress, 120 frames to each new value), the opening
//   flash (additive white from 255 down 3 a frame) and the closing fade (subtractive, 8 a frame) - 1023.w's bits 13/14.
// Every value at the picture's own moment (the frame count, the clocks, the bar and the fade fractional between frames:
// the motion smooth at any rate), Hor+ on a wide screen (the GS's x narrowed about the centre: the cone and the sparks
// wider, the bar the 4:3 picture's), each spark a soft dot of a PS2 pixel's area (nr_d3d.cpp drawCloud). The random
// register, the maps, the clocks and the orbits are the program's own, step by step (checked against a model of the
// microcode: every spark within a third of a GS pixel, the walls within 0.07).
//
// TF_NATIVE_WARP=0: the program runs and its packets are drawn as before. TF_NATIVE_WARP_SHOT=<frame>[,...]: those
// frames of each tunnel saved as runs/native_warp_<tunnel>_<frame>.bmp. TF_NATIVE_WARP_HZ=<n>: pictures a second (default:
// the screen's refresh). TF_NATIVE_WARP_DOT=<x>: the dots' size (1 = a PS2 pixel's area). TF_NATIVE_WARP_LOG=1: a line
// a second while a tunnel shows.
#include "nr_types.h"
#include "nr_backend.h"

#include "runtime/ps2_host_display.h"
#include "runtime/ps2_vu1.h"
#include "runtime/ps2_vu1_native.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
extern "C" __declspec(dllimport) void *__stdcall GetCurrentThread();
extern "C" __declspec(dllimport) long __stdcall SetThreadDescription(void *thread, const wchar_t *description);
extern "C" __declspec(dllimport) int __stdcall SetThreadPriority(void *thread, int priority);
#endif

namespace tf::port
{
    int frameRate(); // (overrides.cpp: the frame rate setting, TF_FRAME_RATE)
}

namespace nr::warp
{
    bool vu1Run(ps2vu1native::Run &run); // (src/vu1_native.cpp's hook, first)

    namespace
    {
        using Clock = std::chrono::steady_clock;
        constexpr uint32_t kCodeBytes = 0x1420u;
        constexpr uint64_t kCodeHash = 0x031F0939D016FCE1ull; // (FNV-1a of the program's 0x1420 bytes)
        constexpr uint32_t kEmitters = 20u, kPoints = 32010u;
        constexpr double kStepNs = 1.0e9 / 59.94; // (the console's field rate: the program ran once a field)

        int64_t nowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }

        uint64_t hashBytes(const uint8_t *bytes, uint32_t size)
        {
            uint64_t hash = 0xCBF29CE484222325ull;
            for (uint32_t i = 0; i < size; ++i)
                hash = (hash ^ bytes[i]) * 0x100000001B3ull;
            return hash;
        }

        float fromBits(uint32_t b)
        {
            float f;
            std::memcpy(&f, &b, 4u);
            return f;
        }
        uint32_t bitsOf(float f)
        {
            uint32_t b;
            std::memcpy(&b, &f, 4u);
            return b;
        }
        // A sum or product of singles to single precision toward zero, as VU1 rounds.
        float rz(double d)
        {
            float f = static_cast<float>(d);
            if (std::fabs(static_cast<double>(f)) > std::fabs(d))
                f = std::nextafter(f, 0.0f);
            return f;
        }
        // VU1's random register: RNEXT (a 23-bit shift register, the bits 4 and 22 fed back), RXOR, as one float in [1, 2).
        uint32_t rnext(uint32_t r) { return (((r << 1) ^ ((r >> 4) & 1u) ^ ((r >> 22) & 1u)) & 0x7FFFFFu) | 0x3F800000u; }
        uint32_t rxor(uint32_t r, uint32_t b) { return 0x3F800000u | ((r ^ b) & 0x7FFFFFu); }

        bool on()
        {
            static const bool v = !(std::getenv("TF_NATIVE_WARP") && *std::getenv("TF_NATIVE_WARP") == '0');
            return v;
        }
        bool checking()
        {
            static const bool v = std::getenv("TF_NATIVE_WARP") && std::string(std::getenv("TF_NATIVE_WARP")) == "check";
            return v;
        }

        // ---- the program's data, as VU1 data memory holds it when the tunnel starts ----
        struct Data
        {
            uint8_t bytes[0x4000];
            uint32_t w(uint32_t q, uint32_t l) const
            {
                uint32_t v;
                std::memcpy(&v, bytes + q * 16u + l * 4u, 4u);
                return v;
            }
            float f(uint32_t q, uint32_t l) const { return fromBits(w(q, l)); }
            uint64_t d(uint32_t q) const { return static_cast<uint64_t>(w(q, 0)) | static_cast<uint64_t>(w(q, 1)) << 32; }
            uint32_t rgba(uint32_t q) const { return (w(q, 0) & 0xFFu) | (w(q, 1) & 0xFFu) << 8 | (w(q, 2) & 0xFFu) << 16 | (w(q, 3) & 0xFFu) << 24; }
        };

        struct Emitter
        {
            float clock = 0.0f;
            float pos[3] = {};
            uint32_t colour = 0u;
            std::shared_ptr<Mesh> points;
        };

        std::atomic<uint64_t> g_nextId{0xA700000000000000ull}; // (the meshes' and textures' ids: apart from the capture's)

        // The program, step by step (one step = one run of it, a field).
        struct Tunnel
        {
            const Data *data = nullptr;
            uint32_t frame = 0u;       // qw20.x: runs so far (F = this after a step's increment)
            float seed = 0.0f;         // qw22.z: the respawns' random state (a float of the register)
            Emitter em[kEmitters];
            float bar[4] = {};         // qw102: length (512), current, target, step
            uint32_t fadeCount = 0u;   // qw30.x: the closing fade's frames so far
            uint32_t fadeBefore = 0u;  // (its value at the last step's draw)
            uint32_t flags = 0xFFFFu;  // 1023.w (the EE's unk_1C): bit 14 the tunnel shown, bit 13 its opening flash
            std::vector<uint8_t> choice; // (the orbit's map choices: the same seed for every emitter)

            void start(const Data &d, float s, uint32_t f)
            {
                data = &d;
                seed = s;
                flags = f;
                frame = 0u;
                fadeCount = fadeBefore = 0u;
                bar[0] = d.f(102, 0), bar[1] = d.f(102, 1), bar[2] = d.f(102, 2), bar[3] = d.f(102, 3);
                // (the clocks: 1 + 0.05 k, a respawn for each at the first step)
                float c = 1.0f;
                const float step = fromBits(0x3D4CCCCDu); // (0.05: the program's loi)
                for (Emitter &e : em)
                {
                    e = Emitter{};
                    e.clock = c;
                    c = rz(static_cast<double>(c) + static_cast<double>(step));
                }
                // The orbit's choices: RINIT from qw21.x, one RNEXT, then each point RXOR a, RNEXT -> a, RXOR b, RNEXT -> b,
                // RNEXT -> the map (6 x (r - 1), toward zero); a and b start as qw21.x and qw21.y.
                choice.resize(kPoints);
                const uint32_t k = d.w(21, 0);
                uint32_t r = rnext(0x3F800000u | (k & 0x7FFFFFu)), a = k, b = d.w(21, 1);
                for (uint32_t i = 0; i < kPoints; ++i)
                {
                    r = rnext(rxor(r, a));
                    a = r;
                    r = rnext(rxor(r, b));
                    b = r;
                    r = rnext(r);
                    choice[i] = static_cast<uint8_t>(std::min(5, static_cast<int>(rz(static_cast<double>(fromBits(r) - 1.0f) * 6.0))));
                }
            }

            // A new emitter (0x0338-0x0A30): its place, colour and six maps from the random register, seeded from qw22.z.
            void respawn(Emitter &e)
            {
                const Data &d = *data;
                uint32_t r = 0x3F800000u | (bitsOf(rz(static_cast<double>(seed) + static_cast<double>(fromBits(0x3DFCB924u)))) & 0x7FFFFFu);
                const uint32_t a = r = rnext(r), b = r = rnext(r), c = r = rnext(r);
                r = rxor(r, a);
                const uint32_t dd = r = rnext(r);
                r = rxor(r, b);
                const uint32_t ee = r = rnext(r);
                r = rxor(r, c);
                const uint32_t g = r = rnext(r);
                e.pos[0] = rz(4.0 * fromBits(dd) - 6.0);
                e.pos[1] = rz(4.0 * fromBits(ee) - 6.0);
                e.pos[2] = -10.0f; // (the loi before the respawn)
                const int idx = std::clamp(static_cast<int>(rz(static_cast<double>(fromBits(g) - 1.0f) * 8.0)), 0, 7);
                e.colour = d.rgba(8u + static_cast<uint32_t>(idx));
                uint32_t v5[3] = {a, b, c};
                double maps[6][4][3];
                for (int m = 0; m < 6; ++m)
                {
                    const uint32_t r1 = r = rnext(r);
                    const uint32_t r2 = r = rnext(r);
                    r = rxor(r, v5[0]);
                    const uint32_t r3 = r = rnext(r);
                    r = rxor(r, v5[1]);
                    const uint32_t r4 = r = rnext(r);
                    r = rxor(r, v5[2]);
                    const uint32_t r5 = r = rnext(r), r6 = r = rnext(r), r7 = r = rnext(r);
                    v5[0] = r2, v5[1] = r3, v5[2] = r4;
                    // The map: p' = (s p + t) Q, Q = Rx(2 u5) Ry(2 u6) Rz(2 u7) (columns), s = 2 (r1 - 1.5), t = 2 (r2..r4 - 1.5).
                    const double s = 2.0 * (fromBits(r1) - 1.5), t[3] = {2.0 * (fromBits(r2) - 1.5), 2.0 * (fromBits(r3) - 1.5), 2.0 * (fromBits(r4) - 1.5)};
                    const double ax = 2.0 * (fromBits(r5) - 1.5), ay = 2.0 * (fromBits(r6) - 1.5), az = 2.0 * (fromBits(r7) - 1.5);
                    const double cx = std::cos(ax), sx = std::sin(ax), cy = std::cos(ay), sy = std::sin(ay), cz = std::cos(az), sz = std::sin(az);
                    const double rx[3][3] = {{1, 0, 0}, {0, cx, -sx}, {0, sx, cx}}, ry[3][3] = {{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}},
                                 rzm[3][3] = {{cz, -sz, 0}, {sz, cz, 0}, {0, 0, 1}};
                    double xy[3][3] = {}, q[3][3] = {};
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j)
                            for (int k = 0; k < 3; ++k)
                                xy[i][j] += rx[i][k] * ry[k][j];
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j)
                            for (int k = 0; k < 3; ++k)
                                q[i][j] += xy[i][k] * rzm[k][j];
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j)
                            maps[m][i][j] = s * q[i][j];
                    for (int j = 0; j < 3; ++j)
                        maps[m][3][j] = t[0] * q[0][j] + t[1] * q[1][j] + t[2] * q[2][j];
                }
                r = rnext(r);
                seed = fromBits(r);
                // The orbit (0x0E18-0x11E8): from the origin, each point the last through its map.
                auto mesh = std::make_shared<Mesh>();
                mesh->id = g_nextId++;
                mesh->vertices.resize(kPoints);
                mesh->indices.assign(1u, 0u); // (none drawn as indices: drawCloud reads the vertices as instances)
                double p[3] = {0.0, 0.0, 0.0};
                for (uint32_t i = 0; i < kPoints; ++i)
                {
                    const double(&mm)[4][3] = maps[choice[i]];
                    const double n[3] = {p[0] * mm[0][0] + p[1] * mm[1][0] + p[2] * mm[2][0] + mm[3][0], p[0] * mm[0][1] + p[1] * mm[1][1] + p[2] * mm[2][1] + mm[3][1],
                                         p[0] * mm[0][2] + p[1] * mm[1][2] + p[2] * mm[2][2] + mm[3][2]};
                    p[0] = n[0], p[1] = n[1], p[2] = n[2];
                    Vertex &v = mesh->vertices[i];
                    v.x = static_cast<float>(p[0]);
                    v.y = static_cast<float>(p[1]);
                    v.z = static_cast<float>(p[2]);
                }
                e.points = std::move(mesh);
            }

            // One run of the program: the frame count, the clocks (and respawns), the bar's step, the fade's count.
            void step(bool progressNew, uint32_t progress, uint32_t steps)
            {
                const Data &d = *data;
                fadeBefore = fadeCount;
                ++frame;
                const float speed = d.f(23, 0);
                for (Emitter &e : em)
                {
                    float c = rz(static_cast<double>(e.clock) + static_cast<double>(speed));
                    if (rz(static_cast<double>(c) - 1.0) >= 0.0f)
                    {
                        c = rz(static_cast<double>(c) - 1.0);
                        respawn(e);
                    }
                    e.clock = c;
                }
                if (progressNew && steps)
                {
                    const float target = rz(static_cast<double>(static_cast<float>(progress)) * 0.001f);
                    bar[2] = target;
                    bar[3] = (target - bar[1]) / static_cast<float>(steps);
                }
                bar[1] = std::min(rz(static_cast<double>(bar[1]) + bar[3]), bar[2]);
                if (!(flags & 0x4000u))
                    ++fadeCount;
            }
        };

        // ---- what the EE hands the program (VU1 data qw1022/1023), and the session ----
        struct Session
        {
            std::mutex mutex;
            std::condition_variable cv;
            bool active = false;       // a tunnel shown natively
            bool ending = false;       // (the game's own frames came back)
            uint32_t number = 0u;      // tunnels so far
            uint32_t declinedTold = 0u; // (TODO 185 b: the tunnel whose run left to VU1 was logged)
            int64_t startNs = 0, lastRunNs = 0;
            uint64_t runs = 0u;
            float seed = 0.0f;
            uint32_t flags = 0xFFFFu;
            // progress updates not yet taken by a step, in order (value, steps)
            std::vector<std::pair<uint32_t, uint32_t>> progress;
            std::shared_ptr<Data> data;
        } g_s;
        std::mutex g_submit; // (held while a picture is handed over: gameFrame waits for it, none goes after)
        std::atomic<bool> g_active{false};

        // ---- the pictures ----
        struct Textures
        {
            std::shared_ptr<const Texture> pass[3];
        };

        // The program's own upload packet (qw115: three 64 x 64 4-bit images at 0x3100/0x3300/0x3500 and their 16-colour
        // palettes at 0x3000/0x3200/0x3400, each IMAGE data after its A+D BITBLTBUF/TRXREG/TRXDIR).
        Textures makeTextures(const Data &d)
        {
            Textures t;
            const uint32_t imageAt[3] = {120u, 262u, 404u}, clutAt[3] = {253u, 395u, 537u};
            for (int p = 0; p < 3; ++p)
            {
                auto tex = std::make_shared<Texture>();
                tex->id = g_nextId++;
                tex->width = tex->height = 64u;
                tex->pixels.resize(64u * 64u);
                uint32_t clut[16];
                for (uint32_t i = 0; i < 16u; ++i)
                    std::memcpy(&clut[i], d.bytes + clutAt[p] * 16u + i * 4u, 4u);
                for (uint32_t i = 0; i < 64u * 64u; ++i)
                {
                    const uint8_t byte = d.bytes[imageAt[p] * 16u + i / 2u];
                    tex->pixels[i] = clut[(i & 1u) ? (byte >> 4) : (byte & 0xFu)];
                }
                buildMips(*tex);
                t.pass[p] = std::move(tex);
            }
            return t;
        }

        constexpr float kOriginX = 2048.0f - 320.0f, kOriginY = 2048.0f - 112.0f;
        constexpr uint32_t kTarget = 0u | 10u << 16; // (FBP 0, FBW 10 (640), PSMCT32: the field buffer the tunnel drew into)

        Draw baseDraw(uint32_t prim)
        {
            Draw d;
            auto mesh = std::make_shared<Mesh>();
            mesh->id = g_nextId++;
            mesh->immediate = true;
            mesh->projective = true;
            mesh->hasColour = true;
            mesh->hasUv = true;
            mesh->prim = prim;
            d.mesh = mesh;
            const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            std::memcpy(d.matrix, identity, sizeof(identity));
            d.target = kTarget;
            d.originX = kOriginX;
            d.originY = kOriginY;
            d.width = 640.0f;
            d.height = 224.0f;
            d.scissor[0] = 0.0f, d.scissor[1] = 0.0f, d.scissor[2] = 639.0f, d.scissor[3] = 223.0f;
            d.test = 0x20000u;      // (no depth test)
            d.zbuf = 1ull << 32;    // (no depth written)
            return d;
        }

        Vertex vertex(float x, float y, uint32_t colour, float u = 0.0f, float v = 0.0f, float q = 1.0f)
        {
            Vertex o;
            o.x = x, o.y = y, o.z = 0.0f;
            o.colour = colour;
            o.u = u, o.v = v, o.size = q;
            return o;
        }

        // A screen-space rectangle as two triangles.
        void rect(Mesh &m, float x0, float y0, float x1, float y1, uint32_t colour)
        {
            const Vertex a = vertex(x0, y0, colour), b = vertex(x1, y0, colour), c = vertex(x0, y1, colour), e = vertex(x1, y1, colour);
            m.vertices.insert(m.vertices.end(), {a, b, c, c, b, e});
        }

        void finish(Draw &d)
        {
            Mesh &m = const_cast<Mesh &>(*d.mesh);
            m.indices.resize(m.vertices.size());
            for (uint32_t i = 0; i < m.indices.size(); ++i)
                m.indices[i] = i;
        }

        // The frame's draws at its moment: `frac` of a step after the tunnel's last step (0..1, a little over when late).
        void buildDraws(const Tunnel &tn, const Textures &tx, double frac, float aspect, std::vector<Draw> &out)
        {
            const Data &d = *tn.data;
            const float squeeze = aspect > 1.34f ? (4.0f / 3.0f) / aspect : 1.0f;
            const double F = static_cast<double>(tn.frame) + frac;
            auto sx = [&](double x) { return static_cast<float>(2048.0 + (x - 2048.0) * squeeze); };
            // 1. The field buffer's clear (GsSwapDBuff's packet: black, opaque).
            {
                Draw clear = baseDraw(0x06u);
                rect(const_cast<Mesh &>(*clear.mesh), kOriginX - 8.0f, kOriginY - 8.0f, kOriginX + 648.0f, kOriginY + 232.0f, 0x80000000u);
                finish(clear);
                out.push_back(std::move(clear));
            }
            // 2. The walls: three passes of the cone's bands (and a nearer one), STQ as the program makes them.
            const uint64_t tex1 = d.d(111), clamp = d.d(112);
            const double step = fromBits(0x3DE38E39u); // (0.111111: a ninth of the texture a point of the circle)
            for (uint32_t p = 0; p < 3u; ++p)
            {
                const uint32_t base = 42u + 4u * p;
                const uint32_t colour = d.rgba(base);
                const double scrollS = d.f(base + 1u, 0) * F, scrollT = d.f(base + 1u, 1) * F;
                const double fs = scrollS - std::trunc(scrollS), ft = scrollT - std::trunc(scrollT);
                Draw wall = baseDraw(0x54u); // (tristrip, textured, blended - as the strips' tag)
                wall.tex0 = d.d(base + 2u);
                wall.alpha = d.d(base + 3u);
                wall.tex1 = tex1;
                wall.clamp = clamp;
                wall.texture = tx.pass[p];
                Mesh &m = const_cast<Mesh &>(*wall.mesh);
                for (int band = -1; band < 4; ++band)
                {
                    // (band -1: z -1 to -2, the first band's cone continued towards the eye - the corners of a picture wider
                    // than 16:9 - its texture the first band's nearest row drawn on outward: continued, T would wrap round to
                    // the texture's far end, the subtractive pass's brightest rows - a black rim round the tunnel. The full-screen
                    // sprites reach a few GS pixels past the screen: the picture's half-pixel shift left its edge uncovered.)
                    double r1, r2, z1, z2, t1;
                    if (band < 0)
                    {
                        const double ra = d.f(54, 0), rb = d.f(54, 1), za = d.f(54, 2), zb = d.f(54, 3);
                        z2 = za, r2 = ra;
                        z1 = za + 1.0;
                        r1 = ra + (ra - rb) / (za - zb) * (z1 - za);
                        t1 = ft; // (the first band's own nearest T: the two meet without a seam)
                    }
                    else
                    {
                        r1 = d.f(54u + static_cast<uint32_t>(band), 0), r2 = d.f(54u + static_cast<uint32_t>(band), 1);
                        z1 = d.f(54u + static_cast<uint32_t>(band), 2), z2 = d.f(54u + static_cast<uint32_t>(band), 3);
                        t1 = 0.25 * band + ft;
                    }
                    const double t2 = band < 0 ? t1 : t1 + 0.25;
                    Vertex prevA, prevB;
                    for (uint32_t k = 0; k < 37u; ++k)
                    {
                        const double c = d.f(58u + k, 0), s = d.f(58u + k, 1);
                        const double S = fs + step * k;
                        const double q1 = 1.0 / -z1, q2 = 1.0 / -z2; // (Q made positive: S/Q, T/Q as the GS's)
                        const Vertex a = vertex(sx(2048.0 + 200.0 * r1 * c / z1), static_cast<float>(2048.0 + 100.0 * r1 * s / z1), colour,
                                                static_cast<float>(S * q1), static_cast<float>(t1 * q1), static_cast<float>(q1));
                        const Vertex b = vertex(sx(2048.0 + 200.0 * r2 * c / z2), static_cast<float>(2048.0 + 100.0 * r2 * s / z2), colour,
                                                static_cast<float>(S * q2), static_cast<float>(t2 * q2), static_cast<float>(q2));
                        if (k)
                            m.vertices.insert(m.vertices.end(), {prevA, prevB, a, prevB, a, b});
                        prevA = a, prevB = b;
                    }
                }
                finish(wall);
                out.push_back(std::move(wall));
            }
            // 3. The sparks: every emitter at the moment's clock and frame.
            {
                auto cloud = std::make_shared<PointCloud>();
                cloud->squeeze = squeeze;
                static const float dot = std::getenv("TF_NATIVE_WARP_DOT") ? static_cast<float>(std::atof(std::getenv("TF_NATIVE_WARP_DOT"))) : 1.0f;
                cloud->sigma = 0.4f * std::max(0.25f, dot);
                const double speed = d.f(23, 0);
                for (const Emitter &e : tn.em)
                {
                    if (!e.points)
                        continue;
                    const double c = e.clock + speed * frac;
                    const double n = std::min(30000.0 * c * c, 32000.0);
                    PointCloud::Set set;
                    set.points = e.points;
                    set.count = std::min(kPoints, static_cast<uint32_t>(std::max(0.0, n)) + 10u);
                    // local -> world: Rz(0.2 c) then the place (and +11 c towards the eye) then Rz(0.004 F); -> the GS: qw16-19.
                    const double a1 = 0.2 * c, a2 = 0.004 * F;
                    const double c1 = std::cos(a1), s1 = std::sin(a1), c2 = std::cos(a2), s2 = std::sin(a2);
                    const double tz = e.pos[2] + 11.0 * c;
                    // rows of M (row vectors): x -> (c1, s1, 0) then Rz(a2); the place rotated by a2 only.
                    const double mx[3] = {c1 * c2 - s1 * s2, c1 * s2 + s1 * c2, 0.0}, my[3] = {-s1 * c2 - c1 * s2, -s1 * s2 + c1 * c2, 0.0};
                    const double mt[3] = {e.pos[0] * c2 - e.pos[1] * s2, e.pos[0] * s2 + e.pos[1] * c2, tz};
                    const double px = d.f(16, 0), py = d.f(17, 1), cx = d.f(18, 0), cy = d.f(18, 1);
                    // GS = (px X + cx Z, py Y + cy Z, -, Z) / Z for a world point (X, Y, Z).
                    auto row = [&](float *r, const double v[3], double w) {
                        r[0] = static_cast<float>(px * v[0] + cx * v[2] + 0.0 * w);
                        r[1] = static_cast<float>(py * v[1] + cy * v[2]);
                        r[2] = 0.0f;
                        r[3] = static_cast<float>(v[2]);
                    };
                    const double mz[3] = {0.0, 0.0, 1.0};
                    row(set.rows + 0, mx, 0.0);
                    row(set.rows + 4, my, 0.0);
                    row(set.rows + 8, mz, 0.0);
                    row(set.rows + 12, mt, 1.0);
                    // (the GS's FIX 0x40 additive: half the colour, of 255)
                    for (int k = 0; k < 3; ++k)
                        set.colour[k] = static_cast<float>((e.colour >> (8 * k)) & 0xFFu) / 255.0f * 0.5f;
                    set.colour[3] = 0.0f;
                    cloud->sets.push_back(std::move(set));
                }
                Draw sparks;
                sparks.cloud = std::move(cloud);
                sparks.target = kTarget;
                sparks.originX = kOriginX;
                sparks.originY = kOriginY;
                sparks.width = 640.0f;
                sparks.height = 224.0f;
                sparks.scissor[0] = 0.0f, sparks.scissor[1] = 0.0f, sparks.scissor[2] = 639.0f, sparks.scissor[3] = 223.0f;
                out.push_back(std::move(sparks));
            }
            // 4. The bar: from the middle out to the progress (qw95-102: 50% blend, ALPHA 0x44).
            {
                const float cur = std::min(static_cast<float>(tn.bar[1] + tn.bar[3] * frac), tn.bar[2]);
                const double half = 0.5 * std::min(static_cast<double>(tn.bar[0]) * cur, static_cast<double>(tn.bar[0]));
                if (half > 0.0)
                {
                    Draw bar = baseDraw(0x46u);
                    bar.alpha = d.d(96);
                    const float y0 = static_cast<float>(d.w(100, 1)) / 16.0f, y1 = static_cast<float>(d.w(101, 1)) / 16.0f;
                    rect(const_cast<Mesh &>(*bar.mesh), sx(2048.0 - half), y0, sx(2048.0 + half), y1, d.rgba(99));
                    finish(bar);
                    out.push_back(std::move(bar));
                }
            }
            // 5. The opening flash or fade-in (the first 120 frames) / the closing fade (1023.w bit 14 clear).
            {
                double level = -1.0;
                uint64_t alpha = 0;
                if (!(tn.flags & 0x4000u))
                {
                    level = std::min(255.0, d.f(28, 0) * (static_cast<double>(tn.fadeBefore) + frac));
                    alpha = d.d(29);
                }
                else if (F <= 120.0)
                {
                    if (tn.flags & 0x2000u)
                        level = std::max(0.0, d.f(24, 0) + d.f(25, 0) * F), alpha = d.d(26);
                    else
                        level = std::max(0.0, d.f(27, 0) - d.f(28, 0) * F), alpha = d.d(29);
                }
                if (level > 0.0)
                {
                    const uint32_t v = static_cast<uint32_t>(level);
                    Draw fade = baseDraw(0x46u);
                    fade.alpha = alpha;
                    rect(const_cast<Mesh &>(*fade.mesh), kOriginX - 8.0f, kOriginY - 8.0f, kOriginX + 648.0f, kOriginY + 232.0f, v | v << 8 | v << 16 | 0x80000000u);
                    finish(fade);
                    out.push_back(std::move(fade));
                }
            }
        }

        std::vector<uint32_t> shotFrames()
        {
            std::vector<uint32_t> v;
            if (const char *e = std::getenv("TF_NATIVE_WARP_SHOT"))
                for (const char *p = e; *p;)
                {
                    char *end = nullptr;
                    const unsigned long n = std::strtoul(p, &end, 10);
                    if (end == p)
                        break;
                    v.push_back(static_cast<uint32_t>(n));
                    p = *end == ',' ? end + 1 : end;
                }
            return v;
        }

        // The pictures' thread: while a tunnel shows, one picture a refresh of the screen, each at its own moment.
        void run()
        {
#if defined(_WIN32)
            SetThreadDescription(GetCurrentThread(), L"NativeWarp");
            // (above normal: it sleeps almost all the time, and a load's busy threads must not make it miss a refresh)
            SetThreadPriority(GetCurrentThread(), 1); // (THREAD_PRIORITY_ABOVE_NORMAL)
#endif
            static const bool log = std::getenv("TF_NATIVE_WARP_LOG") && *std::getenv("TF_NATIVE_WARP_LOG") == '1';
            static const std::vector<uint32_t> shots = shotFrames();
            for (;;)
            {
                std::shared_ptr<Data> data;
                uint32_t number = 0u;
                int64_t startNs = 0;
                Tunnel tn;
                {
                    std::unique_lock<std::mutex> lock(g_s.mutex);
                    g_s.cv.wait(lock, [] { return g_s.active && g_s.data; });
                    data = g_s.data;
                    number = g_s.number;
                    startNs = g_s.startNs;
                    tn.start(*data, g_s.seed, g_s.flags);
                }
                const Textures tx = makeTextures(*data);
                uint64_t steps = 0u, pictures = 0u;
                int64_t next = nowNs(), lastPicture = 0, worstGap = 0, logAt = nowNs();
                double gaps = 0.0;
                size_t shotAt = 0u;
                for (;;)
                {
                    // The picture's period: the screen's refresh, or TF_NATIVE_WARP_HZ.
                    static const double hz = std::getenv("TF_NATIVE_WARP_HZ") ? std::atof(std::getenv("TF_NATIVE_WARP_HZ")) : 0.0;
                    int64_t period = 4166667, vblank = 0;
                    bool fixedRefresh = false;
                    {
                        Clock::time_point vb{};
                        Clock::duration refresh{};
                        if (hz > 1.0)
                            period = static_cast<int64_t>(1.0e9 / hz);
                        else if (ps2_host_display::screenTiming(vb, refresh) && refresh.count() > 0)
                        {
                            period = std::chrono::duration_cast<std::chrono::nanoseconds>(refresh).count();
                            fixedRefresh = !ps2_host_display::screenVariable();
                            vblank = std::chrono::duration_cast<std::chrono::nanoseconds>(vb.time_since_epoch()).count();
                        }
                        // (no faster than the frame rate setting - at least 60: the console's field rate)
                        if (hz <= 1.0)
                            period = std::max<int64_t>(period, static_cast<int64_t>(1.0e9 / std::max(60, tf::port::frameRate())));
                        period = std::clamp<int64_t>(period, 2000000, 34000000);
                    }
                    // The picture's moment: the one planned (a period after the last - a little late out of the sleep is not a
                    // later moment: those added up, the pictures came 7.5 ms apart on a 6.94 ms refresh), on a fixed refresh
                    // the screen's next refresh's (less half a millisecond to be handed over in time); made a millisecond or
                    // two ahead of it. Only when a picture's moment has passed by a whole period is the next one now.
                    int64_t t = nowNs();
                    if (fixedRefresh && vblank && hz <= 1.0)
                    {
                        const int64_t k = (next + period / 2 - vblank) / period; // (the refresh nearest the planned moment)
                        next = vblank + k * period - 500000;
                    }
                    const int64_t lead = std::min<int64_t>(2000000, period / 2);
                    if (t < next - lead)
                    {
                        std::this_thread::sleep_for(std::chrono::nanoseconds(next - lead - t));
                        t = nowNs();
                    }
                    const int64_t due = t > next + period ? t : next;
                    // The steps due by the picture's moment (the program once a field), the EE's inputs taken in order.
                    // (step k - the program's k-th run - at (k - 1) steps' time from the tunnel's start)
                    const double at = std::max(0.0, static_cast<double>(due - startNs) / kStepNs);
                    const uint64_t want = static_cast<uint64_t>(at) + 1u;
                    bool stop = false;
                    {
                        std::lock_guard<std::mutex> lock(g_s.mutex);
                        stop = !g_s.active || g_s.number != number;
                        tn.flags = g_s.flags;
                        // (no run of the program for 5 s: the tunnel is gone without the game's frames - shown no longer)
                        if (nowNs() - g_s.lastRunNs > 5000000000ll)
                        {
                            g_s.active = false, stop = true;
                            g_active.store(false, std::memory_order_release);
                        }
                        if (!stop && want > steps + 120u)
                            steps = want - 1u; // (far behind - a stall of seconds: the steps between not caught up)
                        while (!stop && steps < want)
                        {
                            bool fresh = false;
                            uint32_t value = 0u, count = 0u;
                            if (!g_s.progress.empty())
                            {
                                value = g_s.progress.front().first, count = g_s.progress.front().second;
                                g_s.progress.erase(g_s.progress.begin());
                                fresh = true;
                            }
                            tn.step(fresh, value, count);
                            ++steps;
                        }
                    }
                    if (stop)
                        break;
                    const double frac = std::clamp(at - std::floor(at), 0.0, 1.0);
                    Frame f;
                    f.loose = true;
                    f.tick = vblankTick();
                    f.dueNs = due;
                    f.periodNs = period;
                    f.pictureNs = period;
                    f.vblanksOnRefresh = fixedRefresh;
                    f.displayAspect = ps2_host_display::displayAspect();
                    f.overlaySqueeze = 1.0f;
                    buildDraws(tn, tx, frac, f.displayAspect > 0.5f ? f.displayAspect : 4.0f / 3.0f, f.draws);
                    std::string shot;
                    while (shotAt < shots.size() && shots[shotAt] < tn.frame)
                        ++shotAt;
                    if (shotAt < shots.size() && shots[shotAt] == tn.frame)
                    {
                        shot = "runs/native_warp_" + std::to_string(number) + "_" + std::to_string(tn.frame) + ".bmp";
                        ++shotAt;
                    }
                    {
                        std::lock_guard<std::mutex> lock(g_submit);
                        if (!g_active.load(std::memory_order_acquire))
                            break;
                        submitFrame(std::move(f), shot);
                        noteLooseFrame();
                    }
                    ++pictures;
                    const int64_t shown = nowNs();
                    if (lastPicture)
                    {
                        const int64_t gap = shown - lastPicture;
                        gaps += static_cast<double>(gap);
                        worstGap = std::max(worstGap, gap);
                    }
                    lastPicture = shown;
                    if (log && shown - logAt >= 1000000000ll)
                    {
                        std::fprintf(stderr, "[native warp] tunnel %u: frame %u, %llu pictures, period %.2f ms, worst gap %.2f ms\n", number, tn.frame,
                                     static_cast<unsigned long long>(pictures), static_cast<double>(period) / 1.0e6, static_cast<double>(worstGap) / 1.0e6);
                        logAt = shown;
                    }
                    next = due + period;
                }
                const double seconds = static_cast<double>(nowNs() - startNs) / 1.0e9;
                std::fprintf(stderr, "[native warp] tunnel %u over: %u frames of the program, %llu pictures in %.1f s (%.1f a second, the longest gap %.1f ms)\n",
                             number, tn.frame, static_cast<unsigned long long>(pictures), seconds, seconds > 0.0 ? static_cast<double>(pictures) / seconds : 0.0,
                             static_cast<double>(worstGap) / 1.0e6);
            }
        }

        void ensureThread()
        {
            static std::once_flag once;
            std::call_once(once, [] { std::thread(run).detach(); });
        }
    }

    namespace
    {
        // TF_NATIVE_WARP=check: the program runs as before (its packets drawn as a load screen) and beside it the native
        // steps, one a run, each run's start comparing what the run before left in VU1 data memory with the native state -
        // the emitters' clocks, places, colours, six maps (qw400 + 27 k), the respawns' seed (qw22.z), the bar (qw102).
        void check(const uint8_t *vu)
        {
            static Data data;
            static Tunnel tn;
            static uint64_t runs = 0u, compared = 0u, words = 0u, bad = 0u;
            static double worstMap = 0.0, worstClock = 0.0;
            static int told = 0;
            auto f = [&](uint32_t q, uint32_t l) {
                float v;
                std::memcpy(&v, vu + q * 16u + l * 4u, 4u);
                return v;
            };
            uint32_t count;
            std::memcpy(&count, vu + 20u * 16u, 4u);
            if (count == 0u)
            {
                std::memcpy(data.bytes, vu, 0x4000u);
                uint32_t seed, flags;
                std::memcpy(&seed, vu + 1023u * 16u + 8u, 4u);
                std::memcpy(&flags, vu + 1023u * 16u + 12u, 4u);
                tn.start(data, fromBits(seed), flags & 0xFFFFu);
                runs = compared = words = bad = 0u;
                worstMap = worstClock = 0.0;
                std::fprintf(stderr, "[native warp:check] a tunnel begins: the native steps beside the program's runs\n");
            }
            else if (tn.data && count == tn.frame)
            {
                // (what run `count` left, against the native state after as many steps)
                ++compared;
                for (uint32_t e = 0; e < kEmitters; ++e)
                {
                    const uint32_t b = 400u + 27u * e;
                    const Emitter &em = tn.em[e];
                    const double dc = std::fabs(static_cast<double>(f(b + 24u, 0)) - em.clock);
                    worstClock = std::max(worstClock, dc);
                    ++words;
                    bool wrong = dc > 1e-6;
                    for (int l = 0; l < 2; ++l)
                        wrong = wrong || f(b + 26u, static_cast<uint32_t>(l)) != em.pos[l];
                    uint32_t colour = 0u;
                    for (uint32_t l = 0; l < 4u; ++l)
                        colour |= (vu[(b + 25u) * 16u + l * 4u] & 0xFFu) << (8u * l);
                    wrong = wrong || colour != em.colour;
                    if (em.points)
                    {
                        // (the maps themselves: the orbit's first points through them, as the program's maps take them)
                        double p[3] = {0.0, 0.0, 0.0};
                        for (uint32_t i = 0; i < 64u; ++i)
                        {
                            const uint32_t m = b + 4u * tn.choice[i];
                            double n[3];
                            for (uint32_t j = 0; j < 3u; ++j)
                                n[j] = p[0] * f(m, j) + p[1] * f(m + 1u, j) + p[2] * f(m + 2u, j) + f(m + 3u, j);
                            std::memcpy(p, n, sizeof(p));
                            const Vertex &v = em.points->vertices[i];
                            worstMap = std::max({worstMap, std::fabs(v.x - p[0]), std::fabs(v.y - p[1]), std::fabs(v.z - p[2])});
                        }
                    }
                    bad += wrong ? 1u : 0u;
                    if (wrong && told < 20)
                    {
                        ++told;
                        std::fprintf(stderr, "[native warp:check] run %u emitter %u: VU1 clock %g place %g %g colour %08x, native %g / %g %g / %08x\n", count, e,
                                     f(b + 24u, 0), f(b + 26u, 0), f(b + 26u, 1), colour, em.clock, em.pos[0], em.pos[1], em.colour);
                    }
                }
                const double db = std::fabs(static_cast<double>(f(102, 1)) - tn.bar[1]);
                if (db > 1e-6 && told < 20)
                {
                    ++told;
                    std::fprintf(stderr, "[native warp:check] run %u: the bar VU1 %g, native %g\n", count, f(102, 1), tn.bar[1]);
                }
                if ((compared % 60u) == 0u)
                    std::fprintf(stderr, "[native warp:check] %llu runs compared: %llu of %llu emitter states differ; clocks within %g, the orbits' first 64 points within %g\n",
                                 static_cast<unsigned long long>(compared), static_cast<unsigned long long>(bad), static_cast<unsigned long long>(words),
                                 worstClock, worstMap);
            }
            if (!tn.data)
                return;
            // This run: the native step, with its inputs (1022: the progress; 1023.w: the flags).
            uint32_t progress, steps, flags;
            std::memcpy(&progress, vu + 1022u * 16u, 4u);
            std::memcpy(&steps, vu + 1022u * 16u + 4u, 4u);
            std::memcpy(&flags, vu + 1023u * 16u + 12u, 4u);
            tn.flags = flags & 0xFFFFu;
            tn.step(progress != 0u, progress, steps & 0xFFFFu);
            ++runs;
        }
    }

    bool active() { return g_active.load(std::memory_order_acquire); }

    void gameFrame()
    {
        if (!g_active.load(std::memory_order_acquire))
            return;
        std::lock_guard<std::mutex> submitting(g_submit);
        std::lock_guard<std::mutex> lock(g_s.mutex);
        if (g_s.active)
        {
            g_s.active = false;
            g_active.store(false, std::memory_order_release);
            std::fprintf(stderr, "[native warp] the game's frames are back: tunnel %u ends\n", g_s.number);
        }
    }

    // The tunnel's program offered to the VU1 hook (src/vu1_native.cpp): taken here, nothing run, when the native
    // renderer draws alone. The run's effects kept: its inputs taken from qw1022/1023 (1022.x cleared, as the program
    // does), qw20.x the run count (a tunnel begun: 0 there - Start copies the program's data in afresh), the end pc.
    bool vu1Run(ps2vu1native::Run &run)
    {
        VU1State &state = *run.state;
        if (state.pc != 0u || run.dataSize != 0x4000u || !run.code || !on())
            return false;
        static const uint32_t first = 0x09030014u; // (ILW.x vi03, 20(vi00) - the program's first pair's lower)
        uint32_t lower = 0u;
        std::memcpy(&lower, run.code, 4u);
        // (TODO 185 b) A run of the program left to the microcode while a tunnel is drawn natively: the program then
        // runs on data memory the native steps never kept (only qw20 and 1022 are), so its packets are not its own.
        // Logged once a tunnel, with why, so a freeze in a tunnel names its cause.
        const auto declined = [&](const char *why) {
            std::lock_guard<std::mutex> lock(g_s.mutex);
            if (g_s.active && g_s.declinedTold != g_s.number)
            {
                g_s.declinedTold = g_s.number;
                std::fprintf(stderr, "[native warp] tunnel %u: a run of its program left to VU1 (%s)\n", g_s.number, why);
            }
            return false;
        };
        if (lower != first)
            return false;
        if (hashBytes(run.code, kCodeBytes) != kCodeHash)
            return declined("its code changed");
        if (checking())
        {
            check(run.data);
            return false;
        }
        if (ps2_host_display::emulatedDrawing() || !backend().drawsPictures())
            return declined("the GS draws"); // (the GS draws: the program's packets are its pictures)
        uint8_t *data = run.data;
        auto word = [&](uint32_t q, uint32_t l) {
            uint32_t v;
            std::memcpy(&v, data + q * 16u + l * 4u, 4u);
            return v;
        };
        const uint32_t count = word(20, 0);
        const int64_t t = nowNs();
        {
            std::lock_guard<std::mutex> lock(g_s.mutex);
            if (count == 0u)
            {
                // A new tunnel: the program's data as Start copied it in, the seed (1023.z) and flags (1023.w).
                auto copy = std::make_shared<Data>();
                std::memcpy(copy->bytes, data, 0x4000u);
                g_s.data = std::move(copy);
                g_s.seed = fromBits(word(1023, 2));
                g_s.flags = word(1023, 3) & 0xFFFFu;
                g_s.progress.clear();
                g_s.startNs = t;
                g_s.runs = 0u;
                ++g_s.number;
                g_s.active = true;
                g_active.store(true, std::memory_order_release);
                std::fprintf(stderr, "[native warp] tunnel %u begins (seed %g, flags %04x): drawn natively\n", g_s.number, g_s.seed, g_s.flags);
                ensureThread();
                g_s.cv.notify_all();
            }
            g_s.flags = word(1023, 3) & 0xFFFFu;
            if (const uint32_t progress = word(1022, 0); progress != 0u)
                g_s.progress.emplace_back(progress, word(1022, 1) & 0xFFFFu);
            g_s.lastRunNs = t;
            ++g_s.runs;
        }
        const uint32_t runs = count + 1u, zero = 0u;
        std::memcpy(data + 20u * 16u, &runs, 4u);
        std::memcpy(data + 1022u * 16u, &zero, 4u);
        state.pc = 0x1420u; // (the pair after the E bit's delay slot)
        return true;
    }
}
