// The HQ at the PC port's frame rate (hq_rate.h; TODO 56).
//
// The HQ ran at the console's 30 frames (with in-between pictures, 60 pictures) because a raised
// rate fast-forwarded it. Its update hands nearly everything the frame's seconds; what moves by the
// frame instead is made for frames of 1/R s below (the ring's turn, the Mini-Con power rings' text
// cycle, the globe's scroll, the warp screen's shimmer, the warp gate names' scroll (TODO 90), every
// taMovingObjectState - the rings, bars, panels the screens slide, spin and grow - and the camera's ease between
// stations). overrides.cpp then runs
// the HQ at the frame-rate setting like a level, adapting down the same ladder.
//
// Testing aids: TF_HQ_STEPS=0 leaves the steps as the console's (the HQ fast-forwards above 30 -
// the A/B); TF_HQ_DUMP=<seconds>[,<seconds>...] - when the HQ's own clock (taUIPreGame+0x44, the
// sum of the frames' seconds since the HQ began) passes each, the scene nodes' matrices and the UI
// effect singletons' memory go to the log ([hqdump] lines): two runs at different rates compared at
// the same HQ seconds show what moves by the frame instead of by the time; TF_HQ_NODE_INSPECT (below).

#include "hq_rate.h"

#include "ultrawide.h"

#include "decomp.h"
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_memory.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include "game_version.h" // (TF_GA: the code addresses that differ in the PAL release)

namespace hq_rate
{
    namespace
    {
        constexpr uint32_t kPreGame = 0x006CF688u; // the taUIPreGame (the HQ's executor)
        constexpr uint32_t kPreGameClock = 0x44u;  // its seconds since it began (+= dt a frame)

        uint32_t word(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v = 0u;
            if (const uint8_t *p = getConstMemPtr(rdram, a & ~3u))
                std::memcpy(&v, p, sizeof(v));
            return v;
        }

        float real(const uint8_t *rdram, uint32_t a)
        {
            const uint32_t w = word(rdram, a);
            float f;
            std::memcpy(&f, &w, sizeof(f));
            return f;
        }

        bool inRam(uint32_t a) { return a >= 0x00100000u && a < PS2_RAM_SIZE; }

        std::string cString(const uint8_t *rdram, uint32_t a)
        {
            std::string s;
            if (!inRam(a))
                return "-";
            for (uint32_t i = 0; i < 48u; ++i)
            {
                const uint8_t *p = getConstMemPtr(rdram, a + i);
                if (!p || *p == 0)
                    break;
                if (*p < 0x20 || *p > 0x7E)
                    return "-";
                s += static_cast<char>(*p);
            }
            return s.empty() ? "-" : s;
        }

        // The node classes (vlSceneNode and the ones made from it the HQ uses) and the UI objects
        // whose memory is dumped whole.
        struct Kind
        {
            uint32_t vtable;
            const char *name;
        };
        constexpr Kind kNodes[] = {
            {0x006B2D90u, "vlSceneNode"}, {0x006B2890u, "vlMeshSurface"}, {0x006B3600u, "vlMeshVertexBlend"},
            {0x006B3590u, "vlMeshVertexTween"}, {0x006B2E40u, "vlSceneNodeInstance"},
            {0x006B2CD0u, "vlSceneNodeMotionInterpolator"}, {0x006AF490u, "anAnimationActor"},
            {0x006BB1C0u, "taSwitcherAnimationActor"}, {0x006BB0E0u, "taLODAnimationActor"},
            {0x006B2F10u, "vlSceneStateSwitcher"}, {0x006B26C0u, "vlCamera"}, {0x006BE770u, "taUICamera"},
            {0x006BE720u, "taUICameraFly"}, {0x006BE6D0u, "taUICameraInterpolate"}, {0x006BE690u, "taUICameraWayPoint"},
        };
        constexpr Kind kObjects[] = {
            {0x006BD8F0u, "taScreenPrGRing"}, {0x006BD880u, "taScreenPrGRingCharacterSelect"},
            {0x006C4410u, "taScreenPrGRingLevelSelect"}, {0x006C3ED0u, "taScreenPrGRingMiniconButtonSelect"},
            {0x006C3E40u, "taScreenPrGRingMiniconSelect"}, {0x006C4360u, "taScreenPrGWarpGate"},
            {0x006BD810u, "taScreenPrGHQComm"}, {0x006BB560u, "taUICameraHandler"}, {0x006BB270u, "taUIPowerRingTextCycleBehaviour"},
            {0x006C4D00u, "taUITextureScrollController"}, {0x006C4CF0u, "taUIGlobeTextureScrollController"},
            {0x006C4CE0u, "taUIWarpTunnelOneController"}, {0x006C4CD0u, "taUIWarpTunnelTwoController"},
            {0x006C4C80u, "taUIWarpTunnelThreeController"}, {0x006BB2D0u, "taUIWarpGateEffectRenderBehaviour"},
            {0x006C4C90u, "taUITextureScrollRenderBehaviour"}, {0x006C43F0u, "taUIFMVScreenBehaviour"},
            {0x006BB360u, "taUIModel"}, {0x006BB320u, "taUIModelAnimating"}, {0x006BB2F0u, "taUIModelEffects"},
            {0x006B71C0u, "taUISystemBase"}, {0x006B7160u, "taUISystem"},
        };
        // The UI effect singletons (taUIEffectSystem's, 0x4077D0): pointer, size dumped.
        struct Singleton
        {
            uint32_t pointer;
            uint32_t size;
            const char *name;
        };
        constexpr Singleton kSingletons[] = {
            {0x006CF918u, 0x3400u, "taUIRingEffects"},          {0x006CF91Cu, 0x800u, "effects91C"},
            {0x006CF920u, 0x2000u, "taUIMiniconEffects"},       {0x006CF924u, 0x800u, "taUIWarpGateEffects"},
            {0x006CF928u, 0x1000u, "taUIOptionsScreensEffects"}, {0x006CF92Cu, 0x1940u, "taUICharacterScreensEffects"},
            {kPreGame, 0x200u, "taUIPreGame"},
        };

        void dumpWords(const uint8_t *rdram, float t, const char *what, uint32_t at, uint32_t size)
        {
            std::string line;
            char buf[16];
            for (uint32_t o = 0; o < size; o += 4u)
            {
                std::snprintf(buf, sizeof(buf), "%08x ", word(rdram, at + o));
                line += buf;
            }
            std::fprintf(stderr, "[hqdump] %.3f obj %s %08x %s\n", t, what, at, line.c_str());
        }

        uint64_t g_dumpTick = 0u; // (the VBlank the dump is made at: its place among the run's shots)

        void dump(const uint8_t *rdram, float t)
        {
            unsigned nodes = 0u, objects = 0u;
            for (uint32_t a = 0x00100000u; a < PS2_RAM_SIZE; a += 4u)
            {
                const uint32_t v = word(rdram, a);
                if (v < 0x006AF000u || v > 0x006CC000u)
                    continue;
                for (const Kind &k : kNodes)
                {
                    if (v != k.vtable)
                        continue;
                    const uint32_t xform = word(rdram, a + 0x14u);
                    if (!inRam(xform))
                        break;
                    std::fprintf(stderr, "[hqdump] %.3f node %08x %s %s", t, a, k.name, cString(rdram, word(rdram, a + 0x40u)).c_str());
                    for (uint32_t i = 0; i < 12u; ++i)
                        std::fprintf(stderr, " %.5g", real(rdram, xform + 4u * i));
                    std::fprintf(stderr, "\n");
                    ++nodes;
                    break;
                }
                for (const Kind &k : kObjects)
                {
                    if (v != k.vtable)
                        continue;
                    dumpWords(rdram, t, k.name, a, 0x140u);
                    ++objects;
                    break;
                }
            }
            dumpWords(rdram, t, "textCycle", 0x006CF7E4u, 4u); // (gp-0x390C: the power rings' text cycle, 0x3A8520)
            // TF_HQ_NODE_INSPECT=<name>[,<name>...]: those nodes' first words, and what each pointer
            // among them points to (its first word: a vtable) - their behaviours.
            if (const char *inspect = std::getenv("TF_HQ_NODE_INSPECT"))
                for (uint32_t a = 0x00100000u; a < PS2_RAM_SIZE; a += 4u)
                {
                    const uint32_t v = word(rdram, a);
                    if (v != 0x006B2D90u && v != 0x006B2890u)
                        continue;
                    const std::string name = cString(rdram, word(rdram, a + 0x40u));
                    if (name == "-" || std::strstr(inspect, name.c_str()) == nullptr)
                        continue;
                    std::string line;
                    char buf[48];
                    for (uint32_t o = 0; o < 0x80u; o += 4u)
                    {
                        const uint32_t w = word(rdram, a + o);
                        std::snprintf(buf, sizeof(buf), " +%02x:%08x", o, w);
                        line += buf;
                        if (inRam(w))
                        {
                            const uint32_t pv = word(rdram, w), pv2 = word(rdram, w + 4u);
                            std::snprintf(buf, sizeof(buf), "[%08x %08x]", pv, pv2);
                            line += buf;
                        }
                    }
                    std::fprintf(stderr, "[hqdump] %.3f inspect %08x %s%s\n", t, a, name.c_str(), line.c_str());
                }
            for (const Singleton &s : kSingletons)
            {
                const uint32_t at = s.pointer == kPreGame ? word(rdram, kPreGame) : word(rdram, s.pointer);
                if (inRam(at))
                    dumpWords(rdram, t, s.name, at, s.size);
            }
            std::fprintf(stderr, "[hqdump] %.3f done: %u nodes, %u objects (VBlank %llu)\n", t, nodes, objects, static_cast<unsigned long long>(g_dumpTick));
        }

        std::vector<float> g_dumpAt;
        size_t g_nextDump = 0u;
        uint32_t g_dumpedFor = 0u; // (the HQ the dumps were made in)

        void setReal(uint8_t *rdram, uint32_t a, float f)
        {
            if (uint8_t *p = getMemPtr(rdram, a))
                std::memcpy(p, &f, sizeof(f));
        }

        // --- What the HQ does by the frame ---------------------------------------------------
        // The HQ's update (taUIPreGame, 0x333630) passes the frame's seconds to its systems, but
        // these few pieces step by a fixed amount every frame, made for the console's 30 (found
        // by TF_HQ_DUMP runs at 30 and 120 compared, then in the code). At R frames a second each
        // step is made for a frame of 1/R s: an amount a frame x 30/R, a share of the way a frame
        // 1 - (1 - share)^(30/R) - as far, as much, as often in a 30th of a second as at 30.
        // k = 30/R in the HQ at the PC rate R, 1 elsewhere (the console's values).
        float g_k = 1.0f;

        // Constants of .sdata the per-frame steps read, rewritten for the HQ's rate while it runs
        // and put back to the console's values everywhere else (a save state made in the HQ too).
        struct Step
        {
            uint32_t address;
            float atThirty; // the console's value (the executable's)
            const char *what;
        };
        constexpr Step kSteps[] = {
            // taUIRingEffects::Update (0x444AC0): its time += 0.00033 a frame, the turn of a ring
            // node about z (gp-0x7A28; read nowhere else).
            {0x006CB6C8u, 0.00033f, "the HQ ring's turn"},
            // 0x3A8520 (from 0x444AC0): the power rings' text cycle += 0.002 a frame - the Mini-Con
            // power rings' (A_Power_1..4) texture offset, taUIPowerRingTextCycleBehaviour 0x3A8580: 5 x
            // it; NOT the main ring's section name, whose band scrolls by the time (gp-0x7A14;
            // also read by reCarStaticData::GetMaxSteerAngle 0x326A70 - the cars, levels only).
            {0x006CB6DCu, 0.002f, "the power rings' text cycle"},
            // taUIGlobeTextureScrollController (0x4FC2C0, the level select's globe): its texture's v
            // -= 0.017 a frame (gp-0x7AA0; read nowhere else).
            {0x006CB650u, 0.017f, "the globe's scroll"},
        };
        // taUITextureScrollController (0x4FC390, the warp gate screens' effect): its texture
        // offset jumps by (0.975, 0.634) a frame, wrapped - a shimmer, not a drift: at R the jump
        // is made in one frame of R/30 (its words 0 in the others), as often as at 30.
        constexpr uint32_t kShimmerU = 0x006CB654u, kShimmerV = 0x006CB658u;
        constexpr float kShimmerUAtThirty = 0.9753086566925049f, kShimmerVAtThirty = 0.6341296434402466f;
        double g_shimmerPhase = 0.0;

        float share(float atThirty, float k)
        {
            if (k == 1.0f || !(atThirty > 0.0f && atThirty < 1.0f))
                return atThirty;
            return 1.0f - std::pow(1.0f - atThirty, k);
        }

        // taMovingObjectState::Update (0x4462E0) - every ring, bar, globe and panel the HQ's
        // screens move (the UI effect singletons' objects, 0xE0 bytes each) - moves by shares
        // and steps that are the object's own fields:
        //   +0xA4 the position's share of the way to its target a frame (slides, drops),
        //   +0xA8 the scale's share a frame (grow / shrink),
        //   +0xAC with +0xB9 set (spinning, after the +0xCC delay) the turn a frame; otherwise
        //         the turn's share of the way to its target a frame (the main ring turning to a
        //         face, SpinMainMenuRing: 0.13).
        // Before each call the object's fields are written for the rate from the values the game
        // gave them (kept here; a field that no longer holds what was written was set by the game
        // again), and put back when the call returns - or left until the next call when it stops
        // inside a callee (it goes on later outside this wrapper).
        constexpr uint32_t kMovingObjectUpdate = TF_GA(0x004462E0u);
        constexpr uint32_t kFieldOffsets[3] = {0xA4u, 0xA8u, 0xACu};
        constexpr uint32_t kSpinning = 0xB9u;
        struct Fields
        {
            float game[3];
            float written[3];
        };
        std::unordered_map<uint32_t, Fields> g_objects;

        float field(const uint8_t *rdram, uint32_t a) { return real(rdram, a); }

        void movingObjectUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            static PS2Runtime::RecompiledFunction original = decomp::original(kMovingObjectUpdate);
            const uint32_t self = getRegU32(ctx, 4);
            const float k = g_k;
            if (k == 1.0f && g_objects.find(self) == g_objects.end())
            {
                original(rdram, ctx, runtime);
                return;
            }
            const auto [entry, inserted] = g_objects.try_emplace(self);
            Fields &f = entry->second;
            if (inserted)
                for (int i = 0; i < 3; ++i)
                    f.written[i] = std::nanf("");
            const uint8_t *spin = getConstMemPtr(rdram, self + kSpinning);
            const bool spinning = spin && *spin != 0u;
            for (int i = 0; i < 3; ++i)
            {
                const float now = field(rdram, self + kFieldOffsets[i]);
                if (!(now == f.written[i])) // (the game's own value, set again)
                    f.game[i] = now;
                const float v = i == 2 && spinning ? f.game[i] * k : share(f.game[i], k);
                setReal(rdram, self + kFieldOffsets[i], v);
                f.written[i] = v;
            }
            const uint32_t returnTo = getRegU32(ctx, 31);
            original(rdram, ctx, runtime);
            if (ctx->pc == returnTo)
            {
                // (returned: the game's values back, as any other code reading them expects)
                for (int i = 0; i < 3; ++i)
                    if (field(rdram, self + kFieldOffsets[i]) == f.written[i])
                        setReal(rdram, self + kFieldOffsets[i], f.game[i]);
                if (k == 1.0f)
                    g_objects.erase(self);
                else
                    for (int i = 0; i < 3; ++i)
                        f.written[i] = f.game[i]; // (what the next call finds unless the game sets it)
            }
        }

        const decomp::Registration g_movingObjectRegistration{decomp::Function{
            "hq_rate::movingObjectUpdate", "hq_rate", kMovingObjectUpdate, &movingObjectUpdate, nullptr, 0u, false,
            decomp::Result::None}};

        // taUICameraInterpolate::doUpdate (0x422890), the HQ's camera between its stations: its
        // progress p moves by dt x (1 - p) x speed (+0x80) a frame - an exponential ease whose
        // shape depends on the step (at 30 a frame keeps 1 - speed/29.97 of what is left; frames of
        // 1/R s, (1 - speed/(29.97 R/30))^(R/30) - slower, and much slower for a fast camera).
        // The frame's dt is made the one that keeps what 30/R of a console frame keeps: the same
        // ease, the same time, at every rate.
        constexpr uint32_t kCameraUpdate = TF_GA(0x00422890u), kCameraSpeed = 0x80u;
        void cameraUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            static PS2Runtime::RecompiledFunction original = decomp::original(kCameraUpdate);
            const float k = g_k;
            if (k != 1.0f)
            {
                const float speed = real(rdram, getRegU32(ctx, 4) + kCameraSpeed);
                const float console = 1.0f / 29.97f; // (the console's frame, 0x32E910 at 30)
                const float kept = 1.0f - speed * console;
                if (speed > 0.0f && kept > 0.0f && kept < 1.0f)
                    ctx->f[12] = (1.0f - std::pow(kept, k)) / speed;
            }
            original(rdram, ctx, runtime);
        }

        const decomp::Registration g_cameraRegistration{decomp::Function{
            "hq_rate::cameraUpdate", "hq_rate", kCameraUpdate, &cameraUpdate, nullptr, 0u, false, decomp::Result::None}};

        // The warp gate screen's (taScreenPrGWarpGate, vtable 0x6C4360) update, its slot 18 (0x4EEA00): the ring of
        // warp gate names above the gate scrolls by its +0x70 -= 4.0 every frame (an immediate, in both of its
        // branches; its draw, 0x4EE610, wraps it) - at 120 frames it went round four times as fast (TODO 90, found by
        // TF_HQ_DUMP runs at 30 and 60 on that screen: +0x70 -240 in 2 s at 30, -480 at 60). The step is made 4 x 30/R
        // for frames of 1/R s: once a frame (frame(), below) the scroll is moved back by 4 x (1 - 30/R) when the game
        // moved it since (the update's call often stops inside its callee 0x1A0C20 and goes on outside a wrapper, so
        // the wrapper only notes which object it is); left alone at 0 (the fade-out's end puts it back to 0).
        constexpr uint32_t kWarpGateUpdate = TF_GA(0x004EEA00u), kWarpGateScroll = 0x70u;
        uint32_t g_warpGate = 0u;   // (the screen object, noted by the update's wrapper)
        float g_warpScroll = 0.0f;  // (its scroll as this left it last)
        void warpGateUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            static PS2Runtime::RecompiledFunction original = decomp::original(kWarpGateUpdate);
            g_warpGate = getRegU32(ctx, 4);
            original(rdram, ctx, runtime);
        }

        void warpGateScroll(uint8_t *rdram, float k)
        {
            // The screen can be destroyed before the executor leaves HQ.
            // Its storage may already hold a level object on the next frame.
            if (!inRam(g_warpGate) || !inRam(g_warpGate + kWarpGateScroll + 3u) ||
                word(rdram, g_warpGate) != 0x006C4360u)
            {
                g_warpGate = 0u;
                g_warpScroll = 0.0f;
                return;
            }
            float now = real(rdram, g_warpGate + kWarpGateScroll);
            if (k != 1.0f && now != g_warpScroll && now != 0.0f)
            {
                now += 4.0f * (1.0f - k);
                setReal(rdram, g_warpGate + kWarpGateScroll, now);
            }
            g_warpScroll = now;
        }

        const decomp::Registration g_warpGateRegistration{decomp::Function{
            "hq_rate::warpGateUpdate", "hq_rate", kWarpGateUpdate, &warpGateUpdate, nullptr, 0u, false, decomp::Result::None}};

        void applySteps(uint8_t *rdram, bool inHq, int rate)
        {
            // TF_HQ_STEPS=0 (testing aid): the steps left as the console's at every rate - the HQ
            // fast-forwards above 30, as it did before (an A/B for the fixes).
            static const bool off = []()
            {
                const char *env = std::getenv("TF_HQ_STEPS");
                return env && *env == '0';
            }();
            const float k = inHq && rate > 0 && !off ? 30.0f / static_cast<float>(rate) : 1.0f;
            if (k != g_k)
            {
                std::fprintf(stderr, "[hq_rate] the HQ's per-frame steps for %d frames a second (x%.4g)\n", k == 1.0f ? 30 : rate, k);
                g_shimmerPhase = 0.0;
            }
            g_k = k;
            for (const Step &s : kSteps)
                setReal(rdram, s.address, s.atThirty * k);
            bool jump = true;
            if (k != 1.0f)
            {
                g_shimmerPhase += k;
                jump = g_shimmerPhase >= 1.0 - 1e-6;
                if (jump)
                    g_shimmerPhase -= 1.0;
            }
            setReal(rdram, kShimmerU, jump ? kShimmerUAtThirty : 0.0f);
            setReal(rdram, kShimmerV, jump ? kShimmerVAtThirty : 0.0f);
        }
    }

    void frame(uint8_t *rdram, PS2Runtime &runtime, bool inHq, int rate)
    {
        applySteps(rdram, inHq, rate);
        if (inHq)
            warpGateScroll(rdram, g_k);
        else
        {
            g_warpGate = 0u;
            g_warpScroll = 0.0f;
            g_objects.clear();
        }
        ultrawide::setInHq(inHq); // (its scene at the game's own field of view: TODO 52)
        static const bool dumpOn = []()
        {
            const char *env = std::getenv("TF_HQ_DUMP");
            if (!env || !*env)
                return false;
            for (const char *p = env; *p;)
            {
                char *end = nullptr;
                const float v = std::strtof(p, &end);
                if (end == p)
                    break;
                g_dumpAt.push_back(v);
                p = *end == ',' ? end + 1 : end;
            }
            return !g_dumpAt.empty();
        }();
        if (!dumpOn || !inHq)
            return;
        const uint32_t preGame = word(rdram, kPreGame);
        if (!inRam(preGame))
            return;
        if (preGame != g_dumpedFor)
        {
            g_dumpedFor = preGame;
            g_nextDump = 0u;
        }
        const float t = real(rdram, preGame + kPreGameClock);
        g_dumpTick = runtime.eeScheduler().currentVSyncTick();
        while (g_nextDump < g_dumpAt.size() && t >= g_dumpAt[g_nextDump])
        {
            dump(rdram, t);
            ++g_nextDump;
        }
    }
}
