// Linux: the native renderer's Direct3D 12 is Wine's vkd3d (Direct3D 12 on Vulkan, linked statically: transformers/
// CMakeLists.txt, TF_VKD3D_DIR). This header stands in for windows.h, d3d12.h, d3dcompiler.h and dxgi1_4.h in
// nr_d3d.cpp, with the few Win32 functions the renderer calls besides.
#pragma once

// (vkd3d's COM methods that return a structure are declared the way its library was compiled)
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#ifndef NOMINMAX
#define NOMINMAX
#endif
// (vkd3d_utils.h before vkd3d_d3dcompiler.h: its C declarations of D3DCompile and the rest come first)
#include <vkd3d.h>
#include <vkd3d_utils.h>
#include <vkd3d_d3d12.h>
#include <vkd3d_d3d12sdklayers.h>
#include <vkd3d_d3dcompiler.h>

#define IID_PPV_ARGS(pointer) __uuidof(**(pointer)), reinterpret_cast<void **>(pointer)
#define GENERIC_ALL 0x10000000u

#include <dlfcn.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 258u
#define INFINITE 0xFFFFFFFFu
// (the shader cache's key: vkd3d's compiler makes other code than d3dcompiler_47)
#define D3D_COMPILER_VERSION 2001
#define D3D12_ERROR_ADAPTER_NOT_FOUND _HRESULT_TYPEDEF_(0x887E0001)
#define D3D12_ERROR_DRIVER_VERSION_MISMATCH _HRESULT_TYPEDEF_(0x887E0002)

namespace nr::win32
{
    // The renderer's events: auto-reset, signalled by vkd3d when a fence reaches its value (pfn_signal_event below).
    // vkd3d-utils' own events cannot wait with a time-out on Linux (an unsignalled one fails at once).
    struct Event
    {
        std::mutex mutex;
        std::condition_variable cv;
        bool set = false;
    };

    inline HRESULT signal(HANDLE handle)
    {
        auto *e = static_cast<Event *>(handle);
        {
            std::lock_guard<std::mutex> lock(e->mutex);
            e->set = true;
        }
        e->cv.notify_all();
        return S_OK;
    }
}

inline HANDLE CreateEventW(void *, BOOL, BOOL, const wchar_t *) { return new nr::win32::Event; }

inline unsigned WaitForSingleObject(HANDLE handle, unsigned ms)
{
    auto *e = static_cast<nr::win32::Event *>(handle);
    std::unique_lock<std::mutex> lock(e->mutex);
    const auto isSet = [e] { return e->set; };
    if (ms == INFINITE)
        e->cv.wait(lock, isSet);
    else if (!e->cv.wait_for(lock, std::chrono::milliseconds(ms), isSet))
        return WAIT_TIMEOUT;
    e->set = false;
    return WAIT_OBJECT_0;
}

inline BOOL CloseHandle(HANDLE handle)
{
    delete static_cast<nr::win32::Event *>(handle); // (only events: vkd3d makes no shared handles)
    return TRUE;
}

inline void Sleep(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
inline unsigned GetCurrentThreadId() { return static_cast<unsigned>(syscall(SYS_gettid)); }

inline FILE *_wfopen(const wchar_t *path, const wchar_t *mode)
{
    return std::fopen(std::filesystem::path(path).c_str(), std::filesystem::path(mode).c_str());
}

inline BOOL CreateDirectoryA(const char *path, void *)
{
    std::string p = path;
    std::replace(p.begin(), p.end(), '\\', '/');
    std::error_code ec;
    return std::filesystem::create_directories(p, ec) ? TRUE : FALSE;
}

// The device as vkd3d-utils' D3D12CreateDevice makes it, with the events above.
#undef D3D12CreateDevice
inline HRESULT D3D12CreateDevice(IUnknown *, D3D_FEATURE_LEVEL level, REFIID iid, void **device)
{
    // (vkd3d's errors only - its "fixme" notes, e.g. each shader's ignored optimisation flag, are not the game's; a
    // VKD3D_DEBUG / VKD3D_SHADER_DEBUG already set wins)
    setenv("VKD3D_DEBUG", "err", 0);
    setenv("VKD3D_SHADER_DEBUG", "err", 0);
    static const char *const instanceExtensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, "VK_KHR_xcb_surface", "VK_KHR_xlib_surface",
                                                     "VK_KHR_wayland_surface"};
    static const char *const deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    vkd3d_application_info application{};
    application.type = VKD3D_STRUCTURE_TYPE_APPLICATION_INFO;
    application.application_name = "TransformersRecomp";
    application.api_version = VKD3D_API_VERSION_2_1;
    vkd3d_optional_instance_extensions_info optionalInstance{};
    optionalInstance.type = VKD3D_STRUCTURE_TYPE_OPTIONAL_INSTANCE_EXTENSIONS_INFO;
    optionalInstance.next = &application;
    optionalInstance.extensions = instanceExtensions;
    optionalInstance.extension_count = sizeof(instanceExtensions) / sizeof(instanceExtensions[0]);
    vkd3d_instance_create_info instance{};
    instance.type = VKD3D_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance.next = &optionalInstance;
    instance.pfn_signal_event = nr::win32::signal;
    instance.wchar_size = sizeof(WCHAR);
    vkd3d_optional_device_extensions_info optionalDevice{};
    optionalDevice.type = VKD3D_STRUCTURE_TYPE_OPTIONAL_DEVICE_EXTENSIONS_INFO;
    optionalDevice.extensions = deviceExtensions;
    optionalDevice.extension_count = 1;
    vkd3d_device_create_info create{};
    create.type = VKD3D_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create.next = &optionalDevice;
    create.minimum_feature_level = level;
    create.instance_create_info = &instance;
    return vkd3d_create_device(&create, iid, device);
}

namespace nr::vk
{
    // The device's graphics card as Vulkan tells it (DXGI's adapter description on Windows): vendor id, the largest
    // device-local memory heap in MB (what DXVK reports as dedicated video memory), the name.
    inline bool describe(ID3D12Device *device, uint32_t &vendor, uint64_t &memoryMB, std::string &name)
    {
        if (!device)
            return false;
        static void *vulkan = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL); // (already loaded by vkd3d)
        const auto getProc = vulkan ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(vulkan, "vkGetInstanceProcAddr")) : nullptr;
        const VkInstance instance = vkd3d_instance_get_vk_instance(vkd3d_instance_from_device(device));
        const VkPhysicalDevice physical = vkd3d_get_vk_physical_device(device);
        if (!getProc || !instance || !physical)
            return false;
        const auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(getProc(instance, "vkGetPhysicalDeviceProperties"));
        const auto memory = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(getProc(instance, "vkGetPhysicalDeviceMemoryProperties"));
        if (!properties || !memory)
            return false;
        VkPhysicalDeviceProperties p{};
        properties(physical, &p);
        VkPhysicalDeviceMemoryProperties m{};
        memory(physical, &m);
        VkDeviceSize largest = 0;
        for (uint32_t i = 0; i < m.memoryHeapCount; ++i)
            if (m.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                largest = std::max(largest, m.memoryHeaps[i].size);
        vendor = p.vendorID;
        memoryMB = static_cast<uint64_t>(largest) >> 20;
        name = p.deviceName;
        return true;
    }
}
