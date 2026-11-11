// The API-neutral parts of drawing a frame (nr_draw.h; roadmap P3, step S2). Moved here from nr_d3d.cpp unchanged.
#include "nr_draw.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace nr::draw
{
    gfx::BlendState blendOf(uint64_t alpha, bool abe)
    {
        gfx::BlendState k;
        if (!abe)
            return k;
        const int a = static_cast<int>(alpha & 3u), b = static_cast<int>((alpha >> 2) & 3u), c = static_cast<int>((alpha >> 4) & 3u),
                  d = static_cast<int>((alpha >> 6) & 3u);
        // Coefficients: (weight of C, constant), for Cs (0) and Cd (1).
        int cw[2] = {0, 0}, k1[2] = {0, 0};
        if (a < 2)
            cw[a] += 1;
        if (b < 2)
            cw[b] -= 1;
        if (d < 2)
            k1[d] += 1;
        const gfx::Blend cf = c == 0 ? gfx::Blend::SrcAlpha : c == 1 ? gfx::Blend::DestAlpha : gfx::Blend::Factor;
        const gfx::Blend icf = c == 0 ? gfx::Blend::InvSrcAlpha : c == 1 ? gfx::Blend::InvDestAlpha : gfx::Blend::InvFactor;
        auto factor = [&](int w, int k, bool &negative) {
            negative = false;
            if (w == 0)
                return k ? gfx::Blend::One : gfx::Blend::Zero;
            if (w == 1)
                return k ? gfx::Blend::One : cf; // (1 + C taken as 1)
            negative = !k;
            return k ? icf : cf;                 // -C + 1 = 1 - C; -C alone: subtract
        };
        bool ns = false, nd = false;
        k.src = factor(cw[0], k1[0], ns);
        k.dst = factor(cw[1], k1[1], nd);
        static const bool noPremultiply = std::getenv("TF_NATIVE_PREMULTIPLY") && *std::getenv("TF_NATIVE_PREMULTIPLY") == '0';
        if (!noPremultiply && a == 0 && b == 2 && c != 1 && d != 0)
        {
            k.src = gfx::Blend::One;
            k.premultiply = c == 0 ? 1 : 2;
        }
        k.op = ns && !nd ? gfx::BlendOp::RevSubtract : nd && !ns ? gfx::BlendOp::Subtract : gfx::BlendOp::Add;
        k.enabled = true;
        return k;
    }

    uint32_t writeMaskOf(uint32_t fbmsk)
    {
        uint32_t mask = 0u;
        if ((fbmsk & 0x000000FFu) != 0x000000FFu)
            mask |= gfx::kWriteR;
        if ((fbmsk & 0x0000FF00u) != 0x0000FF00u)
            mask |= gfx::kWriteG;
        if ((fbmsk & 0x00FF0000u) != 0x00FF0000u)
            mask |= gfx::kWriteB;
        if ((fbmsk & 0xFF000000u) != 0xFF000000u)
            mask |= gfx::kWriteA;
        return mask;
    }

    gfx::Compare compareOfZtst(uint32_t ztst)
    {
        return ztst == 0u ? gfx::Compare::Never : ztst == 1u ? gfx::Compare::Always : ztst == 2u ? gfx::Compare::GreaterEqual : gfx::Compare::Greater;
    }

    void tex1Constants(uint64_t tex1, float sharpShift, bool smoothMagnified, float *lod, float *lodf)
    {
        static const bool off = std::getenv("TF_NATIVE_TEX1") && *std::getenv("TF_NATIVE_TEX1") == '0';
        if (off)
            return;
        const uint32_t mmin = static_cast<uint32_t>(tex1 >> 6) & 7u;
        lod[0] = smoothMagnified ? 2.0f : 1.0f;
        lod[1] = static_cast<float>(static_cast<int32_t>(static_cast<uint32_t>(tex1 >> 32) << 20) >> 20) / 16.0f;
        lod[2] = static_cast<float>(1u << ((tex1 >> 19) & 3u));
        lod[3] = static_cast<float>(std::min<uint32_t>(static_cast<uint32_t>(tex1 >> 2) & 7u, 6u));
        lodf[0] = static_cast<float>(tex1 & 1u);
        if ((tex1 & 1u) == 0u)
            lod[1] -= sharpShift;
        lodf[1] = static_cast<float>((tex1 >> 5) & 1u);
        lodf[2] = mmin == 1u || mmin >= 4u ? 1.0f : 0.0f;
        lodf[3] = mmin == 2u || mmin == 4u ? 1.0f : mmin == 3u || mmin == 5u ? 2.0f : 0.0f;
    }

    void boundsOf(const Mesh &mesh, float lo[3], float hi[3])
    {
        lo[0] = lo[1] = lo[2] = 1e30f;
        hi[0] = hi[1] = hi[2] = -1e30f;
        for (const Vertex &v : mesh.vertices)
        {
            const float p[3] = {v.x, v.y, v.z};
            for (int k = 0; k < 3; ++k)
                lo[k] = std::min(lo[k], p[k]), hi[k] = std::max(hi[k], p[k]);
        }
    }

    bool boxInside(const float lo[3], const float hi[3], const float (*clip)[4])
    {
        for (int c = 0; c < 8; ++c)
        {
            const float x = (c & 1) ? hi[0] : lo[0], y = (c & 2) ? hi[1] : lo[1], z = (c & 4) ? hi[2] : lo[2];
            float k[4];
            for (int r = 0; r < 4; ++r)
                k[r] = clip[0][r] * x + clip[1][r] * y + clip[2][r] * z + clip[3][r];
            if (!(std::fabs(k[0]) <= k[3] && std::fabs(k[1]) <= k[3] && std::fabs(k[2]) <= k[3]))
                return false;
        }
        return true;
    }

    bool clipRunsOf(const Mesh &mesh, const float (*clip)[4], std::vector<ClipRun> &runs)
    {
        runs.clear();
        // Per vertex and plane: `side` bit p = not clearly inside plane p, `out` bit p = clearly outside it.
        static thread_local std::vector<uint8_t> side, out;
        const size_t n = mesh.vertices.size();
        side.resize(n);
        out.resize(n);
        for (size_t i = 0; i < n; ++i)
        {
            const Vertex &v = mesh.vertices[i];
            float k[4];
            for (int r = 0; r < 4; ++r)
                k[r] = clip[0][r] * v.x + clip[1][r] * v.y + clip[2][r] * v.z + clip[3][r];
            const float eps = 1e-4f * (std::fabs(k[3]) + 1.0f);
            const float d[6] = {k[3] - k[0], k[3] + k[0], k[3] - k[1], k[3] + k[1], k[3] - k[2], k[3] + k[2]};
            uint8_t s = 0, o = 0;
            for (int p = 0; p < 6; ++p)
            {
                s |= static_cast<uint8_t>((d[p] < eps ? 1u : 0u) << p);
                o |= static_cast<uint8_t>((d[p] < -eps ? 1u : 0u) << p);
            }
            side[i] = s;
            out[i] = o;
        }
        const auto &idx = mesh.indices;
        for (size_t t = 0; t + 2 < idx.size(); t += 3)
        {
            const uint32_t a = idx[t], b = idx[t + 1], c = idx[t + 2];
            if (a >= n || b >= n || c >= n)
                return false;
            const int kind = (out[a] & out[b] & out[c]) ? 2 : ((side[a] | side[b] | side[c]) ? 1 : 0);
            if (!runs.empty() && runs.back().kind == kind)
                runs.back().count += 3u;
            else
            {
                if (runs.size() >= 24u)
                    return false;
                runs.push_back(ClipRun{static_cast<uint32_t>(t), 3u, kind});
            }
        }
        return true;
    }
}
