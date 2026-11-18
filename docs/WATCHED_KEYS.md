# Rapid watched-key presses

Patch `0049-watched-key-press-queue.patch` fixes missed rapid presses in the
runtime's watched-key API, including the game's F1 mod-menu toggle.

Raylib's `IsKeyPressed` compares the previous and current held states. When GLFW
processes a key-down and key-up in the same input poll, both states can be up and
that comparison misses the press. GLFW still puts the original key-down event in
Raylib's keycode queue. The runtime now counts those events once per poll.

This changes genuine input handling; it does not synthesize keys or change game
memory. Held keys do not count again without a new key-down. Two distinct taps
retain a count of two, preserving the menu's existing toggle parity.

The compiled game and runtime have no other `GetKeyPressed` consumer. The ImGui
backend uses held/released key state and the separate `GetCharPressed` character
queue, which this patch does not consume or modify. A future keycode-queue
consumer would need to share this collection point.

The patch does not change held gamepad controls, Escape handling, or the menu's
existing Ctrl+F1 exclusion. It has no dependency on character skins or renderer
changes.

## Validation

After applying the normal runtime patch sequence, run from an MSVC developer shell:

```powershell
python tools/test_watched_keys.py --runtime-source PS2Recomp/ps2xRuntime/src/lib/ps2_pad.cpp --build-directory "<external build directory>"
```

The test extracts the actual collector from the patched source and compiles it
against controlled event queues. Cases cover ordinary presses, held keys,
same-poll press/release, distinct rapid taps, unrelated typed characters,
unavailable windows, empty queues, and the unchanged Ctrl exclusion. It does not
start a game or window.

In the actual native game, the same rapid automated F1 taps that previously had
no effect opened and closed the existing mod menu after this patch. Mouse input
and menu interaction remained available. No character-mod behavior is needed
for that check.

## The port's own keys read once a picture (TODO 433)

The watched keys above are counted by the runtime at every input poll. The port's
other keys are read by the overlay once a picture: the F1 menu's Esc, photo mode's
Esc, H, R and mouse wheel, and the flight recorder's F10. Those used raylib's
`IsKeyPressed` / `GetMouseWheelMove`, which hold only the window's last input poll
(about a millisecond, the loop polls every millisecond between pictures), so at 30
or 60 pictures a second most presses and wheel notches were lost: a real Esc did not
close F1 on a laptop.

They now come from the window's own key and wheel messages, the same hook that
gives the menus their typed characters and wheel (TODO 170/174):
`mod_menu::takePictureKeyPresses(key)` returns every press of a key since the last
picture (and takes them, so one press is not read twice), `mod_menu::pictureWheel()`
the wheel's notches. Key auto-repeats are left out. `TF_PICTURE_KEYS=0` reads them as
before. With the SDL3 window host (PS2X_PLATFORM=sdl3) the old reads stay.
