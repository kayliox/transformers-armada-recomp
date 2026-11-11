// A draw's shader (nr_shader.cpp): its VU1 routine table identified routine by routine, and a copy
// of each routine's data block - what the HLSL needs to compute what the routines do.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace nr
{
    enum class Routine : uint8_t
    {
        Unknown,
        TransformKick,         // 0x1410: the transform, both fields
        Clip,                  // 0x0480
        ConstantColour,        // 0x13b0: colour = q0
        ScaleColours,          // 0x2150: colour *= q0
        MapCoordinates,        // 0x21b0: ST from the position through q0-q3 (environment), colour q4
        LightNormals,          // 0x1ac0/0x1c20 (images 422f, c06a / 6b20): one light + ambient
        FadeByPlane,           // 0x3660: alpha by distance from a plane
        LightWithHighlight,    // 0x21f0: two lights and a highlight in the mesh's space
        TransformTexture,      // 0x1ac0 (6b20, e147, 2011): ST through a 2x3 matrix (scrolling)
        ScaleAndOffsetColours, // 0x1c20 (e147, 2011): colour * q0 + q1
        LitMeshPrepare,        // 0x1d10: the eye and lights for 0x2800
        LitMeshDraw,           // 0x2800: reflection (context 1) + lit base texture (context 2)
        WaterPrepare,          // 0x1df0 (e147): waves
        WaterDraw,             // 0x2cc0
        Instances,             // 0x1ac0 (bec5): one mesh at several places
        Sprites,               // 0x22a0 / 0x2740
        Projection,            // 0x1df0 (2011): a projected texture
        ProjectedDraw,         // 0x22a0 (2011)
        FlatStrip,             // 0x3cc0
        FieldSprites,          // 0x2cf0
        Grass,                 // 0x1380 of the grass image (47dd78e8f5335846, its own core): blades on the ground
        MembraneWave,          // 0x2030 (image 6b20c90260be78a4): the position moved by two waves, the normal with it
        MembraneShade,         // 0x25e0 (6b20): the sphere map of the reflected view, a constant colour, alpha from N.L
        ProjectedLight,        // 0x1e70 (image 83f1438f69bcf7be): a texture projected from the vertex moved along its normal, one light
        RimReflection,         // 0x2920 (image e1472a8c96f62e62): the reflected view in screen axes, alpha by (1 - N.V)^2
        Drift,                 // 0x0480 of the grass image, alone in its table: the ground mist's drifting streaks (Antarctica)
        Smear,                 // 0x2220 (image 83f1438f69bcf7be): the vertices facing q0 moved by it, colour q1 / q2 (the dash's after-image)
        CityBlocks,            // 0x22c0 (image a31dd0bedf37226c): a box per vertex, one face of it a batch (Cybertron's city, TODO 162)
        ElectricShock,         // 0x1ac0 (image 83f1438f69bcf7be): taShader_ElectricShock - the stunned creature's crackling shell (TODO 290)
    };

    constexpr uint32_t kMaxRoutines = 6;
    constexpr uint32_t kBlockQwords = 24; // a routine's block as copied (instances: as many as they need)

    struct Qword
    {
        float v[4];
    };

    struct ShaderState
    {
        uint32_t count = 0;
        bool coreKnown = false;
        uint16_t address[kMaxRoutines] = {};
        Routine routine[kMaxRoutines] = {};
        uint16_t first[kMaxRoutines] = {}; // each routine's block: data[first] on
        std::vector<Qword> data;
        float q21[4] = {};
        uint32_t tail[4] = {}; // q1023: the GIF tag's y, z, w a routine's packet takes (PRE, PRIM, the registers)
        // The block of the first routine of this kind (nullptr: none).
        const Qword *blockOf(Routine r) const
        {
            for (uint32_t k = 0; k < count; ++k)
                if (routine[k] == r)
                    return data.data() + first[k];
            return nullptr;
        }
    };

    // The display list's words in [from, to) applied to the VU1 image (the tracker's own).
    // `direct`: the GIF packets (VIF DIRECT) on the way, in order.
    void trackDisplayList(const uint8_t *rdram, uint32_t from, uint32_t to,
                          const std::function<void(const uint32_t *, size_t)> &direct = {});
    // The routine table as the image now holds it.
    void snapshotShader(ShaderState &out);

    // The display list up to `to` appended to `out` (the game's thread; false: not this region's
    // chain, nothing appended), and those words applied to the VU image (the renderer's thread).
    bool gatherList(const uint8_t *rdram, uint32_t from, uint32_t to, std::vector<uint32_t> &out,
                    std::vector<std::pair<uint32_t, uint32_t>> *positions = nullptr);
    void replayList(const uint32_t *words, size_t count);
    // replayList, with the list's GIF packets (VIF DIRECT) handed to `direct` in order and the particle pipelines'
    // programs run where they start (nr_particles.cpp, TODO 144): their XGKICK packets to `kicked` at their place.
    // (N5.3) While `kicked` runs: the command of the words replayed its packets come from (MSCAL / MSCNT).
    extern thread_local const uint32_t *t_kickWord;
    void replayListRunning(const uint32_t *words, size_t count, const std::function<void(const uint32_t *, size_t)> &direct,
                           const std::function<void(const uint32_t *, size_t)> &kicked);
    // Field A's screen transform as VU1 holds it now (MSCAL 0x450's load of q0-q3); false before any.
    bool fieldAMatrix(float out[16]);
}
