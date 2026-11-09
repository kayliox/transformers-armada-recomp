// The native renderer's capture side (README.md): the game's own render calls, recorded as they
// happen, for the Direct3D 12 side to draw. The emulated path still runs unchanged (each wrapper
// calls the original): until the native picture is complete, it is drawn beside the GS's.
//
// Milestone 1: the geometry sets (vlRenderDevicePS2 slot 25, RenderGeometrySet) with the matrix
// func_00221DB0 sent to VU1 for them, decoded from the sets' own packets (vlGeometrySetPS2Data,
// the engine's vlGeometrySetVertexIterator::SetVertexIndex).
//
// TF_NATIVE_RENDER=1 turns it on. TF_NATIVE_SHOT=<VBlank>[,<VBlank>...]: the first frame begun at or
// after each of those ticks is drawn and saved to runs\native_<tick>.bmp (with PS2X_SHOT_VSYNCS at the
// same ticks, the GS picture beside it). TF_NATIVE_TRACE=<n>: the layout of the first n geometry sets.
// TF_NATIVE_DUMP=1: each saved frame's draws, as text beside the picture (runs\native_<tick>.txt).
#include "nr_types.h"
#include "nr_install.h"
#include "game_version.h" // (TF_GA: the code addresses that differ in the PAL release)
#include "hd_progress.h"
#include "nr_frame_texture_policy.h"
#include "view_turn.h"
#include "nr_shader.h"
#include "nr_gif.h"
#include "nr_vif.h"
#include "nr_occlusion.h"
#include "nr_particles.h"
#include "nr_movie.h"
#include "nr_rain.h"
#include "nr_n5.h"
#include "nr_frame.h" // (frame::setBloom: the modern bloom's setting, TODO 108)

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_memory.h"
#include "runtime/gs/ps2_gs_psmct16.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/ps2_host_display.h"
#include "runtime/ps2_host_input.h"
#include "runtime/ps2_settings.h"

#include <algorithm>
#include <cctype>
#include <array>
#include <atomic>
#include <mutex>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tf::port
{
    bool moviePlaying(); // (overrides.cpp)
}

namespace nr
{
    namespace
    {
        constexpr uint32_t kBeginFrame = 0x0025BE20u;       // vlRenderDevicePS2::BeginFrame (slot 3)
        constexpr uint32_t kEndFrame = 0x0025B050u;         // vlRenderDevicePS2 slot 4
        constexpr uint32_t kRenderGeometrySet = 0x00260A70u; // vlRenderDevicePS2 slot 25 (this, set, state block)
        constexpr uint32_t kSendMatrix = 0x00221DB0u;       // model-to-screen (and field B's) to VU1
        constexpr uint32_t kSetDestructor = 0x00226DD0u;    // vlGeometrySetPS2 slot 0
        constexpr uint32_t kTextureDestructor = 0x00266690u; // vlTexturePS2 slot 0 (rtti: its destructor)
        constexpr uint32_t kSetTexture = 0x0025ACD0u;       // vlRenderDevicePS2 slot 37 (this, stage, texture)
        constexpr uint32_t kApplyStateBlock = 0x002591F0u;  // vlRenderDevicePS2 slot 39 (this, state block)
        constexpr uint32_t kCaptureStateBlock = 0x00259410u; // vlRenderDevicePS2 slot 38 (this, state block): the GS state into it
        constexpr uint32_t kWriteGsRegister = 0x00225D60u;  // func_00225D60 (register, value): one GS register into the list, and its shadow
        constexpr uint32_t kUploadLevel = 0x00267060u;      // func_00267060 (texture, packet, level, GS block): a level's upload
        constexpr uint32_t kPacketPointer = 0x70000D30u;    // D_70000D30: where the next DMA words go
        constexpr uint32_t kPositionExponent = 0x006CD764u; // D_006CD764: compressed positions' scale
        constexpr uint32_t kDeviceTexture = 0x38u;          // vlRenderDevicePS2: the textures by stage (0x18: the default, untextured)
        constexpr uint32_t kGsShadow = 0x70000060u;         // D_70000060: func_00225D60's GS register shadow, 8 bytes a register
        constexpr uint32_t kShadowXyOffset = kGsShadow + 8u * 0x18u; // XYOFFSET_1
        constexpr uint32_t kShadowScissor = kGsShadow + 8u * 0x40u;  // SCISSOR_1
        constexpr uint32_t kShadowFrame = kGsShadow + 8u * 0x4Cu;    // FRAME_1

        PS2Runtime *g_displayRuntime = nullptr; // (gsDisplayedPicture)
        // (N3b) The display as the game's last frames had it: PMODE, DISPLAY1, the last four DISPFB1 (FBP, FBW).
        std::atomic<uint64_t> g_framePmode{0u}, g_frameDisplay{0u};
        std::atomic<uint64_t> g_frameBuffers[4] = {};
        std::atomic<uint32_t> g_frameBuffersNext{0u};
        std::atomic<uint64_t> g_lastFrameTick{0u}; // (N3b: the VBlank of the game's last frame - looseGif)
        std::atomic<uint32_t> g_looseSince3D{0u};
        std::atomic<uint64_t> g_vdumpNthTick{0u}; // (debug: TF_NATIVE_LOOSE_REGS_NTH's frame, for TF_NATIVE_LOOSE_VDUMP=nth)  // (TODO 120: loose frames handed over since the last frame with a scene)
        PS2Runtime::RecompiledFunction g_endFrame = nullptr;
        PS2Runtime::RecompiledFunction g_setTexture = nullptr, g_applyStateBlock = nullptr, g_uploadLevel = nullptr,
                                       g_captureStateBlock = nullptr;
        // The texture each state block was captured with (the material's: SetTexture just before the
        // capture, while the level's state blocks are compiled) - the native binding of a material.
        std::unordered_map<uint32_t, uint32_t> g_blockTextures, g_blockTextures2; // (TEX0_1's, TEX0_2's)
        std::vector<uint32_t> g_recent;          // textures asked for since the last capture
        std::vector<uint32_t> g_prefetchBlocks;  // state blocks captured since the last frame: their textures made ahead (prefetchBlockTextures)
        uint64_t g_bindExact = 0, g_bindGuess = 0; // captures bound by TEX0's block / by the last texture
        uint64_t g_boundRefit = 0; // (TODO 206) draws whose binding, larger than TEX0, gave way to the upload at TBP
        uint64_t g_boundByUpload = 0; // (TODO 124) SetTexture draws whose binding gave way to a later upload at TBP
        uint32_t g_stateBlock = 0; // the last state block applied (its GS registers: TEX0_1 at +0x30)
        // Which wrote a GS register last, in the display list's order: the state block applied last (a
        // DMA call of its packet) or func_00225D60 (a register written on its own, its shadow kept at
        // D_70000060). The front end's scenes (the HQ) set no state block at all - SetRenderState and
        // SetTexture write ALPHA, TEST, TEX0 one by one - so their draws read the shadow.
        uint64_t g_registerWrite[128] = {}; // each register's last func_00225D60 write (a sequence number)
        uint64_t g_blockWrite = 0, g_writeSequence = 0;
        PS2Runtime::RecompiledFunction g_writeGsRegister = nullptr;
        bool shadowHolds(uint32_t reg)
        {
            static const bool blockOnly = std::getenv("TF_NATIVE_SHADOWREGS") && *std::getenv("TF_NATIVE_SHADOWREGS") == '0'; // (debug: the block's, as before)
            return !g_stateBlock || (!blockOnly && g_registerWrite[reg & 0x7Fu] > g_blockWrite);
        }
        struct Upload
        {
            uint32_t texture = 0, level = 0;
            uint64_t seq = 0; // (TODO 124: g_tableSeq when the game's thread recorded it)
        };
        // (TODO 124) The order the game's thread saw uploads and SetTexture bindings in: a texture uploaded at a block
        // after the last binding there is what the GS holds (textureOf).
        uint64_t g_tableSeq = 0;
        std::unordered_map<uint32_t, uint64_t> g_bindSeq; // TEX0's block -> g_tableSeq of SetTexture's last binding there
        // The texture tables below (uploads, bindings, conversions): the game's thread and the
        // renderer's (the 2D's textures) both use them.
        std::recursive_mutex g_texMutex;
        std::unordered_map<uint32_t, Upload> g_uploads; // by GS block: the level last uploaded there
        std::unordered_map<uint32_t, uint32_t> g_tbpTextures; // TEX0's block -> the texture SetTexture put there
        // The two tables as the display list stands where the renderer's thread reads it: the game's
        // thread has gone on uploading by then (the HQ's labels: the font uploaded at one block, drawn
        // into the label sheet, then the next texture of the frame uploaded at the same block - the
        // tables at the frame's end named that one). Every change is logged with a sequence number,
        // each list stretch notes how many came before it, and the renderer's thread keeps its own
        // copy of the tables, brought up to each stretch before it reads it (nr_capture: expandFrame).
        struct TableChange
        {
            bool binding; // g_tbpTextures (else g_uploads)
            uint32_t block;
            Upload upload;
        };
        std::vector<TableChange> g_changes; // g_changes[0] is change number g_changesBase
        uint64_t g_changesBase = 0, g_changeCount = 0, g_tableScans = 0;
        struct TableView
        {
            std::unordered_map<uint32_t, Upload> uploads;
            std::unordered_map<uint32_t, uint32_t> bindings;
            std::unordered_map<uint32_t, uint64_t> bindSeq; // (TODO 259: g_bindSeq as the list stood)
            uint64_t applied = 0, scans = 0;
            bool synced = false;
        };
        TableView g_view;                 // (the renderer's thread's)
        thread_local bool t_useView = false; // (set while the renderer's thread expands a frame)
        const std::unordered_map<uint32_t, Upload> &uploadsTable() { return t_useView ? g_view.uploads : g_uploads; }
        const std::unordered_map<uint32_t, uint32_t> &bindingsTable() { return t_useView ? g_view.bindings : g_tbpTextures; }
        const std::unordered_map<uint32_t, uint64_t> &bindSeqTable() { return t_useView ? g_view.bindSeq : g_bindSeq; }
        std::string guestString(const uint8_t *rdram, uint32_t at);
        const uint8_t *g_logRdram = nullptr; // (TF_NATIVE_UPLOADLOG: main RAM, as the last upload saw it)
        void logChange(bool binding, uint32_t block, Upload upload)
        {
            // (Debug, TODO 259: TF_NATIVE_UPLOADLOG=<block hex>: every upload and binding recorded at that block, its texture's
            // name - the game thread's view of what the GS holds there.)
            static const uint32_t logBlock = std::getenv("TF_NATIVE_UPLOADLOG") ? static_cast<uint32_t>(std::strtoul(std::getenv("TF_NATIVE_UPLOADLOG"), nullptr, 16)) : 0xFFFFFFFFu;
            if (block == logBlock)
            {
                static int left = 200;
                if (left-- > 0)
                {
                    const uint8_t *rdram = g_logRdram;
                    std::fprintf(stderr, "[native render] %s at %04x: texture %08x '%s' level %08x\n", binding ? "binding" : "upload", block,
                                 upload.texture, rdram && upload.texture ? guestString(rdram, upload.texture + 0x18u).c_str() : "-", upload.level);
                }
            }
            if (!framesDeferred())
                return;
            if (g_changes.size() >= 65536u)
            {
                g_changes.erase(g_changes.begin(), g_changes.begin() + 32768);
                g_changesBase += 32768u;
            }
            g_changes.push_back(TableChange{binding, block, upload});
            ++g_changeCount;
        }
        // (The renderer's thread: its tables brought up to `count` changes; a copy of the game's when
        // they fell behind the log or the textures were scanned again.)
        void viewUpTo(uint64_t count)
        {
            std::lock_guard<std::recursive_mutex> lock(g_texMutex);
            // (Debug, TODO 259: TF_NATIVE_VIEWAHEAD=1 - the game thread's tables as they are now, every time: what a frame draws
            // when its copy runs ahead of it.)
            static const bool ahead = std::getenv("TF_NATIVE_VIEWAHEAD") && *std::getenv("TF_NATIVE_VIEWAHEAD") == '1';
            if (ahead || !g_view.synced || g_view.applied < g_changesBase || g_view.scans != g_tableScans)
            {
                g_view.uploads = g_uploads;
                g_view.bindings = g_tbpTextures;
                g_view.bindSeq = g_bindSeq;
                g_view.applied = g_changeCount;
                g_view.scans = g_tableScans;
                g_view.synced = true;
                return;
            }
            for (; g_view.applied < count && g_view.applied < g_changeCount; ++g_view.applied)
            {
                const TableChange &c = g_changes[static_cast<size_t>(g_view.applied - g_changesBase)];
                if (c.binding)
                {
                    g_view.bindings[c.block] = c.upload.texture;
                    g_view.bindSeq[c.block] = c.upload.seq; // (a binding's change carries its g_bindSeq)
                }
                else
                    g_view.uploads[c.block] = c.upload;
            }
        }
        bool g_scanAsked = false;                      // a block was not known: scan the textures again (now and then)
        uint32_t word(const uint8_t *rdram, uint32_t addr);

        // Every vlTexturePS2 in main memory (its vtable, 0x6B4760) and the GS block each of its
        // levels reads from (vlTextureBuffer at level +0x24: block, format, level) - the residency as
        // it stood before any upload was seen (a save state, a level loaded earlier).
        void scanTextures(const uint8_t *rdram)
        {
            std::lock_guard<std::recursive_mutex> lock(g_texMutex);
            size_t textures = 0, levels = 0;
            for (uint32_t a = 0x00100000u; a < PS2_RAM_SIZE; a += 4u)
            {
                uint32_t v;
                std::memcpy(&v, rdram + a, 4); // (main RAM: rdram is its first byte)
                if (v != 0x006B4760u)
                    continue;
                const uint32_t count = word(rdram, a + 0x94u);
                if (count == 0u || count > 16u)
                    continue;
                ++textures;
                for (uint32_t i = 0; i < count; ++i)
                {
                    const uint32_t level = word(rdram, a + 0x98u + 4u * i);
                    const uint32_t buffer = level ? word(rdram, level + 0x24u) : 0u;
                    if (!buffer)
                        continue;
                    const uint32_t block = word(rdram, buffer) & 0x3FFFu;
                    // (A level still a level: its size and depth - the memory of a texture freed
                    // keeps its vtable and pointers, not the rest.)
                    const uint32_t bpp = word(rdram, level + 0x14u);
                    if (!word(rdram, level + 0x0Cu) || !word(rdram, level + 0x10u) ||
                        (bpp != 4u && bpp != 8u && bpp != 16u && bpp != 24u && bpp != 32u))
                        continue;
                    if (word(rdram, buffer + 4u) == level)
                    {
                        g_uploads[block] = Upload{a, level};
                        ++levels;
                    }
                }
            }
            std::fprintf(stderr, "[native render] texture scan: %zu textures, %zu resident levels\n", textures, levels);
            ++g_tableScans;
        }
        uint32_t g_requested[2] = {};  // the texture each stage was last asked for
        // A geometry set's TEX0 (its sampling fields: TBP, TBW, PSM, size, CBP, CPSM, CSM, CSA) -> the
        // material texture its state block is bound to, as drawn last. The microcode primitives
        // with the same TEX0 sample the same texture (Deep Amazon's single-tree billboards: the
        // trees' sprite textures, put in GS memory by a path the upload map does not see - the map
        // still names what was there before, a temple wall).
        std::unordered_map<uint64_t, uint32_t> g_tex0Textures;
        constexpr uint64_t kTex0SampleMask = ~((1ull << 34) | (3ull << 35) | (7ull << 61)); // (TCC, TFX, CLD out)
        uint64_t g_setTextureCalls = 0;
        uint32_t g_packetEnd = 0;     // where the display list stood after the last draw
        uint64_t g_tex0 = 0;          // the last TEX0_1 seen written into the display list
        PS2Runtime::RecompiledFunction g_beginFrame = nullptr, g_renderSet = nullptr, g_sendMatrix = nullptr,
                                       g_setDestructor = nullptr, g_textureDestructor = nullptr;

        bool g_enabled = false;
        int g_trace = 0;
        bool g_dump = false;
        bool g_iterator = false; // TF_NATIVE_ITERATOR=1: the engine's vertex iterator (+0x4C) in place of the packet
        int g_packets = 0;        // TF_NATIVE_PACKET=<n>: the first n sets' packets to runs/native_packets.txt
        int g_why = 0; // TF_NATIVE_WHY=<n>: why the first n empty meshes are empty
        std::vector<uint64_t> g_shots; // VBlank ticks still to save
        uint64_t g_curTick = 0;    // (debug: the tick of the frame being captured)
        uint64_t g_expandTick = 0; // (debug: the tick of the frame being expanded, renderer's thread)
        std::atomic<uint64_t> g_nextId{1}; // (ids: the renderer's thread makes the 2D's too)

        float g_matrix[16] = {};
        Frame g_frame;
        // (N5 step 2 part 2, TF_N5_CONTEXTS: below)
        size_t g_chunkPending = 0;  // (Frame::vuChunks from here: not yet in a list stretch kept)
        // The list up to here left out by the capture (a geometry set culled: its words never replayed): the chunks taken out
        // of that part are left out with it (marked, the renderer passes them by).
        void dropPendingChunks()
        {
            std::vector<uint32_t> &v = g_frame.vuChunks;
            for (size_t i = g_chunkPending; i + 5u <= v.size(); i += 5u + v[i + 4u])
                v[i + 1u] = ~0u;
            g_chunkPending = v.size();
        }
        // The decoded meshes by geometry set (TODO 94, the capture's cost): an open-addressed table - the draw's lookup is
        // one or two probes of a flat array (a node-based map's bucket and node were 0.8% of the game's thread in Deep
        // Amazon, ~300 draws a frame). Keys are guest addresses (never 0: the empty slot); a key's removal shifts the
        // probe run back (no tombstones).
        class MeshTable
        {
        public:
            const std::shared_ptr<const Mesh> *find(uint32_t key) const
            {
                if (!key || m_keys.empty())
                    return nullptr;
                for (size_t i = slot(key);; i = (i + 1) & m_mask)
                {
                    if (m_keys[i] == key)
                        return &m_values[i];
                    if (!m_keys[i])
                        return nullptr;
                }
            }
            void set(uint32_t key, std::shared_ptr<const Mesh> value)
            {
                if (!key)
                    return;
                if ((m_size + 1) * 4 > m_keys.size() * 3) // (kept under 3/4 full)
                    grow();
                for (size_t i = slot(key);; i = (i + 1) & m_mask)
                {
                    if (m_keys[i] == key)
                    {
                        m_values[i] = std::move(value);
                        return;
                    }
                    if (!m_keys[i])
                    {
                        m_keys[i] = key;
                        m_values[i] = std::move(value);
                        ++m_size;
                        return;
                    }
                }
            }
            void erase(uint32_t key)
            {
                if (!key || m_keys.empty())
                    return;
                size_t i = slot(key);
                while (m_keys[i] != key)
                {
                    if (!m_keys[i])
                        return;
                    i = (i + 1) & m_mask;
                }
                // (backward shift: each later key of the run moved into the hole when its home slot allows it)
                for (size_t j = (i + 1) & m_mask; m_keys[j]; j = (j + 1) & m_mask)
                {
                    const size_t home = slot(m_keys[j]);
                    if (((j - home) & m_mask) >= ((j - i) & m_mask))
                    {
                        m_keys[i] = m_keys[j];
                        m_values[i] = std::move(m_values[j]);
                        i = j;
                    }
                }
                m_keys[i] = 0u;
                m_values[i].reset();
                --m_size;
            }
            size_t size() const { return m_size; }

        private:
            size_t slot(uint32_t key) const { return static_cast<size_t>((key * 0x9E3779B1u) >> m_shift) & m_mask; }
            void grow()
            {
                std::vector<uint32_t> keys(m_keys.empty() ? 1024u : m_keys.size() * 2u, 0u);
                std::vector<std::shared_ptr<const Mesh>> values(keys.size());
                keys.swap(m_keys);
                values.swap(m_values);
                m_mask = m_keys.size() - 1u;
                m_shift = 32u;
                for (size_t n = m_keys.size(); n > 1u; n >>= 1)
                    --m_shift;
                m_size = 0;
                for (size_t k = 0; k < keys.size(); ++k)
                    if (keys[k])
                        set(keys[k], std::move(values[k]));
            }
            std::vector<uint32_t> m_keys;
            std::vector<std::shared_ptr<const Mesh>> m_values;
            size_t m_mask = 0, m_size = 0;
            uint32_t m_shift = 32u;
        };
        MeshTable g_meshes;
        FrameTexturePolicy g_frameTexturePolicy = nullptr;
        FrameTexturePolicy g_interframePolicy = nullptr;
        microcode_prefix::Capture g_interframeCapture;
        PS2Runtime::RecompiledFunction g_microUpload=nullptr,g_microReturn=nullptr;
        bool g_interframeReady=false;
        uint64_t g_immediate = 0; // the display list's own draws seen
        uint64_t g_decoded = 0, g_failed = 0, g_texturesConverted = 0, g_texturesFailed = 0, g_texturesUnknown = 0, g_texturesUpscaled = 0, g_texturesVram = 0, g_texturesGameMips = 0;

        uint32_t word(const uint8_t *rdram, uint32_t addr)
        {
            // (FPS) Main memory and its uncached view straight away: the capture reads the game's
            // objects a word at a time, ~2% of the game's thread through getConstMemPtr's general way.
            uint32_t v = 0u;
            if (const uint32_t ram = addr & ~0x20000000u; ram <= PS2_RAM_SIZE - 4u && rdram)
            {
                std::memcpy(&v, rdram + ram, 4);
                return v;
            }
            const uint8_t *p = getConstMemPtr(rdram, addr);
            if (p)
                std::memcpy(&v, p, 4);
            return v;
        }

        float fl(const uint8_t *rdram, uint32_t addr)
        {
            const uint32_t w = word(rdram, addr);
            float f;
            std::memcpy(&f, &w, 4);
            return f;
        }

        uint16_t half(const uint8_t *rdram, uint32_t addr)
        {
            const uint8_t *p = getConstMemPtr(rdram, addr);
            uint16_t v = 0u;
            if (p)
                std::memcpy(&v, p, 2);
            return v;
        }

        // RGBA5551 as the PS2 data holds it (the engine's colour decoder): each 5-bit
        // channel times 16, so 8 = 128 = 1.0; alpha 0 or 0x80.
        uint32_t colour5551(uint16_t c)
        {
            const uint32_t r = std::min<uint32_t>((c & 0x1Fu) << 4, 255u);
            const uint32_t gg = std::min<uint32_t>(((c >> 5) & 0x1Fu) << 4, 255u);
            const uint32_t b = std::min<uint32_t>(((c >> 10) & 0x1Fu) << 4, 255u);
            const uint32_t a = (c & 0x8000u) ? 0x80u : 0u;
            return r | gg << 8 | b << 16 | a << 24;
        }

        // A geometry set's vertices, strip by strip (the engine's SetVertexIndex, followed for each
        // vertex), and triangles along each strip.
        std::shared_ptr<const Mesh> decode(const uint8_t *rdram, uint32_t set)
        {
            auto mesh = std::make_shared<Mesh>();
            mesh->id = g_nextId++;
            mesh->source = set;
            const uint32_t data = word(rdram, set + 0x4Cu);
            if (!data)
            {
                if (g_why > 0)
                {
                    --g_why;
                    std::fprintf(stderr, "[native render] empty set %08x: vtable %08x, no data (+4C 0); words 8..1C %08x %08x %08x %08x %08x %08x\n",
                                 set, word(rdram, set), word(rdram, set + 8u), word(rdram, set + 0xCu), word(rdram, set + 0x10u),
                                 word(rdram, set + 0x14u), word(rdram, set + 0x18u), word(rdram, set + 0x1Cu));
                }
                return mesh;
            }
            const uint32_t format = word(rdram, data + 0x00u);
            const uint32_t base = word(rdram, data + 0x04u);
            const int32_t before = static_cast<int32_t>(word(rdram, data + 0x08u));
            const int32_t perStrip = static_cast<int32_t>(word(rdram, data + 0x0Cu));
            const int32_t perBatch = static_cast<int32_t>(word(rdram, data + 0x10u));
            const int32_t per127 = static_cast<int32_t>(word(rdram, data + 0x14u));
            const int32_t stripsPerBatch = static_cast<int32_t>(word(rdram, data + 0x18u));
            const int32_t vertsPerStrip = static_cast<int32_t>(word(rdram, data + 0x1Cu));
            const int32_t vertsPerBatch = static_cast<int32_t>(word(rdram, data + 0x20u));
            const int32_t afterPositions = static_cast<int32_t>(word(rdram, data + 0x24u));
            const int32_t beforeUv = static_cast<int32_t>(word(rdram, data + 0x28u));
            const int32_t count = static_cast<int32_t>(word(rdram, data + 0x2Cu));
            const uint32_t contents = format ? word(rdram, format + 0x10u) : 0u;
            const bool compressed = format && (word(rdram, format + 0x44u) & 0x40u) != 0u;
            const int32_t perVertex = compressed ? 2 : 4;
            if (count <= 0 || count > 1000000 || vertsPerStrip <= 0 || vertsPerBatch <= 0 || !base)
            {
                if (g_why > 0)
                {
                    --g_why;
                    std::fprintf(stderr, "[native render] empty set %08x: vtable %08x, data %08x, count %d, verts/strip %d, "
                                         "verts/batch %d, base %08x\n",
                                 set, word(rdram, set), data, count, vertsPerStrip, vertsPerBatch, base);
                }
                return mesh;
            }
            mesh->hasColour = afterPositions != 0;
            mesh->hasUv = true; // (to check against `contents`: TF_NATIVE_TRACE)
            const int32_t exponent = static_cast<int32_t>(word(rdram, kPositionExponent));
            const float scaleXyz = std::ldexp(1.0f / 32768.0f, exponent);

            if (g_trace > 0)
            {
                --g_trace;
                std::fprintf(stderr,
                             "[native render] set %08x: count %d, contents %08x, %s positions, words: before %d, strip %d, "
                             "batch %d, per 127 strips %d, after positions %d, before uv %d; strips/batch %d, verts/strip "
                             "%d, verts/batch %d, exponent %d\n",
                             set, count, contents, compressed ? "compressed" : "float", before, perStrip, perBatch, per127,
                             afterPositions, beforeUv, stripsPerBatch, vertsPerStrip, vertsPerBatch, exponent);
            }

            mesh->vertices.reserve(static_cast<size_t>(count));
            int32_t stripStart = 0, stripKey = -1;
            for (int32_t i = 0; i < count; ++i)
            {
                const int32_t batch = i / vertsPerBatch;
                const int32_t rest = i - batch * vertsPerBatch;
                const int32_t strip = rest / vertsPerStrip;
                const int32_t vert = rest - strip * vertsPerStrip;
                const int32_t strips = strip + batch * stripsPerBatch;
                const uint32_t first = base + 4u * static_cast<uint32_t>(before + (strips / 127) * per127 + batch * perBatch +
                                                                         perStrip * strips);
                const int32_t length = std::min(vertsPerStrip, count - batch * vertsPerBatch - strip * vertsPerStrip);
                const uint32_t position = first + 4u * static_cast<uint32_t>(vert * perVertex);
                uint32_t end = first + 4u * static_cast<uint32_t>(length * perVertex);
                Vertex v;
                if (compressed)
                {
                    v.x = static_cast<float>(static_cast<int16_t>(half(rdram, position + 0u))) * scaleXyz;
                    v.y = static_cast<float>(static_cast<int16_t>(half(rdram, position + 2u))) * scaleXyz;
                    v.z = static_cast<float>(static_cast<int16_t>(half(rdram, position + 4u))) * scaleXyz;
                }
                else
                {
                    v.x = fl(rdram, position + 0u);
                    v.y = fl(rdram, position + 4u);
                    v.z = fl(rdram, position + 8u);
                }
                if (afterPositions)
                {
                    const uint32_t colourAt = end + 4u * static_cast<uint32_t>(afterPositions) + 2u * static_cast<uint32_t>(vert) +
                                              (compressed ? 4u : 0u);
                    v.colour = colour5551(half(rdram, colourAt));
                    end += 4u * static_cast<uint32_t>(afterPositions + (length + 1) / 2);
                }
                const uint32_t uvAt = end + 4u * static_cast<uint32_t>(beforeUv) + 8u * static_cast<uint32_t>(vert);
                v.u = fl(rdram, uvAt);
                v.v = fl(rdram, uvAt + 4u);
                // A new strip: its triangles start again.
                const int32_t key = strips;
                if (key != stripKey)
                {
                    stripKey = key;
                    stripStart = i;
                }
                mesh->vertices.push_back(v);
                if (i - stripStart >= 2)
                {
                    const uint32_t c = static_cast<uint32_t>(i);
                    mesh->indices.push_back(c - 2u);
                    mesh->indices.push_back(c - 1u);
                    mesh->indices.push_back(c);
                }
            }
            return mesh;
        }

        // A mesh's vertices as sprites (0x22a0): a quad of four corners each, the corner in u, v (the
        // vertex shader places them on the screen), cached by the mesh.
        std::unordered_map<uint64_t, std::pair<std::weak_ptr<const Mesh>, std::shared_ptr<const Mesh>>> g_sprites;

        std::mutex g_spritesMutex; // (made on the renderer's thread, let go of on the game's)

        std::shared_ptr<const Mesh> spritesOf(const std::shared_ptr<const Mesh> &mesh)
        {
            if (!mesh)
                return mesh;
            std::lock_guard<std::mutex> lock(g_spritesMutex);
            auto &e = g_sprites[mesh->id];
            if (e.second && !e.first.expired())
                return e.second;
            auto quads = std::make_shared<Mesh>();
            static std::atomic<uint64_t> ids{1ull << 61}; // (apart from the capture's own)
            quads->id = ids++;
            quads->source = mesh->source;
            quads->sprites = true;
            quads->hasColour = mesh->hasColour;
            quads->prim = mesh->prim;
            quads->type = mesh->type;
            quads->primitive = mesh->primitive;
            for (const Vertex &c : mesh->vertices)
            {
                const uint32_t base = static_cast<uint32_t>(quads->vertices.size());
                for (int k = 0; k < 4; ++k)
                {
                    Vertex corner = c;
                    corner.u = static_cast<float>(k & 1);
                    corner.v = static_cast<float>(k >> 1);
                    quads->vertices.push_back(corner);
                }
                for (uint32_t q : {0u, 1u, 2u, 2u, 1u, 3u})
                    quads->indices.push_back(base + q);
            }
            e = {mesh, quads};
            return quads;
        }

        // (TODO 162) Cybertron's city (image a31d's 0x22c0, Routine::CityBlocks): each vertex a building - its
        // centre the position, its half sizes the ST, its height / 10 the colour's alpha. The routine draws one face
        // of every building a batch (the block's q10.w: 0 the side along x, 1 the side along y, 2 the roof), as a
        // strip of four corners; here every vertex four corners (their number in size), the vertex shader placing
        // them (nr_d3d.cpp, mode.z 5) - the face, and the side the eye sees, are the frame's. Cached by the mesh.
        std::shared_ptr<const Mesh> cityBlocksOf(const std::shared_ptr<const Mesh> &mesh)
        {
            if (!mesh)
                return mesh;
            std::lock_guard<std::mutex> lock(g_spritesMutex);
            auto &e = g_sprites[mesh->id ^ (1ull << 63)]; // (the sprites' cache, a key of its own)
            if (e.second && !e.first.expired())
                return e.second;
            auto quads = std::make_shared<Mesh>();
            static std::atomic<uint64_t> ids{(1ull << 61) + (1ull << 60)}; // (apart from the capture's and the sprites')
            quads->id = ids++;
            quads->source = mesh->source;
            quads->hasColour = mesh->hasColour;
            quads->hasUv = true;
            quads->prim = mesh->prim;
            quads->type = mesh->type;
            for (const Vertex &c : mesh->vertices)
            {
                const uint32_t base = static_cast<uint32_t>(quads->vertices.size());
                for (int k = 0; k < 4; ++k)
                {
                    Vertex corner = c;
                    corner.size = static_cast<float>(k);
                    quads->vertices.push_back(corner);
                }
                for (uint32_t q : {0u, 1u, 2u, 2u, 1u, 3u})
                    quads->indices.push_back(base + q);
            }
            e = {mesh, quads};
            return quads;
        }

        // A sprite set's vertices as its packet holds them now. The game rewrites a sprite's size
        // (the position's w) in the set's own packet every frame - the forest's distant trees, a
        // sprite per tree, are shrunk to nothing (size 0.009) where the tree is drawn on its own,
        // as a mesh or as a sprite of its own: the routine draws them as specks (checked against
        // the GS draw log at Amazon's tick 10900: every set sprite matched by its depth, the ones
        // with the speck size 1-12 px wide on the GS, full size here). The decode made at the set's
        // first draw, kept since, has every tree at its full size - the giant dark palm at the start
        // of Amazon was the tree beside the player, at its set's full size - and a decode at the
        // draw's call sees the sizes before the frame's update. Read again here, on the renderer's
        // thread, from the packet the frame's DMA reads; a new mesh only when something changed
        // (kept per set while the packet stays the same).
        std::unordered_map<uint64_t, std::shared_ptr<const Mesh>> g_currentSprites;
        std::mutex g_currentSpritesMutex;
        std::shared_ptr<const Mesh> currentSprites(const uint8_t *rdram, const std::shared_ptr<const Mesh> &mesh)
        {
            if (!rdram || !mesh || !mesh->source || mesh->vertices.empty())
                return mesh;
            thread_local Mesh now;
            {
                // (TODO 94: its arrays kept from one call to the next - the forest's sprite sets read again every frame, each
                // grown a vertex at a time into fresh arrays was ~3% of Deep Amazon's render thread. The same decode.)
                std::vector<Vertex> vertices = std::move(now.vertices);
                std::vector<uint32_t> indices = std::move(now.indices);
                now = Mesh{};
                vertices.clear();
                indices.clear();
                vertices.reserve(mesh->vertices.size());
                indices.reserve(mesh->indices.size());
                now.vertices = std::move(vertices);
                now.indices = std::move(indices);
            }
            decodePacket(rdram, word(rdram, static_cast<uint32_t>(mesh->source) + 0x14u), now, nullptr);
            if (now.vertices.size() != mesh->vertices.size())
                return mesh; // (not read the same way: as it was)
            auto same = [&](const Mesh &m) {
                for (size_t k = 0; k < m.vertices.size(); ++k)
                {
                    const Vertex &a = m.vertices[k], &b = now.vertices[k];
                    if (a.size != b.size || a.colour != b.colour || a.x != b.x || a.y != b.y || a.z != b.z)
                        return false;
                }
                return true;
            };
            if (same(*mesh))
                return mesh;
            std::lock_guard<std::mutex> lock(g_currentSpritesMutex);
            if (g_currentSprites.size() > 4096u)
                g_currentSprites.clear(); // (sets come and go with the level's parts)
            auto &kept = g_currentSprites[mesh->id];
            if (kept && kept->vertices.size() == now.vertices.size() && same(*kept))
                return kept;
            auto m = std::make_shared<Mesh>(*mesh);
            m->id = g_nextId++;
            for (size_t k = 0; k < m->vertices.size(); ++k)
            {
                Vertex &v = m->vertices[k];
                const Vertex &n = now.vertices[k];
                v.x = n.x, v.y = n.y, v.z = n.z, v.size = n.size, v.colour = n.colour;
            }
            kept = m;
            return kept;
        }

        // (TODO 133, a checking aid: TF_NATIVE_MESHCHECK=<n> - a cached set's packet hashed as it is now against its hash when
        // decoded, the first n that differ logged: a set whose packet the game rewrote in place keeps its old mesh.)
        std::unordered_map<uint32_t, std::pair<uint64_t, uint32_t>> g_meshHashes; // (hash, packet pointer when decoded)
        uint64_t packetHash(const uint8_t *rdram, uint32_t set)
        {
            static thread_local std::vector<uint32_t> words;
            words.clear();
            if (!vif::gather(rdram, word(rdram, set + 0x14u), words, nullptr))
                return 0u;
            uint64_t h = 1469598103934665603ull ^ words.size();
            for (uint32_t w : words)
                h = (h ^ w) * 1099511628211ull;
            return h;
        }

        std::shared_ptr<const Mesh> meshOf(const uint8_t *rdram, uint32_t set)
        {
            static int meshChecks = std::getenv("TF_NATIVE_MESHCHECK") ? std::atoi(std::getenv("TF_NATIVE_MESHCHECK")) : 0;
            const std::shared_ptr<const Mesh> *found = g_meshes.find(set);
            if (found && meshChecks > 0 && !g_iterator)
            {
                const uint64_t now = packetHash(rdram, set);
                auto h = g_meshHashes.find(set);
                if (h != g_meshHashes.end() && now != h->second.first)
                {
                    --meshChecks;
                    Mesh fresh;
                    decodePacket(rdram, word(rdram, set + 0x14u), fresh, nullptr);
                    float dp = 0.0f, du = 0.0f;
                    uint32_t dc = 0u;
                    const auto &old = (*found)->vertices;
                    for (size_t v = 0; v < old.size() && v < fresh.vertices.size(); ++v)
                    {
                        const Vertex &a = old[v], &b = fresh.vertices[v];
                        dp = std::max({dp, std::fabs(a.x - b.x), std::fabs(a.y - b.y), std::fabs(a.z - b.z)});
                        du = std::max({du, std::fabs(a.u - b.u), std::fabs(a.v - b.v)});
                        dc += a.colour != b.colour ? 1u : 0u;
                    }
                    std::fprintf(stderr, "[native render] mesh check: set %08x packet %08x (was %08x) changed since decoded (tick %llu, mesh %llu, %zu vertices; now %zu; "
                                 "position moved up to %g, uv %g, %u colours, extent %g)\n",
                                 set, word(rdram, set + 0x14u), h->second.second, static_cast<unsigned long long>(g_curTick),
                                 static_cast<unsigned long long>((*found)->id), (*found)->vertices.size(), fresh.vertices.size(), dp, du, dc,
                                 old.empty() ? 0.0f : std::fabs(old[0].x) + std::fabs(old[0].y) + std::fabs(old[0].z));
                    h->second = {now, word(rdram, set + 0x14u)};
                }
            }
            if (found)
                return *found;
            if (meshChecks > 0 && !g_iterator)
                g_meshHashes[set] = {packetHash(rdram, set), word(rdram, set + 0x14u)};
            std::shared_ptr<const Mesh> mesh;
            if (g_iterator)
                mesh = decode(rdram, set);
            else
            {
                auto m = std::make_shared<Mesh>();
                m->id = g_nextId++;
                m->source = set;
                FILE *dump = nullptr;
                if (g_packets > 0)
                {
                    --g_packets;
                    dump = std::fopen("runs/native_packets.txt", "a");
                    if (dump)
                        std::fprintf(dump, "set %08x (+14 %08x)\n", set, word(rdram, set + 0x14u));
                }
                decodePacket(rdram, word(rdram, set + 0x14u), *m, dump);
                if (dump)
                {
                    std::fprintf(dump, "  -> %zu vertices, %zu triangles\n", m->vertices.size(), m->indices.size() / 3);
                    std::fclose(dump);
                }
                mesh = m;
            }
            ++(mesh->indices.empty() ? g_failed : g_decoded);
            g_meshes.set(set, mesh);
            return mesh;
        }

        // What the draw samples: TEX0_1 from the state block applied last (slot 39), its texture and
        // palette found by the GS blocks their levels were uploaded to; converted once per key (the
        // key holds the level and palette pointers, so new texels or another palette convert again).
        struct TextureEntry
        {
            TextureKey key;
            uint32_t data = 0, clutData = 0;
            uint64_t generation = 0; // (the image's and palette's levelGeneration when converted)
            uint64_t epoch = ~0ull;  // (TODO 94) snapshotEpoch when generation was last worked out (same: still right)
            bool tried = false; // (a texture that does not convert is not tried again until its key changes)
            uint64_t ticket = 0; // its large image asked for (nr_hires.cpp), taken in when it comes
            uint64_t lastTick = 0; // the moment it was last drawn with (evictTextures; g_textureTick's clock)
            uint64_t content = 0;  // (TODO 124) its game texture's contentKey once its large image was asked for
            double askedAt = 0.0;  // (TODO 124) when it began to wait for its large image (ms: the in-view-to-HD numbers)
            std::shared_ptr<const Texture> texture;
            // (TODO 94) With a large image in `texture`: the game's texture it stands for (put back when the
            // large one is let go of for the video memory - largeLost: asked for again when next drawn).
            std::shared_ptr<const Texture> game;
            bool largeLost = false;
        };
        std::unordered_map<uint64_t, TextureEntry> g_textures; // by TEX0
        uint64_t g_lastTicket = 0;
        // The frame's moment for TextureEntry::lastTick and the evictions: 60ths of a second of the host's clock
        // (TODO 124). It was the VBlank count - 60 a second at the console's rate, at 240 frames a second four
        // times as many, so "30 s not drawn" was 7.5 s and "5 s" 1.25: a tree out of view for a few seconds was
        // let go of, and drawn again with the game's small texture until its large image came back (the trees
        // that "look flat, then jump"). TF_NATIVE_TEXCLOCK=vblank: the VBlanks, as before.
        uint64_t g_textureTick = 0;
        std::atomic<bool> g_placeChanged{false}; // (TODO 55: placeChanged, taken by evictTextures)
        uint64_t g_placeStart = 0u;
        bool g_placePending = false;
        double nowMs()
        {
            static const auto t0 = std::chrono::steady_clock::now();
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        }
        // (TODO 124) In view to large image: entries that waited for theirs (ms from the draw that asked to the
        // large image taken in - its upload to the GPU, a frame or so, not counted), those that got one at once
        // (shared, or kept: SharedLarge::kept), and large images asked for again after being let go of.
        std::vector<float> g_waitMs;
        uint64_t g_largeAtOnce = 0, g_largeAskedAgain = 0, g_largeKeptUsed = 0;
        std::unordered_set<uint64_t> g_contentLoaded; // (contentKeys that had a large image)

        // One large image for every draw of the same game texture (2026-10-30, TODO 94). The entries are by TEX0
        // (and the texture), and the same texture comes with many: other GS pages each time it is uploaded,
        // other TFX / TCC (the lit meshes' two contexts) - an HQ run loaded each character image ~12 times
        // (529 large images for 80 names), each a GPU copy of its own: most of the HQ's 15-18 GB. Now the
        // large images are shared by their game texture's content (its name, size and texels): asked for
        // once, given to every entry with that content, one GPU copy.
        struct SharedLarge
        {
            std::weak_ptr<const Texture> large; // (the entries hold it; this does not keep it)
            // (TODO 124) Held a while after its last entry was let go of (evictTextures): a texture drawn again
            // takes its large image at once, not the game's until the loaders bring it back.
            std::shared_ptr<const Texture> kept;
            uint64_t keptAt = 0;
            uint64_t ticket = 0;                // asked for, not back yet
            bool rejected = false;              // no large image passed the checks: not asked for again
            // (TODO 219) No large image yet - the HD set is still being made: asked for again once it has grown
            // (hd_progress::generation() past missingGen, the generation when it was asked for).
            bool missing = false;
            uint64_t askedGen = 0, missingGen = 0;
            std::string name;
        };
        std::unordered_map<uint64_t, SharedLarge> g_shared;     // by contentKey
        std::unordered_map<uint64_t, uint64_t> g_ticketContent; // a ticket's contentKey
        uint64_t g_largeShares = 0, g_largeEvicted = 0;

        uint64_t contentKey(const std::string &name, const Texture &t)
        {
            uint64_t h = 1469598103934665603ull;
            auto mix = [&h](const void *p, size_t n) {
                const auto *b = static_cast<const uint8_t *>(p);
                for (size_t i = 0; i < n; ++i)
                    h = (h ^ b[i]) * 1099511628211ull;
            };
            mix(name.data(), name.size());
            const uint32_t dims[3] = {t.width, t.height, t.gameMips};
            mix(dims, sizeof dims);
            // (TODO 164: the texels' hash, 8 bytes at a time - made on the conversion worker, where it was converted
            // there; the bytes one at a time were a sixth of an HQ conversion's time)
            const uint64_t texels = t.pixelHash ? t.pixelHash : pixelHash(t);
            mix(&texels, sizeof texels);
            return h;
        }

        // (TODO 177) The large images by content for the conversion workers (g_shared is the game thread's, under
        // g_texMutex): a texture the game uploads again - the HQ's, a group at a time while the camera moves - gets
        // its large image on the frame it is converted, not the game's texture for that frame (the button prompts'
        // PS2 symbols, the low-res blocks). TF_HIRES_SHARE=0 / TF_NATIVE_SAMEFRAME_LARGE=0: as before.
        std::mutex g_liveLargeMutex;
        std::unordered_map<uint64_t, std::weak_ptr<const Texture>> g_liveLarge;

        void publishLarge(uint64_t ck, const std::shared_ptr<const Texture> &large)
        {
            std::lock_guard<std::mutex> lock(g_liveLargeMutex);
            if (g_liveLarge.size() > 16384u)
                for (auto it = g_liveLarge.begin(); it != g_liveLarge.end();)
                    it = it->second.expired() ? g_liveLarge.erase(it) : std::next(it);
            g_liveLarge[ck] = large;
        }

        std::shared_ptr<const Texture> liveLarge(uint64_t ck)
        {
            std::lock_guard<std::mutex> lock(g_liveLargeMutex);
            const auto it = g_liveLarge.find(ck);
            return it == g_liveLarge.end() ? nullptr : it->second.lock();
        }

        bool sameFrameLarge()
        {
            static const bool on = !(std::getenv("TF_HIRES_SHARE") && *std::getenv("TF_HIRES_SHARE") == '0') &&
                                   !(std::getenv("TF_NATIVE_SAMEFRAME_LARGE") && *std::getenv("TF_NATIVE_SAMEFRAME_LARGE") == '0');
            return on;
        }

        // The entry's large image: one already loaded for the same content, the request already out, or asked for.
        void askLarge(TextureEntry &e, const std::string &name)
        {
            // (TF_HIRES_SHARE=0: one large image per entry, as before - the A/B of the sharing.)
            static const bool share = !(std::getenv("TF_HIRES_SHARE") && *std::getenv("TF_HIRES_SHARE") == '0');
            if (!share)
            {
                e.ticket = ++g_lastTicket;
                requestUpscaled(e.ticket, name, e.texture);
                return;
            }
            const uint64_t ck = contentKey(name, *e.texture);
            SharedLarge &s = g_shared[ck];
            e.content = ck;
            e.askedAt = nowMs();
            if (std::shared_ptr<const Texture> live = s.large.lock())
            {
                if (s.kept && live == s.kept)
                    ++g_largeKeptUsed;
                e.game = e.texture;
                e.texture = std::move(live);
                ++g_texturesUpscaled;
                ++g_largeShares;
                ++g_largeAtOnce;
                return;
            }
            if (s.rejected)
                return;
            if (s.ticket)
            {
                e.ticket = s.ticket;
                return;
            }
            if (s.missing)
            {
                if (s.missingGen == hd_progress::generation())
                    return; // (TODO 219: not made yet, and the set has not grown since)
                s.missing = false;
            }
            if (g_contentLoaded.count(ck))
                ++g_largeAskedAgain;
            s.name = name;
            s.askedGen = hd_progress::generation();
            s.ticket = e.ticket = ++g_lastTicket;
            g_ticketContent[s.ticket] = ck;
            requestUpscaled(e.ticket, name, e.texture);
        }

        // (TODO 219) The textures in use whose HD image was not made yet, asked for again once the set has grown (the
        // HD set being made while the game runs, hd_progress.h): at most every 10 s, at most 64 contents a time, a
        // texture drawn in the last 30 s - the rest ask again when next converted (askLarge). Nothing is read or
        // decoded here: the loaders check the files.
        void askMissingAgain(uint64_t tick)
        {
            static uint64_t lastSweep = 0, lastGen = 0;
            const uint64_t gen = hd_progress::generation();
            if (gen == lastGen || tick < lastSweep + 600u)
                return;
            lastSweep = tick;
            lastGen = gen;
            std::lock_guard<std::recursive_mutex> lock(g_texMutex);
            unsigned asked = 0;
            for (auto &kv : g_textures)
            {
                TextureEntry &e = kv.second;
                if (!e.content || e.ticket || !e.texture || e.texture->upscaled || e.lastTick + 1800u < tick)
                    continue;
                auto sh = g_shared.find(e.content);
                if (sh == g_shared.end())
                    continue;
                SharedLarge &sl = sh->second;
                if (sl.ticket)
                {
                    e.ticket = sl.ticket; // (asked for already this sweep, for another entry with the same content)
                    continue;
                }
                if (!sl.missing || sl.missingGen == gen || sl.name.empty())
                    continue;
                if (asked >= 64u)
                    break;
                sl.missing = false;
                sl.askedGen = gen;
                sl.ticket = e.ticket = ++g_lastTicket;
                g_ticketContent[sl.ticket] = e.content;
                e.askedAt = nowMs();
                requestUpscaled(e.ticket, sl.name, e.texture);
                ++asked;
            }
            static int told = 6;
            if (asked && told > 0)
            {
                --told;
                std::fprintf(stderr, "[native render] HD set grown: %u textures asked for their HD image again\n", asked);
            }
        }

        // Large images over the video memory's budget (d3d::largeImageBudget, TODO 94): the ones drawn least
        // recently (none in the last TF_HIRES_IDLE seconds, default 5) let go of - their game textures drawn
        // again, the large image asked for again when next drawn (from the compressed cache: milliseconds).
        void evictLarge(uint64_t tick)
        {
            static uint64_t lastScan = 0;
            if (tick < lastScan + 30u)
                return;
            const uint64_t budget = d3d::largeImageBudget(), used = d3d::largeImageBytes();
            if (budget == UINT64_MAX || used <= budget)
                return;
            lastScan = tick;
            {
                // (TODO 124: the large images kept for textures let go of go first - nothing draws them now.)
                std::lock_guard<std::recursive_mutex> lock(g_texMutex);
                size_t released = 0;
                for (auto &kv : g_shared)
                    if (kv.second.kept)
                    {
                        kv.second.kept.reset();
                        ++released;
                    }
                if (released)
                    return; // (measured again at the next scan, once their GPU copies are gone)
            }
            static const uint64_t idle = [] {
                const char *e = std::getenv("TF_HIRES_IDLE");
                return static_cast<uint64_t>(std::max(1L, e && *e ? std::atol(e) : 5L)) * 60u;
            }();
            std::lock_guard<std::recursive_mutex> lock(g_texMutex);
            std::unordered_map<const Texture *, uint64_t> lastDrawn; // (a large image's: the latest of its entries')
            for (auto &kv : g_textures)
                if (kv.second.game && kv.second.texture && kv.second.texture->upscaled)
                {
                    uint64_t &t = lastDrawn[kv.second.texture.get()];
                    t = std::max(t, kv.second.lastTick);
                }
            std::vector<std::pair<uint64_t, const Texture *>> idleOnes;
            for (auto &[t, last] : lastDrawn)
                if (last + idle < tick)
                    idleOnes.emplace_back(last, t);
            std::sort(idleOnes.begin(), idleOnes.end());
            uint64_t freed = 0;
            const uint64_t want = used - budget + budget / 10u; // (to 90% of the budget)
            std::unordered_set<const Texture *> chosen;
            for (auto &[last, t] : idleOnes)
            {
                if (freed >= want)
                    break;
                const uint64_t texels = static_cast<uint64_t>(t->width) * t->height;
                freed += (t->format == 1u ? texels : texels * 4u) * 4u / 3u;
                chosen.insert(t);
            }
            for (auto &kv : g_textures)
                if (kv.second.game && chosen.count(kv.second.texture.get()))
                {
                    kv.second.texture = std::move(kv.second.game);
                    kv.second.game.reset();
                    kv.second.largeLost = true;
                }
            g_largeEvicted += chosen.size();
            // (The content records whose large image and request are both gone: dropped now and then.)
            if (g_shared.size() > 8192u)
                for (auto it = g_shared.begin(); it != g_shared.end();)
                    if (!it->second.ticket && !it->second.rejected && it->second.large.expired())
                        it = g_shared.erase(it);
                    else
                        ++it;
            static int told = 8;
            if (told > 0 && !chosen.empty())
            {
                --told;
                std::fprintf(stderr, "[native render] large images over the budget (%llu of %llu MB): %zu let go of (~%llu MB, idle %llu+ s)\n",
                             static_cast<unsigned long long>(used >> 20), static_cast<unsigned long long>(budget >> 20), chosen.size(),
                             static_cast<unsigned long long>(freed >> 20), static_cast<unsigned long long>(idle / 60u));
            }
        }

        // Past TF_NATIVE_TEXCACHE entries: textures the game has not drawn with for TF_NATIVE_TEXIDLE
        // seconds (default 30) let go of - their GPU copies with them (nr_d3d.cpp prune()). Kept for
        // good, every level visited added its whole set (2026-10-28, TODO 50: 47 GB of RAM and 9.8 GB
        // of video memory on a warp loop); the texture destructor hook now empties it per level.
        // Frames already handed to the renderer hold their own references; one drawn again later is
        // simply converted (and its large image loaded) again.
        void evictTextures(uint64_t tick)
        {
            static const uint64_t idle = [] {
                const char *e = std::getenv("TF_NATIVE_TEXIDLE");
                const long s = e && *e ? std::atol(e) : 30;
                return static_cast<uint64_t>(std::max(5L, s)) * 60u;
            }();
            static uint64_t lastScan = 0;
            if (tick < lastScan + 120u)
                return;
            // Only once the cache holds more than one place's set (TF_NATIVE_TEXCACHE, default 1500
            // entries - a level ~1,100, the HQ ~150): within one level or the HQ nothing is let go of
            // (idle eviction there made the HQ flash, item 55); after a level change the old level's
            // idle textures go. (The destructor hook alone did not do it: a level's textures are freed
            // with its memory, not one by one - 2026-10-28, 16.7 GB of video memory on a warp loop.)
            static const size_t cap = [] {
                const char *e = std::getenv("TF_NATIVE_TEXCACHE");
                return static_cast<size_t>(e && *e ? std::max(500L, std::atol(e)) : 1500L);
            }();
            // (TODO 124) The large image of a texture let go of is kept TF_NATIVE_LARGEKEEP seconds (default 600; 0:
            // not kept, as before) or until the video memory runs short (evictLarge); drawn again meanwhile, the
            // texture takes it at once.
            static const uint64_t keep = [] {
                const char *e = std::getenv("TF_NATIVE_LARGEKEEP");
                return static_cast<uint64_t>(std::max(0L, e && *e ? std::atol(e) : 600L)) * 60u;
            }();
            if (keep && tick >= lastScan + 120u)
            {
                std::lock_guard<std::recursive_mutex> lock(g_texMutex);
                for (auto &kv : g_shared)
                    if (kv.second.kept && kv.second.keptAt + keep < tick)
                        kv.second.kept.reset();
            }
            // (TODO 55) By place, not by time (the default; TF_NATIVE_TEXEVICT=time: the idle rule above, as before): ten
            // seconds after the game moved to another place (placeChanged: a level loaded, the HQ entered or left) the
            // textures not drawn since it did are let go of - the old place's set; within a place nothing is, however
            // long a texture waits to be drawn again (the HQ's pieces that come and go: flashes when they came back
            // re-made), unless the cache passes TF_NATIVE_TEXHARD entries (6000; the GPU has 8,192 slots): then the
            // ones drawn least lately, idle a minute or more, down to 90% of it.
            static const bool byTime = std::getenv("TF_NATIVE_TEXEVICT") && std::string(std::getenv("TF_NATIVE_TEXEVICT")) == "time";
            static const size_t hard = [] {
                const char *e = std::getenv("TF_NATIVE_TEXHARD");
                return static_cast<size_t>(e && *e ? std::max(1000L, std::atol(e)) : 6000L);
            }();
            uint64_t evictBefore = 0u; // (an entry last drawn before this goes)
            const char *why = "idle";
            if (!byTime)
            {
                if (g_placeChanged.exchange(false, std::memory_order_acq_rel))
                    g_placeStart = tick, g_placePending = true;
                if (g_placePending && tick >= g_placeStart + 600u)
                    evictBefore = g_placeStart, g_placePending = false, why = "of the place before";
                else if (g_textures.size() > hard)
                {
                    std::lock_guard<std::recursive_mutex> lock(g_texMutex);
                    std::vector<uint64_t> ticks;
                    ticks.reserve(g_textures.size());
                    for (const auto &kv : g_textures)
                        ticks.push_back(kv.second.lastTick);
                    const size_t drop = g_textures.size() - hard * 9u / 10u;
                    std::nth_element(ticks.begin(), ticks.begin() + static_cast<std::ptrdiff_t>(drop - 1u), ticks.end());
                    evictBefore = std::min(ticks[drop - 1u] + 1u, tick > 3600u ? tick - 3600u : 0u);
                    why = "least lately drawn (over the hard cap)";
                }
                if (tick >= lastScan + 120u)
                    lastScan = tick;
                if (!evictBefore)
                    return;
            }
            else if (g_textures.size() <= cap)
            {
                if (tick >= lastScan + 120u)
                    lastScan = tick;
                return;
            }
            else
                evictBefore = tick > idle ? tick - idle : 0u;
            lastScan = tick;
            std::lock_guard<std::recursive_mutex> lock(g_texMutex);
            size_t dropped = 0, kept = 0;
            for (auto it = g_textures.begin(); it != g_textures.end();)
                if (it->second.lastTick < evictBefore)
                {
                    const TextureEntry &e = it->second;
                    if (keep && e.content && e.game && e.texture && e.texture->upscaled)
                        if (auto sh = g_shared.find(e.content); sh != g_shared.end())
                        {
                            sh->second.kept = e.texture;
                            sh->second.keptAt = tick;
                            ++kept;
                        }
                    it = g_textures.erase(it);
                    ++dropped;
                }
                else
                    ++it;
            static int told = 8;
            if (dropped && told > 0)
            {
                --told;
                std::fprintf(stderr, "[native render] %zu textures (%s) let go of, %zu kept (tick %llu); %zu of their large images held for re-use\n",
                             dropped, why, g_textures.size(), static_cast<unsigned long long>(tick), kept);
            }
        }

        // Large images asked for ahead of any draw (TODO 57: prefetchByScan), by name and the game image's
        // size: the game texture they were made from, the large one once loaded, its ticket meanwhile.
        struct Prefetched
        {
            std::shared_ptr<const Texture> game, large;
            uint64_t ticket = 0;
        };
        std::unordered_map<std::string, Prefetched> g_prefetched;
        constexpr uint64_t kPrefetchTicket = 1ull << 62;
        std::unordered_map<uint64_t, std::shared_ptr<const Texture>> g_prefetchGame; // (texture | its level's data << 32)

        // The large images loaded since: each in place of the game's texture it was asked for (if the
        // entry still holds that one) - or, asked for ahead, kept for the draw that will want it.
        void takeInUpscaled()
        {
            std::lock_guard<std::recursive_mutex> lock(g_texMutex);
            auto ready = takeUpscaled();
            if (ready.empty())
                return;
            for (auto &r : ready)
            {
                if (r.ticket & kPrefetchTicket)
                {
                    if (r.large)
                        for (auto &p : g_prefetched)
                            if (p.second.ticket == r.ticket)
                            {
                                r.large->id = g_nextId++;
                                p.second.large = std::move(r.large);
                                break;
                            }
                    continue;
                }
                // (Every entry waiting on the ticket - the same content's - takes the one large image.)
                SharedLarge *s = nullptr;
                uint64_t ck = 0;
                if (auto tc = g_ticketContent.find(r.ticket); tc != g_ticketContent.end())
                {
                    ck = tc->second;
                    s = &g_shared[ck];
                    g_ticketContent.erase(tc);
                    if (s->ticket == r.ticket)
                        s->ticket = 0;
                }
                if (!r.large)
                {
                    // No large image (rejected: not asked for again), or the asking entry's game texture let go of
                    // before its turn - asked for again with a waiting one's.
                    if (s && r.missing)
                    {
                        s->missing = true; // (TODO 219: its HD image not made yet - asked for again when the set grows)
                        s->missingGen = s->askedGen;
                    }
                    else if (s && !r.dropped)
                        s->rejected = true;
                    for (auto &e : g_textures)
                        if (e.second.ticket == r.ticket)
                        {
                            e.second.ticket = 0;
                            if (s && r.dropped && e.second.texture && !e.second.texture->upscaled)
                            {
                                if (!s->ticket)
                                {
                                    s->ticket = ++g_lastTicket;
                                    g_ticketContent[s->ticket] = ck;
                                    requestUpscaled(s->ticket, s->name, e.second.texture);
                                }
                                e.second.ticket = s->ticket;
                            }
                        }
                    continue;
                }
                r.large->id = g_nextId++;
                const std::shared_ptr<const Texture> large = std::move(r.large);
                const double now = nowMs();
                for (auto &e : g_textures)
                    if (e.second.ticket == r.ticket)
                    {
                        e.second.game = e.second.texture;
                        e.second.texture = large;
                        e.second.ticket = 0;
                        ++g_texturesUpscaled;
                        if (e.second.askedAt > 0.0)
                            g_waitMs.push_back(static_cast<float>(now - e.second.askedAt));
                    }
                if (s)
                {
                    s->large = large;
                    g_contentLoaded.insert(ck);
                    if (sameFrameLarge())
                        publishLarge(ck, large);
                }
            }
        }

        std::shared_ptr<const Texture> convertKey(const uint8_t *rdram, uint64_t tex0, const TextureKey &key);

        // (TODO 124) Every 10 s, when any texture waited for its large image: how long, from the draw that asked
        // to the large image taken in (median, 95th percentile, longest), how many had one at once (shared or
        // kept), and how many large images were asked for again after being let go of. TF_HIRES_WAITLOG=0: never.
        void reportLargeWaits()
        {
            static const bool on = !(std::getenv("TF_HIRES_WAITLOG") && *std::getenv("TF_HIRES_WAITLOG") == '0');
            static double last = 0.0;
            const double now = nowMs();
            if (!on || now < last + 10000.0)
                return;
            last = now;
            std::lock_guard<std::recursive_mutex> lock(g_texMutex);
            if (g_waitMs.empty() && !g_largeAskedAgain)
            {
                g_largeAtOnce = g_largeKeptUsed = 0;
                return;
            }
            std::vector<float> w = g_waitMs;
            std::sort(w.begin(), w.end());
            const auto at = [&w](double q) { return w.empty() ? 0.0 : static_cast<double>(w[std::min(w.size() - 1u, static_cast<size_t>(q * static_cast<double>(w.size())))]); };
            unsigned queued = 0, heavy = 0;
            uint64_t compressed = 0, pngs = 0;
            largeImageActivity(queued, heavy, compressed, pngs);
            static uint64_t lastDone = 0, lastQueue = 0, lastWork = 0, lastCache = 0, lastWorkCache = 0, lastPngs = 0;
            uint64_t done = 0, queueUs = 0, workUs = 0, doneCache = 0, workCacheUs = 0;
            largeImageTimes(done, queueUs, workUs, doneCache, workCacheUs);
            const uint64_t nd = done - lastDone, nc = doneCache - lastCache;
            std::fprintf(stderr, "[native render] large images in view: %zu waited (median %.0f ms, 95%% %.0f ms, longest %.0f ms), %llu at once (%llu of them kept ones), %llu asked for again; "
                                 "the loaders: %llu done, %.0f ms in the queue and %.0f ms of work on average (%llu from the cache, %.0f ms each; %llu PNGs); %u queued now\n",
                         w.size(), at(0.5), at(0.95), w.empty() ? 0.0 : static_cast<double>(w.back()), static_cast<unsigned long long>(g_largeAtOnce),
                         static_cast<unsigned long long>(g_largeKeptUsed), static_cast<unsigned long long>(g_largeAskedAgain), static_cast<unsigned long long>(nd),
                         nd ? (queueUs - lastQueue) / 1000.0 / static_cast<double>(nd) : 0.0, nd ? (workUs - lastWork) / 1000.0 / static_cast<double>(nd) : 0.0,
                         static_cast<unsigned long long>(nc), nc ? (workCacheUs - lastWorkCache) / 1000.0 / static_cast<double>(nc) : 0.0,
                         static_cast<unsigned long long>(pngs - lastPngs), queued);
            lastDone = done, lastQueue = queueUs, lastWork = workUs, lastCache = doneCache, lastWorkCache = workCacheUs, lastPngs = pngs;
            g_waitMs.clear();
            g_largeAtOnce = g_largeAskedAgain = g_largeKeptUsed = 0;
        }

        // An MSL std::string in guest memory (msl/string.h: bit 0 of the first word set = long form,
        // the characters at +8's pointer; else inline from +1, its size the first byte >> 1).
        std::string guestString(const uint8_t *rdram, uint32_t at)
        {
            const uint32_t w0 = word(rdram, at);
            std::string out;
            if (w0 & 1u)
            {
                const uint32_t size = std::min(word(rdram, at + 4u), 512u), data = word(rdram, at + 8u);
                for (uint32_t i = 0; i < size; ++i)
                    out.push_back(static_cast<char>(word(rdram, data + i) & 0xFFu));
            }
            else
            {
                const uint32_t size = std::min((w0 & 0xFFu) >> 1, 11u);
                for (uint32_t i = 0; i < size; ++i)
                    out.push_back(static_cast<char>(word(rdram, at + 1u + i) & 0xFFu));
            }
            return out;
        }

        // TF_NATIVE_TEXDUMP=1: each converted texture to runs/native_tex/ (BMP, alpha dropped).
        bool g_texDump = false;
        void dumpTexture(const Texture &t, const TextureKey &key, uint64_t tex0)
        {
            char name[160];
            std::snprintf(name, sizeof(name), "runs/native_tex/%04llu_psm%02x_%ux%u_tex%08x_%016llx.bmp",
                          static_cast<unsigned long long>(t.id), key.psm, t.width, t.height, key.texture,
                          static_cast<unsigned long long>(tex0));
            FILE *f = std::fopen(name, "wb");
            if (!f)
                return;
            const uint32_t size = t.width * t.height * 4u;
            uint8_t header[54] = {'B', 'M'};
            auto put32 = [&](int at, uint32_t v) { std::memcpy(header + at, &v, 4); };
            put32(2, 54u + size);
            put32(10, 54u);
            put32(14, 40u);
            put32(18, t.width);
            put32(22, t.height);
            header[26] = 1;
            header[28] = 32;
            put32(34, size);
            std::fwrite(header, 1, sizeof(header), f);
            for (uint32_t y = t.height; y-- > 0;)
                for (uint32_t x = 0; x < t.width; ++x)
                {
                    const uint32_t c = t.pixels[static_cast<size_t>(y) * t.width + x];
                    const uint8_t bgra[4] = {static_cast<uint8_t>(c >> 16), static_cast<uint8_t>(c >> 8), static_cast<uint8_t>(c),
                                             static_cast<uint8_t>(c >> 24)};
                    std::fwrite(bgra, 1, 4, f);
                }
            std::fclose(f);
        }

        std::shared_ptr<const Texture> textureFor(const uint8_t *rdram, uint64_t tex0, uint32_t bound);

        // Context 1's texture (TEX0_1 at the block's +0x30) or context 2's (TEX0_2 at +0x70).
        std::shared_ptr<const Texture> textureOf(const uint8_t *rdram, int context = 1)
        {
            std::lock_guard<std::recursive_mutex> lock(g_texMutex);
            const uint32_t at = context == 2 ? 0x70u : 0x30u;
            if (shadowHolds(context == 2 ? 0x07u : 0x06u))
            {
                // TEX0 as written on its own (SetTexture, the front end): the texture SetTexture put at its TBP.
                const uint32_t reg = context == 2 ? 0x07u : 0x06u;
                const uint64_t tex0 = static_cast<uint64_t>(word(rdram, kGsShadow + 8u * reg)) |
                                      static_cast<uint64_t>(word(rdram, kGsShadow + 8u * reg + 4u)) << 32;
                if (!g_registerWrite[reg])
                    return nullptr;
                const uint32_t tbp = static_cast<uint32_t>(tex0) & 0x3FFFu;
                // (TODO 259: the tables as the list stood where this draw is - bindingsTable / uploadsTable / bindSeqTable, the
                // renderer's own copy when it expands the frame - not the game thread's, which has run on by then: a texture
                // the game uploaded at this block later (a streaming slot: Antarctica's ice floors share theirs with the
                // Mini-Con panel and seven more) looked newer than the binding and was drawn in its place.
                // TF_NATIVE_LIVETABLES=1: the game thread's, as before.)
                static const bool liveTables = std::getenv("TF_NATIVE_LIVETABLES") && *std::getenv("TF_NATIVE_LIVETABLES") == '1';
                const auto &bindingsNow = liveTables ? g_tbpTextures : bindingsTable();
                const auto &uploadsNow = liveTables ? g_uploads : uploadsTable();
                const auto &bindSeqNow = liveTables ? g_bindSeq : bindSeqTable();
                auto it = bindingsNow.find(tbp);
                uint32_t bound = it != bindingsNow.end() ? it->second : 0u;
                const uint32_t boundBefore = bound;
                // (TODO 124: a texture uploaded at TBP after SetTexture's last binding there is what the GS holds - the
                // draw came before its own binding was recorded. The HQ with Hot Shot selected, 4:3: his car picture
                // (CHARPIC) uploaded over the label font's pages (6MILL_COL, bound there last), its first draw read the
                // font's binding - font glyphs where the car is; on the ring, the QUIT icon (ICON2) drawn with a room
                // texture (RM_07) - a big grey panel. TF_NATIVE_UPLOADWINS=0: the binding, as before.)
                static const bool uploadWins = !(std::getenv("TF_NATIVE_UPLOADWINS") && *std::getenv("TF_NATIVE_UPLOADWINS") == '0');
                const auto uploadOver = [&](const std::unordered_map<uint32_t, Upload> &uploads, const std::unordered_map<uint32_t, uint64_t> &seqs,
                                            uint32_t b) {
                    if (auto up = uploads.find(tbp); up != uploads.end() && up->second.texture && up->second.texture != b &&
                                                     word(rdram, up->second.texture) == 0x006B4760u &&
                                                     word(rdram, up->second.texture + 0x98u + 4u * std::min(word(rdram, up->second.texture + 0x90u), 15u)) ==
                                                         up->second.level)
                    {
                        const auto s = seqs.find(tbp);
                        if (up->second.seq > (s != seqs.end() ? s->second : 0u))
                            return up->second.texture;
                    }
                    return b;
                };
                if (uploadWins && bound)
                {
                    const uint32_t over = uploadOver(uploadsNow, bindSeqNow, bound);
                    if (over != bound)
                    {
                        bound = over;
                        ++g_boundByUpload;
                    }
                }
                // (Debug, TODO 259: TF_NATIVE_VIEWCHECK=1: a draw whose texture the game thread's live tables would have
                // chosen differently, told with both names - the first 60.)
                static const bool viewCheck = std::getenv("TF_NATIVE_VIEWCHECK") && *std::getenv("TF_NATIVE_VIEWCHECK") == '1';
                if (viewCheck && t_useView)
                {
                    auto lb = g_tbpTextures.find(tbp);
                    uint32_t live = lb != g_tbpTextures.end() ? lb->second : 0u;
                    if (uploadWins && live)
                        live = uploadOver(g_uploads, g_bindSeq, live);
                    static int told = 60;
                    if (live != bound && told > 0)
                    {
                        --told;
                        std::fprintf(stderr, "[native render] view check at tick %llu, tbp %04x: drawn '%s', the live tables say '%s' (binding '%s')\n",
                                     static_cast<unsigned long long>(g_expandTick), tbp, bound ? guestString(rdram, bound + 0x18u).c_str() : "-",
                                     live ? guestString(rdram, live + 0x18u).c_str() : "-",
                                     boundBefore ? guestString(rdram, boundBefore + 0x18u).c_str() : "-");
                    }
                }
                return textureFor(rdram, tex0, bound);
            }
            if (word(rdram, g_stateBlock + at + 8u) != (context == 2 ? 0x07u : 0x06u))
                return nullptr;
            const uint64_t tex0 = static_cast<uint64_t>(word(rdram, g_stateBlock + at)) |
                                  static_cast<uint64_t>(word(rdram, g_stateBlock + at + 4u)) << 32;
            auto &bindings = context == 2 ? g_blockTextures2 : g_blockTextures;
            auto bound = bindings.find(g_stateBlock);
            return textureFor(rdram, tex0, bound != bindings.end() ? bound->second : 0u);
        }

        // The game's own mip levels of texture `t` below its image level `image` (vlTexturePS2: the
        // image levels from +0x90 on, +0x54 of them - the base and the levels a texture group uploads
        // for TEX1's MXL, at MIPTBP1's blocks), each half the one above. TF_NATIVE_GAMEMIPS=0: none
        // (the GPU side's box-filtered chain, as before).
        void gameMipsOf(const uint8_t *rdram, uint32_t t, uint32_t image, TextureKey &key)
        {
            static const bool off = std::getenv("TF_NATIVE_GAMEMIPS") && *std::getenv("TF_NATIVE_GAMEMIPS") == '0';
            if (off || !t || !image || word(rdram, t) != 0x006B4760u)
                return;
            const uint32_t first = std::min(word(rdram, t + 0x90u), 15u), count = word(rdram, t + 0x54u);
            const uint32_t levels = std::min(word(rdram, t + 0x94u), 16u);
            if (count < 2u || count > 8u || word(rdram, t + 0x98u + 4u * first) != image)
                return;
            const uint32_t uw = word(rdram, image + 0x0Cu), uh = word(rdram, image + 0x10u);
            for (uint32_t k = 1; k < count && k <= 3u && first + k < levels; ++k)
            {
                const uint32_t level = word(rdram, t + 0x98u + 4u * (first + k));
                if (!level || word(rdram, level + 0x0Cu) != std::max(1u, uw >> k) || word(rdram, level + 0x10u) != std::max(1u, uh >> k))
                    break;
                key.mip[k - 1u] = level;
            }
            static int mipLog = std::getenv("TF_NATIVE_MIPLOG") ? std::atoi(std::getenv("TF_NATIVE_MIPLOG")) : 0;
            if (mipLog > 0 && key.mip[0])
            {
                --mipLog;
                auto blockOf = [&](uint32_t level) {
                    const uint32_t buffer = level ? word(rdram, level + 0x24u) : 0u;
                    return buffer ? word(rdram, buffer) & 0x3FFFu : 0xFFFFu;
                };
                std::fprintf(stderr, "[native render] game mips: texture %08x '%s' %ux%u psm %x, %u levels from %u: blocks %04x %04x %04x %04x, offsets %u %u %u %u\n", t,
                             guestString(rdram, t + 0x18u).c_str(), uw, uh, key.psm, count, first, blockOf(image), blockOf(key.mip[0]),
                             blockOf(key.mip[1]), blockOf(key.mip[2]), word(rdram, t + 0x58u + 4u * first), word(rdram, t + 0x5Cu + 4u * first),
                             word(rdram, t + 0x60u + 4u * first), word(rdram, t + 0x64u + 4u * first));
            }
        }

        // (Debug, TODO 259: TF_NATIVE_TEXNAMELOG=<text>: each texture whose name holds that text, the first 40 times it is drawn
        // with a new TEX0 - found by its binding or by the upload at TBP.)
        void texNameLog(const uint8_t *rdram, uint32_t texture, uint64_t tex0, bool byBinding)
        {
            static const char *want = std::getenv("TF_NATIVE_TEXNAMELOG");
            if (!want || !*want || !texture || word(rdram, texture) != 0x006B4760u)
                return;
            static int left = 40;
            static std::unordered_set<uint64_t> seen;
            const std::string name = guestString(rdram, texture + 0x18u);
            std::string upper = name;
            for (char &c : upper)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (left <= 0 || upper.find(want) == std::string::npos || !seen.insert(tex0 ^ (static_cast<uint64_t>(texture) << 1)).second)
                return;
            --left;
            std::fprintf(stderr, "[native render] texture '%s' (%08x) drawn at tick %llu with tex0 %016llx, %s\n", name.c_str(), texture,
                         static_cast<unsigned long long>(g_expandTick), static_cast<unsigned long long>(tex0), byBinding ? "its binding" : "the upload at TBP");
        }

        // The texture TEX0 samples: `bound` (the material's, or what SetTexture made this TEX0 of),
        // else the one whose level was uploaded to its block.
        std::shared_ptr<const Texture> textureFor(const uint8_t *rdram, uint64_t tex0, uint32_t bound)
        {
            TextureKey key;
            key.psm = static_cast<uint32_t>(tex0 >> 20) & 0x3Fu;
            key.tbw = static_cast<uint32_t>(tex0 >> 14) & 0x3Fu;
            key.width = 1u << (static_cast<uint32_t>(tex0 >> 26) & 0xFu);
            key.height = 1u << (static_cast<uint32_t>(tex0 >> 30) & 0xFu);
            key.csa = static_cast<uint32_t>(tex0 >> 56) & 0x1Fu;
            key.tbp = static_cast<uint32_t>(tex0) & 0x3FFFu;
            // (A binding to an object no longer a texture - a state block compiled for a level since
            // unloaded: by the upload at TBP, as an unbound draw.)
            if (bound && word(rdram, bound) != 0x006B4760u)
                bound = 0u;
            if (bound)
            {
                // (... or one whose image level is no level any more - a texture freed, its memory kept.)
                const uint32_t level = word(rdram, bound + 0x98u + 4u * std::min(word(rdram, bound + 0x90u), 15u));
                const uint32_t bpp = level ? word(rdram, level + 0x14u) : 0u;
                if (!level || !word(rdram, level + 0x0Cu) || !word(rdram, level + 0x10u) ||
                    (bpp != 4u && bpp != 8u && bpp != 16u && bpp != 24u && bpp != 32u))
                    bound = 0u;
                // (TODO 206: ... or a binding left over from another texture at the same TBP, its image larger than
                // TEX0's window - the HQ warp gate's tunnel: its additive pass, TEX0 64 x 64 at 37C3, bound on most
                // frames to the room's 256 x 256 RM_05 kept at that block, while what the GS holds there is
                // LIGHTING_ZAP0A03, 64 x 64: the tunnel bright and even with a dark frame every 7th, where the PS2
                // and the emulated renderers show the lightning's dimmer, pulsing streaks. Then the texture uploaded
                // at TBP, when it fits. TF_NATIVE_BOUNDFIT=0: the binding, as before.)
                static const bool boundFit = !(std::getenv("TF_NATIVE_BOUNDFIT") && *std::getenv("TF_NATIVE_BOUNDFIT") == '0');
                auto inWindow = [&](uint32_t lv) {
                    return lv && word(rdram, lv + 0x0Cu) <= key.width && word(rdram, lv + 0x10u) <= key.height;
                };
                if (bound && boundFit && !inWindow(level))
                    if (auto up = uploadsTable().find(key.tbp); up != uploadsTable().end() && up->second.texture != bound &&
                                                                up->second.texture && word(rdram, up->second.texture) == 0x006B4760u &&
                                                                inWindow(up->second.level) &&
                                                                word(rdram, up->second.texture + 0x98u + 4u * std::min(word(rdram, up->second.texture + 0x90u), 15u)) ==
                                                                    up->second.level)
                    {
                        bound = up->second.texture;
                        ++g_boundRefit;
                    }
            }
            if (bound)
            {
                // The material's own texture: its image level and its current palette.
                const uint32_t t = bound;
                key.texture = t;
                key.image = word(rdram, t + 0x98u + 4u * std::min(word(rdram, t + 0x90u), 15u));
                if (key.psm == 0x13u || key.psm == 0x14u || key.psm == 0x1Bu || key.psm == 0x24u || key.psm == 0x2Cu)
                {
                    // The palette TEX0's CBP names: of a texture with several ('_multi': the effects'
                    // colour variants, one texture shared by effects of different colours), the one
                    // whose level was uploaded at CBP - the active palette (+0xE0) is whichever an
                    // effect set last, not the one baked into this TEX0 (Alaska's dropship engines:
                    // FX_blueredgreen1_multi drawn with its blue palette, its active one red).
                    // Else the active one.
                    key.clut = word(rdram, t + 0x98u + 4u * std::min(word(rdram, t + 0xE0u), 15u));
                    const uint32_t palettes = std::min(word(rdram, t + 0x90u), 15u);
                    if (palettes > 1u)
                        if (auto clut = uploadsTable().find(static_cast<uint32_t>(tex0 >> 37) & 0x3FFFu); clut != uploadsTable().end())
                            for (uint32_t i = 0; i < palettes; ++i)
                                if (word(rdram, t + 0x98u + 4u * i) == clut->second.level)
                                {
                                    key.clut = clut->second.level;
                                    break;
                                }
                }
                // (Debug: TF_NATIVE_BOUNDLOG=<n>: the first n draws whose bound texture's image is not
                // what was last uploaded at TEX0's TBP - another level or another texture in GS memory.)
                // (TF_NATIVE_BOUNDLOG=<n>,<tbp hex>: only draws at that TBP.)
                static int boundLog = std::getenv("TF_NATIVE_BOUNDLOG") ? std::atoi(std::getenv("TF_NATIVE_BOUNDLOG")) : 0;
                static const uint32_t boundTbp = [] {
                    const char *e = std::getenv("TF_NATIVE_BOUNDLOG");
                    const char *comma = e ? std::strchr(e, ',') : nullptr;
                    return comma ? static_cast<uint32_t>(std::strtoul(comma + 1, nullptr, 16)) : 0xFFFFFFFFu;
                }();
                if (boundLog > 0 && (boundTbp == 0xFFFFFFFFu || boundTbp == key.tbp))
                    if (auto up = uploadsTable().find(key.tbp); up != uploadsTable().end() && (boundTbp != 0xFFFFFFFFu || up->second.level != key.image))
                    {
                        --boundLog;
                        const auto clutUp = uploadsTable().find(static_cast<uint32_t>(tex0 >> 37) & 0x3FFFu);
                        std::fprintf(stderr, "[native render] bound %08x '%s' image %08x clut %08x, but at tbp %04x: texture %08x '%s' level %08x, at cbp: texture %08x level %08x (tex0 %016llx)\n",
                                     t, guestString(rdram, t + 0x18u).c_str(), key.image, key.clut, key.tbp, up->second.texture,
                                     up->second.texture && word(rdram, up->second.texture) == 0x006B4760u ? guestString(rdram, up->second.texture + 0x18u).c_str() : "-",
                                     up->second.level, clutUp != uploadsTable().end() ? clutUp->second.texture : 0u,
                                     clutUp != uploadsTable().end() ? clutUp->second.level : 0u, static_cast<unsigned long long>(tex0));
                    }
                gameMipsOf(rdram, t, key.image, key);
                texNameLog(rdram, t, tex0, true);
                return convertKey(rdram, tex0, key);
            }
            auto image = uploadsTable().find(static_cast<uint32_t>(tex0) & 0x3FFFu);
            if (image == uploadsTable().end())
            {
                ++g_texturesUnknown;
                g_scanAsked = true;
                return nullptr;
            }
            key.texture = image->second.texture;
            key.image = image->second.level;
            {
                // (An upload whose level is no level any more - its texture freed since, the block
                // not uploaded to again: as unknown.)
                const uint32_t bpp = word(rdram, key.image + 0x14u);
                if (!word(rdram, key.image + 0x0Cu) || !word(rdram, key.image + 0x10u) ||
                    (bpp != 4u && bpp != 8u && bpp != 16u && bpp != 24u && bpp != 32u))
                    return nullptr;
            }
            if (key.psm == 0x13u || key.psm == 0x14u || key.psm == 0x1Bu || key.psm == 0x24u || key.psm == 0x2Cu)
            {
                auto clut = uploadsTable().find(static_cast<uint32_t>(tex0 >> 37) & 0x3FFFu);
                if (clut == uploadsTable().end())
                {
                    ++g_texturesUnknown;
                    g_scanAsked = true;
                    return nullptr;
                }
                key.clut = clut->second.level;
            }
            gameMipsOf(rdram, key.texture, key.image, key);
            texNameLog(rdram, key.texture, tex0, false);
            return convertKey(rdram, tex0, key);
        }

        // The depth fog's palette: the CLUT an 8-bit TEX0 reads (the bound texture's palette level,
        // else the upload at its CBP), as a 256-entry texture; cached by TEX0 and the palette's data.
        std::shared_ptr<const Texture> paletteFor(const uint8_t *rdram, uint64_t tex0, uint32_t bound)
        {
            TextureKey key;
            key.psm = static_cast<uint32_t>(tex0 >> 20) & 0x3Fu;
            if (bound)
            {
                key.clut = word(rdram, bound + 0x98u + 4u * std::min(word(rdram, bound + 0xE0u), 15u));
                // (Of a texture with several palettes, the one uploaded at TEX0's CBP - textureFor's rule: the
                // active one, +0xE0, is whichever was set last; the lens filters' texture holds seven.
                // TF_NATIVE_FOGCBP=0: the active one, as before.)
                static const bool activeOnly = std::getenv("TF_NATIVE_FOGCBP") && *std::getenv("TF_NATIVE_FOGCBP") == '0';
                const uint32_t palettes = std::min(word(rdram, bound + 0x90u), 15u);
                if (!activeOnly && palettes > 1u)
                    if (auto clut = uploadsTable().find(static_cast<uint32_t>(tex0 >> 37) & 0x3FFFu); clut != uploadsTable().end())
                        for (uint32_t i = 0; i < palettes; ++i)
                            if (word(rdram, bound + 0x98u + 4u * i) == clut->second.level)
                            {
                                key.clut = clut->second.level;
                                break;
                            }
            }
            else if (auto clut = uploadsTable().find(static_cast<uint32_t>(tex0 >> 37) & 0x3FFFu); clut != uploadsTable().end())
                key.clut = clut->second.level;
            {
                // (Debug: TF_NATIVE_PALLOG=<n>: the first n palette lookups of top-byte reads after tick 9000 -
                // the bound texture, its palettes, the active one, the upload at CBP, the one taken.)
                static int palLog = std::getenv("TF_NATIVE_PALLOG") ? std::atoi(std::getenv("TF_NATIVE_PALLOG")) : 0;
                if (palLog > 0 && g_expandTick > 9000u)
                {
                    --palLog;
                    auto up = uploadsTable().find(static_cast<uint32_t>(tex0 >> 37) & 0x3FFFu);
                    std::fprintf(stderr, "[native render] palette: tick %llu tex0 %016llx bound %08x '%s' palettes %u active %u (level %08x), at CBP %08x (texture %08x), taken %08x\n",
                                 static_cast<unsigned long long>(g_expandTick), static_cast<unsigned long long>(tex0), bound,
                                 bound ? guestString(rdram, bound + 0x18u).c_str() : "", bound ? word(rdram, bound + 0x90u) : 0u,
                                 bound ? word(rdram, bound + 0xE0u) : 0u,
                                 bound ? word(rdram, bound + 0x98u + 4u * std::min(word(rdram, bound + 0xE0u), 15u)) : 0u,
                                 up != uploadsTable().end() ? up->second.level : 0u, up != uploadsTable().end() ? up->second.texture : 0u, key.clut);
                }
            }
            if (!key.clut)
                return nullptr;
            struct Entry
            {
                uint32_t clut = 0, data = 0;
                std::shared_ptr<const Texture> palette;
            };
            static std::unordered_map<uint64_t, Entry> palettes;
            Entry &e = palettes[tex0 ^ (static_cast<uint64_t>(bound) << 32)];
            const uint32_t data = word(rdram, key.clut);
            if (!e.palette || e.clut != key.clut || e.data != data)
            {
                e.clut = key.clut;
                e.data = data;
                e.palette = convertPalette(rdram, key, g_nextId++);
                // (TODO 152) The same colours as the palette last made from this CLUT: that one, its id and GPU texture
                // kept. The lens filters' TEX0 and bound texture change now and then (a new entry above each time; after
                // a level's start nearly every frame), so Antarctica's grade was often a new palette - a new GPU texture,
                // and on a frame with many new textures put off (the frame's cap, nr_frame.cpp textureFor): the grade
                // drawn from GS memory, the whole 3D picture bluish-white for that frame.
                static std::unordered_map<uint32_t, std::shared_ptr<const Texture>> byClut;
                std::shared_ptr<const Texture> &last = byClut[key.clut];
                if (e.palette && last && last->pixels == e.palette->pixels)
                    e.palette = last;
                else if (e.palette)
                    last = e.palette;
            }
            return e.palette;
        }

        // A T8H TEX0's palette (topPalette, nr_types.h): the 8-bit index read from a render target's
        // top byte goes through it on the GPU side.
        std::shared_ptr<const Texture> topPaletteFor(const uint8_t *rdram, uint64_t tex0, uint32_t bound)
        {
            if (((tex0 >> 20) & 0x3Fu) != 0x1Bu)
                return nullptr;
            std::lock_guard<std::recursive_mutex> lock(g_texMutex);
            return paletteFor(rdram, tex0, bound);
        }

        // The draw's context 1 (TEX0_1 of the state block applied last), as textureOf finds its texture.
        std::shared_ptr<const Texture> topPaletteOf(const uint8_t *rdram)
        {
            if (shadowHolds(0x06u))
            {
                if (!g_registerWrite[0x06u])
                    return nullptr;
                const uint64_t tex0 = static_cast<uint64_t>(word(rdram, kGsShadow + 8u * 6u)) |
                                      static_cast<uint64_t>(word(rdram, kGsShadow + 8u * 6u + 4u)) << 32;
                std::lock_guard<std::recursive_mutex> lock(g_texMutex);
                auto it = g_tbpTextures.find(static_cast<uint32_t>(tex0) & 0x3FFFu);
                return topPaletteFor(rdram, tex0, it != g_tbpTextures.end() ? it->second : 0u);
            }
            if (word(rdram, g_stateBlock + 0x38u) != 0x06u)
                return nullptr;
            const uint64_t tex0 = static_cast<uint64_t>(word(rdram, g_stateBlock + 0x30u)) |
                                  static_cast<uint64_t>(word(rdram, g_stateBlock + 0x34u)) << 32;
            if (((tex0 >> 20) & 0x3Fu) != 0x1Bu)
                return nullptr;
            std::lock_guard<std::recursive_mutex> lock(g_texMutex);
            auto bound = g_blockTextures.find(g_stateBlock);
            return paletteFor(rdram, tex0, bound != g_blockTextures.end() ? bound->second : 0u);
        }

        // (TODO 164) The conversion pool: a draw's new (or re-uploaded) texture converted on a worker, not on the game's
        // thread - in the HQ each turn of the ring or level-select picture converted textures for 15-20 ms of a frame
        // (TF_PROFILE_SPIKES: the copy and swizzle, and the large image's content hash). TF_NATIVE_TEXTHREADS=<n>
        // (default 3; 0: on the drawing thread, as before).
        struct ConvertJob
        {
            uint64_t entry = 0, tex0 = 0; // (g_textures' key, TEX0)
            TextureKey key;
            std::shared_ptr<TextureInputs> inputs;
            std::shared_ptr<Texture> out; // (the texture the draws hold: its pixels in when the worker is done)
            std::string name;
            uint32_t data = 0;
            uint64_t generation = 0;
            bool ok = false;
            double ms = 0.0;
            // (stage 2: a prefetch scan's texture - made ahead for the draw that will want it, not a draw's)
            bool scan = false;
            uint64_t scanKey = 0; // (g_prefetchGame's: texture | its level's data << 32)
            bool large = false;   // (a large image of its name exists: asked for when it is back)
        };
        std::mutex g_convertMutex;
        std::condition_variable g_convertWake;
        std::deque<ConvertJob> g_convertJobs;
        std::vector<ConvertJob> g_convertDone;
        std::atomic<uint64_t> g_convertedAsync{0}, g_convertWaits{0};
        std::atomic<uint64_t> g_sameFrameLarge{0}; // (TODO 177: given their large image on the worker)
        std::atomic<uint64_t> g_convertWaitUs{0}, g_convertWorkUs{0};

        int convertThreads()
        {
            static const int n = [] {
                const char *e = std::getenv("TF_NATIVE_TEXTHREADS");
                return std::clamp(e && *e ? std::atoi(e) : 3, 0, 8);
            }();
            return n;
        }
        bool convertAsync() { return convertThreads() > 0; }

        void convertWorker()
        {
            for (;;)
            {
                ConvertJob j;
                {
                    std::unique_lock<std::mutex> lock(g_convertMutex);
                    g_convertWake.wait(lock, [] { return !g_convertJobs.empty(); });
                    j = std::move(g_convertJobs.front());
                    g_convertJobs.pop_front();
                }
                const auto start = std::chrono::steady_clock::now();
                auto made = convertTextureFrom(*j.inputs, j.key, j.out->id);
                Texture &o = *j.out;
                if (made)
                {
                    // (made here and held by no one else: its contents moved into the texture the draws hold)
                    Texture &m = const_cast<Texture &>(*made);
                    o.pixels = std::move(m.pixels);
                    o.mips = std::move(m.mips);
                    o.gameMips = m.gameMips;
                    o.width = m.width;
                    o.height = m.height;
                    o.pixelHash = pixelHash(o);
                    // (TODO 177) Its large image when one is loaded for the same content: drawn from this frame on.
                    // Only where afterConversion would ask for one (a texture's own texels, not a movie's).
                    if (sameFrameLarge() && !j.scan && j.generation == 0u && j.key.texture && !j.name.empty())
                        if (auto live = liveLarge(contentKey(j.name, o)))
                        {
                            o.pending->large = std::move(live);
                            g_sameFrameLarge.fetch_add(1u, std::memory_order_relaxed);
                        }
                }
                j.ok = made != nullptr;
                j.inputs.reset();
                const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
                j.ms = static_cast<double>(us) / 1000.0;
                g_convertWorkUs.fetch_add(static_cast<uint64_t>(us), std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> lock(o.pending->mutex);
                    o.pending->done.store(true, std::memory_order_release);
                }
                o.pending->cv.notify_all();
                g_convertedAsync.fetch_add(1u, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lock(g_convertMutex);
                g_convertDone.push_back(std::move(j));
            }
        }

        void submitConversion(ConvertJob &&j)
        {
            static const bool started = [] {
                for (int k = 0; k < convertThreads(); ++k)
                    std::thread(convertWorker).detach();
                return true;
            }();
            (void)started;
            {
                std::lock_guard<std::mutex> lock(g_convertMutex);
                g_convertJobs.push_back(std::move(j));
            }
            g_convertWake.notify_one();
        }

        // The pixels of the frame's textures there before the renderer reads them (expandFrame's end): a worker's
        // conversion started at the draw, a frame or more before - a wait only when the renderer catches up with it.
        void waitFrameTextures(const Frame &frame)
        {
            for (const Draw &d : frame.draws)
                for (const Texture *t : {d.texture.get(), d.texture2.get()})
                    if (t && t->pending && !t->pending->done.load(std::memory_order_acquire))
                    {
                        const auto start = std::chrono::steady_clock::now();
                        waitTexture(*t);
                        g_convertWaits.fetch_add(1u, std::memory_order_relaxed);
                        g_convertWaitUs.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                                            std::chrono::steady_clock::now() - start)
                                                                            .count()),
                                                  std::memory_order_relaxed);
                    }
        }

        void afterConversion(const uint8_t *rdram, TextureEntry &e, const TextureKey &key, uint64_t tex0, uint32_t data, uint64_t generation,
                             const std::string &name);
        void finishScanned(ConvertJob &j); // (prefetchByScan's, below)

        // (TODO 164) The conversions back from the workers (the frame's start, the game's thread): each entry still holding
        // its texture finished as convertKey would have - or, its texels not convertible, left without one (its draws
        // drawn untextured this once, as a draw of a failed conversion always was).
        void takeConverted(const uint8_t *rdram)
        {
            std::vector<ConvertJob> done;
            {
                std::lock_guard<std::mutex> lock(g_convertMutex);
                done.swap(g_convertDone);
            }
            if (done.empty())
                return;
            std::lock_guard<std::recursive_mutex> lock(g_texMutex);
            for (ConvertJob &j : done)
            {
                if (j.scan)
                {
                    if (j.ok)
                        finishScanned(j);
                    continue;
                }
                if (j.generation != 0u)
                    movie::oldConversion(j.data, j.ms);
                auto it = g_textures.find(j.entry);
                if (it == g_textures.end() || it->second.texture != j.out)
                    continue; // (let go of, or converted again meanwhile)
                TextureEntry &e = it->second;
                if (!j.ok)
                    e.texture = nullptr;
                afterConversion(rdram, e, j.key, j.tex0, j.data, j.generation, j.name);
            }
        }

        // What follows a conversion (convertKey's, or a worker's when it is back - takeConverted): the prefetch's twin of
        // the same texels taken, the large image asked for, the logs and counters. `name`: the texture's, read at the draw.
        void afterConversion(const uint8_t *rdram, TextureEntry &e, const TextureKey &key, uint64_t tex0, uint32_t data, uint64_t generation,
                             const std::string &name)
        {
            // (Made ahead by the prefetch with the same texels: that one, already on its way to the GPU.)
            if (e.texture && !e.texture->gameMips && key.texture)
                if (auto pg = g_prefetchGame.find(static_cast<uint64_t>(key.texture) | static_cast<uint64_t>(data) << 32); pg != g_prefetchGame.end())
                {
                    if (pg->second->width == e.texture->width && pg->second->height == e.texture->height && pg->second->pixels == e.texture->pixels)
                        e.texture = pg->second;
                    g_prefetchGame.erase(pg);
                }
            if (e.texture && key.texture && generation == 0u)
            {
                // The large image on the loader's thread; the game's own drawn until it comes.
                // (Not for texels that change - a movie, an animated palette: a still image would
                // freeze them.)
                // (Asked for ahead with the same texels: taken from there - at once if loaded.)
                auto pf = g_prefetched.find(name + "#" + std::to_string(e.texture->width) + "x" + std::to_string(e.texture->height));
                if (pf != g_prefetched.end() && pf->second.game && pf->second.game->pixels == e.texture->pixels)
                {
                    if (pf->second.large)
                    {
                        e.game = e.texture;
                        e.texture = pf->second.large;
                        ++g_texturesUpscaled;
                        g_prefetched.erase(pf);
                    }
                    else
                        e.ticket = pf->second.ticket;
                }
                else
                    askLarge(e, name);
            }
            static int names = std::getenv("TF_NATIVE_NAMES") ? std::atoi(std::getenv("TF_NATIVE_NAMES")) : 0;
            if (names > 0 && key.texture)
            {
                --names;
                std::fprintf(stderr, "[native render] texture %08x name '%s' path '%s' palette %u\n", key.texture,
                             guestString(rdram, key.texture + 0x18u).c_str(), guestString(rdram, key.texture + 0x24u).c_str(),
                             word(rdram, key.texture + 0xE0u));
            }
            if (e.texture && g_texDump)
                dumpTexture(*e.texture, key, tex0);
            ++(e.texture ? g_texturesConverted : g_texturesFailed);
            if (e.texture && e.texture->gameMips)
                ++g_texturesGameMips;
            static int failLog = std::getenv("TF_NATIVE_FAILLOG") ? std::atoi(std::getenv("TF_NATIVE_FAILLOG")) : 0;
            if (!e.texture && failLog > 0)
            {
                --failLog;
                const uint32_t lv = key.image;
                std::fprintf(stderr,
                             "[native render] not converted: tex0 %016llx psm %x %ux%u tbw %u, texture %08x '%s', image level %08x (data %08x psm %x %ux%u bpp %u tile %u), clut level %08x (data %08x %ux%u)\n",
                             static_cast<unsigned long long>(tex0), key.psm, key.width, key.height, key.tbw, key.texture,
                             key.texture ? guestString(rdram, key.texture + 0x18u).c_str() : "", lv, lv ? word(rdram, lv) : 0u,
                             lv ? word(rdram, lv + 4u) : 0u, lv ? word(rdram, lv + 0xCu) : 0u, lv ? word(rdram, lv + 0x10u) : 0u,
                             lv ? word(rdram, lv + 0x14u) : 0u, lv ? word(rdram, lv + 0x1Cu) : 0u, key.clut, key.clut ? word(rdram, key.clut) : 0u,
                             key.clut ? word(rdram, key.clut + 0xCu) : 0u, key.clut ? word(rdram, key.clut + 0x10u) : 0u);
            }
        }

        std::shared_ptr<const Texture> convertKey(const uint8_t *rdram, uint64_t tex0, const TextureKey &key)
        {
            const uint32_t data = word(rdram, key.image), clutData = key.clut ? word(rdram, key.clut) : 0u;
            // (Texels rewritten in place and uploaded again - the HQ's level previews, a movie decoded
            // into a vlTexturePS2 every frame (the player clears its levels' uploaded flags, 0x266570);
            // a palette animated: converted again. Before 2026-10-28 the first frame stayed - TODO 58.
            // TF_NATIVE_REUPLOAD=0: as before.)
            static const bool reuploadOff = std::getenv("TF_NATIVE_REUPLOAD") && *std::getenv("TF_NATIVE_REUPLOAD") == '0';
            // (Roadmap N6: a movie's picture comes from the decoder, named by its own generation - nr_movie.cpp.)
            uint64_t movieGeneration = 0;
            const bool moviePicture = !key.clut && movie::holds(key.texture, data, movieGeneration);
            TextureEntry &e = g_textures[tex0 ^ (static_cast<uint64_t>(key.texture) << 32)];
            // (TODO 94: the levels' generations asked only when one changed since this entry last asked, or its key did)
            const uint64_t epoch = snapshotEpoch();
            const bool sameLevels = e.tried && e.epoch == epoch && e.key.image == key.image && e.key.clut == key.clut;
            const uint64_t generation = moviePicture ? movieGeneration
                                        : reuploadOff ? 0u
                                        : sameLevels  ? e.generation
                                                      : (static_cast<uint64_t>(levelGeneration(key.image)) | static_cast<uint64_t>(key.clut ? levelGeneration(key.clut) : 0u) << 32);
            e.epoch = epoch;
            e.lastTick = g_textureTick;
            if (!e.tried || !(e.key == key) || e.data != data || e.clutData != clutData || e.generation != generation)
            {
                e.generation = generation;
                static int why = std::getenv("TF_NATIVE_RECONVERT") ? std::atoi(std::getenv("TF_NATIVE_RECONVERT")) : 0;
                if (why > 0 && e.tried)
                {
                    --why;
                    std::fprintf(stderr,
                                 "[native render] reconvert tex0 %016llx: texture %08x->%08x image %08x->%08x clut %08x->%08x psm %x->%x "
                                 "%ux%u->%ux%u data %08x->%08x clutData %08x->%08x\n",
                                 static_cast<unsigned long long>(tex0), e.key.texture, key.texture, e.key.image, key.image, e.key.clut, key.clut,
                                 e.key.psm, key.psm, e.key.width, e.key.height, key.width, key.height, e.data, data, e.clutData, clutData);
                }
                e.tried = true;
                e.key = key;
                e.data = data;
                e.clutData = clutData;
                // A texture kept in GS memory alone (flag 0x1000: a render surface's alias, a depth
                // buffer's; no texels in main memory): not converted - the renderer samples the
                // target drawn at its pages. (Its top-byte views - the light maps - are read below.)
                if (key.texture && (word(rdram, key.texture + 0x30u) & 0x1000u) && !data && key.psm != 0x1Bu && key.psm != 0x24u &&
                    key.psm != 0x2Cu)
                {
                    e.texture = nullptr;
                    e.ticket = 0;
                    ++g_texturesVram;
                    return nullptr;
                }
                // (TODO 124: its large image kept for the texture's next owner of the same content - a group loaded
                // again elsewhere, say: taken at once when the same texels come back, not asked for again.)
                if (e.content && e.texture && e.texture->upscaled)
                    if (auto sh = g_shared.find(e.content); sh != g_shared.end())
                    {
                        sh->second.kept = e.texture;
                        sh->second.keptAt = g_textureTick;
                    }
                if (moviePicture)
                    e.texture = movie::texture(rdram, key, data, g_nextId++);
                if (!moviePicture || !e.texture)
                {
                    // (TODO 164) On a conversion worker: the key's guest memory copied now (headers, texels, palette), the
                    // draw given the texture to come - the renderer waits for its pixels (expandFrame), the rest of this
                    // (the prefetch's twin, the large image asked for, the counters) done when it is back (takeConverted).
                    if (convertAsync())
                        if (auto inputs = captureTextureInputs(rdram, key))
                        {
                            auto placeholder = std::make_shared<Texture>();
                            placeholder->id = g_nextId++;
                            placeholder->source = key.texture;
                            placeholder->width = key.width;
                            placeholder->height = key.height;
                            placeholder->pending = std::make_shared<TexturePending>();
                            e.texture = placeholder;
                            e.ticket = 0;
                            e.game.reset();
                            e.largeLost = false;
                            ConvertJob job;
                            job.entry = tex0 ^ (static_cast<uint64_t>(key.texture) << 32);
                            job.tex0 = tex0;
                            job.key = key;
                            job.inputs = std::move(inputs);
                            job.out = std::move(placeholder);
                            job.name = key.texture ? guestString(rdram, key.texture + 0x18u) : std::string();
                            job.data = data;
                            job.generation = generation;
                            submitConversion(std::move(job));
                            return e.texture;
                        }
                    const auto convertStart = std::chrono::steady_clock::now();
                    e.texture = convertTexture(rdram, key, g_nextId++);
                    if (generation != 0u) // (a level uploaded again - a movie's picture among them: its time for the movie log)
                        movie::oldConversion(data, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - convertStart).count());
                }
                e.ticket = 0;
                e.game.reset();
                e.largeLost = false;
                afterConversion(rdram, e, key, tex0, data, generation, key.texture ? guestString(rdram, key.texture + 0x18u) : std::string());
            }
            else if (e.largeLost && !e.ticket && e.texture && key.texture)
            {
                // (Its large image let go of for the video memory, TODO 94: drawn again - asked for again.)
                e.largeLost = false;
                askLarge(e, guestString(rdram, key.texture + 0x18u));
            }
            return e.texture;
        }

        void captureStateBlock(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t device = getRegU32(ctx, 4), block = getRegU32(ctx, 5);
            g_captureStateBlock(rdram, ctx, runtime);
            // The texture whose image level sits where the captured TEX0_1 points (its level bound to
            // its slot while the group's state blocks compile: block + the texture's offset at 0x58).
            auto match = [&](uint32_t reg) {
                const uint32_t tbp = word(rdram, kGsShadow + 8u * reg) & 0x3FFFu;
                for (auto it = g_recent.rbegin(); it != g_recent.rend(); ++it)
                {
                    const uint32_t t = *it;
                    const uint32_t level = word(rdram, t + 0x98u + 4u * std::min(word(rdram, t + 0x90u), 15u));
                    const uint32_t slot = level ? word(rdram, level + 0x24u) : 0u;
                    if (slot && ((word(rdram, slot) & 0xFFFFu) + word(rdram, t + 0x58u)) == tbp)
                        return t;
                }
                return 0u;
            };
            const uint32_t found = match(6u), found2 = match(7u);
            const uint32_t kept = word(rdram, device + kDeviceTexture);
            ++(found ? g_bindExact : g_bindGuess);
            std::lock_guard<std::recursive_mutex> lock(g_texMutex);
            g_blockTextures[block] = found ? found : (g_requested[0] ? g_requested[0] : kept);
            g_blockTextures2[block] = found2 ? found2 : (g_requested[1] ? g_requested[1] : 0u);
            g_recent.clear();
            g_prefetchBlocks.push_back(block); // (its textures made, and their large images asked for, at the next frame)
        }

        // Every texture in main memory with a large image of its name: converted from its own image level
        // (and active palette) and its large image asked for now, kept by name and size (g_prefetched) for
        // the draw that will want it (convertKey takes it when the texels match). Run a second and three
        // seconds after a level's (or the HQ's) textures change: the HQ's screens' characters and room had
        // the game's textures for seconds after each screen opened (TODO 57; the HQ compiles no state
        // blocks that prefetchBlockTextures could take them from). TF_NATIVE_PREFETCH=1: on - OFF by default
        // (2026-10-29, TODO 76/77): measured on the test PC it made the HQ worse - hundreds of textures and
        // large images queued at once kept the GPU's copy queue busy (55-60 ms frames for seconds), and the
        // screen-change frames still made ~50 textures (the draws' keys did not take the prefetched ones).
        std::vector<uint64_t> g_prefetchScanTicks;
        uint32_t g_destroyedSince = 0;
        std::unordered_set<uint64_t> g_prefetchSeen;
        uint64_t g_prefetchTickets = 0;
        std::string guestString(const uint8_t *rdram, uint32_t at);
        // (TODO 164 stage 2) TF_NATIVE_PREFETCH=1: each texture made ahead when the game uploads its image
        // (uploadLevel - a level's are uploaded while its load screen shows) on the conversion pool, its large image asked
        // for when it is back: in place of the memory scans - a walk of the 256 MB of guest memory took 30 ms each, every
        // half second while the HQ held the load flag. =scan: the scans as before (a second and three seconds after a level's
        // textures change); =0 (the default): neither. Off by default as it measured (test PC, Amazon's opening, the pool
        // either way): every uploaded texture's large image asked for put 297 jobs before the loaders against 105, and the
        // textures in view waited longer for theirs (median 199 ms against 69, 95% 322 against 142); the draws convert their
        // own key anyway (another TEX0 than the upload's), so the renderer waited for as many conversions.
        int prefetchMode()
        {
            static const int mode = [] {
                const char *e = std::getenv("TF_NATIVE_PREFETCH");
                if (!e || !*e)
                    return 0; // (off by default: measured worse - above)
                if (std::string(e) == "scan")
                    return 2;
                return *e == '0' ? 0 : 1;
            }();
            return mode;
        }

        // One texture made ahead (a vlTexturePS2 at `a`): its image converted as a draw of its own key would, kept for the
        // draw (convertKey takes it when the texels match) and put on the GPU; its large image asked for by name. 1: made
        // ahead (or on its way), 2: and its large image asked for, 0: not (seen already, or not a texture of texels).
        int prefetchOne(const uint8_t *rdram, uint32_t a)
        {
            const uint32_t level = word(rdram, a + 0x98u + 4u * std::min(word(rdram, a + 0x90u), 15u));
            const uint32_t bpp = level ? word(rdram, level + 0x14u) : 0u;
            const uint32_t uw = level ? word(rdram, level + 0x0Cu) : 0u, uh = level ? word(rdram, level + 0x10u) : 0u;
            if (!level || !uw || !uh || uw > 2048u || uh > 2048u || (bpp != 4u && bpp != 8u && bpp != 16u && bpp != 24u && bpp != 32u))
                return 0;
            const uint32_t data = word(rdram, level);
            if (!data || !g_prefetchSeen.insert(static_cast<uint64_t>(a) | static_cast<uint64_t>(data) << 32).second)
                return 0;
            const std::string name = guestString(rdram, a + 0x18u);
            const bool large = hasUpscaled(name);
            TextureKey key;
            key.texture = a;
            key.image = level;
            key.psm = bpp == 4u ? 0x14u : bpp == 8u ? 0x13u : bpp == 16u ? 0x02u : bpp == 24u ? 0x01u : 0x00u;
            key.width = uw;
            key.height = uh;
            if (bpp <= 8u)
                key.clut = word(rdram, a + 0x98u + 4u * std::min(word(rdram, a + 0xE0u), 15u));
            // (on the conversion pool: the rest when it is back - finishScanned)
            if (convertAsync())
                if (auto inputs = captureTextureInputs(rdram, key))
                {
                    auto out = std::make_shared<Texture>();
                    out->id = g_nextId++;
                    out->source = key.texture;
                    out->width = key.width;
                    out->height = key.height;
                    out->pending = std::make_shared<TexturePending>();
                    ConvertJob job;
                    job.scan = true;
                    job.key = key;
                    job.inputs = std::move(inputs);
                    job.out = std::move(out);
                    job.name = name;
                    job.scanKey = static_cast<uint64_t>(a) | static_cast<uint64_t>(data) << 32;
                    job.large = large;
                    submitConversion(std::move(job));
                    return 1;
                }
            auto game = convertTexture(rdram, key, g_nextId++);
            if (!game)
                return 0;
            // (The game texture itself kept for the draw - convertKey takes it when its texels match -
            // and put on the GPU now, not in the frame that first draws it: TODO 76/77.)
            g_prefetchGame[static_cast<uint64_t>(a) | static_cast<uint64_t>(data) << 32] = game;
            preUpload(game);
            if (!large)
                return 1;
            const std::string id = name + "#" + std::to_string(game->width) + "x" + std::to_string(game->height);
            if (g_prefetched.count(id))
                return 1;
            Prefetched p;
            p.game = game;
            p.ticket = kPrefetchTicket | ++g_prefetchTickets;
            requestUpscaled(p.ticket, name, game);
            g_prefetched.emplace(id, std::move(p));
            return 2;
        }

        void prefetchByScan(const uint8_t *rdram)
        {
            if (prefetchMode() != 2)
                return;
            const auto start = std::chrono::steady_clock::now();
            std::lock_guard<std::recursive_mutex> lock(g_texMutex);
            size_t textures = 0, asked = 0, made = 0;
            for (uint32_t a = 0x00100000u; a < PS2_RAM_SIZE; a += 4u)
            {
                uint32_t v;
                std::memcpy(&v, rdram + a, 4);
                if (v != 0x006B4760u)
                    continue;
                ++textures;
                const int r = prefetchOne(rdram, a);
                made += r > 0 ? 1u : 0u;
                asked += r == 2 ? 1u : 0u;
            }
            static int told = 24;
            if (told > 0)
            {
                --told;
                std::fprintf(stderr, "[native render] prefetch scan: %zu textures, %zu made ahead%s, %zu large images asked for ahead, %.1f ms\n", textures, made,
                             convertAsync() ? " (on the conversion pool)" : "", asked,
                             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            }
        }

        // A scanned texture back from the pool (takeConverted, the game's thread): kept for its draw and put on the GPU,
        // its large image asked for - as prefetchByScan did at once.
        void finishScanned(ConvertJob &j)
        {
            std::shared_ptr<const Texture> game = j.out;
            g_prefetchGame[j.scanKey] = game;
            preUpload(game);
            if (!j.large)
                return;
            const std::string id = j.name + "#" + std::to_string(game->width) + "x" + std::to_string(game->height);
            if (g_prefetched.count(id))
                return;
            Prefetched p;
            p.game = game;
            p.ticket = kPrefetchTicket | ++g_prefetchTickets;
            requestUpscaled(p.ticket, j.name, game);
            g_prefetched.emplace(id, std::move(p));
        }

        // The state blocks a level's (or the HQ's) groups compiled: their textures made at once - converted and
        // their large images asked for (convertKey) - instead of when a draw first uses them, which left each
        // HQ screen on the game's small textures for ~3 s while the loaders caught up (TODO 57). The same key
        // a draw builds (textureFor with the block's TEX0 and binding), so the draw finds the entry made.
        // TF_NATIVE_PREFETCH=1: on (off by default, as prefetchByScan).
        void prefetchBlockTextures(const uint8_t *rdram)
        {
            static const bool off = prefetchMode() != 2; // (=scan, as before: with the memory scans)
            if (off || g_prefetchBlocks.empty())
            {
                g_prefetchBlocks.clear();
                return;
            }
            std::lock_guard<std::recursive_mutex> lock(g_texMutex);
            const auto start = std::chrono::steady_clock::now();
            const uint64_t ticketsBefore = g_lastTicket;
            size_t blocks = 0, made = 0;
            std::vector<uint32_t> list;
            list.swap(g_prefetchBlocks);
            for (const uint32_t block : list)
            {
                ++blocks;
                for (int context = 1; context <= 2; ++context)
                {
                    const uint32_t at = context == 2 ? 0x70u : 0x30u;
                    if (word(rdram, block + at + 8u) != (context == 2 ? 0x07u : 0x06u))
                        continue;
                    const uint64_t tex0 = static_cast<uint64_t>(word(rdram, block + at)) | static_cast<uint64_t>(word(rdram, block + at + 4u)) << 32;
                    auto &bindings = context == 2 ? g_blockTextures2 : g_blockTextures;
                    auto bound = bindings.find(block);
                    if (bound == bindings.end() || !bound->second)
                        continue;
                    made += textureFor(rdram, tex0, bound->second) ? 1u : 0u;
                }
            }
            static int told = 16;
            if (told > 0 && blocks)
            {
                --told;
                std::fprintf(stderr, "[native render] prefetch: %zu state blocks, %zu textures made, %llu large images asked for, %.1f ms\n", blocks, made,
                             static_cast<unsigned long long>(g_lastTicket - ticketsBefore),
                             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            }
        }

        // (N5.1, roadmap N5: the renderer off the display list) TF_N5_NOLIST: the GS registers the renderer takes where a
        // draw comes (TODO 61's overrides, a microcode primitive's state, TEX1, the dash's after-image) from the device's
        // calls - SetGSRegister, its pair, the state blocks applied: Frame::regStream - instead of the display list's
        // packets (=1). Unset / 0: the list's, as before; =check: both kept, compared at every draw ([n5:nolist]; with
        // TF_N5_NOLIST_LOG=<tick> one frame's writes side by side). Not complete: texture packets the list CALLs, the
        // render environment's and the 2D's registers are not in the stream yet (TF_N5_CENSUS_REGS: who writes what).
        int noListMode()
        {
            static const int mode = [] {
                const char *e = std::getenv("TF_N5_NOLIST");
                if (!e || !*e)
                    return 0; // (off by default: the stream does not yet hold every register the list sets - see =check)
                if (std::strcmp(e, "check") == 0)
                    return 2;
                return *e == '0' ? 0 : 1;
            }();
            return mode;
        }

        void streamRegister(uint32_t reg, uint64_t value)
        {
            if (noListMode() != 0)
                g_frame.regStream.push_back(Frame::RegWrite{static_cast<uint32_t>(g_frame.draws.size()), reg & 0x7Fu, value});
        }

        // A state block's registers (its GIF packet: a tag, then NLOOP A+D qwords - value, register), as the DMA call of
        // it the block's application puts in the list sets them.
        void streamStateBlock(const uint8_t *rdram, uint32_t block)
        {
            if (noListMode() == 0 || !block || block >= PS2_RAM_SIZE)
                return;
            const uint32_t tag0 = word(rdram, block), tag1 = word(rdram, block + 4u);
            const uint32_t nloop = tag0 & 0x7FFFu, flg = (tag1 >> 26) & 3u, nreg = (tag1 >> 28) & 0xFu;
            if (flg != 0u || nreg != 1u || (word(rdram, block + 8u) & 0xFu) != 0xEu || nloop > 64u)
                return; // (not an A+D packet: nothing known to take)
            for (uint32_t i = 0; i < nloop; ++i)
            {
                const uint32_t at = block + 16u + 16u * i;
                streamRegister(word(rdram, at + 8u), static_cast<uint64_t>(word(rdram, at)) | static_cast<uint64_t>(word(rdram, at + 4u)) << 32);
            }
        }

        // ---- N5.3: the device's functions native (nr_n5.h: TF_N5_DEVICE) ----
        // A register the native functions set (no packet): applied to the list's GS state where the packet would have
        // been (expandFrame). The GS shadow is kept as the originals keep it.
        constexpr uint32_t kNativeBlock = 0x80u; // (Frame::NativeWrite.reg: a state block's GIF packet - value: its qwords << 32 | address)
        void nativeRegister(const uint8_t *rdram, uint32_t reg, uint64_t value)
        {
            g_frame.nativeRegs.push_back(Frame::NativeWrite{static_cast<uint32_t>(g_frame.draws.size()), word(rdram, kPacketPointer) & 0x1FFFFFFFu,
                                                            reg == kNativeBlock ? reg : reg & 0x7Fu, value});
        }
        void store64(uint8_t *rdram, uint32_t addr, uint64_t value)
        {
            if (uint8_t *at = getMemPtr(rdram, addr))
                std::memcpy(at, &value, 8u);
        }
        void store32(uint8_t *rdram, uint32_t addr, uint32_t value)
        {
            if (uint8_t *at = getMemPtr(rdram, addr))
                std::memcpy(at, &value, 4u);
        }
        uint64_t load64(const uint8_t *rdram, uint32_t addr)
        {
            return static_cast<uint64_t>(word(rdram, addr)) | static_cast<uint64_t>(word(rdram, addr + 4u)) << 32;
        }

        // (Back to the originals - a lost device, the game's own screen: the GS gets the packets again.) The registers the
        // native SetGSRegister kept from it since, written to it once from the shadow, through the original.
        uint64_t g_nativeRegsDirty[2] = {}; // (a bit a register, 0x00..0x7F)
        void resyncRegisters(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!(g_nativeRegsDirty[0] | g_nativeRegsDirty[1]))
                return;
            const __m128i a0 = ctx->r[4], a1 = ctx->r[5];
            for (uint32_t reg = 0; reg < 0x80u; ++reg)
                if (g_nativeRegsDirty[reg >> 6] >> (reg & 63u) & 1u)
                {
                    SET_GPR_U64(ctx, 4, reg);
                    SET_GPR_U64(ctx, 5, load64(rdram, kGsShadow + 8u * reg));
                    g_writeGsRegister(rdram, ctx, runtime);
                }
            g_nativeRegsDirty[0] = g_nativeRegsDirty[1] = 0u;
            ctx->r[4] = a0, ctx->r[5] = a1;
            static int told = 4;
            if (told > 0)
            {
                --told;
                std::fprintf(stderr, "[n5:device] the GS's registers written again from the shadow (the originals back)\n");
            }
        }

        void applyStateBlock(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_stateBlock = getRegU32(ctx, 5);
            g_blockWrite = ++g_writeSequence;
            streamStateBlock(rdram, g_stateBlock);
            if (framesDeferred() && n5::deviceNative(n5::Device::ApplyStateBlock))
            {
                // vlRenderDevicePS2::ApplyStateBlock (slot 39, 0x002591F0) native: the block's material colour (+0xA4..+0xB0,
                // when its byte +0xA0 is set) made the current one (D_70000E8C) - the shaders' contexts marked dirty
                // (D_70000460 |= D_70000E88) when it differs - and the block's registers (its GIF packet, A+D) handed to the
                // renderer instead of the DMA REF of it into the list.
                const uint32_t block = g_stateBlock;
                const uint32_t pointer = word(rdram, kPacketPointer);
                uint64_t shadowRegs[2] = {};
                if ((word(rdram, block + 0xA0u) & 0xFFu) != 0u)
                {
                    const uint32_t current = word(rdram, 0x70000E8Cu);
                    float now[4], wanted[4];
                    for (int i = 0; i < 4; ++i)
                    {
                        const uint32_t n = word(rdram, current + 4u * static_cast<uint32_t>(i)), w = word(rdram, block + 0xA4u + 4u * static_cast<uint32_t>(i));
                        std::memcpy(&now[i], &n, 4u);
                        std::memcpy(&wanted[i], &w, 4u);
                    }
                    if (!(now[1] == wanted[1] && now[2] == wanted[2] && now[3] == wanted[3] && now[0] == wanted[0]))
                    {
                        store32(rdram, 0x70000460u, word(rdram, 0x70000460u) | word(rdram, 0x70000E88u));
                        for (int i = 0; i < 4; ++i)
                            store32(rdram, current + 4u * static_cast<uint32_t>(i), word(rdram, block + 0xA4u + 4u * static_cast<uint32_t>(i)));
                    }
                }
                const uint32_t tag0 = word(rdram, block), tag1 = word(rdram, block + 4u);
                const uint32_t nloop = tag0 & 0x7FFFu;
                const bool adPacket = ((tag1 >> 26) & 3u) == 0u && ((tag1 >> 28) & 0xFu) == 1u && (word(rdram, block + 8u) & 0xFu) == 0xEu && nloop <= 64u;
                if (adPacket) // (the block itself, read by the renderer's thread as the list's REF of it was: its packet is fixed)
                    nativeRegister(rdram, kNativeBlock, (static_cast<uint64_t>(nloop + 1u) << 32) | block);
                if (n5::deviceCheck())
                {
                    // The original into the list, compared (the colour work is the same both ways: run again it finds it
                    // done), then taken back out.
                    const uint32_t flags = word(rdram, 0x70000460u);
                    const uint32_t count0 = ctx->cop0_count;
                    g_applyStateBlock(rdram, ctx, runtime);
                    if (!n5::deviceCyclesMatch(runtime, 0u, ctx->cop0_count - count0))
                        n5::deviceMismatch(n5::Device::ApplyStateBlock, "EE cycles", 0u, ctx->cop0_count - count0);
                    const uint32_t after = word(rdram, kPacketPointer);
                    if (after != pointer + 16u)
                        n5::deviceMismatch(n5::Device::ApplyStateBlock, "bytes written", 0u, after - pointer);
                    else if ((word(rdram, pointer + 4u) & 0x7FFFFFF0u) != (block & 0x7FFFFFF0u))
                        n5::deviceMismatch(n5::Device::ApplyStateBlock, "the REF's address", block, word(rdram, pointer + 4u));
                    else if (!adPacket || (word(rdram, pointer) & 0xFFFFu) != nloop + 1u)
                        n5::deviceMismatch(n5::Device::ApplyStateBlock, "the REF's size against the block's packet", nloop + 1u, word(rdram, pointer) & 0xFFFFu);
                    if (word(rdram, 0x70000460u) != flags)
                        n5::deviceMismatch(n5::Device::ApplyStateBlock, "the dirty flags", flags, word(rdram, 0x70000460u));
                    store32(rdram, kPacketPointer, pointer);
                    n5::deviceChecked(n5::Device::ApplyStateBlock);
                }
                (void)shadowRegs;
                n5::deviceReturn(ctx, runtime, 0u); // (the original has no safe point: no call, no loop)
                return;
            }
            g_applyStateBlock(rdram, ctx, runtime);
        }

        void writeGsRegister(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t reg = getRegU32(ctx, 4);
            const uint64_t value = GPR_U64(ctx, 5);
            g_registerWrite[reg & 0x7Fu] = ++g_writeSequence;
            streamRegister(reg, value);
            if (framesDeferred() && n5::deviceNative(n5::Device::SetGSRegister))
            {
                // func_00225D60 (register, value) native: the GS shadow (D_70000060, 8 bytes a register) as the original keeps
                // it, and the register handed to the renderer instead of its packet (a DMA CNT of 3 qwords: tag, GIF tag, A+D).
                const uint32_t pointer = word(rdram, kPacketPointer);
                store64(rdram, kGsShadow + (reg << 3), value);
                nativeRegister(rdram, reg, value);
                g_nativeRegsDirty[(reg >> 6) & 1u] |= 1ull << (reg & 63u);
                if (n5::deviceCheck())
                {
                    const uint32_t count0 = ctx->cop0_count;
                    g_writeGsRegister(rdram, ctx, runtime);
                    if (!n5::deviceCyclesMatch(runtime, 0u, ctx->cop0_count - count0))
                        n5::deviceMismatch(n5::Device::SetGSRegister, "EE cycles", 0u, ctx->cop0_count - count0);
                    const uint32_t after = word(rdram, kPacketPointer);
                    if (after != pointer + 0x30u)
                        n5::deviceMismatch(n5::Device::SetGSRegister, "bytes written", 0x30u, after - pointer);
                    else if (load64(rdram, pointer + 0x20u) != value || (word(rdram, pointer + 0x28u) & 0x7Fu) != (reg & 0x7Fu))
                        n5::deviceMismatch(n5::Device::SetGSRegister, "the A+D written", value, load64(rdram, pointer + 0x20u));
                    if (load64(rdram, kGsShadow + (reg << 3)) != value)
                        n5::deviceMismatch(n5::Device::SetGSRegister, "the shadow", value, load64(rdram, kGsShadow + (reg << 3)));
                    store32(rdram, kPacketPointer, pointer);
                    n5::deviceChecked(n5::Device::SetGSRegister);
                }
                n5::deviceReturn(ctx, runtime, 0u); // (the original has no safe point: no call, no loop)
                return;
            }
            resyncRegisters(rdram, ctx, runtime);
            g_writeGsRegister(rdram, ctx, runtime);
        }

        // func_00225C50 (register, value): the register of both contexts (reg, reg + 1), into the list and the shadow.
        constexpr uint32_t kWriteGsRegisterPair = 0x00225C50u;
        PS2Runtime::RecompiledFunction g_writeGsRegisterPair = nullptr;
        void writeGsRegisterPair(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t reg = getRegU32(ctx, 4);
            streamRegister(reg, GPR_U64(ctx, 5));
            streamRegister(reg + 1u, GPR_U64(ctx, 5));
            g_writeGsRegisterPair(rdram, ctx, runtime);
        }

        // The display list cut where the packet pointer stands (an upload or a binding about to change
        // the texture tables): the part before is read with the tables as they are now (at once, or by
        // the renderer's thread after the changes logged so far).
        void trackUpTo(const uint8_t *rdram, uint32_t to);
        void resolveListStart(const uint8_t *rdram);
        void splitListHere(const uint8_t *rdram)
        {
            static const bool off = std::getenv("TF_NATIVE_LISTCUT") && *std::getenv("TF_NATIVE_LISTCUT") == '0'; // (debug: no cuts)
            if (off)
                return;
            resolveListStart(rdram);
            const uint32_t now = word(rdram, kPacketPointer);
            if (g_packetEnd && now > g_packetEnd && now - g_packetEnd < (16u << 20))
            {
                trackUpTo(rdram, now);
                g_packetEnd = now;
            }
        }

        void uploadLevel(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            splitListHere(rdram);
            {
                std::lock_guard<std::recursive_mutex> lock(g_texMutex);
                const Upload up{getRegU32(ctx, 4), getRegU32(ctx, 6), ++g_tableSeq};
                g_logRdram = rdram;
                g_uploads[getRegU32(ctx, 7) & 0x3FFFu] = up;
                logChange(false, getRegU32(ctx, 7) & 0x3FFFu, up);
                rememberLevel(rdram, getRegU32(ctx, 6));
                // (TODO 164 stage 2: the texture made ahead, on the pool, when its image comes - its large image asked for)
                if (prefetchMode() == 1)
                {
                    const uint32_t t = getRegU32(ctx, 4), lv = getRegU32(ctx, 6);
                    uint32_t vtable = 0u;
                    if (t && lv && (vtable = word(rdram, t)) == 0x006B4760u &&
                        word(rdram, t + 0x98u + 4u * std::min(word(rdram, t + 0x90u), 15u)) == lv)
                        prefetchOne(rdram, t);
                }
            }
            setGsMemory(runtime->memory().getGSVRAM());
            {
                // (TF_VU1_CENSUS: the upload's bytes - the level's width x height x bits)
                const uint32_t level = getRegU32(ctx, 6);
                if (level && level < PS2_RAM_SIZE)
                    censusNote(0u, (static_cast<uint64_t>(word(rdram, level + 0x0Cu)) * word(rdram, level + 0x10u) * (word(rdram, level + 0x14u) & 0x3Fu) + 7u) / 8u);
            }
            g_uploadLevel(rdram, ctx, runtime);
        }

        // (TODO 118) A recompiled function can stop part-way: when the EE scheduler's checkpoint falls inside it (a VBlank
        // due, its time slice up - at one of its calls), the host stack returns to the dispatcher with pc inside the function,
        // not at ra, and the function is entered again later where it stopped - through that place's own entry in the
        // function table, not its start. A wrapper registered at the start alone never saw such a call end: RenderGeometrySet
        // looked "culled" (its DMA call not added yet) and its end ran unseen, so the set's draw was missing from the frame.
        // At 80-240 frames a second a VBlank falls at the same point of a frame for many frames in a row: in Antarctica the
        // near snow ground was left out for whole stretches (the sky and the dark sea below showing through - the black
        // flicker), drawn again only in the frames where no VBlank fell inside that call. The wrappers below are registered at
        // every place their function can be entered again (wrapResumePoints) and keep a stopped call's arguments, by its stack
        // pointer, until it ends. TF_NATIVE_RESUMEWRAP=0: the start alone, as before.
        struct OpenCall
        {
            uint32_t entrySp = 0u, ra = 0u, a0 = 0u, a1 = 0u, a2 = 0u, packet = 0u;
        };
        // Its start (pc at the function) or where it stopped; false: entered again without a record (just run it).
        bool openCall(R5900Context *ctx, uint32_t function, uint32_t frameSize, std::vector<OpenCall> &open, OpenCall &call)
        {
            if (ctx->pc == function)
            {
                call.entrySp = getRegU32(ctx, 29);
                call.ra = getRegU32(ctx, 31);
                call.a0 = getRegU32(ctx, 4);
                call.a1 = getRegU32(ctx, 5);
                call.a2 = getRegU32(ctx, 6);
                return true;
            }
            const uint32_t entrySp = getRegU32(ctx, 29) + frameSize; // (stopped after its prologue)
            for (size_t i = open.size(); i-- > 0;)
                if (open[i].entrySp == entrySp)
                {
                    call = open[i];
                    open.erase(open.begin() + static_cast<std::ptrdiff_t>(i));
                    return true;
                }
            return false;
        }
        // (TODO 127, a test: TF_NATIVE_FORCEYIELD=<mask> - the wrapped calls made to stop part-way on purpose: a checkpoint
        // asked for at a call's start, so it stops at its first call or loop - 1 RenderGeometrySet, 2 the matrix, 4 the texture
        // group loads, 8 the microcode primitives' ends. With TF_NATIVE_RESUMEWRAP=0 it shows what such a stop did before.)
        void forceStop(R5900Context *ctx, PS2Runtime *runtime, uint32_t function, uint32_t bit)
        {
            static const uint32_t mask = std::getenv("TF_NATIVE_FORCEYIELD") ? static_cast<uint32_t>(std::strtoul(std::getenv("TF_NATIVE_FORCEYIELD"), nullptr, 0)) : 0u;
            if ((mask & bit) && ctx->pc == function)
                runtime->eeScheduler().requestCheckpoint();
        }
        // After the original: true when the call ended; else kept until it does.
        bool closeCall(R5900Context *ctx, std::vector<OpenCall> &open, const OpenCall &call)
        {
            if (ctx->pc == call.ra)
                return true;
            if (open.size() >= 16u)
                open.erase(open.begin());
            open.push_back(call);
            return false;
        }
        std::vector<OpenCall> g_openSets, g_openMatrices, g_openGroupLoads, g_openPrimitiveEnds, g_openPrimitiveStrips;
        constexpr uint32_t kRenderGeometrySetEnd = 0x00260C7Cu, kSendMatrixEnd = 0x00221E68u, kTheirFrames = 0x60u; // (both: addiu sp, sp, -0x60)
        // (TODO 127) The texture group loads (a frame of 0x30) and the microcode primitives' two ends (leaves: no frame).
        constexpr uint32_t kGroupLoadEnd = 0x0023CAD4u, kGroupLoadFrame = 0x30u, kPrimitiveEndEnd = 0x00220E08u, kPrimitiveEndStripsEnd = 0x00220468u;

        // vlTextureGroup::Load (func_0023C9F0: group, region). A texture group's uploads are recorded
        // once, when the group is placed (vlTextureGroupRegion's capture, func_0023D970: func_00267060
        // into the group's own packet - seen by uploadLevel then, at the level's load), and sent
        // whenever the group becomes current by a DMA CALL of that packet: the blocks hold the levels
        // of the group loaded last, which uploadLevel never sees. The groups of a region overlap
        // (Load unloads the ones it covers), so after the level's load the table named whichever
        // group was placed last - a draw sampling by TBP/CBP (the microcode primitives: the dropships'
        // engine glows at Alaska) read another group's texture and palette at those blocks. When Load
        // sends the CALL, the group's slot list (+0x14: the placed levels, 8 bytes each: block, format,
        // level) goes into the table at that point of the list. TF_NATIVE_GROUPLOAD=0: not.
        constexpr uint32_t kGroupLoad = 0x0023C9F0u;
        PS2Runtime::RecompiledFunction g_groupLoad = nullptr;
        uint64_t g_groupLoads = 0, g_groupLevels = 0;
        void groupLoad(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            static const bool off = std::getenv("TF_NATIVE_GROUPLOAD") && *std::getenv("TF_NATIVE_GROUPLOAD") == '0';
            // (TODO 127: a load that stopped part-way - before its CALL - is finished where it is entered again: OpenCall.)
            OpenCall call;
            if (!openCall(ctx, kGroupLoad, kGroupLoadFrame, g_openGroupLoads, call))
            {
                g_groupLoad(rdram, ctx, runtime);
                return;
            }
            if (ctx->pc == kGroupLoad)
                call.packet = word(rdram, kPacketPointer);
            forceStop(ctx, runtime, kGroupLoad, 4u);
            g_groupLoad(rdram, ctx, runtime);
            if (!closeCall(ctx, g_openGroupLoads, call))
                return;
            const uint32_t group = call.a0;
            const uint32_t before = call.packet;
            const uint32_t after = word(rdram, kPacketPointer);
            if (off || after != before + 16u || word(rdram, before) != 0x50000000u)
                return; // (no CALL sent: already loaded, or nothing captured)
            {
                // (The list cut where the CALL went: what comes before it sampled the blocks as they were.)
                static const bool noCut = std::getenv("TF_NATIVE_LISTCUT") && *std::getenv("TF_NATIVE_LISTCUT") == '0';
                resolveListStart(rdram);
                if (!noCut && g_packetEnd && before > g_packetEnd && before - g_packetEnd < (16u << 20))
                {
                    trackUpTo(rdram, before);
                    g_packetEnd = before;
                }
            }
            const uint32_t slots = word(rdram, group + 0x14u);
            if (!slots)
                return;
            const uint32_t first = word(rdram, slots + 0x08u), cursor = word(rdram, slots + 0x10u);
            if (!first || cursor < first || cursor - first > 8u * 4096u)
                return;
            // The level -> texture of the group's textures (vector: count +0x04, data +0x08); kept by
            // group while its capture and textures stay (the groups load several times a frame).
            struct Owners
            {
                uint32_t slots = 0, first = 0, cursor = 0, textures = 0, data = 0;
                std::unordered_map<uint32_t, uint32_t> owner;
            };
            static std::unordered_map<uint32_t, Owners> cache;
            const uint32_t textures = std::min(word(rdram, group + 0x04u), 4096u), data = word(rdram, group + 0x08u);
            if (cache.size() > 4096u)
                cache.clear();
            Owners &o = cache[group];
            if (o.slots != slots || o.first != first || o.cursor != cursor || o.textures != textures || o.data != data)
            {
                o = Owners{slots, first, cursor, textures, data, {}};
                for (uint32_t i = 0; data && i < textures; ++i)
                {
                    const uint32_t t = word(rdram, data + 4u * i);
                    const uint32_t levels = t ? std::min(word(rdram, t + 0x94u), 16u) : 0u;
                    for (uint32_t k = 0; k < levels; ++k)
                        if (const uint32_t level = word(rdram, t + 0x98u + 4u * k))
                            o.owner[level] = t;
                }
            }
            const auto &owner = o.owner;
            std::lock_guard<std::recursive_mutex> lock(g_texMutex);
            g_logRdram = rdram;
            ++g_groupLoads;
            for (uint32_t s = first; s < cursor; s += 8u)
            {
                const uint32_t block = word(rdram, s) & 0x3FFFu, level = word(rdram, s + 4u);
                if (!level)
                    continue;
                auto t = owner.find(level);
                Upload u{t != owner.end() ? t->second : 0u, level};
                Upload &was = g_uploads[block];
                if (was.level == u.level && was.texture == u.texture)
                    continue;
                u.seq = ++g_tableSeq;
                was = u;
                logChange(false, block, u); // (its texels were kept when the capture queued the upload: uploadLevel)
                ++g_groupLevels;
            }
        }

        // The display list's own GS drawing (the HUD, menus, 2D): screen-space draws, in order.
        gif::State g_gif;

        bool g_noImmediate = false, g_noGrass = false; // TF_NATIVE_IMMEDIATE=0, TF_NATIVE_GRASS=0
        // Type-0 batches on the sprite routine (TF_NATIVE_SPRITES0=0: off): the dispatcher converts no type 0,
        // so the routine reads them as it reads type 0x8001's - colour and position, the size in w - and
        // draws a sprite a vertex (Antarctica's falling snow: drawn as a mesh, it covered the screen).
        bool g_spritesType0 = !(std::getenv("TF_NATIVE_SPRITES0") && *std::getenv("TF_NATIVE_SPRITES0") == '0');

        // (FPS) The capture's own timers below run on the game's thread several times a draw: they read
        // the CPU's time stamp counter (a few ns) instead of steady_clock (QueryPerformanceCounter,
        // ~25 ns - 1.3% of the game's thread in Amazon); they add up counter ticks, turned into ms where
        // they are reported (the counter's rate measured against steady_clock since the start).
        struct CaptureClock
        {
            static uint64_t now() { return __rdtsc(); }
            static double msPerTick()
            {
                static const auto t0 = std::chrono::steady_clock::now();
                static const uint64_t c0 = __rdtsc();
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                const uint64_t ticks = __rdtsc() - c0;
                return ticks ? ms / static_cast<double>(ticks) : 0.0;
            }
        };
        const double g_captureClockStart = CaptureClock::msPerTick(); // (its starting point taken early)

        double g_partMs2D = 0.0; // (inside the tracking: the 2D's draws made; counter ticks until reported)
        thread_local std::vector<Draw> *t_out = nullptr; // (the renderer's thread: the frame it expands)
        thread_local bool t_logPrimitives = false;        // (debug: TF_NATIVE_RANGELOG's frame's microcode primitives logged)
        thread_local uint64_t t_looseTick = 0u;           // (N3b: the loose frame being expanded - its textures from GS memory)

        // (N3b) The indexed textures of a loose frame read from GS memory (the game uploaded them itself: no texture of
        // the render device at their blocks); one per TEX0 and frame, the one before kept when its pixels did not change
        // (its id - the GPU's copy - stays). Render thread only.
        std::shared_ptr<const Texture> looseTexture(uint64_t tex0)
        {
            struct Kept
            {
                uint64_t tick = 0u;
                std::shared_ptr<Texture> texture;
            };
            static std::unordered_map<uint64_t, Kept> kept;
            Kept &k = kept[tex0 & ~(7ull << 61)]; // (CLD aside)
            if (k.texture && k.tick == t_looseTick)
                return k.texture;
            auto made = textureFromGsMemory(tex0, 0u);
            if (!made)
                return nullptr;
            static const bool fill = std::getenv("TF_NATIVE_LOOSE_TEXFILL") && *std::getenv("TF_NATIVE_LOOSE_TEXFILL") == '1';
            if (fill)
                std::fill(made->pixels.begin(), made->pixels.end(), 0xFF00FF00u); // (debug: a known green)
            static const uint64_t dumpFrom = std::getenv("TF_NATIVE_LOOSE_GSDUMP") ? std::strtoull(std::getenv("TF_NATIVE_LOOSE_GSDUMP"), nullptr, 10) : 0u;
            static const bool noCache = std::getenv("TF_NATIVE_LOOSE_NOCACHE") && *std::getenv("TF_NATIVE_LOOSE_NOCACHE") == '1';
            const bool dumpDue = (dumpFrom && t_looseTick >= dumpFrom) || noCache; // (debug: dumped below even when unchanged)
            if (k.texture && k.texture->pixels == made->pixels && !dumpDue)
            {
                k.tick = t_looseTick;
                return k.texture;
            }
            made->id = g_nextId++;
            // (Debug: TF_NATIVE_LOOSETEX=<n>: the first n made, as runs/loose_tex_<n>_<tex0>.bmp, and their mean colour)
            static int texLog = std::getenv("TF_NATIVE_LOOSETEX") ? std::atoi(std::getenv("TF_NATIVE_LOOSETEX")) : 0;
            // (and with TF_NATIVE_LOOSE_GSDUMP=<VBlank>: the first six made for the loose frames from it)
            static uint64_t texAt = std::getenv("TF_NATIVE_LOOSE_GSDUMP") ? std::strtoull(std::getenv("TF_NATIVE_LOOSE_GSDUMP"), nullptr, 10) : 0u;
            static int texAtLeft = 6;
            const bool texAtNow = texAt && t_looseTick >= texAt && texAtLeft > 0;
            if (texAtNow)
                --texAtLeft;
            if (texLog > 0 || texAtNow)
            {
                if (texLog > 0)
                    --texLog;
                uint64_t sum[4] = {};
                for (uint32_t c : made->pixels)
                    for (int k = 0; k < 4; ++k)
                        sum[k] += (c >> (8 * k)) & 0xFFu;
                const double n = static_cast<double>(std::max<size_t>(made->pixels.size(), 1u));
                char path[96];
                std::snprintf(path, sizeof(path), "runs/loose_tex_%04llx.ppm", static_cast<unsigned long long>(tex0 & 0x3FFFu));
                if (FILE *f = std::fopen(path, "wb"))
                {
                    std::fprintf(f, "P6\n%u %u\n255\n", made->width, made->height);
                    for (uint32_t c : made->pixels)
                    {
                        const uint8_t rgb[3] = {static_cast<uint8_t>(c), static_cast<uint8_t>(c >> 8), static_cast<uint8_t>(c >> 16)};
                        std::fwrite(rgb, 1, 3, f);
                    }
                    std::fclose(f);
                }
                std::fprintf(stderr, "[native render] loose texture tex0 %016llx: %ux%u, mean R %.1f G %.1f B %.1f A %.1f\n",
                             static_cast<unsigned long long>(tex0), made->width, made->height, sum[0] / n, sum[1] / n, sum[2] / n, sum[3] / n);
            }
            buildMips(*made);
            k.texture = made;
            k.tick = t_looseTick;
            if (kept.size() > 256u)
                kept.clear();
            return made;
        }
        // (TODO 240 follow-up) The button prompts follow the controller's layout (F1 > Controls > Controller buttons,
        // patch 0156): the game prints a prompt as a glyph of its menu font DECK - a quad cut from the button's cell of
        // the atlas, whichever set is drawn (the game's, crisp PS5 / PS2, Xbox, keys: all share the cells). A glyph quad
        // of a game button whose controller button changed is cut from the cell of the controller button that now
        // presses it, shrunk around its centre where that cell's shape differs (never stretched). Live: the layout is
        // read again every quarter second. The default layout touches nothing. Only the drawn quads and the texture's
        // name are read - nothing of the game's code. TF_PROMPT_REMAP=0: off.
        namespace prompts
        {
            enum : int { Up, Down, Left, Right, Cross, Circle, Square, Triangle, L1, R1, L2, R2, L3, R3, Start, Select, kCount };
            constexpr const char *kNames[kCount] = {"up", "down", "left", "right", "cross", "circle", "square", "triangle",
                                                    "l1", "r1", "l2", "r2", "l3", "r3", "start", "select"};
            struct Cell
            {
                // texels of DECK's 256 x 256 atlas, as the game cuts its glyphs (measured with TF_PROMPT_LOG=2 for the face
                // buttons and the D-pad; the shoulder buttons and SELECT from the glyph table); x1 = 0: none
                int x0, y0, x1, y1;
            };
            constexpr Cell kDeck[kCount] = {
                {96, 197, 113, 215}, {96, 215, 113, 233}, {96, 180, 114, 197}, {96, 163, 114, 180}, // up down left right
                {5, 231, 28, 254},   {28, 231, 51, 254},  {51, 231, 74, 254},  {74, 231, 97, 254},  // cross circle square triangle
                {62, 182, 91, 201},  {62, 163, 91, 182},  {1, 163, 30, 190},   {32, 163, 60, 190},  // l1 r1 l2 r2
                {1, 192, 30, 220},   {32, 192, 60, 220},  {0, 0, 0, 0},        {118, 206, 158, 230}, // l3 r3 start select
            };
            std::atomic<bool> g_active{false};
            std::atomic<int> g_shown[kCount]; // the controller button whose picture each game button's prompt shows
            std::atomic<uint64_t> g_remapped{0};
            std::atomic<uint64_t> g_version{0}; // (counts the layout's changes: the slot labels' remapped meshes are made per version)
            std::mutex g_mutex;
            std::chrono::steady_clock::time_point g_readAt{};
            std::string g_layout;

            int index(const std::string &name)
            {
                for (int i = 0; i < kCount; ++i)
                    if (name == kNames[i])
                        return i;
                return -1;
            }

            // The layout read again at most every quarter second (and logged when it changed).
            void refresh()
            {
                static const bool off = std::getenv("TF_PROMPT_REMAP") && *std::getenv("TF_PROMPT_REMAP") == '0';
                if (off)
                    return;
                const auto now = std::chrono::steady_clock::now();
                std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
                if (!lock.owns_lock() || now - g_readAt < std::chrono::milliseconds(250))
                    return;
                g_readAt = now;
                std::string layout = ps2_host_input::padBindings();
                if (layout == g_layout)
                    return;
                g_layout = layout;
                int shown[kCount];
                for (int i = 0; i < kCount; ++i)
                    shown[i] = i;
                for (size_t at = 0; at < layout.size();)
                {
                    size_t end = layout.find(';', at);
                    if (end == std::string::npos)
                        end = layout.size();
                    const std::string item = layout.substr(at, end - at);
                    at = end + 1;
                    const size_t eq = item.find('=');
                    if (eq == std::string::npos)
                        continue;
                    const int game = index(item.substr(0, eq));
                    const size_t comma = item.find(',', eq + 1);
                    const int pad = index(item.substr(eq + 1, comma == std::string::npos ? std::string::npos : comma - eq - 1));
                    // (a controller button with no cell of its own - Start - leaves the game button's picture)
                    if (game >= 0 && pad >= 0 && kDeck[pad].x1 != 0)
                        shown[game] = pad;
                }
                bool any = false;
                std::string told;
                for (int i = 0; i < kCount; ++i)
                {
                    g_shown[i].store(shown[i], std::memory_order_relaxed);
                    if (shown[i] != i && kDeck[i].x1 != 0)
                    {
                        any = true;
                        told += std::string(told.empty() ? "" : ", ") + kNames[i] + " shows " + kNames[shown[i]];
                    }
                }
                g_active.store(any, std::memory_order_release);
                g_version.fetch_add(1u, std::memory_order_release);
                std::fprintf(stderr, "[prompts] the button pictures follow the controller layout: %s\n", any ? told.c_str() : "as the game has them");
            }

            // A draw's glyph quads (six vertices a sprite, as nr_gif makes them) remapped where they are a button's cell.
            void remap(const uint8_t *rdram, Mesh &mesh, uint64_t tex0, uint32_t bound)
            {
                refresh();
                if (!g_active.load(std::memory_order_acquire) || !bound || mesh.vertices.size() < 6u || mesh.vertices.size() % 6u != 0u ||
                    mesh.indices.size() != mesh.vertices.size())
                    return;
                // (the keyboard's key prompts show the keys bound to the game's buttons: the controller's layout is not theirs)
                if (keyPromptsShown())
                    return;
                // (DECK is 256 x 256 in GS memory: the cells' units)
                if (((tex0 >> 26) & 0xFu) != 8u || ((tex0 >> 30) & 0xFu) != 8u)
                    return;
                std::string name = guestString(rdram, bound + 0x18u);
                if (const size_t slash = name.find_last_of("/\\"); slash != std::string::npos)
                    name = name.substr(slash + 1);
                if (const size_t dot = name.find('.'); dot != std::string::npos)
                    name.resize(dot);
                for (char &c : name)
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                if (name != "DECK")
                    return;
                constexpr float kTexels = 256.0f, kTolerance = 1.5f;
                std::vector<Vertex> &v = mesh.vertices;
                for (size_t q = 0; q + 6u <= v.size(); q += 6u)
                {
                    float u0 = v[q].u, u1 = u0, v0 = v[q].v, v1 = v0, x0 = v[q].x, x1 = x0, y0 = v[q].y, y1 = y0;
                    for (size_t k = q + 1u; k < q + 6u; ++k)
                    {
                        u0 = std::min(u0, v[k].u), u1 = std::max(u1, v[k].u), v0 = std::min(v0, v[k].v), v1 = std::max(v1, v[k].v);
                        x0 = std::min(x0, v[k].x), x1 = std::max(x1, v[k].x), y0 = std::min(y0, v[k].y), y1 = std::max(y1, v[k].y);
                    }
                    // (TF_PROMPT_LOG=2: every different glyph rectangle of DECK's drawn, once - the cells as the game cuts them)
                    static const bool cells = std::getenv("TF_PROMPT_LOG") && *std::getenv("TF_PROMPT_LOG") == '2';
                    if (cells)
                    {
                        static std::mutex seenMutex;
                        static std::unordered_set<uint64_t> seen;
                        const auto r = [](float f) { return static_cast<uint64_t>(std::lround(f * 512.0f)) & 0xFFFFu; };
                        const uint64_t key = r(u0) | r(u1) << 16 | r(v0) << 32 | r(v1) << 48;
                        std::lock_guard<std::mutex> lock(seenMutex);
                        if (seen.insert(key).second && seen.size() < 400u)
                            std::fprintf(stderr, "[prompts] DECK glyph cut at %.1f,%.1f - %.1f,%.1f (texels), drawn %.1f x %.1f\n", u0 * kTexels,
                                         v0 * kTexels, u1 * kTexels, v1 * kTexels, x1 - x0, y1 - y0);
                    }
                    // (A glyph may come in pieces - the game splits its text's quads at some screen columns: each piece's rectangle
                    // lies inside its button's cell; matched by where its middle is)
                    const float mu = 0.5f * (u0 + u1) * kTexels, mv = 0.5f * (v0 + v1) * kTexels;
                    int game = -1;
                    for (int b = 0; b < kCount && game < 0; ++b)
                        if (kDeck[b].x1 != 0 && mu > kDeck[b].x0 && mu < kDeck[b].x1 && mv > kDeck[b].y0 && mv < kDeck[b].y1 &&
                            u0 * kTexels >= kDeck[b].x0 - kTolerance && u1 * kTexels <= kDeck[b].x1 + kTolerance &&
                            v0 * kTexels >= kDeck[b].y0 - kTolerance && v1 * kTexels <= kDeck[b].y1 + kTolerance)
                            game = b;
                    if (game < 0)
                        continue;
                    const int pad = g_shown[game].load(std::memory_order_relaxed);
                    if (pad == game)
                        continue;
                    const Cell &from = kDeck[game], &to = kDeck[pad];
                    const float fw = static_cast<float>(from.x1 - from.x0), fh = static_cast<float>(from.y1 - from.y0);
                    const float tw = static_cast<float>(to.x1 - to.x0), th = static_cast<float>(to.y1 - to.y0);
                    const float pu = (u1 - u0) * kTexels, pv = (v1 - v0) * kTexels, w = x1 - x0, h = y1 - y0;
                    if (pu <= 0.0f || pv <= 0.0f || w <= 0.0f || h <= 0.0f)
                        continue;
                    // The whole glyph on the screen (this piece's pixels a texel, from where the piece lies in its cell), and the
                    // new cell's glyph in its place: the same pixels a texel, shrunk to fit where its shape differs, centred.
                    const float sx = w / pu, sy = h / pv;
                    const float gx = x0 - (u0 * kTexels - from.x0) * sx, gy = y0 - (v0 * kTexels - from.y0) * sy;
                    const float gw = fw * sx, gh = fh * sy;
                    float nw = tw * sx, nh = th * sy;
                    const float fit = std::min({1.0f, gw / nw, gh / nh});
                    nw *= fit, nh *= fit;
                    const float nx = gx + 0.5f * (gw - nw), ny = gy + 0.5f * (gh - nh);
                    for (size_t k = q; k < q + 6u; ++k)
                    {
                        const float fu = (v[k].u * kTexels - from.x0) / fw, fv = (v[k].v * kTexels - from.y0) / fh; // (0..1 across the cell)
                        v[k].u = (to.x0 + fu * tw) / kTexels;
                        v[k].v = (to.y0 + fv * th) / kTexels;
                        v[k].x = nx + fu * nw;
                        v[k].y = ny + fv * nh;
                    }
                    g_remapped.fetch_add(1u, std::memory_order_relaxed);
                }
                static const bool log = std::getenv("TF_PROMPT_LOG") != nullptr;
                if (log)
                {
                    // (at most every 5 s)
                    static std::atomic<int64_t> toldAt{0};
                    const int64_t nowMs =
                        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                    int64_t last = toldAt.load(std::memory_order_relaxed);
                    if (g_remapped.load(std::memory_order_relaxed) && nowMs - last >= 5000 && toldAt.compare_exchange_strong(last, nowMs))
                        std::fprintf(stderr, "[prompts] %llu glyphs drawn with the controller's button so far\n",
                                     static_cast<unsigned long long>(g_remapped.load(std::memory_order_relaxed)));
                }
            }
        }

        // (TODO 240 follow-up) The HQ Mini-Con loadout's slot labels follow the controller layout too. They are not font
        // glyphs: each slot's side tab is part of the room's model, textured from MINICONFRAME (256 x 128), whose top row holds
        // three label cells - L2, R1, R2, each 68 x 24 texels - and whose frame's own tab carries L1, turned a quarter (texels
        // 1..25 across, 47.5..114.5 down). The slots L2 / R1 / R2 map their cell onto the middle of their tab, the slot L1 the
        // frame's L1 there. A slot whose game button another controller button now presses gets its label triangles pointed at
        // that button's label instead: a cell moved to the other cell, or turned onto the frame's L1 (and back) the way the
        // slots' own meshes map them - each tab then looks like the original tab of the button it shows, at any texture size
        // (the HD image too); one taken to the other side's slot is turned end for end, so it reads the way that side's
        // labels do. Only the drawn mesh's UVs and the texture's name are read. A layout that moves nothing onto
        // L1 / L2 / R1 / R2, or a button with no label here (the face buttons), leaves the tab as it is.
        namespace slats
        {
            enum : int { kL1, kL2, kR1, kR2, kSlots }; // (the frame's L1, then the three cells left to right)
            constexpr int kGame[kSlots] = {prompts::L1, prompts::L2, prompts::R1, prompts::R2};
            constexpr float kCellU[kSlots] = {0.0f, 9.0f, 94.0f, 179.0f}; // (a cell's left edge, texels; kL1 has none)
            constexpr float kCellV = 8.0f;                                // (the cells' top as the slots sample them)

            int slotOf(int button)
            {
                for (int s = 0; s < kSlots; ++s)
                    if (kGame[s] == button)
                        return s;
                return -1;
            }

            // A label point relative to its picture (along the cell 0..68, across it 0..24), from either kind of region.
            void toRelative(int slot, float u, float v, float &a, float &b)
            {
                if (slot == kL1)
                    a = (v - 47.5f) * (68.0f / 67.0f), b = 25.0f - u; // (the frame's L1: its rows run along the cell, turned)
                else
                    a = u - kCellU[slot], b = v - kCellV;
            }
            void fromRelative(int slot, float a, float b, float &u, float &v)
            {
                if (slot == kL1)
                    u = 25.0f - b, v = 47.5f + a * (67.0f / 68.0f);
                else
                    u = kCellU[slot] + a, v = kCellV + b;
            }

            // Which slot's label a triangle samples (texels), -1: none.
            int labelOf(const float *u, const float *v)
            {
                constexpr float t = 0.6f;
                bool frame = true;
                for (int k = 0; k < 3; ++k)
                    frame = frame && u[k] >= 1.0f - t && u[k] <= 25.0f + t && v[k] >= 47.5f - t && v[k] <= 114.5f + t;
                if (frame)
                    return kL1;
                for (int s = kL2; s < kSlots; ++s)
                {
                    bool in = true;
                    for (int k = 0; k < 3; ++k)
                        in = in && u[k] >= kCellU[s] - t && u[k] <= kCellU[s] + 68.0f + t && v[k] >= 7.0f - t && v[k] <= 32.0f + t;
                    if (in)
                        return s;
                }
                return -1;
            }

            struct Entry
            {
                std::weak_ptr<const Mesh> source;
                uint64_t version = 0;
                std::shared_ptr<const Mesh> out; // (null: nothing of it moved)
            };
            std::mutex g_mutex;
            std::unordered_map<uint64_t, Entry> g_made;   // (by the source mesh's id)
            std::unordered_map<uint32_t, bool> g_isFrame; // (a vlTexturePS2 address: whether it is MINICONFRAME)

            // The draw's mesh with its label triangles remapped, when its texture is MINICONFRAME and the layout moves a label.
            void remap(const uint8_t *rdram, Draw &d)
            {
                // (The layout as the prompts read it: every textured 2D draw reads it again now and then, and the HQ always
                // draws some - the default layout costs this one load.)
                if (!prompts::g_active.load(std::memory_order_acquire))
                    return;
                // (the game's 256 x 128 texture, or an HD image in its place - the same shape, the same source, and the UVs in
                // fractions of either)
                const Texture *t = d.texture.get();
                if (!d.mesh || !t || d.mesh->immediate || d.mesh->indices.size() % 3u != 0u ||
                    !(t->upscaled ? t->width == 2u * t->height : t->width == 256u && t->height == 128u))
                    return;
                int shown[kSlots];
                bool moved = false;
                for (int s = 0; s < kSlots; ++s)
                {
                    shown[s] = slotOf(prompts::g_shown[kGame[s]].load(std::memory_order_relaxed));
                    moved = moved || (shown[s] >= 0 && shown[s] != s);
                }
                if (!moved)
                    return;
                std::lock_guard<std::mutex> lock(g_mutex);
                {
                    if (g_isFrame.size() > 4096u)
                        g_isFrame.clear();
                    auto [it, fresh] = g_isFrame.try_emplace(t->source, false);
                    if (fresh)
                    {
                        std::string name = guestString(rdram, t->source + 0x18u);
                        if (const size_t slash = name.find_last_of("/\\"); slash != std::string::npos)
                            name = name.substr(slash + 1);
                        if (const size_t dot = name.find('.'); dot != std::string::npos)
                            name.resize(dot);
                        for (char &c : name)
                            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                        it->second = name == "MINICONFRAME";
                    }
                    if (!it->second)
                        return;
                }
                const uint64_t version = prompts::g_version.load(std::memory_order_acquire);
                if (auto it = g_made.find(d.mesh->id); it != g_made.end() && it->second.version == version && it->second.source.lock() == d.mesh)
                {
                    if (it->second.out)
                        d.mesh = it->second.out;
                    return;
                }
                if (g_made.size() > 256u)
                    for (auto it = g_made.begin(); it != g_made.end();)
                        it = it->second.source.expired() ? g_made.erase(it) : std::next(it);
                Entry &e = g_made[d.mesh->id];
                e.source = d.mesh;
                e.version = version;
                e.out.reset();
                const Mesh &m = *d.mesh;
                std::shared_ptr<Mesh> out;
                uint32_t labels = 0;
                for (size_t t = 0; t + 3u <= m.indices.size(); t += 3u)
                {
                    float u[3], v[3];
                    for (int k = 0; k < 3; ++k)
                    {
                        const Vertex &x = m.vertices[m.indices[t + k]];
                        const float q = m.projective && x.size != 0.0f ? x.size : 1.0f;
                        u[k] = x.u / q * 256.0f, v[k] = x.v / q * 128.0f;
                    }
                    const int slot = labelOf(u, v);
                    if (slot < 0 || shown[slot] < 0 || shown[slot] == slot)
                        continue;
                    if (!out)
                    {
                        out = std::make_shared<Mesh>(m);
                        out->id = g_nextId++;
                    }
                    // (each vertex of a moved triangle its own: another triangle sharing it keeps its UV)
                    for (int k = 0; k < 3; ++k)
                    {
                        Vertex x = m.vertices[m.indices[t + k]];
                        const float q = m.projective && x.size != 0.0f ? x.size : 1.0f;
                        float a, b, nu, nv;
                        toRelative(slot, u[k], v[k], a, b);
                        // (the right-hand slots' tabs mirror the left's, and each label is drawn for its own side: one taken
                        // to the other side is turned end for end - it then reads along the tab the way that side's do)
                        if ((slot <= kL2) != (shown[slot] <= kL2))
                            a = 68.0f - a;
                        fromRelative(shown[slot], a, b, nu, nv);
                        x.u = nu / 256.0f * q, x.v = nv / 128.0f * q;
                        out->indices[t + k] = static_cast<uint32_t>(out->vertices.size());
                        out->vertices.push_back(x);
                    }
                    ++labels;
                }
                static const bool log = std::getenv("TF_PROMPT_LOG") != nullptr;
                if (log)
                    std::fprintf(stderr, "[prompts] HQ slot labels: mesh %llu (set %08x, %zu triangles), %u label triangles remapped\n",
                                 static_cast<unsigned long long>(m.id), m.source, m.indices.size() / 3u, labels);
                e.out = out;
                if (out)
                    d.mesh = out;
            }
        }

        void addImmediate(const uint8_t *rdram, const gif::Primitives &p, std::vector<Vertex> *take = nullptr)
        {
            struct Add
            {
                uint64_t t;
                ~Add() { g_partMs2D += static_cast<double>(CaptureClock::now() - t); }
            } add{CaptureClock::now()};
            if (g_noImmediate)
                return;
            auto mesh = std::make_shared<Mesh>();
            mesh->id = g_nextId++;
            mesh->immediate = true;
            mesh->projective = true;
            mesh->hasColour = true;
            mesh->hasUv = true;
            mesh->prim = p.prim;
            if (take)
                mesh->vertices.swap(*take); // (the caller's own, taken: the warp tunnel's cloud is 900,000 vertices a frame)
            else
                mesh->vertices = p.vertices;
            if ((p.prim & 7u) == 0u && mesh->vertices.size() % 6u == 0u)
            {
                // Points (nr_gif: two triangles a pixel, a ab ba / ba ab b): four corners a point and their indices - the
                // warp tunnel's 150,000 a frame as six vertices each overran the frame's vertex ring, and the cloud went
                // missing from every few pictures.
                std::vector<Vertex> &v = mesh->vertices;
                const size_t points = v.size() / 6u;
                for (size_t k = 0; k < points; ++k)
                {
                    const Vertex a = v[6u * k], ab = v[6u * k + 1u], ba = v[6u * k + 2u], b = v[6u * k + 5u];
                    v[4u * k] = a, v[4u * k + 1u] = ab, v[4u * k + 2u] = ba, v[4u * k + 3u] = b;
                }
                v.resize(4u * points);
                mesh->indices.resize(6u * points);
                for (uint32_t k = 0; k < points; ++k)
                {
                    uint32_t *ix = mesh->indices.data() + 6u * k;
                    ix[0] = 4u * k, ix[1] = 4u * k + 1u, ix[2] = 4u * k + 2u, ix[3] = 4u * k + 2u, ix[4] = 4u * k + 1u, ix[5] = 4u * k + 3u;
                }
            }
            else
            {
                mesh->indices.resize(mesh->vertices.size()); // (the mesh's own: a taken vector has left p's empty)
                for (uint32_t i = 0; i < mesh->indices.size(); ++i)
                    mesh->indices[i] = i;
            }
            Draw d;
            d.mesh = mesh;
            const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            std::memcpy(d.matrix, identity, sizeof(identity));
            d.target = static_cast<uint32_t>(p.frame);
            d.fbmsk = static_cast<uint32_t>(p.frame >> 32);
            d.scanmsk = p.scanmsk;
            d.clamp = p.clamp;
            d.originX = static_cast<float>(p.xyoffset & 0xFFFFu) / 16.0f;
            d.originY = static_cast<float>((p.xyoffset >> 32) & 0xFFFFu) / 16.0f;
            // (TODO 27, the warp tunnel's shake) The tunnel's two 224-line field buffers are drawn half a line apart (XYOFFSET's
            // Y 1936 into FBP 0, 1936.5 into 0x46): on the interlaced screen each field sits on its own lines, so the picture
            // stands still. Shown whole here, each frame's field filling the picture, the content jumped half a field line
            // (three pixels at 1440 lines) up and down every frame. A loose frame's half line dropped: both fields at the
            // same place, as the screen showed them. TF_NATIVE_LOOSEFIELD=1: as before.
            static const bool looseFieldKept = std::getenv("TF_NATIVE_LOOSEFIELD") && *std::getenv("TF_NATIVE_LOOSEFIELD") == '1';
            if (t_looseTick && !looseFieldKept)
                d.originY = std::floor(d.originY);
            d.width = static_cast<float>(((p.scissor >> 16) & 0x7FFu) + 1u - (p.scissor & 0x7FFu));
            d.height = static_cast<float>(((p.scissor >> 48) & 0x7FFu) + 1u - ((p.scissor >> 32) & 0x7FFu));
            d.scissor[0] = static_cast<float>(p.scissor & 0x7FFu);
            d.scissor[1] = static_cast<float>((p.scissor >> 32) & 0x7FFu);
            d.scissor[2] = static_cast<float>((p.scissor >> 16) & 0x7FFu);
            d.scissor[3] = static_cast<float>((p.scissor >> 48) & 0x7FFu);
            d.alpha = p.alpha;
            d.test = p.test;
            d.tex0 = p.tex0;
            d.tex1 = p.tex1;
            d.zbuf = p.zbuf;
            if (p.prim & 0x10u)
            {
                // (None of the game's textures: a render target's pages - the glare, the blur, the
                // HUD's own pictures; the GPU side samples its target, or leaves the draw out.)
                std::lock_guard<std::recursive_mutex> lock(g_texMutex);
                auto it = bindingsTable().find(static_cast<uint32_t>(p.tex0) & 0x3FFFu);
                uint32_t bound = it != bindingsTable().end() ? it->second : 0u;
                // (TODO 171: a binding at TBP whose texture is no longer what GS memory holds there - another
                // texture uploaded to those pages since - is stale: the GS samples the upload. The HQ's Mini-Con
                // emblem (ICONMIN) shares its pages with the Cybertron globe (CYBER) and drew the globe's
                // texture - a yellow square - whenever the globe's binding was still recorded at its draw.)
                if (bound)
                    if (auto up = uploadsTable().find(static_cast<uint32_t>(p.tex0) & 0x3FFFu);
                        up != uploadsTable().end() && up->second.texture && up->second.texture != bound)
                        bound = 0u;
                // The depth fog: the draw reads its own depth buffer's pages as T8H (the game's shuffle
                // put depth bits 8-15 there) - drawn from the target's depth through the palette.
                static const bool noFog = std::getenv("TF_NATIVE_FOG") && *std::getenv("TF_NATIVE_FOG") == '0';
                const uint32_t tpsm = static_cast<uint32_t>(p.tex0 >> 20) & 0x3Fu;
                if (!noFog && tpsm == 0x1Bu && (static_cast<uint32_t>(p.tex0) & 0x3FFFu) == (static_cast<uint32_t>(p.zbuf) & 0x1FFu) * 32u)
                    d.fogPalette = paletteFor(rdram, p.tex0, bound);
                if (!d.fogPalette)
                {
                    d.texture = textureFor(rdram, p.tex0, bound);
                    d.topPalette = topPaletteFor(rdram, p.tex0, bound);
                    prompts::remap(rdram, *mesh, p.tex0, bound); // (TODO 240 follow-up: the prompts follow the controller layout)
                }
                // (N3b: a loose frame's indexed texture - read from GS memory, as its own packets uploaded it: whatever the
                // game's tables say is there belongs to its frames, not to a screen it draws on its own. The HQ-to-level
                // warp tunnel's streaks came out with the HQ's texture at those pages - brown - over its own blue ones. Not
                // the 32/16-bit ones: those may be the pages of a target drawn into, which the GPU side samples itself.)
                if (!d.fogPalette && t_looseTick && (tpsm == 0x14u || tpsm == 0x13u))
                    if (auto own = looseTexture(p.tex0))
                        d.texture = std::move(own);
            }
            ++g_immediate;
            (t_out ? *t_out : g_frame.draws).push_back(std::move(d));
        }

        // A microcode primitive's GS state (Draw::fromList): the registers its packet is drawn with
        // are the ones the display list set before it (the device's render-state calls write them
        // there), so they are taken from the list's reader where the draw comes in it; PRIM is the
        // routine's own (q1023, its packet's tag).
        gif::State g_callGif; // (N5.1: the registers as the device's calls wrote them - Frame::regStream)
        void applyListState(const uint8_t *rdram, Draw &d, const gif::State &gs = g_gif)
        {
            if (!d.mesh)
                return;
            uint32_t prim = d.mesh->prim;
            if (d.shader.tail[1] & 0x4000u)
                prim = (d.shader.tail[1] >> 15) & 0x7FFu;
            const int c = (prim >> 9) & 1u;
            d.target = static_cast<uint32_t>(gs.frame[c]);
            d.fbmsk = static_cast<uint32_t>(gs.frame[c] >> 32);
            d.zbuf = gs.zbuf[c];
            d.alpha = gs.alpha[c];
            d.test = gs.test[c];
            d.tex0 = gs.tex0[c];
            d.tex1 = gs.tex1[c];
            d.clamp = gs.clamp[c];
            d.scanmsk = gs.scanmsk;
            const uint64_t xy = gs.xyoffset[c], sc = gs.scissor[c];
            d.originX = static_cast<float>(xy & 0xFFFFu) / 16.0f;
            d.originY = static_cast<float>((xy >> 32) & 0xFFFFu) / 16.0f;
            d.width = static_cast<float>(((sc >> 16) & 0x7FFu) + 1u - (sc & 0x7FFu));
            d.height = static_cast<float>(((sc >> 48) & 0x7FFu) + 1u - ((sc >> 32) & 0x7FFu));
            d.scissor[0] = static_cast<float>(sc & 0x7FFu);
            d.scissor[1] = static_cast<float>((sc >> 32) & 0x7FFu);
            d.scissor[2] = static_cast<float>((sc >> 16) & 0x7FFu);
            d.scissor[3] = static_cast<float>((sc >> 48) & 0x7FFu);
            if (prim != d.mesh->prim)
            {
                auto m = std::make_shared<Mesh>(*d.mesh);
                m->id = g_nextId++;
                m->prim = prim;
                d.mesh = std::move(m);
            }
            d.texture = nullptr;
            d.topPalette = nullptr;
            if (prim & 0x10u)
            {
                std::lock_guard<std::recursive_mutex> lock(g_texMutex);
                auto it = bindingsTable().find(static_cast<uint32_t>(d.tex0) & 0x3FFFu);
                const uint32_t bound = it != bindingsTable().end() ? it->second : 0u;
                // What the GS samples: the material of a geometry set drawn with the same TEX0 (the
                // single-tree billboards: their trees' sprite textures); else the image and palette
                // last uploaded at TBP and CBP (a texture last bound there may since have been
                // replaced in GS memory - flat rectangles of another palette); else the bound one.
                if (auto set = g_tex0Textures.find(d.tex0 & kTex0SampleMask); set != g_tex0Textures.end())
                    d.texture = textureFor(rdram, d.tex0, set->second);
                if (!d.texture)
                    d.texture = textureFor(rdram, d.tex0, 0u);
                if (!d.texture)
                    d.texture = textureFor(rdram, d.tex0, bound);
                d.topPalette = topPaletteFor(rdram, d.tex0, bound);
            }
        }

        // (The capture's parts, in ms over the frames reported: the list's tracking, the 2D, meshes,
        // textures, the shaders' state.)
        double g_partMs[5] = {};
        struct PartTimer
        {
            int part;
            uint64_t start = CaptureClock::now();
            explicit PartTimer(int p) : part(p) {}
            ~PartTimer() { g_partMs[part] += static_cast<double>(CaptureClock::now() - start); }
        };

        size_t g_listWords = 0; // (reported: the frame list's words)
        bool g_logTicks = false; // (debug: TF_NATIVE_RANGELOG's frame is being captured)
        const uint8_t *g_rdram = nullptr;

        // Where the frame's display list begins. BeginFrame (slot 3) waits for the frame before's DMA
        // and then swaps packet buffers (BeginDisplayListDmaTransfer, 0x25D2B0: D_70000D30 = the other buffer, its start kept
        // at gp - 0x41B8); a recompiled function that waits returns to its caller before it is done
        // and finishes later, so the packet pointer read after the call is still the old buffer's end
        // (the frame's first stretch - its clear and the blob shadows - was read from there and lost).
        // The start is taken at the frame's first draw instead, from the variable the swap leaves.
        uint32_t g_gp = 0;               // the game's gp (beginFrame's, on entry)
        bool g_listStartPending = false; // the frame's list start not yet taken
        void resolveListStart(const uint8_t *rdram)
        {
            if (!g_listStartPending)
                return;
            g_listStartPending = false;
            const uint32_t now = word(rdram, kPacketPointer);
            uint32_t start = g_gp ? word(rdram, g_gp - 0x41B8u) : 0u;
            if (start && !(start & 0x20000000u))
                start |= now & 0x20000000u; // (the pointer is the uncached view: 0x20000000 set)
            if (start && start <= now && now - start < (16u << 20))
                g_packetEnd = start;
            else if (!g_packetEnd)
                g_packetEnd = now; // (not as expected: from here on, as before)
            if (g_logTicks)
                std::fprintf(stderr, "[native render] list start: %08x (gp %08x, pointer now %08x)\n", g_packetEnd, g_gp, now);
        }


        bool interframeScope(const uint8_t *ram) {
            return g_interframeReady && sampleFrameTexturePolicy(g_interframePolicy,ram,0x02000000u);
        }
        bool interframeOutsideCapture() { return !g_packetEnd && !g_listStartPending; }
        bool interframeSlot(const uint8_t *ram,uint32_t pool,uint32_t slot,uint32_t source,uint32_t qwords) {
            uint32_t nativePool=0,array=0,index=0,owner=0,currentSource=0,currentCount=0;
            return pool&&slot&&!(pool&3u)&&!(slot&3u)&&microcode_prefix::span(pool,12u)&&microcode_prefix::span(slot,0x18u)&&
                microcode_prefix::word(ram,0x02000000u,0x6CEF3Cu,nativePool)&&pool==nativePool&&
                microcode_prefix::word(ram,0x02000000u,pool+8u,array)&&array&&!(array&3u)&&microcode_prefix::span(array,0x90u*4u)&&
                microcode_prefix::word(ram,0x02000000u,slot+4u,index)&&index<0x90u&&
                microcode_prefix::word(ram,0x02000000u,array+4u*index,owner)&&owner==slot&&
                microcode_prefix::word(ram,0x02000000u,slot,currentSource)&&currentSource==source&&
                microcode_prefix::word(ram,0x02000000u,slot+8u,currentCount)&&currentCount==qwords;
        }
        void finishInterframeUpload(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime,const microcode_prefix::Receipt &entry) {
            if(entry.epoch!=g_interframeCapture.epoch() || !interframeScope(ram) || !interframeOutsideCapture() ||
               ctx->pc!=entry.ra || getRegU32(ctx,29)!=entry.sp || runtime->eeScheduler().currentThreadId()!=entry.thread ||
               !interframeSlot(ram,entry.pool,entry.slot,entry.source,entry.qwords))return;
            uint32_t block=0,destination=0;
            if(!microcode_prefix::word(ram,0x02000000u,entry.slot+0x14u,block)||!block||(block&3u)||
               !microcode_prefix::span(block,8u)||!microcode_prefix::word(ram,0x02000000u,block+4u,destination))return;
            std::vector<uint32_t> commands;
            if(!microcode_prefix::parse(ram,0x02000000u,entry,word(ram,kPacketPointer),destination,commands))return;
            const size_t count=commands.size();
            bool accepted=false;
            if(renderOnThread())accepted=g_interframeCapture.enqueue(entry.epoch,std::move(commands));
            else {
                microcode_prefix::Prefix immediate{entry.epoch,std::move(commands)};
                accepted=g_interframeCapture.replay(immediate,[](const std::vector<uint32_t>&actual){replayList(actual.data(),actual.size());});
            }
            if(std::getenv("TF_NATIVE_MICROCODE_TRACE"))std::fprintf(stderr,"[native render] interframe MPG %s epoch=%llu source=%08x destination=%04x words=%zu\n",
                accepted?"captured":"refused",static_cast<unsigned long long>(entry.epoch),entry.source,destination*8u,count);
        }
        void microUpload(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime) {
            microcode_prefix::Receipt receipt;
            bool capture=false;
            if(interframeScope(ram)&&interframeOutsideCapture()&&ctx->pc==0x225300u&&getRegU32(ctx,31)==0x225640u&&
               !(getRegU32(ctx,29)&15u)) {
                receipt.epoch=g_interframeCapture.epoch();receipt.thread=runtime->eeScheduler().currentThreadId();
                receipt.sp=getRegU32(ctx,29);receipt.ra=getRegU32(ctx,31);receipt.pool=getRegU32(ctx,4);receipt.slot=getRegU32(ctx,5);
                receipt.before=word(ram,kPacketPointer);
                uint32_t oldBlock=0;
                capture=receipt.sp<0x02000000u && receipt.sp>=16u && !(receipt.slot&3u) && microcode_prefix::span(receipt.slot,0x18u) &&
                    microcode_prefix::word(ram,0x02000000u,receipt.slot,receipt.source) &&
                    microcode_prefix::word(ram,0x02000000u,receipt.slot+8u,receipt.qwords) && receipt.qwords && receipt.qwords<=1024u &&
                    microcode_prefix::word(ram,0x02000000u,receipt.slot+0x14u,oldBlock) && oldBlock==0 &&
                    interframeSlot(ram,receipt.pool,receipt.slot,receipt.source,receipt.qwords);
            }
            g_interframeCapture.invoke(capture?&receipt:nullptr,
                [&](){g_microUpload(ram,ctx,runtime);},
                [&](const microcode_prefix::Receipt &entry){return ctx->pc==entry.ra;},
                [&](const microcode_prefix::Receipt &entry){finishInterframeUpload(ram,ctx,runtime,entry);});
        }
        void microReturn(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime) {
            microcode_prefix::Receipt receipt;
            if(g_interframeCapture.take(runtime->eeScheduler().currentThreadId(),getRegU32(ctx,29),ctx->pc,receipt))
                finishInterframeUpload(ram,ctx,runtime,receipt);
            g_microReturn(ram,ctx,runtime);
        }

        void trackUpTo(const uint8_t *rdram, uint32_t to)
        {
            resolveListStart(rdram);
            PartTimer timer(0);
            if (framesDeferred())
            {
                // Where the stretch is, nothing more: the renderer's thread reads it (the packet
                // buffer is double buffered: it waits for the frame's reading before the next).
                if (g_packetEnd && to > g_packetEnd)
                {
                    g_frame.ranges.push_back(Frame::Range{g_packetEnd, to, static_cast<uint32_t>(g_frame.draws.size())});
                    g_chunkPending = g_frame.vuChunks.size(); // (N5 step 2 part 2: the chunks so far fall in it)
                    std::lock_guard<std::recursive_mutex> lock(g_texMutex);
                    g_frame.ranges.back().tableChanges = g_changeCount; // (the texture tables as they stand here)
                }
                else if (g_logTicks)
                    std::fprintf(stderr, "[native render] stretch not kept: %08x..%08x before draw %zu (tick %llu)\n", g_packetEnd, to,
                                 g_frame.draws.size(), static_cast<unsigned long long>(g_curTick));
                g_rdram = rdram;
                return;
            }
            trackDisplayList(rdram, g_packetEnd, to, [&](const uint32_t *words, size_t qwords) {
                gif::run(g_gif, words, qwords, [&](const gif::Primitives &p) { addImmediate(rdram, p); });
            });
            gif::flush(g_gif, [&](const gif::Primitives &p) { addImmediate(rdram, p); });
        }

        // The capture's own time on the game's thread (the originals' left out), reported with the
        // frames drawn: what the native renderer costs the game besides the drawing.
        double g_captureMs = 0.0;
        struct CaptureTimer
        {
            uint64_t start = CaptureClock::now();
            ~CaptureTimer() { g_captureMs += static_cast<double>(CaptureClock::now() - start); }
        };

        // A draw's last decisions, once its shader state is known: its sprites made quads, its grass
        // left to be grown.
        void finishDrawImpl(Draw &d)
        {
            // The flat strips (0x3cc0, the player's shadow texture): the routine sends its block's
            // q4-q7 ahead of the strip - a GIF tag and A+D writes of SCISSOR_1 and XYOFFSET_1 - so
            // the GS draws them with the block's screen, not the register shadow's (Amazon's player
            // shadow: rows 224-287 at field B's offset, beside the 128 rows the receivers read).
            if (const Qword *b = d.shader.blockOf(Routine::FlatStrip))
            {
                uint32_t tag[4];
                std::memcpy(tag, b[4].v, 16u);
                const uint32_t nloop = tag[0] & 0x7FFFu, flg = (tag[1] >> 26) & 3u, nreg = (tag[1] >> 28) & 0xFu;
                if (flg == 0u && nreg == 1u && (tag[2] & 0xFu) == 0xEu && nloop <= 3u)
                    for (uint32_t i = 0; i < nloop; ++i)
                    {
                        uint32_t w[4];
                        std::memcpy(w, b[5u + i].v, 16u);
                        const uint64_t data = static_cast<uint64_t>(w[0]) | static_cast<uint64_t>(w[1]) << 32;
                        const uint32_t reg = w[2] & 0xFFu;
                        if (reg == 0x40u) // SCISSOR_1
                        {
                            d.scissor[0] = static_cast<float>(data & 0x7FFu);
                            d.scissor[1] = static_cast<float>((data >> 32) & 0x7FFu);
                            d.scissor[2] = static_cast<float>((data >> 16) & 0x7FFu);
                            d.scissor[3] = static_cast<float>((data >> 48) & 0x7FFu);
                            d.width = d.scissor[2] + 1.0f - d.scissor[0];
                            d.height = d.scissor[3] + 1.0f - d.scissor[1];
                        }
                        else if (reg == 0x18u) // XYOFFSET_1
                        {
                            d.originX = static_cast<float>(data & 0xFFFFu) / 16.0f;
                            d.originY = static_cast<float>((data >> 32) & 0xFFFFu) / 16.0f;
                        }
                    }
            }
            // (Debug: TF_NATIVE_OCEANLOG=<n>: the first n ocean patches after tick 9000 - routines, blocks, state.)
            static int oceanLog = std::getenv("TF_NATIVE_OCEANLOG") ? std::atoi(std::getenv("TF_NATIVE_OCEANLOG")) : 0;
            // (TF_NATIVE_WATERLOG=<n>: the same for the first n water draws - 0x1df0 then 0x2cc0.)
            static int waterLog = std::getenv("TF_NATIVE_WATERLOG") ? std::atoi(std::getenv("TF_NATIVE_WATERLOG")) : 0;
            const bool isWater = d.shader.blockOf(Routine::WaterPrepare) != nullptr;
            if (((oceanLog > 0 && d.mesh && d.mesh->ocean) || (waterLog > 0 && isWater && d.mesh)) && g_expandTick > 9000u)
            {
                --(isWater ? waterLog : oceanLog);
                std::fprintf(stderr, "[native render] (water: set %08x tick %llu, context 2 tex0 %016llx alpha %llx test %llx texture %d)\n", d.mesh->source,
                             static_cast<unsigned long long>(g_expandTick), static_cast<unsigned long long>(d.tex02),
                             static_cast<unsigned long long>(d.alpha2), static_cast<unsigned long long>(d.test2), d.texture2 != nullptr);
                std::fprintf(stderr, "[native render] ocean patch %08x: %zu vertices %zu triangles, type %u prim %x/%x, routines %u, target %08x tex0 %016llx alpha %llx test %llx zbuf %llx clamp %llx texture %d, tail %08x %08x %08x %08x\n",
                             d.mesh->source, d.mesh->vertices.size(), d.mesh->indices.size() / 3u, d.mesh->type, d.mesh->prim, d.mesh->prim2,
                             d.shader.count, d.target, static_cast<unsigned long long>(d.tex0), static_cast<unsigned long long>(d.alpha),
                             static_cast<unsigned long long>(d.test), static_cast<unsigned long long>(d.zbuf), static_cast<unsigned long long>(d.clamp),
                             d.texture != nullptr, d.shader.tail[0], d.shader.tail[1], d.shader.tail[2], d.shader.tail[3]);
                for (uint32_t k = 0; k < d.shader.count; ++k)
                {
                    std::fprintf(stderr, "[native render]   routine %04x (%d):", d.shader.address[k], static_cast<int>(d.shader.routine[k]));
                    for (uint32_t q = 0; q < (isWater ? 22u : 12u) && d.shader.first[k] + q < d.shader.data.size(); ++q)
                    {
                        const float *v = d.shader.data[d.shader.first[k] + q].v;
                        std::fprintf(stderr, " [%u] %g %g %g %g", q, v[0], v[1], v[2], v[3]);
                    }
                    std::fprintf(stderr, "\n");
                }
                const float *m = d.matrix;
                for (size_t i = 0; i < d.mesh->vertices.size(); i += std::max<size_t>(1, d.mesh->vertices.size() / 6))
                {
                    const Vertex &v = d.mesh->vertices[i];
                    float sc[4];
                    for (int r = 0; r < 4; ++r)
                        sc[r] = m[r] * v.x + m[4 + r] * v.y + m[8 + r] * v.z + m[12 + r];
                    std::fprintf(stderr, "[native render]   v%zu %g %g %g c %08x uv %g %g -> gs %g %g z %g w %g\n", i, v.x, v.y, v.z, v.colour, v.u, v.v,
                                 sc[0] / sc[3], sc[1] / sc[3], sc[2] / sc[3], sc[3]);
                }
            }
            // A microcode primitive's sprites (vlMicrocodePrimitive: the blob shadows, say): 0x2cf0 alone
            // in the table, a sprite a vertex (its w the size). Any other routine is not drawn here.
            if (d.mesh && d.mesh->primitive && !d.mesh->sprites)
            {
                if (t_logPrimitives)
                {
                    std::fprintf(stderr, "[native render] primitive: %zu vertices, routines %u (%04x %d, %04x %d), q0 %g %g %g %g, tail %08x %08x %08x %08x, target %08x tex0 %016llx alpha %llx, v0 %g %g %g size %g c %08x\n",
                                 d.mesh->vertices.size(), d.shader.count, d.shader.address[0], static_cast<int>(d.shader.routine[0]),
                                 d.shader.address[1], static_cast<int>(d.shader.routine[1]),
                                 d.shader.count ? d.shader.data[d.shader.first[0]].v[0] : 0.0f, d.shader.count ? d.shader.data[d.shader.first[0]].v[1] : 0.0f,
                                 d.shader.count ? d.shader.data[d.shader.first[0]].v[2] : 0.0f, d.shader.count ? d.shader.data[d.shader.first[0]].v[3] : 0.0f,
                                 d.shader.tail[0], d.shader.tail[1], d.shader.tail[2], d.shader.tail[3], d.target,
                                 static_cast<unsigned long long>(d.tex0), static_cast<unsigned long long>(d.alpha), d.mesh->vertices[0].x,
                                 d.mesh->vertices[0].y, d.mesh->vertices[0].z, d.mesh->vertices[0].size, d.mesh->vertices[0].colour);
                    if (d.shader.count && d.target != 0x01080000u)
                    {
                        std::fprintf(stderr, "[native render]   scissor %g..%g x %g..%g, zbuf %016llx test %016llx clamp %016llx\n", d.scissor[0], d.scissor[2],
                                     d.scissor[1], d.scissor[3], static_cast<unsigned long long>(d.zbuf), static_cast<unsigned long long>(d.test),
                                     static_cast<unsigned long long>(d.clamp));
                        const Qword *b = d.shader.data.data() + d.shader.first[0];
                        for (size_t i = 0; i < std::min<size_t>(12, d.mesh->vertices.size()); ++i)
                        {
                            const Vertex &v = d.mesh->vertices[i];
                            const float *m = d.matrix;
                            float sc[4];
                            for (int r = 0; r < 4; ++r)
                                sc[r] = m[r] * v.x + m[4 + r] * v.y + m[8 + r] * v.z + m[12 + r];
                            std::fprintf(stderr, "[native render]   v%zu %g %g %g size %g -> gs %g %g w %g (origin %g %g); q1 %g %g %g q4 %g %g %g q5 %g %g q8 %g %g\n", i, v.x, v.y, v.z,
                                         v.size, sc[0] / sc[3], sc[1] / sc[3], sc[3], d.originX, d.originY, b[1].v[0], b[1].v[1], b[1].v[2], b[4].v[0],
                                         b[4].v[1], b[4].v[2], b[5].v[0], b[5].v[1], b[8].v[0], b[8].v[1]);
                        }
                    }
                }
                static const bool primitiveParticles = !(std::getenv("TF_NATIVE_PARTICLES") && *std::getenv("TF_NATIVE_PARTICLES") == '0');
                if (d.shader.count == 1u && d.shader.routine[0] == Routine::FieldSprites &&
                    d.shader.data[d.shader.first[0]].v[0] != 0.0f) // (q0.x zero: the routine's other path, not seen)
                    d.mesh = spritesOf(d.mesh);
                else if (primitiveParticles && d.shader.count == 1u && d.shader.routine[0] == Routine::Sprites)
                {
                    // (0x22a0 / 0x2740 sprites a vertex, made as the sets' below are - a primitive's
                    // vertices are type 0x8001, the size in w: the single-tree billboards fading by
                    // distance (ALPHA 0x64, FIX), their texture the material of the sets drawn with the
                    // same TEX0 (applyListState); the glows (ALPHA 0x48). The emulated path draws them
                    // since runtime patch 0038. TF_NATIVE_PARTICLES=0: not drawn.)
                }
                else
                {
                    // (TF_NATIVE_PRIMDROPLOG=<n>: the microcode primitives left out here, by routine table, every n frames.)
                    static const int dropLog = std::getenv("TF_NATIVE_PRIMDROPLOG") ? std::atoi(std::getenv("TF_NATIVE_PRIMDROPLOG")) : 0;
                    if (dropLog > 0)
                    {
                        static std::unordered_map<std::string, uint64_t> dropped;
                        static uint64_t lastTick = 0u;
                        std::string key;
                        char part[24];
                        for (uint32_t k = 0; k < d.shader.count; ++k)
                        {
                            std::snprintf(part, sizeof(part), "%s%04x%s", k ? " " : "", d.shader.address[k], d.shader.routine[k] == Routine::Unknown ? "?" : "");
                            key += part;
                        }
                        std::snprintf(part, sizeof(part), " (%zu v)", d.mesh->vertices.size() > 64u ? size_t(64) : size_t(1));
                        ++dropped[key + part];
                        if (g_expandTick >= lastTick + static_cast<uint64_t>(dropLog))
                        {
                            lastTick = g_expandTick;
                            for (const auto &e : dropped)
                                std::fprintf(stderr, "[native render] primitives left out by tick %llu: %llu x [%s]\n", static_cast<unsigned long long>(g_expandTick),
                                             static_cast<unsigned long long>(e.second), e.first.c_str());
                            dropped.clear();
                        }
                    }
                    d.mesh = nullptr;
                    return;
                }
            }
            // Sprites (vu1_native.cpp's test): type 1 or 0x8001, the sprite routine (by its
            // bytes) alone in the table, a size in q15.xy.
            static const bool noSprites = std::getenv("TF_NATIVE_NOSPRITES") != nullptr;
            if (!noSprites && d.shader.count == 1u && d.shader.routine[0] == Routine::Sprites && d.mesh &&
                (d.mesh->type == 1u || d.mesh->type == 0x8001u || (g_spritesType0 && d.mesh->type == 0u)))
            {
                const Qword *b = d.shader.data.data() + d.shader.first[0];
                static const bool cachedSizes = std::getenv("TF_NATIVE_SPRITECACHE") != nullptr; // (debug: the old way)
                if ((d.mesh->type == 1u || d.mesh->type == 0u) && !d.mesh->immediate && !d.mesh->primitive && !cachedSizes)
                    d.mesh = currentSprites(g_rdram, d.mesh);
                static int spriteLogs = std::getenv("TF_NATIVE_SPRITELOG") ? std::atoi(std::getenv("TF_NATIVE_SPRITELOG")) : 0;
                if (spriteLogs > 0)
                {
                    --spriteLogs;
                    std::fprintf(stderr, "[native render] sprites: %zu, q10 %g %g %g %g q15 %g %g q18 %g %g q11 %g %g q14 %g %g; first pos %g %g %g size %g\n",
                                 d.mesh->vertices.size(), b[10].v[0], b[10].v[1], b[10].v[2], b[10].v[3], b[15].v[0], b[15].v[1], b[18].v[0],
                                 b[18].v[1], b[11].v[0], b[11].v[1], b[14].v[0], b[14].v[1],
                                 d.mesh->vertices.empty() ? 0.0f : d.mesh->vertices[0].x, d.mesh->vertices.empty() ? 0.0f : d.mesh->vertices[0].y,
                                 d.mesh->vertices.empty() ? 0.0f : d.mesh->vertices[0].z, d.mesh->vertices.empty() ? 0.0f : d.mesh->vertices[0].size);
                    if (!d.mesh->vertices.empty())
                    {
                        const Vertex &v = d.mesh->vertices[0];
                        const float *m = d.matrix;
                        float sc[4];
                        for (int r = 0; r < 4; ++r)
                            sc[r] = m[r] * v.x + m[4 + r] * v.y + m[8 + r] * v.z + m[12 + r];
                        std::fprintf(stderr, "[native render]   centre %g %g w %g -> %g %g, offsets %g x %g\n", sc[0], sc[1], sc[3], sc[0] / sc[3],
                                     sc[1] / sc[3], b[15].v[0] * v.size / sc[3], b[15].v[1] * v.size / sc[3]);
                    }
                    std::fprintf(stderr, "[native render]   colour %08x %08x tex0 %016llx alpha %llx test %llx texture %s prim %x\n",
                                 d.mesh->vertices.empty() ? 0u : d.mesh->vertices[0].colour, d.mesh->vertices.size() > 1 ? d.mesh->vertices[1].colour : 0u,
                                 static_cast<unsigned long long>(d.tex0), static_cast<unsigned long long>(d.alpha),
                                 static_cast<unsigned long long>(d.test), d.texture ? "yes" : "no", d.mesh->prim);
                }
                static int bigLogs = std::getenv("TF_NATIVE_BIGSPRITES") ? std::atoi(std::getenv("TF_NATIVE_BIGSPRITES")) : 0;
                if (bigLogs > 0)
                    for (const Vertex &v : d.mesh->vertices)
                    {
                        // (As the vertex shader places it: the size each side at most q10.zw.)
                        const float *m = d.matrix;
                        float sc[4];
                        for (int r = 0; r < 4; ++r)
                            sc[r] = m[r] * v.x + m[4 + r] * v.y + m[8 + r] * v.z + m[12 + r];
                        if (sc[3] <= 0.0f)
                            continue;
                        const float q = 1.0f / sc[3];
                        const float lo = std::min(b[15].v[0] * v.size * q, b[10].v[2]), hi = std::min(b[18].v[0] * v.size * q, b[10].v[2]);
                        const float loY = std::min(b[15].v[1] * v.size * q, b[10].v[3]), hiY = std::min(b[18].v[1] * v.size * q, b[10].v[3]);
                        // (Placed as the routine places it: the centre clamped to 1000-3000; kept only
                        // when the rectangle reaches the 640 x 448 screen around 2048.)
                        const float cx = std::clamp(sc[0] * q, 1000.0f, 3000.0f), cy = std::clamp(sc[1] * q, 1000.0f, 3000.0f);
                        const bool onScreen = cx + hi > 1728.0f && cx - lo < 2368.0f && cy + hiY > 1824.0f && cy - loY < 2272.0f;
                        static const uint64_t bigTick = std::getenv("TF_NATIVE_BIGSPRITES_TICK") ? std::strtoull(std::getenv("TF_NATIVE_BIGSPRITES_TICK"), nullptr, 10) : 0u;
                        if (lo + hi > 300.0f && bigLogs > 0 && onScreen && g_expandTick >= bigTick)
                        {
                            --bigLogs;
                            std::fprintf(stderr, "[native render] big sprite: clamped %g %g x %g..%g y %g..%g, mesh %llx verts %zu\n", cx, cy, cx - lo,
                                         cx + hi, cy - loY, cy + hiY, static_cast<unsigned long long>(d.mesh->source), d.mesh->vertices.size());
                            std::fprintf(stderr, "[native render] big sprite: centre %g %g w %g z/w %g, size %g -> %g + %g px (limit %g %g), colour %08x, q15 %g %g q18 %g %g, tex0 %016llx alpha %llx test %llx\n",
                                         sc[0] * q, sc[1] * q, sc[3], sc[2] * q, v.size, lo, hi, b[10].v[2], b[10].v[3], v.colour, b[15].v[0], b[15].v[1],
                                         b[18].v[0], b[18].v[1], static_cast<unsigned long long>(d.tex0), static_cast<unsigned long long>(d.alpha),
                                         static_cast<unsigned long long>(d.test));
                            // (Every sprite of the mesh as the GS draw log prints it: corners less
                            // the origin, z.)
                            if (std::getenv("TF_NATIVE_BIGSPRITES_ALL"))
                                std::fprintf(stderr, "[native render]   tick %llu origin %g %g matrix %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g\n",
                                             static_cast<unsigned long long>(g_expandTick), d.originX, d.originY, m[0], m[1], m[2], m[3], m[4], m[5], m[6],
                                             m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
                            if (std::getenv("TF_NATIVE_BIGSPRITES_ALL"))
                                for (size_t k = 0; k < d.mesh->vertices.size(); ++k)
                                {
                                    const Vertex &u = d.mesh->vertices[k];
                                    float s2[4];
                                    for (int r = 0; r < 4; ++r)
                                        s2[r] = m[r] * u.x + m[4 + r] * u.y + m[8 + r] * u.z + m[12 + r];
                                    const float q2 = s2[3] > 0.0f ? 1.0f / s2[3] : 0.0f;
                                    const float ux = std::clamp(s2[0] * q2, 1000.0f, 3000.0f), uy = std::clamp(s2[1] * q2, 1000.0f, 3000.0f);
                                    std::fprintf(stderr, "[native render]   sprite %zu: pos %g %g %g size %g -> w %g x %g..%g y %g..%g z %g colour %08x\n", k, u.x, u.y,
                                                 u.z, u.size, s2[3], ux - std::min(b[15].v[0] * u.size * q2, b[10].v[2]) - d.originX,
                                                 ux + std::min(b[18].v[0] * u.size * q2, b[10].v[2]) - d.originX,
                                                 uy - std::min(b[15].v[1] * u.size * q2, b[10].v[3]) - d.originY,
                                                 uy + std::min(b[18].v[1] * u.size * q2, b[10].v[3]) - d.originY, s2[2] * q2, u.colour);
                                }
                        }
                    }
                // (q15.xy both 0, the routine's other path - Antarctica's snow: traced on a captured
                // batch, it is the same sprite from the centre to the centre plus q18 * size * Q,
                // ST q11 and q14 as they are; the vertex shader's corners with q15 zero.)
                d.mesh = spritesOf(d.mesh);
            }
            // (TODO 162) Cybertron's city: a building a vertex, four corners each (cityBlocksOf). TF_NATIVE_CITY=0: as before
            // (the batch's three vertices one triangle - the dark shards).
            static const bool cityOff = std::getenv("TF_NATIVE_CITY") && *std::getenv("TF_NATIVE_CITY") == '0';
            if (!cityOff && d.mesh && !d.mesh->immediate && !d.mesh->sprites && d.shader.count == 1u && d.shader.routine[0] == Routine::CityBlocks)
                d.mesh = cityBlocksOf(d.mesh);
            // Grass: the ground's second draw, grown into blades (screen space).
            if (const Qword *grass = d.shader.blockOf(Routine::Grass); grass && d.mesh && !g_noGrass)
            {
                PartTimer timer(1);
                d.grassPending = true; // (grown on the renderer's thread)
            }
            // The drift (Antarctica's ground mist): streaks the same way (nr_grass.cpp driftStreaks).
            static const bool noDrift = std::getenv("TF_NATIVE_DRIFT") && *std::getenv("TF_NATIVE_DRIFT") == '0';
            if (d.shader.blockOf(Routine::Drift) && d.mesh && !g_noGrass && !noDrift)
                d.grassPending = true;
        }

        // ---- (N5 step 3, step 0 - TODO 94) TF_N5_LCCENSUS=1: what the shaders' LoadContexts and RenderGeometrySet cost ----
        // Every 10 s ("[n5:lc]"): the game thread's time a frame inside RenderGeometrySet's original (inclusive) and the whole
        // capture hook round it, inside func_0023AA90 (the dirty shaders' contexts, inclusive), and each LoadContext
        // (vtable + 0xC of the shaders in the manager's array, 0x700005F0 / count 0x70000604 - wrapped as they are met, up to
        // 64 functions): calls and time a frame, the share of the calls. rdtsc round each call (~10 ns a call).
        bool lcCensus()
        {
            static const bool on = std::getenv("TF_N5_LCCENSUS") && *std::getenv("TF_N5_LCCENSUS") == '1';
            return on;
        }
        struct LcSlot
        {
            uint32_t fn = 0u, vtable = 0u;
            PS2Runtime::RecompiledFunction original = nullptr;
            uint64_t calls = 0u, ticks = 0u;
        };
        constexpr int kLcSlots = 64;
        LcSlot g_lc[kLcSlots];
        int g_lcCount = 0;
        struct LcTotals
        {
            uint64_t rgsTicks = 0u, rgsHookTicks = 0u, rgsCalls = 0u, ctxTicks = 0u, ctxCalls = 0u, frames = 0u;
            std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
        } g_lcT;
        template <int I> void lcTimed(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint64_t t0 = CaptureClock::now();
            ++g_lc[I].calls;
            g_lc[I].original(rdram, ctx, runtime);
            g_lc[I].ticks += CaptureClock::now() - t0;
        }
        template <int... I> constexpr std::array<PS2Runtime::RecompiledFunction, sizeof...(I)> lcTable(std::integer_sequence<int, I...>)
        {
            return {lcTimed<I>...};
        }
        void lcDiscover(const uint8_t *rdram, PS2Runtime *runtime)
        {
            static const auto table = lcTable(std::make_integer_sequence<int, kLcSlots>{});
            const uint32_t count = std::min(word(rdram, 0x70000604u), 64u);
            for (uint32_t s = 0; s < count && g_lcCount < kLcSlots; ++s)
            {
                const uint32_t shader = word(rdram, 0x700005F0u + 4u * s);
                const uint32_t vtable = shader ? word(rdram, shader) : 0u;
                const uint32_t fn = vtable ? word(rdram, vtable + 0xCu) : 0u;
                if (!fn)
                    continue;
                bool known = false;
                for (int k = 0; k < g_lcCount && !known; ++k)
                    known = g_lc[k].fn == fn;
                if (known)
                    continue;
                PS2Runtime::RecompiledFunction original = runtime->lookupFunction(fn);
                if (!original)
                    continue;
                LcSlot &slot = g_lc[g_lcCount];
                slot.fn = fn, slot.vtable = vtable, slot.original = original;
                runtime->registerFunction(fn, table[static_cast<size_t>(g_lcCount)]);
                ++g_lcCount;
            }
        }
        void lcReport()
        {
            ++g_lcT.frames;
            const auto now = std::chrono::steady_clock::now();
            const double wallMs = std::chrono::duration<double, std::milli>(now - g_lcT.last).count();
            if (wallMs < 10000.0 || !g_lcT.frames)
                return;
            const double f = static_cast<double>(g_lcT.frames), k = CaptureClock::msPerTick();
            uint64_t calls = 0u, ticks = 0u;
            int order[kLcSlots];
            for (int i = 0; i < g_lcCount; ++i)
                order[i] = i, calls += g_lc[i].calls, ticks += g_lc[i].ticks;
            std::sort(order, order + g_lcCount, [](int a, int b) { return g_lc[a].calls > g_lc[b].calls; });
            std::fprintf(stderr, "[n5:lc] 10 s, %llu frames, %.2f ms a frame: RenderGeometrySet %.0f calls %.3f ms (with the capture's hook %.3f); "
                                 "0x23AA90 %.0f calls %.3f ms; LoadContexts %.0f calls %.3f ms\n",
                         static_cast<unsigned long long>(g_lcT.frames), wallMs / f, g_lcT.rgsCalls / f, g_lcT.rgsTicks * k / f, g_lcT.rgsHookTicks * k / f,
                         g_lcT.ctxCalls / f, g_lcT.ctxTicks * k / f, calls / f, ticks * k / f);
            uint64_t cum = 0u;
            for (int j = 0; j < g_lcCount; ++j)
            {
                const LcSlot &s = g_lc[order[j]];
                if (!s.calls)
                    continue;
                cum += s.calls;
                std::fprintf(stderr, "[n5:lc]   %08x (vtable %08x): %.1f calls %.1f us a frame, %.2f us a call, calls to here %.1f%%\n", s.fn, s.vtable, s.calls / f,
                             s.ticks * k * 1000.0 / f, s.ticks * k * 1000.0 / static_cast<double>(s.calls), calls ? 100.0 * cum / calls : 0.0);
            }
            for (int i = 0; i < g_lcCount; ++i)
                g_lc[i].calls = g_lc[i].ticks = 0u;
            g_lcT = LcTotals{};
            g_lcT.last = now;
        }

        void renderGeometrySet(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            // (TF_N5_LCCENSUS: the whole hook's time)
            struct HookTime
            {
                uint64_t t0 = lcCensus() ? CaptureClock::now() : 0u;
                ~HookTime()
                {
                    if (t0)
                        g_lcT.rgsHookTicks += CaptureClock::now() - t0;
                }
            } hookTime;
            // The original culls the set's box (returning before any packet when it is out of view)
            // and sends the matrix itself when the state is dirty (func_0023AA90), so the draw is
            // taken after it, and only when it added its DMA call (the packet pointer moved).
            // (TODO 118: a call that stopped part-way is finished where it is entered again - see OpenCall.)
            OpenCall call;
            if (!openCall(ctx, kRenderGeometrySet, kTheirFrames, g_openSets, call))
            {
                g_renderSet(rdram, ctx, runtime);
                return;
            }
            if (ctx->pc == kRenderGeometrySet)
            {
                resolveListStart(rdram);
                call.packet = word(rdram, kPacketPointer);
            }
            forceStop(ctx, runtime, kRenderGeometrySet, 1u);
            {
                const uint64_t t0 = lcCensus() ? CaptureClock::now() : 0u;
                g_renderSet(rdram, ctx, runtime);
                if (t0)
                    g_lcT.rgsTicks += CaptureClock::now() - t0, ++g_lcT.rgsCalls;
            }
            if (!closeCall(ctx, g_openSets, call))
                return;
            const uint32_t device = call.a0;
            const uint32_t set = call.a1;
            const uint32_t stateBlock = call.a2;
            const uint32_t before = call.packet;
            const uint32_t caller = call.ra;
            CaptureTimer timer;
            const uint32_t after = word(rdram, kPacketPointer);
            const bool drawn = set && after != before;
            {
                // (TODO 118, a check: TF_NATIVE_SETWATCH=<set, hex> - every call for that geometry set: drawn or culled,
                // the set's box (set+0x20, two vectors), the culling flag D_70000010, the clip-space matrices' dirty flag
                // D_70000E40 and the model-to-frustum matrix (SPR 0x70000E00) the box was tested with)
                static const uint32_t watch = std::getenv("TF_NATIVE_SETWATCH") ? static_cast<uint32_t>(std::strtoul(std::getenv("TF_NATIVE_SETWATCH"), nullptr, 16)) : 0u;
                if (watch && set == watch)
                {
                    auto f = [&](uint32_t a) {
                        const uint32_t w = word(rdram, a);
                        float v;
                        std::memcpy(&v, &w, 4);
                        return v;
                    };
                    std::fprintf(stderr, "[native render] setwatch tick %llu %08x %s, frame %08x, box %g %g %g .. %g %g %g, cull %08x dirty %08x, ra %08x\n",
                                 static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()), set, drawn ? "DRAWN" : "culled",
                                 word(rdram, kShadowFrame), f(set + 0x20u), f(set + 0x24u), f(set + 0x28u), f(set + 0x30u), f(set + 0x34u), f(set + 0x38u),
                                 word(rdram, 0x70000010u), word(rdram, 0x70000E40u), caller);
                    std::fprintf(stderr, "[native render]   frustum");
                    for (uint32_t k = 0; k < 16u; ++k)
                        std::fprintf(stderr, " %g", f(0x70000E00u + 4u * k));
                    std::fprintf(stderr, "\n[native render]   guard");
                    for (uint32_t k = 0; k < 16u; ++k)
                        std::fprintf(stderr, " %g", f(0x70000DC0u + 4u * k));
                    std::fprintf(stderr, "\n");
                }
            }
            if (g_logTicks)
                std::fprintf(stderr, "[native render] set %08x: packet %08x -> %08x, frame %08x, list end %08x, draw %zu\n", set, before, after,
                             word(rdram, kShadowFrame), g_packetEnd, g_frame.draws.size());
            if (g_logTicks && g_stateBlock)
            {
                std::fprintf(stderr, "[native render]   state block %08x:", g_stateBlock);
                for (uint32_t q = 0; q < 12u; ++q)
                    std::fprintf(stderr, " [%02x]%08x%08x", word(rdram, g_stateBlock + 16u * q + 8u), word(rdram, g_stateBlock + 16u * q + 4u),
                                 word(rdram, g_stateBlock + 16u * q));
                std::fprintf(stderr, " | shadow xy %08x %08x sc %08x %08x\n", word(rdram, kShadowXyOffset + 4u), word(rdram, kShadowXyOffset),
                             word(rdram, kShadowScissor + 4u), word(rdram, kShadowScissor));
            }
            // The display list up to the draw's call (its last qword): the shaders' data on the way.
            if (drawn)
                trackUpTo(rdram, after - 16u);
            else
            {
                dropPendingChunks(); // (N5 step 2 part 2: what the culled set wrote is left out - its contexts too)
                // (TODO 86) What the list holds up to the culled set's call stays in: the others wrote it (a model-to-screen
                // matrix sent after the sky dome's, a shader's context) and the next draws are made with it. Left out
                // with the set, the next draws took the stretch's last matrix from before it: at the Pacific island's
                // lava, after the sky dome, the near rock walls were drawn with the dome's - ~5.7 times their size, off
                // the screen - and the picture showed what lay behind them. TF_NATIVE_CULLEDSTRETCH=0: left out, as before.
                static const bool keepStretch = !(std::getenv("TF_NATIVE_CULLEDSTRETCH") && *std::getenv("TF_NATIVE_CULLEDSTRETCH") == '0');
                if (keepStretch)
                    trackUpTo(rdram, before);
            }
            g_packetEnd = after;
            if (drawn)
            {
                // (FPS round 2) Built in place in the frame's list (reserved at the frame's start): a Draw is
                // ~1 KB, and making it here and moving it in cost the game's thread ~1% (nothing below adds
                // draws to the list or leaves this without its draw).
                Draw &d = g_frame.draws.emplace_back();
                // The target and screen as the game set them up (func_00225D60's GS register
                // shadow: FRAME_1; XYOFFSET_1, 12.4 fixed point; SCISSOR_1, inclusive pixels).
                d.target = word(rdram, kShadowFrame);
                d.fbmsk = word(rdram, kShadowFrame + 4u);
                auto qword64 = [&](uint32_t a) { return static_cast<uint64_t>(word(rdram, a)) | static_cast<uint64_t>(word(rdram, a + 4u)) << 32; };
                if (g_stateBlock && word(rdram, g_stateBlock + 0x18u) == 0x42u && word(rdram, g_stateBlock + 0x28u) == 0x47u)
                {
                    d.alpha = qword64(g_stateBlock + 0x10u);
                    d.test = qword64(g_stateBlock + 0x20u);
                    d.tex0 = qword64(g_stateBlock + 0x30u);
                    if (word(rdram, g_stateBlock + 0x48u) == 0x08u) // (CLAMP_1, when the block sets it)
                        d.clamp = qword64(g_stateBlock + 0x40u);
                    d.alpha2 = qword64(g_stateBlock + 0x50u);
                    d.test2 = qword64(g_stateBlock + 0x60u);
                    d.tex02 = qword64(g_stateBlock + 0x70u);
                    if (word(rdram, g_stateBlock + 0x88u) == 0x09u) // (CLAMP_2, when the block sets it: the water's)
                        d.clamp2 = qword64(g_stateBlock + 0x80u);
                }
                // The registers written on their own after the block (or with no block at all, the
                // front end's scenes): as the shadow holds them.
                auto shadow = [&](uint32_t reg, uint64_t &out) {
                    if (shadowHolds(reg) && g_registerWrite[reg])
                        out = qword64(kGsShadow + 8u * reg);
                };
                shadow(0x42u, d.alpha);
                shadow(0x47u, d.test);
                shadow(0x06u, d.tex0);
                shadow(0x08u, d.clamp);
                shadow(0x43u, d.alpha2);
                shadow(0x48u, d.test2);
                shadow(0x07u, d.tex02);
                shadow(0x09u, d.clamp2);
                d.zbuf = qword64(kGsShadow + 8u * 0x4Eu);
                d.tex1 = qword64(kGsShadow + 8u * 0x14u); // (TEX1 from the shadow; on the renderer's thread, the list's)
                d.tex12 = qword64(kGsShadow + 8u * 0x15u);
                {
                    // (Debug: TF_NATIVE_MIPLOG=<n>: the state block's MIPTBP1 - where the GS reads levels 1-3 -
                    // beside TEX0 and TEX1, for the first n blocks that carry one.)
                    static int mipLog = std::getenv("TF_NATIVE_MIPLOG") ? std::atoi(std::getenv("TF_NATIVE_MIPLOG")) : 0;
                    static std::unordered_set<uint32_t> told;
                    if (mipLog > 0 && g_stateBlock && word(rdram, g_stateBlock + 0x98u) == 0x34u && told.insert(g_stateBlock).second)
                    {
                        --mipLog;
                        const uint64_t mip = qword64(g_stateBlock + 0x90u);
                        std::fprintf(stderr, "[native render] block %08x: tex0 %016llx tex1 %016llx miptbp1 %016llx (tbp1 %04llx tbw1 %llu, tbp2 %04llx tbw2 %llu, tbp3 %04llx tbw3 %llu)\n",
                                     g_stateBlock, static_cast<unsigned long long>(d.tex0), static_cast<unsigned long long>(d.tex1),
                                     static_cast<unsigned long long>(mip), static_cast<unsigned long long>(mip & 0x3FFFu),
                                     static_cast<unsigned long long>((mip >> 14) & 0x3Fu), static_cast<unsigned long long>((mip >> 20) & 0x3FFFu),
                                     static_cast<unsigned long long>((mip >> 34) & 0x3Fu), static_cast<unsigned long long>((mip >> 40) & 0x3FFFu),
                                     static_cast<unsigned long long>((mip >> 54) & 0x3Fu));
                    }
                }
                if (framesDeferred())
                {
                    d.needsShader = true;
                    d.listEnd = static_cast<uint32_t>(g_frame.list.size());
                }
                else
                {
                    PartTimer timer(4);
                    snapshotShader(d.shader);
                }
                d.originX = static_cast<float>(word(rdram, kShadowXyOffset) & 0xFFFFu) / 16.0f;
                d.originY = static_cast<float>(word(rdram, kShadowXyOffset + 4u) & 0xFFFFu) / 16.0f;
                const uint32_t sx = word(rdram, kShadowScissor), sy = word(rdram, kShadowScissor + 4u);
                d.width = static_cast<float>(((sx >> 16) & 0x7FFu) + 1u - (sx & 0x7FFu));
                d.height = static_cast<float>(((sy >> 16) & 0x7FFu) + 1u - (sy & 0x7FFu));
                d.scissor[0] = static_cast<float>(sx & 0x7FFu);
                d.scissor[1] = static_cast<float>(sy & 0x7FFu);
                d.scissor[2] = static_cast<float>((sx >> 16) & 0x7FFu);
                d.scissor[3] = static_cast<float>((sy >> 16) & 0x7FFu);
                // (An experiment, off: the GS registers as the display list's 2D packets last set
                // them. They are not the 3D's - the list's FRAME is the HUD's display buffer by then -
                // so the shadow stays the source; TF_NATIVE_LISTREGS=1 tries the list's.)
                static const bool listRegs = std::getenv("TF_NATIVE_LISTREGS") != nullptr;
                if (listRegs && g_gif.xyoffset[0] && g_gif.frame[0])
                {
                    static int differs = std::getenv("TF_NATIVE_FIELDLOG") ? std::atoi(std::getenv("TF_NATIVE_FIELDLOG")) : 0;
                    const uint64_t xy = g_gif.xyoffset[0], sc = g_gif.scissor[0];
                    if (differs > 0 && (static_cast<uint32_t>(xy) != word(rdram, kShadowXyOffset) ||
                                        static_cast<uint32_t>(xy >> 32) != word(rdram, kShadowXyOffset + 4u) ||
                                        static_cast<uint32_t>(sc >> 32) != sy))
                    {
                        --differs;
                        std::fprintf(stderr, "[native render] list vs shadow: XYOFFSET %016llx / %08x %08x, SCISSOR %016llx / %08x %08x, FRAME %016llx / %08x\n",
                                     static_cast<unsigned long long>(xy), word(rdram, kShadowXyOffset + 4u), word(rdram, kShadowXyOffset),
                                     static_cast<unsigned long long>(sc), sy, sx, static_cast<unsigned long long>(g_gif.frame[0]),
                                     word(rdram, kShadowFrame));
                    }
                    d.target = static_cast<uint32_t>(g_gif.frame[0]);
                    d.fbmsk = static_cast<uint32_t>(g_gif.frame[0] >> 32);
                    d.originX = static_cast<float>(xy & 0xFFFFu) / 16.0f;
                    d.originY = static_cast<float>((xy >> 32) & 0xFFFFu) / 16.0f;
                    d.width = static_cast<float>(((sc >> 16) & 0x7FFu) + 1u - (sc & 0x7FFu));
                    d.height = static_cast<float>(((sc >> 48) & 0x7FFu) + 1u - ((sc >> 32) & 0x7FFu));
                    d.scissor[0] = static_cast<float>(sc & 0x7FFu);
                    d.scissor[1] = static_cast<float>((sc >> 32) & 0x7FFu);
                    d.scissor[2] = static_cast<float>((sc >> 16) & 0x7FFu);
                    d.scissor[3] = static_cast<float>((sc >> 48) & 0x7FFu);
                }
                {
                    PartTimer timer(2);
                    d.mesh = meshOf(rdram, set);
                }
                {
                    static int ctxLog = std::getenv("TF_NATIVE_CTXLOG") ? std::atoi(std::getenv("TF_NATIVE_CTXLOG")) : 0;
                    static uint64_t sets = 0;
                    if (ctxLog > 0 && d.mesh && (++sets % 97u) == 0u)
                    {
                        --ctxLog;
                        std::fprintf(stderr, "[native render] ctx: prim %x prim2 %x | ctx1 frame %016llx xy %016llx sc %016llx | ctx2 frame %016llx xy %016llx sc %016llx\n",
                                     d.mesh->prim, d.mesh->prim2, static_cast<unsigned long long>(g_gif.frame[0]),
                                     static_cast<unsigned long long>(g_gif.xyoffset[0]), static_cast<unsigned long long>(g_gif.scissor[0]),
                                     static_cast<unsigned long long>(g_gif.frame[1]), static_cast<unsigned long long>(g_gif.xyoffset[1]),
                                     static_cast<unsigned long long>(g_gif.scissor[1]));
                    }
                }
                // (Sprites are drawn as quads: their mesh's vertices as the corners' centres.)
                if (g_why > 0)
                {
                    --g_why;
                    std::fprintf(stderr, "[native render] draw set %08x: state block %08x, TEX0_1 shadow %08x %08x, stage 0 %08x, requested %08x, set-texture calls %llu, list TEX0 %016llx, block TEX0 %08x %08x\n", set,
                                 stateBlock, word(rdram, kGsShadow + 8u * 6u + 4u), word(rdram, kGsShadow + 8u * 6u),
                                 word(rdram, device + kDeviceTexture), g_requested[0],
                                 static_cast<unsigned long long>(g_setTextureCalls), static_cast<unsigned long long>(g_tex0),
                                 g_stateBlock ? word(rdram, g_stateBlock + 0x34u) : 0u, g_stateBlock ? word(rdram, g_stateBlock + 0x30u) : 0u);
                }
                {
                    PartTimer timer(3);
                    d.texture = textureOf(rdram);
                    d.topPalette = topPaletteOf(rdram);
                    if (d.mesh && d.mesh->prim2)
                        d.texture2 = textureOf(rdram, 2);
                    slats::remap(rdram, d); // (TODO 240 follow-up: the HQ loadout slots' labels follow the controller layout)
                    if (d.texture && g_stateBlock && word(rdram, g_stateBlock + 0x38u) == 0x06u)
                    {
                        // (The set's TEX0 and its material, for the microcode primitives drawn with it.)
                        std::lock_guard<std::recursive_mutex> lock(g_texMutex);
                        if (auto bound = g_blockTextures.find(g_stateBlock); bound != g_blockTextures.end() && bound->second)
                        {
                            if (g_tex0Textures.size() > 8192u)
                                g_tex0Textures.clear();
                            const uint64_t blockTex0 = static_cast<uint64_t>(word(rdram, g_stateBlock + 0x30u)) |
                                                       static_cast<uint64_t>(word(rdram, g_stateBlock + 0x34u)) << 32;
                            g_tex0Textures[blockTex0 & kTex0SampleMask] = bound->second;
                        }
                    }
                    // (Debug: TF_NATIVE_SETTEXLOG=<n>: the first n sprite sets' (type 1) textures - the
                    // state block's binding beside what was last uploaded at TEX0's TBP and CBP.)
                    static int setTexLogs = std::getenv("TF_NATIVE_SETTEXLOG") ? std::atoi(std::getenv("TF_NATIVE_SETTEXLOG")) : 0;
                    static const uint64_t setTexTick = std::getenv("TF_NATIVE_SETTEXTICK") ? std::strtoull(std::getenv("TF_NATIVE_SETTEXTICK"), nullptr, 10) : 9000u;
                    if (setTexLogs > 0 && d.mesh && d.mesh->type == 1u && g_curTick >= setTexTick)
                    {
                        --setTexLogs;
                        std::lock_guard<std::recursive_mutex> lock(g_texMutex);
                        auto bound = g_blockTextures.find(g_stateBlock);
                        const uint32_t bt = bound != g_blockTextures.end() ? bound->second : 0u;
                        auto up = g_uploads.find(static_cast<uint32_t>(d.tex0) & 0x3FFFu);
                        auto cl = g_uploads.find(static_cast<uint32_t>(d.tex0 >> 37) & 0x3FFFu);
                        const uint32_t ut = up != g_uploads.end() ? up->second.texture : 0u, ct = cl != g_uploads.end() ? cl->second.texture : 0u;
                        const uint32_t blevel = bt ? word(rdram, bt + 0x98u + 4u * std::min(word(rdram, bt + 0x90u), 15u)) : 0u;
                        const uint32_t bclut = bt ? word(rdram, bt + 0x98u + 4u * std::min(word(rdram, bt + 0xE0u), 15u)) : 0u;
                        std::fprintf(stderr, "[native render] settex %llu: set %08x block %08x tex0 %016llx | bound %08x '%s' level %08x clut %08x | tbp up %08x '%s' level %08x | cbp up %08x '%s' level %08x\n",
                                     static_cast<unsigned long long>(g_curTick), set, g_stateBlock, static_cast<unsigned long long>(d.tex0), bt,
                                     bt ? guestString(rdram, bt + 0x18u).c_str() : "", blevel, bclut, ut, ut ? guestString(rdram, ut + 0x18u).c_str() : "",
                                     up != g_uploads.end() ? up->second.level : 0u, ct, ct ? guestString(rdram, ct + 0x18u).c_str() : "",
                                     cl != g_uploads.end() ? cl->second.level : 0u);
                    }
                }
                // The matrix VU1 transforms with (MSCAL 0x450's load, wherever it came from),
                // else the one func_00221DB0 sent.
                // (On the renderer's thread field A's, when its replay saw one, replaces this.)
                if (framesDeferred() || !fieldAMatrix(d.matrix))
                    std::memcpy(d.matrix, g_matrix, sizeof(g_matrix));
                d.camera = view_turn::cameraIndex();
                if (!framesDeferred())
                    finishDrawImpl(d); // (on the renderer's thread after its shader state, else here)
            }
        }

        // The microcode primitives (vlMicrocodePrimitive, vtable 0x6B3120): sprites the device builds
        // itself into the display list - a vertex at a time (slot 3, PrimitiveAddSpriteVertex, 0x220F60:
        // the colour in a1, the position and the size in f12-f15), sent when the batch ends (slot 1,
        // PrimitiveEndSpriteSet, 0x220D40). Their
        // packets are VIF unpacks and MSCNTs into the routine the table holds (the blob shadows:
        // 0x2cf0), which the 2D's reader of the list's GIF packets never sees.
        constexpr uint32_t kPrimitiveVertex = 0x00220F60u, kPrimitiveEnd = 0x00220D40u;
        PS2Runtime::RecompiledFunction g_primitiveVertex = nullptr, g_primitiveEnd = nullptr;
        std::unordered_map<uint32_t, std::shared_ptr<Mesh>> g_primitives; // the vertices so far, by primitive

        void primitiveVertex(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t self = getRegU32(ctx, 4), colour = getRegU32(ctx, 5);
            Vertex v;
            v.x = ctx->f[12];
            v.y = ctx->f[13];
            v.z = ctx->f[14];
            v.size = ctx->f[15];
            // (As the routine gets it: each channel halved, red and blue exchanged - 255 = 1.0.)
            const uint32_t h = colour >> 1;
            v.colour = ((h & 0x7F0000u) >> 16) | (h & 0x7F007F00u) | ((h & 0x7Fu) << 16);
            g_primitiveVertex(rdram, ctx, runtime);
            auto &mesh = g_primitives[self];
            if (!mesh)
            {
                mesh = std::make_shared<Mesh>();
                mesh->primitive = true;
                mesh->hasColour = true;
                mesh->type = 0x8001u;
            }
            mesh->vertices.push_back(v);
        }

        // Slot 2 (PrimitiveAddSpriteVertex's other form, 0x2210D0): the same vertex given as a position in memory (a1: x, y, z), the colour in
        // a2 and the size in f12.
        constexpr uint32_t kPrimitiveVertexAt = 0x002210D0u;
        PS2Runtime::RecompiledFunction g_primitiveVertexAt = nullptr;
        void primitiveVertexAt(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t self = getRegU32(ctx, 4), at = getRegU32(ctx, 5), colour = getRegU32(ctx, 6);
            Vertex v;
            v.x = fl(rdram, at);
            v.y = fl(rdram, at + 4u);
            v.z = fl(rdram, at + 8u);
            v.size = ctx->f[12];
            const uint32_t h = colour >> 1;
            v.colour = ((h & 0x7F0000u) >> 16) | (h & 0x7F007F00u) | ((h & 0x7Fu) << 16);
            g_primitiveVertexAt(rdram, ctx, runtime);
            auto &mesh = g_primitives[self];
            if (!mesh)
            {
                mesh = std::make_shared<Mesh>();
                mesh->primitive = true;
                mesh->hasColour = true;
                mesh->type = 0x8001u;
            }
            mesh->vertices.push_back(v);
        }

        // Its strips' end (0x220350, the strip calls' own: the header, FLUSH and MSCNT, then the
        // packet pointer past its words) - drawn as primitiveEnd draws the sprites'.
        constexpr uint32_t kPrimitiveEndStrips = 0x00220350u;
        PS2Runtime::RecompiledFunction g_primitiveEndStrips = nullptr;
        void primitiveEndImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, PS2Runtime::RecompiledFunction original, uint32_t function,
                              std::vector<OpenCall> &open);
        void primitiveEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { primitiveEndImpl(rdram, ctx, runtime, g_primitiveEnd, kPrimitiveEnd, g_openPrimitiveEnds); }
        void primitiveEndStrips(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            primitiveEndImpl(rdram, ctx, runtime, g_primitiveEndStrips, kPrimitiveEndStrips, g_openPrimitiveStrips);
        }

        void primitiveEndImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, PS2Runtime::RecompiledFunction original, uint32_t function,
                              std::vector<OpenCall> &open)
        {
            // (TODO 127: an end that stopped part-way - in its padding loop - is finished where it is entered again: OpenCall.)
            OpenCall call;
            if (!openCall(ctx, function, 0u, open, call))
            {
                original(rdram, ctx, runtime);
                return;
            }
            if (ctx->pc == function)
            {
                resolveListStart(rdram);
                call.packet = g_packetEnd; // (where the primitive's own words begin: it writes them into the list)
            }
            forceStop(ctx, runtime, function, 8u);
            original(rdram, ctx, runtime);
            if (!closeCall(ctx, open, call))
                return;
            const uint32_t self = call.a0;
            const uint32_t from = call.packet;
            auto it = g_primitives.find(self);
            std::shared_ptr<Mesh> mesh;
            if (it != g_primitives.end() && it->second && !it->second->vertices.empty())
            {
                mesh = std::move(it->second);
                g_primitives.erase(it);
            }
            else
            {
                // No sprite vertices: its strips (the warp beams' and the engines' glow cones), read
                // from the words it wrote (TF_NATIVE_STRIPS=0: not drawn).
                static const bool noStrips = std::getenv("TF_NATIVE_STRIPS") && *std::getenv("TF_NATIVE_STRIPS") == '0';
                if (noStrips)
                    return;
                const uint32_t to = word(rdram, kPacketPointer);
                thread_local std::vector<uint32_t> words;
                words.clear();
                if (!gatherList(rdram, from, to, words))
                    return;
                auto strips = std::make_shared<Mesh>();
                // (TF_NATIVE_STRIPLOG=<count>[,<from tick>]: the first strip primitives' words decoded, from that VBlank on.)
                static int stripLog = std::getenv("TF_NATIVE_STRIPLOG") ? std::atoi(std::getenv("TF_NATIVE_STRIPLOG")) : 0;
                static const uint64_t stripLogFrom = [] {
                    const char *e = std::getenv("TF_NATIVE_STRIPLOG");
                    const char *comma = e ? std::strchr(e, ',') : nullptr;
                    return comma ? std::strtoull(comma + 1, nullptr, 10) : 9001ull;
                }();
                FILE *dump = nullptr;
                if (stripLog > 0 && g_curTick >= stripLogFrom)
                {
                    --stripLog;
                    dump = stderr;
                    std::fprintf(stderr, "[native render] strip primitive %08x: list %08x..%08x, %zu words\n", self, from, to, words.size());
                }
                decodePrimitiveStrips(words.data(), words.size(), *strips, dump);
                if (strips->indices.empty())
                    return;
                strips->hasUv = true;
                mesh = std::move(strips);
            }
            CaptureTimer timer;
            mesh->id = g_nextId++;
            // The list up to the primitive's end: its own packet goes through the VU image (the
            // routine's data, the table), as the GS's draws of it come after.
            const uint32_t after = word(rdram, kPacketPointer);
            trackUpTo(rdram, after);
            g_packetEnd = after;
            Draw d;
            d.mesh = mesh;
            d.fromList = true;
            if (framesDeferred() || !fieldAMatrix(d.matrix))
                std::memcpy(d.matrix, g_matrix, sizeof(g_matrix));
            d.camera = view_turn::cameraIndex();
            if (framesDeferred())
            {
                d.needsShader = true;
                d.listEnd = static_cast<uint32_t>(g_frame.list.size());
            }
            else
            {
                snapshotShader(d.shader);
                applyListState(rdram, d);
                finishDrawImpl(d);
                if (!d.mesh)
                    return;
            }
            g_frame.draws.push_back(std::move(d));
        }

        // The FFT ocean (vlxWaterEffect::Render, Pacific Island and Mid-Atlantic): never a geometry set
        // - each grid patch's packet (a vlDynamicGrid's, SendVertexDataToVU1FromGrid's: the strips' GIF
        // tags to 0x3FE/0x3FF, then per strip of up to 12 quads a header of the count | 0x8000, the two
        // rows' UVs, colours and positions REF'd from the simulation's arrays - live, the heights and the
        // per-vertex sky colours of this frame - the masks 0xC000 and MSCNT into the routine the ocean's
        // shader block made current) is CALLed into the list by 0x26F790 (a0: the packet), after its
        // matrix (SetModelToClip_LocalTranslationAndScaleZ) and its routine table. Read here from its
        // words, drawn as a microcode primitive's strips are (its GS state the list's).
        // TF_NATIVE_OCEAN=0: not drawn.
        constexpr uint32_t kSendGridPacket = 0x0026F790u;
        PS2Runtime::RecompiledFunction g_sendGridPacket = nullptr;
        void sendGridPacket(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t packet = getRegU32(ctx, 4);
            resolveListStart(rdram);
            g_sendGridPacket(rdram, ctx, runtime);
            static const bool off = std::getenv("TF_NATIVE_OCEAN") && *std::getenv("TF_NATIVE_OCEAN") == '0';
            if (off || !packet || !g_enabled)
                return;
            CaptureTimer timer;
            thread_local std::vector<uint32_t> words;
            words.clear();
            static int gridLog = std::getenv("TF_NATIVE_GRIDLOG") ? std::atoi(std::getenv("TF_NATIVE_GRIDLOG")) : 0;
            FILE *dump = nullptr;
            if (gridLog > 0 && g_curTick > 9000u)
            {
                --gridLog;
                dump = stderr;
                std::fprintf(stderr, "[native render] ocean grid packet %08x (tick %llu)\n", packet, static_cast<unsigned long long>(g_curTick));
            }
            const bool gathered = vif::gather(rdram, packet, words, dump);
            auto mesh = std::make_shared<Mesh>();
            if (gathered)
                decodePrimitiveStrips(words.data(), words.size(), *mesh, dump);
            static uint64_t gridCalls = 0;
            if (++gridCalls == 1u || gridCalls == 2000u)
                std::fprintf(stderr, "[native render] ocean: grid packet %08x (call %llu, tick %llu): %s, %zu words, %zu vertices, %zu triangles\n", packet,
                             static_cast<unsigned long long>(gridCalls), static_cast<unsigned long long>(g_curTick), gathered ? "read" : "NOT READ", words.size(), mesh->vertices.size(),
                             mesh->indices.size() / 3u);
            if (!gathered)
                return;
            if (mesh->indices.empty())
                return;
            mesh->hasUv = true;
            mesh->ocean = true;
            mesh->source = packet;
            mesh->id = g_nextId++;
            const uint32_t after = word(rdram, kPacketPointer);
            trackUpTo(rdram, after);
            g_packetEnd = after;
            Draw d;
            d.mesh = std::move(mesh);
            d.fromList = true;
            if (framesDeferred() || !fieldAMatrix(d.matrix))
                std::memcpy(d.matrix, g_matrix, sizeof(g_matrix));
            d.camera = view_turn::cameraIndex();
            if (framesDeferred())
            {
                d.needsShader = true;
                d.listEnd = static_cast<uint32_t>(g_frame.list.size());
            }
            else
            {
                snapshotShader(d.shader);
                applyListState(rdram, d);
                finishDrawImpl(d);
                if (!d.mesh)
                    return;
            }
            g_frame.draws.push_back(std::move(d));
        }

        // ---- N5 step 2 part 2 (TODO 94): the shaders' contexts out of the display list ----
        // Each frame the game writes ~440 KB of VU1 data into the list: the dirty shaders' contexts (func_0023AA90 calls each
        // dirty shader's LoadContext, vtable + 0xC, which allocate through func_00221CC0 - a CNT tag, STCYCL, UNPACK V4-32 -
        // and fill it) and the model-to-screen matrix (func_00221DB0, a 0x70-byte CNT packet). While VIF1 is lite nothing
        // but the native renderer reads it (its replay of the list into its VU image): the kick only copied it and walked
        // its tags. Here, once such a call has ended (synchronously - not stopped part-way), its bytes are checked (CNT tags
        // only, no interrupt bit, only codes VIF1 does nothing with while lite: no MARK / DIRECT / MPG / MSKPATH3), kept in
        // the frame as the VIF words the gather would have read (Frame::vuChunks) and the packet pointer put back where the
        // call started: the list never holds them, the renderer splices them back where they stood. The original code still
        // runs (the game's timing, its EE clock and a recording's replay unchanged). TF_N5_CONTEXTS: 0 off (the default: no
        // gain measured - the kick's copy of these ~0.45 MB and ~3,500 tags a frame was not what it cost; test PC, Deep
        // Amazon, game-thread bound, on 115-121 fps against off 118-123), 1 on, census (counted, the list left alone),
        // check (kept in the list too; the renderer checks that its words with
        // them taken out and spliced back are the same, that no tag points into them, and that VIF1's CYCLE at the list's
        // other UNPACKs is the same without them).
        void copyRainBlock(const uint8_t *rdram);
        int contextsMode() // 0 off, 1 on, 2 check, 3 census
        {
            static const int selected = [] {
                const char *e = std::getenv("TF_N5_CONTEXTS");
                if (e && *e == '1')
                    return 1;
                if (e && std::strcmp(e, "check") == 0)
                    return 2;
                if (e && std::strcmp(e, "census") == 0)
                    return 3;
                return 0;
            }();
            return selected;
        }
        bool g_divertFrame = false; // (decided at the frame's start: lite, frames deferred, the mode)
        int g_divertDepth = 0;      // (a call inside a diverted one - the matrix inside a LoadContext - goes with it)
        enum DivertRefusal
        {
            kRefYield,   // stopped part-way
            kRefTouched, // a list stretch, a native register or the list's start taken while it ran
            kRefSpan,    // the pointer not forward / too far
            kRefTags,    // a tag other than CNT, or running over the end
            kRefIrq,     // a tag's interrupt bit
            kRefCodes,   // a VIF code VIF1 acts on while lite, or the codes not ending with the span
            kRefCount
        };
        constexpr const char *kRefusalNames[] = {"stopped part-way", "list touched", "span", "tags", "interrupt bit", "VIF codes"};
        struct DivertStats
        {
            uint64_t calls[2] = {}, empty = 0, diverted[2] = {}, bytes = 0, tags = 0, refused[kRefCount] = {}, refusedBytes = 0;
            uint64_t frames = 0;
            std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
        } g_dv;
        struct DivertOpen
        {
            uint32_t before = 0u, ra = 0u, packetEnd = 0u;
            size_t ranges = 0u, natives = 0u, regs = 0u;
            bool listStartPending = false;
        };
        DivertOpen divertOpen(const uint8_t *rdram, R5900Context *ctx)
        {
            DivertOpen o;
            o.before = word(rdram, kPacketPointer);
            o.ra = getRegU32(ctx, 31);
            o.packetEnd = g_packetEnd;
            o.ranges = g_frame.ranges.size();
            o.natives = g_frame.nativeRegs.size();
            o.regs = g_frame.regStream.size();
            o.listStartPending = g_listStartPending;
            return o;
        }
        // The words a VIF code takes (itself and its data) with CYCLE as given; 0: one VIF1 acts on while lite (or unknown).
        size_t vifLength(const uint32_t *w, size_t left, uint32_t cycle)
        {
            const uint32_t code = w[0], cmd = (code >> 24) & 0x7Fu, num = (code >> 16) & 0xFFu;
            if (code >> 31)
                return 0u; // (the interrupt bit)
            if ((cmd & 0x60u) == 0x60u)
            {
                const uint32_t vn = (cmd >> 2) & 3u, vl = cmd & 3u, n = num ? num : 256u;
                const uint32_t cl = cycle & 0xFFu, wl = (cycle >> 8) & 0xFFu;
                uint32_t elements = n;
                if (wl > cl && wl) // (filling: CL of every WL written come from the data)
                    elements = cl * (n / wl) + std::min(n % wl, cl);
                const uint32_t bits = (vn == 3u && vl == 3u) ? 16u : (32u >> vl) * (vn + 1u);
                return 1u + (static_cast<size_t>(elements) * bits + 31u) / 32u;
            }
            switch (cmd)
            {
            case 0x00u: case 0x01u: case 0x02u: case 0x03u: case 0x04u: case 0x05u: // NOP STCYCL OFFSET BASE ITOP STMOD
            case 0x10u: case 0x11u: case 0x13u: case 0x14u: case 0x15u: case 0x17u: // FLUSHE FLUSH FLUSHA MSCAL MSCALF MSCNT
                return 1u;
            case 0x20u: return left >= 2u ? 2u : 0u;             // STMASK
            case 0x30u: case 0x31u: return left >= 5u ? 5u : 0u; // STROW STCOL
            default: return 0u;                                  // (MSKPATH3, MARK, MPG, DIRECT: kept in the list)
            }
        }
        // The call's span [before, after) as the gather would read it, appended to the frame's chunks; false (nothing kept):
        // not divertible.
        bool divertTake(const uint8_t *rdram, uint32_t before, uint32_t after, DivertRefusal &why, uint32_t &tags)
        {
            std::vector<uint32_t> &out = g_frame.vuChunks;
            const size_t at = out.size();
            tags = 0u;
            auto fail = [&](DivertRefusal r) {
                out.resize(at);
                why = r;
                return false;
            };
            const uint8_t *base = getConstMemPtr(rdram, before);
            const uint8_t *last = getConstMemPtr(rdram, after - 4u);
            if (!base || !last || last != base + (after - before - 4u))
                return fail(kRefSpan); // (not one stretch of memory)
            out.push_back(static_cast<uint32_t>(g_frame.draws.size()));
            out.push_back(static_cast<uint32_t>(g_frame.ranges.size())); // (the list stretch it falls in: the next one kept)
            out.push_back(before & 0x1FFFFFFFu);
            out.push_back(after & 0x1FFFFFFFu);
            out.push_back(0u);
            const uint32_t *w = reinterpret_cast<const uint32_t *>(base);
            const uint32_t total = (after - before) / 4u;
            // (the words: each tag's upper half then its data - at most the span's words less two a tag; one copy a tag)
            out.resize(at + 5u + total);
            uint32_t *o = out.data() + at + 5u;
            size_t count = 0u;
            bool plain = true; // (every tag STCYCL (WL <= CL), then one UNPACK V4-32 of exactly its data: no walk needed)
            for (uint32_t i = 0u; i < total;)
            {
                const uint32_t lo = w[i], qwc = lo & 0xFFFFu, id = (lo >> 28) & 7u;
                if (lo >> 31)
                    return fail(kRefIrq);
                if (id != 1u || i + 4u + 4u * qwc > total)
                    return fail(kRefTags);
                const uint32_t c0 = w[i + 2u], c1 = w[i + 3u];
                const uint32_t n1 = (c1 >> 16) & 0xFFu;
                plain = plain && (c0 >> 24) == 0x01u && ((c0 >> 8) & 0xFFu) <= (c0 & 0xFFu) && ((c1 >> 24) & 0xEFu) == 0x6Cu &&
                        (n1 ? n1 : 256u) == qwc;
                std::memcpy(o + count, w + i + 2u, 8u + 16u * qwc);
                count += 2u + 4u * qwc;
                i += 4u + 4u * qwc;
                ++tags;
            }
            out.resize(at + 5u + count);
            // (its codes: each one VIF1 passes by while lite, ending with the span - CYCLE from its own STCYCLs; an UNPACK
            // ahead of the span's first STCYCL is refused, its length unknown here)
            uint32_t cycle = ~0u;
            const uint32_t *v = out.data() + at + 5u;
            for (size_t i = 0u; i < count && !plain;)
            {
                const uint32_t cmd = (v[i] >> 24) & 0x7Fu;
                if ((cmd & 0x60u) == 0x60u && cycle == ~0u)
                    return fail(kRefCodes);
                const size_t n = vifLength(v + i, count - i, cycle);
                if (!n || i + n > count)
                    return fail(kRefCodes);
                if (cmd == 0x01u)
                    cycle = v[i] & 0xFFFFu;
                i += n;
            }
            out[at + 4u] = static_cast<uint32_t>(count);
            return true;
        }
        // After the call: its span taken out of the list (or, refused, left in it).
        void divertClose(uint8_t *rdram, R5900Context *ctx, const DivertOpen &o, int kind)
        {
            ++g_dv.calls[kind];
            const uint32_t after = word(rdram, kPacketPointer);
            if (after == o.before)
            {
                ++g_dv.empty;
                return;
            }
            DivertRefusal why = kRefCount;
            uint32_t tags = 0u;
            if (ctx->pc != o.ra)
                why = kRefYield;
            else if (g_packetEnd != o.packetEnd || g_frame.ranges.size() != o.ranges || g_frame.nativeRegs.size() != o.natives ||
                     g_frame.regStream.size() != o.regs || g_listStartPending != o.listStartPending)
                why = kRefTouched;
            else if (after < o.before || after - o.before > (256u << 10) || ((after - o.before) & 15u))
                why = kRefSpan;
            else
            {
                copyRainBlock(rdram); // (the rain's block, before its bytes can be written over)
                const size_t mark = g_frame.vuChunks.size();
                if (divertTake(rdram, o.before, after, why, tags))
                {
                    ++g_dv.diverted[kind];
                    g_dv.bytes += after - o.before;
                    g_dv.tags += tags;
                    if (contextsMode() == 3)
                        g_frame.vuChunks.resize(mark); // (census: counted only)
                    else if (contextsMode() == 1)
                        store32(rdram, kPacketPointer, o.before);
                    return;
                }
            }
            ++g_dv.refused[why];
            if (after > o.before)
                g_dv.refusedBytes += after - o.before;
        }
        void divertReport()
        {
            ++g_dv.frames;
            const auto now = std::chrono::steady_clock::now();
            if (now - g_dv.last < std::chrono::seconds(10))
                return;
            g_dv.last = now;
            static int told = 0;
            if (contextsMode() == 1 && told >= 3 && !std::getenv("TF_N5_CONTEXTS_LOG"))
                return;
            ++told;
            const double f = static_cast<double>(g_dv.frames);
            std::string refused;
            for (int i = 0; i < kRefCount; ++i)
                if (g_dv.refused[i])
                {
                    char e[64];
                    std::snprintf(e, sizeof(e), " %s %.1f", kRefusalNames[i], static_cast<double>(g_dv.refused[i]) / f);
                    refused += e;
                }
            std::fprintf(stderr, "[n5:contexts] %s, a frame (%llu frames): calls 0x23AA90 %.1f / 0x221DB0 %.1f (%.1f wrote nothing); "
                                 "taken out %.1f / %.1f - %.1f KB, %.1f tags; left in %.1f KB, refused:%s\n",
                         contextsMode() == 1 ? "on" : contextsMode() == 2 ? "check" : "census", static_cast<unsigned long long>(g_dv.frames),
                         g_dv.calls[0] / f, g_dv.calls[1] / f, g_dv.empty / f, g_dv.diverted[0] / f, g_dv.diverted[1] / f,
                         g_dv.bytes / f / 1024.0, g_dv.tags / f, g_dv.refusedBytes / f / 1024.0, refused.empty() ? " none" : refused.c_str());
            const auto last = g_dv.last;
            g_dv = DivertStats{};
            g_dv.last = last;
        }
        // (the renderer's thread) The splice's counts: =check's comparisons, and the chunks replayed alone.
        struct ContextsCheck
        {
            uint64_t ranges = 0, chunks = 0, mismatch = 0, refInto = 0, cycleDiffers = 0, alone = 0, lost = 0;
            int told = 0, toldLost = 0;
            std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
        };
        ContextsCheck &contextsCheck()
        {
            static ContextsCheck c;
            return c;
        }
        void contextsCheckReport()
        {
            ContextsCheck &c = contextsCheck();
            const auto now = std::chrono::steady_clock::now();
            if (now - c.last < std::chrono::seconds(10) || (contextsMode() != 2 && !std::getenv("TF_N5_CONTEXTS_LOG")))
                return;
            c.last = now;
            std::fprintf(stderr, "[n5:contexts] renderer, 10 s: %llu stretches with %llu chunks checked - %llu differ, %llu tags point "
                                 "into a chunk, %llu UNPACKs whose CYCLE would differ; %llu chunks replayed alone, %llu not in their stretch\n",
                         static_cast<unsigned long long>(c.ranges), static_cast<unsigned long long>(c.chunks), static_cast<unsigned long long>(c.mismatch),
                         static_cast<unsigned long long>(c.refInto), static_cast<unsigned long long>(c.cycleDiffers), static_cast<unsigned long long>(c.alone), static_cast<unsigned long long>(c.lost));
            c.ranges = c.chunks = c.mismatch = c.refInto = c.cycleDiffers = c.alone = c.lost = 0;
        }
        // Any VIF code's words (itself and its data), CYCLE as given.
        size_t vifLengthAny(const uint32_t *w, uint32_t cycle)
        {
            const uint32_t code = w[0] & 0x7FFFFFFFu, cmd = (code >> 24) & 0x7Fu, num = (code >> 16) & 0xFFu, imm = code & 0xFFFFu;
            if ((cmd & 0x60u) == 0x60u)
                return vifLength(&code, 1u, cycle);
            switch (cmd)
            {
            case 0x20u: return 2u;
            case 0x30u: case 0x31u: return 5u;
            case 0x4Au: return 1u + 2u * (num ? num : 256u);
            case 0x50u: case 0x51u: return 1u + 4u * (imm ? imm : 65536u);
            default: return 1u;
            }
        }

        // func_00221DB0 writes a DMA tag, a VIF UNPACK and the 4x4 model-to-screen matrix at word 8
        // of its packet (then field B's): the matrix the next draws are transformed with.
        void sendMatrix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            // (TODO 118: as RenderGeometrySet's - a call that stopped part-way read its matrix before writing it)
            OpenCall call;
            if (!openCall(ctx, kSendMatrix, kTheirFrames, g_openMatrices, call))
            {
                g_sendMatrix(rdram, ctx, runtime);
                return;
            }
            // (N5 step 2 part 2: taken out of the list - TF_N5_CONTEXTS - when it ends in this call, alone)
            const bool divert = g_divertFrame && g_divertDepth == 0 && ctx->pc == kSendMatrix;
            DivertOpen open;
            if (divert)
                open = divertOpen(rdram, ctx);
            if (ctx->pc == kSendMatrix)
                call.packet = word(rdram, kPacketPointer);
            forceStop(ctx, runtime, kSendMatrix, 2u);
            g_sendMatrix(rdram, ctx, runtime);
            if (!closeCall(ctx, g_openMatrices, call))
            {
                if (divert)
                    divertClose(rdram, ctx, open, 1); // (stopped part-way: counted, left in the list)
                return;
            }
            const uint32_t packet = call.packet;
            for (int i = 0; i < 16; ++i)
                g_matrix[i] = fl(rdram, packet + 32u + 4u * static_cast<uint32_t>(i));
            view_turn::noteMatrixSent(rdram, g_matrix); // (N9: the camera the next draws are made with)
            if (divert)
                divertClose(rdram, ctx, open, 1);
        }

        void setDestructor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_meshes.erase(getRegU32(ctx, 4));
            g_setDestructor(rdram, ctx, runtime);
        }

        // The game deletes a texture (a level's at its end, a menu's when it closes): its conversions
        // are let go of with it - their GPU copies follow (nr_d3d.cpp prune()). This, not idle time,
        // is what empties the cache between levels (2026-10-28: evicting by idle time made the HQ
        // flash and textures come back white or missing - items 55/61). Before the original runs:
        // a recompiled function can stop part way (pc != ra) and be entered again.
        void textureDestructor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kTextureDestructor)
            {
                const uint32_t texture = getRegU32(ctx, 4);
                movie::textureGone(texture); // (a movie's texture: its pictures are no longer drawn from the decoder - N6)
                std::lock_guard<std::recursive_mutex> lock(g_texMutex);
                ++g_destroyedSince; // (prefetchByScan's cue)
                for (auto it = g_textures.begin(); it != g_textures.end();)
                    if (it->second.key.texture == texture)
                        it = g_textures.erase(it);
                    else
                        ++it;
            }
            g_textureDestructor(rdram, ctx, runtime);
        }

        // TF_NATIVE_COUNT=1: how often the device's other drawing functions run (calls a frame).
        struct Counted
        {
            uint32_t address;
            const char *name;
            PS2Runtime::RecompiledFunction original;
            uint64_t calls;
        };
        Counted g_counted[] = {
            {0x00257610u, "slot 26 line", nullptr, 0},
            {0x00257160u, "slot 27", nullptr, 0},
            {0x002578F0u, "slot 28 rectangle", nullptr, 0},
            {0x00256280u, "slot 29 primitive", nullptr, 0},
            {0x00220D40u, "PrimitiveEndSpriteSet", nullptr, 0},
            {0x002210D0u, "PrimitiveAddSpriteVertex (from memory)", nullptr, 0},
            {0x00220F60u, "PrimitiveAddSpriteVertex", nullptr, 0},
            {0x00220D30u, "primitive 5", nullptr, 0},
            {0x00220350u, "primitive 6 end strips", nullptr, 0},
            {0x00220A10u, "primitive 7", nullptr, 0},
            {0x002206D0u, "primitive 8 strip vertex", nullptr, 0},
            {0x002202E0u, "primitive 9", nullptr, 0},
        };
        template <int I> void counted(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ++g_counted[I].calls;
            g_counted[I].original(rdram, ctx, runtime);
        }
        constexpr PS2Runtime::RecompiledFunction kCounters[] = {counted<0>, counted<1>, counted<2>, counted<3>, counted<4>, counted<5>,
                                                               counted<6>, counted<7>, counted<8>, counted<9>, counted<10>, counted<11>};

        // (Debug: TF_NATIVE_SLOTLOG=1 with TF_NATIVE_RANGELOG: every call of the render device's other
        // slots in the logged frame, with the packet pointer around it.)
        constexpr int kLoggedSlots = 160;
        uint32_t g_slotAddress[kLoggedSlots] = {};
        PS2Runtime::RecompiledFunction g_slotOriginal[kLoggedSlots] = {};
        template <int I> void slotLogged(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t before = word(rdram, kPacketPointer), a1 = getRegU32(ctx, 5);
            g_slotOriginal[I](rdram, ctx, runtime);
            if (g_logTicks)
                std::fprintf(stderr, "[native render] slot %d (%08x, a1 %08x): packet %08x -> %08x, frame %08x\n", I, g_slotAddress[I], a1, before,
                             word(rdram, kPacketPointer), word(rdram, kShadowFrame));
        }
        template <int... I> constexpr std::array<PS2Runtime::RecompiledFunction, sizeof...(I)> slotTable(std::integer_sequence<int, I...>)
        {
            return {slotLogged<I>...};
        }

        // TF_NATIVE_DUMP=1: each draw of a saved frame - its set, its matrix, a few vertices and where
        // its vertices land on the GS screen (the transform done here on the CPU, as VU1 does it).
        void dumpFrame(const std::string &path)
        {
            FILE *f = std::fopen(path.c_str(), "w");
            if (!f)
                return;
            std::fprintf(f, "frame synchronous_game_textures %d\n", g_frame.synchronousGameTextures ? 1 : 0);
            int index = 0;
            for (const Draw &d : g_frame.draws)
            {
                const Mesh &m = *d.mesh;
                const float *r = d.matrix;
                float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
                size_t behind = 0;
                for (const Vertex &v : m.vertices)
                {
                    const float s[4] = {r[0] * v.x + r[4] * v.y + r[8] * v.z + r[12], r[1] * v.x + r[5] * v.y + r[9] * v.z + r[13],
                                        r[2] * v.x + r[6] * v.y + r[10] * v.z + r[14], r[3] * v.x + r[7] * v.y + r[11] * v.z + r[15]};
                    if (s[3] <= 0.0f)
                    {
                        ++behind;
                        continue;
                    }
                    const float gx = s[0] / s[3], gy = s[1] / s[3], gz = s[2] / s[3];
                    x0 = std::min(x0, gx), x1 = std::max(x1, gx), y0 = std::min(y0, gy), y1 = std::max(y1, gy);
                    z0 = std::min(z0, gz), z1 = std::max(z1, gz);
                }
                std::fprintf(f, "%4d set %08x v %zu t %zu behind %zu  x %.0f..%.0f y %.0f..%.0f z %.3g..%.3g\n", index++, m.source,
                             m.vertices.size(), m.indices.size() / 3, behind, x0, x1, y0, y1, z0, z1);
                std::fprintf(f, "     m %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g\n", r[0], r[1], r[2], r[3], r[4], r[5],
                             r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
                for (uint32_t k = 0; k < d.shader.count; ++k)
                {
                    const uint32_t n = std::min<uint32_t>(d.shader.address[k] == 0x1380u ? 32u : d.shader.address[k] == 0x3CC0u ? 12u : 6u,
                                                          static_cast<uint32_t>(d.shader.data.size()) - d.shader.first[k]);
                    std::fprintf(f, "     routine %04x (%d):", d.shader.address[k], static_cast<int>(d.shader.routine[k]));
                    for (uint32_t q = 0; q < n; ++q)
                    {
                        const float *v = d.shader.data[d.shader.first[k] + q].v;
                        if (d.shader.address[k] == 0x3CC0u && q >= 4u)
                        {
                            uint32_t w[4];
                            std::memcpy(w, v, 16u);
                            std::fprintf(f, " [%u] %08x %08x %08x %08x", q, w[0], w[1], w[2], w[3]);
                        }
                        else
                            std::fprintf(f, " [%u] %g %g %g %g", q, v[0], v[1], v[2], v[3]);
                    }
                    std::fprintf(f, "\n");
                }
                std::fprintf(f, "     type %u prim %03x/%03x colour %d normal %d%s target %08x tex0 %016llx alpha %llx test %llx texture %s\n",
                             m.type, m.prim, m.prim2, m.hasColour, m.hasNormal, m.immediate ? " IMMEDIATE" : "", d.target,
                             static_cast<unsigned long long>(d.tex0), static_cast<unsigned long long>(d.alpha),
                             static_cast<unsigned long long>(d.test), d.texture ? "yes" : "no");
                for (size_t i = 0; i < std::min<size_t>(3, m.vertices.size()); ++i)
                    std::fprintf(f, "     v %g %g %g c %08x uv %g %g\n", m.vertices[i].x, m.vertices[i].y, m.vertices[i].z,
                                 m.vertices[i].colour, m.vertices[i].u, m.vertices[i].v);
            }
            std::fclose(f);
        }

        void trackUpTo(const uint8_t *rdram, uint32_t to);

        // The frame's last part (the HUD, drawn after the 3D) before the list goes.
        // (TODO 146) The rain's blocks: taShader_Rain::LoadContext (0x5656B0) puts each pass's VU data in the display
        // list itself (func_221CC0: an UNPACK inline, its data returned), and the pass's flush (func_23AB70 / func_23AA90)
        // sends the list so far and starts it again from the same memory - the renderer's thread, reading the list's
        // stretches long after, saw every pass with the last pass's block (its 4 patches drawn 80 times, the others not
        // at all). The block copied while it still stands - before the next allocation, the next flush or the frame's end -
        // into the frame (Frame::rainBlocks), and nr_rain.cpp draws each pass's batches from its copy. TF_NATIVE_RAIN=0: not.
        constexpr uint32_t kListAlloc = 0x00221CC0u, kListAllocFromRain = TF_GA(0x005656F4u); // (the call in LoadContext + 8)
        constexpr uint32_t kListFlush = 0x0023AB70u, kListFlush2 = 0x0023AA90u;
        PS2Runtime::RecompiledFunction g_listAlloc = nullptr, g_listFlush = nullptr, g_listFlush2 = nullptr;
        uint32_t g_rainPending = 0u; // (the block of the pass not yet copied: its data's guest address)

        void copyRainBlock(const uint8_t *rdram)
        {
            if (!g_rainPending)
                return;
            const uint32_t at = g_rainPending;
            g_rainPending = 0u;
            const uint32_t patches = std::min<uint32_t>(word(rdram, at + 0xE0u), 23u);
            const uint32_t qwords = 16u + 2u * patches;
            std::vector<uint32_t> block(4u * qwords);
            for (uint32_t i = 0; i < 4u * qwords; ++i)
                block[i] = word(rdram, at + 4u * i);
            g_frame.rainBlocks.push_back(std::move(block));
        }

        void listAlloc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            copyRainBlock(rdram);
            const uint32_t ra = getRegU32(ctx, 31);
            g_listAlloc(rdram, ctx, runtime); // (a leaf: it returns)
            if (ra == kListAllocFromRain && ctx->pc == ra)
                g_rainPending = getRegU32(ctx, 2);
        }

        void listFlush(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            copyRainBlock(rdram);
            g_listFlush(rdram, ctx, runtime);
        }

        void listFlush2(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            copyRainBlock(rdram);
            if (lcCensus())
            {
                lcDiscover(rdram, runtime);
                const uint64_t t0 = CaptureClock::now();
                g_listFlush2(rdram, ctx, runtime);
                g_lcT.ctxTicks += CaptureClock::now() - t0, ++g_lcT.ctxCalls;
                return;
            }
            // (N5 step 2 part 2: the dirty shaders' contexts - all its LoadContexts wrote - taken out of the list)
            if (!g_divertFrame || g_divertDepth != 0 || ctx->pc != kListFlush2)
            {
                g_listFlush2(rdram, ctx, runtime);
                return;
            }
            const DivertOpen open = divertOpen(rdram, ctx);
            ++g_divertDepth;
            g_listFlush2(rdram, ctx, runtime);
            --g_divertDepth;
            divertClose(rdram, ctx, open, 0);
        }

        void endFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            copyRainBlock(rdram);
            resolveListStart(rdram);
            if (g_logTicks)
                std::fprintf(stderr, "[native render] end frame: list end %08x, packet %08x\n", g_packetEnd, word(rdram, kPacketPointer));
            if (g_packetEnd)
                {
                CaptureTimer timer;
                trackUpTo(rdram, word(rdram, kPacketPointer));
            }
            g_packetEnd = 0u;
            g_endFrame(rdram, ctx, runtime);
            // (N9: the frame's camera, for the camera's turn of each of its pictures - view_turn.h)
            g_frame.turnCamera = view_turn::endFrame(rdram, runtime ? runtime->eeScheduler().currentVSyncTick() : 0u);
            // (Debug: TF_NATIVE_GSDUMP=<tick>,<tbp hex>,<tbw>,<psm hex>,<w>,<h>: that region of GS memory as
            // gsdump_<tick>.ppm at the end of the frames from <tick> to <tick> + 3.)
            static const std::array<uint32_t, 6> gsDump = [] {
                std::array<uint32_t, 6> v{};
                if (const char *e = std::getenv("TF_NATIVE_GSDUMP"))
                    if (std::sscanf(e, "%u,%x,%u,%x,%u,%u", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
                        v[0] = 0u;
                return v;
            }();
            if (gsDump[0] && runtime)
            {
                const uint64_t tick = runtime->eeScheduler().currentVSyncTick();
                if (tick >= gsDump[0] && tick <= gsDump[0] + 3u)
                {
                    setGsMemory(runtime->memory().getGSVRAM());
                    char path[64];
                    std::snprintf(path, sizeof(path), "runs/gsdump_%llu.ppm", static_cast<unsigned long long>(tick));
                    dumpGsRegion(path, gsDump[1], gsDump[2], gsDump[3], gsDump[4], gsDump[5]);
                }
            }
        }

        void setTexture(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t device = getRegU32(ctx, 4), stage = getRegU32(ctx, 5), texture = getRegU32(ctx, 6);
            g_setTexture(rdram, ctx, runtime);
            ++g_setTextureCalls;
            if (stage < 2u)
                g_requested[stage] = texture;
            if (texture && g_recent.size() < 64u)
                g_recent.push_back(texture);
            if (stage < 2u)
            {
                const uint32_t kept = word(rdram, device + kDeviceTexture + 4u * stage);
                splitListHere(rdram);
                std::lock_guard<std::recursive_mutex> lock(g_texMutex);
            g_tbpTextures[word(rdram, kGsShadow + 8u * (6u + stage)) & 0x3FFFu] = kept;
                g_bindSeq[word(rdram, kGsShadow + 8u * (6u + stage)) & 0x3FFFu] = ++g_tableSeq;
                logChange(true, word(rdram, kGsShadow + 8u * (6u + stage)) & 0x3FFFu, Upload{kept, 0u, g_tableSeq});
            }
            if (g_why > 0)
            {
                --g_why;
                std::fprintf(stderr, "[native render] SetTexture(%08x, %u, %08x) -> kept %08x\n", device, stage, texture,
                             word(rdram, device + kDeviceTexture + 4u * (stage & 1u)));
            }
        }

        // A frame ends where the next begins: the draws so far are the frame (saved when asked).
        void beginFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint64_t tick = runtime->eeScheduler().currentVSyncTick();
            const size_t listThisFrame = g_frame.list.size(); // (before the frame goes to the renderer)
            const size_t drawsThisFrame = g_frame.draws.size(); // (likewise: reserved ahead below)
            // The display circuits (the GS's privileged registers): which buffers the picture is.
            {
                const GSRegisters &gs = runtime->memory().gs();
                g_frame.pmode = gs.pmode;
                g_frame.dispfb[0] = gs.dispfb1;
                g_frame.dispfb[1] = gs.dispfb2;
                g_frame.display[0] = gs.display1;
                g_frame.display[1] = gs.display2;
                g_frame.bgcolor = gs.bgcolor;
                g_lastFrameTick.store(tick, std::memory_order_relaxed);
                // (N3b: the display the game's frames show - gsDisplayFromFrames)
                g_framePmode.store(gs.pmode, std::memory_order_relaxed);
                g_frameDisplay.store(gs.display1, std::memory_order_relaxed);
                const uint32_t n = g_frameBuffersNext.fetch_add(1u, std::memory_order_relaxed);
                g_frameBuffers[n % 4u].store(gs.dispfb1 & 0x7FFFu, std::memory_order_relaxed);
            }
            takeInUpscaled();
            takeConverted(rdram);
            {
                static const bool vblankClock = std::getenv("TF_NATIVE_TEXCLOCK") && std::string(std::getenv("TF_NATIVE_TEXCLOCK")) == "vblank";
                g_textureTick = vblankClock ? tick : static_cast<uint64_t>(nowMs() * 0.06);
            }
            evictTextures(g_textureTick);
            evictLarge(g_textureTick);
            askMissingAgain(g_textureTick);
            reportLargeWaits();
            prefetchBlockTextures(rdram);
            {
                // (Textures let go of in numbers - a level or the HQ replaced: its successor's scanned for
                // large images a second and three seconds on; and once early, for the first screens.)
                std::lock_guard<std::recursive_mutex> lock(g_texMutex);
                static bool first = true;
                if (g_destroyedSince >= 32u || (first && tick >= 300u))
                {
                    if (g_destroyedSince >= 32u)
                    {
                        g_prefetched.clear();
                        g_prefetchSeen.clear();
                        g_prefetchGame.clear();
                    }
                    first = false;
                    g_destroyedSince = 0;
                    g_prefetchScanTicks = {tick + 60u, tick + 180u};
                }
                if (!g_prefetchScanTicks.empty() && tick >= g_prefetchScanTicks.front())
                {
                    g_prefetchScanTicks.erase(g_prefetchScanTicks.begin());
                    prefetchByScan(rdram);
                }
            }
            g_frame.overlaySqueeze = ps2_host_display::overlaySqueeze();
            g_frame.displayAspect = ps2_host_display::displayAspect();
            g_frame.tick = tick;
            // (N9 stage 2: the in-between field's moment, the governor's factor - 0 = the frame before's pose; whether a
            // movie plays - the menus' pictures between frames are not made then)
            std::memcpy(&g_frame.fieldBMoment, rdram + 0x006CD6F0u, sizeof(float));
            g_frame.movie = tf::port::moviePlaying();
            {
                // (N9: the frame's moment - the VBlank it was handed over at, as the host's schedule has it - which its
                // picture is paced by on the way to the screen; nr_d3d.cpp's hand-over, nr_present.cpp's pacing log.)
                std::chrono::steady_clock::time_point due{};
                if (!ps2_host_display::vblankDue(tick, due))
                    due = std::chrono::steady_clock::now();
                g_frame.dueNs = std::chrono::duration_cast<std::chrono::nanoseconds>(due.time_since_epoch()).count();
                g_frame.periodNs = std::chrono::duration_cast<std::chrono::nanoseconds>(ps2_host_display::vblankPeriodNow()).count();
                // (whether the VBlanks are timed by the screen's refresh - a clock of its own, a whole number of refreshes a
                // VBlank, as ps2_host_display's nextVBlank locks them: the paced hand-over then keeps to whole VBlanks)
                static int screenAsked = 0;
                static bool onRefresh = false;
                static int64_t refreshNs = 0, refreshVblankNs = 0;
                static bool variable = false;
                if (screenAsked-- <= 0) // (the screen looked at about every second)
                {
                    screenAsked = 60;
                    std::chrono::steady_clock::time_point vb{};
                    std::chrono::steady_clock::duration refresh{};
                    onRefresh = false;
                    variable = ps2_host_display::screenVariable();
                    refreshNs = refreshVblankNs = 0;
                    if (ps2_host_display::screenTiming(vb, refresh) && refresh.count() > 0)
                    {
                        refreshNs = std::chrono::duration_cast<std::chrono::nanoseconds>(refresh).count();
                        refreshVblankNs = std::chrono::duration_cast<std::chrono::nanoseconds>(vb.time_since_epoch()).count();
                    }
                    if (!ps2_host_display::screenVariable() && ps2_host_display::screenTiming(vb, refresh) && refresh.count() > 0 &&
                        g_frame.periodNs > 0)
                    {
                        const double ratio = static_cast<double>(g_frame.periodNs) /
                                             static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(refresh).count());
                        const double whole = std::round(ratio);
                        onRefresh = whole >= 1.0 && std::fabs(ratio - whole) <= 0.01 * whole;
                    }
                }
                g_frame.vblanksOnRefresh = onRefresh;
                g_frame.refreshNs = refreshNs;
                g_frame.refreshVblankNs = refreshVblankNs;
                g_frame.variableRefresh = variable;
            }
            // (Debug, roadmap N3: the sun's visibility test - TF_NATIVE_OCCLOG=<ticks between lines>. The occlusion
            // buffer D_006CEC5C, its 64 x 64 host copy D_006CEC68 - what the GS read-back wrote - and the visibility the
            // count gave, D_006CF870.)
            static const uint32_t occLog = std::getenv("TF_NATIVE_OCCLOG") ? static_cast<uint32_t>(std::atoi(std::getenv("TF_NATIVE_OCCLOG"))) : 0u;
            if (occLog && tick % occLog == 0u)
            {
                const uint32_t buffer = word(rdram, 0x006CEC5Cu), host = word(rdram, 0x006CEC68u);
                float visibility = 0.0f;
                std::memcpy(&visibility, rdram + 0x006CF870u, 4);
                uint32_t lo = 0xFFFFFFu, hi = 0u, zeros = 0u;
                uint64_t sum = 0u;
                if (host && host + 4096u * 4u < PS2_RAM_SIZE)
                    for (uint32_t i = 0; i < 4096u; ++i)
                    {
                        const uint32_t z = word(rdram, host + 4u * i) & 0xFFFFFFu;
                        lo = std::min(lo, z), hi = std::max(hi, z), sum += z, zeros += z == 0u ? 1u : 0u;
                    }
                std::fprintf(stderr, "[native render] occlusion at tick %llu: buffer %08x host %08x, z %06x..%06x mean %06llx (%u zero), visibility %.3f\n",
                             static_cast<unsigned long long>(tick), buffer, host, lo, hi, static_cast<unsigned long long>(sum / 4096u), zeros, visibility);
            }
            const bool shot = !g_shots.empty() && tick >= g_shots.front();
            // A scene in this frame (geometry sets), and the emulated drawing for the next one:
            // TF_NATIVE_ONLY=1 switches it off while the native renderer draws the scene (stage 5:
            // no VU1 programs, no GS primitives); a frame of 2D alone (menus, movies) has it back.
            g_frame.has3D = false;
            for (const Draw &d : g_frame.draws)
                if (d.mesh && !d.mesh->immediate && !d.mesh->primitive)
                {
                    g_frame.has3D = true;
                    break;
                }
            // (TODO 120) The scene's first frame after a load drawn by the game on its own (the warp tunnel: looseGif):
            // the GS shows black for it - its VU1 work draws nothing yet - where the native renderer drew it from the
            // camera and lights as they stood before the scene's own were set (low, close in front of Optimus, the
            // walls' screens flat). Its 3D left out (expandFrame), as the GS's. TF_NATIVE_LOADFIRST=1: drawn as before.
            {
                static const bool drawFirst = std::getenv("TF_NATIVE_LOADFIRST") && *std::getenv("TF_NATIVE_LOADFIRST") == '1';
                if (g_frame.has3D)
                {
                    if (g_looseSince3D.exchange(0u, std::memory_order_relaxed) != 0u && !drawFirst)
                    {
                        g_frame.firstAfterLoad = true;
                        static int told = 4;
                        if (told > 0)
                        {
                            --told;
                            std::fprintf(stderr, "[native render] the scene's first frame after a load (tick %llu): its 3D left out, as the GS shows it\n",
                                         static_cast<unsigned long long>(tick));
                        }
                    }
                }
            }
            // (Debug, TODO 120: TF_NATIVE_DISPLOG=<from>-<to> - each frame's display circuits and targets in those ticks)
            {
                static uint64_t dispFrom = 0u, dispTo = 0u;
                static const bool dispLog = [] {
                    const char *e = std::getenv("TF_NATIVE_DISPLOG");
                    if (!e || !*e)
                        return false;
                    char *end = nullptr;
                    dispFrom = std::strtoull(e, &end, 10);
                    dispTo = end && *end == '-' ? std::strtoull(end + 1, nullptr, 10) : dispFrom;
                    return true;
                }();
                if (dispLog && tick >= dispFrom && tick <= dispTo)
                {
                    uint32_t targets[6] = {};
                    size_t counts[6] = {}, nt = 0u, meshes = 0u;
                    for (const Draw &d : g_frame.draws)
                    {
                        meshes += d.mesh && !d.mesh->immediate && !d.mesh->primitive ? 1u : 0u;
                        size_t i = 0;
                        while (i < nt && targets[i] != d.target)
                            ++i;
                        if (i == nt && nt < 6u)
                            targets[nt++] = d.target;
                        if (i < 6u)
                            ++counts[i];
                    }
                    char line[256] = {};
                    size_t at = 0u;
                    for (size_t i = 0; i < nt && at < sizeof(line) - 32u; ++i)
                        at += static_cast<size_t>(std::snprintf(line + at, sizeof(line) - at, " %08x:%zu", targets[i], counts[i]));
                    std::fprintf(stderr, "[native render] display tick %llu: 3D %d (%zu meshes of %zu draws) PMODE %llx DISPFB1 %llx DISPLAY1 %llx DISPFB2 %llx BGCOLOR %llx movie %d; targets%s\n",
                                 static_cast<unsigned long long>(tick), g_frame.has3D ? 1 : 0, meshes, g_frame.draws.size(),
                                 static_cast<unsigned long long>(g_frame.pmode), static_cast<unsigned long long>(g_frame.dispfb[0]),
                                 static_cast<unsigned long long>(g_frame.display[0]), static_cast<unsigned long long>(g_frame.dispfb[1]),
                                 static_cast<unsigned long long>(g_frame.bgcolor), g_frame.movie ? 1 : 0, line);
                }
            }
            static const bool nativeOnly = std::getenv("TF_NATIVE_ONLY") && *std::getenv("TF_NATIVE_ONLY") == '1';
            if (nativeOnly)
            {
                // Roadmap N3b: the native renderer draws the frames of 2D alone too (menus, loading screens, movies),
                // so the emulated drawing stays off for them; TF_NATIVE_EMU2D=1 brings them back to the GS as before.
                // Only the device lost has the GS draw again - and the first frames, before Direct3D 12 is ready (made on its
                // thread, ~1-4 s), are the GS's: the emulated drawing starts on and goes off once, for good.
                static const bool emu2D = std::getenv("TF_NATIVE_EMU2D") && *std::getenv("TF_NATIVE_EMU2D") == '1';
                static const bool emuLog = std::getenv("TF_NATIVE_EMULOG") && *std::getenv("TF_NATIVE_EMULOG") == '1';
                // (TF_NATIVE_EMULOG=1: the stretches of 2D alone logged - the frames that took the emulated drawing back
                // before N3b - with whether a movie plays and how many draws they have)
                static bool flatBefore = false;
                static uint64_t flatFrom = 0u;
                static size_t flatDrawsMin = SIZE_MAX, flatDrawsMax = 0u;
                static bool flatMovie = false;
                if (emuLog)
                {
                    const bool flat = !g_frame.has3D;
                    if (flat && !flatBefore)
                        flatFrom = tick, flatDrawsMin = SIZE_MAX, flatDrawsMax = 0u, flatMovie = false;
                    if (flat)
                        flatDrawsMin = std::min(flatDrawsMin, g_frame.draws.size()), flatDrawsMax = std::max(flatDrawsMax, g_frame.draws.size()),
                        flatMovie = flatMovie || g_frame.movie;
                    if (!flat && flatBefore)
                        std::fprintf(stderr, "[native render] 2D alone: ticks %llu-%llu (%zu-%zu draws%s)\n", static_cast<unsigned long long>(flatFrom),
                                     static_cast<unsigned long long>(tick), flatDrawsMin, flatDrawsMax, flatMovie ? ", a movie played" : "");
                    flatBefore = flat;
                }
                const bool emulate = (emu2D && !g_frame.has3D) || !d3d::drawsPictures(); // (the device lost: the GS draws again)
                if (emulate != ps2_host_display::emulatedDrawing())
                {
                    ps2_host_display::setEmulatedDrawing(emulate);
                    // (TF_NATIVE_EMULOG=1: every switch logged, not only the first six)
                    static int told = std::getenv("TF_NATIVE_EMULOG") && *std::getenv("TF_NATIVE_EMULOG") == '1' ? 1 << 30 : 6;
                    if (told > 0)
                    {
                        --told;
                        std::fprintf(stderr, "[native render] emulated drawing %s (tick %llu%s, %zu draws)\n", emulate ? "on" : "off",
                                     static_cast<unsigned long long>(tick), g_frame.movie ? ", a movie plays" : "", g_frame.draws.size());
                    }
                }
            }
            if (!shot)
            {
                // Every frame is drawn, not only the ones saved: a frame reads what earlier frames left
                // in their buffers (the 3D field is copied into the display buffer the frame after).
                // TF_NATIVE_EVERYFRAME=0 draws only the saved frames (the old, faster capture runs).
                static const bool everyFrame = [] {
                    const char *v = std::getenv("TF_NATIVE_EVERYFRAME");
                    return !v || *v != '0';
                }();
                if (everyFrame && (!g_frame.draws.empty() || !g_frame.ranges.empty()))
                {
                    static double spent = 0.0, worst = 0.0;
                    static uint32_t frames = 0u;
                    const auto start = std::chrono::steady_clock::now();
                    warp::gameFrame(); // (TODO 27: the game's frames back - a native warp tunnel's pictures stop)
                    submitFrame(std::move(g_frame), std::string());
                    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                    spent += ms;
                    worst = std::max(worst, ms);
                    if (++frames == 300u)
                    {
                        const double tick = CaptureClock::msPerTick(); // (the timers' counter ticks -> ms)
                        g_captureMs *= tick;
                        g_partMs2D *= tick;
                        for (double &m : g_partMs)
                            m *= tick;
                        std::fprintf(stderr, "[native render] 300 frames drawn: %.2f ms a frame on average, %.2f at worst (%zu draws now); capture %.2f ms a frame (tracking %.2f, of it 2D %.2f; grass %.2f, meshes %.2f, textures %.2f, shader state %.2f; list %zu KB)\n",
                                     spent / 300.0, worst, g_frame.draws.size(), g_captureMs / 300.0, g_partMs[0] / 300.0, g_partMs2D / 300.0,
                                     g_partMs[1] / 300.0,
                                     g_partMs[2] / 300.0, g_partMs[3] / 300.0, g_partMs[4] / 300.0, g_listWords * 4u / 1024u);
                        // (TODO 164) The conversion pool since the last line: textures converted on it, the workers' time,
                        // and the renderer's waits for one not done yet.
                        if (convertAsync())
                        {
                            static uint64_t made0 = 0, work0 = 0, waits0 = 0, waited0 = 0, same0 = 0;
                            const uint64_t made = g_convertedAsync.load(), work = g_convertWorkUs.load(), waits = g_convertWaits.load(),
                                           waited = g_convertWaitUs.load(), same = g_sameFrameLarge.load();
                            if (made != made0)
                                std::fprintf(stderr, "[native render] conversion pool: %llu textures converted (%.1f ms of work), the renderer waited for %llu of them (%.1f ms); %llu took their large image at once\n",
                                             static_cast<unsigned long long>(made - made0), (work - work0) / 1000.0, static_cast<unsigned long long>(waits - waits0),
                                             (waited - waited0) / 1000.0, static_cast<unsigned long long>(same - same0));
                            made0 = made, work0 = work, waits0 = waits, waited0 = waited, same0 = same;
                        }
                        g_captureMs = 0.0;
                        g_partMs2D = 0.0;
                        for (double &m : g_partMs)
                            m = 0.0;
                        spent = worst = 0.0;
                        frames = 0u;
                    }
                }
            }
            else
            {
                const uint64_t asked = g_shots.front();
                g_shots.erase(g_shots.begin());
                g_frame.tick = tick;
                std::fprintf(stderr,
                             "[native render] frame at tick %llu: %zu draws, %zu meshes (%llu decoded, %llu empty), "
                             "%llu immediate so far, textures %llu converted (%llu upscaled, %llu of them a large image shared, %llu large let go of for video memory), %llu not, %llu draws with no upload known, state blocks bound %llu by TEX0, %llu guessed, %llu bindings larger than TEX0 given the upload at TBP, %llu bindings older than the upload at TBP given it; %llu kept in GS memory alone (from targets), %llu group loads (%llu levels); %llu with the game's own mip levels\n",
                             static_cast<unsigned long long>(tick), g_frame.draws.size(), g_meshes.size(),
                             static_cast<unsigned long long>(g_decoded), static_cast<unsigned long long>(g_failed),
                             static_cast<unsigned long long>(g_immediate),
                             static_cast<unsigned long long>(g_texturesConverted), static_cast<unsigned long long>(g_texturesUpscaled),
                             static_cast<unsigned long long>(g_largeShares), static_cast<unsigned long long>(g_largeEvicted),
                             static_cast<unsigned long long>(g_texturesFailed), static_cast<unsigned long long>(g_texturesUnknown),
                             static_cast<unsigned long long>(g_bindExact), static_cast<unsigned long long>(g_bindGuess), static_cast<unsigned long long>(g_boundRefit),
                             static_cast<unsigned long long>(g_boundByUpload),
                             static_cast<unsigned long long>(g_texturesVram), static_cast<unsigned long long>(g_groupLoads),
                             static_cast<unsigned long long>(g_groupLevels), static_cast<unsigned long long>(g_texturesGameMips));
                for (Counted &c : g_counted)
                    if (c.original)
                    {
                        std::fprintf(stderr, "[native render]   %s: %llu calls\n", c.name, static_cast<unsigned long long>(c.calls));
                        c.calls = 0;
                    }
                {
                    const GSRegisters &gs = runtime->memory().gs();
                    std::fprintf(stderr, "[native render] PMODE %llx DISPFB1 %llx DISPLAY1 %llx DISPFB2 %llx DISPLAY2 %llx\n",
                                 static_cast<unsigned long long>(gs.pmode), static_cast<unsigned long long>(gs.dispfb1),
                                 static_cast<unsigned long long>(gs.display1), static_cast<unsigned long long>(gs.dispfb2),
                                 static_cast<unsigned long long>(gs.display2));
                }
                if (g_dump)
                {
                    // The GS register shadow (func_00225D60's), to find XYOFFSET / SCISSOR / FRAME.
                    if (FILE *f = std::fopen("runs/native_shadow.txt", "w"))
                    {
                        for (uint32_t a = 0x70000000u; a < 0x70000400u; a += 8u)
                            if (word(rdram, a) || word(rdram, a + 4u))
                                std::fprintf(f, "%08x: %08x %08x\n", a, word(rdram, a + 4u), word(rdram, a));
                        std::fclose(f);
                    }
                    dumpFrame("runs\\native_" + std::to_string(asked) + ".txt");
                }
                {
                    // The shader combos the frame drew with (routine addresses, ? = not identified).
                    std::unordered_map<std::string, int> combos;
                    for (const Draw &d : g_frame.draws)
                    {
                        std::string key;
                        char part[16];
                        for (uint32_t k = 0; k < d.shader.count; ++k)
                        {
                            std::snprintf(part, sizeof(part), "%s%04x%s", k ? " " : "", d.shader.address[k],
                                          d.shader.routine[k] == Routine::Unknown ? "?" : "");
                            key += part;
                        }
                        std::snprintf(part, sizeof(part), " t%u", d.mesh->type);
                        key += part;
                        ++combos[key];
                    }
                    std::vector<std::pair<int, std::string>> sorted;
                    for (auto &c : combos)
                        sorted.push_back({c.second, c.first});
                    std::sort(sorted.rbegin(), sorted.rend());
                    for (size_t i = 0; i < sorted.size() && i < 20; ++i)
                        std::fprintf(stderr, "[native render]   %4d draws: %s\n", sorted[i].first, sorted[i].second.c_str());
                }
                warp::gameFrame();
                submitFrame(std::move(g_frame), "runs\\native_" + std::to_string(asked) + ".bmp");
            }
            g_frame.draws.clear();
            g_frame.list.clear();
            // (TODO 94: a frame handed to the renderer took its vectors with it - the ones of a frame it is done with
            // come back here, so the frame's draws are written into memory already touched, not a fresh megabyte)
            takeFrameBuffers(g_frame.draws, g_frame.list);
            g_interframeCapture.drain(g_frame.interframeMicrocode);
            // The previous capture has already been submitted, including the
            // screenshot path. Only the newly starting frame gets this proof.
            captureFrameTexturePolicy(g_frame, g_frameTexturePolicy, rdram, 0x02000000u);
            g_frame.ranges.clear();
            g_frame.rainBlocks.clear(); // (TODO 146: the rain's blocks go with their frame)
            g_frame.vuChunks.clear(); // (N5 step 2 part 2: and the contexts taken out of the list - their memory reused)
            takeChunkBuffer(g_frame.vuChunks);
            if (contextsMode() != 0)
                divertReport();
            if (lcCensus())
                lcReport();
            g_divertFrame = contextsMode() != 0 && framesDeferred() && n5::liteNow();
            g_divertDepth = 0;
            g_chunkPending = 0;
            g_frame.regStream.clear(); // (N5.1: the register stream too)
            g_frame.nativeRegs.clear(); // (N5.3: and the native functions' registers)
            g_frame.firstAfterLoad = false;
            g_primitives.clear(); // (a batch not ended in its frame: not drawn)
            static size_t listSeen = 0; // (the frame list's size, reserved ahead: it grows to megabytes)
            listSeen = std::max(listSeen * 7 / 8, listThisFrame);
            g_frame.list.clear(); // (a frame not drawn: its words dropped with it)
            g_frame.list.reserve(listSeen + listSeen / 4);
            // (FPS: the draws too - growing the vector moved every Draw, ~600 bytes each, about 11
            // times a frame: 1.5% of the game's thread in Amazon)
            static size_t drawsSeen = 0;
            drawsSeen = std::max(drawsSeen * 7 / 8, drawsThisFrame);
            g_frame.draws.reserve(drawsSeen + drawsSeen / 4);
            g_listWords = listSeen;
            g_packetEnd = 0u;
            {
                std::lock_guard<std::mutex> lock(g_spritesMutex);
                for (auto it = g_sprites.begin(); it != g_sprites.end();)
                    it = it->second.first.expired() ? g_sprites.erase(it) : std::next(it);
            }
            static uint32_t sinceScan = 1000u;
            if (g_scanAsked && ++sinceScan >= 120u)
            {
                g_scanAsked = false;
                sinceScan = 0u;
                scanTextures(rdram);
            }
            static int listLog = std::getenv("TF_NATIVE_LISTLOG") ? std::atoi(std::getenv("TF_NATIVE_LISTLOG")) : 0;
            const uint32_t endedAt = word(rdram, kPacketPointer);
            g_gp = getRegU32(ctx, 28);
            g_beginFrame(rdram, ctx, runtime);
            // A synchronous native BeginFrame may enqueue a program while no
            // display-list range is active; it belongs to this new capture.
            g_interframeCapture.drain(g_frame.interframeMicrocode);
            g_packetEnd = word(rdram, kPacketPointer); // (a fallback: resolveListStart takes the list's start)
            g_listStartPending = true;
            {
                static const uint64_t rangeLog = std::getenv("TF_NATIVE_RANGELOG") ? std::strtoull(std::getenv("TF_NATIVE_RANGELOG"), nullptr, 10) : 0u;
                g_curTick = tick;
                g_logTicks = rangeLog && tick + 3u >= rangeLog && tick <= rangeLog;
                if (g_logTicks)
                    std::fprintf(stderr, "[native render] begin frame (tick %llu): list ended at %08x, begins at %08x\n",
                                 static_cast<unsigned long long>(tick), endedAt, g_packetEnd);
            }
            if (listLog > 0 && tick > 9000u)
            {
                --listLog;
                std::fprintf(stderr, "[native render] list: frame ended at %08x, next begins at %08x (tick %llu)\n", endedAt, g_packetEnd,
                             static_cast<unsigned long long>(tick));
            }
        }

        PS2Runtime::RecompiledFunction wrap(PS2Runtime &runtime, uint32_t address, PS2Runtime::RecompiledFunction fn)
        {
            PS2Runtime::RecompiledFunction original = runtime.lookupFunction(address);
            if (original)
                runtime.registerFunction(address, fn);
            else
                std::fprintf(stderr, "[native render] nothing at %08x to wrap\n", address);
            return original;
        }

        // (TODO 118) The wrapper also at each place inside the function where it is entered again after it stopped part-way
        // (the function table's entries there name the same recompiled function); the places found.
        int wrapResumePoints(PS2Runtime &runtime, uint32_t address, uint32_t end, PS2Runtime::RecompiledFunction original, PS2Runtime::RecompiledFunction fn)
        {
            int count = 0;
            if (!original)
                return 0;
            for (uint32_t a = address + 4u; a < end; a += 4u)
                if (runtime.hasFunction(a) && runtime.lookupFunction(a) == original && runtime.registerFunction(a, fn))
                    ++count;
            return count;
        }

        // A geometry set's GS state where the list overrides its state block (TODO 61). The block's
        // TEX0_1 / ALPHA_1 / TEST_1 are what renderGeometrySet takes, but the game can write its own
        // after the block, into the list itself (VIF DIRECT before the set's call): the destroyed
        // enemies' pieces (PieceGod) - the creature's material block says TEX0 HIGHLIGHT2 with ALPHA
        // Cs * FIX, the pieces' stretch writes TEX0 MODULATE for the same texture and ALPHA
        // (Cs - Cd) * FIX + Cd (FIX their fade). Taken from the block, HIGHLIGHT2 added the vertex alpha
        // (the light routine's q2.w * 128) to the colour: the pieces drew solid white. The list's
        // registers count when written in this draw's own stretch (after the draw before) and, for
        // TEX0, when they sample the same texture as the block's (the texture stays the block's).
        // TF_NATIVE_LISTSTATE=0: the block's, as before; TF_NATIVE_LISTSTATELOG=<n>: the first n
        // overrides logged (and the list TEX0s written in a draw's stretch that were not taken).
        // TODO 140: a chrome-lit creature's block has the reflection map as TEX0_1 and its own texture
        // as TEX0_2 (as the dash's after-image above): the pieces' list TEX0_1 then samples the block's
        // context-2 texture - taken too, the draw's texture becoming that one (else the pieces drew with
        // the reflection map alone: flat grey metal). TF_NATIVE_LISTSTATE2=0: only context 1's, as before.
        void listOverrides(Draw &d, uint64_t writesBefore, const gif::State &gs)
        {
            static const bool off = std::getenv("TF_NATIVE_LISTSTATE") && *std::getenv("TF_NATIVE_LISTSTATE") == '0';
            static const bool context2Off = std::getenv("TF_NATIVE_LISTSTATE2") && *std::getenv("TF_NATIVE_LISTSTATE2") == '0';
            static int log = std::getenv("TF_NATIVE_LISTSTATELOG") ? std::atoi(std::getenv("TF_NATIVE_LISTSTATELOG")) : 0;
            if (off || !d.mesh || gs.tex0At[0] <= writesBefore || !gs.tex0[0] || gs.tex0[0] == d.tex0)
                return;
            const uint64_t listSample = gs.tex0[0] & kTex0SampleMask;
            const bool sameTexture = listSample == (d.tex0 & kTex0SampleMask);
            const bool context2 = !sameTexture && !context2Off && d.texture2 && d.tex02 && listSample == (d.tex02 & kTex0SampleMask);
            if (!sameTexture && !context2)
            {
                if (log > 0)
                {
                    --log;
                    std::fprintf(stderr, "[native render] list state NOT taken: list tex0 %016llx, block tex0 %016llx / context 2 %016llx%s\n",
                                 static_cast<unsigned long long>(gs.tex0[0]), static_cast<unsigned long long>(d.tex0),
                                 static_cast<unsigned long long>(d.tex02), d.texture2 ? "" : " (no context-2 texture)");
                }
                return;
            }
            if (log > 0)
            {
                --log;
                std::fprintf(stderr, "[native render] list state over the block%s: tex0 %016llx -> %016llx, alpha %llx -> %llx%s, test %llx -> %llx%s\n",
                             context2 ? " (context 2's texture)" : "", static_cast<unsigned long long>(d.tex0), static_cast<unsigned long long>(gs.tex0[0]),
                             static_cast<unsigned long long>(d.alpha), static_cast<unsigned long long>(gs.alpha[0]),
                             gs.alphaAt[0] > writesBefore ? "" : " (not this stretch's)", static_cast<unsigned long long>(d.test),
                             static_cast<unsigned long long>(gs.test[0]), gs.testAt[0] > writesBefore ? "" : " (not this stretch's)");
            }
            if (context2)
            {
                d.texture = d.texture2;
                d.topPalette = nullptr;
                d.tex1 = d.tex12;
            }
            d.tex0 = gs.tex0[0];
            if (gs.alphaAt[0] > writesBefore)
                d.alpha = gs.alpha[0];
            if (gs.testAt[0] > writesBefore)
                d.test = gs.test[0];
        }
    }

    void finishDraw(Draw &d) { finishDrawImpl(d); }

    namespace
    {
        // (N5.1, TF_N5_NOLIST=check) The registers the draws' state reads - both contexts' FRAME, ZBUF, ALPHA, TEST, TEX0,
        // TEX1, CLAMP, XYOFFSET, SCISSOR, and whether TEX0 / ALPHA / TEST were written in the draw's own stretch (TODO 61)
        // - as the device's calls wrote them against the display list's, at every 3D draw ([n5:nolist]).
        struct NoListCheck
        {
            uint64_t draws = 0, differing = 0;
            uint64_t byField[20] = {};
            int told = 0;
            std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
        } g_noListCheck;
        constexpr const char *kNoListFields[20] = {"FRAME_1", "FRAME_2", "ZBUF_1", "ZBUF_2", "ALPHA_1", "ALPHA_2", "TEST_1", "TEST_2",
                                                   "TEX0_1", "TEX0_2", "TEX1_1", "TEX1_2", "CLAMP_1", "CLAMP_2", "XYOFFSET_1",
                                                   "XYOFFSET_2", "SCISSOR_1", "SCISSOR_2", "TEX0/ALPHA/TEST_1 written in the stretch", "SCANMSK"};
    }

    void checkCallRegisters(uint64_t tick, size_t draw, uint64_t listWritesBefore, uint64_t callWritesBefore)
    {
        NoListCheck &c = g_noListCheck;
        const gif::State &l = g_gif, &n = g_callGif;
        const uint64_t lv[20] = {l.frame[0], l.frame[1], l.zbuf[0], l.zbuf[1], l.alpha[0], l.alpha[1], l.test[0], l.test[1], l.tex0[0], l.tex0[1],
                                 l.tex1[0], l.tex1[1], l.clamp[0], l.clamp[1], l.xyoffset[0], l.xyoffset[1], l.scissor[0], l.scissor[1],
                                 (l.tex0At[0] > listWritesBefore ? 1u : 0u) | (l.alphaAt[0] > listWritesBefore ? 2u : 0u) | (l.testAt[0] > listWritesBefore ? 4u : 0u),
                                 l.scanmsk};
        const uint64_t nv[20] = {n.frame[0], n.frame[1], n.zbuf[0], n.zbuf[1], n.alpha[0], n.alpha[1], n.test[0], n.test[1], n.tex0[0], n.tex0[1],
                                 n.tex1[0], n.tex1[1], n.clamp[0], n.clamp[1], n.xyoffset[0], n.xyoffset[1], n.scissor[0], n.scissor[1],
                                 (n.tex0At[0] > callWritesBefore ? 1u : 0u) | (n.alphaAt[0] > callWritesBefore ? 2u : 0u) | (n.testAt[0] > callWritesBefore ? 4u : 0u),
                                 n.scanmsk};
        ++c.draws;
        bool any = false;
        for (int f = 0; f < 20; ++f)
            if (lv[f] != nv[f])
            {
                ++c.byField[f];
                if (!any && c.told < 30)
                {
                    ++c.told;
                    std::fprintf(stderr, "[n5:nolist] tick %llu draw %zu: %s - list %016llx, calls %016llx\n", static_cast<unsigned long long>(tick), draw,
                                 kNoListFields[f], static_cast<unsigned long long>(lv[f]), static_cast<unsigned long long>(nv[f]));
                }
                any = true;
            }
        c.differing += any ? 1u : 0u;
        const auto now = std::chrono::steady_clock::now();
        if (now - c.last >= std::chrono::seconds(10))
        {
            c.last = now;
            std::string fields;
            for (int f = 0; f < 20; ++f)
                if (c.byField[f])
                    fields += std::string(fields.empty() ? "" : ", ") + kNoListFields[f] + " " + std::to_string(c.byField[f]);
            std::fprintf(stderr, "[n5:nolist] 10 s: %llu 3D draws, %llu with registers from the calls other than the list's%s%s\n",
                         static_cast<unsigned long long>(c.draws), static_cast<unsigned long long>(c.differing), fields.empty() ? "" : ": ", fields.c_str());
            c.draws = c.differing = 0;
            std::memset(c.byField, 0, sizeof(c.byField));
        }
    }

    void placeChanged()
    {
        g_placeChanged.store(true, std::memory_order_release);
    }

    void deviceRegister(const uint8_t *rdram, uint32_t reg, uint64_t value)
    {
        streamRegister(reg, value);
        nativeRegister(rdram, reg, value);
        if ((reg & 0x7Fu) != 0x3Fu) // (kept from the GS: written again from the shadow if the originals come back - not TEXFLUSH)
            g_nativeRegsDirty[(reg >> 6) & 1u] |= 1ull << (reg & 63u);
    }

    void requestShot(uint64_t tick)
    {
        if (!g_enabled)
            return;
        g_shots.insert(std::lower_bound(g_shots.begin(), g_shots.end(), tick), tick);
    }

    void expandFrame(Frame &frame)
    {
        if (!g_rdram)
            return;
        g_expandTick = frame.tick;
        // Replay actual earlier enqueue commands, not a late VU-state snapshot.
        g_interframeCapture.replay(frame.interframeMicrocode,[&](const std::vector<uint32_t>&commands) {
            replayList(commands.data(),commands.size());
            if(std::getenv("TF_NATIVE_MICROCODE_TRACE"))std::fprintf(stderr,"[native render] interframe MPG replay tick=%llu epoch=%llu words=%zu\n",
                static_cast<unsigned long long>(frame.tick),static_cast<unsigned long long>(frame.interframeMicrocode.epoch),commands.size());
        });
        particles::noteTick(frame.tick);
        rain::noteTick(frame.tick);
        rain::beginFrame(&frame.rainBlocks); // (TODO 146: the passes' blocks, copied when they were made)
        static thread_local std::vector<uint32_t> words;
        std::vector<Draw> out;
        out.reserve(frame.draws.size() * 2u + 64u);
        t_out = &out;
        t_useView = framesDeferred(); // (the 2D's textures from the tables as the list stood)
        // (Debug: TF_NATIVE_RANGELOG=<tick>: the display list's stretches of the frame at that tick,
        // and the DMA chain of any that does not read.)
        static const uint64_t rangeLog = std::getenv("TF_NATIVE_RANGELOG") ? std::strtoull(std::getenv("TF_NATIVE_RANGELOG"), nullptr, 10) : 0u;
        const bool logRanges = rangeLog && frame.tick >= rangeLog && frame.tick < rangeLog + 2u;
        // (N5.3) The native functions' registers (Frame::nativeRegs): applied to the list's state where their packet would
        // have been - before the first of the list's own tags at or after the place the function ran (`at`), and at the
        // latest before the draw they come before.
        size_t nw = 0;
        static thread_local std::vector<std::pair<uint32_t, uint32_t>> positions;
        uint32_t readingBefore = 0;
        auto applyNative = [&](uint32_t upToAt, uint32_t beforeDraw) {
            while (nw < frame.nativeRegs.size() && frame.nativeRegs[nw].before <= beforeDraw && frame.nativeRegs[nw].at <= upToAt)
            {
                const Frame::NativeWrite &w = frame.nativeRegs[nw++];
                if (w.reg == kNativeBlock)
                {
                    // (a state block applied natively: its GIF packet, from guest memory as the list's REF of it was read)
                    const uint32_t block = static_cast<uint32_t>(w.value), qwords = static_cast<uint32_t>(w.value >> 32);
                    if (const uint8_t *at = getConstMemPtr(g_rdram, block); at && qwords && qwords <= 65u)
                        gif::run(g_gif, reinterpret_cast<const uint32_t *>(at), qwords, [&](const gif::Primitives &p) { addImmediate(g_rdram, p); });
                    continue;
                }
                const uint32_t packet[8] = {0x00008001u, 0x10000000u, 0x0000000Eu, 0u, static_cast<uint32_t>(w.value),
                                            static_cast<uint32_t>(w.value >> 32), w.reg, 0u}; // (a tag: NLOOP 1, EOP, one A+D)
                gif::run(g_gif, packet, 2u, [&](const gif::Primitives &p) { addImmediate(g_rdram, p); });
            }
        };
        // (where the words from `w` stand in the list: the tag of the list's own level they come under)
        auto listAt = [&](const uint32_t *w) -> uint32_t {
            if (positions.empty())
                return 0u;
            const uint32_t index = static_cast<uint32_t>(w - words.data());
            auto it = std::upper_bound(positions.begin(), positions.end(), index,
                                       [](uint32_t i, const std::pair<uint32_t, uint32_t> &p) { return i < p.first; });
            return it == positions.begin() ? positions.front().second & 0x1FFFFFFFu : std::prev(it)->second & 0x1FFFFFFFu;
        };
        // (N5 step 2 part 2, TF_N5_CONTEXTS) The shaders' contexts the capture took out of the list (Frame::vuChunks), each put
        // back into the list's words where it stood: before the first of the list's own tags at or after its place, in the
        // first stretch read before the draw it came before (a chunk with no stretch: replayed alone before that draw).
        struct Chunk
        {
            uint32_t before, range, at, end;
            const uint32_t *w;
            uint32_t count;
        };
        static thread_local std::vector<Chunk> chunks;
        chunks.clear();
        for (size_t i = 0; i + 5u <= frame.vuChunks.size();)
        {
            const uint32_t *h = frame.vuChunks.data() + i;
            if (i + 5u + h[4] > frame.vuChunks.size())
                break;
            if (h[1] != ~0u) // (~0: in a part of the list the capture left out)
                chunks.push_back(Chunk{h[0], h[1], h[2], h[3], h + 5, h[4]});
            i += 5u + h[4];
        }
        size_t nc = 0;
        const int ctxMode = contextsMode();
        static thread_local std::vector<uint32_t> merged;
        static thread_local std::vector<std::pair<uint32_t, uint32_t>> mergedAt;
        // `in` (with its tags' places `inAt`) and the chunks [c0, c1) put back where they stood, into merged / mergedAt.
        auto splice = [&](const std::vector<uint32_t> &in, const std::vector<std::pair<uint32_t, uint32_t>> &inAt, size_t c0, size_t c1) {
            merged.clear();
            mergedAt.clear();
            size_t wi = 0, pi = 0;
            auto copyUpTo = [&](size_t end) {
                const size_t shift = merged.size() - wi;
                while (pi < inAt.size() && inAt[pi].first < end)
                {
                    mergedAt.push_back({static_cast<uint32_t>(inAt[pi].first + shift), inAt[pi].second});
                    ++pi;
                }
                merged.insert(merged.end(), in.begin() + static_cast<std::ptrdiff_t>(wi), in.begin() + static_cast<std::ptrdiff_t>(end));
                wi = end;
            };
            for (size_t c = c0; c < c1; ++c)
            {
                size_t p = pi;
                while (p < inAt.size() && (inAt[p].second & 0x1FFFFFFFu) < chunks[c].at)
                    ++p;
                copyUpTo(p < inAt.size() ? inAt[p].first : in.size());
                mergedAt.push_back({static_cast<uint32_t>(merged.size()), chunks[c].at});
                merged.insert(merged.end(), chunks[c].w, chunks[c].w + chunks[c].count);
            }
            copyUpTo(in.size());
        };
        // (=check: the chunks were left in the list too) The list's words with the chunks' spans taken out and the chunks
        // spliced back must be the list's words; no tag of the list may point into a span; and VIF1's CYCLE at each of the
        // list's other UNPACKs must be the same without the spans' STCYCLs (what VIF1 skips by while lite).
        static thread_local std::vector<uint32_t> stripped;
        static thread_local std::vector<std::pair<uint32_t, uint32_t>> strippedAt;
        static thread_local std::vector<uint8_t> spanWord;
        auto inSpan = [&](uint32_t addr, size_t c0, size_t c1) {
            for (size_t c = c0; c < c1; ++c)
                if (addr >= chunks[c].at && addr < chunks[c].end)
                    return true;
            return false;
        };
        uint32_t rFrom = 0u, rTo = 0u, rBefore = 0u; // (the stretch being read: the check's log)
        auto checkChunks = [&](const std::vector<uint32_t> &in, const std::vector<std::pair<uint32_t, uint32_t>> &inAt, size_t c0, size_t c1) {
            ContextsCheck &ck = contextsCheck();
            stripped.clear();
            strippedAt.clear();
            spanWord.assign(in.size(), 0u);
            for (size_t p = 0; p < inAt.size(); ++p)
            {
                const uint32_t addr = inAt[p].second & 0x1FFFFFFFu;
                const size_t from = inAt[p].first, to = p + 1u < inAt.size() ? inAt[p + 1u].first : in.size();
                const uint32_t lo = word(g_rdram, inAt[p].second), hi = word(g_rdram, inAt[p].second + 4u), id = (lo >> 28) & 7u;
                if ((id == 0u || id == 2u || id == 3u || id == 4u || id == 5u) && inSpan(hi & 0x1FFFFFF0u, 0, chunks.size()))
                    ++ck.refInto;
                if (inSpan(addr, c0, c1))
                {
                    std::fill(spanWord.begin() + static_cast<std::ptrdiff_t>(from), spanWord.begin() + static_cast<std::ptrdiff_t>(to), 1u);
                    continue;
                }
                strippedAt.push_back({static_cast<uint32_t>(stripped.size()), inAt[p].second});
                stripped.insert(stripped.end(), in.begin() + static_cast<std::ptrdiff_t>(from), in.begin() + static_cast<std::ptrdiff_t>(to));
            }
            for (size_t c = c0; c < c1; ++c)
            {
                bool found = false;
                for (const auto &pa : inAt)
                    if ((pa.second & 0x1FFFFFFFu) >= chunks[c].at && (pa.second & 0x1FFFFFFFu) < chunks[c].end)
                        found = true;
                if (!found && ck.toldLost++ < 12)
                    std::fprintf(stderr, "[n5:contexts] check: a chunk not in its stretch (tick %llu): chunk %08x..%08x before draw %u, "
                                         "stretch %08x..%08x before draw %u, %zu tags %08x..%08x\n",
                                 static_cast<unsigned long long>(frame.tick), chunks[c].at, chunks[c].end, chunks[c].before, rFrom, rTo, rBefore,
                                 inAt.size(), inAt.empty() ? 0u : inAt.front().second, inAt.empty() ? 0u : inAt.back().second);
                if (!found)
                    ++ck.lost;
            }
            splice(stripped, strippedAt, c0, c1);
            ++ck.ranges;
            ck.chunks += c1 - c0;
            if (merged != in)
            {
                ++ck.mismatch;
                if (ck.told++ < 8)
                {
                    size_t i = 0;
                    while (i < merged.size() && i < in.size() && merged[i] == in[i])
                        ++i;
                    std::fprintf(stderr, "[n5:contexts] check: a stretch differs (tick %llu, before draw %u): %zu words against %zu, first "
                                         "difference at word %zu\n",
                                 static_cast<unsigned long long>(frame.tick), chunks[c0].before, merged.size(), in.size(), i);
                }
            }
            uint32_t cycleAll = 0u, cycleLite = 0u;
            for (size_t i = 0; i < in.size();)
            {
                const uint32_t code = in[i], cmd = (code >> 24) & 0x7Fu;
                if (cmd == 0x01u)
                {
                    cycleAll = code & 0xFFFFu;
                    if (!spanWord[i])
                        cycleLite = code & 0xFFFFu;
                }
                else if ((cmd & 0x60u) == 0x60u && !spanWord[i] && cycleAll != cycleLite)
                {
                    const uint32_t a = cycleAll, l = cycleLite;
                    if (((a >> 8) & 0xFFu) > (a & 0xFFu) || ((l >> 8) & 0xFFu) > (l & 0xFFu))
                        ++ck.cycleDiffers; // (a length VIF1 would take otherwise)
                }
                i += vifLengthAny(in.data() + i, cycleAll);
            }
        };
        auto read = [&](const Frame::Range &r) {
            viewUpTo(r.tableChanges);
            words.clear();
            positions.clear();
            readingBefore = r.before;
            const bool ok = gatherList(g_rdram, r.from, r.to, words, frame.nativeRegs.empty() && chunks.empty() ? nullptr : &positions);
            if (logRanges)
            {
                std::fprintf(stderr, "[native render] range %08x..%08x before draw %u: %s, %zu words\n", r.from, r.to, r.before,
                             ok ? "read" : "NOT READ", words.size());
                // (TF_NATIVE_VIFLOG=<a>[-<b>]: the stretches before draws a to b; set alone, draw 0's.)
                static const char *vifEnv = std::getenv("TF_NATIVE_VIFLOG");
                static const uint32_t vifFrom = vifEnv ? static_cast<uint32_t>(std::strtoul(vifEnv, nullptr, 10)) : 0u;
                static const uint32_t vifTo = vifEnv && std::strchr(vifEnv, '-') ? static_cast<uint32_t>(std::strtoul(std::strchr(vifEnv, '-') + 1, nullptr, 10)) : vifFrom;
                if (vifEnv && ok && r.before >= vifFrom && r.before <= vifTo)
                    for (size_t i = 0; i < words.size();)
                    {
                        const uint32_t w = words[i];
                        const uint32_t cmd = (w >> 24) & 0x7Fu, num = (w >> 16) & 0xFFu, imm = w & 0xFFFFu;
                        size_t len = 1u;
                        if ((cmd & 0x60u) == 0x60u)
                        {
                            const uint32_t vn = (cmd >> 2) & 3u, vl = cmd & 3u, n = num ? num : 256u;
                            const uint32_t bits = (vn == 3u && vl == 3u) ? 16u : (32u >> vl) * (vn + 1u);
                            len += (static_cast<size_t>(n) * bits + 31u) / 32u;
                        }
                        else if (cmd == 0x20u)
                            len += 1u;
                        else if (cmd == 0x30u || cmd == 0x31u)
                            len += 4u;
                        else if (cmd == 0x4Au)
                            len += 2u * (num ? num : 256u);
                        else if (cmd == 0x50u || cmd == 0x51u)
                            len += 4u * (imm ? imm : 65536u);
                        if (cmd != 0u)
                        {
                            std::fprintf(stderr, "[native render]   vif %6zu: cmd %02x num %3u imm %04x", i, cmd, num, imm);
                            if ((cmd == 0x50u || cmd == 0x51u) && i + 4u < words.size())
                                std::fprintf(stderr, " giftag %08x %08x %08x %08x", words[i + 1], words[i + 2], words[i + 3], words[i + 4]);
                            if ((cmd & 0x60u) == 0x60u)
                                for (size_t j = 1; j < len && j <= 8u && i + j < words.size(); ++j)
                                    std::fprintf(stderr, " %08x", words[i + j]);
                            std::fprintf(stderr, "\n");
                        }
                        i += len;
                    }
                if (!ok || words.size() * 4u < (r.to - r.from) / 2u)
                {
                    std::vector<uint32_t> scratch;
                    vif::gather(g_rdram, r.from, scratch, stderr, {}, r.to);
                }
            }
            if (!ok)
                return;
            {
                size_t c1 = nc;
                const uint32_t ri = static_cast<uint32_t>(&r - frame.ranges.data());
                while (c1 < chunks.size() && chunks[c1].range <= ri)
                    ++c1;
                if (c1 > nc)
                {
                    if (ctxMode == 1)
                    {
                        splice(words, positions, nc, c1);
                        words.swap(merged);
                        positions.swap(mergedAt);
                    }
                    else if (ctxMode == 2)
                    {
                        rFrom = r.from, rTo = r.to, rBefore = r.before;
                        checkChunks(words, positions, nc, c1);
                    }
                    nc = c1;
                }
            }
            if (particles::enabled())
            {
                // (TODO 144: the list's GIF packets and the particle pipelines' own, in the list's order - nr_particles.cpp)
                auto toGif = [&](const uint32_t *w, size_t qwords) {
                    if (nw < frame.nativeRegs.size())
                        applyNative(listAt(w), readingBefore);
                    gif::run(g_gif, w, qwords, [&](const gif::Primitives &p) { addImmediate(g_rdram, p); });
                };
                // (TF_NATIVE_PARTICLEDRAWLOG=<n>: the first n draws made of the particle programs' packets, with their state)
                static int particleDrawLog = std::getenv("TF_NATIVE_PARTICLEDRAWLOG") ? std::atoi(std::getenv("TF_NATIVE_PARTICLEDRAWLOG")) : 0;
                static const uint64_t particleDrawFrom = [] {
                    const char *e = std::getenv("TF_NATIVE_PARTICLEDRAWLOG");
                    const char *comma = e ? std::strchr(e, ',') : nullptr;
                    return comma ? std::strtoull(comma + 1, nullptr, 10) : 0ull;
                }();
                auto fromParticles = [&](const uint32_t *w, size_t qwords) {
                    // (N5.3: the native functions' registers before them too - the state the pipelines' packets draw with)
                    if (nw < frame.nativeRegs.size() && t_kickWord)
                        applyNative(listAt(t_kickWord), readingBefore);
                    gif::run(g_gif, w, qwords, [&](const gif::Primitives &p) {
                        if (particleDrawLog > 0 && !p.vertices.empty() && frame.tick >= particleDrawFrom)
                        {
                            --particleDrawLog;
                            float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, z0 = 1e30f, z1 = -1e30f;
                            uint32_t aMax = 0u;
                            for (const Vertex &v : p.vertices)
                                x0 = std::min(x0, v.x), x1 = std::max(x1, v.x), y0 = std::min(y0, v.y), y1 = std::max(y1, v.y), z0 = std::min(z0, v.z), z1 = std::max(z1, v.z), aMax = std::max(aMax, v.colour >> 24);
                            std::fprintf(stderr, "[native render] particle draw (tick %llu): prim %x, %zu verts, xy %g..%g %g..%g z %g..%g, frame %016llx zbuf %016llx test %llx alpha %llx tex0 %016llx xyoffset %016llx scissor %016llx colour %08x alpha max %u\n",
                                         static_cast<unsigned long long>(frame.tick), p.prim, p.vertices.size(), x0, x1, y0, y1, z0, z1,
                                         static_cast<unsigned long long>(p.frame), static_cast<unsigned long long>(p.zbuf), static_cast<unsigned long long>(p.test),
                                         static_cast<unsigned long long>(p.alpha), static_cast<unsigned long long>(p.tex0), static_cast<unsigned long long>(p.xyoffset),
                                         static_cast<unsigned long long>(p.scissor), p.vertices[0].colour, aMax);
                        }
                        addImmediate(g_rdram, p);
                    });
                    if (noListMode() != 0) // (N5.1: their register writes are the renderer's own - in the call state too)
                        gif::run(g_callGif, w, qwords, [](const gif::Primitives &) {});
                };
                replayListRunning(words.data(), words.size(), toGif, fromParticles);
                gif::flush(g_gif, [&](const gif::Primitives &p) { addImmediate(g_rdram, p); });
                return;
            }
            vif::skim(words.data(), words.size(), [&](const uint32_t *w, size_t qwords) {
                if (nw < frame.nativeRegs.size())
                    applyNative(listAt(w), readingBefore);
                gif::run(g_gif, w, qwords, [&](const gif::Primitives &p) { addImmediate(g_rdram, p); });
            });
            gif::flush(g_gif, [&](const gif::Primitives &p) { addImmediate(g_rdram, p); });
            replayList(words.data(), words.size());
        };
        size_t r = 0;
        uint64_t writesBefore = 0; // (the list's register writes up to the draw before: TODO 61)
        // (N5.1) The registers as the device's calls wrote them (Frame::regStream), applied where they come among the
        // draws: read by the draws' state below instead of the list's (TF_N5_NOLIST), or beside it, compared (=check).
        const int noList = noListMode();
        size_t sw = 0;
        uint64_t callWritesBefore = 0;
        auto applyStream = [&](size_t upTo) {
            while (sw < frame.regStream.size() && frame.regStream[sw].before <= upTo)
            {
                const Frame::RegWrite &w = frame.regStream[sw++];
                const uint32_t packet[8] = {0x00008001u, 0x10000000u, 0x0000000Eu, 0u, static_cast<uint32_t>(w.value),
                                            static_cast<uint32_t>(w.value >> 32), w.reg, 0u}; // (a tag: NLOOP 1, EOP, one A+D)
                gif::run(g_callGif, packet, 2u, [](const gif::Primitives &) {});
            }
        };
        // (Debug, =check: TF_N5_NOLIST_LOG=<tick> - in the first frame from that VBlank, each of the first 80 draws' register
        // writes, the list's and the calls', side by side - the vertex registers left out.)
        static const uint64_t noListLogTick = std::getenv("TF_N5_NOLIST_LOG") ? std::strtoull(std::getenv("TF_N5_NOLIST_LOG"), nullptr, 10) : 0u;
        static bool noListLogged = false;
        const bool logWrites = noList == 2 && noListLogTick && !noListLogged && frame.tick >= noListLogTick;
        static thread_local std::string listWrites;
        if (logWrites)
        {
            noListLogged = true;
            std::fprintf(stderr, "[n5:nolist] tick %llu: %zu draws, %zu register writes from the calls, %zu list stretches\n",
                         static_cast<unsigned long long>(frame.tick), frame.draws.size(), frame.regStream.size(), frame.ranges.size());
            g_gif.onWrite = [](uint32_t reg, uint64_t value) {
                if (reg > 0x05u && reg != 0x0Cu && reg != 0x0Du && listWrites.size() < 4000u)
                {
                    char e[40];
                    std::snprintf(e, sizeof(e), " %02x=%llx", reg, static_cast<unsigned long long>(value));
                    listWrites += e;
                }
            };
        }
        for (size_t k = 0; k <= frame.draws.size(); ++k)
        {
            writesBefore = g_gif.writes;
            callWritesBefore = g_callGif.writes;
            const size_t swBefore = sw;
            while (r < frame.ranges.size() && frame.ranges[r].before <= k)
                read(frame.ranges[r++]);
            applyNative(~0u, static_cast<uint32_t>(k));
            if (noList != 0)
                applyStream(k);
            if (logWrites && k < 80u)
            {
                std::string calls;
                for (size_t i = swBefore; i < sw; ++i)
                {
                    char e[40];
                    std::snprintf(e, sizeof(e), " %02x=%llx", frame.regStream[i].reg, static_cast<unsigned long long>(frame.regStream[i].value));
                    calls += e;
                }
                std::fprintf(stderr, "[n5:nolist] tick %llu before draw %zu: list%s | calls%s\n", static_cast<unsigned long long>(frame.tick), k,
                             listWrites.c_str(), calls.c_str());
                listWrites.clear();
            }
            if (logWrites && k == 80u)
                g_gif.onWrite = nullptr;
            const gif::State &regs = noList == 1 ? g_callGif : g_gif; // (where the draws' GS state below comes from)
            const uint64_t regsWritesBefore = noList == 1 ? callWritesBefore : writesBefore;
            // (N5 step 2 part 2: a chunk whose stretch came to nothing - the list not grown since - before its draw's state)
            if (nc < chunks.size() && (k == frame.draws.size() || frame.draws[k].needsShader))
                while (nc < chunks.size() && chunks[nc].before <= k)
                {
                    if (ctxMode == 1)
                    {
                        replayList(chunks[nc].w, chunks[nc].count);
                        ++contextsCheck().alone;
                    }
                    ++nc;
                }
            if (k == frame.draws.size())
            {
                if (ctxMode == 1) // (the rest: the VU image goes on into the next frame)
                    for (; nc < chunks.size(); ++nc)
                        replayList(chunks[nc].w, chunks[nc].count);
                contextsCheckReport();
                break;
            }
            Draw &d = frame.draws[k];
            if (logRanges && k < 120u)
                std::fprintf(stderr, "[native render] draw %zu: target %08x origin %g,%g scissor %g..%g x %g..%g | list ctx1 frame %016llx xyoffset %016llx scissor %016llx\n",
                             k, d.target, d.originX, d.originY, d.scissor[0], d.scissor[2], d.scissor[1], d.scissor[3],
                             static_cast<unsigned long long>(g_gif.frame[0]), static_cast<unsigned long long>(g_gif.xyoffset[0]),
                             static_cast<unsigned long long>(g_gif.scissor[0]));
            if (d.needsShader)
            {
                d.needsShader = false;
                if (noList == 2)
                    checkCallRegisters(frame.tick, k, writesBefore, callWritesBefore);
                snapshotShader(d.shader);
                // (TODO 133) The replayed field A matrix against the one the capture saw sent for the set (func_00221DB0):
                // where they differ the sent one is taken. Amazon's river reflects by mirrored copies of the ground and
                // rocks (sets whose own vertices are the ground's mirrored), drawn with the ordinary camera - below the
                // ground, seen only through the water. Their packets' camera did not reach the replay, which kept a
                // mirrored transform from before: the copies were drawn mirrored into the sky - the ground upside down
                // as a brown ceiling, the real ground under it hidden (black), at the places along the river where
                // they are drawn (676 of ~1400 draws a frame at the user's spot; none in a walk elsewhere). The GS's
                // picture of the same frame: the ordinary sky and ground, as now. TF_NATIVE_MATRIXPICK=replay: the
                // replayed one, as before. (Checking aid: TF_NATIVE_MATRIXCHECK=<n> - the first n such draws, and
                // per frame how many.)
                static int matrixChecks = std::getenv("TF_NATIVE_MATRIXCHECK") ? std::atoi(std::getenv("TF_NATIVE_MATRIXCHECK")) : 0;
                float sent[16];
                std::memcpy(sent, d.matrix, sizeof(sent));
                fieldAMatrix(d.matrix); // (when one was seen: else the one the capture sent)
                static const bool pickSent = !(std::getenv("TF_NATIVE_MATRIXPICK") && std::string(std::getenv("TF_NATIVE_MATRIXPICK")) == "replay");
                static int frameSummaries = std::getenv("TF_NATIVE_MATRIXCHECK") ? 400 : 0;
                if (matrixChecks > 0 || frameSummaries > 0 || pickSent)
                {
                    float diff = 0.0f, size = 0.0f;
                    for (int m = 0; m < 16; ++m)
                        diff = std::max(diff, std::fabs(d.matrix[m] - sent[m])), size = std::max(size, std::fabs(sent[m]));
                    static uint64_t sumTick = 0;
                    static int sumCount = 0, sumDraws = 0;
                    if (frame.tick != sumTick)
                    {
                        if (sumTick && frameSummaries > 0 && sumCount > 0)
                        {
                            --frameSummaries;
                            std::fprintf(stderr, "[native render] matrix check: tick %llu, %d of %d 3D draws with another matrix than the sent one\n",
                                         static_cast<unsigned long long>(sumTick), sumCount, sumDraws);
                        }
                        sumTick = frame.tick, sumCount = 0, sumDraws = 0;
                    }
                    ++sumDraws;
                    const bool differs = diff > 0.02f * (size + 1.0f);
                    sumCount += differs ? 1 : 0;
                    if (differs && pickSent)
                        std::memcpy(d.matrix, sent, sizeof(sent));
                    if (differs && matrixChecks > 0)
                    {
                        --matrixChecks;
                        std::fprintf(stderr, "[native render] matrix check: tick %llu draw %zu target %08x mesh type %u (%zu vertices) routine %04x: sent "
                                     "%g %g %g %g / %g %g %g %g / %g %g %g %g / %g %g %g %g, replayed %g %g %g %g / %g %g %g %g / %g %g %g %g / %g %g %g %g\n",
                                     static_cast<unsigned long long>(frame.tick), k, d.target, d.mesh ? d.mesh->type : 0u,
                                     d.mesh ? d.mesh->vertices.size() : 0u, d.shader.count ? d.shader.address[0] : 0u, sent[0], sent[1], sent[2],
                                     sent[3], sent[4], sent[5], sent[6], sent[7], sent[8], sent[9], sent[10], sent[11], sent[12], sent[13], sent[14],
                                     sent[15], d.matrix[0], d.matrix[1], d.matrix[2], d.matrix[3], d.matrix[4], d.matrix[5], d.matrix[6], d.matrix[7],
                                     d.matrix[8], d.matrix[9], d.matrix[10], d.matrix[11], d.matrix[12], d.matrix[13], d.matrix[14], d.matrix[15]);
                    }
                }
                d.tex1 = regs.tex1[0]; // (TEX1 as the list set it where the draw comes: SetBilinear, the trees' mip bias)
                d.tex12 = regs.tex1[1];
                if (d.fromList)
                    applyListState(g_rdram, d, regs);
                // The dash's after-image (0x2220, TODO 69): the game writes TEX0_1, ALPHA_1 (0x44, the
                // usual blend) and TEST_1 (0x7001b) into the list itself (VIF DIRECT, after the set's state
                // block) - TEX0_1 becoming the lit material's context-2 texture, the character's own, where
                // the block's TEX0_1 is the reflection map (drawn as dark chrome). The GS state as the list
                // stands here; the texture is the block's context 1 or 2 one that matches.
                // TF_NATIVE_SMEARSTATE=0: the block's, as before.
                static const bool smearStateOff = std::getenv("TF_NATIVE_SMEARSTATE") && *std::getenv("TF_NATIVE_SMEARSTATE") == '0';
                if (!smearStateOff && d.shader.blockOf(Routine::Smear) && regs.tex0[0])
                {
                    const uint64_t tex0 = regs.tex0[0];
                    if ((tex0 & kTex0SampleMask) == (d.tex02 & kTex0SampleMask) && d.texture2 && (tex0 & kTex0SampleMask) != (d.tex0 & kTex0SampleMask))
                    {
                        d.texture = d.texture2;
                        d.topPalette = nullptr;
                        d.tex0 = tex0;
                        d.tex1 = d.tex12;
                    }
                    d.alpha = regs.alpha[0];
                    d.test = regs.test[0];
                }
                else if (!d.fromList)
                    listOverrides(d, regsWritesBefore, regs);
                t_logPrimitives = logRanges;
                finishDrawImpl(d);
                t_logPrimitives = false;
                if (!d.mesh)
                    continue; // (a microcode primitive this renderer does not draw)
            }
            out.push_back(std::move(d));
        }
        while (r < frame.ranges.size())
            read(frame.ranges[r++]);
        applyNative(~0u, ~0u);
        frame.nativeRegs.clear();
        if (logWrites)
            g_gif.onWrite = nullptr;
        if (noList != 0)
            applyStream(~size_t(0)); // (the frame's last writes: the state the next frame begins with)
        frame.regStream.clear();
        if (!frame.looseGif.empty())
        {
            // (N3b: a frame of GS packets the game sent outside its frames - looseGif. Its own GS state, kept between
            // such frames; the textures from the tables as they stand.)
            static gif::State looseState;
            // (The buffer the game began a picture in last - an opaque sprite over its whole screen, the warp tunnel's
            // black - and the one it began in before that: the GS shows that one, whole, while the other is drawn.)
            static uint32_t building = 0xFFFFFFFFu, complete = 0xFFFFFFFFu;
            static uint64_t lastLooseTick = 0u;
            static bool merging = false;
            if (frame.tick > lastLooseTick + 8u) // (a new stretch of them: nothing known yet)
                building = complete = 0xFFFFFFFFu;
            lastLooseTick = frame.tick;
            t_useView = false;
            t_looseTick = frame.tick ? frame.tick : 1u;
            // (TF_NATIVE_LOOSELOG=<n>: the first n loose frames' primitives - their TEX0, FRAME, ALPHA, vertices)
            static int looseLog = std::getenv("TF_NATIVE_LOOSELOG") ? std::atoi(std::getenv("TF_NATIVE_LOOSELOG")) : 0;
            // (TF_NATIVE_LOOSELOG_AT=<VBlank>: the four loose frames begun at or after it too)
            static uint64_t looseLogAt = std::getenv("TF_NATIVE_LOOSELOG_AT") ? std::strtoull(std::getenv("TF_NATIVE_LOOSELOG_AT"), nullptr, 10) : 0u;
            static int looseLogAtLeft = 4;
            const bool logAt = looseLogAt && frame.tick >= looseLogAt;
            if (logAt && --looseLogAtLeft <= 0)
                looseLogAt = 0u;
            const bool logThis = looseLog > 0 || logAt;
            if (looseLog > 0)
                --looseLog;
            int logged = 0;
            const auto add = [&](const gif::Primitives &p) {
                if (logThis && logged++ < 40)
                {
                    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
                    float z0 = 1e30f, z1 = -1e30f, q0 = 1e30f, q1 = -1e30f;
                    for (const Vertex &v : p.vertices)
                        x0 = std::min(x0, v.x), y0 = std::min(y0, v.y), x1 = std::max(x1, v.x), y1 = std::max(y1, v.y),
                        z0 = std::min(z0, v.z), z1 = std::max(z1, v.z), q0 = std::min(q0, v.size), q1 = std::max(q1, v.size);
                    std::fprintf(stderr, "[native render] loose z %.0f..%.0f q %g..%g zbuf %016llx colclamp %u clamp %016llx tex1 %016llx\n", z0, z1, q0, q1,
                                 static_cast<unsigned long long>(p.zbuf), p.colclamp, static_cast<unsigned long long>(p.clamp),
                                 static_cast<unsigned long long>(p.tex1));
                    if (p.vertices.size() <= 6u)
                        for (const Vertex &v : p.vertices)
                            std::fprintf(stderr, "[native render]   v %.1f %.1f uv %g %g q %g c %08x\n", v.x, v.y, v.u, v.v, v.size, v.colour);
                    std::fprintf(stderr, "[native render] loose %llu: prim %03x tex0 %016llx frame %016llx alpha %010llx test %llx %zu vertices x %.0f..%.0f y %.0f..%.0f colour %08x\n",
                                 static_cast<unsigned long long>(frame.tick), p.prim, static_cast<unsigned long long>(p.tex0),
                                 static_cast<unsigned long long>(p.frame), static_cast<unsigned long long>(p.alpha),
                                 static_cast<unsigned long long>(p.test), p.vertices.size(), x0, x1, y0, y1,
                                 p.vertices.empty() ? 0u : p.vertices[0].colour);
                }
                // (What the GS takes in its stride, the GPU side would clip: the warp tunnel's VU1 output has Z 2^31
                // (far past its 24-bit depth buffer, drawn with the depth test off) and all-negative Q (S/Q, T/Q the
                // same). Z is clamped to the buffer's format, a batch of negative Q flipped - S, T and Q times -1.)
                // (Debug: TF_NATIVE_LOOSE_SKIP=<tbp hex>[,...]: the loose primitives sampling those texture bases left out)
                static const std::vector<uint32_t> skipTbp = [] {
                    std::vector<uint32_t> v;
                    if (const char *e = std::getenv("TF_NATIVE_LOOSE_SKIP"))
                        for (const char *s = e; *s;)
                        {
                            char *end = nullptr;
                            const unsigned long t = std::strtoul(s, &end, 16);
                            if (end == s)
                                break;
                            v.push_back(static_cast<uint32_t>(t));
                            s = *end == ',' ? end + 1 : end;
                        }
                    return v;
                }();
                // (Debug: TF_NATIVE_LOOSE_VDUMP=<VBlank>: the loose frame begun at it, every primitive's vertices, to
                // runs/loose_vdump_<VBlank>.txt - prim, tex0, then x y z u v q colour a line)
                static const uint64_t vdumpAt = std::getenv("TF_NATIVE_LOOSE_VDUMP") ? std::strtoull(std::getenv("TF_NATIVE_LOOSE_VDUMP"), nullptr, 10) : 0u;
                const uint64_t vdumpTick = vdumpAt ? vdumpAt : g_vdumpNthTick.load(std::memory_order_relaxed);
                if (vdumpTick && frame.tick == vdumpTick)
                {
                    char path[64];
                    std::snprintf(path, sizeof(path), "runs/loose_vdump_%llu.txt", static_cast<unsigned long long>(vdumpTick));
                    static FILE *vf = std::fopen(path, "w");
                    if (vf)
                    {
                        std::fprintf(vf, "prim %03x tex0 %016llx alpha %010llx %zu\n", p.prim, static_cast<unsigned long long>(p.tex0),
                                     static_cast<unsigned long long>(p.alpha), p.vertices.size());
                        for (const Vertex &v : p.vertices)
                            std::fprintf(vf, "%.3f %.3f %.0f %g %g %g %08x\n", v.x, v.y, v.z, v.u, v.v, v.size, v.colour);
                        std::fflush(vf);
                    }
                }
                static const uint32_t skipPrim = std::getenv("TF_NATIVE_LOOSE_SKIPPRIM") ? static_cast<uint32_t>(std::strtoul(std::getenv("TF_NATIVE_LOOSE_SKIPPRIM"), nullptr, 16)) : 0xFFFFu;
                if (p.prim == skipPrim)
                    return;
                if (!skipTbp.empty() && (p.prim & 0x10u) &&
                    std::find(skipTbp.begin(), skipTbp.end(), static_cast<uint32_t>(p.tex0) & 0x3FFFu) != skipTbp.end())
                    return;
                // (The primitives are the GIF state's own, reset once handed over: fixed up in place, their vertices taken.)
                gif::Primitives &q = const_cast<gif::Primitives &>(p);
                // (Debug: TF_NATIVE_LOOSE_NOTEX=<tbp hex>: those primitives drawn untextured - their coverage alone)
                static const uint32_t noTex = std::getenv("TF_NATIVE_LOOSE_NOTEX") ? static_cast<uint32_t>(std::strtoul(std::getenv("TF_NATIVE_LOOSE_NOTEX"), nullptr, 16)) : 0u;
                if (noTex && (q.prim & 0x10u) && (static_cast<uint32_t>(q.tex0) & 0x3FFFu) == noTex)
                    q.prim &= ~0x10u;
                const uint32_t zpsm = static_cast<uint32_t>(p.zbuf >> 24) & 0xFu;
                const float zMax = zpsm == 0u ? 4294967295.0f : zpsm == 1u ? 16777215.0f : 65535.0f;
                bool allNegative = !q.vertices.empty() && !(p.prim & 0x100u);
                for (const Vertex &v : q.vertices)
                    allNegative = allNegative && v.size < 0.0f;
                for (Vertex &v : q.vertices)
                {
                    v.z = std::min(v.z, zMax);
                    if (allNegative)
                        v.u = -v.u, v.v = -v.v, v.size = -v.size;
                }
                if (!merging && (q.prim & 7u) == 6u && !(q.prim & 0x40u) && q.vertices.size() >= 6u)
                {
                    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
                    for (const Vertex &v : q.vertices)
                        x0 = std::min(x0, v.x), y0 = std::min(y0, v.y), x1 = std::max(x1, v.x), y1 = std::max(y1, v.y);
                    const float sw = static_cast<float>(((q.scissor >> 16) & 0x7FFu) + 1u - (q.scissor & 0x7FFu));
                    const float sh = static_cast<float>(((q.scissor >> 48) & 0x7FFu) + 1u - ((q.scissor >> 32) & 0x7FFu));
                    const uint32_t fbp = static_cast<uint32_t>(q.frame) & 0x1FFu;
                    if (sw > 64.0f && sh > 64.0f && (x1 - x0) >= 0.9f * sw && (y1 - y0) >= 0.9f * sh && fbp != building)
                        complete = building, building = fbp;
                }
                addImmediate(g_rdram, q, &q.vertices);
                if (logThis && logged <= 40 && t_out && !t_out->empty())
                {
                    const Draw &d = t_out->back();
                    std::fprintf(stderr, "[native render]   -> draw target %08x origin %.0f,%.0f size %.0fx%.0f texture %s %ux%u fog %d\n", d.target, d.originX, d.originY,
                                 d.width, d.height, d.texture ? "yes" : "no", d.texture ? d.texture->width : 0u, d.texture ? d.texture->height : 0u, d.fogPalette ? 1 : 0);
                }
            };
            gif::run(looseState, frame.looseGif.data(), frame.looseGif.size() / 4u, add);
            gif::flush(looseState, add);
            // The display's two circuits merged (the warp tunnel: PMODE 0x8007 - both on, MMOD 0: circuit 1 blended over
            // circuit 2 by its own pixels' alpha, x2 to 1.0). The picture is circuit 1's buffer; circuit 2's is laid under
            // it by one sprite drawn into it - C1 * a + C2 * (1 - a), the GS's ALPHA (Cd - Cs) * Ad + Cs with C2 as the
            // texture - in a GS state of its own (the frames' kept state untouched). TF_NATIVE_LOOSEMERGE=0: not merged.
            static const bool merge = !(std::getenv("TF_NATIVE_LOOSEMERGE") && *std::getenv("TF_NATIVE_LOOSEMERGE") == '0');
            const uint32_t fbp1 = static_cast<uint32_t>(frame.dispfb[0]) & 0x1FFu, fbw1 = static_cast<uint32_t>(frame.dispfb[0] >> 9) & 0x3Fu;
            // (Circuit 2's buffer: the one the game began its last whole picture in before the one it draws now, where known -
            // the display registers flip ahead of the drawing, and the tunnel's cloud for a picture comes after the flip;
            // TF_NATIVE_LOOSE_SHOWN=display: the display's, as it stands.)
            static const bool byDisplay = std::getenv("TF_NATIVE_LOOSE_SHOWN") && std::string(std::getenv("TF_NATIVE_LOOSE_SHOWN")) == "display";
            const uint32_t fbpShown = static_cast<uint32_t>(frame.dispfb[1]) & 0x1FFu;
            const uint32_t fbp2 = !byDisplay && complete != 0xFFFFFFFFu && complete != (static_cast<uint32_t>(frame.dispfb[0]) & 0x1FFu) ? complete : fbpShown,
                           fbw2 = static_cast<uint32_t>(frame.dispfb[1] >> 9) & 0x3Fu;
            if (std::getenv("TF_NATIVE_LOOSE_SHOWNLOG"))
            {
                // (Debug: each loose frame's draws by target - vertices - and the buffer shown)
                std::string per;
                uint32_t keys[4] = {}, n = 0u;
                size_t counts[4] = {};
                for (const Draw &d : out)
                {
                    const uint32_t k = d.target & 0x1FFu;
                    uint32_t i = 0;
                    while (i < n && keys[i] != k)
                        ++i;
                    if (i == n && n < 4u)
                        keys[n++] = k;
                    if (i < 4u)
                        counts[i] += d.mesh ? d.mesh->vertices.size() : 0u;
                }
                for (uint32_t i = 0; i < n; ++i)
                    per += " " + std::to_string(keys[i]) + ":" + std::to_string(counts[i]);
                std::fprintf(stderr, "[native render] shown %llu: building %u complete %u display %u ->%u;%s\n", static_cast<unsigned long long>(frame.tick),
                             building, complete, fbpShown, fbp2, per.c_str());
            }
            const uint32_t psm1 = static_cast<uint32_t>(frame.dispfb[0] >> 15) & 0x1Fu, psm2 = static_cast<uint32_t>(frame.dispfb[1] >> 15) & 0x1Fu;
            if (merge && (frame.pmode & 3u) == 3u && !(frame.pmode & 0x20u) && !(frame.pmode & 0x80u) && fbp1 != fbp2 && fbw1 && fbw2 &&
                psm1 == 0u && psm2 == 0u && !(frame.dispfb[1] >> 32) && !(frame.dispfb[0] >> 32))
            {
                // (its lines: what the frame's draws into either buffer covered. Circuit 1's buffer not drawn into at all -
                // the tunnel's: a buffer cleared once, black and clear - circuit 2 is the picture whole, copied unblended:
                // the native picture starts each frame cleared, opaque.)
                uint32_t lines = 0u;
                bool c1Drawn = false;
                for (const Draw &d : out)
                {
                    const uint32_t fbp = d.target & 0x1FFu;
                    c1Drawn = c1Drawn || fbp == fbp1;
                    if (fbp == fbp1 || fbp == fbp2)
                        lines = std::max(lines, static_cast<uint32_t>(d.scissor[3]) + 1u);
                }
                if (lines == 0u)
                    lines = 224u;
                const uint32_t w = fbw1 * 64u;
                uint32_t tw = 0u, th = 0u;
                while ((1u << tw) < fbw2 * 64u && tw < 10u)
                    ++tw;
                while ((1u << th) < lines && th < 10u)
                    ++th;
                const uint64_t ofx = 0x6000u, ofy = 0x6000u;
                const std::pair<uint32_t, uint64_t> regs[] = {
                    {0x4Cu, fbp1 | static_cast<uint64_t>(fbw1) << 16 | 0xFF000000ull << 32},  // FRAME_1: circuit 1's, its alpha kept
                    {0x4Eu, 1ull << 32},                                                       // ZBUF_1: no depth written
                    {0x18u, ofx | ofy << 32},                                                  // XYOFFSET_1
                    {0x40u, static_cast<uint64_t>(w - 1u) << 16 | static_cast<uint64_t>(lines - 1u) << 48}, // SCISSOR_1
                    {0x47u, 0x30000u},                                                         // TEST_1: depth always passes
                    {0x42u, 0x11u},                                                            // ALPHA_1: (Cd - Cs) * Ad + Cs
                    {0x08u, 0x5u},                                                             // CLAMP_1: clamped
                    {0x14u, 0u},                                                               // TEX1_1: nearest
                    {0x06u, static_cast<uint64_t>(fbp2) * 32u | static_cast<uint64_t>(fbw2) << 14 | static_cast<uint64_t>(tw) << 26 |
                                static_cast<uint64_t>(th) << 30 | 1ull << 34 | 1ull << 35}, // TEX0_1: circuit 2's buffer, RGBA, DECAL
                    {0x00u, 0x6u | 0x10u | (c1Drawn ? 0x40u : 0u) | 0x100u},                  // PRIM: a sprite, textured, (blended,) UV
                    {0x01u, 0x80808080u},                                                      // RGBAQ
                    {0x03u, 0u},                                                               // UV 0, 0
                    {0x05u, ofx | ofy << 16},                                                  // XYZ2 0, 0
                    {0x03u, static_cast<uint64_t>(w * 16u) | static_cast<uint64_t>(lines * 16u) << 16}, // UV w, lines
                    {0x05u, (ofx + w * 16u) | (ofy + lines * 16u) << 16},                      // XYZ2 w, lines
                };
                const size_t n = sizeof(regs) / sizeof(regs[0]);
                std::vector<uint32_t> words;
                words.reserve(4u * (n + 1u));
                const uint64_t tag = static_cast<uint64_t>(n) | 1ull << 15 | 1ull << 60; // NLOOP n, EOP, PACKED, NREG 1
                words.insert(words.end(), {static_cast<uint32_t>(tag), static_cast<uint32_t>(tag >> 32), 0xEu, 0u}); // REGS: A+D
                for (const auto &r : regs)
                    words.insert(words.end(), {static_cast<uint32_t>(r.second), static_cast<uint32_t>(r.second >> 32), r.first, 0u});
                gif::State mergeState;
                const size_t before = out.size();
                merging = true;
                gif::run(mergeState, words.data(), words.size() / 4u, add);
                gif::flush(mergeState, add);
                merging = false;
                for (size_t i = before; i < out.size(); ++i)
                    out[i].texture = nullptr; // (circuit 2's buffer read as the target it is, never a texture from the tables)
            }
            t_looseTick = 0u;
        }
        t_out = nullptr;
        t_useView = false;
        frame.draws = std::move(out);
        frame.ranges.clear();
        waitFrameTextures(frame); // (TODO 164: the textures its workers convert)
        if (frame.firstAfterLoad)
            frame.draws.erase(std::remove_if(frame.draws.begin(), frame.draws.end(),
                                             [](const Draw &d) { return d.mesh && !d.mesh->immediate && !d.mesh->primitive; }),
                              frame.draws.end());
        frame.has3D = false;
        for (const Draw &d : frame.draws)
            if (d.mesh && !d.mesh->immediate && !d.mesh->primitive)
            {
                frame.has3D = true;
                break;
            }
    }

    void setFrameTexturePolicy(bool (*policy)(const uint8_t *, size_t))
    {
        g_frameTexturePolicy = policy;
        resetFrameTexturePolicySnapshot(g_frame);
    }

    void resetInterframeMicrocodeCapture()
    {
        g_interframeCapture.reset();g_frame.interframeMicrocode={};
    }
    void setInterframeMicrocodePolicy(bool (*policy)(const uint8_t *, size_t))
    {
        g_interframePolicy=policy;resetInterframeMicrocodeCapture();
    }
    void resetFrameTexturePolicy()
    {
        resetFrameTexturePolicySnapshot(g_frame);
        resetInterframeMicrocodeCapture();
    }

    void install(PS2Runtime &runtime)
    {
        // The settings panel's choice (TF_RENDERER, settings.ini; takes a restart): the native renderer
        // shown in the window and drawing alone, the PS2 path's GS drawing switched off for its scenes.
        // Variables already set win (TF_NATIVE_RENDER and the rest, for tests).
        {
            ps2_settings::Setting s;
            s.key = "TF_RENDERER";
            s.section = "Display";
            s.label = "Renderer";
            s.order = 1;
            s.help = "Native (DX12, preview): the game drawn by the PC renderer (Direct3D 12) - more frames, sharper "
                     "pictures; menus and movies too, the PS2 graphics chip left with nothing to draw. Emulated: the PS2's "
                     "graphics chip emulated, as before. Takes a restart.";
            s.choices = {{"emulated", "Emulated (PS2)"}, {"native", "Native (DX12, preview)"}};
            s.defaultValue = "native"; // (TODO 163: the port's renderer; main.cpp sets TF_RENDERER=native when nothing chose)
            s.apply = [](const std::string &) { return false; }; // (takes a restart)
            ps2_settings::add(s);
            const char *chosen = std::getenv("TF_RENDERER");
            if (chosen && std::string(chosen) == "native" && !std::getenv("TF_NATIVE_RENDER"))
            {
                const char *vars[][2] = {{"TF_NATIVE_RENDER", "1"}, {"TF_NATIVE_LIVE", "1"}, {"TF_NATIVE_ONLY", "1"}};
                for (const auto &v : vars)
                    if (!std::getenv(v[0]))
                    {
#if defined(_WIN32)
                        _putenv_s(v[0], v[1]);
#else
                        setenv(v[0], v[1], 1);
#endif
                    }
                std::fprintf(stderr, "[native render] the settings choose the native renderer\n");
            }
        }
        // Its anti-aliasing (TF_NATIVE_AA, live): FXAA over the 3D before the HUD and menus are drawn.
        {
            ps2_settings::Setting s;
            s.key = "TF_NATIVE_AA";
            s.section = "Graphics";
            s.label = "Smooth edges (native renderer)";
            s.order = 55;
            s.help = "Native renderer: softens the steps along the edges of the 3D scene, the grass and leaves too. "
                     "FXAA: smooth and cheap. SMAA 1x: keeps textures and fine detail sharper, costs a little more. "
                     "The HUD, menus and text are drawn after it and stay sharp.";
            s.choices = {{"0", "Off"}, {"1", "FXAA"}, {"2", "SMAA 1x"}};
            s.defaultValue = "1";
            s.apply = [](const std::string &value) {
                d3d::setAntiAliasing(value == "0" ? 0 : value == "2" ? 2 : 1);
                return true;
            };
            s.available = []() {
                const char *on = std::getenv("TF_NATIVE_RENDER");
                return ps2_settings::choice("TF_RENDERER") == "native" || (on && *on && *on != '0');
            };
            s.unavailableHelp = "Only for the native renderer (Display: Renderer).";
            ps2_settings::add(s);
        }
        // (TODO 108) Its modern bloom (TF_NATIVE_BLOOM, live, off by default - the game's own glow is the faithful look).
        {
            ps2_settings::Setting s;
            s.key = "TF_NATIVE_BLOOM";
            s.section = "Graphics";
            s.label = "Glow (native renderer)";
            s.order = 57;
            s.help = "Native renderer: the glow around bright parts of the picture. Original: the game's own glow, worked out "
                     "in a small 128 x 128 picture as on the PS2. Modern bloom: a soft glow made at the screen's full resolution, "
                     "with the same threshold, strength and shape the game sets for each place.";
            s.choices = {{"0", "Original"}, {"1", "Modern bloom"}};
            s.defaultValue = "0";
            s.apply = [](const std::string &value) {
                frame::setBloom(value == "1" ? 1 : 0);
                return true;
            };
            s.available = []() {
                const char *on = std::getenv("TF_NATIVE_RENDER");
                return ps2_settings::choice("TF_RENDERER") == "native" || (on && *on && *on != '0');
            };
            s.unavailableHelp = "Only for the native renderer (Display: Renderer).";
            ps2_settings::add(s);
        }
        // (TODO 108) The modern bloom's strength (the F1 slider; 100% the game's glow matched).
        {
            ps2_settings::Setting s;
            s.key = "TF_NATIVE_BLOOM_STRENGTH";
            s.section = "Graphics";
            s.label = "Bloom strength (native renderer)";
            s.order = 57;
            s.help = "Native renderer, the modern bloom: how strong and wide the glow is. 100% matches the game's own glow; above, "
                     "the bright parts glow brighter and wider; below, fainter.";
            s.choices = {{"0", "0%"}, {"0.25", "25%"}, {"0.5", "50%"}, {"0.75", "75%"}, {"1", "100%"}, {"1.25", "125%"}, {"1.5", "150%"}, {"1.75", "175%"}, {"2", "200%"}};
            s.defaultValue = "1.5";
            s.apply = [](const std::string &value) {
                frame::setBloomStrength(static_cast<float>(std::atof(value.c_str())));
                return true;
            };
            s.available = []() { return ps2_settings::choice("TF_NATIVE_BLOOM") == "1"; };
            s.unavailableHelp = "Turn it on first (the row above).";
            ps2_settings::add(s);
        }
        // (TODO 108 (2)) Its tone mapping (TF_NATIVE_TONEMAP, live, off by default - the picture as the game draws it).
        {
            ps2_settings::Setting s;
            s.key = "TF_NATIVE_TONEMAP";
            s.section = "Graphics";
            s.label = "Tone mapping (native renderer)";
            s.order = 58;
            s.help = "Native renderer: Filmic gives the 3D scene a film-like tone curve - highlights roll off softly instead "
                     "of clipping, mid-tones keep their brightness, shadows get a little deeper. Off: the colours as the game "
                     "draws them. The HUD and menus are not changed.";
            s.choices = {{"0", "Off"}, {"1", "Filmic"}};
            s.defaultValue = "0";
            s.apply = [](const std::string &value) {
                frame::setTone(value == "1" ? 1 : 0);
                return true;
            };
            s.available = []() {
                const char *on = std::getenv("TF_NATIVE_RENDER");
                return ps2_settings::choice("TF_RENDERER") == "native" || (on && *on && *on != '0');
            };
            s.unavailableHelp = "Only for the native renderer (Display: Renderer).";
            ps2_settings::add(s);
        }
        // (TODO 108) The tone mapping's strength (the F1 slider).
        {
            ps2_settings::Setting s;
            s.key = "TF_NATIVE_TONE_STRENGTH";
            s.section = "Graphics";
            s.label = "Tone mapping strength (native renderer)";
            s.order = 58;
            s.help = "Native renderer, the filmic tone mapping: how strong the curve is - contrast, richer highlights and colour, "
                     "deeper shadows. 0% is the picture as the game draws it.";
            s.choices = {{"0", "0%"}, {"0.1", "10%"}, {"0.2", "20%"}, {"0.3", "30%"}, {"0.4", "40%"}, {"0.5", "50%"}, {"0.6", "60%"}, {"0.7", "70%"}, {"0.8", "80%"}, {"0.9", "90%"}, {"1", "100%"}};
            s.defaultValue = "0.6";
            s.apply = [](const std::string &value) {
                frame::setToneStrength(static_cast<float>(std::atof(value.c_str())));
                return true;
            };
            s.available = []() { return ps2_settings::choice("TF_NATIVE_TONEMAP") == "1"; };
            s.unavailableHelp = "Turn it on first (the row above).";
            ps2_settings::add(s);
        }
        // (TODO 108 (3) C1) Its local contrast (TF_NATIVE_CLARITY, live, off by default) and its strength (the F1 slider).
        {
            ps2_settings::Setting s;
            s.key = "TF_NATIVE_CLARITY";
            s.section = "Graphics";
            s.label = "Local contrast (native renderer)";
            s.order = 59;
            s.help = "Native renderer: brings out the detail of the 3D scene - rocks, ice, metal - against its surroundings, "
                     "without changing its colours or overall brightness. The HUD and menus are not changed.";
            s.choices = {{"0", "Off"}, {"1", "On"}};
            s.defaultValue = "0";
            s.apply = [](const std::string &value) {
                frame::setClarity(value == "1" ? 1 : 0);
                return true;
            };
            s.available = []() {
                const char *on = std::getenv("TF_NATIVE_RENDER");
                return ps2_settings::choice("TF_RENDERER") == "native" || (on && *on && *on != '0');
            };
            s.unavailableHelp = "Only for the native renderer (Display: Renderer).";
            ps2_settings::add(s);
        }
        {
            ps2_settings::Setting s;
            s.key = "TF_NATIVE_CLARITY_STRENGTH";
            s.section = "Graphics";
            s.label = "Local contrast strength (native renderer)";
            s.order = 59;
            s.help = "Native renderer, the local contrast: how strong it is. Too strong, it shows halos at hard edges and the "
                     "textures' grain.";
            s.choices = {{"0", "0%"}, {"0.1", "10%"}, {"0.2", "20%"}, {"0.3", "30%"}, {"0.4", "40%"}, {"0.5", "50%"}, {"0.6", "60%"}, {"0.7", "70%"}, {"0.8", "80%"}, {"0.9", "90%"}, {"1", "100%"}};
            s.defaultValue = "0.5";
            s.apply = [](const std::string &value) {
                frame::setClarityStrength(static_cast<float>(std::atof(value.c_str())));
                return true;
            };
            s.available = []() { return ps2_settings::choice("TF_NATIVE_CLARITY") == "1"; };
            s.unavailableHelp = "Turn it on first (the row above).";
            ps2_settings::add(s);
        }
        // Its supersampling (TF_NATIVE_SSAA, live, off by default): the 3D drawn at 1.5x the screen's pixels each way.
        {
            ps2_settings::Setting s;
            s.key = "TF_NATIVE_SSAA";
            s.section = "Graphics";
            s.label = "Supersampling (native renderer)";
            s.order = 56;
            s.help = "Native renderer: the 3D scene drawn at 1.5 times the screen's resolution each way and averaged down - "
                     "smoother edges and finer detail (leaves, fences, distant geometry) than edge smoothing alone. "
                     "Costs GPU time: about twice the 3D's pixels.";
            s.choices = {{"0", "Off"}, {"1", "On (1.5x)"}};
            s.defaultValue = "0";
            s.apply = [](const std::string &value) {
                d3d::setSupersampling(value == "1");
                return true;
            };
            s.available = []() {
                const char *on = std::getenv("TF_NATIVE_RENDER");
                return ps2_settings::choice("TF_RENDERER") == "native" || (on && *on && *on != '0');
            };
            s.unavailableHelp = "Only for the native renderer (Display: Renderer).";
            ps2_settings::add(s);
        }
        // Its render scale (TF_NATIVE_RENDER_SCALE, live, 100% by default): fewer lines than the window's, shown enlarged.
        // On a small integrated GPU (Intel N100 / UHD Graphics) the full-screen passes' pixels are what limits the frame
        // rate: at 1080p it draws ~13 frames a second, at 50% ~34. (TheBrousse, PR #9 of the public repository.)
        {
            ps2_settings::Setting s;
            s.key = "TF_NATIVE_RENDER_SCALE";
            s.section = "Graphics";
            s.label = "Render scale (native renderer)";
            s.order = 56;
            s.help = "Native renderer: the picture drawn with fewer pixels than the window has and enlarged to fill it - "
                     "softer, but much lighter for the graphics card. For integrated graphics that cannot keep up at the "
                     "screen's resolution. 100%: the window's own resolution.";
            s.choices = {{"100", "100%"}, {"85", "85%"}, {"75", "75%"}, {"67", "67%"}, {"50", "50%"}};
            s.defaultValue = "100";
            s.apply = [](const std::string &value) {
                frame::setRenderScale(std::atoi(value.c_str()));
                return true;
            };
            s.available = []() {
                const char *on = std::getenv("TF_NATIVE_RENDER");
                return ps2_settings::choice("TF_RENDERER") == "native" || (on && *on && *on != '0');
            };
            s.unavailableHelp = "Only for the native renderer (Display: Renderer).";
            ps2_settings::add(s);
        }
        // Its quality preset (TF_NATIVE_QUALITY, a restart; TODO 94): Automatic picks from the graphics card's memory.
        {
            ps2_settings::Setting s;
            s.key = "TF_NATIVE_QUALITY";
            s.section = "Graphics";
            s.label = "Quality (native renderer)";
            s.order = 54;
            s.help = "Native renderer: Low / Medium for 4 GB graphics cards (GTX 1650 class), High / Ultra for larger ones. "
                     "Sets the texture filtering (anisotropy 4x / 8x / 16x / 16x), the largest size the remastered textures are "
                     "used at (1024 / 2048 / 2048 / full) and, on Low and Medium (on AMD Radeon cards on every preset), the GPU's "
                     "own clipping of nearby geometry (faster; colours on triangles cut at the screen's near edge are interpolated "
                     "slightly differently). "
                     "Automatic: from your graphics card's memory. Takes effect after a restart.";
            s.choices = {{"auto", "Automatic"}, {"low", "Low"}, {"medium", "Medium"}, {"high", "High"}, {"ultra", "Ultra"}};
            s.defaultValue = "auto";
            s.apply = [](const std::string &) { return false; };
            s.available = []() {
                const char *on = std::getenv("TF_NATIVE_RENDER");
                return ps2_settings::choice("TF_RENDERER") == "native" || (on && *on && *on != '0');
            };
            s.unavailableHelp = "Only for the native renderer (Display: Renderer).";
            ps2_settings::add(s);
        }
        occlusion::install(runtime); // (the sun's visibility test, roadmap N3a: nr_occlusion.cpp)
        particles::install(runtime, runtime.memory().getRDRAM()); // (the particle pipelines, TODO 144: nr_particles.cpp)
        rain::install(runtime.memory().getRDRAM()); // (the rain's batches, TODO 146: nr_rain.cpp)
        const char *env = std::getenv("TF_NATIVE_RENDER");
        g_enabled = env && *env && *env != '0';
        if (!g_enabled)
            return;
        movie::install(runtime); // (the movies' pictures from the decoder, roadmap N6: nr_movie.cpp)
        if (const char *td = std::getenv("TF_NATIVE_TEXDUMP"); td && *td == '1')
        {
            g_texDump = true;
            std::error_code ec;
            std::filesystem::create_directories("runs/native_tex", ec);
        }
        if (const char *imm = std::getenv("TF_NATIVE_IMMEDIATE"))
            g_noImmediate = *imm == '0';
        if (const char *grass = std::getenv("TF_NATIVE_GRASS"))
            g_noGrass = *grass == '0';
        if (const char *it = std::getenv("TF_NATIVE_ITERATOR"))
            g_iterator = *it == '1';
        if (const char *pk = std::getenv("TF_NATIVE_PACKET"))
        {
            g_packets = std::atoi(pk);
            std::remove("runs/native_packets.txt");
        }
        if (const char *why = std::getenv("TF_NATIVE_WHY"))
            g_why = std::atoi(why);
        if (const char *dump = std::getenv("TF_NATIVE_DUMP"))
            g_dump = *dump && *dump != '0';
        if (const char *trace = std::getenv("TF_NATIVE_TRACE"))
            g_trace = std::atoi(trace);
        if (const char *shots = std::getenv("TF_NATIVE_SHOT"))
            for (const char *p = shots; *p;)
            {
                char *end = nullptr;
                const unsigned long long t = std::strtoull(p, &end, 10);
                if (end == p)
                    break;
                g_shots.push_back(t);
                p = *end == ',' ? end + 1 : end;
            }
        std::sort(g_shots.begin(), g_shots.end());
        g_microUpload=wrap(runtime,0x00225300u,microUpload);
        g_microReturn=wrap(runtime,0x00225640u,microReturn);
        g_interframeReady=g_microUpload&&g_microReturn;
        g_beginFrame = wrap(runtime, kBeginFrame, beginFrame);
        g_renderSet = wrap(runtime, kRenderGeometrySet, renderGeometrySet);
        g_sendMatrix = wrap(runtime, kSendMatrix, sendMatrix);
        g_setDestructor = wrap(runtime, kSetDestructor, setDestructor);
        g_textureDestructor = wrap(runtime, kTextureDestructor, textureDestructor);
        g_setTexture = wrap(runtime, kSetTexture, setTexture);
        g_endFrame = wrap(runtime, kEndFrame, endFrame);
        if (rain::enabled())
        {
            // (TODO 146: the rain's blocks copied before their memory is written over)
            g_listAlloc = wrap(runtime, kListAlloc, listAlloc);
            g_listFlush = wrap(runtime, kListFlush, listFlush);
            g_listFlush2 = wrap(runtime, kListFlush2, listFlush2);
        }
        else if (contextsMode() != 0 || lcCensus())
            g_listFlush2 = wrap(runtime, kListFlush2, listFlush2); // (N5 step 2 part 2; step 3's census)
        g_applyStateBlock = wrap(runtime, kApplyStateBlock, applyStateBlock);
        g_captureStateBlock = wrap(runtime, kCaptureStateBlock, captureStateBlock);
        g_writeGsRegister = wrap(runtime, kWriteGsRegister, writeGsRegister);
        if (noListMode() != 0)
            g_writeGsRegisterPair = wrap(runtime, kWriteGsRegisterPair, writeGsRegisterPair); // (N5.1: its writes into the register stream)
        g_uploadLevel = wrap(runtime, kUploadLevel, uploadLevel);
        g_groupLoad = wrap(runtime, kGroupLoad, groupLoad);
        g_primitiveVertex = wrap(runtime, kPrimitiveVertex, primitiveVertex);
        g_primitiveEnd = wrap(runtime, kPrimitiveEnd, primitiveEnd);
        g_primitiveEndStrips = wrap(runtime, kPrimitiveEndStrips, primitiveEndStrips);
        g_primitiveVertexAt = wrap(runtime, kPrimitiveVertexAt, primitiveVertexAt);
        g_sendGridPacket = wrap(runtime, kSendGridPacket, sendGridPacket);
        {
            // (TODO 118, 127: the wrappers that read what their function wrote also where it is entered again)
            static const bool resumeOff = std::getenv("TF_NATIVE_RESUMEWRAP") && *std::getenv("TF_NATIVE_RESUMEWRAP") == '0';
            if (!resumeOff)
            {
                const int sets = wrapResumePoints(runtime, kRenderGeometrySet, kRenderGeometrySetEnd, g_renderSet, renderGeometrySet);
                const int matrices = wrapResumePoints(runtime, kSendMatrix, kSendMatrixEnd, g_sendMatrix, sendMatrix);
                const int groups = wrapResumePoints(runtime, kGroupLoad, kGroupLoadEnd, g_groupLoad, groupLoad);
                const int ends = wrapResumePoints(runtime, kPrimitiveEnd, kPrimitiveEndEnd, g_primitiveEnd, primitiveEnd) +
                                 wrapResumePoints(runtime, kPrimitiveEndStrips, kPrimitiveEndStripsEnd, g_primitiveEndStrips, primitiveEndStrips);
                std::fprintf(stderr, "[native render] wrapped where they are entered again: RenderGeometrySet at %d places, the matrix at %d, the "
                                     "texture group loads at %d, the microcode primitives' ends at %d\n",
                             sets, matrices, groups, ends);
            }
        }
        if (const char *slots = std::getenv("TF_NATIVE_SLOTLOG"); slots && *slots == '1')
        {
            static const auto table = slotTable(std::make_integer_sequence<int, kLoggedSlots>{});
            const uint8_t *rd = runtime.memory().getRDRAM();
            const uint32_t skip[] = {kBeginFrame, kEndFrame, kRenderGeometrySet, kSetTexture, kApplyStateBlock, kCaptureStateBlock};
            for (int i = 0; i < kLoggedSlots; ++i)
            {
                const uint32_t a = word(rd, 0x006B4450u + 4u * static_cast<uint32_t>(i));
                if (a < 0x00100000u || a >= 0x00700000u || std::find(std::begin(skip), std::end(skip), a) != std::end(skip))
                    continue;
                bool seen = false;
                for (int k = 0; k < i; ++k)
                    seen = seen || g_slotAddress[k] == a;
                if (seen)
                    continue;
                g_slotAddress[i] = a;
                g_slotOriginal[i] = wrap(runtime, a, table[static_cast<size_t>(i)]);
                if (!g_slotOriginal[i])
                    g_slotAddress[i] = 0u;
            }
        }
        if (const char *count = std::getenv("TF_NATIVE_COUNT"); count && *count == '1')
            for (size_t i = 0; i < std::size(g_counted); ++i)
                g_counted[i].original = wrap(runtime, g_counted[i].address, kCounters[i]);
        if (!g_beginFrame || !g_renderSet || !g_sendMatrix || !g_setDestructor)
        {
            std::fprintf(stderr, "[native render] off: a function to wrap is missing\n");
            g_enabled = false;
            return;
        }
        std::fprintf(stderr, "[native render] capturing the geometry sets (%zu pictures to save)\n", g_shots.size());
        g_displayRuntime = &runtime;
        // (N3b) What the game draws outside its frames, while the native renderer draws alone (looseGif).
        // TF_NATIVE_LOOSE=0: not (that picture is not drawn).
        if (!(std::getenv("TF_NATIVE_LOOSE") && *std::getenv("TF_NATIVE_LOOSE") == '0'))
            ps2_host_display::setGifObserver(&looseGif);
        // (TF_NATIVE_LOOSE_COMPARE=1: those screens drawn natively with the emulated drawing on too - compare mode's
        // pairs of the same frames, the GS's shot and the native one)
        if (std::getenv("TF_NATIVE_LOOSE_COMPARE") && *std::getenv("TF_NATIVE_LOOSE_COMPARE") == '1')
            ps2_host_display::setGifObserverAlways(true);
        d3d::prepare();
        startLivePicture();
    }

    // (Roadmap N3b) The picture the display circuit shows, read from GS memory. The movies the game plays on their own
    // (the story intro, the level films: taFMVSystem with its pictures DMA'd into the display buffers, no frame of the
    // render device) never pass the native renderer; their pictures are host-to-local uploads, which the GS keeps in
    // local memory with its drawing off (N4) - the window shows them from there (nr_present.cpp). Circuit 1's buffer
    // (DISPFB1: FBP, FBW, PSM, DBX, DBY) over DISPLAY1's area; 32, 24 and 16-bit buffers. False: no buffer shown.
    // (N3b) The GS packets the game sends while no frame of the render device comes (more than two VBlanks since the
    // last): a load screen drawn on its own - the warp tunnel between the front end and the HQ (its VU1 / GIF packets,
    // no BeginFrame). Gathered a VBlank at a time on the thread feeding the GS and handed to the renderer as a frame of
    // its own (Frame::looseGif, drawn as the list's 2D is: expandFrame) - the GS draws none of it. Packets during the
    // game's frames are its own business (the capture reads the list). TF_NATIVE_LOOSE_SHOT=<VBlank>[,...]: those
    // frames saved as runs/native_loose_<VBlank>.bmp; TF_NATIVE_EMULOG=1 tells when they start and stop.
    namespace
    {
        bool censusOn();
        void gifCensus(const uint8_t *data, uint32_t bytes);
    }

    void looseGif(const uint8_t *data, uint32_t bytes)
    {
        PS2Runtime *runtime = g_displayRuntime;
        if (!runtime || bytes < 16u)
            return;
        if (censusOn())
            gifCensus(data, bytes); // (TF_VU1_CENSUS: the GS transfers counted)
        const GSRegisters &gs = runtime->memory().gs();
        const uint64_t tick = gs.vsyncTick.load(std::memory_order_relaxed);
        static std::mutex mutex;
        static std::vector<uint32_t> words;
        static std::vector<uint32_t> pre; // (packets after the game's frames stopped, before its display switched)
        static uint64_t wordsTick = 0u;
        static uint64_t wordsDisplay = 0u; // (the display buffers when the packets so far came: a flip ends a frame)
        static bool on = false;
        static uint64_t frames = 0u;
        static const bool log = std::getenv("TF_NATIVE_EMULOG") && *std::getenv("TF_NATIVE_EMULOG") == '1';
        static std::vector<uint64_t> shots = [] {
            std::vector<uint64_t> v;
            if (const char *e = std::getenv("TF_NATIVE_LOOSE_SHOT"))
                for (const char *p = e; *p;)
                {
                    char *end = nullptr;
                    const unsigned long long t = std::strtoull(p, &end, 10);
                    if (end == p)
                        break;
                    v.push_back(t);
                    p = *end == ',' ? end + 1 : end;
                }
            std::sort(v.rbegin(), v.rend());
            return v;
        }();
        // (TODO 27: a warp tunnel drawn natively - nr_warp.cpp; its GS packets, the field buffers' draw settings and clears,
        // are not a picture of their own)
        if (warp::active())
            return;
        std::lock_guard<std::mutex> lock(mutex);
        const uint64_t last = g_lastFrameTick.load(std::memory_order_relaxed);
        // (and the display not the one the game's frames had - the warp tunnel switches the circuits: a slow frame of the game
        // keeps its display, its own packets are not taken for a screen of their own)
        const bool loose = outsideFrames();
        if (loose != on)
        {
            on = loose;
            if (log)
                std::fprintf(stderr, "[native render] packets outside the game's frames %s (tick %llu, last frame %llu, %llu frames)\n",
                             loose ? "drawn natively" : "over", static_cast<unsigned long long>(tick), static_cast<unsigned long long>(last),
                             static_cast<unsigned long long>(frames));
            frames = 0u;
            if (!loose)
                words.clear();
            else
            {
                // (the packets since the game's frames stopped, before the display switched: the screen's first)
                words.swap(pre);
                pre.clear();
                wordsTick = tick, wordsDisplay = (gs.dispfb1 & 0x7FFFu) | (gs.dispfb2 & 0x7FFFu) << 16;
            }
        }
        if (!loose)
        {
            // (the game's frames stopped, its display not switched yet - kept for the screen that may follow)
            if (last != 0u && tick > last + 2u && pre.size() < (1u << 20))
            {
                const size_t at = pre.size(), qwords = bytes / 16u;
                pre.resize(at + qwords * 4u);
                std::memcpy(pre.data() + at, data, qwords * 16u);
            }
            else if (tick <= last + 2u)
                pre.clear();
            return;
        }
        // A frame ends where the game flips its display buffers (the loader draws a picture over several VBlanks); with
        // no flip for 8 VBlanks, at the VBlank (a screen drawn once).
        const uint64_t display = (gs.dispfb1 & 0x7FFFu) | (gs.dispfb2 & 0x7FFFu) << 16;
        // (TF_NATIVE_LOOSE_FRAMES=vblank: a frame a VBlank - the picture what the display showed through it, its buffers
        // as drawn so far - as the GS scans its display out. Debug choice.)
        static const bool perVblank = std::getenv("TF_NATIVE_LOOSE_FRAMES") && std::string(std::getenv("TF_NATIVE_LOOSE_FRAMES")) == "vblank";
        static uint64_t lastDisplay = 0u; // (the display at the packets before this one's)
        const uint64_t shownDisplay = lastDisplay;
        lastDisplay = display;
        if (!words.empty() && (perVblank ? tick != wordsTick : (display != wordsDisplay || tick >= wordsTick + 8u)))
        {
            // (the VBlank before's packets: a frame)
            Frame f;
            f.pmode = gs.pmode;
            f.dispfb[0] = gs.dispfb1;
            f.dispfb[1] = gs.dispfb2;
            f.display[0] = gs.display1;
            f.display[1] = gs.display2;
            f.bgcolor = gs.bgcolor;
            // The buffers shown while the frame's packets came - the display before the flip that ended it, not the ones it
            // switched to: the game draws the warp tunnel's cloud into the buffer it has just put on show (the GS scans it
            // out with them), the one it switched to holds only the next frame's first draws - the cloud came out missing.
            // (TF_NATIVE_LOOSE_OLDDISP=0: the display after the flip, as before.)
            static const bool oldDisplay = !(std::getenv("TF_NATIVE_LOOSE_OLDDISP") && *std::getenv("TF_NATIVE_LOOSE_OLDDISP") == '0');
            if (oldDisplay || perVblank)
            {
                const uint64_t shown = perVblank ? shownDisplay : wordsDisplay;
                f.dispfb[0] = (gs.dispfb1 & ~0x7FFFull) | (shown & 0x7FFFu);
                f.dispfb[1] = (gs.dispfb2 & ~0x7FFFull) | ((shown >> 16) & 0x7FFFu);
            }
            // (Not narrowed for widescreen: the screen is the game's whole picture, stretched as the GS's is - the warp
            // tunnel's rings, narrowed where they lay inside the 4:3 picture and not where they reached past it, left
            // black crescents between them. TF_NATIVE_LOOSESQUEEZE=1: narrowed as the list's 2D, as before.)
            static const bool looseSqueeze = std::getenv("TF_NATIVE_LOOSESQUEEZE") && *std::getenv("TF_NATIVE_LOOSESQUEEZE") == '1';
            f.overlaySqueeze = looseSqueeze ? ps2_host_display::overlaySqueeze() : 1.0f;
            f.displayAspect = ps2_host_display::displayAspect();
            f.tick = wordsTick;
            f.movie = tf::port::moviePlaying();
            std::chrono::steady_clock::time_point due{};
            if (!ps2_host_display::vblankDue(wordsTick, due))
                due = std::chrono::steady_clock::now();
            f.dueNs = std::chrono::duration_cast<std::chrono::nanoseconds>(due.time_since_epoch()).count();
            f.periodNs = std::chrono::duration_cast<std::chrono::nanoseconds>(ps2_host_display::vblankPeriodNow()).count();
            f.looseGif.swap(words);
            f.loose = true;
            setGsMemory(runtime->memory().getGSVRAM());
            std::string shot;
            if (!shots.empty() && wordsTick >= shots.back())
            {
                shot = "runs/native_loose_" + std::to_string(shots.back()) + ".bmp";
                while (!shots.empty() && wordsTick >= shots.back())
                    shots.pop_back();
            }
            // (Debug: TF_NATIVE_LOOSE_REGS=<VBlank>: three such frames from it - their register writes (A+D, PACKED) other than the vertices'
            // - PRMODECONT, PRMODE, the ALPHAs, TESTs, PABE, FBA, COLCLAMP, DTHE, TEXA, TEX0s, FRAMEs...)
            static uint64_t regsFrom = std::getenv("TF_NATIVE_LOOSE_REGS") ? std::strtoull(std::getenv("TF_NATIVE_LOOSE_REGS"), nullptr, 10) : 0u;
            static int regsLog = 3;
            // (TF_NATIVE_LOOSE_REGS_NTH=<n>: the n-th frame of every stretch of them too)
            static const uint64_t regsNth = std::getenv("TF_NATIVE_LOOSE_REGS_NTH") ? std::strtoull(std::getenv("TF_NATIVE_LOOSE_REGS_NTH"), nullptr, 10) : 0u;
            if (regsNth && frames + 1u == regsNth && std::getenv("TF_NATIVE_LOOSE_VDUMP") && std::string(std::getenv("TF_NATIVE_LOOSE_VDUMP")) == "nth")
                g_vdumpNthTick.store(wordsTick, std::memory_order_relaxed);
            if ((regsFrom && wordsTick >= regsFrom && regsLog > 0) || (regsNth && frames + 1u == regsNth))
            {
                --regsLog;
                std::string line;
                const std::vector<uint32_t> &w = f.looseGif;
                for (size_t q = 0; q + 4u <= w.size();)
                {
                    const uint64_t tag = static_cast<uint64_t>(w[q]) | static_cast<uint64_t>(w[q + 1u]) << 32;
                    const uint64_t regs = static_cast<uint64_t>(w[q + 2u]) | static_cast<uint64_t>(w[q + 3u]) << 32;
                    const uint32_t nloop = static_cast<uint32_t>(tag & 0x7FFFu), flg = static_cast<uint32_t>((tag >> 58) & 3u);
                    uint32_t nreg = static_cast<uint32_t>((tag >> 60) & 0xFu);
                    if (nreg == 0u)
                        nreg = 16u;
                    q += 4u;
                    if (flg == 0u)
                    {
                        for (uint32_t l = 0; l < nloop; ++l)
                            for (uint32_t r = 0; r < nreg && q + 4u <= w.size(); ++r, q += 4u)
                                if (((regs >> (4u * r)) & 0xFu) == 0xEu)
                                {
                                    const uint32_t reg = w[q + 2u] & 0xFFu;
                                    const uint64_t value = static_cast<uint64_t>(w[q]) | static_cast<uint64_t>(w[q + 1u]) << 32;
                                    if (reg != 0x01u && reg != 0x02u && reg != 0x03u && reg != 0x04u && reg != 0x05u &&
                                        reg != 0x0Cu && reg != 0x0Du)
                                    {
                                        char part[48];
                                        std::snprintf(part, sizeof(part), " %02x=%llx", reg, static_cast<unsigned long long>(value));
                                        line += part;
                                    }
                                }
                    }
                    else if (flg == 1u)
                        q += 4u * ((static_cast<size_t>(nloop) * nreg + 1u) / 2u);
                    else
                        q += 4u * nloop;
                }
                std::fprintf(stderr, "[native render] loose %llu registers:%s\n", static_cast<unsigned long long>(wordsTick), line.c_str());
            }
            // (Debug: TF_NATIVE_LOOSE_GSDUMP=<VBlank>: at the first such frame from it, GS memory's 640 x 224 at FBP 0, 0x46 and
            // 0xd2 - the tunnel's buffers, as the GS drew them with the emulated drawing on (compare mode, the software
            // renderer: PS2X_GPU=0) - as runs/gsdump_loose_<tick>_<fbp>.ppm)
            static uint64_t gsDumpAt = std::getenv("TF_NATIVE_LOOSE_GSDUMP") ? std::strtoull(std::getenv("TF_NATIVE_LOOSE_GSDUMP"), nullptr, 10) : 0u;
            static bool gsDumpNth = std::getenv("TF_NATIVE_LOOSE_GSDUMP") && std::string(std::getenv("TF_NATIVE_LOOSE_GSDUMP")) == "nth";
            if ((gsDumpAt && wordsTick >= gsDumpAt) || (gsDumpNth && regsNth && frames + 1u == regsNth && frames + 1u > 121u))
            {
                if (gsDumpNth)
                    shot = "runs/native_loose_nth.bmp";
                gsDumpAt = 0u;
                gsDumpNth = false;
                (void)runtime->gs().ReadVram(0u, 0u, 1u, 0u, 0u); // (the GS through its queue)
                for (const uint32_t fbp : {0x00u, 0x46u, 0xd2u})
                {
                    char path[96];
                    std::snprintf(path, sizeof(path), "runs/gsdump_loose_%llu_%02x.ppm", static_cast<unsigned long long>(wordsTick), fbp);
                    dumpGsRegion(path, fbp * 32u, 10u, 0u, 640u, 224u);
                    std::snprintf(path, sizeof(path), "runs/gsdump_loose_%llu_%02x_alpha.ppm", static_cast<unsigned long long>(wordsTick), fbp);
                    dumpGsRegion(path, fbp * 32u, 10u, 0x7Fu, 640u, 224u);
                }
                // (and TF_NATIVE_LOOSE_TEXDUMP=<tex0 hex>[,...]: those textures as GS memory holds them now - the GS's view
                // at the frame's end - as runs/gsdump_tex_<tbp>.ppm, beside the renderer's own loose_tex_<tbp>.ppm)
                if (const char *e = std::getenv("TF_NATIVE_LOOSE_TEXDUMP"))
                    for (const char *p = e; *p;)
                    {
                        char *end = nullptr;
                        const uint64_t tex0 = std::strtoull(p, &end, 16);
                        if (end == p)
                            break;
                        p = *end == ',' ? end + 1 : end;
                        setGsMemory(runtime->memory().getGSVRAM());
                        if (auto made = textureFromGsMemory(tex0, 0u))
                        {
                            char path[96];
                            std::snprintf(path, sizeof(path), "runs/gsdump_tex_%04llx.ppm", static_cast<unsigned long long>(tex0 & 0x3FFFu));
                            if (FILE *f = std::fopen(path, "wb"))
                            {
                                std::fprintf(f, "P6\n%u %u\n255\n", made->width, made->height);
                                for (uint32_t c : made->pixels)
                                {
                                    const uint8_t rgb[3] = {static_cast<uint8_t>(c), static_cast<uint8_t>(c >> 8), static_cast<uint8_t>(c >> 16)};
                                    std::fwrite(rgb, 1, 3, f);
                                }
                                std::fclose(f);
                            }
                        }
                    }
            }
            if (const char *e = std::getenv("TF_NATIVE_DISPLOG"))
            {
                char *end = nullptr;
                const uint64_t a = std::strtoull(e, &end, 10), b = end && *end == '-' ? std::strtoull(end + 1, nullptr, 10) : a;
                if (wordsTick >= a && wordsTick <= b)
                    std::fprintf(stderr, "[native render] display loose frame from tick %llu to %llu: %zu words, PMODE %llx DISPFB1 %llx DISPLAY1 %llx DISPFB2 %llx DISPLAY2 %llx BGCOLOR %llx\n",
                                 static_cast<unsigned long long>(wordsTick), static_cast<unsigned long long>(tick), f.looseGif.size(),
                                 static_cast<unsigned long long>(f.pmode), static_cast<unsigned long long>(f.dispfb[0]),
                                 static_cast<unsigned long long>(f.display[0]), static_cast<unsigned long long>(f.dispfb[1]),
                                 static_cast<unsigned long long>(f.display[1]), static_cast<unsigned long long>(f.bgcolor));
            }
            submitFrame(std::move(f), shot);
            g_looseSince3D.fetch_add(1u, std::memory_order_relaxed);
            ++frames;
            words.clear();
        }
        if (words.empty())
            wordsTick = tick, wordsDisplay = display;
        const size_t at = words.size(), qwords = bytes / 16u;
        words.resize(at + qwords * 4u);
        std::memcpy(words.data() + at, data, qwords * 16u);
    }

    // (TODO 27) A load screen's picture handed over (nr_warp.cpp): counted as looseGif's are (TODO 120: the scene's first
    // frame after it has its 3D left out).
    void noteLooseFrame() { g_looseSince3D.fetch_add(1u, std::memory_order_relaxed); }

    uint64_t vblankTick()
    {
        PS2Runtime *runtime = g_displayRuntime;
        return runtime ? runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed) : 0u;
    }

    namespace
    {
        std::atomic<uint64_t> g_censusCounts[3] = {}, g_censusAmounts[3] = {};
    }

    void censusNote(uint32_t kind, uint64_t amount)
    {
        if (kind >= 3u)
            return;
        g_censusCounts[kind].fetch_add(1u, std::memory_order_relaxed);
        g_censusAmounts[kind].fetch_add(amount, std::memory_order_relaxed);
    }

    // (TF_VU1_CENSUS) The GS transfers in the packets that reach the GS while the native renderer draws alone (looseGif's
    // observer): by direction, and the host-to-local ones by destination (base, width, format, rectangle) - count, bytes.
    namespace
    {
        struct GifCensus
        {
            std::mutex mutex;
            uint64_t bitbltbuf = 0u, trxreg = 0u, trxpos = 0u;
            uint64_t dir[4] = {}, imageBytes = 0u, imageTags = 0u, packets = 0u, packetBytes = 0u;
            std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> dests; // key -> count, bytes
        } g_gifCensus;

        bool censusOn()
        {
            static const bool on = []() { const char *e = std::getenv("TF_VU1_CENSUS"); return e && *e && *e != '0'; }();
            return on;
        }

        void gifCensus(const uint8_t *data, uint32_t bytes)
        {
            GifCensus &c = g_gifCensus;
            std::lock_guard<std::mutex> lock(c.mutex);
            ++c.packets;
            c.packetBytes += bytes;
            uint32_t at = 0u;
            while (at + 16u <= bytes)
            {
                uint64_t lo, hi;
                std::memcpy(&lo, data + at, 8u);
                std::memcpy(&hi, data + at + 8u, 8u);
                at += 16u;
                const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu), flg = static_cast<uint32_t>((lo >> 58) & 3u);
                uint32_t nreg = static_cast<uint32_t>((lo >> 60) & 0xFu);
                if (nreg == 0u)
                    nreg = 16u;
                if (flg == 0u)
                {
                    for (uint32_t l = 0; l < nloop; ++l)
                        for (uint32_t r = 0; r < nreg; ++r)
                        {
                            if (at + 16u > bytes)
                                return;
                            if (((hi >> (4u * r)) & 0xFu) == 0xEu) // A+D
                            {
                                uint64_t value;
                                std::memcpy(&value, data + at, 8u);
                                const uint8_t addr = data[at + 8u];
                                if (addr == 0x50u)
                                    c.bitbltbuf = value;
                                else if (addr == 0x51u)
                                    c.trxpos = value;
                                else if (addr == 0x52u)
                                    c.trxreg = value;
                                else if (addr == 0x53u)
                                {
                                    const uint32_t d = static_cast<uint32_t>(value & 3u);
                                    ++c.dir[d];
                                    if (d == 0u)
                                    {
                                        const uint64_t key = ((c.bitbltbuf >> 32) & 0xFFFFFFFFull) << 24 | (c.trxreg & 0xFFFu) << 12 | ((c.trxreg >> 32) & 0xFFFu);
                                        ++c.dests[key].first;
                                    }
                                }
                            }
                            at += 16u;
                        }
                }
                else if (flg == 1u)
                    at += ((nloop * nreg + 1u) / 2u) * 16u;
                else
                {
                    const uint64_t b = static_cast<uint64_t>(nloop) * 16u;
                    c.imageBytes += b;
                    ++c.imageTags;
                    const uint64_t key = ((c.bitbltbuf >> 32) & 0xFFFFFFFFull) << 24 | (c.trxreg & 0xFFFu) << 12 | ((c.trxreg >> 32) & 0xFFFu);
                    c.dests[key].second += b;
                    at += static_cast<uint32_t>(std::min<uint64_t>(b, bytes - at));
                }
            }
        }
    }

    std::string censusLine()
    {
        uint64_t n[3], a[3];
        for (int k = 0; k < 3; ++k)
        {
            n[k] = g_censusCounts[k].exchange(0u, std::memory_order_relaxed);
            a[k] = g_censusAmounts[k].exchange(0u, std::memory_order_relaxed);
        }
        char line[512];
        std::snprintf(line, sizeof(line), "level uploads %llu (%.1f KB); light-map style reads from GS memory %llu (%llu pixels); TEX0 reads from GS memory %llu (%llu pixels)",
                      static_cast<unsigned long long>(n[0]), a[0] / 1024.0, static_cast<unsigned long long>(n[1]),
                      static_cast<unsigned long long>(a[1]), static_cast<unsigned long long>(n[2]), static_cast<unsigned long long>(a[2]));
        std::string out = line;
        GifCensus &c = g_gifCensus;
        std::lock_guard<std::mutex> lock(c.mutex);
        std::snprintf(line, sizeof(line), "; GIF packets to the GS %llu (%.1f KB): transfers host->local %llu, local->host %llu, local->local %llu, image data %.1f KB in %llu tags",
                      static_cast<unsigned long long>(c.packets), c.packetBytes / 1024.0, static_cast<unsigned long long>(c.dir[0]),
                      static_cast<unsigned long long>(c.dir[1]), static_cast<unsigned long long>(c.dir[2]), c.imageBytes / 1024.0,
                      static_cast<unsigned long long>(c.imageTags));
        out += line;
        std::vector<std::pair<uint64_t, std::pair<uint64_t, uint64_t>>> top(c.dests.begin(), c.dests.end());
        std::sort(top.begin(), top.end(), [](const auto &x, const auto &y) { return x.second.second > y.second.second; });
        for (size_t i = 0; i < top.size() && i < 8u; ++i)
        {
            const uint64_t k = top[i].first;
            const uint32_t buf = static_cast<uint32_t>(k >> 24);
            std::snprintf(line, sizeof(line), "\n[vu1:census]   GS upload to bp %04x bw %u psm %02x %ux%u: %llu (%.1f KB)", buf & 0x3FFFu, (buf >> 16) & 0x3Fu,
                          (buf >> 24) & 0x3Fu, static_cast<unsigned>((k >> 12) & 0xFFFu), static_cast<unsigned>(k & 0xFFFu),
                          static_cast<unsigned long long>(top[i].second.first), top[i].second.second / 1024.0);
            out += line;
        }
        std::snprintf(line, sizeof(line), "\n[vu1:census]   GS upload destinations: %zu", top.size());
        out += line;
        c.dests.clear();
        c.packets = c.packetBytes = c.imageBytes = c.imageTags = 0u;
        std::fill(std::begin(c.dir), std::end(c.dir), 0ull);
        return out;
    }

    // (N3b) Whether the game draws a screen of its own now: its frames stopped (over two VBlanks) and the display not the
    // one they had. VU1 runs the batches it would leave out then (vu1_native.cpp: the screen's own VU1 work - the warp
    // tunnel's cloud), and the GS hands their native meshes over (ps2_host_display::setGifObserverMeshes). Any thread;
    // worked out once a VBlank per thread.
    bool outsideFrames()
    {
        PS2Runtime *runtime = g_displayRuntime;
        if (!runtime || !ps2_host_display::gifObserver())
            return false;
        const uint64_t tick = runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed);
        thread_local uint64_t cachedTick = ~0ull;
        thread_local bool cached = false;
        if (tick == cachedTick)
            return cached;
        cachedTick = tick;
        const uint64_t last = g_lastFrameTick.load(std::memory_order_relaxed);
        cached = last != 0u && tick > last + 2u && !gsDisplayFromFrames();
        if (cached != ps2_host_display::gifObserverMeshes())
            ps2_host_display::setGifObserverMeshes(cached);
        return cached;
    }

    // (N3b) Whether the display circuit shows what the game's frames showed - else the game shows something of its own
    // without frames of the render device (the warp tunnel of a load, a movie DMA'd to the display buffers).
    bool gsDisplayFromFrames()
    {
        PS2Runtime *runtime = g_displayRuntime;
        if (!runtime || g_frameBuffersNext.load(std::memory_order_relaxed) == 0u)
            return true;
        const GSRegisters &gs = runtime->memory().gs();
        if (gs.pmode != g_framePmode.load(std::memory_order_relaxed) || gs.display1 != g_frameDisplay.load(std::memory_order_relaxed))
            return false;
        const uint64_t buffer = gs.dispfb1 & 0x7FFFu;
        for (const auto &b : g_frameBuffers)
            if (b.load(std::memory_order_relaxed) == buffer)
                return true;
        return false;
    }

    bool gsDisplayedPicture(std::vector<uint32_t> &pixels, uint32_t &width, uint32_t &height)
    {
        PS2Runtime *runtime = g_displayRuntime;
        if (!runtime)
            return false;
        const GSRegisters &gs = runtime->memory().gs();
        const uint8_t *vram = runtime->memory().getGSVRAM();
        const uint64_t pmode = gs.pmode;
        const bool one = (pmode & 1u) != 0u;
        if (!vram || (!one && (pmode & 2u) == 0u))
            return false;
        const uint64_t dispfb = one ? gs.dispfb1 : gs.dispfb2, display = one ? gs.display1 : gs.display2;
        const uint32_t fbp = static_cast<uint32_t>(dispfb & 0x1FFu), fbw = static_cast<uint32_t>((dispfb >> 9) & 0x3Fu);
        const uint32_t psm = static_cast<uint32_t>((dispfb >> 15) & 0x1Fu);
        const uint32_t dbx = static_cast<uint32_t>((dispfb >> 32) & 0x7FFu), dby = static_cast<uint32_t>((dispfb >> 43) & 0x7FFu);
        const uint32_t magh = static_cast<uint32_t>((display >> 23) & 0xFu) + 1u;
        const uint32_t dw = static_cast<uint32_t>((display >> 32) & 0xFFFu) + 1u, dh = static_cast<uint32_t>((display >> 44) & 0x7FFu) + 1u;
        width = std::min(dw / magh, fbw * 64u);
        height = std::min(dh, 1024u);
        if (fbw == 0u || width == 0u || height == 0u || width > 2048u)
            return false;
        pixels.resize(static_cast<size_t>(width) * height);
        const uint32_t block = fbp * 32u;
        constexpr uint32_t kBytes = 4u * 1024u * 1024u;
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
            {
                uint32_t c = 0u;
                if (psm == 0x02u || psm == 0x0Au)
                {
                    const uint32_t at = GSPSMCT16::addrPSMCT16(block, fbw, dbx + x, dby + y);
                    if (at + 2u <= kBytes)
                    {
                        const uint32_t v = vram[at] | vram[at + 1u] << 8;
                        c = ((v & 31u) << 3) | (((v >> 5) & 31u) << 11) | (((v >> 10) & 31u) << 19);
                    }
                }
                else
                {
                    const uint32_t at = GSPSMCT32::addrPSMCT32(block, fbw, dbx + x, dby + y);
                    if (at + 4u <= kBytes)
                        std::memcpy(&c, vram + at, 4u);
                }
                pixels[static_cast<size_t>(y) * width + x] = c | 0xFF000000u;
            }
        // (Debug: TF_NATIVE_GSMOVIE_SHOT=<VBlank>[,...]: the first picture read at or after each as runs\gsmovie_<VBlank>.bmp.)
        static std::vector<uint64_t> shots = [] {
            std::vector<uint64_t> v;
            if (const char *e = std::getenv("TF_NATIVE_GSMOVIE_SHOT"))
                for (const char *p = e; *p;)
                {
                    char *end = nullptr;
                    const unsigned long long t = std::strtoull(p, &end, 10);
                    if (end == p)
                        break;
                    v.push_back(t);
                    p = *end == ',' ? end + 1 : end;
                }
            std::sort(v.rbegin(), v.rend());
            return v;
        }();
        const uint64_t tick = gs.vsyncTick.load(std::memory_order_relaxed);
        if (!shots.empty() && tick >= shots.back())
        {
            const uint64_t asked = shots.back();
            while (!shots.empty() && tick >= shots.back())
                shots.pop_back();
            const std::string path = "runs/gsmovie_" + std::to_string(asked) + ".bmp";
            if (FILE *f = std::fopen(path.c_str(), "wb"))
            {
                const uint32_t row = (width * 3u + 3u) & ~3u, size = 54u + row * height;
                uint8_t h[54] = {'B', 'M'};
                const auto put = [&h](int at, uint32_t v) { std::memcpy(h + at, &v, 4); };
                put(2, size), put(10, 54u), put(14, 40u), put(18, width), put(22, height), h[26] = 1, h[28] = 24, put(34, row * height);
                std::fwrite(h, 1, 54, f);
                std::vector<uint8_t> line(row, 0u);
                for (uint32_t y = height; y-- > 0;)
                {
                    for (uint32_t x = 0; x < width; ++x)
                    {
                        const uint32_t c = pixels[static_cast<size_t>(y) * width + x];
                        line[3u * x] = static_cast<uint8_t>(c >> 16), line[3u * x + 1u] = static_cast<uint8_t>(c >> 8), line[3u * x + 2u] = static_cast<uint8_t>(c);
                    }
                    std::fwrite(line.data(), 1, row, f);
                }
                std::fclose(f);
                std::fprintf(stderr, "[native render] gs movie picture at tick %llu saved (%ux%u, DISPFB %llx)\n", static_cast<unsigned long long>(tick),
                             width, height, static_cast<unsigned long long>(dispfb));
            }
        }
        return true;
    }
}
