// The game's VU1 routines in C++ (docs/RENDERER.md): for the batches whose routines are
// known, the run is done here instead of by the microcode, bit for bit as the microcode does
// it - VU1's arithmetic (single precision rounded toward zero, operands clamped, a product and
// a sum rounded separately; the engine calls in VU1's float mode), the same writes to VU1
// data memory and the same packets. The registers the microcode would leave different are
// ones no later run reads before writing (tools/vu1_liveness.py); the check mode
// (PS2X_VU1_NATIVE=check) compares packets, data memory and end pc with the microcode's.

#include "runtime/ps2_vu1.h"
#include "runtime/ps2_vu1_native.h"
#include "runtime/ps2_host_display.h"
#include "runtime/gs/gs_native_mesh.h"
#include "port.h"

#include <string>

namespace nr
{
    bool outsideFrames(); // (native_render/nr_capture.cpp: a screen the game draws outside its frames - its VU1 work runs)
    uint64_t vblankTick(); // (native_render/nr_capture.cpp: the GS's VBlank count - for TF_VU1_CENSUS's lines)
    std::string censusLine(); // (native_render/nr_capture.cpp: what reached or read GS memory since the last line)
    namespace particles
    {
        bool isParticleProgram(const uint8_t *micro, uint32_t start); // (native_render/nr_particles.cpp: a vlxPPP pipeline's image there)
    }
    namespace warp
    {
        bool vu1Run(ps2vu1native::Run &run); // (native_render/nr_warp.cpp: the warp tunnel's program, drawn natively - TODO 27)
        bool strayRun(ps2vu1native::Run &run); // (the same: a game batch on the warp program, left out - TODO 185 b)
    }
}

#include <immintrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#define TF_INLINE __forceinline // (the game's sources build with /Ob1: only what is marked inlines)
#else
#define TF_INLINE inline __attribute__((always_inline))
#endif

namespace
{
    // The code seven of the game's eight microcode images start with: the dispatcher and the
    // shared routines (0x0000-0x1ABF; the overlays begin at 0x1AC0). Its FNV-1a hash.
    constexpr uint32_t kCoreBytes = 0x1AC0u;
    constexpr uint64_t kCoreHash = 0x337999265B47F13Eull;

    uint64_t hashBytes(const uint8_t *bytes, uint32_t size)
    {
        uint64_t hash = 0xCBF29CE484222325ull; // FNV-1a
        for (uint32_t i = 0; i < size; ++i)
            hash = (hash ^ bytes[i]) * 0x100000001B3ull;
        return hash;
    }

    TF_INLINE uint32_t word(const uint8_t *data, uint32_t qword, uint32_t lane)
    {
        uint32_t value = 0;
        std::memcpy(&value, data + ((qword & 0x3FFu) * 16u + lane * 4u), 4u);
        return value;
    }

    TF_INLINE __m128 loadQ(const uint8_t *data, uint32_t qword)
    {
        return _mm_loadu_ps(reinterpret_cast<const float *>(data + (qword & 0x3FFu) * 16u));
    }

    TF_INLINE void storeQ(uint8_t *data, uint32_t qword, __m128 value)
    {
        _mm_storeu_ps(reinterpret_cast<float *>(data + (qword & 0x3FFu) * 16u), value);
    }

    // Lanes x, y, z of `value` (w left as it is), as `SQ.xyz`.
    TF_INLINE void storeXyz(uint8_t *data, uint32_t qword, __m128 value)
    {
        float *target = reinterpret_cast<float *>(data + (qword & 0x3FFu) * 16u);
        _mm_storeu_ps(target, _mm_blend_ps(value, _mm_loadu_ps(target), 0x8));
    }

    // ISW/ISWR: a VI register's 16 bits into a word, zero-extended.
    TF_INLINE void storeHalf(uint8_t *data, uint32_t qword, uint32_t laneIndex, uint32_t value)
    {
        const uint32_t bits = value & 0xFFFFu;
        std::memcpy(data + (qword & 0x3FFu) * 16u + laneIndex * 4u, &bits, 4u);
    }

    // An FMAC operand: exponent 255 reads as +-FLT_MAX (denormals read as zero by the float mode).
    TF_INLINE __m128 clamp(__m128 value)
    {
        __m128i bits = _mm_castps_si128(value);
        bits = _mm_min_epu32(bits, _mm_set1_epi32(static_cast<int>(0xFF7FFFFFu)));
        bits = _mm_min_epi32(bits, _mm_set1_epi32(0x7F7FFFFF));
        return _mm_castsi128_ps(bits);
    }

    TF_INLINE __m128 lane(__m128 value, int index)
    {
        switch (index)
        {
        case 0:
            return _mm_shuffle_ps(value, value, 0x00);
        case 1:
            return _mm_shuffle_ps(value, value, 0x55);
        case 2:
            return _mm_shuffle_ps(value, value, 0xAA);
        default:
            return _mm_shuffle_ps(value, value, 0xFF);
        }
    }

    struct Matrix
    {
        __m128 row[4];
    };

    // MULAw.xyzw ACC, row3, VF00w; MADDAz ACC, row2, z; MADDAy ACC, row1, y; MADDx out, row0, x
    // (the position's lanes given broadcast).
    TF_INLINE __m128 transform(const Matrix &m, __m128 x, __m128 y, __m128 z)
    {
        __m128 acc = _mm_mul_ps(m.row[3], _mm_set1_ps(1.0f));
        acc = _mm_add_ps(acc, _mm_mul_ps(m.row[2], z));
        acc = _mm_add_ps(acc, _mm_mul_ps(m.row[1], y));
        return _mm_add_ps(acc, _mm_mul_ps(m.row[0], x));
    }

    // DIV Q, VF00w, w (w in every lane): 1/w, or +-FLT_MAX by w's sign for a zero w (a
    // denormal compares as zero, as in the float mode).
    TF_INLINE __m128 reciprocal(__m128 w)
    {
        const __m128 den = clamp(w);
        const __m128 zero = _mm_cmpeq_ps(den, _mm_setzero_ps());
        const __m128 limit = _mm_or_ps(_mm_and_ps(den, _mm_castsi128_ps(_mm_set1_epi32(static_cast<int>(0x80000000u)))),
                                       _mm_castsi128_ps(_mm_set1_epi32(0x7F7FFFFF)));
        return _mm_blendv_ps(_mm_div_ps(_mm_set1_ps(1.0f), den), limit, zero);
    }

    // FTOI0/FTOI4 (`scale` 1 or 16): toward zero, 2^31 and above as 0x7FFFFFFF (the rest
    // saturates in the conversion itself).
    TF_INLINE __m128i ftoi(__m128 value, float scale)
    {
        const __m128 scaled = _mm_mul_ps(clamp(value), _mm_set1_ps(scale));
        const __m128i converted = _mm_cvttps_epi32(scaled);
        const __m128 high = _mm_cmpge_ps(scaled, _mm_set1_ps(2147483648.0f));
        return _mm_castps_si128(_mm_blendv_ps(_mm_castsi128_ps(converted), _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF)), high));
    }

    TF_INLINE __m128i ftoi4(__m128 value) { return ftoi(value, 16.0f); }

    // ITOF0 (rounded toward zero by the float mode, as VU1 does).
    TF_INLINE __m128 itof0(__m128 value) { return _mm_cvtepi32_ps(_mm_castps_si128(value)); }

    // MINI: the smaller as sign-and-magnitude floats, on the bits - as integers the smaller,
    // unless both are negative (then the larger). No clamp.
    TF_INLINE __m128 mini(__m128 a, __m128 b)
    {
        const __m128i ia = _mm_castps_si128(a), ib = _mm_castps_si128(b);
        const __m128i bothNegative = _mm_srai_epi32(_mm_and_si128(ia, ib), 31);
        return _mm_castsi128_ps(_mm_blendv_epi8(_mm_min_epi32(ia, ib), _mm_max_epi32(ia, ib), bothNegative));
    }

    // MAX: the larger as sign-and-magnitude floats, on the bits (the smaller as integers when
    // both are negative). No clamp.
    TF_INLINE __m128 maxBits(__m128 a, __m128 b)
    {
        const __m128i ia = _mm_castps_si128(a), ib = _mm_castps_si128(b);
        const __m128i bothNegative = _mm_srai_epi32(_mm_and_si128(ia, ib), 31);
        return _mm_castsi128_ps(_mm_blendv_epi8(_mm_max_epi32(ia, ib), _mm_min_epi32(ia, ib), bothNegative));
    }

    TF_INLINE __m128 signOf(__m128 value) { return _mm_and_ps(value, _mm_castsi128_ps(_mm_set1_epi32(static_cast<int>(0x80000000u)))); }

    TF_INLINE __m128 floatMax() { return _mm_castsi128_ps(_mm_set1_epi32(0x7F7FFFFF)); }

    // DIV: num / den, or +-FLT_MAX by the two signs for a zero den (a denormal is zero).
    TF_INLINE __m128 divide(__m128 num, __m128 den)
    {
        num = clamp(num);
        den = clamp(den);
        const __m128 zero = _mm_cmpeq_ps(den, _mm_setzero_ps());
        const __m128 limit = _mm_or_ps(_mm_xor_ps(signOf(num), signOf(den)), floatMax());
        return _mm_blendv_ps(_mm_div_ps(num, den), limit, zero);
    }

    // RSQRT: num / sqrt(|rad|), or +-FLT_MAX by num's sign for a zero root.
    TF_INLINE __m128 rsqrtDivide(__m128 num, __m128 rad)
    {
        num = clamp(num);
        const __m128 root = _mm_sqrt_ps(_mm_andnot_ps(_mm_castsi128_ps(_mm_set1_epi32(static_cast<int>(0x80000000u))), clamp(rad)));
        const __m128 zero = _mm_cmpeq_ps(root, _mm_setzero_ps());
        return _mm_blendv_ps(_mm_div_ps(num, root), _mm_or_ps(signOf(num), floatMax()), zero);
    }

    // ERLENG of a sum of squares: 1 / sqrt(sum), or the root itself when it is zero.
    TF_INLINE __m128 reciprocalLength(__m128 sum)
    {
        const __m128 root = _mm_sqrt_ps(sum);
        const __m128 zero = _mm_cmpeq_ps(root, _mm_setzero_ps());
        return _mm_blendv_ps(_mm_div_ps(_mm_set1_ps(1.0f), root), root, zero);
    }

    // The drawing-kick word of vertex v (MFIR.w of the mask after v shifts, as 0x1410 and
    // 0x2800 keep it: 16 vertices to a mask word).
    TF_INLINE int32_t kickBits(uint32_t masks0, uint32_t masks1, uint32_t v)
    {
        const uint32_t mask = v < 16u ? masks0 : masks1;
        const uint32_t shift = v < 16u ? v : v - 16u;
        return static_cast<int16_t>(shift < 16u ? (mask << shift) & 0xFFFFu : 0u);
    }

    // The batch being run: VU1's state and memory, the header at ITOP and its vertex count
    // (vertices of three qwords from ITOP + 1, the input of every routine: `vi01`).
    struct Batch
    {
        VU1State &state;
        uint8_t *data;
        uint32_t header;
        uint32_t headerX;
        uint32_t count;
        uint32_t vertex(uint32_t index) const { return header + 1u + 3u * index; }
    };

    // Batch types 3 and 4 (dispatcher 0x2c0, 0x388): the first one or two qwords of every
    // vertex (ST; type 4 also the normal) less the header's z (integers in a float's mantissa).
    // Four (type 3) or two (type 4) vertices at a time, as the microcode goes, so past the last
    // one; the qword after the vertices (the drawing-kick masks) it saves first and puts back.
    void convertVertices(Batch &b, uint32_t qwords)
    {
        const __m128 bias = clamp(lane(loadQ(b.data, b.header), 2));
        const uint32_t step = qwords == 1u ? 4u : 2u;
        const uint32_t vertices = (b.count + step - 1u) / step * step;
        const uint32_t saved = b.header + 3u * b.count + 4u;
        const __m128 masks = loadQ(b.data, saved);
        for (uint32_t v = 0; v < vertices; ++v)
            for (uint32_t k = 0; k < qwords; ++k)
                storeQ(b.data, b.vertex(v) + k, _mm_sub_ps(clamp(loadQ(b.data, b.vertex(v) + k)), bias));
        storeQ(b.data, saved, masks);
    }

    void convertType3(Batch &b) { convertVertices(b, 1u); }

    // Routine 0x2150 of image bec5738323d4afd8: every vertex's colour (its second qword, as
    // integers) times the data block's q0, at most 255, back to integers. Four vertices at a
    // time, past the last one as the microcode goes: 0x1410 later writes its field B over
    // those qwords but for their w, which its packet carries.
    void scaleColours(Batch &b, uint32_t block)
    {
        const __m128 scale = clamp(loadQ(b.data, block));
        const __m128 limit = _mm_set1_ps(255.0f); // LOI 255
        const uint32_t vertices = (b.count + 3u) & ~3u;
        for (uint32_t v = 0; v < vertices; ++v)
        {
            const uint32_t at = b.vertex(v) + 1u;
            const __m128 colour = mini(_mm_mul_ps(itof0(loadQ(b.data, at)), scale), limit);
            storeQ(b.data, at, _mm_castsi128_ps(ftoi(colour, 1.0f)));
        }
    }

    // Routine 0x21b0 of images 25cef3e84459f32b and 2011a15b85aa5a94 (by their bytes): every
    // vertex's ST x and y from its position by the rows x and y of the data block's q0-q3
    // (environment-mapped coordinates), its colour the data block's q4 as integers with bit 7
    // of the vertex's alpha. (With one vertex the microcode also writes the slots of a second,
    // which 0x1410 then writes over.)
    void mapCoordinates(Batch &b, uint32_t block)
    {
        Matrix m{};
        for (int r = 0; r < 4; ++r)
            m.row[r] = clamp(loadQ(b.data, block + static_cast<uint32_t>(r)));
        const __m128i rgb = ftoi(loadQ(b.data, block + 4u), 1.0f);
        for (uint32_t v = 0; v < b.count; ++v)
        {
            const uint32_t at = b.vertex(v);
            const __m128 position = clamp(loadQ(b.data, at + 2u));
            const __m128 st = transform(m, lane(position, 0), lane(position, 1), lane(position, 2));
            float *target = reinterpret_cast<float *>(b.data + (at & 0x3FFu) * 16u);
            _mm_storeu_ps(target, _mm_blend_ps(st, _mm_loadu_ps(target), 0xC)); // SQ.xy
            const int32_t alphaBit = static_cast<int16_t>(word(b.data, at + 1u, 3)) & 128;
            storeQ(b.data, at + 1u, _mm_castsi128_ps(_mm_insert_epi32(rgb, alphaBit, 3)));
        }
    }

    // Routine 0x1ac0 of images 422f0062aa2ca315 and c06ad47f0d18db35 (by its bytes), before
    // 0x1410 in batches of type 4 (ST, normal, position): every vertex lit by one directional
    // light with an ambient term. Its colour, in place of the normal: q2 * q1 * (max(N.L, 0) +
    // L.w), at most 255, as integers; its alpha q2.w * 128 (L the data block's q0).
    void lightNormals(Batch &b, uint32_t block)
    {
        const __m128 light = clamp(loadQ(b.data, block));
        const __m128 colour = _mm_mul_ps(clamp(loadQ(b.data, block + 2u)), clamp(loadQ(b.data, block + 1u)));
        const int32_t alpha = _mm_extract_epi32(ftoi(_mm_mul_ps(clamp(loadQ(b.data, block + 2u)), _mm_set1_ps(128.0f)), 1.0f), 3);
        const __m128 one = _mm_set1_ps(1.0f), limit = _mm_set1_ps(255.0f); // MAXw.x 0, 1; LOI 255
        for (uint32_t v = 0; v < b.count; ++v)
        {
            const uint32_t at = b.vertex(v) + 1u;
            const __m128 product = _mm_mul_ps(clamp(loadQ(b.data, at)), light); // N * L by lane
            const __m128 dot = _mm_add_ps(_mm_add_ps(lane(product, 0), lane(product, 1)), _mm_mul_ps(one, lane(product, 2)));
            const __m128 intensity = _mm_add_ps(maxBits(dot, _mm_setzero_ps()), lane(light, 3));
            const __m128i lit = ftoi(mini(_mm_mul_ps(colour, intensity), limit), 1.0f);
            storeQ(b.data, at, _mm_castsi128_ps(_mm_insert_epi32(lit, alpha, 3)));
        }
    }

    // Routine 0x13b0 (the shared core), before 0x1410: every vertex's colour the data block's
    // q0 as integers - four vertices at a time, and past the last one as the microcode goes
    // (a group more than (count - 1) / 4 rounded up). It leaves the colour in VF26, which 0x0480's
    // register save (0x1180) puts on the stack when 0x1410 clips: in the state for clipAndKick.
    void constantColour(Batch &b, uint32_t block)
    {
        const __m128 colour = _mm_castsi128_ps(ftoi(loadQ(b.data, block), 1.0f));
        const uint32_t vertices = 4u * (1u + (b.count + 2u) / 4u);
        for (uint32_t v = 0; v < vertices; ++v)
            storeQ(b.data, b.vertex(v) + 1u, colour);
        _mm_storeu_ps(b.state.vf[26], colour);
    }

    // Routine 0x1ac0 of images 6b20c90260be78a4, e1472a8c96f62e62 and 2011a15b85aa5a94 (by its
    // bytes), before 0x1410 in batches of type 3 or 4: the texture coordinates through a 2x3
    // matrix, the data block's q0-q2 - every vertex's ST x and y become q0 * s + q1 * t + q2 by
    // lane (MULAx, MADDAy, MADDw with VF00w; in the captures q0, q1 the identity and q2 an offset
    // that moves from frame to frame: scrolling textures); its ST z and w are kept. Four vertices
    // at a time, and 1 + ceil(count / 4) groups of them, so 1 to 7 vertex slots past the last
    // vertex get their x, y written as well (0x1410 writes over most of them later, not all),
    // but for the qword after the vertices (the drawing-kick masks, vertex count+1's ST), which
    // the routine saves first and puts back. It leaves in VF16-VF19 the ST x, y (z 0, w 1) of the
    // four vertex slots after its last group, loaded and never used: 0x0480's register save
    // (0x1180) puts VF17 and VF18 on the stack as they are when 0x1410 clips a triangle, so they
    // are left in the state for clipAndKick's `kept`.
    void transformTexture(Batch &b, uint32_t block)
    {
        uint8_t *data = b.data;
        const __m128 row0 = clamp(loadQ(data, block)), row1 = clamp(loadQ(data, block + 1u));
        const __m128 offset = _mm_mul_ps(clamp(loadQ(data, block + 2u)), _mm_set1_ps(1.0f)); // q2 * VF16w
        const uint32_t masks = b.header + 3u * b.count + 4u;
        const __m128 saved = loadQ(data, masks);
        const uint32_t vertices = (b.count + 7u) & ~3u;
        for (uint32_t v = 0; v < vertices; ++v)
        {
            const uint32_t at = b.vertex(v);
            const __m128 st = clamp(loadQ(data, at));
            const __m128 acc = _mm_add_ps(_mm_mul_ps(row0, lane(st, 0)), _mm_mul_ps(row1, lane(st, 1)));
            float *target = reinterpret_cast<float *>(data + (at & 0x3FFu) * 16u);
            _mm_storeu_ps(target, _mm_blend_ps(_mm_add_ps(acc, offset), _mm_loadu_ps(target), 0xC)); // SQ.xy
        }
        storeQ(data, masks, saved);
        const __m128 zeroOne = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f); // VF00, then LQ.xy
        for (uint32_t j = 0; j < 4u; ++j)
            _mm_storeu_ps(b.state.vf[16u + j], _mm_blend_ps(loadQ(data, b.vertex(vertices + j)), zeroOne, 0xC));
    }

    // Routine 0x1c20 of images e1472a8c96f62e62 and 2011a15b85aa5a94 (by its bytes; image
    // 6b20c90260be78a4's 0x1c20 is lightNormals), before 0x1410 in batches of type 3: every
    // vertex's colour (its second qword, as integers; all four lanes, alpha too) times the data
    // block's q0 plus q1, from 0 to 255, back to integers (in the captures q0 1 and q1 0). The
    // microcode is pipelined three vertices deep and writes at least three: a count of 1 or 2
    // writes three vertices, 3 or 4 writes four (its first count test, IBLEZ at 0x1c80, reads
    // vi02 as it was before the IADDI in the pair just before it). 0x1410 writes over those slots
    // later but for one: with a count of 1, the third vertex's colour qword is field B's first
    // ST, whose w 0x1410 keeps.
    void scaleAndOffsetColours(Batch &b, uint32_t block)
    {
        const __m128 scale = clamp(loadQ(b.data, block)), offset = clamp(loadQ(b.data, block + 1u));
        const __m128 zero = _mm_setzero_ps(), limit = _mm_set1_ps(255.0f); // MAXx VF00x; MINIi, LOI 255
        const uint32_t n = b.count;
        const uint32_t vertices = n <= 2u ? 3u : n <= 4u ? 4u : n;
        for (uint32_t v = 0; v < vertices; ++v)
        {
            const uint32_t at = b.vertex(v) + 1u;
            const __m128 value = _mm_add_ps(_mm_mul_ps(itof0(loadQ(b.data, at)), scale), offset);
            storeQ(b.data, at, _mm_castsi128_ps(ftoi(mini(maxBits(value, zero), limit), 1.0f)));
        }
    }


    // Routine 0x3660 of images 2011a15b85aa5a94 and 25cef3e84459f32b (by its bytes), before
    // 0x1410: every vertex's alpha its distance from a plane (the data block's q0: the normal,
    // w the offset), from 0 to 128; and a vertex's drawing-kick bit set when the alphas of the
    // three that end with it add up to nothing - a running sum as the microcode keeps it (the
    // oldest taken off, the newest added, each rounded), tested by the MAC zero flag of the
    // last addition (its exact result 0 or below the smallest normal).
    void fadeByPlane(Batch &b, uint32_t block)
    {
        uint8_t *data = b.data;
        const __m128 plane = clamp(loadQ(data, block));
        const __m128 limit = _mm_set1_ps(128.0f); // LOI 128
        __m128 oldest = _mm_setzero_ps(), middle = _mm_setzero_ps(), newest = _mm_setzero_ps(), sum = _mm_setzero_ps();
        uint32_t bits[2] = {};
        for (uint32_t v = 0; v < b.count; ++v)
        {
            const uint32_t at = b.vertex(v);
            const __m128 product = _mm_mul_ps(clamp(loadQ(data, at + 2u)), plane);
            const __m128 less = _mm_sub_ps(sum, oldest); // SUBx.w: the sum less the oldest
            __m128 distance = _mm_add_ps(lane(plane, 3), lane(product, 0));
            distance = _mm_add_ps(distance, _mm_mul_ps(_mm_set1_ps(1.0f), lane(product, 1)));
            distance = _mm_add_ps(distance, _mm_mul_ps(_mm_set1_ps(1.0f), lane(product, 2)));
            const __m128 alpha = mini(maxBits(distance, _mm_setzero_ps()), limit);
            const int32_t alphaBits = _mm_cvtsi128_si32(ftoi(alpha, 1.0f));
            std::memcpy(data + ((at + 1u) & 0x3FFu) * 16u + 12u, &alphaBits, 4u); // SQ.w
            const double exact = static_cast<double>(_mm_cvtss_f32(less)) + static_cast<double>(_mm_cvtss_f32(alpha));
            const bool zero = exact == 0.0 || std::fabs(exact) < 1.1754943508222875e-38; // (or underflow)
            sum = _mm_add_ps(less, alpha);
            oldest = middle;
            middle = newest;
            newest = _mm_add_ps(_mm_setzero_ps(), alpha);
            bits[v < 16u ? 0 : 1] |= (zero ? 1u : 0u) << (15u - (v & 15u));
        }
        const uint32_t masks = b.header + 3u * b.count + 4u;
        storeHalf(data, masks, 0, (word(data, masks, 0) | bits[0]) & 0xFFFFu);
        if (b.count > 16u)
            storeHalf(data, masks, 1, (word(data, masks, 1) | bits[1]) & 0xFFFFu);
    }

    // Routine 0x21f0 of image 422f0062aa2ca315 (by its bytes), before 0x1410 in batches of type 4
    // (ST, normal, position): every vertex lit by two directional lights, with a highlight, in
    // the mesh's own space - the lights and the eye taken there through the inverse of field A's
    // matrix (vf01-vf03 by cofactors over the determinant, vf04 its offset). Its colour, in place
    // of the normal: q3 * q2 * ((q6 * max(N.L1, 0) + q2 * max(N.L2, 0)) * q1.w + q5.w) * 128, from
    // 0 to 255; its alpha q4.w * max(R.L2, 0)^8, R = 2 (N.V) N - V the direction to the eye
    // reflected. L1 = q5, L2 = q1, the eye q0; the lights normalized by RSQRT, V by ERLENG. The
    // microcode's loop does all but the last three vertices, then those three: at least three.
    void lightWithHighlight(Batch &b, uint32_t block)
    {
        uint8_t *data = b.data;
        const uint32_t n = b.count;
        const __m128 one = _mm_set1_ps(1.0f), zero = _mm_setzero_ps();
        const __m128 a = clamp(_mm_loadu_ps(b.state.vf[1])), bb = clamp(_mm_loadu_ps(b.state.vf[2])),
                     c = clamp(_mm_loadu_ps(b.state.vf[3])), offset = clamp(_mm_loadu_ps(b.state.vf[4]));
        const __m128 ax = lane(a, 0), ay = lane(a, 1), az = lane(a, 2);
        const __m128 bx = lane(bb, 0), by = lane(bb, 1), bz = lane(bb, 2);
        const __m128 cx = lane(c, 0), cy = lane(c, 1), cz = lane(c, 2);
        // The inverse of the 3x3 part, as the routine has it (a row's entries for z, y, x).
        const __m128 c0 = _mm_sub_ps(_mm_mul_ps(by, cz), _mm_mul_ps(cy, bz));
        const __m128 c1 = _mm_sub_ps(_mm_mul_ps(cy, az), _mm_mul_ps(ay, cz));
        const __m128 c2 = _mm_sub_ps(_mm_mul_ps(bz, ay), _mm_mul_ps(az, by));
        const __m128 r = divide(one, _mm_add_ps(_mm_add_ps(_mm_mul_ps(ax, c0), _mm_mul_ps(bx, c1)), _mm_mul_ps(cx, c2)));
        const __m128 i0z = _mm_mul_ps(_mm_sub_ps(_mm_mul_ps(bx, cy), _mm_mul_ps(cx, by)), r);
        const __m128 i0y = _mm_mul_ps(_mm_sub_ps(_mm_mul_ps(cx, bz), _mm_mul_ps(bx, cz)), r);
        const __m128 i0x = _mm_mul_ps(c0, r);
        const __m128 i1z = _mm_mul_ps(_mm_sub_ps(_mm_mul_ps(ay, cx), _mm_mul_ps(cy, ax)), r);
        const __m128 i1y = _mm_mul_ps(_mm_sub_ps(_mm_mul_ps(cz, ax), _mm_mul_ps(az, cx)), r);
        const __m128 i1x = _mm_mul_ps(c1, r);
        const __m128 i2z = _mm_mul_ps(_mm_add_ps(zero, _mm_sub_ps(_mm_mul_ps(ax, by), _mm_mul_ps(bx, ay))), r); // (-0 made +0)
        const __m128 i2y = _mm_mul_ps(_mm_sub_ps(_mm_mul_ps(az, bx), _mm_mul_ps(bz, ax)), r);
        const __m128 i2x = _mm_mul_ps(c2, r);
        const auto row = [](__m128 iz, __m128 iy, __m128 ix, __m128 v)
        { return _mm_add_ps(_mm_add_ps(_mm_mul_ps(iz, lane(v, 2)), _mm_mul_ps(iy, lane(v, 1))), _mm_mul_ps(ix, lane(v, 0))); };
        // The two lights in the mesh's space, normalized (RSQRT 1, |L|^2).
        const __m128 light1 = clamp(loadQ(data, block + 5u)), light2 = clamp(loadQ(data, block + 1u));
        const __m128 u1 = row(i0z, i0y, i0x, light1), v1 = row(i1z, i1y, i1x, light1), w1 = row(i2z, i2y, i2x, light1);
        const __m128 s1 = rsqrtDivide(one, _mm_add_ps(_mm_add_ps(_mm_mul_ps(u1, u1), _mm_mul_ps(v1, v1)), _mm_mul_ps(one, _mm_mul_ps(w1, w1))));
        const __m128 l1x = _mm_mul_ps(u1, s1), l1y = _mm_mul_ps(v1, s1), l1z = _mm_mul_ps(w1, s1);
        const __m128 u2 = row(i0z, i0y, i0x, light2), v2 = row(i1z, i1y, i1x, light2), w2 = row(i2z, i2y, i2x, light2);
        const __m128 s2 = rsqrtDivide(one, _mm_add_ps(_mm_add_ps(_mm_mul_ps(u2, u2), _mm_mul_ps(v2, v2)), _mm_mul_ps(one, _mm_mul_ps(w2, w2))));
        const __m128 l2x = _mm_mul_ps(u2, s2), l2y = _mm_mul_ps(v2, s2), l2z = _mm_mul_ps(w2, s2);
        // The eye in the mesh's space: the inverse times (eye - offset), the offset's part first.
        const __m128 eye = clamp(loadQ(data, block));
        const __m128 tx = _mm_sub_ps(zero, lane(offset, 0)), ty = _mm_sub_ps(zero, lane(offset, 1)), tz = _mm_sub_ps(zero, lane(offset, 2));
        const auto eyeRow = [&](__m128 iz, __m128 iy, __m128 ix)
        {
            const __m128 moved = _mm_add_ps(_mm_add_ps(_mm_mul_ps(ix, tx), _mm_mul_ps(iy, ty)), _mm_mul_ps(iz, tz));
            return _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(moved, one), _mm_mul_ps(iz, lane(eye, 2))), _mm_mul_ps(iy, lane(eye, 1))),
                              _mm_mul_ps(ix, lane(eye, 0)));
        };
        const __m128 ex = eyeRow(i0z, i0y, i0x), ey = eyeRow(i1z, i1y, i1x), ez = eyeRow(i2z, i2y, i2x);
        // Per batch: the colours.
        const __m128 colour1 = clamp(loadQ(data, block + 6u)), colour2 = clamp(loadQ(data, block + 2u));
        const __m128 material = _mm_mul_ps(clamp(loadQ(data, block + 3u)), colour2);
        const __m128 scale = lane(light2, 3), ambient = lane(light1, 3), highlight = lane(clamp(loadQ(data, block + 4u)), 3);
        const __m128 limit = _mm_set1_ps(255.0f), x128 = _mm_set1_ps(128.0f);

        for (uint32_t g = 0; g < n; g += 4u)
        {
            __m128 nq[4], pq[4];
            for (uint32_t i = 0; i < 4u; ++i)
            {
                const uint32_t v = g + i < n ? g + i : n - 1u; // (lanes past the end: computed, not stored)
                nq[i] = clamp(loadQ(data, b.vertex(v) + 1u));
                pq[i] = clamp(loadQ(data, b.vertex(v) + 2u));
            }
            __m128 nx = nq[0], ny = nq[1], nz = nq[2], nw = nq[3];
            _MM_TRANSPOSE4_PS(nx, ny, nz, nw);
            __m128 px = pq[0], py = pq[1], pz = pq[2], pw = pq[3];
            _MM_TRANSPOSE4_PS(px, py, pz, pw);
            (void)nw;
            (void)pw;
            // The direction to the eye (ERLENG), N.V and the reflection's R.L2.
            const __m128 dx = _mm_sub_ps(ex, px), dy = _mm_sub_ps(ey, py), dz = _mm_sub_ps(ez, pz);
            const __m128 k = reciprocalLength(_mm_add_ps(_mm_add_ps(_mm_mul_ps(dx, dx), _mm_mul_ps(dy, dy)), _mm_mul_ps(dz, dz)));
            const __m128 vx = _mm_mul_ps(k, dx), vy = _mm_mul_ps(k, dy), vz = _mm_mul_ps(k, dz);
            const __m128 nv = _mm_add_ps(_mm_add_ps(_mm_mul_ps(nx, vx), _mm_mul_ps(ny, vy)), _mm_mul_ps(one, _mm_mul_ps(nz, vz)));
            const __m128 rx = _mm_sub_ps(_mm_mul_ps(_mm_add_ps(nx, nx), nv), _mm_mul_ps(one, vx));
            const __m128 ry = _mm_sub_ps(_mm_mul_ps(_mm_add_ps(ny, ny), nv), _mm_mul_ps(one, vy));
            const __m128 rz = _mm_sub_ps(_mm_mul_ps(_mm_add_ps(nz, nz), nv), _mm_mul_ps(one, vz));
            const __m128 rl = maxBits(_mm_add_ps(_mm_add_ps(_mm_mul_ps(rx, l2x), _mm_mul_ps(ry, l2y)), _mm_mul_ps(one, _mm_mul_ps(rz, l2z))), zero);
            const __m128 rl2 = _mm_mul_ps(rl, rl), rl4 = _mm_mul_ps(rl2, rl2);
            const __m128 alpha = _mm_castsi128_ps(ftoi(_mm_mul_ps(highlight, _mm_mul_ps(rl4, rl4)), 1.0f));
            // The two lights: max(N.L, 0).
            const __m128 nl1 = maxBits(_mm_add_ps(_mm_add_ps(_mm_mul_ps(nx, l1x), _mm_mul_ps(ny, l1y)), _mm_mul_ps(one, _mm_mul_ps(nz, l1z))), zero);
            const __m128 nl2 = maxBits(_mm_add_ps(_mm_add_ps(_mm_mul_ps(nx, l2x), _mm_mul_ps(ny, l2y)), _mm_mul_ps(one, _mm_mul_ps(nz, l2z))), zero);
            __m128 lit[3];
            for (int ch = 0; ch < 3; ++ch)
            {
                const __m128 sum = _mm_add_ps(_mm_mul_ps(_mm_add_ps(_mm_mul_ps(lane(colour1, ch), nl1), _mm_mul_ps(lane(colour2, ch), nl2)), scale), ambient);
                const __m128 value = _mm_mul_ps(_mm_mul_ps(lane(material, ch), sum), x128);
                lit[ch] = _mm_castsi128_ps(ftoi(maxBits(mini(value, limit), zero), 1.0f));
            }
            __m128 o0 = lit[0], o1 = lit[1], o2 = lit[2], o3 = alpha;
            _MM_TRANSPOSE4_PS(o0, o1, o2, o3);
            const __m128 out[4] = {o0, o1, o2, o3};
            for (uint32_t i = 0; i < 4u && g + i < n; ++i)
                storeQ(data, b.vertex(g + i) + 1u, out[i]);
        }
    }

    // Routine 0x1410 (the shared core), drawing (q21.x not above 0, so that it skips the
    // routine after it, 0x0480): every vertex through field A's matrix (vf01-vf04) and field B's
    // (data block q0-q3), Q = 1/w, XYZ * Q to 12.4 fixed point, ST * Q, the drawing-kick bit
    // from the mask after the vertices; field A over the input, field B after it, each with its
    // register writes (data block q8-q10, q12-q14) and vertex tag; one packet for both.
    void transformAndKick(Batch &b, uint32_t block, ps2vu1native::Run &run)
    {
        uint8_t *data = b.data;
        const uint32_t header = b.header, count = b.count;
        Matrix fieldA{}, fieldB{};
        for (int r = 0; r < 4; ++r)
        {
            fieldA.row[r] = clamp(_mm_loadu_ps(b.state.vf[1 + r]));
            fieldB.row[r] = clamp(loadQ(data, block + static_cast<uint32_t>(r)));
        }
        const __m128 tail = loadQ(data, 1023); // the vertex tags' y, z, w
        const uint32_t second = header + 3u * count + 4u; // field B's vertex tag
        const uint32_t masks0 = word(data, second, 0);
        const uint32_t masks1 = word(data, second, 1);

        for (uint32_t v = 0; v < count; ++v)
        {
            const uint32_t at = b.vertex(v);
            const __m128 st = clamp(loadQ(data, at));
            const __m128 position = clamp(loadQ(data, at + 2u));
            const uint32_t mask = v < 16u ? masks0 : masks1;
            const uint32_t shift = v < 16u ? v : v - 16u;
            const uint32_t shifted = shift < 16u ? (mask << shift) & 0xFFFFu : 0u;
            const int32_t adc = static_cast<int16_t>(shifted);
            const __m128 colour = loadQ(data, at + 1u);
            const __m128 x = lane(position, 0), y = lane(position, 1), z = lane(position, 2);
            for (int field = 0; field < 2; ++field)
            {
                const __m128 clip = transform(field == 0 ? fieldA : fieldB, x, y, z);
                const __m128 q = reciprocal(lane(clip, 3));
                const __m128 stq = _mm_mul_ps(st, q);
                __m128i xyz = ftoi4(_mm_mul_ps(clamp(clip), q));
                xyz = _mm_insert_epi32(xyz, adc, 3);
                const uint32_t out = field == 0 ? at : second + 1u + 3u * v;
                storeXyz(data, out, stq);
                if (field == 1)
                    storeQ(data, out + 1u, colour);
                storeQ(data, out + 2u, _mm_castsi128_ps(xyz));
            }
        }

        storeQ(data, header - 3u, loadQ(data, block + 8u));
        storeQ(data, header - 2u, loadQ(data, block + 9u));
        storeQ(data, header - 1u, loadQ(data, block + 10u));
        storeQ(data, header + 3u * count + 1u, loadQ(data, block + 12u));
        storeQ(data, header + 3u * count + 2u, loadQ(data, block + 13u));
        storeQ(data, header + 3u * count + 3u, loadQ(data, block + 14u));
        const __m128i tailBits = _mm_castps_si128(tail);
        storeQ(data, header, _mm_castsi128_ps(_mm_insert_epi32(tailBits, static_cast<int>(count), 0)));
        storeQ(data, second, _mm_castsi128_ps(_mm_insert_epi32(tailBits, static_cast<int>(b.headerX), 0)));

        run.send(run.context, (header - 3u) & 0x3FFu, 6u * count + 8u);
    }

    // 0x1410's drawing as a mesh (stage 2 of the renderer of our own, docs/RENDERER.md: the GPU
    // renderer fills in the vertices): the batch's vertices as they are now (converted, the
    // pre-passes' colours in), both fields' matrices, tags and register writes and the
    // drawing-kick masks, in one packet (runtime/gs/gs_native_mesh.h) in place of the routine's.
    // VU memory is left without the routine's output: nothing reads it but the packet.
    // TF_MOMENTS_CHECK=1 (with TF_MOMENTS=1): a mesh's field B matrix, which with the in-between
    // factor at 0 is the frame before's pose, against the field A matrix the same mesh (its
    // vertices) had the last time - every 2 s, how many were compared and how far apart.
    struct MomentCheck
    {
        bool on = false;
        uint64_t frame = ~0ull;                                       // the list the batches are of
        std::unordered_map<uint64_t, uint32_t> seen;                  // per mesh this frame: how often so far
        std::unordered_map<uint64_t, std::array<float, 16>> thisA, lastA; // field A by mesh and occurrence
        std::unordered_map<uint64_t, std::vector<std::array<float, 16>>> thisAll, lastAll; // every field A by mesh
        uint64_t nearBelow[5] = {}; // the nearest of the mesh's last field As: under 1e-6 .. 1e-2
        uint64_t compared = 0u, exact = 0u;
        double largest = 0.0, sum = 0.0;
        uint64_t below[5] = {}; // relative difference under 1e-6, 1e-5, 1e-4, 1e-3, 1e-2
        std::vector<float> values;
        std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
    } g_momentCheck = []()
    {
        MomentCheck c;
        const char *env = std::getenv("TF_MOMENTS_CHECK");
        c.on = env && *env && *env != '0';
        return c;
    }();

    void checkMoment(Batch &b, uint32_t block)
    {
        MomentCheck &c = g_momentCheck;
        const uint8_t *data = b.data;
        const uint32_t n = b.count, h = b.header;
        uint64_t key = 0xCBF29CE484222325ull ^ n;
        for (uint32_t q = 0; q < std::min<uint32_t>(3u * n, 12u); ++q)
            key = (key ^ hashBytes(data + ((h + 1u + q) & 0x3FFu) * 16u, 16u)) * 0x100000001B3ull;
        // (the n-th time a frame draws this mesh against the n-th time the frame before did)
        const uint64_t frame = tf::port::listsKicked();
        if (frame != c.frame)
        {
            c.lastA = frame == c.frame + 1u ? std::move(c.thisA) : decltype(c.lastA){};
            c.lastAll = frame == c.frame + 1u ? std::move(c.thisAll) : decltype(c.lastAll){};
            c.thisA.clear();
            c.thisAll.clear();
            c.seen.clear();
            c.frame = frame;
        }
        const uint64_t mesh = key;
        const uint32_t occurrence = c.seen[key]++;
        key = (key ^ occurrence) * 0x9E3779B97F4A7C15ull;
        std::array<float, 16> a{}, bm{};
        for (int r = 0; r < 4; ++r)
        {
            std::memcpy(&a[4 * r], b.state.vf[1 + r], 16u);
            std::memcpy(&bm[4 * r], data + ((block + static_cast<uint32_t>(r)) & 0x3FFu) * 16u, 16u);
        }
        const auto all = c.lastAll.find(mesh);
        if (all != c.lastAll.end())
        {
            double nearest = 1e30;
            const size_t from = occurrence > 16u ? occurrence - 16u : 0u;
            const size_t to = std::min<size_t>(all->second.size(), occurrence + 17u);
            for (size_t k = from; k < to; ++k)
            {
                const auto &candidate = all->second[k];
                double worst = 0.0, scale = 1e-30;
                for (int i = 0; i < 16; ++i)
                {
                    worst = std::max(worst, std::fabs(static_cast<double>(bm[i]) - candidate[i]));
                    scale = std::max(scale, std::fabs(static_cast<double>(candidate[i])));
                }
                nearest = std::min(nearest, worst / scale);
            }
            double limit = 1e-6;
            for (int i = 0; i < 5; ++i, limit *= 10.0)
                if (nearest < limit)
                    ++c.nearBelow[i];
        }
        c.thisAll[mesh].push_back(a);
        auto found = c.lastA.find(key);
        if (found != c.lastA.end())
        {
            double worst = 0.0, scale = 1e-30;
            for (int i = 0; i < 16; ++i)
            {
                worst = std::max(worst, std::fabs(static_cast<double>(bm[i]) - found->second[i]));
                scale = std::max(scale, std::fabs(static_cast<double>(found->second[i])));
            }
            const double relative = worst / scale;
            ++c.compared;
            if (std::memcmp(bm.data(), found->second.data(), sizeof(bm)) == 0)
                ++c.exact;
            c.largest = std::max(c.largest, relative);
            c.sum += relative;
            double limit = 1e-6;
            for (int i = 0; i < 5; ++i, limit *= 10.0)
                if (relative < limit)
                    ++c.below[i];
            if (c.values.size() < 200000u)
                c.values.push_back(static_cast<float>(relative));
        }
        c.thisA[key] = a;
        const auto now = std::chrono::steady_clock::now();
        if (now - c.last >= std::chrono::seconds(2))
        {
            c.last = now;
            float median = 0.0f;
            if (!c.values.empty())
            {
                std::nth_element(c.values.begin(), c.values.begin() + static_cast<std::ptrdiff_t>(c.values.size() / 2u), c.values.end());
                median = c.values[c.values.size() / 2u];
            }
            const double total = std::max<double>(1.0, static_cast<double>(c.compared));
            std::fprintf(stderr, "[vu1:moments] %llu meshes: field B against the last field A - %llu the same, relative difference median %.2e, mean %.2e, most %.2e; under 1e-6 %.1f%%, 1e-5 %.1f%%, 1e-4 %.1f%%, 1e-3 %.1f%%, 1e-2 %.1f%%\n",
                         static_cast<unsigned long long>(c.compared), static_cast<unsigned long long>(c.exact), static_cast<double>(median),
                         c.compared ? c.sum / static_cast<double>(c.compared) : 0.0, c.largest, 100.0 * c.below[0] / total,
                         100.0 * c.below[1] / total, 100.0 * c.below[2] / total, 100.0 * c.below[3] / total, 100.0 * c.below[4] / total);
            c.compared = c.exact = 0u;
            c.largest = c.sum = 0.0;
            std::fprintf(stderr, "[vu1:moments] the nearest of the mesh's last field As: under 1e-6 %.1f%%, 1e-5 %.1f%%, 1e-4 %.1f%%, 1e-3 %.1f%%, 1e-2 %.1f%%\n",
                         100.0 * c.nearBelow[0] / total, 100.0 * c.nearBelow[1] / total, 100.0 * c.nearBelow[2] / total,
                         100.0 * c.nearBelow[3] / total, 100.0 * c.nearBelow[4] / total);
            std::fill(std::begin(c.below), std::end(c.below), 0ull);
            std::fill(std::begin(c.nearBelow), std::end(c.nearBelow), 0ull);
            c.values.clear();
        }
    }

    void sendTransformMesh(Batch &b, uint32_t block, ps2vu1native::Run &run)
    {
        if (g_momentCheck.on)
            checkMoment(b, block);
        uint8_t *data = b.data;
        const uint32_t n = b.count, h = b.header;
        thread_local std::vector<uint8_t> packet;
        packet.resize(gsmesh::qwords(n, 2u) * 16u);
        uint8_t *out = packet.data();
        const auto put = [&](uint32_t qword, const void *source) { std::memcpy(out + qword * 16u, source, 16u); };
        const auto putMemory = [&](uint32_t qword, uint32_t from) { put(qword, data + (from & 0x3FFu) * 16u); };
        const uint64_t marker[2] = {gsmesh::kTagLo, gsmesh::kMagic};
        put(0, marker);
        const gsmesh::Header header{gsmesh::kTransform, n, 2u, 0u};
        put(1, &header);
        const __m128i tail = _mm_castps_si128(loadQ(data, 1023)); // the vertex tags' y, z, w
        uint32_t at = 2u;
        for (int field = 0; field < 2; ++field)
        {
            for (uint32_t r = 0; r < 4u; ++r)
            {
                if (field == 0)
                    put(at + r, b.state.vf[1 + r]);
                else
                    putMemory(at + r, block + r);
            }
            for (uint32_t t = 0; t < 3u; ++t)
                putMemory(at + 4u + t, block + (field == 0 ? 8u : 12u) + t);
            const __m128i vertexTag = _mm_insert_epi32(tail, static_cast<int>(field == 0 ? n : b.headerX), 0);
            put(at + 7u, &vertexTag);
            at += gsmesh::kFieldQwords;
        }
        const uint32_t masks[4] = {word(data, h + 3u * n + 4u, 0), word(data, h + 3u * n + 4u, 1), 0u, 0u};
        put(at++, masks);
        for (uint32_t q = 0; q < 3u * n; ++q)
            putMemory(at + q, h + 1u + q);
        run.sendBytes(run.context, out, static_cast<uint32_t>(packet.size()));
    }

    // Batch type 4 (dispatcher 0x388): the first two qwords of every vertex (ST, normal) less
    // the header's z, two vertices at a time (the one past an odd count lands where 0x2800
    // writes its tags afterwards).

    // Routines 0x1d10 then 0x2800 of images 422f0062aa2ca315 and c06ad47f0d18db35 (by their
    // bytes): environment-mapped, lit meshes. A vertex is ST, normal and position; four copies
    // of the strip are sent, each with its tags: context 1 (the reflection: sphere-map
    // coordinates from the view vector reflected about the normal, a Fresnel-like intensity)
    // and context 2 (the vertex's own ST, two directional lights), each for field A and B.
    // Every word as tools/vu1_trace.py --spec gives it, in VU1's order of operations; the
    // lighting block (0x1d10's) holds the environment matrix (q0-q2), the eye (q4), the lights
    // (q5, q9: direction, w a weight), their colours (q6, q10), the material (q7) and the
    // Fresnel factors (q8 x, y); the output block (0x2800's) field B's matrix (q0-q3), the
    // tags (q8-q10, q12-q14) and the alpha (q16 w).
    void litMesh(Batch &b, uint32_t lighting, uint32_t output, ps2vu1native::Run &run)
    {
        uint8_t *data = b.data;
        const uint32_t n = b.count, h = b.header;
        const uint32_t stride = 3u * n + 4u; // a copy: four qwords of tags, then the vertices
        const uint32_t base[4] = {h + 1u, h + 1u + stride, h + 1u + 2u * stride, h + 1u + 3u * stride};
        const __m128 bias = clamp(lane(loadQ(data, h), 2));
        const __m128 env0 = clamp(loadQ(data, lighting + 0u)), env1 = clamp(loadQ(data, lighting + 1u)),
                     env2 = clamp(loadQ(data, lighting + 2u));
        const __m128 eye = clamp(loadQ(data, lighting + 4u));
        const __m128 light1 = clamp(loadQ(data, lighting + 5u)), colour1 = clamp(loadQ(data, lighting + 6u));
        const __m128 material = clamp(loadQ(data, lighting + 7u)), fresnel = clamp(loadQ(data, lighting + 8u));
        const __m128 light2 = clamp(loadQ(data, lighting + 9u)), colour2 = clamp(loadQ(data, lighting + 10u));
        Matrix fieldA{}, fieldB{};
        for (int r = 0; r < 4; ++r)
        {
            fieldA.row[r] = clamp(_mm_loadu_ps(b.state.vf[1 + r]));
            fieldB.row[r] = clamp(loadQ(data, output + static_cast<uint32_t>(r)));
        }
        const __m128 one = _mm_set1_ps(1.0f), half = _mm_set1_ps(0.5f), zero = _mm_setzero_ps();
        // Per batch: the alpha, and the Fresnel and light factors that do not depend on a vertex.
        const __m128i alphaBits = ftoi(_mm_mul_ps(clamp(lane(loadQ(data, output + 16u), 3)), _mm_set1_ps(128.0f)), 1.0f);
        const __m128 fresnelScale = _mm_mul_ps(_mm_mul_ps(one, lane(fresnel, 0)), _mm_set1_ps(255.0f));
        const __m128 materialLight = _mm_mul_ps(material, colour1); // (k7 * k6, per channel)
        const uint32_t masks0 = word(data, h + 3u * n + 4u, 0), masks1 = word(data, h + 3u * n + 4u, 1);

        for (uint32_t g = 0; g < n; g += 4u)
        {
            __m128 a[4], nb[4], c[4];
            for (uint32_t i = 0; i < 4u; ++i)
            {
                const uint32_t v = g + i < n ? g + i : n - 1u; // (lanes past the end: computed, not stored)
                a[i] = _mm_sub_ps(clamp(loadQ(data, base[0] + 3u * v)), bias);
                nb[i] = _mm_sub_ps(clamp(loadQ(data, base[0] + 3u * v + 1u)), bias);
                c[i] = clamp(loadQ(data, base[0] + 3u * v + 2u));
            }
            // Four vertices a lane each: ax = the four vertices' converted ST x, and so on.
            __m128 ax = a[0], ay = a[1], az = a[2], aw = a[3];
            _MM_TRANSPOSE4_PS(ax, ay, az, aw);
            __m128 nx = nb[0], ny = nb[1], nz = nb[2], nw = nb[3];
            _MM_TRANSPOSE4_PS(nx, ny, nz, nw);
            __m128 px = c[0], py = c[1], pz = c[2], pw = c[3];
            _MM_TRANSPOSE4_PS(px, py, pz, pw);
            (void)az;
            (void)aw;
            (void)nw;
            (void)pw;

            // The view vector, normalized (RSQRT 1, |D|^2), and N.V.
            const __m128 dx = _mm_sub_ps(lane(eye, 0), px), dy = _mm_sub_ps(lane(eye, 1), py), dz = _mm_sub_ps(lane(eye, 2), pz);
            const __m128 length2 = _mm_add_ps(_mm_add_ps(_mm_mul_ps(dx, dx), _mm_mul_ps(dy, dy)), _mm_mul_ps(dz, dz));
            const __m128 inverse = rsqrtDivide(one, length2);
            const __m128 vx = _mm_mul_ps(dx, inverse), vy = _mm_mul_ps(dy, inverse), vz = _mm_mul_ps(dz, inverse);
            const __m128 nv = _mm_add_ps(_mm_add_ps(_mm_mul_ps(nx, vx), _mm_mul_ps(ny, vy)), _mm_mul_ps(nz, vz));
            // R = 2 (N.V) N - V, through the environment matrix, normalized (ERLENG).
            const __m128 rx = _mm_sub_ps(_mm_mul_ps(_mm_add_ps(nx, nx), nv), vx);
            const __m128 ry = _mm_sub_ps(_mm_mul_ps(_mm_add_ps(ny, ny), nv), vy);
            const __m128 rz = _mm_sub_ps(_mm_mul_ps(_mm_add_ps(nz, nz), nv), vz);
            const __m128 ex = _mm_add_ps(_mm_add_ps(_mm_mul_ps(lane(env0, 0), rx), _mm_mul_ps(lane(env1, 0), ry)), _mm_mul_ps(lane(env2, 0), rz));
            const __m128 ey = _mm_add_ps(_mm_add_ps(_mm_mul_ps(lane(env0, 1), rx), _mm_mul_ps(lane(env1, 1), ry)), _mm_mul_ps(lane(env2, 1), rz));
            const __m128 ez = _mm_add_ps(_mm_add_ps(_mm_mul_ps(lane(env0, 2), rx), _mm_mul_ps(lane(env1, 2), ry)), _mm_mul_ps(lane(env2, 2), rz));
            const __m128 unit = reciprocalLength(_mm_add_ps(_mm_add_ps(_mm_mul_ps(ex, ex), _mm_mul_ps(ey, ey)), _mm_mul_ps(ez, ez)));
            // Sphere-map coordinates: 0.5 + (E/|E|) * 0.5 / (Ez/|E| + 1); q = Ez/|E| + 1.
            const __m128 sq = _mm_add_ps(_mm_mul_ps(unit, ez), one);
            const __m128 scale = divide(_mm_mul_ps(one, half), sq);
            const __m128 sx = _mm_add_ps(_mm_mul_ps(_mm_mul_ps(unit, ex), scale), half);
            const __m128 sy = _mm_add_ps(_mm_mul_ps(_mm_mul_ps(unit, ey), scale), half);
            // The reflection's intensity: 255 * fresnel.x * ((1 - N.V) * fresnel.y), as an integer, at least 0.
            const __m128 reflection = maxBits(zero, _mm_castsi128_ps(ftoi(_mm_mul_ps(fresnelScale, _mm_mul_ps(_mm_sub_ps(one, nv), lane(fresnel, 1))), 1.0f)));
            // The two lights: max(N.L, 0).
            const __m128 nl1 = maxBits(_mm_add_ps(_mm_add_ps(_mm_mul_ps(nx, lane(light1, 0)), _mm_mul_ps(ny, lane(light1, 1))), _mm_mul_ps(nz, lane(light1, 2))), zero);
            const __m128 nl2 = maxBits(_mm_add_ps(_mm_add_ps(_mm_mul_ps(nx, lane(light2, 0)), _mm_mul_ps(ny, lane(light2, 1))), _mm_mul_ps(nz, lane(light2, 2))), zero);
            __m128 lit[3];
            for (int k = 0; k < 3; ++k)
            {
                const __m128 sum = _mm_add_ps(_mm_mul_ps(_mm_add_ps(_mm_mul_ps(lane(colour2, k), nl2), _mm_mul_ps(lane(colour1, k), nl1)), lane(light1, 3)), lane(light2, 3));
                const __m128 value = _mm_mul_ps(_mm_mul_ps(lane(materialLight, k), sum), _mm_set1_ps(128.0f));
                lit[k] = _mm_castsi128_ps(ftoi(maxBits(mini(value, _mm_set1_ps(255.0f)), zero), 1.0f));
            }
            // Both fields' positions.
            const __m128 clipA[4] = {
                _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(lane(fieldA.row[3], 0), one), _mm_mul_ps(lane(fieldA.row[2], 0), pz)), _mm_mul_ps(lane(fieldA.row[1], 0), py)), _mm_mul_ps(lane(fieldA.row[0], 0), px)),
                _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(lane(fieldA.row[3], 1), one), _mm_mul_ps(lane(fieldA.row[2], 1), pz)), _mm_mul_ps(lane(fieldA.row[1], 1), py)), _mm_mul_ps(lane(fieldA.row[0], 1), px)),
                _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(lane(fieldA.row[3], 2), one), _mm_mul_ps(lane(fieldA.row[2], 2), pz)), _mm_mul_ps(lane(fieldA.row[1], 2), py)), _mm_mul_ps(lane(fieldA.row[0], 2), px)),
                _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(lane(fieldA.row[3], 3), one), _mm_mul_ps(lane(fieldA.row[2], 3), pz)), _mm_mul_ps(lane(fieldA.row[1], 3), py)), _mm_mul_ps(lane(fieldA.row[0], 3), px))};
            const __m128 clipB[4] = {
                _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(lane(fieldB.row[3], 0), one), _mm_mul_ps(lane(fieldB.row[2], 0), pz)), _mm_mul_ps(lane(fieldB.row[1], 0), py)), _mm_mul_ps(lane(fieldB.row[0], 0), px)),
                _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(lane(fieldB.row[3], 1), one), _mm_mul_ps(lane(fieldB.row[2], 1), pz)), _mm_mul_ps(lane(fieldB.row[1], 1), py)), _mm_mul_ps(lane(fieldB.row[0], 1), px)),
                _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(lane(fieldB.row[3], 2), one), _mm_mul_ps(lane(fieldB.row[2], 2), pz)), _mm_mul_ps(lane(fieldB.row[1], 2), py)), _mm_mul_ps(lane(fieldB.row[0], 2), px)),
                _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(lane(fieldB.row[3], 3), one), _mm_mul_ps(lane(fieldB.row[2], 3), pz)), _mm_mul_ps(lane(fieldB.row[1], 3), py)), _mm_mul_ps(lane(fieldB.row[0], 3), px))};
            const __m128 qa = divide(one, clipA[3]), qb = divide(one, clipB[3]);
            const __m128i xa = ftoi4(_mm_mul_ps(clipA[0], qa)), ya = ftoi4(_mm_mul_ps(clipA[1], qa)), za = ftoi4(_mm_mul_ps(clipA[2], qa));
            const __m128i xb = ftoi4(_mm_mul_ps(clipB[0], qb)), yb = ftoi4(_mm_mul_ps(clipB[1], qb)), zb = ftoi4(_mm_mul_ps(clipB[2], qb));
            // The base texture's ST: 0 + (ST - bias) (a zero added: -0 becomes +0).
            const __m128 tx = _mm_add_ps(zero, ax), ty = _mm_add_ps(zero, ay);

            // Per copy, the three qwords of the four vertices, back to one vertex a qword.
            const __m128 stA[4] = {_mm_mul_ps(sx, qa), _mm_mul_ps(sy, qa), _mm_mul_ps(sq, qa), ay};
            const __m128 stB[4] = {_mm_mul_ps(sx, qb), _mm_mul_ps(sy, qb), _mm_mul_ps(sq, qb), zero};
            const __m128 baseA[4] = {_mm_mul_ps(tx, qa), _mm_mul_ps(ty, qa), _mm_mul_ps(one, qa), zero};
            const __m128 baseB[4] = {_mm_mul_ps(tx, qb), _mm_mul_ps(ty, qb), _mm_mul_ps(one, qb), zero};
            const __m128 alpha = _mm_castsi128_ps(alphaBits);
            const __m128 shine[4] = {reflection, reflection, reflection, alpha};
            const __m128 light[4] = {lit[0], lit[1], lit[2], alpha};
            const __m128 xyzA[4] = {_mm_castsi128_ps(xa), _mm_castsi128_ps(ya), _mm_castsi128_ps(za), zero};
            const __m128 xyzB[4] = {_mm_castsi128_ps(xb), _mm_castsi128_ps(yb), _mm_castsi128_ps(zb), zero};
            const struct
            {
                const __m128 *st, *colour, *xyz;
                bool stW; // the ST qword's w written too (copy 0 only)
            } copies[4] = {{stA, shine, xyzA, true}, {stB, shine, xyzB, false}, {baseA, light, xyzA, false}, {baseB, light, xyzB, false}};
            for (int copy = 0; copy < 4; ++copy)
            {
                __m128 st0 = copies[copy].st[0], st1 = copies[copy].st[1], st2 = copies[copy].st[2], st3 = copies[copy].st[3];
                _MM_TRANSPOSE4_PS(st0, st1, st2, st3);
                __m128 c0 = copies[copy].colour[0], c1 = copies[copy].colour[1], c2 = copies[copy].colour[2], c3 = copies[copy].colour[3];
                _MM_TRANSPOSE4_PS(c0, c1, c2, c3);
                __m128 p0 = copies[copy].xyz[0], p1 = copies[copy].xyz[1], p2 = copies[copy].xyz[2], p3 = copies[copy].xyz[3];
                _MM_TRANSPOSE4_PS(p0, p1, p2, p3);
                const __m128 st[4] = {st0, st1, st2, st3}, colour[4] = {c0, c1, c2, c3}, xyz[4] = {p0, p1, p2, p3};
                for (uint32_t i = 0; i < 4u && g + i < n; ++i)
                {
                    const uint32_t v = g + i;
                    const uint32_t at = base[copy] + 3u * v;
                    if (copies[copy].stW)
                        storeQ(data, at, st[i]);
                    else
                        storeXyz(data, at, st[i]);
                    storeQ(data, at + 1u, colour[i]);
                    storeQ(data, at + 2u, _mm_castsi128_ps(_mm_insert_epi32(_mm_castps_si128(xyz[i]), kickBits(masks0, masks1, v), 3)));
                }
            }
        }

        // The tags: per copy, 0x2800's register writes (context 2's with the register's number
        // plus one: SCISSOR_2, XYOFFSET_2) and the vertex tag (context 2's from q1021; the last
        // one's x the header's, with the end-of-packet bit).
        const auto plusOne = [&](uint32_t qword)
        {
            const __m128i value = _mm_castps_si128(loadQ(data, qword));
            const int32_t reg = static_cast<int16_t>((static_cast<uint32_t>(_mm_extract_epi32(value, 2)) + 1u) & 0xFFFFu);
            return _mm_castsi128_ps(_mm_insert_epi32(value, reg, 2));
        };
        const __m128i tail1 = _mm_castps_si128(loadQ(data, 1023)), tail2 = _mm_castps_si128(loadQ(data, 1021));
        const __m128 tags[4][4] = {
            {loadQ(data, output + 8u), loadQ(data, output + 9u), loadQ(data, output + 10u), _mm_castsi128_ps(_mm_insert_epi32(tail1, static_cast<int>(n), 0))},
            {loadQ(data, output + 12u), loadQ(data, output + 13u), loadQ(data, output + 14u), _mm_castsi128_ps(_mm_insert_epi32(tail1, static_cast<int>(n), 0))},
            {loadQ(data, output + 8u), plusOne(output + 9u), plusOne(output + 10u), _mm_castsi128_ps(_mm_insert_epi32(tail2, static_cast<int>(n), 0))},
            {loadQ(data, output + 12u), plusOne(output + 13u), plusOne(output + 14u), _mm_castsi128_ps(_mm_insert_epi32(tail2, static_cast<int>(b.headerX), 0))}};
        for (int copy = 0; copy < 4; ++copy)
            for (uint32_t t = 0; t < 4u; ++t)
                storeQ(data, base[copy] - 4u + t, tags[copy][t]);

        run.send(run.context, (h - 3u) & 0x3FFu, 4u * stride);
    }

    // With moment pictures (docs/RENDERER.md stage 4): a water batch the microcode would clip
    // (q21.x above 0) drawn whole by drawWater instead, when every vertex of its strip, moved by
    // the waves, is in front of the eye in both fields and well inside the GS's coordinates. The
    // microcode's clipper cuts each field at that field's screen edges, so the pieces of the two
    // fields differ and the GPU renderer cannot mix them: at the moments between, holes opened in
    // the river near the screen's edges while the view turned (the user, 2026-10-08, at the bridge
    // after the first Mini-Con). Whole, the GPU's scissor cuts it at the edge, as the joint
    // clipping of 0x1410's batches does (ClipPath::planJoint). Not the microcode's arithmetic:
    // only with moment pictures (TF_MOMENTS_WATER=0 leaves these batches to the microcode).
    bool waterWhole(const Batch &b, uint32_t waves, uint32_t output)
    {
        const uint8_t *data = b.data;
        const uint32_t n = b.count, first = b.header + 1u;
        const __m128 speeds = clamp(loadQ(data, waves + 1u));
        const __m128 wave1 = clamp(loadQ(data, waves + 2u)), wave2 = clamp(loadQ(data, waves + 3u));
        float rows[2][4][4];
        for (int r = 0; r < 4; ++r)
        {
            _mm_storeu_ps(rows[0][r], clamp(_mm_loadu_ps(b.state.vf[1 + r])));
            _mm_storeu_ps(rows[1][r], clamp(loadQ(data, output + static_cast<uint32_t>(r))));
        }
        float s1[4], w1[4], w2[4];
        _mm_storeu_ps(s1, speeds);
        _mm_storeu_ps(w1, wave1);
        _mm_storeu_ps(w2, wave2);
        const auto cubicOf = [](double p)
        {
            const double x = p + 100000.0;
            const double s = 2.0 * (x - std::trunc(x)) - 1.0;
            return s - s * s * s;
        };
        for (uint32_t v = 0; v < n; ++v)
        {
            float c[4];
            _mm_storeu_ps(c, clamp(loadQ(data, first + 3u * v + 2u)));
            const double p1 = (static_cast<double>(c[1]) * w1[1] + static_cast<double>(c[0]) * w1[0] + w1[3]) * s1[1];
            const double p2 = (w2[3] + (static_cast<double>(c[1]) * w2[1] + static_cast<double>(c[0]) * w2[0])) * s1[3];
            const double z = c[2] + cubicOf(p1) * w1[2] + cubicOf(p2) * w2[2];
            for (int field = 0; field < 2; ++field)
            {
                double out[4];
                for (int k = 0; k < 4; ++k)
                    out[k] = rows[field][3][k] + rows[field][2][k] * z + rows[field][1][k] * c[1] + rows[field][0][k] * c[0];
                if (!(out[3] > 1.0e-3))
                    return false; // (at or behind the eye)
                const double x = out[0] / out[3], y = out[1] / out[3], depth = out[2] / out[3];
                if (!(x >= 64.0 && x <= 4032.0 && y >= 64.0 && y <= 4032.0 && depth >= 0.0 && depth < 134217728.0))
                    return false; // (outside the GS's coordinates, or its depth's)
            }
        }
        return true;
    }

    // Routines 0x1df0 then 0x2cc0 of image e1472a8c96f62e62 (by their bytes): water - the
    // river. A vertex is ST, colour (integers) and position; its height is moved by two waves,
    // each the cubic s - s^3 of a phase s = 2 frac(p + 100000) - 1, p = (the position's x, y by
    // the wave's x, y, plus its w) times its speed (the waves q2 and q3, their speeds q1.y and
    // q1.w, their heights q2.z and q3.z). The normal comes from their slopes (the same cubic a
    // quarter of a period on, by q2 and q3's x and y, times q1.x; its z q1.z); the direction to
    // the eye (q0) from the unmoved position (ERLENG), reflected about it, gives sphere-map
    // coordinates 0.5 + R * 0.5 / (Rz + 1). Four copies of the strip go out with the lit mesh's
    // tags: context 1 the ST moved by q4.xy (the current) with the vertex's colour, context 2
    // the sphere map with the colour times q21, each for field A and B (field B's matrix
    // 0x2cc0's q0-q3). Every word as tools/vu1_trace.py --spec gives it, in VU1's order of
    // operations. The vertices are converted (type 3) before. (`vertices`: each vertex's values
    // before the projection, for drawWaterJoint; `send` false leaves the packet unsent.)
    struct WaterVertex
    {
        float position[3]; // moved by the waves
        float st[3];       // context 1's: the ST moved by the current, and its z
        float map[2];      // context 2's: the sphere map's S and T
        __m128 colour;     // the input's (integers)
    };

    void drawWater(Batch &b, uint32_t waves, uint32_t output, ps2vu1native::Run &run, WaterVertex *vertices = nullptr,
                   bool send = true)
    {
        uint8_t *data = b.data;
        const uint32_t n = b.count, h = b.header;
        const uint32_t stride = 3u * n + 4u; // a copy: four qwords of tags, then the vertices
        const uint32_t base[4] = {h + 1u, h + 1u + stride, h + 1u + 2u * stride, h + 1u + 3u * stride};
        const __m128 one = _mm_set1_ps(1.0f), zero = _mm_setzero_ps();
        const __m128 eye = clamp(loadQ(data, waves)), speeds = clamp(loadQ(data, waves + 1u));
        const __m128 wave1 = clamp(loadQ(data, waves + 2u)), wave2 = clamp(loadQ(data, waves + 3u));
        const __m128 current = clamp(loadQ(data, waves + 4u)), tint = clamp(loadQ(data, waves + 21u));
        Matrix fieldA{}, fieldB{};
        for (int r = 0; r < 4; ++r)
        {
            fieldA.row[r] = clamp(_mm_loadu_ps(b.state.vf[1 + r]));
            fieldB.row[r] = clamp(loadQ(data, output + static_cast<uint32_t>(r)));
        }
        const __m128 height1 = _mm_mul_ps(one, lane(wave1, 2)), height2 = _mm_mul_ps(one, lane(wave2, 2));
        const __m128 quarter = _mm_mul_ps(one, _mm_set1_ps(-0.75f)), shift = _mm_set1_ps(100000.0f);
        const __m128 slope = lane(speeds, 0), nz = lane(speeds, 2), nz2 = _mm_add_ps(nz, nz);
        const __m128 half = _mm_mul_ps(one, _mm_set1_ps(0.5f));
        const uint32_t masks0 = word(data, h + 3u * n + 4u, 0), masks1 = word(data, h + 3u * n + 4u, 1);
        const auto fraction = [](__m128 x) { return _mm_sub_ps(x, itof0(_mm_castsi128_ps(ftoi(x, 1.0f)))); }; // x - ITOF0(FTOI0(x))
        const auto cubic = [](__m128 s) { return _mm_sub_ps(s, _mm_mul_ps(_mm_mul_ps(s, s), s)); };
        const auto project = [&](const Matrix &m, int k, __m128 x, __m128 y, __m128 z)
        {
            return _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(lane(m.row[3], k), one), _mm_mul_ps(lane(m.row[2], k), z)),
                                         _mm_mul_ps(lane(m.row[1], k), y)),
                              _mm_mul_ps(lane(m.row[0], k), x));
        };

        for (uint32_t g = 0; g < n; g += 4u)
        {
            __m128 a[4], colours[4], c[4];
            for (uint32_t i = 0; i < 4u; ++i)
            {
                const uint32_t v = g + i < n ? g + i : n - 1u; // (lanes past the end: computed, not stored)
                a[i] = clamp(loadQ(data, base[0] + 3u * v));
                colours[i] = loadQ(data, base[0] + 3u * v + 1u);
                c[i] = clamp(loadQ(data, base[0] + 3u * v + 2u));
            }
            __m128 ax = a[0], ay = a[1], az = a[2], aw = a[3];
            _MM_TRANSPOSE4_PS(ax, ay, az, aw);
            __m128 px = c[0], py = c[1], pz = c[2], pw = c[3];
            _MM_TRANSPOSE4_PS(px, py, pz, pw);
            (void)aw;
            (void)pw;
            // The waves' phases and the moved height.
            const __m128 p1 = _mm_mul_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(py, lane(wave1, 1)), _mm_mul_ps(px, lane(wave1, 0))), lane(wave1, 3)), lane(speeds, 1));
            const __m128 p2 = _mm_mul_ps(_mm_add_ps(lane(wave2, 3), _mm_add_ps(_mm_mul_ps(py, lane(wave2, 1)), _mm_mul_ps(px, lane(wave2, 0)))), lane(speeds, 3));
            const __m128 f1 = fraction(_mm_add_ps(p1, shift)), f2 = fraction(_mm_add_ps(p2, shift));
            const __m128 s1 = _mm_sub_ps(_mm_add_ps(f1, f1), one), s2 = _mm_sub_ps(_mm_add_ps(f2, f2), one);
            const __m128 z = _mm_add_ps(_mm_add_ps(pz, _mm_mul_ps(cubic(s1), height1)), _mm_mul_ps(cubic(s2), height2));
            // The slopes (a quarter of a period on) and the normal.
            const __m128 e1 = fraction(_mm_add_ps(_mm_add_ps(quarter, p1), shift)), e2 = fraction(_mm_add_ps(_mm_add_ps(quarter, p2), shift));
            const __m128 u1 = _mm_sub_ps(_mm_add_ps(e1, e1), one), u2 = _mm_sub_ps(_mm_add_ps(e2, e2), one);
            const __m128 d1 = _mm_mul_ps(cubic(u1), height1), d2 = _mm_mul_ps(cubic(u2), height2);
            const __m128 ny = _mm_mul_ps(_mm_add_ps(_mm_mul_ps(lane(wave1, 1), d1), _mm_mul_ps(lane(wave2, 1), d2)), slope);
            const __m128 nx = _mm_mul_ps(_mm_add_ps(_mm_mul_ps(lane(wave1, 0), d1), _mm_mul_ps(lane(wave2, 0), d2)), slope);
            // The direction to the eye, N.V, and the sphere map of the reflection.
            const __m128 dx = _mm_sub_ps(lane(eye, 0), px), dy = _mm_sub_ps(lane(eye, 1), py), dz = _mm_sub_ps(lane(eye, 2), pz);
            const __m128 k = reciprocalLength(_mm_add_ps(_mm_add_ps(_mm_mul_ps(dx, dx), _mm_mul_ps(dy, dy)), _mm_mul_ps(dz, dz)));
            const __m128 vx = _mm_mul_ps(k, dx), vy = _mm_mul_ps(k, dy), vz = _mm_mul_ps(k, dz);
            const __m128 nv = _mm_add_ps(_mm_add_ps(_mm_mul_ps(nx, vx), _mm_mul_ps(ny, vy)), _mm_mul_ps(one, _mm_mul_ps(nz, vz)));
            const __m128 scale = divide(half, _mm_add_ps(_mm_sub_ps(_mm_mul_ps(nz2, nv), _mm_mul_ps(one, vz)), one));
            const __m128 sphereT = _mm_add_ps(_mm_mul_ps(_mm_sub_ps(_mm_mul_ps(_mm_add_ps(ny, ny), nv), _mm_mul_ps(one, vy)), scale), half);
            const __m128 sphereS = _mm_add_ps(zero, _mm_add_ps(_mm_mul_ps(_mm_sub_ps(_mm_mul_ps(_mm_add_ps(nx, nx), nv), _mm_mul_ps(one, vx)), scale), half));
            const __m128 sphereT0 = _mm_add_ps(zero, sphereT); // (-0 made +0)
            // Both fields' positions, the moved height.
            const __m128 clipA[4] = {project(fieldA, 0, px, py, z), project(fieldA, 1, px, py, z), project(fieldA, 2, px, py, z), project(fieldA, 3, px, py, z)};
            const __m128 clipB[4] = {project(fieldB, 0, px, py, z), project(fieldB, 1, px, py, z), project(fieldB, 2, px, py, z), project(fieldB, 3, px, py, z)};
            const __m128 qa = divide(one, clipA[3]), qb = divide(one, clipB[3]);
            const __m128 xyzA[4] = {_mm_castsi128_ps(ftoi4(_mm_mul_ps(clipA[0], qa))), _mm_castsi128_ps(ftoi4(_mm_mul_ps(clipA[1], qa))),
                                    _mm_castsi128_ps(ftoi4(_mm_mul_ps(clipA[2], qa))), zero};
            const __m128 xyzB[4] = {_mm_castsi128_ps(ftoi4(_mm_mul_ps(clipB[0], qb))), _mm_castsi128_ps(ftoi4(_mm_mul_ps(clipB[1], qb))),
                                    _mm_castsi128_ps(ftoi4(_mm_mul_ps(clipB[2], qb))), zero};
            // ST: context 1 moved by the current, context 2 the sphere map; each times the field's Q.
            const __m128 tx = _mm_add_ps(ax, lane(current, 0)), ty = _mm_add_ps(ay, lane(current, 1));
            const __m128 stA[4] = {_mm_mul_ps(tx, qa), _mm_mul_ps(ty, qa), _mm_mul_ps(az, qa), sphereT};
            const __m128 stB[4] = {_mm_mul_ps(tx, qb), _mm_mul_ps(ty, qb), _mm_mul_ps(az, qb), zero};
            const __m128 mapA[4] = {_mm_mul_ps(sphereS, qa), _mm_mul_ps(sphereT0, qa), _mm_mul_ps(one, qa), zero};
            const __m128 mapB[4] = {_mm_mul_ps(sphereS, qb), _mm_mul_ps(sphereT0, qb), _mm_mul_ps(one, qb), zero};
            if (vertices)
            {
                float lanes[8][4];
                const __m128 values[8] = {px, py, z, tx, ty, az, sphereS, sphereT0};
                for (int k = 0; k < 8; ++k)
                    _mm_storeu_ps(lanes[k], values[k]);
                for (uint32_t i = 0; i < 4u && g + i < n; ++i)
                {
                    WaterVertex &w = vertices[g + i];
                    for (int k = 0; k < 3; ++k)
                    {
                        w.position[k] = lanes[k][i];
                        w.st[k] = lanes[3 + k][i];
                    }
                    w.map[0] = lanes[6][i];
                    w.map[1] = lanes[7][i];
                    w.colour = colours[i];
                }
            }
            const struct
            {
                const __m128 *st, *xyz;
                bool stW;      // the ST qword's w written too (copy 0 only)
                int colour;    // 0 left as it is (copy 0: the input's), 1 the vertex's, 2 times q21
            } copies[4] = {{stA, xyzA, true, 0}, {stB, xyzB, false, 1}, {mapA, xyzA, false, 2}, {mapB, xyzB, false, 2}};
            for (int copy = 0; copy < 4; ++copy)
            {
                __m128 st0 = copies[copy].st[0], st1 = copies[copy].st[1], st2 = copies[copy].st[2], st3 = copies[copy].st[3];
                _MM_TRANSPOSE4_PS(st0, st1, st2, st3);
                __m128 p0 = copies[copy].xyz[0], p1x = copies[copy].xyz[1], p2x = copies[copy].xyz[2], p3 = copies[copy].xyz[3];
                _MM_TRANSPOSE4_PS(p0, p1x, p2x, p3);
                const __m128 st[4] = {st0, st1, st2, st3}, xyz[4] = {p0, p1x, p2x, p3};
                for (uint32_t i = 0; i < 4u && g + i < n; ++i)
                {
                    const uint32_t v = g + i;
                    const uint32_t at = base[copy] + 3u * v;
                    if (copies[copy].stW)
                        storeQ(data, at, st[i]);
                    else
                        storeXyz(data, at, st[i]);
                    if (copies[copy].colour == 1)
                        storeQ(data, at + 1u, colours[i]);
                    else if (copies[copy].colour == 2)
                        storeQ(data, at + 1u, _mm_castsi128_ps(ftoi(_mm_mul_ps(itof0(colours[i]), tint), 1.0f)));
                    storeQ(data, at + 2u, _mm_castsi128_ps(_mm_insert_epi32(_mm_castps_si128(xyz[i]), kickBits(masks0, masks1, v), 3)));
                }
            }
        }

        // The tags, as the lit mesh's: per copy, 0x2cc0's register writes (context 2's with the
        // register's number plus one) and the vertex tag (context 2's from q1021; the last one's x
        // the header's, with the end-of-packet bit).
        const auto plusOne = [&](uint32_t qword)
        {
            const __m128i value = _mm_castps_si128(loadQ(data, qword));
            const int32_t reg = static_cast<int16_t>((static_cast<uint32_t>(_mm_extract_epi32(value, 2)) + 1u) & 0xFFFFu);
            return _mm_castsi128_ps(_mm_insert_epi32(value, reg, 2));
        };
        const __m128i tail1 = _mm_castps_si128(loadQ(data, 1023)), tail2 = _mm_castps_si128(loadQ(data, 1021));
        const __m128 tags[4][4] = {
            {loadQ(data, output + 8u), loadQ(data, output + 9u), loadQ(data, output + 10u), _mm_castsi128_ps(_mm_insert_epi32(tail1, static_cast<int>(n), 0))},
            {loadQ(data, output + 12u), loadQ(data, output + 13u), loadQ(data, output + 14u), _mm_castsi128_ps(_mm_insert_epi32(tail1, static_cast<int>(n), 0))},
            {loadQ(data, output + 8u), plusOne(output + 9u), plusOne(output + 10u), _mm_castsi128_ps(_mm_insert_epi32(tail2, static_cast<int>(n), 0))},
            {loadQ(data, output + 12u), plusOne(output + 13u), plusOne(output + 14u), _mm_castsi128_ps(_mm_insert_epi32(tail2, static_cast<int>(b.headerX), 0))}};
        for (int copy = 0; copy < 4; ++copy)
            for (uint32_t t = 0; t < 4u; ++t)
                storeQ(data, base[copy] - 4u + t, tags[copy][t]);

        if (send)
            run.send(run.context, (h - 3u) & 0x3FFu, 4u * stride);
    }

    // A matrix row times another matrix (MULAx ACC, m0, row.x; MADDAy ACC, m1, row.y; MADDAz
    // ACC, m2, row.z; MADDw out, m3, row.w).
    TF_INLINE __m128 rowTimes(const Matrix &m, __m128 row)
    {
        __m128 acc = _mm_mul_ps(m.row[0], lane(row, 0));
        acc = _mm_add_ps(acc, _mm_mul_ps(m.row[1], lane(row, 1)));
        acc = _mm_add_ps(acc, _mm_mul_ps(m.row[2], lane(row, 2)));
        return _mm_add_ps(acc, _mm_mul_ps(m.row[3], lane(row, 3)));
    }

    // CLIP's judgement of x, y and z against +-|w|, on the bits (an exponent of 0 counts as
    // zero): its six flags (bit 0 x above +|w|, bit 1 x below -|w|, then y, then z).
    TF_INLINE uint32_t clipFlags(__m128 value)
    {
        const __m128i bits = _mm_castps_si128(value);
        const uint32_t w = static_cast<uint32_t>(_mm_extract_epi32(bits, 3));
        const __m128i limit = _mm_set1_epi32((w & 0x7F800000u) != 0u ? static_cast<int>(w & 0x7FFFFFFFu) : 0x007FFFFF);
        const uint32_t above = static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpgt_epi32(bits, limit))));
        const uint32_t below = static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(
            _mm_cmpgt_epi32(_mm_xor_si128(bits, _mm_set1_epi32(static_cast<int>(0x80000000u))), limit))));
        return (above & 1u) | ((below & 1u) << 1) | ((above & 2u) << 1) | ((below & 2u) << 2) | ((above & 4u) << 2) |
               ((below & 4u) << 3);
    }

    // As FCAND 0x3f reads a judgement back: 1 when any of its six flags is set.
    TF_INLINE uint32_t outsideOf(__m128 value) { return clipFlags(value) != 0u ? 1u : 0u; }

    // Whether qwords [a, a + aSize) and [b, b + bSize) of data memory share one (it wraps).
    bool overlaps(uint32_t a, uint32_t aSize, uint32_t b, uint32_t bSize)
    {
        return ((b - a) & 0x3FFu) < aSize || ((a - b) & 0x3FFu) < bSize;
    }

    // Routine 0x1ac0 of image bec5738323d4afd8 (by its bytes), alone in the routine table of
    // a batch of type 3: one mesh drawn at several places. The data block holds field A's and
    // field B's register writes (q0-q2, q3-q5), what turns field A's matrix into field B's
    // (q6-q9), the guard band (q10, q11), the number of instances (q12 x) and per instance a
    // colour and field A's matrix (q13 + 5i, q14-q17 + 5i). Per instance one packet of both
    // fields goes out, from two buffers after the input in turn; the colour is the vertex's
    // times the instance's. A vertex does not start a triangle (bit 15 of XYZF2's w) when the
    // mask after the vertices says so (16 vertices to its x, the next 16 to its y) or when it
    // or one of the two before it is outside the guard band: CLIP of q11 * w + q10 * the
    // position, for each field.
    void drawInstances(Batch &b, uint32_t block, uint32_t count, ps2vu1native::Run &run)
    {
        uint8_t *data = b.data;
        const uint32_t n = b.count, h = b.header;
        const uint32_t stride = 3u * n + 4u; // a field: four qwords of tags, then the vertices
        const uint32_t fieldA[2] = {h + 1u + stride, h + 1u + 3u * stride};
        const uint32_t masks[2] = {word(data, h + 3u * n + 4u, 0) & 0xFFFFu, word(data, h + 3u * n + 4u, 1) & 0xFFFFu};
        const __m128i tail = _mm_castps_si128(loadQ(data, 1023)); // the vertex tags' y, z, w

        // The register writes and the vertex tags (field B's x the header's, with the
        // end-of-packet bit), the same in both buffers.
        for (const uint32_t a : fieldA)
        {
            for (uint32_t t = 0; t < 3u; ++t)
            {
                storeQ(data, a - 4u + t, loadQ(data, block + t));
                storeQ(data, a + stride - 4u + t, loadQ(data, block + 3u + t));
            }
            storeQ(data, a - 1u, _mm_castsi128_ps(_mm_insert_epi32(tail, static_cast<int>(n), 0)));
            storeQ(data, a + stride - 1u, _mm_castsi128_ps(_mm_insert_epi32(tail, static_cast<int>(b.headerX), 0)));
        }

        Matrix toFieldB{};
        for (int r = 0; r < 4; ++r)
            toFieldB.row[r] = clamp(loadQ(data, block + 6u + static_cast<uint32_t>(r)));
        const __m128 guardScale = clamp(loadQ(data, block + 10u)), guardOffset = clamp(loadQ(data, block + 11u));
        uint32_t history[2] = {}; // per field: whether each of the last vertices was outside
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t instance = block + 5u * i;
            const __m128 tint = clamp(loadQ(data, instance + 13u));
            Matrix matrix[2] = {};
            for (int r = 0; r < 4; ++r)
                matrix[0].row[r] = clamp(loadQ(data, instance + 14u + static_cast<uint32_t>(r)));
            for (int r = 0; r < 4; ++r)
                matrix[1].row[r] = rowTimes(toFieldB, matrix[0].row[r]);
            const uint32_t out[2] = {fieldA[i & 1u], fieldA[i & 1u] + stride};
            history[0] = history[1] = 0u;
            for (uint32_t v = 0; v < n; ++v)
            {
                const uint32_t at = b.vertex(v);
                const __m128 st = clamp(loadQ(data, at));
                const __m128 position = clamp(loadQ(data, at + 2u));
                const __m128 colour = _mm_castsi128_ps(ftoi(_mm_mul_ps(itof0(loadQ(data, at + 1u)), tint), 1.0f));
                const uint32_t mask = (v < 16u ? masks[0] << v : masks[1] << (v - 16u)) & 0xFFFFu;
                const __m128 x = lane(position, 0), y = lane(position, 1), z = lane(position, 2);
                for (int field = 0; field < 2; ++field)
                {
                    const __m128 clip = transform(matrix[field], x, y, z);
                    const __m128 w = lane(clip, 3);
                    const __m128 guard = _mm_add_ps(_mm_mul_ps(guardOffset, w), _mm_mul_ps(guardScale, clip));
                    history[field] = (history[field] << 1) | outsideOf(guard);
                    const int32_t kick = static_cast<int16_t>(((history[field] + 0x7FFFu) | mask) & 0xFFFFu);
                    history[field] &= 3u;
                    const __m128 q = reciprocal(w);
                    const uint32_t slot = out[field] + 3u * v;
                    storeXyz(data, slot, _mm_mul_ps(st, q));
                    storeQ(data, slot + 1u, colour);
                    storeQ(data, slot + 2u, _mm_castsi128_ps(_mm_insert_epi32(ftoi4(_mm_mul_ps(clip, q)), kick, 3)));
                }
            }
            run.send(run.context, (out[0] - 4u) & 0x3FFu, 2u * stride);
        }
        b.state.vi[14] = static_cast<int32_t>(history[1]); // (the stack pointer of 0x0480 at MSCAL 0)
    }

    // ---- 0x1410's clipping path: 0x0480 for each field, with its triangle clipper ----

    // The qwords an XGKICK at `start` sends: its GIF tags walked as PATH1 does (0 when the
    // packet would not fit the transfer buffer: the microcode's run sends nothing then).
    uint32_t packetQwords(const uint8_t *data, uint32_t start)
    {
        constexpr uint32_t kLimit = 0x10000u / 16u;
        uint32_t qwords = 0;
        for (;;)
        {
            if (qwords > kLimit - 1u)
                return 0u;
            const uint32_t at = start + qwords;
            const uint64_t tag = static_cast<uint64_t>(word(data, at, 0)) | (static_cast<uint64_t>(word(data, at, 1)) << 32);
            const uint64_t nloop = tag & 0x7FFFu, format = (tag >> 58) & 3u;
            uint64_t nreg = (tag >> 60) & 0xFu;
            if (nreg == 0u)
                nreg = 16u;
            const uint64_t size = 1u + (format == 0u ? nloop * nreg : format == 1u ? (nloop * nreg + 1u) / 2u : nloop);
            if (size > kLimit - qwords)
                return 0u;
            qwords += static_cast<uint32_t>(size);
            if ((tag >> 15) & 1u)
                return qwords;
        }
    }

    void kickAt(ps2vu1native::Run &run, const uint8_t *data, uint32_t start)
    {
        const uint32_t qwords = packetQwords(data, start);
        if (qwords != 0u)
            run.send(run.context, start & 0x3FFu, qwords);
    }

    // With q21.x above 0, 0x1410 sets the drawing-kick masks into the positions' w and runs
    // 0x0480 for field A, then - with the matrices swapped about - for field B. 0x0480 draws
    // the strip after the input (field A, then field B after it), every vertex through the
    // field's matrix (vf01-vf04) and through the clipping matrix (0x0480's data block q0-q3,
    // CLIP against w); a triangle with a vertex outside is not drawn in the strip (bit 15 of
    // the last vertex's XYZF2 w), and unless all three are outside one plane it is clipped
    // against the six planes in turn (-z, +z, -x, +x, -y, +y) and drawn as a polygon of its
    // own: first the field's register writes, then the polygon (from 0x0480's data block q9).
    // Every word as the microcode leaves it, the clipper's work buffers (q117, q147) and the
    // registers it saves on 0x0480's stack (q999 on) included.
    struct ClipPath
    {
        Batch &b;
        ps2vu1native::Run &run;
        uint8_t *data;
        uint32_t h, n;
        uint32_t block0480, block0480Bits; // (as an address; as the 16 bits the microcode keeps)
        int32_t vi12, vi13;
        Matrix field{};     // vf01-vf04 as the microcode has them (their bits)
        Matrix fieldMath{}; // ... as FMAC operands
        __m128 kept[5];     // vf17, vf18, vf26, vf27, vf31: what 0x0480 saves without writing
        uint32_t savedAt = 0;
        // Moment pictures (TF_MOMENTS=1, docs/RENDERER.md stage 4): the two fields clipped alike.
        // The GPU renderer draws a moment between the fields by mixing field A's pieces with field
        // B's, which the clipper, clipping each field by its own planes, would make different: a
        // triangle is clipped once instead, in its own barycentric coordinates, against both
        // fields' planes (planJoint), and each field's pass draws that same polygon through its
        // own matrices (drawJointPolygon); the strip leaves out the same triangles in both. The
        // clipper's x and y planes are the screen's edges (x = +-w 257 GS pixels either side of
        // its middle, y = +-w its top and bottom rows: TF_MOMENTS_PLANES), so cutting one field
        // at the other's would take away what it shows near an edge: they are taken kJointReach
        // times as far out - still well inside the GS's coordinates - and a triangle across an
        // edge goes whole into the strip, the GPU's scissor cutting it there. What only one field
        // sees is lost: slivers past that and at the near plane. Not the microcode's arithmetic:
        // only with moment pictures.
        static constexpr float kJointReach = 6.0f;
        bool joint = false;
        struct JointTriangle
        {
            uint8_t kind = 0;  // 0 drawn in the strip, 1 culled, 2 a polygon
            uint8_t count = 0; // the polygon's vertices
            float bary[16][3]; // ... as weights of the triangle's corners
        };
        std::vector<JointTriangle> *plan = nullptr; // (by the strip's vertex the triangle ends at)

        void setField(const __m128 (&rows)[4])
        {
            for (int r = 0; r < 4; ++r)
            {
                field.row[r] = rows[r];
                fieldMath.row[r] = clamp(rows[r]);
            }
        }

        static constexpr uint32_t kBufferA = 117u, kBufferB = 147u;
        static constexpr uint32_t kSaveLink = 0x0750u >> 3, kRestoreLink = 0x0770u >> 3;

        // One run of 0x0480 (`link` the VI its caller is returned to).
        uint32_t pass(uint32_t sp, uint32_t link)
        {
            const uint32_t which = (1u - word(data, 21, 2)) & 0xFFFFu; // q21.z toggled: 0 field A, 1 field B
            storeHalf(data, 21, 2, which);
            --sp;
            storeHalf(data, sp, 0, n);
            storeHalf(data, sp, 1, link);
            storeHalf(data, sp, 2, static_cast<uint32_t>(vi13));
            const __m128 vf30 = loadQ(data, 1022);
            Matrix clipping{};
            for (uint32_t r = 0; r < 4u; ++r)
                clipping.row[r] = clamp(loadQ(data, block0480 + r));
            storeQ(data, block0480 + 9u, vf30);
            const __m128 tag = _mm_castsi128_ps(_mm_insert_epi32(_mm_castps_si128(loadQ(data, 1023)), static_cast<int>(word(data, h, 0)), 0));
            const uint32_t region = static_cast<int16_t>(which) <= 0 ? h + 1u + 3u * n : h + 6u * n + 5u;
            const uint32_t out0 = region + 4u;
            storeQ(data, out0 - 1u, tag);

            const __m128 position0 = clamp(loadQ(data, h + 3u));
            __m128 clipNow = transform(clipping, lane(position0, 0), lane(position0, 1), lane(position0, 2));
            __m128 screenNow = transform(fieldMath, lane(position0, 0), lane(position0, 1), lane(position0, 2));
            const __m128 origin = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
            __m128 p2 = origin, p1 = origin, p0 = origin;
            uint32_t c2 = 0, c1 = 0;
            for (uint32_t v = 0; v < n; ++v)
            {
                p2 = p1;
                p1 = p0;
                p0 = clipNow;
                const uint32_t positionW = word(data, h + 3u + 3u * v, 3);
                const __m128 next = loadQ(data, h + 6u + 3u * v);
                const __m128 colour = loadQ(data, h + 2u + 3u * v);
                const __m128 q = reciprocal(lane(screenNow, 3));
                const uint32_t j2 = clipFlags(p2), j1 = clipFlags(p1), j0 = clipFlags(p0);
                const __m128 nextClamped = clamp(next);
                const __m128 nx = lane(nextClamped, 0), ny = lane(nextClamped, 1), nz = lane(nextClamped, 2);
                const __m128 clipNext = transform(clipping, nx, ny, nz);
                const uint32_t out = out0 + 3u * v;
                storeQ(data, out + 1u, colour);
                const __m128 stq = _mm_blend_ps(_mm_mul_ps(clamp(loadQ(data, h + 1u + 3u * v)), q), next, 0x8);
                storeXyz(data, out, stq);
                storeQ(data, out + 2u, _mm_castsi128_ps(_mm_insert_epi32(ftoi4(_mm_mul_ps(clamp(screenNow), q)), static_cast<int>(positionW), 3)));
                const uint32_t c0 = (j0 & 0x3Fu) != 0u ? 1u : 0u;
                const __m128 screenNext = transform(fieldMath, nx, ny, nz);
                if (joint && plan)
                {
                    // (both fields alike: planJoint)
                    if (v >= 2u && (positionW & 0x8000u) == 0u && (*plan)[v].kind != 0u)
                    {
                        if ((*plan)[v].kind == 2u)
                            drawJointPolygon(v, out, (*plan)[v], clipping);
                        storeHalf(data, out + 2u, 3, 0x8000u);
                    }
                }
                else if (v >= 2u && c2 + c1 + c0 > 0u && (positionW & 0x8000u) == 0u)
                {
                    if ((j2 & j1 & j0 & 0x3Fu) == 0u)
                    {
                        const __m128 registers[15] = {kept[0], kept[1], tag, next, stq, p2, p1, p0, screenNext,
                                                      kept[2], kept[3], colour, clipNext, vf30, kept[4]};
                        const uint32_t vi[12] = {region, h + 1u + 3u * v, c2 + c1 + c0, c1, c0, out, 0u, 0x8000u,
                                                 n - v, block0480Bits, static_cast<uint32_t>(vi12), static_cast<uint32_t>(vi13)};
                        save(sp, registers, vi);
                        clipTriangle(v, out, p2, p1, p0);
                        storeHalf(data, savedAt, 0, savedAt); // the restore's ISWR (its own link)
                        storeHalf(data, savedAt, 1, kRestoreLink);
                    }
                    storeHalf(data, out + 2u, 3, 0x8000u);
                }
                c2 = c1;
                c1 = c0;
                clipNow = clipNext;
                screenNow = screenNext;
            }
            kickAt(run, data, region);
            return sp + 1u;
        }

        // Each triangle of the strip as both fields will draw it (`joint`): with the current field's
        // clipping matrix (0x0480's q0-q3) and the other's (`otherBlock` q4-q7: 0x1410's or 0x22a0's
        // data block, which swapMatrices trades with it).
        void planJoint(uint32_t otherBlock)
        {
            Matrix clipA{}, clipB{};
            for (uint32_t r = 0; r < 4u; ++r)
            {
                clipA.row[r] = clamp(loadQ(data, block0480 + r));
                clipB.row[r] = clamp(loadQ(data, otherBlock + 4u + r));
            }
            thread_local std::vector<JointTriangle> triangles;
            triangles.assign(n, JointTriangle{});
            {
                // (TF_MOMENTS_PLANES=<n>, debugging aid: where the clipping planes x, y = +-w lie on the
                // screen - the clip-space points through 0x0480's q4-q7 - for the first n batches)
                static int planesLeft = []()
                {
                    const char *env = std::getenv("TF_MOMENTS_PLANES");
                    return env ? std::atoi(env) : 0;
                }();
                if (planesLeft > 0)
                {
                    --planesLeft;
                    Matrix toScreen{};
                    for (uint32_t r = 0; r < 4u; ++r)
                        toScreen.row[r] = clamp(loadQ(data, block0480 + 4u + r));
                    const float w = 100.0f;
                    const __m128 points[5] = {_mm_set_ps(w, 0.0f, 0.0f, 0.0f), _mm_set_ps(w, 0.0f, 0.0f, w), _mm_set_ps(w, 0.0f, 0.0f, -w),
                                              _mm_set_ps(w, 0.0f, w, 0.0f), _mm_set_ps(w, 0.0f, -w, 0.0f)};
                    float out[5][4];
                    for (int k = 0; k < 5; ++k)
                    {
                        const __m128 screen = rowTimes(toScreen, points[k]);
                        _mm_storeu_ps(out[k], _mm_div_ps(screen, lane(screen, 3)));
                    }
                    std::fprintf(stderr, "[vu1:planes] centre %.1f,%.1f; x=+w %.1f, x=-w %.1f; y=+w %.1f, y=-w %.1f\n", out[0][0], out[0][1],
                                 out[1][0], out[2][0], out[3][1], out[4][1]);
                }
            }
            plan = &triangles;
            __m128 inA[32], inB[32];
            uint32_t flagsA[32], flagsB[32];
            for (uint32_t v = 0; v < n && v < 32u; ++v)
            {
                const __m128 position = clamp(loadQ(data, h + 3u + 3u * v));
                inA[v] = reachOut(transform(clipA, lane(position, 0), lane(position, 1), lane(position, 2)));
                inB[v] = reachOut(transform(clipB, lane(position, 0), lane(position, 1), lane(position, 2)));
                flagsA[v] = clipFlags(inA[v]) & 0x3Fu;
                flagsB[v] = clipFlags(inB[v]) & 0x3Fu;
            }
            for (uint32_t v = 2u; v < n && v < 32u; ++v)
            {
                if ((word(data, h + 3u + 3u * v, 3) & 0x8000u) != 0u)
                    continue; // (not a triangle of the strip)
                JointTriangle &t = triangles[v];
                const uint32_t anyA = flagsA[v - 2u] | flagsA[v - 1u] | flagsA[v], anyB = flagsB[v - 2u] | flagsB[v - 1u] | flagsB[v];
                const uint32_t allA = flagsA[v - 2u] & flagsA[v - 1u] & flagsA[v], allB = flagsB[v - 2u] & flagsB[v - 1u] & flagsB[v];
                if ((anyA | anyB) == 0u)
                    continue; // inside both: the strip draws it
                t.kind = 1;
                if (allA != 0u || allB != 0u)
                    continue; // all outside one plane in a field
                float polygon[24][3];
                const uint32_t count = clipBoth(&inA[v - 2u], &inB[v - 2u], polygon);
                if (count < 3u)
                    continue;
                t.kind = 2;
                t.count = static_cast<uint8_t>(std::min<uint32_t>(count, 16u));
                std::memcpy(t.bary, polygon, sizeof(float) * 3u * t.count);
            }
        }

        // A clipping position with x and y over kJointReach: the planes that far out.
        static __m128 reachOut(__m128 clip) { return _mm_mul_ps(clip, _mm_set_ps(1.0f, 1.0f, 1.0f / kJointReach, 1.0f / kJointReach)); }

        // A triangle clipped once against both fields' planes, in its own barycentric coordinates
        // (`cornersA`, `cornersB`: its three corners' clipping positions in fields A and B,
        // reachOut): the polygon left, as weights of the corners, and how many vertices it has
        // (fewer than 3: none).
        static uint32_t clipBoth(const __m128 *cornersA, const __m128 *cornersB, float (&polygon)[24][3])
        {
            // (as clipTriangle's planes: -z, +z, -x, +x, -y, +y; the flag of being outside each)
            static constexpr struct
            {
                int axis;
                float sign;
                uint32_t outside;
            } kPlanes[6] = {{2, -1.0f, 1u << 5}, {2, 1.0f, 1u << 4}, {0, -1.0f, 1u << 1},
                            {0, 1.0f, 1u << 0},  {1, -1.0f, 1u << 3}, {1, 1.0f, 1u << 2}};
            const float start[3][3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
            std::memcpy(polygon, start, sizeof(start));
            float next[24][3];
            uint32_t count = 3u;
            for (int field = 0; field < 2 && count >= 3u; ++field)
            {
                const __m128 *corners = field == 0 ? cornersA : cornersB;
                const auto at = [&](const float (&weights)[3])
                {
                    return _mm_add_ps(_mm_add_ps(_mm_mul_ps(corners[0], _mm_set1_ps(weights[0])), _mm_mul_ps(corners[1], _mm_set1_ps(weights[1]))),
                                      _mm_mul_ps(corners[2], _mm_set1_ps(weights[2])));
                };
                for (const auto &plane : kPlanes)
                {
                    if (count < 3u)
                        break;
                    uint32_t written = 0u;
                    for (uint32_t e = 0; e < count; ++e)
                    {
                        const float (&from)[3] = polygon[(e + count - 1u) % count];
                        const float (&to)[3] = polygon[e];
                        const __m128 a = at(from), c = at(to);
                        const bool fromOut = (clipFlags(a) & plane.outside) != 0u, toOut = (clipFlags(c) & plane.outside) != 0u;
                        if (fromOut != toOut && written < 24u)
                        {
                            // where the edge meets the plane: d = x_axis - sign * w
                            float la[4], lc[4];
                            _mm_storeu_ps(la, a);
                            _mm_storeu_ps(lc, c);
                            const float dFrom = la[plane.axis] - plane.sign * la[3], dTo = lc[plane.axis] - plane.sign * lc[3];
                            const float u = dFrom != dTo ? dFrom / (dFrom - dTo) : 0.0f;
                            for (int k = 0; k < 3; ++k)
                                next[written][k] = from[k] + (to[k] - from[k]) * u;
                            ++written;
                        }
                        if (!toOut && written < 24u)
                        {
                            for (int k = 0; k < 3; ++k)
                                next[written][k] = to[k];
                            ++written;
                        }
                    }
                    std::memcpy(polygon, next, sizeof(float) * 3u * written);
                    count = written;
                }
            }
            return count;
        }

        // A planned polygon (planJoint) drawn as 0x0f50 draws one (drawPolygon): the field's register
        // writes, then the polygon - a fan of at most 9 vertices a packet - each vertex a corner through
        // the field's matrix or a point between them through 0x0480's q4-q7 from its clipping position.
        void drawJointPolygon(uint32_t v, uint32_t out, const JointTriangle &t, const Matrix &clipping)
        {
            const uint32_t packet = block0480 + 9u;
            uint32_t tags = word(data, 21, 3) & 0xFFFFu;
            if (tags == 0u)
                tags = 1023u;
            Matrix toScreen{}, fieldNow{};
            for (uint32_t r = 0; r < 4u; ++r)
            {
                toScreen.row[r] = clamp(loadQ(data, block0480 + 4u + r));
                fieldNow.row[r] = clamp(loadQ(data, r));
            }
            __m128 positions[3], clips[3], colours[3], sts[3];
            for (uint32_t k = 0; k < 3u; ++k)
            {
                positions[k] = clamp(loadQ(data, h + 3u + 3u * (v - 2u + k)));
                clips[k] = transform(clipping, lane(positions[k], 0), lane(positions[k], 1), lane(positions[k], 2));
                colours[k] = itof0(loadQ(data, out - 5u + 3u * k));
                sts[k] = clamp(loadQ(data, h + 1u + 3u * (v - 2u + k)));
            }
            const auto mix = [&](const __m128 (&values)[3], const float (&weights)[3])
            {
                return _mm_add_ps(_mm_add_ps(_mm_mul_ps(values[0], _mm_set1_ps(weights[0])), _mm_mul_ps(values[1], _mm_set1_ps(weights[1]))),
                                  _mm_mul_ps(values[2], _mm_set1_ps(weights[2])));
            };
            // (XYZF2's F: the last new vertex's colour w, as the clipper leaves it in vf26; or the
            // middle corner's)
            __m128 lastColour = colours[1];
            for (uint32_t i = 0; i < t.count; ++i)
            {
                const float (&w)[3] = t.bary[i];
                if (!(w[0] == 1.0f || w[1] == 1.0f || w[2] == 1.0f))
                    lastColour = mix(colours, w);
            }
            const int32_t fog = _mm_extract_epi32(ftoi4(lastColour), 3);
            kickAt(run, data, tags);
            for (uint32_t start = 1u; start + 1u < t.count; start += 7u)
            {
                const uint32_t more = std::min<uint32_t>(8u, t.count - start);
                storeHalf(data, packet, 0, (more + 1u) + 0x8000u);
                for (uint32_t i = 0; i <= more; ++i)
                {
                    const uint32_t index = i == 0u ? 0u : start + i - 1u;
                    const float (&w)[3] = t.bary[index];
                    const int corner = w[0] == 1.0f ? 0 : (w[1] == 1.0f ? 1 : (w[2] == 1.0f ? 2 : -1));
                    const __m128 screen = corner >= 0 ? transform(fieldNow, lane(positions[corner], 0), lane(positions[corner], 1),
                                                                  lane(positions[corner], 2))
                                                      : rowTimes(toScreen, clamp(mix(clips, w)));
                    const __m128 q = reciprocal(lane(screen, 3));
                    storeQ(data, packet + 2u + 3u * i, _mm_castsi128_ps(ftoi(corner >= 0 ? colours[corner] : mix(colours, w), 1.0f)));
                    storeXyz(data, packet + 1u + 3u * i, _mm_mul_ps(clamp(corner >= 0 ? sts[corner] : mix(sts, w)), q));
                    storeQ(data, packet + 3u + 3u * i, _mm_castsi128_ps(_mm_insert_epi32(ftoi4(_mm_mul_ps(screen, q)), fog, 3)));
                }
                if (static_cast<int16_t>((word(data, 4, 1) - 2u) & 0xFFFFu) < 0) // q4.y less 2, as a VI
                    kickAt(run, data, packet);
            }
        }

        // 0x1180: vf17-vf31, then vi02-vi13, vi14 and the link, down from the stack pointer.
        void save(uint32_t sp, const __m128 (&registers)[15], const uint32_t (&vi)[12])
        {
            for (const __m128 &value : registers)
                storeQ(data, --sp, value);
            for (uint32_t group = 0; group < 3u; ++group)
            {
                --sp;
                for (uint32_t l = 0; l < 4u; ++l)
                    storeHalf(data, sp, l, vi[group * 4u + l]);
            }
            --sp;
            storeHalf(data, sp, 0, sp);
            storeHalf(data, sp, 1, kSaveLink);
            savedAt = sp;
        }

        // The point on the edge from outside vertex o to inside vertex i where it meets the plane
        // (`axis` against `sign` * w): o + (i - o) * |d_o / (d_i - d_o)|, d = x_axis - sign * w.
        // ST's w -1: not one of the triangle's own vertices.
        static void intersect(const __m128 (&o)[3], const __m128 (&i)[3], int axis, __m128 sign, __m128 (&point)[3])
        {
            const __m128 oc = clamp(o[0]), ic = clamp(i[0]);
            const __m128 dO = _mm_sub_ps(oc, _mm_mul_ps(lane(oc, 3), sign));
            const __m128 dI = _mm_sub_ps(ic, _mm_mul_ps(lane(ic, 3), sign));
            const __m128 q = divide(lane(dO, axis), _mm_sub_ps(lane(dI, axis), lane(dO, axis)));
            const __m128 t = _mm_andnot_ps(_mm_castsi128_ps(_mm_set1_epi32(static_cast<int>(0x80000000u))), _mm_add_ps(_mm_setzero_ps(), q));
            for (int k = 0; k < 3; ++k)
            {
                const __m128 from = clamp(o[k]);
                point[k] = _mm_add_ps(_mm_mul_ps(_mm_sub_ps(clamp(i[k]), from), t), from);
            }
            point[2] = _mm_castsi128_ps(_mm_insert_epi32(_mm_castps_si128(point[2]), -1, 3));
        }

        // 0x0790: the triangle (v-2, v-1, v) - clipping position, colour as floats, ST with its
        // index in w - clipped plane by plane between the two buffers, then drawn.
        void clipTriangle(uint32_t v, uint32_t out, __m128 p2, __m128 p1, __m128 p0)
        {
            const __m128 positions[3] = {p2, p1, p0};
            __m128 colours[3];
            for (uint32_t k = 0; k < 3u; ++k)
            {
                colours[k] = itof0(loadQ(data, out - 5u + 3u * k));
                const __m128 st = _mm_castsi128_ps(_mm_insert_epi32(_mm_castps_si128(loadQ(data, h + 1u + 3u * (v - 2u + k))), static_cast<int>(2u - k), 3));
                storeQ(data, kBufferA + 3u * k, positions[k]);
                storeQ(data, kBufferA + 3u * k + 1u, colours[k]);
                storeQ(data, kBufferA + 3u * k + 2u, st);
                if (k == 0u)
                {
                    storeQ(data, kBufferA + 9u, positions[0]);
                    storeQ(data, kBufferA + 10u, colours[0]);
                    storeQ(data, kBufferA + 11u, st);
                }
            }
            __m128 lastColour = colours[1]; // vf26: its w goes into the polygon's XYZF2
            struct Plane
            {
                int axis;
                float sign;
                uint32_t previousBit, nextBit;
            };
            static constexpr Plane kPlanes[6] = {{2, -1.0f, 1u << 11, 1u << 5}, {2, 1.0f, 1u << 10, 1u << 4},
                                                 {0, -1.0f, 1u << 7, 1u << 1},  {0, 1.0f, 1u << 6, 1u << 0},
                                                 {1, -1.0f, 1u << 9, 1u << 3},  {1, 1.0f, 1u << 8, 1u << 2}};
            uint32_t source = kBufferA, target = kBufferB, count = 3u;
            for (const Plane &plane : kPlanes)
            {
                const __m128 sign = _mm_set1_ps(plane.sign);
                __m128 current[3] = {loadQ(data, source), loadQ(data, source + 1u), loadQ(data, source + 2u)};
                uint32_t read = source + 3u, write = target, emitted = 0u;
                for (uint32_t e = 0; e < count; ++e)
                {
                    const __m128 previous[3] = {_mm_add_ps(clamp(current[0]), _mm_setzero_ps()),
                                                _mm_add_ps(clamp(current[1]), _mm_setzero_ps()), current[2]};
                    const __m128 next[3] = {loadQ(data, read), loadQ(data, read + 1u), loadQ(data, read + 2u)};
                    read += 3u;
                    const uint32_t flags = (clipFlags(previous[0]) << 6) | clipFlags(next[0]);
                    __m128 point[3];
                    if (flags & plane.previousBit)
                    {
                        if (!(flags & plane.nextBit))
                        {
                            intersect(previous, next, plane.axis, sign, point);
                            for (uint32_t k = 0; k < 3u; ++k)
                                storeQ(data, write + k, point[k]);
                            write += 3u;
                            ++emitted;
                            lastColour = point[1];
                        }
                    }
                    else
                    {
                        for (uint32_t k = 0; k < 3u; ++k)
                            storeQ(data, write + k, previous[k]);
                        write += 3u;
                        ++emitted;
                        if (flags & plane.nextBit)
                        {
                            intersect(next, previous, plane.axis, sign, point);
                            for (uint32_t k = 0; k < 3u; ++k)
                                storeQ(data, write + k, point[k]);
                            write += 3u;
                            ++emitted;
                            lastColour = point[1];
                        }
                    }
                    for (uint32_t k = 0; k < 3u; ++k)
                        current[k] = next[k];
                }
                const __m128 first[3] = {loadQ(data, target), loadQ(data, target + 1u), loadQ(data, target + 2u)};
                for (uint32_t k = 0; k < 3u; ++k)
                    storeQ(data, write + k, first[k]);
                if (emitted == 0u)
                    return;
                std::swap(source, target);
                count = emitted;
            }
            drawPolygon(v, source, count, lastColour);
        }

        // 0x0f50: the field's register writes (from q21.w), then the polygon as a packet of its
        // own from 0x0480's data block q9: a vertex of the triangle through the field's matrix
        // (q0-q3) as in the strip, a new one through 0x0480's q4-q7 from its clipping position.
        void drawPolygon(uint32_t v, uint32_t source, uint32_t count, __m128 lastColour)
        {
            const uint32_t packet = block0480 + 9u;
            storeHalf(data, packet, 0, 0x8000u);
            uint32_t tags = word(data, 21, 3) & 0xFFFFu;
            if (tags == 0u)
                tags = 1023u;
            Matrix toScreen{};
            for (uint32_t r = 0; r < 4u; ++r)
                toScreen.row[r] = clamp(loadQ(data, block0480 + 4u + r));
            kickAt(run, data, tags);
            storeHalf(data, packet, 0, count + 0x8000u);
            const int32_t w = _mm_extract_epi32(ftoi4(lastColour), 3);
            for (uint32_t i = 0; i < count; ++i)
            {
                const __m128 position = loadQ(data, source + 3u * i);
                storeQ(data, packet + 2u + 3u * i, _mm_castsi128_ps(ftoi(loadQ(data, source + 3u * i + 1u), 1.0f)));
                const __m128 st = loadQ(data, source + 3u * i + 2u);
                const int32_t index = static_cast<int16_t>(word(data, source + 3u * i + 2u, 3) & 0xFFFFu);
                __m128 screen;
                if (index >= 0)
                {
                    Matrix fieldNow{};
                    for (uint32_t r = 0; r < 4u; ++r)
                        fieldNow.row[r] = clamp(loadQ(data, r));
                    const __m128 original = clamp(loadQ(data, h + 3u + 3u * v - 3u * static_cast<uint32_t>(index)));
                    screen = transform(fieldNow, lane(original, 0), lane(original, 1), lane(original, 2));
                }
                else
                    screen = rowTimes(toScreen, clamp(position));
                const __m128 q = reciprocal(lane(screen, 3));
                storeXyz(data, packet + 1u + 3u * i, _mm_mul_ps(clamp(st), q));
                storeQ(data, packet + 3u + 3u * i, _mm_castsi128_ps(_mm_insert_epi32(ftoi4(_mm_mul_ps(screen, q)), w, 3)));
            }
            if (static_cast<int16_t>((word(data, 4, 1) - 2u) & 0xFFFFu) < 0) // q4.y less 2, as a VI
                kickAt(run, data, packet);
            const __m128 rows[4] = {loadQ(data, 0), loadQ(data, 1), loadQ(data, 2), loadQ(data, 3)};
            setField(rows);
        }
    };

    // Swaps 0x1410's and 0x0480's matrices as 0x1410 does between the two runs of 0x0480 (and
    // back after): the field matrix in vf01-vf04, q0-q3 and 0x1410's q0-q3, the clipping one
    // in 0x0480's q0-q3 and 0x1410's q4-q7.
    void swapMatrices(ClipPath &c, uint32_t block1410)
    {
        __m128 other[4], clipping[4], current[4];
        for (uint32_t r = 0; r < 4u; ++r)
            other[r] = loadQ(c.data, block1410 + r);
        for (uint32_t r = 0; r < 4u; ++r)
        {
            storeQ(c.data, block1410 + r, c.field.row[r]);
            storeQ(c.data, r, other[r]);
        }
        for (uint32_t r = 0; r < 4u; ++r)
            clipping[r] = loadQ(c.data, block1410 + 4u + r);
        c.setField(other); // (MAX of a register with itself: its bits)
        for (uint32_t r = 0; r < 4u; ++r)
            current[r] = loadQ(c.data, c.block0480 + r);
        for (uint32_t r = 0; r < 4u; ++r)
        {
            storeQ(c.data, c.block0480 + r, clipping[r]);
            storeQ(c.data, block1410 + 4u + r, current[r]);
        }
    }

    // `index`: the dispatcher's routine index while 0x1410 runs (the table's 0x1410 entry plus
    // one; the entry after it is 0x0480's).
    void clipAndKick(Batch &b, int32_t index, ps2vu1native::Run &run)
    {
        uint8_t *data = b.data;
        const uint32_t h = b.header, n = b.count;
        const uint32_t block1410Bits = word(data, 4u + static_cast<uint32_t>(index), 1) & 0xFFFFu;
        const uint32_t block1410 = block1410Bits & 0x3FFu;
        const uint32_t block0480Bits = word(data, 5u + static_cast<uint32_t>(index), 1) & 0xFFFFu;
        ClipPath c{b, run, data, h, n, block0480Bits & 0x3FFu, block0480Bits, index, b.state.vi[13]};
        const __m128 rows[4] = {_mm_loadu_ps(b.state.vf[1]), _mm_loadu_ps(b.state.vf[2]), _mm_loadu_ps(b.state.vf[3]),
                                _mm_loadu_ps(b.state.vf[4])};
        c.setField(rows);
        const int kept[5] = {17, 18, 26, 27, 31};
        for (int k = 0; k < 5; ++k)
            c.kept[k] = _mm_loadu_ps(b.state.vf[kept[k]]);

        const uint32_t masks0 = word(data, h + 3u * n + 4u, 0), masks1 = word(data, h + 3u * n + 4u, 1);
        for (uint32_t v = 0; v < n; ++v)
            storeHalf(data, h + 3u + 3u * v, 3, v < 16u ? masks0 << v : masks1 << (v - 16u));
        uint32_t sp = static_cast<uint32_t>(b.state.vi[14]) & 0xFFFFu;
        storeHalf(data, sp, 0, 0x00C8u >> 3); // the dispatcher's link
        --sp;
        storeHalf(data, 21, 1, 1u);
        storeHalf(data, 21, 2, 1u);
        for (uint32_t t = 0; t < 3u; ++t)
        {
            storeQ(data, h + 3u * n + 1u + t, loadQ(data, block1410 + 8u + t));
            storeQ(data, h + 6u * n + 5u + t, loadQ(data, block1410 + 12u + t));
        }
        storeHalf(data, 21, 3, block1410Bits + 8u);
        static const bool jointClips = []() { const char *env = std::getenv("TF_MOMENTS_JOINT"); return !env || *env != '0'; }();
        if (tf::port::momentPictures() && jointClips)
        {
            c.joint = true;
            c.planJoint(block1410);
        }
        sp = c.pass(sp, 0x1880u >> 3);
        swapMatrices(c, block1410);
        storeHalf(data, 21, 3, block1410Bits + 12u);
        sp = c.pass(sp, 0x19A0u >> 3);
        swapMatrices(c, block1410);
        storeHalf(data, 21, 1, 0u);
        storeHalf(data, 21, 3, 0u);
        for (uint32_t r = 0; r < 4u; ++r)
            _mm_storeu_ps(b.state.vf[1 + r], c.field.row[r]);
    }

    // Whether 0x1410's clipping path can be run here: 0x0480 the entry after 0x1410, and what
    // it writes - the input and the strips after it, the clipper's buffers, the stack, the two
    // data blocks, q0-q3 and q21 - apart from each other and from what it reads (q4, q1022,
    // q1023), so that the order of its reads and writes cannot matter.
    bool clipPathFits(const Batch &b, int32_t index)
    {
        const uint8_t *data = b.data;
        if ((word(data, 5u + static_cast<uint32_t>(index), 0) & 0x7FFu) != 0x0480u / 8u)
            return false;
        const uint32_t h = b.header, n = b.count;
        if (h + 9u * n + 9u > 1024u)
            return false;
        const uint32_t block1410 = word(data, 4u + static_cast<uint32_t>(index), 1) & 0x3FFu;
        const uint32_t block0480 = word(data, 5u + static_cast<uint32_t>(index), 1) & 0x3FFu;
        const uint32_t sp = static_cast<uint32_t>(b.state.vi[14]) & 0x3FFu;
        const uint32_t ranges[][2] = {{h, 9u * n + 9u}, {ClipPath::kBufferA, 60u}, {(sp - 21u) & 0x3FFu, 22u},
                                      {block1410, 15u}, {block0480, 8u}, {(block0480 + 9u) & 0x3FFu, 28u},
                                      {0u, 5u}, {21u, 1u}, {1022u, 2u}};
        constexpr size_t kRanges = sizeof(ranges) / sizeof(ranges[0]);
        for (size_t i = 0; i < kRanges; ++i)
            for (size_t j = i + 1u; j < kRanges; ++j)
                if (overlaps(ranges[i][0], ranges[i][1], ranges[j][0], ranges[j][1]))
                    return false;
        return true;
    }

    // With moment pictures (docs/RENDERER.md stage 4): a water batch the microcode would clip
    // (q21.x above 0) that does not fit whole (waterWhole) - the river right below the camera as
    // it rises in a jump, part of it behind the eye - drawn by drawWater with its triangles
    // clipped alike in both fields, as 0x1410's are (ClipPath::planJoint). A triangle not inside
    // both fields' planes (x and y kJointReach times as far out: the GPU's scissor cuts at the
    // screen's edge) is left out of the four strips and, unless all of it is outside one plane
    // in a field, clipped once in its own barycentric coordinates against both fields' clipping
    // planes - field A's matrix in 0x0480's data block (q0-q3), field B's in 0x2cc0's (q4-q7), as
    // the microcode's clipping path has them - and drawn as a fan in each copy: the corners as the
    // strip has them, the points between through the field's matrix from the corners' values
    // mixed before the projection (times the point's Q, as the GS interpolates), under the polygon
    // tags of the microcode's clipper (q1022; q1020 for context 2, which its clipping path swaps
    // in and back). The microcode's clipper cut each field at its own planes: pieces the renderer
    // cannot pair, holes in the river at the moments between (the user, at the bridge, jumping
    // while the view turned). Not the microcode's arithmetic: only with moment pictures
    // (TF_MOMENTS_WATER=0 leaves these batches to the microcode).
    void drawWaterJoint(Batch &b, uint32_t waves, uint32_t output, uint32_t block0480, ps2vu1native::Run &run)
    {
        uint8_t *data = b.data;
        const uint32_t n = b.count, h = b.header;
        const uint32_t stride = 3u * n + 4u;
        const uint32_t base[4] = {h + 1u, h + 1u + stride, h + 1u + 2u * stride, h + 1u + 3u * stride};
        // (before the strips are written over them: the matrices, the masks)
        Matrix clipA{}, clipB{}, screens[2]{};
        for (uint32_t r = 0; r < 4u; ++r)
        {
            clipA.row[r] = clamp(loadQ(data, block0480 + r));
            clipB.row[r] = clamp(loadQ(data, output + 4u + r));
            screens[0].row[r] = clamp(_mm_loadu_ps(b.state.vf[1 + r]));
            screens[1].row[r] = clamp(loadQ(data, output + r));
        }
        const uint32_t masks0 = word(data, h + 3u * n + 4u, 0), masks1 = word(data, h + 3u * n + 4u, 1);
        const __m128 tint = clamp(loadQ(data, waves + 21u));
        WaterVertex vertices[32];
        drawWater(b, waves, output, run, vertices, false);

        struct Planned
        {
            uint8_t kind = 0;  // 0 in the strips, 1 left out, 2 a polygon
            uint8_t count = 0; // the polygon's vertices
            float bary[16][3]; // ... as weights of the triangle's corners
        };
        Planned plan[32];
        __m128 inA[32], inB[32];
        uint32_t flagsA[32], flagsB[32];
        for (uint32_t v = 0; v < n; ++v)
        {
            const __m128 x = _mm_set1_ps(vertices[v].position[0]), y = _mm_set1_ps(vertices[v].position[1]),
                         z = _mm_set1_ps(vertices[v].position[2]);
            inA[v] = ClipPath::reachOut(transform(clipA, x, y, z));
            inB[v] = ClipPath::reachOut(transform(clipB, x, y, z));
            flagsA[v] = clipFlags(inA[v]) & 0x3Fu;
            flagsB[v] = clipFlags(inB[v]) & 0x3Fu;
        }
        bool left = false;
        for (uint32_t v = 2u; v < n; ++v)
        {
            if ((static_cast<uint32_t>(kickBits(masks0, masks1, v)) & 0x8000u) != 0u)
                continue; // (not a triangle of the strip)
            if (((flagsA[v - 2u] | flagsA[v - 1u] | flagsA[v]) | (flagsB[v - 2u] | flagsB[v - 1u] | flagsB[v])) == 0u)
                continue; // inside both: the strips draw it
            Planned &t = plan[v];
            t.kind = 1;
            left = true;
            if ((flagsA[v - 2u] & flagsA[v - 1u] & flagsA[v]) != 0u || (flagsB[v - 2u] & flagsB[v - 1u] & flagsB[v]) != 0u)
                continue; // all outside one plane in a field
            float polygon[24][3];
            const uint32_t count = ClipPath::clipBoth(&inA[v - 2u], &inB[v - 2u], polygon);
            if (count < 3u)
                continue;
            t.kind = 2;
            t.count = static_cast<uint8_t>(std::min<uint32_t>(count, 16u));
            std::memcpy(t.bary, polygon, sizeof(float) * 3u * t.count);
        }
        if (!left)
        {
            run.send(run.context, (h - 3u) & 0x3FFu, 4u * stride);
            return;
        }

        // The strips without those triangles (bit 15 of their last vertex's XYZF2 w), sent; the
        // corners and the copies' register writes kept for the fans.
        const auto setKick = [&](uint32_t qword, bool off)
        {
            uint32_t bits = word(data, qword, 3);
            bits = off ? (bits | 0x8000u) : (bits & ~0x8000u);
            std::memcpy(data + (qword & 0x3FFu) * 16u + 12u, &bits, 4u);
        };
        for (uint32_t v = 2u; v < n; ++v)
            if (plan[v].kind != 0u)
                for (uint32_t copy = 0; copy < 4u; ++copy)
                    setKick(base[copy] + 3u * v + 2u, true);
        __m128 strips[4][32][3], writes[4][3];
        for (uint32_t copy = 0; copy < 4u; ++copy)
        {
            for (uint32_t t = 0; t < 3u; ++t)
                writes[copy][t] = loadQ(data, base[copy] - 4u + t);
            for (uint32_t v = 0; v < n; ++v)
                for (uint32_t k = 0; k < 3u; ++k)
                    strips[copy][v][k] = loadQ(data, base[copy] + 3u * v + k);
        }
        const __m128 polygonTags[2] = {loadQ(data, 1022), loadQ(data, 1020)};
        run.send(run.context, (h - 3u) & 0x3FFu, 4u * stride);

        // Each copy's fans, one packet each (in the strips' room: at most 4 + 3 x 16 qwords, the
        // strips at least 4 x 13): its register writes, the polygon tag, the vertices.
        const uint32_t packet = (h - 3u) & 0x3FFu;
        for (uint32_t copy = 0; copy < 4u; ++copy)
        {
            const Matrix &screen = screens[copy & 1u];
            const bool sphere = copy >= 2u; // (context 2: the sphere map, the colour times q21)
            for (uint32_t v = 2u; v < n; ++v)
            {
                const Planned &t = plan[v];
                if (t.kind != 2u)
                    continue;
                for (uint32_t r = 0; r < 3u; ++r)
                    storeQ(data, packet + r, writes[copy][r]);
                storeQ(data, packet + 3u, polygonTags[sphere ? 1 : 0]);
                storeHalf(data, packet + 3u, 0, t.count + 0x8000u); // NLOOP, the packet's end
                const WaterVertex *corner = &vertices[v - 2u];
                for (uint32_t i = 0; i < t.count; ++i)
                {
                    const float (&w)[3] = t.bary[i];
                    const uint32_t at = packet + 4u + 3u * i;
                    const int which = w[0] == 1.0f ? 0 : (w[1] == 1.0f ? 1 : (w[2] == 1.0f ? 2 : -1));
                    if (which >= 0)
                    {
                        for (uint32_t k = 0; k < 3u; ++k)
                            storeQ(data, at + k, strips[copy][v - 2u + static_cast<uint32_t>(which)][k]);
                        setKick(at + 2u, false);
                        continue;
                    }
                    const auto mix = [&](float a, float b2, float c) { return a * w[0] + b2 * w[1] + c * w[2]; };
                    const float px = mix(corner[0].position[0], corner[1].position[0], corner[2].position[0]);
                    const float py = mix(corner[0].position[1], corner[1].position[1], corner[2].position[1]);
                    const float pz = mix(corner[0].position[2], corner[1].position[2], corner[2].position[2]);
                    const __m128 position = transform(screen, _mm_set1_ps(px), _mm_set1_ps(py), _mm_set1_ps(pz));
                    const __m128 q = reciprocal(lane(position, 3));
                    const __m128 st = sphere ? _mm_set_ps(0.0f, 1.0f, mix(corner[0].map[1], corner[1].map[1], corner[2].map[1]),
                                                          mix(corner[0].map[0], corner[1].map[0], corner[2].map[0]))
                                             : _mm_set_ps(0.0f, mix(corner[0].st[2], corner[1].st[2], corner[2].st[2]),
                                                          mix(corner[0].st[1], corner[1].st[1], corner[2].st[1]),
                                                          mix(corner[0].st[0], corner[1].st[0], corner[2].st[0]));
                    const __m128 colour = _mm_add_ps(_mm_add_ps(_mm_mul_ps(itof0(corner[0].colour), _mm_set1_ps(w[0])),
                                                                _mm_mul_ps(itof0(corner[1].colour), _mm_set1_ps(w[1]))),
                                                     _mm_mul_ps(itof0(corner[2].colour), _mm_set1_ps(w[2])));
                    storeQ(data, at, _mm_mul_ps(st, q));
                    storeQ(data, at + 1u, _mm_castsi128_ps(ftoi(sphere ? _mm_mul_ps(colour, tint) : colour, 1.0f)));
                    storeQ(data, at + 2u, _mm_castsi128_ps(_mm_insert_epi32(ftoi4(_mm_mul_ps(position, q)), 0, 3)));
                }
                kickAt(run, data, packet);
            }
        }
    }

    // ---- Sprites ----

    // Batch type 1 (dispatcher 0x170): a vertex is two qwords, colour and position; the
    // position less the header's z in x, y and z, and less (1 + z) / 1024 in w - four vertices
    // at a time as the microcode goes, so past the last one.
    void convertType1(Batch &b)
    {
        const __m128 z = clamp(lane(loadQ(b.data, b.header), 2));
        const __m128 shifted = _mm_add_ps(_mm_setzero_ps(), z); // ADDz.xyw VF10, VF00, VF10z
        const __m128 w = _mm_mul_ps(_mm_add_ps(_mm_set1_ps(1.0f), z), _mm_set1_ps(1.0f / 1024.0f));
        const __m128 bias = _mm_blend_ps(_mm_blend_ps(shifted, z, 0x4), w, 0x8);
        const uint32_t vertices = (b.count + 3u) & ~3u;
        for (uint32_t v = 0; v < vertices; ++v)
        {
            const uint32_t at = b.header + 2u + 2u * v;
            storeQ(b.data, at, _mm_sub_ps(clamp(loadQ(b.data, at)), bias));
        }
    }

    // The MAC S flag of the w lane of MADDx out, m0, x after the ACC's three steps: the sign of
    // the exact sum (ACC.w + m0.w * x, in double precision as the engine works it out).
    TF_INLINE uint32_t signOfLastMadd(__m128 acc, __m128 m0, __m128 x)
    {
        const __m128d sum = _mm_add_sd(_mm_cvtss_sd(_mm_setzero_pd(), lane(acc, 3)),
                                       _mm_mul_sd(_mm_cvtss_sd(_mm_setzero_pd(), lane(m0, 3)), _mm_cvtss_sd(_mm_setzero_pd(), x)));
        return static_cast<uint32_t>(_mm_movemask_pd(sum)) & 1u;
    }

    // Routine 0x22a0 of image bec5738323d4afd8 (by its bytes), alone in the table of a batch of
    // type 1: every vertex a sprite, for each field. The centre is the position through the
    // field's matrix (field A vf01-vf04, field B the data block's q0-q3) times Q = 1 / max(w, 0),
    // x and y from 1000 to 3000, z at most 1.6e7; the corners the centre less q15 * w * Q and
    // plus q18 * w * Q (each at most q10.zw; w the position's own w), their ST q11 and q14 times
    // Q; the second corner not drawn when the field's w is negative (the MAC sign flag of its
    // last step). The first corner's XYZF2 w is 0xFFFF8000. One packet: field A's register
    // writes (data block q4-q6) and sprites after the input, then field B's (q7-q9).
    void drawSprites(Batch &b, uint32_t block, ps2vu1native::Run &run)
    {
        uint8_t *data = b.data;
        const uint32_t h = b.header, n = b.count;
        Matrix fields[2] = {};
        for (int r = 0; r < 4; ++r)
        {
            fields[0].row[r] = clamp(_mm_loadu_ps(b.state.vf[1 + r]));
            fields[1].row[r] = clamp(loadQ(data, block + static_cast<uint32_t>(r)));
        }
        const uint32_t base[2] = {h + 2u * n + 5u, h + 7u * n + 9u}; // the sprites, after 4 qwords of tags
        const __m128i tail = _mm_castps_si128(loadQ(data, 1023));
        for (uint32_t t = 0; t < 3u; ++t)
        {
            storeQ(data, base[0] - 4u + t, loadQ(data, block + 4u + t));
            storeQ(data, base[1] - 4u + t, loadQ(data, block + 7u + t));
        }
        storeQ(data, base[0] - 1u, _mm_castsi128_ps(_mm_insert_epi32(tail, static_cast<int>(n), 0)));
        storeQ(data, base[1] - 1u, _mm_castsi128_ps(_mm_insert_epi32(tail, static_cast<int>(b.headerX), 0)));

        const __m128 sizeBefore = clamp(loadQ(data, block + 15u)); // SUB.xy VF19, VF15, VF00: its bits
        const __m128 sizeAfter = clamp(loadQ(data, block + 18u));
        const __m128 limit = _mm_add_ps(_mm_setzero_ps(), _mm_shuffle_ps(clamp(loadQ(data, block + 10u)), clamp(loadQ(data, block + 10u)), 0xFE)); // (q10.z, q10.w)
        const __m128 st0 = clamp(loadQ(data, block + 11u)), st1 = clamp(loadQ(data, block + 14u));
        const __m128 low = _mm_set1_ps(1000.0f), high = _mm_set_ps(0.0f, 1.6e7f, 3000.0f, 3000.0f);
        const __m128 one = _mm_set1_ps(1.0f);
        for (uint32_t k = 0; k < n; ++k)
        {
            const __m128 colour = loadQ(data, h + 1u + 2u * k);
            const __m128 position = clamp(loadQ(data, h + 2u + 2u * k));
            const __m128 x = lane(position, 0), y = lane(position, 1), z = lane(position, 2), w = lane(position, 3);
            for (int f = 0; f < 2; ++f)
            {
                const Matrix &m = fields[f];
                __m128 acc = _mm_mul_ps(m.row[3], one);
                acc = _mm_add_ps(acc, _mm_mul_ps(m.row[2], z));
                acc = _mm_add_ps(acc, _mm_mul_ps(m.row[1], y));
                const __m128 screen = _mm_add_ps(acc, _mm_mul_ps(m.row[0], x));
                const uint32_t behind = signOfLastMadd(acc, m.row[0], x);
                const __m128 q = reciprocal(lane(maxBits(screen, _mm_setzero_ps()), 3));
                const __m128 scaled = _mm_mul_ps(screen, q);
                __m128 centre = mini(maxBits(scaled, low), high);
                centre = _mm_blend_ps(centre, mini(scaled, high), 0x4); // z: at most 1.6e7, no floor
                const __m128 before = mini(_mm_mul_ps(_mm_mul_ps(sizeBefore, w), q), limit);
                const __m128 after = mini(_mm_mul_ps(_mm_mul_ps(sizeAfter, w), q), limit);
                const __m128 zero = _mm_setzero_ps();
                const __m128 corner0 = _mm_sub_ps(centre, _mm_blend_ps(before, zero, 0xC)); // z less +0
                const __m128 corner1 = _mm_add_ps(centre, _mm_blend_ps(after, zero, 0xC));  // z plus +0
                const uint32_t at = base[f] + 5u * k;
                storeXyz(data, at, _mm_mul_ps(st0, q));
                storeQ(data, at + 1u, colour);
                storeQ(data, at + 2u, _mm_castsi128_ps(_mm_insert_epi32(ftoi4(corner0), static_cast<int>(0xFFFF8000u), 3)));
                storeXyz(data, at + 3u, _mm_mul_ps(st1, q));
                storeQ(data, at + 4u, _mm_castsi128_ps(_mm_insert_epi32(ftoi4(corner1), static_cast<int16_t>(0x7FF0u + 16u * behind), 3)));
            }
        }
        kickAt(run, data, h + 2u * n + 1u);
    }

    // ---- A projected texture ----

    // ---- A flat strip ----

    // Routine 0x3cc0 of image 1a707099ecafadff (by its bytes), alone in the routine table of a
    // batch the dispatcher does not convert (type 0): a mesh drawn flat in one colour - in the
    // captures into a 128 x 128 scissor (the register writes: SCISSOR_1, SCANMSK, RGBAQ
    // 0x80808080), a shadow or mask texture by the look of it. A vertex is one qword, its x, y
    // and z; each through the data block's q0-q3 (x, y and z only: no w, no divide) to 12.4
    // fixed point, in place, its drawing-kick bit (XYZF2's w) from the mask after the vertices
    // (h + n + 4: 16 vertices to its x, the next ones to its y). The tags before the vertices:
    // the data block's q4 (a GIF tag of register writes) and q5-q7 at h - 4 to h - 1, the
    // header's y, z and w from q1023 (its x, the count with the end-of-packet bit, as it is);
    // one packet from h - 4. The data block's q0-q3 and the mask are read after the tags are
    // written, as the microcode reads them.
    bool drawFlatStrip(Batch &b, uint32_t block, ps2vu1native::Run &run)
    {
        uint8_t *data = b.data;
        const uint32_t h = b.header, n = b.count;
        if (n == 0u || h < 4u || h + n + 4u > 1023u) // (no vertices: the microcode's loop goes round memory)
            return false;
        std::memset(data + h * 16u + 4u, 0, 4u); // the dispatcher's ISWR.y VI00 (the type)
        const __m128 tail = loadQ(data, 1023);
        const __m128 registers[4] = {loadQ(data, block + 7u), loadQ(data, block + 6u), loadQ(data, block + 5u),
                                     loadQ(data, block + 4u)};
        storeQ(data, h, _mm_blend_ps(tail, loadQ(data, h), 0x1)); // SQ.yzw
        for (uint32_t k = 0; k < 4u; ++k)
            storeQ(data, h - 1u - k, registers[k]);
        const uint32_t masks0 = word(data, h + n + 4u, 0), masks1 = word(data, h + n + 4u, 1);
        Matrix m{};
        for (int r = 0; r < 4; ++r)
            m.row[r] = clamp(loadQ(data, block + static_cast<uint32_t>(r)));
        for (uint32_t v = 0; v < n; ++v)
        {
            const __m128 position = clamp(loadQ(data, h + 1u + v));
            const __m128 xyz = transform(m, lane(position, 0), lane(position, 1), lane(position, 2));
            storeQ(data, h + 1u + v, _mm_castsi128_ps(_mm_insert_epi32(ftoi4(xyz), kickBits(masks0, masks1, v), 3)));
        }
        kickAt(run, data, h - 4u);
        return true;
    }

    // ---- Sprites of one field ----

    // Routine 0x2cf0 of image 8673b3b4fc9a8b0c (by its bytes; image 2011a15b85aa5a94 has the same
    // code there), alone in the routine table of a batch the dispatcher does not convert (its
    // header 0x8000 | n in every word, so its "type" too): a sprite per vertex, field A only, the
    // texture whole (ST the data block's, not times Q). A vertex is two qwords, its colour and its
    // position (w the sprite's size). The centre is the position through field A's matrix
    // (vf01-vf04) times Q = 1 / max(w, 0), x and y from 1000 to 3000, z at most 1.6e7; the corners
    // the centre less q5 * size * Q and plus q8 * size * Q (x and y, each at most q0.z and q0.w),
    // their ST the data block's q1 and q4 as they are. The second corner is not drawn when the
    // field's w is negative (the MAC sign flag of its transform's last step); the first's XYZF2
    // w is 0xFFFF8000. One packet after the input (h + 1 + 2n): the vertex tag (the header's x,
    // q1023's y, z, w), then five qwords a sprite - ST, RGBAQ, XYZF2, ST, XYZF2. With q0.x zero
    // (the MAC zero flag of SUB.x) the routine takes another path, from 0x3168: the microcode's.
    bool drawFieldSprites(Batch &b, uint32_t block, ps2vu1native::Run &run)
    {
        uint8_t *data = b.data;
        const uint32_t h = b.header, n = b.count;
        const uint32_t base = h + 2u * n + 2u; // the first sprite, after the vertex tag
        if ((word(data, block, 0) & 0x7F800000u) == 0u || n == 0u || base + 5u * n > 1024u)
            return false;
        std::memset(data + h * 16u + 4u, 0, 4u); // the dispatcher's ISWR.y VI00 (the type)
        Matrix field{};
        for (int r = 0; r < 4; ++r)
            field.row[r] = clamp(_mm_loadu_ps(b.state.vf[1 + r]));
        // Everything from the data block and q1023 is read before the first store, as the
        // microcode reads it (the packet may cover them).
        const __m128 q0 = clamp(loadQ(data, block));
        const __m128 limit = _mm_add_ps(_mm_setzero_ps(), _mm_shuffle_ps(q0, q0, 0xFE)); // ADDz.x, ADDw.y: (q0.z, q0.w)
        const __m128 st0 = loadQ(data, block + 1u), st1 = loadQ(data, block + 4u);
        const __m128 size0 = clamp(loadQ(data, block + 5u)), size1 = clamp(loadQ(data, block + 8u));
        const __m128i tail = _mm_castps_si128(loadQ(data, 1023));
        storeQ(data, base - 1u, _mm_castsi128_ps(_mm_insert_epi32(tail, static_cast<int>(b.headerX), 0)));
        const __m128 low = _mm_set1_ps(1000.0f), high = _mm_set_ps(0.0f, 1.6e7f, 3000.0f, 3000.0f);
        const __m128 one = _mm_set1_ps(1.0f), zero = _mm_setzero_ps();
        for (uint32_t k = 0; k < n; ++k)
        {
            const __m128 colour = loadQ(data, h + 1u + 2u * k);
            const __m128 position = clamp(loadQ(data, h + 2u + 2u * k));
            const __m128 x = lane(position, 0), y = lane(position, 1), z = lane(position, 2), size = lane(position, 3);
            __m128 acc = _mm_mul_ps(field.row[3], one);
            acc = _mm_add_ps(acc, _mm_mul_ps(field.row[2], z));
            acc = _mm_add_ps(acc, _mm_mul_ps(field.row[1], y));
            const __m128 screen = _mm_add_ps(acc, _mm_mul_ps(field.row[0], x));
            const uint32_t behind = signOfLastMadd(acc, field.row[0], x);
            const __m128 q = reciprocal(lane(maxBits(screen, zero), 3));
            const __m128 scaled = _mm_mul_ps(screen, q);
            const __m128 centre = _mm_blend_ps(mini(maxBits(scaled, low), high), mini(scaled, high), 0x4); // z: no floor
            const __m128 before = mini(_mm_mul_ps(_mm_mul_ps(size0, size), q), limit);
            const __m128 after = mini(_mm_mul_ps(_mm_mul_ps(size1, size), q), limit);
            const __m128 corner0 = _mm_sub_ps(centre, _mm_blend_ps(before, zero, 0xC)); // z less +0
            const __m128 corner1 = _mm_add_ps(centre, _mm_blend_ps(after, zero, 0xC));  // z plus +0
            const uint32_t at = base + 5u * k;
            storeQ(data, at, st0);
            storeQ(data, at + 1u, colour);
            storeQ(data, at + 2u, _mm_castsi128_ps(_mm_insert_epi32(ftoi4(corner0), static_cast<int>(0xFFFF8000u), 3)));
            storeQ(data, at + 3u, st1);
            storeQ(data, at + 4u, _mm_castsi128_ps(_mm_insert_epi32(ftoi4(corner1), static_cast<int16_t>(0x7FF0u + 16u * behind), 3)));
        }
        kickAt(run, data, base - 1u);
        return true;
    }

    // Routine 0x1df0 of images 2011a15b85aa5a94 and 25cef3e84459f32b (by its bytes), first in
    // the table of a batch of type 3 (then image 2011's 0x22a0 draws, which 0x1df0 skips when
    // nothing is to be drawn): a texture projected onto the mesh. Per vertex its ST x and y become
    // the position through the data block's q0-q3 and its ST w and position w the projector's
    // coordinates (q4-q7's x and y); its colour q8, the alpha q8.w * (bit 7 of the vertex's
    // alpha). A triangle whose three vertices are all on the outside of one side of the box
    // (x and y of q0-q3's from 0 to 1, z above -1; CLIP of 2 (t - 0.5) against 1) gets its
    // drawing-kick bit. Nothing left to draw: 0x1df0 sends an empty packet (from q1023) and ends
    // the table. (drawProjected, below.)
    // How many triangles 0x1df0 leaves to draw (the count it tests: n - 2 - culled).
    int32_t projectionLeft(const Batch &b, const Matrix &mesh)
    {
        const __m128 offset = _mm_set_ps(0.0f, 0.0f, -0.5f, -0.5f), scale = _mm_set_ps(0.0f, 1.0f, 2.0f, 2.0f);
        uint32_t judgements = 0, culled = 0; // (FCSET 0 first)
        for (uint32_t v = 0; v < b.count; ++v)
        {
            const __m128 position = clamp(loadQ(b.data, b.vertex(v) + 2u));
            const __m128 inMesh = rowTimes(mesh, _mm_blend_ps(position, _mm_set1_ps(1.0f), 0x8));
            const __m128 box = _mm_blend_ps(_mm_mul_ps(_mm_add_ps(inMesh, offset), scale), _mm_set1_ps(1.0f), 0x8);
            judgements = ((judgements << 6) | clipFlags(box)) & 0x3FFFFu;
            // FCOR on +x, -x, +y, -y and -z (not +z) of the last three judgements
            culled += (judgements & (judgements >> 6) & (judgements >> 12) & 0x2Fu) != 0u ? 1u : 0u;
        }
        return static_cast<int16_t>((b.count - 2u - culled) & 0xFFFFu);
    }

    void projectionMatrices(const Batch &b, uint32_t block, Matrix &mesh, Matrix &projector)
    {
        for (int r = 0; r < 4; ++r)
        {
            mesh.row[r] = clamp(loadQ(b.data, block + static_cast<uint32_t>(r)));
            projector.row[r] = clamp(loadQ(b.data, block + 4u + static_cast<uint32_t>(r)));
        }
    }

    bool projectsNothing(const Batch &b, uint32_t block)
    {
        Matrix mesh{}, projector{};
        projectionMatrices(b, block, mesh, projector);
        return projectionLeft(b, mesh) == 0;
    }

    // ---- A projected texture, drawn ----

    // Routine 0x1df0 all but its empty packet: per vertex ST.xy the
    // position through q0-q3, ST.w and position.w the projector's y and x (q4-q7), the colour q8 with
    // the alpha q8.w * (bit 7 of the vertex's); a triangle all outside one side of the box gets its
    // drawing-kick bit - 16 vertices to a mask word, the vertex of the last turn in bit 0 and the word
    // then shifted to put the first in bit 15: the second word is shifted at least once (0x2148: the
    // loop tests after its shift in the delay slot), so with 32 vertices vertex 16's bit falls off
    // and the others land a place higher (at 32 - v; 31 - v for fewer: the trace gives masks.y
    // 0xfffe for 32 vertices where 31 - v would give 0xffff - an agent's reading). Returns what
    // 0x1df0 tests, n - 2 - culled (0: nothing to draw), and what it leaves in vf17 and vf18 (0x0480's
    // stack saves them): the last vertex's ST x, y, w and colour; vf17.z the colour's z, or the box's z
    // when the last group of 16 is one vertex (n 1 or 17: no turn of the loop).
    int32_t projectVertices(Batch &b, uint32_t block, __m128 &vf17, __m128 &vf18)
    {
        uint8_t *data = b.data;
        const uint32_t n = b.count;
        Matrix mesh{}, projector{};
        projectionMatrices(b, block, mesh, projector);
        const __m128 colour = clamp(loadQ(data, block + 8u));
        const __m128 offset = _mm_set_ps(0.0f, 0.0f, -0.5f, -0.5f), scale = _mm_set_ps(0.0f, 1.0f, 2.0f, 2.0f);
        const __m128 one = _mm_set1_ps(1.0f);
        uint32_t judgements = 0, culled = 0; // (FCSET 0 first)
        uint32_t bits[2] = {};
        for (uint32_t v = 0; v < n; ++v)
        {
            const uint32_t at = b.vertex(v);
            const __m128 position = clamp(loadQ(data, at + 2u));
            const __m128 withOne = _mm_blend_ps(position, one, 0x8);
            const __m128 inMesh = rowTimes(mesh, withOne);
            const __m128 projected = rowTimes(projector, withOne);
            const __m128 box = _mm_blend_ps(_mm_mul_ps(_mm_add_ps(inMesh, offset), scale), one, 0x8);
            judgements = ((judgements << 6) | clipFlags(box)) & 0x3FFFFu;
            // FCOR on +x, -x, +y, -y and -z (not +z) of the last three judgements
            const uint32_t out = (judgements & (judgements >> 6) & (judgements >> 12) & 0x2Fu) != 0u ? 1u : 0u;
            culled += out;
            const uint32_t bit = v < 16u ? 15u - v : (n == 32u ? 32u : 31u) - v;
            bits[v < 16u ? 0 : 1] |= out << bit;
            const uint32_t alphaBit = word(data, at + 1u, 3) & 128u;
            const __m128 st = loadQ(data, at);
            const __m128 stOut = _mm_blend_ps(_mm_blend_ps(st, inMesh, 0x3), lane(projected, 1), 0x8); // SQ.xyw
            storeQ(data, at, stOut);
            const __m128 alpha = _mm_mul_ps(_mm_set1_ps(static_cast<float>(alphaBit)), colour);
            const __m128 rgba = _mm_castsi128_ps(ftoi(_mm_blend_ps(loadQ(data, block + 8u), alpha, 0x8), 1.0f));
            storeQ(data, at + 1u, rgba);
            const int32_t projectedX = _mm_cvtsi128_si32(_mm_castps_si128(projected));
            std::memcpy(data + ((at + 2u) & 0x3FFu) * 16u + 12u, &projectedX, 4u); // SQ.w
            if (v + 1u == n)
            {
                vf18 = rgba;
                vf17 = _mm_blend_ps(stOut, v % 16u == 0u ? box : rgba, 0x4);
            }
        }
        const uint32_t masks = b.header + 3u * n + 4u;
        storeHalf(data, masks, 0, (word(data, masks, 0) | bits[0]) & 0xFFFFu);
        if (n > 16u)
            storeHalf(data, masks, 1, (word(data, masks, 1) | bits[1]) & 0xFFFFu);
        return static_cast<int16_t>((n - 2u - culled) & 0xFFFFu);
    }

    // Image 2011's 0x22a0 drawing (q21.x not above 0): 0x1410's transformAndKick but for field B's
    // ST - field A (vf01-vf04, over the input) has ST * Q, ST being 0x1df0's coordinates in the mesh's
    // box (ST.z the vertex's own, ST.w left as 0x1df0 wrote it: the projector's y); field B (the data
    // block's q0-q3, after the input) has the projector's coordinates (0 + position.w, 0 + ST.w, 1)
    // times its Q. Both copies with the drawing-kick bits; the tags as 0x1410's (q8-q10, q12-q14, the
    // vertex tags from q1023: field A's x the count, field B's the header's x); one packet. 0x22a0 then
    // skips 0x0480 and ends the table (vi13 = vi12: unchanged with three routines).
    void drawProjectedStrip(Batch &b, uint32_t block, ps2vu1native::Run &run)
    {
        uint8_t *data = b.data;
        const uint32_t header = b.header, count = b.count;
        Matrix fieldA{}, fieldB{};
        for (int r = 0; r < 4; ++r)
        {
            fieldA.row[r] = clamp(_mm_loadu_ps(b.state.vf[1 + r]));
            fieldB.row[r] = clamp(loadQ(data, block + static_cast<uint32_t>(r)));
        }
        const __m128 tail = loadQ(data, 1023); // the vertex tags' y, z, w
        const uint32_t second = header + 3u * count + 4u; // the masks, then field B's vertex tag
        const uint32_t masks0 = word(data, second, 0), masks1 = word(data, second, 1);
        const __m128 one = _mm_set1_ps(1.0f), zero = _mm_setzero_ps();
        for (uint32_t v = 0; v < count; ++v)
        {
            const uint32_t at = b.vertex(v);
            const __m128 st = clamp(loadQ(data, at));
            const __m128 colour = loadQ(data, at + 1u);
            const __m128 position = clamp(loadQ(data, at + 2u));
            const int32_t adc = kickBits(masks0, masks1, v);
            const __m128 x = lane(position, 0), y = lane(position, 1), z = lane(position, 2);
            // the projector's coordinates: ADDw.x 0 + position.w, ADDw.y 0 + ST.w; z 1 (MR32.z of VF00)
            const __m128 projector = _mm_blend_ps(_mm_add_ps(zero, _mm_unpacklo_ps(lane(position, 3), lane(st, 3))), one, 0x4);
            const __m128 clipA = transform(fieldA, x, y, z);
            const __m128 qa = reciprocal(lane(clipA, 3));
            storeXyz(data, at, _mm_mul_ps(st, qa));
            storeQ(data, at + 2u, _mm_castsi128_ps(_mm_insert_epi32(ftoi4(_mm_mul_ps(clamp(clipA), qa)), adc, 3)));
            const __m128 clipB = transform(fieldB, x, y, z);
            const __m128 qb = reciprocal(lane(clipB, 3));
            const uint32_t out = second + 1u + 3u * v;
            storeXyz(data, out, _mm_mul_ps(projector, qb));
            storeQ(data, out + 1u, colour);
            storeQ(data, out + 2u, _mm_castsi128_ps(_mm_insert_epi32(ftoi4(_mm_mul_ps(clamp(clipB), qb)), adc, 3)));
        }
        for (uint32_t t = 0; t < 3u; ++t)
        {
            storeQ(data, header - 3u + t, loadQ(data, block + 8u + t));
            storeQ(data, header + 3u * count + 1u + t, loadQ(data, block + 12u + t));
        }
        const __m128i tailBits = _mm_castps_si128(tail);
        storeQ(data, header, _mm_castsi128_ps(_mm_insert_epi32(tailBits, static_cast<int>(count), 0)));
        storeQ(data, second, _mm_castsi128_ps(_mm_insert_epi32(tailBits, static_cast<int>(b.headerX), 0)));
        kickAt(run, data, header - 3u); // (6n + 8 qwords with the tags the game gives it)
    }

    // Image 2011's 0x22a0 clipping (q21.x above 0): 0x1410's clipping path (clipAndKick) with three
    // differences - before the masks go into the positions' w, each w (the projector's x from 0x1df0)
    // is kept after field B's tags (h + 6n + 9 + v, lane w); between the two runs of 0x0480 every ST.xy
    // becomes the projector's coordinates (0 + that kept x, 0 + ST.w), so field B's strip and polygons
    // carry them (the loop's last turn is after its test: with one vertex it rewrites a second); and
    // 0x0480 is called from 0x2730 and 0x28f8 (the links on its stack). vf17 and vf18, which 0x0480's
    // stack saves, are 0x1df0's (projectVertices). `index`: the dispatcher's routine index while
    // 0x22a0 runs (2; the entry after it, 0x0480's, is 3 - 1).
    void clipProjected(Batch &b, int32_t index, __m128 vf17, __m128 vf18, ps2vu1native::Run &run)
    {
        uint8_t *data = b.data;
        const uint32_t h = b.header, n = b.count;
        const uint32_t blockBits = word(data, 4u + static_cast<uint32_t>(index), 1) & 0xFFFFu; // 0x22a0's data block
        const uint32_t block = blockBits & 0x3FFu;
        const uint32_t block0480Bits = word(data, 5u + static_cast<uint32_t>(index), 1) & 0xFFFFu;
        ClipPath c{b, run, data, h, n, block0480Bits & 0x3FFu, block0480Bits, index, b.state.vi[13]};
        const __m128 rows[4] = {_mm_loadu_ps(b.state.vf[1]), _mm_loadu_ps(b.state.vf[2]), _mm_loadu_ps(b.state.vf[3]),
                                _mm_loadu_ps(b.state.vf[4])};
        c.setField(rows);
        c.kept[0] = vf17;
        c.kept[1] = vf18;
        c.kept[2] = _mm_loadu_ps(b.state.vf[26]);
        c.kept[3] = _mm_loadu_ps(b.state.vf[27]);
        c.kept[4] = _mm_loadu_ps(b.state.vf[31]);

        const uint32_t kept = h + 6u * n + 9u; // the projector's x of vertex v at kept + v, lane w
        const uint32_t masks0 = word(data, h + 3u * n + 4u, 0), masks1 = word(data, h + 3u * n + 4u, 1);
        for (uint32_t v = 0; v < n; ++v)
        {
            const uint32_t positionW = word(data, h + 3u + 3u * v, 3);
            std::memcpy(data + ((kept + v) & 0x3FFu) * 16u + 12u, &positionW, 4u); // LQ.w, SQ.w
            storeHalf(data, h + 3u + 3u * v, 3, v < 16u ? masks0 << v : masks1 << (v - 16u));
        }
        uint32_t sp = static_cast<uint32_t>(b.state.vi[14]) & 0xFFFFu;
        storeHalf(data, sp, 0, 0x00C8u >> 3); // the dispatcher's link
        --sp;
        storeHalf(data, 21, 1, 1u);
        storeHalf(data, 21, 2, 1u);
        for (uint32_t t = 0; t < 3u; ++t)
        {
            storeQ(data, h + 3u * n + 1u + t, loadQ(data, block + 8u + t));
            storeQ(data, h + 6u * n + 5u + t, loadQ(data, block + 12u + t));
        }
        storeHalf(data, 21, 3, blockBits + 8u);
        static const bool jointClips = []() { const char *env = std::getenv("TF_MOMENTS_JOINT"); return !env || *env != '0'; }();
        if (tf::port::momentPictures() && jointClips)
        {
            c.joint = true;
            c.planJoint(block);
        }
        sp = c.pass(sp, 0x2740u >> 3);
        swapMatrices(c, block);
        const uint32_t rewritten = n > 2u ? n : 2u;
        for (uint32_t v = 0; v < rewritten; ++v)
        {
            const uint32_t at = h + 1u + 3u * v;
            const __m128 coordinates = _mm_add_ps(_mm_setzero_ps(), clamp(_mm_unpacklo_ps(lane(loadQ(data, kept + v), 3), lane(loadQ(data, at), 3))));
            float *target = reinterpret_cast<float *>(data + (at & 0x3FFu) * 16u);
            _mm_storeu_ps(target, _mm_blend_ps(coordinates, _mm_loadu_ps(target), 0xC)); // SQ.xy
        }
        storeHalf(data, 21, 3, blockBits + 12u);
        sp = c.pass(sp, 0x2908u >> 3);
        swapMatrices(c, block);
        storeHalf(data, 21, 1, 0u);
        storeHalf(data, 21, 3, 0u);
        for (uint32_t r = 0; r < 4u; ++r)
            _mm_storeu_ps(b.state.vf[1 + r], c.field.row[r]);
    }

    // Routine 0x1df0 then image 2011's 0x22a0 (whose bytes are checked: kProjectedDraw), batch type 3
    // converted: 0x1df0's coordinates, colours and culling; nothing left to draw - its empty packet
    // (q1023) and the end of the table; else 0x22a0 draws (q21.x not above 0) or clips by way of 0x0480.
    void drawProjected(Batch &b, uint32_t projectionBlock, uint32_t drawBlock, ps2vu1native::Run &run)
    {
        __m128 vf17 = _mm_setzero_ps(), vf18 = _mm_setzero_ps();
        if (projectVertices(b, projectionBlock, vf17, vf18) == 0)
        {
            kickAt(run, b.data, 1023u);
            return;
        }
        if (static_cast<int16_t>(word(b.data, 21, 0)) > 0)
            clipProjected(b, 2, vf17, vf18, run);
        else
            drawProjectedStrip(b, drawBlock, run);
    }

    // ---- Programs recompiled whole (tools/vu1_recompile.py) ----
    //
    // Run in program order, a VU1 program does what VU1 does but for the reads its pipeline
    // lets lag behind: Q, P, the clip flags and the MAC/status flags, which the generated code
    // takes from short histories that many producers back (tools/vu1_lags.py measured each
    // site), and a branch's view of a VI written by the pair before it.

    TF_INLINE float fromBits(uint32_t bits)
    {
        float value;
        std::memcpy(&value, &bits, 4u);
        return value;
    }

    TF_INLINE __m128 signMask() { return _mm_castsi128_ps(_mm_set1_epi32(static_cast<int>(0x80000000u))); }

    // SQ with a dest mask (x = 1 in `mask`).
    TF_INLINE void storeMasked(uint8_t *data, uint32_t qword, __m128 value, int mask)
    {
        float *target = reinterpret_cast<float *>(data + (qword & 0x3FFu) * 16u);
        const __m128 old = _mm_loadu_ps(target);
        const __m128 lanes = _mm_castsi128_ps(_mm_set_epi32((mask & 8) ? -1 : 0, (mask & 4) ? -1 : 0, (mask & 2) ? -1 : 0, (mask & 1) ? -1 : 0));
        _mm_storeu_ps(target, _mm_blendv_ps(old, value, lanes));
    }

    TF_INLINE void pushQ(float (&history)[8], uint32_t &count, float value) { history[count++ & 7u] = value; }
    TF_INLINE void pushP(float (&history)[8], uint32_t &count, float value) { history[count++ & 7u] = value; }
    TF_INLINE void pushClip(uint32_t (&history)[8], uint32_t &count, uint32_t value) { history[count++ & 7u] = value; }
    TF_INLINE void pushFlags(uint32_t (&history)[8], uint32_t &count, uint32_t value) { history[count++ & 7u] = value; }

    // ELENG / ERLENG: the length of x, y, z (each squared and the sum rounded in turn), and its
    // reciprocal (or the length itself when 0).
    TF_INLINE float eleng(__m128 value)
    {
        const __m128 c = clamp(value);
        const __m128 squares = _mm_mul_ps(c, c);
        const __m128 sum = _mm_add_ss(_mm_add_ss(squares, lane(squares, 1)), lane(squares, 2));
        return _mm_cvtss_f32(_mm_sqrt_ss(sum));
    }

    TF_INLINE float erleng(__m128 value)
    {
        const float length = eleng(value);
        return length != 0.0f ? _mm_cvtss_f32(_mm_div_ss(_mm_set_ss(1.0f), _mm_set_ss(length))) : length;
    }

    // The MAC flags (low 16 bits) and the status register's Z, S, U, O (bits 16-19) of an FMAC
    // result, from its exact value in double precision as the engine works them out. Kind: 0
    // ADD, 1 SUB, 2 MUL, 3 MADD, 4 MSUB, 5 OPMSUB, 6 OPMULA (w +0 for the last two); the
    // operands as read (clamped).
    uint32_t fmacFlags(int kind, __m128 a, __m128 b, __m128 acc, uint32_t dest)
    {
        alignas(16) float fa[4], fb[4], fc[4];
        _mm_store_ps(fa, a);
        _mm_store_ps(fb, b);
        _mm_store_ps(fc, acc);
        uint32_t mac = 0, status = 0;
        for (int l = 0; l < 4; ++l)
        {
            if (!(dest & (8u >> l)))
                continue;
            const __m128d x = _mm_set_sd(fa[l]), y = _mm_set_sd(fb[l]), z = _mm_set_sd(fc[l]);
            __m128d r;
            switch (kind)
            {
            case 0: r = _mm_add_sd(x, y); break;
            case 1: r = _mm_sub_sd(x, y); break;
            case 2: case 6: r = _mm_mul_sd(x, y); break;
            case 3: r = _mm_add_sd(z, _mm_mul_sd(x, y)); break;
            default: r = _mm_sub_sd(z, _mm_mul_sd(x, y)); break;
            }
            if (kind >= 5 && l == 3)
                r = _mm_setzero_pd();
            const double value = _mm_cvtsd_f64(r), magnitude = std::fabs(value);
            const bool sign = (_mm_movemask_pd(r) & 1) != 0;
            const bool under = magnitude != 0.0 && magnitude < 1.1754943508222875e-38;
            const bool zero = magnitude == 0.0 || under;
            const bool over = magnitude > 3.4028234663852886e38;
            const uint32_t bit = 1u << (3 - l);
            mac |= (zero ? bit : 0u) | (sign ? bit << 4 : 0u) | (under ? bit << 8 : 0u) | (over ? bit << 12 : 0u);
            status |= (zero ? 1u : 0u) | (sign ? 2u : 0u) | (under ? 4u : 0u) | (over ? 8u : 0u);
        }
        return (status << 16) | mac;
    }

#include "vu1_grass47dd.inc"

    // ---- Routines recognized by their code ----

    using PrePass = void (*)(Batch &, uint32_t block);

    // An overlay routine (0x1AC0 on, different code in different images) done here: where it
    // starts, where it ends, and the FNV-1a hash of its bytes in the images that have it.
    struct Overlay
    {
        uint32_t start, end;
        uint64_t hash;
        PrePass run;
        uint32_t minCount = 0u; // (fewer vertices: the microcode's)
        bool clips = true;      // false: the batches that clip are the microcode's (the routine
                                // leaves its own values in registers 0x0480 saves on its stack)
    };

    constexpr Overlay kOverlays[] = {
        {0x2150u, 0x2298u, 0xEB203B69B13A1286ull, &scaleColours},
        {0x21B0u, 0x22A0u, 0x74141DCA3C26CA6Full, &mapCoordinates},
        {0x1AC0u, 0x1D10u, 0xE1BBD9635116BFA8ull, &lightNormals},
        {0x1C20u, 0x1E70u, 0xE1BBD9635116BFA8ull, &lightNormals}, // (the same code, image 6b20c90260be78a4)
        {0x3660u, 0x3818u, 0x429FB896A6A5D839ull, &fadeByPlane},
        {0x13B0u, 0x1408u, 0x460D9E912CA3A637ull, &constantColour},
        {0x21F0u, 0x2800u, 0xDF0443C249D0BCE8ull, &lightWithHighlight, 3u, false},
        {0x1AC0u, 0x1C20u, 0x1FE1C6C429DB90A8ull, &transformTexture},      // (images 6b20, e147, 2011)
        {0x1C20u, 0x1DF0u, 0xDD4A81CC9C0BA94Dull, &scaleAndOffsetColours}, // (images e147, 2011)
    };

    // The lit-mesh pair's bytes (its routines run from 0x1D10 and 0x2800; 0x2800's clipping
    // path, from 0x2BE0, is the microcode's).
    constexpr Overlay kLitMesh[] = {
        {0x1D10u, 0x21F0u, 0xF72677CDF8F26FDCull, nullptr},
        {0x2800u, 0x2BE0u, 0xA7C29B1C9E68F5B0ull, nullptr},
    };

    // The water's pair (image e1472a8c96f62e62; 0x2cc0's clipping path is the microcode's).
    constexpr Overlay kWater[] = {
        {0x1DF0u, 0x2180u, 0xF2D2F41A18B2057Full, nullptr},
        {0x2CC0u, 0x3088u, 0xB86C004B89CF27A2ull, nullptr},
    };

    // The instancing routine's bytes (0x1AC0 to its last pair), and the sprites' (0x22A0).
    constexpr Overlay kInstances = {0x1AC0u, 0x2148u, 0x36D4C8D7756E303Dull, nullptr};
    constexpr Overlay kSprites = {0x22A0u, 0x2950u, 0x843D008B96A24BDCull, nullptr};
    constexpr Overlay kProjection = {0x1DF0u, 0x21B0u, 0x6606DB8EEAA72B0Dull, nullptr};
    // Image 2011's 0x22a0 (a 0x1410 with the projector's coordinates for field B), from 0x22A0 to
    // the end of its clipping path (jr vi15 at 0x2A20 and its delay slot); 0x22A0-0x2640, the
    // drawing part alone, hashes 0x56A3FF28892ED3DA. The same bytes in images 8673b3b4fc9a8b0c
    // (with 0x1df0) and 47dd78e8f5335846 (another core: not taken).
    constexpr Overlay kProjectedDraw = {0x22A0u, 0x2A30u, 0x1C83D0A51E6431BFull, nullptr};
    // The flat strip's bytes (0x3CC0 to the end of its jr's delay slot), and the one-field
    // sprites' (0x2CF0 to the same; its path for q0.x zero, from 0x3168, is not included).
    constexpr Overlay kFlatStrip = {0x3CC0u, 0x3E40u, 0x4B83A7D74253AD29ull, nullptr};
    constexpr Overlay kFieldSprites = {0x2CF0u, 0x3168u, 0x24C0203B7D9FD81Dull, nullptr};

    // What an image has: the shared core, and which of the overlays above at which address.
    struct Image
    {
        bool core = false;
        bool litMesh = false;   // 0x1d10 and 0x2800 as above
        bool water = false;     // 0x1df0 and 0x2cc0 as above
        bool flatStrip = false;    // 0x3cc0 as above (image 1a707099ecafadff)
        bool fieldSprites = false; // 0x2cf0 as above (images 8673b3b4fc9a8b0c, 2011a15b85aa5a94)
        bool instances = false; // 0x1ac0 as above
        bool sprites = false;   // 0x22a0 as above
        bool spritesAt2740 = false; // the same code at 0x2740 (image 6b20c90260be78a4)
        bool projection = false; // 0x1df0 as above
        bool projectedDraw = false; // image 2011's 0x22a0 (kProjectedDraw)
        bool grass = false;      // image 47dd78e8f5335846 (its own core): recompiled whole
        PrePass overlay[0x4000u / 8u] = {};
        uint8_t minCount[0x4000u / 8u] = {};
        bool noClipping[0x4000u / 8u] = {};
    };

    const Image &imageOf(const ps2vu1native::Run &run)
    {
        static uint64_t lastHash = 0u;
        static const Image *last = nullptr;
        if (last && run.image == lastHash)
            return *last;
        static std::unordered_map<uint64_t, std::unique_ptr<Image>> images;
        std::unique_ptr<Image> &slot = images[run.image];
        if (!slot)
        {
            slot = std::make_unique<Image>();
            slot->core = hashBytes(run.code, kCoreBytes) == kCoreHash;
            for (const Overlay &overlay : kOverlays)
                if (hashBytes(run.code + overlay.start, overlay.end - overlay.start) == overlay.hash)
                {
                    slot->overlay[overlay.start / 8u] = overlay.run;
                    slot->minCount[overlay.start / 8u] = static_cast<uint8_t>(overlay.minCount);
                    slot->noClipping[overlay.start / 8u] = !overlay.clips;
                }
            slot->litMesh = true;
            for (const Overlay &overlay : kLitMesh)
                slot->litMesh = slot->litMesh && hashBytes(run.code + overlay.start, overlay.end - overlay.start) == overlay.hash;
            slot->water = true;
            for (const Overlay &overlay : kWater)
                slot->water = slot->water && hashBytes(run.code + overlay.start, overlay.end - overlay.start) == overlay.hash;
            slot->instances = hashBytes(run.code + kInstances.start, kInstances.end - kInstances.start) == kInstances.hash;
            slot->sprites = hashBytes(run.code + kSprites.start, kSprites.end - kSprites.start) == kSprites.hash;
            slot->spritesAt2740 = hashBytes(run.code + 0x2740u, kSprites.end - kSprites.start) == kSprites.hash;
            slot->projection = hashBytes(run.code + kProjection.start, kProjection.end - kProjection.start) == kProjection.hash;
            slot->projectedDraw = hashBytes(run.code + kProjectedDraw.start, kProjectedDraw.end - kProjectedDraw.start) == kProjectedDraw.hash;
            slot->flatStrip = hashBytes(run.code + kFlatStrip.start, kFlatStrip.end - kFlatStrip.start) == kFlatStrip.hash;
            slot->fieldSprites = hashBytes(run.code + kFieldSprites.start, kFieldSprites.end - kFieldSprites.start) == kFieldSprites.hash;
            slot->grass = hashBytes(run.code, 0x4000u) == 0x831A3306F7AFF255ull;
        }
        lastHash = run.image;
        last = slot.get();
        return *last;
    }

    // TF_VU1_NATIVE_NEW=0 (a testing aid, for timings): the routines done natively on 2026-10-06
    // (0x21f0, the water, sprites of type 0x8001, 0x1ac0/0x1c20, the projection drawn, 0x3cc0,
    // 0x2cf0, the set-up programs) left to the microcode, as before.
    const bool kNewNative = []()
    {
        const char *env = std::getenv("TF_VU1_NATIVE_NEW");
        return !(env && *env == '0');
    }();

    // TF_VU1_NATIVE_STATS=1: every 2 s, the batches each way took and their vertices (instances:
    // the vertices times the instances), and the batches left to the microcode by their routines.
    enum Path : uint32_t
    {
        kPathSetUp,
        kPathGrass,
        kPathLitMesh,
        kPathWater,
        kPathInstances,
        kPathSprites,
        kPathProjected,
        kPathFlatStrip,
        kPathFieldSprites,
        kPathMeshes,
        kPathTransformed,
        kPathClipped,
        kPathCount
    };
    const char *const kPathNames[kPathCount] = {"set-up programs", "grass (47dd)", "lit meshes", "water", "instances", "sprites",
                                                "projected", "flat strips", "field sprites", "0x1410 as meshes", "0x1410 as packets",
                                                "clipped (0x0480)"};
    struct NativeStats
    {
        uint32_t path = kPathCount, vertices = 0u; // the run's: set by the way that took it
        uint64_t runs[kPathCount] = {}, verts[kPathCount] = {};
        std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> left; // the microcode's: runs, vertices
        std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
    } g_stats;

    TF_INLINE void taken(uint32_t path, uint32_t vertices)
    {
        g_stats.path = path;
        g_stats.vertices = vertices;
    }

    bool runNative(ps2vu1native::Run &run)
    {
        VU1State &state = *run.state;
        if (state.branchPending || state.ebit || state.haltAfterDelaySlot)
            return false;
        // (TODO 27) The warp tunnel's own program (a load's screen): left out while the native renderer draws it.
        if (state.pc == 0x0000u && nr::warp::vu1Run(run))
            return true;
        // (TODO 185 b) A game batch started on the warp program while a tunnel shows: left out, nothing run.
        if (state.pc != 0x0000u && nr::warp::strayRun(run))
            return true;
        // The shared core's two set-up programs, some 60,000 runs a second: MSCAL 0 (ILW.x vi13,
        // 4(vi00) - how many routines the table has - with the E bit; IADDIU vi14, vi00, 1020 - the
        // dispatcher's stack) and MSCAL 0x450 (LQ vf01-vf04 from q0-q3: field A's screen transform,
        // the E bit on the last; a NOP pair after it). VI loads sign-extended, as the engine keeps them.
        if (state.pc == 0x0000u || state.pc == 0x0450u)
        {
            if (!kNewNative || !imageOf(run).core)
                return false;
            taken(kPathSetUp, 0u);
            if (state.pc == 0x0000u)
            {
                state.vi[13] = static_cast<int16_t>(word(run.data, 4, 0));
                state.vi[14] = 1020;
                state.pc = 0x0010u;
            }
            else
            {
                for (int r = 0; r < 4; ++r)
                    std::memcpy(state.vf[1 + r], run.data + 16 * r, 16u);
                state.pc = 0x0478u;
            }
            return true;
        }
        if (state.pc != 0x00F8u && state.pc != 0x0010u)
            return false;
        const Image &image = imageOf(run);
        // Image 47dd78e8f5335846 (grass, 0x1380): the whole program, recompiled, for routine
        // tables of 0x1380 only.
        if (image.grass)
        {
            const int32_t routines = state.vi[13];
            if (routines < 1 || routines > 6)
                return false;
            for (int32_t k = 0; k < routines; ++k)
                if ((word(run.data, 5u + static_cast<uint32_t>(k), 0) & 0x7FFu) != 0x1380u / 8u)
                    return false;
            taken(kPathGrass, 0u);
            return runGrass47dd(state, run.data, run);
        }
        if (!image.core)
            return false;
        uint8_t *data = run.data;
        const uint32_t header = state.itop & 0x3FFu;
        const uint32_t headerX = word(data, header, 0);
        Batch b{state, data, header, headerX, headerX & 0x7FFFu};
        const int32_t routines = state.vi[13];
        const uint32_t type = word(data, header, 1) & 0xFFFFu;
        // Lit meshes: type 4, the table 0x1d10, 0x2800 (drawing: q21.x not above 0), then the
        // routine 0x2800 skips.
        if (type == 4u && image.litMesh && routines == 3 && (word(data, 5, 0) & 0x7FFu) == 0x1D10u / 8u &&
            (word(data, 6, 0) & 0x7FFu) == 0x2800u / 8u && static_cast<int16_t>(word(data, 21, 0)) <= 0 && b.count != 0u &&
            b.count <= 32u)
        {
            taken(kPathLitMesh, b.count);
            litMesh(b, word(data, 5, 1) & 0x3FFu, word(data, 6, 1) & 0x3FFu, run);
            state.pc = 0x00F8u;
            return true;
        }
        // Water: type 3, the table 0x1df0, 0x2cc0 (drawing: q21.x not above 0; with moment pictures
        // a clipping batch too, whole when it fits, waterWhole, else clipped alike in both fields,
        // drawWaterJoint - 0x0480 the third routine), the four copies below the tags' sources
        // (q1021, q1023).
        static const bool momentsWater = []() { const char *env = std::getenv("TF_MOMENTS_WATER"); return !env || *env != '0'; }();
        if (kNewNative && type == 3u && image.water && routines == 3 && (word(data, 5, 0) & 0x7FFu) == 0x1DF0u / 8u &&
            (word(data, 6, 0) & 0x7FFu) == 0x2CC0u / 8u && b.count != 0u && b.count <= 32u && header + 4u * (3u * b.count + 4u) <= 1020u &&
            (static_cast<int16_t>(word(data, 21, 0)) <= 0 ||
             (momentsWater && tf::port::momentPictures() && (word(data, 7, 0) & 0x7FFu) == 0x0480u / 8u)))
        {
            const uint32_t waves = word(data, 5, 1) & 0x3FFu, output = word(data, 6, 1) & 0x3FFu;
            const bool joint = static_cast<int16_t>(word(data, 21, 0)) > 0 && !waterWhole(b, waves, output);
            std::memset(data + header * 16u + 4u, 0, 4u); // the dispatcher's ISWR.y VI00 (the type)
            taken(kPathWater, b.count);
            convertType3(b);
            if (joint)
                drawWaterJoint(b, waves, output, word(data, 7, 1) & 0x3FFu, run);
            else
                drawWater(b, waves, output, run);
            state.pc = 0x00F8u;
            return true;
        }
        // Instances: type 3, the table 0x1ac0 alone; at most 64 instances (the microcode draws
        // one for a count of 0 or less), its two buffers clear of the data block and of q1023.
        if (type == 3u && image.instances && routines == 1 && (word(data, 5, 0) & 0x7FFu) == 0x1AC0u / 8u &&
            b.count != 0u && b.count <= 32u)
        {
            const uint32_t block = word(data, 5, 1) & 0x3FFu;
            const int32_t count = static_cast<int16_t>(word(data, block + 12u, 0));
            const uint32_t instances = count > 0 ? static_cast<uint32_t>(count) : 1u;
            const uint32_t buffers = header + 3u * b.count + 1u, buffersSize = 4u * (3u * b.count + 4u);
            if (instances > 64u || buffers + buffersSize > 1023u || overlaps(buffers, buffersSize, block, 13u + 5u * instances))
                return false;
            taken(kPathInstances, b.count * instances);
            convertType3(b);
            std::memset(data + header * 16u + 4u, 0, 4u); // the dispatcher's ISWR.y VI00 (the type)
            drawInstances(b, block, instances, run);
            state.pc = 0x00F8u;
            return true;
        }
        // Sprites: type 1 (converted) or 0x8001 (no conversion: the vertices as they are), the
        // table 0x22a0 alone - or 0x2740, the same code, in image 6b20c90260be78a4 (sprites with
        // a size: the routine's path for q15.xy both 0 is the microcode's), the packet clear of
        // the data block and of q1023.
        const uint32_t spritesAt = image.sprites ? 0x22A0u : (image.spritesAt2740 && kNewNative ? 0x2740u : 0u);
        if ((type == 1u || (type == 0x8001u && kNewNative)) && spritesAt != 0u && routines == 1 && (word(data, 5, 0) & 0x7FFu) == spritesAt / 8u &&
            b.count != 0u && b.count <= 32u)
        {
            const uint32_t block = word(data, 5, 1) & 0x3FFu;
            const bool sizeless = (word(data, block + 15u, 0) & 0x7F800000u) == 0u && (word(data, block + 15u, 1) & 0x7F800000u) == 0u;
            const uint32_t packet = header + 2u * b.count + 1u, packetSize = 10u * b.count + 8u;
            if (sizeless || packet + packetSize > 1023u || overlaps(packet, packetSize, block, 19u))
                return false;
            std::memset(data + header * 16u + 4u, 0, 4u); // the dispatcher's ISWR.y VI00 (the type)
            taken(kPathSprites, b.count);
            if (type == 1u)
                convertType1(b);
            drawSprites(b, block, run);
            state.pc = 0x00F8u;
            return true;
        }
        // A projected texture: type 3, 0x1df0 first in the table (whether anything is left to draw is
        // worked out from the positions, which the conversion does not touch, before anything is
        // written). Nothing left: 0x1df0's empty packet. Something left: image 2011's 0x22a0 (the
        // table 1df0, 22a0, 0480 - its end, so vi13 stays) draws, or with q21.x above 0 clips by way
        // of 0x0480 (clipPathFits, with 0x22a0's data block where 0x1410's is), the input clear of
        // q0-q21 so that q21.x and the table read now are what 0x22a0 reads.
        if (type == 3u && image.projection && routines >= 1 && (word(data, 5, 0) & 0x7FFu) == 0x1DF0u / 8u &&
            b.count != 0u && b.count <= 32u && header + 3u * b.count + 5u <= 1023u)
        {
            const uint32_t block = word(data, 5, 1) & 0x3FFu;
            if (overlaps(header, 3u * b.count + 5u, block, 9u))
                return false;
            if (!projectsNothing(b, block))
            {
                if (!kNewNative || !image.projectedDraw || routines != 3 || (word(data, 6, 0) & 0x7FFu) != 0x22A0u / 8u || header < 25u)
                    return false;
                const uint32_t drawBlock = word(data, 6, 1) & 0x3FFu;
                if (static_cast<int16_t>(word(data, 21, 0)) > 0)
                {
                    if (!clipPathFits(b, 2))
                        return false;
                }
                else
                {
                    const uint32_t packet = header - 3u, packetSize = 6u * b.count + 8u;
                    if (packet + packetSize > 1023u || overlaps(packet, packetSize, drawBlock, 15u))
                        return false;
                }
            }
            std::memset(data + header * 16u + 4u, 0, 4u); // the dispatcher's ISWR.y VI00 (the type)
            taken(kPathProjected, b.count);
            convertVertices(b, 1u);
            drawProjected(b, block, word(data, 6, 1) & 0x3FFu, run);
            state.pc = 0x00F8u;
            return true;
        }
        // A flat strip (0x3cc0) or sprites of one field (0x2cf0): the routine alone in the table, a
        // type the dispatcher does not convert (0x2cf0's header is 0x8000 | n in every word).
        if (kNewNative && routines == 1 && (type < 1u || type > 4u))
        {
            const uint32_t address = (word(data, 5, 0) & 0x7FFu) * 8u, block = word(data, 5, 1) & 0x3FFu;
            if ((image.flatStrip && address == 0x3CC0u) || (image.fieldSprites && address == 0x2CF0u))
            {
                taken(address == 0x3CC0u ? kPathFlatStrip : kPathFieldSprites, b.count);
                if (!(address == 0x3CC0u ? drawFlatStrip(b, block, run) : drawFieldSprites(b, block, run)))
                    return false;
                state.pc = 0x00F8u;
                return true;
            }
        }
        // The routine table (vi13 entries since MSCAL 0): known routines, then 0x1410 (drawing,
        // or clipping by way of 0x0480, the entry after it). Batch type 3 or 4 (converted first),
        // or one the dispatcher has no conversion for (not 1 to 4: 0x8004, for one).
        if (routines < 2 || routines > 6 || type == 1u || type == 2u || b.count == 0u || b.count > 32u)
            return false;
        PrePass prePasses[4] = {};
        uint32_t blocks[6] = {};
        bool clippingAllowed = true;
        for (int32_t k = 0; k < routines - 1; ++k)
        {
            const uint32_t address = (word(data, 5u + static_cast<uint32_t>(k), 0) & 0x7FFu) * 8u;
            blocks[k] = word(data, 5u + static_cast<uint32_t>(k), 1) & 0x3FFu;
            if (k == routines - 2)
            {
                if (address != 0x1410u)
                    return false;
            }
            else if (!(prePasses[k] = image.overlay[address / 8u]) || b.count < image.minCount[address / 8u])
                return false;
            else if (!kNewNative && (prePasses[k] == &lightWithHighlight || prePasses[k] == &transformTexture ||
                                     prePasses[k] == &scaleAndOffsetColours))
                return false;
            else if (image.noClipping[address / 8u])
                clippingAllowed = false;
        }
        const bool clipping = static_cast<int16_t>(word(data, 21, 0)) > 0; // 0x1410 runs 0x0480
        if (clipping && (!clippingAllowed || !clipPathFits(b, routines - 1)))
            return false;

        std::memset(data + header * 16u + 4u, 0, 4u); // the dispatcher's ISWR.y VI00 (the type)
        if (type == 3u || type == 4u)
            convertVertices(b, type == 3u ? 1u : 2u);
        for (int32_t k = 0; k < routines - 2; ++k)
            prePasses[k](b, blocks[k]);
        taken(clipping ? kPathClipped : run.sendBytes ? kPathMeshes : kPathTransformed, b.count);
        if (clipping)
            clipAndKick(b, routines - 1, run);
        else if (run.sendBytes)
            sendTransformMesh(b, blocks[routines - 2], run);
        else
            transformAndKick(b, blocks[routines - 2], run);
        state.pc = 0x00F8u;
        return true;
    }

    // runNative counted (TF_VU1_NATIVE_STATS=1). A run left to the microcode is keyed by its image
    // (the hash's low 16 bits), entry pc, batch type, routine count and first routine.
    bool runNativeCounted(ps2vu1native::Run &run)
    {
        NativeStats &s = g_stats;
        const VU1State &state = *run.state;
        const uint32_t pc = state.pc;
        const uint32_t header = state.itop & 0x3FFu;
        const uint32_t type = word(run.data, header, 1) & 0xFFFFu;
        const uint32_t count = word(run.data, header, 0) & 0x7FFFu;
        const uint32_t routines = static_cast<uint32_t>(state.vi[13]) & 0xFu;
        const uint32_t first = (word(run.data, 5, 0) & 0x7FFu) * 8u;
        s.path = kPathCount;
        s.vertices = 0u;
        const bool done = runNative(run);
        if (done)
        {
            if (s.path < kPathCount)
            {
                ++s.runs[s.path];
                s.verts[s.path] += s.vertices;
            }
        }
        else
        {
            const uint64_t key = (static_cast<uint64_t>(run.image & 0xFFFFu) << 48) | (static_cast<uint64_t>(pc & 0x3FFFu) << 34) |
                                 (static_cast<uint64_t>(type) << 18) | (static_cast<uint64_t>(routines) << 14) | (first & 0x3FFFu);
            auto &entry = s.left[key];
            ++entry.first;
            entry.second += (pc == 0x00F8u || pc == 0x0010u) ? count : 0u;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - s.last >= std::chrono::seconds(2))
        {
            s.last = now;
            std::fprintf(stderr, "[vu1:paths] 2 s:");
            for (uint32_t p = 0; p < kPathCount; ++p)
                if (s.runs[p] != 0u)
                    std::fprintf(stderr, " %s %llu (%llu vertices);", kPathNames[p], static_cast<unsigned long long>(s.runs[p]),
                                 static_cast<unsigned long long>(s.verts[p]));
            std::fprintf(stderr, "\n");
            std::vector<std::pair<uint64_t, std::pair<uint64_t, uint64_t>>> left(s.left.begin(), s.left.end());
            std::sort(left.begin(), left.end(), [](const auto &a, const auto &b) { return a.second.second != b.second.second ? a.second.second > b.second.second : a.second.first > b.second.first; });
            for (size_t i = 0; i < left.size() && i < 10u; ++i)
            {
                const uint64_t k = left[i].first;
                std::fprintf(stderr, "[vu1:paths]   microcode: image ..%04llx pc %04llx type %04llx routines %llu first %04llx: %llu runs, %llu vertices\n",
                             static_cast<unsigned long long>(k >> 48), static_cast<unsigned long long>((k >> 34) & 0x3FFFu),
                             static_cast<unsigned long long>((k >> 18) & 0xFFFFu), static_cast<unsigned long long>((k >> 14) & 0xFu),
                             static_cast<unsigned long long>(k & 0x3FFFu), static_cast<unsigned long long>(left[i].second.first),
                             static_cast<unsigned long long>(left[i].second.second));
            }
            std::fill(std::begin(s.runs), std::end(s.runs), 0ull);
            std::fill(std::begin(s.verts), std::end(s.verts), 0ull);
            s.left.clear();
        }
        return done;
    }

    // ---- N2 (docs/TODO.md): VU1's drawing work left out while the native renderer draws ----
    //
    // On by default with the native renderer (TF_RENDERER=native / TF_NATIVE_ONLY=1; TF_NATIVE_NOVU1=0 turns it
    // off, =1 forces it): while the emulated GS draws no
    // primitive (ps2_host_display::emulatedDrawing() false: the native renderer draws the frame from the
    // display list, never from VU1's output), a batch run of the dispatcher - an MSCNT resuming at 0x00f8
    // (or 0x0010) in an image whose dispatcher is known (the shared core, or the grass image's own) - is
    // not run: no vertices worked out, no packets for the GS. What such a run leaves that anything later
    // reads is kept: the end pc (0x00f8, where the next MSCNT resumes) and the header's type cleared, as
    // the dispatcher does. The set-up programs (MSCAL 0, MSCAL 0x450 and the grass image's) run as
    // before; VIF1's unpacks, its interrupts and the DMA go on unchanged (none of them waits on a run's
    // work: the runtime charges a native run one cycle). A frame of 2D alone (the emulated drawing back
    // on) runs everything as before.
    //
    // TF_NATIVE_NOVU1=check runs everything as before and, beside it, works out the VU1 world in which
    // those runs were skipped (from the same moments: every run, every unpack between them): the data
    // memory words and the registers in which the two differ; a run that is not skipped is then run
    // on the microcode in both worlds and its packets compared - a difference means a skipped run's
    // work was read later; an access of the EE to a word that differs is reported likewise. It also
    // reads the packets the skipped runs would send and counts the GS registers they write (anything
    // but drawing - a GS transfer, SIGNAL / FINISH / LABEL - would be an event the skip loses). Slow
    // (every run's data memory compared): for checking, not for playing.
    namespace novu1
    {
        enum class Mode : uint8_t
        {
            Off,
            On,
            Check,
        };

        Mode mode()
        {
            static const Mode selected = []()
            {
                const char *env = std::getenv("TF_NATIVE_NOVU1");
                if (!env || !*env)
                {
                    // Default (N2, 2026-10-29): on with the native renderer - the settings' TF_RENDERER=native
                    // (read here: native_render's install sets TF_NATIVE_ONLY after this runs) or TF_NATIVE_ONLY
                    // set for a test. The emulated renderer runs VU1 as before. TF_NATIVE_NOVU1=0 opts out.
                    // Safe either way: runOn skips only while the emulated GS draws nothing.
                    const char *renderer = std::getenv("TF_RENDERER");
                    const char *only = std::getenv("TF_NATIVE_ONLY");
                    const bool native = (renderer && std::strcmp(renderer, "native") == 0) || (only && *only && *only != '0');
                    return native ? Mode::On : Mode::Off;
                }
                if (*env == '0')
                    return Mode::Off;
                if (std::strcmp(env, "check") == 0)
                    return Mode::Check;
                return Mode::On;
            }();
            return selected;
        }

        ps2vu1native::Hook g_real = &runNative; // (runNativeCounted with TF_VU1_NATIVE_STATS)

        constexpr uint32_t kDataBytes = 0x4000u, kWords = kDataBytes / 4u;

        // (emurest step 2) A batch of a particle pipeline (vlxPPP_*: smoke, sparks, trails - its own microcode image,
        // started by MSCAL at the image's address, its batches MSCNTs resuming 16 bytes in): the native renderer runs the
        // pipeline itself on its list tracker's VU1 memory (nr_particles.cpp) and draws its packets, so VU1's own run of
        // it draws nothing anyone sees while the native renderer draws alone. Left out as the dispatchers' batches are,
        // the run's end pc kept (where it started: the next MSCNT resumes there). The set-up run (the MSCAL) runs as
        // before. TF_NATIVE_PARTICLE_SKIP=0: every particle batch on the microcode, as before; checked with
        // TF_NATIVE_NOVU1=check like the rest.
        bool particleBatch(const ps2vu1native::Run &run)
        {
            static const bool on = []() { const char *e = std::getenv("TF_NATIVE_PARTICLE_SKIP"); return !(e && *e == '0'); }();
            const uint32_t pc = run.state->pc;
            return on && pc >= 0x10u && pc < 0x4000u && run.code && nr::particles::isParticleProgram(run.code, pc - 0x10u);
        }

        // A batch run of a known dispatcher, which the skip may leave out.
        bool skippable(const ps2vu1native::Run &run)
        {
            const VU1State &state = *run.state;
            if (run.dataSize != kDataBytes || state.branchPending || state.ebit || state.haltAfterDelaySlot)
                return false;
            if (state.pc != 0x00F8u && state.pc != 0x0010u)
                return particleBatch(run);
            const Image &image = imageOf(run);
            return image.core || image.grass;
        }

        // What a skipped run leaves: the end pc, and the type cleared (ISWR.y VI00 of the core's
        // dispatcher, for every type). A particle batch: its pc as it was (its program resumes there).
        void skipEffects(VU1State &state, uint8_t *data, const ps2vu1native::Run &run)
        {
            if (state.pc != 0x00F8u && state.pc != 0x0010u)
                return;
            if (imageOf(run).core)
                std::memset(data + (state.itop & 0x3FFu) * 16u + 4u, 0, 4u);
            state.pc = 0x00F8u;
        }

        // Counted for the stats line (TF_VU1_NATIVE_STATS) and the first engage/disengage lines.
        struct Counts
        {
            uint64_t skipped = 0, skippedCore = 0, skippedGrass = 0, skippedParticles = 0, notSkipped = 0;
            std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
        } g_counts;

        bool runOn(ps2vu1native::Run &run)
        {
            static bool skipping = false;
            static int told = 6;
            const bool skipNow = !ps2_host_display::emulatedDrawing();
            if (skipNow != skipping)
            {
                skipping = skipNow;
                if (told > 0)
                {
                    --told;
                    std::fprintf(stderr, "[novu1] VU1's batches %s (the emulated drawing %s)\n", skipNow ? "skipped" : "run again",
                                 skipNow ? "off" : "on");
                }
            }
            if (skipNow && skippable(run) && !nr::outsideFrames())
            {
                skipEffects(*run.state, run.data, run);
                ++g_counts.skipped;
                const uint32_t pc = run.state->pc;
                ++(pc != 0x00F8u && pc != 0x0010u ? g_counts.skippedParticles : imageOf(run).core ? g_counts.skippedCore : g_counts.skippedGrass);
                return true;
            }
            if (skipNow && run.dataSize == kDataBytes && (run.state->pc == 0x00F8u || run.state->pc == 0x0010u))
                ++g_counts.notSkipped;
            static const bool stats = []() { const char *e = std::getenv("TF_VU1_NATIVE_STATS"); return e && *e && *e != '0'; }();
            if (stats)
            {
                const auto now = std::chrono::steady_clock::now();
                if (now - g_counts.last >= std::chrono::seconds(2))
                {
                    std::fprintf(stderr, "[novu1] 2 s: %llu batch runs skipped (%llu core, %llu grass, %llu particle), %llu batch runs on VU1 while skipping\n",
                                 static_cast<unsigned long long>(g_counts.skipped), static_cast<unsigned long long>(g_counts.skippedCore),
                                 static_cast<unsigned long long>(g_counts.skippedGrass), static_cast<unsigned long long>(g_counts.skippedParticles),
                                 static_cast<unsigned long long>(g_counts.notSkipped));
                    g_counts = Counts{};
                }
            }
            return g_real(run);
        }

        // ---- TF_NATIVE_NOVU1=check ----

        struct RegDiff
        {
            uint32_t vf = 0, vi = 0; // a bit a register
            bool acc = false, q = false, p = false, i = false, r = false, mac = false, clip = false, status = false;
            bool any() const { return vf || vi || acc || q || p || i || r || mac || clip || status; }
        };

        RegDiff compareRegisters(const VU1State &a, const VU1State &b)
        {
            RegDiff d;
            for (uint32_t k = 0; k < 32u; ++k)
                if (std::memcmp(a.vf[k], b.vf[k], 16u) != 0)
                    d.vf |= 1u << k;
            for (uint32_t k = 0; k < 16u; ++k)
                if (a.vi[k] != b.vi[k])
                    d.vi |= 1u << k;
            d.acc = std::memcmp(a.acc, b.acc, 16u) != 0;
            d.q = std::memcmp(&a.q, &b.q, 4u) != 0;
            d.p = std::memcmp(&a.p, &b.p, 4u) != 0;
            d.i = std::memcmp(&a.i, &b.i, 4u) != 0;
            d.r = a.r != b.r;
            d.mac = a.mac != b.mac;
            d.clip = a.clip != b.clip;
            d.status = a.status != b.status;
            return d;
        }

        // `to` given `from`'s values in the registers `d` names.
        void copyRegisters(VU1State &to, const VU1State &from, const RegDiff &d)
        {
            for (uint32_t k = 0; k < 32u; ++k)
                if (d.vf & (1u << k))
                    std::memcpy(to.vf[k], from.vf[k], 16u);
            for (uint32_t k = 0; k < 16u; ++k)
                if (d.vi & (1u << k))
                    to.vi[k] = from.vi[k];
            if (d.acc)
                std::memcpy(to.acc, from.acc, 16u);
            if (d.q)
                to.q = from.q;
            if (d.p)
                to.p = from.p;
            if (d.i)
                to.i = from.i;
            if (d.r)
                to.r = from.r;
            if (d.mac)
                to.mac = from.mac;
            if (d.clip)
                to.clip = from.clip;
            if (d.status)
                to.status = from.status;
        }

        std::string describe(const RegDiff &d)
        {
            std::string s;
            char part[24];
            for (uint32_t k = 0; k < 32u; ++k)
                if (d.vf & (1u << k))
                {
                    std::snprintf(part, sizeof(part), " vf%02u", k);
                    s += part;
                }
            for (uint32_t k = 0; k < 16u; ++k)
                if (d.vi & (1u << k))
                {
                    std::snprintf(part, sizeof(part), " vi%02u", k);
                    s += part;
                }
            const std::pair<bool, const char *> others[] = {{d.acc, " acc"}, {d.q, " q"},       {d.p, " p"},       {d.i, " i"},
                                                            {d.r, " r"},     {d.mac, " mac"},   {d.clip, " clip"}, {d.status, " status"}};
            for (const auto &o : others)
                if (o.first)
                    s += o.second;
            return s.empty() ? std::string(" none") : s;
        }

        enum class Pending : uint8_t
        {
            None,
            Same,      // the two worlds alike, the run the same in both
            Skipped,   // skipped in the other world: shadowAfter is its result
            Simulated, // run in both on the microcode: shadowAfter / simReal their results
        };

        struct Check
        {
            std::mutex mutex;
            // The other world: the words of data memory and the registers where it differs.
            std::vector<uint8_t> divergent = std::vector<uint8_t>(kWords, 0u);
            std::vector<uint32_t> shadowWord = std::vector<uint32_t>(kWords, 0u); // the other world's value
            std::vector<uint32_t> realWord = std::vector<uint32_t>(kWords, 0u);   // this one's, when compared
            uint32_t divergentCount = 0;
            RegDiff regDiff;
            VU1State shadowRegs{};
            // The run in progress.
            Pending pending = Pending::None;
            std::vector<uint8_t> shadowAfter = std::vector<uint8_t>(kDataBytes, 0u), simReal = std::vector<uint8_t>(kDataBytes, 0u);
            VU1State shadowAfterState{}, simRealState{};
            uint32_t runPc = 0, runType = 0, runRoutines = 0, runFirst = 0;
            uint64_t runImage = 0;
            // Findings.
            uint64_t runs = 0, skipped = 0, simulated = 0, same = 0, packetDiffers = 0, simDiffers = 0, eeAccesses = 0,
                     eeDivergent = 0, eeStores = 0, audited = 0, imagePackets = 0;
            std::map<uint32_t, uint64_t> registersWritten; // A+D / REGLIST / PACKED destinations of skipped runs' packets
            std::map<std::string, uint64_t> differingKinds;
            int reports = 0;
            const uint8_t *dataMemory = nullptr; // (VU1 data memory: the EE's accesses are compared with it)
            std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
            uint64_t maxDivergent = 0;
        };

        Check &check()
        {
            static Check c;
            return c;
        }

        // The words of the other world's data memory: this one's, those that differ put back - unless
        // written since (an unpack, the EE: both worlds see the write).
        void shadowData(const Check &c, const uint8_t *real, uint8_t *out)
        {
            std::memcpy(out, real, kDataBytes);
            if (c.divergentCount != 0u)
                for (uint32_t w = 0; w < kWords; ++w)
                    if (c.divergent[w] && std::memcmp(real + 4u * w, &c.realWord[w], 4u) == 0)
                        std::memcpy(out + 4u * w, &c.shadowWord[w], 4u);
        }

        // The GS registers a packet stream (one packet after another) writes: A+D destinations,
        // PACKED descriptors other than A+D as 0x100 + descriptor, REGLIST as 0x200 + descriptor,
        // IMAGE data (a transfer) as 0x300.
        void auditPackets(Check &c, const std::vector<uint8_t> &p)
        {
            size_t at = 0;
            while (at + 16u <= p.size())
            {
                uint64_t lo = 0, hi = 0;
                std::memcpy(&lo, p.data() + at, 8u);
                std::memcpy(&hi, p.data() + at + 8u, 8u);
                at += 16u;
                const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu);
                const uint32_t flg = static_cast<uint32_t>((lo >> 58) & 3u);
                uint32_t nreg = static_cast<uint32_t>((lo >> 60) & 0xFu);
                if (nreg == 0u)
                    nreg = 16u;
                if ((lo >> 46) & 1u)
                    ++c.registersWritten[0x000u]; // PRE: PRIM from the tag
                if (flg == 0u)
                {
                    for (uint32_t l = 0; l < nloop && at + 16u <= p.size(); ++l)
                        for (uint32_t r = 0; r < nreg && at + 16u <= p.size(); ++r, at += 16u)
                        {
                            const uint32_t desc = static_cast<uint32_t>((hi >> (4u * r)) & 0xFu);
                            if (desc == 0xEu)
                                ++c.registersWritten[p[at + 8u]];
                            else
                                ++c.registersWritten[0x100u + desc];
                        }
                }
                else if (flg == 1u)
                {
                    for (uint32_t r = 0; r < nreg; ++r)
                        c.registersWritten[0x200u + static_cast<uint32_t>((hi >> (4u * r)) & 0xFu)] += nloop;
                    at += ((static_cast<size_t>(nloop) * nreg + 1u) / 2u) * 16u;
                }
                else
                {
                    c.registersWritten[0x300u] += nloop;
                    ++c.imagePackets;
                    at += static_cast<size_t>(nloop) * 16u;
                }
            }
        }

        // A finding, printed the first few times for each kind of run (image, entry, first routine).
        bool report(Check &c, const char *what, const std::string &detail)
        {
            char key[96];
            std::snprintf(key, sizeof(key), "%s %016llx %04x %04x %04x", what, static_cast<unsigned long long>(c.runImage), c.runPc,
                          c.runFirst, c.runType);
            if (++c.differingKinds[key] > 6u)
                return false;
            std::fprintf(stderr, "[novu1:check] %s: image %016llx pc %04x type %04x routines %u first %04x; %u words differ before it, registers%s%s\n",
                         what, static_cast<unsigned long long>(c.runImage), c.runPc, c.runType, c.runRoutines, c.runFirst,
                         c.divergentCount, describe(c.regDiff).c_str(), detail.c_str());
            return true;
        }

        // Which of the differences makes a run's packets differ: the registers one by one, the words by
        // halves (a single word that does it alone, if there is one).
        std::string blame(Check &c, ps2vu1native::Run &run, const std::vector<uint8_t> &realPackets)
        {
            const VU1State &state = *run.state;
            std::vector<uint32_t> words; // the differing words as they stand (not written since)
            for (uint32_t w = 0; w < kWords; ++w)
                if (c.divergent[w] && std::memcmp(run.data + 4u * w, &c.realWord[w], 4u) == 0)
                    words.push_back(w);
            const auto differs = [&](const RegDiff &regs, const uint32_t *list, size_t count)
            {
                VU1State s = state;
                copyRegisters(s, c.shadowRegs, regs);
                std::vector<uint8_t> d(run.data, run.data + kDataBytes), packets;
                for (size_t k = 0; k < count; ++k)
                    std::memcpy(d.data() + 4u * list[k], &c.shadowWord[list[k]], 4u);
                run.microcode(run.context, s, d.data(), packets);
                return packets != realPackets;
            };
            std::string out;
            char part[96];
            if (differs(c.regDiff, nullptr, 0))
            {
                out += "; the registers do it:";
                RegDiff one;
                for (uint32_t k = 0; k < 32u; ++k)
                    if (c.regDiff.vf & (1u << k))
                    {
                        one = RegDiff{};
                        one.vf = 1u << k;
                        if (differs(one, nullptr, 0))
                        {
                            std::snprintf(part, sizeof(part), " vf%02u", k);
                            out += part;
                        }
                    }
                for (uint32_t k = 0; k < 16u; ++k)
                    if (c.regDiff.vi & (1u << k))
                    {
                        one = RegDiff{};
                        one.vi = 1u << k;
                        if (differs(one, nullptr, 0))
                        {
                            std::snprintf(part, sizeof(part), " vi%02u (%d here, %d there)", k, state.vi[k], c.shadowRegs.vi[k]);
                            out += part;
                        }
                    }
                const std::pair<bool RegDiff::*, const char *> flags[] = {{&RegDiff::acc, " acc"}, {&RegDiff::q, " q"}, {&RegDiff::p, " p"},
                                                                         {&RegDiff::i, " i"}, {&RegDiff::r, " r"}, {&RegDiff::mac, " mac"},
                                                                         {&RegDiff::clip, " clip"}, {&RegDiff::status, " status"}};
                for (const auto &f : flags)
                    if (c.regDiff.*(f.first))
                    {
                        one = RegDiff{};
                        one.*(f.first) = true;
                        if (differs(one, nullptr, 0))
                            out += f.second;
                    }
            }
            if (!words.empty() && differs(RegDiff{}, words.data(), words.size()))
            {
                size_t lo = 0, hi = words.size();
                while (hi - lo > 1u)
                {
                    const size_t mid = lo + (hi - lo) / 2u;
                    if (differs(RegDiff{}, words.data() + lo, mid - lo))
                        hi = mid;
                    else if (differs(RegDiff{}, words.data() + mid, hi - mid))
                        lo = mid;
                    else
                        break;
                }
                if (hi - lo == 1u)
                {
                    const uint32_t w = words[lo];
                    uint32_t here = 0;
                    std::memcpy(&here, run.data + 4u * w, 4u);
                    std::snprintf(part, sizeof(part), "; the word q%03x.%c does it (%08x here, %08x there)", w / 4u, "xyzw"[w & 3u], here,
                                  c.shadowWord[w]);
                }
                else
                    std::snprintf(part, sizeof(part), "; %zu of the %zu words together (q%03x.. q%03x)", hi - lo, words.size(),
                                  words[lo] / 4u, words[hi - 1u] / 4u);
                out += part;
            }
            return out;
        }

        bool runCheck(ps2vu1native::Run &run)
        {
            Check &c = check();
            std::lock_guard<std::mutex> lock(c.mutex);
            if (run.dataSize != kDataBytes || c.pending != Pending::None) // (VU0, or a program going on)
                return g_real(run);
            VU1State &state = *run.state;
            c.dataMemory = run.data;
            ++c.runs;
            c.runPc = state.pc;
            c.runImage = run.image;
            const uint32_t header = state.itop & 0x3FFu;
            c.runType = word(run.data, header, 1) & 0xFFFFu;
            c.runRoutines = static_cast<uint32_t>(state.vi[13]) & 0xFu;
            c.runFirst = (word(run.data, 5, 0) & 0x7FFu) * 8u;
            const bool skip = !ps2_host_display::emulatedDrawing() && skippable(run);
            if (skip)
            {
                ++c.skipped;
                shadowData(c, run.data, c.shadowAfter.data());
                c.shadowAfterState = state;
                copyRegisters(c.shadowAfterState, c.shadowRegs, c.regDiff);
                skipEffects(c.shadowAfterState, c.shadowAfter.data(), run);
                // The packets the run sends here (every fourth run's, on copies).
                if (run.microcode && (c.skipped & 3u) == 0u)
                {
                    VU1State copyState = state;
                    std::vector<uint8_t> copyData(run.data, run.data + kDataBytes), packets;
                    run.microcode(run.context, copyState, copyData.data(), packets);
                    ++c.audited;
                    auditPackets(c, packets);
                }
                c.pending = Pending::Skipped;
            }
            else if (c.divergentCount == 0u && !c.regDiff.any())
            {
                ++c.same;
                c.pending = Pending::Same;
            }
            else if (run.microcode)
            {
                // Both worlds on the microcode, from copies.
                ++c.simulated;
                VU1State realState = state, otherState = state;
                copyRegisters(otherState, c.shadowRegs, c.regDiff);
                std::memcpy(c.simReal.data(), run.data, kDataBytes);
                shadowData(c, run.data, c.shadowAfter.data());
                std::vector<uint8_t> realPackets, otherPackets;
                run.microcode(run.context, realState, c.simReal.data(), realPackets);
                run.microcode(run.context, otherState, c.shadowAfter.data(), otherPackets);
                c.simRealState = realState;
                c.shadowAfterState = otherState;
                if ((c.simulated & 63u) == 1u)
                {
                    // The copies repeat: the same run on the same copy gives the same packets.
                    VU1State again = state;
                    std::vector<uint8_t> d(run.data, run.data + kDataBytes), packets;
                    run.microcode(run.context, again, d.data(), packets);
                    if (packets != realPackets || std::memcmp(d.data(), c.simReal.data(), kDataBytes) != 0)
                        report(c, "THE COPIES DO NOT REPEAT", "");
                }
                if (realPackets != otherPackets)
                {
                    ++c.packetDiffers;
                    size_t first = 0;
                    while (first < realPackets.size() && first < otherPackets.size() && realPackets[first] == otherPackets[first])
                        ++first;
                    char detail[96];
                    std::snprintf(detail, sizeof(detail), "; packets %zu / %zu bytes, first difference at byte %zu", realPackets.size(),
                                  otherPackets.size(), first);
                    char key[96];
                    std::snprintf(key, sizeof(key), "PACKETS DIFFER %016llx %04x %04x %04x", static_cast<unsigned long long>(c.runImage),
                                  c.runPc, c.runFirst, c.runType);
                    const auto seen = c.differingKinds.find(key);
                    report(c, "PACKETS DIFFER", std::string(detail) + (seen == c.differingKinds.end() || seen->second < 6u ? blame(c, run, realPackets) : std::string()));
                }
                c.pending = Pending::Simulated;
            }
            return g_real(run);
        }

        void afterCheck(const VU1State &state, const uint8_t *data, uint32_t dataSize)
        {
            Check &c = check();
            std::lock_guard<std::mutex> lock(c.mutex);
            if (dataSize != kDataBytes || c.pending == Pending::None)
                return;
            const Pending pending = c.pending;
            c.pending = Pending::None;
            if (pending == Pending::Same)
                return;
            if (pending == Pending::Simulated)
            {
                // The copy's run should be the real one (the native routines are the microcode's to the
                // bit in data memory): if not, the check's other world is not to be trusted there.
                if (std::memcmp(c.simReal.data(), data, kDataBytes) != 0 || c.simRealState.pc != state.pc)
                {
                    ++c.simDiffers;
                    uint32_t first = 0;
                    while (first < kWords && std::memcmp(c.simReal.data() + 4u * first, data + 4u * first, 4u) == 0)
                        ++first;
                    char detail[128];
                    std::snprintf(detail, sizeof(detail), "; end pc %04x on the copy, %04x real; first word unlike q%03x.%c", c.simRealState.pc,
                                  state.pc, first / 4u, "xyzw"[first & 3u]);
                    report(c, "the microcode on copies differs from the real run", detail);
                }
            }
            // The worlds' difference after the run: data words, then registers.
            c.divergentCount = 0u;
            const uint8_t *other = c.shadowAfter.data();
            for (uint32_t q = 0; q < kDataBytes / 16u; ++q)
            {
                if (std::memcmp(data + 16u * q, other + 16u * q, 16u) == 0)
                {
                    std::memset(&c.divergent[4u * q], 0, 4u);
                    continue;
                }
                for (uint32_t l = 0; l < 4u; ++l)
                {
                    const uint32_t w = 4u * q + l;
                    uint32_t mine = 0, theirs = 0;
                    std::memcpy(&mine, data + 4u * w, 4u);
                    std::memcpy(&theirs, other + 4u * w, 4u);
                    c.divergent[w] = mine != theirs;
                    if (mine != theirs)
                    {
                        c.shadowWord[w] = theirs;
                        c.realWord[w] = mine;
                        ++c.divergentCount;
                    }
                }
            }
            c.maxDivergent = std::max<uint64_t>(c.maxDivergent, c.divergentCount);
            const VU1State &otherState = c.shadowAfterState;
            c.regDiff = compareRegisters(state, otherState);
            c.shadowRegs = otherState;
            const auto now = std::chrono::steady_clock::now();
            if (now - c.last >= std::chrono::seconds(2))
            {
                c.last = now;
                std::fprintf(stderr, "[novu1:check] 2 s: %llu runs: %llu skipped, %llu run in both worlds (%llu packet differences, %llu copies unlike the real run), %llu alike; "
                                     "now %u words differ (at most %llu), registers%s; EE accesses %llu (%llu stores; loads of differing words %llu); %llu skipped runs' packets read (%llu with IMAGE data)\n",
                             static_cast<unsigned long long>(c.runs), static_cast<unsigned long long>(c.skipped),
                             static_cast<unsigned long long>(c.simulated), static_cast<unsigned long long>(c.packetDiffers),
                             static_cast<unsigned long long>(c.simDiffers), static_cast<unsigned long long>(c.same), c.divergentCount,
                             static_cast<unsigned long long>(c.maxDivergent), describe(c.regDiff).c_str(),
                             static_cast<unsigned long long>(c.eeAccesses), static_cast<unsigned long long>(c.eeStores),
                             static_cast<unsigned long long>(c.eeDivergent),
                             static_cast<unsigned long long>(c.audited), static_cast<unsigned long long>(c.imagePackets));
                std::string regs;
                for (const auto &r : c.registersWritten)
                {
                    char part[40];
                    std::snprintf(part, sizeof(part), " %s%02x:%llu", r.first >= 0x300u ? "IMAGE" : r.first >= 0x200u ? "list" : r.first >= 0x100u ? "packed" : "",
                                  r.first & 0xFFu, static_cast<unsigned long long>(r.second));
                    regs += part;
                }
                std::fprintf(stderr, "[novu1:check]   GS registers the skipped runs' packets write (sampled):%s\n", regs.c_str());
                c.runs = c.skipped = c.simulated = c.same = c.audited = c.eeAccesses = c.eeStores = 0u;
                c.maxDivergent = 0u;
            }
        }

        // The EE reading or writing VU1 data memory (on the EE's thread, the VIF1 thread caught up). A
        // store reaches both worlds alike (the next run sees it); a load of a word that differs is the
        // EE seeing the skip.
        void eeAccess(uint32_t offset, uint32_t bytes, bool write)
        {
            Check &c = check();
            std::lock_guard<std::mutex> lock(c.mutex);
            ++c.eeAccesses;
            if (write)
            {
                ++c.eeStores;
                return;
            }
            for (uint32_t w = offset / 4u; w < (offset + bytes + 3u) / 4u && w < kWords; ++w)
                if (c.divergent[w] && c.dataMemory && std::memcmp(c.dataMemory + 4u * w, &c.realWord[w], 4u) == 0)
                {
                    ++c.eeDivergent;
                    if (c.reports < 40)
                    {
                        ++c.reports;
                        std::fprintf(stderr, "[novu1:check] EE LOAD of a differing word:q%03x.%c (0x%08x, %u bytes)\n", w / 4u,
                                     "xyzw"[w & 3u], 0x1100C000u + offset, bytes);
                    }
                    break;
                }
        }
    }

    // ---- The emulation census (2026-11-03): what still runs on the emulated VU1 ----
    //
    // TF_VU1_CENSUS=<seconds> (10 if just set): around whatever hook is chosen, every program run offered to it is put
    // under a key - the 64 bytes of code at its start pc (FNV-1a), the pc and, for a dispatcher's batch (pc 0x00f8 /
    // 0x0010), its batch type and first routine - and timed: a run the hook does (skipped by N2, a native routine, the
    // warp tunnel's) until the hook returns, a run left to the microcode until the fast engine's after-run hook. Every
    // <seconds>: by key, the runs and host time of each way, sorted by the microcode's time ("[vu1:census]"); each key's
    // first line names the image hash and its code's first 16 bytes (to find it in the ELF). VU0's runs (4 KB data) are
    // counted apart. A debugging aid: it changes nothing the runs do.
    namespace census
    {
        struct Entry
        {
            uint64_t image = 0u, codeHash = 0u;
            uint32_t pc = 0u, type = 0u, first = 0u;
            uint8_t code[16] = {};
            uint64_t micro = 0u, microNs = 0u, unclosed = 0u, skipped = 0u, skippedNs = 0u, native = 0u, nativeNs = 0u;
            uint64_t totalMicro = 0u, totalMicroNs = 0u;
            bool told = false;
        };
        struct State
        {
            std::unordered_map<uint64_t, Entry> entries;
            Entry *pending = nullptr;
            std::chrono::steady_clock::time_point pendingAt{};
            std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
            uint64_t vu0 = 0u;
            int number = -1; // (which thread: the VIF1 thread's runs, the renderer's particle runs - nr_particles.cpp)
        };
        thread_local State g_census; // (per thread: the fast engine runs on the VIF1 thread and the renderer's)
        std::mutex g_censusPrint;
        std::atomic<int> g_censusThreads{0};

        ps2vu1native::Hook g_inner = nullptr;
        ps2vu1native::AfterHook g_innerAfter = nullptr;

        uint32_t seconds()
        {
            static const uint32_t value = []() -> uint32_t
            {
                const char *env = std::getenv("TF_VU1_CENSUS");
                if (!env || !*env || *env == '0')
                    return 0u;
                const int n = std::atoi(env);
                return n > 1 ? static_cast<uint32_t>(n) : 10u;
            }();
            return value;
        }

        uint64_t fnv(const uint8_t *bytes, uint32_t count)
        {
            uint64_t h = 0xCBF29CE484222325ull;
            for (uint32_t i = 0; i < count; ++i)
                h = (h ^ bytes[i]) * 0x100000001B3ull;
            return h;
        }

        void report(std::chrono::steady_clock::time_point now)
        {
            State &s = g_census;
            const double secs = std::chrono::duration<double>(now - s.last).count();
            std::lock_guard<std::mutex> printLock(g_censusPrint);
            s.last = now;
            std::vector<Entry *> list;
            uint64_t micro = 0u, microNs = 0u, skipped = 0u, skippedNs = 0u, native = 0u, nativeNs = 0u;
            for (auto &kv : s.entries)
            {
                Entry &e = kv.second;
                if (e.micro + e.skipped + e.native == 0u)
                    continue;
                list.push_back(&e);
                micro += e.micro;
                microNs += e.microNs;
                skipped += e.skipped;
                skippedNs += e.skippedNs;
                native += e.native;
                nativeNs += e.nativeNs;
            }
            std::sort(list.begin(), list.end(), [](const Entry *a, const Entry *b)
                      { return a->microNs != b->microNs ? a->microNs > b->microNs : a->micro + a->native + a->skipped > b->micro + b->native + b->skipped; });
            if (s.number < 0)
                s.number = g_censusThreads.fetch_add(1);
            std::fprintf(stderr, "[vu1:census] thread %d tick %llu, %.1f s: microcode %llu runs %.2f ms/s; native %llu runs %.2f ms/s; skipped (N2) %llu runs %.2f ms/s; VU0 %llu runs\n",
                         s.number, static_cast<unsigned long long>(nr::vblankTick()), secs, static_cast<unsigned long long>(micro), microNs / 1.0e6 / secs,
                         static_cast<unsigned long long>(native), nativeNs / 1.0e6 / secs, static_cast<unsigned long long>(skipped),
                         skippedNs / 1.0e6 / secs, static_cast<unsigned long long>(s.vu0));
            for (Entry *e : list)
            {
                if (!e->told)
                {
                    e->told = true;
                    char bytes[40];
                    for (int i = 0; i < 16; ++i)
                        std::snprintf(bytes + 2 * i, 3, "%02x", e->code[i]);
                    std::fprintf(stderr, "[vu1:census]   new key %016llx: image %016llx pc %04x type %04x first %04x code %s\n",
                                 static_cast<unsigned long long>(e->codeHash), static_cast<unsigned long long>(e->image), e->pc, e->type,
                                 e->first, bytes);
                }
                std::fprintf(stderr, "[vu1:census]   %016llx pc %04x type %04x first %04x: microcode %llu (%.3f ms/s, %.2f us a run%s) native %llu (%.3f ms/s) skipped %llu (%.3f ms/s)\n",
                             static_cast<unsigned long long>(e->codeHash), e->pc, e->type, e->first, static_cast<unsigned long long>(e->micro),
                             e->microNs / 1.0e6 / secs, e->micro ? e->microNs / 1.0e3 / static_cast<double>(e->micro) : 0.0,
                             e->unclosed ? ", some cut off" : "", static_cast<unsigned long long>(e->native), e->nativeNs / 1.0e6 / secs,
                             static_cast<unsigned long long>(e->skipped), e->skippedNs / 1.0e6 / secs);
                e->totalMicro += e->micro;
                e->totalMicroNs += e->microNs;
                e->micro = e->microNs = e->unclosed = e->skipped = e->skippedNs = e->native = e->nativeNs = 0u;
            }
            s.vu0 = 0u;
            std::fprintf(stderr, "[vu1:census] GS memory: %s\n", nr::censusLine().c_str());
        }

        bool run(ps2vu1native::Run &run)
        {
            State &s = g_census;
            if (s.pending)
            {
                ++s.pending->unclosed; // (the last run left to the microcode did not end: cut off by the budget)
                s.pending = nullptr;
            }
            if (run.dataSize != 0x4000u)
            {
                ++s.vu0;
                return g_inner(run);
            }
            const VU1State &state = *run.state;
            const uint32_t pc = state.pc & 0x3FF8u;
            const bool batch = pc == 0x00F8u || pc == 0x0010u;
            const uint32_t header = state.itop & 0x3FFu;
            const uint32_t type = batch ? word(run.data, header, 1) & 0xFFFFu : 0u;
            const uint32_t first = batch ? (word(run.data, 5, 0) & 0x7FFu) * 8u : 0u;
            uint8_t code[64] = {};
            for (uint32_t i = 0; i < 64u; ++i)
                code[i] = run.code[(pc + i) & 0x3FFFu];
            const uint64_t codeHash = fnv(code, 64u);
            const uint64_t key = codeHash ^ (static_cast<uint64_t>(type) << 32) ^ (static_cast<uint64_t>(first) << 16) ^ pc;
            Entry &e = s.entries[key];
            if (e.codeHash == 0u)
            {
                e.image = run.image;
                e.codeHash = codeHash;
                e.pc = pc;
                e.type = type;
                e.first = first;
                std::memcpy(e.code, code, 16u);
            }
            const uint64_t skippedBefore = novu1::g_counts.skipped;
            const auto t0 = std::chrono::steady_clock::now();
            const bool done = g_inner(run);
            if (done)
            {
                const uint64_t ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
                if (novu1::g_counts.skipped != skippedBefore)
                {
                    ++e.skipped;
                    e.skippedNs += ns;
                }
                else
                {
                    ++e.native;
                    e.nativeNs += ns;
                }
            }
            else
            {
                s.pending = &e;
                s.pendingAt = t0;
            }
            if (t0 - s.last >= std::chrono::seconds(seconds()))
                report(t0);
            return done;
        }

        void after(const VU1State &state, const uint8_t *data, uint32_t dataSize)
        {
            State &s = g_census;
            if (s.pending)
            {
                ++s.pending->micro;
                s.pending->microNs += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - s.pendingAt).count());
                s.pending = nullptr;
            }
            if (g_innerAfter)
                g_innerAfter(state, data, dataSize);
        }
    }
}

namespace tf::port
{
    void installVu1Native()
    {
        const char *stats = std::getenv("TF_VU1_NATIVE_STATS");
        const ps2vu1native::Hook real = stats && *stats && *stats != '0' ? &runNativeCounted : &runNative;
        ps2vu1native::Hook hook = real;
        ps2vu1native::AfterHook after = nullptr;
        switch (novu1::mode())
        {
        case novu1::Mode::Off:
            break;
        case novu1::Mode::On:
            novu1::g_real = real;
            hook = &novu1::runOn;
            break;
        case novu1::Mode::Check:
            novu1::g_real = real;
            hook = &novu1::runCheck;
            after = &novu1::afterCheck;
            ps2vu1native::setDataObserver(&novu1::eeAccess);
            std::fprintf(stderr, "[novu1:check] checking what skipping VU1's batches would change (TF_NATIVE_NOVU1=check)\n");
            break;
        }
        if (census::seconds() != 0u)
        {
            // (TF_VU1_CENSUS: every run offered to the hook counted and timed, by its code)
            census::g_inner = hook;
            census::g_innerAfter = after;
            hook = &census::run;
            after = &census::after;
            std::fprintf(stderr, "[vu1:census] on: every %u s, the VU1 programs run, by their code (TF_VU1_CENSUS)\n", census::seconds());
        }
        ps2vu1native::setHook(hook);
        if (after)
            ps2vu1native::setAfterHook(after);
    }
}
