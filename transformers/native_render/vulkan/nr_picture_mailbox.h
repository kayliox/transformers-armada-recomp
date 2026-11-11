// nr_picture_mailbox.h - the CPU hand-over's mailbox (TF_VK_ALONE), in a header of its own so
// tests/vulkan_picture_tests.cpp can exercise the backend's actual lease without the Vulkan SDK; nr_vulkan.cpp is its
// production user.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include "../nr_glshare.h" // (d3d::SharedPicture: the hand-over's metadata)

namespace nr::vulkan
{
    // TF_VK_ALONE: the newest picture as CPU pixels for the main window (latestPicture / pictureShown / waitPicture).
    // The window's picture stays as it is - its pointer and pixels - until pictureShown, newer publications filling
    // the other buffer; the two buffers swap at each take, so their capacity is kept.
    struct Published
    {
        std::mutex mutex;
        std::condition_variable cv;
        std::vector<uint8_t> pixels; // (the next take's picture)
        std::vector<uint8_t> held;   // (the window's, from a take until pictureShown)
        d3d::SharedPicture meta;     // (the newest publication: size, has3D, tick, moment, turn)
        uint64_t serial = 0;
        int64_t lastNs = 0;
        bool taken = false;
        uint32_t lease = 0;

        // (The render thread) The read-back's rows copied in (`rowBytes` a row, RGBA8), the newest replacing one not
        // taken yet; false: nothing copied (no rows, or a row shorter than the picture's width * 4).
        bool publish(const uint8_t *rows, uint32_t width, uint32_t height, uint32_t rowBytes, bool has3D, uint64_t tick,
                     int64_t dueNs, int64_t readyNs, uint64_t turnId)
        {
            const std::size_t row = static_cast<std::size_t>(width) * 4u;
            if (!rows || !width || !height || rowBytes < row)
                return false;
            std::lock_guard<std::mutex> lock(mutex);
            pixels.resize(row * height);
            if (rowBytes == row)
                std::memcpy(pixels.data(), rows, row * height);
            else
                for (uint32_t y = 0; y < height; ++y)
                    std::memcpy(pixels.data() + static_cast<std::size_t>(y) * row, rows + static_cast<std::size_t>(y) * rowBytes, row);
            meta = d3d::SharedPicture{};
            meta.width = width;
            meta.height = height;
            meta.has3D = has3D;
            meta.tick = tick;
            meta.dueNs = dueNs;
            meta.readyNs = readyNs;
            meta.turnId = turnId;
            ++serial;
            lastNs = readyNs;
            cv.notify_all();
            return true;
        }

        // (The window's thread) The newest picture published since `since` (then updated), as the window's until
        // shown(): its pointer and pixels do not change before that. False: none newer, or one is still held.
        bool take(const uint8_t *&out, uint32_t &rowPitch, int &index, uint64_t &since, d3d::SharedPicture &outMeta)
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (taken || serial == since || pixels.empty())
                return false;
            std::swap(pixels, held);
            pixels.clear(); // (the consumed picture is not pending again - its capacity kept: a reset serial takes only a new one)
            out = held.data();
            rowPitch = meta.width * 4u;
            outMeta = meta;
            since = serial;
            lease = lease >= 0x7fffffffu ? 1u : lease + 1u; // (nr_present acknowledges only nonnegative handles)
            index = static_cast<int>(lease);
            taken = true;
            return true;
        }

        // (The window's thread) Its upload has returned: the buffer may be filled again. An ack of an older take
        // never releases the one held now.
        void shown(int index)
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (taken && static_cast<uint32_t>(index) == lease)
                taken = false;
        }

        // (The window's thread; the pacer) Waits until a picture newer than `since` is published or until `untilNs`
        // (steady clock): true when one is. `outLastNs`: when the newest was published (0: none yet).
        bool wait(uint64_t since, int64_t untilNs, int64_t &outLastNs)
        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait_until(lock, std::chrono::steady_clock::time_point(std::chrono::nanoseconds(untilNs)),
                          [this, since] { return serial != since; });
            outLastNs = lastNs;
            return serial != since;
        }
    };
}
