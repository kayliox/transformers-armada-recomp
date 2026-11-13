// The surround encoding of the game's 3D sounds (TODO 305 (10)).
//
// The game's Options have a Sound Mode (0_DEFS.TMD soundMode: NoSFX / Mono / Stereo / ProLogic2; ProLogic2 by default,
// taOptionsData::ResetToDefaults). The level's sound start-up applies it (0x280890: 3 = Pro Logic II sets the Dolby
// surround controller's flag, 0x1F1B20), and every 3D voice's channel volumes follow that flag
// (slVoice3D_PlayStation2::UpdateChannelVolumes asks the flag through 0x1F1B10): set, the left and right levels come
// from the microphone's position (0x1E92F0) and a sound beside or behind the camera gets one channel phase-inverted,
// the Pro Logic II rear encoding; clear, from a plain pan (0x1F10C0), both channels in phase. On the PS2 a Pro Logic II
// decoder put those sounds behind you; on headphones and speakers without one the inverted pair partly cancels - summed
// to mono or on a laptop's close speakers largely (measured at Amazon's lower waterfall: -46 dB stereo, -63 dB mono):
// bbatts's silent waterfall.
//
// "Surround encoding" (TF_SURROUND, F1 > Sound): Plain stereo (default) answers the flag "off" - the game's own Stereo
// panning for every 3D sound, nothing phase-inverted. Pro Logic II leaves the game's own Sound Mode in charge (its
// default is Pro Logic II). The native sound server and the IOP's play the volumes the EE works out the same way, so
// it holds for both. A change shows at once (the volumes are worked out again every frame).
//
// Decomp equivalent: UpdateChannelVolumes taking the plain pan unless the port's setting asks for Pro Logic II.

#include "surround_mode.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_settings.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace surround_mode
{
    namespace
    {
        constexpr uint32_t kDolbyFlag = 0x001F1B10u; // slAudioDeviceExtension_DolbySurroundController's flag getter
        PS2Runtime::RecompiledFunction g_flag = nullptr;
        std::atomic<bool> g_plain{true};

        bool plainOf(const std::string &v) { return v != "pl2"; }

        void flag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kDolbyFlag && g_plain.load(std::memory_order_relaxed))
            {
                SET_GPR_U32(ctx, 2, 0u);
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
            g_flag(rdram, ctx, runtime);
        }
    }

    void addSettings()
    {
        if (const char *env = std::getenv("TF_SURROUND"); env && *env)
            g_plain.store(plainOf(env));
        ps2_settings::Setting s;
        s.key = "TF_SURROUND";
        s.section = "Sound";
        s.label = "Surround encoding";
        s.order = 6;
        s.help = "Plain stereo: every sound panned left and right, as the game's Stereo sound mode; this overrides the game's "
                 "own Options > Sound Mode \"Surround\". Pro Logic II: follows the game's Sound Mode (Surround by default), for "
                 "a Pro Logic II receiver: sounds beside and behind you are phase-inverted on one side, which partly cancels "
                 "on TV, laptop and headphone speakers.";
        s.choices = {{"stereo", "Plain stereo"}, {"pl2", "Pro Logic II"}};
        s.defaultValue = "stereo";
        s.apply = [](const std::string &value)
        {
            g_plain.store(plainOf(value));
            return true;
        };
        ps2_settings::add(s);
    }

    void install(PS2Runtime &runtime)
    {
        if (const char *env = std::getenv("TF_SURROUND"); env && *env) // (install runs before addSettings: the line below)
            g_plain.store(plainOf(env));
        if ((g_flag = runtime.lookupFunction(kDolbyFlag)) == nullptr)
        {
            std::fprintf(stderr, "[sound] surround encoding: nothing at %08x to hook - the game's Sound Mode decides\n", kDolbyFlag);
            return;
        }
        runtime.registerFunction(kDolbyFlag, flag);
        std::fprintf(stderr, "[sound] surround encoding: %s (TF_SURROUND)\n",
                     g_plain.load() ? "plain stereo - the 3D sounds panned, nothing phase-inverted" : "Pro Logic II - as the game's Sound Mode");
    }
}
