#pragma once
// The HD textures while they are still being made (TODO 219).
//
// tools/make_hd_textures.ps1 (Windows) or tools/make_hd_textures.sh (Linux) makes the HD set and keeps
// hd_textures/upscale_status.json up to date (its state - working, paused, upscaled, done - and how many
// of how many are made). "done" follows colour matching and sharp fonts, not the renderer's BC7 cache.
// Linux also repairs an older "upscaled" status when its colour ledger follows the upscale and sharp fonts are made,
// and leaves the once-only image check (.images_checked_3) to make_hd_textures.sh: the game starts no maker there.
// The game can be played meanwhile:
//  - On Windows, while the game runs it holds hd_textures\.game_running (its process id inside; Windows deletes the
//    file when the game ends, a crash too). The upscale waits while the file is there - the two share the
//    graphics card - unless hd_options.json says "while_playing": true (the setup's "Keep making HD textures
//    while I play"): then it carries on at the graphics card's lowest priority.
//  - On Windows, a set left unfinished (the setup closed, the game started from Play.bat, its exe or Steam): at the start
//    the game opens a small minimized window that carries on with it (make_hd_textures.ps1 -Resume -GamePid) -
//    unless one is making them already (hd_textures\.upscaling holds its process id). (TODO 267, the user's choice)
//    It makes them only while the game is open, beside it at the graphics card's lowest priority (never waiting
//    for it, whatever hd_options.json says), and it is in a job of the game's that ends it with the game - a crash
//    too. Images are written whole and the lock goes with its process, so nothing is left half-made; the next
//    start carries on. F1 > Graphics: Stop (for this session) and Resume. The setup's own window (before the
//    game ever runs) is as before.
//  - The bottom-right corner (cornerLine): "HD textures 412 / 1,793" while they are made, then "HD textures
//    ready. Restart to use them (F1)" for a minute, and F1's footer a Restart button (readyToRestart).
//  - The renderer takes the images as they appear: a texture without its HD image yet is asked for again
//    when the set has grown (generation(); nr_capture.cpp), never decoded or looked at in the frame.
//  - The F1 menu shows "HD TEXTURES 640 OF 1900" (line()) until the set is done.
// A set without upscale_status.json (made before this, or without HD textures) is left alone.
// TF_HD_HELPER=0: no helper window started (tests).

#include <cstdint>
#include <string>

namespace hd_progress
{
    // At start, in the game's folder (after main's enterGameFolder): the lock, the helper, the watcher.
    void start();
    // Counts up each time the set has grown (the status file rewritten); 0 before.
    uint64_t generation();
    // The F1 menu's line, upper case ("" when there is nothing being made).
    std::string line();
    // (TODO 267) The bottom-right corner's line while the F1 menu is closed, for players too: "HD textures 412 / 1,793"
    // while the set is being made, "Making sharp text" while its fonts are, and "HD textures ready. Restart to use them
    // (F1)" for a minute once a set this run saw being made is done ("" otherwise: nothing shows).
    std::string cornerLine();
    // (TODO 267) A set (or its sharp text) finished while this run was going: F1's Restart button restarts the game
    // (the settings' restart: back where the player was) so that they are used.
    bool readyToRestart();
    // F1 > Graphics, "Open texture folder": the folder the game loads the HD textures from (nr::hdTextureFolder: the
    // install's hd_textures\upscaled, or TF_HIRES_DIR) opened in Explorer. Returns a line to show under the button:
    // "" when it opened; "No texture pack yet" (its parent opened when that exists) when the folder is not there.
    std::string openTextureFolder();
    // F1 > Graphics, "Make HD textures" / "Check HD textures" (makeLabel: Check once a set is done): starts the same
    // small window the game starts for an unfinished set (make_hd_textures.ps1 -Resume: the whole step - the disc's
    // archives, the conversion, the fonts, the upscale - each part skipped when done), waiting while the game runs
    // unless "Keep making HD textures while I play" is on; the F1 line follows it. Returns a line for the menu.
    std::string makeLabel();
    std::string makeHdTextures();
    // F1 > Display, "Sharp text": whether its fonts were made (hd_textures\crisp_fonts). Checked at most every 2 s.
    bool crispFontsFound();
}
