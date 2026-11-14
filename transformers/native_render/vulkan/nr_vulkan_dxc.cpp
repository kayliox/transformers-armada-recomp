// nr_vulkan_dxc.cpp - nr_vulkan_dxc.h: DXC's library loaded by the build's SPIR-V bake from the DXC release CMake
// fetched (no Vulkan SDK needed, nothing links it, and the game never loads it). Its own file: the compiler's header
// needs the Windows headers.
#include "nr_vulkan_dxc.h"
#include "nr_vulkan_shaders.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // (whole: DXC's interfaces are COM's - IUnknown, BSTR, IStream)
#include <dxcapi.h>
#else
// (Linux: the same release's libdxcompiler.so; its header brings the COM types itself - WinAdapter.h)
#include <dlfcn.h>
#include <dxcapi.h>
#endif

#include <cstring>

namespace nr::vulkan::dxc
{
    namespace
    {
        IDxcUtils *g_utils = nullptr;
        IDxcCompiler3 *g_compiler = nullptr;

        std::wstring wide(const std::string &s) { return std::wstring(s.begin(), s.end()); } // (ASCII: names and numbers)
    }

    bool load(const char *library, std::string &why)
    {
        if (g_compiler)
            return true;
#if defined(_WIN32)
        // (that file only, never another dxcompiler.dll found on the PATH; its own dependencies from its folder or System32)
        const std::wstring path = wide(library);
        HMODULE dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dll)
        {
            why = std::string("cannot load ") + library;
            return false;
        }
        auto create = reinterpret_cast<DxcCreateInstanceProc>(reinterpret_cast<void *>(GetProcAddress(dll, "DxcCreateInstance")));
#else
        // (that file only, by its full path: never a libdxcompiler.so found on the system's library path)
        void *so = dlopen(library, RTLD_NOW | RTLD_LOCAL);
        if (!so)
        {
            const char *e = dlerror();
            why = std::string("cannot load ") + library + (e ? std::string(": ") + e : std::string());
            return false;
        }
        auto create = reinterpret_cast<DxcCreateInstanceProc>(dlsym(so, "DxcCreateInstance"));
#endif
        if (!create)
        {
            why = std::string(library) + " has no DxcCreateInstance";
            return false;
        }
        if (FAILED(create(CLSID_DxcUtils, IID_PPV_ARGS(&g_utils))) || FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&g_compiler))))
        {
            why = std::string(library) + " did not make its compiler";
            g_compiler = nullptr;
            return false;
        }
        return true;
    }

    bool compile(const char *source, size_t size, const char *entry, const char *profile,
                 const std::vector<std::pair<std::string, std::string>> &defines, std::vector<uint32_t> &spirv, std::string &errors)
    {
        spirv.clear();
        errors.clear();
        if (!g_compiler)
        {
            errors = "the compiler is not loaded";
            return false;
        }
        std::vector<std::wstring> args;
        for (const char *a : shaders::kCompilerArgs)
            args.push_back(wide(a));
        for (const std::string &a : {std::string("-E"), std::string(entry), std::string("-T"), std::string(profile)})
            args.push_back(wide(a));
        for (const auto &[name, value] : defines)
        {
            args.push_back(L"-D");
            args.push_back(wide(name + "=" + value));
        }
        std::vector<LPCWSTR> argv;
        for (const std::wstring &a : args)
            argv.push_back(a.c_str());
        DxcBuffer buffer{source, size, DXC_CP_UTF8};
        IDxcResult *result = nullptr;
        if (FAILED(g_compiler->Compile(&buffer, argv.data(), static_cast<UINT32>(argv.size()), nullptr, IID_PPV_ARGS(&result))) || !result)
        {
            errors = "the compiler failed";
            return false;
        }
        HRESULT status = E_FAIL;
        result->GetStatus(&status);
        IDxcBlobUtf8 *messages = nullptr;
        if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&messages), nullptr)) && messages && messages->GetStringLength())
            errors.assign(messages->GetStringPointer(), messages->GetStringLength());
        if (messages)
            messages->Release();
        IDxcBlob *object = nullptr;
        if (SUCCEEDED(status) && SUCCEEDED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) && object &&
            object->GetBufferSize() >= 20u && object->GetBufferSize() % 4u == 0u)
        {
            spirv.resize(object->GetBufferSize() / 4u);
            std::memcpy(spirv.data(), object->GetBufferPointer(), object->GetBufferSize());
        }
        if (object)
            object->Release();
        result->Release();
        return !spirv.empty();
    }
}
