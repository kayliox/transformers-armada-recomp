#pragma once

// (TODO 279, release A) The installed layout, old and new, read side by side.
//
// Today an install is the checkout: settings.ini, game\mc0, states\, Saves\, runs\ all sit in it. The layout
// planned for 0.5.0 (docs/INSTALL_LAYOUT_PLAN.md) has them in the folder above it:
//
//   <root>\layout.json              {"layout": 2}: the mark that the new places are in use
//   <root>\settings.ini
//   <root>\saves\mc0, mc1           memory cards
//   <root>\saves\states\            save states
//   <root>\saves\                   backups, exports, imports (F1 > Saves)
//   <root>\logs\                    the game's log, crash and hang dumps, reports
//   <root>\data\game\               the extracted disc
//   <root>\data\movies_hd\          HD movies
//   <root>\data\shader cache\
//
// This release only READS that layout: nothing is moved and nothing new is made. A new place is used when the mark
// is there AND that place is there already; every other item stays where it is today. Without the mark (every
// install and every checkout today) nothing changes at all. The HD textures and the mods stay where they are in
// both layouts for now: the tools that make them write beside the checkout.
//
// TF_LAYOUT=0: the old places whatever is found.

#include <filesystem>
#include <string>

namespace install_layout
{
    // In main, right after the game folder became the current folder: looks for the mark.
    void find();
    // In main, after the launch environment was noted and before settings.ini is read: the switches of the places
    // found (PS2X_SETTINGS_FILE, TF_MC_ROOT, PS2X_STATE_FILE, TRANSFORMERS_GAME_DIR, TF_HD_MOVIES, TF_SHADER_CACHE),
    // each one only when nothing set it already.
    void apply();

    // True when the mark was found.
    bool isNew();
    // <root> ("" without the mark).
    std::filesystem::path root();
    // Where settings.ini is read from when the new place has one ("" otherwise).
    std::filesystem::path settingsFile();
    // The game's log and the dumps: <root>\logs when it is there, else runs\ in the game folder (made when missing).
    std::filesystem::path logsDir();
    // F1 > Saves' own folder (backups, exports, imports): <root>\saves when it is there, else `today`.
    std::filesystem::path savesDir(const std::filesystem::path &today);
    // What the log says about it at the start.
    std::string summary();
}
