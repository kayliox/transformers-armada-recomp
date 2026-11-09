// The game's language (language.h).
//
// The PAL release (SLES-52388) carries English, French, German, Spanish and Italian - its text and its
// voices (SOUND\ENGLISH ... SOUND\SPANISH; 213 of the Spanish and music files lie past 4 GiB on the disc,
// read as files here). It has no language menu: at start-up it asks the console's system settings
// (sceScfGetLanguage, 0x135980, from 0x32E0F0 and 0x33BCA0), as a PS2 set to French plays in French.
// The runtime's system settings say English (ps2_syscalls: the OSD config), so the PAL game was English
// only. Here the setting answers instead: Auto follows Windows' display language (English when it is none
// of the five), or one of them. The game takes it at start-up.
//
// The SCE language numbers: 1 English, 2 French, 3 Spanish, 4 German, 5 Italian.

#include "language.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_settings.h"

#include <cstdint>
#include <cstdio>
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

        uint32_t chosen()
        {
            const std::string v = ps2_settings::value("TF_LANGUAGE");
            if (v == "en") return 1u;
            if (v == "fr") return 2u;
            if (v == "es") return 3u;
            if (v == "de") return 4u;
            if (v == "it") return 5u;
            return windowsLanguage();
        }

        void scfGetLanguage(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            static const uint32_t language = []()
            {
                const uint32_t l = chosen();
                const char *const names[] = {"", "English", "French", "Spanish", "German", "Italian"};
                std::fprintf(stderr, "[tf] language: %s\n", names[l]);
                return l;
            }();
            setReturnU32(ctx, language);
            ctx->pc = getRegU32(ctx, 31);
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
                 "five). Takes a restart.";
        s.choices = {{"auto", "Auto"}, {"en", "English"}, {"fr", "Fran\xC3\xA7" "ais"}, {"de", "Deutsch"},
                     {"es", "Espa\xC3\xB1ol"}, {"it", "Italiano"}};
        s.defaultValue = "auto";
        ps2_settings::add(s);
#endif
    }

    void install(PS2Runtime &runtime)
    {
#if defined(TF_GAME_PAL)
        runtime.registerFunction(kScfGetLanguage, scfGetLanguage);
#else
        (void)runtime;
#endif
    }
}
