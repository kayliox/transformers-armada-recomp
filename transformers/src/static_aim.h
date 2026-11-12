#pragma once
// A still crosshair for the mouse (static_aim.cpp, TODO 292 B): the aim ray from the camera's eye, so the crosshair
// keeps one place on the screen whatever is under it, and shots go where it points. TF_STATIC_AIM / F1 Controls
// "Still crosshair".

class PS2Runtime;

namespace static_aim
{
    // From applyTransformers: the hooks on the game's crosshair (0x46B390) and what it calls.
    void install(PS2Runtime &runtime);
    void addSettings();
    // (TODO 292 A) The third-person camera's pivot as the game last placed it (GetThirdPersonCameraPivotPoint, from the
    // crosshair's update each frame): the centre the camera turns about. False when none came lately.
    bool cameraPivot(float point[3]);
}
