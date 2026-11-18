// nr_submit.cpp - in the core (nr_core S10): a frame the host records itself (nr_core::submit, NR_CORE_SPEC.md 3), beside the
// recomp's frames (nr::queueFrame: the capture's nr::Frame, drawn by the frame builder).
//
// A submitted frame goes through the core's frame queue (nr_queue.cpp) to its drawing thread - the one thread that records into
// the device in a process, whichever host feeds it; S11: in line behind the frames queueFrame handed over before it (the read
// thread passes it on unread), so a process using both keeps their order - and is drawn here: the device started (open()'s preparing
// waited for), the frame begun at its picture's size (prepareFrame, beginFrame), the core's picture target made at that size,
// the host's recording run (device calls only), the frame ended with its one picture (endFrame: the picture's constants the
// identity - no camera turn, no extra pictures), then presented into the core's window (nr_core::present, S9b) letterboxed to the
// frame's shape. The game's thread waits while two submitted frames wait (nr_queue.cpp queueSubmitted).
// Nothing here runs unless a host calls submit(): the recomp's frames take the queue's own paths unchanged.
#include "nr_submit.h"
#include "nr_backend.h"
#include "nr_core.h"
#include "nr_device.h"
#include "nr_draw.h"
#include "nr_glshare.h" // (d3d::SharedPicture)
#include "nr_queue.h"
#include "nr_types.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

namespace nr
{
    namespace
    {
        constexpr unsigned kWaiting = 2u; // submitted frames waiting to be drawn before submit() waits (NR_CORE_SPEC.md 3)

        std::atomic<bool> g_startFailed{false}; // the device did not start for a submitted frame: submit() refuses
        std::atomic<bool> g_anySubmitted{false};

        // ---- the drawing thread's own ----
        gfx::TargetId g_picture = 0;
        uint32_t g_pictureW = 0, g_pictureH = 0;
        uint64_t g_serial = 0;       // the newest shared picture taken (latestShared)
        unsigned g_presentTries = 0; // frames asked to be presented while no shared picture ever came (said once)

        // The device up for a frame: open()'s preparing waited for (at most 60 s), then started (Backend::start).
        bool deviceUp()
        {
            Backend &be = backend();
            if (be.deviceLost())
                return false;
            const auto t0 = std::chrono::steady_clock::now();
            while (be.startingNow() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(60))
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (be.startingNow())
            {
                std::fprintf(stderr, "[native render] submit: the device is still being made after 60 s - a frame not drawn\n");
                return false;
            }
            if (!be.start())
            {
                if (!g_startFailed.exchange(true))
                    std::fprintf(stderr, "[native render] submit: the device did not start - no frames drawn\n");
                return false;
            }
            return !be.deviceLost();
        }

        void releaseAll()
        {
            if (g_picture)
                gfx::device().releaseTarget(g_picture);
            g_picture = 0;
            g_pictureW = g_pictureH = 0;
            nr_core::releasePresent();
        }

        // The picture into the core's window: the newest shared picture (with waitForGpu this frame's; otherwise the newest the
        // GPU has finished), its shown rect letterboxed / pillarboxed to the frame's shape, centred. True: presented.
        bool presentFrame(const SubmittedFrame &s, const nr_core::Recording &rec)
        {
            const nr_core::FrameInfo &in = s.info;
            if (in.present == 0 || (in.present < 0 && !nr_core::window()))
                return false;
            d3d::SharedPicture sp;
            if (!backend().latestShared(sp, g_serial))
            {
                // (none newer: the frame before's stays shown - normal without waitForGpu. Never one: said once.)
                if (g_serial == 0u && ++g_presentTries == 30u)
                    std::fprintf(stderr, "[native render] submit: no shared picture to present after 30 frames - open() with "
                                         "Settings::sharedPictures = 1 and livePictures = 1 (or TF_NATIVE_LIVE=1)\n");
                return false;
            }
            int ww = 0, wh = 0;
            nr_core::windowSize(ww, wh);
            if (ww <= 0 || wh <= 0)
                return false; // (minimised: drawn, not shown)
            nr_core::PresentRequest rq;
            rq.picture = sp.index;
            const int *r = rec.end.displayRect;
            const bool whole = r[2] <= 0 || r[3] <= 0 ||
                               (r[0] == 0 && r[1] == 0 && static_cast<uint32_t>(r[2]) == sp.width && static_cast<uint32_t>(r[3]) == sp.height);
            if (!whole)
                for (int i = 0; i < 4; ++i)
                    rq.src[i] = static_cast<float>(r[i]);
            const float sw = whole ? static_cast<float>(sp.width) : static_cast<float>(r[2]);
            const float sh = whole ? static_cast<float>(sp.height) : static_cast<float>(r[3]);
            const float shape = rec.end.aspect > 0.0f ? rec.end.aspect : (sh > 0.0f ? sw / sh : 1.0f);
            float dw = static_cast<float>(ww), dh = dw / shape;
            if (dh > static_cast<float>(wh))
                dh = static_cast<float>(wh), dw = dh * shape;
            rq.dst[0] = (static_cast<float>(ww) - dw) * 0.5f, rq.dst[1] = (static_cast<float>(wh) - dh) * 0.5f, rq.dst[2] = dw, rq.dst[3] = dh;
            rq.upscaler = in.upscaler;
            rq.sharpness = in.sharpness;
            rq.syncInterval = in.syncInterval;
            std::memcpy(rq.clear, in.clear, sizeof(rq.clear));
            rq.shot = in.windowShot.empty() ? nullptr : in.windowShot.c_str();
            rq.overlay = in.overlay; // (S12: the host's overlay over this frame's picture)
            if (!nr_core::present(rq))
            {
                static std::atomic<unsigned> fails{0};
                if (fails.fetch_add(1) < 3u)
                    std::fprintf(stderr, "[native render] submit: present failed (window %dx%d, picture %d)\n", ww, wh, sp.index);
                return false;
            }
            return true;
        }

        // The frame drawn: true when recorded and ended; `presented` set when it was presented too.
        bool drawFrame(Job &job, const SubmittedFrame &s, bool &presented)
        {
            if (!deviceUp())
                return false;
            const nr_core::FrameInfo &in = s.info;
            gfx::Device &dev = gfx::device();
            if (!dev.prepareFrame(in.pictureWidth, in.pictureHeight))
                return false;
            if (!g_picture || g_pictureW != in.pictureWidth || g_pictureH != in.pictureHeight)
            {
                if (g_picture)
                    dev.releaseTarget(g_picture);
                gfx::TargetDesc td;
                td.width = in.pictureWidth;
                td.height = in.pictureHeight;
                td.colour = gfx::Format::RGBA8;
                td.depth = false;
                td.shared = true;
                g_picture = dev.createTarget(td);
                g_pictureW = in.pictureWidth, g_pictureH = in.pictureHeight;
                if (!g_picture)
                {
                    std::fprintf(stderr, "[native render] submit: the picture target (%ux%u) was not made\n", in.pictureWidth, in.pictureHeight);
                    g_pictureW = g_pictureH = 0;
                    return false;
                }
            }
            dev.beginFrame(in.tick);
            nr_core::Recording rec;
            rec.picture = g_picture;
            rec.width = in.pictureWidth, rec.height = in.pictureHeight;
            rec.tick = in.tick;
            rec.slot = dev.frameSlot();
            rec.end.pictureTarget = g_picture;
            rec.end.displayRect[2] = static_cast<int>(in.pictureWidth), rec.end.displayRect[3] = static_cast<int>(in.pictureHeight);
            rec.end.aspect = in.aspect;
            if (s.record)
                s.record(dev, rec);
            // The frame's one picture: the identity's constants (no turn, as nr_core_smoke and a 2D frame draw it).
            gfx::FrameEnd end;
            end.frame = &job.frame; // (its tick and moment: submit() set them from FrameInfo)
            end.picture = rec.end.pictureTarget ? rec.end.pictureTarget : g_picture;
            gfx::PictureOut out;
            draw::PictureConstants pc{};
            pc.picture[0] = 1.0f;
            for (int i = 0; i < 4; ++i)
                pc.turn[5 * i] = pc.turnClip[5 * i] = 1.0f;
            out.constants.assign(reinterpret_cast<const uint8_t *>(&pc), reinterpret_cast<const uint8_t *>(&pc) + sizeof(pc));
            out.dueNs = in.dueNs;
            end.pictures.push_back(std::move(out));
            end.shot = in.shot;
            end.waitForGpu = in.waitForGpu;
            if (!dev.endFrame(end))
            {
                if (backend().deviceLost())
                    std::fprintf(stderr, "[native render] submit: device lost - nothing more drawn\n");
                return false;
            }
            presented = presentFrame(s, rec);
            return true;
        }
    }

    void drawSubmitted(Job &job)
    {
        const SubmittedFrame &s = *job.submitted;
        if (s.release)
        {
            releaseAll();
            return;
        }
        bool presented = false;
        const bool drawn = drawFrame(job, s, presented);
        if (s.info.done)
            s.info.done(drawn, presented);
    }
}

namespace nr_core
{
    bool submit(Record record, const FrameInfo &info)
    {
        if (!record || info.pictureWidth == 0u || info.pictureHeight == 0u)
            return false;
        if (nr::backend().deviceLost() || nr::g_startFailed.load())
            return false;
        auto s = std::make_shared<nr::SubmittedFrame>();
        s->record = std::move(record);
        s->info = info;
        nr::Job job;
        job.frame.tick = info.tick;
        job.frame.dueNs = info.dueNs;
        job.submitted = std::move(s);
        nr::g_anySubmitted = true;
        nr::queueSubmitted(std::move(job), nr::kWaiting);
        return true;
    }

    void flushSubmitted()
    {
        if (nr::g_anySubmitted.load())
            nr::waitSubmittedDrawn();
    }

    void endSubmits()
    {
        if (!nr::g_anySubmitted.load())
            return;
        auto s = std::make_shared<nr::SubmittedFrame>();
        s->release = true;
        nr::Job job;
        job.submitted = std::move(s);
        nr::queueSubmitted(std::move(job), nr::kWaiting);
        nr::waitSubmittedDrawn();
    }

    bool ready(int timeoutMs)
    {
        nr::Backend &be = nr::backend();
        const auto t0 = std::chrono::steady_clock::now();
        while (be.startingNow() && std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(timeoutMs))
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return !be.startingNow() && !be.deviceLost() && be.start();
    }
}
