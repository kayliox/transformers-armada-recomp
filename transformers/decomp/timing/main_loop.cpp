// Frame timing: the main loop that runs a mode of the game.

#include "timing.h"

#include "port.h"
#include "runtime/ee_scheduler.h"

#include <cstdio>
#include "game_version.h" // (TF_GA: the code addresses that differ in the PAL release)

namespace tf::timing
{
    namespace
    {
        // The original's places for its locals: callee-saved registers, which the calls leave as
        // they found them, so the loop can stop inside a call and go on at its return address.
        constexpr int kV1 = 3, kA0 = 4, kS0 = 16, kS1 = 17, kT9 = 25, kSp = 29, kRa = 31;
        constexpr int kFrameEnd = kS0;     // the VBlank count the frame is to end at
        constexpr int kTheMode = kS1;      // the mode being run
        constexpr int kVBlanksAFrame = 20; // f20
        constexpr int kWaitUntil = 0;      // f0: the frame's end as a float, during the wait

        // The original's addresses where the loop is entered again: the return address of each
        // call and the targets of the loops' back edges.
        constexpr uint32_t kEntry = 0x295310u;
        constexpr uint32_t kAfterTvRate = 0x29532Cu;
        constexpr uint32_t kAfterTargetRate = 0x295334u;
        constexpr uint32_t kAfterTargetRateAgain = 0x295350u;
        constexpr uint32_t kAfterSetTarget = 0x29535Cu;
        constexpr uint32_t kAfterLoopStartsA = 0x295370u;
        constexpr uint32_t kAfterLoopStartsB = 0x295378u;
        constexpr uint32_t kFrame = 0x295380u;
        constexpr uint32_t kAfterUpdate = 0x2953A4u;
        constexpr uint32_t kAfterDraw = 0x2953B4u;
        constexpr uint32_t kWait = 0x2953CCu;

        Mode TheMode(decomp::Guest &g)
        {
            return Mode::at(g.gpr(kTheMode));
        }
    }

    // 0x295310: runs a mode of the game - the intro, the menus, a level - until it sets its
    // quit flag. Once a frame: the mode's update and its drawing, then (unless betwixtMode,
    // where each frame's two fields pace themselves at the frame end) a wait until the frame's
    // VBlanks have passed: the TV's field rate over the target frame rate, worked out once when
    // the loop starts - 2 on a 60 Hz TV at 30 frames a second, 1 at 60.
    void Mode_Run(decomp::Guest &g)
    {
        R5900Context *ctx = g.ctx();
        if (g.pc() == kWait)
        {
            // PC: entered again in the wait after a yield. At the PC port's rate a frame lasts
            // one VBlank (a loop started at 30 - in a save state made at 30 - holds 2), and the EE
            // clock jumps to the next event instead of the loop polling the counter until it comes.
            if (port::levelRate() != 0)
                ctx->f[kVBlanksAFrame] = 1.0f;
            const uint32_t before = g.cop0Count();
            g.runtime()->eeScheduler().skipIdleCycles();
            port::idleSkipped(g.cop0Count() - before);
        }
        switch (g.pc())
        {
        case kEntry:
        {
            // A frame of 0x40 bytes for ra, s1, s0 and f20.
            const uint32_t sp = g.gpr(kSp) - 0x40u;
            g.setGpr(kSp, sp);
            g.write<uint64_t>(sp + 0x30u, g.gpr64(kRa));
            g.storeQuad(sp + 0x20u, kS1);
            g.storeQuad(sp + 0x10u, kS0);
            g.copyGpr(kTheMode, kA0);
            g.write<f32>(sp + 0x00u, ctx->f[kVBlanksAFrame]);
        }
            DECOMP_CALL(g, 0x32E840u, kAfterTvRate) // TvFieldRate
            ctx->f[kVBlanksAFrame] = ctx->f[0];
            DECOMP_CALL(g, 0x32E8B0u, kAfterTargetRate) // TargetFrameRate
            ctx->f[kVBlanksAFrame] = (f32(ctx->f[kVBlanksAFrame]) / f32(ctx->f[0])).v;
            TheMode(g).quit() = 0;
            DECOMP_CALL(g, 0x32E8B0u, kAfterTargetRateAgain) // TargetFrameRate
            ctx->f[12] = ctx->f[0];
            g.setGpr64(kA0, 0u);
            DECOMP_CALL(g, 0x202FF0u, kAfterSetTarget) // SetTargetRate(0, the rate)
            // Whatever listens at gp-0x36EC is told that a mode's loop starts.
            g.setGpr(kS0, loopStartListener);
            g.copyGpr(kA0, kS0);
            if (g.gpr(kS0) != 0u)
            {
                DECOMP_CALL(g, TF_GA(0x4A11D0u), kAfterLoopStartsA)
                g.copyGpr(kA0, kS0);
                DECOMP_CALL(g, TF_GA(0x4A1080u), kAfterLoopStartsB)
            }
            for (;;)
            {
                // The end of a frame (0x2953E8): round again unless the mode quits.
                {
                    const int32_t quit = TheMode(g).quit();
                    g.setGpr(kV1, static_cast<uint32_t>(quit));
                    if (quit != 0)
                        break;
                }
                if (g.backEdge(kFrame))
                    return;
            case kFrame:
            {
                port::frameBegins(g.rdram(), g.runtime()); // (PC: the frame statistics, TF_FPS / TF_HITCH_MS)
                // The frame is to end once its VBlanks have passed from now.
                const int32_t end = (f32(ctx->f[kVBlanksAFrame]) + f32::fromInt(vblankCount)).toInt();
                g.setGpr(kFrameEnd, static_cast<uint32_t>(end));
                ctx->f[0] = f32::fromBits(static_cast<uint32_t>(end)).v; // (cvt.w.s leaves the integer in f0)
                g.setGpr(kT9, TheMode(g).method(Mode::kUpdate));
                g.copyGpr(kA0, kTheMode);
            }
                DECOMP_CALL(g, g.gpr(kT9), kAfterUpdate) // the mode's update
                g.setGpr(kT9, TheMode(g).method(Mode::kDraw));
                g.copyGpr(kA0, kTheMode);
                DECOMP_CALL(g, g.gpr(kT9), kAfterDraw) // the mode's drawing
                g.setGpr(kV1, betwixtMode);
                if (g.gpr(kV1) != 0u)
                    continue;
                // Wait until the frame's end (polling the VBlank counter).
                ctx->f[kWaitUntil] = f32::fromInt(static_cast<int32_t>(g.gpr(kFrameEnd))).v;
                for (;;)
                {
                case kWait:
                {
                    const f32 now = f32::fromInt(vblankCount);
                    ctx->f[1] = now.v;
                    if (!(now < f32(ctx->f[kWaitUntil])))
                        break;
                }
                    if (g.backEdge(kWait))
                        return;
                }
            }
            {
                // Epilogue.
                const uint32_t sp = g.gpr(kSp);
                g.setGpr64(kRa, g.read<uint64_t>(sp + 0x30u));
                ctx->f[kVBlanksAFrame] = g.read<f32>(sp + 0x00u).v;
                g.loadQuad(sp + 0x20u, kS1);
                g.loadQuad(sp + 0x10u, kS0);
                g.setGpr(kSp, sp + 0x40u);
            }
            g.returnToCaller();
            return;
        default:
            std::fprintf(stderr, "[decomp] Mode_Run entered at %08x\n", g.pc());
            return;
        }
    }

    DECOMP_YIELDING(0x295310, Mode_Run, "timing", kAfterTvRate, kAfterTargetRate, kAfterTargetRateAgain,
                    kAfterSetTarget, kAfterLoopStartsA, kAfterLoopStartsB, kFrame, kAfterUpdate, kAfterDraw, kWait);
}
