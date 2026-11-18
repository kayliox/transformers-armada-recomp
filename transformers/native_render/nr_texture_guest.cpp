// (nr_core S7) The recomp's texture entry - glue on top of the core's host-memory entry (nr_texture.cpp convertHostTexture):
// a vlTexturePS2's levels read from guest memory (VL_TextureNative_PlayStation2.cpp: the levels at 0x98; a level's data pointer
// at +0x00, its format +0x04, size +0x0C / +0x10, bits per pixel +0x14, column width +0x1C), the snapshots of levels the game
// freed once uploaded, and the conversion workers' copies of a key's guest memory (TODO 164). Moved out of nr_texture.cpp as
// they were; what read the levels now fills a HostTexture, and the conversion is the core's.
#include "nr_types.h"

#include "nr_core.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace nr
{
    // (TODO 164) A key's guest memory as convertTexture reads it, copied at the draw: each level it touches - the
    // image, the palette, the game's mip levels - its object's first 0x20 bytes (data pointer, format, size, bpp,
    // tile) and its texels. A worker converts from it (t_inputs) while the game goes on writing its memory.
    struct TextureInputs
    {
        struct Level
        {
            uint32_t at = 0;
            uint8_t header[0x20] = {};
            std::vector<uint8_t> texels;
        };
        std::vector<Level> levels;
        const Level *find(uint32_t level) const
        {
            for (const Level &l : levels)
                if (l.at == level)
                    return &l;
            return nullptr;
        }
    };

    namespace
    {
        thread_local const TextureInputs *t_inputs = nullptr; // (a worker's conversion: guest memory from here)

        // (nr_core S1b: main memory comes from the host, nr_core::TextureHost)
        const uint8_t *guestPtr(const uint8_t *rdram, uint32_t addr)
        {
            const auto fn = nr_core::textureHost().guestPtr;
            return fn ? fn(rdram, addr) : nullptr;
        }
        // (nr_core S5: the movie pictures' questions, through the host as well)
        bool movieSkipsSnapshot(uint32_t data)
        {
            const auto fn = nr_core::textureHost().skipsSnapshot;
            return fn && fn(data);
        }

        uint32_t word(const uint8_t *rdram, uint32_t addr)
        {
            if (t_inputs)
            {
                uint32_t v = 0u;
                for (const TextureInputs::Level &l : t_inputs->levels)
                    if (addr >= l.at && addr + 4u <= l.at + sizeof(l.header))
                    {
                        std::memcpy(&v, l.header + (addr - l.at), 4);
                        break;
                    }
                return v;
            }
            const uint8_t *p = guestPtr(rdram, addr);
            uint32_t v = 0u;
            if (p)
                std::memcpy(&v, p, 4);
            return v;
        }

        // The bytes of [addr, addr + size) in guest memory (main RAM: contiguous).
        const uint8_t *bytes(const uint8_t *rdram, uint32_t addr, uint32_t size)
        {
            const uint8_t *first = guestPtr(rdram, addr);
            const uint8_t *last = size ? guestPtr(rdram, addr + size - 1u) : first;
            return first && last && last == first + size - 1u ? first : nullptr;
        }

        // Levels' texels as they were when uploaded, by level object (their data pointer then).
        struct Snapshot
        {
            uint32_t data = 0;
            std::vector<uint8_t> bytes;
            uint32_t generation = 0; // (its texels changed since the first upload, this many times)
        };
        std::unordered_map<uint32_t, Snapshot> g_snapshots;
        uint64_t g_snapshotEpoch = 0; // (TODO 94: a generation changed - snapshotEpoch)

        uint32_t levelSize(const uint8_t *rdram, uint32_t level)
        {
            const uint32_t uw = word(rdram, level + 0x0Cu), uh = word(rdram, level + 0x10u), bpp = word(rdram, level + 0x14u);
            if (uw == 0u || uh == 0u || uw > 2048u || uh > 2048u || (bpp != 4u && bpp != 8u && bpp != 16u && bpp != 24u && bpp != 32u))
                return 0u;
            return (uw * uh * bpp + 7u) / 8u;
        }

        // A level's texels: in main memory while its copy is there, else the snapshot of its upload.
        const uint8_t *levelBytes(const uint8_t *rdram, uint32_t level, uint32_t size)
        {
            if (t_inputs)
            {
                const TextureInputs::Level *l = t_inputs->find(level);
                return l && l->texels.size() >= size && size ? l->texels.data() : nullptr;
            }
            const uint32_t data = word(rdram, level);
            if (data)
                if (const uint8_t *live = bytes(rdram, data, size))
                    return live;
            auto it = g_snapshots.find(level);
            return it != g_snapshots.end() && it->second.bytes.size() >= size ? it->second.bytes.data() : nullptr;
        }
    }

    void rememberLevel(const uint8_t *rdram, uint32_t level)
    {
        if (!level)
            return;
        const uint32_t data = word(rdram, level), size = levelSize(rdram, level);
        if (!data || !size)
            return;
        // (Roadmap N6: a movie's picture drawn from the decoder - the game's copy of it not written, nothing to keep.)
        if (movieSkipsSnapshot(data))
            return;
        Snapshot &s = g_snapshots[level];
        if (s.data == data && s.bytes.size() == size)
        {
            // (The same upload again: refreshed only if the texels changed - a level's own data may be
            // rewritten in place, a palette animated, say.)
            const uint8_t *live = bytes(rdram, data, size);
            if (live && std::memcmp(live, s.bytes.data(), size) == 0)
                return;
        }
        if (const uint8_t *live = bytes(rdram, data, size))
        {
            if (!s.bytes.empty())
            {
                ++s.generation; // (uploaded again with other texels: its conversions are stale - TODO 58)
                ++g_snapshotEpoch;
            }
            s.data = data;
            s.bytes.assign(live, live + size);
        }
    }

    void rememberRewrite(const uint8_t *rdram, uint32_t level)
    {
        const uint32_t data = level ? word(rdram, level) : 0u, size = level ? levelSize(rdram, level) : 0u;
        const uint8_t *live = data && size ? bytes(rdram, data, size) : nullptr;
        if (!live)
            return;
        // (as an upload with other texels: stale whether or not one was seen before)
        Snapshot &s = g_snapshots[level];
        ++s.generation;
        ++g_snapshotEpoch;
        s.data = data;
        s.bytes.assign(live, live + size);
    }

    uint64_t snapshotEpoch() { return g_snapshotEpoch; }

    uint32_t levelGeneration(uint32_t level)
    {
        auto it = g_snapshots.find(level);
        return it != g_snapshots.end() ? it->second.generation : 0u;
    }

    std::shared_ptr<const Texture> convertPalette(const uint8_t *rdram, const TextureKey &key, uint64_t id)
    {
        if (!key.clut)
            return nullptr;
        const uint32_t clutEntries = word(rdram, key.clut + 0x0Cu) * word(rdram, key.clut + 0x10u);
        const uint8_t *c = clutEntries >= 256u ? levelBytes(rdram, key.clut, clutEntries * 4u) : nullptr;
        if (!c)
            return nullptr;
        return convertHostPalette(c, clutEntries, id);
    }

    namespace
    {
        // (nr_core S7) A level's object as the core's HostTexture::Level: its format, size, bits per pixel and column width
        // from the object, its texels where convertTexture read them (main memory, else the snapshot of its upload; a
        // worker's copy) - none when the object's size is not one (levelSize).
        HostTexture::Level readLevel(const uint8_t *rdram, uint32_t level)
        {
            HostTexture::Level l;
            l.psm = word(rdram, level + 0x04u);
            l.width = word(rdram, level + 0x0Cu);
            l.height = word(rdram, level + 0x10u);
            l.bpp = word(rdram, level + 0x14u);
            l.tile = word(rdram, level + 0x1Cu);
            if (const uint32_t size = levelSize(rdram, level))
                if ((l.texels = levelBytes(rdram, level, size)))
                    l.bytes = size;
            return l;
        }
    }

    std::shared_ptr<const Texture> convertTexture(const uint8_t *rdram, const TextureKey &key, uint64_t id)
    {
        if (!key.image)
            return nullptr;
        HostTexture in;
        in.psm = key.psm;
        in.width = key.width;
        in.height = key.height;
        in.tbw = key.tbw;
        in.csa = key.csa;
        in.source = key.texture;
        in.image = readLevel(rdram, key.image);
        if (key.clut)
        {
            in.clutEntries = word(rdram, key.clut + 0x0Cu) * word(rdram, key.clut + 0x10u);
            in.clut = levelBytes(rdram, key.clut, in.clutEntries * 4u);
        }
        // The game's own levels below the image (what the GS reads past level 0 under TEX1), up to the first without texels
        // in main memory (not read from GS memory).
        for (uint32_t k = 0; k < 3u && key.mip[k]; ++k)
        {
            const HostTexture::Level l = readLevel(rdram, key.mip[k]);
            if (!l.texels)
                break;
            in.mips[in.mipCount++] = l;
        }
        // (No texels in main memory: a top-byte image read from GS memory where TEX0 points, if the GS memory is known - the
        // light maps. Never on a worker: captureTextureInputs leaves those to the draw.)
        in.gsMemory = !t_inputs;
        in.tbp = key.tbp;
        return convertHostTexture(in, id);
    }

    std::shared_ptr<TextureInputs> captureTextureInputs(const uint8_t *rdram, const TextureKey &key)
    {
        if (!key.image || key.width == 0u || key.height == 0u)
            return nullptr;
        auto in = std::make_shared<TextureInputs>();
        // (a level: its header, and its texels when `size` - false when they are not there)
        auto add = [&](uint32_t level, uint32_t size) {
            if (!level || in->find(level))
                return true;
            const uint8_t *h = bytes(rdram, level, sizeof(TextureInputs::Level::header));
            if (!h)
                return false;
            TextureInputs::Level l;
            l.at = level;
            std::memcpy(l.header, h, sizeof(l.header));
            bool ok = true;
            if (size)
            {
                if (const uint8_t *t = levelBytes(rdram, level, size))
                    l.texels.assign(t, t + size);
                else
                    ok = false;
            }
            in->levels.push_back(std::move(l));
            return ok;
        };
        const uint32_t imageSize = levelSize(rdram, key.image);
        if (!imageSize || !add(key.image, imageSize))
            return nullptr; // (no texels in main memory: GS memory's, or none - at once, as before)
        if (key.clut)
        {
            const uint32_t entries = word(rdram, key.clut + 0x0Cu) * word(rdram, key.clut + 0x10u);
            if (entries && entries <= 65536u && !add(key.clut, entries * 4u))
                return nullptr;
            add(key.clut, 0u);
        }
        for (uint32_t k = 0; k < 3u && key.mip[k]; ++k)
            if (const uint32_t size = levelSize(rdram, key.mip[k]))
                add(key.mip[k], size); // (a level without texels: the conversion stops its mips there, as it did)
        return in;
    }

    std::shared_ptr<const Texture> convertTextureFrom(const TextureInputs &inputs, const TextureKey &key, uint64_t id)
    {
        const TextureInputs *before = t_inputs;
        t_inputs = &inputs;
        auto out = convertTexture(nullptr, key, id);
        t_inputs = before;
        return out;
    }

}
