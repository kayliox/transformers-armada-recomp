// Antarctica's music on a revisit (docs/TODO.md item 83), and the music-step triggers landing on their own track
// (TODO 305 (12), (13)): original PS2 bugs, fixed here.
//
// The bug (the console's own, confirmed by the user on PCSX2): Antarctica's music list by warp gate is
// `0 1 1 1` (METAGAME.TDB WarpGateMusicTrack) - from the first gate the level opens its music file 0,
// silence.ms, and the music comes in at trigger 201 on the cliff's edge (CREATURE.TDB/.BDB:
// TriggerEvent HQ_ToPlane, the "to the plane" voice, + TriggerEvent2 NextMusicTrack, a 50-unit sphere
// at 336.9,1157.5,-16). Trigger 201 is a story object (GenerationSpawnType 0, "Narrative"), and when a
// level is played again after it is finished the game leaves its story objects out
// (taCreatureSystem::StripEnemiesFromDatabase 0x2B0FA0 drops every record its spawn filter 0x2B1450
// rejects: Narrative when the episode's record has the "completed" bit, LevelDone when it has not).
// So a finished Antarctica entered from its first gate stays silent to the end (only the wind); from
// gates 1-3 it opens anta.ms at once.
//
// The fix: on a finished Antarctica only, trigger 201 is kept as a music-only trigger - same centre,
// radius and shape, NextMusicTrack and nothing else. The spawn filter is answered "keep" for that one
// record (CreatureTrigger, object id 201, episode 1 = Antarctica, the level done); its TriggerLink (85,
// the welcome voice's trigger, itself a story object and gone) is cleared by the game's own pass that
// unlinks removed records; its first event (the HQ_ToPlane voice, trigger +0xA0) is taken out before
// the trigger first waits for the player (WaitingForPlayerToEnter 0x2F9EF0: +0xA0 = 0, which the game
// treats as "no event"), so only the second, NextMusicTrack (+0xA4), fires. A first visit is not
// touched (the filter keeps 201 there itself), nor any other level or record. The record is the same
// with Mods on (the JetBike mod's CREATURE.BDB keeps trigger 201 as on the disc) and off.
//
// Also, every level load writes one line saying whether the level is finished (its story objects then
// left out) - the first thing to look at when a level's script seems not to run:
//   [music] level start (episode 1): level done - its story objects are not placed
//
// TF_REVISIT_MUSIC=0: the PS2's behaviour (a finished Antarctica silent from its first gate).
//
// The step triggers (TODO 305 (12), (13)). A level's music is a list (MUSICSTYLE.BT), the warp gate picks where it
// starts (METAGAME.TDB WarpGateMusicTrack), and a NextMusicTrack event moves it one entry on
// (the event's listener, taMusicManager 0x2E64C0: CrossfadeToTrack 0x2E6980(this, current + 1); the current entry at
// +0x10C, one being crossfaded to at +0x114 while +0x110 is set). The level's triggers that fire it are placed for one way
// through the level, so a trigger reached out of order steps from the wrong entry:
//   Deep Amazon (amazon2.ms, temple.ms, cyclonus.alb_t; gates 0 0 0 1 0 0): trigger 155 at the temple steps to
//     temple.ms, trigger 228 (the Cyclonus fight, with FMV_3_1) to cyclonus.alb_t. Skip the temple and Cyclonus's
//     trigger steps amazon2 -> temple.ms: the temple music for the boss (13, the PS2 does it too). From gate 3 (on
//     temple.ms already) the temple's trigger would step to Cyclonus's music.
//   Antarctica (silence.ms, anta.ms, starscream.alb_t; gates 0 1 1 1): trigger 201 at the cliff steps to anta.ms,
//     688 (the Starscream fight, with CS_StarscreamIntro) to his theme. On a first visit 201 is crossed from gate 0
//     only, before the boss (it fires once: RepeatCount, CREATURE.TMD default 1, then it is killed). Kept on a
//     revisit (above), it is still waiting when you come from gates 1-3 on anta.ms, so crossing it - walking back from
//     the Research Center after the boss too - stepped anta.ms -> Starscream's theme (12).
// So each of these triggers moves the music to ITS entry: while the trigger's enter events fire
// (DelayBeforeTriggerEnterEvents 0x2F9CB0; taEvent::Fire calls the listeners at once), the next-track listener
// crossfades to the trigger's entry if the music is before it, and does nothing if the music is already on it or past:
//   [music] step trigger at (328.0, 1395.5, 288.0), Deep Amazon's Cyclonus fight: entry 0 -> 2 (the PS2: 1)
//   [music] step trigger at (336.9, 1157.5, -16.0), Antarctica's cliff: the music already on entry 1 - no step (the PS2: 2)
// Played in the levels' own order this is the PS2's single step. The other levels' step triggers (Amazon's 29,
// Alaska's 244, Easter Island's 145: a two-entry list and one trigger) and 06/08's cutscene steps can't land wrong.
// TF_MUSIC_STEPS=0: the PS2's one-entry steps.
// Decomp equivalent: the spawn filter keeping CreatureTrigger 201 on a finished Antarctica, its
// TriggerEvent left empty.

#include "revisit_music.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_memory.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "game_version.h" // (TF_GA: the code addresses that differ in the PAL release)

namespace revisit_music
{
    namespace
    {
        constexpr uint32_t kStrip = 0x002B0FA0u;   // taCreatureSystem::StripEnemiesFromDatabase(db, mask)
        constexpr uint32_t kFilter = 0x002B1450u;  // the spawn filter (record): 1 = keep
        constexpr uint32_t kWaiting = 0x002F9EF0u; // taCreatureTrigger::WaitingForPlayerToEnter(this)
        constexpr uint32_t kEpisodeNow = 0x00716EF0u;   // the current episode (0x34F2D0's owner, word 0: 0x350D30)
        constexpr uint32_t kProgress = 0x007170E0u;     // 0x34F2C0: the progress record
        constexpr uint32_t kEpisodeFlags = 0x34u, kEpisodeStride = 0x4Cu; // 0x351AA0: bit 1 = completed
        constexpr uint32_t kEpisodes = 9u;
        constexpr uint32_t kAntarctica = 1u;
        constexpr uint16_t kCliffTrigger = 201u;
        constexpr float kCliff[3] = {336.888f, 1157.54f, -16.0233f};

        PS2Runtime::RecompiledFunction g_strip = nullptr, g_filter = nullptr, g_waiting = nullptr;
        // (TF_EVENT_TRACE=1, a test aid - TODO 199: every named game event as it fires, taEvent::Fire 0x2B5C50,
        // with its caller: which cutscenes and level events a stretch of play set off)
        constexpr uint32_t kFire = 0x002B5C50u;
        PS2Runtime::RecompiledFunction g_fire = nullptr;
        // (with it, TODO 199: Starscream's hits and his slot 20, which marks him dead and fires CS_DefeatedStarscream)
        constexpr uint32_t kTakeDamage = 0x00297C70u, kStarscreamDead = TF_GA(0x0048A1A0u), kStarscreamVtable = 0x006C07F0u;
        PS2Runtime::RecompiledFunction g_takeDamage = nullptr, g_starscreamDead = nullptr;
        // (TF_HIT_STARSCREAM=<damage>, with TF_EVENT_TRACE: when his intro ends (TA_AUTOSAVE) Starscream takes one hit of
        // that much through his own hit slot 7 (0x48A880), as a player's shot lands - the one-hit-kills test without aiming)
        constexpr uint32_t kStarscreamHit = TF_GA(0x0048A880u);
        PS2Runtime::RecompiledFunction g_starscreamHit = nullptr;
        uint32_t g_starscream = 0u;
        float g_hitAmount = 0.0f;
        uint64_t g_hitDelay = 0u, g_autosaveTick = 0u; // (amount@ticks: the hit that many VBlanks after the intro's end)
        constexpr uint32_t kStarscreamUpdate = TF_GA(0x0048D7D0u);
        PS2Runtime::RecompiledFunction g_starscreamUpdate = nullptr;
        uint32_t g_lastFlags = 0u; float g_lastHealth = -2.0f; int g_flagLines = 0;
        bool g_on = true;
        bool g_fixLevel = false; // this level: a finished Antarctica, the fix on
        bool g_trim = false;     // trigger 201 kept: its voice event still to be taken out

        // The step triggers and the music entry each is placed to start (TODO 305 (12), (13)), by episode and centre.
        constexpr uint32_t kTriggerEnter = 0x002F9CB0u; // taCreatureTrigger::DelayBeforeTriggerEnterEvents(this, dt)
        constexpr uint32_t kNextTrack = 0x002E64C0u;    // taMusicManager's NextMusicTrack listener (this)
        constexpr uint32_t kCrossfade = 0x002E6980u;    // taMusicManager::CrossfadeToTrack(this, index)
        struct StepTrigger
        {
            uint32_t episode;
            float centre[3];
            int32_t entry;
            const char *what;
        };
        constexpr StepTrigger kSteps[] = {
            {1u, {336.888f, 1157.54f, -16.0233f}, 1, "Antarctica's cliff"},            // 201 -> anta.ms
            {1u, {-126.233f, 3067.3f, -20.9207f}, 2, "Antarctica's Starscream fight"}, // 688 -> starscream.alb_t
            {2u, {328.728f, 1347.15f, 229.15f}, 1, "Deep Amazon's temple"},            // 155 -> temple.ms
            {2u, {327.993f, 1395.5f, 288.0f}, 2, "Deep Amazon's Cyclonus fight"},      // 228 -> cyclonus.alb_t
        };
        PS2Runtime::RecompiledFunction g_triggerEnter = nullptr, g_nextTrack = nullptr, g_crossfade = nullptr;
        bool g_stepsOn = true;
        const StepTrigger *g_firing = nullptr; // the step trigger whose enter events are firing

        uint32_t word(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v = 0u;
            if (a < 0x00100000u || a >= PS2_RAM_SIZE || (a & 3u))
                return 0u;
            if (const uint8_t *p = getConstMemPtr(rdram, a))
                std::memcpy(&v, p, sizeof(v));
            return v;
        }
        float fword(const uint8_t *rdram, uint32_t a)
        {
            const uint32_t v = word(rdram, a);
            float f = 0.0f;
            std::memcpy(&f, &v, sizeof(f));
            return f;
        }
        std::string cString(const uint8_t *rdram, uint32_t a)
        {
            std::string s;
            for (uint32_t i = 0; a && i < 64u; ++i)
            {
                const uint8_t *c = getConstMemPtr(rdram, a + i);
                if (!c || *c < 0x20 || *c > 0x7E)
                    break;
                s.push_back(static_cast<char>(*c));
            }
            return s;
        }
        // A CodeWarrior std::string: word 0 = capacity << 1 | 1 for the long form (+4 size, +8 the
        // characters' address), else byte 0 = size << 1 and the characters from +1.
        std::string stdString(const uint8_t *rdram, uint32_t a)
        {
            const uint32_t w0 = word(rdram, a);
            if (w0 & 1u)
            {
                std::string s = cString(rdram, word(rdram, a + 8u));
                const uint32_t size = word(rdram, a + 4u);
                return size < s.size() ? s.substr(0, size) : s;
            }
            std::string s = cString(rdram, a + 1u);
            const uint32_t size = (w0 & 0xFFu) >> 1;
            return size < s.size() ? s.substr(0, size) : s;
        }

        // A database node's name: +4 points to a std::string (0x100320; 0 = "").
        std::string nodeName(const uint8_t *rdram, uint32_t node)
        {
            const uint32_t name = node ? word(rdram, node + 4u) : 0u;
            return name ? stdString(rdram, name) : std::string();
        }

        void strip(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kStrip)
            {
                const uint32_t episode = word(rdram, kEpisodeNow);
                const bool done = episode < kEpisodes && (word(rdram, kProgress + kEpisodeFlags + episode * kEpisodeStride) & 2u);
                g_fixLevel = g_on && done && episode == kAntarctica;
                g_trim = false;
                std::fprintf(stderr, "[music] level start (episode %u): %s\n", episode,
                             done ? "level done - its story objects are not placed" : "not done - its story objects are placed");
            }
            g_strip(rdram, ctx, runtime);
        }

        void filter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kFilter && g_fixLevel)
            {
                const uint32_t record = getRegU32(ctx, 4);
                const uint32_t id = word(rdram, record + 8u) & 0xFFFFu; // (0x175670: the record's id at +8, a u16)
                // Its type: the name (+4, a pointer to a std::string) of the record or of a node up its parent chain (+0), as the
                // game's own "is a" test (0x174CC0) walks it.
                bool trigger = false;
                std::string names;
                for (uint32_t node = record, depth = 0; id == kCliffTrigger && node && depth < 6u && !trigger;
                     node = word(rdram, node), ++depth)
                {
                    const std::string name = nodeName(rdram, node);
                    names += (names.empty() ? "" : " < ") + name;
                    trigger = name == "CreatureTrigger";
                }
                if (id == kCliffTrigger && !trigger)
                    std::fprintf(stderr, "[music] revisit: record 201 is not a trigger ('%s') - left out as the game does\n", names.c_str());
                if (trigger)
                {
                    g_trim = true;
                    std::fprintf(stderr, "[music] revisit: Antarctica's cliff trigger 201 kept, music only (TF_REVISIT_MUSIC=0: the PS2's silence)\n");
                    SET_GPR_U32(ctx, 2, 1u);
                    ctx->pc = getRegU32(ctx, 31);
                    return;
                }
            }
            g_filter(rdram, ctx, runtime);
        }

        void waiting(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kWaiting && g_trim)
            {
                const uint32_t self = getRegU32(ctx, 4);
                bool here = std::fabs(fword(rdram, self + 0x74u) - 2500.0f) < 1.0f;
                for (int i = 0; i < 3; ++i)
                    here = here && std::fabs(fword(rdram, self + 0x40u + 4u * static_cast<uint32_t>(i)) - kCliff[i]) < 0.5f;
                const uint32_t voice = word(rdram, self + 0xA0u), music = word(rdram, self + 0xA4u);
                if (here && voice && music)
                {
                    const std::string first = stdString(rdram, voice + 4u), second = stdString(rdram, music + 4u);
                    g_trim = false;
                    if (first == "HQ_ToPlane" && second == "NextMusicTrack")
                    {
                        if (uint8_t *p = getMemPtr(rdram, self + 0xA0u))
                            std::memset(p, 0, 4u);
                        std::fprintf(stderr, "[music] revisit: trigger %08x at the cliff: its %s voice taken out, %s kept\n", self,
                                     first.c_str(), second.c_str());
                    }
                    else
                        std::fprintf(stderr, "[music] revisit: trigger %08x at the cliff has events '%s' / '%s' - left as it is\n", self,
                                     first.c_str(), second.c_str());
                }
            }
            g_waiting(rdram, ctx, runtime);
        }

        void triggerEnter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const StepTrigger *step = nullptr;
            if (ctx->pc == kTriggerEnter && g_stepsOn)
            {
                const uint32_t self = getRegU32(ctx, 4), episode = word(rdram, kEpisodeNow);
                for (const StepTrigger &t : kSteps)
                {
                    bool here = t.episode == episode;
                    for (int i = 0; i < 3 && here; ++i)
                        here = std::fabs(fword(rdram, self + 0x40u + 4u * static_cast<uint32_t>(i)) - t.centre[i]) < 0.5f;
                    if (here)
                        step = &t;
                }
            }
            const StepTrigger *outer = g_firing;
            g_firing = step;
            g_triggerEnter(rdram, ctx, runtime);
            g_firing = outer;
        }

        // NextMusicTrack's listener (a tail call to CrossfadeToTrack with this + 0x10C plus one) while a step trigger fires.
        void nextTrack(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kNextTrack && g_firing && g_crossfade)
            {
                const uint32_t music = getRegU32(ctx, 4);
                const int32_t current = static_cast<int32_t>(word(rdram, music + 0x10Cu)), asked = current + 1;
                const uint8_t *pending = getConstMemPtr(rdram, music + 0x110u);
                const int32_t now = (pending && *pending) ? static_cast<int32_t>(word(rdram, music + 0x114u)) : current;
                const StepTrigger &t = *g_firing;
                g_firing = nullptr; // (once)
                if (now >= t.entry)
                {
                    std::fprintf(stderr, "[music] step trigger at (%.1f, %.1f, %.1f), %s: the music already on entry %d - no step (the PS2: %d)\n",
                                 t.centre[0], t.centre[1], t.centre[2], t.what, now, asked);
                    ctx->pc = getRegU32(ctx, 31);
                    return;
                }
                std::fprintf(stderr, "[music] step trigger at (%.1f, %.1f, %.1f), %s: entry %d -> %d%s\n", t.centre[0], t.centre[1],
                             t.centre[2], t.what, now, t.entry, asked == t.entry ? " (as the PS2)" : asked == t.entry - 1 ? " (the PS2: one short)" : "");
                SET_GPR_U32(ctx, 5, static_cast<uint32_t>(t.entry));
                ctx->pc = kCrossfade;
                g_crossfade(rdram, ctx, runtime);
                return;
            }
            g_nextTrack(rdram, ctx, runtime);
        }

        float health(const uint8_t *rdram, uint32_t actor)
        {
            const uint32_t creature = word(rdram, actor + 8u);
            return creature ? fword(rdram, creature + 0x28u) : -1.0f;
        }

        // One hit on Starscream through his hit slot 7: the damage details on the guest's stack below its sp
        // (taDamageDetails' defaults: direction +x, kind 1, type -1, the amount at +0x54, scale 1 at +0x58), the
        // registers put back after.
        void testHit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_hitAmount <= 0.0f || !g_starscream || !g_starscreamHit)
                return;
            const R5900Context saved = *ctx;
            const uint32_t details = (getRegU32(ctx, 29) - 0x200u) & ~0xFu;
            if (uint8_t *d = getMemPtr(rdram, details))
            {
                std::memset(d, 0, 0x60u);
                const float one = 1.0f, amount = g_hitAmount;
                const uint32_t kind = 1u, type = 0xFFFFFFFFu;
                std::memcpy(d + 0x20u, &one, 4u);
                std::memcpy(d + 0x40u, &kind, 4u);
                std::memcpy(d + 0x44u, &type, 4u);
                std::memcpy(d + 0x54u, &amount, 4u);
                std::memcpy(d + 0x58u, &one, 4u);
                SET_GPR_U32(ctx, 29, details - 0x80u);
                SET_GPR_U32(ctx, 4, g_starscream);
                SET_GPR_U32(ctx, 5, details);
                const uint32_t creature = word(rdram, g_starscream + 8u);
                std::fprintf(stderr, "[event] Starscream (actor %08x, creature %08x, flags %08x) given a test hit of %.1f\n", g_starscream, creature, creature ? word(rdram, creature + 0xD8u) : 0u, amount);
                g_starscreamHit(rdram, ctx, runtime);
            }
            *ctx = saved;
            g_hitAmount = 0.0f;
        }

        void fire(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kFire)
            {
                const uint32_t event = getRegU32(ctx, 4);
                std::fprintf(stderr, "[event] tick %llu: %s fired (from %08x)\n",
                             static_cast<unsigned long long>(runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed)),
                             stdString(rdram, event + 4u).c_str(), getRegU32(ctx, 31));
            }
            const bool autosave = ctx->pc == kFire && g_starscream && stdString(rdram, getRegU32(ctx, 4) + 4u) == "TA_AUTOSAVE";
            g_fire(rdram, ctx, runtime);
            if (autosave)
            {
                g_autosaveTick = runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed);
                if (g_hitDelay == 0u)
                    testHit(rdram, ctx, runtime);
            }
        }

        void takeDamage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t actor = getRegU32(ctx, 4), details = getRegU32(ctx, 5);
            const bool his = ctx->pc == kTakeDamage && word(rdram, actor) == kStarscreamVtable;
            const float before = his ? health(rdram, actor) : 0.0f;
            const uint64_t tick = runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed);
            const uint32_t ra = getRegU32(ctx, 31);
            g_takeDamage(rdram, ctx, runtime);
            if (his)
                std::fprintf(stderr, "[event] tick %llu: Starscream hit: %.1f damage, kind %u type %d, health %.1f -> %.1f (from %08x)\n",
                             static_cast<unsigned long long>(tick), fword(rdram, details + 0x54u), word(rdram, details + 0x40u),
                             static_cast<int>(word(rdram, details + 0x44u)), before, health(rdram, actor), ra);
        }

        void starscreamDead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kStarscreamDead)
            {
                const uint32_t actor = getRegU32(ctx, 4);
                g_starscream = actor;
                std::fprintf(stderr, "[event] tick %llu: Starscream slot 20 (%u), marked %u, health %.1f (from %08x)\n",
                             static_cast<unsigned long long>(runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed)),
                             getRegU32(ctx, 5) & 0xFFu, rdram[actor + 0x10u], health(rdram, actor), getRegU32(ctx, 31));
            }
            g_starscreamDead(rdram, ctx, runtime);
        }

        // Starscream's frame (Update 0x48D7D0): his creature's state flags (+0xD8; 0x800 robot) and health when they
        // change, and the delayed test hit
        void starscreamUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kStarscreamUpdate)
            {
                const uint32_t actor = getRegU32(ctx, 4), creature = word(rdram, actor + 8u);
                const uint64_t tick = runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed);
                const uint32_t flags = creature ? word(rdram, creature + 0xD8u) : 0u;
                const float hp = health(rdram, actor);
                if ((flags != g_lastFlags || hp != g_lastHealth) && g_flagLines < 300)
                {
                    ++g_flagLines;
                    std::fprintf(stderr, "[event] tick %llu: Starscream flags %08x health %.1f marked %u\n", static_cast<unsigned long long>(tick), flags, hp, rdram[actor + 0x10u]);
                    g_lastFlags = flags, g_lastHealth = hp;
                }
                if (g_hitDelay && g_autosaveTick && tick >= g_autosaveTick + g_hitDelay)
                {
                    g_hitDelay = 0u;
                    g_starscream = actor;
                    testHit(rdram, ctx, runtime);
                }
            }
            g_starscreamUpdate(rdram, ctx, runtime);
        }

        void hook(PS2Runtime &runtime, uint32_t address, PS2Runtime::RecompiledFunction &original, PS2Runtime::RecompiledFunction fn)
        {
            if ((original = runtime.lookupFunction(address)) != nullptr)
                runtime.registerFunction(address, fn);
            else
                std::fprintf(stderr, "[music] revisit fix: nothing at %08x to hook\n", address);
        }
    }

    void install(PS2Runtime &runtime)
    {
        const char *v = std::getenv("TF_REVISIT_MUSIC");
        g_on = !(v && *v == '0');
        hook(runtime, kStrip, g_strip, strip);
        hook(runtime, kFilter, g_filter, filter);
        hook(runtime, kWaiting, g_waiting, waiting);
        const char *steps = std::getenv("TF_MUSIC_STEPS");
        g_stepsOn = !(steps && *steps == '0');
        hook(runtime, kTriggerEnter, g_triggerEnter, triggerEnter);
        g_crossfade = runtime.lookupFunction(kCrossfade);
        hook(runtime, kNextTrack, g_nextTrack, nextTrack);
        if (const char *e = std::getenv("TF_EVENT_TRACE"); e && *e == '1')
        {
            hook(runtime, kFire, g_fire, fire);
            hook(runtime, kTakeDamage, g_takeDamage, takeDamage);
            hook(runtime, kStarscreamDead, g_starscreamDead, starscreamDead);
            g_starscreamHit = runtime.lookupFunction(kStarscreamHit);
            hook(runtime, kStarscreamUpdate, g_starscreamUpdate, starscreamUpdate);
            if (const char *h = std::getenv("TF_HIT_STARSCREAM"))
            {
                g_hitAmount = static_cast<float>(std::atof(h));
                if (const char *at = std::strchr(h, '@'))
                    g_hitDelay = std::strtoull(at + 1, nullptr, 10);
            }
        }
    }
}
