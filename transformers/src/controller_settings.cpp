// The Controller tab of the settings panel (controller_settings.h).

#include "controller_settings.h"

#include "runtime/ps2_settings.h"

#include <cstdlib>
#include <string>

namespace controller_settings
{
    void add()
    {
        using ps2_settings::Setting;
        {
            Setting s;
            s.key = "PS2X_GAMEPAD_BACKEND";
            s.section = "Controller";
            s.label = "Gamepad";
            s.order = 10;
            s.help = "DualSense / DualShock 4: read natively (vibration and light bar, no Steam Input or DS4Windows needed) "
                     "when one is connected, else any gamepad. 'Other gamepads only' leaves a Sony pad to Steam Input.";
            s.choices = {{"auto", "Sony pad natively, else others"}, {"sony", "Sony pad only"}, {"glfw", "Other gamepads only"}};
            s.defaultValue = "auto";
            ps2_settings::add(s);
        }
        {
            Setting s;
            s.key = "PS2X_SONYPAD_RUMBLE";
            s.section = "Controller";
            s.label = "Vibration";
            s.order = 20;
            s.help = "The game's own vibration on a DualSense / DualShock 4 or an Xbox (XInput) controller, this strong.";
            s.choices = {{"100", "Full"}, {"75", "75%"}, {"50", "50%"}, {"25", "25%"}, {"0", "Off"}};
            s.defaultValue = "100";
            ps2_settings::add(s);
        }
        {
            Setting s;
            s.key = "PS2X_SONYPAD_LIGHTBAR";
            s.section = "Controller";
            s.label = "Light bar";
            s.order = 30;
            s.help = "The light bar's colour on a DualSense / DualShock 4. Your Autobot's: Optimus red, Hot Shot yellow, "
                     "Red Alert blue.";
            s.choices = {{"autobot", "Your Autobot's"}, {"DC1616", "Autobot red"},     {"1E50DC", "Optimus blue"},
                         {"00C8DC", "Energon cyan"},    {"F0B400", "Hot Shot yellow"}, {"8C28C8", "Decepticon purple"},
                         {"FFFFFF", "White"},           {"off", "The pad's own"}};
            s.defaultValue = "autobot";
            // (the game sends the colour as it plays, dualsense.cpp: a change shows at once; only to or from the pad's
            // own light it takes a restart - the device thread reads that when it starts)
            s.apply = [](const std::string &value)
            {
                static const bool startedOff = [] {
                    const char *env = std::getenv("PS2X_SONYPAD_LIGHTBAR");
                    return env && std::string(env) == "off";
                }();
                return (value == "off") == startedOff;
            };
            ps2_settings::add(s);
        }
        const auto responseSetting = [](const char *key, const char *label, const char *help,
                                        const char *fallback, std::initializer_list<ps2_settings::Choice> choices, int order)
        {
            Setting s;
            s.key = key;
            s.section = "Controller";
            s.label = label;
            s.help = help;
            s.defaultValue = fallback;
            s.choices = choices;
            s.order = order;
            s.apply = [](const std::string &) { return true; }; // read on the next host pad poll
            ps2_settings::add(s);
        };
        responseSetting("PS2X_STICK_RESPONSE", "Stick response", "Modern: early robot walk/run and selectable car steering. Off restores the old host pad bytes.",
                        "on", {{"on", "Modern"}, {"off", "Off (original port)"}}, 40);
#if defined(__linux__)
        responseSetting("PS2X_STICK_DEADZONE", "Stick dead zone", "How far a stick must move before it counts. Larger stops a drifting stick "
                        "(a Steam Deck's can rest a little off centre) moving you or the camera on its own.",
                        "0.12", {{"0.05", "5% (Windows)"}, {"0.08", "8%"}, {"0.12", "12% (default)"}, {"0.16", "16%"}, {"0.20", "20%"}}, 40);
#endif
        responseSetting("PS2X_STICK_LEFT_LIFT", "Left anti-deadzone", "Robot lift before sensitivity; 0.625 starts walking just outside 5% tilt.",
                        "0.625", {{"0", "Off"}, {"0.20", "0.20"}, {"0.625", "0.625 (default)"}, {"0.70", "0.70"}}, 41);
        responseSetting("PS2X_STICK_RIGHT_LIFT", "Right anti-deadzone", "Camera stick lift; the mouse and keyboard bypass it.",
                        "0.20", {{"0", "Off"}, {"0.10", "0.10"}, {"0.20", "0.20 (default)"}, {"0.30", "0.30"}}, 42);
        responseSetting("PS2X_STICK_LEFT_SENSITIVITY", "Left sensitivity", "Robot gain after the lift; changing it changes walk/run onset. Cars keep gain 1.",
                        "1.33", {{"1", "1"}, {"1.25", "1.25"}, {"1.33", "1.33 (default)"}, {"1.5", "1.5"}}, 43);
        responseSetting("PS2X_STICK_VEHICLE_MODE", "Car steering", "Original restores the port's axes on both sticks, including Sony's drift filter. Tuned selects the separate car curve. Both blend during transforms.",
                        "original", {{"original", "Port's original (default)"}, {"tuned", "Tuned"}}, 44);
        responseSetting("PS2X_STICK_VEHICLE_LIFT", "Car anti-deadzone", "Tuned mode only: Auto cancels the live guest axis deadzone, rounding outward only to the first accepted byte. Off removes the car lift.",
                        "auto", {{"auto", "Auto (default)"}, {"off", "Off"}, {"0.10", "0.10"}, {"0.20", "0.20"}, {"0.30", "0.30"}}, 45);
        responseSetting("PS2X_STICK_VEHICLE_RESPONSE", "Car response", "Tuned mode only: car travel exponent; larger values make centre steering gentler. Robot movement is unchanged.",
                        "1.15", {{"1", "Linear"}, {"1.15", "1.15 (default)"}, {"1.5", "1.5"}, {"2", "2"}}, 46);
    }
}
