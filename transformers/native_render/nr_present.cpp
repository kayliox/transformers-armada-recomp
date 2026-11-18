// The native picture shown live in the window (README.md, stage 4): each frame the renderer reads
// its picture back (nr_d3d.cpp: publish) and the window's thread, before it draws, takes the next one
// handed over into a GL texture of its own, which the window shows in place of the GS's picture
// (ps2_host_display::setPictureProvider) - the overlays (FPS, the settings panel) still over it.
// TF_NATIVE_LIVE=1 (with TF_NATIVE_RENDER=1). Where the driver can, the picture is a D3D12 texture
// the window's GL reads itself (nr_glshare.h: no copy through the CPU); otherwise the CPU hand-over
// uploads straight from the renderer's mapped readback (no copy on the render side), which the window
// gives back once the upload has returned. TF_NATIVE_SHARE=0 keeps the CPU hand-over.
#include "graphics_driver.h"
#include "nr_types.h"
#include "nr_core.h"
#include "nr_backend.h"
#include "nr_glshare.h"
#include "nr_pacelog.h"
#include "nr_devswap.h" // (nr_core S9c: TF_NATIVE_SWAPCHAIN)
#include "nr_overlay_host.h" // (nr_core S12: the ImGui overlays through the core too)

#include "runtime/gs/gs_gpu.h"
#include "runtime/ps2_host_display.h"
#include "runtime/ps2_host_present.h"

#include "raylib.h"
#include "rlgl.h"
#include "nr_frame.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tf::port
{
    bool moviePlaying(); // (overrides.cpp)
    bool startHeld();    // (overrides.cpp, TODO 298: the start's turbo - black until the main menu)
}

#if defined(_WIN32)
extern "C" __declspec(dllimport) void *__stdcall GetCurrentThread(void);
extern "C" __declspec(dllimport) int __stdcall SetThreadPriority(void *thread, int priority);
#endif

namespace nr
{
    namespace
    {
        // (S2b) A shared picture as the pacing log (nr_pacelog.cpp) takes it.
        pacelog::Shown shownOf(const d3d::SharedPicture &sp)
        {
            pacelog::Shown s;
            s.turnId = sp.turnId;
            s.tick = sp.tick;
            s.dueNs = sp.dueNs;
            s.readyNs = sp.readyNs;
            return s;
        }

        // ---- N9: the window shows a native picture when it is handed over (nr_d3d.cpp's paced hand-over) ------
        // instead of at the VBlanks (ps2_host_display::setPicturePacer): the pictures reach the screen as evenly
        // as they are handed over. While no native picture comes (a movie, a screen it does not draw, over 100 ms)
        // the window goes back to the VBlanks and the GS's pictures.
        std::atomic<uint64_t> g_takenSerial{0u}; // the newest picture the window took
        std::atomic<bool> g_pacedLive{false};    // the window shows the native picture now
        bool g_pendingShown = false;             // a new picture drawn, for the pacing log once presented
        d3d::SharedPicture g_pendingPicture;
        uint64_t g_pendingSkipped = 0u;
        bool g_pacerOn = false;
        // (The CPU hand-over's pictures evened out: they are handed over as their frame's slot is used again, not at their
        // frame's moment as the shared ones are (nr_d3d.cpp's pacer), so two often came within a millisecond and the next
        // 60-90 ms later - at the game's 30 in a window on a low-end PC the time between pictures read 1..85 ms. A CPU
        // picture is taken at once and presented no sooner than 4/5 of the recent interval between the hand-over's arrivals
        // after the one before. Based on the arrivals, not on when pictures were shown, and the take never held: holding the
        // take slowed the renderer, which lengthened the shown interval and so the next hold - on a ~10 fps laptop the frame
        // rate fell to 3-6 (kayliox, PR #21). Window thread only.)
        bool g_cpuPictures = false;   // the picture shown now is the CPU hand-over's (provide)
        int64_t g_cpuShownNs = 0;     // when the last CPU picture was presented
        int64_t g_cpuArrivalNs = 0;   // when the newest picture seen by paceWait was handed over
        int64_t g_cpuIntervalNs = 0;  // the recent interval between the hand-over's arrivals (7/8 average)

        int paceWait(std::chrono::steady_clock::time_point until)
        {
            if (!g_pacedLive.load(std::memory_order_relaxed))
                return -1;
            int64_t lastNs = 0;
            if (backend().waitPicture(g_takenSerial.load(std::memory_order_relaxed),
                                 std::chrono::duration_cast<std::chrono::nanoseconds>(until.time_since_epoch()).count(), lastNs))
            {
                if (lastNs != 0 && lastNs != g_cpuArrivalNs)
                {
                    const int64_t d = g_cpuArrivalNs != 0 ? lastNs - g_cpuArrivalNs : 0;
                    if (d > 0 && d < 1000000000ll)
                        g_cpuIntervalNs = g_cpuIntervalNs == 0 ? d : (g_cpuIntervalNs * 7 + d) / 8;
                    g_cpuArrivalNs = lastNs;
                }
                return 1;
            }
            // (How long without a picture counts as none coming: 100 ms, or three of the recent intervals between them where
            // the renderer is slower than that - on a low-end PC in full screen it hands over one every 140-200 ms, and at
            // 100 ms the window dropped back to its own redraws every other picture. A second at most. Unchanged at 30
            // pictures a second and above: three intervals are then 100 ms or less. PR #18, TheBrousse.)
            static int64_t seenNs = 0, intervalNs = 0;
            if (lastNs != 0 && lastNs != seenNs)
            {
                const int64_t d = seenNs != 0 ? lastNs - seenNs : 0;
                if (d > 0 && d < 1000000000ll)
                    intervalNs = intervalNs == 0 ? d : (intervalNs * 7 + d) / 8;
                seenNs = lastNs;
            }
            const int64_t noneAfterNs = std::clamp<int64_t>(3 * intervalNs, 100000000ll, 1000000000ll);
            if (lastNs == 0 || pacelog::now() - lastNs > noneAfterNs)
            {
                g_pacedLive.store(false, std::memory_order_relaxed);
                return -1;
            }
            return 0;
        }

        void pacePresented()
        {
            if (g_cpuPictures)
                g_cpuShownNs = pacelog::now();
            if (g_pendingShown)
            {
                g_pendingShown = false;
                pacelog::noteShown(shownOf(g_pendingPicture), g_pendingSkipped);
            }
        }

        // (nr_core S9c, TF_NATIVE_SWAPCHAIN) The shared picture the window shows this frame (-1: none - another picture or none)
        // and its frame's tick, for the developer switch's drawer.
        int g_swapPicture = -1;
        uint64_t g_swapTick = 0;

        bool provide(unsigned int &texture, int &width, int &height)
        {
            g_swapPicture = -1;
            static Texture2D shown{};
            static uint64_t serial = 0;
            static bool asked = false;
            if (!asked)
            {
                asked = true;
                std::fprintf(stderr, "[native render] the window asks for the live picture\n");
#if defined(_WIN32)
                // (TODO 201) The window's thread - it takes each picture and presents it, and opens and closes the
                // upscaled movies - above normal priority: with every core busy at normal priority (a video encode, a
                // build) it got its turn late, the pictures were made but replaced before it showed them (3-8 a second
                // of the 150 made) and the movies' starts and ends reached it seconds late. It mostly waits (for a
                // picture, the screen's refresh), so above normal costs the rest of the PC little.
                SetThreadPriority(GetCurrentThread(), 1); // (THREAD_PRIORITY_ABOVE_NORMAL)
#endif
            }
            uint32_t w = 0, h = 0;
            static bool has3D = false;
#if defined(__linux__)
            {
                nr_core::Settings coreSettings; // (the picture drawn at the size shown in the window)
                coreSettings.windowWidth = GetRenderWidth();
                coreSettings.windowHeight = GetRenderHeight();
                nr_core::applySettings(coreSettings);
            }
#else
            {
                nr_core::Settings coreSettings; // (the picture drawn at the window's own lines)
                coreSettings.windowLines = GetRenderHeight();
                nr_core::applySettings(coreSettings);
            }
#endif
            // The shared textures, on the first frame (the window's GL context is current here).
            static int share = 0;
            if (share == 0)
            {
                const char *v = std::getenv("TF_NATIVE_SHARE");
                // (TODO 290) Intel's drivers: the CPU hand-over from the start. The N100's UHD Graphics read the shared
                // pictures as zeros (a black start until the check gave up on them), an Iris Xe on an old driver could
                // not share at all; TF_NATIVE_SHARE=1 tries the sharing all the same.
                const bool intel = graphics_driver::vendor() == 0x8086u && !(v && *v == '1');
                if (intel)
                    std::fprintf(stderr, "[native render] an Intel graphics card: pictures handed over through the CPU (TF_NATIVE_SHARE=1: shared)\n");
                share = (!v || *v != '0') && !intel && glshare::available() ? 1 : -1;
                nr_core::Settings coreSettings;
                coreSettings.sharedPictures = share == 1 ? 1 : 0;
                nr_core::applySettings(coreSettings);
            }
            struct Imported
            {
                unsigned texture = 0u, memory = 0u;
                uint64_t generation = 0;
            };
            static Imported imported[d3d::kSharedPictures]; // (every one the renderer may make: TODO 98)
            static int sharedShown = -1;
            static uint64_t sharedTick = 0; // (S9c: its frame's tick)
            static uint32_t sharedW = 0, sharedH = 0;
            d3d::SharedPicture sp;
#if defined(__linux__)
            const uint8_t *src = nullptr;
            uint32_t pitch = 0;
            int readback = -1;
#else
            static std::vector<uint32_t> pixels;
#endif
            const uint64_t serialBefore = serial;
            bool fresh = false;
            // (Milestone 3) The backend presents the window itself (Vulkan in SDL3's window): its picture is a texture of
            // the window's platform, the GPU's own - no copy, no import. Asked each picture: the window draws it now.
            static const bool windowPictures = backend().presentsWindow();
            WindowPicture wp;
            const bool windowNow = windowPictures && backend().latestWindowPicture(wp);
            if (windowNow)
            {
                fresh = wp.serial != serial;
                if (fresh)
                    g_takenSerial.store(wp.serial, std::memory_order_relaxed);
                serial = wp.serial;
                has3D = wp.has3D;
                sp.tick = wp.tick;
                sp.dueNs = wp.dueNs;
                sp.readyNs = wp.readyNs;
                sp.has3D = wp.has3D;
            }
            else if (share == 1 && backend().latestShared(sp, serial) && sp.index >= 0 && sp.index < d3d::kSharedPictures)
            {
                fresh = true;
                g_takenSerial.store(serial, std::memory_order_relaxed);
                Imported &im = imported[sp.index];
                if (!im.texture || im.generation != sp.generation)
                {
                    glshare::release(im.texture, im.memory);
                    im = Imported{};
                    im.texture = glshare::import(sp.handle, sp.size, sp.width, sp.height, im.memory);
                    im.generation = sp.generation;
                    if (im.texture)
                        std::fprintf(stderr, "[native render] live picture: shared texture %d, %ux%u\n", sp.index, sp.width, sp.height);
                    else if (sharedShown < 0)
                    {
                        // The driver says it can share but no picture can be read (Wine / Proton: GL error 0500 on
                        // every import, so the window stayed its own pink-purple - a player's report): the CPU
                        // hand-over for the rest of the session, from the next frame on.
                        std::fprintf(stderr, "[native render] shared pictures cannot be read by the window: the CPU hand-over from now on\n");
                        share = -1;
                        nr_core::Settings coreSettings;
                        coreSettings.sharedPictures = 0;
                        nr_core::applySettings(coreSettings);
                    }
                }
                if (sp.stamp)
                    std::fprintf(stderr, "[present] window takes stamp %u (texture %d)\n", sp.stamp, sp.index);
                if (im.texture)
                {
                    sharedShown = sp.index;
                    sharedTick = sp.tick;
                    sharedW = sp.width;
                    sharedH = sp.height;
                    has3D = sp.has3D;
                    backend().sharedHeld(sp.index);
                    // The import checked by what GL reads from it: Intel's driver (the N100's UHD Graphics, 2026-11-10)
                    // imports the shared textures without an error and reads zeros - the window stayed black while the
                    // game ran (a player's report). Every 10th picture is read back until one shows anything (a picture
                    // the renderer made is opaque, even a black one); six reads of nothing but zeros (60 pictures, a
                    // second or two) and the CPU hand-over takes over for the session. TF_NATIVE_SHARE_CHECK=0: no check.
                    static const bool check = !(std::getenv("TF_NATIVE_SHARE_CHECK") && *std::getenv("TF_NATIVE_SHARE_CHECK") == '0');
                    static int checked = 0, zeroReads = 0;
                    static bool verified = false;
                    if (check && !verified && share == 1 && ++checked % 10 == 1)
                    {
                        unsigned colour = 0u, alpha = 0u;
                        if (glshare::readBack(im.texture, sp.width, sp.height, colour, alpha))
                        {
                            // (TF_NATIVE_SHARE_TEST=zeros: read as nothing but zeros, as Intel's driver does - tests of the
                            // fallback to the CPU hand-over, TODO 284)
                            static const bool zeros = std::getenv("TF_NATIVE_SHARE_TEST") && std::strcmp(std::getenv("TF_NATIVE_SHARE_TEST"), "zeros") == 0;
                            if (zeros)
                                colour = alpha = 0u;
                            if (colour != 0u || alpha != 0u)
                            {
                                verified = true;
                                std::fprintf(stderr, "[native render] the window reads the shared pictures (colour up to %u, alpha up to %u)\n",
                                             colour, alpha);
                            }
                            else if (++zeroReads >= 6)
                            {
                                std::fprintf(stderr, "[native render] the window's GL reads only zeros from the shared pictures (%d pictures): "
                                                     "the CPU hand-over from now on\n",
                                             checked);
                                share = -1;
                                nr_core::Settings coreSettings;
                                coreSettings.sharedPictures = 0;
                                nr_core::applySettings(coreSettings);
                            }
                        }
                    }
                }
                else
                    backend().sharedHeld(sharedShown); // (not imported: the one still shown stays held - latestShared marked the new one)
            }
            // (TODO 284) The picture taken, as the shared path marks it: the paced hand-over (paceWait) waits for one
            // newer than this. Unmarked, every wait found the CPU hand-over's picture "new" again - the window drew
            // as fast as it could and counted each draw: the FPS counter read 600-1500 (TF_NATIVE_SHARE=0, Proton, the
            // Intel fallback). (PR #18, TheBrousse: and marked fresh, so it is paced and logged as a shared picture is, below.)
#if defined(__linux__)
            else if (share != 1 && backend().latestPicture(src, pitch, readback, serial, sp))
            {
                fresh = true;
                g_takenSerial.store(serial, std::memory_order_relaxed);
                w = sp.width;
                h = sp.height;
                has3D = sp.has3D;
                // (The rows as raylib takes them: one copy only when a readback row is padded - widths not a
                // multiple of 64 - since UpdateTexture sets no GL_UNPACK_ROW_LENGTH.)
                const uint8_t *tight = src;
                if (pitch != w * 4u)
                {
                    static std::vector<uint32_t> rows;
                    rows.resize(static_cast<size_t>(w) * h);
                    for (uint32_t y = 0; y < h; ++y)
                        std::memcpy(rows.data() + static_cast<size_t>(y) * w, src + static_cast<size_t>(y) * pitch, w * 4u);
                    tight = reinterpret_cast<const uint8_t *>(rows.data());
                }
                if (shown.id == 0u || shown.width != static_cast<int>(w) || shown.height != static_cast<int>(h))
                {
                    if (shown.id != 0u)
                        UnloadTexture(shown);
                    Image image{const_cast<uint8_t *>(tight), static_cast<int>(w), static_cast<int>(h), 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
                    shown = LoadTextureFromImage(image);
                    SetTextureFilter(shown, TEXTURE_FILTER_BILINEAR);
                    std::fprintf(stderr, "[native render] live picture: %ux%u\n", w, h);
                }
                else
                    UpdateTexture(shown, tight);
                // (The upload has its copy: the readback is the renderer's again.)
                backend().pictureShown(readback);
            }
#else
            else if (share != 1 && backend().latestPicture(pixels, w, h, serial, has3D))
            {
                fresh = true;
                g_takenSerial.store(serial, std::memory_order_relaxed);
                if (shown.id == 0u || shown.width != static_cast<int>(w) || shown.height != static_cast<int>(h))
                {
                    if (shown.id != 0u)
                        UnloadTexture(shown);
                    Image image{pixels.data(), static_cast<int>(w), static_cast<int>(h), 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
                    shown = LoadTextureFromImage(image);
                    SetTextureFilter(shown, TEXTURE_FILTER_BILINEAR);
                    std::fprintf(stderr, "[native render] live picture: %ux%u\n", w, h);
                }
                else
                    UpdateTexture(shown, pixels.data());
            }
#endif
            // (No new native picture for a while - a movie, a screen the native renderer does not draw
            // yet: the GS's is shown, not a stale one.)
            static auto lastNew = std::chrono::steady_clock::time_point{};
            static uint64_t lastSerial = 0;
            if (serial != lastSerial)
            {
                lastSerial = serial;
                lastNew = std::chrono::steady_clock::now();
            }
            const bool sharedNow = share == 1 && sharedShown >= 0 && imported[sharedShown].texture;
            // (TODO 298) The start runs unthrottled to the main menu: its screens and logos flash by in a fraction of a
            // second - black instead, the main menu the first picture shown.
            if (tf::port::startHeld())
            {
                static Texture2D startBlack{};
                if (startBlack.id == 0u)
                {
                    Image image = GenImageColor(4, 4, BLACK);
                    startBlack = LoadTextureFromImage(image);
                    UnloadImage(image);
                }
                if (startBlack.id != 0u)
                {
                    g_pacedLive.store(false, std::memory_order_relaxed);
                    texture = startBlack.id;
                    width = startBlack.width;
                    height = startBlack.height;
                    return true;
                }
            }
            // (Neither picture yet: black, not the window's own fill. 2026-10-28, TODO 63: after the
            // settings' restart the new process loads a state in a level; with the native renderer
            // drawing alone the GS publishes no picture there, the native one comes only once Direct3D
            // 12 is made ready on its thread - and the window showed its "no picture" texture,
            // ps2_runtime.cpp UploadFrame's MAGENTA fill: the purple screen. Until the GS has shown a
            // picture once, a frame with no native picture is black.)
            static bool gsShown = false;
            // (Roadmap N3b: with the emulated drawing off - the native renderer draws every frame, menus and movies too -
            // the GS draws nothing, so its picture is never the one to show: the native picture stays up while no new one
            // comes - a load, a pause of the game's frames. Before its first, the GS's last (from before the switch) or black.)
            const bool gsOff = !ps2_host_display::emulatedDrawing();
            const bool nativeAny = shown.id != 0u || sharedNow || windowNow;
            const bool nativeNone = !nativeAny || (!gsOff && std::chrono::steady_clock::now() - lastNew > std::chrono::milliseconds(100));
            // (N3b) What the game shows without frames of the render device - the warp tunnel of a load (its pictures
            // uploaded into the display buffers, the display switched to them), a movie DMA'd there: no native picture comes,
            // the display buffer is read from GS memory (nr_capture.cpp gsDisplayedPicture) - while a movie plays, or the
            // display is not the one the game's frames had (else, a load holding its last frame: the native picture stays).
            // Off by default (TF_NATIVE_GSMOVIE=1: on): the one picture found this way, the warp tunnel of a load, is drawn by
            // GS primitives, not uploaded - GS memory holds nothing of it with the GS drawing off; it is drawn natively from
            // its packets instead (nr_capture.cpp looseGif). Kept for a movie a game DMAs to its display buffers.
            static const bool movieFromGs = std::getenv("TF_NATIVE_GSMOVIE") && *std::getenv("TF_NATIVE_GSMOVIE") == '1';
            static bool fromGs = false; // (the picture read from GS memory now: logged as it starts and stops)
            static int fromGsTold = 0;
            const bool wantGs = gsOff && movieFromGs && (tf::port::moviePlaying() || !gsDisplayFromFrames()) &&
                                (!nativeAny || std::chrono::steady_clock::now() - lastNew > std::chrono::milliseconds(50));
            if (wantGs != fromGs)
            {
                fromGs = wantGs;
                if (fromGsTold < 40)
                {
                    ++fromGsTold;
                    std::fprintf(stderr, "[native render] picture from GS memory %s (no frames of the game: %s)\n", wantGs ? "on" : "off",
                                 tf::port::moviePlaying() ? "a movie" : "the display not the frames' - a load's warp tunnel");
                }
            }
            if (wantGs)
            {
                static Texture2D movie{};
                static std::vector<uint32_t> moviePixels;
                static auto movieRead = std::chrono::steady_clock::time_point{};
                static bool movieHave = false, movieTold = false;
                const auto now = std::chrono::steady_clock::now();
                if (!movieHave || now - movieRead >= std::chrono::milliseconds(4))
                {
                    movieRead = now;
                    uint32_t mw = 0u, mh = 0u;
                    if (gsDisplayedPicture(moviePixels, mw, mh))
                    {
                        if (movie.id == 0u || movie.width != static_cast<int>(mw) || movie.height != static_cast<int>(mh))
                        {
                            if (movie.id != 0u)
                                UnloadTexture(movie);
                            Image image{moviePixels.data(), static_cast<int>(mw), static_cast<int>(mh), 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
                            movie = LoadTextureFromImage(image);
                            SetTextureFilter(movie, TEXTURE_FILTER_BILINEAR);
                        }
                        else
                            UpdateTexture(movie, moviePixels.data());
                        movieHave = movie.id != 0u;
                        if (!movieTold)
                        {
                            movieTold = true;
                            std::fprintf(stderr, "[native render] a picture with no frames of the game (a movie, a load's warp tunnel): read from GS memory (%ux%u)\n", mw, mh);
                        }
                    }
                }
                if (movieHave)
                {
                    g_pacedLive.store(false, std::memory_order_relaxed);
                    texture = movie.id;
                    width = movie.width;
                    height = movie.height;
                    return true;
                }
            }
            if (nativeNone && !gsShown)
            {
                static std::vector<uint8_t> probe;
                uint32_t pw = 0, ph = 0;
                gsShown = ps2_host_display::copyPicture(0, probe, pw, ph);
                std::vector<uint8_t>().swap(probe);
                // (TODO 294) The GS drawn on the GPU (PS2X_GPU, the default) publishes no software picture - copyPicture
                // never has one - and the window shows the GPU's own (ps2_runtime's window loop: ps2_gpu::picture). Looked
                // for only in the software one, a GS on the GPU never counted as shown: once the native renderer stopped (a
                // lost device: the GS drawing again) every frame got the black picture, the game behind it unseen. Its
                // newest picture counts too.
                if (!gsShown && ps2_gpu::viewActive())
                {
                    ps2_gpu::Picture gpu;
                    gsShown = ps2_gpu::picture(gpu, ~0ull) && gpu.texture != 0u;
                }
                if (!gsShown)
                {
                    static Texture2D black{};
                    if (black.id == 0u)
                    {
                        Image image = GenImageColor(4, 4, BLACK);
                        black = LoadTextureFromImage(image);
                        UnloadImage(image);
                        std::fprintf(stderr, "[native render] no picture from either renderer yet: black shown (not the window's purple)\n");
                    }
                    if (black.id != 0u)
                    {
                        texture = black.id;
                        width = black.width;
                        height = black.height;
                        return true;
                    }
                }
                else
                    std::fprintf(stderr, "[native render] the GS's first picture\n");
            }
            if (nativeNone)
            {
                g_pacedLive.store(false, std::memory_order_relaxed);
                return false;
            }
            // (A frame of 2D alone - a menu, a movie, which come in GS uploads the native renderer
            // does not show yet: the GS's picture, drawn for it - see nr_capture's emulated drawing.)
            static const bool show2D = std::getenv("TF_NATIVE_2D") && *std::getenv("TF_NATIVE_2D") == '1';
            if (!has3D && !show2D && !gsOff)
            {
                g_pacedLive.store(false, std::memory_order_relaxed);
                return false;
            }
            if (windowNow)
            {
                const uint64_t skipped = serialBefore != 0u && serial > serialBefore + 1u ? serial - serialBefore - 1u : 0u;
                if (fresh && g_pacerOn)
                {
                    g_pendingShown = true;
                    g_pendingPicture = sp;
                    g_pendingSkipped = skipped;
                }
                else if (fresh)
                    pacelog::noteShown(shownOf(sp), skipped);
                else
                    pacelog::noteRepeat();
                g_pacedLive.store(true, std::memory_order_relaxed);
                texture = wp.texture;
                width = wp.width;
                height = wp.height;
                return true;
            }
            if (sharedNow)
            {
                const uint64_t skipped = serialBefore != 0u && serial > serialBefore + 1u ? serial - serialBefore - 1u : 0u;
                if (fresh && g_pacerOn)
                {
                    // (logged once presented: pacePresented)
                    g_pendingShown = true;
                    g_pendingPicture = sp;
                    g_pendingSkipped = skipped;
                }
                else if (fresh)
                    pacelog::noteShown(shownOf(sp), skipped);
                else
                    pacelog::noteRepeat();
                g_pacedLive.store(true, std::memory_order_relaxed);
                g_cpuPictures = false;
                g_swapPicture = sharedShown;
                g_swapTick = sharedTick;
                texture = imported[sharedShown].texture;
                width = static_cast<int>(sharedW);
                height = static_cast<int>(sharedH);
                return true;
            }
#if defined(__linux__)
            // (N9: the CPU hand-over too is shown as each picture is handed over, not at the next VBlank - the
            // window's pacer waits for the serial, and while no picture comes the 100 ms rule takes it back.)
            {
                const uint64_t skipped = serialBefore != 0u && serial > serialBefore + 1u ? serial - serialBefore - 1u : 0u;
                if (fresh)
                {
                    if (g_pacerOn)
                    {
                        // (logged once presented: pacePresented)
                        g_pendingShown = true;
                        g_pendingPicture = sp;
                        g_pendingSkipped = skipped;
                    }
                    else
                        pacelog::noteShown(shownOf(sp), skipped);
                    g_pacedLive.store(true, std::memory_order_relaxed);
                }
                else
                    pacelog::noteRepeat();
            }
#else
            // The CPU hand-over's picture: paced and logged as a shared one (shown when it is handed over). (PR #18, TheBrousse)
            {
                const uint64_t skipped = serialBefore != 0u && serial > serialBefore + 1u ? serial - serialBefore - 1u : 0u;
                if (fresh && g_pacerOn)
                {
                    g_pendingShown = true;
                    g_pendingPicture = d3d::SharedPicture{};
                    g_pendingSkipped = skipped;
                }
                else if (fresh)
                    pacelog::noteShown(pacelog::Shown{}, skipped);
                else
                    pacelog::noteRepeat();
                g_pacedLive.store(true, std::memory_order_relaxed);
                g_cpuPictures = true;
                // (Taken already: only its present waits - no sooner than 4/5 of the arrivals' interval after the one before.)
                if (fresh && g_pacerOn && g_cpuShownNs != 0 && g_cpuIntervalNs != 0)
                    std::this_thread::sleep_until(std::chrono::steady_clock::time_point(std::chrono::nanoseconds(g_cpuShownNs + g_cpuIntervalNs * 4 / 5)));
            }
#endif
            texture = shown.id;
            width = shown.width;
            height = shown.height;
            return true;
        }
    }

    // ---- TODO 295: FSR 1 (AMD FidelityFX Super Resolution 1, MIT) as the window's upscaler ----
    // At a render scale under 100% the picture is smaller than the window; the window stretched it (bilinear). With
    // F1 > Graphics > Upscaler at Auto or FSR 1 it is drawn here instead: EASU (the edge-adaptive upscale, its 12 taps
    // read with texelFetch - GL 3.3 has no textureGather) into a texture of the window's size, then RCAS (its
    // sharpening) onto the window. PR #23's CAS in the FXAA pass is skipped meanwhile (frame::sharpenAmount: one
    // sharpening, not two). Not enlarged (100%), Bilinear, or the shaders not made: the window's own stretch, as before.
    namespace fsr
    {
        const char *const kEasu = R"(#version 330
in vec2 fragTexCoord;
out vec4 finalColor;
uniform sampler2D texture0;
vec3 tap(ivec2 p)
{
    ivec2 s = textureSize(texture0, 0);
    return texelFetch(texture0, clamp(p, ivec2(0), s - 1), 0).rgb;
}
float luma(vec3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }
void setF(inout vec2 dir, inout float len, vec2 pp, int corner, float lA, float lB, float lC, float lD, float lE)
{
    float w = corner == 0 ? (1.0 - pp.x) * (1.0 - pp.y) : corner == 1 ? pp.x * (1.0 - pp.y) : corner == 2 ? (1.0 - pp.x) * pp.y : pp.x * pp.y;
    float dc = lD - lC, cb = lC - lB;
    float lenX = 1.0 / max(max(abs(dc), abs(cb)), 1.0 / 65536.0);
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = clamp(abs(dirX) * lenX, 0.0, 1.0);
    len += lenX * lenX * w;
    float ec = lE - lC, ca = lC - lA;
    float lenY = 1.0 / max(max(abs(ec), abs(ca)), 1.0 / 65536.0);
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = clamp(abs(dirY) * lenY, 0.0, 1.0);
    len += lenY * lenY * w;
}
void tapF(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len, float lob, float clp, vec3 c)
{
    vec2 v = vec2(off.x * dir.x + off.y * dir.y, off.x * -dir.y + off.y * dir.x) * len;
    float d2 = min(v.x * v.x + v.y * v.y, clp);
    float wB = 2.0 / 5.0 * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = 25.0 / 16.0 * wB - (25.0 / 16.0 - 1.0);
    float w = wB * wA;
    aC += c * w;
    aW += w;
}
void main()
{
    vec2 pp = fragTexCoord * vec2(textureSize(texture0, 0)) - 0.5;
    vec2 fp = floor(pp);
    pp -= fp;
    ivec2 o = ivec2(fp);
    vec3 b = tap(o + ivec2(0, -1)), c = tap(o + ivec2(1, -1));
    vec3 e = tap(o + ivec2(-1, 0)), f = tap(o), g = tap(o + ivec2(1, 0)), h = tap(o + ivec2(2, 0));
    vec3 i = tap(o + ivec2(-1, 1)), j = tap(o + ivec2(0, 1)), k = tap(o + ivec2(1, 1)), l = tap(o + ivec2(2, 1));
    vec3 n = tap(o + ivec2(0, 2)), oo = tap(o + ivec2(1, 2));
    float bL = luma(b), cL = luma(c), eL = luma(e), fL = luma(f), gL = luma(g), hL = luma(h);
    float iL = luma(i), jL = luma(j), kL = luma(k), lL = luma(l), nL = luma(n), oL = luma(oo);
    vec2 dir = vec2(0.0);
    float len = 0.0;
    setF(dir, len, pp, 0, bL, eL, fL, gL, jL);
    setF(dir, len, pp, 1, cL, fL, gL, hL, kL);
    setF(dir, len, pp, 2, fL, iL, jL, kL, nL);
    setF(dir, len, pp, 3, gL, jL, kL, lL, oL);
    vec2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < 1.0 / 32768.0;
    dirR = zro ? 1.0 : inversesqrt(dirR);
    dir.x = zro ? 1.0 : dir.x;
    dir *= dirR;
    len = len * 0.5;
    len *= len;
    float stretch = (dir.x * dir.x + dir.y * dir.y) / max(abs(dir.x), abs(dir.y));
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = 1.0 / lob;
    vec3 min4 = min(min(f, g), min(j, k));
    vec3 max4 = max(max(f, g), max(j, k));
    vec3 aC = vec3(0.0);
    float aW = 0.0;
    tapF(aC, aW, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, b);
    tapF(aC, aW, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, c);
    tapF(aC, aW, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, i);
    tapF(aC, aW, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, j);
    tapF(aC, aW, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, f);
    tapF(aC, aW, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, e);
    tapF(aC, aW, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, k);
    tapF(aC, aW, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, l);
    tapF(aC, aW, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, h);
    tapF(aC, aW, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, g);
    tapF(aC, aW, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, oo);
    tapF(aC, aW, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, n);
    finalColor = vec4(min(max4, max(min4, aC / aW)), 1.0);
}
)";
        const char *const kRcas = R"(#version 330
in vec2 fragTexCoord;
out vec4 finalColor;
uniform sampler2D texture0;
uniform float sharpness;
vec3 tap(ivec2 p)
{
    ivec2 s = textureSize(texture0, 0);
    return texelFetch(texture0, clamp(p, ivec2(0), s - 1), 0).rgb;
}
void main()
{
    ivec2 sp = ivec2(floor(fragTexCoord * vec2(textureSize(texture0, 0))));
    vec3 b = tap(sp + ivec2(0, -1)), d = tap(sp + ivec2(-1, 0)), e = tap(sp), f = tap(sp + ivec2(1, 0)), h = tap(sp + ivec2(0, 1));
    vec3 mn4 = min(min(b, d), min(f, h));
    vec3 mx4 = max(max(b, d), max(f, h));
    vec3 hitMin = min(mn4, e) / (4.0 * mx4 + 1.0 / 65536.0);
    vec3 hitMax = (1.0 - max(mx4, e)) / (4.0 * mn4 - 4.0 - 1.0 / 65536.0);
    vec3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-(0.25 - 1.0 / 16.0), min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * sharpness;
    finalColor = vec4((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0), 1.0);
}
)";
        Shader g_easu{}, g_rcas{};
        int g_sharpLoc = -1;
        bool g_tried = false, g_ok = false;
        RenderTexture2D g_mid{};
        uint64_t g_drawn = 0;

        bool ready()
        {
            if (g_tried)
                return g_ok;
            g_tried = true;
            g_easu = LoadShaderFromMemory(nullptr, kEasu);
            g_rcas = LoadShaderFromMemory(nullptr, kRcas);
            const unsigned int fallback = rlGetShaderIdDefault();
            g_ok = g_easu.id != 0u && g_easu.id != fallback && g_rcas.id != 0u && g_rcas.id != fallback;
            if (g_ok)
                g_sharpLoc = GetShaderLocation(g_rcas, "sharpness");
            std::fprintf(stderr, "[native render] FSR 1 upscaler: %s\n", g_ok ? "ready (EASU + RCAS)" : "its shaders did not build - the window's stretch is used");
            return g_ok;
        }

        // ps2_host_display::setPictureDrawer: true when drawn.
        bool draw(unsigned int texture, int width, int height, float srcX, float srcY, float srcW, float srcH, float dstX, float dstY,
                  float dstW, float dstH)
        {
            if (!frame::fsrOn() || dstW <= srcW * 1.01f || dstH <= srcH * 1.01f || !ready())
                return false;
            const int ow = static_cast<int>(std::lround(dstW)), oh = static_cast<int>(std::lround(dstH));
            if (ow <= 0 || oh <= 0)
                return false;
            if (g_mid.id == 0u || g_mid.texture.width != ow || g_mid.texture.height != oh)
            {
                if (g_mid.id != 0u)
                    UnloadRenderTexture(g_mid);
                g_mid = LoadRenderTexture(ow, oh);
                if (g_mid.id == 0u)
                    return false;
            }
            const Texture2D picture{texture, width, height, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
            BeginTextureMode(g_mid);
            BeginShaderMode(g_easu);
            DrawTexturePro(picture, Rectangle{srcX, srcY, srcW, srcH}, Rectangle{0.0f, 0.0f, static_cast<float>(ow), static_cast<float>(oh)},
                           Vector2{0.0f, 0.0f}, 0.0f, WHITE);
            EndShaderMode();
            EndTextureMode();
            // (RCAS's sharpness in stops, 0 = the most: AMD's default 0.2; TF_NATIVE_FSR_SHARPNESS=<stops>)
            static const float sharp = std::exp2(-(std::getenv("TF_NATIVE_FSR_SHARPNESS") ? static_cast<float>(std::atof(std::getenv("TF_NATIVE_FSR_SHARPNESS"))) : 0.2f));
            BeginShaderMode(g_rcas);
            SetShaderValue(g_rcas, g_sharpLoc, &sharp, SHADER_UNIFORM_FLOAT);
            // (a render texture's rows are upside down: read it flipped)
            DrawTexturePro(g_mid.texture, Rectangle{0.0f, 0.0f, static_cast<float>(ow), -static_cast<float>(oh)}, Rectangle{dstX, dstY, dstW, dstH},
                           Vector2{0.0f, 0.0f}, 0.0f, WHITE);
            EndShaderMode();
            // (told for each new size, the first few)
            static int lastW = 0, lastH = 0;
            if ((width != lastW || height != lastH) && g_drawn < 8u)
            {
                ++g_drawn;
                lastW = width;
                lastH = height;
                std::fprintf(stderr, "[native render] FSR 1 upscaler: %dx%d (shown %.0fx%.0f of it) -> %dx%d\n", width, height, srcW, srcH, ow, oh);
            }
            return true;
        }
    }

    // (nr_core S9c) The picture drawer under the developer switch TF_NATIVE_SWAPCHAIN=1 (nr_devswap.cpp): the GL window draws the
    // picture as without the switch (fsr::draw, or the plain stretch the runtime would make), then, when it is one of the
    // renderer's shared pictures, the core presents the same picture at the same rects through its own swap chain into a child
    // window over this one (FSR 1 by the same choice, the vsync as the runtime's present: ps2_host_present::sync, by the screen when
    // not forced). TF_NATIVE_SWAPCHAIN_SHOT=<tick>,...: both saved for that picture - the core's back buffer
    // (runs/swap_<tick>.bmp) and this window's drawing of it so far, read back here (runs/swapgl_<tick>.png). nr_core S12: with the
    // debug UI built in, the window's ImGui overlays are drawn into the core's picture too (nr_overlay_host.cpp, set up here at the
    // first picture): the picture is then presented by overlaysDrawn() once they are drawn, and both shots are taken there. S13a:
    // the window's raylib overlays too (drawn into layers the core draws as well - nr_overlay_host.cpp).
    bool devDraw(unsigned int texture, int width, int height, float srcX, float srcY, float srcW, float srcH, float dstX, float dstY, float dstW,
                 float dstH)
    {
        if (!fsr::draw(texture, width, height, srcX, srcY, srcW, srcH, dstX, dstY, dstW, dstH))
        {
            const Texture2D picture{texture, width, height, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
            DrawTexturePro(picture, Rectangle{srcX, srcY, srcW, srcH}, Rectangle{dstX, dstY, dstW, dstH}, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
        }
        const float src[4] = {srcX, srcY, srcW, srcH}, dst[4] = {dstX, dstY, dstW, dstH};
        const uint64_t due = g_swapPicture >= 0 ? devswap::shotDue(g_swapTick) : 0u;
        const std::string shot = due ? "runs/swap_" + std::to_string(due) + ".bmp" : std::string();
        static const float stops = std::getenv("TF_NATIVE_FSR_SHARPNESS") ? static_cast<float>(std::atof(std::getenv("TF_NATIVE_FSR_SHARPNESS"))) : 0.2f;
        const int forced = ps2_host_present::sync();
        const int sync = forced >= 0 ? forced : (ps2_host_display::screenVariable() ? 0 : 1);
        static const bool overlaysToo = overlay_host::install(); // (S12, once, on this thread)
        devswap::show(GetWindowHandle(), g_swapPicture, src, dst, frame::fsrOn() ? 1 : 0, stops, sync, due ? shot.c_str() : nullptr);
        if (due && !overlaysToo)
        {
            rlDrawRenderBatchActive(); // (what is drawn so far, out of raylib's batch)
            Image gl = LoadImageFromScreen();
            const std::string file = "runs/swapgl_" + std::to_string(due) + ".png";
            ExportImage(gl, file.c_str());
            UnloadImage(gl);
            std::fprintf(stderr, "[native render] TF_NATIVE_SWAPCHAIN_SHOT %llu: %s and %s (picture %d, tick %llu)\n",
                         static_cast<unsigned long long>(due), shot.c_str(), file.c_str(), g_swapPicture, static_cast<unsigned long long>(g_swapTick));
        }
        return true;
    }

    void overlaysDrawn()
    {
        // (nr_core S12) Nothing at all without the developer switch: the window's present as before.
        if (!devswap::on())
            return;
        // (S13a) The window's last raylib overlay layer closed (drawn into the window, handed to the core), and GL done with the
        // layers before the core's present reads them.
        ps2_host_present::overlayLayerEnd();
        if (overlay_host::takeLayersDrawn())
            nr::glshare::finish();
        if (const uint64_t due = devswap::keptShotTick())
        {
            // (the GL twin of the core's shot: this window's picture with its overlays, as the core's will be)
            rlDrawRenderBatchActive();
            Image gl = LoadImageFromScreen();
            const std::string file = "runs/swapgl_" + std::to_string(due) + ".png";
            ExportImage(gl, file.c_str());
            UnloadImage(gl);
            std::fprintf(stderr, "[native render] TF_NATIVE_SWAPCHAIN_SHOT %llu: runs/swap_%llu.bmp and %s (with the overlays)\n",
                         static_cast<unsigned long long>(due), static_cast<unsigned long long>(due), file.c_str());
        }
        devswap::flush();
    }

    void overlayShotNext(const char *path)
    {
        if (devswap::on())
            devswap::shotNext(path);
    }

    void startLivePicture()
    {
        const char *live = std::getenv("TF_NATIVE_LIVE");
        if (!live || *live != '1')
            return;
        ps2_host_display::setPictureProvider(&provide);
        // (TODO 295: FSR 1 when enlarged and chosen; nr_core S9c: TF_NATIVE_SWAPCHAIN=1, the core's swap chain - a developer switch)
        ps2_host_display::setPictureDrawer(devswap::on() ? &devDraw : &fsr::draw);
        std::fprintf(stderr, "[native render] the native picture is shown live\n");
        // (N9) Shown as it is handed over (TF_PACING, nr_d3d.cpp): paced by default; TF_PACING=0 at the VBlanks, as before.
        if (backend().pacingMode() != 0)
        {
            g_pacerOn = true;
            ps2_host_display::PicturePacer pacer;
            pacer.wait = &paceWait;
            pacer.presented = &pacePresented;
            ps2_host_display::setPicturePacer(pacer);
            std::fprintf(stderr, "[native render] pictures shown as they are handed over (%s)\n",
                         backend().pacingMode() == 2 ? "paced: each at its frame's moment plus a steady delay" : "as soon as the GPU is done");
        }
    }
}
