// (0.7.1) The one-time reset of the graphics settings to the laptop defaults: see laptop_reset.h.
//
// Which PCs: graphics_driver::lowEnd() - built-in graphics or a card with under 3.5 GB of its own memory, as Windows'
// DXGI reports it (the ones the Low preset's values are the defaults on). Elsewhere (a dedicated card; Linux and the
// Steam Deck, where the game asks no DXGI and lowEnd() is false) nothing happens.
//
// What: the rows below are taken out of settings.ini - every one of them then starts from its default, the laptop
// one where graphics_driver has one (applyLowEndDefaults: draw distance 1x, enemy detail 1, no MSAA, 67% drawn and
// enlarged by FSR 1, at least 480 lines), the frame-rate cap 60. Everything else in the file stays as it is, the
// player's comments too. The old file is kept beside it (settings.ini.bak-071, not overwritten if it is there).

#include "laptop_reset.h"

#include "graphics_driver.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace laptop_reset
{
    namespace
    {
        constexpr const char *kMarker = "TF_LAPTOP_RESET_DONE";
        constexpr const char *kVersion = "071";
        // The graphics and frame-rate rows (F1's Graphics page and the Display page's frame rate, VSync and pacing; the
        // Emulated renderer's resolution and smoothing too), the picture's shape and field of view, and a texture pack
        // (the user, 2026-11-13), and the renderer (back to the native one) - but not where graphics_driver warns of a
        // pre-2023 Intel driver, which may need the Emulated one: there TF_RENDERER stays the player's. Not the window's
        // mode or monitor.
        const char *const kKeys[] = {
            "PS2X_GPU", "PS2X_GPU_SCALE", "PS2X_GPU_AA", "PS2X_GPU_MSAA", "PS2X_GPU_ANISOTROPY", "TF_DRAW_DISTANCE",
            "TF_SCENERY_DISTANCE", "TF_FOREST_DISTANCE", "TF_FOREST_SPRITES", "TF_FOREST_FULL_DETAIL", "TF_FOREST_ADAPTIVE",
            "TF_GRASS_DISTANCE", "TF_TERRAIN_DETAIL", "TF_NEAR_TREES", "TF_ENEMY_DETAIL", "TF_HIRES", "TF_SMOOTH_TEXTURES",
            "TF_SHARP_TEXTURES", "TF_NATIVE_AA", "TF_NATIVE_SSAA", "TF_NATIVE_QUALITY", "TF_NATIVE_BLOOM",
            "TF_NATIVE_BLOOM_STRENGTH", "TF_NATIVE_TONEMAP", "TF_NATIVE_TONE_STRENGTH", "TF_NATIVE_CLARITY",
            "TF_NATIVE_CLARITY_STRENGTH", "TF_NATIVE_SHARPEN", "TF_NATIVE_RENDER_SCALE", "TF_NATIVE_UPSCALER",
            "TF_NATIVE_MIN_LINES", "TF_MOTION_TRAILS", "TF_FPS_CAP", "TF_FRAME_RATE", "TF_FPS_LOCK", "TF_FRAME_PACING",
            "TF_PACING", "PS2X_PRESENT_SYNC", "PS2X_THREAD_PLACEMENT", "TF_ASPECT", "PS2X_WIDESCREEN", "TF_FOV", "TF_HQ_FOV",
            "PS2X_TEXTURE_PACK"};

        std::string trim(const std::string &s)
        {
            const size_t a = s.find_first_not_of(" \t");
            if (a == std::string::npos)
                return "";
            const size_t b = s.find_last_not_of(" \t");
            return s.substr(a, b - a + 1);
        }

        // The line's key ("" for a comment or a line without '='), as ps2_settings reads them.
        std::string keyOf(const std::string &line)
        {
            const std::string t = trim(line);
            if (t.empty() || t[0] == '#' || t[0] == ';')
                return "";
            const size_t eq = t.find('=');
            return eq == std::string::npos ? "" : trim(t.substr(0, eq));
        }

        bool resetKey(const std::string &key)
        {
            if (key == "TF_RENDERER")
                return graphics_driver::warning().empty(); // (an old Intel driver: the player's renderer kept)
            for (const char *k : kKeys)
                if (key == k)
                    return true;
            return false;
        }

        void tell(const std::string &backup)
        {
            const std::string text = "Your graphics settings were reset to the recommended laptop settings for this update. "
                                     "Your old settings are saved in " + backup + ".";
            std::fprintf(stderr, "[laptop reset] %s\n", text.c_str());
#if defined(_WIN32)
            // (as graphics_driver::showOnce: none in a hidden test run or with TF_DRIVER_WARN_BOX=0; a throwaway window
            // takes a launcher's SW_HIDE first, so the box shows)
            const char *hidden = std::getenv("PS2X_WINDOW_HIDDEN");
            const char *box = std::getenv("TF_DRIVER_WARN_BOX");
            if ((hidden && *hidden == '1') || (box && *box == '0'))
                return;
            if (HWND dummy = CreateWindowExA(0, "STATIC", "", 0, 0, 0, 0, 0, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr))
            {
                ShowWindow(dummy, SW_HIDE);
                DestroyWindow(dummy);
            }
            MessageBoxA(nullptr, text.c_str(), "Transformers - graphics settings", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
#endif
        }
    }

    void run()
    {
        if (const char *off = std::getenv("TF_LAPTOP_RESET"); off && *off == '0')
            return;
        if (!graphics_driver::lowEnd())
            return;
        // settings.ini where ps2_settings::loadFile reads it (PS2X_SETTINGS_FILE, else the working folder's)
        std::filesystem::path file = "settings.ini";
        if (const char *env = std::getenv("PS2X_SETTINGS_FILE"))
        {
            const std::string name = trim(env);
            if (name.empty() || name == "0" || name == "none" || name == "NONE" || name == "None")
                return; // (no settings file: a test, a replay)
            file = name;
        }
        std::error_code ec;
        std::vector<std::string> lines;
        bool exists = std::filesystem::is_regular_file(file, ec);
        if (exists)
        {
            std::ifstream in(file, std::ios::binary);
            std::string line;
            while (std::getline(in, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (keyOf(line) == kMarker)
                    return; // (done before: the player's choices since then stay)
                lines.push_back(line);
            }
        }
        std::vector<std::string> kept;
        std::string removed;
        for (const std::string &line : lines)
        {
            const std::string key = keyOf(line);
            if (!key.empty() && resetKey(key))
            {
                removed += (removed.empty() ? "" : ", ") + trim(line);
                continue;
            }
            kept.push_back(line);
        }
        std::filesystem::path backup = file;
        backup += std::string(".bak-") + kVersion;
        if (exists && !removed.empty() && !std::filesystem::exists(backup, ec))
        {
            std::filesystem::copy_file(file, backup, ec);
            if (ec)
            {
                std::fprintf(stderr, "[laptop reset] could not keep a copy of %s (%s): nothing changed\n", file.string().c_str(),
                             ec.message().c_str());
                return;
            }
        }
        kept.push_back(std::string(kMarker) + "=" + kVersion);
        {
            const std::filesystem::path tmp = file.string() + ".tmp";
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                for (const std::string &line : kept)
                    out << line << "\r\n";
                if (!out)
                {
                    std::fprintf(stderr, "[laptop reset] could not write %s: nothing changed\n", tmp.string().c_str());
                    return;
                }
            }
            std::filesystem::rename(tmp, file, ec);
            if (ec)
            {
                std::fprintf(stderr, "[laptop reset] could not replace %s (%s): nothing changed\n", file.string().c_str(),
                             ec.message().c_str());
                std::filesystem::remove(tmp, ec);
                return;
            }
        }
        if (removed.empty())
        {
            std::fprintf(stderr, "[laptop reset] built-in or low-end graphics: the graphics settings were already the laptop "
                                 "defaults (%s marked)\n", file.string().c_str());
            return;
        }
        std::fprintf(stderr, "[laptop reset] built-in or low-end graphics: back to the laptop defaults in %s (was: %s; the old "
                             "file kept as %s)\n", file.string().c_str(), removed.c_str(), backup.string().c_str());
        tell(backup.filename().string());
    }
}
