// The display list's own GS drawing read as data (README.md): the GIF packets the device's 2D calls
// put between the geometry sets (RenderRect, RenderLine, RenderPrimitive, the HUD, the menus: VIF
// DIRECT), their register writes (PACKED, REGLIST, A+D) applied to a GS state of our own, and each
// kicked primitive collected - in screen space, in order - into draws for the GPU side. Nothing is
// rasterised here; the GS's primitive rules (strips, fans, sprites, the drawing-kick bit) only say
// which triangles there are.
#include "nr_gif.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace nr::gif
{
    thread_local bool t_offList = false;
    namespace
    {
        uint64_t lo64(const uint32_t *q) { return static_cast<uint64_t>(q[0]) | static_cast<uint64_t>(q[1]) << 32; }

        float asFloat(uint32_t w)
        {
            float f;
            std::memcpy(&f, &w, 4);
            return f;
        }

        // The state the primitive's triangles are drawn with (a change starts a new draw).
        bool sameState(const State &s, const Primitives &p)
        {
            const uint32_t c = (s.prim >> 9) & 1u;
            return p.prim == (s.prim & 0x7FFu) && p.tex0 == s.tex0[c] && p.alpha == s.alpha[c] && p.test == s.test[c] &&
                   p.frame == s.frame[c] && p.zbuf == s.zbuf[c] && p.xyoffset == s.xyoffset[c] && p.scissor == s.scissor[c] &&
                   p.clamp == s.clamp[c] && p.tex1 == s.tex1[c] && p.scanmsk == s.scanmsk && p.colclamp == s.colclamp;
        }

        void vertex(State &s, uint32_t x, uint32_t y, uint32_t z, bool kick, const Emit &emit)
        {
            Vertex v;
            v.x = static_cast<float>(x & 0xFFFFu) / 16.0f;
            v.y = static_cast<float>(y & 0xFFFFu) / 16.0f;
            // GS Z as it is: the GPU side takes every draw's z (a mesh's z / w, the grass blades') times
            // 16 / 2^32 for its depth, so a 2D draw tests against the 3D at the GS's own values. (It was
            // Z / 16 here, sixteen times nearer the far end than the meshes: the depth fog's quads, at
            // Z 0xFFFF with GEQUAL to fog what lies beyond, passed only Z 0xFFF and nearer to the far end
            // - the middle distance, Deep Amazon's hills and Antarctica's mountains, was never fogged.)
            v.z = static_cast<float>(z);
            v.colour = s.rgba;
            const bool fst = (s.prim & 0x100u) != 0u;
            const uint32_t c = (s.prim >> 9) & 1u;
            if (fst)
            {
                const float tw = static_cast<float>(1u << ((s.tex0[c] >> 26) & 0xFu)), th = static_cast<float>(1u << ((s.tex0[c] >> 30) & 0xFu));
                v.u = static_cast<float>(s.uv & 0x3FFFu) / 16.0f / tw;
                v.v = static_cast<float>((s.uv >> 16) & 0x3FFFu) / 16.0f / th;
                v.size = 1.0f;
            }
            else
            {
                // S, T and Q as they are: the GS interpolates the three across the primitive and
                // divides per pixel (a quad with a Q per corner is a texture seen in perspective -
                // the HUD's tilted gauges), which a per-vertex S / Q would flatten.
                v.u = asFloat(static_cast<uint32_t>(s.st));
                v.v = asFloat(static_cast<uint32_t>(s.st >> 32));
                v.size = s.q != 0.0f ? s.q : 1.0f;
            }
            const uint32_t type = s.prim & 7u;
            auto &queue = s.queue;
            queue.push_back(v);
            auto out = [&](const Vertex &a, const Vertex &b, const Vertex &d) {
                if (!s.open || !sameState(s, s.current))
                {
                    if (s.open && !s.current.vertices.empty())
                        emit(s.current);
                    s.current = Primitives{};
                    s.current.prim = s.prim & 0x7FFu;
                    s.current.tex0 = s.tex0[c];
                    s.current.alpha = s.alpha[c];
                    s.current.test = s.test[c];
                    s.current.frame = s.frame[c];
                    s.current.zbuf = s.zbuf[c];
                    s.current.xyoffset = s.xyoffset[c];
                    s.current.scissor = s.scissor[c];
                    s.current.clamp = s.clamp[c];
                    s.current.tex1 = s.tex1[c];
                    s.current.scanmsk = s.scanmsk;
                    s.current.colclamp = s.colclamp;
                    s.open = true;
                }
                s.current.vertices.push_back(a);
                s.current.vertices.push_back(b);
                s.current.vertices.push_back(d);
            };
            switch (type)
            {
            case 3u: // triangles
                if (queue.size() == 3u)
                {
                    if (kick)
                        out(queue[0], queue[1], queue[2]);
                    queue.clear();
                }
                break;
            case 4u: // strip
                if (queue.size() >= 3u)
                {
                    if (kick)
                        out(queue[queue.size() - 3u], queue[queue.size() - 2u], queue[queue.size() - 1u]);
                    queue.erase(queue.begin());
                }
                break;
            case 5u: // fan
                if (queue.size() >= 3u)
                {
                    if (kick)
                        out(queue[0], queue[queue.size() - 2u], queue[queue.size() - 1u]);
                    queue.erase(queue.begin() + 1);
                }
                break;
            case 6u: // sprite: a rectangle from two corners, the second's colour
                if (queue.size() == 2u)
                {
                    if (kick)
                    {
                        Vertex a = queue[0], b = queue[1];
                        a.colour = b.colour;
                        Vertex ab = a, ba = a;
                        ab.x = b.x, ab.u = b.u; // (top right)
                        ba.y = b.y, ba.v = b.v; // (bottom left)
                        ab.z = ba.z = b.z;
                        a.z = b.z;
                        out(a, ab, ba);
                        out(ba, ab, b);
                    }
                    queue.clear();
                }
                break;
            case 1u: // line
            case 2u: // line strip: a quad one pixel wide
                if (queue.size() == 2u)
                {
                    if (kick)
                    {
                        Vertex a = queue[0], b = queue[1];
                        float dx = b.x - a.x, dy = b.y - a.y;
                        float len = std::sqrt(dx * dx + dy * dy);
                        // (A line under a GS pixel long is still a pixel on the GS - the far rain's drops, TODO 146 - so it
                        // is made a pixel long along its own way, from its first end; a longer one stays as it is.)
                        // (TF_NATIVE_LINEMIN=0: as before, a short line as short as it is.)
                        static const bool lineMinOff = std::getenv("TF_NATIVE_LINEMIN") && *std::getenv("TF_NATIVE_LINEMIN") == '0';
                        if (len < 1.0f && !lineMinOff)
                        {
                            const float ux = len > 1e-4f ? dx / len : 1.0f, uy = len > 1e-4f ? dy / len : 0.0f;
                            b.x = a.x + ux, b.y = a.y + uy;
                            dx = ux, dy = uy, len = 1.0f;
                        }
                        const float nx = -dy / len * 0.5f, ny = dx / len * 0.5f;
                        Vertex a0 = a, a1 = a, b0 = b, b1 = b;
                        a0.x += nx, a0.y += ny, a1.x -= nx, a1.y -= ny;
                        b0.x += nx, b0.y += ny, b1.x -= nx, b1.y -= ny;
                        out(a0, a1, b0);
                        out(b0, a1, b1);
                    }
                    queue.erase(queue.begin());
                    if (type == 1u)
                        queue.clear();
                }
                break;
            case 0u: // point: the one GS pixel it covers, as a square of it (the warp tunnel's sparkling cloud)
                if (kick)
                {
                    Vertex a = queue[0], b = queue[0];
                    b.x += 1.0f, b.y += 1.0f;
                    Vertex ab = a, ba = a;
                    ab.x = b.x;
                    ba.y = b.y;
                    out(a, ab, ba);
                    out(ba, ab, b);
                }
                queue.clear();
                break;
            default:
                queue.clear();
                break;
            }
        }

        // A register write (A+D or REGLIST: the register's own 64-bit form).
        void write(State &s, uint32_t reg, uint64_t value, const Emit &emit)
        {
            if (s.onWrite)
                s.onWrite(reg, value);
            switch (reg)
            {
            case 0x00u: // PRIM
                s.primReg = static_cast<uint32_t>(value) & 0x7FFu;
                s.prim = s.prmodecont ? s.primReg : (s.primReg & 7u) | (s.prmode & 0x7F8u);
                s.queue.clear();
                break;
            case 0x1Au: // PRMODECONT
                s.prmodecont = static_cast<uint32_t>(value) & 1u;
                s.prim = s.prmodecont ? s.primReg : (s.primReg & 7u) | (s.prmode & 0x7F8u);
                break;
            case 0x1Bu: // PRMODE
                s.prmode = static_cast<uint32_t>(value) & 0x7FFu;
                s.prim = s.prmodecont ? s.primReg : (s.primReg & 7u) | (s.prmode & 0x7F8u);
                break;
            case 0x01u: // RGBAQ
                s.rgba = static_cast<uint32_t>(value);
                s.q = asFloat(static_cast<uint32_t>(value >> 32));
                break;
            case 0x02u: // ST
                s.st = value;
                break;
            case 0x03u: // UV
                s.uv = static_cast<uint32_t>(value);
                break;
            case 0x04u: // XYZF2
            case 0x0Cu: // XYZF3
                vertex(s, static_cast<uint32_t>(value), static_cast<uint32_t>(value >> 16), static_cast<uint32_t>(value >> 32) & 0xFFFFFFu,
                       reg == 0x04u, emit);
                break;
            case 0x05u: // XYZ2
            case 0x0Du: // XYZ3
                vertex(s, static_cast<uint32_t>(value), static_cast<uint32_t>(value >> 16), static_cast<uint32_t>(value >> 32), reg == 0x05u, emit);
                break;
            case 0x06u:
            case 0x07u:
                s.tex0[reg - 0x06u] = value;
                s.tex0At[reg - 0x06u] = ++s.writes;
                {
                    // (Debug, TODO 270: TF_NATIVE_TEX0LOG=<tbp hex>: each TEX0 write with that TBP - its value, the state's write
                    // count, and whether it came from the list's own words or applied from off the list, t_offList; 400 lines)
                    static const uint32_t want = std::getenv("TF_NATIVE_TEX0LOG") ? static_cast<uint32_t>(std::strtoul(std::getenv("TF_NATIVE_TEX0LOG"), nullptr, 16)) : 0xFFFFFFFFu;
                    static int left = 400;
                    if ((static_cast<uint32_t>(value) & 0x3FFFu) == want && left-- > 0)
                        std::fprintf(stderr, "[native render] TEX0_%u %016llx at write %llu, %s\n", reg - 5u, static_cast<unsigned long long>(value),
                                     static_cast<unsigned long long>(s.writes), t_offList ? "off the list" : "the list's own");
                }
                break;
            case 0x08u:
            case 0x09u:
                s.clamp[reg - 0x08u] = value;
                break;
            case 0x14u: // TEX1_1, TEX1_2
            case 0x15u:
                s.tex1[reg - 0x14u] = value;
                break;
            case 0x22u: // SCANMSK
                s.scanmsk = static_cast<uint32_t>(value) & 3u;
                break;
            case 0x46u: // COLCLAMP
                s.colclamp = static_cast<uint32_t>(value) & 1u;
                break;
            case 0x18u:
            case 0x19u:
                s.xyoffset[reg - 0x18u] = value;
                break;
            case 0x40u:
            case 0x41u:
                s.scissor[reg - 0x40u] = value;
                break;
            case 0x42u:
            case 0x43u:
                s.alpha[reg - 0x42u] = value;
                s.alphaAt[reg - 0x42u] = ++s.writes;
                break;
            case 0x47u:
            case 0x48u:
                s.test[reg - 0x47u] = value;
                s.testAt[reg - 0x47u] = ++s.writes;
                break;
            case 0x4Cu:
            case 0x4Du:
                s.frame[reg - 0x4Cu] = value;
                break;
            case 0x4Eu:
            case 0x4Fu:
                s.zbuf[reg - 0x4Eu] = value;
                break;
            default:
                break;
            }
        }
    }

    // A qword at a time, the tag's progress kept in the state: a packet's data goes on across
    // DIRECT chunks (an upload's image, long register lists).
    void run(State &s, const uint32_t *words, size_t qwords, const Emit &emit)
    {
        for (size_t at = 0; at < qwords; ++at)
        {
            const uint32_t *q = words + 4u * at;
            if (s.left == 0u)
            {
                // A tag - and a draw of its own: the 2D is laid out a tag at a time (the widescreen
                // narrowing places each tag's element, as the PS2 path's front end does).
                // (Points - a tag each, the warp tunnel's cloud: 150,000 a frame - stay one draw while the state holds.)
                if (s.open && !s.current.vertices.empty() && (s.current.prim & 7u) != 0u)
                {
                    emit(s.current);
                    s.open = false;
                }
                else if ((s.current.prim & 7u) != 0u)
                    s.open = false;
                const uint32_t nloop = q[0] & 0x7FFFu;
                s.flg = (q[1] >> 26) & 3u;
                s.nreg = (q[1] >> 28) ? (q[1] >> 28) : 16u;
                s.regs = lo64(q + 2);
                if (q[1] & 0x4000u) // PRE: PRIM from the tag
                    write(s, 0x00u, (q[1] >> 15) & 0x7FFu, emit);
                s.reg = 0u;
                if (s.flg == 0u)
                    s.left = nloop * s.nreg;
                else if (s.flg == 1u)
                    s.left = (nloop * s.nreg + 1u) / 2u, s.regsLeft = nloop * s.nreg;
                else
                    s.left = nloop; // IMAGE: its data skipped
                continue;
            }
            --s.left;
            if (s.flg == 0u) // PACKED: a qword a register
            {
                const uint32_t reg = static_cast<uint32_t>(s.regs >> (4u * s.reg)) & 0xFu;
                s.reg = (s.reg + 1u) % s.nreg;
                switch (reg)
                {
                case 0x0u:
                    write(s, 0x00u, q[0] & 0x7FFu, emit);
                    break;
                case 0x1u: // RGBAQ (Q: the last ST's)
                    s.rgba = (q[0] & 0xFFu) | (q[1] & 0xFFu) << 8 | (q[2] & 0xFFu) << 16 | (q[3] & 0xFFu) << 24;
                    break;
                case 0x2u: // ST (and Q)
                    s.st = lo64(q);
                    s.q = asFloat(q[2]);
                    break;
                case 0x3u:
                    s.uv = (q[0] & 0x3FFFu) | (q[1] & 0x3FFFu) << 16;
                    break;
                case 0x4u: // XYZF2 (the kick bit: bit 15 of the last word)
                case 0xCu:
                    vertex(s, q[0], q[1], (q[2] >> 4) & 0xFFFFFFu, reg == 0x4u && !(q[3] & 0x8000u), emit);
                    break;
                case 0x5u:
                case 0xDu:
                    vertex(s, q[0], q[1], q[2], reg == 0x5u && !(q[3] & 0x8000u), emit);
                    break;
                case 0xEu: // A+D
                    write(s, q[2] & 0xFFu, lo64(q), emit);
                    break;
                case 0xFu: // NOP
                    break;
                default:
                    write(s, reg, lo64(q), emit);
                    break;
                }
            }
            else if (s.flg == 1u) // REGLIST: 64 bits a register, two a qword
            {
                for (int half = 0; half < 2 && s.regsLeft > 0u; ++half, --s.regsLeft)
                {
                    const uint32_t reg = static_cast<uint32_t>(s.regs >> (4u * s.reg)) & 0xFu;
                    s.reg = (s.reg + 1u) % s.nreg;
                    if (reg != 0xEu && reg != 0xFu)
                        write(s, reg, lo64(q + 2 * half), emit);
                }
            }
        }
    }

    void flush(State &s, const Emit &emit)
    {
        if (s.open && !s.current.vertices.empty())
            emit(s.current);
        s.open = false;
        s.current = Primitives{};
    }
}
