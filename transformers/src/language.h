#pragma once
// The game's language (language.cpp): the PAL release's English, French, German, Spanish or Italian.
// TF_LANGUAGE=auto|en|fr|de|es|it, the overlay (F1), Gameplay: "Language" (PAL only; the USA disc is in English).

class PS2Runtime;

namespace language
{
    void addSettings();
    // The language the game asks the console for (sceScfGetLanguage): the setting's.
    void install(PS2Runtime &runtime);
}
