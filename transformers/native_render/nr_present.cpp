// The native picture shown live in the window (README.md, stage 4): each frame the renderer reads
// its picture back (nr_d3d.cpp: publish) and the window's thread, before it draws, takes the newest
// into a GL texture of its own, which the window shows in place of the GS's picture
// (ps2_host_display::setPictureProvider) - the overlays (FPS, the settings panel) still over it.
// TF_NATIVE_LIVE=1 (with TF_NATIVE_RENDER=1). Where the driver can, the picture is a D3D12 texture
// the window's GL reads itself (nr_glshare.h: no copy through the CPU); otherwise a CPU hand-over
// (about 11 MB a frame at 2520 x 1080). TF_NATIVE_SHARE=0 keeps the CPU hand-over.
#include "nr_types.h"
#include "nr_backend.h"
#include "nr_glshare.h"
#include "view_turn.h"

#include "runtime/ps2_host_display.h"

#include "raylib.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace tf::port
{
    bool moviePlaying(); // (overrides.cpp)
}

#if defined(_WIN32)
extern "C" __declspec(dllimport) void *__stdcall GetCurrentThread(void);
extern "C" __declspec(dllimport) int __stdcall SetThreadPriority(void *thread, int priority);
#endif

namespace nr
{
    namespace
    {
        // ---- N9: the pacing log --------------------------------------------------------------------------
        // Every 2 s, `[tf] pacing ...`: how evenly the native pictures reach the screen - the time between the
        // new pictures the window shows (average, spread, the longest 1% and the 1% low), the draws that showed
        // the picture before again and the pictures never shown; from each picture's moment (its frame's VBlank,
        // Frame::dueNs) to being ready (the GPU done) and to the screen; and the screen's refresh rate, whether
        // its refresh is variable (FreeSync, G-Sync). On by default with the native picture live (TF_PACING_LOG=0
        // leaves it out). TF_PACING_TRACE=<tick>,<count>: every picture from that VBlank on, a line each.
        struct Pacing
        {
            std::vector<double> between, toScreen, toReady;
            uint32_t repeats = 0u, unshown = 0u;
            int64_t lastShownNs = 0;
            // (TODO 35: on a screen with a refresh of its own, how many refreshes each picture stayed up - the refresh after
            // its present, from the vblank watcher's phase: 0 = replaced before it was shown, then 1, 2, 3 or more)
            uint32_t held[4] = {};
            int64_t lastRefresh = 0, refreshCount = 0, lastVbNs = 0; // (refreshCount: the refreshes counted since the watch began)
            std::chrono::steady_clock::time_point since{};
        };
        Pacing g_pacing;

        int64_t nowNs()
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        bool pacingLogOn()
        {
            static const bool on = [] {
                const char *v = std::getenv("TF_PACING_LOG");
                return !v || *v != '0';
            }();
            return on;
        }

        void spreadOf(std::vector<double> v, double &avg, double &spread, double &lo, double &hi)
        {
            avg = spread = lo = hi = 0.0;
            if (v.empty())
                return;
            for (double x : v)
                avg += x;
            avg /= static_cast<double>(v.size());
            for (double x : v)
                spread += (x - avg) * (x - avg);
            spread = v.size() > 1u ? std::sqrt(spread / static_cast<double>(v.size() - 1u)) : 0.0;
            std::sort(v.begin(), v.end());
            lo = v.front();
            hi = v.back();
        }

        void printPacing(double seconds)
        {
            Pacing &p = g_pacing;
            std::vector<double> b = p.between;
            std::sort(b.begin(), b.end());
            double mean, sd, lo, hi;
            spreadOf(b, mean, sd, lo, hi);
            // (the 1% low: the rate of the slowest 1% of the pictures - their intervals' mean - at least one)
            const size_t worst = std::min(b.size(), std::max<size_t>(1u, b.size() / 100u));
            double worstMean = 0.0;
            for (size_t i = 0; i < worst; ++i)
                worstMean += b[b.size() - 1u - i];
            worstMean = worst ? worstMean / static_cast<double>(worst) : 0.0;
            double sAvg, sSd, sLo, sHi, rAvg, rSd, rLo, rHi;
            spreadOf(p.toScreen, sAvg, sSd, sLo, sHi);
            spreadOf(p.toReady, rAvg, rSd, rLo, rHi);
            std::chrono::steady_clock::time_point vb{};
            std::chrono::steady_clock::duration period{};
            const bool known = ps2_host_display::screenTiming(vb, period) && period.count() > 0;
            const double hz = known ? 1.0 / std::chrono::duration<double>(period).count() : 0.0;
            char turn[384];
            view_turn::appendPacing(turn, sizeof(turn)); // (N9: how evenly the view turned picture to picture)
            char cadence[192] = "";
            {
                const double n = static_cast<double>(p.held[0] + p.held[1] + p.held[2] + p.held[3]);
                if (n > 0.0)
                    std::snprintf(cadence, sizeof(cadence),
                                  "; up for 1/2/3+ refreshes %.0f/%.0f/%.0f%%, replaced unshown %.0f%% (estimated from the refresh after each present)",
                                  100.0 * p.held[1] / n, 100.0 * p.held[2] / n, 100.0 * p.held[3] / n, 100.0 * p.held[0] / n);
            }
            std::fprintf(stderr,
                         "[tf] pacing: %.1f new pictures a second (%zu in %.2f s); between them %.2f ms on average, spread %.2f ms, "
                         "longest 1%% %.2f ms (1%% low %.1f a second), shortest %.2f, longest %.2f; repeats %u, never shown %u; "
                         "from the frame's moment to ready %.1f ms (spread %.1f), to the screen %.1f ms (spread %.2f, %.1f..%.1f); "
                         "screen %.0f Hz, variable refresh %s%s%s\n",
                         static_cast<double>(b.size()) / seconds, b.size(), seconds, mean, sd, worstMean,
                         worstMean > 0.0 ? 1000.0 / worstMean : 0.0, lo, hi, p.repeats, p.unshown, rAvg, rSd, sAvg, sSd, sLo, sHi, hz,
                         !known ? "unknown" : ps2_host_display::screenVariable() ? "on" : "off", cadence, turn);
            std::fill(std::begin(p.held), std::end(p.held), 0u);
            p.between.clear();
            p.toScreen.clear();
            p.toReady.clear();
            p.repeats = p.unshown = 0u;
        }

        // A new picture to the screen now (`skipped`: pictures handed over since the last one that never were).
        void noteShown(const d3d::SharedPicture &sp, uint64_t skipped)
        {
            if (!pacingLogOn())
                return;
            Pacing &p = g_pacing;
            const auto now = std::chrono::steady_clock::now();
            const int64_t ns = nowNs();
            if (p.since == std::chrono::steady_clock::time_point{})
                p.since = now;
            if (p.lastShownNs != 0 && ns - p.lastShownNs < 1000000000ll) // (not across a pause of a second: a movie, a load)
                p.between.push_back(static_cast<double>(ns - p.lastShownNs) / 1.0e6);
            p.lastShownNs = ns;
            {
                std::chrono::steady_clock::time_point vb{};
                std::chrono::steady_clock::duration period{};
                if (ps2_host_display::screenTiming(vb, period) && period.count() > 0 && !ps2_host_display::screenVariable())
                {
                    const int64_t vbNs = std::chrono::duration_cast<std::chrono::nanoseconds>(vb.time_since_epoch()).count();
                    const int64_t per = std::chrono::duration_cast<std::chrono::nanoseconds>(period).count();
                    // (the watcher gives the latest refresh: counted on from the one before by whole periods)
                    if (p.lastVbNs != 0 && vbNs != p.lastVbNs)
                        p.refreshCount += (vbNs - p.lastVbNs + per / 2) / per;
                    p.lastVbNs = vbNs;
                    const int64_t d = ns - vbNs;
                    const int64_t refresh = p.refreshCount + (d >= 0 ? d / per : -((-d + per - 1) / per)) + 1; // (the first refresh after now)
                    if (p.lastRefresh != 0)
                    {
                        const int64_t up = refresh - p.lastRefresh; // (the picture before's refreshes on the screen)
                        if (up >= 0 && up < 64)
                            ++p.held[std::min<int64_t>(up, 3)];
                    }
                    p.lastRefresh = refresh;
                }
                else
                    p.lastRefresh = p.lastVbNs = 0;
            }
            view_turn::noteShownPicture(sp.turnId, ns, sp.dueNs);
            if (sp.dueNs != 0)
            {
                p.toScreen.push_back(static_cast<double>(ns - sp.dueNs) / 1.0e6);
                if (sp.readyNs != 0)
                    p.toReady.push_back(static_cast<double>(sp.readyNs - sp.dueNs) / 1.0e6);
            }
            p.unshown += static_cast<uint32_t>(skipped);
            static const std::pair<uint64_t, uint64_t> trace = [] {
                std::pair<uint64_t, uint64_t> t{0u, 0u};
                if (const char *v = std::getenv("TF_PACING_TRACE"))
                {
                    unsigned long long a = 0, n = 0;
                    if (std::sscanf(v, "%llu,%llu", &a, &n) == 2)
                        t = {a, n};
                }
                return t;
            }();
            static uint64_t traced = 0u;
            if (trace.second && sp.tick >= trace.first && traced < trace.second)
            {
                ++traced;
                std::fprintf(stderr, "[pace] tick %llu due %.3f ready +%.2f shown +%.2f (skipped %llu)\n",
                             static_cast<unsigned long long>(sp.tick), static_cast<double>(sp.dueNs) / 1.0e6,
                             static_cast<double>(sp.readyNs - sp.dueNs) / 1.0e6, static_cast<double>(ns - sp.dueNs) / 1.0e6,
                             static_cast<unsigned long long>(skipped));
            }
            const double seconds = std::chrono::duration<double>(now - p.since).count();
            if (seconds >= 2.0)
            {
                printPacing(seconds);
                p.since = now;
            }
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

        int paceWait(std::chrono::steady_clock::time_point until)
        {
            if (!g_pacedLive.load(std::memory_order_relaxed))
                return -1;
            int64_t lastNs = 0;
            if (backend().waitPicture(g_takenSerial.load(std::memory_order_relaxed),
                                 std::chrono::duration_cast<std::chrono::nanoseconds>(until.time_since_epoch()).count(), lastNs))
                return 1;
            if (lastNs == 0 || nowNs() - lastNs > 100000000ll)
            {
                g_pacedLive.store(false, std::memory_order_relaxed);
                return -1;
            }
            return 0;
        }

        void pacePresented()
        {
            if (g_pendingShown)
            {
                g_pendingShown = false;
                noteShown(g_pendingPicture, g_pendingSkipped);
            }
        }

        bool provide(unsigned int &texture, int &width, int &height)
        {
            static Texture2D shown{};
            static std::vector<uint32_t> pixels;
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
            backend().setWindowLines(GetRenderHeight()); // (the picture drawn at the window's own lines)
            // The shared textures, on the first frame (the window's GL context is current here).
            static int share = 0;
            if (share == 0)
            {
                const char *v = std::getenv("TF_NATIVE_SHARE");
                share = (!v || *v != '0') && glshare::available() ? 1 : -1;
                backend().setSharedPictures(share == 1);
            }
            struct Imported
            {
                unsigned texture = 0u, memory = 0u;
                uint64_t generation = 0;
            };
            static Imported imported[d3d::kSharedPictures]; // (every one the renderer may make: TODO 98)
            static int sharedShown = -1;
            static uint32_t sharedW = 0, sharedH = 0;
            d3d::SharedPicture sp;
            const uint64_t serialBefore = serial;
            bool fresh = false;
            if (share == 1 && backend().latestShared(sp, serial) && sp.index >= 0 && sp.index < d3d::kSharedPictures)
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
                        backend().setSharedPictures(false);
                    }
                }
                if (sp.stamp)
                    std::fprintf(stderr, "[present] window takes stamp %u (texture %d)\n", sp.stamp, sp.index);
                if (im.texture)
                {
                    sharedShown = sp.index;
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
                                backend().setSharedPictures(false);
                            }
                        }
                    }
                }
                else
                    backend().sharedHeld(sharedShown); // (not imported: the one still shown stays held - latestShared marked the new one)
            }
            else if (share != 1 && backend().latestPicture(pixels, w, h, serial, has3D))
            {
                // (taken: the pacer waits for the next one - paceWait. It was left at the last shared picture taken
                // before the CPU hand-over took over, so every pass of the window found "a new picture": the same one
                // was drawn ~190 times a second and the FPS counter counted each.)
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
            const bool nativeAny = shown.id != 0u || sharedNow;
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
                    noteShown(sp, skipped);
                else if (pacingLogOn())
                    ++g_pacing.repeats;
                g_pacedLive.store(true, std::memory_order_relaxed);
                texture = imported[sharedShown].texture;
                width = static_cast<int>(sharedW);
                height = static_cast<int>(sharedH);
                return true;
            }
            // The CPU hand-over's picture: paced and logged as a shared one (shown when it is handed over).
            {
                const uint64_t skipped = serialBefore != 0u && serial > serialBefore + 1u ? serial - serialBefore - 1u : 0u;
                if (fresh && g_pacerOn)
                {
                    g_pendingShown = true;
                    g_pendingPicture = d3d::SharedPicture{};
                    g_pendingSkipped = skipped;
                }
                else if (fresh)
                    noteShown(d3d::SharedPicture{}, skipped);
                else if (pacingLogOn())
                    ++g_pacing.repeats;
                g_pacedLive.store(true, std::memory_order_relaxed);
            }
            texture = shown.id;
            width = shown.width;
            height = shown.height;
            return true;
        }
    }

    void startLivePicture()
    {
        const char *live = std::getenv("TF_NATIVE_LIVE");
        if (!live || *live != '1')
            return;
        ps2_host_display::setPictureProvider(&provide);
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
