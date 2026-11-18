// The movies' pictures drawn from the decoder (nr_movie_core.h; in the core - nr_core S4, roadmap N6).
//
// The decoder hands each picture over as it is served (the recomp: ps2_mpeg_native.h, PS2Recomp patch 0072, through
// nr_movie.cpp): kept by its buffer id, and when a texture whose level holds that buffer is converted the picture is made
// into the texture directly - the texture's area past the picture holding its last row and column. The host's own copy is
// no longer written while the native renderer draws alone (when the emulated drawing is on - a lost device - it still is).
// The modes, the colour and the log: nr_movie.cpp (TF_NATIVE_MOVIES, TF_MOVIE_COLOUR, TF_NATIVE_MOVIELOG).

#include "nr_movie_core.h"
#include "nr_types.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace nr
{
    namespace
    {
        MovieHost g_movieHost;
    }

    void setMovieHost(const MovieHost &host) { g_movieHost = host; }
}

namespace nr::movie_core
{
    namespace
    {
        // (TODO 159) The HQ's level preview: its pictures drawn from an upscaled copy of the movie (movies_hd/LEVELS/
        // <name>.mkv, tools/upscale_previews.py - 4x, the same frames), frame N for the game's picture N. The copy is
        // decoded by the host (MoviePreviewSource: the recomp's ffmpeg of its own, started and restarted - each loop of the
        // preview - on a thread of its own, so the game's thread never waits for it); a picture whose frame is not there
        // yet is drawn from the game's picture, as before. The large texture goes up through the large images' uploader
        // with the game-size one as its fallback (nr_d3d.cpp). TF_HD_PREVIEWS=0 (overrides.cpp): no file given - as before.
        struct Preview
        {
            std::string key, file;
            std::unique_ptr<MoviePreviewSource> source;
            uint32_t width = 0, height = 0; // the copy's frames: the picture's size times 4
            uint32_t lastIndex = ~0u;
            int64_t cachedIndex = -1;
            std::vector<uint32_t> cached;
            // (the texture made last, by the game picture's serial: asked for again, given again - the copy's frame is moved
            // into a texture, not copied)
            std::shared_ptr<Texture> last;
            uint64_t lastSerial = 0;
            void start(uint32_t w, uint32_t h)
            {
                cachedIndex = -1;
                width = w;
                height = h;
                // (the frame's time = its number: fps 1)
                source->start(w, h);
            }
        };
        std::unique_ptr<Preview> g_preview; // (under g_mutex)
        struct PreviewCounts
        {
            uint32_t drawn = 0, fromGame = 0, sampled = 0;
            double buildMs = 0.0, error = 0.0;
        };
        PreviewCounts g_previewCounts;
        Mode g_mode = Mode::Off;
        bool g_log = false;

        struct Picture
        {
            uint32_t mpegAddr = 0;
            uint32_t texture = 0; // the vlTexturePS2 first seen drawing it (another texture at the same address is not it)
            uint32_t width = 0, height = 0;
            uint64_t serial = 0;  // this port's number of the picture (one for each picture served, never 0)
            uint32_t index = 0;   // its number in its stream (the decoder's)
            bool guestWritten = true;
            std::vector<uint8_t> rgba;
        };
        std::mutex g_mutex;
        std::unordered_map<uint32_t, Picture> g_pictures; // by the game's buffer (the buffer id)
        // (N5, TF_N5_SETCACHE) How many it holds, for holds() to answer without the lock while no movie plays (every
        // textured draw asks). Kept with each change, under g_mutex.
        std::atomic<size_t> g_pictureCount{0};
        uint64_t g_nextSerial = 1;

        // The log (TF_NATIVE_MOVIELOG): per period, and per movie (by its decoder).
        struct Period
        {
            uint32_t served = 0, built = 0, notDrawn = 0, builtTwice = 0, indexGaps = 0;
            uint64_t minSpacing = ~0ull, maxSpacing = 0;
            double buildMs = 0.0, oldConvertMs = 0.0;
            uint32_t oldConversions = 0, checked = 0, checkDiffer = 0, checkMax = 0;
        };
        Period g_period;
        std::chrono::steady_clock::time_point g_periodStart = std::chrono::steady_clock::now();
        struct Stream
        {
            uint32_t mpegAddr = 0, lastIndex = ~0u, served = 0;
            uint64_t firstTick = 0, lastTick = 0, lastBuiltSerial = 0;
            uint32_t lastBuiltIndex = 0;
            int64_t firstPts = -1, lastPts = -1;
            uint32_t audioBlocksAtFirst = 0;
            double worstAudioLeadMs = -1e9, leastAudioLeadMs = 1e9;
            uint32_t notDrawn = 0, builtTwice = 0, built = 0;
        };
        std::unordered_map<uint32_t, Stream> g_streams;
        uint32_t g_audioBlocks = 0; // the sound blocks sent to the sound server (audioBlockSent), since the start

        double msSince(std::chrono::steady_clock::time_point t)
        {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
        }

        void report(bool force)
        {
            if (!g_log)
                return;
            const double secs = msSince(g_periodStart) / 1000.0;
            if (!force && secs < 2.0)
                return;
            if (g_period.served || g_period.built || g_period.oldConversions)
            {
                const MovieHostCosts c = g_movieHost.takeCosts ? g_movieHost.takeCosts() : MovieHostCosts{};
                const double per = c.picturesServed ? 1.0 / c.picturesServed : 0.0;
                std::fprintf(stderr,
                             "[movie] %.1f s: %u pictures served (VBlanks apart %llu..%llu, %u index gaps), %u drawn from the decoder (%u not "
                             "drawn, %u made twice); host ms a picture: decode %.2f (of it YUV->RGB %.2f), game copy %.2f (%u copies), "
                             "texture from the decoder %.2f, from the game's copy %.2f (%u)%s\n",
                             secs, g_period.served, static_cast<unsigned long long>(g_period.minSpacing == ~0ull ? 0 : g_period.minSpacing),
                             static_cast<unsigned long long>(g_period.maxSpacing), g_period.indexGaps, g_period.built, g_period.notDrawn,
                             g_period.builtTwice, c.decodeMs * per, c.convertMs * per, c.guestCopies ? c.guestCopyMs / c.guestCopies : 0.0,
                             c.guestCopies, g_period.built ? g_period.buildMs / g_period.built : 0.0,
                             g_period.oldConversions ? g_period.oldConvertMs / g_period.oldConversions : 0.0, g_period.oldConversions,
                             g_mode == Mode::Check ? (" check: " + std::to_string(g_period.checked) + " compared, " + std::to_string(g_period.checkDiffer) +
                                                      " differing pixels, most " + std::to_string(g_period.checkMax) + " levels")
                                                         .c_str()
                                                   : "");
            }
            g_period = Period{};
            g_periodStart = std::chrono::steady_clock::now();
        }

        void streamEnded(const Stream &s)
        {
            if (!g_log)
                return;
            const double ticks = static_cast<double>(s.lastTick - s.firstTick);
            const double ptsSecs = s.firstPts >= 0 && s.lastPts >= 0 ? static_cast<double>(s.lastPts - s.firstPts) / 90000.0 : -1.0;
            std::fprintf(stderr,
                         "[movie] movie on decoder %08x ended: %u pictures over %.0f VBlanks (%.2f a picture; their own times %.2f s), %u drawn, %u not "
                         "drawn, %u made twice; sound ahead of the picture %.0f..%.0f ms (%u blocks)\n",
                         s.mpegAddr, s.served, ticks, s.served > 1 ? ticks / (s.served - 1) : 0.0, ptsSecs, s.built, s.notDrawn, s.builtTwice,
                         s.leastAudioLeadMs > 1e8 ? 0.0 : s.leastAudioLeadMs, s.worstAudioLeadMs < -1e8 ? 0.0 : s.worstAudioLeadMs,
                         g_audioBlocks - s.audioBlocksAtFirst);
        }
    }

    void setMode(Mode mode, bool log)
    {
        g_mode = mode;
        g_log = log;
    }

    void audioBlockSent() { ++g_audioBlocks; }

    bool submit(const DecodedPicture &p)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!p.rgba)
        {
            // (the decoder deleted: its pictures are no longer drawn from here)
            for (auto it = g_pictures.begin(); it != g_pictures.end();)
                it = it->second.mpegAddr == p.stream ? g_pictures.erase(it) : std::next(it);
            g_pictureCount.store(g_pictures.size(), std::memory_order_release);
            if (auto s = g_streams.find(p.stream); s != g_streams.end())
            {
                report(true);
                streamEnded(s->second);
                g_streams.erase(s);
            }
            return false;
        }
        Picture &pic = g_pictures[p.buffer];
        g_pictureCount.store(g_pictures.size(), std::memory_order_release);
        if (pic.mpegAddr != p.stream)
            pic = Picture{};
        pic.mpegAddr = p.stream;
        pic.width = p.width;
        pic.height = p.height;
        pic.serial = g_nextSerial++;
        pic.index = p.index;
        const bool drawn = g_mode == Mode::Native || g_mode == Mode::Check;
        if (drawn)
            pic.rgba.assign(p.rgba, p.rgba + static_cast<size_t>(p.width) * p.height * 4u);
        // (the game's copy still written: the old path, the check, or the emulated GS drawing - a lost device)
        pic.guestWritten = g_mode != Mode::Native || (g_movieHost.emulatedDrawing && g_movieHost.emulatedDrawing());

        if (g_log)
        {
            Stream &s = g_streams[p.stream];
            if (s.served != 0 && p.index <= s.lastIndex)
            {
                // (the picture count back at the start: another movie on the same sceMpeg object - the game keeps one
                // from movie to movie - or the same one looping)
                report(true);
                streamEnded(s);
                s = Stream{};
            }
            const double audioMs = static_cast<double>(g_audioBlocks - s.audioBlocksAtFirst) * 2048.0 / 48.0;
            if (s.served == 0)
            {
                s.mpegAddr = p.stream;
                s.firstTick = p.tick;
                s.firstPts = p.pts90k;
                s.audioBlocksAtFirst = g_audioBlocks;
            }
            else
            {
                const uint64_t spacing = p.tick - s.lastTick;
                g_period.minSpacing = std::min(g_period.minSpacing, spacing);
                g_period.maxSpacing = std::max(g_period.maxSpacing, spacing);
                if (p.index != s.lastIndex + 1u)
                    ++g_period.indexGaps;
                // The sound sent against this picture's time since the first (its own time when the stream gives one,
                // else its place at 29.97 a second): how far the sound runs ahead of the picture.
                const double pictureMs = p.pts90k >= 0 && s.firstPts >= 0 ? static_cast<double>(p.pts90k - s.firstPts) / 90.0
                                                                          : s.served * 1001.0 / 30.0;
                if (g_audioBlocks != s.audioBlocksAtFirst)
                {
                    s.worstAudioLeadMs = std::max(s.worstAudioLeadMs, audioMs - pictureMs);
                    s.leastAudioLeadMs = std::min(s.leastAudioLeadMs, audioMs - pictureMs);
                }
            }
            s.lastTick = p.tick;
            s.lastPts = p.pts90k;
            s.lastIndex = p.index;
            ++s.served;
            ++g_period.served;
            report(false);
        }
        return !pic.guestWritten;
    }

    bool holds(uint32_t texture, uint32_t buffer, uint64_t &generation)
    {
        if (g_mode != Mode::Native && g_mode != Mode::Check)
            return false;
        if (g_pictureCount.load(std::memory_order_acquire) == 0u && g_movieHost.setCacheOn && g_movieHost.setCacheOn())
            return false; // (none: as the find below would answer)
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_pictures.find(buffer);
        if (it == g_pictures.end() || it->second.rgba.empty())
            return false;
        Picture &pic = it->second;
        if (pic.texture == 0u)
            pic.texture = texture;
        else if (pic.texture != texture)
            return false;
        generation = 1ull << 63 | pic.serial;
        return true;
    }

    // (TODO 159, under g_mutex) The level preview's picture from its upscaled copy: the texture of the key's size times
    // the copy's scale (the picture's texels at the top left, its last row and column held past them, as the game-size
    // one), marked large, with the game-size one as its fallback; null while the copy's frame for this picture is not
    // there (the game-size one drawn).
    static std::shared_ptr<Texture> previewTexture(const Picture &pic, const TextureKey &key, uint64_t id, uint32_t pw, uint32_t ph,
                                                   const std::shared_ptr<Texture> &game)
    {
        Preview *pv = g_preview.get();
        if (!pv || pv->file.empty() || !pv->source || pic.width == 0u || pic.height == 0u)
            return nullptr;
        const auto start = std::chrono::steady_clock::now();
        constexpr uint32_t kScale = 4u;
        const uint32_t fw = pic.width * kScale, fh = pic.height * kScale;
        // (the first picture, a new size, or the picture count back at the start - the preview looping: from the start)
        if (pv->width != fw || pv->height != fh || (pv->lastIndex != ~0u && pic.index < pv->lastIndex))
            pv->start(fw, fh);
        pv->lastIndex = pic.index;
        if (pv->last && pv->lastSerial == pic.serial)
            return pv->last; // (the same picture asked for again)
        if (pv->cachedIndex != static_cast<int64_t>(pic.index) && pv->source->ready())
        {
            std::vector<uint32_t> frame;
            double time = 0.0;
            if (pv->source->take(static_cast<double>(pic.index), frame, time) && frame.size() == static_cast<size_t>(fw) * fh)
            {
                pv->cached = std::move(frame);
                pv->cachedIndex = static_cast<int64_t>(time + 0.5);
            }
        }
        if (pv->cachedIndex != static_cast<int64_t>(pic.index))
        {
            ++g_previewCounts.fromGame;
            return nullptr;
        }
        if (g_log)
        {
            // (the copy's frame against the game's picture, 4x4 blocks averaged at 64 places: a frame off would show here)
            const uint8_t *hd = reinterpret_cast<const uint8_t *>(pv->cached.data());
            double sum = 0.0;
            for (uint32_t k = 0; k < 64u; ++k)
            {
                const uint32_t x = (k % 8u) * (pw / 8u) + pw / 16u, y = (k / 8u) * (ph / 8u) + ph / 16u;
                for (int c = 0; c < 3; ++c)
                {
                    double avg = 0.0;
                    for (uint32_t dy = 0; dy < kScale; ++dy)
                        for (uint32_t dx = 0; dx < kScale; ++dx)
                            avg += hd[((static_cast<size_t>(y) * kScale + dy) * fw + x * kScale + dx) * 4u + c];
                    sum += std::abs(avg / (kScale * kScale) - pic.rgba[(static_cast<size_t>(y) * pic.width + x) * 4u + c]);
                }
            }
            g_previewCounts.error += sum / (64.0 * 3.0);
            ++g_previewCounts.sampled;
        }
        const uint32_t w = key.width * kScale, h = key.height * kScale, cw = pw * kScale, ch = ph * kScale;
        auto out = std::make_shared<Texture>();
        out->id = id ^ (1ull << 61); // (its own: the GPU keys its copy by the id - the game-size one's stays its fallback's)
        out->source = key.texture;
        out->width = w;
        out->height = h;
        out->upscaled = true;
        out->fallback = game;
        if (w == fw && h == fh)
        {
            out->pixels = std::move(pv->cached); // (the texture the picture's size: the frame itself, no copy - its alpha 255 already)
            pv->cached.clear();
            pv->cachedIndex = -1; // (the frame given away: this picture again comes as `last`)
        }
        else
        {
            out->pixels.resize(static_cast<size_t>(w) * h);
            for (uint32_t y = 0; y < h; ++y)
            {
                uint32_t *row = out->pixels.data() + static_cast<size_t>(y) * w;
                std::memcpy(row, pv->cached.data() + static_cast<size_t>(std::min(y, ch - 1u)) * fw, static_cast<size_t>(cw) * 4u);
                if (cw < w)
                    std::fill(row + cw, row + w, row[cw - 1u]);
            }
        }
        pv->last = out;
        pv->lastSerial = pic.serial;
        ++g_previewCounts.drawn;
        g_previewCounts.buildMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        return out;
    }

    std::shared_ptr<const Texture> texture(const uint8_t *checkSource, const TextureKey &key, uint32_t buffer, uint64_t id)
    {
        const auto start = std::chrono::steady_clock::now();
        std::shared_ptr<Texture> preview;
        std::shared_ptr<Texture> out;
        uint64_t serial = 0;
        uint32_t pw = 0, ph = 0, index = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_pictures.find(buffer);
            if (it == g_pictures.end() || it->second.rgba.empty() || key.width == 0u || key.height == 0u || key.width > 2048u ||
                key.height > 2048u)
                return nullptr;
            const Picture &pic = it->second;
            serial = pic.serial;
            index = pic.index;
            pw = std::min(pic.width, key.width);
            ph = std::min(pic.height, key.height);
            if (pw == 0u || ph == 0u)
                return nullptr;
            out = std::make_shared<Texture>();
            out->id = id;
            out->source = key.texture;
            out->width = key.width;
            out->height = key.height;
            out->pixels.resize(static_cast<size_t>(key.width) * key.height);
            for (uint32_t y = 0; y < key.height; ++y)
            {
                uint32_t *row = out->pixels.data() + static_cast<size_t>(y) * key.width;
                const uint8_t *src = pic.rgba.data() + static_cast<size_t>(std::min(y, ph - 1u)) * pic.width * 4u;
                std::memcpy(row, src, static_cast<size_t>(pw) * 4u);
                for (uint32_t x = 0; x < pw; ++x)
                    row[x] |= 0xFF000000u; // (alpha 1: the game's copy says 0x80, the GS's 1.0)
                std::fill(row + pw, row + key.width, row[pw - 1u]);
            }
            preview = previewTexture(pic, key, id, pw, ph, out);
            if (g_log)
            {
                Stream &s = g_streams[pic.mpegAddr];
                if (s.lastBuiltSerial != 0 && serial == s.lastBuiltSerial)
                {
                    ++s.builtTwice;
                    ++g_period.builtTwice;
                }
                else if (s.lastBuiltSerial != 0 && index > s.lastBuiltIndex + 1u)
                {
                    // (pictures of this movie served between the two drawn, never drawn; a picture number going back = the
                    // movie started again - a loop)
                    s.notDrawn += index - s.lastBuiltIndex - 1u;
                    g_period.notDrawn += index - s.lastBuiltIndex - 1u;
                }
                s.lastBuiltSerial = serial;
                s.lastBuiltIndex = index;
                ++s.built;
                ++g_period.built;
                g_period.buildMs += msSince(start);
            }
        }
        if (g_mode == Mode::Check)
        {
            // The game's copy converted as before: the same texels in the picture's area expected.
            if (auto old = g_movieHost.checkConvert ? g_movieHost.checkConvert(checkSource, key) : nullptr;
                old && old->pixels.size() == out->pixels.size())
            {
                uint32_t differ = 0, most = 0;
                for (uint32_t y = 0; y < ph; ++y)
                    for (uint32_t x = 0; x < pw; ++x)
                    {
                        const size_t i = static_cast<size_t>(y) * key.width + x;
                        const uint32_t a = out->pixels[i], b = old->pixels[i];
                        if (a == b)
                            continue;
                        ++differ;
                        for (int k = 0; k < 4; ++k)
                            most = std::max<uint32_t>(most, static_cast<uint32_t>(std::abs(static_cast<int>((a >> (8 * k)) & 0xFFu) -
                                                                                              static_cast<int>((b >> (8 * k)) & 0xFFu))));
                    }
                std::lock_guard<std::mutex> lock(g_mutex);
                ++g_period.checked;
                g_period.checkDiffer += differ;
                g_period.checkMax = std::max(g_period.checkMax, most);
                static int told = 0;
                if (differ && told < 5)
                {
                    ++told;
                    std::fprintf(stderr, "[movie] check: picture %llu (%ux%u in %ux%u) differs from the game's copy at %u pixels, by up to %u\n",
                                 static_cast<unsigned long long>(serial), pw, ph, key.width, key.height, differ, most);
                }
            }
        }
        if (preview)
            return preview;
        return out;
    }

    void previewStarted(const std::string &key, const std::string &hdFile)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_preview.reset();
        if (hdFile.empty() || (g_mode != Mode::Native && g_mode != Mode::Check))
        {
            std::fprintf(stderr, "[movie] level preview %s: the game's pictures (%s)\n", key.c_str(),
                         hdFile.empty() ? "no upscaled copy, or TF_HD_PREVIEWS=0" : "TF_NATIVE_MOVIES off");
            return;
        }
        g_preview = std::make_unique<Preview>();
        g_preview->key = key;
        g_preview->file = hdFile;
        if (g_movieHost.openPreview)
            g_preview->source = g_movieHost.openPreview(hdFile);
        g_previewCounts = PreviewCounts{};
        std::fprintf(stderr, "[movie] level preview %s: drawn from %s (TODO 159; TF_HD_PREVIEWS=0: the game's)\n", key.c_str(), hdFile.c_str());
    }

    void previewEnded()
    {
        std::unique_ptr<Preview> gone;
        PreviewCounts counts;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            gone = std::move(g_preview);
            counts = g_previewCounts;
            g_previewCounts = PreviewCounts{};
        }
        if (!gone)
            return;
        std::fprintf(stderr, "[movie] level preview %s ended: %u pictures from the upscaled copy (%.2f ms each to build), %u from the game's "
                             "(the copy's frame not there yet)%s\n",
                     gone->key.c_str(), counts.drawn, counts.drawn ? counts.buildMs / counts.drawn : 0.0, counts.fromGame,
                     counts.sampled ? (", the copy against the game's picture: " + std::to_string(counts.error / counts.sampled) + " levels on average").c_str() : "");
        gone.reset(); // (its decoding stopped - outside the lock)
    }

    void textureGone(uint32_t texture)
    {
        if (g_mode == Mode::Off || texture == 0u)
            return;
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto it = g_pictures.begin(); it != g_pictures.end();)
        {
            if (it->second.texture != texture)
            {
                ++it;
                continue;
            }
            if (auto s = g_streams.find(it->second.mpegAddr); s != g_streams.end())
            {
                report(true);
                streamEnded(s->second);
                g_streams.erase(s);
            }
            it = g_pictures.erase(it);
        }
        g_pictureCount.store(g_pictures.size(), std::memory_order_release);
    }

    bool skipsSnapshot(uint32_t buffer)
    {
        if (g_mode != Mode::Native)
            return false;
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_pictures.find(buffer);
        return it != g_pictures.end() && !it->second.guestWritten;
    }

    void oldConversion(uint32_t buffer, double ms)
    {
        if (!g_log || (g_mode != Mode::Observe && g_mode != Mode::Native && g_mode != Mode::Check))
            return;
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_pictures.find(buffer) == g_pictures.end())
            return;
        ++g_period.oldConversions;
        g_period.oldConvertMs += ms;
    }
}
