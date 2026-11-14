#pragma once
// The Crosshair row (crosshair_row.cpp, TODO 355): F1 > Gameplay > Crosshair, Original (the game's own, the default) or
// Fixed (still, in the dead centre). TF_CROSSHAIR=original / fixed, live, kept in the settings file.

namespace crosshair_row
{
    void addSettings();
    // Each game frame: the row's word to static_aim (setFixed).
    void gameFrame();
    // (TODO 358) F1 > Gameplay > Crosshair size, TF_CROSSHAIR_SIZE: the crosshair's size as a share of the game's own,
    // 0.25 to 1 (the default, the largest). Read by photo_mode (the game's ring, its SetScale) and small_crosshair.
    float size();
}
