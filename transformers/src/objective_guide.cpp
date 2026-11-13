// (TODO 315) The objective guide (Gameplay > Objective guide, TF_OBJECTIVE_GUIDE = 0 / 1, off by default, live).
//
// Where the chain comes from: the level's own triggers (taCreatureTrigger, CREATURE.TDB's CreatureTrigger records),
// live. Each frame every trigger the level's script has ARMED waits for the player through
// taCreatureTrigger::WaitingForPlayerToEnter (0x2F9EF0, the same address in the European release: below its first
// shift); a fired trigger is killed and the triggers its events arm start waiting in turn. So the triggers seen
// waiting this frame are exactly where the level's own chain stands - in every episode, with no list of places.
// Each one: its centre (+0x40, world, z up), its radius (+0x74, squared), its shape (+0x90), its enter events
// (+0xA0, +0xA4: taEvent, the name a std::string at +4; 0 = none).
//
// The pick: among the armed triggers, those with an objective event (by name, kObjectivePrefixes below: the radio's
// guidance, a cutscene or a movie; the music steps and the level's own machinery are skipped). A level arms many of its
// triggers from the start (Amazon: 15 at once, the whole route), so the ORDER comes from the level's own links: a
// trigger's TriggerLink (+0x80) is the beat before it, which it kills when it fires. The links make the level's story
// chain (Amazon: robot training, combat training, the dropship, the ruins, the waterfall, the road, Machu Picchu), each
// beat at its depth (hops back to the first). Where the player stands in it: a warp-in at a later gate leaves the
// earlier beats armed too, so at the level's start the chain beat nearest the player is next (each warp gate stands by
// the beat that follows it - checked against CREATURE.TDB's WarpGateNumber for every gate of the nine maps, gate 0 by
// each level's first beat); after that, every chain beat that stops waiting has been passed (fired, or killed by the
// one after it). The target: the earliest beat after the player's place, the nearest of those at one depth.
// Objectives outside the chain (the cutscenes, Amazon's ravine voices) take the arrow when they are clearly nearer and
// close (kSoloNearer, kSoloReach) - the ravine lies on the way from the road to Machu Picchu. No chain at all: the
// nearest objective. No story beat left: the level's boss while it lives ("Defeat Unicron":
// Cybertron has no triggers; its player is where the camera stands), else nothing.
//
// What it draws (the window thread, raylib, like the distance meter): at the bottom centre an arrow that turns with
// the game's camera (the direction to the target against the camera's ahead and left axes, photo_mode::viewFrame),
// the target's name (its event's, made readable: kLabels for the ones that read badly) and its distance in metres
// (3.06 game units a metre, to the trigger's edge). Not in the HQ, the menus, a movie or photo mode, nor while the
// game's HUD is off (its cutscenes).
//
// Cost: the hook copies 28 bytes per armed trigger into a fixed table (no allocation, no string); the names are read
// once per event (a fixed cache), the pick once a frame. TF_GUIDE_TRACE=1 (tests): every trigger seen once per level
// (its events, centre, radius) and every change of the pick, to the log ("[guide] ...").

#include "objective_guide.h"

#include "photo_mode.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "raylib.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_settings.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace objective_guide
{
    namespace
    {
        constexpr uint32_t kWaiting = 0x002F9EF0u; // taCreatureTrigger::WaitingForPlayerToEnter(this)
        // Where the player is, as the triggers see it: WaitingForPlayerToEnter asks 0x2A4050(out, centre) for the nearest
        // player's position (x y z, z up) - in every level, also where the camera's target is not known (Antarctica,
        // the carrier, the spaceship: playerPositionAddress gives 0 there). Its call from the trigger returns to 0x2F9FC0.
        constexpr uint32_t kNearestPlayer = 0x002A4050u, kNearestPlayerFromTrigger = 0x002F9FC0u;
        // A trigger's TriggerLink (+0x80: the trigger it kills when it is killed itself, 0x2FA070) is the beat BEFORE it
        // (Antarctica's cliff, 201, links the welcome voice, 85): the order the level's own data gives.
        constexpr uint32_t kLink = 0x80u;
        // The boss: every creature sets its actor through taCreatureBase slot 49 (0x2A64B0, creature +0x24 = actor; 0 when
        // it lets it go - its death); one whose class, or its actor's, is a boss (the type_info's name has "Boss":
        // taCreatureBossUnicron, taActorBossStarscream ...) is the level's boss. Its place: the actor's scene node (+0xC),
        // its transform (+0x14), the translation (+0x30).
        constexpr uint32_t kSetActor = 0x002A64B0u;
        constexpr float kUnitsPerMetre = 3.06f;     // (the robot 5.5 units tall: MODS.md)
        constexpr int kMaxArmed = 96;
        constexpr int kName = 48;
        // An objective outside the chain takes the arrow when nearer than this share of the chain's next beat, and within
        // this reach (Deep Amazon's closing movie, on top of the temple, would otherwise beat its third temple).
        constexpr float kSoloNearer = 0.75f, kSoloReach = 150.0f * kUnitsPerMetre;
        constexpr uint64_t kGoneFrames = 60; // (a chain beat not waiting for this many frames while others are: passed)

        // The events, by name (TODO 315: every CreatureTrigger event in the nine maps' CREATURE.TDB - 119 triggers, 40
        // objective names - matching the TF_GUIDE_TRACE runs). An objective is a story beat the player is sent to: the
        // radio's guidance (HQ_ToWaterfall, HQ_TheLake ...), a cutscene (CS_DropshipComing ...) or a movie (FMV_3_1). The
        // music steps are music. Everything else a trigger fires (55 names) is the level's own machinery, not a place to
        // go: enemy waves and ambushes (BunkerA_1_Activate, DropShipAmbush, HorseshoeReinforce, LeftWingGroup ...), lifts
        // and doors (lift3_0_level1, door_0_openx ...), and the developers' leftovers (bypass, nancy, KillMe, DEMO_END,
        // Trigger437isalsohere). A trigger with no event is skipped too.
        const char *const kObjectivePrefixes[] = {"HQ_", "CS_", "FMV_"};
        const char *const kMusic[] = {"NextMusicTrack"};
        // What the guide says for each objective the nine maps have (every CreatureTrigger event in their CREATURE.TDB,
        // matching the TF_GUIDE_TRACE runs): the place, not the event's name. Any other name is made readable by rule (readable below); a movie is a story scene.
        const std::pair<const char *, const char *> kLabels[] = {
            // Amazon
            {"HQ_RobotTraining", "Robot training"}, {"HQ_CombatTraining", "Combat training"}, {"HQ_ToDropship", "The dropship"},
            {"CS_DropshipComing", "The dropship"}, {"HQ_ToRuins", "The ruins"}, {"HQ_ToWaterfall", "The waterfall"},
            {"HQ_FollowRoad", "The road"}, {"HQ_DownRavine", "The ravine"}, {"HQ_DownRavine2", "The ravine"},
            {"HQ_ToMachuPicchu", "Machu Picchu"}, {"CS_HeavyGruntIntro", "The Heavy Grunt"},
            // Antarctica
            {"HQ_WelcomeAnt", "Antarctica"}, {"HQ_ToPlane", "The plane"}, {"HQ_ToResearch", "The research station"},
            {"HQ_BattleResearch", "The research station"},
            {"CS_StarscreamIntro", "Starscream"},
            // Deep Amazon
            {"HQ_ToThirdTemple", "The third temple"}, {"HQ_StrangeSignal", "The strange signal"}, {"HQ_UpUpUp", "Climb up"},
            // Mid-Atlantic (the carrier, then the beach)
            {"CS_IntroBossTidal", "Tidal Wave"}, {"HQ_GoingDown2", "Go below"}, {"HQ_TWAdvice", "Tidal Wave"},
            // Alaska
            {"CS_ItsATrap", "The trap"}, {"HQ_TheLake", "The lake"}, {"HQ_CarJumping", "The jump"}, {"HQ_LookUp", "Look up"},
            {"HQ_TheIceCliff", "The ice cliff"},
            // Starship
            {"CS_door_x_open", "The door"}, {"CS_LiftX", "The lift"}, {"HQ_ToCockpit", "The cockpit"},
            {"HQ_LookingStarscream", "Starscream"}, {"HQ_ClimbOut", "Climb out"}, {"CS_IntroBossStarscream2", "Starscream"},
            // Pacific Island
            {"HQ_IslandFortress", "The island fortress"}, {"HQ_GlideAround", "Glide around"}, {"HQ_WhalingStation", "The whaling station"},
            {"HQ_MountainPass", "The mountain pass"}, {"HQ_FinalBattle", "The final battle"}, {"CS_MegatronIntro", "Megatron"},
        };

        std::atomic<bool> g_on{false};
        bool g_trace = false;
        PS2Runtime::RecompiledFunction g_waiting = nullptr, g_nearestPlayer = nullptr, g_setActor = nullptr;
        uint32_t g_boss = 0u; // (the boss creature, 0: none known)
        char g_bossName[kName] = {};
        float g_player[3] = {};
        uint64_t g_playerTick = 0u; // (the frame it was last seen in: 0 = never)
        uint64_t g_frameTick = 0u;

        struct Armed
        {
            uint32_t self;
            float centre[3];
            float radius2;
            uint32_t events[2];
            uint32_t link;
        };
        Armed g_armed[kMaxArmed]; // (the game thread: the hook fills it, frame() takes it)
        int g_armedCount = 0;

        // The level's objective triggers seen so far (this level), for the chain: each one's depth (hops back along the
        // links to the first beat; -1: not in a chain, or the links loop) and the last frame it waited.
        struct Known
        {
            uint32_t self;
            uint32_t link;
            int depth;
            uint64_t lastSeen; // (g_activeFrames)
            bool gone;
        };
        Known g_known[kMaxArmed];
        int g_knownCount = 0;
        int g_progress = -1;      // (the deepest chain beat passed: -1 none)
        uint64_t g_activeFrames = 0; // (this level's frames that saw triggers: lastSeen counts these, so a cutscene passes nothing)
        bool g_startDone = false; // (the player's place at the level's start decided)

        // Event names, read once per event object (a level's few dozen): its name and whether it is an objective.
        struct EventName
        {
            uint32_t event;
            char name[kName];
            char label[kName];
            uint8_t kind; // 0 objective, 1 music, 2 other
        };
        constexpr int kMaxEvents = 128;
        EventName g_events[kMaxEvents];
        int g_eventCount = 0;

        // What the window thread draws (a seqlock: the game thread writes it whole between two counts).
        struct View
        {
            bool show;
            float target[3];
            float distance; // (metres, to the trigger's edge)
            char label[kName];
        };
        View g_view{};
        std::atomic<uint32_t> g_viewSeq{0};
        std::atomic<uint64_t> g_tick{0}; // (the last game frame's VBlank: TF_GUIDE_SHOT's clock on the window thread)

        uint32_t word(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v = 0u;
            if (const uint8_t *p = getConstMemPtr(rdram, a))
                std::memcpy(&v, p, 4u);
            return v;
        }
        float fword(const uint8_t *rdram, uint32_t a)
        {
            float v = 0.0f;
            if (const uint8_t *p = getConstMemPtr(rdram, a))
                std::memcpy(&v, p, 4u);
            return v;
        }

        // A guest std::string into `out` (short: the size in the first byte, the text from +1; long: the size at +4,
        // the text at the pointer at +8).
        void guestString(const uint8_t *rdram, uint32_t a, char (&out)[kName])
        {
            out[0] = '\0';
            const uint32_t w0 = word(rdram, a);
            uint32_t text = a + 1u, size = (w0 & 0xFFu) >> 1;
            if (w0 & 1u)
            {
                text = word(rdram, a + 8u);
                size = word(rdram, a + 4u);
            }
            size = std::min<uint32_t>(size, kName - 1);
            uint32_t i = 0;
            for (; i < size; ++i)
            {
                const uint8_t *c = getConstMemPtr(rdram, text + i);
                if (!c || *c == 0u)
                    break;
                out[i] = static_cast<char>(*c);
            }
            out[i] = '\0';
        }

        // The name made readable: a short prefix (HQ_, CS_, TA_ ...) left out, words split at '_', at a capital after a
        // small letter and at the last capital of a run before a small one, the first capital kept ("HQ_ToPlane" ->
        // "To plane", "ItsATrap" -> "Its a trap").
        void readable(const char *name, char (&out)[kName])
        {
            for (const auto &[from, to] : kLabels)
                if (std::strcmp(name, from) == 0)
                {
                    std::snprintf(out, kName, "%s", to);
                    return;
                }
            if (std::strncmp(name, "FMV_", 4) == 0)
            {
                std::snprintf(out, kName, "Story scene");
                return;
            }
            const char *s = name;
            if (const char *u = std::strchr(s, '_'); u && u - s <= 3 && u[1])
                s = u + 1;
            int n = 0;
            for (const char *p = s; *p && n < kName - 2; ++p)
            {
                const unsigned char c = static_cast<unsigned char>(*p);
                if (c == '_')
                {
                    if (n > 0 && out[n - 1] != ' ')
                        out[n++] = ' ';
                    continue;
                }
                const bool after = p != s && std::islower(static_cast<unsigned char>(p[-1])),
                           lastOfRun = p != s && std::isupper(static_cast<unsigned char>(p[-1])) && std::islower(static_cast<unsigned char>(p[1]));
                if (std::isupper(c) && (after || lastOfRun) && n > 0 && out[n - 1] != ' ')
                    out[n++] = ' ';
                out[n++] = static_cast<char>(n == 0 ? std::toupper(c) : std::tolower(c));
            }
            out[n] = '\0';
        }

        const EventName *eventName(const uint8_t *rdram, uint32_t event)
        {
            for (int i = 0; i < g_eventCount; ++i)
                if (g_events[i].event == event)
                    return &g_events[i];
            if (g_eventCount == kMaxEvents)
                return nullptr;
            EventName &e = g_events[g_eventCount++];
            e.event = event;
            guestString(rdram, event + 4u, e.name);
            e.kind = 2;
            for (const char *o : kObjectivePrefixes)
                if (std::strncmp(e.name, o, std::strlen(o)) == 0)
                    e.kind = 0;
            for (const char *m : kMusic)
                if (std::strcmp(e.name, m) == 0)
                    e.kind = 1;
            readable(e.name, e.label);
            return &e;
        }

        void waiting(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kWaiting && g_on.load(std::memory_order_relaxed))
            {
                const uint32_t self = getRegU32(ctx, 4);
                bool known = false;
                for (int i = 0; i < g_armedCount && !known; ++i)
                    known = g_armed[i].self == self;
                if (!known && g_armedCount < kMaxArmed)
                {
                    Armed &a = g_armed[g_armedCount++];
                    a.self = self;
                    for (int k = 0; k < 3; ++k)
                        a.centre[k] = fword(rdram, self + 0x40u + 4u * static_cast<uint32_t>(k));
                    a.radius2 = fword(rdram, self + 0x74u);
                    a.events[0] = word(rdram, self + 0xA0u);
                    a.events[1] = word(rdram, self + 0xA4u);
                    a.link = word(rdram, self + kLink);
                }
            }
            g_waiting(rdram, ctx, runtime);
        }

        void nearestPlayer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t out = getRegU32(ctx, 4), ra = getRegU32(ctx, 31);
            g_nearestPlayer(rdram, ctx, runtime);
            if (ra == kNearestPlayerFromTrigger && g_on.load(std::memory_order_relaxed))
            {
                for (int k = 0; k < 3; ++k)
                    g_player[k] = fword(rdram, out + 4u * static_cast<uint32_t>(k));
                g_playerTick = g_frameTick + 1u;
            }
        }

        // A vtable's class name (its type_info's first word points at the name) into `out`.
        void className(const uint8_t *rdram, uint32_t vtable, char (&out)[kName])
        {
            out[0] = '\0';
            const uint32_t ti = vtable ? word(rdram, vtable) : 0u, str = ti ? word(rdram, ti) : 0u;
            int n = 0;
            for (; str && n < kName - 1; ++n)
            {
                const uint8_t *c = getConstMemPtr(rdram, str + static_cast<uint32_t>(n));
                if (!c || *c < 0x20 || *c > 0x7E)
                    break;
                out[n] = static_cast<char>(*c);
            }
            out[n] = '\0';
        }

        void setActor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kSetActor && g_on.load(std::memory_order_relaxed))
            {
                const uint32_t creature = getRegU32(ctx, 4), actor = getRegU32(ctx, 5);
                if (!actor && creature == g_boss)
                    g_boss = 0u; // (it let its actor go: dead)
                else if (actor)
                {
                    char cls[kName], act[kName];
                    className(rdram, word(rdram, creature), cls);
                    className(rdram, word(rdram, actor), act);
                    const char *boss = std::strstr(cls, "Boss") ? cls : std::strstr(act, "Boss") ? act : nullptr;
                    if (boss)
                    {
                        g_boss = creature;
                        const char *b = std::strstr(boss, "Boss") + 4;
                        std::snprintf(g_bossName, kName, "Defeat %s", *b ? b : "the boss");
                        if (g_trace)
                            std::fprintf(stderr, "[guide] the boss: creature %08x (%s), actor %08x (%s)\n", creature, cls, actor, act);
                    }
                }
            }
            g_setActor(rdram, ctx, runtime);
        }

        bool bossPlace(const uint8_t *rdram, float out[3])
        {
            if (!g_boss)
                return false;
            const uint32_t actor = word(rdram, g_boss + 0x24u), node = actor ? word(rdram, actor + 0xCu) : 0u,
                           xform = node ? word(rdram, node + 0x14u) : 0u;
            if (!xform)
                return false;
            for (int k = 0; k < 3; ++k)
                out[k] = fword(rdram, xform + 0x30u + 4u * static_cast<uint32_t>(k));
            return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
        }

        void publish(const View &v)
        {
            g_viewSeq.fetch_add(1u, std::memory_order_acq_rel); // (odd: being written)
            g_view = v;
            g_viewSeq.fetch_add(1u, std::memory_order_acq_rel);
        }

        // (TF_GUIDE_TRACE) the triggers seen this level, by object, so each is logged once
        uint32_t g_traced[256];
        int g_tracedCount = 0;
    }

    void addSettings()
    {
        ps2_settings::Setting s;
        s.key = "TF_OBJECTIVE_GUIDE";
        s.section = "Gameplay";
        s.label = "Objective guide";
        s.help = "An arrow at the bottom of the screen pointing to the level's next story place, with its name and distance. "
                 "It follows the level's own triggers.";
        s.choices = {{"0", "Off"}, {"1", "On"}};
        s.defaultValue = "0";
        s.apply = [](const std::string &value)
        {
            g_on.store(value == "1");
            return true;
        };
        ps2_settings::add(s);
        const char *v = std::getenv("TF_OBJECTIVE_GUIDE");
        g_on.store(v && *v == '1');
    }

    void install(PS2Runtime &runtime)
    {
        g_trace = std::getenv("TF_GUIDE_TRACE") && *std::getenv("TF_GUIDE_TRACE") == '1';
        if (const char *v = std::getenv("TF_OBJECTIVE_GUIDE"); v && *v == '1')
            g_on.store(true);
        if ((g_setActor = runtime.lookupFunction(kSetActor)) != nullptr)
            runtime.registerFunction(kSetActor, setActor);
        if ((g_nearestPlayer = runtime.lookupFunction(kNearestPlayer)) != nullptr)
            runtime.registerFunction(kNearestPlayer, nearestPlayer);
        if ((g_waiting = runtime.lookupFunction(kWaiting)) != nullptr)
            runtime.registerFunction(kWaiting, waiting);
        else
            std::fprintf(stderr, "[guide] nothing at %08x to hook: the objective guide is off\n", kWaiting);
    }

    void frame(const uint8_t *rdram, bool inLevel, uint32_t playerPosition, bool moviePlaying, uint64_t tick)
    {
        g_tick.store(tick, std::memory_order_relaxed);
        g_frameTick = tick;
        static bool wasLevel = false;
        static int quiet = 0; // (frames with no trigger seen: some update every other frame)
        static uint32_t lastPick = 0u;
        if (inLevel != wasLevel)
        {
            wasLevel = inLevel;
            g_eventCount = 0; // (a new level: its own event objects)
            g_playerTick = 0u;
            if (!inLevel)
                g_boss = 0u;
            g_tracedCount = 0;
            g_knownCount = 0;
            g_progress = -1;
            g_activeFrames = 0;
            g_startDone = false;
            lastPick = 0u;
        }
        View v{};
        const bool seen = g_playerTick != 0u && tick <= g_playerTick + 30u; // (the triggers asked for the player lately)
        float player[3] = {};
        bool placed = seen || playerPosition != 0u;
        for (int k = 0; k < 3; ++k)
            player[k] = seen ? g_player[k] : playerPosition ? fword(rdram, playerPosition + 4u * static_cast<uint32_t>(k)) : 0.0f;
        if (!placed && g_boss)
            placed = photo_mode::viewPosition(player); // (no trigger, no camera target: Cybertron - where the camera stands)
        if (g_trace && tick % 600u == 0u) // (every 10 s of VBlanks: why nothing shows, when nothing does)
            std::fprintf(stderr,
                         "[guide] tick %llu: on %d, level %d, movie %d, player %08x at (%.0f, %.0f, %.0f)%s, photo %d, hud hidden %d, armed %d, "
                         "passed depth %d\n",
                         static_cast<unsigned long long>(tick), g_on.load() ? 1 : 0, inLevel ? 1 : 0, moviePlaying ? 1 : 0, playerPosition, player[0],
                         player[1], player[2], placed ? "" : " (not known)", photo_mode::active() ? 1 : 0, photo_mode::hudHidden() ? 1 : 0,
                         g_armedCount, g_progress);
        if (!g_on.load(std::memory_order_relaxed) || !inLevel || moviePlaying || !placed || photo_mode::active() || photo_mode::hudHidden())
        {
            g_armedCount = 0;
            publish(v);
            return;
        }
        if (g_armedCount == 0 && ++quiet < 8)
            return; // (the last pick stays shown)
        quiet = 0;
        if (g_armedCount > 0)
            ++g_activeFrames;
        auto distance = [&](const float at[3], float radius2)
        {
            const float dx = at[0] - player[0], dy = at[1] - player[1], dz = at[2] - player[2];
            return std::sqrt(dx * dx + dy * dy + dz * dz) - std::sqrt(std::max(0.0f, radius2));
        };
        // This frame's triggers: their objective (if any), and the level's known objective triggers brought up to date.
        const EventName *objectives[kMaxArmed] = {};
        int known[kMaxArmed];
        for (int i = 0; i < g_armedCount; ++i)
        {
            const Armed &a = g_armed[i];
            const EventName *&objective = objectives[i];
            known[i] = -1;
            for (uint32_t e : a.events)
                if (e)
                    if (const EventName *n = eventName(rdram, e); n && n->kind == 0 && !objective)
                        objective = n;
            if (g_trace && g_tracedCount < 256 && std::find(g_traced, g_traced + g_tracedCount, a.self) == g_traced + g_tracedCount)
            {
                g_traced[g_tracedCount++] = a.self;
                const EventName *e0 = a.events[0] ? eventName(rdram, a.events[0]) : nullptr;
                const EventName *e1 = a.events[1] ? eventName(rdram, a.events[1]) : nullptr;
                std::fprintf(stderr, "[guide] tick %llu trigger %08x link %08x at (%.1f, %.1f, %.1f) radius %.1f: events '%s' (%s) '%s' (%s)\n",
                             static_cast<unsigned long long>(tick), a.self, a.link, a.centre[0], a.centre[1], a.centre[2], std::sqrt(std::max(0.0f, a.radius2)),
                             e0 ? e0->name : "", e0 ? (e0->kind == 0 ? "objective" : e0->kind == 1 ? "music" : "other") : "-", e1 ? e1->name : "",
                             e1 ? (e1->kind == 0 ? "objective" : e1->kind == 1 ? "music" : "other") : "-");
            }
            if (!objective)
                continue;
            int k = 0;
            while (k < g_knownCount && g_known[k].self != a.self)
                ++k;
            if (k == g_knownCount)
            {
                if (g_knownCount == kMaxArmed)
                    continue;
                Known &n = g_known[g_knownCount++];
                n.self = a.self;
                n.link = a.link;
                n.gone = false;
                n.depth = 0; // (hops back along the links to the first beat; a loop: not a chain)
                for (uint32_t at = a.link; at && n.depth >= 0; at = word(rdram, at + kLink))
                    n.depth = (at == a.self || n.depth >= 16) ? -1 : n.depth + 1;
            }
            g_known[k].lastSeen = g_activeFrames;
            known[i] = k;
        }
        // In the chain: a beat with a link, or one a beat links to. Passed: a chain beat that stopped waiting.
        auto inChain = [&](const Known &n)
        {
            if (n.depth < 0)
                return false;
            if (n.link)
                return true;
            for (int k = 0; k < g_knownCount; ++k)
                if (g_known[k].link == n.self)
                    return true;
            return false;
        };
        if (g_armedCount > 0)
            for (int k = 0; k < g_knownCount; ++k)
                if (Known &n = g_known[k]; !n.gone && g_activeFrames > n.lastSeen + kGoneFrames)
                {
                    n.gone = true;
                    if (inChain(n) && n.depth > g_progress)
                    {
                        g_progress = n.depth;
                        if (g_trace)
                            std::fprintf(stderr, "[guide] tick %llu: chain beat %08x (depth %d) passed\n", static_cast<unsigned long long>(tick), n.self, n.depth);
                    }
                }
        // The level's start: the chain beat whose centre is nearest the warp gate the player came in by is next (by the
        // centres, every level's gate 0 picks its first beat; Pacific Island's gate 0 stands inside the second's 230 radius).
        if (!g_startDone && g_armedCount > 0)
        {
            g_startDone = true;
            int nearest = -1;
            float nearestD = 1e30f;
            for (int i = 0; i < g_armedCount; ++i)
                if (known[i] >= 0 && inChain(g_known[known[i]]))
                    if (const float d = distance(g_armed[i].centre, 0.0f); d < nearestD) // (to the centre: a wide trigger's edge would win)
                        nearestD = d, nearest = i;
            if (nearest >= 0)
                g_progress = g_known[known[nearest]].depth - 1;
            if (g_trace)
                std::fprintf(stderr, "[guide] tick %llu: the start at (%.0f, %.0f, %.0f): %s, so the chain from depth %d\n",
                             static_cast<unsigned long long>(tick), player[0], player[1], player[2],
                             nearest < 0 ? "no chain" : "the nearest chain beat is next", g_progress + 1);
        }
        // The chain's next beat: the earliest after the player's place (at one depth, the nearest); none after it (the
        // player went back, or the place was a guess): the nearest chain beat. Outside the chain: the nearest objective,
        // which takes the arrow when clearly nearer.
        int chainPick = -1, chainAny = -1, soloPick = -1;
        float chainD = 1e30f, chainAnyD = 1e30f, soloD = 1e30f;
        int chainDepth = 1 << 30;
        for (int i = 0; i < g_armedCount; ++i)
        {
            if (!objectives[i])
                continue;
            const float d = distance(g_armed[i].centre, g_armed[i].radius2);
            const Known *n = known[i] >= 0 ? &g_known[known[i]] : nullptr;
            if (n && inChain(*n))
            {
                if (d < chainAnyD)
                    chainAnyD = d, chainAny = i;
                if (n->depth > g_progress && (n->depth < chainDepth || (n->depth == chainDepth && d < chainD)))
                    chainDepth = n->depth, chainD = d, chainPick = i;
            }
            else if (d < soloD)
                soloD = d, soloPick = i;
        }
        if (chainPick < 0)
            chainPick = chainAny, chainD = chainAnyD;
        int pickIndex = chainPick;
        float best = chainD;
        if (soloPick >= 0 && (chainPick < 0 || (soloD < kSoloNearer * chainD && soloD < kSoloReach)))
            pickIndex = soloPick, best = soloD;
        const Armed *pick = pickIndex >= 0 ? &g_armed[pickIndex] : nullptr;
        const EventName *pickName = pickIndex >= 0 ? objectives[pickIndex] : nullptr;
        float bossAt[3];
        if (pick)
        {
            v.show = true;
            std::memcpy(v.target, pick->centre, sizeof(v.target));
            v.distance = std::max(0.0f, best) / kUnitsPerMetre;
            std::snprintf(v.label, kName, "%s", pickName->label);
        }
        else if (bossPlace(rdram, bossAt)) // (no story beat left: the boss while it lives - Cybertron's Unicron)
        {
            v.show = true;
            std::memcpy(v.target, bossAt, sizeof(v.target));
            const float dx = bossAt[0] - player[0], dy = bossAt[1] - player[1], dz = bossAt[2] - player[2];
            v.distance = std::sqrt(dx * dx + dy * dy + dz * dz) / kUnitsPerMetre;
            std::snprintf(v.label, kName, "%s", g_bossName);
        }
        const uint32_t now = pick ? pick->self : v.show ? g_boss : 0u;
        if (g_trace && now != lastPick)
            std::fprintf(stderr, "[guide] tick %llu pick: %s (%s%s, %.0f m) from (%.0f, %.0f, %.0f)\n", static_cast<unsigned long long>(tick),
                         v.show ? v.label : "none", pick ? pickName->name : v.show ? "boss" : "",
                         !pick ? "" : pickIndex == soloPick ? ", outside the chain" : ", the chain", v.distance, player[0], player[1], player[2]);
        lastPick = now;
        g_armedCount = 0;
        publish(v);
    }

    void draw()
    {
        if (!g_on.load(std::memory_order_relaxed))
            return;
        View v;
        uint32_t before = 0, after = 0;
        do
        {
            before = g_viewSeq.load(std::memory_order_acquire);
            v = g_view;
            after = g_viewSeq.load(std::memory_order_acquire);
        } while (before != after || (before & 1u));
        if (!v.show)
            return;
        float ahead[3], left[3], up[3], tanUp = 0.0f, eye[3];
        if (!photo_mode::viewFrame(ahead, left, up, tanUp) || !photo_mode::viewPosition(eye))
            return;
        const float d[3] = {v.target[0] - eye[0], v.target[1] - eye[1], v.target[2] - eye[2]};
        const float a = d[0] * ahead[0] + d[1] * ahead[1] + d[2] * ahead[2];
        const float l = d[0] * left[0] + d[1] * left[1] + d[2] * left[2];
        const float angle = std::atan2(-l, a); // (0: ahead, up the screen; positive: to the right)
        const int h = GetScreenHeight(), w = GetScreenWidth();
        if (g_trace) // (tests: the arrow's angle against where the game's camera shows the target - right of the middle: positive)
        {
            static double last = 0.0;
            if (GetTime() - last > 2.0)
            {
                last = GetTime();
                float px = 0.0f, py = 0.0f;
                const bool on = photo_mode::project(v.target, static_cast<float>(w), static_cast<float>(h), static_cast<float>(w) / h, px, py);
                std::fprintf(stderr, "[guide] arrow %s: angle %.0f, ahead %.0f left %.0f, the target on screen %s (%.0f of %d)\n", v.label,
                             angle * 57.2958f, a, l, on ? (px > w * 0.5f ? "right" : "left") : "not in view", px, w);
            }
        }
        const float size = std::max(14.0f, h / 30.0f);
        const float cx = w * 0.5f, cy = h - size * 2.6f;
        const float s = std::sin(angle), c = std::cos(angle);
        auto at = [&](float x, float y) { return Vector2{cx + x * c - y * s, cy + x * s + y * c}; }; // (y down the screen)
        const Vector2 tip = at(0.0f, -size), leftBack = at(-size * 0.62f, size * 0.7f), notch = at(0.0f, size * 0.3f),
                      rightBack = at(size * 0.62f, size * 0.7f);
        const Color fill{120, 220, 255, 235}, shade{0, 0, 0, 150};
        // (raylib draws a triangle only with its corners counter-clockwise on the screen: put them so)
        auto tri = [](Vector2 p, Vector2 q, Vector2 r, float off, Color col)
        {
            p.x += off, p.y += off, q.x += off, q.y += off, r.x += off, r.y += off;
            if ((q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x) > 0.0f)
                std::swap(q, r);
            DrawTriangle(p, q, r, col);
        };
        for (const float off : {2.0f, 0.0f})
        {
            const Color col = off > 0.0f ? shade : fill;
            tri(tip, leftBack, notch, off, col);
            tri(tip, notch, rightBack, off, col);
        }
        char text[kName + 24];
        std::snprintf(text, sizeof(text), "%s  %.0f m", v.label, v.distance);
        const int fontSize = std::max(14, h / 40);
        const int tw = MeasureText(text, fontSize);
        const int tx = static_cast<int>(cx) - tw / 2, ty = static_cast<int>(cy + size * 1.2f);
        DrawText(text, tx + 2, ty + 2, fontSize, Color{0, 0, 0, 170});
        DrawText(text, tx, ty, fontSize, fill);
        // TF_GUIDE_SHOT=<vblank>,... (tests): the window's picture with the guide on it, to runs/guide_<vblank>.png
        static const char *shots = std::getenv("TF_GUIDE_SHOT");
        static const char *next = shots;
        if (next && *next)
        {
            char *end = nullptr;
            const unsigned long long shotAt = std::strtoull(next, &end, 10);
            if (g_tick.load(std::memory_order_relaxed) >= shotAt)
            {
                char file[64];
                std::snprintf(file, sizeof(file), "runs/guide_%llu.png", shotAt);
                Image shot = LoadImageFromScreen();
                ExportImage(shot, file);
                UnloadImage(shot);
                std::fprintf(stderr, "[guide] TF_GUIDE_SHOT: %s (%s, %.0f m, angle %.0f)\n", file, v.label, v.distance, angle * 57.2958f);
                next = (end && *end == ',') ? end + 1 : nullptr;
            }
        }
    }
}
