// The Crosshair row (TODO 355): F1 > Gameplay > Crosshair, Original / Fixed.
//
//   Original  the game's own (the default): it moves with the aim, and with what is under it, near or far. Nothing of
//             the still crosshair acts: static_aim::fixed() is false, as in every build before this row.
//   Fixed     still, in the dead centre of the picture: static_aim's line from the camera's eye, the pin, the reticle kept
//             out of the pictures' turn and its one size (static_aim.h, view_turn, photo_mode) - with the mouse and with
//             a controller.
//
// TF_CROSSHAIR=original / fixed, live (no restart), kept in the settings file. Unset - never chosen - it is Original,
// except in a build with the Apex movement mod while that mod is on (apex_keys::modOn, which only the mod sets): Fixed,
// as TODO 343 had it; once chosen, the choice holds with the mod on or off. The row's word goes to static_aim each game
// frame (setFixed), which nothing else sets.
//
// (TODO 358) Under it, Crosshair size (TF_CROSSHAIR_SIZE, 0.25 to 1, a slider of 5% steps, live, kept): the crosshair's
// size as a share of the game's own - 1, the default, is today's size and the largest. photo_mode multiplies the game's
// ring's scale by it (about the model's centre: where it is placed, so in Fixed it stays in the dead centre), and
// small_crosshair the small cross's bars. Nothing else reads it: the aim and the other reticles are untouched.
//
// Decomp equivalent: a setting read by CalculateCrosshair (the line from the camera's eye) and by the crosshair's draw.

#include "crosshair_row.h"

#include "apex_keys.h"
#include "static_aim.h"

#include "runtime/ps2_settings.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace crosshair_row
{
    namespace
    {
        std::atomic<int> g_choice{-1}; // -1 never chosen (Original; Fixed while the Apex mod is on), 0 Original, 1 Fixed

        int parse(const std::string &value)
        {
            if (value == "fixed" || value == "1")
                return 1;
            if (value == "original" || value == "0")
                return 0;
            return -1;
        }

        bool fixedNow()
        {
            const int c = g_choice.load(std::memory_order_relaxed);
            return c < 0 ? apex_keys::modOn() : c == 1;
        }

        // (TODO 358) The crosshair's size, a share of the game's own: 1 (the default) is today's, the largest.
        constexpr float kSizeFloor = 0.25f;
        std::atomic<float> g_size{1.0f};

        float parseSize(const std::string &value)
        {
            const float v = static_cast<float>(std::atof(value.c_str()));
            return v > 0.0f ? std::clamp(v, kSizeFloor, 1.0f) : 1.0f;
        }
    }

    float size() { return g_size.load(std::memory_order_relaxed); }

    void addSettings()
    {
        using ps2_settings::Choice;
        if (const char *env = std::getenv("TF_CROSSHAIR"); env && *env)
            g_choice.store(parse(env), std::memory_order_relaxed);
        ps2_settings::Setting s;
        s.key = "TF_CROSSHAIR";
        s.section = "Gameplay";
        s.label = "Crosshair";
        s.order = 59;
        s.help = "Original: the game's own crosshair - it moves with your aim and with what is under it. "
                 "Fixed: the crosshair stays still in the exact centre of the screen, and shots go where it points. "
                 "Changes at once, no restart.";
        s.choices.push_back(Choice{"original", "Original"});
        s.choices.push_back(Choice{"fixed", "Fixed"});
        s.defaultValue = ""; // (unset: Original, or the Apex mod's Fixed - either choice made is kept in the settings file)
        s.current = [] { return std::string(fixedNow() ? "fixed" : "original"); };
        s.apply = [](const std::string &value)
        {
            g_choice.store(parse(value), std::memory_order_relaxed);
            static_aim::setFixed(fixedNow());
            std::fprintf(stderr, "[crosshair] %s (F1 > Gameplay > Crosshair)\n", fixedNow() ? "Fixed: still, in the centre" : "Original: the game's own");
            return true;
        };
        ps2_settings::add(s);

        // (TODO 358) The crosshair's size, under the row above: the game's own size (100%, the default) down to a quarter,
        // live. Both looks follow it - the game's ring (its scale, about its own centre: in Fixed it stays in the dead centre)
        // and the small cross; where it points does not move. The lock-on and sniper reticles are other models: not scaled.
        if (const char *env = std::getenv("TF_CROSSHAIR_SIZE"); env && *env)
            g_size.store(parseSize(env), std::memory_order_relaxed);
        ps2_settings::Setting z;
        z.key = "TF_CROSSHAIR_SIZE";
        z.section = "Gameplay";
        z.label = "Crosshair size";
        z.order = 59;
        z.help = "How big the crosshair is: 100% is the game's own size, the largest. Smaller sizes shrink it around its "
                 "centre; where it points does not change. Changes at once, no restart.";
        for (int percent = 25; percent <= 100; percent += 5)
        {
            char value[16], label[16];
            std::snprintf(value, sizeof(value), "%g", static_cast<double>(percent) / 100.0); // ("0.25" .. "0.95", "1")
            std::snprintf(label, sizeof(label), "%d%%", percent);
            z.choices.push_back(Choice{value, label});
        }
        z.defaultValue = "1";
        z.current = []
        {
            char value[16];
            std::snprintf(value, sizeof(value), "%g", std::round(static_cast<double>(size()) * 20.0) / 20.0); // (the nearest step)
            return std::string(value);
        };
        z.apply = [](const std::string &value)
        {
            g_size.store(parseSize(value), std::memory_order_relaxed);
            std::fprintf(stderr, "[crosshair] size %.0f%% (F1 > Gameplay > Crosshair size)\n", static_cast<double>(size()) * 100.0);
            return true;
        };
        ps2_settings::add(z);
    }

    void gameFrame()
    {
        const bool fixed = fixedNow();
        if (static int told = -1; told != (fixed ? 1 : 0))
        {
            told = fixed ? 1 : 0;
            std::fprintf(stderr, "[crosshair] %s%s\n", fixed ? "Fixed: still, in the centre" : "Original: the game's own",
                         g_choice.load(std::memory_order_relaxed) < 0 ? (fixed ? " (not chosen: the Apex mod is on)" : " (not chosen)") : "");
        }
        static_aim::setFixed(fixed);
    }
}
