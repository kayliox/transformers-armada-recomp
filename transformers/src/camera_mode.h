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
    // (TODO 333) The Apex mod's fixed eye: learnt over the first half second standing, then kept (no sway or bob). Any thread.
    void setEyeLock(bool on);
    // (TODO 300 (4), the Apex mod's aim down sights) While it is the owner, the right mouse button in first person is the mod's
    // ADS: adsHeld() = held now (no recon mode on the mouse; the pad keeps it). setAds: the eased amount 0..1, the zoom (the
    // field of view / zoom at 1) and the eye's move (right, down, units - as given, the mod eases it; TODO 327). adsFovFactor(): the field of view's
    // factor now (photo_mode's FOV hook). Any thread.
    void setAdsOwner(bool on);
    // (TODO 327) setAdsShoulder: in third person the mod's aim stays over the shoulder (on, the default) instead of the game's
    // aiming view; adsShoulder(): that aim is the right button's this frame (third person, the mod the owner).
    void setAdsShoulder(bool on);
    bool adsShoulder();
    bool adsHeld();
    void setAds(float amount, float zoom, float right, float down);
    float adsFovFactor();
    // (TODO 300, the Apex page) setApexFov: the mod's field of view in first person on foot, degrees horizontal at 16:9 (0: the
    // game's); fieldOfView(game): what the view's field becomes (photo_mode's FOV hook) - that, then aim down sights' zoom.
    // apexFovActive: it is in force now (ultrawide leaves its Field of view factor out then). setAdsToggle: aim down sights
    // a click in, a click out. Any thread.
    void setApexFov(float degrees);
    bool apexFovActive();
    float fieldOfView(float game);
    void setAdsToggle(bool on);
    // (TODO 300, right-click melee) setRclickMelee: the Apex mod decides each right-button press (the aim waits two frames
    // for it); takeAdsPress: a press waiting for that decision (taken once); setAdsVeto: it was a melee - no aim until the
    // button is let go. Any thread.
    void setRclickMelee(bool on);
    bool takeAdsPress();
    void setAdsVeto();
    // Once a game frame (overrides.cpp): a switch of the mode made since, carried out.
    void frame(uint8_t *rdram, bool inLevel);
}
