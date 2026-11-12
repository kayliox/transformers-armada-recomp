// The native sound server's engine (native_sound_mixer.h).
//
// What it reproduces of SIRENS3 (decomp Claude's Sirens report and the 2026-10-22 handoff):
//   - Voices: a tone or an instrument (a region's tone with the instrument's envelope) from the
//     loaded tone libraries, played at the SPU2's 48 kHz: the pitch register clamp(rate x 4096 x
//     factor / 48000, 0, 3FFFh) as a 12-bit fraction counter, the hardware's 4-point gaussian
//     interpolation, SPU2 15-bit volume words (bit 14: phase-inverted), loops from the ADPCM
//     flags, an envelope from the instrument's ADSR in seconds. 48 channels less the ones the
//     streams reserved; past that the lowest priority is taken. A voice counts as playing until
//     its envelope is back at 0 (the server's ENVX = 0 rule), its data ended, or it was stolen.
//   - File streams (.MS: channel-interleaved SPU ADPCM, header SL3\0MS2\0) and albums (.ALB_T:
//     tunes of parts, each repeated RepeatCount times, 9999 for ever), with per-channel left and
//     right loudness (tag 0x49: the 8-channel music is four stereo stems), loudness, pitch and
//     looping; the next tune-part index each frame, -1 when played out.
//   - Movie audio streamed by the EE in 8 KB chunks (16-bit PCM in rows of 256 left and 256
//     right samples; ADPCM in 16-byte rows), one credit back per chunk played.
//   - The reverb: the SPU2's algorithm (as the IOP model's iop_spu2.cpp has it: psx-spx's order
//     of computation, the half-rate resampling filters) over a work area of its own, with the
//     preset registers read from the disc's LIBSD.IRX (Room, Studio A-C, Hall, Space, Echo, Delay,
//     Pipe), and its return level.

#include "native_sound_mixer.h"

#include "native_sound_data.h"
#include "pad_speaker.h"

#include "runtime/ps2_audio_stream.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <deque>
#include <fstream>
#include <map>
#include <set>
#include <vector>
#include <memory>

namespace native_sound::mixer
{
    namespace
    {
        constexpr double kRate = 48000.0;
        constexpr int kChannels = 48;
        constexpr double kMaxRenderSeconds = 0.25; // a longer gap (a load) is skipped, not mixed
        // The output level: the emulated SPU2's comes out at half of tone x envelope x volume word,
        // for voices and streams alike (the A/B of 2026-10-22: menu music 2.00x at correlation 0.996,
        // the title jingle 1.9x), so the mix is halved to sound as the game always has here.
        constexpr float kOutputGain = 0.5f;

        using data::Envelope;
        using data::Tone;

        // ---- helpers -------------------------------------------------------------------------
        constexpr int16_t kGauss[512] = {
            -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
            0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 3, 3,
            3, 4, 4, 5, 5, 6, 7, 7, 8, 9, 9, 10, 11, 12, 13, 14,
            15, 16, 17, 18, 19, 21, 22, 24, 25, 27, 28, 30, 32, 33, 35, 37,
            39, 41, 44, 46, 48, 51, 53, 56, 58, 61, 64, 67, 70, 73, 77, 80,
            84, 87, 91, 95, 99, 103, 107, 111, 116, 120, 125, 130, 135, 140, 145, 150,
            156, 161, 167, 173, 179, 186, 192, 199, 205, 212, 219, 227, 234, 242, 250, 257,
            266, 274, 283, 291, 300, 309, 319, 328, 338, 348, 358, 369, 379, 390, 401, 412,
            424, 436, 448, 460, 473, 485, 498, 512, 525, 539, 553, 567, 582, 597, 612, 627,
            643, 659, 675, 692, 708, 726, 743, 761, 779, 797, 816, 835, 854, 874, 894, 914,
            935, 956, 977, 999, 1020, 1043, 1066, 1089, 1112, 1136, 1160, 1184, 1209, 1234, 1260, 1286,
            1312, 1339, 1366, 1394, 1422, 1450, 1479, 1508, 1537, 1567, 1598, 1628, 1660, 1691, 1723, 1756,
            1789, 1822, 1856, 1890, 1924, 1959, 1995, 2031, 2067, 2104, 2141, 2179, 2217, 2256, 2295, 2334,
            2374, 2415, 2456, 2497, 2539, 2582, 2624, 2668, 2712, 2756, 2801, 2846, 2892, 2938, 2985, 3032,
            3079, 3128, 3176, 3225, 3275, 3325, 3376, 3427, 3479, 3531, 3584, 3637, 3691, 3745, 3799, 3855,
            3910, 3967, 4023, 4081, 4138, 4197, 4255, 4315, 4374, 4435, 4495, 4557, 4619, 4681, 4744, 4807,
            4871, 4935, 5000, 5065, 5131, 5197, 5264, 5332, 5399, 5468, 5536, 5606, 5676, 5746, 5817, 5888,
            5959, 6032, 6104, 6177, 6251, 6325, 6400, 6475, 6550, 6626, 6702, 6779, 6856, 6934, 7012, 7091,
            7170, 7249, 7329, 7409, 7490, 7571, 7653, 7735, 7817, 7900, 7983, 8066, 8150, 8234, 8319, 8404,
            8489, 8575, 8661, 8748, 8834, 8922, 9009, 9097, 9185, 9273, 9362, 9451, 9541, 9630, 9720, 9811,
            9901, 9992, 10083, 10174, 10266, 10358, 10450, 10542, 10635, 10727, 10820, 10913, 11007, 11100, 11194, 11288,
            11382, 11476, 11571, 11665, 11760, 11855, 11950, 12045, 12140, 12236, 12331, 12427, 12522, 12618, 12714, 12809,
            12905, 13001, 13097, 13193, 13289, 13385, 13481, 13577, 13673, 13769, 13865, 13961, 14056, 14152, 14248, 14343,
            14439, 14534, 14630, 14725, 14820, 14915, 15010, 15104, 15199, 15293, 15387, 15481, 15575, 15669, 15762, 15855,
            15948, 16041, 16133, 16226, 16317, 16409, 16500, 16592, 16682, 16773, 16863, 16953, 17042, 17131, 17220, 17308,
            17396, 17484, 17571, 17658, 17744, 17830, 17916, 18001, 18086, 18170, 18254, 18337, 18420, 18502, 18584, 18665,
            18746, 18826, 18905, 18985, 19063, 19141, 19219, 19295, 19372, 19447, 19522, 19597, 19671, 19744, 19816, 19888,
            19959, 20030, 20100, 20169, 20238, 20306, 20373, 20439, 20505, 20570, 20634, 20698, 20760, 20822, 20884, 20944,
            21004, 21063, 21121, 21178, 21235, 21290, 21345, 21399, 21452, 21505, 21556, 21607, 21657, 21706, 21754, 21801,
            21848, 21893, 21938, 21982, 22025, 22066, 22107, 22148, 22187, 22225, 22262, 22299, 22334, 22369, 22402, 22435,
            22467, 22498, 22527, 22556, 22584, 22611, 22637, 22662, 22686, 22709, 22731, 22752, 22772, 22791, 22809, 22826,
            22842, 22857, 22872, 22885, 22897, 22908, 22918, 22927, 22935, 22942, 22948, 22953, 22957, 22960, 22962, 22963,
        };

        // The four newest samples, oldest first; the interpolation by bits 4-11 of the counter.
        int32_t gauss(const int16_t *h, uint32_t counter)
        {
            const uint32_t i = (counter >> 4u) & 0xFFu;
            int32_t out = (kGauss[0x0FFu - i] * h[0]) >> 15;
            out += (kGauss[0x1FFu - i] * h[1]) >> 15;
            out += (kGauss[0x100u + i] * h[2]) >> 15;
            out += (kGauss[0x000u + i] * h[3]) >> 15;
            return out;
        }

        uint32_t pitchRegister(double sampleRate, double factor)
        {
            const double v = sampleRate * 4096.0 * factor / kRate;
            return static_cast<uint32_t>(std::clamp(std::lround(v), 0L, 0x3FFFL));
        }

        // An SPU2 volume word: 15 bits, bit 14 = negative (phase-inverted); 3FFFh is about 1.
        float volumeGain(uint16_t v)
        {
            int32_t s = v & 0x7FFF;
            if ((s & 0x4000) != 0)
                s -= 0x8000;
            return static_cast<float>(s) * 2.0f / 32768.0f;
        }

        float loudnessGain(uint32_t v) { return std::min(static_cast<float>(v) / 16383.0f, 2.0f); }

        // ---- the disc ----------------------------------------------------------------------
        std::filesystem::path g_disc;
        data::Archive g_archive;
        std::map<int32_t, std::unique_ptr<data::ToneLibrary>> g_libraries;
        int32_t g_nextLibrary = 0;

        std::filesystem::path discPath(const std::string &gamePath)
        {
            std::string rel = data::normalise(gamePath);
            for (char &c : rel)
                if (c == '/')
                    c = static_cast<char>(std::filesystem::path::preferred_separator);
            return g_disc / rel;
        }

        // ---- voices --------------------------------------------------------------------------
        enum class Phase
        {
            Off,
            Delay,
            Attack,
            Decay,
            Sustain,
            Release,
        };

        struct Voice
        {
            const Tone *tone = nullptr;
            Envelope envelope;
            float gainLeft = 0.0f, gainRight = 0.0f, send = 0.0f, dry = 1.0f;
            std::string toneName; // (as set: a save state's record)
            bool toPad = false;   // (TODO 213: one of the game alerts the DualSense's speaker plays, pad_speaker.h)
            uint16_t volumeWords[2] = {0u, 0u}; // (as sent: TF_NATIVE_SOUND_KON)
            float pitch = 1.0f;
            int32_t priority = 0;
            uint64_t keyedAt = 0;
            // (TF_NATIVE_SOUND_REQ: the voice's life, to tell requested from heard)
            bool updated = false;   // a voice update (tag 0x22) seen since it was made
            double keyedTime = 0.0; // the mix clock at its key-on
            float peak = 0.0f;      // the loudest envelope x volume it reached since

            Phase phase = Phase::Off;
            float level = 0.0f, releaseStep = 0.0f;
            double phaseLeft = 0.0;
            bool softStop = false; // (TODO 290 (10)) this release is the engine's soft stop at a transformation
            // The SPU2's envelope (the default, sirensAdsr below): the ADSR words SIRENS3 writes for this tone or
            // instrument, the envelope level 0..7FFFh and its rate counter, stepped as the SPU2 does every sample.
            uint16_t adsr1 = 0x0D0Fu, adsr2 = 0x5FE2u;
            int32_t env = 0;
            uint32_t envCounter = 0u;
            uint32_t counter = 0, position = 0;
            int16_t history[4] = {};
            // (from the tone, the pitch and the envelope, worked out where they are set - prepareVoice -
            // instead of at each of the 48,000 samples a second)
            uint32_t pitchReg = 0x1000u;
            float attackStep = 0.0f, decayStep = 0.0f;
        };

        std::map<int32_t, Voice> g_voices;
        int g_reserved = 0;
        uint64_t g_keyCount = 0;

        const Envelope kToneEnvelope = [] {
            Envelope e;
            e.release = 0.005f; // a tone without an instrument: no attack, a click-free stop
            return e;
        }();

        // What the mix takes from a voice's tone, pitch and envelope at every sample, by the very
        // expressions it used there: worked out at each of their writers (setTone, updateVoice; a
        // save state's voice goes through both), the same values once instead of 48,000 times a
        // second. (The steps not where the envelope never takes them: no division by 0.)
        void prepareVoice(Voice &v)
        {
            const double dt = 1.0 / kRate;
            v.pitchReg = v.tone ? pitchRegister(v.tone->sampleRate, v.pitch) : 0x1000u;
            v.attackStep = v.envelope.attack <= 0.0f ? 0.0f : static_cast<float>(dt / v.envelope.attack);
            v.decayStep = v.envelope.decay <= 0.0f ? 0.0f : static_cast<float>((1.0 - v.envelope.sustain) * dt / v.envelope.decay);
        }

        // ---- the SPU2's envelope, as SIRENS3 sets it up (2026-11-11) ------------------------------
        // SIRENS3 turns an instrument's ADSR in seconds into the SPU2's ADSR words when it makes the instrument
        // (SIRENS3.IRX 0x375E8, called from the constructor at 0x378B4): the attack and the release are at least
        // 0.0005 s; each of attack, decay, sustain level and release becomes the index of the nearest entry of a
        // table of times (128 attack rates, 16 decay shifts, 16 sustain levels, 32 release shifts; data in the
        // module, read here from the disc's copy); ADSR1 = attack << 8 | decay << 4 | sustain (linear attack),
        // ADSR2 = 5FE0h | release (an exponential release, the sustain held). A tone without an instrument gets the
        // same from all-zero times with sustain 1 (0D0Fh / 5FE2h, measured on the emulated IOP, 321 of 443 key-ons
        // in Amazon). The voice's level then follows the SPU2's own envelope stepper (the IOP model's
        // iop_spu2.cpp, psx-spx), every 48 kHz sample - the PS2's curves: an exponential release where straight
        // ramps used to run, which ended long releases (engine idle 2.6 s, the dropship's 5.4 s) differently.
        // TF_NATIVE_SOUND_ENV=ramps: the straight ramps in seconds, as before.
        struct SirensTables
        {
            std::vector<float> attack, decay, sustain, release;
            bool ok = false;
        };
        SirensTables g_sirens;

        bool spu2Envelope()
        {
            static const bool on = [] {
                const char *env = std::getenv("TF_NATIVE_SOUND_ENV");
                return !(env && std::strcmp(env, "ramps") == 0);
            }();
            return on && g_sirens.ok;
        }

        void loadSirensTables(const std::filesystem::path &irx)
        {
            g_sirens = SirensTables{};
            std::ifstream f(irx, std::ios::binary);
            std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            auto at = [&](size_t o) {
                uint32_t w = 0;
                std::memcpy(&w, &d[o], 4);
                float x;
                std::memcpy(&x, &w, 4);
                return x;
            };
            // Found by the sustain levels (1/16 .. 16/16); the decay times are the 16 floats before them, the
            // release times the 32 after, then the 128 attack times.
            for (size_t o = 16u * 4u; o + (16u + 32u + 128u) * 4u <= d.size(); o += 4u)
            {
                bool levels = true;
                for (int k = 0; k < 16 && levels; ++k)
                    levels = at(o + static_cast<size_t>(k) * 4u) == static_cast<float>(k + 1) / 16.0f;
                if (!levels)
                    continue;
                auto take = [&](size_t from, int n) {
                    std::vector<float> t(static_cast<size_t>(n));
                    for (int k = 0; k < n; ++k)
                        t[static_cast<size_t>(k)] = at(from + static_cast<size_t>(k) * 4u);
                    return t;
                };
                SirensTables s;
                s.decay = take(o - 16u * 4u, 16);
                s.sustain = take(o, 16);
                s.release = take(o + 16u * 4u, 32);
                s.attack = take(o + 48u * 4u, 128);
                auto rising = [](const std::vector<float> &t) {
                    for (size_t k = 1; k < t.size(); ++k)
                        if (!(t[k] >= t[k - 1]) || !std::isfinite(t[k]))
                            return false;
                    return t.front() > 0.0f;
                };
                s.ok = rising(s.decay) && rising(s.release) && rising(s.attack);
                if (s.ok)
                {
                    g_sirens = std::move(s);
                    std::fprintf(stderr, "[native sound] the SPU2 envelope: SIRENS3's ADSR tables found in %s (attack %.5f..%.0f s, "
                                         "release %.5f..%.0f s)\n",
                                 irx.filename().string().c_str(), g_sirens.attack.front(), g_sirens.attack.back(),
                                 g_sirens.release.front(), g_sirens.release.back());
                    return;
                }
            }
            std::fprintf(stderr, "[native sound] SIRENS3's ADSR tables not found in %s: the envelopes are straight ramps\n",
                         irx.string().c_str());
        }

        // The index of the table entry nearest to `value` (SIRENS3 0x37724: the first entry above it, or the one
        // before when that is nearer; the last when none is above it).
        int nearestIndex(const std::vector<float> &t, float value)
        {
            size_t i = 1;
            while (i < t.size() && !(value < t[i]))
                ++i;
            if (i == t.size())
                return static_cast<int>(t.size()) - 1;
            return value - t[i - 1] > t[i] - value ? static_cast<int>(i) : static_cast<int>(i) - 1;
        }

        void sirensAdsr(const Envelope &e, uint16_t &adsr1, uint16_t &adsr2)
        {
            constexpr float kShortest = 0.0005f;
            const float attack = e.attack < kShortest ? kShortest : e.attack;
            const float release = e.release < kShortest ? kShortest : e.release;
            const int ar = nearestIndex(g_sirens.attack, attack), dr = nearestIndex(g_sirens.decay, e.decay);
            const int sl = nearestIndex(g_sirens.sustain, e.sustain), rr = nearestIndex(g_sirens.release, release);
            adsr1 = static_cast<uint16_t>(((ar & 0x7F) << 8) | ((dr & 0xF) << 4) | (sl & 0xF));
            adsr2 = static_cast<uint16_t>(0x5FE0 | (rr & 0x1F));
        }

        // One step of an SPU2 envelope level (iop_spu2.cpp's stepLevel, psx-spx's ADSR).
        void spu2StepLevel(int32_t &level, uint32_t &counter, int shift, int step, bool exponential, bool decreasing)
        {
            uint32_t counterStep = 0x8000u >> std::max(0, shift - 11);
            int32_t levelStep = step * (1 << std::max(0, 11 - shift));
            if (exponential)
            {
                if (decreasing)
                    levelStep = static_cast<int16_t>((levelStep * level) >> 15);
                else if (level > 0x6000)
                    counterStep >>= 2;
            }
            counter += std::max<uint32_t>(1u, counterStep);
            if (counter >= 0x8000u)
            {
                counter = 0u;
                level = std::clamp<int32_t>(level + levelStep, 0, 0x7FFF);
            }
        }

        void startPhase(Voice &v, Phase p)
        {
            v.phase = p;
            switch (p)
            {
            case Phase::Delay:
                v.phaseLeft = v.envelope.delay;
                break;
            case Phase::Attack:
                v.phaseLeft = v.envelope.attack;
                break;
            case Phase::Decay:
                v.phaseLeft = v.envelope.decay;
                break;
            case Phase::Release:
                v.phaseLeft = std::max(0.001f, v.envelope.release);
                v.releaseStep = v.level / static_cast<float>(v.phaseLeft * kRate);
                break;
            default:
                break;
            }
        }

        // The SPU2's envelope for a sample (iop_spu2.cpp's stepEnvelope): attack to 7FFFh, decay to the sustain level,
        // sustain until the key-off, release to 0. The delay before the attack (an instrument's DelayTime) stays in
        // seconds, as before.
        void stepSpu2Envelope(Voice &v)
        {
            int shift = 0, step = 0;
            bool exponential = false, decreasing = false;
            int32_t target = 0;
            switch (v.phase)
            {
            case Phase::Delay:
                v.phaseLeft -= 1.0 / kRate;
                if (v.phaseLeft <= 0.0)
                    v.phase = Phase::Attack;
                return;
            case Phase::Attack:
            {
                const int rate = (v.adsr1 >> 8) & 0x7F;
                shift = rate >> 2;
                step = 7 - (rate & 3);
                exponential = (v.adsr1 & 0x8000u) != 0u;
                target = 0x7FFF;
                break;
            }
            case Phase::Decay:
                shift = (v.adsr1 >> 4) & 0xF;
                step = -8;
                exponential = decreasing = true;
                target = static_cast<int32_t>((v.adsr1 & 0xFu) + 1u) << 11;
                break;
            case Phase::Sustain:
            {
                const int rate = (v.adsr2 >> 6) & 0x7F;
                shift = rate >> 2;
                step = 7 - (rate & 3);
                exponential = (v.adsr2 & 0x8000u) != 0u;
                decreasing = (v.adsr2 & 0x4000u) != 0u;
                if (decreasing)
                    step = ~step;
                break;
            }
            case Phase::Release:
                if (v.softStop) // (TODO 290 (10): the engine's 0.15 s fade at a transformation, a straight ramp)
                {
                    v.level = std::max(0.0f, v.level - v.releaseStep);
                    v.env = static_cast<int32_t>(v.level * 32768.0f);
                    if (v.level <= 0.0f)
                    {
                        v.env = 0;
                        v.phase = Phase::Off;
                    }
                    return;
                }
                shift = v.adsr2 & 0x1F;
                step = -8;
                exponential = (v.adsr2 & 0x20u) != 0u;
                decreasing = true;
                break;
            default:
                return;
            }
            spu2StepLevel(v.env, v.envCounter, shift, step, exponential, decreasing);
            v.level = static_cast<float>(v.env) / 32768.0f;
            if (v.phase == Phase::Sustain)
            {
                if (v.env == 0 && decreasing)
                    v.phase = Phase::Off;
                return;
            }
            if (decreasing ? v.env <= target : v.env >= target)
            {
                if (v.phase == Phase::Release)
                {
                    v.env = 0;
                    v.level = 0.0f;
                    v.phase = Phase::Off;
                }
                else
                    v.phase = v.phase == Phase::Attack ? Phase::Decay : Phase::Sustain;
            }
        }

        void stepEnvelope(Voice &v)
        {
            if (spu2Envelope())
            {
                stepSpu2Envelope(v);
                return;
            }
            const double dt = 1.0 / kRate;
            switch (v.phase)
            {
            case Phase::Delay:
                v.phaseLeft -= dt;
                if (v.phaseLeft <= 0.0)
                    startPhase(v, Phase::Attack);
                break;
            case Phase::Attack:
                if (v.envelope.attack <= 0.0f)
                {
                    v.level = 1.0f;
                    startPhase(v, Phase::Decay);
                    break;
                }
                v.level = std::min(1.0f, v.level + v.attackStep);
                if (v.level >= 1.0f)
                    startPhase(v, Phase::Decay);
                break;
            case Phase::Decay:
                if (v.envelope.decay <= 0.0f || v.level <= v.envelope.sustain)
                {
                    v.level = std::min(v.level, std::max(0.0f, v.envelope.sustain));
                    startPhase(v, Phase::Sustain);
                    break;
                }
                v.level = std::max(v.envelope.sustain, v.level - v.decayStep);
                break;
            case Phase::Release:
                v.level -= v.releaseStep;
                if (v.level <= 0.0f)
                {
                    v.level = 0.0f;
                    v.phase = Phase::Off;
                }
                break;
            default:
                break;
            }
        }

        // The next sample of a voice's tone into its history; false at the end of its data.
        bool fetch(Voice &v)
        {
            const Tone &t = *v.tone;
            if (v.position >= t.pcm.size())
            {
                if (!t.loop || t.loopStart >= t.pcm.size())
                    return false;
                v.position = t.loopStart;
            }
            v.history[0] = v.history[1];
            v.history[1] = v.history[2];
            v.history[2] = v.history[3];
            v.history[3] = t.pcm[v.position++];
            return true;
        }

        int capacity() { return std::max(0, kChannels - g_reserved); }

        // Which of two voices gives way when the channels run out: the LARGER priority number (TODO
        // 106). SIRENS3's steal fallback (IOP 0x4DE74) sorts the playing voices by their priority
        // (voice +0x1C, the tag 0x22 value; 0 until the first update, 0x3EB68) ascending (0x4E478)
        // and takes the channel of the LAST one; the EE's own sound-node pool (0x27B7F0) likewise
        // gives a node to a new sound only by taking the one with the largest number, and refuses
        // when the new sound's number is larger still. The game's numbers follow it: immediate voices
        // (the player's weapons, Mini-Cons, car engine, the interface) send 9, 3D voices group x
        // 10000 + priority (impacts and alerts 5xxxx, birds, footsteps and ambience 6xxxx). Until
        // now the larger number won here, so at the 38-channel limit a bird or a footstep took the
        // player's own car engine and gun. TF_NATIVE_SOUND_PRIO=high: the old (wrong) order.
        bool lessImportant(const Voice &a, const Voice &b)
        {
            static const bool lowWins = [] {
                const char *env = std::getenv("TF_NATIVE_SOUND_PRIO");
                return !(env && std::strcmp(env, "high") == 0);
            }();
            return lowWins ? a.priority > b.priority : a.priority < b.priority;
        }

        // TF_NATIVE_SOUND_REQ=1 (TODO 106): every key-on the game asks for and what came of it (keyed,
        // REFUSED for want of a channel, or another voice stolen), every key-off, and each voice's end
        // with how long it played and the loudest it got (envelope x volume word, 1 = full): a request
        // that ends with peak 0 was asked for but never heard.
        bool reqLog()
        {
            static const bool on = [] {
                const char *env = std::getenv("TF_NATIVE_SOUND_REQ");
                return env && *env == '1';
            }();
            return on;
        }
        double g_reqClock = 0.0; // (g_mixedUntil, set below: the log's clock)
        const char *toneOf(const Voice &v) { return v.tone ? v.tone->name.c_str() : v.toneName.c_str(); }
        void reqEnd(int32_t handle, const Voice &v, const char *why)
        {
            if (reqLog() && handle >= 0)
                std::fprintf(stderr, "[ns req] %.3f end h %d '%s' %s after %.3f s peak %.3f\n", g_reqClock, handle, toneOf(v), why,
                             g_reqClock - v.keyedTime, v.peak);
        }

        // ---- streams -------------------------------------------------------------------------
        struct Stream
        {
            bool pcm = false, eeFed = false, looping = false;
            bool open = false, playing = false, finished = false, flushed = false;
            float loudness = 1.0f, pitch = 1.0f;
            std::array<float, 8> left{}, right{};
            bool channelLoudness = false; // (tag 0x49 seen: the channels' own levels, the loudness not applied on top)
            bool toPad = false;           // (TODO 213: an HQ call's voice message, for the DualSense's speaker)
            int32_t index = 0;

            // a file or an album
            std::string file;
            data::Album album;
            bool isAlbum = false;
            int tune = 0, part = 0, repeatsDone = 0;
            std::string pendingTune;
            std::ifstream in;
            data::StreamHeader header;
            uint32_t rowsLeft = 0; // interleave rows still to read
            std::array<int32_t, 8> h1{}, h2{};
            std::array<std::deque<int16_t>, 8> pcmQueue;
            std::vector<uint8_t> rowBytes; // (readRow's, kept: no allocation for each row)
            uint32_t samplesLeft = 0; // per channel, of the part

            // resampling to 48 kHz
            uint32_t counter = 0;
            std::array<std::array<int16_t, 4>, 8> history{};
            double sampleRate = 48000.0;
            uint32_t pitchReg = 0x1000u; // pitchRegister(sampleRate, pitch), kept at their writers (48 kHz at 1: 1000h)

            // movie audio
            std::deque<int16_t> eeLeft, eeRight;
            uint64_t chunkFrames = 0;   // frames a chunk holds
            uint64_t framesPlayed = 0, framesQueued = 0;
            int chunksQueued = 0, chunksConsumed = 0, chunksReported = 0;
        };

        std::map<int32_t, Stream> g_streams;

        bool openPart(Stream &s, const std::string &file)
        {
            s.in.close();
            s.in.clear();
            s.in.open(discPath(file), std::ios::binary);
            uint8_t h[0x28];
            if (!s.in || !s.in.read(reinterpret_cast<char *>(h), sizeof(h)) || !data::readStreamHeader(h, sizeof(h), s.header))
            {
                std::fprintf(stderr, "[native sound] stream file '%s' not readable\n", file.c_str());
                return false;
            }
            s.in.seekg(s.header.dataOffset ? s.header.dataOffset : 0x8000u);
            s.samplesLeft = s.header.samples;
            s.sampleRate = s.header.sampleRate;
            s.pitchReg = pitchRegister(s.sampleRate, s.pitch);
            s.h1.fill(0);
            s.h2.fill(0);
            for (auto &q : s.pcmQueue)
                q.clear();
            return true;
        }

        // Reads one interleave row of every channel and decodes it.
        bool readRow(Stream &s)
        {
            const data::StreamHeader &h = s.header;
            std::vector<uint8_t> &row = s.rowBytes; // (every byte read over below, or the row is not used)
            row.resize(static_cast<size_t>(h.interleave) * h.channels);
            if (!s.in.read(reinterpret_cast<char *>(row.data()), static_cast<std::streamsize>(row.size())))
                return false;
            for (uint32_t c = 0; c < h.channels && c < 8; ++c)
                for (uint32_t b = 0; b + 16 <= h.interleave; b += 16)
                {
                    int16_t out[28];
                    data::decodeBlock(&row[c * h.interleave + b], out, s.h1[c], s.h2[c]);
                    s.pcmQueue[c].insert(s.pcmQueue[c].end(), out, out + 28);
                }
            return true;
        }

        const data::Album::Tune *currentTune(const Stream &s)
        {
            if (!s.isAlbum || s.tune < 0 || s.tune >= static_cast<int>(s.album.tunes.size()))
                return nullptr;
            return &s.album.tunes[static_cast<size_t>(s.tune)];
        }

        // The part that plays now ended: repeat it, go to the next, or end.
        bool nextPart(Stream &s)
        {
            if (!s.isAlbum)
            {
                if (s.looping)
                    return openPart(s, s.file);
                return false;
            }
            if (!s.pendingTune.empty())
            {
                for (size_t i = 0; i < s.album.tunes.size(); ++i)
                    if (s.album.tunes[i].name == s.pendingTune)
                        s.tune = static_cast<int>(i);
                s.pendingTune.clear();
                s.part = 0;
                s.repeatsDone = 0;
            }
            else
            {
                const data::Album::Tune *t = currentTune(s);
                if (!t || t->parts.empty())
                    return false;
                const int repeats = t->parts[static_cast<size_t>(s.part)].repeatCount;
                if (++s.repeatsDone < repeats || repeats >= 9999)
                {
                    // (again)
                }
                else
                {
                    s.repeatsDone = 0;
                    if (++s.part >= static_cast<int>(t->parts.size()))
                        return false;
                }
            }
            const data::Album::Tune *t = currentTune(s);
            if (!t || s.part >= static_cast<int>(t->parts.size()))
                return false;
            if (s.index != s.part)
                std::fprintf(stderr, "[native sound] album '%s': tune '%s' part %d '%s'\n", s.file.c_str(), t->name.c_str(), s.part,
                             t->parts[static_cast<size_t>(s.part)].file.c_str());
            s.index = s.part;
            return openPart(s, t->parts[static_cast<size_t>(s.part)].file);
        }

        // The next sample of each channel into the resampling history; false at the end.
        bool fetchStream(Stream &s)
        {
            const uint32_t channels = std::min<uint32_t>(s.header.channels, 8u);
            if (s.samplesLeft == 0u)
            {
                if (!nextPart(s))
                    return false;
            }
            if (s.pcmQueue[0].empty() && !readRow(s))
            {
                s.samplesLeft = 0;
                return fetchStream(s);
            }
            for (uint32_t c = 0; c < channels; ++c)
            {
                auto &h = s.history[c];
                h[0] = h[1];
                h[1] = h[2];
                h[2] = h[3];
                h[3] = s.pcmQueue[c].empty() ? 0 : s.pcmQueue[c].front();
                if (!s.pcmQueue[c].empty())
                    s.pcmQueue[c].pop_front();
            }
            --s.samplesLeft;
            return true;
        }

        void endStream(Stream &s)
        {
            if (s.toPad && !s.finished)
                pad_speaker::noteAlertStreamEnded(); // (TODO 213: the HQ call's next message may follow)
            s.playing = false;
            s.finished = true;
            s.index = -1;
        }

        // ---- the reverb ------------------------------------------------------------------------
        // The 32 registers of a preset, in the PS1/libsd order.
        enum Reg
        {
            dAPF1, dAPF2, vIIR, vCOMB1, vCOMB2, vCOMB3, vCOMB4, vWALL, vAPF1, vAPF2, mLSAME, mRSAME, mLCOMB1, mRCOMB1,
            mLCOMB2, mRCOMB2, dLSAME, dRSAME, mLDIFF, mRDIFF, mLCOMB3, mRCOMB3, mLCOMB4, mRCOMB4, dLDIFF, dRDIFF,
            mLAPF1, mRAPF1, mLAPF2, mRAPF2, vLIN, vRIN, kRegs
        };

        struct Preset
        {
            std::array<uint16_t, kRegs> regs{};
            uint32_t areaSamples = 0;
        };

        constexpr int32_t kReverbTaps[39] = {
            -1, 0, 2, 0, -10, 0, 35, 0, -103, 0, 266, 0, -616, 0, 1332, 0, -2960, 0, 10246, 16384,
            10246, 0, -2960, 0, 1332, 0, -616, 0, 266, 0, -103, 0, 35, 0, -10, 0, 2, 0, -1,
        };
        constexpr uint32_t kReverbTapAt[21] = {0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 19, 20, 22, 24, 26, 28, 30, 32, 34, 36, 38};
        // The filters' span: the 39 newest samples of a ring (kReverbTaps).
        constexpr uint32_t kReverbSpan = static_cast<uint32_t>(std::size(kReverbTaps));

        int32_t reverbFilter(const int16_t *history)
        {
            int32_t sum = 0;
            for (const uint32_t tap : kReverbTapAt)
                sum += kReverbTaps[tap] * history[tap];
            return sum;
        }
        int32_t clamp16(int32_t v) { return std::clamp<int32_t>(v, -0x8000, 0x7FFF); }

        struct Reverb
        {
            std::vector<Preset> presets; // [0] = off
            int mode = 0;
            float level = 0.0f; // the return level (tag 0x30)
            std::vector<int16_t> area;
            uint64_t sample = 0;
            uint32_t position = 0;
            int16_t input[2][128] = {}, output[2][128] = {};
            // The work-area places one sample of the effect reads and writes, per channel, relative to
            // the area's position: reduced modulo the area's size once, when a preset is set (prepare),
            // instead of by a 64-bit division at each of the 14 a sample makes. (p + rel) mod size is
            // (p + (rel mod size)) mod size: the same places, at every position.
            enum Spot
            {
                SameSrc, SamePrev, SameDst, DiffSrc, DiffPrev, DiffDst, Comb1, Comb2, Comb3, Comb4, Apf1Old, Apf1Dst, Apf2Old, Apf2Dst, kSpots
            };
            std::array<std::array<size_t, kSpots>, 2> spots{};
            size_t areaPos = 0; // (sample / 2) % area.size(): set by prepare, moved on with sample while the effect is on
            // Samples in a row with the effect off since it was last on or its rings' position started
            // again (up to kReverbSpan): from kReverbSpan on, the output rings' window holds only 0s.
            uint32_t quiet = 0;

            void loadPresets()
            {
                presets.assign(1, Preset{});
                std::ifstream f(g_disc / "IOP" / "LIBSD.IRX", std::ios::binary);
                std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                // Found by Room's first three registers; nine presets of 34 halfwords, the work
                // area sizes (8-byte units, Off first) as ten u32s 0x78 bytes before.
                static const uint8_t kRoom[6] = {0x7D, 0x00, 0x5B, 0x00, 0x80, 0x6D};
                auto it = std::search(d.begin(), d.end(), std::begin(kRoom), std::end(kRoom));
                if (it == d.end() || it - d.begin() < 0x80)
                {
                    std::fprintf(stderr, "[native sound] reverb presets not found in LIBSD.IRX: no reverb\n");
                    return;
                }
                const size_t base = static_cast<size_t>(it - d.begin());
                for (int p = 0; p < 9 && base + (p + 1) * 68u <= d.size(); ++p)
                {
                    Preset pr;
                    for (int r = 0; r < kRegs; ++r)
                        pr.regs[static_cast<size_t>(r)] = static_cast<uint16_t>(d[base + p * 68u + r * 2u] | (d[base + p * 68u + r * 2u + 1] << 8));
                    const size_t sizeAt = base - 0x78u + static_cast<size_t>(p + 1) * 8u;
                    uint32_t units = 0;
                    std::memcpy(&units, &d[sizeAt], 4);
                    pr.areaSamples = units * 4u; // 8 bytes = 4 samples
                    presets.push_back(pr);
                }
                prepare(); // (presets read again with one in force: its places again)
            }

            void setMode(int m)
            {
                if (m == mode)
                    return;
                mode = (m >= 0 && m < static_cast<int>(presets.size())) ? m : 0;
                area.assign(mode ? std::max<uint32_t>(presets[static_cast<size_t>(mode)].areaSamples, 16u) : 0u, 0);
                position = 0;
                quiet = 0; // (the rings' position starts again: their 0s are counted again)
                prepare();
            }

            // The places of the preset in force and the area position for the sample count, after a
            // change of preset or area; nothing while the effect is off.
            void prepare()
            {
                if (mode == 0 || area.empty() || static_cast<size_t>(mode) >= presets.size())
                    return;
                const Preset &pr = presets[static_cast<size_t>(mode)];
                const int64_t size = static_cast<int64_t>(area.size());
                const auto off = [&](Reg r) { return static_cast<int64_t>(pr.regs[r]) * 4; };
                const auto wrap = [&](int64_t rel)
                {
                    const int64_t w = rel % size;
                    return static_cast<size_t>(w < 0 ? w + size : w);
                };
                for (uint32_t right = 0; right < 2u; ++right)
                {
                    std::array<size_t, kSpots> &place = spots[right];
                    const int64_t sameDst = off(right ? mRSAME : mLSAME), diffDst = off(right ? mRDIFF : mLDIFF);
                    const int64_t apf1Dst = off(right ? mRAPF1 : mLAPF1), apf2Dst = off(right ? mRAPF2 : mLAPF2);
                    place[SameSrc] = wrap(off(right ? dRSAME : dLSAME));
                    place[SamePrev] = wrap(sameDst - 1);
                    place[SameDst] = wrap(sameDst);
                    place[DiffSrc] = wrap(off(right ? dLDIFF : dRDIFF));
                    place[DiffPrev] = wrap(diffDst - 1);
                    place[DiffDst] = wrap(diffDst);
                    place[Comb1] = wrap(off(right ? mRCOMB1 : mLCOMB1));
                    place[Comb2] = wrap(off(right ? mRCOMB2 : mLCOMB2));
                    place[Comb3] = wrap(off(right ? mRCOMB3 : mLCOMB3));
                    place[Comb4] = wrap(off(right ? mRCOMB4 : mLCOMB4));
                    place[Apf1Old] = wrap(apf1Dst - off(dAPF1));
                    place[Apf1Dst] = wrap(apf1Dst);
                    place[Apf2Old] = wrap(apf2Dst - off(dAPF2));
                    place[Apf2Dst] = wrap(apf2Dst);
                }
                areaPos = static_cast<size_t>((sample >> 1u) % static_cast<uint64_t>(size));
            }

            // One 48 kHz sample in and out (the effect runs on one channel a sample).
            void process(int32_t inLeft, int32_t inRight, int32_t &outLeft, int32_t &outRight)
            {
                position = (position + 1u) & 63u;
                const uint32_t at = position;
                input[0][at] = input[0][at + 64u] = static_cast<int16_t>(clamp16(inLeft));
                input[1][at] = input[1][at + 64u] = static_cast<int16_t>(clamp16(inRight));
                const uint32_t right = static_cast<uint32_t>(sample & 1u);
                const uint32_t first = (at - 38u) & 63u;
                int32_t value = 0;
                if (mode != 0 && !area.empty())
                {
                    const Preset &pr = presets[static_cast<size_t>(mode)];
                    const auto coef = [&](Reg r) { return static_cast<int32_t>(static_cast<int16_t>(pr.regs[r])); };
                    const size_t size = area.size();
                    const std::array<size_t, kSpots> &spot = spots[right];
                    const auto idx = [&](Spot s)
                    {
                        const size_t w = areaPos + spot[s];
                        return w >= size ? w - size : w;
                    };
                    const auto rd = [&](Spot s) { return static_cast<int32_t>(area[idx(s)]); };
                    const auto wr = [&](Spot s, int32_t v) { area[idx(s)] = static_cast<int16_t>(v); };
                    const auto mul = [](int32_t a, int32_t b) { return (a * b) >> 15; };
                    const int32_t in = clamp16(mul(clamp16(reverbFilter(&input[right][first]) >> 15), coef(right ? vRIN : vLIN)));
                    const int32_t iir = coef(vIIR), wall = coef(vWALL);
                    const auto reflect = [&](int32_t input, int32_t reflected, int32_t previous)
                    {
                        const int32_t mixed = clamp16(input + mul(reflected, wall));
                        const int32_t stored = clamp16(mul(clamp16(mixed - previous), iir) + previous);
                        return iir == -0x8000 ? clamp16(-stored) : stored;
                    };
                    const int32_t sameSrc = rd(SameSrc);
                    const int32_t samePrev = rd(SamePrev);
                    const int32_t diffSrc = rd(DiffSrc);
                    wr(SameDst, reflect(in, sameSrc, samePrev));
                    const int32_t diffPrev = rd(DiffPrev);
                    const int32_t comb1 = rd(Comb1);
                    wr(DiffDst, reflect(in, diffSrc, diffPrev));
                    const int32_t comb2 = rd(Comb2);
                    const int32_t comb3 = rd(Comb3);
                    const int32_t comb4 = rd(Comb4);
                    const int32_t apf1Old = rd(Apf1Old), apf2Old = rd(Apf2Old);
                    int32_t out = clamp16(mul(comb1, coef(vCOMB1)) + mul(comb2, coef(vCOMB2)) + mul(comb3, coef(vCOMB3)) + mul(comb4, coef(vCOMB4)));
                    const int32_t a1v = coef(vAPF1), a2v = coef(vAPF2);
                    const int32_t apf1 = clamp16(out - mul(a1v, apf1Old));
                    out = clamp16(mul(apf1, a1v) + apf1Old);
                    const int32_t apf2 = clamp16(out - mul(a2v, apf2Old));
                    out = clamp16(mul(apf2, a2v) + apf2Old);
                    wr(Apf1Dst, apf1);
                    wr(Apf2Dst, apf2);
                    value = out;
                    if (right) // (the area moves on one place a left-right pair)
                        areaPos = areaPos + 1u == size ? 0u : areaPos + 1u;
                    quiet = 0;
                }
                else if (quiet < kReverbSpan)
                    ++quiet;
                output[right][at] = output[right][at + 64u] = static_cast<int16_t>(value);
                output[right ^ 1u][at] = output[right ^ 1u][at + 64u] = 0;
                if (quiet >= kReverbSpan)
                    outLeft = outRight = 0; // (what the filters give then: their window holds only the 0s written since)
                else
                {
                    outLeft = clamp16(reverbFilter(&output[0][first]) >> 14);
                    outRight = clamp16(reverbFilter(&output[1][first]) >> 14);
                }
                ++sample;
            }
        };
        Reverb g_reverb;

        double g_mixedUntil = -1.0;
        double g_fraction = 0.0;

        // TF_NATIVE_SOUND_SOLO=<prefix>[,<prefix>...] (a measuring aid, 2026-11-11): only the voices whose tone's
        // name starts with one of these (any case) are heard, and no stream - the sounds under test alone in a dump
        // (PS2X_AUDIO_DUMP), for comparing envelopes and fades. Everything still plays on, unheard.
        const std::vector<std::string> &soloPrefixes()
        {
            static const std::vector<std::string> list = [] {
                std::vector<std::string> v;
                const char *env = std::getenv("TF_NATIVE_SOUND_SOLO");
                std::string cur;
                for (const char *c = env; c && *c; ++c)
                {
                    if (*c == ',')
                    {
                        if (!cur.empty())
                            v.push_back(cur);
                        cur.clear();
                    }
                    else
                        cur.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*c))));
                }
                if (!cur.empty())
                    v.push_back(cur);
                return v;
            }();
            return list;
        }
        bool soloHeard(const Voice &v)
        {
            const auto &list = soloPrefixes();
            if (list.empty())
                return true;
            std::string name = v.tone ? v.tone->name : v.toneName;
            for (char &c : name)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (const std::string &p : list)
                if (name.rfind(p, 0) == 0)
                    return true;
            return false;
        }
        // (TODO 213) toPad: the game alerts go to padL / padR instead of the PC's mix (pad_speaker.h).
        void mixVoices(float &dryL, float &dryR, float &sendL, float &sendR, bool toPad, float &padL, float &padR)
        {
            const bool req = reqLog(); // (fixed for the process: read once a sample, not once a voice)
            for (auto &[handle, v] : g_voices)
            {
                if (v.phase == Phase::Off || !v.tone)
                    continue;
                stepEnvelope(v);
                if (v.phase == Phase::Off)
                {
                    reqEnd(handle, v, "released");
                    continue;
                }
                // A voice that cannot be heard this sample - its envelope at 0, or both volume words 0 -
                // would add exactly 0 to every bus: it is not interpolated or added (its envelope, counter
                // and data go on all the same). Only where that is certain: a level over 1 or not a
                // number, or a dry or send level not a number (never so from the game), take the long way.
                const bool silent = (v.level == 0.0f || (v.gainLeft == 0.0f && v.gainRight == 0.0f && std::fabs(v.level) <= 1.0f)) &&
                                    !std::isnan(v.dry) && !std::isnan(v.send);
                // (With the SPU2's envelope the level is the hardware's own: an instrument whose decay is the fastest (most of them,
                // and every plain tone: 0D0Fh) drops to half on the step after its attack - the 0.5 that kOutputGain stood in for
                // until now, as measured on the emulated SPU2. Its voices so take x2 here: the mix as the emulated SPU2's,
                // instrument by instrument, where the ramps had every voice at the same level.)
                const float spuGain = spu2Envelope() ? 2.0f : 1.0f;
                const float sample = silent ? 0.0f : static_cast<float>(gauss(v.history, v.counter)) * v.level * spuGain;
                v.counter += v.pitchReg;
                bool ended = false;
                while (v.counter >= 0x1000u && !ended)
                {
                    v.counter -= 0x1000u;
                    ended = !fetch(v);
                }
                if (ended)
                {
                    v.phase = Phase::Off; // the data ended (no loop): the channel is free at once
                    reqEnd(handle, v, "data ended");
                }
                if (!soloHeard(v))
                    continue;
                const float l = sample * v.gainLeft, r = sample * v.gainRight;
                if (req)
                    v.peak = std::max(v.peak, v.level * std::max(std::fabs(v.gainLeft), std::fabs(v.gainRight)));
                if (silent)
                    continue;
                if (toPad && v.toPad)
                {
                    padL += l * v.dry;
                    padR += r * v.dry;
                    continue;
                }
                dryL += l * v.dry;
                dryR += r * v.dry;
                sendL += l * v.send;
                sendR += r * v.send;
            }
        }

        void mixStreams(float &dryL, float &dryR, bool toPad, float &padL, float &padR)
        {
            const float keep[4] = {dryL, dryR, padL, padR}; // (TF_NATIVE_SOUND_SOLO: no stream heard)
            struct Restore
            {
                const float *k;
                float &a, &b, &c, &d;
                ~Restore()
                {
                    if (!soloPrefixes().empty())
                    {
                        a = k[0];
                        b = k[1];
                        c = k[2];
                        d = k[3];
                    }
                }
            } restore{keep, dryL, dryR, padL, padR};
            for (auto &[handle, s] : g_streams)
            {
                if (!s.playing)
                    continue;
                if (s.eeFed)
                {
                    if (s.eeLeft.empty())
                    {
                        if (s.flushed)
                            endStream(s);
                        continue;
                    }
                    const float g = s.channelLoudness ? 1.0f : s.loudness;
                    dryL += static_cast<float>(s.eeLeft.front()) * g * s.left[0];
                    dryR += static_cast<float>(s.eeRight.front()) * g * s.right[1];
                    s.eeLeft.pop_front();
                    s.eeRight.pop_front();
                    if (++s.framesPlayed >= static_cast<uint64_t>(s.chunksConsumed + 1) * s.chunkFrames && s.chunkFrames)
                        ++s.chunksConsumed;
                    continue;
                }
                const uint32_t channels = std::min<uint32_t>(s.header.channels, 8u);
                float l = 0.0f, r = 0.0f;
                for (uint32_t c = 0; c < channels; ++c)
                {
                    if (s.left[c] == 0.0f && s.right[c] == 0.0f)
                        continue; // (a channel at 0 on both sides would add exactly 0: not interpolated)
                    const float v = static_cast<float>(gauss(s.history[c].data(), s.counter));
                    l += v * s.left[c];
                    r += v * s.right[c];
                }
                // (the SPU2 voices of a stream get the channel loudness as their volume words: the
                // stream's loudness only where no channel loudness was sent)
                const float g = s.channelLoudness ? 1.0f : s.loudness;
                (toPad && s.toPad ? padL : dryL) += l * g;
                (toPad && s.toPad ? padR : dryR) += r * g;
                s.counter += s.pitchReg;
                while (s.counter >= 0x1000u)
                {
                    s.counter -= 0x1000u;
                    if (!fetchStream(s))
                    {
                        endStream(s);
                        break;
                    }
                }
            }
        }
    }

    // ---- the disc -------------------------------------------------------------------------
    void setDisc(const std::filesystem::path &cdRoot)
    {
        g_disc = cdRoot;
        g_reverb.loadPresets();
        loadSirensTables(g_disc / "IOP" / "SIRENS3.IRX");
    }

    namespace
    {
        std::string g_archiveName; // (the mounted one: a save state's record)
    }

    void mountArchive(const std::string &archive)
    {
        g_archiveName = archive;
        const std::filesystem::path p = discPath(archive);
        if (!g_archive.open(p))
            std::fprintf(stderr, "[native sound] cannot open the sound archive %s\n", p.string().c_str());
    }

    int32_t createLibrary()
    {
        const int32_t handle = g_nextLibrary++;
        g_libraries[handle] = std::make_unique<data::ToneLibrary>();
        return handle;
    }

    namespace
    {
        // Voices playing a library's tones stop and forget them (the server frees its sound RAM).
        void forgetTones(const data::ToneLibrary &lib)
        {
            if (lib.tones.empty())
                return;
            const Tone *first = &lib.tones.front(), *last = &lib.tones.back();
            for (auto &[h, v] : g_voices)
                if (v.tone && v.tone >= first && v.tone <= last)
                {
                    v.tone = nullptr;
                    v.phase = Phase::Off;
                }
        }
    }

    void destroyLibrary(int32_t handle)
    {
        auto it = g_libraries.find(handle);
        if (it == g_libraries.end())
            return;
        forgetTones(*it->second);
        g_libraries.erase(it);
    }

    bool loadLibrary(int32_t handle, const std::string &path)
    {
        auto it = g_libraries.find(handle);
        if (it == g_libraries.end())
        {
            // (a handle the game made before a save state whose record did not have it yet: made now,
            // else its sounds would be missing until the level is loaded again)
            std::fprintf(stderr, "[native sound] tone library %d was not made: made for '%s'\n", handle, path.c_str());
            it = g_libraries.emplace(handle, std::make_unique<data::ToneLibrary>()).first;
        }
        data::ToneLibrary lib;
        if (!data::loadToneLibrary(g_archive, path, lib))
        {
            std::fprintf(stderr, "[native sound] tone library '%s' not loaded\n", path.c_str());
            return false;
        }
        forgetTones(*it->second); // (a library loaded again: its old tones go)
        *it->second = std::move(lib);
        return true;
    }

    // Below every handle the game makes (0 up): the lookups (setTone) go from the highest down,
    // so its tones are found only where no library of the game's has the name.
    constexpr int32_t kFallbackLibrary = -1;

    void setFallbackLibrary(const std::string &path)
    {
        auto it = g_libraries.find(kFallbackLibrary);
        if (path.empty())
        {
            if (it != g_libraries.end())
                destroyLibrary(kFallbackLibrary);
            return;
        }
        if (it != g_libraries.end() && it->second && it->second->path == path)
            return;
        g_libraries[kFallbackLibrary] = std::make_unique<data::ToneLibrary>();
        if (loadLibrary(kFallbackLibrary, path))
            std::fprintf(stderr, "[native sound] '%s' looked in last, for the tones its level's revisit bank lacks\n", path.c_str());
        else
            g_libraries.erase(kFallbackLibrary);
    }

    // ---- voices -----------------------------------------------------------------------------
    void createVoice(int32_t handle) { g_voices[handle] = Voice{}; }
    void destroyVoice(int32_t handle)
    {
        auto it = g_voices.find(handle);
        if (it != g_voices.end() && it->second.phase != Phase::Off)
            reqEnd(handle, it->second, "destroyed while playing");
        g_voices.erase(handle);
    }

    void setTone(int32_t handle, const std::string &name)
    {
        Voice &v = g_voices[handle];
        v.tone = nullptr;
        v.toneName = name;
        v.toPad = pad_speaker::alertTone(name);
        v.envelope = kToneEnvelope;
        for (auto it = g_libraries.rbegin(); it != g_libraries.rend() && !v.tone; ++it)
        {
            if (const Tone *t = it->second->tone(name))
                v.tone = t;
            else if (const data::Instrument *in = it->second->instrument(name))
            {
                const std::string toneName = in->regions.empty() ? in->toneName : in->regions.front().toneName;
                for (auto jt = g_libraries.rbegin(); jt != g_libraries.rend() && !v.tone; ++jt)
                    v.tone = jt->second->tone(toneName);
                if (in->envelope.given)
                    v.envelope = in->envelope;
            }
        }
        if (g_sirens.ok)
        {
            // (a tone without an instrument: SIRENS3's all-zero times with sustain 1 - not kToneEnvelope's 5 ms)
            sirensAdsr(v.envelope.given ? v.envelope : Envelope{}, v.adsr1, v.adsr2);
        }
        if (!v.tone)
        {
            // (once a name: one sound asked for again and again hid the others behind a count)
            static std::set<std::string> missing;
            if (missing.size() < 200u && missing.insert(name).second)
            {
                std::string have;
                for (const auto &[h, lib] : g_libraries)
                    if (lib)
                        have += " " + std::to_string(h) + "=" + (lib->path.empty() ? std::string("(empty)") : lib->path);
                std::fprintf(stderr, "[native sound] tone '%s' not found; libraries:%s\n", name.c_str(), have.c_str());
            }
        }
        prepareVoice(v);
    }

    void updateVoice(int32_t handle, uint16_t volLeft, uint16_t volRight, float pitch, int32_t priority, float send, float dry)
    {
        Voice &v = g_voices[handle];
        v.gainLeft = volumeGain(volLeft);
        v.gainRight = volumeGain(volRight);
        v.volumeWords[0] = volLeft;
        v.volumeWords[1] = volRight;
        v.pitch = std::isfinite(pitch) ? pitch : 1.0f;
        v.priority = priority;
        v.updated = true;
        v.send = std::clamp(send, 0.0f, 1.0f);
        v.dry = std::clamp(dry, 0.0f, 1.0f);
        prepareVoice(v);
    }

    // (TODO 290 (10), the user's choice 2026-11-11) The vehicle's engine loops fade out over 0.15 s when the game keys
    // them off at a transformation, instead of the PS2's cut: the game keys them off in the frame packet that starts
    // the transformation's sound (Transform_Car / Transform_Robot / Transform_Fail), and Optimus's truck engine
    // (Truck_ThrottleOn/Off) has a release time of 0 - the SPU2's fastest, about 2 ms. Only an engine loop (the
    // truck's, Red Alert's jeep, the shared eng_ ones) whose own release is shorter than the fade, and only at a
    // transformation; every other key-off is the game's own. TF_ENGINE_SOFT_STOP=0: the console's cut.
    namespace
    {
        constexpr float kSoftStopSeconds = 0.15f;
        std::vector<int32_t> g_engineStops; // engine loops keyed off in this packet
        double g_transformKeyed = -1e9;      // the mix clock of the last transformation sound's key-on
        bool g_transformThisPacket = false;

        bool softStopOn()
        {
            static const bool on = [] {
                const char *env = std::getenv("TF_ENGINE_SOFT_STOP");
                return !(env && *env == '0');
            }();
            return on;
        }

        bool startsWith(const std::string &s, const char *prefix)
        {
            const size_t n = std::strlen(prefix);
            if (s.size() < n)
                return false;
            for (size_t i = 0; i < n; ++i)
                if (std::tolower(static_cast<unsigned char>(s[i])) != prefix[i])
                    return false;
            return true;
        }

        bool engineLoop(const Voice &v)
        {
            return v.tone && v.tone->loop &&
                   (startsWith(v.toneName, "truck_") || startsWith(v.toneName, "jeep_") || startsWith(v.toneName, "eng_"));
        }

        bool transformSound(const std::string &tone) { return startsWith(tone, "transform_"); }

        void softStop(int32_t handle, Voice &v)
        {
            if (v.phase != Phase::Release || v.envelope.release >= kSoftStopSeconds || v.level <= 0.0f)
                return;
            v.phaseLeft = kSoftStopSeconds;
            v.releaseStep = v.level / static_cast<float>(kSoftStopSeconds * kRate);
            v.softStop = true;
            static const bool log = std::getenv("TF_ENGINE_SOFT_STOP_LOG") != nullptr;
            if (log || reqLog())
                std::fprintf(stderr, "[ns] %.3f engine loop h %d '%s' fades out over %.2f s (a transformation)\n", g_mixedUntil, handle,
                             toneOf(v), kSoftStopSeconds);
        }
    }

    void play(int32_t handle)
    {
        Voice &v = g_voices[handle];
        if (handle >= 0)
        {
            pad_speaker::noteToneKeyed(v.toneName); // (TODO 213: an HQ call's beep and its answer)
            // TF_PAD_SPEAKER_LOG=1: each game alert keyed, and every tone with it (to find the alerts' names).
            static const char *padLog = std::getenv("TF_PAD_SPEAKER_LOG");
            if (padLog && (v.toPad || *padLog == '2'))
                std::fprintf(stderr, "[pad speaker] %.3f tone '%s'%s\n", g_mixedUntil, v.toneName.c_str(),
                             v.toPad ? " -> the controller's speaker" : "");
        }
        if (!v.tone)
        {
            if (reqLog() && handle >= 0)
                std::fprintf(stderr, "[ns req] %.3f kon h %d '%s' -> NO TONE\n", g_reqClock, handle, v.toneName.c_str());
            return;
        }
        // A free channel, else the least important one is taken (if it is less important than this one).
        int active = 0;
        Voice *lowest = nullptr;
        for (auto &[h, o] : g_voices)
            if (h != handle && o.phase != Phase::Off)
            {
                ++active;
                if (!lowest || lessImportant(o, *lowest) || (o.priority == lowest->priority && o.keyedAt < lowest->keyedAt))
                    lowest = &o;
            }
        const bool wasPlaying = v.phase != Phase::Off;
        if (reqLog() && wasPlaying)
            reqEnd(handle, v, "keyed again");
        if (active >= capacity())
        {
            if (!lowest || lessImportant(v, *lowest))
            {
                if (reqLog())
                    std::fprintf(stderr, "[ns req] %.3f kon h %d '%s' prio %d%s vol %04x %04x active %d/%d -> REFUSED (lowest prio %d '%s')\n",
                                 g_reqClock, handle, toneOf(v), v.priority, v.updated ? "" : " (no update yet)", v.volumeWords[0],
                                 v.volumeWords[1], active, capacity(), lowest ? lowest->priority : 0, lowest ? toneOf(*lowest) : "-");
                return; // no channel: the voice does not play
            }
            if (reqLog())
            {
                int32_t stolen = 0;
                for (auto &[h, o] : g_voices)
                    if (&o == lowest)
                        stolen = h;
                std::fprintf(stderr, "[ns req] %.3f kon h %d '%s' prio %d steals h %d '%s' prio %d (active %d/%d)\n", g_reqClock, handle,
                             toneOf(v), v.priority, stolen, toneOf(*lowest), lowest->priority, active, capacity());
                reqEnd(stolen, *lowest, "STOLEN");
            }
            lowest->phase = Phase::Off;
        }
        else if (reqLog())
            std::fprintf(stderr, "[ns req] %.3f kon h %d '%s' prio %d%s vol %04x %04x pitch %.3f active %d/%d\n", g_reqClock, handle,
                         toneOf(v), v.priority, v.updated ? "" : " (no update yet)", v.volumeWords[0], v.volumeWords[1], v.pitch, active,
                         capacity());
        v.keyedTime = g_reqClock;
        v.peak = 0.0f;
        v.position = 0;
        v.counter = 0;
        std::fill(std::begin(v.history), std::end(v.history), int16_t(0));
        v.level = 0.0f;
        v.env = 0;
        v.envCounter = 0u;
        v.keyedAt = ++g_keyCount;
        v.softStop = false;
        if (handle >= 0 && transformSound(v.toneName))
        {
            g_transformKeyed = g_mixedUntil;
            g_transformThisPacket = true;
        }
        startPhase(v, v.envelope.delay > 0.0f ? Phase::Delay : Phase::Attack);
        fetch(v);
        // TF_NATIVE_SOUND_KON=1: each key-on as the SPU2 register log has it (PS2X_SPU2_LOG=2), to compare.
        static const bool kon = std::getenv("TF_NATIVE_SOUND_KON") != nullptr;
        if (kon)
        {
            double sum = 0.0;
            int peak = 0;
            for (const int16_t x : v.tone->pcm)
            {
                sum += static_cast<double>(x) * x;
                peak = std::max(peak, std::abs(static_cast<int>(x)));
            }
            const double rms = v.tone->pcm.empty() ? 0.0 : std::sqrt(sum / static_cast<double>(v.tone->pcm.size()));
            std::fprintf(stderr, "[ns kon] %.3f h %d tone '%s' (%zu samples, rms %.0f, peak %d, %u Hz, loop %d) pitch %04x vol %04x %04x prio %d send %.2f dry %.2f env %.3f/%.3f/%.3f/%.3f adsr %04x %04x active %d\n",
                         g_mixedUntil, handle, v.tone->name.c_str(), v.tone->pcm.size(), rms, peak, v.tone->sampleRate, v.tone->loop ? 1 : 0,
                         pitchRegister(v.tone->sampleRate, v.pitch), v.volumeWords[0], v.volumeWords[1], v.priority, v.send, v.dry,
                         v.envelope.attack, v.envelope.decay, v.envelope.sustain, v.envelope.release, v.adsr1, v.adsr2, active);
        }
    }

    void stop(int32_t handle)
    {
        auto it = g_voices.find(handle);
        if (it != g_voices.end() && it->second.phase != Phase::Off && it->second.phase != Phase::Release)
        {
            if (reqLog() && handle >= 0)
                std::fprintf(stderr, "[ns req] %.3f koff h %d '%s' after %.3f s peak %.3f\n", g_reqClock, handle, toneOf(it->second),
                             g_reqClock - it->second.keyedTime, it->second.peak);
            startPhase(it->second, Phase::Release);
            it->second.softStop = false;
            if (handle >= 0 && softStopOn() && engineLoop(it->second))
            {
                if (g_transformThisPacket || g_mixedUntil - g_transformKeyed < 0.25)
                    softStop(handle, it->second);
                else
                    g_engineStops.push_back(handle); // (the transformation's key-on may come later in this packet)
            }
        }
    }

    void endPacket()
    {
        if (g_transformThisPacket)
            for (int32_t h : g_engineStops)
                if (auto it = g_voices.find(h); it != g_voices.end())
                    softStop(h, it->second);
        g_engineStops.clear();
        g_transformThisPacket = false;
    }

    void rewind(int32_t handle) { play(handle); }

    // The key-ons of a frame packet held until its channel assignment (tag 0x25, as SIRENS3 assigns
    // its channels there, after the packet's voice updates), then made most important first.
    namespace
    {
        std::vector<int32_t> g_pendingKeys;
    }
    void queuePlay(int32_t handle)
    {
        if (std::find(g_pendingKeys.begin(), g_pendingKeys.end(), handle) == g_pendingKeys.end())
            g_pendingKeys.push_back(handle);
    }
    void assignChannels()
    {
        if (g_pendingKeys.empty())
            return;
        std::vector<int32_t> keys;
        keys.swap(g_pendingKeys);
        std::stable_sort(keys.begin(), keys.end(), [](int32_t a, int32_t b) { return lessImportant(g_voices[b], g_voices[a]); });
        for (int32_t h : keys)
            play(h);
    }
    void forgetPlay(int32_t handle)
    {
        g_pendingKeys.erase(std::remove(g_pendingKeys.begin(), g_pendingKeys.end(), handle), g_pendingKeys.end());
    }

    void silenceVoices()
    {
        int n = 0;
        for (auto &[h, v] : g_voices)
        {
            if (v.phase != Phase::Off && h >= 0)
                ++n;
            v.phase = Phase::Off;
        }
        if (reqLog())
            std::fprintf(stderr, "[ns req] %.3f SILENCE all voices (%d were playing)\n", g_reqClock, n);
    }

    void setReservedChannels(int channels)
    {
        g_reserved = std::clamp(channels, 0, kChannels);
        if (reqLog())
            std::fprintf(stderr, "[ns req] %.3f reserved channels %d (voices get %d)\n", g_reqClock, g_reserved, capacity());
    }

    // ---- streams ----------------------------------------------------------------------------
    int32_t createStream(bool pcm, bool eeFed)
    {
        int32_t handle = 0;
        while (g_streams.count(handle))
            ++handle;
        Stream &s = g_streams[handle];
        s.pcm = pcm;
        s.eeFed = eeFed;
        if (eeFed)
        {
            // Movie audio comes in left-right pairs at 48 kHz; its loudness is set by tag 0x40.
            s.left[0] = 1.0f;
            s.right[1] = 1.0f;
            s.chunkFrames = pcm ? 2048u : 7168u; // 8 KB: PCM 2048 frames; ADPCM 4 KB a channel
        }
        return handle;
    }

    void destroyStream(int32_t handle)
    {
        if (auto it = g_streams.find(handle); it != g_streams.end() && it->second.toPad && !it->second.finished)
            pad_speaker::noteAlertStreamEnded(); // (TODO 213)
        g_streams.erase(handle);
    }

    void openStream(int32_t handle, const std::string &name)
    {
        Stream &s = g_streams[handle];
        if (s.toPad && !s.finished)
            pad_speaker::noteAlertStreamEnded(); // (its last message replaced before it ended)
        s.toPad = pad_speaker::alertStream(name); // (TODO 213: an HQ call's voice messages)
        if (s.toPad)
            std::fprintf(stderr, "[pad speaker] %.3f '%s': an HQ call's message, for the controller's speaker\n", g_mixedUntil,
                         name.c_str());
        s.isAlbum = false;
        s.album = data::Album{}; // (TODO 85: the stream's previous album must not stay - see loadAlbum)
        s.pendingTune.clear();
        s.file = name;
        s.finished = false;
        s.playing = false;
        s.index = 0;
        s.tune = s.part = s.repeatsDone = 0;
        s.counter = 0;
        for (auto &h : s.history)
            h.fill(0);
        const std::string n = data::normalise(name);
        if (n.size() > 6 && n.compare(n.size() - 6, 6, ".ALB_T") == 0)
        {
            std::vector<uint8_t> text;
            std::ifstream f(discPath(name), std::ios::binary);
            if (f)
                text.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if ((text.empty() && !g_archive.read(name, text)) || !data::loadAlbum(text, s.album) || s.album.tunes.empty())
            {
                std::fprintf(stderr, "[native sound] album '%s' not readable\n", name.c_str());
                return;
            }
            s.isAlbum = true;
            const data::Album::Tune &t = s.album.tunes.front();
            if (!t.parts.empty())
                openPart(s, t.parts.front().file);
            std::fprintf(stderr, "[native sound] album '%s' (\"%s\"): %zu tune(s), plays '%s' part 0 '%s'\n", name.c_str(),
                         s.album.name.c_str(), s.album.tunes.size(), t.name.c_str(), t.parts.empty() ? "-" : t.parts.front().file.c_str());
        }
        else
            openPart(s, name);
        s.open = true;
        // (each stream the game opens: the music and the ambience of a level - the user heard none
        // in Antarctica, 2026-10-23)
        std::fprintf(stderr, "[native sound] stream %d opened: '%s'%s\n", handle, name.c_str(), s.isAlbum ? " (album)" : "");
    }

    void playStream(int32_t handle)
    {
        Stream &s = g_streams[handle];
        if (s.eeFed)
        {
            s.playing = true;
            return;
        }
        if (!s.in.is_open())
        {
            std::fprintf(stderr, "[native sound] stream %d asked to play, but no file is open ('%s')\n", handle, s.file.c_str());
            return;
        }
        std::fprintf(stderr, "[native sound] stream %d plays '%s' (loudness %g)\n", handle, s.file.c_str(), s.loudness);
        s.playing = true;
        s.finished = false;
        s.index = s.isAlbum ? s.part : 0;
        // the first four samples into the history
        for (int i = 0; i < 4 && s.playing; ++i)
            if (!fetchStream(s))
                endStream(s);
    }

    void stopStream(int32_t handle)
    {
        Stream &s = g_streams[handle];
        s.playing = false;
        if (s.eeFed)
            s.index = -1;
    }

    void closeStream(int32_t handle)
    {
        Stream &s = g_streams[handle];
        s.playing = false;
        s.open = false;
        s.in.close();
    }

    void switchTune(int32_t handle, bool immediately, const std::string &tune)
    {
        Stream &s = g_streams[handle];
        if (!s.isAlbum)
            return;
        if (!immediately && s.playing)
        {
            s.pendingTune = tune;
            return;
        }
        for (size_t i = 0; i < s.album.tunes.size(); ++i)
            if (s.album.tunes[i].name == tune)
            {
                s.tune = static_cast<int>(i);
                s.part = 0;
                s.repeatsDone = 0;
                s.index = 0;
                if (!s.album.tunes[i].parts.empty())
                    openPart(s, s.album.tunes[i].parts.front().file);
                s.finished = false;
            }
    }

    void setTunePart(int32_t handle, int32_t part)
    {
        Stream &s = g_streams[handle];
        const data::Album::Tune *t = currentTune(s);
        if (!t || part < 0 || part >= static_cast<int32_t>(t->parts.size()))
            return;
        s.part = part;
        s.repeatsDone = 0;
        s.index = part;
        openPart(s, t->parts[static_cast<size_t>(part)].file);
    }

    void setStreamLoudness(int32_t handle, uint32_t loudness) { g_streams[handle].loudness = loudnessGain(loudness); }

    void setStreamChannelLoudness(int32_t handle, uint32_t channel, uint32_t left, uint32_t right)
    {
        if (channel >= 8u)
            return;
        Stream &s = g_streams[handle];
        s.channelLoudness = true;
        s.left[channel] = loudnessGain(left);
        s.right[channel] = loudnessGain(right);
    }

    void setStreamPitch(int32_t handle, float pitch, float loudness)
    {
        Stream &s = g_streams[handle];
        if (std::isfinite(pitch) && pitch > 0.0f)
            s.pitch = pitch;
        s.pitchReg = pitchRegister(s.sampleRate, s.pitch);
        (void)loudness;
    }

    void setStreamLooping(int32_t handle, bool looping) { g_streams[handle].looping = looping; }

    void flushStream(int32_t handle) { g_streams[handle].flushed = true; }

    void feedStream(int32_t handle, const uint8_t *bytes, uint32_t size)
    {
        Stream &s = g_streams[handle];
        if (s.pcm)
        {
            // rows of 256 left and 256 right 16-bit samples (the movies' interleave 0x200)
            for (uint32_t row = 0; row + 0x400u <= size; row += 0x400u)
                for (uint32_t i = 0; i < 256u; ++i)
                {
                    int16_t l, r;
                    std::memcpy(&l, bytes + row + i * 2u, 2);
                    std::memcpy(&r, bytes + row + 0x200u + i * 2u, 2);
                    s.eeLeft.push_back(l);
                    s.eeRight.push_back(r);
                }
        }
        else
        {
            // ADPCM (the European disc's movies, SShd type 0x10): the two channels alternate every 0x20 bytes,
            // two 16-byte blocks of one channel, then two of the other (the interleave of all 82 of them).
            // Read as 16-byte alternation, half of each channel's blocks went to the other one: static.
            constexpr uint32_t kInterleave = 0x20u;
            for (uint32_t row = 0; row + 2u * kInterleave <= size; row += 2u * kInterleave)
                for (uint32_t b = 0; b < kInterleave; b += 16u)
                {
                    int16_t l[28], r[28];
                    data::decodeBlock(bytes + row + b, l, s.h1[0], s.h2[0]);
                    data::decodeBlock(bytes + row + kInterleave + b, r, s.h1[1], s.h2[1]);
                    s.eeLeft.insert(s.eeLeft.end(), l, l + 28);
                    s.eeRight.insert(s.eeRight.end(), r, r + 28);
                }
        }
        ++s.chunksQueued;
        s.playing = true;
        s.finished = false;
        s.index = 0;
    }

    int takeConsumedChunks(int32_t handle)
    {
        auto it = g_streams.find(handle);
        if (it == g_streams.end())
            return 0;
        const int n = it->second.chunksConsumed - it->second.chunksReported;
        it->second.chunksReported = it->second.chunksConsumed;
        return n;
    }

    // ---- effects ------------------------------------------------------------------------------
    void setReverbPreset(const std::string &name)
    {
        static const char *const kNames[] = {"Off", "Room", "Studio A", "Studio B", "Studio C", "Hall", "Space", "Echo", "Delay", "Pipe"};
        for (int i = 0; i < 10; ++i)
        {
            std::string a = kNames[i], b = name;
            const auto squash = [](std::string s)
            {
                std::string o;
                for (char c : s)
                    if (c != ' ' && c != '_')
                        o += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return o;
            };
            if (squash(a) == squash(b))
            {
                g_reverb.setMode(i);
                return;
            }
        }
        std::fprintf(stderr, "[native sound] unknown reverb preset '%s'\n", name.c_str());
    }

    void setReverbLevel(float level) { g_reverb.level = std::isfinite(level) ? std::clamp(level, 0.0f, 1.0f) : 0.0f; }
    void setReverbDelay(float) {}
    void setReverbFeedback(float) {}

    // ---- status -----------------------------------------------------------------------------
    std::vector<int32_t> playingVoices()
    {
        std::vector<int32_t> out;
        for (const auto &[h, v] : g_voices)
            if (v.phase != Phase::Off && h >= 0) // (the port's own voices are not the game's)
                out.push_back(h);
        return out;
    }

    // ---- the port's own sounds --------------------------------------------------------------
    namespace
    {
        constexpr int32_t kHostLibraryBase = 0x40000000; // (library handles the game never gets)
        std::map<std::string, int32_t> g_hostLibraries; // path -> handle, -1 when it failed
        int32_t hostVoice(int id) { return -1 - id; }
    }

    bool hostLoop(int id, const std::filesystem::path &folder, const std::string &library, const std::string &tone, float gain, float pitch,
                  float pan)
    {
        const std::string key = folder.string() + "|" + library;
        auto lib = g_hostLibraries.find(key);
        if (lib == g_hostLibraries.end())
        {
            const int32_t handle = kHostLibraryBase + static_cast<int32_t>(g_hostLibraries.size());
            bool ok = false;
            data::ToneLibrary loaded;
            if (folder.empty())
                ok = data::loadToneLibrary(g_archive, library, loaded);
            else
            {
                data::Archive files;
                ok = files.openFolder(folder) && data::loadToneLibrary(files, library, loaded);
            }
            if (ok)
                g_libraries[handle] = std::make_unique<data::ToneLibrary>(std::move(loaded));
            lib = g_hostLibraries.emplace(key, ok ? handle : -1).first;
            std::fprintf(stderr, "[native sound] the port's library '%s' %s\n", library.c_str(), ok ? "loaded" : "is not on the disc");
        }
        if (lib->second < 0)
            return false;
        const int32_t h = hostVoice(id);
        auto v = g_voices.find(h);
        if (v == g_voices.end())
        {
            createVoice(h);
            setTone(h, tone);
            v = g_voices.find(h);
        }
        if (!v->second.tone)
            return false;
        const float g = std::clamp(gain, 0.0f, 1.0f), p = std::clamp(pan, -1.0f, 1.0f);
        const auto word = [](float x) { return static_cast<uint16_t>(std::lround(std::clamp(x, 0.0f, 1.0f) * 0x3FFF)); };
        // (the priority of the game's own immediate voices - the player's weapons and car engine: 9;
        // until TODO 106 this was 0x7FFFFFFF, which in the corrected order is the least important)
        updateVoice(h, word(g * std::min(1.0f, 1.0f - p)), word(g * std::min(1.0f, 1.0f + p)), pitch, 9, 0.0f, 1.0f);
        if (v->second.phase == Phase::Off || v->second.phase == Phase::Release)
            play(h);
        return true;
    }

    void hostStop(int id) { stop(hostVoice(id)); }

    std::vector<std::pair<int32_t, int32_t>> streams()
    {
        std::vector<std::pair<int32_t, int32_t>> out;
        for (const auto &[h, s] : g_streams)
            out.emplace_back(h, s.index);
        return out;
    }

    // ---- mixing -------------------------------------------------------------------------------
    namespace
    {
        // What renderAhead mixed in all, in seconds (TF_NATIVE_SOUND_AHEAD_LOG).
        double g_aheadTotal = 0.0;

        void mixFrames(size_t frames);
    }

    void render(double seconds)
    {
        if (g_mixedUntil < 0.0 || seconds < g_mixedUntil)
            g_mixedUntil = seconds; // (the first mix, or the clock went back: a state, a reset)
        double span = seconds - g_mixedUntil;
        g_mixedUntil = seconds;
        g_reqClock = seconds;
        if (span > kMaxRenderSeconds)
            span = kMaxRenderSeconds;
        g_fraction += span * kRate;
        const size_t frames = static_cast<size_t>(g_fraction);
        g_fraction -= static_cast<double>(frames);
        mixFrames(frames);
    }

    // The frames mixed ahead are not taken off the game's later mixes: the time a hitch costs the
    // game is lost for good (the EE scheduler catches up two VBlank periods at most, then goes on
    // from now), so the game's clock never covers it again. Taking it off (tried first, the run-ahead
    // as a debt the game's mixes paid back) made the output sag again after each run-ahead, in a
    // loop: its fill held at ~60 ms instead of 80 and its rate at 0.995 for good (test PC,
    // 2026-10-28). The at most two periods a game does catch up come in on top of the fill (up to
    // ~30 ms more for a moment), which the playback rate takes back.
    void renderAhead(size_t frames)
    {
        if (g_mixedUntil < 0.0 || frames == 0u)
            return; // (nothing mixed yet: the game's first mix starts the clock)
        mixFrames(frames);
        g_aheadTotal += static_cast<double>(frames) / kRate;
        static const bool log = std::getenv("TF_NATIVE_SOUND_AHEAD_LOG") != nullptr;
        if (log)
            std::fprintf(stderr, "[ns ahead] %.3f mixed %.1f ms ahead (%.2f s in all)\n", g_mixedUntil,
                         1000.0 * static_cast<double>(frames) / kRate, g_aheadTotal);
    }

    namespace
    {
    void mixFrames(size_t frames)
    {
        if (frames == 0)
            return;
        // (kept from mix to mix, as pcDelay below: native_sound.cpp lets one caller at a time into the mixer)
        static std::vector<int16_t> out;
        out.resize(frames * 2u);
        // (TODO 213) The DualSense's speaker, while one plays (pad_speaker.h): the game alerts instead of the PC.
        const bool padLive = pad_speaker::live();
        const pad_speaker::Mode padMode = pad_speaker::mode();
        const bool alertsToPad = padLive && padMode == pad_speaker::Mode::Alerts;
        const float padGain = pad_speaker::volume();
        static std::vector<int16_t> pad;
        pad.resize(padLive && padMode != pad_speaker::Mode::Off ? frames * 2u : 0u);
        for (size_t i = 0; i < frames; ++i)
        {
            float dryL = 0.0f, dryR = 0.0f, sendL = 0.0f, sendR = 0.0f, padL = 0.0f, padR = 0.0f;
            mixVoices(dryL, dryR, sendL, sendR, alertsToPad, padL, padR);
            mixStreams(dryL, dryR, alertsToPad, padL, padR);
            int32_t revL = 0, revR = 0;
            g_reverb.process(static_cast<int32_t>(sendL), static_cast<int32_t>(sendR), revL, revR);
            const float l = (dryL + static_cast<float>(revL) * g_reverb.level) * kOutputGain;
            const float r = (dryR + static_cast<float>(revR) * g_reverb.level) * kOutputGain;
            out[i * 2u] = static_cast<int16_t>(std::clamp(std::lround(l), -32768L, 32767L));
            out[i * 2u + 1u] = static_cast<int16_t>(std::clamp(std::lround(r), -32768L, 32767L));
            if (!pad.empty())
            {
                const float pl = (padMode == pad_speaker::Mode::Surround ? l : padL * kOutputGain) * padGain;
                const float pr = (padMode == pad_speaker::Mode::Surround ? r : padR * kOutputGain) * padGain;
                pad[i * 2u] = static_cast<int16_t>(std::clamp(std::lround(pl), -32768L, 32767L));
                pad[i * 2u + 1u] = static_cast<int16_t>(std::clamp(std::lround(pr), -32768L, 32767L));
            }
        }
        // (TODO 213 step 3) Surround: the PC's sound held back by the speaker delay, so it plays together with the
        // controller's (later over Bluetooth). As many frames go out as came in: a longer delay starts with silence,
        // a shorter one skips the frames it no longer needs.
        static std::deque<int16_t> pcDelay;
        const size_t delayFrames = padLive && padMode == pad_speaker::Mode::Surround
                                       ? static_cast<size_t>(pad_speaker::surroundDelayMs(static_cast<int>(
                                             ps2_audio_stream::targetFrames() * 1000u / static_cast<size_t>(kRate)))) *
                                             static_cast<size_t>(kRate) / 1000u
                                       : 0u;
        if (delayFrames == 0u)
        {
            pcDelay.clear();
            ps2_audio_stream::push(out.data(), frames);
        }
        else
        {
            pcDelay.insert(pcDelay.end(), out.begin(), out.end());
            const size_t queued = pcDelay.size() / 2u;
            if (queued > delayFrames + frames) // (the delay was shortened)
                pcDelay.erase(pcDelay.begin(), pcDelay.begin() + static_cast<std::ptrdiff_t>((queued - delayFrames - frames) * 2u));
            const size_t ready = pcDelay.size() / 2u > delayFrames ? pcDelay.size() / 2u - delayFrames : 0u;
            const size_t silence = frames - std::min(frames, ready);
            std::fill(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(silence * 2u), int16_t{0});
            std::copy(pcDelay.begin(), pcDelay.begin() + static_cast<std::ptrdiff_t>((frames - silence) * 2u),
                      out.begin() + static_cast<std::ptrdiff_t>(silence * 2u));
            pcDelay.erase(pcDelay.begin(), pcDelay.begin() + static_cast<std::ptrdiff_t>((frames - silence) * 2u));
            ps2_audio_stream::push(out.data(), frames);
        }
        if (!pad.empty())
            pad_speaker::push(pad.data(), frames);

        // TF_NATIVE_SOUND_KON=2: also every 2 s of sound, what plays and how loud: each voice (tone,
        // volume words, envelope level) and each stream (file, loudness, the channels' left/right).
        static const bool report = [] {
            const char *env = std::getenv("TF_NATIVE_SOUND_KON");
            return env && *env == '2';
        }();
        static double nextReport = 0.0;
        if (report && g_mixedUntil >= nextReport)
        {
            nextReport = g_mixedUntil + 2.0;
            for (const auto &[h, v] : g_voices)
                if (v.phase != Phase::Off && v.tone)
                    std::fprintf(stderr, "[ns now] %.3f voice %d '%s' vol %04x %04x level %.2f pitch %.3f\n", g_mixedUntil, h,
                                 v.tone->name.c_str(), v.volumeWords[0], v.volumeWords[1], v.level, v.pitch);
            for (const auto &[h, s] : g_streams)
                if (s.playing)
                {
                    std::string ch;
                    char b[40];
                    for (uint32_t c = 0; c < std::min<uint32_t>(s.header.channels, 8u); ++c)
                    {
                        std::snprintf(b, sizeof(b), " %.2f/%.2f", s.left[c], s.right[c]);
                        ch += b;
                    }
                    std::fprintf(stderr, "[ns now] %.3f stream %d '%s' loudness %.2f%s channels%s\n", g_mixedUntil, h,
                                 s.isAlbum ? ("album:" + s.file).c_str() : s.file.c_str(), s.loudness,
                                 s.channelLoudness ? " (channel levels)" : "", ch.c_str());
                }
        }
    }
    }

    // ---- save states ---------------------------------------------------------------------------
    namespace
    {
        uint32_t bits(float f)
        {
            uint32_t u;
            std::memcpy(&u, &f, 4);
            return u;
        }
        float unbits(uint32_t u)
        {
            float f;
            std::memcpy(&f, &u, 4);
            return f;
        }
    }

    std::string saveSetup()
    {
        std::ostringstream o;
        o << std::hex;
        o << "A " << g_archiveName << "\n";
        o << "N " << g_nextLibrary << "\n";
        for (const auto &[h, lib] : g_libraries)
            if (h >= 0 && h < kHostLibraryBase && lib)
                o << "L " << h << ' ' << (lib->path.empty() ? std::string("-") : lib->path) << "\n"; // (-: made, not loaded yet)
        if (auto it = g_libraries.find(kFallbackLibrary); it != g_libraries.end() && it->second && !it->second->path.empty())
            o << "F " << it->second->path << "\n";
        o << "R " << g_reverb.mode << ' ' << bits(g_reverb.level) << "\n";
        o << "C " << g_reserved << "\n";
        for (const auto &[h, v] : g_voices)
            if (h >= 0)
                o << "V " << h << ' ' << v.volumeWords[0] << ' ' << v.volumeWords[1] << ' ' << bits(v.pitch) << ' ' << static_cast<uint32_t>(v.priority)
                  << ' ' << bits(v.send) << ' ' << bits(v.dry) << ' ' << (v.tone ? v.toneName : std::string()) << "\n";
        // A voice still sounding (2026-10-25, item 36): where it is, so it goes on after the state
        // is loaded. The game keys a level's looping ambience (Antarctica's wind) once at the
        // level's start and never again; without this line it stayed silent until the next level.
        for (const auto &[h, v] : g_voices)
            if (h >= 0 && v.tone && v.phase != Phase::Off)
                o << "P " << h << ' ' << static_cast<int>(v.phase) << ' ' << bits(v.level) << ' ' << bits(static_cast<float>(v.phaseLeft)) << ' '
                  << bits(v.releaseStep) << ' ' << v.position << ' ' << v.counter << ' ' << v.keyedAt << ' '
                  << static_cast<uint16_t>(v.history[0]) << ' ' << static_cast<uint16_t>(v.history[1]) << ' '
                  << static_cast<uint16_t>(v.history[2]) << ' ' << static_cast<uint16_t>(v.history[3]) << ' ' << v.envCounter << "\n";
        for (const auto &[h, s] : g_streams)
        {
            o << "S " << h << ' ' << s.pcm << ' ' << s.eeFed << ' ' << s.looping << ' ' << s.playing << ' ' << bits(s.loudness) << ' '
              << s.channelLoudness << ' ' << bits(s.pitch) << ' ' << s.part;
            for (int c = 0; c < 8; ++c)
                o << ' ' << bits(s.left[static_cast<size_t>(c)]) << ' ' << bits(s.right[static_cast<size_t>(c)]);
            const std::string tune = s.isAlbum && s.tune >= 0 && s.tune < static_cast<int>(s.album.tunes.size())
                                         ? s.album.tunes[static_cast<size_t>(s.tune)].name
                                         : std::string("-");
            o << ' ' << (tune.empty() ? std::string("-") : tune) << ' ' << (s.open && !s.eeFed ? s.file : std::string()) << "\n";
        }
        return o.str();
    }

    void loadSetup(const std::string &text)
    {
        reset();
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line))
        {
            std::istringstream l(line);
            l >> std::hex;
            std::string kind;
            l >> kind;
            if (kind == "A")
            {
                std::string a;
                std::getline(l >> std::ws, a);
                if (!a.empty())
                    mountArchive(a);
            }
            else if (kind == "N")
                l >> g_nextLibrary;
            else if (kind == "F")
            {
                std::string path;
                std::getline(l >> std::ws, path);
                setFallbackLibrary(path);
            }
            else if (kind == "L")
            {
                int32_t h = 0;
                std::string path;
                l >> h;
                std::getline(l >> std::ws, path);
                g_libraries[h] = std::make_unique<data::ToneLibrary>();
                if (path != "-")
                    loadLibrary(h, path);
            }
            else if (kind == "R")
            {
                int mode = 0;
                uint32_t level = 0;
                l >> mode >> level;
                g_reverb.setMode(mode);
                g_reverb.level = unbits(level);
            }
            else if (kind == "C")
                l >> g_reserved;
            else if (kind == "V")
            {
                int32_t h = 0;
                uint32_t vl = 0, vr = 0, pitch = 0, priority = 0, send = 0, dry = 0;
                std::string tone;
                l >> h >> vl >> vr >> pitch >> priority >> send >> dry;
                std::getline(l >> std::ws, tone);
                createVoice(h);
                if (!tone.empty())
                    setTone(h, tone);
                updateVoice(h, static_cast<uint16_t>(vl), static_cast<uint16_t>(vr), unbits(pitch), static_cast<int32_t>(priority), unbits(send),
                            unbits(dry));
            }
            else if (kind == "P") // (after its V line: a voice that was sounding; older states have none)
            {
                int32_t h = 0;
                int phase = 0;
                uint32_t level = 0, left = 0, step = 0, position = 0, counter = 0;
                uint64_t keyed = 0;
                uint32_t hist[4] = {};
                l >> h >> phase >> level >> left >> step >> position >> counter >> keyed >> hist[0] >> hist[1] >> hist[2] >> hist[3];
                auto it = g_voices.find(h);
                if (!l || it == g_voices.end() || !it->second.tone || phase <= static_cast<int>(Phase::Off) ||
                    phase > static_cast<int>(Phase::Release))
                    continue;
                Voice &v = it->second;
                v.phase = static_cast<Phase>(phase);
                v.level = unbits(level);
                v.phaseLeft = unbits(left);
                v.releaseStep = unbits(step);
                v.position = std::min<uint32_t>(position, static_cast<uint32_t>(v.tone->pcm.size()));
                v.counter = counter;
                v.keyedAt = keyed;
                g_keyCount = std::max(g_keyCount, keyed);
                for (int i = 0; i < 4; ++i)
                    v.history[i] = static_cast<int16_t>(static_cast<uint16_t>(hist[i]));
                // (the SPU2 envelope's level and its rate counter - older records: the counter from 0)
                v.env = std::clamp(static_cast<int32_t>(std::lround(v.level * 32768.0f)), 0, 0x7FFF);
                uint32_t envCounter = 0u;
                v.envCounter = (l >> envCounter) ? envCounter : 0u;
            }
            else if (kind == "S")
            {
                int32_t h = 0, part = 0;
                int pcm = 0, eeFed = 0, looping = 0, playing = 0, channelLoudness = 0;
                uint32_t loudness = 0, pitch = 0;
                l >> h >> pcm >> eeFed >> looping >> playing >> loudness >> channelLoudness >> pitch >> part;
                uint32_t lr[16] = {};
                for (uint32_t &v : lr)
                    l >> v;
                std::string tune, file;
                l >> tune;
                std::getline(l >> std::ws, file);
                Stream &s = g_streams[h];
                s.pcm = pcm != 0;
                s.eeFed = eeFed != 0;
                if (s.eeFed)
                    s.chunkFrames = s.pcm ? 2048u : 7168u;
                if (!s.eeFed && !file.empty())
                {
                    openStream(h, file);
                    if (tune != "-")
                        switchTune(h, true, tune);
                    if (part > 0)
                        setTunePart(h, part);
                }
                s.looping = looping != 0;
                s.loudness = unbits(loudness);
                s.channelLoudness = channelLoudness != 0;
                s.pitch = unbits(pitch);
                s.pitchReg = pitchRegister(s.sampleRate, s.pitch);
                for (int c = 0; c < 8; ++c)
                {
                    s.left[static_cast<size_t>(c)] = unbits(lr[2 * c]);
                    s.right[static_cast<size_t>(c)] = unbits(lr[2 * c + 1]);
                }
                if (playing && !s.eeFed)
                    playStream(h);
            }
        }
    }

    void reset()
    {
        g_voices.clear();
        g_streams.clear();
        g_libraries.clear();
        g_hostLibraries.clear();
        g_nextLibrary = 0;
        g_mixedUntil = -1.0;
        g_fraction = 0.0;
    }
}
