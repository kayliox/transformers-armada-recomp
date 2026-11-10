#pragma once
// The game's language (language.cpp): the PAL release's English, French, German, Spanish or Italian.
// TF_LANGUAGE=auto|en|fr|de|es|it, the overlay (F1), Gameplay: "Language" (PAL only; the USA disc is in English).

#include <cstdint>

class PS2Runtime;
struct R5900Context;

namespace language
{
    void addSettings();
    // The language the game asks the console for (sceScfGetLanguage): the setting's.
    void install(PS2Runtime &runtime);
    // Once a frame on the game's thread (the frame kick): the running game set to the setting's language when it
    // has another (a save state, the overlay's Restart, a change of the setting).
    void frame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    // At the frame kick's very start: true when the game was sent to its text loader for the setting's language
    // (the frame kick returns at once, and is entered again when the loader is done).
    bool redirectForText(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
}
