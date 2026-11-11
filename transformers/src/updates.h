#pragma once
// Updates (TODO 104): the version this build was made from, a check for a newer one on the home
// repository's branch, and updating from the F1 menu's About page with the game resumed where it was.
//
// The version: tf_version.h (transformers/version.cmake, written at every build): the commit, and
// whether this is an installed copy (a clone set up with setup.ps1 or tfa-setup.exe, or the port\ of
// a repository that holds the game's folder: kind "install") or a development checkout ("dev": no
// update check there).
//
// The check runs git in that copy, hidden, never asking for anything (GIT_TERMINAL_PROMPT=0,
// GCM_INTERACTIVE=never: the game's repository is public and needs no sign-in; a copy of another
// one uses the sign-in it was cloned with): `git fetch origin <branch>`, then the commits on
// origin/<branch> the build is not made from. It follows the copy's own origin and branch.
// On a thread of its own, never in the frame: 20 s after start, then every 4 hours while "Check for
// updates automatically" (TF_UPDATE_CHECK, on by default) is on, and when "Check now" is pressed.
//
// Updating from the menu: the game saves the moment it is at (a save state,
// port\states\update_resume.ps2state, and a small marker beside it, update_resume.json), starts the
// updater - tfa-setup.exe when it is in the install folder (a window with the steps), else update.ps1
// in a console window - and closes. The updater waits for the game to close, pulls, rebuilds what the
// change needs, keeps Mods\ (and the choices made in it) and settings.ini, and starts the game again.
// At that start the marker is found: the state is loaded once (before the game's first instruction),
// then deleted. A state the new build cannot take (another save-state format, another layout of the
// port's added code: guest_code::layoutKey) is not loaded: the game starts normally - the memory card
// saves are untouched - and says so. A state the runtime refuses at loading stops it; the game is
// then started again normally, once (afterRun).
//
// Testing aids: TF_UPDATE_REPO=<an installed copy> (the copy to check and update, instead
// of the one the game's folder is in), TF_UPDATE_HIDDEN=1 (the updater without a window: worker
// tests), TF_UPDATE_TEST=<vblank> (presses "Update now" at that tick), TF_UPDATE_CHECK_AT=<seconds>
// (the first automatic check that many seconds after start, instead of 20), TF_RESUME=0 (a resume
// left by an update is not loaded).

#include <functional>
#include <string>

class PS2Runtime;

namespace updates
{
    // First thing in main, before settings.ini fills the environment: the environment the game was
    // started with, which the updater gets (and the game it starts again).
    void noteLaunchEnvironment();
    // Before the window loop: TF_UPDATE_CHECK (the About page).
    void addSettings();
    // In main after loadELF (the port's added code is in place then), before run(): a resume left by
    // an update is loaded, when this build can take it.
    void beginResume(PS2Runtime &runtime);
    // In main after run() returned: a resume the runtime could not load ended the run - the game is
    // started again normally (true: this process should just exit).
    bool afterRun(PS2Runtime &runtime);
    // Once a frame on the window thread (mod_menu::draw): the update's steps, the resume's outcome.
    void frame(PS2Runtime &runtime);
    // The About page of the F1 menu (mod_menu.cpp; ImGui). `settings`: the page's settings rows (the
    // automatic check), drawn by the menu under the Updates heading.
    void drawAbout(PS2Runtime &runtime, const std::function<void()> &settings);
    // One line for a report: the version's number, commit, branch, date and kind (support_report.h).
    std::string buildLine();
    // A newer version is there (the menu's notice); and a line for it ("" when there is none).
    bool updateAvailable();
    std::string noticeText();
    // A message for the player after a start (the resume's outcome, an update that failed); "" none.
    std::string startMessage();
    // "v0.1.3 (2026-11-01)" (the VERSION file's number, TODO 111), "v0.1.3 development build (...)" in a
    // checkout, "1a2b3c4 (2026-10-31)" for a build from before the file.
    std::string versionText();
}
