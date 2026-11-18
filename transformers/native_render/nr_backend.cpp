// nr_backend.cpp - which graphics backend the native renderer uses (docs/RENDERER_BACKEND.md, section 2): chosen once at
// start by TF_GFX_API (settings.ini or the environment). d3d12 (the default): Direct3D 12, nr_d3d.cpp. vulkan: the Vulkan
// backend with its own SDL3 window, nr_vulkan.cpp (built with TF_VULKAN; in development - see that file's head for what
// it draws so far). A build with neither Direct3D 12 (nr_none.cpp stands in: builds other than Windows, no vkd3d) nor
// Vulkan has no native renderer: the emulated GS draws, and every call reports it.
#include "nr_backend.h"
#include "nr_device.h"
#include "nr_frame.h" // (nr_core S6: the core's own frame builder, the default)

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace nr
{
#if defined(TF_NATIVE_RENDERER)
    Backend &d3d12Backend(); // (nr_d3d.cpp)
    namespace gfx
    {
        Device &deviceD3d12(); // (nr_d3d.cpp)
    }
#else
    Backend &noneBackend(); // (nr_none.cpp: no native renderer)
    namespace gfx
    {
        Device &noneDevice(); // (nr_none.cpp)
    }
#endif
#if defined(TF_VULKAN)
    namespace vulkan
    {
        Backend &vulkanBackend(); // (nr_vulkan.cpp)
        gfx::Device &vulkanDevice();
    }
    namespace vk::window
    {
        bool beforeWindow(); // (nr_vulkan_window.cpp)
    }
#endif

    namespace
    {
#if defined(TF_NATIVE_RENDERER)
        constexpr const char *kDefaultApi = "Direct3D 12";
#elif defined(TF_VULKAN)
        constexpr const char *kDefaultApi = "none (the emulated GS draws: TF_GFX_API is not vulkan)";
#else
        constexpr const char *kDefaultApi = "none (the emulated GS draws: this build has no native renderer)";
#endif
        // (S2a) The API the host chose through nr_core::open (-1: none chosen - the TF_GFX_API environment variable, as before)
        std::atomic<int> g_apiChosen{-1};

        bool vulkanChosen()
        {
            static const bool chosen = [] {
                const char *v = std::getenv("TF_GFX_API");
                const int byHost = g_apiChosen.load(std::memory_order_acquire);
                const bool asked = byHost >= 0 ? byHost == 1 : v && (std::strcmp(v, "vulkan") == 0 || std::strcmp(v, "Vulkan") == 0);
#if defined(TF_VULKAN)
                std::fprintf(stderr, "[native render] graphics API: %s\n", asked ? "Vulkan (TF_GFX_API=vulkan; in development)" : kDefaultApi);
                return asked;
#else
                if (asked)
                    std::fprintf(stderr, "[native render] TF_GFX_API=vulkan: this build has no Vulkan backend (TF_VULKAN off) - %s\n", kDefaultApi);
                return false;
#endif
            }();
            return chosen;
        }
    }

    // (S2a, nr_core::open) The host's API: 0 the default, 1 Vulkan. Only before the backend's first use - the choice is made once.
    void chooseApi(int api)
    {
        g_apiChosen.store(api, std::memory_order_release);
    }

    Backend &backend()
    {
#if defined(TF_VULKAN)
        if (vulkanChosen())
            return vulkan::vulkanBackend();
#else
        (void)vulkanChosen();
#endif
#if defined(TF_NATIVE_RENDERER)
        return d3d12Backend();
#else
        return noneBackend();
#endif
    }

    bool nativeBackendChosen()
    {
#if defined(TF_VULKAN)
        if (vulkanChosen())
            return true;
#endif
#if defined(TF_NATIVE_RENDERER)
        return true;
#else
        return false;
#endif
    }
}

namespace nr
{
    void beforeWindow()
    {
#if defined(TF_VULKAN)
        // (Milestone 3) Vulkan presents SDL3's window itself: the window is made a Vulkan window.
        if (vulkanChosen())
            vk::window::beforeWindow();
#endif
    }
}

namespace nr::gfx
{
    Device &device()
    {
#if defined(TF_VULKAN)
        if (vulkanChosen())
            return vulkan::vulkanDevice();
#endif
#if defined(TF_NATIVE_RENDERER)
        return deviceD3d12();
#else
        return noneDevice();
#endif
    }
}

namespace nr
{
    namespace
    {
        // (nr_core S5; S6: the core's own frame builder, nr_frame.cpp, unless the host sets another)
        FrameBuilderHost g_frameBuilderHost{&frame::render, &frame::preUpload};
    }
    void setFrameBuilderHost(const FrameBuilderHost &host) { g_frameBuilderHost = host; }
    const FrameBuilderHost &frameBuilderHost() { return g_frameBuilderHost; }

    namespace
    {
        std::atomic<int> g_livePictures{-1}; // (nr_core S11: Settings::livePictures; -1 = TF_NATIVE_LIVE)
    }
    void setLivePictures(int live) { g_livePictures.store(live < 0 ? -1 : (live != 0 ? 1 : 0), std::memory_order_release); }
    int livePictures()
    {
        const int set = g_livePictures.load(std::memory_order_acquire);
        if (set >= 0)
            return set;
        const char *v = std::getenv("TF_NATIVE_LIVE");
        return v && *v == '1' ? 1 : 0;
    }
}
