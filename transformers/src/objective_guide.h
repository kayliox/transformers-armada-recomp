#pragma once
// (TODO 315) The objective guide: a small arrow at the bottom of the screen pointing to the level's next story trigger,
// with its name and distance. Off by default (Gameplay > Objective guide, TF_OBJECTIVE_GUIDE). objective_guide.cpp has
// the details.

#include <cstdint>

class PS2Runtime;

namespace objective_guide
{
    // The setting (before the window loop).
    void addSettings();
    // The hook on the triggers' wait (after revisit_music's, which it chains).
    void install(PS2Runtime &runtime);
    // Once a game frame, on the game thread: the armed triggers seen since the last frame, the target picked.
    void frame(const uint8_t *rdram, bool inLevel, uint32_t playerPosition, bool moviePlaying, uint64_t tick);
    // The arrow, its name and distance (the window thread, raylib); nothing when there is no target or it is off.
    void draw();
}
