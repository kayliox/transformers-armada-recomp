// The start-up screens setting (start_screens.h has the long form; TODO 110).

#include "start_screens.h"
#include "port_ext.h" // (ext::skipMovie)

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_audio_stream.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_settings.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "game_version.h" // (TF_GA: the code addresses that differ in the PAL release)

namespace start_screens
{
    namespace
    {
        // The front end (retail SLUS-20668; the team's decomp names them):
        constexpr uint32_t kIntroProcessInput = TF_GA(0x0034D060u); // taItemIntroFMV::ProcessInput(this, krInput, krPlayer)
        // ... reached through its krItem part's thunk (a jump - the function table never sees the call to 0x34D060):
        // this' = a0 - 0x9C + *(a0 + 0x20)
        constexpr uint32_t kIntroProcessInputThunk = TF_GA(0x0034E7B0u);
        constexpr uint32_t kIntroItemVtable = 0x006B8250u;   // taItemIntroFMV's own vtable, at the item's +4 (not the Extras' movies)
        constexpr uint32_t kIntroState = 0x006CFA6Cu;        // -> taUIIntro (func_004CE7C0): the start movie it plays
        constexpr uint32_t kIntroPath = 0x38u;               // taUIIntro +0x38: the movie's path (std::string)
        constexpr uint32_t kIntroMovie = 0x50u;              // taUIIntro +0x50: its stream, while it plays
        constexpr uint32_t kSkipInput = 5u;                  // krInput 5: the screen system's "skip" (the movie's next screen)
        constexpr uint32_t kTitleUpdate = 0x0033B690u;       // taScreenPrGTitle::Update(this, dt) (vtable slot 16)
        constexpr uint32_t kTitleStartSound = 0xC8u;         // its start sound's handle: nonzero = Start was pressed
        constexpr uint32_t kStartMenuUpdate = TF_GA(0x00557450u);   // taScreenStartMenu::Update (slot 16): the main menu is up
        // (TODO 116) The screens before the movies: the memory card's check, then the copyright screen.
        // taScreenStartInitCardCheck::ProcessInput (slot 18, called every frame): a state machine at +0x848 with a
        // timer at +0x84C (seconds, counted down by its Update while it is above 0), its messages set by 0x552C10:
        //   1/2 "Loading..." (the card read - real work, kept), 3 "Load Successful" (1 s), 9 "No save data present"
        //   (3 s, or X), 0xC "This game uses an autosave feature..." (5 s), then 0xA = "%Legal" (the copyright screen);
        //   4/5 a load error or a corrupt save (Retry / Continue without loading), 6 no card (Yes / No), 8 no room
        //   (Yes / No), 0xB autosave disabled (Continue) - choices, never touched.
        constexpr uint32_t kCardCheckProcessInput = TF_GA(0x00552370u);
        constexpr uint32_t kCardCheckState = 0x848u;
        constexpr uint32_t kCardCheckTimer = 0x84Cu;
        // taItemTimeout::ProcessInput(this, krInput) (0x352860; its timer: +0x24 the timeout, +0x28 the part that can
        // not be skipped, +0x2C the time so far, +0x18 the next screen's name) and its krItem part's thunk (a jump:
        // this' = a0 - 0x30 + *(a0 + 0x20)). The copyright screen ("Legal") is such a timer: 8 s, the first 4 not
        // skippable, then "%AtariLogo" (the first movie).
        constexpr uint32_t kTimeoutProcessInput = TF_GA(0x00352860u);
        constexpr uint32_t kTimeoutProcessInputThunk = TF_GA(0x00352C20u);

        enum Mode : int
        {
            kMenu = 0,  // straight to the main menu (the default)
            kTitle = 1, // the movies skipped, the title kept
            kAll = 2,   // everything shown
            kScripted = 3, // a scripted test run without the setting: as before it came (below)
        };
        std::atomic<int> g_mode{kMenu};

        // (TODO 341 (1)) The game's VBlank tick when the copyright screen's timer last took input while it ran (0 never), and
        // since when it has without a break - in game time, as the start's turbo runs many frames in a moment of the PC's -
        // and the tick of the last game frame (frame()).
        std::atomic<uint64_t> g_copyrightTick{0}, g_copyrightSince{0}, g_frameTick{0};
        std::atomic<int> g_legalShape{1};
        // (TODO 341 (2)) The same for the title ("press Start") and the card check's notices: their screens' updates, while
        // they run; and the option to show them at 4:3 in a wide picture (TF_START_SHAPE).
        std::atomic<uint64_t> g_titleTick{0}, g_titleSince{0}, g_cardTick{0}, g_cardSince{0};
        std::atomic<int> g_startShape{1};

        void stampSeen(std::atomic<uint64_t> &at, std::atomic<uint64_t> &since, uint64_t now)
        {
            if (now > at.load(std::memory_order_relaxed) + 12u)
                since.store(now, std::memory_order_relaxed);
            at.store(now, std::memory_order_relaxed);
        }

        bool seenNow(const std::atomic<uint64_t> &at, const std::atomic<uint64_t> &since)
        {
            const uint64_t t = at.load(std::memory_order_relaxed), now = g_frameTick.load(std::memory_order_relaxed);
            return t != 0 && now <= t + 6u && t >= since.load(std::memory_order_relaxed) + 18u;
        }


        PS2Runtime::RecompiledFunction g_introProcessInput = nullptr;
        PS2Runtime::RecompiledFunction g_introProcessInputThunk = nullptr;
        PS2Runtime::RecompiledFunction g_titleUpdate = nullptr;
        PS2Runtime::RecompiledFunction g_startMenuUpdate = nullptr;
        PS2Runtime::RecompiledFunction g_cardCheckProcessInput = nullptr;
        PS2Runtime::RecompiledFunction g_timeoutProcessInput = nullptr;
        PS2Runtime::RecompiledFunction g_timeoutProcessInputThunk = nullptr;

        // The session's front end so far (the game thread only).
        bool g_titleSeen = false;    // the title has been up once
        bool g_menuReached = false;  // the main menu has been up once: the title is never skipped again
        bool g_titlePressed = false; // the title was told Start was pressed (logged once)
        std::string g_lastMovie;     // (the trace and the skip log: once a movie)

        bool trace()
        {
            static const bool on = std::getenv("TF_START_TRACE") && *std::getenv("TF_START_TRACE") == '1';
            return on;
        }

        int modeOf(const std::string &value)
        {
            if (value == "title")
                return kTitle;
            if (value == "all")
                return kAll;
            if (value == "scripted")
                return kScripted; // (TODO 150: a play recording of a scripted run says so - noteScriptedRun)
            return kMenu; // ("menu", empty, anything unknown)
        }

        uint32_t read32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t v = 0u;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&v, p, sizeof(v));
            return v;
        }

        // The game's std::string (CodeWarrior's): byte 0's low bit set = the characters elsewhere, their
        // address at +8; clear = held in the string, from +1.
        std::string stringAt(const uint8_t *rdram, uint32_t string)
        {
            std::string out;
            const uint8_t *flag = string ? getConstMemPtr(rdram, string) : nullptr;
            if (!flag)
                return out;
            const uint32_t chars = (*flag & 1u) ? read32(rdram, string + 8u) : string + 1u;
            for (uint32_t i = 0; chars != 0u && i < 160u; ++i)
            {
                const uint8_t *c = getConstMemPtr(rdram, chars + i);
                if (!c || *c < 0x20u || *c >= 0x7Fu)
                    break;
                out.push_back(static_cast<char>(*c));
            }
            return out;
        }

        // "FMV/NATARI.PSS" (or a path with other separators) -> "NATARI".
        std::string keyOf(const std::string &path)
        {
            size_t from = path.find_last_of("/\\:");
            from = from == std::string::npos ? 0u : from + 1u;
            std::string key = path.substr(from);
            if (const size_t dot = key.find('.'); dot != std::string::npos)
                key.resize(dot);
            for (char &c : key)
                if (c >= 'a' && c <= 'z')
                    c = static_cast<char>(c - 'a' + 'A');
            return key;
        }

        bool skipsKey(const std::string &key)
        {
            const int mode = g_mode.load(std::memory_order_relaxed);
            if (mode == kAll)
                return false;
            if (mode == kScripted) // (the old "Intro screens" switch Off: its three movies, by the FMV skip's presses)
            {
                const char *old = std::getenv("TF_CFG_runUIIntro");
                return old && std::string(old) == "0" && (key == "NATARI" || key == "NMH_LOGO" || key == "NINTRO");
            }
            // The logos and the story intro; the attract film only before the first title (the start's
            // "Attract1stPlay" - the title's own after 30 s idle is the player's to watch).
            return key == "NATARI" || key == "NMH_LOGO" || key == "NINTRO" || (key == "NATTRACT" && !g_titleSeen);
        }

        uint64_t tick(PS2Runtime *runtime) { return runtime->eeScheduler().currentVSyncTick(); }

        // (the seconds since the game started, for the log: this file's statics are made as the program starts)
        const std::chrono::steady_clock::time_point g_started = std::chrono::steady_clock::now();
        double secondsSinceStart()
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_started).count();
        }

        // (TODO 298) The start's turbo (start_screens.h): held from install until the main menu, released once.
        std::atomic<bool> g_turbo{false};
        PS2Runtime *g_runtime = nullptr;

        void releaseTurbo(const char *why)
        {
            if (!g_turbo.exchange(false) || !g_runtime)
                return;
            g_runtime->eeScheduler().setTurboHeld(false);
            ps2_audio_stream::setMuted(false);
            std::fprintf(stderr, "[tf] start screens: the console's pace again - %s (vblank %llu, %.2f s after the start)\n", why,
                         static_cast<unsigned long long>(tick(g_runtime)), secondsSinceStart());
        }

        // (TODO 116) The card check's notices and the copyright screen skipped in "menu" mode, before the main menu has
        // been up once. TF_TEST_KEEP_NOTICES=1 (tests): kept, as item 110 had them - for before/after timings.
        bool skipNotices()
        {
            static const bool keep = std::getenv("TF_TEST_KEEP_NOTICES") && *std::getenv("TF_TEST_KEEP_NOTICES") == '1';
            return !keep && g_mode.load(std::memory_order_relaxed) == kMenu && !g_menuReached;
        }

#if defined(TF_GAME_PAL)
        // The PAL release asks, after the card check, for the TV's frequency (taScreenFrequencySelect: 50 Hz, 60 Hz,
        // Test 60 Hz): the card check goes on to "%PAL_FrequencySelect" when 0x32E5C0 says the console is in its 50 Hz
        // mode, else to "%Legal" (0x552350). The port times the game as in its 60 Hz mode in both (rates.cpp): the
        // choice only picks the picture's mode. "Straight to the main menu" leaves the question out - the card check
        // asked it is answered "not 50 Hz" and goes on to the copyright screen (itself left out), the game in its 50 Hz
        // mode as it starts; "Everything" keeps it.
        constexpr uint32_t kIsPal50Hz = 0x0032E5C0u;
        constexpr uint32_t kCardCheckAsks = TF_VA(0u, 0x00552424u); // (PAL only: the USA release has no such screen)
        PS2Runtime::RecompiledFunction g_isPal50Hz = nullptr;

        void isPal50Hz(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (getRegU32(ctx, 31) == kCardCheckAsks && skipNotices())
            {
                std::fprintf(stderr, "[tf] start screens: the PAL 50/60 Hz menu left out (the game's speed is the same in both)\n");
                setReturnU32(ctx, 0u);
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
            g_isPal50Hz(rdram, ctx, runtime);
        }
#endif

        // The skip input for a start movie's item `self` (a1 = krInput 5) when the setting skips its movie.
        void skipInput(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t self)
        {
            const uint32_t intro = read32(rdram, kIntroState);
            // Only the start movies' items (not the Extras' movies, their own classes with this function
            // inherited) and only once the movie's stream plays - then the game's own skip, as the player's.
            if (read32(rdram, self + 4u) == kIntroItemVtable && intro != 0u && read32(rdram, intro + kIntroMovie) != 0u)
            {
                const std::string key = keyOf(stringAt(rdram, intro + kIntroPath));
                // (An extension's skipped movie, port_ext.h skipMovie - e.g. the overlay's Playable Decepticons skips the
                // start movies that show the Autobots whatever the setting; decided once a movie, at its first frame)
                static bool extensionSkip = false;
                if (key != g_lastMovie)
                    extensionSkip = ext::skipMovie(key);
                const bool skip = (g_mode.load(std::memory_order_relaxed) != kScripted && skipsKey(key)) || extensionSkip;
                if (key != g_lastMovie)
                {
                    g_lastMovie = key;
                    if (skip || trace())
                        std::fprintf(stderr, "[tf] start screens: movie %s %s (vblank %llu, %.2f s)\n", key.c_str(),
                                     skip ? "skipped" : "plays", static_cast<unsigned long long>(tick(runtime)), secondsSinceStart());
                }
                if (skip)
                    SET_GPR_U32(ctx, 5, kSkipInput);
            }
        }

        void introProcessInput(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            skipInput(rdram, ctx, runtime, getRegU32(ctx, 4));
            g_introProcessInput(rdram, ctx, runtime);
        }

        void introProcessInputThunk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t part = getRegU32(ctx, 4);
            skipInput(rdram, ctx, runtime, part - 0x9Cu + read32(rdram, part + 0x20u));
            g_introProcessInputThunk(rdram, ctx, runtime);
        }

        void titleUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t self = getRegU32(ctx, 4);
            stampSeen(g_titleTick, g_titleSince, tick(runtime)); // (TODO 341 (2))
            if (!g_titleSeen)
            {
                g_titleSeen = true;
                if (trace())
                    std::fprintf(stderr, "[tf] start screens: the title (vblank %llu, %.2f s)\n", static_cast<unsigned long long>(tick(runtime)),
                                 secondsSinceStart());
                if (g_mode.load(std::memory_order_relaxed) == kTitle)
                    releaseTurbo("the title");
            }
            // Before its update runs: the update itself goes to the main menu once the screen is shown (its
            // state 2) and Start was pressed - this is that press, kept until it acts.
            if (g_mode.load(std::memory_order_relaxed) == kMenu && !g_menuReached)
            {
                if (uint8_t *p = getMemPtr(rdram, self + kTitleStartSound))
                {
                    uint32_t sound = 0u;
                    std::memcpy(&sound, p, sizeof(sound));
                    if (sound == 0u)
                    {
                        sound = 1u; // (no sound of its own: the skip is silent)
                        std::memcpy(p, &sound, sizeof(sound));
                    }
                    if (!g_titlePressed)
                    {
                        g_titlePressed = true;
                        std::fprintf(stderr, "[tf] start screens: the title skipped - Start, as the player presses it (vblank %llu)\n",
                                     static_cast<unsigned long long>(tick(runtime)));
                    }
                }
            }
            g_titleUpdate(rdram, ctx, runtime);
        }

        // (TODO 116) Before the main menu has been up once, with "Straight to the main menu": the card check's
        // notices that only wait - "Load Successful", "No save data present", the autosave notice - have their own
        // timer run out, so the screen's own code goes on to the next as it does when their seconds have passed.
        // The card is read as always ("Loading..." is the real read); its errors and questions (a load error, a
        // corrupt save, no card, no room, autosave disabled) wait for the player as on the PS2.
        const char *noticeName(uint32_t state)
        {
            switch (state)
            {
            case 3u: return "Load Successful";
            case 9u: return "No save data present";
            case 0xCu: return "the autosave notice";
            default: return nullptr;
            }
        }

        void cardCheckProcessInput(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t self = getRegU32(ctx, 4);
            stampSeen(g_cardTick, g_cardSince, tick(runtime)); // (TODO 341 (2))
            static uint32_t lastState = ~0u;
            const uint32_t state = read32(rdram, self + kCardCheckState);
            if (state != lastState)
            {
                lastState = state;
                if (trace())
                    std::fprintf(stderr, "[tf] start screens: card check state %u (vblank %llu, %.2f s)\n", state,
                                 static_cast<unsigned long long>(tick(runtime)), secondsSinceStart());
                // (TODO 298) A question or an error (4/5 a load error or a corrupt save, 6 no card, 8 no room, 0xB
                // autosave disabled): the player's, at the game's own pace.
                if (state == 4u || state == 5u || state == 6u || state == 8u || state == 0xBu)
                    releaseTurbo("the memory card asks the player");
            }
            if (skipNotices())
            {
                if (const char *notice = noticeName(state))
                {
                    if (uint8_t *timer = getMemPtr(rdram, self + kCardCheckTimer))
                    {
                        float left = 0.0f;
                        std::memcpy(&left, timer, sizeof(left));
                        if (left > 0.0f)
                        {
                            left = 0.0f;
                            std::memcpy(timer, &left, sizeof(left));
                            std::fprintf(stderr, "[tf] start screens: %s skipped - its time run out, as when it has been shown (vblank %llu)\n",
                                         notice, static_cast<unsigned long long>(tick(runtime)));
                        }
                    }
                }
            }
            g_cardCheckProcessInput(rdram, ctx, runtime);
        }

        // The copyright screen's timer (taItemTimeout, next screen "%AtariLogo"): its time run out - its own way on to
        // the next screen, as after its 8 s.
        void timeoutInput(uint8_t *rdram, PS2Runtime *runtime, uint32_t self)
        {
            const uint32_t next = read32(rdram, self + 0x18u);
            char name[12] = {};
            for (uint32_t i = 0; next != 0u && i + 1u < sizeof(name); ++i)
            {
                const uint8_t *c = getConstMemPtr(rdram, next + i);
                if (!c || *c == 0u)
                    break;
                name[i] = static_cast<char>(*c);
            }
            if (std::strcmp(name, "%AtariLogo") != 0)
                return;
            uint8_t *p = getMemPtr(rdram, self + 0x24u);
            if (!p)
                return;
            float timer[3] = {}; // the timeout, the part that cannot be skipped, the time so far
            std::memcpy(timer, p, sizeof(timer));
            const float end = timer[0] > timer[1] ? timer[0] : timer[1];
            // (TODO 341 (1): the screen counts only while its timer runs - its item is still handed input for a frame or two
            // between the start movies after it, its time long run out)
            if (timer[2] < end)
                stampSeen(g_copyrightTick, g_copyrightSince, tick(runtime));
            if (!skipNotices())
                return;
            if (timer[2] < end)
            {
                timer[2] = end;
                std::memcpy(p + 8u, &timer[2], sizeof(float));
                static bool logged = false;
                if (!logged)
                {
                    logged = true;
                    std::fprintf(stderr, "[tf] start screens: the copyright screen skipped - its %.0f s run out (vblank %llu, %.2f s)\n", end,
                                 static_cast<unsigned long long>(tick(runtime)), secondsSinceStart());
                }
            }
        }

        void timeoutProcessInput(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            timeoutInput(rdram, runtime, getRegU32(ctx, 4));
            g_timeoutProcessInput(rdram, ctx, runtime);
        }

        void timeoutProcessInputThunk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t part = getRegU32(ctx, 4);
            timeoutInput(rdram, runtime, part - 0x30u + read32(rdram, part + 0x20u));
            g_timeoutProcessInputThunk(rdram, ctx, runtime);
        }

        void startMenuUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!g_menuReached)
            {
                g_menuReached = true;
                g_titleSeen = true;
                std::fprintf(stderr, "[tf] start screens: the main menu (vblank %llu, %.2f s after the start)\n",
                             static_cast<unsigned long long>(tick(runtime)), secondsSinceStart());
                releaseTurbo("the main menu");
            }
            g_startMenuUpdate(rdram, ctx, runtime);
        }
    }

    // A scripted test run (TF_INPUT, docs/DEBUGGING.md) with the setting given neither in settings.ini nor the
    // environment starts as before the setting came (the old switch's movies skipped by the FMV skip's presses, the
    // title kept): its presses are timed for that route, and runs stay comparable with older builds'.
    // TF_START_SCREENS=menu|title|all in its environment tests the setting itself. Said in the environment
    // (TF_START_SCREENS=scripted) before a play recording takes its settings (TODO 150): the replay has no TF_INPUT
    // (its presses are the recording's), and without it went straight to the main menu - the movies and the title
    // skipped where the recording had them, the random seed apart at VBlank 752.
    void noteScriptedRun()
    {
        if (std::getenv("TF_START_SCREENS") || !std::getenv("TF_INPUT") || !*std::getenv("TF_INPUT"))
            return;
#if defined(_WIN32)
        _putenv_s("TF_START_SCREENS", "scripted");
#else
        setenv("TF_START_SCREENS", "scripted", 1);
#endif
    }

    bool copyrightShown()
    {
        // (shown while its timer took input within the last 6 VBlanks, after 18 in a row: a frame or two of its item between
        // the start movies is not the screen)
        return seenNow(g_copyrightTick, g_copyrightSince);
    }

    bool legalShapeOn() { return g_legalShape.load(std::memory_order_relaxed) != 0; }

    bool startScreen43()
    {
        return g_startShape.load(std::memory_order_relaxed) != 0 && (seenNow(g_titleTick, g_titleSince) || seenNow(g_cardTick, g_cardSince));
    }

    void addSettings()
    {
        {
            // (TODO 341 (1)) The copyright screen's logos at their own shape.
            ps2_settings::Setting s;
            s.key = "TF_LEGAL_SHAPE";
            s.section = "Display";
            s.label = "Copyright screen logos";
            s.order = 60;
            s.help = "The Transformers and Hasbro logos on the copyright screen at start: at their own shape, or stretched a "
                     "little wide as on the PS2 (its picture is drawn 512 pixels wide across the TV).";
            s.choices = {{"1", "Their own shape"}, {"0", "As on the PS2"}};
            s.defaultValue = "1";
            s.apply = [](const std::string &value)
            {
                g_legalShape.store(value == "0" ? 0 : 1, std::memory_order_relaxed);
                return true;
            };
            ps2_settings::add(s);
            g_legalShape.store(ps2_settings::value("TF_LEGAL_SHAPE") == "0" ? 0 : 1, std::memory_order_relaxed);
        }
        {
            // (TODO 341 (2)) The title and the card check's notices at 4:3 in a wide picture.
            ps2_settings::Setting s;
            s.key = "TF_START_SHAPE";
            s.section = "Display";
            s.label = "Start screens on a wide screen";
            s.order = 61;
            s.help = "The title (\"Press Start\") and the memory card notices at start are pictures made for a 4:3 TV. "
                     "4:3: shown at that shape, black at the sides. Stretched: widened to the screen, as the PS2 does in its "
                     "widescreen mode.";
            s.choices = {{"1", "4:3"}, {"0", "Stretched"}};
            s.defaultValue = "1";
            s.apply = [](const std::string &value)
            {
                g_startShape.store(value == "0" ? 0 : 1, std::memory_order_relaxed);
                return true;
            };
            ps2_settings::add(s);
            g_startShape.store(ps2_settings::value("TF_START_SHAPE") == "0" ? 0 : 1, std::memory_order_relaxed);
        }
        ps2_settings::Setting s;
        s.key = "TF_START_SCREENS";
        s.section = "Gameplay";
        s.label = "When the game starts";
        s.order = 5;
        s.help = "Straight to the main menu: the memory card's notices (Load Successful, the autosave notice), the copyright "
                 "screen, the logos, the attract film, the title (\"press Start\") and the story intro after New Game are "
                 "skipped - you land on New Game / Continue / Options in a second or two, the screen black until then (a card "
                 "error or question still shows). Title screen: only the movies skipped. Everything: as on the PS2. From the "
                 "next start.";
        s.choices = {{"menu", "Straight to the main menu"}, {"title", "Title screen (no movies)"}, {"all", "Everything"}};
        s.defaultValue = "menu";
        s.apply = [](const std::string &value)
        {
            g_mode.store(modeOf(value));
            return true;
        };
        ps2_settings::add(s);
        g_mode.store(modeOf(ps2_settings::value("TF_START_SCREENS")));
        if (const char *e = std::getenv("TF_START_SCREENS"); e && std::strcmp(e, "scripted") == 0)
            g_mode.store(kScripted); // (a scripted run, or its play recording's replay: noteScriptedRun above)
        static const char *const kModes[] = {"straight to the main menu", "the title, no movies", "everything",
                                             "as before the setting (a scripted run without it)"};
        std::fprintf(stderr, "[tf] start screens: %s\n", kModes[g_mode.load()]);
        if (g_mode.load() != kMenu && g_mode.load() != kTitle)
            releaseTurbo("the setting shows the start");
    }

    void frame(PS2Runtime &runtime, bool inGame)
    {
        g_frameTick.store(tick(&runtime), std::memory_order_relaxed); // (TODO 341 (1): copyrightShown's clock)
        if (!g_turbo.load(std::memory_order_relaxed))
            return;
        const int mode = g_mode.load(std::memory_order_relaxed);
        if (mode != kMenu && mode != kTitle)
            releaseTurbo("the setting shows the start");
        else if (inGame)
            releaseTurbo("a level or the HQ runs");
        else if (tick(&runtime) > 3000u)
            releaseTurbo("3000 VBlanks and no main menu");
    }

    bool starting() { return g_turbo.load(std::memory_order_relaxed); }

    void install(PS2Runtime &runtime)
    {
        if ((g_introProcessInput = runtime.lookupFunction(kIntroProcessInput)) != nullptr)
            runtime.registerFunction(kIntroProcessInput, introProcessInput);
        if ((g_introProcessInputThunk = runtime.lookupFunction(kIntroProcessInputThunk)) != nullptr)
            runtime.registerFunction(kIntroProcessInputThunk, introProcessInputThunk);
        if ((g_titleUpdate = runtime.lookupFunction(kTitleUpdate)) != nullptr)
            runtime.registerFunction(kTitleUpdate, titleUpdate);
        if ((g_startMenuUpdate = runtime.lookupFunction(kStartMenuUpdate)) != nullptr)
            runtime.registerFunction(kStartMenuUpdate, startMenuUpdate);
        if ((g_cardCheckProcessInput = runtime.lookupFunction(kCardCheckProcessInput)) != nullptr)
            runtime.registerFunction(kCardCheckProcessInput, cardCheckProcessInput);
        if ((g_timeoutProcessInput = runtime.lookupFunction(kTimeoutProcessInput)) != nullptr)
            runtime.registerFunction(kTimeoutProcessInput, timeoutProcessInput);
        if ((g_timeoutProcessInputThunk = runtime.lookupFunction(kTimeoutProcessInputThunk)) != nullptr)
            runtime.registerFunction(kTimeoutProcessInputThunk, timeoutProcessInputThunk);
#if defined(TF_GAME_PAL)
        if ((g_isPal50Hz = runtime.lookupFunction(kIsPal50Hz)) != nullptr)
            runtime.registerFunction(kIsPal50Hz, isPal50Hz);
#endif
        if (!(g_introProcessInputThunk && g_titleUpdate && g_startMenuUpdate && g_cardCheckProcessInput && g_timeoutProcessInputThunk))
            std::fprintf(stderr, "[tf] start screens: a front-end function not found - those screens are shown as usual\n");
        // (TODO 298) The start unthrottled until the main menu: not in a scripted run or one that shows the start (its
        // setting comes later - addSettings releases it then), not when a start-up state is loaded, not without the
        // main menu's hook (it would never be released before the 3000 VBlanks).
        g_runtime = &runtime;
        const char *env = std::getenv("TF_START_SCREENS");
        const char *off = std::getenv("TF_START_TURBO");
        const char *state = std::getenv("PS2X_LOAD_STATE");
        if (g_startMenuUpdate && !(off && *off == '0') && !(state && *state) &&
            !(env && (std::strcmp(env, "all") == 0 || std::strcmp(env, "scripted") == 0)))
        {
            g_turbo.store(true);
            runtime.eeScheduler().setTurboHeld(true);
            ps2_audio_stream::setMuted(true);
        }
    }

    bool skipsMovie(const char *movieKey) { return movieKey && skipsKey(movieKey); }
}
