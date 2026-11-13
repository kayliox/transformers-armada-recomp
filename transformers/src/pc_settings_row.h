#pragma once
// The PC settings as rows and pages inside the game's own Options menus, so the common settings are reached the way the
// game's own are (docs/PC_SETTINGS.md has the pages and what each row sets):
//   - the Start menu: Options opens the hub (Display, Graphics with a preset, Audio, Controls, Gameplay, Other, More settings =
//     the F1 menu); the game's old Options rows moved into it (Widescreen is the Aspect Ratio row, Screen Position a row of
//     Other, Sound Mode an Audio row, the Controller Settings are rows of Controls); the Start menu itself also gets QUIT GAME;
//   - the HQ's Options screen lists Audio Settings, Display, Graphics, Controls, Gameplay and More settings, with the same pages;
//   - the pause menu's Options row opens an OPTIONS list with them.
//
// All of them are taScreenInGMenu lists (the same class menu_mouse.cpp already walks): the row count is at +0x7B0, the
// labels (std::strings, 12 bytes each) from +0x7C0, room for 7 rows.
//
//  1. The hosts. Each screen's input handler (slot 18) and the function that fills its rows are hooked; while a page is shown
//     the hook answers the input itself, otherwise the game does.
//  2. The pages. Opening a page saves the screen's own rows (three blocks of its memory) and fills the same list with the
//     page: rows whose choices are F1's own (the registry's, copied by pump()), stepped with left and right by the game's own
//     code (a row of more than four choices has its label in all four texts, and the move given moves the choice). Cross on a link opens its
//     page, Circle goes back (from the top page, back to the screen's own rows). No new screen is made: the game builds its
//     screens from a data file, this reuses a live one.
//  3. The settings. A value row stepped by the game is queued by key and value; pump() (the window thread, from
//     mod_menu::draw) writes it through the settings registry, as F1 does, so settings.ini follows.
//  4. The words. Every label is a short key ("PC" and the text's hash: it fits a row's slot with no allocation). The string
//     table has no such key and answers a missing key with the key itself, copied; the lookup hook (0x1A9410) hands it the
//     label itself as the key - a std::string in a frame of the guest stack, for that one call - so the answer is the label,
//     made by the game's own string code. The screen names the module changes screens by are temporaries of the same kind.
//     Nothing is added to guest memory (no guest_code::addData block): the save states' layout key is the same with the
//     pages on and off, and states made before the pages existed keep loading. A state made with a page open is loaded
//     with the page closed (the screen filled again by the game's own function: processInput).
//  5. The game's own options (Invert Y, Vibration, Look Spring, R3 Recon, Sound Mode) live in the game's profile, which the
//     game writes only from its own save screens. They are also kept in settings.ini (TF_OPT_*; "keep" = what the save
//     has) and written back after the game copies or applies a profile and after its default / reset functions
//     (installLate()).
//
// On by default; TF_PC_SETTINGS_ROW=0 leaves the game's own menus alone. TF_PC_SETTINGS_TRACE=1 logs what it does.

class PS2Runtime;

namespace pc_settings_row
{
    // Before the window loop starts (overrides.cpp, after start_screens::install).
    void install(PS2Runtime &runtime);
    // After the port's own hooks on the game's options functions (overrides.cpp): the stored options are applied last.
    void installLate(PS2Runtime &runtime);
    // The window thread, once a frame (mod_menu::draw): the settings the pages asked for, written.
    void pump();
    // A save state was loaded (overrides.cpp's state-loaded hook): what this side remembers of open pages is dropped.
    void stateLoaded();
    // One of the pages is up on the pause menu's screen (the game thread). The pause screen's own input handler runs for them too,
    // so mod_menu's "the pause screen's top is showing" (Escape resumes there) has to leave them out: in a page Escape is Back.
    bool pausePagesUp();
}
