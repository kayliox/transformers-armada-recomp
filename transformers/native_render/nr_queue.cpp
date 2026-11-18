// The frame queue (nr_queue.h), in the core (nr_core S3a): moved here from nr_thread.cpp. The game's thread hands each finished
// frame over (queueFrame) and goes on with the game; one thread reads each frame's list as soon as it comes (the game waits for
// that alone), the other draws (TODO 161 a). With the levels' fixed logic rate a frame the drawing has not reached when a newer
// one is read is left out (its picture, never the game's time); following the PC's rate (or TF_FRAME_DROPS=0) a faster game
// waits for the drawing rather than piling frames up. What a frame's read and draw are is the host's (nr::FrameQueueHost: the
// recomp's display-list read, grass and pictures between frames stay in nr_thread.cpp).
#include "nr_queue.h"
#include "nr_pictures.h"
#include "nr_submit.h"
#include "nr_backend.h"
#include "nr_core.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
// (kernel32, declared here as nr_thread.cpp does)
extern "C" __declspec(dllimport) void *__stdcall GetCurrentThread();
extern "C" __declspec(dllimport) long __stdcall SetThreadDescription(void *thread, const wchar_t *description);
#endif

namespace nr
{
    namespace
    {
        FrameQueueHost g_host;

        int64_t nowNs()
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // (TODO 169) TF_NATIVE_LATLOG=<s>: every <s> seconds, where a drawn frame's time goes from its moment (the VBlank it
        // was handed over at) to its picture drawn: handed over, its read begun, read, its drawing begun, drawn - each the
        // mean and the 95th percentile, ms after the moment.
        uint32_t latLogSeconds()
        {
            static const uint32_t s = std::getenv("TF_NATIVE_LATLOG") ? static_cast<uint32_t>(std::max(0, std::atoi(std::getenv("TF_NATIVE_LATLOG")))) : 0u;
            return s;
        }
        struct LatLog
        {
            std::vector<double> steps[6]; // (the pad read, handed over, read begun, read, drawing begun, drawn)
            std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
        } g_latLog;
        double g_latePickMs = 0.0; // (TODO 169 a: the drawing thread's waits for the GPU before taking a frame; the render thread's)

        void latNote(const Job &job, int64_t drawStartNs, int64_t drawEndNs)
        {
            if (latLogSeconds() == 0u || job.frame.dueNs == 0 || !job.frame.has3D)
                return;
            LatLog &l = g_latLog;
            const int64_t at[6] = {job.inputNs ? job.inputNs : job.frame.dueNs, job.submitNs, job.readStartNs, job.readEndNs, drawStartNs, drawEndNs};
            for (int i = 0; i < 6; ++i)
                l.steps[i].push_back(static_cast<double>(at[i] - job.frame.dueNs) / 1e6);
            const auto now = std::chrono::steady_clock::now();
            if (now - l.last < std::chrono::seconds(latLogSeconds()))
                return;
            static const char *const names[6] = {"the pad read", "handed over", "read begun", "read", "drawing begun", "drawn"};
            std::string line;
            for (int i = 0; i < 6; ++i)
            {
                std::vector<double> &v = l.steps[i];
                double mean = 0.0;
                for (double x : v)
                    mean += x;
                mean /= static_cast<double>(v.size());
                const size_t k = std::min(v.size() - 1u, static_cast<size_t>(0.95 * static_cast<double>(v.size())));
                std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
                char item[96];
                std::snprintf(item, sizeof(item), "%s%s %+.2f (p95 %+.2f)", i ? ", " : "", names[i], mean, v[k]);
                line += item;
                v.clear();
            }
            std::fprintf(stderr, "[native render] latency, ms after the frame's moment: %s; the drawing waited %.1f ms for the GPU before taking a frame\n",
                         line.c_str(), g_latePickMs);
            g_latePickMs = 0.0;
            l.last = now;
        }
        std::mutex g_mutex;
        std::condition_variable g_cv, g_room;
        std::deque<Job> g_jobs;
        bool g_started = false;
        bool g_busy = false;
        bool g_reading = false; // a frame's display list being read from guest memory
        // (nr_core S10) Submitted frames (nr_core::submit) waiting to be drawn, and those not yet drawn to the end (waiting or
        // being drawn); under g_mutex, waited on with g_room.
        unsigned g_submitWaiting = 0, g_submitInFlight = 0;
        std::atomic<uint64_t> g_gameWaitNs{0}; // (TODO 112) the game's thread's waits in queueFrame, all of them
        // (TODO 94) Frames' draws and lists given back by the renderer, for the game's next frames (recycleFrameBuffers).
        std::mutex g_spareMutex;
        std::vector<std::vector<Draw>> g_spareDraws;
        std::vector<std::vector<uint32_t>> g_spareLists;
        std::vector<std::vector<uint32_t>> g_spareChunks; // (N5 step 2 part 2: Frame::vuChunks)

        // (TODO 161 a) Frames left out when the renderer is behind: with the fixed logic rate (TF_FRAME_DROPS=0: every frame
        // drawn, the game held back by the drawing as before).
        bool framesDropped()
        {
            // (TODO 165: following the cap too - a short hitch leaves pictures out, a lasting share of them lowers the rate,
            // overrides.cpp)
            static const bool off = std::getenv("TF_FRAME_DROPS") && *std::getenv("TF_FRAME_DROPS") == '0';
            return !off;
        }

        // (TODO 161 a) The frames read, waiting for the drawing (readLoop -> drawLoop), and what was left out.
        std::deque<Job> g_ready;
        std::condition_variable g_readyCv;
        struct Drops
        {
            uint64_t drawn = 0, behind = 0, capped = 0, tested = 0, keptDraws = 0, window = 0;
        } g_drops;
        std::atomic<uint64_t> g_behindTotal{0}; // (nr::framesLeftOutBehind)
        // (TODO 290 fps) The drawing's time for each frame it drew - the slower of its two stages, the reading (its own
        // thread) and the drawing (this one, its waits for the GPU included): what one frame holds the renderer for. The
        // rate governor's way up asks whether a higher rate fits it (nr::drawnFrameMs); before, it judged the EE's work
        // alone and climbed into rates the renderer could not draw, fell, and climbed again.
        struct DrawnTimes
        {
            std::mutex mutex;
            std::array<float, 1024> ms{};
            size_t next = 0, count = 0;
        } g_drawnTimes, g_gpuTimes; // (and the GPU's time for each frame: nr::noteGpuFrame, from the backends' timestamps)
        void noteTime(DrawnTimes &t, double ms)
        {
            std::lock_guard<std::mutex> lock(t.mutex);
            t.ms[t.next] = static_cast<float>(ms);
            t.next = (t.next + 1u) % t.ms.size();
            t.count = std::min(t.count + 1u, t.ms.size());
        }
        void noteDrawnFrame(double readMs, double drawMs) { noteTime(g_drawnTimes, std::max(readMs, drawMs)); }
        double timeAt(DrawnTimes &t, double share, unsigned frames)
        {
            std::vector<float> v;
            {
                std::lock_guard<std::mutex> lock(t.mutex);
                const size_t n = std::min<size_t>(frames, t.count);
                v.reserve(n);
                for (size_t i = 0; i < n; ++i)
                    v.push_back(t.ms[(t.next + t.ms.size() - 1u - i) % t.ms.size()]);
            }
            if (v.size() < 8u)
                return 0.0;
            const size_t k = std::min(v.size() - 1u, static_cast<size_t>(std::clamp(share, 0.0, 1.0) * static_cast<double>(v.size() - 1u)));
            std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
            return v[k];
        }

        uint32_t keyOf(uint32_t target) { return (target & 0x1FFu) | ((target >> 16) & 0x3Fu) << 9; } // (nr_d3d.cpp targetKey)
        bool depthTarget(uint32_t target) { return ((target >> 24) & 0x30u) == 0x30u; }

        std::vector<uint32_t> targetsOf(const Frame &f)
        {
            std::vector<uint32_t> keys;
            for (const Draw &d : f.draws)
                if (!depthTarget(d.target))
                {
                    const uint32_t k = keyOf(d.target);
                    if (std::find(keys.begin(), keys.end(), k) == keys.end())
                        keys.push_back(k);
                }
            return keys;
        }

        // A frame that may be left out: a scene's (a menu, a movie, a load screen or the first frame after a load are
        // cheap, or shown as they come), not one asked to be saved.
        bool droppable(const Job &job)
        {
            const Frame &f = job.frame;
            return f.has3D && !f.loose && !f.firstAfterLoad && !f.movie && job.shot.empty();
        }

        // A frame left out: still drawn where it draws a render-to-texture of its own - a target the newer frame does not
        // draw and not its own picture (the largest it draws into) - else not drawn at all.
        void dropFrame(Job &job, std::vector<uint32_t> &&superseded)
        {
            Frame &f = job.frame;
            f.dropped = true;
            f.pictures.clear();
            static const bool keepAll = std::getenv("TF_DROP_KEEP") && std::strcmp(std::getenv("TF_DROP_KEEP"), "all") == 0;
            if (keepAll)
            {
                f.droppedDrawn = true;
                backend().render(f, std::string());
                picturesFrameLeftOut(f);
                return;
            }
            uint32_t largest = 0u;
            float area = -1.0f;
            for (const Draw &d : f.draws)
                if (!depthTarget(d.target) && d.width * d.height > area)
                    area = d.width * d.height, largest = keyOf(d.target);
            // (the picture: every target on the frame buffer's base - the largest drawn and the one the display shows; the
            // game draws its glow and its HUD into the same memory at other widths, and the double buffer alternates them
            // frame by frame, so a frame left out would otherwise keep those draws of its own: Deep Amazon, 870 and 1070)
            const uint32_t shownFbp = (f.pmode & 1u) ? static_cast<uint32_t>(f.dispfb[0]) & 0x1FFu : ~0u;
            const auto picture = [&](uint32_t k) { return k == largest || (k & 0x1FFu) == (largest & 0x1FFu) || (k & 0x1FFu) == shownFbp; };
            bool kept = false;
            for (const Draw &d : f.draws)
            {
                if (depthTarget(d.target))
                    continue;
                const uint32_t k = keyOf(d.target);
                if (picture(k) && std::find(superseded.begin(), superseded.end(), k) == superseded.end())
                    superseded.push_back(k);
                if (!picture(k) && std::find(superseded.begin(), superseded.end(), k) == superseded.end())
                {
                    kept = true;
                    ++g_drops.keptDraws;
                }
            }
            // (TF_DROP_LOG=1, a checking aid: the first 20 frames left out - what of theirs is still drawn)
            static int logged = std::getenv("TF_DROP_LOG") && *std::getenv("TF_DROP_LOG") == '1' ? 20 : 0;
            if (logged > 0)
            {
                --logged;
                std::string kept, gone;
                std::vector<std::pair<uint32_t, int>> counts;
                for (const Draw &d : f.draws)
                {
                    if (depthTarget(d.target))
                        continue;
                    const uint32_t k = keyOf(d.target);
                    auto it = std::find_if(counts.begin(), counts.end(), [&](const auto &c) { return c.first == k; });
                    if (it == counts.end())
                        counts.emplace_back(k, 1);
                    else
                        ++it->second;
                }
                for (const auto &[k, n] : counts)
                {
                    const bool off = std::find(superseded.begin(), superseded.end(), k) != superseded.end();
                    char item[48];
                    std::snprintf(item, sizeof(item), " %x:%d", k, n);
                    (off ? gone : kept) += item;
                }
                std::fprintf(stderr, "[native render] frame %llu left out: drawn still%s; left out%s\n", static_cast<unsigned long long>(f.tick),
                             kept.empty() ? " none" : kept.c_str(), gone.c_str());
            }
            f.superseded = std::move(superseded);
            f.superseded.push_back(largest);
            if (kept)
                backend().render(f, std::string());
            // (the next frame's pictures between frames span from this one's moment: the pictures before it are the ones
            // left out - posed from the frame before's pose they would run the time on slower)
            picturesFrameLeftOut(f);
        }

        // A frame drawn by the host, timed: the latency log, the drawn frames' times (TODO 290 fps), the slow frames told.
        void drawOne(Job &job)
        {
            const int64_t drawStartNs = latLogSeconds() ? nowNs() : 0;
            const auto start = std::chrono::steady_clock::now();
            if (job.submitted)
                drawSubmitted(job); // (nr_core S10: the host's own recording, nr_submit.cpp)
            else if (g_host.draw)
                g_host.draw(job);
            if (latLogSeconds())
                latNote(job, drawStartNs, nowNs());
            const double drawMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            const double ms = job.readMs + drawMs;
            noteDrawnFrame(job.readMs, drawMs); // (TODO 290 fps: nr::drawnFrameMs)
            // (A slow frame told, its parts: the list read and the GPU side - TODO 59's hunt.)
            static const double slowMs = std::getenv("TF_NATIVE_SLOWMS") ? std::atof(std::getenv("TF_NATIVE_SLOWMS")) : 50.0;
            if (ms > slowMs)
            {
                static int told = 0;
                if (told < (slowMs < 50.0 ? 400 : 40))
                {
                    ++told;
                    std::fprintf(stderr, "[native render] slow frame at tick %llu: %.1f ms (list %.1f, drawing %.1f), %zu draws\n",
                                 static_cast<unsigned long long>(job.frame.tick), ms, job.readMs, ms - job.readMs, job.frame.draws.size());
                }
            }
            static double spent = 0.0, worst = 0.0, readSpent = 0.0;
            static uint32_t frames = 0u;
            spent += ms;
            readSpent += job.readMs;
            worst = std::max(worst, ms);
            if (++frames == 300u)
            {
                std::fprintf(stderr, "[native render] render thread: %.2f ms a frame on average, %.2f at worst (the list read %.2f, the drawing %.2f)\n",
                             spent / 300.0, worst, readSpent / 300.0, (spent - readSpent) / 300.0);
                spent = worst = readSpent = 0.0;
                frames = 0u;
            }
        }

        // The frames' reading from guest memory: as soon as each is handed over (the game waits for it alone - its packet
        // buffer is the one the game builds the frame after next in), then on to the drawing.
        void readLoop()
        {
#if defined(_WIN32)
            SetThreadDescription(GetCurrentThread(), L"NativeRead");
#endif
            nr_core::placeThisThread(nr_core::ThreadRole::RenderRead); // (the host's rule: PS2X_THREAD_PLACEMENT)
            for (;;)
            {
                Job job;
                {
                    std::unique_lock<std::mutex> lock(g_mutex);
                    g_cv.wait(lock, [] { return !g_jobs.empty(); });
                    job = std::move(g_jobs.front());
                    g_jobs.pop_front();
                    // (nr_core S11) A submitted frame: nothing to read - passed on to the drawing in its place in line.
                    if (job.submitted)
                    {
                        g_ready.push_back(std::move(job));
                        lock.unlock();
                        g_room.notify_all();
                        g_readyCv.notify_one();
                        continue;
                    }
                    g_reading = true;
                }
                g_room.notify_all();
                const auto start = std::chrono::steady_clock::now();
                if (latLogSeconds())
                    job.readStartNs = nowNs();
                // (TODO 406, a test switch: TF_NATIVE_READ_SLOW=<ms> - each frame's read starts that much later, so an
                // in-session state load lands in the middle of one: the race finishReadsBeforeLoad closes, made certain)
                static const int readSlow = std::getenv("TF_NATIVE_READ_SLOW") ? std::atoi(std::getenv("TF_NATIVE_READ_SLOW")) : 0;
                if (readSlow > 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(readSlow));
                if (g_host.read)
                    g_host.read(job);
                job.readMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                if (latLogSeconds())
                    job.readEndNs = nowNs();
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_reading = false; // (the game may write the packet buffer this frame read)
                    g_ready.push_back(std::move(job));
                }
                g_room.notify_all();
                g_readyCv.notify_one();
            }
        }

        // The drawing (TODO 161 a): the newest frame read, the ones before it left out when the renderer is behind - the
        // game's logic keeps its fixed rate, its pictures are fewer (TF_FRAME_DROPS=0: every frame drawn, the game held back
        // as before). The frame rate cap's too: a frame due within the cap's period of the last shown is left out.
        // TF_DROP_TEST=<n> (a checking aid): every n-th frame left out, as if the renderer were behind.
        void drawLoop()
        {
#if defined(_WIN32)
            // (Named for debuggers and the profiler's TF_PROFILE_THREAD=NativeRender.)
            SetThreadDescription(GetCurrentThread(), L"NativeRender");
#endif
            nr_core::placeThisThread(nr_core::ThreadRole::Render); // (the host's rule: PS2X_THREAD_PLACEMENT)
            static const uint64_t test = std::getenv("TF_DROP_TEST") ? std::strtoull(std::getenv("TF_DROP_TEST"), nullptr, 10) : 0u;
            uint64_t testCount = 0;
            int64_t lastShownDue = 0;
            std::vector<uint32_t> lastTargets;
            auto saidAt = std::chrono::steady_clock::now();
            for (;;)
            {
                std::deque<Job> batch;
                {
                    std::unique_lock<std::mutex> lock(g_mutex);
                    g_readyCv.wait(lock, [] { return !g_ready.empty(); });
                    // (TODO 169 a) A GPU holding the renderer back: the frame to draw taken only once the GPU can start it
                    // (its slot given back) - the newest read by then, the older ones left out as below - rather than the
                    // one read when this thread came free, which then waited for the GPU inside its drawing.
                    // OFF by default (TF_NATIVE_LATEPICK=1 to try it): measured on the test PC (2026-11-06) it cuts input to
                    // screen where the GPU holds the renderer back (cap 240: 32.7 -> 29.4 ms) but draws ~8-12% fewer frames - the
                    // frame's CPU work before its drawing (the grass) no longer overlaps the GPU's - and at 120 it lost both.
                    static const bool latePick = std::getenv("TF_NATIVE_LATEPICK") && *std::getenv("TF_NATIVE_LATEPICK") == '1';
                    if (latePick)
                    {
                        lock.unlock();
                        g_latePickMs += backend().waitForFrameSlot();
                        lock.lock();
                    }
                    batch.swap(g_ready);
                    g_busy = true;
                }
                g_room.notify_all(); // (the game may hand over its next frame: TF_FRAME_DROPS=0's wait)
                for (size_t i = 0; i < batch.size(); ++i)
                {
                    Job &job = batch[i];
                    const bool submitted = job.submitted != nullptr; // (nr_core S10: never left out - droppable() is false)
                    if (submitted)
                    {
                        {
                            std::lock_guard<std::mutex> lock(g_mutex);
                            --g_submitWaiting; // (taken: the host may hand over one more)
                        }
                        g_room.notify_all();
                    }
                    const bool newer = i + 1 < batch.size();
                    const bool can = framesDropped() && droppable(job);
                    const int cap = g_host.pictureCap ? g_host.pictureCap() : 0;
                    const int64_t capNs = cap > 0 ? 1000000000ll / cap : 0;
                    const int64_t periodNs = job.frame.periodNs > 0 ? job.frame.periodNs : 0;
                    // (only a cap under the logic rate, and moments that go forward - a schedule started again goes back)
                    const bool overCap = can && capNs > periodNs && lastShownDue != 0 && job.frame.dueNs > lastShownDue &&
                                         job.frame.dueNs - lastShownDue < capNs - periodNs / 2;
                    const bool tested = can && test > 0 && ++testCount % test == 0;
                    // (TODO 420) The window has not taken the pictures before this one (the host's answer; null: never): left
                    // out, so the GPU's time goes to the pictures it shows. Not counted as the renderer behind - the rate's
                    // governor reads that.
                    const bool windowBehind = can && !newer && !overCap && !tested && g_host.windowBehind && g_host.windowBehind();
                    if (can && (newer || overCap || tested || windowBehind))
                    {
                        (newer ? g_drops.behind : overCap ? g_drops.capped : tested ? g_drops.tested : g_drops.window) += 1;
                        if (newer)
                            g_behindTotal.fetch_add(1u, std::memory_order_relaxed);
                        dropFrame(job, newer ? targetsOf(batch[i + 1].frame) : std::vector<uint32_t>(lastTargets));
                    }
                    else
                    {
                        if (job.frame.has3D)
                            lastTargets = targetsOf(job.frame);
                        drawOne(job);
                        ++g_drops.drawn;
                        if (job.frame.dueNs != 0)
                            lastShownDue = job.frame.dueNs;
                    }
                    recycleFrameBuffers(job.frame); // (its draws' meshes and textures let go of here, their memory kept)
                    job = Job{}; // (the frame's meshes and textures let go of here, not under the lock)
                    if (submitted)
                    {
                        {
                            std::lock_guard<std::mutex> lock(g_mutex);
                            --g_submitInFlight; // (drawn, its recording's captures let go of: flushSubmitted may return)
                        }
                        g_room.notify_all();
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_busy = false;
                }
                g_room.notify_all();
                if (std::chrono::steady_clock::now() - saidAt >= std::chrono::seconds(10))
                {
                    saidAt = std::chrono::steady_clock::now();
                    if (g_drops.behind || g_drops.capped || g_drops.tested || g_drops.window)
                        std::fprintf(stderr, "[native render] frames in 10 s: %llu drawn, left out %llu (renderer behind), %llu (the cap), %llu (TF_DROP_TEST), "
                                             "%llu (the window behind); %llu draws of theirs kept (a render-to-texture of their own)\n",
                                     static_cast<unsigned long long>(g_drops.drawn), static_cast<unsigned long long>(g_drops.behind),
                                     static_cast<unsigned long long>(g_drops.capped), static_cast<unsigned long long>(g_drops.tested),
                                     static_cast<unsigned long long>(g_drops.window), static_cast<unsigned long long>(g_drops.keptDraws));
                    g_drops = Drops{};
                }
            }
        }
    }

    void setFrameQueueHost(const FrameQueueHost &host) { g_host = host; }

    void recycleFrameBuffers(Frame &frame)
    {
        // (frames in flight: the one being read, one drawn, one waiting - and, TODO 383, the list read's expanded vector
        // taken from the same kept ones and the capture's given back: one more)
        constexpr size_t kKeep = 4;
        if (frame.vuChunks.capacity() > 0)
        {
            frame.vuChunks.clear();
            std::lock_guard<std::mutex> lock(g_spareMutex);
            if (g_spareChunks.size() < kKeep)
                g_spareChunks.push_back(std::move(frame.vuChunks));
        }
        if (frame.draws.capacity() == 0 && frame.list.capacity() == 0)
            return;
        frame.draws.clear(); // (each draw's meshes and textures let go of on this thread)
        frame.list.clear();
        std::lock_guard<std::mutex> lock(g_spareMutex);
        if (frame.draws.capacity() > 0 && g_spareDraws.size() < kKeep)
            g_spareDraws.push_back(std::move(frame.draws));
        if (frame.list.capacity() > 0 && g_spareLists.size() < kKeep)
            g_spareLists.push_back(std::move(frame.list));
    }

    void takeFrameBuffers(std::vector<Draw> &draws, std::vector<uint32_t> &list)
    {
        std::lock_guard<std::mutex> lock(g_spareMutex);
        const auto take = [](auto &spares, auto &into) {
            if (spares.empty() || into.capacity() > 0)
                return;
            auto best = spares.begin();
            for (auto it = spares.begin(); it != spares.end(); ++it)
                if (it->capacity() > best->capacity())
                    best = it;
            into = std::move(*best);
            spares.erase(best);
        };
        take(g_spareDraws, draws);
        take(g_spareLists, list);
    }

    // (TF_NATIVE_KEEPDRAWS=0: a fresh vector for each frame's expanded draws, as before - the A/B of TODO 383's faults)
    bool keepDraws()
    {
        static const bool on = !(std::getenv("TF_NATIVE_KEEPDRAWS") && *std::getenv("TF_NATIVE_KEEPDRAWS") == '0');
        return on;
    }

    void takeSpareDraws(std::vector<Draw> &draws)
    {
        if (!keepDraws())
            return;
        std::lock_guard<std::mutex> lock(g_spareMutex);
        if (g_spareDraws.empty() || draws.capacity() > 0)
            return;
        auto best = g_spareDraws.begin();
        for (auto it = g_spareDraws.begin(); it != g_spareDraws.end(); ++it)
            if (it->capacity() > best->capacity())
                best = it;
        draws = std::move(*best);
        g_spareDraws.erase(best);
    }

    void giveSpareDraws(std::vector<Draw> &&draws)
    {
        if (draws.capacity() == 0 || !keepDraws())
            return;
        draws.clear(); // (the draws' meshes and textures let go of here, on the calling thread)
        std::lock_guard<std::mutex> lock(g_spareMutex);
        if (g_spareDraws.size() < 4u)
            g_spareDraws.push_back(std::move(draws));
    }

    void takeChunkBuffer(std::vector<uint32_t> &chunks)
    {
        std::lock_guard<std::mutex> lock(g_spareMutex);
        if (g_spareChunks.empty() || chunks.capacity() > 0)
            return;
        chunks = std::move(g_spareChunks.back());
        g_spareChunks.pop_back();
    }

    double gameWaitMs() { return static_cast<double>(g_gameWaitNs.load(std::memory_order_relaxed)) / 1e6; }

    namespace
    {
        // The queue's two threads, started by the first frame (under g_mutex).
        void startThreads()
        {
            if (g_started)
                return;
            g_started = true;
            std::thread(readLoop).detach();
            std::thread(drawLoop).detach();
        }
    }

    void queueFrame(Frame &&frame, const std::string &shot)
    {
        const int64_t handedNs = latLogSeconds() ? nowNs() : 0; // (TODO 169: TF_NATIVE_LATLOG)
        std::unique_lock<std::mutex> lock(g_mutex);
        startThreads();
        // The frame before read from guest memory first: its packet buffer is the one the game
        // builds the next frame in (double buffered). Then two frames waiting at most.
        // (TODO 161 a: and, every frame drawn - TF_FRAME_DROPS=0 or the rate following the PC - the frame before taken to be drawn,
        // as when one thread read and drew; else at most 16 frames read ahead of the drawing - a renderer held up for long)
        const auto room = [] { return g_jobs.empty() && !g_reading && (framesDropped() ? g_ready.size() < 16u : g_ready.empty()); };
        if (!room())
        {
            // (TODO 112: the game's thread held back by the renderer - TF_HITCH_MS's "waited for the renderer")
            const auto since = std::chrono::steady_clock::now();
            g_room.wait(lock, room);
            g_gameWaitNs.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - since).count()),
                                   std::memory_order_relaxed);
        }
        Job job{std::move(frame), shot};
        if (latLogSeconds())
            job.submitNs = handedNs, job.inputNs = g_host.inputNs ? g_host.inputNs() : 0;
        g_jobs.push_back(std::move(job));
        g_cv.notify_one();
    }

    // (nr_core S10) A submitted frame: in line behind every frame handed over before it (S11: through g_jobs - the read thread
    // passes it on unread, so it cannot overtake a frame still being read; S10 put it straight into g_ready). The game's thread
    // waits while `waiting` submitted frames already wait to be drawn.
    void queueSubmitted(Job &&job, unsigned waiting)
    {
        std::unique_lock<std::mutex> lock(g_mutex);
        startThreads();
        if (g_submitWaiting >= waiting)
        {
            const auto since = std::chrono::steady_clock::now();
            g_room.wait(lock, [waiting] { return g_submitWaiting < waiting; });
            g_gameWaitNs.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - since).count()),
                                   std::memory_order_relaxed);
        }
        ++g_submitWaiting;
        ++g_submitInFlight;
        g_jobs.push_back(std::move(job));
        lock.unlock();
        g_cv.notify_one();
    }

    void waitSubmittedDrawn()
    {
        std::unique_lock<std::mutex> lock(g_mutex);
        g_room.wait(lock, [] { return g_submitInFlight == 0u; });
    }

    unsigned long long framesLeftOutBehind() { return g_behindTotal.load(std::memory_order_relaxed); }

    double drawnFrameMs(double share, unsigned frames) { return timeAt(g_drawnTimes, share, frames); }
    double gpuFrameMs(double share, unsigned frames) { return timeAt(g_gpuTimes, share, frames); }
    void noteGpuFrame(double ms)
    {
        if (ms > 0.0 && ms < 1000.0)
            noteTime(g_gpuTimes, ms);
    }

    // (TODO 406) Right before a state is loaded (EeScheduler's before-load hook, on the EE thread, the frames' only giver):
    // the frames handed to the reader read to the end first. The reader reads a frame's display lists from guest memory
    // while the game runs on; a load overwrote that memory under it, and the frame read came out half old, half loaded -
    // thousands of draws into GS buffers that do not exist (37 new full-size targets in one frame, a GPU hang).
    void finishReadsBeforeLoad()
    {
        static const bool off = std::getenv("TF_NATIVE_LOADWAIT") && *std::getenv("TF_NATIVE_LOADWAIT") == '0'; // (as before)
        std::unique_lock<std::mutex> lock(g_mutex);
        if (!g_started || off)
            return;
        const auto since = std::chrono::steady_clock::now();
        const bool done = g_room.wait_for(lock, std::chrono::seconds(3), [] { return g_jobs.empty() && !g_reading; });
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count();
        if (!done)
            std::fprintf(stderr, "[native render] warning: a frame still being read when the state loads (waited %.0f ms)\n", ms);
        else if (ms >= 1.0)
            std::fprintf(stderr, "[native render] the frames being read finished before the state loads (%.1f ms)\n", ms);
    }
}
