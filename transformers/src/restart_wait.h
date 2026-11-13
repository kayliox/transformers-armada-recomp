// The game's restart (F1's Restart now, the PC settings' Restart to apply) starts the game again as a new process and the first
// one exits. Where a launcher keeps the game's place by the process it started - Steam's Game Mode, which then counts the game
// as closed and goes back to its library while the new process runs behind it - that looks like a restart that did not happen.
// So the first process stays until the new one ends, with nothing left to do but wait: main() calls wait() as its last step
// (before std::_Exit, which skips atexit handlers). While it waits every other thread of the process is suspended (a waiting
// process must not keep a core busy next to the new game) and it ends itself when the new game ends. With no such process - the
// usual exit - wait() returns at once.
#pragma once

namespace restart_wait
{
    void wait();
}
