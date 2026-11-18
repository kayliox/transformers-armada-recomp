// The native renderer's own thread (README.md): the capture (on the game's thread) hands each
// finished frame over and goes on with the game; here the frame's remaining CPU work - the grass
// blades grown from the ground's draws (screen space, every frame), the passes, the D3D12
// recording and submission - is done. Two threads (TODO 161 a): one reads each frame's list from guest memory as soon
// as it comes (the game waits for that alone), the other draws; with the levels' fixed logic rate a frame the drawing
// has not reached when a newer one is read is left out (its picture, never the game's time). Following the PC's rate
// (or TF_FRAME_DROPS=0) a faster game waits for the drawing rather than piling frames up, as before. TF_NATIVE_THREAD=0
// draws on the game's thread as before.
#include "nr_types.h"
#include "nr_movie.h" // (nr_core S5: the texture host's movie question)
#include "nr_frame.h" // (frame::g_bloom: TF_NATIVE_SHOTGLOW, TODO 108)
#include "nr_backend.h"
#include "nr_shader.h"
#include "nr_n5.h"
#include "nr_taa.h"
#include "view_turn.h"
#include "runtime/ps2_thread_placement.h"
#include "nr_core.h"
#include "ps2_runtime.h"
#include "nr_queue.h"
#include "nr_pictures.h"
#include "runtime/ps2_memory.h"
#include "runtime/gs/gs_threaded_backend.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
// (kernel32, declared here: windows.h's macros clash with names in the runtime headers)
extern "C" __declspec(dllimport) void *__stdcall GetCurrentThread();
extern "C" __declspec(dllimport) long __stdcall SetThreadDescription(void *thread, const wchar_t *description);
#endif

namespace tf::port
{
    bool posePictures(); // (overrides.cpp: the setting "Smooth pictures between frames", TF_POSES)
    bool nativePictures(); // (overrides.cpp: the window shows the native renderer's pictures, one a frame)
    int pictureCap();      // (overrides.cpp, TODO 161 a: the frame rate cap as a cap of pictures - 0: none)
    int fixedLogicRate();  // (overrides.cpp, TODO 161 a: the levels' fixed logic rate - 0: the rate follows the PC)
    bool vsyncOff();       // (overrides.cpp, TODO 266: the settings' VSync off - the presents at once)
    int frameCap();        // (overrides.cpp, TODO 266: the settings' frame rate cap - 0: Unlimited)
}

namespace nr
{
    void drawQueuedFrame(Job &job);
}

// (nr_core S0) The core's threads are placed by the runtime's rule (PS2X_THREAD_PLACEMENT): the hook set before main.
namespace
{
    const bool g_coreThreadPlacer = []
    {
        nr_core::setThreadPlacer([](nr_core::ThreadRole role)
                                 { ps2_thread_placement::place(static_cast<ps2_thread_placement::Role>(static_cast<int>(role))); });
        return true;
    }();
    // (nr_core S1b) The texture conversion's host: guest RAM and the GS upload skip's bookkeeping (PS2Recomp's gsupload).
    const bool g_coreTextureHost = []
    {
        nr_core::TextureHost host;
        host.guestPtr = [](const uint8_t *ram, uint32_t addr) -> const uint8_t * { return getConstMemPtr(ram, addr); };
        host.ensureWhole = [](uint64_t first, uint64_t last) { gsupload::ensureWhole(first, last); };
        host.checkMode = []() { return gsupload::mode() == gsupload::Mode::Check; };
        host.staleAt = [](uint32_t address, bool topByte) { return gsupload::staleAt(address, topByte); };
        host.reportRead = [](const char *what, uint32_t tbp, uint32_t psm, uint32_t staleBytes, uint32_t bytes)
        { gsupload::reportRead(what, tbp, psm, staleBytes, bytes); };
        host.skipsSnapshot = [](uint32_t data) { return nr::movie::skipsSnapshot(data); }; // (nr_core S5)
        host.censusNote = [](uint32_t kind, uint64_t amount) { nr::censusNote(kind, amount); };
        nr_core::setTextureHost(host);
        return true;
    }();
    // (nr_core S3a) The frame queue's host (nr_queue.h): a frame's list read from guest memory, its drawing (the grass, the
    // pictures between frames), the pictures' moment carried over a frame left out, the frame rate cap, the last pad read.
    const bool g_coreFrameQueueHost = []
    {
        nr::FrameQueueHost host;
        host.read = [](nr::Job &job) { nr::expandFrame(job.frame); };
        host.draw = [](nr::Job &job) { nr::drawQueuedFrame(job); };
        host.pictureCap = []() { return tf::port::pictureCap(); };
        host.inputNs = []() -> int64_t { return nr::n5::lastInputNs(); };
#if !defined(__linux__)
        host.windowBehind = []() { return nr::d3d::windowBehind(); }; // (TODO 420: the CPU hand-over's window behind)
#endif
        nr::setFrameQueueHost(host);
        return true;
    }();
    // (nr_core S3b) The pictures between frames' host (nr_pictures.h): the settings' answers and the camera turn's.
    const bool g_corePicturesHost = []
    {
        nr::PicturesHost host;
        host.posePictures = []() { return tf::port::posePictures(); };
        host.nativePictures = []() { return tf::port::nativePictures(); };
        host.pictureCap = []() { return tf::port::pictureCap(); };
        host.vsyncOff = []() { return tf::port::vsyncOff(); };
        host.frameCap = []() { return tf::port::frameCap(); };
        host.cameraCut = [](const view_turn::FrameCamera &fc) { return view_turn::cameraCut(fc); };
        host.turnable = [](const view_turn::FrameCamera &fc) { return view_turn::turnable(fc); };
        host.turnEnabled = []() { return view_turn::enabled(); };
        host.turnPicturesWanted = []() { return view_turn::turnPicturesWanted(); };
        nr::setPicturesHost(host);
        return true;
    }();
}

namespace nr
{
    namespace
    {

        // The frame's display list replayed into the VU image (the shaders' data blocks, field A's
        // matrix) up to each 3D draw, whose shader state is taken there, then the rest of it (the
        // image goes on into the next frame, as the VU's memory does).
        void replay(Frame &frame)
        {
            size_t at = 0;
            for (Draw &d : frame.draws)
            {
                if (!d.needsShader)
                    continue;
                d.needsShader = false;
                const size_t end = std::min<size_t>(d.listEnd, frame.list.size());
                if (end > at)
                {
                    replayList(frame.list.data() + at, end - at);
                    at = end;
                }
                snapshotShader(d.shader);
                fieldAMatrix(d.matrix); // (when one was seen: else the one the capture sent)
                finishDraw(d);
            }
            if (frame.list.size() > at)
                replayList(frame.list.data() + at, frame.list.size() - at);
            frame.list.clear();
        }

        // The ground's grass draws grown into blades (what the capture left for here). (TODO 94) Each ground's blades are its
        // own - its triangles and its block - so they are grown on several threads at once, the draws' ids given out in their
        // order first (the same blades, the same ids as one after the other): in Deep Amazon replayed the grass was 12% of the
        // render thread's time, twice Amazon's. TF_GRASS_THREADS=<n> (default: the processor's threads less 4, 2 to 6; 1 =
        // on the render thread alone, as before).
        struct GrassJob
        {
            Draw *d;
            const Qword *grass, *drift;
            uint64_t id;
            bool gpu = false, check = false; // (grassgpu: grown on the GPU - and on the CPU too, for the check)
        };

        void growOne(const GrassJob &j)
        {
            Draw &d = *j.d;
            d.mesh = j.gpu ? grassBladesGpu(d.mesh, j.grass, j.id, j.check)
                           : j.grass ? grassBlades(*d.mesh, j.grass, j.id) : driftStreaks(*d.mesh, j.drift, j.id);
            const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            std::memcpy(d.matrix, identity, sizeof(identity));
            d.shader = ShaderState{};
        }

        struct GrassPool
        {
            std::mutex mutex;
            std::condition_variable cv, done;
            const std::vector<GrassJob> *jobs = nullptr;
            std::atomic<size_t> next{0};
            int acked = 0; // (helpers done with this round)
            uint64_t round = 0;
            bool started = false;
        };

        void takeGrass(GrassPool &p)
        {
            for (size_t i; (i = p.next.fetch_add(1, std::memory_order_relaxed)) < p.jobs->size();)
                growOne((*p.jobs)[i]);
        }

        void growGrass(Frame &frame)
        {
            static uint64_t nextId = 1ull << 62; // (ids of the renderer's own: apart from the capture's)
            using Job = GrassJob;
            thread_local std::vector<Job> jobs;
            jobs.clear();
            // (grassgpu) The blades grown on the GPU (nr_d3d.cpp's grass compute pass) - the render thread then makes none:
            // TF_GRASS_GPU=0 grows them here, as before. Not while pictures between frames blend the blades from the frame
            // before's (TF_POSES with its vertices: they need the blades on the CPU). TF_GRASS_GPU_CHECK=<frames>: those
            // frames' blades grown both ways and compared (nr_d3d.cpp).
            static const bool gpuOn = !(std::getenv("TF_GRASS_GPU") && *std::getenv("TF_GRASS_GPU") == '0');
            static int gpuCheck = std::getenv("TF_GRASS_GPU_CHECK") ? std::atoi(std::getenv("TF_GRASS_GPU_CHECK")) : 0;
            static const bool posesVertices = !(std::getenv("TF_POSES_VERTICES") && *std::getenv("TF_POSES_VERTICES") == '0');
            const bool gpu = gpuOn && backend().grassGpuReady() && !(tf::port::posePictures() && posesVertices);
            bool checkThis = false;
            for (Draw &d : frame.draws)
            {
                if (!d.grassPending || !d.mesh)
                    continue;
                d.grassPending = false;
                const Qword *grass = d.shader.blockOf(Routine::Grass);
                const Qword *drift = grass ? nullptr : d.shader.blockOf(Routine::Drift);
                if (grass || drift)
                {
                    jobs.push_back(Job{&d, grass, drift, nextId++});
                    if (gpu && grass)
                    {
                        jobs.back().gpu = true;
                        jobs.back().check = gpuCheck > 0;
                        checkThis = checkThis || gpuCheck > 0;
                    }
                }
            }
            if (checkThis)
                --gpuCheck;
            // (the GPU's own first: next to nothing here; what is left - the drift - on the threads below)
            {
                size_t keep = 0;
                for (size_t k = 0; k < jobs.size(); ++k)
                    if (jobs[k].gpu)
                        growOne(jobs[k]);
                    else
                        jobs[keep++] = jobs[k];
                jobs.resize(keep);
            }
            static const int threads = [] {
                if (const char *v = std::getenv("TF_GRASS_THREADS"))
                    return std::max(1, std::atoi(v));
                return std::clamp(static_cast<int>(std::thread::hardware_concurrency()) - 4, 2, 6);
            }();
            if (threads <= 1 || jobs.size() < 2u)
            {
                for (const Job &j : jobs)
                    growOne(j);
                return;
            }
            // (TF_GRASS_CHECK=<frames>: those frames' blades grown again one after the other and compared, bit for bit)
            static int check = std::getenv("TF_GRASS_CHECK") ? std::atoi(std::getenv("TF_GRASS_CHECK")) : 0;
            std::vector<std::shared_ptr<const Mesh>> grounds;
            std::vector<std::vector<Qword>> blocks; // (the draws' shader data, copied: growOne lets it go)
            std::vector<size_t> at;               // (where the job's block lies in it)
            if (check > 0)
                for (const Job &j : jobs)
                {
                    grounds.push_back(j.d->mesh);
                    blocks.push_back(j.d->shader.data);
                    at.push_back(static_cast<size_t>((j.grass ? j.grass : j.drift) - j.d->shader.data.data()));
                }
            // (a few helpers of its own, waiting between frames; the render thread takes jobs too)
            static GrassPool pool;
            {
                std::lock_guard<std::mutex> lock(pool.mutex);
                if (!pool.started)
                {
                    pool.started = true;
                    for (int k = 0; k < threads - 1; ++k)
                        std::thread([] {
#if defined(_WIN32)
                            SetThreadDescription(GetCurrentThread(), L"NativeGrass");
#endif
                            uint64_t seen = 0;
                            for (;;)
                            {
                                {
                                    std::unique_lock<std::mutex> lock(pool.mutex);
                                    pool.cv.wait(lock, [&] { return pool.round != seen; });
                                    seen = pool.round;
                                }
                                takeGrass(pool); // (the jobs stay until every helper has said it is done with this round)
                                std::lock_guard<std::mutex> lock(pool.mutex);
                                ++pool.acked;
                                pool.done.notify_all();
                            }
                        }).detach();
                }
                pool.jobs = &jobs;
                pool.next.store(0, std::memory_order_relaxed);
                pool.acked = 0;
                ++pool.round;
            }
            pool.cv.notify_all();
            takeGrass(pool);
            std::unique_lock<std::mutex> lock(pool.mutex);
            pool.done.wait(lock, [&] { return pool.acked == threads - 1; });
            pool.jobs = nullptr;
            lock.unlock();
            if (check > 0 && grounds.size() == jobs.size())
            {
                --check;
                size_t differ = 0, blades = 0;
                for (size_t k = 0; k < jobs.size(); ++k)
                {
                    const Job &j = jobs[k];
                    const Qword *block = blocks[k].data() + at[k];
                    const std::shared_ptr<Mesh> again = j.grass ? grassBlades(*grounds[k], block, j.id) : driftStreaks(*grounds[k], block, j.id);
                    const Mesh &got = *j.d->mesh;
                    blades += got.vertices.size() / 4u;
                    const bool same = again->vertices.size() == got.vertices.size() && again->indices == got.indices &&
                                      std::memcmp(again->vertices.data(), got.vertices.data(), got.vertices.size() * sizeof(Vertex)) == 0;
                    differ += !same;
                }
                std::fprintf(stderr, "[native render] grass on %d threads: %zu grounds, %zu blades, %zu differ from one thread's\n", threads, jobs.size(), blades, differ);
            }
        }


        // A frame drawn (the queue's render thread; its times are the queue's, nr_queue.cpp drawOne): the grass grown, the
        // pictures between frames (or a checking aid's shots).
        void drawJob(Job &job)
        {
            const auto start = std::chrono::steady_clock::now();
            growGrass(job.frame);
            // (the drawing thread's own time: the list is read on its own thread now, beside the drawing - counted here it took
            // the pictures between frames down to one a frame for seconds at a time, at 120 on this PC's 240 Hz)
            notePicturesPreMs(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            static const bool shotAb = std::getenv("TF_NATIVE_SHOTAB") != nullptr;
            static const bool shotGlow = std::getenv("TF_NATIVE_SHOTGLOW") != nullptr;
            static const bool shotTone = std::getenv("TF_NATIVE_SHOTTONE") != nullptr;
            static const bool shotClarity = std::getenv("TF_NATIVE_SHOTCLARITY") != nullptr;
            if (shotClarity && !job.shot.empty())
            {
                // (TODO 108 C1, a checking aid: TF_NATIVE_SHOTCLARITY=<s>,<s>... - the frame at each strength, _cs0 _cs1 ...;
                // 0 off.)
                const size_t dot = job.shot.rfind('.');
                const std::string stem = dot == std::string::npos ? job.shot : job.shot.substr(0, dot);
                const std::string ext = dot == std::string::npos ? std::string(".bmp") : job.shot.substr(dot);
                const int was = frame::clarityMode();
                const float wasStrength = frame::clarityStrength();
                const std::string list = std::getenv("TF_NATIVE_SHOTCLARITY");
                size_t at = 0;
                for (int n = 0; at <= list.size(); ++n)
                {
                    size_t end = list.find(',', at);
                    if (end == std::string::npos)
                        end = list.size();
                    const float s = static_cast<float>(std::atof(list.substr(at, end - at).c_str()));
                    frame::g_clarity.store(s > 0.0f ? 1 : 0, std::memory_order_relaxed);
                    frame::setClarityStrength(s);
                    backend().render(job.frame, stem + "_cs" + std::to_string(n) + ext);
                    at = end + 1;
                }
                frame::g_clarity.store(was, std::memory_order_relaxed);
                frame::setClarityStrength(wasStrength);
            }
            else if (shotTone && !job.shot.empty())
            {
                // (TODO 108 (2), a checking aid: the frame drawn without and with the tone mapping - _t0 _t1.)
                const size_t dot = job.shot.rfind('.');
                const std::string stem = dot == std::string::npos ? job.shot : job.shot.substr(0, dot);
                const std::string ext = dot == std::string::npos ? std::string(".bmp") : job.shot.substr(dot);
                const int was = frame::toneMode();
                const float wasStrength = frame::toneStrength();
                // (TF_NATIVE_SHOTTONE=<s>,<s>...: the frame at each strength, _ts0 _ts1 ... - 0 off; else _t0 _t1, off and on)
                const std::string list = std::getenv("TF_NATIVE_SHOTTONE");
                if (list.find(',') != std::string::npos)
                {
                    size_t at = 0;
                    for (int n = 0; at <= list.size(); ++n)
                    {
                        size_t end = list.find(',', at);
                        if (end == std::string::npos)
                            end = list.size();
                        const float s = static_cast<float>(std::atof(list.substr(at, end - at).c_str()));
                        frame::g_tone.store(s > 0.0f ? 1 : 0, std::memory_order_relaxed);
                        frame::setToneStrength(s);
                        backend().render(job.frame, stem + "_ts" + std::to_string(n) + ext);
                        at = end + 1;
                    }
                }
                else
                    for (int m = 0; m < 2; ++m)
                    {
                        frame::g_tone.store(m, std::memory_order_relaxed);
                        backend().render(job.frame, stem + "_t" + std::to_string(m) + ext);
                    }
                frame::g_tone.store(was, std::memory_order_relaxed);
                frame::setToneStrength(wasStrength);
            }
            else if (shotGlow && !job.shot.empty())
            {
                // (TODO 108, a checking aid: the frame drawn with the game's glow, the modern bloom and no glow - the same
                // input, three shots _g0 _g1 _g2.)
                const size_t dot = job.shot.rfind('.');
                const std::string stem = dot == std::string::npos ? job.shot : job.shot.substr(0, dot);
                const std::string ext = dot == std::string::npos ? std::string(".bmp") : job.shot.substr(dot);
                const int was = frame::bloomMode();
                const float wasStrength = frame::bloomStrength();
                // (TF_NATIVE_SHOTGLOW=<s>,<s>...: the game's glow _g0, then the bloom at each strength _gs0 _gs1 ...)
                const std::string list = std::getenv("TF_NATIVE_SHOTGLOW");
                if (list.find(',') != std::string::npos)
                {
                    frame::g_bloom.store(0, std::memory_order_relaxed);
                    backend().render(job.frame, stem + "_g0" + ext);
                    size_t at = 0;
                    for (int n = 0; at <= list.size(); ++n)
                    {
                        size_t end = list.find(',', at);
                        if (end == std::string::npos)
                            end = list.size();
                        frame::g_bloom.store(1, std::memory_order_relaxed);
                        frame::setBloomStrength(static_cast<float>(std::atof(list.substr(at, end - at).c_str())));
                        backend().render(job.frame, stem + "_gs" + std::to_string(n) + ext);
                        at = end + 1;
                    }
                }
                else
                    for (int m = 0; m < 3; ++m)
                    {
                        frame::g_bloom.store(m, std::memory_order_relaxed);
                        backend().render(job.frame, stem + "_g" + std::to_string(m) + ext);
                    }
                frame::g_bloom.store(was, std::memory_order_relaxed);
                frame::setBloomStrength(wasStrength);
            }
            else if (shotAb && !job.shot.empty())
            {
                // (TODO 94: the frame drawn with the picture-keeping speed-ups off, then on - the same input, two shots.)
                const size_t dot = job.shot.rfind('.');
                const std::string stem = dot == std::string::npos ? job.shot : job.shot.substr(0, dot);
                const std::string ext = dot == std::string::npos ? std::string(".bmp") : job.shot.substr(dot);
                {
                    nr_core::Settings coreSettings;
                    coreSettings.speedupsForced = 0;
                    nr_core::applySettings(coreSettings);
                }
                backend().render(job.frame, stem + "_a" + ext);
                {
                    nr_core::Settings coreSettings;
                    coreSettings.speedupsForced = 1;
                    nr_core::applySettings(coreSettings);
                }
                backend().render(job.frame, stem + "_b" + ext);
                backend().setSpeedupsForced(-1);
            }
            else
                renderPictures(job.frame, job.shot);
        }
    }

    // TF_NATIVE_THREAD: unset = each frame read from guest memory and drawn on the renderer's thread; 0 = the same reading
    // and drawing (expandFrame, the grass, the pictures) on the game's thread at the frame's end, before the game writes
    // the next one - the A/B for the renderer thread's reads of memory the game has moved on from (TODO 149: before, 0
    // was the older path below, which drew differently - the near foliage blurrier - so it could not tell a race from its
    // own differences); old = that older path (each stretch read as the game sends it, drawn without the per-picture
    // camera).
    static const std::string &threadMode()
    {
        static const std::string mode = std::getenv("TF_NATIVE_THREAD") ? std::getenv("TF_NATIVE_THREAD") : "";
        return mode;
    }

    bool renderOnThread()
    {
        static const bool on = !(threadMode() == "0" || threadMode() == "old");
        return on;
    }

    bool framesDeferred()
    {
        static const bool on = threadMode() != "old";
        return on;
    }

    void submitFrame(Frame &&frame, const std::string &shot)
    {
        if (!renderOnThread())
        {
            if (framesDeferred())
            {
                // (TF_NATIVE_THREAD=0: the renderer thread's work, here and now - run()'s steps)
                expandFrame(frame);
                growGrass(frame);
                renderPictures(frame, shot);
                return;
            }
            growGrass(frame);
            backend().render(frame, shot);
            return;
        }
        queueFrame(std::move(frame), shot); // (nr_queue.cpp: the core's frame queue and its two threads)
    }

    void drawQueuedFrame(Job &job) { drawJob(job); }
}
