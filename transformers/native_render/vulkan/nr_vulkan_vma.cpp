// nr_vulkan_vma.cpp - the Vulkan Memory Allocator's implementation (its header, fetched by CMake), for the Vulkan backend's
// images and buffers. Its functions come through volk (VMA_DYNAMIC_VULKAN_FUNCTIONS, set by CMake).
#if defined(TF_VULKAN)

#include <volk.h> // (first: the Vulkan headers without prototypes)

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#endif // TF_VULKAN
