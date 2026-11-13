// The game's own log (TODO 186).
//
// An installed copy is started by port\Play.bat with a plain `start` - nothing redirected - so a player had no log
// to send after a freeze (Jenvery: a runs folder, no playtest.err). Here, at the very start of main, when stderr is
// not already a file or a pipe (the playtest launcher's `2> runs\playtest.err` and the test scripts' redirection
// keep theirs), stdout and stderr are reopened on runs\play_<yyyyMMdd_HHmmss>.out / .err in the folder the game
// starts in, and the Windows standard handles set to them (what a restart started again inherits). The newest 10
// sessions are kept. No window is opened. TF_SELF_LOG=0: never.

#include "self_log.h"

#include "install_layout.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <io.h>
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace self_log
{
    namespace
    {
        namespace fs = std::filesystem;
        constexpr size_t kKeep = 10;

#if defined(_WIN32)
        // stderr already somewhere: a file or a pipe (a console, nothing or an unknown handle: no)
        bool redirected()
        {
            const HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
            if (h == nullptr || h == INVALID_HANDLE_VALUE)
                return false;
            const DWORD type = GetFileType(h);
            return type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE;
        }
#else
        // (the same on Linux: a file, a pipe or a socket keeps it - a terminal, /dev/null or nothing does not)
        bool redirected()
        {
            struct stat st{};
            return fstat(2, &st) == 0 && (S_ISREG(st.st_mode) || S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode));
        }
#endif

        // The oldest sessions' logs removed: the newest kKeep - 1 left, room for this one.
        void prune(const fs::path &dir)
        {
            std::error_code ec;
            std::vector<fs::path> logs;
            for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
            {
                const std::string name = it->path().filename().string();
                if (name.rfind("play_", 0) == 0 && it->path().extension() == ".err")
                    logs.push_back(it->path());
            }
            std::sort(logs.begin(), logs.end()); // (the names sort by their date)
            while (logs.size() >= kKeep)
            {
                fs::remove(logs.front(), ec);
                fs::path out = logs.front();
                fs::remove(out.replace_extension(".out"), ec);
                logs.erase(logs.begin());
            }
        }
    }

    void start()
    {
        if (const char *e = std::getenv("TF_SELF_LOG"); e && *e == '0')
            return;
        if (redirected())
            return;
        std::error_code ec;
        // (TODO 279: logs\ of the new folder layout when it is there, else runs\ as before)
        const fs::path dir = install_layout::isNew() ? install_layout::logsDir() : fs::path("runs");
        fs::create_directories(dir, ec);
        if (!fs::is_directory(dir, ec))
            return;
        prune(dir);
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        char stamp[32];
#if defined(_WIN32)
        localtime_s(&local, &now);
        std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &local);
        const std::string base = (dir / (std::string("play_") + stamp)).string();
        // (freopen, not freopen_s: the secure one opens a file for writing with no sharing at all, so nothing could
        // read the log of the running game - not the player, and not "Make a report", TODO 279)
#pragma warning(push)
#pragma warning(disable : 4996)
        FILE *err = std::freopen((base + ".err").c_str(), "w", stderr);
        if (!err)
            return;
        FILE *out = std::freopen((base + ".out").c_str(), "w", stdout);
#pragma warning(pop)
        SetStdHandle(STD_ERROR_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stderr))));
        if (out)
            SetStdHandle(STD_OUTPUT_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdout))));
#else
        localtime_r(&now, &local);
        std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &local);
        const std::string base = (dir / (std::string("play_") + stamp)).string();
        // (glibc's freopen keeps the descriptors 1 and 2: a process started again inherits the files)
        if (!std::freopen((base + ".err").c_str(), "w", stderr))
            return;
        std::freopen((base + ".out").c_str(), "w", stdout);
#endif
        std::fprintf(stderr, "[log] this session's log: %s.err (TF_SELF_LOG=0: none)\n", base.c_str());
    }
}
