// One-hit kills and the bosses (TODO 199) - an original PS2 behaviour, changed here as a player option.
//
// The pause menu's "one-hit kills" cheat (Square, Circle, Square, Circle, L1, L1, L2, L1: the cheat manager's flag
// +0x4D) works in the hit handler every Decepticon shares, taActorEvenSimpler's (0x3DB740): with the flag on, a hit's
// damage is the creature's whole health. Starscream (Antarctica) only goes down - his death sequence, whose end marks him
// dead (taActorBossStarscream slot 20, 0x48A1A0) and fires CS_DefeatedStarscream (his defeat film, EpisodeComplete) -
// from a knockdown: in robot form, on the ground, his stagger (taCreatureBossStarscream +speed) worn to nothing by the
// hits (TakeDamage 0x49E840). One cheat hit takes his health to 0 while his stagger has barely moved, so no knockdown
// comes, and every later hit is turned away at once (no health left): he stays in the air or on his feet, unkillable,
// and the level never ends. Measured from the user's card at 144 and at the console's 30 fps: one cheat hit, then his
// state flags cycle for minutes with no death; a single 1300-point hit without the cheat (his health 1200) gets stuck
// the same way unless some later knock happens to start the death (it did once: a 25-point knock, his defeat film, the
// level's end). The bosses that share the handler - Starscream, Megatron, Cyclonus, Tidal Wave - are fought the same
// way (Unicron's handler is not this one).
//
// The option (One-hit kills on bosses: Off by default): the cheat leaves the bosses alone - they take normal damage, so their fights play out and
// the levels end; every other Decepticon still falls to one hit. On: as the PS2 (a boss can get stuck).
//
// Decomp equivalent: in taActorEvenSimpler::TakeDamage, the one-hit-kills branch skipped for the four taActorBoss
// classes.

#include "boss_cheat.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_settings.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "game_version.h" // (TF_GA: the code addresses that differ in the PAL release)

namespace boss_cheat
{
    namespace
    {
        constexpr uint32_t kTakeDamage = TF_GA(0x003DB740u);      // taActorEvenSimpler::TakeDamage(actor, details)
        constexpr uint32_t kCheatManager = 0x006CFDF0u;    // -> the cheat codes (0x54 bytes), entered[] from +0x34
        constexpr uint32_t kOneHitKills = 0x34u + 25u;     // code 25: one-hit kills
        constexpr uint32_t kBossVtables[] = {0x006C05D0u, 0x006C07F0u, 0x006C0A50u, 0x006C80D0u}; // Megatron, Starscream, Cyclonus, Tidal Wave
        PS2Runtime::RecompiledFunction g_takeDamage = nullptr;
        std::atomic<bool> g_spare{true};

        uint32_t word(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v = 0u;
            if (const uint8_t *p = getConstMemPtr(rdram, a))
                std::memcpy(&v, p, 4u);
            return v;
        }

        void takeDamage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            uint8_t *flag = nullptr;
            if (ctx->pc == kTakeDamage && g_spare.load(std::memory_order_relaxed))
            {
                const uint32_t manager = word(rdram, kCheatManager);
                uint8_t *f = manager ? getMemPtr(rdram, manager + kOneHitKills) : nullptr;
                if (f && *f)
                {
                    const uint32_t vtable = word(rdram, getRegU32(ctx, 4));
                    for (const uint32_t boss : kBossVtables)
                        if (vtable == boss)
                        {
                            flag = f;
                            break;
                        }
                }
            }
            if (!flag)
            {
                g_takeDamage(rdram, ctx, runtime);
                return;
            }
            static int told = 0;
            if (told++ < 3)
                std::fprintf(stderr, "[cheats] one-hit kills: boss %08x takes a normal hit (TF_ONEHIT_BOSSES=1: as the PS2)\n",
                             getRegU32(ctx, 4));
            *flag = 0u;
            g_takeDamage(rdram, ctx, runtime);
            *flag = 1u;
        }
    }

    void install(PS2Runtime &runtime)
    {
        if (const char *env = std::getenv("TF_ONEHIT_BOSSES"); env && *env)
            g_spare.store(*env == '0');
        if ((g_takeDamage = runtime.lookupFunction(kTakeDamage)) != nullptr)
            runtime.registerFunction(kTakeDamage, takeDamage);
    }

    void addSettings()
    {
        ps2_settings::Setting s;
        s.key = "TF_ONEHIT_BOSSES";
        s.section = "Cheats";
        s.label = "One-hit kills on bosses";
        s.help = "With the pause menu's one-hit-kills cheat. Off (the default): bosses take normal damage, so their fights "
                 "play out and the level ends. On: bosses fall to one hit too, as on the PS2 - where a boss killed that way "
                 "can stay up, unkillable, and the level never ends (Starscream in Antarctica).";
        s.choices = {{"0", "Off"}, {"1", "On"}};
        s.defaultValue = "0";
        s.apply = [](const std::string &value)
        {
            g_spare.store(value != "1");
            return true;
        };
        ps2_settings::add(s);
    }
}
