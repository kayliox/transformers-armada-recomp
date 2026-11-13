// Which shader each draw runs with which data (README.md): the display list between draws read as
// data into a VU1 image of our own (nr_vif.h) - the shaders' data blocks (lights, eye, environment
// matrix, material colour) and the routine table (q4: count; q5 on: routine address / 8 and data
// block, written by func_0023AB70), the microcode images (MPG) by their bytes. At a draw the table
// is identified routine by routine (the same FNV-1a hashes of their bytes as src/vu1_native.cpp, so
// a routine at an address shared by different code in different images is told apart) and its
// blocks copied. The HLSL (nr_d3d.cpp) runs what the routines compute (docs/RENDERER.md).
#include "nr_shader.h"
#include "nr_vif.h"
#include "nr_particles.h"
#include "nr_rain.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace nr
{
    namespace
    {
        uint64_t hashBytes(const uint8_t *bytes, uint32_t size)
        {
            uint64_t hash = 0xCBF29CE484222325ull; // FNV-1a
            for (uint32_t i = 0; i < size; ++i)
                hash = (hash ^ bytes[i]) * 0x100000001B3ull;
            return hash;
        }

        struct Known
        {
            uint32_t start, end;
            uint64_t hash;
            Routine routine;
            // (TODO 290) Found at any address: the game builds each image by putting the shaders' code one after the
            // other (vlRenderDeviceUtility::BuildShaderBlock), so the same routine lands at another address in another
            // image - its code, position-independent, the same bytes.
            bool anywhere = false;
        };

        // (src/vu1_native.cpp's overlay table, the same byte ranges and hashes.)
        constexpr Known kKnown[] = {
            {0x2150u, 0x2298u, 0xEB203B69B13A1286ull, Routine::ScaleColours},
            {0x21B0u, 0x22A0u, 0x74141DCA3C26CA6Full, Routine::MapCoordinates},
            {0x1AC0u, 0x1D10u, 0xE1BBD9635116BFA8ull, Routine::LightNormals},
            {0x1C20u, 0x1E70u, 0xE1BBD9635116BFA8ull, Routine::LightNormals},
            {0x2100u, 0x2350u, 0xE1BBD9635116BFA8ull, Routine::LightNormals}, // (the HQ's images 60a8ddde9d123220, 6f9f0e96b0ab22f8: the same code)
            {0x3660u, 0x3818u, 0x429FB896A6A5D839ull, Routine::FadeByPlane},
            // (TODO 147) The same code at 0x2b00 of image e1472a8c96f62e62 (to its jr's delay slot, 0x2cb8): the FFT
            // ocean's routine (vlxWaterEffect, the "ShaderAlphaFog" block's) - each patch's alpha by the plane the game
            // sets in q0, so the far sea fades out over what lies behind it. Unidentified, the patches kept their
            // packet's alpha (0x80): waves to the horizon where the PS2 shows the dark band (Mid-Atlantic after the boss).
            // TF_NATIVE_OCEANFADE=0: as before.
            {0x2B00u, 0x2CB8u, 0x429FB896A6A5D839ull, Routine::FadeByPlane},
            {0x13B0u, 0x1408u, 0x460D9E912CA3A637ull, Routine::ConstantColour},
            {0x21F0u, 0x2800u, 0xDF0443C249D0BCE8ull, Routine::LightWithHighlight},
            // (TODO 290) vlShader_TextureTransform's code (0x5DFC50), found anywhere: in images 1a707099ecafadff (at 0x3b60)
            // and 83f1438f69bcf7be (at 0x2500) it is the scroll of a level's open warp gate - its swirl (WARP3) and its
            // haze, both drawn with their texture standing still while it was known at 0x1ac0 alone (the HQ's gate,
            // in images where it sits there, turned).
            {0x1AC0u, 0x1C20u, 0x1FE1C6C429DB90A8ull, Routine::TransformTexture, true},
            {0x1C20u, 0x1DF0u, 0xDD4A81CC9C0BA94Dull, Routine::ScaleAndOffsetColours},
            {0x1D10u, 0x21F0u, 0xF72677CDF8F26FDCull, Routine::LitMeshPrepare},
            {0x2800u, 0x2BE0u, 0xA7C29B1C9E68F5B0ull, Routine::LitMeshDraw},
            // Image 422f's 0x2ec0 (0x2EC0 to the end of its drawing part's jr): other code, the same
            // four copies as 0x2800's (tools/vu1_trace.py --spec 4 on a captured batch: the sphere map
            // and Fresnel term in context 1, the two lights on the base texture in context 2, alpha
            // q16.w) - the carrier's doors and hull; unknown, they drew context 1 alone: chrome.
            {0x2EC0u, 0x3290u, 0x5614C7CEA6BDAE45ull, Routine::LitMeshDraw},
            {0x1DF0u, 0x2180u, 0xF2D2F41A18B2057Full, Routine::WaterPrepare},
            {0x2CC0u, 0x3088u, 0xB86C004B89CF27A2ull, Routine::WaterDraw},
            {0x1AC0u, 0x2148u, 0x36D4C8D7756E303Dull, Routine::Instances},
            {0x22A0u, 0x2950u, 0x843D008B96A24BDCull, Routine::Sprites},
            {0x2740u, 0x2DF0u, 0x843D008B96A24BDCull, Routine::Sprites},
            {0x1DF0u, 0x21B0u, 0x6606DB8EEAA72B0Dull, Routine::Projection},
            {0x22A0u, 0x2A30u, 0x1C83D0A51E6431BFull, Routine::ProjectedDraw},
            {0x3CC0u, 0x3E40u, 0x4B83A7D74253AD29ull, Routine::FlatStrip},
            {0x2CF0u, 0x3168u, 0x24C0203B7D9FD81Dull, Routine::FieldSprites},
            // Image 6b20's 0x2030 and 0x25e0 (to their jr's delay slots; tools/vu1_trace.py --spec 2 on
            // captured batches): Cybertron's bubble - the vertex moved by two waves, the sphere map of
            // the view reflected about the normal moved with it, a constant colour and alpha N.L.
            {0x2030u, 0x25E0u, 0x311E9D15C697259Cull, Routine::MembraneWave},
            {0x25E0u, 0x2740u, 0xD375028CD940F78Cull, Routine::MembraneShade},
            // Image 83f1's 0x1e70 (the Starship's light cones on the walls, 55 draws) and image e147's
            // 0x2920 (a rim-lit reflection), the same way.
            {0x1E70u, 0x2220u, 0xA45953DAAD1BB16Bull, Routine::ProjectedLight},
            {0x2920u, 0x2B00u, 0x53ADBAC0901A382Aull, Routine::RimReflection},
            // Image 83f1's 0x2220 (to its jr's delay slot; tools/vu1_trace.py on captured batches of
            // the dash's table 2220 1410 0480, TODO 69): the character's after-image while it dashes.
            {0x2220u, 0x2500u, 0xEB3622E0AEE92A9Full, Routine::Smear},
            // (TODO 162) Image a31d's 0x22c0 (to the delay slot of its branch out, 0x2ad0; tools/vu1_trace.py on captured
            // batches, PS2X_VU1_CAPTURE="22c0/q35@a31d#3"): Cybertron's city - each vertex a building, a box (its centre
            // x, y; ST the half sizes; colour.a the height / 10), the block's q10.w which face the batch draws. Unknown,
            // its three vertices drew as one triangle a batch: the dark shards, and none of the city.
            {0x22C0u, 0x2AD8u, 0x01EBE0476A31640Dull, Routine::CityBlocks},
            // (TODO 290) taShader_ElectricShock's code (0x5FF410-0x5FF7C0; 0x1ac0 of image 83f1438f69bcf7be, read on
            // captured batches, PS2X_VU1_CAPTURE="1ac0/q35 1410/q38 0480/qd@83f1#4"): a stunned creature drawn again as
            // a crackling shell - each vertex pushed out by q0.z along its direction from q2 (the creature's centre),
            // ST that direction's y, z plus q0.xy (the effect's jumping offset), the colour q1 (its flicker). Unknown,
            // it drew the electricShock texture on the mesh's own UVs at full colour: a still purple coat.
            {0x1AC0u, 0x1E70u, 0x40F490F10159C61Eull, Routine::ElectricShock, true},
            // (TODO 305) Image 6b20's 0x1e70 (to its jr's delay slot, 0x2030; tools/vu1_trace.py --spec 2 on captured batches,
            // PS2X_VU1_CAPTURE="1e70/q35 25e0/q37 1410/q3a 0480/qd@6b20#4"): the tractor beam's ball, table 1e70 25e0 1410 0480 -
            // S the normal dotted with the direction to the eye q0 (plus q0.w), T and Q the mesh's. Unknown, the ball drew
            // with its white vertex colour and the mesh's own UVs, added in: a solid white ball (0x25e0 then sends alpha 0).
            {0x1E70u, 0x2030u, 0x160CDCF173F65748ull, Routine::ViewRamp},
        };

        struct Tracker
        {
            vif::State vu;
            uint8_t micro[0x4000] = {};
            uint64_t identifiedAt = ~0ull;                      // the micro version the cache below is for
            std::unordered_map<uint32_t, Routine> identified;    // by address
            bool coreKnown = false;
            bool grassImage = false; // the grass image (its own core; 0x1380 its blades)
            float fieldA[16] = {};   // vf01-vf04 as MSCAL 0x450 last loaded them from q0-q3
            bool haveFieldA = false;
        } t;

        Routine identify(uint32_t address)
        {
            if (t.identifiedAt != t.vu.microVersion)
            {
                t.identified.clear();
                t.identifiedAt = t.vu.microVersion;
                t.coreKnown = hashBytes(t.micro, 0x1AC0u) == 0x337999265B47F13Eull;
                t.grassImage = hashBytes(t.micro, 0x4000u) == 0x831A3306F7AFF255ull;
            }
            auto it = t.identified.find(address);
            if (it != t.identified.end())
                return it->second;
            Routine r = Routine::Unknown;
            if (address == 0x1380u && t.grassImage)
                r = Routine::Grass;
            else if (address == 0x1410u && t.coreKnown)
                r = Routine::TransformKick;
            else if (address == 0x0480u && t.coreKnown)
                r = Routine::Clip;
            else
                for (const Known &k : kKnown)
                {
                    // (TODO 290: TF_NATIVE_ANYWHERE=0 - each routine known at its own address alone, as before;
                    // TF_NATIVE_SHOCK=0 - the electric shock left unknown, as before)
                    static const bool anywhereOff = std::getenv("TF_NATIVE_ANYWHERE") && *std::getenv("TF_NATIVE_ANYWHERE") == '0';
                    static const bool shockOff = std::getenv("TF_NATIVE_SHOCK") && *std::getenv("TF_NATIVE_SHOCK") == '0';
                    if (shockOff && k.routine == Routine::ElectricShock)
                        continue;
                    // (TODO 305: TF_NATIVE_VIEWRAMP=0 - the tractor beam's ball's 0x1e70 left unknown, as before)
                    static const bool rampOff = std::getenv("TF_NATIVE_VIEWRAMP") && *std::getenv("TF_NATIVE_VIEWRAMP") == '0';
                    if (rampOff && k.routine == Routine::ViewRamp)
                        continue;
                    const uint32_t size = k.end - k.start;
                    const bool at = k.start == address || (k.anywhere && !anywhereOff && address + size <= sizeof(t.micro));
                    if (at && hashBytes(t.micro + address, size) == k.hash)
                    {
                        static const bool oceanFadeOff = std::getenv("TF_NATIVE_OCEANFADE") && *std::getenv("TF_NATIVE_OCEANFADE") == '0';
                        if (oceanFadeOff && k.start == 0x2B00u && k.routine == Routine::FadeByPlane)
                            continue;
                        r = k.routine;
                        break;
                    }
                }
            t.identified[address] = r;
            return r;
        }
    }

    void trackDisplayList(const uint8_t *rdram, uint32_t from, uint32_t to, const std::function<void(const uint32_t *, size_t)> &direct)
    {
        if (!t.vu.micro)
            t.vu.micro = t.micro;
        if (!from || !to || to <= from || to - from > (1u << 20))
            return;
        static thread_local std::vector<uint32_t> words; // (kept: a thousand draws a frame come here)
        words.clear();
        // The display list's own chain up to the draw's call (a geometry set's packet is the mesh);
        // as far as it goes when it ends before.
        if (!vif::gather(rdram, from, words, nullptr, {}, to))
            return;
        t.vu.direct = direct;
        vif::run(t.vu, words.data(), words.size(), [](vif::State &st, uint32_t start) {
            if (start == 0x450u) // the set-up program loading field A's screen transform
            {
                std::memcpy(t.fieldA, st.vu[0], 64u);
                t.haveFieldA = true;
            }
        });
        t.vu.direct = nullptr;
    }

    bool gatherList(const uint8_t *rdram, uint32_t from, uint32_t to, std::vector<uint32_t> &out,
                    std::vector<std::pair<uint32_t, uint32_t>> *positions)
    {
        if (!from || !to || to <= from || to - from > (1u << 20))
            return false;
        const size_t at = out.size();
        const size_t positionsAt = positions ? positions->size() : 0u;
        if (!vif::gather(rdram, from, out, nullptr, {}, to, positions))
        {
            out.resize(at);
            if (positions)
                positions->resize(positionsAt);
            return false;
        }
        return true;
    }

    void replayList(const uint32_t *words, size_t count)
    {
        if (!t.vu.micro)
            t.vu.micro = t.micro;
        t.vu.direct = nullptr; // (the 2D was taken on the game's thread)
        vif::run(t.vu, words, count, [](vif::State &st, uint32_t start) {
            if (start == 0x450u)
            {
                std::memcpy(t.fieldA, st.vu[0], 64u);
                t.haveFieldA = true;
            }
        });
    }

    thread_local const uint32_t *t_kickWord = nullptr;

    void replayListRunning(const uint32_t *words, size_t count, const std::function<void(const uint32_t *, size_t)> &direct,
                           const std::function<void(const uint32_t *, size_t)> &kicked)
    {
        if (!t.vu.micro)
            t.vu.micro = t.micro;
        t.vu.direct = direct;
        // (A particle pipeline's program: started by MSCAL at its image, its batches by MSCNT - until a MSCAL elsewhere.)
        static bool inParticles = false;
        static std::vector<uint8_t> packets;
        vif::run(t.vu, words, count, [&](vif::State &st, uint32_t start) {
            if (start == 0x450u)
            {
                std::memcpy(t.fieldA, st.vu[0], 64u);
                t.haveFieldA = true;
            }
            // (and only while that image is still there: an MPG over it - the pipeline evicted - ends it too)
            static uint32_t particleStart = 0u;
            if (start != ~0u)
            {
                inParticles = particles::isParticleProgram(t.micro, start);
                particleStart = start;
                // (debug, TF_NATIVE_PARTICLEVIF=<n>: the VIF commands of the first n stretches a particle program starts in)
                static int vifLog = std::getenv("TF_NATIVE_PARTICLEVIF") ? std::atoi(std::getenv("TF_NATIVE_PARTICLEVIF")) : 0;
                if (inParticles && vifLog > 0)
                {
                    --vifLog;
                    static vif::State scratch;
                    scratch = st;
                    scratch.direct = nullptr;
                    std::fprintf(stderr, "[native render] particle stretch, %zu words:\n", count);
                    vif::run(scratch, words, count, {}, stderr);
                }
            }
            else if (inParticles && !particles::isParticleProgram(t.micro, particleStart))
                inParticles = false;
            // (The rain's batches, TODO 146: their packets built from the same memory - nr_rain.cpp. A pass begins with a
            // MSCAL - the next of the frame's copied blocks - and its three batches follow as MSCNTs; the MSCAL itself, as
            // the tracker sees it, holds the pass before's last batch again, so it draws nothing here.)
            if (!inParticles && rain::isRainBatch(st.vu, t.micro))
            {
                if (start != ~0u)
                {
                    rain::nextPass();
                    return;
                }
                packets.clear();
                rain::build(st.vu, st.itop, packets, start);
                t_kickWord = st.at;
                if (packets.size() >= 16u && kicked)
                    kicked(reinterpret_cast<const uint32_t *>(packets.data()), packets.size() / 16u);
                t_kickWord = nullptr;
                return;
            }
            if (!inParticles)
                return;
            packets.clear();
            particles::run(reinterpret_cast<uint8_t *>(st.vu), t.micro, start, st.tops, st.itop, packets);
            t_kickWord = st.at;
            if (packets.size() >= 16u && kicked)
                kicked(reinterpret_cast<const uint32_t *>(packets.data()), packets.size() / 16u);
            t_kickWord = nullptr;
        });
        t.vu.direct = nullptr;
    }

    bool fieldAMatrix(float out[16])
    {
        if (t.haveFieldA)
            std::memcpy(out, t.fieldA, 64u);
        return t.haveFieldA;
    }

    void snapshotShader(ShaderState &out)
    {
        out = ShaderState{};
        const uint32_t count = t.vu.vu[4][0];
        if (count == 0u || count > kMaxRoutines)
            return;
        out.count = count;
        out.coreKnown = t.coreKnown;
        for (uint32_t k = 0; k < count; ++k)
        {
            const uint32_t *entry = t.vu.vu[5u + k];
            const uint32_t address = (entry[0] & 0x7FFu) * 8u, block = entry[1] & 0x3FFu;
            out.address[k] = static_cast<uint16_t>(address);
            out.routine[k] = identify(address);
            // The grass (image 47dd's 0x1380), by its block: q16 the blades' GIF tag - eight loops of
            // XYZF2, ST, RGBAQ, XYZF2 as sprites, textured and blended (as the engine's
            // vlShader_Grass::LoadContext sets it up). (Its image's hash takes in leftovers our image lacks.)
            if (address == 0x1380u)
            {
                const uint32_t *tag = t.vu.vu[(block + 16u) & 0x3FFu];
                if ((tag[0] & 0x7FFFu) == 8u && tag[2] == 0x4124u && ((tag[1] >> 15) & 0x7FFu) == 0x56u)
                    out.routine[k] = Routine::Grass;
            }
            // The drift (vlShader_Drift: the grass image's 0x0480, alone in the table) - the grass's
            // blades' GIF tag at q16 too, its wind at q24 and its 64 points from q25.
            if (address == 0x0480u && count == 1u)
            {
                const uint32_t *tag = t.vu.vu[(block + 16u) & 0x3FFu];
                if ((tag[0] & 0x7FFFu) == 8u && tag[2] == 0x4124u && ((tag[1] >> 15) & 0x7FFu) == 0x56u)
                    out.routine[k] = Routine::Drift;
            }
            // Instances: 13 qwords, then 5 per instance (q12.x of them, at most 64).
            uint32_t size = out.routine[k] == Routine::Grass ? 56u : out.routine[k] == Routine::Drift ? 57u : kBlockQwords; // (grass: its 64 points from q24; drift: from q25)
            if (out.routine[k] == Routine::Instances)
                size = 13u + 5u * std::clamp<int32_t>(static_cast<int16_t>(t.vu.vu[(block + 12u) & 0x3FFu][0] & 0xFFFFu), 1, 64);
            out.first[k] = static_cast<uint16_t>(out.data.size());
            const size_t at = out.data.size();
            out.data.resize(at + size);
            if (block + size <= 0x400u)
                std::memcpy(out.data.data() + at, t.vu.vu[block], 16u * size); // (in one piece)
            else
                for (uint32_t q = 0; q < size; ++q)
                    std::memcpy(out.data[at + q].v, t.vu.vu[(block + q) & 0x3FFu], 16u);
        }
        // The core's field A matrix, its clipping and the strips' tags live outside the blocks:
        // q21 (clipping on), 1020-1023 (the tags).
        std::memcpy(out.q21, t.vu.vu[21], 16u);
        std::memcpy(out.tail, t.vu.vu[1023], 16u);
    }
}
