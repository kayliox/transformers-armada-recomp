#pragma once
// (0.7.1, the user's request) A one-time reset of the graphics settings to the laptop defaults on built-in or low-end
// graphics (graphics_driver::lowEnd): players there may have kept settings from before those defaults were tuned
// (TODO 295), or set something the PC cannot keep up with. laptop_reset.cpp.

namespace laptop_reset
{
    // Before settings.ini is read (it rewrites the file): once on such a PC, the graphics and frame-rate rows taken out
    // of settings.ini (a copy kept as settings.ini.bak-071), so their laptop defaults apply; a line
    // TF_LAPTOP_RESET_DONE=071 keeps it from happening again, and what the player picks afterwards stays. A message box
    // says so once (the picture's shape, field of view, a texture pack and the renderer - native, unless the graphics
    // driver is a pre-2023 Intel one - go back to theirs too). Controls, mouse, sound, gameplay, mods, saves and the
    // window's mode and place are kept.
    // TF_LAPTOP_RESET=0: never.
    void run();
}
