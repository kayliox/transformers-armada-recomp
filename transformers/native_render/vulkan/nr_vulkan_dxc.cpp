// nr_vulkan_dxc.cpp - nr_vulkan_dxc.h: DXC's library loaded at run time (dxcompiler.dll beside the game: CMake copies it
// there from the DXC release it fetches), so building needs no Vulkan SDK and nothing links it. Its own file: the
// compiler's header needs the Windows headers, which the rest of the backend keeps out.
#if defined(TF_VULKAN)

#include "nr_vulkan_dxc.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // (whole: DXC's interfaces are COM's - IUnknown, BSTR, IStream)
#include <dxcapi.h>
#endif

#include <cstring>
#include <mutex>

namespace nr::vulkan::dxc
{
#if defined(_WIN32)
    namespace
    {
        std::mutex g_mutex;
        bool g_tried = false;
        std::string g_why;
        IDxcUtils *g_utils = nullptr;
        IDxcCompiler3 *g_compiler = nullptr;

        bool load()
        {
            if (g_tried)
                return g_compiler != nullptr;
            g_tried = true;
            // (only the game's own folder: the DLL that came with it, never another one found on the PATH; its own
            // dependencies from that folder or System32)
            wchar_t path[MAX_PATH] = {};
            HMODULE dll = nullptr;
            if (const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH); n > 0 && n < MAX_PATH)
            {
                std::wstring dir(path, n);
                dir.resize(dir.find_last_of(L"\\/") + 1u);
                dll = LoadLibraryExW((dir + L"dxcompiler.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            }
            if (!dll)
            {
                g_why = "dxcompiler.dll not found beside the game";
                return false;
            }
            auto create = reinterpret_cast<DxcCreateInstanceProc>(reinterpret_cast<void *>(GetProcAddress(dll, "DxcCreateInstance")));
            if (!create)
            {
                g_why = "dxcompiler.dll has no DxcCreateInstance";
                return false;
            }
            if (FAILED(create(CLSID_DxcUtils, IID_PPV_ARGS(&g_utils))) || FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&g_compiler))))
            {
                g_why = "dxcompiler.dll did not make its compiler";
                g_compiler = nullptr;
                return false;
            }
            return true;
        }

        std::wstring wide(const std::string &s) { return std::wstring(s.begin(), s.end()); } // (ASCII: names and numbers)
    }

    bool available(std::string &why)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const bool ok = load();
        why = g_why;
        return ok;
    }

    bool compile(const char *source, size_t size, const char *entry, const char *profile,
                 const std::vector<std::pair<std::string, std::string>> &defines, std::vector<uint32_t> &spirv, std::string &errors)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        spirv.clear();
        errors.clear();
        if (!load())
        {
            errors = g_why;
            return false;
        }
        // (The Direct3D 12 root signature's registers as one descriptor set: b0, b1 at 0, 1; t0-t2 at 10-12; s0-s5 at
        // 20-25. Direct3D's constant buffer layout, so the C++ structs stay as they are. Y is flipped by the viewport.)
        // (-HV 2018: the language as D3DCompile reads it - HLSL 2021's ternary on vectors refused the tone mapping's.
        // ddx / ddy as the coarse derivatives: D3DCompile's ddx is ddx_coarse, SPIR-V's plain derivative leaves the choice
        // to the driver - the texture levels the shader picks itself (sampleGs) came out otherwise in fine detail.)
        // Vulkan 1.3: `discard` becomes OpDemoteToHelperInvocation - the pixel stays in its 2 x 2 quad as a helper, as
        // Direct3D's discard does, so its neighbours' derivatives (the texture's level) stay right; 1.1's OpKill ended it,
        // and the alpha-tested foliage's neighbours took other levels.)
        std::vector<std::wstring> args = {L"-spirv", L"-HV", L"2018", L"-D", L"ddx=ddx_coarse", L"-D", L"ddy=ddy_coarse",
                                          L"-fspv-target-env=vulkan1.3", L"-fvk-use-dx-layout",
                                          L"-fvk-b-shift", L"0", L"0", L"-fvk-t-shift", L"10", L"0",
                                          L"-fvk-s-shift", L"20", L"0", L"-fvk-u-shift", L"30", L"0",
                                          L"-O3", L"-E", wide(entry), L"-T", wide(profile)};
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
#else
    bool available(std::string &why)
    {
        why = "no shader compiler on this system yet";
        return false;
    }
    bool compile(const char *, size_t, const char *, const char *, const std::vector<std::pair<std::string, std::string>> &,
                 std::vector<uint32_t> &spirv, std::string &errors)
    {
        spirv.clear();
        errors = "no shader compiler on this system yet";
        return false;
    }
#endif
}

#endif // TF_VULKAN
