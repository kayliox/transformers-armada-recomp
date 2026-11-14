#pragma once
// (TODO 300 (6)) The Apex movement mod's keys (apex_keys.cpp). While the mod is on and the player is on foot in a level (setActive,
// from the mod's game frame) these keys belong to the mod: sprint (TF_APEX_SPRINT_KEY, Left Shift), crouch (TF_APEX_CROUCH_KEY, C)
// and slide (TF_APEX_SLIDE_KEY, Left Control). mod_menu's key overlay takes them off the game's keyboard layout meanwhile, so the
// game's actions there keep only their other keys (in the default layout Circle: Backspace, L2: Z, R2: E / Right Shift). The
// window loop polls them (windowFrame): sprintHeld() is the sprint key held - or toggled, with TF_APEX_SPRINT_TOGGLE=1.
//
// (TODO 326) Two more keys for the game's own actions, only while the mod is on: jump (TF_APEX_JUMP_EXTRA_KEY, the mouse wheel
// down) and move forward (TF_APEX_FORWARD_EXTRA_KEY, the wheel up) - Apex's tap strafing and bunny hops on the wheel. The overlay
// adds them to the jump input (L1) and to the left stick's up, beside Space and W, and takes them off every other input. A wheel
// notch is a short press (the runtime's: 60 ms held, then released, so the game sees it at 30 to 240 frames a second); with the
// wheel bound, the right-click aim's wheel zoom leaves those notches to the binding (camera_mode.cpp: a binding wins).

#include <string>

namespace apex_keys
{
    enum class Key
    {
        Sprint,
        Crouch,
        Slide,
        JumpExtra,   // (TODO 326) the jump input's extra key while the mod is on (the wheel down)
        ForwardExtra // (TODO 326) the left stick's up's extra key while the mod is on (the wheel up)
    };
    // The F1 rows of the two extra keys (Apex movement > Keys while it is on). Before the window loop starts.
    void addSettings();
    // Any thread.
    void setActive(bool on);
    bool active();
    // (TODO 344) The mod is switched on (its game frame says so each frame, in a level or not - never a setting's text,
    // which a build without the mod could carry in its settings file): what F1's pages that depend on it ask. Any thread.
    void setModOn(bool on);
    bool modOn();
    // The key's raylib code (-1: none). setKey: a rebinding (the F1 page's), any thread.
    int key(Key which);
    void setKey(Key which, int code);
    bool sprintToggle();
    void setSprintToggle(bool on);
    // Sprint now (held, or toggled on). stopSprint ends a toggled sprint (the mod: the robot stopped). Any thread.
    bool sprintHeld();
    // (TODO 324 / 329) The slide key held now (Left Control by default): the mod's crouch while held, a slide when sprinting -
    // not a toggle (the crouch key, C, stays the toggle). Any thread.
    bool slideHeld();
    void stopSprint();
    // The window thread, each picture (mod_menu's draw).
    void windowFrame();
    // The layout ("cross=X;circle=C,BACKSPACE;...") with the mod's keys taken off every input, and the two extra keys added to
    // the jump (l1) and to the left stick's up (lup).
    std::string layoutWithout(const std::string &layout);
}
