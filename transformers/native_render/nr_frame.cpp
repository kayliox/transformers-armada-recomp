// nr_frame.cpp - the native renderer's frame builder: a frame's draw list (Frame, nr_types.h) turned into a picture through
// the device interface (gfx::Device, nr_device.h) alone - no graphics API here. It owns everything about the GS: which
// targets exist and at which GS address, the passes' order, the field weave, the glow, the anti-aliasing's place, the
// draw loop (each draw's state, constants and geometry), the sun's occlusion probe, the moment pictures' constants, the
// profile and the debug saves. Its textures and meshes are kept by the game's ids; the device holds the GPU's side.
// docs/RENDERER_BACKEND.md describes the split and what a backend has to provide.

#include "nr_frame.h"
#include "nr_types.h"
#include "nr_draw.h"
#include "nr_device.h"
#include "nr_occlusion.h"
#include "view_turn.h"
#include "runtime/ps2_host_display.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nr
{
    // N8 item 5 (sharp textures): the mip levels' shift toward the finer ones, in levels: -1 matched to the picture's height
    // (log2(lines / 448): the GS's distance-based choice made for the PS2's 448 lines), 0 the PS2's, above 0 that many.
    std::atomic<float> g_texSharp{-1.0f};
    void setTextureSharpness(float shift) { g_texSharp.store(shift, std::memory_order_relaxed); }
    // Issue #17 (smooth textures): a texture magnified on screen takes the draw's MMAG filter even where the GS's
    // distance rule picks MMIN (nr_d3d.cpp sampleGs). On by default; off: the PS2's choice.
    std::atomic<bool> g_texSmooth{true};
    void setTextureSmoothing(bool on) { g_texSmooth.store(on, std::memory_order_relaxed); }
}

namespace nr::frame
{
    // A mip level from the one above: each texel the rounded mean of its 2x2 (per channel, (sum + 2) / 4; an odd
    // edge's last row or column repeated) - two channels at a time in 16-bit lanes (TODO 76: the per-channel loop
    // was most of a new game texture's cost once its video memory came from the pool).
    void halveLevel(const uint32_t *src, uint32_t pw, uint32_t ph, uint32_t *dst)
    {
        const uint32_t w = std::max(1u, pw / 2u), h = std::max(1u, ph / 2u);
        constexpr uint32_t m = 0x00FF00FFu, r = 0x00020002u;
        for (uint32_t y = 0; y < h; ++y)
        {
            const uint32_t *r0 = src + static_cast<size_t>(std::min(ph - 1u, 2u * y)) * pw;
            const uint32_t *r1 = src + static_cast<size_t>(std::min(ph - 1u, 2u * y + 1u)) * pw;
            uint32_t *out = dst + static_cast<size_t>(y) * w;
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint32_t x0 = std::min(pw - 1u, 2u * x), x1 = std::min(pw - 1u, 2u * x + 1u);
                const uint32_t a = r0[x0], b = r0[x1], c = r1[x0], d = r1[x1];
                const uint32_t lo = (a & m) + (b & m) + (c & m) + (d & m) + r;
                const uint32_t hi = ((a >> 8) & m) + ((b >> 8) & m) + ((c >> 8) & m) + ((d >> 8) & m) + r;
                out[x] = ((lo >> 2) & m) | (((hi >> 2) & m) << 8);
            }
        }
    }

    // The mip chain (the frame builder's), each level the one before box-filtered (2x2, per channel) - or as made
    // ahead (the large images: off the game's thread). (The levels read in place: a 4K image is not copied on the
    // game's thread; `computed` holds the ones made here.)
    void mipChain(uint32_t width, uint32_t height, const uint32_t *pixels, const std::vector<std::vector<uint32_t>> *made,
                  std::vector<const uint32_t *> &mips, std::vector<std::vector<uint32_t>> &computed)
    {
        const auto t0 = std::chrono::steady_clock::now();
        mips.assign(1, pixels);
        computed.clear();
        computed.reserve(16);
        std::vector<std::pair<uint32_t, uint32_t>> sizes(1, {width, height});
        if (made && !made->empty())
            for (const auto &level : *made)
            {
                const uint32_t w = std::max(1u, sizes.back().first / 2u), h = std::max(1u, sizes.back().second / 2u);
                if (level.size() != static_cast<size_t>(w) * h)
                    break;
                mips.push_back(level.data());
                sizes.push_back({w, h});
            }
        while (sizes.back().first > 1u || sizes.back().second > 1u)
        {
            const uint32_t pw = sizes.back().first, ph = sizes.back().second;
            const uint32_t w = std::max(1u, pw / 2u), h = std::max(1u, ph / 2u);
            std::vector<uint32_t> dst(static_cast<size_t>(w) * h);
            halveLevel(mips.back(), pw, ph, dst.data());
            computed.push_back(std::move(dst));
            mips.push_back(computed.back().data());
            sizes.push_back({w, h});
        }
        g_mkMs[0] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }


    namespace
    {
        using Constants = draw::Constants;               // (the layout the shaders read: nr_draw.h)
        using PictureConstants = draw::PictureConstants; // (a picture's: the shader's b1)
        int qualityPreset() { return backend().qualityPreset(); } // (the backend in use: nr_backend.cpp)
        uint32_t writeMaskOf(uint32_t fbmsk) { return draw::writeMaskOf(fbmsk); } // (nr_draw.h)

        // A mesh as the frame builder keeps it: the device's mesh (its buffers), or where a mesh of this frame's alone went
        // in the Vertices ring; its index count and its box (the core's clipping: a mesh wholly inside needs none).
        struct GpuMesh
        {
            std::weak_ptr<const Mesh> mesh;
            uint32_t meshId = 0; // (the device's mesh - its buffers; 0: the frame's ring, or the GPU grass)
            gfx::RingSpan ringVertices, ringIndices; // (a frame's own mesh: where it went in the Vertices ring)
            uint32_t count = 0;
            float lo[3] = {}, hi[3] = {}; // the vertices' box
            bool bounded = false;
            bool indirect = false; // (the GPU grass: drawn by the device's drawIndirect - its count is the GPU's)
        };
        // (the box, the inside test and the clip runs: nr_draw.h)
        void boundsOf(const Mesh &mesh, GpuMesh &m) { draw::boundsOf(mesh, m.lo, m.hi); }
        // (The frame builder's: the grounds whose grass the device grows on the GPU this frame - drawn by their count.)
        std::unordered_map<uint64_t, GpuMesh> g_grassMeshes;
        bool boxInside(const GpuMesh &m, const float (*clip)[4]) { return draw::boxInside(m.lo, m.hi, clip); }
        using draw::ClipRun;
        using draw::clipRunsOf;

        struct GpuTexture
        {
            std::weak_ptr<const Texture> texture;
            gfx::TextureId id = 0; // (P3 S3: the device's texture; 0 the white one)
            uint64_t largeBytes = 0; // a large image's video memory (g_largeBytes, TODO 94); 0: a game texture
        };
        // The frame builder's state: what it keeps between frames (its meshes, textures and targets by the game's ids and
        // the GS's addresses), and per frame slot what it reads back in frameDone.
        struct State
        {
            static constexpr uint32_t kFrames = 2; // (the device's frame slots)
            std::unordered_map<uint64_t, GpuMesh> meshes;
            std::unordered_map<uint64_t, GpuTexture> textures;
            GpuMesh dynamicMesh; // (a mesh of this frame's alone: where it went in the Vertices ring)
            std::vector<Pass> passes;
            // The GS frame buffers the frame draws into, as render targets (by FBP; the picture's is
            // the main one: g.colour / g.depth, descriptor slot 0).
            // (P3 S3: the frame builder's side - which GS buffer, its GS size; the surfaces behind it are the
            // device's, named by `id` - see DeviceTarget below and nr_device.h.)
            struct Target
            {
                gfx::TargetId id = 0;         // the device's target (0: none - not made, or made in vain)
                uint32_t w = 0, h = 0;            // pixels
                float gsW = 0.0f, gsH = 0.0f; // GS pixels it covers
                bool main = false;
            };
            std::unordered_map<uint32_t, Target> targets;
            uint32_t scaleX = 2, scaleY = 2; // the targets' pixels a GS pixel
            uint32_t scaleYField = 2;        // (N10) a field buffer's rows a GS row: its field fills the picture's height
            uint32_t pictureW = 0, pictureH = 0; // (the frame builder's: the picture's size this frame)
            uint32_t aaW = 0, aaH = 0; // (the anti-aliasing pass's size this frame)
            uint64_t frameIndex = 0;
            // (the frame builder's: the anti-aliasing pass's two time stamps a slot, TF_NATIVE_PROFILE)
            bool aaTimed[kFrames] = {};
            gfx::QueryId aaStamps[kFrames][2] = {};
            // (the frame builder's: its target - made at its first run - and each slot's read-back of it and its tick)
            gfx::TargetId probeId = 0;
            bool probeFailed = false;
            gfx::ReadbackId probeRead[kFrames] = {};
            uint64_t probeTick[kFrames] = {};
            // (TF_NATIVE_PASSPROFILE: a timestamp after every draw, each interval put to the draw's kind.)
            // (each mark a gfx timestamp(); the frame builder's)
            static constexpr uint32_t kPassMarks = 8192u;
            // (TF_NATIVE_DRAWCOST with TF_NATIVE_DRAWSTATS=1: each mark's pipeline statistics too - pixels shaded, triangles
            // rasterised; the queries slow those windows down, so their times are not the frame's.)
            bool statOpen = false;
            bool statsTaken[kFrames] = {}; // (the slot's frame resolved its statistics)
            std::vector<uint8_t> passKinds[kFrames];
            std::vector<gfx::QueryId> passIds[kFrames]; // (each mark's time stamp)
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

        uint64_t g_markBarriers = 0, g_markCopies = 0; // (TF_NATIVE_DRAWCOST: the counts at the last mark)
        uint64_t g_targetsMade = 0;


        // The anti-aliasing pass (TF_NATIVE_AA): -1 not read yet, 0 off, 1 on (the settings panel
        // changes it live). Its cost (TF_NATIVE_PROFILE): the render thread's recording, the GPU's.

        double g_aaCpu = 0.0, g_aaGpu = 0.0;



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
        uint64_t g_clipCulled[2] = {};
        uint64_t g_ringFull = 0; // (TODO 99 check: draws or meshes left out because a frame's ring was full)
        uint64_t g_meshesMade = 0, g_meshVertices = 0, g_ringMeshes = 0, g_pictureReads = 0; // (TF_NATIVE_PROFILE: buffers made, their vertices; meshes in the frame's ring)
        // (TODO 99, a check: TF_NATIVE_PSTATS=1 - on a saved frame, each pass's pipeline statistics (what the GPU's stages took
        // in and let through: triangles into and out of the clipper, pixels shaded) into runs\pstats_<tick>.txt)
        constexpr uint32_t kPStats = 8192u; // (gfx kStatEntriesPerFrame: the device's statistics entries a frame)
        struct PStatNote
        {
            uint32_t draw = 0, pass = 0, source = 0;
        };
        std::vector<PStatNote> g_pstatNotes;
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
        std::unordered_map<uint64_t, bool> g_cutOuts; // (TODO 290 (14): a small texture with clear texels, by id: no stand-in)
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

        // The pixel shader's variants (TODO 94): the paths a draw's constants use, the device's PipelineDesc::variant.
        namespace psv
        {
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

        }

        GpuMesh *upload(const std::shared_ptr<const Mesh> &mesh)
        {
            if (mesh->gpuGrass)
            {
                // (the GPU grass: grown by the device at the frame's start, drawn by drawIndirect - its count the GPU's)
                if (!gfx::device().grassMesh(mesh->id))
                    return nullptr;
                const auto [it, fresh] = g_grassMeshes.try_emplace(mesh->id);
                if (fresh)
                    it->second.indirect = true;
                return &it->second;
            }
            // A small mesh not resident goes into the frame's ring too: the meshes made anew every
            // frame (a buffer of their own each - CreateCommittedResource, about a third of a ms -
            // cost the carrier's second map 37 ms a frame: 110 of them, six vertices each). Copying
            // a few hundred bytes a frame costs nothing. TF_NATIVE_SMALLRING=0: buffers of their own.
            static const bool smallRing = !(std::getenv("TF_NATIVE_SMALLRING") && *std::getenv("TF_NATIVE_SMALLRING") == '0');
            const bool tiny = smallRing && mesh->vertices.size() <= 256u && g.meshes.find(mesh->id) == g.meshes.end();
            if (mesh->immediate || mesh->primitive || mesh->ocean || tiny) // (this frame's alone: in the frame's ring - a buffer of its own each frame cost the ocean's patches 12 ms)
            {
                const uint64_t vb = mesh->vertices.size() * sizeof(Vertex), ib = mesh->indices.size() * sizeof(uint32_t);
                gfx::RingSpan span;
                if (vb == 0 || !gfx::device().ringAlloc(gfx::RingKind::Vertices, static_cast<uint32_t>(vb + ib), 16u, span))
                {
                    g_ringFull += vb != 0 ? 1u : 0u; // (TODO 99 check: the frame's ring full - the mesh not drawn)
                    return nullptr;
                }
                std::memcpy(span.cpu, mesh->vertices.data(), vb);
                std::memcpy(static_cast<uint8_t *>(span.cpu) + vb, mesh->indices.data(), ib);
                g.dynamicMesh.ringVertices = span;
                g.dynamicMesh.ringVertices.size = static_cast<uint32_t>(vb);
                g.dynamicMesh.ringIndices = span;
                g.dynamicMesh.ringIndices.cpu = static_cast<uint8_t *>(span.cpu) + vb;
                g.dynamicMesh.ringIndices.gpu = span.gpu + vb;
                g.dynamicMesh.ringIndices.size = static_cast<uint32_t>(ib);
                g.dynamicMesh.count = static_cast<uint32_t>(mesh->indices.size());
                g.dynamicMesh.bounded = false; // (its box made when a pass needs it)
                ++g_ringMeshes;
                return &g.dynamicMesh;
            }
            auto it = g.meshes.find(mesh->id);
            if (it != g.meshes.end())
                return &it->second;
            if (mesh->vertices.empty() || mesh->indices.empty())
                return nullptr;
            const uint64_t vertexBytes = mesh->vertices.size() * sizeof(Vertex);
            const uint64_t indexBytes = mesh->indices.size() * sizeof(uint32_t);
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
            m.mesh = mesh;
            m.count = static_cast<uint32_t>(mesh->indices.size());
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
                const uint64_t vb = e.vertices.size() * sizeof(Vertex), ib = mesh->indices.size() * sizeof(uint32_t);
                gfx::RingSpan span;
                if (vb == 0 || !gfx::device().ringAlloc(gfx::RingKind::Vertices, static_cast<uint32_t>(vb + ib), 16u, span))
                {
                    g_ringFull += vb != 0 ? 1u : 0u;
                    return nullptr;
                }
                std::memcpy(span.cpu, e.vertices.data(), vb);
                std::memcpy(static_cast<uint8_t *>(span.cpu) + vb, mesh->indices.data(), ib);
                g.dynamicMesh.ringVertices = span;
                g.dynamicMesh.ringVertices.size = static_cast<uint32_t>(vb);
                g.dynamicMesh.ringIndices = span;
                g.dynamicMesh.ringIndices.cpu = static_cast<uint8_t *>(span.cpu) + vb;
                g.dynamicMesh.ringIndices.gpu = span.gpu + vb;
                g.dynamicMesh.ringIndices.size = static_cast<uint32_t>(ib);
                g.dynamicMesh.count = static_cast<uint32_t>(mesh->indices.size());
                g.dynamicMesh.bounded = false;
                ++g_ringMeshes;
                return &g.dynamicMesh;
            }
            if (!e.gpu.meshId)
            {
                const uint64_t vertexBytes = e.vertices.size() * sizeof(Vertex), indexBytes = mesh->indices.size() * sizeof(uint32_t);
                e.gpu.meshId = gfx::device().createMesh(e.vertices.data(), static_cast<uint32_t>(vertexBytes), mesh->indices.data(),
                                                        static_cast<uint32_t>(indexBytes));
                if (!e.gpu.meshId)
                    return nullptr;
                e.gpu.mesh = mesh;
                e.gpu.count = static_cast<uint32_t>(mesh->indices.size());
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
            return g.pictureH > 448u ? std::log2(static_cast<float>(g.pictureH) / 448.0f) : 0.0f;
        }

        void tex1Constants(uint64_t tex1, float *lod, float *lodf)
        {
            draw::tex1Constants(tex1, texSharpShift(), g_texSmooth.load(std::memory_order_relaxed), lod, lodf); // (nr_draw.h)
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
            ++g_frameStandIns;
            return &g_standIn;
        }

        // (TODO 290 (14)) A small texture with clear texels (a HUD piece, a sprite, an effect): never stood in for. The stand-in
        // is one flat colour over the whole quad - its clear parts too: a solid box in the texture's average colour, a frame
        // or two (bbatts's Iris Xe: boxes around the HUD and the Autobot insignia, effects flickering when a warp gate opened;
        // a slower GPU spends its texture budget sooner, so stand-ins come there). Made at once instead (they are small).
        bool noStandIn(const Texture &texture)
        {
            const size_t n = texture.pixels.size();
            if (n == 0u || n > 256u * 256u)
                return false;
            if (g_cutOuts.size() > 8192u)
                g_cutOuts.clear();
            auto [it, fresh] = g_cutOuts.try_emplace(texture.id, false);
            if (fresh)
            {
                const size_t step = std::max<size_t>(1u, n / 4096u);
                size_t clear = 0, count = 0;
                for (size_t i = 0; i < n; i += step, ++count)
                    clear += ((texture.pixels[i] >> 24) & 0xFFu) < 16u;
                it->second = clear * 100u >= count; // (1% of its texels or more clear)
            }
            return it->second;
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
            if (standInOk && !texture->upscaled && g_laterAsked.count(texture->id) && !texture->pixels.empty() && !noStandIn(*texture) &&
                !(kTexBudgetMs > 0.0 && g_frameMakeMs < kTexBudgetMs))
                return standInFor(*texture);
            // (Over the frame's creation budget: to the uploader, the stand-in drawn meanwhile.)
            if (standInOk && !texture->upscaled && kTexBudgetMs > 0.0 && g_frameMakeMs >= kTexBudgetMs &&
                texture->pixels.size() == static_cast<size_t>(texture->width) * texture->height && !texture->pixels.empty() && !noStandIn(*texture) &&
                askLater(texture, true))
                return standInFor(*texture);
            // (A game texture is made here, when first drawn with: it has nothing to stand in for it, and
            // a draw left out while its texture was on the uploader's way dropped whole models for a frame
            // - the HQ's Autobots gone at every menu change, TODO 71. TF_NATIVE_UPLOADGAME=1: on the
            // uploader, its draws left out the frame or two the copy takes - for tests only.)
            static const bool gameOn = std::getenv("TF_NATIVE_UPLOADGAME") && *std::getenv("TF_NATIVE_UPLOADGAME") == '1';
            if (gameOn && !texture->upscaled && texture->pixels.size() > 256u && askLater(texture, true))
                return nullptr;
            if (gfx::device().textureSlotsFree() == 0u || texture->pixels.size() != static_cast<size_t>(texture->width) * texture->height)
            {
                ++g_refused;
                return nullptr;
            }
            // (TODO 152) A palette (256 entries or fewer: the depth fog's, the lens filters') is never put off: it costs
            // nothing to make, and without it the pass is drawn from GS memory or left out - a frame of another colour.
            if (texture->pixels.size() > 256u && g_frameMade > 0 && !noStandIn(*texture) &&
                (g_frameMade >= kTexCap.first || g_frameTexels + texture->pixels.size() > kTexCap.second))
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
                             static_cast<size_t>(gfx::device().textureSlotsFree()));
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
            t.texture = texture;
            // A large image's texels let go of once on the GPU (2026-10-28, TODO 50): kept, a level's
            // thousand 2K images held ~20 GB of RAM for good. Nothing reads them after the upload (the
            // GPU copy lives exactly as long as this texture - prune()), and these are this thread's
            // alone by now (the loader handed it over; the game's side never reads an upscaled one).
            if (texture->upscaled)
            {
                t.largeBytes = dev.textureBytes(t.id);
                g_largeBytes += t.largeBytes;
                Texture &own = const_cast<Texture &>(*texture);
                std::vector<uint32_t>().swap(own.pixels);
                std::vector<std::vector<uint32_t>>().swap(own.mips);
            }
            return &g.textures.emplace(texture->id, t).first->second;
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
                gt.texture = t;
                if (t->upscaled)
                {
                    gt.largeBytes = dev.textureBytes(gt.id);
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


        // ---- The frame builder's side (nr_frame.cpp from sub-step 3) ----
        // A render target for a GS frame buffer (twice its GS size), cleared to transparent black; a grown one
        // keeps its device target (new surfaces, the same views' places).
        bool makeTarget(State::Target &t, float gsW, float gsH, uint32_t sx = 0u, uint32_t sy = 0u)
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
            if (!dev.effect(gfx::Effect::Fxaa) || !picture.id || !aa::sourceFor(g.pictureW, g.pictureH) ||
                !dev.ringAlloc(gfx::RingKind::Constants, kConstantStride, 256u, cs))
                return false;
            g.aaW = g.pictureW;
            g.aaH = g.pictureH;
            gfx::PipelineDesc smaaWeights;
            smaaWeights.effect = gfx::Effect::SmaaWeights;
            const bool smaa = aaMode() == 2 && dev.effect(gfx::Effect::SmaaEdges) && dev.effect(gfx::Effect::SmaaBlend) && aa::smaaFor(g.aaW, g.aaH) &&
                              dev.setPipeline(smaaWeights);
            static const bool profiling = std::getenv("TF_NATIVE_PROFILE") != nullptr;
            const bool timed = profiling && dev.timestamps();
            const uint32_t slot = dev.frameSlot();
            if (timed)
                g.aaStamps[slot][0] = dev.timestamp();
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
                    g.aaStamps[slot][1] = dev.timestamp();
                    g.aaTimed[slot] = g.aaStamps[slot][0] && g.aaStamps[slot][1];
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
                g.aaStamps[slot][1] = dev.timestamp();
                g.aaTimed[slot] = g.aaStamps[slot][0] && g.aaStamps[slot][1];
            }
            return true;
        }
    }

    namespace
    {
        // The sun's occlusion probe (roadmap N3a) recorded: the depth of `scene` - the target the frame's 3D went into -
        // over `rect` (its GS pixels: x0, y0, x1, y1) point-sampled into the 64 x 64 probe target (gfx::Effect::Probe) and
        // read back (frameDone hands it to nr_occlusion). Leaves the probe target bound and the scene's depth readable: the
        // caller binds its own again. False: not run.
        // (TODO 108) The modern bloom, in place of the game's glow (TF_NATIVE_BLOOM=1; off by default: the faithful look).
        // The game's glow is a chain of 2D draws: the picture shrunk into a small effect buffer (128 x 128), a threshold
        // taken off, a pyramid, warps and copies through its ping-pong, then a few additions of the result over the
        // picture (README stage 3, "The glow"). findGlow finds that chain in a frame: the additions are the 2D draws into
        // the picture reading a small buffer drawn only in 2D this frame, added (Cs * C + Cd); the chain is every draw into
        // the buffers those read, and so on back. It counts only when nothing else reads those buffers. Its threshold is
        // the chain's untextured subtraction (Cd - Cs), its strength the additions' factors summed - so a level's own
        // parameters carry over, and a frame with no glow gets no bloom.
        uint64_t g_bloomFrames = 0; // (frames the modern bloom was drawn in)
        struct GlowChain
        {
            bool found = false;
            std::vector<uint8_t> role; // per draw: 0 none, 1 the chain's (left out), 2 an addition (the bloom drawn at the first)
            float threshold[3] = {}, strength[3] = {};
            size_t chain = 0, additions = 0;
            // The chain's vignette: its strips of one grey each (a copy modulated by a colour, a band of the buffer), in the
            // buffer's order - after the chain's turn, top to bottom on the screen. 0: none found (a flat bloom). Each is the
            // grey's own weight (grey / 0x80, the GS's modulation), as the game's glow takes it: the vignette follows the
            // sun down the screen, often only two or three bands lit and none brighter than half (TODO 271).
            float profile[8] = {};
            int profileN = 0;
        };

        uint32_t readKey(uint64_t tex0, bool &ok)
        {
            const uint32_t tbp = static_cast<uint32_t>(tex0) & 0x3FFFu, tbw = static_cast<uint32_t>(tex0 >> 14) & 0x3Fu;
            ok = (tbp % 32u) == 0u;
            return (tbp / 32u) | tbw << 9;
        }

        GlowChain findGlow(const Frame &input, uint32_t mainFbp)
        {
            GlowChain out;
            const size_t n = input.draws.size();
            // (the targets drawn this frame: whether only in 2D, and their width)
            std::unordered_map<uint32_t, bool> only2D;
            std::unordered_map<uint32_t, uint32_t> width;
            for (const Draw &d : input.draws)
            {
                if (depthFormat(d.target) || !d.mesh)
                    continue;
                const uint32_t k = targetKey(d.target);
                auto [it, fresh] = only2D.try_emplace(k, true);
                it->second = it->second && d.mesh->immediate;
                width[k] = std::max(1u, (d.target >> 16) & 0x3Fu) * 64u;
            }
            const auto small = [&](uint32_t k) {
                auto it = only2D.find(k);
                return k != mainFbp && it != only2D.end() && it->second && width[k] <= 128u;
            };
            // A target texture read by a 2D draw (no game texture: the GPU side samples the target).
            const auto reads = [&](const Draw &d, uint32_t &k) {
                bool ok = false;
                if (!d.mesh || !d.mesh->immediate || !(d.mesh->prim & 0x10u) || d.texture || d.fogPalette)
                    return false;
                k = readKey(d.tex0, ok);
                return ok;
            };
            out.role.assign(n, 0u);
            std::unordered_set<uint32_t> buffers;
            for (size_t i = 0; i < n; ++i)
            {
                const Draw &d = input.draws[i];
                uint32_t k = 0;
                const uint64_t a = d.alpha;
                if (targetKey(d.target) != mainFbp || depthFormat(d.target) || !reads(d, k) || !small(k) || !(d.mesh->prim & 0x40u))
                    continue;
                if ((a & 3u) != 0u || ((a >> 2) & 3u) != 2u || ((a >> 6) & 3u) != 1u) // (Cs * C + Cd)
                    continue;
                out.role[i] = 2u;
                buffers.insert(k);
                // (its strength: the vertex colour when modulated, times C - FIX, or the colour's alpha for As)
                const uint32_t c = d.mesh->vertices.empty() ? 0x80808080u : d.mesh->vertices[0].colour;
                const uint32_t cSel = static_cast<uint32_t>(a >> 4) & 3u, tfx = static_cast<uint32_t>(d.tex0 >> 35) & 3u;
                const float factor = cSel == 2u ? static_cast<float>((a >> 32) & 0xFFu) / 128.0f
                                     : cSel == 0u ? static_cast<float>((c >> 24) & 0xFFu) / 128.0f
                                                  : 1.0f;
                for (int ch = 0; ch < 3; ++ch)
                    out.strength[ch] += factor * (tfx == 0u ? static_cast<float>((c >> (8 * ch)) & 0xFFu) / 128.0f : 1.0f);
                ++out.additions;
            }
            if (buffers.empty())
                return out;
            // The chain: back from the buffers the additions read, through the buffers each one's draws read.
            for (bool grew = true; grew;)
            {
                grew = false;
                for (const Draw &d : input.draws)
                {
                    uint32_t k = 0;
                    if (!depthFormat(d.target) && buffers.count(targetKey(d.target)) && reads(d, k) && small(k) && buffers.insert(k).second)
                        grew = true;
                }
            }
            for (size_t i = 0; i < n; ++i)
            {
                const Draw &d = input.draws[i];
                uint32_t k = 0;
                const bool into = !depthFormat(d.target) && buffers.count(targetKey(d.target)) != 0u;
                if (into)
                {
                    out.role[i] = 1u;
                    ++out.chain;
                    // (the threshold: an untextured Cd - Cs, its colour as the GS takes it - 0..255 - times FIX)
                    const uint64_t a = d.alpha;
                    if (d.mesh && !(d.mesh->prim & 0x10u) && (a & 3u) == 2u && ((a >> 2) & 3u) == 0u && ((a >> 4) & 3u) == 2u &&
                        ((a >> 6) & 3u) == 1u && !d.mesh->vertices.empty())
                    {
                        const uint32_t c = d.mesh->vertices[0].colour;
                        const float fix = static_cast<float>((a >> 32) & 0xFFu) / 128.0f;
                        for (int ch = 0; ch < 3; ++ch)
                            out.threshold[ch] = std::min(1.0f, static_cast<float>((c >> (8 * ch)) & 0xFFu) / 255.0f * fix);
                    }
                }
                // (anything else reading the buffers: not the glow as found - left as the game draws it)
                else if (out.role[i] != 2u && reads(d, k) && buffers.count(k))
                    return GlowChain{};
            }
            // (the vignette: the chain's textured copies modulated by a grey other than 0x80, each a narrow band of the
            // buffer - at least four, at most eight of them - their greys in the order of their bands)
            {
                std::vector<std::pair<float, float>> strips; // (band's left edge, weight)
                for (size_t i = 0; i < n; ++i)
                {
                    const Draw &d = input.draws[i];
                    if (out.role[i] != 1u || !d.mesh || !d.mesh->immediate || !(d.mesh->prim & 0x10u) || d.mesh->vertices.empty())
                        continue;
                    const uint32_t c = d.mesh->vertices[0].colour;
                    const uint32_t r = c & 0xFFu, gg = (c >> 8) & 0xFFu, b = (c >> 16) & 0xFFu;
                    if (r != gg || r != b || r == 0x80u || static_cast<uint32_t>(d.tex0 >> 35 & 3u) != 0u)
                        continue;
                    float x0 = 1e9f, x1 = -1e9f;
                    for (const Vertex &v : d.mesh->vertices)
                        x0 = std::min(x0, v.x), x1 = std::max(x1, v.x);
                    if (x1 - x0 > 40.0f)
                        continue;
                    strips.emplace_back(x0, static_cast<float>(r) / 128.0f);
                }
                if (strips.size() >= 4u && strips.size() <= 8u)
                {
                    std::sort(strips.begin(), strips.end());
                    // (TODO 271: not scaled to a mean of 1 any more - with the sun's band alone lit, Deep Amazon's sunset, that
                    // made its two bands 5.7 and 2.3 times the glow, where the game gives them 0.43 and 0.17: a yellow-white
                    // band across the sky with a hard edge; in the other states measured 3 to 27 times the game's glow. The F1 strength slider
                    // stays the way to more.)
                    out.profileN = static_cast<int>(strips.size());
                    for (int k = 0; k < out.profileN; ++k)
                        out.profile[k] = strips[static_cast<size_t>(k)].second;
                }
            }
            out.found = out.additions != 0u;
            return out;
        }

        namespace bloom
        {
            constexpr int kLevels = 6;
            gfx::TargetId level[kLevels] = {};
            uint32_t levelW[kLevels] = {}, levelH[kLevels] = {}, madeW = 0, madeH = 0;
            int count = 0;
            bool failed = false;
            // The levels for a picture of w x h: each half the one above, down to 1/64 or about 16 pixels.
            bool levelsFor(uint32_t w, uint32_t h)
            {
                if (madeW == w && madeH == h && count > 0)
                    return true;
                madeW = madeH = 0;
                count = 0;
                uint32_t lw = w, lh = h;
                for (int i = 0; i < kLevels; ++i)
                {
                    lw = (lw + 1u) / 2u, lh = (lh + 1u) / 2u;
                    if (i > 0 && (lw < 16u || lh < 16u))
                        break;
                    if (!aa::ensure(level[i], lw, lh, gfx::Format::RGBA8))
                        return false;
                    levelW[i] = lw, levelH[i] = lh;
                    count = i + 1;
                }
                madeW = w, madeH = h;
                return count > 0;
            }
        }

        // The modern bloom over the picture as it is (the 3D in, the HUD not yet), with the glow chain's threshold and
        // strength. Leaves the picture bound as the render target, without its depth: the caller binds its own again.
        bool runBloom(State::Target &picture, const GlowChain &glow)
        {
            gfx::Device &dev = gfx::device();
            if (!dev.effect(gfx::Effect::BloomDown) || !dev.effect(gfx::Effect::BloomUp) || !picture.id ||
                !bloom::levelsFor(g.pictureW, g.pictureH))
                return false;
            // (a tuning aid: TF_NATIVE_BLOOM_GAIN scales the strength the game's additions give)
            static const float tune = std::getenv("TF_NATIVE_BLOOM_GAIN") ? static_cast<float>(std::atof(std::getenv("TF_NATIVE_BLOOM_GAIN"))) : 1.0f;
            const int levels = bloom::count;
            const auto pass = [&](gfx::Effect e, gfx::TargetId into, uint32_t iw, uint32_t ih, gfx::TextureId from, uint32_t fw, uint32_t fh,
                                  const float param[4], bool profiled = false) {
                gfx::RingSpan cs;
                if (!dev.ringAlloc(gfx::RingKind::Constants, kConstantStride, 256u, cs))
                    return false;
                float k[16] = {1.0f / static_cast<float>(iw), 1.0f / static_cast<float>(ih), 1.0f / static_cast<float>(fw),
                               1.0f / static_cast<float>(fh), param[0], param[1], param[2], param[3]};
                if (profiled && glow.profileN > 0)
                    std::memcpy(k + 8, glow.profile, sizeof(glow.profile)); // (bloomProfile; its count in bloomShape, after it)
                std::memcpy(cs.cpu, k, sizeof(k));
                const float shape[4] = {profiled ? static_cast<float>(glow.profileN) : 0.0f, 0.0f, 0.0f, 0.0f}; // (bloomShape)
                std::memcpy(static_cast<uint8_t *>(cs.cpu) + sizeof(k), shape, sizeof(shape));
                dev.setTarget(into, false);
                dev.setViewport(0.0f, 0.0f, static_cast<float>(iw), static_cast<float>(ih));
                dev.setScissor(gfx::Rect{0, 0, static_cast<int32_t>(iw), static_cast<int32_t>(ih)});
                gfx::PipelineDesc pd;
                pd.effect = e;
                if (!dev.setPipeline(pd))
                    return false;
                dev.bindDrawConstants(cs);
                dev.bindTextures(from, 0u, 0u);
                dev.draw(3u, 0u);
                return true;
            };
            // Down: the picture into level 0 (the threshold taken off), each level into the next.
            dev.use(picture.id, gfx::UseFor::Sample);
            gfx::TextureId from = dev.view(picture.id);
            uint32_t fw = g.pictureW, fh = g.pictureH;
            // (The F1 slider: 1 the game's glow matched; above it brighter, and the threshold lowered - to 65% at 2 - so more of
            // the bright parts bloom; below it fainter.)
            const float strength = bloomStrength();
            const float lower = 1.0f - 0.35f * std::max(strength - 1.0f, 0.0f);
            for (int i = 0; i < levels; ++i)
            {
                const float param[4] = {glow.threshold[0] * lower, glow.threshold[1] * lower, glow.threshold[2] * lower, i == 0 ? 1.0f : 0.0f};
                dev.use(bloom::level[i], gfx::UseFor::Draw);
                if (!pass(gfx::Effect::BloomDown, bloom::level[i], bloom::levelW[i], bloom::levelH[i], from, fw, fh, param))
                    return false;
                dev.use(bloom::level[i], gfx::UseFor::Sample);
                from = dev.view(bloom::level[i]);
                fw = bloom::levelW[i], fh = bloom::levelH[i];
            }
            // Up: each level enlarged and added into the one above - that level's own content kept at its weight (the blend
            // factor: the fine levels little, the wide ones whole, so the bloom is a soft wash, not a copy of the bright
            // parts) - the first into the picture with the game's strength over the weights' sum.
            // (TF_NATIVE_BLOOM_WEIGHTS=a,b,...: other weights, finest first - a tuning aid)
            static const std::array<float, bloom::kLevels> kWeight = [] {
                std::array<float, bloom::kLevels> w = {0.0f, 0.0f, 0.05f, 0.2f, 0.6f, 1.0f}; // (the wide levels: the game's glow is a wash)
                if (const char *e = std::getenv("TF_NATIVE_BLOOM_WEIGHTS"))
                    for (int i = 0; i < bloom::kLevels && *e; ++i)
                    {
                        char *end = nullptr;
                        w[static_cast<size_t>(i)] = std::strtof(e, &end);
                        if (end == e)
                            break;
                        e = *end == ',' ? end + 1 : end;
                    }
                return w;
            }();
            float weights = 1.0f; // (the deepest level's own)
            for (int i = 0; i + 1 < levels; ++i)
                weights += kWeight[static_cast<size_t>(i)];
            for (int i = levels - 1; i >= 1; --i)
            {
                const float one[4] = {1.0f, 1.0f, 1.0f, 0.0f};
                const float wk = kWeight[static_cast<size_t>(i - 1)];
                const float keep[4] = {wk, wk, wk, wk};
                dev.setBlendFactor(keep);
                dev.use(bloom::level[i - 1], gfx::UseFor::Draw);
                if (!pass(gfx::Effect::BloomUp, bloom::level[i - 1], bloom::levelW[i - 1], bloom::levelH[i - 1], dev.view(bloom::level[i]),
                          bloom::levelW[i], bloom::levelH[i], one))
                    return false;
                dev.use(bloom::level[i - 1], gfx::UseFor::Sample);
            }
            const float share = tune * strength / weights;
            const float gain[4] = {glow.strength[0] * share, glow.strength[1] * share, glow.strength[2] * share, 0.0f};
            const float whole[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            dev.setBlendFactor(whole); // (the picture kept as it is, the bloom added)
            dev.use(picture.id, gfx::UseFor::Draw);
            const bool done = pass(gfx::Effect::BloomUp, picture.id, g.pictureW, g.pictureH, dev.view(bloom::level[0]), bloom::levelW[0],
                                   bloom::levelH[0], gain, true);
            dev.bindTextures(0u, 0u, 0u);
            return done;
        }

        // (TODO 108 (2)) The filmic tone mapping over the picture as it is (the 3D in, the HUD not yet): the picture's copy
        // through gfx::Effect::ToneMap back into it, its alpha kept. The exposure keeps a dark grey (sRGB 0.30) where it was.
        // Leaves the picture bound as the render target, without its depth: the caller binds its own again. False: not run.
        bool runTone(State::Target &picture)
        {
            gfx::Device &dev = gfx::device();
            gfx::RingSpan cs;
            if (!dev.effect(gfx::Effect::ToneMap) || !picture.id || !dev.ringAlloc(gfx::RingKind::Constants, kConstantStride, 256u, cs))
                return false;
            // (TF_NATIVE_TONE_K, TF_NATIVE_TONE_STRENGTH: tuning aids)
            static const float k = std::getenv("TF_NATIVE_TONE_K") ? static_cast<float>(std::atof(std::getenv("TF_NATIVE_TONE_K"))) : 0.8f;
            const float strength = toneStrength(); // (the F1 slider, TF_NATIVE_TONE_STRENGTH)
            static const float exposure = [] {
                const auto aces = [](double x) { return (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14); };
                // (the anchor: an sRGB level the curve keeps where it is - 0.30, a dark grey: this game's pictures are mostly
                // darker than mid-grey, and anchored there the shadows keep their brightness while the brighter half gains
                // contrast; TF_NATIVE_TONE_ANCHOR: another)
                const double a = std::getenv("TF_NATIVE_TONE_ANCHOR") ? std::atof(std::getenv("TF_NATIVE_TONE_ANCHOR")) : 0.30;
                const double grey = a <= 0.04045 ? a / 12.92 : std::pow((a + 0.055) / 1.055, 2.4);
                const double hdr = grey / std::max(1.0 - static_cast<double>(k) * grey, 1.0 - static_cast<double>(k));
                double lo = 0.01, hi = 10.0;
                for (int i = 0; i < 60; ++i)
                {
                    const double e = 0.5 * (lo + hi);
                    (aces(hdr * e) < grey ? lo : hi) = e;
                }
                return static_cast<float>(0.5 * (lo + hi));
            }();
            const gfx::TextureId copy = dev.snapshot(picture.id);
            if (!copy)
                return false;
            const float p[4] = {exposure, k, strength, 0.3f * strength}; // (saturation: up to +30% at the whole curve)
            std::memcpy(cs.cpu, p, sizeof(p));
            dev.use(picture.id, gfx::UseFor::Draw);
            dev.setTarget(picture.id, false);
            dev.setViewport(0.0f, 0.0f, static_cast<float>(g.pictureW), static_cast<float>(g.pictureH));
            dev.setScissor(gfx::Rect{0, 0, static_cast<int32_t>(g.pictureW), static_cast<int32_t>(g.pictureH)});
            gfx::PipelineDesc pd;
            pd.effect = gfx::Effect::ToneMap;
            if (!dev.setPipeline(pd))
                return false;
            dev.bindDrawConstants(cs);
            dev.bindTextures(copy, 0u, 0u);
            dev.draw(3u, 0u);
            dev.bindTextures(0u, 0u, 0u);
            return true;
        }

        // (TODO 108 (3) C1) The local contrast over the picture as it is (the 3D in, the HUD not yet): a wide blur of the picture
        // made with the bloom's steps and levels (from the picture straight to 1/4, down to 1/16, no threshold; up again to
        // 1/4, each step replacing the level above - the blend factor 0), then the picture's copy and that blur through gfx::Effect::LocalContrast back into the picture.
        // Leaves the picture bound as the render target, without its depth: the caller binds its own again. False: not run.
        bool runClarity(State::Target &picture)
        {
            gfx::Device &dev = gfx::device();
            constexpr int kDeep = 4; // (levels 0..3: 1/2 to 1/16 - a blur about a sixtieth of the picture wide)
            if (!dev.effect(gfx::Effect::LocalContrast) || !dev.effect(gfx::Effect::BloomDown) || !dev.effect(gfx::Effect::BloomUp) || !picture.id ||
                !bloom::levelsFor(g.pictureW, g.pictureH) || bloom::count < kDeep)
                return false;
            const auto pass = [&](gfx::Effect e, gfx::TargetId into, uint32_t iw, uint32_t ih, gfx::TextureId t0, gfx::TextureId t1, const float *k,
                                  size_t floats) {
                gfx::RingSpan cs;
                if (!dev.ringAlloc(gfx::RingKind::Constants, kConstantStride, 256u, cs))
                    return false;
                std::memset(cs.cpu, 0, 128u);
                std::memcpy(cs.cpu, k, floats * sizeof(float));
                dev.setTarget(into, false);
                dev.setViewport(0.0f, 0.0f, static_cast<float>(iw), static_cast<float>(ih));
                dev.setScissor(gfx::Rect{0, 0, static_cast<int32_t>(iw), static_cast<int32_t>(ih)});
                gfx::PipelineDesc pd;
                pd.effect = e;
                if (!dev.setPipeline(pd))
                    return false;
                dev.bindDrawConstants(cs);
                dev.bindTextures(t0, t1, 0u);
                dev.draw(3u, 0u);
                return true;
            };
            // Down: the picture straight into level 1 (a quarter: the half-size step skipped - the blur is wide, and that step
            // was the dearest), each level into the next (no threshold).
            dev.use(picture.id, gfx::UseFor::Sample);
            gfx::TextureId from = dev.view(picture.id);
            uint32_t fw = g.pictureW, fh = g.pictureH;
            for (int i = 1; i < kDeep; ++i)
            {
                const float k[8] = {1.0f / static_cast<float>(bloom::levelW[i]), 1.0f / static_cast<float>(bloom::levelH[i]), 1.0f / static_cast<float>(fw),
                                    1.0f / static_cast<float>(fh), 0.0f, 0.0f, 0.0f, 0.0f};
                dev.use(bloom::level[i], gfx::UseFor::Draw);
                if (!pass(gfx::Effect::BloomDown, bloom::level[i], bloom::levelW[i], bloom::levelH[i], from, 0u, k, 8u))
                    return false;
                dev.use(bloom::level[i], gfx::UseFor::Sample);
                from = dev.view(bloom::level[i]);
                fw = bloom::levelW[i], fh = bloom::levelH[i];
            }
            // Up: each level enlarged into the one above, in its place (the blend factor 0: the level's own content dropped).
            const float none[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            dev.setBlendFactor(none);
            for (int i = kDeep - 1; i >= 2; --i)
            {
                const float k[8] = {1.0f / static_cast<float>(bloom::levelW[i - 1]), 1.0f / static_cast<float>(bloom::levelH[i - 1]),
                                    1.0f / static_cast<float>(bloom::levelW[i]), 1.0f / static_cast<float>(bloom::levelH[i]), 1.0f, 1.0f, 1.0f, 0.0f};
                dev.use(bloom::level[i - 1], gfx::UseFor::Draw);
                if (!pass(gfx::Effect::BloomUp, bloom::level[i - 1], bloom::levelW[i - 1], bloom::levelH[i - 1], dev.view(bloom::level[i]), 0u, k, 8u))
                    return false;
                dev.use(bloom::level[i - 1], gfx::UseFor::Sample);
            }
            const float whole[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            dev.setBlendFactor(whole);
            // The picture: its copy and the blur (level 1, a quarter of the picture) into it.
            const gfx::TextureId copy = dev.snapshot(picture.id);
            if (!copy)
                return false;
            dev.use(picture.id, gfx::UseFor::Draw);
            const float k[4] = {1.0f / static_cast<float>(g.pictureW), 1.0f / static_cast<float>(g.pictureH), 1.2f * clarityStrength(), 0.0f};
            const bool done = pass(gfx::Effect::LocalContrast, picture.id, g.pictureW, g.pictureH, copy, dev.view(bloom::level[1]), k, 4u);
            dev.bindTextures(0u, 0u, 0u);
            return done;
        }

        bool runProbe(State::Target &scene, const float rect[4], uint64_t tick)
        {
            gfx::Device &dev = gfx::device();
            const gfx::TextureId sceneDepth = scene.id ? dev.view(scene.id, true) : 0u;
            if (!g.probeId && !g.probeFailed)
            {
                // (its target, made at its first run: one value a pixel, no depth)
                gfx::TargetDesc d;
                d.width = d.height = 64u;
                d.colour = gfx::Format::R32Float;
                d.depth = false;
                g.probeId = dev.createTarget(d);
                g.probeFailed = !g.probeId;
                if (g.probeFailed)
                    std::fprintf(stderr, "[native render] the sun's occlusion probe has no target: the game reads the GS's\n");
            }
            gfx::RingSpan cs;
            if (!g.probeId || !scene.id || !sceneDepth || scene.gsW <= 0.0f || scene.gsH <= 0.0f ||
                !dev.ringAlloc(gfx::RingKind::Constants, kConstantStride, 256u, cs))
                return false;
            dev.useDepth(scene.id, gfx::DepthUse::ReadSample);
            dev.use(g.probeId, gfx::UseFor::Draw);
            dev.setTarget(g.probeId, false);
            dev.setViewport(0.0f, 0.0f, 64.0f, 64.0f);
            dev.setScissor(gfx::Rect{0, 0, 64, 64});
            gfx::PipelineDesc pd;
            pd.effect = gfx::Effect::Probe;
            if (!dev.setPipeline(pd))
                return false;
            const float sx = static_cast<float>(scene.w) / scene.gsW, sy = static_cast<float>(scene.h) / scene.gsH;
            const float k[8] = {rect[0] * sx, rect[1] * sy, (rect[2] - rect[0]) * sx / 64.0f, (rect[3] - rect[1]) * sy / 64.0f,
                                static_cast<float>(scene.w), static_cast<float>(scene.h), 0.0f, 0.0f};
            std::memcpy(cs.cpu, k, sizeof(k));
            dev.bindDrawConstants(cs);
            dev.bindTextures(0u, gfx::kKeepTexture, sceneDepth);
            dev.draw(3u, 0u);
            const uint32_t slot = dev.frameSlot();
            if (g.probeRead[slot])
                dev.releaseReadback(g.probeRead[slot]); // (one a frame: a stale one let go of)
            g.probeRead[slot] = dev.readTarget(g.probeId);
            g.probeTick[slot] = tick;
            return g.probeRead[slot] != 0;
        }

        // The frame builder's part of a slot's frame done on the GPU (the device calls it, gfx setFrameDone): the sun's
        // probe to the game's read-back, the profile's time stamps and statistics.
        void frameDone(uint32_t slot)
        {
            gfx::Device &dev = gfx::device();
            if (const gfx::ReadbackId r = g.probeRead[slot])
            {
                // (The sun's occlusion probe of the slot's frame, its GPU work done: to the game's read-back, roadmap N3a.)
                g.probeRead[slot] = 0;
                gfx::Readback rb;
                if (dev.readback(r, rb) && rb.width >= 64u && rb.height >= 64u)
                {
                    if (rb.rowBytes == 64u * sizeof(float))
                        occlusion::deliver(g.probeTick[slot], static_cast<const float *>(rb.data));
                    else
                    {
                        static std::vector<float> rows(64u * 64u);
                        for (uint32_t y = 0; y < 64u; ++y)
                            std::memcpy(rows.data() + y * 64u, static_cast<const uint8_t *>(rb.data) + static_cast<size_t>(y) * rb.rowBytes, 64u * sizeof(float));
                        occlusion::deliver(g.probeTick[slot], rows.data());
                    }
                }
                dev.releaseReadback(r);
            }
            if (!g.aaTimed[slot] && g.passKinds[slot].empty())
                return;
            static std::vector<uint64_t> ticks;
            uint64_t hz = 0;
            const bool stamps = dev.readTimestamps(slot, ticks, hz) && hz;
            auto tickOf = [&](gfx::QueryId q) { return q && q <= ticks.size() ? ticks[q - 1u] : 0ull; };
            if (g.aaTimed[slot])
            {
                // (The anti-aliasing pass's two timestamps, TF_NATIVE_PROFILE.)
                g.aaTimed[slot] = false;
                const uint64_t t0 = tickOf(g.aaStamps[slot][0]), t1 = tickOf(g.aaStamps[slot][1]);
                if (stamps && t0 && t1 > t0)
                {
                    g_aaGpu += static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(hz);
                    ++g_aaGpuFrames;
                }
            }
            if (!g.passKinds[slot].empty())
            {
                const uint32_t n = static_cast<uint32_t>(g.passKinds[slot].size());
                if (stamps)
                {
                    static std::vector<uint64_t> marks;
                    marks.assign(n, 0ull);
                    for (uint32_t i = 0; i < n && i < g.passIds[slot].size(); ++i)
                        marks[i] = tickOf(g.passIds[slot][i]);
                    const uint64_t *t = marks.data();
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
                            for (uint32_t i = 1; i < n; ++i)
                                if (t[i - 1] && t[i] >= t[i - 1])
                                {
                                    g_drawCostSum[i] += static_cast<double>(t[i] - t[i - 1]) * 1000.0 / static_cast<double>(hz);
                                    ++g_drawCostN[i];
                                }
                            ++g_drawCostN[0];
                            static std::vector<gfx::PipelineStats> st;
                            if (g.statsTaken[slot] && dev.readStats(slot, st))
                            {
                                g_drawPixels.resize(n, 0.0);
                                g_drawPrims.resize(n, 0.0);
                                for (uint32_t i = 0; i < n && i < st.size(); ++i)
                                {
                                    g_drawPixels[i] += static_cast<double>(st[i].psInvocations);
                                    g_drawPrims[i] += static_cast<double>(st[i].clipPrimitives);
                                }
                                ++g_drawStatFrames;
                            }
                            g_drawCostNote.resize(n);
                            for (uint32_t i = 0; i < n && i < g.passNotes[slot].size(); ++i)
                                g_drawCostNote[i] = {g.passKinds[slot][i], g.passNotes[slot][i]};
                        }
                    }
                    for (uint32_t i = 1; i < n; ++i)
                        if (t[i - 1] && t[i] >= t[i - 1] && g.passKinds[slot][i] < kPassKinds)
                        {
                            const double ms = static_cast<double>(t[i] - t[i - 1]) * 1000.0 / static_cast<double>(hz);
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
                }
                g.passKinds[slot].clear();
                g.passIds[slot].clear();
                g.passNotes[slot].clear();
            }
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
            if (!gfx::device().effect(gfx::Effect::Cloud) || !draw.cloud || !tg.id || tg.gsW <= 0.0f || tg.gsH <= 0.0f)
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
                const uint32_t count = std::min<uint32_t>(set.count, static_cast<uint32_t>(set.points->vertices.size()));
                dev.draw(4u, 0u, count, 0u);
                drawn = true;
            }
            return drawn; // (the triangle lists set again with the next pipeline)
        }
    }


    // Game textures made ahead (the capture's prefetch, TODO 76/77): handed to the uploader at the next
    // frame's start, so a screen's textures are on the GPU before a frame draws with them - made on the
    // render thread when first drawn, an HQ screen change made ~50 of them in one frame (60-180 ms).
    namespace
    {
        std::mutex g_preUploadMutex;
        std::vector<std::weak_ptr<const Texture>> g_preUploads;
    }
    void preUpload(const std::shared_ptr<const Texture> &texture)
    {
        std::lock_guard<std::mutex> lock(g_preUploadMutex);
        g_preUploads.push_back(texture);
    }


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


    void render(const Frame &input, const std::string &shotPath)
    {
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
        // As many lines as the picture shows in the window: the window's lines, or its width over the picture's shape
        // in a narrower, letterboxed window (a 16:9 picture on the Steam Deck's 16:10 screen). The extra lines were
        // drawn and then scaled away (the Deck: 1424x801 for 1280x720 shown). As wide as the picture shows on the
        // screen (the game's widescreen picture is anamorphic). TF_NATIVE_HEIGHT=<n> fixes the lines (above the window's
        // = supersampling).
        static const int linesFixed = [] {
            const char *v = std::getenv("TF_NATIVE_HEIGHT");
            return v && *v ? std::atoi(v) : 0;
        }();
        // (the render scale: that share of the window's size, the picture shown enlarged - setRenderScale; 100 unless set)
        const int scale = renderScale();
        const int window = g_windowLines.load(std::memory_order_relaxed) * scale / 100;
#if defined(__linux__)
        const int windowWidth = g_windowWidth.load(std::memory_order_relaxed) * scale / 100;
#endif
        // (TODO 255) A movie shown as a band of the picture's rows (ps2_host_display::setPictureBand): the picture as
        // many lines taller as the band is short of it, so the band itself still has the window's lines.
        float bandTop = 0.0f, bandBottom = 1.0f;
        ps2_host_display::pictureBand(bandTop, bandBottom);
        const float bandLines = linesFixed > 0 ? 1.0f : 1.0f / std::max(0.25f, bandBottom - bandTop);
        const float shape = input.displayAspect > 0.5f ? input.displayAspect : 4.0f / 3.0f;
        const float tall = (window > 0 ? window : 1080) * bandLines;
#if defined(__linux__)
        // (Linux: the window's lines, or its width over the picture's shape in a narrower, letterboxed window - a 16:9
        // picture on the Steam Deck's 16:10 screen - the extra lines would be drawn and then scaled away.)
        const float lines = linesFixed > 0 ? static_cast<float>(linesFixed) :
            windowWidth > 0 ? std::min(tall, windowWidth / shape) : tall;
#else
        const float lines = linesFixed > 0 ? static_cast<float>(linesFixed) : tall;
#endif
        const uint32_t linesWanted = static_cast<uint32_t>(
            std::clamp(static_cast<int>(std::lround(lines)), 240, 4320));
        const uint32_t height = linesWanted, width = (static_cast<uint32_t>(std::lround(linesWanted * shape)) + 7u) & ~7u;
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
        const uint32_t ssW = ssaa ? (width * 3u + 1u) / 2u : width, ssH = ssaa ? (height * 3u + 1u) / 2u : height;
        g.scaleX = std::max(2u, (ssW + 511u) / 512u);
        g.scaleY = std::max(2u, (ssH + 447u) / 448u);
        // (Roadmap N10) The scene buffer's 3D is drawn once, into field A's 224 rows, and copied over the whole
        // picture: at the picture's scale (lines / 448) that field held half the picture's rows, enlarged x2 down by
        // the copy - every edge's steps two lines tall, the 3D softer up and down than across. A buffer a field copy
        // reads (SCANMSK) gets twice the rows: the field at the picture's own resolution, copied 1:1. (Kept under
        // 8192 rows.) TF_NATIVE_FIELDRES=0: as before.
        static const bool fieldResOff = std::getenv("TF_NATIVE_FIELDRES") && *std::getenv("TF_NATIVE_FIELDRES") == '0';
        g.scaleYField = fieldResOff ? g.scaleY : std::max(g.scaleY, std::min(18u, (ssH + 223u) / 224u));
        static const bool frameDoneSet = [] {
            gfx::device().setFrameDone(frameDone);
            return true;
        }();
        (void)frameDoneSet;
        gfx::Device &dev = gfx::device();
        if (!dev.prepareFrame(width, height))
            return;
        g.pictureW = width;
        g.pictureH = height;
        prune();
        dev.beginFrame(input.tick); // (the picture's start time stamp in it: before the grass's reads now, not after - P3 S3)
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
        g_grassMeshes.clear();
        dev.growGrass(input); // (the GPU grass: the frame's blades grown by the device's own pass, before its first picture)
        // (TF_NATIVE_PASSPROFILE: a timestamp now and after each draw; each interval is the next mark's kind.)
        static const bool passProfileOn = std::getenv("TF_NATIVE_PASSPROFILE") != nullptr;
        static const bool drawStatsOn = passProfileOn && std::getenv("TF_NATIVE_DRAWCOST") && std::getenv("TF_NATIVE_DRAWSTATS");
        const bool passProfile = passProfileOn && dev.timestamps();
        const bool statsNow = passProfile && drawStatsOn && drawCostWindow(g_profWindow) && dev.pipelineStats();
        g.statOpen = false;
        auto passMark = [&](uint8_t kind) {
            const uint32_t slot = dev.frameSlot();
            if (passProfile && g.passKinds[slot].size() < State::kPassMarks)
            {
                if (statsNow)
                {
                    const uint32_t entry = static_cast<uint32_t>(g.passKinds[slot].size());
                    if (!g.statOpen)
                        dev.beginStats(entry);
                    dev.endStats(entry);
                    g.statOpen = false;
                }
                g.passIds[slot].push_back(dev.timestamp());
                g.passKinds[slot].push_back(kind);
                g.passNotes[slot].emplace_back();
            }
        };
        g.passKinds[gfx::device().frameSlot()].clear();
        g.passIds[gfx::device().frameSlot()].clear();
        g.passNotes[gfx::device().frameSlot()].clear();
        passMark(255u);
        // The main target: the picture's resources.
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
            if (!m.main || !m.id || m.w != width || m.h != height)
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
                const uint32_t sx = unit ? 1u : g.scaleX, sy = unit ? 1u : fieldRead.count(e.first) ? g.scaleYField : g.scaleY;
                if (t.id && t.gsW >= e.second.first && t.gsH >= e.second.second && t.w == static_cast<uint32_t>(t.gsW) * sx &&
                    t.h == static_cast<uint32_t>(t.gsH) * sy)
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
        State::Target *current = nullptr;
        bool currentFog = false;
        // The anti-aliasing pass (TF_NATIVE_AA): run once the picture holds the frame's 3D - drawn
        // into it, or copied in from a target holding it (the scene buffer's field copies, the glow
        // made from the picture) - just before the first draw into the picture that carries none:
        // the HUD, menus and text are drawn after it, as sharp as before. At the frame's end if no
        // such draw came; never on a picture with no 3D (the front end's menus).
        const bool aaWanted = dev.effect(gfx::Effect::Fxaa) && aaOn();
        bool aaDone = false, picture3D = false;
        // (TODO 108 (2): the tone mapping, at the same place - first, the anti-aliasing then smooths the mapped picture)
        const bool toneWanted = toneMode() != 0 && dev.effect(gfx::Effect::ToneMap);
        bool toneDone = false;
        // (TODO 108 C1: the local contrast, at the same place - before the tone mapping, on the picture as drawn)
        const bool clarityWanted = clarityMode() != 0 && clarityStrength() > 0.0f && dev.effect(gfx::Effect::LocalContrast);
        bool clarityDone = false;
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
        size_t drawn = 0, fromTargets = 0, left = 0, topReads = 0;
        struct AfterSave
        {
            size_t index = 0;
            gfx::ReadbackId read = 0;
            uint32_t w = 0, h = 0;
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
            if (input.pictures.size() > 1u && skipOn && dev.predication())
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
                dev.predicate(on);
                predicated = on;
            }
        };
        passMark(7u);
        // (TODO 108) The modern bloom in place of the game's glow, when chosen and the frame has a glow (findGlow): its chain left
        // out, the bloom drawn where its first addition was.
        // (not in a frame left out: its picture is not drawn)
        const GlowChain glow = bloomMode() != 0 && !(input.dropped && !input.droppedDrawn) ? findGlow(input, mainFbp) : GlowChain{};
        bool bloomDone = false;
        {
            static const bool bloomLog = std::getenv("TF_NATIVE_BLOOMLOG") != nullptr;
            if (bloomLog && !shotPath.empty() && bloomMode() != 0)
                std::fprintf(stderr, "[native render] bloom: glow %s (%zu chain draws, %zu additions), threshold %.3f %.3f %.3f, strength %.3f %.3f %.3f\n",
                             glow.found ? "found" : "none", glow.chain, glow.additions, glow.threshold[0], glow.threshold[1], glow.threshold[2],
                             glow.strength[0], glow.strength[1], glow.strength[2]);
            if (bloomLog && !shotPath.empty() && glow.profileN)
            {
                std::string p;
                for (int k = 0; k < glow.profileN; ++k)
                    p += " " + std::to_string(glow.profile[k]).substr(0, 4);
                std::fprintf(stderr, "[native render] bloom: the glow's profile down the screen:%s\n", p.c_str());
            }
        }
        for (const Draw &draw : input.draws)
        {
            if (glow.found)
            {
                const uint8_t role = glow.role[static_cast<size_t>(&draw - input.draws.data())];
                if (role == 1u)
                    continue;
                if (role == 2u)
                {
                    if (!bloomDone)
                    {
                        bloomDone = true;
                        if (auto pic = g.targets.find(mainFbp); bloomMode() == 1 && pic != g.targets.end() && pic->second.id && runBloom(pic->second, glow))
                            ++g_bloomFrames;
                        current = nullptr; // (its own targets and pipelines: the next draw binds its target, and with it all, again)
                    }
                    continue;
                }
            }
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
            if (m && draw.meshB && draw.meshB->vertices.size() == draw.mesh->vertices.size())
            {
                const uint64_t bytes = draw.meshB->vertices.size() * kVertexBStride;
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
                uint32_t selfSlot = 0u; // (a target sampling itself: its copy's view)
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
                    // (TODO 152: the fog left out - a frame of another colour; told as the lens filter's is)
                    static uint64_t fogMissed = 0;
                    if (fog && (fogMissed < 10u || fogMissed % 300u == 0u))
                        std::fprintf(stderr, "[native render] warning: the depth fog at tick %llu left out (its palette not made) - %llu so far\n",
                                     static_cast<unsigned long long>(input.tick), static_cast<unsigned long long>(fogMissed + 1u));
                    fogMissed += fog ? 1u : 0u;
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
                    {
                        if ((topPalette = textureFor(pass.topPalette)) != nullptr && (selfSlot = dev.snapshot(tg.id)) != 0u)
                        {
                            topSource = source = &tg;
                            t = nullptr;
                            ++topReads;
                        }
                        else
                        {
                            // (TODO 152: the grade then reads GS memory - the frame's 3D bluish-white. Told, the first
                            // ten times and then every 300th, so a frame like that names itself in the log.)
                            static uint64_t lensMissed = 0;
                            if (lensMissed < 10u || lensMissed % 300u == 0u)
                                std::fprintf(stderr, "[native render] warning: the lens filter at tick %llu drawn from GS memory (its palette %s, the target's copy %s) - %llu so far\n",
                                             static_cast<unsigned long long>(input.tick), topPalette ? "made" : "not made",
                                             selfSlot ? "made" : "not made", static_cast<unsigned long long>(lensMissed + 1u));
                            ++lensMissed;
                        }
                    }
                }
                // (Debug, TODO 108 S1: TF_NATIVE_NOSHADOWREAD=1 - the draws that read another target's top byte through a palette
                // left out: the shadow receivers - the player's shadow, the trees' and blob shadows on the ground. Not the lens
                // filter, which reads its own target.)
                static const bool noShadowRead = std::getenv("TF_NATIVE_NOSHADOWREAD") && *std::getenv("TF_NATIVE_NOSHADOWREAD") == '1';
                if (noShadowRead && topSource && topSource != &tg)
                {
                    ++left;
                    continue;
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
                                 pass.texture->format, t ? t->id : 9999u, standInColour[0], standInColour[1], standInColour[2], standInColour[3]);
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
                    if (clarityWanted && !clarityDone && tg.main && picture3D && !carries3D)
                    {
                        if (runClarity(tg))
                            current = nullptr; // (the draw's target, depth and viewport bound again)
                        clarityDone = true;
                    }
                    if (toneWanted && !toneDone && tg.main && picture3D && !carries3D)
                    {
                        if (runTone(tg))
                            current = nullptr; // (the draw's target, depth and viewport bound again)
                        toneDone = true;
                    }
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
                if (!halfPixelUnitOnly || (tg.w == static_cast<uint32_t>(tg.gsW) && tg.h == static_cast<uint32_t>(tg.gsH)))
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
                if (ssaa && source && !topSource && !selfSlot && !fog && tg.main && source->w > static_cast<uint32_t>(source->gsW))
                    c.down[0] = 1.0f;
                // (N10, TF_NATIVE_BOXDOWN=1, off by default) A shrink of the picture itself into a target at the GS's own
                // size (the glow's first copy): the average over the footprint (the shader's boxdown). Measured (other PC,
                // 3440x1440): the glow changes by at most 9 levels in Deep Amazon (its 8 x 8 pyramid already averages the
                // single taps away), no shimmer to take out on Amazon, Deep Amazon or the HQ, and +0.2-0.3 ms of the GPU a
                // frame - so off; kept for an A/B.
                {
                    static const bool boxOn = std::getenv("TF_NATIVE_BOXDOWN") && *std::getenv("TF_NATIVE_BOXDOWN") == '1';
                    if (boxOn && source && source->main && !topSource && !selfSlot && !fog && c.down[0] < 0.5f && tg.w == static_cast<uint32_t>(tg.gsW) &&
                        tg.h == static_cast<uint32_t>(tg.gsH) && (source->w >= static_cast<uint32_t>(source->gsW) * 2u || source->h >= static_cast<uint32_t>(source->gsH) * 2u))
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
                static const bool clipGsPresetOff = qualityPreset() < 2 || dev.vendorId() == 0x1002u;
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
                uint32_t firstInst = 0u;
                uint32_t instCount = 1u;
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
                uint32_t drawCount = m->count;
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
                        pass.mode[2] < 0.5f && pass.mode[3] < 0.5f && draw.mesh && draw.mesh->indices.size() == m->count &&
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
                            const uint64_t bytes = kept.size() * sizeof(uint32_t);
                            if (dev.ringAlloc(gfx::RingKind::Vertices, static_cast<uint32_t>(bytes), 16u, keptSpan))
                            {
                                std::memcpy(keptSpan.cpu, kept.data(), static_cast<size_t>(bytes));
                                keptView = true;
                                drawCount = static_cast<uint32_t>(kept.size());
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
                            sigF = std::fopen(("runs/sig_" + std::to_string(input.tick) + ".txt").c_str(), "w");
                        }
                        if (sigF && draw.mesh)
                        {
                            double vs = 0.0;
                            for (const Vertex &v : draw.mesh->vertices)
                                vs += v.x * 1.0 + v.y * 3.0 + v.z * 7.0 + v.u * 11.0 + v.v * 13.0 + (v.colour & 0xFFFFu);
                            const char *stale = ""; // (P3 S3: the mesh's GPU copy is the device's now - not compared)
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
                                         static_cast<unsigned long long>(pass.texture ? pass.texture->id : 0u), t ? static_cast<int>(t->id) : -1,
                                         standIn ? 1 : 0, firstInst, instCount, clips ? 1 : 0, clipGs ? 1 : 0,
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
                if (statsNow && !g.statOpen && g.passKinds[gfx::device().frameSlot()].size() < State::kPassMarks)
                {
                    dev.beginStats(static_cast<uint32_t>(g.passKinds[gfx::device().frameSlot()].size()));
                    g.statOpen = true;
                }
                static const bool pstatOn = std::getenv("TF_NATIVE_PSTATS") && *std::getenv("TF_NATIVE_PSTATS") == '1';
                const bool pstatHere = pstatOn && !shotPath.empty() && !statsNow && g_pstatNotes.size() < kPStats && dev.pipelineStats();
                if (pstatHere)
                {
                    dev.beginStats(static_cast<uint32_t>(g_pstatNotes.size()));
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
                    dev.endStats(static_cast<uint32_t>(g_pstatNotes.size() - 1u));
                ++drawn;
                drawnTargets.insert(found->first);
                if (passProfile)
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
                    if (!g.passNotes[gfx::device().frameSlot()].empty())
                        g.passNotes[gfx::device().frameSlot()].back() = nt;
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
                if ((sv.read = dev.readTarget(tg.id)) != 0)
                    afterSaves.push_back(sv);
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
                    if ((s.read = dev.readTarget(tg.id)) != 0)
                        afterSaves.push_back(s);
                }
            }
        }
        predicate(false);
        // (Debug: a target saved beside the picture, TF_NATIVE_SAVE_TARGET=<key in hex>.)
        gfx::ReadbackId savedRead = 0;
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
                savedRead = dev.readTarget(saved->id);
            }
        }
        if (!shotPath.empty())
            std::fprintf(stderr, "[native render] picture: target %03x (%.0fx%.0f), %zu targets, %zu passes drawn, %zu from targets (%zu top-byte reads), %zu left out\n",
                     mainFbp, mainW, mainH, g.targets.size(), drawn, fromTargets, topReads, left);
        if (!shotPath.empty())
            // (TODO 99 check: the frame's rings - used of their room - what was left out for want of room so far, and the
            // views rewritten under a frame's use so far)
        {
            dev.report(gfx::Report::Rings);
            std::fprintf(stderr, "[native render] rings: full %llu; clipping triangles left out %llu, passes %llu\n", static_cast<unsigned long long>(g_ringFull),
                         static_cast<unsigned long long>(g_clipCulled[0]), static_cast<unsigned long long>(g_clipCulled[1]));
        }
        probe(false);
        if (clarityWanted && !clarityDone && picture3D)
        {
            runClarity(picture); // (no 2D over the 3D this frame: over the whole picture now)
            clarityDone = true;
        }
        if (toneWanted && !toneDone && picture3D)
        {
            runTone(picture); // (no 2D over the 3D this frame: over the whole picture now)
            toneDone = true;
        }
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
                             !aaOn() ? "off" : !dev.effect(gfx::Effect::Fxaa) ? "no pipeline" : picture3D ? "the pass failed" : "no 3D in the picture");
        }
        // (Debug: TF_NATIVE_AASAVE=1: on a saved frame, the picture as it was before the pass too -
        // <shot>_noaa.bmp, without the 2D drawn after it.)
        gfx::ReadbackId aaRead = 0;
        if (aaDone && aaAt && !shotPath.empty() && std::getenv("TF_NATIVE_AASAVE") && aa::source)
            aaRead = dev.readTarget(aa::source);
        dev.use(picture.id, gfx::UseFor::Draw);
        passMark(6u);
        g.statsTaken[gfx::device().frameSlot()] = statsNow && !g.passKinds[gfx::device().frameSlot()].empty();
        prof::record += prof::since(profT);
        // ---- N9 stage 3: the frame's pictures, each the frame's recording run again (the device's endFrame) ------------
        // The recording above is the whole frame; each picture runs it with its own constants (where it lies between the frame
        // before's pose and this frame's, the camera's turn: PictureConstants, the shader's b1). A frame of one picture
        // (Frame::pictures empty) is drawn the same way, once, at this frame's pose.
        gfx::FrameEnd end;
        end.frame = &input;
        end.picture = picture.id;
        end.shot = shotPath;
        {
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
            const size_t pictureCount = std::min<size_t>(pictures.size(), kMaxPictures);
            for (size_t k = 0; k < pictureCount; ++k)
            {
                const Frame::Picture &pic = pictures[k];
                gfx::PictureOut out;
                PictureConstants pc{};
                pc.picture[0] = pic.s;
                for (int i = 0; i < 4; ++i)
                    pc.turn[5 * i] = pc.turnClip[5 * i] = 1.0f;
                out.constants.assign(reinterpret_cast<const uint8_t *>(&pc), reinterpret_cast<const uint8_t *>(&pc) + sizeof(pc));
                out.dueNs = pic.dueNs;
                out.shot = pic.shot;
                // (N9: the camera's turn of this picture - the mouse and stick since the frame's read, view_turn.h - written into
                // its constants just before it is submitted: an extra picture may wait for the moment it is read at.)
                if (input.turnCamera && input.turnMain >= 0)
                    out.beforeSubmit = [cam = input.turnCamera, main = input.turnMain, frameDue = input.dueNs, span = input.periodNs, due = pic.dueNs,
                                        s = pic.s, waits = input.turnWaits, set = input.turnLeadSet](uint8_t *constants, uint64_t id, bool extra) {
                        const double wait = extra && waits ? 25.0 : 0.0;
                        view_turn::PictureTurn t;
                        const auto t0 = std::chrono::steady_clock::now();
                        view_turn::pictureTurn(*cam, main, frameDue, span, due, s, wait, set, t);
                        const double waited =
                            wait > 0.0 ? std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() : 0.0;
                        std::memcpy(constants + offsetof(PictureConstants, turn), t.screen, sizeof(t.screen));
                        std::memcpy(constants + offsetof(PictureConstants, turnClip), t.clip, sizeof(t.clip));
                        view_turn::notePictureYaw(id, t.valid, t.yawShown, t.sampleNs);
                        return waited;
                    };
                end.pictures.push_back(std::move(out));
            }
        }
        // (a saved frame, or one whose debug saves read back after it: waited for)
        end.waitForGpu = !shotPath.empty() || savedRead != 0 || !afterSaves.empty() || std::getenv("TF_NATIVE_SYNC") != nullptr;
        dev.endFrame(end);
        prof::wait += prof::since(profT);
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
                         static_cast<unsigned long long>(g_refused), g.textures.size(), static_cast<size_t>(dev.textureSlotsFree()),
                         static_cast<unsigned long long>(g_targetsMade), static_cast<unsigned long long>(g_picturesMade),
                         static_cast<unsigned long long>(dev.videoMemory(false) >> 20), static_cast<unsigned long long>(dev.videoMemory(true) >> 20));
            std::fprintf(stderr, "[native render] profile: mesh buffers made %.1f a frame (%.0f vertices), %.1f meshes in the frame's ring, %zu resident; %.1f passes a frame read the picture\n",
                         g_meshesMade / 300.0, g_meshVertices / 300.0, g_ringMeshes / 300.0, g.meshes.size(), g_pictureReads / 300.0);
            std::fprintf(stderr, "[native render] profile: anti-aliasing in %u of 300 frames (SMAA in %u), %.3f ms of the render thread and %.3f ms of the GPU a frame it ran (%ux%u)\n",
                         g_aaFrames, g_smaaFrames, g_aaFrames ? g_aaCpu / g_aaFrames : 0.0, g_aaGpuFrames ? g_aaGpu / g_aaGpuFrames : 0.0, g.aaW, g.aaH);
            dev.report(gfx::Report::Profile); // (the device's: its uploads, targets' and textures' video memory, its counters)
            std::fprintf(stderr, "[native render] profile: the whole frame %.2f ms on the GPU (%u frames timed); the render thread waited %.2f ms a frame for a frame slot (in setup); window %u\n",
                         g_frameGpuFrames ? g_frameGpu / g_frameGpuFrames : 0.0, g_frameGpuFrames, g_slotWaitMs / 300.0, g_profWindow);
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
        if (aaRead)
        {
            gfx::Readback rb;
            if (dev.readback(aaRead, rb))
            {
                const uint8_t *p = static_cast<const uint8_t *>(rb.data);
                const std::string path = shotPath.substr(0, shotPath.size() - 4) + "_noaa.bmp";
                if (FILE *f = std::fopen(path.c_str(), "wb"))
                {
                    const uint32_t w = g.aaW, h = g.aaH;
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
                    for (uint32_t y = h; y-- > 0;) // (the picture is RGBA: a BMP's rows are BGRA, bottom up)
                    {
                        std::memcpy(row.data(), p + static_cast<size_t>(y) * rb.rowBytes, rowBytes);
                        for (uint32_t x = 0; x < rowBytes; x += 4u)
                            std::swap(row[x], row[x + 2u]);
                        std::fwrite(row.data(), 1, rowBytes, f);
                    }
                    std::fclose(f);
                }
            }
            dev.releaseReadback(aaRead);
        }
        if (savedRead)
        {
            gfx::Readback rb;
            if (dev.readback(savedRead, rb))
            {
                const uint8_t *p = static_cast<const uint8_t *>(rb.data);
                const std::string path = shotPath.substr(0, shotPath.size() - 4) + "_target.bmp";
                if (FILE *f = std::fopen(path.c_str(), "wb"))
                {
                    const uint32_t w = saved->w, h = saved->h;
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
                    for (uint32_t y = h; y-- > 0;)
                        std::fwrite(p + static_cast<size_t>(y) * rb.rowBytes, 1, rowBytes, f);
                    std::fclose(f);
                }
            }
            dev.releaseReadback(savedRead);
        }
        if (!g_pstatNotes.empty() && !shotPath.empty())
        {
            const std::string path = shotPath.substr(0, shotPath.rfind('\\') + 1) + "pstats_" + std::to_string(input.tick) + ".txt";
            std::vector<gfx::PipelineStats> st;
            if (dev.readStats(dev.frameSlot(), st))
            {
                if (FILE *f = std::fopen(path.c_str(), "w"))
                {
                    for (size_t k = 0; k < g_pstatNotes.size() && k < st.size(); ++k)
                        std::fprintf(f, "%u.%u set %08x ia %llu vs %llu gs %llu gsout %llu cin %llu cout %llu ps %llu\n", g_pstatNotes[k].draw,
                                     g_pstatNotes[k].pass, g_pstatNotes[k].source, static_cast<unsigned long long>(st[k].iaPrimitives),
                                     static_cast<unsigned long long>(st[k].vsInvocations), static_cast<unsigned long long>(st[k].gsInvocations),
                                     static_cast<unsigned long long>(st[k].gsPrimitives), static_cast<unsigned long long>(st[k].clipInvocations),
                                     static_cast<unsigned long long>(st[k].clipPrimitives), static_cast<unsigned long long>(st[k].psInvocations));
                    std::fclose(f);
                }
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
                    std::fill(thumb.begin(), thumb.end(), uint8_t{0});
                    gfx::Readback rb;
                    if (dev.readback(s.read, rb))
                    {
                        const uint8_t *p = static_cast<const uint8_t *>(rb.data);
                        for (uint32_t y = 0; y < 90u; ++y)
                            for (uint32_t x = 0; x < 160u; ++x)
                            {
                                uint32_t sum = 0, cnt = 0;
                                const uint32_t y0 = y * s.h / 90u, y1 = std::max(y0 + 1u, (y + 1u) * s.h / 90u);
                                const uint32_t x0 = x * s.w / 160u, x1 = std::max(x0 + 1u, (x + 1u) * s.w / 160u);
                                for (uint32_t yy = y0; yy < y1; yy += 2u)
                                    for (uint32_t xx = x0; xx < x1; xx += 2u)
                                    {
                                        const uint8_t *px = p + static_cast<size_t>(yy) * rb.rowBytes + xx * 4u;
                                        sum += (px[0] + 2u * px[1] + px[2]) / 4u;
                                        ++cnt;
                                    }
                                thumb[y * 160u + x] = static_cast<uint8_t>(cnt ? sum / cnt : 0u);
                            }
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
                    dev.releaseReadback(s.read);
                    s.read = 0;
                }
            afterSaves.erase(std::remove_if(afterSaves.begin(), afterSaves.end(), [](const AfterSave &s) { return s.split; }), afterSaves.end());
        }
        for (AfterSave &s : afterSaves)
        {
            gfx::Readback rb;
            if (dev.readback(s.read, rb))
            {
                const uint8_t *p = static_cast<const uint8_t *>(rb.data);
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
                    for (uint32_t y = s.h; y-- > 0;)
                        std::fwrite(p + static_cast<size_t>(y) * rb.rowBytes, 1, rowBytes, f);
                    std::fclose(f);
                }
            }
            dev.releaseReadback(s.read);
        }
    }
}

// (The frame builder speaks to the GPU through gfx::Device alone: no graphics API's header may reach it - checked here.)
#if defined(__d3d12_h__) || defined(__dxgi_h__) || defined(__d3dcompiler_h__) || defined(_WINDOWS_) || defined(VULKAN_H_)
#error "nr_frame.cpp must include no graphics API header (nor windows.h)"
#endif
