// N7 step 3: the game's look-and-sound pools made bigger with the 256 MB RAM (pool_growth.h).
//
// - Particle buffers (taParticleManager's shared ones): 0x36A6D0 (manager, count, type) makes `count` buffers of the
//   type's 10 / 25 / 50 / 100 / 200 particles (0x20 bytes each), their records and particles from the game's heap;
//   taGame's start-up (0x292060) asks 0x20, 0x10, 0x60, 0x18, 0x10 and 8. An effect that finds no free buffer of its
//   type has no particles (Antarctica's: 212 of 258 asks refused, the census). The count is multiplied.
// - Surface impacts (ImpactManager, 0x40C8A0: 55 pools of the impacts of a surface type - dust, sparks, splashes):
//   its constructor puts 9, 4 or 2 impacts on each pool's free list in 57 loops of the same shape - `new` (0x1061F0),
//   the impact's constructor (0x40F9B0), its counter s3 up by one, `slti s3, 9|4|2` - and 0x40F610 takes from the
//   list (none left: the impact is not shown; 626 of 840 refused on level 3). The impact's constructor, called from one
//   of those loops, steps s3 back by one on (factor - 1) of each factor calls: the loop goes on, and the game makes,
//   constructs and links the extra impacts itself. (The generated code reloads s3 from the context after the call.)
//   A loop is told by the instruction after the call site: `addiu s3, s3, 1` (0x26730001) four bytes on from the
//   return address; the constructor's two single-impact calls are left alone.
//
// N7 step 4: the safety limits - where the game has no way out when they fill (a generic fatal error):
// - The display lists' blocks (taDisplayListBase::CreateBlockPool, 0x5A8080: one block of 0x100C bytes for each 0xFEC
//   bytes asked - 0x4B000, 75 blocks; 0x7D000, 125, for episode type 7; the HQ's 0x4B000): the bytes asked multiplied
//   by kDisplayBlocksFactor. The game makes and frees them itself, from its own heap.
// - The creature lists' items (taCreatureListUtil, 0x3772D0: two vectors - 0x726700 of 44-byte items, 0x726710 of
//   16-byte entries - each filled with pointers to 500 static elements; an empty one is a generic fatal error; made at
//   a level's start, freed at its end): once a frame (frame()), a vector of the game's own (500, no "CLU4" before its
//   array) is given an array for kCreatureListItems pointers from the runtime's heap - its free pointers and the added
//   elements (zeroed; the game sets each when it takes it), the elements in use coming back into it as before. The
//   block starts with "CLU4", the game's own array and its capacity, which the shutdown (0x3771B0, its storage swapped
//   out and deleted) gets back before it runs: the game frees only what it allocated. (From the frame, not a wrapper
//   on the startup: that may stop at a checkpoint, and a save state's lists may be the game's.)
//
// N7 step 5: the enemy waves. The actor-pool manager (*0x6CFBD8: +4 the memory its pools' actors use, +8 the budget,
// 0x100000 from its constructor) refuses a new actor whose footprint would pass the budget (0x525EF0) - the game then
// holds the spawn back until enough have died, which is the console's pace (thousands of refusals on levels 4, 5, 7).
// Unlimited sets the budget to 1 GB (only the arena's room holds them back, its check left as it is); the PS2's pace
// puts the game's 1 MB back. Written once a frame, and only over the game's value or ours.
#include "pool_growth.h"

#include "ps2_runtime.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "game_version.h" // (TF_GA: the code addresses that differ in the PAL release)

namespace pool_growth
{
    namespace
    {
        constexpr uint32_t kParticleBuffersMake = TF_GA(0x0036A6D0u);
        constexpr uint32_t kImpactManagerCtor = TF_GA(0x0040C8A0u), kImpactManagerCtorEnd = TF_GA(0x0040F4A0u);
        constexpr uint32_t kImpactCtor = TF_GA(0x0040F9B0u);
        constexpr uint32_t kLoopStep = 0x26730001u; // (addiu s3, s3, 1)
        constexpr uint32_t kDisplayBlocksCreate = TF_GA(0x005A8080u);
        constexpr uint32_t kCreatureListsShutdown = TF_GA(0x003771B0u);
        constexpr uint32_t kCreatureListGame = 500u; // (the game's static elements, each vector)
        constexpr uint32_t kCreatureItems = 0x00726700u, kCreatureEntries = 0x00726710u; // (MSL vectors: capacity, count, data)
        constexpr uint32_t kCreatureListMagic = 0x34554C43u;                             // ("CLU4")
        PS2Runtime::RecompiledFunction g_displayBlocksCreate = nullptr, g_creatureListsShutdown = nullptr;
        PS2Runtime *g_runtime = nullptr;

        PS2Runtime::RecompiledFunction g_particleBuffersMake = nullptr, g_impactCtor = nullptr;
        uint32_t g_impactSite = 0u, g_impactCalls = 0u;
        uint64_t g_impactMade = 0u, g_impactLoops = 0u; // (TF_POOL_CENSUS: the impacts the loops made, the loops)

        void setReg(R5900Context *ctx, int reg, uint32_t value)
        {
            ctx->r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(value)));
        }

        void particleBuffersMake(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t count = getRegU32(ctx, 5);
            if (count > 0u && count < 0x1000u)
                setReg(ctx, 5, count * static_cast<uint32_t>(factor()));
            g_particleBuffersMake(rdram, ctx, runtime);
        }

        void impactCtor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = getRegU32(ctx, 31);
            uint32_t next = 0u;
            const bool inLoop = ra >= kImpactManagerCtor && ra < kImpactManagerCtorEnd &&
                                (std::memcpy(&next, rdram + ra + 4u, sizeof(next)), next == kLoopStep);
            g_impactCtor(rdram, ctx, runtime);
            if (!inLoop || ctx->pc != ra)
                return; // (not one of the loops, or stopped at a checkpoint: that pass counts as it is)
            if (ra != g_impactSite)
            {
                g_impactSite = ra;
                g_impactCalls = 0u;
                ++g_impactLoops;
            }
            ++g_impactMade;
            static const bool census = std::getenv("TF_POOL_CENSUS") && *std::getenv("TF_POOL_CENSUS") == '1';
            if (census && ra == TF_GA(0x0040C9FCu) && g_impactCalls == 0u && g_impactLoops > 1u)
                std::fprintf(stderr, "[tf:census] surface impacts made: %llu in %llu loops (the impact manager before this one)\n",
                             static_cast<unsigned long long>(g_impactMade - 1u), static_cast<unsigned long long>(g_impactLoops - 1u));
            if (++g_impactCalls % static_cast<uint32_t>(factor()) != 0u)
                setReg(ctx, 19, getRegU32(ctx, 19) - 1u); // (s3: this pass again)
        }
    }

    namespace
    {
        uint32_t word(uint8_t *rdram, uint32_t a)
        {
            uint32_t v = 0u;
            if (a >= 0x00100000u && a <= PS2_RAM_SIZE - 4u)
                std::memcpy(&v, rdram + a, 4);
            return v;
        }
        void putWord(uint8_t *rdram, uint32_t a, uint32_t v)
        {
            if (a >= 0x00100000u && a <= PS2_RAM_SIZE - 4u)
                std::memcpy(rdram + a, &v, 4);
        }

        void displayBlocksCreate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t bytes = getRegU32(ctx, 4);
            if (bytes > 0u && bytes <= 0x00400000u)
                setReg(ctx, 4, bytes * kDisplayBlocksFactor);
            g_displayBlocksCreate(rdram, ctx, runtime);
        }

        // One vector: its free pointers moved into a bigger array - a block of 16 bytes ("CLU4", the game's array, its
        // capacity, 0), the pointers, then the added elements. False when it is not the game's (or not made).
        bool creatureListGrow(uint8_t *rdram, PS2Runtime *runtime, uint32_t vec, uint32_t elementSize)
        {
            const uint32_t capacity = word(rdram, vec), count = word(rdram, vec + 4u), data = word(rdram, vec + 8u);
            if (data < 0x00100010u || capacity != kCreatureListGame || count > capacity || word(rdram, data - 16u) == kCreatureListMagic)
                return false;
            const uint32_t added = kCreatureListItems - kCreatureListGame;
            const uint32_t block = runtime->guestMalloc(16u + kCreatureListItems * 4u + added * elementSize, 16u);
            if (block == 0u)
            {
                static bool told = false;
                if (!told)
                    std::fprintf(stderr, "[tf] creature list %08x not grown: no room\n", vec);
                told = true;
                return false;
            }
            const uint32_t ptrs = block + 16u, elements = ptrs + kCreatureListItems * 4u;
            std::memset(rdram + block, 0, 16u + kCreatureListItems * 4u + added * elementSize);
            putWord(rdram, block, kCreatureListMagic);
            putWord(rdram, block + 4u, data);
            putWord(rdram, block + 8u, capacity);
            // (the added ones first in the array, the game's free ones last: the vector is taken from its end, so the
            // game's own elements are used first, as before)
            for (uint32_t i = 0; i < added; ++i)
            {
                const uint32_t e = elements + i * elementSize;
                if (elementSize == 16u)
                    rdram[e + 8u] = 1u; // (Rec4: unk_08 = 1, as its constructor)
                putWord(rdram, ptrs + i * 4u, e);
            }
            std::memcpy(rdram + ptrs + added * 4u, rdram + data, count * 4u);
            putWord(rdram, vec, kCreatureListItems);
            putWord(rdram, vec + 4u, added + count);
            putWord(rdram, vec + 8u, ptrs);
            return true;
        }
        // Back to the game's own arrays before it frees them (our blocks freed here).
        void creatureListsShutdown(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            for (const uint32_t vec : {kCreatureItems, kCreatureEntries})
            {
                const uint32_t data = word(rdram, vec + 8u);
                if (data < 16u || word(rdram, data - 16u) != kCreatureListMagic)
                    continue;
                const uint32_t block = data - 16u;
                putWord(rdram, vec + 8u, word(rdram, block + 4u));
                putWord(rdram, vec, word(rdram, block + 8u));
                putWord(rdram, vec + 4u, 0u);
                runtime->guestFree(block);
                std::fprintf(stderr, "[tf] creature list %08x: the game's array given back for the shutdown\n", vec);
            }
            g_creatureListsShutdown(rdram, ctx, runtime);
        }
    }

    namespace
    {
        constexpr uint32_t kActorManager = 0x006CFBD8u;
        constexpr uint32_t kBudgetPs2 = 0x00100000u, kBudgetUnlimited = 0x40000000u;
        std::atomic<bool> g_unlimitedWaves{false};
    }

    void setUnlimitedWaves(bool on)
    {
        const bool allowed = on && PS2Runtime::guestLayout().ramMb > 32u;
        if (on && !allowed)
            std::fprintf(stderr, "[tf] enemy waves: unlimited needs the 256 MB RAM - the PS2's pace kept (TF_RAM_MB=32)\n");
        g_unlimitedWaves.store(allowed);
    }

    bool unlimitedWaves() { return g_unlimitedWaves.load(std::memory_order_relaxed); }

    void frame(uint8_t *rdram)
    {
        if (const uint32_t manager = word(rdram, kActorManager); manager != 0u)
        {
            const uint32_t budget = word(rdram, manager + 8u), want = unlimitedWaves() ? kBudgetUnlimited : kBudgetPs2;
            if (budget != want && (budget == kBudgetPs2 || budget == kBudgetUnlimited)) // (only the game's value, or ours)
            {
                putWord(rdram, manager + 8u, want);
                std::fprintf(stderr, "[tf] enemy waves: %s (the actor budget %u KB)\n",
                             want == kBudgetPs2 ? "the PS2's pace" : "unlimited", want / 1024u);
            }
        }
        if (g_runtime == nullptr) // (the creature lists grown only with the shutdown's wrapper in place)
            return;
        const bool items = creatureListGrow(rdram, g_runtime, kCreatureItems, 44u);
        const bool entries = creatureListGrow(rdram, g_runtime, kCreatureEntries, 16u);
        if (items || entries)
            std::fprintf(stderr, "[tf] the creature lists: room for %u items and %u entries (the game's 500 each; N7 step 4)\n",
                         word(rdram, kCreatureItems), word(rdram, kCreatureEntries));
    }

    int factor()
    {
        static const int f = []()
        {
            if (PS2Runtime::guestLayout().ramMb <= 32u)
                return 1;
            const char *env = std::getenv("TF_POOL_GROW");
            return env && *env ? std::clamp(std::atoi(env), 1, 16) : 16;
        }();
        return f;
    }

    void install(PS2Runtime &runtime)
    {
        // (The emitters' count is in the game's code (patched at recompile time): a save state made with another count
        // holds an array of another size - refused, in both layouts.)
        PS2Runtime::setStateLayoutSalt(0x534E4F4445000000ull | kSoundEmitters); // ("SNODE" and the count)
        if (PS2Runtime::guestLayout().ramMb > 32u)
        {
            if ((g_displayBlocksCreate = runtime.lookupFunction(kDisplayBlocksCreate)) != nullptr)
                runtime.registerFunction(kDisplayBlocksCreate, displayBlocksCreate);
            if ((g_creatureListsShutdown = runtime.lookupFunction(kCreatureListsShutdown)) != nullptr)
            {
                runtime.registerFunction(kCreatureListsShutdown, creatureListsShutdown);
                g_runtime = &runtime;
            }
            std::fprintf(stderr, "[tf] the game's safety limits (N7 step 4): display-list blocks x%u%s, creature lists %u%s\n",
                         kDisplayBlocksFactor, g_displayBlocksCreate ? "" : " (not found)", kCreatureListItems,
                         g_creatureListsShutdown ? "" : " (not found)");
        }
        if (factor() <= 1)
            return;
        if ((g_particleBuffersMake = runtime.lookupFunction(kParticleBuffersMake)) != nullptr)
            runtime.registerFunction(kParticleBuffersMake, particleBuffersMake);
        if ((g_impactCtor = runtime.lookupFunction(kImpactCtor)) != nullptr)
            runtime.registerFunction(kImpactCtor, impactCtor);
        std::fprintf(stderr, "[tf] the game's look-and-sound pools x%d (N7 step 3): particle buffers%s, surface impacts%s\n", factor(),
                     g_particleBuffersMake ? "" : " (not found)", g_impactCtor ? "" : " (not found)");
    }
}
