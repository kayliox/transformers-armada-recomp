// nr_core S12: the whole ImGui host for a window whose messages the core forwards (nr_core_imgui.h setup / begin / end) - the half
// a host without an ImGui platform of its own uses (the native decomp build). ImGui's input is ImGui's own Win32 backend
// (imgui_impl_win32: mouse position, buttons, wheel, keys and modifiers, WM_CHAR text - UTF-16 surrogate pairs joined -, focus,
// XInput pads, the IME's composition window placed at the text cursor through ImGui's default IME function on the window), fed
// with the window's messages as nr_core::WindowEvents::message forwards them: queued as they come (the window's thread) and handed
// to ImGui at the next begin() (the ImGui context's thread), so the two may be different threads. WM_SETCURSOR in the client area
// is answered at once with the cursor ImGui asked for in its last frame (else the window's class cursor would come back at every
// move). Drawn through the module's renderer with the textures its own (Textures::Owned). A separate file so a host that links the
// module for the renderer alone (the recomp) does not take ImGui's Win32 backend with it.
#include "nr_core_imgui.h"

#include "imgui.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "backends/imgui_impl_win32.h"

#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace nr_core_imgui
{
    namespace
    {
        struct Message
        {
            HWND hwnd;
            UINT message;
            WPARAM wParam;
            LPARAM lParam;
        };
        std::mutex g_queueMutex;
        std::deque<Message> g_queue;
        constexpr size_t kQueueMax = 4096; // (no frames for a while: the oldest go)

        ImGuiContext *g_context = nullptr;
        HWND g_window = nullptr;     // asked for at setup (null: nr_core::window())
        bool g_win32 = false;        // ImGui's Win32 backend started on g_input
        HWND g_input = nullptr;
        bool g_ownMessageHook = false;
        std::atomic<int> g_cursor{ImGuiMouseCursor_Arrow};
        std::atomic<bool> g_active{false};

        bool wanted(UINT m)
        {
            switch (m)
            {
            case WM_MOUSEMOVE: case WM_NCMOUSEMOVE: case WM_MOUSELEAVE: case WM_NCMOUSELEAVE:
            case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
            case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK:
            case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP:
            case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
            case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
            case WM_SETFOCUS: case WM_KILLFOCUS: case WM_INPUTLANGCHANGE: case WM_CHAR: case WM_DEVICECHANGE:
                return true;
            default:
                return false;
            }
        }

        // The system cursor ImGui's cursor stands for (its resource number: IDC_* in either character set).
        WORD cursorFor(int c)
        {
            switch (c)
            {
            case ImGuiMouseCursor_TextInput: return 32513;  // IDC_IBEAM
            case ImGuiMouseCursor_ResizeAll: return 32646;  // IDC_SIZEALL
            case ImGuiMouseCursor_ResizeEW: return 32644;   // IDC_SIZEWE
            case ImGuiMouseCursor_ResizeNS: return 32645;   // IDC_SIZENS
            case ImGuiMouseCursor_ResizeNESW: return 32643; // IDC_SIZENESW
            case ImGuiMouseCursor_ResizeNWSE: return 32642; // IDC_SIZENWSE
            case ImGuiMouseCursor_Hand: return 32649;       // IDC_HAND
            case ImGuiMouseCursor_Wait: return 32514;       // IDC_WAIT
            case ImGuiMouseCursor_Progress: return 32650;   // IDC_APPSTARTING
            case ImGuiMouseCursor_NotAllowed: return 32648; // IDC_NO
            default: return 32512;                          // IDC_ARROW
            }
        }

        bool startWin32()
        {
            if (g_win32)
                return true;
            HWND hwnd = g_window ? g_window : static_cast<HWND>(nr_core::window());
            if (!hwnd)
                return false;
            if (!ImGui_ImplWin32_Init(hwnd))
                return false;
            g_win32 = true;
            g_input = hwnd;
            std::fprintf(stderr, "[nr_core_imgui] ImGui's Win32 input on the window %p\n", static_cast<void *>(hwnd));
            return true;
        }
    }

    bool onWindowMessage(void *window, uint32_t message, uint64_t wParam, int64_t lParam)
    {
        if (!g_active.load())
            return false;
        const HWND hwnd = static_cast<HWND>(window);
        if (message == WM_SETCURSOR)
        {
            if (LOWORD(lParam) != HTCLIENT)
                return false;
            const int c = g_cursor.load();
            ::SetCursor(c == ImGuiMouseCursor_None ? nullptr : ::LoadCursorW(nullptr, MAKEINTRESOURCEW(cursorFor(c))));
            return true;
        }
        if (!wanted(message))
            return false;
        std::lock_guard<std::mutex> lock(g_queueMutex);
        if (g_queue.size() >= kQueueMax)
            g_queue.pop_front();
        g_queue.push_back(Message{hwnd, message, static_cast<WPARAM>(wParam), static_cast<LPARAM>(lParam)});
        return false;
    }

    void setup(bool darkTheme, void *window, float fontPixels)
    {
        if (g_context)
            return;
        g_context = ImGui::CreateContext();
        ImGui::SetCurrentContext(g_context);
        if (darkTheme)
            ImGui::StyleColorsDark();
        else
            ImGui::StyleColorsLight();
        ImGuiIO &io = ImGui::GetIO();
        ImFontConfig font;
        font.SizePixels = fontPixels > 0.0f ? fontPixels : 13.0f;
        font.PixelSnapH = true;
        io.Fonts->AddFontDefault(&font);
        io.BackendRendererName = "nr_core_imgui";
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
        g_window = static_cast<HWND>(window);
        startWin32();
        nr_core::WindowEvents ev = nr_core::windowEvents();
        if (!ev.message)
        {
            ev.message = &onWindowMessage;
            nr_core::setWindowEvents(ev);
            g_ownMessageHook = true;
        }
        g_active.store(true);
    }

    void shutdown()
    {
        if (!g_context)
            return;
        g_active.store(false);
        if (g_ownMessageHook)
        {
            nr_core::WindowEvents ev = nr_core::windowEvents();
            if (ev.message == &onWindowMessage)
            {
                ev.message = nullptr;
                nr_core::setWindowEvents(ev);
            }
            g_ownMessageHook = false;
        }
        ImGui::SetCurrentContext(g_context);
        if (g_win32)
            ImGui_ImplWin32_Shutdown();
        g_win32 = false;
        g_input = nullptr;
        drop();
        forgetTextures();
        ImGuiIO &io = ImGui::GetIO();
        io.BackendRendererName = nullptr;
        io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset);
        ImGui::DestroyContext(g_context);
        g_context = nullptr;
        std::lock_guard<std::mutex> lock(g_queueMutex);
        g_queue.clear();
    }

    void begin()
    {
        if (!g_context)
            return;
        ImGui::SetCurrentContext(g_context);
        ImGuiIO &io = ImGui::GetIO();
        if (startWin32())
        {
            std::deque<Message> queued;
            {
                std::lock_guard<std::mutex> lock(g_queueMutex);
                queued.swap(g_queue);
            }
            for (const Message &m : queued)
                ImGui_ImplWin32_WndProcHandler(m.hwnd, m.message, m.wParam, m.lParam);
            ImGui_ImplWin32_NewFrame();
        }
        else
        {
            // (no window yet: the core's window size if any, a 60th of a second)
            int w = 0, h = 0;
            nr_core::windowSize(w, h);
            io.DisplaySize = ImVec2(static_cast<float>(w > 0 ? w : 1), static_cast<float>(h > 0 ? h : 1));
            io.DeltaTime = 1.0f / 60.0f;
        }
        ImGui::NewFrame();
    }

    void end()
    {
        if (!g_context)
            return;
        ImGui::SetCurrentContext(g_context);
        ImGui::Render();
        const ImGuiIO &io = ImGui::GetIO();
        if (!(io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange))
            g_cursor.store(io.MouseDrawCursor ? static_cast<int>(ImGuiMouseCursor_None) : static_cast<int>(ImGui::GetMouseCursor()));
        capture(ImGui::GetDrawData(), Textures::Owned);
    }

    bool feedsInput() { return true; }
}

#else

namespace nr_core_imgui
{
    bool onWindowMessage(void *, uint32_t, uint64_t, int64_t) { return false; }
    void setup(bool, void *, float) {}
    void shutdown() {}
    void begin() {}
    void end() {}
    bool feedsInput() { return true; }
}

#endif
