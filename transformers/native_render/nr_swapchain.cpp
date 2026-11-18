// nr_core S9b: the core presents its pictures into its window (nr_core.h present, PresentRequest; NR_CORE_SPEC.md 4). A DXGI
// flip-model swap chain of the core's own on the window in use (nr_window.cpp: the core's window or the host's), made at the
// first present and resized when the window's client area changes; the picture (one of the shared textures the frames are
// handed over in, d3d::latestShared) drawn into the back buffer the way the recomp's GL window draws it today
// (nr_present.cpp, ps2_runtime): the bars cleared, then either a bilinear stretch of the picture's source rect into the
// destination rect (raylib's DrawTexturePro), or - enlarged with FSR 1 chosen - EASU into a texture of the destination's size
// and RCAS from it into the destination rect, both ported 1:1 from nr_present.cpp's GLSL (texelFetch -> Load, the same
// arithmetic and order). Presented with the sync interval asked; without sync and when the system allows it, with tearing
// (the "Unlimited" cap, VRR). A shot saves the back buffer as drawn (BMP, as the device's saveBmp). Its own command lists,
// fence and descriptors on the renderer's device and queue (ExecuteCommandLists is thread-safe), from the host's thread. S12: a
// request's overlay (a menu: nr_core.h OverlayTarget) is drawn into the back buffer over the picture before the shot and present.
// S13a: the layers (nr_core.h Layer) - shared textures another API of the host draws into (the recomp's GL overlays) - and the
// overlay that draws one over the back buffer with premultiplied alpha (layerOverlay), one texel a pixel.
// Not used by the recomp's player build: nothing calls present().
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
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_5.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace nr::d3d
{
    // (nr_d3d.cpp, for this file) The renderer's device and queue (null: not started), and shared picture `index`'s texture
    // (R8G8B8A8_UNORM, COMMON between uses) with its size (null: none).
    ID3D12Device *presentDevice();
    ID3D12CommandQueue *presentQueue();
    ID3D12Resource *sharedTexture(int index, uint32_t &width, uint32_t &height);
}

namespace nr_core
{
    namespace
    {
        // The present passes. VS: one triangle over the viewport (the destination rect), uv 0..1 across it. Every pass reads
        // its source at tc = (srcOrigin + uv * srcSize) / textureSize - the texture coordinate raylib's DrawTexturePro
        // interpolates for a quad of the source rect over the destination rect.
        const char kPresentShader[] = R"(
cbuffer PresentPass : register(b0)
{
    float4 src;       // the source rect in the texture (pixels): x, y, w, h
    float sharpness;  // RCAS (linear: 2 ^ -stops)
    float3 pad;
};
Texture2D<float4> texture0 : register(t0);
SamplerState linearClamp : register(s0);
struct V
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};
V vsMain(uint id : SV_VertexID)
{
    V o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.uv = t;
    o.pos = float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);
    return o;
}
float2 texSize()
{
    uint w, h;
    texture0.GetDimensions(w, h);
    return float2(w, h);
}
float2 coord(float2 uv) { return (src.xy + uv * src.zw) / texSize(); }
float4 psBilinear(V v) : SV_Target
{
    return float4(texture0.SampleLevel(linearClamp, coord(v.uv), 0.0).rgb, 1.0);
}
float3 tap(int2 p)
{
    int2 s = int2(texSize());
    return texture0.Load(int3(clamp(p, int2(0, 0), s - 1), 0)).rgb;
}
// (S13a) A layer over the back buffer: src = (layer w, h, target w, h), sharpness > 0.5: its rows bottom up. One texel a pixel
// when the sizes match; premultiplied alpha (the pipeline's blend: rgb + dst * (1 - a)).
float4 psLayer(V v) : SV_Target
{
    int2 size = int2(src.xy);
    int2 t = clamp(int2((floor(v.pos.xy) + 0.5) * src.xy / src.zw), int2(0, 0), size - 1);
    if (sharpness > 0.5)
        t.y = size.y - 1 - t.y;
    return texture0.Load(int3(t, 0));
}
// ---- EASU (FSR 1's edge-adaptive upscale): nr_present.cpp kEasu, line for line ----
float luma(float3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }
void setF(inout float2 dir, inout float len, float2 pp, int corner, float lA, float lB, float lC, float lD, float lE)
{
    float w = corner == 0 ? (1.0 - pp.x) * (1.0 - pp.y) : corner == 1 ? pp.x * (1.0 - pp.y) : corner == 2 ? (1.0 - pp.x) * pp.y : pp.x * pp.y;
    float dc = lD - lC, cb = lC - lB;
    float lenX = 1.0 / max(max(abs(dc), abs(cb)), 1.0 / 65536.0);
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = clamp(abs(dirX) * lenX, 0.0, 1.0);
    len += lenX * lenX * w;
    float ec = lE - lC, ca = lC - lA;
    float lenY = 1.0 / max(max(abs(ec), abs(ca)), 1.0 / 65536.0);
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = clamp(abs(dirY) * lenY, 0.0, 1.0);
    len += lenY * lenY * w;
}
void tapF(inout float3 aC, inout float aW, float2 off, float2 dir, float2 len, float lob, float clp, float3 c)
{
    float2 v = float2(off.x * dir.x + off.y * dir.y, off.x * -dir.y + off.y * dir.x) * len;
    float d2 = min(v.x * v.x + v.y * v.y, clp);
    float wB = 2.0 / 5.0 * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = 25.0 / 16.0 * wB - (25.0 / 16.0 - 1.0);
    float w = wB * wA;
    aC += c * w;
    aW += w;
}
float4 psEasu(V v) : SV_Target
{
    float2 pp = coord(v.uv) * texSize() - 0.5;
    float2 fp = floor(pp);
    pp -= fp;
    int2 o = int2(fp);
    float3 b = tap(o + int2(0, -1)), c = tap(o + int2(1, -1));
    float3 e = tap(o + int2(-1, 0)), f = tap(o), g = tap(o + int2(1, 0)), h = tap(o + int2(2, 0));
    float3 i = tap(o + int2(-1, 1)), j = tap(o + int2(0, 1)), k = tap(o + int2(1, 1)), l = tap(o + int2(2, 1));
    float3 n = tap(o + int2(0, 2)), oo = tap(o + int2(1, 2));
    float bL = luma(b), cL = luma(c), eL = luma(e), fL = luma(f), gL = luma(g), hL = luma(h);
    float iL = luma(i), jL = luma(j), kL = luma(k), lL = luma(l), nL = luma(n), oL = luma(oo);
    float2 dir = float2(0.0, 0.0);
    float len = 0.0;
    setF(dir, len, pp, 0, bL, eL, fL, gL, jL);
    setF(dir, len, pp, 1, cL, fL, gL, hL, kL);
    setF(dir, len, pp, 2, fL, iL, jL, kL, nL);
    setF(dir, len, pp, 3, gL, jL, kL, lL, oL);
    float2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < 1.0 / 32768.0;
    dirR = zro ? 1.0 : rsqrt(dirR);
    dir.x = zro ? 1.0 : dir.x;
    dir *= dirR;
    len = len * 0.5;
    len *= len;
    float stretch = (dir.x * dir.x + dir.y * dir.y) / max(abs(dir.x), abs(dir.y));
    float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = 1.0 / lob;
    float3 min4 = min(min(f, g), min(j, k));
    float3 max4 = max(max(f, g), max(j, k));
    float3 aC = float3(0.0, 0.0, 0.0);
    float aW = 0.0;
    tapF(aC, aW, float2(0.0, -1.0) - pp, dir, len2, lob, clp, b);
    tapF(aC, aW, float2(1.0, -1.0) - pp, dir, len2, lob, clp, c);
    tapF(aC, aW, float2(-1.0, 1.0) - pp, dir, len2, lob, clp, i);
    tapF(aC, aW, float2(0.0, 1.0) - pp, dir, len2, lob, clp, j);
    tapF(aC, aW, float2(0.0, 0.0) - pp, dir, len2, lob, clp, f);
    tapF(aC, aW, float2(-1.0, 0.0) - pp, dir, len2, lob, clp, e);
    tapF(aC, aW, float2(1.0, 1.0) - pp, dir, len2, lob, clp, k);
    tapF(aC, aW, float2(2.0, 1.0) - pp, dir, len2, lob, clp, l);
    tapF(aC, aW, float2(2.0, 0.0) - pp, dir, len2, lob, clp, h);
    tapF(aC, aW, float2(1.0, 0.0) - pp, dir, len2, lob, clp, g);
    tapF(aC, aW, float2(1.0, 2.0) - pp, dir, len2, lob, clp, oo);
    tapF(aC, aW, float2(0.0, 2.0) - pp, dir, len2, lob, clp, n);
    return float4(min(max4, max(min4, aC / aW)), 1.0);
}
// ---- RCAS (FSR 1's sharpening): nr_present.cpp kRcas, line for line (its source = the EASU texture, 1:1) ----
float4 psRcas(V v) : SV_Target
{
    int2 sp = int2(floor(coord(v.uv) * texSize()));
    float3 b = tap(sp + int2(0, -1)), d = tap(sp + int2(-1, 0)), e = tap(sp), f = tap(sp + int2(1, 0)), h = tap(sp + int2(0, 1));
    float3 mn4 = min(min(b, d), min(f, h));
    float3 mx4 = max(max(b, d), max(f, h));
    float3 hitMin = min(mn4, e) / (4.0 * mx4 + 1.0 / 65536.0);
    float3 hitMax = (1.0 - max(mx4, e)) / (4.0 * mn4 - 4.0 - 1.0 / 65536.0);
    float3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-(0.25 - 1.0 / 16.0), min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * sharpness;
    return float4((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0), 1.0);
}
)";

        template <typename T> void release(T *&p)
        {
            if (p)
                p->Release();
            p = nullptr;
        }

        constexpr UINT kBuffers = 3;
        constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        // The shader-visible views per back buffer in flight: the picture, the EASU texture, then (S13a) up to 8 layers.
        constexpr UINT kLayerViews = 8;
        constexpr UINT kViewsPerBuffer = 2u + kLayerViews;

        // (S13a) A layer (nr_core.h Layer): its texture, NT handle, and the fence value of the last present that drew it.
        struct LayerTexture
        {
            ID3D12Resource *resource = nullptr;
            HANDLE handle = nullptr;
            uint64_t size = 0;
            UINT width = 0, height = 0;
            uint64_t generation = 0;
            UINT64 readFence = 0;
        };

        struct Chain
        {
            std::mutex mutex;
            bool failed = false;
            HWND hwnd = nullptr;
            ID3D12Device *device = nullptr;
            ID3D12CommandQueue *queue = nullptr;
            IDXGISwapChain3 *swap = nullptr;
            bool tearing = false;
            UINT width = 0, height = 0;
            ID3D12Resource *buffers[kBuffers] = {};
            ID3D12DescriptorHeap *rtvHeap = nullptr, *srvHeap = nullptr;
            UINT rtvStep = 0, srvStep = 0;
            ID3D12CommandAllocator *allocators[kBuffers] = {};
            ID3D12GraphicsCommandList *list = nullptr;
            ID3D12Fence *fence = nullptr;
            HANDLE event = nullptr;
            UINT64 fenceValue = 0, bufferFence[kBuffers] = {};
            ID3D12RootSignature *root = nullptr;
            ID3D12PipelineState *bilinear = nullptr, *easu = nullptr, *rcas = nullptr;
            ID3D12PipelineState *layer = nullptr; // (S13a: premultiplied alpha over the back buffer)
            LayerTexture layers[kLayers];
            UINT layerViews = 0; // (this present's layer views so far)
            UINT presentBuffer = 0; // (the back buffer this present draws)
            ID3D12Resource *mid = nullptr; // (EASU's output, the destination's size)
            UINT midW = 0, midH = 0;
            ID3D12Resource *readback = nullptr;
            UINT64 readbackSize = 0;
            uint64_t presents = 0, overlays = 0;
        };
        Chain g_chain;

        void waitFor(Chain &c, UINT64 value)
        {
            if (c.fence->GetCompletedValue() < value && SUCCEEDED(c.fence->SetEventOnCompletion(value, c.event)))
                WaitForSingleObject(c.event, 5000);
        }
        void waitIdle(Chain &c)
        {
            if (!c.fence)
                return;
            c.queue->Signal(c.fence, ++c.fenceValue);
            waitFor(c, c.fenceValue);
        }

        void releaseBuffers(Chain &c)
        {
            for (ID3D12Resource *&b : c.buffers)
                release(b);
        }
        bool makeTargets(Chain &c)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE h = c.rtvHeap->GetCPUDescriptorHandleForHeapStart();
            for (UINT i = 0; i < kBuffers; ++i)
            {
                if (FAILED(c.swap->GetBuffer(i, IID_PPV_ARGS(&c.buffers[i]))))
                    return false;
                c.device->CreateRenderTargetView(c.buffers[i], nullptr, h);
                h.ptr += c.rtvStep;
            }
            return true;
        }

        ID3D12PipelineState *pipeline(Chain &c, ID3DBlob *vs, const char *entry, bool premultiplied = false)
        {
            ID3DBlob *ps = nullptr, *errors = nullptr;
            if (FAILED(D3DCompile(kPresentShader, sizeof(kPresentShader) - 1, "nr_present", nullptr, nullptr, entry, "ps_5_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &errors)))
            {
                std::fprintf(stderr, "[nr_core] present: shader %s: %s\n", entry, errors ? static_cast<const char *>(errors->GetBufferPointer()) : "failed");
                release(errors);
                return nullptr;
            }
            release(errors);
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = c.root;
            pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
            pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
            pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            if (premultiplied)
            {
                D3D12_RENDER_TARGET_BLEND_DESC &b = pd.BlendState.RenderTarget[0];
                b.BlendEnable = TRUE;
                b.SrcBlend = b.SrcBlendAlpha = D3D12_BLEND_ONE;
                b.DestBlend = b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
                b.BlendOp = b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            }
            pd.SampleMask = UINT_MAX;
            pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pd.RasterizerState.DepthClipEnable = TRUE;
            pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pd.NumRenderTargets = 1;
            pd.RTVFormats[0] = kFormat;
            pd.SampleDesc.Count = 1;
            ID3D12PipelineState *pso = nullptr;
            if (FAILED(c.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso))))
                std::fprintf(stderr, "[nr_core] present: pipeline %s not made\n", entry);
            release(ps);
            return pso;
        }

        // The swap chain, its targets and the passes, made once for the window (false: cannot present).
        bool ready(Chain &c, HWND hwnd)
        {
            if (c.swap && c.hwnd == hwnd)
                return true;
            if (c.failed && c.hwnd == hwnd)
                return false;
            if (c.swap) // (another window: the old chain let go)
            {
                waitIdle(c);
                releaseBuffers(c);
                release(c.swap);
            }
            c.hwnd = hwnd;
            c.failed = true;
            c.device = nr::d3d::presentDevice();
            c.queue = nr::d3d::presentQueue();
            if (!c.device || !c.queue)
            {
                std::fprintf(stderr, "[nr_core] present: the renderer's device is not ready\n");
                c.failed = false; // (asked again at the next present)
                return false;
            }
            IDXGIFactory4 *factory = nullptr;
            if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
                return false;
            IDXGIFactory5 *factory5 = nullptr;
            BOOL tearing = FALSE;
            if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory5))))
            {
                if (FAILED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))))
                    tearing = FALSE;
                release(factory5);
            }
            c.tearing = tearing != FALSE;
            RECT r = {};
            GetClientRect(hwnd, &r);
            c.width = static_cast<UINT>(std::max<LONG>(1, r.right - r.left));
            c.height = static_cast<UINT>(std::max<LONG>(1, r.bottom - r.top));
            DXGI_SWAP_CHAIN_DESC1 sd{};
            sd.Width = c.width;
            sd.Height = c.height;
            sd.Format = kFormat;
            sd.SampleDesc.Count = 1;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.BufferCount = kBuffers;
            sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            sd.Flags = c.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;
            IDXGISwapChain1 *swap1 = nullptr;
            const HRESULT hr = factory->CreateSwapChainForHwnd(c.queue, hwnd, &sd, nullptr, nullptr, &swap1);
            if (SUCCEEDED(hr))
                factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER); // (the host decides about fullscreen)
            release(factory);
            if (FAILED(hr) || FAILED(swap1->QueryInterface(IID_PPV_ARGS(&c.swap))))
            {
                std::fprintf(stderr, "[nr_core] present: no swap chain for the window (%08lx)\n", static_cast<unsigned long>(hr));
                release(swap1);
                return false;
            }
            release(swap1);
            if (!c.rtvHeap)
            {
                D3D12_DESCRIPTOR_HEAP_DESC hd{};
                hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
                hd.NumDescriptors = kBuffers + 1u; // (+ the EASU texture)
                D3D12_DESCRIPTOR_HEAP_DESC sh{};
                sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
                sh.NumDescriptors = kViewsPerBuffer * kBuffers; // (per back buffer in flight: the picture, the EASU texture, layers)
                sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
                if (FAILED(c.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&c.rtvHeap))) ||
                    FAILED(c.device->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&c.srvHeap))))
                    return false;
                c.rtvStep = c.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
                c.srvStep = c.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                for (ID3D12CommandAllocator *&a : c.allocators)
                    if (FAILED(c.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a))))
                        return false;
                if (FAILED(c.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, c.allocators[0], nullptr, IID_PPV_ARGS(&c.list))) ||
                    FAILED(c.list->Close()) || FAILED(c.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&c.fence))))
                    return false;
                c.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                // The root: 8 constants (b0), the source texture (t0), a bilinear clamped sampler (s0).
                D3D12_DESCRIPTOR_RANGE range{};
                range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                range.NumDescriptors = 1;
                D3D12_ROOT_PARAMETER params[2] = {};
                params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
                params[0].Constants.Num32BitValues = 8;
                params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                params[1].DescriptorTable.NumDescriptorRanges = 1;
                params[1].DescriptorTable.pDescriptorRanges = &range;
                params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                D3D12_STATIC_SAMPLER_DESC sampler{};
                sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                sampler.MaxLOD = D3D12_FLOAT32_MAX;
                sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                D3D12_ROOT_SIGNATURE_DESC rd{};
                rd.NumParameters = 2;
                rd.pParameters = params;
                rd.NumStaticSamplers = 1;
                rd.pStaticSamplers = &sampler;
                ID3DBlob *rootBlob = nullptr, *vs = nullptr, *errors = nullptr;
                const bool rootOk = SUCCEEDED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &rootBlob, nullptr)) &&
                                    SUCCEEDED(c.device->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(), IID_PPV_ARGS(&c.root)));
                release(rootBlob);
                const bool vsOk = rootOk && SUCCEEDED(D3DCompile(kPresentShader, sizeof(kPresentShader) - 1, "nr_present", nullptr, nullptr, "vsMain",
                                                                 "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &errors));
                if (!vsOk && errors)
                    std::fprintf(stderr, "[nr_core] present: vsMain: %s\n", static_cast<const char *>(errors->GetBufferPointer()));
                release(errors);
                if (vsOk)
                {
                    c.bilinear = pipeline(c, vs, "psBilinear");
                    c.easu = pipeline(c, vs, "psEasu");
                    c.rcas = pipeline(c, vs, "psRcas");
                    c.layer = pipeline(c, vs, "psLayer", true);
                }
                release(vs);
                if (!c.bilinear || !c.easu || !c.rcas || !c.layer)
                    return false;
            }
            if (!makeTargets(c))
                return false;
            c.failed = false;
            std::fprintf(stderr, "[nr_core] present: a flip-model swap chain on the window, %ux%u, %u buffers, tearing %s\n", c.width, c.height,
                         kBuffers, c.tearing ? "allowed" : "not available");
            return true;
        }

        bool resizeIfNeeded(Chain &c)
        {
            RECT r = {};
            GetClientRect(c.hwnd, &r);
            const UINT w = static_cast<UINT>(std::max<LONG>(1, r.right - r.left)), h = static_cast<UINT>(std::max<LONG>(1, r.bottom - r.top));
            if (w == c.width && h == c.height)
                return true;
            waitIdle(c);
            releaseBuffers(c);
            const HRESULT hr = c.swap->ResizeBuffers(kBuffers, w, h, kFormat, c.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);
            if (FAILED(hr))
            {
                std::fprintf(stderr, "[nr_core] present: the swap chain not resized to %ux%u (%08lx)\n", w, h, static_cast<unsigned long>(hr));
                return false;
            }
            c.width = w;
            c.height = h;
            return makeTargets(c);
        }

        bool midFor(Chain &c, UINT w, UINT h)
        {
            if (c.mid && c.midW == w && c.midH == h)
                return true;
            waitIdle(c);
            release(c.mid);
            D3D12_RESOURCE_DESC td{};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = w;
            td.Height = h;
            td.DepthOrArraySize = 1;
            td.MipLevels = 1;
            td.Format = kFormat; // (8 bits a channel between the passes, as the GL side's render texture)
            td.SampleDesc.Count = 1;
            td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            if (FAILED(c.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                         IID_PPV_ARGS(&c.mid))))
                return false;
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = c.rtvHeap->GetCPUDescriptorHandleForHeapStart();
            rtv.ptr += static_cast<SIZE_T>(kBuffers) * c.rtvStep;
            c.device->CreateRenderTargetView(c.mid, nullptr, rtv);
            c.midW = w;
            c.midH = h;
            return true;
        }

        void barrier(ID3D12GraphicsCommandList *l, ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = r;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = from;
            b.Transition.StateAfter = to;
            l->ResourceBarrier(1, &b);
        }

        void saveShot(Chain &c, const char *path, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &fp)
        {
            void *p = nullptr;
            if (FAILED(c.readback->Map(0, nullptr, &p)))
                return;
            FILE *f = std::fopen(path, "wb");
            if (f)
            {
                const uint32_t rowBytes = c.width * 4u, size = rowBytes * c.height;
                uint8_t header[54] = {'B', 'M'};
                auto put32 = [&](int at, uint32_t v) { std::memcpy(header + at, &v, 4); };
                put32(2, 54u + size);
                put32(10, 54u);
                put32(14, 40u);
                put32(18, c.width);
                put32(22, c.height);
                header[26] = 1;
                header[28] = 32;
                put32(34, size);
                std::fwrite(header, 1, sizeof(header), f);
                std::vector<uint8_t> row(rowBytes);
                for (UINT y = c.height; y-- > 0;) // (BMP rows bottom up, BGRA)
                {
                    std::memcpy(row.data(), static_cast<const uint8_t *>(p) + fp.Offset + static_cast<size_t>(y) * fp.Footprint.RowPitch, rowBytes);
                    for (uint32_t x = 0; x < rowBytes; x += 4u)
                        std::swap(row[x], row[x + 2u]);
                    std::fwrite(row.data(), 1, rowBytes, f);
                }
                std::fclose(f);
                std::fprintf(stderr, "[nr_core] present: the window's picture saved: %s\n", path);
            }
            const D3D12_RANGE none{0, 0};
            c.readback->Unmap(0, &none);
        }
    }

    bool present(const PresentRequest &rq)
    {
        Chain &c = g_chain;
        std::lock_guard<std::mutex> lock(c.mutex);
        const HWND hwnd = static_cast<HWND>(window());
        uint32_t pw = 0, ph = 0;
        ID3D12Resource *picture = rq.picture >= 0 ? nr::d3d::sharedTexture(rq.picture, pw, ph) : nullptr;
        if (!hwnd || !picture || !ready(c, hwnd) || !resizeIfNeeded(c))
            return false;
        const float sx = rq.src[2] > 0.0f ? rq.src[0] : 0.0f, sy = rq.src[2] > 0.0f ? rq.src[1] : 0.0f;
        const float sw = rq.src[2] > 0.0f ? rq.src[2] : static_cast<float>(pw), sh = rq.src[2] > 0.0f ? rq.src[3] : static_cast<float>(ph);
        const float dx = rq.dst[2] > 0.0f ? rq.dst[0] : 0.0f, dy = rq.dst[2] > 0.0f ? rq.dst[1] : 0.0f;
        const float dw = rq.dst[2] > 0.0f ? rq.dst[2] : static_cast<float>(c.width), dh = rq.dst[2] > 0.0f ? rq.dst[3] : static_cast<float>(c.height);
        // (FSR 1 as on the GL side: only when enlarged by more than 1 %; otherwise the bilinear stretch)
        const bool fsr = rq.upscaler == 1 && dw > sw * 1.01f && dh > sh * 1.01f;
        const UINT ow = static_cast<UINT>(std::lround(dw)), oh = static_cast<UINT>(std::lround(dh));
        if (fsr && (ow == 0u || oh == 0u || !midFor(c, ow, oh)))
            return false;

        const UINT index = c.swap->GetCurrentBackBufferIndex();
        waitFor(c, c.bufferFence[index]);
        ID3D12CommandAllocator *alloc = c.allocators[index];
        alloc->Reset();
        ID3D12GraphicsCommandList *l = c.list;
        l->Reset(alloc, nullptr);
        ID3D12DescriptorHeap *heaps[] = {c.srvHeap};
        l->SetDescriptorHeaps(1, heaps);
        l->SetGraphicsRootSignature(c.root);
        l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        // (SRVs per back buffer: the picture, the EASU texture, the layers - S13a)
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = c.srvHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = c.srvHeap->GetGPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(kViewsPerBuffer * index) * c.srvStep;
        gpu.ptr += static_cast<UINT64>(kViewsPerBuffer * index) * c.srvStep;
        c.layerViews = 0;
        c.presentBuffer = index;
        c.device->CreateShaderResourceView(picture, nullptr, cpu);
        D3D12_GPU_DESCRIPTOR_HANDLE pictureSrv = gpu, midSrv = gpu;
        midSrv.ptr += c.srvStep;
        if (fsr)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE m = cpu;
            m.ptr += c.srvStep;
            c.device->CreateShaderResourceView(c.mid, nullptr, m);
        }
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = c.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(index) * c.rtvStep;
        D3D12_CPU_DESCRIPTOR_HANDLE midRtv = c.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        midRtv.ptr += static_cast<SIZE_T>(kBuffers) * c.rtvStep;

        ID3D12Resource *back = c.buffers[index];
        barrier(l, back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        barrier(l, picture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        const float bars[4] = {rq.clear[0], rq.clear[1], rq.clear[2], 1.0f};
        l->ClearRenderTargetView(rtv, bars, 0, nullptr);
        const float sharp = std::exp2(-rq.sharpness);
        const float constants[8] = {sx, sy, sw, sh, sharp, 0.0f, 0.0f, 0.0f};
        const D3D12_VIEWPORT dstView{dx, dy, dw, dh, 0.0f, 1.0f};
        // (the destination rect clipped to the window: a scissor of the whole back buffer)
        const D3D12_RECT all{0, 0, static_cast<LONG>(c.width), static_cast<LONG>(c.height)};
        const D3D12_VIEWPORT allView{0.0f, 0.0f, static_cast<float>(c.width), static_cast<float>(c.height), 0.0f, 1.0f};
        if (fsr)
        {
            // EASU: the picture's source rect into the destination's size (the GL side's g_mid), ...
            barrier(l, c.mid, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            l->OMSetRenderTargets(1, &midRtv, FALSE, nullptr);
            const D3D12_VIEWPORT midView{0.0f, 0.0f, static_cast<float>(ow), static_cast<float>(oh), 0.0f, 1.0f};
            const D3D12_RECT midAll{0, 0, static_cast<LONG>(ow), static_cast<LONG>(oh)};
            l->RSSetViewports(1, &midView);
            l->RSSetScissorRects(1, &midAll);
            l->SetPipelineState(c.easu);
            l->SetGraphicsRoot32BitConstants(0, 8, constants, 0);
            l->SetGraphicsRootDescriptorTable(1, pictureSrv);
            l->DrawInstanced(3, 1, 0, 0);
            barrier(l, c.mid, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            // ... then RCAS from it, 1:1, into the destination rect.
            const float rc[8] = {0.0f, 0.0f, static_cast<float>(ow), static_cast<float>(oh), sharp, 0.0f, 0.0f, 0.0f};
            l->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            l->RSSetViewports(1, &dstView);
            l->RSSetScissorRects(1, &all);
            l->SetPipelineState(c.rcas);
            l->SetGraphicsRoot32BitConstants(0, 8, rc, 0);
            l->SetGraphicsRootDescriptorTable(1, midSrv);
            l->DrawInstanced(3, 1, 0, 0);
        }
        else
        {
            l->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            l->RSSetViewports(1, &dstView);
            l->RSSetScissorRects(1, &all);
            l->SetPipelineState(c.bilinear);
            l->SetGraphicsRoot32BitConstants(0, 8, constants, 0);
            l->SetGraphicsRootDescriptorTable(1, pictureSrv);
            l->DrawInstanced(3, 1, 0, 0);
        }
        barrier(l, picture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        // (S12) The overlay over the picture, on this command list into the back buffer, before the shot and the present.
        if (rq.overlay)
        {
            l->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            l->RSSetViewports(1, &allView);
            l->RSSetScissorRects(1, &all);
            OverlayTarget t;
            t.device = c.device;
            t.queue = c.queue;
            t.commandList = l;
            t.target = back;
            t.rtv = static_cast<uint64_t>(rtv.ptr);
            t.width = c.width;
            t.height = c.height;
            t.format = static_cast<uint32_t>(kFormat);
            t.buffer = index;
            t.buffers = kBuffers;
            rq.overlay(t);
            ++c.overlays;
        }
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        const bool shot = rq.shot && *rq.shot;
        if (shot)
        {
            const D3D12_RESOURCE_DESC bd = back->GetDesc();
            UINT64 bytes = 0;
            c.device->GetCopyableFootprints(&bd, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
            if (!c.readback || c.readbackSize < bytes)
            {
                release(c.readback);
                D3D12_HEAP_PROPERTIES hp{};
                hp.Type = D3D12_HEAP_TYPE_READBACK;
                D3D12_RESOURCE_DESC rd{};
                rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                rd.Width = bytes;
                rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
                rd.SampleDesc.Count = 1;
                rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                if (SUCCEEDED(c.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                IID_PPV_ARGS(&c.readback))))
                    c.readbackSize = bytes;
            }
            barrier(l, back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            if (c.readback)
            {
                D3D12_TEXTURE_COPY_LOCATION to{}, from{};
                to.pResource = c.readback;
                to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                to.PlacedFootprint = fp;
                from.pResource = back;
                from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                l->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            }
            barrier(l, back, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        }
        else
            barrier(l, back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        if (FAILED(l->Close()))
            return false;
        ID3D12CommandList *lists[] = {l};
        c.queue->ExecuteCommandLists(1, lists);
        const UINT sync = static_cast<UINT>(std::clamp(rq.syncInterval, 0, 4));
        const HRESULT hr = c.swap->Present(sync, sync == 0u && c.tearing ? DXGI_PRESENT_ALLOW_TEARING : 0u);
        c.queue->Signal(c.fence, ++c.fenceValue);
        c.bufferFence[index] = c.fenceValue;
        if (shot && c.readback)
        {
            waitFor(c, c.fenceValue);
            saveShot(c, rq.shot, fp);
        }
        if (++c.presents <= 3u || FAILED(hr))
            std::fprintf(stderr, "[nr_core] present %llu: %ux%u, %s, sync %u%s%s -> %08lx\n", static_cast<unsigned long long>(c.presents), c.width,
                         c.height, fsr ? "FSR 1 (EASU + RCAS)" : "bilinear", sync, sync == 0u && c.tearing ? " (tearing)" : "",
                         rq.overlay ? ", an overlay" : "", static_cast<unsigned long>(hr));
        if (rq.overlay && c.overlays == 1u)
            std::fprintf(stderr, "[nr_core] present: the first overlay drawn (present %llu)\n", static_cast<unsigned long long>(c.presents));
        return SUCCEEDED(hr) || hr == DXGI_STATUS_OCCLUDED;
    }

    void releasePresent()
    {
        Chain &c = g_chain;
        std::lock_guard<std::mutex> lock(c.mutex);
        if (!c.swap)
            return;
        waitIdle(c);
        releaseBuffers(c);
        release(c.swap);
        c.hwnd = nullptr;
        c.width = c.height = 0;
    }

    // ---- S13a: the layers ----
    bool layer(int index, uint32_t width, uint32_t height, Layer &out)
    {
        out = Layer{};
        if (index < 0 || index >= kLayers || width == 0u || height == 0u)
            return false;
        Chain &c = g_chain;
        std::lock_guard<std::mutex> lock(c.mutex);
        ID3D12Device *device = c.device ? c.device : nr::d3d::presentDevice();
        if (!device)
            return false;
        LayerTexture &t = c.layers[index];
        // (the presents that drew it done: only a present made the fence, so none drew it without one)
        if (c.fence && t.readFence)
            waitFor(c, t.readFence);
        if (!t.resource || t.width != width || t.height != height)
        {
            release(t.resource);
            if (t.handle)
                CloseHandle(t.handle);
            t.handle = nullptr;
            D3D12_RESOURCE_DESC td{};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = width;
            td.Height = height;
            td.DepthOrArraySize = 1;
            td.MipLevels = 1;
            td.Format = kFormat;
            td.SampleDesc.Count = 1;
            td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET; // (another API draws into it)
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &td, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                       IID_PPV_ARGS(&t.resource))) ||
                FAILED(device->CreateSharedHandle(t.resource, nullptr, GENERIC_ALL, nullptr, &t.handle)))
            {
                std::fprintf(stderr, "[nr_core] layer %d (%ux%u) not made\n", index, width, height);
                release(t.resource);
                t.handle = nullptr;
                t.width = t.height = 0;
                return false;
            }
            t.size = device->GetResourceAllocationInfo(0, 1, &td).SizeInBytes;
            t.width = width;
            t.height = height;
            ++t.generation;
            t.readFence = 0;
            if (t.generation == 1u)
                std::fprintf(stderr, "[nr_core] layer %d made: %ux%u, shared\n", index, width, height);
        }
        out.handle = t.handle;
        out.size = t.size;
        out.width = t.width;
        out.height = t.height;
        out.generation = t.generation;
        return true;
    }

    namespace
    {
        // Within a present (its mutex held): layer `index` drawn into the target on the present's command list.
        void drawLayer(int index, bool bottomUp, const OverlayTarget &target)
        {
            Chain &c = g_chain;
            if (index < 0 || index >= kLayers || !c.layer || !c.srvHeap || !target.commandList || target.width == 0u || target.height == 0u)
                return;
            LayerTexture &t = c.layers[index];
            if (!t.resource)
                return;
            if (c.layerViews >= kLayerViews)
            {
                static bool told = false;
                if (!told)
                    std::fprintf(stderr, "[nr_core] present: more than %u layers in one present - the rest left out\n", kLayerViews);
                told = true;
                return;
            }
            const UINT view = kViewsPerBuffer * (c.presentBuffer % kBuffers) + 2u + c.layerViews++;
            D3D12_CPU_DESCRIPTOR_HANDLE cpu = c.srvHeap->GetCPUDescriptorHandleForHeapStart();
            D3D12_GPU_DESCRIPTOR_HANDLE gpu = c.srvHeap->GetGPUDescriptorHandleForHeapStart();
            cpu.ptr += static_cast<SIZE_T>(view) * c.srvStep;
            gpu.ptr += static_cast<UINT64>(view) * c.srvStep;
            c.device->CreateShaderResourceView(t.resource, nullptr, cpu);
            ID3D12GraphicsCommandList *l = static_cast<ID3D12GraphicsCommandList *>(target.commandList);
            const D3D12_CPU_DESCRIPTOR_HANDLE rtv{static_cast<SIZE_T>(target.rtv)};
            ID3D12DescriptorHeap *heaps[] = {c.srvHeap};
            l->SetDescriptorHeaps(1, heaps);
            l->SetGraphicsRootSignature(c.root);
            l->SetPipelineState(c.layer);
            l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            l->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(target.width), static_cast<float>(target.height), 0.0f, 1.0f};
            const D3D12_RECT all{0, 0, static_cast<LONG>(target.width), static_cast<LONG>(target.height)};
            l->RSSetViewports(1, &vp);
            l->RSSetScissorRects(1, &all);
            const float constants[8] = {static_cast<float>(t.width), static_cast<float>(t.height), static_cast<float>(target.width),
                                        static_cast<float>(target.height), bottomUp ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
            l->SetGraphicsRoot32BitConstants(0, 8, constants, 0);
            l->SetGraphicsRootDescriptorTable(1, gpu);
            barrier(l, t.resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            l->DrawInstanced(3, 1, 0, 0);
            barrier(l, t.resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            t.readFence = c.fenceValue + 1u; // (the value this present signals once its command list is done)
        }
    }

    Overlay layerOverlay(int index, bool bottomUp)
    {
        return [index, bottomUp](const OverlayTarget &target) { drawLayer(index, bottomUp, target); };
    }

    void releaseLayers()
    {
        Chain &c = g_chain;
        std::lock_guard<std::mutex> lock(c.mutex);
        for (LayerTexture &t : c.layers)
        {
            if (c.fence && t.readFence)
                waitFor(c, t.readFence);
            release(t.resource);
            if (t.handle)
                CloseHandle(t.handle);
            t = LayerTexture{0, nullptr, 0u, 0u, 0u, t.generation, 0u};
        }
    }
}

#else // (not Windows: the core presents nothing yet)

namespace nr_core
{
    bool present(const PresentRequest &) { return false; }
    void releasePresent() {}
    bool layer(int, uint32_t, uint32_t, Layer &out)
    {
        out = Layer{};
        return false;
    }
    Overlay layerOverlay(int, bool) { return {}; }
    void releaseLayers() {}
}

#endif
