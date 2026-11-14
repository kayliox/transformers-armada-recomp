// HD movies (hd_movies.h): upscaled versions of the game's movies, shown over its picture.
//
// The game decodes its movies (.PSS / .M2V, MPEG-2 at 640x384 to 640x448) into its own buffers at
// the console's size, so an upscaled movie cannot go through it. Instead, while the game plays a
// movie (fmvStreamPS2 alive: overrides.cpp's createFmvStream / destroyFmvStream), the port plays
// the upscaled file for that movie over the window, its sound left to the game:
//   - the file: <folder>\<the movie's path under FMV, no extension>[_reforged].<mkv|mp4|webm>, e.g.
//     movies_hd\CG\EP01\N1_1.mkv (TF_HD_MOVIES=<folder>, "movies_hd" by default; =0: off);
//   - decoded by ffmpeg (hd_movies_decoder.cpp; TF_FFMPEG=<ffmpeg.exe>) at the window's height, at
//     most 1440 lines;
//   - timed by the console's VBlanks since the movie began (movies run at the console's timing),
//     so a pause or a slow host keeps it in step with the game's own sound.
// Nothing changes for a movie with no upscaled file.

#include "hd_movies.h"
#include "hd_movies_decoder.h"

#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>

#if defined(PS2X_ENABLE_DEBUG_UI)
#include "raylib.h"
#endif

namespace fs = std::filesystem;

namespace hd_movies
{
    namespace
    {
        std::mutex g_mutex;
        std::string g_pendingName; // set on the game thread, opened on the window thread
        bool g_pendingStart = false, g_pendingEnd = false;
        // (TODO 201) A movie that ended before the window thread took its start: never begun (its name, for the log).
        std::string g_cancelledName;
        // (the frames of the movie shown, logged at its end: how well it kept its pace)
        uint32_t g_shownFrames = 0u;
        std::chrono::steady_clock::time_point g_shownSince{};
        // (TF_HD_MOVIE_LAG=<ms>, a test aid: the window thread takes the game's starts and ends only every <ms>, as a
        // window thread held back by a busy PC)
        const int g_takeLagMs = [] {
            const char *v = std::getenv("TF_HD_MOVIE_LAG");
            return v && *v ? std::clamp(std::atoi(v), 0, 60000) : 0;
        }();
        std::atomic<uint64_t> g_startTick{0};
        std::atomic<bool> g_playing{false};
        std::atomic<float> g_shownShape{0.0f}; // (TODO 341: the band's shape on the screen now - setShownShape)
        // (TODO 341, the follow-up) The share of the decoded picture's height that is the movie's own black bar, top and
        // bottom each, when the copy holds the whole 384-row frame (0: the copy is the picture alone). The window thread's.
        float g_barShare = 0.0f;
        int g_width = 0, g_height = 0;

        std::string folder()
        {
            const char *env = std::getenv("TF_HD_MOVIES");
            if (env && env[0] == '0' && env[1] == '\0')
                return "";
            return env && *env ? env : "movies_hd";
        }
    }

    // "cdrom0:\\FMV\\CG\\EP01\\N1_1.PSS;1" -> "CG/EP01/N1_1"
    std::string movieKey(std::string name)
    {
        for (char &c : name)
            if (c == '\\')
                c = '/';
        std::string upper = name;
        for (char &c : upper)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        const size_t fmv = upper.find("FMV/");
        if (fmv != std::string::npos)
            name = name.substr(fmv + 4);
        const size_t semi = name.find(';');
        if (semi != std::string::npos)
            name = name.substr(0, semi);
        const size_t dot = name.find_last_of('.');
        const size_t slash = name.find_last_of('/');
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
            name = name.substr(0, dot);
        while (!name.empty() && name.front() == '/')
            name.erase(name.begin());
        return name;
    }

    std::string fileFor(const std::string &key)
    {
        const std::string dir = folder();
        if (dir.empty() || key.empty())
            return "";
        for (const char *suffix : {"", ".reforge", "_reforged"}) // (Reforge writes <name>.reforge.mkv)
            for (const char *ext : {".mkv", ".mp4", ".webm"})
            {
                const fs::path p = fs::path(dir) / (key + suffix + ext);
                std::error_code e;
                if (fs::exists(p, e))
                    return p.string();
            }
        return "";
    }

    void movieStarted(PS2Runtime &runtime, const std::string &name)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pendingName = name;
        g_pendingStart = true;
        g_startTick = runtime.eeScheduler().currentVSyncTick();
    }

    // (TODO 201, 2026-11-08) The game starts and ends a movie on its own thread; the window's thread opens and closes the
    // upscaled one at its next draw. With the PC busy (a video encode on every core, a build) the window's thread fell
    // seconds behind at the start screens: the game had begun and skipped the attract movie (NATTRACT) before the
    // window's thread came round, it found the start and the end both waiting - and took the end for the movie before
    // (none playing), then began this one. Its end already spent, it played on for good over the main menu: a black
    // screen (the movie's black until its first frame, then the attract movie at the busy PC's decoding speed, ~5 a
    // second) while the menu's music played. Now an end that comes while its start still waits cancels the start: the
    // movie is never begun; an end with nothing waiting closes the one playing, as before.
    void movieEnded()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_pendingStart)
        {
            g_pendingStart = false;
            g_cancelledName = g_pendingName;
        }
        g_pendingEnd = true;
    }

    bool playing() { return g_playing.load(); }

    void setShownShape(float widthOverHeight) { g_shownShape.store(widthOverHeight, std::memory_order_relaxed); }

    bool movieShapeAsPs2()
    {
        static const bool ps2 = [] {
            const char *v = std::getenv("TF_MOVIE_SHAPE");
            return v && (std::tolower(static_cast<unsigned char>(v[0])) == 'p');
        }();
        return ps2;
    }

    float movieShape(const std::string &key)
    {
        const bool bars = key != "NATARI"; // (the Atari logo has no black bars of its own: 384 rows of picture)
        const float rows = bars ? 360.0f : 384.0f;
        // (the PS2's: its 640 x 448 frame filled a 4:3 TV, so a pixel was 0.933 wide)
        return movieShapeAsPs2() ? (4.0f / 3.0f) * (448.0f / rows) : 640.0f / rows;
    }

    void draw(PS2Runtime &runtime)
    {
#if defined(PS2X_ENABLE_DEBUG_UI)
        static Texture2D texture{};
        static bool haveFrame = false; // (the movie's own first frame shown: until then black, not the last movie's)
        std::string start, cancelled;
        bool end = false;
        static auto lastTake = std::chrono::steady_clock::time_point{};
        const auto now = std::chrono::steady_clock::now();
        if (g_takeLagMs <= 0 || now - lastTake >= std::chrono::milliseconds(g_takeLagMs))
        {
            lastTake = now;
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_pendingStart)
                start = g_pendingName;
            g_pendingStart = false;
            end = g_pendingEnd;
            g_pendingEnd = false;
            cancelled.swap(g_cancelledName);
        }
        if (!cancelled.empty())
            std::fprintf(stderr, "[hd movies] %s: ended before the window came to it - not shown (TODO 201)\n", movieKey(cancelled).c_str());
        if ((end || !start.empty()) && g_playing)
        {
            hd_movies_decoder::stop();
            g_playing = false;
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_shownSince).count();
            std::fprintf(stderr, "[hd movies] end: %u frames shown in %.1f s (%.1f a second)\n", g_shownFrames, secs,
                         secs > 0.0 ? g_shownFrames / secs : 0.0);
        }
        if (!start.empty())
        {
            const std::string key = movieKey(start);
            const std::string file = fileFor(key);
            if (!file.empty())
            {
                const int height = std::clamp(GetScreenHeight(), 360, 1440) & ~1;
                int width = 0;
                double fps = 30.0;
                hd_movies_decoder::probe(file, height, width, fps);
                // (TODO 341 (4)) Decoded at the movie's own shape, not the file's (the reforged films' 1.42:1 squeezed the
                // picture); TF_MOVIE_SHAPE=file: the file's, as before.
                static const bool fileShape = std::getenv("TF_MOVIE_SHAPE") && std::tolower(static_cast<unsigned char>(*std::getenv("TF_MOVIE_SHAPE"))) == 'f';
                // (TODO 341, the follow-up) The copies come in two layouts: the picture alone (the films: 1.42:1 files, no
                // bars), and the movie's whole 640 x 384 frame with its own 12 black rows above and below the picture (the
                // Melbourne House logo: a 5:3 file). The second kind was squeezed into the picture's rectangle bars and
                // all - the logo 6% flat with two thin black bars. It is decoded at the frame's own shape now and drawn
                // without its bar rows. The Atari logo (384 rows of picture) has none. (The level previews' copies are not
                // shown here: they play inside the HQ's scene, nr_movie.cpp.)
                const float fileIs = static_cast<float>(width) / static_cast<float>(height);
                const float wholeFrame = 640.0f / 384.0f;
                // (told by the file's shape: 1.60 to 1.74 is the whole frame - its stream says 1856 x 1082 for a shown
                // 1800 x 1080 - a film's copy is 1.42, a 16:9 copy 1.78)
                const bool hasBars = key != "NATARI" && !fileShape && fileIs > 1.60f && fileIs < 1.74f;
                g_barShare = hasBars ? (key.find("NPSA") != std::string::npos ? 13.0f : 12.0f) / 384.0f : 0.0f;
                if (hasBars)
                    width = (static_cast<int>(height * wholeFrame + 0.5f) + 1) & ~1;
                else if (!fileShape)
                    width = (static_cast<int>(height * movieShape(key) + 0.5f) + 1) & ~1;
                if (hd_movies_decoder::start(file, width, height, fps))
                {
                    haveFrame = false;
                    g_width = width;
                    g_height = height;
                    g_playing = true;
                    g_shownFrames = 0u;
                    g_shownSince = std::chrono::steady_clock::now();
                    std::fprintf(stderr, "[hd movies] %s: %s (%dx%d, %.3f a second%s)\n", key.c_str(), file.c_str(), width, height, fps,
                                 g_barShare > 0.0f ? "; the whole frame: shown without its black bars" : "");
                }
            }
            else
                std::fprintf(stderr, "[hd movies] %s: none in %s\n", key.c_str(), folder().c_str());
        }
        if (!g_playing)
            return;
        if (texture.id == 0 || texture.width != g_width || texture.height != g_height)
        {
            if (texture.id != 0)
                UnloadTexture(texture);
            Image blank = GenImageColor(g_width, g_height, BLACK);
            texture = LoadTextureFromImage(blank);
            UnloadImage(blank);
            SetTextureFilter(texture, TEXTURE_FILTER_BILINEAR);
        }
        // The frame for now: the console's VBlanks since the movie began.
        const double moment = static_cast<double>(runtime.eeScheduler().currentVSyncTick() - g_startTick.load()) / 59.94;
        static hd_movies_decoder::Frame frame;
        if (hd_movies_decoder::take(moment, frame) && frame.rgba.size() == static_cast<size_t>(g_width) * g_height)
        {
            UpdateTexture(texture, frame.rgba.data());
            haveFrame = true;
            ++g_shownFrames;
        }
        // (TODO 341) Over the rectangle the game's own movie has (the window's presenter, ps2_runtime.cpp): the band's shape
        // "Menus and movies" asks for (overrides.cpp movieFrameAspect), as large as the window allows, in its middle - the
        // same picture with and without an HD copy. Before it is told (0): the shape it was decoded at.
        const float sw = static_cast<float>(GetScreenWidth()), sh = static_cast<float>(GetScreenHeight());
        const float told = g_shownShape.load(std::memory_order_relaxed);
        const float pictureTop = g_barShare * static_cast<float>(g_height);
        const float pictureRows = static_cast<float>(g_height) - 2.0f * pictureTop;
        const float shape = told > 0.5f ? told : static_cast<float>(g_width) / pictureRows;
        const float w = std::min(sw, sh * shape), h = w / shape;
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), BLACK);
        if (haveFrame)
            DrawTexturePro(texture, Rectangle{0, pictureTop, static_cast<float>(g_width), pictureRows},
                       Rectangle{(sw - w) * 0.5f, (sh - h) * 0.5f, w, h}, Vector2{0, 0}, 0.0f, WHITE);
#else
        (void)runtime;
#endif
    }
}
