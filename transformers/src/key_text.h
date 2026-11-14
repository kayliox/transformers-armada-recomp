#pragma once
// The game's words for the controller's sticks said for the keyboard (TODO 223): while the button prompts show keys
// (ps2_host_input::keyboardPromptsShown - the keyboard and mouse used last), the strings that name an analog stick or
// the sticks' L3 / R3 buttons read with the player's own keys instead ("W A S D - Move/Strafe", "Mouse - Look/Aim",
// "A D - Steering", "Recon on B"), from the player's own key layout (the defaults with controls.txt over them; never
// the menus' overlay, which moves Enter, Space and Escape: mod_menu::playerKeyLayout - the hints describe play). The
// button symbols in the text were already keys (the crisp fonts' text_keys set); these are the words around them.
// In the same strings "the L1 button" and the like become the key too (the Glide Mini-Con's description).
//
// How: the game's string table (STRINGS\ENGLISH.TXT, loaded at boot) keeps each string as a CodeWarrior std::string
// (word 0 = capacity << 1 | 1, +4 its size, +8 a pointer to the characters). At the keyboard's first use after each
// load, and again at each switch to it (TODO 348 (b): a hint opened meanwhile holds copies of its own), the strings that
// name a stick are found (a search of the game's RAM, on a thread of its own, about 0.3 s; the button symbols wait for
// it: symbolsAsKeys) with the std::string
// that holds each; their characters are then written over in place with the keyboard's wording, and the size set -
// only where the new text is no longer than the game's, so nothing past its buffer is touched and the pointer stays
// the game's. A controller used again puts the game's own words back. Checked once a second and at every change of
// device or keys (a state loaded puts back what it held); a string no longer where it was found is looked for again.
// TF_KEY_TEXT=0: never. TF_KEY_TEXT_LOG=1: the layout in the log.

#include <cstdint>

namespace key_text
{
    // Every game frame, on the game's thread (overrides.cpp frameKick).
    void frame(uint8_t *rdram);
    // (TODO 348 (b)) Whether the button symbols show keys, given `keys` (the device in use is the keyboard): while the
    // words naming the sticks are still the other device's (a search after a switch, about 0.3 s, or the next game frame)
    // the symbols stay the other device's too, so a hint never mixes "Left analog stick" with a SPACE key - 2.5 s at
    // most. Any thread (the renderer's: nr_hires keyPromptsShown).
    bool symbolsAsKeys(bool keys);
    // TF_KEY_TEXT_SWITCH_TEST=<vblank> (tests, TODO 348 (b)): the prompts as for the controller until that VBlank, then as
    // for the keyboard - a switch of device on a machine without a controller. Called before frame, with the VBlank.
    void switchTest(uint64_t vblank);
    // (TODO 309) The HQ ended: the strings found are let go (the HQ's pages freed; a level's load is seen by itself).
    void levelChanged();
}
