// The graphics card's driver: which one, and whether it is known to be too old (graphics_driver.h, TODO 290).

#include "graphics_driver.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi.h>
#include <shellapi.h>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#endif

#define INTEL_DSA "https://www.intel.com/content/www/us/en/support/detect.html"

namespace graphics_driver
{
    namespace
    {
        struct Info
        {
            uint32_t vendor = 0;
            std::string name;
            uint64_t dedicatedMB = 0;             // (its own video memory; built-in graphics: a token 128 MB or so)
            uint32_t version[4] = {0, 0, 0, 0}; // (the user-mode driver's: 27.20.100.9565)
            bool known = false;                  // (the version could be read)
            std::string warning;
        };

        const Info &info()
        {
            static const Info i = [] {
                Info out;
#if defined(_WIN32)
                IDXGIFactory1 *factory = nullptr;
                if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))))
                {
                    IDXGIAdapter1 *adapter = nullptr;
                    if (SUCCEEDED(factory->EnumAdapters1(0, &adapter)))
                    {
                        DXGI_ADAPTER_DESC1 desc{};
                        if (SUCCEEDED(adapter->GetDesc1(&desc)))
                        {
                            out.vendor = desc.VendorId;
                            char name[256] = {};
                            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
                            out.name = name;
                            out.dedicatedMB = static_cast<uint64_t>(desc.DedicatedVideoMemory) >> 20;
                        }
                        LARGE_INTEGER umd{};
                        // (DXGI answers this one question with the user-mode driver's version: major.minor in the high
                        // part, build.revision in the low part)
                        if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
                        {
                            out.version[0] = static_cast<uint32_t>(umd.HighPart) >> 16;
                            out.version[1] = static_cast<uint32_t>(umd.HighPart) & 0xFFFFu;
                            out.version[2] = static_cast<uint32_t>(umd.LowPart) >> 16;
                            out.version[3] = static_cast<uint32_t>(umd.LowPart) & 0xFFFFu;
                            out.known = true;
                        }
                        adapter->Release();
                    }
                    factory->Release();
                }
#endif
                // Intel's driver numbers: 26.x / 27.x (2019-2021), 30.0.100 / 30.0.101 (2021-2022), 31.0.101 (2023 on).
                // TF_DRIVER_WARN_TEST=1: the warning as if the driver were that old (tests of the menu).
                const bool test = std::getenv("TF_DRIVER_WARN_TEST") && *std::getenv("TF_DRIVER_WARN_TEST") == '1';
                if (test || (out.vendor == 0x8086u && out.known && out.version[0] < 31u))
                {
                    char text[768];
                    std::snprintf(text, sizeof(text),
                                  "Your graphics driver is very old (%s, version %u.%u.%u.%u). Intel's drivers from before 2023 "
                                  "draw this game wrong: menus as outlines, garbled pictures and noise. Update it with Intel's "
                                  "Driver & Support Assistant (Windows' Device Manager may say the best driver is already "
                                  "installed even when a much newer one exists):\n" INTEL_DSA,
                                  out.name.empty() ? "unknown card" : out.name.c_str(), out.version[0], out.version[1], out.version[2],
                                  out.version[3]);
                    out.warning = text;
                }
                return out;
            }();
            return i;
        }
    }

    uint32_t vendor() { return info().vendor; }

    bool lowEnd()
    {
        static const bool low = [] {
            if (const char *e = std::getenv("TF_LOW_END"); e && *e)
                return *e == '1';
            const Info &i = info();
            // (Unknown card - no DXGI, another system: not low end, nothing changes.) 3.5 GB: a 4 GB card reports a little
            // under 4096 MB; built-in graphics report their token share (Iris Xe 128 MB, an AMD APU 512 MB - 2 GB).
            return i.vendor != 0u && i.dedicatedMB < 3584u;
        }();
        return low;
    }

    namespace
    {
        // (TODO 295, the user's laptop target: a steady 60, sharper than the PS2) The Low preset's values that differ from
        // the defaults (mod_menu.cpp's presets: Medium is the default) - draw distance 1x (the console's), enemy detail 1,
        // no MSAA - and the picture drawn at 67% and enlarged by FSR 1 (the ThinkPad's UHD 620 at 720p: 29 -> 40 fps). Edge
        // smoothing (FXAA) and texture quality (already Low from the card's memory) stay. While that render scale is the
        // default the picture keeps at least 480 lines (TF_NATIVE_MIN_LINES; the PS2 shows 448), so it is never softer.
        struct LowEndDefault
        {
            const char *key;
            const char *value;
        };
        constexpr LowEndDefault kLowEndDefaults[] = {{"TF_DRAW_DISTANCE", "1"}, {"TF_ENEMY_DETAIL", "1"}, {"PS2X_GPU_MSAA", "0"},
                                                   {"TF_NATIVE_RENDER_SCALE", "67"}, {"TF_NATIVE_UPSCALER", "fsr1"}};
    }

    const char *defaultFor(const char *key, const char *normal)
    {
        if (lowEnd())
            for (const LowEndDefault &d : kLowEndDefaults)
                if (std::strcmp(d.key, key) == 0)
                    return d.value;
        return normal;
    }

    void applyLowEndDefaults()
    {
        if (!lowEnd())
            return;
        std::string applied;
        for (const LowEndDefault &d : kLowEndDefaults)
        {
            const char *set = std::getenv(d.key);
            if (set && *set)
                continue; // (settings.ini or the environment: the player's)
#if defined(_WIN32)
            _putenv_s(d.key, d.value);
#else
            setenv(d.key, d.value, 1);
#endif
            applied += (applied.empty() ? "" : ", ") + std::string(d.key) + "=" + d.value;
            if (std::strcmp(d.key, "TF_NATIVE_RENDER_SCALE") == 0 && !(std::getenv("TF_NATIVE_MIN_LINES") && *std::getenv("TF_NATIVE_MIN_LINES")))
            {
#if defined(_WIN32)
                _putenv_s("TF_NATIVE_MIN_LINES", "480");
#else
                setenv("TF_NATIVE_MIN_LINES", "480", 1);
#endif
                applied += " (at least 480 lines)";
            }
        }
        if (!applied.empty())
            std::fprintf(stderr, "[graphics] built-in or low-end graphics: the Low preset's %s (not set in settings.ini)\n", applied.c_str());
    }

    void writeGpuFile(const std::string &hdDir)
    {
        const Info &i = info();
        std::string name;
        for (char c : i.name)
            if (c != '"' && c != '\\')
                name += c;
        char text[512];
        std::snprintf(text, sizeof(text), "{\"card\": \"%s\", \"vendor\": %u, \"video_memory_mb\": %llu, \"low_end\": %s}\n",
                      name.c_str(), i.vendor, static_cast<unsigned long long>(i.dedicatedMB), lowEnd() ? "true" : "false");
        const std::string path = hdDir + "/.gpu.json";
        if (FILE *f = std::fopen(path.c_str(), "wb"))
        {
            std::fputs(text, f);
            std::fclose(f);
        }
    }

    const std::string &warning() { return info().warning; }

    void logOnce()
    {
        static bool done = false;
        if (done)
            return;
        done = true;
        const Info &i = info();
        if (i.known)
            std::fprintf(stderr, "[graphics] %s, driver %u.%u.%u.%u, %llu MB of its own video memory%s\n",
                         i.name.empty() ? "unknown card" : i.name.c_str(), i.version[0], i.version[1], i.version[2], i.version[3],
                         static_cast<unsigned long long>(i.dedicatedMB),
                         lowEnd() ? ": built-in or low-end graphics, no HD textures made here" : "");
        if (!i.warning.empty())
            std::fprintf(stderr, "[graphics] warning: %s\n", i.warning.c_str());
    }

    void showOnce()
    {
#if defined(_WIN32)
        const Info &i = info();
        if (i.warning.empty())
            return;
        // (Hidden test runs never stop on a question; TF_DRIVER_WARN_BOX=0: no box.)
        const char *hidden = std::getenv("PS2X_WINDOW_HIDDEN");
        const char *box = std::getenv("TF_DRIVER_WARN_BOX");
        if ((hidden && *hidden == '1') || (box && *box == '0'))
            return;
        // Once for each driver version: the file keeps the version it was shown for.
        char version[64];
        std::snprintf(version, sizeof(version), "%u.%u.%u.%u", i.version[0], i.version[1], i.version[2], i.version[3]);
        const char *mark = "runs\\driver_warning_shown.txt";
        if (FILE *f = std::fopen(mark, "rb"))
        {
            char seen[64] = {};
            const size_t n = std::fread(seen, 1, sizeof(seen) - 1, f);
            std::fclose(f);
            seen[n] = 0;
            if (std::string(seen).find(version) != std::string::npos)
                return;
        }
        CreateDirectoryA("runs", nullptr);
        if (FILE *f = std::fopen(mark, "wb"))
        {
            std::fputs(version, f);
            std::fclose(f);
        }
        // A plain Windows message box: Windows draws it, not the graphics driver, so it reads clean even where the
        // driver breaks the game's own picture and its F1 menu (TODO 290: the title as outlines, F1 garbled).
        // (A game started with its window hidden - a launcher's or a test's STARTUPINFO SW_HIDE - would have the box hidden
        // too: Windows applies that to the process's first ShowWindow. A throwaway window takes it first; tested on the
        // T480s, where the box otherwise never showed and the game waited for it.)
        if (HWND dummy = CreateWindowExA(0, "STATIC", "", 0, 0, 0, 0, 0, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr))
        {
            ShowWindow(dummy, SW_HIDE);
            DestroyWindow(dummy);
        }
        // (The link without "https://www.": the box is narrow and splits a long word mid-way - seen on the Vostro,
        // "detect.ht" / "ml". Yes opens it anyway.)
        std::string text = i.warning;
        if (const size_t at = text.find("https://www."); at != std::string::npos)
            text.erase(at, 12);
        text += "\n\nOpen Intel's Driver & Support Assistant page now?";
        wchar_t wtext[2048] = {};
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wtext, 2047);
        if (MessageBoxW(nullptr, wtext, L"Transformers - update your graphics driver",
                        MB_YESNO | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST) == IDYES)
            ShellExecuteW(nullptr, L"open", L"" INTEL_DSA, nullptr, nullptr, SW_SHOWNORMAL);
#endif
    }
}
