// The mod menu (F1): mod_menu.h has the long form.

#include "mod_menu.h"

#include "camera_mode.h"
#include "hd_progress.h"
#include "photo_mode.h"
#include "menu_style.h"
#include "menu_mouse.h"
#include "pad_speaker.h"
#include "small_crosshair.h"
#include "boss_cheat.h"
#include "distance_meter.h"
#include "port_ext.h"
#include "dev_tree.h"
#include "speedrun_timer.h"
#include "start_screens.h"
#include "game_config.h"
#include "save_io.h"
#include "state_manager.h"
#include "updates.h"

#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_memory.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iterator>
#if defined(_WIN32)
#include <process.h>
#endif
#include <mutex>
#include <sstream>
#include <chrono>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(PS2X_ENABLE_DEBUG_UI)
#include "imgui.h"
#include "raylib.h"
#include "rlgl.h"
#include "rlImGui.h"
#include "runtime/ps2_sony_pad.h"
#include "ps2_runtime.h"
#include "runtime/ps2_host_display.h"
#include "runtime/ps2_host_input.h"
#include "runtime/ps2_settings.h"
// The settings' registry as F5's panel reads it (rows, choose): the port's own, not a public header.
#include "../../PS2Recomp/ps2xRuntime/src/lib/ps2_settings_internal.h"
#endif

namespace fs = std::filesystem;

// profiler.cpp (Windows): the overlay's characters and wheel from the window's own messages (TODO 170 / 174), Windows'
// scroll lines a notch, and the tests' typed keys.
bool tfOverlayInputHook();
void tfOverlayTakeInput(std::vector<uint32_t> &chars, std::vector<int> &keys, float &wheelX, float &wheelY);
int tfWheelScrollLines();
void tfPostTestKey(int vk, bool down);

namespace mod_menu
{
    namespace
    {
        // The controller layout used when settings.ini has none: on a Steam Deck the triggers fire and jump (its
        // bumpers are hard to reach in a hand-held grip), elsewhere each game button on its own controller button.
        const char *padDefault()
        {
#if defined(__linux__)
            static const bool deck = []
            {
                std::ifstream in("/sys/class/dmi/id/sys_vendor");
                std::string vendor;
                return std::getline(in, vendor) && vendor == "Valve";
            }();
            if (deck)
                return "l1=l2;l2=l1;r1=r2;r2=r1"; // (as "Triggers fire")
#endif
            return "";
        }

        // ---------------------------------------------------------------------------------
        // The 32 cheat codes (docs/CHEATS.md of github.com/kayliox/transformers-armada-ps2-mods:
        // decoded from the game's own table, the same in the PAL and USA releases).

        struct Cheat
        {
            const char *screen;
            const char *code; // the buttons, in order
            const char *does;
        };

        const std::vector<Cheat> &cheats()
        {
            static const std::vector<Cheat> list = {
                {"Difficulty screen", "Left, Left, Right, L1, L2, Left, Left, Right", "Amazon Basin's Mini-Cons and warp gates; start at its last gate"},
                {"Difficulty screen", "L1, L1, L2, Square, Square, Circle, R1, R2", "As above, with Amazon Basin completed"},
                {"Difficulty screen", "L1, Left, L2, Right, Square, Square, Circle, Circle", "Adds Antarctica's Mini-Cons and gates; start at its last gate"},
                {"Difficulty screen", "R1, R1, R2, L2, L1, L1, R1, R1", "As above, with Antarctica completed"},
                {"Difficulty screen", "Square, Circle, Circle, R1, L1, Left, Left, Right", "Adds Deep Amazon; start at the antechamber before the boss"},
                {"Difficulty screen", "Left, Right, Left, Left, Right, R1, R2, Circle", "As above, with Deep Amazon completed"},
                {"Difficulty screen", "L2, Left, Right, Right, Left, L2, L1, L2", "Adds Mid-Atlantic's Mini-Cons and gates"},
                {"Difficulty screen", "Circle, Square, Circle, Square, Right, Left, Left, Right", "As above, with Mid-Atlantic completed, so Alaska starts from its beginning"},
                {"Difficulty screen", "R1, R1, R2, L1, L1, L2, Circle, Circle", "Adds Alaska; start at the Cliff-Top gate near the end of the level"},
                {"Difficulty screen", "R1, Circle, R1, Square, Left, Left, Right, Left", "As above, with Alaska completed"},
                {"Difficulty screen", "Right, Right, Square, R1, R2, Circle, Left, Left", "Adds the Starship's Mini-Cons and gates"},
                {"Difficulty screen", "Left, Left, Right, Circle, Circle, Right, Right, Left", "As above, with the Starship completed"},
                {"Difficulty screen", "L1, R1, L2, R2, Left, Left, Right, Square", "Adds Pacific Island; start at the Volcano gate before the boss"},
                {"Difficulty screen", "R2, R1, L1, L2, Square, Circle, Square, Circle", "As above, with Pacific Island completed (Earth complete)"},
                {"Difficulty screen", "R2, R2, L1, L2, R2, R1, L2, L2", "Everything: every level up to Pacific Island and every Mini-Con"},
                {"Extras menu", "Square, Square, Circle, Square, Square, Circle, L1, L2", "Unlock all extras"},
                {"Extras menu", "Circle, Square, Circle, Square, L1, L1, L1, Square", "Unlock the movies"},
                {"Extras menu", "Circle, Square, Circle, Square, L1, R1, L1, Circle", "Unlock the art"},
                {"Extras menu", "Circle, Square, Circle, Square, R1, L1, L1, L1", "Unlock the music"},
                {"HQ", "L1, L2, Circle, Circle, Square, Circle, L2, L1", "Unlock all Mini-Cons"},
                {"HQ", "R1, R1, L2, R1, R1, L2, Circle, Circle", "No Mini-Con overload: equip any four Mini-Cons"},
                {"HQ", "Up, Up, Down, Down, L1, L2, L1, L2", "Infinite stealth (with the Convert Mini-Con)"},
                {"HQ", "Up, Up, Square, Circle, Square, Square, L1, L2", "Infinite dash"},
                {"HQ", "L2, L2, Square, Circle, Square, R1, R1, R2", "Infinite tractor beam (with the Tractor Mini-Con)"},
                {"HQ", "Up, Down, Up, Down, Circle, Square, Square, Circle", "Infinite Powerlinx"},
                {"HQ", "Down, Up, Up, Down, R1, R2, R2, R1", "Super Mini-Con damage"},
                {"HQ", "Circle, Circle, Up, Down, R1, R2, Down, Down", "Glide camera tilt"},
                {"HQ", "Circle, Circle, Circle, Square, L1, L1, L1, L2", "Big head mode"},
                {"HQ", "L1, R2, R2, Square, Square, Square, Square, L1", "Turbo mode"},
                {"Pause menu", "R1, Circle, Circle, R1, R2, L1, L1, L2", "Invincibility (enter it again in each level)"},
                {"Pause menu", "Square, Circle, Square, Circle, L1, L1, L2, L1", "One-hit kills (enter it again in each level)"},
                {"Pause menu", "Left, Right, Left, R1, R2, R1, Right, Right", "Enemies invisible"},
            };
            return list;
        }

        // A button's bit, as TF_INPUT's padButton (overrides.cpp).
        uint16_t buttonBit(std::string name)
        {
            for (char &c : name)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            static const std::pair<const char *, uint16_t> kButtons[] = {
                {"select", 1u << 0}, {"l3", 1u << 1}, {"r3", 1u << 2}, {"start", 1u << 3},
                {"up", 1u << 4}, {"right", 1u << 5}, {"down", 1u << 6}, {"left", 1u << 7},
                {"l2", 1u << 8}, {"r2", 1u << 9}, {"l1", 1u << 10}, {"r1", 1u << 11},
                {"triangle", 1u << 12}, {"circle", 1u << 13}, {"cross", 1u << 14}, {"square", 1u << 15}};
            for (const auto &[button, mask] : kButtons)
                if (name == button)
                    return mask;
            return 0u;
        }

        std::vector<uint16_t> parseCode(const char *code)
        {
            std::vector<uint16_t> out;
            std::stringstream in(code);
            std::string part;
            while (std::getline(in, part, ','))
            {
                part.erase(std::remove_if(part.begin(), part.end(), [](unsigned char c)
                                          { return std::isspace(c) != 0; }),
                           part.end());
                if (const uint16_t bit = buttonBit(part))
                    out.push_back(bit);
            }
            return out;
        }

        // The code being entered: each button held kHold ticks, then released kGap ticks (ticks
        // are the console's VBlanks, 60 a second of game time: TF_INPUT's), so the game sees
        // every press however fast or slow it runs.
        constexpr uint64_t kHold = 6u, kGap = 6u, kLead = 4u;
        std::mutex g_codeLock;
        std::vector<uint16_t> g_code;
        uint64_t g_codeStart = 0u; // 0 = not started yet (the next frame picks the tick)
        bool g_codeActive = false;
        std::string g_codeName;

        void enterCode(const Cheat &cheat)
        {
            std::lock_guard<std::mutex> guard(g_codeLock);
            g_code = parseCode(cheat.code);
            g_codeStart = 0u;
            g_codeActive = !g_code.empty();
            g_codeName = cheat.does;
        }

#if defined(PS2X_ENABLE_DEBUG_UI)
        // rlImGui hands ImGui only the mouse's and modifier keys' changes (IsMouseButtonPressed /
        // Released), but the runtime polls the window's input every few milliseconds, several times
        // a drawn frame: a release between two frames was never seen, and a window being dragged
        // then followed the mouse for good. Their state, given before each ImGui frame, cannot be
        // missed (ImGui drops an event that repeats the state it has).
        void syncInputState()
        {
            if (!ImGui::GetCurrentContext())
                return;
            ImGuiIO &io = ImGui::GetIO();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, IsMouseButtonDown(MOUSE_BUTTON_LEFT));
            io.AddMouseButtonEvent(ImGuiMouseButton_Right, IsMouseButtonDown(MOUSE_BUTTON_RIGHT));
            io.AddMouseButtonEvent(ImGuiMouseButton_Middle, IsMouseButtonDown(MOUSE_BUTTON_MIDDLE));
            io.AddKeyEvent(ImGuiMod_Ctrl, IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL));
            io.AddKeyEvent(ImGuiMod_Shift, IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT));
            io.AddKeyEvent(ImGuiMod_Alt, IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT));
        }

        // (TODO 170 / 174) Typed characters and the wheel: raylib keeps only its last poll's (each PollInputEvents clears its
        // character queue and sets the wheel to that poll's move), and the runtime polls several times a drawn picture -
        // rlImGui, reading once a picture, lost wheel notches ("the wheel sometimes won't scroll") and characters. They
        // are taken from the window's own messages instead (tfOverlayTakeInput, every WM_CHAR / WM_MOUSEWHEEL), all of
        // them, once a picture. raylib's own characters are emptied before rlImGui would read them; the wheel goes on
        // top of what rlImGui gave (its last poll's), scaled to Windows' own scroll speed (SPI_GETWHEELSCROLLLINES lines a
        // notch, a line ImGui's text line; ImGui alone scrolls 5 font heights a notch).
        std::vector<uint32_t> g_typed;
        std::vector<int> g_keys; // (the text-editing keys' downs and ups: rlImGui sees a key's press or release only when it
                                 // falls in the last poll before a picture - Backspace, the arrows, Ctrl+C... were lost too)
        float g_wheelX = 0.0f, g_wheelY = 0.0f;
        void takeOverlayInput()
        {
            tfOverlayInputHook();
            g_typed.clear();
            g_keys.clear();
            tfOverlayTakeInput(g_typed, g_keys, g_wheelX, g_wheelY);
        }
        ImGuiKey imguiKeyOf(int vk)
        {
            switch (vk)
            {
            case 0x08: return ImGuiKey_Backspace;
            case 0x09: return ImGuiKey_Tab;
            case 0x0D: return ImGuiKey_Enter;
            case 0x1B: return ImGuiKey_Escape;
            case 0x21: return ImGuiKey_PageUp;
            case 0x22: return ImGuiKey_PageDown;
            case 0x23: return ImGuiKey_End;
            case 0x24: return ImGuiKey_Home;
            case 0x25: return ImGuiKey_LeftArrow;
            case 0x26: return ImGuiKey_UpArrow;
            case 0x27: return ImGuiKey_RightArrow;
            case 0x28: return ImGuiKey_DownArrow;
            case 0x2D: return ImGuiKey_Insert;
            case 0x2E: return ImGuiKey_Delete;
            case 'A': return ImGuiKey_A;
            case 'C': return ImGuiKey_C;
            case 'V': return ImGuiKey_V;
            case 'X': return ImGuiKey_X;
            case 'Y': return ImGuiKey_Y;
            case 'Z': return ImGuiKey_Z;
            default: return ImGuiKey_None;
            }
        }
        void dropRaylibCharacters()
        {
            while (GetCharPressed() != 0)
            {
            }
        }
        void feedOverlayInput()
        {
            ImGuiIO &io = ImGui::GetIO();
            for (const int k : g_keys)
                if (const ImGuiKey key = imguiKeyOf(k < 0 ? -k : k); key != ImGuiKey_None)
                    io.AddKeyEvent(key, k > 0);
            if (io.WantCaptureKeyboard)
                for (const uint32_t c : g_typed)
                    if (c >= 32u && c != 127u)
                        io.AddInputCharacter(c);
            const int lines = tfWheelScrollLines();
            const float line = ImGui::GetTextLineHeightWithSpacing(), font = ImGui::GetFontSize();
            const float scale = lines < 0 ? 100.0f : font > 0.0f ? static_cast<float>(lines) * line / (5.0f * font) : 1.0f;
            const Vector2 given = IsWindowFocused() ? GetMouseWheelMoveV() : Vector2{0.0f, 0.0f}; // (rlImGui's, this picture)
            const float x = g_wheelX * scale - given.x, y = g_wheelY * scale - given.y;
            if (x != 0.0f || y != 0.0f)
                io.AddMouseWheelEvent(x, y);
            static const bool log = std::getenv("TF_OVERLAY_INPUT_LOG") && *std::getenv("TF_OVERLAY_INPUT_LOG") == '1';
            if (log && (!g_typed.empty() || g_wheelY != 0.0f))
                std::fprintf(stderr, "[overlay input] a picture: %zu characters, wheel %.2f notches (x %.2f) at %d lines a notch, %s\n", g_typed.size(),
                             g_wheelY, g_wheelX, lines, io.WantCaptureKeyboard ? "a text box takes the keys" : "no text box");
        }

        // (Tests) TF_MENU_TYPE_TEST=<vblank>,<text>: from that VBlank, the text typed into the window one key every
        // 12 pictures (down, up 4 pictures later) - the Debug page's "New state" box takes it (state_manager.cpp).
        void typeTestFrame(uint64_t vblank)
        {
            static const char *spec = std::getenv("TF_MENU_TYPE_TEST");
            if (!spec)
                return;
            static const uint64_t from = std::strtoull(spec, nullptr, 10);
            static const std::string text = std::strchr(spec, ',') ? std::string(std::strchr(spec, ',') + 1) : std::string();
            static uint64_t picture = 0u;
            if (vblank < from)
                return;
            const uint64_t i = picture / 12u, phase = picture % 12u;
            ++picture;
            if (i >= text.size())
                return;
            const char c = text[i];
            const int vk = c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c == ' ' ? 0x20 : c == '!' ? 0x08 : c == '~' ? -1 : c; // ('!': Backspace, '~': the wheel a notch down)
            if (phase == 0u)
                tfPostTestKey(vk, true);
            else if (phase == 4u)
                tfPostTestKey(vk, false);
        }

        bool g_open = std::getenv("TF_MOD_MENU") != nullptr; // TF_MOD_MENU=1: open from the start
        bool g_modRestarting = false;
        std::string g_modRestartError;

        void drawCheats()
        {
            {
                std::lock_guard<std::mutex> guard(g_codeLock);
                if (g_codeActive)
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Entering: %s", g_codeName.c_str());
                else
                    ImGui::TextWrapped("Open the screen a code belongs to, then press Enter: the code is typed for you and the "
                                       "game runs its own cheat. Red Alert says so when the game accepts one.");
            }
            const char *section = nullptr;
            int id = 0;
            for (const Cheat &cheat : cheats())
            {
                if (!section || std::string(section) != cheat.screen)
                {
                    section = cheat.screen;
                    menu_style::heading(section);
                }
                ImGui::PushID(id++);
                if (menu_style::smallButton("Enter"))
                    enterCode(cheat);
                ImGui::SameLine();
                ImGui::TextUnformatted(cheat.does);
                if (ImGui::IsItemHovered())
                    menu_style::setHelp(std::string("Buttons: ") + cheat.code);
                ImGui::PopID();
            }
        }

        // ---------------------------------------------------------------------------------
        // Saves: import into and export from the port's memory card (save_io.h).

        struct Found
        {
            fs::path path;
            std::vector<save_io::Save> saves;
            std::string error;
        };
        std::vector<Found> g_found;
        double g_scannedAt = -1.0;
        std::string g_saveMessage;
        size_t g_confirmFound = SIZE_MAX, g_confirmSave = 0;

        fs::path gameFolder() { return PS2Runtime::getIoPaths().elfDirectory; }
        fs::path savesFolder() { return gameFolder().parent_path() / "Saves"; }
        fs::path cardFolder() { return PS2Runtime::getIoPaths().mcRoot; }

        std::string stampNow()
        {
            const std::time_t t = std::time(nullptr);
            std::tm tm{};
#if defined(_WIN32)
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            char text[32];
            std::strftime(text, sizeof(text), "%Y-%m-%d %H-%M-%S", &tm);
            return text;
        }

        void scanSaves()
        {
            g_found.clear();
            std::error_code ec;
            fs::create_directories(savesFolder(), ec);
            for (const auto &item : fs::directory_iterator(savesFolder(), ec))
            {
                const std::string name = item.path().filename().string();
                if (name == "Backups" || name == "Exports" || name == "README.txt")
                    continue;
                Found f;
                f.path = item.path();
                f.saves = save_io::load(item.path(), f.error);
                g_found.push_back(std::move(f));
            }
            std::sort(g_found.begin(), g_found.end(), [](const Found &a, const Found &b)
                      { return a.path < b.path; });
            g_scannedAt = ImGui::GetTime();
            if (!fs::exists(savesFolder() / "README.txt", ec))
                std::ofstream(savesFolder() / "README.txt")
                    << "Put Transformers saves here to import them in the mod menu (F1, Saves).\n"
                       "Read: .psu, .max, .psv, PCSX2 memory cards (.ps2) and save folders\n"
                       "(\"BASLUS-20668 TRANSFM\" is NTSC, \"BESLES-52388 TRANSFM\" is PAL: both work).\n"
                       "Exports go to Exports\\, and every import first copies the save it replaces to Backups\\.\n";
        }

        // The save on the port's card now (the NTSC one: this game does not read a PAL one there).
        bool cardSave(save_io::Save &out)
        {
            std::string error;
            std::error_code ec;
            const fs::path dir = cardFolder() / save_io::kNtscDirectory;
            if (!fs::is_directory(dir, ec))
                return false;
            auto saves = save_io::load(dir, error);
            if (saves.empty())
                return false;
            out = std::move(saves.front());
            return true;
        }

        void importSave(const save_io::Save &save)
        {
            std::string error;
            save_io::Save current;
            if (cardSave(current))
            {
                const fs::path backup = savesFolder() / "Backups" / stampNow();
                std::error_code ec;
                fs::create_directories(backup, ec);
                if (!save_io::writeFolder(current, backup, error))
                {
                    g_saveMessage = "Import stopped: the backup failed (" + error + ").";
                    return;
                }
            }
            if (!save_io::writeFolder(save_io::convert(save, save_io::Region::Ntsc), cardFolder(), error))
                g_saveMessage = "Import failed: " + error;
            else
                g_saveMessage = std::string("Imported (") + save_io::regionName(save.region) +
                                "). Open Continue Game; if it does not show, go back to the title screen or restart.";
        }

        void exportSave(save_io::Region region, bool asFolder)
        {
            save_io::Save current;
            if (!cardSave(current))
            {
                g_saveMessage = "There is no save on the port's memory card yet.";
                return;
            }
            const save_io::Save out = save_io::convert(current, region);
            const fs::path dir = savesFolder() / "Exports";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string base = std::string("Transformers ") + (region == save_io::Region::Pal ? "PAL " : "NTSC ") + stampNow();
            std::string error;
            const bool ok = asFolder ? save_io::writeFolder(out, dir / base, error) : save_io::writePsu(out, dir / (base + ".psu"), error);
            g_saveMessage = ok ? "Exported to " + (asFolder ? (dir / base / out.directory) : (dir / (base + ".psu"))).string()
                               : "Export failed: " + error;
        }

        void slotList(const save_io::Save &save)
        {
            const auto dates = save_io::slotDates(save);
            if (dates.empty())
                ImGui::TextDisabled("    no slot used");
            for (const std::string &date : dates)
                ImGui::TextDisabled("    slot saved %s (UTC)", date.c_str());
        }

        void drawSaves()
        {
            if (g_scannedAt < 0.0 || ImGui::GetTime() - g_scannedAt > 3.0)
                scanSaves();

            menu_style::heading("On this port's memory card");
            save_io::Save current;
            if (cardSave(current))
            {
                ImGui::TextUnformatted(current.directory.c_str());
                slotList(current);
            }
            else
                ImGui::TextDisabled("No save yet (%s)", cardFolder().string().c_str());
            ImGui::TextUnformatted("Export it:");
            ImGui::SameLine();
            if (menu_style::smallButton("NTSC .psu"))
                exportSave(save_io::Region::Ntsc, false);
            ImGui::SameLine();
            if (menu_style::smallButton("PAL .psu"))
                exportSave(save_io::Region::Pal, false);
            ImGui::SameLine();
            if (menu_style::smallButton("NTSC folder"))
                exportSave(save_io::Region::Ntsc, true);
            ImGui::SameLine();
            if (menu_style::smallButton("PAL folder"))
                exportSave(save_io::Region::Pal, true);
            if (ImGui::IsItemHovered())
                menu_style::setHelp("A folder goes into a PCSX2 folder memory card.");

            menu_style::heading("Import from the Saves folder");
            ImGui::TextWrapped("%s", savesFolder().string().c_str());
            if (menu_style::smallButton("Open the folder"))
                OpenURL(savesFolder().string().c_str());
            ImGui::SameLine();
            if (menu_style::smallButton("Refresh"))
                scanSaves();
            if (g_found.empty())
                ImGui::TextDisabled("Nothing here yet: put .psu, .max, .psv, .ps2 cards or save folders in it.");
            for (size_t i = 0; i < g_found.size(); ++i)
            {
                const Found &f = g_found[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::TextUnformatted(f.path.filename().string().c_str());
                if (f.saves.empty())
                    ImGui::TextDisabled("    %s", f.error.c_str());
                for (size_t k = 0; k < f.saves.size(); ++k)
                {
                    const save_io::Save &save = f.saves[k];
                    ImGui::PushID(static_cast<int>(k));
                    ImGui::Text("    %s", save_io::regionName(save.region));
                    ImGui::SameLine();
                    if (menu_style::smallButton("Import"))
                    {
                        g_confirmFound = i;
                        g_confirmSave = k;
                        ImGui::OpenPopup("Import this save?");
                    }
                    slotList(save);
                    if (ImGui::BeginPopupModal("Import this save?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
                    {
                        ImGui::TextUnformatted("It replaces the save on the port's memory card.");
                        ImGui::TextUnformatted("The current one is copied to Saves\\Backups first.");
                        if (menu_style::button("Import", true))
                        {
                            if (g_confirmFound < g_found.size() && g_confirmSave < g_found[g_confirmFound].saves.size())
                                importSave(g_found[g_confirmFound].saves[g_confirmSave]);
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::SameLine();
                        if (menu_style::button("Cancel"))
                            ImGui::CloseCurrentPopup();
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
                ImGui::PopID();
            }
            if (!g_saveMessage.empty())
            {
                ImGui::Separator();
                ImGui::TextWrapped("%s", g_saveMessage.c_str());
            }
        }

        // ---------------------------------------------------------------------------------
        // Controls: the keyboard and mouse layout (ps2_host_input's binding table, patch 0010),
        // kept in controls.txt beside the game (one line: the layout string).
        struct ControlRow
        {
            const char *input; // the layout's name
            const char *label;
        };
        const ControlRow kControls[] = {
            {"lup", "Move forward"},          {"ldown", "Move back"},          {"lleft", "Move left"},
            {"lright", "Move right"},         {"r1", "Fire (R1)"},             {"r3", "R3 (recon zoom; the right mouse button runs recon mode)"},
            {"l1", "Jump / movement Mini-Con (L1)"}, {"triangle", "Transform (Triangle)"},
            {"r2", "Mini-Con ability (R2)"},  {"l2", "Mini-Con ability (L2)"}, {"start", "Pause (Start)"},
            {"select", "Select"},             {"cross", "Cross (menus: confirm)"}, {"circle", "Circle (menus: back)"},
            {"square", "Square"},             {"l3", "L3"},                    {"up", "D-pad up"},
            {"down", "D-pad down"},           {"left", "D-pad left"},          {"right", "D-pad right"},
            {"rup", "Look up (keys)"},        {"rdown", "Look down (keys)"},   {"rleft", "Look left (keys)"},
            {"rright", "Look right (keys)"}};

        int g_capturing = -1;     // the row waiting for a key or a mouse button
        bool g_captureAdd = false; // add it to the row's list (else replace the list)
        bool g_captureArmed = false; // everything held at the click let go
        bool g_controlsLoaded = false;

        fs::path controlsFile() { return fs::current_path() / "controls.txt"; }

        // The layout string's list for one input ("A,B").
        std::string listFor(const std::string &layout, const std::string &input)
        {
            const std::string key = input + "=";
            size_t at = 0;
            while (at < layout.size())
            {
                size_t end = layout.find(';', at);
                if (end == std::string::npos)
                    end = layout.size();
                if (layout.compare(at, key.size(), key) == 0)
                    return layout.substr(at + key.size(), end - at - key.size());
                at = end + 1;
            }
            return "";
        }

        // Keyboard prompts (tools/make_key_prompts.py): the game's button icons as the keys bound to
        // them, a texture-pack replacement of its menu font's atlas, in replacements\keyboard\: the
        // renderer uses it while the keyboard is the device last used (a gamepad's input: the
        // PlayStation buttons again, the next frame; ps2_host_input::lastDevice). Off: never. The
        // controls saved make it again, in the background (the new file is read at the next start).
        constexpr const char *kPromptsPack = "99ef5a9eb1ea110-4229cc3f80932a00-00002213.dds";
        std::atomic<bool> g_keyPrompts{true};

        void spawnPython(const std::vector<std::string> &args, const char *what)
        {
            std::vector<std::string> quoted;
            for (const std::string &a : args)
                quoted.push_back("\"" + a + "\"");
            std::vector<const char *> argv{"pythonw"};
            for (const std::string &q : quoted)
                argv.push_back(q.c_str());
            argv.push_back(nullptr);
#if defined(_WIN32)
            if (_spawnvp(_P_NOWAIT, "pythonw", argv.data()) < 0)
                std::fprintf(stderr, "[mod menu] %s: could not start pythonw\n", what);
#else
            (void)what;
#endif
        }

        // (TODO 119) The native renderer's crisp font sets beside the HD textures (nr_hires.cpp finds them the
        // same way): TF_CRISP_DIR, else crisp_fonts beside the upscaled tree (TF_HIRES_DIR, hd_textures\upscaled,
        // the remaster's Extracted\upscaled).
        fs::path crispFontsDir()
        {
            if (const char *d = std::getenv("TF_CRISP_DIR"); d && *d)
                return fs::path(d);
            std::error_code e;
            if (const char *d = std::getenv("TF_HIRES_DIR"); d && *d)
                return fs::path(d).parent_path() / "crisp_fonts";
            if (fs::is_directory(fs::current_path() / "hd_textures" / "upscaled", e))
                return fs::current_path() / "hd_textures" / "crisp_fonts";
            // (TODO 186: the exe's folder or one above it, as nr_hires.cpp looks; the developer's tree last, dev_tree.h)
            fs::path at = fs::path(GetApplicationDirectory());
            for (int up = 0; up < 5 && !at.empty() && at != at.parent_path(); ++up, at = at.parent_path())
                if (fs::is_directory(at / "hd_textures" / "upscaled", e))
                    return at / "hd_textures" / "crisp_fonts";
            if (!devTree().empty())
                return devTree() / "crisp_fonts";
            return fs::current_path() / "hd_textures" / "crisp_fonts";
        }

        void makeKeyPrompts()
        {
            if (!g_keyPrompts.load())
                return;
            const fs::path here = fs::current_path();
            std::error_code e;
            // The native renderer's key caps (tools\hd_textures\make_crisp_fonts.py --keys-only: the crisp font
            // atlases' key-prompt sets made again from the new layout, in a few seconds).
            const fs::path crispTool = here / "tools" / "hd_textures" / "make_crisp_fonts.py";
            const fs::path crisp = crispFontsDir();
            if (fs::exists(crispTool, e) && fs::exists(crisp / "manifest.json", e))
                spawnPython({crispTool.string(), "--out", crisp.string(), "--keys-only", "--controls", controlsFile().string()},
                            "key prompts (crisp fonts)");
            // The emulated renderer's (a texture-pack replacement).
            const fs::path tool = here / "transformers" / "tools" / "make_key_prompts.py";
            const fs::path base = here / "texture_pack_work" / "key_prompts" / "DECK.png";
            if (!fs::exists(tool, e) || !fs::exists(base, e))
                return;
            spawnPython({tool.string(), "--base", base.string(), "--out", (here / "replacements" / "keyboard").string(), "--controls",
                         controlsFile().string(), "--fonts", (here / "fonts").string()},
                        "keyboard prompts");
        }

        // The prompts were once replacements\<name>.dds (always on), or .dds.off: moved into
        // keyboard\ (before the renderer reads the pack).
        void moveKeyPrompts()
        {
            const fs::path dir = fs::current_path() / "replacements";
            const fs::path into = dir / "keyboard" / kPromptsPack;
            std::error_code e;
            for (const fs::path &old : {dir / kPromptsPack, dir / (std::string(kPromptsPack) + ".off")})
            {
                if (!fs::exists(old, e))
                    continue;
                fs::create_directories(into.parent_path(), e);
                if (fs::exists(into, e))
                    fs::remove(old, e);
                else
                    fs::rename(old, into, e);
                std::fprintf(stderr, "[mod menu] keyboard prompts moved to replacements\\keyboard\n");
            }
        }

        // "Press START button" (the title, KEY_START / KEY_PRESSSTART) and "PRESS START" (the attract
        // movie's, KEY_ATTRACTMODE) say ENTER while the keyboard prompts are shown (user, 2026-10-23:
        // in the menus Enter is Start). The only strings of the table with START in capitals, so the
        // word is swapped in place where the loaded table has them (same length: nothing moves).
        // Found by a scan of 2 MB a game frame while no level runs; each found word is checked every
        // frame and dropped when its bytes are something else (the memory reused).
        constexpr uint32_t kRamSize = PS2_RAM_SIZE, kScanStep = 0x00200000u;
        const std::string_view kStartTexts[] = {"Press START button", "PRESS START"};
        std::vector<uint32_t> g_startWords; // the address of each START / ENTER
        uint32_t g_startScanAt = 0u;
        int g_startScanPasses = 0;

        void startText(uint8_t *rdram, int inMenu)
        {
            const bool keys = ps2_host_input::keyboardPromptsShown();
            std::vector<uint32_t> kept;
            for (const uint32_t at : g_startWords)
            {
                char *p = reinterpret_cast<char *>(rdram + at);
                if (std::memcmp(p, "START", 5) != 0 && std::memcmp(p, "ENTER", 5) != 0)
                    continue;
                std::memcpy(p, keys ? "ENTER" : "START", 5);
                kept.push_back(at);
            }
            if (kept.size() != g_startWords.size())
                g_startScanPasses = 0; // (one moved: look for it again)
            g_startWords.swap(kept);
            // (scanned until both strings are found twice over - the table and its title's copy - or
            // eight passes over the memory: no more while a level runs)
            if (inMenu != 1 || g_startWords.size() >= 3u || g_startScanPasses >= 8)
                return;
            const uint32_t end = std::min(g_startScanAt + kScanStep + 32u, kRamSize);
            const char *base = reinterpret_cast<const char *>(rdram);
            for (const std::string_view text : kStartTexts)
            {
                for (const char *p = base + g_startScanAt; p + text.size() <= base + end;)
                {
                    const char *hit = std::search(p, base + end, text.begin(), text.end());
                    if (hit == base + end)
                        break;
                    const uint32_t at = static_cast<uint32_t>(hit - base) + 6u; // (START: at 6 in both)
                    // PRESS START alone: not followed by more capitals (another string)
                    const char next = hit + text.size() < base + kRamSize ? hit[text.size()] : '\0';
                    if ((text.size() > 11u || !(next >= 'A' && next <= 'Z')) &&
                        std::find(g_startWords.begin(), g_startWords.end(), at) == g_startWords.end())
                    {
                        g_startWords.push_back(at);
                        std::fprintf(stderr, "[mod menu] \"%.*s\" at %08x\n", static_cast<int>(text.size()), hit, at - 6u);
                    }
                    p = hit + 1;
                }
            }
            g_startScanAt += kScanStep;
            if (g_startScanAt >= kRamSize)
            {
                g_startScanAt = 0u;
                ++g_startScanPasses;
            }
        }

        void saveControls()
        {
            const std::string layout = ps2_host_input::keyBindings();
            {
                std::ofstream out(controlsFile(), std::ios::binary | std::ios::trunc);
                out << layout << "\n";
            }
            makeKeyPrompts();
        }

        void loadControls()
        {
            if (g_controlsLoaded)
                return;
            g_controlsLoaded = true;
            std::ifstream in(controlsFile(), std::ios::binary);
            std::string layout;
            if (in && std::getline(in, layout) && !layout.empty())
            {
                // (TODO 224) R3 no longer has B and the right mouse button by default: the right button runs recon mode
                // itself (camera_mode). A layout saved with the old default gets the new one; a right button the
                // player put on R3 among his own keys is taken off it (his other keys stay).
                const std::string r3 = listFor(layout, "r3");
                std::string kept;
                if (r3 != "B,MOUSE_RIGHT")
                {
                    size_t p = 0;
                    while (p <= r3.size())
                    {
                        size_t comma = r3.find(',', p);
                        if (comma == std::string::npos)
                            comma = r3.size();
                        const std::string key = r3.substr(p, comma - p);
                        if (!key.empty() && key != "MOUSE_RIGHT")
                            kept += (kept.empty() ? "" : ",") + key;
                        p = comma + 1;
                    }
                }
                if (kept != r3)
                {
                    std::fprintf(stderr, "[mod menu] controls.txt: R3's '%s' -> '%s' (the right mouse button is recon mode)\n", r3.c_str(),
                                 kept.c_str());
                    layout += ";r3=" + kept;
                }
                ps2_host_input::setKeyBindings(layout);
                if (kept != r3)
                    saveControls();
            }
        }

        void setList(const std::string &input, const std::string &list)
        {
            ps2_host_input::setKeyBindings(ps2_host_input::keyBindings() + ";" + input + "=" + list);
            saveControls();
        }

        // Driving with W / S. In the car the game accelerates with cross and brakes or reverses with
        // square; the left stick only steers (measured: nothing else moves the car). While the Autobot
        // is a car, the keys of the left stick's up and down (W and S by default) press those buttons
        // too. The player's own layout is back as soon as the Autobot leaves the car or this menu
        // opens, so the Controls page always shows and saves it. TF_CAR_KEYS / "Drive with the movement
        // keys" (Controls, Keyboard).
        std::atomic<bool> g_carKeys{true};
        // Menu keys (user, 2026-10-22): in the start menu, the HQ and the pause screen Enter and
        // Space select (cross) and Escape goes back (triangle); in play Space jumps and Enter /
        // Escape pause, as the layout has them. Set by frame (the game thread): 1 no level running
        // (the title, the start menu, the HQ), 2 the level paused (the sound system's pause state,
        // [0x6CF2A8]+0x28, is 2), 0 playing. Outside a level Enter and Space are Start as well (the user: the title's
        // "Press Start" takes only Start; Start does nothing in the HQ's menus); paused, Start
        // would resume, so Enter is cross alone there.
        std::atomic<int> g_inMenu{0}; // (3: paused, at the pause screen's top - Escape resumes there)
        // taScreenInGPause::ProcessInput (0x311A30) runs only while the pause screen's top is the
        // screen shown (its pages - options, the quit confirmation - have their own): the game
        // frame it last ran in, so Escape can be Start (resume) there and triangle (back) in a page.
        constexpr uint32_t kPauseInput = 0x00311A30u;
        PS2Runtime::RecompiledFunction g_pauseInput = nullptr;
        std::atomic<uint64_t> g_pauseInputFrame{0}, g_gameFrames{0};
        enum class Overlay
        {
            None,
            Car,
            Menu,
            MenuPaused,
            MenuPauseTop
        };
        Overlay g_overlay = Overlay::None;
        std::string g_userLayout;  // the player's own layout, under an overlay
        std::string g_overlaySet;  // the layout the overlay put in (changed meanwhile: the player's new one stays)
        // (TODO 223) The player's own layout while an overlay is in (key_text.cpp reads it on the game's thread)
        std::mutex g_playerLayoutMutex;
        std::string g_playerLayoutCopy; // empty: no overlay (the layout in force is the player's)

        std::string joined(const std::string &a, const std::string &b)
        {
            if (a.empty() || b.empty())
                return a + b;
            return a + "," + b;
        }

        // A list ("A,B,C") without the keys named.
        std::string without(const std::string &list, std::initializer_list<const char *> keys)
        {
            std::string out;
            for (size_t at = 0; at <= list.size();)
            {
                size_t end = list.find(',', at);
                if (end == std::string::npos)
                    end = list.size();
                const std::string key = list.substr(at, end - at);
                bool drop = key.empty();
                for (const char *k : keys)
                    drop = drop || key == k;
                if (!drop)
                    out = joined(out, key);
                at = end + 1;
            }
            return out;
        }

        // The menu layout: Enter and Space on cross, Escape on triangle, and those three taken off
        // whatever else they were on (start, l1); the rest as the player has it.
        std::string menuLayout(const std::string &user, bool front, bool top = false)
        {
            std::string out;
            for (size_t at = 0; at < user.size();)
            {
                size_t end = user.find(';', at);
                if (end == std::string::npos)
                    end = user.size();
                const std::string item = user.substr(at, end - at);
                at = end + 1;
                const size_t eq = item.find('=');
                if (eq == std::string::npos)
                    continue;
                const std::string input = item.substr(0, eq);
                std::string list = without(item.substr(eq + 1), {"ENTER", "SPACE", "ESCAPE"});
                if (input == "cross")
                    list = joined(list, "ENTER,SPACE");
                else if (input == "triangle")
                    list = joined(list, "ESCAPE");
                else if (input == "start" && top)
                    list = joined(list, "ESCAPE");
                else if (input == "start" && front)
                    list = joined(list, "ENTER,SPACE");
                out += (out.empty() ? "" : ";") + input + "=" + list;
            }
            return out;
        }

        void updateCarKeys()
        {
            Overlay want = Overlay::None;
            if (const int menu = g_inMenu.load(std::memory_order_relaxed); !g_open && menu != 0)
                want = menu == 1 ? Overlay::Menu : (menu == 3 ? Overlay::MenuPauseTop : Overlay::MenuPaused);
            else if (g_carKeys.load(std::memory_order_relaxed) && !g_open && camera_mode::inVehicle())
                want = Overlay::Car;
            if (want == g_overlay)
                return;
            // (not while Enter, Space or Escape is held: the key that paused or picked would read as a
            // new press of what it means in the other layout - Escape pausing, then resuming at once)
            if (IsKeyDown(KEY_ENTER) || IsKeyDown(KEY_KP_ENTER) || IsKeyDown(KEY_SPACE) || IsKeyDown(KEY_ESCAPE))
                return;
            if (g_overlay != Overlay::None)
            {
                // (the player's layout back - unless it was changed under the overlay: that one stays)
                if (ps2_host_input::keyBindings() == g_overlaySet)
                    ps2_host_input::setKeyBindings(g_userLayout);
                std::lock_guard<std::mutex> lock(g_playerLayoutMutex);
                g_playerLayoutCopy.clear();
            }
            static const char *const kOverlayNames[] = {"the player's", "the car's", "the menus'", "the pause screen's pages'", "the pause screen's"};
            std::fprintf(stderr, "[mod menu] keys: %s layout\n", kOverlayNames[static_cast<int>(want)]);
            g_overlay = want;
            if (want == Overlay::None)
                return;
            g_userLayout = ps2_host_input::keyBindings();
            {
                std::lock_guard<std::mutex> lock(g_playerLayoutMutex);
                g_playerLayoutCopy = g_userLayout;
            }
            if (want == Overlay::Car)
            {
                const std::string forward = listFor(g_userLayout, "lup"), back = listFor(g_userLayout, "ldown");
                ps2_host_input::setKeyBindings(g_userLayout + ";cross=" + joined(listFor(g_userLayout, "cross"), forward) +
                                               ";square=" + joined(listFor(g_userLayout, "square"), back) + ";lup=;ldown=");
            }
            else
                ps2_host_input::setKeyBindings(menuLayout(g_userLayout, want == Overlay::Menu, want == Overlay::MenuPauseTop));
            g_overlaySet = ps2_host_input::keyBindings();
            if (std::getenv("TF_KEYS_TRACE"))
                std::fprintf(stderr, "[mod menu] keys: %s\n", g_overlaySet.c_str());
        }

        void drawControls()
        {
            ImGui::TextWrapped("Click Change, then press the key or mouse button (Esc cancels). Add keeps the old ones too. "
                               "WASD, the mouse and a gamepad work as before; this changes only the keyboard and mouse.");
            const std::string layout = ps2_host_input::keyBindings();
            if (g_capturing >= 0)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Press a key or mouse button for: %s", kControls[g_capturing].label);
                // (held state, not raylib's pressed queue: the runtime polls input several times a
                // frame. Waits until everything held when Change was clicked is let go.)
                int code = -1;
                bool anyHeld = false;
                for (int key = KEY_SPACE; key <= KEY_KP_EQUAL; ++key)
                    if (IsKeyDown(key))
                    {
                        anyHeld = true;
                        if (g_captureArmed && code < 0)
                            code = key;
                    }
                for (int b = MOUSE_BUTTON_LEFT; b <= MOUSE_BUTTON_EXTRA; ++b)
                    if (IsMouseButtonDown(b))
                    {
                        anyHeld = true;
                        if (g_captureArmed && code < 0)
                            code = ps2_host_input::mouseButtonCode(b);
                    }
                if (!anyHeld)
                    g_captureArmed = true;
                if (code == KEY_ESCAPE)
                    g_capturing = -1;
                else if (code > 0)
                {
                    const std::string input = kControls[g_capturing].input;
                    std::string list = g_captureAdd ? listFor(layout, input) : "";
                    const std::string name = ps2_host_input::keyName(code);
                    if (list.find(name) == std::string::npos)
                        list += (list.empty() ? "" : ",") + name;
                    setList(input, list);
                    g_capturing = -1;
                }
            }
            if (ImGui::BeginTable("controls", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX))
            {
                for (int i = 0; i < static_cast<int>(sizeof(kControls) / sizeof(kControls[0])); ++i)
                {
                    ImGui::PushID(i);
                    ImGui::TableNextRow(0, menu_style::textSize() * 2.0f);
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(kControls[i].label);
                    ImGui::TableSetColumnIndex(1);
                    const std::string list = listFor(layout, kControls[i].input);
                    ImGui::TextUnformatted(list.empty() ? "-" : list.c_str());
                    ImGui::TableSetColumnIndex(2);
                    if (menu_style::smallButton("Change"))
                    {
                        g_capturing = i;
                        g_captureAdd = false;
                        g_captureArmed = false;
                    }
                    ImGui::SameLine();
                    if (menu_style::smallButton("Add"))
                    {
                        g_capturing = i;
                        g_captureAdd = true;
                        g_captureArmed = false;
                    }
                    ImGui::TableSetColumnIndex(3);
                    if (menu_style::smallButton("Clear"))
                        setList(kControls[i].input, "");
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            if (menu_style::button("Reset all to the default layout"))
            {
                ps2_host_input::setKeyBindings(ps2_host_input::defaultKeyBindings());
                saveControls();
            }
        }

        // ---------------------------------------------------------------------------------
        // (TODO 240) The controller's buttons: which controller button presses each of the game's buttons
        // (ps2_host_input::setPadBindings, patch 0156), kept in settings.ini as PS2X_PADMAP (only what differs from
        // the default). Change waits for a press of a controller button (read as it is: the layout does not apply to
        // the menu's own pad keys, which stay fixed). The controller buttons are named as the pad used last shows them.
        struct PadRow
        {
            const char *input; // the game's button (the layout's name)
            const char *label;
        };
        const PadRow kPadRows[] = {
            {"l1", "Jump / movement Mini-Con (L1)"}, {"r1", "Fire (R1)"}, {"l2", "Mini-Con ability (L2)"},
            {"r2", "Mini-Con ability (R2)"}, {"triangle", "Transform (Triangle)"}, {"r3", "Recon (R3)"},
            {"cross", "Cross (menus: confirm)"}, {"circle", "Circle (menus: back)"}, {"square", "Square"}, {"l3", "L3"},
            {"start", "Pause (Start)"}, {"select", "Select"}, {"up", "D-pad up"}, {"down", "D-pad down"},
            {"left", "D-pad left"}, {"right", "D-pad right"}};
        // The controller buttons, in the layout's order (up .. select), with the raylib button each is.
        const char *const kPadNames[16] = {"up", "down", "left", "right", "cross", "circle", "square", "triangle",
                                           "l1", "r1", "l2", "r2", "l3", "r3", "start", "select"};
        const int kPadRaylib[16] = {GAMEPAD_BUTTON_LEFT_FACE_UP, GAMEPAD_BUTTON_LEFT_FACE_DOWN, GAMEPAD_BUTTON_LEFT_FACE_LEFT,
                                    GAMEPAD_BUTTON_LEFT_FACE_RIGHT, GAMEPAD_BUTTON_RIGHT_FACE_DOWN, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT,
                                    GAMEPAD_BUTTON_RIGHT_FACE_LEFT, GAMEPAD_BUTTON_RIGHT_FACE_UP, GAMEPAD_BUTTON_LEFT_TRIGGER_1,
                                    GAMEPAD_BUTTON_RIGHT_TRIGGER_1, GAMEPAD_BUTTON_LEFT_TRIGGER_2, GAMEPAD_BUTTON_RIGHT_TRIGGER_2,
                                    GAMEPAD_BUTTON_LEFT_THUMB, GAMEPAD_BUTTON_RIGHT_THUMB, GAMEPAD_BUTTON_MIDDLE_RIGHT,
                                    GAMEPAD_BUTTON_MIDDLE_LEFT};
        const char *const kPadPlayStation[16] = {"D-pad up", "D-pad down", "D-pad left", "D-pad right", "Cross", "Circle", "Square",
                                                 "Triangle", "L1", "R1", "L2", "R2", "L3", "R3", "Options", "Create"};
        const char *const kPadXbox[16] = {"D-pad up", "D-pad down", "D-pad left", "D-pad right", "A", "B", "X", "Y",
                                          "LB", "RB", "LT", "RT", "LS", "RS", "Menu", "View"};
        int g_padCapturing = -1;   // the row waiting for a controller button
        bool g_padArmed = false;   // everything held at the click let go
        double g_padCaptureAt = 0.0;

        int padIndexOf(const std::string &name)
        {
            for (int i = 0; i < 16; ++i)
                if (name == kPadNames[i])
                    return i;
            return -1;
        }

        // The controller buttons held now, read as they are (-1 none; `any`: something is held).
        int padHeld(bool &any)
        {
            any = false;
            int first = -1;
            ps2x::sonypad::InputState s;
            if (ps2x::sonypad::readHostPad(s))
            {
                for (int i = 0; i < 16; ++i)
                {
                    bool down = ps2x::sonypad::gamepadButtonDown(s, kPadRaylib[i]);
                    if (i == 10)
                        down = down || s.l2 > 0x2Au;
                    if (i == 11)
                        down = down || s.r2 > 0x2Au;
                    if (i == 15)
                        down = down || (s.buttons & ps2x::sonypad::kTouchpad) != 0u;
                    if (down)
                    {
                        any = true;
                        if (first < 0)
                            first = i;
                    }
                }
                return first;
            }
            if (!ps2x::sonypad::glfwGamepadsAllowed())
                return -1;
            for (int g = 0; g < 4; ++g)
            {
                if (!IsGamepadAvailable(g))
                    continue;
                for (int i = 0; i < 16; ++i)
                    if (IsGamepadButtonDown(g, kPadRaylib[i]))
                    {
                        any = true;
                        if (first < 0)
                            first = i;
                    }
                break;
            }
            return first;
        }

        // The layout without what is as the default (each game button on its own controller button): what is saved.
        std::string padChanges(const std::string &layout)
        {
            std::string out;
            for (const char *name : kPadNames)
            {
                const std::string list = listFor(layout, name);
                if (list != name)
                    out += (out.empty() ? "" : ";") + std::string(name) + "=" + list;
            }
            return out.empty() && *padDefault() ? "l1=l1" : out; // (the plain layout kept when the default is another)
        }

        void setPadLayout(const std::string &layout)
        {
            ps2_host_input::setPadBindings(layout);
            ps2_settings::noteChanged("PS2X_PADMAP", padChanges(ps2_host_input::padBindings()));
        }

        void drawPadButtons()
        {
            const bool xbox = ps2_host_input::lastPadStyle() == ps2_host_input::PadStyle::Xbox;
            const char *const *names = xbox ? kPadXbox : kPadPlayStation;
            ImGui::TextWrapped("Which controller button does what. Click Change, then press the controller button (Esc, or "
                               "waiting 8 seconds, cancels). The F1 menu's own buttons stay as they are.");
            const std::string layout = ps2_host_input::padBindings();
            if (g_padCapturing >= 0)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Press a controller button for: %s", kPadRows[g_padCapturing].label);
                bool any = false;
                const int held = padHeld(any);
                if (!any)
                    g_padArmed = true;
                if (IsKeyPressed(KEY_ESCAPE) || ImGui::GetTime() - g_padCaptureAt > 8.0)
                    g_padCapturing = -1;
                else if (g_padArmed && held >= 0)
                {
                    std::string next;
                    for (const char *name : kPadNames)
                    {
                        const std::string list = name == std::string(kPadRows[g_padCapturing].input) ? kPadNames[held] : listFor(layout, name);
                        next += (next.empty() ? "" : ";") + std::string(name) + "=" + list;
                    }
                    setPadLayout(next);
                    g_padCapturing = -1;
                }
            }
            if (ImGui::BeginTable("padbuttons", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX))
            {
                for (int i = 0; i < static_cast<int>(sizeof(kPadRows) / sizeof(kPadRows[0])); ++i)
                {
                    ImGui::PushID(1000 + i);
                    ImGui::TableNextRow(0, menu_style::textSize() * 2.0f);
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(kPadRows[i].label);
                    ImGui::TableSetColumnIndex(1);
                    std::string shown;
                    const std::string list = listFor(layout, kPadRows[i].input);
                    for (size_t at = 0; at <= list.size();)
                    {
                        size_t end = list.find(',', at);
                        if (end == std::string::npos)
                            end = list.size();
                        const int p = padIndexOf(list.substr(at, end - at));
                        if (p >= 0)
                            shown += (shown.empty() ? "" : ", ") + std::string(names[p]);
                        at = end + 1;
                    }
                    const bool moved = list != kPadRows[i].input;
                    if (moved)
                        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "%s", shown.empty() ? "-" : shown.c_str());
                    else
                        ImGui::TextUnformatted(shown.empty() ? "-" : shown.c_str());
                    ImGui::TableSetColumnIndex(2);
                    if (menu_style::smallButton("Change"))
                    {
                        g_padCapturing = i;
                        g_padArmed = false;
                        g_padCaptureAt = ImGui::GetTime();
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            if (menu_style::button("Triggers fire"))
                setPadLayout("l1=l2;l2=l1;r1=r2;r2=r1");
            if (ImGui::IsItemHovered())
                menu_style::setHelp("Fire and jump on the triggers (R2, L2), the Mini-Con abilities on the bumpers (R1, L1).");
            ImGui::SameLine();
            if (menu_style::button("Reset to default"))
                setPadLayout(padDefault());
        }

        // ---------------------------------------------------------------------------------
        // The settings pages: every setting of the registry (the runtime's and the game's, which
        // F5's panel used to show and now only this menu does), on the page it belongs to. A
        // setting of a section no page names goes to Other, so none is ever hidden.
        enum class Page
        {
            Display,
            Graphics,
            Controls,
            Sound,
            Gameplay,
            Cheats,
            Levels,
            Saves,
            Mods,
            Other,
            About,
            Quit, // (TODO 277)
        };
        struct PageInfo
        {
            Page page;
            const char *name;
        };
        // (TODO 73 (a), 2026-10-29: Cheats and Levels folded into Mods - the mods, the prototype's
        // features, the unlocks and the cheats on one page under headings.)
        const PageInfo kPages[] = {{Page::Display, "Display"}, {Page::Graphics, "Graphics"}, {Page::Controls, "Controls"},
                                   {Page::Sound, "Sound"},     {Page::Gameplay, "Gameplay"}, {Page::Mods, "Mods & cheats"},
                                   {Page::Saves, "Saves"},     {Page::Other, "Debug"},      {Page::About, "About"},
                                   {Page::Quit, "Quit game"}}; // (TODO 277: the last one, at the bottom of the list)
        int g_page = 0;

        using SettingRow = ps2_settings::detail::Row;

        // The display page's settings, in this order, whatever their section.
        const char *const kDisplayKeys[] = {"PS2X_FULLSCREEN", "PS2X_MONITOR", "TF_ASPECT",    "PS2X_WIDESCREEN",
                                            "TF_FOV",          "TF_FRAME_RATE", "TF_FPS_LOCK", "TF_FRAME_PACING", "PS2X_SHOW_FPS"};
        // Settings first on their page, in this order (the rest follow as the registry lists them).
        const char *const kFirstKeys[] = {"TF_RENDERER", "PS2X_GPU_SCALE", "PS2X_GPU_MSAA", "PS2X_GPU_AA", "TF_NATIVE_AA", "PS2X_GPU_ANISOTROPY",
                                          "TF_DRAW_DISTANCE", "PS2X_TEXTURE_PACK", "PS2X_MOUSE_SENSITIVITY",
                                          "PS2X_MOUSE_INVERT_Y", "TF_GAME_SPEED", "TF_GRAVITY"};

        int indexIn(const char *const *list, size_t count, const std::string &key)
        {
            for (size_t i = 0; i < count; ++i)
                if (key == list[i])
                    return static_cast<int>(i);
            return -1;
        }

        // The Debug page's (TODO 73 (b), the user's choice 2026-10-29: test and developer switches on
        // one page, the last): the emulated GPU's own switches, the sound engine, turbo loading and the
        // developers' test options.
        const char *const kDebugKeys[] = {"PS2X_GPU",          "PS2X_GPU_NATIVE",          "PS2X_GPU_TEXTURE_CACHE",
                                          "PS2X_GPU_FULL_FIELDS", "PS2X_TEXTURE_DUMP",       "TF_NATIVE_SOUND",
                                          "TF_FAST_LOAD",      "TF_CFG_enableWireframe",   "TF_CFG_debugRagdollMode",
                                          "TF_CFG_creaturesAlwaysUseCover", "TF_CFG_enableIncidentals",
                                          "TF_CFG_enableOcclusion"};

        // One smoothing row: the chosen renderer's (the other's is not shown).
        bool otherRenderersRow(const SettingRow &r)
        {
            const bool native = ps2_settings::value("TF_RENDERER") == "native";
            // (TODO 178: the mouse follows the game's own Invert Y option - its own switch is not shown)
            return (r.key == "PS2X_GPU_AA" && native) || (r.key == "TF_NATIVE_AA" && !native) || r.key == "PS2X_MOUSE_INVERT_Y";
        }

        Page pageOf(const SettingRow &r)
        {
            if (indexIn(kDebugKeys, std::size(kDebugKeys), r.key) >= 0)
                return Page::Other;
            if (r.key == "TF_RENDERER")
                return Page::Graphics;
            if (indexIn(kDisplayKeys, std::size(kDisplayKeys), r.key) >= 0 || r.section == "Display" || r.section == "On screen")
                return Page::Display;
            if (r.section == "Graphics")
                return Page::Graphics;
            if (r.section == "Mouse" || r.section == "Controller" || r.section == "Keyboard")
                return Page::Controls;
            if (r.section == "Sound")
                return Page::Sound;
            if (r.section == "Gameplay" || r.section == "Debug")
                return Page::Gameplay;
            if (r.section == "Cheats" || r.section == "Prototype" || r.section == "Jet bike")
                return Page::Mods;
            if (r.section == "Updates")
                return Page::About; // (the update check: updates.h)
            return Page::Other;
        }

        // Names a player looks for.
        std::string labelOf(const SettingRow &r)
        {
            static const std::pair<const char *, const char *> kNames[] = {
                {"PS2X_GPU_SCALE", "Rendering resolution"}, {"PS2X_MOUSE_SENSITIVITY", "Mouse camera speed"},
                {"PS2X_MOUSE_INVERT_Y", "Invert mouse up / down"}, {"TF_ASPECT", "Aspect ratio"},
                {"PS2X_FULLSCREEN", "Display mode"},        {"PS2X_WIDESCREEN", "Widescreen (the game's 16:9)"},
                {"TF_FOV", "Field of view"},               {"PS2X_GPU_AA", "Smooth edges"},
                {"TF_NATIVE_AA", "Smooth edges"}};
            for (const auto &[key, name] : kNames)
                if (r.key == key)
                    return name;
            return r.label;
        }

        int orderOf(const SettingRow &r)
        {
            const int display = indexIn(kDisplayKeys, std::size(kDisplayKeys), r.key);
            if (display >= 0)
                return display;
            const int first = indexIn(kFirstKeys, std::size(kFirstKeys), r.key);
            return first >= 0 ? first : 1000;
        }

        std::vector<SettingRow> g_rows;
        double g_rowsAt = -1.0;

        const std::vector<SettingRow> &rows()
        {
            if (g_rowsAt < 0.0 || ImGui::GetTime() - g_rowsAt > 0.25)
            {
                g_rows = ps2_settings::detail::rows();
                std::stable_sort(g_rows.begin(), g_rows.end(), [](const SettingRow &a, const SettingRow &b)
                                 { return orderOf(a) < orderOf(b); });
                g_rowsAt = ImGui::GetTime();
            }
            return g_rows;
        }

        // ---- The pages' layout (the overlay tidy, 2026-11-08) -------------------------------------------------
        // Each page names its rows in order; related settings merge into one row (a choice that sets several keys)
        // with the detail in an "Advanced" fold-out. No key changed: a merged row reads and writes the same keys, so
        // an old settings.ini maps onto it. A registered row no page names still shows, at the end of its page
        // (pageOf), so a new setting never goes missing.
        //
        // TF_MOD_MENU_LIST=1 logs this layout (listTabs): g_list then collects the rows instead of drawing them.
        std::string *g_list = nullptr;
        std::vector<SettingRow> g_listRows;

        const std::vector<SettingRow> &rowsNow() { return g_list ? g_listRows : rows(); }

        const SettingRow *rowFor(const char *key)
        {
            for (const SettingRow &r : rowsNow())
                if (r.key == key)
                    return &r;
            return nullptr;
        }

        std::string valueOf(const SettingRow &r)
        {
            return r.selected >= 0 && r.selected < static_cast<int>(r.choices.size()) ? r.choices[static_cast<size_t>(r.selected)].value : "";
        }

        // Shorter names and one-line help (the registry's own stay for the rest).
        struct Words
        {
            const char *key, *label, *help;
        };
        const Words kWords[] = {
            {"PS2X_FULLSCREEN", "Display mode", "How the game fills your screen."},
            {"PS2X_MONITOR", "Screen", "Which monitor the game uses."},
            {"TF_FOV", "Field of view", "How wide the camera sees."},
            {"TF_HQ_FOV", "Field of view in the HQ", "Use the field of view in the HQ too."},
            {"TF_FPS_CAP", "Frame rate limit", "The most frames a second the game draws."},
            {"PS2X_SHOW_FPS", "Show frame rate", "A frame counter in the corner."},
            {"TF_FRAME_RATE", "Game update rate", "How often the game itself updates in levels. Takes a restart."},
            {"TF_FPS_LOCK", "Picture rate", "How many pictures a second are shown in levels."},
            {"TF_FRAME_PACING", "Frame pacing", "Fixed steps suit screens without FreeSync or G-Sync."},
            {"TF_CRISP_TEXT", "Sharp text", "Redrawn, sharper fonts."},
            {"TF_SMALL_CROSSHAIR", "Crosshair", "The aiming crosshair's size."},
            {"TF_HQ_PANELS", "HQ menu panels", "The size of the HQ's hologram menus."},
            {"TF_DISTANCE_METER", "Distance meter", "The prototype's distance to the target, under the crosshair."},
            {"TF_SPEEDRUN_TIMER", "Speedrun timer", "A timer with a split for each level."},
            {"TF_RENDERER", "Renderer", "Native is faster and sharper. Takes a restart."},
            {"PS2X_GPU_SCALE", "Resolution", "The resolution the game renders at."},
            {"TF_DRAW_DISTANCE", "Draw distance", "How far you can see detail: scenery, trees, grass and the ground."},
            {"TF_SCENERY_DISTANCE", "Scenery distance", "How far buildings and ruins keep their detail."},
            {"TF_FOREST_DISTANCE", "Forest distance", "How far the trees are full 3D models."},
            {"TF_FOREST_SPRITES", "Far trees", "The farthest trees: 3D models or flat pictures."},
            {"TF_FOREST_FULL_DETAIL", "Trees", "Every tree as a full 3D model at any distance. Costs a lot of frame rate."},
            {"TF_GRASS_DISTANCE", "Grass distance", "How far the grass reaches."},
            {"TF_TERRAIN_DETAIL", "Ground detail distance", "How far the ground keeps its close-up texture."},
            {"TF_FOREST_ADAPTIVE", "Forest on slower PCs", "Adaptive shortens the forest when the frame rate drops."},
            {"TF_ENEMY_DETAIL", "Enemy detail", "How detailed enemies look up close."},
            {"TF_NATIVE_AA", "Edge filter", "Smooths edges after drawing."},
            {"PS2X_GPU_AA", "Edge filter", "Smooths edges after drawing."},
            {"PS2X_GPU_MSAA", "Multisampling", "Smooths the edges of shapes. Higher costs more."},
            {"TF_NATIVE_SSAA", "Supersampling", "Draws at a higher resolution, then scales down. Costly."},
            {"TF_NATIVE_QUALITY", "Texture quality", "Texture sharpness and filtering. Low suits 4 GB cards. Takes a restart."},
            {"TF_SHARP_TEXTURES", "Sharp distant textures", "Keeps textures sharp into the distance on high-resolution screens."},
            {"TF_HIRES", "HD textures", "The HD textures made from your disc. Off: the game's own. Takes a restart."},
            {"PS2X_TEXTURE_PACK", "Texture pack", "Emulated renderer: PCSX2 texture packs from a replacements folder."},
            {"PS2X_GPU_ANISOTROPY", "Texture pack filtering", "Emulated renderer: how sharp a texture pack stays at an angle."},
            {"PS2X_MOUSE_SENSITIVITY", "Mouse speed", "How fast the mouse turns the camera."},
            {"TF_MENU_MOUSE", "Mouse in menus", "Point and click in the game's menus."},
            {"PS2X_SONYPAD_RUMBLE", "Vibration", "Controller vibration strength."},
            {"PS2X_SONYPAD_LIGHTBAR", "Light bar", "The DualSense or DualShock 4 light colour. Your Autobot's: Optimus red, Hot Shot yellow, Red Alert blue."},
            {"TF_LIGHTBAR_ALERTS", "Light bar warnings", "The light blinks blue and red when your health is low, and blinks your Autobot's colour when he is destroyed."},
            {"TF_ADAPTIVE_TRIGGERS", "Adaptive triggers", "L2 or R2 pushes back while a weapon Mini-Con is on it: missiles click, cannons kick harder, beam rifles resist. Off in car mode."},
            {"TF_TRIGGER_STRENGTH", "Trigger strength", "How hard the adaptive triggers push back. Light is the softest."},
            {"TF_PAD_SPEAKER", "Controller speaker", "Game alerts: transforming, enemies spotting you, the low-health beep and the HQ's calls play on the DualSense instead of the PC. Surround: everything on both."},
            {"TF_PAD_SPEAKER_VOLUME", "Controller speaker volume", "How loud the DualSense's speaker plays."},
            {"TF_PAD_SPEAKER_DELAY", "Speaker delay (Bluetooth)", "Surround over Bluetooth: holds the PC's sound back so it plays together with the controller's. Raise it if the PC is heard first. Over a USB cable the game measures it by itself."},
            {"PS2X_GAMEPAD_BACKEND", "Controller support", "Others only leaves a Sony pad to Steam Input."},
            {"PS2X_STICK_RESPONSE", "Stick response", "Modern: a light touch walks, a third of the stick runs. Off: as before."},
            {"PS2X_STICK_VEHICLE_MODE", "Car steering", "Original: the car steers as before. Tuned: gentler near the centre."},
            {"PS2X_STICK_LEFT_DEADZONE", "Left stick dead zone", "How far the left stick must move before the game sees it. Raise it if the robot walks or a menu scrolls on its own."},
            {"PS2X_STICK_RIGHT_DEADZONE", "Right stick dead zone", "How far the right stick must move before the game sees it. Raise it if the view turns on its own."},
            {"PS2X_STICK_LEFT_LIFT", "Left stick lift", "How much a light touch of the left stick moves the robot."},
            {"PS2X_STICK_LEFT_SENSITIVITY", "Left stick sensitivity", "Higher: running starts earlier on the stick."},
            {"PS2X_STICK_RIGHT_LIFT", "Right stick lift", "Makes small camera moves respond."},
            {"PS2X_STICK_VEHICLE_LIFT", "Car stick lift", "With Tuned car steering only. Auto: just past the game's own dead zone."},
            {"PS2X_STICK_VEHICLE_RESPONSE", "Car stick curve", "With Tuned car steering only. Higher: gentler near the centre."},
            {"TF_CAR_KEYS", "Drive with movement keys", "W and S drive the car."},
            {"TF_NATIVE_SOUND", "Sound engine", "Native or the emulated PS2 sound chip."},
            {"TF_CFG_showHUD", "HUD", "Show the in-game HUD."},
            {"TF_START_SCREENS", "Start-up", "What shows when the game starts."},
            {"PS2X_THREAD_PLACEMENT", "CPU thread placement", "Auto: the busiest threads each get a core, the ones that share work on cores that share a cache. Can steady the frame rate on 6+ core CPUs, Ryzen especially. Off: Windows decides. Takes a restart."},
        };
        const Words *wordsFor(const std::string &key)
        {
            for (const Words &w : kWords)
                if (key == w.key)
                    return &w;
            return nullptr;
        }

        // Shorter choice names ("KEY=value").
        const std::pair<const char *, const char *> kChoiceWords[] = {
            {"TF_FRAME_PACING=even", "Fixed steps"}, {"TF_FRAME_PACING=any", "Variable (VRR)"},
            {"TF_ENEMY_DETAIL=0", "PS2"}, {"TF_ENEMY_DETAIL=1", "Medium"},
            {"PS2X_GAMEPAD_BACKEND=auto", "Auto"}, {"PS2X_GAMEPAD_BACKEND=sony", "Sony pads only"},
            {"PS2X_GAMEPAD_BACKEND=glfw", "Others only"},
            {"PS2X_STICK_RESPONSE=on", "Modern"}, {"PS2X_STICK_RESPONSE=off", "Off"},
            {"PS2X_STICK_VEHICLE_MODE=original", "Original"}, {"PS2X_STICK_VEHICLE_MODE=tuned", "Tuned"},
            {"TF_START_SCREENS=menu", "Main menu"}, {"TF_START_SCREENS=title", "Title screen"},
            {"TF_START_SCREENS=all", "Everything (PS2)"},
            {"TF_RENDERER=native", "Native (DX12)"}, {"TF_RENDERER=emulated", "Emulated (PS2)"},
        };
        std::string choiceLabel(const SettingRow &r, const ps2_settings::Choice &c)
        {
            const std::string id = r.key + "=" + c.value;
            for (const auto &[k, label] : kChoiceWords)
                if (id == k)
                    return label;
            return c.label;
        }

        bool rowShown(const SettingRow &r) { return !r.choices.empty() && !otherRenderersRow(r); }

        void pageHeading(const char *text)
        {
            if (g_list)
                *g_list += std::string(" | ") + text + ":";
            else
                menu_style::heading(text);
        }

        void listItem(const std::string &text)
        {
            if (g_list)
                *g_list += " " + text + ";";
        }

        // One setting's row, by key (nothing when it is not registered or not for this renderer).
        void keyRow(const char *key)
        {
            const SettingRow *found = rowFor(key);
            if (!found || !rowShown(*found))
                return;
            const SettingRow r = *found;
            const Words *w = wordsFor(r.key);
            const std::string label = w ? w->label : labelOf(r);
            if (g_list)
            {
                listItem(label + " (" + r.key + ")");
                return;
            }
            std::vector<std::string> labels;
            labels.reserve(r.choices.size());
            for (const auto &c : r.choices)
                labels.push_back(choiceLabel(r, c));
            // (TODO 217: a dimmed row says why it is dimmed - its unavailable help - not what it would do)
            const int chosen = menu_style::choiceRow(r.key.c_str(), label, labels, r.selected, r.available,
                                                     r.pending ? "RESTART" : nullptr, w && r.available ? std::string(w->help) : r.help);
            if (chosen >= 0 && chosen < static_cast<int>(r.choices.size()))
            {
                ps2_settings::detail::choose(r.entry, r.choices[static_cast<size_t>(chosen)].value);
                g_rowsAt = -1.0;
            }
        }

        // An "Advanced" fold-out: true while open (its rows follow). Closed by default; kept while the game runs.
        bool advanced(const char *id)
        {
            static std::vector<std::string> open;
            // (TF_MENU_ADVANCED=1, tests: every fold-out open from the start, for TF_MENU_SHOT's pictures)
            static const bool allOpen = [] { const char *e = std::getenv("TF_MENU_ADVANCED"); return e && *e == '1'; }();
            if (allOpen && std::find(open.begin(), open.end(), id) == open.end())
                open.push_back(id);
            const auto at = std::find(open.begin(), open.end(), id);
            const bool isOpen = at != open.end();
            if (g_list)
            {
                *g_list += " [advanced:";
                return true; // (listed open; advancedEnd closes the bracket)
            }
            ImGui::PushID(id);
            ImGui::Indent(menu_style::textSize() * 0.6f);
            const bool clicked = menu_style::smallButton(isOpen ? "Hide advanced" : "Advanced");
            ImGui::Unindent(menu_style::textSize() * 0.6f);
            ImGui::PopID();
            if (clicked)
            {
                if (isOpen)
                    open.erase(at);
                else
                    open.push_back(id);
                return !isOpen;
            }
            return isOpen;
        }

        void advancedEnd()
        {
            if (g_list)
                *g_list += " ]";
        }

        // A merged row: one choice that sets several keys.
        struct Pair
        {
            const char *key;
            std::string value;                 // kMax: the row's last (highest) choice
            std::vector<std::string> accepted; // other values that count as this choice (and are kept when it is chosen)
        };
        struct Option
        {
            std::string label;
            std::vector<Pair> pairs;
        };
        const char *const kMax = "\x01max";

        std::string wanted(const Pair &p, const SettingRow &r)
        {
            return p.value == kMax && !r.choices.empty() ? r.choices.back().value : p.value;
        }

        // (a key that is not registered - another build's - is left out of both)
        bool matches(const Option &o)
        {
            for (const Pair &p : o.pairs)
                if (const SettingRow *r = rowFor(p.key))
                {
                    const std::string v = valueOf(*r);
                    if (v != wanted(p, *r) && std::find(p.accepted.begin(), p.accepted.end(), v) == p.accepted.end())
                        return false;
                }
            return true;
        }

        void apply(const Option &o)
        {
            for (const Pair &p : o.pairs)
                if (const SettingRow *r = rowFor(p.key))
                {
                    const std::string v = valueOf(*r), want = wanted(p, *r);
                    if (v != want && std::find(p.accepted.begin(), p.accepted.end(), v) == p.accepted.end())
                        ps2_settings::detail::choose(r->entry, want);
                }
            g_rowsAt = -1.0;
        }

        // `custom`: a "Custom" entry shown when no option matches (choosing it changes nothing).
        void mergedRow(const char *id, const char *label, const char *help, const std::vector<Option> &options, bool custom)
        {
            bool pending = false, available = false, any = false;
            for (const Option &o : options)
                for (const Pair &p : o.pairs)
                    if (const SettingRow *r = rowFor(p.key))
                    {
                        any = true;
                        pending = pending || r->pending;
                        available = available || r->available;
                    }
            if (!any)
                return;
            if (g_list)
            {
                std::string keys;
                for (const Pair &p : options.front().pairs)
                    keys += (keys.empty() ? "" : "+") + std::string(p.key);
                std::string now = custom ? "Custom" : "Other";
                for (const Option &o : options)
                    if (matches(o))
                    {
                        now = o.label;
                        break;
                    }
                listItem(std::string(label) + " = " + now + " (merged: " + keys + ")");
                return;
            }
            int current = -1;
            std::vector<std::string> labels;
            for (size_t i = 0; i < options.size(); ++i)
            {
                labels.push_back(options[i].label);
                if (current < 0 && matches(options[i]))
                    current = static_cast<int>(i);
            }
            if (current < 0)
            {
                labels.push_back(custom ? "Custom" : "Other");
                current = static_cast<int>(labels.size()) - 1;
            }
            const int chosen = menu_style::choiceRow(id, label, labels, current, available, pending ? "RESTART" : nullptr, help);
            if (chosen >= 0 && chosen < static_cast<int>(options.size()) && chosen != current)
                apply(options[static_cast<size_t>(chosen)]);
        }

        // Graphics presets. Ultra is everything at its highest (the sub-distances at their last choice, the Trees row's
        // "Full 3D at any distance" - TODO 194; the other presets leave Trees off).
        const char *const kUltraMax[] = {"TF_DRAW_DISTANCE", "TF_SCENERY_DISTANCE", "TF_FOREST_DISTANCE", "TF_GRASS_DISTANCE",
                                         "TF_TERRAIN_DETAIL", "TF_FOREST_FULL_DETAIL"};
        std::vector<Option> presets()
        {
            const auto preset = [](const char *name, const char *draw, const char *enemy, const char *edge, const char *msaa,
                                   const char *ssaa, const char *quality, std::vector<std::string> qualityAlso)
            {
                Option o{name, {}};
                o.pairs.push_back({"TF_DRAW_DISTANCE", draw, {}});
                for (const char *k : {"TF_SCENERY_DISTANCE", "TF_FOREST_DISTANCE", "TF_GRASS_DISTANCE", "TF_TERRAIN_DETAIL", "TF_FOREST_SPRITES"})
                    o.pairs.push_back({k, "", {}}); // (Auto: from Draw distance)
                o.pairs.push_back({"TF_FOREST_ADAPTIVE", "0", {}});
                o.pairs.push_back({"TF_FOREST_FULL_DETAIL", "0", {}}); // (Ultra: kUltraMax)
                o.pairs.push_back({"TF_ENEMY_DETAIL", enemy, {}});
                o.pairs.push_back({"TF_NATIVE_AA", edge, {}});
                o.pairs.push_back({"PS2X_GPU_MSAA", msaa, {}});
                o.pairs.push_back({"TF_NATIVE_SSAA", ssaa, {}});
                o.pairs.push_back({"TF_NATIVE_QUALITY", quality, std::move(qualityAlso)});
                o.pairs.push_back({"TF_SHARP_TEXTURES", "", {}});
                return o;
            };
            std::vector<Option> list{preset("Low", "1", "1", "1", "0", "0", "low", {}),
                                     preset("Medium", "2", "4", "1", "4", "0", "medium", {"auto"}), // (today's defaults)
                                     preset("High", "3", "4", "2", "4", "0", "high", {}),
                                     preset("Ultra", "4", "4", "2", "8", "1", "ultra", {})};
            Option &ultra = list.back();
            for (Pair &p : ultra.pairs)
            {
                if (std::find_if(std::begin(kUltraMax), std::end(kUltraMax), [&](const char *k) { return std::strcmp(k, p.key) == 0; }) !=
                    std::end(kUltraMax))
                    p.value = kMax;
                if (std::strcmp(p.key, "TF_FOREST_SPRITES") == 0)
                    p.value = "1"; // (3D to the edge)
            }
            return list;
        }

        std::vector<Option> antiAliasing()
        {
            if (ps2_settings::value("TF_RENDERER") == "native")
                return {{"Off", {{"TF_NATIVE_AA", "0", {}}, {"PS2X_GPU_MSAA", "0", {}}, {"TF_NATIVE_SSAA", "0", {}}}},
                        {"Low", {{"TF_NATIVE_AA", "1", {}}, {"PS2X_GPU_MSAA", "0", {}}, {"TF_NATIVE_SSAA", "0", {}}}},
                        {"Medium", {{"TF_NATIVE_AA", "1", {}}, {"PS2X_GPU_MSAA", "4", {}}, {"TF_NATIVE_SSAA", "0", {}}}},
                        {"High", {{"TF_NATIVE_AA", "2", {}}, {"PS2X_GPU_MSAA", "4", {}}, {"TF_NATIVE_SSAA", "0", {}}}},
                        {"Ultra", {{"TF_NATIVE_AA", "2", {}}, {"PS2X_GPU_MSAA", "8", {}}, {"TF_NATIVE_SSAA", "1", {}}}}};
            return {{"Off", {{"PS2X_GPU_AA", "0", {}}, {"PS2X_GPU_MSAA", "0", {}}}},
                    {"Low", {{"PS2X_GPU_AA", "1", {}}, {"PS2X_GPU_MSAA", "0", {}}}},
                    {"Medium", {{"PS2X_GPU_AA", "1", {}}, {"PS2X_GPU_MSAA", "4", {}}}},
                    {"High", {{"PS2X_GPU_AA", "1", {}}, {"PS2X_GPU_MSAA", "8", {}}}}};
        }

        std::vector<Option> aspectRatios()
        {
            const auto wide = [](const char *label, const char *aspect)
            { return Option{label, {{"PS2X_WIDESCREEN", "1", {}}, {"TF_ASPECT", aspect, {}}}}; };
            return {{"4:3", {{"PS2X_WIDESCREEN", "0", {}}}}, wide("16:9", "16:9"), wide("21:9", "21:9"), wide("Match screen", "screen"),
                    wide("Ultrawide 2.4:1", "2.4")};
        }

        std::vector<Option> buttonPrompts()
        {
            // Match device: the keys while the keyboard was used last, else the pad's own buttons (PlayStation or
            // Xbox, from its name: TF_PAD_PROMPTS auto, PR #11). TF_PAD_PROMPTS auto only follows the pad with the
            // key prompts on (nr_prompt_style.h), so an older settings.ini without it still reads as it did:
            // PlayStation and PS2 accept auto. (An old ps2 + key prompts on still shows PS2: prompts 1 is accepted
            // there; Xbox keeps either symbol set, so choosing it needs no restart.)
            return {{"Match device", {{"TF_BUTTON_SYMBOLS", "ps5", {}}, {"TF_KEY_PROMPTS", "1", {}}, {"TF_PAD_PROMPTS", "auto", {}}}},
                    {"PlayStation", {{"TF_BUTTON_SYMBOLS", "ps5", {}}, {"TF_KEY_PROMPTS", "0", {}}, {"TF_PAD_PROMPTS", "ps", {"auto"}}}},
                    {"Xbox", {{"TF_BUTTON_SYMBOLS", "ps5", {"ps2"}}, {"TF_KEY_PROMPTS", "0", {}}, {"TF_PAD_PROMPTS", "xbox", {}}}},
                    {"PS2 (original)",
                     {{"TF_BUTTON_SYMBOLS", "ps2", {}}, {"TF_KEY_PROMPTS", "0", {"1"}}, {"TF_PAD_PROMPTS", "ps", {"auto"}}}}};
        }

        std::vector<Option> volumes()
        {
            std::vector<Option> list{{"Off", {{"PS2X_AUDIO", "0", {}}}}};
            if (const SettingRow *v = rowFor("PS2X_AUDIO_VOLUME"))
                for (const auto &c : v->choices)
                    list.push_back({c.label, {{"PS2X_AUDIO", "1", {}}, {"PS2X_AUDIO_VOLUME", c.value, {}}}});
            return list;
        }

        // The keys the layouts place (the merged rows' too): left out of the leftover rows.
        const char *const kPlaced[] = {
            "PS2X_FULLSCREEN", "PS2X_MONITOR", "PS2X_WIDESCREEN", "TF_ASPECT", "TF_FOV", "TF_HQ_FOV", "TF_FPS_CAP", "PS2X_SHOW_FPS",
            "TF_FRAME_RATE", "TF_FPS_LOCK", "TF_FRAME_PACING", "TF_CRISP_TEXT", "TF_BUTTON_SYMBOLS", "TF_KEY_PROMPTS", "TF_PAD_PROMPTS",
            "TF_SMALL_CROSSHAIR", "TF_HQ_PANELS", "TF_DISTANCE_METER", "TF_SPEEDRUN_TIMER", "TF_RENDERER", "PS2X_GPU_SCALE",
            "TF_DRAW_DISTANCE", "TF_SCENERY_DISTANCE", "TF_FOREST_DISTANCE", "TF_FOREST_SPRITES", "TF_GRASS_DISTANCE",
            "TF_TERRAIN_DETAIL", "TF_FOREST_ADAPTIVE", "TF_FOREST_FULL_DETAIL", "TF_ENEMY_DETAIL", "TF_NATIVE_AA", "PS2X_GPU_AA", "PS2X_GPU_MSAA",
            "TF_NATIVE_SSAA", "TF_NATIVE_QUALITY", "TF_SHARP_TEXTURES", "PS2X_TEXTURE_PACK", "PS2X_GPU_ANISOTROPY", "TF_HIRES",
            "PS2X_MOUSE_SENSITIVITY", "TF_MENU_MOUSE", "PS2X_SONYPAD_RUMBLE", "PS2X_SONYPAD_LIGHTBAR", "TF_LIGHTBAR_ALERTS", "TF_ADAPTIVE_TRIGGERS", "TF_TRIGGER_STRENGTH", "TF_PAD_SPEAKER", "TF_PAD_SPEAKER_VOLUME", "TF_PAD_SPEAKER_DELAY", "PS2X_GAMEPAD_BACKEND",
            "PS2X_STICK_LEFT_DEADZONE", "PS2X_STICK_RIGHT_DEADZONE",
            "PS2X_STICK_RESPONSE", "PS2X_STICK_VEHICLE_MODE", "PS2X_STICK_LEFT_LIFT", "PS2X_STICK_LEFT_SENSITIVITY",
            "PS2X_STICK_RIGHT_LIFT", "PS2X_STICK_VEHICLE_LIFT", "PS2X_STICK_VEHICLE_RESPONSE",
            "TF_CAR_KEYS", "PS2X_AUDIO", "PS2X_AUDIO_VOLUME", "TF_NATIVE_SOUND", "TF_CFG_showHUD", "TF_START_SCREENS",
            "TF_ENEMY_WAVES", "TF_PLAY_RECORDING", "TF_UPDATE_CHECK"};
        bool laidOut(const std::string &key)
        {
            return std::find_if(std::begin(kPlaced), std::end(kPlaced), [&](const char *k) { return key == k; }) != std::end(kPlaced);
        }

        bool anyRows(Page page, const char *section = nullptr)
        {
            for (const SettingRow &r : rowsNow())
                if (pageOf(r) == page && rowShown(r) && !laidOut(r.key) && (!section || r.section == section))
                    return true;
            return false;
        }

        // A page's rows no layout names (of one section, when given; all but one, `notSection`).
        void settingRows(Page page, const char *section = nullptr, const char *notSection = nullptr)
        {
            std::vector<std::string> keys;
            for (const SettingRow &r : rowsNow())
                if (pageOf(r) == page && rowShown(r) && !laidOut(r.key) && (!section || r.section == section) &&
                    (!notSection || r.section != notSection))
                    keys.push_back(r.key);
            for (const std::string &key : keys)
                keyRow(key.c_str());
        }

        void pageDisplay()
        {
            pageHeading("Screen");
            keyRow("PS2X_FULLSCREEN");
            keyRow("PS2X_MONITOR");
            mergedRow("aspect", "Aspect ratio", "The shape of the picture. Wider shows more at the sides.", aspectRatios(), false);
            keyRow("TF_FOV");
            if (advanced("display-screen"))
                keyRow("TF_HQ_FOV");
            advancedEnd();
            pageHeading("Frame rate");
            keyRow("TF_FPS_CAP");
            keyRow("PS2X_SHOW_FPS");
            if (advanced("display-rate"))
            {
                keyRow("TF_FRAME_RATE");
                keyRow("TF_FPS_LOCK");
                keyRow("TF_FRAME_PACING");
            }
            advancedEnd();
            pageHeading("Interface");
            keyRow("TF_CRISP_TEXT");
            // (Sharp text on, but its fonts never made - the HD textures step skipped or stopped before them: said
            // plainly, not the old letters in silence.)
            if (!g_list)
                if (const SettingRow *crisp = rowFor("TF_CRISP_TEXT"); crisp && crisp->available && valueOf(*crisp) != "0" && !hd_progress::crispFontsFound())
                    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Sharp text: not made yet - the HD textures step makes them");
            mergedRow("prompts", "Button prompts",
                      "Which buttons the game shows in its hints. Match device: your keys, or your controller's buttons.",
                      buttonPrompts(), false);
            keyRow("TF_SMALL_CROSSHAIR");
            keyRow("TF_HQ_PANELS");
            keyRow("TF_DISTANCE_METER");
            keyRow("TF_SPEEDRUN_TIMER"); // (kept in settings.ini, TODO 74)
            settingRows(Page::Display);
            if (!g_list && speedrun_timer::enabled())
            {
                ImGui::Dummy(ImVec2(0.0f, 4.0f));
                if (menu_style::button("Reset the timer"))
                    speedrun_timer::reset();
                const speedrun_timer::View v = speedrun_timer::view();
                for (size_t i = 0; i < v.splits.size(); ++i)
                    ImGui::TextDisabled("Level %zu   %s", i + 1, speedrun_timer::format(v.splits[i]).c_str());
            }
        }

        void pageGraphics()
        {
            mergedRow("preset", "Preset", "One choice for all the graphics settings.", presets(), true);
            keyRow("TF_RENDERER");
            keyRow("PS2X_GPU_SCALE");
            keyRow("TF_DRAW_DISTANCE");
            if (advanced("graphics-distance"))
            {
                keyRow("TF_SCENERY_DISTANCE");
                keyRow("TF_FOREST_DISTANCE");
                keyRow("TF_FOREST_SPRITES");
                keyRow("TF_FOREST_FULL_DETAIL"); // (TODO 194: a big cost, so under Advanced; Ultra turns it on)
                keyRow("TF_GRASS_DISTANCE");
                keyRow("TF_TERRAIN_DETAIL");
                keyRow("TF_FOREST_ADAPTIVE");
            }
            advancedEnd();
            keyRow("TF_ENEMY_DETAIL");
            mergedRow("antialiasing", "Anti-aliasing", "Smooths jagged edges. Higher costs more.", antiAliasing(), true);
            if (advanced("graphics-aa"))
            {
                keyRow("TF_NATIVE_AA");
                keyRow("PS2X_GPU_AA");
                keyRow("PS2X_GPU_MSAA");
                keyRow("TF_NATIVE_SSAA");
            }
            advancedEnd();
            keyRow("TF_NATIVE_QUALITY");
            // (TODO 243) The HD textures (the native renderer's, made from the disc) on or off, with their buttons. The
            // texture pack rows are the emulated renderer's PCSX2 packs, a different thing: shown only with it.
            const bool native = ps2_settings::value("TF_RENDERER") == "native";
            keyRow("TF_HIRES");
            {
                // (The folder the HD textures are loaded from, in Explorer: hd_progress.h)
                if (!g_list)
                {
                    static std::string note;
                    static double noteAt = -100.0;
                    ImGui::Dummy(ImVec2(0.0f, 4.0f));
                    if (menu_style::button("Open texture folder"))
                    {
                        note = hd_progress::openTextureFolder();
                        noteAt = ImGui::GetTime();
                    }
                    // (TODO 219) The HD step for a player who skipped it or never finished it, in its own small window.
                    static std::string makeLabel;
                    static double labelAt = -100.0;
                    if (ImGui::GetTime() - labelAt > 2.0)
                    {
                        makeLabel = hd_progress::makeLabel();
                        labelAt = ImGui::GetTime();
                    }
                    ImGui::SameLine();
                    if (menu_style::button(makeLabel.c_str()))
                    {
                        note = hd_progress::makeHdTextures();
                        noteAt = ImGui::GetTime();
                    }
                    if (!note.empty() && ImGui::GetTime() - noteAt < 10.0)
                        ImGui::TextDisabled("%s", note.c_str());
                }
                else
                    listItem("Open texture folder, Make HD textures (buttons)");
            }
            if (advanced("graphics-textures"))
            {
                keyRow("TF_SHARP_TEXTURES");
                if (!native)
                {
                    keyRow("PS2X_TEXTURE_PACK");
                    keyRow("PS2X_GPU_ANISOTROPY");
                }
            }
            advancedEnd();
            settingRows(Page::Graphics);
        }

        void pageControls()
        {
            pageHeading("Mouse");
            keyRow("PS2X_MOUSE_SENSITIVITY");
            keyRow("TF_MENU_MOUSE");
            settingRows(Page::Controls, "Mouse");
            pageHeading("Controller");
            keyRow("PS2X_SONYPAD_RUMBLE");
            keyRow("PS2X_SONYPAD_LIGHTBAR");
            keyRow("TF_LIGHTBAR_ALERTS");
            keyRow("TF_ADAPTIVE_TRIGGERS");
            keyRow("TF_TRIGGER_STRENGTH");
            keyRow("TF_PAD_SPEAKER");
            keyRow("TF_PAD_SPEAKER_VOLUME");
            keyRow("TF_PAD_SPEAKER_DELAY");
            keyRow("PS2X_STICK_LEFT_DEADZONE");
            keyRow("PS2X_STICK_RIGHT_DEADZONE");
            keyRow("PS2X_STICK_RESPONSE");
            keyRow("PS2X_STICK_VEHICLE_MODE");
            if (advanced("controls-pad"))
            {
                keyRow("PS2X_STICK_LEFT_LIFT");
                keyRow("PS2X_STICK_LEFT_SENSITIVITY");
                keyRow("PS2X_STICK_RIGHT_LIFT");
                keyRow("PS2X_STICK_VEHICLE_LIFT");
                keyRow("PS2X_STICK_VEHICLE_RESPONSE");
                keyRow("PS2X_GAMEPAD_BACKEND");
            }
            advancedEnd();
            settingRows(Page::Controls, "Controller");
            pageHeading("Controller buttons");
            if (!g_list)
                drawPadButtons();
            else
                listItem("the controller's buttons (a table: Change, Triggers fire, Reset to default)");
            pageHeading("Keyboard");
            keyRow("TF_CAR_KEYS");
            settingRows(Page::Controls, "Keyboard");
            pageHeading("Keyboard and mouse buttons");
            if (!g_list)
                drawControls();
        }

        void pageSound()
        {
            pageHeading("Sound");
            mergedRow("volume", "Volume", "How loud the game plays. Off: no sound.", volumes(), false);
            if (advanced("sound"))
                keyRow("TF_NATIVE_SOUND");
            advancedEnd();
            settingRows(Page::Sound);
        }

        void pageGameplay()
        {
            pageHeading("Camera");
            if (g_list)
                listItem("Camera (merged: first person + aim)");
            else
            {
                // (TF_FIRST_PERSON + TF_FIRST_PERSON_AIM, camera_mode.h)
                const bool first = camera_mode::firstPerson(), aim = camera_mode::aim();
                const int current = !first ? 0 : aim ? 2 : 1;
                const int chosen = menu_style::choiceRow("camera", "Camera", {"Third person", "First person", "First person, aim on foot"},
                                                         current, true, nullptr,
                                                         "First person is for the robot; the car keeps its own camera. Aim on foot: the game's own aiming view (strafe, R3 zooms, no run or jump).");
                if (chosen >= 0 && chosen != current)
                {
                    camera_mode::setFirstPerson(chosen != 0);
                    if (chosen != 0)
                        camera_mode::setAim(chosen == 2);
                }
                ImGui::Dummy(ImVec2(0.0f, 4.0f));
                if (menu_style::button("Photo mode  (F8)", true))
                {
                    photo_mode::toggle();
                    g_open = false;
                }
                if (ImGui::IsItemHovered())
                    menu_style::setHelp("Freeze the game and fly the camera (mouse to look, WASD to move, F8 or Esc to leave).");
            }
            pageHeading("Game");
            keyRow("TF_CFG_showHUD");
            keyRow("TF_START_SCREENS");
            settingRows(Page::Gameplay);
            pageHeading("Cheat codes");
            if (!g_list)
                drawCheats();
        }

        // Mods & cheats (TODO 73): what changes the game, in one place.
        void pageMods()
        {
            pageHeading("The prototype's features");
            if (!g_list)
                ext::menuSection("Prototype"); // (an extension's controls: the jet bike's Summon, port_ext.h)
            settingRows(Page::Mods, "Prototype");
            settingRows(Page::Mods, "Jet bike"); // (TODO 92: its own settings)
            pageHeading("Gameplay");
            keyRow("TF_ENEMY_WAVES");
            keyRow("TF_PLAY_RECORDING");
            pageHeading("Unlocks");
            if (!g_list)
                ext::menuSection("Unlocks"); // (an extension's: the Levels unlocks, levels_unlock.cpp)
            pageHeading("Tuning and cheats");
            settingRows(Page::Mods, "Cheats");
            pageHeading("Mods folder");
            if (!g_list)
                ext::menuSection("Mods folder"); // (the mod loader's list: mod_loader.cpp)
        }

        // Debug (the last page, TODO 73 (b)): for testing - the emulated GPU's own switches, turbo loading, the
        // developers' test options, and anything not placed elsewhere.
        void closeMenu() { g_open = false; }

        void pageOther()
        {
            pageHeading("Play recordings");
            if (!g_list)
                state_manager::drawRecordings(closeMenu);
            pageHeading("For testing");
            if (!g_list)
                ImGui::TextDisabled("Switches for tests and the developers' own options (most of these take effect from the next level).");
            settingRows(Page::Other);
        }

        void pageSaves(PS2Runtime *runtime)
        {
            pageHeading("Saves");
            if (!g_list)
                drawSaves();
            // (TODO 156) Save states by name (state_manager.h)
            pageHeading("Save states");
            if (!g_list)
                state_manager::drawStates(*runtime, closeMenu);
        }

        bool pageShown(Page page)
        {
#if defined(TF_OVERLAY)
            (void)page;
            return true; // (Debug always: its recordings - TODO 156)
#else
            // The public build (no overlay): no Mods & cheats page and no Debug page - their rows are the overlay's.
            return page != Page::Mods && page != Page::Other;
#endif
        }

        // (TODO 277) Quit game: out of the game to Windows, asked twice (the page, then "Yes, quit"). The same clean stop
        // as closing the window (PS2Runtime::requestStop: the main loop ends, the game's thread is joined, the files it
        // has open are closed); the settings are written the moment they change, the memory card when the game saves.
        // TF_QUIT_BUTTON_AT=<tick> (tests): "Yes, quit" pressed at that VBlank, as the button does it.
        uint64_t g_quitAskedFrame = 0u, g_quitPageFrame = 0u;
        void quitNow(PS2Runtime &runtime)
        {
            std::fprintf(stderr, "[mod menu] Quit game: the game is closed from the menu\n");
            runtime.requestStop();
        }
        void pageQuit(PS2Runtime &runtime)
        {
            pageHeading("Quit game");
            if (g_list)
            {
                listItem("Quit game (asks first)");
                return;
            }
            // (asked again each time the page is opened: the question does not wait behind another page)
            static uint64_t frame = 0u;
            ++frame;
            if (g_quitPageFrame + 1u != frame)
                g_quitAskedFrame = 0u;
            g_quitPageFrame = frame;
            ImGui::TextWrapped("Close the game and go back to Windows.");
            ImGui::TextWrapped("Your settings are kept. What you did since the game last saved is lost: save first if you need to.");
            ImGui::Dummy(ImVec2(0.0f, menu_style::textSize() * 0.6f));
            if (g_quitAskedFrame == 0u)
            {
                if (menu_style::button("Quit game", true))
                    g_quitAskedFrame = frame;
            }
            else
            {
                ImGui::TextWrapped("Quit now?");
                if (menu_style::button("Yes, quit", true) && frame > g_quitAskedFrame + 10u) // (not by the same press held on)
                    quitNow(runtime);
                ImGui::SameLine();
                if (menu_style::button("No, keep playing"))
                    g_quitAskedFrame = 0u;
            }
        }

        void drawPage(Page page, PS2Runtime &runtime)
        {
            switch (page)
            {
            case Page::Display:
                pageDisplay();
                break;
            case Page::Graphics:
                pageGraphics();
                break;
            case Page::Controls:
                pageControls();
                break;
            case Page::Sound:
                pageSound();
                break;
            case Page::Gameplay:
                pageGameplay();
                break;
            case Page::Saves:
                pageSaves(&runtime);
                break;
            case Page::Mods:
                pageMods();
                break;
            case Page::Other:
                pageOther();
                break;
            case Page::Quit:
                pageQuit(runtime);
                break;
            case Page::About:
                // The version, the update check and updating (TODO 104: updates.h).
                if (g_list)
                    keyRow("TF_UPDATE_CHECK");
                else
                    updates::drawAbout(runtime, [] { keyRow("TF_UPDATE_CHECK"); settingRows(Page::About); });
                break;
            }
        }

        bool anyPending()
        {
            if (ext::restartPending()) // (a mod switched on or off: port_ext.h)
                return true;
            for (const SettingRow &r : rows())
                if (r.pending)
                    return true;
            return false;
        }

        // TF_MOD_MENU_LIST=1 (testing aid): the tabs and each one's settings, once, to the log - the
        // overlay's layout without a picture of it (from mod_menu::frame: a hidden test window draws
        // no overlay).
        void listTabs()
        {
            g_listRows = ps2_settings::detail::rows();
            for (const PageInfo &p : kPages)
            {
                std::string line;
                g_list = &line;
                switch (p.page)
                {
                case Page::Display: pageDisplay(); break;
                case Page::Graphics: pageGraphics(); break;
                case Page::Controls: pageControls(); break;
                case Page::Sound: pageSound(); break;
                case Page::Gameplay: pageGameplay(); break;
                case Page::Saves: pageSaves(nullptr); break;
                case Page::Mods: pageMods(); break;
                case Page::Other: pageOther(); break;
                case Page::About: keyRow("TF_UPDATE_CHECK"); settingRows(Page::About); break;
                case Page::Quit: listItem("Quit game (asks first)"); break;
                default: break;
                }
                g_list = nullptr;
                std::fprintf(stderr, "[mod menu] tab \"%s\":%s\n", p.name, line.c_str());
            }
            g_listRows.clear();
        }

        // (TODO 212) The menu with a controller. Two levels: the tabs (Up / Down pick one, Cross opens it, Circle closes
        // the menu) and the page (menu_style's focus: Up / Down move it, Cross flips, holds or presses, Circle lets a held
        // row go, then goes back to the tabs). A pad's frames show from its first press until the mouse moves; that first
        // press only shows where the focus is. Directions repeat while held, by real time (any frame rate).
        int g_navLevel = 0;      // 0 the tabs, 1 the page
        bool g_navPad = false;   // the pad drives the menu now (its frames drawn)
        bool g_navEnter = false; // a page just entered: its focus reset inside its window
        bool g_holdBlock = false; // closed with a pad button: the game's input stays blocked until it is let go
        menu_style::NavInput g_navIn;

        struct MenuPad
        {
            bool up = false, down = false, left = false, right = false, cross = false, circle = false, any = false;
            bool xbox = false; // (the button names to show)
        };

        int64_t navNowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // TF_MENU_PAD_SCRIPT=down,down,cross,right,cross,circle,... (a test aid): those presses, one every 250 ms once
        // the menu is open, each held 100 ms, in place of the pad (logged "[mod menu] pad script: ...").
        bool scriptedPad(MenuPad &out)
        {
            static const std::vector<std::string> steps = [] {
                std::vector<std::string> list;
                if (const char *env = std::getenv("TF_MENU_PAD_SCRIPT"))
                {
                    std::string s = env, item;
                    std::stringstream in(s);
                    while (std::getline(in, item, ','))
                        if (!item.empty())
                            list.push_back(item);
                }
                return list;
            }();
            if (steps.empty())
                return false;
            static int64_t start = 0;
            static size_t logged = 0;
            if (!g_open)
                return true; // (nothing pressed until the menu is open)
            if (start == 0)
                start = navNowMs() + 1000; // (a second for the menu to settle)
            const int64_t t = navNowMs() - start;
            if (t < 0)
                return true;
            const size_t step = static_cast<size_t>(t / 250);
            if (step >= steps.size() || t % 250 >= 100)
                return true;
            const std::string &b = steps[step];
            if (logged <= step)
            {
                logged = step + 1;
                std::fprintf(stderr, "[mod menu] pad script: %s\n", b.c_str());
            }
            out.up = b == "up";
            out.down = b == "down";
            out.left = b == "left";
            out.right = b == "right";
            out.cross = b == "cross";
            out.circle = b == "circle";
            out.any = true;
            return true;
        }

        MenuPad readMenuPad()
        {
            MenuPad p;
            if (scriptedPad(p))
                return p;
            if (g_padCapturing >= 0) // (a controller button being chosen: not the menu's)
                return p;
            if (!IsWindowFocused())
                return p;
            const auto stick = [](float v, bool positive) { return positive ? v > 0.6f : v < -0.6f; };
            ps2x::sonypad::InputState s;
            if (ps2x::sonypad::readHostPad(s))
            {
                using namespace ps2x::sonypad;
                const float lx = (static_cast<float>(s.lx) - 128.0f) / 128.0f, ly = (static_cast<float>(s.ly) - 128.0f) / 128.0f;
                p.up = (s.buttons & kUp) != 0u || stick(ly, false);
                p.down = (s.buttons & kDown) != 0u || stick(ly, true);
                p.left = (s.buttons & kLeft) != 0u || stick(lx, false);
                p.right = (s.buttons & kRight) != 0u || stick(lx, true);
                p.cross = (s.buttons & kCross) != 0u;
                p.circle = (s.buttons & kCircle) != 0u;
                p.any = (s.buttons & 0x7FFFFu) != 0u || p.up || p.down || p.left || p.right;
                return p;
            }
            if (!ps2x::sonypad::glfwGamepadsAllowed())
                return p;
            for (int i = 0; i < 4; ++i)
            {
                if (!IsGamepadAvailable(i))
                    continue;
                const float lx = GetGamepadAxisMovement(i, GAMEPAD_AXIS_LEFT_X), ly = GetGamepadAxisMovement(i, GAMEPAD_AXIS_LEFT_Y);
                p.up = IsGamepadButtonDown(i, GAMEPAD_BUTTON_LEFT_FACE_UP) || stick(ly, false);
                p.down = IsGamepadButtonDown(i, GAMEPAD_BUTTON_LEFT_FACE_DOWN) || stick(ly, true);
                p.left = IsGamepadButtonDown(i, GAMEPAD_BUTTON_LEFT_FACE_LEFT) || stick(lx, false);
                p.right = IsGamepadButtonDown(i, GAMEPAD_BUTTON_LEFT_FACE_RIGHT) || stick(lx, true);
                p.cross = IsGamepadButtonDown(i, GAMEPAD_BUTTON_RIGHT_FACE_DOWN);   // (Xbox A)
                p.circle = IsGamepadButtonDown(i, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT); // (Xbox B)
                for (int b = GAMEPAD_BUTTON_LEFT_FACE_UP; b <= GAMEPAD_BUTTON_RIGHT_THUMB && !p.any; ++b)
                    p.any = IsGamepadButtonDown(i, b);
                p.any = p.any || p.up || p.down || p.left || p.right;
                p.xbox = true;
                return p;
            }
            return p;
        }

        // This frame's presses: Cross and Circle once per press, a direction at its press and then every 90 ms after
        // it has been held for 350 ms.
        menu_style::NavInput navPresses(const MenuPad &p)
        {
            menu_style::NavInput in;
            static MenuPad was;
            static int64_t heldSince[4] = {0, 0, 0, 0}, lastRepeat[4] = {0, 0, 0, 0};
            const int64_t now = navNowMs();
            const bool dirs[4] = {p.up, p.down, p.left, p.right};
            const bool wasDirs[4] = {was.up, was.down, was.left, was.right};
            bool fire[4] = {false, false, false, false};
            for (int i = 0; i < 4; ++i)
            {
                if (dirs[i] && !wasDirs[i])
                {
                    fire[i] = true;
                    heldSince[i] = lastRepeat[i] = now;
                }
                else if (dirs[i] && now - heldSince[i] >= 350 && now - lastRepeat[i] >= 90)
                {
                    fire[i] = true;
                    lastRepeat[i] = now;
                }
            }
            in.up = fire[0];
            in.down = fire[1];
            in.left = fire[2];
            in.right = fire[3];
            in.confirm = p.cross && !was.cross;
            in.back = p.circle && !was.circle;
            was = p;
            return in;
        }

        bool anyPress(const menu_style::NavInput &in)
        {
            return in.up || in.down || in.left || in.right || in.confirm || in.back;
        }

        // The tabs, one up or down among the shown ones.
        void stepPage(int direction)
        {
            const int n = static_cast<int>(std::size(kPages));
            for (int k = 1; k <= n; ++k)
            {
                const int i = ((g_page + direction * k) % n + n) % n;
                if (pageShown(kPages[i].page))
                {
                    g_page = i;
                    return;
                }
            }
        }

        // Once a frame the menu is open (draw, before drawMenu): the pad's presses for the tabs; the page's go to
        // menu_style in drawMenu.
        void navFrame()
        {
            const MenuPad pad = readMenuPad();
            menu_style::NavInput in = navPresses(pad);
            if (ImGui::GetCurrentContext())
            {
                const ImGuiIO &io = ImGui::GetIO();
                if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f || ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    g_navPad = false;
            }
            if (anyPress(in) && !g_navPad)
            {
                g_navPad = true; // (the first press shows the focus; it does nothing else)
                in = menu_style::NavInput{};
            }
            g_navIn = menu_style::NavInput{};
            if (!g_navPad)
                return;
            if (g_navLevel == 0)
            {
                if (in.up)
                    stepPage(-1);
                if (in.down)
                    stepPage(1);
                if (in.confirm)
                {
                    g_navLevel = 1;
                    g_navEnter = true;
                }
                else if (in.back)
                {
                    g_open = false;
                    g_holdBlock = true; // (Circle is still down: the game must not take it)
                }
                return;
            }
            g_navIn = in; // (the page's: drawMenu)
        }

        // With TF_MENU_PAD_SCRIPT (tests): where the pad is, each time it changes.
        void navLog()
        {
            static const bool on = std::getenv("TF_MENU_PAD_SCRIPT") != nullptr;
            static std::string was;
            if (!on)
                return;
            char now[160];
            std::snprintf(now, sizeof(now), "open %d, %s, page \"%s\"%s", g_open ? 1 : 0, g_navLevel == 0 ? "the tabs" : "the page",
                          kPages[g_page].name, menu_style::navHolding() ? ", a row held" : "");
            if (was != now)
            {
                was = now;
                std::fprintf(stderr, "[mod menu] pad: %s\n", now);
            }
        }

        // The keys named in the header: the pad's when one is connected.
        const char *menuKeys()
        {
            ps2x::sonypad::InputState s;
            if (ps2x::sonypad::readHostPad(s))
                return "PS  CLOSE     CROSS  SELECT     CIRCLE  BACK";
            if (IsGamepadAvailable(0))
                return "VIEW + MENU  CLOSE     A  SELECT     B  BACK";
            return "F1  CLOSE     ESC  BACK";
        }

        // The whole menu: a full-window screen over the game (its picture dimmed behind).
        void drawMenu(PS2Runtime &runtime)
        {
            using namespace menu_style;
            const float w = static_cast<float>(GetScreenWidth()), h = static_cast<float>(GetScreenHeight());
            const float size = textSize();
            const float pad = size * 1.6f;
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(ImVec2(w, h));
            const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                           ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
                                           ImGuiWindowFlags_NoScrollWithMouse;
            if (!ImGui::Begin("##command-console", nullptr, flags))
            {
                ImGui::End();
                return;
            }
            // (TODO 212) The pad's presses for the page this frame (the tabs took theirs in navFrame).
            navBegin(g_navPad && g_navLevel == 1 && !g_navEnter, g_navIn);
            ImDrawList *dl = ImGui::GetWindowDrawList();
            // The backdrop: the game dimmed, darker to the left where the menu is.
            dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w, h), IM_COL32(6, 8, 12, 238), IM_COL32(6, 8, 12, 200),
                                        IM_COL32(6, 8, 12, 200), IM_COL32(6, 8, 12, 238));
            for (float y = 0.0f; y < h; y += 4.0f) // (faint scan lines)
                dl->AddLine(ImVec2(0.0f, y), ImVec2(w, y), IM_COL32(255, 255, 255, 5));

            // The header: the title, and the keys.
            const Fonts &f = fonts();
            const float titleSize = size * 2.5f;
            dl->AddText(f.title, titleSize, ImVec2(pad, pad * 0.6f), IM_COL32(255, 255, 255, 255), "TRANSFORMERS");
            const ImVec2 titleExtent = f.title->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, "TRANSFORMERS");
            dl->AddText(f.heading, size * 0.62f, ImVec2(pad + size * 0.1f, pad * 0.6f + titleExtent.y + size * 0.1f), colour("blue"),
                        "ARMADA   //   COMMAND CONSOLE");
            {
                char fps[48];
                std::snprintf(fps, sizeof(fps), "%.0f FPS", ImGui::GetIO().Framerate);
                const char *keys = menuKeys();
                const ImVec2 ke = f.heading->CalcTextSizeA(size * 0.55f, FLT_MAX, 0.0f, keys);
                dl->AddText(f.heading, size * 0.55f, ImVec2(w - pad - ke.x, pad * 0.8f), colour("dim"), keys);
                const ImVec2 fe = f.heading->CalcTextSizeA(size * 0.55f, FLT_MAX, 0.0f, fps);
                dl->AddText(f.heading, size * 0.55f, ImVec2(w - pad - fe.x, pad * 0.8f + ke.y + 6.0f), colour("dim"), fps);
                float lineY = pad * 0.8f + ke.y + fe.y + 12.0f;
                if (updates::updateAvailable()) // (TODO 104: a newer version - the About page)
                {
                    const char *update = "UPDATE AVAILABLE - ABOUT";
                    const ImVec2 ue = f.heading->CalcTextSizeA(size * 0.55f, FLT_MAX, 0.0f, update);
                    dl->AddText(f.heading, size * 0.55f, ImVec2(w - pad - ue.x, lineY), colour("amber"), update);
                    lineY += ue.y + 6.0f;
                }
                // (TODO 219) The HD textures still being made: how far they are.
                if (const std::string hd = hd_progress::line(); !hd.empty())
                {
                    const ImVec2 he = f.heading->CalcTextSizeA(size * 0.55f, FLT_MAX, 0.0f, hd.c_str());
                    dl->AddText(f.heading, size * 0.55f, ImVec2(w - pad - he.x, lineY), colour("blue"), hd.c_str());
                }
            }
            const float headerBottom = pad * 0.6f + titleExtent.y + size * 1.3f;
            // the red rule under it, cut off at the right like the HQ's panels
            const ImVec2 rule[4] = {{pad, headerBottom}, {w - pad - size, headerBottom}, {w - pad - size * 1.4f, headerBottom + 3.0f},
                                    {pad, headerBottom + 3.0f}};
            dl->AddConvexPolyFilled(rule, 4, colour("red"));
            dl->AddRectFilled(ImVec2(w - pad - size * 0.9f, headerBottom), ImVec2(w - pad, headerBottom + 3.0f), colour("blue"));

            const float footerHeight = size * 3.4f;
            const float top = headerBottom + size * 1.2f, bottom = h - footerHeight;
            const float sideWidth = std::clamp(w * 0.18f, size * 10.0f, size * 14.0f);

            // The side bar.
            ImGui::SetCursorScreenPos(ImVec2(pad, top));
            ImGui::BeginChild("##side", ImVec2(sideWidth, bottom - top), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
            for (int i = 0; i < static_cast<int>(std::size(kPages)); ++i)
            {
                if (!pageShown(kPages[i].page))
                    continue;
                if (sideTab(kPages[i].name, i == g_page, sideWidth))
                {
                    g_page = i;
                    g_navLevel = 0;
                }
                // (TODO 277: the list is one entry longer - on a small window its last entries are below the edge: the
                // chosen one is brought into view when it changes, by the pad or a test's first page)
                if (static int inView = -1; i == g_page && inView != g_page)
                {
                    inView = g_page;
                    ImGui::SetScrollHereY(0.5f);
                }
                if (g_navPad && g_navLevel == 0 && i == g_page) // (the pad's place among the tabs)
                    ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), colour("blue"), 3.0f, 0, 2.0f);
                ImGui::Dummy(ImVec2(0.0f, size * 0.15f));
            }
            ImGui::EndChild();

            // The page.
            const float pageX = pad + sideWidth + pad;
            const float pageWidth = w - pageX - pad;
            ImGui::SetCursorScreenPos(ImVec2(pageX, top));
            dl->AddRectFilled(ImVec2(pageX - size * 0.6f, top), ImVec2(w - pad, bottom), IM_COL32(14, 18, 24, 150));
            dl->AddRectFilled(ImVec2(pageX - size * 0.6f, top), ImVec2(pageX - size * 0.6f + 2.0f, bottom), colour("line"));
            ImGui::BeginChild("##page", ImVec2(pageWidth, bottom - top), ImGuiChildFlags_None, 0);
            if (g_navEnter) // (a page opened with the pad: its first row has the focus, at the top)
            {
                g_navEnter = false;
                navReset();
                navBegin(g_navPad && g_navLevel == 1, menu_style::NavInput{});
            }
            ImGui::PushFont(f.text, size);
            ImGui::PushTextWrapPos(pageWidth - size);
            drawPage(kPages[g_page].page, runtime);
            ImGui::Dummy(ImVec2(0.0f, size));
            ImGui::PopTextWrapPos();
            ImGui::PopFont();
            ImGui::EndChild();

            // The footer: what the pointer is on, and a restart when a choice waits for one.
            dl->AddLine(ImVec2(pad, bottom + size * 0.4f), ImVec2(w - pad, bottom + size * 0.4f), colour("line"), 1.0f);
            const float restartWidth = size * 13.0f;
            const bool modRestart = ext::restartPending();
            const std::string helpText = modRestart ? std::string("Mod changes need a restart. Save your game first; restart returns to the title screen.")
                                         : help().empty() && hd_progress::readyToRestart() && !anyPending()
                                             ? std::string("The HD textures made in the background are ready. Restart to use them: you come back where you are.")
                                         : help().empty() ? std::string("Point at a setting for what it does. Changes are saved at once.")
                                                          : help();
            dl->AddText(f.text, size * 0.85f, ImVec2(pad, bottom + size * 0.9f), colour("dim"), helpText.c_str(), nullptr,
                        w - pad * 2.0f - restartWidth);
            {
                // (TODO 212) The Restart button appearing takes the pad's focus (it is the next widget drawn); Circle
                // gives the focus back to the row that made it appear.
                static bool restartWas = false;
                const bool restartNow = (anyPending() || hd_progress::readyToRestart()) && !ps2_settings::restarting() && !g_modRestarting;
                if (restartNow && !restartWas && g_navPad && g_navLevel == 1)
                    navFocusNext();
                restartWas = restartNow;
            }
            // (TODO 267: HD textures or sharp text made while the game ran: a restart, back where the player is, uses them)
            const bool hdRestart = hd_progress::readyToRestart() && !anyPending();
            if (anyPending() || hdRestart || ps2_settings::restarting() || g_modRestarting)
            {
                ImGui::SetCursorScreenPos(ImVec2(w - pad - restartWidth, bottom + size * 0.8f));
                ImGui::PushFont(f.text, size);
                if (ps2_settings::restarting() || g_modRestarting)
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(colour("amber")), "Restarting...");
                else if (button(modRestart ? "Restart game" : hdRestart ? "Restart for HD textures" : "Restart to apply", true))
                {
                    if (modRestart && ext::restartGame(true))
                    {
                        // (an extension's own restart: it asks first - the mod loader's, port_ext.h)
                    }
                    else if (modRestart)
                    {
                        // File mods replace loaded resources: never resume an old
                        // snapshot after rebuilding the disc with different files.
                        g_modRestartError.clear();
                        if (ps2_settings::detail::startAgain({}, g_modRestartError))
                        {
                            g_modRestarting = true;
                            runtime.requestStop();
                        }
                    }
                    else
                        ps2_settings::restartNow(runtime);
                }
                ImGui::PopFont();
            }
            if (const std::string message = ps2_settings::restartMessage(); !message.empty())
                dl->AddText(f.text, size * 0.75f, ImVec2(pad, bottom + size * 2.2f), colour("amber"), message.c_str());
            if (!g_modRestartError.empty())
                dl->AddText(f.text, size * 0.75f, ImVec2(pad, bottom + size * 2.2f), colour("amber"), g_modRestartError.c_str());
            ext::restartGame(false); // (an extension's restart dialog, while it is open: port_ext.h)
            if (navEnd()) // (Circle in the page, nothing held: back to the tabs)
                g_navLevel = 0;
            ImGui::End();
        }
#endif
    }

    void addSettings()
    {
        updates::addSettings(); // About: checking for updates automatically (updates.h)
        menu_mouse::addSettings(); // Controls: the mouse in the pause menu (menu_mouse.h)
        distance_meter::addSettings(); // Mods & cheats: the prototype's distance meter (distance_meter.h)
        boss_cheat::addSettings(); // Mods & cheats: one-hit kills on bosses (boss_cheat.h, TODO 199)
        speedrun_timer::addSettings(); // Display, On screen: the speedrun timer (speedrun_timer.h)
        start_screens::addSettings();  // Gameplay: what the game shows from boot to its main menu (start_screens.h, TODO 110)
        {
            // (TODO 243) The native renderer's HD textures on or off (TF_HIRES, read when the renderer starts: a restart).
            // Off draws the game's own textures; the sharp text keeps its own row (Sharp text).
            ps2_settings::Setting hd;
            hd.key = "TF_HIRES";
            hd.section = "Graphics";
            hd.label = "HD textures";
            hd.help = "Native renderer: the HD textures made from your disc (hd_textures). Off: the game's own textures. "
                      "Takes effect after a restart.";
            hd.choices = {ps2_settings::Choice{"", "On"}, ps2_settings::Choice{"0", "Off"}};
            hd.defaultValue = "";
            hd.apply = [](const std::string &) { return false; };
            hd.available = []()
            {
                const char *on = std::getenv("TF_NATIVE_RENDER");
                return ps2_settings::choice("TF_RENDERER") == "native" || (on && *on && *on != '0');
            };
            hd.unavailableHelp = "Only for the native renderer (Graphics: Renderer). The emulated one uses texture packs.";
            ps2_settings::add(hd);
        }
        {
            // (TODO 240) The controller's buttons (drawPadButtons): kept in settings.ini, no row of its own (no choices:
            // the Controls page's table sets it). The runtime reads PS2X_PADMAP at start.
            ps2_settings::Setting pad;
            pad.key = "PS2X_PADMAP";
            pad.section = "Controller";
            pad.label = "Controller buttons";
            pad.defaultValue = padDefault();
#if defined(__linux__)
            if (*padDefault() && !std::getenv("PS2X_PADMAP"))
            {
                setenv("PS2X_PADMAP", padDefault(), 1); // (no saved layout: the default, as the runtime reads it)
                ps2_host_input::setPadBindings(padDefault());
                std::fprintf(stderr, "[pad] controller buttons: %s (a Steam Deck's default)\n", ps2_host_input::padBindings().c_str());
            }
#endif
            pad.apply = [](const std::string &value)
            {
                ps2_host_input::setPadBindings(value);
                return true;
            };
            ps2_settings::add(pad);
        }
#if defined(PS2X_ENABLE_DEBUG_UI)
        if (const char *env = std::getenv("TF_CAR_KEYS"); env && *env)
            g_carKeys.store(*env != '0');
        ps2_settings::Setting s;
        s.key = "TF_CAR_KEYS";
        s.section = "Keyboard";
        s.label = "Drive with the movement keys";
        s.order = 10;
        s.help = "In the car, the keys that move the Autobot forward and back (W and S) accelerate and brake, as "
                 "Cross and Square do (the car does not go by the stick).";
        s.choices = {{"1", "On"}, {"0", "Off"}};
        s.defaultValue = "1";
        s.apply = [](const std::string &value)
        {
            g_carKeys.store(value != "0");
            return true;
        };
        ps2_settings::add(s);

        ps2_settings::Setting k;
        k.key = "TF_KEY_PROMPTS";
        k.section = "Keyboard";
        k.label = "On-screen button prompts";
        k.order = 20;
        k.help = "The game's button icons (Cross, Triangle, L1, ...) drawn as the keys and mouse buttons bound to them. "
                 "Following the device: the keys while you play with the keyboard and mouse, the controller's own buttons "
                 "while you play with a gamepad. Rebinding on this page draws them again (at the next start). In the native "
                 "renderer the keys come with the sharp font sets (Display: Sharp text / Button symbols).";
        k.choices = {{"1", "Follow the device"}, {"0", "PlayStation"}};
        k.defaultValue = "1";
        k.apply = [](const std::string &value)
        {
            g_keyPrompts.store(value != "0");
            ps2_host_input::setKeyboardPrompts(value != "0");
            return true;
        };
        ps2_settings::add(k);
        g_keyPrompts.store(ps2_settings::value("TF_KEY_PROMPTS") != "0");
        ps2_host_input::setKeyboardPrompts(g_keyPrompts.load());
        moveKeyPrompts(); // (before the renderer reads the pack)
#endif
    }

    std::string playerKeyLayout()
    {
        {
            std::lock_guard<std::mutex> lock(g_playerLayoutMutex);
            if (!g_playerLayoutCopy.empty())
                return g_playerLayoutCopy;
        }
        return ps2_host_input::keyBindings();
    }

    bool isOpen()
    {
#if defined(PS2X_ENABLE_DEBUG_UI)
        return g_open;
#else
        return false;
#endif
    }

    void draw(PS2Runtime &runtime)
    {
#if defined(PS2X_ENABLE_DEBUG_UI)
        // F1's presses are counted at every input poll (IsKeyPressed alone sees only the last one
        // before this frame, and misses a quick tap); Ctrl+F1 is the runtime's debugger.
        static const bool watching = (ps2_host_input::watchKey(KEY_F1), true);
        (void)watching;
        const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
        if ((ps2_host_input::takeKeyPresses(KEY_F1) % 2) == 1 && !ctrl)
            g_open = !g_open;
        // (TODO 208) The PlayStation button of a DualSense / DualShock 4 opens and closes it too (the pad read natively,
        // ps2_sony_pad.h; the game never gets that button), and so do View + Menu held together on another gamepad
        // (an Xbox pad: Windows keeps its guide button; the game gets neither Select nor Start meanwhile, ps2_pad.cpp).
        // Only while the game's window has the focus.
        {
            static bool shortcutWas = false;
            ps2x::sonypad::InputState pad;
            const bool sony = ps2x::sonypad::readHostPad(pad);
            bool shortcut = sony && (pad.buttons & ps2x::sonypad::kPs) != 0u;
            if (!sony && ps2x::sonypad::glfwGamepadsAllowed())
                for (int i = 0; i < 4 && !shortcut; ++i)
                    shortcut = IsGamepadAvailable(i) && IsGamepadButtonDown(i, GAMEPAD_BUTTON_MIDDLE_LEFT) &&
                               IsGamepadButtonDown(i, GAMEPAD_BUTTON_MIDDLE_RIGHT);
            shortcut = shortcut && IsWindowFocused();
            if (shortcut && !shortcutWas && g_capturing < 0)
                g_open = !g_open;
            shortcutWas = shortcut;
        }
        // One menu: F5's panel no longer opens (its settings are this menu's pages).
        static const bool noPanel = (ps2_settings::setPanelKeyEnabled(false), true);
        (void)noPanel;
        if (g_open && IsKeyPressed(KEY_ESCAPE) && g_capturing < 0 && ImGui::GetCurrentContext() &&
            !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
            g_open = false;
        // (TODO 212) The controller in the menu: the tabs' presses now (Circle at the tabs closes it), the page's in
        // drawMenu. Closed: at the tabs again next time.
        if (g_open)
            navFrame();
        if (!g_open)
            g_navLevel = 0;
        navLog();
        if (g_holdBlock && !g_open)
        {
            // (closed with a pad button: until every button is let go, so the game does not take that press)
            static int64_t since = 0;
            if (since == 0)
                since = navNowMs();
            if (!readMenuPad().any || navNowMs() - since > 3000)
            {
                g_holdBlock = false;
                since = 0;
            }
        }
        // The game gets no keys, pad or mouse while the menu is open (the cursor is the menu's).
        static bool wasOpen = false;
        if ((g_open || g_holdBlock) != wasOpen)
        {
            ps2_host_input::setGameInputBlocked(g_open || g_holdBlock);
            wasOpen = g_open || g_holdBlock;
        }
        updates::frame(runtime); // (an update's steps, a resume's outcome: updates.h)
        state_manager::frame(runtime); // (a save or load asked for in the Debug page: its answer - state_manager.h)
        syncInputState(); // (also for the runtime's debugger, drawn after this with the same ImGui)
        takeOverlayInput(); // (TODO 170 / 174: every character and wheel notch since the last picture)
        typeTestFrame(runtime.eeScheduler().currentVSyncTick());
        loadControls(); // (controls.txt, once)
        updateCarKeys(); // (W / S drive the car)
        if (!g_open)
            photo_mode::windowFrame(); // (its keys, and its hint: photo_mode.h)
        const bool clean = photo_mode::active(); // (photo mode: no overlay text over the picture)
        small_crosshair::draw(runtime); // (the small crosshair, when chosen: small_crosshair.h)
        if (!clean)
        {
            // The distance meter (distance_meter.h): above the crosshair, at the screen's centre.
            // Not in the car (its view has no crosshair to read it by). Placed over the crosshair: the
            // game puts its model in the world (photo_mode records where), projected here with the
            // game's own camera; the screen's middle stands in while that is not known yet.
            float metres = 0.0f;
            if (!camera_mode::inVehicle() && distance_meter::distance(runtime, metres)) // (on a jet bike too, as in the prototype)
            {
                char text[32];
                std::snprintf(text, sizeof(text), metres < 100.0f ? "%.1f m" : "%.0f m", metres);
                const int size = std::max(16, GetScreenHeight() / 34);
                const int width = MeasureText(text, size);
                float cx = GetScreenWidth() * 0.5f, cy = GetScreenHeight() * 0.5f;
                float point[3];
                const bool placed = photo_mode::crosshair(point) &&
                                    photo_mode::project(point, static_cast<float>(GetScreenWidth()), static_cast<float>(GetScreenHeight()),
                                                        ps2_host_display::displayAspect(), cx, cy);
                // (above the crosshair: the game's ring reaches about a twelfth of the picture's height over
                // its middle, the small cross a sixtieth)
                const float above = small_crosshair::enabled() ? GetScreenHeight() / 60.0f + 4.0f : GetScreenHeight() / 12.0f;
                const int x = static_cast<int>(cx) - width / 2;
                const int y = static_cast<int>(cy - above) - size;
                DrawText(text, x + 2, y + 2, size, Color{0, 0, 0, 170});
                DrawText(text, x, y, size, Color{120, 220, 255, 235});
                static const bool mark = std::getenv("TF_METER_MARK") != nullptr; // (tests: where the projection lands)
                if (mark && placed)
                    DrawCircle(static_cast<int>(cx), static_cast<int>(cy), 4.0f, Color{255, 40, 40, 255});
            }
        }
        if (speedrun_timer::enabled() && !clean)
        {
            // The speedrun timer (speedrun_timer.h), top right.
            const speedrun_timer::View v = speedrun_timer::view();
            const int size = std::max(18, GetScreenHeight() / 28);
            const std::string level = speedrun_timer::format(v.level);
            const std::string total = "total " + speedrun_timer::format(v.total);
            const int x = GetScreenWidth() - std::max(MeasureText(level.c_str(), size), MeasureText(total.c_str(), size * 2 / 3)) - size;
            DrawText(level.c_str(), x + 2, size / 2 + 2, size, Color{0, 0, 0, 170});
            DrawText(level.c_str(), x, size / 2, size, v.inLevel ? Color{255, 255, 255, 235} : Color{170, 170, 170, 200});
            DrawText(total.c_str(), x, size / 2 + size + 4, size * 2 / 3, Color{200, 230, 255, 220});
        }
        if (!clean && !g_open)
        {
            // Updates (TODO 104): a message after a start (back where you were, an update that failed)
            // for 15 seconds, and a newer version's notice while no level runs (the menus, the HQ, the
            // pause screen) - small, at the bottom left.
            const int size = std::max(14, GetScreenHeight() / 48);
            int y = GetScreenHeight() - size * 2;
            if (const std::string message = ext::notification(); !message.empty()) // (an extension's: port_ext.h)
            {
                const int width = MeasureText(message.c_str(), size);
                DrawRectangle(size / 2, y - size / 3, width + size, size * 5 / 3, Color{32, 16, 45, 230});
                DrawRectangle(size / 2, y - size / 3, 3, size * 5 / 3, Color{95, 39, 144, 255});
                DrawText(message.c_str(), size, y, size, Color{235, 220, 250, 255});
                y -= size * 2;
            }
            static std::string shownMessage;
            static double messageSince = -1.0;
            if (const std::string message = updates::startMessage(); message != shownMessage)
            {
                shownMessage = message;
                messageSince = GetTime();
            }
            if (!shownMessage.empty() && GetTime() - messageSince < 15.0)
            {
                DrawText(shownMessage.c_str(), size + 2, y + 2, size, Color{0, 0, 0, 170});
                DrawText(shownMessage.c_str(), size, y, size, Color{255, 200, 90, 235});
                y -= size + size / 2;
            }
            if (const std::string notice = updates::noticeText(); !notice.empty() && g_inMenu.load(std::memory_order_relaxed) != 0)
            {
                DrawText(notice.c_str(), size + 2, y + 2, size, Color{0, 0, 0, 170});
                DrawText(notice.c_str(), size, y, size, Color{120, 220, 255, 220});
            }
            // (TODO 267) The HD textures (or the sharp text) being made in the background: how far, small, in the bottom right
            // corner; once done, that a restart uses them (F1 has the button). Nothing shows otherwise.
            if (const std::string hd = hd_progress::cornerLine(); !hd.empty())
            {
                const int width = MeasureText(hd.c_str(), size);
                const int x = GetScreenWidth() - width - size, hy = GetScreenHeight() - size * 2;
                const bool ready = hd_progress::readyToRestart() && hd.find("eady") != std::string::npos;
                DrawText(hd.c_str(), x + 2, hy + 2, size, Color{0, 0, 0, 170});
                DrawText(hd.c_str(), x, hy, size, ready ? Color{255, 200, 90, 235} : Color{120, 220, 255, 200});
            }
        }
        // TF_HD_SHOT=<vblank>,<vblank>... (tests): the window's picture with the menu closed at those VBlanks, to
        // runs/hd_shot_<vblank>.png (the corner line, on a machine nobody watches).
        {
            static std::vector<unsigned long long> hdShots = []
            {
                std::vector<unsigned long long> list;
                for (const char *s = std::getenv("TF_HD_SHOT"); s && *s;)
                {
                    char *end = nullptr;
                    list.push_back(std::strtoull(s, &end, 10));
                    s = end && *end == ',' ? end + 1 : nullptr;
                }
                return list;
            }();
            if (!hdShots.empty() && !g_open && runtime.eeScheduler().currentVSyncTick() >= hdShots.front())
            {
                const std::string file = "runs/hd_shot_" + std::to_string(hdShots.front()) + ".png";
                hdShots.erase(hdShots.begin());
                rlDrawRenderBatchActive(); // (what is drawn so far, out of raylib's batch)
                Image shot = LoadImageFromScreen();
                ExportImage(shot, file.c_str());
                UnloadImage(shot);
                std::fprintf(stderr, "[mod menu] TF_HD_SHOT: %s (%dx%d), corner '%s'\n", file.c_str(), shot.width, shot.height,
                             hd_progress::cornerLine().c_str());
            }
        }
        // TF_HD_BUTTON_AT=<vblank>,<vblank>... (tests): F1 > Graphics' Make / Stop / Resume HD textures button pressed at
        // those VBlanks (TODO 267).
        {
            static std::vector<unsigned long long> presses = []
            {
                std::vector<unsigned long long> list;
                for (const char *s = std::getenv("TF_HD_BUTTON_AT"); s && *s;)
                {
                    char *end = nullptr;
                    list.push_back(std::strtoull(s, &end, 10));
                    s = end && *end == ',' ? end + 1 : nullptr;
                }
                return list;
            }();
            if (!presses.empty() && runtime.eeScheduler().currentVSyncTick() >= presses.front())
            {
                presses.erase(presses.begin());
                const std::string label = hd_progress::makeLabel();
                const std::string note = hd_progress::makeHdTextures();
                std::fprintf(stderr, "[mod menu] TF_HD_BUTTON_AT: '%s' -> %s\n", label.c_str(), note.c_str());
            }
        }
        // TF_MOD_MENU_AT=<vblank> (tests): the menu opened at that VBlank (with TF_MENU_SHOT: its picture).
        if (static const unsigned long long openAt = std::getenv("TF_MOD_MENU_AT") ? std::strtoull(std::getenv("TF_MOD_MENU_AT"), nullptr, 10) : 0ull;
            openAt != 0ull)
        {
            static bool opened = false;
            if (!opened && runtime.eeScheduler().currentVSyncTick() >= openAt)
            {
                opened = true;
                g_open = true;
            }
        }
        // TF_QUIT_BUTTON_AT=<vblank> (tests, TODO 277): the Quit game page's "Yes, quit" at that VBlank.
        if (static const unsigned long long quitAt = std::getenv("TF_QUIT_BUTTON_AT") ? std::strtoull(std::getenv("TF_QUIT_BUTTON_AT"), nullptr, 10) : 0ull;
            quitAt != 0ull)
        {
            static bool done = false;
            if (!done && runtime.eeScheduler().currentVSyncTick() >= quitAt)
            {
                done = true;
                quitNow(runtime);
            }
        }
        if (!g_open)
            return;
        menu_style::prepare(); // (fonts: before ImGui's frame)

        dropRaylibCharacters(); // (TODO 170: the characters come from the window's messages - feedOverlayInput)
        rlImGuiBegin();
        feedOverlayInput();
        {
            // The menu's own look, put back after it for the runtime's debugger (the same ImGui).
            const ImGuiStyle saved = ImGui::GetStyle();
            menu_style::applyStyle();
            menu_style::clearHelp();
            // TF_MOD_MENU_TAB=<page> (tests): that page first.
            static bool startPage = false;
            if (!startPage)
            {
                startPage = true;
                if (const char *tab = std::getenv("TF_MOD_MENU_TAB"))
                    for (int i = 0; i < static_cast<int>(std::size(kPages)); ++i)
                        if (std::strcmp(tab, kPages[i].name) == 0)
                            g_page = i;
            }
            drawMenu(runtime);
            navLog();
            ImGui::GetStyle() = saved;
        }
        rlImGuiEnd();
        // TF_MENU_SHOT=<vblank> (tests): the window's picture with the menu over it, once the menu has been open from
        // that VBlank for 30 frames, to runs/menu_shot.png (what the overlay looks like, on a machine nobody watches).
        // TF_MENU_SHOT_PAGES=all or <page>,<page>... with it: one picture of each shown page in turn, 30 frames apart, to
        // runs/menu_shot_<page>.png (TF_MENU_ADVANCED=1 opens the fold-outs).
        if (static const unsigned long long shotAt = std::getenv("TF_MENU_SHOT") ? std::strtoull(std::getenv("TF_MENU_SHOT"), nullptr, 10) : 0ull;
            shotAt != 0ull)
        {
            static std::vector<int> pages = []
            {
                std::vector<int> list;
                const char *env = std::getenv("TF_MENU_SHOT_PAGES");
                const std::string want = env ? env : "";
                for (int i = 0; i < static_cast<int>(std::size(kPages)); ++i)
                    if (!want.empty() && pageShown(kPages[i].page) &&
                        (want == "all" || ("," + want + ",").find("," + std::string(kPages[i].name) + ",") != std::string::npos))
                        list.push_back(i);
                return list;
            }();
            static size_t next = 0;
            static bool done = false;
            static int openFrames = 0;
            if (!done && next < pages.size() && openFrames == 0)
                g_page = pages[next];
            openFrames = !done && g_open && runtime.eeScheduler().currentVSyncTick() >= shotAt ? openFrames + 1 : 0;
            if (openFrames == 30)
            {
                std::string file = "runs/menu_shot.png";
                if (next < pages.size())
                {
                    std::string name;
                    for (const char *c = kPages[pages[next]].name; *c; ++c)
                        if (std::isalnum(static_cast<unsigned char>(*c)))
                            name.push_back(*c);
                    file = "runs/menu_shot_" + name + ".png";
                }
                Image shot = LoadImageFromScreen();
                ExportImage(shot, file.c_str());
                UnloadImage(shot);
                std::fprintf(stderr, "[mod menu] TF_MENU_SHOT: %s (%dx%d)\n", file.c_str(), shot.width, shot.height);
                openFrames = 0;
                done = pages.empty() || ++next >= pages.size();
            }
        }
#endif
    }

    void frame(uint8_t *rdram)
    {
        {
            // (the menu keys: no level running - the start menu, the HQ - or the level paused)
            const auto read = [rdram](uint32_t address)
            {
                uint32_t v = 0u;
                if (const uint8_t *p = getConstMemPtr(rdram, address))
                    std::memcpy(&v, p, sizeof(v));
                return v;
            };
            constexpr uint32_t kGame = 0x006CF2B8u, kSoundSystem = 0x006CF2A8u, kSoundPauseState = 0x28u, kPaused = 2u;
            const uint32_t sound = read(kSoundSystem);
            const bool paused = sound != 0u && read(sound + kSoundPauseState) == kPaused;
            const uint64_t frames = g_gameFrames.fetch_add(1u) + 1u;
            const bool top = paused && frames - g_pauseInputFrame.load() <= 2u;
            g_inMenu.store(read(kGame) == 0u ? 1 : (paused ? (top ? 3 : 2) : 0), std::memory_order_relaxed);
            menu_mouse::frame(rdram, paused && read(kGame) != 0u); // (the pause menu: menu_mouse.h)
            pad_speaker::setLevelPaused(paused && read(kGame) != 0u); // (TODO 257: Show Objective's message)
#if defined(PS2X_ENABLE_DEBUG_UI)
            if (static bool listed = std::getenv("TF_MOD_MENU_LIST") == nullptr; !listed && frames >= 120u)
            {
                listed = true;
                listTabs();
            }
#endif
        }
        startText(rdram, g_inMenu.load(std::memory_order_relaxed));
    }

    uint16_t padButtons(uint64_t tick)
    {
        const uint16_t mouse = menu_mouse::padButtons(); // (the pause menu's clicks: menu_mouse.h)
        // TF_MOD_CODE=<tick>:<n>: enters code n (0-31, the list's order) at that tick, as its Enter
        // button would (for unattended test runs).
        static uint64_t testTick = 0u;
        static size_t testCode = 0u;
        static bool testParsed = false;
        if (!testParsed)
        {
            testParsed = true;
            if (const char *env = std::getenv("TF_MOD_CODE"))
                if (std::sscanf(env, "%llu:%zu", reinterpret_cast<unsigned long long *>(&testTick), &testCode) != 2 ||
                    testCode >= cheats().size())
                    testTick = 0u;
        }
        if (testTick != 0u && tick >= testTick)
        {
            testTick = 0u;
            enterCode(cheats()[testCode]);
        }

        std::lock_guard<std::mutex> guard(g_codeLock);
        if (!g_codeActive)
            return mouse;
        if (g_codeStart == 0u)
            g_codeStart = tick + kLead;
        if (tick < g_codeStart)
            return mouse;
        const uint64_t step = (tick - g_codeStart) / (kHold + kGap);
        const uint64_t phase = (tick - g_codeStart) % (kHold + kGap);
        if (step >= g_code.size())
        {
            g_codeActive = false;
            return mouse;
        }
        if (phase == 0u)
            std::fprintf(stderr, "[mod menu] code \"%s\": button %llu of %zu (tick %llu)\n", g_codeName.c_str(),
                         static_cast<unsigned long long>(step + 1u), g_code.size(), static_cast<unsigned long long>(tick));
        return static_cast<uint16_t>((phase < kHold ? g_code[step] : 0u) | mouse);
    }
}

namespace mod_menu
{
    namespace
    {
        void pauseInputFn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_pauseInputFrame.store(g_gameFrames.load()); // (before the call: it may stop mid-way)
            g_pauseInput(rdram, ctx, runtime);
        }
    }

    void install(PS2Runtime &runtime)
    {
        g_pauseInput = runtime.lookupFunction(kPauseInput);
        if (g_pauseInput)
            runtime.registerFunction(kPauseInput, pauseInputFn);
        else
            std::fprintf(stderr, "[mod menu] no function at %08x: Escape is back on the pause screen's top too\n", kPauseInput);
    }
}
