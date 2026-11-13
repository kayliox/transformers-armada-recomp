// (TODO 300 (6)) The Apex movement mod's keys: see apex_keys.h.

#include "apex_keys.h"

#include "runtime/ps2_host_input.h"
#include "runtime/ps2_settings.h"

#include "raylib.h"

#include <atomic>
#include <cstdlib>
#include <string>

namespace apex_keys
{
    namespace
    {
        int fromEnv(const char *name, const char *fallback)
        {
            const char *v = std::getenv(name);
            const int code = ps2_host_input::keyCode(v && *v ? v : fallback);
            return code >= 0 ? code : ps2_host_input::keyCode(fallback);
        }

        std::atomic<bool> g_active{false};
        constexpr int kKeys = 5;
        std::atomic<int> g_keys[kKeys] = {-2, -2, -2, -2, -2}; // (-2: not read yet - from the environment on first use)
        // Each key's setting and default ("NONE": no key - the extra keys can be switched off)
        const char *const kNames[kKeys][2] = {{"TF_APEX_SPRINT_KEY", "LEFT_SHIFT"},
                                              {"TF_APEX_CROUCH_KEY", "C"},
                                              {"TF_APEX_SLIDE_KEY", "LEFT_CONTROL"},
                                              {"TF_APEX_JUMP_EXTRA_KEY", "MOUSE_WHEEL_DOWN"},
                                              {"TF_APEX_FORWARD_EXTRA_KEY", "MOUSE_WHEEL_UP"}};
        int extraCode(const char *value, const char *fallback)
        {
            const std::string v = value && *value ? value : fallback;
            if (v == "NONE")
                return -1;
            const int code = ps2_host_input::keyCode(v);
            return code >= 0 ? code : ps2_host_input::keyCode(fallback);
        }
        std::atomic<bool> g_toggle{std::getenv("TF_APEX_SPRINT_TOGGLE") && *std::getenv("TF_APEX_SPRINT_TOGGLE") == '1'};
        std::atomic<bool> g_sprint{false}, g_toggledOn{false}, g_slide{false};
        bool g_wasDown = false;
    }

    void setActive(bool on) { g_active.store(on, std::memory_order_relaxed); }
    bool active() { return g_active.load(std::memory_order_relaxed); }
    int key(Key which)
    {
        std::atomic<int> &slot = g_keys[static_cast<int>(which)];
        int code = slot.load(std::memory_order_relaxed);
        if (code == -2)
        {
            const int i = static_cast<int>(which);
            code = which == Key::JumpExtra || which == Key::ForwardExtra ? extraCode(std::getenv(kNames[i][0]), kNames[i][1])
                                                                          : fromEnv(kNames[i][0], kNames[i][1]);
            int expected = -2;
            if (!slot.compare_exchange_strong(expected, code, std::memory_order_relaxed))
                code = expected;
        }
        return code;
    }
    void setKey(Key which, int code) { g_keys[static_cast<int>(which)].store(code, std::memory_order_relaxed); }
    bool sprintToggle() { return g_toggle.load(std::memory_order_relaxed); }
    void setSprintToggle(bool on)
    {
        g_toggle.store(on, std::memory_order_relaxed);
        g_toggledOn.store(false, std::memory_order_relaxed);
    }
    bool sprintHeld() { return g_sprint.load(std::memory_order_relaxed); }
    bool slideHeld() { return g_slide.load(std::memory_order_relaxed); }
    void stopSprint() { g_toggledOn.store(false, std::memory_order_relaxed); }

    void windowFrame()
    {
        const int code = key(Key::Sprint);
        // (only keyboard keys: a mouse button's code, from the layout's names, is not a raylib key)
        const bool down = active() && code > 0 && code < 512 && IsKeyDown(code);
        if (!active())
            g_toggledOn.store(false, std::memory_order_relaxed);
        else if (down && !g_wasDown && sprintToggle())
            g_toggledOn.store(!g_toggledOn.load(std::memory_order_relaxed), std::memory_order_relaxed);
        g_wasDown = down;
        g_sprint.store(sprintToggle() ? g_toggledOn.load(std::memory_order_relaxed) : down, std::memory_order_relaxed);
        const int slide = key(Key::Slide);
        g_slide.store(active() && slide > 0 && slide < 512 && IsKeyDown(slide), std::memory_order_relaxed);
    }

    void addSettings()
    {
        // (TODO 326) Any key name of the layout: the wheel first (Apex's tap strafing), the mouse's side buttons, then keys.
        static const char *const kChoices[] = {"MOUSE_WHEEL_DOWN", "MOUSE_WHEEL_UP", "MOUSE_MIDDLE", "MOUSE_SIDE", "MOUSE_EXTRA", "LEFT_ALT",
                                               "CAPS_LOCK", "V", "B", "F", "G", "Q", "E", "Z", "X", "TAB"};
        static const char *const kWords[][2] = {{"MOUSE_WHEEL_DOWN", "Wheel down"}, {"MOUSE_WHEEL_UP", "Wheel up"},
                                                {"MOUSE_MIDDLE", "Middle mouse button"}, {"MOUSE_SIDE", "Mouse side button"},
                                                {"MOUSE_EXTRA", "Mouse extra button"}};
        const struct
        {
            Key which;
            const char *label, *help;
        } kRows[] = {
            {Key::JumpExtra, "Jump (extra)", "A second jump key while Apex movement is on (Space keeps jumping): the wheel down by default, for bunny hops."},
            {Key::ForwardExtra, "Move forward (extra)", "A second move forward key while Apex movement is on (W keeps moving): the wheel up by default, for tap strafing."},
        };
        for (const auto &row : kRows)
        {
            const int i = static_cast<int>(row.which);
            ps2_settings::Setting s;
            s.key = kNames[i][0];
            s.section = "Apex movement";
            s.label = row.label;
            s.help = std::string(row.help) + " A wheel notch is a short press. The key leaves the game's other actions while Apex movement is on.";
            s.choices.push_back({"NONE", "None"});
            for (const char *k : kChoices)
            {
                std::string label = ps2_host_input::keyName(ps2_host_input::keyCode(k));
                for (const auto &w : kWords)
                    if (std::string(w[0]) == k)
                        label = w[1];
                s.choices.push_back({k, label});
            }
            s.defaultValue = kNames[i][1];
            const Key which = row.which;
            const char *const fallback = kNames[i][1];
            s.apply = [which, fallback](const std::string &value)
            {
                if (value != "NONE" && ps2_host_input::keyCode(value) < 0)
                    return false;
                setKey(which, extraCode(value.c_str(), fallback));
                return true;
            };
            ps2_settings::add(s);
        }
    }

    std::string layoutWithout(const std::string &layout)
    {
        std::string names[kKeys];
        for (int i = 0; i < kKeys; ++i)
        {
            const int code = key(static_cast<Key>(i));
            names[i] = code >= 0 ? ps2_host_input::keyName(code) : std::string();
        }
        // (TODO 326) the extra keys' inputs: jump (L1) and the left stick's up
        const std::string &jump = names[static_cast<int>(Key::JumpExtra)], &forward = names[static_cast<int>(Key::ForwardExtra)];
        std::string out;
        for (size_t at = 0; at < layout.size();)
        {
            size_t end = layout.find(';', at);
            if (end == std::string::npos)
                end = layout.size();
            const std::string item = layout.substr(at, end - at);
            at = end + 1;
            const size_t eq = item.find('=');
            if (eq == std::string::npos)
                continue;
            std::string list;
            const std::string keys = item.substr(eq + 1);
            for (size_t k = 0; k <= keys.size();)
            {
                size_t e = keys.find(',', k);
                if (e == std::string::npos)
                    e = keys.size();
                const std::string name = keys.substr(k, e - k);
                k = e + 1;
                bool drop = name.empty();
                for (const std::string &n : names)
                    drop = drop || (!n.empty() && name == n);
                if (!drop)
                    list += (list.empty() ? "" : ",") + name;
            }
            const std::string input = item.substr(0, eq);
            for (const std::string *extra : {input == "l1" ? &jump : nullptr, input == "lup" ? &forward : nullptr})
                if (extra && !extra->empty())
                    list += (list.empty() ? "" : ",") + *extra;
            out += (out.empty() ? "" : ";") + input + "=" + list;
        }
        return out;
    }
}
