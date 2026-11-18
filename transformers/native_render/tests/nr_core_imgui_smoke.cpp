// nr_core S12: the smoke test of the core's optional ImGui module (native_render/overlay/nr_core_imgui.h) - a program of the core's
// objects, the module, ImGui and (for the A/B alone) ImGui's own Direct3D 12 renderer. Not part of the player build:
// `cmake --build . --config RelWithDebInfo --target nr_core_imgui_smoke` (with the debug UI's ImGui), then run nr_core_imgui_smoke.exe
// from a folder it may write to (native_render/README.md). Uses the core's public API only, as a native host (the decomp build) does:
// open() with a window of the core's own (hidden unless --show), frames handed over through submit() and presented with an overlay.
//
// 1. Drawing: a fixed ImGui scene (a window with text, a button, a slider; a red rect and a green circle on the foreground) in a
//    context of its own drawn through the module (Textures::Owned) over a picture of one colour, the window's picture saved
//    (imgui_module.bmp): the red rect's inside exactly red, the picture's colour exact away from ImGui's shapes, the font atlas
//    uploaded (its request answered: Status OK). The same scene in a second context drawn by ImGui's own renderer
//    (imgui_impl_dx12) into the same picture (imgui_dx12.bmp), and the module again with bilinear sampling (imgui_module_linear.bmp):
//    the pixels that differ between them counted (where: their box) - the module samples nearest as rlImGui's GL does and rounds
//    the scissor as rlImGui does; imgui_impl_dx12 samples bilinear and rounds its own way.
//    (S13a) insert(): an overlay put between two frames (a magenta rect cleared over the scene's red one, then a frame of a blue
//    rect over both) drawn in its place - blue over magenta over red, exact - and the module's own drawing set up again after it
//    (the blue frame drawn); an overlay inserted alone (no ImGui frame) still drawn (imgui_inserted.bmp, imgui_inserted_alone.bmp).
// 2. Input through the core's window messages (the module's whole host: setup / begin / end on the core's window): a click on a
//    button (WM_MOUSEMOVE + WM_LBUTTONDOWN / UP posted to the window, the core forwarding them, ImGui's Win32 input reading them at
//    the next begin) - one click; the wheel (one notch: MouseWheel 1); text typed into a text field it clicked into (WM_CHAR 'A',
//    U+00E9, U+1F600 as its two UTF-16 halves -> joined into ONE character: U+1F600 with a 32-bit ImWchar, else U+FFFD as rlImGui
//    gives), Backspace (WM_KEYDOWN / UP) -> "A\xC3\xA9"; the IME:
//    ImGui's IME call made for the window with the field's text position, and the window's input context's composition window
//    placed there (ImmGetCompositionWindow; "no IME context" told, not failed, where the system has none); the cursor over the
//    field (WM_SETCURSOR answered by the module: the I-beam). Then that frame presented through the core (imgui_input.bmp).
// Exit 0: SMOKE OK; 1 a drawing check failed; 2 the core did not start; 3 an input check failed.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "nr_core.h"
#include "nr_device.h"
#include "overlay/nr_core_imgui.h"

#include "imgui.h"
#include "backends/imgui_impl_dx12.h"

#include <d3d12.h>
#include <imm.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "imm32")

namespace
{
    constexpr uint32_t kW = 640, kH = 400;
    constexpr uint8_t kBg[3] = {16u, 32u, 64u};
    int g_drawFailed = 0, g_inputFailed = 0;

    void check(int &failed, bool ok, const char *what)
    {
        std::printf("[imgui smoke] %s: %s\n", what, ok ? "ok" : "FAILED");
        failed += ok ? 0 : 1;
    }

    bool loadBmp(const char *path, uint32_t &w, uint32_t &h, std::vector<uint8_t> &rgb)
    {
        FILE *f = std::fopen(path, "rb");
        if (!f)
            return false;
        uint8_t hd[54];
        const bool ok = std::fread(hd, 1, 54, f) == 54 && hd[0] == 'B' && hd[1] == 'M';
        uint32_t off = 0;
        int32_t sw = 0, sh = 0;
        uint16_t bpp = 0;
        if (ok)
        {
            std::memcpy(&off, hd + 10, 4);
            std::memcpy(&sw, hd + 18, 4);
            std::memcpy(&sh, hd + 22, 4);
            std::memcpy(&bpp, hd + 28, 2);
        }
        if (!ok || bpp != 32 || sw <= 0 || sh == 0)
        {
            std::fclose(f);
            return false;
        }
        w = static_cast<uint32_t>(sw);
        h = static_cast<uint32_t>(sh < 0 ? -sh : sh);
        std::vector<uint8_t> raw(static_cast<size_t>(w) * h * 4u);
        std::fseek(f, static_cast<long>(off), SEEK_SET);
        const bool read = std::fread(raw.data(), 1, raw.size(), f) == raw.size();
        std::fclose(f);
        if (!read)
            return false;
        rgb.assign(static_cast<size_t>(w) * h * 3u, 0u);
        for (uint32_t y = 0; y < h; ++y)
        {
            const uint32_t sy = sh > 0 ? h - 1u - y : y;
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint8_t *p = raw.data() + (static_cast<size_t>(sy) * w + x) * 4u;
                uint8_t *q = rgb.data() + (static_cast<size_t>(y) * w + x) * 3u;
                q[0] = p[2], q[1] = p[1], q[2] = p[0];
            }
        }
        return true;
    }

    // A frame of one colour presented with `overlay`, the window's picture saved to `shot`: true when drawn and presented.
    bool presentWith(nr_core::Overlay overlay, const char *shot, uint64_t tick)
    {
        nr_core::FrameInfo f;
        f.tick = tick;
        f.pictureWidth = kW, f.pictureHeight = kH;
        f.aspect = static_cast<float>(kW) / static_cast<float>(kH);
        f.waitForGpu = true;
        f.present = 1;
        f.clear[0] = 0.0f;
        if (shot)
        {
            std::remove(shot);
            f.windowShot = shot;
        }
        f.overlay = std::move(overlay);
        std::atomic<bool> shown{false};
        f.done = [&shown](bool drawn, bool presented) { shown = drawn && presented; };
        const bool queued = nr_core::submit(
            [](nr::gfx::Device &dev, nr_core::Recording &rec) {
                const float c[4] = {kBg[0] / 255.0f, kBg[1] / 255.0f, kBg[2] / 255.0f, 1.0f};
                dev.use(rec.picture, nr::gfx::UseFor::Draw);
                dev.clearColour(rec.picture, c);
            },
            f);
        nr_core::flushSubmitted();
        return queued && shown.load();
    }

    // ---- 1. drawing ----
    float g_slider = 0.5f;
    void scene()
    {
        ImGui::SetNextWindowPos(ImVec2(40.0f, 40.0f));
        ImGui::SetNextWindowSize(ImVec2(320.0f, 190.0f));
        ImGui::Begin("nr_core_imgui smoke", nullptr, ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextUnformatted("The quick brown fox 0123456789");
        ImGui::Button("A button", ImVec2(120.0f, 30.0f));
        ImGui::SliderFloat("slider", &g_slider, 0.0f, 1.0f);
        ImGui::TextUnformatted("Sharp edges, round corners: ()[]{}");
        ImGui::End();
        // (a window at a fractional position: its glyphs fall between texels - where nearest and bilinear sampling part)
        ImGui::SetNextWindowPos(ImVec2(380.5f, 270.25f));
        ImGui::SetNextWindowSize(ImVec2(220.0f, 80.0f));
        ImGui::Begin("half a pixel off", nullptr, ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextUnformatted("Between the texels 0123");
        ImGui::End();
        ImDrawList *fg = ImGui::GetForegroundDrawList();
        fg->AddRectFilled(ImVec2(420.0f, 60.0f), ImVec2(480.0f, 100.0f), IM_COL32(255, 0, 0, 255));
        // (ImGui snaps its windows and glyphs to whole pixels, so its own drawing samples texels 1:1 - nearest and bilinear agree.
        // A corner of the font atlas stretched by a non-whole factor: where they part)
        fg->AddImage(ImGui::GetIO().Fonts->TexRef, ImVec2(30.0f, 262.0f), ImVec2(30.0f + 151.7f, 262.0f + 90.3f), ImVec2(0.0f, 0.0f), ImVec2(0.125f, 0.125f));
        fg->AddCircleFilled(ImVec2(520.0f, 220.0f), 30.0f, IM_COL32(0, 255, 0, 200));
    }
    // A context set up the same way each time: ImGui's default font at 13 px (rlImGui's), the dark style, no ini file.
    ImGuiContext *makeContext()
    {
        ImGuiContext *ctx = ImGui::CreateContext();
        ImGui::SetCurrentContext(ctx);
        ImGui::StyleColorsDark();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        ImFontConfig font;
        font.SizePixels = 13.0f;
        font.PixelSnapH = true;
        io.Fonts->AddFontDefault(&font);
        io.DisplaySize = ImVec2(static_cast<float>(kW), static_cast<float>(kH));
        io.DeltaTime = 1.0f / 60.0f;
        return ctx;
    }
    void sceneFrames(bool dx12, int frames)
    {
        for (int i = 0; i < frames; ++i)
        {
            ImGuiIO &io = ImGui::GetIO();
            io.DeltaTime = 1.0f / 60.0f;
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            if (dx12)
                ImGui_ImplDX12_NewFrame();
            ImGui::NewFrame();
            scene();
            ImGui::Render();
            if (!dx12)
            {
                nr_core_imgui::drop(); // (only the last frame drawn)
                nr_core_imgui::capture(ImGui::GetDrawData(), nr_core_imgui::Textures::Owned);
            }
        }
    }

    // Two window pictures: how many pixels differ (any channel), by how much at most, and where (their box).
    void diff(const char *a, const char *b, const char *why)
    {
        uint32_t wa = 0, ha = 0, wb = 0, hb = 0;
        std::vector<uint8_t> pa, pb;
        if (!loadBmp(a, wa, ha, pa) || !loadBmp(b, wb, hb, pb) || wa != wb || ha != hb)
        {
            std::printf("[imgui smoke] A/B %s vs %s: not compared (missing or sizes differ)\n", a, b);
            return;
        }
        uint32_t n = 0, most = 0, x0 = wa, y0 = ha, x1 = 0, y1 = 0;
        for (uint32_t y = 0; y < ha; ++y)
            for (uint32_t x = 0; x < wa; ++x)
            {
                bool d = false;
                for (int c = 0; c < 3; ++c)
                {
                    const size_t i = (static_cast<size_t>(y) * wa + x) * 3u + c;
                    const uint32_t v = static_cast<uint32_t>(pa[i] > pb[i] ? pa[i] - pb[i] : pb[i] - pa[i]);
                    most = std::max(most, v);
                    d = d || v != 0u;
                }
                if (d)
                {
                    ++n;
                    x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
                }
            }
        if (n)
            std::printf("[imgui smoke] A/B %s vs %s: %u of %u pixels differ, most by %u levels, in the box %u,%u-%u,%u (%s)\n", a, b, n, wa * ha,
                        most, x0, y0, x1, y1, why);
        else
            std::printf("[imgui smoke] A/B %s vs %s: identical (%s)\n", a, b, why);
    }

    // ImGui's own Direct3D 12 renderer: its device objects made on this thread (the core's device, taken from a first overlay),
    // its draws recorded inside the core's present (the overlay) while this thread waits in flushSubmitted.
    ID3D12DescriptorHeap *g_dx12Heap = nullptr;
    int g_dx12Next = 0;
    UINT g_dx12Step = 0;

    void drawingChecks()
    {
        std::atomic<ID3D12Device *> device{nullptr};
        std::atomic<ID3D12CommandQueue *> queue{nullptr};
        presentWith([&](const nr_core::OverlayTarget &t) {
            device = static_cast<ID3D12Device *>(t.device);
            queue = static_cast<ID3D12CommandQueue *>(t.queue);
        }, nullptr, 1u);
        check(g_drawFailed, device.load() && queue.load(), "the core's device and queue handed to an overlay");
        if (!device.load())
            return;

        // (a) the module, nearest (rlImGui's sampling)
        ImGuiContext *a = makeContext();
        ImGuiIO &ioA = ImGui::GetIO();
        ioA.BackendRendererName = "nr_core_imgui";
        ioA.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
        sceneFrames(false, 3);
        ImTextureData *atlas = ioA.Fonts->TexData;
        const bool answered = atlas && atlas->Status == ImTextureStatus_OK && atlas->GetTexID() != ImTextureID_Invalid;
        nr_core_imgui::setLinearSampling(false);
        const bool shownA = presentWith(nr_core_imgui::take(), "imgui_module.bmp", 2u);
        const nr_core_imgui::Stats st = nr_core_imgui::stats();
        uint32_t w = 0, h = 0;
        std::vector<uint8_t> got;
        const bool loaded = shownA && loadBmp("imgui_module.bmp", w, h, got) && w == kW && h == kH;
        uint32_t redWrong = 0, bgWrong = 0;
        if (loaded)
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x)
                {
                    const uint8_t *p = got.data() + (static_cast<size_t>(y) * w + x) * 3u;
                    if (x >= 420u && x < 480u && y >= 60u && y < 100u)
                        redWrong += (p[0] != 255u || p[1] != 0u || p[2] != 0u) ? 1u : 0u;
                    else if ((x < 30u || x >= 380u || y < 30u || y >= 250u) && !(x >= 410u && x < 490u && y >= 50u && y < 110u) &&
                             !(x >= 480u && x < 560u && y >= 180u && y < 260u) && !(x >= 375u && x < 606u && y >= 265u && y < 356u) &&
                             !(x >= 25u && x < 187u && y >= 257u && y < 358u))
                        bgWrong += (p[0] != kBg[0] || p[1] != kBg[1] || p[2] != kBg[2]) ? 1u : 0u;
                }
        char what[300];
        std::snprintf(what, sizeof(what), "the module's drawing (imgui_module.bmp): presented %s, the red rect wrong %u px, the picture away from "
                                          "ImGui wrong %u px; overlays %llu, draws %llu, uploads %llu, left out %llu; the atlas's request answered %s",
                      shownA ? "yes" : "NO", redWrong, bgWrong, static_cast<unsigned long long>(st.overlays), static_cast<unsigned long long>(st.draws),
                      static_cast<unsigned long long>(st.uploads), static_cast<unsigned long long>(st.skipped), answered ? "yes" : "NO");
        check(g_drawFailed, loaded && redWrong == 0u && bgWrong == 0u && answered && st.draws > 0u && st.uploads > 0u && st.skipped == 0u, what);

        // (a2) the module, bilinear
        sceneFrames(false, 1);
        nr_core_imgui::setLinearSampling(true);
        check(g_drawFailed, presentWith(nr_core_imgui::take(), "imgui_module_linear.bmp", 3u), "the module's drawing, bilinear (imgui_module_linear.bmp)");
        nr_core_imgui::setLinearSampling(false);

        // (a3, S13a) insert(): an overlay between two frames, and one alone.
        {
            struct Quad
            {
                int r[4];
                uint8_t c[3];
            };
            auto clearQuad = [](Quad q) {
                return [q](const nr_core::OverlayTarget &t) {
                    auto *l = static_cast<ID3D12GraphicsCommandList *>(t.commandList);
                    const D3D12_CPU_DESCRIPTOR_HANDLE rtv{static_cast<SIZE_T>(t.rtv)};
                    const float c[4] = {q.c[0] / 255.0f, q.c[1] / 255.0f, q.c[2] / 255.0f, 1.0f};
                    const D3D12_RECT rect{q.r[0], q.r[1], q.r[2], q.r[3]};
                    if (l && t.rtv)
                        l->ClearRenderTargetView(rtv, c, 1, &rect);
                };
            };
            const Quad magenta{{430, 66, 470, 94}, {255u, 0u, 255u}};
            sceneFrames(false, 1);
            nr_core_imgui::insert(clearQuad(magenta));
            {
                ImGuiIO &io = ImGui::GetIO();
                io.DeltaTime = 1.0f / 60.0f;
                ImGui::NewFrame();
                ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(440.0f, 72.0f), ImVec2(460.0f, 88.0f), IM_COL32(0, 0, 255, 255));
                ImGui::Render();
                nr_core_imgui::capture(ImGui::GetDrawData(), nr_core_imgui::Textures::Owned);
            }
            const bool shownI = presentWith(nr_core_imgui::take(), "imgui_inserted.bmp", 4u);
            uint32_t wi = 0, hi = 0, blueWrong = 0, magentaWrong = 0, redWrong2 = 0;
            std::vector<uint8_t> gi;
            const bool loadedI = shownI && loadBmp("imgui_inserted.bmp", wi, hi, gi) && wi == kW && hi == kH;
            if (loadedI)
                for (uint32_t y = 60u; y < 100u; ++y)
                    for (uint32_t x = 420u; x < 480u; ++x)
                    {
                        const uint8_t *p = gi.data() + (static_cast<size_t>(y) * wi + x) * 3u;
                        if (x >= 440u && x < 460u && y >= 72u && y < 88u)
                            blueWrong += (p[0] != 0u || p[1] != 0u || p[2] != 255u) ? 1u : 0u;
                        else if (x >= 430u && x < 470u && y >= 66u && y < 94u)
                            magentaWrong += (p[0] != 255u || p[1] != 0u || p[2] != 255u) ? 1u : 0u;
                        else
                            redWrong2 += (p[0] != 255u || p[1] != 0u || p[2] != 0u) ? 1u : 0u;
                    }
            std::snprintf(what, sizeof(what), "an overlay inserted between two frames (S13a, imgui_inserted.bmp): presented %s; the second frame's "
                                              "blue wrong %u px, the inserted magenta wrong %u px, the first frame's red wrong %u px",
                          shownI ? "yes" : "NO", blueWrong, magentaWrong, redWrong2);
            check(g_drawFailed, loadedI && blueWrong == 0u && magentaWrong == 0u && redWrong2 == 0u, what);
            const Quad alone{{10, 10, 30, 20}, {0u, 255u, 255u}};
            nr_core_imgui::drop();
            nr_core_imgui::insert(clearQuad(alone));
            const bool shownJ = presentWith(nr_core_imgui::take(), "imgui_inserted_alone.bmp", 5u);
            std::vector<uint8_t> gj;
            uint32_t wj = 0, hj = 0, aloneWrong = 0, restWrong = 0;
            const bool loadedJ = shownJ && loadBmp("imgui_inserted_alone.bmp", wj, hj, gj) && wj == kW && hj == kH;
            if (loadedJ)
                for (uint32_t y = 0; y < hj; ++y)
                    for (uint32_t x = 0; x < wj; ++x)
                    {
                        const uint8_t *p = gj.data() + (static_cast<size_t>(y) * wj + x) * 3u;
                        if (x >= 10u && x < 30u && y >= 10u && y < 20u)
                            aloneWrong += (p[0] != 0u || p[1] != 255u || p[2] != 255u) ? 1u : 0u;
                        else
                            restWrong += (p[0] != kBg[0] || p[1] != kBg[1] || p[2] != kBg[2]) ? 1u : 0u;
                    }
            std::snprintf(what, sizeof(what), "an overlay inserted alone (S13a, imgui_inserted_alone.bmp): presented %s; its rect wrong %u px, the "
                                              "rest wrong %u px",
                          shownJ ? "yes" : "NO", aloneWrong, restWrong);
            check(g_drawFailed, loadedJ && aloneWrong == 0u && restWrong == 0u, what);
        }

        // (b) ImGui's own renderer, in a context of its own
        ImGuiContext *b = makeContext();
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 16;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device.load()->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_dx12Heap))))
        {
            check(g_drawFailed, false, "imgui_impl_dx12's descriptor heap");
            return;
        }
        g_dx12Step = device.load()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        ImGui_ImplDX12_InitInfo info;
        info.Device = device.load();
        info.CommandQueue = queue.load();
        info.NumFramesInFlight = 3;
        info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        info.DSVFormat = DXGI_FORMAT_UNKNOWN;
        info.SrvDescriptorHeap = g_dx12Heap;
        info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo *, D3D12_CPU_DESCRIPTOR_HANDLE *cpu, D3D12_GPU_DESCRIPTOR_HANDLE *gpu) {
            *cpu = g_dx12Heap->GetCPUDescriptorHandleForHeapStart();
            *gpu = g_dx12Heap->GetGPUDescriptorHandleForHeapStart();
            cpu->ptr += static_cast<SIZE_T>(g_dx12Next) * g_dx12Step;
            gpu->ptr += static_cast<UINT64>(g_dx12Next) * g_dx12Step;
            ++g_dx12Next;
        };
        info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo *, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE) {};
        const bool inited = ImGui_ImplDX12_Init(&info);
        check(g_drawFailed, inited, "imgui_impl_dx12 set up on the core's device (the A/B)");
        if (!inited)
            return;
        sceneFrames(true, 3);
        ImDrawData *data = ImGui::GetDrawData();
        const bool shownB = presentWith([data](const nr_core::OverlayTarget &t) {
            auto *l = static_cast<ID3D12GraphicsCommandList *>(t.commandList);
            l->SetDescriptorHeaps(1, &g_dx12Heap);
            ImGui_ImplDX12_RenderDrawData(data, l);
        }, "imgui_dx12.bmp", 4u);
        check(g_drawFailed, shownB, "imgui_impl_dx12's drawing of the same scene (imgui_dx12.bmp)");
        diff("imgui_module.bmp", "imgui_dx12.bmp", "the module samples nearest + rounds the scissor as rlImGui; imgui_impl_dx12 bilinear + its own rounding");
        diff("imgui_module_linear.bmp", "imgui_dx12.bmp", "both bilinear: what remains is the scissor rounding and the rasterising");
        diff("imgui_module.bmp", "imgui_module_linear.bmp", "the module nearest vs bilinear");
        // (three plain presents: every earlier present's GPU work waited for before the contexts go)
        for (uint64_t t = 5u; t < 8u; ++t)
            presentWith({}, nullptr, t);
        ImGui::SetCurrentContext(b);
        ImGui_ImplDX12_Shutdown();
        ImGui::DestroyContext(b);
        ImGui::SetCurrentContext(a);
        ImGui::GetIO().BackendRendererName = nullptr;
        ImGui::GetIO().BackendFlags &= ~(ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset);
        ImGui::DestroyContext(a);
        nr_core_imgui::forgetTextures(); // (context a's texture numbers: the next context counts from 0 again)
        g_dx12Heap->Release();
        g_dx12Heap = nullptr;
    }

    // ---- 2. input through the window's messages ----
    struct Ui
    {
        ImVec2 button0, button1, field0, field1;
        int clicks = 0;
        char text[64] = {};
        float wheel = 0.0f;
        bool fieldActive = false;
    };
    Ui g_ui;
    void inputScene()
    {
        ImGui::SetNextWindowPos(ImVec2(40.0f, 40.0f));
        ImGui::SetNextWindowSize(ImVec2(360.0f, 220.0f));
        ImGui::Begin("input", nullptr, ImGuiWindowFlags_NoSavedSettings);
        if (ImGui::Button("Click", ImVec2(140.0f, 34.0f)))
            ++g_ui.clicks;
        g_ui.button0 = ImGui::GetItemRectMin(), g_ui.button1 = ImGui::GetItemRectMax();
        ImGui::InputText("##field", g_ui.text, sizeof(g_ui.text));
        g_ui.field0 = ImGui::GetItemRectMin(), g_ui.field1 = ImGui::GetItemRectMax();
        g_ui.fieldActive = ImGui::IsItemActive();
        ImGui::End();
        g_ui.wheel = std::max(g_ui.wheel, ImGui::GetIO().MouseWheel);
    }
    void inputFrame()
    {
        nr_core::pumpEvents(); // (the core's window: its messages handled - forwarded to the module)
        nr_core_imgui::begin();
        inputScene();
        nr_core_imgui::end();
    }
    void frames(int n)
    {
        for (int i = 0; i < n; ++i)
        {
            inputFrame();
            nr_core_imgui::drop();
        }
    }
    LPARAM at(ImVec2 a, ImVec2 b) { return MAKELPARAM(static_cast<int>((a.x + b.x) * 0.5f), static_cast<int>((a.y + b.y) * 0.5f)); }

    struct Ime
    {
        int calls = 0;
        bool visible = false;
        ImVec2 pos{};
        void *handle = nullptr;
    };
    Ime g_ime;
    void (*g_imeNext)(ImGuiContext *, ImGuiViewport *, ImGuiPlatformImeData *) = nullptr;

    void inputChecks()
    {
        const HWND hwnd = static_cast<HWND>(nr_core::window());
        nr_core_imgui::setup(true, nullptr, 13.0f);
        ImGui::GetIO().IniFilename = nullptr;
        ImGuiPlatformIO &pio = ImGui::GetPlatformIO();
        g_imeNext = pio.Platform_SetImeDataFn;
        pio.Platform_SetImeDataFn = [](ImGuiContext *ctx, ImGuiViewport *vp, ImGuiPlatformImeData *d) {
            ++g_ime.calls;
            g_ime.visible = d->WantVisible;
            g_ime.pos = d->InputPos;
            g_ime.handle = vp->PlatformHandleRaw;
            if (g_imeNext)
                g_imeNext(ctx, vp, d);
        };
        check(g_inputFailed, nr_core::windowEvents().message == &nr_core_imgui::onWindowMessage && nr_core_imgui::feedsInput(),
              "the module's host set on the core's window (its messages forwarded to ImGui's Win32 input)");
        frames(3);

        // A click: move + down in one go (ImGui's Win32 input tracks the mouse leaving a hidden window at once - the move again
        // before each button message), up in the next.
        PostMessageW(hwnd, WM_MOUSEMOVE, 0, at(g_ui.button0, g_ui.button1));
        PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, at(g_ui.button0, g_ui.button1));
        frames(2);
        PostMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON, at(g_ui.button0, g_ui.button1));
        PostMessageW(hwnd, WM_LBUTTONUP, 0, at(g_ui.button0, g_ui.button1));
        frames(3);
        char what[300];
        std::snprintf(what, sizeof(what), "a click on the button (WM_MOUSEMOVE, WM_LBUTTONDOWN / UP at %.0f,%.0f): %d click(s)",
                      (g_ui.button0.x + g_ui.button1.x) * 0.5f, (g_ui.button0.y + g_ui.button1.y) * 0.5f, g_ui.clicks);
        check(g_inputFailed, g_ui.clicks == 1, what);

        // The wheel: one notch forward.
        PostMessageW(hwnd, WM_MOUSEMOVE, 0, at(g_ui.button0, g_ui.button1));
        PostMessageW(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), at(g_ui.button0, g_ui.button1));
        frames(3);
        std::snprintf(what, sizeof(what), "the wheel (WM_MOUSEWHEEL one notch): MouseWheel %.2f", g_ui.wheel);
        check(g_inputFailed, g_ui.wheel == 1.0f, what);

        // Into the field, then text: 'A', U+00E9, U+1F600 (two UTF-16 halves), then Backspace.
        PostMessageW(hwnd, WM_MOUSEMOVE, 0, at(g_ui.field0, g_ui.field1));
        PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, at(g_ui.field0, g_ui.field1));
        frames(2);
        PostMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON, at(g_ui.field0, g_ui.field1));
        PostMessageW(hwnd, WM_LBUTTONUP, 0, at(g_ui.field0, g_ui.field1));
        frames(3);
        const bool active = g_ui.fieldActive;
        PostMessageW(hwnd, WM_CHAR, 'A', 1);
        PostMessageW(hwnd, WM_CHAR, 0x00E9, 1);
        PostMessageW(hwnd, WM_CHAR, 0xD83D, 1);
        PostMessageW(hwnd, WM_CHAR, 0xDE00, 1);
        frames(6);
        const std::string typed = g_ui.text;
        // (the two halves joined into ONE character: U+1F600 itself where ImGui is built with 32-bit characters (IMGUI_USE_WCHAR32),
        // else ImGui's replacement character U+FFFD for it - what rlImGui gives too, its characters being 16 bits as well)
        const char *want = sizeof(ImWchar) == 4 ? "A\xC3\xA9\xF0\x9F\x98\x80" : "A\xC3\xA9\xEF\xBF\xBD";
        std::string hex;
        for (unsigned char c : typed)
        {
            char b[4];
            std::snprintf(b, sizeof(b), "%02X ", c);
            hex += b;
        }
        std::snprintf(what, sizeof(what), "text typed into the field (active %s; WM_CHAR A, U+00E9, U+D83D U+DE00; ImWchar %zu bytes): %s(%zu bytes)",
                      active ? "yes" : "NO", sizeof(ImWchar), hex.c_str(), typed.size());
        check(g_inputFailed, active && typed == want, what);
        PostMessageW(hwnd, WM_KEYDOWN, VK_BACK, 0x000E0001);
        frames(2);
        PostMessageW(hwnd, WM_KEYUP, VK_BACK, static_cast<LPARAM>(0xC00E0001u));
        frames(3);
        std::snprintf(what, sizeof(what), "Backspace (WM_KEYDOWN / UP VK_BACK): \"%s\"", g_ui.text);
        check(g_inputFailed, std::string(g_ui.text) == "A\xC3\xA9", what);

        // The IME: ImGui's call for the window with the field's text position, and the window's composition window placed there.
        std::snprintf(what, sizeof(what), "the IME asked for (%d call(s), visible %s, at %.0f,%.0f, for the core's window %s)", g_ime.calls,
                      g_ime.visible ? "yes" : "no", g_ime.pos.x, g_ime.pos.y, g_ime.handle == hwnd ? "yes" : "NO");
        check(g_inputFailed, g_ime.calls > 0 && g_ime.visible && g_ime.handle == hwnd && g_ime.pos.x >= g_ui.field0.x && g_ime.pos.x <= g_ui.field1.x &&
                                 g_ime.pos.y >= g_ui.field0.y - 2.0f && g_ime.pos.y <= g_ui.field1.y,
              what);
        if (HIMC himc = ImmGetContext(hwnd))
        {
            COMPOSITIONFORM cf = {};
            const bool got = ImmGetCompositionWindow(himc, &cf) != FALSE;
            ImmReleaseContext(hwnd, himc);
            std::snprintf(what, sizeof(what), "the window's IME composition window at %ld,%ld (ImGui's %.0f,%.0f)", cf.ptCurrentPos.x,
                          cf.ptCurrentPos.y, g_ime.pos.x, g_ime.pos.y);
            check(g_inputFailed, got && cf.ptCurrentPos.x == static_cast<LONG>(g_ime.pos.x) && cf.ptCurrentPos.y == static_cast<LONG>(g_ime.pos.y), what);
        }
        else
            std::printf("[imgui smoke] the window has no IME input context on this system (not checked)\n");

        // The cursor over the field: the I-beam, set by the module's answer to WM_SETCURSOR.
        // (one frame: the mouse-leave ImGui's tracking posts for the hidden window comes in the next)
        PostMessageW(hwnd, WM_MOUSEMOVE, 0, at(g_ui.field0, g_ui.field1));
        frames(1);
        const LRESULT answered = SendMessageW(hwnd, WM_SETCURSOR, reinterpret_cast<WPARAM>(hwnd), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
        const bool ibeam = GetCursor() == LoadCursorW(nullptr, MAKEINTRESOURCEW(32513));
        std::snprintf(what, sizeof(what), "the cursor over the field (WM_SETCURSOR answered %lld, the I-beam %s)", static_cast<long long>(answered),
                      ibeam ? "yes" : "NO");
        check(g_inputFailed, answered == 1 && ibeam, what);

        // That frame presented through the core.
        inputFrame();
        const bool shown = presentWith(nr_core_imgui::take(), "imgui_input.bmp", 20u);
        check(g_inputFailed, shown, "the input frame presented through the core (imgui_input.bmp)");
        for (uint64_t t = 21u; t < 24u; ++t)
            presentWith({}, nullptr, t);
        nr_core_imgui::shutdown();
        check(g_inputFailed, nr_core::windowEvents().message == nullptr, "shutdown: the module's message hook taken back");
    }
}

int main(int argc, char **argv)
{
    bool show = false;
    for (int i = 1; i < argc; ++i)
        show = show || std::strcmp(argv[i], "--show") == 0;
    std::printf("[imgui smoke] nr_core_imgui smoke test (ImGui %s)\n", IMGUI_VERSION);
    _putenv_s("TF_NATIVE_LIVE", "");
    _putenv_s("TF_PACING", "");
    nr_core::Settings settings;
    settings.sharedPictures = 1;
    settings.livePictures = 1;
    settings.corePacing = 0;
    nr_core::WindowSpec spec;
    spec.create = true;
    spec.title = "nr_core_imgui smoke";
    spec.width = static_cast<int>(kW), spec.height = static_cast<int>(kH);
    spec.visible = show;
    if (!nr_core::open(settings, spec) || !nr_core::ready(60000))
    {
        std::printf("[imgui smoke] the core did not start\n");
        return 2;
    }
    nr_core::pumpEvents();
    drawingChecks();
    std::printf("[imgui smoke] drawing checks: %d failed\n", g_drawFailed);
    inputChecks();
    std::printf("[imgui smoke] input checks: %d failed\n", g_inputFailed);
    nr_core::endSubmits();
    nr_core_imgui::release();
    nr_core::closeWindow();
    if (g_drawFailed)
    {
        std::printf("[imgui smoke] SMOKE FAILED (drawing)\n");
        return 1;
    }
    if (g_inputFailed)
    {
        std::printf("[imgui smoke] SMOKE FAILED (input)\n");
        return 3;
    }
    std::printf("[imgui smoke] SMOKE OK\n");
    return 0;
}
