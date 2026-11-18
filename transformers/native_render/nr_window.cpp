// nr_core S9a: the core's window (nr_core.h WindowSpec; NR_CORE_SPEC.md 2 and 8). Either a window of the core's own (a plain
// Win32 window: its class, title, size, frame; its messages pumped by the host's call nr_core::pumpEvents on the thread that
// opened it) or a window the host made and passes in (its HWND - the host keeps its own message loop; the core watches the
// window's messages through two message hooks on the window's thread, WH_CALLWNDPROC for the sent ones and WH_GETMESSAGE for
// the posted ones, set by a thread of the core's own that keeps them until closeWindow, and changes nothing about how the
// messages are handled). Either way the window's size, focus, close request, DPI
// and raw keyboard / mouse input go out through nr_core::WindowEvents (S12: and every message as it comes, for a host that reads
// the window's input itself - ImGui's Win32 input); the core reads no input itself. No swap chain yet (S9b):
// the pictures still leave as before. Not used by the recomp's player build: nothing calls open() with a window.
#include "nr_core.h"
#include "nr_window.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <future>
#include <memory>
#include <string>
#include <thread>

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

namespace nr_core
{
    namespace
    {
        constexpr wchar_t kClass[] = L"nr_core_window";

        std::atomic<HWND> g_window{nullptr};
        std::atomic<bool> g_own{false};        // the core made the window (else: the host's, watched)
        std::atomic<bool> g_closeAsked{false}; // the window was asked to close (or is gone)
        std::atomic<bool> g_destroyAsked{false}; // closeWindow from another thread: its own thread destroys it at its next pump
        DWORD g_thread = 0;                    // the window's thread
        HHOOK g_hookSent = nullptr, g_hookPosted = nullptr; // (a host's window: set and taken down by g_watcher)
        std::thread g_watcher;
        std::atomic<DWORD> g_watcherThread{0};
        std::atomic<uint32_t> g_rawRegistered{0}; // bit 0 keyboard, bit 1 mouse

        int dpiOf(HWND hwnd)
        {
            using GetDpiForWindowFn = UINT(WINAPI *)(HWND);
            static const GetDpiForWindowFn getDpi =
                reinterpret_cast<GetDpiForWindowFn>(reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
            return getDpi ? static_cast<int>(getDpi(hwnd)) : 96;
        }

        void readRawInput(LPARAM lParam)
        {
            alignas(8) unsigned char buffer[sizeof(RAWINPUT) + 64];
            UINT size = sizeof(buffer);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1))
                return;
            window_detail::forwardRawInput(buffer);
        }

        // One of the window's messages seen (before the window's own handling): forwarded through WindowEvents. True: the
        // host's `message` callback (S12) said it handled it (only the core's own window acts on that).
        bool observe(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
        {
            const WindowEvents &ev = windowEvents();
            const bool handled = ev.message && ev.message(hwnd, static_cast<uint32_t>(message), static_cast<uint64_t>(wParam),
                                                           static_cast<int64_t>(lParam));
            switch (message)
            {
            case WM_SIZE:
                if (ev.resized)
                    ev.resized(static_cast<int>(LOWORD(lParam)), static_cast<int>(HIWORD(lParam)));
                break;
            case WM_SETFOCUS:
                if (ev.focus)
                    ev.focus(true);
                break;
            case WM_KILLFOCUS:
                if (ev.focus)
                    ev.focus(false);
                break;
            case WM_CLOSE:
                g_closeAsked.store(true);
                if (ev.closed)
                    ev.closed();
                break;
            case WM_DPICHANGED:
                if (ev.dpiChanged)
                    ev.dpiChanged(static_cast<int>(LOWORD(wParam)));
                break;
            case WM_INPUT:
                readRawInput(lParam);
                break;
            default:
                break;
            }
            return handled;
        }

        // The core's own window.
        LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
        {
            if (hwnd == g_window.load() && observe(hwnd, message, wParam, lParam) && message != WM_CLOSE && message != WM_DESTROY)
                return 1; // (S12: the host's message callback handled it, e.g. WM_SETCURSOR)
            switch (message)
            {
            case WM_CLOSE:
                return 0; // (the host decides: closeWindow)
            case WM_DPICHANGED:
            {
                const RECT *r = reinterpret_cast<const RECT *>(lParam); // (the size Windows suggests at the new scale)
                SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
            case WM_ERASEBKGND:
                return 1; // (the pictures paint it: no flash of the class background)
            case WM_DESTROY:
                if (hwnd == g_window.load())
                    g_closeAsked.store(true);
                return 0;
            default:
                break;
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }

        // A host's window: its thread's sent messages (WH_CALLWNDPROC) and the posted ones it takes (WH_GETMESSAGE, PM_REMOVE
        // only - a peek that leaves the message is seen again when it is taken). Observation only: the window handles them as before.
        LRESULT CALLBACK sentHook(int code, WPARAM wParam, LPARAM lParam)
        {
            if (code == HC_ACTION)
            {
                const CWPSTRUCT *m = reinterpret_cast<const CWPSTRUCT *>(lParam);
                if (m && m->hwnd == g_window.load())
                    observe(m->hwnd, m->message, m->wParam, m->lParam);
            }
            return CallNextHookEx(nullptr, code, wParam, lParam);
        }
        LRESULT CALLBACK postedHook(int code, WPARAM wParam, LPARAM lParam)
        {
            if (code == HC_ACTION && wParam == PM_REMOVE)
            {
                const MSG *m = reinterpret_cast<const MSG *>(lParam);
                if (m && m->hwnd == g_window.load())
                    observe(m->hwnd, m->message, m->wParam, m->lParam);
            }
            return CallNextHookEx(nullptr, code, wParam, lParam);
        }

        bool registerRawInput(HWND hwnd)
        {
            RAWINPUTDEVICE d[2] = {};
            d[0].usUsagePage = 0x01, d[0].usUsage = 0x06, d[0].hwndTarget = hwnd; // keyboard (the legacy key messages kept)
            d[1].usUsagePage = 0x01, d[1].usUsage = 0x02, d[1].hwndTarget = hwnd; // mouse
            if (!RegisterRawInputDevices(d, 2, sizeof(RAWINPUTDEVICE)))
            {
                std::fprintf(stderr, "[nr_core] window: raw input not registered (error %lu)\n", GetLastError());
                return false;
            }
            g_rawRegistered.store(3u);
            return true;
        }
        void unregisterRawInput()
        {
            if (!g_rawRegistered.exchange(0u))
                return;
            RAWINPUTDEVICE d[2] = {};
            d[0].usUsagePage = 0x01, d[0].usUsage = 0x06, d[0].dwFlags = RIDEV_REMOVE;
            d[1].usUsagePage = 0x01, d[1].usUsage = 0x02, d[1].dwFlags = RIDEV_REMOVE;
            RegisterRawInputDevices(d, 2, sizeof(RAWINPUTDEVICE));
        }

        std::wstring wide(const char *s)
        {
            if (!s || !*s)
                return L"";
            const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
            std::wstring w(n > 0 ? static_cast<size_t>(n) : 1u, L'\0');
            if (n > 0)
                MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
            w.resize(w.size() - 1u);
            return w;
        }

        bool makeOwnWindow(const WindowSpec &spec)
        {
            const HINSTANCE inst = GetModuleHandleW(nullptr);
            static const bool registered = [inst]
            {
                WNDCLASSEXW wc = {};
                wc.cbSize = sizeof(wc);
                wc.style = CS_HREDRAW | CS_VREDRAW;
                wc.lpfnWndProc = windowProc;
                wc.hInstance = inst;
                wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // (IDC_ARROW)
                wc.lpszClassName = kClass;
                return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
            }();
            if (!registered)
            {
                std::fprintf(stderr, "[nr_core] window: its class was not registered (error %lu)\n", GetLastError());
                return false;
            }
            const DWORD style = spec.borderless ? WS_POPUP : WS_OVERLAPPEDWINDOW;
            RECT r = {0, 0, spec.width > 0 ? spec.width : 1280, spec.height > 0 ? spec.height : 720};
            AdjustWindowRectEx(&r, style, FALSE, 0);
            const std::wstring title = wide(spec.title);
            const HWND hwnd = CreateWindowExW(0, kClass, title.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                                              r.bottom - r.top, nullptr, nullptr, inst, nullptr);
            if (!hwnd)
            {
                std::fprintf(stderr, "[nr_core] window: not made (error %lu)\n", GetLastError());
                return false;
            }
            g_thread = GetCurrentThreadId();
            g_own.store(true);
            g_closeAsked.store(false);
            g_window.store(hwnd);
            if (spec.rawInput != 0)
                registerRawInput(hwnd);
            if (spec.visible)
            {
                ShowWindow(hwnd, SW_SHOW);
                UpdateWindow(hwnd);
            }
            return true;
        }

        bool watchHostWindow(const WindowSpec &spec)
        {
            const HWND hwnd = static_cast<HWND>(spec.handle);
            DWORD process = 0;
            const DWORD thread = IsWindow(hwnd) ? GetWindowThreadProcessId(hwnd, &process) : 0;
            if (!thread || process != GetCurrentProcessId())
            {
                std::fprintf(stderr, "[nr_core] window: the host's window %p is not a window of this program\n", spec.handle);
                return false;
            }
            g_thread = thread;
            g_own.store(false);
            g_closeAsked.store(false);
            g_window.store(hwnd);
            // The hooks belong to the thread that sets them and go when it ends, so a thread of the core's own sets them and
            // waits until closeWindow: whichever thread opens or closes, the hooks live as long as the watching (the hook
            // procedures themselves run on the window's thread).
            auto set = std::make_shared<std::promise<bool>>(); // (shared: the waiter may return before set_value has)
            std::future<bool> isSet = set->get_future();
            g_watcher = std::thread([thread, set] {
                MSG m;
                PeekMessageW(&m, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // (this thread's message queue, for the WM_QUIT)
                g_watcherThread.store(GetCurrentThreadId());
                // (A thread of this program, hooks in this program's code: no module handle.)
                g_hookSent = SetWindowsHookExW(WH_CALLWNDPROC, sentHook, nullptr, thread);
                g_hookPosted = SetWindowsHookExW(WH_GETMESSAGE, postedHook, nullptr, thread);
                const bool ok = g_hookSent && g_hookPosted;
                if (!ok)
                    std::fprintf(stderr, "[nr_core] window: the host's window's messages cannot be watched (error %lu)\n", GetLastError());
                set->set_value(ok);
                if (ok)
                    while (GetMessageW(&m, nullptr, 0, 0) > 0)
                    {
                    }
                if (g_hookSent)
                    UnhookWindowsHookEx(g_hookSent);
                if (g_hookPosted)
                    UnhookWindowsHookEx(g_hookPosted);
                g_hookSent = g_hookPosted = nullptr;
            });
            if (!isSet.get())
            {
                g_watcher.join();
                g_watcherThread.store(0);
                g_window.store(nullptr);
                return false;
            }
            if (spec.rawInput == 1)
            {
                if (thread == GetCurrentThreadId())
                    registerRawInput(hwnd);
                else
                    std::fprintf(stderr, "[nr_core] window: raw input for the host's window only from its own thread - not registered\n");
            }
            return true;
        }
    }

    namespace
    {
        void releaseWindow()
        {
            const HWND hwnd = g_window.load();
            releasePresent(); // (S9b: the swap chain goes before its window)
            unregisterRawInput();
            if (g_own.load())
                DestroyWindow(hwnd);
            g_window.store(nullptr); // (the hooks tell nothing from here on)
            if (g_watcher.joinable())
            {
                PostThreadMessageW(g_watcherThread.load(), WM_QUIT, 0, 0); // (the watching thread takes its hooks down and ends)
                g_watcher.join();
                g_watcherThread.store(0);
            }
            g_own.store(false);
            g_closeAsked.store(false);
            g_destroyAsked.store(false);
            g_thread = 0;
        }
    }

    bool open(const Settings &settings, const WindowSpec &spec)
    {
        open(settings);
        if (g_window.load())
        {
            std::fprintf(stderr, "[nr_core] window: one is already in use\n");
            return false;
        }
        if (spec.handle)
            return watchHostWindow(spec);
        return spec.create ? makeOwnWindow(spec) : true;
    }

    void *window() { return g_window.load(); }

    void windowSize(int &width, int &height)
    {
        width = height = 0;
        RECT r = {};
        const HWND hwnd = g_window.load();
        if (hwnd && GetClientRect(hwnd, &r))
            width = r.right - r.left, height = r.bottom - r.top;
    }

    bool pumpEvents()
    {
        const HWND hwnd = g_window.load();
        if (!hwnd)
            return false;
        if (g_own.load() && GetCurrentThreadId() == g_thread)
        {
            if (g_destroyAsked.load())
            {
                releaseWindow();
                return false;
            }
            MSG m;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
            {
                if (m.message == WM_QUIT)
                {
                    g_closeAsked.store(true);
                    break;
                }
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        }
        return !g_closeAsked.load() && IsWindow(hwnd);
    }

    void closeWindow()
    {
        if (!g_window.load())
            return;
        if (g_own.load() && GetCurrentThreadId() != g_thread)
        {
            // (A window is destroyed by its own thread: the core's window goes at that thread's next pumpEvents.)
            g_destroyAsked.store(true);
            return;
        }
        releaseWindow();
    }

    namespace window_detail
    {
        bool forwardRawInput(const void *rawInput)
        {
            const RAWINPUT *ri = static_cast<const RAWINPUT *>(rawInput);
            const WindowEvents &ev = windowEvents();
            if (ri->header.dwType == RIM_TYPEKEYBOARD)
            {
                const RAWKEYBOARD &k = ri->data.keyboard;
                if (k.VKey == 0xFF) // (a fake key Windows adds around some keys: not a key of the keyboard)
                    return true;
                KeyEvent e;
                e.scanCode = k.MakeCode;
                e.virtualKey = k.VKey;
                e.down = (k.Flags & RI_KEY_BREAK) == 0;
                e.extended = (k.Flags & (RI_KEY_E0 | RI_KEY_E1)) != 0;
                if (ev.key)
                    ev.key(e);
                return true;
            }
            if (ri->header.dwType == RIM_TYPEMOUSE)
            {
                const RAWMOUSE &m = ri->data.mouse;
                MouseEvent e;
                e.dx = m.lLastX;
                e.dy = m.lLastY;
                e.absolute = (m.usFlags & MOUSE_MOVE_ABSOLUTE) != 0;
                const USHORT f = m.usButtonFlags;
                const USHORT downs[5] = {RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_DOWN,
                                         RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_5_DOWN};
                const USHORT ups[5] = {RI_MOUSE_LEFT_BUTTON_UP, RI_MOUSE_RIGHT_BUTTON_UP, RI_MOUSE_MIDDLE_BUTTON_UP, RI_MOUSE_BUTTON_4_UP,
                                       RI_MOUSE_BUTTON_5_UP};
                for (int b = 0; b < 5; ++b)
                {
                    if (f & downs[b])
                        e.pressed = static_cast<uint8_t>(e.pressed | (1u << b));
                    if (f & ups[b])
                        e.released = static_cast<uint8_t>(e.released | (1u << b));
                }
                if (f & RI_MOUSE_WHEEL)
                    e.wheel = static_cast<int16_t>(static_cast<SHORT>(m.usButtonData));
                if (f & RI_MOUSE_HWHEEL)
                    e.hwheel = static_cast<int16_t>(static_cast<SHORT>(m.usButtonData));
                if (ev.mouse)
                    ev.mouse(e);
                return true;
            }
            return false;
        }

        uint32_t rawInputRegistered() { return g_rawRegistered.load(); }
        bool watchingHostWindow() { return g_window.load() && !g_own.load() && g_hookSent && g_hookPosted; }
    }
}

#else // (not Windows: no window of the core's yet - the Linux build shows its pictures through the host, as before)

namespace nr_core
{
    bool open(const Settings &settings, const WindowSpec &spec)
    {
        open(settings);
        return !spec.handle && !spec.create;
    }
    void *window() { return nullptr; }
    void windowSize(int &width, int &height) { width = height = 0; }
    bool pumpEvents() { return false; }
    void closeWindow() {}
    namespace window_detail
    {
        bool forwardRawInput(const void *) { return false; }
        uint32_t rawInputRegistered() { return 0u; }
        bool watchingHostWindow() { return false; }
    }
}

#endif
