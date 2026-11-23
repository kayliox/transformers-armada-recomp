// The game's console when it is started from a command window (console_attach.cpp).
#pragma once

namespace console_attach
{
    // First thing in main. Windows: the game is a windows program (no console of its own); started from a command window
    // with its output not already sent to a file or a pipe, stdout and stderr are reopened on that window. Elsewhere: nothing.
    void attachParent();
}
