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
// Left to the game: a cutscene script's camera and any camera not looking along the aim (TODO 331), R3's aiming view
// and first-person aiming (state 9 - its camera already looks at the point), first person's free look (the eye looks
// along the aim: the crosshair already keeps its place), a turret (state 0x13, the muzzles'), the jet bike (its own
// aim, port_ext.h), photo mode, and a frame whose eye is not known or lies far from the pivot (a camera cut).
//
// (TODO 355) On while the Crosshair row says Fixed (setFixed; the row is the mods build's - without it, and in the public
// build, nothing here acts: the game's own aim). With the mouse and with a pad alike. TF_AIM_LOG=1: the offset and the line,
// twice a second. (TF_STATIC_AIM, the old "Still crosshair" row's variable - with the mouse / always / off - is no longer
// read: one left in a settings file must not fight the row.)
// (TODO 322) While it holds, the crosshair is pinned to one screen point (pinned(): the line's fixed height in third person,
// the centre in first person and the aiming view) - photo_mode places the game's model on the camera's ray through it each
// frame and small_crosshair draws there.
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
#include "runtime/ps2_host_input.h"
#include "runtime/ps2_memory.h"

#include <algorithm>
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

        // (TODO 322) The crosshair pinned to one screen point this frame: its share of half the view's height above the
        // centre (0 in first person and the aiming view), and when that was decided (stale after a frame or two: the car,
        // a cutscene, a menu - no CalculateCrosshair then).
        std::atomic<float> g_pinShare{0.0f};
        std::atomic<bool> g_pinOn{false};
        std::atomic<int64_t> g_pinAtNs{0};

        // (TODO 322) The still crosshair's height: a share of half the view's height above the centre - one fixed value, not
        // learnt (the learnt value eased and crept; it always came out 0.179-0.180, where the game's own far point shows with
        // the camera level). TF_STATIC_AIM_SHARE=<f>: another.
        float stillShare()
        {
            static const float share = [] {
                const char *e = std::getenv("TF_STATIC_AIM_SHARE");
                const float v = e && *e ? std::strtof(e, nullptr) : 0.0f; // (TODO 343: the middle; 0.18 = where the game's own showed)
                return std::isfinite(v) && std::fabs(v) < 0.6f ? v : 0.0f;
            }();
            return share;
        }

        // (TODO 322) The still crosshair held from the start with the mouse and handed the aim back to the game when a pad
        // was used; before that it followed the mouse turning the view, and an aim-stick value it took for the pad's handed
        // it back mid-turn: the crosshair jumped between the two places (~2% of the height).
        // (TODO 343, the user's decision) It became the Apex mod's alone: without the mod the original game's crosshair.
        // (TODO 355, the user's test) Now a choice of its own - the Crosshair row, Original / Fixed (setFixed: the row's
        // word each game frame; the Apex mod only picks what the row starts at). Fixed holds for the mouse and for a pad:
        // the crosshair is still whatever turns the view. Original: nothing here acts.
        std::atomic<bool> g_fixed{false};
        bool stillOn() { return g_fixed.load(std::memory_order_relaxed); }

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
            float leastAhead = 2.0f; // (TF_AIM_LOG: the camera's least ahead along the aim since the last line)
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
            // (TODO 331) The player's camera looks along the aim (the aim follows it); a scripted one - a cutscene's, the
            // transformation's, any camera not behind the player looking his way - does not: its ray is not the
            // crosshair's, and the aim point put on it would turn the torso toward wherever that camera looks. Left to
            // the game (60 degrees and more apart: no turn of a frame goes that far).
            const bool foreign = g_offset.camAhead.x < 0.5f;
            g_offset.leastAhead = std::min(g_offset.leastAhead, g_offset.camAhead.x);
            if (static bool was = false; foreign != was)
            {
                was = foreign;
                if (logOn())
                    std::fprintf(stderr, "[aim] the camera %s (its ahead %.2f along the aim)\n",
                                 foreign ? "does not look along the aim: the game's aim" : "looks along the aim again", g_offset.camAhead.x);
            }
            if (foreign)
            {
                g_offset.valid = g_offset.camValid = false;
                return;
            }
            // (the crosshair's height: one fixed share of half the view's height above the centre - stillShare, TODO 322)
            g_offset.share = stillShare();
            g_offset.shareKnown = true;
        }

        bool wanted(const uint8_t *rdram, uint32_t context)
        {
            if (!stillOn() || photo_mode::active() || context == 0u)
                return false;
            // (TODO 331) A cutscene script plays - a level's opening, an HQ call, a Mini-Con or Data-Con discovery: the view is
            // its camera's, and a line along that camera's ray pulled the aim point (and the torso that turns to it) round with
            // the camera - the Autobot spun, his back always to it. The game's own aim then, as on the PS2.
            const bool cutscene = tf::port::cutscenePlaying(rdram);
            if (static bool was = false; cutscene != was)
            {
                was = cutscene;
                if (logOn())
                    std::fprintf(stderr, "[aim] a cutscene %s: %s\n", cutscene ? "plays" : "is over", cutscene ? "the game's aim" : "the still aim again");
            }
            if (cutscene)
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

        // (TODO 322) This frame's pin: mouse and keyboard play on foot - the still line's height in third person, the centre in
        // first person and the aiming view (state 9: its camera looks at the point). Not on a turret, the jet bike, in photo
        // mode or a cutscene (the game's aim there).
        void notePin(const uint8_t *rdram, uint32_t context, bool lineActive)
        {
            bool on = false;
            float share = 0.0f;
            if (stillOn() && !photo_mode::active() && context != 0u && !camera_mode::inVehicle() &&
                !tf::port::cutscenePlaying(rdram)) // (TODO 331: a cutscene's camera is not the player's)
            {
                const uint32_t actor = word(rdram, context);
                const uint32_t state = actor ? word(rdram, actor + kState) : 0u;
                float bike[3];
                if (state != 0x13u && !ext::aimPoint(rdram, bike))
                {
                    if (camera_mode::firstPerson() || state == 9u)
                        on = true; // (share 0: the eye looks along the aim)
                    else if (lineActive)
                    {
                        on = true;
                        share = stillShare();
                    }
                }
            }
            g_pinShare.store(share, std::memory_order_relaxed);
            g_pinOn.store(on, std::memory_order_relaxed);
            g_pinAtNs.store(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(),
                            std::memory_order_relaxed);
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
            notePin(rdram, context, active);
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
                    std::fprintf(stderr, "[aim] eye from the pivot: ahead %.2f right %.2f up %.2f (%s), the crosshair %.3f up (%s); the line %s; point %.1f %.1f %.1f at %.1f; the camera at least %.3f along the aim\n",
                                 g_offset.ahead, g_offset.right, g_offset.up, g_offset.valid ? "known" : "not known", g_offset.share,
                                 g_offset.shareKnown ? "learnt" : "not yet",
                                 g_call.lineDone ? "from the eye's line" : g_call.farDone ? "in-between on the eye's line" : "the game's", t.x, t.y, t.z,
                                 fword(rdram, context + kRange), g_offset.leastAhead);
                    g_offset.leastAhead = 2.0f;
                }
            }
        }
    }

    bool pinned(float &share)
    {
        if (!g_pinOn.load(std::memory_order_relaxed))
            return false;
        const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now - g_pinAtNs.load(std::memory_order_relaxed) > 150000000) // (no CalculateCrosshair lately: the car, a menu, a cut)
            return false;
        share = g_pinShare.load(std::memory_order_relaxed);
        return true;
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

    void setFixed(bool on) { g_fixed.store(on, std::memory_order_relaxed); }
    bool fixed() { return g_fixed.load(std::memory_order_relaxed); }
}
