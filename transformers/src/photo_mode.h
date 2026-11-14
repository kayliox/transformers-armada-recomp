#pragma once
// Photo mode (photo_mode.cpp): F8 in a level freezes the game, hides the HUD and frees the camera.
//
//   mouse / arrow keys  look          W A S D  move (W/S along the view)
//   Space / E  up       Q / Ctrl  down        Shift  faster, Alt  slower
//   mouse wheel or + / -  field of view      H  the HUD back on / off
//   R  back to the game's own view            F8 or Esc  leave
//
// Also the mod menu's Display tab ("Photo mode") and TF_PHOTO_MODE=1 (on as soon as a level runs,
// for tests).

#include <cstdint>

class PS2Runtime;

namespace photo_mode
{
    // From applyTransformers.
    void install(PS2Runtime &runtime);
    // Once a rendered frame on the window thread (mod_menu::draw): the keys and the mouse, and the
    // hint drawn over the picture.
    void windowFrame();
    // Once a game frame (overrides.cpp, after game_config::frame): the game's speed, the HUD and the
    // pad, while photo mode is on. scripted (TODO 369): a cutscene or a movie owns the view - F8 is not taken then.
    void frame(uint8_t *rdram, bool inLevel, bool scripted = false);
    bool active();
    // Photo mode on with its HUD hidden (game_config's HUD switch hides the game's HUD while this is true).
    bool hudHidden();
    bool takeHudRestore(); // once, after a state saved in photo mode: taHUD's flag on again (TODO 269)
    // For the overlays (distance_meter's text): where the crosshair model was put this moment (world),
    // and a world point in the window's pixels as the game's camera shows it (pictureAspect: the
    // picture's shape in the window, ps2_host_display::displayAspect). False when there is none.
    // The game's crosshair model kept out of the picture (small_crosshair draws its own).
    void setHideCrosshair(bool hide);
    bool crosshair(float point[3]);
    // (TODO 355) The view's turn as the picture will have it, where something turns the camera after the game's own camera
    // update (first person's free look, camera_mode: the head's view replaced by the camera control's angles) - w x y z.
    // The Fixed crosshair's pin goes by it for the next 150 ms: by the head's turn the pinned crosshair left the centre
    // whenever the head and the view differed (walking while the view turns). Nothing else reads it. The game thread.
    void noteViewTurn(const float turn[4]);
    // (TODO 307 h) How far above the crosshair's centre its ring's top bracket reaches on a screen `screenHeight` pixels
    // tall (from the scale and distance it was drawn with this moment, and the field of view); false: not known.
    bool crosshairTop(float screenHeight, float &pixels);
    // (TODO 292 B) Where the game's camera stood when it last placed the view (world): false before it has.
    bool viewPosition(float point[3]);
    // (TODO 292 B) The view's axes as the camera last placed it (world: ahead, left, up) and the tangent of half its
    // vertical angle (the TV picture's; wider windows add width): false before it has.
    bool viewFrame(float ahead[3], float left[3], float up[3], float &tanUp);
    bool project(const float point[3], float windowW, float windowH, float pictureAspect, float &x, float &y);
    void toggle();
}
