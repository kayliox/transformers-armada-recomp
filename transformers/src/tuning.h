#pragma once
// Tuning: the game's speed (tuning.cpp). TF_GAME_SPEED=<factor>; F5 Cheats "Game speed".

#include <cstdint>

namespace tuning
{
    // (no setting rows of its own: the Game speed and Gravity rows are an extension's, overlay tuning_settings.cpp)
    float speed();
    float gravity(); // the physics' gravity step x it
    void setSpeed(float speed); // (0: frozen, photo mode)
    void setGravity(float gravity);
    // Once a game frame (overrides.cpp).
    void frame(uint8_t *rdram, bool inLevel);
    // Right after a state has loaded, before anything reads its level (overrides.cpp adoptLoadedState): a state saved
    // while the game was stopped gets the level's own frame period back (TODO 269). True when it was one.
    bool thawLoadedState(uint8_t *rdram);
    // True once after such a state was repaired: photo mode's hidden HUD comes back too.
    bool takeFrozenState();
}
