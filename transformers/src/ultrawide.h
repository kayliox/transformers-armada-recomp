#pragma once
// Ultrawide: the game's 16:9 mode drawn wider still (21:9, 32:9, the screen's own shape), hor+.
//
// Ported from our earlier recomp's widescreen data for the PAL release (TFA-Modding-Kit, 2-game-data:
// widescreen.txt and the reasons in profile-widescreen.json), its addresses carried to SLUS-20668
// by the code around each one (all 36 agree word for word, shifted 0, 16 or 32 bytes). With the
// shape S wider than 16:9 and k = S / (16/9), while the game draws wide (its WIDESCREEN byte):
//   - the 4/3 the display mode multiplies the pixel aspect by (0x6CB1FC) becomes 4/3 x k;
//   - vlCamera::GetFieldOfView (0x1F2840, degrees, horizontal) returns 2 atan(k tan(fov/2)) to the
//     calls that build the picture: SetProjectionMatrixPerspective derives the vertical field from
//     the horizontal one and the aspect, so the scaled aspect alone was vert-minus (TFA-15). Not to
//     the scene traversal's frustum: the game decides from the camera's frustum which creatures
//     update and spawn (TFA-16), so gameplay keeps the view it had - only the effects' frustum
//     (taRenderingInfo, via 0x2B8A1C) and the water, sky, eco globe and rain (via BuildFrustum
//     0x20AB70) get the wide one, besides the projection's own calls;
//   - vlCamera::GetTanHalfFieldOfView (0x1F2830) returns tan x k to the vegetation's renderer
//     (ecoQuadtree::Render, the call at 0x42E3BC), so the trees reach the sides;
//   - vlRenderDevicePS2::GetDisplayModeInfo (0x25DD50, its aspect at +0x10 of the info it returns
//     through a0) gives the scene traversal's and the eco code's frustum (0x1F4B00 via 0x2191A4,
//     0x42A868) and BuildFrustum's callers its aspect times k, so what the wider picture shows is
//     not culled at its sides.
// The window then shows the picture at S, and the 2D (HUD, menus, text, drawn for 4:3) is narrowed
// by (4/3) / S, its groups kept at the screen's edges (the runtime's overlay squeeze).
//
// TF_ASPECT = screen (the default: the window's shape), 16:9 (the game's own), 21:9, 32:9 or W:H.
// Up to 2.4 (the data's tested range; 32:9 showed culling holes at the sides in 12 of 90 frames
// in the PAL tests, so it is clamped). Movies stay 4:3.

#include <cstdint>

class PS2Runtime;

namespace ultrawide
{
    // Installs the hooks (applyTransformers). They do nothing while the shape is 16:9.
    void install(PS2Runtime &runtime);
    // The window's size, from the window thread each frame (for TF_ASPECT=screen).
    void noteWindow(int width, int height);
    // The shape the game's wide picture is shown at (16:9 or wider).
    float shape();
    // shape / 16:9 while the game draws its widescreen picture, else 1 (the game thread's last frame()).
    float wideFactor();
    // Once a rendered frame, on the game thread: the 4/3 constant for the shape.
    void frame(uint8_t *rdram);
    // The settings panel's choice (F5, Graphics).
    void addSettings();
    // The HQ (hq_rate.cpp, each game frame): its scene at the game's own field of view - the Field of
    // view setting left out - so the ring's band and its name sit as on a 4:3 screen, the picture
    // only wider at the sides (TODO 52, the user's decision 2026-10-28). TF_HQ_FOV=1: the setting there too.
    void setInHq(bool inHq);
    // A taScreenInGMenu screen is up (menu_mouse.cpp, each game frame): in the HQ, one of its hologram
    // panels - the camera closes in on it while it is (TF_HQ_PANELS, TODO 53).
    void notePanel(bool shown);
    // An HQ panel is up, zoomed in all the way (TF_HQ_PANELS on): it is where it was measured to be (menu_mouse.cpp points at it
    // only then; while it eases in or out, or with the setting off, it is somewhere else).
    bool panelSettled();
}
