// The HD textures while they are still being made (TODO 219): hd_progress.h.
// (Its own file: windows.h and raylib.h do not go in one translation unit.)

#include "hd_progress.h"
#include "graphics_driver.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;

namespace nr
{
    std::filesystem::path hdTextureFolder(); // (nr_hires.cpp)
    bool crispFontsFound();                  // (nr_hires.cpp)
    std::string hdSetAside();                // (nr_hires.cpp, TODO 259)
}

namespace hd_progress
{
    namespace
    {
        // (TODO 281) A whole set's making time, in minutes, beside the game, per kind of graphics card (makeMinutes).
        // Measured 2026-11-11, a Best set from scratch: RX 7900 XTX 32.7 min, RX 5700 XT 57.5 min; beside a running
        // game (a level, about 100 fps) the 7900 XTX made 735 images in 12 minutes against 758 alone - 3% slower. The
        // ranges reach down to the slower cards of each kind (RX 6800 / RTX 3060; GTX 1060 / 1650).
        constexpr int MINUTES_FAST_LO = 30, MINUTES_FAST_HI = 60;
        constexpr int MINUTES_MID_LO = 55, MINUTES_MID_HI = 120;
        constexpr int MINUTES_ANY_LO = 45, MINUTES_ANY_HI = 240;
        std::atomic<uint64_t> g_generation{0};
        std::mutex g_mutex;
        std::string g_line;
        std::string g_corner; // (TODO 267: cornerLine's, while the set is being made)
        fs::path g_dir;
        // (TODO 267) A set seen unfinished this run that is done now (its time), or sharp text made meanwhile; and a
        // maker at work now (the watcher's, every 5 s).
        std::atomic<bool> g_ready{false};
        std::atomic<bool> g_fontsReady{false};
        std::atomic<bool> g_makerAlive{false};
        std::atomic<int64_t> g_readyAtMs{0};

        int64_t nowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // 1793 -> "1,793"
        std::string grouped(long long n)
        {
            std::string digits = std::to_string(n < 0 ? 0 : n), out;
            for (size_t i = 0; i < digits.size(); ++i)
            {
                if (i > 0 && (digits.size() - i) % 3 == 0)
                    out.push_back(',');
                out.push_back(digits[i]);
            }
            return out;
        }

        struct Status
        {
            bool ok = false;
            std::string state;
            long long done = 0, total = 0;
        };

        // A field of the status file (a flat JSON object the tools write): its text, quotes taken off.
        std::string field(const std::string &text, const char *name)
        {
            const std::string key = std::string("\"") + name + "\"";
            size_t at = text.find(key);
            if (at == std::string::npos)
                return {};
            at = text.find(':', at + key.size());
            if (at == std::string::npos)
                return {};
            ++at;
            while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\r' || text[at] == '\n'))
                ++at;
            if (at < text.size() && text[at] == '"')
            {
                const size_t end = text.find('"', at + 1);
                return end == std::string::npos ? std::string() : text.substr(at + 1, end - at - 1);
            }
            size_t end = at;
            while (end < text.size() && text[end] != ',' && text[end] != '}' && text[end] != '\r' && text[end] != '\n')
                ++end;
            return text.substr(at, end - at);
        }

#if defined(__linux__)
        // Older Linux makers finished the colours and fonts but left the status at "upscaled".
        void finishLegacySet(const fs::path &file, Status &s, std::string &text)
        {
            if (s.state != "upscaled" || s.total <= 0 || s.done != s.total)
                return;
            const fs::path dir = file.parent_path();
            std::error_code ec;
            const auto upscaledAt = fs::last_write_time(file, ec);
            if (ec)
                return;
            if (fs::file_size(dir / "upscaled_precolour" / "colour_gains.json", ec) == 0 || ec)
                return;
            const auto coloursAt = fs::last_write_time(dir / "upscaled_precolour" / "colour_gains.json", ec);
            if (ec || coloursAt < upscaledAt)
                return;
            // Sharp fonts may already have been installed before the upscale.
            if (fs::file_size(dir / "crisp_fonts" / "manifest.json", ec) == 0 || ec)
                return;
            // The colour ledger ends the offline match; the renderer's BC7 cache is not a finishing step.
            const size_t at = text.find("\"upscaled\"", text.find("\"state\""));
            if (at == std::string::npos)
                return;
            text.replace(at, sizeof("\"upscaled\"") - 1, "\"done\"");
            fs::path tmp = file;
            tmp += ".finished.tmp";
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out << text;
            out.close();
            if (out)
                fs::rename(tmp, file, ec);
            if (!out || ec)
            {
                fs::remove(tmp, ec);
                std::fprintf(stderr, "[hd textures] could not mark the finished Linux set done\n");
                return;
            }
            s.state = "done";
            std::fprintf(stderr, "[hd textures] the older Linux set's colours and fonts were already finished: marked done\n");
        }
#endif

        Status readStatus(const fs::path &file)
        {
            Status s;
            std::ifstream in(file, std::ios::binary);
            if (!in)
                return s;
            std::stringstream ss;
            ss << in.rdbuf();
            std::string text = ss.str();
            s.state = field(text, "state");
            s.done = std::atoll(field(text, "done").c_str());
            s.total = std::atoll(field(text, "total").c_str());
            s.ok = !s.state.empty();
#if defined(__linux__)
            in.close();
            finishLegacySet(file, s, text);
#endif
            return s;
        }

        // (card_broken, TODO 242: this graphics card's upscales come out broken in every way the upscaler has, so
        // nothing is made on it - finished as far as the game goes; F1 > Make HD textures tries the card again)
        bool finished(const Status &s)
        {
            return s.ok && (s.state == "done" || s.state == "card_broken");
        }

        void setLine(const Status &s)
        {
            std::string line;
            if (s.ok && s.state == "card_broken")
                line = "HD TEXTURES: NOT MADE (THIS GRAPHICS CARD'S COME OUT BLACK)";
            else if (s.ok && s.state == "checking" && s.total > 0)
            {
                char text[96];
                std::snprintf(text, sizeof text, "CHECKING HD TEXTURES %lld OF %lld", s.done, s.total);
                line = text;
            }
            else if (s.ok && s.state != "done" && s.total > 0)
            {
                char text[96];
                std::snprintf(text, sizeof text, "HD TEXTURES %lld OF %lld%s", s.done, s.total,
                              s.state == "paused" ? " (PAUSED WHILE YOU PLAY)" : "");
                line = text;
            }
            // (TODO 267) The corner's, in plain words; the counts as the status file has them.
            std::string corner;
            if (s.ok && s.total > 0 && s.state != "done" && s.state != "card_broken")
                corner = s.state != "checking" && s.done >= s.total
                             ? std::string("Finishing HD textures") // (all made: their colours matched, the sharp text again)
                             : std::string(s.state == "checking" ? "Checking HD textures " : "HD textures ") + grouped(s.done) + " / " +
                                   grouped(s.total) + (s.state == "paused" ? ": paused while you play (carries on when you close the game)" : "");
            std::lock_guard<std::mutex> lock(g_mutex);
            g_line = std::move(line);
            g_corner = std::move(corner);
        }

#if defined(_WIN32)
        HANDLE g_running = INVALID_HANDLE_VALUE;
        DWORD g_ourMaker = 0; // (the window this game started: closed by the player, it is not started again)
        enum class Mode { Make, Verify };
        std::atomic<Mode> g_ourMode{Mode::Make}; // (TODO 281: what that window does)
        // (TODO 267, the user's choice: the HD textures are made only while the game is open) The game's maker window,
        // and everything it starts (Python, the upscaler), in a job of the game's that kills them when it closes - the
        // game's end too, a crash included. Images are written whole (a file beside, then swapped in) and the lock goes
        // with its process, so a stop leaves no half-made image and no stale lock; the next start carries on.
        HANDLE g_makerJob = nullptr;
        std::atomic<bool> g_stopped{false}; // (F1 > Stop: none made for the rest of this session)

        HANDLE makerJob()
        {
            if (g_makerJob)
                return g_makerJob;
            g_makerJob = CreateJobObjectW(nullptr, nullptr);
            if (g_makerJob)
            {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
                info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                SetInformationJobObject(g_makerJob, JobObjectExtendedLimitInformation, &info, sizeof info);
            }
            return g_makerJob;
        }

        bool pidAlive(DWORD pid)
        {
            if (!pid)
                return false;
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!h)
                return false;
            DWORD code = 0;
            const bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
            CloseHandle(h);
            return alive;
        }

        // Whether a maker holds hd_textures\.upscaling (its process id inside; deleted when it ends).
        bool makerRunning(const fs::path &dir, DWORD *pidOut = nullptr)
        {
            const fs::path lock = dir / ".upscaling";
            std::error_code ec;
            if (!fs::exists(lock, ec))
                return false;
            HANDLE h = CreateFileW(lock.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
                return true; // (there, and not to be read: held)
            char buf[32] = {};
            DWORD got = 0;
            ReadFile(h, buf, sizeof buf - 1, &got, nullptr);
            CloseHandle(h);
            const DWORD pid = static_cast<DWORD>(std::strtoul(buf, nullptr, 10));
            if (pidOut)
                *pidOut = pid;
            return pid ? pidAlive(pid) : true;
        }

        // The minimized window that carries on with the set (make_hd_textures.ps1 -Resume; TODO 281: -Verify, -Remake).
        void startMaker(const fs::path &game, const wchar_t *mode = L"-Resume")
        {
            const fs::path script = game / "tools" / "make_hd_textures.ps1";
            std::error_code ec;
            if (!fs::exists(script, ec))
                return;
            std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File \"" + script.wstring() + L"\" " + mode + L" -GamePid " +
                               std::to_wstring(GetCurrentProcessId());
            STARTUPINFOW si{};
            si.cb = sizeof si;
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_SHOWMINNOACTIVE;
            PROCESS_INFORMATION pi{};
            const std::wstring dir = game.wstring();
            // (TODO 267: in the game's own maker job - started suspended, put in the job, then let go - so that it ends
            // with the game; out of a launcher's job where it can, which nests on older Windows only with breakaway)
            for (const DWORD extra : {static_cast<DWORD>(CREATE_BREAKAWAY_FROM_JOB), static_cast<DWORD>(0)})
            {
                std::wstring line = cmd;
                if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE,
                                   CREATE_NEW_CONSOLE | BELOW_NORMAL_PRIORITY_CLASS | CREATE_SUSPENDED | extra, nullptr, dir.c_str(), &si, &pi))
                {
                    const HANDLE job = makerJob();
                    const bool held = job && AssignProcessToJobObject(job, pi.hProcess);
                    ResumeThread(pi.hThread);
                    g_ourMaker = pi.dwProcessId;
                    g_ourMode = std::wstring(mode) == L"-Verify" ? Mode::Verify : Mode::Make;
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                    std::fprintf(stderr, "[hd textures] the set is not finished: a window makes the rest while the game is open%s\n",
                                 held ? "" : " (not held by the game: it stops on its own when the game closes)");
                    return;
                }
            }
            std::fprintf(stderr, "[hd textures] the window to carry on with the set could not be started (error %lu)\n", GetLastError());
        }
#endif

        fs::path findDir()
        {
            std::error_code ec;
            if (fs::is_directory(fs::path("hd_textures"), ec))
                return fs::absolute(fs::path("hd_textures"), ec);
            return {};
        }

        std::atomic<bool> g_watching{false};

        // (fromNow: the state the file holds now is not news - a set done already, checked again from the F1 menu)
        void watch(fs::path status, bool helper, bool fromNow)
        {
            fs::file_time_type last{};
            if (fromNow)
            {
                std::error_code ec0;
                last = fs::last_write_time(status, ec0);
            }
            // (A maker seen at work that went away while the set is unfinished - the setup's window closed during play:
            // the game's own window carries on, at most three times a run.)
            bool seenMaker = false;
            int relaunched = 0;
            // (TODO 267: whether this run saw the set unfinished - then its "done" is news: restart to use it - and the
            // sharp text as it was when the watch began)
            bool sawUnfinished = !fromNow;
            const bool fontsBefore = nr::crispFontsFound();
            for (unsigned n = 1;; ++n)
            {
                std::this_thread::sleep_for(std::chrono::seconds(5));
#if defined(_WIN32)
                g_makerAlive = makerRunning(g_dir);
#endif
                if (!fontsBefore && !g_fontsReady && nr::crispFontsFound())
                {
                    g_fontsReady = true;
                    g_readyAtMs = nowMs();
                    std::fprintf(stderr, "[hd textures] the sharp text is made: a restart uses it\n");
                }
#if defined(_WIN32)
                if (helper && n % 6u == 0u)
                {
                    DWORD pid = 0;
                    if (makerRunning(g_dir, &pid))
                        seenMaker = pid != g_ourMaker; // (a window of our own closed: the player's choice)
                    else if (seenMaker && relaunched < 3 && !g_stopped)
                    {
                        seenMaker = false;
                        ++relaunched;
                        startMaker(g_dir.parent_path());
                    }
                }
#endif
                std::error_code ec;
                const fs::file_time_type t = fs::last_write_time(status, ec);
                if (ec || t == last)
                    continue;
                last = t;
                const Status s = readStatus(status);
                setLine(s);
                g_generation.fetch_add(1, std::memory_order_relaxed);
                if (!finished(s))
                    sawUnfinished = true;
                if (finished(s) && s.state == "done" && sawUnfinished)
                {
                    g_ready = true;
                    g_readyAtMs = nowMs();
                }
                if (finished(s))
                {
                    std::fprintf(stderr, s.state == "done" ? "[hd textures] the set is done (%lld images)\n"
                                                           : "[hd textures] none made: this graphics card's come out broken (%lld images)\n",
                                 s.total);
                    g_watching = false;
                    g_makerAlive = false;
                    return;
                }
            }
        }
    }

    namespace
    {
        // The sign that the game runs, for the upscaler (deleted by Windows when the game ends, a crash too).
        void holdRunningSign()
        {
#if defined(_WIN32)
            if (g_running != INVALID_HANDLE_VALUE)
                return;
            const fs::path running = g_dir / ".game_running";
            g_running = CreateFileW(running.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
            if (g_running != INVALID_HANDLE_VALUE)
            {
                const std::string pid = std::to_string(GetCurrentProcessId());
                DWORD wrote = 0;
                WriteFile(g_running, pid.data(), static_cast<DWORD>(pid.size()), &wrote, nullptr);
                FlushFileBuffers(g_running);
            }
#endif
        }

        void startWatch(bool helper, bool fromNow)
        {
            if (g_watching.exchange(true))
                return;
            const fs::path status = g_dir / "upscale_status.json";
            std::thread([status, helper, fromNow] { watch(status, helper, fromNow); }).detach();
        }
    }

    void start()
    {
        g_dir = findDir();
        if (g_dir.empty())
            return;
        // (User decision 2026-11-11) The card, for the HD tools (make_hd_textures.ps1 reads it: on built-in or low-end
        // graphics it makes nothing, and an update takes away a set made before).
        graphics_driver::writeGpuFile(g_dir.string());
        if (graphics_driver::lowEnd())
        {
            std::fprintf(stderr, "[hd textures] built-in or low-end graphics: no HD textures are made here\n");
            return;
        }
        holdRunningSign();
        const fs::path status = g_dir / "upscale_status.json";
        const Status s = readStatus(status);
        if (!s.ok)
        {
#if defined(_WIN32)
            // (TODO 267) No status yet but a maker at work - the setup's "Play now" in its first steps (the disc's
            // archives, the conversion, the sharp text): followed, so that the corner shows it and its counts.
            if (makerRunning(g_dir))
            {
                std::fprintf(stderr, "[hd textures] being made (its first steps)\n");
                startWatch(false, false);
            }
#endif
            return; // (else one made before the status file: nothing to do)
        }
        setLine(s);
        // (TODO 242) A finished set made before the maker checked its images (black ones from a GTX 16-series card):
        // checked once, in the window that carries on - the maker's own .images_checked_3 says it was.
#if defined(_WIN32)
        std::error_code ec;
        const bool unchecked = s.state == "done" && fs::exists(g_dir / "upscaled" / "upscale_manifest.jsonl", ec) &&
                               !fs::exists(g_dir / ".images_checked_3", ec);
#else
        // (no window carries on here to check it: make_hd_textures.sh checks a set once when it runs. Watched, the
        // finished set only read as news: "HD textures ready, restart" at every start)
        const bool unchecked = false;
#endif
        if (finished(s) && !unchecked)
            return; // (a finished set: nothing to do)
        std::fprintf(stderr, unchecked ? "[hd textures] checking the set's images once (%lld of %lld, %s)\n"
                                       : "[hd textures] being made: %lld of %lld (%s)\n",
                     s.done, s.total, s.state.c_str());
        const char *off = std::getenv("TF_HD_HELPER");
        const bool helper = !(off && *off == '0');
#if defined(_WIN32)
        if (helper && !makerRunning(g_dir))
            startMaker(g_dir.parent_path());
#endif
        startWatch(helper, false);
    }

    std::string makeLabel()
    {
#if defined(_WIN32)
        // (TODO 267: the game's own window at work - Stop; stopped this session - Resume; TODO 281: a verify at work)
        if (!g_stopped && g_ourMaker && pidAlive(g_ourMaker))
            return g_ourMode == Mode::Verify ? "Verifying HD textures" : "Stop making HD textures";
        if (g_stopped)
            return "Resume making HD textures";
#endif
        std::error_code ec;
        const fs::path dir = g_dir.empty() ? fs::absolute(fs::path("hd_textures"), ec) : g_dir;
        const Status s = readStatus(dir / "upscale_status.json");
        // (TODO 281) A finished set: Verify (it reports, nothing changed). A set this card made broken, or set aside
        // (TODO 242 / 259): Make tries the card again (after a driver update). None, or unfinished: Make.
        const bool made = s.ok ? (finished(s) && s.state == "done") : fs::is_directory(dir / "upscaled", ec);
        return made && nr::hdSetAside().empty() ? "Verify HD textures" : "Make HD textures";
    }

    std::string makeHdTextures()
    {
        if (graphics_driver::lowEnd())
            return "Not on this PC: its graphics are built-in or low-end, and making HD textures would make the game stutter or crash.";
#if defined(_WIN32)
        // (TODO 281) A verify at work: let it finish (it changes nothing; F1 shows its result).
        if (g_ourMaker && pidAlive(g_ourMaker) && g_ourMode == Mode::Verify)
            return "Verifying: the result shows here when it is done.";
        // (TODO 267) The button's Stop: the game's maker window and what it started end now (the job's), for this session.
        if (!g_stopped && g_ourMaker && pidAlive(g_ourMaker) && g_makerJob)
        {
            g_stopped = true;
            TerminateJobObject(g_makerJob, 0);
            CloseHandle(g_makerJob);
            g_makerJob = nullptr;
            g_ourMaker = 0;
            g_makerAlive = false;
            std::fprintf(stderr, "[hd textures] stopped for this session (F1); the next start carries on\n");
            return "Stopped. What is made is kept; Resume (or the next start) carries on.";
        }
        g_stopped = false;
        std::error_code ec;
        if (g_dir.empty())
        {
            fs::create_directories(fs::path("hd_textures"), ec);
            g_dir = fs::absolute(fs::path("hd_textures"), ec);
        }
        if (!fs::exists(g_dir.parent_path() / "tools" / "make_hd_textures.ps1", ec))
            return "Not here: tools\\make_hd_textures.ps1 is missing";
        holdRunningSign(); // (so that the upscale waits while the game runs)
        if (makerRunning(g_dir))
            return "Being made already, in its own window";
        startMaker(g_dir.parent_path());
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_line.empty())
                g_line = "HD TEXTURES: STARTING";
        }
        startWatch(true, true);
        std::ifstream o(g_dir / "hd_options.json", std::ios::binary);
        std::stringstream ss;
        ss << o.rdbuf();
        std::string t = ss.str();
        t.erase(std::remove_if(t.begin(), t.end(), [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }), t.end());
        const bool whilePlaying = t.find("\"while_playing\":true") != std::string::npos;
        (void)whilePlaying; // (TODO 267: the game's own window never waits for the game - it is made while the game is open)
        return "Making them in a small window while the game is open. Closing the game stops it; the next start carries on.";
#else
        return "Run tools/make_hd_textures.sh";
#endif
    }

    // ---- TODO 281: the card before a first make, Verify HD textures, Remake N broken images ----

    namespace
    {
        // The graphics card the upscaler works on (the first one Windows lists that is not a software one).
        std::string cardName()
        {
#if defined(_WIN32)
            static const std::string name = []() -> std::string {
                std::string out;
                IDXGIFactory1 *factory = nullptr;
                if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) && factory)
                {
                    IDXGIAdapter1 *adapter = nullptr;
                    for (UINT i = 0; out.empty() && factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
                    {
                        DXGI_ADAPTER_DESC1 d{};
                        if (SUCCEEDED(adapter->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
                        {
                            char buf[256] = {};
                            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, buf, sizeof buf - 1, nullptr, nullptr);
                            out = buf;
                        }
                        adapter->Release();
                    }
                    factory->Release();
                }
                return out;
            }();
            return name;
#else
            return {};
#endif
        }

        bool has(const std::string &text, const char *part) { return text.find(part) != std::string::npos; }

        // How long a whole set takes on this card, while the game runs beside it (MINUTES_*: measured, see TODO 281).
        void makeMinutes(const std::string &card, int &lo, int &hi)
        {
            std::string c = card;
            std::transform(c.begin(), c.end(), c.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
            // (recent cards: AMD RX 6800 and up, RX 7000 / 9000; NVIDIA RTX 30 / 40 / 50)
            const bool fast = (has(c, "RX 68") || has(c, "RX 69") || has(c, "RX 7") || has(c, "RX 9")) ||
                              (has(c, "RTX") && (has(c, " 30") || has(c, " 40") || has(c, " 50")));
            // (the generation before: AMD RX 5000 / RX 6400 - 6750; NVIDIA RTX 20, GTX 16, GTX 10)
            const bool mid = has(c, "RX 5") || has(c, "RX 6") || has(c, "RTX 20") || has(c, "GTX 16") || has(c, "GTX 10");
            if (fast)
                lo = MINUTES_FAST_LO, hi = MINUTES_FAST_HI;
            else if (mid)
                lo = MINUTES_MID_LO, hi = MINUTES_MID_HI;
            else
                lo = MINUTES_ANY_LO, hi = MINUTES_ANY_HI; // (any other card, laptops' and older ones too: a wide range)
        }

        std::string readText(const fs::path &file)
        {
            std::ifstream in(file, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            return ss.str();
        }
    }

    bool needsMakeCard()
    {
        std::error_code ec;
        const fs::path dir = g_dir.empty() ? fs::absolute(fs::path("hd_textures"), ec) : g_dir;
        if (readStatus(dir / "upscale_status.json").ok)
            return false; // (begun, or made: carried on without asking again)
        return !fs::is_directory(dir / "upscaled", ec);
    }

    MakeCost makeCost()
    {
        MakeCost c;
        // (measured 2026-11-11, a whole Best set: 7.5 GB in hd_textures - 5.6 the images, 1.6 their colour-match originals,
        // the rest the disc's archives and the fonts - and the game's compressed copies of them, about 0.9 GB)
        c.needGb = 8.5;
        std::error_code ec;
        const fs::space_info space = fs::space(fs::current_path(ec), ec);
        c.freeGb = ec ? -1.0 : static_cast<double>(space.available) / (1024.0 * 1024.0 * 1024.0);
        c.card = cardName();
        makeMinutes(c.card, c.minutesLo, c.minutesHi);
        return c;
    }

    VerifyState verifyState()
    {
        static std::chrono::steady_clock::time_point at{};
        static VerifyState cached;
        const auto now = std::chrono::steady_clock::now();
        if (at != std::chrono::steady_clock::time_point{} && now - at < std::chrono::seconds(1))
            return cached;
        at = now;
        VerifyState v;
        std::error_code ec;
        const fs::path dir = g_dir.empty() ? fs::absolute(fs::path("hd_textures"), ec) : g_dir;
        const std::string json = readText(dir / "verify_report.json");
        v.state = field(json, "state");
        if (v.state == "running")
        {
            v.done = std::atoll(field(json, "done").c_str());
            v.total = std::atoll(field(json, "total").c_str());
        }
        else if (v.state == "done")
        {
            v.remake = std::atoi(field(json, "remake").c_str());
            v.intact = field(json, "intact") == "true";
            v.finished = field(json, "finished");
            std::istringstream lines(readText(dir / "verify_report.txt"));
            for (std::string l; std::getline(lines, l);)
            {
                if (!l.empty() && l.back() == '\r')
                    l.pop_back();
                if (!l.empty())
                    v.lines.push_back(l);
            }
        }
#if defined(_WIN32)
        // (a verify that ended without a report - the window closed: not "running" any more)
        if (v.state == "running" && !(g_ourMaker && pidAlive(g_ourMaker) && g_ourMode == Mode::Verify) && !makerRunning(dir))
            v.state = "stopped";
#endif
        cached = v;
        return v;
    }

    namespace
    {
        // (Verify / Remake: the game's small window, as Make starts it)
        std::string startMode(const wchar_t *mode)
        {
#if defined(_WIN32)
            std::error_code ec;
            if (g_dir.empty())
                return "Not here: there are no HD textures";
            if (!fs::exists(g_dir.parent_path() / "tools" / "make_hd_textures.ps1", ec))
                return "Not here: tools\\make_hd_textures.ps1 is missing";
            if (makerRunning(g_dir))
                return "Being made already, in its own window";
            holdRunningSign();
            g_stopped = false;
            startMaker(g_dir.parent_path(), mode);
            return {};
#else
            // (Linux) F1 names the command; make_hd_textures.sh passes the same options to upscale_textures.py.
            return std::wstring(mode) == L"-Verify" ? "Run tools/make_hd_textures.sh --verify" : "Run tools/make_hd_textures.sh --remake";
#endif
        }
    }

    std::string verifyHdTextures()
    {
        const std::string why = startMode(L"-Verify");
        if (!why.empty())
            return why;
        std::fprintf(stderr, "[hd textures] verifying the set (F1): nothing is changed\n");
        return "Verifying in a small window. Nothing is changed; the result shows here.";
    }

    std::string remakeBroken()
    {
        const std::string why = startMode(L"-Remake");
        if (!why.empty())
            return why;
        std::fprintf(stderr, "[hd textures] remaking the images the last verify found (F1)\n");
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_line.empty())
                g_line = "HD TEXTURES: STARTING";
        }
        startWatch(true, true);
        return "Making them again in a small window while the game is open.";
    }

    uint64_t generation() { return g_generation.load(std::memory_order_relaxed); }

    std::string line()
    {
        // (TODO 259) A set that was set aside: said before everything else (asked every 2 s, the marker is a file).
        {
            static std::chrono::steady_clock::time_point at{};
            static bool aside = false;
            const auto now = std::chrono::steady_clock::now();
            if (at == std::chrono::steady_clock::time_point{} || now - at > std::chrono::seconds(2))
            {
                at = now;
                aside = !nr::hdSetAside().empty();
            }
            if (aside)
                return "HD TEXTURES: NOT USED. THIS GRAPHICS CARD MAKES BROKEN HD IMAGES. AFTER A DRIVER UPDATE: MAKE HD TEXTURES";
        }
        if (g_ready)
            return "HD TEXTURES READY: RESTART TO USE THEM";
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_line.empty() && g_fontsReady)
            return "SHARP TEXT READY: RESTART TO USE IT";
        return g_line;
    }

    std::string cornerLine()
    {
        // (TF_HD_CORNER_TEST=<text> (tests): that text, to see the corner without a set being made)
        static const char *test = std::getenv("TF_HD_CORNER_TEST");
        if (test && *test)
            return test;
        // (the counts first; a finished set's note, or the sharp text's, for a minute; nothing while stopped from F1)
#if defined(_WIN32)
        if (g_stopped)
            return {};
#endif
        const bool recent = nowMs() - g_readyAtMs.load() < 60000;
        if (g_ready)
            return recent ? "HD textures ready. Restart to use them (F1)" : std::string();
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!g_corner.empty())
                return g_corner;
        }
        if (g_fontsReady)
            return recent ? "Sharp text ready. Restart to use it (F1)" : std::string();
        // (the maker's first minute: the sharp text, before any count)
        if (g_makerAlive && !crispFontsFound())
            return "Making sharp text";
        if (g_makerAlive && g_watching)
            return "Making HD textures";
        return {};
    }

    bool readyToRestart()
    {
        return g_ready || g_fontsReady;
    }

    bool crispFontsFound()
    {
        static std::chrono::steady_clock::time_point at{};
        static bool found = false;
        const auto now = std::chrono::steady_clock::now();
        if (at == std::chrono::steady_clock::time_point{} || now - at > std::chrono::seconds(2))
        {
            at = now;
            found = nr::crispFontsFound();
        }
        return found;
    }

    std::string openTextureFolder()
    {
        const fs::path folder = nr::hdTextureFolder();
        if (folder.empty())
            return "No texture pack yet";
        std::error_code ec;
        const fs::path dir = fs::absolute(folder, ec);
        fs::path open = fs::is_directory(dir, ec) ? dir : fs::path();
        if (open.empty() && fs::is_directory(dir.parent_path(), ec))
            open = dir.parent_path();
#if defined(_WIN32)
        if (!open.empty())
        {
            const std::wstring w = open.wstring();
            ShellExecuteW(nullptr, L"explore", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
#endif
        std::fprintf(stderr, "[hd textures] texture folder: %s%s\n", dir.string().c_str(), open == dir ? "" : " (not there yet)");
        return open == dir ? std::string() : "No texture pack yet";
    }
}
