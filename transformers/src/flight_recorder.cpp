// (TODO 405) The flight recorder: flight_recorder.h.
//
// Two rings, made once at start: the game's (one entry a game frame - the camera as the boom left it, the player, the
// mouse, the frame's time) and the native renderer's (one entry a frame drawn - its new textures, the texture uses the
// cap put off, the stand-ins drawn, the draws and a signature of them: the meshes' and textures' addresses mixed, a
// cheap stand-in for a hash of the picture - a view from inside a wall draws other meshes). Nothing is written while
// nothing fires. A detector that fires asks for a dump one second later (the aftermath kept too); the dump copies both
// rings (a few hundred microseconds) and a thread of its own writes runs\flight_<time>.log. Automatic dumps at most
// every TF_FLIGHT_GAP (15) seconds and TF_FLIGHT_MAX (12) a session; F10 always (and with a save state of the moment).
//
// Detectors (each its own TF_ switch for a threshold, 0 = off):
//  - jump: the boom let out by more than TF_FLIGHT_JUMP (2) units in one frame, as the game wanted it (before the release
//    of TODO 405 holds it);
//  - chatter: the camera's collision sweep hitting and missing by turns - TF_FLIGHT_CHATTER (6) changes in 12 frames;
//  - leap: the eye moving more than TF_FLIGHT_LEAP (3) units in a frame and back the next, its target still;
//  - stand-ins: a frame drawn with TF_FLIGHT_STANDINS (64) stand-in textures or more;
//  - put off: a frame drawn with TF_FLIGHT_PUTOFF (32) texture uses or more put off by the cap.
//  (the last two only after TF_FLIGHT_SETTLE (5) s of a level's camera: a load makes them by the hundred)
//  - below: the game's eye, or the lowest a turned picture showed, within TF_FLIGHT_BELOW (0.3) of the ground the game's own
//    height check (0x2C9F10) found under it.

#include "flight_recorder.h"
#include "mod_menu.h" // (TODO 433: F10, every press)

#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "game_version.h"
#include "ultrawide.h"

#include "raylib.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace flight_recorder
{
    namespace
    {
        struct GameEntry
        {
            uint64_t tick;
            double wall;   // (seconds since the recorder started)
            float frameMs; // (since the frame before)
            Camera cam;
            uint8_t camSeen, cutByte;
            float player[3];
            float mouse[2];
            float groundZ, lifted;     // (the game's height check: the ground under the eye - NAN if none - and its lift)
            float pictureZ;            // (the lowest eye a picture showed this frame - NAN: no turned picture)
            float pictureYaw, picturePitch; // (the largest turn a picture showed beyond the frame, degrees)
        };
        struct RenderEntry
        {
            std::atomic<uint64_t> tick{0};
            double wall = 0.0;
            uint32_t newTextures = 0, putOff = 0, standIns = 0, draws = 0;
            uint64_t signature = 0;
        };

        bool g_on = false;
        float g_jump = 2.0f, g_leap = 3.0f;
        int g_chatter = 6, g_gapSeconds = 15, g_max = 12;
        uint32_t g_standInBurst = 64, g_putOffBurst = 32;
        std::vector<GameEntry> g_game;   // (the ring; g_gameNext the next slot, g_gameCount how many are filled)
        size_t g_gameNext = 0, g_gameCount = 0;
        std::vector<RenderEntry> g_render; // (the render thread's ring)
        std::atomic<size_t> g_renderNext{0};
        Camera g_cam{};
        bool g_camSeen = false;
        float g_mouse[2] = {};
        std::atomic<float> g_radius{0.0f}, g_fov{0.0f};
        std::atomic<bool> g_keyAsked{false};
        float g_groundZ = std::nanf(""), g_lifted = 0.0f, g_below = 0.3f;
        std::atomic<float> g_pictureZ{1e30f}, g_pictureYaw{0.0f}, g_picturePitch{0.0f};
        std::atomic<int> g_renderFired{0}; // (1 stand-ins, 2 put off: from the render thread)
        std::chrono::steady_clock::time_point g_start, g_lastFrame;
        // (a dump asked for: its reason, when it is due, whether a state goes with it)
        std::string g_pendingWhy;
        double g_pendingAt = -1.0, g_lastAuto = -1e9;
        bool g_pendingState = false;
        int g_autoDumps = 0;
        unsigned g_hitBits = 0; // (the sweep's hit, one bit a frame, the newest lowest)
        // (the render detectors wait TF_FLIGHT_SETTLE (5) s of a level's camera running without a gap of over half a
        // second: a level or a state loading makes stand-ins and put-off uses by the hundred, not worth a file)
        double g_cameraSince = -1.0, g_cameraLast = -1.0, g_settle = 5.0;

        double since() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count(); }

        std::string stamp()
        {
            const std::time_t t = std::time(nullptr);
            std::tm tm{};
#if defined(_WIN32)
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            char b[32];
            std::strftime(b, sizeof(b), "%Y%m%d-%H%M%S", &tm);
            return b;
        }

        float dist(const float *a, const float *b)
        {
            return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
        }

        const GameEntry *back(size_t k) // (k frames before the newest; null if not kept)
        {
            if (k >= g_gameCount)
                return nullptr;
            return &g_game[(g_gameNext + g_game.size() - 1u - k) % g_game.size()];
        }

        void ask(const std::string &why, bool automatic, bool state)
        {
            const double now = since();
            if (automatic)
            {
                if (g_autoDumps >= g_max || now - g_lastAuto < g_gapSeconds || g_pendingAt >= 0.0)
                    return;
                g_lastAuto = now;
                ++g_autoDumps;
            }
            if (g_pendingAt >= 0.0) // (one already due: this joins it)
            {
                g_pendingWhy += "; " + why;
                g_pendingState = g_pendingState || state;
                return;
            }
            g_pendingWhy = why;
            g_pendingAt = now + (state ? 0.0 : 1.0); // (the key: at once; a detector: a second of aftermath)
            g_pendingState = state;
            std::fprintf(stderr, "[flight] %s at %.1f s: the last %zu frames will be written%s\n", why.c_str(), now, g_gameCount,
                         state ? " (and a save state of this moment)" : " (in a second)");
        }

        void dump(PS2Runtime &runtime)
        {
            const std::string name = "flight_" + stamp();
            std::error_code ec;
            std::filesystem::create_directories("runs", ec);
            const std::string path = "runs/" + name + ".log";
            if (g_pendingState)
            {
                const std::filesystem::path state = std::filesystem::absolute("runs/" + name + ".ps2state", ec);
                runtime.eeScheduler().requestSaveState(state);
            }
            // The copies (this thread, the rings' owner for the game's; the render ring read as it stands).
            std::vector<GameEntry> game;
            game.reserve(g_gameCount);
            for (size_t k = g_gameCount; k-- > 0;)
                game.push_back(*back(k));
            struct R
            {
                uint64_t tick;
                double wall;
                uint32_t newTextures, putOff, standIns, draws;
                uint64_t signature;
            };
            std::vector<R> render;
            render.reserve(g_render.size());
            for (const RenderEntry &e : g_render)
                if (const uint64_t t = e.tick.load(std::memory_order_acquire))
                    render.push_back({t, e.wall, e.newTextures, e.putOff, e.standIns, e.draws, e.signature});
            std::sort(render.begin(), render.end(), [](const R &a, const R &b) { return a.wall < b.wall; });
            const std::string why = g_pendingWhy;
            const float radius = g_radius.load(), fov = g_fov.load(), wide = ultrawide::wideFactor();
            std::thread([path, why, radius, fov, wide, game = std::move(game), render = std::move(render)]() {
                std::FILE *f = std::fopen(path.c_str(), "w");
                if (!f)
                    return;
                std::fprintf(f, "# flight recorder (TODO 405): %s\n", why.c_str());
                std::fprintf(f, "# camera collision: sweeps of radius %.3f and %.3f, 1.25 back along the boom; field of view %.2f, widened x%.3f "
                                "for the window; near plane 0.5 (half-wide %.3f at the game's view, %.3f at the window's)\n",
                             0.25f * radius, 0.5f * radius, fov, wide, 0.5f * std::tan(0.5f * fov * 0.0174533f),
                             0.5f * wide * std::tan(0.5f * fov * 0.0174533f));
                std::fprintf(f, "# game: tick,wall_s,frame_ms,eye_x,eye_y,eye_z,target_x,target_y,target_z,boom_before,boom_game,boom_shown,wanted,"
                                "sweep,scene_hits,model_hits,329,32A,held,cut_byte,camera_seen,player_x,player_y,player_z,mouse_x,mouse_y,ground_z,lifted,"
                                "picture_eye_z,picture_yaw,picture_pitch\n");
                for (const GameEntry &e : game)
                    std::fprintf(f, "g,%llu,%.4f,%.2f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%u,%u,%u,%u,%u,%u,%.3f,%.3f,%.3f,%.1f,%.1f,%.3f,%.3f,%.3f,%.2f,%.2f\n",
                                 static_cast<unsigned long long>(e.tick), e.wall, e.frameMs, e.cam.eye[0], e.cam.eye[1], e.cam.eye[2],
                                 e.cam.target[0], e.cam.target[1], e.cam.target[2], e.cam.boomBefore, e.cam.boomGame, e.cam.boom, e.cam.wanted,
                                 e.cam.sweep, e.cam.sceneHits, e.cam.modelHits, e.cam.arrived329, e.cam.cut32A, e.cam.held, e.cutByte, e.camSeen,
                                 e.player[0], e.player[1], e.player[2], e.mouse[0], e.mouse[1], e.groundZ, e.lifted, e.pictureZ, e.pictureYaw,
                                 e.picturePitch);
                std::fprintf(f, "# render (frames drawn): tick,wall_s,new_textures,put_off,stand_ins,draws,signature\n");
                for (const R &r : render)
                    std::fprintf(f, "r,%llu,%.4f,%u,%u,%u,%u,%016llx\n", static_cast<unsigned long long>(r.tick), r.wall, r.newTextures, r.putOff,
                                 r.standIns, r.draws, static_cast<unsigned long long>(r.signature));
                std::fclose(f);
                std::fprintf(stderr, "[flight] written %s (%zu game frames, %zu drawn)\n", path.c_str(), game.size(), render.size());
            }).detach();
        }
    }

    void install(PS2Runtime &)
    {
        const char *on = std::getenv("TF_FLIGHT_RECORDER");
        g_on = on && *on == '1';
        if (!g_on)
            return;
        const auto num = [](const char *name, double fallback) {
            const char *v = std::getenv(name);
            return v && *v ? std::atof(v) : fallback;
        };
        g_jump = static_cast<float>(num("TF_FLIGHT_JUMP", 2.0));
        g_below = static_cast<float>(num("TF_FLIGHT_BELOW", 0.3));
        g_leap = static_cast<float>(num("TF_FLIGHT_LEAP", 3.0));
        g_chatter = static_cast<int>(num("TF_FLIGHT_CHATTER", 6));
        g_standInBurst = static_cast<uint32_t>(num("TF_FLIGHT_STANDINS", 64));
        g_putOffBurst = static_cast<uint32_t>(num("TF_FLIGHT_PUTOFF", 32));
        g_gapSeconds = static_cast<int>(num("TF_FLIGHT_GAP", 15));
        g_settle = num("TF_FLIGHT_SETTLE", 5.0);
        g_max = static_cast<int>(num("TF_FLIGHT_MAX", 12));
        const size_t frames = static_cast<size_t>(std::clamp(num("TF_FLIGHT_FRAMES", 12288), 600.0, 100000.0)); // (~20 s at 600 frames a second)
        g_game.assign(frames, GameEntry{});
        g_render = std::vector<RenderEntry>(4096u);
        g_start = g_lastFrame = std::chrono::steady_clock::now();
        std::fprintf(stderr, "[flight] TF_FLIGHT_RECORDER: the last %zu game frames kept in memory; written to runs\\flight_<time>.log on F10 (with a "
                             "save state) or when a detector fires (jump %.1f, chatter %d, leap %.1f, stand-ins %u, put off %u; 0 = off)\n",
                     frames, g_jump, g_chatter, g_leap, g_standInBurst, g_putOffBurst);
    }

    bool enabled() { return g_on; }

    void camera(const Camera &c)
    {
        if (!g_on)
            return;
        g_cam = c;
        g_camSeen = true;
    }

    void noteRadius(float radius)
    {
        if (g_on && radius > g_radius.load(std::memory_order_relaxed))
            g_radius.store(radius, std::memory_order_relaxed);
    }

    void noteFieldOfView(float degrees)
    {
        if (g_on && degrees > 1.0f && degrees < 179.0f)
            g_fov.store(degrees, std::memory_order_relaxed);
    }

    void noteGround(bool found, float groundZ, float lifted)
    {
        if (!g_on)
            return;
        g_groundZ = found ? groundZ : std::nanf("");
        g_lifted = lifted;
    }

    void notePicture(float eyeZ, float yawDegrees, float pitchDegrees)
    {
        if (!g_on)
            return;
        for (float z = g_pictureZ.load(std::memory_order_relaxed); eyeZ < z && !g_pictureZ.compare_exchange_weak(z, eyeZ, std::memory_order_relaxed);)
            ;
        for (float v = g_pictureYaw.load(std::memory_order_relaxed), a = std::fabs(yawDegrees);
             a > v && !g_pictureYaw.compare_exchange_weak(v, a, std::memory_order_relaxed);)
            ;
        for (float v = g_picturePitch.load(std::memory_order_relaxed), a = std::fabs(pitchDegrees);
             a > v && !g_picturePitch.compare_exchange_weak(v, a, std::memory_order_relaxed);)
            ;
    }

    void noteMouse(float x, float y)
    {
        if (!g_on)
            return;
        g_mouse[0] += x;
        g_mouse[1] += y;
    }

    void frame(uint8_t *rdram, PS2Runtime &runtime, uint64_t tick, const float player[3])
    {
        if (!g_on)
            return;
        const auto now = std::chrono::steady_clock::now();
        GameEntry &e = g_game[g_gameNext];
        e.tick = tick;
        e.wall = since();
        e.frameMs = static_cast<float>(std::chrono::duration<double, std::milli>(now - g_lastFrame).count());
        g_lastFrame = now;
        e.cam = g_cam;
        e.camSeen = g_camSeen ? 1u : 0u;
        e.cutByte = rdram[TF_GA(0x006CF8BCu)]; // (taCamera::UpdateScripts's cut byte)
        std::memcpy(e.player, player, sizeof(e.player));
        e.mouse[0] = g_mouse[0];
        e.mouse[1] = g_mouse[1];
        e.groundZ = g_groundZ;
        e.lifted = g_lifted;
        g_groundZ = std::nanf("");
        g_lifted = 0.0f;
        const float pz = g_pictureZ.exchange(1e30f, std::memory_order_relaxed);
        e.pictureZ = pz < 1e29f ? pz : std::nanf("");
        e.pictureYaw = g_pictureYaw.exchange(0.0f, std::memory_order_relaxed);
        e.picturePitch = g_picturePitch.exchange(0.0f, std::memory_order_relaxed);
        g_mouse[0] = g_mouse[1] = 0.0f;
        const bool seen = g_camSeen;
        if (seen)
        {
            if (g_cameraLast < 0.0 || e.wall - g_cameraLast > 0.5)
                g_cameraSince = e.wall;
            g_cameraLast = e.wall;
        }
        g_camSeen = false;
        g_gameNext = (g_gameNext + 1u) % g_game.size();
        g_gameCount = std::min(g_gameCount + 1u, g_game.size());

        // The detectors (the camera ones only on frames the camera ran).
        if (seen)
        {
            const Camera &c = e.cam;
            if (g_jump > 0.0f && c.boomBefore > 0.0f && c.boomGame > c.boomBefore + g_jump)
            {
                char b[160];
                std::snprintf(b, sizeof(b), "jump: the boom let out %.2f -> %.2f in one frame at tick %llu%s", c.boomBefore, c.boomGame,
                              static_cast<unsigned long long>(tick), c.held ? " (held by the release)" : "");
                ask(b, true, false);
            }
            const bool hit = c.sweep >= 0.0f && c.sweep < c.wanted - 0.05f;
            g_hitBits = (g_hitBits << 1) | (hit ? 1u : 0u);
            if (g_chatter > 0)
            {
                unsigned changes = 0u;
                for (unsigned d = (g_hitBits ^ (g_hitBits >> 1)) & 0x7FFu; d; d &= d - 1u)
                    ++changes;
                if (static_cast<int>(changes) >= g_chatter)
                {
                    char b[128];
                    std::snprintf(b, sizeof(b), "chatter: the camera's collision hit and missed %u times in 12 frames at tick %llu", changes,
                                  static_cast<unsigned long long>(tick));
                    ask(b, true, false);
                }
            }
            // below: the eye (the game's, or the lowest a turned picture showed) under the ground the game's check found
            if (g_below > 0.0f && !std::isnan(e.groundZ))
            {
                const float shown = std::isnan(e.pictureZ) ? c.eye[2] : std::min(e.pictureZ, c.eye[2]);
                // (a "ground" far above the eye is a roof, a bridge or a canopy the probe started inside - not the ground)
                if (shown < e.groundZ + g_below && e.groundZ < c.eye[2] + 5.0f)
                {
                    char b[200];
                    std::snprintf(b, sizeof(b), "below: the eye %.2f (the game's %.2f, lifted %.2f) against the ground %.2f at tick %llu%s", shown,
                                  c.eye[2], e.lifted, e.groundZ, static_cast<unsigned long long>(tick),
                                  !std::isnan(e.pictureZ) && e.pictureZ < c.eye[2] - 0.01f ? " (a turned picture)" : "");
                    ask(b, true, false);
                }
            }
            if (g_leap > 0.0f)
                if (const GameEntry *b1 = back(1), *b2 = back(2); b1 && b2 && b1->camSeen && b2->camSeen)
                {
                    const float out = dist(b1->cam.eye, b2->cam.eye), home = dist(c.eye, b2->cam.eye), still = dist(c.target, b2->cam.target);
                    if (out > g_leap && home < 0.5f * out && still < 1.0f)
                    {
                        char b[160];
                        std::snprintf(b, sizeof(b), "leap: the eye moved %.2f in one frame and back (to %.2f) at tick %llu", out, home,
                                      static_cast<unsigned long long>(tick));
                        ask(b, true, false);
                    }
                }
        }
        if (const int r = g_renderFired.exchange(0, std::memory_order_acq_rel); r && g_cameraSince >= 0.0 && e.wall - g_cameraLast < 0.5 &&
                                                                                    e.wall - g_cameraSince >= g_settle)
            ask(r == 1 ? "stand-ins: a frame drawn with many stand-in textures" : "put off: a frame drawn with many texture uses put off by the cap", true, false);
        if (g_keyAsked.exchange(false, std::memory_order_acq_rel))
            ask("F10 pressed", false, true);
        if (g_pendingAt >= 0.0 && since() >= g_pendingAt)
        {
            dump(runtime);
            g_pendingAt = -1.0;
            g_pendingState = false;
        }
    }

    void renderFrame(uint64_t tick, uint32_t newTextures, uint32_t putOff, uint32_t standIns, uint32_t draws, uint64_t signature)
    {
        if (!g_on)
            return;
        RenderEntry &e = g_render[g_renderNext.fetch_add(1u, std::memory_order_relaxed) % g_render.size()];
        e.tick.store(0u, std::memory_order_relaxed);
        e.wall = since();
        e.newTextures = newTextures;
        e.putOff = putOff;
        e.standIns = standIns;
        e.draws = draws;
        e.signature = signature;
        e.tick.store(tick ? tick : 1u, std::memory_order_release);
        if (g_standInBurst && standIns >= g_standInBurst)
            g_renderFired.store(1, std::memory_order_release);
        else if (g_putOffBurst && putOff >= g_putOffBurst)
            g_renderFired.store(2, std::memory_order_release);
    }

    void windowFrame()
    {
        // (TODO 433: every F10 press since the last picture - raylib's IsKeyPressed, read once a picture, saw only the
        // window's last ~1 ms input poll)
        if (g_on && mod_menu::takePictureKeyPresses(KEY_F10) > 0)
            g_keyAsked.store(true, std::memory_order_release);
    }
}
