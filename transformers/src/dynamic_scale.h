#pragma once
// (TODO 295) The dynamic render scale: on built-in / low-end graphics, while the render scale is the built-in default
// (graphics_driver::applyLowEndDefaults set it - the player has not chosen one), the picture's share of the window's
// lines follows the GPU so the frame rate holds: 67, 63, 58, 54 or 50%. The user's rule (2026-11-13): a steady 60 first -
// below its start level the 480-line floor the default keeps (the PS2 shows 448) is waived, down to 360 lines at 50% of a
// 720p window, only while the GPU cannot hold the cap; a scale the player sets keeps the floor. Once a second the GPU's own time a frame (its timestamps, the 90th percentile of the last
// second's frames) is judged against the frame budget (the cap's period, at most 60's: 16.7 ms) - down fast, up slowly
// (dynamic_scale.cpp: the UHD 620's cost is a cliff). While it can still step down, the frame-rate governor's fall for the
// drawing waits for it (canGiveWay). A render scale the player sets (the F1 setting, settings.ini,
// TF_NATIVE_RENDER_SCALE) turns it off for good. TF_DYNAMIC_SCALE=0 / 1 says so for tests. The F1 row shows
// "Auto (now N%)" while it runs.

namespace dynamic_scale
{
    // Every frame, on the game's thread: `cap` the frame rate cap (0: unlimited).
    void tick(int cap);
    // Whether it runs now (the render scale is still the dynamic one's).
    bool active();
    // Whether it can still step down (it runs, above its floor): the frame-rate governor's fall for the drawing waits.
    bool canGiveWay();
}
