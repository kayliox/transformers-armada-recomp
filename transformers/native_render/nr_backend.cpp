// nr_backend.cpp - which graphics backend the native renderer uses (docs/RENDERER_BACKEND.md, section 2): chosen once at
// start by TF_GFX_API (settings.ini or the environment). d3d12 (the default): Direct3D 12, nr_d3d.cpp. vulkan: the Vulkan
// backend with its own SDL3 window, nr_vulkan.cpp (built with TF_VULKAN; in development - see that file's head for what
// it draws so far). A build with neither Direct3D 12 (nr_none.cpp stands in: builds other than Windows, no vkd3d) nor
// Vulkan has no native renderer: the emulated GS draws, and every call reports it.
#include "nr_backend.h"
#include "nr_device.h"

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
        bool vulkanChosen()
        {
            static const bool chosen = [] {
                const char *v = std::getenv("TF_GFX_API");
                const bool asked = v && (std::strcmp(v, "vulkan") == 0 || std::strcmp(v, "Vulkan") == 0);
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
