// nr_core S9c: TF_NATIVE_SWAPCHAIN=1 - a developer switch (default off; not in settings.ini or the F1 menu) that shows the
// native pictures through the core's own swap chain (nr_swapchain.cpp, nr_core::present) instead of the GL window's drawing.
// The recomp's window already presents through a DXGI swap chain of the runtime's (ps2_host_present: GL blitted into it), and a
// window takes one flip-model swap chain, so the core presents into a CHILD window over the game window's client area: no
// frame, transparent to the mouse (WM_NCHITTEST HTTRANSPARENT: clicks and the cursor go to the game window), never activated,
// following the client area's size; the game window keeps its input, its message loop and its own present (drawn as before
// beneath, never seen while the child shows). The child shows only while the window's picture is one of the renderer's shared
// pictures; otherwise (the start's black, a movie read from GS memory, the CPU hand-over, the emulated drawing) it hides and the
// GL window's own picture shows. The game's GL overlays are drawn in the game window, under the child: not
// shown while it is up - a stated limitation of the switch. Glue (the recomp's): raylib-free, Windows only.
// TF_NATIVE_SWAPCHAIN_SHOT=<tick>,<tick>,...: the first picture of a tick at or after each is saved twice - the core's back buffer
// (runs/swap_<tick>.bmp) and, by the caller, the GL window's drawing of the same picture (runs/swapgl_<tick>.png) - the A/B.
// nr_core S12: the window's ImGui overlays (the F1 menu, the debugger) drawn into the core's picture too, when nr_overlay_host.cpp
// sets an overlay source (the debug UI built in): the picture is then kept here and presented by flush() once the window's
// overlays of that picture are drawn (main.cpp's debug-UI callback, nr::overlaysDrawn), with the overlays the ImGui frames since
// made - the same picture's menu, as the GL window beneath shows it. nr_core S13a: the other GL overlays (raylib's text and shapes:
// the frame counter, the notices, the settings panel, the small crosshair, the distance meter, the objective arrow, the REC mark,
// HD movies) too - drawn into layers the core draws between the same ImGui frames (nr_overlay_host.cpp), in the GL window's order.
#include "nr_devswap.h"
#include "nr_core.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace nr::devswap
{
    namespace
    {
        HWND g_child = nullptr, g_parent = nullptr;
        bool g_failed = false, g_shown = false;
        uint64_t g_presents = 0;

        // (S12) The overlays' source, and the picture kept for flush().
        OverlaySource g_source = nullptr;
        struct Kept
        {
            bool on = false;
            HWND parent = nullptr;
            int picture = -1;
            float src[4] = {}, dst[4] = {};
            int upscaler = 0;
            float sharpness = 0.2f;
            int sync = 0;
            std::string shot;
            uint64_t shotTick = 0;
        };
        Kept g_kept;
        std::string g_shotNext;
        unsigned g_missedFlushes = 0;

        LRESULT CALLBACK childProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
        {
            switch (message)
            {
            case WM_NCHITTEST:
                return HTTRANSPARENT; // (the mouse is the game window's)
            case WM_MOUSEACTIVATE:
                return MA_NOACTIVATE;
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT:
                ValidateRect(hwnd, nullptr);
                return 0;
            default:
                return DefWindowProcW(hwnd, message, wParam, lParam);
            }
        }

        bool child(HWND parent)
        {
            if (g_child && g_parent == parent && IsWindow(g_child))
                return true;
            if (g_failed)
                return false;
            g_failed = true;
            WNDCLASSEXW wc = {};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = childProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"nr_core_devswap";
            if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
                return false;
            RECT r = {};
            GetClientRect(parent, &r);
            g_child = CreateWindowExW(WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, r.right - r.left, r.bottom - r.top,
                                      parent, nullptr, wc.hInstance, nullptr);
            if (!g_child)
            {
                std::fprintf(stderr, "[native render] TF_NATIVE_SWAPCHAIN: the child window was not made (%lu)\n", GetLastError());
                return false;
            }
            nr_core::WindowSpec spec;
            spec.handle = g_child;
            if (!nr_core::open(nr_core::Settings{}, spec))
            {
                std::fprintf(stderr, "[native render] TF_NATIVE_SWAPCHAIN: the core did not take the child window\n");
                DestroyWindow(g_child);
                g_child = nullptr;
                return false;
            }
            g_parent = parent;
            g_failed = false;
            std::fprintf(stderr, "[native render] TF_NATIVE_SWAPCHAIN: the pictures shown through the core's swap chain in a child window "
                                 "(%ldx%ld); the GL window's overlays drawn into it with the debug UI built in (else not shown while it is up)\n",
                         r.right - r.left, r.bottom - r.top);
            return true;
        }

        std::vector<uint64_t> &shotTicks()
        {
            static std::vector<uint64_t> ticks = [] {
                std::vector<uint64_t> list;
                for (const char *s = std::getenv("TF_NATIVE_SWAPCHAIN_SHOT"); s && *s;)
                {
                    char *end = nullptr;
                    list.push_back(std::strtoull(s, &end, 10));
                    s = end && *end == ',' ? end + 1 : nullptr;
                }
                return list;
            }();
            return ticks;
        }
    }

    bool on()
    {
        static const bool value = [] {
            const char *v = std::getenv("TF_NATIVE_SWAPCHAIN");
            return v && *v == '1';
        }();
        return value;
    }

    uint64_t shotDue(uint64_t pictureTick)
    {
        std::vector<uint64_t> &t = shotTicks();
        if (t.empty() || pictureTick < t.front())
            return 0u;
        const uint64_t due = t.front();
        while (!t.empty() && pictureTick >= t.front())
            t.erase(t.begin());
        return due;
    }

    namespace
    {
        bool presentNow(HWND parent, int picture, const float src[4], const float dst[4], int upscaler, float sharpness, int sync,
                        const char *shot, nr_core::Overlay overlay)
        {
            if (!parent || !child(parent))
                return false;
            RECT pr = {}, cr = {};
            GetClientRect(parent, &pr);
            GetClientRect(g_child, &cr);
            if (cr.right != pr.right || cr.bottom != pr.bottom)
                SetWindowPos(g_child, HWND_TOP, 0, 0, pr.right, pr.bottom, SWP_NOACTIVATE);
            if (picture < 0)
            {
                if (g_shown)
                {
                    ShowWindow(g_child, SW_HIDE);
                    g_shown = false;
                }
                return false;
            }
            nr_core::PresentRequest rq;
            rq.picture = picture;
            std::memcpy(rq.src, src, sizeof(rq.src));
            std::memcpy(rq.dst, dst, sizeof(rq.dst));
            rq.upscaler = upscaler;
            rq.sharpness = sharpness;
            rq.syncInterval = sync;
            // (S12, tests: TF_MENU_SHOT's twin - the next present saved there, in place of a tick's shot)
            if (!g_shotNext.empty())
            {
                if (shot && *shot)
                    std::fprintf(stderr, "[native render] TF_NATIVE_SWAPCHAIN: %s not saved (the menu shot's twin takes this present)\n", shot);
                rq.shot = g_shotNext.c_str();
            }
            else
                rq.shot = shot;
            rq.overlay = std::move(overlay);
            const bool ok = nr_core::present(rq);
            if (rq.shot == g_shotNext.c_str())
                g_shotNext.clear();
            if (ok && !g_shown)
            {
                SetWindowPos(g_child, HWND_TOP, 0, 0, pr.right, pr.bottom, SWP_NOACTIVATE | SWP_SHOWWINDOW);
                g_shown = true;
            }
            else if (!ok && g_shown)
            {
                ShowWindow(g_child, SW_HIDE);
                g_shown = false;
            }
            if (!ok && ++g_presents <= 5u)
                std::fprintf(stderr, "[native render] TF_NATIVE_SWAPCHAIN: picture %d not presented - the GL window's own shows\n", picture);
            return ok;
        }
    }

    bool show(void *parentHandle, int picture, const float src[4], const float dst[4], int upscaler, float sharpness, int sync, const char *shot)
    {
        const HWND parent = static_cast<HWND>(parentHandle);
        if (!g_source || picture < 0)
        {
            // (no overlays through the core: at once, as before S12; no picture: the child hidden at once)
            if (g_kept.on) // (a kept one never flushed: shown first)
            {
                g_kept.on = false;
                presentNow(g_kept.parent, g_kept.picture, g_kept.src, g_kept.dst, g_kept.upscaler, g_kept.sharpness, g_kept.sync,
                           g_kept.shot.empty() ? nullptr : g_kept.shot.c_str(), g_source ? g_source() : nr_core::Overlay{});
            }
            return presentNow(parent, picture, src, dst, upscaler, sharpness, sync, shot, {});
        }
        if (g_kept.on)
        {
            // (the window's overlays never ended that picture - no debug UI callback: shown now, one picture late)
            if (++g_missedFlushes <= 3u)
                std::fprintf(stderr, "[native render] TF_NATIVE_SWAPCHAIN: a picture kept for the overlays was not flushed - shown at the next\n");
            flush();
        }
        if (!parent || !child(parent))
            return false;
        g_kept.on = true;
        g_kept.parent = parent;
        g_kept.picture = picture;
        std::memcpy(g_kept.src, src, sizeof(g_kept.src));
        std::memcpy(g_kept.dst, dst, sizeof(g_kept.dst));
        g_kept.upscaler = upscaler;
        g_kept.sharpness = sharpness;
        g_kept.sync = sync;
        g_kept.shot = shot ? shot : "";
        g_kept.shotTick = 0u;
        if (shot && *shot)
        {
            // (runs/swap_<tick>.bmp: the tick for the GL twin read before flush)
            const char *u = std::strrchr(shot, '_');
            g_kept.shotTick = u ? std::strtoull(u + 1, nullptr, 10) : 0u;
        }
        return true;
    }

    void setOverlaySource(OverlaySource source)
    {
        g_source = source;
        std::fprintf(stderr, "[native render] TF_NATIVE_SWAPCHAIN: the window's ImGui overlays (F1 menu, debugger) drawn into the core's "
                             "picture %s\n", source ? "too - each picture presented once its overlays are drawn" : "no more");
    }

    void flush()
    {
        nr_core::Overlay overlay = g_source ? g_source() : nr_core::Overlay{};
        if (!g_kept.on)
            return; // (no picture kept: the frames dropped with `overlay`)
        g_kept.on = false;
        presentNow(g_kept.parent, g_kept.picture, g_kept.src, g_kept.dst, g_kept.upscaler, g_kept.sharpness, g_kept.sync,
                   g_kept.shot.empty() ? nullptr : g_kept.shot.c_str(), std::move(overlay));
    }

    uint64_t keptShotTick() { return g_kept.on ? g_kept.shotTick : 0u; }

    void shotNext(const char *path) { g_shotNext = path ? path : ""; }
}

#else

namespace nr::devswap
{
    bool on() { return false; }
    uint64_t shotDue(uint64_t) { return 0u; }
    bool show(void *, int, const float *, const float *, int, float, int, const char *) { return false; }
    void setOverlaySource(OverlaySource) {}
    void flush() {}
    uint64_t keptShotTick() { return 0u; }
    void shotNext(const char *) {}
}

#endif
