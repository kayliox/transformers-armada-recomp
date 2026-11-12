// nr_vulkan_window.cpp - the Vulkan backend presenting the game's own window (nr_vulkan_window.h).
#if defined(TF_VULKAN) && !defined(TF_VK_GAME_WINDOW)
// (a build without SDL3's platform, PS2X_SDL3 off: the backend keeps its development window - nothing here)
#include "nr_vulkan_window.h"
namespace nr::vk::window
{
    bool beforeWindow() { return false; }
    bool on() { return false; }
    SDL_Window *sdlWindow() { return nullptr; }
    std::mutex &queueMutex()
    {
        static std::mutex m;
        return m;
    }
    void deviceReady(const Device &) {}
    void deviceGone() {}
    bool takeFree(uint32_t, uint32_t, VkFormat, Target &) { return false; }
    void published(int, uint64_t, bool, uint64_t, int64_t, int64_t) {}
    uint64_t nextValue() { return 0u; }
    bool latest(unsigned int &, int &, int &, uint64_t &, bool &, uint64_t &, int64_t &, int64_t &) { return false; }
    bool waitNewer(uint64_t, int64_t, int64_t &) { return false; }
    void takeCounts(uint64_t &presented, uint64_t &notPresented) { presented = notPresented = 0u; }
    double refreshMs() { return 0.0; }
}
#elif defined(TF_VULKAN)

#include "nr_vulkan_window.h"

#include "runtime/ps2_host_present.h"
#include "runtime/ps2_platform.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <thread>
#include <vector>

namespace nr::vk::window
{
    namespace
    {
        using Clock = std::chrono::steady_clock;
        // (window pictures: the newest, the one the window draws, and those the pacer holds until their moment - nr_pace.h;
        // with none free a frame's picture is left out of the window)
        constexpr int kPictures = 6, kInFlight = 2;

        int64_t nowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }

        bool ok(VkResult r, const char *what)
        {
            if (r == VK_SUCCESS)
                return true;
            std::fprintf(stderr, "[vulkan window] %s: VkResult %d\n", what, static_cast<int>(r));
            return false;
        }

        // A window picture: the render thread copies its frame's picture into it, the window samples it.
        struct Picture
        {
            VkImage image = VK_NULL_HANDLE;
            VmaAllocation alloc = nullptr;
            VkImageView view = VK_NULL_HANDLE;
            VkFormat format = VK_FORMAT_UNDEFINED;
            uint32_t w = 0, h = 0;
            VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
            uint64_t generation = 0, value = 0, tick = 0;
            int64_t dueNs = 0, readyNs = 0;
            bool has3D = false;
            int heldBy = -1;      // (the window's frame in flight that draws it)
            bool pending = false; // (copied into, not handed over yet: the pacer holds it until its moment)
            // (the window's own: its ImGui descriptor set, for `setGeneration`)
            VkDescriptorSet set = VK_NULL_HANDLE;
            uint64_t setGeneration = 0;
        };

        struct Retired
        {
            VkImage image = VK_NULL_HANDLE;
            VmaAllocation alloc = nullptr;
            VkImageView view = VK_NULL_HANDLE;
            VkDescriptorSet set = VK_NULL_HANDLE;
        };

        // A texture of the platform's (the GS's picture, a movie, black): its pixels kept on the CPU and copied up at
        // the present after they change.
        struct Tex
        {
            int w = 0, h = 0;
            bool linear = false, dirty = true, setStale = true;
            std::vector<uint8_t> pixels;
            VkImage image = VK_NULL_HANDLE;
            VmaAllocation alloc = nullptr;
            VkImageView view = VK_NULL_HANDLE;
            VkDescriptorSet set = VK_NULL_HANDLE;
            VkBuffer staging = VK_NULL_HANDLE;
            VmaAllocation stagingAlloc = nullptr;
            void *mapped = nullptr;
            bool fresh = true; // (its image UNDEFINED yet)
        };

        struct State
        {
            bool wanted = false;
            SDL_Window *window = nullptr;
            std::mutex queueMutex;
            // the device (set by the device thread)
            std::mutex devMutex;
            Device dev;
            std::atomic<bool> devReady{false};
            // the pictures (render thread <-> window)
            std::mutex picMutex;
            std::condition_variable picCv;
            Picture pics[kPictures];
            int newest = -1;
            uint64_t serial = 0, nextValue = 0;
            int64_t lastNs = 0;
            std::vector<Retired> retired; // (let go by the window after its frames in flight)
            // the window's own (the main thread)
            bool imguiReady = false;
            VkSwapchainKHR swapchain = VK_NULL_HANDLE;
            VkFormat format = VK_FORMAT_UNDEFINED;
            VkExtent2D extent{};
            VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
            int syncAsked = -99;
            bool stale = false, transferSrc = false;
            std::vector<VkImage> images;
            std::vector<VkImageView> views;
            std::vector<VkSemaphore> renderDone;
            VkSemaphore acquired[kInFlight] = {};
            VkFence fence[kInFlight] = {};
            bool inFlight[kInFlight] = {};
            VkCommandPool pool = VK_NULL_HANDLE;
            VkCommandBuffer cmd[kInFlight] = {};
            int frame = 0;
            VkSampler linear = VK_NULL_HANDLE, nearest = VK_NULL_HANDLE;
            // (a hidden or minimised window: the overlays drawn into an image of the window's size instead - for shots)
            VkImage offscreen = VK_NULL_HANDLE;
            VmaAllocation offscreenAlloc = nullptr;
            VkImageView offscreenView = VK_NULL_HANDLE;
            VkExtent2D offscreenExtent{};
            std::vector<Tex *> textures;
            std::vector<Tex *> texRetired; // (let go once both frames in flight are done: either may draw them)
            // this frame
            bool frameOpen = false;
            int drawn = -1; // (the window picture this frame draws)
            uint64_t drawnValue = 0;
            unsigned int pictureTexture = 0u;
            VkDescriptorSet pictureSet = VK_NULL_HANDLE;
            // shots (PS2X_SHOT_KEEP=1, or after the first): each picture read back
            bool keep = false;
            VkBuffer shotBuf[kInFlight] = {};
            VmaAllocation shotAlloc[kInFlight] = {};
            void *shotMapped[kInFlight] = {};
            VkExtent2D shotExtent[kInFlight] = {};
            bool shotTaken[kInFlight] = {}, shotBgra[kInFlight] = {};
            std::vector<uint8_t> last;
            int lastW = 0, lastH = 0;
            // presents and their times on the screen (present_wait)
            uint64_t presentId = 0;
            std::deque<int64_t> shownNs;
            std::atomic<double> refreshMs{0.0};
            std::atomic<uint64_t> presented{0}, notPresented{0};
            bool told = false;
        };
        State &S()
        {
            static State s;
            return s;
        }

        void barrier(VkCommandBuffer c, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                     VkPipelineStageFlags dstStage, VkAccessFlags dstAccess)
        {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.srcAccessMask = srcAccess;
            b.dstAccessMask = dstAccess;
            b.oldLayout = from;
            b.newLayout = to;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
            vkCmdPipelineBarrier(c, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
        }

        VkImageView makeView(VkImage image, VkFormat format)
        {
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = image;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = format;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
            VkImageView v = VK_NULL_HANDLE;
            ok(vkCreateImageView(S().dev.device, &vi, nullptr, &v), "vkCreateImageView");
            return v;
        }

        bool makeImage(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage, VkImage &image, VmaAllocation &alloc)
        {
            VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ci.imageType = VK_IMAGE_TYPE_2D;
            ci.format = format;
            ci.extent = {w, h, 1u};
            ci.mipLevels = ci.arrayLayers = 1u;
            ci.samples = VK_SAMPLE_COUNT_1_BIT;
            ci.tiling = VK_IMAGE_TILING_OPTIMAL;
            ci.usage = usage;
            ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VmaAllocationCreateInfo ai{};
            ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            return ok(vmaCreateImage(S().dev.vma, &ci, &ai, &image, &alloc, nullptr), "vmaCreateImage (window)");
        }

        bool makeHostBuffer(VkDeviceSize bytes, VkBufferUsageFlags usage, bool read, VkBuffer &buf, VmaAllocation &alloc, void *&mapped)
        {
            VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bi.size = bytes;
            bi.usage = usage;
            VmaAllocationCreateInfo ai{};
            ai.usage = VMA_MEMORY_USAGE_AUTO;
            ai.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                       (read ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
            VmaAllocationInfo info{};
            if (!ok(vmaCreateBuffer(S().dev.vma, &bi, &ai, &buf, &alloc, &info), "vmaCreateBuffer (window)"))
                return false;
            mapped = info.pMappedData;
            return mapped != nullptr;
        }

        void dropPictureObjects(Retired &r)
        {
            State &s = S();
            if (r.set)
                ImGui_ImplVulkan_RemoveTexture(r.set);
            if (r.view)
                vkDestroyImageView(s.dev.device, r.view, nullptr);
            if (r.image)
                vmaDestroyImage(s.dev.vma, r.image, r.alloc);
            r = Retired{};
        }

        void dropTex(Tex *t)
        {
            State &s = S();
            if (t->set && s.imguiReady)
                ImGui_ImplVulkan_RemoveTexture(t->set);
            if (t->view)
                vkDestroyImageView(s.dev.device, t->view, nullptr);
            if (t->image)
                vmaDestroyImage(s.dev.vma, t->image, t->alloc);
            if (t->staging)
                vmaDestroyBuffer(s.dev.vma, t->staging, t->stagingAlloc);
            delete t;
        }

        // ---- the swapchain ------------------------------------------------------------------------------------------
        void dropSwapchainParts()
        {
            State &s = S();
            for (VkSemaphore x : s.renderDone)
                vkDestroySemaphore(s.dev.device, x, nullptr);
            for (VkImageView v : s.views)
                vkDestroyImageView(s.dev.device, v, nullptr);
            s.renderDone.clear();
            s.views.clear();
            s.images.clear();
        }

        VkPresentModeKHR modeWanted()
        {
            State &s = S();
            uint32_t n = 0;
            vkGetPhysicalDeviceSurfacePresentModesKHR(s.dev.phys, s.dev.surface, &n, nullptr);
            std::vector<VkPresentModeKHR> modes(n);
            vkGetPhysicalDeviceSurfacePresentModesKHR(s.dev.phys, s.dev.surface, &n, modes.data());
            const auto has = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
            // TF_VK_PRESENT as before; else the VSync setting (ps2_host_present::sync: 1 always, 0 never, -1 by the screen)
            const char *want = std::getenv("TF_VK_PRESENT");
            if (want && std::strcmp(want, "mailbox") == 0 && has(VK_PRESENT_MODE_MAILBOX_KHR))
                return VK_PRESENT_MODE_MAILBOX_KHR;
            if (want && std::strcmp(want, "immediate") == 0 && has(VK_PRESENT_MODE_IMMEDIATE_KHR))
                return VK_PRESENT_MODE_IMMEDIATE_KHR;
            if (want && std::strcmp(want, "fifo") == 0)
                return VK_PRESENT_MODE_FIFO_KHR;
            const int sync = ps2_host_present::sync();
            if (sync == 0)
                return has(VK_PRESENT_MODE_IMMEDIATE_KHR) ? VK_PRESENT_MODE_IMMEDIATE_KHR
                       : has(VK_PRESENT_MODE_MAILBOX_KHR) ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;
            if (sync < 0 && has(VK_PRESENT_MODE_FIFO_RELAXED_KHR))
                return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
            return VK_PRESENT_MODE_FIFO_KHR;
        }

        const char *modeName(VkPresentModeKHR m)
        {
            switch (m)
            {
            case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate";
            case VK_PRESENT_MODE_MAILBOX_KHR: return "mailbox";
            case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO relaxed";
            default: return "FIFO";
            }
        }

        bool makeSwapchain(uint32_t width, uint32_t height)
        {
            State &s = S();
            VkSurfaceCapabilitiesKHR caps{};
            if (!ok(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s.dev.phys, s.dev.surface, &caps), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"))
                return false;
            VkExtent2D extent = caps.currentExtent.width != 0xFFFFFFFFu ? caps.currentExtent : VkExtent2D{width, height};
            if (extent.width == 0u || extent.height == 0u)
                return false; // (minimised)
            if (s.format == VK_FORMAT_UNDEFINED)
            {
                uint32_t n = 0;
                vkGetPhysicalDeviceSurfaceFormatsKHR(s.dev.phys, s.dev.surface, &n, nullptr);
                std::vector<VkSurfaceFormatKHR> formats(n);
                vkGetPhysicalDeviceSurfaceFormatsKHR(s.dev.phys, s.dev.surface, &n, formats.data());
                // (UNORM, not sRGB: the picture's values go to the screen as they are, as with the blit and D3D12's)
                for (const VkSurfaceFormatKHR &f : formats)
                    if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                    {
                        s.format = f.format;
                        break;
                    }
                if (s.format == VK_FORMAT_UNDEFINED && !formats.empty())
                    s.format = formats[0].format;
            }
            s.mode = modeWanted();
            s.syncAsked = ps2_host_present::sync();
            uint32_t count = std::max(caps.minImageCount + 1u, 3u);
            if (caps.maxImageCount)
                count = std::min(count, caps.maxImageCount);
            s.transferSrc = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
            VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
            ci.surface = s.dev.surface;
            ci.minImageCount = count;
            ci.imageFormat = s.format;
            ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            ci.imageExtent = extent;
            ci.imageArrayLayers = 1u;
            ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (s.transferSrc ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0u);
            ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            ci.preTransform = caps.currentTransform;
            ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                                                                                    : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
            ci.presentMode = s.mode;
            ci.clipped = VK_TRUE;
            ci.oldSwapchain = s.swapchain;
            VkSwapchainKHR made = VK_NULL_HANDLE;
            const VkResult r = vkCreateSwapchainKHR(s.dev.device, &ci, nullptr, &made);
            if (s.swapchain)
            {
                {
                    std::lock_guard<std::mutex> q(s.queueMutex);
                    vkQueueWaitIdle(s.dev.queue); // (the old one's presents done before it goes)
                }
                dropSwapchainParts();
                vkDestroySwapchainKHR(s.dev.device, s.swapchain, nullptr);
                s.swapchain = VK_NULL_HANDLE;
            }
            if (!ok(r, "vkCreateSwapchainKHR"))
                return false;
            s.swapchain = made;
            uint32_t n = 0;
            vkGetSwapchainImagesKHR(s.dev.device, made, &n, nullptr);
            s.images.resize(n);
            vkGetSwapchainImagesKHR(s.dev.device, made, &n, s.images.data());
            VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            for (VkImage im : s.images)
            {
                s.views.push_back(makeView(im, s.format));
                VkSemaphore x = VK_NULL_HANDLE;
                ok(vkCreateSemaphore(s.dev.device, &si, nullptr, &x), "vkCreateSemaphore (window)");
                s.renderDone.push_back(x);
            }
            s.extent = extent;
            s.stale = false;
            std::fprintf(stderr, "[vulkan window] swapchain %ux%u, %u images, %s%s\n", extent.width, extent.height, n, modeName(s.mode),
                         s.dev.presentWait ? ", present_wait" : "");
            if (s.imguiReady)
                ImGui_ImplVulkan_SetMinImageCount(std::max(2u, caps.minImageCount));
            return true;
        }

        // ---- the window's own objects, once the device is there ---------------------------------------------------
        bool ready()
        {
            State &s = S();
            if (s.imguiReady)
                return true;
            if (!s.devReady.load(std::memory_order_acquire))
                return false;
            if (s.told)
                return false;
            s.told = true; // (one try)
            VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pi.queueFamilyIndex = s.dev.family;
            if (!ok(vkCreateCommandPool(s.dev.device, &pi, nullptr, &s.pool), "vkCreateCommandPool (window)"))
                return false;
            VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            ai.commandPool = s.pool;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = kInFlight;
            if (!ok(vkAllocateCommandBuffers(s.dev.device, &ai, s.cmd), "vkAllocateCommandBuffers (window)"))
                return false;
            VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            for (int i = 0; i < kInFlight; ++i)
                if (!ok(vkCreateSemaphore(s.dev.device, &si, nullptr, &s.acquired[i]), "vkCreateSemaphore (window)") ||
                    !ok(vkCreateFence(s.dev.device, &fi, nullptr, &s.fence[i]), "vkCreateFence (window)"))
                    return false;
            VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            sc.magFilter = sc.minFilter = VK_FILTER_LINEAR;
            sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sc.maxLod = 0.0f;
            ok(vkCreateSampler(s.dev.device, &sc, nullptr, &s.linear), "vkCreateSampler (window)");
            sc.magFilter = sc.minFilter = VK_FILTER_NEAREST;
            ok(vkCreateSampler(s.dev.device, &sc, nullptr, &s.nearest), "vkCreateSampler (window)");
            int pw = 0, ph = 0;
            SDL_GetWindowSizeInPixels(s.window, &pw, &ph);
            if (!makeSwapchain(static_cast<uint32_t>(std::max(pw, 1)), static_cast<uint32_t>(std::max(ph, 1))) && s.format == VK_FORMAT_UNDEFINED)
                s.format = VK_FORMAT_B8G8R8A8_UNORM; // (minimised at the start: the overlays' pipeline for the usual format)
            ImGui_ImplVulkan_InitInfo ii{};
            ii.ApiVersion = s.dev.apiVersion;
            ii.Instance = s.dev.instance;
            ii.PhysicalDevice = s.dev.phys;
            ii.Device = s.dev.device;
            ii.QueueFamily = s.dev.family;
            ii.Queue = s.dev.queue;
            ii.DescriptorPoolSize = 256u;
            ii.MinImageCount = 2u;
            ii.ImageCount = std::max<uint32_t>(2u, static_cast<uint32_t>(s.images.size()));
            ii.UseDynamicRendering = true;
            ii.PipelineInfoMain.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
            ii.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1u;
            ii.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &s.format;
            ii.CheckVkResultFn = [](VkResult r) { if (r != VK_SUCCESS) std::fprintf(stderr, "[vulkan window] ImGui's Vulkan: VkResult %d\n", static_cast<int>(r)); };
            {
                std::lock_guard<std::mutex> q(s.queueMutex);
                if (!ImGui_ImplVulkan_Init(&ii))
                {
                    std::fprintf(stderr, "[vulkan window] ImGui's Vulkan renderer could not start: no pictures in the window\n");
                    return false;
                }
            }
            const char *keep = std::getenv("PS2X_SHOT_KEEP");
            s.keep = keep && *keep == '1';
            s.imguiReady = true;
            std::fprintf(stderr, "[vulkan window] the game's window is presented by Vulkan (ImGui %s for the overlays, dynamic rendering)\n", IMGUI_VERSION);
            return true;
        }

        // A frame's objects free again: its fence passed - what it drew may be written, what it let go of goes.
        void frameDone(int f)
        {
            State &s = S();
            if (s.inFlight[f])
            {
                vkWaitForFences(s.dev.device, 1, &s.fence[f], VK_TRUE, 5ull * 1000000000ull);
                vkResetFences(s.dev.device, 1, &s.fence[f]);
                s.inFlight[f] = false;
            }
            {
                std::lock_guard<std::mutex> lock(s.picMutex);
                for (Picture &p : s.pics)
                    if (p.heldBy == f)
                        p.heldBy = -1;
            }
            // (the shot this frame read back, as RGBA rows)
            if (s.shotTaken[f] && s.shotMapped[f])
            {
                s.shotTaken[f] = false;
                const uint32_t w = s.shotExtent[f].width, h = s.shotExtent[f].height;
                vmaInvalidateAllocation(s.dev.vma, s.shotAlloc[f], 0, VK_WHOLE_SIZE);
                s.last.resize(static_cast<size_t>(w) * h * 4u);
                std::memcpy(s.last.data(), s.shotMapped[f], s.last.size());
                if (s.shotBgra[f])
                    for (size_t i = 0; i < s.last.size(); i += 4u)
                        std::swap(s.last[i], s.last[i + 2u]);
                for (size_t i = 3; i < s.last.size(); i += 4u)
                    s.last[i] = 255u;
                s.lastW = static_cast<int>(w);
                s.lastH = static_cast<int>(h);
            }
        }

        // ---- the platform's presenter -------------------------------------------------------------------------------
        bool open(void *window)
        {
            S().window = static_cast<SDL_Window *>(window);
            return true; // (the device comes later: until then no picture is drawn)
        }

        void close()
        {
            deviceGone();
        }

        bool newFrame()
        {
            State &s = S();
            if (!ready())
                return false;
            // Both frames' work done before this one changes the textures' pixels or the pictures held (the window's
            // work is small: a copy and the overlays).
            for (int f = 0; f < kInFlight; ++f)
                frameDone(f);
            for (Tex *t : s.texRetired)
                dropTex(t);
            s.texRetired.clear();
            // Window pictures let go by the render thread (made again at another size): their sets go now.
            {
                std::lock_guard<std::mutex> lock(s.picMutex);
                for (Retired &r : s.retired)
                    dropPictureObjects(r);
                s.retired.clear();
            }
            s.drawn = -1;
            s.drawnValue = 0;
            s.frameOpen = true;
            ImGui_ImplVulkan_NewFrame();
            return true;
        }

        void uploadTextures(VkCommandBuffer c)
        {
            State &s = S();
            for (Tex *t : s.textures)
            {
                if (!t->dirty || !t->image || !t->mapped)
                    continue;
                t->dirty = false;
                std::memcpy(t->mapped, t->pixels.data(), t->pixels.size());
                vmaFlushAllocation(s.dev.vma, t->stagingAlloc, 0, VK_WHOLE_SIZE);
                barrier(c, t->image, t->fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
                t->fresh = false;
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
                region.imageExtent = {static_cast<uint32_t>(t->w), static_cast<uint32_t>(t->h), 1u};
                vkCmdCopyBufferToImage(c, t->staging, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                barrier(c, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            }
        }

        bool ensureOffscreen(uint32_t w, uint32_t h)
        {
            State &s = S();
            if (s.offscreen && s.offscreenExtent.width == w && s.offscreenExtent.height == h)
                return true;
            if (s.offscreen)
            {
                {
                    std::lock_guard<std::mutex> q(s.queueMutex);
                    vkQueueWaitIdle(s.dev.queue);
                }
                vkDestroyImageView(s.dev.device, s.offscreenView, nullptr);
                vmaDestroyImage(s.dev.vma, s.offscreen, s.offscreenAlloc);
                s.offscreen = VK_NULL_HANDLE;
            }
            if (!makeImage(w, h, s.format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, s.offscreen, s.offscreenAlloc))
                return false;
            s.offscreenView = makeView(s.offscreen, s.format);
            s.offscreenExtent = {w, h};
            return true;
        }

        void present(void *drawData)
        {
            State &s = S();
            if (!s.frameOpen)
                return;
            s.frameOpen = false;
            const int f = s.frame;
            s.frame = (s.frame + 1) % kInFlight;
            int pw = 0, ph = 0;
            SDL_GetWindowSizeInPixels(s.window, &pw, &ph);
            const SDL_WindowFlags wf = SDL_GetWindowFlags(s.window);
            // A hidden or minimised window (PS2X_WINDOW_HIDDEN test runs): no present - with FIFO its acquire would wait
            // for the screen - the overlays drawn into an image of the window's size instead, for the shots.
            const bool unseen = (wf & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)) != 0 || pw <= 0 || ph <= 0;
            uint32_t imageIndex = 0u;
            VkImage target = VK_NULL_HANDLE;
            VkImageView targetView = VK_NULL_HANDLE;
            VkExtent2D extent{};
            bool acquired = false;
            if (!unseen)
            {
                if (s.swapchain && (s.stale || s.extent.width != static_cast<uint32_t>(pw) || s.extent.height != static_cast<uint32_t>(ph) ||
                                    s.syncAsked != ps2_host_present::sync()))
                    makeSwapchain(static_cast<uint32_t>(pw), static_cast<uint32_t>(ph));
                else if (!s.swapchain)
                    makeSwapchain(static_cast<uint32_t>(pw), static_cast<uint32_t>(ph));
                // (present_wait: the picture before the last on the screen first - at most one picture waiting in the queue)
                if (s.swapchain && s.dev.presentWait && s.presentId >= 2u)
                {
                    const VkResult w = vkWaitForPresentKHR(s.dev.device, s.swapchain, s.presentId - 1u, 100ull * 1000000ull);
                    if (w == VK_SUCCESS)
                    {
                        const int64_t t = nowNs();
                        s.shownNs.push_back(t);
                        while (s.shownNs.size() > 120u)
                            s.shownNs.pop_front();
                        // (the shortest gap between two pictures reaching the screen, over the last ones: the refresh,
                        // when the pictures come at least as often as the screen refreshes)
                        double best = 0.0;
                        for (size_t i = 1; i < s.shownNs.size(); ++i)
                        {
                            const double ms = static_cast<double>(s.shownNs[i] - s.shownNs[i - 1u]) / 1.0e6;
                            if (ms > 2.0 && (best == 0.0 || ms < best))
                                best = ms;
                        }
                        s.refreshMs.store(best, std::memory_order_relaxed);
                    }
                    else if (w == VK_ERROR_OUT_OF_DATE_KHR)
                        s.stale = true;
                }
                if (s.swapchain && !s.stale)
                {
                    const VkResult r = vkAcquireNextImageKHR(s.dev.device, s.swapchain, 100ull * 1000000ull, s.acquired[f], VK_NULL_HANDLE, &imageIndex);
                    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR)
                    {
                        acquired = true;
                        s.stale = r == VK_SUBOPTIMAL_KHR;
                        target = s.images[imageIndex];
                        targetView = s.views[imageIndex];
                        extent = s.extent;
                    }
                    else if (r == VK_ERROR_OUT_OF_DATE_KHR)
                        s.stale = true;
                    else if (r != VK_TIMEOUT && r != VK_NOT_READY)
                        ok(r, "vkAcquireNextImageKHR (window)");
                }
                if (!acquired)
                {
                    ++s.notPresented;
                    return;
                }
            }
            else
            {
                if (!s.keep || !ensureOffscreen(static_cast<uint32_t>(std::max(pw, 1)), static_cast<uint32_t>(std::max(ph, 1))))
                {
                    ++s.notPresented;
                    return;
                }
                target = s.offscreen;
                targetView = s.offscreenView;
                extent = s.offscreenExtent;
                ++s.notPresented; // (drawn for the shots, not shown)
            }
            VkCommandBuffer c = s.cmd[f];
            vkResetCommandBuffer(c, 0);
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(c, &bi);
            uploadTextures(c);
            barrier(c, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            att.imageView = targetView;
            att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            att.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = {{0, 0}, extent};
            ri.layerCount = 1u;
            ri.colorAttachmentCount = 1u;
            ri.pColorAttachments = &att;
            std::unique_lock<std::mutex> q(s.queueMutex); // (ImGui's Vulkan may submit its texture changes while drawing)
            vkCmdBeginRendering(c, &ri);
            ImGui_ImplVulkan_RenderDrawData(static_cast<ImDrawData *>(drawData), c);
            vkCmdEndRendering(c);
            const bool shot = s.keep && (s.transferSrc || unseen);
            if (shot)
            {
                const VkDeviceSize bytes = static_cast<VkDeviceSize>(extent.width) * extent.height * 4u;
                if (!s.shotBuf[f] || s.shotExtent[f].width != extent.width || s.shotExtent[f].height != extent.height)
                {
                    if (s.shotBuf[f])
                        vmaDestroyBuffer(s.dev.vma, s.shotBuf[f], s.shotAlloc[f]);
                    s.shotBuf[f] = VK_NULL_HANDLE;
                    makeHostBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, s.shotBuf[f], s.shotAlloc[f], s.shotMapped[f]);
                    s.shotExtent[f] = extent;
                }
                if (s.shotBuf[f])
                {
                    barrier(c, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
                    VkBufferImageCopy region{};
                    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
                    region.imageExtent = {extent.width, extent.height, 1u};
                    vkCmdCopyImageToBuffer(c, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.shotBuf[f], 1, &region);
                    VkMemoryBarrier toHost{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                    toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toHost, 0, nullptr, 0, nullptr);
                    if (acquired)
                        barrier(c, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
                    s.shotTaken[f] = true;
                    s.shotBgra[f] = s.format == VK_FORMAT_B8G8R8A8_UNORM;
                }
                else if (acquired)
                    barrier(c, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
            }
            else if (acquired)
                barrier(c, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
            vkEndCommandBuffer(c);
            // The submit: after the acquire (the swapchain image free), and - on the GPU - after the frame that copied
            // the window picture it draws (the timeline's value). The CPU never waits for the frame.
            VkSemaphore waits[2];
            VkPipelineStageFlags stages[2];
            uint64_t values[2] = {0u, 0u};
            uint32_t nWaits = 0u;
            if (acquired)
            {
                waits[nWaits] = s.acquired[f];
                stages[nWaits] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                values[nWaits++] = 0u;
            }
            if (s.drawn >= 0 && s.drawnValue && s.dev.timeline)
            {
                waits[nWaits] = s.dev.timeline;
                stages[nWaits] = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
                values[nWaits++] = s.drawnValue;
                std::lock_guard<std::mutex> lock(s.picMutex);
                s.pics[s.drawn].heldBy = f;
            }
            VkTimelineSemaphoreSubmitInfo ts{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
            ts.waitSemaphoreValueCount = nWaits;
            ts.pWaitSemaphoreValues = values;
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.pNext = &ts;
            si.waitSemaphoreCount = nWaits;
            si.pWaitSemaphores = waits;
            si.pWaitDstStageMask = stages;
            si.commandBufferCount = 1u;
            si.pCommandBuffers = &c;
            if (acquired)
            {
                si.signalSemaphoreCount = 1u;
                si.pSignalSemaphores = &s.renderDone[imageIndex];
            }
            if (!ok(vkQueueSubmit(s.dev.queue, 1, &si, s.fence[f]), "vkQueueSubmit (window)"))
                return;
            s.inFlight[f] = true;
            if (acquired)
            {
                VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
                pi.waitSemaphoreCount = 1u;
                pi.pWaitSemaphores = &s.renderDone[imageIndex];
                pi.swapchainCount = 1u;
                pi.pSwapchains = &s.swapchain;
                pi.pImageIndices = &imageIndex;
                VkPresentIdKHR pid{VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
                const uint64_t id = s.presentId + 1u;
                if (s.dev.presentId)
                {
                    pid.swapchainCount = 1u;
                    pid.pPresentIds = &id;
                    pi.pNext = &pid;
                }
                const VkResult r = vkQueuePresentKHR(s.dev.queue, &pi);
                if (s.dev.presentId)
                    s.presentId = id;
                if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
                    s.stale = true;
                else if (r != VK_SUCCESS)
                    ok(r, "vkQueuePresentKHR (window)");
                ++s.presented;
            }
        }

        uint64_t makeTexture(int width, int height, bool linear)
        {
            if (width <= 0 || height <= 0)
                return 0u;
            Tex *t = new Tex;
            t->w = width;
            t->h = height;
            t->linear = linear;
            t->pixels.assign(static_cast<size_t>(width) * height * 4u, 0u);
            S().textures.push_back(t);
            return reinterpret_cast<uint64_t>(t);
        }

        void updateTexture(uint64_t texture, const void *pixels, int pitch)
        {
            Tex *t = reinterpret_cast<Tex *>(texture);
            if (!t || !pixels)
                return;
            const size_t row = static_cast<size_t>(t->w) * 4u;
            for (int y = 0; y < t->h; ++y)
                std::memcpy(t->pixels.data() + y * row, static_cast<const uint8_t *>(pixels) + static_cast<size_t>(y) * pitch, row);
            t->dirty = true;
        }

        void setTextureFilter(uint64_t texture, bool linear)
        {
            Tex *t = reinterpret_cast<Tex *>(texture);
            if (t && t->linear != linear)
            {
                t->linear = linear;
                t->setStale = true;
            }
        }

        void dropTexture(uint64_t texture)
        {
            State &s = S();
            Tex *t = reinterpret_cast<Tex *>(texture);
            if (!t)
                return;
            s.textures.erase(std::remove(s.textures.begin(), s.textures.end(), t), s.textures.end());
            if (!s.imguiReady)
            {
                delete t;
                return;
            }
            s.texRetired.push_back(t); // (after the frames that may draw it)
        }

        // The texture's ImGui descriptor set, its image made on first use (the device exists by then: in a frame).
        uint64_t imguiTexture(uint64_t texture)
        {
            State &s = S();
            Tex *t = reinterpret_cast<Tex *>(texture);
            if (!t || !s.imguiReady)
                return 0u;
            if (!t->image)
            {
                if (!makeImage(static_cast<uint32_t>(t->w), static_cast<uint32_t>(t->h), VK_FORMAT_R8G8B8A8_UNORM,
                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, t->image, t->alloc) ||
                    !makeHostBuffer(t->pixels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false, t->staging, t->stagingAlloc, t->mapped))
                    return 0u;
                t->view = makeView(t->image, VK_FORMAT_R8G8B8A8_UNORM);
                t->dirty = t->fresh = t->setStale = true;
            }
            if (t->setStale)
            {
                if (t->set)
                    ImGui_ImplVulkan_RemoveTexture(t->set); // (not drawn since: the frames before are done)
                t->set = ImGui_ImplVulkan_AddTexture(t->linear ? s.linear : s.nearest, t->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                t->setStale = false;
            }
            return reinterpret_cast<uint64_t>(t->set);
        }

        bool readLastPicture(void *rgba, int width, int height)
        {
            State &s = S();
            if (!s.keep)
            {
                s.keep = true;
                std::fprintf(stderr, "[vulkan window] a shot of the window: each picture kept from now on (PS2X_SHOT_KEEP=1 from the start)\n");
            }
            if (s.last.empty() || s.lastW != width || s.lastH != height)
                return false;
            std::memcpy(rgba, s.last.data(), s.last.size());
            return true;
        }
    }

    // ---- the API ----------------------------------------------------------------------------------------------------
    bool beforeWindow()
    {
        if (!ps2_platform::sdl3())
            return false;
        ps2_platform::Presenter p;
        p.name = "Vulkan";
        p.windowFlags = SDL_WINDOW_VULKAN;
        p.open = open;
        p.close = close;
        p.newFrame = newFrame;
        p.present = present;
        p.makeTexture = makeTexture;
        p.updateTexture = updateTexture;
        p.setTextureFilter = setTextureFilter;
        p.dropTexture = dropTexture;
        p.imguiTexture = imguiTexture;
        p.readLastPicture = readLastPicture;
        ps2_platform::setPresenter(p);
        S().wanted = true;
        std::fprintf(stderr, "[vulkan window] Vulkan presents the game's window (SDL3's)\n");
        return true;
    }

    bool on() { return S().wanted; }
    SDL_Window *sdlWindow()
    {
        if (!S().wanted)
            return nullptr;
        // (the device may be made on its thread before the main thread has made the window: waited for, 15 s at most)
        const auto until = Clock::now() + std::chrono::seconds(15);
        void *w = ps2_platform::sdlWindow();
        while (!w && Clock::now() < until)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            w = ps2_platform::sdlWindow();
        }
        if (!w)
            std::fprintf(stderr, "[vulkan window] no window from the platform after 15 s\n");
        return static_cast<SDL_Window *>(w);
    }
    std::mutex &queueMutex() { return S().queueMutex; }

    void deviceReady(const Device &device)
    {
        State &s = S();
        {
            std::lock_guard<std::mutex> lock(s.devMutex);
            s.dev = device;
        }
        s.devReady.store(true, std::memory_order_release);
    }

    void deviceGone()
    {
        // The window is going (the game's end): no more pictures taken or presented. What the render thread may still
        // copy into - the window pictures - and the platform's textures stay until the process ends (the backend's
        // device does too); the window's own objects go before SDL's window does.
        State &s = S();
        {
            std::lock_guard<std::mutex> lock(s.picMutex);
            if (!s.devReady.load(std::memory_order_acquire))
                return;
            s.devReady.store(false, std::memory_order_release);
        }
        {
            std::lock_guard<std::mutex> q(s.queueMutex);
            vkQueueWaitIdle(s.dev.queue);
        }
        for (int f = 0; f < kInFlight; ++f)
            frameDone(f);
        if (s.imguiReady)
            ImGui_ImplVulkan_Shutdown(); // (its descriptor sets go with its pool: nothing is drawn any more)
        s.imguiReady = false;
        if (s.offscreen)
        {
            vkDestroyImageView(s.dev.device, s.offscreenView, nullptr);
            vmaDestroyImage(s.dev.vma, s.offscreen, s.offscreenAlloc);
            s.offscreen = VK_NULL_HANDLE;
        }
        for (int f = 0; f < kInFlight; ++f)
        {
            if (s.shotBuf[f])
                vmaDestroyBuffer(s.dev.vma, s.shotBuf[f], s.shotAlloc[f]);
            s.shotBuf[f] = VK_NULL_HANDLE;
            if (s.fence[f])
                vkDestroyFence(s.dev.device, s.fence[f], nullptr);
            if (s.acquired[f])
                vkDestroySemaphore(s.dev.device, s.acquired[f], nullptr);
            s.fence[f] = VK_NULL_HANDLE;
            s.acquired[f] = VK_NULL_HANDLE;
        }
        dropSwapchainParts();
        if (s.swapchain)
            vkDestroySwapchainKHR(s.dev.device, s.swapchain, nullptr);
        s.swapchain = VK_NULL_HANDLE;
        if (s.pool)
            vkDestroyCommandPool(s.dev.device, s.pool, nullptr);
        s.pool = VK_NULL_HANDLE;
        if (s.linear)
            vkDestroySampler(s.dev.device, s.linear, nullptr);
        if (s.nearest)
            vkDestroySampler(s.dev.device, s.nearest, nullptr);
        s.linear = s.nearest = VK_NULL_HANDLE;
    }

    bool takeFree(uint32_t width, uint32_t height, VkFormat format, Target &out)
    {
        State &s = S();
        if (!width || !height)
            return false;
        std::lock_guard<std::mutex> lock(s.picMutex);
        if (!s.devReady.load(std::memory_order_acquire))
            return false;
        int pick = -1;
        for (int i = 0; i < kPictures; ++i)
            if (i != s.newest && s.pics[i].heldBy < 0 && !s.pics[i].pending && (pick < 0 || s.pics[i].value < s.pics[pick].value))
                pick = i;
        if (pick < 0)
            return false;
        Picture &p = s.pics[pick];
        if (!p.image || p.w != width || p.h != height || p.format != format)
        {
            if (p.image)
                s.retired.push_back(Retired{p.image, p.alloc, p.view, p.set}); // (its set is the window's: let go there)
            p.image = VK_NULL_HANDLE;
            p.view = VK_NULL_HANDLE;
            p.set = VK_NULL_HANDLE;
            p.alloc = nullptr;
            if (!makeImage(width, height, format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, p.image, p.alloc))
            {
                p = Picture{};
                return false;
            }
            p.view = makeView(p.image, format);
            p.w = width;
            p.h = height;
            p.format = format;
            p.layout = VK_IMAGE_LAYOUT_UNDEFINED;
            ++p.generation;
        }
        out.index = pick;
        out.image = p.image;
        out.layout = p.layout;
        p.pending = true;
        p.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; // (where the frame's copy leaves it)
        return true;
    }

    uint64_t nextValue()
    {
        return ++S().nextValue;
    }

    void published(int index, uint64_t value, bool has3D, uint64_t tick, int64_t dueNs, int64_t readyNs)
    {
        State &s = S();
        {
            std::lock_guard<std::mutex> lock(s.picMutex);
            if (index < 0 || index >= kPictures)
                return;
            Picture &p = s.pics[index];
            p.pending = false;
            p.value = value;
            p.has3D = has3D;
            p.tick = tick;
            p.dueNs = dueNs;
            // (the pacer's: when the GPU was seen done with it; without the pacer, the submit - the window's own submit then
            // waits for it on the GPU)
            p.readyNs = readyNs ? readyNs : nowNs();
            s.newest = index;
            ++s.serial;
            s.lastNs = p.readyNs;
        }
        s.picCv.notify_all();
    }

    bool latest(unsigned int &texture, int &width, int &height, uint64_t &serial, bool &has3D, uint64_t &tick, int64_t &dueNs, int64_t &readyNs)
    {
        State &s = S();
        if (!s.imguiReady || !s.frameOpen)
            return false;
        int index = -1;
        Picture copy;
        {
            std::lock_guard<std::mutex> lock(s.picMutex);
            if (s.newest < 0)
                return false;
            index = s.newest;
            Picture &p = s.pics[index];
            if (!p.set || p.setGeneration != p.generation)
            {
                // (the window's thread: ImGui's descriptor sets are made here only; the old set's image is a retired one)
                p.set = ImGui_ImplVulkan_AddTexture(s.linear, p.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                p.setGeneration = p.generation;
            }
            p.heldBy = s.frame; // (the frame about to be presented draws it: never written until that frame is done)
            copy = p;
            serial = s.serial;
        }
        s.drawn = index;
        s.drawnValue = copy.value;
        const uint64_t set = reinterpret_cast<uint64_t>(copy.set);
        if (!s.pictureTexture)
            s.pictureTexture = ps2_platform::registerExternalTexture(set, static_cast<int>(copy.w), static_cast<int>(copy.h));
        else
            ps2_platform::updateExternalTexture(s.pictureTexture, set, static_cast<int>(copy.w), static_cast<int>(copy.h));
        texture = s.pictureTexture;
        width = static_cast<int>(copy.w);
        height = static_cast<int>(copy.h);
        has3D = copy.has3D;
        tick = copy.tick;
        dueNs = copy.dueNs;
        readyNs = copy.readyNs;
        return texture != 0u;
    }

    bool waitNewer(uint64_t serial, int64_t untilNs, int64_t &lastNs)
    {
        State &s = S();
        std::unique_lock<std::mutex> lock(s.picMutex);
        const auto until = Clock::time_point(std::chrono::nanoseconds(untilNs));
        s.picCv.wait_until(lock, until, [&] { return s.serial != serial; });
        lastNs = s.lastNs;
        return s.serial != serial;
    }

    void takeCounts(uint64_t &presented, uint64_t &notPresented)
    {
        presented = S().presented.exchange(0u);
        notPresented = S().notPresented.exchange(0u);
    }

    double refreshMs() { return S().refreshMs.load(std::memory_order_relaxed); }
}

#endif
