// A lost graphics device: the game saved and started again from that moment (gpu_recovery.h, TODO 291).
#include "gpu_recovery.h"

#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_settings.h"
#include "../native_render/nr_backend.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gpu_recovery
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        enum class Phase
        {
            Watching,  // the device as it should be
            Saving,    // the moment being saved for the restart (ps2_settings::restartNow)
            Restarted, // the new process started: this one is closing
            GivenUp    // no restart (off, the session's restarts used up, or no moment could be saved): emulated drawing
        };

        Phase g_phase = Phase::Watching;
        Clock::time_point g_lostAt{}, g_askedAt{};
        int g_tries = 0;

        std::mutex g_messageMutex;
        std::string g_message;
        Clock::time_point g_messageSince{};

        void setMessage(const std::string &m)
        {
            std::lock_guard<std::mutex> lock(g_messageMutex);
            g_message = m;
            g_messageSince = Clock::now();
        }

        int envInt(const char *name, int fallback)
        {
            const char *v = std::getenv(name);
            return v && *v ? std::atoi(v) : fallback;
        }

        // The restarts this play session had before this process (TF_GPU_RESTARTS, set for the process each one started;
        // read once - this process sets it for the one it starts).
        int restartsSoFar()
        {
            static const int n = envInt("TF_GPU_RESTARTS", 0);
            return n;
        }
        int restartsAllowed()
        {
            if (const char *v = std::getenv("TF_GPU_RECOVERY"); v && v[0] == '0' && !v[1])
                return 0;
            return std::max(0, envInt("TF_GPU_RECOVERY_MAX", 2));
        }

        void setEnv(const char *name, const char *value)
        {
#if defined(_WIN32)
            SetEnvironmentVariableA(name, value); // (null: removed - the new process's environment is this one's)
            if (value)
                _putenv_s(name, value);
            else
                _putenv_s(name, "");
#else
            if (value)
                setenv(name, value, 1);
            else
                unsetenv(name);
#endif
        }

        std::atomic<bool> g_givenUp{false};

        // (Once the new process runs, this one closes - the panel's restart asks it to stop, and the window loop ends
        // before another frame here. If its shutdown is held up - a thread still waiting on the lost device - it is ended
        // 20 s after the stop, so that two games never run on together. Started with the first save asked for; it ends
        // by itself when the recovery gives up.)
        void endLaterIfStuck(PS2Runtime &runtime)
        {
#if defined(_WIN32)
            std::thread([&runtime] {
                const auto begun = Clock::now();
                while (!runtime.isStopRequested())
                {
                    if (g_givenUp.load() || Clock::now() - begun > std::chrono::seconds(90))
                        return;
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
                std::this_thread::sleep_for(std::chrono::seconds(20));
                std::fprintf(stderr, "[gpu] the old process did not close in 20 s after the restart: ended\n");
                std::fflush(stderr);
                TerminateProcess(GetCurrentProcess(), 0);
            }).detach();
#else
            (void)runtime;
#endif
        }
    }

    std::string message(double &ageSeconds)
    {
        std::lock_guard<std::mutex> lock(g_messageMutex);
        ageSeconds = std::chrono::duration<double>(Clock::now() - g_messageSince).count();
        return g_message;
    }

    void frame(PS2Runtime &runtime)
    {
        EeScheduler &ee = runtime.eeScheduler();
        (void)restartsSoFar(); // (read before anything here sets it)
        // A process this one's recovery started: once the moment has loaded, the player is told.
        static bool told = false;
        if (!told && restartsSoFar() > 0 && ee.loadedStateCount() > 0u)
        {
            told = true;
            const int n = restartsSoFar();
            const char *lostAt = std::getenv("TF_GPU_LOST_AT");
            const double took = lostAt && *lostAt ? (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() -
                                                     std::strtoll(lostAt, nullptr, 10)) / 1000.0
                                                  : -1.0;
            std::fprintf(stderr, "[gpu] started again after the graphics device was lost (restart %d this session; %.1f s from the loss to the moment loaded)\n", n, took);
            setMessage(n <= 1 ? "The graphics driver restarted. The game recovered."
                              : "The graphics driver restarted again. The game recovered. If it keeps happening, lower the graphics settings (F1).");
        }

        switch (g_phase)
        {
        case Phase::Watching:
        {
            if (!nr::backend().deviceLost())
                return;
            g_lostAt = Clock::now();
            const int done = restartsSoFar(), allowed = restartsAllowed();
            if (done >= allowed)
            {
                g_phase = Phase::GivenUp;
                std::fprintf(stderr, "[gpu] the graphics device was lost; %s - the emulated renderer goes on drawing\n",
                             allowed == 0 ? "restarting is off (TF_GPU_RECOVERY)" : "the restarts of this session are used up");
                setMessage(allowed == 0 ? "The graphics driver restarted: the slower emulated renderer goes on."
                                        : "The graphics driver restarted again: the slower emulated renderer goes on. Lower the graphics settings (F1).");
                return;
            }
            std::fprintf(stderr, "[gpu] the graphics device was lost: the moment is saved and the game starts again from it (restart %d of %d)\n",
                         done + 1, allowed);
            // The new process: the count one more, DRED on (the next loss described in the log), no test save of its own.
            const std::string next = std::to_string(done + 1);
            setEnv("TF_GPU_RESTARTS", next.c_str());
            setEnv("TF_NATIVE_DRED", "1");
            // (when it was lost, on the wall clock: the new process logs how long the recovery took)
            const std::string lostAt = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
            setEnv("TF_GPU_LOST_AT", lostAt.c_str());
            setEnv("PS2X_SAVE_STATE_TICK", nullptr);
            setEnv("PS2X_SAVE_STATE", nullptr);
            g_tries = 1;
            g_askedAt = Clock::now();
            g_phase = Phase::Saving;
            endLaterIfStuck(runtime);
            ps2_settings::restartNow(runtime);
            return;
        }
        case Phase::Saving:
        {
            if (ps2_settings::restarting())
                return; // (the state being saved; the panel's restart gives up after 15 s and says why)
            const std::string failed = ps2_settings::restartMessage();
            if (failed.empty())
            {
                // Saved and started: the panel asked this process to stop (the window loop usually ends before this).
                g_phase = Phase::Restarted;
                return;
            }
            // Not now (a movie, a load, the memory card being written): asked again every few seconds, for up to 60 s.
            if (Clock::now() - g_lostAt < std::chrono::seconds(60))
            {
                if (Clock::now() - g_askedAt >= std::chrono::seconds(3))
                {
                    ++g_tries;
                    g_askedAt = Clock::now();
                    std::fprintf(stderr, "[gpu] the moment could not be saved (%s) - try %d\n", failed.c_str(), g_tries);
                    ps2_settings::restartNow(runtime);
                }
                return;
            }
            g_phase = Phase::GivenUp;
            g_givenUp.store(true);
            std::fprintf(stderr, "[gpu] no moment could be saved in 60 s (%s): the emulated renderer goes on drawing\n", failed.c_str());
            setMessage("The graphics driver restarted: the slower emulated renderer goes on. Restart the game for the native one.");
            return;
        }
        case Phase::Restarted:
        case Phase::GivenUp:
            return;
        }
    }
}
