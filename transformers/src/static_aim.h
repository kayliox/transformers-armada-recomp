#pragma once
// A still crosshair for the mouse (static_aim.cpp, TODO 292 B): the aim ray from the camera's eye, so the crosshair
// keeps one place on the screen whatever is under it, and shots go where it points. (TODO 355) On only while the
// Crosshair row says Fixed (setFixed - the mods build's row; the public build has none: the original game's crosshair).

class PS2Runtime;

namespace static_aim
{
    // From applyTransformers: the hooks on the game's crosshair (0x46B390) and what it calls.
    void install(PS2Runtime &runtime);
    // (TODO 355) The Crosshair row's choice, Fixed (its owner sets it each game frame; never set in a build without the
    // row): the still crosshair, the pin, the reticle's place in the pictures (view_turn) and its one size (photo_mode) are
    // on only then - with the mouse and with a pad. fixed: it is set. Any thread.
    void setFixed(bool on);
    bool fixed();
    // (TODO 292 A) The third-person camera's pivot as the game last placed it (GetThirdPersonCameraPivotPoint, from the
    // crosshair's update each frame): the centre the camera turns about. False when none came lately.
    bool cameraPivot(float point[3]);
    // (TODO 322) The crosshair pinned to one point of the screen now (the Fixed crosshair, on foot): `share` = its height as a
    // share of half the view's height above the centre (0: the centre - first person, the aiming view). photo_mode places
    // the game's crosshair model on the camera's ray through it, small_crosshair draws there. Any thread.
    bool pinned(float &share);
}
