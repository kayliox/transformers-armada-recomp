// Logging the drop ships, what they carry and the spider tanks (ai_probe.h). Read only.
//
// taCreatureDropShip: health +0x28, flags +0x08 (u64), the update mode
// (a member-function pointer, 12 bytes) at +0x5C, the slot being dropped +0x4C, the spawner +0x70 (its vector of
// Data* {char type; creature*} at +0x74: capacity, count, data), the body (the actor, taActorDropShip) +0x9C, the
// slots' behaviours +0xC8 (count +0xCC, data +0xD0; 1 = rides along and shoots from the door). The body:
// +0x11C/+0x11E flight flags, +0x11F dying, +0x128 the fall's timer. A creature's position: actor +0x24 ->
// scene node +0xC -> transform +0x14 -> +0x30 (0x2A6800). taCreatureAI: the running goal +0xA0 (26 = none),
// the plan's count +0xEC, the state +0x110 (as the drop ship reads it).

#include "ai_probe.h"

#include "guest_code.h"

#include "port.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <unordered_map>
#include "game_version.h" // (TF_GA: the code addresses that differ in the PAL release)

namespace ai_probe
{
    namespace
    {
        constexpr uint32_t kDropShipUpdate = TF_GA(0x003E28C0u);
        constexpr uint32_t kGruntActorUpdate = TF_GA(0x0044A8D0u), kSpiderActorUpdate = TF_GA(0x00402D20u); // (their vtables' +0x14)
        constexpr uint32_t kSpiderAim = TF_GA(0x00401BD0u); // the spider tank's turret aim (actor, dt)
        
        PS2Runtime::RecompiledFunction g_dropShipUpdate = nullptr, g_gruntActorUpdate = nullptr, g_spiderActorUpdate = nullptr, g_spiderAim = nullptr;
        std::unordered_map<uint32_t, uint64_t> g_aimSeen;
        // TF_AI_PROBE_KILL=<tick>[,fall] (a test action, not read-only): the first drop ship updated at or after the tick
        // while it still carries a creature it has not dropped dies as its TakeDamage makes it die - health 0 and
        // StartDeath (0x423F90, the explosion: its ActorDeathStartedCallback lets the carried go), or with ",fall"
        // StartFallToDeath (0x424620, the tumble a 0x100 hit starts, which ends in StartDeath). That frame's update
        // is the call instead (a tail call: the ship's caller gets it back).
        uint64_t g_killAt = 0u;
        bool g_killFall = false, g_killed = false;
        // ",hit": the death the game gives it - its TakeDamage (taCreatureDropShip slot 4, 0x3E29A0) with damage of
        // type 0x100 (the kind that may start the tumble, one time in ten) every frame from the tick until it falls or
        // dies; the damage details in an added block (0x60 bytes, taDamageDetails as KillCreatureIfNecessary builds
        // them: position, type +0x40, amount +0x54).
        bool g_killHit = false;
        // ",hitfall": hits of type 0x100 (amount 40) every 6 ticks of game time from the tick, the first one with the
        // tumble's one-in-ten roll (taScriptRoutines::TrueWithPercentage, 0x2A5260) made to come up, then on through
        // the fall until it has no health left - a ship shot down while it drops, as the player does it.
        bool g_hitFall = false, g_forceRoll = false;
        uint64_t g_lastHit = 0u;
        constexpr uint32_t kTrueWithPercentage = 0x002A5260u;
        PS2Runtime::RecompiledFunction g_trueWithPercentage = nullptr;
        constexpr uint32_t kDropIt = TF_GA(0x003E0D30u);
        PS2Runtime::RecompiledFunction g_dropIt = nullptr;
        uint32_t g_damage = 0u;
        constexpr uint32_t kTakeDamage = TF_GA(0x003E29A0u);
        constexpr uint32_t kStartDeath = TF_GA(0x00423F90u), kStartFallToDeath = TF_GA(0x00424620u);
        uint64_t g_every = 15u;
        bool g_all = false;
        struct Seen
        {
            uint64_t tick = 0u;
            std::string state;
        };
        std::unordered_map<uint32_t, Seen> g_seen;
        std::set<uint32_t> g_carried;

        uint32_t word(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v = 0u;
            if (a && (a & 3u) == 0u)
                if (const uint8_t *p = getConstMemPtr(rdram, a))
                    std::memcpy(&v, p, sizeof(v));
            return v;
        }

        void setWord(uint8_t *rdram, uint32_t a, uint32_t v)
        {
            if (uint8_t *p = getMemPtr(rdram, a))
                std::memcpy(p, &v, sizeof(v));
        }
        uint8_t byte(const uint8_t *rdram, uint32_t a)
        {
            const uint8_t *p = a ? getConstMemPtr(rdram, a) : nullptr;
            return p ? *p : 0u;
        }
        float flt(const uint8_t *rdram, uint32_t a)
        {
            const uint32_t v = word(rdram, a);
            float f;
            std::memcpy(&f, &v, sizeof(f));
            return f;
        }

        // The creature's position (0x2A6800's way), or nothing.
        bool position(const uint8_t *rdram, uint32_t creature, float out[3])
        {
            const uint32_t actor = word(rdram, creature + 0x24u);
            const uint32_t node = actor ? word(rdram, actor + 0xCu) : 0u;
            const uint32_t transform = node ? word(rdram, node + 0x14u) : 0u;
            if (!transform)
                return false;
            for (int i = 0; i < 3; ++i)
                out[i] = flt(rdram, transform + 0x30u + 4u * i);
            return true;
        }

        std::string creatureText(const uint8_t *rdram, uint32_t creature)
        {
            char text[200];
            float p[3] = {};
            const bool at = position(rdram, creature, p);
            const uint32_t flagsLo = word(rdram, creature + 8u), flagsHi = word(rdram, creature + 12u);
            std::snprintf(text, sizeof(text), "%08x vt %06x hp %.1f flags %08x%08x s10 %x goal %u plan %u st %u pos %s(%.1f %.1f %.1f)",
                          creature, word(rdram, creature), flt(rdram, creature + 0x28u), flagsHi, flagsLo, word(rdram, creature + 0x10u),
                          word(rdram, creature + 0xA0u), word(rdram, creature + 0xECu), word(rdram, creature + 0x110u),
                          at ? "" : "? ", p[0], p[1], p[2]);
            return text;
        }

        // Logs when the state (all but the position) changed, or every g_every ticks.
        void report(uint32_t key, const std::string &state, const std::string &position, const char *what)
        {
            const uint64_t tick = tf::port::testTickNow();
            Seen &seen = g_seen[key];
            if (seen.state == state && tick < seen.tick + g_every)
                return;
            seen.tick = tick;
            seen.state = state;
            std::fprintf(stderr, "[ai probe] tick %llu %s %s %s\n", static_cast<unsigned long long>(tick), what, state.c_str(),
                         position.c_str());
        }

        void dropShipUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ship = getRegU32(ctx, 4);
            const uint32_t body = word(rdram, ship + 0x9Cu);
            char text[400];
            std::snprintf(text, sizeof(text), "ship %08x hp %.1f mode %08x/%08x/%08x slot %u kind %d body %08x 11c %u 11e %u 11f %u",
                          ship, flt(rdram, ship + 0x28u), word(rdram, ship + 0x5Cu), word(rdram, ship + 0x60u),
                          word(rdram, ship + 0x64u), word(rdram, ship + 0x4Cu), static_cast<int>(word(rdram, ship + 0x68u)),
                          body, byte(rdram, body + 0x11Cu), byte(rdram, body + 0x11Eu), byte(rdram, body + 0x11Fu));
            std::string state = text;
            const uint32_t count = word(rdram, ship + 0x78u), data = word(rdram, ship + 0x7Cu);
            const uint32_t behaviours = word(rdram, ship + 0xD0u), behaviourCount = word(rdram, ship + 0xCCu);
            for (uint32_t i = 0; i < count && i < 8u; ++i)
            {
                const uint32_t entry = word(rdram, data + 4u * i);
                const uint32_t creature = entry ? word(rdram, entry + 4u) : 0u;
                std::snprintf(text, sizeof(text), " | %u beh %d c %08x", i,
                              i < behaviourCount ? static_cast<int>(word(rdram, behaviours + 4u * i)) : -1, creature);
                state += text;
                if (creature)
                    g_carried.insert(creature);
            }
            float p[3] = {};
            std::string where;
            if (body)
            {
                const uint32_t node = word(rdram, body + 0xCu), transform = node ? word(rdram, node + 0x14u) : 0u;
                if (transform)
                    for (int i = 0; i < 3; ++i)
                        p[i] = flt(rdram, transform + 0x30u + 4u * i);
                std::snprintf(text, sizeof(text), "at (%.1f %.1f %.1f) fall %.2f dt %.5f", p[0], p[1], p[2], flt(rdram, body + 0x128u),
                              ctx->f[12]);
                where = text;
            }
            report(ship, state, where, "dropship");
            const uint64_t now = tf::port::testTickNow();
            const bool hitDue = g_hitFall ? (body && byte(rdram, body + 0x11Fu) == 0u && now >= g_killAt && flt(rdram, ship + 0x28u) > 0.0f &&
                                             (g_lastHit != 0u || word(rdram, ship + 0x4Cu) < count) && now >= g_lastHit + 6u)
                                          : (body && byte(rdram, body + 0x11Fu) == 0u && flt(rdram, body + 0x128u) == 0.0f &&
                                             word(rdram, ship + 0x4Cu) < count && now >= g_killAt && flt(rdram, ship + 0x28u) > 0.0f);
            if (g_killHit && g_damage && hitDue)
            {
                if (g_hitFall)
                {
                    g_forceRoll = g_lastHit == 0u;
                    g_lastHit = now;
                }
                if (PS2Runtime::RecompiledFunction fn = runtime->lookupFunction(kTakeDamage))
                {
                    uint8_t details[0x60] = {};
                    std::memcpy(details, p, sizeof(p)); // where it was hit
                    const float one = 1.0f, amount = g_hitFall ? 40.0f : 20.0f;
                    const uint32_t type = 0x100u, minus1 = 0xFFFFFFFFu;
                    std::memcpy(details + 0x20, &one, 4);  // the push's direction (+x)
                    std::memcpy(details + 0x40, &type, 4);
                    std::memcpy(details + 0x44, &minus1, 4);
                    std::memcpy(details + 0x54, &amount, 4);
                    std::memcpy(details + 0x58, &one, 4);
                    if (uint8_t *at = getMemPtr(rdram, g_damage))
                        std::memcpy(at, details, sizeof(details));
                    std::fprintf(stderr, "[ai probe] tick %llu: ship %08x hit (type 0x100, hp %.1f, fall %.2f, slot %u)%s\n",
                                 static_cast<unsigned long long>(now), ship, flt(rdram, ship + 0x28u), flt(rdram, body + 0x128u),
                                 word(rdram, ship + 0x4Cu), g_forceRoll ? " - the tumble's roll made to come up" : "");
                    SET_GPR_U32(ctx, 5, g_damage);
                    ctx->pc = kTakeDamage;
                    fn(rdram, ctx, runtime);
                    return;
                }
            }
            if (g_killAt && !g_killHit && !g_killed && body && byte(rdram, body + 0x11Fu) == 0u && word(rdram, ship + 0x4Cu) < count &&
                tf::port::testTickNow() >= g_killAt)
            {
                g_killed = true;
                const uint32_t target = g_killFall ? kStartFallToDeath : kStartDeath;
                if (PS2Runtime::RecompiledFunction fn = runtime->lookupFunction(target))
                {
                    std::fprintf(stderr, "[ai probe] tick %llu: ship %08x made to %s (%u of %u carried still aboard)\n",
                                 static_cast<unsigned long long>(tf::port::testTickNow()), ship, g_killFall ? "fall to its death" : "die",
                                 count - word(rdram, ship + 0x4Cu), count);
                    if (!g_killFall)
                        if (uint8_t *hp = getMemPtr(rdram, ship + 0x28u))
                            std::memset(hp, 0, 4);
                    SET_GPR_U32(ctx, 4, body);
                    ctx->pc = target;
                    fn(rdram, ctx, runtime);
                    return;
                }
            }
            g_dropShipUpdate(rdram, ctx, runtime);
        }

        // taCreatureDropShip::DropIt (0x3E0D30, a0 = the ship): the slot it lets go next and what its creature is then -
        // DropIt lets it go only when it is active or suspended (+0x10 & 0xC), of the kinds 0x39D0FFC00 (+0x8),
        // carried (+0xD8 bit 0x200000) and not 0x2000; else it only moves on to the next slot, the creature still carried.
        void dropIt(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ship = getRegU32(ctx, 4);
            const uint32_t slot = word(rdram, ship + 0x4Cu), count = word(rdram, ship + 0x78u), data = word(rdram, ship + 0x7Cu);
            const uint32_t entry = slot < count ? word(rdram, data + 4u * slot) : 0u;
            const uint32_t creature = entry ? word(rdram, entry + 4u) : 0u;
            if (creature)
            {
                const uint32_t s10 = word(rdram, creature + 0x10u), d8 = word(rdram, creature + 0xD8u);
                const uint64_t kinds = (static_cast<uint64_t>(word(rdram, creature + 12u)) << 32) | word(rdram, creature + 8u);
                const bool lets = (s10 & 0xCu) && (kinds & 0x39D0FFC00ull) && (d8 & 0x200000u) && !(d8 & 0x2000u);
                std::fprintf(stderr, "[ai probe] tick %llu: ship %08x drops slot %u: %s (s10 %x d8 %08x)%s\n",
                             static_cast<unsigned long long>(tf::port::testTickNow()), ship, slot, creatureText(rdram, creature).c_str(),
                             s10, d8, lets ? "" : " - NOT let go: stays carried");
            }
            else
                std::fprintf(stderr, "[ai probe] tick %llu: ship %08x drops slot %u: no creature\n",
                             static_cast<unsigned long long>(tf::port::testTickNow()), ship, slot);
            g_dropIt(rdram, ctx, runtime);
        }

        // TF_AI_PROBE_CINEMA=<on>,<off> (a test action): cinema mode on at the first tick, off at the second, as a
        // .CS script's DisableCreatures / EnableCreatures make it (taStatementSpecialEffect cases 13 / 14:
        // taCreatureSystem 0x2B0B40(1) then 0x2B09A0(1 | 0x42800002), the types that go on updating; 0x2B0B40(0)),
        // done from the creature system's update (0x2B25B0, its dt in f12) before its own work that frame.
        constexpr uint32_t kCreatureSystemUpdate = 0x002B25B0u, kSetCinemaMode = 0x002B0B40u, kSetCinemaMask = 0x002B09A0u;
        constexpr uint64_t kCinemaMask = 1u | 0x42800002u;
        uint64_t g_cinemaOn = 0u, g_cinemaOff = 0u;
        bool g_cinemaOnDone = false, g_cinemaOffDone = false;
        uint32_t g_cinemaState = 0u, g_cinemaFn = 0u, g_cinemaOriginal = 0u;

        void creatureSystemUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            enum : uint32_t
            {
                sRa = 0x0, sDt = 0x4
            };
            uint32_t pc = ctx->pc;
            if (pc == kCreatureSystemUpdate)
            {
                setWord(rdram, g_cinemaState + sRa, getRegU32(ctx, 31));
                uint32_t dt;
                std::memcpy(&dt, &ctx->f[12], 4);
                setWord(rdram, g_cinemaState + sDt, dt);
                const uint64_t now = tf::port::testTickNow();
                if (!g_cinemaOnDone && now >= g_cinemaOn)
                {
                    g_cinemaOnDone = true;
                    std::fprintf(stderr, "[ai probe] tick %llu: cinema mode on (DisableCreatures)\n", static_cast<unsigned long long>(now));
                    SET_GPR_U32(ctx, 4, 1u);
                    if (!guest_code::call(rdram, ctx, runtime, kSetCinemaMode, g_cinemaFn + 4u))
                        return;
                    pc = ctx->pc;
                }
                else if (g_cinemaOnDone && !g_cinemaOffDone && g_cinemaOff && now >= g_cinemaOff)
                {
                    g_cinemaOffDone = true;
                    std::fprintf(stderr, "[ai probe] tick %llu: cinema mode off (EnableCreatures)\n", static_cast<unsigned long long>(now));
                    SET_GPR_U32(ctx, 4, 0u);
                    if (!guest_code::call(rdram, ctx, runtime, kSetCinemaMode, g_cinemaFn + 8u))
                        return;
                    pc = ctx->pc;
                }
                else
                    pc = g_cinemaFn + 8u;
            }
            if (pc == g_cinemaFn + 4u)
            {
                SET_GPR_U64(ctx, 4, kCinemaMask);
                if (!guest_code::call(rdram, ctx, runtime, kSetCinemaMask, g_cinemaFn + 8u))
                    return;
                pc = ctx->pc;
            }
            if (pc == g_cinemaFn + 8u)
            {
                const uint32_t dt = word(rdram, g_cinemaState + sDt);
                std::memcpy(&ctx->f[12], &dt, 4);
                if (!guest_code::call(rdram, ctx, runtime, g_cinemaOriginal, g_cinemaFn + 12u))
                    return;
                pc = ctx->pc;
            }
            if (pc == g_cinemaFn + 12u)
                ctx->pc = word(rdram, g_cinemaState + sRa);
        }

        // TrueWithPercentage (",hitfall"): true once when asked to be.
        void trueWithPercentage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_forceRoll)
            {
                g_forceRoll = false;
                SET_GPR_U32(ctx, 2, 1u);
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
            g_trueWithPercentage(rdram, ctx, runtime);
        }

        // The actors' updates (vtable +0x14, with dt): a0 = the actor, its creature at +0xF4.
        void logActor(uint8_t *rdram, uint32_t actor, bool tank)
        {
            const uint32_t creature = word(rdram, actor + 0xF4u);
            if (!creature || !(tank || g_carried.count(creature) || g_all))
                return;
            std::string text = creatureText(rdram, creature);
            const size_t at = text.find(" pos ");
            std::string state = text.substr(0, at), where = text.substr(at + 1);
            char extra[200];
            if (tank)
            {
                // The turret's turn the aim stored (+0x108), its node (+0x544), the time at +0x574 and the
                // flag the aim and the bursts wait for (creature +0xD8 bit 0x200).
                // The turret's rotation: the object at +0x544 keeps it as a quaternion at +0x40 (the aim 0x401BD0 reads
                // and writes it there).
                const uint32_t turret = word(rdram, actor + 0x544u);
                std::snprintf(extra, sizeof(extra), " actor %08x yaw %.3f t574 %.2f d8 %08x turret q(%.3f %.3f %.3f %.3f)", actor,
                              flt(rdram, actor + 0x108u), flt(rdram, actor + 0x574u), word(rdram, creature + 0xD8u),
                              flt(rdram, turret + 0x40u), flt(rdram, turret + 0x44u), flt(rdram, turret + 0x48u), flt(rdram, turret + 0x4Cu));
            }
            else
                std::snprintf(extra, sizeof(extra), " actor %08x d8 %08x", actor, word(rdram, creature + 0xD8u));
            where += extra;
            report(creature, state, where, tank ? "spider" : g_carried.count(creature) ? "carried" : "creature");
        }

        void gruntActorUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            logActor(rdram, getRegU32(ctx, 4), false);
            g_gruntActorUpdate(rdram, ctx, runtime);
        }

        // The aim (0x401BD0): the pitch's spring at +0x100 (its speed and acceleration limits, then the position - the
        // pitch, which the aim stores at +0x108 first - the speed and the acceleration) before and after, and dt.
        std::string springText(const uint8_t *rdram, uint32_t actor)
        {
            char text[160];
            const uint32_t turret = word(rdram, actor + 0x544u);
            std::snprintf(text, sizeof(text), "lim %.3f/%.2f pos %.4f vel %.4f acc %.2f q(%.3f %.3f %.3f %.3f)", flt(rdram, actor + 0x100u),
                          flt(rdram, actor + 0x104u), flt(rdram, actor + 0x108u), flt(rdram, actor + 0x10Cu), flt(rdram, actor + 0x110u),
                          flt(rdram, turret + 0x40u), flt(rdram, turret + 0x44u), flt(rdram, turret + 0x48u), flt(rdram, turret + 0x4Cu));
            return text;
        }

        void spiderAim(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t actor = getRegU32(ctx, 4), ra = getRegU32(ctx, 31);
            const float dt = ctx->f[12];
            const uint64_t tick = tf::port::testTickNow();
            uint64_t &seen = g_aimSeen[actor];
            const bool log = tick >= seen + g_every;
            const std::string before = log ? springText(rdram, actor) : std::string();
            g_spiderAim(rdram, ctx, runtime);
            if (log && ctx->pc == ra)
            {
                seen = tick;
                std::fprintf(stderr, "[ai probe] tick %llu aim %08x dt %.5f before %s after %s\n", static_cast<unsigned long long>(tick), actor,
                             dt, before.c_str(), springText(rdram, actor).c_str());
            }
        }

        void spiderActorUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            logActor(rdram, getRegU32(ctx, 4), true);
            g_spiderActorUpdate(rdram, ctx, runtime);
        }
    }

    void install(PS2Runtime &runtime)
    {
        // Added at every start, whatever the settings (guest_code.h: the added code's addresses must be the same
        // in every session, and installers after this one add theirs too).
        g_damage = guest_code::addState("ai probe: damage details", 0x60u);
        g_cinemaState = guest_code::addState("ai probe: cinema mode", 0x10u);
        g_cinemaFn = guest_code::addResumable(runtime, "taCreatureSystem::Update (+ TF_AI_PROBE_CINEMA)", creatureSystemUpdate, 4);
        const PS2Runtime::RecompiledFunction systemUpdate = runtime.lookupFunction(kCreatureSystemUpdate);
        if (systemUpdate)
            g_cinemaOriginal = guest_code::addFunction(runtime, "taCreatureSystem::Update (original)", systemUpdate);
        const char *env = std::getenv("TF_AI_PROBE");
        if (!env || !*env || *env == '0')
            return;
        if (const char *cinema = std::getenv("TF_AI_PROBE_CINEMA"))
        {
            char *end = nullptr;
            g_cinemaOn = std::strtoull(cinema, &end, 10);
            g_cinemaOff = end && *end == ',' ? std::strtoull(end + 1, nullptr, 10) : 0u;
            if (systemUpdate && g_cinemaOn)
                runtime.registerFunction(kCreatureSystemUpdate, creatureSystemUpdate);
        }
        g_every = std::strtoull(env, nullptr, 10);
        if (g_every == 0u)
            g_every = 15u;
        g_all = std::getenv("TF_AI_PROBE_ALL") != nullptr;
        if (const char *kill = std::getenv("TF_AI_PROBE_KILL"))
        {
            g_killAt = std::strtoull(kill, nullptr, 10);
            g_killFall = std::strstr(kill, "fall") != nullptr;
            g_killHit = std::strstr(kill, "hit") != nullptr;
            g_hitFall = std::strstr(kill, "hitfall") != nullptr;
            g_killFall = g_killFall && !g_hitFall;
            if (g_hitFall && (g_trueWithPercentage = runtime.lookupFunction(kTrueWithPercentage)) != nullptr)
                runtime.registerFunction(kTrueWithPercentage, trueWithPercentage);
        }
        if ((g_dropShipUpdate = runtime.lookupFunction(kDropShipUpdate)) != nullptr)
            runtime.registerFunction(kDropShipUpdate, dropShipUpdate);
        if ((g_dropIt = runtime.lookupFunction(kDropIt)) != nullptr)
            runtime.registerFunction(kDropIt, dropIt);
        if ((g_gruntActorUpdate = runtime.lookupFunction(kGruntActorUpdate)) != nullptr)
            runtime.registerFunction(kGruntActorUpdate, gruntActorUpdate);
        if ((g_spiderAim = runtime.lookupFunction(kSpiderAim)) != nullptr)
            runtime.registerFunction(kSpiderAim, spiderAim);
        if ((g_spiderActorUpdate = runtime.lookupFunction(kSpiderActorUpdate)) != nullptr)
            runtime.registerFunction(kSpiderActorUpdate, spiderActorUpdate);
        std::fprintf(stderr, "[ai probe] every %llu ticks\n", static_cast<unsigned long long>(g_every));
    }
}
