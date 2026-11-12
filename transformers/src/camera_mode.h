#pragma once
// First person / third person, in robot and car mode (camera_mode.cpp). TF_FIRST_PERSON=1 starts
// in first person; the mod menu's Display tab switches it.

#include <cstdint>

class PS2Runtime;

namespace camera_mode
{
    // From applyTransformers.
    void install(PS2Runtime &runtime);
    bool firstPerson();
    // (TODO 292 H) The F1 row "Right-click aim" (Click / Hold, TF_RIGHT_AIM).
    void addSettings();
    void setFirstPerson(bool on);
    // The game's view is the car's (the Autobot is in vehicle mode), first person or not. Any thread.
    bool inVehicle();
    // First person on foot as the game's own aiming view (R3: strafing, R3 zooms): the button it
    // presses for that, once a rendered frame (overrides.cpp, the pad override).
    uint16_t padButtons();
    // (TODO 224) The right mouse button's recon mode: the pad button a click presses (in, zoom, out), once a rendered frame.
    uint16_t reconButtons();
    void setAim(bool on);
    bool aim();
    // The current camera's transform (overrides.cpp, the listener update's 0x1AFD40 call): the free look's (TODO 190).
    void noteCameraTransform(uint32_t transform);
    // (TODO 300 a) The steady first-person eye: the camera's place as the game blended it (photo_mode's hook on the view's
    // SetPosition, before the call), held at its average offset from the robot's root so the animations no longer jolt it.
    // The game thread. TF_FP_STEADY_EYE / F1 Gameplay "Steady first-person view".
    void steadyEye(uint8_t *rdram, float position[3]);
    // (TODO 300 h) The eye lowered by that many units in first person on foot (the Apex mod's crouch; 0 = none, the default).
    // Any thread.
    void setEyeDrop(float units);
    // Once a game frame (overrides.cpp): a switch of the mode made since, carried out.
    void frame(uint8_t *rdram, bool inLevel);
}
