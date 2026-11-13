// The PC settings as rows and pages inside the game's own Options menus (pc_settings_row.h has the long form).

#include "pc_settings_row.h"

#include "game_menu_list.h"
#include "guest_code.h"
#include "mod_menu.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_settings.h"
#if defined(PS2X_ENABLE_DEBUG_UI)
// The settings' registry as F1's pages read it (rows, choose): the port's own, not a public header.
#include "../../PS2Recomp/ps2xRuntime/src/lib/ps2_settings_internal.h"
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include "game_version.h" // (TF_GA: the code addresses that differ in the PAL release)

namespace pc_settings_row
{
    namespace
    {
        // ---- The game's side (retail SLUS-20668): taScreenStartMenu, a taScreenInGMenu list ----
        constexpr uint32_t kStartMenuVtable = 0x006C7640u;
        constexpr uint32_t kProcessInput = TF_GA(0x00557540u); // slot 18: (this, krInput, krPlayer), every frame
        constexpr uint32_t kStringLookup = 0x001A9410u; // StringTable_String(result, key, table)
        // The HQ's Options screen (taScreenOptions) and the in-game pause menu (taScreenInGPause), two more taScreenInGMenu lists:
        // vtable, input handler (slot 18) and the function that fills their rows (slot 14 and slot 9).
        constexpr uint32_t kOptionsVtable = 0x006BA050u, kOptionsInput = TF_GA(0x003862A0u), kOptionsInit = TF_GA(0x00386460u);
        constexpr uint32_t kPauseVtable = 0x006B6FE0u, kPauseInput = 0x00311A30u, kPauseInit = 0x00311F30u;
        // The Start menu's Options screen (taScreenStartOptionsMenu, "StartOptions"): Sound Mode, Widescreen, Screen Position.
        constexpr uint32_t kStartOptionsVtable = 0x006C75C0u, kStartOptionsInput = TF_GA(0x005568B0u), kStartOptionsInit = TF_GA(0x00557220u);

        // The list's per-row data (taScreenInGMenu's fields; menu_mouse.cpp has the same offsets). Seven rows at most.
        constexpr uint32_t kBarFlags = 0x560u;    // [7] 1: a volume bar
        constexpr uint32_t kSliderValues = 0x568u; // [7] int: a volume bar's steps, 0 to 10 (the game's stepping clamps them)
        constexpr uint32_t kValueFlags = 0x584u;  // [7] 1: a left / right value row
        constexpr uint32_t kValueTexts = 0x58Cu;  // [7][4] std::string: the value texts (keys)
        constexpr uint32_t kNames = 0x6DCu;       // [7] std::string: a value row's label (key)
        constexpr uint32_t kCurrent = 0x730u;     // [7] the value shown
        constexpr uint32_t kLimits = 0x74Cu;      // [7] how many values
        constexpr uint32_t kCount = 0x7B0u;       // the number of rows
        constexpr uint32_t kTitle = 0x7B4u;       // std::string: the title (key)
        constexpr uint32_t kLabels = 0x7C0u;      // [7] std::string: a plain row's label (key)
        constexpr uint32_t kSelected = 0x814u;    // the selected row
        constexpr uint32_t kSkipped = 0x828u;     // [7] 1: the selection skips the row
        constexpr uint32_t kState = 0x848u;       // the Start menu's state; 9 = the menu itself
        constexpr uint32_t kPending = 0x858u;     // a confirm the menu keeps for itself (5)
        constexpr uint32_t kStringSize = 0x0Cu;
        constexpr uint32_t kMenuState = 9u;
        constexpr uint32_t kUnlocked = 0x006CFA70u; // a byte: 0 keeps Continue and Extras locked, the selection off them
        constexpr uint32_t kRows = 4u;           // New Game, Continue, Options, Extras: QUIT GAME is index 4
        constexpr uint32_t kQuitRow = kRows;     // QUIT GAME
        constexpr uint32_t kMaxRows = 7u, kMaxValues = 4u;
        constexpr uint32_t kInputConfirm = 5u, kInputBack = 6u, kInputRight = 4u; // krInput (1 up, 2 down, 3 left, 4 right)

        // What a page change saves and puts back: the list's per-row data, in three pieces.
        constexpr uint32_t kSave1 = kBarFlags, kSave1Size = 0x768u - 0x560u;  // flags, values, names, limits
        constexpr uint32_t kSave2 = kCount, kSave2Size = 0x818u - 0x7B0u;     // count, title, labels, selected
        constexpr uint32_t kSave3 = kSkipped, kSave3Size = kMaxRows;          // skip flags

        // The lookup: a key the table does not have is answered with the key itself, copied (by the game's own string code,
        // into the game's own heap). Our keys are short ("PC" and eight hex digits: they fit a list slot with no allocation), so
        // for one of them the lookup is handed the label itself as its key: a std::string built in a frame of the guest stack
        // for the length of that one call (lookupHook). Nothing of ours stays in guest memory: no added data block, so the save
        // states' layout key (guest_code::layoutKey) is the same with the pages on and off.
        constexpr size_t kMaxText = 80u;

        // The screens the settings go on, in the game's own Options menus:
        //  - the Start menu's Options screen ("StartOptions": Sound Mode, Widescreen, Screen Position) and the HQ's Options screen
        //    (Audio Settings, Controller Settings) get four more rows each: Display, Graphics, PC Controls, Advanced;
        //  - the pause menu's Options row goes straight to the in-game audio screen, so it opens an OPTIONS list (Audio Settings,
        //    which is the game's own screen, and the same four rows);
        //  - the Start menu itself only gets QUIT GAME after Extras.
        enum Kind { kStartMenu, kStartOptions, kHqOptions, kPauseMenu };
        struct Host
        {
            Kind kind;
            const char *name;
            uint32_t vtable, input, init; // init: 0 = none needed
            const char *firstKey;         // the screen's own first label, a key: how its own rows are told from other uses
            uint32_t baseRows, totalRows; // (the pause menu's rows stay as they are)
            PS2Runtime::RecompiledFunction inputOriginal, initOriginal;
        };
        Host g_hosts[] = {
            {kStartMenu, "Start menu", kStartMenuVtable, kProcessInput, 0u, "KEY_NEW_GAME", kRows, kRows + 1u, nullptr, nullptr},
            {kStartOptions, "Start Options", kStartOptionsVtable, kStartOptionsInput, kStartOptionsInit, "KEY_STARTOPTS_SOUNDMODE", 3u, 3u,
             nullptr, nullptr}, // (no rows added: it shows the hub, kStartHubPage, from the moment it is shown)
            {kHqOptions, "HQ Options", kOptionsVtable, kOptionsInput, kOptionsInit, "KEY_AUDIOSETTINGS", 2u, 6u, nullptr, nullptr},
            {kPauseMenu, "pause menu", kPauseVtable, kPauseInput, kPauseInit, "KEY_RESUME", 4u, 4u, nullptr, nullptr},
        };
        // The Start menu's own fill function (taScreenStartMenu's slot 14: four rows, the count, the title, the selection on
        // New Game) and the word it clears (set by the menu's memory card check when a save was found).
        constexpr uint32_t kStartMenuFill = TF_GA(0x00558DA0u), kSavesFound = 0x85Cu;
        constexpr uint32_t kPauseOptionsRow = 2u; // the pause menu's Options row (Resume, Show Objective, Options, Quit Game)
        const Host *g_host = nullptr; // the screen the pages are on

        PS2Runtime::RecompiledFunction g_lookup = nullptr;
        bool g_on = false; // install() went ahead
        PS2Runtime *g_runtime = nullptr; // (for the quit)

        bool trace()
        {
            static const bool on = std::getenv("TF_PC_SETTINGS_TRACE") && *std::getenv("TF_PC_SETTINGS_TRACE") == '1';
            return on;
        }

        // ---- The pages: what the submenus show and which settings they write ----
        struct Set
        {
            const char *key, *value;
            std::vector<const char *> alts; // other values that count as this one (F1's Medium takes "auto" for the texture quality)
            bool onlyWrite = false;         // written with the choice, but not asked for when the choice is told from the others
        };
        struct Opt
        {
            const char *label;
            std::vector<Set> sets; // the settings this choice writes
        };
        struct Row
        {
            const char *label;
            int link;              // >= 0: a plain row that opens that page
            std::vector<Opt> opts; // else, with no action: a value row (2 to 4 choices)
            int action = 0;        // 1: back (one page up, or out of the pages), 2: quit the game, 3: open the F1 menu, 7: Restart to apply,
                                   // 8: the notice row, the first row of the Start menu's value pages (skipped by the selection; blank, or the
                                   // red REQUIRES RESTART line: the text-only list counts its rows up to its title, so the line needs a row),
                                   // 4: out of the pages and the confirm given to the game's own row (the pause menu's Audio Settings),
                                   // 5: the game's Screen Position mode (the Start Options screen's own), then back to the hub,
                                   // 6: the game's button layout screen (the HQ's Options only)
            bool preset = false;   // the graphics preset: its last choice is "Custom" (no settings), shown once a member row was changed
            bool member = false;   // a setting the preset sets: changing it by hand makes the preset Custom
            int legacy = 0;        // the game's own options, written through the game's setters, not the settings registry: 1 Sound Mode
                                   // (a word option), 2 Invert Y Axis, 3 Vibration, 4 Look Spring, 5 R3 Recon (byte options 1, 0, 2, 3)
            bool widescreen = false; // the aspect row: its 4:3 / wide choice is also the game's widescreen switch (option 4), set live
            const char *key = nullptr; // a setting of the registry: the choices are F1's own (instantiate), whatever F1 offers
            int dyn = 0;               // 2: the vibration (the rumble steps, and the trigger strength and the game's own Vibration option that
                                       // go with each)
            bool custom = false;       // instantiate added a last choice for a value no choice stands for
            bool inPause = true;       // false: left out in the pause menu, whose rows reach the footer at seven
            bool inStart = true;       // false: left out on the Start menu's screens, where the restart notice takes the seventh row's place
            bool slider = false;       // on the lists with bars (the HQ, the pause menu): shown as a row of steps like the game's volume bars,
                                       // the choice in its label; the Start menu's text list shows the choice as a value
            const char *merged = nullptr; // one of F1's merged rows (game_menu_list.h): its choices are F1's own (instantiate)
            const char *omit = nullptr;   // a choice of that merged row left out here (F1's label)
        };
        struct Page
        {
            const char *title;
            std::vector<Row> rows;
            bool leavesScreen = false; // Back from this page leaves the screen the pages are on (the Start Options hub), the game's Back
        };

        Opt opt(const char *label, const char *key, const char *value) { return Opt{label, {{key, value}}}; }
        Row choice(const char *label, std::vector<Opt> opts) { return Row{label, -1, std::move(opts)}; }
        Row link(const char *label, int page) { return Row{label, page, {}}; }
        Row act(const char *label, int action) { return Row{label, -1, {}, action}; }
        Row legacyChoice(const char *label, int legacy, std::vector<Opt> opts)
        {
            Row r = choice(label, std::move(opts));
            r.legacy = legacy;
            return r;
        }
        // A choice of the game's option rows with the settings.ini key that keeps it
        Opt kept(const char *label, const char *key, const char *value) { return Opt{label, {{key, value}}}; }
        constexpr int kQuitPage = 4; // the confirmation the Start menu's QUIT GAME row opens
        constexpr int kStartHubPage = 5, kAudioPage = 6, kStartDisplayPage = 7; // the Start menu's Options: its hub, Audio, Display
        constexpr int kControlsPage = 3, kGamepadPage = 8, kMousePage = 9, kControlsHqPage = 10; // Controls and its sub menus
        constexpr int kSticksPage = 11, kGameplayPage = 12, kOtherPage = 13;                   // the pages after them
        constexpr int kRestartPage = 14; // the prompt shown on leaving a page with a change that waits for a restart

        // The game's own options (the Start Options screen's, read and written through the game's functions): a table of bytes
        // from +0 (4 = the widescreen switch) and words from +8 (1 = the sound mode: 1 Mono, 2 Stereo, 3 Surround).
        constexpr uint32_t kOptionsPtr = 0x006CF244u;
        constexpr uint32_t kSetByteOption = 0x00278010u; // (index, value): sets and applies (4 also copies it for the renderer)
        constexpr uint32_t kVibrate = 0x00316630u;       // taVibrationManager::Vibrate(player, action): the game's own buzz (8: the test one)
        constexpr uint32_t kSetWordOption = 0x00277F10u; // (index, value)
        constexpr uint32_t kApplyWordOption = 0x00277EA0u; // (index)
        constexpr uint32_t kOptionsFn1 = 0x0032E800u, kOptionsFn2 = 0x0032E770u, kOptionsFn3 = 0x00276E70u; // lays the screen out again
        // The Start Options screen's own fields: the widescreen switch and the sound mode as they were when it opened (its Back
        // puts them back), the Screen Position mode flag.
        constexpr uint32_t kOpenWidescreen = 0x848u, kOpenSoundMode = 0x85Cu, kPositionMode = 0x850u;

        // The rows added to the Start Options and the HQ's Options screens (after the game's own): a page, or -1 for the F1 menu.
        struct Extra
        {
            const char *label;
            int page;
        };
        constexpr Extra kExtras[] = {{"Display", 1}, {"Graphics", 2}, {"Controls", 10}, {"Gameplay", 12}, {"More settings", -1}};
        constexpr uint32_t kExtraCount = 5u;

        // The pages follow F1's default list (Display, Graphics, Controls, Sound, Gameplay, Other): the rows are F1's settings, the
        // same keys and the same choices. A screen holds seven rows at most, and a value row's own stepping four texts: longer
        // lists are handled in buildPage / scanChanges (the same text in all four, the stepping counted here).

        // ---- The pages' rows come from the shared list (game_menu_list.h): the same rows F1 has, in the game's menus ----
        // The row of one entry: an F1 setting (its choices are F1's own, filled in by instantiate), one of F1's merged rows (preset,
        // anti aliasing, aspect ratio, volume, button prompts: their choices and the settings each writes are F1's too) or one of the
        // game's own options (kept in settings.ini, read and written through the game's setters).
        Row rowFor(const game_menu_list::Entry &e)
        {
            const std::string id = e.id;
            Row r{e.label, -1, {}};
            if (id == "aspect" || id == "preset" || id == "antialiasing" || id == "volume" || id == "prompts")
            {
                r.merged = e.id;
                r.omit = e.omit;
                r.preset = id == "preset";
                r.widescreen = id == "aspect"; // (its 4:3 / wide choice is also the game's widescreen switch, set live)
            }
            else if (id == "TF_OPT_SOUND_MODE")
                r = legacyChoice(e.label, 1, {kept("Mono", "TF_OPT_SOUND_MODE", "mono"), kept("Stereo", "TF_OPT_SOUND_MODE", "stereo"),
                                              kept("Surround", "TF_OPT_SOUND_MODE", "surround")});
            else if (id == "TF_OPT_INVERT_Y")
                r = legacyChoice(e.label, 2, {kept("No", "TF_OPT_INVERT_Y", "no"), kept("Yes", "TF_OPT_INVERT_Y", "yes")});
            else if (id == "TF_OPT_LOOK_SPRING")
                r = legacyChoice(e.label, 4, {kept("On", "TF_OPT_LOOK_SPRING", "on"), kept("Off", "TF_OPT_LOOK_SPRING", "off")});
            else if (id == "TF_OPT_R3_RECON")
                r = legacyChoice(e.label, 5, {kept("On", "TF_OPT_R3_RECON", "on"), kept("Off", "TF_OPT_R3_RECON", "off")});
            else
            {
                r.key = e.id;
                // (TODO 336 (22)) With the native renderer the game is drawn at the window's size and the Resolution row sets only
                // the menus and movies: the row's name says so (F1 says it under its row).
                if (id == "PS2X_GPU_SCALE" && ps2_settings::value("TF_RENDERER") == "native")
                    r.label = "Menu Resolution";
                // F1's Vibration (the rumble's strength, Off included) is also the DualSense's trigger strength: the haptic triggers
                // are on by default and are turned off in F1 only. Another pad has no triggers to push back: it only rumbles.
                if (id == "PS2X_SONYPAD_RUMBLE")
                    r.dyn = 2;
            }
            r.inPause = !(e.flags & game_menu_list::kNotInPause);
            r.inStart = !(e.flags & game_menu_list::kNotInStart);
            r.slider = (e.flags & game_menu_list::kSlider) != 0;
            r.member = (e.flags & game_menu_list::kPresetMember) != 0;
            return r;
        }

        std::vector<Row> rowsOf(game_menu_list::Page page)
        {
            std::vector<Row> rows;
            for (const game_menu_list::Entry &e : game_menu_list::kEntries)
                if (e.page == page)
                    rows.push_back(rowFor(e));
            return rows;
        }

        // (The values and defaults are F1's: docs/PC_SETTINGS.md has the table.)
        const std::vector<Page> &pages()
        {
            static const std::vector<Page> list = {
                // The pause menu's OPTIONS list (the HQ's Options list has these rows in its own list: kExtras).
                {"OPTIONS",
                 {act("Audio Settings", 4), // (the game's own in-game audio screen: the pause menu's Options row, given back to it)
                  link("Display", 1), link("Graphics", 2), link("Controls", kControlsPage), link("Gameplay", kGameplayPage),
                  act("More settings", 3)}}, // (the F1 menu, for everything the basic pages leave out)
                {"DISPLAY", rowsOf(game_menu_list::Page::Display)},
                // (Preset is F1's: Low, Medium, High; Ultra stays in F1. Its last choice is Custom: it shows once a row below was changed
                // by hand.)
                {"GRAPHICS", rowsOf(game_menu_list::Page::Graphics)},
                // Controls: ONE entry, with sub menus. The game's own Controller Settings screen is folded in: its on / off options
                // are rows of the Gamepad page (the game's own table, written through the game's setter), beside the PC ones.
                {"CONTROLS", {link("Gamepad", kGamepadPage), link("Sticks", kSticksPage), link("Mouse", kMousePage)}},
                // (the page index 3 is kControlsPage: the hub the Start menu's and the pause menu's lists open)
                {"QUIT GAME?", {act("No", 1), act("Yes", 2)}}, // (kQuitPage: "No" is the first row, the one selected)
                // The Start menu's Options: the game's own Start Options screen, shown as these pages. Its rows went here: Widescreen is
                // the Aspect Ratio row, Sound Mode the Audio page's row, Screen Position a row of Other. Back leaves the screen.
                {"OPTIONS",
                 {link("Display", kStartDisplayPage), link("Graphics", 2), link("Audio", kAudioPage), link("Controls", kControlsPage),
                  link("Gameplay", kGameplayPage), link("Other", kOtherPage), act("More settings", 3)},
                 true},
                {"AUDIO", rowsOf(game_menu_list::Page::Audio)},
                {"DISPLAY", rowsOf(game_menu_list::Page::Display)},
                {"GAMEPAD", rowsOf(game_menu_list::Page::Gamepad)},
                {"MOUSE", rowsOf(game_menu_list::Page::Mouse)},
                // The HQ's Options list opens this one: the same, and the game's button layout screen (its Controller Setup row).
                {"CONTROLS", {link("Gamepad", kGamepadPage), link("Sticks", kSticksPage), link("Mouse", kMousePage), act("Button Layout", 6)}},
                {"STICKS", rowsOf(game_menu_list::Page::Sticks)},
                {"GAMEPLAY", rowsOf(game_menu_list::Page::Gameplay)},
                {"OTHER", []() { std::vector<Row> rows = rowsOf(game_menu_list::Page::Other);
                                 rows.push_back(act("Screen Position", 5)); // (the game's own: the picture's place)
                                 return rows; }()},
                // F1's words: a change that only applies at the next start is tagged RESTART, and its button says Restart to apply. The
                // prompt is a page like the QUIT GAME one: Later (the first, selected) goes on, Restart to apply starts F1's own restart.
                {"RESTART REQUIRED", {act("Later", 1), act("Restart to apply", 7)}},
            };
            return list;
        }

        // ---- Texts: every label is a short key the lookup hook turns into its text ----
        // The key is "PC" and the text's hash (FNV-1a, eight hex digits: ten characters, a list slot's inline room). It depends
        // on the text alone, not on the order the pages were opened in: a key a save state holds in a screen's rows (the QUIT GAME
        // row, a page left open) means the same text in the session that loads it. install() registers every text the pages have.
        std::map<uint32_t, std::string> g_texts;

        uint32_t textHash(const std::string &text)
        {
            uint32_t h = 2166136261u;
            for (const char c : text)
                h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
            return h;
        }

        std::string keyFor(const std::string &text)
        {
            const std::string kept = text.substr(0, kMaxText);
            const uint32_t h = textHash(kept);
            const auto [it, added] = g_texts.emplace(h, kept);
            if (!added && it->second != kept)
                std::fprintf(stderr, "[pc settings] the labels \"%s\" and \"%s\" have the same key: the second shows the first\n",
                             it->second.c_str(), kept.c_str());
            char key[16];
            std::snprintf(key, sizeof(key), "PC%08X", h);
            return key;
        }

        // ---- Guest memory ----
        bool inRam(uint32_t address) { return address >= 0x00100000u && address + 0x1000u < PS2_RAM_SIZE; }

        uint32_t read32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t v = 0u;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&v, p, sizeof(v));
            return v;
        }

        void write32(uint8_t *rdram, uint32_t address, uint32_t value)
        {
            if (uint8_t *p = getMemPtr(rdram, address))
                std::memcpy(p, &value, sizeof(value));
        }

        // The game's std::string (CodeWarrior's): byte 0's low bit set = the characters elsewhere (their address at +8, the
        // size at +4); clear = held in the string from +1, at most 10 characters.
        uint32_t charsOf(const uint8_t *rdram, uint32_t string)
        {
            const uint8_t *flag = getConstMemPtr(rdram, string);
            if (!flag)
                return 0u;
            return (*flag & 1u) ? read32(rdram, string + 8u) : string + 1u;
        }

        // A short key written into a string slot of the list (no allocation: nothing of the slot's old content is freed -
        // a page change keeps those in its save).
        void putKey(uint8_t *rdram, uint32_t slot, const std::string &key)
        {
            uint8_t *p = getMemPtr(rdram, slot);
            if (!p || key.size() > 10u)
                return;
            std::memset(p, 0, kStringSize);
            p[0] = static_cast<uint8_t>(key.size() << 1);
            std::memcpy(p + 1, key.c_str(), key.size() + 1u);
        }

        bool textIs(const uint8_t *rdram, uint32_t string, const char *text, size_t length)
        {
            const uint32_t chars = charsOf(rdram, string);
            if (!inRam(chars))
                return false;
            const uint8_t *p = getConstMemPtr(rdram, chars);
            return p && std::memcmp(p, text, length + 1u) == 0;
        }

        // The text of one of our keys ("PC" and eight hex digits), or null.
        const std::string *ourText(const uint8_t *rdram, uint32_t key)
        {
            const uint32_t chars = charsOf(rdram, key);
            const uint8_t *p = inRam(chars) ? getConstMemPtr(rdram, chars) : nullptr;
            if (!p || p[0] != 'P' || p[1] != 'C')
                return nullptr;
            uint32_t h = 0u;
            for (int i = 2; i < 10; ++i)
            {
                const uint8_t c = p[i];
                if (c >= '0' && c <= '9')
                    h = (h << 4) | static_cast<uint32_t>(c - '0');
                else if (c >= 'A' && c <= 'F')
                    h = (h << 4) | static_cast<uint32_t>(c - 'A' + 10);
                else
                    return nullptr;
            }
            if (p[10] != 0)
                return nullptr;
            const auto it = g_texts.find(h);
            return it != g_texts.end() ? &it->second : nullptr;
        }

        // ---- The settings the pages write: the game thread asks, the window thread does it (the settings' apply
        //      functions are the window thread's) ----
        std::mutex g_queueLock;
        std::vector<std::pair<std::string, std::string>> g_queue;

        // ---- The registry as the pages need it: each setting's label and choices, copied by the window thread (pump) because the
        //      registry's rows are made there (some choices come from the window: the monitors, the resolutions). The game thread
        //      builds its pages from the copy, so the pages offer what F1 offers, whatever F1 adds. ----
        struct Info
        {
            std::string label;
            std::vector<std::pair<std::string, std::string>> choices; // (value, label)
            bool available = true;
            bool pending = false; // chosen, and waiting for the next start (F1 tags such a row RESTART)
        };
        std::mutex g_infoLock;
        std::map<std::string, Info> g_info;
        // F1's merged rows (preset, anti aliasing, aspect ratio, volume, button prompts), copied the same way: the choices and the
        // settings each writes are F1's own, never a copy kept here.
        std::map<std::string, std::vector<mod_menu::MergedOption>> g_merged;
        constexpr const char *kMergedIds[] = {"aspect", "preset", "antialiasing", "volume", "prompts"};

        // The pages are up: the registry is copied oftener (the red restart notice follows what the registry says). The game thread sets it.
        std::atomic<bool> g_pagesUp{false};
        // The prompt's Restart to apply: the game thread asks, the window thread starts F1's own restart (pump).
        std::atomic<bool> g_restartAsked{false};

        bool isPending(const std::string &key)
        {
            std::lock_guard<std::mutex> lock(g_infoLock);
            const auto it = g_info.find(key);
            return it != g_info.end() && it->second.pending;
        }

        bool infoFor(const std::string &key, Info &out)
        {
            std::lock_guard<std::mutex> lock(g_infoLock);
            const auto it = g_info.find(key);
            if (it == g_info.end())
                return false;
            out = it->second;
            return true;
        }

        bool mergedFor(const std::string &id, std::vector<mod_menu::MergedOption> &out)
        {
            std::lock_guard<std::mutex> lock(g_infoLock);
            const auto it = g_merged.find(id);
            if (it == g_merged.end() || it->second.empty())
                return false;
            out = it->second;
            return true;
        }

        // What the pages chose, with what the registry held at that moment. A setting that only takes effect at the next start
        // (HD textures, sharp text) or whose live value is the device's (rumble) reads back its OLD value, and its row would show
        // that - as if the choice had not stuck. While the registry still holds what it held then, the row shows the choice.
        struct Chosen
        {
            std::string value, registryThen;
        };
        std::map<std::string, Chosen> g_chosen; // (the game thread only)

        void queueOpt(const Opt &o)
        {
            std::lock_guard<std::mutex> lock(g_queueLock);
            for (const Set &s : o.sets)
            {
                g_queue.emplace_back(s.key, s.value);
                g_chosen[s.key] = Chosen{s.value, ps2_settings::value(s.key)};
            }
        }

        // Does the setting hold what the choice writes? With an overlay (a preset about to be applied) the overlay's values count
        // for the keys it has: the rows are shown as they will be, before the window thread has written them.
        bool setHolds(const Set &s, const Opt *overlay)
        {
            std::string now = ps2_settings::value(s.key);
            if (const auto it = g_chosen.find(s.key); it != g_chosen.end() && now == it->second.registryThen)
                now = it->second.value;
            if (overlay)
                for (const Set &o : overlay->sets)
                    if (std::strcmp(o.key, s.key) == 0)
                        now = o.value;
            if (now == s.value)
                return true;
            for (const char *alt : s.alts)
                if (now == alt)
                    return true;
            return false;
        }

        // The choice a row shows. The preset row has Custom (no settings) last: it is what is left when no preset holds.
        int currentIndex(const Row &r, const Opt *overlay = nullptr)
        {
            for (size_t k = 0; k < r.opts.size(); ++k)
            {
                bool all = true;
                for (const Set &s : r.opts[k].sets)
                    if (!s.onlyWrite && !setHolds(s, overlay))
                        all = false;
                if (all)
                    return static_cast<int>(k);
            }
            return -1; // (a value no choice stands for: instantiate gives such a row a last choice, Custom)
        }

        // ---- The rows of the registry's settings: their choices, as F1 has them ----
        // A string that lives as long as the program (a Set's key and value are pointers).
        const char *intern(const std::string &s)
        {
            static std::set<std::string> pool;
            return pool.insert(s).first->c_str();
        }

        // The game's font has no "x" sign of F1's labels ("2x") in UTF-8 and the value column holds about 16 characters.
        std::string ascii(const std::string &s)
        {
            std::string out;
            for (size_t i = 0; i < s.size(); ++i)
            {
                const unsigned char c = static_cast<unsigned char>(s[i]);
                if (c == 0xC3u && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x97u)
                {
                    out += 'x';
                    ++i;
                }
                else if (c < 0x80u)
                    out += static_cast<char>(c);
            }
            return out;
        }

        std::string shortLabel(const std::string &key, const std::string &value, const std::string &label)
        {
            static const struct
            {
                const char *key, *value, *label;
            } kNames[] = {{"PS2X_FULLSCREEN", "1", "Borderless"},   {"PS2X_FULLSCREEN", "2", "Exclusive"},   {"PS2X_GPU_SCALE", "auto", "Auto"},
                          {"PS2X_STICK_RESPONSE", "off", "Off"},    {"PS2X_STICK_VEHICLE_MODE", "original", "Original"},
                          {"TF_DRAW_DISTANCE", "1", "1x (console)"}, {"TF_START_SCREENS", "menu", "Main menu"},
                          {"TF_START_SCREENS", "title", "Title screen"}, {"TF_START_SCREENS", "all", "Everything"}};
            for (const auto &n : kNames)
                if (key == n.key && value == n.value)
                    return n.label;
            std::string s = ascii(label);
            if (s.size() > 16u)
            {
                const size_t paren = s.find(" (");
                if (paren != std::string::npos && paren > 0u)
                    s.resize(paren);
            }
            if (s.size() > 16u)
                s.resize(16u);
            return s;
        }

        // The trigger strength that goes with a rumble strength (F1 has the strengths as 100, 75, 50, 25 and 0 = Off): the
        // DualSense's triggers push back as hard as the pad rumbles. Off leaves the triggers as they are (turned off in F1 only).
        const char *triggerStrengthFor(const std::string &rumble)
        {
            if (rumble == "100")
                return "strong";
            if (rumble == "75" || rumble == "50")
                return "medium";
            if (rumble == "25")
                return "light";
            return nullptr;
        }

        // F1's anti-aliasing choices are Off, Low, Medium and High. Here each is named by its technique, told from the settings the choice
        // writes (the edge filter, TF_NATIVE_AA: 1 FXAA, 2 SMAA; the multisampling; the supersampling), so the name is always what the
        // choice does, whatever F1 sets it to. A list that does not write those settings (the emulated renderer's) keeps F1's labels.
        std::string antiAliasingName(const mod_menu::MergedOption &m)
        {
            bool native = false;
            std::string filter, msaa, ssaa;
            for (const mod_menu::MergedPair &p : m.pairs)
            {
                if (p.key == "TF_NATIVE_AA")
                {
                    native = true;
                    filter = p.value;
                }
                else if (p.key == "PS2X_GPU_MSAA")
                    msaa = p.value;
                else if (p.key == "TF_NATIVE_SSAA")
                    ssaa = p.value;
            }
            if (!native)
                return m.label;
            std::string name = filter == "1" ? "FXAA" : filter == "2" ? "SMAA" : "";
            if (!msaa.empty() && msaa != "0")
                name += std::string(name.empty() ? "" : " ") + "MSAA " + msaa + "x";
            if (ssaa == "1")
                name += std::string(name.empty() ? "" : " ") + "SSAA";
            return name.empty() ? m.label : name; // (Off)
        }

        // A page of the table, with its registry rows filled in from F1's own settings (a setting this build does not have, or one
        // F1 greys out, leaves its row out) and, for a row whose value no choice stands for (a hand-written settings.ini, F1's
        // other steps), a last choice that shows it: Custom, with the value.
        constexpr int kNoticeAction = 8;
        constexpr char kNoticeText[] = "REQUIRES RESTART";

        bool isNoticeRow(const Row &row) { return row.action == kNoticeAction; }

        // The text of the notice row: the red line while a change on the page waits for a restart, else a space (draws nothing).
        std::string noticeText(bool on) { return on ? std::string(kNoticeText) : std::string(" "); }

        Page instantiate(const Page &spec)
        {
            Page out;
            out.title = spec.title;
            out.leavesScreen = spec.leavesScreen;
            for (const Row &row : spec.rows)
            {
                Row r = row;
                if (!r.inPause && g_host && g_host->kind == kPauseMenu)
                    continue;
                if (!r.inStart && g_host && (g_host->kind == kStartMenu || g_host->kind == kStartOptions))
                    continue;
                if (r.merged)
                {
                    // One of F1's merged rows: its choices and the settings each one writes are F1's own (copied by the window thread).
                    std::vector<mod_menu::MergedOption> options;
                    if (!mergedFor(r.merged, options))
                    {
                        if (trace())
                            std::fprintf(stderr, "[pc settings] %s: F1 has no such row in this build, the row is left out\n", r.label);
                        continue;
                    }
                    for (const mod_menu::MergedOption &m : options)
                    {
                        if (r.omit && m.label == r.omit)
                            continue;
                        const std::string name = std::strcmp(r.merged, "antialiasing") == 0 ? antiAliasingName(m) : m.label;
                        Opt o{intern(shortLabel("", "", name)), {}};
                        for (const mod_menu::MergedPair &p : m.pairs)
                        {
                            Set s{intern(p.key), intern(p.value)};
                            for (const std::string &also : p.accepted)
                                s.alts.push_back(intern(also));
                            o.sets.push_back(std::move(s));
                        }
                        r.opts.push_back(std::move(o));
                    }
                    if (r.preset)
                        r.opts.push_back(Opt{"Custom", {}}); // (no settings: what is left when no preset holds)
                }
                if (r.key)
                {
                    Info info;
                    if (!infoFor(r.key, info))
                    {
                        if (trace())
                            std::fprintf(stderr, "[pc settings] %s: not a setting in this build, the row is left out\n", r.label);
                        continue;
                    }
                    if (r.dyn == 0 && !info.available)
                    {
                        if (trace())
                            std::fprintf(stderr, "[pc settings] %s: greyed out in F1, the row is left out\n", r.label);
                        continue;
                    }
                    for (const auto &[value, label] : info.choices)
                    {
                        const char *text = intern(shortLabel(r.key, value, label));
                        Opt o{text, {{intern(r.key), intern(value)}}};
                        if (r.dyn == 2)
                        {
                            if (const char *strength = triggerStrengthFor(value))
                                o.sets.push_back(Set{"TF_TRIGGER_STRENGTH", strength, {}, true});
                            // (the game's own Vibration on / off goes with the strength: Off turns it off, any strength turns it on,
                            // so a save that has it off cannot leave the slider without effect)
                            o.sets.push_back(Set{"TF_OPT_VIBRATION", value == "0" ? "off" : "on", {}, true});
                        }
                        r.opts.push_back(std::move(o));
                    }
                    // (F1 lists the vibration from Full down to Off: here the stronger choices are to the right, as on a slider)
                    if (r.dyn == 2)
                        std::reverse(r.opts.begin(), r.opts.end());
                }
                if (!r.opts.empty() && r.legacy == 0 && !r.widescreen && !r.preset && currentIndex(r) < 0)
                {
                    std::string text = "Custom";
                    if (r.key && r.dyn == 0)
                    {
                        std::string value = ps2_settings::value(r.key);
                        if (const auto it = g_chosen.find(r.key); it != g_chosen.end() && value == it->second.registryThen)
                            value = it->second.value;
                        text = ascii("Custom (" + value + ")");
                        if (text.size() > 16u)
                            text.resize(16u);
                    }
                    r.opts.push_back(Opt{intern(text), {}});
                    r.custom = true;
                }
                out.rows.push_back(std::move(r));
            }
            // (the Start menu's value pages: the notice row first, if the page has a row to spare)
            if (g_host && (g_host->kind == kStartMenu || g_host->kind == kStartOptions) && out.rows.size() < kMaxRows)
            {
                bool values = false;
                for (const Row &r : out.rows)
                    values = values || (r.link < 0 && r.action == 0 && !r.opts.empty());
                if (values)
                    out.rows.insert(out.rows.begin(), Row{"", -1, {}, kNoticeAction});
            }
            return out;
        }

        // ---- The game's own options (Sound Mode, the widescreen switch): read from its table, written through its functions ----
        // A guest function called from a hook: the registers are kept as they were (the hooked function's result is in v0).
        // *returned (when asked for): the function ran to its end. A callee the scheduler stops half way returns to the
        // scheduler, not here: its frames are dropped with the saved registers put back, and it is not resumed (abandoned).
        uint32_t callGuest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t address, uint32_t a0, uint32_t a1,
                           uint32_t a2 = 0u, bool *returned = nullptr)
        {
            if (returned)
                *returned = false;
            if (!runtime || !runtime->hasFunction(address))
                return 0u;
            const PS2Runtime::RecompiledFunction fn = runtime->lookupFunction(address);
            if (!fn)
                return 0u;
            const R5900Context saved = *ctx;
            SET_GPR_U32(ctx, 4, a0);
            SET_GPR_U32(ctx, 5, a1);
            SET_GPR_U32(ctx, 6, a2);
            ctx->pc = address;
            fn(rdram, ctx, runtime);
            if (returned)
                *returned = ctx->pc == getRegU32(&saved, 31);
            const uint32_t result = getRegU32(ctx, 2);
            *ctx = saved;
            return result;
        }

        // The game's menu sounds (its one-shot tone player, 0x27B460: manager, tone, bank, 0, 0.0): the game plays them from its own rows'
        // code, so a page's Cross and Back are given theirs here. The in-game bank (the pause menu) has 0x209 for a confirm and 0x205 for
        // a back, the front end's (the Start menu, the HQ) 0x218 and 0x202.
        constexpr uint32_t kPlayTone = 0x0027B460u, kToneManager = 0x006CF258u;
        void playTone(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, bool confirm)
        {
            if (!runtime || !runtime->hasFunction(kPlayTone))
                return;
            const PS2Runtime::RecompiledFunction fn = runtime->lookupFunction(kPlayTone);
            const uint32_t manager = read32(rdram, kToneManager);
            if (!fn || !inRam(manager))
                return;
            const bool inGame = g_host && g_host->kind == kPauseMenu;
            const uint32_t tone = inGame ? (confirm ? 0x209u : 0x205u) : (confirm ? 0x218u : 0x202u);
            if (trace())
                std::fprintf(stderr, "[pc settings] menu tone %03x (bank %u)\n", tone, inGame ? 1u : 0u);
            const R5900Context saved = *ctx;
            SET_GPR_U32(ctx, 4, manager);
            SET_GPR_U32(ctx, 5, tone);
            SET_GPR_U32(ctx, 6, inGame ? 1u : 0u);
            SET_GPR_U32(ctx, 7, 0u);
            ctx->f[12] = 0.0f;
            ctx->pc = kPlayTone;
            fn(rdram, ctx, runtime);
            *ctx = saved;
        }

        // The world music is paused under the pause menu. The game's audio screen (taScreenAudioOptions, in game) lets it play while it
        // is shown - PauseMusic(false, 1, -1) in its show function, PauseMusic(true, 1, -1) when it leaves - so the volumes can be
        // judged; the pages of the pause menu do the same (setMusic).
        constexpr uint32_t kPauseMusic = 0x002E6790u, kMusicManager = 0x006CF4A0u;
        bool g_musicOn = false; // the pages have let the music play
        void setMusic(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, bool on)
        {
            if (g_musicOn == on)
                return;
            g_musicOn = on;
            if (trace())
                std::fprintf(stderr, "[pc settings] the pause menu's pages: the music %s\n", on ? "plays" : "is paused again");
            if (!runtime || !runtime->hasFunction(kPauseMusic))
                return;
            const PS2Runtime::RecompiledFunction fn = runtime->lookupFunction(kPauseMusic);
            const uint32_t manager = read32(rdram, kMusicManager);
            if (!fn || !inRam(manager))
                return;
            const R5900Context saved = *ctx;
            SET_GPR_U32(ctx, 4, manager);
            SET_GPR_U32(ctx, 5, on ? 0u : 1u); // (the pause flag: false lets it play)
            SET_GPR_U32(ctx, 6, 1u);
            SET_GPR_U32(ctx, 7, 0xFFFFFFFFu);
            ctx->pc = kPauseMusic;
            fn(rdram, ctx, runtime);
            *ctx = saved;
        }

        // The pause menu's footer says "X Select" only; its pages have a Back as the HQ's list has ("X Select  T Back"). The footer is the
        // screen's help-text item ("HelpText", taItemHelpText): its text is set by a key, with a fade (FadeToNewHelpText, 0x535700), the way
        // the game's Extras screen changes its own. The pages put the HQ list's key (KEY_HELPTEXT_OPTIONSMENU) there and the pause menu's
        // own (KEY_HELPTEXT_INGAMEPAUSE) back when they close.
        constexpr uint32_t kGetItem = 0x001A0430u, kHelpItem = TF_GA(0x0059B600u), kFadeHelpText = TF_GA(0x00535700u);
        bool g_footerBack = false; // the footer says Back
        void setPauseFooter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t screen, bool back)
        {
            if (g_footerBack == back)
                return;
            g_footerBack = back;
            if (!runtime || !runtime->hasFunction(kGetItem) || !runtime->hasFunction(kHelpItem) || !runtime->hasFunction(kFadeHelpText))
                return;
            const uint32_t frame = getRegU32(ctx, 29) - 160u;
            uint8_t *p = inRam(frame) ? getMemPtr(rdram, frame) : nullptr;
            if (!p)
                return;
            const R5900Context saved = *ctx;
            SET_GPR_U32(ctx, 29, frame);
            std::memset(p, 0, 160);
            std::memcpy(p, "HelpText", 9);
            const char *key = back ? "KEY_HELPTEXT_OPTIONSMENU" : "KEY_HELPTEXT_INGAMEPAUSE";
            const uint32_t length = static_cast<uint32_t>(std::strlen(key));
            std::memcpy(p + 64, key, length + 1u);
            // (the key: the string's long form, the characters elsewhere; the second string is empty, the short form: all zero)
            write32(rdram, frame + 16u, (length << 1) | 1u);
            write32(rdram, frame + 20u, length);
            write32(rdram, frame + 24u, frame + 64u);
            const uint32_t item = callGuest(rdram, ctx, runtime, kGetItem, screen, frame);
            if (inRam(item))
            {
                const uint32_t help = callGuest(rdram, ctx, runtime, kHelpItem, item, 0u);
                if (inRam(help))
                {
                    callGuest(rdram, ctx, runtime, kFadeHelpText, help, frame + 16u, frame + 32u);
                    if (trace())
                        std::fprintf(stderr, "[pc settings] the pause menu's footer: %s\n", key);
                }
            }
            *ctx = saved;
        }

        // A guest function that takes a const std::string& as its second argument (a screen's name), called from a hook. The
        // string is a temporary in a frame of the guest stack, as the game's own callers have it (std::string("StartMenu")): the
        // stack pointer is moved under it for the call, so the callee's frames lie below. The registers, the stack pointer
        // among them, are put back as callGuest does. (No added data block: see kMaxText.)
        uint32_t callGuestNamed(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t address, uint32_t a0, const char *name,
                                uint32_t a2)
        {
            const uint32_t length = static_cast<uint32_t>(std::strlen(name));
            const uint32_t frame = (16u + length + 1u + 15u) & ~15u;
            const uint32_t sp = getRegU32(ctx, 29) - frame;
            uint8_t *p = inRam(sp) ? getMemPtr(rdram, sp) : nullptr;
            if (!runtime || !runtime->hasFunction(address) || !p)
                return 0u;
            const PS2Runtime::RecompiledFunction fn = runtime->lookupFunction(address);
            if (!fn)
                return 0u;
            write32(rdram, sp, (length << 1) | 1u); // (CodeWarrior's long form: capacity << 1 | 1, the size, the characters' address)
            write32(rdram, sp + 4u, length);
            write32(rdram, sp + 8u, sp + 16u);
            write32(rdram, sp + 12u, 0u);
            std::memcpy(p + 16, name, length + 1u);
            const R5900Context saved = *ctx;
            SET_GPR_U32(ctx, 29, sp);
            SET_GPR_U32(ctx, 4, a0);
            SET_GPR_U32(ctx, 5, sp);
            SET_GPR_U32(ctx, 6, a2);
            ctx->pc = address;
            fn(rdram, ctx, runtime);
            const uint32_t result = getRegU32(ctx, 2);
            *ctx = saved;
            return result;
        }

        bool gameWidescreen(const uint8_t *rdram)
        {
            const uint32_t table = read32(rdram, kOptionsPtr);
            const uint8_t *p = inRam(table) ? getConstMemPtr(rdram, table + 4u) : nullptr;
            return p && *p != 0u;
        }

        int gameSoundMode(const uint8_t *rdram) // 1 Mono, 2 Stereo, 3 Surround
        {
            const uint32_t table = read32(rdram, kOptionsPtr);
            return inRam(table) ? static_cast<int>(read32(rdram, table + 8u + 4u)) : 0;
        }

        // The game's Controller Settings options are byte options: 1 Invert Y Axis, 0 Vibration, 2 Look Spring, 3 R3 Recon. As that
        // screen has them, the first choice (On for the options, No for Invert Y) is the byte set, the second the byte clear.
        int byteOptionOf(int legacy) { return legacy == 2 ? 1 : legacy == 3 ? 0 : legacy == 4 ? 2 : 3; }

        // The port flips the answer of the Invert Y option for the cameras (overrides.cpp, TODO 178): the byte stored is the
        // label's own - 0 is No (the default, stick up looks up), 1 is Yes - so for Invert Y the first choice, No, is the byte
        // CLEAR. TF_INVERT_FLIP=0 restores the game's meaning (No is the byte set, as for the other options).
        bool firstIsByteSet(uint32_t option)
        {
            static const bool flipped = !(std::getenv("TF_INVERT_FLIP") && *std::getenv("TF_INVERT_FLIP") == '0');
            return !(option == 1u && flipped);
        }
        // The byte that stands for a choice (0 the first, 1 the second).
        uint32_t byteFor(uint32_t option, uint32_t choice) { return (choice == 0u) == firstIsByteSet(option) ? 1u : 0u; }

        int gameByteChoice(const uint8_t *rdram, int legacy)
        {
            const uint32_t table = read32(rdram, kOptionsPtr);
            const uint32_t option = static_cast<uint32_t>(byteOptionOf(legacy));
            const uint8_t *p = inRam(table) ? getConstMemPtr(rdram, table + option) : nullptr;
            const bool set = p && *p != 0u;
            return set == firstIsByteSet(option) ? 0 : 1;
        }

        // The game's options changed here are kept as the port's own settings (settings.ini), which are applied again at the start
        // and after a profile is loaded (its options are copied over the current ones): else they are only in the game's save, put
        // on the card by the game's own save screens, and a restart before one forgets them. "keep" leaves the profile's value.
        struct StoredOption
        {
            const char *key;
            int legacy;         // the row's number (2 Invert Y, 3 Vibration, 4 Look Spring, 5 R3 Recon)
            uint32_t option;    // the game's byte option
            const char *first;  // the first choice's word (the byte set; for Invert Y the byte clear: byteFor)
            const char *second; // the second's
            const char *label;
        };
        constexpr StoredOption kStored[] = {{"TF_OPT_INVERT_Y", 2, 1u, "no", "yes", "Invert Y axis"},
                                            {"TF_OPT_VIBRATION", 3, 0u, "on", "off", "Vibration"},
                                            {"TF_OPT_LOOK_SPRING", 4, 2u, "on", "off", "Look spring"},
                                            {"TF_OPT_R3_RECON", 5, 3u, "on", "off", "Recon on the R3 button"}};
        const StoredOption *storedFor(int legacy)
        {
            for (const StoredOption &o : kStored)
                if (o.legacy == legacy)
                    return &o;
            return nullptr;
        }

        // The aspect row as the game has it: 4:3 when its widescreen switch is off, else the wide shape the settings name.
        int aspectIndex(const uint8_t *rdram, const Row &r)
        {
            if (!gameWidescreen(rdram))
                return 0;
            for (size_t k = 1; k < r.opts.size(); ++k)
                for (const Set &s : r.opts[k].sets)
                    if (std::strcmp(s.key, "TF_ASPECT") == 0 && setHolds(s, nullptr))
                        return static_cast<int>(k);
            return 1;
        }

        // The choice a row shows: the game's own value for its two rows, else the settings'.
        int liveIndex(const uint8_t *rdram, const Row &row)
        {
            if (row.legacy == 1)
            {
                const int mode = gameSoundMode(rdram);
                return mode >= 1 && mode <= 3 ? mode - 1 : 0;
            }
            if (row.legacy >= 2)
                return gameByteChoice(rdram, row.legacy);
            if (row.dyn == 2 && gameByteChoice(rdram, 3) == 1)
            {
                // The vibration row while the game's own Vibration is off: it shows Off (the choice that writes a strength of 0).
                for (size_t k = 0; k < row.opts.size(); ++k)
                    for (const Set &s : row.opts[k].sets)
                        if (std::strcmp(s.key, "PS2X_SONYPAD_RUMBLE") == 0 && std::strcmp(s.value, "0") == 0)
                            return static_cast<int>(k);
            }
            if (row.widescreen)
                return aspectIndex(rdram, row);
            const int index = currentIndex(row);
            return index < 0 ? 0 : index;
        }

        // ---- The paged mode: the Start menu's list shows our pages ----
        bool g_pc = false;
        uint32_t g_self = 0u;
        int g_page = 0;
        int g_lastRow = 0; // the row the player had selected, last frame the page was shown
        uint32_t g_closeSeen = 0u; // mod_menu::closeCount() as the page last read its rows: F1 closed since means they are read again
        uint8_t g_lockWas = 0u; // the game's "unlocked" byte (0x6CFA70) before the pages
        bool g_lock = false;    // the pages are on the Start menu: its locked rows (Continue, Extras) and that byte matter
        std::vector<std::pair<int, int>> g_stack; // the pages above this one, with the row each was left on
        int g_shown[kMaxRows] = {}; // what the game's own stepping has in each row's current (a row with more choices than texts: 0)
        int g_idx[kMaxRows] = {};   // the choice each row stands on
        Page g_cur;                 // the page that is shown, its registry rows filled in (instantiate)
        std::vector<uint8_t> g_save; // the Start menu's own rows (three pieces)

        // A row whose change waits for the next start (F1's RESTART tag): one of the settings it writes is pending in the registry.
        bool rowPending(const Row &row)
        {
            for (const Opt &o : row.opts)
                for (const Set &set : o.sets)
                    if (!set.onlyWrite && isPending(set.key))
                        return true;
            return false;
        }

        bool pageHasPending()
        {
            for (const Row &row : g_cur.rows)
                if (rowPending(row))
                    return true;
            return false;
        }

        bool g_tagged[kMaxRows] = {}; // the rows whose change waits for a restart (the page's red notice shows while one does)
        bool g_noticeWanted = false;  // the page shows the notice under its title (a row of it waits for a restart)

        // A row stands on a choice. A row the game's own stepping can hold (up to four texts) has the choice in its current. A longer
        // one has the choice's label in all four texts and 0 in its current: the game steps that, and scanChanges takes the step
        // from the move that was given (left or right), so the arrows, the sound and the wrap are the game's, the list is ours.
        // A slider row (the lists with bars): the game draws ten steps from the value at +0x568, and its left / right moves it by one,
        // clamped to 0..10. The row's choices are spread over those steps (the first at 0, the last at 10: a move at an end changes
        // nothing), and the choice is told in the row's label, the bar having no text of its own.
        bool isSlider(const Row &row)
        {
            return row.slider && g_host && (g_host->kind == kHqOptions || g_host->kind == kPauseMenu) && row.opts.size() > 1u;
        }

        int stepsFor(size_t choices, int index)
        {
            return choices <= 1u ? 0 : static_cast<int>(std::lround(static_cast<double>(index) * 10.0 / static_cast<double>(choices - 1u)));
        }

        void showRow(uint8_t *rdram, uint32_t self, uint32_t i, const Row &row, int index)
        {
            g_idx[i] = index;
            g_tagged[i] = rowPending(row);
            if (isSlider(row))
            {
                std::string choice = row.opts[static_cast<size_t>(index)].label;
                if (const size_t paren = choice.find(" ("); paren != std::string::npos && paren > 0u)
                    choice.resize(paren); // ("Auto (default)": the label is as long as the bar is wide)
                std::string name = row.label;
                // (shorter: the label is right-aligned in a bar that starts off the screen, and the choice follows it)
                if (name.rfind("Left Dead Zone", 0) == 0)
                    name = "L Deadzone";
                else if (name.rfind("Right Dead Zone", 0) == 0)
                    name = "R Deadzone";
                const std::string key = keyFor(name + " " + choice);
                putKey(rdram, self + kLabels + i * kStringSize, key);
                putKey(rdram, self + kNames + i * kStringSize, key);
                const int steps = stepsFor(row.opts.size(), index);
                write32(rdram, self + kSliderValues + i * 4u, static_cast<uint32_t>(steps));
                g_shown[i] = steps;
                return;
            }
            const std::string here = keyFor(row.opts[static_cast<size_t>(index)].label);
            if (row.opts.size() <= kMaxValues)
            {
                for (uint32_t k = 0; k < row.opts.size(); ++k)
                    putKey(rdram, self + kValueTexts + (i * kMaxValues + k) * kStringSize,
                           static_cast<int>(k) == index ? here : keyFor(row.opts[k].label));
                write32(rdram, self + kCurrent + i * 4u, static_cast<uint32_t>(index));
                g_shown[i] = index;
                return;
            }
            write32(rdram, self + kCurrent + i * 4u, 0u);
            g_shown[i] = 0;
            for (uint32_t k = 0; k < kMaxValues; ++k)
                putKey(rdram, self + kValueTexts + (i * kMaxValues + k) * kStringSize, here);
        }

        // The notice follows the registry: a row whose change has become pending (or stopped being) is told of (the page shows the
        // red notice while any row waits).
        void refreshTags(uint8_t *rdram, uint32_t self)
        {
            bool any = false;
            for (uint32_t i = 0; i < g_cur.rows.size() && i < kMaxRows; ++i)
            {
                const Row &row = g_cur.rows[i];
                if (row.link >= 0 || row.action != 0 || row.opts.empty())
                    continue;
                any = any || rowPending(row);
                if (rowPending(row) != g_tagged[i])
                {
                    g_tagged[i] = !g_tagged[i];
                    if (trace())
                        std::fprintf(stderr, "[pc settings] %s: %s\n", row.label, g_tagged[i] ? "RESTART (the change waits for the next start)" : "no longer waits for a restart");
                }
            }
            if (any != g_noticeWanted && !g_cur.rows.empty() && isNoticeRow(g_cur.rows[0]))
            {
                const std::string key = keyFor(noticeText(any));
                putKey(rdram, self + kLabels, key);
                putKey(rdram, self + kNames, key);
            }
            g_noticeWanted = any;
        }

        void savePieces(const uint8_t *rdram, uint32_t self)
        {
            g_save.assign(kSave1Size + kSave2Size + kSave3Size, 0u);
            std::memcpy(g_save.data(), getConstMemPtr(rdram, self + kSave1), kSave1Size);
            std::memcpy(g_save.data() + kSave1Size, getConstMemPtr(rdram, self + kSave2), kSave2Size);
            std::memcpy(g_save.data() + kSave1Size + kSave2Size, getConstMemPtr(rdram, self + kSave3), kSave3Size);
        }

        void restorePieces(uint8_t *rdram, uint32_t self)
        {
            if (g_save.size() != kSave1Size + kSave2Size + kSave3Size)
                return;
            std::memcpy(getMemPtr(rdram, self + kSave1), g_save.data(), kSave1Size);
            std::memcpy(getMemPtr(rdram, self + kSave2), g_save.data() + kSave1Size, kSave2Size);
            std::memcpy(getMemPtr(rdram, self + kSave3), g_save.data() + kSave1Size + kSave2Size, kSave3Size);
        }

        void buildPage(uint8_t *rdram, uint32_t self, int pageIndex, int selected)
        {
            g_cur = instantiate(pages()[static_cast<size_t>(pageIndex)]);
            const Page &page = g_cur;
            g_page = pageIndex;
            g_pagesUp.store(true);
            g_noticeWanted = pageHasPending();
            std::memset(getMemPtr(rdram, self + kSave1), 0, kSave1Size); // (empty short strings: nothing to free)
            std::memset(getMemPtr(rdram, self + kSkipped), 0, kSave3Size);
            for (uint32_t i = 0; i < page.rows.size() && i < kMaxRows; ++i)
            {
                const Row &row = page.rows[i];
                const std::string label = isNoticeRow(row) ? keyFor(noticeText(g_noticeWanted)) : keyFor(row.label);
                putKey(rdram, self + kLabels + i * kStringSize, label);
                putKey(rdram, self + kNames + i * kStringSize, label);
                if (isNoticeRow(row))
                {
                    *getMemPtr(rdram, self + kSkipped + i) = 1; // (the selection skips it: the game's own handler honours the flag)
                    if (selected == static_cast<int>(i))
                        selected = static_cast<int>(i) + 1;
                }
                if (row.link >= 0 || row.action != 0)
                    continue;
                *getMemPtr(rdram, self + kValueFlags + i) = 1;
                if (isSlider(row))
                {
                    *getMemPtr(rdram, self + kBarFlags + i) = 1; // (a bar of steps, no value text)
                    *getMemPtr(rdram, self + kValueFlags + i) = 0;
                }
                const bool longer = row.opts.size() > kMaxValues;
                write32(rdram, self + kLimits + i * 4u, static_cast<uint32_t>(longer ? kMaxValues : row.opts.size()));
                const int now = liveIndex(rdram, row);
                if (trace())
                {
                    std::fprintf(stderr, "[pc settings]   %s shows %s\n", row.label, row.opts[static_cast<size_t>(now)].label);
                    // (the row as JSON, for the settings test: its choices and the settings each one writes; * = only written)
                    std::string d = std::string("{\"label\":\"") + row.label + "\",\"key\":\"" + (row.key ? row.key : "") + "\",\"legacy\":" +
                                    std::to_string(row.legacy) + ",\"preset\":" + (row.preset ? "true" : "false") + ",\"now\":" +
                                    std::to_string(now) + ",\"custom\":" + (row.custom ? "true" : "false") + ",\"opts\":[";
                    for (size_t k = 0; k < row.opts.size(); ++k)
                    {
                        d += std::string(k ? "," : "") + "{\"l\":\"" + row.opts[k].label + "\",\"s\":[";
                        for (size_t m = 0; m < row.opts[k].sets.size(); ++m)
                        {
                            const Set &s = row.opts[k].sets[m];
                            d += std::string(m ? "," : "") + "[\"" + s.key + "\",\"" + s.value + "\"," + (s.onlyWrite ? "1" : "0") + "]";
                        }
                        d += "]}";
                    }
                    std::fprintf(stderr, "[pc settings dump] %s]}\n", d.c_str());
                }
                showRow(rdram, self, i, row, now);
            }
            putKey(rdram, self + kTitle, keyFor(page.title));
            write32(rdram, self + kCount, static_cast<uint32_t>(page.rows.size()));
            write32(rdram, self + kSelected, static_cast<uint32_t>(selected));
            if (trace())
                std::fprintf(stderr, "[pc settings] page %s (%zu rows)\n", page.title, page.rows.size());
        }

        // The page is on the list: its row count and its first label are ours.
        bool pageShown(const uint8_t *rdram, uint32_t self)
        {
            const Page &page = g_cur;
            if (page.rows.empty())
                return false;
            if (read32(rdram, self + kCount) != page.rows.size())
                return false;
            // (the first row whose label does not change: the notice row's text does, and a slider row's label tells its choice)
            size_t first = 0u;
            while (first < page.rows.size() && (isNoticeRow(page.rows[first]) || isSlider(page.rows[first])))
                ++first;
            if (first >= page.rows.size())
                return true; // (nothing to tell it by: the count is what there is)
            const std::string key = keyFor(page.rows[first].label);
            return textIs(rdram, self + kLabels + static_cast<uint32_t>(first) * kStringSize, key.c_str(), key.size());
        }

        // (the game sets the "locked" flags of rows 1 and 3, Continue and Extras, again at each of its states, and puts the
        // selection back on row 0 when it rests on either - unless its global at 0x6CFA70 is set: held set while the pages
        // are up, as it was when they close. The game only tests that byte for zero, and it only ever stores 1 (0 until the
        // Start menu's first run): the pages hold it as 2 | the value it had, so that a save state made with a page open
        // still carries the value to put back (value & 1, see closeLeftPage).)
        void clearSkips(uint8_t *rdram, uint32_t self)
        {
            std::memset(getMemPtr(rdram, self + kSkipped), 0, kSave3Size);
            if (!g_cur.rows.empty() && isNoticeRow(g_cur.rows[0]))
                *getMemPtr(rdram, self + kSkipped) = 1; // (the notice row stays skipped)
            if (g_lock)
                if (uint8_t *p = getMemPtr(rdram, kUnlocked))
                    *p = static_cast<uint8_t>(g_lockWas | 2u);
        }

        void putBackLock(uint8_t *rdram)
        {
            if (g_lock)
                if (uint8_t *p = getMemPtr(rdram, kUnlocked))
                    *p = g_lockWas;
            g_lock = false;
        }

        void enterPages(uint8_t *rdram, uint32_t self, const Host &host, int startPage, int selected = 0)
        {
            g_closeSeen = mod_menu::closeCount(); // (F1 closed before the pages were up changes nothing on them)
            savePieces(rdram, self);
            g_lock = host.kind == kStartMenu;
            if (g_lock)
                if (const uint8_t *p = getConstMemPtr(rdram, kUnlocked))
                    g_lockWas = *p;
            g_pc = true;
            g_host = &host;
            g_self = self;
            g_lastRow = 0;
            g_stack.clear();
            buildPage(rdram, self, startPage, selected);
        }

        void leavePages(uint8_t *rdram, uint32_t self)
        {
            if (g_pc)
                restorePieces(rdram, self);
            putBackLock(rdram);
            g_pc = false;
            g_pagesUp.store(false);
            g_noticeWanted = false;
            if (trace())
                std::fprintf(stderr, "[pc settings] back to the %s\n", g_host ? g_host->name : "screen");
        }

        // The game's options, written by the pages: the widescreen switch (as the Start Options screen's own row did) and the sound
        // mode. The Start Options screen's Back puts both back as they were when it opened, so that copy is what they are now.
        void syncOpenOptions(uint8_t *rdram, uint32_t self)
        {
            if (!g_host || g_host->kind != kStartOptions)
                return;
            if (uint8_t *p = getMemPtr(rdram, self + kOpenWidescreen))
                *p = gameWidescreen(rdram) ? 1u : 0u;
            write32(rdram, self + kOpenSoundMode, static_cast<uint32_t>(gameSoundMode(rdram)));
        }

        void setGameWidescreen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t self, bool wide)
        {
            if (gameWidescreen(rdram) == wide)
                return;
            callGuest(rdram, ctx, runtime, kSetByteOption, 4u, wide ? 1u : 0u);
            const uint32_t a = callGuest(rdram, ctx, runtime, kOptionsFn1, 0u, 0u);
            const uint32_t b = callGuest(rdram, ctx, runtime, kOptionsFn2, a, 0u);
            callGuest(rdram, ctx, runtime, kOptionsFn3, b, 0u); // (the screen laid out again, as the game's own Widescreen row did)
            syncOpenOptions(rdram, self);
            if (trace())
                std::fprintf(stderr, "[pc settings] the game's widescreen switch: %s\n", gameWidescreen(rdram) ? "on" : "off");
        }

        void setGameSoundMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t self, int mode)
        {
            callGuest(rdram, ctx, runtime, kSetWordOption, 1u, static_cast<uint32_t>(mode));
            callGuest(rdram, ctx, runtime, kApplyWordOption, 1u, 0u);
            syncOpenOptions(rdram, self);
            if (trace())
                std::fprintf(stderr, "[pc settings] the game's sound mode: %d\n", gameSoundMode(rdram));
        }

        // One page up; from the first page, out of the pages (the screen's own rows come back). From a page that leaves the screen
        // (the Start Options hub) the game's Back is returned: it puts its options away and goes on to its save.
        // ---- The bars slide between pages ----
        // The game's own screens slide their bars in when they come and out when they go (the audio screen does, on its own): the pause
        // menu's and the HQ's pages did not, the rows were swapped in place. A page change now slides the old rows out, makes the change,
        // and slides the new ones in. The slide is the game's own: SetBarPositions (0x312820) places the bars by the screen's state and
        // its transition position (unk_50, 0 to 1); during a slide that call is made with the state "moving in" and a position of our
        // own timing, and the screen's real state and position (which the pause menu's tint and the screen manager read) are put back
        // after it. TF_PC_SETTINGS_ANIM=0 leaves it out.
        constexpr uint32_t kSetBarPositions = 0x00312820u, kScreenState = 0x54u, kScreenT = 0x50u, kSlideInSpeed = 0x48u, kSlideOutSpeed = 0x4Cu;
        struct Slide
        {
            int phase = 0; // 0 none, 1 the old rows slide out, 2 the new rows slide in
            std::chrono::steady_clock::time_point start{};
            float seconds = 0.25f;
            uint32_t screen = 0u;
        };
        Slide g_slide;
        std::function<void()> g_slideChange; // the page change, made when the old rows are out

        float floatAt(const uint8_t *rdram, uint32_t address)
        {
            const uint32_t v = read32(rdram, address);
            float f;
            std::memcpy(&f, &v, sizeof(f));
            return f;
        }

        bool slidesHere()
        {
            static const bool on = !(std::getenv("TF_PC_SETTINGS_ANIM") && *std::getenv("TF_PC_SETTINGS_ANIM") == '0');
            return on && g_host && (g_host->kind == kHqOptions || g_host->kind == kPauseMenu);
        }

        bool sliding() { return g_slide.phase != 0; }

        float slideElapsed() { return std::chrono::duration<float>(std::chrono::steady_clock::now() - g_slide.start).count(); }

        // The bars' position: 1 in place, 0 off the screen; out 1 to 0, in 0 to 1.
        float slideT()
        {
            const float e = std::clamp(slideElapsed() / g_slide.seconds, 0.0f, 1.0f);
            return g_slide.phase == 1 ? 1.0f - e : e;
        }

        float slideSeconds(const uint8_t *rdram, uint32_t screen, bool out)
        {
            const float speed = floatAt(rdram, screen + (out ? kSlideOutSpeed : kSlideInSpeed)); // (the screen's own transition speeds)
            return speed > 0.5f && speed < 50.0f ? 1.0f / speed : 0.25f;
        }

        // A page change: the old rows slide out, `change` runs, the new rows slide in. Where there is no slide it runs at once.
        void changePage(uint8_t *rdram, uint32_t self, std::function<void()> change)
        {
            if (!slidesHere() || g_slide.phase == 1 || !inRam(self))
            {
                change();
                return;
            }
            // (a change asked for while the new rows are still sliding in: they slide out from where they are)
            const float where = g_slide.phase == 2 ? slideT() : 1.0f;
            g_slideChange = std::move(change);
            g_slide.phase = 1;
            g_slide.screen = self;
            g_slide.seconds = slideSeconds(rdram, self, true);
            g_slide.start = std::chrono::steady_clock::now() -
                            std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<float>((1.0f - where) * g_slide.seconds));
        }

        void stopSlide()
        {
            g_slide.phase = 0;
            g_slideChange = nullptr;
        }

        // Each input call of the screen: the phases follow one another.
        void tickSlide(uint8_t *rdram, uint32_t self)
        {
            if (g_slide.phase == 1 && slideElapsed() >= g_slide.seconds)
            {
                const std::function<void()> change = std::move(g_slideChange);
                g_slideChange = nullptr;
                if (change)
                    change();
                g_slide.phase = 2;
                g_slide.start = std::chrono::steady_clock::now();
                g_slide.seconds = slideSeconds(rdram, self, false);
            }
            else if (g_slide.phase == 2 && slideElapsed() >= g_slide.seconds)
                g_slide.phase = 0;
        }

        PS2Runtime::RecompiledFunction g_barPositionsOriginal = nullptr;

        void barPositionsHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const bool entry = ctx->pc == kSetBarPositions;
            const uint32_t self = getRegU32(ctx, 4), returnTo = getRegU32(ctx, 31);
            if (!entry || g_slide.phase == 0 || self != g_slide.screen || !inRam(self))
            {
                g_barPositionsOriginal(rdram, ctx, runtime);
                return;
            }
            const uint32_t state = read32(rdram, self + kScreenState), t = read32(rdram, self + kScreenT);
            write32(rdram, self + kScreenState, 1u);
            const float slide = slideT();
            uint32_t bits;
            std::memcpy(&bits, &slide, sizeof(bits));
            write32(rdram, self + kScreenT, bits);
            g_barPositionsOriginal(rdram, ctx, runtime);
            if (ctx->pc == returnTo)
            {
                write32(rdram, self + kScreenState, state);
                write32(rdram, self + kScreenT, t);
            }
        }

        std::set<std::string> g_asked;     // the settings the prompt has been shown for on leaving their page (once each)
        std::set<std::string> g_askedExit; // the settings it has been shown for on leaving the pages altogether (once each)
        int g_promptFrom = -1;             // the page the prompt came up on: Later goes on from there

        // The settings of the page that are pending and not asked about yet.
        bool freshPending()
        {
            bool fresh = false;
            for (const Row &row : g_cur.rows)
                for (const Opt &o : row.opts)
                    for (const Set &set : o.sets)
                        if (!set.onlyWrite && isPending(set.key) && g_asked.insert(set.key).second)
                            fresh = true;
            return fresh;
        }

        // Every setting of the pages that is pending and not asked about on leaving the pages yet: a change on one page and another
        // on another page are asked about together, once.
        bool freshPendingAnywhere()
        {
            bool fresh = false;
            for (const Page &page : pages())
                for (const Row &row : page.rows)
                {
                    if (row.key && isPending(row.key) && g_askedExit.insert(row.key).second)
                        fresh = true;
                    for (const Opt &o : row.opts)
                        for (const Set &set : o.sets)
                            if (!set.onlyWrite && isPending(set.key) && g_askedExit.insert(set.key).second)
                                fresh = true;
                }
            return fresh;
        }

        uint32_t goBackNow(uint8_t *rdram, uint32_t self)
        {
            // (TF_PC_SETTINGS_PROMPT=0: no prompt, the red notice stays)
            static const bool prompt = !(std::getenv("TF_PC_SETTINGS_PROMPT") && *std::getenv("TF_PC_SETTINGS_PROMPT") == '0');
            if (prompt && g_page != kRestartPage && g_page != kQuitPage)
            {
                // A change that only applies at the next start was made on this page: asked about before the page is left. Leaving the
                // pages for the screen they sit on (Back at the first page) asks once more about every change that still waits, on
                // whichever page it was made.
                const bool leaving = g_stack.empty();
                if (leaving ? freshPendingAnywhere() : freshPending())
                {
                    if (trace())
                        std::fprintf(stderr, "[pc settings] the prompt: a change waits for the next start%s\n", leaving ? " (leaving the pages)" : "");
                    g_promptFrom = g_page;
                    buildPage(rdram, self, kRestartPage, 0);
                    return 0u;
                }
            }
            if (g_stack.empty())
            {
                const int from = g_page == kRestartPage && g_promptFrom >= 0 ? g_promptFrom : g_page; // (Later: on from the page it was asked on)
                g_promptFrom = -1;
                const bool leaves = pages()[static_cast<size_t>(from)].leavesScreen;
                leavePages(rdram, self);
                return leaves ? kInputBack : 0u;
            }
            g_promptFrom = -1;
            const auto [p, s] = g_stack.back();
            g_stack.pop_back();
            buildPage(rdram, self, p, s);
            return 0u;
        }

        // One page up with the slide where the screen has one (the Start menu's text lists have none: it is made at once).
        uint32_t goBack(uint8_t *rdram, uint32_t self)
        {
            if (!slidesHere() || sliding())
                return goBackNow(rdram, self);
            changePage(rdram, self, [rdram, self]() { goBackNow(rdram, self); });
            return 0u;
        }

        bool g_layoutReturn = false;   // the button layout screen was opened from the Controls menu: its Back returns there
        bool g_controlsOnShow = false; // the HQ's Options screen, shown next, opens the Controls menu
        bool g_audioOnShow = false;    // the pause menu's Audio Settings (the game's own screen) was opened from the OPTIONS list: its Back
                                       // shows the pause menu again, and that is this list again
        constexpr uint32_t kPauseFlag = 0x006CF5A8u; // the game's pause state word: 2 = the pause button was pressed on the audio screen
        constexpr char kOptionsName[] = "OptionsScreen"; // the HQ's Options screen
        int g_positionState = 0; // the game's Screen Position mode: 1 asked for, 2 on (the hub comes back when it ends)
        int g_positionWait = 0;

        // Cross on a row, Back: the page's own moves. Returns the input the game's handler is then given.
        constexpr char kLayoutName[] = "ControllerConfiguration"; // the game's button layout screen
        constexpr uint32_t kScreens = 0x00316A60u, kChangeScreen = 0x00318090u, kCloseScreen = 0x001A0E50u; // (owner), (owner, name, 1), (screen, 3)

        uint32_t pageInput(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t self, uint32_t input, bool pending)
        {
            const Page &page = g_cur;
            if (input == kInputConfirm || pending)
            {
                const uint32_t selected = read32(rdram, self + kSelected);
                if (selected >= page.rows.size())
                    return 0u;
                const Row &row = page.rows[selected];
                if (row.link >= 0)
                {
                    playTone(rdram, ctx, runtime, true);
                    const int from = g_page, at = static_cast<int>(selected), to = row.link;
                    changePage(rdram, self,
                               [rdram, self, from, at, to]()
                               {
                                   g_stack.emplace_back(from, at);
                                   buildPage(rdram, self, to, 0);
                               });
                    return 0u;
                }
                if (row.action == 1)
                {
                    playTone(rdram, ctx, runtime, false);
                    return goBack(rdram, self);
                }
                if (row.action == 2)
                {
                    playTone(rdram, ctx, runtime, true);
                    if (trace())
                        std::fprintf(stderr, "[pc settings] quit the game\n");
                    if (g_runtime)
                        g_runtime->requestStop();
                    return 0u;
                }
                if (row.action == 3)
                {
                    playTone(rdram, ctx, runtime, true);
                    if (trace())
                        std::fprintf(stderr, "[pc settings] More settings: the F1 menu\n");
                    mod_menu::requestOpen(); // (the window thread opens it: mod_menu::draw)
                    return 0u;
                }
                if (row.action == 7)
                {
                    playTone(rdram, ctx, runtime, true);
                    g_restartAsked.store(true); // (the window thread starts F1's restart: pump)
                    return 0u;
                }
                if (row.action == 4)
                {
                    // Out of the pages: the screen's own rows are back, the selection where it was (the Options row), and the
                    // confirm goes to the game as it would have.
                    g_audioOnShow = g_host && g_host->kind == kPauseMenu;
                    setMusic(rdram, ctx, runtime, false);
                    leavePages(rdram, self);
                    return kInputConfirm;
                }
                if (row.action == 6)
                {
                    playTone(rdram, ctx, runtime, true);
                    g_layoutReturn = true;
                    // The game's button layout screen, as its Controller Settings screen's first row opens it: the screen is
                    // changed by name and this one closed. Its Back goes on to the Controller Settings and the Options list.
                    leavePages(rdram, self);
                    const uint32_t owner = callGuest(rdram, ctx, runtime, kScreens, 0u, 0u);
                    callGuestNamed(rdram, ctx, runtime, kChangeScreen, owner, kLayoutName, 1u);
                    callGuest(rdram, ctx, runtime, kCloseScreen, self, 3u);
                    if (trace())
                        std::fprintf(stderr, "[pc settings] Button Layout: the game's controller screen\n");
                    return 0u;
                }
                if (row.action == 5)
                {
                    // The game's own Screen Position mode: its rows are back, the selection on its row, the confirm goes to it. The
                    // hub comes back when the mode ends (processInput).
                    leavePages(rdram, self);
                    write32(rdram, self + kSelected, 2u);
                    g_positionState = 1;
                    g_positionWait = 0;
                    return kInputConfirm;
                }
                // A confirm on a value row steps nothing: the choice is made with left / right and is in force at once (and kept in
                // settings.ini). On the Start Options screen, whose footer says Save, it is the Save: the page goes up.
                if (g_host && g_host->kind == kStartOptions)
                {
                    playTone(rdram, ctx, runtime, true);
                    return goBack(rdram, self);
                }
                return 0u;
            }
            if (input == kInputBack)
            {
                playTone(rdram, ctx, runtime, false);
                return goBack(rdram, self);
            }
            return input;
        }

        // The rows the preset sets take the values a preset is about to give them (the window thread writes the settings a
        // moment later; the rows show them at once).
        void showPresetRows(uint8_t *rdram, uint32_t self, const Page &page, const Opt &preset)
        {
            for (uint32_t i = 0; i < page.rows.size() && i < kMaxRows; ++i)
            {
                const Row &row = page.rows[i];
                if (row.link >= 0 || row.action != 0 || row.preset || !row.member)
                    continue;
                const int now = currentIndex(row, &preset);
                if (now >= 0) // (no choice stands for the preset's value: the row stays as it is)
                    showRow(rdram, self, i, row, now);
            }
        }

        // A value row stepped by the game's own code: the setting it writes is queued. The graphics preset sets the rows
        // that belong to it too; one of those rows changed by hand makes the preset Custom (its last choice).
        void scanChanges(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t self, uint32_t input)
        {
            const Page &page = g_cur;
            int presetRow = -1;
            bool byHand = false;
            for (uint32_t i = 0; i < page.rows.size() && i < kMaxRows; ++i)
            {
                const Row &row = page.rows[i];
                if (row.link >= 0 || row.action != 0)
                    continue;
                if (row.preset)
                    presetRow = static_cast<int>(i);
                const int indexBefore = g_idx[i]; // (the choice the row stood on before this move)
                const uint32_t seen = read32(rdram, self + kCurrent + i * 4u);
                const bool longer = row.opts.size() > kMaxValues;
                uint32_t now;
                if (isSlider(row))
                {
                    // (the game moved the steps by one: left or right; the choice moves by one, the steps are put where the choice has them)
                    const int steps = static_cast<int>(read32(rdram, self + kSliderValues + i * 4u));
                    if (steps == g_shown[i])
                        continue;
                    const int previous = g_idx[i];
                    const int next = std::clamp(previous + (steps > g_shown[i] ? 1 : -1), 0, static_cast<int>(row.opts.size()) - 1);
                    showRow(rdram, self, i, row, next);
                    if (next == previous)
                        continue;
                    now = static_cast<uint32_t>(next);
                }
                else if (longer)
                {
                    // (The game stepped the row's current from 0 to 1 or 3 and wrapped it within its four texts, all the same: the
                    // choice moves by the move that was given, 3 left and 4 right.)
                    if (seen == 0u)
                        continue;
                    write32(rdram, self + kCurrent + i * 4u, 0u);
                    const int delta = input == kInputRight ? 1 : input == 3u ? -1 : 0;
                    if (delta == 0)
                        continue;
                    const int n = static_cast<int>(row.opts.size());
                    now = static_cast<uint32_t>((g_idx[i] + delta + n) % n);
                    showRow(rdram, self, i, row, static_cast<int>(now));
                }
                else
                {
                    now = seen;
                    if (now == static_cast<uint32_t>(g_shown[i]) || now >= row.opts.size())
                        continue;
                    g_shown[i] = static_cast<int>(now);
                    g_idx[i] = static_cast<int>(now);
                }
                if (row.preset)
                {
                    if (!row.opts[now].sets.empty()) // (Custom writes nothing)
                    {
                        queueOpt(row.opts[now]);
                        showPresetRows(rdram, self, page, row.opts[now]);
                    }
                }
                else
                {
                    queueOpt(row.opts[now]);
                    byHand = byHand || row.member;
                    if (row.widescreen) // (4:3 or a wide shape: the game's own switch too, live)
                    {
                        bool wide = false;
                        for (const Set &s : row.opts[now].sets)
                            if (std::strcmp(s.key, "PS2X_WIDESCREEN") == 0 && std::strcmp(s.value, "1") == 0)
                                wide = true;
                        setGameWidescreen(rdram, ctx, runtime, self, wide);
                    }
                    else if (row.legacy == 1)
                        setGameSoundMode(rdram, ctx, runtime, self, static_cast<int>(now) + 1);
                    else if (row.legacy >= 2)
                    {
                        const uint32_t option = static_cast<uint32_t>(byteOptionOf(row.legacy));
                        const uint32_t byte = byteFor(option, static_cast<uint32_t>(now));
                        callGuest(rdram, ctx, runtime, kSetByteOption, option, byte);
                        if (trace())
                            std::fprintf(stderr, "[pc settings] the game's option %u: %s\n", option, byte != 0u ? "set" : "clear");
                    }
                    else if (row.dyn == 2)
                    {
                        // The vibration strength: the game's own Vibration option (byte option 0) goes on with any strength, off with 0.
                        bool off = false;
                        for (const Set &s : row.opts[now].sets)
                            if (std::strcmp(s.key, "PS2X_SONYPAD_RUMBLE") == 0 && std::strcmp(s.value, "0") == 0)
                                off = true;
                        const uint32_t byte = byteFor(0u, off ? 1u : 0u);
                        callGuest(rdram, ctx, runtime, kSetByteOption, 0u, byte);
                        if (trace())
                            std::fprintf(stderr, "[pc settings] the game's Vibration option: %s\n", byte != 0u ? "on" : "off");
                        // A short buzz when the vibration comes on, as the game's own audio screen gave (taVibrationManager::Vibrate(0, 8)).
                        bool wasOff = false;
                        if (indexBefore >= 0 && static_cast<size_t>(indexBefore) < row.opts.size())
                            for (const Set &s : row.opts[static_cast<size_t>(indexBefore)].sets)
                                if (std::strcmp(s.key, "PS2X_SONYPAD_RUMBLE") == 0 && std::strcmp(s.value, "0") == 0)
                                    wasOff = true;
                        if (!off && wasOff)
                            callGuest(rdram, ctx, runtime, kVibrate, 0u, 8u);
                    }
                }
                if (trace())
                    std::fprintf(stderr, "[pc settings] %s: %s\n", row.label, row.opts[now].label);
            }
            if (byHand && presetRow >= 0)
            {
                const uint32_t custom = static_cast<uint32_t>(page.rows[static_cast<size_t>(presetRow)].opts.size()) - 1u;
                if (g_shown[presetRow] != static_cast<int>(custom))
                {
                    write32(rdram, self + kCurrent + static_cast<uint32_t>(presetRow) * 4u, custom);
                    g_shown[presetRow] = static_cast<int>(custom);
                    g_idx[presetRow] = static_cast<int>(custom);
                    if (trace())
                        std::fprintf(stderr, "[pc settings] Preset: Custom (a row was changed by hand)\n");
                }
            }
        }

        // ---- The new rows. The Start menu: QUIT GAME (row 4) after Extras. The Start Options and the HQ's Options screens: Display,
        //      Graphics, PC Controls and More settings (kExtras) after the game's own rows, so the settings sit in the game's own
        //      Options lists. The pause menu's rows stay as they are: its Options row opens the pages (an OPTIONS list). ----
        void plainRow(uint8_t *rdram, uint32_t self, uint32_t row)
        {
            *getMemPtr(rdram, self + kBarFlags + row) = 0; // a plain row: no bar, no value, not skipped
            *getMemPtr(rdram, self + kValueFlags + row) = 0;
            *getMemPtr(rdram, self + kSkipped + row) = 0;
        }

        bool ensureRow(uint8_t *rdram, uint32_t self, const Host &host)
        {
            if (host.kind == kStartMenu && read32(rdram, self + kState) != kMenuState)
                return false;
            const uint32_t count = read32(rdram, self + kCount);
            if (count == host.totalRows)
                return true;
            // Only the screen's own rows (the Start menu's memory-card questions reuse its screen with other labels).
            if (count != host.baseRows || !textIs(rdram, self + kLabels, host.firstKey, std::strlen(host.firstKey)))
                return false;
            if (host.kind == kStartMenu)
            {
                putKey(rdram, self + kLabels + kQuitRow * kStringSize, keyFor("QUIT GAME"));
                plainRow(rdram, self, kQuitRow);
            }
            else
            {
                // The HQ's Options: Audio Settings stays the game's; the game's Controller Settings row (its options are in
                // Controls now) gives its place to our rows, its string kept in a spare slot (not copied, not leaked).
                const uint32_t first = host.kind == kHqOptions ? 1u : host.baseRows;
                if (host.kind == kHqOptions)
                {
                    uint8_t *labels = getMemPtr(rdram, self + kLabels);
                    std::memcpy(labels + 6u * kStringSize, labels + 1u * kStringSize, kStringSize);
                }
                for (uint32_t k = 0; k < kExtraCount; ++k)
                {
                    putKey(rdram, self + kLabels + (first + k) * kStringSize, keyFor(kExtras[k].label));
                    plainRow(rdram, self, first + k);
                }
            }
            write32(rdram, self + kCount, host.totalRows);
            if (trace())
                std::fprintf(stderr, "[pc settings] the %s at %08x: rows added (%u rows)\n", host.name, self, host.totalRows);
            return true;
        }

        // The screen's title is one of our pages' (its key is ours).
        bool leftoverPage(const uint8_t *rdram, uint32_t self)
        {
            const std::string *title = ourText(rdram, self + kTitle);
            if (!title)
                return false;
            for (const Page &page : pages())
                if (*title == page.title)
                    return true;
            return false;
        }

        // A page of ours left on a screen by a save state, closed (processInput): the rows emptied and the screen filled again by
        // the game's own function. True when that function ran to its end (callGuest: false when the scheduler stopped it and the
        // call was abandoned). The Start menu's own fill also clears its "a save was found" word, which its card check sets: kept.
        // The "unlocked" byte was held as 2 | its value under the page (clearSkips): the value is put back.
        constexpr int kCloseTries = 16;     // a close that is still stopped after this many input calls is given up
        uint32_t g_closeSelf = 0u;          // the screen whose left page is being closed (0: none)
        int g_closeTries = 0;

        bool closeLeftPage(const Host &host, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t self, bool start)
        {
            std::memset(getMemPtr(rdram, self + kSave1), 0, kSave1Size);
            std::memset(getMemPtr(rdram, self + kSkipped), 0, kSave3Size);
            bool returned = false;
            if (start)
            {
                const uint32_t found = read32(rdram, self + kSavesFound);
                callGuest(rdram, ctx, runtime, kStartMenuFill, self, 0u, 0u, &returned);
                write32(rdram, self + kSavesFound, found);
                if (returned)
                    if (uint8_t *p = getMemPtr(rdram, kUnlocked))
                        *p &= 1u;
            }
            else
                callGuest(rdram, ctx, runtime, host.init, self, 0u, 0u, &returned);
            return returned;
        }

        // A screen's input handler. The game's code sets the row count back in some of its states and knows nothing of the
        // new rows: they are put back around its call. A confirm on one of the new rows opens its page; while they are open the handler
        // is given only the moves (up, down, left, right), the pages take the confirm and the back.
        void processInput(Host &host, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            // (The recompiled function can be resumed in its middle after a call that yielded - ctx->pc is then one of its
            // labels and the argument registers are not the arguments: left alone, as game_config's HUD hook does.)
            if (ctx->pc != host.input)
            {
                host.inputOriginal(rdram, ctx, runtime);
                return;
            }
            const uint32_t self = getRegU32(ctx, 4);
            uint32_t input = getRegU32(ctx, 5);
            if (!inRam(self) || read32(rdram, self) != host.vtable)
            {
                host.inputOriginal(rdram, ctx, runtime);
                return;
            }
            const bool start = host.kind == kStartMenu;
            if (g_slide.phase != 0 && g_slide.screen != self)
                stopSlide(); // (another screen object: its slide is not this one's)
            if (g_slide.phase != 0)
            {
                tickSlide(rdram, self);
                if (g_slide.phase == 1)
                {
                    input = 0u; // (the pages take no input while the old rows slide out: the change is not made yet)
                    SET_GPR_U32(ctx, 5, 0u);
                }
            }
            if (host.kind == kPauseMenu)
            {
                const bool pages = g_pc && g_host == &host && g_self == self; // (the pause menu's pages: the music plays, the footer has a Back)
                setMusic(rdram, ctx, runtime, pages);
                setPauseFooter(rdram, ctx, runtime, self, pages);
            }
            if (!g_pc && ((g_closeSelf == self && g_closeTries < kCloseTries) || leftoverPage(rdram, self)))
            {
                // A page of ours on the screen while this side shows none: a save state made with the page open (F6, or the
                // settings' "restart to apply" from the More settings row) and loaded - the page mode and the screen's own rows
                // kept aside were the other session's. The page is closed the one way that needs neither: its rows emptied
                // (short strings and flags: nothing to free) and the screen filled again by the game's own function, through
                // its hook here (which opens the hub again on the Start menu's Options). The fill can be stopped half way
                // by the scheduler, and callGuest then abandons it: the close stays pending for this screen and is run
                // again at the next input call (the fills assign everything from scratch, a second run is harmless) until the
                // fill has returned. The game's handler is given no input meanwhile: the rows are half made.
                if (g_closeSelf != self)
                {
                    g_closeSelf = self;
                    g_closeTries = 0;
                }
                ++g_closeTries;
                if (closeLeftPage(host, rdram, ctx, runtime, self, start))
                {
                    g_closeSelf = 0u;
                    if (trace())
                        std::fprintf(stderr, "[pc settings] a page left on the %s by a save state: closed, the screen filled again (%d %s)\n",
                                     host.name, g_closeTries, g_closeTries == 1 ? "try" : "tries");
                }
                else
                {
                    if (trace())
                        std::fprintf(stderr, "[pc settings] a page left on the %s by a save state: the fill was stopped half way, closed again at the next input call\n",
                                     host.name);
                    input = 0u;
                    SET_GPR_U32(ctx, 5, 0u);
                }
            }
            if (g_pc && (g_self != self || g_host != &host))
            {
                putBackLock(rdram);
                g_pc = false; // (another screen object: its own rows are there)
            }
            if (g_pc)
            {
                // The Start menu re-checks the memory card from time to time: its state leaves 9 and comes back, and it writes
                // its own four rows (and the "locked" flags of Continue and Extras) again. The game's own states run untouched;
                // when the menu is back in its own state, the page is put back.
                // (State 0 is the card poll's one-call blip, 1 call in 14, with the rows as they were: a press that lands on it
                // is the pages' too. Any other state is the game's own, rows and all.) The other screens have no such states.
                const uint32_t stateNow = start ? read32(rdram, self + kState) : kMenuState;
                const bool ours = !start || stateNow == kMenuState || stateNow == 0u;
                if (!ours)
                {
                    if (trace() && input != 0u)
                        std::fprintf(stderr, "[pc settings] input %u lost to the game's own state %u\n", input, stateNow);
                    host.inputOriginal(rdram, ctx, runtime);
                    return;
                }
                // F1 can be opened over a page and change a setting the page shows: when it has been closed, the rows are read again
                // (else a row keeps the choice it had when the page was built, and stepping it from there could undo F1's change).
                const bool f1Closed = mod_menu::closeCount() != g_closeSeen;
                if (stateNow == kMenuState && (f1Closed || !pageShown(rdram, self)))
                {
                    if (f1Closed && trace())
                        std::fprintf(stderr, "[pc settings] F1 was closed over the page: its rows are read again\n");
                    g_closeSeen = mod_menu::closeCount();
                    buildPage(rdram, self, g_page, g_lastRow);
                }
                clearSkips(rdram, self);
                const bool pending = start && read32(rdram, self + kPending) == kInputConfirm;
                if (pending)
                    write32(rdram, self + kPending, 0u);
                if (trace() && (input != 0u || pending))
                    std::fprintf(stderr, "[pc settings] input %u%s on page %d, row %u of %u\n", input, pending ? " (pending confirm)" : "",
                                 g_page, read32(rdram, self + kSelected), read32(rdram, self + kCount));
                input = pageInput(rdram, ctx, runtime, self, input, pending);
                SET_GPR_U32(ctx, 5, input);
                // The Start Options screen's handler changes the footer's help line on an up or a down by the row it moves from (its
                // own Screen Position row has "Adjust Screen Offset"). Told it is on row 2 the line is its normal one, Change /
                // Save / Cancel; the selection is then put where the move really goes.
                const bool helpFix = host.kind == kStartOptions && (input == 1u || input == 2u);
                const uint32_t before = read32(rdram, self + kSelected);
                if (helpFix)
                    write32(rdram, self + kSelected, 2u);
                host.inputOriginal(rdram, ctx, runtime);
                if (helpFix)
                {
                    const uint32_t count = read32(rdram, self + kCount);
                    if (count != 0u)
                    {
                        uint32_t to = (before + (input == 2u ? 1u : count - 1u)) % count;
                        if (to == 0u && count > 1u && !g_cur.rows.empty() && isNoticeRow(g_cur.rows[0]))
                            to = input == 2u ? 1u : count - 1u; // (the notice row is skipped)
                        write32(rdram, self + kSelected, to);
                    }
                }
                if (trace() && input != 0u)
                    std::fprintf(stderr, "[pc settings]   after the game's handler (given %u): row %u\n", input,
                                 read32(rdram, self + kSelected));
                if (g_pc && (!start || read32(rdram, self + kState) == kMenuState || read32(rdram, self + kState) == 0u) &&
                    pageShown(rdram, self))
                {
                    clearSkips(rdram, self);
                    scanChanges(rdram, ctx, runtime, self, input);
                    refreshTags(rdram, self);
                    g_lastRow = static_cast<int>(read32(rdram, self + kSelected));
                }
                return;
            }
            int enter = -1;
            uint32_t given = input;
            bool openF1 = false;
            if (ensureRow(rdram, self, host) && (input == kInputConfirm || (start && read32(rdram, self + kPending) == kInputConfirm)))
            {
                const uint32_t selected = read32(rdram, self + kSelected);
                if (start)
                {
                    if (selected == kQuitRow)
                        enter = kQuitPage;
                }
                else if (host.kind == kPauseMenu)
                {
                    if (selected == kPauseOptionsRow && input == kInputConfirm)
                    {
                        enter = 0; // (the OPTIONS list; its Audio Settings gives the confirm back to the game)
                        given = 0u;
                    }
                }
                else if (selected >= (host.kind == kHqOptions ? 1u : host.baseRows) && selected < host.totalRows &&
                         input == kInputConfirm)
                {
                    const Extra &extra = kExtras[selected - (host.kind == kHqOptions ? 1u : host.baseRows)];
                    given = 0u; // (the game has nothing to do with these rows)
                    if (extra.page >= 0)
                        enter = extra.page;
                    else
                        openF1 = true;
                }
            }
            if (given != input)
                SET_GPR_U32(ctx, 5, given);
            host.inputOriginal(rdram, ctx, runtime);
            ensureRow(rdram, self, host);
            if (host.kind == kStartOptions && g_positionState != 0)
            {
                // The game's Screen Position mode runs on its own screen; when it ends, the hub comes back.
                const uint8_t *mode = getConstMemPtr(rdram, self + kPositionMode);
                const bool inMode = mode && *mode != 0u;
                if (g_positionState == 1 && inMode)
                    g_positionState = 2;
                else if ((g_positionState == 2 && !inMode) || (g_positionState == 1 && ++g_positionWait > 30))
                {
                    g_positionState = 0;
                    {
                        // The footer still has the position mode's help line: an up on the Screen Position row (the selection is
                        // still there) puts the Options screen's own back, as the game does when its selection moves off that row.
                        const R5900Context saved = *ctx;
                        SET_GPR_U32(ctx, 4, self);
                        SET_GPR_U32(ctx, 5, 1u);
                        ctx->pc = host.input;
                        host.inputOriginal(rdram, ctx, runtime);
                        *ctx = saved;
                    }
                    enterPages(rdram, self, host, kStartHubPage);
                }
            }
            if (openF1)
            {
                if (trace())
                    std::fprintf(stderr, "[pc settings] More settings: the F1 menu\n");
                mod_menu::requestOpen(); // (the window thread opens it: mod_menu::draw)
            }
            if (enter >= 0)
            {
                g_host = &host; // (playTone chooses the bank by the screen)
                playTone(rdram, ctx, runtime, true);
                Host *hostPointer = &host;
                const int page = enter;
                changePage(rdram, self, [rdram, self, hostPointer, page]() { enterPages(rdram, self, *hostPointer, page); });
            }
        }

        // The function that fills a screen's rows again (the game shows the screen anew): pages left open over it are gone.
        void initHook(Host &host, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc != host.init)
            {
                host.initOriginal(rdram, ctx, runtime);
                return;
            }
            const uint32_t self = getRegU32(ctx, 4);
            const uint32_t pauseFlag = read32(rdram, kPauseFlag); // (the fill itself closes the pause menu when it is 2 and clears it)
            stopSlide(); // (shown anew: a slide in progress is of the screen before)
            if (host.kind == kPauseMenu)
            {
                g_musicOn = false; // (shown anew: the music is as the game has it, and so is the footer: after a page it said Back)
                if (g_footerBack)
                    setPauseFooter(rdram, ctx, runtime, self, false);
                g_footerBack = false;
            }
            host.initOriginal(rdram, ctx, runtime);
            if (trace())
                std::fprintf(stderr, "[pc settings] the %s was filled again (screen %08x, controls wanted %d)\n", host.name, self,
                             g_controlsOnShow ? 1 : 0);
            if (host.kind == kPauseMenu && g_audioOnShow)
            {
                // Back from the game's audio screen: the pause menu is shown again, and the OPTIONS list over it, on Audio Settings.
                g_audioOnShow = false;
                if (pauseFlag != 2u)
                {
                    g_pc = false;
                    g_stack.clear();
                    write32(rdram, self + kSelected, kPauseOptionsRow);
                    enterPages(rdram, self, host, 0, 0);
                    return;
                }
            }
            if (host.kind == kStartOptions)
            {
                // The Start menu's Options: the screen is shown as the hub from its first frame (its own three rows are kept aside
                // and come back for the game's Back and Screen Position).
                g_pc = false;
                g_stack.clear();
                g_positionState = 0;
                enterPages(rdram, self, host, kStartHubPage);
                return;
            }
            if (host.kind == kHqOptions && g_controlsOnShow)
            {
                // Back from the button layout screen: the Controls menu, on its Button Layout row; its Back is this list, on Controls.
                g_controlsOnShow = false;
                g_pc = false;
                g_stack.clear();
                write32(rdram, self + kSelected, 3u);
                enterPages(rdram, self, host, kControlsHqPage, 2);
                return;
            }
            if (g_pc && g_host == &host && g_self == self)
            {
                putBackLock(rdram);
                // The fill sets the count, the title and the labels, not the rows' flags, values, names and limits: those are still the
                // page's when the screen is shown again with a page up (the pause button pressed on a page closes the pause menu): the
                // rows then showed the page's names and bars and did what the game's own rows do. The screen's own are put back.
                if (g_save.size() == kSave1Size + kSave2Size + kSave3Size)
                    std::memcpy(getMemPtr(rdram, self + kSave1), g_save.data(), kSave1Size);
                g_pc = false;
                g_stack.clear();
                if (trace())
                    std::fprintf(stderr, "[pc settings] the %s was shown anew: the pages are closed\n", host.name);
            }
        }

        // The stored game options, applied (the game's own setters, as its Options screens call them) after the card's settings are
        // copied at the start and after a profile is loaded and applied; the port's own widescreen hook sits on the same two.
        constexpr uint32_t kCopyOptionsFn = 0x0032EE40u, kApplyProfileOptionsFn = 0x002780E0u;
        PS2Runtime::RecompiledFunction g_copyOptionsOriginal = nullptr, g_applyProfileOriginal = nullptr;

        void applyStoredOptions(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            for (const StoredOption &o : kStored)
            {
                const std::string v = ps2_settings::value(o.key);
                if (v != o.first && v != o.second)
                    continue; // (keep)
                callGuest(rdram, ctx, runtime, kSetByteOption, o.option, byteFor(o.option, v == o.first ? 0u : 1u));
            }
            const std::string m = ps2_settings::value("TF_OPT_SOUND_MODE");
            const uint32_t mode = m == "mono" ? 1u : m == "stereo" ? 2u : m == "surround" ? 3u : 0u;
            if (mode != 0u)
            {
                callGuest(rdram, ctx, runtime, kSetWordOption, 1u, mode);
                callGuest(rdram, ctx, runtime, kApplyWordOption, 1u, 0u);
            }
            if (trace())
                std::fprintf(stderr, "[pc settings] the stored game options applied\n");
        }

        void copyOptionsHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const bool entry = ctx->pc == kCopyOptionsFn;
            g_copyOptionsOriginal(rdram, ctx, runtime);
            if (entry)
                applyStoredOptions(rdram, ctx, runtime);
        }

        void applyProfileHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const bool entry = ctx->pc == kApplyProfileOptionsFn;
            g_applyProfileOriginal(rdram, ctx, runtime);
            if (entry)
                applyStoredOptions(rdram, ctx, runtime);
        }

        // A new game, or a start with no save, sets the options by the game's two default-setters (the port's own hook on them puts
        // Invert Y off afterwards): the stored options are written into the object they were given, after both. Written as bytes (and
        // the sound mode's word): the setters' calls would go to the options the game has not made current yet.
        constexpr uint32_t kDefaultsFn[2] = {0x0032DFD0u, 0x0032E0F0u};
        PS2Runtime::RecompiledFunction g_defaultsOriginal[2] = {nullptr, nullptr};

        // The stored options written into an options object as bytes (and the sound mode's word).
        void writeStored(uint8_t *rdram, uint32_t options)
        {
            if (!inRam(options))
                return;
            for (const StoredOption &o : kStored)
            {
                const std::string v = ps2_settings::value(o.key);
                if (v != o.first && v != o.second)
                    continue; // (keep)
                if (uint8_t *p = getMemPtr(rdram, options + o.option))
                    *p = static_cast<uint8_t>(byteFor(o.option, v == o.first ? 0u : 1u));
            }
            const std::string m = ps2_settings::value("TF_OPT_SOUND_MODE");
            const uint32_t mode = m == "mono" ? 1u : m == "stereo" ? 2u : m == "surround" ? 3u : 0u;
            if (mode != 0u)
                write32(rdram, options + 8u + 4u, mode);
        }

        template <int I> void defaultsHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const bool entry = ctx->pc == kDefaultsFn[I];
            const uint32_t options = getRegU32(ctx, 4);
            g_defaultsOriginal[I](rdram, ctx, runtime);
            if (entry)
                writeStored(rdram, options);
        }

        // The two small functions that set the CURRENT options (D_006CF244) to the defaults - at the start, and when no save is
        // chosen - reach the default-setters by a jump, which the function table does not see: they are hooked themselves.
        constexpr uint32_t kResetFn[2] = {0x00278250u, 0x00278260u};
        PS2Runtime::RecompiledFunction g_resetOriginal[2] = {nullptr, nullptr};

        template <int I> void resetHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const bool entry = ctx->pc == kResetFn[I];
            g_resetOriginal[I](rdram, ctx, runtime);
            if (entry)
            {
                writeStored(rdram, read32(rdram, kOptionsPtr));
                if (trace())
                    std::fprintf(stderr, "[pc settings] the options reset to the defaults: the stored ones written again\n");
            }
        }

        // The button layout screen's input handler: its Back went to the game's Controller Settings screen, whose options are in
        // Controls now. From the Controls menu it goes back to the Options list with that menu open instead.
        constexpr uint32_t kLayoutInput = TF_GA(0x00384DE0u);
        PS2Runtime::RecompiledFunction g_layoutOriginal = nullptr;

        void layoutInputHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc != kLayoutInput || !g_layoutReturn || getRegU32(ctx, 5) != kInputBack)
            {
                g_layoutOriginal(rdram, ctx, runtime);
                return;
            }
            const uint32_t self = getRegU32(ctx, 4);
            g_layoutReturn = false;
            g_controlsOnShow = true;
            const uint32_t owner = callGuest(rdram, ctx, runtime, kScreens, 0u, 0u);
            callGuestNamed(rdram, ctx, runtime, kChangeScreen, owner, kOptionsName, 1u);
            callGuest(rdram, ctx, runtime, kCloseScreen, self, 3u);
            if (trace())
                std::fprintf(stderr, "[pc settings] the button layout screen: back to the Options list, Controls open\n");
            SET_GPR_U32(ctx, 5, 0u); // (the game's own handler is given nothing: it would send the screen on to Controller Settings)
            g_layoutOriginal(rdram, ctx, runtime);
        }

        // The game's audio screen in game (taScreenAudioOptions, "InGAudioSettings") has five rows: Music and SFX, and then the game's own Invert
        // Y-axis, Vibration and Look Spring. Those three are in Controls > Gamepad now: after its load function (0x386040: the labels and the
        // count) the screen holds the two sound rows only. The screen's other code reads the three rows' values and gives them back
        // unchanged.
        constexpr uint32_t kAudioLoadFn = TF_GA(0x00386040u), kAudioNames = 0x6F4u; // (the third row's first text)
        PS2Runtime::RecompiledFunction g_audioLoadOriginal = nullptr;

        void audioLoadHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const bool entry = ctx->pc == kAudioLoadFn;
            const uint32_t self = getRegU32(ctx, 4), returnTo = getRegU32(ctx, 31);
            g_audioLoadOriginal(rdram, ctx, runtime);
            if (entry && ctx->pc == returnTo && inRam(self) && read32(rdram, self + kCount) == 5u &&
                textIs(rdram, self + kAudioNames, "KEY_INVERTYAXIS", 15u))
            {
                write32(rdram, self + kCount, 2u);
                if (trace())
                    std::fprintf(stderr, "[pc settings] the game's audio screen: Invert Y-axis, Vibration and Look Spring are not on it (they are in Controls)\n");
            }
        }

        // The HQ's Options screen comes back by being SHOWN again (taScreenOptions::vfunc_09), not rebuilt: its rows are still ours.
        // After the button layout screen the Controls menu is opened there.
        constexpr uint32_t kHqShowFn = TF_GA(0x00386260u);
        PS2Runtime::RecompiledFunction g_hqShowOriginal = nullptr;

        // ---- The notice under the page title: "REQUIRES RESTART", in red, while a change on the page waits for the next start ----
        // The list screen draws itself in two ways (taScreenInGMenu::RenderAsPreHQ, the text-only list of the Start menu's screens, and
        // RenderAsInGame, the list with bars of the HQ and the pause menu). In the text-only list the notice is the first row of the
        // page (the title is counted up from the rows, so the line needs a row of its own): a row the selection skips, which the game
        // draws in the style it keeps for its greyed rows, set red here before its draw (setGreyedColour). In the list with bars the
        // line is drawn after the screen's own draw, with the game's own text functions (set a style's size and colour, bind it to a
        // text, measure, draw at a place), on a tile of its own: a copy of the first row's label bar, one row up, drawn the way the
        // game draws its bars (a dark base with the first texture, a light one over it with the third; each bar is two rects) in red
        // instead of blue. The greyed rows' style (no row of these pages is greyed) is put back as it was.
        constexpr uint32_t kRenderFn[2] = {0x003136C0u, 0x00313F30u};
        constexpr uint32_t kTextBind = 0x00233090u, kTextSetColour = 0x00232670u, kTextSetSize = 0x002326B0u, kTextDraw = 0x00233330u,
                           kTextWidth = 0x002330F0u;
        constexpr uint32_t kRowTextSize = 0x006CB1DCu, kGreyedColour = 0x0063B300u; // (the game's globals: the rows' size, the greyed colour)
        constexpr uint32_t kText2dGreyed = 0x798u, kFormatGreyed = 0x79Cu, kVertexSize = 20u;
        constexpr uint32_t kRowBars = 0x100u, kRowBarSize = 80u, kTextures = 0x770u; // (the label bars, 4 vertices each; the three textures)
        constexpr uint32_t kRectFn = 0x001F8390u, kSetTextureFn = 0x001F8330u, kSetBlendFn = 0x001F8800u;
        constexpr uint32_t kTileDark = 0xFFAA1E1Eu, kTileLight = 0xFFEB5A50u; // (the game's bar colours, 0xFF23669B and 0xFF73B6EB, are blue)
        PS2Runtime::RecompiledFunction g_renderOriginal[2] = {nullptr, nullptr};

        float readF(const uint8_t *rdram, uint32_t address)
        {
            const uint32_t v = read32(rdram, address);
            float f;
            std::memcpy(&f, &v, sizeof(f));
            return f;
        }
        void writeF(uint8_t *rdram, uint32_t address, float f)
        {
            uint32_t v;
            std::memcpy(&v, &f, sizeof(v));
            write32(rdram, address, v);
        }

        // The text-only list (the Start menu's screens): the notice is the first row of the page, skipped by the selection, and the game
        // draws a skipped row in the style it keeps for its greyed rows. That style is red on our pages with the notice row, and the
        // game's own colour everywhere else (set at each draw: a save state may have held it red).
        void setGreyedColour(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t self, bool red)
        {
            const uint32_t text2d = read32(rdram, self + kText2dGreyed), format = read32(rdram, self + kFormatGreyed);
            if (!inRam(text2d) || !inRam(format) || !runtime)
                return;
            const uint32_t frame = getRegU32(ctx, 29) - 32u;
            if (!inRam(frame))
                return;
            const R5900Context saved = *ctx;
            SET_GPR_U32(ctx, 29, frame);
            uint32_t colour = kGreyedColour;
            if (red)
            {
                // (the colour is alpha, red, green, blue as floats: the game's own order)
                writeF(rdram, frame + 0u, 1.0f);
                writeF(rdram, frame + 4u, 1.0f);
                writeF(rdram, frame + 8u, 0.12f);
                writeF(rdram, frame + 12u, 0.12f);
                colour = frame;
            }
            callGuest(rdram, ctx, runtime, kTextSetColour, format, colour);
            callGuest(rdram, ctx, runtime, kTextBind, text2d, format); // (the text takes the style's colour when it is bound to it)
            *ctx = saved;
        }

        // The list with bars (the HQ and the pause menu): the notice on a red tile under the title, drawn after the screen's own draw.
        void drawNotice(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t self)
        {
            const uint32_t text2d = read32(rdram, self + kText2dGreyed), format = read32(rdram, self + kFormatGreyed);
            if (!inRam(text2d) || !inRam(format) || !runtime)
                return;
            const R5900Context saved = *ctx;
            const uint32_t returnTo = getRegU32(&saved, 31);
            const uint32_t frame = getRegU32(ctx, 29) - 256u; // (a frame of the guest stack for the colour, the place, the text and the tile)
            uint8_t *p = inRam(frame) ? getMemPtr(rdram, frame) : nullptr;
            if (!p)
                return;
            SET_GPR_U32(ctx, 29, frame);
            const auto call = [&](uint32_t address, uint32_t a0, uint32_t a1, uint32_t a2, float f12) -> bool
            {
                if (!runtime->hasFunction(address))
                    return false;
                const PS2Runtime::RecompiledFunction fn = runtime->lookupFunction(address);
                if (!fn)
                    return false;
                SET_GPR_U32(ctx, 4, a0);
                SET_GPR_U32(ctx, 5, a1);
                SET_GPR_U32(ctx, 6, a2);
                SET_GPR_U32(ctx, 31, returnTo);
                ctx->f[12] = f12;
                ctx->pc = address;
                fn(rdram, ctx, runtime);
                return ctx->pc == returnTo;
            };
            const float rowSize = readF(rdram, kRowTextSize);
            const float size = rowSize * 0.9f; // (a little smaller than a row's text, white, on the tile)
            // (the colour is alpha, red, green, blue as floats: the game's own order)
            writeF(rdram, frame + 0u, 1.0f);
            writeF(rdram, frame + 4u, 1.0f);
            writeF(rdram, frame + 8u, 1.0f);
            writeF(rdram, frame + 12u, 1.0f);
            static const char kText[] = "REQUIRES RESTART";
            std::memcpy(p + 32, kText, sizeof(kText));
            bool ok = call(kTextSetColour, format, frame + 0u, 0u, 0.0f) && call(kTextSetSize, format, 0u, 0u, size) &&
                      call(kTextBind, text2d, format, 0u, 0.0f) && call(kTextWidth, text2d, frame + 32u, 0u, 0.0f);
            if (ok)
            {
                const float width = ctx->f[0];
                float x, y;
                {
                    // the first row's label bar copied one row up (the step is the first row to the second), red
                    const uint32_t count = read32(rdram, self + kCount);
                    const uint32_t row0 = self + kRowBars;
                    const float step = count >= 2 ? readF(rdram, row0 + kRowBarSize + 4u) - readF(rdram, row0 + 4u) : 41.0f;
                    const uint32_t tile = frame + 128u;
                    for (uint32_t i = 0; i < kRowBarSize; i += 4)
                        write32(rdram, tile + i, read32(rdram, row0 + i));
                    for (uint32_t v = 0; v < 4; ++v)
                        writeF(rdram, tile + v * kVertexSize + 4u, readF(rdram, tile + v * kVertexSize + 4u) - step);
                    const auto colourTile = [&](uint32_t colour)
                    {
                        for (uint32_t v = 0; v < 4; ++v)
                            write32(rdram, tile + v * kVertexSize + 8u, colour);
                    };
                    bool drawn = call(kSetBlendFn, 1u, 0u, 0u, 0.0f);
                    colourTile(kTileDark);
                    drawn = drawn && call(kSetTextureFn, read32(rdram, self + kTextures + 0u), 0u, 0u, 0.0f) &&
                            call(kRectFn, tile, tile + kVertexSize, 0u, 0.0f) &&
                            call(kRectFn, tile + 2u * kVertexSize, tile + 3u * kVertexSize, 0u, 0.0f);
                    colourTile(kTileLight);
                    drawn = drawn && call(kSetTextureFn, read32(rdram, self + kTextures + 8u), 0u, 0u, 0.0f) &&
                            call(kRectFn, tile, tile + kVertexSize, 0u, 0.0f) &&
                            call(kRectFn, tile + 2u * kVertexSize, tile + 3u * kVertexSize, 0u, 0.0f);
                    // (the text as the rows' labels are drawn: right-aligned in the bar, a little above its top)
                    x = readF(rdram, tile + kVertexSize + 0u) - width;
                    y = readF(rdram, tile + 4u) - 9.0f;
                    if (!drawn && trace())
                        std::fprintf(stderr, "[pc settings] the notice tile was not drawn\n");
                }
                writeF(rdram, frame + 16u, x);
                writeF(rdram, frame + 20u, y);
                ok = call(kTextDraw, text2d, frame + 32u, frame + 16u, 0.0f);
            }
            // (the style of the greyed rows as the screen had it)
            call(kTextSetColour, format, kGreyedColour, 0u, 0.0f);
            call(kTextSetSize, format, 0u, 0u, rowSize);
            *ctx = saved;
        }

        template <int I> void renderHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const bool entry = ctx->pc == kRenderFn[I];
            const uint32_t self = getRegU32(ctx, 4);
            const uint32_t returnTo = getRegU32(ctx, 31);
            if (I == 0 && entry && inRam(self))
                setGreyedColour(rdram, ctx, runtime, self, g_pc && self == g_self && !g_cur.rows.empty() && isNoticeRow(g_cur.rows[0]));
            g_renderOriginal[I](rdram, ctx, runtime);
            if (I == 0 || !entry || ctx->pc != returnTo || !g_noticeWanted || !g_pc || self != g_self || g_page == kRestartPage ||
                !inRam(self))
                return;
            drawNotice(rdram, ctx, runtime, self);
        }

        void hqShowHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const bool entry = ctx->pc == kHqShowFn;
            const uint32_t self = getRegU32(ctx, 4);
            g_hqShowOriginal(rdram, ctx, runtime);
            Host &host = g_hosts[2];
            if (!entry || !g_controlsOnShow || !inRam(self) || read32(rdram, self) != host.vtable)
                return;
            g_controlsOnShow = false;
            g_pc = false;
            g_stack.clear();
            write32(rdram, self + kSelected, 3u);
            enterPages(rdram, self, host, kControlsHqPage, 2);
            if (trace())
                std::fprintf(stderr, "[pc settings] the HQ Options shown again: the Controls menu opened\n");
        }

        void startInput(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { processInput(g_hosts[0], rdram, ctx, runtime); }
        void startOptionsInput(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { processInput(g_hosts[1], rdram, ctx, runtime); }
        void optionsInput(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { processInput(g_hosts[2], rdram, ctx, runtime); }
        void pauseInput(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { processInput(g_hosts[3], rdram, ctx, runtime); }
        void startOptionsInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { initHook(g_hosts[1], rdram, ctx, runtime); }
        void optionsInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { initHook(g_hosts[2], rdram, ctx, runtime); }
        void pauseInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { initHook(g_hosts[3], rdram, ctx, runtime); }

        // The string table's lookup: our keys get their label. The table does not have them, and a key it does not have is
        // answered with the key itself, copied; so for one of our keys the lookup is given the LABEL as its key, and its answer
        // is the label, in a string the game's own code made (inline, or in the game's heap: any length).
        //
        // The label has to be a guest std::string for that one call. It is built in a frame of the guest stack:
        //     sp -> +0x00  kFrameMark0, kFrameMark1, the caller's return address, sp ^ kFrameMark1   (what tells the frame)
        //           +0x10  the std::string (12 bytes, the long form), the result's address
        //           +0x20  the characters (kMaxText + 1 at most)
        // and the lookup is entered with the stack pointer at the frame and ITS OWN ADDRESS as the return address. When it
        // returns, this hook is entered again (the function table has it at that address), finds the frame and leaves it: the
        // stack pointer back, the caller's return address taken. Everything the second half needs is in guest registers and
        // guest memory, the resumable way (guest_code.h): a lookup stopped at a checkpoint goes on later and still ends here,
        // also in another session that loaded a state made in between (a save state is made at a checkpoint, wherever the game
        // thread is: F6 can land inside a lookup). For that the hook is registered whatever TF_PC_SETTINGS_ROW says (install()).
        // A hook registered over this one (the Decepticon HQ's) is entered first with the lookup's address and no arguments
        // when a stopped lookup returns: it must do what that one does - check what it reads, then pass the call on.
        //   - "returned" is told from "entered" by the frame AND the return address register holding the lookup's address AND
        //     the result no longer holding the mark put there before the call (the lookup zeroes it first thing);
        //   - nothing is written after the call but registers and the frame's own words (the mark in the result is cleared when
        //     the call has ended): the label needs no patching.
        constexpr uint32_t kFrameSize = 0x80u, kFrameMark0 = 0x50435345u, kFrameMark1 = 0x54464D45u;
        constexpr uint32_t kFrameString = 0x10u, kFrameResult = 0x1Cu, kFrameChars = 0x20u;
        constexpr uint32_t kResultMark = 0xFFFFFFFDu; // (a string's first word the game cannot have left: long, capacity 2^31 - 2)

        bool lookupFrame(const uint8_t *rdram, uint32_t sp)
        {
            return inRam(sp) && read32(rdram, sp) == kFrameMark0 && read32(rdram, sp + 4u) == kFrameMark1 &&
                   read32(rdram, sp + 12u) == (sp ^ kFrameMark1);
        }

        // The lookup has returned into our frame: true, and the frame is left (the caller goes on).
        bool lookupReturned(uint8_t *rdram, R5900Context *ctx)
        {
            const uint32_t sp = getRegU32(ctx, 29);
            if (ctx->pc != kStringLookup || getRegU32(ctx, 31) != kStringLookup || !lookupFrame(rdram, sp))
                return false;
            const uint32_t result = read32(rdram, sp + kFrameResult);
            if (read32(rdram, result) == kResultMark)
                return false; // (entered, not run yet: the call was stopped before its first instruction)
            const uint32_t back = read32(rdram, sp + 8u);
            write32(rdram, sp, 0u); // (the frame is no frame any more)
            SET_GPR_U32(ctx, 29, sp + kFrameSize);
            SET_GPR_U32(ctx, 31, back);
            ctx->pc = back;
            return true;
        }

        uint64_t g_lookups = 0u, g_lookupsStopped = 0u, g_lookupsResumed = 0u; // (the trace's counts)

        void lookupHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc != kStringLookup)
            {
                // (A call resumed in its middle. The function table has this hook at the lookup's first address only, so the
                // scheduler resumes the original itself and this is not expected; should it end in our frame, the frame is left.)
                g_lookup(rdram, ctx, runtime);
                lookupReturned(rdram, ctx);
                return;
            }
            if (lookupReturned(rdram, ctx))
            {
                // (the second half on its own: the lookup, stopped and resumed by the scheduler, came back here)
                ++g_lookupsResumed;
                if (trace())
                    std::fprintf(stderr, "[pc settings] a stopped lookup ended: back to %08x (%llu of %llu stopped, %llu ended)\n", ctx->pc,
                                 static_cast<unsigned long long>(g_lookupsStopped), static_cast<unsigned long long>(g_lookups),
                                 static_cast<unsigned long long>(g_lookupsResumed));
                return;
            }
            const uint32_t sp = getRegU32(ctx, 29);
            if (getRegU32(ctx, 31) == kStringLookup && lookupFrame(rdram, sp))
            {
                g_lookup(rdram, ctx, runtime); // (our call, stopped before its first instruction: entered again as it was)
                lookupReturned(rdram, ctx);
                return;
            }
            // (With the pages off - g_on false - only the two cases above are this hook's: no key is ours.)
            const uint32_t result = getRegU32(ctx, 4), key = getRegU32(ctx, 5);
            const std::string *text = g_on && inRam(key) && inRam(result) ? ourText(rdram, key) : nullptr;
            const uint32_t frame = sp - kFrameSize;
            uint8_t *p = text && inRam(frame) ? getMemPtr(rdram, frame) : nullptr;
            if (!p || text->size() > kMaxText)
            {
                // (a development aid, TF_PC_SETTINGS_TRACE_KEYS=1: the game's own keys looked up while the pages are up, with their texts)
                static const bool keysTrace = std::getenv("TF_PC_SETTINGS_TRACE_KEYS") != nullptr;
                std::string name;
                if (keysTrace && !text && inRam(key))
                {
                    const uint32_t chars = charsOf(rdram, key);
                    if (const uint8_t *k = inRam(chars) ? getConstMemPtr(rdram, chars) : nullptr)
                        name.assign(reinterpret_cast<const char *>(k), strnlen(reinterpret_cast<const char *>(k), 80));
                }
                const uint32_t returnTo = getRegU32(ctx, 31);
                g_lookup(rdram, ctx, runtime);
                static std::set<std::string> seen;
                if (!name.empty() && ctx->pc == returnTo && seen.insert(name).second)
                {
                    std::string shown;
                    const uint32_t chars = charsOf(rdram, result);
                    if (const uint8_t *r = inRam(chars) ? getConstMemPtr(rdram, chars) : nullptr)
                        for (size_t i = 0; i < 100 && r[i]; ++i)
                        {
                            char buf[8];
                            if (r[i] >= 0x20 && r[i] < 0x7F)
                                shown.push_back(static_cast<char>(r[i]));
                            else
                            {
                                std::snprintf(buf, sizeof(buf), "<%02X>", r[i]);
                                shown += buf;
                            }
                        }
                    std::fprintf(stderr, "[pc settings keys] \"%s\" -> \"%s\"\n", name.c_str(), shown.c_str());
                }
                return;
            }
            const uint32_t length = static_cast<uint32_t>(text->size());
            std::memset(p, 0, kFrameSize);
            write32(rdram, frame, kFrameMark0);
            write32(rdram, frame + 4u, kFrameMark1);
            write32(rdram, frame + 8u, getRegU32(ctx, 31));
            write32(rdram, frame + 12u, frame ^ kFrameMark1);
            write32(rdram, frame + kFrameString, (length << 1) | 1u); // (the long form: capacity << 1 | 1, the size, the characters)
            write32(rdram, frame + kFrameString + 4u, length);
            write32(rdram, frame + kFrameString + 8u, frame + kFrameChars);
            write32(rdram, frame + kFrameResult, result);
            std::memcpy(p + kFrameChars, text->c_str(), length + 1u);
            write32(rdram, result, kResultMark);
            SET_GPR_U32(ctx, 29, frame);
            SET_GPR_U32(ctx, 5, frame + kFrameString);
            SET_GPR_U32(ctx, 31, kStringLookup);
            ++g_lookups;
            g_lookup(rdram, ctx, runtime);
            const bool returned = lookupReturned(rdram, ctx);
            if (!returned)
                ++g_lookupsStopped;
            if (trace())
            {
                if (g_lookups % 5000u == 0u)
                    std::fprintf(stderr, "[pc settings] %llu label lookups so far, %llu stopped on the way, %llu of those ended\n",
                                 static_cast<unsigned long long>(g_lookups), static_cast<unsigned long long>(g_lookupsStopped),
                                 static_cast<unsigned long long>(g_lookupsResumed));
                if (!returned)
                    std::fprintf(stderr, "[pc settings] the lookup of \"%s\" was stopped at %08x: it ends later\n", text->c_str(), ctx->pc);
                else if (!textIs(rdram, result, text->c_str(), text->size()))
                    std::fprintf(stderr, "[pc settings] the lookup of \"%s\" answered something else (a key the table has?)\n", text->c_str());
                else if (std::getenv("TF_PC_SETTINGS_TRACE_LOOKUPS"))
                    std::fprintf(stderr, "[pc settings] lookup \"%s\" (frame %08x, result %08x, word0 %08x)\n", text->c_str(), frame, result,
                                 read32(rdram, result));
            }
        }
    }

    void install(PS2Runtime &runtime)
    {
        // On by default; TF_PC_SETTINGS_ROW=0 turns it off (the original game menus alone).
        const char *on = std::getenv("TF_PC_SETTINGS_ROW");
        const bool off = on && *on == '0';
        if (!runtime.hasFunction(kStringLookup) || (g_lookup = runtime.lookupFunction(kStringLookup)) == nullptr)
        {
            if (!off)
                std::fprintf(stderr, "[pc settings] the string lookup is not in this build: the row is off\n");
            return;
        }
        // The lookup's hook is registered at every start, whatever the setting says (guest_code.h's rule for added functions,
        // for the same reason): a save state made with the pages on may hold a label's lookup half done, which returns into
        // the hook's frame (lookupHook). With the pages off the hook does that second half and nothing else.
        runtime.registerFunction(kStringLookup, lookupHook);
        if (off)
            return;
        g_runtime = &runtime;
        g_on = true;
        // Every text the pages have, registered now: a key in a loaded save state (a row added in another session) has its text.
        keyFor("QUIT GAME");
        keyFor(kNoticeText);
        keyFor(" ");
        for (const Extra &e : kExtras)
            keyFor(e.label);
        for (const Page &page : pages())
        {
            keyFor(page.title);
            for (const Row &row : page.rows)
            {
                keyFor(row.label);
                for (const Opt &o : row.opts)
                    keyFor(o.label);
            }
        }
        // The game's options kept in settings.ini (see kStored): the settings, and the hooks that apply them.
        for (const StoredOption &o : kStored)
        {
            ps2_settings::Setting setting;
            setting.key = o.key;
            setting.section = "Controls";
            setting.label = o.label;
            setting.help = "The game's own option, kept here so a restart does not forget it. keep = what the save has.";
            setting.apply = [](const std::string &) { return true; }; // (the game's own setters apply it: nothing waits for a restart)
            setting.choices = {{"keep", "As the save has it"}, {o.first, o.first}, {o.second, o.second}};
            setting.defaultValue = "keep";
            ps2_settings::add(setting);
        }
        {
            ps2_settings::Setting setting;
            setting.key = "TF_OPT_SOUND_MODE";
            setting.section = "Sound";
            setting.label = "Sound mode";
            setting.help = "The game's own Sound Mode option, kept here so a restart does not forget it. keep = what the save has.";
            setting.apply = [](const std::string &) { return true; };
            setting.choices = {{"keep", "As the save has it"}, {"mono", "Mono"}, {"stereo", "Stereo"}, {"surround", "Surround"}};
            setting.defaultValue = "keep";
            ps2_settings::add(setting);
        }
        static const PS2Runtime::RecompiledFunction kInputHooks[] = {startInput, startOptionsInput, optionsInput, pauseInput};
        static const PS2Runtime::RecompiledFunction kInitHooks[] = {nullptr, startOptionsInit, optionsInit, pauseInit};
        for (size_t i = 0; i < std::size(g_hosts); ++i)
        {
            Host &h = g_hosts[i];
            if (!runtime.hasFunction(h.input) || (h.inputOriginal = runtime.lookupFunction(h.input)) == nullptr ||
                (h.init != 0u && (!runtime.hasFunction(h.init) || (h.initOriginal = runtime.lookupFunction(h.init)) == nullptr)))
            {
                std::fprintf(stderr, "[pc settings] the %s: an address is not in this build, its settings rows are not added there\n", h.name);
                h.inputOriginal = nullptr;
                continue;
            }
            runtime.registerFunction(h.input, kInputHooks[i]);
            if (h.init != 0u)
                runtime.registerFunction(h.init, kInitHooks[i]);
            std::fprintf(stderr, "[pc settings] the %s: the settings are on\n", h.name);
        }
        if (runtime.hasFunction(kLayoutInput) && (g_layoutOriginal = runtime.lookupFunction(kLayoutInput)) != nullptr)
            runtime.registerFunction(kLayoutInput, layoutInputHook);
        if (runtime.hasFunction(kHqShowFn) && (g_hqShowOriginal = runtime.lookupFunction(kHqShowFn)) != nullptr)
            runtime.registerFunction(kHqShowFn, hqShowHook);
        if (runtime.hasFunction(kRenderFn[0]) && (g_renderOriginal[0] = runtime.lookupFunction(kRenderFn[0])) != nullptr)
            runtime.registerFunction(kRenderFn[0], renderHook<0>);
        if (runtime.hasFunction(kRenderFn[1]) && (g_renderOriginal[1] = runtime.lookupFunction(kRenderFn[1])) != nullptr)
            runtime.registerFunction(kRenderFn[1], renderHook<1>);
        if (runtime.hasFunction(kAudioLoadFn) && (g_audioLoadOriginal = runtime.lookupFunction(kAudioLoadFn)) != nullptr)
            runtime.registerFunction(kAudioLoadFn, audioLoadHook);
        // (the bars' slide between pages; another hook sits on it too, this one is chained to whatever is there)
        if (runtime.hasFunction(kSetBarPositions) && (g_barPositionsOriginal = runtime.lookupFunction(kSetBarPositions)) != nullptr)
            runtime.registerFunction(kSetBarPositions, barPositionsHook);
        std::fprintf(stderr, "[pc settings] %zu labels (no added guest data: the save states' layout is the game's without the pages)\n",
                     g_texts.size());
    }

    // After the port's own hooks on the options' functions (overrides.cpp registers them after install()): these sit on top of
    // them, so the stored options are applied last. The profile's options are copied (0x32EE40) and applied (0x2780E0) when
    // a save is loaded; a new game or a start with no save runs the default-setters.
    void installLate(PS2Runtime &runtime)
    {
        if (!g_on) // (install() did not go ahead: the menus are off)
            return;
        if (runtime.hasFunction(kCopyOptionsFn) && (g_copyOptionsOriginal = runtime.lookupFunction(kCopyOptionsFn)) != nullptr)
            runtime.registerFunction(kCopyOptionsFn, copyOptionsHook);
        if (runtime.hasFunction(kApplyProfileOptionsFn) && (g_applyProfileOriginal = runtime.lookupFunction(kApplyProfileOptionsFn)) != nullptr)
            runtime.registerFunction(kApplyProfileOptionsFn, applyProfileHook);
        if (runtime.hasFunction(kDefaultsFn[0]) && (g_defaultsOriginal[0] = runtime.lookupFunction(kDefaultsFn[0])) != nullptr)
            runtime.registerFunction(kDefaultsFn[0], defaultsHook<0>);
        if (runtime.hasFunction(kDefaultsFn[1]) && (g_defaultsOriginal[1] = runtime.lookupFunction(kDefaultsFn[1])) != nullptr)
            runtime.registerFunction(kDefaultsFn[1], defaultsHook<1>);
        if (runtime.hasFunction(kResetFn[0]) && (g_resetOriginal[0] = runtime.lookupFunction(kResetFn[0])) != nullptr)
            runtime.registerFunction(kResetFn[0], resetHook<0>);
        if (runtime.hasFunction(kResetFn[1]) && (g_resetOriginal[1] = runtime.lookupFunction(kResetFn[1])) != nullptr)
            runtime.registerFunction(kResetFn[1], resetHook<1>);
        std::fprintf(stderr, "[pc settings] the stored game options are applied after the profile's and the defaults\n");
    }

    // A save state was loaded: the screens are the state's. What this side remembers of the pages (which screen they were
    // on, its own rows kept aside, the page stack) belongs to the session before the load and is dropped: kept, the next input
    // would put the old page over the state's screen and later write the old rows - their strings' addresses - back into it.
    void stateLoaded()
    {
        if (trace() && g_on)
            std::fprintf(stderr, "[pc settings] a save state was loaded%s\n", g_pc ? ": the pages' session state is dropped" : "");
        g_pc = false;
        g_lock = false;
        g_host = nullptr;
        g_self = 0u;
        g_stack.clear();
        g_save.clear();
        stopSlide(); // (a slide in progress was of the session before the state)
        g_positionState = 0;
        g_layoutReturn = false;
        g_controlsOnShow = false;
        g_closeSelf = 0u; // (a close still pending belonged to the screens before the load)
        g_pagesUp.store(false);
        g_closeTries = 0;
        g_noticeWanted = false;
    }

    bool pausePagesUp() { return g_pc && g_host && g_host->kind == kPauseMenu; }

    void pump()
    {
#if defined(PS2X_ENABLE_DEBUG_UI)
        // The registry's settings, copied for the game thread: at the first frame, then now and then (a setting added later, a
        // choice list that depends on the window).
        if (g_on)
        {
            static uint32_t frame = 0;
            if (frame++ % (g_pagesUp.load() ? 6u : 300u) == 0u)
            {
                const auto rows = ps2_settings::detail::rows();
                std::map<std::string, std::vector<mod_menu::MergedOption>> merged;
                for (const char *id : kMergedIds)
                    merged[id] = mod_menu::mergedOptions(id);
                std::lock_guard<std::mutex> lock(g_infoLock);
                g_merged = std::move(merged);
                g_info.clear();
                for (const ps2_settings::detail::Row &r : rows)
                {
                    Info info;
                    info.label = r.label;
                    info.available = r.available;
                    info.pending = r.pending;
                    for (const ps2_settings::Choice &c : r.choices)
                        info.choices.emplace_back(c.value, c.label);
                    g_info[r.key] = std::move(info);
                }
                static bool dumped = false;
                if (!dumped && frame > 600u && std::getenv("TF_PC_SETTINGS_DUMP"))
                {
                    dumped = true;
                    for (const auto &[key, info] : g_info)
                    {
                        std::fprintf(stderr, "[pc settings dump] %s | %s | avail %d |", key.c_str(), info.label.c_str(), info.available ? 1 : 0);
                        for (const auto &[value, label] : info.choices)
                            std::fprintf(stderr, " [%s=%s]", value.c_str(), label.c_str());
                        std::fprintf(stderr, "\n");
                    }
                }
            }
        }
        if (g_restartAsked.exchange(false) && g_runtime && !ps2_settings::restarting())
        {
            if (trace())
                std::fprintf(stderr, "[pc settings] Restart to apply: F1's own restart is started (the game is saved and started again)\n");
            ps2_settings::restartNow(*g_runtime);
        }
        std::vector<std::pair<std::string, std::string>> now;
        {
            std::lock_guard<std::mutex> lock(g_queueLock);
            if (g_queue.empty())
                return;
            now.swap(g_queue);
        }
        const auto rows = ps2_settings::detail::rows();
        for (const auto &[key, value] : now)
        {
            bool found = false;
            for (const ps2_settings::detail::Row &r : rows)
                if (r.key == key)
                {
                    ps2_settings::detail::choose(r.entry, value);
                    found = true;
                    if (trace())
                    {
                        // (read back: did the registry take it? "now" is what it holds after the choice)
                        const std::string got = ps2_settings::value(key.c_str());
                        std::fprintf(stderr, "[pc settings] %s = \"%s\" (window thread), the registry now holds \"%s\"%s\n", key.c_str(),
                                     value.c_str(), got.c_str(), got == value ? "" : "  <-- NOT THE SAME");
                    }
                    break;
                }
            if (!found && trace())
                std::fprintf(stderr, "[pc settings] %s: NOT A SETTING in this build\n", key.c_str());
        }
#endif
    }
}
