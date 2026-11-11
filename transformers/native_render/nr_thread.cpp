// The native renderer's own thread (README.md): the capture (on the game's thread) hands each
// finished frame over and goes on with the game; here the frame's remaining CPU work - the grass
// blades grown from the ground's draws (screen space, every frame), the passes, the D3D12
// recording and submission - is done. Two threads (TODO 161 a): one reads each frame's list from guest memory as soon
// as it comes (the game waits for that alone), the other draws; with the levels' fixed logic rate a frame the drawing
// has not reached when a newer one is read is left out (its picture, never the game's time). Following the PC's rate
// (or TF_FRAME_DROPS=0) a faster game waits for the drawing rather than piling frames up, as before. TF_NATIVE_THREAD=0
// draws on the game's thread as before.
#include "nr_types.h"
#include "nr_frame.h" // (frame::g_bloom: TF_NATIVE_SHOTGLOW, TODO 108)
#include "nr_backend.h"
#include "nr_shader.h"
#include "nr_n5.h"
#include "view_turn.h"
#include "runtime/ps2_thread_placement.h"

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
    namespace
    {
        struct Job
        {
            Frame frame;
            std::string shot;
            double readMs = 0.0; // (TODO 161 a: its list read, readLoop)
            int64_t inputNs = 0, submitNs = 0, readStartNs = 0, readEndNs = 0; // (TODO 169: TF_NATIVE_LATLOG's steps, steady clock)
        };

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
        std::atomic<uint64_t> g_gameWaitNs{0}; // (TODO 112) the game's thread's waits in submitFrame, all of them
        // (TODO 94) Frames' draws and lists given back by the renderer, for the game's next frames (recycleFrameBuffers).
        std::mutex g_spareMutex;
        std::vector<std::vector<Draw>> g_spareDraws;
        std::vector<std::vector<uint32_t>> g_spareLists;
        std::vector<std::vector<uint32_t>> g_spareChunks; // (N5 step 2 part 2: Frame::vuChunks)

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

        // ---- N9 stage 2: pictures between the game's frames ---------------------------------------------------
        // The game's logic runs at a rate it holds (the ladder's, overrides.cpp); the screen refreshes faster. Each
        // 3D draw carries two poses: field A's matrix (Draw::matrix, this frame) and in its core's block (0x1410's
        // q0-q3, the lit metal's 0x2800) the in-between field's - the frame before's pose with the in-between factor
        // at 0 (tf::port::oneFieldFactor; Frame::fieldBMoment says where it lies). So every picture between two frames
        // is the GPU's to draw: here a frame is drawn once for each refresh of the screen its moments cover (the frame
        // before's moment to its own), each draw's matrix the two poses blended for that picture's moment, and each
        // picture handed to the window at its moment plus the pacer's steady delay (nr_d3d.cpp pace::) - so they reach
        // the screen as evenly as the refreshes. Draws the game gives no field B (the forest's instances, sprites,
        // the grass grown here, the ocean) move with the camera: by the frame's camera motion on their target - the
        // field A -> field B change the world's own draws there agree on (a static mesh's B = A x D, D the same for
        // all of them). The 2D (HUD, menus, text) is drawn as it is.
        namespace poses
        {
            using M4 = std::array<float, 16>;

            M4 mul(const float *a, const float *b)
            {
                M4 r{};
                for (int i = 0; i < 4; ++i)
                    for (int j = 0; j < 4; ++j)
                    {
                        float v = 0.0f;
                        for (int k = 0; k < 4; ++k)
                            v += a[4 * i + k] * b[4 * k + j];
                        r[4 * i + j] = v;
                    }
                return r;
            }

            bool invert(const float *m, float *out)
            {
                double a[16];
                for (int i = 0; i < 16; ++i)
                    a[i] = m[i];
                double inv[16];
                inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
                inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
                inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
                inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
                inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
                inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
                inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
                inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
                inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
                inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
                inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
                inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
                inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
                inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
                inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
                inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
                const double det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
                // (singular against the rows' own lengths - the columns' scales differ by orders: depth in GS Z units)
                double bound = 1.0;
                for (int r = 0; r < 4; ++r)
                    bound *= std::sqrt(a[4 * r] * a[4 * r] + a[4 * r + 1] * a[4 * r + 1] + a[4 * r + 2] * a[4 * r + 2] + a[4 * r + 3] * a[4 * r + 3]);
                if (!std::isfinite(det) || !(std::fabs(det) > 1e-9 * bound))
                    return false;
                for (int i = 0; i < 16; ++i)
                    out[i] = static_cast<float>(inv[i] / det);
                return true;
            }

            bool invertD(const float *m, double *out)
            {
                float f[16];
                if (!invert(m, f))
                    return false;
                // (one Newton step in double from the float inverse: X = X (2I - M X))
                double x[16], mx[16];
                for (int i = 0; i < 16; ++i)
                    x[i] = f[i];
                for (int i = 0; i < 4; ++i)
                    for (int j = 0; j < 4; ++j)
                    {
                        double v = 0.0;
                        for (int q = 0; q < 4; ++q)
                            v += static_cast<double>(m[4 * i + q]) * x[4 * q + j];
                        mx[4 * i + j] = (i == j ? 2.0 : 0.0) - v;
                    }
                for (int i = 0; i < 4; ++i)
                    for (int j = 0; j < 4; ++j)
                    {
                        double v = 0.0;
                        for (int q = 0; q < 4; ++q)
                            v += x[4 * i + q] * mx[4 * q + j];
                        out[4 * i + j] = v;
                    }
                return true;
            }

            double relDiff(const float *a, const float *b)
            {
                double worst = 0.0, scale = 1e-30;
                for (int i = 0; i < 16; ++i)
                {
                    worst = std::max(worst, std::fabs(static_cast<double>(a[i]) - b[i]));
                    scale = std::max(scale, std::fabs(static_cast<double>(b[i])));
                }
                return worst / scale;
            }

            // A draw's field B block (0x1410's - found by its address: the levels' images are not the one
            // Routine::TransformKick is told by - or the lit metal's 0x2800).
            int fieldBAt(const Draw &d)
            {
                for (uint32_t k = 0; k < d.shader.count; ++k)
                    if ((d.shader.address[k] == 0x1410u || d.shader.address[k] == 0x2800u || d.shader.routine[k] == Routine::TransformKick ||
                         d.shader.routine[k] == Routine::LitMeshDraw) &&
                        d.shader.first[k] + 4u <= d.shader.data.size())
                        return static_cast<int>(d.shader.first[k]);
                return -1;
            }

            // The core's clipping matrix (0x0480 after the core: nr_passes.cpp basePass's rule), -1 none.
            int clipAt(const Draw &d)
            {
                bool core = false;
                for (uint32_t k = 0; k < d.shader.count; ++k)
                {
                    if (d.shader.routine[k] == Routine::Instances)
                        return -1;
                    core = core || d.shader.address[k] == 0x1410u || d.shader.address[k] == 0x2800u || d.shader.address[k] == 0x2EC0u;
                    if (core && d.shader.address[k] == 0x0480u && d.shader.first[k] + 4u <= d.shader.data.size())
                        return static_cast<int>(d.shader.first[k]);
                }
                return -1;
            }

            // A target's buffer: FRAME's base and width, not its format (the grass is drawn into the scene's buffer as CT32,
            // the scene as CT24).
            uint32_t bufferOf(uint32_t target) { return target & 0x003F01FFu; }

            struct Saved
            {
                size_t draw = 0;
                M4 a{}, b{}, ainv{}, bGame{}; // (bGame: field B as the game gave it, before the carry)
                bool carriedB = false;        // (b: the field A the frame before, not the game's field B)
                bool gameB = false;           // (the game gave a field B that is a pose: bGame)
                bool hasB = false, ainvOk = false;
                int clip = -1;
                M4 c{};
                std::array<double, 16> k{}; // (A^-1 C in double: the clipping matrix of a pose m is m K)
                bool kOk = false;
                int instances = -1, instanceCount = 0;
                std::vector<M4> inst;
                std::shared_ptr<const Mesh> meshB; // (the vertex carry: the frame before's mesh of this kind)
            };

            struct State
            {
                std::vector<Saved> saved;
                std::unordered_map<uint32_t, M4> cameraDelta; // per target: D (field A -> field B for what does not move)
                int64_t prevDue = 0;
                double perPictureMs = 0.0; // (the render thread's time a picture, smoothed)
                double frameBaseMs = 0.0;  // (N9 stage 3: its time a frame, less the extra pictures - smoothed)
                double preMs = 0.0;        // (this frame's list read and grass, before its pictures: run())
                double singleMs = 0.0;     // (N9's turn pictures: the render thread's time a frame of one picture - smoothed)
                bool lastGpuLimits = false;
                // (N9 stage 3: the GPU's cap tried higher now and then - see render)
                bool probing = false;
                double probeN = 1.0, probeWait = 0.0, probeBackoff = 4.0, probeDelay = 0.0, proven = 0.0, waitMark = 0.0;
                bool probeForced = false; // (this frame's count is a try's: the refreshes between pictures set at once)
                std::chrono::steady_clock::time_point waitSince{};
                std::chrono::steady_clock::time_point probeUntil{}, nextProbe{};
                uint64_t probesKept = 0, probesFailed = 0;
                int refreshes = 1;         // (a fixed refresh: the refreshes a picture now)
                // (TODO 266: the menus' pictures tried one refresh closer now and then - see render)
                bool flatProbing = false;
                int flatProbeRefreshes = 0, flatProven = 0; // (the count being tried; the fewest refreshes a try kept, 0: none)
                double flatProbeWait = 0.0, flatProbeDelay = 0.0, flatBackoff = 4.0, flatWaitMark = 0.0;
                std::chrono::steady_clock::time_point flatProbeUntil{}, flatNextProbe{}, flatWaitSince{};
                // (the menus: the last frame of 2D alone - its 2D draws' meshes and states, in order)
                struct Flat
                {
                    std::shared_ptr<const Mesh> mesh;
                    uint64_t tex0 = 0;
                    uint32_t prim = 0;
                };
                std::vector<Flat> lastFlat;
                uint64_t flatFrames = 0, flatPictures = 0, flatUnmatched = 0;
                std::chrono::steady_clock::time_point roomSince{};
                // (TF_N9_CHECK) the poses checked against the frame before
                std::unordered_map<uint64_t, M4> lastA, nowA;
                std::unordered_map<uint64_t, M4> carryLast, carryNow; // (the carry: every draw's field A by mesh and place)
                // (the vertex carry: the meshes made anew each frame, by kind and place, with their field A)
                std::unordered_map<uint64_t, std::pair<std::shared_ptr<const Mesh>, M4>> vertexLast, vertexNow;
                uint64_t vertexCarried = 0, vertexJumped = 0, vertexNew = 0;
                // (the HUD's moving parts: the 2D over the 3D, by kind and place)
                std::unordered_map<uint64_t, std::shared_ptr<const Mesh>> hudLast, hudNow;
                uint64_t hudMoved = 0, hudStill = 0, hudOther = 0;
                std::unordered_map<uint64_t, uint32_t> countLast, countNow; // (and how many times each mesh was drawn)
                uint64_t carried = 0, notPose = 0, shared = 0, worldTaken = 0;
                uint64_t cuts = 0, cutsByTurn = 0;  // (TODO 101 b) frames shown as they are: a camera cut
                bool cutNow = false;
                uint64_t shrinkDraws = 0, shrinkOver[3] = {}; // (TODO 101 a, TF_N9_CHECK) halfway blends that shrink a part
                double shrinkMax = 0.0;
                uint64_t shrinkSnapped = 0, shrinkLinearOver = 0; // (TODO 101 a: turns over 60 degrees, not blended; the straight blend's over 1%)
                double shrinkLinearMax = 0.0;
                uint64_t checked = 0, near[3] = {}, moved = 0, withB = 0, without = 0, frames = 0, pictures = 0, bIsA = 0, bOther = 0, told = 0;
                std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
            } g_poses;

            bool wanted()
            {
                // (TF_POSES_ALT=<s>, a measuring aid: on and off by turns every that many seconds - an A/B within one run)
                static const int alt = std::getenv("TF_POSES_ALT") ? std::atoi(std::getenv("TF_POSES_ALT")) : 0;
                bool on = tf::port::posePictures();
                if (alt > 0 && on)
                {
                    static const auto t0 = std::chrono::steady_clock::now();
                    on = (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count() / alt) % 2 == 0;
                    static bool last = !on;
                    if (on != last)
                        std::fprintf(stderr, "[n9] pictures between frames %s (TF_POSES_ALT)\n", on ? "on" : "off");
                    last = on;
                }
                return on;
            }

            // The frame's two poses read once (each draw's A, B, the clipping matrix, the instances'), the camera's
            // motion per target from the draws that have both.
            void prepare(Frame &f, bool vertices = false)
            {
                State &st = g_poses;
                st.saved.clear();
                st.cameraDelta.clear();
                std::unordered_map<uint32_t, std::vector<M4>> deltas;
                uint64_t withBNow = 0, notPoseNow = 0; // (this frame's: the game's field B poses, those that are no pose)
                // Only the scene's buffer - the one most of the frame's 3D draws go to - is posed between frames. What is
                // drawn into a texture (the player's shadow, reflections, the glow's buffers) is read through this frame's
                // matrices (the shadow's receivers project it with field A's): drawn at field A, as it is (2026-10-30: the
                // shadow's strips carried between frames moved the shadow on the ground at every in-between picture).
                uint32_t scene = 0;
                {
                    std::unordered_map<uint32_t, uint32_t> count;
                    uint32_t most = 0;
                    for (const Draw &d : f.draws)
                        if (d.mesh && !d.mesh->immediate && ++count[bufferOf(d.target)] > most)
                            most = count[bufferOf(d.target)], scene = bufferOf(d.target);
                }
                for (size_t i = 0; i < f.draws.size(); ++i)
                {
                    Draw &d = f.draws[i];
                    // (the 2D is drawn as it is; the grass grown here - screen space, ids from 1 << 62 - moves with the camera)
                    if (!d.mesh || (d.mesh->immediate && d.mesh->id < (1ull << 62)) || bufferOf(d.target) != scene)
                        continue;
                    Saved sv;
                    sv.draw = i;
                    std::memcpy(sv.a.data(), d.matrix, 64u);
                    if (const int at = fieldBAt(d); at >= 0)
                    {
                        std::memcpy(sv.b.data(), d.shader.data[static_cast<size_t>(at)].v, 64u);
                        sv.hasB = true;
                        sv.bGame = sv.b;
                        ++st.withB;
                        ++withBNow;
                    }
                    else
                        ++st.without;
                    sv.ainvOk = invert(sv.a.data(), sv.ainv.data());
                    if ((sv.clip = clipAt(d)) >= 0)
                    {
                        std::memcpy(sv.c.data(), d.shader.data[static_cast<size_t>(sv.clip)].v, 64u);
                        double inv[16];
                        if ((sv.kOk = invertD(sv.a.data(), inv)))
                            for (int i = 0; i < 4; ++i)
                                for (int j = 0; j < 4; ++j)
                                {
                                    double v = 0.0;
                                    for (int q = 0; q < 4; ++q)
                                        v += inv[4 * i + q] * static_cast<double>(sv.c[4 * q + j]);
                                    sv.k[4 * i + j] = v;
                                }
                    }
                    if (const Qword *block = d.shader.blockOf(Routine::Instances))
                    {
                        const int first = static_cast<int>(block - d.shader.data.data());
                        const int count = std::max(0, (static_cast<int>(d.shader.data.size()) - first - 13) / 5);
                        sv.instances = first;
                        sv.instanceCount = count;
                        sv.inst.resize(static_cast<size_t>(count));
                        for (int k = 0; k < count; ++k)
                            std::memcpy(sv.inst[static_cast<size_t>(k)].data(), d.shader.data[static_cast<size_t>(first + 14 + 5 * k)].v, 64u);
                    }
                    st.saved.push_back(std::move(sv));
                }
                // The frame before's pose of each draw. Field B is it for the world (the camera's motion: 99% of the draws
                // within 1e-5 of the field A they had the frame before) but not quite for what moves on its own: measured
                // 2026-10-30 in Amazon, an enemy's B 22% of the way from where it was to where it is, the player's model's
                // B = A in the shadow pass - so it would step at the frame's turn. The field A the same mesh had the frame
                // before, in its place among the frame's draws of it (the carry), is that pose exactly: taken when it is
                // near (a moved piece, not another one: moved under 5%) and the mesh is drawn as many times as the frame
                // before (its places line up); when that count changed (an instance culled, one more drawn), only where it
                // agrees with the game's field B at least as well as this frame's does (the same piece). Measured in the
                // HQ (2026-10-30): the Autobot on the turntable has a field B far from where he was the frame before - he
                // jumped at every frame's turn until the carry took him whatever his field B said. A field B that is not a
                // pose (a block whose q0-q3 hold something else: all zero in one of Amazon's draws) is not used.
                // TF_POSES_CARRY=0: field B alone.
                static const bool carry = [] {
                    const char *v = std::getenv("TF_POSES_CARRY");
                    return !v || *v != '0';
                }();
                st.carryLast = std::move(st.carryNow);
                st.carryNow.clear();
                st.countLast = std::move(st.countNow);
                st.countNow.clear();
                for (const Saved &sv : st.saved)
                    ++st.countNow[f.draws[sv.draw].mesh->id];
                {
                    std::unordered_map<uint64_t, uint32_t> seen;
                    for (Saved &sv : st.saved)
                    {
                        const Draw &d = f.draws[sv.draw];
                        const uint64_t key = (d.mesh->id * 0x9E3779B97F4A7C15ull) ^ (static_cast<uint64_t>(seen[d.mesh->id]++) << 1) ^ 1u;
                        st.carryNow[key] = sv.a;
                        const double bOff = sv.hasB ? relDiff(sv.b.data(), sv.a.data()) : 1.0;
                        if (sv.hasB && !(bOff < 0.25))
                        {
                            sv.hasB = false; // (not a pose)
                            ++st.notPose;
                            ++notPoseNow;
                        }
                        sv.gameB = sv.hasB;
                        if (!carry)
                            continue;
                        const auto last = st.carryLast.find(key);
                        if (last == st.carryLast.end())
                            continue;
                        const double moved = relDiff(sv.a.data(), last->second.data());
                        if (!(moved < 0.05))
                            continue;
                        const auto lastCount = st.countLast.find(d.mesh->id);
                        const bool linedUp = lastCount != st.countLast.end() && lastCount->second == st.countNow[d.mesh->id];
                        if (!linedUp && sv.hasB && relDiff(last->second.data(), sv.b.data()) > 3.0 * std::max(bOff, 1e-6) + 1e-5)
                            continue; // (perhaps another piece of the same mesh: field B kept)
                        if (moved > 1e-6 && (!sv.hasB || relDiff(sv.b.data(), last->second.data()) > 1e-6))
                            ++st.carried;
                        sv.b = last->second;
                        sv.hasB = true;
                        sv.carriedB = true;
                    }
                }
                // The camera's motion on each target: the D most of its draws agree on (the static world's).
                for (const Saved &sv : st.saved)
                {
                    const Draw &d = f.draws[sv.draw];
                    // (from the game's own field B - the world's draws agree on the camera's motion - whether or not the
                    // carry took the frame before's field A for the draw itself)
                    if (sv.gameB && sv.ainvOk && deltas[bufferOf(d.target)].size() < 512u)
                        deltas[bufferOf(d.target)].push_back(mul(sv.ainv.data(), sv.bGame.data()));
                }
                for (auto &[target, list] : deltas)
                {
                    size_t best = 0, bestVotes = 0;
                    const size_t stride = std::max<size_t>(1u, list.size() / 48u); // (48 candidates, every draw a vote)
                    for (size_t i = 0; i < list.size(); i += stride)
                    {
                        size_t votes = 0;
                        for (size_t j = 0; j < list.size(); ++j)
                            if (relDiff(list[j].data(), list[i].data()) < 1e-3)
                                ++votes;
                        if (votes > bestVotes)
                            bestVotes = votes, best = i;
                    }
                    if (bestVotes >= 2u || list.size() == 1u)
                        st.cameraDelta[target] = list[best];
                }
                // (TODO 99) The static world's draws all posed by the game's own field B - the camera's motion, as the frame's
                // instances (the forest) are - not some of them by the carry: a world draw's field A of the frame before and the
                // game's field B differ now and then, and a ground piece the carry took was then drawn at another moment than the
                // trees and the ground around it - for one picture the near rocks and ground stood elsewhere (the flashes, at
                // draw distance 4). A draw is the static world's when its field B is its field A moved by the camera's motion.
                // (The camera's motion is worked out here, before: it reads the game's field B alone.) TF_POSES_WORLDB=0: off.
                {
                    static const bool worldB = !(std::getenv("TF_POSES_WORLDB") && *std::getenv("TF_POSES_WORLDB") == '0');
                    for (Saved &sv : st.saved)
                    {
                        if (!worldB || !sv.carriedB || !sv.gameB)
                            continue;
                        const auto camera = st.cameraDelta.find(bufferOf(f.draws[sv.draw].target));
                        if (camera == st.cameraDelta.end())
                            continue;
                        const M4 moved = mul(sv.a.data(), camera->second.data());
                        if (relDiff(moved.data(), sv.bGame.data()) < 1e-3)
                        {
                            sv.b = sv.bGame;
                            sv.carriedB = false;
                            ++st.worldTaken;
                        }
                    }
                }
                // Draws with the same matrix posed alike, bit for bit: what the game draws over the same geometry again (the
                // shadow's receivers over the ground, decals) tests depth for equality against it, and a difference in the
                // last bit of a pose took the shadow off the ground at every in-between picture (2026-10-30). The first of
                // them with a pose gives it to the rest.
                {
                    std::unordered_map<uint64_t, size_t> firstWith;
                    for (size_t k = 0; k < st.saved.size(); ++k)
                    {
                        Saved &sv = st.saved[k];
                        uint64_t h = 0xCBF29CE484222325ull;
                        const uint8_t *bytes = reinterpret_cast<const uint8_t *>(sv.a.data());
                        for (size_t i = 0; i < 64u; ++i)
                            h = (h ^ bytes[i]) * 0x100000001B3ull;
                        const auto found = firstWith.find(h);
                        if (found == firstWith.end())
                        {
                            if (sv.hasB)
                                firstWith.emplace(h, k);
                            continue;
                        }
                        const Saved &first = st.saved[found->second];
                        if (first.a == sv.a && (!sv.hasB || sv.b != first.b))
                        {
                            sv.b = first.b;
                            sv.hasB = true;
                            sv.carriedB = sv.carriedB || first.carriedB;
                            ++st.shared;
                        }
                    }
                }
                // (TODO 101 b, Tenkaichi3Decomp's sInterpRepeat) A camera cut - a cutscene's cut, a respawn, a warp - is no
                // motion to show between frames: a picture halfway between two unrelated views is neither. The frame is shown
                // as it is for all its pictures - no draw blended from the frame before's, none moved by the camera's
                // motion - when the frame's known camera jumped (view_turn's cut: its angles or its place moved more than a
                // frame's turn or walk can) or when most of the world's own in-between poses are no poses (the field B of
                // more than half the draws that have one over a quarter away from field A: a cut the game drew its field B
                // across). Single draws far off were left as they are before (not a pose, the carry under 5%); this is the
                // frame as a whole, so that no draw is blended while the rest stand. TF_POSES_CUT=0: off.
                {
                    static const bool cutOn = !(std::getenv("TF_POSES_CUT") && *std::getenv("TF_POSES_CUT") == '0');
                    const bool byTurn = f.turnCamera && view_turn::cameraCut(*f.turnCamera);
                    const bool byPoses = withBNow >= 8u && 2u * notPoseNow > withBNow;
                    // (TF_POSES_CUT_TEST=<tick>: a checking aid - the frames of that tick and the three after it taken as cuts)
                    static const long long testTick = std::getenv("TF_POSES_CUT_TEST") ? std::atoll(std::getenv("TF_POSES_CUT_TEST")) : -1;
                    const bool byTest = testTick >= 0 && static_cast<long long>(f.tick) >= testTick && static_cast<long long>(f.tick) <= testTick + 3;
                    st.cutNow = cutOn && (byTurn || byPoses || byTest);
                    if (st.cutNow)
                    {
                        for (Saved &sv : st.saved)
                        {
                            sv.hasB = false;
                            sv.gameB = false;
                            sv.carriedB = false;
                            sv.b = sv.a;
                        }
                        st.cameraDelta.clear();
                        ++st.cuts;
                        st.cutsByTurn += byTurn;
                        static int told = 0;
                        if (told < 40)
                        {
                            ++told;
                            std::fprintf(stderr, "[n9] a camera cut at tick %llu: the frame shown as it is (%s; %llu of %llu field B poses no pose)\n",
                                         static_cast<unsigned long long>(f.tick), byTurn ? "the camera jumped" : byPoses ? "the world's poses" : "TF_POSES_CUT_TEST",
                                         static_cast<unsigned long long>(notPoseNow), static_cast<unsigned long long>(withBNow));
                        }
                    }
                }
                // (TODO 101 a, TF_N9_CHECK) How much a part shrinks halfway between its two poses: a matrix blended by its
                // entries shortens an axis that turns (Tenkaichi3Decomp's fighters "went 2D" until interp_bone). The axis
                // rows' 3D length halfway against the halfway of their lengths, for every draw blended.
                {
                    static const bool check = std::getenv("TF_N9_CHECK") != nullptr;
                    if (check && !st.cutNow)
                        for (const Saved &sv : st.saved)
                        {
                            if (!sv.hasB)
                                continue;
                            // (TODO 101 a) The shrink of the turn as drawn: the matrix a picture is drawn with, as the operator Op on the
                            // model's axes that takes field B's rows to it (Op = C pinv(B)) - a turn keeps Op's smallest singular value
                            // at 1, the straight blend of a turn by an angle a takes it to cos(a / 2) halfway. (On the screen the rows'
                            // own lengths change with the view - an axis turning towards the eye shortens there - so they are no measure.)
                            double worst = 0.0, worstLinear = 0.0;
                            float mid[16];
                            const bool posed = midPose(sv.b.data(), sv.a.data(), mid);
                            if (!posed)
                                ++st.shrinkSnapped;
                            double B3[3][4], G[3][3], Gi[3][3];
                            for (int r = 0; r < 3; ++r)
                                for (int c = 0; c < 4; ++c)
                                    B3[r][c] = sv.b[4 * r + c];
                            for (int i = 0; i < 3; ++i)
                                for (int j = 0; j < 3; ++j)
                                {
                                    double v = 0.0;
                                    for (int c = 0; c < 4; ++c)
                                        v += B3[i][c] * B3[j][c];
                                    G[i][j] = v;
                                }
                            const auto det3 = [](const double M[3][3]) {
                                return M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
                                       M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
                            };
                            const double tr = G[0][0] + G[1][1] + G[2][2], dg = det3(G);
                            const bool measurable = tr > 0.0 && std::fabs(dg) > 1e-9 * tr * tr * tr;
                            if (measurable)
                            {
                                Gi[0][0] = (G[1][1] * G[2][2] - G[1][2] * G[2][1]) / dg;
                                Gi[0][1] = (G[0][2] * G[2][1] - G[0][1] * G[2][2]) / dg;
                                Gi[0][2] = (G[0][1] * G[1][2] - G[0][2] * G[1][1]) / dg;
                                Gi[1][0] = (G[1][2] * G[2][0] - G[1][0] * G[2][2]) / dg;
                                Gi[1][1] = (G[0][0] * G[2][2] - G[0][2] * G[2][0]) / dg;
                                Gi[1][2] = (G[0][2] * G[1][0] - G[0][0] * G[1][2]) / dg;
                                Gi[2][0] = (G[1][0] * G[2][1] - G[1][1] * G[2][0]) / dg;
                                Gi[2][1] = (G[0][1] * G[2][0] - G[0][0] * G[2][1]) / dg;
                                Gi[2][2] = (G[0][0] * G[1][1] - G[0][1] * G[1][0]) / dg;
                                // The smallest singular value of an operator: the square root of Op^T Op's smallest eigenvalue (the
                                // symmetric 3x3's, by the trigonometric solution).
                                const auto sigmaMin = [&](const double C[3][4]) {
                                    double Op[3][3];
                                    for (int i = 0; i < 3; ++i)
                                        for (int j = 0; j < 3; ++j)
                                        {
                                            double v = 0.0;
                                            for (int k = 0; k < 3; ++k)
                                            {
                                                double cb = 0.0;
                                                for (int c = 0; c < 4; ++c)
                                                    cb += C[i][c] * B3[k][c];
                                                v += cb * Gi[k][j];
                                            }
                                            Op[i][j] = v;
                                        }
                                    double M[3][3];
                                    for (int i = 0; i < 3; ++i)
                                        for (int j = 0; j < 3; ++j)
                                        {
                                            double v = 0.0;
                                            for (int k = 0; k < 3; ++k)
                                                v += Op[k][i] * Op[k][j];
                                            M[i][j] = v;
                                        }
                                    const double p1 = M[0][1] * M[0][1] + M[0][2] * M[0][2] + M[1][2] * M[1][2];
                                    const double q = (M[0][0] + M[1][1] + M[2][2]) / 3.0;
                                    double lo;
                                    if (p1 < 1e-30)
                                        lo = std::min({M[0][0], M[1][1], M[2][2]});
                                    else
                                    {
                                        const double p2 = (M[0][0] - q) * (M[0][0] - q) + (M[1][1] - q) * (M[1][1] - q) + (M[2][2] - q) * (M[2][2] - q) + 2.0 * p1;
                                        const double pp = std::sqrt(p2 / 6.0);
                                        double Bm[3][3];
                                        for (int i = 0; i < 3; ++i)
                                            for (int j = 0; j < 3; ++j)
                                                Bm[i][j] = (M[i][j] - (i == j ? q : 0.0)) / pp;
                                        const double r = std::clamp(det3(Bm) / 2.0, -1.0, 1.0);
                                        const double phi = std::acos(r) / 3.0;
                                        lo = q + 2.0 * pp * std::cos(phi + 2.0 * 3.14159265358979323846 / 3.0);
                                    }
                                    return std::sqrt(std::max(0.0, lo));
                                };
                                double A3[3][4];
                                for (int r = 0; r < 3; ++r)
                                    for (int c = 0; c < 4; ++c)
                                        A3[r][c] = sv.a[4 * r + c];
                                const double endA = sigmaMin(A3); // (a scale changing between the poses: what the blend should have)
                                for (const double t : {0.25, 0.5, 0.75})
                                {
                                    double C[3][4], L[3][4];
                                    for (int r = 0; r < 3; ++r)
                                        for (int c = 0; c < 4; ++c)
                                        {
                                            const double a = sv.a[4 * r + c], b = sv.b[4 * r + c], m = mid[4 * r + c];
                                            C[r][c] = b * (1.0 - t) * (1.0 - 2.0 * t) + m * 4.0 * t * (1.0 - t) + a * t * (2.0 * t - 1.0);
                                            L[r][c] = b + (a - b) * t;
                                        }
                                    const double want = 1.0 + (endA - 1.0) * t;
                                    if (want > 1e-6)
                                    {
                                        if (posed)
                                            worst = std::max(worst, 1.0 - sigmaMin(C) / want);
                                        worstLinear = std::max(worstLinear, 1.0 - sigmaMin(L) / want);
                                    }
                                }
                            }
                            st.shrinkLinearOver += worstLinear > 1e-2;
                            st.shrinkLinearMax = std::max(st.shrinkLinearMax, worstLinear);
                            ++st.shrinkDraws;
                            st.shrinkMax = std::max(st.shrinkMax, worst);
                            static int toldShrink = 0;
                            if (worst > 0.2 && toldShrink < 40)
                            {
                                ++toldShrink;
                                const Draw &d = f.draws[sv.draw];
                                std::fprintf(stderr, "[n9] halfway shrink %.1f%% at tick %llu: mesh %llu (%zu vertices, type %u, prim %02x), target %08x, %s, field B %.3f from field A\n",
                                             100.0 * worst, static_cast<unsigned long long>(f.tick), static_cast<unsigned long long>(d.mesh->id),
                                             d.mesh->vertices.size(), d.mesh->type, d.mesh->prim, d.target,
                                             sv.carriedB ? "carried (the frame before's field A)" : sv.gameB ? "the game's field B" : "shared",
                                             relDiff(sv.b.data(), sv.a.data()));
                                std::string rows;
                                for (int q = 0; q < 16; ++q)
                                {
                                    char v[64];
                                    std::snprintf(v, sizeof(v), " %.6g/%.6g", static_cast<double>(sv.b[q]), static_cast<double>(sv.a[q]));
                                    rows += v;
                                }
                                std::fprintf(stderr, "[n9]   its rows (field B / field A):%s\n", rows.c_str());
                            }
                            st.shrinkOver[0] += worst > 1e-4;
                            st.shrinkOver[1] += worst > 1e-3;
                            st.shrinkOver[2] += worst > 1e-2;
                        }
                }
                // The meshes the game makes anew every frame - the grass's blades (grown here from the ground's draw), a
                // microcode primitive's sprites (the warp gate's beams, the engines' glows), the ocean's patch - move on
                // their own between frames, and drawn at one place for all the pictures of a frame they stepped (N9 stage
                // 2: the grass's sway and the beams' turning jumped at every frame's start). Matched to the frame before's
                // of the same kind (what it is, its texture, its count of vertices, its place among the frame's draws of
                // that kind), each vertex is blended from the one it had then - with the matrix it had then - to this
                // frame's: the frame's first picture is the frame before's last again. A vertex that moved more than 64
                // GS pixels on the screen (one that wrapped round, another blade in that place) leaves the mesh as it is.
                // TF_POSES_VERTICES=0: as before.
                {
                    static const bool vertexOff = std::getenv("TF_POSES_VERTICES") && *std::getenv("TF_POSES_VERTICES") == '0';
                    st.vertexLast = std::move(st.vertexNow);
                    st.vertexNow.clear();
                    std::unordered_map<uint64_t, uint32_t> seen;
                    auto screen = [](const Vertex &v, const M4 &m, float &x, float &y) {
                        const float w = m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15];
                        if (!(std::fabs(w) > 1e-6f))
                            return false;
                        x = (m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12]) / w;
                        y = (m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13]) / w;
                        return true;
                    };
                    for (Saved &sv : st.saved)
                    {
                        const Draw &d = f.draws[sv.draw];
                        const Mesh &m = *d.mesh;
                        const bool grass = m.immediate && m.id >= (1ull << 62);
                        if (vertexOff || !vertices || !(m.primitive || m.ocean || grass) || m.vertices.empty() || m.vertices.size() > 65536u)
                            continue;
                        uint64_t key = 0xCBF29CE484222325ull;
                        for (const uint64_t part : {static_cast<uint64_t>(m.type), static_cast<uint64_t>(m.prim), d.tex0, static_cast<uint64_t>(m.vertices.size()),
                                                    static_cast<uint64_t>(m.sprites) | static_cast<uint64_t>(m.primitive) << 1 | static_cast<uint64_t>(m.ocean) << 2 |
                                                        static_cast<uint64_t>(grass) << 3})
                            key = (key ^ part) * 0x100000001B3ull;
                        key ^= static_cast<uint64_t>(seen[key]++) << 40;
                        st.vertexNow[key] = {d.mesh, sv.a};
                        const auto last = st.vertexLast.find(key);
                        if (last == st.vertexLast.end() || st.cutNow)
                        {
                            ++st.vertexNew;
                            continue;
                        }
                        const Mesh &before = *last->second.first;
                        bool jump = before.vertices.size() != m.vertices.size();
                        for (size_t i = 0; i < m.vertices.size() && !jump; ++i)
                        {
                            float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
                            if (!screen(before.vertices[i], last->second.second, x0, y0) || !screen(m.vertices[i], sv.a, x1, y1))
                                continue; // (behind the eye: no screen place - left to the blend)
                            jump = std::fabs(x1 - x0) > 64.0f || std::fabs(y1 - y0) > 64.0f;
                        }
                        if (jump)
                        {
                            ++st.vertexJumped;
                            continue;
                        }
                        ++st.vertexCarried;
                        sv.meshB = last->second.first;
                        sv.b = last->second.second;
                        sv.hasB = true;
                        sv.carriedB = true;
                    }
                }
                // The HUD's moving parts (the 2D drawn over the 3D: bars, gauges, the reticle, what slides or fades) stepped
                // with the game's frames. A 2D draw the same as the frame before's in kind (primitive, texture, target, count
                // of vertices, place among the frame's draws of that kind) and in its texture coordinates - only its places
                // or colours changed, by at most 64 GS pixels - is blended from the frame before's like the meshes above;
                // one whose texture coordinates changed (a digit, a glyph, a scrolled texture) is drawn as it is now.
                // TF_POSES_HUD=0: as before.
                {
                    static const bool hudOff = std::getenv("TF_POSES_HUD") && *std::getenv("TF_POSES_HUD") == '0';
                    st.hudLast = std::move(st.hudNow);
                    st.hudNow.clear();
                    std::unordered_map<uint64_t, uint32_t> seen;
                    for (Draw &d : f.draws)
                    {
                        if (hudOff || !vertices || !d.mesh || !d.mesh->immediate || d.mesh->id >= (1ull << 62) || d.mesh->vertices.empty() ||
                            d.mesh->vertices.size() > 4096u)
                            continue;
                        const Mesh &m = *d.mesh;
                        uint64_t key = 0x84222325CBF29CE4ull;
                        for (const uint64_t part : {static_cast<uint64_t>(m.prim), d.tex0, static_cast<uint64_t>(d.target), static_cast<uint64_t>(m.vertices.size())})
                            key = (key ^ part) * 0x100000001B3ull;
                        key ^= static_cast<uint64_t>(seen[key]++) << 40;
                        st.hudNow[key] = d.mesh;
                        const auto last = st.hudLast.find(key);
                        if (last == st.hudLast.end())
                            continue;
                        const Mesh &before = *last->second;
                        bool same = before.vertices.size() == m.vertices.size(), moved = false;
                        for (size_t i = 0; i < m.vertices.size() && same; ++i)
                        {
                            const Vertex &a = before.vertices[i], &b = m.vertices[i];
                            same = a.u == b.u && a.v == b.v && (!m.projective || a.size == b.size) && std::fabs(a.x - b.x) <= 64.0f &&
                                   std::fabs(a.y - b.y) <= 64.0f;
                            moved = moved || a.x != b.x || a.y != b.y || a.z != b.z || a.colour != b.colour || a.size != b.size;
                        }
                        if (!same)
                            ++st.hudOther;
                        else if (!moved)
                            ++st.hudStill;
                        else
                        {
                            ++st.hudMoved;
                            d.meshB = last->second;
                        }
                    }
                }
            }

            // The draws posed at moment t (0 the frame before's pose, 1 this frame's; `fb`: where field B lies).
            void apply(Frame &f, float t, float fb)
            {
                State &st = g_poses;
                const float s = (t - fb) / std::max(1e-3f, 1.0f - fb); // (0 at field B, 1 at field A)
                static const M4 identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                std::unordered_map<uint32_t, M4> dt;
                for (const auto &[target, d] : st.cameraDelta)
                {
                    M4 m{};
                    for (int i = 0; i < 16; ++i)
                        m[i] = s * identity[i] + (1.0f - s) * d[i];
                    dt[target] = m;
                }
                for (const Saved &sv : st.saved)
                {
                    Draw &d = f.draws[sv.draw];
                    if (t >= 1.0f)
                    {
                        std::memcpy(d.matrix, sv.a.data(), 64u);
                        if (sv.clip >= 0)
                            std::memcpy(d.shader.data[static_cast<size_t>(sv.clip)].v, sv.c.data(), 64u);
                        for (int k = 0; k < sv.instanceCount; ++k)
                            std::memcpy(d.shader.data[static_cast<size_t>(sv.instances + 14 + 5 * k)].v, sv.inst[static_cast<size_t>(k)].data(), 64u);
                        continue;
                    }
                    const auto camera = dt.find(bufferOf(d.target));
                    M4 m = sv.a;
                    if (sv.hasB)
                        for (int i = 0; i < 16; ++i)
                            m[i] = sv.b[i] + (sv.a[i] - sv.b[i]) * s;
                    else if (camera != dt.end())
                        m = mul(sv.a.data(), camera->second.data());
                    std::memcpy(d.matrix, m.data(), 64u);
                    // (the clipping matrix of the pose: m K - or the frame's own, bit for bit, while the draw does not move:
                    // a clipping matrix off in its last bits clipped the ground's triangles otherwise than its decals,
                    // which test depth for equality against it - the player's shadow left the ground, 2026-10-30)
                    if (sv.clip >= 0)
                    {
                        M4 c = sv.c;
                        if (m != sv.a && sv.kOk)
                            for (int i = 0; i < 4; ++i)
                                for (int j = 0; j < 4; ++j)
                                {
                                    double v = 0.0;
                                    for (int q = 0; q < 4; ++q)
                                        v += static_cast<double>(m[4 * i + q]) * sv.k[4 * q + j];
                                    c[4 * i + j] = static_cast<float>(v);
                                }
                        std::memcpy(d.shader.data[static_cast<size_t>(sv.clip)].v, c.data(), 64u);
                    }
                    if (sv.instanceCount > 0 && camera != dt.end())
                        for (int k = 0; k < sv.instanceCount; ++k)
                        {
                            const M4 im = mul(sv.inst[static_cast<size_t>(k)].data(), camera->second.data());
                            std::memcpy(d.shader.data[static_cast<size_t>(sv.instances + 14 + 5 * k)].v, im.data(), 64u);
                        }
                }
            }

            // (N9 stage 3) The frame before's pose of each draw given to the draw itself (Draw::matrixB, clipB, instB): the
            // frame is recorded once and the GPU blends each matrix for every picture (nr_d3d.cpp's `posed`) - what apply
            // does on the processor for each picture, the same rules: field B or the carry where a draw has a pose, else
            // the camera's motion on its target; the clipping matrix the pose's (B K), or the frame's own, bit for bit,
            // while the draw does not move; every instance moved with the camera. All of them take the camera's turn.
            void assign(Frame &f)
            {
                State &st = g_poses;
                for (const Saved &sv : st.saved)
                {
                    Draw &d = f.draws[sv.draw];
                    d.turned = true;
                    d.meshB = sv.meshB;
                    const auto camera = st.cameraDelta.find(bufferOf(d.target));
                    M4 b = sv.a;
                    if (sv.hasB)
                        b = sv.b;
                    else if (camera != st.cameraDelta.end())
                        b = mul(sv.a.data(), camera->second.data());
                    if (b != sv.a)
                    {
                        d.hasB = true;
                        std::memcpy(d.matrixB, b.data(), 64u);
                    }
                    if (sv.clip >= 0)
                    {
                        M4 c = sv.c;
                        if (b != sv.a && sv.kOk)
                            for (int i = 0; i < 4; ++i)
                                for (int j = 0; j < 4; ++j)
                                {
                                    double v = 0.0;
                                    for (int q = 0; q < 4; ++q)
                                        v += static_cast<double>(b[4 * i + q]) * sv.k[4 * q + j];
                                    c[4 * i + j] = static_cast<float>(v);
                                }
                        d.hasClipB = true;
                        std::memcpy(d.clipB, c.data(), 64u);
                    }
                    if (sv.instanceCount > 0 && camera != st.cameraDelta.end())
                    {
                        d.instB.resize(16u * static_cast<size_t>(sv.instanceCount));
                        for (int k = 0; k < sv.instanceCount; ++k)
                        {
                            const M4 im = mul(sv.inst[static_cast<size_t>(k)].data(), camera->second.data());
                            std::memcpy(d.instB.data() + 16 * k, im.data(), 64u);
                        }
                    }
                }
            }

            // (N9 stage 3) The frame drawn from one recording for all its pictures (TF_POSES_REPLAY unset or 1); =0: each
            // picture recorded again, as stage 2 did; =alt<s>: by turns every s seconds (an A/B in one run).
            bool replayWanted()
            {
                static const char *v = std::getenv("TF_POSES_REPLAY");
                if (v && *v == '0')
                    return false;
                if (!v || std::strncmp(v, "alt", 3) != 0)
                    return true;
                static const int alt = std::max(1, std::atoi(v + 3));
                static const auto t0 = std::chrono::steady_clock::now();
                const bool on = (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count() / alt) % 2 == 0;
                static bool last = !on;
                if (on != last)
                    std::fprintf(stderr, "[n9] one recording for a frame's pictures %s (TF_POSES_REPLAY)\n", on ? "on" : "off");
                last = on;
                return on;
            }

            // (TODO 88) The menus' pictures between their frames: always, whatever the levels' setting (TF_POSES) - the front
            // end's logic stays at the console's 30 frames, so without them the menus show 30 pictures a second; each picture
            // blends their 2D for its moment, nothing posed. TF_MENU_POSES=0: with the levels' setting, as before.
            bool menuWanted()
            {
                static const bool always = !std::getenv("TF_MENU_POSES") || *std::getenv("TF_MENU_POSES") != '0';
                return wanted() || (always && tf::port::nativePictures());
            }

            // TF_N9_CHECK=1: every 2 s, field B against the field A the same mesh (and its place among the frame's
            // draws of it) had the frame before; how many draws have a field B; the pictures a frame.
            void check(const Frame &f)
            {
                static const bool on = [] {
                    const char *v = std::getenv("TF_N9_CHECK");
                    return v && *v && *v != '0';
                }();
                if (!on)
                    return;
                State &st = g_poses;
                st.lastA = std::move(st.nowA);
                st.nowA.clear();
                std::unordered_map<uint64_t, uint32_t> seen;
                for (const Saved &sv : st.saved)
                {
                    const Draw &d = f.draws[sv.draw];
                    const uint64_t key = (d.mesh->id * 0x9E3779B97F4A7C15ull) ^ seen[d.mesh->id]++;
                    st.nowA[key] = sv.a;
                    if (!sv.hasB || (sv.carriedB && sv.bGame == M4{}))
                        continue;
                    const auto p = st.lastA.find(key);
                    if (p == st.lastA.end())
                        continue;
                    ++st.checked;
                    const double r = relDiff(sv.bGame.data(), p->second.data());
                    if (r >= 1e-5)
                    {
                        const double same = relDiff(sv.bGame.data(), sv.a.data());
                        ++(same < 1e-5 ? st.bIsA : st.bOther);
                        static uint64_t lastTold = 0;
                        if (st.told < 6u && f.tick - lastTold > 120u)
                        {
                            if (++st.told == 6u)
                                lastTold = f.tick;
                            char routines[64] = {};
                            for (uint32_t k = 0, at = 0; k < d.shader.count && at + 6u < sizeof(routines); ++k)
                                at += static_cast<uint32_t>(std::snprintf(routines + at, sizeof(routines) - at, "%04x ", d.shader.address[k]));
                            const float *pa = p->second.data();
                            std::fprintf(stderr, "[n9]   field B off (tick %llu): draw %zu mesh %llu (set %08x, %zu vertices, type %u, target %08x), routines %s- B vs last A %.2e, B vs this A %.2e, this A vs last A %.2e; row 3: last A %.2f %.2f %.4g %.4g, B %.2f %.2f %.4g %.4g, A %.2f %.2f %.4g %.4g\n",
                                         static_cast<unsigned long long>(f.tick), sv.draw, static_cast<unsigned long long>(d.mesh->id), d.mesh->source, d.mesh->vertices.size(), d.mesh->type, d.target, routines, r, same,
                                         relDiff(sv.a.data(), p->second.data()), pa[12], pa[13], pa[14], pa[15], sv.bGame[12], sv.bGame[13], sv.bGame[14], sv.bGame[15], sv.a[12], sv.a[13],
                                         sv.a[14], sv.a[15]);
                        }
                    }
                    st.near[0] += r < 1e-5;
                    st.near[1] += r < 1e-4;
                    st.near[2] += r < 1e-3;
                    st.moved += relDiff(sv.a.data(), p->second.data()) >= 1e-5;
                }
                const auto now = std::chrono::steady_clock::now();
                if (now - st.since < std::chrono::seconds(2))
                    return;
                st.since = now;
                const double n = std::max<double>(1.0, static_cast<double>(st.checked));
                std::fprintf(stderr,
                             "[n9] poses (tick %llu): %llu draws with a field B, %llu without (instances, sprites, grass: moved with the camera); "
                             "%llu compared with the frame before - field B within 1e-5/1e-4/1e-3 of its field A then %.1f/%.1f/%.1f%% (%.1f%% of them "
                             "moved; of those off by 1e-4, field B = this field A %llu, other %llu); carried %llu, not a pose %llu, pose shared %llu; %llu frames, %llu pictures, %zu targets with the camera's motion; field B at %.2f; %.2f ms a picture, %.2f on the GPU; an extra picture %.2f on the GPU, %.3f on the render thread; the frame's own work %.2f ms; tries kept %llu, given up %llu\n",
                             static_cast<unsigned long long>(f.tick), static_cast<unsigned long long>(st.withB), static_cast<unsigned long long>(st.without),
                             static_cast<unsigned long long>(st.checked), 100.0 * st.near[0] / n, 100.0 * st.near[1] / n, 100.0 * st.near[2] / n,
                             100.0 * st.moved / n, static_cast<unsigned long long>(st.bIsA), static_cast<unsigned long long>(st.bOther), static_cast<unsigned long long>(st.carried), static_cast<unsigned long long>(st.notPose), static_cast<unsigned long long>(st.shared), static_cast<unsigned long long>(st.frames), static_cast<unsigned long long>(st.pictures),
                             st.cameraDelta.size(), static_cast<double>(f.fieldBMoment), st.perPictureMs, backend().gpuPictureMs(), backend().gpuExtraMs(),
                             backend().cpuPictureMs(), st.frameBaseMs, static_cast<unsigned long long>(st.probesKept), static_cast<unsigned long long>(st.probesFailed));
                std::fprintf(stderr, "[n9] camera cuts (frames shown as they are): %llu, %llu of them the camera's jump; blends: %llu draws, a part shorter by over 0.01/0.1/1%%: %llu/%llu/%llu, at most %.3f%% (along the turn; %llu turns over 60 degrees not blended; a straight blend: %llu over 1%%, at most %.3f%%)\n",
                             static_cast<unsigned long long>(st.cuts), static_cast<unsigned long long>(st.cutsByTurn),
                             static_cast<unsigned long long>(st.shrinkDraws), static_cast<unsigned long long>(st.shrinkOver[0]),
                             static_cast<unsigned long long>(st.shrinkOver[1]), static_cast<unsigned long long>(st.shrinkOver[2]), 100.0 * st.shrinkMax,
                             static_cast<unsigned long long>(st.shrinkSnapped), static_cast<unsigned long long>(st.shrinkLinearOver), 100.0 * st.shrinkLinearMax);
                st.cuts = st.cutsByTurn = st.shrinkDraws = st.shrinkSnapped = st.shrinkLinearOver = 0;
                std::fill(std::begin(st.shrinkOver), std::end(st.shrinkOver), 0ull);
                st.shrinkMax = st.shrinkLinearMax = 0.0;
                std::fprintf(stderr, "[n9] meshes made anew each frame: %llu blended from the frame before's, %llu jumped, %llu new\n",
                             static_cast<unsigned long long>(st.vertexCarried), static_cast<unsigned long long>(st.vertexJumped),
                             static_cast<unsigned long long>(st.vertexNew));
                st.vertexCarried = st.vertexJumped = st.vertexNew = 0;
                std::fprintf(stderr, "[n9] the 2D over the 3D: %llu draws blended from the frame before's (moved), %llu as before, %llu changed otherwise\n",
                             static_cast<unsigned long long>(st.hudMoved), static_cast<unsigned long long>(st.hudStill),
                             static_cast<unsigned long long>(st.hudOther));
                st.hudMoved = st.hudStill = st.hudOther = 0;
                st.checked = st.withB = st.without = st.frames = st.pictures = st.moved = st.bIsA = st.bOther = st.carried = st.notPose = st.shared = 0;
                st.told = 0;
                std::fill(std::begin(st.near), std::end(st.near), 0ull);
            }

            // ---- The menus (TODO 88): pictures between their frames too -------------------------------------------
            // The front end (the title, the main menu, the saves, up to the HQ) is 2D alone and runs at the console's 30
            // frames: its fixed steps a frame (the clouds' scroll, the highlight's pulse, the slides and fades) stay as
            // they are, and the pictures between its frames are made here - each 2D draw's vertices (place, texture
            // coordinates, colour) blended between the frame before's and this frame's, when the frame draws the same
            // 2D as the frame before (as many draws, each with as many vertices, the same texture and primitive: the same
            // screen). A draw whose vertex jumps (a texture coordinate wrapping round, an item moved at once - more than
            // half a texture or 64 GS pixels) is drawn as it is now. Frames with 3D are not touched here (the HUD).
            // The same draw in the frame before: blended between them; else drawn as it is now (TODO 88).
            bool flatSame(const Draw &d, const State::Flat &before)
            {
                return d.mesh->vertices.size() == before.mesh->vertices.size() && d.tex0 == before.tex0 && d.mesh->prim == before.prim;
            }

            bool flatMatches(const Frame &f, const std::vector<State::Flat> &last, std::vector<size_t> &flat)
            {
                flat.clear();
                for (size_t i = 0; i < f.draws.size(); ++i)
                {
                    const Draw &d = f.draws[i];
                    if (d.mesh && d.mesh->immediate && !d.mesh->vertices.empty())
                        flat.push_back(i);
                }
                if (flat.empty() || flat.size() != last.size())
                    return false;
                // (TODO 88: a few draws that changed form - the Extras screen's scrolling line, its glyphs at the edge drawn as
                // strips one frame and sprites the next - no longer stop the whole screen's blending: those few, and the draws of
                // their texture, are drawn as they are now, the rest blended. Before, the Extras screen showed 30 pictures a
                // second.)
                size_t differ = 0;
                for (size_t k = 0; k < flat.size(); ++k)
                    differ += !flatSame(f.draws[flat[k]], last[k]);
                return differ * 4u <= flat.size();
            }

            uint32_t lerpColour(uint32_t a, uint32_t b, float t)
            {
                uint32_t out = 0;
                for (int c = 0; c < 32; c += 8)
                {
                    const float x = static_cast<float>((a >> c) & 0xFFu), y = static_cast<float>((b >> c) & 0xFFu);
                    out |= static_cast<uint32_t>(std::lround(x + (y - x) * t)) << c;
                }
                return out;
            }

            std::shared_ptr<const Mesh> blendFlat(const Mesh &before, const std::shared_ptr<const Mesh> &now, float t)
            {
                const auto &a = before.vertices;
                const auto &b = now->vertices;
                for (size_t i = 0; i < a.size(); ++i)
                    if (std::fabs(a[i].u - b[i].u) > 0.5f || std::fabs(a[i].v - b[i].v) > 0.5f || std::fabs(a[i].x - b[i].x) > 64.0f ||
                        std::fabs(a[i].y - b[i].y) > 64.0f)
                        return now; // (a jump: as it is now)
                auto m = std::make_shared<Mesh>(*now);
                for (size_t i = 0; i < a.size(); ++i)
                {
                    Vertex &v = m->vertices[i];
                    v.x = a[i].x + (b[i].x - a[i].x) * t;
                    v.y = a[i].y + (b[i].y - a[i].y) * t;
                    v.z = a[i].z + (b[i].z - a[i].z) * t;
                    v.u = a[i].u + (b[i].u - a[i].u) * t;
                    v.v = a[i].v + (b[i].v - a[i].v) * t;
                    v.size = a[i].size + (b[i].size - a[i].size) * t;
                    v.colour = lerpColour(a[i].colour, b[i].colour, t);
                }
                return m;
            }

            // (N9: the camera's turn for every picture, view_turn.h) The scene's 3D draws made with the scene's camera - the
            // matrices most of them were made with - take each picture's turn; draws with another camera in the scene's buffer
            // (none known) do not. f.turnMain the camera, -1 when no turn.
            void markTurned(Frame &f)
            {
                f.turnMain = -1;
                if (!f.turnCamera || !view_turn::turnable(*f.turnCamera))
                    return;
                uint32_t scene = 0;
                {
                    std::unordered_map<uint32_t, uint32_t> count;
                    uint32_t most = 0;
                    for (const Draw &d : f.draws)
                        if (d.mesh && !d.mesh->immediate && ++count[bufferOf(d.target)] > most)
                            most = count[bufferOf(d.target)], scene = bufferOf(d.target);
                }
                uint32_t uses[256] = {};
                for (const Draw &d : f.draws)
                    if (d.mesh && !(d.mesh->immediate && d.mesh->id < (1ull << 62)) && bufferOf(d.target) == scene && d.camera != 255u)
                        ++uses[d.camera];
                int main = -1;
                for (int k = 0; k < 255; ++k)
                    if (uses[k] && (main < 0 || uses[k] > uses[main]))
                        main = k;
                if (main < 0)
                    return;
                f.turnMain = main;
                for (Draw &d : f.draws)
                    if (d.mesh && !(d.mesh->immediate && d.mesh->id < (1ull << 62)) && bufferOf(d.target) == scene)
                        d.turned = view_turn::enabled() && (d.camera == static_cast<uint8_t>(main) || d.camera == 255u);
            }

            void keepFlat(const Frame &f, const std::vector<size_t> &flat)
            {
                State &st = g_poses;
                st.lastFlat.clear();
                for (size_t i : flat)
                {
                    const Draw &d = f.draws[i];
                    st.lastFlat.push_back(State::Flat{d.mesh, d.tex0, d.mesh->prim});
                }
            }

            // (N9: the camera's turn for every picture, view_turn.h) With no pictures between frames made (TF_POSES=0) and the
            // game's rate at half the screen's refresh or below (a heavy scene, a weaker PC, a lower TF_FRAME_RATE), the frame is
            // still drawn for each refresh it covers: its own pose in every picture, only the camera's turn newer (the mouse and
            // the stick by each picture's moment), so turning keeps the screen's rate while what moves on its own steps with the
            // game's frames. The pictures: the frame's moment and the refreshes after it until the next frame's (the game's
            // rate holds: the frame's span repeats), each waiting on the submitting thread for its moment's turn. As many as the
            // render thread and the GPU manage (pictures between frames' cap); on a screen of fixed refresh only where the
            // frame is a whole number of refreshes. TF_TURN_PICTURES=0: one picture a frame, as before. false: not made.
            bool turnPictures(Frame &f, const std::string &shot, int64_t prev, int64_t span, int64_t period)
            {
                State &st = g_poses;
                f.turnLeadSet = 0;
                f.turnWaits = false;
                if (f.turnMain < 0 || !view_turn::turnPicturesWanted() || !shot.empty() || f.movie || !f.has3D || f.refreshNs <= 0 ||
                    f.dueNs == 0 || prev == 0 || span <= 0 || span > 3 * period || !replayWanted())
                    return false;
                const double refresh = static_cast<double>(f.refreshNs);
                const double ratio = static_cast<double>(span) / refresh;
                int n = static_cast<int>(std::floor(ratio + 0.02));
                if (!f.variableRefresh && std::fabs(ratio - std::round(ratio)) > 0.05)
                    return false;
                const double spanMs = static_cast<double>(span) / 1.0e6;
                const double gpuFirst = backend().gpuPictureMs(), gpuExtra = backend().gpuExtraMs() > 0.0 ? backend().gpuExtraMs() : gpuFirst;
                const double cpuExtra = backend().cpuPictureMs() > 0.0 ? backend().cpuPictureMs() : 0.3;
                const double nGpu = gpuExtra > 0.0 ? 1.0 + (0.8 * spanMs - gpuFirst) / gpuExtra : 16.0;
                // (the render thread: an extra picture here is the frame's recording run again - next to nothing on the
                // processor - so up to 90% of the frame's time, as the menus' pictures: the frame's own work already within it)
                const double frameMs = st.singleMs > 0.0 ? st.singleMs : st.frameBaseMs;
                const double nCpu = 1.0 + (0.9 * spanMs - frameMs) / cpuExtra;
                const int fit = n;
                n = std::min({n, static_cast<int>(std::floor(nGpu)), static_cast<int>(std::floor(nCpu)), 16});
                // (TODO 161: the frame rate cap is the pictures' - with the fixed logic rate)
                if (const int cap = tf::port::pictureCap(); cap > 0)
                    n = std::min(n, std::max(1, static_cast<int>(std::floor(static_cast<double>(span) * cap / 1.0e9 + 0.02))));
                // (TODO 35, even frame pacing: on a screen with a refresh of its own the frame's pictures divide its refreshes,
                // each up the same number of them - 3 refreshes a frame (48 on 144 Hz) take 1 or 3 pictures, never 2, which
                // held them 1, 2, 1, 2 refreshes; the probe below tries the next such count. Variable refresh: any count.)
                const auto evenDown = [&](int k) {
                    if (f.variableRefresh || fit <= 0)
                        return k;
                    while (k > 1 && fit % k != 0)
                        --k;
                    return k;
                };
                int probeN = 2;
                if (!f.variableRefresh)
                    while (probeN < fit && fit % probeN != 0)
                        ++probeN;
                {
                    // (as pictures between frames' cap: a graphics card with little to do lowers its clock, so its time a
                    // picture measured at one a frame is too long - where the GPU's count is what keeps the pictures to one,
                    // two are tried now and then for 2 s, kept while the GPU keeps up: the render thread waited for a frame
                    // slot under 20 ms meanwhile and the pacer's delay did not grow by over a picture's spacing and 2 ms; tried
                    // again 4 s on when kept, after 8, 16 ... 64 s when not; the floor dropped when the GPU falls behind)
                    static bool probing = false;
                    static int proven = 0;
                    static double probeWait = 0.0, probeDelay = 0.0, backoff = 4.0, waitMark = 0.0;
                    static std::chrono::steady_clock::time_point until{}, next{}, waitSince{};
                    static const bool probeOff = std::getenv("TF_POSES_PROBE") && *std::getenv("TF_POSES_PROBE") == '0';
                    const auto now = std::chrono::steady_clock::now();
                    if (next.time_since_epoch().count() == 0)
                        next = now + std::chrono::seconds(4), waitSince = now, waitMark = backend().slotWaitMs();
                    if (now - waitSince > std::chrono::seconds(2))
                    {
                        if (backend().slotWaitMs() - waitMark > 30.0 && proven > 0 && !probing)
                            proven = 0, backoff = std::min(64.0, backoff * 2.0), next = now + std::chrono::milliseconds(static_cast<int64_t>(backoff * 1000.0));
                        waitMark = backend().slotWaitMs();
                        waitSince = now;
                    }
                    const int cpuCap = static_cast<int>(std::floor(nCpu));
                    if (probing)
                    {
                        const double waited = backend().slotWaitMs() - probeWait, later = backend().pacerDelayMs() - probeDelay;
                        const double allowed = spanMs / 2.0 + 2.0;
                        if (waited > 20.0 || later > allowed || now >= until)
                        {
                            probing = false;
                            const bool kept = waited <= 20.0 && later <= allowed;
                            if (kept)
                                proven = probeN;
                            backoff = kept ? 4.0 : std::min(64.0, backoff * 2.0);
                            next = now + std::chrono::milliseconds(static_cast<int64_t>(backoff * 1000.0));
                            static int told = 0;
                            if (told++ < 20)
                                std::fprintf(stderr, "[turn] pictures for the camera's turn: %d a frame tried - %s (GPU %.2f ms a picture; waited %.0f ms, delay %+.1f ms)\n",
                                             probeN, kept ? "kept" : "given up", gpuFirst, waited, later);
                        }
                        else
                            n = std::min({probeN, fit, cpuCap});
                    }
                    else if (!probeOff && n < probeN && fit >= probeN && cpuCap >= probeN && proven < probeN && now >= next)
                    {
                        probing = true;
                        probeWait = backend().slotWaitMs();
                        probeDelay = backend().pacerDelayMs();
                        until = now + std::chrono::seconds(2);
                        n = probeN;
                    }
                    if (!probing && proven >= 2)
                        n = std::max(n, std::min({proven, fit, cpuCap}));
                }
                n = evenDown(n);
                static uint64_t frames = 0, made = 0, capped = 0;
                static auto since = std::chrono::steady_clock::now();
                if (n < fit)
                    ++capped;
                if (std::chrono::steady_clock::now() - since > std::chrono::seconds(10))
                {
                    if (frames || capped)
                        std::fprintf(stderr, "[turn] pictures for the camera's turn: %llu frames drawn for %llu pictures in 10 s, %llu frames held to fewer by the GPU or the render thread (GPU %.2f ms a picture, %.2f an extra one; room for %.2f by the GPU, %.2f by the render thread (%.2f ms a frame), %d by the refreshes)\n",
                                     static_cast<unsigned long long>(frames), static_cast<unsigned long long>(made), static_cast<unsigned long long>(capped),
                                     gpuFirst, gpuExtra, nGpu, nCpu, frameMs, fit);
                    frames = made = capped = 0;
                    since = std::chrono::steady_clock::now();
                }
                if (n < 2)
                    return false;
                const double pictureNs = static_cast<double>(span) / static_cast<double>(n);
                f.pictures.clear();
                for (int j = 0; j < n; ++j)
                {
                    Frame::Picture pic;
                    pic.dueNs = f.dueNs + static_cast<int64_t>(j * pictureNs);
                    pic.s = 1.0f;
                    f.pictures.push_back(std::move(pic));
                }
                f.turnWaits = true;
                const int64_t keepPeriod = f.periodNs;
                const bool keepOnRefresh = f.vblanksOnRefresh;
                f.periodNs = span;
                f.pictureNs = static_cast<int64_t>(pictureNs);
                f.vblanksOnRefresh = !f.variableRefresh;
                const auto start = std::chrono::steady_clock::now();
                backend().render(f, shot);
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() - backend().replayWaitMs();
                f.periodNs = keepPeriod;
                f.vblanksOnRefresh = keepOnRefresh;
                f.pictureNs = 0;
                f.pictures.clear();
                f.turnWaits = false;
                const double base = st.preMs + ms - static_cast<double>(n - 1) * backend().cpuPictureMs();
                st.singleMs = st.singleMs <= 0.0 ? base : 0.95 * st.singleMs + 0.05 * base;
                ++frames;
                made += static_cast<uint64_t>(n);
                return true;
            }

            // The frame drawn: one picture for each refresh its moments cover (or one, t = 1).
            void render(Frame &f, const std::string &shot)
            {
                State &st = g_poses;
                // (TF_N9_LIST2D=<tick>,<count>: the 2D draws of that many frames from that VBlank on, a line each - what
                // the menus draw and how it moves from frame to frame)
                static const char *list2d = std::getenv("TF_N9_LIST2D");
                static uint64_t list2dFrom = list2d ? std::strtoull(list2d, nullptr, 10) : 0ull;
                static int list2dCount = list2d && std::strchr(list2d, ',') ? std::atoi(std::strchr(list2d, ',') + 1) : 2;
                if (list2d && f.tick >= list2dFrom && list2dCount > 0)
                {
                    --list2dCount;
                    std::fprintf(stderr, "[n9] 2D of tick %llu (due %.3f ms, has3D %d, %zu draws):\n", static_cast<unsigned long long>(f.tick),
                                 static_cast<double>(f.dueNs) / 1.0e6, f.has3D ? 1 : 0, f.draws.size());
                    for (size_t i = 0; i < f.draws.size(); ++i)
                    {
                        const Draw &d = f.draws[i];
                        if (!d.mesh || !d.mesh->immediate || d.mesh->vertices.empty())
                            continue;
                        const Vertex &v0 = d.mesh->vertices.front(), &v1 = d.mesh->vertices.back();
                        std::fprintf(stderr, "[n9]   %zu: %zu vertices prim %x tex0 %016llx target %08x tex %p; first %.2f,%.2f uv %.4f,%.4f c %08x; last %.2f,%.2f uv %.4f,%.4f c %08x\n",
                                     i, d.mesh->vertices.size(), d.mesh->prim, static_cast<unsigned long long>(d.tex0), d.target,
                                     static_cast<const void *>(d.texture.get()), v0.x, v0.y, v0.u, v0.v, v0.colour, v1.x, v1.y, v1.u, v1.v, v1.colour);
                    }
                }
                const int64_t due = f.dueNs, prev = st.prevDue;
                st.prevDue = due;
                const int64_t period = f.periodNs > 0 ? f.periodNs : 16666667;
                const int64_t span = due - prev;
                // (the menus: 2D alone, the same screen as the frame before - TODO 88)
                std::vector<size_t> flat;
                const bool flatFrame = !f.has3D;
                // (the menus shown as the native renderer draws them while it makes pictures between their frames - not
                // while a movie plays, whose pictures come to the GS from the movie decoder; TF_NATIVE_MENUS=0: the
                // emulated GS's menus, as before)
                static const bool menus = !std::getenv("TF_NATIVE_MENUS") || *std::getenv("TF_NATIVE_MENUS") != '0';
                const bool menuShown = flatFrame && menus && menuWanted() && !f.movie && f.refreshNs > 0;
                f.shown2D = menuShown;
                const bool flatBlend = menuShown && due != 0 && prev != 0 && flatMatches(f, st.lastFlat, flat);
                if (flatFrame && !flatBlend)
                {
                    if (!st.lastFlat.empty())
                        ++st.flatUnmatched;
                    flatMatches(f, {}, flat);
                }
                const bool blend = flatBlend || (wanted() && f.has3D && due != 0 && prev != 0 && f.fieldBMoment < 0.999f && f.refreshNs > 0);
                if (!blend || span <= 0 || span > 3 * period)
                {
                    if (!flatFrame)
                    {
                        markTurned(f);
                        if (turnPictures(f, shot, prev, span, period))
                        {
                            st.lastFlat.clear();
                            return;
                        }
                    }
                    const auto oneStart = std::chrono::steady_clock::now();
                    backend().render(f, shot);
                    if (!flatFrame)
                    {
                        // (the render thread's time for a frame of one picture, less its waits for the GPU)
                        const double ms = st.preMs + std::max(0.0, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - oneStart).count() -
                                                                       backend().replayWaitMs());
                        st.singleMs = st.singleMs <= 0.0 ? ms : 0.95 * st.singleMs + 0.05 * ms;
                    }
                    if (flatFrame)
                        keepFlat(f, flat);
                    else
                        st.lastFlat.clear();
                    return;
                }
                // The pictures' period: the screen's refresh - no faster than the render thread (its time a picture,
                // smoothed, within 60% of the time: the game's own thread needs the processor more) and the GPU (its time a
                // picture by its timestamps, within 80%) manage, and no slower than a frame. Measured 2026-10-30 on the
                // other PC (RX 5700 XT, 180 Hz): 4.5-9 ms of GPU a picture; with the render thread at 85% of the time the
                // game's own rate fell from 80 frames to 40. On a screen of fixed refresh a whole number of refreshes, the
                // moments on its refreshes (a little before each, to be handed over in time); the number goes up at once
                // when the computer falls behind, down only when it has had room for two seconds (no flapping).
                const double refresh = static_cast<double>(f.refreshNs);
                // (N9 stage 3: with the frame recorded once, an extra picture costs the GPU its time again (less where it
                // can skip work) and the render thread next to nothing: how many fit in the frame's span, its own picture
                // and the render thread's frame work counted once.)
                const bool replay = !flatBlend && replayWanted();
                // (TODO 88: the menus' 2D, the game's thread with next to nothing to do in them: 90% of the render thread's time
                // for their pictures - at 60% the RX 5700 XT's 1-3 ms a picture kept them at every other refresh at 180 Hz)
                const double cpuShare = flatBlend ? 0.9 : 0.6;
                // (TODO 266: the menus' 2D may take 95% of the graphics card's time a picture, not 80%: a card with next to
                // nothing to do lowers its clock and its measured time grows - the GTX 1080's 5.75 ms a menu picture at 144
                // Hz, 8x MSAA, was over 80% of a refresh (5.56) and halved the menus to 72 pictures. The try below covers
                // the rest.)
                // (TF_MENU_GPU_SHARE=<share> (tests): another share for the menus, to make the card's time hold them back)
                static const double menuShare = std::getenv("TF_MENU_GPU_SHARE") ? std::clamp(std::atof(std::getenv("TF_MENU_GPU_SHARE")), 0.05, 1.0) : 0.95;
                const double gpuShare = flatBlend ? menuShare : 0.8;
                const double cpuNeeded = st.perPictureMs / cpuShare * 1.0e6;
                double needed = std::max(st.perPictureMs / cpuShare, backend().gpuPictureMs() / gpuShare) * 1.0e6;
                double capNs = 0.0; // (the caps' part of `needed`: no try goes under it)
                // (TODO 161: the frame rate cap is the pictures' - with the fixed logic rate - so no closer than it)
                if (const int cap = tf::port::pictureCap(); cap > 0)
                    capNs = 1.0e9 / static_cast<double>(cap), needed = std::max(needed, capNs);
                // (TODO 266: VSync off - the presents at once - the menus' pictures are not tied to the screen's refresh: as
                // many as the render thread and the GPU make, up to the frame rate cap; on a 60 Hz screen they were 60 at most)
                const bool unpaced = flatBlend && tf::port::vsyncOff();
                if (unpaced)
                    if (const int cap = tf::port::frameCap(); cap > 0)
                        needed = std::max(needed, 1.0e9 / static_cast<double>(cap));
                if (replay)
                {
                    const double spanMs = static_cast<double>(span) / 1.0e6;
                    const double gpuFirst = backend().gpuPictureMs(), gpuExtra = backend().gpuExtraMs() > 0.0 ? backend().gpuExtraMs() : gpuFirst;
                    const double cpuExtra = backend().cpuPictureMs() > 0.0 ? backend().cpuPictureMs() : 0.3;
                    const double nGpu = gpuExtra > 0.0 ? 1.0 + (0.8 * spanMs - gpuFirst) / gpuExtra : 16.0;
                    const double nCpu = 1.0 + (0.6 * spanMs - st.frameBaseMs) / cpuExtra;
                    // A graphics card with little to do lowers its clock, and its time a picture grows with it (the other
                    // PC's RX 5700 XT: 8-10 ms a picture at one picture a frame, 4.5 once it draws two or three; its
                    // timestamps also count the time the window's own drawing takes on the same card) - measured at one
                    // picture a frame, the cap would never let it draw more. So where the GPU's time is what holds the pictures
                    // back, one refresh less between pictures is tried now and then for 2 s: kept (`proven`, a floor under the
                    // timestamps' count) when the GPU kept up meanwhile - the render thread waited for a frame slot under 20
                    // ms in all and the pictures' delay to the screen (the pacer's) did not grow by over a picture's spacing
                    // and 2 ms (the frame's first picture is an earlier moment: that much is the pictures' own) - given up at
                    // once otherwise; tried again after 4 s when kept (one more step), after 8, 16 ... 64 s when not. The floor
                    // goes when the GPU falls behind (frame-slot waits over 30 ms in 2 s). TF_POSES_PROBE=0: no tries.
                    static const bool probeOff = std::getenv("TF_POSES_PROBE") && *std::getenv("TF_POSES_PROBE") == '0';
                    const auto now = std::chrono::steady_clock::now();
                    const double fit = refresh > 0.0 ? static_cast<double>(span) / refresh : 16.0; // (pictures the refreshes allow)
                    if (now - st.waitSince > std::chrono::seconds(2))
                    {
                        const double waited = backend().slotWaitMs() - st.waitMark;
                        if (waited > 30.0 && st.proven > 0.0 && !st.probing)
                        {
                            st.proven = 0.0;
                            st.probeBackoff = std::min(64.0, st.probeBackoff * 2.0);
                            st.nextProbe = now + std::chrono::milliseconds(static_cast<int64_t>(st.probeBackoff * 1000.0));
                            std::fprintf(stderr, "[n9] pictures between frames: the GPU fell behind (waited %.0f ms in 2 s) - back to its timestamps' count\n", waited);
                        }
                        st.waitMark = backend().slotWaitMs();
                        st.waitSince = now;
                    }
                    const double nGpuNow = std::max(nGpu, st.proven);
                    double n = std::clamp(std::min(nGpuNow, nCpu), 1.0, 16.0);
                    st.lastGpuLimits = nGpuNow < nCpu;
                    st.probeForced = false;
                    if (st.probing)
                    {
                        const double waited = backend().slotWaitMs() - st.probeWait;
                        const double later = backend().pacerDelayMs() - st.probeDelay;
                        // (the delay grows by up to a picture's spacing by itself: the frame's first picture is an earlier moment)
                        const double laterAllowed = static_cast<double>(span) / st.probeN / 1.0e6 + 2.0;
                        if (waited > 20.0 || later > laterAllowed || now >= st.probeUntil)
                        {
                            st.probing = false;
                            const bool kept = waited <= 20.0 && later <= laterAllowed;
                            if (kept)
                                st.proven = std::max(st.proven, st.probeN);
                            st.probeBackoff = kept ? 4.0 : std::min(64.0, st.probeBackoff * 2.0);
                            st.nextProbe = now + std::chrono::milliseconds(static_cast<int64_t>(st.probeBackoff * 1000.0));
                            ++(kept ? st.probesKept : st.probesFailed);
                            static int told = 0;
                            if (told < 40)
                            {
                                ++told;
                                std::fprintf(stderr, "[n9] pictures between frames: %.2f a frame tried - %s (GPU %.2f ms a picture, %.2f an extra one; waited %.0f ms, delay %+.1f ms)\n",
                                             st.probeN, kept ? "kept" : "given up", gpuFirst, gpuExtra, waited, later);
                            }
                            n = std::clamp(std::min(std::max(nGpu, st.proven), nCpu), 1.0, 16.0);
                        }
                        else
                        {
                            n = std::max(n, st.probeN);
                            st.probeForced = true;
                        }
                    }
                    else if (const double target = f.variableRefresh || refresh <= 0.0 ? std::floor(n) + 1.0
                                                   : static_cast<double>(span) / (refresh * std::max(1, st.refreshes - 1));
                             !probeOff && st.lastGpuLimits && st.refreshes > (f.variableRefresh ? 0 : 1) && target > n + 0.01 &&
                             target <= std::min(fit + 0.02, nCpu) && now >= st.nextProbe && st.nextProbe.time_since_epoch().count() != 0)
                    {
                        // (on a fixed refresh: the pictures a frame one refresh fewer between them takes)
                        st.probing = true;
                        st.probeN = target;
                        st.probeWait = backend().slotWaitMs();
                        st.probeDelay = backend().pacerDelayMs();
                        st.probeUntil = now + std::chrono::seconds(2);
                        n = st.probeN;
                        st.probeForced = true;
                    }
                    if (st.nextProbe.time_since_epoch().count() == 0)
                        st.nextProbe = now + std::chrono::seconds(4); // (not in a level's first seconds)
                    needed = static_cast<double>(span) / n;
                }
                // (unpaced: no faster than the 16 moments a frame below allow, nor 1 ms - before the first timings `needed` is 0,
                // and a period of 0 made the moments' loop below spin for ever: a hang at the boot's first menu, VSync off)
                double pictureNs = unpaced ? std::max({needed, static_cast<double>(span) / 16.0, 1.0e6}) : std::max(refresh, needed);
                if (!f.variableRefresh && !unpaced)
                {
                    int want = std::max(1, static_cast<int>(std::ceil(pictureNs / refresh - 0.02)));
                    const auto now = std::chrono::steady_clock::now();
                    // (TODO 266) The menus held at every other refresh by the graphics card's measured time: that time is
                    // taken at the lower picture rate, where the card clocks down, so it can stay over its share for good
                    // (a friend's RX 5600 XT: menus at 30 on a 60 Hz screen). As the levels' pictures do (above), one
                    // refresh fewer between pictures is tried for 2 s now and then: kept when the card kept up meanwhile -
                    // the render thread waited under 100 ms in all for a frame slot and the pictures' delay to the screen
                    // did not grow by over a picture's spacing and 2 ms - and given up at once otherwise; tried again
                    // after 4 s when kept (one more step), after 8, 16 ... 64 s when not. What a try kept holds until the
                    // card falls behind (frame-slot waits over 150 ms in 2 s). Never closer than the frame rate cap or the
                    // render thread's own time allow. TF_MENU_PROBE=0: no tries.
                    bool flatForced = false;
                    if (flatBlend)
                    {
                        // (the waits: 5% of a try's 2 s, and 150 ms in 2 s for "fell behind" - the levels' 20 and 30 ms are at
                        // the menus' own noise: measured 20 to 23 ms in 2 s on a GTX 1080 that needs 1 ms a menu picture; a card
                        // that cannot keep a refresh waits for 15% of the time and more)
                        constexpr double kFlatWaitMs = 100.0, kFlatBehindMs = 150.0;
                        static const bool probeOff = std::getenv("TF_MENU_PROBE") && *std::getenv("TF_MENU_PROBE") == '0';
                        const auto fits = [&](int refreshes)
                        { return refreshes >= 1 && refresh * refreshes >= std::max(cpuNeeded, capNs) * 0.98; };
                        if (now - st.flatWaitSince > std::chrono::seconds(2))
                        {
                            if (backend().slotWaitMs() - st.flatWaitMark > kFlatBehindMs && st.flatProven > 0 && !st.flatProbing)
                            {
                                std::fprintf(stderr, "[tf] menus: the graphics card fell behind at a picture every %d refresh(es) - back to its measured time\n",
                                             st.flatProven);
                                st.flatProven = 0;
                                st.flatBackoff = std::min(64.0, st.flatBackoff * 2.0);
                                st.flatNextProbe = now + std::chrono::milliseconds(static_cast<int64_t>(st.flatBackoff * 1000.0));
                            }
                            st.flatWaitMark = backend().slotWaitMs();
                            st.flatWaitSince = now;
                        }
                        if (st.flatProbing)
                        {
                            const double waited = backend().slotWaitMs() - st.flatProbeWait, later = backend().pacerDelayMs() - st.flatProbeDelay;
                            const double laterAllowed = refresh * st.flatProbeRefreshes / 1.0e6 + 2.0;
                            if (waited > kFlatWaitMs || later > laterAllowed || now >= st.flatProbeUntil)
                            {
                                st.flatProbing = false;
                                const bool kept = waited <= kFlatWaitMs && later <= laterAllowed;
                                if (kept)
                                    st.flatProven = st.flatProbeRefreshes;
                                st.flatBackoff = kept ? 4.0 : std::min(64.0, st.flatBackoff * 2.0);
                                st.flatNextProbe = now + std::chrono::milliseconds(static_cast<int64_t>(st.flatBackoff * 1000.0));
                                static int told = 0;
                                if (told < 20)
                                {
                                    ++told;
                                    std::fprintf(stderr, "[tf] menus: a picture every %d refresh(es) tried - %s (the graphics card %.2f ms a picture; waited %.0f ms, delay %+.1f ms)\n",
                                                 st.flatProbeRefreshes, kept ? "kept" : "given up", backend().gpuPictureMs(), waited, later);
                                }
                            }
                        }
                        else if (!probeOff && want > 1 && st.refreshes > 1 && fits(std::min(want, st.refreshes) - 1) &&
                                 (st.flatProven == 0 || st.flatProven > std::min(want, st.refreshes) - 1) &&
                                 st.flatNextProbe.time_since_epoch().count() != 0 && now >= st.flatNextProbe)
                        {
                            st.flatProbing = true;
                            st.flatProbeRefreshes = std::min(want, st.refreshes) - 1;
                            st.flatProbeWait = backend().slotWaitMs();
                            st.flatProbeDelay = backend().pacerDelayMs();
                            st.flatProbeUntil = now + std::chrono::seconds(2);
                        }
                        if (st.flatNextProbe.time_since_epoch().count() == 0)
                            st.flatNextProbe = now + std::chrono::seconds(4); // (not in the menus' first seconds)
                        const int floor = st.flatProbing ? st.flatProbeRefreshes : st.flatProven;
                        if (floor > 0 && fits(floor) && want > floor)
                            want = floor, flatForced = true;
                    }
                    if (want > st.refreshes || ((flatForced || (replay && st.probeForced)) && want < st.refreshes))
                        st.refreshes = want, st.roomSince = now;
                    // (with one recording, a count the GPU was proven to keep up with is exact: no margin under it - with 15%
                    // the RX 5700 XT stayed at 90 pictures where it had kept up with 180, 2026-10-30)
                    else if (want == st.refreshes || needed > refresh * (st.refreshes - 1) * (replay ? 1.001 : 0.85))
                        st.roomSince = now;
                    else if (now - st.roomSince > std::chrono::seconds(2))
                        st.refreshes = want, st.roomSince = now;
                    pictureNs = refresh * st.refreshes;
                    // (TODO 266: a friend's menus at 30 on a 60 Hz screen, not reproduced here. In every log, each time the
                    // menus' pictures change how many refreshes apart they are, with what set it: his log then tells whether
                    // his graphics card's or render thread's time a picture is what halves them. At most 20 lines a run.)
                    if (flatBlend)
                    {
                        static int toldRefreshes = 0, lines = 0;
                        if (st.refreshes != toldRefreshes && lines < 20)
                        {
                            ++lines;
                            toldRefreshes = st.refreshes;
                            std::fprintf(stderr, "[tf] menus: a picture every %d refresh(es) of %.2f ms (the render thread needs %.2f ms a picture, "
                                         "the graphics card %.2f; a picture may take 90%% of the thread's time and %.0f%% of the card's)\n",
                                         st.refreshes, refresh / 1.0e6, st.perPictureMs, backend().gpuPictureMs(), gpuShare * 100.0);
                        }
                    }
                }
                pictureNs = std::min(pictureNs, static_cast<double>(span));
                if (!(pictureNs > 0.0)) // (never a period of 0 or NaN: the loop below would not end)
                    pictureNs = static_cast<double>(std::max<int64_t>(span, 1));
                const double lead = std::min(3.0e6, 0.45 * refresh);
                const double phase = f.variableRefresh || unpaced ? 0.0 : static_cast<double>(f.refreshVblankNs) - lead;
                // Moments in (prev, due]: phase + n * pictureNs.
                std::vector<int64_t> moments;
                {
                    const double n0 = std::floor((static_cast<double>(prev) - phase) / pictureNs) + 1.0;
                    for (double n = n0;; n += 1.0)
                    {
                        const int64_t m = static_cast<int64_t>(phase + n * pictureNs);
                        if (m > due || moments.size() >= 16u)
                            break;
                        if (m > prev)
                            moments.push_back(m);
                    }
                }
                if (moments.empty())
                    moments.push_back(due);
                // (TF_N9_SHOTS=<n>, a checking aid: a saved frame drawn at the moments k/n of it, k = 0..n, each saved -
                // k = 0 is the frame before's pose, to be the same as that frame's last picture)
                static const int shotMoments = std::getenv("TF_N9_SHOTS") ? std::max(1, std::atoi(std::getenv("TF_N9_SHOTS"))) : 0;
                if (!shot.empty() && shotMoments > 0)
                {
                    moments.clear();
                    for (int k = 0; k <= shotMoments; ++k)
                        moments.push_back(prev + span * k / shotMoments);
                }
                if (!flatBlend)
                {
                    prepare(f, replay); // (the vertex carry with one recording only: the GPU blends the vertices)
                    check(f);
                    st.lastFlat.clear();
                }
                else
                    ++st.flatFrames;
                std::vector<std::shared_ptr<const Mesh>> flatNow;
                std::vector<char> flatBlended; // (each of them blended, or drawn as it is now)
                if (flatBlend)
                {
                    for (size_t i : flat)
                        flatNow.push_back(f.draws[i].mesh);
                    // (TODO 88) A draw whose form changed is drawn as it is now - and so is every draw with its texture: the
                    // Extras screen's scrolling line is a row of draws of the font, and when one of them changes form at the
                    // edge the others no longer line up with the frame before's by their place in the list (blended, they
                    // showed parts of the line missing between frames). That line then moves at the menus' 30 frames; the
                    // rest of the screen between them.
                    std::vector<uint64_t> changed;
                    for (size_t k = 0; k < flat.size(); ++k)
                        if (!flatSame(f.draws[flat[k]], st.lastFlat[k]))
                            changed.push_back(f.draws[flat[k]].tex0);
                    for (size_t k = 0; k < flat.size(); ++k)
                        flatBlended.push_back(std::find(changed.begin(), changed.end(), f.draws[flat[k]].tex0) == changed.end());
                }
                // (TF_N9_LIST=<tick>: that frame's draws, a line each - how each is posed between frames, how far its matrix
                // and its clipping matrix move at t = 0.5)
                static const uint64_t listTick = std::getenv("TF_N9_LIST") ? std::strtoull(std::getenv("TF_N9_LIST"), nullptr, 10) : 0ull;
                if (listTick && f.tick == listTick)
                {
                    std::vector<M4> before(st.saved.size()), clipBefore(st.saved.size());
                    for (size_t k = 0; k < st.saved.size(); ++k)
                    {
                        std::memcpy(before[k].data(), f.draws[st.saved[k].draw].matrix, 64u);
                        if (st.saved[k].clip >= 0)
                            std::memcpy(clipBefore[k].data(), f.draws[st.saved[k].draw].shader.data[static_cast<size_t>(st.saved[k].clip)].v, 64u);
                    }
                    apply(f, 0.5f, f.fieldBMoment);
                    for (size_t k = 0; k < st.saved.size(); ++k)
                    {
                        const Saved &sv = st.saved[k];
                        const Draw &d = f.draws[sv.draw];
                        char routines[64] = {};
                        for (uint32_t r = 0, at = 0; r < d.shader.count && at + 6u < sizeof(routines); ++r)
                            at += static_cast<uint32_t>(std::snprintf(routines + at, sizeof(routines) - at, "%04x ", d.shader.address[r]));
                        const double moved = relDiff(d.matrix, before[k].data());
                        const double clipMoved = sv.clip >= 0 ? relDiff(d.shader.data[static_cast<size_t>(sv.clip)].v, clipBefore[k].data()) : 0.0;
                        std::fprintf(stderr, "[n9]   draw %zu: %s mesh %llu (%zu vertices, type %u%s%s%s), target %08x, routines %s- at t=0.5 matrix moved %.2e, clip %.2e%s\n",
                                     sv.draw, sv.carriedB ? "carried" : sv.gameB ? "B" : "camera", static_cast<unsigned long long>(d.mesh->id), d.mesh->vertices.size(),
                                     d.mesh->type, d.mesh->sprites ? " sprites" : "", d.mesh->primitive ? " primitive" : "", d.mesh->immediate ? " grass" : "", d.target,
                                     routines, moved, clipMoved, sv.instanceCount > 0 ? " (instances)" : "");
                    }
                    apply(f, 1.0f, f.fieldBMoment);
                }
                if (replay)
                {
                    // (N9 stage 3) One recording, every picture from it: the poses to the draws, the pictures' moments to
                    // the frame.
                    assign(f);
                    markTurned(f);
                    f.turnLeadSet = 1;
                    f.turnWaits = false;
                    f.pictures.clear();
                    for (size_t j = 0; j < moments.size(); ++j)
                    {
                        const float t = std::clamp(static_cast<float>(static_cast<double>(moments[j] - prev) / static_cast<double>(span)), 0.0f, 1.0f);
                        Frame::Picture pic;
                        pic.dueNs = moments[j];
                        pic.s = t >= 1.0f ? 1.0f : (t - f.fieldBMoment) / std::max(1e-3f, 1.0f - f.fieldBMoment);
                        if (!shot.empty() && shotMoments > 0)
                        {
                            const size_t dot = shot.rfind('.');
                            char suffix[32];
                            std::snprintf(suffix, sizeof(suffix), "_m%zu_t%03d", j, static_cast<int>(std::lround(t * 1000.0f)));
                            pic.shot = dot == std::string::npos ? shot + suffix : shot.substr(0, dot) + suffix + shot.substr(dot);
                        }
                        f.pictures.push_back(std::move(pic));
                    }
                    ++st.frames;
                    st.pictures += moments.size();
                    const int64_t keepPeriod = f.periodNs;
                    const bool keepOnRefresh = f.vblanksOnRefresh;
                    f.periodNs = span;
                    f.pictureNs = static_cast<int64_t>(pictureNs);
                    f.vblanksOnRefresh = !f.variableRefresh;
                    const auto start = std::chrono::steady_clock::now();
                    backend().render(f, shot);
                    // (less the waits for the extra pictures' turns - the GPU's time, counted by gpuExtraMs: d3d553)
                    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() - backend().replayWaitMs();
                    f.periodNs = keepPeriod;
                    f.vblanksOnRefresh = keepOnRefresh;
                    f.pictureNs = 0;
                    f.pictures.clear();
                    const double base = st.preMs + ms - static_cast<double>(moments.size() - 1u) * backend().cpuPictureMs();
                    st.frameBaseMs = st.frameBaseMs <= 0.0 ? base : (base > st.frameBaseMs ? 0.7 * st.frameBaseMs + 0.3 * base : 0.97 * st.frameBaseMs + 0.03 * base);
                    st.perPictureMs = ms / static_cast<double>(moments.size());
                    return;
                }
                ++st.frames;
                if (!flatBlend)
                {
                    markTurned(f);
                    f.turnLeadSet = 1;
                    f.turnWaits = false;
                }
                const auto start = std::chrono::steady_clock::now();
                const double slotWaitBefore = backend().slotWaitMs();
                const int64_t keepDue = f.dueNs, keepPeriod = f.periodNs;
                const bool keepOnRefresh = f.vblanksOnRefresh;
                const bool shotsEach = shotMoments > 0;
                for (size_t j = 0; j < moments.size(); ++j)
                {
                    const float t = std::clamp(static_cast<float>(static_cast<double>(moments[j] - prev) / static_cast<double>(span)), 0.0f, 1.0f);
                    if (flatBlend)
                    {
                        for (size_t k = 0; k < flat.size(); ++k)
                            f.draws[flat[k]].mesh = t >= 1.0f || !flatBlended[k] ? flatNow[k] : blendFlat(*st.lastFlat[k].mesh, flatNow[k], t);
                        ++st.flatPictures;
                    }
                    else
                        apply(f, t, f.fieldBMoment);
                    f.dueNs = moments[j];
                    f.periodNs = span;
                    f.pictureNs = static_cast<int64_t>(pictureNs);
                    f.vblanksOnRefresh = !f.variableRefresh;
                    std::string name;
                    if (!shot.empty() && (shotsEach || j + 1u == moments.size()))
                    {
                        name = shot;
                        if (shotsEach)
                        {
                            const size_t dot = name.rfind('.');
                            char suffix[32];
                            std::snprintf(suffix, sizeof(suffix), "_m%zu_t%03d", j, static_cast<int>(std::lround(t * 1000.0f)));
                            name = dot == std::string::npos ? name + suffix : name.substr(0, dot) + suffix + name.substr(dot);
                        }
                    }
                    backend().render(f, name);
                    ++st.pictures;
                }
                if (flatBlend)
                {
                    for (size_t k = 0; k < flat.size(); ++k)
                        f.draws[flat[k]].mesh = flatNow[k];
                    keepFlat(f, flat);
                    // (TF_N9_CHECK: every 2 s, the menus' frames and the pictures made between them)
                    static const bool told = std::getenv("TF_N9_CHECK") != nullptr;
                    static auto since = std::chrono::steady_clock::now();
                    if (told && std::chrono::steady_clock::now() - since > std::chrono::seconds(2))
                    {
                        since = std::chrono::steady_clock::now();
                        std::fprintf(stderr, "[n9] menus (tick %llu): %llu frames of 2D alone blended, %llu pictures; %llu frames not (another screen); "
                                     "the render thread %.2f ms a picture, the GPU %.2f; %d refreshes a picture\n",
                                     static_cast<unsigned long long>(f.tick), static_cast<unsigned long long>(st.flatFrames),
                                     static_cast<unsigned long long>(st.flatPictures), static_cast<unsigned long long>(st.flatUnmatched),
                                     st.perPictureMs, backend().gpuPictureMs(), st.refreshes);
                        st.flatFrames = st.flatPictures = st.flatUnmatched = 0;
                    }
                }
                else
                    apply(f, 1.0f, f.fieldBMoment);
                f.dueNs = keepDue;
                f.periodNs = keepPeriod;
                f.vblanksOnRefresh = keepOnRefresh;
                f.pictureNs = 0;
                // (TODO 88: less the waits for a frame slot - the GPU's time, which gpuPictureMs counts. Each picture of a frame
                // is drawn right after the one before, so each waits for the GPU to free the slot two pictures back: counted
                // here the menus' render thread measured 2-5 ms a picture on the RX 5700 XT, 1-3 without them.)
                const double ms = std::max(0.0, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() -
                                                    (backend().slotWaitMs() - slotWaitBefore)) /
                                  static_cast<double>(moments.size());
                st.perPictureMs = st.perPictureMs <= 0.0 ? ms : (ms > st.perPictureMs ? 0.7 * st.perPictureMs + 0.3 * ms : 0.97 * st.perPictureMs + 0.03 * ms);
            }
        }

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
            uint64_t drawn = 0, behind = 0, capped = 0, tested = 0, keptDraws = 0;
        } g_drops;
        std::atomic<uint64_t> g_behindTotal{0}; // (nr::framesLeftOutBehind)

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
                if (f.dueNs != 0)
                    poses::g_poses.prevDue = f.dueNs;
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
            if (f.dueNs != 0)
                poses::g_poses.prevDue = f.dueNs;
        }

        void drawJob(Job &job)
        {
            const int64_t drawStartNs = latLogSeconds() ? nowNs() : 0;
            const auto start = std::chrono::steady_clock::now();
            growGrass(job.frame);
            // (the drawing thread's own time: the list is read on its own thread now, beside the drawing - counted here it took
            // the pictures between frames down to one a frame for seconds at a time, at 120 on this PC's 240 Hz)
            poses::g_poses.preMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
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
                backend().setSpeedupsForced(0);
                backend().render(job.frame, stem + "_a" + ext);
                backend().setSpeedupsForced(1);
                backend().render(job.frame, stem + "_b" + ext);
                backend().setSpeedupsForced(-1);
            }
            else
                poses::render(job.frame, job.shot);
            if (latLogSeconds())
                latNote(job, drawStartNs, nowNs());
            const double ms = job.readMs + std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
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
            static double spent = 0.0, worst = 0.0;
            static uint32_t frames = 0u;
            spent += ms;
            worst = std::max(worst, ms);
            if (++frames == 300u)
            {
                std::fprintf(stderr, "[native render] render thread: %.2f ms a frame on average, %.2f at worst\n", spent / 300.0, worst);
                spent = worst = 0.0;
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
            ps2_thread_placement::place(ps2_thread_placement::Role::RenderRead); // (PS2X_THREAD_PLACEMENT)
            for (;;)
            {
                Job job;
                {
                    std::unique_lock<std::mutex> lock(g_mutex);
                    g_cv.wait(lock, [] { return !g_jobs.empty(); });
                    job = std::move(g_jobs.front());
                    g_jobs.pop_front();
                    g_reading = true;
                }
                g_room.notify_all();
                const auto start = std::chrono::steady_clock::now();
                if (latLogSeconds())
                    job.readStartNs = nowNs();
                expandFrame(job.frame);
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
            ps2_thread_placement::place(ps2_thread_placement::Role::Render); // (PS2X_THREAD_PLACEMENT)
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
                    const bool newer = i + 1 < batch.size();
                    const bool can = framesDropped() && droppable(job);
                    const int cap = tf::port::pictureCap();
                    const int64_t capNs = cap > 0 ? 1000000000ll / cap : 0;
                    const int64_t periodNs = job.frame.periodNs > 0 ? job.frame.periodNs : 0;
                    // (only a cap under the logic rate, and moments that go forward - a schedule started again goes back)
                    const bool overCap = can && capNs > periodNs && lastShownDue != 0 && job.frame.dueNs > lastShownDue &&
                                         job.frame.dueNs - lastShownDue < capNs - periodNs / 2;
                    const bool tested = can && test > 0 && ++testCount % test == 0;
                    if (can && (newer || overCap || tested))
                    {
                        (newer ? g_drops.behind : overCap ? g_drops.capped : g_drops.tested) += 1;
                        if (newer)
                            g_behindTotal.fetch_add(1u, std::memory_order_relaxed);
                        dropFrame(job, newer ? targetsOf(batch[i + 1].frame) : std::vector<uint32_t>(lastTargets));
                    }
                    else
                    {
                        if (job.frame.has3D)
                            lastTargets = targetsOf(job.frame);
                        drawJob(job);
                        ++g_drops.drawn;
                        if (job.frame.dueNs != 0)
                            lastShownDue = job.frame.dueNs;
                    }
                    recycleFrameBuffers(job.frame); // (its draws' meshes and textures let go of here, their memory kept)
                    job = Job{}; // (the frame's meshes and textures let go of here, not under the lock)
                }
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_busy = false;
                }
                g_room.notify_all();
                if (std::chrono::steady_clock::now() - saidAt >= std::chrono::seconds(10))
                {
                    saidAt = std::chrono::steady_clock::now();
                    if (g_drops.behind || g_drops.capped || g_drops.tested)
                        std::fprintf(stderr, "[native render] frames in 10 s: %llu drawn, left out %llu (renderer behind), %llu (the cap), %llu (TF_DROP_TEST); "
                                             "%llu draws of theirs kept (a render-to-texture of their own)\n",
                                     static_cast<unsigned long long>(g_drops.drawn), static_cast<unsigned long long>(g_drops.behind),
                                     static_cast<unsigned long long>(g_drops.capped), static_cast<unsigned long long>(g_drops.tested),
                                     static_cast<unsigned long long>(g_drops.keptDraws));
                    g_drops = Drops{};
                }
            }
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

    void recycleFrameBuffers(Frame &frame)
    {
        constexpr size_t kKeep = 3; // (frames in flight: the one being read, one drawn, one waiting)
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

    void takeChunkBuffer(std::vector<uint32_t> &chunks)
    {
        std::lock_guard<std::mutex> lock(g_spareMutex);
        if (g_spareChunks.empty() || chunks.capacity() > 0)
            return;
        chunks = std::move(g_spareChunks.back());
        g_spareChunks.pop_back();
    }

    double gameWaitMs() { return static_cast<double>(g_gameWaitNs.load(std::memory_order_relaxed)) / 1e6; }

    void submitFrame(Frame &&frame, const std::string &shot)
    {
        const int64_t handedNs = latLogSeconds() ? nowNs() : 0; // (TODO 169: TF_NATIVE_LATLOG)
        if (!renderOnThread())
        {
            if (framesDeferred())
            {
                // (TF_NATIVE_THREAD=0: the renderer thread's work, here and now - run()'s steps)
                expandFrame(frame);
                growGrass(frame);
                poses::render(frame, shot);
                return;
            }
            growGrass(frame);
            backend().render(frame, shot);
            return;
        }
        std::unique_lock<std::mutex> lock(g_mutex);
        if (!g_started)
        {
            g_started = true;
            std::thread(readLoop).detach();
            std::thread(drawLoop).detach();
        }
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
            job.submitNs = handedNs, job.inputNs = n5::lastInputNs();
        g_jobs.push_back(std::move(job));
        g_cv.notify_one();
    }
}

namespace nr
{
    unsigned long long framesLeftOutBehind() { return g_behindTotal.load(std::memory_order_relaxed); }
}
