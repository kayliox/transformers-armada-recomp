// The DualSense's own speaker (pad_speaker.h; TODO 213).

#include "pad_speaker.h"

#include "runtime/ps2_settings.h"
#include "runtime/ps2_sony_pad.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>

namespace pad_speaker
{
    namespace
    {
        std::atomic<Mode> g_mode{Mode::Alerts};
        std::atomic<int> g_volume{80};
        std::atomic<int> g_delay{0}; // (your playtest over Bluetooth: 0 ms in sync)
        std::atomic<bool> g_testLive{false};


        // The HQ's call: its beep, then the answer (the HUD's confirm beep) and the voice messages after it.
        std::mutex g_callMutex;
        double g_ringAt = -1e9;
        double g_callUntil = -1e9;  // dialogue streams opened before this are the call's
        int g_callStreams = 0;      // the call's streams playing now
        double g_callSeen = -1e9;   // (a count left over - a stream never ended - is dropped after 90 s)
        // (TODO 257) The call's messages heard so far: the pause menu's Show Objective plays the last one again, with no
        // beep and no answer before it - the same file, so it goes to the controller too.
        std::deque<std::string> g_callFiles;
        std::atomic<bool> g_levelPaused{false};
        std::atomic<double> g_unpausedAt{-1e9}; // (Show Objective closes the pause menu, then opens the message: just after)
        constexpr double kUnpauseWindow = 3.0;
        constexpr double kRingWindow = 30.0;   // s from a beep to the answer
        constexpr double kVoiceWindow = 4.0;   // s from the answer, or a message's end, to the next message

        double seconds()
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        std::string lower(std::string s)
        {
            for (char &c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        // (TODO 268) The HQ's radio lines, wherever they play: the scripted first-time calls (a warp gate's intro, the
        // first Data-Con, Mini-Con, Golden Disk, sidekick, energon, gliding, sentry drone) and the level's welcome play
        // them with no HQ beep before, so the call window never opens for them. By the game's sound list (SOUNDLST.TXT):
        // every line in a level's folder (audiocue/amazon/, antarc/ ...) is an _HQ_ event; audiocue/misc/<autobot>/
        // (optimus, redalert, hotshot) holds _HQ_ events (the intros, the Mini-Con quest and area messages, "to
        // Cybertron", the asked / answered lines more/<x>_ask, more/<x>_yes) and the Autobot's own quips (selected, death,
        // transform, power link, Mini-Con safe, warp from HQ, the cheat accepted), which stay on the PC.
        bool hqRadioLine(const std::string &lowerFile)
        {
            const std::string root = "audiocue/";
            size_t at = lowerFile.find(root);
            if (at == std::string::npos)
                return false;
            at += root.size();
            if (lowerFile.compare(at, 5, "misc/") != 0)
                return true; // (a level's folder)
            at += 5;
            const size_t slash = lowerFile.find('/', at);
            if (slash == std::string::npos)
                return false; // (misc/mg_mock, misc/ss_mock: the bosses' taunts, not the HQ)
            const std::string who = lowerFile.substr(at, slash - at);
            if (who != "optimus" && who != "redalert" && who != "hotshot")
                return false;
            std::string stem = lowerFile.substr(lowerFile.find_last_of('/') + 1);
            if (const size_t dot = stem.find('.'); dot != std::string::npos)
                stem.resize(dot);
            if (stem.size() > 3 && stem[2] == '_')
                stem.erase(0, 3); // (op_ / ra_ / hs_)
            while (!stem.empty() && std::isdigit(static_cast<unsigned char>(stem.back())))
                stem.pop_back();
            static const char *const kQuips[] = {"sel", "die", "xform", "link", "safe", "warp", "cht"};
            return std::none_of(std::begin(kQuips), std::end(kQuips), [&](const char *q) { return stem == q; });
        }

        Mode modeOf(const std::string &v)
        {
            const std::string s = lower(v);
            return s == "off" ? Mode::Off : s == "surround" ? Mode::Surround : Mode::Alerts;
        }
    }

    void addSettings()
    {
        if (const char *env = std::getenv("TF_PAD_SPEAKER"); env && *env)
            g_mode.store(modeOf(env));
        if (const char *env = std::getenv("TF_PAD_SPEAKER_VOLUME"); env && *env)
            g_volume.store(std::clamp(std::atoi(env), 0, 100));

        ps2_settings::Setting s;
        s.key = "TF_PAD_SPEAKER";
        s.section = "Controller";
        s.label = "Controller speaker";
        s.order = 34;
        s.help = "The DualSense's own speaker. Game alerts: transforming, enemies spotting you, the low-health beep and "
                 "the HQ's calls play on the controller instead of the PC. Surround: all the sound plays on both.";
        s.choices = {{"alerts", "Game alerts"}, {"surround", "Surround"}, {"off", "Off"}};
        s.defaultValue = "alerts";
        s.apply = [](const std::string &value)
        {
            g_mode.store(modeOf(value));
            return true;
        };
        ps2_settings::add(s);

        ps2_settings::Setting v;
        v.key = "TF_PAD_SPEAKER_VOLUME";
        v.section = "Controller";
        v.label = "Controller speaker volume";
        v.order = 35;
        v.help = "How loud the DualSense's speaker plays.";
        for (int i = 100; i >= 0; i -= 10)
            v.choices.push_back({std::to_string(i), std::to_string(i) + "%"});
        v.defaultValue = "80";
        v.apply = [](const std::string &value)
        {
            g_volume.store(std::clamp(std::atoi(value.c_str()), 0, 100));
            return true;
        };
        ps2_settings::add(v);

        if (const char *env = std::getenv("TF_PAD_SPEAKER_DELAY"); env && *env)
            g_delay.store(std::clamp(std::atoi(env), 0, 400));
        ps2_settings::Setting d;
        d.key = "TF_PAD_SPEAKER_DELAY";
        d.section = "Controller";
        d.label = "Speaker delay (Bluetooth)";
        d.order = 36;
        d.help = "Surround only: holds the PC's sound back by this much so it plays together with the controller's, which "
                 "comes later over Bluetooth. Raise it if the PC is heard first. Over a USB cable the game measures it by itself.";
        for (int i = 0; i <= 400; i += 20)
            d.choices.push_back({std::to_string(i), std::to_string(i) + " ms"});
        d.defaultValue = "0";
        d.apply = [](const std::string &value)
        {
            g_delay.store(std::clamp(std::atoi(value.c_str()), 0, 400));
            return true;
        };
        ps2_settings::add(d);

        // TF_PAD_SPEAKER_TEST=1 (a test aid): as if a controller's speaker were playing, so the mixer routes the game
        // alerts (their sound is dropped): with TF_PAD_SPEAKER_LOG on a machine without a DualSense.
        if (const char *env = std::getenv("TF_PAD_SPEAKER_TEST"); env && *env == '1')
        {
            g_testLive.store(true);
            std::fprintf(stderr, "[pad speaker] TF_PAD_SPEAKER_TEST: the game alerts routed as if a controller's speaker played\n");
        }
    }

    Mode mode() { return g_mode.load(std::memory_order_relaxed); }

    float volume() { return static_cast<float>(g_volume.load(std::memory_order_relaxed)) / 100.0f; }

    int surroundDelayMs(int pcBufferMs)
    {
        const int usb = ps2x::sonypad::speakerUsbLatencyMs();
        const int slider = g_delay.load(std::memory_order_relaxed);
        const int delay = usb >= 0 ? std::clamp(usb - pcBufferMs, 0, 100) : slider;
        static int logged = -1, loggedUsb = -2;
        if (delay != logged || usb != loggedUsb)
        {
            logged = delay;
            loggedUsb = usb;
            if (usb >= 0)
                std::fprintf(stderr, "[pad speaker] surround: the PC held back %d ms (over USB, measured: the controller is heard "
                                     "%d ms after the mix, the PC's buffer %d ms; the Bluetooth slider is not used)\n",
                             delay, usb, pcBufferMs);
            else
                std::fprintf(stderr, "[pad speaker] surround: the PC held back %d ms (over Bluetooth: the speaker delay setting)\n",
                             delay);
        }
        return delay;
    }

    bool live()
    {
        // (and the runtime streams only while the game wants it: the mode, kept up to date here)
        ps2x::sonypad::setSpeakerWanted(mode() != Mode::Off);
        return g_testLive.load(std::memory_order_relaxed) || ps2x::sonypad::speakerLive();
    }

    bool alertTone(const std::string &tone)
    {
        const std::string t = lower(tone);
        // 1. the Autobot's transformation; 2. an enemy noticing you; 3. the low-health beep; 4. the HQ's call
        return t == "transform_car" || t == "transform_robot" || t == "transform_fail" || t == "alert_1" || t == "alert_2" ||
               t.rfind("playerspotted", 0) == 0 || t == "hud_lowhealth" || t == "hud_hqcalling";
    }

    void noteToneKeyed(const std::string &tone)
    {
        const std::string t = lower(tone);
        const double now = seconds();
        std::lock_guard<std::mutex> lock(g_callMutex);
        if (t == "hud_hqcalling")
            g_ringAt = now;
        else if (t == "hud_x_confirm" && now - g_ringAt < kRingWindow)
        {
            g_ringAt = -1e9;
            g_callUntil = now + kVoiceWindow;
        }
    }

    bool alertStream(const std::string &file)
    {
        const std::string f = lower(file);
        if (f.find("audiocue") == std::string::npos)
            return false;
        const double now = seconds();
        std::lock_guard<std::mutex> lock(g_callMutex);
        if (g_callStreams > 0 && now - g_callSeen > 90.0)
            g_callStreams = 0;
        const bool fromPause = g_levelPaused.load(std::memory_order_relaxed) || now - g_unpausedAt.load(std::memory_order_relaxed) < kUnpauseWindow;
        const bool again = std::find(g_callFiles.begin(), g_callFiles.end(), f) != g_callFiles.end() || fromPause;
        const bool radio = hqRadioLine(f);
        if (g_callStreams <= 0 && now >= g_callUntil && !again && !radio)
            return false;
        if (radio && g_callStreams <= 0 && now >= g_callUntil)
            std::fprintf(stderr, "[pad speaker] '%s': an HQ line with no call before it (a scripted intro), for the controller's speaker\n",
                         file.c_str());
        if (!again)
        {
            g_callFiles.push_back(f);
            if (g_callFiles.size() > 32u)
                g_callFiles.pop_front();
        }
        else if (g_callStreams <= 0 && now >= g_callUntil)
            std::fprintf(stderr, "[pad speaker] '%s': a call's message played again (Show Objective%s), for the controller's speaker\n",
                         file.c_str(), fromPause ? ", from the pause menu" : "");
        ++g_callStreams;
        g_callSeen = now;
        return true;
    }

    void setLevelPaused(bool paused)
    {
        if (g_levelPaused.exchange(paused, std::memory_order_relaxed) && !paused)
            g_unpausedAt.store(seconds(), std::memory_order_relaxed);
    }

    void noteAlertStreamEnded()
    {
        std::lock_guard<std::mutex> lock(g_callMutex);
        if (g_callStreams > 0 && --g_callStreams == 0)
            g_callUntil = seconds() + kVoiceWindow;
    }

    void push(const int16_t *frames, size_t count) { ps2x::sonypad::submitSpeaker(frames, count); }
}
