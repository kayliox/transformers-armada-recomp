// A vlTexturePS2 read as data into a PC texture (README.md): its texels and palette are in main
// memory (VL_TextureNative_PlayStation2.cpp: the levels at 0x98, the palette levels[unk_E0], the
// image levels[unk_90]); the upload packet (func_00267060) sends each level as an IMAGE transfer in
// the level's own pixel format, in columns `tile` wide, and the GS samples it in the texture's
// format (0x34). Where the two differ - 8-bit data stored as a half-size 32-bit image, 4-bit as a
// 16-bit one (ITF flag 0x80) - the GS's page layouts do the reordering, so the conversion writes the
// level the way the GS stores it and reads it back the way the GS samples it (the runtime's GS
// address functions). A one-time conversion when the texture is first seen, not GS emulation at draw
// time; a PC build stores the result.
//
// (nr_core S7) The core's entry takes the texture from HOST memory (convertHostTexture: a HostTexture's values and pointers, no
// guest addresses); the recomp's entry - a vlTexturePS2's levels read from guest memory, the uploads' snapshots, the conversion
// workers' copies - is glue on top of it (nr_texture_guest.cpp).
#include "nr_types.h"

#include "nr_core.h"
#include "gs_decode.h"           // (nr_core S1: the GS page layouts, no runtime include)

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace nr
{
    namespace
    {
        constexpr uint32_t kPsmCt32 = 0x00u, kPsmCt24 = 0x01u, kPsmCt16 = 0x02u, kPsmT8 = 0x13u, kPsmT4 = 0x14u;
        // The indexed formats kept in a 32-bit buffer's top byte (its alpha: the picture's pages
        // reused for an 8-bit image, the colour beside it untouched): 8 bits, or 4 in either half.
        constexpr uint32_t kPsmT8H = 0x1Bu, kPsmT4HL = 0x24u, kPsmT4HH = 0x2Cu;

        bool highByte(uint32_t psm) { return psm == kPsmT8H || psm == kPsmT4HL || psm == kPsmT4HH; }
        bool indexed(uint32_t psm) { return psm == kPsmT8 || psm == kPsmT4 || highByte(psm); }
        uint32_t entriesOf(uint32_t psm) { return psm == kPsmT8 || psm == kPsmT8H ? 256u : 16u; }

        // (nr_core S1b: the GS upload skip's bookkeeping comes from the host, nr_core::TextureHost)
        void gsEnsureWhole(uint64_t first, uint64_t last)
        {
            if (const auto fn = nr_core::textureHost().ensureWhole)
                fn(first, last);
        }
        bool gsCheckMode()
        {
            const auto fn = nr_core::textureHost().checkMode;
            return fn && fn();
        }
        bool gsStaleAt(uint32_t address, bool topByte)
        {
            const auto fn = nr_core::textureHost().staleAt;
            return fn && fn(address, topByte);
        }
        void gsReportRead(const char *what, uint32_t tbp, uint32_t psm, uint32_t staleBytes, uint32_t bytes)
        {
            if (const auto fn = nr_core::textureHost().reportRead)
                fn(what, tbp, psm, staleBytes, bytes);
        }
        // (nr_core S5: the census's questions, through the host as well)
        void noteCensus(uint32_t kind, uint64_t amount)
        {
            if (const auto fn = nr_core::textureHost().censusNote)
                fn(kind, amount);
        }

        uint32_t pageWidth(uint32_t psm, uint32_t width)
        {
            return psm == kPsmT8 || psm == kPsmT4 ? ((width + 127u) / 128u) * 2u : (width + 63u) / 64u;
        }

        // The byte (or, for 4-bit, nibble) address of pixel (x, y) in a GS buffer at block 0.
        uint32_t address(uint32_t psm, uint32_t bw, uint32_t x, uint32_t y)
        {
            switch (psm)
            {
            case kPsmCt16:
                return nr_core::gs::gspsmct16::addrPSMCT16(0u, bw, x, y);
            case kPsmT8:
                return nr_core::gs::gspsmt8::addrPSMT8(0u, bw, x, y);
            case kPsmT4:
                return nr_core::gs::gspsmt4::addrPSMT4(0u, bw, x, y);
            default:
                return nr_core::gs::gspsmct32::addrPSMCT32(0u, bw, x, y);
            }
        }

        uint32_t gsColour(uint32_t c) // R in the low byte, alpha 0x80 = 1.0
        {
            const uint32_t a = std::min((c >> 24) * 2u, 255u);
            return (c & 0x00FFFFFFu) | a << 24;
        }

        constexpr size_t kGsBytes = 4u << 20;

        uint32_t paletteSwizzle(uint32_t i) { return (i & ~0x18u) | ((i & 8u) << 1) | ((i & 0x10u) >> 1); }

    }

    namespace
    {
        const uint8_t *g_vram = nullptr;
    }

    void setGsMemory(const uint8_t *vram) { g_vram = vram; }

    // (Debug: a region of GS memory as a PPM - 16- or 32-bit pixels at TBP, TBW.)
    void dumpGsRegion(const char *path, uint32_t tbp, uint32_t tbw, uint32_t psm, uint32_t w, uint32_t h)
    {
        if (!g_vram || w == 0u || h == 0u || w > 2048u || h > 2048u)
            return;
        FILE *f = std::fopen(path, "wb");
        if (!f)
            return;
        std::fprintf(f, "P6\n%u %u\n255\n", w, h);
        const uint32_t bw = tbw ? tbw : pageWidth(psm, w);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                uint8_t rgb[3] = {};
                if (psm == kPsmCt16 || psm == 0x0Au)
                {
                    const uint32_t at = nr_core::gs::gspsmct16::addrPSMCT16(tbp, bw, x, y);
                    if (at + 2u <= kGsBytes)
                    {
                        const uint32_t c = g_vram[at] | g_vram[at + 1u] << 8;
                        rgb[0] = static_cast<uint8_t>((c & 31u) << 3), rgb[1] = static_cast<uint8_t>(((c >> 5) & 31u) << 3),
                        rgb[2] = static_cast<uint8_t>(((c >> 10) & 31u) << 3);
                    }
                }
                else if (psm == 0x7Fu) // (a debug choice: a CT32 buffer's alpha, as grey)
                {
                    const uint32_t at = nr_core::gs::gspsmct32::addrPSMCT32(tbp, bw, x, y);
                    if (at + 4u <= kGsBytes)
                        rgb[0] = rgb[1] = rgb[2] = g_vram[at + 3u];
                }
                else
                {
                    const uint32_t at = nr_core::gs::gspsmct32::addrPSMCT32(tbp, bw, x, y);
                    if (at + 4u <= kGsBytes)
                        std::memcpy(rgb, g_vram + at, 3u);
                }
                std::fwrite(rgb, 1u, 3u, f);
            }
        std::fclose(f);
    }

    void buildMips(Texture &texture)
    {
        texture.mips.clear();
        const std::vector<uint32_t> *src = &texture.pixels;
        uint32_t pw = texture.width, ph = texture.height;
        while (pw > 1u || ph > 1u)
        {
            const uint32_t w = std::max(1u, pw / 2u), h = std::max(1u, ph / 2u);
            std::vector<uint32_t> dst(static_cast<size_t>(w) * h);
            for (uint32_t y = 0; y < h; ++y)
            {
                const uint32_t *r0 = src->data() + static_cast<size_t>(std::min(ph - 1u, 2u * y)) * pw;
                const uint32_t *r1 = src->data() + static_cast<size_t>(std::min(ph - 1u, 2u * y + 1u)) * pw;
                for (uint32_t x = 0; x < w; ++x)
                {
                    const uint32_t x0 = std::min(pw - 1u, 2u * x), x1 = std::min(pw - 1u, 2u * x + 1u);
                    const uint32_t c[4] = {r0[x0], r0[x1], r1[x0], r1[x1]};
                    uint32_t out = 0u;
                    for (int k = 0; k < 4; ++k)
                    {
                        const uint32_t sum = ((c[0] >> (8 * k)) & 0xFFu) + ((c[1] >> (8 * k)) & 0xFFu) + ((c[2] >> (8 * k)) & 0xFFu) +
                                             ((c[3] >> (8 * k)) & 0xFFu);
                        out |= ((sum + 2u) / 4u) << (8 * k);
                    }
                    dst[static_cast<size_t>(y) * w + x] = out;
                }
            }
            texture.mips.push_back(std::move(dst));
            src = &texture.mips.back();
            pw = w;
            ph = h;
        }
    }

    std::shared_ptr<const Texture> convertHostPalette(const uint8_t *c, uint32_t clutEntries, uint64_t id)
    {
        if (!c || clutEntries < 256u)
            return nullptr;
        auto out = std::make_shared<Texture>();
        out->id = id;
        out->width = 256u;
        out->height = 1u;
        out->pixels.resize(256u);
        for (uint32_t i = 0; i < 256u; ++i)
        {
            uint32_t v;
            std::memcpy(&v, c + 4u * paletteSwizzle(i), 4);
            out->pixels[i] = gsColour(v);
        }
        return out;
    }

    namespace
    {
        // One level into RGBA8 (convertTexture's conversion before nr_core S7): its texels written the way the GS stores
        // them in the level's own format, read back the way the GS samples them in TEX0's (psm, width x height, tbw), with
        // the palette's entries (csa) for an indexed format. Null: not readable as one (no texels, an unknown size).
        std::shared_ptr<Texture> decodeLevel(uint32_t psm, uint32_t width, uint32_t height, uint32_t tbw, uint32_t csa, const HostTexture::Level &lv,
                                             const uint8_t *clut, uint32_t clutEntries, bool gsMemory, uint32_t tbp, uint32_t source, uint64_t id)
        {
            if (width == 0u || height == 0u || width > 2048u || height > 2048u)
                return nullptr;
            uint32_t upsm = lv.psm;
            // A group with 24-bit textures packs its 8-bit and 4-bit levels into those pages' alpha bytes
            // (vlTextureGroupRegion's capture: T8 -> T8H, T4 -> T4HL / T4HH, the upload sent in that
            // format) and puts the level's own format back afterwards: the texels in main memory are the
            // plain indices, stored in GS memory as TEX0's format says, not as the level's.
            if (highByte(psm) && (upsm == kPsmT8 || upsm == kPsmT4) && (upsm == kPsmT8) == (psm == kPsmT8H))
                upsm = psm;
            const uint32_t uw = lv.width, uh = lv.height;
            const uint32_t bpp = lv.bpp;
            uint32_t tile = lv.tile;
            if (uw == 0u || uh == 0u || uw > 2048u || uh > 2048u || (bpp != 4u && bpp != 8u && bpp != 16u && bpp != 24u && bpp != 32u))
                return nullptr;
            if (tile == 0u || tile > uw)
                tile = uw;
            const uint8_t *src = lv.texels && lv.bytes >= (uw * uh * bpp + 7u) / 8u ? lv.texels : nullptr;
            // (No texels in main memory: a top-byte image read from GS memory where TEX0 points, if the
            // GS memory is known - the light maps. Never on a worker: captureTextureInputs leaves those to the draw.)
            const bool fromGs = !src && g_vram && highByte(psm) && gsMemory;
            if (!src && !fromGs)
                return nullptr;
            if (fromGs)
                noteCensus(1u, static_cast<uint64_t>(width) * height); // (TF_VU1_CENSUS: a light map read from GS memory)

            // The level as the GS stores it. (TODO 164: only the pages the level and the reading reach are cleared - the
            // whole 4 MB each time was a fifth of a conversion's time; what lies beyond them is never read.)
            static thread_local std::vector<uint8_t> gs;
            if (gs.size() != kGsBytes)
                gs.assign(kGsBytes, 0u);
            {
                const uint32_t rbw0 = tbw ? tbw : pageWidth(psm, width);
                auto pageEnd = [](uint32_t p, uint32_t bw, uint32_t w, uint32_t h) -> size_t {
                    const uint32_t a = address(p, std::max(bw, 1u), w - 1u, h - 1u);
                    const size_t at = (p == kPsmT4 ? a >> 1 : a) + 4u;
                    return std::min(kGsBytes, (at + 8191u) / 8192u * 8192u + 8192u);
                };
                size_t reach = std::max(pageEnd(upsm, pageWidth(upsm, uw), uw, uh), pageEnd(psm, rbw0, width, height));
                if (fromGs)
                    reach = std::max(reach, pageEnd(kPsmCt32, rbw0, width, height));
                static const bool wholeClear = std::getenv("TF_NATIVE_TEXCLEAR") && *std::getenv("TF_NATIVE_TEXCLEAR") == '1';
                std::fill(gs.begin(), gs.begin() + static_cast<std::ptrdiff_t>(wholeClear ? kGsBytes : reach), uint8_t{0});
            }
            if (fromGs)
            {
                // Its pages copied to block 0, where the reading below looks.
                const uint32_t rbw = tbw ? tbw : pageWidth(psm, width);
                // (gsupload: the uploads a skip left out that reach into its pages written first - PS2Recomp patch 0073)
                gsEnsureWhole(static_cast<uint64_t>(tbp) * 256u,
                                      static_cast<uint64_t>(tbp) * 256u + ((static_cast<uint64_t>(height - 1u) / 32u) * std::max(rbw, 1u) + (width - 1u) / 64u + 1u) * 8192u);
                for (uint32_t y = 0; y < height; ++y)
                    for (uint32_t x = 0; x < width; ++x)
                    {
                        const uint32_t from = nr_core::gs::gspsmct32::addrPSMCT32(tbp, rbw, x, y), to = nr_core::gs::gspsmct32::addrPSMCT32(0u, rbw, x, y);
                        if (from + 4u <= kGsBytes && to + 4u <= kGsBytes)
                            std::memcpy(&gs[to], g_vram + from, 4u);
                    }
                // (PS2X_GS_SKIP_UPLOADS=check: would a skip of the uploads have left any of it stale - its top bytes, a light map's)
                if (gsCheckMode())
                {
                    uint32_t stale = 0u;
                    for (uint32_t y = 0; y < height; ++y)
                        for (uint32_t x = 0; x < width; ++x)
                            if (gsStaleAt(nr_core::gs::gspsmct32::addrPSMCT32(tbp, rbw, x, y), true))
                                ++stale;
                    gsReportRead("a light map read from GS memory", tbp, psm, stale, width * height);
                }
            }
            const uint32_t dbw = pageWidth(upsm, uw);
            for (uint32_t x0 = 0; !fromGs && x0 < uw; x0 += tile)
                for (uint32_t y = 0; y < uh; ++y)
                    for (uint32_t i = 0; i < tile && x0 + i < uw; ++i)
                    {
                        const uint32_t n = x0 * uh + y * tile + i; // the pixel's index in the level's data
                        const uint32_t x = x0 + i;
                        const uint32_t at = address(upsm, dbw, x, y);
                        switch (upsm)
                        {
                        case kPsmT4:
                        {
                            const uint8_t nib = (src[n >> 1] >> ((n & 1u) * 4u)) & 0xFu;
                            if ((at >> 1) < kGsBytes)
                                gs[at >> 1] = static_cast<uint8_t>((gs[at >> 1] & ~(0xFu << ((at & 1u) * 4u))) | nib << ((at & 1u) * 4u));
                            break;
                        }
                        case kPsmT8:
                            if (at < kGsBytes)
                                gs[at] = src[n];
                            break;
                        case kPsmCt16:
                            if (at + 2u <= kGsBytes)
                                std::memcpy(&gs[at], src + 2u * n, 2u);
                            break;
                        case kPsmT8H:
                            if (at + 4u <= kGsBytes)
                                gs[at + 3u] = src[n];
                            break;
                        case kPsmT4HL:
                        case kPsmT4HH:
                            if (at + 4u <= kGsBytes)
                            {
                                const uint8_t nib = (src[n >> 1] >> ((n & 1u) * 4u)) & 0xFu;
                                const uint32_t shift = upsm == kPsmT4HH ? 4u : 0u;
                                gs[at + 3u] = static_cast<uint8_t>((gs[at + 3u] & ~(0xFu << shift)) | nib << shift);
                            }
                            break;
                        case kPsmCt24:
                            if (at + 3u <= kGsBytes)
                                std::memcpy(&gs[at], src + 3u * n, 3u);
                            break;
                        default:
                            if (at + 4u <= kGsBytes)
                                std::memcpy(&gs[at], src + 4u * n, 4u);
                            break;
                        }
                    }

            // The palette (indexed formats): its level, in 32-bit GS form (an 8-bit one with its
            // entries' bits 3 and 4 exchanged, the GS's CSM1 order); a 4-bit one at CSA * 16.
            std::vector<uint32_t> palette;
            if (indexed(psm))
            {
                const uint32_t entries = entriesOf(psm);
                const uint32_t offset = entries == 16u ? csa * 16u : 0u;
                const uint8_t *c = clut && clutEntries >= offset + entries ? clut : nullptr;
                if (!c)
                    return nullptr;
                palette.resize(entries);
                for (uint32_t i = 0; i < entries; ++i)
                {
                    uint32_t v;
                    std::memcpy(&v, c + 4u * (entries == 256u ? paletteSwizzle(i) : offset + i), 4);
                    palette[i] = gsColour(v);
                }
            }

            // Read back the way the GS samples it.
            auto out = std::make_shared<Texture>();
            out->id = id;
            out->source = source;
            out->width = width;
            out->height = height;
            out->pixels.resize(static_cast<size_t>(width) * height);
            const uint32_t rbw = tbw ? tbw : pageWidth(psm, width);
            for (uint32_t y = 0; y < height; ++y)
                for (uint32_t x = 0; x < width; ++x)
                {
                    const uint32_t at = address(psm, rbw, x, y);
                    uint32_t c = 0u;
                    switch (psm)
                    {
                    case kPsmT4:
                        c = (at >> 1) < kGsBytes ? palette[(gs[at >> 1] >> ((at & 1u) * 4u)) & 0xFu] : 0u;
                        break;
                    case kPsmT8:
                        c = at < kGsBytes ? palette[gs[at]] : 0u;
                        break;
                    case kPsmT8H:
                        c = at + 4u <= kGsBytes ? palette[gs[at + 3u]] : 0u;
                        break;
                    case kPsmT4HL:
                        c = at + 4u <= kGsBytes ? palette[gs[at + 3u] & 0xFu] : 0u;
                        break;
                    case kPsmT4HH:
                        c = at + 4u <= kGsBytes ? palette[gs[at + 3u] >> 4] : 0u;
                        break;
                    case kPsmCt16:
                    {
                        uint16_t v = 0;
                        if (at + 2u <= kGsBytes)
                            std::memcpy(&v, &gs[at], 2);
                        c = (v & 0x1Fu) << 3 | ((v >> 5) & 0x1Fu) << 11 | ((v >> 10) & 0x1Fu) << 19 | (v & 0x8000u ? 0xFF000000u : 0u);
                        break;
                    }
                    case kPsmCt24:
                        if (at + 4u <= kGsBytes)
                            std::memcpy(&c, &gs[at], 4);
                        c = (c & 0x00FFFFFFu) | 0xFF000000u;
                        break;
                    default:
                        if (at + 4u <= kGsBytes)
                            std::memcpy(&c, &gs[at], 4);
                        c = gsColour(c);
                        break;
                    }
                    out->pixels[static_cast<size_t>(y) * width + x] = c;
                }
            return out;
        }
    }

    std::shared_ptr<const Texture> convertHostTexture(const HostTexture &in, uint64_t id)
    {
        auto top = decodeLevel(in.psm, in.width, in.height, in.tbw, in.csa, in.image, in.clut, in.clutEntries, in.gsMemory, in.tbp, in.source, id);
        if (!in.mipCount || !top)
            return top;
        // The game's own levels below the image (what the GS reads past level 0 under TEX1): each
        // converted as the image is - its texels in the level's layout, read in TEX0's format at
        // half the size of the one above, the image's palette - and kept as the texture's first
        // mips; the GPU side box-filters only below the last of them (nr_d3d.cpp: makeTexture).
        auto out = std::make_shared<Texture>(*top);
        for (uint32_t k = 1; k <= 3u && k <= in.mipCount; ++k)
        {
            const uint32_t mw = std::max(1u, in.width >> k), mh = std::max(1u, in.height >> k);
            // (the level's own buffer width, as its upload laid it out; its texels there - the host gives no level without)
            auto level = decodeLevel(in.psm, mw, mh, 0u, in.csa, in.mips[k - 1u], in.clut, in.clutEntries, false, in.tbp, in.source, 0u);
            if (!level || level->pixels.size() != static_cast<size_t>(mw) * mh)
                break;
            out->mips.push_back(level->pixels);
            ++out->gameMips;
        }
        static int mipLog = std::getenv("TF_NATIVE_MIPLOG") ? std::atoi(std::getenv("TF_NATIVE_MIPLOG")) : 0;
        if (mipLog > 0 && out->gameMips)
        {
            // (Debug: how far the game's level 1 is from the image box-filtered: per channel, mean.)
            --mipLog;
            Texture boxed;
            boxed.width = out->width;
            boxed.height = out->height;
            boxed.pixels = out->pixels;
            buildMips(boxed);
            double diff[4] = {};
            const auto &a = out->mips[0], &b = boxed.mips[0];
            for (size_t i = 0; i < a.size() && i < b.size(); ++i)
                for (int k = 0; k < 4; ++k)
                    diff[k] += std::abs(static_cast<int>((a[i] >> (8 * k)) & 0xFFu) - static_cast<int>((b[i] >> (8 * k)) & 0xFFu));
            const double n = static_cast<double>(std::max<size_t>(a.size(), 1u));
            std::fprintf(stderr, "[native render] game mips of %08x %ux%u: %u levels, level 1 against the box filter: R %.2f G %.2f B %.2f A %.2f\n", in.source,
                         out->width, out->height, out->gameMips, diff[0] / n, diff[1] / n, diff[2] / n, diff[3] / n);
        }
        return out;
    }

    uint64_t pixelHash(const Texture &t)
    {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&h](const std::vector<uint32_t> &v) {
            const size_t n = v.size();
            size_t i = 0;
            for (; i + 2u <= n; i += 2u)
            {
                uint64_t q;
                std::memcpy(&q, v.data() + i, 8);
                h = (h ^ q) * 1099511628211ull;
            }
            if (i < n)
                h = (h ^ v[i]) * 1099511628211ull;
        };
        mix(t.pixels);
        for (size_t k = 0; k < t.gameMips && k < t.mips.size(); ++k)
            mix(t.mips[k]);
        return h ? h : 1u;
    }

    uint64_t hdContentKey(const std::string &name, const Texture &t)
    {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&h](const void *p, size_t n) {
            const auto *b = static_cast<const uint8_t *>(p);
            for (size_t i = 0; i < n; ++i)
                h = (h ^ b[i]) * 1099511628211ull;
        };
        mix(name.data(), name.size());
        const uint32_t dims[3] = {t.width, t.height, t.gameMips};
        mix(dims, sizeof dims);
        // (TODO 164: the texels' hash, 8 bytes at a time - made on the conversion worker, where it was converted
        // there; the bytes one at a time were a sixth of an HQ conversion's time)
        const uint64_t texels = t.pixelHash ? t.pixelHash : pixelHash(t);
        mix(&texels, sizeof texels);
        return h;
    }

    // (N3b) A texture read straight from GS memory as TEX0 says - its pixels and palette uploaded there by the game's own
    // IMAGE transfers, no texture object of the render device (the load screens drawn outside the game's frames).
    // CT32 / CT24 / CT16, T8 / T4 (and their top-byte views) with a CT32 or CT16 palette in CSM1 order. Null: none.
    std::shared_ptr<Texture> textureFromGsMemory(uint64_t tex0, uint64_t id)
    {
        if (!g_vram)
            return nullptr;
        const uint32_t tbp = static_cast<uint32_t>(tex0 & 0x3FFFu), tbw = static_cast<uint32_t>((tex0 >> 14) & 0x3Fu);
        const uint32_t psm = static_cast<uint32_t>((tex0 >> 20) & 0x3Fu);
        const uint32_t width = 1u << ((tex0 >> 26) & 0xFu), height = 1u << ((tex0 >> 30) & 0xFu);
        const uint32_t cbp = static_cast<uint32_t>((tex0 >> 37) & 0x3FFFu), cpsm = static_cast<uint32_t>((tex0 >> 51) & 0xFu);
        const uint32_t csa = static_cast<uint32_t>((tex0 >> 56) & 0x1Fu);
        if (width > 1024u || height > 1024u)
            return nullptr;
        noteCensus(2u, static_cast<uint64_t>(width) * height); // (TF_VU1_CENSUS)
        {
            // (gsupload: the uploads a skip left out that reach into its texels or its palette written first - patch 0073)
            const uint32_t pw = tbw ? tbw : std::max(1u, width / 64u);
            gsEnsureWhole(static_cast<uint64_t>(tbp) * 256u,
                                  static_cast<uint64_t>(tbp) * 256u + ((static_cast<uint64_t>(height - 1u) / 32u) * pw + (width - 1u) / 64u + 1u) * 8192u);
            gsEnsureWhole(static_cast<uint64_t>(cbp) * 256u, static_cast<uint64_t>(cbp) * 256u + 8192u);
        }
        if (gsCheckMode())
        {
            // (PS2X_GS_SKIP_UPLOADS=check) Its texels' and palette's GS memory, every 4th texel each way: stale under a skip?
            uint32_t stale = 0u, seen = 0u;
            const uint32_t w = tbw ? tbw : std::max(1u, width / 64u);
            for (uint32_t y = 0; y < height; y += 4u)
                for (uint32_t x = 0; x < width; x += 4u)
                {
                    uint32_t at;
                    switch (psm)
                    {
                    case 0x02u: case 0x0Au: at = nr_core::gs::gspsmct16::addrPSMCT16(tbp, w, x, y); break;
                    case 0x13u: at = nr_core::gs::gspsmt8::addrPSMT8(tbp, w, x, y); break;
                    case 0x14u: at = nr_core::gs::gspsmt4::addrPSMT4(tbp, w, x, y) >> 1u; break;
                    default: at = nr_core::gs::gspsmct32::addrPSMCT32(tbp, w, x, y); break;
                    }
                    ++seen;
                    if (gsStaleAt(at, psm == 0x1Bu || psm == 0x24u || psm == 0x2Cu))
                        ++stale;
                }
            gsReportRead("a texture read from GS memory (TEX0)", tbp, psm, stale, seen);
        }
        const uint32_t bw = tbw ? tbw : std::max(1u, width / 64u);
        const auto ct32 = [](uint32_t block, uint32_t w, uint32_t x, uint32_t y) {
            const uint32_t at = nr_core::gs::gspsmct32::addrPSMCT32(block, w, x, y);
            uint32_t c = 0u;
            if (at + 4u <= kGsBytes)
                std::memcpy(&c, g_vram + at, 4u);
            return c;
        };
        const auto ct16 = [](uint32_t block, uint32_t w, uint32_t x, uint32_t y) {
            const uint32_t at = nr_core::gs::gspsmct16::addrPSMCT16(block, w, x, y);
            const uint32_t v = at + 2u <= kGsBytes ? static_cast<uint32_t>(g_vram[at] | g_vram[at + 1u] << 8) : 0u;
            return (v & 0x1Fu) << 3 | ((v >> 5) & 0x1Fu) << 11 | ((v >> 10) & 0x1Fu) << 19 | (v & 0x8000u ? 0x80000000u : 0u);
        };
        std::vector<uint32_t> palette;
        if (indexed(psm))
        {
            const uint32_t entries = entriesOf(psm);
            palette.resize(entries);
            for (uint32_t i = 0; i < entries; ++i)
            {
                uint32_t x, y;
                if (entries == 256u)
                {
                    const uint32_t s = paletteSwizzle(i);
                    x = s & 15u, y = s >> 4;
                }
                else
                {
                    // (16 entries: an 8 x 2 tile of the palette's 16 x 16, chosen by CSA)
                    x = (csa & 1u) * 8u + (i & 7u), y = (csa >> 1) * 2u + (i >> 3);
                }
                palette[i] = gsColour(cpsm == 0u || cpsm == 1u ? ct32(cbp, 1u, x, y) : ct16(cbp, 1u, x, y));
            }
        }
        auto out = std::make_shared<Texture>();
        out->id = id;
        out->width = width;
        out->height = height;
        out->pixels.resize(static_cast<size_t>(width) * height);
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
            {
                uint32_t c = 0u;
                switch (psm)
                {
                case kPsmT4:
                {
                    const uint32_t at = nr_core::gs::gspsmt4::addrPSMT4(tbp, bw, x, y);
                    c = (at >> 1) < kGsBytes ? palette[(g_vram[at >> 1] >> ((at & 1u) * 4u)) & 0xFu] : 0u;
                    break;
                }
                case kPsmT8:
                {
                    const uint32_t at = nr_core::gs::gspsmt8::addrPSMT8(tbp, bw, x, y);
                    c = at < kGsBytes ? palette[g_vram[at]] : 0u;
                    break;
                }
                case kPsmT8H:
                    c = palette[ct32(tbp, bw, x, y) >> 24];
                    break;
                case kPsmT4HL:
                    c = palette[(ct32(tbp, bw, x, y) >> 24) & 0xFu];
                    break;
                case kPsmT4HH:
                    c = palette[ct32(tbp, bw, x, y) >> 28];
                    break;
                case kPsmCt16:
                case 0x0Au:
                    c = gsColour(ct16(tbp, bw, x, y));
                    break;
                case kPsmCt24:
                    c = (ct32(tbp, bw, x, y) & 0x00FFFFFFu) | 0xFF000000u;
                    break;
                default:
                    c = gsColour(ct32(tbp, bw, x, y));
                    break;
                }
                out->pixels[static_cast<size_t>(y) * width + x] = c;
            }
        return out;
    }
}
