// nr_pace.cpp - the paced hand-over (nr_pace.h, N9): the steady delay that follows the GPU's times, the delay in whole
// refreshes on a screen of fixed refresh, and the thread that hands each picture over at its time. The same rules for
// every backend; Direct3D 12's were in nr_d3d.cpp (pace::) until they moved here.
#include "nr_pace.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace nr::pace
{
    int mode()
    {
        static const int m = [] {
            const char *v = std::getenv("TF_PACING");
            if (!v || !*v)
                return 2;
            if (std::strcmp(v, "ready") == 0)
                return 1;
            return (*v == '0' || std::strcmp(v, "old") == 0) ? 0 : 2;
        }();
        return m;
    }

    int64_t now()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    namespace
    {
        std::atomic<int> g_pictureCap{0};
        std::atomic<unsigned> g_capDrops{0};
        std::atomic<unsigned> g_gateHeld{0};                    // (TODO 336 (24): the gate's holds, for the pacing log)
        std::atomic<int64_t> g_gateHeldNs{0}, g_gateSlicesNs{0};

        std::mutex g_capMutex;       // (the cap's state below)
        int64_t g_capLastNs = 0;     // when the last picture went (0: none yet - the next one goes at once)
        int64_t g_capLastMoment = 0; // and its moment (its arrival time when it had none)
        bool g_capLastHadMoment = false;
        double g_capCredit = 0.0;    // the ONE credit left after it (both clocks share it - two would let the wall
                                     // clock's phase-lock back in)
        constexpr double kCapGate = 0.75; // (the gate: three quarters of a period)

        // The credit a picture would have at t: the larger of its moment's count and the wall clock's, at most 1.1. A
        // moment never counts backwards (a state load, the frame clock rebased after a load or a movie, a rate change);
        // a picture with a moment after one without (or the other way round) counts by the wall clock alone - the two
        // clocks' difference is not a distance.
        double capCreditLocked(int64_t t, int64_t momentNs, double periodNs)
        {
            if (!g_capLastNs)
                return 1.0;
            const double byWall = static_cast<double>(std::max<int64_t>(0, t - g_capLastNs)) / periodNs;
            const double byMoment = momentNs && g_capLastHadMoment
                ? static_cast<double>(std::max<int64_t>(0, momentNs - g_capLastMoment)) / periodNs
                : 0.0;
            return std::min(1.1, g_capCredit + std::max(byMoment, byWall));
        }

        // (TODO 336 (24)) A wait to a moment on the steady clock, to about a tenth of a millisecond - for the gate's rest,
        // a millisecond or less: a sleep ends up to a timer step late (1 to 2 ms where half a millisecond was asked for),
        // a quarter to half a period at a cap of 240. Windows: a high-resolution timer (Windows 10 1803 on) to 0.3 ms
        // before the moment; without one, and elsewhere, sleeps while 2 ms or more are left. The rest is given up time
        // slice by time slice: under half a millisecond with the timer, and only where the gate holds a picture (a few
        // times a second at most). The calling thread's own timer. Returns the time spent in slices (ns).
        int64_t waitUntil(int64_t untilNs)
        {
#if defined(_WIN32)
            thread_local const HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002u /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
                                                                     TIMER_ALL_ACCESS);
            if (timer)
            {
                if (const int64_t left = untilNs - now(); left > 500000)
                {
                    LARGE_INTEGER due;
                    due.QuadPart = -((left - 300000) / 100); // (relative, in 100 ns)
                    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
                        WaitForSingleObject(timer, static_cast<DWORD>(left / 1000000 + 2));
                }
            }
            else
#endif
            {
                for (int64_t left = untilNs - now(); left >= 2000000; left = untilNs - now())
                    std::this_thread::sleep_for(std::chrono::nanoseconds(left - 1500000));
            }
            const int64_t slicesFrom = now();
            int64_t t = slicesFrom;
            for (; t < untilNs; t = now())
                std::this_thread::yield();
            return t - slicesFrom;
        }
    }

    void setPictureCap(int perSecond)
    {
        const int cap = perSecond > 0 ? perSecond : 0;
        if (g_pictureCap.exchange(cap, std::memory_order_relaxed) != cap)
        {
            // (a new cap - Unlimited to a cap included - starts afresh: the next picture goes at once)
            std::lock_guard<std::mutex> lock(g_capMutex);
            g_capLastNs = 0;
            g_capLastMoment = 0;
            g_capLastHadMoment = false;
            g_capCredit = 0.0;
        }
    }

    unsigned takeCapDrops() { return g_capDrops.exchange(0u, std::memory_order_relaxed); }

    int pictureCap() { return g_pictureCap.load(std::memory_order_relaxed); }

    void noteCapDrop() { g_capDrops.fetch_add(1u, std::memory_order_relaxed); }

    GateHolds takeGateHolds()
    {
        GateHolds h;
        h.pictures = g_gateHeld.exchange(0u, std::memory_order_relaxed);
        h.heldMs = static_cast<double>(g_gateHeldNs.exchange(0, std::memory_order_relaxed)) / 1.0e6;
        h.slicesMs = static_cast<double>(g_gateSlicesNs.exchange(0, std::memory_order_relaxed)) / 1.0e6;
        return h;
    }

    int64_t capDueAt(int64_t t, int64_t momentNs)
    {
        const int cap = pictureCap();
        if (cap <= 0)
            return t; // (Unlimited: no gate either)
        const double period = 1.0e9 / cap;
        std::lock_guard<std::mutex> lock(g_capMutex);
        if (!g_capLastNs)
            return t;
        const int64_t gate = g_capLastNs + static_cast<int64_t>(kCapGate * period);
        const double credit = capCreditLocked(t, momentNs, period);
        const int64_t byCredit = credit >= 1.0 ? t : g_capLastNs + static_cast<int64_t>((1.0 - g_capCredit) * period) + 1;
        return std::max({t, gate, byCredit});
    }

    bool capTryTake(int64_t t, int64_t momentNs) { return capTry(t, momentNs) == CapAnswer::Go; }

    CapAnswer capTry(int64_t t, int64_t momentNs)
    {
        const int cap = pictureCap();
        if (cap <= 0)
            return CapAnswer::Go;
        const double period = 1.0e9 / cap;
        std::lock_guard<std::mutex> lock(g_capMutex);
        const double credit = capCreditLocked(t, momentNs, period);
        if (credit < 1.0)
            return CapAnswer::NoCredit;
        // (the gate is load-bearing: when the game runs ahead of real time - a stall's catch-up, old moments - the
        // moments' count says yes to every picture, and only the gate keeps two from going at once)
        if (g_capLastNs && t - g_capLastNs < static_cast<int64_t>(kCapGate * period))
            return CapAnswer::Gate;
        g_capCredit = credit - 1.0;
        g_capLastNs = t;
        g_capLastMoment = momentNs ? momentNs : t; // (both anchors together; a moment that jumped back is the new base)
        g_capLastHadMoment = momentNs != 0;
        return CapAnswer::Go;
    }

    struct Pacer::State
    {
        struct Item
        {
            uint64_t fence = 0;
            Moment moment;
            std::function<void(int64_t)> handOver;
            std::function<void()> drop; // (TODO 336: left out for the cap - its slot freed, nothing shown)
            int64_t readyNs = 0; // (stamped once its fence is seen passed)
        };
        Gpu gpu;
        std::mutex mutex;
        std::condition_variable cv;
        // (TODO 336) Only run() pops this queue, so every item is either handed over or dropped. Any future clear of it (a
        // level load, a lost device) must call each item's `drop`: else the backend's pending marks leak and its shared
        // pictures run out.
        std::deque<Item> queue;
        bool started = false;
        double delayMs = -1.0;                 // (-1: none yet; the pacer's thread only)
        std::atomic<double> delayShown{0.0};   // (the same, for other threads: N9 stage 3's tries, the menus' probes)
        std::deque<std::pair<int64_t, double>> lateness; // (when, ms after the frame's moment)

        // The fences passed now stamped with this moment (they finish in order: the front ones).
        void stampPassed(uint64_t done, int64_t t)
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (Item &it : queue)
            {
                if (it.fence > done)
                    break;
                if (it.readyNs == 0)
                    it.readyNs = t;
            }
        }

        // The delay after the frame's moment for this picture (ms), from its readiness.
        double delayFor(double lateMs, double periodMs, int64_t t)
        {
            if (lateMs > 4.0 * periodMs)
                return delayMs < 0.0 ? 0.0 : delayMs; // (a stall: shown when ready, the delay kept)
            lateness.emplace_back(t, lateMs);
            while (!lateness.empty() && t - lateness.front().first > 2000000000ll)
                lateness.pop_front();
            std::vector<double> v;
            v.reserve(lateness.size());
            for (const auto &l : lateness)
                v.push_back(l.second);
            const size_t at = std::min(v.size() - 1u, static_cast<size_t>(0.98 * static_cast<double>(v.size())));
            std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(at), v.end());
            const double wanted = v[at] + 1.0;
            // (TODO 169) Down by 2% of what is over the wanted delay a picture, 0.05 to 0.25 ms (TF_PACING_DOWN=<ms>:
            // a fixed step; 0.05 was the old one). The fixed 0.05 ms took 7-10 s to give back a delay raised by a load or
            // a rate change (the other PC at 80: shown 50 ms after the moment for the first 8 s, 17 ms after), and a busy
            // PC's late pictures kept it high for good; a step of at most 0.25 ms moves a picture by 2% of a 12.5 ms
            // interval - not seen.
            static const double fixedDown = std::getenv("TF_PACING_DOWN") ? std::atof(std::getenv("TF_PACING_DOWN")) : 0.0;
            if (delayMs < 0.0 || lateMs + 1.0 > delayMs)
                delayMs = std::max(delayMs, lateMs + 1.0); // (up at once)
            else if (wanted < delayMs - 0.5)
            {
                const double step = fixedDown > 0.0 ? fixedDown : std::clamp(0.02 * (delayMs - wanted), 0.05, 0.25);
                delayMs = std::max(wanted, delayMs - step);
            }
            delayMs = std::min(delayMs, 4.0 * periodMs);
            delayShown.store(delayMs, std::memory_order_relaxed);
            return delayMs;
        }
    };

    Pacer::Pacer(Gpu gpu) : m_state(std::make_unique<State>())
    {
        m_state->gpu = std::move(gpu);
    }

    Pacer::~Pacer() = default;

    double Pacer::delayMs() const { return m_state->delayShown.load(std::memory_order_relaxed); }

    void Pacer::run()
    {
        State &s = *m_state;
        const bool paced = mode() == 2;
        for (;;)
        {
            State::Item it;
            std::function<void(int64_t)> handOver;
            std::function<void()> drop;
            {
                std::unique_lock<std::mutex> lock(s.mutex);
                s.cv.wait(lock, [&s] { return !s.queue.empty(); });
                State::Item &front = s.queue.front();
                it.fence = front.fence;
                it.moment = front.moment;
                handOver = std::move(front.handOver);
                drop = std::move(front.drop);
            }
            // Its fence (the GPU done with the picture).
            if (s.gpu.completed() < it.fence)
                s.gpu.waitFor(it.fence);
            const int64_t seen = now();
            s.stampPassed(s.gpu.completed(), seen);
            {
                std::lock_guard<std::mutex> lock(s.mutex);
                it.readyNs = s.queue.front().readyNs ? s.queue.front().readyNs : seen;
            }
            // Its time: the frame's moment plus the delay (on a fixed refresh, whole VBlanks).
            int64_t scheduledNs = it.readyNs; // (when it was to go: its time when paced, else as soon as ready)
            if (paced && it.moment.dueNs != 0)
            {
                const double periodMs = it.moment.periodNs > 0 ? static_cast<double>(it.moment.periodNs) / 1.0e6 : 16.667;
                double delay = s.delayFor(static_cast<double>(it.readyNs - it.moment.dueNs) / 1.0e6, periodMs, it.readyNs);
                if (it.moment.onRefresh)
                {
                    // (TODO 169) The delay a whole number of the screen's REFRESHES, not of the picture's period. The
                    // frames' moments are on refreshes (a whole number of them a picture), so any whole number of
                    // refreshes after the moment still meets a refresh and keeps the pictures exactly as far apart
                    // as before. Rounding up to whole picture periods put a picture ready 15-17 ms after its moment
                    // at a cap of 60 on a 180 Hz screen a whole period later (shown 34 ms after the moment where
                    // 23 would do); at 30, 33 ms later. TF_PACING_QUANTUM=picture: whole picture periods (before).
                    static const bool byPicture = std::getenv("TF_PACING_QUANTUM") && std::string(std::getenv("TF_PACING_QUANTUM")) == "picture";
                    const double picture = it.moment.quantumNs > 0 ? static_cast<double>(it.moment.quantumNs) / 1.0e6 : periodMs;
                    const double refresh = static_cast<double>(it.moment.refreshNs) / 1.0e6;
                    const double q = !byPicture && refresh > 1.0 && refresh <= picture + 0.01 ? refresh : picture;
                    delay = std::ceil(delay / q - 1.0e-3) * q;
                }
                const int64_t at = it.moment.dueNs + static_cast<int64_t>(delay * 1.0e6);
                scheduledNs = std::max(scheduledNs, at);
                // (meanwhile the later pictures' fences are stamped as they pass, to the half millisecond)
                for (int64_t t = now(); t < at; t = now())
                {
                    std::this_thread::sleep_for(std::chrono::nanoseconds(std::min<int64_t>(at - t, 500000)));
                    s.stampPassed(s.gpu.completed(), now());
                }
            }
            // (TODO 336 (18)/(20)/(21)) The cap: a bucket of one picture's credit a cap period, at most 1.1 banked (a picture a
            // little early by the VBlanks' jitter at cap 60 is not held, a stall's catch-up burst is). Without credit the
            // picture waits; it is left out only once a newer picture is ready AND that one's moment has come - so at cap 30
            // a 30-a-second menu's own picture is never dropped for an in-between one due later (a visible step).
            // (TODO 336 (24)) A picture with its credit that only the gate holds (it came close behind the last one: the
            // timing's jitter) waits the gate's rest out, to a tenth of a millisecond, and is shown; left out for a newer
            // one only when it is a whole period behind its time (a stall's catch-up burst).
            if (const int cap = g_pictureCap.load(std::memory_order_relaxed); cap > 0)
            {
                const int64_t periodNs = 1000000000ll / cap;
                bool dropped = false;
                int64_t gateFrom = 0;
                for (int64_t t = now();; t = now())
                {
                    const CapAnswer answer = capTry(t, it.moment.dueNs);
                    if (answer == CapAnswer::Go)
                    {
                        if (gateFrom)
                            g_gateHeldNs.fetch_add(t - gateFrom, std::memory_order_relaxed);
                        break;
                    }
                    if (answer == CapAnswer::Gate && !gateFrom)
                    {
                        gateFrom = t;
                        g_gateHeld.fetch_add(1u, std::memory_order_relaxed);
                    }
                    if (answer == CapAnswer::NoCredit || t - scheduledNs >= periodNs)
                    {
                        std::lock_guard<std::mutex> lock(s.mutex);
                        if (s.queue.size() > 1u)
                        {
                            const State::Item &next = s.queue[1];
                            dropped = s.gpu.completed() >= next.fence && (next.moment.dueNs == 0 || next.moment.dueNs <= t);
                        }
                    }
                    if (dropped)
                        break;
                    const int64_t due = capDueAt(t, it.moment.dueNs);
                    if (answer == CapAnswer::Gate)
                        g_gateSlicesNs.fetch_add(waitUntil(std::min(due, t + 1000000)), // (in steps of a millisecond at most: the
                                                 std::memory_order_relaxed);            // fences stamped between)
                    else
                        std::this_thread::sleep_for(std::chrono::nanoseconds(std::clamp<int64_t>(due - t, 100000, 500000)));
                    s.stampPassed(s.gpu.completed(), now());
                }
                if (dropped)
                {
                    if (drop)
                        drop();
                    noteCapDrop();
                    std::lock_guard<std::mutex> lock(s.mutex);
                    s.queue.pop_front();
                    continue;
                }
            }
            handOver(it.readyNs);
            {
                std::lock_guard<std::mutex> lock(s.mutex);
                s.queue.pop_front();
            }
        }
    }

    void Pacer::submit(uint64_t fence, const Moment &moment, std::function<void(int64_t readyNs)> handOver, std::function<void()> drop)
    {
        State &s = *m_state;
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.started)
        {
            s.started = true;
            std::thread([this] { run(); }).detach();
        }
        State::Item it;
        it.fence = fence;
        it.moment = moment;
        it.handOver = std::move(handOver);
        it.drop = std::move(drop);
        s.queue.push_back(std::move(it));
        s.cv.notify_one();
    }
}
