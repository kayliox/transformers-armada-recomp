#pragma once
// The surround encoding of the game's 3D sounds (surround_mode.cpp, TODO 305 (10)): F1 > Sound > "Surround encoding".

class PS2Runtime;

namespace surround_mode
{
    // The Sound tab's setting (before the window loop starts).
    void addSettings();
    // The hook on the game's Pro Logic II switch.
    void install(PS2Runtime &runtime);
}
