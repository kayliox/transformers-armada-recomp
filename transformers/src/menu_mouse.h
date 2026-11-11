#pragma once
// The mouse in the game's own menus (menu_mouse.cpp; TODO 70, 175): while the game is paused, or
// outside a level (the front end, the HQ), or a pickup menu is up, the cursor is the player's. In
// the taScreenInGMenu lists pointing at an entry selects it, the left button confirms (cross), the
// right button goes back (triangle, the menus' Cancel), the wheel steps up and down; in the HQ's
// rings a click on the left or right side turns the ring, in the middle chooses. The mouse goes back
// to the camera on resume (a click into the picture takes it again, as everywhere).

#include <cstdint>

class PS2Runtime;

namespace menu_mouse
{
    void addSettings();
    // Wraps the HQ screens' updates (overrides.cpp, after decomp::install).
    void install(PS2Runtime &runtime);
    // The window thread, once a window frame (main.cpp): the cursor and the buttons as they are. `overlayOpen`: the F1
    // overlay or the debugger is up (TODO 286 a): the game's menus get no click, wheel or pointing then, nor for a moment
    // after it closes (the click that closed it).
    void windowFrame(int windowWidth, int windowHeight, bool overlayOpen = false);
    // While true the window keeps the cursor (main.cpp: the mouse is not captured).
    bool wantsCursor();
    // Once a game frame (mod_menu::frame): `paused` - the level is paused.
    void frame(uint8_t *rdram, bool paused);
    // The pad buttons the mouse presses this frame (mod_menu::padButtons merges them).
    uint16_t padButtons();
}
