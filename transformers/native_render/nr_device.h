// nr_device.h - the device interface under the native renderer's frame builder (docs/RENDERER_BACKEND.md, section 4).
//
// The frame builder (nr_frame.cpp, no graphics API) owns everything that is about the GS: which targets exist and at
// which GS address, the order of the passes, the field weave, the glow and anti-aliasing decisions, the draw loop. It
// talks to the GPU only through Device below. nr_d3d.cpp is the Direct3D 12 implementation; a second backend (Vulkan)
// implements the same calls.
//
// Rules for the interface:
// - Handles are small integers the device hands out; 0 is "none". The frame builder never sees API objects.
// - Every call is made on the render thread, between prepareFrame and endFrame unless its comment says otherwise.
// - Resource states are named by use (UseFor, DepthUse); the device inserts the barriers / layout transitions and
//   tracks each resource's current state itself.
// - Pipelines are named by PipelineDesc; the device keeps its own cache (and on-disk library) keyed as it likes.
// - Everything recorded between beginFrame and endFrame is run once for each picture of FrameEnd::pictures (the
//   moment pictures); the device does the replay, the frame builder only marks draws with predicate().
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "nr_backend.h" // gfx::BlendState, gfx::Compare, gfx::Format, gfx::WriteMask

namespace nr
{
    struct Texture; // (nr_types.h: a texture's texels as the capture made them)
    struct Frame;   // (nr_types.h: the frame the capture made - its draws, its timing)
}

namespace nr::gfx
{
    using TargetId = uint32_t;  // a colour (+ optional depth) render surface
    using TextureId = uint32_t; // a sampled texture, opaque (the device maps it to its own view); 0 = white
    constexpr TextureId kKeepTexture = ~0u; // bindTextures: leave that slot bound to what it was
    using QueryId = uint32_t;    // a GPU time stamp of the frame being recorded: 1, 2, ... in each frame; 0 = none
    using ReadbackId = uint32_t; // a copy of a target into CPU memory; 0 = none
    constexpr uint32_t kTimestampsPerFrame = 8256u; // timestamp(): at least this many a frame (the profile's 8192 + spare)
    constexpr uint32_t kStatEntriesPerFrame = 8192u; // beginStats / endStats: entries 0 .. this - 1 a frame

    enum class UseFor : uint8_t
    {
        Draw,       // render target (+ depth write)
        Sample,     // pixel-shader read (colour, or depth for the depth fog)
        CopySource,
        CopyDest,
    };

    enum class DepthUse : uint8_t
    {
        Write,      // drawn with depth test/write
        ReadSample, // read-only depth bound while the shader samples it (the depth fog, the 3D probe)
    };

    struct TargetDesc
    {
        uint32_t width = 0, height = 0; // pixels
        Format colour = Format::RGBA8;
        bool depth = true;      // D32 alongside, also readable (the depth fog); false: colour alone
        bool shared = false;    // can be handed to the window (the picture)
        bool motion = false;    // (TODO 299) a motion target beside the colour (RG16F, the temporal upscaler's), drawn with it
        bool storage = false;   // (TODO 299) also writable by compute (the temporal upscaler's output)
    };

    // The passes of the shared shader set that are not the GS's draws: a full-screen triangle (drawn with draw(3): the
    // corners made from the vertex index, no vertex input) or, for Cloud, a quad per instance. Their constants through
    // bindDrawConstants, their inputs t0-t2 through bindTextures; each writes the format its target has.
    enum class Effect : uint8_t
    {
        None,
        Fxaa,        // FXAA 3.11 quality: the picture's copy (t0) into the picture (RGBA8)
        SmaaEdges,   // SMAA 1x luma edges of the picture's copy (t0) (RG8; the target cleared first: no edge, no pixel)
        SmaaWeights, // the blend weights from the edges (t0) (RGBA8). SMAA's area and search tables are the effect's own:
                     // the device binds them (t1, t2) with the pipeline - leave those two slots with kKeepTexture
        SmaaBlend,   // the picture's copy (t0) blended by the weights (t1) into the picture (RGBA8)
        Probe,       // the sun's occlusion probe: the depth (t2, a depth view) point-sampled over a rect (b0: x0, y0, step x,
                     // step y; the depth's width, height) into a 64 x 64 target (R32Float), one value a pixel
        Cloud,       // the warp tunnel's sparks: a strip of 4 a point, the points per instance (setVertices or setMesh,
                     // draw(4, 0, count)), added into the target
        BloomDown,   // (TODO 108) the bloom's step down: t0 shrunk by two into the target with a 13-tap filter (b0: 1 / the
                     // target's width, height; 1 / t0's width, height; the threshold rgb subtracted after, w > 0: on), RGBA8
        BloomUp,     // the bloom's step up: t0 enlarged by two with a 3 x 3 tent (b0: 1 / the target's width, height; 1 / t0's
                     // width, height; the gain rgb), ADDED to the target's colour times the blend factor (setBlendFactor;
                     // its alpha kept): target = up * gain * profile + target * factor, RGBA8. b0 goes on with an
                     // optional weight profile down the target: 8 floats (top to bottom, linear between band centres)
                     // and their count (0: none)
        ToneMap,     // (TODO 108) the filmic tone mapping: the picture's copy (t0) into the picture, its colour only (alpha
                     // kept), RGBA8. b0: exposure, the inverse curve's k, the strength (0: as it was, 1: the full curve,
                     // faded out below sRGB 0.25), the saturation added after (0: none)
        LocalContrast, // (TODO 108) the local contrast: the picture's copy (t0) with its luminance's difference from a blur
                       // of it (t1, any size, sampled linear) added, k times, into the picture - its colour only (alpha kept),
                       // RGBA8. b0: 1 / the target's width, height; k
    };

    // (TODO 299) PipelineDesc::writeMask's bit for a draw that writes its motion into the target's motion target (the scene's 3D).
    constexpr uint32_t kWriteMotion = 0x20u;

    struct PipelineDesc
    {
        BlendState blend;
        bool depthTest = false;
        Compare depthFunc = Compare::Always;
        bool depthWrite = false;
        uint32_t writeMask = kWriteAll;
        bool picture = false;    // drawn into the picture (the presentation pass shaders)
        bool clip = false;       // the clipping geometry stage
        uint32_t variant = 0u;   // pixel-shader variant (shader bits from the draw)
        uint32_t alphaTest = 0u; // the AID / ATST combination
        Effect effect = Effect::None; // one of the effects: the fields above not used
    };

    // The frame's streaming rings, each its own (each a frame slot's part, started again with the frame): the draws'
    // constants (256-byte aligned entries; the D3D12 device copies the frame's part into video memory before the
    // frame runs), the passes' instance entries (a vertex stream), and vertices / indices made this frame.
    enum class RingKind : uint8_t
    {
        Constants,
        Instances,
        Vertices,
    };

    // Where per-draw data went in a ring: what the builder wrote, and where the GPU reads it.
    struct RingSpan
    {
        void *cpu = nullptr;
        uint64_t gpu = 0; // device address / offset, opaque to the builder (a byte offset from it stays in the span)
        uint32_t size = 0;
        uint32_t first = 0; // (RingKind::Instances: the entry's index - draw()'s firstInstance)
    };

    struct Rect
    {
        int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    };

    // What the GPU's stages took in and let through between beginStats and endStats (profile and debug only).
    struct PipelineStats
    {
        uint64_t iaVertices = 0, iaPrimitives = 0, vsInvocations = 0, gsInvocations = 0, gsPrimitives = 0;
        uint64_t clipInvocations = 0, clipPrimitives = 0, psInvocations = 0;
    };

    // One picture of a frame (endFrame): the frame's recording run once for each - its constants (the shader's b1,
    // PictureConstants of nr_draw.h, written by the frame builder), its moment, and a file it is saved to once drawn.
    struct PictureOut
    {
        std::vector<uint8_t> constants;
        int64_t dueNs = 0;
        std::string shot; // (empty: not saved)
        // Called just before the picture is submitted - on another thread for an extra picture - with its constants as
        // they will be read (the frame builder writes the camera's turn of that moment into them), an id for it (the
        // pacer's log), and whether it is an extra picture; returns the ms it waited for that moment. May be empty.
        std::function<double(uint8_t *constants, uint64_t id, bool extra)> beforeSubmit;
    };

    // The frame's end (endFrame): its pictures and what is done with them.
    struct FrameEnd
    {
        const Frame *frame = nullptr;   // the frame: its tick, timing (the pacer's) and flags (dropped, has3D, shown2D)
        TargetId picture = 0;           // the target that is the picture (TargetDesc::shared)
        std::vector<PictureOut> pictures; // at least one; the first is the frame's own, the others the extra pictures
        std::string shot;               // the frame's own picture saved here too when no picture has a shot of its own
        bool waitForGpu = false;        // return only once the frame has finished on the GPU (and its frame-done call ran)
    };

    // The device's own report lines (TF_NATIVE_PROFILE and a saved frame's log): the frame builder prints its own.
    enum class Report : uint8_t
    {
        Rings,   // a saved frame's: how full the frame's rings are
        Profile, // every profile window (300 frames): uploads, video memory, the device's counters
    };

    // A read-back's data, mapped: `height` rows of `rowBytes` each (rowBytes >= width * the format's size).
    struct Readback
    {
        const void *data = nullptr;
        uint32_t width = 0, height = 0, rowBytes = 0;
        Format format = Format::RGBA8;
    };

    // (TODO 299) The temporal upscaler's frame: the projection's jitter (pixels of the render size), the frame's time, a
    // camera cut (the history thrown away) and the sharpening.
    struct Temporal
    {
        float jitterX = 0.0f, jitterY = 0.0f;
        float frameMs = 16.7f;
        bool reset = false;
        float sharpness = 0.0f;
        uint32_t rows = 0; // the source's rows upscaled, from the top (0: all - a field buffer's field A: its top half)
        // The scene's camera (nr::taa::Camera, fitted from the game's projection): the depth's near / far in w's units, its
        // field of view, w's units in metres.
        float nearZ = 1.0f, farZ = 1.0e6f, fovY = 1.0f, metersPerUnit = 1.0f;
        bool infinite = true;
    };

    class Device
    {
    public:
        virtual ~Device() = default;
        // (TODO 299) The temporal upscaler (the FidelityFX SDK's FSR 3.1 / 4): `source`'s colour, depth and motion (a target made
        // with TargetDesc::motion) upscaled into `output` (made with TargetDesc::storage). False: not done (no SDK, a backend
        // without it) - `output` is then as it was.
        virtual bool temporalUpscale(TargetId source, TargetId output, const Temporal &t) { (void)source, (void)output, (void)t; return false; }

        // --- frame life (two slots in flight) ---
        // The frame's start, before anything is recorded: the picture's surfaces at that size (made again when it changes -
        // a target made with TargetDesc::shared must then be made again: the frame builder sees it by the size), the
        // frame's slot waited for (its frame two before finished: the frame-done call for it made), the device's own
        // per-frame work. False: no picture - the frame is not drawn.
        virtual bool prepareFrame(uint32_t pictureWidth, uint32_t pictureHeight) = 0;
        // The frame's recording begun in its slot: its command list started, the shared bindings set, the rings started
        // again, its time stamps, statistics and read-backs counted from 0, the picture's own start time stamp taken.
        virtual bool beginFrame(uint64_t tick) = 0;
        virtual uint32_t frameSlot() = 0;                       // the slot being recorded (0 or 1)
        // The recording ended and submitted: its time stamps and statistics resolved, the list closed, then run once for
        // each picture (an extra picture's draws under predicate(true) left out), each copied to the window - a shared
        // texture or a read-back for the CPU hand-over - and saved when it has a shot. False: it did not close (the
        // device has said why).
        virtual bool endFrame(const FrameEnd &end) = 0;
        // Called by the device on the render thread once a slot's frame has finished on the GPU, before the slot is
        // recorded again (and for every other slot when the device waits for the GPU to go idle). The frame builder reads that
        // slot's time stamps, statistics and read-backs in it; the device's own work for the slot (the picture's times, its
        // hand-over to the window) is done in the same call, before and after it.
        virtual void setFrameDone(std::function<void(uint32_t slot)> done) = 0;
        virtual void report(Report what) = 0;
        virtual uint32_t vendorId() = 0;                        // the GPU's maker, its PCI vendor id (0x1002 AMD, 0x10DE NVIDIA; 0 unknown)
        virtual uint64_t videoMemory(bool budget) = 0;          // its video memory in use, or (budget) the system's budget for it
        virtual void videoMemoryNote(double frameMs) { (void)frameMs; } // (issue #20) a frame over 250 ms: the video memory logged, at most once in 5 s

        // --- targets (the builder keys them by GS address; the device only owns the surfaces) ---
        virtual TargetId createTarget(const TargetDesc &d) = 0; // 0 = failed
        virtual bool resizeTarget(TargetId t, const TargetDesc &d) = 0; // new surfaces, same handle and views (cleared)
        virtual void releaseTarget(TargetId t) = 0;             // freed once the frames using it are done
        virtual void clearColour(TargetId t, const float rgba[4]) = 0;
        virtual void clearDepth(TargetId t, float value) = 0;
        virtual void use(TargetId t, UseFor how) = 0;           // barrier to that state (no-op if already there)
        virtual void useDepth(TargetId t, DepthUse how) = 0;
        virtual TextureId view(TargetId t, bool depth = false) = 0; // the target as a sampled texture
        virtual TextureId snapshot(TargetId t) = 0;             // a copy of it as it is now (a target read while drawn into)

        // --- textures ---
        virtual TextureId createTexture(uint32_t width, uint32_t height, Format f, uint32_t levels) = 0;
        virtual bool uploadTexture(TextureId t, uint32_t level, const void *texels, uint32_t rowBytes) = 0; // on this frame
        // The texture made and uploaded off the drawing thread when the device gets to it (D3D12: its copy queue), the
        // texels read then from `source` - gone by then: skipped; urgent: before the large images. textureReady tells.
        virtual bool uploadTextureLater(TextureId t, std::weak_ptr<const Texture> source, bool urgent) = 0;
        virtual bool textureReady(TextureId t) = 0;             // its upload has reached the GPU
        virtual void releaseTexture(TextureId t) = 0;
        virtual uint32_t textureSlotsFree() = 0;                // room for this many more textures (0: a new one is refused)
        virtual uint64_t textureBytes(TextureId t) = 0;         // its video memory (the HD images' budget)

        // --- meshes (the game's geometry sets, cached across frames) ---
        virtual uint32_t createMesh(const void *vertices, uint32_t vertexBytes, const void *indices, uint32_t indexBytes) = 0;
        virtual void releaseMesh(uint32_t meshId) = 0;

        // --- streaming ring (constants, vertices of immediate draws, instances) ---
        virtual bool ringAlloc(RingKind kind, uint32_t size, uint32_t align, RingSpan &out) = 0; // false: that ring is full this frame

        // --- state ---
        virtual void setTarget(TargetId colour, bool withDepth) = 0;
        virtual void setViewport(float x, float y, float w, float h) = 0;
        virtual void setScissor(const Rect &r) = 0;
        virtual bool setPipeline(const PipelineDesc &p) = 0;    // a pipeline not made yet is made inside; false: none (the draw left out)
        virtual bool effect(Effect e) = 0;                       // that effect's pipeline was made (false: the pass is left out)
        virtual void setBlendFactor(const float rgba[4]) = 0;
        virtual void bindTextures(TextureId t0, TextureId t1, TextureId t2) = 0; // the three sampled slots (kKeepTexture: unchanged)
        virtual void bindDrawConstants(const RingSpan &c) = 0;  // b0 (b1, the picture's constants, is the device's: PictureOut)

        // --- draws ---
        virtual void setVertices(const RingSpan &v, uint32_t stride) = 0; // ring vertices (immediate draws); else setMesh
        virtual void setIndices(const RingSpan &indices) = 0;    // their 32-bit indices, in the ring (after setMesh: in place of its own)
        // The frame before's vertices of a mesh made anew each frame (moment pictures: place, colour, size - 20 bytes each),
        // a vertex stream of their own; an empty span: none.
        virtual void setPreviousVertices(const RingSpan &v) = 0;
        virtual void setMesh(uint32_t meshId) = 0;               // a cached mesh's vertex + index buffers
        virtual void draw(uint32_t vertices, uint32_t first, uint32_t instances = 1u, uint32_t firstInstance = 0u) = 0;
        virtual void drawIndexed(uint32_t indices, uint32_t first, uint32_t instances = 1u, uint32_t firstInstance = 0u) = 0;
        virtual void drawIndirect(uint64_t groundMeshId, uint32_t firstInstance) = 0; // the GPU grass of that ground (Mesh::id): counts from the compute pass
        // The GPU grass (optional - grassGpuReady() false: the frame builder grows the grass on the CPU): the frame's grass
        // grown by the device's compute pass from the grounds the frame draws, recorded ahead of the frame's draws;
        // grassMesh tells whether a ground (Mesh::id) was grown so (then drawn by drawIndirect).
        virtual void growGrass(const Frame &frame) = 0;
        virtual bool grassMesh(uint64_t groundMeshId) = 0;

        // --- copies ---
        virtual void copyWhole(TargetId dst, TargetId src) = 0; // same size and format

        // --- queries and statistics (read in the frame-done call of the slot that recorded them) ---
        virtual bool timestamps() = 0;                           // the GPU can take time stamps
        virtual QueryId timestamp() = 0;                         // a GPU time stamp at this point of the frame; 0: none
        // The slot's last frame's time stamps: ticks[id - 1] for each QueryId it handed out; false: none taken.
        virtual bool readTimestamps(uint32_t slot, std::vector<uint64_t> &ticks, uint64_t &ticksPerSecond) = 0;
        virtual bool pipelineStats() = 0;                        // the GPU can count its stages' work (made on the first ask)
        virtual void beginStats(uint32_t entry) = 0;             // count from here into the frame's entry (< kStatEntriesPerFrame)
        virtual void endStats(uint32_t entry) = 0;
        // The slot's last frame's entries, 0 up to the last one ended (in its frame-done call, or after waiting for the GPU).
        virtual bool readStats(uint32_t slot, std::vector<PipelineStats> &out) = 0;
        // Moment pictures: predicate(true) leaves the DRAWS recorded from here out of the frame's extra pictures (drawn by its
        // first picture alone), predicate(false) ends that. Copies, clears and state changes are never left out (a
        // predicated copy or clear hung the RX 5700 XT). predication(): the device can do it.
        virtual bool predication() = 0;
        virtual void predicate(bool skipInExtraPictures) = 0;

        // --- read-backs (a target's colour copied into CPU memory) ---
        // The copy is recorded here, the target left in the state it was in; its data is readable once this frame has
        // finished on the GPU (its slot's frame-done call, or after waiting for the GPU), until releaseReadback.
        virtual ReadbackId readTarget(TargetId t) = 0;
        virtual bool readback(ReadbackId r, Readback &out) = 0;  // maps it: false = none
        virtual void releaseReadback(ReadbackId r) = 0;          // its memory used again by a later frame of the same slot
    };

    Device &device(); // the backend in use (D3D12 today)
}
