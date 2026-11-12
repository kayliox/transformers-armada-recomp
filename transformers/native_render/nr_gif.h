// The display list's GS drawing read as data (nr_gif.cpp): screen-space triangles with the state
// they were drawn with.
#pragma once

#include "nr_types.h"

#include <cmath>
#include <functional>
#include <vector>

namespace nr::gif
{
    // (Debug, TODO 270) Set while the renderer's thread applies a write that does not come from the list's own words (the
    // native device functions' registers, the particle pipelines' packets): TF_NATIVE_TEX0LOG tells them apart.
    extern thread_local bool t_offList;
    // Triangles (three vertices each, GS pixels, Z / 16) drawn with one state.
    struct Primitives
    {
        uint32_t prim = 0;
        uint64_t tex0 = 0, alpha = 0, test = 0, frame = 0, zbuf = 0, xyoffset = 0, scissor = 0, clamp = 0;
        uint64_t tex1 = 0; // TEX1: the texture's filtering and mip choice (LCM, MXL, MMAG, MMIN, L, K)
        uint32_t scanmsk = 0; // SCANMSK: 2 = odd lines not drawn, 3 = even lines not drawn
        uint32_t colclamp = 1; // COLCLAMP: 0 = the blend's result wraps (masked to 8 bits), 1 = clamped
        std::vector<Vertex> vertices;
    };

    struct State
    {
        uint32_t prim = 0, rgba = 0x80808080u, uv = 0;
        // PRIM as written, PRMODECONT's AC and PRMODE: with AC 0 the primitive's attributes (IIP ... FIX, bits 3-10) are
        // PRMODE's, only its type PRIM's - `prim` above is the two put together (the warp tunnel's flash: PRIM 0x16,
        // PRMODE 0x540 - blended, the tunnel seen through it).
        uint32_t primReg = 0, prmode = 0, prmodecont = 1;
        uint64_t st = 0;
        float q = 1.0f;
        uint64_t tex0[2] = {}, alpha[2] = {}, test[2] = {}, frame[2] = {}, zbuf[2] = {}, xyoffset[2] = {}, scissor[2] = {}, clamp[2] = {};
        uint64_t tex1[2] = {};
        uint32_t scanmsk = 0;
        uint32_t colclamp = 1;
        // When TEX0, ALPHA and TEST (contexts 1 and 2) were last written: a count of the register writes so
        // far (TODO 61: a draw's own stretch of the list overriding its geometry set's state block).
        uint64_t writes = 0, tex0At[2] = {}, alphaAt[2] = {}, testAt[2] = {};
        // (Debug, N5.1's check: every register write seen - A+D or REGLIST - told here, when set.)
        void (*onWrite)(uint32_t reg, uint64_t value) = nullptr;
        // The tag being read: its mode, registers, the register next, qwords (REGLIST: register
        // values) still to come.
        uint32_t flg = 0, nreg = 0, reg = 0, left = 0, regsLeft = 0;
        uint64_t regs = 0;
        std::vector<Vertex> queue;
        bool open = false;
        Primitives current;
    };

    using Emit = std::function<void(const Primitives &)>;

    // GIF packets (qwords: tag, data, ...) applied to the state; the triangles come out at each
    // change of state (and at flush).
    void run(State &s, const uint32_t *words, size_t qwords, const Emit &emit);
    void flush(State &s, const Emit &emit);
}
