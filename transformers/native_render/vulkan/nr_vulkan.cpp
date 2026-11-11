// nr_vulkan.cpp - the native renderer's Vulkan backend (docs/RENDERER_BACKEND.md), with a window and swapchain of its
// own made with SDL3 (roadmap P2 / P3). TF_GFX_API=vulkan chooses it at start (nr_backend.cpp); Direct3D 12 stays the
// default. Built with TF_VULKAN (CMake option, off by default): the Vulkan headers, the volk loader, the Vulkan Memory
// Allocator, SDL3 and DXC's release are fetched at configure time like raylib, so no Vulkan SDK is needed to build it.
//
// Milestone 1: the device, the window and the swapchain; the frame's life (prepareFrame, beginFrame, endFrame, two
// slots in flight, the frame-done call); the targets (made zeroed, cleared, moved between uses, copied, read back - the
// shots and the debug saves); time stamps; and the picture shown in the window through the swapchain.
// Milestone 2: the draws - textures (uploaded on the frame's own upload list, uploadTextureLater at the next frame's
// start), meshes, the rings as host-visible buffers, the Direct3D 12 backend's HLSL as SPIR-V compiled at build time
// (nr_vulkan_shaders.h: DXC run by the build, the table built in), pipelines from PipelineDesc, the root signature as
// one descriptor set, dynamic rendering begun at the first draw after a target is set (and ended before any copy, clear
// or layout change outside it), snapshot, the pixel shader's variants (on a thread, as D3D12's psv::), and the effects
// FXAA, SMAA, the probe, the sparks, the bloom, the tone mapping and the local contrast.
// Left for milestone 4: the extra moment pictures (TF_POSES), predication, pipeline statistics, the GPU grass and the
// pipeline cache on disk.
// drawsPictures() stays false unless TF_VK_ALONE=1: the emulated GS keeps drawing the game in the main window and the
// Vulkan window beside it shows the Vulkan picture. TF_VK_ALONE=1 (a test switch) makes Vulkan the game's renderer: the
// GS stops drawing the scenes, and the main window takes the Vulkan picture as CPU pixels (with TF_NATIVE_SHARE=0).
//
// Switches: TF_VK_DEBUG=1 the validation layer (when installed) with its messages on stderr; TF_VK_GPU=<n> the n-th GPU
// Vulkan lists; TF_VK_PRESENT=fifo (default) / mailbox / immediate; TF_VK_WINDOW=0 no window (pictures only to shots);
// TF_VK_TEST=1 the picture cleared to a colour that cycles with the frame's VBlank before it is shown (a check that each
// picture reaches the window, evenly); TF_VK_ALONE=1 above. PS2X_WINDOW_HIDDEN=1 (test machines) keeps this window hidden.
#if defined(TF_VULKAN)

#include <volk.h> // (first: it includes the Vulkan headers itself, without prototypes)
#include <vk_mem_alloc.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "nr_backend.h"
#include "nr_device.h"
#include "nr_frame.h"
#include "nr_glshare.h" // (d3d::SharedPicture: the hand-over's metadata)
#include "nr_hlsl_sources.h" // (generated at build time from nr_d3d.cpp: extract_hlsl.cmake)
#if defined(__linux__)
#include "nr_picture_mailbox.h" // (Published: the TF_VK_ALONE CPU hand-over, tested by tests/vulkan_picture_tests.cpp)
#endif
#include "nr_types.h"
#include "nr_vulkan_shaders.h" // (the SPIR-V the build compiled: bake_spirv.cpp)

#include "ext/smaa/AreaTex.h"   // (SMAA's lookup tables, MIT: ext/smaa/LICENSE.txt - as the D3D12 backend uploads them)
#include "ext/smaa/SearchTex.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace nr::vulkan
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr uint32_t kSlots = 2u;          // frames in flight (docs section 3)
        constexpr uint32_t kMaxTextures = 8192u; // as the D3D12 device's descriptor slots
        constexpr uint32_t kInstanceEntries = 32768u;
        // The rings' size a slot: the draws' constants, the passes' instance entries, the vertices made this frame (D3D12's).
        constexpr uint64_t kRingBytes[3] = {32ull << 20, uint64_t(kInstanceEntries) * frame::kInstanceStride, 64ull << 20};
        constexpr uint32_t kPictureBytes = 256u;   // b1, the picture's constants (PictureConstants: 144 bytes), a slot's
        constexpr uint32_t kB0Range = 1024u;       // b0 as the shaders read it (the GS draws' cbuffer Draw: 944 bytes)
        constexpr uint64_t kStagingBytes = 32ull << 20; // a slot's texture uploads (larger ones get a buffer of their own)
        constexpr uint32_t kVertexBStride = 20u;   // setPreviousVertices: place, colour, size
        constexpr gfx::TextureId kViewBit = 0x80000000u, kDepthViewBit = 0x40000000u, kSnapBit = 0x20000000u, kIdMask = 0x0FFFFFFFu;
        // (SMAA's two lookup tables: bound in t1, t2 by setPipeline(SmaaWeights), as the D3D12 device binds them)
        constexpr gfx::TextureId kSmaaAreaId = 0x10000001u, kSmaaSearchId = 0x10000002u;

        // SMAA.hlsl's text as string pieces, as nr_d3d.cpp includes it (between kSmaaPrelude and kSmaaEntries).
        const char *const kSmaaSource[] = {
#include "ext/smaa/smaa_hlsl.inc"
        };

        bool envIs(const char *name, const char *value)
        {
            const char *v = std::getenv(name);
            return v && std::strcmp(v, value) == 0;
        }

        const char *resultName(VkResult r)
        {
            switch (r)
            {
            case VK_SUCCESS: return "VK_SUCCESS";
            case VK_NOT_READY: return "VK_NOT_READY";
            case VK_TIMEOUT: return "VK_TIMEOUT";
            case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
            case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
            case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
            case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
            case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
            case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
            case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
            case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
            case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
            case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED (no display for this session?)";
            case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
            case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
            case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
            case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
            default: return "VkResult";
            }
        }

        struct Image
        {
            VkImage image = VK_NULL_HANDLE;
            VmaAllocation alloc = nullptr;
            VkImageView view = VK_NULL_HANDLE;
            VkFormat format = VK_FORMAT_UNDEFINED;
            VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED; // (where the recorded work leaves it)
            VkImageAspectFlags aspect = 0;
            uint32_t w = 0, h = 0, levels = 1;
            VkDeviceSize bytes = 0;
        };

        struct Buffer
        {
            VkBuffer buffer = VK_NULL_HANDLE;
            VmaAllocation alloc = nullptr;
            uint8_t *mapped = nullptr;
            VkDeviceSize size = 0;
        };

        struct Target
        {
            bool live = false, picture = false, withDepth = false;
            gfx::Format format = gfx::Format::RGBA8;
            Image colour, depth; // (the picture's are State::picColour / picDepth)
            Image snap;          // snapshot(): a copy of the colour as it was (TEST's DATE reads it)
        };

        struct ReadbackBuffer
        {
            Buffer mem;
            uint32_t w = 0, h = 0, rowBytes = 0, slot = 0;
            gfx::Format format = gfx::Format::RGBA8;
            bool live = false;
        };

        struct TextureInfo
        {
            bool live = false, ready = false;
            uint32_t w = 0, h = 0, levels = 0, generation = 0;
            gfx::Format format = gfx::Format::RGBA8;
            std::vector<const void *> given; // (uploadTexture: the levels given so far, made once all are there)
            Image image;
        };

        struct MeshInfo
        {
            Buffer mem; // vertices, then the 32-bit indices
            uint32_t vertexBytes = 0;
            bool live = false;
        };

        // A shader module and, for a vertex shader, its inputs: the HLSL semantic and the location DXC gave it.
        struct Shader
        {
            VkShaderModule module = VK_NULL_HANDLE;
            std::string entry;
            std::vector<std::pair<std::string, uint32_t>> inputs;
        };

        // uploadTextureLater: made and uploaded at the next frame's start.
        struct Later
        {
            gfx::TextureId id = 0;
            uint32_t generation = 0;
            std::weak_ptr<const Texture> source;
        };

        // A vertex stream as set (setVertices, setMesh, setPreviousVertices) or the index buffer.
        struct Stream
        {
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceSize offset = 0, stride = 0;
        };

        // A bound sampled texture: its view, the layout the shader reads it in, and the target's image if it is one.
        struct Bound
        {
            VkImageView view = VK_NULL_HANDLE;
            VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
            Image *image = nullptr;
        };

        struct State
        {
            bool ready = false, failed = false;
            VkInstance instance = VK_NULL_HANDLE;
            VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
            VkPhysicalDevice phys = VK_NULL_HANDLE;
            VkPhysicalDeviceProperties props{};
            VkPhysicalDeviceMemoryProperties mem{};
            VkDevice device = VK_NULL_HANDLE;
            uint32_t family = ~0u;
            VkQueue queue = VK_NULL_HANDLE;
            VmaAllocator vma = nullptr;
            bool memoryBudget = false;
            uint32_t timestampBits = 0;
            // ---- what the device can do ----
            bool canDraw = false;    // Vulkan 1.3 (dynamic rendering), clip distances, the shaders compiled
            bool geometry = false;   // the clipping geometry shader
            bool anisotropy = false, bc7 = false;
            // ---- the window's swapchain ----
            VkSurfaceKHR surface = VK_NULL_HANDLE;
            VkSwapchainKHR swapchain = VK_NULL_HANDLE;
            VkFormat swapFormat = VK_FORMAT_UNDEFINED;
            VkExtent2D swapExtent{};
            VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
            std::vector<VkImage> swapImages;
            std::vector<VkSemaphore> presentReady; // (one per swapchain image: signalled by the frame, waited by its present)
            VkSemaphore acquired[kSlots] = {};
            bool swapStale = false;
            // ---- the frames ----
            VkCommandPool pool = VK_NULL_HANDLE;
            VkCommandBuffer cmd[kSlots] = {};
            VkCommandBuffer upCmd[kSlots] = {}; // (the frame's texture uploads: submitted before its own list)
            bool upBegun[kSlots] = {};
            VkFence fence[kSlots] = {};
            bool inFlight[kSlots] = {};
            uint64_t frameIndex = 0;
            uint32_t slot = 0;
            bool recording = false, readsThisFrame = false;
            std::function<void(uint32_t)> frameDone;
            // ---- targets ----
            std::vector<Target> targets = std::vector<Target>(1); // (id 0: none)
            std::vector<gfx::TargetId> freeTargets;
            Image picColour, picDepth;
            uint32_t picW = 0, picH = 0;
            bool picZero = false;
            std::vector<gfx::TargetId> zeroLater; // (made outside a recording: cleared at the next frame's start)
            std::vector<Image> retired[kSlots];   // (let go of: destroyed once that slot's frame has finished)
            std::vector<Buffer> retiredBuffers[kSlots];
            // ---- textures and meshes ----
            std::vector<TextureInfo> textures = std::vector<TextureInfo>(1); // (id 0: the white one, `white`)
            std::vector<gfx::TextureId> freeTextures;
            uint32_t texturesLive = 0;
            Image white;
            bool whiteUploaded = false;
            std::deque<Later> later;
            std::vector<MeshInfo> meshes = std::vector<MeshInfo>(1);
            std::vector<uint32_t> freeMeshes;
            uint64_t uploadsBytes = 0, uploads = 0;
            // ---- rings: one host-visible buffer - constants, instances, vertices (two slots each), the pictures' b1 ----
            Buffer ring;
            uint64_t ringBase[4] = {};
            uint64_t ringAt[3] = {};
            uint64_t ringPeak[3] = {};
            Buffer staging[kSlots];
            uint64_t stagingAt[kSlots] = {};
            // ---- bindings, shaders, pipelines ----
            VkSampler samplers[6] = {};
            VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
            VkPipelineLayout layout = VK_NULL_HANDLE;
            VkPipelineCache cache = VK_NULL_HANDLE;
            std::vector<VkDescriptorPool> pools[kSlots];
            uint32_t poolAt[kSlots] = {};
            Shader vs, ps, gs, aaVs, aaPs, probeVs, probePs, cloudVs, cloudPs, bloomVs, bloomDownPs, bloomUpPs, toneVs, tonePs, clarityVs, clarityPs;
            Shader smaaEdgeVs, smaaEdgePs, smaaWeightVs, smaaWeightPs, smaaBlendVs, smaaBlendPs;
            Image smaaArea, smaaSearch; // (SMAA's lookup tables, uploaded with the white texture)
            bool smaaReady = false;
            std::unordered_map<uint64_t, VkPipeline> pipelines; // (VK_NULL_HANDLE: tried and failed)
            uint64_t setsThisFrame = 0, drawsThisFrame = 0;
            uint64_t staleSlots = 0, movedForRead = 0; // (resolve: slots read as white, targets moved to be read - all frames)
            // ---- the recording's state (milestone 2) ----
            bool rendering = false;            // dynamic rendering begun (ended before copies, clears, layout changes)
            gfx::TargetId target = 0;          // setTarget's
            bool withDepth = false;
            gfx::PipelineDesc desc{};          // setPipeline's
            bool haveDesc = false;
            bool viewportEmpty = false;
            gfx::TextureId tex[3] = {};        // bindTextures'
            uint32_t b0 = 0;                   // bindDrawConstants': its offset in the ring
            Stream vb, vbB, ib;
            VkPipeline boundPipeline = VK_NULL_HANDLE;
            bool boundHasStreams = false, streamsDirty = true, indexDirty = true;
            VkDescriptorSet boundSet = VK_NULL_HANDLE;
            VkImageView boundViews[3] = {};
            VkImageLayout boundLayouts[3] = {};
            uint32_t boundB0 = ~0u;
            // ---- read-backs ----
            std::vector<ReadbackBuffer> readbacks = std::vector<ReadbackBuffer>(1);
            std::vector<gfx::ReadbackId> readbackFree[kSlots], readbackReleased[kSlots];
            gfx::ReadbackId publishRead[kSlots] = {}; // (TF_VK_ALONE: the picture for the window, read back)
            bool publishHas3D[kSlots] = {};
#if defined(__linux__)
            uint64_t publishTick[kSlots] = {};    // (the frame's VBlank and its picture's moment, for the hand-over)
            int64_t publishDueNs[kSlots] = {};
            uint64_t publishTurn[kSlots] = {};    // (the frame's id: the pacing log's turn)
#endif
            // ---- time stamps ----
            VkQueryPool stamps = VK_NULL_HANDLE;
            uint32_t stampsUsed[kSlots] = {};
            VkQueryPool picStamps = VK_NULL_HANDLE; // (the picture's own: its start and end, two a slot - the profile's whole frame)
            bool picTimed[kSlots] = {};
            // ---- figures ----
            uint64_t presented = 0, notPresented = 0;
            Clock::time_point since{};
        };
        State g;

        std::atomic<int> g_starting{0}; // 0 not begun, 1 on its thread, 2 done
        std::atomic<bool> g_lost{false};
        std::atomic<double> g_slotWaitAll{0.0};
        std::atomic<double> g_gpuPictureMs{0.0}; // (the picture's GPU time, smoothed as D3D12's)
        std::mutex g_startMutex;
        // (TF_VK_ALONE: read at its first use, not when the program starts - settings.ini is loaded into the environment
        // after the static objects are made, so the file's TF_VK_ALONE=1 was missed and only the environment worked)
        bool alone()
        {
            static const bool on = envIs("TF_VK_ALONE", "1");
            return on;
        }

#if defined(__linux__)
        Published g_pub;
#else
        // TF_VK_ALONE: the newest picture as CPU pixels for the main window (latestPicture / waitPicture).
        struct Published
        {
            std::mutex mutex;
            std::condition_variable cv;
            std::vector<uint32_t> pixels;
            uint32_t w = 0, h = 0;
            uint64_t serial = 0;
            bool has3D = false;
            int64_t lastNs = 0;
        };
        Published g_pub;
#endif

        // ---- the window (SDL3): made and pumped on a thread of its own ----------------------------------------------
        // (The main window is still the runtime's raylib window on the main thread; this one is the Vulkan backend's own.
        // SDL documents window creation and event pumping as main-thread calls; on Windows a window's messages belong to
        // the thread that made it, so this thread makes it and pumps it. Roadmap P2 moves the whole window to SDL3 on the
        // main thread.)
        struct Window
        {
            std::mutex mutex;
            std::condition_variable cv;
            int state = 0; // 1 made, -1 failed
            SDL_Window *window = nullptr;
            std::atomic<int> w{0}, h{0};
            std::atomic<bool> resized{false}, closed{false};
            // (hidden or minimised: its presents are left out - with FIFO the frame's submit waited on the acquire, and a
            // hidden window held the whole game back to its pace: PR 14's review measured 30 fps instead of 108 on a GTX 1080)
            std::atomic<bool> unseen{false};
        };
        Window g_win;

        bool openWindow()
        {
            std::thread([] {
                SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0"); // (the game's window keeps the keyboard)
                SDL_Window *window = nullptr;
                if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
                    std::fprintf(stderr, "[vulkan] SDL could not start its video: %s\n", SDL_GetError());
                else
                {
                    const bool hidden = envIs("PS2X_WINDOW_HIDDEN", "1");
                    const SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | (hidden ? SDL_WINDOW_HIDDEN : 0);
                    window = SDL_CreateWindow("Transformers - Vulkan (in development)", 1280, 720, flags);
                    if (!window)
                        std::fprintf(stderr, "[vulkan] SDL could not make the window: %s\n", SDL_GetError());
                }
                if (window)
                {
                    int pw = 0, ph = 0;
                    SDL_GetWindowSizeInPixels(window, &pw, &ph);
                    g_win.w.store(pw);
                    g_win.h.store(ph);
                    g_win.unseen.store((SDL_GetWindowFlags(window) & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)) != 0);
                }
                {
                    std::lock_guard<std::mutex> lock(g_win.mutex);
                    g_win.window = window;
                    g_win.state = window ? 1 : -1;
                }
                g_win.cv.notify_all();
                if (!window)
                    return;
                for (;;)
                {
                    SDL_Event e;
                    if (!SDL_WaitEventTimeout(&e, 250))
                        continue;
                    switch (e.type)
                    {
                    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                    case SDL_EVENT_WINDOW_RESIZED:
                    {
                        int pw = 0, ph = 0;
                        SDL_GetWindowSizeInPixels(window, &pw, &ph);
                        g_win.w.store(pw);
                        g_win.h.store(ph);
                        g_win.resized.store(true);
                        break;
                    }
                    case SDL_EVENT_WINDOW_MINIMIZED:
                        g_win.unseen.store(true);
                        g_win.resized.store(true);
                        break;
                    case SDL_EVENT_WINDOW_RESTORED:
                    case SDL_EVENT_WINDOW_MAXIMIZED:
                        g_win.unseen.store((SDL_GetWindowFlags(window) & SDL_WINDOW_HIDDEN) != 0);
                        g_win.resized.store(true);
                        break;
                    case SDL_EVENT_WINDOW_HIDDEN:
                        g_win.unseen.store(true);
                        break;
                    case SDL_EVENT_WINDOW_SHOWN:
                        g_win.unseen.store((SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) != 0);
                        break;
                    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                        // (Closing it does not end the game - the game's own window does that: this one hides and its
                        // pictures stop.)
                        g_win.closed.store(true);
                        SDL_HideWindow(window);
                        std::fprintf(stderr, "[vulkan] the Vulkan window closed: its pictures stop (the game goes on)\n");
                        break;
                    default:
                        break;
                    }
                }
            }).detach();
            std::unique_lock<std::mutex> lock(g_win.mutex);
            g_win.cv.wait_for(lock, std::chrono::seconds(15), [] { return g_win.state != 0; });
            return g_win.state == 1;
        }

        // ---- helpers -------------------------------------------------------------------------------------------------
        bool ok(VkResult r, const char *what)
        {
            if (r == VK_SUCCESS)
                return true;
            if (r == VK_ERROR_DEVICE_LOST && !g_lost.exchange(true))
                std::fprintf(stderr, "[vulkan] the device was lost (%s): nothing more is drawn with Vulkan\n", what);
            else if (r != VK_ERROR_DEVICE_LOST)
            {
                static int told = 0;
                if (told++ < 40)
                    std::fprintf(stderr, "[vulkan] %s failed: %s (%d)\n", what, resultName(r), static_cast<int>(r));
            }
            return false;
        }

        VkFormat vkFormat(gfx::Format f)
        {
            switch (f)
            {
            case gfx::Format::RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
            case gfx::Format::BGRA8: return VK_FORMAT_B8G8R8A8_UNORM;
            case gfx::Format::BC7: return VK_FORMAT_BC7_UNORM_BLOCK;
            case gfx::Format::R32Float: return VK_FORMAT_R32_SFLOAT;
            case gfx::Format::Depth32Float: return VK_FORMAT_D32_SFLOAT;
            case gfx::Format::RG8: return VK_FORMAT_R8G8_UNORM;
            case gfx::Format::R8: return VK_FORMAT_R8_UNORM;
            }
            return VK_FORMAT_R8G8B8A8_UNORM;
        }

        uint32_t texelBytes(gfx::Format f)
        {
            switch (f)
            {
            case gfx::Format::RG8: return 2u;
            case gfx::Format::R8: return 1u;
            default: return 4u;
            }
        }

        VkAccessFlags accessOf(VkImageLayout l)
        {
            switch (l)
            {
            case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
                return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL: return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
            case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return VK_ACCESS_SHADER_READ_BIT;
            case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return VK_ACCESS_TRANSFER_READ_BIT;
            case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return VK_ACCESS_TRANSFER_WRITE_BIT;
            case VK_IMAGE_LAYOUT_UNDEFINED:
            case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR: return 0;
            default: return VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            }
        }

        // The stages that use an image in a layout (the shaders read textures in the pixel stage only).
        VkPipelineStageFlags stagesOf(VkImageLayout l, bool before)
        {
            switch (l)
            {
            case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
                return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return VK_PIPELINE_STAGE_TRANSFER_BIT;
            case VK_IMAGE_LAYOUT_UNDEFINED: return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT; // (new images, and the swapchain's: after the acquire's wait)
            case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR: return before ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
            default: return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            }
        }

        // A layout transition: the stages that used the image in its old layout before those of its new one
        // (TF_VK_WIDEBARRIERS=1: every stage on both sides, as milestone 1 had them - a checking aid).
        void transition(VkCommandBuffer c, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to)
        {
            static const bool wide = envIs("TF_VK_WIDEBARRIERS", "1");
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.srcAccessMask = accessOf(from);
            b.dstAccessMask = accessOf(to);
            b.oldLayout = from;
            b.newLayout = to;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image;
            b.subresourceRange = {aspect, 0u, VK_REMAINING_MIP_LEVELS, 0u, VK_REMAINING_ARRAY_LAYERS};
            vkCmdPipelineBarrier(c, wide ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : stagesOf(from, true), wide ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : stagesOf(to, false),
                                 0, 0, nullptr, 0, nullptr, 1, &b);
            ++frame::g_cntBarriers;
        }

        // Dynamic rendering ended: before anything that may not happen inside it (a barrier, a copy, a transfer clear).
        void endRendering()
        {
            if (g.rendering)
            {
                vkCmdEndRendering(g.cmd[g.slot]);
                g.rendering = false;
            }
        }

        void moveTo(Image &im, VkImageLayout to)
        {
            if (!im.image || im.layout == to || !g.recording)
                return;
            endRendering();
            transition(g.cmd[g.slot], im.image, im.aspect, im.layout, to);
            im.layout = to;
        }

        bool makeImage(Image &im, uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, uint32_t levels = 1u)
        {
            im = Image{};
            VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ci.imageType = VK_IMAGE_TYPE_2D;
            ci.format = format;
            ci.extent = {std::max(1u, w), std::max(1u, h), 1u};
            ci.mipLevels = std::max(1u, levels);
            ci.arrayLayers = 1u;
            ci.samples = VK_SAMPLE_COUNT_1_BIT;
            ci.tiling = VK_IMAGE_TILING_OPTIMAL;
            ci.usage = usage;
            ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VmaAllocationCreateInfo ac{};
            ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            VmaAllocationInfo info{};
            if (!ok(vmaCreateImage(g.vma, &ci, &ac, &im.image, &im.alloc, &info), "vmaCreateImage"))
            {
                im = Image{};
                return false;
            }
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = im.image;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = format;
            vi.subresourceRange = {aspect, 0u, ci.mipLevels, 0u, 1u};
            if (!ok(vkCreateImageView(g.device, &vi, nullptr, &im.view), "vkCreateImageView"))
                im.view = VK_NULL_HANDLE;
            im.format = format;
            im.aspect = aspect;
            im.w = ci.extent.width;
            im.h = ci.extent.height;
            im.levels = ci.mipLevels;
            im.bytes = info.size;
            return true;
        }

        void destroyImage(Image &im)
        {
            if (im.view)
                vkDestroyImageView(g.device, im.view, nullptr);
            if (im.image)
                vmaDestroyImage(g.vma, im.image, im.alloc);
            im = Image{};
        }

        // A buffer the CPU writes in order (rings, meshes, uploads) or reads (read-backs), mapped while it lives; memory the
        // CPU and GPU see alike (coherent), as Direct3D 12's upload and read-back heaps.
        bool makeBuffer(Buffer &b, VkDeviceSize size, VkBufferUsageFlags usage, bool cpuReads)
        {
            b = Buffer{};
            VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bi.size = std::max<VkDeviceSize>(size, 16u);
            bi.usage = usage;
            bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VmaAllocationCreateInfo ac{};
            ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
            ac.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                       (cpuReads ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
            ac.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            VmaAllocationInfo info{};
            if (!ok(vmaCreateBuffer(g.vma, &bi, &ac, &b.buffer, &b.alloc, &info), "vmaCreateBuffer"))
            {
                b = Buffer{};
                return false;
            }
            b.mapped = static_cast<uint8_t *>(info.pMappedData);
            b.size = bi.size;
            return b.mapped != nullptr;
        }

        void destroyBuffer(Buffer &b)
        {
            if (b.buffer)
                vmaDestroyBuffer(g.vma, b.buffer, b.alloc);
            b = Buffer{};
        }

        // Let go of once the frames that may use it are done: the slot's (its frame before is the other slot's, which a
        // fence of this slot's frame also covers - submission order).
        void retire(Image &im, uint32_t slot)
        {
            if (im.image)
                g.retired[slot].push_back(im);
            im = Image{};
        }
        void retire(Buffer &b, uint32_t slot)
        {
            if (b.buffer)
                g.retiredBuffers[slot].push_back(b);
            b = Buffer{};
        }

        bool makeColour(Image &im, uint32_t w, uint32_t h, gfx::Format f)
        {
            return makeImage(im, w, h, vkFormat(f),
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT);
        }

        bool makeDepth(Image &im, uint32_t w, uint32_t h)
        {
            return makeImage(im, w, h, VK_FORMAT_D32_SFLOAT,
                             VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             VK_IMAGE_ASPECT_DEPTH_BIT);
        }

        // A new or remade surface cleared to zero, as Direct3D 12's committed resources come (docs 4.2): a read before the
        // first draw gives the same pixels.
        void zero(Image &im)
        {
            if (!im.image || !g.recording)
                return;
            endRendering();
            moveTo(im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            const VkImageSubresourceRange range{im.aspect, 0u, 1u, 0u, 1u};
            if (im.aspect & VK_IMAGE_ASPECT_DEPTH_BIT)
            {
                const VkClearDepthStencilValue z{0.0f, 0u};
                vkCmdClearDepthStencilImage(g.cmd[g.slot], im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &range);
            }
            else
            {
                const VkClearColorValue z{};
                vkCmdClearColorImage(g.cmd[g.slot], im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &range);
            }
        }

        Target *targetOf(gfx::TargetId id)
        {
            return id && id < g.targets.size() && g.targets[id].live ? &g.targets[id] : nullptr;
        }
        Image &colourOf(Target &t) { return t.picture ? g.picColour : t.colour; }
        Image &depthOf(Target &t) { return t.picture ? g.picDepth : t.depth; }
        gfx::Format formatOf(const Target &t) { return t.picture ? gfx::Format::RGBA8 : t.format; }

        // ---- the swapchain ---------------------------------------------------------------------------------------------
        void dropSwapchainParts()
        {
            for (VkSemaphore s : g.presentReady)
                vkDestroySemaphore(g.device, s, nullptr);
            g.presentReady.clear();
            g.swapImages.clear();
        }

        bool makeSwapchain()
        {
            if (!g.surface)
                return false;
            VkSurfaceCapabilitiesKHR caps{};
            if (!ok(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.phys, g.surface, &caps), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"))
                return false;
            VkExtent2D extent = caps.currentExtent;
            if (extent.width == 0xFFFFFFFFu)
                extent = {static_cast<uint32_t>(std::max(0, g_win.w.load())), static_cast<uint32_t>(std::max(0, g_win.h.load()))};
            extent.width = std::clamp(extent.width, caps.minImageExtent.width, std::max(caps.minImageExtent.width, caps.maxImageExtent.width));
            extent.height = std::clamp(extent.height, caps.minImageExtent.height, std::max(caps.minImageExtent.height, caps.maxImageExtent.height));
            if (extent.width == 0u || extent.height == 0u) // (minimised: none until it has a size again)
                return false;
            uint32_t n = 0;
            vkGetPhysicalDeviceSurfaceFormatsKHR(g.phys, g.surface, &n, nullptr);
            std::vector<VkSurfaceFormatKHR> formats(n);
            vkGetPhysicalDeviceSurfaceFormatsKHR(g.phys, g.surface, &n, formats.data());
            if (formats.empty())
                return false;
            // (UNORM, not sRGB: the picture is shown as its bytes, as the GL window shows the D3D12 one.)
            VkSurfaceFormatKHR chosen = formats[0];
            for (VkFormat want : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM})
            {
                const auto it = std::find_if(formats.begin(), formats.end(), [want](const VkSurfaceFormatKHR &f) {
                    return f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
                });
                if (it != formats.end())
                {
                    chosen = *it;
                    break;
                }
            }
            vkGetPhysicalDeviceSurfacePresentModesKHR(g.phys, g.surface, &n, nullptr);
            std::vector<VkPresentModeKHR> modes(n);
            vkGetPhysicalDeviceSurfacePresentModesKHR(g.phys, g.surface, &n, modes.data());
            VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR; // (always there)
            const char *want = std::getenv("TF_VK_PRESENT");
            const VkPresentModeKHR asked = !want                              ? VK_PRESENT_MODE_FIFO_KHR
                                           : std::strcmp(want, "mailbox") == 0   ? VK_PRESENT_MODE_MAILBOX_KHR
                                           : std::strcmp(want, "immediate") == 0 ? VK_PRESENT_MODE_IMMEDIATE_KHR
                                                                                 : VK_PRESENT_MODE_FIFO_KHR;
            if (std::find(modes.begin(), modes.end(), asked) != modes.end())
                mode = asked;
            else if (want)
                std::fprintf(stderr, "[vulkan] present mode %s not offered here: FIFO\n", want);
            uint32_t images = std::max(caps.minImageCount + 1u, 3u);
            if (caps.maxImageCount)
                images = std::min(images, caps.maxImageCount);
            VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
            ci.surface = g.surface;
            ci.minImageCount = images;
            ci.imageFormat = chosen.format;
            ci.imageColorSpace = chosen.colorSpace;
            ci.imageExtent = extent;
            ci.imageArrayLayers = 1u;
            ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | (caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
            ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            ci.preTransform = caps.currentTransform;
            ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                                    ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                    : static_cast<VkCompositeAlphaFlagBitsKHR>(caps.supportedCompositeAlpha & (~caps.supportedCompositeAlpha + 1u));
            ci.presentMode = mode;
            ci.clipped = VK_TRUE;
            ci.oldSwapchain = g.swapchain;
            if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
            {
                std::fprintf(stderr, "[vulkan] the window's surface cannot be copied into (no TRANSFER_DST): no pictures shown\n");
                return false;
            }
            VkSwapchainKHR made = VK_NULL_HANDLE;
            const VkResult r = vkCreateSwapchainKHR(g.device, &ci, nullptr, &made);
            if (g.swapchain)
            {
                dropSwapchainParts();
                vkDestroySwapchainKHR(g.device, g.swapchain, nullptr);
                g.swapchain = VK_NULL_HANDLE;
            }
            if (!ok(r, "vkCreateSwapchainKHR"))
                return false;
            g.swapchain = made;
            vkGetSwapchainImagesKHR(g.device, made, &n, nullptr);
            g.swapImages.resize(n);
            vkGetSwapchainImagesKHR(g.device, made, &n, g.swapImages.data());
            VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            g.presentReady.assign(n, VK_NULL_HANDLE);
            for (VkSemaphore &s : g.presentReady)
                if (!ok(vkCreateSemaphore(g.device, &si, nullptr, &s), "vkCreateSemaphore (present)"))
                {
                    // (no swapchain then: the pictures are not shown, the next frames try again)
                    dropSwapchainParts();
                    vkDestroySwapchainKHR(g.device, g.swapchain, nullptr);
                    g.swapchain = VK_NULL_HANDLE;
                    return false;
                }
            g.swapFormat = chosen.format;
            g.swapExtent = extent;
            g.presentMode = mode;
            g.swapStale = false;
            static const char *kModes[] = {"immediate", "mailbox", "FIFO", "FIFO relaxed"};
            std::fprintf(stderr, "[vulkan] swapchain %ux%u, %u images, %s, present mode %s\n", extent.width, extent.height, n,
                         chosen.format == VK_FORMAT_B8G8R8A8_UNORM ? "BGRA8" : chosen.format == VK_FORMAT_R8G8B8A8_UNORM ? "RGBA8" : "other format",
                         static_cast<uint32_t>(mode) < 4u ? kModes[mode] : "other");
            return true;
        }

        void waitIdle()
        {
            if (g.device)
                vkDeviceWaitIdle(g.device);
        }

        // ---- the quality preset (the D3D12 backend's, from Vulkan's video memory) --------------------------------------
        uint64_t memoryMB()
        {
            if (const char *e = std::getenv("TF_NATIVE_VRAM_MB"); e && *e)
                return std::strtoull(e, nullptr, 10);
            if (!g.phys)
                return 0u;
            uint64_t largest = 0;
            for (uint32_t i = 0; i < g.mem.memoryHeapCount; ++i)
                if (g.mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                    largest = std::max<uint64_t>(largest, g.mem.memoryHeaps[i].size);
            return largest >> 20;
        }

        int presetFor(uint64_t mb)
        {
            const char *e = std::getenv("TF_NATIVE_QUALITY");
            const std::string v = e ? e : "";
            const int level = v == "low" ? 0 : v == "medium" ? 1 : v == "high" ? 2 : v == "ultra" ? 3 : -1;
            if (level >= 0)
                return level;
            return mb == 0u ? 2 : mb < 3000u ? 0 : mb < 5000u ? 1 : mb < 10000u ? 2 : 3;
        }

        // ---- shaders: the D3D12 backend's HLSL as SPIR-V ---------------------------------------------------------------
        // A vertex shader's inputs from its SPIR-V: DXC names each "in.var.<SEMANTIC>" and decorates it with its location.
        std::vector<std::pair<std::string, uint32_t>> inputsOf(const uint32_t *w, size_t count)
        {
            std::unordered_map<uint32_t, std::string> names;
            std::unordered_map<uint32_t, uint32_t> locations;
            std::vector<uint32_t> inputs;
            for (size_t i = 5; i < count;)
            {
                const uint32_t op = w[i] & 0xFFFFu, n = w[i] >> 16;
                if (!n || i + n > count)
                    break;
                if (op == 5u && n >= 3u) // OpName
                {
                    const char *s = reinterpret_cast<const char *>(&w[i + 2]);
                    size_t len = 0;
                    while (len < (n - 2u) * 4u && s[len])
                        ++len;
                    names[w[i + 1]] = std::string(s, len);
                }
                else if (op == 71u && n >= 4u && w[i + 2] == 30u) // OpDecorate Location
                    locations[w[i + 1]] = w[i + 3];
                else if (op == 59u && n >= 4u && w[i + 3] == 1u) // OpVariable Input
                    inputs.push_back(w[i + 2]);
                i += n;
            }
            std::vector<std::pair<std::string, uint32_t>> out;
            for (const uint32_t id : inputs)
            {
                const auto l = locations.find(id);
                const auto nm = names.find(id);
                if (l != locations.end() && nm != names.end() && nm->second.rfind("in.var.", 0) == 0)
                    out.emplace_back(nm->second.substr(7), l->second);
            }
            return out;
        }

        // A shader's SPIR-V from the table the build made (nr_vulkan_shaders.h), as a module. Any thread.
        bool makeShader(Shader &s, const char *source, size_t size, const char *entry, const char *profile,
                        const shaders::Defines &defines = {})
        {
            const shaders::Baked *b = shaders::find(shaders::keyOf(source, size, entry, profile, defines));
            if (!b)
            {
                std::fprintf(stderr, "[vulkan] shader %s (%s, %zu defines) is not in the SPIR-V the build made (bake_spirv.cpp's list)\n",
                             entry, profile, defines.size());
                return false;
            }
            VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            ci.codeSize = size_t(b->count) * 4u;
            ci.pCode = b->words;
            if (!ok(vkCreateShaderModule(g.device, &ci, nullptr, &s.module), "vkCreateShaderModule"))
                return false;
            s.entry = entry;
            if (profile[0] == 'v')
                s.inputs = inputsOf(b->words, b->count);
            return true;
        }

        // ---- the pipeline cache on disk (D3D12's pipeline library's counterpart): the same folder as D3D12's shader
        // cache - TF_SHADER_CACHE=<folder> (0: none), else "shader cache" beside the settings file or the working folder -
        // its own file; taken only when its header names this GPU and driver; saved a few seconds after the last pipeline
        // was made ----
        std::atomic<uint32_t> g_pipelinesMade{0};
        uint32_t g_pipelinesSaved = 0;
        Clock::time_point g_pipelinesChangedAt{};

        std::filesystem::path pipelineCacheFile()
        {
            std::error_code ec;
            const char *e = std::getenv("TF_SHADER_CACHE");
            std::filesystem::path dir;
            if (e && e[0] == '0' && !e[1])
                return {};
            if (e && *e)
                dir = std::filesystem::path(e);
            else
            {
                const char *settings = std::getenv("PS2X_SETTINGS_FILE");
                const std::filesystem::path base = settings && *settings && std::string(settings) != "0"
                                                       ? std::filesystem::absolute(settings, ec).parent_path()
                                                       : std::filesystem::current_path(ec);
                dir = base / "shader cache";
            }
            std::filesystem::create_directories(dir, ec);
            return ec ? std::filesystem::path{} : dir / "vulkan_pipelines.bin";
        }

        void savePipelineCache()
        {
            const uint32_t made = g_pipelinesMade.load(std::memory_order_relaxed);
            if (!g.cache || made == g_pipelinesSaved)
                return;
            // (saved once no pipeline has been made for 5 s: not in the middle of a burst of new ones)
            static uint32_t seen = 0;
            if (made != seen)
            {
                seen = made;
                g_pipelinesChangedAt = Clock::now();
                return;
            }
            if (Clock::now() - g_pipelinesChangedAt < std::chrono::seconds(5))
                return;
            const std::filesystem::path file = pipelineCacheFile();
            size_t size = 0;
            if (file.empty() || vkGetPipelineCacheData(g.device, g.cache, &size, nullptr) != VK_SUCCESS || !size)
                return;
            std::vector<uint8_t> data(size);
            if (vkGetPipelineCacheData(g.device, g.cache, &size, data.data()) != VK_SUCCESS)
                return;
            std::filesystem::path tmp = file;
            tmp += ".tmp";
            if (FILE *f = std::fopen(tmp.string().c_str(), "wb"))
            {
                const bool written = std::fwrite(data.data(), 1, size, f) == size;
                std::fclose(f);
                std::error_code ec;
                if (written)
                    std::filesystem::rename(tmp, file, ec);
                if (written && !ec)
                    std::fprintf(stderr, "[vulkan] pipeline cache: %u pipelines made, %zu KB saved (%s)\n", made, size / 1024u, file.string().c_str());
            }
            g_pipelinesSaved = made;
        }

        // ---- the bindings: Direct3D 12's root signature as one descriptor set (VULKAN_PLAN.md, the binding layout) ------
        bool makeBindings(int preset)
        {
            // s0: anisotropic, wrap (4, 8, 16, 16 by the quality preset, TF_NATIVE_ANISO); s1 trilinear; s2 point with linear
            // mips (the GS's nearest with TEX1's level); s3 anisotropic at TF_NATIVE_ANISO_ALT (16); s4, s5 SMAA's - linear and
            // point, clamped, no mips. (nr_d3d.cpp's static samplers, the same filters.)
            static const uint32_t kPresetAniso[4] = {4u, 8u, 16u, 16u};
            const char *aniso = std::getenv("TF_NATIVE_ANISO"), *alt = std::getenv("TF_NATIVE_ANISO_ALT");
            const float maxAniso = g.props.limits.maxSamplerAnisotropy;
            const float a0 = static_cast<float>(aniso ? std::clamp(std::atoi(aniso), 1, 16) : static_cast<int>(kPresetAniso[std::clamp(preset, 0, 3)]));
            const float a3 = static_cast<float>(alt ? std::clamp(std::atoi(alt), 1, 16) : 16);
            for (int k = 0; k < 6; ++k)
            {
                VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
                const bool smaa = k >= 4;
                si.magFilter = si.minFilter = (k == 2 || k == 5) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
                si.mipmapMode = smaa ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
                si.addressModeU = si.addressModeV = si.addressModeW = smaa ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
                si.minLod = 0.0f;
                si.maxLod = smaa ? 0.0f : VK_LOD_CLAMP_NONE;
                if ((k == 0 || k == 3) && g.anisotropy)
                {
                    si.anisotropyEnable = VK_TRUE;
                    si.maxAnisotropy = std::min(k == 0 ? a0 : a3, maxAniso);
                }
                si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
                if (!ok(vkCreateSampler(g.device, &si, nullptr, &g.samplers[k]), "vkCreateSampler"))
                    return false;
            }
            VkDescriptorSetLayoutBinding b[11]{};
            const VkShaderStageFlags all = VK_SHADER_STAGE_ALL_GRAPHICS;
            b[0] = {0u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1u, all, nullptr}; // b0, the draw's constants (the ring)
            b[1] = {1u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1u, all, nullptr}; // b1, the picture's constants
            for (uint32_t k = 0; k < 3; ++k)
                b[2 + k] = {10u + k, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1u, all, nullptr}; // t0 - t2
            for (uint32_t k = 0; k < 6; ++k)
                b[5 + k] = {20u + k, VK_DESCRIPTOR_TYPE_SAMPLER, 1u, all, &g.samplers[k]}; // s0 - s5, immutable
            VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            li.bindingCount = 11u;
            li.pBindings = b;
            if (!ok(vkCreateDescriptorSetLayout(g.device, &li, nullptr, &g.setLayout), "vkCreateDescriptorSetLayout"))
                return false;
            VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            pi.setLayoutCount = 1u;
            pi.pSetLayouts = &g.setLayout;
            if (!ok(vkCreatePipelineLayout(g.device, &pi, nullptr, &g.layout), "vkCreatePipelineLayout"))
                return false;
            // The pipeline cache, from the file a run before saved when it is this GPU's and driver's (the header's
            // vendor, device and cache UUID), else empty.
            VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
            std::vector<uint8_t> data;
            const std::filesystem::path file = pipelineCacheFile();
            if (!file.empty())
                if (FILE *f = std::fopen(file.string().c_str(), "rb"))
                {
                    std::fseek(f, 0, SEEK_END);
                    const long n = std::ftell(f);
                    std::fseek(f, 0, SEEK_SET);
                    if (n > 32 && n < (256l << 20))
                    {
                        data.resize(static_cast<size_t>(n));
                        if (std::fread(data.data(), 1, data.size(), f) != data.size())
                            data.clear();
                    }
                    std::fclose(f);
                }
            VkPipelineCacheHeaderVersionOne header{};
            if (data.size() >= sizeof(header))
                std::memcpy(&header, data.data(), sizeof(header));
            const bool fits = data.size() >= sizeof(header) && header.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
                              header.vendorID == g.props.vendorID && header.deviceID == g.props.deviceID &&
                              std::memcmp(header.pipelineCacheUUID, g.props.pipelineCacheUUID, VK_UUID_SIZE) == 0;
            if (fits)
            {
                ci.initialDataSize = data.size();
                ci.pInitialData = data.data();
            }
            if (!ok(vkCreatePipelineCache(g.device, &ci, nullptr, &g.cache), "vkCreatePipelineCache") && fits)
            {
                ci.initialDataSize = 0;
                ci.pInitialData = nullptr;
                vkCreatePipelineCache(g.device, &ci, nullptr, &g.cache);
            }
            std::fprintf(stderr, "[vulkan] pipeline cache: %s\n", fits ? "the one on disk taken" : data.empty() ? "none on disk yet" : "the one on disk is another GPU's or driver's - made anew");
            return true;
        }

        // A descriptor set from the slot's pools (reset when the slot is recorded again; another pool when one is full).
        VkDescriptorSet allocSet()
        {
            const uint32_t s = g.slot;
            std::vector<VkDescriptorPool> &pools = g.pools[s];
            for (;;)
            {
                if (g.poolAt[s] == pools.size())
                {
                    constexpr uint32_t kSets = 4096u;
                    const VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2u * kSets},
                                                           {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3u * kSets},
                                                           {VK_DESCRIPTOR_TYPE_SAMPLER, 6u * kSets}};
                    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
                    pi.maxSets = kSets;
                    pi.poolSizeCount = 3u;
                    pi.pPoolSizes = sizes;
                    VkDescriptorPool pool = VK_NULL_HANDLE;
                    if (!ok(vkCreateDescriptorPool(g.device, &pi, nullptr, &pool), "vkCreateDescriptorPool"))
                        return VK_NULL_HANDLE;
                    pools.push_back(pool);
                }
                VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                ai.descriptorPool = pools[g.poolAt[s]];
                ai.descriptorSetCount = 1u;
                ai.pSetLayouts = &g.setLayout;
                VkDescriptorSet set = VK_NULL_HANDLE;
                const VkResult r = vkAllocateDescriptorSets(g.device, &ai, &set);
                if (r == VK_SUCCESS)
                    return set;
                if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL)
                {
                    ok(r, "vkAllocateDescriptorSets");
                    return VK_NULL_HANDLE;
                }
                ++g.poolAt[s];
            }
        }

        // ---- pipelines ---------------------------------------------------------------------------------------------------
        // The vertex streams (nr_d3d.cpp's input layout): stream 0 nr::Vertex, 1 the Instances ring's entries, 2 the frame
        // before's vertices.
        struct Attribute
        {
            const char *semantic;
            uint32_t binding;
            VkFormat format;
            uint32_t offset;
        };
        const Attribute kAttributes[] = {
            {"POSITION", 0u, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, x)},
            {"COLOR", 0u, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Vertex, colour)},
            {"TEXCOORD", 0u, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, u)},
            {"NORMAL", 0u, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, nx)},
            {"PSIZE", 0u, VK_FORMAT_R32_SFLOAT, offsetof(Vertex, size)},
            {"IROW0", 1u, VK_FORMAT_R32G32B32A32_SFLOAT, 0u},
            {"IROW1", 1u, VK_FORMAT_R32G32B32A32_SFLOAT, 16u},
            {"IROW2", 1u, VK_FORMAT_R32G32B32A32_SFLOAT, 32u},
            {"IROW3", 1u, VK_FORMAT_R32G32B32A32_SFLOAT, 48u},
            {"IPZERO", 1u, VK_FORMAT_R32G32B32A32_SFLOAT, 64u},
            {"IROWB0", 1u, VK_FORMAT_R32G32B32A32_SFLOAT, 80u},
            {"IROWB1", 1u, VK_FORMAT_R32G32B32A32_SFLOAT, 96u},
            {"IROWB2", 1u, VK_FORMAT_R32G32B32A32_SFLOAT, 112u},
            {"IROWB3", 1u, VK_FORMAT_R32G32B32A32_SFLOAT, 128u},
            {"IROWM0", 1u, VK_FORMAT_R32G32B32A32_SFLOAT, 144u},
            {"IROWM1", 1u, VK_FORMAT_R32G32B32A32_SFLOAT, 160u},
            {"IROWM2", 1u, VK_FORMAT_R32G32B32A32_SFLOAT, 176u},
            {"IROWM3", 1u, VK_FORMAT_R32G32B32A32_SFLOAT, 192u},
            {"POSB", 2u, VK_FORMAT_R32G32B32_SFLOAT, 0u},
            {"COLORB", 2u, VK_FORMAT_R8G8B8A8_UNORM, 12u},
            {"PSIZEB", 2u, VK_FORMAT_R32_SFLOAT, 16u},
        };

        struct PipeSpec
        {
            const Shader *vs = nullptr, *ps = nullptr, *gs = nullptr;
            bool blend = false;
            VkBlendFactor src = VK_BLEND_FACTOR_ONE, dst = VK_BLEND_FACTOR_ZERO, srcA = VK_BLEND_FACTOR_ONE, dstA = VK_BLEND_FACTOR_ZERO;
            VkBlendOp op = VK_BLEND_OP_ADD, opA = VK_BLEND_OP_ADD;
            uint32_t mask = 15u;
            bool depthTest = false, depthWrite = false;
            VkCompareOp compare = VK_COMPARE_OP_ALWAYS;
            VkFormat colour = VK_FORMAT_UNDEFINED, depth = VK_FORMAT_UNDEFINED;
            VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            bool pointsPerInstance = false; // (the sparks: stream 0 per instance)
        };

        VkPipeline makePipeline(const PipeSpec &s)
        {
            VkPipelineShaderStageCreateInfo stages[3]{};
            uint32_t n = 0;
            const Shader *const shaders[3] = {s.vs, s.gs, s.ps};
            const VkShaderStageFlagBits kinds[3] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
            for (int k = 0; k < 3; ++k)
            {
                if (!shaders[k])
                    continue;
                stages[n].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                stages[n].stage = kinds[k];
                stages[n].module = shaders[k]->module;
                stages[n].pName = shaders[k]->entry.c_str(); // (DXC keeps the HLSL entry point's name)
                ++n;
            }
            // The vertex streams the shader reads: its inputs by semantic.
            std::vector<VkVertexInputAttributeDescription> attributes;
            bool used[3] = {};
            for (const auto &[semantic, location] : s.vs->inputs)
            {
                const Attribute *a = nullptr;
                for (const Attribute &k : kAttributes)
                    if (semantic == k.semantic)
                        a = &k;
                if (!a)
                {
                    std::fprintf(stderr, "[vulkan] a vertex input %s the backend has no stream for\n", semantic.c_str());
                    return VK_NULL_HANDLE;
                }
                attributes.push_back({location, a->binding, a->format, a->offset});
                used[a->binding] = true;
            }
            std::vector<VkVertexInputBindingDescription> bindings;
            if (used[0])
                bindings.push_back({0u, static_cast<uint32_t>(sizeof(Vertex)), s.pointsPerInstance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX});
            if (used[1])
                bindings.push_back({1u, frame::kInstanceStride, VK_VERTEX_INPUT_RATE_INSTANCE});
            if (used[2])
                bindings.push_back({2u, kVertexBStride, VK_VERTEX_INPUT_RATE_VERTEX});
            VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            vi.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
            vi.pVertexBindingDescriptions = bindings.data();
            vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
            vi.pVertexAttributeDescriptions = attributes.data();
            VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            ia.topology = s.topology;
            VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            vp.viewportCount = vp.scissorCount = 1u;
            VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            rs.polygonMode = VK_POLYGON_MODE_FILL;
            rs.cullMode = VK_CULL_MODE_NONE; // (the GS draws both sides; the game culls on the CPU and in VU1)
            rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rs.lineWidth = 1.0f;
            VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            const bool depth = s.depth != VK_FORMAT_UNDEFINED;
            VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            ds.depthTestEnable = depth && (s.depthTest || s.depthWrite);
            ds.depthWriteEnable = depth && s.depthWrite;
            ds.depthCompareOp = s.depthTest ? s.compare : VK_COMPARE_OP_ALWAYS;
            VkPipelineColorBlendAttachmentState ba{};
            ba.blendEnable = s.blend;
            ba.srcColorBlendFactor = s.src;
            ba.dstColorBlendFactor = s.dst;
            ba.colorBlendOp = s.op;
            ba.srcAlphaBlendFactor = s.srcA;
            ba.dstAlphaBlendFactor = s.dstA;
            ba.alphaBlendOp = s.opA;
            ba.colorWriteMask = s.mask; // (the same bits: R 1, G 2, B 4, A 8)
            VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            cb.attachmentCount = 1u;
            cb.pAttachments = &ba;
            VkDynamicState dynamic[4] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                         VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE};
            VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
            dy.dynamicStateCount = bindings.empty() ? 3u : 4u; // (the strides: setVertices gives stream 0's, stream 2's may be 0)
            dy.pDynamicStates = dynamic;
            VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
            ri.colorAttachmentCount = 1u;
            ri.pColorAttachmentFormats = &s.colour;
            ri.depthAttachmentFormat = s.depth;
            VkGraphicsPipelineCreateInfo pc{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            pc.pNext = &ri;
            pc.stageCount = n;
            pc.pStages = stages;
            pc.pVertexInputState = &vi;
            pc.pInputAssemblyState = &ia;
            pc.pViewportState = &vp;
            pc.pRasterizationState = &rs;
            pc.pMultisampleState = &ms;
            pc.pDepthStencilState = &ds;
            pc.pColorBlendState = &cb;
            pc.pDynamicState = &dy;
            pc.layout = g.layout;
            VkPipeline p = VK_NULL_HANDLE;
            if (!ok(vkCreateGraphicsPipelines(g.device, g.cache, 1u, &pc, nullptr, &p), "vkCreateGraphicsPipelines"))
                return VK_NULL_HANDLE;
            g_pipelinesMade.fetch_add(1u, std::memory_order_relaxed);
            return p;
        }

        // (the GS's ALPHA as the blend: nr_draw.h, in neutral terms - here Vulkan's)
        VkBlendFactor factorOf(gfx::Blend b)
        {
            switch (b)
            {
            case gfx::Blend::Zero: return VK_BLEND_FACTOR_ZERO;
            case gfx::Blend::One: return VK_BLEND_FACTOR_ONE;
            case gfx::Blend::SrcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
            case gfx::Blend::InvSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            case gfx::Blend::DestAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
            case gfx::Blend::InvDestAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
            case gfx::Blend::Factor: return VK_BLEND_FACTOR_CONSTANT_COLOR;
            case gfx::Blend::InvFactor: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
            }
            return VK_BLEND_FACTOR_ONE;
        }
        VkCompareOp compareOf(gfx::Compare c)
        {
            return c == gfx::Compare::Never ? VK_COMPARE_OP_NEVER : c == gfx::Compare::Always ? VK_COMPARE_OP_ALWAYS
                   : c == gfx::Compare::GreaterEqual ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_GREATER;
        }

        bool effectMade(gfx::Effect e)
        {
            switch (e)
            {
            case gfx::Effect::None: return true;
            case gfx::Effect::Fxaa: return g.aaVs.module && g.aaPs.module;
            case gfx::Effect::Probe: return g.probeVs.module && g.probePs.module;
            case gfx::Effect::Cloud: return g.cloudVs.module && g.cloudPs.module;
            case gfx::Effect::BloomDown: return g.bloomVs.module && g.bloomDownPs.module;
            case gfx::Effect::BloomUp: return g.bloomVs.module && g.bloomUpPs.module;
            case gfx::Effect::ToneMap: return g.toneVs.module && g.tonePs.module;
            case gfx::Effect::LocalContrast: return g.clarityVs.module && g.clarityPs.module;
            case gfx::Effect::SmaaEdges: return g.smaaReady && g.smaaEdgeVs.module && g.smaaEdgePs.module;
            case gfx::Effect::SmaaWeights: return g.smaaReady && g.smaaWeightVs.module && g.smaaWeightPs.module;
            case gfx::Effect::SmaaBlend: return g.smaaReady && g.smaaBlendVs.module && g.smaaBlendPs.module;
            default: return false;
            }
        }

        uint64_t pipelineKeyOf(const gfx::PipelineDesc &p, gfx::Format colour, bool depth);
        PipeSpec specOf(const gfx::PipelineDesc &p, gfx::Format colour, bool depth);

        // ---- the pixel shader's variants (D3D12's psv::): only the paths a draw uses, compiled and made into pipelines on
        // a thread of their own; until one is ready the whole shader draws (the same pixels, only slower) ----
        namespace psv
        {
            struct Job
            {
                uint64_t key = 0;
                uint32_t variant = 0;
                PipeSpec spec; // (its ps: the variant's, set by the thread)
            };
            std::mutex mutex;
            std::condition_variable wake;
            std::deque<Job> jobs;
            std::vector<std::pair<uint64_t, VkPipeline>> done;
            std::unordered_set<uint64_t> asked; // (render thread)
            bool started = false;
            std::atomic<uint32_t> made{0}, failed{0};

            // The variant's switches (nr_vulkan_shaders.h's variantDefines: nr_d3d.cpp's compileVariant), its SPIR-V from
            // the build's table - every variant is built in.
            bool compileVariant(uint32_t v, Shader &out)
            {
                if (makeShader(out, hlsl::kShader, sizeof(hlsl::kShader) - 1u, "ps", "ps_6_0", shaders::variantDefines(v)))
                    return true;
                std::fprintf(stderr, "[vulkan] pixel shader variant %02x: not made\n", v);
                return false;
            }

            void worker()
            {
                std::unordered_map<uint32_t, Shader> shaders; // (the thread's own: kept for the run)
                for (;;)
                {
                    Job j;
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        wake.wait(lock, [] { return !jobs.empty(); });
                        j = jobs.front();
                        jobs.pop_front();
                    }
                    auto it = shaders.find(j.variant);
                    if (it == shaders.end())
                    {
                        Shader s;
                        if (!compileVariant(j.variant, s))
                            s.module = VK_NULL_HANDLE;
                        it = shaders.emplace(j.variant, s).first;
                    }
                    VkPipeline p = VK_NULL_HANDLE;
                    if (it->second.module)
                    {
                        j.spec.ps = &it->second;
                        p = makePipeline(j.spec); // (pipelines may be made on any thread; the cache is synchronized)
                    }
                    ++(p ? made : failed);
                    std::lock_guard<std::mutex> lock(mutex);
                    done.emplace_back(j.key, p);
                }
            }

            // (Render thread, the frame's start: the finished pipelines into the map - a failed one stays the whole
            // shader's, as D3D12's.)
            void collect(std::unordered_map<uint64_t, VkPipeline> &pipelines)
            {
                std::vector<std::pair<uint64_t, VkPipeline>> got;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    got.swap(done);
                }
                for (auto &[key, p] : got)
                    pipelines[key] = p;
            }
        }

        // The pipeline for a description drawn into a target of that colour format, with or without depth: made on first
        // use, kept (a failed one kept as none: the draws asking for it are left out). A pixel shader variant not made yet
        // is asked of the variants' thread once and the whole shader's pipeline drawn meanwhile.
        VkPipeline pipelineFor(const gfx::PipelineDesc &p, gfx::Format colour, bool depth)
        {
            // (read once: a draw asks twice, and getenv scans the whole environment under a lock - ~1.5 us a call on
            // Windows, nearly 3 ms a frame over the level's ~900 draws)
            static const bool variantsOff = envIs("TF_NATIVE_PSVARIANTS", "0");
            if (p.effect == gfx::Effect::None && p.variant && !p.alphaTest && !variantsOff)
            {
                gfx::PipelineDesc whole = p;
                whole.variant = 0u;
                const VkPipeline base = pipelineFor(whole, colour, depth);
                if (!base)
                    return VK_NULL_HANDLE;
                const uint64_t vkey = (pipelineKeyOf(whole, colour, depth)) | static_cast<uint64_t>(p.variant & 0xFFu) << 32;
                const auto it = g.pipelines.find(vkey);
                if (it != g.pipelines.end())
                    return it->second ? it->second : base;
                if (psv::asked.insert(vkey).second)
                {
                    if (!psv::started)
                    {
                        psv::started = true;
                        std::thread(psv::worker).detach();
                    }
                    psv::Job j;
                    j.key = vkey;
                    j.variant = p.variant & 0xFFu;
                    j.spec = specOf(whole, colour, depth);
                    {
                        std::lock_guard<std::mutex> lock(psv::mutex);
                        psv::jobs.push_back(j);
                    }
                    psv::wake.notify_one();
                }
                return base;
            }
            const uint64_t key = pipelineKeyOf(p, colour, depth);
            const auto it = g.pipelines.find(key);
            if (it != g.pipelines.end())
                return it->second;
            const PipeSpec s = specOf(p, colour, depth);
            VkPipeline made = s.vs && s.ps && s.vs->module && s.ps->module ? makePipeline(s) : VK_NULL_HANDLE;
            g.pipelines[key] = made;
            return made;
        }

        uint64_t pipelineKeyOf(const gfx::PipelineDesc &p, gfx::Format colour, bool depth)
        {
            const bool clip = p.clip && g.geometry && g.gs.module;
            return p.effect != gfx::Effect::None
                       ? (1ull << 63) | static_cast<uint64_t>(p.effect) << 8 | static_cast<uint64_t>(colour) << 4 | (depth ? 1u : 0u)
                       : static_cast<uint64_t>(p.blend.src) | static_cast<uint64_t>(p.blend.dst) << 4 | static_cast<uint64_t>(p.blend.op) << 8 |
                             static_cast<uint64_t>(p.blend.enabled) << 10 | static_cast<uint64_t>(p.depthTest) << 11 |
                             static_cast<uint64_t>(p.depthFunc) << 12 | static_cast<uint64_t>(p.depthWrite) << 14 |
                             static_cast<uint64_t>(p.writeMask & 15u) << 15 | static_cast<uint64_t>(clip) << 19 |
                             static_cast<uint64_t>(colour) << 20 | static_cast<uint64_t>(depth) << 24;
        }

        // A description's pipeline as Vulkan state (the whole pixel shader; a variant's is set by the variants' thread).
        PipeSpec specOf(const gfx::PipelineDesc &p, gfx::Format colour, bool depth)
        {
            const bool clip = p.clip && g.geometry && g.gs.module;
            PipeSpec s;
            s.colour = vkFormat(colour);
            s.depth = depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_UNDEFINED;
            switch (p.effect)
            {
            case gfx::Effect::None:
                s.vs = &g.vs;
                s.ps = &g.ps; // (the whole shader; a variant's pipeline: the variants' thread puts its own in)
                s.gs = clip ? &g.gs : nullptr;
                s.blend = p.blend.enabled;
                s.src = factorOf(p.blend.src);
                s.dst = factorOf(p.blend.dst);
                s.op = p.blend.op == gfx::BlendOp::RevSubtract ? VK_BLEND_OP_REVERSE_SUBTRACT
                       : p.blend.op == gfx::BlendOp::Subtract  ? VK_BLEND_OP_SUBTRACT
                                                              : VK_BLEND_OP_ADD;
                s.mask = p.writeMask & 15u;
                s.depthTest = p.depthTest;
                s.depthWrite = p.depthWrite;
                s.compare = compareOf(p.depthFunc);
                break;
            case gfx::Effect::Fxaa:
                s.vs = &g.aaVs, s.ps = &g.aaPs;
                break;
            case gfx::Effect::Probe:
                s.vs = &g.probeVs, s.ps = &g.probePs;
                break;
            case gfx::Effect::Cloud: // (added into the target, its colour only; a strip of four a point)
                s.vs = &g.cloudVs, s.ps = &g.cloudPs;
                s.blend = true;
                s.src = s.dst = s.srcA = s.dstA = VK_BLEND_FACTOR_ONE;
                s.mask = gfx::kWriteR | gfx::kWriteG | gfx::kWriteB;
                s.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
                s.pointsPerInstance = true;
                break;
            case gfx::Effect::BloomDown:
                s.vs = &g.bloomVs, s.ps = &g.bloomDownPs;
                break;
            case gfx::Effect::BloomUp: // (added to the target times the blend factor, its alpha kept)
                s.vs = &g.bloomVs, s.ps = &g.bloomUpPs;
                s.blend = true;
                s.dst = s.dstA = VK_BLEND_FACTOR_CONSTANT_COLOR;
                s.mask = gfx::kWriteR | gfx::kWriteG | gfx::kWriteB;
                break;
            case gfx::Effect::ToneMap:
                s.vs = &g.toneVs, s.ps = &g.tonePs;
                s.mask = gfx::kWriteR | gfx::kWriteG | gfx::kWriteB;
                break;
            case gfx::Effect::LocalContrast: // (the picture's copy and its blur into the picture, its alpha kept: as the tone mapping)
                s.vs = &g.clarityVs, s.ps = &g.clarityPs;
                s.mask = gfx::kWriteR | gfx::kWriteG | gfx::kWriteB;
                break;
            case gfx::Effect::SmaaEdges: // (each written whole, its target's format: R8G8, then RGBA8 twice)
                s.vs = &g.smaaEdgeVs, s.ps = &g.smaaEdgePs;
                break;
            case gfx::Effect::SmaaWeights:
                s.vs = &g.smaaWeightVs, s.ps = &g.smaaWeightPs;
                break;
            case gfx::Effect::SmaaBlend:
                s.vs = &g.smaaBlendVs, s.ps = &g.smaaBlendPs;
                break;
            default:
                break;
            }
            return s;
        }

        // ---- start ---------------------------------------------------------------------------------------------------
        VKAPI_ATTR VkBool32 VKAPI_CALL debugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                                     const VkDebugUtilsMessengerCallbackDataEXT *data, void *)
        {
            // (The layer adds the clipping geometry shader's input and output clip distances together - 6 + 6 against a
            // limit of 8 that holds for each on its own (checked in its SPIR-V: float[6] in for each corner, float[6] out):
            // told once as a note, not counted as an error.)
            if (data && data->pMessageIdName && std::strstr(data->pMessageIdName, "maxCombinedClipAndCullDistances"))
            {
                static std::atomic<bool> noted{false};
                if (!noted.exchange(true))
                    std::fprintf(stderr, "[vulkan] validation note (the clipping shader's 6 clip distances in and 6 out counted together by the "
                                         "layer; each is within the limit): %s\n", data->pMessage ? data->pMessage : "");
                return VK_FALSE;
            }
            // (A pixel shader variant leaves out inputs its paths do not read; the vertex shader still writes them. The others
            // keep their locations - checked: QDIV stays at 5 with CULL left out - so it is only an output nobody reads.)
            if (data && data->pMessage && std::strstr(data->pMessage, "but there is no corresponding Input declared"))
            {
                static std::atomic<bool> noted{false};
                if (!noted.exchange(true))
                    std::fprintf(stderr, "[vulkan] validation note (a pixel shader variant does not read every output of the vertex shader - "
                                         "allowed; told once): %s\n", data->pMessage);
                return VK_FALSE;
            }
            static std::atomic<int> told{0};
            if (told.fetch_add(1) < 400)
                std::fprintf(stderr, "[vulkan] validation %s: %s\n",
                             (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "error" : "warning", data && data->pMessage ? data->pMessage : "");
            return VK_FALSE;
        }

        bool makeShaders()
        {
            if (shaders::kBakedCount == 0u)
            {
                std::fprintf(stderr, "[vulkan] this build has no SPIR-V built in (no shader compiler for its system yet): nothing is drawn with Vulkan\n");
                return false;
            }
            const auto t0 = Clock::now();
            using namespace hlsl;
            const size_t kS = sizeof(kShader) - 1u;
            if (!makeShader(g.vs, kShader, kS, "vs", "vs_6_0") || !makeShader(g.ps, kShader, kS, "ps", "ps_6_0"))
                return false;
            if (g.geometry)
                makeShader(g.gs, kShader, kS, "gsClip", "gs_6_0"); // (none: the GPU's clipper alone, as D3D12 without it)
            makeShader(g.aaVs, kAaShader, sizeof(kAaShader) - 1u, "aaVs", "vs_6_0");
            makeShader(g.aaPs, kAaShader, sizeof(kAaShader) - 1u, "aaPs", "ps_6_0");
            makeShader(g.probeVs, kProbeShader, sizeof(kProbeShader) - 1u, "probeVs", "vs_6_0");
            makeShader(g.probePs, kProbeShader, sizeof(kProbeShader) - 1u, "probePs", "ps_6_0");
            makeShader(g.cloudVs, kCloudShader, sizeof(kCloudShader) - 1u, "cloudVs", "vs_6_0");
            makeShader(g.cloudPs, kCloudShader, sizeof(kCloudShader) - 1u, "cloudPs", "ps_6_0");
            makeShader(g.bloomVs, kBloomShader, sizeof(kBloomShader) - 1u, "bloomVs", "vs_6_0");
            makeShader(g.bloomDownPs, kBloomShader, sizeof(kBloomShader) - 1u, "bloomDownPs", "ps_6_0");
            makeShader(g.bloomUpPs, kBloomShader, sizeof(kBloomShader) - 1u, "bloomUpPs", "ps_6_0");
            makeShader(g.toneVs, kToneShader, sizeof(kToneShader) - 1u, "toneVs", "vs_6_0");
            makeShader(g.tonePs, kToneShader, sizeof(kToneShader) - 1u, "tonePs", "ps_6_0");
            makeShader(g.clarityVs, kClarityShader, sizeof(kClarityShader) - 1u, "clarityVs", "vs_6_0");
            makeShader(g.clarityPs, kClarityShader, sizeof(kClarityShader) - 1u, "clarityPs", "ps_6_0");
            {
                // SMAA 1x: its prelude (our porting macros, the "high" preset), SMAA.hlsl, its entries - D3D12's text.
                std::string smaa(kSmaaPrelude, sizeof(kSmaaPrelude) - 1u);
                for (const char *piece : kSmaaSource)
                    smaa += piece;
                smaa.append(kSmaaEntries, sizeof(kSmaaEntries) - 1u);
                Shader *const out[6] = {&g.smaaEdgeVs, &g.smaaEdgePs, &g.smaaWeightVs, &g.smaaWeightPs, &g.smaaBlendVs, &g.smaaBlendPs};
                const char *const names[6] = {"smaaEdgeVs", "smaaEdgePs", "smaaWeightVs", "smaaWeightPs", "smaaBlendVs", "smaaBlendPs"};
                for (int k = 0; k < 6; ++k)
                    makeShader(*out[k], smaa.c_str(), smaa.size(), names[k], (k & 1) ? "ps_6_0" : "vs_6_0");
            }
            std::fprintf(stderr, "[vulkan] shaders made from the built-in SPIR-V (%zu) in %.0f ms (the clipping stage %s; effects: FXAA %s, "
                                 "probe %s, sparks %s, bloom %s, tone mapping %s, local contrast %s, SMAA %s)\n",
                         shaders::kBakedCount, std::chrono::duration<double, std::milli>(Clock::now() - t0).count(), g.gs.module ? "made" : "none",
                         effectMade(gfx::Effect::Fxaa) ? "yes" : "no", effectMade(gfx::Effect::Probe) ? "yes" : "no",
                         effectMade(gfx::Effect::Cloud) ? "yes" : "no", effectMade(gfx::Effect::BloomUp) ? "yes" : "no",
                         effectMade(gfx::Effect::ToneMap) ? "yes" : "no", effectMade(gfx::Effect::LocalContrast) ? "yes" : "no",
                         g.smaaEdgePs.module && g.smaaWeightPs.module && g.smaaBlendPs.module ? "yes (its tables at the first frame)" : "no");
            return true;
        }

        bool startDevice()
        {
            const auto t0 = Clock::now();
            const bool windowed = !envIs("TF_VK_WINDOW", "0");
            if (windowed && !openWindow())
                std::fprintf(stderr, "[vulkan] no window: the pictures go only to shots\n");
            SDL_Window *window = g_win.window;
            // (SDL's own Vulkan loader when the window is SDL's - it can have none, then the system's vulkan-1)
            PFN_vkGetInstanceProcAddr sdlLoader = window ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr()) : nullptr;
            if (window && !sdlLoader)
                std::fprintf(stderr, "[vulkan] SDL has no Vulkan loader (%s): the system's vulkan-1 instead\n", SDL_GetError());
            if (sdlLoader)
                volkInitializeCustom(sdlLoader);
            else if (!ok(volkInitialize(), "loading Vulkan (vulkan-1)"))
                return false;
            const uint32_t version = volkGetInstanceVersion();
            if (version < VK_API_VERSION_1_1)
            {
                std::fprintf(stderr, "[vulkan] Vulkan 1.1 is needed (the loader offers %u.%u)\n", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version));
                return false;
            }
            // ---- the instance ----
            std::vector<const char *> extensions, layers;
            if (window)
            {
                Uint32 count = 0;
                if (const char *const *names = SDL_Vulkan_GetInstanceExtensions(&count))
                    extensions.assign(names, names + count);
            }
            const bool debug = envIs("TF_VK_DEBUG", "1");
            if (debug)
            {
                uint32_t n = 0;
                vkEnumerateInstanceLayerProperties(&n, nullptr);
                std::vector<VkLayerProperties> have(n);
                vkEnumerateInstanceLayerProperties(&n, have.data());
                for (const VkLayerProperties &l : have)
                    if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0)
                        layers.push_back("VK_LAYER_KHRONOS_validation");
                vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
                std::vector<VkExtensionProperties> exts(n);
                vkEnumerateInstanceExtensionProperties(nullptr, &n, exts.data());
                for (const VkExtensionProperties &e : exts)
                    if (std::strcmp(e.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0)
                        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                std::fprintf(stderr, "[vulkan] TF_VK_DEBUG: validation layer %s\n", layers.empty() ? "not installed" : "on");
            }
            VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
            app.pApplicationName = "Transformers";
            app.pEngineName = "TransformersRecomp native renderer";
            app.apiVersion = std::min(version, static_cast<uint32_t>(VK_API_VERSION_1_3));
            VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
            ici.pApplicationInfo = &app;
            ici.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
            ici.ppEnabledExtensionNames = extensions.data();
            ici.enabledLayerCount = static_cast<uint32_t>(layers.size());
            ici.ppEnabledLayerNames = layers.data();
            if (!ok(vkCreateInstance(&ici, nullptr, &g.instance), "vkCreateInstance"))
                return false;
            volkLoadInstance(g.instance);
            if (debug && vkCreateDebugUtilsMessengerEXT)
            {
                VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
                mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
                mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
                mi.pfnUserCallback = &debugMessage;
                vkCreateDebugUtilsMessengerEXT(g.instance, &mi, nullptr, &g.messenger);
            }
            if (window && !SDL_Vulkan_CreateSurface(window, g.instance, nullptr, &g.surface))
            {
                std::fprintf(stderr, "[vulkan] no surface for the window: %s\n", SDL_GetError());
                g.surface = VK_NULL_HANDLE;
            }
            // ---- the GPU: TF_VK_GPU=<n>, else the first discrete one, else the first that draws ----
            uint32_t n = 0;
            vkEnumeratePhysicalDevices(g.instance, &n, nullptr);
            std::vector<VkPhysicalDevice> gpus(n);
            vkEnumeratePhysicalDevices(g.instance, &n, gpus.data());
            auto familyOf = [](VkPhysicalDevice p) -> uint32_t {
                uint32_t m = 0;
                vkGetPhysicalDeviceQueueFamilyProperties(p, &m, nullptr);
                std::vector<VkQueueFamilyProperties> fams(m);
                vkGetPhysicalDeviceQueueFamilyProperties(p, &m, fams.data());
                for (uint32_t i = 0; i < m; ++i)
                {
                    if (!(fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
                        continue;
                    VkBool32 present = VK_TRUE;
                    if (g.surface)
                        vkGetPhysicalDeviceSurfaceSupportKHR(p, i, g.surface, &present);
                    if (present)
                        return i;
                }
                return ~0u;
            };
            int pick = -1;
            const char *asked = std::getenv("TF_VK_GPU");
            for (uint32_t i = 0; i < n; ++i)
            {
                VkPhysicalDeviceProperties p{};
                vkGetPhysicalDeviceProperties(gpus[i], &p);
                const bool usable = familyOf(gpus[i]) != ~0u && p.apiVersion >= VK_API_VERSION_1_1;
                std::fprintf(stderr, "[vulkan] GPU %u: %s (Vulkan %u.%u.%u, %s)%s\n", i, p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
                             VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion),
                             p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? "discrete"
                             : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "integrated"
                                                                                       : "other",
                             usable ? "" : " - cannot draw to this window");
                if (!usable)
                    continue;
                if (asked && *asked)
                {
                    if (std::atoi(asked) == static_cast<int>(i))
                        pick = static_cast<int>(i);
                }
                else if (pick < 0 || (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && [&] {
                             VkPhysicalDeviceProperties q{};
                             vkGetPhysicalDeviceProperties(gpus[pick], &q);
                             return q.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
                         }()))
                    pick = static_cast<int>(i);
            }
            if (pick < 0)
            {
                std::fprintf(stderr, "[vulkan] no GPU that can draw%s\n", g.surface ? " to the window" : "");
                return false;
            }
            g.phys = gpus[static_cast<size_t>(pick)];
            g.family = familyOf(g.phys);
            vkGetPhysicalDeviceProperties(g.phys, &g.props);
            vkGetPhysicalDeviceMemoryProperties(g.phys, &g.mem);
            {
                uint32_t m = 0;
                vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &m, nullptr);
                std::vector<VkQueueFamilyProperties> fams(m);
                vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &m, fams.data());
                g.timestampBits = fams[g.family].timestampValidBits;
            }
            // ---- the device: Vulkan 1.3's dynamic rendering and the features the draws use ----
            vkEnumerateDeviceExtensionProperties(g.phys, nullptr, &n, nullptr);
            std::vector<VkExtensionProperties> have(n);
            vkEnumerateDeviceExtensionProperties(g.phys, nullptr, &n, have.data());
            auto offered = [&](const char *name) {
                return std::any_of(have.begin(), have.end(), [name](const VkExtensionProperties &e) { return std::strcmp(e.extensionName, name) == 0; });
            };
            std::vector<const char *> deviceExtensions;
            if (g.surface)
            {
                if (!offered(VK_KHR_SWAPCHAIN_EXTENSION_NAME))
                {
                    std::fprintf(stderr, "[vulkan] the GPU has no swapchain: no window\n");
                    vkDestroySurfaceKHR(g.instance, g.surface, nullptr);
                    g.surface = VK_NULL_HANDLE;
                }
                else
                    deviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
            }
            if (offered(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME))
            {
                deviceExtensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
                g.memoryBudget = true;
            }
            const bool is13 = app.apiVersion >= VK_API_VERSION_1_3 && g.props.apiVersion >= VK_API_VERSION_1_3;
            VkPhysicalDeviceVulkan13Features have13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            VkPhysicalDeviceFeatures2 haveF{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            if (is13)
                haveF.pNext = &have13;
            vkGetPhysicalDeviceFeatures2(g.phys, &haveF);
            VkPhysicalDeviceVulkan13Features want13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            want13.dynamicRendering = have13.dynamicRendering;
            want13.shaderDemoteToHelperInvocation = have13.shaderDemoteToHelperInvocation; // (discard as Direct3D's: nr_vulkan_shaders.h)
            VkPhysicalDeviceFeatures2 wantF{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            wantF.features.samplerAnisotropy = haveF.features.samplerAnisotropy;
            wantF.features.geometryShader = haveF.features.geometryShader;
            wantF.features.shaderClipDistance = haveF.features.shaderClipDistance;
            wantF.features.textureCompressionBC = haveF.features.textureCompressionBC;
            if (is13)
                wantF.pNext = &want13;
            g.anisotropy = haveF.features.samplerAnisotropy;
            g.geometry = haveF.features.geometryShader;
            g.bc7 = haveF.features.textureCompressionBC;
            g.canDraw = is13 && have13.dynamicRendering && have13.shaderDemoteToHelperInvocation && haveF.features.shaderClipDistance;
            if (!g.canDraw)
                std::fprintf(stderr, "[vulkan] this GPU cannot draw the game with Vulkan yet (Vulkan 1.3 with dynamic rendering, demote to "
                                     "helper and clip distances needed): frames, targets and the window only\n");
            const float priority = 1.0f;
            VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            qi.queueFamilyIndex = g.family;
            qi.queueCount = 1u;
            qi.pQueuePriorities = &priority;
            VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
            di.pNext = &wantF;
            di.queueCreateInfoCount = 1u;
            di.pQueueCreateInfos = &qi;
            di.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
            di.ppEnabledExtensionNames = deviceExtensions.data();
            if (!ok(vkCreateDevice(g.phys, &di, nullptr, &g.device), "vkCreateDevice"))
                return false;
            volkLoadDevice(g.device);
            vkGetDeviceQueue(g.device, g.family, 0u, &g.queue);
            // ---- the memory allocator ----
            {
                VmaVulkanFunctions fns{};
                fns.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
                fns.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
                VmaAllocatorCreateInfo ai{};
                ai.flags = g.memoryBudget ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT : 0u;
                ai.physicalDevice = g.phys;
                ai.device = g.device;
                ai.instance = g.instance;
                ai.vulkanApiVersion = is13 ? VK_API_VERSION_1_3 : VK_API_VERSION_1_1;
                ai.pVulkanFunctions = &fns;
                if (!ok(vmaCreateAllocator(&ai, &g.vma), "vmaCreateAllocator"))
                    return false;
            }
            // ---- the frames' command buffers, fences and semaphores; the time stamps ----
            VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pi.queueFamilyIndex = g.family;
            if (!ok(vkCreateCommandPool(g.device, &pi, nullptr, &g.pool), "vkCreateCommandPool"))
                return false;
            VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            ai.commandPool = g.pool;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = kSlots;
            if (!ok(vkAllocateCommandBuffers(g.device, &ai, g.cmd), "vkAllocateCommandBuffers") ||
                !ok(vkAllocateCommandBuffers(g.device, &ai, g.upCmd), "vkAllocateCommandBuffers"))
                return false;
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            for (uint32_t s = 0; s < kSlots; ++s)
                if (!ok(vkCreateFence(g.device, &fi, nullptr, &g.fence[s]), "vkCreateFence") ||
                    !ok(vkCreateSemaphore(g.device, &si, nullptr, &g.acquired[s]), "vkCreateSemaphore"))
                    return false;
            if (g.timestampBits)
            {
                VkQueryPoolCreateInfo qp{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
                qp.queryType = VK_QUERY_TYPE_TIMESTAMP;
                qp.queryCount = kSlots * gfx::kTimestampsPerFrame;
                if (!ok(vkCreateQueryPool(g.device, &qp, nullptr, &g.stamps), "vkCreateQueryPool"))
                    g.stamps = VK_NULL_HANDLE;
                qp.queryCount = kSlots * 2u;
                if (!ok(vkCreateQueryPool(g.device, &qp, nullptr, &g.picStamps), "vkCreateQueryPool"))
                    g.picStamps = VK_NULL_HANDLE;
            }
            // ---- the rings (one buffer: constants, instances, vertices - two slots each - and the pictures' b1, then room
            // for b0's whole range at the end), the slots' upload buffers, the white texture ----
            g.ringBase[0] = 0u;
            g.ringBase[1] = g.ringBase[0] + kRingBytes[0] * kSlots;
            g.ringBase[2] = g.ringBase[1] + kRingBytes[1] * kSlots;
            g.ringBase[3] = g.ringBase[2] + kRingBytes[2] * kSlots;
            if (!makeBuffer(g.ring, g.ringBase[3] + kPictureBytes * kSlots + kB0Range,
                            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, false))
                return false;
            for (uint32_t s = 0; s < kSlots; ++s)
                if (!makeBuffer(g.staging[s], kStagingBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false))
                    return false;
            if (!makeImage(g.white, 1u, 1u, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
                return false;
            // (SMAA's lookup tables: none - SMAA left out, FXAA drawn - if they cannot be made)
            if (!makeImage(g.smaaArea, AREATEX_WIDTH, AREATEX_HEIGHT, VK_FORMAT_R8G8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT) ||
                !makeImage(g.smaaSearch, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT))
            {
                destroyImage(g.smaaArea);
                destroyImage(g.smaaSearch);
            }
            // ---- the bindings, the shaders ----
            if (g.canDraw && (!makeBindings(presetFor(memoryMB())) || !makeShaders()))
                g.canDraw = false;
            if (g.surface)
                makeSwapchain();
            std::fprintf(stderr, "[vulkan] %s, Vulkan %u.%u (driver %u), %s; made in %.0f ms\n", g.props.deviceName,
                         VK_API_VERSION_MAJOR(app.apiVersion), VK_API_VERSION_MINOR(app.apiVersion), g.props.driverVersion,
                         g.surface ? "its own window" : "no window",
                         std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
            std::fprintf(stderr, "[vulkan] milestone 2: %s\n",
                         !g.canDraw ? "no draws on this GPU - the game stays drawn by the emulated GS"
                         : alone()  ? "the game's draws; TF_VK_ALONE: Vulkan draws the game (the main window shows its picture with TF_NATIVE_SHARE=0)"
                                    : "the game's draws, shown in the Vulkan window; the emulated GS keeps drawing the main window (TF_VK_ALONE=1: Vulkan alone)");
            g.since = Clock::now();
            return true;
        }

        bool start()
        {
            std::lock_guard<std::mutex> lock(g_startMutex);
            if (g.ready || g.failed)
                return g.ready;
            g.failed = true;
            if (!startDevice())
            {
                std::fprintf(stderr, "[vulkan] not started: the native renderer stays off\n");
                return false;
            }
            g.failed = false;
            g.ready = true;
            return true;
        }

        // ---- the slots ---------------------------------------------------------------------------------------------
        // TF_VK_ALONE: the slot's picture, read back, handed to the main window.
        void publish(uint32_t s)
        {
            const gfx::ReadbackId r = g.publishRead[s];
            g.publishRead[s] = 0;
            if (!r || r >= g.readbacks.size() || !g.readbacks[r].live)
                return;
            ReadbackBuffer &rb = g.readbacks[r];
            if (rb.format == gfx::Format::RGBA8 && rb.mem.mapped)
            {
#if defined(__linux__)
                const int64_t readyNs = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
                g_pub.publish(rb.mem.mapped, rb.w, rb.h, rb.rowBytes, g.publishHas3D[s], g.publishTick[s], g.publishDueNs[s],
                              readyNs, g.publishTurn[s]);
#else
                std::lock_guard<std::mutex> lock(g_pub.mutex);
                g_pub.pixels.resize(static_cast<size_t>(rb.w) * rb.h);
                for (uint32_t y = 0; y < rb.h; ++y)
                    std::memcpy(g_pub.pixels.data() + static_cast<size_t>(y) * rb.w, rb.mem.mapped + static_cast<size_t>(y) * rb.rowBytes, rb.w * 4u);
                g_pub.w = rb.w;
                g_pub.h = rb.h;
                g_pub.has3D = g.publishHas3D[s];
                ++g_pub.serial;
                g_pub.lastNs = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
#endif
            }
#if !defined(__linux__)
            g_pub.cv.notify_all();
#endif
            rb.live = false;
            g.readbackReleased[rb.slot].push_back(r);
        }

        // The slot's last frame finished on the GPU: what it let go of destroyed, its frame-done call made.
        void finishSlot(uint32_t s)
        {
            if (g.inFlight[s])
            {
                const auto t0 = Clock::now();
                const VkResult r = vkWaitForFences(g.device, 1, &g.fence[s], VK_TRUE, 10ull * 1000000000ull);
                if (r == VK_TIMEOUT)
                {
                    if (!g_lost.exchange(true))
                        std::fprintf(stderr, "[vulkan] a frame did not finish in 10 s (a GPU hang): nothing more is drawn with Vulkan\n");
                    return;
                }
                if (!ok(r, "vkWaitForFences"))
                    return;
                vkResetFences(g.device, 1, &g.fence[s]);
                g.inFlight[s] = false;
                const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
                g_slotWaitAll.store(g_slotWaitAll.load(std::memory_order_relaxed) + ms, std::memory_order_relaxed);
                // (The picture's time on the GPU, as D3D12's device takes it: the profile's whole frame, gpuPictureMs.)
                if (g.picTimed[s])
                {
                    g.picTimed[s] = false;
                    uint64_t t[2] = {};
                    if (vkGetQueryPoolResults(g.device, g.picStamps, s * 2u, 2u, sizeof(t), t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
                    {
                        const uint64_t mask = g.timestampBits >= 64u ? ~0ull : (1ull << g.timestampBits) - 1ull;
                        const uint64_t d = ((t[1] & mask) - (t[0] & mask)) & mask;
                        const double gpuMs = static_cast<double>(d) * static_cast<double>(g.props.limits.timestampPeriod) / 1.0e6;
                        if (gpuMs > 0.0 && gpuMs < 1000.0)
                        {
                            frame::g_frameGpu += gpuMs;
                            ++frame::g_frameGpuFrames;
                            const double was = g_gpuPictureMs.load(std::memory_order_relaxed);
                            g_gpuPictureMs.store(was <= 0.0 ? gpuMs : (gpuMs > was ? 0.8 * was + 0.2 * gpuMs : 0.97 * was + 0.03 * gpuMs),
                                                 std::memory_order_relaxed);
                        }
                    }
                }
                if (g.frameDone)
                    g.frameDone(s);
                publish(s);
            }
            for (Image &im : g.retired[s])
                destroyImage(im);
            g.retired[s].clear();
            for (Buffer &b : g.retiredBuffers[s])
                destroyBuffer(b);
            g.retiredBuffers[s].clear();
        }

        void writeBmp(const std::string &path, const gfx::Readback &rb)
        {
            FILE *f = std::fopen(path.c_str(), "wb");
            if (!f)
            {
                std::fprintf(stderr, "[vulkan] picture not saved: %s did not open\n", path.c_str());
                return;
            }
            const uint32_t rowBytes = rb.width * 4u, size = rowBytes * rb.height;
            uint8_t header[54] = {'B', 'M'};
            auto put32 = [&](int at, uint32_t v) { std::memcpy(header + at, &v, 4); };
            put32(2, 54u + size);
            put32(10, 54u);
            put32(14, 40u);
            put32(18, rb.width);
            put32(22, rb.height);
            header[26] = 1;
            header[28] = 32;
            put32(34, size);
            std::fwrite(header, 1, sizeof(header), f);
            std::vector<uint8_t> row(rowBytes);
            const bool rgba = rb.format == gfx::Format::RGBA8;
            for (uint32_t y = rb.height; y-- > 0;) // (BMP rows bottom up, BGRA - as the D3D12 device saves them)
            {
                std::memcpy(row.data(), static_cast<const uint8_t *>(rb.data) + static_cast<size_t>(y) * rb.rowBytes, rowBytes);
                if (rgba)
                    for (uint32_t x = 0; x < rowBytes; x += 4u)
                        std::swap(row[x], row[x + 2u]);
                std::fwrite(row.data(), 1, rowBytes, f);
            }
            std::fclose(f);
            std::fprintf(stderr, "[native render] picture saved: %s\n", path.c_str());
        }

        // ---- uploads: the frame's upload list (submitted just before the frame's own) and its staging buffer -------------
        VkCommandBuffer upList()
        {
            const uint32_t s = g.slot;
            if (g.inFlight[s]) // (outside a frame - after endFrame, before the next prepareFrame - the slot is the GPU's)
                return VK_NULL_HANDLE;
            if (!g.upBegun[s])
            {
                VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                if (!ok(vkResetCommandBuffer(g.upCmd[s], 0), "vkResetCommandBuffer") || !ok(vkBeginCommandBuffer(g.upCmd[s], &bi), "vkBeginCommandBuffer"))
                    return VK_NULL_HANDLE;
                g.upBegun[s] = true;
            }
            return g.upCmd[s];
        }

        // `bytes` copied where the upload list reads them: the slot's staging buffer, or a buffer of its own let go of with
        // the slot's frame.
        bool stage(const void *data, size_t bytes, VkBuffer &buffer, VkDeviceSize &offset)
        {
            const uint32_t s = g.slot;
            const uint64_t at = (g.stagingAt[s] + 15u) & ~uint64_t(15u); // (BC7's block, RGBA8's texel)
            if (at + bytes <= kStagingBytes)
            {
                std::memcpy(g.staging[s].mapped + at, data, bytes);
                g.stagingAt[s] = at + bytes;
                buffer = g.staging[s].buffer;
                offset = at;
                return true;
            }
            Buffer own;
            if (!makeBuffer(own, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false))
                return false;
            std::memcpy(own.mapped, data, bytes);
            buffer = own.buffer;
            offset = 0u;
            retire(own, s);
            return true;
        }

        // An image made for the texture and its levels uploaded on this frame's upload list (ready for this frame's draws).
        struct Level
        {
            const void *data;
            size_t bytes;
        };
        // (A failed upload lets go of its image - at once when nothing was recorded with it, else once this slot's frame is
        // done - so that a later try never makes a new one over it.)
        bool uploadLevels(Image &im, uint32_t w, uint32_t h, gfx::Format format, const std::vector<Level> &levels)
        {
            if (im.image)
                retire(im, g.slot); // (one left by an earlier try)
            if (levels.empty() || !makeImage(im, w, h, vkFormat(format), VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                             VK_IMAGE_ASPECT_COLOR_BIT, static_cast<uint32_t>(levels.size())))
                return false;
            const VkCommandBuffer u = upList();
            if (!u)
            {
                destroyImage(im);
                return false;
            }
            transition(u, im.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            for (uint32_t l = 0; l < levels.size(); ++l)
            {
                VkBuffer buffer = VK_NULL_HANDLE;
                VkDeviceSize offset = 0;
                if (!stage(levels[l].data, levels[l].bytes, buffer, offset))
                {
                    retire(im, g.slot); // (the upload list already names it)
                    return false;
                }
                VkBufferImageCopy region{};
                region.bufferOffset = offset;
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0u, 1u};
                region.imageExtent = {std::max(1u, w >> l), std::max(1u, h >> l), 1u};
                vkCmdCopyBufferToImage(u, buffer, im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                g.uploadsBytes += levels[l].bytes;
            }
            transition(u, im.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            im.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            ++g.uploads;
            return true;
        }

        // uploadTextureLater's textures, at the frame's start: a source gone is skipped, one still being converted waits.
        void uploadLater()
        {
            std::deque<Later> keep;
            while (!g.later.empty())
            {
                Later item = std::move(g.later.front());
                g.later.pop_front();
                if (item.id >= g.textures.size())
                    continue;
                TextureInfo &t = g.textures[item.id];
                if (!t.live || t.ready || t.generation != item.generation)
                    continue;
                const std::shared_ptr<const Texture> src = item.source.lock();
                if (!src)
                    continue;
                if (src->pending && !src->pending->done.load(std::memory_order_acquire))
                {
                    keep.push_back(std::move(item));
                    continue;
                }
                // (The levels as D3D12's uploader makes them, up::record: a large image's BC7 blocks as they are, up to the
                // first level of the wrong size; an RGBA8 texture's chain to 1 x 1, the levels the game or the converter
                // did not make box-filtered here - frame::mipChain, the shared code. With the levels given alone, the far
                // ferns read their sharp level 0 and came out brighter than D3D12's.)
                std::vector<Level> levels;
                std::vector<const uint32_t *> mips;
                std::vector<std::vector<uint32_t>> computed;
                gfx::Format format = gfx::Format::RGBA8;
                if (src->format == 1u)
                {
                    if (!g.bc7)
                        continue; // (no BC7 here: the game's own texture stays drawn - milestone 4 decompresses it)
                    format = gfx::Format::BC7;
                    for (size_t l = 0; l < src->blocks.size(); ++l)
                    {
                        const uint32_t w = std::max(1u, src->width >> l), h = std::max(1u, src->height >> l);
                        if (src->blocks[l].size() != static_cast<size_t>((w + 3u) / 4u) * ((h + 3u) / 4u) * 16u)
                            break;
                        levels.push_back({src->blocks[l].data(), src->blocks[l].size()});
                    }
                    if (levels.empty())
                        continue;
                }
                else
                {
                    if (src->pixels.size() < static_cast<size_t>(src->width) * src->height)
                        continue;
                    frame::mipChain(src->width, src->height, src->pixels.data(), &src->mips, mips, computed);
                    uint32_t w = src->width, h = src->height;
                    for (const uint32_t *level : mips)
                    {
                        levels.push_back({level, static_cast<size_t>(w) * h * 4u});
                        w = std::max(1u, w / 2u);
                        h = std::max(1u, h / 2u);
                    }
                }
                t.format = format;
                t.levels = static_cast<uint32_t>(levels.size());
                if (uploadLevels(t.image, src->width, src->height, format, levels))
                    t.ready = true;
            }
            g.later.swap(keep);
        }

        // ---- the device ----------------------------------------------------------------------------------------------
        class VulkanDevice final : public gfx::Device
        {
        public:
            // --- frame life ---
            bool prepareFrame(uint32_t pictureWidth, uint32_t pictureHeight) override
            {
                if (!g.ready || g_lost.load(std::memory_order_relaxed))
                    return false;
                g.slot = static_cast<uint32_t>(g.frameIndex % kSlots);
                finishSlot(g.slot);
                if (g_lost.load(std::memory_order_relaxed))
                    return false;
                // (the slot's per-frame memory: its frame before is done)
                for (VkDescriptorPool pool : g.pools[g.slot])
                    vkResetDescriptorPool(g.device, pool, 0);
                g.poolAt[g.slot] = 0;
                g.stagingAt[g.slot] = 0;
                g.upBegun[g.slot] = false;
                if (pictureWidth != g.picW || pictureHeight != g.picH || !g.picColour.image)
                {
                    // (The other slot's frame may still read the old picture: let go of after it.)
                    const uint32_t other = (g.slot + 1u) % kSlots;
                    retire(g.picColour, other);
                    retire(g.picDepth, other);
                    if (!makeColour(g.picColour, pictureWidth, pictureHeight, gfx::Format::RGBA8) || !makeDepth(g.picDepth, pictureWidth, pictureHeight))
                    {
                        g.picW = g.picH = 0;
                        return false;
                    }
                    g.picW = pictureWidth;
                    g.picH = pictureHeight;
                    g.picZero = true;
                    ++frame::g_picturesMade;
                    std::fprintf(stderr, "[vulkan] the picture made at %ux%u\n", pictureWidth, pictureHeight);
                }
                // (The swapchain made again after the window changed, or when the last present said so - at most twice a
                // second while it cannot be made: a minimised window has no size.)
                // (the window's first size event, at its making, changes nothing: the swapchain already has that size)
                const bool resized = g_win.resized.exchange(false) &&
                                     !(g.swapchain && !g.swapStale && static_cast<uint32_t>(g_win.w.load()) == g.swapExtent.width &&
                                       static_cast<uint32_t>(g_win.h.load()) == g.swapExtent.height);
                static Clock::time_point lastTry{};
                if (g.surface && (resized || ((g.swapStale || !g.swapchain) && Clock::now() - lastTry > std::chrono::milliseconds(500))))
                {
                    lastTry = Clock::now();
                    waitIdle();
                    for (uint32_t s = 0; s < kSlots; ++s)
                        finishSlot(s);
                    makeSwapchain();
                }
                return true;
            }

            bool beginFrame(uint64_t) override
            {
                const uint32_t s = g.slot;
                g.readbackFree[s].insert(g.readbackFree[s].end(), g.readbackReleased[s].begin(), g.readbackReleased[s].end());
                g.readbackReleased[s].clear();
                VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                if (!ok(vkResetCommandBuffer(g.cmd[s], 0), "vkResetCommandBuffer") || !ok(vkBeginCommandBuffer(g.cmd[s], &bi), "vkBeginCommandBuffer"))
                    return false;
                g.recording = true;
                g.readsThisFrame = false;
                if (g.stamps)
                    vkCmdResetQueryPool(g.cmd[s], g.stamps, s * gfx::kTimestampsPerFrame, gfx::kTimestampsPerFrame);
                g.stampsUsed[s] = 0;
                if (g.picStamps)
                {
                    vkCmdResetQueryPool(g.cmd[s], g.picStamps, s * 2u, 2u);
                    vkCmdWriteTimestamp(g.cmd[s], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g.picStamps, s * 2u);
                }
                for (uint64_t &at : g.ringAt)
                    at = 0;
                // (the recording's state forgotten: nothing bound in a new list)
                g.rendering = false;
                g.target = 0;
                g.withDepth = false;
                g.haveDesc = false;
                g.viewportEmpty = false;
                g.tex[0] = g.tex[1] = g.tex[2] = 0u;
                g.b0 = 0u;
                g.vb = g.ib = Stream{};
                g.vbB = Stream{g.ring.buffer, g.ringBase[2] + static_cast<uint64_t>(s) * kRingBytes[2], 0u};
                g.boundPipeline = VK_NULL_HANDLE;
                g.boundHasStreams = false;
                g.streamsDirty = g.indexDirty = true;
                g.boundSet = VK_NULL_HANDLE;
                g.boundB0 = ~0u;
                g.setsThisFrame = g.drawsThisFrame = 0;
                psv::collect(g.pipelines); // (the variants made since the frame before)
                if (g.picZero)
                {
                    g.picZero = false;
                    zero(g.picColour);
                    zero(g.picDepth);
                }
                for (const gfx::TargetId id : g.zeroLater)
                    if (Target *t = targetOf(id))
                    {
                        zero(t->colour);
                        zero(t->depth);
                    }
                g.zeroLater.clear();
                if (g.canDraw)
                {
                    if (!g.whiteUploaded)
                    {
                        // (texture 0: white, 1 x 1 - a draw's slot with nothing in it reads it)
                        const VkCommandBuffer u = upList();
                        const uint32_t texel = 0xFFFFFFFFu;
                        VkBuffer buffer = VK_NULL_HANDLE;
                        VkDeviceSize offset = 0;
                        if (u && stage(&texel, 4u, buffer, offset))
                        {
                            transition(u, g.white.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                            VkBufferImageCopy region{};
                            region.bufferOffset = offset;
                            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
                            region.imageExtent = {1u, 1u, 1u};
                            vkCmdCopyBufferToImage(u, buffer, g.white.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                            transition(u, g.white.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                            g.white.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                            g.whiteUploaded = true;
                            // SMAA's tables, once (their rows as packed as the headers have them: the pitch is the width)
                            const unsigned char *const data[2] = {areaTexBytes, searchTexBytes};
                            const size_t bytes[2] = {sizeof(areaTexBytes), sizeof(searchTexBytes)};
                            Image *const lookups[2] = {&g.smaaArea, &g.smaaSearch};
                            bool made = g.smaaArea.image && g.smaaSearch.image;
                            for (int k = 0; made && k < 2; ++k)
                            {
                                VkBuffer lb = VK_NULL_HANDLE;
                                VkDeviceSize lo = 0;
                                if (!stage(data[k], bytes[k], lb, lo))
                                {
                                    made = false;
                                    break;
                                }
                                transition(u, lookups[k]->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                                VkBufferImageCopy r{};
                                r.bufferOffset = lo;
                                r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
                                r.imageExtent = {lookups[k]->w, lookups[k]->h, 1u};
                                vkCmdCopyBufferToImage(u, lb, lookups[k]->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
                                transition(u, lookups[k]->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                                lookups[k]->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                            }
                            g.smaaReady = made;
                        }
                    }
                    uploadLater();
                }
                return true;
            }

            uint32_t frameSlot() override { return g.slot; }

            bool endFrame(const gfx::FrameEnd &end) override
            {
                if (!g.recording)
                    return false;
                endRendering();
                const uint32_t s = g.slot;
                VkCommandBuffer c = g.cmd[s];
                // (The picture's end time stamp here, its drawing done - before its hand-over: the shot's and TF_VK_ALONE's
                // read-backs and the copy into the window, as D3D12's picture time ends before its own hand-over.)
                if (g.picStamps)
                {
                    vkCmdWriteTimestamp(c, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.picStamps, s * 2u + 1u);
                    g.picTimed[s] = true;
                }
                Target *pic = targetOf(end.picture);
                Image &picture = pic ? colourOf(*pic) : g.picColour;
                // (TF_VK_TEST=1: the picture cleared to a colour that cycles with the frame's VBlank - each picture's own.)
                static const bool test = envIs("TF_VK_TEST", "1");
                if (test && picture.image && end.frame)
                {
                    const float hue = static_cast<float>(end.frame->tick % 240u) / 240.0f * 6.0f;
                    const float x = 1.0f - std::fabs(std::fmod(hue, 2.0f) - 1.0f);
                    const int k = static_cast<int>(hue) % 6;
                    const float rgb[6][3] = {{1, x, 0}, {x, 1, 0}, {0, 1, x}, {0, x, 1}, {x, 0, 1}, {1, 0, x}};
                    const float rgba[4] = {rgb[k][0], rgb[k][1], rgb[k][2], 1.0f};
                    clearColour(end.picture ? end.picture : 0u, rgba);
                    if (!pic)
                    {
                        moveTo(picture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                        VkClearColorValue v{};
                        std::memcpy(v.float32, rgba, sizeof(rgba));
                        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
                        vkCmdClearColorImage(c, picture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range);
                    }
                }
                // The picture's shot (its own, else the frame's) read back here, saved once the frame is done; TF_VK_ALONE's
                // picture for the main window read back too, handed over in the slot's frame-done.
                std::string shot = !end.pictures.empty() && !end.pictures[0].shot.empty() ? end.pictures[0].shot : end.shot;
                const gfx::ReadbackId shotRead = !shot.empty() && end.picture ? readTarget(end.picture) : 0u;
                if (alone() && end.picture && g.canDraw)
                {
                    g.publishRead[s] = readTarget(end.picture);
                    g.publishHas3D[s] = end.frame && end.frame->has3D;
#if defined(__linux__)
                    g.publishTick[s] = end.frame ? end.frame->tick : 0;
                    g.publishDueNs[s] = end.pictures.empty() ? 0 : end.pictures[0].dueNs; // (the picture Vulkan draws: the first)
                    g.publishTurn[s] = g.frameIndex;
#endif
                }
                // The picture into the window: the swapchain's next image, the picture fitted into it (bars black).
                uint32_t imageIndex = 0;
                bool acquired = false;
                if (g.swapchain && !g.swapStale && !g_win.closed.load(std::memory_order_relaxed) && !g_win.unseen.load(std::memory_order_relaxed) &&
                    picture.image)
                {
                    // (Not waited for: with no image free - the screen still showing the ones before - this picture is not
                    // shown, and the render thread is never held up by this window. Pacing to the screen: milestone 3.)
                    const VkResult r = vkAcquireNextImageKHR(g.device, g.swapchain, 0ull, g.acquired[s], VK_NULL_HANDLE, &imageIndex);
                    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR)
                    {
                        acquired = true;
                        g.swapStale = r == VK_SUBOPTIMAL_KHR;
                    }
                    else if (r == VK_ERROR_OUT_OF_DATE_KHR)
                        g.swapStale = true;
                    else if (r != VK_TIMEOUT && r != VK_NOT_READY)
                        ok(r, "vkAcquireNextImageKHR");
                }
                if (acquired)
                {
                    moveTo(picture, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                    const VkImage dst = g.swapImages[imageIndex];
                    transition(c, dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                    const VkClearColorValue black{};
                    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
                    vkCmdClearColorImage(c, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
                    VkMemoryBarrier clearDone{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                    clearDone.srcAccessMask = clearDone.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &clearDone, 0, nullptr, 0, nullptr);
                    // (fitted, its shape kept: the picture is already the shape it is shown at - the frame builder made it so)
                    const double sx = static_cast<double>(g.swapExtent.width) / picture.w, sy = static_cast<double>(g.swapExtent.height) / picture.h;
                    const double scale = std::min(sx, sy);
                    const int32_t w = std::max(1, static_cast<int32_t>(std::lround(picture.w * scale)));
                    const int32_t h = std::max(1, static_cast<int32_t>(std::lround(picture.h * scale)));
                    const int32_t x0 = (static_cast<int32_t>(g.swapExtent.width) - w) / 2, y0 = (static_cast<int32_t>(g.swapExtent.height) - h) / 2;
                    VkImageBlit blit{};
                    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
                    blit.srcOffsets[1] = {static_cast<int32_t>(picture.w), static_cast<int32_t>(picture.h), 1};
                    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
                    blit.dstOffsets[0] = {x0, y0, 0};
                    blit.dstOffsets[1] = {x0 + w, y0 + h, 1};
                    vkCmdBlitImage(c, picture.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                                   (w == static_cast<int32_t>(picture.w) && h == static_cast<int32_t>(picture.h)) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
                    transition(c, dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
                }
                if (g.readsThisFrame)
                {
                    // (the read-backs' copies made visible to the CPU once the frame's fence has passed)
                    VkMemoryBarrier toHost{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                    toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toHost, 0, nullptr, 0, nullptr);
                }
                g.recording = false;
                if (!ok(vkEndCommandBuffer(c), "vkEndCommandBuffer"))
                    return false;
                const bool uploads = g.upBegun[s];
                if (uploads && !ok(vkEndCommandBuffer(g.upCmd[s]), "vkEndCommandBuffer (uploads)"))
                    return false;
                g.upBegun[s] = false;
                // b1: the picture's constants - the first picture's (the extra moment pictures, TF_POSES: milestone 4), its
                // camera's turn written into them just before the submit (PictureOut::beforeSubmit).
                if (!end.pictures.empty())
                {
                    const gfx::PictureOut &po = end.pictures[0];
                    uint8_t *b1 = g.ring.mapped + g.ringBase[3] + static_cast<uint64_t>(s) * kPictureBytes;
                    std::memset(b1, 0, kPictureBytes);
                    std::memcpy(b1, po.constants.data(), std::min<size_t>(po.constants.size(), kPictureBytes));
                    if (po.beforeSubmit)
                        po.beforeSubmit(b1, g.frameIndex, false);
                    static bool told = false;
                    if (end.pictures.size() > 1u && !told)
                    {
                        told = true;
                        std::fprintf(stderr, "[vulkan] extra moment pictures (TF_POSES) are not drawn by Vulkan yet: one a frame\n");
                    }
                }
                const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                const VkCommandBuffer lists[2] = {g.upCmd[s], c};
                VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                si.commandBufferCount = uploads ? 2u : 1u;
                si.pCommandBuffers = uploads ? lists : &c;
                if (acquired)
                {
                    si.waitSemaphoreCount = 1u;
                    si.pWaitSemaphores = &g.acquired[s];
                    si.pWaitDstStageMask = &waitStage;
                    si.signalSemaphoreCount = 1u;
                    si.pSignalSemaphores = &g.presentReady[imageIndex];
                }
                if (!ok(vkQueueSubmit(g.queue, 1, &si, g.fence[s]), "vkQueueSubmit"))
                    return false;
                g.inFlight[s] = true;
                ++g.frameIndex;
                if (acquired)
                {
                    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
                    pi.waitSemaphoreCount = 1u;
                    pi.pWaitSemaphores = &g.presentReady[imageIndex];
                    pi.swapchainCount = 1u;
                    pi.pSwapchains = &g.swapchain;
                    pi.pImageIndices = &imageIndex;
                    const VkResult r = vkQueuePresentKHR(g.queue, &pi);
                    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
                        g.swapStale = true;
                    else if (r != VK_SUCCESS)
                        ok(r, "vkQueuePresentKHR");
                    ++g.presented;
                }
                else
                    ++g.notPresented;
                if (end.waitForGpu || shotRead)
                    finishSlot(s);
                if (shotRead)
                {
                    gfx::Readback rb;
                    if (readback(shotRead, rb))
                        writeBmp(shot, rb);
                    releaseReadback(shotRead);
                }
                savePipelineCache(); // (when new pipelines have settled)
                const double seconds = std::chrono::duration<double>(Clock::now() - g.since).count();
                if (seconds >= 10.0)
                {
                    std::fprintf(stderr, "[vulkan] %.1f pictures a second shown in its window (%llu not shown) over %.0f s; last frame %llu draws, "
                                         "%llu descriptor sets, %zu pipelines; slots read as white %llu, targets moved to be read %llu\n",
                                 static_cast<double>(g.presented) / seconds, static_cast<unsigned long long>(g.notPresented), seconds,
                                 static_cast<unsigned long long>(g.drawsThisFrame), static_cast<unsigned long long>(g.setsThisFrame), g.pipelines.size(),
                                 static_cast<unsigned long long>(g.staleSlots), static_cast<unsigned long long>(g.movedForRead));
                    g.presented = g.notPresented = 0;
                    g.since = Clock::now();
                }
                return true;
            }

            void setFrameDone(std::function<void(uint32_t slot)> done) override { g.frameDone = std::move(done); }

            void report(gfx::Report what) override
            {
                if (what == gfx::Report::Rings)
                    std::fprintf(stderr, "[vulkan] rings this frame: constants %.1f MB, instances %llu, vertices %.1f MB\n",
                                 static_cast<double>(g.ringAt[0]) / 1048576.0, static_cast<unsigned long long>(g.ringAt[1]),
                                 static_cast<double>(g.ringAt[2]) / 1048576.0);
                else
                    std::fprintf(stderr, "[vulkan] textures %u (%llu uploads, %.1f MB in all), %zu pipelines, video memory %llu MB in use of %llu MB\n",
                                 g.texturesLive, static_cast<unsigned long long>(g.uploads), static_cast<double>(g.uploadsBytes) / 1048576.0,
                                 g.pipelines.size(), static_cast<unsigned long long>(videoMemory(false) >> 20),
                                 static_cast<unsigned long long>(videoMemory(true) >> 20));
            }

            uint32_t vendorId() override { return g.props.vendorID; }

            uint64_t videoMemory(bool budget) override
            {
                if (!g.phys)
                    return 0u;
                uint64_t used = 0, all = 0;
                if (g.memoryBudget)
                {
                    VkPhysicalDeviceMemoryBudgetPropertiesEXT mb{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
                    VkPhysicalDeviceMemoryProperties2 mp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
                    mp.pNext = &mb;
                    vkGetPhysicalDeviceMemoryProperties2(g.phys, &mp);
                    for (uint32_t i = 0; i < mp.memoryProperties.memoryHeapCount; ++i)
                        if (mp.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                            used += mb.heapUsage[i], all += mb.heapBudget[i];
                }
                else
                    for (uint32_t i = 0; i < g.mem.memoryHeapCount; ++i)
                        if (g.mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                            all += g.mem.memoryHeaps[i].size;
                return budget ? all : used;
            }

            // --- targets ---
            gfx::TargetId createTarget(const gfx::TargetDesc &d) override
            {
                gfx::TargetId id;
                if (!g.freeTargets.empty())
                {
                    id = g.freeTargets.back();
                    g.freeTargets.pop_back();
                }
                else
                {
                    id = static_cast<gfx::TargetId>(g.targets.size());
                    g.targets.emplace_back();
                }
                Target &t = g.targets[id];
                t = Target{};
                t.live = true;
                if (d.shared)
                {
                    // The picture: the device's own surfaces, made in prepareFrame at the picture's size.
                    t.picture = true;
                    t.withDepth = true;
                    return id;
                }
                t.format = d.colour;
                t.withDepth = d.depth;
                if (!makeColour(t.colour, d.width, d.height, d.colour) || (d.depth && !makeDepth(t.depth, d.width, d.height)))
                {
                    destroyImage(t.colour);
                    destroyImage(t.depth);
                    t = Target{};
                    g.freeTargets.push_back(id);
                    return 0;
                }
                if (g.recording)
                {
                    zero(t.colour);
                    zero(t.depth);
                }
                else
                    g.zeroLater.push_back(id);
                return id;
            }

            bool resizeTarget(gfx::TargetId id, const gfx::TargetDesc &d) override
            {
                Target *t = targetOf(id);
                if (!t || t->picture)
                    return t != nullptr;
                if (id == g.target)
                    endRendering();
                // (The frames recorded so far may still use the old surfaces: let go of after this slot's frame.)
                retire(t->colour, g.slot);
                retire(t->depth, g.slot);
                retire(t->snap, g.slot);
                t->format = d.colour;
                t->withDepth = d.depth;
                if (!makeColour(t->colour, d.width, d.height, d.colour) || (d.depth && !makeDepth(t->depth, d.width, d.height)))
                    return false;
                if (g.recording)
                {
                    zero(t->colour);
                    zero(t->depth);
                }
                else
                    g.zeroLater.push_back(id);
                return true;
            }

            void releaseTarget(gfx::TargetId id) override
            {
                Target *t = targetOf(id);
                if (!t)
                    return;
                if (id == g.target)
                {
                    endRendering();
                    g.target = 0;
                }
                if (!t->picture)
                {
                    retire(t->colour, g.slot);
                    retire(t->depth, g.slot);
                }
                retire(t->snap, g.slot);
                *t = Target{};
                g.freeTargets.push_back(id);
            }

            // (Inside the target's own rendering a clear of its attachment, as D3D12's views are cleared; else a transfer
            // clear outside rendering.)
            void clearColour(gfx::TargetId id, const float rgba[4]) override
            {
                Target *t = targetOf(id);
                if (!t || !g.recording)
                    return;
                Image &im = colourOf(*t);
                if (!im.image)
                    return;
                VkClearColorValue v{};
                std::memcpy(v.float32, rgba, 4u * sizeof(float));
                if (g.rendering && id == g.target)
                {
                    const VkClearAttachment a{VK_IMAGE_ASPECT_COLOR_BIT, 0u, VkClearValue{v}};
                    const VkClearRect r{{{0, 0}, {im.w, im.h}}, 0u, 1u};
                    vkCmdClearAttachments(g.cmd[g.slot], 1, &a, 1, &r);
                    return;
                }
                endRendering();
                moveTo(im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
                vkCmdClearColorImage(g.cmd[g.slot], im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range);
            }

            void clearDepth(gfx::TargetId id, float value) override
            {
                Target *t = targetOf(id);
                if (!t || !g.recording)
                    return;
                Image &im = depthOf(*t);
                if (!im.image) // (a target made without depth: none to clear)
                    return;
                if (g.rendering && id == g.target && g.withDepth && im.layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
                {
                    VkClearValue cv{};
                    cv.depthStencil = {value, 0u};
                    const VkClearAttachment a{VK_IMAGE_ASPECT_DEPTH_BIT, 0u, cv};
                    const VkClearRect r{{{0, 0}, {im.w, im.h}}, 0u, 1u};
                    vkCmdClearAttachments(g.cmd[g.slot], 1, &a, 1, &r);
                    return;
                }
                endRendering();
                moveTo(im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                const VkClearDepthStencilValue v{value, 0u};
                const VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT, 0u, 1u, 0u, 1u};
                vkCmdClearDepthStencilImage(g.cmd[g.slot], im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range);
            }

            void use(gfx::TargetId id, gfx::UseFor how) override
            {
                Target *t = targetOf(id);
                if (!t)
                    return;
                static constexpr VkImageLayout kLayouts[] = {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL};
                moveTo(colourOf(*t), kLayouts[static_cast<int>(how)]);
            }

            void useDepth(gfx::TargetId id, gfx::DepthUse how) override
            {
                Target *t = targetOf(id);
                if (!t)
                    return;
                moveTo(depthOf(*t), how == gfx::DepthUse::Write ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                                                 : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
            }

            // (A target's view as a TextureId: its handle with kViewBit, kDepthViewBit for its depth, kSnapBit for its copy.)
            // (A depth view only of a target made with depth, not of the picture: 0 otherwise, as D3D12's - the frame builder
            // draws the depth fog only where the target's depth view is not 0.)
            gfx::TextureId view(gfx::TargetId id, bool depth) override
            {
                Target *t = targetOf(id);
                if (!t || (depth && (t->picture || !t->depth.image)))
                    return 0u;
                return id | kViewBit | (depth ? kDepthViewBit : 0u);
            }

            // TEST's DATE: the target's colour as it is now, copied for the draw to read (a target cannot be read while drawn
            // into) - one copy a target, as D3D12's destCopyOf; the target left in the layout it was in.
            gfx::TextureId snapshot(gfx::TargetId id) override
            {
                Target *t = targetOf(id);
                if (!t || !g.recording)
                    return 0u;
                Image &c = colourOf(*t);
                if (!c.image)
                    return 0u;
                if (!t->snap.image || t->snap.w != c.w || t->snap.h != c.h || t->snap.format != c.format)
                {
                    retire(t->snap, g.slot);
                    if (!makeImage(t->snap, c.w, c.h, c.format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
                        return 0u;
                }
                const VkImageLayout was = c.layout;
                endRendering();
                moveTo(c, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                moveTo(t->snap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkImageCopy region{};
                region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
                region.dstSubresource = region.srcSubresource;
                region.extent = {c.w, c.h, 1u};
                vkCmdCopyImage(g.cmd[g.slot], c.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->snap.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                ++frame::g_cntCopies;
                moveTo(t->snap, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                if (was != VK_IMAGE_LAYOUT_UNDEFINED)
                    moveTo(c, was);
                return id | kViewBit | kSnapBit;
            }

            // --- textures (0: the white one) ---
            gfx::TextureId createTexture(uint32_t width, uint32_t height, gfx::Format f, uint32_t levels) override
            {
                if (g.texturesLive >= kMaxTextures)
                    return 0;
                gfx::TextureId id;
                if (!g.freeTextures.empty())
                {
                    id = g.freeTextures.back();
                    g.freeTextures.pop_back();
                }
                else
                {
                    id = static_cast<gfx::TextureId>(g.textures.size());
                    g.textures.emplace_back();
                }
                TextureInfo &t = g.textures[id];
                const uint32_t generation = t.generation + 1u;
                t = TextureInfo{};
                t.live = true;
                t.generation = generation;
                t.w = width;
                t.h = height;
                t.levels = std::max(1u, levels);
                t.format = f;
                ++g.texturesLive;
                return id;
            }

            // A level's texels (RGBA8 rows of rowBytes; this frame's upload list). The texture is made once its last level is
            // given: the levels' texels must stay where they are until then (as D3D12's).
            bool uploadTexture(gfx::TextureId id, uint32_t level, const void *texels, uint32_t rowBytes) override
            {
                if (!id || id >= g.textures.size() || !g.textures[id].live)
                    return false;
                TextureInfo &t = g.textures[id];
                if (t.ready || t.format != gfx::Format::RGBA8 || level >= t.levels || rowBytes != std::max(1u, t.w >> level) * 4u)
                    return false;
                if (!g.canDraw)
                {
                    t.ready = level + 1u >= t.levels; // (nothing drawn: counted only)
                    return true;
                }
                if (t.given.size() < t.levels)
                    t.given.resize(t.levels, nullptr);
                t.given[level] = texels;
                for (const void *p : t.given)
                    if (!p)
                        return true; // (more levels to come)
                std::vector<Level> levels;
                for (uint32_t l = 0; l < t.levels; ++l)
                    levels.push_back({t.given[l], static_cast<size_t>(std::max(1u, t.w >> l)) * std::max(1u, t.h >> l) * 4u});
                std::vector<const void *>().swap(t.given);
                if (!uploadLevels(t.image, t.w, t.h, gfx::Format::RGBA8, levels))
                    return false;
                t.ready = true;
                return true;
            }

            bool uploadTextureLater(gfx::TextureId id, std::weak_ptr<const Texture> source, bool urgent) override
            {
                if (!id || id >= g.textures.size() || !g.textures[id].live || source.expired())
                    return false;
                if (!g.canDraw)
                {
                    g.textures[id].ready = true; // (nothing drawn: counted only)
                    return true;
                }
                Later item{id, g.textures[id].generation, std::move(source)};
                if (urgent)
                    g.later.push_front(std::move(item));
                else
                    g.later.push_back(std::move(item));
                return true;
            }

            bool textureReady(gfx::TextureId id) override { return id == 0 || (id < g.textures.size() && g.textures[id].ready); }

            void releaseTexture(gfx::TextureId id) override
            {
                if (!id || id >= g.textures.size() || !g.textures[id].live)
                    return;
                TextureInfo &t = g.textures[id];
                retire(t.image, g.slot);
                const uint32_t generation = t.generation + 1u; // (an upload still queued finds another generation)
                t = TextureInfo{};
                t.generation = generation;
                g.freeTextures.push_back(id);
                --g.texturesLive;
            }

            uint32_t textureSlotsFree() override { return kMaxTextures - g.texturesLive; }

            uint64_t textureBytes(gfx::TextureId id) override
            {
                if (!id || id >= g.textures.size() || !g.textures[id].live)
                    return 0u;
                const TextureInfo &t = g.textures[id];
                if (t.image.bytes)
                    return t.image.bytes;
                const uint64_t top = t.format == gfx::Format::BC7 ? uint64_t(t.w) * t.h : uint64_t(t.w) * t.h * 4u;
                return t.levels > 1u ? top * 4u / 3u : top;
            }

            // --- meshes (in memory the CPU writes and the GPU reads over the bus, as D3D12's upload heap) ---
            uint32_t createMesh(const void *vertices, uint32_t vertexBytes, const void *indices, uint32_t indexBytes) override
            {
                uint32_t id;
                if (!g.freeMeshes.empty())
                {
                    id = g.freeMeshes.back();
                    g.freeMeshes.pop_back();
                }
                else
                {
                    id = static_cast<uint32_t>(g.meshes.size());
                    g.meshes.emplace_back();
                }
                MeshInfo &m = g.meshes[id];
                m = MeshInfo{};
                m.live = true;
                m.vertexBytes = vertexBytes;
                if (g.canDraw)
                {
                    if (!makeBuffer(m.mem, static_cast<VkDeviceSize>(vertexBytes) + indexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                    false))
                    {
                        m = MeshInfo{};
                        g.freeMeshes.push_back(id);
                        return 0;
                    }
                    std::memcpy(m.mem.mapped, vertices, vertexBytes);
                    std::memcpy(m.mem.mapped + vertexBytes, indices, indexBytes);
                }
                return id;
            }

            void releaseMesh(uint32_t id) override
            {
                if (!id || id >= g.meshes.size() || !g.meshes[id].live)
                    return;
                retire(g.meshes[id].mem, g.slot);
                g.meshes[id] = MeshInfo{};
                g.freeMeshes.push_back(id);
            }

            // --- rings (a slot's part of each; RingSpan::gpu the byte offset in the ring buffer) ---
            bool ringAlloc(gfx::RingKind kind, uint32_t size, uint32_t align, gfx::RingSpan &out) override
            {
                align = std::max(align, 1u);
                const int k = static_cast<int>(kind);
                if (kind == gfx::RingKind::Constants)
                    align = std::max<uint32_t>(align, static_cast<uint32_t>(g.props.limits.minUniformBufferOffsetAlignment));
                const uint64_t base = g.ringBase[k] + static_cast<uint64_t>(g.slot) * kRingBytes[k];
                if (!g.ring.mapped)
                    return false;
                if (kind == gfx::RingKind::Instances)
                {
                    const uint64_t entries = (size + frame::kInstanceStride - 1u) / frame::kInstanceStride;
                    if (g.ringAt[k] + entries > kInstanceEntries)
                        return false;
                    out.cpu = g.ring.mapped + base + g.ringAt[k] * frame::kInstanceStride;
                    out.gpu = base + g.ringAt[k] * frame::kInstanceStride;
                    out.size = static_cast<uint32_t>(entries * frame::kInstanceStride);
                    out.first = static_cast<uint32_t>(g.slot * kInstanceEntries + g.ringAt[k]); // (the instance stream's entry)
                    g.ringAt[k] += entries;
                    return true;
                }
                const uint64_t at = (g.ringAt[k] + align - 1u) / align * align;
                if (at + size + 16u > kRingBytes[k])
                    return false;
                out.cpu = g.ring.mapped + base + at;
                out.gpu = base + at;
                out.size = size;
                out.first = 0;
                g.ringAt[k] = at + size;
                g.ringPeak[k] = std::max(g.ringPeak[k], g.ringAt[k]);
                return true;
            }

            // --- state (recorded as given; the target's rendering begun, the pipeline, bindings and streams set at the draw) ---
            void setTarget(gfx::TargetId id, bool withDepth) override
            {
                if (g.rendering && (id != g.target || withDepth != g.withDepth))
                    endRendering();
                g.target = targetOf(id) ? id : 0u;
                g.withDepth = withDepth;
            }

            void setViewport(float x, float y, float w, float h) override
            {
                if (!g.recording)
                    return;
                g.viewportEmpty = !(w > 0.0f) || h == 0.0f;
                if (g.viewportEmpty)
                    return;
                // (Y flipped by a negative height: Direct3D's clip space as the shaders write it, its top at y)
                const VkViewport vp{x, y + h, w, -h, 0.0f, 1.0f};
                vkCmdSetViewport(g.cmd[g.slot], 0, 1, &vp);
            }

            void setScissor(const gfx::Rect &r) override
            {
                if (!g.recording)
                    return;
                const int32_t x0 = std::max(0, r.x0), y0 = std::max(0, r.y0);
                const VkRect2D s{{x0, y0}, {static_cast<uint32_t>(std::max(0, r.x1 - x0)), static_cast<uint32_t>(std::max(0, r.y1 - y0))}};
                vkCmdSetScissor(g.cmd[g.slot], 0, 1, &s);
            }

            bool setPipeline(const gfx::PipelineDesc &p) override
            {
                if (!g.canDraw || !effectMade(p.effect))
                    return false;
                g.desc = p;
                g.haveDesc = true;
                if (p.effect == gfx::Effect::SmaaWeights)
                {
                    // (Its own tables: SMAA's area and search textures in t1, t2 - the frame builder keeps those slots.)
                    g.tex[1] = kSmaaAreaId;
                    g.tex[2] = kSmaaSearchId;
                }
                // (made now for the bound target's format, so that a pipeline that cannot be made leaves the draw out)
                if (Target *t = targetOf(g.target))
                {
                    const bool depth = g.withDepth && depthOf(*t).image;
                    if (!pipelineFor(p, formatOf(*t), depth))
                    {
                        g.haveDesc = false;
                        return false;
                    }
                }
                return true;
            }

            bool effect(gfx::Effect e) override { return g.canDraw && effectMade(e); }

            void setBlendFactor(const float rgba[4]) override
            {
                if (g.recording)
                    vkCmdSetBlendConstants(g.cmd[g.slot], rgba);
            }

            void bindTextures(gfx::TextureId t0, gfx::TextureId t1, gfx::TextureId t2) override
            {
                g.tex[0] = t0;
                if (t1 != gfx::kKeepTexture)
                    g.tex[1] = t1;
                if (t2 != gfx::kKeepTexture)
                    g.tex[2] = t2;
            }

            void bindDrawConstants(const gfx::RingSpan &c) override { g.b0 = static_cast<uint32_t>(c.gpu); }

            // --- geometry: stream 0 (a mesh, or ring vertices), the instance stream (1, the Instances ring), the frame
            // before's vertices (2), the indices - set on the list at the next draw ---
            void setVertices(const gfx::RingSpan &v, uint32_t stride) override
            {
                g.vb = Stream{g.ring.buffer, v.gpu, stride};
                g.streamsDirty = true;
            }
            void setIndices(const gfx::RingSpan &i) override
            {
                g.ib = Stream{g.ring.buffer, i.gpu, 4u};
                g.indexDirty = true;
            }
            void setPreviousVertices(const gfx::RingSpan &v) override
            {
                // (none: a zero stride's one entry at the ring's start - read by nothing, pose.y off)
                g.vbB = v.size ? Stream{g.ring.buffer, v.gpu, kVertexBStride}
                               : Stream{g.ring.buffer, g.ringBase[2] + static_cast<uint64_t>(g.slot) * kRingBytes[2], 0u};
                g.streamsDirty = true;
            }
            void setMesh(uint32_t meshId) override
            {
                if (!meshId || meshId >= g.meshes.size() || !g.meshes[meshId].live || !g.meshes[meshId].mem.buffer)
                    return;
                const MeshInfo &m = g.meshes[meshId];
                g.vb = Stream{m.mem.buffer, 0u, sizeof(Vertex)};
                g.ib = Stream{m.mem.buffer, m.vertexBytes, 4u};
                g.streamsDirty = g.indexDirty = true;
            }

            void draw(uint32_t vertices, uint32_t first, uint32_t instances, uint32_t firstInstance) override
            {
                if (flush(false))
                {
                    vkCmdDraw(g.cmd[g.slot], vertices, instances, first, firstInstance);
                    ++g.drawsThisFrame;
                }
            }
            void drawIndexed(uint32_t indices, uint32_t first, uint32_t instances, uint32_t firstInstance) override
            {
                if (flush(true))
                {
                    vkCmdDrawIndexed(g.cmd[g.slot], indices, instances, first, 0, firstInstance);
                    ++g.drawsThisFrame;
                }
            }
            void drawIndirect(uint64_t, uint32_t) override {}  // (the GPU grass: milestone 4 - grassGpuReady() is false)
            void growGrass(const Frame &) override {}
            bool grassMesh(uint64_t) override { return false; }

            // --- copies ---
            void copyWhole(gfx::TargetId dst, gfx::TargetId src) override
            {
                Target *d = targetOf(dst), *s = targetOf(src);
                if (!d || !s || !g.recording)
                    return;
                Image &di = colourOf(*d), &si = colourOf(*s);
                if (!di.image || !si.image || di.w != si.w || di.h != si.h)
                    return;
                moveTo(si, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                moveTo(di, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                endRendering();
                VkImageCopy region{};
                region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
                region.dstSubresource = region.srcSubresource;
                region.extent = {di.w, di.h, 1u};
                vkCmdCopyImage(g.cmd[g.slot], si.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, di.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                ++frame::g_cntCopies;
            }

            // --- time stamps ---
            bool timestamps() override { return g.stamps != VK_NULL_HANDLE; }
            gfx::QueryId timestamp() override
            {
                const uint32_t s = g.slot;
                if (!g.stamps || !g.recording || g.stampsUsed[s] >= gfx::kTimestampsPerFrame)
                    return 0;
                vkCmdWriteTimestamp(g.cmd[s], VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.stamps, s * gfx::kTimestampsPerFrame + g.stampsUsed[s]);
                return ++g.stampsUsed[s];
            }
            bool readTimestamps(uint32_t slot, std::vector<uint64_t> &ticks, uint64_t &ticksPerSecond) override
            {
                if (!g.stamps || slot >= kSlots || !g.stampsUsed[slot])
                    return false;
                const uint32_t n = g.stampsUsed[slot];
                ticks.resize(n);
                const VkResult r = vkGetQueryPoolResults(g.device, g.stamps, slot * gfx::kTimestampsPerFrame, n, n * sizeof(uint64_t), ticks.data(),
                                                         sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
                if (r != VK_SUCCESS)
                    return false;
                if (g.timestampBits < 64u)
                    for (uint64_t &t : ticks)
                        t &= (1ull << g.timestampBits) - 1ull;
                ticksPerSecond = static_cast<uint64_t>(std::llround(1.0e9 / std::max(1.0e-6, static_cast<double>(g.props.limits.timestampPeriod))));
                return true;
            }
            bool pipelineStats() override { return false; }
            void beginStats(uint32_t) override {}
            void endStats(uint32_t) override {}
            bool readStats(uint32_t, std::vector<gfx::PipelineStats> &) override { return false; }
            bool predication() override { return false; }
            void predicate(bool) override {}

            // --- read-backs ---
            gfx::ReadbackId readTarget(gfx::TargetId id) override
            {
                Target *t = targetOf(id);
                if (!t || !g.recording)
                    return 0;
                Image &im = colourOf(*t);
                if (!im.image)
                    return 0;
                const gfx::Format format = formatOf(*t);
                const uint32_t rowBytes = im.w * texelBytes(format);
                const VkDeviceSize size = static_cast<VkDeviceSize>(rowBytes) * im.h;
                const uint32_t s = g.slot;
                gfx::ReadbackId r = 0;
                // (one of the slot's own, its last frame done, big enough)
                std::vector<gfx::ReadbackId> &pool = g.readbackFree[s];
                for (size_t i = 0; i < pool.size(); ++i)
                    if (g.readbacks[pool[i]].mem.size >= size)
                    {
                        r = pool[i];
                        pool.erase(pool.begin() + static_cast<std::ptrdiff_t>(i));
                        break;
                    }
                if (!r)
                {
                    ReadbackBuffer b;
                    if (!makeBuffer(b.mem, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true))
                        return 0;
                    r = static_cast<gfx::ReadbackId>(g.readbacks.size());
                    g.readbacks.push_back(b);
                }
                ReadbackBuffer &rb = g.readbacks[r];
                rb.w = im.w;
                rb.h = im.h;
                rb.rowBytes = rowBytes;
                rb.format = format;
                rb.slot = s;
                rb.live = true;
                const VkImageLayout was = im.layout;
                moveTo(im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                endRendering();
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
                region.imageExtent = {im.w, im.h, 1u};
                vkCmdCopyImageToBuffer(g.cmd[s], im.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.mem.buffer, 1, &region);
                g.readsThisFrame = true;
                if (was != VK_IMAGE_LAYOUT_UNDEFINED) // (the target keeps its state: docs 4.5)
                    moveTo(im, was);
                return r;
            }

            bool readback(gfx::ReadbackId id, gfx::Readback &out) override
            {
                if (!id || id >= g.readbacks.size() || !g.readbacks[id].live)
                    return false;
                const ReadbackBuffer &rb = g.readbacks[id];
                vmaInvalidateAllocation(g.vma, rb.mem.alloc, 0, VK_WHOLE_SIZE); // (nothing to do on coherent memory)
                out.data = rb.mem.mapped;
                out.width = rb.w;
                out.height = rb.h;
                out.rowBytes = rb.rowBytes;
                out.format = rb.format;
                return true;
            }

            void releaseReadback(gfx::ReadbackId id) override
            {
                if (!id || id >= g.readbacks.size() || !g.readbacks[id].live)
                    return;
                ReadbackBuffer &rb = g.readbacks[id];
                rb.live = false;
                g.readbackReleased[rb.slot].push_back(id);
            }

        private:
            // A texture slot's view and the layout it is read in; a target's image moved to a layout a shader can read.
            // Nothing there: the white texture. The bound target's own colour or depth in a layout a shader cannot read is
            // a slot left over from an earlier pass (bindTextures keeps t1, t2): Direct3D 12 leaves such a view bound and the
            // shader does not read it; here the slot reads white, and the attachment keeps its layout - moving the depth to
            // read-only for it dropped the pass's depth writes.
            Bound resolve(gfx::TextureId id, const Image *attachedColour, const Image *attachedDepth)
            {
                const Bound white{g.white.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, nullptr};
                if (id == kSmaaAreaId || id == kSmaaSearchId)
                {
                    const Image &lookup = id == kSmaaAreaId ? g.smaaArea : g.smaaSearch;
                    return lookup.view && g.smaaReady ? Bound{lookup.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, nullptr} : white;
                }
                if (id & kViewBit)
                {
                    Target *t = targetOf(id & kIdMask);
                    if (!t)
                        return white;
                    Image *im = (id & kSnapBit) ? &t->snap : (id & kDepthViewBit) ? &depthOf(*t) : &colourOf(*t);
                    if (!im->image || !im->view)
                        return white;
                    const bool depth = (im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
                    const bool readable = im->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || im->layout == VK_IMAGE_LAYOUT_GENERAL ||
                                          (depth && im->layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
                    if (!readable)
                    {
                        if (im == attachedColour || im == attachedDepth)
                        {
                            ++g.staleSlots;
                            return white;
                        }
                        moveTo(*im, depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        ++g.movedForRead;
                    }
                    return {im->view, im->layout, im};
                }
                if (id && id < g.textures.size() && g.textures[id].live && g.textures[id].ready && g.textures[id].image.view)
                    return {g.textures[id].image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, nullptr};
                return white;
            }

            // Everything a draw needs set on the list: the textures readable, the target's rendering begun, the pipeline,
            // the descriptor set (a new one when a texture or its layout changed; b0's offset as its dynamic offset), the
            // streams. False: the draw is left out.
            bool flush(bool indexed)
            {
                if (!g.canDraw || !g.recording || !g.haveDesc || g.viewportEmpty)
                    return false;
                Target *t = targetOf(g.target);
                if (!t)
                    return false;
                Image &col = colourOf(*t);
                Image *dep = g.withDepth && depthOf(*t).image ? &depthOf(*t) : nullptr;
                if (!col.image || !col.view)
                    return false;
                const VkPipeline pipeline = pipelineFor(g.desc, formatOf(*t), dep != nullptr);
                if (!pipeline)
                    return false;
                Bound b[3];
                for (int k = 0; k < 3; ++k)
                    b[k] = resolve(g.tex[k], &col, dep);
                const VkCommandBuffer c = g.cmd[g.slot];
                if (!g.rendering)
                {
                    moveTo(col, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                    if (dep && dep->layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL)
                        moveTo(*dep, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
                    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                    ca.imageView = col.view;
                    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                    if (dep)
                    {
                        // (read-only while the shader samples it - the depth fog's pass - as D3D12's read-only view)
                        const bool readOnly = dep->layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                        da.imageView = dep->view;
                        da.imageLayout = dep->layout;
                        da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                        da.storeOp = readOnly ? VK_ATTACHMENT_STORE_OP_NONE : VK_ATTACHMENT_STORE_OP_STORE;
                    }
                    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
                    ri.renderArea = {{0, 0}, {col.w, col.h}};
                    ri.layerCount = 1u;
                    ri.colorAttachmentCount = 1u;
                    ri.pColorAttachments = &ca;
                    ri.pDepthAttachment = dep ? &da : nullptr;
                    vkCmdBeginRendering(c, &ri);
                    g.rendering = true;
                }
                if (pipeline != g.boundPipeline)
                {
                    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                    g.boundPipeline = pipeline;
                    g.boundHasStreams = g.desc.effect == gfx::Effect::None || g.desc.effect == gfx::Effect::Cloud;
                    g.streamsDirty = true; // (the strides are set again with the streams)
                }
                bool rebind = false;
                VkImageView views[3];
                VkImageLayout layouts[3];
                for (int k = 0; k < 3; ++k)
                {
                    views[k] = b[k].view;
                    layouts[k] = b[k].image ? b[k].image->layout : b[k].layout;
                }
                if (!g.boundSet || std::memcmp(views, g.boundViews, sizeof(views)) != 0 || std::memcmp(layouts, g.boundLayouts, sizeof(layouts)) != 0)
                {
                    const VkDescriptorSet set = allocSet();
                    if (!set)
                        return false;
                    const VkDescriptorBufferInfo b0{g.ring.buffer, 0u, kB0Range}, b1{g.ring.buffer, 0u, kPictureBytes};
                    VkDescriptorImageInfo images[3];
                    VkWriteDescriptorSet w[5]{};
                    for (int k = 0; k < 5; ++k)
                    {
                        w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                        w[k].dstSet = set;
                        w[k].descriptorCount = 1u;
                    }
                    w[0].dstBinding = 0u;
                    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                    w[0].pBufferInfo = &b0;
                    w[1].dstBinding = 1u;
                    w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                    w[1].pBufferInfo = &b1;
                    for (int k = 0; k < 3; ++k)
                    {
                        images[k] = {VK_NULL_HANDLE, views[k], layouts[k]};
                        w[2 + k].dstBinding = 10u + static_cast<uint32_t>(k);
                        w[2 + k].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                        w[2 + k].pImageInfo = &images[k];
                    }
                    vkUpdateDescriptorSets(g.device, 5u, w, 0u, nullptr);
                    g.boundSet = set;
                    std::memcpy(g.boundViews, views, sizeof(views));
                    std::memcpy(g.boundLayouts, layouts, sizeof(layouts));
                    ++g.setsThisFrame;
                    rebind = true;
                }
                if (rebind || g.boundB0 != g.b0)
                {
                    const uint32_t offsets[2] = {g.b0, static_cast<uint32_t>(g.ringBase[3] + static_cast<uint64_t>(g.slot) * kPictureBytes)};
                    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0u, 1u, &g.boundSet, 2u, offsets);
                    g.boundB0 = g.b0;
                }
                if (g.streamsDirty && g.boundHasStreams)
                {
                    const VkBuffer buffers[3] = {g.vb.buffer ? g.vb.buffer : g.ring.buffer, g.ring.buffer, g.vbB.buffer ? g.vbB.buffer : g.ring.buffer};
                    const VkDeviceSize offsets[3] = {g.vb.buffer ? g.vb.offset : 0u, g.ringBase[1], g.vbB.offset};
                    const VkDeviceSize strides[3] = {g.vb.buffer ? g.vb.stride : sizeof(Vertex), frame::kInstanceStride, g.vbB.stride};
                    vkCmdBindVertexBuffers2(c, 0u, 3u, buffers, offsets, nullptr, strides);
                    g.streamsDirty = false;
                }
                if (indexed && g.indexDirty && g.ib.buffer)
                {
                    vkCmdBindIndexBuffer(c, g.ib.buffer, g.ib.offset, VK_INDEX_TYPE_UINT32);
                    g.indexDirty = false;
                }
                return !indexed || g.ib.buffer != VK_NULL_HANDLE;
            }
        };

        VulkanDevice &vulkanDeviceImpl()
        {
            static VulkanDevice d;
            return d;
        }

        // ---- the backend's outer calls (nr_backend.h) -------------------------------------------------------------------
        class VulkanBackend final : public Backend
        {
        public:
            void prepare() override
            {
                int expected = 0;
                if (!g_starting.compare_exchange_strong(expected, 1))
                    return;
                std::thread([] {
                    const auto t0 = Clock::now();
                    const bool up = vulkan::start();
                    std::fprintf(stderr, "[native render] Vulkan %s in %.0f ms (on its own thread)\n", up ? "made ready" : "not started",
                                 std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
                    g_starting.store(2, std::memory_order_release);
                }).detach();
            }
            bool start() override { return vulkan::start(); }
            bool startingNow() override { return g_starting.load(std::memory_order_acquire) == 1; }
            // (Vulkan draws the game in place of the emulated GS only with TF_VK_ALONE=1, a test switch for now: else false,
            // and the emulated GS goes on drawing the game in its window while the Vulkan window shows Vulkan's picture.)
            bool drawsPictures() override
            {
                return alone() && g_starting.load(std::memory_order_acquire) == 2 && g.ready && g.canDraw && !g_lost.load(std::memory_order_relaxed);
            }
            bool deviceLost() override { return g_lost.load(std::memory_order_relaxed); }
            bool grassGpuReady() override { return false; }

            double waitForFrameSlot() override
            {
                if (!g.ready || g_lost.load(std::memory_order_relaxed))
                    return 0.0;
                const uint32_t next = static_cast<uint32_t>(g.frameIndex % kSlots);
                if (!g.inFlight[next])
                    return 0.0;
                const auto t0 = Clock::now();
                vkWaitForFences(g.device, 1, &g.fence[next], VK_TRUE, 10ull * 1000000000ull);
                return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            }

            void render(const Frame &input, const std::string &shotPath) override
            {
                if (g_starting.load(std::memory_order_acquire) == 1 || g_lost.load(std::memory_order_relaxed))
                    return;
                if (!vulkan::start())
                    return;
                frame::render(input, shotPath);
            }

            void preUpload(const std::shared_ptr<const Texture> &texture) override { frame::preUpload(texture); }

            // TF_VK_ALONE: the picture as CPU pixels (RGBA8, R in the low byte) - the main window's CPU hand-over.
#if defined(__linux__)
            // The pixels stay valid and unchanged until pictureShown, newer publications going into the other buffer.
            bool latestPicture(const uint8_t *&pixels, uint32_t &rowPitch, int &index, uint64_t &serial, d3d::SharedPicture &meta) override
            {
                return g_pub.take(pixels, rowPitch, index, serial, meta);
            }
            void pictureShown(int index) override { g_pub.shown(index); }
#else
            bool latestPicture(std::vector<uint32_t> &pixels, uint32_t &width, uint32_t &height, uint64_t &serial, bool &has3D) override
            {
                std::lock_guard<std::mutex> lock(g_pub.mutex);
                if (g_pub.serial == serial || g_pub.pixels.empty())
                    return false;
                pixels.swap(g_pub.pixels);
                width = g_pub.w;
                height = g_pub.h;
                has3D = g_pub.has3D;
                serial = g_pub.serial;
                return true;
            }
#endif
            void setSharedPictures(bool on) override
            {
                static bool told = false;
                if (on && alone() && !told)
                {
                    told = true;
                    std::fprintf(stderr, "[vulkan] TF_VK_ALONE: the main window takes Vulkan's pictures only as CPU pixels - set TF_NATIVE_SHARE=0\n");
                }
            }
            bool latestShared(d3d::SharedPicture &, uint64_t &) override { return false; }
            void sharedHeld(int) override {}
#if defined(__linux__)
            bool waitPicture(uint64_t serial, int64_t untilNs, int64_t &lastNs) override { return g_pub.wait(serial, untilNs, lastNs); }
#else
            bool waitPicture(uint64_t serial, int64_t untilNs, int64_t &lastNs) override
            {
                std::unique_lock<std::mutex> lock(g_pub.mutex);
                const auto until = Clock::time_point(std::chrono::nanoseconds(untilNs));
                g_pub.cv.wait_until(lock, until, [serial] { return g_pub.serial != serial; });
                lastNs = g_pub.lastNs;
                return g_pub.serial != serial;
            }
#endif
            int pacingMode() override { return 0; }

#if defined(__linux__)
            void setWindowSize(int width, int height) override
            {
                frame::g_windowWidth.store(width, std::memory_order_relaxed);
                frame::g_windowLines.store(height, std::memory_order_relaxed);
            }
#else
            void setWindowLines(int lines) override { frame::g_windowLines.store(lines, std::memory_order_relaxed); }
#endif
            void setAntiAliasing(int mode) override { frame::g_aa.store(std::clamp(mode, 0, 2), std::memory_order_relaxed); }
            void setSupersampling(bool on) override { frame::g_ssaa.store(on ? 1 : 0, std::memory_order_relaxed); }
            void setSpeedupsForced(int on) override { frame::g_speedupsForced.store(on, std::memory_order_relaxed); }

            uint32_t largeImageCap() override
            {
                const char *e = std::getenv("TF_HIRES_MAX");
                if (e && *e)
                    return static_cast<uint32_t>(std::max(0, std::atoi(e)));
                const uint64_t mb = memoryMB();
                static const uint32_t kPresetCap[4] = {1024u, 2048u, 2048u, 0u};
                const uint32_t byMemory = mb == 0u ? 0u : mb < 3000u ? 1024u : mb < 5000u ? 2048u : 0u, byPreset = kPresetCap[qualityPreset()];
                return !byMemory ? byPreset : !byPreset ? byMemory : std::min(byMemory, byPreset);
            }
            uint64_t largeImageBudget() override
            {
                if (const char *e = std::getenv("TF_HIRES_BUDGET_MB"); e && *e)
                    return std::strtoull(e, nullptr, 10) << 20;
                const uint64_t mb = memoryMB();
                if (!mb)
                    return UINT64_MAX;
                return std::max<uint64_t>(384u, std::min<uint64_t>(mb > 1536u ? mb - 1536u : 0u, mb * 3u / 4u)) << 20;
            }
            uint64_t largeImageBytes() override { return frame::g_largeBytes.load(std::memory_order_relaxed); }
            uint64_t adapterMemoryMB() override { return memoryMB(); }
            int qualityPreset() override
            {
                static std::atomic<int> kept{-1};
                if (const int q = kept.load(std::memory_order_relaxed); q >= 0)
                    return q;
                const uint64_t mb = memoryMB();
                const int q = presetFor(mb);
                if (g.ready || std::getenv("TF_NATIVE_QUALITY") || std::getenv("TF_NATIVE_VRAM_MB"))
                {
                    int expected = -1;
                    if (kept.compare_exchange_strong(expected, q))
                    {
                        static const char *kNames[4] = {"Low", "Medium", "High", "Ultra"};
                        std::fprintf(stderr, "[native render] quality %s (Vulkan; %llu MB of video memory)\n", kNames[q], static_cast<unsigned long long>(mb));
                    }
                }
                return q;
            }

            double gpuPictureMs() override { return g_gpuPictureMs.load(std::memory_order_relaxed); }
            double gpuExtraMs() override { return 0.0; }
            double cpuPictureMs() override { return 0.0; }
            double replayWaitMs() override { return 0.0; }
            double slotWaitMs() override { return g_slotWaitAll.load(std::memory_order_relaxed); }
            double pacerDelayMs() override { return 0.0; }
        };
    }

    Backend &vulkanBackend()
    {
        static VulkanBackend b;
        return b;
    }

    gfx::Device &vulkanDevice() { return vulkanDeviceImpl(); }
}

#endif // TF_VULKAN
