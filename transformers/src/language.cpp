// The game's language (language.h).
//
// The PAL release (SLES-52388) carries English, French, German, Spanish and Italian - its text and its
// voices (SOUND\ENGLISH ... SOUND\SPANISH; 213 of the Spanish and music files lie past 4 GiB on the disc,
// read as files here). It has no language menu: at start-up it asks the console's system settings
// (sceScfGetLanguage, 0x135980, from 0x32E0F0 and 0x33BCA0), as a PS2 set to French plays in French.
// The runtime's system settings say English (ps2_syscalls: the OSD config), so the PAL game was English
// only. Here the setting answers instead: Auto follows Windows' display language (English when it is none
// of the five), or one of them.
//
// The game takes the language at start-up: its text (0x33BCA0 loads Strings/<language>.txt into the strings object,
// gp-0x3A44) and its option 3 (the options object's +0x14), from which 0x27E5D0 picks the voices' folder
// (gp-0x3E78, one of the table at 0x6CDB54: sound/english/ ... sound/spanish/). A save state keeps them, and the
// overlay's Restart restarts from one: the language did not change there (the user's report, 2026-11-10: "the
// voices did not change even after restart"). So the running game is set to the setting's language whenever it has
// another: the option and the voices' folder written (frame) - the next voice line is in the new language - and
// the text loaded again by the game's own loader (redirectForText). The loader reads its file from the disc and
// waits for it, which a call from the port's C++ cannot do (it returned half-way: the strings unchanged), so the
// game runs it as its own call: at the frame kick's start its registers are kept, it goes to the loader with the
// new language and returns to an added function (guest_code.h) that gives them back and enters the frame kick
// again. A change of the setting takes effect at once.
//
// The SCE language numbers: 1 English, 2 French, 3 Spanish, 4 German, 5 Italian.

#include "language.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_settings.h"

#include "game_version.h" // (TF_VA: the code addresses that differ in the PAL release)
#include "guest_code.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace language
{
    namespace
    {
        constexpr uint32_t kScfGetLanguage = 0x00135980u; // (the SDK: at the same address in both releases)

        uint32_t windowsLanguage()
        {
#if defined(_WIN32)
            switch (PRIMARYLANGID(GetUserDefaultUILanguage()))
            {
            case LANG_FRENCH: return 2u;
            case LANG_SPANISH: return 3u;
            case LANG_GERMAN: return 4u;
            case LANG_ITALIAN: return 5u;
            default: break;
            }
#endif
            return 1u;
        }

        uint32_t chosenFrom(const std::string &v)
        {
            if (v == "en") return 1u;
            if (v == "fr") return 2u;
            if (v == "es") return 3u;
            if (v == "de") return 4u;
            if (v == "it") return 5u;
            return windowsLanguage();
        }

        const char *const kNames[] = {"", "English", "French", "Spanish", "German", "Italian"};
        std::atomic<uint32_t> g_wanted{0u}; // the SCE number the setting asks for (0: not read yet)

        uint32_t wanted()
        {
            uint32_t l = g_wanted.load();
            if (l == 0u)
            {
                l = chosenFrom(ps2_settings::value("TF_LANGUAGE"));
                g_wanted.store(l);
            }
            return l;
        }

        void scfGetLanguage(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            static bool said = false;
            const uint32_t language = wanted();
            if (!said)
            {
                said = true;
                std::fprintf(stderr, "[tf] language: %s\n", kNames[language]);
            }
            setReturnU32(ctx, language);
            ctx->pc = getRegU32(ctx, 31);
        }

        // The game's records of its language, by SCE number (1 English .. 5 Italian): option 3's value (0x32E0F0's
        // mapping), the index of the voices' folder in the table at 0x6CDB54 (en, fr, de, it, es: 0x27E5D0) and the
        // strings file's number for 0x33BCA0 (3 EnglishPAL, 4 French, 5 German, 6 Italian, 7 Spanish).
        constexpr uint32_t kOption[] = {0u, 0u, 2u, 4u, 3u, 5u};
        constexpr uint32_t kVoiceFolder[] = {0u, 0u, 1u, 4u, 2u, 3u};
        constexpr uint32_t kStrings[] = {3u, 3u, 4u, 7u, 5u, 6u};
        constexpr uint32_t kOptionsObject = 0x006CF244u; // gp-0x3EAC
        constexpr uint32_t kStringsObject = 0x006CF6ACu; // gp-0x3A44
        constexpr uint32_t kVoicesFolder = 0x006CF278u;  // gp-0x3E78
        constexpr uint32_t kVoiceFolders = 0x006CDB54u;  // gp-0x559C
        // The strings' maker (0x33BF10: a new, empty table in the strings object, then 0x33BCA0 loads the language's file
        // into it and 0x1A9590 makes it the current one). The loader alone adds a file's entries to the table it has, the
        // first language's staying the ones found: the text stayed English. The old table is left as it is (screens
        // made before may still point into it): a table's memory once per change of the language.
        constexpr uint32_t kLoadStrings = TF_VA(0u, 0x0033BF10u); // (PAL only: this file's code is for the PAL release)

        uint32_t word(const uint8_t *rdram, uint32_t address)
        {
            uint32_t v = 0u;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&v, p, sizeof(v));
            return v;
        }

        // (the port's memory is larger than the console's 32 MB: the options object sits at 0x21B99F0 - pool_growth.cpp)
        bool inRam(uint32_t address) { return address >= 0x00100000u && address < PS2_RAM_SIZE; }

        // The frame kick's registers while the game runs the text loader for it (redirectForText).
        struct Kept
        {
            __m128i r[32];
            uint64_t hi, lo, hi1, lo1;
            uint32_t sa, pc, fcr31;
            float f[32];
            float fAcc;
        };
        Kept g_kept{};
        bool g_inLoader = false;
        uint32_t g_afterLoader = 0u; // the added function the loader returns to
        uint32_t g_textTried = 0u;   // the strings file asked for last (once per change)

        void afterLoader(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t strings = word(rdram, kStringsObject);
            std::fprintf(stderr, "[tf] language: the text loaded again (strings file %u)\n", inRam(strings) ? word(rdram, strings + 4u) : 0u);
            std::memcpy(ctx->r, g_kept.r, sizeof(g_kept.r));
            ctx->hi = g_kept.hi;
            ctx->lo = g_kept.lo;
            ctx->hi1 = g_kept.hi1;
            ctx->lo1 = g_kept.lo1;
            ctx->sa = g_kept.sa;
            std::memcpy(ctx->f, g_kept.f, sizeof(g_kept.f));
            ctx->f_acc = g_kept.fAcc;
            ctx->fcr31 = g_kept.fcr31;
            ctx->pc = g_kept.pc; // (the frame kick, entered again)
            g_inLoader = false;
        }
    }

    void addSettings()
    {
#if defined(TF_GAME_PAL)
        ps2_settings::Setting s;
        s.key = "TF_LANGUAGE";
        s.section = "Gameplay";
        s.label = "Language";
        s.order = 1;
        s.help = "The game's text and voices. Auto follows Windows' display language (English if it is none of the "
                 "five). Changes at once: the text on the next screen, the voices from the next line.";
        s.choices = {{"auto", "Auto"}, {"en", "English"}, {"fr", "Fran\xC3\xA7" "ais"}, {"de", "Deutsch"},
                     {"es", "Espa\xC3\xB1ol"}, {"it", "Italiano"}};
        s.defaultValue = "auto";
        s.apply = [](const std::string &value)
        {
            g_wanted.store(chosenFrom(value)); // (taken by frame on the game's thread)
            return true;
        };
        ps2_settings::add(s);
#endif
    }

    void frame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
#if defined(TF_GAME_PAL)
        const uint32_t l = wanted();
        if (l < 1u || l > 5u)
            return;
        const uint32_t options = word(rdram, kOptionsObject);
        if (inRam(options) && word(rdram, options + 0x14u) != kOption[l])
        {
            const uint32_t option = kOption[l];
            const uint32_t folder = word(rdram, kVoiceFolders + 4u * kVoiceFolder[l]);
            if (uint8_t *p = getMemPtr(rdram, options + 0x14u))
                std::memcpy(p, &option, sizeof(option));
            if (uint8_t *p = getMemPtr(rdram, kVoicesFolder); p && folder != 0u)
                std::memcpy(p, &folder, sizeof(folder));
            std::fprintf(stderr, "[tf] language: the voices in %s from the next line\n", kNames[l]);
        }
#else
        (void)rdram, (void)ctx, (void)runtime;
#endif
    }

    bool redirectForText(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
#if defined(TF_GAME_PAL)
        if (g_inLoader || g_afterLoader == 0u)
            return false;
        const uint32_t l = wanted();
        if (l < 1u || l > 5u)
            return false;
        const uint32_t strings = word(rdram, kStringsObject);
        const uint32_t loaded = inRam(strings) ? word(rdram, strings + 4u) : 0u;
        if (loaded == kStrings[l])
            g_textTried = 0u;
        if (loaded < 2u || loaded > 7u || loaded == kStrings[l] || g_textTried == kStrings[l])
            return false;
        g_textTried = kStrings[l];
        std::memcpy(g_kept.r, ctx->r, sizeof(g_kept.r));
        g_kept.hi = ctx->hi;
        g_kept.lo = ctx->lo;
        g_kept.hi1 = ctx->hi1;
        g_kept.lo1 = ctx->lo1;
        g_kept.sa = ctx->sa;
        std::memcpy(g_kept.f, ctx->f, sizeof(g_kept.f));
        g_kept.fAcc = ctx->f_acc;
        g_kept.fcr31 = ctx->fcr31;
        g_kept.pc = ctx->pc;
        g_inLoader = true;
        SET_GPR_U32(ctx, 4, strings);
        SET_GPR_U32(ctx, 5, kStrings[l]);
        SET_GPR_U32(ctx, 31, g_afterLoader);
        ctx->pc = kLoadStrings;
        std::fprintf(stderr, "[tf] language: the text in %s (strings file %u -> %u) - the game's loader\n", kNames[l], loaded, kStrings[l]);
        return true;
#else
        (void)rdram, (void)ctx;
        return false;
#endif
    }

    void install(PS2Runtime &runtime)
    {
#if defined(TF_GAME_PAL)
        runtime.registerFunction(kScfGetLanguage, scfGetLanguage);
        g_afterLoader = guest_code::addFunction(runtime, "language: after the text loader", afterLoader);
#else
        (void)runtime;
#endif
    }
}
