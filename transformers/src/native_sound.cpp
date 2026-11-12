// The native Sirens sound server (native_sound.h): the protocol side.
//
// The game's sound classes (sl*_PlayStation2) talk to the IOP's SIRENS3 server through
// slSIFCommander. Three of its functions are replaced, so nothing reaches the IOP:
//   0x1F0470 SendAsynchronousCommand(command, argsSize, resultsSize): the commands. The arguments
//            are in slSIFCommander's argument buffer, the results go to its results buffer, in
//            the transport's encoding (each value aligned to its own size; a string is a u16
//            length and the bytes). Command 4 is the frame packet (tags, below).
//   0x1F0170 SendAsynchronousMemoryRequest(dest, src, size): the EE's pull of the server's
//            status buffer (the address command 7 gave): written here from the engine.
//   0x1E8B20 the device's EE-to-IOP transfer start (movie audio into the stream's landing
//            buffer): the 8 KB go to the engine's stream, the slot is filled as the original does,
//            without the DMA (the runtime's sceSifDmaStat reads every transfer as done, so the
//            device announces it with tag 0x46 the next frame as usual).
// The engine (native_sound_mixer.cpp) does what the server does with them, and mixes into the
// port's audio stream; the IOP is parked once the server has started (it has nothing else to do
// in this game: pad, memory card and files are the runtime's own).
// What each command, packet tag and status field means: see TF_SIRENS_LOG / TF_SIRENS_SCHEMA
// (sirens_trace.cpp), which record what the game sends.
//
// Time is the game's own EE clock (0x108780's count), so the sound follows pauses, loads and
// frame rates as the game's own sound timing does.

#include "native_sound.h"

#include "guest_code.h"
#include "native_disc.h"
#include "native_sound_mixer.h"
#include "sound_ledger.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_audio_stream.h"
#include "runtime/ps2_settings.h"
#include "runtime/ps2_thread_placement.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace native_sound
{
    namespace
    {
        // The mixing thread (2026-10-23; TF_NATIVE_SOUND_THREAD=0: on the EE's): the frame packet's
        // mix (up to the EE clock's time then) is done on a thread of its own, the EE going on with
        // the game - 4% of the EE's time in a busy level (Deep Amazon, profiled), which the
        // in-between pictures need there. Nothing else touches the mixer meanwhile: every way into
        // it (the commands, the status, the transfers, a state loaded, the port's own voices) first
        // waits for the mix to be done (waitMix), and the packets come once a frame, so the EE
        // rarely waits. With a stream the EE feeds (a movie's audio) the mix stays on the EE: the
        // credits it grants come from what the mix consumed, in the same packet.
        std::mutex g_mixMutex;
        std::condition_variable g_mixCv;
        bool g_mixPending = false;
        double g_mixTo = 0.0;
        bool g_mixThreaded = false;
        // Held by whoever is in the mixer: an EE's way in for its whole call (MixerScope), the
        // mixing thread for a mix. The thread also mixes on its own (runAhead), so the EE's ways in
        // no longer only wait for the posted mix: they hold this while they change the engine.
        std::mutex g_access;

        void waitMix()
        {
            if (!g_mixThreaded)
                return;
            std::unique_lock<std::mutex> lock(g_mixMutex);
            g_mixCv.wait(lock, [] { return !g_mixPending; });
        }

        // An EE's way into the mixer: the frame's posted mix done, then the mixer to itself.
        struct MixerScope
        {
            MixerScope()
            {
                waitMix();
                g_access.lock();
            }
            ~MixerScope() { g_access.unlock(); }
            MixerScope(const MixerScope &) = delete;
            MixerScope &operator=(const MixerScope &) = delete;
        };

        void runAhead(); // (below: it reads the streams the EE feeds)

        // The mixing thread: the frame's mix when one is posted, and every few milliseconds a look
        // at the output (runAhead).
        void mixLoop()
        {
            ps2_thread_placement::place(ps2_thread_placement::Role::Helper); // (PS2X_THREAD_PLACEMENT)
            std::unique_lock<std::mutex> lock(g_mixMutex);
            for (;;)
            {
                g_mixCv.wait_for(lock, std::chrono::milliseconds(4), [] { return g_mixPending; });
                if (!g_mixPending)
                {
                    lock.unlock();
                    runAhead();
                    lock.lock();
                    continue;
                }
                const double to = g_mixTo;
                lock.unlock();
                {
                    std::lock_guard<std::mutex> access(g_access);
                    mixer::render(to);
                }
                lock.lock();
                g_mixPending = false;
                g_mixCv.notify_all();
            }
        }

        // The mix up to `seconds`: on the mixing thread, or here (`here`).
        void postMix(double seconds, bool here)
        {
            if (!g_mixThreaded || here)
            {
                waitMix();
                mixer::render(seconds);
                return;
            }
            {
                std::lock_guard<std::mutex> lock(g_mixMutex);
                g_mixTo = seconds;
                g_mixPending = true;
            }
            g_mixCv.notify_all();
        }

        constexpr uint32_t kSendCommand = 0x001F0470u, kMemoryRequest = 0x001F0170u, kTransferStart = 0x001E8B20u;
        constexpr uint32_t kCommander = 0x006CE8F4u; // slSIFCommander*: +0x18 arguments buffer, +0x1C results buffer
        constexpr uint32_t kCredits = 0x006CE8E4u;   // the stream-data credit counter (the IOP's SIF command 4 adds)
        constexpr uint32_t kClockCycles = 0x006CE5B0u, kSecondsPerCycle = 0x006CB1A4u; // the EE clock (0x108780)
        constexpr uint32_t kVersion = 0x311500AAu;   // SIRENS3's version (the EE needs >= 0x311500A7)
        constexpr uint32_t kStatusAddress = 0x00111A00u, kStatusSize = 0x2000u; // where the IOP keeps its status
        constexpr uint32_t kLandingBuffer = 0x00131240u; // a stream's DMA buffer (never written: no DMA)
        constexpr uint32_t kStatusEnd = 0xCAFEF00Du;
        constexpr int kPlayingSlots = 48;

        enum Command : uint32_t
        {
            Sys_VersionQuery = 1,
            Sys_Startup = 2,
            Sys_FramePacket = 4,
            Sys_QueryStatus = 7,
            Master_SilenceVoices = 12,
            VoiceImmediate_Create = 14,
            VoiceImmediate_Destroy = 15,
            VoiceImmediate_SetTone = 17,
            ToneLibrary_Create = 23,
            ToneLibrary_Destroy = 24,
            ToneLibrary_StringCommand = 25,
            SoundStream_Create = 29,
            SoundStream_Destroy = 30,
            SoundStream_AllocateDMABuffer = 31,
            SoundStream_Command = 32,
            SoundStream_ReserveChannels = 37,
            TDG_Fish_Command = 40,
        };

        bool g_active = false;
        bool g_trace = false;
        // The save-state record (installState): "NSS1", u32 length, the setup text.
        constexpr uint32_t kRecordMagic = 0x3153534Eu, kRecordSize = 0x8000u;
        uint32_t g_record = 0u;
        uint64_t g_recordedAt = 0u;
        int g_packets = 0;
        void writeRecord(uint8_t *rdram);
        PS2Runtime *g_runtime = nullptr;
        bool g_iopParked = false;
        std::map<int32_t, int> g_reservedChannels; // per stream
        std::map<int32_t, bool> g_eeFedStreams;

        // The output kept from running dry while the game is behind real time (TODO 60/62): the
        // mix follows the game's EE clock, so a frame that hitches (the renderer uploading a level's
        // textures, a frame rate the host does not hold) mixed nothing until the game went on, and
        // the sound card played the buffer empty - every playing sound cut for a moment. When the
        // output holds under half of its ~80 ms, the mixing thread mixes on by real time up to its
        // ~80 ms (the voices and streams go on as the console's SPU2 does, whatever the EE is
        // doing; the time the hitch cost the game is lost to it for good: mixer::renderAhead).
        // In normal play the output never gets that low, so nothing changes there (no added
        // latency). Not while a stream the EE feeds plays (a movie's audio: its credits come from
        // what the frame's own mix consumed), nor while the output is off or muted (a fast load).
        // TF_NATIVE_SOUND_AHEAD=0: never.
        void runAhead()
        {
            static const bool on = []
            {
                const char *env = std::getenv("TF_NATIVE_SOUND_AHEAD");
                return !(env && *env == '0');
            }();
            if (!on || !g_access.try_lock())
                return; // (the EE is in the mixer: its own mix comes next)
            std::lock_guard<std::mutex> access(g_access, std::adopt_lock);
            for (const auto &[handle, eeFed] : g_eeFedStreams)
                if (eeFed)
                    return;
            const long long held = ps2_audio_stream::bufferedFrames();
            const long long target = static_cast<long long>(ps2_audio_stream::targetFrames());
            if (held < 0 || held >= target / 2)
                return;
            mixer::renderAhead(static_cast<size_t>(target - held));
        }

        uint8_t *g_rdram = nullptr;

        uint32_t read32(uint32_t a)
        {
            uint32_t v = 0u;
            if (const uint8_t *p = getConstMemPtr(g_rdram, a))
                std::memcpy(&v, p, 4);
            return v;
        }
        void write32(uint32_t a, uint32_t v)
        {
            if (uint8_t *p = getMemPtr(g_rdram, a))
                std::memcpy(p, &v, 4);
        }
        uint8_t read8(uint32_t a)
        {
            const uint8_t *p = getConstMemPtr(g_rdram, a);
            return p ? *p : 0u;
        }
        float readf(uint32_t a)
        {
            const uint32_t v = read32(a);
            float f;
            std::memcpy(&f, &v, 4);
            return f;
        }

        double now()
        {
            const uint64_t cycles = static_cast<uint64_t>(read32(kClockCycles)) | (static_cast<uint64_t>(read32(kClockCycles + 4u)) << 32);
            float perCycle = 0.0f;
            const uint32_t bits = read32(kSecondsPerCycle);
            std::memcpy(&perCycle, &bits, 4);
            return static_cast<double>(cycles) * perCycle;
        }

        void grantCredits(int n)
        {
            if (n > 0)
                write32(kCredits, read32(kCredits) + static_cast<uint32_t>(n));
        }

        // The transport's encoding: each value aligned to its own size; a string is a u16 length
        // and the bytes.
        struct Arguments
        {
            uint32_t at, end;

            void align(uint32_t size) { at = (at + size - 1u) & ~(size - 1u); }
            uint32_t u32()
            {
                align(4u);
                const uint32_t v = at + 4u <= end ? read32(at) : 0u;
                at += 4u;
                return v;
            }
            int32_t i32() { return static_cast<int32_t>(u32()); }
            float f32()
            {
                const uint32_t v = u32();
                float f;
                std::memcpy(&f, &v, 4);
                return f;
            }
            bool b()
            {
                const bool v = at < end && read8(at) != 0u;
                at += 1u;
                return v;
            }
            std::string str()
            {
                align(2u);
                const uint32_t length = at + 2u <= end ? (read8(at) | (read8(at + 1u) << 8)) : 0u;
                at += 2u;
                std::string s;
                for (uint32_t i = 0u; i < length && at + i < end; ++i)
                    s += static_cast<char>(read8(at + i));
                at += length;
                return s;
            }
        };

        // The frame packet: 32-bit words; a string is a u8 length (at most 250) and the bytes,
        // padded so that 1 + length is a multiple of 4.
        struct Packet
        {
            uint32_t at, end;

            bool more() const { return at + 4u <= end; }
            uint32_t u32()
            {
                const uint32_t v = at + 4u <= end ? read32(at) : 0u;
                at += 4u;
                return v;
            }
            int32_t i32() { return static_cast<int32_t>(u32()); }
            float f32()
            {
                const uint32_t v = u32();
                float f;
                std::memcpy(&f, &v, 4);
                return f;
            }
            std::string str()
            {
                const uint32_t length = at < end ? read8(at) : 0u;
                std::string s;
                for (uint32_t i = 0u; i < length && at + 1u + i < end; ++i)
                    s += static_cast<char>(read8(at + 1u + i));
                at += (1u + length + 3u) & ~3u;
                return s;
            }
        };

        void updateReserved()
        {
            int total = 0;
            for (const auto &[h, n] : g_reservedChannels)
                total += n;
            mixer::setReservedChannels(total);
        }

        // TF_NATIVE_SOUND_POOLS=1 (a diagnostic for item 37, run once a frame packet): the level's
        // projectile pools (the manager at gp-0x38B8, 0x6CF838: pool = manager + type x 20 + 0x14,
        // its lists at +0x14: free +0, used +4, linked through +0x20) - per type the fewest free
        // and the most in use over each 2 s, and every time one runs empty.
        void reportPools()
        {
            static const bool on = [] {
                const char *env = std::getenv("TF_NATIVE_SOUND_POOLS");
                return env && *env == '1';
            }();
            if (!on)
                return;
            constexpr uint32_t kManager = 0x006CF838u;
            struct Seen
            {
                uint32_t minFree = ~0u, maxUsed = 0u;
                bool empty = false;
            };
            static std::map<uint32_t, Seen> seen;
            static double nextReport = 0.0;
            const auto count = [](uint32_t at) {
                uint32_t n = 0u;
                for (; at && n < 1024u; at = read32(at + 0x20u))
                    ++n;
                return n;
            };
            const uint32_t manager = read32(kManager);
            for (uint32_t type = 0u; manager && type < 64u; ++type)
            {
                const uint32_t info = read32(manager + type * 20u + 0x14u);
                const uint32_t lists = info ? read32(info + 0x14u) : 0u;
                if (!lists)
                    continue;
                const uint32_t free = count(read32(lists)), used = count(read32(lists + 4u));
                Seen &s = seen[type];
                if (free == 0u && !s.empty)
                    std::fprintf(stderr, "[ns pools] %.3f type %u ran EMPTY (%u in use)\n", now(), type, used);
                s.empty = free == 0u;
                s.minFree = std::min(s.minFree, free);
                s.maxUsed = std::max(s.maxUsed, used);
            }
            if (now() >= nextReport && !seen.empty())
            {
                nextReport = now() + 2.0;
                std::string line;
                char b[48];
                for (auto &[type, s] : seen)
                {
                    std::snprintf(b, sizeof(b), " %u:%u/%u", type, s.minFree, s.maxUsed);
                    line += b;
                    s.minFree = ~0u;
                    s.maxUsed = 0u;
                }
                std::fprintf(stderr, "[ns pools] %.3f type:fewest free/most in use%s\n", now(), line.c_str());
            }
        }

        // ---- the frame packet ---------------------------------------------------------------
        // TF_NATIVE_SOUND_STREAMS=1: every stream command of the frame packets (loudness, channel
        // loudness, open, play, stop, tune) and every stream made or destroyed, with the EE clock -
        // for following a stream's fade (the HQ's music after a level, TODO 66).
        bool streamLog()
        {
            static const bool on = std::getenv("TF_NATIVE_SOUND_STREAMS") != nullptr;
            return on;
        }

        // A packet's key-ons are made at its channel assignment (tag 0x25, where SIRENS3 assigns its
        // channels: IOP 0x4DF9C), after the packet's voice updates, the most important first (TODO
        // 106). The EE sends a new voice's play (and rewind) BEFORE its first update in the same
        // packet: keyed at once it had priority 0 and no volume yet when the channels were weighed.
        // TF_NATIVE_SOUND_DEFER=0: keyed where the tag stands, as before.
        bool deferKeys()
        {
            static const bool on = [] {
                const char *env = std::getenv("TF_NATIVE_SOUND_DEFER");
                return !(env && *env == '0');
            }();
            return on;
        }

        void framePacket(uint32_t args, uint32_t size)
        {
            reportPools();
            Packet p{args, args + size};
            while (p.more())
            {
                const uint32_t tag = p.u32();
                switch (tag)
                {
                case 0x01: // end
                    p.at = p.end;
                    break;
                case 0x02: // the noise clocks
                    p.u32();
                    p.u32();
                    break;
                case 0x20: // play
                case 0x24: // rewind (sent with the play)
                    if (deferKeys())
                        mixer::queuePlay(p.i32());
                    else if (tag == 0x20)
                        mixer::play(p.i32());
                    else
                        mixer::rewind(p.i32());
                    break;
                case 0x21: // stop (key off: the release runs)
                {
                    const int32_t handle = p.i32();
                    mixer::forgetPlay(handle);
                    mixer::stop(handle);
                    break;
                }
                case 0x22: // the voice update: handle, u16 volL, u16 volR, pitch, priority, reverb send, dry
                {
                    const int32_t handle = p.i32();
                    const uint32_t volumes = p.u32();
                    const float pitch = p.f32();
                    const int32_t priority = p.i32();
                    const float send = p.f32();
                    const float dry = p.f32();
                    mixer::updateVoice(handle, static_cast<uint16_t>(volumes), static_cast<uint16_t>(volumes >> 16), pitch, priority, send, dry);
                    break;
                }
                case 0x23: // SetTone by name
                {
                    const int32_t handle = p.i32();
                    mixer::setTone(handle, p.str());
                    break;
                }
                case 0x25: // sort and assign the channels: the packet's key-ons are made here
                    mixer::assignChannels();
                    break;
                case 0x30: // reverb return level
                    mixer::setReverbLevel(p.f32());
                    break;
                case 0x31: // reverb preset
                    mixer::setReverbPreset(p.str());
                    break;
                case 0x32:
                    mixer::setReverbDelay(p.f32());
                    break;
                case 0x33:
                    mixer::setReverbFeedback(p.f32());
                    break;
                case 0x40: // stream loudness
                {
                    const int32_t handle = p.i32();
                    const uint32_t loudness = p.u32();
                    if (streamLog())
                        std::fprintf(stderr, "[ns stream] %.3f %d loudness %u\n", now(), handle, loudness);
                    mixer::setStreamLoudness(handle, loudness);
                    break;
                }
                case 0x41: // open: an .MS file or an album
                {
                    const int32_t handle = p.i32();
                    const std::string name = p.str();
                    if (g_trace)
                        std::fprintf(stderr, "[native sound] stream %d open '%s'\n", handle, name.c_str());
                    mixer::openStream(handle, name);
                    break;
                }
                case 0x42:
                case 0x43:
                case 0x44:
                case 0x47:
                {
                    const int32_t handle = p.i32();
                    if (streamLog())
                        std::fprintf(stderr, "[ns stream] %.3f %d %s\n", now(), handle,
                                     tag == 0x42 ? "close" : tag == 0x43 ? "play" : tag == 0x44 ? "stop" : "flush");
                    if (tag == 0x42)
                        mixer::closeStream(handle);
                    else if (tag == 0x43)
                        mixer::playStream(handle);
                    else if (tag == 0x44)
                        mixer::stopStream(handle);
                    else
                        mixer::flushStream(handle);
                    break;
                }
                case 0x45: // switch tune: handle, immediately, tune
                {
                    const int32_t handle = p.i32();
                    const bool immediately = p.u32() != 0u;
                    const std::string tune = p.str();
                    if (streamLog())
                        std::fprintf(stderr, "[ns stream] %.3f %d tune '%s'%s\n", now(), handle, tune.c_str(), immediately ? " now" : "");
                    mixer::switchTune(handle, immediately, tune);
                    break;
                }
                case 0x46: // movie audio: a chunk arrived in the landing buffer (fed at its transfer)
                    p.u32();
                    break;
                case 0x48:
                {
                    const int32_t handle = p.i32();
                    mixer::setTunePart(handle, p.i32());
                    break;
                }
                case 0x49: // channel loudness: handle, channel, left, right
                {
                    const int32_t handle = p.i32();
                    const uint32_t channel = p.u32(), left = p.u32(), right = p.u32();
                    if (streamLog())
                        std::fprintf(stderr, "[ns stream] %.3f %d channel %u loudness %u %u\n", now(), handle, channel, left, right);
                    mixer::setStreamChannelLoudness(handle, channel, left, right);
                    break;
                }
                default:
                {
                    static int unknown = 0;
                    if (unknown++ < 10)
                        std::fprintf(stderr, "[native sound] unknown packet tag %#x: rest of the packet skipped\n", tag);
                    p.at = p.end;
                    break;
                }
                }
            }

            mixer::assignChannels(); // (a packet without tag 0x25: its key-ons all the same)
            mixer::endPacket();
            bool fed = false; // (a stream the EE feeds: the mix here, its credits from it at once)
            for (const auto &[handle, eeFed] : g_eeFedStreams)
                fed = fed || eeFed;
            if (fed)
            {
                postMix(now(), true);
                for (const auto &[handle, eeFed] : g_eeFedStreams)
                    if (eeFed)
                        grantCredits(mixer::takeConsumedChunks(handle)); // the IOP grants one per chunk it moved on
            }
            if (++g_packets % 15 == 0) // (the record a save state keeps: a few times a second)
                writeRecord(g_rdram);
            if (!fed)
                postMix(now(), false);
        }

        // ---- the status buffer ----------------------------------------------------------------
        // u32 words: three timings (us), twelve statistics, the pending tone-library loads, the
        // playing list (n, n handles sorted, -1 = empty), the streams (m, m pairs of handle and
        // tune-part index), the sequencers (k, k handles), two priority debug lists (counts 0),
        // then 0xCAFEF00D.
        void writeStatus(uint32_t dest, uint32_t size)
        {
            static const uint32_t kStatistics[12] = {0x200000u, 0x4AD00u, 0x4AD00u, 0x96000u, 0x8F890u, 0x8F890u,
                                                     0xA800u,   0x540u,   0x540u,   0x1FAFEFu, 0x1FAF6Fu, 0x1FAF6Fu}; // as recorded from the server
            std::vector<uint32_t> w;
            w.insert(w.end(), {0u, 0u, 0u});
            w.insert(w.end(), std::begin(kStatistics), std::end(kStatistics));
            w.push_back(0u); // pending tone-library loads (they load at once)
            std::vector<int32_t> playing = mixer::playingVoices();
            std::sort(playing.begin(), playing.end(), [](int32_t a, int32_t b) { return static_cast<uint32_t>(a) < static_cast<uint32_t>(b); });
            if (playing.size() > kPlayingSlots)
                playing.resize(kPlayingSlots);
            w.push_back(kPlayingSlots);
            for (int i = 0; i < kPlayingSlots; ++i)
                w.push_back(i < static_cast<int>(playing.size()) ? static_cast<uint32_t>(playing[static_cast<size_t>(i)]) : 0xFFFFFFFFu);
            const auto streams = mixer::streams();
            w.push_back(static_cast<uint32_t>(streams.size()));
            for (auto it = streams.rbegin(); it != streams.rend(); ++it) // (the server lists the newest first)
            {
                w.push_back(static_cast<uint32_t>(it->first));
                w.push_back(static_cast<uint32_t>(it->second));
            }
            w.insert(w.end(), {4u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu}); // the sequencers: none
            w.insert(w.end(), {0u, 0u, kStatusEnd});
            for (uint32_t i = 0u; i < size; i += 4u)
                write32(dest + i, i / 4u < w.size() ? w[i / 4u] : 0u);
        }

        void parkIop()
        {
            if (g_iopParked || !g_runtime)
                return;
            static const bool keep = [] {
                const char *env = std::getenv("TF_NATIVE_SOUND_IOP");
                return env && *env == '1';
            }();
            if (keep)
                return;
            if (!native_disc::active())
            {
                std::fprintf(stderr, "[native sound] the IOP stays: the disc is read through its TwoFish (TF_NATIVE_DISC=0)\n");
                return;
            }
            g_runtime->setIopParked(true);
            g_iopParked = true;
            std::fprintf(stderr, "[native sound] the IOP is parked (nothing left for it to do)\n");
        }

        // ---- the commands ---------------------------------------------------------------------
        void command(uint32_t number, uint32_t argsSize, uint32_t resultsSize)
        {
            const uint32_t commander = read32(kCommander);
            const uint32_t args = read32(commander + 0x18u), results = read32(commander + 0x1Cu);
            Arguments a{args, args + argsSize};
            const uint32_t resultsRounded = (resultsSize + 0x3Fu) & ~0x3Fu;
            for (uint32_t i = 0u; i < resultsRounded; i += 4u)
                write32(results + i, 0u);
            switch (number)
            {
            case Sys_VersionQuery:
                write32(results, kVersion);
                break;
            case Sys_Startup:
                parkIop();
                break;
            case Sys_QueryStatus:
                write32(results, kStatusAddress);
                write32(results + 4u, kStatusSize);
                break;
            case Sys_FramePacket:
                framePacket(args, argsSize);
                break;
            case Master_SilenceVoices:
                mixer::silenceVoices();
                break;
            case VoiceImmediate_Create:
                mixer::createVoice(a.i32());
                break;
            case VoiceImmediate_Destroy:
            {
                const int32_t handle = a.i32();
                mixer::forgetPlay(handle);
                mixer::destroyVoice(handle);
                break;
            }
            case VoiceImmediate_SetTone:
            {
                const int32_t handle = a.i32();
                mixer::setTone(handle, a.str());
                break;
            }
            case ToneLibrary_Create:
            {
                const int32_t made = mixer::createLibrary();
                write32(results, static_cast<uint32_t>(made));
                std::fprintf(stderr, "[native sound] tone library %d made\n", made); // (rare: always logged)
                writeRecord(g_rdram); // (the libraries at once: a save state taken now keeps them)
                break;
            }
            case ToneLibrary_Destroy:
            {
                const int32_t gone = a.i32();
                mixer::destroyLibrary(gone);
                std::fprintf(stderr, "[native sound] tone library %d destroyed\n", gone);
                writeRecord(g_rdram);
                break;
            }
            case ToneLibrary_StringCommand:
            {
                const int32_t handle = a.i32();
                const int32_t op = a.i32();
                a.b();
                const std::string file = a.str();
                // (op 0 at a level's start; a library named under another op is loaded too and the op
                // logged once: a Mini-Con equipped in the middle of a level had no sounds - user,
                // 2026-10-23 - and the op its load used was never seen)
                const auto isLibrary = [&file]()
                {
                    std::string f = file;
                    for (char &c : f)
                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    return f.size() > 4 && (f.compare(f.size() - 4, 4, ".tlb") == 0 || f.compare(f.size() - 4, 4, ".tla") == 0);
                };
                if (op != 0)
                {
                    static std::set<int32_t> seen;
                    if (seen.insert(op).second)
                        std::fprintf(stderr, "[native sound] tone library command op %d on %d: '%s'\n", op, handle, file.c_str());
                }
                if (op == 0 || isLibrary())
                {
                    const bool ok = mixer::loadLibrary(handle, file);
                    std::fprintf(stderr, "[native sound] tone library %d '%s' %s\n", handle, file.c_str(), ok ? "loaded" : "FAILED");
                    // A level's bank (.tla). Revisited after it is done the game loads a trimmed one
                    // (`<level>_ret.tla`) that lacks tones the level still plays - the carrier's
                    // elevator and switches (Switch_On, Elevator_Instrument, Launch, Move,
                    // AircraftCarrier_Elevator_End: silent on the PS2 too). The full bank is looked
                    // in last, for those only; any other level's bank takes it away.
                    std::string lower = file;
                    for (char &c : lower)
                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (ok && lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".tla") == 0)
                    {
                        const size_t ret = lower.size() >= 8 ? lower.size() - 8 : std::string::npos;
                        const bool revisit = ret != std::string::npos && lower.compare(ret, 8, "_ret.tla") == 0;
                        mixer::setFallbackLibrary(revisit ? file.substr(0, ret) + file.substr(ret + 4) : std::string());
                    }
                    writeRecord(g_rdram);
                }
                break;
            }
            case SoundStream_Create:
            {
                const bool pcm = a.b();
                const bool eeFed = a.b();
                const int32_t handle = mixer::createStream(pcm, eeFed);
                g_eeFedStreams[handle] = eeFed;
                if (streamLog())
                    std::fprintf(stderr, "[ns stream] %.3f %d made (pcm %d, EE-fed %d)\n", now(), handle, pcm ? 1 : 0, eeFed ? 1 : 0);
                write32(results, static_cast<uint32_t>(handle));
                break;
            }
            case SoundStream_Destroy:
            {
                const int32_t handle = a.i32();
                if (streamLog())
                    std::fprintf(stderr, "[ns stream] %.3f %d destroyed\n", now(), handle);
                mixer::destroyStream(handle);
                g_eeFedStreams.erase(handle);
                g_reservedChannels.erase(handle);
                updateReserved();
                break;
            }
            case SoundStream_AllocateDMABuffer:
                write32(results, kLandingBuffer);
                break;
            case SoundStream_Command:
            {
                const int32_t handle = a.i32();
                const int32_t op = a.i32();
                const uint32_t x = a.u32();
                const uint32_t y = a.u32();
                float fx, fy;
                std::memcpy(&fx, &x, 4);
                std::memcpy(&fy, &y, 4);
                if (streamLog())
                    std::fprintf(stderr, "[ns stream] %.3f %d command %d %08x %08x\n", now(), handle, op, x, y);
                if (op == 4) // stop (movie audio)
                    mixer::stopStream(handle);
                else if (op == 5) // pitch bend, loudness
                    mixer::setStreamPitch(handle, fx, fy);
                else if (op == 7)
                    mixer::setStreamLooping(handle, x != 0u);
                else if (op == 8) // buffer options: the server allocates its ring and grants size / 8 KB credits
                    grantCredits(static_cast<int>(x / 0x2000u));
                break;
            }
            case SoundStream_ReserveChannels:
            {
                const int32_t handle = a.i32();
                g_reservedChannels[handle] = a.i32();
                updateReserved();
                break;
            }
            case TDG_Fish_Command:
            {
                const int32_t op = a.i32();
                const std::string archive = a.str();
                if (op == 0)
                    mixer::mountArchive(archive);
                break;
            }
            default: // the rest need no reply and change nothing the game can hear or see
                break;
            }
            if (g_trace && number != Sys_FramePacket)
                std::fprintf(stderr, "[native sound] command %u (args %u, results %u)\n", number, argsSize, resultsSize);
        }

        void sendCommandFn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            g_rdram = rdram;
            const MixerScope scope; // (the posted mix done, the mixer to the EE until this returns)
            command(getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 6));
            ctx->pc = getRegU32(ctx, 31);
        }

        void memoryRequestFn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            g_rdram = rdram;
            const uint32_t dest = getRegU32(ctx, 4), src = getRegU32(ctx, 5), size = getRegU32(ctx, 6);
            const MixerScope scope; // (the posted mix done, the mixer to the EE until this returns)
            if (src == kStatusAddress)
                writeStatus(dest, size);
            else
            {
                static int other = 0;
                if (other++ < 5)
                    std::fprintf(stderr, "[native sound] memory request from IOP %08x (%u bytes): zeros\n", src, size);
                for (uint32_t i = 0u; i < size; i += 4u)
                    write32(dest + i, 0u);
            }
            ctx->pc = getRegU32(ctx, 31);
        }

        // 0x1E8B20(slot, device, src, size, dest, op, arg): slot +0 the device, +4 a SIF DMA transfer
        // (its id at +0x14, which sceSifDmaStat reads as done), +0x18 the op and +0x1C the argument
        // the device sends once it is done (tag 0x46, the stream). The bytes go to the stream now.
        void transferStartFn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            g_rdram = rdram;
            const MixerScope scope; // (the posted mix done, the mixer to the EE until this returns)
            const uint32_t slot = getRegU32(ctx, 4);
            // (the size before the destination: measured, src 01105b40 / 0x2000 bytes / the landing
            // buffer 0x131240 - read the other way round every movie's audio was dropped as 1.2 MB)
            const uint32_t src = getRegU32(ctx, 6), size = getRegU32(ctx, 7), dest = getRegU32(ctx, 8);
            const uint32_t op = getRegU32(ctx, 9), arg = getRegU32(ctx, 10);
            static int logged = 0;
            if (g_trace && logged++ < 3)
                std::fprintf(stderr, "[native sound] transfer src %08x dest %08x size %u op %#x arg %u\n", src, dest, size, op, arg);
            write32(slot, getRegU32(ctx, 5));
            write32(slot + 0x14u, 0u);
            write32(slot + 0x18u, op);
            write32(slot + 0x1Cu, arg);
            if (op == 0x46u && size > 0u && size <= 0x10000u)
                if (const uint8_t *p = getConstMemPtr(rdram, src))
                {
                    // (the chunk is contiguous EE memory: the feeder's ring is in main RAM)
                    std::vector<uint8_t> chunk(p, p + size);
                    mixer::feedStream(static_cast<int32_t>(arg), chunk.data(), size);
                }
            ctx->pc = getRegU32(ctx, 31);
        }
    }

    namespace
    {
        void writeRecord(uint8_t *rdram)
        {
            if (!g_record || !rdram)
                return;
            std::string text = mixer::saveSetup();
            if (text.size() > kRecordSize - 8u)
            {
                static bool warned = false;
                if (!warned)
                    std::fprintf(stderr, "[native sound] the setup (%zu bytes) does not fit the save-state record: cut\n", text.size());
                warned = true;
                text.resize(kRecordSize - 8u);
            }
            if (uint8_t *p = getMemPtr(rdram, g_record))
            {
                const uint32_t n = static_cast<uint32_t>(text.size());
                std::memcpy(p, &kRecordMagic, 4);
                std::memcpy(p + 4, &n, 4);
                std::memcpy(p + 8, text.data(), n);
            }
        }
    }

    void installState() { g_record = guest_code::addState("native sound server (its setup, for save states)", kRecordSize); }

    void stateLoaded(uint8_t *rdram)
    {
        if (!g_active || !g_record)
            return;
        g_rdram = rdram;
        const MixerScope scope; // (the posted mix done, the mixer to the EE until this returns)
        parkIop(); // (no Sys_Startup comes after a load: the state's IOP, whichever server it ran, is parked here)
        const uint8_t *p = getConstMemPtr(rdram, g_record);
        uint32_t magic = 0u, n = 0u;
        if (p)
        {
            std::memcpy(&magic, p, 4);
            std::memcpy(&n, p + 4, 4);
        }
        if (!p || magic != kRecordMagic || n > kRecordSize - 8u)
        {
            mixer::reset();
            std::fprintf(stderr, "[native sound] the state has no record of the native server (made on the IOP's, or before it was kept): "
                                 "the sound comes back with the next level\n");
            return;
        }
        mixer::loadSetup(std::string(reinterpret_cast<const char *>(p + 8), n));
        std::fprintf(stderr, "[native sound] the server's setup rebuilt from the state (%u bytes)\n", n);
    }

    bool active() { return g_active; }

    std::unique_lock<std::mutex> lockMixer()
    {
        waitMix();
        return std::unique_lock<std::mutex>(g_access);
    }

    void addSettings()
    {
        ps2_settings::Setting s;
        s.key = "TF_NATIVE_SOUND";
        s.section = "Sound";
        s.label = "Sound engine";
        s.order = 5;
        s.help = "Native: the game's sound server runs on the PC (no IOP or SPU2 emulated: a lighter load). "
                 "Emulated: the original IOP module on the emulated sound chip. Takes a restart.";
        s.choices = {{"1", "Native"}, {"0", "Emulated (IOP)"}};
        s.defaultValue = "1";
        s.apply = [](const std::string &) { return false; }; // (takes a restart)
        ps2_settings::add(s);
    }

    void install(PS2Runtime &runtime)
    {
        const char *env = std::getenv("TF_NATIVE_SOUND");
        if (env && *env == '0')
        {
            sound_ledger::install(runtime); // (TF_SOUND_LEDGER=1: around the IOP's path)
            return;
        }
        g_trace = std::getenv("TF_NATIVE_SOUND_TRACE") != nullptr;
        if (!runtime.lookupFunction(kSendCommand) || !runtime.lookupFunction(kMemoryRequest) || !runtime.lookupFunction(kTransferStart))
        {
            std::fprintf(stderr, "[native sound] the sound functions were not found: not installed\n");
            return;
        }
        g_runtime = &runtime;
        mixer::setDisc(PS2Runtime::getIoPaths().cdRoot);
        runtime.registerFunction(kSendCommand, sendCommandFn);
        runtime.registerFunction(kMemoryRequest, memoryRequestFn);
        runtime.registerFunction(kTransferStart, transferStartFn);
        g_active = true;
        std::fprintf(stderr, "[native sound] the Sirens server runs natively (TF_NATIVE_SOUND=0: the IOP's)\n");
        if (const char *thread = std::getenv("TF_NATIVE_SOUND_THREAD"); !(thread && *thread == '0'))
        {
            g_mixThreaded = true;
            std::thread(mixLoop).detach(); // (the mixing thread: see g_mixMutex)
        }
        sound_ledger::install(runtime); // (TF_SOUND_LEDGER=1: around the native server's functions)
    }
}
