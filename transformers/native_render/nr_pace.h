// nr_pace.h - the paced hand-over (N9, TODO 95 / 169), the same rules for every backend: each picture handed to the
// window - or presented - at its frame's moment (Frame::dueNs, PictureOut::dueNs: the VBlank it was handed over at on the
// host's schedule) plus a delay, the same for every picture, so that they reach the screen as evenly as the frames'
// moments are spaced, whatever the render thread's and the GPU's times do from one frame to the next. The delay follows
// the pictures' readiness (the GPU done, after the frame's moment): the 98th percentile of the last 2 s plus 1 ms, raised
// at once by a late one (not by a stall of over four frames: a load, a hitch, which no delay hides) and lowered slowly.
// When the VBlanks are timed by the screen's refresh (a whole number of refreshes a VBlank: Frame::vblanksOnRefresh) the
// delay is a whole number of refreshes, so each picture meets its refresh. A thread of its own waits for each picture's
// GPU work (in the order given: a GPU queue finishes them in order) and then for its time. TF_PACING=ready hands each
// over as soon as the GPU is done with it; TF_PACING=0 leaves the pacer out (the backend's own hand-over).
//
// A backend gives the pacer only its GPU side (Gpu: how far its fence or timeline has come, a wait for a value) and, for
// each picture, its hand-over (the Direct3D 12 backend: the shared texture to the window's thread, nr_d3d.cpp).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace nr::pace
{
    // TF_PACING: 2 paced (the default) - each picture at its frame's moment plus the steady delay; 1 ("ready") - each as
    // soon as the GPU is done with it; 0 ("0" or "old") - no pacer: the backend's own hand-over (D3D12: at the next
    // frame's start, as before N9). Fixed at its first use.
    int mode();

    // The steady clock in ns (the frames' moments are on it).
    int64_t now();

    // (TODO 336 (18)/(20)/(21)) The pictures' rate cap: the settings' frame-rate cap (the SETTING - never the rate ladder's
    // current top, which would limit twice at its margin), 0 = none (Unlimited). Never more pictures a second than this -
    // in the boot, the menus, a load and a stall's catch-up as in a level: a picture without credit waits for it, and is
    // left out (dropped, its slot freed) once a newer picture is ready and that one's moment has come too. The game never
    // waits for it: its time is kept. Any thread.
    void setPictureCap(int perSecond);
    // The pictures left out for the cap since the last call (the pacing log's "left out N (the cap)"). Any thread.
    unsigned takeCapDrops();
    // The cap now (0: none) and a picture left out for it. Any thread.
    int pictureCap();
    void noteCapDrop();
    // (TODO 336) The cap for every hand-over to the window - the Pacer and the Intel CPU hand-over (nr_d3d.cpp cpucopy,
    // which never reaches the Pacer) - under ONE lock: a backend can switch from one to the other mid-run, and the cap means
    // pictures a second at the window. Two rules, both needed for a picture to go:
    //  - credit by the pictures' MOMENTS (their frames' VBlanks; the arrival time for a picture without one): each brings
    //    its moment's distance from the last shown picture's, in cap periods, at most 1.1 banked; one goes at 1. In a level
    //    at the cap's rate every picture brings exactly one - never held, whatever the GPU's jitter; a 60-a-second loading
    //    screen under a 30 cap brings a half - every other one is left out. (A bucket of ARRIVAL times phase-locks there:
    //    a picture taken h late leaves the next short by h, for ever - the Vostro's trace, cap 30: every picture held
    //    9-11 ms.) The wall clock gives credit too (the larger of the two counts), so the last picture before a pause or
    //    the end of a load is never stuck. ONE credit for both clocks; a moment never counts backwards (a state load, the
    //    frame clock rebased, a rate change); a picture with a moment after one without (or the other way round) counts by
    //    the wall clock alone; a new cap (Unlimited to a cap included) starts afresh.
    //  - a gate: at least three quarters of a period since the last picture went, nothing owed - bunched arrivals and a
    //    stall's catch-up never pass two at once, and an offset shrinks by a quarter period a picture.
    // capDueAt(t, moment): when that picture may go by the wall clock (t itself when it may now, or without a cap);
    // capTryTake(t, moment): it may go at t - checked and taken under the one lock (true without a cap). Every picture
    // shown under a cap goes through capTryTake. Any thread.
    int64_t capDueAt(int64_t t, int64_t momentNs);
    bool capTryTake(int64_t t, int64_t momentNs);

    // A picture's moment and the VBlanks' clock it was made on (from the frame: Frame::dueNs / PictureOut::dueNs,
    // periodNs, pictureNs, refreshNs, vblanksOnRefresh).
    struct Moment
    {
        int64_t dueNs = 0;      // the frame's moment (0: none - handed over as soon as it is ready)
        int64_t periodNs = 0;   // the VBlank period then (0: 16.667 ms)
        int64_t quantumNs = 0;  // (N9 stage 2: whole refreshes on a screen of fixed refresh - the picture's period)
        int64_t refreshNs = 0;  // (TODO 169: the screen's refresh, the delay's step on a fixed refresh)
        bool onRefresh = false; // the VBlanks timed by the screen's refresh
    };

    // The backend's GPU side, called on the pacer's thread: how far its fence (or timeline semaphore) has come, and a wait
    // until it passes a value - returning early when the device is lost (D3D12: checked every 200 ms).
    struct Gpu
    {
        std::function<uint64_t()> completed;
        std::function<void(uint64_t value)> waitFor;
    };

    // The pacer: one per backend, its thread started at the first picture and running to the end.
    class Pacer
    {
    public:
        explicit Pacer(Gpu gpu);
        ~Pacer();
        // A picture to hand over once the GPU has passed `fence` (in the order given) and, paced, at its moment plus the
        // delay: `handOver(readyNs)` on the pacer's thread, readyNs when its fence was first seen passed.
        // `drop`: the picture left out for the cap (TODO 336) - not shown, only its slot freed (the backend's pending mark).
        void submit(uint64_t fence, const Moment &moment, std::function<void(int64_t readyNs)> handOver,
                    std::function<void()> drop = {});
        // The steady delay now (ms after the frames' moments; 0 before the first paced picture). Any thread.
        double delayMs() const;

    private:
        struct State;
        std::unique_ptr<State> m_state;
        void run();
    };
}
