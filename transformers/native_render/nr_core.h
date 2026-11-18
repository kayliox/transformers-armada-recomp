// The native renderer's core (nr_core): the part that builds and runs without the PS2 runtime (no ps2_memory, ee_scheduler,
// gs/*, vu1, ps2_settings, raylib, view_turn, ps2_host_input, no guest addresses). Lane B's spec: runs\handover_2026-11-15\
// NR_CORE_SPEC.md. The recomp keeps its glue (nr_capture, nr_vif, nr_gif, the GS replay in nr_frame, ...) on top of it.
//
// S0 (this file): the build gate. The core's sources are compiled as the library nr_core (CMake: native_render/nr_core.cmake)
// with an include path that cannot see src/, decomp/ or the runtime, so a stray include fails at compile time. What the
// core needs from the outside comes in through small hooks the glue sets at start-up, declared here.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace nr::gfx
{
    class Device; // (nr_device.h: the device a recorded frame draws through - S10's submit)
}

namespace nr_core
{
    // The threads the core starts say what they are; the PC layer decides where they run (the recomp: PS2X_THREAD_PLACEMENT,
    // ps2_thread_placement). Values mirror ps2_thread_placement::Role (Windows laptops' placement, TODO 290).
    enum class ThreadRole : int
    {
        Ee = 0,
        Vif1,
        GsFeed,
        RenderRead,
        Render,
        Helper, // HD texture uploads, sound mixer, movie decoder
        Count
    };

    using ThreadPlacer = void (*)(ThreadRole);

    // What the texture conversion (nr_texture.cpp) needs of its host: a main-memory pointer (guest RAM) and the GS upload skip's
    // bookkeeping (PS2Recomp's gsupload: the checks and the work that complete a texture's pages before they are read). A host
    // with neither leaves the hooks null: reads of guest memory give nothing, the checks see nothing stale.
    struct TextureHost
    {
        // The pointer to [addr] in guest RAM (null: not mapped). `ram` is the caller's base pointer, as the host gave it.
        // (nr_core S7: read only by the recomp's guest-memory entry, nr_texture_guest.cpp; the core's own entry takes host
        // memory - nr::convertHostTexture.)
        const uint8_t *(*guestPtr)(const uint8_t *ram, uint32_t addr) = nullptr;
        // gsupload::ensureWhole(first, last): the uploads a skip left out that reach into the bytes written first.
        void (*ensureWhole)(uint64_t first, uint64_t last) = nullptr;
        // gsupload::mode() == Check
        bool (*checkMode)() = nullptr;
        // gsupload::staleAt(address, topByte)
        bool (*staleAt)(uint32_t address, bool topByte) = nullptr;
        // gsupload::reportRead(what, tbp, psm, staleBytes, bytes)
        void (*reportRead)(const char *what, uint32_t tbp, uint32_t psm, uint32_t staleBytes, uint32_t bytes) = nullptr;
        // (nr_core S5) Whether the level whose texels are at `data` (a guest address) holds a movie's picture drawn from the
        // decoder, its game copy not written (nr::movie::skipsSnapshot): its snapshot not kept.
        bool (*skipsSnapshot)(uint32_t data) = nullptr;
        // (nr_core S5, TF_VU1_CENSUS) A texel read counted by kind (nr::censusNote).
        void (*censusNote)(uint32_t kind, uint64_t amount) = nullptr;
    };
    void setTextureHost(const TextureHost &host);
    const TextureHost &textureHost();

    // What the HD texture pack (nr_hires.cpp) needs of its host: a PNG / image file as RGBA8 (the PC layer's image library),
    // the prompt-style questions (which controller / keyboard the game's button pictures should follow) and the developer's own
    // asset tree (the private build's Extracted folder, looked in last). Left null: no images load, the console's prompts show,
    // no developer tree.
    struct HiresHost
    {
        // An image as RGBA8 (R in the low byte); false when it does not load.
        bool (*loadImage)(const char *path, uint32_t &width, uint32_t &height, std::vector<uint32_t> &pixels) = nullptr;
        bool (*keyboardPromptsShown)() = nullptr; // key_text::symbolsAsKeys(ps2_host_input::keyboardPromptsShown())
        bool (*lastPadIsPlayStation)() = nullptr; // ps2_host_input::lastPadStyle() == PlayStation
        bool (*keyboardPrompts)() = nullptr;      // ps2_host_input::keyboardPrompts()
        bool (*xbox360Pads)() = nullptr;          // ps2_host_input::xbox360Pads()
        const char *(*applicationDirectory)() = nullptr; // the folder of the executable (raylib's GetApplicationDirectory)
        std::string devTree;                      // the developer's asset tree (empty: none)
    };
    void setHiresHost(const HiresHost &host);
    const HiresHost &hiresHost();

    // Set once by the host before the core starts a thread (null: no placement).
    void setThreadPlacer(ThreadPlacer placer);
    // From the thread itself, as it starts.
    void placeThisThread(ThreadRole role);

    // ---- S2a: opening the core and its settings (NR_CORE_SPEC.md 2), a frame's in and out (3, 4), the window's events (8) ----
    // The graphics API: the default is Direct3D 12 where the build has it (else none: the host draws); Vulkan is in development.
    enum class Api : int
    {
        Default = 0,
        Vulkan = 1
    };
    // The core's settings, a plain struct in place of the recomp's ps2_settings. A field left at -1 is not changed.
    struct Settings
    {
        int api = -1;            // Api: open() only (chosen once); -1 = the TF_GFX_API environment variable, as before
        int antiAliasing = -1;   // the 3D's edges: 0 off, 1 FXAA, 2 SMAA 1x
        int supersampling = -1;  // 1: the 3D drawn at 1.5 times the window's size each way and averaged down
        int speedupsForced = -1; // the picture-keeping speed-ups forced off (0) or on (1) - a testing aid (TODO 94)
        int windowLines = -1;    // the picture drawn at the window's own lines (Windows)
        int windowWidth = -1, windowHeight = -1; // (Linux: the window's size)
        int sharedPictures = -1; // 1: the pictures handed to the window as shared GPU textures, 0: through the CPU
        // (S11) open() only, read when the device is first prepared (the first open()); applySettings leaves them alone.
        int livePictures = -1;   // 1: each picture handed over (to the window, to present) as its frame is drawn, 0: not;
                                 // -1 = the TF_NATIVE_LIVE environment variable, as before
        int corePacing = -1;     // how the core hands the pictures over: 0 at once (the host paces), 1 as soon as the GPU is done,
                                 // 2 paced (each at its frame's moment plus a steady delay); -1 = TF_PACING, as before (unset: 2)
    };
    // Chooses the API (once: the first call or the first use of the backend decides) and starts preparing the device, its
    // shaders and pipelines on a thread of their own; then the settings.
    void open(const Settings &settings);
    // The settings' fields that are not -1, through the backend (any thread; from the next frame).
    void applySettings(const Settings &settings);
    // (S12) What an overlay (a menu, a debugger: drawn over the picture, e.g. by ImGui) is handed when the core presents: the
    // window's back buffer with the picture already in it, before it is saved (PresentRequest::shot) and presented. Direct3D 12
    // objects as plain pointers so the core's header needs no Direct3D: the device and its queue, the open command list the
    // present records on (the back buffer bound as its only render target, in D3D12_RESOURCE_STATE_RENDER_TARGET), the back
    // buffer itself and its render target view (a D3D12_CPU_DESCRIPTOR_HANDLE's ptr), its size and DXGI_FORMAT, and which of the
    // swap chain's buffers it is: whatever the overlay used when it was last called for buffer `buffer` is free again (the core
    // waited for the GPU to finish with that buffer before calling it). The overlay records draws only (no Close / Execute); it may
    // set its own root signature, pipeline, descriptor heaps, viewport, scissor and render target, and must leave the back
    // buffer in the render target state - the core sets nothing it needs again after it.
    struct OverlayTarget
    {
        void *device = nullptr;      // ID3D12Device *
        void *queue = nullptr;       // ID3D12CommandQueue *
        void *commandList = nullptr; // ID3D12GraphicsCommandList *
        void *target = nullptr;      // ID3D12Resource *: the back buffer
        uint64_t rtv = 0;            // D3D12_CPU_DESCRIPTOR_HANDLE::ptr of the back buffer's render target view
        uint32_t width = 0, height = 0;
        uint32_t format = 0;         // DXGI_FORMAT
        uint32_t buffer = 0, buffers = 0;
    };
    // An overlay to draw at one present (PresentRequest::overlay, FrameInfo::overlay), called on the presenting thread (the
    // host's present thread; a submitted frame's: the core's drawing thread). Empty: none.
    using Overlay = std::function<void(const OverlayTarget &target)>;

    // (S13a) A layer: a texture of the core's (RGBA8, premultiplied alpha) that ANOTHER API of the host draws into - the recomp's
    // GL window draws its raylib overlays into such layers through their shared handles (EXT_memory_object_win32) - and that
    // layerOverlay() draws over a present's back buffer. kLayers of them, by index, made on the renderer's device as shared
    // textures (an NT handle the caller does not close) at the size asked, made again when the size changes (a new generation:
    // the host imports it again).
    constexpr int kLayers = 32;
    struct Layer
    {
        void *handle = nullptr; // the NT handle (the core's)
        uint64_t size = 0;      // its allocation size
        uint32_t width = 0, height = 0;
        uint64_t generation = 0;
    };
    // Layer `index` at width x height (made or made again), returned once the GPU is done with every present that drew it, so it
    // may be drawn into now. The host's own drawing into it must be done before the present that reads it (GL: glFinish). False:
    // the renderer's device is not ready, or the texture was not made. From the host's present thread.
    bool layer(int index, uint32_t width, uint32_t height, Layer &out);
    // An overlay that draws layer `index` over the target with premultiplied alpha (rgb + target * (1 - a)): one texel a pixel
    // when the sizes match (else stretched, nearest); `bottomUp`: its rows bottom up (GL's). Called only by a present (its
    // PresentRequest::overlay, or from within one); nothing when the layer is not made. At most 8 layers in one present.
    Overlay layerOverlay(int index, bool bottomUp = true);
    // The layers let go (after the presents that drew them).
    void releaseLayers();

    // A frame as the host hands it over (S3 on: the core's frame queue) and its picture as it comes out (the swap chain's side,
    // S5: the core presenting its own window). Declared here so the hosts can be written against them.
    struct FrameInfo
    {
        uint64_t tick = 0;                          // the frame's moment (the recomp: its VBlank)
        uint32_t pictureWidth = 0, pictureHeight = 0; // the picture's size
        // (S10) The rest is read by submit() alone (the recomp's frames, nr::queueFrame, carry their own).
        float aspect = 0.0f;       // the shape the picture is shown at, width / height (0: its own)
        int64_t dueNs = 0;         // the frame's moment on the steady clock, ns, for the device's timing figures (0: none)
        std::string shot;          // the frame's picture saved here once drawn (BMP, the device's saveBmp); empty: not saved
        bool waitForGpu = false;   // the drawing thread waits for the GPU to finish the frame before going on (simple; slower)
        // Presented into the core's window (nr_core::present) once drawn: -1 when a window is in use, 0 never, 1 always (no
        // window or no shared pictures: not presented, said once). Presenting needs the pictures handed over as shared textures
        // AT ONCE: open() with Settings::sharedPictures = 1, livePictures = 1 and corePacing = 0 (the host paces) - S11; before
        // it, TF_NATIVE_LIVE=1 and TF_PACING=0 in the environment, which still do it when those fields are left at -1.
        int present = -1;
        int syncInterval = 0;      // PresentRequest's: 0 at once (tearing where allowed), 1..4 vsync
        int upscaler = 0;          // PresentRequest's: 0 a bilinear stretch, 1 FSR 1
        float sharpness = 0.2f;    // PresentRequest's (FSR 1's RCAS)
        float clear[3] = {};       // the bars' colour
        std::string windowShot;    // the window's picture as presented saved here (BMP); empty: not saved
        Overlay overlay;           // (S12) PresentRequest's: drawn over the picture before it is presented (and saved)
        // Called on the drawing thread once the frame is through: drawn (recorded and ended) and presented. May be empty.
        std::function<void(bool drawn, bool presented)> done;
    };
    struct FrameEnd
    {
        uint32_t pictureTarget = 0;   // the target the frame's picture is
        int displayRect[4] = {};      // x, y, width, height of it shown
        float aspect = 0.0f;          // the shape it is shown at (width / height)
    };

    // ---- S10: a frame the host records itself (NR_CORE_SPEC.md 3), beside the recomp's nr::queueFrame(Frame) ----
    // What the recording is given beside the device. The core keeps one picture target (gfx::TargetId: RGBA8, no depth, shared,
    // made again when the frame's size changes, NOT cleared - clear it or draw all of it); `end` says what is shown: preset to that
    // target, all of it, at FrameInfo::aspect - a recording may name another target it made with TargetDesc::shared, a part of
    // it, another shape.
    struct Recording
    {
        uint32_t picture = 0;           // the core's picture target at the frame's size
        uint32_t width = 0, height = 0; // the frame's picture size (FrameInfo's)
        uint64_t tick = 0;
        uint32_t slot = 0;              // the device's frame slot being recorded (0 or 1: what a slot's own resources key on)
        FrameEnd end;
    };
    // The recording: device calls only (gfx::Device, nr_device.h - state, targets, textures, rings, draws), made on the core's
    // drawing thread between the frame's start and end (prepareFrame + beginFrame before it, endFrame after it: not called by
    // the recording). Its captures are let go of on that thread too, once the frame is drawn.
    using Record = std::function<void(nr::gfx::Device &device, Recording &recording)>;
    // A frame handed to the core (the host's game thread; one thread): queued for the core's drawing thread (the one that draws
    // the recomp's frames - nr_queue.cpp's, started by the first frame), which starts the device when it gets to it (waits for
    // open()'s preparing, at most 60 s), records it through `record`, ends it, then presents it (FrameInfo::present). Waits
    // while two submitted frames are waiting to be drawn (the back-pressure: at most one more beside the one being drawn).
    // False: not queued - the device failed or was lost, no size, no recording.
    bool submit(Record record, const FrameInfo &info);
    // Waits until every frame submitted so far is drawn (and presented). From the submitting thread.
    void flushSubmitted();
    // flushSubmitted, then the core's picture target and its swap chain let go of on the drawing thread. Before closeWindow()
    // when the host is done with its frames (another submit after it makes them again).
    void endSubmits();
    // Waits (at most `timeoutMs`) for open()'s preparing of the device to end, then starts it if it is not (Backend::start):
    // true when it draws. For a host that wants to know before its first frame (submit does not need it).
    bool ready(int timeoutMs);
    // (S9a) A key as the keyboard reported it (raw input, Windows WM_INPUT): the scan code, the virtual key Windows gives it,
    // pressed or let go, and whether it is an extended key (E0 / E1 prefix: the right Ctrl and Alt, the arrows' own block...).
    struct KeyEvent
    {
        uint16_t scanCode = 0;
        uint16_t virtualKey = 0;
        bool down = false;
        bool extended = false;
    };
    // (S9a) The mouse as it reported itself (raw input): its movement (counts, not pixels; `absolute`: a tablet / remote desktop
    // giving a position 0..65535 instead), the buttons pressed and let go in this report (bits 0 left, 1 right, 2 middle, 3 and 4
    // the side buttons) and the wheels' turns (120 a notch, forward / right positive).
    struct MouseEvent
    {
        int32_t dx = 0, dy = 0;
        bool absolute = false;
        uint8_t pressed = 0, released = 0;
        int16_t wheel = 0, hwheel = 0;
    };
    // The window's events to the PC layer (resize, focus, close; S9a: the screen's DPI and raw keyboard / mouse); the core reads
    // no input itself, it forwards. Called on the window's own thread (the core's window: the thread that pumps it; a host's
    // window: the thread that owns it). Left null: not told.
    struct WindowEvents
    {
        void (*resized)(int width, int height) = nullptr; // the client area's new size (0 x 0: minimised)
        void (*focus)(bool focused) = nullptr;
        void (*closed)() = nullptr;                       // asked to close (the X, Alt+F4): the host decides, the core closes nothing
        void (*dpiChanged)(int dpi) = nullptr;            // (S9a) moved to a screen of another scale (96 = 100 %)
        void (*key)(const KeyEvent &key) = nullptr;       // (S9a)
        void (*mouse)(const MouseEvent &mouse) = nullptr; // (S9a)
        // (S12) Every message of the window as it comes (Windows: the HWND, the message, its wParam and lParam), before the
        // window handles it - for a host that reads the window's input itself (ImGui's own Win32 input: mouse position, buttons,
        // wheel, keys, WM_CHAR text, focus). True: handled - the core's own window then returns 1 without its default handling
        // (e.g. WM_SETCURSOR: the cursor already set); a host's window is only watched, its own handling always runs.
        bool (*message)(void *window, uint32_t message, uint64_t wParam, int64_t lParam) = nullptr;
    };
    void setWindowEvents(const WindowEvents &events);
    const WindowEvents &windowEvents();

    // ---- S9a: the window (NR_CORE_SPEC.md 2 and 8; Windows - elsewhere none is made and these say so) ----
    // Which window the core works with: a window the host made (`handle`, its HWND - the host keeps its own message loop, the
    // core only watches the window's messages and forwards them), or one of the core's own (`create`), or none (the default:
    // as before S9 - the core draws its pictures off screen and the host shows them).
    struct WindowSpec
    {
        void *handle = nullptr;    // the host's window (Windows: an HWND); null: see `create`
        bool create = false;       // no handle: true = the core makes its own window, false = no window
        const char *title = "";    // the core's window: its title (UTF-8)
        int width = 1280, height = 720; // the core's window: its client area's size
        bool visible = true;       // the core's window: shown at once
        bool borderless = false;   // the core's window: no frame or title bar
        int rawInput = -1;         // keyboard + mouse raw input registered for the window: -1 = on for the core's own window, off
                                   // for a host's (it may read raw input itself); 1 / 0 = on / off (a host's window: only when
                                   // open() runs on the window's own thread)
    };
    // open(settings) as above, then the window: true when the window asked for is in place (or none was asked for).
    bool open(const Settings &settings, const WindowSpec &window);
    // The window in use (Windows: its HWND), null: none.
    void *window();
    // Its client area's size now (0 x 0: none).
    void windowSize(int &width, int &height);
    // The core's own window: the messages waiting for this thread (the one that opened it) handled - the events above fired
    // from here. False once the window was asked to close (or is gone, or there is none); a host's window: its own loop pumps,
    // this only answers.
    bool pumpEvents();
    // The core's own window destroyed, or a host's let go (its messages no longer watched; raw input the core registered for it
    // taken back).
    void closeWindow();

    // ---- S9b: the core presents into the window (NR_CORE_SPEC.md 4; Windows, Direct3D 12) ----
    // One picture into the window in use through a DXGI flip-model swap chain of the core's own (made at the first present,
    // resized with the window's client area): the bars cleared, the picture's source rect drawn into the destination rect -
    // stretched bilinearly, or, enlarged with FSR 1 asked for, EASU + RCAS (the recomp's GL window does the same today,
    // nr_present.cpp) - then presented. The picture: a shared texture the host holds (backend latestShared / sharedHeld).
    struct PresentRequest
    {
        int picture = -1;          // the shared picture's index (d3d::SharedPicture::index)
        float src[4] = {};         // x, y, width, height of it shown (width 0: all of it)
        float dst[4] = {};         // x, y, width, height in the window's client area (width 0: all of it)
        int upscaler = 0;          // 0 a bilinear stretch, 1 FSR 1 (when enlarged by more than 1 %)
        float sharpness = 0.2f;    // FSR 1's RCAS, in stops (0 = the most; AMD's default 0.2)
        int syncInterval = 0;      // 0: at once (with tearing where the system allows it - an unlimited cap, VRR), 1..4: vsync
        float clear[3] = {};       // the bars' colour
        const char *shot = nullptr; // the window's picture as drawn saved here (BMP), when not null - with the overlay in it
        Overlay overlay;           // (S12) drawn into the back buffer over the picture before the shot and the present; empty: none
    };
    // From any one thread at a time (the host's present thread); false: not presented (no window, no picture, no device).
    bool present(const PresentRequest &request);
    // The swap chain let go (closeWindow does this first; a host may before it destroys or reuses its window).
    void releasePresent();

    // (S2b) What the pacing log (nr_pacelog.cpp) needs of its host: the screen's refresh timing (the latest refresh and the
    // period, steady-clock nanoseconds; false: not known), whether the refresh is variable, the view turn's own figures for the
    // log line and each picture shown to it (N9), the hand-over's gate holds and cap drops since the last line (TODO 336).
    // Left null: those parts of the line are empty or zero.
    struct PresentHost
    {
        bool (*screenTiming)(int64_t &refreshNs, int64_t &periodNs) = nullptr;
        bool (*screenVariable)() = nullptr;
        void (*appendTurnPacing)(char *text, size_t size) = nullptr;
        void (*noteShownPicture)(uint64_t turnId, int64_t shownNs, int64_t dueNs) = nullptr;
        void (*takeGateHolds)(unsigned &pictures, double &heldMs, double &slicesMs) = nullptr;
        unsigned (*takeCapDrops)() = nullptr;
    };
    void setPresentHost(const PresentHost &host);
    const PresentHost &presentHost();
}
