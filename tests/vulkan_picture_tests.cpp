// tests/vulkan_picture_tests.cpp - the Vulkan backend's CPU hand-over lease (TF_VK_ALONE), exercised against the
// actual private header the backend uses. Standalone C++20, no Vulkan SDK, display or game:
//   g++ -std=c++20 -O2 -pthread tests/vulkan_picture_tests.cpp -o "$TMPDIR/vulkan-picture-tests"
// (compile to a directory outside the repository; the include paths below are relative to this file).
#include "../transformers/native_render/vulkan/nr_picture_mailbox.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace
{
    int g_failures = 0;

    void check(bool ok, const char *what)
    {
        if (!ok)
            std::printf("FAIL: %s\n", what);
        g_failures += ok ? 0 : 1;
    }
}

int main()
{
    using nr::d3d::SharedPicture;
    using nr::vulkan::Published;

    Published mail;
    SharedPicture meta;
    const uint8_t *pixels = nullptr;
    uint32_t rowPitch = 0;
    int index = -1;
    uint64_t serial = 0;

    // A first picture: taken, and from then on the window's - pointer, pixels and metadata - until it says so.
    uint8_t red[2 * 3 * 4];
    std::memset(red, 0x11, sizeof(red));
    check(mail.publish(red, 2, 3, 2 * 4, false, 10, 1000, 900, 7), "a valid first picture was refused");
    check(mail.take(pixels, rowPitch, index, serial, meta), "the first picture was not taken");
    check(meta.width == 2 && meta.height == 3 && !meta.has3D, "the first picture's size or 3D flag was lost");
    check(meta.tick == 10 && meta.dueNs == 1000 && meta.readyNs == 900 && meta.turnId == 7,
          "the first picture's metadata was lost");
    check(rowPitch == 8, "the taken row pitch is not the picture's packed row");
    check(serial != 0, "the first take reported no new serial");
    check(index >= 0, "the first lease handle is negative");

    const uint8_t *redHeld = pixels;
    const int redIndex = index;
    const uint64_t redSerial = serial;
    check(std::memcmp(redHeld, red, sizeof(red)) == 0, "the first picture's pixels were not handed over");

    // A newer, differently sized picture while the window still holds the first: it is not handed over before the
    // acknowledgement, and it does not touch the held pointer or pixels.
    uint8_t green[4 * 2 * 4];
    std::memset(green, 0x22, sizeof(green));
    check(mail.publish(green, 4, 2, 4 * 4, true, 11, 2000, 1900, 8), "a newer picture was refused");
    check(!mail.take(pixels, rowPitch, index, serial, meta), "a newer picture was handed over before the acknowledgement");
    check(pixels == redHeld && rowPitch == 8 && index == redIndex && serial == redSerial,
          "a refused take changed the held picture's pointer or state");
    check(std::memcmp(redHeld, red, sizeof(red)) == 0, "a newer publication overwrote the held picture");

    // The acknowledgement hands over the newest publication, with the metadata that belongs to its pixels.
    mail.shown(redIndex);
    check(mail.take(pixels, rowPitch, index, serial, meta), "no picture was handed over after the acknowledgement");
    check(meta.width == 4 && meta.height == 2 && meta.has3D, "the second picture's size or 3D flag was lost");
    check(meta.tick == 11 && meta.dueNs == 2000 && meta.readyNs == 1900 && meta.turnId == 8,
          "the second picture's metadata does not belong to its pixels");
    check(rowPitch == 16, "the second picture's row pitch is not its packed row");
    check(serial != redSerial, "a new publication did not advance the serial");
    check(index >= 0, "the second lease handle is negative");
    check(std::memcmp(pixels, green, sizeof(green)) == 0, "the second picture's pixels were not handed over");

    const uint8_t *greenHeld = pixels;
    const int greenIndex = index;
    const uint64_t greenSerial = serial;

    // A late acknowledgement of the first take does not release the picture held now; its own does.
    uint8_t blue[4 * 2 * 4];
    std::memset(blue, 0x33, sizeof(blue));
    check(mail.publish(blue, 4, 2, 4 * 4, false, 12, 3000, 2900, 9), "the third picture was refused");
    mail.shown(redIndex);
    check(!mail.take(pixels, rowPitch, index, serial, meta), "a stale acknowledgement released the picture held now");
    check(pixels == greenHeld && rowPitch == 16 && index == greenIndex && serial == greenSerial,
          "the stale acknowledgement changed the held picture's pointer or state");
    check(std::memcmp(greenHeld, green, sizeof(green)) == 0, "the held picture's pixels changed while held");

    mail.shown(greenIndex);
    check(mail.take(pixels, rowPitch, index, serial, meta), "the take after its own acknowledgement failed");
    check(meta.tick == 12 && meta.dueNs == 3000 && meta.readyNs == 2900 && meta.turnId == 9 && !meta.has3D,
          "the third picture's metadata does not belong to its pixels");
    check(serial != greenSerial, "the third take reported the old serial");
    check(index >= 0, "the third lease handle is negative");
    check(std::memcmp(pixels, blue, sizeof(blue)) == 0, "the third picture's pixels were not handed over");
    mail.shown(index);

    // Rows with padding are packed into the hand-over's buffer (rowPitch = width * 4). The padding is poisoned and the
    // two valid rows differ, so a copy of the rows as one contiguous run fails here.
    constexpr uint32_t w = 3, h = 2, rowBytes = w * 4 + 4;
    uint8_t padded[h * rowBytes];
    std::memset(padded, 0xee, sizeof(padded));
    std::memset(padded, 0x44, w * 4);
    std::memset(padded + rowBytes, 0x55, w * 4);
    check(mail.publish(padded, w, h, rowBytes, false, 13, 4000, 3900, 10), "a padded picture was refused");
    check(mail.take(pixels, rowPitch, index, serial, meta), "the padded picture was not taken");
    check(rowPitch == w * 4, "the padded picture's row pitch is not its packed row");
    check(meta.width == w && meta.height == h, "the padded picture's size was lost");
    uint8_t packed[w * 4 * h];
    std::memset(packed, 0x44, w * 4);
    std::memset(packed + w * 4, 0x55, w * 4);
    check(std::memcmp(pixels, packed, sizeof(packed)) == 0, "the padded rows were not packed into their valid pixels");
    mail.shown(index);

    // A consumer that starts its serial again is not handed the picture it already consumed: only a new publication
    // can be taken (the consumed picture no longer sits in the pending buffer).
    uint64_t fresh = 0;
    check(!mail.take(pixels, rowPitch, index, fresh, meta), "a reset serial took the consumed picture");
    check(fresh == 0, "a refused take updated the reset serial");

    uint8_t yellow[2 * 2 * 4];
    std::memset(yellow, 0x66, sizeof(yellow));
    check(mail.publish(yellow, 2, 2, 2 * 4, true, 14, 5000, 4900, 11), "a fresh publication after a reset was refused");
    check(mail.take(pixels, rowPitch, index, fresh, meta), "a fresh publication after a reset serial was not taken");
    check(fresh != 0, "the fresh take reported no new serial");
    check(meta.tick == 14 && meta.dueNs == 5000 && meta.readyNs == 4900 && meta.turnId == 11 && meta.has3D,
          "the fresh picture's metadata does not belong to its pixels");
    check(std::memcmp(pixels, yellow, sizeof(yellow)) == 0, "the fresh picture's pixels were not handed over");
    mail.shown(index);

    // The lease handle is an opaque nonnegative int: nr_present acknowledges only index >= 0, across the signed
    // 32-bit boundary too.
    // shortcut: the counter is poked to reach the boundary instead of taking 2^31 pictures; upgrade if the handle stops
    // being a wrapping uint32_t.
    mail.lease = 0x7fffffffu;
    check(mail.publish(yellow, 2, 2, 2 * 4, false, 15, 6000, 5900, 12), "the boundary picture was refused");
    check(mail.take(pixels, rowPitch, index, fresh, meta), "the boundary picture was not taken");
    check(index >= 0, "the lease handle went negative at the signed boundary");
    mail.shown(index);
    check(mail.publish(yellow, 2, 2, 2 * 4, false, 16, 7000, 6900, 13), "the picture after the boundary was refused");
    check(mail.take(pixels, rowPitch, index, fresh, meta), "the picture after the boundary was not taken");
    check(index >= 0, "the lease handle stayed negative after the boundary");
    mail.shown(index);

    if (g_failures)
        return 1;
    std::printf("vulkan picture mailbox tests passed\n");
    return 0;
}
