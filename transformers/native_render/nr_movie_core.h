// The movies' pictures drawn from the decoder (nr_movie_core.cpp, in the core - nr_core S4; roadmap N6): the pictures the
// host's decoder serves, kept by an opaque buffer id (the recomp: the game's buffer address with its segment bits off), each
// made into a texture of the key's size when a texture whose level holds that buffer is converted; the HQ level preview's
// upscaled copy drawn in its place (TODO 159); the movie log (TF_NATIVE_MOVIELOG). No guest memory, no decoder of its own:
// what it needs of the host comes through nr::MovieHost. The recomp's glue (nr_movie.cpp) hooks the runtime's decoder to it
// and keeps nr_movie.h's API.
#pragma once

#include "nr_types.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nr
{
    // (TODO 159) The HQ level preview's upscaled copy, decoded by the host (the recomp: an ffmpeg of its own,
    // hd_movies_decoder). Made for each preview, let go of when it ends (its decoding stopped - outside the core's lock).
    struct MoviePreviewSource
    {
        virtual ~MoviePreviewSource() = default;
        // Decoding from the start at this size, the frame's time = its number; started off the caller's thread (the game's
        // thread never waits for it), an earlier start finished first.
        virtual void start(uint32_t width, uint32_t height) = 0;
        // That decoding started.
        virtual bool ready() const = 0;
        // The newest frame whose number has come (false: none new since): its texels (RGBA8, one word each) and number.
        virtual bool take(double index, std::vector<uint32_t> &rgba, double &time) = 0;
    };

    // (the log) The host's time spent on movie pictures since the last call: decoding, the YUV -> RGB conversion of it, the
    // copies into the game's buffers; the pictures served and copied.
    struct MovieHostCosts
    {
        double decodeMs = 0.0, convertMs = 0.0, guestCopyMs = 0.0;
        uint32_t picturesServed = 0, guestCopies = 0;
    };

    // What the movie pictures need of the host. Set once before the first picture (a question left null: no / nothing).
    struct MovieHost
    {
        // The emulated drawing on (a lost device): the host's own copy of each picture is still written.
        bool (*emulatedDrawing)() = nullptr;
        // (N5, TF_N5_SETCACHE) The geometry-set hook's caches on: holds() answers without the lock while no picture is kept.
        bool (*setCacheOn)() = nullptr;
        // (the log) The host's time on the pictures since the last call.
        MovieHostCosts (*takeCosts)() = nullptr;
        // (TODO 159) The preview's upscaled copy from its file (null: the game-size pictures drawn).
        std::unique_ptr<MoviePreviewSource> (*openPreview)(const std::string &file) = nullptr;
        // (TF_NATIVE_MOVIES=check) The host's copy of the picture converted as before - `source` as the caller of texture()
        // gave it (the recomp: guest RAM).
        std::shared_ptr<const Texture> (*checkConvert)(const uint8_t *source, const TextureKey &key) = nullptr;
    };
    void setMovieHost(const MovieHost &host);

    namespace movie_core
    {
        enum class Mode
        {
            Off,     // the old path (no picture handed over)
            Observe, // the old path, the pictures' buffers known for the log's timings (TF_NATIVE_MOVIES=0 with the log)
            Native,  // drawn from the decoder
            Check,   // drawn from the decoder and compared with the host's copy
        };
        // Once, before the first picture.
        void setMode(Mode mode, bool log);

        // A picture the decoder served (stream = the decoder, buffer = where the game asked for it), or with rgba null: that
        // decoder deleted. True when the host's own copy need not be written (drawn from here alone).
        struct DecodedPicture
        {
            uint32_t stream = 0, buffer = 0;
            uint32_t width = 0, height = 0;
            uint32_t index = 0;   // its number in its stream (0, 1, 2 ...)
            int64_t pts90k = -1;  // its presentation time (90 kHz), -1 when the stream gives none
            uint64_t tick = 0;    // the VBlank it was served at
            const uint8_t *rgba = nullptr; // width x height RGBA, R first
        };
        bool submit(const DecodedPicture &picture);
        // (the log) One sound block sent by the movie's player (0x2000 bytes of 48 kHz 16-bit stereo).
        void audioBlockSent();

        // nr_movie.h's questions, by buffer id (the glue maps the game's addresses).
        bool holds(uint32_t texture, uint32_t buffer, uint64_t &generation);
        std::shared_ptr<const Texture> texture(const uint8_t *checkSource, const TextureKey &key, uint32_t buffer, uint64_t id);
        bool skipsSnapshot(uint32_t buffer);
        void textureGone(uint32_t texture);
        void oldConversion(uint32_t buffer, double ms);
        void previewStarted(const std::string &key, const std::string &hdFile);
        void previewEnded();
    }
}
