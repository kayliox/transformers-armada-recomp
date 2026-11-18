// nr_core S9a: the window's inner seam (nr_window.cpp) - what a test (tests/nr_core_smoke.cpp) reaches below nr_core.h. Hosts use
// nr_core.h (open with a WindowSpec, window, windowSize, pumpEvents, closeWindow, WindowEvents).
#pragma once

#include <cstdint>

namespace nr_core::window_detail
{
    // One raw-input report (Windows: a RAWINPUT record, as GetRawInputData gives it) turned into KeyEvent / MouseEvent and
    // forwarded through WindowEvents. False when it is neither a keyboard's nor a mouse's.
    bool forwardRawInput(const void *rawInput);
    // Whether the core registered raw input for the window in use, and for what (bit 0 keyboard, bit 1 mouse).
    uint32_t rawInputRegistered();
    // Whether the core watches a host's window (its thread's message hooks in place).
    bool watchingHostWindow();
}
