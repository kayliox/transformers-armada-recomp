// The game's words for the sticks said for the keyboard (TODO 223): key_text.h.

#include "key_text.h"

#include "mod_menu.h"

#include "ps2_runtime.h"
#include "runtime/ps2_host_input.h"

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace key_text
{
    namespace
    {
        // One string of the table that names a stick: its CodeWarrior std::string (word 0 = capacity << 1 | 1, +4 the
        // size, +8 the pointer to the characters; measured on the loaded table) and its text as the game has it.
        struct Entry
        {
            uint32_t object = 0, chars = 0;
            std::string original;
            std::string written; // (the text last written there: the original or a keyboard wording - "" none yet)
        };

        std::mutex g_mutex;
        std::vector<Entry> g_entries; // (found by the search; the game's thread applies them)
        // (TODO 309: those let go at a load, kept to be taken back after it where the string is still the game's - the boot
        // table's - with its original: the search finds only texts that still name a button or stick, not ones rewritten)
        std::vector<Entry> g_previous;
        std::atomic<bool> g_searching{false};
        bool g_searched = false;
        bool g_shown = false;                // the keyboard texts in place
        std::string g_shownLayout;           // (the layout they were made from: a change makes them again)
        std::chrono::steady_clock::time_point g_lastSearch{}, g_lastCheck{};
        int g_searches = 0;
        // (TODO 309: a load frees what the strings were found in - the HQ's Mini-Con pages - and the heap gives their bytes
        // to the level: the strings are let go when a load begins or the HQ ends, and looked for again once it settles)
        std::atomic<bool> g_dropRequested{false};
        std::atomic<unsigned> g_epoch{0u}; // (a search started before a drop: its results thrown away)
        std::chrono::steady_clock::time_point g_holdUntil{};
        uint32_t g_loadingBefore = 0u;
        constexpr uint32_t kLoading = 0x006CD324u; // (set while the game loads - and throughout the HQ)

        uint32_t word(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, rdram + a, 4);
            return v;
        }
        void setWord(uint8_t *rdram, uint32_t a, uint32_t v) { std::memcpy(rdram + a, &v, 4); }

        // (TODO 309) The string is still exactly as last left there - the game's text or ours - in the same std::string.
        bool stillHeld(const uint8_t *rdram, const Entry &e)
        {
            const std::string &held = e.written.empty() ? e.original : e.written;
            const uint32_t flags = word(rdram, e.object);
            return e.chars + held.size() + 1u < PS2_RAM_SIZE && word(rdram, e.object + 8u) == e.chars && (flags & 1u) &&
                   (flags >> 1) >= e.original.size() && word(rdram, e.object + 4u) == held.size() &&
                   std::memcmp(rdram + e.chars, held.data(), held.size()) == 0 && rdram[e.chars + held.size()] == 0;
        }

        bool enabled()
        {
            static const bool on = !(std::getenv("TF_KEY_TEXT") && *std::getenv("TF_KEY_TEXT") == '0');
            return on;
        }

        // ---- The keys' names, as the prompts' key caps show them (make_crisp_fonts.py: shown_key, KEY_SHORT) ----
        std::map<std::string, std::vector<std::string>> layout(const std::string &text)
        {
            std::map<std::string, std::vector<std::string>> out;
            size_t at = 0;
            while (at < text.size())
            {
                size_t end = text.find(';', at);
                if (end == std::string::npos)
                    end = text.size();
                const std::string part = text.substr(at, end - at);
                at = end + 1;
                const size_t eq = part.find('=');
                if (eq == std::string::npos)
                    continue;
                std::vector<std::string> keys;
                size_t k = eq + 1;
                while (k <= part.size())
                {
                    size_t c = part.find(',', k);
                    if (c == std::string::npos)
                        c = part.size();
                    if (c > k)
                        keys.push_back(part.substr(k, c - k));
                    k = c + 1;
                }
                out[part.substr(0, eq)] = keys;
            }
            return out;
        }

        std::string keyWord(const std::string &key)
        {
            static const std::map<std::string, std::string> shortNames = {
                {"ESCAPE", "ESC"}, {"BACKSPACE", "BKSP"}, {"INSERT", "INS"}, {"DELETE", "DEL"}, {"PAGE_UP", "PGUP"},
                {"PAGE_DOWN", "PGDN"}, {"LEFT_SHIFT", "SHIFT"}, {"RIGHT_SHIFT", "SHIFT"}, {"LEFT_CONTROL", "CTRL"},
                {"RIGHT_CONTROL", "CTRL"}, {"LEFT_ALT", "ALT"}, {"RIGHT_ALT", "ALT"}, {"CAPS_LOCK", "CAPS"},
                {"MOUSE_LEFT", "left click"}, {"MOUSE_RIGHT", "right click"}, {"MOUSE_MIDDLE", "middle click"},
                {"MOUSE_SIDE", "side mouse button"}, {"MOUSE_EXTRA", "extra mouse button"},
                {"MOUSE_WHEEL_UP", "wheel up"}, {"MOUSE_WHEEL_DOWN", "wheel down"}};
            const auto it = shortNames.find(key);
            if (it != shortNames.end())
                return it->second;
            if (key.rfind("KP_", 0) == 0)
                return "keypad " + key.substr(3);
            std::string s = key;
            std::replace(s.begin(), s.end(), '_', ' ');
            return s;
        }

        std::string shownKey(const std::map<std::string, std::vector<std::string>> &keys, const char *button)
        {
            const auto it = keys.find(button);
            if (it == keys.end() || it->second.empty())
                return {};
            const std::vector<std::string> &k = it->second;
            static const char *const modifiers[] = {"LEFT_SHIFT", "RIGHT_SHIFT", "LEFT_CONTROL", "RIGHT_CONTROL", "LEFT_ALT", "RIGHT_ALT"};
            if (std::find_if(std::begin(modifiers), std::end(modifiers), [&](const char *m) { return k[0] == m; }) != std::end(modifiers))
                for (const std::string &m : k)
                    if (m.rfind("MOUSE_", 0) == 0)
                        return keyWord(m);
            return keyWord(k[0]);
        }

        void replaceAll(std::string &s, const std::string &from, const std::string &to)
        {
            for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
                s.replace(at, from.size(), to);
        }

        // (TODO 288 follow-up, the European disc) The same hints in French, German, Italian and Spanish: the tables'
        // own phrases for the sticks and the buttons (Windows-1252, as the tables hold them), said with the player's keys
        // in each language's words - "touche" / "Taste" / "tasto", the mouse as "Souris" / "Maus" / "Mouse" / "Rat\xf3n",
        // the mouse buttons named in the language. Nothing matches a string of another language, so the English table
        // (and the USA disc) is untouched. The same text when nothing in it changes. `compact`: a key named without the
        // word for "key" ("Appuyer sur SPACE", "Auf SPACE legen", "Collega a SPACE"), for a line too long the other way.
        std::string europeanText(const std::string &original, const std::map<std::string, std::vector<std::string>> &keys,
                                 bool compact = false)
        {
            const auto has = [&](const char *t) { return original.find(t) != std::string::npos; };
            // (the Mini-Con descriptions and their equip page name only buttons: "touche R1", "R1-Taste", "tasto R1",
            // "bot\xf3n R1" - words no English string holds)
            const int lang = has("analogique") || has("Recon avec R3") || has("touche")                      ? 1 // French
                             : has("Analog-Stick") || has("Aufkl\xe4rung: R3") || has("-Taste") || has(" mit L2 ") ? 2 // German
                             : has("evetta analogica") || has("tasto ") || has("tasti L1")                   ? 3 // Italian
                             : has("oystick anal\xf3g") || has("bot\xf3n") || has("botones L1")              ? 4 // Spanish
                                                                                                             : 0;
            if (lang == 0)
                return original;
            // (the mouse buttons' names, as keyWord gives them in English, in the string's language)
            static const std::map<std::string, std::string> mouse[5] = {
                {},
                {{"left click", "clic gauche"}, {"right click", "clic droit"}, {"middle click", "clic milieu"},
                 {"side mouse button", "bouton souris"}, {"extra mouse button", "bouton souris"},
                 {"wheel up", "molette haut"}, {"wheel down", "molette bas"}},
                {{"left click", "Linksklick"}, {"right click", "Rechtsklick"}, {"middle click", "Mittelklick"},
                 {"side mouse button", "Maustaste"}, {"extra mouse button", "Maustaste"},
                 {"wheel up", "Mausrad hoch"}, {"wheel down", "Mausrad runter"}},
                {{"left click", "clic sinistro"}, {"right click", "clic destro"}, {"middle click", "clic centrale"},
                 {"side mouse button", "tasto mouse"}, {"extra mouse button", "tasto mouse"},
                 {"wheel up", "rotellina su"}, {"wheel down", "rotellina gi\xf9"}},
                {{"left click", "clic izquierdo"}, {"right click", "clic derecho"}, {"middle click", "clic central"},
                 {"side mouse button", "bot\xf3n del rat\xf3n"}, {"extra mouse button", "bot\xf3n del rat\xf3n"},
                 {"wheel up", "rueda arriba"}, {"wheel down", "rueda abajo"}}};
            const auto key = [&](const char *button) {
                std::string k = shownKey(keys, button);
                if (k.empty() && std::string(button) == "r3" && !ps2_host_input::mouseButtonBound(1))
                    k = "right click"; // (TODO 224: R3 has no key by default, the right mouse button does recon mode)
                const auto it = mouse[lang].find(k);
                return it != mouse[lang].end() ? it->second : k;
            };
            const std::string up = key("lup"), left = key("lleft"), down = key("ldown"), right = key("lright");
            const std::string move = up + " " + left + " " + down + " " + right;
            const std::string l1 = key("l1"), cross = key("cross"), r3 = key("r3");
            // (a button on the mouse is no key: "le clic milieu", not "la touche clic milieu")
            const auto onMouse = [&](const char *button) { return mouse[lang].count(shownKey(keys, button)) != 0; };
            // (the wheel is feminine in French and Italian: "la molette haut enfonc\xe9e", "la rotellina su")
            const auto wheel = [](const std::string &name) { return name.rfind("molette", 0) == 0 || name.rfind("rotellina", 0) == 0; };
            // (TODO 288 in the other languages: the shoulder buttons the Mini-Cons are equipped to. A mouse button starts
            // the sentence as English's "Left-click to fire" does - "Clic gauche pour tirer", "Linksklick, um ...",
            // "Clic sinistro per sparare", "Clic izquierdo para disparar" - so the line stays no longer than the game's.)
            static const std::pair<const char *, const char *> shoulders[] = {{"L1", "l1"}, {"L2", "l2"}, {"R1", "r1"}, {"R2", "r2"}};
            const auto capital = [](std::string k) {
                if (!k.empty())
                    k[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(k[0])));
                return k;
            };
            const std::string l2 = key("l2"), r1 = key("r1"), r2 = key("r2");
            const bool allFour = !l1.empty() && !l2.empty() && !r1.empty() && !r2.empty();
            std::string s = original;
            if (lang == 1)
            {
                replaceAll(s, "Joystick analogique gauche - Direction", left + " " + right + " - Direction");
                replaceAll(s, "joystick vers l'arri\xe8re", "touche " + down);
                replaceAll(s, "Joystick analogique gauche", move);
                replaceAll(s, "Joystick analogique droit", "Souris");
                if (allFour)
                    replaceAll(s, "aux touches L1, L2, R1 et R2", "\xe0 " + l1 + ", " + l2 + ", " + r1 + " et " + r2);
                for (const auto &[name, button] : shoulders)
                {
                    const std::string k = key(button), touche = std::string("touche ") + name;
                    if (k.empty())
                        continue;
                    if (!onMouse(button))
                    {
                        if (compact)
                            replaceAll(s, "la " + touche, k);
                        replaceAll(s, touche, compact ? k : "touche " + k);
                        continue;
                    }
                    const std::string le = wheel(k) ? "la " : "le ", held = wheel(k) ? " enfonc\xe9" "e" : " enfonc\xe9";
                    // ("la maintenir", "la rel\xe2cher": the key, now a click - "le maintenir", "le rel\xe2cher")
                    if (s.find(touche) != std::string::npos)
                    {
                        replaceAll(s, "la maintenir enfonc\xe9" "e", le + "maintenir");
                        replaceAll(s, "la rel\xe2" "cher", le + "rel\xe2" "cher");
                    }
                    replaceAll(s, "Appuyer sur la " + touche, capital(k));
                    replaceAll(s, "Maintenir la " + touche + " enfonc\xe9" "e", "Maintenir " + le + k);
                    replaceAll(s, "la " + touche + " enfonc\xe9" "e", le + k + held);
                    replaceAll(s, touche + " enfonc\xe9" "e", k + held);
                    replaceAll(s, "la " + touche, le + k);
                    replaceAll(s, touche, k);
                }
                if (!cross.empty())
                    replaceAll(s, "touche X", onMouse("cross") ? cross : "touche " + cross);
                if (!r3.empty())
                    replaceAll(s, "Recon avec R3", "Recon avec " + r3);
            }
            else if (lang == 2)
            {
                replaceAll(s, "Linker Analog-Stick - Steuern", left + " " + right + " - Steuern");
                replaceAll(s, "Linker Analog-Stick nach unten", down + "-Taste");
                replaceAll(s, "Linker Analog-Stick", move);
                replaceAll(s, "Rechter Analog-Stick", "Maus");
                if (allFour)
                    replaceAll(s, "auf die L1-, L2-, R1- und R2-Tasten", "auf " + l1 + ", " + l2 + ", " + r1 + " und " + r2);
                for (const auto &[name, button] : shoulders)
                {
                    const std::string k = key(button), taste = std::string(name) + "-Taste";
                    if (k.empty())
                        continue;
                    replaceAll(s, std::string(" mit ") + name + " ", " mit " + k + " ");
                    if (!onMouse(button))
                    {
                        replaceAll(s, taste, compact ? k : k + "-Taste");
                        continue;
                    }
                    if (s.find(taste) != std::string::npos)
                        replaceAll(s, " gedr\xfc" "ckt halten", " halten");
                    replaceAll(s, taste + " dr\xfc" "cken, um", k + ", um");
                    replaceAll(s, taste + " zum Feuern dr\xfc" "cken", k + " zum Feuern");
                    replaceAll(s, taste, k);
                }
                if (!cross.empty())
                    replaceAll(s, "X-Taste", onMouse("cross") ? cross : cross + "-Taste");
                if (!r3.empty())
                    replaceAll(s, "Aufkl\xe4rung: R3", "Aufkl\xe4rung: " + r3);
            }
            else if (lang == 3)
            {
                replaceAll(s, "Levetta analogica sinistra: sterzo", left + " " + right + ": sterzo");
                replaceAll(s, "muovi la levetta analogica sinistra in basso", "tieni premuto " + down);
                replaceAll(s, "Muovi la levetta analogica sinistra", "Usa " + move);
                replaceAll(s, "Levetta analogica sinistra", move);
                replaceAll(s, "Levetta analogica destra", "Mouse");
                if (!r3.empty())
                    replaceAll(s, "Ricognizione: tasto R3", "Ricognizione: " + r3);
                if (allFour)
                    replaceAll(s, "con i tasti L1, L2, R1 e R2", "con " + l1 + ", " + l2 + ", " + r1 + " e " + r2);
                for (const auto &[name, button] : shoulders)
                {
                    const std::string k = key(button), tasto = std::string("tasto ") + name;
                    if (k.empty())
                        continue;
                    if (!onMouse(button))
                    {
                        if (compact)
                        {
                            replaceAll(s, "al " + tasto, "a " + k);
                            replaceAll(s, "il " + tasto, k);
                        }
                        replaceAll(s, tasto, compact ? k : "tasto " + k);
                        continue;
                    }
                    const std::string il = wheel(k) ? "la " : "il ";
                    if (wheel(k) && s.find(tasto) != std::string::npos)
                        for (const char *it : {"premilo", "tienilo", "rilascialo"})
                        {
                            std::string la = it;
                            la.back() = 'a'; // ("premila": the wheel)
                            replaceAll(s, it, la);
                        }
                    replaceAll(s, "al " + tasto, (wheel(k) ? "alla " : "al ") + k);
                    replaceAll(s, "Premi il " + tasto, capital(k));
                    replaceAll(s, "Tieni premuto il " + tasto, "Tieni " + il + k);
                    replaceAll(s, "il " + tasto, il + k);
                    replaceAll(s, tasto, k);
                }
                if (!cross.empty())
                {
                    if (wheel(cross))
                        replaceAll(s, "il tasto X", "la " + cross);
                    replaceAll(s, "tasto X", onMouse("cross") ? cross : "tasto " + cross);
                }
            }
            else
            {
                replaceAll(s, "Joystick anal\xf3gico izquierdo: Direcci\xf3n", left + " " + right + ": Direcci\xf3n");
                replaceAll(s, "tira del joystick anal\xf3g. izq.", "mant\xe9n " + down);
                replaceAll(s, "el joystick anal\xf3g. izq.", move);
                replaceAll(s, "Joystick anal\xf3gico izquierdo", move);
                replaceAll(s, "Joystick anal\xf3gico derecho", "Rat\xf3n");
                if (allFour)
                    replaceAll(s, "con los botones L1, L2, R1 y R2", "con " + l1 + ", " + l2 + ", " + r1 + " y " + r2);
                for (const auto &[name, button] : shoulders)
                {
                    const std::string k = key(button), boton = std::string("bot\xf3n ") + name;
                    if (k.empty())
                        continue;
                    if (!onMouse(button))
                    {
                        replaceAll(s, "al " + boton, "a " + k);
                        replaceAll(s, "el " + boton, k);
                        continue;
                    }
                    const std::string el = k.rfind("rueda", 0) == 0 ? "la " : "el ";
                    replaceAll(s, "Pulsa el " + boton, capital(k));
                    replaceAll(s, "Mant\xe9n pulsado el " + boton, "Mant\xe9n " + el + k);
                    replaceAll(s, "Mant\xe9n el " + boton + " pulsado", "Mant\xe9n " + el + k);
                    replaceAll(s, "al " + boton, (el == "el " ? "al " : "a la ") + k);
                    replaceAll(s, "el " + boton, el + k);
                }
                if (!l1.empty())
                {
                    replaceAll(s, "Pulsa L1", "Pulsa " + l1);
                    replaceAll(s, "pulsado L1", "pulsado " + l1);
                }
                if (!cross.empty())
                    replaceAll(s, "el bot\xf3n X", cross);
                if (!r3.empty())
                    replaceAll(s, "Recon. bot\xf3n R3", "Recon. " + r3);
            }
            if (!compact && s.size() > original.size())
                return europeanText(original, keys, true);
            return s;
        }

        // The keyboard's wording of a string (the stick phrases first, the longest first; then the buttons named in
        // the same strings). The same text when nothing in it changes.
        std::string keyboardText(const std::string &original, const std::map<std::string, std::vector<std::string>> &keys)
        {
            if (std::string eu = europeanText(original, keys); eu != original)
                return eu; // (the European disc's other languages)
            const std::string up = shownKey(keys, "lup"), left = shownKey(keys, "lleft"), down = shownKey(keys, "ldown"),
                              right = shownKey(keys, "lright");
            const std::string move = up + " " + left + " " + down + " " + right;
            std::string s = original;
            replaceAll(s, "Left analog stick - Steering", left + " " + right + " - Steering");
            replaceAll(s, "pull back on the left analog stick", "hold " + down);
            replaceAll(s, "Use the left analog stick", "Use " + move);
            replaceAll(s, "use the left analog stick", "use " + move);
            replaceAll(s, "the left analog stick", move);
            replaceAll(s, "the right analog stick", "the mouse");
            replaceAll(s, "Left analog stick", move);
            replaceAll(s, "Right analog stick", "Mouse");
            replaceAll(s, "left analog stick", move);
            replaceAll(s, "right analog stick", "the mouse");
            static const std::pair<const char *, const char *> buttons[] = {
                {"L1", "l1"}, {"R1", "r1"}, {"L2", "l2"}, {"R2", "r2"}, {"L3", "l3"}, {"R3", "r3"}, {"X", "cross"}, {"START", "start"}};
            for (const auto &[name, button] : buttons)
            {
                std::string key = shownKey(keys, button);
                if (key.empty() && std::string(button) == "r3" && !ps2_host_input::mouseButtonBound(1))
                    key = "right click"; // (TODO 224: R3 has no key by default, the right mouse button does recon mode)
                if (key.empty())
                    continue;
                const std::string the = std::string("the ") + name + " button";
                // (TODO 288: the Mini-Con descriptions, "Press the R1 button to fire...". A mouse button is clicked, not
                // pressed: "Left-click to fire", "Hold left click", "release right click" - each no longer than the
                // game's words.)
                const size_t click = key.size() > 6 ? key.rfind(" click") : std::string::npos;
                if (click != std::string::npos && click + 6 == key.size())
                {
                    std::string verb = key;
                    verb[click] = '-'; // "left-click"
                    std::string Verb = verb;
                    Verb[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(Verb[0])));
                    replaceAll(s, "Press " + the, Verb);
                    replaceAll(s, "press " + the, verb);
                }
                // (short lines with no room for a long key name: "Attach to R1 Button" -> "Attach: left click", "Recon on R3
                // button" -> "Recon: right click")
                for (const auto &[phrase, lead, shortLead] : {std::tuple<std::string, std::string, std::string>{
                                                                  std::string("Attach to ") + name + " Button", "Attach to ", "Attach: "},
                                                              std::tuple<std::string, std::string, std::string>{
                                                                  std::string("Recon on ") + name + " button", "Recon on ", "Recon: "}})
                    replaceAll(s, phrase, (lead + key).size() <= phrase.size() ? lead + key : shortLead + key);
                replaceAll(s, the, key);
                replaceAll(s, std::string(name) + " button", key);
                replaceAll(s, std::string(name) + " Button", key); // (the Mini-Con menu's "Attach to L2 Button")
            }
            return s;
        }

        // ---- The search: the strings that name a stick, and the std::strings that hold them ----
        void search(const uint8_t *rdram, unsigned epoch)
        {
            const auto t0 = std::chrono::steady_clock::now();
            const uint32_t from = 0x00100000u, to = PS2_RAM_SIZE;
            // (TODO 288: "button" for the buttons' own lines - the Mini-Con descriptions, their equip page's "Attach to R2
            // Button" - the strings it finds that name no button left as they are; and on the European disc its French, German,
            // Italian and Spanish phrases: europeanText. The USA disc has English only, so it does not search for those.)
            static const char *const marks[] = {"analog stick", "button", "Button",
#if defined(TF_GAME_PAL)
                                                "Joystick analogique", "joystick vers l'arri\xe8re", "Recon avec R3", "Analog-Stick",
                                                "Aufkl\xe4rung: R3", "evetta analogica", "oystick anal\xf3g",
                                                // (the Mini-Con descriptions and their equip page in those languages - few
                                                // and short, as every mark is a pass over the memory; "tasto R" and
                                                // "bot\xf3n R" find the recon hints too. None finds the title's "START-Taste",
                                                // "tasto START", "bot\xf3n START": mod_menu's, which this would put back.)
                                                "touche", "1-Taste", "2-Taste", " mit L2 ", "tasto L", "tasto R", "tasti L1",
                                                "bot\xf3n L", "bot\xf3n R", "botones L1",
#endif
            };
            std::vector<uint32_t> starts;
            for (const char *m : marks)
            {
                const size_t n = std::strlen(m);
                const char *ram = reinterpret_cast<const char *>(rdram);
                const char *begin = ram + from, *end = ram + to;
                const std::boyer_moore_horspool_searcher finder(m, m + n);
                for (const char *at = begin;;)
                {
                    const char *hit = std::search(at, end, finder);
                    if (hit == end)
                        break;
                    uint32_t a = static_cast<uint32_t>(hit - ram);
                    uint32_t s = a;
                    while (s > from && rdram[s - 1] != 0 && a - s < 4096u)
                        --s;
                    starts.push_back(s);
                    at = hit + n;
                }
            }
            // (sorted, once each: the pass below looks every word of memory up in them)
            std::sort(starts.begin(), starts.end());
            starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
            std::vector<Entry> found;
            if (!starts.empty())
            {
                const uint32_t min = starts.front(), max = starts.back();
                for (uint32_t p = from + 8u; p + 4u <= to; p += 4u)
                {
                    const uint32_t v = word(rdram, p);
                    if (v < min || v > max || !std::binary_search(starts.begin(), starts.end(), v))
                        continue;
                    const uint32_t object = p - 8u, flags = word(rdram, object), size = word(rdram, object + 4u);
                    // (a long-form std::string: an odd first word, its capacity at least the size, the text that long)
                    if (!(flags & 1u) || (flags >> 1) < size || size == 0u || size > 4096u || v + size >= to ||
                        rdram[v + size] != 0 || std::strlen(reinterpret_cast<const char *>(rdram + v)) != size)
                        continue;
                    Entry e;
                    e.object = object;
                    e.chars = v;
                    e.original.assign(reinterpret_cast<const char *>(rdram + v), size);
                    found.push_back(std::move(e));
                }
            }
            std::fprintf(stderr, "[key text] the sticks' strings: %zu found (%zu texts), %.0f ms\n", found.size(), starts.size(),
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            std::lock_guard<std::mutex> lock(g_mutex);
            if (epoch != g_epoch.load())
                return; // (a load began while it searched: what it found may be freed - looked for again)
            // (TODO 309: the strings already rewritten no longer name a button, so the search does not find them: those still
            // held as left are kept, with their originals. One the search finds again - a keyboard wording that still names a
            // button, as the French "touche E" does - keeps the original it had: the search sees only the text written there.)
            for (Entry &e : g_entries)
            {
                if (!stillHeld(rdram, e))
                    continue;
                const auto same = std::find_if(found.begin(), found.end(), [&](const Entry &f) { return f.object == e.object && f.chars == e.chars; });
                if (same != found.end())
                    *same = std::move(e);
                else
                    found.push_back(std::move(e));
            }
            g_entries = std::move(found);
            g_searched = true;
        }

        // Each string as `keyboard` says (its keyboard text, or the original back), where it is still the game's.
        // False: a string is not where it was found (another table: a state of another session) - search again.
        bool apply(uint8_t *rdram, bool keyboard, const std::string &layoutText)
        {
            const auto keys = layout(ps2_host_input::defaultKeyBindings() + ";" + layoutText);
            static bool toldLayout = std::getenv("TF_KEY_TEXT_LOG") == nullptr;
            if (!toldLayout)
            {
                toldLayout = true;
                std::fprintf(stderr, "[key text] layout: %s\n", layoutText.c_str());
            }
            std::lock_guard<std::mutex> lock(g_mutex);
            bool allThere = true;
            int changed = 0;
            for (Entry &e : g_entries)
            {
                // (still the game's std::string as found: its pointer, an odd first word and room for the original)
                const uint32_t flags = word(rdram, e.object);
                if (word(rdram, e.object + 8u) != e.chars || !(flags & 1u) || (flags >> 1) < e.original.size())
                {
                    allThere = false;
                    continue;
                }
                const std::string kb = keyboardText(e.original, keys);
                const std::string &want = keyboard && kb.size() <= e.original.size() ? kb : e.original;
                const uint32_t size = word(rdram, e.object + 4u);
                const std::string now(reinterpret_cast<const char *>(rdram + e.chars), std::min<uint32_t>(size, 4096u));
                if (now == want)
                    continue;
                // (TODO 309: written over only where it still holds the game's text or the one written last - an old layout's.
                // Any other text means the bytes are no longer this string's: the heap gave them to something else - the
                // level loaded after the HQ - and writing there broke the level's data: "Generic fatal error" at the warp.)
                if (now != e.original && now != kb && (e.written.empty() || now != e.written))
                {
                    allThere = false;
                    continue;
                }
                static int told = 24;
                if (told > 0)
                {
                    --told;
                    std::fprintf(stderr, "[key text] \"%s\" -> \"%s\"\n", now.c_str(), want.c_str());
                }
                std::memset(rdram + e.chars, 0, e.original.size() + 1u);
                std::memcpy(rdram + e.chars, want.data(), want.size());
                setWord(rdram, e.object + 4u, static_cast<uint32_t>(want.size()));
                e.written = want;
                ++changed;
            }
            if (changed)
                std::fprintf(stderr, "[key text] %d strings for the %s\n", changed, keyboard ? "keyboard" : "controller");
            return allThere;
        }
    }

    void levelChanged() { g_dropRequested = true; }

    void frame(uint8_t *rdram)
    {
        if (!enabled())
            return;
        const auto now = std::chrono::steady_clock::now();
        // (TODO 309: a load begun, or the HQ ended - levelChanged: the strings found are let go, and looked for again 1 s
        // after a level's load ends - 10 s at most, the HQ keeps the loading word set)
        {
            const uint32_t loading = word(rdram, kLoading);
            const bool loadBegan = loading != 0u && g_loadingBefore == 0u;
            g_loadingBefore = loading;
            if (loadBegan || g_dropRequested.exchange(false))
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                ++g_epoch;
                const bool had = !g_entries.empty();
                g_previous.insert(g_previous.end(), std::make_move_iterator(g_entries.begin()), std::make_move_iterator(g_entries.end()));
                g_entries.clear();
                g_searched = false;
                g_shown = false;
                g_shownLayout.clear();
                g_searches = 0;
                g_holdUntil = now + std::chrono::seconds(10); // (the HQ keeps the loading word set: at most 10 s)
                if (had)
                    std::fprintf(stderr, "[key text] a load: the strings found are let go (looked for again after it)\n");
            }
            // (a level: 1 s after its load ends)
            if (loading == 0u && now < g_holdUntil && g_holdUntil - now > std::chrono::seconds(1))
                g_holdUntil = now + std::chrono::seconds(1);
            if (now < g_holdUntil)
                return;
        }
        // (TODO 309: after a load, the strings let go that are still exactly as written - the boot table's, never freed - are
        // taken back with their originals; the others were freed with the load and stay let go)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!g_previous.empty())
            {
                size_t kept = 0;
                for (Entry &p : g_previous)
                {
                    if (!stillHeld(rdram, p))
                        continue;
                    const auto same = std::find_if(g_entries.begin(), g_entries.end(), [&](const Entry &e)
                                                   { return e.object == p.object && e.chars == p.chars; });
                    if (same != g_entries.end())
                        *same = std::move(p);
                    else
                        g_entries.push_back(std::move(p));
                    ++kept;
                }
                std::fprintf(stderr, "[key text] after the load: %zu of %zu strings still the game's, taken back\n", kept, g_previous.size());
                g_previous.clear();
            }
        }
        const bool keyboard = ps2_host_input::keyboardPromptsShown();
        // (Nothing to do until the keyboard is used: the controller's words are the game's own.)
        if (!g_searched && !g_searching.load())
        {
            if (!keyboard || (g_searches > 0 && now - g_lastSearch < std::chrono::seconds(20)) || g_searches >= 12)
                return;
            ++g_searches;
            g_lastSearch = now;
            g_searching = true;
            const unsigned epoch = g_epoch.load();
            std::thread([rdram, epoch] {
                search(rdram, epoch);
                g_searching = false;
            }).detach();
            return;
        }
        if (g_searching.load())
            return;
        bool empty;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            empty = g_entries.empty();
        }
        if (empty)
        {
            // (the table not loaded yet when the keyboard was first used: looked for again a while later)
            if (keyboard && g_searches < 12 && now - g_lastSearch >= std::chrono::seconds(20))
                g_searched = false;
            return;
        }
        const std::string layoutText = mod_menu::playerKeyLayout();
        // On a change of device or layout at once; else checked once a second (a state loaded puts back what it held).
        if (keyboard == g_shown && layoutText == g_shownLayout && now - g_lastCheck < std::chrono::seconds(1))
            return;
        g_lastCheck = now;
        if (!apply(rdram, keyboard, layoutText) && g_searches < 12 && now - g_lastSearch >= std::chrono::seconds(20))
            g_searched = false; // (search again)
        g_shown = keyboard;
        g_shownLayout = layoutText;
    }
}
