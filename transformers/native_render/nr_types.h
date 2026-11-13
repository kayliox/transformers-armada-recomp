// The native renderer's own data (README.md): what the capture side (the game's thread, guest
// memory) hands the Direct3D 12 side. Plain types only - no Windows, no raylib, no runtime headers.
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>

#include "nr_shader.h"
#include <atomic>
#include <condition_variable>
#include "nr_microcode_prefix.h"
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace view_turn
{
    struct FrameCamera;
}

namespace nr
{
    // (TODO 290 fps) A frame's own time on the GPU, from the backend's timestamps once they are read (nr_thread.cpp keeps
    // the last ones: nr::gpuFrameMs in nr_install.h, the rate governor's way up).
    void noteGpuFrame(double ms);

    // A vertex in the model's space, as a geometry set holds it (decoded once per set).
    struct Vertex
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
        uint32_t colour = 0xFFFFFFFFu; // RGBA8, 128 per channel = the PS2's 1.0 (as the GS reads it)
        float u = 0.0f, v = 0.0f;
        float nx = 0.0f, ny = 0.0f, nz = 0.0f; // the normal (batch type 4: lit meshes)
        float size = 0.0f;                     // a sprite's (batch type 1: the position's w)
    };

    struct Mesh;
    // (grassgpu) A ground's grass to be grown on the GPU (nr_d3d.cpp's grass compute pass) in place of nr_grass.cpp's blades on
    // the CPU: the ground's triangles, the grass routine's block (its first 56 qwords: the frustum, the blade, the matrix, the
    // density, the points' table), the thinning with the grass's reach (TF_GRASS_DISTANCE) as nr_grass.cpp has it. `cpu`: the
    // CPU's blades for the same, a checking aid (TF_GRASS_GPU_CHECK).
    struct GpuGrass
    {
        std::shared_ptr<const Mesh> ground;
        Qword block[56] = {};
        float thinning = 0.0f;
        std::shared_ptr<const Mesh> cpu;
    };

    // A geometry set's triangles (a list: three indices a triangle), decoded from its packet.
    struct Mesh
    {
        uint64_t id = 0;          // unique while this mesh lives (the GPU side keys its buffers by it)
        uint32_t source = 0;      // the vlGeometrySetPS2 it came from (guest address)
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        bool hasColour = false, hasUv = false, hasNormal = false;
        bool sprites = false;   // four corners a sprite (u, v the corner), from batch type 1
        bool immediate = false; // drawn this frame only (the display list's own GS drawing): screen space
        bool projective = false; // u, v are S, T and size is Q: divided per pixel, as the GS does (STQ)
        bool primitive = false;  // a microcode primitive's (vlMicrocodePrimitive: sprites the device builds itself), this frame's
        bool ocean = false;      // the FFT ocean's grid patch (vlxWaterEffect: its packet read from the list), this frame's
        bool devicePrimitive = false; // (TODO 295) the render device's own primitive's strips (slot 29), this frame's
        uint32_t type = 0; // the batches' type (docs/RENDERER.md: 3 ST colour position, 4 ST normal position, ...)
        uint32_t prim = 0;  // the strips' GS PRIM, context 1's tag: bit 6 ABE (blending), bit 5 FGE (fog)
        uint32_t prim2 = 0; // context 2's tag (the lit meshes' base pass)
        std::shared_ptr<const GpuGrass> gpuGrass; // (grassgpu: its blades grown on the GPU - no vertices here)
    };

    struct Texture;

    // (TODO 164) A texture being converted on a worker (nr_capture.cpp's conversion pool): its pixels, mips and
    // gameMips go in there, and nothing reads them until `done` (waitTexture).
    struct TexturePending
    {
        std::atomic<bool> done{false};
        std::mutex mutex;
        std::condition_variable cv;
        // (TODO 177) The large image already loaded for the same content, found by the worker: drawn in its place from
        // the conversion's own frame (set before `done`) - not the game's texture for a frame until the large image is
        // asked for when the conversion is back.
        std::shared_ptr<const Texture> large;
    };

    // A texture converted once to RGBA8 (R in the low byte), from a vlTexturePS2 (nr_texture.cpp).
    struct Texture
    {
        uint64_t id = 0;          // unique while this texture lives (the GPU side keys its copy by it)
        uint32_t source = 0;      // the vlTexturePS2 (guest address)
        uint32_t width = 0, height = 0;
        std::vector<uint32_t> pixels;
        bool upscaled = false; // the remaster's high-resolution image in place of the game's (nr_hires.cpp)
        std::vector<std::vector<uint32_t>> mips; // the levels below, when made ahead (off the game's thread)
        uint32_t gameMips = 0; // of them, the first this many are the game's own uploaded levels (TEX1's), not box-filtered
        // A large image's game texture (the one it was asked for with): drawn in its place until the
        // large one is on the GPU (nr_d3d.cpp's uploader), then let go of (TODO 59).
        std::shared_ptr<const Texture> fallback;
        // Block-compressed (TODO 94; a large image only): 1 = BC7, every level from level 0 in `blocks`
        // (pixels and mips empty); 0 = RGBA8 in pixels and mips.
        uint32_t format = 0;
        std::vector<std::vector<uint8_t>> blocks;
        // (TODO 119) A font atlas's twin with the keyboard's keys in place of the button symbols: drawn
        // instead while the keyboard and mouse are the device last used (keyPromptsShown).
        std::shared_ptr<const Texture> keys;
        std::shared_ptr<const Texture> xbox;
        // (its Xbox 360 look: drawn instead of xbox while the pads plugged in are 360 ones, xbox360PromptsShown)
        std::shared_ptr<const Texture> xbox360;
        // (TODO 164) Set while a worker converts it: its pixels are not there until pending->done (waitTexture).
        std::shared_ptr<TexturePending> pending;
        // (TODO 164) Its texels' hash (pixelHash, nr_texture.cpp), made where it was converted; 0 = not made yet.
        uint64_t pixelHash = 0;
    };

    // (TODO 164) A texture's pixels are there: at once, or once its worker is done with it.
    inline void waitTexture(const Texture &t)
    {
        const std::shared_ptr<TexturePending> p = t.pending;
        if (!p || p->done.load(std::memory_order_acquire))
            return;
        std::unique_lock<std::mutex> lock(p->mutex);
        p->cv.wait(lock, [&] { return p->done.load(std::memory_order_acquire); });
    }

    // Whether the button prompts show the keyboard's keys now (TF_KEY_PROMPTS on and the keyboard / mouse the
    // device last used; nr_hires.cpp). Any thread.
    bool keyPromptsShown();
    bool xboxPromptsShown();
    bool xbox360PromptsShown();

    // The mip chain below a texture's image (each level the one above box-filtered), into its mips.
    void buildMips(Texture &texture);

    // An 8-bit texture's palette alone, as a 256 x 1 texture in the order the indices read it (the
    // depth fog's: its index is made from the depth on the GPU, nr_d3d.cpp).
    struct TextureKey;
    std::shared_ptr<const Texture> convertPalette(const uint8_t *rdram, const TextureKey &key, uint64_t id);

    // The remaster's large images, loaded on a thread of their own (a level's hundred 4K images
    // took the game's thread seconds): asked for with the game's own texture (its texels pick the
    // candidate), taken back when ready - the game's texture is drawn meanwhile.
    void requestUpscaled(uint64_t ticket, const std::string &name, std::shared_ptr<const Texture> game);
    // (One per request: the large image, or none - `dropped`: the game's texture let go of before its turn,
    // else no large image passed the checks.)
    struct UpscaledResult
    {
        uint64_t ticket = 0;
        std::shared_ptr<Texture> large;
        bool dropped = false;
        // (TODO 219) No large image because the HD set has none of this texture YET (being made): asked for again when
        // the set has grown, not taken for a rejection.
        bool missing = false;
    };
    std::vector<UpscaledResult> takeUpscaled();
    // A game texture made ahead of its first draw, handed to the uploader at the next frame (nr_d3d.cpp).
    void preUpload(const std::shared_ptr<const Texture> &texture);
    // Whether the remaster has a large image of that name (its index built if not yet).
    bool hasUpscaled(const std::string &name);
    // (F1 > Graphics, "Open texture folder") The folder the HD images are loaded from (empty: none, TF_HIRES=0).
    std::filesystem::path hdTextureFolder();
    // (F1, "Sharp text") Whether the sharp font atlases are there (crisp_fonts	ext_ps5 or the others).
    bool crispFontsFound();
    // (TODO 259) Why the HD set is not used although it is there: its marker says it was set aside (made on a graphics
    // card whose HD images come out broken). "" when it is not set aside. Reads the marker: not for every frame.
    std::string hdSetAside();
    // The large images' loading since the last call (cache hits, PNGs decoded, compression time...): a profile line.
    std::string largeImageStats();

    // (TODO 27) The warp tunnel's sparks (nr_warp.cpp): sets of points in their own space, each drawn by the GPU through
    // its rows (local to GS screen: s = row0 * x + row1 * y + row2 * z + row3, kept where s.w < 0, the eye's side) as soft
    // dots added to the picture (nr_d3d.cpp drawCloud) - the PS2's single-pixel GS points, at the picture's resolution.
    struct PointCloud
    {
        struct Set
        {
            std::shared_ptr<const Mesh> points; // (resident: drawn from its first `count` vertices, x y z each)
            uint32_t count = 0;
            float rows[16] = {};
            float colour[4] = {}; // (added at a dot's centre, 0..1 of the picture's)
        };
        std::vector<Set> sets;
        float squeeze = 1.0f;  // (Hor+: GS x narrowed about 2048 by this - the 4:3 picture's width over the screen's)
        float sigma = 0.4f;    // (a dot's Gaussian radius, in units of the PS2's pixel at the picture's height)
    };

    // One RenderGeometrySet: the mesh with the model-to-screen matrix it was drawn with (the four
    // rows func_00221DB0 sends to VU1: screen = row0 * x + row1 * y + row2 * z + row3, then / w).
    struct Draw
    {
        std::shared_ptr<const PointCloud> cloud; // (TODO 27: the warp tunnel's sparks - drawn by drawCloud, no mesh)
        std::shared_ptr<const Mesh> mesh;
        std::shared_ptr<const Texture> texture; // the device's stage 0 texture (none: untextured)
        float matrix[16] = {};
        // The render target the GS was drawing into (FRAME_1's low word: base, width, format) and its
        // screen (XYOFFSET_1, SCISSOR_1): the frame buffer, or a texture (shadows, reflections, glare).
        uint32_t target = 0;
        uint32_t fbmsk = 0; // FRAME's high word: the bits the draw does not write (an alpha-only pass, say)
        uint32_t scanmsk = 0; // SCANMSK: 2 = odd lines left alone, 3 = even lines (a field drawn to its lines)
        uint64_t clamp = 0;   // CLAMP: how texture coordinates outside the texture wrap (WMS, WMT: repeat, clamp, region)
        bool grassPending = false; // the ground's grass, its blades grown on the renderer's thread (nr_thread.cpp)
        bool needsShader = false;  // its shader state taken on the renderer's thread: the list replayed up to listEnd
        bool fromList = false;     // its GS state (FRAME, TEX0, ALPHA...) the display list's registers where it is replayed (a microcode primitive)
        // The depth fog: an 8-bit-in-the-top-byte read of the draw's own depth buffer's pages (where
        // the game shuffles depth bits 8-15): drawn from the target's depth through this palette.
        std::shared_ptr<const Texture> fogPalette;
        // An 8-bit-in-the-top-byte texture (T8H)'s palette: when its pages are a render target this
        // frame drew (the shadow textures), the GPU side reads that target's green - what the game's
        // 16-bit copy moves into the top byte - through it, in place of GS memory.
        std::shared_ptr<const Texture> topPalette;
        uint32_t listEnd = 0;      // (words of the frame's list before the draw's call)
        // The GS state it was drawn with: the state block's ALPHA_1 (the blend), TEST_1 (alpha and
        // depth tests), TEX0_1 (TFX, TCC) and ZBUF_1 from the shadow (ZMSK: no depth writes).
        uint64_t alpha = 0, test = 0, tex0 = 0, zbuf = 0;
        uint64_t alpha2 = 0, test2 = 0, tex02 = 0; // context 2's (the lit meshes' base pass)
        uint64_t clamp2 = 0;                       // context 2's CLAMP (the water's reflection: CLAMP_2, the block's ninth register)
        // TEX1_1 / TEX1_2: the GS's texture filter and mip choice (LCM, MXL, MMAG, MMIN, L, K), as the display
        // list set them where the draw comes (SetBilinear, the eco quadtree's mip bias).
        uint64_t tex1 = 0, tex12 = 0;
        std::shared_ptr<const Texture> texture2;  // context 2's texture
        ShaderState shader;                       // the routines it ran and their data
        float originX = 0.0f, originY = 0.0f, width = 0.0f, height = 0.0f;
        float scissor[4] = {0.0f, 0.0f, 4096.0f, 4096.0f}; // SCISSOR_1: x0, y0, x1, y1 (inclusive), window pixels
        // (N9 stage 3) The frame before's pose, for the pictures between frames drawn from one recording (nr_thread.cpp
        // poses::assign): the GPU blends each matrix between it (0) and this frame's (1) by the picture's moment.
        // matrixB for `matrix`, clipB for the core's clipping matrix (0x0480's block), instB for each instance's matrix (16
        // floats an instance); none: the draw does not move between frames. `turned`: the camera's turn of each picture
        // (the newest mouse and stick) applies to it - the scene's 3D.
        bool hasB = false, hasClipB = false, turned = false;
        bool sceneMotion = false; // (TODO 299) the scene's 3D for the temporal upscaler: jittered, its motion written (poses::assign)
        uint8_t camera = 255; // (N9's turn for every picture: the camera matrices it was made with - view_turn.h's index)
        float matrixB[16] = {}, clipB[16] = {};
        std::vector<float> instB;
        // (N9 stage 3) A mesh the game makes anew every frame - the grass's blades, a microcode primitive's (beams,
        // glows), the ocean's patch - matched to the frame before's of its kind: its vertices then (place, colour, size),
        // blended to this frame's by the picture's moment, as the matrix is (none: drawn as it is now).
        std::shared_ptr<const Mesh> meshB;
    };

    // A frame: every draw between two BeginFrames; the picture is the target the frame buffer is
    // (render() picks it: the largest screen) and how GS screen space maps to it.
    struct Frame
    {
        // The display list's stretches before each 3D draw (guest addresses: the game's packet
        // buffer, double buffered - intact until the frame after next), gathered, parsed and
        // replayed on the renderer's thread (nr_capture.cpp: expandFrame).
        struct Range
        {
            uint32_t from, to;
            uint32_t before; // the 3D draw it comes before (draws.size(): after the last)
            uint64_t tableChanges = 0; // the texture tables' changes logged before it (nr_capture.cpp: the uploads' order)
        };
        std::vector<Range> ranges;
        microcode_prefix::Prefix interframeMicrocode; // completed native uploads before this capture
        // (N3b) GS packets that reached the GS outside the game's frames (a load screen drawn on its own): its qwords,
        // read on the renderer's thread like the list's 2D (nr_capture.cpp looseGif).
        std::vector<uint32_t> looseGif;
        bool loose = false; // (such a frame: its picture is the target its last draw went to - nr_d3d.cpp)
        // (TODO 120) The scene's first frame after a load the game drew on its own (looseGif): its 3D left out - the
        // GS shows black there (the VU1 work of that frame draws nothing yet), the native one drew it from an
        // unset camera and lights (nr_capture.cpp endFrame).
        bool firstAfterLoad = false;
        bool has3D = false; // a scene drawn (geometry sets), not only 2D (menus, movies)
        // Captured with this frame, never read from live game state on the render thread.
        bool synchronousGameTextures = false;
        std::vector<uint32_t> list; // the display list's words up to each draw (the game's DMA chain), replayed on the renderer's thread
        // (TODO 146) The rain's blocks, one per pass in order (taShader_Rain::LoadContext's VU data - headers, transforms,
        // the patches' positions - copied before the pass's list stretch is flushed and its memory written over: the
        // renderer's thread reads the list's stretches long after, nr_rain.cpp). Four words a qword.
        std::vector<std::vector<uint32_t>> rainBlocks;
        // (N5.1) The GS registers written through the device's calls - SetGSRegister, its pair, the state blocks applied -
        // in their order among the draws: the register state the renderer takes instead of the display list's
        // (nr_capture.cpp expandFrame; TF_N5_NOLIST).
        struct RegWrite
        {
            uint32_t before; // the 3D draw it comes before (as Range::before)
            uint32_t reg;
            uint64_t value;
        };
        std::vector<RegWrite> regStream;
        // (N5.3) The registers the device's native functions set - they write no packet: applied to the list's GS state
        // where their packet would have been (`at`: the packet pointer when they ran - nr_capture.cpp expandFrame).
        struct NativeWrite
        {
            uint32_t before; // the 3D draw it comes before
            uint32_t at;     // the list position (D_70000D30, its 0x1FFFFFFF part)
            uint32_t reg;
            uint64_t value;
        };
        std::vector<NativeWrite> nativeRegs;
        // (N5 step 2 part 2, TODO 94: TF_N5_CONTEXTS) The shaders' contexts the game wrote into the list (0x23AA90's
        // LoadContexts, 0x221DB0's matrix packet), taken out of it where they were written (the packet pointer put back)
        // and kept here as the VIF words the list's gather would have read: one record per call - 5 header words (the 3D
        // draw it comes before, the list stretch it falls in, the list position it stood at, where it ended, its word count) then its words. The
        // renderer splices each into the list's words where it stood (nr_capture.cpp expandFrame). Reused frame to frame
        // (recycleFrameBuffers / takeFrameBuffers).
        std::vector<uint32_t> vuChunks;
        float displayAspect = 0.0f; // the picture's shape on the screen (ps2_host_display): 4:3, 16:9, the window's
        float overlaySqueeze = 1.0f; // the 2D's narrowing in widescreen (ps2_host_display): (4/3) / the picture's aspect
        uint64_t tick = 0;              // the VBlank count when the frame was handed over
        int64_t dueNs = 0;              // when that VBlank was due (steady clock, ns): the frame's moment, which its picture is paced by (N9)
        int64_t periodNs = 0;           // (N9) the VBlank period then (ns), and whether the VBlanks are timed by the screen's
        bool vblanksOnRefresh = false;  // refresh (a clock of its own, a whole number of refreshes a VBlank: ps2_host_display)
        // (N9 stage 2) The screen's refresh period and one of its vertical blanks (steady clock, ns; 0: not known), and
        // whether its refresh is variable (FreeSync, G-Sync): the pictures between frames are made for its refreshes.
        int64_t refreshNs = 0, refreshVblankNs = 0;
        bool variableRefresh = false;
        // (N9 stage 2) Where the in-between field's pose lies between the frame before's (0) and this frame's (1): the
        // governor's factor (0x6CD6F0) when the frame was drawn - each 3D draw's field B matrix is that pose.
        float fieldBMoment = 1.0f;
        // (N9 stage 2) A picture between frames: its period (the pictures' spacing; 0 = the frame's), which the paced
        // hand-over keeps whole on a screen of fixed refresh.
        int64_t pictureNs = 0;
        // (N9 stage 3) The pictures to draw of this frame from one recording, in order: each its moment (steady clock, ns),
        // where it lies between the frame before's pose (0) and this frame's (1), and a file to save it to (a checking aid).
        // Empty: one picture, this frame's pose, at dueNs.
        struct Picture
        {
            int64_t dueNs = 0;
            float s = 1.0f;
            std::string shot;
        };
        std::vector<Picture> pictures;
        // (N9: the camera's turn for every picture, view_turn.h) The frame's camera as the game had it, and the index of
        // the scene's camera matrices (the turned draws'; -1: none turned).
        std::shared_ptr<const view_turn::FrameCamera> turnCamera;
        int turnMain = -1;
        bool turnWaits = false; // (extra pictures ahead of their moments - this frame's pose, the turn newer: they wait for it)
        int turnLeadSet = 0;    // (the turn's lead measured apart for pictures between frames (1) and the rest (0))
        // (N9 stage 2, TODO 88) A movie plays (tf::port::moviePlaying); and a frame of 2D alone - a menu - is shown as the
        // native renderer draws it (the pictures between the menus' frames), not as the emulated GS's.
        bool movie = false;
        bool shown2D = false;
        // (TODO 161 a) A frame not shown: the renderer fell behind the game's fixed logic rate (or the frame rate cap),
        // and a newer frame waits - its picture left out, never the game's time. Its draws into the targets that newer frame
        // draws (`superseded`: target keys, FBP | FBW << 9; the picture's always) are left out too; the rest - a render-to-
        // texture the next frames read and do not draw again - still drawn (nr_thread.cpp, nr_d3d.cpp).
        bool dropped = false;
        std::vector<uint32_t> superseded;
        bool droppedDrawn = false; // (TF_DROP_KEEP=all, a checking aid: a frame left out drawn in full all the same, only not shown)
        float originX = 2048.0f - 256.0f, originY = 2048.0f - 224.0f; // GS coordinates of the top-left pixel (XYOFFSET)
        float width = 512.0f, height = 448.0f;                          // the frame's size in GS pixels (SCISSOR; shown 4:3)
        // The display circuits (PMODE, DISPFB1/2, DISPLAY1/2, BGCOLOR): how the picture is made of
        // the frame buffers (the 3D field and the HUD's buffer, merged by the PCRTC).
        uint64_t pmode = 0, dispfb[2] = {}, display[2] = {}, bgcolor = 0;
        float depthScale = 1.0f / 4294967296.0f;                        // GS Z (Z32, ZBUF) to 0..1 (larger = nearer)
        std::vector<Draw> draws;
    };

    // A geometry set's DMA/VIF packet read as data into `mesh` (nr_vif.cpp); `dump`: its commands
    // and batches as text.
    void decodePacket(const uint8_t *rdram, uint32_t start, Mesh &mesh, FILE *dump);
    // A microcode primitive's strips (vlMicrocodePrimitive's strip calls, 0x220530 / 0x220A10: the warp
    // beams, the engines' glow cones) from the display list's words it wrote - batches of three qwords a
    // vertex (ST, colour, position) with their drawing-kick masks, a header of the count and 0x8000.
    void decodePrimitiveStrips(const uint32_t *words, size_t count, Mesh &mesh, FILE *dump);

    // What a draw samples, from its TEX0 (the state block's) and the uploads that put the texture's
    // levels in GS memory: the vlTexturePS2, its image level, its palette level (indexed formats),
    // the sampling format and size, and a 4-bit palette's offset (CSA).
    struct TextureKey
    {
        uint32_t texture = 0, image = 0, clut = 0;
        uint32_t psm = 0, width = 0, height = 0, csa = 0;
        uint32_t tbw = 0; // TEX0's buffer width (64 pixels a unit)
        uint32_t tbp = 0; // TEX0's base block (a GS-resident image is read from there)
        // The game's own mip levels below the image (vlTexturePS2's levels after its image level,
        // count at +0x54: what TEX1's MXL reads at MIPTBP1), each half the size of the one above; 0 = none.
        uint32_t mip[3] = {};
        bool operator==(const TextureKey &o) const
        {
            return texture == o.texture && image == o.image && clut == o.clut && psm == o.psm && width == o.width &&
                   height == o.height && csa == o.csa && tbw == o.tbw && tbp == o.tbp && mip[0] == o.mip[0] && mip[1] == o.mip[1] &&
                   mip[2] == o.mip[2];
        }
    };

    // The texture's image as RGBA8 (nr_texture.cpp); null when it is not readable as one (no texels
    // in main memory, an unknown format).
    // A level's texels kept as they were uploaded (the game frees the copy in main memory of some
    // once they are in GS memory: the light maps kept in a buffer's top byte, say).
    void rememberLevel(const uint8_t *rdram, uint32_t level);
    // How many times a level was uploaded again with other texels (a movie's frames, an animated
    // palette): the converted textures of it are stale when it moves (nr_capture.cpp convertKey).
    uint32_t levelGeneration(uint32_t level);
    // (TODO 94) Counts every levelGeneration change: unchanged, no level's generation changed (convertKey skips asking).
    uint64_t snapshotEpoch();

    // GS memory, for the images that live only there (uploaded once from a buffer since freed: the
    // light maps in the display buffers' top bytes). A stopgap until they are read from the level's
    // own files; null = none.
    void setGsMemory(const uint8_t *vram);
    // (N3b) A texture read straight from GS memory as TEX0 says (nr_texture.cpp): what the game uploaded there itself.
    std::shared_ptr<Texture> textureFromGsMemory(uint64_t tex0, uint64_t id);
    // (N3b) The display circuit's buffer read from GS memory: the movies the game DMAs into it (nr_capture.cpp).
    bool gsDisplayedPicture(std::vector<uint32_t> &pixels, uint32_t &width, uint32_t &height);
    bool gsDisplayFromFrames(); // (N3b) the display shows what the game's frames showed (nr_capture.cpp)
    bool outsideFrames();       // (N3b) the game draws a screen of its own now - no frames, the display switched
    void looseGif(const uint8_t *data, uint32_t bytes); // (N3b) GS packets outside the game's frames (nr_capture.cpp)
    void noteLooseFrame();                              // (TODO 27: a picture of a load screen handed over - TODO 120's count)
    uint64_t vblankTick();                              // (TODO 27: the GS's VBlank count now; 0 before the runtime)
    // (The emulation census, TF_VU1_CENSUS) What reaches or reads GS memory, counted (any thread): kind 0 a level upload
    // (amount = its bytes), 1 a texture read from GS memory for a level with no texels in main memory (the light maps:
    // amount = pixels), 2 a texture read from GS memory as TEX0 says (looseGif, dumps). censusLine: the counts since
    // the last call, as text.
    void censusNote(uint32_t kind, uint64_t amount);
    std::string censusLine();

    // (TODO 27) The warp tunnel of a load drawn natively (nr_warp.cpp): the game's VU1 program for it left out, its
    // pictures made here from the real clock at the screen's rate. TF_NATIVE_WARP=0: the game's program and the GS's
    // packets drawn as a load screen, as before.
    namespace warp
    {
        bool active();    // a tunnel is shown natively now (the load screen's GS packets are not drawn)
        void gameFrame(); // the game drew a frame of its own (capture): the tunnel's pictures stop
    }
    // (Debug: a region of GS memory written as a PPM; TF_NATIVE_GSDUMP, nr_capture.cpp.)
    void dumpGsRegion(const char *path, uint32_t tbp, uint32_t tbw, uint32_t psm, uint32_t w, uint32_t h);

    namespace d3d
    {
        // The window's height in pixels (the window's thread, nr_present.cpp, each frame): the
        // native picture is drawn with that many lines unless TF_NATIVE_HEIGHT says otherwise.
        void setWindowLines(int lines);

        // The large images' bytes on the GPU now, and the adapter's video memory (its dedicated memory, MB; 0: not
        // known) - the figure the video-memory policy (the quality preset, the large images' cap and budget: TODO 94,
        // nr_policy.h) is made of. Any thread.
        uint64_t largeImageBytes();
        uint64_t adapterMemoryMB();

        // The anti-aliasing pass (TF_NATIVE_AA, default FXAA): the 3D picture's edges smoothed before the
        // HUD, menus and text are drawn over it - 0 off, 1 FXAA, 2 SMAA 1x. Any thread, takes effect next frame.
        void setAntiAliasing(int mode);
        // Supersampling (TF_NATIVE_SSAA, default off): the 3D drawn at 1.5x the picture pixels each way, averaged
        // down into it. Any thread, takes effect next frame.
        void setSupersampling(bool on);

        // (N9 stage 2) A picture's time on the GPU (ms, smoothed; 0: not measured yet) - the pictures between frames' cap.
        double gpuPictureMs();
        // (N9 stage 3) An extra picture of a frame - drawn again from the frame's recording - on the GPU (ms, smoothed; 0:
        // none measured yet).
        double gpuExtraMs();
        // (N9 stage 3) The render thread's time a picture once the frame's list is recorded (ms, smoothed).
        double cpuPictureMs();
        // (d3d553) The last render call's time waiting for its extra pictures' turns (ms; render thread): the GPU's time
        // (an extra picture runs the frame's list again only once the one before finished), not the render thread's work.
        double replayWaitMs();
        // (N9 stage 3) The render thread's time waiting for a frame slot so far (ms, all of it): the GPU behind the frames.
        double slotWaitMs();
        // (N9 stage 3) The pacer's delay after a picture's moment (ms): how late the GPU's pictures are ready.
        double pacerDelayMs();

        // The latest picture drawn (RGBA8, R in the low byte), copied into `pixels` when newer than
        // `serial` (which is then updated); false when there is none newer. (Any thread.)
        bool latestPicture(std::vector<uint32_t> &pixels, uint32_t &width, uint32_t &height, uint64_t &serial, bool &has3D);

        // The device, queue, shaders and pipeline made on a thread of their own (seconds: the
        // shaders' compilation), so the game never waits for them; frames before are not drawn.
        void prepare();
    }

    // A finished frame to the renderer's thread (nr_thread.cpp; TF_NATIVE_THREAD=0: drawn at once).
    void submitFrame(Frame &&frame, const std::string &shot);
    // (TODO 94, the capture's cost) A frame's draws and list vectors back from the renderer once it is done with them:
    // their memory kept for the game's next frames (nr_thread.cpp) - a fresh megabyte or two each frame was page faults
    // the game's thread paid for as it first wrote each draw. takeFrameBuffers: the largest kept ones (empty, their
    // capacity kept), else none.
    void recycleFrameBuffers(Frame &frame);
    void takeFrameBuffers(std::vector<Draw> &draws, std::vector<uint32_t> &list);
    void takeChunkBuffer(std::vector<uint32_t> &chunks); // (N5 step 2 part 2: Frame::vuChunks' memory)
    // (TODO 112, TF_HITCH_MS) The game's thread's time waiting for the renderer in submitFrame so far (ms, all of it);
    // the large images' loaders now (queued requests, PNGs being decoded/compressed) and so far (chains compressed
    // into the cache, PNGs decoded); pipelines made so far (on the render thread and the variants' thread).
    double gameWaitMs();
    void largeImageActivity(unsigned &queued, unsigned &heavy, uint64_t &compressed, uint64_t &pngs);
    // (TODO 124) The requests finished so far: how many, their summed time in the queue and in a loader's work (and
    // of those read from the compressed cache: how many, their work), microseconds.
    void largeImageTimes(uint64_t &done, uint64_t &queueUs, uint64_t &workUs, uint64_t &doneCache, uint64_t &workCacheUs);
    uint64_t pipelinesMade();
    // A draw's last decisions once its shader state is known (sprites, grass: nr_capture.cpp).
    void finishDraw(Draw &d);
    // The frame's display list read (its 2D draws, the VU image, the 3D draws' shader states), on
    // the renderer's thread; the 2D draws placed among the 3D ones in the list's order.
    void expandFrame(Frame &frame);
    bool renderOnThread();
    // Whether frames are kept and read whole at their end (expandFrame) - on the renderer's thread or, with
    // TF_NATIVE_THREAD=0, the game's; false only for TF_NATIVE_THREAD=old (each stretch read as it is sent).
    bool framesDeferred();
    // (N5.3, game thread) A GS register a native device function set instead of writing its packet: into the frame's
    // native registers (applied where the packet would have been) and the register stream (nr_capture.cpp).
    void deviceRegister(const uint8_t *rdram, uint32_t reg, uint64_t value);

    // The native picture shown live in the window (nr_present.cpp): TF_NATIVE_LIVE=1.
    void startLivePicture();

    std::shared_ptr<const Texture> convertTexture(const uint8_t *rdram, const TextureKey &key, uint64_t id);
    // (TODO 164) What convertTexture reads of guest memory for a key - the levels' headers, texels and palette -
    // copied, so a worker converts it while the game goes on (convertTextureFrom). Null: not to be copied (a level
    // read from GS memory, the light maps; no texels): converted at once, as before.
    struct TextureInputs;
    std::shared_ptr<TextureInputs> captureTextureInputs(const uint8_t *rdram, const TextureKey &key);
    std::shared_ptr<const Texture> convertTextureFrom(const TextureInputs &inputs, const TextureKey &key, uint64_t id);
    // (TODO 164) A hash of a texture's texels and its game mips (8 bytes at a time): the large images' contentKey.
    uint64_t pixelHash(const Texture &t);

    // The texture's upscaled image in place of its pixels, found by its name and confirmed by its
    // content (nr_hires.cpp); false when there is none (the texture is left as it is).
    bool replaceWithUpscaled(const std::string &name, Texture &texture);

    // A pass the GPU draws (nr_passes.cpp): a draw is one or more (the lit metal two, instances one
    // each), with what its routines compute as modes and parameters.
    struct Pass
    {
        float matrix[16] = {};
        float matrixB[16] = {}; // (N9 stage 3: the frame before's pose of `matrix` - the same when the draw does not move)
        float matrixM[16] = {}; // (TODO 101 a: the pose halfway between them along its turn, midPose - the GPU's curve runs through it)
        std::shared_ptr<const Texture> texture;
        std::shared_ptr<const Texture> fogPalette; // (the depth fog: nr_types Draw::fogPalette)
        std::shared_ptr<const Texture> topPalette; // (a T8H read of a render target: Draw::topPalette)
        uint64_t tex0 = 0, alpha = 0, test = 0, zbuf = 0;
        uint32_t prim = 0;
        uint32_t fbmsk = 0;
        uint64_t clamp = 0;
        uint64_t tex1 = 0; // TEX1 (the pass's context)
        float mode[4] = {}; // colour, ST, alpha
        float p[24][4] = {};
        // The core's clipping (0x1410 with q21.x above 0 calls 0x0480): the position through 0x0480's q0-q3,
        // the triangle clipped to |x|, |y|, |z| <= w there - its near plane, w ~ 0.5 in a level, is the PS2's.
        bool clips = false;
        float clip[4][4] = {};
        float clipB[4][4] = {}; // (N9 stage 3: the frame before's pose of it)
        float clipM[4][4] = {}; // (TODO 101 a: halfway, as matrixM)
        float lightsB[3][4] = {}; // (N9 stage 3: p[8..10] - the lights and eye in the mesh's space, 0x21f0 - in that pose)
        bool turned = false;    // (N9 stage 3: the camera's turn of each picture applies - Draw::turned)
        bool sceneMotion = false; // (TODO 299: Draw::sceneMotion)
    };
    void buildPasses(const Draw &draw, std::vector<Pass> &out);
    // (TODO 101 a) The pose halfway between a draw's two (16 floats, rows: the images of the model's axes and its origin):
    // the axes turned by half the turn between them - not their average, which shortens an axis that turns - the origin
    // halfway. False when the turn is over 60 degrees in a frame: no pose of the same thing (a part snapped round), to be
    // drawn as it is now. Degenerate rows (an axis scaled away) give the average.
    bool midPose(const float *b, const float *a, float *m);

    // The grass the ground's second draw grows (nr_grass.cpp): blades as screen-space sprites.
    std::shared_ptr<Mesh> grassBlades(const Mesh &ground, const Qword *block, uint64_t id);
    // (grassgpu) The same blades to be grown on the GPU: a mesh with no vertices, its GpuGrass to the GPU's compute pass.
    std::shared_ptr<Mesh> grassBladesGpu(const std::shared_ptr<const Mesh> &ground, const Qword *block, uint64_t id, bool check);
    // The ground mist's drifting streaks (vlShader_Drift, the grass image's 0x0480; nr_grass.cpp).
    std::shared_ptr<Mesh> driftStreaks(const Mesh &ground, const Qword *block, uint64_t id);

    // The Direct3D 12 side (nr_d3d.cpp).
    namespace d3d
    {
        bool start();                                   // the device and the picture (false: no D3D12)
        bool drawsPictures();                           // Direct3D 12 made ready on its thread and drawing (N3b)
        // (TODO 169 a) On the render thread: waits until the GPU can start the next frame - the slot it will draw into
        // given back (its frame two before done) - and returns the ms waited (0 when it could at once, or not drawing).
        double waitForFrameSlot();
        bool deviceLost();                              // the device removed (a GPU hang): nothing more drawn, the GS's picture back
        bool grassGpuReady();                           // (grassgpu) the grass compute pass made: blades may be grown on the GPU
        // Draws a frame into the picture; with `shotPath`, saves the picture there (BMP).
        void render(const Frame &frame, const std::string &shotPath);
        // (TODO 94, a checking aid - TF_NATIVE_SHOTAB=1: a shot frame drawn twice, the speed-ups that keep the picture -
        // shader variants, instancing, the state filter - off (<shot>_a.bmp), then on (_b.bmp); -1: as the settings say.)
        void setSpeedupsForced(int on);
    }
}
