#pragma once
// Which of F1's settings the game's own Options menus show: ONE list, read by both sides.
//
// - The pages (pc_settings_row.cpp) are built from it: a page's rows are the entries of that page, in this order. An entry names
//   F1's setting (its key) or one of F1's merged rows (one choice that sets several keys: "aspect", "preset", "antialiasing",
//   "volume", "prompts"), so a row takes its choices and its values from F1, never from a copy.
// - F1 (mod_menu.cpp) checks it against its own layout: TF_MOD_MENU_LIST=1 logs the tabs and then "[mod menu] game menus: ..."
//   lines for every entry F1 does not have, and for every row F1 shows by default (not under "Show all settings") that is in
//   neither the game's menus nor the F1-only part below. A new default row in F1 is therefore decided here, not forgotten.
//
// What is not here: the game's own screens and rows that are not F1's settings (the hub's links, Back, QUIT GAME, Screen Position,
// Button Layout, the restart prompt); they are the pages' own structure.

#include <cstdint>

namespace game_menu_list
{
    // The page a row goes on. Controls is a hub: Gamepad, Sticks and Mouse are the pages under it.
    enum class Page : uint8_t
    {
        Display,
        Graphics,
        Audio,
        Gamepad,
        Sticks,
        Mouse,
        Gameplay,
        Other,
        F1Only // (shown in F1 by default, deliberately not in the game's menus)
    };

    enum Flag : uint8_t
    {
        kNone = 0,
        kNotInPause = 1,  // left out in the pause menu (its list holds six rows)
        kNotInStart = 2,  // left out on the Start menu's screens (the restart notice takes one of the seven rows)
        kSlider = 4,      // a row of steps on the lists with bars (the HQ, the pause menu)
        kPresetMember = 8 // a setting the Graphics preset sets: changing it by hand makes the preset Custom
    };

    struct Entry
    {
        Page page;
        const char *id;     // F1's setting key, or the id of one of F1's merged rows
        const char *label;  // the name in the game's menus (the game's font has no lower case: short, as the list is narrow)
        uint8_t flags;
        const char *omit;   // a choice of F1's that the game's row leaves out (F1's label), or null
    };

    // The order is the order on the page.
    inline constexpr Entry kEntries[] = {
        // Display
        {Page::Display, "PS2X_FULLSCREEN", "Display Mode", kNone, nullptr},
        {Page::Display, "PS2X_MONITOR", "Screen", kNotInPause, nullptr},
        {Page::Display, "aspect", "Aspect Ratio", kNone, nullptr},
        {Page::Display, "TF_FOV", "Field of View", kNone, nullptr},
        {Page::Display, "TF_FPS_CAP", "Frame Limit", kNone, nullptr},
        {Page::Display, "PS2X_PRESENT_SYNC", "VSync", kNone, nullptr},
        {Page::Display, "PS2X_SHOW_FPS", "Show FPS", kNotInStart, nullptr},
        // Graphics (Ultra stays in F1: its 8x multisampling and every tree in 3D cost too much for a basic menu)
        {Page::Graphics, "preset", "Preset", kNone, "Ultra"},
        {Page::Graphics, "PS2X_GPU_SCALE", "Resolution", kNotInPause, nullptr},
        {Page::Graphics, "TF_DRAW_DISTANCE", "Draw Distance", kPresetMember, nullptr},
        {Page::Graphics, "antialiasing", "Anti Aliasing", kPresetMember, "Ultra"},
        {Page::Graphics, "TF_HIRES", "HD Textures", kNone, nullptr},
        {Page::Graphics, "TF_SMOOTH_TEXTURES", "Smooth Textures", kNone, nullptr},
        {Page::Graphics, "TF_NATIVE_BLOOM", "Glow", kNotInStart, nullptr},
        // Audio (the Start menu's Options only: the HQ's and the pause menu's Audio Settings are the game's own screens)
        {Page::Audio, "volume", "Volume", kNone, nullptr},
        {Page::Audio, "TF_OPT_SOUND_MODE", "Sound Mode", kNone, nullptr}, // (the game's own option, kept in settings.ini)
        // Controls > Gamepad (the first three are the game's own options, kept in settings.ini)
        {Page::Gamepad, "TF_OPT_INVERT_Y", "Invert Y Axis", kNone, nullptr},
        {Page::Gamepad, "TF_OPT_LOOK_SPRING", "Look Spring", kNone, nullptr},
        {Page::Gamepad, "TF_OPT_R3_RECON", "R3 Recon", kNone, nullptr},
        {Page::Gamepad, "PS2X_SONYPAD_RUMBLE", "Vibration", kSlider, nullptr},
        // Controls > Sticks
        {Page::Sticks, "PS2X_STICK_LEFT_DEADZONE", "Left Dead Zone", kSlider, nullptr},
        {Page::Sticks, "PS2X_STICK_RIGHT_DEADZONE", "Right Dead Zone", kSlider, nullptr},
        {Page::Sticks, "PS2X_STICK_RESPONSE", "Stick Response", kNone, nullptr},
        {Page::Sticks, "PS2X_STICK_VEHICLE_MODE", "Car Steering", kNone, nullptr},
        // Controls > Mouse
        {Page::Mouse, "TF_MOUSE", "Mouse in Game", kNone, nullptr},
        {Page::Mouse, "PS2X_MOUSE_SENSITIVITY", "Mouse Speed", kNone, nullptr},
        {Page::Mouse, "PS2X_MOUSE_INVERT_Y", "Invert Mouse", kNone, nullptr},
        {Page::Mouse, "TF_MENU_MOUSE", "Mouse in Menus", kNone, nullptr},
        {Page::Mouse, "TF_RIGHT_AIM", "Right-Click Aim", kNone, nullptr},
        {Page::Mouse, "TF_CAR_KEYS", "Drive with Keys", kNone, nullptr},
        // Gameplay
        {Page::Gameplay, "TF_CFG_showHUD", "HUD", kNone, nullptr},
        {Page::Gameplay, "TF_CRISP_TEXT", "Sharp Text", kNone, nullptr},
        {Page::Gameplay, "prompts", "Button Prompts", kNone, nullptr},
        {Page::Gameplay, "TF_SMALL_CROSSHAIR", "Crosshair", kNone, nullptr},
        {Page::Gameplay, "TF_START_SCREENS", "Start-up", kNone, nullptr},
        {Page::Gameplay, "TF_OBJECTIVE_GUIDE", "Objective Guide", kNone, nullptr},
        // Other (the Start menu's Options only; the game's own Screen Position row follows these)
        {Page::Other, "TF_UPDATE_CHECK", "Check for Updates", kNone, nullptr},
        // F1 only: shown by F1 by default, deliberately not in the game's menus. (F1's Camera row has no key: it is not checked.)
        {Page::F1Only, "PS2X_PADMAP", "Controller buttons", kNone, nullptr}, // (the HQ's Controls has the game's own Button Layout)
        {Page::F1Only, "trees", "Trees", kNone, nullptr}, // (TODO 350 b: the game's Graphics pages are full - six and seven rows)
    };

    inline constexpr unsigned kEntryCount = sizeof(kEntries) / sizeof(kEntries[0]);
}
