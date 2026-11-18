// nr_core S12: ImGui frames drawn as the core's present overlay (nr_core_imgui.h) - the renderer half. A small Direct3D 12 ImGui
// renderer of the module's own, drawing COPIES of captured frames (their vertices, indices and commands in plain vectors) with copies
// of ImGui's texture pixels, so it touches no ImGui context or allocator when it draws (the overlay may run on another thread than
// the context's) and leaves the texture
// requests to their renderer when another one owns them (the recomp: rlImGui's GL). It draws what rlImGui draws, the same way:
// vertex colour times the texture, sampled nearest (raylib's point filter), blended source-alpha / one-minus-source-alpha, the
// scissor rounded as rlImGui's EnableScissor does (GL's bottom-up box turned top-down), the projection ImGui's backends use.
// Its GPU objects (root signature, pipelines, a shader-visible heap of texture views, per back buffer vertex / index upload
// buffers) are made at the first overlay on the core's device; whatever a back buffer's overlay used is reused or let go when
// the core calls the overlay for that buffer again (the core waited for it).
// S13a: insert() puts another overlay (a layer of the host's own drawing: nr_core::layerOverlay) between the frames, drawn in its
// place in their order; the module sets its own state again after it.
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
#include <d3d12.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace nr_core_imgui
{
    namespace
    {
        // ---- what capture() keeps (the ImGui context's thread) and the overlay reads (the presenting thread), under g_mutex ----
        struct Cmd
        {
            ImVec4 clip;
            int texture = -1;            // ImTextureData::UniqueID (-1: not one of ImGui's - left out)
            uint32_t elements = 0, indexAt = 0, vertexAt = 0; // in the frame's own arrays
            bool resetState = false;     // ImDrawCallback_ResetRenderState (any other callback is left out)
        };
        struct Frame
        {
            std::vector<ImDrawVert> vertices; // every draw list's, one after the other
            std::vector<ImDrawIdx> indices;
            std::vector<Cmd> cmds;
            ImVec2 pos, size, scale;
            nr_core::Overlay between; // (S13a, insert(): not a frame - this overlay drawn at its place)
        };
        struct Batch
        {
            std::vector<std::unique_ptr<Frame>> frames;
        };
        struct Texture
        {
            int width = 0, height = 0;
            std::vector<uint8_t> pixels; // RGBA8, the latest
            bool dirty = true;           // pixels not yet uploaded
            bool destroy = false;        // ImGui let it go: its copy goes once no frame needs it
            // (the presenting thread's)
            ID3D12Resource *resource = nullptr;
            int resW = 0, resH = 0;
            int view = -1; // its view in the heap
        };

        std::mutex g_mutex;
        std::vector<std::unique_ptr<Frame>> g_captured;
        std::map<int, Texture> g_textures; // by ImTextureData::UniqueID
        std::atomic<bool> g_linear{false};
        std::atomic<uint64_t> g_overlays{0}, g_draws{0}, g_uploads{0}, g_skipped{0};

        // ---- the GPU side (the presenting thread, under g_mutex) ----
        constexpr int kViews = 256;
        constexpr UINT kMaxBuffers = 8;

        template <typename T> void release(T *&p)
        {
            if (p)
                p->Release();
            p = nullptr;
        }

        struct Slot
        {
            ID3D12Resource *vertices = nullptr, *indices = nullptr;
            size_t vertexBytes = 0, indexBytes = 0;
            std::vector<IUnknown *> retired; // let go when the slot comes round again
            std::vector<int> freedViews;
        };
        struct Gpu
        {
            ID3D12Device *device = nullptr;
            ID3D12RootSignature *root = nullptr;
            ID3D12PipelineState *nearest = nullptr, *linear = nullptr;
            ID3D12DescriptorHeap *heap = nullptr;
            UINT viewStep = 0;
            std::vector<int> freeViews;
            Slot slots[kMaxBuffers];
            bool failed = false;
        };
        Gpu g_gpu;

        const char kShader[] = R"(
cbuffer Projection : register(b0) { float4x4 projection; };
struct VI { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };
struct PI { float4 pos : SV_POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };
PI vsMain(VI i)
{
    PI o;
    o.pos = mul(projection, float4(i.pos.xy, 0.0, 1.0));
    o.col = i.col;
    o.uv = i.uv;
    return o;
}
Texture2D texture0 : register(t0);
SamplerState sampler0 : register(SAMPLER_REGISTER); // (s0 nearest, s1 bilinear: the root's two static samplers)
float4 psMain(PI i) : SV_Target
{
    return i.col * texture0.Sample(sampler0, i.uv);
}
)";

        void retire(Slot &s, IUnknown *p)
        {
            if (p)
                s.retired.push_back(p);
        }

        void releaseGpu()
        {
            Gpu &g = g_gpu;
            for (Slot &s : g.slots)
            {
                for (IUnknown *p : s.retired)
                    p->Release();
                s.retired.clear();
                s.freedViews.clear();
                release(s.vertices);
                release(s.indices);
                s.vertexBytes = s.indexBytes = 0;
            }
            for (auto &kv : g_textures)
            {
                release(kv.second.resource);
                kv.second.resW = kv.second.resH = 0;
                kv.second.view = -1;
                kv.second.dirty = true; // (uploaded again on a new device)
            }
            release(g.nearest);
            release(g.linear);
            release(g.root);
            release(g.heap);
            g.freeViews.clear();
            g.device = nullptr;
            g.failed = false;
        }

        ID3D12PipelineState *makePipeline(ID3D12Device *dev, ID3D12RootSignature *root, ID3DBlob *vs, ID3DBlob *ps, DXGI_FORMAT format)
        {
            static const D3D12_INPUT_ELEMENT_DESC layout[] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, static_cast<UINT>(offsetof(ImDrawVert, pos)), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, static_cast<UINT>(offsetof(ImDrawVert, uv)), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, static_cast<UINT>(offsetof(ImDrawVert, col)), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            };
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = root;
            pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
            pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
            pd.InputLayout = {layout, 3};
            pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pd.NumRenderTargets = 1;
            pd.RTVFormats[0] = format;
            pd.SampleDesc.Count = 1;
            pd.SampleMask = UINT_MAX;
            D3D12_RENDER_TARGET_BLEND_DESC &b = pd.BlendState.RenderTarget[0];
            b.BlendEnable = TRUE;
            b.SrcBlend = D3D12_BLEND_SRC_ALPHA; // (raylib's BLEND_ALPHA, what rlImGui draws with)
            b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            b.BlendOp = D3D12_BLEND_OP_ADD;
            b.SrcBlendAlpha = D3D12_BLEND_ONE;
            b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pd.RasterizerState.DepthClipEnable = TRUE;
            ID3D12PipelineState *pso = nullptr;
            if (FAILED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso))))
                return nullptr;
            return pso;
        }

        bool makeGpu(ID3D12Device *dev, DXGI_FORMAT format)
        {
            Gpu &g = g_gpu;
            if (g.device == dev && g.root)
                return true;
            if (g.device == dev && g.failed)
                return false;
            if (g.device) // (another device: everything made again)
                releaseGpu();
            g.device = dev;
            g.failed = true;
            // The root: 16 constants (b0, the projection), the texture (t0), two static samplers (s0 nearest, s1 bilinear: the
            // pixel shader is compiled once for each, two pipelines on the one root).
            D3D12_DESCRIPTOR_RANGE range{};
            range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            range.NumDescriptors = 1;
            D3D12_ROOT_PARAMETER params[2] = {};
            params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            params[0].Constants.Num32BitValues = 16;
            params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
            params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[1].DescriptorTable.NumDescriptorRanges = 1;
            params[1].DescriptorTable.pDescriptorRanges = &range;
            params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
            for (int i = 0; i < 2; ++i)
            {
                samplers[i].Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
                samplers[i].ShaderRegister = static_cast<UINT>(i);
                samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            }
            D3D12_ROOT_SIGNATURE_DESC rd{};
            rd.NumParameters = 2;
            rd.pParameters = params;
            rd.NumStaticSamplers = 2;
            rd.pStaticSamplers = samplers;
            rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
            ID3DBlob *blob = nullptr, *errors = nullptr, *vs = nullptr, *psNearest = nullptr, *psLinear = nullptr;
            bool ok = SUCCEEDED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) &&
                      SUCCEEDED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g.root)));
            release(blob);
            release(errors);
            const D3D_SHADER_MACRO nearestMacros[] = {{"SAMPLER_REGISTER", "s0"}, {nullptr, nullptr}};
            ok = ok && SUCCEEDED(D3DCompile(kShader, sizeof(kShader) - 1, "nr_core_imgui", nearestMacros, nullptr, "vsMain", "vs_5_0", 0, 0, &vs, &errors));
            if (!ok && errors)
                std::fprintf(stderr, "[nr_core_imgui] vs: %s\n", static_cast<const char *>(errors->GetBufferPointer()));
            release(errors);
            ok = ok && SUCCEEDED(D3DCompile(kShader, sizeof(kShader) - 1, "nr_core_imgui", nearestMacros, nullptr, "psMain", "ps_5_0", 0, 0, &psNearest, &errors));
            if (!ok && errors)
                std::fprintf(stderr, "[nr_core_imgui] ps: %s\n", static_cast<const char *>(errors->GetBufferPointer()));
            release(errors);
            const D3D_SHADER_MACRO linearMacros[] = {{"SAMPLER_REGISTER", "s1"}, {nullptr, nullptr}};
            ok = ok && SUCCEEDED(D3DCompile(kShader, sizeof(kShader) - 1, "nr_core_imgui", linearMacros, nullptr, "psMain", "ps_5_0", 0, 0, &psLinear, &errors));
            release(errors);
            if (ok)
            {
                g.nearest = makePipeline(dev, g.root, vs, psNearest, format);
                g.linear = makePipeline(dev, g.root, vs, psLinear, format);
                ok = g.nearest && g.linear;
            }
            release(vs);
            release(psNearest);
            release(psLinear);
            D3D12_DESCRIPTOR_HEAP_DESC hd{};
            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            hd.NumDescriptors = kViews;
            hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            ok = ok && SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.heap)));
            if (!ok)
            {
                std::fprintf(stderr, "[nr_core_imgui] the overlay's pipeline was not made: no ImGui drawn through the core\n");
                return false;
            }
            g.viewStep = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            for (int i = kViews - 1; i >= 0; --i)
                g.freeViews.push_back(i);
            g.failed = false;
            std::fprintf(stderr, "[nr_core_imgui] the overlay's renderer made on the core's device\n");
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

        ID3D12Resource *uploadBuffer(ID3D12Device *dev, size_t bytes)
        {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = bytes;
            rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ID3D12Resource *r = nullptr;
            if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&r))))
                return nullptr;
            return r;
        }

        // A texture's latest pixels to the GPU (its resource made again at a new size), on this overlay's command list.
        bool upload(Slot &s, ID3D12GraphicsCommandList *l, Texture &t)
        {
            Gpu &g = g_gpu;
            ID3D12Device *dev = g.device;
            if (t.width <= 0 || t.height <= 0 || t.pixels.size() < static_cast<size_t>(t.width) * t.height * 4u)
                return false;
            const bool fresh = !t.resource || t.resW != t.width || t.resH != t.height;
            if (fresh)
            {
                if (t.resource)
                {
                    retire(s, t.resource);
                    t.resource = nullptr;
                }
                if (t.view < 0)
                {
                    if (g.freeViews.empty())
                    {
                        std::fprintf(stderr, "[nr_core_imgui] no texture views left (%d)\n", kViews);
                        return false;
                    }
                    t.view = g.freeViews.back();
                    g.freeViews.pop_back();
                }
                D3D12_HEAP_PROPERTIES hp{};
                hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                D3D12_RESOURCE_DESC td{};
                td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                td.Width = static_cast<UINT64>(t.width);
                td.Height = static_cast<UINT>(t.height);
                td.DepthOrArraySize = 1;
                td.MipLevels = 1;
                td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                td.SampleDesc.Count = 1;
                if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                        IID_PPV_ARGS(&t.resource))))
                    return false;
                t.resW = t.width;
                t.resH = t.height;
                D3D12_CPU_DESCRIPTOR_HANDLE cpu = g.heap->GetCPUDescriptorHandleForHeapStart();
                cpu.ptr += static_cast<SIZE_T>(t.view) * g.viewStep;
                dev->CreateShaderResourceView(t.resource, nullptr, cpu);
            }
            else
                barrier(l, t.resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            const UINT pitch = (static_cast<UINT>(t.width) * 4u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
            ID3D12Resource *staging = uploadBuffer(dev, static_cast<size_t>(pitch) * static_cast<size_t>(t.height));
            if (!staging)
                return false;
            uint8_t *p = nullptr;
            const D3D12_RANGE none{0, 0};
            if (FAILED(staging->Map(0, &none, reinterpret_cast<void **>(&p))))
            {
                staging->Release();
                return false;
            }
            for (int y = 0; y < t.height; ++y)
                std::memcpy(p + static_cast<size_t>(y) * pitch, t.pixels.data() + static_cast<size_t>(y) * t.width * 4u, static_cast<size_t>(t.width) * 4u);
            staging->Unmap(0, nullptr);
            D3D12_TEXTURE_COPY_LOCATION to{}, from{};
            to.pResource = t.resource;
            to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            from.pResource = staging;
            from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            from.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            from.PlacedFootprint.Footprint.Width = static_cast<UINT>(t.width);
            from.PlacedFootprint.Footprint.Height = static_cast<UINT>(t.height);
            from.PlacedFootprint.Footprint.Depth = 1;
            from.PlacedFootprint.Footprint.RowPitch = pitch;
            l->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            barrier(l, t.resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            retire(s, staging);
            t.dirty = false;
            ++g_uploads;
            return true;
        }

        // ImGui's projection (its backends'): the frame's display rect to clip space.
        void projection(const Frame &f, float m[16])
        {
            const float L = f.pos.x, R = f.pos.x + f.size.x, T = f.pos.y, B = f.pos.y + f.size.y;
            const float mvp[16] = {2.0f / (R - L), 0.0f, 0.0f, 0.0f, 0.0f, 2.0f / (T - B), 0.0f, 0.0f, 0.0f, 0.0f, 0.5f, 0.0f,
                                   (R + L) / (L - R), (T + B) / (B - T), 0.5f, 1.0f};
            std::memcpy(m, mvp, sizeof(mvp));
        }

        // A command's scissor as rlImGui's EnableScissor sets GL's (the integer truncations included), turned top-down; false: empty.
        bool scissor(const Frame &f, const ImVec4 &clip, const nr_core::OverlayTarget &t, D3D12_RECT &r)
        {
            const float sx = f.scale.x > 0.0f ? f.scale.x : 1.0f, sy = f.scale.y > 0.0f ? f.scale.y : 1.0f;
            const float x = clip.x - f.pos.x, y = clip.y - f.pos.y;
            const float w = clip.z - x, h = clip.w - y;
            const int glX = static_cast<int>(x * sx);
            const int glY = static_cast<int>((f.size.y - static_cast<float>(static_cast<int>(y + h))) * sy);
            const int glW = static_cast<int>(w * sx), glH = static_cast<int>(h * sy);
            const int fbH = static_cast<int>(std::lround(f.size.y * sy));
            LONG left = glX, right = glX + glW, top = fbH - (glY + glH), bottom = fbH - glY;
            left = std::clamp<LONG>(left, 0, static_cast<LONG>(t.width));
            right = std::clamp<LONG>(right, 0, static_cast<LONG>(t.width));
            top = std::clamp<LONG>(top, 0, static_cast<LONG>(t.height));
            bottom = std::clamp<LONG>(bottom, 0, static_cast<LONG>(t.height));
            r = {left, top, right, bottom};
            return right > left && bottom > top;
        }

        void draw(Batch &batch, const nr_core::OverlayTarget &t)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            ID3D12Device *dev = static_cast<ID3D12Device *>(t.device);
            ID3D12GraphicsCommandList *l = static_cast<ID3D12GraphicsCommandList *>(t.commandList);
            if (!dev || !l || t.buffers == 0u || t.buffers > kMaxBuffers || !makeGpu(dev, static_cast<DXGI_FORMAT>(t.format)))
            {
                // (no ImGui drawing: the inserted overlays still drawn, in order)
                for (const auto &f : batch.frames)
                    if (f->between)
                        f->between(t);
                return;
            }
            Gpu &g = g_gpu;
            Slot &s = g.slots[t.buffer % t.buffers];
            // (this back buffer's last overlay is done on the GPU: its leftovers go)
            for (IUnknown *p : s.retired)
                p->Release();
            s.retired.clear();
            for (int v : s.freedViews)
                g.freeViews.push_back(v);
            s.freedViews.clear();

            // The textures: ImGui's let-go ones dropped (their views freed with this slot, next time round), the changed ones uploaded.
            for (auto it = g_textures.begin(); it != g_textures.end();)
            {
                Texture &tex = it->second;
                if (tex.destroy)
                {
                    retire(s, tex.resource);
                    tex.resource = nullptr;
                    if (tex.view >= 0)
                        s.freedViews.push_back(tex.view);
                    it = g_textures.erase(it);
                    continue;
                }
                if (tex.dirty)
                    upload(s, l, tex);
                ++it;
            }

            size_t vertices = 0, indices = 0;
            bool inserted = false;
            for (const auto &f : batch.frames)
            {
                vertices += f->vertices.size(), indices += f->indices.size();
                inserted = inserted || static_cast<bool>(f->between);
            }
            if (vertices == 0u || indices == 0u)
            {
                for (const auto &f : batch.frames)
                    if (f->between)
                        f->between(t);
                return;
            }
            const size_t vBytes = vertices * sizeof(ImDrawVert), iBytes = indices * sizeof(ImDrawIdx);
            if (s.vertexBytes < vBytes)
            {
                release(s.vertices);
                s.vertexBytes = vBytes + 5000u * sizeof(ImDrawVert);
                s.vertices = uploadBuffer(dev, s.vertexBytes);
            }
            if (s.indexBytes < iBytes)
            {
                release(s.indices);
                s.indexBytes = iBytes + 10000u * sizeof(ImDrawIdx);
                s.indices = uploadBuffer(dev, s.indexBytes);
            }
            if (!s.vertices || !s.indices)
            {
                s.vertexBytes = s.indexBytes = 0;
                return;
            }
            ImDrawVert *vp = nullptr;
            ImDrawIdx *ip = nullptr;
            const D3D12_RANGE none{0, 0};
            if (FAILED(s.vertices->Map(0, &none, reinterpret_cast<void **>(&vp))))
                return;
            if (FAILED(s.indices->Map(0, &none, reinterpret_cast<void **>(&ip))))
            {
                s.vertices->Unmap(0, nullptr);
                return;
            }
            for (const auto &f : batch.frames)
            {
                if (!f->vertices.empty())
                    std::memcpy(vp, f->vertices.data(), f->vertices.size() * sizeof(ImDrawVert));
                if (!f->indices.empty())
                    std::memcpy(ip, f->indices.data(), f->indices.size() * sizeof(ImDrawIdx));
                vp += f->vertices.size();
                ip += f->indices.size();
            }
            s.vertices->Unmap(0, nullptr);
            s.indices->Unmap(0, nullptr);

            const D3D12_CPU_DESCRIPTOR_HANDLE rtv{static_cast<SIZE_T>(t.rtv)};
            ID3D12DescriptorHeap *heaps[] = {g.heap};
            D3D12_VERTEX_BUFFER_VIEW vbv{s.vertices->GetGPUVirtualAddress(), static_cast<UINT>(vBytes), static_cast<UINT>(sizeof(ImDrawVert))};
            D3D12_INDEX_BUFFER_VIEW ibv{s.indices->GetGPUVirtualAddress(), static_cast<UINT>(iBytes),
                                        sizeof(ImDrawIdx) == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT};
            const float blend[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            // (the module's state: set before its first frame, and again after each inserted overlay)
            auto setState = [&] {
                l->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                l->SetDescriptorHeaps(1, heaps);
                l->SetGraphicsRootSignature(g.root);
                l->SetPipelineState(g_linear.load() ? g.linear : g.nearest);
                l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                l->IASetVertexBuffers(0, 1, &vbv);
                l->IASetIndexBuffer(&ibv);
                l->OMSetBlendFactor(blend);
            };
            bool stateSet = false;
            const D3D12_GPU_DESCRIPTOR_HANDLE heapStart = g.heap->GetGPUDescriptorHandleForHeapStart();

            UINT vtxBase = 0, idxBase = 0;
            for (const auto &f : batch.frames)
            {
                if (f->between)
                {
                    f->between(t);
                    stateSet = false;
                    continue;
                }
                if (!stateSet)
                {
                    setState();
                    stateSet = true;
                }
                const UINT fv = static_cast<UINT>(f->vertices.size()), fi = static_cast<UINT>(f->indices.size());
                if (f->size.x <= 0.0f || f->size.y <= 0.0f)
                {
                    vtxBase += fv, idxBase += fi;
                    continue;
                }
                const D3D12_VIEWPORT vpd{0.0f, 0.0f, f->size.x * (f->scale.x > 0.0f ? f->scale.x : 1.0f), f->size.y * (f->scale.y > 0.0f ? f->scale.y : 1.0f),
                                         0.0f, 1.0f};
                l->RSSetViewports(1, &vpd);
                float m[16];
                projection(*f, m);
                l->SetGraphicsRoot32BitConstants(0, 16, m, 0);
                int bound = -1;
                for (const Cmd &cmd : f->cmds)
                {
                    if (cmd.resetState)
                    {
                        l->SetPipelineState(g_linear.load() ? g.linear : g.nearest);
                        l->SetGraphicsRoot32BitConstants(0, 16, m, 0);
                        bound = -1;
                        continue;
                    }
                    if (cmd.elements == 0u)
                        continue;
                    D3D12_RECT r;
                    if (!scissor(*f, cmd.clip, t, r))
                        continue;
                    const auto tex = g_textures.find(cmd.texture);
                    if (cmd.texture < 0 || tex == g_textures.end() || !tex->second.resource || tex->second.view < 0)
                    {
                        ++g_skipped;
                        continue;
                    }
                    if (bound != tex->second.view)
                    {
                        D3D12_GPU_DESCRIPTOR_HANDLE h = heapStart;
                        h.ptr += static_cast<UINT64>(tex->second.view) * g.viewStep;
                        l->SetGraphicsRootDescriptorTable(1, h);
                        bound = tex->second.view;
                    }
                    l->RSSetScissorRects(1, &r);
                    l->DrawIndexedInstanced(cmd.elements, 1, cmd.indexAt + idxBase, static_cast<INT>(cmd.vertexAt + vtxBase), 0);
                    ++g_draws;
                }
                vtxBase += fv, idxBase += fi;
            }
            if (++g_overlays == 1u)
                std::fprintf(stderr, "[nr_core_imgui] the first ImGui overlay drawn (%zu frame(s)%s, %ux%u)\n", batch.frames.size(),
                             inserted ? " with inserted overlays" : "", t.width, t.height);
        }

        // The texture's pixels as RGBA8 (Alpha8: white with that alpha, as ImGui means it).
        void copyPixels(Texture &t, ImTextureData *tex)
        {
            const int w = tex->Width, h = tex->Height;
            const unsigned char *src = static_cast<const unsigned char *>(tex->GetPixels());
            if (!src || w <= 0 || h <= 0)
                return;
            t.width = w;
            t.height = h;
            t.pixels.resize(static_cast<size_t>(w) * h * 4u);
            if (tex->Format == ImTextureFormat_Alpha8)
            {
                for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i)
                {
                    t.pixels[i * 4u + 0u] = t.pixels[i * 4u + 1u] = t.pixels[i * 4u + 2u] = 255u;
                    t.pixels[i * 4u + 3u] = src[i];
                }
            }
            else
                std::memcpy(t.pixels.data(), src, t.pixels.size());
            t.dirty = true;
        }
    }

    void capture(const ImDrawData *data, Textures textures)
    {
        if (!data || !data->Valid)
            return;
        const bool owned = textures == Textures::Owned;
        auto frame = std::make_unique<Frame>();
        frame->pos = data->DisplayPos;
        frame->size = data->DisplaySize;
        frame->scale = data->FramebufferScale;
        std::lock_guard<std::mutex> lock(g_mutex);
        // ImGui's textures: a new one (or one first seen) and a changed one copied whole; a let-go one marked.
        if (data->Textures)
            for (ImTextureData *tex : *data->Textures)
            {
                const auto known = g_textures.find(tex->UniqueID);
                switch (tex->Status)
                {
                case ImTextureStatus_WantCreate:
                case ImTextureStatus_WantUpdates:
                    copyPixels(g_textures[tex->UniqueID], tex);
                    if (owned)
                    {
                        tex->SetTexID(static_cast<ImTextureID>(tex->UniqueID + 1));
                        tex->Status = ImTextureStatus_OK;
                    }
                    break;
                case ImTextureStatus_WantDestroy:
                    if (known != g_textures.end())
                        known->second.destroy = true;
                    if (owned)
                    {
                        tex->SetTexID(ImTextureID_Invalid);
                        tex->BackendUserData = nullptr;
                        tex->Status = ImTextureStatus_Destroyed;
                    }
                    break;
                case ImTextureStatus_OK:
                    if (known == g_textures.end()) // (made before the module saw it: its pixels are still ImGui's)
                        copyPixels(g_textures[tex->UniqueID], tex);
                    break;
                default:
                    break;
                }
            }
        frame->vertices.reserve(static_cast<size_t>(data->TotalVtxCount));
        frame->indices.reserve(static_cast<size_t>(data->TotalIdxCount));
        for (int n = 0; n < data->CmdListsCount; ++n)
        {
            const ImDrawList *dl = data->CmdLists[n];
            const uint32_t vertexBase = static_cast<uint32_t>(frame->vertices.size()), indexBase = static_cast<uint32_t>(frame->indices.size());
            frame->vertices.insert(frame->vertices.end(), dl->VtxBuffer.Data, dl->VtxBuffer.Data + dl->VtxBuffer.Size);
            frame->indices.insert(frame->indices.end(), dl->IdxBuffer.Data, dl->IdxBuffer.Data + dl->IdxBuffer.Size);
            for (const ImDrawCmd &c : dl->CmdBuffer)
            {
                Cmd cmd;
                cmd.clip = c.ClipRect;
                if (c.UserCallback)
                {
                    cmd.resetState = c.UserCallback == ImDrawCallback_ResetRenderState; // (another callback cannot run here: left out)
                    if (cmd.resetState)
                        frame->cmds.push_back(cmd);
                    continue;
                }
                cmd.texture = c.TexRef._TexData ? c.TexRef._TexData->UniqueID : -1;
                cmd.elements = c.ElemCount;
                cmd.indexAt = indexBase + c.IdxOffset;
                cmd.vertexAt = vertexBase + c.VtxOffset;
                frame->cmds.push_back(cmd);
            }
        }
        g_captured.push_back(std::move(frame));
    }

    void insert(nr_core::Overlay overlay)
    {
        if (!overlay)
            return;
        auto frame = std::make_unique<Frame>();
        frame->between = std::move(overlay);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_captured.push_back(std::move(frame));
    }

    nr_core::Overlay take()
    {
        auto batch = std::make_shared<Batch>();
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            batch->frames.swap(g_captured);
        }
        if (batch->frames.empty())
            return {};
        return [batch](const nr_core::OverlayTarget &t) { draw(*batch, t); };
    }

    void drop()
    {
        std::vector<std::unique_ptr<Frame>> gone;
        std::lock_guard<std::mutex> lock(g_mutex);
        gone.swap(g_captured);
    }

    void forgetTextures()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto &kv : g_textures)
            kv.second.destroy = true;
    }

    void setLinearSampling(bool linear) { g_linear.store(linear); }

    void release()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        releaseGpu();
    }

    Stats stats()
    {
        Stats s;
        s.overlays = g_overlays.load();
        s.draws = g_draws.load();
        s.uploads = g_uploads.load();
        s.skipped = g_skipped.load();
        return s;
    }
}

#else // (not Windows: no Direct3D 12 - nothing drawn)

namespace nr_core_imgui
{
    void capture(const ImDrawData *, Textures) {}
    void insert(nr_core::Overlay) {}
    nr_core::Overlay take() { return {}; }
    void drop() {}
    void forgetTextures() {}
    void setLinearSampling(bool) {}
    void release() {}
    Stats stats() { return {}; }
}

#endif
