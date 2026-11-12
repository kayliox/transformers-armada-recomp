// The IOP's tripwire (iop_tripwire.h): the "emulation out" plan's step 1 (S1) - a record, nothing changed.
//
// Wraps the game's SIF library functions that reach the IOP (the addresses are the SDK's, below the range the PAL
// release moves, so the same on both discs) and counts each call: before the IOP is parked (the boot) and after
// it. A call after the park is told at once (up to 32 lines, then counted) - with the native sound server and the
// native disc nothing should reach the IOP any more; what does is what an IOP-less boot (S2) has to answer. The DMA
// into IOP memory goes through a syscall the port does not see here: PS2X_SIF_LOG=1 shows it (the IOP's own log).
//
// At the park (native_sound.cpp) one line: the host's time since start, the IOP's own time (its emulated cycles), and
// the calls of the boot; at the game's end (main.cpp) the whole count. TF_IOP_TRIPWIRE=0: not installed.

#include "iop_tripwire.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <utility>

namespace iop_tripwire
{
    namespace
    {
        struct Hook
        {
            uint32_t address;
            const char *name;
        };
        constexpr std::array<Hook, 20> kHooks{{
            {0x001225D8u, "sceSifBindRpc"},
            {0x001227A8u, "sceSifCallRpc"},
            {0x00122320u, "sceSifGetOtherData"},
            {0x00121BC8u, "sceSifSendCmd"},
            {0x00121C08u, "isceSifSendCmd"},
            {0x00127310u, "_sceSifLoadModule"},
            {0x00126D00u, "_sceSifLoadModuleBuffer"},
            {0x00127578u, "_sceSifLoadElfPart"},
            {0x00126F08u, "sceSifStopModule"},
            {0x00127110u, "sceSifUnloadModule"},
            {0x00126670u, "sceSifAllocIopHeap"},
            {0x001266E0u, "sceSifAllocSysMemory"},
            {0x00126760u, "sceSifFreeSysMemory"},
            {0x001267D8u, "sceSifFreeIopHeap"},
            {0x001267F8u, "sceSifLoadIopHeap"},
            {0x001277B8u, "sceSifSetIopAddr"},
            {0x001276C8u, "sceSifGetIopAddr"},
            {0x00127898u, "sceSifResetIop"},
            {0x00127A18u, "sceSifSyncIop"},
            {0x00127A68u, "sceSifRebootIop"},
        }};

        const auto g_start = std::chrono::steady_clock::now(); // (the module's start: the process's, near enough)
        std::array<PS2Runtime::RecompiledFunction, kHooks.size()> g_next{};
        std::array<std::atomic<uint64_t>, kHooks.size()> g_before{}, g_after{};
        std::atomic<bool> g_parked{false};
        std::atomic<int> g_told{0};
        bool g_installed = false;

        void note(size_t i, R5900Context *ctx, PS2Runtime *runtime)
        {
            // (the park is native sound's, at the server's start: the boot up to it - with the IOP-less boot, iop_boot.cpp, the
            // IOP is parked from the start, and its boot is still the boot)
            if (!g_parked.load(std::memory_order_relaxed))
            {
                g_before[i].fetch_add(1u, std::memory_order_relaxed);
                return;
            }
            g_after[i].fetch_add(1u, std::memory_order_relaxed);
            if (g_told.fetch_add(1, std::memory_order_relaxed) < 32)
                std::fprintf(stderr, "[iop tripwire] %s after the IOP was parked (vblank %llu, called from %08x; a0 %08x a1 %08x)\n",
                             kHooks[i].name, runtime ? static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()) : 0ull,
                             getRegU32(ctx, 31) - 8u, getRegU32(ctx, 4), getRegU32(ctx, 5));
        }

        // (the host's time inside the calls, the IOP's runs they wait for included: the IOP's share of the boot)
        std::atomic<uint64_t> g_insideNs{0};

        template <size_t I>
        void hook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            note(I, ctx, runtime);
            const auto begun = std::chrono::steady_clock::now();
            g_next[I](rdram, ctx, runtime);
            g_insideNs.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begun).count()),
                                 std::memory_order_relaxed);
        }

        template <size_t I>
        void installOne(PS2Runtime &runtime)
        {
            g_next[I] = runtime.lookupFunction(kHooks[I].address);
            if (g_next[I])
                runtime.registerFunction(kHooks[I].address, &hook<I>);
        }

        template <size_t... I>
        void installAll(PS2Runtime &runtime, std::index_sequence<I...>)
        {
            (installOne<I>(runtime), ...);
        }

        std::string counts(const std::array<std::atomic<uint64_t>, kHooks.size()> &c)
        {
            std::string s;
            for (size_t i = 0; i < kHooks.size(); ++i)
                if (const uint64_t n = c[i].load(std::memory_order_relaxed))
                {
                    char item[96];
                    std::snprintf(item, sizeof(item), "%s%s %llu", s.empty() ? "" : ", ", kHooks[i].name, static_cast<unsigned long long>(n));
                    s += item;
                }
            return s.empty() ? std::string("none") : s;
        }

    }

    void summary()
    {
        if (g_installed)
            std::fprintf(stderr, "[iop tripwire] the session's calls to the IOP: before the park %s; after it %s\n",
                         counts(g_before).c_str(), counts(g_after).c_str());
    }

    void install(PS2Runtime &runtime)
    {
        if (const char *e = std::getenv("TF_IOP_TRIPWIRE"); e && *e == '0')
            return;
        installAll(runtime, std::make_index_sequence<kHooks.size()>{});
        size_t missing = 0;
        for (size_t i = 0; i < kHooks.size(); ++i)
            if (!g_next[i])
            {
                ++missing;
                std::fprintf(stderr, "[iop tripwire] %s (%08x) is not a function here: not watched\n", kHooks[i].name, kHooks[i].address);
            }
        g_installed = true;
        std::fprintf(stderr, "[iop tripwire] watching %zu of the SIF library's IOP calls (TF_IOP_TRIPWIRE=0: off)\n", kHooks.size() - missing);
    }

    void noteParked(PS2Runtime &runtime)
    {
        if (!g_installed || g_parked.exchange(true))
            return;
        const double hostMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_start).count();
        const auto snap = runtime.iopDebugSnapshot();
        std::fprintf(stderr,
                     "[iop tripwire] the IOP parked %.0f ms after start (vblank %llu), %.1f ms of it inside the calls below; it had run "
                     "%.3f s of its own time (%llu cycles, %llu instructions), %u modules, %u threads, %u RPC servers; the boot's calls to it: %s\n",
                     hostMs, static_cast<unsigned long long>(runtime.eeScheduler().currentVSyncTick()),
                     static_cast<double>(g_insideNs.load(std::memory_order_relaxed)) / 1.0e6,
                     static_cast<double>(snap.emulatorCycles) / 36'864'000.0, static_cast<unsigned long long>(snap.emulatorCycles),
                     static_cast<unsigned long long>(snap.emulatorInstructions), snap.emulatorLoadedModules, snap.emulatorThreads,
                     snap.emulatorRpcServers, counts(g_before).c_str());
    }
}
