// Photo mode (photo_mode.h).
//
// The view: every frame taCamera::UpdateScripts (0x2CAB30) blends its scripts into one place and
// one turn and hands them to the camera's scene node: vlSceneNode::SetPosition (0x210FF0, called at
// 0x2CB37C: a1 = x, y, z) and vlSceneNode::SetOrientation (0x210DD0, at 0x2CB388: a1 = a quaternion
// w, x, y, z, made by mlQuaternionFromRotationMatrix 0x1AED80 from the view's matrix, whose first row is where the view looks: the
// camera's own +x, with z up), then the field of view to vlCamera::SetFieldOfView (0x1F2850, at
// 0x2CB41C: f12 in degrees). While photo mode is on, those three calls get the free camera's
// values instead (before the call: the recompiled setters may stop part way). The free camera starts
// where the game's view is (its roll dropped) and R puts it back there.
//
// The game: frozen through its speed (tuning.h: the frame period near zero, so everything that moves
// with it stops: the Autobot, the Decepticons, the effects, the camera's own scripts), the pad held
// at rest (the runtime's pad override, overrides.cpp: the mouse stays captured, so it can look) and
// the game's own HUD switch (the showHUD config setting, game_config.h) off. The crosshair is no
// HUD: it is a model in the world (taModelPredictionCrosshair, its Update 0x2FF730 places it with
// SetPosition from 0x2FFCB4; skipping its DoRender 0x2FF640 does not hide it), so its position goes 100000 units down while the HUD is off. Leaving
// puts the speed and the HUD setting back as they were.
//
// Tests: TF_PHOTO_MODE=1 (on once a level runs), TF_PHOTO_OFFSET="ahead,right,up,yaw,pitch[,fov]"
// (the free camera moved that far as it starts), TF_PHOTO_FRAMES=<n> (off after n game frames),
// TF_PHOTO_TRACE=1 (logs every caller of SetPosition while on: finds other models to hide), TF_PHOTO_LIVE=1 (the game
// not frozen: the free camera watches it run).
//
// Decomp equivalent: in taCamera's update, the place, turn and field of view passed on replaced by
// the free camera's while photo mode is on; the game's period at zero (or its update skipped) and
// showHUD off.

#include "photo_mode.h"

#include "camera_mode.h"
#include "static_aim.h" // (TODO 322: the crosshair pinned to one screen point)
#include "game_config.h"
#include "menu_mouse.h"
#include "port_ext.h"
#include "tuning.h"
#include "view_turn.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_host_input.h"
#include "runtime/ps2_memory.h"

#include "raylib.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace photo_mode
{
    namespace
    {
        constexpr uint32_t kSetPosition = 0x00210FF0u, kSetPositionFromCamera = 0x002CB384u; // (return addresses)
        constexpr uint32_t kSetRotation = 0x00210DD0u, kSetRotationFromCamera = 0x002CB390u;
        constexpr uint32_t kSetFieldOfView = 0x001F2850u, kSetFieldOfViewFromCamera = 0x002CB424u;
        constexpr uint32_t kSetScale = 0x00210D20u; // (vlSceneNode::SetScale: f12, one factor - the crosshair's at 0x2FFDC0)
        constexpr uint32_t kGetScale = 0x001AFD00u;  // (its model's scale, [a0+0x1C] in f0 - read back by the crosshair at 0x2FFD98)
        constexpr uint32_t kCrosshairSetScaleReturn = 0x002FFDC8u, kCrosshairGetScaleReturn = 0x002FFDA0u;
        // The prediction crosshair's code (taModelPredictionCrosshair: Update 0x2FF730 places its model).
        constexpr uint32_t kCrosshairCodeBegin = 0x002FF640u, kCrosshairCodeEnd = 0x00300000u;
        constexpr float kPi = 3.14159265358979f;
        constexpr float kPitchLimit = 1.53f; // (just short of straight up or down)

        PS2Runtime::RecompiledFunction g_setPosition = nullptr, g_setRotation = nullptr, g_setFov = nullptr, g_setScale = nullptr,
                                       g_getScale = nullptr;
        // (TODO 91) The crosshair model's scale against where it is drawn: Update places it at L = min(16, ...) x t
        // from the eye along the aim and scales it by t x the field of view / 90 - the same size on the screen
        // wherever the aim is. On the jet bike its place is moved to the bike's aim point (port_ext.h aimPoint, far
        // past L) while its scale stays L's: the user's "way smaller" crosshair. This is the factor between the two
        // distances from the eye, for the SetScale that follows the SetPosition in the same Update (the game thread).
        float g_crosshairScale = 1.0f;
        // The crosshair's own scale, as the game set it, before the factor: the game eases its scale from the one it set
        // the frame before (GetScale -> SetScale), so the read gets this back - else the factor would compound frame by
        // frame (the first try: 1.2 to 1e38 in a second).
        float g_crosshairGameScale = 1.0f;
        bool g_crosshairScaled = false;
        // (TODO 307 g) The distance from the eye the model is drawn at this frame (0: not known), for its constant size.
        float g_crosshairDrawnAt = 0.0f;
        std::atomic<float> g_crosshairTopK{0.0f}; // (TODO 307 h: the top bracket's height over the half screen, for the meter)

        std::atomic<bool> g_want{false}; // what the player asked for (the window thread)
        std::atomic<bool> g_on{false};   // in effect (the game thread: only in a level)
        std::atomic<bool> g_inLevel{false};
        std::atomic<bool> g_hideCrosshair{false}; // (small_crosshair: drawn by the port instead)
        std::atomic<bool> g_hudOn{false};

        // The free camera (both threads).
        std::mutex g_lock;
        struct Pose
        {
            bool havePosition = false, haveTurn = false; // taken from the game's view yet
            float position[3] = {0.0f, 0.0f, 0.0f};
            float yaw = 0.0f, pitch = 0.0f; // radians: yaw about z from +x, pitch up from level
            float fovScale = 1.0f;
        } g_pose;

        // The game's settings while photo mode is on (the game thread).
        float g_speedBefore = 1.0f;
        int g_hudBefore = -1; // the showHUD byte before (-1: not changed)
        std::atomic<bool> g_hudToRestore{false};

        float loadFloat(const uint8_t *rdram, uint32_t a)
        {
            float v = 0.0f;
            if (const uint8_t *p = getConstMemPtr(rdram, a))
                std::memcpy(&v, p, sizeof(v));
            return v;
        }

        void storeFloat(uint8_t *rdram, uint32_t a, float v)
        {
            if (uint8_t *p = getMemPtr(rdram, a))
                std::memcpy(p, &v, sizeof(v));
        }

        // The quaternion (w, x, y, z) turning +x to the yaw and pitch: about z by the yaw, then about
        // the turned y by minus the pitch (a turn about y by a positive angle takes +x down).
        void quaternionOf(float yaw, float pitch, float q[4])
        {
            const float cy = std::cos(yaw * 0.5f), sy = std::sin(yaw * 0.5f);
            const float cp = std::cos(-pitch * 0.5f), sp = std::sin(-pitch * 0.5f);
            // qz(yaw) * qy(-pitch)
            q[0] = cy * cp;
            q[1] = -sy * sp;
            q[2] = cy * sp;
            q[3] = sy * cp;
        }

        // Where the quaternion turns +x (the view's direction).
        void forwardOf(const float q[4], float f[3])
        {
            const float w = q[0], x = q[1], y = q[2], z = q[3];
            f[0] = 1.0f - 2.0f * (y * y + z * z);
            f[1] = 2.0f * (x * y + w * z);
            f[2] = 2.0f * (x * z - w * y);
        }

        // The view as the camera last set it and the crosshair's place in the world (any thread:
        // project() turns a point of the world into the window's pixels for the overlays).
        struct Seen
        {
            float position[3] = {0, 0, 0};
            float turn[4] = {1, 0, 0, 0};
            float fov = 0.0f; // degrees, across the TV picture (SetFieldOfView's)
            float crosshair[3] = {0, 0, 0};
            std::chrono::steady_clock::time_point crosshairAt{};
            bool widescreen = false;
        } g_seen;
        std::mutex g_seenLock;
        constexpr uint32_t kWidescreenOption = 0x006CF004u; // the game's own 16:9 switch (option 4)

        // (TODO 322) The ray from the view through the crosshair's pinned screen point: the view's ahead raised by `share` of half
        // the view's height (its turn and field of view as seen this frame - the camera's SetPosition / SetOrientation come
        // before the crosshair's Update in a frame). False with TF_XHAIR_PIN=0 or no view yet.
        bool pinRay(float share, float ray[3])
        {
            static const bool off = std::getenv("TF_XHAIR_PIN") && *std::getenv("TF_XHAIR_PIN") == '0';
            if (off)
                return false;
            Seen v;
            {
                std::lock_guard<std::mutex> guard(g_seenLock);
                v = g_seen;
            }
            if (!(v.fov > 1.0f && v.fov < 179.0f))
                return false;
            const float w = v.turn[0], qx = v.turn[1], qy = v.turn[2], qz = v.turn[3];
            const float ahead[3] = {1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy + w * qz), 2 * (qx * qz - w * qy)};
            const float up[3] = {2 * (qx * qz + w * qy), 2 * (qy * qz - w * qx), 1 - 2 * (qx * qx + qy * qy)};
            const float tanUp = std::tan(v.fov * 0.5f * kPi / 180.0f) / (v.widescreen ? 16.0f / 9.0f : 4.0f / 3.0f);
            float n = 0.0f;
            for (int i = 0; i < 3; ++i)
            {
                ray[i] = ahead[i] + up[i] * share * tanUp;
                n += ray[i] * ray[i];
            }
            n = std::sqrt(n);
            if (!(n > 1e-4f) || !std::isfinite(n))
                return false;
            for (int i = 0; i < 3; ++i)
                ray[i] /= n;
            return true;
        }

        void setPosition(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (static const bool adsTrace = std::getenv("TF_ADS_EYE_TRACE") != nullptr; adsTrace && camera_mode::adsFovFactor() < 1.0f)
            {
                // (a measuring aid, TODO 300: the SetPosition callers while aim down sights is in - which one places its camera)
                static uint32_t seen[32];
                static int count = 0;
                const uint32_t ra = getRegU32(ctx, 31);
                bool known = false;
                for (int i = 0; i < count; ++i)
                    known |= seen[i] == ra;
                if (!known && count < 32)
                {
                    seen[count++] = ra;
                    std::fprintf(stderr, "[ads eye] SetPosition from %08x (node %08x) at %.2f %.2f %.2f\n", ra, getRegU32(ctx, 4),
                                 loadFloat(rdram, getRegU32(ctx, 5)), loadFloat(rdram, getRegU32(ctx, 5) + 4u), loadFloat(rdram, getRegU32(ctx, 5) + 8u));
                }
            }
            if (static const bool trace = std::getenv("TF_PHOTO_TRACE") != nullptr; trace && g_on.load())
            {
                static uint32_t seen[64];
                static int count = 0;
                const uint32_t ra = getRegU32(ctx, 31);
                bool known = false;
                for (int i = 0; i < count; ++i)
                    known |= seen[i] == ra;
                if (!known && count < 64)
                {
                    seen[count++] = ra;
                    std::fprintf(stderr, "[photo] SetPosition from %08x (node %08x)\n", ra, getRegU32(ctx, 4));
                }
            }
            const uint32_t from = getRegU32(ctx, 31);
            if (from >= kCrosshairCodeBegin && from < kCrosshairCodeEnd)
            {
                const uint32_t at = getRegU32(ctx, 5);
                // (on the jet bike the targeting follows the turret state's gun, which points nowhere
                // the bike fires: the crosshair goes where its guns do, straight ahead of it)
                // (TODO 193) The model stood at the aim point - on the ground, an enemy's feet, the bike's target -
                // and the ground's depth hid its lower half. It now stands on the same line from the eye, at most
                // kNear in front of it (the bike's aim point the same way), its scale times its new distance over the
                // game's: the same place and size on the screen, in front of what it points at. TF_CROSSHAIR_NEAR=0:
                // the aim point itself (as before).
                static const bool nearOff = std::getenv("TF_CROSSHAIR_NEAR") && *std::getenv("TF_CROSSHAIR_NEAR") == '0';
                constexpr float kNear = 8.0f;
                float aim[3];
                g_crosshairScale = 1.0f;
                g_crosshairDrawnAt = 0.0f;
                const bool bike = ext::aimPoint(rdram, aim); // (an extension's aim point: the jet bike's, port_ext.h)
                if (!bike)
                    for (int i = 0; i < 3; ++i)
                        aim[i] = loadFloat(rdram, at + 4u * i);
                float eye[3];
                {
                    std::lock_guard<std::mutex> guard(g_seenLock);
                    std::memcpy(eye, g_seen.position, sizeof(eye));
                }
                float before = 0.0f, after = 0.0f;
                for (int i = 0; i < 3; ++i)
                {
                    const float b = loadFloat(rdram, at + 4u * i) - eye[i], c = aim[i] - eye[i];
                    before += b * b;
                    after += c * c;
                }
                before = std::sqrt(before);
                after = std::sqrt(after);
                const bool eyeKnown = eye[0] != 0.0f || eye[1] != 0.0f || eye[2] != 0.0f;
                float pinShare = 0.0f, ray[3];
                if (!bike && eyeKnown && before > 1.0e-2f && static_aim::pinned(pinShare) && pinRay(pinShare, ray))
                {
                    // (TODO 322) Mouse and keyboard: the model on this frame's camera ray through the crosshair's one screen
                    // point (static_aim::pinned) - the same place on the screen every frame, whatever the aim meets (before, the
                    // aim point's own place, which drifted with the camera's lag and the line's learnt height). As before at most
                    // kNear ahead (TODO 193); its scale times its distance over the game's: the game's size on the screen. The
                    // pictures between frames leave its draws unturned (view_turn, TODO 292 B: found by this place).
                    // TF_XHAIR_PIN=0: as before.
                    const float d = std::isfinite(after) ? (nearOff ? after : std::min(after, kNear)) : kNear;
                    if (std::isfinite(d) && d > 0.1f)
                    {
                        g_crosshairScale = std::clamp(d / before, 0.01f, 50.0f);
                        g_crosshairDrawnAt = d;
                        for (int i = 0; i < 3; ++i)
                            storeFloat(rdram, at + 4u * i, eye[i] + ray[i] * d);
                    }
                }
                else if (eyeKnown && before > 1.0e-2f && after > 1.0e-2f && std::isfinite(after))
                {
                    const float d = nearOff ? after : std::min(after, kNear);
                    g_crosshairDrawnAt = before;
                    if (bike || d < after)
                    {
                        g_crosshairScale = std::clamp(d / before, 0.01f, 50.0f);
                        g_crosshairDrawnAt = d;
                        for (int i = 0; i < 3; ++i)
                            storeFloat(rdram, at + 4u * i, eye[i] + (aim[i] - eye[i]) * (d / after));
                    }
                }
                else if (bike)
                    for (int i = 0; i < 3; ++i)
                        storeFloat(rdram, at + 4u * i, aim[i]);
                std::lock_guard<std::mutex> guard(g_seenLock);
                for (int i = 0; i < 3; ++i)
                    g_seen.crosshair[i] = loadFloat(rdram, at + 4u * i);
                g_seen.crosshairAt = std::chrono::steady_clock::now();
            }
            if (((g_on.load(std::memory_order_relaxed) && !g_hudOn.load(std::memory_order_relaxed)) ||
                 game_config::hudOff() || // (TODO 228: the HUD setting Off hides it as photo mode does)
                 g_hideCrosshair.load(std::memory_order_relaxed) ||
                 ext::hideCrosshair(rdram)) && // (an extension's: none on the jet bike, TODO 92)
                from >= kCrosshairCodeBegin && from < kCrosshairCodeEnd)
            {
                // The prediction crosshair: a model in the world (placed at 0x2FFCB4), out of the picture.
                const uint32_t at = getRegU32(ctx, 5);
                storeFloat(rdram, at + 8u, loadFloat(rdram, at + 8u) - 100000.0f);
            }
            if (!g_on.load(std::memory_order_relaxed) && from == kSetPositionFromCamera)
            {
                // (TODO 300 a: the steady first-person eye, camera_mode.h)
                const uint32_t at = getRegU32(ctx, 5);
                float p[3] = {loadFloat(rdram, at), loadFloat(rdram, at + 4u), loadFloat(rdram, at + 8u)};
                const float before[3] = {p[0], p[1], p[2]};
                camera_mode::steadyEye(rdram, p);
                for (int i = 0; i < 3; ++i)
                    if (p[i] != before[i])
                        storeFloat(rdram, at + 4u * i, p[i]);
            }
            if (g_on.load(std::memory_order_relaxed) && from == kSetPositionFromCamera)
            {
                const uint32_t at = getRegU32(ctx, 5);
                std::lock_guard<std::mutex> guard(g_lock);
                if (!g_pose.havePosition)
                {
                    for (int i = 0; i < 3; ++i)
                        g_pose.position[i] = loadFloat(rdram, at + 4u * i);
                    g_pose.havePosition = true;
                }
                else
                    for (int i = 0; i < 3; ++i)
                        storeFloat(rdram, at + 4u * i, g_pose.position[i]);
            }
            if (from == kSetPositionFromCamera)
            {
                view_turn::noteViewNode(getRegU32(ctx, 4)); // (TODO 292 A: the view's node, for the pictures' turn)
                const uint32_t at = getRegU32(ctx, 5);
                std::lock_guard<std::mutex> guard(g_seenLock);
                for (int i = 0; i < 3; ++i)
                    g_seen.position[i] = loadFloat(rdram, at + 4u * i);
                const uint8_t *ws = getConstMemPtr(rdram, kWidescreenOption);
                g_seen.widescreen = ws && *ws != 0u;
            }
            g_setPosition(rdram, ctx, runtime);
        }

        void setRotation(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_on.load(std::memory_order_relaxed) && getRegU32(ctx, 31) == kSetRotationFromCamera)
            {
                const uint32_t at = getRegU32(ctx, 5);
                std::lock_guard<std::mutex> guard(g_lock);
                if (!g_pose.haveTurn)
                {
                    float q[4], f[3];
                    for (int i = 0; i < 4; ++i)
                        q[i] = loadFloat(rdram, at + 4u * i);
                    forwardOf(q, f);
                    g_pose.yaw = std::atan2(f[1], f[0]);
                    g_pose.pitch = std::asin(std::clamp(f[2], -1.0f, 1.0f));
                    g_pose.haveTurn = true;
                    // TF_PHOTO_OFFSET="ahead,right,up,yaw,pitch[,fov]" (tests: units, degrees, a factor): the
                    // free camera moved that far from the game's view as it starts.
                    static const char *offset = std::getenv("TF_PHOTO_OFFSET");
                    float o[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
                    if (offset && std::sscanf(offset, "%f,%f,%f,%f,%f,%f", &o[0], &o[1], &o[2], &o[3], &o[4], &o[5]) >= 3)
                    {
                        const float cy = std::cos(g_pose.yaw), sy = std::sin(g_pose.yaw);
                        const float cp = std::cos(g_pose.pitch), sp = std::sin(g_pose.pitch);
                        const float ahead[3] = {cy * cp, sy * cp, sp}, right[3] = {sy, -cy, 0.0f};
                        for (int i = 0; i < 3; ++i)
                            g_pose.position[i] += ahead[i] * o[0] + right[i] * o[1] + (i == 2 ? o[2] : 0.0f);
                        g_pose.yaw += o[3] * kPi / 180.0f;
                        g_pose.pitch = std::clamp(g_pose.pitch + o[4] * kPi / 180.0f, -kPitchLimit, kPitchLimit);
                        g_pose.fovScale = std::clamp(o[5], 0.2f, 2.0f);
                    }
                    std::fprintf(stderr, "[photo] the view at (%.2f, %.2f, %.2f), looking (%.3f, %.3f, %.3f): yaw %.1f, pitch %.1f degrees\n",
                                 g_pose.position[0], g_pose.position[1], g_pose.position[2], f[0], f[1], f[2],
                                 g_pose.yaw * 180.0f / kPi, g_pose.pitch * 180.0f / kPi);
                }
                float q[4];
                quaternionOf(g_pose.yaw, g_pose.pitch, q);
                for (int i = 0; i < 4; ++i)
                    storeFloat(rdram, at + 4u * i, q[i]);
            }
            if (getRegU32(ctx, 31) == kSetRotationFromCamera)
            {
                const uint32_t at = getRegU32(ctx, 5);
                std::lock_guard<std::mutex> guard(g_seenLock);
                for (int i = 0; i < 4; ++i)
                    g_seen.turn[i] = loadFloat(rdram, at + 4u * i);
            }
            g_setRotation(rdram, ctx, runtime);
        }

        void setFieldOfView(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_on.load(std::memory_order_relaxed) && getRegU32(ctx, 31) == kSetFieldOfViewFromCamera)
            {
                float scale;
                {
                    std::lock_guard<std::mutex> guard(g_lock);
                    scale = g_pose.fovScale;
                }
                ctx->f[12] = std::clamp(ctx->f[12] * scale, 10.0f, 150.0f);
            }
            if (!g_on.load(std::memory_order_relaxed) && getRegU32(ctx, 31) == kSetFieldOfViewFromCamera)
                ctx->f[12] = camera_mode::fieldOfView(ctx->f[12]); // (TODO 300: the Apex mod's field of view and aim down sights)
            if (getRegU32(ctx, 31) == kSetFieldOfViewFromCamera)
            {
                std::lock_guard<std::mutex> guard(g_seenLock);
                g_seen.fov = ctx->f[12];
            }
            g_setFov(rdram, ctx, runtime);
        }

        void setScale(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t from = getRegU32(ctx, 31);
            if (from == kCrosshairSetScaleReturn)
            {
                g_crosshairGameScale = ctx->f[12];
                g_crosshairScaled = g_crosshairScale != 1.0f;
            }
            // (TODO 307 g) One size on the screen, near or far, in first person: the game sizes the reticle by the aim's distance
            // (2 t^2 with t = (500 - d) / 500, clamped 1.3..4, x fov / 90 - taModelPredictionCrosshair::Update) and places it
            // nearer for a near aim, so it changed size as the aim swept from a wall to the distance. Here its scale is the
            // distance it is drawn at times one factor, the size it has at 16 units for a 2.0 game size, kept for the field of
            // view (tan of its half). TF_CROSSHAIR_CONSTANT=0: the game's sizes; =2: in third person too.
            static const int constantMode = [] {
                const char *e = std::getenv("TF_CROSSHAIR_CONSTANT");
                return e && *e ? std::atoi(e) : 1;
            }();
            if (from == kCrosshairSetScaleReturn && constantMode > 0 && g_crosshairDrawnAt > 0.01f &&
                (constantMode >= 2 || camera_mode::firstPerson()))
            {
                float fov = 0.0f;
                {
                    std::lock_guard<std::mutex> guard(g_seenLock);
                    fov = g_seen.fov;
                }
                const float half = std::clamp(fov > 1.0f && fov < 179.0f ? fov : 90.0f, 10.0f, 170.0f) * 0.5f * kPi / 180.0f;
                ctx->f[12] = 0.125f * std::tan(half) * g_crosshairDrawnAt;
                g_crosshairScaled = true;
                if (static const bool log = std::getenv("TF_XHAIR_LOG") != nullptr; log)
                {
                    static auto last = std::chrono::steady_clock::time_point{};
                    if (std::chrono::steady_clock::now() - last > std::chrono::seconds(1))
                    {
                        last = std::chrono::steady_clock::now();
                        std::fprintf(stderr, "[crosshair] one size: game %.3f, drawn %.3f at %.2f (fov %.1f)\n", g_crosshairGameScale, ctx->f[12],
                                     g_crosshairDrawnAt, fov);
                    }
                }
            }
            else if (from == kCrosshairSetScaleReturn && g_crosshairScale != 1.0f)
            {
                // (TF_XHAIR_LOG=1: the factor, once a second)
                if (static const bool log = std::getenv("TF_XHAIR_LOG") != nullptr; log)
                {
                    static auto last = std::chrono::steady_clock::time_point{};
                    if (std::chrono::steady_clock::now() - last > std::chrono::seconds(1))
                    {
                        last = std::chrono::steady_clock::now();
                        std::fprintf(stderr, "[crosshair] the model moved along its line: scale %.3f x %.2f\n", ctx->f[12], g_crosshairScale);
                    }
                }
                ctx->f[12] *= g_crosshairScale;
            }
            if (from == kCrosshairSetScaleReturn)
            {
                // (TODO 307 h) The ring's top bracket over the half screen's height: its model reaches 1.39 units above its
                // centre (measured: 125 of 720 half-lines at a scale of 0.0871 a unit away, fov 69.7), seen at the drawn
                // scale over the distance, through the field of view's tan.
                float fov = 0.0f;
                {
                    std::lock_guard<std::mutex> guard(g_seenLock);
                    fov = g_seen.fov;
                }
                const float half = std::clamp(fov > 1.0f && fov < 179.0f ? fov : 90.0f, 10.0f, 170.0f) * 0.5f * kPi / 180.0f;
                g_crosshairTopK.store(g_crosshairDrawnAt > 0.01f ? 1.389f * ctx->f[12] / g_crosshairDrawnAt / std::tan(half) : 0.0f,
                                      std::memory_order_relaxed);
            }
            g_setScale(rdram, ctx, runtime);
        }

        void getScale(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t from = getRegU32(ctx, 31);
            g_getScale(rdram, ctx, runtime);
            if (from == kCrosshairGetScaleReturn && g_crosshairScaled)
                ctx->f[0] = g_crosshairGameScale; // (the game's own, not the one drawn: see g_crosshairGameScale)
        }

        void hook(PS2Runtime &runtime, uint32_t address, PS2Runtime::RecompiledFunction &original,
                  PS2Runtime::RecompiledFunction fn)
        {
            original = runtime.lookupFunction(address);
            if (original)
                runtime.registerFunction(address, fn);
            else
                std::fprintf(stderr, "[photo] no function at %08x: not hooked\n", address);
        }
    }

    void install(PS2Runtime &runtime)
    {
        if (const char *env = std::getenv("TF_PHOTO_MODE"); env && *env && *env != '0')
            g_want.store(true);
        hook(runtime, kSetPosition, g_setPosition, setPosition);
        hook(runtime, kSetRotation, g_setRotation, setRotation);
        hook(runtime, kSetFieldOfView, g_setFov, setFieldOfView);
        hook(runtime, kSetScale, g_setScale, setScale);
        hook(runtime, kGetScale, g_getScale, getScale);
    }

    bool active() { return g_on.load(std::memory_order_relaxed); }

    bool takeHudRestore() { return g_hudToRestore.exchange(false); }
    bool hudHidden() { return g_on.load(std::memory_order_relaxed) && !g_hudOn.load(std::memory_order_relaxed); }

    void setHideCrosshair(bool hide) { g_hideCrosshair.store(hide); }

    bool crosshairTop(float screenHeight, float &pixels)
    {
        {
            std::lock_guard<std::mutex> guard(g_seenLock);
            if (std::chrono::steady_clock::now() - g_seen.crosshairAt > std::chrono::milliseconds(200))
                return false;
        }
        const float k = g_crosshairTopK.load(std::memory_order_relaxed);
        if (!(k > 0.0f) || !std::isfinite(k))
            return false;
        pixels = k * screenHeight * 0.5f;
        return true;
    }

    bool crosshair(float point[3])
    {
        std::lock_guard<std::mutex> guard(g_seenLock);
        if (std::chrono::steady_clock::now() - g_seen.crosshairAt > std::chrono::milliseconds(200))
            return false;
        std::memcpy(point, g_seen.crosshair, sizeof(g_seen.crosshair));
        return true;
    }

    bool viewPosition(float point[3])
    {
        std::lock_guard<std::mutex> guard(g_seenLock);
        std::memcpy(point, g_seen.position, sizeof(g_seen.position));
        return point[0] != 0.0f || point[1] != 0.0f || point[2] != 0.0f;
    }

    bool viewFrame(float ahead[3], float left[3], float up[3], float &tanUp)
    {
        Seen v;
        {
            std::lock_guard<std::mutex> guard(g_seenLock);
            v = g_seen;
        }
        if (!(v.fov > 1.0f && v.fov < 179.0f))
            return false;
        const float w = v.turn[0], qx = v.turn[1], qy = v.turn[2], qz = v.turn[3];
        const float a[3] = {1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy + w * qz), 2 * (qx * qz - w * qy)};
        const float l[3] = {2 * (qx * qy - w * qz), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz + w * qx)};
        const float u[3] = {2 * (qx * qz + w * qy), 2 * (qy * qz - w * qx), 1 - 2 * (qx * qx + qy * qy)};
        for (int i = 0; i < 3; ++i)
        {
            ahead[i] = a[i];
            left[i] = l[i];
            up[i] = u[i];
        }
        tanUp = std::tan(v.fov * 0.5f * kPi / 180.0f) / (v.widescreen ? 16.0f / 9.0f : 4.0f / 3.0f);
        return std::isfinite(tanUp) && tanUp > 0.0f;
    }

    bool project(const float point[3], float windowW, float windowH, float pictureAspect, float &x, float &y)
    {
        Seen v;
        {
            std::lock_guard<std::mutex> guard(g_seenLock);
            v = g_seen;
        }
        if (!(v.fov > 1.0f && v.fov < 179.0f) || pictureAspect <= 0.0f)
            return false;
        // the view's axes: +x ahead, +z up (the view matrix's rows), +y to the left
        const float w = v.turn[0], qx = v.turn[1], qy = v.turn[2], qz = v.turn[3];
        const float ahead[3] = {1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy + w * qz), 2 * (qx * qz - w * qy)};
        const float left[3] = {2 * (qx * qy - w * qz), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz + w * qx)};
        const float up[3] = {2 * (qx * qz + w * qy), 2 * (qy * qz - w * qx), 1 - 2 * (qx * qx + qy * qy)};
        float d[3];
        for (int i = 0; i < 3; ++i)
            d[i] = point[i] - v.position[i];
        const float z = d[0] * ahead[0] + d[1] * ahead[1] + d[2] * ahead[2];
        if (z <= 0.01f)
            return false;
        const float sx = -(d[0] * left[0] + d[1] * left[1] + d[2] * left[2]) / z;
        const float sy = (d[0] * up[0] + d[1] * up[1] + d[2] * up[2]) / z;
        // The angle is across the TV picture the game draws for (4:3, 16:9 in its widescreen mode); the
        // window's wider shapes add width (hor+), the height's angle stays.
        const float tv = v.widescreen ? 16.0f / 9.0f : 4.0f / 3.0f;
        const float tanUp = std::tan(v.fov * 0.5f * kPi / 180.0f) / tv;
        const float picW = std::min(windowW, windowH * pictureAspect), picH = picW / pictureAspect;
        x = windowW * 0.5f + sx / (tanUp * pictureAspect) * picW * 0.5f;
        y = windowH * 0.5f - sy / tanUp * picH * 0.5f;
        return true;
    }

    void toggle()
    {
        // (outside a level there is nothing to photograph: F8 then does nothing)
        g_want.store(!g_want.load() && g_inLevel.load(std::memory_order_relaxed));
    }

    namespace
    {
        void drawHint()
        {
            static const char *lines[] = {"PHOTO MODE", "mouse / arrows  look     W A S D  move     Space E / Q Ctrl  up / down",
                                          "Shift  faster     Alt  slower     wheel  + / -  field of view",
                                          "H  HUD     R  back to the game's view     F8 / Esc  leave"};
            const int size = std::max(16, GetScreenHeight() / 40);
            int y = GetScreenHeight() - size * 6;
            for (const char *line : lines)
            {
                const int x = GetScreenWidth() / 2 - MeasureText(line, size) / 2;
                DrawText(line, x + 2, y + 2, size, Color{0, 0, 0, 170});
                DrawText(line, x, y, size, Color{230, 240, 255, 230});
                y += size + size / 3;
            }
        }
    }

    void windowFrame()
    {
        static const bool watching = (ps2_host_input::watchKey(KEY_F8), true);
        (void)watching;
        if ((ps2_host_input::takeKeyPresses(KEY_F8) % 2) == 1)
            toggle();
        static bool wasOn = false;
        static double hintUntil = 0.0;
        const bool on = g_on.load(std::memory_order_relaxed);
        if (on && !wasOn)
            hintUntil = GetTime() + 6.0;
        wasOn = on;
        if (!on)
            return;
        if (GetTime() < hintUntil)
            drawHint();
        if (IsKeyPressed(KEY_ESCAPE))
        {
            g_want.store(false);
            return;
        }
        if (IsKeyPressed(KEY_H))
            g_hudOn.store(!g_hudOn.load());

        const float dt = std::min(GetFrameTime(), 0.1f);
        const bool fast = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        const bool slow = IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);
        const float speed = 10.0f * (fast ? 5.0f : 1.0f) * (slow ? 0.2f : 1.0f); // units (about metres) a second
        const float turn = 1.6f * (slow ? 0.3f : 1.0f);                         // radians a second (arrow keys)
        // (TODO 290 (6): F1 > Controls > Mouse in the game Off - not the mouse either)
        const bool mouseOn = menu_mouse::gameMouseOn();
        const Vector2 mouse = IsWindowFocused() && mouseOn ? GetMouseDelta() : Vector2{0.0f, 0.0f};
        const float wheel = mouseOn ? GetMouseWheelMove() : 0.0f;

        std::lock_guard<std::mutex> guard(g_lock);
        if (IsKeyPressed(KEY_R))
        {
            g_pose.havePosition = g_pose.haveTurn = false; // (the game's view again, from its next frame)
            g_pose.fovScale = 1.0f;
            return;
        }
        if (!g_pose.havePosition || !g_pose.haveTurn)
            return;
        g_pose.yaw -= mouse.x * 0.0025f * (slow ? 0.3f : 1.0f);
        g_pose.pitch -= mouse.y * 0.0025f * (slow ? 0.3f : 1.0f);
        if (IsKeyDown(KEY_LEFT))
            g_pose.yaw += turn * dt;
        if (IsKeyDown(KEY_RIGHT))
            g_pose.yaw -= turn * dt;
        if (IsKeyDown(KEY_UP))
            g_pose.pitch += turn * dt;
        if (IsKeyDown(KEY_DOWN))
            g_pose.pitch -= turn * dt;
        g_pose.pitch = std::clamp(g_pose.pitch, -kPitchLimit, kPitchLimit);
        g_pose.yaw = std::remainder(g_pose.yaw, 2.0f * kPi);

        float zoom = 0.0f;
        if (wheel != 0.0f)
            zoom -= wheel * 0.08f;
        if (IsKeyDown(KEY_EQUAL) || IsKeyDown(KEY_KP_ADD))
            zoom -= 0.8f * dt;
        if (IsKeyDown(KEY_MINUS) || IsKeyDown(KEY_KP_SUBTRACT))
            zoom += 0.8f * dt;
        g_pose.fovScale = std::clamp(g_pose.fovScale * std::exp(zoom), 0.2f, 2.0f);

        const float cy = std::cos(g_pose.yaw), sy = std::sin(g_pose.yaw);
        const float cp = std::cos(g_pose.pitch), sp = std::sin(g_pose.pitch);
        const float ahead[3] = {cy * cp, sy * cp, sp};
        const float right[3] = {sy, -cy, 0.0f}; // (looking along +x, +y is to the left)
        float move[3] = {0.0f, 0.0f, 0.0f};
        const auto add = [&move](const float v[3], float k)
        {
            for (int i = 0; i < 3; ++i)
                move[i] += v[i] * k;
        };
        if (IsKeyDown(KEY_W))
            add(ahead, 1.0f);
        if (IsKeyDown(KEY_S))
            add(ahead, -1.0f);
        if (IsKeyDown(KEY_D))
            add(right, 1.0f);
        if (IsKeyDown(KEY_A))
            add(right, -1.0f);
        if (IsKeyDown(KEY_SPACE) || IsKeyDown(KEY_E))
            move[2] += 1.0f;
        if (IsKeyDown(KEY_Q) || IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL))
            move[2] -= 1.0f;
        for (int i = 0; i < 3; ++i)
            g_pose.position[i] += move[i] * speed * dt;
    }

    void frame(uint8_t *rdram, bool inLevel)
    {
        g_inLevel.store(inLevel, std::memory_order_relaxed);
        // TF_PHOTO_AT=<n> (tests): photo mode on after n game frames in a level (TF_PHOTO_MODE's, later).
        // (Not again in the process a settings restart starts.)
        static const int photoAt = std::getenv("TF_PHOTO_AT") && !std::getenv("PS2X_RESTART_STATE") ? std::atoi(std::getenv("TF_PHOTO_AT")) : 0;
        static int levelFrames = 0;
        if (photoAt > 0 && inLevel && ++levelFrames == photoAt)
        {
            g_want.store(true);
            std::fprintf(stderr, "[photo] TF_PHOTO_AT: on after %d level frames\n", photoAt);
        }
        const bool want = g_want.load(std::memory_order_relaxed) && inLevel;
        const uint32_t hud = game_config::valueAddress("showHUD");
        // TODO 269: that state also holds photo mode's HUD off: the setting and this HUD's flag on again.
        if (tuning::takeFrozenState() && !want && !game_config::hudOff())
        {
            if (uint8_t *p = hud ? getMemPtr(rdram, hud) : nullptr)
                *p = 1u;
            g_hudToRestore.store(true);
        }
        if (want != g_on.load(std::memory_order_relaxed))
        {
            // TF_PHOTO_LIVE=1 (tests, N8 item 4): the game left running - the free camera watches it move.
            static const bool live = std::getenv("TF_PHOTO_LIVE") && *std::getenv("TF_PHOTO_LIVE") == '1';
            if (want)
            {
                g_speedBefore = tuning::speed();
                if (!live)
                    tuning::setSpeed(0.0f);
                {
                    std::lock_guard<std::mutex> guard(g_lock);
                    g_pose = Pose{};
                }
                g_hudOn.store(false);
                std::fprintf(stderr, "[photo] on\n");
            }
            else
            {
                if (!live)
                    tuning::setSpeed(g_speedBefore);
                if (g_hudBefore >= 0 && hud)
                    if (uint8_t *p = getMemPtr(rdram, hud))
                        *p = static_cast<uint8_t>(g_hudBefore);
                g_hudBefore = -1;
                std::fprintf(stderr, "[photo] off\n");
            }
            g_on.store(want);
            if (!inLevel)
                g_want.store(false); // (a level that ended leaves photo mode)
        }
        if (want)
        {
            // TF_PHOTO_FRAMES=<n> (tests): leave photo mode after n game frames of it.
            static const int leaveAfter = std::getenv("TF_PHOTO_FRAMES") ? std::atoi(std::getenv("TF_PHOTO_FRAMES")) : 0;
            static int frames = 0;
            if (leaveAfter > 0 && ++frames == leaveAfter)
                g_want.store(false);
        }
        if (!want || !hud)
            return;
        if (uint8_t *p = getMemPtr(rdram, hud))
        {
            if (g_hudBefore < 0)
                g_hudBefore = *p;
            *p = g_hudOn.load(std::memory_order_relaxed) ? static_cast<uint8_t>(g_hudBefore) : 0u;
        }
    }
}
