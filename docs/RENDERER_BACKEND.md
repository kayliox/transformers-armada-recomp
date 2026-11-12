# The native renderer's graphics backend

This is the document to work from when writing a second backend for the native renderer (Vulkan), and a new window
and present path (SDL3 with a swapchain). It describes what the renderer asks of a backend, what the Direct3D 12
backend does for each call, what a Vulkan backend has to replace, and the test a new backend has to pass.

The native renderer (`transformers/native_render/`) draws the game with the PC's GPU in place of the emulated PS2
graphics hardware. It does not run the PS2's GS. It captures what the game sends to the GS each frame, turns that into
a draw list, and draws the list with a modern graphics API.

## 1. The parts

| Part | Files | Graphics API |
|---|---|---|
| The capture: the game's display lists and device calls turned into a frame's draw list (`Frame`, `nr_types.h`) | `nr_capture.cpp`, `nr_vif.cpp`, `nr_gif.cpp`, `nr_n5.cpp`, `nr_passes.cpp`, `nr_grass.cpp`, `nr_particles.cpp`, `nr_rain.cpp`, `nr_movie.cpp`, `nr_warp.cpp`, `nr_occlusion.cpp`, `nr_texture.cpp`, `nr_hires.cpp` | none |
| The render thread: hands each frame to the backend | `nr_thread.cpp` | none |
| The frame builder: a frame's draw list turned into calls on the device | `nr_frame.cpp`, `nr_frame.h` | none (an `#error` at the end of `nr_frame.cpp` fails the build if a graphics API header reaches it) |
| The device interface | `nr_device.h` (`nr::gfx::Device`), `nr_backend.h` (the neutral types, the outer calls) | none |
| Shared helpers: the GS state as neutral values, the constant blocks' layout, clipping runs | `nr_draw.h`, `nr_draw.cpp` | none |
| The Direct3D 12 backend: the device, the shaders, the pacing, the hand-over to the window | `nr_d3d.cpp` | Direct3D 12 |
| The window's side of the hand-over | `nr_present.cpp`, `nr_glshare.cpp`, `nr_glshare.h` | OpenGL (raylib) |

A Vulkan backend replaces `nr_d3d.cpp` (and, with SDL3, the window's side). Everything above it stays as it is. Every
rendering fix is made in the shared files, so it reaches every backend.

## 2. The backend's outer calls

The rest of the game talks to the backend through a few calls. They were free functions in `nr::d3d`, declared in
`nr_types.h` and `nr_glshare.h`, and the callers (`nr_capture.cpp`, `nr_thread.cpp`, `nr_present.cpp`, `nr_hires.cpp`,
`nr_warp.cpp`, `src/overrides.cpp`) called them directly. `nr_backend.h` holds the same calls as one interface,
`nr::Backend`, which the Direct3D 12 backend implements by forwarding to those functions. A second backend
implements `nr::Backend`. Done: the callers use `nr::backend()` instead of `nr::d3d::` (only the shared-texture type
`d3d::SharedPicture` and `d3d::kSharedPictures` stay), and `nr_backend.cpp` chooses the backend once at start:
`TF_GFX_API=vulkan` the Vulkan backend (`nr_vulkan.cpp`, in development), anything else Direct3D 12. `nr::backend()`
and `gfx::device()` are defined there; the Direct3D 12 backend's own are `d3d12Backend()` and `gfx::deviceD3d12()`.

| Call | Thread | What it does |
|---|---|---|
| `prepare()` | game thread | Make the device, shaders and pipelines on a thread of their own. |
| `start()` | render thread | The same on the calling thread. False: no backend (the emulated renderer stays). |
| `startingNow()`, `drawsPictures()`, `deviceLost()` | any | Still starting; up and drawing; the device was removed (a GPU hang): nothing more is drawn and the emulated picture comes back. |
| `grassGpuReady()` | render thread | The GPU grass pass exists (section 4.9). False: the capture grows the grass on the CPU. |
| `waitForFrameSlot()` | render thread | Wait until the next frame slot is free; returns the ms waited. |
| `render(frame, shotPath)` | render thread | Draw the frame (section 3). With `shotPath`, also save the picture as a BMP. |
| `preUpload(texture)` | any | A game texture made ahead, handed to the uploader at the next frame. |
| `latestPicture(pixels, rowPitch, index, serial, meta)`, `pictureShown(index)` | window thread | Linux only: the next CPU picture, in the order the pictures were handed over: the hand-over's mapped read-back itself, held (not read back into again) until the window calls `pictureShown` after its upload. Windows keeps 0.6's `latestPicture(pixels, width, height, serial, has3D)`, which copies the newest picture into a vector. |
| `setSharedPictures(on)`, `latestShared(picture, serial)`, `sharedHeld(index)` | window thread | The newest picture as a texture the window imports, and which one the window still holds. |
| `waitPicture(serial, untilNs, lastNs)`, `pacingMode()` | window thread | The pacer: wait for the next picture by its due time. |
| `setWindowSize(w, h)` (Linux) / `setWindowLines(h)` (Windows: what 0.6 and earlier use) | window thread | The window's size in pixels, called every frame by the picture provider, not only on a resize. It is one or two relaxed atomic stores (Linux also stores the width), so calling it each frame costs next to nothing; the next picture uses it (Linux: as many lines as the window shows, or its width over the picture's shape in a narrower window; Windows: the window's lines, as 0.6 does). |
| `setAntiAliasing(mode)`, `setSupersampling(on)`, `setSpeedupsForced(on)` | any | Settings, used from the next frame: the anti-aliasing pass (0 off, 1 FXAA, 2 SMAA 1x), supersampling (the 3D drawn at 1.5x the picture's pixels and averaged down), and a checking aid that forces the picture-keeping speed-ups on or off (-1: as the settings say). |
| `largeImageCap`, `largeImageBudget`, `largeImageBytes`, `adapterMemoryMB`, `qualityPreset` | any | The HD textures' video-memory budget and the quality preset. |
| `gpuPictureMs`, `gpuExtraMs`, `cpuPictureMs`, `replayWaitMs`, `slotWaitMs`, `pacerDelayMs` | render or window thread | Timing figures for the frame pacer and the stats lines. |

The backend's `render()` is short: it returns at once while the device is still starting or after it was lost, then
calls `frame::render(frame, shotPath)` (`nr_frame.h`). The frame builder does the rest through the device interface.
In the Direct3D 12 backend this is `d3d::render` near the end of `nr_d3d.cpp`; the device-removed report (DRED
breadcrumbs) is in its `endFrame`.

## 3. A frame's life

Two frames are in flight: while the GPU draws one, the render thread records the next. Each frame uses one of two
slots (`frameSlot()`: 0 or 1). A slot owns its command list, its part of each ring, its time stamps, its statistics and
its read-backs. The frame builder calls, in this order:

1. **`prepareFrame(width, height)`**, before anything is recorded. The device makes the picture's surfaces at the
   picture's size (again when the size changes), waits until the slot's previous frame (two frames back) has finished
   on the GPU, and calls the frame-done callback for every slot that has finished. False: no picture; the frame is not
   drawn.
2. **`beginFrame(tick)`**. The device takes the textures its uploader has finished, starts the slot's command list,
   sets the bindings every draw uses (section 5), starts the slot's rings again, counts time stamps, statistics and
   read-backs from zero, and takes the picture's start time stamp.
3. **The recording.** The frame builder makes and resizes targets, clears them, sets state, writes per-draw data into
   the rings and records the draws, copies, effects, time stamps and read-backs (section 4). Nothing is submitted yet.
4. **`endFrame(FrameEnd)`**. The device closes the recording (its time stamps and statistics resolved) and submits it
   once for each picture of `FrameEnd::pictures`. Each picture first gets its own constants (`PictureOut::constants`,
   the shader's `b1`). After it, the picture is copied to the window: a shared texture, or a read-back for the CPU
   hand-over (section 7), and saved as a BMP when it has a shot. With `FrameEnd::waitForGpu` the call returns only
   once the frame has finished on the GPU and the slot's frame-done callback has run.
5. **The frame-done callback** (`setFrameDone`). The device calls it on the render thread once a slot's frame has
   finished on the GPU, before the slot is recorded again. In it the frame builder reads that slot's time stamps
   (`readTimestamps`), statistics (`readStats`) and read-backs (`readback`). The sun's occlusion probe is read back
   this way and handed to the game. When the device waits for the GPU to go idle in the middle of a frame (a target
   resized), it calls the callback for every slot except the one being recorded.

**Rings.** Per-draw data goes into three streaming rings, each split into one part per slot: the draws' constants
(256-byte aligned blocks of `draw::Constants`), the passes' instance entries (208 bytes each, a vertex stream), and
vertices and indices of meshes made this frame. `ringAlloc` returns where the CPU writes (`RingSpan::cpu`) and where the
GPU reads (`RingSpan::gpu`, opaque to the frame builder). A ring that is full returns false; the frame builder then
leaves that draw out. The Direct3D 12 ring sizes a slot: 32 MB of constants, 32,768 instance entries, 64 MB of vertices.

**Moment pictures.** A frame normally has one picture. With the test switch `TF_POSES=1` (off by default) a frame has
several, each the same recording at its own pose between the previous frame and this one; the extra pictures are
`FrameEnd::pictures[1..]`. The device runs the recording again for each, restoring every resource to the state the
recording began with. `predicate(true)` marks draws that extra pictures may skip (only with `TF_POSES_SKIP=1`).
A first Vulkan backend can draw only the first picture and add the extra ones later.

**Read-backs.** `readTarget(target)` records a copy of a target's colour into CPU memory at that point of the frame.
The data is valid once the frame has finished (in the frame-done callback, or after `endFrame` with `waitForGpu`)
and stays valid until `releaseReadback`. The memory is then used again by a later frame of the same slot. The debug
saves use it too (`TF_NATIVE_SAVE_TARGET`, `TF_NATIVE_SAVE_AFTER`, `TF_NATIVE_SPLIT`, `TF_NATIVE_AASAVE`).

**Time stamps.** `timestamp()` returns a `QueryId` (1, 2, ... in each frame; 0 when none is available). At least
`kTimestampsPerFrame` (8,256) must be available a frame. `readTimestamps(slot, ticks, ticksPerSecond)` returns the slot's
last frame's values, `ticks[id - 1]`. They feed the profile (`TF_NATIVE_PROFILE`, `TF_NATIVE_PASSPROFILE`). The
pictures' own GPU times (for the pacer) are the device's own business and not part of the interface.

## 4. The device interface, call by call

All calls are made on the render thread, between `prepareFrame` and `endFrame`, unless noted. Handles are small
integers handed out by the device; 0 means none. The device tracks each resource's state (Direct3D 12 resource states,
Vulkan image layouts) and inserts the barriers itself. "D3D12" names the place in `nr_d3d.cpp` (the class
`D3d12Device` and the helpers it calls).

### 4.1 The frame's life

| Call | Rules | D3D12 |
|---|---|---|
| `prepareFrame(width, height)` | First call of a frame. A target made with `TargetDesc::shared` is stale after the picture's size changed; the frame builder makes it again when it sees the new size. | `makeTargets` (the picture's colour, depth and read-back), `beginSlot` (the fence wait, `finishSlot`), the pipeline variants and library collected |
| `beginFrame(tick)` | Second call. | `takeUploaded`, the slot's allocator and list reset, root signature, descriptor heap, `b1`, white texture, topology, `beginRings`, `beginDraws` (the state filter) |
| `frameSlot()` | The slot being recorded. | `g.slot` |
| `endFrame(FrameEnd)` | Last call. `pictures` holds at least one picture. False: the recording did not close. | `closeFrame`, then per picture a small list before (its constants and predicate value) and one after (its time stamp, the copy into a shared texture or a read-back, its shot); `replay::` submits extra pictures on a thread of its own; `pace::` hands pictures to the window at their time |
| `setFrameDone(fn)` | Set once. `fn(slot)` is called on the render thread. | `finishSlot` calls `g_frameDone` |
| `report(Report)` | The device's own log lines: a saved frame's ring use, the profile window's uploads and video memory. Optional. | `D3d12Device::report` |
| `vendorId()`, `videoMemory(budget)` | The GPU's PCI vendor id; its video memory in use or the budget. The clipping shader is off by default on AMD and at the two lower quality presets. | DXGI adapter queries |

### 4.2 Targets

A target is a colour surface with an optional depth surface. The frame builder keeps one per PS2 frame buffer it
draws into (keyed by the GS address); the picture is a target of its own.

| Call | Rules | D3D12 |
|---|---|---|
| `createTarget(TargetDesc)` | Formats: `BGRA8` for the PS2's frame buffers, `RGBA8`, `RG8`, `R8`, `R32Float` for the effects' targets. With `depth`, a 32-bit float depth surface that can also be sampled. With `shared`, the picture itself. Direct3D 12 hands out a new target zeroed (a committed resource); a Vulkan device should clear a new or resized target to zero too, so that a read before the first draw gives the same pixels. 0: failed. | `makeSurfaces` (colour, typeless depth with a depth view, a read-only depth view and a shader view; a render-target view; at most 64 targets) |
| `resizeTarget(id, TargetDesc)` | New surfaces, the same handle. Waits for the GPU to go idle. | `makeSurfaces` after `waitIdle` |
| `releaseTarget(id)` | Freed once the frames using it are done. | resources and views returned after the slot's fence |
| `clearColour(id, rgba)`, `clearDepth(id, value)` | The depth clear does nothing on a target without depth. | `ClearRenderTargetView`, `ClearDepthStencilView` |
| `use(id, UseFor)`, `useDepth(id, DepthUse)` | Move the colour or depth surface to a use: `Draw`, `Sample`, `CopySource`, `CopyDest`; depth `Write` or `ReadSample` (read-only depth while the shader samples it). No-op if already there. | `stateOf`, `depthStateOf`, `transition` (also noted in the journal for moment pictures) |
| `view(id, depth)` | The target (or its depth) as a sampled texture. | its shader view (`srvFor`) |
| `snapshot(id)` | A copy of the target as it is now, as a texture: a draw that reads the target it draws into. | `destCopyOf` |

The depth convention is the GS's: larger is nearer, depth is cleared to 0 and tested `GreaterEqual` or `Greater`.

### 4.3 Textures and meshes

| Call | Rules | D3D12 |
|---|---|---|
| `createTexture(w, h, Format, levels)` | `RGBA8` or `BC7` (the HD images). Texture 0 is a white 1 x 1 texture. | a placed resource in `pool::` heaps, a descriptor slot (8,192 in all) |
| `uploadTexture(id, level, texels, rowBytes)` | Levels given in order on this frame; the texture is usable once the last level is given. | through the staging ring (`g_texRing`), copies recorded on the frame's list |
| `uploadTextureLater(id, source, urgent)` | The texture is made and uploaded off the render thread when the device gets to it, its texels read from `source` then (gone by then: skipped). `urgent` goes before the large images. An RGBA8 source's mip chain goes down to 1 x 1: the levels it does not carry are box-filtered by the device (`frame::mipChain`) - the pictures depend on it (with the given levels alone, distant foliage read its sharp top level and came out brighter). | the uploader thread (`up::`) on a copy queue; `takeUploaded` writes its view at a frame's start |
| `textureReady(id)` | Its upload reached the GPU. Until then the frame builder draws the game's own texture or a stand-in. | |
| `releaseTexture(id)` | Freed once the frames using it are done. | |
| `textureSlotsFree()`, `textureBytes(id)` | Room for more textures (0: a new one is refused); a texture's video memory (the HD images' budget). | free descriptor slots; allocation size |
| `createMesh(vertices, bytes, indices, bytes)`, `releaseMesh(id)` | A game mesh kept across frames: `nr::Vertex` (40 bytes) and 32-bit indices. | an upload-heap buffer (`TF_NATIVE_MESHVRAM`: a video memory copy) |

### 4.4 State and draws

| Call | Rules | D3D12 |
|---|---|---|
| `setTarget(id, withDepth)` | Binds the target (and its depth unless it has none or `withDepth` is false). Forgets every filtered state. | `OMSetRenderTargets` |
| `setViewport`, `setScissor` | Pixels of the bound target. | |
| `setPipeline(PipelineDesc)` | The pipeline for a draw's state (blend, depth test, depth write, write mask, picture or not, clipping stage, pixel-shader variant, alpha-test combination), or an effect (`PipelineDesc::effect`). Made on first use. False: none could be made; the frame builder leaves the draw out. | `pipelineFor` (key `pipelineKey`), the pipeline library on disk (`plib::`), variants made on a thread (`psv::`) |
| `effect(Effect)` | The effect's pipeline exists; false: the frame builder leaves the pass out. | |
| `setBlendFactor(rgba)` | The GS's FIX. | |
| `bindTextures(t0, t1, t2)` | The three sampled slots: the draw's texture, the palette or a target's copy, the depth (the depth fog). `kKeepTexture` leaves a slot as it was. | descriptor tables 1, 2, 3 |
| `bindDrawConstants(span)` | The draw's constants (`b0`), a span from the Constants ring. | root CBV 0 |
| `setVertices(span, stride)`, `setIndices(span)`, `setMesh(id)`, `setPreviousVertices(span)` | Geometry: ring vertices or a kept mesh, ring indices, and the previous frame's vertices of a mesh made anew each frame (20 bytes each; an empty span: none). | vertex streams 0 and 2, the index buffer |
| `draw`, `drawIndexed` | Instances come from the Instances ring: `firstInstance` is `RingSpan::first`. | `DrawInstanced`, `DrawIndexedInstanced` |
| `copyWhole(dst, src)` | Same size and format. | `CopyResource` |

The device may leave out a state call equal to the last one (`TF_NATIVE_STATEFILTER`); on some GPUs each set is a
register write.

The **effects** are the passes that are not GS draws. Each is a full-screen triangle drawn with `draw(3, 0)` (no vertex
input) except `Cloud`. Their constants go through `bindDrawConstants`, their inputs through `bindTextures`:
`Fxaa`, `SmaaEdges`, `SmaaWeights` (the device binds SMAA's area and search tables itself), `SmaaBlend`, `Probe` (the
sun's occlusion probe: the scene's depth point-sampled into a 64 x 64 `R32Float` target), `Cloud` (the warp tunnel's
sparks: a strip of four vertices a point, the points per instance, added into the target), and the modern bloom's two
steps (an option, off by default):

- `BloomDown`: `t0` shrunk by two into the target with a 13-tap filter, written. Constants (b0, float4s): 1 / the
  target's width and height, 1 / `t0`'s width and height; the threshold's rgb, subtracted after the filter when its w is
  above 0. RGBA8.
- `BloomUp`: `t0` enlarged by two with a 3 x 3 tent, added to the target: target = up x gain x profile + target x the
  blend factor (`setBlendFactor`; the target's alpha is kept). Constants: the same two sizes; the gain's rgb; then eight
  floats, a weight profile down the target (linear between the centres of equal bands, top to bottom), and their count
  in the next float4's x (0: no profile). RGBA8. The frame builder uses the factor to weight each level it adds into,
  and 1 for the last step, into the picture.

A backend without them returns false from `effect()` for either; the frame builder then draws the game's own glow, as
it does when the option is off.

`ToneMap` (the filmic tone mapping, an option, off by default): the picture's copy (`t0`, a `snapshot`) through a tone
curve into the picture, its colour only (the picture's alpha kept). Constants (b0): the exposure, the inverse curve's k,
the strength (0: as it was, 1: the whole curve, faded out in the deepest shadows below sRGB 0.25), and the saturation
added after (0: none; the frame builder gives 0.3 x the strength). RGBA8. Recorded once a frame where the anti-aliasing is, just before
it. A backend without it returns false from `effect()`: the picture is drawn as it is.

`LocalContrast` (the local contrast, an option, off by default): the picture's copy (`t0`) with k times its luminance's
difference from a blur of it (`t1`, any size, sampled linear with `s4`; the difference held to +-0.2) added to each
channel, into the picture, its colour only (alpha kept). Constants (b0): 1 / the target's width and height, k. RGBA8.
The frame builder makes the blur with `BloomDown` (no threshold, from the picture straight to a quarter of its size,
then down to 1/16) and `BloomUp` (the blend factor 0: each step replaces the level above, back up to the quarter), and records it where the anti-aliasing is, before the tone mapping. A backend without
it returns false from `effect()`: the picture is drawn as it is.

### 4.5 Queries, statistics, predication, read-backs

| Call | Rules | D3D12 |
|---|---|---|
| `timestamps()`, `timestamp()`, `readTimestamps(slot, ticks, ticksPerSecond)` | Section 3. | one timestamp query heap, a part per slot, resolved at `endFrame` |
| `pipelineStats()`, `beginStats(entry)`, `endStats(entry)`, `readStats(slot, out)` | Profile and debug only (`TF_NATIVE_DRAWCOST` with `TF_NATIVE_DRAWSTATS`, `TF_NATIVE_PSTATS`). An entry begins at a draw and ends after it, on the same target. A backend may return false from `pipelineStats()`. | a statistics query heap made on the first ask |
| `predication()`, `predicate(skip)` | Moment pictures only, off by default (section 3). Draws only: copies and clears are never skipped (a predicated copy or clear hung an RX 5700 XT). A backend may return false from `predication()`. | `SetPredication` on the slot's 8 bytes of a predicate buffer |
| `readTarget(id)`, `readback(r, out)`, `releaseReadback(r)` | Section 3. `Readback` gives the data, width, height, bytes a row and format. The target keeps its state. | `CopyTextureRegion` into a read-back buffer, kept per slot |

### 4.6 The GPU grass

Optional. The grass on the ground is normally grown on the CPU (`nr_grass.cpp`). The Direct3D 12 backend can grow it
on the GPU with a compute pass bit-identical to the CPU's (`grassgpu::` in `nr_d3d.cpp`). `growGrass(frame)` records the
pass for the frame's grounds, `grassMesh(groundId)` tells whether a ground was grown so, and `drawIndirect(groundId,
firstInstance)` draws its blades with the counts the GPU wrote. A backend without it returns false from
`grassGpuReady()` (section 2) and the capture grows the grass on the CPU; these three calls are then never used.

## 5. Resources, shaders and bindings

**Formats.** `R8G8B8A8_UNORM` (textures, the picture), `B8G8R8A8_UNORM` (the PS2's frame buffers), `BC7_UNORM` (HD
images), `R32_FLOAT`, depth `D32_FLOAT` on an `R32_TYPELESS` resource (sampled by the depth fog), `R8G8_UNORM`,
`R8_UNORM` (SMAA), `R32_UINT` indices.

**Vertex streams.** Stream 0, per vertex: `nr::Vertex` - `POSITION` float3, `COLOR` RGBA8 unorm, `TEXCOORD` float2,
`NORMAL` float3, `PSIZE` float. Stream 1, per instance (the Instances ring, 208 bytes): `IROW0-3`, `IPZERO`, `IROWB0-3`,
`IROWM0-3`, each float4. Stream 2, per vertex (`setPreviousVertices`, 20 bytes): `POSB` float3, `COLORB` RGBA8 unorm,
`PSIZEB` float.

**Binding layout** (the D3D12 root signature):

| Slot | What | Set by |
|---|---|---|
| `b0` | the draw's constants (`draw::Constants`) | `bindDrawConstants` |
| `t0` | the draw's texture | `bindTextures` |
| `t1` | the palette, a target's copy, or SMAA's area table | `bindTextures` |
| `t2` | the depth read (the fog, the probe), or SMAA's search table | `bindTextures` |
| `b1` | the picture's constants (`draw::PictureConstants`) | the device, from `PictureOut::constants` |
| `s0` | anisotropic, wrap (4x to 16x by the quality preset) | static |
| `s1` | trilinear | static |
| `s2` | point with linear mips (the GS's nearest filter with TEX1's level) | static |
| `s3` | anisotropic at a test setting | static |
| `s4`, `s5` | linear and point, clamped, no mips (SMAA) | static |

**Shaders.** HLSL, as string literals in `nr_d3d.cpp`: `kShader` (the GS draws: vertex shader `vs`, pixel shader `ps`,
the clipping geometry shader `gsClip`), `kAaShader` (FXAA), `kSmaaPrelude` and `kSmaaEntries` (SMAA 1x),
`kProbeShader`, `kCloudShader`, `kBloomShader` (the modern bloom's two steps), `kToneShader` (the tone mapping), `kClarityShader` (the local contrast), and the grass compute shader. They are compiled at start with `D3DCompile` at Shader
Model 5.1 (`vs_5_1`, `ps_5_1`, `gs_5_1`; the grass `cs_5_0`). The pixel shader has variants: the `PSV_` defines compile
only the paths a draw uses (`PipelineDesc::variant`); until a variant is ready the device draws with the whole shader,
which gives the same pixels. Compiled shaders and pipelines are kept on disk (`cache::`, `plib::`) and warmed at start.

**Pipelines** come from `PipelineDesc`: the blend (the GS's ALPHA register as source and destination factors and an
operation, or a product done in the shader where the GS's factor goes past 1.0), the depth test and write, the colour
write mask (FBMSK), the clipping stage, the target's format. Triangle lists everywhere except `Cloud` (strips).

**Behaviour to keep exactly**, because the pictures depend on it: the blend products done in the shader, the depth
direction and the GS's depth as written by the shader, the filtering per TEX1 (which sampler, which mip level), the
order of draws within a target, the copies between targets where the game reads one it drew, and the sampler set.

## 6. What a Vulkan device replaces

| Direct3D 12 | Used for | Vulkan |
|---|---|---|
| Root signature, descriptor heaps (8,192 shader-visible texture slots, render-target and depth views) | every draw's bindings | One descriptor set layout for section 5's slots. For the textures: a large sampled-image array with descriptor indexing, or a descriptor set per bound combination. `b0` as a dynamic uniform buffer (or a push constant with its ring offset). |
| Static samplers | `s0` to `s5` | Immutable samplers in the layout. |
| HLSL compiled at run time with `D3DCompile` (SM 5.1) | all shaders | The same HLSL compiled to SPIR-V with DXC (section 8). One source for both backends. |
| Geometry shader (`gs_5_1`) | the PS2's triangle clipping | Geometry shaders (a desktop feature). The clipping stage is already off by default on AMD (`vendorId() == 0x1002`) and at the two lower quality presets, so a backend can start without it. |
| Compute shader | the GPU grass | A compute pipeline, or leave the grass on the CPU (section 4.6). |
| Resource states and transition barriers | every target and texture | Image layouts and pipeline barriers, tracked per image as `stateOf` does. |
| Render target and depth views, `OMSetRenderTargets` | `setTarget` | Dynamic rendering (`VK_KHR_dynamic_rendering`, core in 1.3), or a render pass begun at `setTarget` and ended before a copy, clear or barrier. |
| Typeless depth with a depth view and a float view | the depth fog, the probe | A `D32_SFLOAT` image with `SAMPLED` usage, a depth view, and `DEPTH_STENCIL_READ_ONLY_OPTIMAL` (or the general layout) while it is sampled. |
| `ClearRenderTargetView`, `ClearDepthStencilView` | the clears | `vkCmdClearColorImage` / `vkCmdClearDepthStencilImage` outside rendering, or `vkCmdClearAttachments` inside. |
| `CopyResource`, `CopyTextureRegion` | `copyWhole`, `snapshot`, read-backs | `vkCmdCopyImage`, `vkCmdCopyImageToBuffer` with the layouts moved around them. |
| Upload heaps, a copy queue | rings, uploads | Host-visible buffers (device-local where the GPU offers it); a transfer queue with queue ownership released to the graphics queue, or uploads on the graphics queue. |
| Pipeline library on disk (`ID3D12PipelineLibrary`) | start-up time | `VkPipelineCache` saved to disk. |
| Timestamp and pipeline-statistics query heaps | the profile | Query pools: `VK_QUERY_TYPE_TIMESTAMP` (ticks per second = 1e9 / `timestampPeriod`), `VK_QUERY_TYPE_PIPELINE_STATISTICS` (optional feature). Reset the slot's queries at `beginFrame`, outside rendering. |
| Predication (`SetPredication`) | moment pictures' skipped draws | `VK_EXT_conditional_rendering`, or return false from `predication()`. |
| One command list submitted once per picture | moment pictures | Re-submitting a command buffer after its fence (or with `SIMULTANEOUS_USE`); the restore list becomes layout transitions back. |
| `ExecuteIndirect` | the GPU grass | `vkCmdDrawIndexedIndirect`. |
| Fences, `waitFence` | frame slots | Timeline semaphores or fences per slot. |
| BC7 textures | HD images | Desktop GPUs have BC7. Elsewhere decompress to RGBA8. |
| Shared textures with NT handles | the hand-over to the window (section 7) | External memory, or present from the backend's own swapchain. |
| Device removed, DRED breadcrumbs | a GPU hang: back to the emulated picture | `VK_ERROR_DEVICE_LOST`; breadcrumbs with `VK_NV_device_diagnostic_checkpoints` or `VK_AMD_buffer_marker` where available. |
| Feature level 11.0 device | the device | Vulkan 1.1 or later. |

## 7. The window and the present seam

**Today.** The window is the runtime's raylib window (OpenGL), made by the PS2 runtime (`ps2_host_display`). It shows
either the emulated GS's picture or, through a picture provider (`nr_present.cpp`), the native one. The window draws
its overlays (the FPS counter, the settings panel) over whichever picture it shows. The emulated renderer still draws
some screens (the menus and movies), so the window must keep both paths.

The native picture reaches the window in one of two ways, chosen at start:

- **Shared textures** (the default where the driver can): the backend copies each finished picture into one of
  `kSharedPictures` (24) shareable `RGBA8` textures with NT handles. The window imports them into OpenGL with
  `GL_EXT_memory_object_win32` (`nr_glshare.cpp`) and draws the one handed over (`latestShared`). The backend never draws
  into a texture the window holds (`sharedHeld`) or the newest one handed over.
- **CPU pixels**: the picture is read back and the window uploads it: on Linux straight from the mapped read-back
  (`latestPicture`, then `pictureShown`), one hand-over of four in turn so a newer picture never replaces one
  waiting, presented as it is handed over, paced like the shared textures; on Windows as 0.6 does, the newest
  picture copied into a vector. This is the path on Linux, the fallback when the import fails (for example under
  Wine/Proton), and with `TF_NATIVE_SHARE=0`.

The pacer (`pace::` in `nr_d3d.cpp`, `waitPicture` on the window's side) hands each picture over at its due time
(`PictureOut::dueNs`) plus a delay that follows the GPU's times, so pictures reach the screen evenly.

**With SDL3 and a swapchain.** A Vulkan backend can present straight to a swapchain on an SDL3 window. Then:

- `endFrame` copies (or draws) the picture into the swapchain image instead of a shared texture, and presents it at
  its due time; the pacer's job moves to the present (FIFO or mailbox present modes, or `VK_KHR_present_wait`).
- The overlays have to be drawn by the new window layer (or into the picture before present), since raylib no longer
  draws over it.
- The screens the emulated renderer draws still need a path to the window: its picture as CPU pixels uploaded into a
  texture and drawn by the same present.
- `latestPicture` and the shared-texture calls are then unused by the window, but the CPU read-back stays useful for
  shots and checks.

**HDR output** needs this swapchain: the raylib/OpenGL window and its 8-bit shared textures cannot show HDR, so today
only the tone mapping's SDR part exists (`ToneMap`). For an HDR mode:

- A swapchain in an HDR colour space: `VK_EXT_swapchain_colorspace` with `VK_COLOR_SPACE_HDR10_ST2084_EXT` on an
  `A2B10G10R10` image (HDR10, PQ), or `VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT` on an `R16G16B16A16_SFLOAT` image (scRGB);
  `VK_EXT_hdr_metadata` for the display's peak brightness. On Windows the display's HDR switch must be on; ask the
  driver which colour spaces the surface offers and fall back to SDR when none is HDR.
- A wider picture: the frame builder's picture and the tone mapping's output in `RGBA16F` (a new `Format`), the
  `ToneMap` effect's curve replaced in HDR by a mapping to the display's range (the same inverse curve to linear, then
  scaled to paper white, about 200 nits, with the highlights up to the display's peak; PQ encoded for HDR10).
- The window's overlays and the emulated renderer's screens drawn into the HDR picture at paper white.
- A setting for paper white and peak brightness, since displays differ.

The hand-over is the part of the backend that is least separated today: the window's side lives in `nr_present.cpp`
and `nr_glshare.cpp`, the calls go through `nr::backend()`, and changing it is part of the SDL3 work.

## 8. Build integration

- **CMake.** `transformers/CMakeLists.txt` picks up every `native_render/*.cpp` with a glob, and links `d3d12`, `dxgi`
  and `d3dcompiler` on Windows. A Vulkan backend adds its files (for example `nr_vulkan.cpp`), links the Vulkan loader
  (`find_package(Vulkan)`), and selects the backend at start (section 2). `nr_d3d.cpp` should only build on Windows.
- **Shaders today.** HLSL source as string literals in `nr_d3d.cpp`, compiled at run time by `D3DCompile` and cached on
  disk by the hash of source, entry, target and defines.
- **The SPIR-V route.** Move the HLSL into files shared by both backends (`.hlsl`), and compile them for Vulkan with DXC:
  `dxc -spirv -T ps_6_0 -E ps -D PSV_TEX=1 ... -fvk-use-dx-layout`, with `-fvk-b-shift`, `-fvk-t-shift` and `-fvk-s-shift`
  (or `[[vk::binding]]` attributes) to map `b0`, `t0..t2`, `b1` and `s0..s5` into one descriptor set. Compile at build
  time (a CMake custom command, the SPIR-V embedded as arrays) or at run time with the DXC library. The pixel shader's
  variants are `PSV_` defines: either compile the variants a run asks for, or a fixed list, or the whole shader only
  (`PipelineDesc::variant` 0 gives the same pixels, only slower).
- The public build (`build.ps1 -Public`) builds the same renderer files; keep it building.
- **The Vulkan backend's build** (`TF_VULKAN`, a CMake option, off by default while it is in development:
  `build.ps1 -Vulkan` or `-DTF_VULKAN=ON` turns it on, and the build folder keeps it): the Vulkan headers and the volk loader
  (`vulkan-sdk-1.4.363.0`) and SDL3 (`release-3.4.18`, static) are fetched at configure time like raylib; volk loads the
  system's Vulkan at run time, so building needs no Vulkan SDK. The backend (`native_render/vulkan/`, outside the
  `native_render/*.cpp` glob) is a static library of its own, `tf_vulkan_backend`, that links them privately: their
  include folders and defines stay on its files, so the game's other files keep their command lines (no full recompile
  when switching between a build with and without it). Off (`-DTF_VULKAN=OFF`, the default) builds Direct3D 12 alone and
  fetches nothing.
  Its draws also fetch the Vulkan Memory Allocator (`v3.4.0`, its header) and, on Windows, DXC's release (`v1.9.2609`,
  checked by its SHA-256; the Windows SDK's DXC has no SPIR-V), which only the build uses: a small program of the build's
  own (`native_render/vulkan/bake_spirv.cpp`, target `tf_vulkan_bake`) loads its `dxcompiler.dll` and compiles every
  shader the backend makes, the pixel shader's 64 variants included, into a table of SPIR-V words built into the game
  (`nr_spirv_baked.cpp` in the build folder, read through `nr_vulkan_shaders.h`). CMake runs it again whenever the HLSL
  changes, so every build path (build.ps1, `cmake --build`, the IDE) has it, and the game needs no shader compiler and no
  `dxcompiler.dll`. Each shader is found by a key over DXC's switches, the HLSL, entry point, profile and defines, so a
  shader the bake's list lacks is reported at start rather than taken stale. Until the HLSL moves into files of its own,
  the build takes it from `nr_d3d.cpp`'s string literals into a generated header (`native_render/vulkan/extract_hlsl.cmake`,
  looked at again when `nr_d3d.cpp` changes and written only when its shaders did), so both backends draw with one source. For the same pixels as `D3DCompile`'s, DXC is given `-HV 2018` (the language as D3DCompile
  reads it), `ddx`/`ddy` as `ddx_coarse`/`ddy_coarse` (D3DCompile's meaning of them), and Vulkan 1.3 as its target, so
  that `discard` demotes the pixel to a helper as Direct3D's does rather than ending it (`OpKill` left its 2 x 2
  neighbours' derivatives - their texture levels - undefined).

## 9. The acceptance test

A new backend is done when it draws the same pictures as the Direct3D 12 one on the check routes, and its frame rate
is in the same range on the same PC.

- **Routes, from save states.** Make the states from your own disc with `tools/make_check_states.ps1` (an HQ room, a
  Mini-Con screen, three places in Amazon Basin; the same states as the cases of `tools/check_frames.ps1`). Play each from its state with `PS2X_LOAD_STATE`, the game's movies
  skipped (`TF_SKIP_FMV=1`), invulnerable (`TF_GOD=1`), on a fresh memory card, and save pictures at fixed VBlanks with
  `TF_NATIVE_SHOT=<tick>,...` (they land in `runs/native_<tick>.bmp`). Include a run with SMAA (`TF_NATIVE_AA=2`), one
  where the sun is on screen (turn the camera with `TF_INPUT` and log the probe with `TF_NATIVE_OCCLOG=30`), and one
  with the HQ menus.
- **Repeatable settings.** A run is repeatable only where nothing depends on timing: use `TF_HIRES=0` (no HD images,
  which load at disk speed), a fixed frame rate (`TF_FRAME_RATE=60`), and a hidden window. Run the reference backend
  twice first: a picture that differs between two reference runs is not a test.
- **Comparison.** Where the reference repeats itself, the pictures must be identical bit for bit between the two backends
  on the same GPU, or within rounding: `tools/native_compare.py <reference> <candidate> --max-mean 0.5 --max-flat16 0.05
  --min-ssim 0.995` (it compares folders of pictures, one folder per case), and every difference over that looked at with
  `--heatmaps`. A missing draw or a wrong blend shows as a difference on a flat area. The probe's log lines
  (`[occlusion] ... native frame ... z <lo>..<hi>`) must match too.
- **Speed.** The same routes at a 120 fps cap with `TF_FPS_CAP=120` and `TF_NATIVE_PROFILE=1`: the pictures a second and
  the GPU time per frame within 10% of the Direct3D 12 backend on the same PC, run alternately (a GPU's clocks drift
  between runs).
- **GPUs.** At least one AMD and one NVIDIA GPU. Drivers differ: a Direct3D 12 change once passed on NVIDIA and crashed
  AMD's driver (a depth clear through a stale view), and predicated copies hung an RX 5700 XT.

## 10. Where to start

A first Vulkan device can leave out everything optional and still draw the game:

1. The outer calls through `nr::Backend` and a backend chosen at start (section 2).
2. The device, the swapchain or shared-texture hand-over, `prepareFrame`, `beginFrame`, `endFrame` with one picture.
3. Targets, textures (`uploadTexture`; `uploadTextureLater` can upload at the next frame start at first), meshes, rings.
4. The pipelines from `PipelineDesc`, the binding layout, the HLSL as SPIR-V, the draws, `copyWhole`, `snapshot`.
5. `readTarget` (shots and the probe) and `timestamp` (the profile).

Optional, each with a way to say no: the GPU grass (`grassGpuReady` false), pipeline statistics (`pipelineStats`
false), predication (`predication` false), the clipping geometry shader (off on AMD already), extra moment pictures
(`TF_POSES` off by default), the modern bloom (`effect(BloomDown)` false: the game's glow is drawn), the tone mapping
(`effect(ToneMap)` false), the local contrast (`effect(LocalContrast)` false), `report`.

## Appendix: how the split was made

The interface was cut out of a single Direct3D 12 renderer in steps, each checked to give the same pictures and the
same frame rate as before. S1 named the neutral types (`nr_backend.h`). S2 moved the GS state's translation into
`nr_draw.h` and `nr_draw.cpp`. S3, in six sub-steps, moved every call the frame builder makes behind `gfx::Device`:
the targets; the textures, meshes and rings; the state and draws; the copies, the effects and the anti-aliasing; the
queries, statistics, predication and read-backs (the occlusion probe became an effect read back through `readTarget`);
and finally the frame's life (`prepareFrame`, `beginFrame`, `endFrame`), after which the frame builder moved into
`nr_frame.cpp`. The callers then moved to `nr::backend()`, and the hand-over followed: Linux hands over its mapped
read-back in turn, keeping each picture until the window has uploaded it, while Windows keeps 0.6's copied newest
picture. The window's own side stayed in `nr_present.cpp` and `nr_glshare.cpp` (section 7).
