# Changes to PS2Recomp

PS2Recomp (the `PS2Recomp/` submodule, pinned at upstream commit `c5a9d02`) is both the
recompiler and the runtime that stands in for the console. Transformers needed fixes in both
and a good deal of new code: a VU1 engine with a JIT, VIF1 and VU1 on a thread of their own,
a threaded software renderer, the sound processor, save states. All of it is kept as patches
in `patches/ps2recomp/`, which `build.ps1` applies to the submodule (`build.ps1 -NoPatches`
leaves the submodule alone, for working on it directly).

Related: what the game itself does is in [NOTES.md](NOTES.md), the options and tools used to
find all of this in [DEBUGGING.md](DEBUGGING.md), where things stand in [STATUS.md](STATUS.md).

- [The patches](#the-patches)
- [Where the code is](#where-the-code-is)
- [Fixes, in the order they were found](#fixes-in-the-order-they-were-found)
- [Console arithmetic](#console-arithmetic)
- [VU1](#vu1)
- [VIF1 and VU1 on their own thread](#vif1-and-vu1-on-their-own-thread)
- [The GS renderer](#the-gs-renderer)
- [Sound](#sound)
- [Timing](#timing)
- [Host input](#host-input)
- [Save states](#save-states)
- [Speed history](#speed-history)
- [Building](#building)

## The patches

The submodule's working tree is where the work happens; the patch files are generated from
it and must be regenerated before every commit of this repository:

```powershell
python tools\make_patches.py --verify
```

This writes one patch per area from the difference between the pinned commit and the working
tree, new files included, then checks out the pinned commit into `out\patch_verify`, applies
the patches there and compares every changed and new file with the working tree. It reads
the submodule without touching its files or its index.

| Patch | Covers |
|---|---|
| `0001-iop-emulator` | `ps2xIOP/`: SIF commands, one memory pool, module loading, `sprintf`, RPC servers on their threads, the SPU2, the EE/IOP cycle balance, save state hooks |
| `0002-recompiler-fpu-and-vu0-translation` | `ps2xRecomp/`: FPU and VU0 macro-mode translation, the register cache, re-entry after `jalr`, pc stores |
| `0003-vu1-fast-engine-and-jit` | `ps2xRuntime/src/lib/vu/` |
| `0004-gs-threaded-software-renderer` | `ps2xRuntime/src/lib/gs/`, `include/runtime/gs/` |
| `0005-runtime-scheduler-hle-save-states` | The rest of `ps2xRuntime/`: console arithmetic, scheduler, HLE stubs (MPEG, varargs), DMA/VIF1 and its thread, host audio, save states, CMake |

The patches touch disjoint files and can be applied in any order. `build.ps1` skips a patch
that is already applied.

## Where the code is

Paths are relative to `PS2Recomp/`.

| Area | Files |
|---|---|
| Recompiler: FPU and VU0 macro-mode translation | `ps2xRecomp/src/lib/fpu_translator.cpp`, `vu_translation_helpers.cpp`, `vu_translator.cpp` |
| Recompiler: registers in locals, re-entry after `jalr`, pc stores ([The generated code](#the-generated-code)) | `ps2xRecomp/src/lib/register_cache.cpp`, `control_flow_analyzer.cpp`, `function_emitter.cpp`, `control_flow_emitter.cpp`; the macros in `ps2xRuntime/include/ps2_register_cache.h` |
| Console arithmetic used by the generated code | `ps2xRuntime/include/ps2_runtime_macros.h` |
| EE scheduler: guest threads, events, VBlank pacing | `ps2xRuntime/src/lib/Kernel/EeScheduler.cpp`, `include/runtime/ee_scheduler.h` |
| Host input: gamepad, keyboard, mouse | `ps2xRuntime/src/lib/ps2_pad.cpp`, `include/runtime/ps2_host_input.h`; the window loop in `ps2_runtime.cpp` |
| From the VBlank to the screen: latched pictures, field mode, the screen's refresh | `ps2xRuntime/src/lib/ps2_host_display.cpp`, `include/runtime/ps2_host_display.h`; the latch in `EeScheduler.cpp`, the window loop in `ps2_runtime.cpp` |
| HLE of the Sony libraries | `ps2xRuntime/src/lib/Kernel/Stubs/` (`GS.cpp`, `MPEG.cpp`, `SIF.cpp`, `CD.cpp`, `Pad.cpp`, `MemoryCard.cpp`, `DMA.cpp`, `Audio.cpp`) and `Kernel/Syscalls/` |
| DMA and VIF1 | `ps2xRuntime/src/lib/ps2_memory.cpp`, `ps2_vif1_interpreter.cpp` |
| VIF1, VU1 and the GS front end on their own thread | `ps2xRuntime/src/lib/ps2_vif1_thread.cpp`, `include/runtime/ps2_vif1_thread.h` (hooked into `ps2_memory.cpp`, `ps2_vif1_interpreter.cpp`, `gs_frontend.cpp`, `ps2_runtime.cpp`) |
| VU1, reference interpreter | `ps2xRuntime/src/lib/vu/ps2_vu1_core.cpp`, `ps2_vu1_upper.cpp`, `ps2_vu1_lower.cpp` |
| VU1, fast engine and JIT | `ps2xRuntime/src/lib/vu/ps2_vu1_fast.cpp`, `ps2_vu1_fast.h`, `ps2_vu1_jit.inl` |
| GS registers, GIF, command stream | `ps2xRuntime/src/lib/gs/gs_frontend.cpp`, `ps2_gif_arbiter.cpp`, `ps2_gs_memory.cpp`, `include/runtime/gs/gs_draw_state.h` |
| GS thread | `ps2xRuntime/src/lib/gs/gs_threaded_backend.cpp`, `include/runtime/gs/gs_threaded_backend.h` |
| Sound | `ps2xIOP/src/emulator/core/iop_spu2.cpp` (the SPU2), `iop_memory.cpp` (its registers and DMA), `iop_emulator.cpp` (when it runs); `ps2xRuntime/src/lib/ps2_audio_stream.cpp` (host playback), `ps2_iop_host.cpp` |
| Movies (libmpeg) | `ps2xRuntime/src/lib/Kernel/Stubs/MPEG.cpp` |
| GS software renderer | `ps2xRuntime/src/lib/gs/gs_cpu_backend.cpp` (draw state, textures, set-up, presentation), `gs_raster.h` (data structures and the scalar loops), `gs_raster_avx2.cpp` (the same loops, eight pixels at a time) |
| GS on the GPU (the default) | `ps2xRuntime/src/lib/gs/gs_gpu.cpp` (buffers, local memory on the GPU, batches, presentation), `gs_gpu_shaders.h` (the pixel pipeline and the GS swizzle in GLSL), `include/runtime/gs/gs_gpu.h`; fed by `gs_threaded_backend.cpp`, shown by the window loop in `ps2_runtime.cpp` |
| Widescreen: the TV setting, the picture's shape, 2D narrowed | `ps2xRuntime/src/lib/Kernel/Syscalls/Helpers/Runtime.h` (`GetOsdConfigParam`), `ps2_host_display.cpp`, `gs_frontend.cpp` (`scanOverlayTag`, `squeezeOverlay`), `ps2_gif_arbiter.cpp` (each packet's path); the game's side in `transformers/src/overrides.cpp` |
| Save states | `ps2xRuntime/src/lib/ps2_savestate.cpp`, `include/runtime/ps2_savestate.h`, `Kernel/Stubs/SaveState.h`, `ps2xIOP/include/ps2x/state_stream.h` |
| The settings panel (F5) and `settings.ini` ([The settings panel](#the-settings-panel)) | `ps2xRuntime/src/lib/ps2_settings.cpp` (the settings, the file, the restart), `ps2_settings_panel.cpp` (the panel, the runtime's settings), `include/runtime/ps2_settings.h`; the window loop in `ps2_runtime.cpp`; the game's settings in `transformers/src/overrides.cpp` (`addSettings`, `takeSettings`) |
| IOP emulator | `ps2xIOP/src/emulator/` (kernel, memory, imports, module loader, RPC: server functions run on their own server thread, `services/iop_rpc.cpp` and `iop_emulator.cpp`) |

Game-specific hooks are not in the submodule: they live in `transformers/src/overrides.cpp`.
Decompiled game functions, which take the place of recompiled ones through the same function
table, are in `transformers/decomp/` ([its README](../transformers/decomp/README.md)): the
layer itself in `decomp.h`/`decomp.cpp`, a folder per part of the game.

## Fixes, in the order they were found

| Problem | Fix |
|---|---|
| `sceSifLoadModuleBuffer` read the IRX from EE RAM; the SDK passes an IOP address | Game override stages the image from IOP RAM (`transformers/src/overrides.cpp`) |
| Loading a module was instant, so the EE never saw the module's SIF commands before continuing | Override yields to the EE scheduler after the load, like waiting on the RPC reply |
| IOP ownership check rejected packets/buffers on the synthetic start-routine stack | Mark that stack as owned IOP RAM |
| EE SIF command handlers ran with `$gp = 0` | Capture `$gp` at `sceSifAddCmdHandler` |
| Start routines could not sleep while waiting for CD callbacks | `GetThreadId` returns a stand-in id outside threads; `SleepThread` there runs the next pending callback |
| EE → IOP SIF commands were never delivered | IOP records `sceSifSetCmdBuffer` / `sceSifAddCmdHandler`; EE `sceSifSendCmd` delivers the packet |
| EE `sceSifSendCmd` read arguments 5-6 from the stack | EE ABI passes them in `$t0`/`$t1` |
| `(i)sceSifSendCmdIntr` (sifcmd 28/29) unimplemented | Send + scheduled completion callback |
| A callee yielding at a loop back to its own first instruction looked like a finished call | Scheduler flags checkpoint unwinds; `dispatchGuestBranch` respects it |
| IOP RAM split into a fixed module area and an 832 KB heap; TwoFish's 512 KB cache left no room for SIRENS3 | One pool from `0x10000` for modules and `AllocSysMemory`, first-fit so freed blocks are reused |
| COP0 Count never advanced, so the game's `mfc0 $9` timing loops spun forever | The scheduler publishes its EE cycle counter into the running context's Count |
| Async callback stacks (SIF/interrupt handlers) were carved from the top of RAM, on top of the main thread's stack | Put them in kernel RAM (`0x20000`–`0x80000`), where hardware runs these handlers |
| An IOP SIF command handler could be interrupted by callbacks it scheduled itself (CD completion), which desynced TwoFish's request queue: it later re-sent an old 32 KB chunk into freed EE memory and corrupted the game's heap | Deliver EE → IOP commands with nested callbacks held off, like a real interrupt handler |
| A checkpoint at a function's return left the "unwinding" flag set | Clear it whenever a call completes normally |
| The display list ends with a VIF1 `NOP` carrying the i-bit; the game waits for the INTC VIF1 handler (`0x260090`), which checks `VIF1_STAT.INT` and `[D1_MADR] == 0xAAAAAAAA` | The VIF1 interpreter raises INTC VIF1 on the i-bit; chain DMA leaves `D1_MADR` where hardware does |
| GIFtags with FLG 3 were dropped | The GIF treats FLG 3 as IMAGE, like hardware |
| The IOP was stepped for every EE cycle batch, ~15% of EE time | Batch at least 8192 EE cycles per IOP run |
| **Every texture was shifted by 8 bytes.** The game sends images over PATH2 as `DIRECT 1` (GIFtags) then `NOP; DIRECT n` (pixels) in the DMA tag's upper half. After a GIFtag announced an IMAGE, the VIF1 interpreter took the following raw bytes as pixels, so the `NOP; DIRECT n` codes became the first 8 bytes of the image and its last 8 bytes were parsed as VIF codes (the "VIF1 reserved lower" XGKICK warnings) | VIF codes are always parsed; data of later `DIRECT` commands continues the pending IMAGE, and `VIF1_NUM` tracks DIRECT data that spans transfers |
| Software rasterizer ran the whole GS pipeline per pixel through generic helpers (9–12 fps at the title screen, 2 Mpixels a frame) | New rasterizer path next to it (see [The GS renderer](#the-gs-renderer)): 30 fps there, the game's cap |
| **EE stores to VU0 micro memory (`0x11000000`) and VU1 data memory (`0x1100C000`) landed in EE RAM.** `Ps2IsSpecialAddress` only covered `0x11004000`–`0x1100BFFF`, so the fast path masked the rest to `0x01000000`+. After an FMV the game restores VU1 memory with `memcpy(0x11008000/0x1100C000, saved, 0x4000)` (`sub_0026ED80`), which overwrote 16 KB of heap and crashed the level load on a freed-looking `vlMeshSurface` | Treat all of `0x11000000`–`0x1100FFFF` as special (inlined into generated code: needs a full rebuild) |
| **No 3D model was drawn: every camera and model matrix was skewed.** The VU1 renderer culled every triangle (clip-space `z` beyond `w`). The matrices came from quaternions of length ~19 because of three FPU translation bugs: `sqrt.s fd, ft` was translated as `fd = sqrt(fs)` (the R5900 encodes the operand in `ft`, so every square root took `$f0`); `cvt.w.s` rounded to nearest where the R5900 truncates, which broke the game's angle wrap (`sub_001AD450`, fractional part via `cvt.w.s`) and pushed its sin/cos polynomial out of range; `rsqrt.s fd, fs, ft` was `1/sqrt(fs)` instead of `fs/sqrt(ft)` | Recompiler: `sqrt.s` reads `ft`, `rsqrt.s` is `fs/sqrt(|ft|)`, `div.s`/`rsqrt.s` by zero give ±FLT_MAX like the R5900; runtime: `FPU_CVT_W_S` truncates and saturates, `FPU_SQRT_S` takes `sqrt(|x|)` |
| VU0 macro-mode translations: `VSQI`/`VSQD` swapped data and address registers; `VLQI`/`VSQI`/`VLQD`/`VSQD`/`VILWR`/`VISWR` accessed EE RAM at `vi*16` instead of VU0 data memory; `VCALLMSR` read `vi[27]` (past the 16-entry array) instead of CMSAR0; `VRSQRT` ignored its numerator; `VDIV` by zero gave 0; `VCLIP` stored -x/+x flags in swapped bits and compared against `w` instead of `|w|` | Fixed in `ps2xRecomp` (`vu_translation_helpers.cpp`, `vu_translator.cpp`) |
| **3D scenes ran at ~19 fps and in slow motion (~37 VBlanks/s).** VU1 microcode took ~13 ns per instruction pair in the interpreter, ~75% of the EE thread | VU1 JIT (`ps2_vu1_jit.inl`, x86-64 AVX via xbyak): basic blocks compiled to native code with the pipeline model resolved at compile time; ~2.9 ns per pair, verified bit- and cycle-exact against the reference (`PS2X_VU1_VERIFY=1`). Decoded and compiled code is cached per VU1 code image (the game swaps microprograms ~70×/s between 9 images) and MPG uploads of identical code no longer invalidate it. Picker: 30 fps at 60 VBlanks/s |
| After a slow stretch the game ran faster than real time until it had caught up | VBlank pacing restarts from "now" once it lags more than two VBlanks |
| **Yellow/green full-screen flashes and flicker in the Autobot picker.** The game rotates two buffers: page 112 is the depth buffer while a frame is drawn, then receives the finished image and is displayed while page 224 becomes the next depth buffer. The host window read the display registers, then waited for the GS thread and read VRAM - by then the EE had flipped and drawn the next frame's depth into the buffer it was about to show | The EE scheduler presents at every VBlank through the GS command queue (`GS::latchHostPresentationFrame` → `GSThreadedBackend::QueuePresent`); the GS thread builds the image at exactly that point of the stream and the window shows the newest one |
| The 512-pixel-wide frame was shown pixel for pixel between black bars | The window is 640×480 and the frame fills a 4:3 box like on a TV (`PS2X_ASPECT`) |
| **The Drop Zones room hung on its warp gate video.** The movie player (`0x275A00`) calls `sceMpegGetPicture` (`0x133F40`) once per frame and only demuxes more data afterwards; the HLE waited inside `sceMpegGetPicture` for a picture that only that loop could feed | `ps2_stubs::setMpegPictureNeverBlocks(true)` (set by `overrides.cpp`): `sceMpegGetPicture` returns at once, repeating the last picture, and fails only once the stream has ended. The player treats a failure as the end of the video when at least one picture was shown and the reported size matches the sequence header (`[s1+0xC8]`/`[s1+0xCC]`; `[s1+5]` is its loop flag); any other failure goes to the fatal error screen (`0x106620` → `0x255E90` → `0x255F90`) |
| The same video's display list never ended: its REGLIST triangles got DMA tags of twice their size, the chain ran into vertex data and the VIF1 end-of-list interrupt never found its `0xAAAAAAAA` marker | Sony's packet builders (`sceVif1Pk*`/`sceGifPk*`) only move pointers and write tags in guest memory, and the game runs most of that library as its own code. The four the analyzer bound to HLE versions (`sceGifPkCloseGifTag`, `sceVif1PkOpenDirectCode`, `sceVif1PkCloseGifTag`, `chaGifPkOpenGifTag2`) kept their own idea of the packet: `remove_stubs` in `config/overrides.toml` drops them (the recompiler binds by name, so `find_functions.py` leaves those functions unnamed) |
| IOP `sprintf`/`vsprintf` copied the format string without its arguments: strings built by IOP modules were wrong (an IOP crash on the way into a level) and the IOP log was unreadable | Real formatting for `sysclib` `sprintf`/`vsprintf`, `Kprintf` and `stdio` (`iop_printf.h`); `look_ctype_table` and `strtok` implemented |
| **EE crash to PC `0xC` while loading a level.** `find_functions.py` cut a function where a `jr` was followed by a jump target: the new function started in the delay slot, so the first one lost its stack restore and returned through a stale frame | `looks_like_boundary` never splits between a jump and its delay slot (only `eret` has none); `functions()` warns about any function that ends on a jump without its slot |
| In-game frames were cut off: the VIF1 display list has more than 4096 DMA tags | Chain DMA walks up to 2^20 tags / 256 MB |
| Reaching a level took two minutes of boot and menus for every test | [Save states](#save-states) |
| A VU1 microprogram was cut after 65,536 cycles (the level's grass batches run longer). The next `MSCNT` resumed it after the VIF had unpacked the next batch over its buffers, which corrupted a GIF tag: `[VU1 fast reserved lower] pc=0x1ff0 instruction=0xfffffffb` (the XGKICK "packet does not fit" code) several times a frame | Programs run to their E bit (`kVu1ProgramCycleLimit`, 64M cycles); `MSCNT` counts as a program start |
| **The 3D view of a level was black.** The game draws a frame as two half-height fields and every vertex of the second one came out of VU1 as zero: its matrix was all NaN. Matrix rows carry garbage in their unused `w` lane (`0xFFE0E001` here). The R5900 FPU and the VUs have no NaN or infinity - an exponent of 255 is a large number, results saturate and `0 * x` is 0 - but the recompiled code used host IEEE arithmetic, where that word is a NaN that spreads through every product (`sub_001B0D50`, the 4x4 multiply) | `ps2_runtime_macros.h`: operands and results of FPU and VU0 macro arithmetic are clamped to +/-FLT_MAX with the sign kept (`ps2_vclamp`); comparisons order the bit patterns (nothing is unordered); `MAX`/`MINI` pass bits through; `DIV`/`SQRT`/`RSQRT` and `FTOI` follow the console (helpers in the same header, emitted by `fpu_translator.cpp` / `vu_translation_helpers.cpp`). Rounding is still the host's (nearest, where the console truncates) |
| The two fields were not interleaved: the second copy pass overwrote the first | `SCANMSK` in the rasterizer (both paths): 2 leaves the even rows alone, 3 the odd ones |
| **In a level VU1 took 13.5 ns per pair instead of 3.** The level's microcode reads the MAC flags in a few places (`FMAND`/`FMOR`, `FSAND` with the non-sticky bits), and with any reader in the code image every FMAC instruction computed and queued its flags through a C++ helper | `analyzeFlagDemand` (`ps2_vu1_fast.cpp`): flags are computed only for the instructions a reader can observe - walking back from every reader, those less than four pairs before it (a stall lets them complete) and then the first one, which hides the rest; JR/JALR delay slots count as preceding any pair, and the last FMAC instruction before a program end stays visible. About 10% of the FMAC instructions; code that reads the sticky status bits gets flags everywhere (`PS2X_VU1_ALL_FLAGS=1` forces that). 4.1 ns per pair |
| **Frames differed from run to run, and the GS thread stalled ~340 times a frame.** The rasterizer threads each own bands of rows of a surface, so two primitives are drawn in order only if they reach a piece of memory through the same surface. The level draws the same pages as a 512-wide depth buffer, a 256-wide colour target and a 16-bit target, and several effects sample the buffer they draw into | A primitive that touches in-flight pages through another surface waits for the queued ones (`inflightSurface`); one that samples the pages it draws to, or whose frame and depth buffers share pages, is drawn on the GS thread. Pages only read by queued primitives (a depth buffer that is tested, not written) no longer block texture decodes, a decode waits only for the pages of the tiles it needs, and a write drops only the tiles on the written pages (`Texture::pageTiles`) |
| **In a level the picture combed whenever something moved, and the display registers held garbage.** The HLE of `sceGsSetDefDispEnv` filled in `DISPFB` and `DISPLAY` only; `PMODE`, `SMODE2` and `BGCOLOR` kept whatever the stack held (`SMODE2` a `SCISSOR` value), so the level was shown as one progressive frame. The game asks for interlaced field mode (`sceGsResetGraph(0, 1, 2, 0)`) and draws its two half-height fields for different moments | The HLE writes the whole structure like the SDK routine (`0x11CF00`): `PMODE` `0x66`, `SMODE2` 1, 2 or 3 from the mode, `BGCOLOR` 0, and the NTSC/PAL `DISPLAY` values (`MAGH = (w + 2559) / w - 1`, `DX = 636 + dx * (MAGH + 1)`, `DY = 50 + dy`, `DH = h - 1` or `2h - 1`). In field mode the presenter shows one field per VBlank (see [Presentation](#presentation)) |
| The two display circuits were composed at the same position. In a level circuit 1 is a line lower than circuit 2 (`DISPLAY1.DY` 51, `DISPLAY2.DY` 50 with `DISPFB2.DBY` 1) and is mixed in with weight 0 (`PMODE` `0x67`: `MMOD` with `ALP` 0), so only circuit 2 shows | `PresentFromLocalMemory` places each circuit at its `DISPLAY.DY`, skips a circuit that cannot show, and takes the field from the screen line of the picture's first row |
| The game's VBlank wait (`0x25DE90`) polls a counter in a loop; executing the loop until the next VBlank took several milliseconds of host time per frame | `EeScheduler::skipIdleCycles` (see [Timing](#timing)), called from `overrides.cpp` |
| **A level ran at 10 fps.** Seven million pixels a frame through scalar loops, texture tiles decoded texel by texel through the swizzle helpers, a 4 MiB copy of GS memory for every presented frame, a heap allocation for every GIF packet and DMA chain | Block-wise decode, AVX2 span loops, presentation from GS memory with frame reuse, reused buffers: see [The GS renderer](#the-gs-renderer) and [Speed history](#speed-history). 30 fps standing still |
| **No sound: the sound driver found none of its tones** (`Tone '...' not found, maybe tonebank not loaded`, `Potential deadlock on mutex ToneLibrary::Lock`). The IOP ran an RPC server's function outside the IOP thread system, with a budget of 2,000,000 instructions and `GetThreadId` answering 0x7FFF: the tone-library load was abandoned halfway, with its mutex held | RPC server functions run on the thread that called `sceSifRpcLoop`: its sleeping state is parked, the function runs on that thread's stack below the parked frame, and the IOP scheduler is pumped until it returns (`executeGuestFunctionOnThread`). Then an SPU2 model and host playback, see [Sound](#sound) |
| **Movies were black.** The MPEG HLE decoded while demultiplexing and held data back after 8 pictures; the game's player (built like Sony's sample) waits for its own 1 MB stream ring to fill before it starts and drains it only through its "no data" callback | A guest-buffered mode (`setMpegGuestBuffersStream`, set by `overrides.cpp`): `sceMpegDemuxPss` hands every packet to the game's stream callbacks, and `sceMpegGetPicture` asks for data through the no-data callback, which programs the IPU's DMA channel (`D4_MADR`/`D4_QWC`), then decodes what that channel would have carried |
| Movie sound repeated or skipped blocks. The game feeds one 0x2000-byte block per movie update and the sound driver's streamer asks for one per block played: the IOP ran far ahead of the EE whenever an RPC or a module load ran IOP code on the EE's behalf, and decoding a picture cost the EE no time | An EE/IOP cycle balance: IOP code run on the EE's behalf is charged against later EE time (at most 4 ms owed), and each decoded picture charges the EE 10 ms of cycles (`MPEG.cpp`), as the console's decoder would take |
| Movie pictures reached the native renderer the PS2 way: written into the game's texture bitmap in the IPU's 16-pixel macroblock columns, uploaded again, converted back from a GS-memory layout - ~4-6 ms of the game's thread a picture (Transformers roadmap N6) | `ps2_stubs::setMpegNativePictureSink` (patch 0072, `include/runtime/ps2_mpeg_native.h`): sceMpegGetPicture hands each picture it serves (RGBA, its number, the game's buffer, the VBlank) to the port, which draws it itself and may leave the PS2-layout copy out; sceMpegDelete tells it; `setMpegAccurateColour` interpolates the chroma to every pixel across; `takeMpegHostCosts` reports the decoding / conversion / copy time. Nothing changes without a sink |
| **Cutscenes and HQ calls never played.** GCC's EE ABI gives every variadic argument an 8-byte slot (`sd` saves of `a2`–`t3`); the runtime's `va_list` walked 4 bytes at a time, so the game's string formatter (`0x101490` → `vsprintf`) got only its first argument right and every name put together from two parts (`"%s%s"`: a level's cutscene scripts) ended in `(null)` | `ps2_stubs::setVarArgSlotBytes(8)` (set by `overrides.cpp`): the `va_list` cursor and the variadic-argument reader use 8-byte slots |
| The frame's read-back of GS memory (the occlusion buffer, page 464) waited for the whole queue of the GS thread | Answered from a capture taken in the GS command stream right after the last write to the region (`GSThreadedBackend::ReadbackAhead`, `Op::Capture`); with VIF1 on its own thread also ahead of the VIF1 queue (see [VIF1 and VU1 on their own thread](#vif1-and-vu1-on-their-own-thread)) |
| After a slow frame the pacing restarted from "now", so the EE then idled for a whole VBlank before the next frame | The restart keeps one VBlank period of lag (`now - period`); alarms and idle timers use the same schedule (`hostTimeOfCycle`) |
| **The dropship area ran at 17 fps.** The display list's VIF1, VU1 and GS front-end work ran inside the DMA start on the EE thread, and was the larger part of a frame | VIF1, VU1 and the GS front end on their own thread, and many smaller steps: see [Speed history](#speed-history) |
| **At a steady 30 fps the game still felt laggy, and motion juddered.** Three causes. The picture was latched at the start of each VBlank, before the game flipped in it, and that latch then waited in the VIF1 queue behind the display list the game kicks right after its flip: a finished frame reached the window about 33–36 ms after the flip, plus up to 17 ms of the window's own 60 Hz timer. So the first field shown of every frame was its later one (a level frame holds two fields drawn for two moments): while the view turned, the picture moved −25 px, +8 px, −25 px... instead of −8.5 px per VBlank. And the VBlank between two flips is taken by the game only when it has finished its frame, about 28 ms after the first: the two fields of a frame were up for about 28 and 5 ms | The picture is latched just before the next work the EE queues for VIF1 or the GS after the VBlank started (the flip comes before the next display list), and `ps2_host_display.cpp` shows the VBlanks on the host's schedule, one field each: a flipped frame is on its way about 3 ms after the flip, the fields in order, 16.7 ms apart to within about 1 ms. The window reads the input every 4 ms. See [Presentation](#presentation) |
| **No loading screen through a warp gate: black, with green blocks across the bottom** (the user, 2026-10-08: "it should be like a blue portal with a loading bar in the middle"). The screen is a VU1 program the EE starts at every VBlank by writing CMSAR1 (`ctc2 $zero, $vi31`), after waiting on the COP2 condition (`bc2t`) and writing its parameters into VU1's data memory; the field's draw settings go through the GIF FIFO (EE `sq` to 0x10006000). The recompiled CTC2 only stored CMSAR1, BC2T tested VU0's status flag, and the GIF FIFO's stores were dropped: nothing was drawn, and the last frame's buffer stayed up with the next level's textures uploaded over its bottom rows. Then the HLE `sceGsSetDefDBuff` (both buffers at page 0, `DISPLAY` 640 x 224 at `DY` 32 with `MAGH` 0) and `sceGsSetDefClear` (nothing filled in), the presenters reading `SMODE2` frame mode as progressive, and the level's field settings still on during the load | CTC2 to CMSAR1 calls `PS2Runtime::vu1StartMicroProgram` (the program at that address run as VIF1's MSCAL runs one, deferred behind what is queued for VIF1); BC2F/BC2T call `vu1Busy` (the VIF1 thread caught up, then idle). EE stores to the GIF FIFO are gathered up to the end of a packet and sent as PATH3 (`gatherGifFifo`). The game's own `sceGsSetDefDBuff`, `sceGsSetDefClear` and `sceGszbufaddr` run (`remove_stubs`). In interlaced frame mode (`SMODE2` 3) a circuit shows half its `DH` in buffer rows and `DY` counts whole-picture lines, in both presenters; while it is shown, `fieldsBuffer`, `secondFieldDraws` and `momentFields` answer "none" (`setFrameLinesShown`, from the GS front end's latch). Above scale 1 a fill's (an untextured sprite's) edge on a whole coordinate covers whole GS-pixel blocks, as a half-coordinate one does (`PS2X_GPU_SPRITE_EDGES`; level pictures unchanged) |
| **No reverb, and voices resampled by a curve of the model's own** (the user, 2026-10-09: "the sound should be the first thing that is working 100%"). The SPU2 model kept the reverb's registers and dropped what the voices sent to it, and interpolated with a cubic curve; the game turns on the SDK's "Room" reverb on both cores (at 0.2; which voices it sends there depends on the place) | Each core's reverb after psx-spx's measured description (the 39-tap resampling filters, left and right on alternate samples, the hardware's order of accesses, 16-bit saturation, ATTR bit 7, vIIR -8000h), the wet paths of the mixer (VMIXEL/VMIXER, MMIX's wet bits, EVOL), the hardware's 512-entry interpolation table, the capture areas and the address interrupt on them; the filters' histories a save-state section (`RVRB`). 1% of the game thread on the dropship walk; `PS2X_SPU2_REVERB=0`, `PS2X_SPU2_INTERPOLATION=cubic` give the old sound |

## Console arithmetic

The R5900 FPU and the vector units are not IEEE 754 machines. They have no NaN and no
infinity: a word with exponent 255 is just a large number, a result that overflows saturates
at ±FLT_MAX (`0x7F7FFFFF`), and 0 × anything is 0. Games depend on that without knowing it.
Transformers leaves garbage in the unused `w` lane of its matrix rows (`0xFFE0E001`, a NaN on
a PC); with host arithmetic that lane spread through every matrix product and blanked the 3D
view of a level.

`ps2_runtime_macros.h` gives the generated code the console's behaviour:

- `ps2_vclamp` / `ps2_fclamp` turn bit patterns from `0x7F800000` up into +FLT_MAX and from
  `0xFF800000` up into −FLT_MAX (two integer `min`s on the bits). Every FPU and VU0
  macro-mode operation clamps its operands and its result (`PS2_VADD`, `PS2_VMUL`,
  `FPU_ADD_S`, ...).
- Comparisons order the bit patterns as sign-magnitude integers (`ps2_fpu_compare_key`), so
  nothing is unordered; the "unordered" predicates are constant 0.
- `MAX`/`MINI` compare the same way and pass the winning operand's bits through unchanged.
- `DIV`, `SQRT` and `RSQRT`: a division by zero gives ±FLT_MAX and sets the I or D flag and
  its sticky bit in FCR31; square roots take the absolute value.
- `FTOI` saturates (`ps2_vftoi`); `CVT.W.S` truncates and saturates.

Not done: the console rounds toward zero where the host rounds to nearest. Changing the
host's rounding mode (or DAZ/FTZ) on the EE thread would also change the VU1 reference
interpreter, which derives its underflow flags from host arithmetic, and the host math
library that HLE stubs call.

These macros are inlined into every generated function, so a change to the header rebuilds
everything (see [Building](#building)).

## VU1

Three engines run VU1 microcode; all give the same results.

| Engine | What it is | Cost per instruction pair |
|---|---|---|
| Reference (`ps2_vu1_core.cpp`, `PS2X_VU1_REFERENCE=1`) | Models every pipeline stage cycle by cycle. The tracing and watch options run on it | ~200 ns |
| Fast (`ps2_vu1_fast.cpp`, `PS2X_VU1_JIT=0`) | Pairs are decoded once per code image. Register writes take effect at issue; only what a program can observe early or late without an interlock keeps its exact latency: Q, P, the MAC/status/clip flags and the PATH1 transfer of `XGKICK` | ~13 ns |
| JIT (`ps2_vu1_jit.inl`, the default; x86-64 with AVX, through xbyak) | Basic blocks of the decoded form compiled to native code, with the pipeline model resolved at compile time | 1.7 ns (353 million pairs a second at the start of Amazon Basin flat out at 144; 2.1 ns before the second round below, 3.0 ns before 2026-10-04) |

`PS2X_VU1_VERIFY=1` runs every program on the reference and on the fast engine (JIT
included) and reports any difference in registers, data memory, `XGKICK` packets or cycle
count. Run it after every change to the fast engine or the JIT.

- **Code images.** Decoded and compiled code is cached per content of VU1 micro memory. The
  game swaps microprograms 70–90 times a second between eight or nine images, and uploading
  code that is already there invalidates nothing.
- **Program length.** A program runs until its E bit (`kVu1ProgramCycleLimit`, 64M cycles,
  only catches runaways). The level's grass batches run for more than 65,536 cycles; cutting
  them there let the VIF unpack the next batch over buffers still in use.
- **Flags.** Computing MAC and status flags for every FMAC instruction costs more than the
  instruction itself, and almost none of them are ever read. `analyzeFlagDemand` walks back
  from every reader (`FMAND`, `FMOR`, `FMEQ`, `FSAND`...): the FMAC instructions less than
  four pairs before a reader may be visible to it (a stall lets them complete), and from
  four pairs on the first one hides everything before it. A `JR`/`JALR` delay slot can
  precede any pair, so it counts as one pair before every reader; the last FMAC instruction
  before a program end stays visible to the next program. Code that reads the sticky status
  bits gets flags everywhere. About 10% of the FMAC instructions of the level's microcode
  keep their flags (`PS2X_VU1_ALL_FLAGS=1` computes them all, for comparison).
- **Compiled inline.** The flags of those FMAC instructions (the exact result of the four
  lanes in double precision, classified with compares: zero, sign, underflow, overflow, and
  the product's sticky bits for MADD/MSUB/OPMSUB), `CLIP` and `DIV` are emitted into the
  block, entry of the flag FIFO and Q queue included; until 2026-10-03 they were calls into
  C++ handlers. The blocks run under the VU float mode (round to zero, flush to zero,
  denormals as zero), so a quotient needs no normalizing beyond FLT_MAX for overflow.
  `PS2X_VU1_VERIFY=1` holds them to the reference. So are `SQRT`, `RSQRT` and the EFU's
  `ESADD`, `ERSADD`, `ELENG`, `ERLENG`, `ESUM`, `ESQRT`, `ERSQRT` and `ERCPR` since
  2026-10-04: the same operations in the same order as the handlers compile to (checked
  against their machine code), entry of the P queue included (`PS2X_VU1_JIT_QP=0` puts them
  back in the handlers). Still handlers: `EATAN`, `EATANxy`, `EATANxz`, `ESIN`, `EEXP`,
  `XGKICK`, `FCSET`/`FSSET`, and stores while an `XGKICK` transfer is in flight.
- **XGKICK.** PATH1 reads qword k of a packet at cycle issue + 1 + 2k; stores made while a
  transfer is pending into the packet log the qword they overwrite, and the packet is read
  with the stores it could not see undone. The packet's extent (a walk over its GIF tags, as
  the transfer reads them) is worked out when the transfer starts and again after a logged
  store (`Engine::kickWatch`): stores elsewhere it never reads are not logged, and compiled
  code stores inline unless the address falls in it. (The game's microcode fills one buffer
  while the other is sent: until 2026-10-04 every one of those stores went through a handler.)
- **Faster blocks** (2026-10-04, each step held to the reference by `PS2X_VU1_VERIFY=1`):
  - operands are normalized (exponent 255 as ±FLT_MAX) with two integer minimums on the
    bits - unsigned against -FLT_MAX, then signed against FLT_MAX - instead of five
    instructions;
  - a block that ends with a branch back to its own start goes round in place, as the run's
    loop would call it again (within the budget, no stop asked for, the program not ending);
  - the VF registers and ACC a block uses most stay in xmm6-xmm15 from its start to wherever
    it is left (written back there and before a handler; `PS2X_VU1_JIT_CACHE=0` keeps them
    in memory), so a chain of FMAC instructions no longer goes through memory;
  - the events (Q, P, the flag FIFO) are committed only before a pair that can see them and
    before the block is left, in the order of the cycles they became due (`commitEvents`;
    a lone Q directly);
  - a register check only where a write of the three pairs before has a latency longer
    than the pairs in between (a stall only puts them further apart); a block that goes round
    on itself has a second copy of its first three pairs for the way round, where those three
    are its own last ones;
  - each program run commits events only when one is due, and finds its engine without a map
    lookup.
- **Faster blocks, second round** (2026-10-04 night: VU1 2.1 to 1.7 ns a pair; `PS2X_VU1_VERIFY=1`
  and the frames unchanged; each step can be switched off for comparison):
  - an operand known to be finite is not clamped (`PS2X_VU1_JIT_FINITE=0` clamps all). The
    clamp changes only lanes with exponent 255, and a block knows which lanes cannot hold one:
    VF0; what its arithmetic writes (in the VU float mode a result is finite: the operands are
    clamped and overflow rounds to FLT_MAX); ITOF and ABS results; copies of such lanes (MOVE,
    MR32, MAX/MINI of two of them); I after a LOI of a finite immediate (`m_finite`, worked
    out pair by pair before the block is emitted). A block that goes round on itself checks
    at its start that the lanes it reads before writing them are finite, and its way round
    meets the way in until nothing changes, so the registers set up before a loop (matrices,
    light vectors) count all the way round; if one of them is not finite the interpreter
    runs the block's first pair. The lighting loop at `0x2090` keeps 9 of its 88 clamps;
  - `SQRT`, `RSQRT` and the simpler EFU instructions compiled inline (above);
  - events committed in the block itself: one Q or P result due, or flag entries and nothing
    else (`PS2X_VU1_JIT_COMMIT=0`: all through `commitEvents`). The clipping code pushes a
    `CLIP` entry every few pairs, and the last FMAC instruction before a `JR` keeps its flags
    (a jump through a register can land right before a reader): 15 million calls a second
    at the start of the level before, a tenth of the thread;
  - a block that ends goes on into the next compiled block itself when `run()` would call it
    next (no stop asked for, within the budget; `emitChain`): blocks ran 19 pairs a call,
    16 million calls a second, each through the dispatch loop and a full prologue and
    epilogue. Every block has the same frame and saves all of xmm6-xmm15, so the next one
    enters after the saves and the last one restores what the first saved; a block counts
    its own pairs (`jitLoopPairs`). `PS2X_VU1_JIT_CHAIN=0` returns after every block, and
    `PS2X_VU1_PROFILE` does so too, to count blocks one by one.
- **Native program runs** (2026-10-04, night). `ps2vu1native::setHook`
  (`runtime/ps2_vu1_native.h`) lets a game do program runs itself: the fast engine offers
  every run to the hook, in VU1's float mode, before its microcode runs; the hook either does
  it - VU1 data memory written as the microcode would, the packets sent through the same
  path as an `XGKICK`'s, the end pc set - or leaves it to the microcode. `PS2X_VU1_NATIVE=check`
  runs the microcode as usual and the hook on copies of the registers and data memory, and
  compares packets, data memory, end pc and the registers later runs read (`vi13`, `vi14`,
  `vf01`-`vf04`). Transformers' hook (`vu1_native.cpp`, [RENDERER.md](RENDERER.md)) does
  most of the game's batches: `0x1410` drawing and clipping, its pre-passes, the lit meshes,
  the instanced meshes.
- **A word the game does not use.** At the end of every frame (`0x25BF50`) the game reads a
  counter its microcode keeps in VU1 data memory (`0x1100C0B0`), stores it times 8 at
  `0x6CF068`, which nothing reads, and clears it. With VIF1 on its own thread that read would
  have to wait for the queue; `overrides.cpp` declares the word unobserved
  (`ps2vif1::setUnobservedWord`): reads give 0 at once, the clear takes its place in the
  queue.

## VIF1 and VU1 on their own thread

On the console VIF1 unpacks a display list, VU1 transforms it and the GIF sends the result to
the GS beside the EE. The runtime did all of that inside the DMA start, on the EE thread, and
in the dropship area that was the larger part of a frame. `ps2_vif1_thread.cpp` gives it a
thread (the "VIF1 thread"): a transfer is queued and the EE goes on.

- **What the EE observes is unchanged.** What a transfer does that the EE can see - the
  i-bit interrupt, `MARK`, the end of the DMA - is worked out when it is queued, from a scan
  of its VIF codes; `VIF1_STAT` (`INT`, `MRK`) and `VIF1_MARK` read that. Anything else that
  changes what VIF1 works with (GIF path 3 packets, GS register writes from the HLE
  libraries, uploads, the presentation at VBlank) is queued behind it, so it all still
  happens in the order the EE asked for (`ps2vif1::defer`). Code that reads results - VU1
  memory, GS memory, a save state - waits for the queue (`ps2vif1::sync`).
- **The D and T bits** for microprograms come from VU0's `FBRST`, which belongs to the EE:
  it is sampled when a transfer is queued.
- **Read-backs ahead of the queue.** The game draws a small buffer near the end of every
  display list (the occlusion buffer, page 464) and reads it back right after; waiting for
  the whole list would put the EE and the VIF1 thread one after the other. The scan also
  follows the GS registers set through PATH2 that say where primitives are drawn (`FRAME`,
  `ZBUF`) and where images go (`BITBLTBUF`), and works out the point of the list after which
  nothing can reach the region of the last read-back. The next read-back of that region
  waits for the VIF1 thread to pass that point, then for the GS thread's capture there
  (`GSThreadedBackend::ReadbackAhead`). Microprograms (PATH1) are not scanned: the first time
  one sets `FRAME` or `ZBUF`, this is switched off for good, and a write into the region after
  an early answer is reported (`readbackAheadFailed`) and turns it off for a while.
- **Waking the EE.** The EE waits for the VIF1 thread's position and for the capture on
  condition variables that the other side signals when it gets there; a sleep would wake it
  up to a millisecond late on Windows, twice a frame.
- **The GS front end on a thread of its own** (2026-10-03). What VIF1 hands the GS - the
  packets microprograms kick out (PATH1), VIF1's own (PATH2), the EE's (PATH3, which still
  take their turn on the VIF1 thread for the GIF arbiter) and GS work the EE queues - took a
  third of the VIF1 thread's time in a level (`GS::processGIFPacket`, `vertexKick`,
  `writeRegisterPacked`). The VIF1 thread now passes it on in batches, in its own order: a
  packet as a record in a 64 KB buffer (no allocation each), queued GS work
  (`ps2vif1::deferGs`, which the GS front end uses where it used `defer`) as a function in
  the same order. A batch goes over at the end of each item the VIF1 thread processes, when
  it is full, and when a read-back waits for a position (the VIF1 thread checks at each VIF
  code). `sync` and the read-back's wait for its region also wait for the GS's thread to get
  through what came before. The walk from the dropship to the first Mini-Con, flat out: 36
  frames a second before (VIF1 thread busy 93%), 50 after. `PS2X_GS_FEED=0` keeps the GS
  front end on the VIF1 thread.
- `PS2X_MTVU=0` runs everything on the EE thread as before. `PS2X_MTVU_STATS=1` prints every
  2 s how often and how long the EE waited, by reason, and the frame as the read-back sees
  it: display list queued → read-back requested → region settled → capture answered → next
  display list.

## The GS renderer

The renderer was software first: the game's effects read and write GS memory in ways (the
same pages as depth buffer, colour target and texture at once) that need exact GS
behaviour. The GPU renderer ([below](#on-the-gpu)) keeps that behaviour and draws by default
since 2026-10-03; `PS2X_GPU=0` (`run.ps1 -Software`) draws in software.

### Threads

```
EE thread            recompiled game code, the IOP and the sound processor
   | VIF1 queue: display lists, GIF path 3 packets, GS register writes, presents
VIF1 thread          VIF1, VU1, GIF: decodes registers, assembles primitives (GS front end)
   | command ring (32 MiB): draw states, runs of primitives, uploads, transfers, presents, captures
GS thread            GSThreadedBackend -> GSCpuBackend: draw state, textures, set-up bookkeeping
   | job ring (8192 jobs)
rasterizer threads   each sets up and draws its own rows of every job; barrier jobs
```

- The front end sends the draw state only when it changes (`gs_draw_state.h`), and then
  runs of bare vertices; the GS thread prepares a run once (`PrepareRun`) and its primitives
  one by one (`DrawFastPrimitive`).
- `PS2X_GS_SYNC=1` runs the GS thread's work on the producing thread;
  `PS2X_GS_THREADS=<n>` sets the number of rasterizer threads (0 or 1 draws on the GS
  thread). The default leaves three hardware threads for the EE, VIF1 and GS threads from 8
  hardware threads up (8: 5 rasterizer threads, measured faster than 4; at most 8); below
  8, it is the hardware threads minus two, at most 4.
- Every rasterizer thread walks the whole job queue in order and draws only the rows it
  owns: bands of 8 rows, dealt out in turn. A pixel is therefore always drawn by the same
  thread, in submission order, with no locks.
- **Barrier jobs.** Work that must see everything queued before it and must be done before
  anything after it - a texture decode from pages queued primitives draw to, a primitive that
  has to be drawn by one thread, a present, a capture for a read-back - is queued as a barrier:
  the first rasterizer thread runs it once all threads have finished the jobs before it, and
  none goes on until it is done. Barriers that follow each other with nothing between are
  one. The GS thread does not wait.
- What still makes the GS thread wait for the rasterizer threads: an upload into pages being
  drawn to, a CLUT load from them, a local-to-host or local-to-local transfer, primitives the
  reference rasterizer draws (other than lines and points). `PS2X_GS_STATS=1` prints how
  often and how long, by reason.

### Drawing

`DrawFastPrimitive` handles triangles, strips, fans and sprites; lines and points go
through the original per-pixel code, which is kept as the reference (`PS2X_GS_REFERENCE=1`
draws everything with it). Lines and points only read and write the frame and depth
buffers, so they are drawn by a rasterizer thread behind a barrier.

- The draw state is decoded once per primitive into a `PixelPipe` (frame and depth buffer,
  masks, tests, blending, `SCANMSK`).
- Coverage follows the GS: sample points at integer pixel coordinates, top-left fill rule.
  Triangles are walked span by span; attributes are evaluated from the span start
  (`start + k * step`, in doubles for Z, S, T and Q), not accumulated, so a pixel's value
  does not depend on where its span was entered.
- Bilinear filtering samples at texel centres (+0.5), perspective-correct through Q.
- `SCANMSK` 2 leaves the even rows alone, 3 the odd ones.

### Textures

Textures are sampled from linear copies of GS memory (`FastState::Texture`), decoded on demand
in 16×16-texel tiles. Each tile is decoded block by block: GS memory is laid out in 256-byte
blocks (8×8 pixels at 32 bits, 16×8 at 16 bits, 16×16 for 8-bit and 32×16 for 4-bit indices),
and within a block the pixel order is a fixed table (`BlockLayout`), so a block of 32-bit
pixels is unswizzled with a few SSE shuffles.

Every write to GS memory (draws, uploads, transfers, clears) stamps the 8 KiB pages it
touches. A texture knows which of its tiles read which page (`pageTiles`), and a write drops
only those tiles. The decode asks for just the texels a primitive can reach; a range that
crosses the repeat boundary once becomes the two ends of the texture (`wrappedRanges`)
rather than the whole of it - the level's interleave passes sample a 1024×1024 declaration
of which 512×448 exists.

Queued primitives read the decoded copies and the expanded palette directly. A primitive
therefore sees the texels as they were when it was queued, never what it draws itself - the
same as when everything is drawn in order on one thread, since its tiles were brought up to
date just before it. Tiles whose pages have been written are dropped; one that queued jobs
may still read is decoded again either behind a barrier (after those jobs, into the same
buffer) or, when the GS thread decodes it itself, into new buffers (`replaceBuffers`).
Buffers set aside are kept until the last job that may read them has finished (`Deferred`).
A decode from pages that queued primitives still draw to is queued as a barrier.

### Ordering between threads

Rows are a property of a *surface* (base pointer, width, format family): the same memory seen
through another width or format has its rows elsewhere. The level draws the same pages as a
512-wide Z24 buffer, a 256-wide CT32 target and as CT16 and Z16. So:

- a primitive that touches pages which queued primitives reach through another surface comes
  after a barrier (`inflightSurface`; from 2026-10-03 the GS thread no longer waits there);
- a primitive whose frame and depth buffers share pages is drawn by one thread behind a
  barrier;
- a primitive that samples the pages it draws to (the effect passes: each strip of a pass
  reads what the one before drew) is an ordinary job: it samples the decoded copy, which
  was brought up to date behind the jobs that drew those pages. Until 2026-10-03 these were
  drawn by one thread, about 190 a frame in the dropship area.

Pages that queued primitives only read (a depth buffer that is tested but not written) do
not block texture decodes. With this, runs are repeatable down to the pixel; before it,
frames differed from run to run. `PS2X_GS_BARRIER_LOG=1` logs the primitives that go behind
a barrier or change surface, with their targets and texture coordinates.

### The vector code

`gs_raster_avx2.cpp` holds the span loops for eight pixels at a time. It is compiled with
`/arch:AVX2` and entered only when the CPU and the OS support AVX2 (`rasterSimdAvailable`;
`PS2X_GS_SIMD=0` selects the scalar code). It is written to give exactly the pixels of the
scalar loops in `gs_raster.h` - same attribute evaluation, same texel arithmetic, same order
of tests - and `tools/check_frames.ps1` holds it to that.

- Pixel addresses come from two tables per surface, `row[y] + col[x]` (`SurfaceOffsets`);
  the GS layouts for 32-, 24- and 16-bit buffers are separable that way, which the tables'
  builder verifies. Frame, depth and texels are gathered; stores are scalar.
- Channels are kept as 16-bit words; blending is `((A − B) · C >> 7) + D` built from
  `mulhi`/`mullo`.
- Covered: the alpha test with every `AFAIL` mode, destination alpha test, depth test and
  write, blending, `PABE`, `FBA`, `FBMSK`, RGB24 frames, 16-bit frames (5:5:5:1 expanded and
  packed in the loop), fogging, all texture functions, nearest and bilinear sampling.
- Not covered, and drawn by the scalar code: frames whose pixels alias their depth buffer.

### Uploads and transfers

Host-to-local uploads arrive in chunks of up to 256 KiB and are written row by row into the
swizzled layout, the format's addressing inlined (`uploadPixels`; until 2026-10-03 pixel by
pixel through two indirect calls and a division each, 16% of the GS thread in the dropship
area). An upload that does not change memory (the game re-sends the same textures every
frame) leaves the page stamps alone, so cached textures stay valid.

Each pixel's address is worked out once (patch 0019, 2026-10-22): a run that stays in one row of
one page takes the page and its lookup table's row once, then reads, compares and writes each
pixel at one address (before, `Read` and `Write` each worked it out, the page division
included). Amazon streams about 145 MB of textures a second, 84% of the uploads changing memory;
the GS backend thread spent an eighth of its time here, and the level ran 4% faster uncapped
after.

### Presentation

The EE scheduler latches a picture once per VBlank through the command stream
(`GS::latchHostPresentationFrame` → `GSThreadedBackend::QueuePresent`), so the image is built
at exactly that point of the game's drawing: the game flips and reuses its display buffers,
and reading them any later shows the next frame half drawn. The GS thread queues it as a
barrier job (`PresentQueued`), so it is built once the primitives before it are drawn,
without the GS thread waiting; the capture for a read-back works the same way
(`CaptureQueued`).

- **Where in the stream.** Not at the start of the VBlank: the game flips right after it
  (`PS2X_TRACE_FLIP=1`: VBlank start, the game's wait returns 0.02 ms later, `DISPFB2` and
  `DISPFB1` written at 0.04 ms, the display list kicked at 0.05 ms). The latch waits for the
  first work the EE queues for VIF1 or the GS after the VBlank started (`ps2vif1::setBeforeQueue`,
  `EeScheduler::takePresentationLatch`) - the next display list, after the flip - or at the
  latest for the next VBlank. Not the end of the VBlank either: with the wait loop skipped
  (or a VBlank taken late) the game reaches it before it has flipped.
  `PS2X_PRESENT_LATCH=vblank` latches at the start of the VBlank as before.
- The frame is read block-wise from GS memory. If the display registers and the stamps of
  the pages it was read from are unchanged, the last frame is reused.
- Two display circuits are composed where they are on the screen (`DISPLAY.DY`), blended as
  `PMODE` says; a circuit that cannot show (blended in with weight 0, or replaced by the
  background colour) is not read.
- **To the screen** (`ps2_host_display.cpp`). The window shows one VBlank after the other on
  the host's schedule (the deadlines the EE scheduler paces the game by), not when the game
  gets round to a VBlank: a game busy with its frame takes the VBlank in the middle of it
  late (in a level about 28 ms after the one before it). A VBlank in which the game flipped
  is shown once the GS has its picture; any other one, with the picture there is, at most
  2 ms after it was due. In the dropship area: a flipped frame on its way about 3 ms after
  the flip, every VBlank shown, 16.7 ms apart to within about 1 ms (`PS2X_PRESENT_STATS=1`).
  The window has no frame limit of its own (raylib's `SetTargetFPS` was a third 60 Hz clock
  beside the VBlanks and the screen) and reads the input every 4 ms.
- Interlaced field mode (`SMODE2` = 1): the console sends the even lines of the buffer in
  one field and the odd lines in the next, and the game draws the two for different
  moments. The picture is published woven, as the buffer holds it, and each VBlank shows its
  field over the whole height, the lines between taken as the mean of their neighbours
  ("bob", the default). `PS2X_DEINTERLACE=weave` shows both fields at once (sharp when
  nothing moves, combs when something does), `blend` mixes every line with the next. The
  order is checked with the view turning at a steady speed: the picture moves by the same
  8.5 px every VBlank (it went back and forth while the latch came before the flip).
- **The screen's refresh.** The window's own screen's (`ps2_host_display::setWindow`): a thread
  waits for its vertical blanks (DXGI's `IDXGIOutput::WaitForVBlank` on the output the window is
  on, looked for again when the window moves or the screens change) for where in time the
  refreshes fall, and the period is the screen's mode's, as exactly as Windows has it
  (`QueryDisplayConfig`: 144/1, 60000/1001). Without the thread's vertical blanks, the desktop
  compositor's (`DwmGetCompositionTimingInfo`) - one timing for the whole desktop, which said
  170 Hz for a window on a 144 Hz screen beside a 170 Hz one. When the refresh is the VBlank rate
  or a multiple of it (to 1%), the VBlanks are timed by it: its period, and 8 ms ahead of a
  refresh at 60 VBlanks a second (`PS2X_DISPLAY_LEAD`; half a VBlank at a faster rate), a tenth
  of the phase error taken out per VBlank, so that every VBlank's picture comes up on a refresh
  of its own (`PS2X_DISPLAY_SYNC=0` keeps the plain rate). At other rates a picture stays up for
  two or three refreshes (60 VBlanks at 170 Hz: 11.8 or 17.6 ms) - which is why a game's frame
  rate is best the screen's (144 on a 144 Hz screen) or a whole fraction of it.
- **Variable refresh** (FreeSync, G-Sync: `ps2_host_display::screenVariable`). A screen that
  refreshes when pictures come has vertical blanks that do not keep to its mode's period (three
  in four within 4% of one or two periods, else variable). Its refreshes then follow the game's
  own pictures, so nothing follows them: the VBlanks keep their own clock, moment pictures go at
  the mode's rate (the fastest the screen may go), and a refresh without a new picture is not
  counted as missed. (Before, on the user's 144 Hz FreeSync screen in full screen, the VBlanks
  locked to the game's own pictures, 91-141 a second, and slowed it to 56 frames a second.) The
  `[present]` line says `screen 144.00 Hz (variable refresh: N refreshes a second)`.
- **Full screen** (F11, `PS2X_FULLSCREEN=1`, the settings' Display mode) is a borderless window
  a pixel taller than the screen. One the screen's exact size is flipped straight to the screen
  by the driver, and with GL's swap and the GPU renderer busy that held the GPU back: on the
  user's 4K 144 Hz FreeSync screen (RX 9070 XT) the GPU fell behind by about 15 ms a frame and
  the game to 33-40 frames a second, where the taller one, composed by the desktop, holds 120
  pictures. `PS2X_FULLSCREEN_EXACT=1` makes it the screen's size, `0` a pixel taller. With
  FreeSync on and the game in front, the driver puts the screen into variable refresh, and the
  desktop then composes the window only about 65 times a second (the screen refreshing with
  each): 65 of the 120 pictures shown. With FreeSync off for the game (or the game not in front)
  the screen keeps 144 and every picture is composed.
- **The DXGI swap chain** (`PS2X_PRESENT=dxgi`, a trial, off: `ps2_host_present.h`,
  `ps2_host_present_dxgi.cpp`). The window's drawing (raylib's, unchanged) goes into one of
  three textures; a thread of its own copies it into a Direct3D 11 texture
  (`WGL_NV_DX_interop2`) and a flip-model swap chain (three buffers) presents it - at once,
  allowed to tear, when the screen's refresh is variable, else at its refresh
  (`PS2X_PRESENT_SYNC=0`/`1` forces one). Full screen is then the screen's exact size, flipped
  straight to the screen. The thread never waits inside GL: a wait in AMD's GL driver
  (`glClientWaitSync` with a timeout, the interop's unlock with its blit unfinished, which waits
  for it on the CPU behind the GPU renderer's work) holds every context's GL meanwhile, the GPU
  renderer's too - moments came late (17-48 in 2 s) and the game fell to 56-59 frames a second. It
  asks its fences without waiting and sleeps between the asks, the GPU waits for the window's
  drawing (`glWaitSync`), and two pictures' copies are on their way at most. On the user's 4K
  FreeSync screen (2026-10-09, their settings, the walk down the rocks from a10): the game at 60,
  0-2 moments late in 2 s, about 115 of the 143 moments a second shown and the screen refreshing
  100-115 times a second - against about 65 with GL's swap. Left: the interop's lock and unlock
  still take 1-2 ms, now and then 10-20 (AMD's), and hold the window's drawing up as long - the
  window gets to about 135 of the 144 refreshes. The way that waits on the GPU alone
  (`EXT_memory_object_win32` with Direct3D's fences) made AMD's driver fault on a thread of its own
  at the first flush after GL wrote a shared texture - in the game only, not in a test program, at
  4K neither (Adrenalin 25.20.42); at 4K that test program then hung in the driver as it exited (a
  process Windows cannot end, a core busy, until a restart).
  Two Direct3D devices: the interop's (the textures GL draws into; its lock waits only for its own
  work) and the presenting one (copies, presents). Their order is a shared `ID3D11Fence` (TODO 98,
  patch 0062): the interop's device signals it after each unlock, the presenting device's GPU waits
  for it before its copy - without it AMD's driver now and then let the copy read the texture before
  GL's new picture had reached it, and the screen showed the picture the texture held three presents
  before (stamped pictures read back from the back buffer, RX 5700 XT: 13% of presents went back to
  an older picture with the in-between pictures off, 15% with them on, a quarter in the HQ; 0 with
  the fence). And GL draws into a texture again only once the presenting device's last copy out of
  it is done (an event query, asked without waiting).
- The 512-pixel-wide picture fills a box of the TV's shape: 4:3, or 16:9 while the game
  draws for a widescreen TV ([Widescreen](#widescreen)); `PS2X_ASPECT=4:3`, `16:9` or `pixel`
  fixes it.
- **Moment pictures** (`ps2_host_display::setMomentFields`, with the GPU's pictures): the window
  shows a picture each refresh of the screen instead of each VBlank - `waitForRefresh` hands the
  refresh's picture over 3 ms before it (`PS2X_MOMENT_LEAD`), or later while there is still time
  (0.7 ms), and `ps2_gpu::pictureFor` gives the newest picture made for that refresh or one before
  it. The counter then counts what the window shows. `PS2X_PRESENT_STATS=1` prints
  `[present] moments:` - pictures made and how far ahead of their refresh, refreshes, new
  pictures, the picture shown against the one wanted, the window's drawing time.
- **The FPS counter** (`PS2X_SHOW_FPS=1`, F3 in the window): new pictures a second in the
  window's top left corner, timed from picture to picture over about a second so that a
  steady 59.94 reads 60; 0 while nothing new comes (loading). The GS front end counts them in
  the order of its stream (`ps2_host_display::notePicture`): a VBlank in which the game
  flipped, and the VBlank after it when the frame was drawn as two fields for two moments
  (rows with `SCANMSK` 2 and 3 since the flip, as Transformers draws its levels: 30 frames,
  60 pictures). `TF_FPS` prints the same as pictures/s.

### On the GPU

The default (`PS2X_GPU=0` turns it off): the same command stream drawn with OpenGL 4.5 on a
context of its own (a hidden GLFW window sharing textures with raylib's), on the GS thread
(`gs_gpu.cpp`, `gs_gpu_shaders.h`). Built to be exact first: at the console's resolution it
draws the software renderer's frames to within rounding (in the dropship area a mean
difference of 0.08 per channel, under 0.2% of pixels off by more than 8, all of them on the
last line of the picture where the two fill in a field differently).

- **Resolution.** A GS pixel is `scaleX` x `scaleY` GL pixels: more across than down for a
  picture shown 16:9, which the console draws 512 pixels wide either way. `PS2X_GPU_SCALE=auto`
  (the default for the game; `run.ps1` uses 1 unless given `-Scale`) gives about as many GL
  pixels as the screen the window opens on has for the picture: its 448 lines in the screen's
  height (5 on a 2160-line screen, 3 on 1440, 2 on 1080) and, for a widescreen TV
  (`PS2X_WIDESCREEN`), its 512 columns in the width of a 16:9 picture on that screen (7 on 4K,
  5 on 1440p, 4 on 1080p), else as many as down (`createContext`); exactly halfway (4K: 7.5)
  rounds down. So on 4K the 16:9 picture is drawn 3584x2240 and shown at 3840x2160, on 1440p
  2560x1344 for 2560x1440. (At 8x5, which 4K got until 2026-10-03, the busiest stretch needs
  15-16 ms of an RX 9070 XT per VBlank and the game slowed to 57 VBlanks a second there; at
  7x5 14.6 ms and a steady 60.)
  `PS2X_GPU_SCALE=<n>` is n both ways, `<x>x<y>` sets both (1 to 8 each). The window's pictures
  have mipmaps, so a picture bigger than the window is filtered down, not sampled through. F11
  switches to borderless full screen (`PS2X_FULLSCREEN=1` starts there; `PS2X_WINDOW=<w>x<h>`
  sizes the window, which otherwise opens 16:9 (4:3 with `PS2X_WIDESCREEN=0`) at two thirds of
  the screen's height; `PS2X_MONITOR=<n>` puts it on monitor `n` before full screen and the
  scale are decided). In the dropship area, per VBlank, the GPU spends about 11.9 ms at 5x5
  (8.6 drawing, 2.5 for the field pictures), 13.2 ms at 7x5 and 15 ms at 8x5 (9.9 and 4.1): the
  game still runs at 30 fps at 8x5, with little to spare (16.7 ms a VBlank).

- **The pixel pipeline in a shader.** Frame and depth buffers are `R32UI` images holding the
  GS's own values (RGBA8; RGB24 with its top byte, which is not the buffer's; 5551; Z32, Z24,
  Z16). The fragment shader does what `PixelPipe::shade` does - texturing and `TFX`, fog, the
  alpha test and its `AFAIL` modes, the destination alpha test, the depth test,
  `(A - B) * C >> 7 + D` blending with `PABE` and the clamp, `FBA`, `FBMSK`, the 16- and 24-bit
  stores - reading and writing the images inside a pixel interlock
  (`GL_ARB_fragment_shader_interlock`, ordered): overlapping primitives take effect in the order
  they were sent, as on the GS. The driver needs GLSL 4.50 for the interlock qualifiers.
- **One draw call for a run of draws.** Every draw state is an entry in a buffer of draw
  records, and each vertex carries the index of its record. The draws into one frame buffer
  (and depth buffer) are one `glDrawArrays` until something has to happen in between: another
  frame buffer, a transfer, a draw that samples the buffer being drawn (it reads a copy), more
  than four buffers sampled. In the dropship area about 135 draw calls a frame for 14,000 draw
  states.
- **Primitives outside their scissor.** The shader tests a draw's scissor pixel by pixel, and
  the batch's GL scissor covers all of its draws: a triangle or sprite wholly outside its own
  scissor would still be run over all of its pixels. `appendPrimitives` leaves those out
  (bounding box against the scissor a pixel wider before and two after). Transformers at 60
  frames a second sends half its primitives through the other field's pass, whose scissor is
  one pixel: since 2026-10-04 the GS front end leaves those out before they are queued
  (`outsideScissor` in `gs_frontend.cpp`, the same test with the bounds worked out once per
  scissor and offset), for both renderers - in the dropship area 22 million primitives every
  2 s reached the GPU renderer, 10 million now, the same 36 million vertices drawn.
- **The front end's decoding** (2026-10-05). In real time at 60 on the walk from the dropship
  the GS front end took about 55% of a core, nearly all of it on the packed vertex registers
  VU1's packets are made of. The packed loop now decodes `RGBAQ`, `ST` and `XYZF2` itself (as
  `writeRegisterPacked` does them; the other registers still through it); a primitive's
  vertex count comes from a table; the vertex queue's slot takes no division while the queue
  has not wrapped; the vertices are copied for an overlay's narrowing only while an overlay
  tag is active; the scissor bounds and the overlay tag are plain statics, read and written
  with the GS's state mutex held, instead of thread-locals; and
  `updatePreferredDisplaySourceForDraw` is called only when it can do something (a display
  source chosen, a sprite, or `PS2X_GS_TARGET_LOG` set). About 45% of a core now, the frames
  identical.
- **The GS thread waits** for more commands spinning for about 30 us (2,000 pauses), then
  sleeps until the queue wakes it. It spun for 20,000 before: a third of its time in a level,
  a core's worth of nothing.
- **Rasterization.** GS coordinate x is GL window coordinate `(x + 0.5) * scaleX` (y likewise):
  at the console's resolution the GS's sample points (whole coordinates) are pixel centres, and
  at a higher scale a GS pixel is the block of GL pixels centred on its sample point, standing
  for `[x - 0.5, x + 0.5)` - what a GS with that much more resolution would draw. The game
  draws its 2D and its screen passes with edges on half coordinates (a full-screen pass runs
  from -0.5 to 511.5), which then cover whole blocks, and a copy of a buffer at the scale lands
  pixel for pixel. (Until 2026-10-03 the block started at the sample point: those passes left
  the right half of the last column's block untouched, a line of an old colour down the right
  side of the screen at higher scales, and every copy moved the picture by part of a block.)
  Write-backs take the sample in the middle of each block. Triangles
  follow the GPU's top-left fill rule (the GS's), sprites are two triangles (`[min, max)` as on
  the GS), lines and points are pixel quads (`[x - 0.5, x + 0.5)`) placed like `DrawLine`
  places them. The viewports are powers of two, so the arithmetic from GS to window
  coordinates is exact. The GPU
  interpolates the attributes, so where the software renderer rounds a value at .5 the two can
  differ by one.
- **Local memory on the GPU.** A copy of the 4 MiB in a shader storage buffer, and the GS
  swizzle (`GSMem`'s page tables) in another. Textures and CLUTs are read straight from it, every
  format, with wrapping and `TEXA` as `TexSampler` does: no texture is decoded on the CPU. Pages
  the EE side writes (uploads, local-to-local copies) are copied up when something next reads
  them.
- **The buffers it keeps** (`Target`): one per base, width and address layout (C32, Z32, C16,
  C16S, Z16, Z16S), as tall as what was drawn into it. The game uses the same memory in several
  layouts each frame - the depth buffer's pages are also a 256-wide colour target, 16-bit colour
  and 16-bit depth views, and the next display buffer - so every page knows the buffer that drew
  it last (its owner). Before anything else reads the page (another buffer, a texture, the
  display), the owner writes it back into the GPU's local memory (`writeBack`: the swizzle the
  other way round, one fragment per GS pixel), and the other buffers reload the page-sized cells
  they hold of it. All of it on the GPU.
- **Drawing alone.** Unless `PS2X_GPU_MIRROR=1`, the software renderer does not draw: it only
  keeps local memory for transfers, read-backs and save states. Before the CPU side reads
  memory the GPU drew - a local-to-host or local-to-local transfer, a CLUT load, a pixel write, a
  save state - those pages are downloaded from the GPU (`beforeTransfer`, `downloadAll`, the
  `GpuDownload` record). In a level the game's one read-back a frame (64x64 at page 384) reads
  memory only the EE writes, so nothing waits for the GPU. Until the GPU is up (no window yet)
  the software renderer draws, and the GPU then starts from its local memory.
- **Presentation.** At each VBlank (`present`) the two circuits are composed as
  `PresentFromLocalMemory` does, then the field is filled in ("bob" by GS lines, as
  `ps2_host_display` does), into one of four pictures the window samples through the shared
  context (it waits for the GPU's fence on its own context). The window takes the picture of
  the VBlank it shows. At most two VBlanks' pictures are in flight: after queuing one, the GS
  thread waits for the GPU to finish the one before last. Nothing held it back before, and a
  GPU slower than the game let the window's pictures fall further and further behind (at 8x5
  in the busiest stretch: 120 ms late, most VBlanks never shown); now the game waits for the
  GPU instead (every VBlank shown, about 3 ms after it is due).
- **Field pictures** (`PS2X_GPU_FIELDS`, on above scale 1). In a level the game draws two
  half-height fields and interleaves them into the display buffer with `SCANMSK` before adding
  the glow and the HUD. With field pictures the GPU also keeps each field as a whole picture:
  what is drawn with `SCANMSK` 3 goes into the first, with 2 into the second, everything else
  into both, without the mask; each VBlank shows the picture of the field whose lines it would
  show. No lines are filled in, so there are no stripes at higher scales, and every VBlank is
  still its own moment (checked with the view turning: the same 4.5 px every VBlank). At the
  console's resolution the picture stays as the software renderer shows it ("bob").
- **The field shown** (`ps2_host_display::setShownField`): -1 by default, each VBlank's own
  field; 0 or 1 shows the field of the even or the odd buffer rows at every VBlank, for a
  game whose fields are not two moments by turns. Then the GS front end
  (`GS::refreshDrawState`) leaves out what is drawn for the other field's rows alone
  (`SCANMSK` 2 when the even rows show) and draws what is for the shown field's rows
  (`SCANMSK` 3) to every row: the buffer holds the shown field as a whole picture - for the
  passes that read it, too - and all of it shows, in both renderers, with no field pictures
  (the GPU lets a buffer's go when it is drawn into then). Transformers at 60 frames a second
  draws field A alone, interleaves it into the display buffer and blends the odd rows with the
  frame before, so it shows the even ones ([NOTES.md](NOTES.md#frame-rates)): at 7x5
  the GPU drew the scene twice and every effect pass three times before (23.5 ms a frame,
  35-38 fps at 4K), 13.5 ms now. The FPS counter counts one picture a flip. (Weaving the two
  fields instead, line by line of the GPU's field pictures, was tried: at 1x the moving
  player combed, the odd rows being half the frame before.)
- **What a game blends into a field's rows for a TV** (`ps2_host_display::setTvFiltersLeftOut`,
  for a screen that shows whole pictures, one field's or both): the GS front end leaves out a
  `SCANMSK` draw that reads a buffer on the screen now (the frame before: Transformers' trail
  over the odd rows) or its own buffer (its anti-flicker pass, each even row with 30% of the
  row below). The anti-flicker pass only softens a whole field's picture, and above scale 1 it
  reads the row below a few GL pixels down: every edge came out doubled. (Until 2026-10-05 it
  was left out with both fields only: 120 pictures looked sharper than 60.)
- **Both fields as moments** (`ps2_host_display::setFieldsAsMoments`, for the game's in-between
  pictures, [NOTES.md](NOTES.md#frame-rates)): each VBlank shows its own field. With field
  pictures the GPU keeps a buffer drawn by fields as its two pictures alone: what the game draws there
  goes into them only, what reads the buffer reads field A's, and the buffer itself is neither
  drawn, loaded nor kept in local memory (Transformers copies each field over the whole of its
  picture first, and the memory under it - the scene's depth buffer before - is not read for it):
  3.2 ms a frame less at 7x5.
- **Two pictures a frame as a cap** (`ps2_host_display::setSecondFieldDraws`): a game whose
  frame is two fields for two moments says where it draws the second - a buffer and its rows,
  copied into the second field's rows with `SCANMSK` 2. When a picture has to wait for the GPU
  (the one two VBlanks before it is not done), the next frame leaves that field out: the GPU
  renderer drops its draws (by the buffer and the scissor's rows) and that copy, and shows the
  first field's picture at both VBlanks - also when the second's is not valid any more (its
  memory used for something else since; the buffer itself is not kept with field pictures as
  moments, and showing it showed garbage: fixed the same evening). Decided on the GS thread at
  each flip, so it takes effect at once; after a frame that left its own out, only on a wait of
  over half a frame. The FPS counter counts the pictures shown (`secondFieldsLeftOut`).
  `PS2X_SECOND_FIELD_DROP=<n>` leaves it out of every n-th frame as well (testing aid).
- **What was drawn goes to the GPU when the GS thread runs out of work**
  (`GSGpuRenderer::idle`: the batch drawn and a `glFlush`, as the thread goes to sleep): the
  driver had kept a frame's draws until the next picture's `glFlush`, which with Transformers'
  latches (both VBlanks' just before the next frame's draws) came most of a frame later.
- **Texture packs** (`gs_texture_pack.cpp`): PCSX2's replacement format, so the packs made for
  it work here. Files named `<hash>-<CLUT hash>-<bits>.dds` (or `<hash>-<bits>.dds`) in
  `replacements\` (or `textures\replacements\`, or `PS2X_TEXTURE_PACK=<folder>`; `0` turns it
  off) are indexed at start-up and read in the background; a texture the game draws before the
  reader got to it is read on the spot, by whichever thread comes to it first, and nobody waits for
  another file than its own (one lock round every read once froze the GS thread for up to 0.7 s
  behind the reader). A texture the game uploaded is hashed
  as PCSX2 hashes it (`HashCacheKey::Create`/`HashTextureLevel`: XXH3-64 of its 256-byte blocks
  in order when it is at least a block in size and its format fills 32 bits, else of its texels
  unswizzled - indices as bytes, RGBA8 with `TEXA` for 16 and 24 bits; the CLUT as 16 or 256
  RGBA8 colours, `GSClut::Read32`'s order), again only when its pages change. A match is drawn
  from the replacement (BC1/BC3/BC7 or RGBA8 with its mipmaps, sampled over the texture's size
  with the GPU's trilinear and 16x anisotropic filtering for the GS's bilinear, its alpha on the
  PS2's scale as PCSX2 keeps it); textures the GS drew, and the `CLAMP` region modes, are not
  replaced. With the pack for this game (1,367 BC7 textures at 4x, 413 MB), 128 of the 129
  textures in the dropship area are found. `PS2X_TEXTURE_PACK_LOG=1` lists each texture's name
  and whether the pack has it.
- **The texture cache** (2026-10-08, night; `PS2X_GPU_TEXTURE_CACHE`, on by default since the user's OK; native
  batches only, so the emulated path stays exact). Where the pack has no replacement,
  `replacementSlotUncached` asks `decodedTexture` with the hash it made of the texture's memory:
  found by that, the format and size, and the colours the texture takes from the CLUT
  (`clutColour`, kMemory's `lookupClut`: CSM, CSA, TEXA of 16-bit entries and of a CT24 CLUT) or
  TEXA for 16 and 24-bit texels; else decoded from the CPU's copy of local memory - as the GS has
  it at the draw, in the stream's order - with the swizzles of `ps2_gs_psm*.h` into an RGBA8
  texture of the texture's size, the oldest dropped past 3,072 (not one the batch being made
  has). It is bound in a replacement slot (`replacementBound`) with samplers of its own
  (`decodedSamplers`: bilinear or nearest as the GS's, no anisotropy - the pack's 16x changed which
  of the leaves' pixels pass their alpha test), so the native shader samples it as a
  replacement and the batch reads no texture from local memory: no page the game writes later
  ends it (`memoryTexture`). The replacement memo's key has whether a draw may decode. Without a
  pack at 7x5: 13,000 batches every 2 s to 3,400, the in-between's two pictures 22.2 to 10.4 ms of
  the GPU a frame; 128 textures for the way to the Mini-Con, decoded in its first seconds.
- GPU time in the dropship area per VBlank (`PS2X_GPU_STATS=1`): about 4.3 ms of drawing at the
  console's resolution, 8.1 at 5x3 plus 1.6 for the field pictures, 10.1 and 3.6 at 7x5, 11 and
  4 at 8x5. The drawing barely grows with the pixels: the per-primitive cost of the pixel
  pipeline in the shader (about 75,000 primitives a VBlank there) is most of it. A memory
  barrier before each batch looked like the cost, but leaving out the half not needed between
  draws into the same buffers (they are ordered by the interlock) changed nothing.
- **Faster, bit for bit** (2026-10-05, for the in-between pictures at 4K: two fields a frame at
  7x5 took 24.6 ms of an RX 9070 XT, 14.3 now; every step checked identical with
  `check_frames.ps1` at scale 1 and 3). `PS2X_GPU_PROFILE=1` times the batches by pass and
  counts why batches end, `=2` counts the fragments too (and stalls the GPU a batch):
  - Integer division takes a run of slow multiplies on the GPU. The shaders divide by the scale
    in float, (a + 0.5) / b - at least 1/32 from a whole number, so its floor is exact - and the
    local memory address takes shifts for its power-of-two page sizes: the field pictures 2.7 to
    2.1 ms a VBlank, the scene 5.6 to 5.2.
  - Early depth test (`PS2X_GPU_DEPTH_EARLY=0` leaves it out): in a batch whose Z only grows
    (every draw that writes Z tests it GEQUAL or GREATER), a fragment reads the Z there before
    the interlock - through a second, non-coherent binding - and goes where the test fails
    already: the ordered interlock lets no later primitive write the pixel before this
    invocation has ended, and the earlier ones can only have raised it. In the dropship area 21%
    of the scene's fragments fail the depth test (48% are dropped by the alpha test, 31% drawn);
    now they skip the texture and the interlock (draws 6.6 to 5.6 ms a VBlank). A hardware depth
    buffer for the same cannot work: a shader that writes images runs for every fragment unless
    the tests come first, and then the GPU writes its own depth, not the GS's.
  - Program variants with the paths a batch does not take left out (no depth buffer; only
    texture pack replacements), and a buffer's bilinear sample as four loads together.
  - Fewer batches: 16 replacement textures a batch instead of 4, and a draw that takes a
    replacement no longer brings the GPU's copy of local memory up to date first (which ended
    the batch whenever the game had written there): about 30 batches a VBlank in the dropship
    area instead of 110 (draws 5.3 to 4.6 ms a VBlank).
  - Measured and left: the interlock costs the scene a third of its time (without it, wrong
    where primitives overlap, 4.1 instead of 6.6 ms a VBlank); leaving out `coherent` or the
    barriers between batches changed nothing; anisotropic filtering 16x to 4x, 0.15 ms.
- **For 60 frames and 120 pictures with room to spare** (2026-10-05, the user's goal: fixed 60
  with the in-between, easily, everywhere). Measured flat out (`bench.ps1` without `-RealTime`:
  frames a second at 7x5 give the GPU's time a frame - `PS2X_GPU_PROFILE` times every batch
  and makes the GPU look half as fast again), turning the view a full circle near the drop ship
  and the first Mini-Con, the busiest places found: 15.2 ms a frame (16.0 the worst 2 s) to 12.2
  to 12.6 (12.9 to 13.4). Each step bit for bit (the pictures identical with and without it):
  - Both field pictures at once (`GS_DUAL_FRAME`, `PS2X_GPU_DUAL=0` leaves it out): when every
    draw of a batch goes to both of a buffer's field pictures sampling the same, they are drawn
    once, each fragment blended into both inside the interlock - Transformers' glow and HUD,
    drawn over both pictures of every frame. 15.2 to 13.9 ms.
  - Repeated draws folded (`foldRepeats`, `PS2X_GPU_FOLD=0`): a run of one draw entry that is
    the same primitives over and over - the glow is the same four strips over the screen four
    times, adding 5/128 of the glow buffer each time - is drawn once with its blend applied that
    many times (the entry's `texa[3]`), for a draw that adds the same each time (A = Cs, B = 0,
    D = Cd, C = As or FIX) or writes without blending, with no depth and no DATE. Only the entries
    that can fold are looked at (noted as they are made: scanning every batch cost the GS
    thread 1.4 ms a frame). 13.9 to 12.7 ms near the Mini-Con.
  - The picture composed straight into the VBlank's picture when no lines are filled in (the
    field pass was a copy then): a pass of 4480 x 2560 a VBlank less.
  - The GS thread's vertices (`appendPrimitives`): the members it reads in locals and room for
    a call's vertices at once - it was half of the GS thread's time (450,000 vertices a frame);
    the thread's floor (flat out at scale 1, where the GPU does not count) 13.0 to 10.9 ms a
    frame near the drop ship, 11.3 to 9.9 near the Mini-Con.
  - Measured and left: the interlock's cost is about 20 ns a primitive whatever its size (the
    same at 1x as at 7x5: 1.5 ms a VBlank of the scene's 70,000 triangles - three quarters of
    them under 10 GS pixels), unordered as much as ordered. A compute renderer drawing the
    triangles by tiles of 16 x 16 (and then 32 x 32) GL pixels - binned, each tile's in order,
    each pixel's frame and depth in registers, no interlock - drew the same pictures (mean
    difference 0.002 a channel) but took 8.9 ms a VBlank to the draw program's 4.6: binning
    1.3, the loop over a tile's triangles 4.5 (latency), the shading 2.5. Not kept.
  - In 16:9 (2026-10-06; the save states are 4:3, so the game's widescreen switched on with
    `TF_POKE`, see [DEBUGGING.md](DEBUGGING.md)): the wider view (88.7 degrees across instead of
    70) has more to draw. At 7x5 near the drop ship 12.97 ms a frame (13.79 the worst 2 s; 12.34
    and 12.93 in 4:3 in the same session), near the Mini-Con 13.02 (13.67; 12.70 and 13.42): 17
    to 19% to spare in the worst stretch. Near the drop ship it is no longer the GPU that sets
    the pace: flat out at scale 1 the frame takes 12.86 ms (11.06 in 4:3), the GPU renderer's
    own thread 90% busy - `appendPrimitives` 37% of it, `GSCpuBackend::UploadImage` (the
    textures sent each frame, copied into the CPU's picture of the GS memory too) 12%, heap
    allocations 5%, the GL driver 8% - and the EE 87%.
- **Native meshes** (2026-10-06, stage 2 of [RENDERER.md](RENDERER.md#stage-2-in-detail); on
  by default when the GPU renderer draws alone, `PS2X_GPU_MESHES=0` leaves them out). A game's
  native VU1 code (`ps2vu1native`, `runtime/ps2_vu1_native.h`) can send a batch as a packet of
  its own (`runtime/gs/gs_native_mesh.h`: a marker tag no game sends, then per field its matrix,
  register writes and vertex tag, the drawing-kick masks and the vertices as uploaded) through
  the same PATH1 and GIF arbiter, so it reaches the GS front end in its place in the stream. The
  front end applies each field's register writes and PRIM as the routine's packet would, brings
  the draw state up to date and hands the renderer the field (`GSThreadedBackend::SubmitMesh`, a
  ring record `Op::Mesh`); where the renderer cannot take it (the software renderer drawing, a
  display source or target log in use, a strip it does not know) it makes the routine's packet
  itself (`meshFieldPacket`) and decodes that. The renderer keeps three vertices of room for
  each triangle the masks kick and a compute pass (`kMeshExpand`) fills them after the vertex
  upload, before the batch is drawn: each corner through the field's matrix, Q = 1/w, 12.4
  fixed point, Z, STQ, the colour and the fog, as `appendPrimitives` would have them. In VU1's
  arithmetic - a product or a sum rounded toward zero, which a GPU does not do: the product
  rounded to nearest with its exact error taken in double precision (a float times a float is
  exact in a double; the AMD driver did not fuse `fma` even with `precise`), a sum's error by
  TwoSum (kept from being reassociated by a uniform zero XORed in), a reciprocal checked by its
  residual in double, then a step toward zero where the error says so - so the vertices are
  the CPU's bit for bit (`PS2X_GPU_MESHES=3` makes every vertex on the CPU too and compares:
  0 of 660,000 different). Draws into a field picture's shadow batch take the CPU's exact
  expansion (`gsmesh::fieldTriangles`). Only `0x1410`'s drawing so far - two thirds of the
  vertices near the drop ship. In real time at 4K, 60 frames and 120 pictures near the drop
  ship, off and on: the front end 68 to 38% busy, the VIF1 thread 47 to 41%, the whole process
  3.04 to 2.62 cores.
- **The texture pack's lookup remembered** (2026-10-06): with meshes every field writes PRIM,
  so the draw state is prepared about 1.4 million times a second near the drop ship (flat out at
  scale 3), and each
  time looked the texture up in the pack: its pages listed (a new list each time), two hash
  lookups. The last answer is kept while nothing it depends on changes - the same texture and
  TEXA, filter and wrap, no page written or drawn since (`writeClock`), the same CLUT, the same
  batch (its replacement slots) - and the page lists reuse their storage. `PS2X_GPU_PACK_MEMO=2`
  checks every remembered answer against the full lookup: 82 million on three save states, none
  different. The renderer's thread 78 to 66% busy in real time near the drop ship.
- **The native pixel path** (2026-10-06, stage 3 of [RENDERER.md](RENDERER.md#stage-3-in-detail);
  on by default since 2026-10-08, `PS2X_GPU_NATIVE=0` the exact emulated pipeline). `prepareDraw` decides per draw (`nativeDraw`): a 32- or
  24-bit frame, Z24/Z32 or no depth, no field pictures, no DATE, AFAIL KEEP, the frame mask in
  whole bytes, a blend with factors within 0 to 1; a change between native and emulated ends the
  batch (flush reason "native"), as does a draw that would write alpha into a batch that reads
  Ad from its copy, or the other way round. A native batch keeps runs (`nativeRuns`: first
  vertex, ZTST and Z write) and is drawn by `kNativeVertex`/`kNativeFragmentMain` into an FBO
  with the frame's RGBA8 view (`Target::view`) and the depth target's `nativeDepth`, blend
  `ONE, SRC1_COLOR, ONE, SRC1_ALPHA`, the runs' depth state set between `glDrawArrays` calls.
  `nativeDepthReady` (`kNativeUnpack`, the depth from the R32UI texture) and
  `nativeDepthCurrent` (`kNativePack`) keep `Target::depthCopy` (0 texture newer, 1 the same,
  2 native newer); the emulated flush, image sources, `feedbackCopy`, `writeBack`, `allocate`,
  `ensureLoaded` and eviction pack first, and an emulated batch that writes Z or a load marks
  the native copy old. With `PS2X_GPU_STATS`, `[gpu]   native:` - batches, runs, draws
  (emulated ones), depth packs and unpacks. The native vertex shader (`kNativeVertex`, after
  `kDrawStruct`, the draw entry's layout it shares with the draw programs) reads the draw's entry
  and hands the pixel shader what it needs, worked out a vertex: packed switches and values
  (`vState`), the scissor in the target's pixels, the blend's factors (`vBlend`), the bytes the
  frame mask keeps (`vKeep`), a replacement's coordinates (`vPack`); the pixel shader does the
  GS's integer arithmetic in floating point (exact below 2^24), and reads the draw's entry only for a texture in local
  memory or a buffer it does not read through a view. A 32-bit buffer read as C32 or C24 and
  clamped is sampled through its RGBA8 view (`uSourceView`, units 16 to 19: `Target::view`,
  `Target::shadowView`, a moment layer's `view` - `sourceViews` and `ShadowBatch::views`, a bit
  each in `uSourceViews`) with the texture unit's filtering (2026-10-08). Transformers' depth fog in
  a native batch (`nativeFog`, `PS2X_GPU_NATIVE_FOG`): the slot of its T8H read of the depth
  buffer's block (`momentFogSlot`, the moments' rule) is bound to a fog index made from the
  batch depth's native depth (`nativeFogIndex`, `Target::fogIndex`; the buffer read neither loaded
  nor made current), and `prepareDraw` leaves out a draw into a depth buffer through a depth
  format's view that reads its own buffer (the shuffle that made those top bytes:
  `statFogShufflesLeftOut`).
- **The scene multisampled** (2026-10-08, night; `PS2X_GPU_MSAA`, 4x by default since the user's OK, `Impl::sceneMsaa`). A native
  batch with a depth buffer whose frame and depth are C32 and Z32-kind buffers not kept as field
  pictures (`msaaFor`) draws into their multisampled copies (`Target::ms`, renderbuffers: RGBA8,
  D32F) through `msFbo`. `msStale` - an emulated batch, a native one not multisampled, a load,
  `nativeDepthReady`'s unpack - has `msaaFill` (`kMsaaFill`, through `msOneFbo`) copy the buffer in
  before the next such batch; `msNewer`, GS rows `msY0` to `msY1`, has `msaaResolve` copy them out
  before anything else uses the buffer (`flush` for any other batch into it, `prepareDraw`'s source
  read, `feedbackCopy`, `writeBack`, `allocate`, `ensureLoaded`, `presentPicture`, `ensureShadow`,
  `nativeDepthCurrent`, `nativeFogIndex`, the alpha copy): the colour blitted into `msResolveTex`
  and its bytes copied into the R32UI texture, the depth into `nativeDepth`, its rows marked newer
  there - blitted (one sample a pixel, the rows kept as `msOneY0`-`msOneY1`), or for a batch that
  tests against it (`flush`'s resolve for a batch not multisampled, `farthest`) the farthest of each
  pixel's samples: the renderbuffer's rows copied into `msDepthSamples` (a multisampled texture,
  `glTexImage2DMultisample` - the loader, run by raylib for its 3.3 context, has no
  `glTexStorage2DMultisample`) and read by `kMsaaDepthResolve`, again over rows handed back as one
  sample while the copy is still the buffer's. `PS2X_GPU_MSAA_PIXEL_DEPTH=1` draws the batches with
  `nativePrograms[2]`/`[3]` (`GS_MS_DEPTH`: `gl_FragDepth` the GS's Z a pixel, `floor(vZ + 0.5)` as
  the emulated path has it). `dropNative` drops the copies (`msaaDrop`). With `PS2X_GPU_STATS`,
  `[gpu]   the scene multisampled:` - fills, resolves by what needed them (`statMsaaResolveWhy`),
  and the depth as the farthest sample (`statMsFarthest`).
- **Field pictures drawn natively** (2026-10-07, with `PS2X_GPU_NATIVE=1`; `PS2X_GPU_NATIVE_FIELDS=0`
  turns it off): a buffer whose field pictures are the buffer (`fieldsAsMoments`) has its shadow
  batches drawn through an RGBA8 view of each picture (`Target::shadowView`) with the native
  program, when every draw of the batch is one the native path takes and none reads Ad of a
  32-bit frame (`batchShadowNative`); folded as the emulated ones are (`foldRepeats`: the native
  shader adds the source's part the entry's repeat count of times - 2026-10-08; before, the GPU's
  blend drew each repeat). Transformers' field copies, glow and HUD: 0.6 to 0.37 ms a picture at
  7x5 (timer queries of the profile); against the emulated ones 99% of the pixels identical, at
  most 3 off. With the glow folded and the copies read through views, 0.57 ms a picture flat out
  at 7x5 with three moments a frame (0.74 without the views).
- **ZTE 0** (2026-10-08): `GS::buildDrawState` turns a draw state with the depth test off (TEST bit
  16 clear) into ZTST ALWAYS with the Z mask on - every pixel passes, no Z written, as on the GS -
  for every renderer (before, ZTST was taken as it stood: Transformers' two alpha resets, ZTST 0,
  drew nothing).
- **Moment pictures** (2026-10-07, [RENDERER.md](RENDERER.md#stage-4-in-detail);
  `ps2_host_display::setMomentFields`, Transformers' `TF_FPS_LOCK=refresh`, the settings' Frame
  rate or `TF_MOMENTS=1` - `g_moments` in `overrides.cpp`, `tf::port::momentPictures()`, switched
  at a frame by `takeSettings`; needs the native path; the default since 2026-10-08 where the
  native path is on, `TF_FPS_LOCK` unset and no `TF_FRAME_RATE` given).
  `present` at a flip picks the next frame's moments (`pickMoments`: the screen's refreshes while
  the frame will be up, at most four, fewer under the cap `momentCap`); `prepareDraw` marks a
  batch into the buffer of both fields `batchMoments` (no culling, the buffer and its depth
  buffer neither loaded nor drawn); `drawPrimitives` and `addMeshJob` note its runs
  (`momentSegments`); `flush` pairs them into pieces (`prepareMoments`), keeps room for the
  moments' vertices after the batch's own, makes them (`kMomentLerp`, a thread a vertex, the
  piece found by binary search) and draws each moment into its layer (`drawMoments`), the depth
  fog's source slot replaced by the moment's own depth (`kMomentFog`). Draws into the display
  buffer go to a field picture per moment (`Target::shadow[2 + m]`), the copy of field A reading
  the moment's layer; `presentPicture` composes and publishes them all at the flip, each with
  `Published::showsNs`, in twelve picture slots, and waits for the GPU two presentations back
  (`PS2X_GPU_MSAA`: each layer drawn into multisampled renderbuffers, `msFbo`, and resolved by
  `resolveLayer` before the alpha copy, the fog's index and the copy into the picture - the colour
  through `MomentLayer::resolved`, a plain RGBA8 texture, since a blit into the R32UI texture's
  view wrote other values)
  (`presentedSlots`) rather than two pictures. The cap (`present`, at each flip): a strike for a
  wait of more than 4 ms there, or for a moment handed over after its refresh since the last flip
  (`ps2_host_display::momentsMissed`, counted in `noteMomentMade` with or without the
  statistics); two within about 20 frames take a moment off (`momentCap`, down to one), none
  counted in the first 60 frames of moments or the 10 after a cut (`momentQuiet`);
  `momentCalmNeeded` frames without one put one back (60, doubled up to 240 when the one put
  back is cut within 120 frames, 60 again after 600). `PS2X_GPU_STATS` adds `[gpu]   moments:` (frames,
  pictures, pairs mixed, runs alone, the cap) and `GPU ms a frame`. In perspective (2026-10-08,
  night): a piece of an STQ draw (`MomentPiece::flags`) is mixed by `t * Q_B / ((1 - t) * Q_A +
  t * Q_B)` for its place, depth and S, T, Q, its colours and fog by `t` - off by default since the
  same night, `PS2X_MOMENT_PERSPECTIVE=1`: pieces without the Q beside them, mixed on the straight
  line, parted from them along shared edges halfway between the fields). Carried (the same night, `PS2X_MOMENT_CARRY`): `carryFrom` notes each paired
  packet run (`CarryRun`: its kind - the draw entry's words that stay frame to frame and its
  length - its first vertex, and for a carried kind its vertices in `carryStore`); `carrySwap` at
  the flip makes them the frame before's, a kind's runs in their order (`carrySlots`, an
  open-addressing table, and `carryOrder`, counted and placed: nothing sorted or allocated once
  the vectors have grown). A run whose field B is where the frame before left one of its kind
  (the one in its place in the order first, then the nearest of them by the first vertex: within
  0.13 px) stays as it is; one moved within `kCarryReach` (32 GS pixels) marks its kind carried
  (`carryKinds`); a carried kind's run takes the frame before's field A vertices (appended after
  the batch's own, `shift` 0) from the run nearest it by all its vertices, when no other is near
  (the next at least four times as far and a pixel). Kinds of more than `kCarryDense` (128) runs a
  frame are left alone. `[gpu]   carried:` with the statistics.

### Widescreen

On the console a game's 16:9 mode draws the same buffer for a TV that stretches it; the
runtime does what that TV did, and keeps the 2D in shape.

- **The TV setting.** `PS2X_WIDESCREEN=1` makes `GetOsdConfigParam` report a 16:9 TV
  (`screenType` 2, `Kernel/Syscalls/Helpers/Runtime.h`), which is what games read
  (`sceScfGetAspect`) for their default.
- **The picture's shape.** Only the game knows whether it draws for 4:3 or 16:9 at a given
  moment (menus, movies and levels differ), so its code says so
  (`ps2_host_display::setGameAspect`; Transformers reads its renderer's display mode each
  frame and says 4:3 while a movie plays). The window, full screen included, fits the picture
  in that shape.
- **2D drawn for 4:3.** A game whose HUD, menus and text are drawn the same in both modes
  shows them a third too wide on 16:9. With `ps2_host_display::setOverlaySqueeze(0.75)` the GS
  front end (`gs_frontend.cpp`, so for both renderers) narrows them back into shape. Each GIF
  tag from the EE (PATH2 or PATH3; `ps2vif1::gifPath`, which the arbiter sets for each packet
  it lets through) is scanned for the range of its vertices before it is processed (packed
  XYZ data has Y in bits 32-47, a register's own value in bits 16-31); its primitives are
  narrowed (a copy is submitted: a strip's next primitive reuses its vertices) when they go
  into a buffer that one of the enabled circuits showed lately, at the width it was shown with,
  with the depth test off or ALWAYS, the scissor across the whole buffer, a texture that is
  not in a frame, depth or shown buffer, and the tag not spanning the buffer's width. Whole
  tags decide, so a bar or a fade drawn as a strip of 32-pixel columns in one tag stays full
  width; VU1's 3D (PATH1) is never touched. What the rule excludes in Transformers: the fades,
  the cinematic bars, the menu backgrounds, the post-processing passes (full width), a depth
  effect drawn as 32-pixel sprites with a tag each (by the texture test), and a 256 x 256 work
  picture drawn into the next display buffer's memory, its edges cleared with lines (by the
  width: until 2026-10-06 those lines were narrowed into its columns 31 and 223).
- **Where the 2D goes** (2026-10-06). Narrowed about the middle of the screen, a HUD keeps its
  4:3 layout inside the middle of the picture - the health bars a sixth of the way in, not in
  the corners where a widescreen game has them. So each tag is an element, and elements that
  overlap make a group (a panel and its bars, the pieces of a ring, a banner with its icon and
  text, a box with its text). A group in the outer quarter of the 4:3 picture (its middle
  there, not reaching the picture's middle) is narrowed about that edge, keeping its distance
  to it as on a 4:3 screen of the same height - unless it touches the edge: a HUD keeps clear
  of the edges for a TV's overscan, a menu's banners and boxes start at them. The rest goes
  about the middle as before. The pieces come one by one and a group is seen whole only at the
  end of the frame, so an element takes the anchor its group had in the frame before (the same
  element: the two overlap by at least half of what they cover together), or else that of an
  element drawn before it in this frame that it overlaps, or else goes by its own place: a
  layout that stays put keeps together whatever order its pieces come in, and a new one is
  placed by its first pieces. A frame ends at a flip (`GS::latchHostPresentationFrame`) or when
  the 2D goes to the other buffer. In Transformers the panels at the top, the Autobot ring
  and the "Autosaving" message go to the sides; the reticle, the pickup menu (its slide
  out too), the HQ call's prompt and the Drop Zones' prompts stay as they were, and the HUD
  comes back in the corners on its first frame after the menu. `PS2X_OVERLAY_ANCHOR=0`
  narrows everything about the middle again, `TF_WIDE_HUD=1` leaves the 2D stretched,
  `PS2X_OVERLAY_LOG=1` logs one frame's elements and groups in 120.

## Sound

**Since 2026-10-22 the sound server runs natively by default** (`transformers/src/native_sound*.cpp`;
`TF_NATIVE_SOUND=0`, the Sound tab's "Sound engine", for the emulated one below):

- **The protocol** (`native_sound.cpp`). The game's sound classes reach SIRENS3 only through
  `slSIFCommander` (USA addresses): `0x1F0470` SendAsynchronousCommand (the 41 commands, one RPC
  function each, the arguments and results in the transport's aligned encoding; command 4 is
  the per-frame packet of tags), `0x1F0170` SendAsynchronousMemoryRequest (the EE's pull of the
  server's 8 KB status buffer) and the device's transfer start `0x1E8B20` (movie audio, 8 KB
  chunks). All three are replaced: the commands and tags drive the engine, the status buffer
  is written from it (timings, statistics, pending loads, the sorted playing list, every
  stream with its next tune-part index, the sequencers, the priority lists, `0xCAFEF00D`), the
  chunks go to the engine's stream and the slot reads as done. Movie audio's credits (the
  IOP's SIF command 4) are added to the EE's counter (`0x6CE8E4`) as the engine plays chunks.
  At `Sys_Startup` the IOP is parked (`PS2Runtime::setIopParked`, patch 0016): nothing else in
  this game needs it after boot, and its interpreter and SPU2 stop costing the EE thread.
- **The engine** (`native_sound_mixer.cpp`, data in `native_sound_data.cpp`). Tone libraries
  from SOUNDE.ZIP (.TLB + .VAC text, .TLA binary FISH; tones decoded from SPU ADPCM, loops from
  the block flags; instruments' regions and ADSR in seconds); voices at 48 kHz with the SPU2's
  pitch register (rate x 4096 x factor / 48000, clamped to 3FFFh), 4-point gaussian
  interpolation and 15-bit volume words (bit 14 phase-inverted), 48 channels less the streams'
  reserved ones, lowest priority stolen, playing until the envelope is back at 0 or the data
  ended; .MS streams (channel-interleaved ADPCM) and .ALB_T albums (parts with repeat counts,
  9999 for ever) with per-channel loudness (the four stereo stems of the music) and the next
  part index; movie audio (16-bit PCM in 0x200-byte rows on this disc); the SPU2 reverb
  algorithm on a work area of its own, with libsd's presets read from the disc's LIBSD.IRX
  (0x4B88: Room ... Pipe, 32 registers each, work-area sizes before them). Mixed up to the
  game's EE clock at each frame packet into `ps2_audio_stream`.
  Levels as the emulated chain's (measured A/B: menu music 1.00x at correlation 0.996): a
  stream's voices take the channel loudness (tag 0x49) as their volume words, the loudness (tag
  0x40) only where no channel loudness came; the whole mix is halved (the emulated SPU2's output
  sits at half of tone x envelope x volume word, music and voices alike).
- **The disc** (`native_disc.cpp`, `TF_NATIVE_DISC=0` for the IOP's). The game reads its files
  through TwoFish, its own IOP disc server (EE side `0x192FF0` sends a request: LSN, sector
  count, buffer; `0x192FB0`/`0x192FE0` wait on its status). With the IOP parked those requests
  were never answered (the boot stalled at the first load after `Sys_Startup`), so the request
  is answered on the host: the sectors from `PS2Runtime::readDiscSectors` (patch 0017: the IOP
  CD module's own reader, the same LSNs over the disc image or the virtual disc of a folder) go
  into the buffer and the status is final at once. A chunk running past the last file reads as
  zeros there. The IOP is parked only when the disc is native too.
- `PS2X_SPU2_LOG=2` (patch 0018) logs the emulated SPU2's voice volumes, pitches, envelopes,
  key on/off, gates and core volumes; `TF_NATIVE_SOUND_KON=1` logs the native engine's key-ons in
  the same terms (and each tone's length, level and loop), for comparing the two.
- `TF_MUSIC_LOG=1` (`sound_trace.cpp`, either server) logs the EE's level music as the game decides it:
  `taMusicManager`'s intensity each time it or the threat behind it changes (creatures near / in view /
  aware / alerted, threat, the player's speed, the playing and target stem pair, the duck), its
  `Switch` calls, the music file it opens or crossfades to by index (a level starts on its warp gate's
  `WarpGateMusicTrack` entry of METAGAME.TDB; `NextMusicTrack` triggers step it), stings and the duck,
  and `slMusicStream_PlayStation2`'s channel loudness / loudness / part calls. The same game calls run
  on both servers, so two logs (`TF_NATIVE_SOUND=0` and `=1`) show whether a difference is the server's.
- **Channels and priority** (TODO 106, 2026-11-01). When the 48 channels less the streams' (38 in a
  level with music and ambience) are all in use, the voice with the LARGEST priority number gives way:
  SIRENS3's steal fallback (IOP 0x4DE74) sorts the playing voices by priority (voice +0x1C, the tag 0x22
  value) and takes the last, and the EE's sound-node pool does the same. The game's numbers: 9 for the
  immediate voices (the player's weapons and Mini-Cons, the car engine, the interface), group x 10000
  + priority for 3D ones (5xxxx impacts, alerts, landings, footsteps near; 6xxxx birds, monkeys,
  ambience, far footsteps). The native engine had the order the wrong way round: at the limit, which a
  busy level reaches (38 voices measured on both servers), birds and footsteps took the player's car
  engine and gun. A packet's key-ons are also made at its channel assignment (tag 0x25), after its
  voice updates: the EE sends a new voice's play before its first update. `TF_NATIVE_SOUND_PRIO=high`
  / `TF_NATIVE_SOUND_DEFER=0` give the old behaviour.
- `TF_NATIVE_SOUND_REQ=1` (native engine): every key-on with its outcome (keyed, `steals h ...`,
  `REFUSED`, `NO TONE`), key-offs, and each voice's end with its length and loudest level (peak 0 =
  asked for but never heard). `TF_SOUND_LEDGER=1` (`sound_ledger.cpp`, either server): read on the EE
  side, the game's requests to the sound list (`ask 3d|imm ... -> node | NO NODE`), every key-on sent
  (`play h <handle> '<tone>' prio vol`), and from the server's status buffer whether it was ever listed
  as playing (`NEVER`) and how it ended (`stopped` / `not stopped`), with the peak number of playing
  voices - the same lines for both servers, so a run with `TF_NATIVE_SOUND=0` is the comparison.
- Protocol notes: `TF_SIRENS_LOG` /
  `TF_SIRENS_SCHEMA` (`sirens_trace.cpp`) record what the game sends.

The emulated path: the game's own sound driver (SIRENS3, an IOP module) runs in the IOP
emulator as it would on the console; what was missing was the hardware under it.

- **SPU2** (`iop_spu2.cpp`): two cores of 24 ADPCM voices over 2 MiB of sample memory at
  48 kHz (one sample every 768 IOP cycles). Modelled: ADPCM decoding with loops, the voices'
  4-point interpolation through the hardware's 512-entry table (psx-spx's "gaussian": a cosine
  window, every four taps summing to 7F80h; until 2026-10-09 a cubic curve,
  `PS2X_SPU2_INTERPOLATION=cubic`), pitch and pitch modulation, noise, ADSR envelopes, volume
  sweeps, the mixer's dry and wet paths with their gates and volumes, each core's reverb (below),
  the capture areas (voices 1 and 3, the cores' dry and wet mixes and core 0's output written to
  sample memory each sample), sample memory through the data port and through DMA (channels 4
  and 7) in both directions, the address interrupt (data, voices, reverb and capture accesses),
  and the "AutoDMA" streams of 16-bit samples that movies and streamed PCM use. Not modelled: the
  optical output modes. It is brought up to date whenever its registers are touched and runs
  alongside the IOP otherwise; it is part of a save state.
- **Reverb** (since 2026-10-09; `PS2X_SPU2_REVERB=0` without it): per core, the sum of the
  voices sent to it (VMIXEL/VMIXER) and of the stream and the external input where MMIX sends
  them wet, through a 39-tap half-band filter down to one channel a sample (left, then right),
  the PS1's reverb formula on the effect area (ESA to EEA's xFFFF, the 22 address registers from
  `0x2E4` relative to a position that moves a word every second sample, the 10 coefficients from
  `0x774` among the cores' volumes), and the same filter back up (twice its gain over the zeros
  between); the result times EVOL joins the core's dry mix before its master volume. The 14
  accesses of a step go in the hardware's order, every step saturates to 16 bits, ATTR bit 7
  clear turns the writes into reads, and vIIR -8000h negates what the reflections store
  (psx-spx, "SPU Reverb Formula", "Reverb Precision"). Transformers uses the SDK's "Room" on both
  cores at 0.2; which voices it sends there depends on the place. The filters' 64-sample
  histories are a save-state section of their own (`RVRB`; older states load without it, the
  histories empty). `PS2X_SPU2_LOG=1` logs the reverb's set-up as the game writes it.
- **Host playback** (`ps2_audio_stream.cpp`): the samples arrive in bursts at the pace of
  the emulated machine; the host's device takes them steadily. A buffer of about 80 ms sits
  between, and the playback rate follows its fill (0.72–1.25, averaged over about 1.5 s), so
  a machine that runs a little slow sounds a little low instead of crackling. It fades out
  when it runs dry and drops what turbo mode produces.
- `PS2X_AUDIO=0` keeps the host device closed (the samples are still produced and can be
  dumped; `run.ps1` sets it unless `-Sound` is given), `PS2X_AUDIO_VOLUME=<0..1>`,
  `PS2X_AUDIO_DUMP=<file.wav>` writes everything the SPU2 produces (`run.ps1 -DumpSound`),
  `PS2X_AUDIO_STATS=1` prints the buffer, rate, underruns and drops.
- **RPC servers on their thread.** SIRENS3's "Primary RPC" thread serves the game's sound
  commands; loading a tone library sleeps on semaphores and DMA. The IOP emulator used to
  call RPC server functions directly with an instruction budget, which cut that load off
  halfway. They now run on the server's own thread (see the fixes above).
- **Keeping the IOP in step.** The IOP clock is the EE's divided by 8, run in batches of at
  least 8,192 EE cycles. IOP code run on the EE's behalf (module loads, RPCs) is charged
  against later EE time, at most 4 ms ahead; otherwise the sound driver's streamer, which
  asks for one movie sound block per block played, ran ahead of the game feeding it.
- **The interpreter.** The IOP runs on the EE thread, about a seventh of its time at 60
  frames a second: the game sends the sound driver a buffer of commands a frame
  (`sid 0x514E95`, function 4, no wait) and the driver works through them at once. An
  instruction is read once (the import-stub check and the execution share it), RAM reads are
  inline, and the DMA start is looked at only when one is pending.
- **Native routines** (`PS2X_IOP_NATIVE=1`, off by default). The sound module links
  libgcc's soft-float `__floatsisf` (int to float), which normalizes one bit a step: a third
  of all IOP instructions in a level. Found by its code when a module is loaded (or a state
  that holds one), its first instruction becomes a trap (opcode 31, which the R3000 does not
  have) and the conversion runs natively, rounded as fp-bit's `pack_d` rounds and charged the
  instructions the routine would have taken (checked against the module's own code in a MIPS
  model over 5,000 values). The EE thread's frame goes from 14.7 to 13.3 ms. Off by default:
  an interrupt or the end of a time slice that would have come inside the routine comes after
  it, and the scripted Mini-Con fight of `check_frames.ps1` then plays out differently.

## Timing

- The EE clock counts cycles of executed guest code (294.912 MHz) and drives the timers, COP0
  Count and the IOP.
- A VBlank is scheduled every 16,667 µs of both clocks (or the period a game asks for:
  below): it fires when the EE clock has reached its cycle deadline **and** the host clock its
  time. Guest code that is fast on the
  host waits for the wall clock; guest code that is slow gets VBlanks no faster than its own
  cycles allow, so the game slows down instead of dropping logic. `TF_FPS=1` prints both
  rates: below 60 VBlanks per second the game runs slower than real time.
- One schedule maps EE cycles to host time (`hostTimeOfCycle`), set at every VBlank start;
  alarms and idle timers use it too. A VBlank is 60.0 Hz (the console's is 59.94), or the
  screen's refresh when that is the VBlank rate or a multiple of it (see
  [Presentation](#presentation)).
- **The VBlank period** (`EeScheduler::setVBlankPeriod`; 0 or `kConsoleVBlankPeriod`, 16,667 µs,
  is the console's). A PC port's frame rate: a game that simulates a frame per VBlank runs at
  the VBlank rate. It takes effect from the VBlank after the one already scheduled; the
  cycles of a VBlank (`vblankPeriodCycles`), the restart after a stall (two periods) and
  `ps2_host_display` (its wait for a flipped picture and the lead before a refresh, both in
  proportion) follow it. What counts VBlanks as the console's time does not: the MPEG
  player's picture clock (`MPEG.cpp`, two fields a picture) and the disc stream's production
  (`CD.cpp`, sectors a VBlank: a faster disc) - Transformers keeps the console's period while
  a movie plays. Transformers runs its levels at `TF_FRAME_RATE` this way
  ([NOTES.md](NOTES.md#frame-rates)): at 120 a VBlank every 8,333.5 µs.
- A VBlank fires no earlier than its host time, but later when the game is busy: in a
  level the one between two flips comes when the frame's work is done, about 28 ms after the
  one before. The window does not follow that; it shows the VBlanks on the host's schedule.
- After a stall of more than two VBlanks the pacing restarts one VBlank period before "now",
  so the game neither races to catch up nor idles right after a late frame.
  `PS2X_PACE_STATS=1` prints every 120 VBlanks how late they were on average and at most, and
  how often the schedule restarted.
- `PS2X_TURBO_VSYNCS=<n>` runs unthrottled until VBlank tick `n`: the scheduler moves its
  pacing clock forward instead of waiting. Runs in turbo mode are repeatable.
- **Headroom.** `EeScheduler::idleHostTime` sums the host time the EE thread waited for the
  host's clock (a VBlank or another event due later than the guest got there): a game's frame
  less that is the frame's work. Transformers adapts its frame rate by it, and `TF_FPS` shows
  it a frame.
- **Idle skip.** The game waits for a VBlank in a loop that only polls a counter. Executing
  that loop until the next VBlank cost several milliseconds of host time per frame.
  `EeScheduler::skipIdleCycles` moves the EE clock to the next scheduled event in steps of at
  most 65,536 cycles (so timers and the IOP keep up); `overrides.cpp` calls it when the wait
  is re-entered at its loop. See "Hooks" in [NOTES.md](NOTES.md).
- **Cycle scale** (`PS2X_EE_CYCLE_SCALE=<factor>`, `EeScheduler::setCycleScale`). Executed
  guest code is charged that factor of its cycles: below 1 the EE runs faster than the
  console's, so more of the game's code fits between two VBlanks. Idle skips and the HLE's
  own charges are not scaled, so VBlanks, timers and the IOP keep their pace.
  `setCycleScale(0)` goes back to the setting (Transformers charges 30/R while a level runs at
  R frames a second: half at 60, a quarter at 120).
- **EE timers in steps.** Cycles are accounted at every checkpoint (every function call and
  loop back edge of the recompiled code). The timers get them in steps instead, as far as the
  next interrupt they can raise (`PS2Memory::advanceEeTimers`/`catchUpEeTimers`: the clock
  remainder carries the fractions, so one step is several exactly); a read or write of a
  timer register, an idle skip and a save state first bring them up to date. The FIFO count
  in `GIF_STAT` still goes back to 0 at every checkpoint, looked at only when it was set.
- **Events.** A checkpoint with nothing due by the EE clock does not take the event lock or
  read the host's clock, and the events taken from the queue go into storage that is kept.
- **The checkpoint inline.** `EeScheduler::checkpointDue` is in the header, so the generated
  code's `PS2Runtime::eeCheckpointDue` is one call: the cycle scale, the clock, COP0 Count, the
  timers' step and the IOP's cycles, then the due tests; the end of a time slice goes out of
  line (`timeSliceExpired`).
- **The IOP's cycles held.** The IOP runs in batches of 8192 EE cycles
  (`IopEmulator::runEeCycles`); `PS2Runtime::advanceIopEeCycles` holds the cycles until its
  next batch is due (`IopSubsystem::eeCyclesUntilRun`) and hands them over before every call
  into the IOP (RPCs, SIF commands and transfers, modules) and a save state: the same batches
  at the same checkpoints.

## EE loads and stores

- The recompiled code's loads and stores (`READ32`, `WRITE32`...) go straight to main RAM
  unless the address is special (`Ps2IsSpecialAddress`), which main RAM below 32 MB is tested
  for first. Reads of the scratchpad (16 KB at `0x70000000`) come straight from its memory
  when aligned; its writes, and everything else special, go through the runtime (a store
  there also runs the DMA handlers that have completed).

## The generated code

`FunctionEmitter` (`ps2xRecomp/src/lib/function_emitter.cpp`) writes a C++ function for each
guest function: a `switch (ctx->pc)` over the places where it can be entered again (after a
call, at a loop's checkpoint, after a syscall), then each instruction as the translators give
it, with `ctx->pc` stored before it; `ControlFlowEmitter` writes the branches and calls. Three
settings in `transformers/config/overrides.toml` (`[general]`, passed on by `make_config.py`)
make that code faster without changing what it does:

- **Registers in locals** (`register_cache`; `ps2xRecomp/src/lib/register_cache.cpp`, the
  macros in `ps2xRuntime/include/ps2_register_cache.h`). The translators reach the guest's
  registers through `GPR_*(ctx, n)`, `SET_GPR_*(ctx, n, v)` and `ctx->f[n]`. So every
  instruction loaded its operands from the context and stored its result there, and as a guest
  store may alias the context as far as the C++ compiler knows, nothing stayed in a host
  register across one: a chain of dependent instructions went through memory. A pass over each
  function's text turns them into locals, `rc_<n>` (the low 64 bits) and `rf_<n>`, loaded on
  entry. Reads come from the locals. Writes go to the local and straight through to the
  context (an 8-byte store; the upper half of a general register stays in the context, and a
  128-bit read joins the two halves), so the context is current for anything that looks at it:
  calls, syscalls, returns, the scheduler, a syscall's `EeDispatcherTransfer`. After anything
  that may change the context's registers (a call through `dispatchGuestBranch`, a syscall, a
  stub, any other call given `ctx`) the locals are loaded again; the compiler drops the loads
  of those not read before they are written. Calls that never touch the registers (memory, the
  FPU's control word, the write trace, `SignalException`) are known by name. Register 0 keeps
  the macros: a constant, and a write dropped unevaluated. The FPU's bit moves (`mtc1`, `mfc1`,
  `cvt.w.s`, `cvt.s.w`: `std::memcpy` to or from `ctx->f[n]`) become `std::bit_cast`, so no
  local's address is taken. The pass knows the emitter's shapes; a function with anything else
  is left as it was, with a `register-cache` warning in `work\recomp.log`. In this game there is
  none: 21,788 functions are rewritten, the rest name no register. (A 128-bit read first joined
  the halves with a `memcpy` of the upper one and `_mm_set_epi64x`, which made MSVC 14.29 stop
  with C1001 on `sub_003711A0`; it is `_mm_loadh_pd` now, which reads only the upper 8 bytes
  and so never waits for the store of the lower half.)
- **Re-entry after an indirect call** (`indirect_call_resume_only`). A `jalr` whose target is
  not known made every instruction of its function a place to enter
  (`indirectFallbackEntryPoints`): 9,670 functions, 1.15 million labels and function-table
  entries, each one a join in the compiler's view of the function. A `jalr` now adds only the
  place after it (+8), as `jal` does: its target is a function's start, looked up at run time
  (one that is not would show as `[guest-branch:missing-target]`). A `jr` without a jump table
  keeps the old rule. `register_functions.cpp` went from 54 to 12 MB.
- **The pc stored only where it is read** (`minimal_pc_stores`, on since 2026-11-01). Every
  instruction stored its address in `ctx->pc`, every delay slot also `in_delay_slot` and
  `branch_pc`, while the runtime reads them only at calls, returns, checkpoints and syscalls
  (which store their own targets) and when it raises an exception. With the option, they are
  left out before an instruction that does not call the runtime (`translationObservesPc`) and
  after a branch whose fall-through stays in the function. The game thread ~4% faster in levels
  (test PC, Amazon at 120 held: 12.4 -> 11.9 ms a frame).
  (It was off from 2026-10-04: on five hot functions `check_frames`'s `minicon` fight went
  another way in 3 of 8 runs. Those runs had VIF1 on its own thread, where a faster EE changes
  where the threads meet; FPS round 3 checked it with VIF1 on the EE thread - the scripted cold
  boot into Amazon and a fight route, every frame the same as without it, 8 + 10 runs on both
  workers and here. `PS2X_CODEGEN_FULL_STORES=1` for `build.ps1` turns it off for an A/B.)
- **Direct calls** (`direct_calls`, on). A `jal` to a known function becomes
  `runtime->dispatchGuestCall(..., PS2_CALL_TARGET(target))`: the generated code reads the
  target's entry of the function table itself, one load from a fixed place, instead of the
  dispatcher's lookup. The entry is what the lookup returns (a replaced function's replacement
  too), so the same function is entered, after the same checkpoint (`PS2Runtime::enterCall` /
  `afterCall` are the rest of `dispatchGuestBranch`, shared). Measured about even with the lookup
  (round 2's per-thread table already hit 99%): builds with and without it 82.6 / 82.7 fps on the
  test PC. `PS2X_DIRECT_CALLS=0` at run time takes the lookup through `dispatchGuestCall` - one
  call more than a build without the option, so it measures slower than either (an A/B of the
  path, not of the gain). `direct_call_sites` (off) makes the call itself at the `jal`'s site
  (`Ps2CallKnown`: `callBegin`, a direct call, `callEnd`) - measured ~2% slower: two out-of-line
  calls a call cost more than the dispatcher's one indirect call.
- **Register write-back** (`register_write_back`, off). A write goes to the local only and the
  registers written since are stored back before a call, a jump to another function, a syscall
  or a return (inside a loop checkpoint's `if`); a label's set comes from the gotos to it. Off:
  measured slower (~3% a frame; ~29% with every written register at a label) - MSVC already drops
  most of the write-through stores, and the deferred ones keep the locals live across the
  function, so it spills them.

The scratchpad's loads and stores (`READn`/`WRITEn`, `ps2_runtime_macros.h`) go straight to its
memory after the main-RAM test, before the special-address tests and the call; `PS2Runtime::
Load*` (the constant-address ones the translator emits) the same first. `PS2X_INLINE_SCRATCHPAD=0`:
through the runtime's general path as before (with the flag tests in front: an A/B of the path).
~0.7% of the game thread (test PC 82.6 -> 83.2 fps).

**FPS round 4 (2026-11-02, PS2Recomp patch 0070).** What the game thread spends on calls and on the
scratchpad, measured with `PS2X_HOT_STATS=1` (a measuring aid: every 20 s of wall time the guest calls by
target, the loop checkpoints and the loads/stores that reach `PS2Runtime::Load*`/`Store*`, to stderr). In
Amazon: ~7.5 million calls a second, ~5 million loop checkpoints and ~5.9 million `Load*`/`Store*` calls -
every one of them to the scratchpad at a constant address (the analysis marks those MMIO, so the translator
emitted a runtime call for each).
- **The scratchpad at a constant address** (`inline_scratchpad_constants`, on). `PS2_SPR_LOADn(addr)` /
  `PS2_SPR_STOREn(addr, v)`: the scratchpad's memory read or written in place, as `Load*`/`Store*` did
  first (a 32-bit store still drains the completed DMA handlers after it). `PS2X_SCRATCHPAD_CONST=0`: the
  calls. The game thread ~1.5-2.7% faster.
- **The DMA-handler drain tested in place** (`PS2Runtime::drainCompletedDmacHandlersFast`). Every 32-bit
  store to the scratchpad called `drainCompletedDmacHandlers`, which returns at once when nothing is queued;
  the same test (its flag, `PS2X_EE_FASTPATHS` bit 2) is now made inline. `PS2X_DRAIN_INLINE=0`: the call
  every time. ~1.4-2.3%.
- **The generated code with `/Ob2`** (`transformers/CMakeLists.txt`, as the runtime libraries already were).
  With RelWithDebInfo's `/Ob1` the standard library's small helpers were not inlined: every `std::atomic`
  load in the generated code's inline paths - the scratchpad's host pointer behind each inline scratchpad
  access, the checkpoint's flags - was a call to `std::_Atomic_address_as`. `-DTF_GENERATED_OB1=ON`: `/Ob1`.
- **A leaner `dispatchGuestCall`**. The usual end of a call (back at the fallthrough, not stopped, no caller
  of this level to resume) is tested first and taken in a few instructions; every other end goes through
  `callEnd` (now out of line), so the dispatcher's own frame is small. The dispatch history (the last 64
  call targets, printed when a call finds no function) is recorded on this path only with
  `PS2X_DISPATCH_HISTORY=1`. `PS2X_DISPATCH_LEAN=0`: as before. ~1.4%.
- Tried and left off (options of the recompiler, each with its run-time switch): the call made at the
  `jal`'s site with its begin and end inline (`direct_call_sites` + `PS2Runtime::leafBegin/leafEnd`), a
  `jalr` made at its site (`indirect_call_sites`, `Ps2CallIndirect`), a loop's checkpoint inline
  (`inline_loop_checkpoints`, `eeLoopCheckpoint`) and leaf functions inlined into their callers
  (`inline_leaves = N`, `Ps2CallLeaf_<name>`; `sync_generated.py` declares the leaf's function for the
  "still not replaced" test). Each put ~50-70 instructions (or the leaf's body) at every site; the
  generated code grew by 20-50% and the game thread was even or 1-4% slower. The dispatcher's work per
  call is mostly the checkpoint and the call level, which every one of these keeps; doing it at the site
  saves an indirect call and costs code size.
- Tried and left off (2026-11-05): **whole-program optimisation** for the played build, `/GL` on the game and the
  runtime and `/LTCG` at the link (`-DTF_LTCG=ON`; `/fp:precise`, no `/arch` change - `EnableFastReleaseMode`'s own
  `/GL` + `/LTCG` are Release-only, so RelWithDebInfo never had them). The game thread was 3% SLOWER: the test PC
  in turbo (the game as fast as it goes, logic 240, native only, scripted Amazon and Deep Amazon, two rounds each,
  the builds in turn) 200.0 -> 194.2 frames a second in Amazon, 189.3 -> 184.8 in Deep Amazon (the rounds within
  1% of each other); at the uncapped frame-rate cap both settle at the same ~5 ms a frame. The link took 230 s and
  6.0 GB (1 GB and 5 s without), for every change of any file: whole-program code generation inlines across the
  generated code's 22,000 functions, and the bigger code costs more than the calls it saves (as the inlining
  options above).
Every change runs the same guest code in the same order with the same EE clock: the scripted cold boot
into Amazon and the fight route give the same game state on every game tick on both workers and here,
and the native renderer's shots are pixel-identical.

**FPS round 5 (2026-11-02, PS2Recomp patch 0071).** The generated code itself. The disassembly of a 30-instruction
VU0 matrix function (`sub_001B0D50`) was 9.3 KB of machine code: each of its ten 128-bit loads and stores carried,
inline, two unrolled 16-byte "wrap around the end of RAM" copies, the special-address chain and the scratchpad
test - code that never runs in a level, but that every one of the ~1 million guest loads and stores in the game
had inline. The generated code was 223 MB for 21,590 functions.
- **Only main RAM inline** (`ps2_runtime_macros.h`, the default; `-DTF_MEMORY_COLD_INLINE=ON` = before). `READn`/
  `WRITEn` read or write in place when the access lies wholly inside the 32 MB (or its uncached mirror at
  0x20000000, where the game writes its DMA lists: the address with bit 29 cleared; `-DTF_MEMORY_MIRROR_INLINE=
  OFF` = not); everything else calls one out-of-line function per size (`Ps2ReadSlowN`/`Ps2WriteSlowN`) holding
  the former inline logic unchanged, the scratchpad included (`-DTF_MEMORY_SCRATCHPAD_OUTLINE=OFF`: its test
  inline after RAM's, as round 3 had it). The generated code fell to 63 MB (the executable 247 -> 83 MB, the
  game's compile ~23 -> ~5 min), and the game thread is about a third faster: test PC Amazon at 240 100.1 ->
  142.8 fps with the scratchpad test still inline, 146.3 without it; the other PC 128.2 -> 169.7 -> ~175. The
  uncached mirror inline: the other PC +0.8% (the out-of-line 32-bit stores' share of the thread 2.6 -> 0.8%).
  `PS2X_HOT_STATS=1` also counts the out-of-line accesses (per 64 KB of address, size + 64).
- **VU0's registers in SIMD locals** (`vu0_register_cache`, register_cache.cpp): `ctx->vu0_vf[n]`/`ctx->vu0_acc`
  become `rv_<n>`/`rvacc`, written through to the context after the line that writes them and loaded again after
  a call (a VU0 micro program changes them), as the FPU's are. Before, every VU0 macro instruction loaded its
  operands from the context and stored its result there, and the next instruction loaded them again (a guest
  store may alias the context for the compiler). Only 32 functions use VU0 macro mode, but some are among the
  hottest (the matrix and vector routines at 0x1B0xxx, `sub_00260C80`): test PC +3.5%, the other PC +0.5%.
- **Constant blends and repeated clamps folded** (`vu0_fold_blends`): a VU0 instruction's destination-field blend
  with a constant mask (`_mm_blendv_ps`, which MSVC keeps a variable blend) as `_mm_blend_ps` with an immediate,
  or no blend when every field is written; `PS2_VADD/VSUB(acc, mul_res)` of a `PS2_VMUL` result without
  clamping it twice (`PS2_VADD_C`). About even (other PC +0.4%, test PC -1.4%, both within build-layout noise).
- **FPU clamps left out where the value is clamped already** (`fpu_clamp_elision`, `elideFpuClamps`): an
  ADD/SUB/MUL operand that is an `rf_<n>` last written by ADD/SUB/MUL/DIV/SQRT/RSQRT (or a MOV/NEG/ABS/MAX/MIN of
  such), the ACC, or a nested ADD/SUB/MUL keeps its value without `ps2_vclamp` (`FPU_ADD_S_KN` etc.). Followed
  line by line: a label or a reload after a call forgets, a block's end keeps what both ways agree on.
  Measured against the final build without it (3 runs each): test PC +0.5%, the other PC +1.3%.
- **No pc store before a forward jump inside a function** (`minimal_goto_pc_stores`): nothing at the label reads
  `ctx->pc` before storing it - except a branch whose delay slot is also a jump target (its `if (ctx->pc ==
  <delay slot>)`), whose labels keep it (`pcReadLabels`). Backward jumps keep it for their loop checkpoint. In
  `sub_00429510` 137 -> 75 pc stores. The other PC +0.9%, test PC within noise.
- **A call's table entry without the bounds test** (`direct_calls_unchecked`, `PS2_CALL_TARGET_IN`): a `jal`'s known
  target is a function of the table, which spans every function (this game: 95,638 sites, all inside the table on
  a filled slot - checked on the generated code). The other PC +0.9%, test PC +0.5%.
All of these give the same bits: the clamp is idempotent, a constant blend selects the same lanes, the locals
and stores leave the context as before at every place anything else can look at it, and an access takes the
same path to the same bytes. The scripted cold boot into Amazon (8,047 game ticks), the fight route (19,244)
and Deep Amazon (19,358 - the VU0-heavy level), all in turbo with VIF1 on the EE thread, give the same game
state on every game tick as before on the test PC, the other PC and here, and the native renderer's shots
are pixel-identical.
Round 5 altogether (240 held, game ticks 8600-13000, B0 = the round-4 build): test PC Amazon 97.5 -> 148.1 fps
(game thread 10.18 -> 6.66 ms a frame), Deep Amazon 79.7 -> 111.7 (12.45 -> 8.84 ms); the other PC Amazon 126.9 ->
178.1 (7.86 -> 5.60 ms), Deep Amazon 99.1 -> 139.8 (10.05 -> 7.08 ms).

**FPS round 6 (2026-11-08, TODO 94, PS2Recomp patch 0083).** What round 5 moved out of line and Deep Amazon still
takes: 15-20 million scratchpad loads and stores a second through `Ps2ReadSlowN`/`Ps2WriteSlowN` (the scratchpad is
where the game's VU0 matrix and vector routines keep their work), the slow path ~5-6% of the busy game thread.
- **The scratchpad inline again, only in the functions that use it** (`transformers/config/scratchpad_hot.txt`,
  `transformers/src/scratchpad_hot.h`, `scratchpad_hot_begin.inc`/`_end.inc`): `tools/sync_generated.py` wraps each
  listed function in its batch file with the two includes, which redefine `READn`/`PS2_SPR_TRY_WRITE` for that
  function only: main RAM inline, then `Ps2ScratchpadInlineRead`/`Write`, then the out-of-line call (the round-3
  order). Everything else keeps round 5's small code. The list = the 54 functions doing 92% of the out-of-line
  scratchpad accesses in Deep Amazon (`sub_001B0D50` alone 28%), measured with a temporary per-function counter on
  `PS2X_HOT_STATS=1` (the caller's start address noted with each slow access; not kept in the runtime - redo it the
  same way if the list goes stale). The sync prints how many listed functions it found ("54 of 54").
- **`writeGuestBytes` copies a contiguous range in one `memcpy`** (patch 0083, `Kernel/Stubs/Helpers/Support.h`): it
  looked up every byte through `getMemPtr`; now, when the first and the last byte map `len - 1` apart, one copy. Its
  main caller is the `sceVu0UnitMatrix` stub (`getMemPtr` was 0.85% of the thread).
Both give the same bits (the same path to the same bytes): the Deep Amazon play recording (8,316 frames) replays
identically on both builds, and the native shots there are pixel-identical. On a GTX 1080 test PC, Deep Amazon at 240 uncapped,
turbo, 3 runs each in turn: the scratchpad inline 189.0 -> 193.3 fps (game thread 5.29 -> 5.17 ms a frame, +2.3%);
the `memcpy` 192.9 -> 193.6 (+0.4%, within the noise).
- **`mlxFrustum::Test` (0x1B2DD0) in one native call** (`transformers/src/frustum_native.cpp`, `TF_FRUSTUM_NATIVE=0`:
  the recompiled one). The view-frustum test of a box (the forest's trees, the scenery's cells), which made up to seven
  calls a box through the dispatcher (its sphere test 0x1B2760, then a plane test 0x1B2630 per plane still set): the
  three functions' generated code joined, step for step - the same loads and stores (the stack frame's saves
  included), the same FPU helpers in the same order, every register written as the register cache writes it - with
  the dispatcher's own checkpoints (each box test inside `leafBegin`/`leafEnd`, the loop's back edge through
  `eeCheckpointDue`), so a checkpoint that comes due finds the same context and the call returns as the recompiled
  code does; a call resumed inside the function goes to the recompiled one. Named after the game's own functions; left
  out when another replacement takes any of the three. The gt3 recording (8,435 frames) replays identically and the Deep
  Amazon shots are pixel-identical. Measured after patch 0082 (whose leaner call took most of what this saves), the
  same build on and off, test PC Deep Amazon: the forest's leaves 0.276 -> 0.258 ms a frame and its drawing
  0.467 -> 0.450 (the forest log, two runs each), about 0.035 ms of a 5.05 ms frame (~0.7%; the frame rate itself
  within the noise, 197.8 against 197.6 over three runs each). What is left of the test is its arithmetic.

`fast_codegen_only = ["0x..."]` limits these to some functions (by start address): with
`build.ps1 -NoPatches` only their batches are compiled again, so a change can be tried on a few
before the full rebuild.

**N11f (2026-11-08, PS2Recomp patch 0082): the dispatcher's own cost.** Sampled in Deep Amazon (the test PC, turbo,
logic 240, `TF_PROFILE_FILES`) the dispatcher was 16.5% of the EE thread: `dispatchGuestCall` 9.0, `dispatchGuestBranch`
3.6, `eeCheckpointDue` (the loops') 3.5 - instructions every call pays, about 80 for a plain call, a quarter of them only
saving and restoring the nine registers the whole dispatcher's frame needed; the checkpoint itself (charge, scale
remainder, COP0 Count, the stop compare and two flag loads) about 25. The scheduler's re-entries (~42,000 a second) are
0.3%: not worth changing. Changed:
- **The checkpoint's flags folded into its compare.** Raising the checkpoint-pending flag or the stop also sets the
  folded stop (`m_stopAtCycle`, now atomic) to 0 (`EeScheduler::raiseCheckpoint`: the flag, then the 0, both seq_cst -
  every place that raised the flag, from any thread), so the lean checkpoint is one compare; its out-of-line part tests
  the flags as before, in the same order, and puts the stop back once they are down (`rearmStopAt`). `updateStopAt`
  stores the stop, a full fence, then reads the flags and keeps it at 0 if one is up: a flag raised after that read
  writes its 0 after the store, so no stop or checkpoint is lost to a stop worked out again. `PS2X_LEAN_CHECKPOINT=0`'s
  way out of line (`checkpointDueOld`).
- **Small frames.** `dispatchGuestCall` is the plain call alone (everything else in `dispatchGuestCallFull`);
  `eeCheckpointDue` a leaf without a frame (`PS2X_HOT_STATS`'s count out of line).
Measured: Deep Amazon from a state, turbo, two interleaved runs each: the game thread 7.212 -> 7.070 ms a frame (-2.0%;
the test PC's run-to-run spread is about as large); the dispatcher's samples 16.5% -> 14.4% of the EE thread. The game
plays the same: a play recording from the boot (menus, the HQ, the warp into Deep Amazon, a fight and a walk, a pause
and its end; 21,721 frames) replays identically. Requests from other host threads (`PS2X_RAISE_LOG`) are taken as
promptly as before: F1 > Debug's save and load 38 EE cycles at the median (0.8 us), the window's close and the settings
panel's restart within 12 cycles, the same as the build before; real-time pacing at 60 the same (60.06 VBlanks a
second). Tried and left out: a `jalr` through the function table into the plain call (`indirect_calls_via_table`, in
place of `dispatchGuestBranch` and its call-target cache): inline at the 16,463 sites 3.1% slower (the code +1.7 MB),
through one runtime function even (+0.5%, noise). The checkpoint's charge (the cycle scale's remainder kept beside the
clock, COP0 Count stored every time) is now the largest part left (~2.7% of the thread).

## Host input

`PSPadBackend::readState` (`ps2_pad.cpp`) answers the pad HLE with one DualShock 2 state
built from the first gamepad, the keyboard and the mouse together (the key map is in the
README and at the top of the file). Upstream read either the gamepad or the keyboard, mapped
WASD to the D-pad and had no stick or mouse input, so a level could not be played on a
keyboard.

- The game thread reads keys when the game polls the pad; the window loop reads the host's
  input every 4 ms (it was once per drawn frame), so they are at most that old. What only
  the window loop can see is fed in from there (`ps2_host_input::updateFromWindow`): whether
  the mouse belongs to the game (after a click into the window; Esc or losing focus releases
  it) and how fast it moves.
- A stick only says how far it is pushed. By default mouse motion becomes a right-stick
  deflection in proportion to its speed, averaged over one game frame (1/30 s), the
  smallest motion mapped just past the game's dead zone. A game can take the mouse travel
  instead (`ps2_host_input::takeMouseTravel`) and set the stick itself (`setMouseStick`):
  Transformers does, in `overrides.cpp`, through the exact inverse of its measured stick
  response (dead zone of 25 of 127 steps, turning speed with the cube of the deflection,
  `kRadiansPerPixel`), so the view turns as far as the mouse travels, up to the game's own
  turning speed. The game tilts the view like a flight stick (the stick pulled down looks
  up), so the mouse's vertical travel goes in reversed: moving the mouse up looks up
  (`PS2X_MOUSE_INVERT_Y=1` for the flight-stick way). Checked with simulated mouse motion
  (`PS2X_MOUSE_TEST`); the first hands-on play (2026-10-03) asked for the vertical reversal.
- Esc is no longer raylib's exit key: the window closes through its close button.
- Scripted input (`TF_INPUT`) goes through the pad override of the HLE and replaces all of
  this while a press is active.
- A window without the focus takes no live input: no keys, mouse or gamepad (a gamepad's state
  is the whole desktop's - the user's moves while playing once reached a test run opened beside
  the game). The pad reads as released with the sticks centred; `PS2X_INPUT_UNFOCUSED=1` takes
  the input all the same. Scripted input and `PS2X_MOUSE_TEST` are not live input and still work.

## The settings panel

F5 opens a settings window over the picture (`ps2_settings.h`; the settings and the file in
`ps2_settings.cpp`, the panel in `ps2_settings_panel.cpp` - raylib and `windows.h` do not go in
one file). A setting is one of the environment variables the runtime and the game already read,
with a list of values: the code that reads them stays as it was.

- **The file.** `settings.ini` in the working directory (`PS2X_SETTINGS_FILE` another, `=0`
  none), a `VARIABLE=value` a line. `ps2_settings::loadFile()`, first thing in `main`, puts its
  variables into the environment where the environment does not set them: one set at the start
  wins, so `run.ps1` and the test tools keep control (`run.ps1` sets `PS2X_SETTINGS_FILE=0`
  unless `-Settings`). The panel writes the file at each change; a setting back at its default
  leaves it, lines the panel does not know and comments stay.
- **Live or at start.** A setting with an `apply` function changes while the game runs: the panel
  calls it on the window thread and sets the variable. What belongs to the game's thread goes
  through atomics it takes at its next frame: `takeSettings` in `overrides.cpp` takes the frame
  rate (120 or 60 pictures a second: the picture lock, converting a level's rate as F4 converts it
  and redoing the in-between pictures; or the screen's refresh - the moments, `g_wantMoments` -
  since 2026-10-08, night); `applyDrawDistance` rebuilds the draw distance's table. A frame rate of
  its own and the adaptive rate are the environment variables' (`TF_FRAME_RATE`,
  `TF_FPS_LOCK=0`). `PS2X_SETTINGS_CHOOSE=<s>:<KEY>=<value>` makes a choice as the panel does
  (testing aid, `updatePanel`). The rest is read
  once at start (the GPU's options when it starts: the resolution sizes every target), and an
  `apply` can say no to a value (to or from the console's 30, whose hooks are installed at
  start; the draw distance of a game started at 1). A choice that waits for a start shows amber.
- **Restart now.** The panel asks the EE scheduler for a state (`requestSaveState`, served at
  the top of the dispatcher loop; `savedStateCount` and `failedStateSaveCount` say when it is
  done), starts the same executable with the same command line and standard output and an
  environment in which the settings come from the file again (one set at the original start
  keeps the panel's choice), `PS2X_LOAD_STATE` naming the state, and stops. The new process loads
  it before its first instruction and deletes it. A state cannot be taken while a host callback
  is pending (a movie, a load): the panel then says so. The memory card is left alone
  (`PS2X_STATE_RESTORE_CARD` as it was).
- **Input.** While the panel is open the game gets no keys or gamepad
  (`ps2_host_input::setGameInputBlocked`, as for a window without the focus) and the mouse is not
  captured; once it closes, the game waits for the panel's keys and buttons to be let go. The
  keyboard (arrows or WASD, Q and E or Tab for the tabs, Enter, Esc), the mouse (pointing, a
  value's halves, the wheel) and the first gamepad (the D-pad or the left stick, repeating; A; B
  or Start; LB and RB).
- F3 (the FPS counter), F4 (the picture lock) and F11 (full screen) go into the file too
  (`ps2_settings::noteChanged`). Quick save moved to F6.
- The runtime's settings are added by `addRuntimeSettings()` and the window loop, Transformers'
  own by `addSettings()` in `overrides.cpp`. The text is Segoe UI from Windows' fonts at the
  screen's size (raylib's own font where it is missing).

## Save states

`ps2_savestate.cpp` writes the whole machine to one file and loads it back: EE RAM, scratchpad,
I/O registers, EE timers and VU memory; the EE scheduler (thread contexts, semaphores, event
flags, alarms, interrupt handlers, pending events and interrupt calls); GS registers, local
memory and the CLUT buffer; VU1 registers; the IOP emulator (RAM, threads, modules, export
tables, RPC servers and the threads that serve them, timers, the files its modules hold open,
the SPU2); what the HLE libraries keep on the host (SIF/RPC, pad, memory card, CD, MPEG, DMA,
DECI2, open `sceOpen` files); and the memory card's files. Format 2 (with the SPU2) loads no
state of format 1. A save waits for the VIF1 thread's queue, and the EE's view of the VIF1
registers is what gets saved.

This works because of how the runtime runs guest code: at the top of the EE dispatcher loop
every guest thread is a saved register context (functions re-enter at any instruction
through their `switch (ctx->pc)`), the IOP interpreter has returned, RPCs are synchronous, and
a VU1 microprogram has drained its pipelines. The only things a file cannot hold are host
closures - a wait or guest call that completes through a C++ lambda - so a save is postponed
(up to 600 VBlanks) while one is pending (`EeScheduler::saveStateBlocker`). Caches are not
saved: decoded textures, VU1 decode/JIT images and the disc index are rebuilt on demand.

Making and loading states (`run.ps1 -SaveState` / `-LoadState`, F6 / F9) is described in
[DEBUGGING.md](DEBUGGING.md#save-states). A game whose own host-side state has to agree with
the loaded memory gets a call right after the load, before any guest code runs on
(`EeScheduler::setStateLoadedHook`; Transformers sets its level's frame rate there).

The VBlank tick continues from the state's, so `TF_INPUT`, `PS2X_SHOT_VSYNCS`,
`TF_QUIT_VSYNC` and `PS2X_TURBO_VSYNCS` keep their meaning; `TF_INPUT` presses for ticks before
the load are dropped. A state loads only into a build of the same recompiled game with the
same state layout (`kVersion` and a fingerprint of the raw structures), and it contains the
game's memory: never commit one (`states/` is ignored). A state stores the memory card's
files; loading restores them into the card folder only when asked to
(`PS2X_STATE_RESTORE_CARD=1`, which `run.ps1 -LoadState` sets together with a scratch card).
One made while a movie plays resumes without the MPEG decoder's reference pictures, and the
host's `std::rand()` state is not saved (the game's `rand` is an HLE stub).

Checked against a normal boot: a state made in the Drop Zones room (tick 5700), loaded at start
or in the middle of another run, gives pixel-identical frames through the level load and
in-game (ticks 5702–6800).

## Speed history

Frames per second in Amazon Basin, standing at the start of the level (Ryzen 7 7800X3D, four
rasterizer threads). The game's own cap is 30 frames at 60 VBlanks per second.

| Step | fps |
|---|---|
| Level first drawn correctly | 9.8 |
| VU1 flags only where a reader can see them | 10.3 |
| GS waits: surface guard, feedback primitives on the GS thread, read/write split of in-flight pages, per-page tile invalidation | 11 |
| Texture tiles decoded block by block | 11.6 |
| AVX2 span loops | 17–18 |
| Presentation without a 4 MiB snapshot of GS memory, block-wise read, frame reuse | 18.8 |
| Texel ranges across the repeat boundary (no more whole-texture decodes) | 19–21 |
| Idle skip in the VBlank wait | 24 |
| Buffers of GIF packets and DMA chains reused instead of allocated | 24.9 |
| AVX2 for 16-bit frames | 29.5–30 |

The heaviest stretch found later is the walk from the dropship to the first Mini-Con, with
enemies firing (about 124,000 triangles and 29,000 sprites a frame). Frames per second there
in turbo mode (`tools\bench.ps1`, 1000 ticks from state `a11`; four rasterizer threads unless
said otherwise):

| Step (2026-10-02 night to 10-03) | fps |
|---|---|
| Read-back answered from a capture, pacing keeps one VBlank of lag | 18.1 |
| Draw state sent once per run of primitives, bare vertices for the rest | 19.7 |
| VIF1 `UNPACK` without per-element dispatch | 22.2 |
| VIF1, VU1 and the GS front end on their own thread | 27.6 |
| Read-backs ahead of the VIF1 queue; texture decodes, presents and captures as barrier jobs | 26–27 (29.3 with five rasterizer threads) |
| Effect passes that sample what they draw as ordinary parallel jobs | 28.4 (30.3 with five) |
| Five rasterizer threads by default on 8 hardware threads; FMAC flags four lanes at a time | 31.1–31.6 |
| The EE woken when its read-back is ready, not after a sleep | 31.7–31.8 |
| `CLIP`, `DIV` and FMAC flags compiled into the VU1 JIT's blocks | 32.0 |
| Uploads written row by row with the addressing inlined | 34.3–34.5 |

In real time with sound the stretch now holds 60 VBlanks per second throughout. What a frame
costs now, and where the next gains are, is in [STATUS.md](STATUS.md).

At 60 frames a second (the same stretch, 600 ticks from `a11` with 60 forced on the state,
1x, five rasterizer threads; 2026-10-04):

| Step | fps |
|---|---|
| True 60 as made the day before (both fields drawn) | 49.5 |
| One field drawn and shown; views of a state made at 30 as at 60 | 50.5 |
| VU1 JIT: operands normalized with two integer minimums | 54.4 |
| VU1 JIT: blocks that go round on themselves in place | 55.6 |
| VU1 JIT: VF registers and ACC in host registers | 57.9 |
| VU1 JIT: events committed only before a pair that sees them | 58.6 |
| VU1 JIT: register stalls worked out at compile time | 60.2 |
| EE timers in steps; main RAM tested first, scratchpad reads inline | 61.3 |
| IOP interpreter: one fetch, inline RAM reads; scheduler events without allocation | 61.6 |
| VU1: stores during an `XGKICK` logged only inside the packet | 63–66 |
| VU1: events committed in time order, a lone Q directly | 65 |

At 144 frames a second, the start of Amazon Basin (1500 game-time ticks from `amazon`, forced
to 144, flat out, 1x; 2026-10-04). There a frame keeps the EE thread busy for nearly all of
its time: the game code and, inside it, the IOP's code for the game's requests (about 11%),
the cycle accounting at every checkpoint (8%), the display list's copy for VIF1 (3%) and the
wait for the read-back (3%) (`TF_PROFILE`):

| Step | fps |
|---|---|
| As above | 113.6 |
| The generated code's helpers forced inline (`PS2_FORCEINLINE`): register halves, RAM loads and stores, the address test, float arithmetic; loads as functions with the rare path out of line instead of immediately invoked lambdas; no stack cookies (`/GS-`). The executable 345 MB to 244 MB | 119 |
| A checkpoint's chain of calls inline (`EeScheduler::checkpointDue` in the header), the IOP's cycles held until its next batch (`ps2IopFlushHeldEeCycles` before every call into it), one look at the function table a call | 120 (within the noise) |
| The guest's registers in locals, re-entry after `jalr` only where it returns ([The generated code](#the-generated-code)): the EE's work 8.3 to 6.3 ms a frame, the thread now waiting a quarter of the time. The executable 244 to 181 MB | 119 (116.9 against 119.1 run in turn) |
| The GS front end leaves out primitives wholly outside their scissor; the GS thread spins 2,000 pauses before it sleeps, not 20,000 | 122.0 (119.1 against 122.0) |
| VU1 JIT, second round ([VU1](#vu1)): operands known to be finite left unclamped, `SQRT`/`RSQRT`/simpler EFU inline, events committed in the block, blocks going on into each other. VU1 2.1 to 1.7 ns a pair | 141.7 (121.7 against 141.7) |

That last step left the GS thread as the limit there: busy 99% of the time, in its own loop
(30%), the GPU's batches of primitives (`appendPrimitives`, 20%) and uploads (`UploadImage`,
18%); the VIF1/VU1 thread 88%, the EE 74%. On the walk from the dropship to the first
Mini-Con at 144 (600 ticks from `a11`), where the EE did more of the work, the same step gave
70.9 to 76.8 fps; at 60 there 64.4 to 65.0.

The GS thread was not the limit, though (a quarter of its time was spinning): every frame the
EE reads back the occlusion buffer its display list draws and waits for VIF1 to get there, so
VIF1/VU1 set the pace. After the VU1 steps a frame there is the EE's own work from the
display list to the read-back request (5.4 ms), the wait for VIF1 (1.0 ms; 1.8 before), the
capture (0.5) and the answer to the next display list (0.65) (`PS2X_MTVU_STATS=1`). On the
walk from the dropship the two steps gave 76.8 to 78.6 and 78.2 to 88.5 fps.

## Building

- The generated code is compiled with a precompiled header. A change to any header in its
  chain recompiles all ~22,000 generated functions (12 to 14 minutes at first; about 45 since
  2026-10-04): `ps2_runtime.h`,
  `ps2_runtime_macros.h`, `ps2_memory.h`, `ps2_address.h`, `ps2_vu1.h`, `ps2_vfs.h`,
  `ps2_audio.h`, `ps2_gif_arbiter.h`, `gs_frontend.h`, `gs_backend.h`, `gs_types.h`,
  `ps2x/iop/iop_types.h`.
- Not in the chain (a rebuild takes under a minute): `ee_scheduler.h`, `gs_cpu_backend.h`,
  `gs_threaded_backend.h`, `gs_draw_state.h`, `ps2_vif1_thread.h`, `ps2_gs_memory.h`,
  `ps2_savestate.h`, `ps2_vu1_fast.h`, `ps2_audio_stream.h`, the stub headers (`LibC.h`,
  `MPEG.h`, `Helpers/Support.h`), and everything under `src/`.
- To add runtime state without a full rebuild, keep it in a `.cpp` file; private members of
  `PS2Runtime`, `GS`, `VU1Interpreter`, `PS2Vfs` and `GifArbiter` are reachable through
  `struct PS2StateAccess` (a friend of all of them, defined in `ps2_savestate.cpp`).
- `gs_raster_avx2.cpp` is the only file compiled for AVX2 (`ps2xRuntime/CMakeLists.txt`
  sets `/arch:AVX2` or `-mavx2` for it and defines `PS2X_GS_AVX2`). `gs_raster.h` keeps
  everything in an unnamed namespace so that the two translation units, compiled for
  different instruction sets, never share an out-of-line copy of a function.
- Git must never rewrite the submodule's files while work is in progress there (`git stash`,
  `git checkout`): the new timestamps force the full rebuild.

### Modern host stick response (0084)

Dead zones and menus (0163, TODO 274): `Config::leftDeadzone` / `rightDeadzone` (settings
`PS2X_STICK_LEFT_DEADZONE` / `PS2X_STICK_RIGHT_DEADZONE`, default 0.05 = the response's own) replace the
fixed 5% of `radial` and `vehicleAxis`; the travel runs from the dead zone's edge to 95% of the stick.
`map(..., play)`: the lift is not applied while a game menu's list is shown (`setMenuShown`, called each
frame by `menu_mouse.cpp`: the front end, the HQ, a pause, the pickup menus; after ravioli-gh's Linux
port). There the sticks go on as the pad gives them (`cut`: zero inside a dead zone
set above the default). Before, the left lift (0.625 x 1.33) turned a stick resting 6% off centre into a
stick held 83% over, and the menus scrolled on their own.

`runtime/ps2_stick_response.h` is the pure mapping used by `PSPadBackend::readState` and the
standalone host test. `PS2X_STICK_VEHICLE_MODE=original` (default) preserves the incoming
GLFW axes and reapplies Sony's legacy 0.08 per-axis drift filter at the car endpoint, on
both sticks. `tuned` selects the previous separate left-stick curve and modern right-stick
response. Both modes retain the existing transform blend; robot mapping and detection are unchanged.
Sony bytes are decoded without their old 8% per-axis drift filter only
when response is enabled. The GLFW and Sony paths then share radial robot/camera mapping and
per-axis vehicle mapping, before keyboard and mouse overrides and before `axisByte`. Response
off skips all shaping and uses the exact previous Sony decoding and byte conversion.

The game module (`movement.cpp`) wraps the controller getter 0x34B040, axis getter 0x197DC0
and pad read 0x13A898 with `lookupFunction` / `registerFunction`, preserving their original
functions, including resumed calls. Controller vtable 0x6B8000, binding id 0, actor/context
backlinks and bounded guest spans validate the player. Axis vtable 0x6B14A0 and masks
0x20000 / 0x40000 validate live deadzones. Actor state index +0xF8 selects the state pointer
at +0x80 + 4*index, with its vtable at +0x18: car 0x6BFA20, to car 0x6BFE80, to robot
0x6BFED0. Other states, disabled controllers, stale observations and missing hooks select
robot. These are the archived carstick2 addresses, not copied decompilation source.

Only the EE thread reads guest state. A mutex publishes host-only weight/deadzone to the pad
backend; the snapshot expires after 500 ms. Transform weights advance from elapsed host time
at pad reads (0.2 seconds for a full ramp), avoiding a six-frame ramp changing duration with
`TF_FRAME_RATE`. Stable states snap to their exact endpoint. Camera script state is unused;
jet bikes retain robot response. The existing PickDesiredSpeed replacement and its 0.85
threshold are untouched. Settings are registered by `controller_settings.cpp` and read live
through the existing settings API. Full option table: DEBUGGING.md, Controller stick response.

`TF_INPUT` uses the pad override before the backend. `playrec.cpp::padRead` records after the
stub and replaces all 32 bytes after it during replay; it installs after movement's wrapper.
No record format, stored pad bytes, guest state writes or replay mapping changes are involved.

### Last controller style for native prompts

Patch 0085 adds `ps2_host_input::PadStyle`, `lastPadStyle()` and `padStyleForName()`.
Native Sony reads select PlayStation. GLFW names first check explicit Xbox/XInput identities
(`xbox`, `xinput`, `x-box`, `360 controller`, `series x`), then handheld/Steam identities
(`steam deck`, `steam virtual`, `steam controller`, `legion`, `rog ally`, `msi claw`, `ayaneo`,
`one xplayer`, `onexplayer`). These identify Xbox-labelled controllers, Steam virtual pads or
handhelds normally presenting Xbox-compatible input, so they take priority over Sony-like suffixes.
The bare name `Wireless Controller` matches Sony exactly; longer names need a clear Sony identity
(`sony`, `playstation`, `dualsense`, `dualshock`, `ps4`, `ps5`). Unknown names select Xbox. The style is published atomically
only when that pad is active, or during the existing initial-device detection. Keyboard activity
keeps the most recently used pad style. Existing activity thresholds, simultaneous-input priority,
scripted input, pad bytes and trigger values are unchanged. No XInput or GS upload hook is added.

Native font replacements load an Xbox twin beside the existing keyboard twin. Each has a separate
GPU id (bits 61 and 62 respectively) and original-texture fallback. A draw checks keyboard first,
then the live controller style (`TF_PAD_PROMPTS`); PlayStation continues using `TF_BUTTON_SYMBOLS`.
The emulated texture pack's `keyboard/` handling is unchanged. Fonts are made from decoded local
RGBA atlases, so drawing never precedes palette availability.

The Xbox labels come in two looks, as the pads have them: the Xbox One / Series pad's (dark buttons, coloured
letters, View and Menu icons: `text_xbox` / `xbox`) and the Xbox 360 pad's (coloured buttons, pale letters,
Back and Start arrows: `text_xbox360` / `xbox360`, a third twin with GPU id bit 60). GLFW names both pads
"Xbox Controller", so the runtime (patch 0172, `ps2_host_input::xbox360Pads`) reads the USB ids in the Windows
device path of each XInput pad (raw input, `IG_` in the path): when every one is a Microsoft 360 pad
(045E:028E, 028F, 0291, 02A1, 0719) the 360 look is drawn, otherwise (One / Series, other makers, a mix, not
Windows) the One / Series look. Looked at when a gamepad appears and every 3 s while one is there. Fonts made
before the 360 look keep the One / Series look for every Xbox pad.
