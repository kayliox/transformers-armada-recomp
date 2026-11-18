// nr_core S5: the core's smoke test - a program that links ONLY the renderer's core (every object of nr_core_objects, nothing of
// the recomp: no runtime, no guest memory, no raylib) and the system's graphics libraries. Not part of the player build:
// `cmake --build . --config RelWithDebInfo --target nr_core_smoke` in a build folder, then run nr_core_smoke.exe from a folder it
// may write to (native_render/README.md). Exit 0: SMOKE OK.
//
// 1. The core opens (nr_core::open, the default settings) and its device starts, on a host that sets none of the recomp's hooks.
// 2. A known draw stream through gfx::Device, as the frame builder records: a frame of 256 x 144, its picture target cleared to a
//    known colour, a known pattern uploaded as a texture and copied into the picture's left half by the tone mapping's pass at
//    strength 0 (an exact copy: the shader Loads the texel at the pixel, writes its RGB, keeps the alpha), the frame ended with a
//    shot - the device saves its picture (nr_core_smoke.bmp).
// 3. The saved picture compared pixel for pixel with the expected one (made here from the same pattern), and the expected one's
//    hash with the stored reference (kReferenceHash): a change to the pattern, the clear or the copy fails one of the two.
// 4. (nr_core S7) A PS2-format texture through the core's host-memory entry (nr::convertHostTexture): an 8-bit 256 x 256 image
//    uploaded as the game uploads one (its own format T8, no column tiling) with a 256-entry palette level in the GS's CSM1
//    order, from this program's own memory. Its decoded pixels checked one by one against the expected ones (the palette entry
//    at the index with bits 3 and 4 exchanged, alpha doubled as the GS reads it), its pixelHash and HD-pack content key
//    (hdContentKey, name "SMOKE_T8") against the stored ones, and the texture drawn into the picture's right half by the same
//    copy - so the stored picture hash covers it too.
// 5. (nr_core S9a, `--window` only; Windows) The core's window: the core opens with a window of its own (320 x 200, hidden unless
//    `--show`) and the draw above runs with it open; its handle, client size and raw-input registration are checked, then each
//    event the core forwards (nr_core::WindowEvents): a resize (SetWindowPos), focus gained / lost and a DPI change (the messages
//    sent to it as Windows sends them), raw keyboard / mouse reports (RAWINPUT records as GetRawInputData gives them, through the
//    core's translation - real input is never injected), a close request (posted, then pumpEvents: false, the window still there
//    until closeWindow). Then a window the HOST made, passed in by its handle: the core watches its messages without taking its
//    loop (resize, focus, a posted close seen once even when peeked first, the host's own handler still sees each message), lets
//    it go (no events after closeWindow), the same from another thread's open(), and raw input registered for it on request and
//    taken back at closeWindow. Exit 6: a window check failed.
// 6. (nr_core S9b, `--present`, implies --window; `--input <bmp>` a picture of one's own) The core presents into its window: a
//    picture (the test pattern, 640 x 360) made a frame's shared picture and presented through the core's swap chain
//    (nr_core::present), the window's picture read back as drawn and saved (present_<case>.bmp): 1:1 into a bordered window (an
//    exact copy, the bars exactly the clear colour), render scales 75 % and 50 % and a 16:9 picture letterboxed in a 21:9
//    window, each stretched bilinearly and with FSR 1 (EASU + RCAS; those pictures are compared offline with the GL side's -
//    nr_present.cpp's GLSL - on the same input), then a present with vsync and one without.
// 7. (nr_core S10) Frames the host records itself (nr_core::submit): step 2's draws recorded by a callback into the core's own
//    picture target on the core's drawing thread (not this one), the frame saved (nr_core_submit.bmp) and checked as step 2's -
//    the same pixels, the same stored hash; a refused submit (no recording, no size); the back-pressure: eight frames each
//    recording ~25 ms - never more than three handed over and not drawn to the end (two waiting, one drawing), the submits held
//    back by them, all drawn after flushSubmitted. (S11) The order of frames handed over through the recomp's queueFrame and
//    through submit, mixed: each submit right behind a queueFrame frame whose read takes ~40 ms - drawn in the order handed
//    over (S10 let a submitted frame overtake a frame still being read), the submitted ones never read. With `--present`: the
//    core opened with Settings::livePictures = 1 and corePacing = 0 and neither TF_NATIVE_LIVE nor TF_PACING in the
//    environment (S11: the settings, not the environment - the pacing checked 0), a submitted frame presented by the core
//    into its window (1:1 in a taller window: letterboxed to the frame's shape, the bars exactly the clear colour, the picture
//    exact), then endSubmits. Exit 7: a submit check failed.
// 8. (nr_core S12) Overlays and the window's messages. `--window`: every message of the core's own window forwarded
//    (WindowEvents::message - a sent one with its wParam / lParam; one the callback says it handled answered 1 by the core's
//    window) and of a host's window (sent and posted; "handled" not acted on there: the host's own handler answers). With
//    `--present`: a present with an overlay (PresentRequest::overlay) - the OverlayTarget it is handed checked (the device, queue,
//    open command list, back buffer and its view, the window's size, R8G8B8A8_UNORM, a buffer index below the count of 3) and a
//    known quad cleared into the back buffer through it (a rect of one colour) - read back exactly: the quad's pixels that colour,
//    the bars and the rest of the picture as without it (the shot taken after the overlay); then the same through a submitted
//    frame (FrameInfo::overlay, on the core's drawing thread). The overlays' failures count with the window's (exit 6).
// 9. (nr_core S13a, `--present`) A layer (nr_core::layer, layerOverlay): layer 0 made at the window's size (a shared handle, its
//    size and generation 1; asked again at the same size: the same texture, generation 1; another size: generation 2), opened
//    through its handle and filled here with known premultiplied pixels - an opaque rect, a half-covered rect, the rest clear -
//    then presented over the picture by layerOverlay with its rows bottom up (GL's): read back, the opaque rect exactly its colour
//    at the mirrored rows, the half-covered one rgb + picture * (1 - a) within one level, every other pixel the picture or the
//    bars exactly; then two layers in one present, the second over the first (the order kept). Failures count with the
//    window's (exit 6).
#include "nr_core.h"
#include "nr_backend.h"
#include "nr_device.h"
#include "nr_draw.h"
#include "nr_frame.h" // (kConstantStride)
#include "nr_queue.h" // (S11: nr::queueFrame beside submit)
#include "nr_types.h"
#include "nr_window.h"
#include "nr_glshare.h" // (d3d::SharedPicture)

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h> // (S12: the overlay check clears its quad on the present's command list)
#include <thread>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
    constexpr uint32_t kW = 256u, kH = 144u, kCopyW = 128u; // the picture, and the left part the pattern is copied into
    constexpr uint8_t kClear[3] = {48u, 96u, 160u};          // the clear colour (8-bit levels: exact as unorm floats)
    // The stored reference: FNV-1a 64 of the expected picture's RGB, row by row from the top (tests/nr_core_smoke_ref.py makes it).
    constexpr uint64_t kReferenceHash = 0x92273A06B9B09525ull;
    // (step 4) The PS2-format texture's stored pixelHash and HD-pack content key (tests/nr_core_smoke_ref.py makes them).
    constexpr uint64_t kTexPixelHash = 0x19C385397F9A0383ull, kTexContentKey = 0x30C8D43DA840AE0Aull;
    constexpr uint32_t kTexSize = 256u;

    // The texture's index at (x, y) and its palette level's entry j as uploaded (R j, G 255 - j, B 7j, alpha j / 2: PS2 0x80 = 1.0).
    uint32_t ps2Index(uint32_t x, uint32_t y) { return (x + 2u * y) & 0xFFu; }
    uint32_t clutEntry(uint32_t j) { return j | (255u - j) << 8 | ((7u * j) & 0xFFu) << 16 | (j >> 1) << 24; }
    // The decoded texel the GS samples there: an 8-bit palette is read in CSM1 order (the index's bits 3 and 4 exchanged).
    uint32_t ps2Texel(uint32_t x, uint32_t y)
    {
        const uint32_t i = ps2Index(x, y), s = (i & ~0x18u) | ((i & 8u) << 1) | ((i & 0x10u) >> 1);
        const uint32_t c = clutEntry(s);
        return (c & 0x00FFFFFFu) | ((c >> 24) * 2u) << 24;
    }

    // The pattern's texel (x, y): R a ramp across, G a ramp down, B an 8-pixel checker.
    void pattern(uint32_t x, uint32_t y, uint8_t rgb[3])
    {
        rgb[0] = static_cast<uint8_t>((x * 2u) & 0xFFu);
        rgb[1] = static_cast<uint8_t>(y * 255u / (kH - 1u));
        rgb[2] = static_cast<uint8_t>(((x / 8u + y / 8u) & 1u) ? 200u : 40u);
    }
    void expected(uint32_t x, uint32_t y, uint8_t rgb[3])
    {
        if (x < kCopyW)
            pattern(x, y, rgb);
        else
        {
            // (step 4: the PS2 texture's texel at the pixel, drawn over the clear colour)
            const uint32_t t = ps2Texel(x, y);
            rgb[0] = static_cast<uint8_t>(t), rgb[1] = static_cast<uint8_t>(t >> 8), rgb[2] = static_cast<uint8_t>(t >> 16);
        }
    }

    // Step 4: the PS2-format texture through the core's host-memory entry; null when it did not decode as expected.
    std::shared_ptr<const nr::Texture> ps2Texture()
    {
        static std::vector<uint8_t> texels(static_cast<size_t>(kTexSize) * kTexSize), clut(256u * 4u);
        for (uint32_t y = 0; y < kTexSize; ++y)
            for (uint32_t x = 0; x < kTexSize; ++x)
                texels[static_cast<size_t>(y) * kTexSize + x] = static_cast<uint8_t>(ps2Index(x, y));
        for (uint32_t j = 0; j < 256u; ++j)
        {
            const uint32_t c = clutEntry(j);
            std::memcpy(clut.data() + 4u * j, &c, 4);
        }
        nr::HostTexture in;
        in.psm = 0x13u; // T8, as the GS samples it
        in.width = in.height = kTexSize;
        in.image.psm = 0x13u; // and as the game uploads it
        in.image.width = in.image.height = kTexSize;
        in.image.bpp = 8u;
        in.image.texels = texels.data();
        in.image.bytes = texels.size();
        in.clut = clut.data();
        in.clutEntries = 256u;
        std::shared_ptr<const nr::Texture> t = nr::convertHostTexture(in, 1u);
        if (!t || t->width != kTexSize || t->height != kTexSize || t->pixels.size() != static_cast<size_t>(kTexSize) * kTexSize)
        {
            std::printf("[smoke] the PS2 texture did not convert (%s)\n", t ? "wrong size" : "null");
            return nullptr;
        }
        uint32_t differ = 0;
        for (uint32_t y = 0; y < kTexSize; ++y)
            for (uint32_t x = 0; x < kTexSize; ++x)
                differ += t->pixels[static_cast<size_t>(y) * kTexSize + x] != ps2Texel(x, y) ? 1u : 0u;
        const uint64_t ph = nr::pixelHash(*t), ck = nr::hdContentKey("SMOKE_T8", *t);
        std::printf("[smoke] PS2 texture (T8 256 x 256, CSM1 palette) through the host-memory entry: %u of %u texels differ from the "
                    "expected ones; pixelHash %016llx (stored %016llx), HD-pack content key %016llx (stored %016llx)\n",
                    differ, kTexSize * kTexSize, static_cast<unsigned long long>(ph), static_cast<unsigned long long>(kTexPixelHash),
                    static_cast<unsigned long long>(ck), static_cast<unsigned long long>(kTexContentKey));
        return differ == 0u && ph == kTexPixelHash && ck == kTexContentKey ? t : nullptr;
    }
    uint64_t fnv(uint64_t h, uint8_t b) { return (h ^ b) * 0x100000001B3ull; }

    // The device's saved picture (saveBmp: 54-byte header, 32-bit BGRA, bottom-up) as RGB rows from the top.
    bool loadBmp(const char *path, uint32_t &w, uint32_t &h, std::vector<uint8_t> &rgb)
    {
        FILE *f = std::fopen(path, "rb");
        if (!f)
            return false;
        uint8_t hd[54] = {};
        bool ok = std::fread(hd, 1, sizeof(hd), f) == sizeof(hd) && hd[0] == 'B' && hd[1] == 'M';
        uint32_t off = 0, bits = 0;
        int32_t sw = 0, sh = 0;
        if (ok)
        {
            std::memcpy(&off, hd + 10, 4);
            std::memcpy(&sw, hd + 18, 4);
            std::memcpy(&sh, hd + 22, 4);
            bits = hd[28];
            ok = bits == 32u && sw > 0 && sh != 0 && std::fseek(f, static_cast<long>(off), SEEK_SET) == 0;
        }
        if (ok)
        {
            w = static_cast<uint32_t>(sw);
            h = static_cast<uint32_t>(sh < 0 ? -sh : sh);
            std::vector<uint8_t> row(static_cast<size_t>(w) * 4u);
            rgb.assign(static_cast<size_t>(w) * h * 3u, 0u);
            for (uint32_t r = 0; ok && r < h; ++r)
            {
                ok = std::fread(row.data(), 1, row.size(), f) == row.size();
                const uint32_t y = sh > 0 ? h - 1u - r : r;
                for (uint32_t x = 0; ok && x < w; ++x)
                    for (int c = 0; c < 3; ++c)
                        rgb[(static_cast<size_t>(y) * w + x) * 3u + c] = row[x * 4u + 2u - c]; // (BGRA -> RGB)
            }
        }
        std::fclose(f);
        return ok;
    }

    // Step 2's draws (and step 4's texture in the right half) into `picture` (kW x kH), in a frame begun on `dev`; the textures
    // it made let go of at its end. False: a part was not made (said).
    bool recordStream(nr::gfx::Device &dev, nr::gfx::TargetId picture, const nr::Texture &ps2)
    {
        using namespace nr;
        const gfx::TextureId tex = dev.createTexture(kW, kH, gfx::Format::RGBA8, 1u);
        const gfx::TextureId tex2 = dev.createTexture(ps2.width, ps2.height, gfx::Format::RGBA8, 1u);
        if (!picture || !tex || !tex2 || !dev.effect(gfx::Effect::ToneMap))
        {
            std::printf("[smoke] target %u, texture %u, tone pass %s\n", picture, tex, dev.effect(gfx::Effect::ToneMap) ? "yes" : "no");
            return false;
        }
        std::vector<uint8_t> texels(static_cast<size_t>(kW) * kH * 4u);
        for (uint32_t y = 0; y < kH; ++y)
            for (uint32_t x = 0; x < kW; ++x)
            {
                uint8_t *t = texels.data() + (static_cast<size_t>(y) * kW + x) * 4u;
                pattern(x, y, t);
                t[3] = 255u;
            }
        dev.uploadTexture(tex, 0u, texels.data(), kW * 4u);
        dev.uploadTexture(tex2, 0u, ps2.pixels.data(), ps2.width * 4u); // (step 4: the core's RGBA8, R in the low byte)
        const float clear[4] = {kClear[0] / 255.0f, kClear[1] / 255.0f, kClear[2] / 255.0f, 1.0f};
        dev.clearColour(picture, clear);
        gfx::RingSpan cs;
        if (!dev.ringAlloc(gfx::RingKind::Constants, frame::kConstantStride, 256u, cs))
        {
            std::printf("[smoke] no room in the constants ring\n");
            return false;
        }
        const float tone[4] = {1.0f, 0.8f, 0.0f, 0.0f}; // (exposure, k, strength 0: as it was, saturation 0)
        std::memcpy(cs.cpu, tone, sizeof(tone));
        dev.use(picture, gfx::UseFor::Draw);
        dev.setTarget(picture, false);
        dev.setViewport(0.0f, 0.0f, static_cast<float>(kCopyW), static_cast<float>(kH));
        dev.setScissor(gfx::Rect{0, 0, static_cast<int32_t>(kCopyW), static_cast<int32_t>(kH)});
        gfx::PipelineDesc pd;
        pd.effect = gfx::Effect::ToneMap;
        if (!dev.setPipeline(pd))
        {
            std::printf("[smoke] the tone pass's pipeline was not made\n");
            return false;
        }
        dev.bindDrawConstants(cs);
        dev.bindTextures(tex, 0u, 0u);
        dev.draw(3u, 0u);
        // (step 4) The PS2 texture copied the same way into the right half.
        dev.setViewport(static_cast<float>(kCopyW), 0.0f, static_cast<float>(kW - kCopyW), static_cast<float>(kH));
        dev.setScissor(gfx::Rect{static_cast<int32_t>(kCopyW), 0, static_cast<int32_t>(kW), static_cast<int32_t>(kH)});
        dev.bindTextures(tex2, 0u, 0u);
        dev.draw(3u, 0u);
        dev.bindTextures(0u, 0u, 0u);
        dev.releaseTexture(tex);
        dev.releaseTexture(tex2);
        return true;
    }

    // Step 2: the known draw stream (and step 4's texture in the right half); true when the frame was recorded and ended.
    bool drawStream(const char *shot, const nr::Texture &ps2)
    {
        using namespace nr;
        gfx::Device &dev = gfx::device();
        if (!dev.prepareFrame(kW, kH))
        {
            std::printf("[smoke] prepareFrame failed\n");
            return false;
        }
        dev.beginFrame(1u);
        gfx::TargetDesc td;
        td.width = kW;
        td.height = kH;
        td.colour = gfx::Format::RGBA8;
        td.depth = false;
        td.shared = true;
        const gfx::TargetId picture = dev.createTarget(td);
        if (!picture || !recordStream(dev, picture, ps2))
            return false;

        Frame frame;
        frame.tick = 1u;
        gfx::FrameEnd end;
        end.frame = &frame;
        end.picture = picture;
        gfx::PictureOut out;
        draw::PictureConstants pc{};
        pc.picture[0] = 1.0f;
        for (int i = 0; i < 4; ++i)
            pc.turn[5 * i] = pc.turnClip[5 * i] = 1.0f;
        out.constants.assign(reinterpret_cast<const uint8_t *>(&pc), reinterpret_cast<const uint8_t *>(&pc) + sizeof(pc));
        end.pictures.push_back(std::move(out));
        end.shot = shot;
        end.waitForGpu = true;
        const bool ended = dev.endFrame(end);
        std::printf("[smoke] frame recorded and ended: %s\n", ended ? "yes" : "no");
        dev.releaseTarget(picture);
        return ended;
    }
    // ---- step 7 (S10): frames the host records itself (nr_core::submit) ----
    int g_submitFailed = 0;
    void submitCheck(bool ok, const char *what)
    {
        std::printf("[smoke] submit: %s: %s\n", what, ok ? "ok" : "FAILED");
        g_submitFailed += ok ? 0 : 1;
    }

    // The device's saved picture against step 2's expected one: 0 px and the stored hash.
    bool samePicture(const char *shot)
    {
        uint32_t w = 0, h = 0;
        std::vector<uint8_t> got;
        if (!loadBmp(shot, w, h, got) || w != kW || h != kH)
        {
            std::printf("[smoke] submit: %s did not load as %u x %u (got %u x %u)\n", shot, kW, kH, w, h);
            return false;
        }
        uint64_t hash = 0xCBF29CE484222325ull;
        uint32_t differ = 0;
        for (uint32_t y = 0; y < kH; ++y)
            for (uint32_t x = 0; x < kW; ++x)
            {
                uint8_t e[3];
                expected(x, y, e);
                bool d = false;
                for (int c = 0; c < 3; ++c)
                {
                    const uint8_t g = got[(static_cast<size_t>(y) * kW + x) * 3u + c];
                    hash = fnv(hash, g);
                    d = d || g != e[c];
                }
                differ += d ? 1u : 0u;
            }
        std::printf("[smoke] submit: %s: hash %016llx, %u of %u pixels differ from the expected one\n", shot,
                    static_cast<unsigned long long>(hash), differ, kW * kH);
        return differ == 0u && hash == kReferenceHash;
    }

    void submitChecks(const nr::Texture &ps2)
    {
        const std::thread::id mainThread = std::this_thread::get_id();
        // (a) Step 2's stream through submit, offscreen (present 0: a window open for --window is not shown to).
        const char *shot = "nr_core_submit.bmp";
        std::remove(shot);
        std::atomic<int> recorded{-1};
        nr_core::FrameInfo info;
        info.tick = 2u;
        info.pictureWidth = kW, info.pictureHeight = kH;
        info.shot = shot;
        info.waitForGpu = true;
        info.present = 0;
        std::atomic<int> told{-1}; // (FrameInfo::done: drawn 1 / not 0, presented +2)
        info.done = [&told](bool drawn, bool presented) { told = (drawn ? 1 : 0) + (presented ? 2 : 0); };
        const bool queued = nr_core::submit(
            [&](nr::gfx::Device &dev, nr_core::Recording &rec) {
                const bool other = std::this_thread::get_id() != mainThread;
                const bool given = rec.picture != 0u && rec.width == kW && rec.height == kH && rec.tick == 2u && rec.slot < 2u &&
                                   rec.end.pictureTarget == rec.picture && rec.end.displayRect[2] == static_cast<int>(kW) &&
                                   rec.end.displayRect[3] == static_cast<int>(kH);
                std::printf("[smoke] submit: recording on %s thread, picture target %u, %u x %u, slot %u\n", other ? "another" : "THIS",
                            rec.picture, rec.width, rec.height, rec.slot);
                recorded = other && given && recordStream(dev, rec.picture, ps2) ? 1 : 0;
            },
            info);
        nr_core::flushSubmitted();
        submitCheck(queued && recorded.load() == 1 && told.load() == 1,
                    "step 2's stream recorded through submit on the core's drawing thread (done: drawn, not presented)");
        submitCheck(recorded.load() == 1 && samePicture(shot), "its picture = step 2's (0 px, the stored hash)");

        // (b) Refused: no recording, no size.
        nr_core::FrameInfo none = info;
        none.shot.clear();
        none.pictureWidth = 0u;
        const bool refused = !nr_core::submit(nr_core::Record(), info) && !nr_core::submit([](nr::gfx::Device &, nr_core::Recording &) {}, none);
        submitCheck(refused, "a submit with no recording or no size refused");

        // (c) The back-pressure: eight frames recording ~25 ms each.
        static std::atomic<unsigned> started{0}, done{0};
        started = 0u, done = 0u;
        unsigned most = 0u;
        const auto t0 = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < 8u; ++i)
        {
            nr_core::FrameInfo f;
            f.tick = 10u + i;
            f.pictureWidth = 64u, f.pictureHeight = 64u;
            f.present = 0;
            nr_core::submit(
                [i](nr::gfx::Device &dev, nr_core::Recording &rec) {
                    ++started;
                    std::this_thread::sleep_for(std::chrono::milliseconds(25));
                    const float c[4] = {static_cast<float>(i) / 8.0f, 0.0f, 0.0f, 1.0f};
                    dev.clearColour(rec.picture, c);
                    ++done;
                },
                f);
            most = std::max(most, (i + 1u) - done.load());
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        nr_core::flushSubmitted();
        char what[200];
        std::snprintf(what, sizeof(what), "8 frames of ~25 ms: at most %u handed over and not drawn (<= 3), the submits took %.0f ms (>= 100: held "
                                          "back), %u drawn after flushSubmitted",
                      most, ms, done.load());
        submitCheck(most <= 3u && ms >= 100.0 && done.load() == 8u && started.load() == 8u, what);

        // (d) (S11) queueFrame and submit mixed, in one process: four frames through the recomp's queueFrame, each read ~40 ms
        // (the read thread; the host's draw only notes it), each followed at once by a submitted frame - drawn in the order
        // handed over (ticks 100..107), the submitted ones passed on unread.
        static std::mutex orderMutex;
        static std::vector<uint64_t> order;
        static std::atomic<int> readSubmitted{0};
        order.clear();
        readSubmitted = 0;
        nr::FrameQueueHost qh;
        qh.read = [](nr::Job &job) {
            if (job.submitted)
                ++readSubmitted;
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        };
        qh.draw = [](nr::Job &job) {
            std::lock_guard<std::mutex> lock(orderMutex);
            order.push_back(job.frame.tick);
        };
        nr::setFrameQueueHost(qh);
        for (uint64_t i = 0; i < 4u; ++i)
        {
            nr::Frame qf;
            qf.tick = 100u + 2u * i;
            nr::queueFrame(std::move(qf), std::string());
            nr_core::FrameInfo f;
            f.tick = 101u + 2u * i;
            f.pictureWidth = 64u, f.pictureHeight = 64u;
            f.present = 0;
            nr_core::submit(
                [](nr::gfx::Device &dev, nr_core::Recording &rec) {
                    const float c[4] = {0.0f, 0.5f, 0.0f, 1.0f};
                    dev.clearColour(rec.picture, c);
                    std::lock_guard<std::mutex> lock(orderMutex);
                    order.push_back(rec.tick);
                },
                f);
        }
        nr_core::flushSubmitted(); // (the last frame handed over is a submitted one: every frame before it drawn too)
        nr::setFrameQueueHost(nr::FrameQueueHost{});
        std::string got;
        bool inOrder = order.size() == 8u;
        {
            std::lock_guard<std::mutex> lock(orderMutex);
            for (size_t i = 0; i < order.size(); ++i)
            {
                got += (i ? " " : "") + std::to_string(order[i]);
                inOrder = inOrder && order[i] == 100u + i;
            }
        }
        std::snprintf(what, sizeof(what), "queueFrame and submit mixed: drawn in the order handed over (100..107): %s; submitted frames read: %d",
                      got.c_str(), readSubmitted.load());
        submitCheck(inOrder && readSubmitted.load() == 0, what);
    }

#if defined(_WIN32)
    // ---- step 5 (S9a): the window ----
    struct Seen
    {
        int resized = 0, lastW = 0, lastH = 0, focusIn = 0, focusOut = 0, closed = 0, dpi = 0, lastDpi = 0, keys = 0, mice = 0;
        int messages = 0, userSeen = 0; // (S12: every message; WM_USER + 6..8 seen with their wParam / lParam)
        uint32_t userMessage = 0;
        uint64_t userW = 0;
        int64_t userL = 0;
        nr_core::KeyEvent key;
        nr_core::MouseEvent mouse;
    };
    Seen g_seen;
    int g_failed = 0;
    void check(bool ok, const char *what)
    {
        std::printf("[smoke] window: %s: %s\n", what, ok ? "ok" : "FAILED");
        g_failed += ok ? 0 : 1;
    }
    void listen()
    {
        nr_core::WindowEvents ev;
        ev.resized = [](int w, int h) { ++g_seen.resized, g_seen.lastW = w, g_seen.lastH = h; };
        ev.focus = [](bool f) { ++(f ? g_seen.focusIn : g_seen.focusOut); };
        ev.closed = [] { ++g_seen.closed; };
        ev.dpiChanged = [](int d) { ++g_seen.dpi, g_seen.lastDpi = d; };
        ev.key = [](const nr_core::KeyEvent &k) { ++g_seen.keys, g_seen.key = k; };
        ev.mouse = [](const nr_core::MouseEvent &m) { ++g_seen.mice, g_seen.mouse = m; };
        // (S12) Every message; WM_USER + 7 said handled.
        ev.message = [](void *, uint32_t message, uint64_t wParam, int64_t lParam) {
            ++g_seen.messages;
            if (message >= WM_USER + 6u && message <= WM_USER + 8u)
                ++g_seen.userSeen, g_seen.userMessage = message, g_seen.userW = wParam, g_seen.userL = lParam;
            return message == WM_USER + 7u;
        };
        nr_core::setWindowEvents(ev);
    }
    // The raw-input registrations of this program for keyboard (6) and mouse (2) that point at `target`.
    int rawTargets(HWND target)
    {
        UINT n = 0;
        GetRegisteredRawInputDevices(nullptr, &n, sizeof(RAWINPUTDEVICE));
        std::vector<RAWINPUTDEVICE> d(n ? n : 1u);
        if (n && GetRegisteredRawInputDevices(d.data(), &n, sizeof(RAWINPUTDEVICE)) == static_cast<UINT>(-1))
            return -1;
        int found = 0;
        for (UINT i = 0; i < n; ++i)
            if (d[i].usUsagePage == 1u && (d[i].usUsage == 6u || d[i].usUsage == 2u) && d[i].hwndTarget == target)
                found |= d[i].usUsage == 6u ? 1 : 2;
        return found;
    }
    // Resizes `hwnd` so its client area is w x h (as a user's drag would: WM_SIZE comes from it, sent).
    void resizeClient(HWND hwnd, int w, int h)
    {
        RECT r = {0, 0, w, h};
        AdjustWindowRectEx(&r, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE,
                           static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)));
        SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // The core's own window, open (from main), before the draw: handle, size, raw input, resize, focus, DPI, raw reports.
    void ownWindowChecks()
    {
        const HWND hwnd = static_cast<HWND>(nr_core::window());
        int w = 0, h = 0;
        nr_core::windowSize(w, h);
        std::printf("[smoke] window: the core's own window %p, client %d x %d\n", static_cast<void *>(hwnd), w, h);
        check(hwnd && IsWindow(hwnd), "the core made its window");
        check(w == 320 && h == 200, "its client area 320 x 200");
        check(nr_core::window_detail::rawInputRegistered() == 3u && rawTargets(hwnd) == 3, "raw keyboard + mouse registered for it");
        nr_core::pumpEvents();
        g_seen = Seen{};
        resizeClient(hwnd, 400, 300);
        nr_core::windowSize(w, h);
        check(g_seen.resized >= 1 && g_seen.lastW == 400 && g_seen.lastH == 300 && w == 400 && h == 300, "resize to 400 x 300 told (resized)");
        SendMessageW(hwnd, WM_SETFOCUS, 0, 0);
        SendMessageW(hwnd, WM_KILLFOCUS, 0, 0);
        check(g_seen.focusIn == 1 && g_seen.focusOut == 1, "focus gained and lost told (focus true, false)");
        RECT now = {};
        GetWindowRect(hwnd, &now);
        RECT suggested = {now.left, now.top, now.left + 600, now.top + 450};
        const int resizedBefore = g_seen.resized;
        SendMessageW(hwnd, 0x02E0 /* WM_DPICHANGED */, MAKEWPARAM(144, 144), reinterpret_cast<LPARAM>(&suggested));
        nr_core::windowSize(w, h);
        RECT outer = {};
        GetWindowRect(hwnd, &outer);
        check(g_seen.dpi == 1 && g_seen.lastDpi == 144 && outer.right - outer.left == 600 && outer.bottom - outer.top == 450 &&
                  g_seen.resized > resizedBefore && g_seen.lastW == w && g_seen.lastH == h,
              "DPI change told (dpiChanged 144) and the suggested size taken (resized)");

        RAWINPUT ri = {};
        ri.header.dwType = RIM_TYPEKEYBOARD;
        ri.data.keyboard.MakeCode = 0x1D, ri.data.keyboard.VKey = VK_CONTROL, ri.data.keyboard.Flags = RI_KEY_E0; // right Ctrl down
        nr_core::window_detail::forwardRawInput(&ri);
        const nr_core::KeyEvent k1 = g_seen.key;
        ri.data.keyboard.Flags = RI_KEY_E0 | RI_KEY_BREAK; // ... let go
        nr_core::window_detail::forwardRawInput(&ri);
        const nr_core::KeyEvent k2 = g_seen.key;
        ri.data.keyboard.VKey = 0xFF; // (a fake key: not told)
        nr_core::window_detail::forwardRawInput(&ri);
        check(g_seen.keys == 2 && k1.scanCode == 0x1D && k1.virtualKey == VK_CONTROL && k1.down && k1.extended && !k2.down && k2.extended,
              "raw keys told (right Ctrl down, up; a fake key left out)");
        RAWINPUT rm = {};
        rm.header.dwType = RIM_TYPEMOUSE;
        rm.data.mouse.lLastX = 7, rm.data.mouse.lLastY = -3;
        rm.data.mouse.usButtonFlags = RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_UP | RI_MOUSE_BUTTON_5_DOWN | RI_MOUSE_WHEEL;
        rm.data.mouse.usButtonData = static_cast<USHORT>(-120);
        nr_core::window_detail::forwardRawInput(&rm);
        const nr_core::MouseEvent m = g_seen.mouse;
        check(g_seen.mice == 1 && m.dx == 7 && m.dy == -3 && !m.absolute && m.pressed == 0x11u && m.released == 0x02u && m.wheel == -120 &&
                  m.hwheel == 0,
              "raw mouse told (move 7,-3; left + side 2 pressed, right let go; wheel one notch back)");

        // (S12) Every message forwarded: a sent one with its parameters; one the callback handled answered 1 by the core's window.
        g_seen = Seen{};
        const LRESULT plain = SendMessageW(hwnd, WM_USER + 6u, 11u, -22);
        const bool plainSeen = g_seen.userSeen == 1 && g_seen.userMessage == WM_USER + 6u && g_seen.userW == 11u && g_seen.userL == -22;
        const LRESULT handled = SendMessageW(hwnd, WM_USER + 7u, 33u, 44);
        check(plainSeen && plain == 0 && handled == 1 && g_seen.userSeen == 2 && g_seen.messages >= 2,
              "every message forwarded (S12: WM_USER+6 with 11, -22; WM_USER+7 said handled -> answered 1)");
    }

    // The core's own window asked to close, then closed.
    void ownWindowClose()
    {
        const HWND hwnd = static_cast<HWND>(nr_core::window());
        g_seen = Seen{};
        const bool openBefore = nr_core::pumpEvents();
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        const bool openAfter = nr_core::pumpEvents();
        check(openBefore && !openAfter && g_seen.closed == 1 && IsWindow(hwnd), "close asked: told once (closed), pumpEvents false, the window kept");
        nr_core::closeWindow();
        check(!IsWindow(hwnd) && !nr_core::window() && nr_core::window_detail::rawInputRegistered() == 0u && rawTargets(hwnd) == 0,
              "closeWindow: the window gone, raw input taken back");
    }

    // A window the host made, passed in.
    struct HostSeen
    {
        int size = 0, close = 0, focus = 0;
    };
    HostSeen g_host;
    LRESULT CALLBACK hostProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_SIZE)
            ++g_host.size;
        else if (message == WM_CLOSE)
        {
            ++g_host.close; // (the host keeps its window)
            return 0;
        }
        else if (message == WM_SETFOCUS)
            ++g_host.focus;
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    void hostPump()
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    void hostWindowChecks()
    {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = hostProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"nr_core_smoke_host";
        RegisterClassExW(&wc);
        const HWND host = CreateWindowExW(0, wc.lpszClassName, L"smoke host", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 300, 200,
                                          nullptr, nullptr, wc.hInstance, nullptr);
        nr_core::WindowSpec spec;
        spec.handle = host;
        const bool opened = nr_core::open(nr_core::Settings{}, spec);
        check(host && opened && nr_core::window() == host && nr_core::window_detail::watchingHostWindow() &&
                  nr_core::window_detail::rawInputRegistered() == 0u && rawTargets(host) == 0,
              "a host's window passed in: watched, no raw input registered by default");
        hostPump();
        g_seen = Seen{}, g_host = HostSeen{};
        resizeClient(host, 360, 240);
        check(g_seen.resized >= 1 && g_seen.lastW == 360 && g_seen.lastH == 240 && g_host.size >= 1, "its resize told (and its own handler saw it)");
        SendMessageW(host, WM_SETFOCUS, 0, 0);
        check(g_seen.focusIn == 1 && g_host.focus == 1, "its focus told");
        // (S12) Its messages forwarded, sent and posted; "handled" not acted on (the host's own handler answers: 0).
        g_seen.userSeen = 0;
        const LRESULT hostAnswer = SendMessageW(host, WM_USER + 7u, 5u, 6);
        PostMessageW(host, WM_USER + 8u, 7u, 8);
        hostPump();
        check(hostAnswer == 0 && g_seen.userSeen == 2 && g_seen.userMessage == WM_USER + 8u && g_seen.userW == 7u && g_seen.userL == 8,
              "its messages forwarded (S12: a sent one, a posted one with 7, 8; 'handled' left to the host's handler)");
        PostMessageW(host, WM_CLOSE, 0, 0);
        MSG peek;
        PeekMessageW(&peek, host, WM_CLOSE, WM_CLOSE, PM_NOREMOVE); // (a peek that leaves it: not told yet)
        const int afterPeek = g_seen.closed;
        hostPump();
        check(afterPeek == 0 && g_seen.closed == 1 && g_host.close == 1 && IsWindow(host) && nr_core::pumpEvents() == false,
              "its posted close told once (a peek before it not counted), its own handler saw it, the window kept");
        nr_core::closeWindow();
        g_seen = Seen{}, g_host = HostSeen{};
        resizeClient(host, 380, 250);
        hostPump();
        check(!nr_core::window() && !nr_core::window_detail::watchingHostWindow() && g_seen.resized == 0 && g_host.size >= 1,
              "let go: nothing told after closeWindow, its handler still runs");

        // open() on another thread than the window's (the recomp's case: the window is the main thread's).
        bool otherOpened = false;
        std::thread([&] { otherOpened = nr_core::open(nr_core::Settings{}, spec); }).join();
        g_seen = Seen{};
        resizeClient(host, 300, 200);
        check(otherOpened && g_seen.resized >= 1 && g_seen.lastW == 300 && g_seen.lastH == 200, "opened from another thread: its resize told");
        std::thread([] { nr_core::closeWindow(); }).join();
        g_seen = Seen{};
        resizeClient(host, 320, 210);
        check(!nr_core::window() && g_seen.resized == 0, "let go from another thread: nothing told");

        // Raw input for it on request (open() on its own thread), taken back at closeWindow.
        spec.rawInput = 1;
        const bool rawOpened = nr_core::open(nr_core::Settings{}, spec);
        const int targets = rawTargets(host);
        nr_core::closeWindow();
        check(rawOpened && targets == 3 && rawTargets(host) == 0 && nr_core::window_detail::rawInputRegistered() == 0u,
              "raw input for it on request, taken back at closeWindow");
        DestroyWindow(host);
        hostPump();
    }
#endif
#if defined(_WIN32)
    // ---- step 6 (S9b): the core presents into its window ----
    // The test picture (or --input's): edges at many angles, a ramp, fine lines and rings - the content EASU and RCAS react to.
    void presentPattern(uint32_t w, uint32_t h, std::vector<uint8_t> &rgba)
    {
        rgba.assign(static_cast<size_t>(w) * h * 4u, 255u);
        const float cx = w * 0.5f, cy = h * 0.5f;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                uint8_t *p = rgba.data() + (static_cast<size_t>(y) * w + x) * 4u;
                const float dx = x - cx, dy = y - cy, r = std::sqrt(dx * dx + dy * dy);
                const float a = std::atan2(dy, dx);
                const bool spoke = std::fmod(a * 24.0f / 3.14159265f + 48.0f, 2.0f) < 1.0f; // (48 wedges: edges at every angle)
                const bool ring = static_cast<uint32_t>(r / 9.0f) % 2u == 0u;
                const bool line = x % 7u == 0u || y % 11u == 0u;
                p[0] = static_cast<uint8_t>(r < h * 0.45f ? (spoke ? 230u : 20u) : (x * 255u / (w - 1u)));
                p[1] = static_cast<uint8_t>(r < h * 0.45f ? (ring ? 200u : 60u) : (line ? 250u : 90u));
                p[2] = static_cast<uint8_t>((y * 255u / (h - 1u)) ^ (spoke ? 0x40u : 0u));
            }
    }
    bool loadInput(const char *path, uint32_t &w, uint32_t &h, std::vector<uint8_t> &rgba)
    {
        std::vector<uint8_t> rgb;
        if (!loadBmp(path, w, h, rgb))
            return false;
        rgba.assign(static_cast<size_t>(w) * h * 4u, 255u);
        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
            std::memcpy(rgba.data() + i * 4u, rgb.data() + i * 3u, 3u);
        return true;
    }

    // The picture made a frame's shared picture (the tone pass's exact copy into a shared target), handed over; its index.
    int sharedPicture(uint32_t w, uint32_t h, const std::vector<uint8_t> &rgba, uint64_t tick)
    {
        using namespace nr;
        gfx::Device &dev = gfx::device();
        if (!dev.prepareFrame(w, h))
            return -1;
        dev.beginFrame(tick);
        gfx::TargetDesc td;
        td.width = w;
        td.height = h;
        td.colour = gfx::Format::RGBA8;
        td.depth = false;
        td.shared = true;
        const gfx::TargetId picture = dev.createTarget(td);
        const gfx::TextureId tex = dev.createTexture(w, h, gfx::Format::RGBA8, 1u);
        gfx::RingSpan cs;
        if (!picture || !tex || !dev.ringAlloc(gfx::RingKind::Constants, frame::kConstantStride, 256u, cs))
            return -1;
        dev.uploadTexture(tex, 0u, rgba.data(), w * 4u);
        const float tone[4] = {1.0f, 0.8f, 0.0f, 0.0f};
        std::memcpy(cs.cpu, tone, sizeof(tone));
        dev.use(picture, gfx::UseFor::Draw);
        dev.setTarget(picture, false);
        dev.setViewport(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
        dev.setScissor(gfx::Rect{0, 0, static_cast<int32_t>(w), static_cast<int32_t>(h)});
        gfx::PipelineDesc pd;
        pd.effect = gfx::Effect::ToneMap;
        if (!dev.setPipeline(pd))
            return -1;
        dev.bindDrawConstants(cs);
        dev.bindTextures(tex, 0u, 0u);
        dev.draw(3u, 0u);
        dev.bindTextures(0u, 0u, 0u);
        Frame frame;
        frame.tick = tick;
        gfx::FrameEnd end;
        end.frame = &frame;
        end.picture = picture;
        gfx::PictureOut out;
        draw::PictureConstants pc{};
        pc.picture[0] = 1.0f;
        for (int i = 0; i < 4; ++i)
            pc.turn[5 * i] = pc.turnClip[5 * i] = 1.0f;
        out.constants.assign(reinterpret_cast<const uint8_t *>(&pc), reinterpret_cast<const uint8_t *>(&pc) + sizeof(pc));
        end.pictures.push_back(std::move(out));
        end.waitForGpu = true;
        const bool ended = dev.endFrame(end);
        dev.releaseTexture(tex);
        dev.releaseTarget(picture);
        static uint64_t serial = 0;
        d3d::SharedPicture sp;
        if (!ended || !backend().latestShared(sp, serial))
            return -1;
        return sp.index;
    }

    // One case: the picture shown in a window of ww x wh at the destination rect, bilinear or FSR 1; the window's picture saved
    // as present_<name>.bmp; the bars checked (exactly the clear colour) and, `exact`, the destination checked against the
    // picture itself (a 1:1 rect: the stretch must be an exact copy).
    void presentCase(const char *name, uint32_t pw, uint32_t ph, const std::vector<uint8_t> &rgba, int ww, int wh, const float dst[4], int upscaler,
                     bool exact, uint64_t tick)
    {
        const HWND hwnd = static_cast<HWND>(nr_core::window());
        resizeClient(hwnd, ww, wh);
        nr_core::pumpEvents();
        const int index = sharedPicture(pw, ph, rgba, tick);
        char shot[96];
        std::snprintf(shot, sizeof(shot), "present_%s.bmp", name);
        std::remove(shot);
        nr_core::PresentRequest rq;
        rq.picture = index;
        std::memcpy(rq.dst, dst, sizeof(rq.dst));
        rq.upscaler = upscaler;
        rq.clear[0] = 16.0f / 255.0f, rq.clear[1] = 32.0f / 255.0f, rq.clear[2] = 64.0f / 255.0f;
        rq.shot = shot;
        const bool presented = index >= 0 && nr_core::present(rq);
        uint32_t w = 0, h = 0;
        std::vector<uint8_t> got;
        const bool loaded = presented && loadBmp(shot, w, h, got) && w == static_cast<uint32_t>(ww) && h == static_cast<uint32_t>(wh);
        uint32_t barsWrong = 0, inWrong = 0, most = 0;
        if (loaded)
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x)
                {
                    const uint8_t *g = got.data() + (static_cast<size_t>(y) * w + x) * 3u;
                    const bool inside = x >= dst[0] && x < dst[0] + dst[2] && y >= dst[1] && y < dst[1] + dst[3];
                    if (!inside)
                        barsWrong += (g[0] != 16u || g[1] != 32u || g[2] != 64u) ? 1u : 0u;
                    else if (exact)
                    {
                        const uint8_t *e = rgba.data() + (static_cast<size_t>(y - static_cast<uint32_t>(dst[1])) * pw + (x - static_cast<uint32_t>(dst[0]))) * 4u;
                        bool d = false;
                        for (int c = 0; c < 3; ++c)
                        {
                            const uint32_t diff = static_cast<uint32_t>(g[c] > e[c] ? g[c] - e[c] : e[c] - g[c]);
                            most = std::max(most, diff);
                            d = d || diff != 0u;
                        }
                        inWrong += d ? 1u : 0u;
                    }
                }
        char what[256];
        std::snprintf(what, sizeof(what), "present %s: picture %ux%u (shared %d) into %dx%d at %.0f,%.0f %.0fx%.0f, %s; saved %s %ux%u; bars wrong %u px%s",
                      name, pw, ph, index, ww, wh, dst[0], dst[1], dst[2], dst[3], upscaler ? "FSR 1" : "bilinear", shot, w, h, barsWrong,
                      exact ? "" : " (the picture: compared offline with the GL side)");
        if (exact)
            std::snprintf(what + std::strlen(what), sizeof(what) - std::strlen(what), "; picture wrong %u px (most %u levels)", inWrong, most);
        check(loaded && barsWrong == 0u && (!exact || inWrong == 0u), what);
    }

    // (S12) The quad an overlay draws: a rect of one colour cleared into the back buffer through the OverlayTarget it is handed.
    struct OverlaySeen
    {
        std::atomic<int> calls{0};
        std::atomic<bool> fieldsOk{false};
        std::atomic<uint32_t> width{0}, height{0}, buffer{99}, buffers{0}, format{0};
    };
    constexpr int kQuad[4] = {150, 80, 330, 200}; // left, top, right, bottom in the window
    constexpr uint8_t kQuadColour[3] = {200u, 40u, 90u};
    nr_core::Overlay quadOverlay(OverlaySeen &seen)
    {
        return [&seen](const nr_core::OverlayTarget &t) {
            ++seen.calls;
            seen.width = t.width, seen.height = t.height, seen.buffer = t.buffer, seen.buffers = t.buffers, seen.format = t.format;
            seen.fieldsOk = t.device && t.queue && t.commandList && t.target && t.rtv != 0u;
            if (!t.commandList || !t.rtv)
                return;
            auto *l = static_cast<ID3D12GraphicsCommandList *>(t.commandList);
            const D3D12_CPU_DESCRIPTOR_HANDLE rtv{static_cast<SIZE_T>(t.rtv)};
            const float c[4] = {kQuadColour[0] / 255.0f, kQuadColour[1] / 255.0f, kQuadColour[2] / 255.0f, 1.0f};
            const D3D12_RECT r{kQuad[0], kQuad[1], kQuad[2], kQuad[3]};
            l->ClearRenderTargetView(rtv, c, 1, &r);
        };
    }
    // A window shot `got` (w x h, RGB) against the picture `rgba` (pw x ph) placed 1:1 at (ox, oy), the bars the clear colour,
    // the quad's pixels its colour: the counts of wrong pixels.
    void overlayCompare(const std::vector<uint8_t> &got, uint32_t w, uint32_t h, const std::vector<uint8_t> &rgba, uint32_t pw, uint32_t ph,
                        uint32_t ox, uint32_t oy, uint32_t &quadWrong, uint32_t &barsWrong, uint32_t &pictureWrong)
    {
        quadWrong = barsWrong = pictureWrong = 0u;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint8_t *g = got.data() + (static_cast<size_t>(y) * w + x) * 3u;
                if (static_cast<int>(x) >= kQuad[0] && static_cast<int>(x) < kQuad[2] && static_cast<int>(y) >= kQuad[1] && static_cast<int>(y) < kQuad[3])
                    quadWrong += (g[0] != kQuadColour[0] || g[1] != kQuadColour[1] || g[2] != kQuadColour[2]) ? 1u : 0u;
                else if (x < ox || y < oy || x >= ox + pw || y >= oy + ph)
                    barsWrong += (g[0] != 16u || g[1] != 32u || g[2] != 64u) ? 1u : 0u;
                else
                {
                    const uint8_t *e = rgba.data() + (static_cast<size_t>(y - oy) * pw + (x - ox)) * 4u;
                    pictureWrong += (g[0] != e[0] || g[1] != e[1] || g[2] != e[2]) ? 1u : 0u;
                }
            }
    }
    void overlayCase(uint32_t pw, uint32_t ph, const std::vector<uint8_t> &rgba, uint64_t tick)
    {
        const HWND hwnd = static_cast<HWND>(nr_core::window());
        const int ww = static_cast<int>(pw) + 200, wh = static_cast<int>(ph) + 100;
        resizeClient(hwnd, ww, wh);
        nr_core::pumpEvents();
        const int index = sharedPicture(pw, ph, rgba, tick);
        const char *shot = "present_overlay.bmp";
        std::remove(shot);
        OverlaySeen seen;
        nr_core::PresentRequest rq;
        rq.picture = index;
        const float dst[4] = {100.0f, 50.0f, static_cast<float>(pw), static_cast<float>(ph)};
        std::memcpy(rq.dst, dst, sizeof(rq.dst));
        rq.clear[0] = 16.0f / 255.0f, rq.clear[1] = 32.0f / 255.0f, rq.clear[2] = 64.0f / 255.0f;
        rq.shot = shot;
        rq.overlay = quadOverlay(seen);
        const bool presented = index >= 0 && nr_core::present(rq);
        uint32_t w = 0, h = 0, quadWrong = 0, barsWrong = 0, pictureWrong = 0;
        std::vector<uint8_t> got;
        const bool loaded = presented && loadBmp(shot, w, h, got) && w == static_cast<uint32_t>(ww) && h == static_cast<uint32_t>(wh);
        if (loaded)
            overlayCompare(got, w, h, rgba, pw, ph, 100u, 50u, quadWrong, barsWrong, pictureWrong);
        char what[300];
        std::snprintf(what, sizeof(what), "an overlay at present (S12): called %d time(s), target %ux%u format %u buffer %u of %u, fields %s; "
                                          "the quad wrong %u px, bars wrong %u px, picture wrong %u px",
                      seen.calls.load(), seen.width.load(), seen.height.load(), seen.format.load(), seen.buffer.load(), seen.buffers.load(),
                      seen.fieldsOk.load() ? "set" : "MISSING", quadWrong, barsWrong, pictureWrong);
        check(loaded && seen.calls.load() == 1 && seen.fieldsOk.load() && seen.width.load() == static_cast<uint32_t>(ww) &&
                  seen.height.load() == static_cast<uint32_t>(wh) && seen.format.load() == 28u /* DXGI_FORMAT_R8G8B8A8_UNORM */ &&
                  seen.buffers.load() == 3u && seen.buffer.load() < 3u && quadWrong == 0u && barsWrong == 0u && pictureWrong == 0u,
              what);
        // No overlay: none called (the request without one is as before).
        OverlaySeen none;
        nr_core::PresentRequest plain;
        plain.picture = index;
        const bool again = index >= 0 && nr_core::present(plain);
        check(again && none.calls.load() == 0, "a present without an overlay (S12): presented, nothing else called");
    }

    // (S13a) A layer filled through its shared handle: rects cleared into it (premultiplied values) on the present's command list,
    // before the layer's own overlay draws it. Rows as the texture holds them (layerOverlay(.., true) shows row 0 at the bottom).
    struct LayerFill
    {
        void *handle = nullptr; // (the layer's: opened on the present's device at the first fill)
        ID3D12Resource *resource = nullptr;
        ID3D12DescriptorHeap *rtvHeap = nullptr;
        bool ok = false;
    };
    struct FillRect
    {
        int r[4];        // left, top, right, bottom in the texture's rows
        uint8_t c[4];    // premultiplied RGBA
    };
    nr_core::Overlay fillLayer(LayerFill &fill, const std::vector<FillRect> &rects)
    {
        return [&fill, rects](const nr_core::OverlayTarget &t) {
            auto *dev = static_cast<ID3D12Device *>(t.device);
            auto *l = static_cast<ID3D12GraphicsCommandList *>(t.commandList);
            if (!dev || !l || !fill.handle)
                return;
            if (!fill.resource)
            {
                D3D12_DESCRIPTOR_HEAP_DESC hd{};
                hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
                hd.NumDescriptors = 1;
                if (FAILED(dev->OpenSharedHandle(static_cast<HANDLE>(fill.handle), IID_PPV_ARGS(&fill.resource))) ||
                    FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&fill.rtvHeap))))
                    return;
            }
            const D3D12_CPU_DESCRIPTOR_HANDLE rtv = fill.rtvHeap->GetCPUDescriptorHandleForHeapStart();
            dev->CreateRenderTargetView(fill.resource, nullptr, rtv);
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = fill.resource;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            l->ResourceBarrier(1, &b);
            const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            l->ClearRenderTargetView(rtv, clear, 0, nullptr);
            for (const FillRect &f : rects)
            {
                const float c[4] = {f.c[0] / 255.0f, f.c[1] / 255.0f, f.c[2] / 255.0f, f.c[3] / 255.0f};
                const D3D12_RECT r{f.r[0], f.r[1], f.r[2], f.r[3]};
                l->ClearRenderTargetView(rtv, c, 1, &r);
            }
            std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
            l->ResourceBarrier(1, &b);
            fill.ok = true;
        };
    }
    bool openLayer(const nr_core::Layer &layer, LayerFill &fill)
    {
        fill = LayerFill{};
        fill.handle = layer.handle;
        return layer.handle != nullptr;
    }
    void closeLayer(LayerFill &fill)
    {
        if (fill.resource)
            fill.resource->Release();
        if (fill.rtvHeap)
            fill.rtvHeap->Release();
        fill = LayerFill{};
    }
    void layerCase(uint32_t pw, uint32_t ph, const std::vector<uint8_t> &rgba, uint64_t tick)
    {
        const HWND hwnd = static_cast<HWND>(nr_core::window());
        const int ww = static_cast<int>(pw) + 200, wh = static_cast<int>(ph) + 100;
        resizeClient(hwnd, ww, wh);
        nr_core::pumpEvents();
        nr_core::Layer a, b, c;
        const bool made = nr_core::layer(0, static_cast<uint32_t>(ww), static_cast<uint32_t>(wh), a);
        const bool same = nr_core::layer(0, static_cast<uint32_t>(ww), static_cast<uint32_t>(wh), b);
        char what[400];
        std::snprintf(what, sizeof(what), "a layer (S13a): made %d (%ux%u, handle %s, %llu bytes, generation %llu), asked again: %d (same %s, generation %llu)",
                      made, a.width, a.height, a.handle ? "set" : "MISSING", static_cast<unsigned long long>(a.size),
                      static_cast<unsigned long long>(a.generation), same, b.handle == a.handle ? "texture" : "ANOTHER",
                      static_cast<unsigned long long>(b.generation));
        check(made && same && a.handle && a.size > 0u && a.width == static_cast<uint32_t>(ww) && a.height == static_cast<uint32_t>(wh) &&
                  a.generation == 1u && b.handle == a.handle && b.generation == 1u,
              what);
        LayerFill fill;
        const bool opened = made && openLayer(a, fill);
        // The rects in the texture's rows; shown mirrored (bottomUp): texture row y at window row wh - 1 - y.
        const int ox = 100, oy = 50; // (the picture's place)
        const FillRect opaque{{20, 30, 140, 90}, {10u, 220u, 30u, 255u}};
        const FillRect half{{300, 200, 420, 260}, {100u, 50u, 0u, 128u}};
        const int index = sharedPicture(pw, ph, rgba, tick);
        const char *shot = "present_layer.bmp";
        std::remove(shot);
        nr_core::PresentRequest rq;
        rq.picture = index;
        const float dst[4] = {static_cast<float>(ox), static_cast<float>(oy), static_cast<float>(pw), static_cast<float>(ph)};
        std::memcpy(rq.dst, dst, sizeof(rq.dst));
        rq.clear[0] = 16.0f / 255.0f, rq.clear[1] = 32.0f / 255.0f, rq.clear[2] = 64.0f / 255.0f;
        rq.shot = shot;
        const nr_core::Overlay fillIt = fillLayer(fill, {opaque, half});
        const nr_core::Overlay drawIt = nr_core::layerOverlay(0, true);
        rq.overlay = [fillIt, drawIt](const nr_core::OverlayTarget &t) {
            fillIt(t);
            drawIt(t);
        };
        const bool presented = opened && index >= 0 && nr_core::present(rq);
        uint32_t w = 0, h = 0;
        std::vector<uint8_t> got;
        const bool loaded = presented && loadBmp(shot, w, h, got) && w == static_cast<uint32_t>(ww) && h == static_cast<uint32_t>(wh);
        uint32_t opaqueWrong = 0, halfWrong = 0, halfMost = 0, restWrong = 0;
        auto under = [&](int x, int y, uint8_t out[3]) {
            if (x < ox || y < oy || x >= ox + static_cast<int>(pw) || y >= oy + static_cast<int>(ph))
                out[0] = 16u, out[1] = 32u, out[2] = 64u;
            else
            {
                const uint8_t *e = rgba.data() + (static_cast<size_t>(y - oy) * pw + (x - ox)) * 4u;
                out[0] = e[0], out[1] = e[1], out[2] = e[2];
            }
        };
        auto inside = [](const FillRect &f, int x, int ty) { return x >= f.r[0] && x < f.r[2] && ty >= f.r[1] && ty < f.r[3]; };
        if (loaded)
            for (int y = 0; y < wh; ++y)
                for (int x = 0; x < ww; ++x)
                {
                    const uint8_t *g = got.data() + (static_cast<size_t>(y) * w + x) * 3u;
                    const int ty = wh - 1 - y; // (the texture's row shown here)
                    uint8_t u[3];
                    under(x, y, u);
                    if (inside(opaque, x, ty))
                        opaqueWrong += (g[0] != opaque.c[0] || g[1] != opaque.c[1] || g[2] != opaque.c[2]) ? 1u : 0u;
                    else if (inside(half, x, ty))
                    {
                        bool wrong = false;
                        for (int k = 0; k < 3; ++k)
                        {
                            const float e = half.c[k] + u[k] * (1.0f - half.c[3] / 255.0f);
                            const uint32_t d = static_cast<uint32_t>(std::lround(std::fabs(g[k] - e)));
                            halfMost = std::max(halfMost, d);
                            wrong = wrong || d > 1u;
                        }
                        halfWrong += wrong ? 1u : 0u;
                    }
                    else
                        restWrong += (g[0] != u[0] || g[1] != u[1] || g[2] != u[2]) ? 1u : 0u;
                }
        std::snprintf(what, sizeof(what), "a layer over the picture (S13a, rows bottom up): filled %d, presented %d; the opaque rect wrong %u px, "
                                          "the half-covered one wrong %u px (most %u levels), the rest wrong %u px",
                      fill.ok, presented, opaqueWrong, halfWrong, halfMost, restWrong);
        check(loaded && fill.ok && opaqueWrong == 0u && halfWrong == 0u && restWrong == 0u, what);
        closeLayer(fill);

        // Two layers in one present: layer 1's opaque rect over layer 0's at the same place - layer 1's colour wins (the order kept).
        LayerFill f0, f1;
        nr_core::Layer l1;
        const bool made1 = nr_core::layer(1, static_cast<uint32_t>(ww), static_cast<uint32_t>(wh), l1);
        const bool opened2 = made && made1 && nr_core::layer(0, static_cast<uint32_t>(ww), static_cast<uint32_t>(wh), c) && openLayer(c, f0) &&
                             openLayer(l1, f1);
        const FillRect first{{200, 100, 260, 160}, {255u, 0u, 0u, 255u}};
        const FillRect second{{230, 100, 290, 160}, {0u, 0u, 255u, 255u}};
        const char *shot2 = "present_layers2.bmp";
        std::remove(shot2);
        nr_core::PresentRequest rq2 = rq;
        rq2.picture = sharedPicture(pw, ph, rgba, tick + 1u);
        rq2.shot = shot2;
        const nr_core::Overlay fill0 = fillLayer(f0, {first}), fill1 = fillLayer(f1, {second});
        const nr_core::Overlay draw0 = nr_core::layerOverlay(0, true), draw1 = nr_core::layerOverlay(1, true);
        rq2.overlay = [fill0, fill1, draw0, draw1](const nr_core::OverlayTarget &t) {
            fill0(t);
            fill1(t);
            draw0(t);
            draw1(t);
        };
        const bool presented2 = opened2 && rq2.picture >= 0 && nr_core::present(rq2);
        std::vector<uint8_t> got2;
        uint32_t w2 = 0, h2 = 0, onlyFirst = 0, both = 0, onlySecond = 0;
        const bool loaded2 = presented2 && loadBmp(shot2, w2, h2, got2) && w2 == static_cast<uint32_t>(ww);
        if (loaded2)
            for (int y = 0; y < wh; ++y)
                for (int x = 0; x < ww; ++x)
                {
                    const uint8_t *g = got2.data() + (static_cast<size_t>(y) * w2 + x) * 3u;
                    const int ty = wh - 1 - y;
                    const bool in0 = inside(first, x, ty), in1 = inside(second, x, ty);
                    if (in1)
                        (in0 ? both : onlySecond) += (g[0] != 0u || g[1] != 0u || g[2] != 255u) ? 1u : 0u;
                    else if (in0)
                        onlyFirst += (g[0] != 255u || g[1] != 0u || g[2] != 0u) ? 1u : 0u;
                }
        std::snprintf(what, sizeof(what), "two layers in one present (S13a): the first's own part wrong %u px, the overlap (the second's colour) "
                                          "wrong %u px, the second's own part wrong %u px; generation %llu",
                      onlyFirst, both, onlySecond, static_cast<unsigned long long>(c.generation));
        check(loaded2 && f0.ok && f1.ok && onlyFirst == 0u && both == 0u && onlySecond == 0u && c.generation == 1u, what);
        closeLayer(f0);
        closeLayer(f1);
        // Another size: made again (generation 2).
        nr_core::Layer d;
        const bool remade = nr_core::layer(0, static_cast<uint32_t>(ww - 10), static_cast<uint32_t>(wh - 10), d);
        std::snprintf(what, sizeof(what), "a layer asked at another size (S13a): made again %d, %ux%u, generation %llu", remade, d.width, d.height,
                      static_cast<unsigned long long>(d.generation));
        check(remade && d.width == static_cast<uint32_t>(ww - 10) && d.height == static_cast<uint32_t>(wh - 10) && d.generation == 2u, what);
        nr_core::releaseLayers();
    }

    // The input saved as it is presented (present_input.bmp: the GL side's compare reads the same picture).
    void saveInput(uint32_t w, uint32_t h, const std::vector<uint8_t> &rgba)
    {
        FILE *f = std::fopen("present_input.bmp", "wb");
        if (!f)
            return;
        uint8_t hd[54] = {'B', 'M'};
        auto put = [&](int at, uint32_t v) { std::memcpy(hd + at, &v, 4); };
        put(2, 54u + w * h * 4u);
        put(10, 54u);
        put(14, 40u);
        put(18, w);
        put(22, h);
        hd[26] = 1;
        hd[28] = 32;
        put(34, w * h * 4u);
        std::fwrite(hd, 1, sizeof(hd), f);
        std::vector<uint8_t> row(static_cast<size_t>(w) * 4u);
        for (uint32_t y = h; y-- > 0;)
        {
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint8_t *p = rgba.data() + (static_cast<size_t>(y) * w + x) * 4u;
                row[x * 4u] = p[2], row[x * 4u + 1u] = p[1], row[x * 4u + 2u] = p[0], row[x * 4u + 3u] = 255u;
            }
            std::fwrite(row.data(), 1, row.size(), f);
        }
        std::fclose(f);
    }

    void presentChecks(const char *input)
    {
        uint32_t pw = 640u, ph = 360u;
        std::vector<uint8_t> rgba;
        if (!input || !loadInput(input, pw, ph, rgba))
        {
            pw = 640u, ph = 360u;
            presentPattern(pw, ph, rgba);
        }
        std::printf("[smoke] present: input %s, %ux%u\n", input ? input : "(the test pattern)", pw, ph);
        saveInput(pw, ph, rgba);
        const float fpw = static_cast<float>(pw), fph = static_cast<float>(ph);
        uint64_t tick = 100u;
        // 1:1 into a bordered window: an exact copy, the bars exact.
        const float one[4] = {100.0f, 50.0f, fpw, fph};
        presentCase("copy", pw, ph, rgba, static_cast<int>(pw) + 200, static_cast<int>(ph) + 100, one, 0, true, tick++);
        // A render scale of 75 % (the picture 3/4 of the window each way) and of 50 %, the whole window.
        const float s75[4] = {0.0f, 0.0f, fpw * 4.0f / 3.0f, fph * 4.0f / 3.0f};
        const float s50[4] = {0.0f, 0.0f, fpw * 2.0f, fph * 2.0f};
        for (int up = 0; up < 2; ++up)
        {
            presentCase(up ? "s75_fsr" : "s75_bilinear", pw, ph, rgba, static_cast<int>(s75[2]), static_cast<int>(s75[3]), s75, up, false, tick++);
            presentCase(up ? "s50_fsr" : "s50_bilinear", pw, ph, rgba, static_cast<int>(s50[2]), static_cast<int>(s50[3]), s50, up, false, tick++);
        }
        // Ultrawide letterbox: 16:9 content (the picture at 75 %) in a 21:9 window, bars left and right.
        const float wh = fph * 4.0f / 3.0f, ww = std::round(wh * 21.0f / 9.0f), cw = fpw * 4.0f / 3.0f;
        const float lb[4] = {std::round((ww - cw) * 0.5f), 0.0f, cw, wh};
        for (int up = 0; up < 2; ++up)
            presentCase(up ? "uw_fsr" : "uw_bilinear", pw, ph, rgba, static_cast<int>(ww), static_cast<int>(wh), lb, up, false, tick++);
        // A sync-on present (interval 1) and a tearing one, nothing saved: both accepted.
        nr_core::PresentRequest rq;
        rq.picture = sharedPicture(pw, ph, rgba, tick++);
        rq.syncInterval = 1;
        const bool synced = nr_core::present(rq);
        rq.syncInterval = 0;
        const bool unsynced = nr_core::present(rq);
        check(synced && unsynced, "present with vsync (interval 1) and without (tearing where allowed)");
        overlayCase(pw, ph, rgba, tick++); // (S12)
        layerCase(pw, ph, rgba, tick); // (S13a)
        tick += 2u;

        // (step 7, S10) A submitted frame presented by the core: the picture (pw x ph) into a window as wide and 100 taller -
        // letterboxed to the frame's shape (1:1, 50 above and below), the bars exactly the clear colour, the picture exact.
        const HWND hwnd = static_cast<HWND>(nr_core::window());
        resizeClient(hwnd, static_cast<int>(pw), static_cast<int>(ph) + 100);
        nr_core::pumpEvents();
        const char *wshot = "present_submit.bmp";
        std::remove(wshot);
        nr_core::FrameInfo f;
        f.tick = tick++;
        f.pictureWidth = pw, f.pictureHeight = ph;
        f.aspect = fpw / fph;
        f.waitForGpu = true;
        f.present = 1;
        f.clear[0] = 16.0f / 255.0f, f.clear[1] = 32.0f / 255.0f, f.clear[2] = 64.0f / 255.0f;
        f.windowShot = wshot;
        std::atomic<bool> drew{false}, shown{false};
        f.done = [&shown](bool drawn, bool presented) { shown = drawn && presented; };
        const bool queued = nr_core::submit(
            [&](nr::gfx::Device &dev, nr_core::Recording &rec) {
                using namespace nr;
                const gfx::TextureId tex = dev.createTexture(pw, ph, gfx::Format::RGBA8, 1u);
                gfx::RingSpan cs;
                if (!tex || !dev.ringAlloc(gfx::RingKind::Constants, frame::kConstantStride, 256u, cs))
                    return;
                dev.uploadTexture(tex, 0u, rgba.data(), pw * 4u);
                const float tone[4] = {1.0f, 0.8f, 0.0f, 0.0f};
                std::memcpy(cs.cpu, tone, sizeof(tone));
                dev.use(rec.picture, gfx::UseFor::Draw);
                dev.setTarget(rec.picture, false);
                dev.setViewport(0.0f, 0.0f, fpw, fph);
                dev.setScissor(gfx::Rect{0, 0, static_cast<int32_t>(pw), static_cast<int32_t>(ph)});
                gfx::PipelineDesc pd;
                pd.effect = gfx::Effect::ToneMap;
                if (!dev.setPipeline(pd))
                    return;
                dev.bindDrawConstants(cs);
                dev.bindTextures(tex, 0u, 0u);
                dev.draw(3u, 0u);
                dev.bindTextures(0u, 0u, 0u);
                dev.releaseTexture(tex);
                drew = true;
            },
            f);
        nr_core::flushSubmitted();
        uint32_t w = 0, h = 0;
        std::vector<uint8_t> got;
        const bool loaded = queued && drew.load() && shown.load() && loadBmp(wshot, w, h, got) && w == pw && h == ph + 100u;
        uint32_t barsWrong = 0, inWrong = 0, most = 0;
        if (loaded)
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x)
                {
                    const uint8_t *g = got.data() + (static_cast<size_t>(y) * w + x) * 3u;
                    if (y < 50u || y >= 50u + ph)
                        barsWrong += (g[0] != 16u || g[1] != 32u || g[2] != 64u) ? 1u : 0u;
                    else
                    {
                        const uint8_t *e = rgba.data() + (static_cast<size_t>(y - 50u) * pw + x) * 4u;
                        bool d = false;
                        for (int c = 0; c < 3; ++c)
                        {
                            const uint32_t diff = static_cast<uint32_t>(g[c] > e[c] ? g[c] - e[c] : e[c] - g[c]);
                            most = std::max(most, diff);
                            d = d || diff != 0u;
                        }
                        inWrong += d ? 1u : 0u;
                    }
                }
        char what[200];
        std::snprintf(what, sizeof(what), "a submitted frame presented by the core (%ux%u into %ux%u, letterboxed): saved %ux%u, bars wrong %u px, "
                                          "picture wrong %u px (most %u levels)",
                      pw, ph, pw, ph + 100u, w, h, barsWrong, inWrong, most);
        submitCheck(loaded && barsWrong == 0u && inWrong == 0u, what);

        // (S12) The same submitted frame with an overlay (FrameInfo::overlay, run on the core's drawing thread): the quad exact.
        const char *oshot = "present_submit_overlay.bmp";
        std::remove(oshot);
        OverlaySeen seen;
        nr_core::FrameInfo fo = f;
        fo.tick = tick++;
        fo.windowShot = oshot;
        fo.overlay = quadOverlay(seen);
        std::atomic<bool> shownO{false};
        fo.done = [&shownO](bool drawn, bool presented) { shownO = drawn && presented; };
        std::atomic<DWORD> overlayThread{0};
        const nr_core::Overlay inner = fo.overlay;
        fo.overlay = [&overlayThread, inner](const nr_core::OverlayTarget &t) {
            overlayThread = GetCurrentThreadId();
            inner(t);
        };
        const bool queuedO = nr_core::submit(
            [&](nr::gfx::Device &dev, nr_core::Recording &rec) {
                using namespace nr;
                const gfx::TextureId tex = dev.createTexture(pw, ph, gfx::Format::RGBA8, 1u);
                gfx::RingSpan cs;
                if (!tex || !dev.ringAlloc(gfx::RingKind::Constants, frame::kConstantStride, 256u, cs))
                    return;
                dev.uploadTexture(tex, 0u, rgba.data(), pw * 4u);
                const float tone[4] = {1.0f, 0.8f, 0.0f, 0.0f};
                std::memcpy(cs.cpu, tone, sizeof(tone));
                dev.use(rec.picture, gfx::UseFor::Draw);
                dev.setTarget(rec.picture, false);
                dev.setViewport(0.0f, 0.0f, fpw, fph);
                dev.setScissor(gfx::Rect{0, 0, static_cast<int32_t>(pw), static_cast<int32_t>(ph)});
                gfx::PipelineDesc pd;
                pd.effect = gfx::Effect::ToneMap;
                if (!dev.setPipeline(pd))
                    return;
                dev.bindDrawConstants(cs);
                dev.bindTextures(tex, 0u, 0u);
                dev.draw(3u, 0u);
                dev.bindTextures(0u, 0u, 0u);
                dev.releaseTexture(tex);
            },
            fo);
        nr_core::flushSubmitted();
        std::vector<uint8_t> gotO;
        uint32_t qw = 0, bw = 0, iw = 0;
        const bool loadedO = queuedO && shownO.load() && loadBmp(oshot, w, h, gotO) && w == pw && h == ph + 100u;
        if (loadedO)
            overlayCompare(gotO, w, h, rgba, pw, ph, 0u, 50u, qw, bw, iw);
        std::snprintf(what, sizeof(what), "a submitted frame's overlay (S12): called %d time(s) on %s, the quad wrong %u px, bars wrong %u px, "
                                          "picture wrong %u px",
                      seen.calls.load(), overlayThread.load() != GetCurrentThreadId() ? "the drawing thread" : "THIS thread", qw, bw, iw);
        submitCheck(loadedO && seen.calls.load() == 1 && seen.fieldsOk.load() && overlayThread.load() != 0u &&
                        overlayThread.load() != GetCurrentThreadId() && qw == 0u && bw == 0u && iw == 0u,
                    what);
    }
#endif
}

int main(int argc, char **argv)
{
    bool withWindow = false, show = false, presenting = false;
    const char *input = nullptr;
    for (int i = 1; i < argc; ++i)
    {
        presenting = presenting || std::strcmp(argv[i], "--present") == 0;
        withWindow = withWindow || presenting || std::strcmp(argv[i], "--window") == 0;
        show = show || std::strcmp(argv[i], "--show") == 0;
        if (std::strcmp(argv[i], "--input") == 0 && i + 1 < argc)
            input = argv[++i];
    }
    std::printf("[smoke] nr_core smoke test: the core alone%s\n", withWindow ? ", with its window (--window)" : "");
    nr_core::Settings settings; // (all -1: the defaults)
#if defined(_WIN32)
    if (withWindow)
    {
        listen();
        nr_core::WindowSpec spec;
        spec.create = true;
        spec.title = "nr_core smoke";
        spec.width = 320, spec.height = 200;
        spec.visible = show;
        if (presenting)
        {
            // (the pictures handed over as shared textures, at once - not paced. S11: by the settings alone - the environment's
            // TF_NATIVE_LIVE / TF_PACING taken away, so a present below proves the settings did it)
            _putenv_s("TF_NATIVE_LIVE", "");
            _putenv_s("TF_PACING", "");
            settings.sharedPictures = 1;
            settings.livePictures = 1;
            settings.corePacing = 0;
        }
        check(nr_core::open(settings, spec), "open with a window of the core's own");
        ownWindowChecks();
    }
    else
#endif
        nr_core::open(settings);
    nr::Backend &backend = nr::backend();
    const auto t0 = std::chrono::steady_clock::now();
    // (open() prepares the device on a thread of its own: start() while that is still at work answers "no" - waited for, as a
    // host waits for it; S9b found the race in a run from the interactive session)
    while (backend.startingNow() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(60))
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const bool started = backend.start();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("[smoke] backend start: %s (%.0f ms), drawing: %s, device lost: %s\n", started ? "yes" : "no", ms,
                backend.drawsPictures() ? "yes" : "no", backend.deviceLost() ? "yes" : "no");
    if (!started)
        return 2;
#if defined(_WIN32)
    if (presenting) // (step 7, S11: the core opened by the settings alone)
    {
        char what[200];
        std::snprintf(what, sizeof(what), "opened with livePictures 1, corePacing 0, no TF_NATIVE_LIVE / TF_PACING (%s / %s): pacing mode %d (0)",
                      std::getenv("TF_NATIVE_LIVE") ? "SET" : "unset", std::getenv("TF_PACING") ? "SET" : "unset", backend.pacingMode());
        submitCheck(!std::getenv("TF_NATIVE_LIVE") && !std::getenv("TF_PACING") && backend.pacingMode() == 0, what);
    }
#endif
    nr::gfx::Device &device = nr::gfx::device();
    std::printf("[smoke] device vendor %04x, video memory in use %llu MB\n", device.vendorId(),
                static_cast<unsigned long long>(device.videoMemory(false) >> 20));

    // The expected picture's hash against the stored reference.
    uint64_t want = 0xCBF29CE484222325ull;
    for (uint32_t y = 0; y < kH; ++y)
        for (uint32_t x = 0; x < kW; ++x)
        {
            uint8_t e[3];
            expected(x, y, e);
            for (uint8_t b : e)
                want = fnv(want, b);
        }
    std::printf("[smoke] expected picture's hash %016llx, the stored reference %016llx: %s\n", static_cast<unsigned long long>(want),
                static_cast<unsigned long long>(kReferenceHash), want == kReferenceHash ? "same" : "DIFFERENT");

    const std::shared_ptr<const nr::Texture> ps2 = ps2Texture();
    if (!ps2)
    {
        std::printf("[smoke] SMOKE FAILED (the PS2 texture)\n");
        return 5;
    }
    const char *shot = "nr_core_smoke.bmp";
    std::remove(shot);
    if (!drawStream(shot, *ps2))
        return 3;
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> got;
    if (!loadBmp(shot, w, h, got) || w != kW || h != kH)
    {
        std::printf("[smoke] the device's picture %s did not load as %u x %u (got %u x %u)\n", shot, kW, kH, w, h);
        return 4;
    }
    uint64_t hash = 0xCBF29CE484222325ull;
    uint32_t differ = 0, most = 0, firstX = 0, firstY = 0;
    for (uint32_t y = 0; y < kH; ++y)
        for (uint32_t x = 0; x < kW; ++x)
        {
            uint8_t e[3];
            expected(x, y, e);
            bool d = false;
            for (int c = 0; c < 3; ++c)
            {
                const uint8_t g = got[(static_cast<size_t>(y) * kW + x) * 3u + c];
                hash = fnv(hash, g);
                const uint32_t diff = static_cast<uint32_t>(g > e[c] ? g - e[c] : e[c] - g);
                most = diff > most ? diff : most;
                d = d || diff != 0u;
            }
            if (d && differ++ == 0u)
                firstX = x, firstY = y;
        }
    std::printf("[smoke] the device's picture: hash %016llx, %u of %u pixels differ from the expected one (most by %u levels; first at "
                "%u,%u)\n",
                static_cast<unsigned long long>(hash), differ, kW * kH, most, firstX, firstY);
    const bool ok = differ == 0u && hash == kReferenceHash && want == kReferenceHash;
    if (ok)
        submitChecks(*ps2); // (step 7, S10)
#if defined(_WIN32)
    if (withWindow)
    {
        if (presenting && ok)
            presentChecks(input);
        nr_core::endSubmits(); // (step 7: the core's picture target and swap chain let go of before the window goes)
        ownWindowClose();
        hostWindowChecks();
        std::printf("[smoke] window checks: %d failed\n", g_failed);
        if (ok && g_failed)
        {
            std::printf("[smoke] SMOKE FAILED (the window)\n");
            return 6;
        }
    }
#endif
    nr_core::endSubmits();
    std::printf("[smoke] submit checks: %d failed\n", g_submitFailed);
    if (ok && g_submitFailed)
    {
        std::printf("[smoke] SMOKE FAILED (submit)\n");
        return 7;
    }
    std::printf("[smoke] %s\n", ok ? "SMOKE OK" : "SMOKE FAILED");
    return ok ? 0 : 1;
}
