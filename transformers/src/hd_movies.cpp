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
                if (!fileShape)
                    width = (static_cast<int>(height * movieShape(key) + 0.5f) + 1) & ~1;
                if (hd_movies_decoder::start(file, width, height, fps))
                {
                    haveFrame = false;
                    g_width = width;
                    g_height = height;
                    g_playing = true;
                    g_shownFrames = 0u;
                    g_shownSince = std::chrono::steady_clock::now();
                    std::fprintf(stderr, "[hd movies] %s: %s (%dx%d, %.3f a second)\n", key.c_str(), file.c_str(), width, height, fps);
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
        const float sw = static_cast<float>(GetScreenWidth()), sh = static_cast<float>(GetScreenHeight());
        const float scale = std::min(sw / static_cast<float>(g_width), sh / static_cast<float>(g_height));
        const float w = g_width * scale, h = g_height * scale;
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), BLACK);
        if (haveFrame)
            DrawTexturePro(texture, Rectangle{0, 0, static_cast<float>(g_width), static_cast<float>(g_height)},
                       Rectangle{(sw - w) * 0.5f, (sh - h) * 0.5f, w, h}, Vector2{0, 0}, 0.0f, WHITE);
#else
        (void)runtime;
#endif
    }
}
