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

    struct Pacer::State
    {
        struct Item
        {
            uint64_t fence = 0;
            Moment moment;
            std::function<void(int64_t)> handOver;
            int64_t readyNs = 0; // (stamped once its fence is seen passed)
        };
        Gpu gpu;
        std::mutex mutex;
        std::condition_variable cv;
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
            {
                std::unique_lock<std::mutex> lock(s.mutex);
                s.cv.wait(lock, [&s] { return !s.queue.empty(); });
                State::Item &front = s.queue.front();
                it.fence = front.fence;
                it.moment = front.moment;
                handOver = std::move(front.handOver);
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
                // (meanwhile the later pictures' fences are stamped as they pass, to the half millisecond)
                for (int64_t t = now(); t < at; t = now())
                {
                    std::this_thread::sleep_for(std::chrono::nanoseconds(std::min<int64_t>(at - t, 500000)));
                    s.stampPassed(s.gpu.completed(), now());
                }
            }
            handOver(it.readyNs);
            {
                std::lock_guard<std::mutex> lock(s.mutex);
                s.queue.pop_front();
            }
        }
    }

    void Pacer::submit(uint64_t fence, const Moment &moment, std::function<void(int64_t readyNs)> handOver)
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
        s.queue.push_back(std::move(it));
        s.cv.notify_one();
    }
}
