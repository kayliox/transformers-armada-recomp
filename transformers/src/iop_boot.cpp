// The IOP-less boot (iop_boot.h): the "emulation out" plan's step 2 (S2), opt-in (TF_IOP_BOOT=0).
//
// What the game asks of the IOP, measured in S1 (iop_tripwire.cpp, every episode, PAL, movies, states): at boot it
// reboots it (sceSifRebootIop), syncs, loads 8 modules through _sceSifLoadModuleBuffer - TWOFISH, SIO2MAN, LIBSD,
// MODMIDI, SIRENS3, MCMAN, MCSERV, PADMAN, each DMA'd into IOP memory from a buffer of sceSifAllocSysMemory, freed after -
// and binds one RPC client, the Sirens sound server's; after that nothing. With the native sound server
// (native_sound.cpp: its commands answered on the EE) and the native disc (native_disc.cpp: TwoFish's reads answered on
// the host) none of the modules' work is used, and the pad and the memory card are the runtime's own (no binds).
// So here:
//   - the IOP is parked from the start (its processor is given no cycles) and again after the reboot (which resets it);
//   - each module load is answered as the IOP's own load answers it - the module ids in order, the start results the
//     modules give (MCMAN and MCSERV 2, REMOVABLE_RESIDENT_END; the others 0) - without loading the module;
//   - TwoFish's "ready" is the game's own: its SIF command 1 (magic 0xF154F154, bytes 6 and 1 - what TwoFish sends from
//     its start routine) delivered to the handler the game registered (0x192EC0), as the IOP's send delivers it;
//   - the Sirens bind gets a server descriptor made here (sid set, no buffers), as the runtime does for its own routes:
//     the SDK's bind loops until the client has a server.
// The IOP's memory model stays: the system memory the game allocates and the DMA into it work as before. A module this
// does not know is loaded on the IOP as before (said in the log). Decomp equivalent: the EE's boot without an IOP.

#include "iop_boot.h"

#include "native_disc.h"
#include "native_sound.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ps2_stubs
{
    // (the runtime's: an IOP-to-EE SIF command delivered to the handler the EE registered, as the IOP's send does)
    bool dispatchSifCommand(uint8_t *rdram, PS2Runtime *runtime, uint32_t commandId, const void *packet, size_t packetSize) noexcept;
}

namespace iop_boot
{
    namespace
    {
        constexpr uint32_t kBindRpc = 0x001225D8u, kRebootIop = 0x00127A68u;
        constexpr uint32_t kClientServer = 36u; // (t_SifRpcClientData: header 16, command, buf, cbuf, end_function, end_param, server)
        constexpr uint32_t kClientBuf = 20u, kClientCbuf = 24u;

        bool g_active = false;
        int32_t g_nextId = 0;
        uint32_t g_sirensServer = 0u; // (the descriptor made for the Sirens bind, guest memory)
        PS2Runtime::RecompiledFunction g_bind = nullptr, g_reboot = nullptr;

        struct Known
        {
            const char *match; // (in the module's name, case ignored)
            const char *label;
            int32_t start;     // (its start routine's result on the IOP)
            bool twoFish;
        };
        // (the order the game loads them; the results measured on the IOP's own loads, S1's trace)
        constexpr Known kModules[] = {
            {"twofish", "TWOFISH", 0, true},  {"sio2", "SIO2MAN", 0, false}, {"sound_device", "LIBSD", 0, false},
            {"midi", "MODMIDI", 0, false},    {"sirens", "SIRENS3", 0, false}, {"mcman", "MCMAN", 2, false},
            {"mcserv", "MCSERV", 2, false},   {"pad", "PADMAN", 0, false},
        };

        uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
        uint16_t le16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

        // The IRX's own name: its .iopmod section (moduleinfo, entry, gp, text/data/bss sizes, a u16 version, then the
        // name).
        std::string irxName(PS2Runtime &runtime, uint32_t at)
        {
            uint8_t eh[52];
            if (!runtime.readIopMemory(at, eh, sizeof(eh)) || std::memcmp(eh, "\x7F" "ELF", 4) != 0)
                return {};
            const uint32_t shoff = le32(eh + 32);
            const uint16_t shentsize = le16(eh + 46), shnum = le16(eh + 48), shstrndx = le16(eh + 50);
            if (shentsize < 40u || shnum == 0u || shnum > 64u || shstrndx >= shnum)
                return {};
            std::vector<uint8_t> sh(static_cast<size_t>(shnum) * shentsize);
            if (!runtime.readIopMemory(at + shoff, sh.data(), sh.size()))
                return {};
            const uint8_t *strHdr = sh.data() + static_cast<size_t>(shstrndx) * shentsize;
            std::vector<char> names(std::min<uint32_t>(le32(strHdr + 20), 4096u) + 1u, '\0');
            if (!runtime.readIopMemory(at + le32(strHdr + 16), names.data(), names.size() - 1u))
                return {};
            for (uint16_t i = 0; i < shnum; ++i)
            {
                const uint8_t *h = sh.data() + static_cast<size_t>(i) * shentsize;
                const uint32_t nameAt = le32(h);
                if (nameAt >= names.size() || std::strcmp(names.data() + nameAt, ".iopmod") != 0)
                    continue;
                char info[96] = {};
                const uint32_t size = std::min<uint32_t>(le32(h + 20), sizeof(info) - 1u);
                if (size <= 0x1Au || !runtime.readIopMemory(at + le32(h + 16), info, size))
                    return {};
                return std::string(info + 0x1A, strnlen(info + 0x1A, size - 0x1Au));
            }
            return {};
        }

        const Known *knownFor(const std::string &name)
        {
            std::string lower(name);
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            for (const Known &k : kModules)
                if (lower.find(k.match) != std::string::npos)
                    return &k;
            return nullptr;
        }

        // TwoFish's "ready" (its start routine's SIF command 1): the game's handler stores the packet's bytes at 0x10 and 0x11
        // (the server's 6 and 1) once the magic at 0x0C is TwoFish's.
        void sendTwoFishReady(PS2Runtime &runtime, uint8_t *rdram)
        {
            uint8_t packet[20] = {};
            const uint32_t words[5] = {20u, 0u, 1u, 0xF154F154u, 0x00000106u}; // (size, extra destination, cid, magic; 6, 1)
            std::memcpy(packet, words, sizeof(packet));
            if (!ps2_stubs::dispatchSifCommand(rdram, &runtime, 1u, packet, sizeof(packet)))
                std::fprintf(stderr, "[iop boot] TwoFish's ready could not be delivered (no handler for SIF command 1): the disc setup will fail\n");
        }

        void bindHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t client = getRegU32(ctx, 4), sid = getRegU32(ctx, 5);
            g_bind(rdram, ctx, runtime);
            if (!g_active)
                return;
            uint8_t *c = getMemPtr(rdram, client);
            if (!c)
                return;
            uint32_t server = 0u;
            std::memcpy(&server, c + kClientServer, 4);
            if (server != 0u)
                return;
            // (the game binds one server, the Sirens sound server's - S1's trace on both discs; another is told, and bound the
            // same way: a client left without a server would loop in the SDK's bind for ever)
            static uint32_t firstSid = sid;
            if (sid != firstSid)
            {
                static int told = 0;
                if (told++ < 8)
                    std::fprintf(stderr, "[iop boot] a second server bound (%08x, the first %08x): given the same kind of descriptor\n", sid, firstSid);
            }
            if (g_sirensServer == 0u)
            {
                g_sirensServer = runtime->guestMalloc(80u, 16u);
                if (uint8_t *s = g_sirensServer ? getMemPtr(rdram, g_sirensServer) : nullptr)
                {
                    std::memset(s, 0, 80u);
                    std::memcpy(s, &sid, 4);
                }
            }
            const uint32_t zero = 0u;
            std::memcpy(c + kClientServer, &g_sirensServer, 4);
            std::memcpy(c + kClientBuf, &zero, 4);
            std::memcpy(c + kClientCbuf, &zero, 4);
        }

        void rebootHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_reboot(rdram, ctx, runtime);
            if (g_active && runtime)
                runtime->setIopParked(true); // (the reboot resets the IOP, its park with it)
        }
    }

    void install(PS2Runtime &runtime)
    {
        const char *want = std::getenv("TF_IOP_BOOT");
        if (!want || *want != '0')
            return;
        const char *iopSound = std::getenv("TF_NATIVE_SOUND_IOP");
        if (!native_sound::active() || !native_disc::active() || (iopSound && *iopSound == '1'))
        {
            std::fprintf(stderr, "[iop boot] TF_IOP_BOOT=0 asks for a boot without the IOP, but %s: the IOP boots as before\n",
                         !native_sound::active() ? "the sound server is the IOP's (TF_NATIVE_SOUND=0)"
                         : !native_disc::active() ? "the disc is read through the IOP's TwoFish (TF_NATIVE_DISC=0)"
                                                  : "TF_NATIVE_SOUND_IOP=1 keeps the IOP");
            return;
        }
        g_bind = runtime.lookupFunction(kBindRpc);
        g_reboot = runtime.lookupFunction(kRebootIop);
        if (!g_bind || !g_reboot)
        {
            std::fprintf(stderr, "[iop boot] the SIF library's bind or reboot is not here: the IOP boots as before\n");
            return;
        }
        runtime.registerFunction(kBindRpc, bindHook);
        runtime.registerFunction(kRebootIop, rebootHook);
        g_active = true;
        runtime.setIopParked(true);
        std::fprintf(stderr, "[iop boot] the game boots without the IOP: its modules answered here, its processor never started "
                             "(TF_IOP_BOOT=0; unset: the IOP's boot)\n");
    }

    bool active() { return g_active; }

    bool takeModule(PS2Runtime &runtime, uint8_t *rdram, uint32_t iopAddr, int32_t &moduleId, int32_t &startResult)
    {
        if (!g_active)
            return false;
        const std::string name = irxName(runtime, iopAddr);
        const Known *k = knownFor(name);
        if (!k)
        {
            std::fprintf(stderr, "[iop boot] a module this boot does not know (\"%s\", IOP %08x): loaded on the IOP\n", name.c_str(), iopAddr);
            return false;
        }
        moduleId = ++g_nextId;
        startResult = k->start;
        std::fprintf(stderr, "[iop boot] %s (\"%s\") answered without loading it: module %d, start %d\n", k->label, name.c_str(), moduleId,
                     startResult);
        if (k->twoFish)
            sendTwoFishReady(runtime, rdram);
        return true;
    }

    void stateLoaded(PS2Runtime &runtime)
    {
        const uint32_t modules = runtime.iopDebugSnapshot().emulatorLoadedModules;
        if (!g_active && modules == 0u)
        {
            if (!native_sound::active() || !native_disc::active())
                std::fprintf(stderr, "[iop boot] this save state was made without the IOP (TF_IOP_BOOT=0), and this game's %s the IOP's: "
                                     "it has no sound server or disc server to run - start with TF_NATIVE_SOUND=1 and TF_NATIVE_DISC=1, "
                                     "or make the state again\n",
                             !native_sound::active() ? "sound server is" : "disc is read through");
            else
                std::fprintf(stderr, "[iop boot] this save state was made without the IOP (TF_IOP_BOOT=0): the native sound server and "
                                     "disc serve it as they would\n");
        }
        else if (g_active && modules != 0u)
            std::fprintf(stderr, "[iop boot] this save state was made with the IOP booted (%u modules): they stay parked, nothing runs them\n",
                         modules);
        if (g_active)
            runtime.setIopParked(true);
    }
}
