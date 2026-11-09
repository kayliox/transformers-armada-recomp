// HD movies: the ffmpeg process that decodes an upscaled movie to raw frames (hd_movies.cpp has the
// rest). Its own file: windows.h and raylib.h do not go in one translation unit.

#include "hd_movies_decoder.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
extern char **environ;
#endif

namespace fs = std::filesystem;

namespace hd_movies_decoder
{
    namespace
    {
        constexpr size_t kAhead = 6u; // frames decoded ahead of the picture

        std::string ffmpegPath()
        {
            const char *env = std::getenv("TF_FFMPEG");
            return env && *env ? env : "ffmpeg";
        }

#if !defined(_WIN32)
        // A process with its stdout into a pipe (no shell: the file name needs no quoting); false if it cannot start.
        bool spawnReading(const std::vector<std::string> &args, pid_t &pid, int &readEnd)
        {
            int fds[2];
            if (pipe(fds) != 0)
                return false;
            posix_spawn_file_actions_t actions;
            posix_spawn_file_actions_init(&actions);
            posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
            posix_spawn_file_actions_addclose(&actions, fds[0]);
            posix_spawn_file_actions_addclose(&actions, fds[1]);
            std::vector<char *> argv;
            for (const std::string &a : args)
                argv.push_back(const_cast<char *>(a.c_str()));
            argv.push_back(nullptr);
            const int failed = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
            posix_spawn_file_actions_destroy(&actions);
            close(fds[1]);
            if (failed)
            {
                close(fds[0]);
                return false;
            }
            readEnd = fds[0];
            return true;
        }
#endif
    }

    struct Decoder::Impl
    {
        std::mutex mutex;
        std::deque<Frame> frames;
        std::atomic<bool> stop{false};
        std::thread reader;
#if defined(_WIN32)
        HANDLE process = nullptr;
        HANDLE pipe = nullptr;
        long readSome(uint8_t *to, size_t bytes)
        {
            DWORD n = 0;
            return ReadFile(pipe, to, static_cast<DWORD>(bytes), &n, nullptr) ? static_cast<long>(n) : -1;
        }
#else
        pid_t process = 0;
        int pipe = -1;
        long readSome(uint8_t *to, size_t bytes) { return static_cast<long>(read(pipe, to, bytes)); }
#endif

        void readFrames(int width, int height, double fps)
        {
            const size_t texels = static_cast<size_t>(width) * static_cast<size_t>(height), size = texels * 4u;
            uint64_t index = 0;
            while (!stop)
            {
                bool full;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    full = frames.size() >= kAhead;
                }
                if (full)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                Frame frame;
                frame.time = static_cast<double>(index) / fps;
                frame.rgba.resize(texels);
                uint8_t *bytes = reinterpret_cast<uint8_t *>(frame.rgba.data());
                size_t got = 0;
                while (got < size && !stop)
                {
                    const long n = readSome(bytes + got, size - got);
                    if (n <= 0)
                        break; // (the end of the movie, or stopped)
                    got += static_cast<size_t>(n);
                }
                if (got < size)
                    break;
                ++index;
                std::lock_guard<std::mutex> lock(mutex);
                frames.push_back(std::move(frame));
            }
        }
    };

    bool probe(const std::string &file, int height, int &width, double &fps)
    {
        width = (height * 5 / 3 + 1) & ~1; // (the source's 5:3 when ffprobe says nothing)
        fps = 30.0;
#if defined(_WIN32)
        std::string probe = "ffprobe";
        if (const char *env = std::getenv("TF_FFMPEG"); env && *env)
            probe = (fs::path(env).parent_path() / "ffprobe.exe").string();
        const std::string cmd = "\"\"" + probe + "\" -v error -select_streams v:0 -show_entries stream=width,height,r_frame_rate "
                                "-of csv=p=0 \"" + file + "\"\"";
        if (FILE *f = _popen(cmd.c_str(), "r"))
        {
            int w = 0, h = 0, num = 0, den = 1;
            const int n = std::fscanf(f, "%d,%d,%d/%d", &w, &h, &num, &den);
            _pclose(f);
            if (n >= 2 && w > 0 && h > 0)
            {
                width = (height * w / h + 1) & ~1;
                if (n >= 4 && num > 0 && den > 0)
                    fps = static_cast<double>(num) / den;
                return true;
            }
        }
#else
        std::string probe = "ffprobe";
        if (const char *env = std::getenv("TF_FFMPEG"); env && *env)
            probe = (fs::path(env).parent_path() / "ffprobe").string();
        pid_t pid = 0;
        int out = -1;
        if (spawnReading({probe, "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width,height,r_frame_rate",
                          "-of", "csv=p=0", file},
                         pid, out))
        {
            char text[128] = {};
            size_t got = 0;
            for (ssize_t n; got + 1 < sizeof(text) && (n = read(out, text + got, sizeof(text) - 1 - got)) > 0;)
                got += static_cast<size_t>(n);
            close(out);
            waitpid(pid, nullptr, 0);
            int w = 0, h = 0, num = 0, den = 1;
            const int n = std::sscanf(text, "%d,%d,%d/%d", &w, &h, &num, &den);
            if (n >= 2 && w > 0 && h > 0)
            {
                width = (height * w / h + 1) & ~1;
                if (n >= 4 && num > 0 && den > 0)
                    fps = static_cast<double>(num) / den;
                return true;
            }
        }
#endif
        return false;
    }

    Decoder::Decoder() : impl(std::make_unique<Impl>()) {}

    Decoder::~Decoder() { stop(); }

    bool Decoder::start(const std::string &file, int width, int height, double fps)
    {
        stop();
#if defined(_WIN32)
        const std::string cmd = "\"" + ffmpegPath() + "\" -v error -nostdin -i \"" + file + "\" -an -vf \"scale=" +
                                std::to_string(width) + ":" + std::to_string(height) +
                                ":flags=lanczos\" -f rawvideo -pix_fmt rgba -";
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE readEnd = nullptr, writeEnd = nullptr;
        if (!CreatePipe(&readEnd, &writeEnd, &sa, 1u << 22))
            return false;
        SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = writeEnd;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION pi{};
        std::string line = cmd;
        const BOOL ok = CreateProcessA(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(writeEnd);
        if (!ok)
        {
            CloseHandle(readEnd);
            std::fprintf(stderr, "[hd movies] cannot start %s (set TF_FFMPEG to ffmpeg.exe)\n", ffmpegPath().c_str());
            return false;
        }
        CloseHandle(pi.hThread);
        // (TODO 201) Above normal: with every core busy (a video encode, a build) at normal priority the decoder got its
        // share of the CPU with them and the movie fell to ~5 frames a second; above them it keeps the movie's pace (it
        // needs about a core), the rest of the PC slowed by that much while a movie plays.
        SetPriorityClass(pi.hProcess, ABOVE_NORMAL_PRIORITY_CLASS);
        impl->process = pi.hProcess;
        impl->pipe = readEnd;
        impl->stop = false;
        impl->reader = std::thread([this, width, height, fps] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL); // (TODO 201: with the decoder)
            impl->readFrames(width, height, fps);
        });
        return true;
#else
        pid_t pid = 0;
        int out = -1;
        if (!spawnReading({ffmpegPath(), "-v", "error", "-nostdin", "-i", file, "-an", "-vf",
                           "scale=" + std::to_string(width) + ":" + std::to_string(height) + ":flags=lanczos", "-f", "rawvideo",
                           "-pix_fmt", "rgba", "-"},
                          pid, out))
        {
            std::fprintf(stderr, "[hd movies] cannot start %s (set TF_FFMPEG to the ffmpeg program)\n", ffmpegPath().c_str());
            return false;
        }
        impl->process = pid;
        impl->pipe = out;
        impl->stop = false;
        impl->reader = std::thread([this, width, height, fps] { impl->readFrames(width, height, fps); });
        return true;
#endif
    }

    void Decoder::stop()
    {
        impl->stop = true;
#if defined(_WIN32)
        if (impl->process)
            TerminateProcess(impl->process, 0);
        if (impl->reader.joinable())
            impl->reader.join();
        if (impl->pipe)
            CloseHandle(impl->pipe);
        if (impl->process)
            CloseHandle(impl->process);
        impl->pipe = impl->process = nullptr;
#else
        if (impl->process > 0)
            kill(impl->process, SIGTERM);
        if (impl->reader.joinable())
            impl->reader.join();
        if (impl->pipe >= 0)
            close(impl->pipe);
        if (impl->process > 0)
            waitpid(impl->process, nullptr, 0);
        impl->pipe = -1;
        impl->process = 0;
#endif
        std::lock_guard<std::mutex> lock(impl->mutex);
        impl->frames.clear();
    }

    bool Decoder::take(double now, Frame &frame)
    {
        bool have = false;
        std::lock_guard<std::mutex> lock(impl->mutex);
        while (!impl->frames.empty() && impl->frames.front().time <= now)
        {
            frame = std::move(impl->frames.front());
            impl->frames.pop_front();
            have = true;
        }
        return have;
    }

    namespace
    {
        Decoder &fullWindow()
        {
            static Decoder decoder;
            return decoder;
        }
    }

    bool start(const std::string &file, int width, int height, double fps) { return fullWindow().start(file, width, height, fps); }
    void stop() { fullWindow().stop(); }
    bool take(double now, Frame &frame) { return fullWindow().take(now, frame); }
}
