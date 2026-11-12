// nr_vulkan_window.h - the Vulkan backend presenting the game's own window (milestone 3), inside the backend's library.
//
// Under SDL3's window (PS2X_PLATFORM=sdl3, ps2_platform.h) with TF_GFX_API=vulkan the window is a Vulkan window and
// this file draws it: a presenter of the platform's (ps2_platform::Presenter) on the main thread, with a swapchain, a
// command buffer and a submit of its own - the frame's own work never waits for the window (with FIFO, PR 14's review
// measured a hidden window holding a GTX 1080 at 30 fps instead of 108). Each window picture is one ImGui frame
// (ps2_platform.h): the game's picture - the native one, or the GS's, a movie, black - as its first image, the overlays
// (the FPS line, notices, the settings panel, the F1 menu, the debugger) over it, drawn by imgui_impl_vulkan with
// dynamic rendering.
//
// The native picture reaches the window without the CPU: at its frame's end the render thread copies it into a window
// picture of its own (takeFree), and the frame's submit signals a timeline semaphore with the picture's value. The
// shared pacer (nr_pace.h) hands it over (published) at its frame's moment plus the steady delay, once the timeline has
// passed the value - as Direct3D 12's shared pictures are; the window loop then draws and presents it. A window picture
// the pacer still holds, or the window still draws (its present not done), is never written.
//
// Presents: FIFO by default (TF_VK_PRESENT=mailbox / immediate as before), with VK_KHR_present_id + present_wait where
// the driver offers them - the window then waits until its picture before the last is on the screen before it draws the
// next (at most one picture queued: the pictures leave when the pacer hands them over, not when FIFO lets them), and
// the times the pictures reach the screen give the refresh where the system tells none (kayliox's Linux note).
#pragma once

#if defined(TF_VULKAN)

#include <volk.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <mutex>

struct SDL_Window;

namespace nr::vk::window
{
    // ---- set up -----------------------------------------------------------------------------------------------------
    // Main thread, before the window is made: under SDL3's platform the presenter is this file's (the window then is a
    // Vulkan window). True when it is.
    bool beforeWindow();
    // The window presented here (beforeWindow said yes and the platform made it).
    bool on();
    SDL_Window *sdlWindow();

    // The device, once made (the device thread): what the presenter needs of it. `queueMutex` guards the queue: every
    // vkQueueSubmit / vkQueuePresentKHR / vkQueueWaitIdle of both threads holds it.
    struct Device
    {
        VkInstance instance = VK_NULL_HANDLE;
        VkPhysicalDevice phys = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        uint32_t family = 0u;
        VkQueue queue = VK_NULL_HANDLE;
        VmaAllocator vma = nullptr;
        uint32_t apiVersion = 0u;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        bool presentId = false, presentWait = false;
        VkSemaphore timeline = VK_NULL_HANDLE; // (the pictures' values: signalled by the frames' submits)
    };
    std::mutex &queueMutex();
    void deviceReady(const Device &device);
    // (before the device goes: the presenter's own objects let go, on the calling thread with the queue idle)
    void deviceGone();

    // ---- the render thread's pictures -----------------------------------------------------------------------------
    struct Target
    {
        int index = -1;
        VkImage image = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED; // (as the last copy left it)
    };
    // A window picture of that size to copy the frame's picture into (made or made again as needed; held as pending until
    // published): false when none is free (the newest, the one drawn, those the pacer holds) - this frame's picture is
    // then not shown.
    bool takeFree(uint32_t width, uint32_t height, VkFormat format, Target &out);
    // Its hand-over (the pacer's, on the pacer's thread; or at the submit without the pacer): the copy recorded (the
    // picture left in SHADER_READ_ONLY_OPTIMAL), the frame submitted with the timeline's `value` - the newest picture for
    // the window from now on. `dueNs` (its frame's moment), `readyNs` (the GPU seen done; 0: now) and `has3D` for the
    // pacing log.
    void published(int index, uint64_t value, bool has3D, uint64_t tick, int64_t dueNs, int64_t readyNs);
    // The next value to signal (the render thread's own count).
    uint64_t nextValue();

    // ---- the window's thread (nr_present.cpp) -------------------------------------------------------------------
    // The newest published picture as a texture for the window's drawing (ps2_platform's id, for DrawTexturePro), its
    // size and serial: false when there is none (yet, or the device lost).
    bool latest(unsigned int &texture, int &width, int &height, uint64_t &serial, bool &has3D, uint64_t &tick, int64_t &dueNs,
                int64_t &readyNs);
    // Waits until a picture newer than `serial` is published, or until `untilNs` (steady clock): true when one is. lastNs:
    // when the last one was.
    bool waitNewer(uint64_t serial, int64_t untilNs, int64_t &lastNs);

    // Figures: pictures presented / not presented since the last call (the backend's 10 s line), and the screen's
    // refresh seen in the presents (ms between refreshes; 0 unknown).
    void takeCounts(uint64_t &presented, uint64_t &notPresented);
    double refreshMs();
}

#endif
