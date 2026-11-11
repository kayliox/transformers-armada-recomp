// Freed actors kept by creatures across a level's end (docs/TODO.md item 51).
//
// The bug: a creature's ReleaseActor (taCreatureBase slot 50, 0x2A64C0) hands the actor at creature
// +0x24 back to its pool when the next level loads - but at the old level's end the pool manager
// (0x5262F0, from 0x296200) has already deleted every pool's actors (taActorPoolBase 0x2B4BE0;
// the shared pool at gp-0x3510 = 0x6CFBE0 is left alone). A creature that still holds one then
// works on freed memory: func_00297780 -> the actor's slot 25 -> func_003DB890 (taActorEvenSimpler)
// walks a garbage child list (a child at 0x6e007, its vtable 0), the call goes to address 0 and the
// game hangs on the warp's loading screen (2026-10-23 bikefix/c3db; 2026-10-28 both Amazon warp loops
// on the user's PC). Normally every creature has given its actor back before the level ends; one that
// took (or kept) one late does not. The jet bike's fix (jet_bike.cpp, shutdownFn) covered the bikes.
//
// The fix at the source, for every creature: every creature sets its actor through taCreatureBase
// slot 49 (0x2A64B0: creature +0x24 = actor; each class's own slot 49 - taCreatureAI 0x2AC420,
// taTransformerCreature 0x34A250, taCreatureBossUnicron 0x4A0000, taSidekickCreature 0x4E8FB0,
// taCreatureTurretManned 0x5BE7E0 - calls it first, then keeps a copy of its own), so the creatures
// that hold an actor are known here. When the pool manager deletes the pools, each creature whose
// actor is among the ones being deleted lets go of it: +0x24 and the class's own copy are cleared,
// as ReleaseActor would leave them. Its later ReleaseActor (and taCreatureAI's DoKill 0x2AC2A0, which
// hands +0xF4 to 0x3DB890) then has nothing to give back.
//
// The safety net, kept: before func_003DB890 walks an actor's children, every child and the function
// each one would call are checked (main memory, aligned, a vtable whose slot 3 is a function the game
// has); a list that fails is not walked ("[actor guard] ..." in the log).
//
// TF_ACTOR_GUARD=0: both off (the original game). TF_ACTOR_GUARD=log: neither changes anything, the
// creatures that would have been let go are reported (the culprits, with the hang still happening).
// Decomp equivalent: the pool manager's delete telling the creatures (or each actor's destructor
// clearing its creature's pointer), and the check at the top of func_003DB890.

#include "actor_guard.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include "game_version.h" // (TF_GA: the code addresses that differ in the PAL release)

std::string ps2xRecentDispatches(); // (the runtime: the thread's last guest dispatches)

namespace actor_guard
{
    namespace
    {
        constexpr uint32_t kChildWalk = TF_GA(0x003DB890u);   // func_003DB890 (taActorEvenSimpler, its children's vfunc_03)
        constexpr uint32_t kSetActor = 0x002A64B0u;    // taCreatureBase slot 49: creature +0x24 = actor
        constexpr uint32_t kDeletePools = TF_GA(0x005262F0u); // the pool manager: every pool's actors deleted (a level's end)
        constexpr uint32_t kPoolManager = 0x006CFBD8u; // gp-0x3518: pools at +0xC + 4 * type bit (64)
        constexpr uint32_t kSharedPool = 0x006CFBE0u;  // gp-0x3510: the pool the manager leaves alone
        constexpr uint32_t kVtSlot49 = 8u + 4u * 49u;  // (vtables: type_info, 0, then the slots)

        PS2Runtime::RecompiledFunction g_childWalk = nullptr, g_setActor = nullptr, g_deletePools = nullptr;
        PS2Runtime *g_runtime = nullptr;
        bool g_logOnly = false;
        uint64_t g_skipped = 0u, g_released = 0u;

        struct Held
        {
            uint32_t actor, vtable;
        };
        std::unordered_map<uint32_t, Held> g_held; // creature -> the actor it set, its vtable then

        bool readWord(const uint8_t *rdram, uint32_t a, uint32_t &v)
        {
            if (a < 0x00100000u || a >= PS2_RAM_SIZE || (a & 3u))
                return false;
            const uint8_t *p = getConstMemPtr(rdram, a);
            if (!p)
                return false;
            std::memcpy(&v, p, sizeof(v));
            return true;
        }
        uint32_t word(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v = 0u;
            return readWord(rdram, a, v) ? v : 0u;
        }
        void setWord(uint8_t *rdram, uint32_t a, uint32_t v)
        {
            if (a < 0x00100000u || a >= PS2_RAM_SIZE || (a & 3u))
                return;
            if (uint8_t *p = getMemPtr(rdram, a))
                std::memcpy(p, &v, sizeof(v));
        }

        // The class name of a vtable (its type_info's first word is the name).
        const char *className(const uint8_t *rdram, uint32_t vtable)
        {
            static char name[64];
            name[0] = 0;
            const uint32_t ti = word(rdram, vtable), str = ti ? word(rdram, ti) : 0u;
            for (uint32_t k = 0; str && k + 1 < sizeof(name); ++k)
            {
                const uint32_t w = word(rdram, (str + k) & ~3u);
                const char c = static_cast<char>((w >> (8u * ((str + k) & 3u))) & 0xFFu);
                if (c < 0x20 || c > 0x7E)
                    break;
                name[k] = c;
                name[k + 1] = 0;
            }
            return name[0] ? name : "?";
        }

        // ---- the source fix ---------------------------------------------------------------------
        void setActor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kSetActor)
            {
                const uint32_t creature = getRegU32(ctx, 4), actor = getRegU32(ctx, 5);
                if (actor)
                    g_held[creature] = Held{actor, word(rdram, creature)};
                else
                    g_held.erase(creature);
            }
            g_setActor(rdram, ctx, runtime);
        }

        void deletePools(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kDeletePools)
            {
                // The actors about to be deleted: every used entry of every pool but the shared one
                // (the loop of 0x5262F0; a pool's entries are 16 bytes: owner, actor, id, block).
                std::unordered_set<uint32_t> doomed, pooled; // (pooled: every pool's, the shared one's too)
                const uint32_t manager = word(rdram, kPoolManager), shared = word(rdram, kSharedPool);
                auto collect = [&](uint32_t pool, std::unordered_set<uint32_t> &into)
                {
                    const uint32_t count = word(rdram, pool + 0x04u), entries = word(rdram, pool + 0x20u);
                    for (uint32_t k = 0; entries && k < count && k < 1024u; ++k)
                        if (const uint32_t actor = word(rdram, entries + 16u * k + 4u))
                            into.insert(actor);
                };
                if (shared)
                    collect(shared, pooled);
                for (uint32_t i = 0; manager && i < 64u; ++i)
                {
                    const uint32_t pool = word(rdram, manager + 0x0Cu + 4u * i);
                    if (!pool || pool == shared)
                        continue;
                    collect(pool, doomed);
                    collect(pool, pooled);
                }
                uint32_t released = 0u, kept = 0u, unpooled = 0u;
                for (auto it = g_held.begin(); it != g_held.end();)
                {
                    const uint32_t creature = it->first, actor = it->second.actor, vtable = it->second.vtable;
                    // (still that creature, still holding that actor: a creature deleted since is not touched)
                    const bool same = word(rdram, creature) == vtable && word(rdram, creature + 0x24u) == actor;
                    if (!same)
                    {
                        it = g_held.erase(it);
                        continue;
                    }
                    if (!doomed.count(actor))
                    {
                        // (an actor in no pool at all: deleted earlier - a large pool deletes the actor
                        // handed back, 0x2B4D20 - or never pooled; reported, left alone)
                        if (!pooled.count(actor) && unpooled++ < 8u && std::getenv("TF_ACTOR_GUARD_TRACE"))
                            std::fprintf(stderr, "[actor guard] the level ends: %s %08x holds actor %08x, in no pool\n",
                                         className(rdram, vtable), creature, actor);
                        ++kept, ++it;
                        continue;
                    }
                    const uint32_t own = word(rdram, vtable + kVtSlot49);
                    if (g_released + released < 40u)
                        std::fprintf(stderr, "[actor guard] the level ends: %s %08x still holds actor %08x (deleted with the pools)%s\n",
                                     className(rdram, vtable), creature, actor, g_logOnly ? " - left (log only)" : " - let go");
                    ++released;
                    if (!g_logOnly)
                    {
                        // ReleaseActor's end state: +0x24 cleared, and the class's own copy as its slot 49
                        // with 0 leaves it (only where it still holds this actor).
                        auto clear = [&](uint32_t at)
                        {
                            if (word(rdram, at) == actor)
                                setWord(rdram, at, 0u);
                        };
                        clear(creature + 0x24u);
                        if (own == 0x002AC420u || own == TF_GA(0x005BE7E0u)) // taCreatureAI (and the manned turret, one of them)
                            clear(creature + 0xF4u);
                        if (own == TF_GA(0x005BE7E0u))
                        {
                            clear(creature + 0x110u);
                            if (const uint32_t sub = word(rdram, creature + 0x3Cu))
                                clear(sub + 0x10u);
                        }
                        if (own == TF_GA(0x004A0000u) || own == TF_GA(0x004E8FB0u)) // taCreatureBossUnicron, taSidekickCreature
                            clear(creature + 0x38u);
                        if (own == TF_GA(0x0034A250u)) // taTransformerCreature
                            clear(creature + 0x54u);
                    }
                    it = g_held.erase(it);
                }
                g_released += released;
                if (released || std::getenv("TF_ACTOR_GUARD_TRACE"))
                    std::fprintf(stderr, "[actor guard] pools deleted: %zu actors (%zu pooled in all); %u creatures let go of theirs, %u keep one (%u in no pool)\n",
                                 doomed.size(), pooled.size(), released, kept, unpooled);
            }
            g_deletePools(rdram, ctx, runtime);
        }

        // ---- the safety net ---------------------------------------------------------------------
        // The actor's children can be walked as func_003DB890 walks them: +0x4C how many, +0x50 the array of their
        // pointers, each one an object whose vtable's +0x14 (slot 3) is code. (TODO 185: until 2026-11-08 this read
        // +0x48 / +0x4C as the array's start and end and slot 3 at +0x0C - every actor with children, a Heavy Grunt's
        // 14 among them, failed it, and their release walks were all left out: the children never let go.)
        bool childrenValid(const uint8_t *rdram, uint32_t self)
        {
            uint32_t count = 0u, array = 0u;
            if (!readWord(rdram, self + 0x4Cu, count) || !readWord(rdram, self + 0x50u, array))
                return false;
            if (count == 0u)
                return true; // (no children)
            if (count > 1024u)
                return false;
            for (uint32_t i = 0; i < count; ++i)
            {
                uint32_t child = 0u, vtable = 0u, fn = 0u;
                if (!readWord(rdram, array + 4u * i, child) || !readWord(rdram, child, vtable) || !readWord(rdram, vtable + 0x14u, fn))
                    return false;
                if (fn < 0x00100000u || fn >= 0x00800000u || (fn & 3u) || !g_runtime->lookupFunction(fn))
                    return false;
            }
            return true;
        }

        void childWalk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kChildWalk)
            {
                const uint32_t self = getRegU32(ctx, 4);
                if (!childrenValid(rdram, self))
                {
                    if (g_skipped++ < 8u)
                        std::fprintf(stderr, "[actor guard] actor %08x's child list is not valid (a freed actor) - its release walk left out%s\n",
                                     self, g_logOnly ? " (log only: walked anyway)" : "");
                    if (!g_logOnly)
                    {
                        ctx->pc = getRegU32(ctx, 31); // (returns at once: the function has no result)
                        return;
                    }
                }
            }
            g_childWalk(rdram, ctx, runtime);
        }

        // (TODO 181) An AI action left over from a level: taAction_SimpleCommand's run (0x391280, its slot 2) calls its
        // creature's (+8) component at +0xF4 (slot 0xB4, with the command at +0x10), then the action manager (0x726B50)
        // takes it off its list (0x395940). One queued for a creature of the level before - a Medium Grunt of Amazon on
        // the user's PC, its actor let go above when that level ended - ran in the next warp (HQ -> Antarctica): +0xF4 was
        // 0, the call went to address 0 and the EE stopped on the warp's loading screen. A creature or component that is
        // no longer an object (not in main memory, no vtable whose slot 0xB4 is code): the command is left out and the
        // action handed to the manager as the function ends (a tail call - the function keeps nothing).
        constexpr uint32_t kSimpleCommandRun = TF_GA(0x00391280u), kActionManagerDone = TF_GA(0x00395940u), kActionManager = 0x00726B50u;
        PS2Runtime::RecompiledFunction g_simpleCommandRun = nullptr;
        uint32_t g_commandsSkipped = 0u;
        bool componentValid(const uint8_t *rdram, uint32_t action)
        {
            uint32_t creature = 0u, component = 0u, vtable = 0u, fn = 0u;
            if (!readWord(rdram, action + 8u, creature) || !creature || (creature & 3u) || !readWord(rdram, creature + 0xF4u, component) ||
                !component || (component & 3u) || !readWord(rdram, component, vtable) || (vtable & 3u) || !readWord(rdram, vtable + 0xB4u, fn))
                return false;
            return fn >= 0x00100000u && fn < 0x00800000u && !(fn & 3u) && g_runtime->lookupFunction(fn);
        }
        void simpleCommandRun(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kSimpleCommandRun)
            {
                const uint32_t action = getRegU32(ctx, 4);
                if (!componentValid(rdram, action))
                {
                    uint32_t creature = 0u;
                    readWord(rdram, action + 8u, creature);
                    if (g_commandsSkipped++ < 8u)
                        std::fprintf(stderr, "[actor guard] an AI command for creature %08x, not an object any more (a level's leftover) - left out%s\n",
                                     creature, g_logOnly ? " (log only: run anyway)" : "");
                    if (!g_logOnly)
                    {
                        SET_GPR_U32(ctx, 4, kActionManager);
                        SET_GPR_U32(ctx, 5, action);
                        SET_GPR_U32(ctx, 6, 1u);
                        ctx->pc = kActionManagerDone; // (a tail call: it returns to this function's caller)
                        if (PS2Runtime::RecompiledFunction done = runtime->lookupFunction(kActionManagerDone))
                            done(rdram, ctx, runtime);
                        return;
                    }
                }
            }
            g_simpleCommandRun(rdram, ctx, runtime);
        }

        // (TODO 185 a) A projectile whose flight logic is gone: the energy blast's update (taEnergyBlastProjectile, 0x3D5CA0)
        // asks its logic (+0x10, a taWeaponLogic2_StraightProjectile) through the logic's slot 4 (vtable +0x10) - a logic of 0
        // it handles itself (returns 0). On the user's PC (2026-11-07 18:20, on the jet bike by a warp gate) a projectile's
        // logic had been freed while the projectile still flew: its block was back in the heap's free list and its first
        // words written over (a matrix row), the call went to address 0 and the game stopped. The hang dump: the projectile
        // at 0x0FECDFD0, its logic at 0x0FECDFB0 (vtable word 0xBD9A7DBD), the block in the free list of 0x0FECE24C. A logic
        // that is no longer an object (not in main memory, no vtable whose slot 4 is a function the game has) is taken as
        // none: the update returns 0, as for a projectile without one.
        constexpr uint32_t kProjectileUpdate = TF_GA(0x003D5CA0u);
        PS2Runtime::RecompiledFunction g_projectileUpdate = nullptr;
        uint32_t g_projectilesSkipped = 0u;
        void projectileUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kProjectileUpdate)
            {
                const uint32_t projectile = getRegU32(ctx, 4);
                uint32_t logic = 0u, vtable = 0u, fn = 0u;
                if (readWord(rdram, projectile + 0x10u, logic) && logic &&
                    (!readWord(rdram, logic, vtable) || !readWord(rdram, vtable + 0x10u, fn) || fn < 0x00100000u || fn >= 0x00800000u ||
                     (fn & 3u) || !runtime->lookupFunction(fn)))
                {
                    if (g_projectilesSkipped++ < 8u)
                        std::fprintf(stderr, "[actor guard] projectile %08x (%s): its flight logic %08x is not an object any more (vtable %08x) - "
                                             "taken as none%s\n",
                                     projectile, className(rdram, word(rdram, projectile)), logic, vtable, g_logOnly ? " (log only: called anyway)" : "");
                    if (!g_logOnly)
                    {
                        SET_GPR_U32(ctx, 2, 0u);
                        ctx->pc = getRegU32(ctx, 31);
                        return;
                    }
                }
            }
            g_projectileUpdate(rdram, ctx, runtime);
        }

        // (TODO 185 a) Who frees a live flight logic: operator delete (0x1061E0) given a taWeaponLogic2_StraightProjectile
        // whose vtable is still its own - its destructors (the projectile's 0x3D5F10, the logic's own) change it first, so
        // a delete of an intact one comes from elsewhere (a stale pointer to the block: the 18:20 freeze). Logged with the
        // caller and the last guest calls, the first 4 times; nothing changed. A load and a compare a delete.
        constexpr uint32_t kOperatorDelete = 0x001061E0u, kLogicVtable = 0x006BC000u;
        PS2Runtime::RecompiledFunction g_operatorDelete = nullptr;
        uint32_t g_logicDeletesLogged = 0u;
        void operatorDelete(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kOperatorDelete && g_logicDeletesLogged < 4u)
            {
                const uint32_t p = getRegU32(ctx, 4);
                uint32_t vtable = 0u;
                if (p && readWord(rdram, p, vtable) && vtable == kLogicVtable)
                {
                    ++g_logicDeletesLogged;
                    std::fprintf(stderr, "[actor guard] a projectile's flight logic %08x freed while still whole, from %08x (tick %llu) - trace: %s\n", p,
                                 getRegU32(ctx, 31), static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()),
                                 ps2xRecentDispatches().c_str());
                }
            }
            g_operatorDelete(rdram, ctx, runtime);
        }

        void hook(PS2Runtime &runtime, uint32_t address, PS2Runtime::RecompiledFunction &original, PS2Runtime::RecompiledFunction fn)
        {
            if ((original = runtime.lookupFunction(address)) != nullptr)
                runtime.registerFunction(address, fn);
            else
                std::fprintf(stderr, "[actor guard] nothing at %08x to hook\n", address);
        }
    }

    void install(PS2Runtime &runtime)
    {
        const char *v = std::getenv("TF_ACTOR_GUARD");
        if (v && *v == '0')
            return;
        g_logOnly = v && (*v == 'l' || *v == 'L');
        g_runtime = &runtime;
        hook(runtime, kChildWalk, g_childWalk, childWalk);
        hook(runtime, kSetActor, g_setActor, setActor);
        hook(runtime, kDeletePools, g_deletePools, deletePools);
        hook(runtime, kSimpleCommandRun, g_simpleCommandRun, simpleCommandRun); // (TODO 181)
        hook(runtime, kProjectileUpdate, g_projectileUpdate, projectileUpdate); // (TODO 185 a)
        hook(runtime, kOperatorDelete, g_operatorDelete, operatorDelete); // (TODO 185 a: who frees a live one)
    }
}
