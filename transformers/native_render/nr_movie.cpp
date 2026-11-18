// Roadmap N6: the movies' pictures drawn by the native renderer straight from the decoder (nr_movie.h).
//
// The game plays its movies (the logos, the intro and level films, the HQ's level previews, the Drop Zones' warp gate)
// through an fmvStreamPS2 (0x276110), which asks Sony's libmpeg for each picture (sceMpegGetPicture, 0x133F40) into
// the bitmap of a 32-bit vlTexturePS2 and then has that texture uploaded again (0x266570). The runtime replaces libmpeg
// with FFmpeg (PS2Recomp's Kernel/Stubs/MPEG.cpp): it decodes each picture to RGBA and wrote it into the game's buffer
// in the IPU's layout (16-pixel macroblock columns); the native renderer then converted that copy back - the level
// laid out as the GS stores it, read out again - for every picture, a 4 MB pass on the game's thread.
//
// Here the decoded picture comes to the renderer as it is served (ps2_mpeg_native.h, PS2Recomp patch 0072): kept by
// the game's buffer address, and when a texture whose level holds that buffer is converted (nr_capture.cpp
// convertKey) the picture is made into the texture directly - at the decoder's accurate YUV -> RGB conversion (full
// chroma interpolation), the texture's area past the picture holding its last row and column. The game's copy is no
// longer written while the native renderer draws alone (the emulated GS, when it draws - a lost device - still gets
// it). Nothing in the game's movie logic changes: the same calls, the same pictures at the same VBlanks, the sound
// untouched (start, skip, end, the per-character films, the previews' own pace - TODO 113's).
//
//   TF_NATIVE_MOVIES   1 (default): as above; 0: the old path (the game's copy converted); check: both - the copy
//                      written and converted too, and compared with the decoder's picture (the colour conversion then
//                      the old one unless TF_MOVIE_COLOUR=accurate)
//   TF_MOVIE_COLOUR    accurate (default) / old: the decoder's YUV -> RGB conversion (bilinear, as before)
//   TF_NATIVE_MOVIELOG 1: every 2 s while pictures come, and at each movie's end: pictures served and drawn (none
//                      left out, none made twice), their spacing in VBlanks, the sound blocks the player sent against
//                      the pictures' time, and the host time per picture (decoding, the copy, the texture).
//
// (nr_core S4) The picture store, the textures, the preview's bookkeeping and the log are the core's (nr_movie_core.cpp);
// this file is the recomp's glue: the runtime's decoder hooked to the core, the sound blocks counted, the game's addresses
// mapped to the core's buffer ids, the preview's upscaled copy decoded by an ffmpeg of its own.

#include "nr_movie.h"
#include "nr_movie_core.h"
#include "nr_types.h"

#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_host_display.h"
#include "runtime/ps2_mpeg_native.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "hd_movies_decoder.h"

namespace nr::movie
{
    namespace
    {
        // (TODO 159) The HQ's level preview's upscaled copy (movies_hd/LEVELS/<name>.mkv), decoded by an ffmpeg of its own
        // (hd_movies_decoder: started and restarted - each loop of the preview - on a thread of its own, so the game's
        // thread never waits for it).
        struct HdPreview final : MoviePreviewSource
        {
            std::string file;
            hd_movies_decoder::Decoder decoder;
            std::thread starter;
            std::atomic<bool> started{false};
            ~HdPreview() override
            {
                if (starter.joinable())
                    starter.join();
                decoder.stop();
            }
            void start(uint32_t w, uint32_t h) override
            {
                if (starter.joinable())
                    starter.join();
                started = false;
                // (the frame's time = its number: fps 1)
                starter = std::thread([this, w, h] { started = decoder.start(file, static_cast<int>(w), static_cast<int>(h), 1.0); });
            }
            bool ready() const override { return started; }
            bool take(double index, std::vector<uint32_t> &rgba, double &time) override
            {
                hd_movies_decoder::Frame f;
                if (!decoder.take(index, f))
                    return false;
                rgba = std::move(f.rgba);
                time = f.time;
                return true;
            }
        };

        PS2Runtime *g_runtime = nullptr;

        constexpr uint32_t kRamMask = PS2_RAM_MASK; // (a buffer's address with its segment bits off: kseg / uncached)

        // The sound streams' data sent to the sound server (slMusicStream::SendStreamData, 0x1EB950: a movie's player sends
        // one 0x2000-byte block of 48 kHz 16-bit stereo - 2,048 sample frames, 42.67 ms - a call, and only when the server
        // has played one, so the blocks sent run a fixed few ahead of the sound heard), counted for the sync line. (The
        // previews of the HQ have no sound of their own: what is counted there is the HQ's music.)
        constexpr uint32_t kAudioFeed = 0x001EB950u;
        PS2Runtime::RecompiledFunction g_audioFeed = nullptr;
        void audioFeed(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kAudioFeed)
                movie_core::audioBlockSent();
            g_audioFeed(rdram, ctx, runtime);
        }

        bool sink(const ps2_stubs::MpegNativePicture &p)
        {
            movie_core::DecodedPicture d;
            d.stream = p.mpegAddr;
            d.buffer = p.imageAddr & kRamMask;
            d.width = p.width;
            d.height = p.height;
            d.index = p.index;
            d.pts90k = p.pts90k;
            d.tick = p.vsyncTick;
            d.rgba = p.rgba;
            return movie_core::submit(d);
        }

        // The core's questions (nr_movie_core.h), answered from the runtime.
        const bool g_coreMovieHost = []
        {
            MovieHost host;
            host.emulatedDrawing = []() -> bool { return ps2_host_display::emulatedDrawing(); };
            host.setCacheOn = []() -> bool { return nr::setCacheOn(); };
            host.takeCosts = []() -> MovieHostCosts
            {
                const ps2_stubs::MpegHostCosts c = ps2_stubs::takeMpegHostCosts();
                MovieHostCosts out;
                out.decodeMs = c.decodeMs;
                out.convertMs = c.convertMs;
                out.guestCopyMs = c.guestCopyMs;
                out.picturesServed = c.picturesServed;
                out.guestCopies = c.guestCopies;
                return out;
            };
            host.openPreview = [](const std::string &file) -> std::unique_ptr<MoviePreviewSource>
            {
                auto source = std::make_unique<HdPreview>();
                source->file = file;
                return source;
            };
            host.checkConvert = [](const uint8_t *rdram, const TextureKey &key) -> std::shared_ptr<const Texture>
            { return convertTexture(rdram, key, 0u); };
            setMovieHost(host);
            return true;
        }();
    }

    void install(PS2Runtime &runtime)
    {
        g_runtime = &runtime;
        const char *m = std::getenv("TF_NATIVE_MOVIES");
        const std::string mode = m ? m : "1";
        const char *log = std::getenv("TF_NATIVE_MOVIELOG");
        const bool logged = log && *log && *log != '0';
        using Mode = movie_core::Mode;
        const Mode chosen = mode == "0" ? (logged ? Mode::Observe : Mode::Off) : mode == "check" ? Mode::Check : Mode::Native;
        movie_core::setMode(chosen, logged);
        const char *colour = std::getenv("TF_MOVIE_COLOUR");
        // (the check compares with the old conversion's copy unless asked otherwise: the same texels expected)
        const bool accurate = colour ? std::string(colour) == "accurate" : (chosen == Mode::Native);
        ps2_stubs::setMpegAccurateColour(accurate);
        if (chosen != Mode::Off)
            ps2_stubs::setMpegNativePictureSink(sink);
        if (logged)
        {
            g_audioFeed = runtime.lookupFunction(kAudioFeed);
            if (g_audioFeed)
                runtime.registerFunction(kAudioFeed, audioFeed);
        }
        std::fprintf(stderr, "[native render] movies: %s, colour conversion %s%s (roadmap N6; TF_NATIVE_MOVIES, TF_MOVIE_COLOUR)\n",
                     chosen == Mode::Native  ? "drawn from the decoder"
                     : chosen == Mode::Check ? "drawn from the decoder, checked against the game's copy"
                                             : "the game's copy converted (the old path)",
                     accurate ? "accurate" : "as before", logged ? ", logged" : "");
    }

    bool holds(uint32_t texture, uint32_t data, uint64_t &generation) { return movie_core::holds(texture, data & kRamMask, generation); }

    std::shared_ptr<const Texture> texture(const uint8_t *rdram, const TextureKey &key, uint32_t data, uint64_t id)
    {
        return movie_core::texture(rdram, key, data & kRamMask, id);
    }

    void previewStarted(const std::string &key, const std::string &hdFile) { movie_core::previewStarted(key, hdFile); }

    void previewEnded() { movie_core::previewEnded(); }

    void textureGone(uint32_t texture) { movie_core::textureGone(texture); }

    bool skipsSnapshot(uint32_t data) { return movie_core::skipsSnapshot(data & kRamMask); }

    void oldConversion(uint32_t data, double ms) { movie_core::oldConversion(data & kRamMask, ms); }
}
