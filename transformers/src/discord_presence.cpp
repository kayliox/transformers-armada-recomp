// Discord status (discord_presence.h): Rich Presence over the local Discord client's IPC pipe, no SDK.
//
// The protocol (Discord's documented RPC over IPC): frames of a little-endian u32 opcode, a little-endian u32 length
// and that many bytes of JSON. Opcode 0 the handshake ({"v":1,"client_id":"<application id>"}; Discord answers with a
// READY frame, or closes with an error for an unknown id), 1 a frame (our SET_ACTIVITY commands), 2 close, 3 ping
// (answered with 4 pong, the same payload). Discord shows a status while the pipe is open and clears it when it closes.
//
// The game thread only decides what to show (frame(), a few times a second) and hands it over; the worker thread
// owns the pipe: it connects (every 30 s while Discord is not there), sends a status only when it changed and at most
// every 15 s (Discord's limit), answers pings, and clears the status at the game's end. Nothing here waits on Discord
// from the game thread.
//
// Where the player is: the taGame (0x6CF2B8) while a level runs, the HQ's executor (overrides.cpp's inHq), a movie
// stream that is a film (not the HQ's level previews, the boot logos or the title's attract loop); the episode (0x716EF0, word 0: the current episode, 0..7 in the HQ's
// order - dualsense.cpp reads the same) and the Autobot (0x716FB0, its first halfword: 0 Optimus Prime, 1 Hot Shot,
// 2 Red Alert). An extension may name another character (port_ext.h presenceCharacter: Playable Decepticons).

#include "discord_presence.h"

#include "port_ext.h"
#include "runtime/ps2_settings.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__)
#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace discord_presence
{
    namespace
    {
        // The Discord application (discord.com/developers/applications, "Transformers Armada"): its Application ID.
        // (the game's own, made 2026-11-09). TF_DISCORD_APP_ID (environment or settings.ini) overrides it.
        constexpr const char *kApplicationId = "1569372292717486110";

        constexpr uint32_t kEpisodeNow = 0x00716EF0u;
        constexpr uint32_t kAutobotData = 0x00716FB0u;
        constexpr const char *kEpisodes[8] = {"Amazon Basin", "Antarctica", "Deep Amazon", "Mid-Atlantic",
                                              "Alaska",       "Starship",   "Pacific Island", "Cybertron"};
        struct Autobot
        {
            const char *name, *image;
        };
        constexpr Autobot kAutobots[3] = {{"Optimus Prime", "optimus"}, {"Hot Shot", "hotshot"}, {"Red Alert", "redalert"}};

        constexpr int kRetrySeconds = 30;
        constexpr int kMinSecondsBetweenUpdates = 15;

        struct Status
        {
            std::string details, state, smallImage, smallText;
            int64_t start = 0;
            bool operator==(const Status &o) const
            {
                return details == o.details && state == o.state && smallImage == o.smallImage && smallText == o.smallText &&
                       start == o.start;
            }
            bool operator!=(const Status &o) const { return !(*this == o); }
        };

        std::atomic<bool> g_enabled{true};
        std::atomic<bool> g_stop{false};
        std::atomic<bool> g_done{false}; // (the worker has ended)
        std::mutex g_mutex;
        std::condition_variable g_wake;
        Status g_wanted;           // (g_mutex)
        bool g_haveWanted = false; // (g_mutex)
        std::thread g_thread;

        // The game thread's own (frame()).
        int64_t g_sessionStart = 0;
        std::string g_placeKey;
        int64_t g_placeStart = 0;
        std::string g_lastLevelKey; // (the level left last, when, and its start)
        int64_t g_lastLevelStart = 0, g_leftLevelAt = 0;
        Status g_last;
        unsigned g_frames = 0u;

        int64_t unixNow() { return static_cast<int64_t>(std::time(nullptr)); }

        bool readWord(const uint8_t *rdram, uint32_t at, uint32_t &out)
        {
            if (!rdram)
                return false;
            std::memcpy(&out, rdram + at, sizeof(out));
            return true;
        }

        std::string applicationId()
        {
            if (const char *id = std::getenv("TF_DISCORD_APP_ID"); id && *id)
            {
                bool digits = std::strlen(id) <= 32;
                for (const char *p = id; *p && digits; ++p)
                    digits = *p >= '0' && *p <= '9';
                if (digits)
                    return id;
                std::fprintf(stderr, "[discord] TF_DISCORD_APP_ID is not an application id (digits only): the built-in one is used\n");
            }
            return kApplicationId;
        }

        std::string jsonText(const std::string &s)
        {
            std::string out = "\"";
            for (const unsigned char c : s)
            {
                if (c == '"' || c == '\\')
                {
                    out += '\\';
                    out += static_cast<char>(c);
                }
                else if (c < 0x20)
                {
                    char hex[8];
                    std::snprintf(hex, sizeof(hex), "\\u%04x", c);
                    out += hex;
                }
                else
                    out += static_cast<char>(c);
            }
            return out + "\"";
        }

#if defined(_WIN32)
        // ---- The worker's pipe ------------------------------------------------------------------------------------
        HANDLE g_pipe = INVALID_HANDLE_VALUE;
        std::string g_pipeName;
        unsigned g_nonce = 0u;

        void closePipe()
        {
            if (g_pipe != INVALID_HANDLE_VALUE)
                CloseHandle(g_pipe);
            g_pipe = INVALID_HANDLE_VALUE;
        }

        bool writeFrame(uint32_t op, const std::string &json)
        {
            std::vector<uint8_t> frame(8 + json.size());
            const uint32_t length = static_cast<uint32_t>(json.size());
            std::memcpy(frame.data(), &op, 4);
            std::memcpy(frame.data() + 4, &length, 4);
            std::memcpy(frame.data() + 8, json.data(), json.size());
            DWORD written = 0;
            return WriteFile(g_pipe, frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr) && written == frame.size();
        }

        // A whole frame when one is there within timeoutMs (false: none, or the pipe broke - broken set).
        bool readFrame(uint32_t &op, std::string &json, int timeoutMs, bool &broken)
        {
            broken = false;
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
            for (;;)
            {
                DWORD available = 0;
                if (!PeekNamedPipe(g_pipe, nullptr, 0, nullptr, &available, nullptr))
                {
                    broken = true;
                    return false;
                }
                if (available >= 8)
                    break;
                if (std::chrono::steady_clock::now() >= until)
                    return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            uint32_t header[2] = {0u, 0u};
            DWORD got = 0;
            if (!ReadFile(g_pipe, header, 8, &got, nullptr) || got != 8 || header[1] > (1u << 20))
            {
                broken = true;
                return false;
            }
            op = header[0];
            json.assign(header[1], '\0');
            DWORD done = 0;
            while (done < header[1])
            {
                DWORD part = 0;
                if (!ReadFile(g_pipe, json.data() + done, header[1] - done, &part, nullptr) || part == 0)
                {
                    broken = true;
                    return false;
                }
                done += part;
            }
            return true;
        }

        bool connectPipe()
        {
            std::vector<std::string> names;
            if (const char *test = std::getenv("TF_DISCORD_PIPE"); test && *test)
                names.push_back(std::string("\\\\.\\pipe\\") + test);
            else
                for (int i = 0; i < 10; ++i)
                    names.push_back("\\\\.\\pipe\\discord-ipc-" + std::to_string(i));
            for (const std::string &name : names)
            {
                g_pipe = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
                if (g_pipe == INVALID_HANDLE_VALUE)
                    continue;
                const std::string id = applicationId();
                uint32_t op = 0u;
                std::string reply;
                bool broken = false;
                if (writeFrame(0u, "{\"v\":1,\"client_id\":" + jsonText(id) + "}") && readFrame(op, reply, 3000, broken) && op == 1u &&
                    reply.find("\"READY\"") != std::string::npos)
                {
                    g_pipeName = name;
                    std::fprintf(stderr, "[discord] connected (%s)\n", name.c_str());
                    return true;
                }
                static bool told = false;
                if (!told)
                {
                    told = true;
                    std::fprintf(stderr, "[discord] the Discord app did not accept application id %s (%s)%s\n", id.c_str(),
                                 reply.empty() ? "no answer" : reply.substr(0, 160).c_str(),
                                 "");
                }
                closePipe();
            }
            return false;
        }
#elif defined(__linux__)
        // ---- The worker's socket (Linux): Discord's Unix socket, under the names the shared worker below uses ----------
        constexpr int INVALID_HANDLE_VALUE = -1;
        int g_pipe = INVALID_HANDLE_VALUE;
        std::string g_pipeName;
        unsigned g_nonce = 0u;
        unsigned GetCurrentProcessId() { return static_cast<unsigned>(getpid()); }

        void closePipe()
        {
            if (g_pipe != INVALID_HANDLE_VALUE)
                close(g_pipe);
            g_pipe = INVALID_HANDLE_VALUE;
        }

        bool writeFrame(uint32_t op, const std::string &json)
        {
            std::vector<uint8_t> frame(8 + json.size());
            const uint32_t length = static_cast<uint32_t>(json.size());
            std::memcpy(frame.data(), &op, 4);
            std::memcpy(frame.data() + 4, &length, 4);
            std::memcpy(frame.data() + 8, json.data(), json.size());
            return send(g_pipe, frame.data(), frame.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(frame.size());
        }

        // A whole frame when one is there within timeoutMs (false: none, or the socket broke - broken set).
        bool readFrame(uint32_t &op, std::string &json, int timeoutMs, bool &broken)
        {
            broken = false;
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
            uint32_t header[2] = {0u, 0u};
            for (;;)
            {
                const ssize_t n = recv(g_pipe, header, 8, MSG_PEEK | MSG_DONTWAIT);
                if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
                {
                    broken = true;
                    return false;
                }
                if (n == 8)
                    break;
                if (std::chrono::steady_clock::now() >= until)
                    return false;
                pollfd p{g_pipe, POLLIN, 0};
                poll(&p, 1, 20);
            }
            if (recv(g_pipe, header, 8, MSG_WAITALL) != 8 || header[1] > (1u << 20))
            {
                broken = true;
                return false;
            }
            op = header[0];
            json.assign(header[1], '\0');
            if (header[1] && recv(g_pipe, json.data(), header[1], MSG_WAITALL) != static_cast<ssize_t>(header[1]))
            {
                broken = true;
                return false;
            }
            return true;
        }

        // Discord's socket: discord-ipc-0..9 in XDG_RUNTIME_DIR (or TMPDIR, /tmp), directly or in the Flatpak / Snap
        // app's folder there. TF_DISCORD_PIPE: that socket's path only (tests).
        bool connectPipe()
        {
            std::vector<std::string> names;
            if (const char *test = std::getenv("TF_DISCORD_PIPE"); test && *test)
                names.push_back(test);
            else
            {
                std::string base = "/tmp";
                for (const char *v : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"})
                    if (const char *dir = std::getenv(v); dir && *dir)
                    {
                        base = dir;
                        break;
                    }
                for (const char *sub : {"/", "/app/com.discordapp.Discord/", "/snap.discord/"})
                    for (int i = 0; i < 10; ++i)
                        names.push_back(base + sub + "discord-ipc-" + std::to_string(i));
            }
            for (const std::string &name : names)
            {
                sockaddr_un address{};
                address.sun_family = AF_UNIX;
                if (name.size() >= sizeof(address.sun_path))
                    continue;
                std::memcpy(address.sun_path, name.c_str(), name.size() + 1);
                g_pipe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
                if (g_pipe < 0 || connect(g_pipe, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0)
                {
                    closePipe();
                    continue;
                }
                const std::string id = applicationId();
                uint32_t op = 0u;
                std::string reply;
                bool broken = false;
                if (writeFrame(0u, "{\"v\":1,\"client_id\":" + jsonText(id) + "}") && readFrame(op, reply, 3000, broken) && op == 1u &&
                    reply.find("\"READY\"") != std::string::npos)
                {
                    g_pipeName = name;
                    std::fprintf(stderr, "[discord] connected (%s)\n", name.c_str());
                    return true;
                }
                static bool told = false;
                if (!told)
                {
                    told = true;
                    std::fprintf(stderr, "[discord] the Discord app did not accept application id %s (%s)\n", id.c_str(),
                                 reply.empty() ? "no answer" : reply.substr(0, 160).c_str());
                }
                closePipe();
            }
            return false;
        }
#endif
#if defined(_WIN32) || defined(__linux__)

        std::string activityCommand(const Status *status)
        {
            std::string args = "{\"pid\":" + std::to_string(GetCurrentProcessId());
            if (status)
            {
                args += ",\"activity\":{\"details\":" + jsonText(status->details);
                if (!status->state.empty())
                    args += ",\"state\":" + jsonText(status->state);
                if (status->start)
                    args += ",\"timestamps\":{\"start\":" + std::to_string(status->start) + "}";
                args += ",\"assets\":{\"large_image\":\"logo\",\"large_text\":\"Transformers Armada\"";
                if (!status->smallImage.empty())
                    args += ",\"small_image\":" + jsonText(status->smallImage) + ",\"small_text\":" + jsonText(status->smallText);
                args += "}}";
            }
            args += "}";
            return "{\"cmd\":\"SET_ACTIVITY\",\"args\":" + args + ",\"nonce\":\"" + std::to_string(++g_nonce) + "\"}";
        }

        void worker()
        {
            using clock = std::chrono::steady_clock;
            clock::time_point nextTry = clock::now(), lastSend{};
            bool sentAny = false;
            Status sent;
            while (!g_stop.load())
            {
                Status wanted;
                bool haveWanted = false;
                {
                    std::unique_lock<std::mutex> lock(g_mutex);
                    g_wake.wait_for(lock, std::chrono::milliseconds(500));
                    wanted = g_wanted;
                    haveWanted = g_haveWanted;
                }
                if (g_stop.load())
                    break;
                const bool connected = g_pipe != INVALID_HANDLE_VALUE;
                if (!g_enabled.load())
                {
                    if (connected)
                    {
                        writeFrame(1u, activityCommand(nullptr)); // (cleared at once; then closed)
                        closePipe();
                        std::fprintf(stderr, "[discord] switched off: status cleared\n");
                    }
                    sentAny = false;
                    continue;
                }
                if (!connected)
                {
                    if (!haveWanted || clock::now() < nextTry)
                        continue;
                    if (!connectPipe())
                    {
                        nextTry = clock::now() + std::chrono::seconds(kRetrySeconds);
                        continue;
                    }
                    sentAny = false;
                }
                // What Discord sent: pings answered, a close ends the connection.
                for (;;)
                {
                    uint32_t op = 0u;
                    std::string payload;
                    bool broken = false;
                    if (!readFrame(op, payload, 0, broken))
                    {
                        if (broken)
                        {
                            closePipe();
                            std::fprintf(stderr, "[discord] the pipe closed (Discord quit?): trying again in %d s\n", kRetrySeconds);
                            nextTry = clock::now() + std::chrono::seconds(kRetrySeconds);
                        }
                        break;
                    }
                    if (op == 3u)
                        writeFrame(4u, payload);
                    else if (op == 2u)
                    {
                        closePipe();
                        std::fprintf(stderr, "[discord] Discord closed the connection: %s\n", payload.substr(0, 160).c_str());
                        nextTry = clock::now() + std::chrono::seconds(kRetrySeconds);
                        break;
                    }
                }
                if (g_pipe == INVALID_HANDLE_VALUE || !haveWanted)
                    continue;
                if (sentAny && wanted == sent)
                    continue;
                if (sentAny && clock::now() - lastSend < std::chrono::seconds(kMinSecondsBetweenUpdates))
                    continue; // (the newest status goes out when the time is up)
                if (!writeFrame(1u, activityCommand(&wanted)))
                {
                    closePipe();
                    nextTry = clock::now() + std::chrono::seconds(kRetrySeconds);
                    continue;
                }
                sent = wanted;
                sentAny = true;
                lastSend = clock::now();
                std::fprintf(stderr, "[discord] status: %s%s%s\n", wanted.details.c_str(), wanted.state.empty() ? "" : " / ",
                             wanted.state.c_str());
            }
            if (g_pipe != INVALID_HANDLE_VALUE)
            {
                writeFrame(1u, activityCommand(nullptr));
                closePipe();
                std::fprintf(stderr, "[discord] the game ends: status cleared\n");
            }
            g_done.store(true);
        }
#else
        void worker() { g_done.store(true); } // (other systems: no Discord connection)
#endif

        // What to show now (the game thread).
        Status decide(const uint8_t *rdram, bool inLevel, bool inHq, bool moviePlaying)
        {
            uint32_t episode = 0u, autobotWord = 0u;
            readWord(rdram, kEpisodeNow, episode);
            const bool haveAutobot = readWord(rdram, kAutobotData, autobotWord) && (autobotWord & 0xFFFFu) <= 2u;
            Status s;
            std::string key;
            if (moviePlaying && !inHq)
            {
                s.details = "Watching a film";
                key = "film";
            }
            else if (inLevel)
            {
                s.details = episode < 8u ? kEpisodes[episode] : "In a level";
                key = "level " + std::to_string(episode);
            }
            else if (inHq)
            {
                s.details = "HQ";
                key = "hq";
            }
            else
            {
                s.details = "Main menu";
                key = "menu";
            }
            if (inLevel || inHq)
            {
                std::string name, image;
                if (ext::presenceCharacter(rdram, name, image))
                {
                    s.state = "as " + name;
                    s.smallImage = image;
                    s.smallText = name;
                }
                else if (haveAutobot)
                {
                    const Autobot &a = kAutobots[autobotWord & 0xFFFFu];
                    s.state = std::string("as ") + a.name;
                    s.smallImage = a.image;
                    s.smallText = a.name;
                }
            }
            const int64_t now = unixNow();
            if (key != g_placeKey)
            {
                // (back in the same level within seconds - its intro film, a load - keeps the level's own time)
                const bool sameLevelAgain = key == g_lastLevelKey && now - g_leftLevelAt <= 30;
                if (g_placeKey.rfind("level", 0) == 0)
                {
                    g_lastLevelKey = g_placeKey;
                    g_lastLevelStart = g_placeStart;
                    g_leftLevelAt = now;
                }
                g_placeKey = key;
                g_placeStart = sameLevelAgain ? g_lastLevelStart : now;
            }
            s.start = key.rfind("level", 0) == 0 ? g_placeStart : g_sessionStart; // (a level's own time; else the session's)
            return s;
        }
    }

    void addSettings()
    {
        ps2_settings::Setting s;
        s.key = "TF_DISCORD";
        s.section = "Discord"; // (the Other page)
        s.label = "Discord status";
        s.help = "Shows on your Discord profile what you play: the level, your Autobot and for how long. Needs the Discord "
                 "app running on this PC; nothing happens without it.";
        s.choices = {{"1", "On"}, {"0", "Off"}};
        s.defaultValue = "1";
        s.apply = [](const std::string &value)
        {
            g_enabled.store(value != "0");
            g_wake.notify_all();
            return true;
        };
        ps2_settings::add(s);
        g_enabled.store(ps2_settings::value("TF_DISCORD") != "0");
        g_sessionStart = unixNow();
        if (!g_thread.joinable())
            g_thread = std::thread(worker);
    }

    void frame(const uint8_t *rdram, bool inLevel, bool inHq, bool moviePlaying)
    {
        if (!rdram || !g_enabled.load() || (g_frames++ % 15u) != 0u) // (a few times a second is plenty)
            return;
        const Status s = decide(rdram, inLevel, inHq, moviePlaying);
        if (s == g_last)
            return;
        g_last = s;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_wanted = s;
            g_haveWanted = true;
        }
        g_wake.notify_all();
    }

    void shutdown()
    {
        if (!g_thread.joinable())
            return;
        g_stop.store(true);
        g_wake.notify_all();
        // (a Discord that does not read its pipe must not hold the game's end up: then the thread is left behind, and
        // the pipe closes with the process - which clears the status too)
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        while (!g_done.load() && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (g_done.load())
            g_thread.join();
        else
            g_thread.detach();
    }
}
