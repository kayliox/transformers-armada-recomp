// N9's camera turn for every picture - see view_turn.h.
#include "view_turn.h"

#include "cam_trace.h"
#include "camera_mode.h"
#include "photo_mode.h"
#include "playrec.h"
#include "port.h"
#include "ps2_runtime.h"
#include "static_aim.h"
#include "runtime/ps2_host_input.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace view_turn
{
    namespace
    {
        constexpr float kRadiansPerPixel = 2.35619f / 1200.0f; // (overrides.cpp's mouse look: the same angle a pixel)
        constexpr double kPi = 3.14159265358979323846;

        int64_t nowNs()
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        bool envOn(const char *name, bool fallback)
        {
            const char *v = std::getenv(name);
            return !v || !*v ? fallback : *v != '0';
        }

        // ---- the mouse's travel, read every 1 ms, with its times ----------------------------------------------
        // The window's loop adds the mouse's motion to ps2_host_input's travel as it reads it (every 1 ms); a thread of
        // ours takes it from there as often and keeps the total so far with the moment of each reading, so a picture can
        // ask for the travel at any moment of the last two seconds. The game's own take (once a frame) gets what came
        // since its last take - the same travel it got before - and the take's place in that history is its anchor.
        struct MouseSample
        {
            int64_t ns;
            double x, y;
        };
        struct Mouse
        {
            std::mutex mutex;
            std::deque<MouseSample> history; // (2 s)
            double x = 0.0, y = 0.0;         // the travel so far (pixels, as ps2_host_input gives it)
            double takenX = 0.0, takenY = 0.0;
            int64_t takenNs = 0;
            bool captured = false;
            bool started = false;
            bool passThrough = false; // (PS2X_MOUSE_TEST: its own travel per take - not read from another thread)
            std::atomic<float> testX{0.0f}, testY{0.0f};
            std::atomic<bool> testOn{false};
            int64_t testLastNs = 0;
        } g_mouse;

        // (the mutex held) The window's travel since the last reading, and the sample.
        void pullLocked(int64_t t)
        {
            Mouse &m = g_mouse;
            if (m.testOn.load(std::memory_order_relaxed))
            {
                if (m.testLastNs != 0)
                {
                    const double dt = static_cast<double>(t - m.testLastNs) / 1.0e9;
                    m.x += m.testX.load(std::memory_order_relaxed) * dt;
                    m.y += m.testY.load(std::memory_order_relaxed) * dt;
                }
                m.testLastNs = t;
                m.captured = true;
                float dx = 0.0f, dy = 0.0f;
                ps2_host_input::takeMouseTravel(dx, dy); // (the real mouse's dropped meanwhile)
            }
            else
            {
                m.testLastNs = 0;
                float dx = 0.0f, dy = 0.0f;
                m.captured = ps2_host_input::takeMouseTravel(dx, dy);
                m.x += dx;
                m.y += dy;
            }
            if (!m.history.empty() && m.history.back().ns >= t)
                return;
            m.history.push_back(MouseSample{t, m.x, m.y});
            while (!m.history.empty() && t - m.history.front().ns > 2000000000ll)
                m.history.pop_front();
        }

        void mouseThread()
        {
#if defined(_WIN32)
            SetThreadDescription(GetCurrentThread(), L"ViewTurnMouse");
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                std::lock_guard<std::mutex> lock(g_mouse.mutex);
                pullLocked(nowNs());
            }
        }

        // The travel so far at moment `t` (interpolated between readings; a moment after the newest: read now).
        void travelAt(int64_t t, double &x, double &y)
        {
            Mouse &m = g_mouse;
            std::lock_guard<std::mutex> lock(m.mutex);
            const int64_t now = nowNs();
            if (m.history.empty() || t >= m.history.back().ns)
            {
                if (t >= now - 200000ll)
                    pullLocked(now);
                x = m.x;
                y = m.y;
                return;
            }
            if (t <= m.history.front().ns)
            {
                x = m.history.front().x;
                y = m.history.front().y;
                return;
            }
            const auto it = std::lower_bound(m.history.begin(), m.history.end(), t,
                                             [](const MouseSample &s, int64_t v) { return s.ns < v; });
            const MouseSample &b = *it, &a = *(it - 1);
            const double f = b.ns > a.ns ? static_cast<double>(t - a.ns) / static_cast<double>(b.ns - a.ns) : 1.0;
            x = a.x + (b.x - a.x) * f;
            y = a.y + (b.y - a.y) * f;
        }

        // ---- the game's camera, frame by frame -------------------------------------------------------------------
        using M4 = std::array<double, 16>;

        M4 mul(const M4 &a, const M4 &b)
        {
            M4 r{};
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                {
                    double v = 0.0;
                    for (int k = 0; k < 4; ++k)
                        v += a[4 * i + k] * b[4 * k + j];
                    r[4 * i + j] = v;
                }
            return r;
        }

        bool invert(const M4 &a, M4 &out)
        {
            // (Gauss-Jordan with partial pivoting, in double)
            double m[4][8];
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                {
                    m[i][j] = a[4 * i + j];
                    m[i][j + 4] = i == j ? 1.0 : 0.0;
                }
            for (int c = 0; c < 4; ++c)
            {
                int p = c;
                for (int r = c + 1; r < 4; ++r)
                    if (std::fabs(m[r][c]) > std::fabs(m[p][c]))
                        p = r;
                if (!(std::fabs(m[p][c]) > 1e-300))
                    return false;
                if (p != c)
                    for (int j = 0; j < 8; ++j)
                        std::swap(m[p][j], m[c][j]);
                const double inv = 1.0 / m[c][c];
                for (int j = 0; j < 8; ++j)
                    m[c][j] *= inv;
                for (int r = 0; r < 4; ++r)
                    if (r != c && m[r][c] != 0.0)
                    {
                        const double f = m[r][c];
                        for (int j = 0; j < 8; ++j)
                            m[r][j] -= f * m[c][j];
                    }
            }
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    out[4 * i + j] = m[i][j + 4];
            for (double v : out)
                if (!std::isfinite(v))
                    return false;
            return true;
        }

        // A rotation by `angle` about the unit axis `a` through `p`, as a row-vector 4x4 (x' = (x - p) R + p).
        M4 rotationAbout(const double *a, const double *p, double angle)
        {
            const double c = std::cos(angle), s = std::sin(angle), t = 1.0 - c;
            const double x = a[0], y = a[1], z = a[2];
            // (column-vector Rodrigues, transposed for row vectors)
            const double R[3][3] = {{t * x * x + c, t * x * y - s * z, t * x * z + s * y},
                                    {t * x * y + s * z, t * y * y + c, t * y * z - s * x},
                                    {t * x * z - s * y, t * y * z + s * x, t * z * z + c}};
            M4 m{};
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    m[4 * i + j] = R[j][i];
            for (int j = 0; j < 3; ++j)
            {
                double v = p[j];
                for (int i = 0; i < 3; ++i)
                    v -= p[i] * m[4 * i + j];
                m[12 + j] = v;
            }
            m[15] = 1.0;
            return m;
        }

        float readFloat(const uint8_t *rdram, uint32_t address)
        {
            float v = 0.0f;
            if (const uint8_t *p = getConstMemPtr(const_cast<uint8_t *>(rdram), address))
                std::memcpy(&v, p, sizeof(v));
            return v;
        }
        uint32_t readWord(const uint8_t *rdram, uint32_t address)
        {
            uint32_t v = 0u;
            if (const uint8_t *p = getConstMemPtr(const_cast<uint8_t *>(rdram), address))
                std::memcpy(&v, p, sizeof(v));
            return v;
        }

        double wrapAngle(double a)
        {
            while (a >= kPi)
                a -= 2.0 * kPi;
            while (a < -kPi)
                a += 2.0 * kPi;
            return a;
        }

        // How the camera's angles move it (signs, found from the game's own camera, TF_TURN_CHECK): its yaw (+0x11C) about
        // the world's up (z), its pitch (+0x118) about its horizontal axis, both about the target. +1 / -1.
        double g_yawSign = 1.0, g_pitchSign = 1.0;
        bool parseSigns()
        {
            if (const char *v = std::getenv("TF_TURN_SIGNS"))
            {
                float a = 1.0f, b = 1.0f;
                if (std::sscanf(v, "%f,%f", &a, &b) == 2)
                {
                    g_yawSign = a < 0.0f ? -1.0 : 1.0;
                    g_pitchSign = b < 0.0f ? -1.0 : 1.0;
                }
            }
            return true;
        }

        struct Game
        {
            // (this frame: the third-person camera's turn, 0x46D410)
            bool turned = false;
            uint32_t control = 0u;
            float yaw = 0.0f, pitch = 0.0f, limit = 0.0f;
            float mouseYaw = 0.0f, mousePitch = 0.0f, frameSeconds = 0.0f;
            double anchorX = 0.0, anchorY = 0.0;
            int64_t anchorNs = 0;
            // (the camera object, the listener's)
            uint32_t camera = 0u, transform = 0u;
            // (the frame's camera matrices: scratchpad 0x6A0, 0x6E0, 0xEC0 at each matrix sent)
            std::vector<std::array<float, 32>> cameras;
            uint8_t index = 0;
            // (the frame before's)
            bool prevValid = false;
            int prevKind = -2;
            double prevYaw = 0.0, prevPitch = 0.0, yawUnwrapped = 0.0;
            double prevPivot[3] = {}, prevEye[3] = {}, prevForward[3] = {};
            uint64_t serial = 0;
            int64_t endNs = 0; // (when the frame before ended: TODO 292's slow frames)
            uint32_t viewNode = 0u; // (the view's camera node: noteViewNode)
            bool reticle = false;   // (TODO 292 B: the matrix sent now is the game's crosshair model's: not turned)
            uint64_t reticleDraws = 0, reticleDrawsAtEnd = 0;
        } g_game;
    }

    struct FrameCamera
    {
        bool valid = false;  // the third-person camera turned this frame (its angles known), or another the turn takes
        bool mouseDirect = true; // (the third-person camera: the mouse turns its angles itself - else the mouse is a stick)
        int kind = 0;        // (0 the third-person camera's turn; 1 + cam_trace::Script for the others)
        bool cut = false;    // the camera jumped from the frame before's (a cut, a respawn): no blending of its turn
        std::vector<std::array<float, 32>> cameras;
        double pivot[3] = {}, eye[3] = {}, forward[3] = {}, left[3] = {}, up[3] = {}; // (the camera's transform: its rows)
        double yaw = 0.0, pitch = 0.0, limit = 0.0, yawUnwrapped = 0.0;
        double dYaw = 0.0, dPitch = 0.0;           // the frame's own turn (from the frame before's camera)
        double stickYawRate = 0.0, stickPitchRate = 0.0; // (rad/s: the turn not the mouse's)
        double anchorX = 0.0, anchorY = 0.0;
        int64_t anchorNs = 0;
        double frameSeconds = 0.0;
        uint64_t tick = 0, serial = 0;
        int main = -1; // (the scene's camera: the most used index - set by the renderer through turnable's caller)
        double realFrameMs = 0.0; // (TODO 292: the time since the frame before ended, on the clock)
        bool reticleKept = false;  // (TODO 292 B: the crosshair model's draws left unturned this frame)
        bool reticleKnown = false; // (and where it is, for TF_XHAIR_PICTURE_LOG)
        double reticle[3] = {};
    };

    bool enabled()
    {
        static const bool on = envOn("TF_TURN", true);
        // (TF_TURN_ALT=<s>, a measuring aid: on and off by turns every that many seconds - an A/B within one run)
        static const int alt = std::getenv("TF_TURN_ALT") ? std::atoi(std::getenv("TF_TURN_ALT")) : 0;
        if (alt <= 0 || !on)
            return on;
        static const auto t0 = std::chrono::steady_clock::now();
        const bool now = (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count() / alt) % 2 == 0;
        static std::atomic<int> last{-1};
        if (last.exchange(now ? 1 : 0) != (now ? 1 : 0))
            std::fprintf(stderr, "[turn] the camera's turn for every picture %s (TF_TURN_ALT)\n", now ? "on" : "off");
        return now;
    }

    bool turnPicturesWanted()
    {
        // (TODO 165: off by default - the game's rate follows the screen, every picture a game frame. On, with the game at half
        // the refresh or less, each frame was shown for every refresh with only the camera's turn newer: what moves with the
        // camera - the player riding the jet bike - stood in two places, a double image at 240. TF_TURN_PICTURES=1: a test switch.)
        static const bool on = envOn("TF_TURN_PICTURES", false);
        return on && enabled();
    }

    bool takeMouseTravelLive(float &pixelsX, float &pixelsY);

    // (N12 b: a play recording keeps what the game took, its replay gives it back - playrec.h)
    bool takeMouseTravel(float &pixelsX, float &pixelsY)
    {
        if (playrec::replaying())
        {
            pixelsX = pixelsY = 0.0f;
            return playrec::mouseTravel(false, pixelsX, pixelsY);
        }
        const bool captured = takeMouseTravelLive(pixelsX, pixelsY);
        return playrec::mouseTravel(captured, pixelsX, pixelsY);
    }

    bool takeMouseTravelLive(float &pixelsX, float &pixelsY)
    {
        Mouse &m = g_mouse;
        static const bool passThrough = std::getenv("PS2X_MOUSE_TEST") != nullptr; // (TF_TURN=0 keeps the reading: the measurements' A/B)
        if (passThrough)
        {
            const bool captured = ps2_host_input::takeMouseTravel(pixelsX, pixelsY);
            std::lock_guard<std::mutex> lock(m.mutex);
            m.passThrough = true;
            m.x += pixelsX;
            m.y += pixelsY;
            m.takenX = m.x;
            m.takenY = m.y;
            m.takenNs = nowNs();
            return captured;
        }
        std::lock_guard<std::mutex> lock(m.mutex);
        if (!m.started)
        {
            m.started = true;
            std::thread(mouseThread).detach();
        }
        const int64_t t = nowNs();
        pullLocked(t);
        // (TODO 292 A) Travel no one took for a while - a load, a frame of many hundred ms, a pause that took none - is not
        // turned in one go when the takes come back: the view swung to its pitch limit at once on entering a level. Of a
        // gap over TF_MOUSE_STALE_MS (250 by default; 0: as before) only the last 50 ms of travel is kept.
        static const int64_t staleNs = [] {
            const char *v = std::getenv("TF_MOUSE_STALE_MS");
            return static_cast<int64_t>((v && *v ? std::atof(v) : 250.0) * 1.0e6);
        }();
        if (staleNs > 0 && m.takenNs != 0 && t - m.takenNs > staleNs && !m.history.empty())
        {
            const int64_t keepFrom = t - 50000000ll;
            double kx = m.x, ky = m.y;
            for (auto it = m.history.rbegin(); it != m.history.rend(); ++it)
                if (it->ns <= keepFrom)
                {
                    kx = it->x;
                    ky = it->y;
                    break;
                }
            const double droppedX = kx - m.takenX, droppedY = ky - m.takenY;
            if (std::fabs(droppedX) + std::fabs(droppedY) > 0.5)
            {
                static int told = 0;
                if (told++ < 50)
                    std::fprintf(stderr, "[turn] mouse travel of %.0f, %.0f pixels left out: no frame took it for %.0f ms\n", droppedX, droppedY,
                                 static_cast<double>(t - m.takenNs) / 1.0e6);
            }
            m.takenX = kx;
            m.takenY = ky;
        }
        pixelsX = static_cast<float>(m.x - m.takenX);
        pixelsY = static_cast<float>(m.y - m.takenY);
        m.takenX = m.x;
        m.takenY = m.y;
        m.takenNs = t;
        return m.captured;
    }

    void setTestMouse(float x, float y)
    {
        std::lock_guard<std::mutex> lock(g_mouse.mutex);
        pullLocked(nowNs()); // (the travel so far at the old speed)
        g_mouse.testX.store(x, std::memory_order_relaxed);
        g_mouse.testY.store(y, std::memory_order_relaxed);
        g_mouse.testOn.store(true, std::memory_order_relaxed);
    }

    bool testMouseOn() { return g_mouse.testOn.load(std::memory_order_relaxed); }

    void noteTurned(const uint8_t *rdram, uint32_t control, float mouseYaw, float mousePitch, float frameSeconds)
    {
        Game &g = g_game;
        g.turned = true;
        g.control = control;
        g.yaw = readFloat(rdram, control + 0x11Cu);
        g.pitch = readFloat(rdram, control + 0x118u);
        {
            uint16_t id = 0u;
            if (const uint8_t *p = getConstMemPtr(const_cast<uint8_t *>(rdram), control + 0x18u))
                std::memcpy(&id, p, sizeof(id));
            g.limit = std::fabs(readFloat(rdram, 0x006CBB4Cu) * readFloat(rdram, 0x00717430u + id * 0x240u + 0x60u + 0x15Cu));
        }
        g.mouseYaw = mouseYaw;
        g.mousePitch = mousePitch;
        g.frameSeconds = frameSeconds;
        std::lock_guard<std::mutex> lock(g_mouse.mutex);
        g.anchorX = g_mouse.takenX;
        g.anchorY = g_mouse.takenY;
        g.anchorNs = g_mouse.takenNs;
    }

    void noteCameraObject(uint32_t camera, uint32_t transform)
    {
        g_game.camera = camera;
        g_game.transform = transform;
    }

    void noteViewNode(uint32_t node) { g_game.viewNode = node; }

    void noteMatrixSent(const uint8_t *rdram, float *sent)
    {
        Game &g = g_game;
        {
            // (TODO 292 B) The game's crosshair model (the original crosshair) is a model in the scene, placed once a frame on
            // the frame's camera's ray. Turned with the scene by a picture's turn it was carried off that ray - by the mouse's
            // travel since the frame was read, more for a model a few units ahead of the eye - so it shook while the view
            // turned (the user: "the original crosshair stutters a lot when moving the camera around"). The model's matrices
            // are found by their origin (the scratchpad's model-to-camera at 0xD40, put back into the world by the camera's
            // transform) lying where the crosshair was placed (photo_mode::crosshair), and their draws are left unturned: the
            // crosshair keeps its place on the screen in every picture. TF_CROSSHAIR_TURN=1: turned with the scene, as before.
            static const bool turnIt = std::getenv("TF_CROSSHAIR_TURN") && *std::getenv("TF_CROSSHAIR_TURN") == '1';
            g.reticle = false;
            float at[3];
            uint32_t transform = g.transform;
            if (!transform && g.viewNode)
                transform = readWord(rdram, g.viewNode + 0x14u);
            // (TODO 343 / 355: the Fixed crosshair's - with Original the reticle turns with the scene, as before 292 B)
            if (!turnIt && static_aim::fixed() && transform >= 0x00100000u && transform < PS2_RAM_SIZE - 0x40u &&
                photo_mode::crosshair(at))
            {
                float rows[16], m[16];
                const uint8_t *t = getConstMemPtr(const_cast<uint8_t *>(rdram), transform);
                const uint8_t *mc = getConstMemPtr(const_cast<uint8_t *>(rdram), 0x70000D40u);
                if (t && mc)
                {
                    std::memcpy(rows, t, sizeof(rows));
                    std::memcpy(m, mc, sizeof(m));
                    // (the camera's space: x right = -left, y up, z ahead, from the eye)
                    double d2 = 0.0;
                    for (int i = 0; i < 3; ++i)
                    {
                        const double w = rows[12 + i] - rows[4 + i] * m[12] + rows[8 + i] * m[13] + rows[i] * m[14];
                        d2 += (w - at[i]) * (w - at[i]);
                    }
                    g.reticle = d2 < 0.05 * 0.05;
                    if (g.reticle)
                    {
                        ++g.reticleDraws;
                        // (TODO 343) The crosshair dead centre, exactly: with the crosshair pinned (static_aim, mouse and
                        // keyboard) the model's origin is put on the camera's axis in the matrix it is drawn with - the
                        // model-to-screen matrix's last row made the camera-to-screen matrix (scratchpad 0x6A0) times the
                        // point at the model's own depth on the axis (across 0; up 0 with the share 0, the middle) - so it
                        // shows at the picture's middle whatever the frame's placement was measured against (a frame's
                        // camera read before or after the crosshair was placed: up to 2 px at 240 fps). Only where the sent
                        // row checks out as the model's origin through 0x6A0. TF_XHAIR_CENTRE=0: as placed;
                        // TF_XHAIR_CENTRE_LOG=1: the row before and after, every 30th.
                        static const bool centre = !(std::getenv("TF_XHAIR_CENTRE") && *std::getenv("TF_XHAIR_CENTRE") == '0');
                        static const bool tell = std::getenv("TF_XHAIR_CENTRE_LOG") != nullptr;
                        float share = 0.0f, c[16];
                        const uint8_t *cs = getConstMemPtr(const_cast<uint8_t *>(rdram), 0x700006A0u);
                        if (centre && sent && cs && static_aim::pinned(share))
                        {
                            std::memcpy(c, cs, sizeof(c));
                            const float t[4] = {0.0f, share == 0.0f ? 0.0f : m[13], m[14], 1.0f};
                            const float was[4] = {sent[12], sent[13], sent[14], sent[15]};
                            float row[4], check[4];
                            for (int j = 0; j < 4; ++j)
                            {
                                row[j] = t[0] * c[j] + t[1] * c[4 + j] + t[2] * c[8 + j] + t[3] * c[12 + j];
                                check[j] = m[12] * c[j] + m[13] * c[4 + j] + m[14] * c[8 + j] + c[12 + j];
                            }
                            bool same = true;
                            for (int j = 0; j < 4; ++j)
                                same = same && std::fabs(check[j] - was[j]) <= 1e-3f * (1.0f + std::fabs(was[j]));
                            if (same)
                                for (int j = 0; j < 4; ++j)
                                    sent[12 + j] = row[j];
                            if (static int told = 0; tell && told++ % 30 == 0)
                                std::fprintf(stderr, "[crosshair] its matrix row %.3f %.3f %.3f %.3f -> %.3f %.3f %.3f %.3f (%s; camera space %.3f %.3f %.3f)\n",
                                             was[0], was[1], was[2], was[3], sent[12], sent[13], sent[14], sent[15],
                                             same ? "put on the axis" : "NOT the origin through 0x6A0: left", m[12], m[13], m[14]);
                        }
                    }
                }
            }
        }
        {
            // (TF_TURN_DUMP=<count>, a checking aid: the scratchpad's camera matrices at the first matrices sent of the frames
            // after the third-person camera's turn is first seen - 0x6A0, 0x6E0, 0xD40, 0xEC0, 0xF00 - and the camera's own)
            static int dump = std::getenv("TF_TURN_DUMP") ? std::atoi(std::getenv("TF_TURN_DUMP")) : 0;
            static int inFrame = 0;
            static uint64_t frameSerial = ~0ull;
            if (dump > 0 && g.prevValid)
            {
                if (frameSerial != g.serial)
                {
                    frameSerial = g.serial;
                    inFrame = 0;
                    --dump;
                }
                if (inFrame++ < 12)
                {
                    std::string line = "[turn] dump frame " + std::to_string(g.serial) + " send " + std::to_string(inFrame);
                    for (uint32_t base : {0x700006A0u, 0x700006E0u, 0x70000D40u, 0x70000EC0u, 0x70000F00u})
                    {
                        char t[32];
                        std::snprintf(t, sizeof(t), " | %x:", base & 0xFFFu);
                        line += t;
                        for (uint32_t i = 0; i < 16u; ++i)
                        {
                            std::snprintf(t, sizeof(t), " %.6g", readFloat(rdram, base + 4u * i));
                            line += t;
                        }
                    }
                    if (sent)
                    {
                        line += " | sent:";
                        for (int i = 0; i < 16; ++i)
                        {
                            char t[32];
                            std::snprintf(t, sizeof(t), " %.6g", sent[i]);
                            line += t;
                        }
                    }
                    if (inFrame == 1 && g.transform)
                    {
                        line += " | C:";
                        for (uint32_t i = 0; i < 16u; ++i)
                        {
                            char t[32];
                            std::snprintf(t, sizeof(t), " %.6g", readFloat(rdram, g.transform + 4u * i));
                            line += t;
                        }
                    }
                    std::fprintf(stderr, "%s\n", line.c_str());
                }
            }
        }
        // (the camera to screen and to the clipping space, scratchpad 0x6A0 and 0x6E0 - 128 bytes in a row; compared with the
        // last ones first: most matrices of a frame are made with the same camera - 2026-11-03, this was 1.7% of the game
        // thread's time read float by float)
        std::array<float, 32> c{};
        const uint8_t *spr = getConstMemPtr(const_cast<uint8_t *>(rdram), 0x700006A0u);
        if (!spr)
            return;
        std::memcpy(c.data(), spr, sizeof(c));
        if (g.index < g.cameras.size() && std::memcmp(g.cameras[g.index].data(), c.data(), sizeof(c)) == 0)
            return;
        for (size_t k = 0; k < g.cameras.size(); ++k)
            if (g.cameras[k] == c)
            {
                g.index = static_cast<uint8_t>(k);
                return;
            }
        if (g.cameras.size() >= 250u)
        {
            g.index = 255u; // (too many: none of them turned)
            return;
        }
        g.cameras.push_back(c);
        g.index = static_cast<uint8_t>(g.cameras.size() - 1u);
    }

    uint8_t cameraIndex() { return g_game.reticle ? 254u : g_game.index; } // (254: the crosshair model's, never turned)


    std::shared_ptr<const FrameCamera> endFrame(const uint8_t *rdram, uint64_t tick)
    {
        static const bool signs = parseSigns();
        (void)signs;
        Game &g = g_game;
        auto fc = std::make_shared<FrameCamera>();
        {
            // (the average of the last four frames: one frame's end comes early or late by the waits around it - a 29 ms
            // gap then a 3 ms one at a steady 60 - so a single gap is no measure)
            const int64_t t = nowNs();
            static int64_t ends[5] = {};
            static int count = 0;
            for (int i = 4; i > 0; --i)
                ends[i] = ends[i - 1];
            ends[0] = t;
            count = std::min(count + 1, 5);
            const int64_t span = t - ends[count - 1];
            fc->realFrameMs = count > 1 && span < 2000000000ll ? static_cast<double>(span) / 1.0e6 / (count - 1) : 0.0;
            g.endNs = t;
        }
        fc->tick = tick;
        fc->serial = ++g.serial;
        fc->reticleKept = g.reticleDraws != g.reticleDrawsAtEnd;
        g.reticleDrawsAtEnd = g.reticleDraws;
        g.reticle = false;
        {
            float at[3];
            if (photo_mode::crosshair(at))
            {
                fc->reticleKnown = true;
                for (int i = 0; i < 3; ++i)
                    fc->reticle[i] = at[i];
            }
        }
        fc->cameras = std::move(g.cameras);
        g.cameras.clear();
        g.index = 0;
        const bool turned = g.turned;
        g.turned = false;
        // (the other cameras the turn takes - their angles from the camera's transform, the mouse a stick to them: the car,
        // gliding, the jet pack, first person. Not the fixed cameras, the ragdolls' or the transformation's - cutscenes and the
        // game's own camera paths stay as they are. TF_TURN_OTHERS=0: the third-person camera's turn alone, as before.)
        const uint32_t scripts = cam_trace::takeScripts();
        static const bool othersOn = envOn("TF_TURN_OTHERS", true);
        int kind = turned ? 0 : -1;
        if (!turned && othersOn)
        {
            using cs = cam_trace::Script;
            const uint32_t fixedOnes = (1u << cs::Fixed) | (1u << cs::FixedLookat) | (1u << cs::Ragdoll) | (1u << cs::ReverseRagdoll) |
                                       (1u << cs::CybertronRagdoll) | (1u << cs::TransformToVehicle);
            // (the walking camera, taThirdPersonCameraScriptThree, takes the turn by its own 0x46D410 - kind 0 - in first
            // person too; without it, it is the level's opening or another of the game's own camera paths: left as it is.
            // Measured 2026-11-03, TF_TURN_KINDS: car = taVehicleCameraScript, first person = taFirstPersonCameraScript.)
            for (const cs k : {cs::Vehicle, cs::Glide, cs::JetPack, cs::FirstPerson})
                if (scripts & (1u << k))
                {
                    kind = 1 + static_cast<int>(k);
                    break;
                }
            if (scripts & fixedOnes)
                kind = -1;
        }
        {
            // (TF_TURN_KINDS=1: each change of the camera the turn sees, with the scripts that ran)
            static const bool tell = std::getenv("TF_TURN_KINDS") != nullptr;
            static uint32_t lastScripts = ~0u;
            if (tell && scripts != lastScripts)
            {
                lastScripts = scripts;
                std::string names;
                for (int k = 0; k < 17; ++k)
                    if (scripts & (1u << k))
                        names += std::string(" ") + cam_trace::scriptName(k);
                std::fprintf(stderr, "[turn] tick %llu: camera scripts%s; the turn's %s\n", static_cast<unsigned long long>(tick),
                             names.empty() ? " (none)" : names.c_str(), kind < 0 ? "none" : kind == 0 ? "third person (mouse direct)" : cam_trace::scriptName(kind - 1));
            }
        }
        // The camera's transform (forward, left, up, position) and its target (the player).
        float rows[16] = {};
        bool have = false;
        if (!g.transform && g.viewNode)
        {
            // (TODO 292 A: no sound listener's camera seen - Antarctica - so the view's own node's transform)
            const uint32_t t = readWord(rdram, g.viewNode + 0x14u);
            if (t >= 0x00100000u && t < PS2_RAM_SIZE - 0x50u)
                g.transform = t;
        }
        if (kind >= 0 && g.transform)
            if (const uint8_t *p = getConstMemPtr(const_cast<uint8_t *>(rdram), g.transform))
            {
                std::memcpy(rows, p, sizeof(rows));
                have = true;
            }
        uint32_t player = g.camera ? readWord(rdram, g.camera + 0x74u) : 0u;
        if (!(player >= 0x00100000u && player < PS2_RAM_SIZE))
            player = 0u;
        // (TODO 292 A: the walking camera's centre is the game's own pivot below, the player object not needed for it - in
        // Antarctica there was none, and no picture turned)
        float gamePivot[3] = {};
        const bool haveGamePivot = kind == 0 && static_aim::cameraPivot(gamePivot);
        if (!have || (!player && !haveGamePivot) || photo_mode::active())
        {
            if (static const bool debug = std::getenv("TF_TURN_DEBUG") && *std::getenv("TF_TURN_DEBUG") == '1'; debug)
                std::fprintf(stderr, "[turn debug] tick %llu: no turn - kind %d, transform %08x (%s), player %08x, the game's pivot %s\n",
                             static_cast<unsigned long long>(tick), kind, g.transform, have ? "read" : "not read", player, haveGamePivot ? "known" : "not known");
            g.prevValid = false;
            return fc;
        }
        for (int i = 0; i < 3; ++i)
        {
            fc->pivot[i] = player ? readFloat(rdram, player + 0x620u + 4u * static_cast<uint32_t>(i)) : 0.0f;
            fc->eye[i] = rows[12 + i];
            fc->forward[i] = rows[i];
            fc->left[i] = rows[4 + i];
            fc->up[i] = rows[8 + i];
        }
        // (TODO 292 A) The centre the view turns about. The player's position (player +0x620) was taken for it: in Alaska
        // and Antarctica that word is 0 - the pictures' turn swung the view about the world's origin, 3 km away, so a
        // degree of mouse moved the camera by 50 units: into the canyon's walls, above the cliffs, down on the robot (the
        // user's clip, SirCebollo's "from the moment he enters Alaska"; the pad's turn adds nothing to a frame's own
        // picture, so the pad was fine). The walking camera turns about the game's own pivot (GetThirdPersonCameraPivotPoint,
        // recorded by static_aim each frame); first person about the eye. Whatever the source, a centre further than
        // TF_TURN_PIVOT_MAX (60) units from the eye turns no picture.
        if (kind == 0)
        {
            if (camera_mode::firstPerson() && !camera_mode::inVehicle())
                for (int i = 0; i < 3; ++i)
                    fc->pivot[i] = fc->eye[i];
            else if (haveGamePivot)
                for (int i = 0; i < 3; ++i)
                    fc->pivot[i] = gamePivot[i];
        }
        {
            static const double maxPivot = std::getenv("TF_TURN_PIVOT_MAX") ? std::atof(std::getenv("TF_TURN_PIVOT_MAX")) : 60.0;
            const double d = std::sqrt((fc->eye[0] - fc->pivot[0]) * (fc->eye[0] - fc->pivot[0]) + (fc->eye[1] - fc->pivot[1]) * (fc->eye[1] - fc->pivot[1]) +
                                       (fc->eye[2] - fc->pivot[2]) * (fc->eye[2] - fc->pivot[2]));
            if (maxPivot > 0.0 && !(d <= maxPivot))
            {
                static int told = 0;
                if (told++ < 10)
                    std::fprintf(stderr, "[turn] the view's centre is %.0f units from the eye (kind %d): no picture turned beyond its frame\n", d, kind);
                g.prevValid = false;
                return fc;
            }
        }
        fc->kind = kind;
        fc->mouseDirect = kind == 0;
        if (kind == 0)
        {
            fc->yaw = g.yaw;
            fc->pitch = g.pitch;
            fc->limit = g.limit;
        }
        else
        {
            // (the angles of the view itself: its yaw about the world's up as the third-person camera's grows - to the left
            // - and its pitch growing as it looks down; no limit of its own here)
            fc->yaw = std::atan2(fc->forward[1], fc->forward[0]);
            fc->pitch = -std::asin(std::clamp(fc->forward[2] / std::max(1e-9, std::sqrt(fc->forward[0] * fc->forward[0] + fc->forward[1] * fc->forward[1] +
                                                                                         fc->forward[2] * fc->forward[2])), -1.0, 1.0));
            fc->limit = 0.0;
            g.mouseYaw = g.mousePitch = 0.0f;
            if (kind == 1 + cam_trace::FirstPerson)
                for (int i = 0; i < 3; ++i)
                    fc->pivot[i] = fc->eye[i]; // (first person: the view turns about the eye)
            g.frameSeconds = tf::port::levelRate() > 0 ? 1.0f / static_cast<float>(tf::port::levelRate()) : 1.0f / 30.0f;
        }
        fc->anchorX = g.anchorX;
        fc->anchorY = g.anchorY;
        fc->anchorNs = g.anchorNs;
        fc->frameSeconds = g.frameSeconds > 0.0f ? g.frameSeconds : 1.0 / 60.0;
        bool finite = std::isfinite(fc->yaw) && std::isfinite(fc->pitch);
        for (int i = 0; i < 3; ++i)
            finite = finite && std::isfinite(fc->pivot[i]) && std::isfinite(fc->eye[i]);
        if (!finite)
        {
            g.prevValid = false;
            return fc;
        }
        fc->valid = true;
        if (g.prevValid && g.prevKind != kind)
            g.prevValid = false; // (another camera: its angles not comparable - nothing blended across)
        g.prevKind = kind;
        if (g.prevValid)
        {
            fc->dYaw = wrapAngle(fc->yaw - g.prevYaw);
            fc->dPitch = fc->pitch - g.prevPitch;
            double jump = 0.0, eyeJump = 0.0;
            for (int i = 0; i < 3; ++i)
            {
                jump += (fc->pivot[i] - g.prevPivot[i]) * (fc->pivot[i] - g.prevPivot[i]);
                eyeJump += (fc->eye[i] - g.prevEye[i]) * (fc->eye[i] - g.prevEye[i]);
            }
            // (a cut: the angles or the target jumped - more than a frame's turn or walk can make)
            fc->cut = std::fabs(fc->dYaw) > 0.5 || std::fabs(fc->dPitch) > 0.5 || jump > 25.0 * 25.0 || eyeJump > 40.0 * 40.0;
            if (!fc->cut)
            {
                fc->stickYawRate = (fc->dYaw - g.mouseYaw) / fc->frameSeconds;
                fc->stickPitchRate = (fc->dPitch - g.mousePitch) / fc->frameSeconds;
                // (a turn rate the stick can make: 135 degrees a second of yaw, 45 of pitch at most - else not the stick's)
                if (std::fabs(fc->stickYawRate) > 2.6 || std::fabs(fc->stickPitchRate) > 0.9 ||
                    (std::fabs(fc->stickYawRate) < 1e-3 && std::fabs(fc->stickPitchRate) < 1e-3))
                    fc->stickYawRate = fc->stickPitchRate = 0.0;
            }
            else
                fc->dYaw = fc->dPitch = 0.0;
        }
        g.yawUnwrapped += fc->dYaw;
        fc->yawUnwrapped = g.yawUnwrapped;
        // (TF_TURN_CHECK=1: every frame that turned, how the game's camera moved against a turn about its target by the
        // frame's own angles - the model the pictures' turn makes - for each of the four signs; the error in units)
        static const bool check = std::getenv("TF_TURN_CHECK") && *std::getenv("TF_TURN_CHECK") == '1';
        if (check && g.prevValid && !fc->cut && (std::fabs(fc->dYaw) > 1e-4 || std::fabs(fc->dPitch) > 1e-4))
        {
            double best = 1e30, err[4] = {};
            for (int sy = 0; sy < 2; ++sy)
                for (int sp = 0; sp < 2; ++sp)
                {
                    const double up[3] = {0.0, 0.0, 1.0};
                    const M4 ry = rotationAbout(up, g.prevPivot, (sy ? -1.0 : 1.0) * fc->dYaw);
                    double eye[4] = {g.prevEye[0], g.prevEye[1], g.prevEye[2], 1.0}, fwd[4] = {g.prevForward[0], g.prevForward[1], g.prevForward[2], 0.0};
                    auto apply = [](const M4 &m, double *v) {
                        double r[4];
                        for (int j = 0; j < 4; ++j)
                            r[j] = v[0] * m[j] + v[1] * m[4 + j] + v[2] * m[8 + j] + v[3] * m[12 + j];
                        std::memcpy(v, r, sizeof(r));
                    };
                    apply(ry, eye);
                    apply(ry, fwd);
                    double axis[3] = {-fwd[1], fwd[0], 0.0}; // (horizontal, at right angles to the view: z x forward)
                    const double n = std::hypot(axis[0], axis[1]);
                    if (n > 1e-9)
                    {
                        axis[0] /= n;
                        axis[1] /= n;
                        const M4 rp = rotationAbout(axis, g.prevPivot, (sp ? -1.0 : 1.0) * fc->dPitch);
                        apply(rp, eye);
                        apply(rp, fwd);
                    }
                    double e = 0.0;
                    for (int i = 0; i < 3; ++i)
                        e += (eye[i] - fc->eye[i]) * (eye[i] - fc->eye[i]) + 100.0 * (fwd[i] - fc->forward[i]) * (fwd[i] - fc->forward[i]);
                    err[2 * sy + sp] = std::sqrt(e);
                    best = std::min(best, err[2 * sy + sp]);
                }
            double moved = 0.0, eyeMoved = 0.0;
            for (int i = 0; i < 3; ++i)
            {
                moved += (fc->pivot[i] - g.prevPivot[i]) * (fc->pivot[i] - g.prevPivot[i]);
                eyeMoved += (fc->eye[i] - g.prevEye[i]) * (fc->eye[i] - g.prevEye[i]);
            }
            static int told = 0;
            if (told++ < 4000)
                std::fprintf(stderr, "[turn] check tick %llu: yaw %+.5f pitch %+.5f (mouse %+.5f %+.5f, stick %+.3f %+.3f rad/s); target moved %.3f, eye %.3f; "
                             "err ++ %.4f +- %.4f -+ %.4f -- %.4f; eye %.2f,%.2f,%.2f target %.2f,%.2f,%.2f dist %.2f\n",
                             static_cast<unsigned long long>(tick), fc->dYaw, fc->dPitch, g.mouseYaw, g.mousePitch, fc->stickYawRate,
                             fc->stickPitchRate, std::sqrt(moved), std::sqrt(eyeMoved), err[0], err[1], err[2], err[3], fc->eye[0], fc->eye[1],
                             fc->eye[2], fc->pivot[0], fc->pivot[1], fc->pivot[2],
                             std::sqrt((fc->eye[0] - fc->pivot[0]) * (fc->eye[0] - fc->pivot[0]) + (fc->eye[1] - fc->pivot[1]) * (fc->eye[1] - fc->pivot[1]) +
                                       (fc->eye[2] - fc->pivot[2]) * (fc->eye[2] - fc->pivot[2])));
        }
        g.prevValid = true;
        g.prevYaw = fc->yaw;
        g.prevPitch = fc->pitch;
        for (int i = 0; i < 3; ++i)
        {
            g.prevPivot[i] = fc->pivot[i];
            g.prevEye[i] = fc->eye[i];
            g.prevForward[i] = fc->forward[i];
        }
        return fc;
    }

    // ---- the renderer --------------------------------------------------------------------------------------------
    namespace
    {
        // The lead: how soon after its moment a picture's turn can be read - the earliest the pictures could be
        // submitted (the 1st percentile of the last 2 s), followed slowly so the turn's steps stay even.
        // (two of them: pictures between frames (TF_POSES), all behind their moments; and the rest - a frame's own picture,
        // and the turn's pictures, which wait for their moment and so are not counted)
        std::mutex g_leadMutex;
        std::deque<std::pair<int64_t, double>> g_offsetSets[2]; // (when, ms after the moment)
        double g_leadSets[2] = {-1.0, -1.0};

        double leadFor(int set, int64_t t, double offsetMs, bool record)
        {
            std::lock_guard<std::mutex> lock(g_leadMutex);
            std::deque<std::pair<int64_t, double>> &g_offsets = g_offsetSets[set & 1];
            double &g_leadMs = g_leadSets[set & 1];
            if (record && offsetMs > -50.0 && offsetMs < 100.0)
            {
                g_offsets.emplace_back(t, offsetMs);
                while (!g_offsets.empty() && t - g_offsets.front().first > 2000000000ll)
                    g_offsets.pop_front();
                std::vector<double> v;
                v.reserve(g_offsets.size());
                for (const auto &o : g_offsets)
                    v.push_back(o.second);
                const size_t at = std::min(v.size() - 1u, static_cast<size_t>(0.01 * static_cast<double>(v.size())));
                std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(at), v.end());
                const double wanted = v[at];
                if (g_leadMs < -0.5)
                    g_leadMs = wanted;
                else
                    g_leadMs += std::clamp(wanted - g_leadMs, -0.02, 0.02); // (0.02 ms a picture)
            }
            return g_leadMs < -0.5 ? 0.0 : g_leadMs;
        }
    }

    double leadMs(int set)
    {
        std::lock_guard<std::mutex> lock(g_leadMutex);
        return g_leadSets[set & 1];
    }

    namespace
    {

        // The turn the game will surely make by `sigma` (radians) on top of its frame's angles.
        void turnAt(const FrameCamera &fc, int64_t sigma, double stickSeconds, double &yaw, double &pitch)
        {
            yaw = pitch = 0.0;
            double x = 0.0, y = 0.0;
            travelAt(sigma, x, y);
            // (the mouse: right turns right - the yaw down; down looks down - the pitch up: cameraTurn's. Where the mouse is a
            // stick to the camera, the game turns it as the stick, below.)
            if (fc.mouseDirect)
            {
                yaw = -(x - fc.anchorX) * kRadiansPerPixel;
                pitch = (y - fc.anchorY) * kRadiansPerPixel;
            }
            // (the stick: the game turns it a step a frame, its own time - so only by the picture's place among the frame's
            // moments (0 for the frame's own picture: never ahead of what the game shows, never jittered by the threads'
            // times), at the frame's rate, at most a frame and a half on)
            const double tau = std::clamp(stickSeconds, -1.5 * fc.frameSeconds, 1.5 * fc.frameSeconds);
            yaw += fc.stickYawRate * tau;
            pitch += fc.stickPitchRate * tau;
        }

        struct Shown
        {
            uint64_t id;
            bool valid;
            double yaw;
            int64_t sampleNs;
        };
        std::mutex g_shownMutex;
        std::deque<Shown> g_shownYaw;

        // (the pacing log: the yaw shown picture to picture)
        struct Steps
        {
            std::mutex mutex;
            int64_t lastNs = 0, lastDue = 0;
            double lastYaw = 0.0;
            std::vector<double> rateDue; // (deg/s: each step over the time between the pictures' moments)
            std::vector<double> toScreen; // (ms: from the input a picture shows - the turn's reading, else the game's - to the screen)
            bool lastValid = false;
            std::vector<double> rate;  // (deg/s, each picture's step over its time)
            std::vector<double> step;  // (deg)
            std::vector<double> dt;    // (ms)
        } g_steps;
    }

    bool cameraCut(const FrameCamera &fc) { return fc.valid && fc.cut; }

    bool turnable(const FrameCamera &fc)
    {
        return fc.valid; // (the shown yaw is recorded with the turn off too - TF_TURN=0's measurements)
    }

    void pictureTurn(const FrameCamera &fc, int camera, int64_t frameDueNs, int64_t spanNs, int64_t momentNs, float s, double waitMs, int leadSet,
                     PictureTurn &out)
    {
        for (int i = 0; i < 16; ++i)
            out.screen[i] = out.clip[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        out.any = false;
        out.valid = false;
        if (!fc.valid)
            return;
        const float sc = std::clamp(s, 0.0f, 1.0f);
        // (the pose's own share of the frame's turn: the frame before's angles blended to this frame's)
        const double poseYaw = fc.cut ? fc.yaw : fc.yaw - (1.0 - sc) * fc.dYaw;
        const double posePitch = fc.cut ? fc.pitch : fc.pitch - (1.0 - sc) * fc.dPitch;
        out.valid = true;
        out.yawShown = fc.yawUnwrapped - (fc.yaw - poseYaw);
        out.sampleNs = fc.anchorNs;
        if (!enabled() || camera < 0 || camera >= static_cast<int>(fc.cameras.size()))
            return;
        // Its moment plus the lead: waited for if it is ahead (by up to waitMs), else read now.
        int64_t t = nowNs();
        const double lead = leadFor(leadSet, t, static_cast<double>(t - momentNs) / 1.0e6, waitMs <= 0.0);
        int64_t sigma = momentNs + static_cast<int64_t>(lead * 1.0e6);
        if (sigma > t && waitMs > 0.0)
        {
            const int64_t until = std::min<int64_t>(sigma, t + static_cast<int64_t>(waitMs * 1.0e6));
            while ((t = nowNs()) < until)
            {
                const int64_t left = until - t;
                if (left > 1500000ll)
                    std::this_thread::sleep_for(std::chrono::nanoseconds(left - 1000000ll));
                else
                    std::this_thread::yield();
            }
        }
        sigma = std::min(sigma, nowNs());
        out.sampleNs = sigma;
        double dYaw = 0.0, dPitch = 0.0;
        // (the stick: the share of the frame's span the picture lies after the frame's moment, as a share of the game's own
        // frame step - the game's time, which runs slower than the clock where the host falls behind)
        const double stickSeconds = frameDueNs != 0 && spanNs > 0 ? static_cast<double>(momentNs - frameDueNs) / static_cast<double>(spanNs) * fc.frameSeconds : 0.0;
        turnAt(fc, sigma, stickSeconds, dYaw, dPitch);
        {
            // (TF_TURN_FIXED=<yaw>,<pitch> radians, a checking aid: that much more turn in every picture - its shots show the
            // view turned about the player as the mouse would, the player in his place)
            static const std::pair<double, double> fixed = [] {
                std::pair<double, double> f{0.0, 0.0};
                if (const char *v = std::getenv("TF_TURN_FIXED"))
                    if (std::sscanf(v, "%lf,%lf", &f.first, &f.second) < 1)
                        f = {0.0, 0.0};
                return f;
            }();
            dYaw += fixed.first;
            dPitch += fixed.second;
        }
        // (TODO 292 A/D) The turn a picture shows beyond its frame is bounded. Where the game's frames are slow (over
        // TF_TURN_SLOW_MS = 25 ms on the clock, a hitch or a slow PC: 55-69 ms on a laptop) no picture turns beyond its frame
        // - the turn of a whole long frame swung the view about the player through the terrain, and the next frame took it
        // back (the game's camera keeps out of the ground, the pictures' turn does not). Otherwise at most TF_TURN_MAX_DEG
        // (8) of yaw and a third of that of pitch: a picture between two frames shows a part of a frame's turn, never a
        // swing. 0 for either: no bound of that kind.
        {
            static const double slowMs = std::getenv("TF_TURN_SLOW_MS") ? std::atof(std::getenv("TF_TURN_SLOW_MS")) : 25.0;
            static const double maxRad = (std::getenv("TF_TURN_MAX_DEG") ? std::atof(std::getenv("TF_TURN_MAX_DEG")) : 8.0) * kPi / 180.0;
            if (slowMs > 0.0 && fc.realFrameMs > slowMs)
            {
                static int told = 0;
                if (told++ < 20)
                    std::fprintf(stderr, "[turn] a slow frame (%.1f ms): its pictures show the frame's own view\n", fc.realFrameMs);
                dYaw = dPitch = 0.0;
            }
            if (maxRad > 0.0)
            {
                dYaw = std::clamp(dYaw, -maxRad, maxRad);
                dPitch = std::clamp(dPitch, -maxRad / 3.0, maxRad / 3.0);
            }
        }
        const double targetYaw = fc.yaw + dYaw;
        const double targetPitch = fc.limit > 0.0 ? std::clamp(fc.pitch + dPitch, -fc.limit, fc.limit) : fc.pitch + dPitch;
        const double turnYaw = wrapAngle(targetYaw - poseYaw), turnPitch = targetPitch - posePitch;
        // (TF_TURN_DEBUG=1, a measuring aid: each picture's turn, its frame's tick, pose and the frame's own turn - degrees)
        if (static const bool debug = std::getenv("TF_TURN_DEBUG") && *std::getenv("TF_TURN_DEBUG") == '1'; debug)
            std::fprintf(stderr, "[turn debug] tick %llu s %.2f: turn yaw %+.2f pitch %+.2f (mouse/stick %+.2f %+.2f, frame %+.2f %+.2f), pitch %.2f, %.1f ms frame; pivot %.1f %.1f %.1f, the eye %.1f from it\n",
                         static_cast<unsigned long long>(fc.tick), sc, turnYaw * 57.2958, turnPitch * 57.2958, dYaw * 57.2958, dPitch * 57.2958,
                         fc.dYaw * 57.2958, fc.dPitch * 57.2958, fc.pitch * 57.2958, fc.realFrameMs, fc.pivot[0], fc.pivot[1], fc.pivot[2],
                         std::sqrt((fc.eye[0] - fc.pivot[0]) * (fc.eye[0] - fc.pivot[0]) + (fc.eye[1] - fc.pivot[1]) * (fc.eye[1] - fc.pivot[1]) +
                                   (fc.eye[2] - fc.pivot[2]) * (fc.eye[2] - fc.pivot[2])));
        out.yawShown = fc.yawUnwrapped - (fc.yaw - poseYaw) + turnYaw;
        if (std::fabs(turnYaw) < 1e-7 && std::fabs(turnPitch) < 1e-7)
            return;
        // The camera's motion: about the target, its yaw about the world's up, then its pitch about its horizontal axis.
        const double up[3] = {0.0, 0.0, 1.0};
        M4 move = rotationAbout(up, fc.pivot, g_yawSign * turnYaw);
        {
            const double c = std::cos(g_yawSign * turnYaw), sn = std::sin(g_yawSign * turnYaw);
            const double fx = fc.forward[0] * c - fc.forward[1] * sn, fy = fc.forward[0] * sn + fc.forward[1] * c;
            double axis[3] = {-fy, fx, 0.0};
            const double n = std::hypot(axis[0], axis[1]);
            if (n > 1e-9 && std::fabs(turnPitch) > 0.0)
            {
                axis[0] /= n;
                axis[1] /= n;
                move = mul(move, rotationAbout(axis, fc.pivot, g_pitchSign * turnPitch));
            }
        }
        M4 moveInv{};
        if (!invert(move, moveInv))
            return;
        // World to screen (and to the clipping space): the frame's world to camera times its camera to screen. The camera's
        // space (the model-to-camera matrices', scratchpad 0xD40, the sent matrix = 0xD40 x 0x6A0) has x to the right, y up
        // and z ahead: its axes the camera's transform's -left, up and forward rows, from its position (checked against the
        // sky's matrices, whose model is the camera's position alone - TF_TURN_DUMP).
        const std::array<float, 32> &cam = fc.cameras[static_cast<size_t>(camera)];
        M4 p{}, pc{}, w2c{};
        for (int i = 0; i < 16; ++i)
        {
            p[i] = cam[i];
            pc[i] = cam[16 + i];
        }
        for (int i = 0; i < 3; ++i)
        {
            w2c[4 * i + 0] = -fc.left[i];
            w2c[4 * i + 1] = fc.up[i];
            w2c[4 * i + 2] = fc.forward[i];
        }
        for (int j = 0; j < 3; ++j)
            w2c[12 + j] = -(fc.eye[0] * w2c[j] + fc.eye[1] * w2c[4 + j] + fc.eye[2] * w2c[8 + j]);
        w2c[15] = 1.0;
        const M4 mw = mul(w2c, p), mwc = mul(w2c, pc);
        M4 mwInv{}, mwcInv{};
        if (!invert(mw, mwInv))
            return;
        // (screen x world^-1 ... the points seen from the moved camera: s' = s MW^-1 move^-1 MW)
        const M4 turn = mul(mul(mwInv, moveInv), mw);
        M4 turnClip = turn;
        const bool clipOk = invert(mwc, mwcInv);
        if (clipOk)
            turnClip = mul(mul(mwcInv, moveInv), mwc);
        for (int i = 0; i < 16; ++i)
        {
            out.screen[i] = static_cast<float>(turn[i]);
            out.clip[i] = static_cast<float>(turnClip[i]);
        }
        out.any = true;
        // (TF_XHAIR_PICTURE_LOG=1, a measuring aid for TODO 292 B: per picture, how far the picture's turn carries the game's
        // crosshair model off its place - GS pixels of the 448-line picture - and whether its draws were kept unturned)
        if (static const bool xlog = std::getenv("TF_XHAIR_PICTURE_LOG") && *std::getenv("TF_XHAIR_PICTURE_LOG") == '1'; xlog && fc.reticleKnown)
        {
            double v[4] = {fc.reticle[0], fc.reticle[1], fc.reticle[2], 1.0}, scr[4] = {}, st[4] = {};
            for (int j = 0; j < 4; ++j)
                for (int k = 0; k < 4; ++k)
                    scr[j] += v[k] * mw[4 * k + j];
            for (int j = 0; j < 4; ++j)
                for (int k = 0; k < 4; ++k)
                    st[j] += scr[k] * turn[4 * k + j];
            if (std::fabs(scr[3]) > 1e-9 && std::fabs(st[3]) > 1e-9)
            {
                const double dx = st[0] / st[3] - scr[0] / scr[3], dy = st[1] / st[3] - scr[1] / scr[3];
                std::fprintf(stderr, "[xhair picture] tick %llu s %.2f: the turn would carry it %.2f, %.2f pixels; %s\n",
                             static_cast<unsigned long long>(fc.tick), sc, dx, dy, fc.reticleKept ? "kept in place" : "turned with the scene");
            }
        }
    }

    void notePictureYaw(uint64_t id, bool valid, double yaw, int64_t sampleNs)
    {
        std::lock_guard<std::mutex> lock(g_shownMutex);
        g_shownYaw.push_back(Shown{id, valid, yaw, sampleNs});
        while (g_shownYaw.size() > 256u)
            g_shownYaw.pop_front();
    }

    bool pictureYaw(uint64_t id, double &yaw, int64_t &sampleNs)
    {
        std::lock_guard<std::mutex> lock(g_shownMutex);
        for (auto it = g_shownYaw.rbegin(); it != g_shownYaw.rend(); ++it)
            if (it->id == id)
            {
                yaw = it->yaw;
                sampleNs = it->sampleNs;
                return it->valid;
            }
        return false;
    }

    void noteShownPicture(uint64_t id, int64_t shownNs, int64_t dueNs)
    {
        double yaw = 0.0;
        int64_t sampleNs = 0;
        const bool valid = id != 0 && pictureYaw(id, yaw, sampleNs);
        Steps &st = g_steps;
        std::lock_guard<std::mutex> lock(st.mutex);
        if (valid && sampleNs != 0 && shownNs > sampleNs && shownNs - sampleNs < 200000000ll)
            st.toScreen.push_back(static_cast<double>(shownNs - sampleNs) / 1.0e6);
        if (valid && st.lastValid && st.lastNs != 0 && shownNs > st.lastNs && shownNs - st.lastNs < 100000000ll)
        {
            const double dtMs = static_cast<double>(shownNs - st.lastNs) / 1.0e6;
            const double stepDeg = (yaw - st.lastYaw) * 180.0 / kPi;
            st.rate.push_back(stepDeg / (dtMs / 1000.0));
            st.step.push_back(stepDeg);
            st.dt.push_back(dtMs);
            st.rateDue.push_back(dueNs > st.lastDue && st.lastDue != 0 ? stepDeg / (static_cast<double>(dueNs - st.lastDue) / 1.0e9) : 0.0);
        }
        st.lastValid = valid;
        st.lastYaw = yaw;
        st.lastNs = shownNs;
        st.lastDue = dueNs;
    }

    void appendPacing(char *text, size_t size)
    {
        if (size)
            text[0] = '\0';
        Steps &st = g_steps;
        std::lock_guard<std::mutex> lock(st.mutex);
        // How evenly the view turned: of the pictures shown while turning (over 5 degrees a second), the turning speed
        // each step shows (its angle over the time since the picture before) - its average and spread, the slowest and
        // fastest 1% against the average, and steps backwards (against the way it turns: a snap back).
        std::vector<double> turning, steps, byMoment;
        double sum = 0.0;
        for (double r : st.rate)
            sum += r;
        const double mean = st.rate.empty() ? 0.0 : sum / static_cast<double>(st.rate.size());
        size_t back = 0;
        for (size_t i = 0; i < st.rate.size(); ++i)
        {
            if (std::fabs(mean) > 5.0 && std::fabs(st.rate[i]) > 0.0 && (st.rate[i] > 0.0) != (mean > 0.0) && std::fabs(st.step[i]) > 0.01)
                ++back;
            if (std::fabs(st.rate[i]) > 5.0)
            {
                turning.push_back(std::fabs(st.rate[i]));
                steps.push_back(std::fabs(st.step[i]));
                if (st.rateDue[i] != 0.0)
                    byMoment.push_back(std::fabs(st.rateDue[i]));
            }
        }
        if (turning.size() >= 10u && size)
        {
            double tAvg = 0.0, tSd = 0.0, sAvg = 0.0;
            for (double v : turning)
                tAvg += v;
            tAvg /= static_cast<double>(turning.size());
            for (double v : turning)
                tSd += (v - tAvg) * (v - tAvg);
            tSd = std::sqrt(tSd / static_cast<double>(turning.size() - 1u));
            for (double v : steps)
                sAvg += v;
            sAvg /= static_cast<double>(steps.size());
            std::sort(turning.begin(), turning.end());
            const size_t one = std::max<size_t>(1u, turning.size() / 100u);
            double lo = 0.0, hi = 0.0;
            for (size_t i = 0; i < one; ++i)
            {
                lo += turning[i];
                hi += turning[turning.size() - 1u - i];
            }
            lo /= static_cast<double>(one);
            hi /= static_cast<double>(one);
            // (and against the pictures' moments - what the pacer shows them by - leaving out the window's own presentation)
            // (and the jitter: each step's turning speed against the average of the steps either side of it - what changes
            // from picture to picture beyond a smooth speeding up or slowing down, as a share of the speed)
            double jit = 0.0, jitBase = 0.0;
            for (size_t i = 1; i + 1 < st.rateDue.size(); ++i)
                if (st.rateDue[i - 1] != 0.0 && st.rateDue[i] != 0.0 && st.rateDue[i + 1] != 0.0)
                {
                    jit += std::fabs(st.rateDue[i] - 0.5 * (st.rateDue[i - 1] + st.rateDue[i + 1]));
                    jitBase += std::fabs(st.rateDue[i]);
                }
            double mAvg = 0.0, mSd = 0.0;
            for (double v : byMoment)
                mAvg += v;
            mAvg /= std::max<size_t>(1u, byMoment.size());
            for (double v : byMoment)
                mSd += (v - mAvg) * (v - mAvg);
            mSd = byMoment.size() > 1u ? std::sqrt(mSd / static_cast<double>(byMoment.size() - 1u)) : 0.0;
            std::snprintf(text, size, "; turn: %zu steps turning at %.1f deg/s (spread %.1f%%, slowest 1%% %.0f%%, fastest 1%% %.0f%%; against the moments spread %.1f%%, jitter %.1f%%), %.3f deg a step, %zu back",
                          turning.size(), tAvg, 100.0 * tSd / std::max(1e-9, tAvg), 100.0 * lo / std::max(1e-9, tAvg),
                          100.0 * hi / std::max(1e-9, tAvg), 100.0 * mSd / std::max(1e-9, mAvg), 100.0 * jit / std::max(1e-9, jitBase), sAvg, back);
        }
        if (!st.toScreen.empty() && size)
        {
            double avg = 0.0;
            for (double v : st.toScreen)
                avg += v;
            avg /= static_cast<double>(st.toScreen.size());
            std::sort(st.toScreen.begin(), st.toScreen.end());
            const size_t len = std::strlen(text);
            std::snprintf(text + len, size - len, "; from the input to the screen %.1f ms (%.1f..%.1f)", avg, st.toScreen.front(), st.toScreen.back());
        }
        st.toScreen.clear();
        st.rate.clear();
        st.rateDue.clear();
        st.step.clear();
        st.dt.clear();
    }
}
