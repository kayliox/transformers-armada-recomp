// The game textures' mip chains (nr_frame.h): each level the one above box-filtered - for the frame builder and the D3D12 device's
// own uploads. In the core since nr_core S5 (moved unchanged from nr_frame.cpp): pure image work, no runtime.
#include "nr_frame.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

namespace nr::frame
{
    // A mip level from the one above: each texel the rounded mean of its 2x2 (per channel, (sum + 2) / 4; an odd
    // edge's last row or column repeated) - two channels at a time in 16-bit lanes (TODO 76: the per-channel loop
    // was most of a new game texture's cost once its video memory came from the pool).
    void halveLevel(const uint32_t *src, uint32_t pw, uint32_t ph, uint32_t *dst)
    {
        const uint32_t w = std::max(1u, pw / 2u), h = std::max(1u, ph / 2u);
        constexpr uint32_t m = 0x00FF00FFu, r = 0x00020002u;
        for (uint32_t y = 0; y < h; ++y)
        {
            const uint32_t *r0 = src + static_cast<size_t>(std::min(ph - 1u, 2u * y)) * pw;
            const uint32_t *r1 = src + static_cast<size_t>(std::min(ph - 1u, 2u * y + 1u)) * pw;
            uint32_t *out = dst + static_cast<size_t>(y) * w;
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint32_t x0 = std::min(pw - 1u, 2u * x), x1 = std::min(pw - 1u, 2u * x + 1u);
                const uint32_t a = r0[x0], b = r0[x1], c = r1[x0], d = r1[x1];
                const uint32_t lo = (a & m) + (b & m) + (c & m) + (d & m) + r;
                const uint32_t hi = ((a >> 8) & m) + ((b >> 8) & m) + ((c >> 8) & m) + ((d >> 8) & m) + r;
                out[x] = ((lo >> 2) & m) | (((hi >> 2) & m) << 8);
            }
        }
    }

    // The mip chain (the frame builder's), each level the one before box-filtered (2x2, per channel) - or as made
    // ahead (the large images: off the game's thread). (The levels read in place: a 4K image is not copied on the
    // game's thread; `computed` holds the ones made here.)
    void mipChain(uint32_t width, uint32_t height, const uint32_t *pixels, const std::vector<std::vector<uint32_t>> *made,
                  std::vector<const uint32_t *> &mips, std::vector<std::vector<uint32_t>> &computed)
    {
        const auto t0 = std::chrono::steady_clock::now();
        mips.assign(1, pixels);
        computed.clear();
        computed.reserve(16);
        std::vector<std::pair<uint32_t, uint32_t>> sizes(1, {width, height});
        if (made && !made->empty())
            for (const auto &level : *made)
            {
                const uint32_t w = std::max(1u, sizes.back().first / 2u), h = std::max(1u, sizes.back().second / 2u);
                if (level.size() != static_cast<size_t>(w) * h)
                    break;
                mips.push_back(level.data());
                sizes.push_back({w, h});
            }
        while (sizes.back().first > 1u || sizes.back().second > 1u)
        {
            const uint32_t pw = sizes.back().first, ph = sizes.back().second;
            const uint32_t w = std::max(1u, pw / 2u), h = std::max(1u, ph / 2u);
            std::vector<uint32_t> dst(static_cast<size_t>(w) * h);
            halveLevel(mips.back(), pw, ph, dst.data());
            computed.push_back(std::move(dst));
            mips.push_back(computed.back().data());
            sizes.push_back({w, h});
        }
        g_mkMs[0] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
}
