// A still crosshair for the mouse (static_aim.h, TODO 292 B).
//
// The game's crosshair point (the transformer context's +0x290, kept again at +0x2A0; what the weapons fire at, the
// crosshair model's place) comes from taTransformerMotionShaderAimTorso::CalculateCrosshair (0x46B390): a line test
// (CalculateCollisionPoint 0x46A370) from the third-person camera's PIVOT (GetThirdPersonCameraPivotPoint 0x2D1330,
// over the shoulder) along the aim direction (GetAimDirection 0x348810), as far as the longer weapon reaches; the hit's
// distance goes to +0x2C8. It does that every other call (the shader's +0xD1 flips each call): between, the point is the
// pivot plus the aim times that distance, and a second test from the body checks the range. The camera's eye is not on
// that line (it stands behind and above the pivot, looking at the player's local target), so the point's place on the
// screen depends on its distance: a near wall or an enemy's legs pulls the crosshair down and aside, a far hill puts it
// back - "the crosshair changes place if you aim at something near vs far" (the user, 2026-11-12).
//
// Here the line is the camera's own ray through one fixed place on the screen: from the eye (its offset from the pivot,
// measured in the aim's frame at the frame before and put on this frame's pivot and aim) along the camera's ahead
// raised by the crosshair's share of the picture's height (the camera's axes, as photo_mode saw them, carried along with
// the aim's turn since). That share is where the game's own far point shows with the camera level (learnt; centre
// across): every point of the ray shows there, near or far, at any pitch, picture shape or field of view (the share is
// of the view's height, whose angle the game keeps). The test starts at the player's own depth, so nothing between the
// camera and the player is hit; the in-between frames' point goes on the same ray. The weapons fire at the point as
// before, so shots go where the crosshair is.
//
// Left to the game: R3's aiming view and first-person aiming (state 9 - its camera already looks at the point), first
// person's free look (the eye looks along the aim: the crosshair already keeps its place), a
// turret (state 0x13, the muzzles'), the jet bike (its own aim, port_ext.h), photo mode, and a frame whose eye is not
// known or lies far from the pivot (a camera cut).
//
// TF_STATIC_AIM: 1 = while the mouse turns the view (the default; the pad keeps the game's aim), 2 = always, 0 = the
// game's. TF_AIM_LOG=1: the offset and the line, twice a second.
//
// Decomp equivalent: CalculateCrosshair taking the start of its line test from the camera's eye (projected to the
// pivot's depth) rather than the pivot.

#include "static_aim.h"

#include "camera_mode.h"
#include "game_version.h"
#include "photo_mode.h"
#include "port.h"
#include "port_ext.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_settings.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace static_aim
{
    namespace
    {
        constexpr uint32_t kCalculateCrosshair = TF_GA(0x0046B390u);
        constexpr uint32_t kCollisionPoint = TF_GA(0x0046A370u);
        constexpr uint32_t kAimDirection = TF_GA(0x00348810u);
        constexpr uint32_t kPivotPoint = 0x002D1330u; // GetThirdPersonCameraPivotPoint
        constexpr uint32_t kTarget = 0x290u, kTarget2 = 0x2A0u, kRange = 0x2C8u, kSeat = 0xB8u;
        constexpr uint32_t kFlip = 0xD1u;   // the shader's: the line test this call
        constexpr uint32_t kState = 0xF8u;  // the actor's state (9 aiming, 0x13 a turret)

        PS2Runtime::RecompiledFunction g_crosshair = nullptr, g_collision = nullptr, g_aimDirection = nullptr, g_pivot = nullptr;
        std::atomic<int> g_mode{1}; // 0 the game's, 1 with the mouse, 2 always

        struct V3
        {
            float x = 0.0f, y = 0.0f, z = 0.0f;
        };
        V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
        V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
        V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
        float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
        V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
        float length(V3 a) { return std::sqrt(dot(a, a)); }
        bool finite(V3 a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }

        V3 readV3(const uint8_t *rdram, uint32_t at)
        {
            V3 v;
            if (const uint8_t *p = at >= 0x00100000u && at < PS2_RAM_SIZE - 12u ? getConstMemPtr(const_cast<uint8_t *>(rdram), at) : nullptr)
                std::memcpy(&v, p, sizeof(v));
            return v;
        }
        // (the stack copies the calls take their vectors from lie in the scratchpad's stack too: any address the memory has)
        V3 readAny(uint8_t *rdram, uint32_t at)
        {
            V3 v;
            if (const uint8_t *p = getConstMemPtr(rdram, at))
                std::memcpy(&v, p, sizeof(v));
            return v;
        }
        void writeAny(uint8_t *rdram, uint32_t at, V3 v)
        {
            if (uint8_t *p = getMemPtr(rdram, at))
                std::memcpy(p, &v, sizeof(v));
        }
        uint32_t word(const uint8_t *rdram, uint32_t at)
        {
            uint32_t v = 0u;
            if (const uint8_t *p = at >= 0x00100000u && at < PS2_RAM_SIZE - 4u ? getConstMemPtr(const_cast<uint8_t *>(rdram), at) : nullptr)
                std::memcpy(&v, p, sizeof(v));
            return v;
        }
        float fword(const uint8_t *rdram, uint32_t at)
        {
            const uint32_t w = word(rdram, at);
            float f;
            std::memcpy(&f, &w, sizeof(f));
            return f;
        }

        // The aim's frame: ahead, right (level), up. False when the aim is (near) straight up or down.
        bool frameOf(V3 aim, V3 &ahead, V3 &right, V3 &up)
        {
            const float n = length(aim);
            if (!(n > 1e-4f) || !finite(aim))
                return false;
            ahead = aim * (1.0f / n);
            right = cross(ahead, V3{0.0f, 0.0f, 1.0f});
            const float r = length(right);
            if (!(r > 0.05f))
                return false;
            right = right * (1.0f / r);
            up = cross(right, ahead);
            return true;
        }

        // This call of CalculateCrosshair.
        struct Call
        {
            bool in = false;
            bool active = false;     // the line moved this call (else only the pivot recorded)
            uint32_t context = 0u;
            bool far = false;        // the in-between call: no line test from the pivot
            bool havePivot = false, haveAim = false;
            V3 pivot, aim;
            bool farDone = false;    // the in-between point put on the line
            bool lineDone = false;   // the line test moved
        } g_call;

        // The eye's offset from the pivot in the aim's frame (its right and up parts), from the frame before.
        struct Offset
        {
            bool prevValid = false;
            V3 prevPivot, prevAim;
            bool valid = false;
            float right = 0.0f, up = 0.0f, ahead = 0.0f;
            // The camera's axes (its ahead and up) in the aim's frame of the frame before - (ahead, right, up) parts - and
            // the crosshair's height: the up-share of the view where the game's aim shows with the camera level.
            bool camValid = false;
            V3 camAhead, camUp;
            float tanUp = 0.0f;
            float prevPitch = 0.0f;
            bool shareKnown = false;
            float share = 0.0f;
        } g_offset;

        // The camera's pivot as the game last placed it (0x2D1330 from CalculateCrosshair) - the centre the third-person
        // camera turns about, for view_turn's pictures (cameraPivot).
        struct Pivot
        {
            V3 at;
            std::chrono::steady_clock::time_point when{};
            bool valid = false;
        } g_lastPivot;

        bool logOn()
        {
            static const bool on = std::getenv("TF_AIM_LOG") && *std::getenv("TF_AIM_LOG") == '1';
            return on;
        }

        // The line: from the eye (its offset from the pivot put on this frame's pivot and aim) through the crosshair's
        // place - the camera's ahead raised by the crosshair's share of the view's height, the camera's axes carried
        // along with the aim from the frame before - started at the player's own depth.
        bool lineStart(V3 pivot, V3 aim, V3 &from, V3 &dir)
        {
            V3 f, r, u;
            if (!g_offset.valid || !frameOf(aim, f, r, u))
                return false;
            const V3 eye = pivot + f * g_offset.ahead + r * g_offset.right + u * g_offset.up;
            dir = f; // (no camera known yet: along the aim, the eye's line - near or far, the same place)
            if (g_offset.camValid && g_offset.shareKnown)
            {
                const V3 &ca = g_offset.camAhead, &cu = g_offset.camUp;
                const V3 a = f * ca.x + r * ca.y + u * ca.z, up = f * cu.x + r * cu.y + u * cu.z;
                const V3 d = a + up * (g_offset.share * g_offset.tanUp);
                const float n = length(d);
                if (n > 1e-4f && finite(d))
                    dir = d * (1.0f / n);
            }
            from = eye + dir * dot(pivot - eye, dir);
            return finite(from) && finite(dir);
        }

        void measureOffset(const uint8_t *rdram)
        {
            g_offset.valid = false;
            float eye[3];
            if (!g_offset.prevValid || !photo_mode::viewPosition(eye))
                return;
            V3 ahead, right, up;
            if (!frameOf(g_offset.prevAim, ahead, right, up))
                return;
            const V3 d = V3{eye[0], eye[1], eye[2]} - g_offset.prevPivot;
            // (a cut, a respawn, a camera far from the player: left to the game)
            if (!finite(d) || length(d) > 60.0f)
                return;
            g_offset.ahead = dot(d, ahead);
            g_offset.right = dot(d, right);
            g_offset.up = dot(d, up);
            g_offset.valid = true;
            (void)rdram;
            float ca[3], cl[3], cu[3], tanUp = 0.0f;
            g_offset.camValid = photo_mode::viewFrame(ca, cl, cu, tanUp);
            if (!g_offset.camValid)
                return;
            const V3 A{ca[0], ca[1], ca[2]}, U{cu[0], cu[1], cu[2]};
            g_offset.camAhead = V3{dot(A, ahead), dot(A, right), dot(A, up)};
            g_offset.camUp = V3{dot(U, ahead), dot(U, right), dot(U, up)};
            g_offset.tanUp = tanUp;
            // (the crosshair's height: where the game's own aim shows while the camera is level - its far point - learnt
            // and kept; TF_STATIC_AIM_SHARE=<f> sets it, the share of half the picture's height above the centre)
            static const float fixedShare = std::getenv("TF_STATIC_AIM_SHARE") ? std::strtof(std::getenv("TF_STATIC_AIM_SHARE"), nullptr) : -100.0f;
            if (fixedShare > -99.0f)
            {
                g_offset.share = fixedShare;
                g_offset.shareKnown = true;
            }
            else if (std::fabs(g_offset.prevPitch) < 0.06f)
            {
                const float along = dot(ahead, A);
                if (along > 0.5f)
                {
                    const float share = dot(ahead, U) / along / tanUp;
                    if (std::isfinite(share) && std::fabs(share) < 0.6f)
                    {
                        g_offset.share = g_offset.shareKnown ? g_offset.share + 0.1f * (share - g_offset.share) : share;
                        g_offset.shareKnown = true;
                    }
                }
            }
        }

        bool wanted(const uint8_t *rdram, uint32_t context)
        {
            const int mode = g_mode.load(std::memory_order_relaxed);
            if (mode == 0 || (mode == 1 && !tf::port::mouseTurnsView()) || photo_mode::active() || context == 0u)
                return false;
            // (first person: the eye looks along the aim already - the game's crosshair keeps its place; measured, the line
            // moved it more than it does itself)
            if (camera_mode::firstPerson() && !camera_mode::inVehicle())
                return false;
            const uint32_t actor = word(rdram, context);
            const uint32_t state = actor ? word(rdram, actor + kState) : 0u;
            if (state == 9u || state == 0x13u)
                return false;
            float bike[3];
            return !ext::aimPoint(rdram, bike); // (the jet bike's aim: its own)
        }

        void pivotPoint(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t out = getRegU32(ctx, 4); // (the vector returned through a0)
            const bool take = g_call.in && !g_call.havePivot;
            g_pivot(rdram, ctx, runtime);
            if (take)
            {
                g_call.pivot = readAny(rdram, out);
                g_call.havePivot = finite(g_call.pivot);
            }
        }

        void aimDirection(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t out = getRegU32(ctx, 4);
            const bool take = g_call.in && !g_call.haveAim;
            g_aimDirection(rdram, ctx, runtime);
            if (take)
            {
                g_call.aim = readAny(rdram, out);
                g_call.haveAim = finite(g_call.aim);
            }
        }

        // The in-between point on the line: the start plus the aim times the last test's distance.
        bool placeFar(uint8_t *rdram)
        {
            V3 from, ahead;
            if (!g_call.havePivot || !g_call.haveAim || !lineStart(g_call.pivot, g_call.aim, from, ahead))
                return false;
            const float range = fword(rdram, g_call.context + kRange);
            if (!std::isfinite(range) || range < 0.0f)
                return false;
            const V3 target = from + ahead * range;
            writeAny(rdram, g_call.context + kTarget, target);
            writeAny(rdram, g_call.context + kTarget2, target);
            g_call.farDone = true;
            return true;
        }

        void collisionPoint(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_call.in && g_call.active && g_call.havePivot && g_call.haveAim)
            {
                const uint32_t fromAt = getRegU32(ctx, 6), dirAt = getRegU32(ctx, 7), hitAt = getRegU32(ctx, 8);
                if (!g_call.far && hitAt == g_call.context + kTarget && !g_call.lineDone)
                {
                    // The line test from the pivot: along the eye's line through the crosshair instead.
                    V3 from, dir;
                    if (lineStart(g_call.pivot, g_call.aim, from, dir))
                    {
                        writeAny(rdram, fromAt, from);
                        writeAny(rdram, dirAt, dir);
                        g_call.lineDone = true;
                    }
                }
                else if (g_call.far && hitAt != g_call.context + kTarget && !g_call.farDone)
                {
                    // The body's range check of the in-between call: the point first put on the line, then the check aimed
                    // at it from the body (as the game aims it at its own point).
                    if (placeFar(rdram))
                    {
                        const V3 origin = readAny(rdram, fromAt);
                        V3 dir = readV3(rdram, g_call.context + kTarget) - origin;
                        const float n = length(dir);
                        if (n > 1e-4f && finite(dir))
                            writeAny(rdram, dirAt, dir * (1.0f / n));
                    }
                }
            }
            g_collision(rdram, ctx, runtime);
        }

        void calculateCrosshair(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t shader = getRegU32(ctx, 4), context = getRegU32(ctx, 5);
            const bool active = wanted(rdram, context);
            if (active)
                measureOffset(rdram);
            g_call = Call{};
            g_call.in = true;
            g_call.active = active;
            g_call.context = context;
            const uint8_t *flip = getConstMemPtr(rdram, shader + kFlip);
            g_call.far = flip && *flip == 0u;
            g_crosshair(rdram, ctx, runtime);
            if (active && g_call.far && !g_call.farDone)
                placeFar(rdram); // (no body check this call)
            g_call.in = false;
            if (g_call.havePivot)
            {
                g_lastPivot.at = g_call.pivot;
                g_lastPivot.when = std::chrono::steady_clock::now();
                g_lastPivot.valid = true;
            }
            if (!active)
            {
                g_offset.prevValid = false;
                return;
            }
            if (g_call.havePivot && g_call.haveAim)
            {
                g_offset.prevPivot = g_call.pivot;
                g_offset.prevAim = g_call.aim;
                g_offset.prevPitch = fword(rdram, context + 0x118u); // (the camera control's pitch)
                g_offset.prevValid = true;
            }
            else
                g_offset.prevValid = false;
            if (logOn())
            {
                static auto last = std::chrono::steady_clock::now();
                const auto now = std::chrono::steady_clock::now();
                if (now - last > std::chrono::milliseconds(500))
                {
                    last = now;
                    const V3 t = readV3(rdram, context + kTarget);
                    std::fprintf(stderr, "[aim] eye from the pivot: ahead %.2f right %.2f up %.2f (%s), the crosshair %.3f up (%s); the line %s; point %.1f %.1f %.1f at %.1f\n",
                                 g_offset.ahead, g_offset.right, g_offset.up, g_offset.valid ? "known" : "not known", g_offset.share,
                                 g_offset.shareKnown ? "learnt" : "not yet",
                                 g_call.lineDone ? "from the eye's line" : g_call.farDone ? "in-between on the eye's line" : "the game's", t.x, t.y, t.z,
                                 fword(rdram, context + kRange));
                }
            }
        }
    }

    bool cameraPivot(float point[3])
    {
        if (!g_lastPivot.valid || std::chrono::steady_clock::now() - g_lastPivot.when > std::chrono::milliseconds(250))
            return false;
        point[0] = g_lastPivot.at.x;
        point[1] = g_lastPivot.at.y;
        point[2] = g_lastPivot.at.z;
        return true;
    }

    void install(PS2Runtime &runtime)
    {
        if (const char *v = std::getenv("TF_STATIC_AIM"); v && *v)
            g_mode.store(std::atoi(v));
        g_crosshair = runtime.lookupFunction(kCalculateCrosshair);
        g_collision = runtime.lookupFunction(kCollisionPoint);
        g_aimDirection = runtime.lookupFunction(kAimDirection);
        g_pivot = runtime.lookupFunction(kPivotPoint);
        if (!g_crosshair || !g_collision || !g_aimDirection || !g_pivot)
        {
            std::fprintf(stderr, "[aim] the crosshair's functions not found: the game's aim kept\n");
            return;
        }
        runtime.registerFunction(kCalculateCrosshair, calculateCrosshair);
        runtime.registerFunction(kCollisionPoint, collisionPoint);
        runtime.registerFunction(kAimDirection, aimDirection);
        runtime.registerFunction(kPivotPoint, pivotPoint);
    }

    void addSettings()
    {
        ps2_settings::Setting s;
        s.key = "TF_STATIC_AIM";
        s.section = "Mouse";
        s.label = "Still crosshair";
        s.help = "The crosshair keeps one place on the screen, near or far, and shots go where it points. With the mouse: while "
                 "the mouse turns the view (a controller keeps the game's aim). Off: the game's aim.";
        s.choices = {{"1", "With the mouse"}, {"2", "Always"}, {"0", "Off"}};
        s.defaultValue = "1";
        s.apply = [](const std::string &value)
        {
            g_mode.store(std::atoi(value.c_str()));
            return true;
        };
        ps2_settings::add(s);
    }
}
