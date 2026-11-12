// (TODO 279, release A) "Make a report": support_report.h has the long form.

#include "support_report.h"

#include "install_layout.h"
#include "ps2_runtime.h"
#include "updates.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi.h>
#include <shellapi.h>
#endif

namespace support_report
{
    namespace
    {
        namespace fs = std::filesystem;

        // A log is text; the start of it has the PC and the settings, the end has what went wrong.
        constexpr size_t kHead = 1u << 20, kTail = 7u << 20;
        constexpr size_t kSessions = 3;

        uint32_t crc32(const std::string &data)
        {
            static const std::array<uint32_t, 256> table = []
            {
                std::array<uint32_t, 256> t{};
                for (uint32_t i = 0; i < 256; ++i)
                {
                    uint32_t c = i;
                    for (int k = 0; k < 8; ++k)
                        c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    t[i] = c;
                }
                return t;
            }();
            uint32_t c = 0xFFFFFFFFu;
            for (unsigned char b : data)
                c = table[(c ^ b) & 0xFFu] ^ (c >> 8);
            return c ^ 0xFFFFFFFFu;
        }

        // A zip with its files stored as they are (no compression: nothing to link, and text logs are small).
        class Zip
        {
          public:
            void add(const std::string &name, const std::string &data)
            {
                Entry e{name, crc32(data), static_cast<uint32_t>(data.size()), static_cast<uint32_t>(m_out.size())};
                put32(0x04034B50u);
                header(e);
                m_out += name;
                m_out += data;
                m_entries.push_back(e);
            }
            bool write(const fs::path &file)
            {
                const uint32_t start = static_cast<uint32_t>(m_out.size());
                for (const Entry &e : m_entries)
                {
                    put32(0x02014B50u);
                    put16(20);
                    header(e);
                    put16(0); // comment
                    put16(0); // disk
                    put16(0); // internal attributes
                    put32(0); // external attributes
                    put32(e.offset);
                    m_out += e.name;
                }
                const uint32_t size = static_cast<uint32_t>(m_out.size()) - start;
                put32(0x06054B50u);
                put16(0);
                put16(0);
                put16(static_cast<uint16_t>(m_entries.size()));
                put16(static_cast<uint16_t>(m_entries.size()));
                put32(size);
                put32(start);
                put16(0);
                std::ofstream out(file, std::ios::binary | std::ios::trunc);
                out.write(m_out.data(), static_cast<std::streamsize>(m_out.size()));
                return out.good();
            }
            const std::vector<std::string> names() const
            {
                std::vector<std::string> n;
                for (const Entry &e : m_entries)
                    n.push_back(e.name);
                return n;
            }

          private:
            struct Entry
            {
                std::string name;
                uint32_t crc, size, offset;
            };
            void put16(uint16_t v)
            {
                m_out.push_back(static_cast<char>(v & 0xFF));
                m_out.push_back(static_cast<char>(v >> 8));
            }
            void put32(uint32_t v)
            {
                put16(static_cast<uint16_t>(v & 0xFFFF));
                put16(static_cast<uint16_t>(v >> 16));
            }
            // (the part the local and the central header share)
            void header(const Entry &e)
            {
                put16(20);     // version needed
                put16(0x0800); // names in UTF-8
                put16(0);      // stored
                put16(m_time);
                put16(m_date);
                put32(e.crc);
                put32(e.size);
                put32(e.size);
                put16(static_cast<uint16_t>(e.name.size()));
                put16(0); // extra
            }
            std::string m_out;
            std::vector<Entry> m_entries;

          public:
            uint16_t m_time = 0, m_date = 0x21;
        };

        // The file's text: all of it, or its first and last parts with a line between them.
        bool readText(const fs::path &file, std::string &text)
        {
            std::error_code ec;
            const uintmax_t size = fs::file_size(file, ec);
            if (ec)
                return false;
            std::ifstream in(file, std::ios::binary);
            if (!in)
                return false;
            if (size <= kHead + kTail)
            {
                text.resize(static_cast<size_t>(size));
                in.read(text.data(), static_cast<std::streamsize>(size));
                text.resize(static_cast<size_t>(in.gcount()));
                return true;
            }
            std::string head(kHead, '\0'), tail(kTail, '\0');
            in.read(head.data(), static_cast<std::streamsize>(kHead));
            in.seekg(static_cast<std::streamoff>(size - kTail));
            in.read(tail.data(), static_cast<std::streamsize>(kTail));
            tail.resize(static_cast<size_t>(in.gcount()));
            text = head + "\n\n[... the middle of this log is left out of the report ...]\n\n" + tail;
            return true;
        }

        std::string stampNow(const char *format)
        {
            const std::time_t t = std::time(nullptr);
            std::tm tm{};
#if defined(_WIN32)
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            char text[40];
            std::strftime(text, sizeof(text), format, &tm);
            return text;
        }

#if defined(_WIN32)
        std::string registryText(HKEY root, const char *key, const char *value)
        {
            char text[256];
            DWORD size = sizeof(text);
            if (RegGetValueA(root, key, value, RRF_RT_REG_SZ, nullptr, text, &size) != ERROR_SUCCESS)
                return "";
            return text;
        }

        std::string narrow(const wchar_t *w)
        {
            const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
            if (n <= 1)
                return "";
            std::string s(static_cast<size_t>(n - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
            return s;
        }
#endif

        std::string pcDetails()
        {
            std::string t;
#if defined(_WIN32)
            const char *nt = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
            t += "Windows: " + registryText(HKEY_LOCAL_MACHINE, nt, "ProductName") + " " + registryText(HKEY_LOCAL_MACHINE, nt, "DisplayVersion") +
                 ", build " + registryText(HKEY_LOCAL_MACHINE, nt, "CurrentBuild") + "\n";
            t += "Processor: " + registryText(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString");
            SYSTEM_INFO si{};
            GetSystemInfo(&si);
            t += " (" + std::to_string(si.dwNumberOfProcessors) + " threads)\n";
            MEMORYSTATUSEX mem{};
            mem.dwLength = sizeof(mem);
            if (GlobalMemoryStatusEx(&mem))
                t += "Memory: " + std::to_string(mem.ullTotalPhys >> 20) + " MB, " + std::to_string(mem.ullAvailPhys >> 20) + " MB free\n";
            IDXGIFactory1 *factory = nullptr;
            if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) && factory)
            {
                IDXGIAdapter1 *adapter = nullptr;
                for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
                {
                    DXGI_ADAPTER_DESC1 d{};
                    if (SUCCEEDED(adapter->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
                    {
                        // (Issue #20: two lines of the same name - one card listed twice, or two - told apart by the
                        // card's id (LUID, changes at each boot) and its PCI ids.)
                        char ids[96];
                        std::snprintf(ids, sizeof ids, " (id %08lX-%08lX, PCI %04X:%04X rev %02X)", static_cast<unsigned long>(d.AdapterLuid.HighPart),
                                      static_cast<unsigned long>(d.AdapterLuid.LowPart), d.VendorId, d.DeviceId, d.Revision);
                        t += "Graphics card " + std::to_string(i) + ": " + narrow(d.Description) + ", " +
                             std::to_string(static_cast<unsigned long long>(d.DedicatedVideoMemory >> 20)) + " MB" + ids + "\n";
                    }
                    adapter->Release();
                }
                factory->Release();
            }
            std::error_code ec;
            const fs::space_info space = fs::space(fs::current_path(ec), ec);
            if (!ec)
                t += "Free on the game's drive: " + std::to_string(space.available >> 30) + " GB\n";
#endif
            return t;
        }

        fs::path settingsPath()
        {
            std::error_code ec;
            const char *env = std::getenv("PS2X_SETTINGS_FILE");
            if (env && env[0] == '0' && !env[1])
                return {};
            return env && *env ? fs::path(env) : fs::current_path(ec) / "settings.ini";
        }

        // The newest sessions' logs of a folder (play_<date>.err and .out), the newest first.
        std::vector<fs::path> sessionLogs(const fs::path &dir)
        {
            std::error_code ec;
            std::vector<fs::path> errs;
            for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
            {
                const std::string name = it->path().filename().string();
                if (name.rfind("play_", 0) == 0 && it->path().extension() == ".err")
                    errs.push_back(it->path());
            }
            std::sort(errs.rbegin(), errs.rend()); // (the names sort by their date)
            if (errs.size() > kSessions)
                errs.resize(kSessions);
            std::vector<fs::path> all;
            for (const fs::path &e : errs)
            {
                all.push_back(e);
                fs::path out = e;
                out.replace_extension(".out");
                if (fs::is_regular_file(out, ec))
                    all.push_back(out);
            }
            return all;
        }
    }

    std::string make(fs::path &made)
    {
        std::error_code ec;
        std::fflush(stdout);
        std::fflush(stderr);
        const fs::path game = fs::current_path(ec);
        const fs::path logs = install_layout::logsDir();
        if (!fs::is_directory(logs, ec))
            return "The report could not be made: no logs folder (" + logs.string() + ")";

        Zip zip;
        {
            const std::time_t t = std::time(nullptr);
            std::tm tm{};
#if defined(_WIN32)
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            zip.m_time = static_cast<uint16_t>((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
            zip.m_date = static_cast<uint16_t>(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
        }
        std::string text;
        std::string report = "Transformers Armada (PC version): a report for support\n";
        report += "Made: " + stampNow("%Y-%m-%d %H:%M:%S") + "\n";
        report += "Version: " + updates::buildLine() + "\n";
#if defined(TF_OVERLAY)
        report += "Build: with mods\n";
#else
        report += "Build: public\n";
#endif
        report += "\n" + pcDetails();
        report += "\nGame folder: " + game.string() + "\nFolder layout: " + install_layout::summary() + "\nLogs: " + logs.string() + "\n";
        // Where the saves are, and how many files: their names and contents stay out.
        auto count = [](const fs::path &dir)
        {
            std::error_code e;
            size_t n = 0;
            for (auto it = fs::recursive_directory_iterator(dir, e); !e && it != fs::recursive_directory_iterator(); it.increment(e))
                n += it->is_regular_file(e) ? 1u : 0u;
            return fs::is_directory(dir, e) ? std::to_string(n) + " file(s)" : std::string("not there");
        };
        const fs::path card = PS2Runtime::getIoPaths().mcRoot;
        report += "Memory card: " + card.string() + " (" + count(card) + ")\n";
        if (const char *state = std::getenv("PS2X_STATE_FILE"); state && *state)
            report += "Save states: " + fs::path(state).parent_path().string() + "\n";
        else
            report += "Save states: " + (game / "states").string() + "\n";
        report += "Settings: " + settingsPath().string() + "\n";

        // The game's logs: the logs folder, and runs\ too when the logs folder is another one (sessions from before).
        std::vector<fs::path> files = sessionLogs(logs);
        if (const fs::path runs = game / "runs"; !fs::equivalent(runs, logs, ec))
            for (const fs::path &f : sessionLogs(runs))
                files.push_back(f);
        // The setup's and the updater's logs: beside the install's folder, in the game folder, in the logs folder.
        for (const fs::path &f : {game.parent_path() / "setup.log", game.parent_path() / "update-setup.log", game / "update.log",
                                  logs / "setup.log", logs / "update-setup.log", logs / "update.log"})
            if (fs::is_regular_file(f, ec))
                files.push_back(f);
        std::vector<std::string> used;
        for (const fs::path &f : files)
        {
            std::string name = "logs/" + f.filename().string();
            while (std::find(used.begin(), used.end(), name) != used.end())
                name = "logs/older_" + name.substr(5);
            if (readText(f, text))
            {
                zip.add(name, text);
                used.push_back(name);
            }
        }
        const fs::path settings = settingsPath();
        if (!settings.empty() && fs::is_regular_file(settings, ec) && readText(settings, text))
            zip.add("settings.ini", text);
        else
            report += "settings.ini: none (" + settings.string() + ")\n";

        // Crash and hang dumps are memory: they stay out. Their names are listed, to send one by hand when asked.
        std::string dumps;
        for (auto it = fs::directory_iterator(logs, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
            if (it->path().extension() == ".dmp")
                dumps += "  " + it->path().filename().string() + " (" + std::to_string(fs::file_size(it->path(), ec) >> 10) + " KB)\n";
        report += "\nCrash dumps in the logs folder (not in this zip; send one only when asked):\n" + (dumps.empty() ? std::string("  none\n") : dumps);
        report += "\nIn this zip:\n  report.txt\n";
        for (const std::string &n : zip.names())
            report += "  " + n + "\n";
        report += "\nNot in this zip: saves, save states, memory dumps, the disc's files, HD textures.\n";
        zip.add("report.txt", report);

        made = logs / ("report_" + stampNow("%Y%m%d_%H%M%S") + ".zip");
        if (!zip.write(made))
        {
            std::fprintf(stderr, "[report] %s could not be written\n", made.string().c_str());
            return "The report could not be written to " + made.string();
        }
        std::fprintf(stderr, "[report] made %s (%zu files)\n", made.string().c_str(), zip.names().size());
        return "Made " + made.filename().string() + " in " + logs.string();
    }

    bool openFolder(const fs::path &folder)
    {
        std::error_code ec;
        if (folder.empty() || !fs::is_directory(folder, ec))
            return false;
#if defined(_WIN32)
        const std::wstring w = fs::absolute(folder, ec).wstring();
        ShellExecuteW(nullptr, L"explore", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
        return true;
    }
}
