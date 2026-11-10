#pragma once
// The start-up screens (TODO 110): what the game shows from boot to its main menu, a setting of the
// F1 overlay's Gameplay page (`TF_START_SCREENS` in settings.ini):
//   menu  - straight to the main menu (New Game / Continue / Options / Extras), the default: the memory card's
//           notices ("Load Successful", "No save data present", the autosave notice - TODO 116), the
//           copyright screen, the Atari and Melbourne House logos, the attract film the first start
//           plays before the title, the title ("press Start") and the story intro after New Game are
//           all skipped. The card is still read as always, and its errors and questions (a load
//           error, a corrupt save, no card, no room, autosave disabled) still wait for the player;
//   title - the movies skipped, the title kept (what the old "Intro screens" switch Off did, TODO 80);
//   all   - everything, as on the PS2.
//
// Done through the game's own front end, never with timed key presses and never with the
// developers' runUIIntro (it skips the title's whole flow - the profile is never set up and the first
// level stops the game, TODO 80):
//  - a start movie (taItemIntroFMV::ProcessInput, 0x34D060) is handed the screen system's own skip
//    input (krInput 5) once its stream plays - the path the game takes when the player skips it, to
//    the screen the movie names as its next;
//  - the card check's notices and the copyright screen, which only wait, have their own timer run out
//    (taScreenStartInitCardCheck's +0x84C, the copyright screen's taItemTimeout): their own code goes on
//    to the next screen as when their seconds have passed;
//  - the title (taScreenPrGTitle's update, 0x33B690) is told Start was pressed (its +0xC8, the start
//    sound's handle, set): its own code then goes to "StartMenu" the first frame its screen is shown
//    and taking input - exactly as when the player presses Start, the profile and the saves set up
//    after it as always. Only the first title of a session: backing out of the main menu to the title
//    later stays there.
// If any of it does not happen (an address not found, a screen never reached) the game simply shows
// that screen as usual - the setting can never stop the game from starting. TF_START_TRACE=1 logs the
// front end's movies and screens as they come.
// Linux adds Quit Game to that menu; it takes the same shutdown path as closing the window.

#include <cstdint>

class PS2Runtime;

namespace start_screens
{
    // A scripted test run (TF_INPUT) without the setting: TF_START_SCREENS=scripted into the environment, the
    // start as before the setting (main, before a play recording takes the settings - TODO 150).
    void noteScriptedRun();
    void addSettings();
    void install(PS2Runtime &runtime);
    // A start movie the setting skips (by its key, hd_movies::movieKey: "NATARI", "NINTRO", ...) - the
    // FMV skip's fallback presses (overrides.cpp) use it too, for a movie the state skip above missed.
    bool skipsMovie(const char *movieKey);
}
