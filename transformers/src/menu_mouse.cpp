// The mouse in the game's own menus (menu_mouse.h; TODO 70).
//
// The menus the pause opens are taScreenInGMenu screens (the game's class names, from its RTTI):
// the pause menu itself (taScreenInGPause), options, controller, audio, the quit confirmation. Each
// frame the visible one lays out its bars (SetBarPositions, 0x312820 - hooked here: its `this` is the
// screen shown). Its fields: +0x7B0 the number of entries, +0x814 the selected one, +0x828[7] the
// entries the selection skips; the rows are 30 high at y = (400 - [+0x7AC]) - (i + 1) x [+0x840] in
// the game's 2D space.
//
// The same lists outside a level (TODO 175): the front end's start menu and memory card prompts
// (taScreenStartMenu, taScreenStartInitCardCheck - both taScreenInGMenu screens, their input is its
// 0x312480) and the HQ's option screens, and in a level the Mini-Con / Data-Con pickup menus. The HQ draws
// its lists on slanted 3D hologram panels (Options..., the Exit's "Are you sure?"): there the wheel steps and a
// click takes the selected entry, no pointing (frame()).
//
// The HQ's own screens (taScreenPrG*: the main ring, character / level / Mini-Con select, the warp
// gate, the comm screen, the title) are not lists: their input (vtable slot 18; 0x3F7660 the ring's...)
// takes the UI's events 1 up, 2 down, 3 left, 4 right, 5 confirm, 6 back - the rings step left and
// right. Each frame the shown one runs its update (slot 16 - hooked here, with the base update
// 0x1A0C20 they call: its `this` is the screen). A click there: in the left or right third of the 2D
// steps the ring, in the middle confirms (cross); the right button goes back (triangle), the wheel
// steps (up and down on the Mini-Con button screen, the only one that takes them).
//
// Testing aids: TF_MENU_TRACE=1 logs the shown screen's fields; TF_MENU_MOUSE_TEST=<frames>:<u>,<v>[:L|R|U|D]
// [/...] puts a cursor at (u, v) of the 2D's 4:3 area (0..1 each, y down) that many game frames after
// the first menu came up, with a left or right click or a wheel step (U, D) - the worker PCs' hidden window
// has no mouse.

#include "menu_mouse.h"

#include "playrec.h"
#include "ultrawide.h"

#include "decomp.h"
#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_settings.h"
#include "runtime/ps2_stick_response.h"

#include "port_ext.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "raylib.h"
#include "runtime/ps2_sony_pad.h"
#include "game_version.h" // (TF_GA: the code addresses that differ in the PAL release)

// profiler.cpp (Windows): the window's own clicks and wheel since the last call, and the cursor hidden for a controller
// (TODO 175).
void tfTakeMenuMouse(int &left, int &right, float &wheel);
void tfHideCursorForPad();

namespace menu_mouse
{
    namespace
    {
        // --- The window thread's side ---------------------------------------------------------
        // The cursor in the game's 2D area - a 4:3 rectangle, the picture's height, in the middle of
        // the picture (the menus' 2D is drawn there at every picture shape: measured in a 2.4:1 shot,
        // the pause menu's bars start at 0.31 of its width = x 80 of the 2D's 512) - 0..1 each, y down.
        std::atomic<float> g_u{-1.0f}, g_v{-1.0f};
        std::atomic<float> g_shape{16.0f / 9.0f}; // the picture's shape (the game thread's: 4:3 without widescreen)
        std::atomic<int> g_leftClicks{0}, g_rightClicks{0};
        std::atomic<float> g_wheel{0.0f};
        std::atomic<bool> g_wantCursor{false}; // (the game thread's answer: a menu is up)
        std::atomic<bool> g_enabled{true};     // TF_MENU_MOUSE
        std::atomic<bool> g_gameMouse{true};   // TF_MOUSE (TODO 290 (6)): off = no mouse input in the game at all

        // --- The game thread's side -----------------------------------------------------------
        constexpr uint32_t kSetBarPositions = 0x00312820u;
        constexpr uint32_t kCount = 0x7B0u, kSelected = 0x814u, kSkipped = 0x828u;
        constexpr uint32_t kRowTop = 0x7ACu, kRowStep = 0x840u;
        constexpr uint32_t kSize2D = 0x768u;   // the 2D's width and height (the render device's offset: 512, 448)
        constexpr uint32_t kRowQuads = 0x100u; // per entry its bar's quad (4 x {x, y, colour, u, v})
        constexpr uint32_t kWidescreenByte = 0x006CF004u;
        constexpr uint16_t kPadStart = 1u << 3, kPadUp = 1u << 4, kPadRight = 1u << 5, kPadDown = 1u << 6, kPadLeft = 1u << 7;
        constexpr uint16_t kPadTriangle = 1u << 12, kPadCross = 1u << 14;
        constexpr int kPressFrames = 3;
        constexpr uint32_t kGame = 0x006CF2B8u; // the taGame (0: no level - the front end)
        constexpr uint32_t kPreGame = 0x006CF688u, kPreGameVtable = 0x006B7360u; // the HQ's (and front end's) taUIPreGame
        constexpr uint32_t kMiniconPickupVtable = 0x006B6EE0u, kDataconPickupVtable = 0x006B6E60u;
        // The front end's lists: taScreenStartLoadGame (0x6C7240) ... taScreenStartMenu (0x6C7640), 0x80 apart.
        constexpr uint32_t kStartScreensFirst = 0x006C7240u, kStartScreensLast = 0x006C7640u;
        constexpr float kStartLastRow = 362.0f, kStartRowStep = 25.0f;

        // The HQ's own screens (the header): their update (vtable slot 16), the vtable, how a click steps them.
        enum class Kind : uint8_t
        {
            Ring,    // left / right, confirm, back
            Grid,    // also up / down (the Mini-Con button screen)
            Confirm, // confirm and back only (the comm screen)
            Title,   // "press start"
        };
        struct OwnScreen
        {
            uint32_t update, vtable;
            const char *name;
            Kind kind;
            PS2Runtime::RecompiledFunction original;
        };
        constexpr uint32_t kBaseUpdate = 0x001A0C20u; // (the ring's own slot 16; the others call it)
        OwnScreen g_own[] = {
            {kBaseUpdate, 0x006BD8F0u, "taScreenPrGRing", Kind::Ring, nullptr},
            {TF_GA(0x003F6E90u), 0x006BD880u, "taScreenPrGRingCharacterSelect", Kind::Ring, nullptr},
            {TF_GA(0x004F1720u), 0x006C4410u, "taScreenPrGRingLevelSelect", Kind::Ring, nullptr},
            {TF_GA(0x004EEA00u), 0x006C4360u, "taScreenPrGWarpGate", Kind::Ring, nullptr},
            {TF_GA(0x004E3C70u), 0x006C3E40u, "taScreenPrGRingMiniconSelect", Kind::Ring, nullptr},
            {TF_GA(0x004E5D10u), 0x006C3ED0u, "taScreenPrGRingMiniconButtonSelect", Kind::Grid, nullptr},
            {TF_GA(0x003F4A50u), 0x006BD810u, "taScreenPrGHQComm", Kind::Confirm, nullptr},
            {0x0033B690u, 0x006B77C0u, "taScreenPrGTitle", Kind::Title, nullptr},
        };
        constexpr uint64_t kOwnStale = 6u; // game frames an update may be missed (it can yield inside 0x1A0C20)
        uint32_t g_ownScreen = 0u;         // the HQ screen whose update ran last
        int g_ownIndex = -1;               // its g_own entry
        uint64_t g_ownFrame = 0u;          // the game frame that was
        float g_lastU = -2.0f, g_lastV = -2.0f;
        uint32_t g_menu = 0u;       // the screen whose bars were laid out last
        uint64_t g_menuFrame = 0u;  // the game frame that was
        uint64_t g_frames = 0u;     // game frames counted here
        uint64_t g_shownSince = 0u; // the frame the current menu came up
        uint16_t g_buttons = 0u;    // pressed this frame
        int g_holdFrames = 0;       // how long g_heldButtons stay pressed
        uint16_t g_heldButtons = 0u;

        uint32_t word(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v = 0u;
            if (const uint8_t *p = getConstMemPtr(rdram, a))
                std::memcpy(&v, p, sizeof(v));
            return v;
        }
        float real(const uint8_t *rdram, uint32_t a)
        {
            const uint32_t w = word(rdram, a);
            float f;
            std::memcpy(&f, &w, sizeof(f));
            return f;
        }
        bool inRam(uint32_t a) { return a >= 0x00100000u && a < PS2_RAM_SIZE; }

        void setBarPositions(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            static PS2Runtime::RecompiledFunction original = decomp::original(kSetBarPositions);
            g_menu = getRegU32(ctx, 4);
            g_menuFrame = g_frames;
            original(rdram, ctx, runtime);
        }

        const decomp::Registration g_registration{decomp::Function{
            "menu_mouse::setBarPositions", "menu_mouse", kSetBarPositions, &setBarPositions, nullptr, 0u, false,
            decomp::Result::None}};

        // An HQ screen's update (and the base update they call): notes the screen when it is one of g_own's.
        void ownUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            PS2Runtime::RecompiledFunction original = nullptr;
            for (const OwnScreen &s : g_own)
                if (s.update == ctx->pc)
                {
                    original = s.original;
                    break;
                }
            const uint32_t self = getRegU32(ctx, 4);
            if (inRam(self))
            {
                const uint32_t vtable = word(rdram, self);
                for (int i = 0; i < static_cast<int>(std::size(g_own)); ++i)
                    if (g_own[i].vtable == vtable)
                    {
                        g_ownScreen = self;
                        g_ownIndex = i;
                        g_ownFrame = g_frames;
                        break;
                    }
                // (TF_MENU_TRACE: each other screen class the base update sees, once)
                static const bool traceOn = std::getenv("TF_MENU_TRACE") != nullptr;
                static uint32_t seen[32] = {};
                if (traceOn && ctx->pc == kBaseUpdate && std::find(std::begin(seen), std::end(seen), vtable) == std::end(seen))
                    for (uint32_t &slot : seen)
                        if (slot == 0u)
                        {
                            slot = vtable;
                            const uint32_t name = word(rdram, word(rdram, vtable));
                            std::string text;
                            for (uint32_t k = 0; inRam(name) && k < 64u; ++k)
                            {
                                const uint8_t *c = getConstMemPtr(rdram, name + k);
                                if (!c || !*c)
                                    break;
                                text.push_back(static_cast<char>(*c));
                            }
                            std::fprintf(stderr, "[menu mouse] the base update runs a %s (%08x, vtable %08x)\n", text.c_str(), self, vtable);
                            break;
                        }
            }
            if (original)
                original(rdram, ctx, runtime);
            else
                runtime->requestStop();
        }

        void trace(const uint8_t *rdram, uint32_t menu, uint32_t size = 0x850u)
        {
            std::string line;
            char buf[16];
            for (uint32_t o = 0; o < size; o += 4u)
            {
                std::snprintf(buf, sizeof(buf), "%08x ", word(rdram, menu + o));
                line += buf;
            }
            std::fprintf(stderr, "[menu mouse] frame %llu screen %08x (vtable %08x): %s\n", static_cast<unsigned long long>(g_frames), menu,
                         word(rdram, menu), line.c_str());
        }

        struct TestPoint
        {
            uint64_t frames;
            float x, y;
            char click; // 0, 'L', 'R'
        };
        std::vector<TestPoint> testPoints()
        {
            std::vector<TestPoint> v;
            const char *env = std::getenv("TF_MENU_MOUSE_TEST");
            for (const char *p = env; p && *p;)
            {
                TestPoint t{};
                char c = 0;
                unsigned long long f = 0;
                int n = 0;
                if (std::sscanf(p, "%llu:%f,%f%n", &f, &t.x, &t.y, &n) < 3)
                    break;
                p += n;
                if (*p == ':' && (p[1] == 'L' || p[1] == 'R' || p[1] == 'U' || p[1] == 'D'))
                {
                    c = p[1];
                    p += 2;
                }
                t.frames = f;
                t.click = c;
                v.push_back(t);
                while (*p && *p != ';' && *p != '/')
                    ++p;
                if (*p == ';' || *p == '/')
                    ++p;
            }
            return v;
        }

        void press(uint16_t buttons, int frames)
        {
            g_heldButtons = buttons;
            g_holdFrames = frames;
        }
    }

    void addSettings()
    {
        // (TODO 290 (6), bbatts: a controller player wants the mouse out of the game) Off: the window never takes the mouse
        // for the camera (main.cpp: no capture, so no turning and no mouse buttons in play), and the game's menus get no
        // pointing or clicks. The F1 overlay keeps the mouse either way.
        if (const char *env = std::getenv("TF_MOUSE"); env && *env)
            g_gameMouse.store(*env != '0');
        {
            ps2_settings::Setting m;
            m.key = "TF_MOUSE";
            m.section = "Mouse";
            m.label = "Mouse in the game";
            m.order = 30;
            m.help = "Off: the game takes no mouse input at all - no camera, no buttons, no pointing in its menus - for playing "
                     "with a controller without the mouse getting in the way. The F1 menu still works with the mouse.";
            m.choices = {{"1", "On"}, {"0", "Off"}};
            m.defaultValue = "1";
            m.apply = [](const std::string &value)
            {
                g_gameMouse.store(value != "0");
                return true;
            };
            ps2_settings::add(m);
        }
        if (const char *env = std::getenv("TF_MENU_MOUSE"); env && *env)
            g_enabled.store(*env != '0');
        ps2_settings::Setting s;
        s.key = "TF_MENU_MOUSE";
        s.section = "Mouse";
        s.label = "Mouse in the menus";
        s.order = 40;
        s.help = "In the game's menus (paused, the front end, the HQ) the cursor shows. In a list: point at an entry to select it, "
                 "left click to confirm, right click to go back, the wheel to step. In the HQ's rings: click the left or right "
                 "side to turn the ring, the middle to choose, right click to go back, the wheel turns it. Click into the "
                 "picture after resuming to give the mouse back to the camera.";
        s.choices = {{"1", "On"}, {"0", "Off"}};
        s.defaultValue = "1";
        s.apply = [](const std::string &value)
        {
            g_enabled.store(value != "0");
            return true;
        };
        ps2_settings::add(s);
    }

    void install(PS2Runtime &runtime)
    {
        for (OwnScreen &s : g_own)
        {
            if (!runtime.hasFunction(s.update) || (s.original = runtime.lookupFunction(s.update)) == nullptr)
            {
                std::fprintf(stderr, "[menu mouse] no update at %08x (%s): its clicks are off\n", s.update, s.name);
                continue;
            }
            runtime.registerFunction(s.update, ownUpdate);
        }
    }

    // (TODO 175) A controller in use: a button pressed or a stick / trigger pushed this picture that was not the picture
    // before (a pad merely held at rest is not "used"). The DualShock / DualSense the runtime reads itself (ps2_sony_pad.h)
    // or a GLFW gamepad.
    bool padUsedNow()
    {
        static bool before = false;
        bool active = false;
        ps2x::sonypad::InputState s;
        if (ps2x::sonypad::readHostPad(s))
        {
            const auto off = [](uint8_t v) { return v < 0x80u - 48u || v > 0x80u + 48u; };
            active = (s.buttons & 0x3FFFu) != 0u || off(s.lx) || off(s.ly) || off(s.rx) || off(s.ry) || s.l2 > 48u || s.r2 > 48u;
        }
        for (int i = 0; i < 4 && !active; ++i)
            if (IsGamepadAvailable(i))
            {
                for (int b = GAMEPAD_BUTTON_LEFT_FACE_UP; b <= GAMEPAD_BUTTON_RIGHT_THUMB && !active; ++b)
                    active = IsGamepadButtonDown(i, b);
                for (int a = GAMEPAD_AXIS_LEFT_X; a <= GAMEPAD_AXIS_RIGHT_Y && !active; ++a)
                    active = std::fabs(GetGamepadAxisMovement(i, a)) > 0.4f;
            }
        const bool started = active && !before;
        before = active;
        return started;
    }

    void windowFrame(int windowWidth, int windowHeight, bool overlayOpen)
    {
        if (windowWidth <= 0 || windowHeight <= 0)
            return;
        // (TODO 175) The cursor hides while a controller is used - the game window, the F1 overlay, every menu - and
        // comes back when the mouse moves (profiler.cpp's window procedure).
        if (padUsedNow())
            tfHideCursorForPad();
        // (TODO 286 a) The F1 overlay (or the debugger) up: the window's clicks and wheel are the overlay's, never the game
        // menu's under it - they were also handed to the pause menu, the HQ's screens and the front end, which pressed their
        // buttons through the overlay. Taken and dropped while it is up, and for a quarter of a second after it closes: the
        // click on an overlay button that closes it arrives in the frame it closed.
        static auto overlaySeen = std::chrono::steady_clock::time_point{};
        const auto nowOverlay = std::chrono::steady_clock::now();
        if (overlayOpen)
            overlaySeen = nowOverlay;
        if (overlayOpen || nowOverlay - overlaySeen < std::chrono::milliseconds(250))
        {
            int dropLeft = 0, dropRight = 0;
            float dropWheel = 0.0f;
            tfTakeMenuMouse(dropLeft, dropRight, dropWheel);
            g_u.store(-1.0f, std::memory_order_relaxed);
            g_v.store(-1.0f, std::memory_order_relaxed);
            static const bool log = std::getenv("TF_OVERLAY_INPUT_LOG") && *std::getenv("TF_OVERLAY_INPUT_LOG") == '1';
            if (log && (dropLeft > 0 || dropRight > 0 || dropWheel != 0.0f))
                std::fprintf(stderr, "[menu mouse] the overlay %s: %d left / %d right clicks, wheel %.2f kept from the game\n",
                             overlayOpen ? "is up" : "just closed", dropLeft, dropRight, dropWheel);
            return;
        }
        // (TODO 175) The clicks and wheel of the window's messages: raylib's pressed / wheel are its last poll's only, and
        // the runtime polls several times a picture - a click between them was lost.
        int clicksLeft = 0, clicksRight = 0;
        float wheelMoved = 0.0f;
        tfTakeMenuMouse(clicksLeft, clicksRight, wheelMoved);
        // The picture, fitted to the window at its shape and centred; the 2D's 4:3 in its middle.
        const float w = static_cast<float>(windowWidth), h = static_cast<float>(windowHeight);
        const float shape = g_shape.load(std::memory_order_relaxed);
        const float pictureH = std::min(h, w / shape);
        const float areaW = pictureH * 4.0f / 3.0f;
        const float ax = (w - areaW) * 0.5f, ay = (h - pictureH) * 0.5f;
        const Vector2 m = GetMousePosition();
        const bool on = IsCursorOnScreen() && IsWindowFocused();
        g_u.store(on ? (m.x - ax) / areaW : -1.0f, std::memory_order_relaxed);
        g_v.store(on ? (m.y - ay) / pictureH : -1.0f, std::memory_order_relaxed);
        if (!g_wantCursor.load(std::memory_order_relaxed) || !on)
            return;
        if (clicksLeft > 0)
            g_leftClicks.fetch_add(clicksLeft, std::memory_order_relaxed);
        if (clicksRight > 0)
            g_rightClicks.fetch_add(clicksRight, std::memory_order_relaxed);
        if (wheelMoved != 0.0f)
            g_wheel.fetch_add(wheelMoved, std::memory_order_relaxed);
    }

    bool wantsCursor() { return g_wantCursor.load(std::memory_order_relaxed); }

    bool gameMouseOn() { return g_gameMouse.load(std::memory_order_relaxed); }

    uint16_t padButtons()
    {
        return g_buttons;
    }

    void frame(uint8_t *rdram, bool paused)
    {
        playrec::menuFrame(rdram); // (N12 b: a replay's recorded selections - playrec.h)
        ++g_frames;
        g_buttons = 0u;
        if (g_holdFrames > 0)
        {
            --g_holdFrames;
            g_buttons = g_heldButtons;
        }
        const uint32_t menu = g_menu;
        const bool current = inRam(menu) && g_frames - g_menuFrame <= 2u;
        // (TODO 175) The lists: paused, outside a level (the front end, the HQ) or a pickup menu.
        const uint32_t menuVtable = current ? word(rdram, menu) : 0u;
        // (the HQ has a taGame too: its menus' executor taUIPreGame, [0x6CF688], up - 0 in a level)
        const uint32_t preGame = word(rdram, kPreGame);
        const bool inHq = inRam(preGame) && word(rdram, preGame) == kPreGameVtable;
        const bool listShown = current && (paused || inHq || word(rdram, kGame) == 0u || menuVtable == kMiniconPickupVtable ||
                                           menuVtable == kDataconPickupVtable ||
                                           ext::ownsMouseScreen(rdram, menu)); // (an extension's own list screen: port_ext.h)
        // Else the HQ's own screen whose update ran last - while it is still that screen.
        const OwnScreen *own = nullptr;
        if (!listShown && g_ownIndex >= 0 && inRam(g_ownScreen) && g_frames - g_ownFrame <= kOwnStale &&
            word(rdram, g_ownScreen) == g_own[g_ownIndex].vtable)
            own = &g_own[g_ownIndex];
        const bool shown = listShown || own != nullptr;
        // (TODO 274) While a menu's list is shown the sticks go to the game without the movement's lift: it turned a
        // stick resting a little off centre into a held direction and the list scrolled on its own. (From ravioli-gh's
        // Linux port, there for the Steam Deck's sticks.)
        ps2_stick_response::setMenuShown(shown);
        if (static const bool traceStick = std::getenv("TF_STICK_TRACE") != nullptr; traceStick) // (each change logged)
        {
            static int told = -1;
            if (told != (shown ? 1 : 0))
                std::fprintf(stderr, "[stick] %s (tick %llu)\n", (told = shown ? 1 : 0) ? "a menu: the sticks as the pad gives them" : "play: the response's lift on",
                             static_cast<unsigned long long>(g_frames));
        }
        if (static const bool traceList = std::getenv("TF_MENU_TRACE") != nullptr; traceList)
        {
            static uint32_t lastVtable = 1u;
            if (menuVtable != lastVtable)
                std::fprintf(stderr, "[menu mouse] the list screen: vtable %08x (in the HQ %d, a level %d, paused %d)\n", menuVtable, inHq ? 1 : 0,
                             word(rdram, kGame) != 0u ? 1 : 0, paused ? 1 : 0);
            lastVtable = menuVtable;
        }
        const bool active = shown && g_enabled.load(std::memory_order_relaxed) && g_gameMouse.load(std::memory_order_relaxed);
        // (TODO 175) A list the HQ shows on a hologram panel (its Options, Controller, Audio..., the "Are you sure?" of
        // its Exit - taScreenExit): the 2D drawn into the panel, the panel in 3D, slanted, wherever the camera has it -
        // the rows are not where the 2D has them on the screen. No pointing there: the wheel steps, a left click takes the
        // entry the selection is on, the right one goes back. (Not the front end's lists, drawn flat, nor the faction
        // screen, which finds its rows itself.)
        const bool frontEnd = menuVtable >= kStartScreensFirst && menuVtable <= kStartScreensLast &&
                              ((menuVtable - kStartScreensFirst) & 0x7Fu) == 0u;
        const bool panel = listShown && !paused && inHq && !frontEnd &&
                           !ext::ownsMouseScreen(rdram, menu);
        ultrawide::notePanel(inRam(menu) && g_frames - g_menuFrame <= 2u); // (the HQ's panels: TODO 53)
        if (!shown)
            g_shownSince = 0u;
        else if (g_shownSince == 0u)
            g_shownSince = g_frames;
        g_wantCursor.store(active, std::memory_order_relaxed);
        static const bool traceOn = std::getenv("TF_MENU_TRACE") != nullptr;
        if (traceOn && shown && (g_frames - g_shownSince) % 120u == 1u)
        {
            if (own)
            {
                std::fprintf(stderr, "[menu mouse] %s:\n", own->name);
                trace(rdram, g_ownScreen, 0x200u);
            }
            else
                trace(rdram, menu);
        }
        {
            const uint8_t *wide = getConstMemPtr(rdram, kWidescreenByte);
            g_shape.store(wide && *wide != 0u ? ultrawide::shape() : 4.0f / 3.0f, std::memory_order_relaxed);
        }
        if (!active)
        {
            g_leftClicks.store(0, std::memory_order_relaxed);
            g_rightClicks.store(0, std::memory_order_relaxed);
            g_wheel.store(0.0f, std::memory_order_relaxed);
            g_lastU = g_lastV = -2.0f;
            return;
        }
        float u = g_u.load(std::memory_order_relaxed), v = g_v.load(std::memory_order_relaxed);
        int left = g_leftClicks.exchange(0, std::memory_order_relaxed);
        int right = g_rightClicks.exchange(0, std::memory_order_relaxed);
        float wheel = g_wheel.exchange(0.0f, std::memory_order_relaxed);
        // (a test's cursor: put at its frame, kept there until the next one)
        static const std::vector<TestPoint> tests = testPoints();
        if (!tests.empty())
        {
            // (counted from the first menu shown: one test can go through several screens)
            static float testU = -1.0f, testV = -1.0f;
            static const uint64_t testBase = g_shownSince;
            for (const TestPoint &t : tests)
                if (g_frames - testBase == t.frames)
                {
                    testU = t.x;
                    testV = t.y;
                    left += t.click == 'L' ? 1 : 0;
                    right += t.click == 'R' ? 1 : 0;
                    wheel += t.click == 'U' ? 1.0f : (t.click == 'D' ? -1.0f : 0.0f);
                    const char *what = t.click == 'L'   ? " left click"
                                       : t.click == 'R' ? " right click"
                                       : t.click == 'U' ? " wheel up"
                                       : t.click == 'D' ? " wheel down"
                                                        : "";
                    std::fprintf(stderr, "[menu mouse] test cursor (%.3f, %.3f)%s, screen %s (vtable %08x)\n", t.x, t.y, what,
                                 own ? own->name : "list",
                                 word(rdram, own ? g_ownScreen : menu));
                }
            u = testU;
            v = testV;
        }
        if (own)
        {
            // An HQ screen: where the click is decides (the header).
            if (g_holdFrames == 0)
            {
                uint16_t button = 0u;
                const bool onLeft = u < 1.0f / 3.0f, onRight = u > 2.0f / 3.0f;
                if (left > 0)
                {
                    switch (own->kind)
                    {
                    case Kind::Title:
                        button = kPadStart;
                        break;
                    case Kind::Confirm:
                        button = kPadCross;
                        break;
                    case Kind::Grid:
                        button = onLeft ? kPadLeft : onRight ? kPadRight : v < 0.3f ? kPadUp : v > 0.7f ? kPadDown : kPadCross;
                        break;
                    case Kind::Ring:
                        button = onLeft ? kPadLeft : onRight ? kPadRight : kPadCross;
                        break;
                    }
                }
                else if (right > 0)
                    button = own->kind == Kind::Title ? 0u : kPadTriangle;
                else if (wheel != 0.0f && own->kind == Kind::Ring)
                    button = wheel > 0.0f ? kPadLeft : kPadRight;
                else if (wheel != 0.0f && own->kind == Kind::Grid)
                    button = wheel > 0.0f ? kPadUp : kPadDown;
                if (button != 0u)
                {
                    press(button, kPressFrames);
                    if (traceOn)
                        std::fprintf(stderr, "[menu mouse] %s: the mouse presses %04x\n", own->name, button);
                }
            }
            g_lastU = u;
            g_lastV = v;
            return;
        }
        if (panel)
        {
            if (g_holdFrames == 0)
            {
                const uint16_t button = left > 0 ? kPadCross : right > 0 ? kPadTriangle : wheel > 0.0f ? kPadUp : wheel < 0.0f ? kPadDown : 0u;
                if (button != 0u)
                {
                    press(button, kPressFrames);
                    if (traceOn)
                        std::fprintf(stderr, "[menu mouse] an HQ panel (vtable %08x): the mouse presses %04x\n", menuVtable, button);
                }
            }
            g_lastU = u;
            g_lastV = v;
            return;
        }
        // The entry under the cursor: the rows in the 2D ([+0x768] x [+0x76C], y up from the bottom),
        // from a bar's start (its quad's leftmost x) to the picture's right edge.
        const float width2D = real(rdram, menu + kSize2D), height2D = real(rdram, menu + kSize2D + 4u);
        const int count = static_cast<int>(std::min<uint32_t>(word(rdram, menu + kCount), 7u));
        int hovered = -1;
        if (u > -0.5f && width2D > 0.0f && height2D > 0.0f)
        {
            const float x = u * width2D, y = (1.0f - v) * height2D;
            const float top = real(rdram, menu + kRowTop), step = real(rdram, menu + kRowStep);
            // (TODO 175) The front end's lists (taScreenStart*) lay their bars out the same but draw no bars: their
            // entries are centred text, the last one's middle 362 of the 448 down, 25 apart (measured in shots of the
            // start menu and its options).
            if (frontEnd)
            {
                if (u >= 0.15f && u <= 0.85f)
                    for (int i = 0; i < count; ++i)
                    {
                        const float middle = (kStartLastRow - static_cast<float>(count - 1 - i) * kStartRowStep) / 448.0f * height2D;
                        if (std::fabs(v * height2D - middle) <= kStartRowStep * 0.5f)
                            hovered = i;
                    }
            }
            else
            for (int i = 0; i < count; ++i)
            {
                const float rowTop = (400.0f - top) - static_cast<float>(i + 1) * step;
                float start = 1e9f;
                for (uint32_t k = 0; k < 4u; ++k)
                    start = std::min(start, real(rdram, menu + kRowQuads + 0x50u * static_cast<uint32_t>(i) + 0x14u * k));
                if (!(start > -1000.0f && start < width2D))
                    start = 0.0f;
                float end = width2D;
                const bool factionBound = ext::rowBounds(rdram, menu, static_cast<unsigned>(i), start, end); // (an extension's rows)
                // The renderer extends the mirrored ribbon's left edge beyond the central 4:3 area.
                const float pictureLeft = (1.0f - g_shape.load(std::memory_order_relaxed) * 0.75f) * 0.5f;
                const bool insideLeft = factionBound && start == 0.0f ? u >= pictureLeft : x >= start - 8.0f;
                if (y <= rowTop && y >= rowTop - 30.0f && insideLeft &&
                    (!factionBound || (end >= start && x <= end + 8.0f)))
                    hovered = i;
            }
        }
        const uint8_t *skipped = getConstMemPtr(rdram, menu + kSkipped);
        const bool selectable = hovered >= 0 && !(skipped && skipped[hovered] != 0u);
        const bool moved = std::fabs(u - g_lastU) > 1e-4f || std::fabs(v - g_lastV) > 1e-4f;
        g_lastU = u;
        g_lastV = v;
        // (only when the cursor moves or clicks: the keys and the pad keep the selection otherwise)
        if (selectable && (moved || left > 0) && word(rdram, menu + kSelected) != static_cast<uint32_t>(hovered))
        {
            if (uint8_t *p = getMemPtr(rdram, menu + kSelected))
            {
                const uint32_t value = static_cast<uint32_t>(hovered);
                std::memcpy(p, &value, sizeof(value));
                playrec::menuSelect(menu + kSelected, value);
            }
            if (traceOn)
                std::fprintf(stderr, "[menu mouse] the cursor selects entry %d\n", hovered);
        }
        if (g_holdFrames == 0)
        {
            if (left > 0 && selectable)
                press(kPadCross, kPressFrames);
            else if (right > 0)
                press(kPadTriangle, kPressFrames); // (the menus' Cancel: their bar shows triangle)
            else if (wheel > 0.0f)
                press(kPadUp, kPressFrames);
            else if (wheel < 0.0f)
                press(kPadDown, kPressFrames);
        }
    }
}
