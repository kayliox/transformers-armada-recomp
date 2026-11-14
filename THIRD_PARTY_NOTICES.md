# Third-party notices

This project's own code is licensed under the GNU General Public License v3.0 (see [LICENSE](LICENSE)).
It uses the components below. Each keeps its own licence.

## Built into the game

| Component | Author | Licence | How it is used |
|---|---|---|---|
| [PS2Recomp](https://github.com/ran-j/PS2Recomp) | ran-j | GPL-3.0 | The recompiler and runtime (git submodule, with this port's patches) |
| [raylib](https://github.com/raysan5/raylib) | Ramon Santamaria | zlib | Window, input and graphics base (fetched by PS2Recomp) |
| [Dear ImGui](https://github.com/ocornut/imgui) | Omar Cornut | MIT | The settings menu (fetched) |
| [rlImGui](https://github.com/raylib-extras/rlImGui) | Jeffery Myers | zlib | ImGui on raylib (fetched) |
| [xbyak](https://github.com/herumi/xbyak) | MITSUNARI Shigeo | BSD-3-Clause | Code generation at run time (fetched) |
| [Opus](https://opus-codec.org/) | Xiph.Org Foundation and others | BSD-3-Clause | The DualSense speaker's sound over Bluetooth (fetched by a PS2Recomp patch) |
| [fmt](https://github.com/fmtlib/fmt) | Victor Zverovich | MIT | Text formatting (fetched) |
| [toml11](https://github.com/ToruNiina/toml11) | Toru Niina | MIT | Config files (fetched) |
| [sse2neon](https://github.com/DLTcollab/sse2neon) | DLTcollab | MIT | SIMD helpers (fetched) |
| [xxHash](https://github.com/Cyan4973/xxHash) | Yann Collet | BSD-2-Clause | Fast hashing (in a PS2Recomp patch) |
| [bc7enc](https://github.com/richgel999/bc7enc) | Rich Geldreich | MIT or public domain | Texture compression (`transformers/native_render/ext/bc7enc`) |
| [SMAA](https://github.com/iryoku/smaa) | Jorge Jimenez and others | MIT-style (see its `LICENSE.txt`) | Anti-aliasing (`transformers/native_render/ext/smaa`) |
| [FidelityFX CAS](https://github.com/GPUOpen-Effects/FidelityFX-CAS) | Advanced Micro Devices, Inc. | MIT | The sharpening's contrast-adaptive weights, written after it (`aaSharpen` in `transformers/native_render/nr_d3d.cpp`) |
| [FidelityFX Super Resolution 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR) | Advanced Micro Devices, Inc. | MIT | The window's upscaler at render scales under 100% - EASU and RCAS, ported to GLSL (`fsr::kEasu`, `fsr::kRcas` in `transformers/native_render/nr_present.cpp`) |
| [FFmpeg](https://ffmpeg.org/) | The FFmpeg developers | LGPL-2.1 or later | Movie playback. On Windows: FFmpeg 7.1 cut down to the MPEG-2 decoder and swscale, static libraries in `tools/ffmpeg_static/prebuilt/` (built by `tools/ffmpeg_static/build.sh` from the release tarball pinned there; `BUILD.txt` and `COPYING.LGPLv2.1` inside the zip) and linked into the game; you rebuild and relink it from source yourself. On Linux: the system's FFmpeg. Source: <https://ffmpeg.org/download.html> |

## Built into the game with the Vulkan backend

Only in a build made with `build.ps1 -Vulkan` (`-DTF_VULKAN=ON`); the Vulkan backend is off by default while it is in
development.

| Component | Author | Licence | How it is used |
|---|---|---|---|
| [SDL3](https://github.com/libsdl-org/SDL) | Sam Lantinga and the SDL contributors | zlib | The Vulkan backend's window (`release-3.4.18`, fetched, built static) |
| [volk](https://github.com/zeux/volk) | Arseny Kapoulkine | MIT | Loads the system's Vulkan at run time (`vulkan-sdk-1.4.363.0`, fetched) |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | The Khronos Group | Apache-2.0 or MIT | The Vulkan API's headers (`vulkan-sdk-1.4.363.0`, fetched) |
| [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | Advanced Micro Devices | MIT | Video memory sub-allocation (`v3.4.0`, header only, fetched) |

## Built into the game with the FidelityFX SDK (FSR 3 / FSR 4)

Every build fetches these (TODO 299; `build.ps1 -NoFsrSdk` leaves them out). FSR 3 / 4 is off in the settings by default.

| Component | Author | Licence | How it is used |
|---|---|---|---|
| [FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) API headers | Advanced Micro Devices, Inc. | MIT | The SDK's API (`transformers/native_render/ext/fidelityfx`) |
| FidelityFX SDK signed DLLs (`amd_fidelityfx_loader_dx12.dll`, `amd_fidelityfx_upscaler_dx12.dll`: FSR 3.1 / FSR 4) | Advanced Micro Devices, Inc. | AMD's licence below (`docs/license.md` of the SDK, release v2.4.0) | Downloaded by the build on your PC from AMD's own repository (release v2.4.0, SHA-256 checked) and placed beside the game; never in this repository or in the setup program |

AMD's licence for the FidelityFX SDK v2.4.0 (`docs/license.md`, quoted in full except its list of files). The SDK's
licence puts most of its files under the first notice and lists the files under the second; the two DLLs above are in
that list. Both notices are reproduced here as the licence asks.

> The following license applies to all files except as noted below.
>
> Copyright (C) Advanced Micro Devices, Inc.
>
> REDISTRIBUTION: Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the “Software”), to install, reproduce, copy and distribute copies of the Software, in binary form only, and to permit persons to whom the Software is provided to do the same, provided that the following conditions are met:
>
> No reverse engineering, decompilation, or disassembly of this Software is permitted.
>
> Redistributions must reproduce the above copyright notice, this permission notice, and the following disclaimers and notices in the Software documentation and/or other materials provided with the Software.
>
> DISCLAIMER: THE USE OF THE SOFTWARE IS AT YOUR SOLE RISK.  THE SOFTWARE IS PROVIDED “AS IS” AND WITHOUT WARRANTY OF ANY KIND AND COPYRIGHT HOLDER AND ITS LICENSORS EXPRESSLY DISCLAIM ALL WARRANTIES, EXPRESS AND IMPLIED, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. COPYRIGHT HOLDER AND ITS LICENSORS DO NOT WARRANT THAT THE SOFTWARE WILL MEET YOUR REQUIREMENTS, OR THAT THE OPERATION OF THE SOFTWARE WILL BE UNINTERRUPTED OR ERROR-FREE.  THE ENTIRE RISK ASSOCIATED WITH THE USE OF THE SOFTWARE IS ASSUMED BY YOU.  FURTHERMORE, COPYRIGHT HOLDER AND ITS LICENSORS DO NOT WARRANT OR MAKE ANY REPRESENTATIONS REGARDING THE USE OR THE RESULTS OF THE USE OF THE SOFTWARE IN TERMS OF ITS CORRECTNESS, ACCURACY, RELIABILITY, CURRENTNESS, OR OTHERWISE.
>
> DISCLAIMER: UNDER NO CIRCUMSTANCES INCLUDING NEGLIGENCE, SHALL COPYRIGHT HOLDER AND ITS LICENSORS OR ITS DIRECTORS, OFFICERS, EMPLOYEES OR AGENTS (“AUTHORIZED REPRESENTATIVES”) BE LIABLE FOR ANY INCIDENTAL, INDIRECT, SPECIAL OR CONSEQUENTIAL DAMAGES (INCLUDING DAMAGES FOR LOSS OF BUSINESS PROFITS, BUSINESS INTERRUPTION, LOSS OF BUSINESS INFORMATION, AND THE LIKE) ARISING OUT OF THE USE, MISUSE OR INABILITY TO USE THE SOFTWARE, BREACH OR DEFAULT, INCLUDING THOSE ARISING FROM INFRINGEMENT OR ALLEGED INFRINGEMENT OF ANY PATENT, TRADEMARK, COPYRIGHT OR OTHER INTELLECTUAL PROPERTY RIGHT EVEN IF COPYRIGHT HOLDER AND ITS AUTHORIZED REPRESENTATIVES HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGES.  IN NO EVENT SHALL COPYRIGHT HOLDER OR ITS AUTHORIZED REPRESENTATIVES TOTAL LIABILITY FOR ALL DAMAGES, LOSSES, AND CAUSES OF ACTION (WHETHER IN CONTRACT, TORT (INCLUDING NEGLIGENCE) OR OTHERWISE) EXCEED THE AMOUNT OF US$10.
>
> These source files [the list] are subject to the following license:
>
> Copyright (C) Advanced Micro Devices, Inc.
>
> Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the “Software”), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

AMD, FidelityFX and FSR are trademarks of Advanced Micro Devices, Inc.

## Build tools

| Component | Author | Licence | How it is used |
|---|---|---|---|
| [ELFIO](https://github.com/serge1/ELFIO) | Serge Lamikhov-Center | MIT | Reading the game's program (recompiler) |
| [rabbitizer](https://github.com/Decompollaborate/rabbitizer) | Decompollaborate | MIT | MIPS instruction decoding (recompiler) |
| [libdwarf](https://github.com/davea42/libdwarf-code) | David Anderson and others | LGPL-2.1 (library) | Debug information (recompiler) |
| [nlohmann/json](https://github.com/nlohmann/json) | Niels Lohmann | MIT | JSON (analyzer) |
| [DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler) | Microsoft and the LLVM contributors | University of Illinois/NCSA and MIT (the release's `LICENSE-LLVM.txt`, `LICENCE-MIT.txt`) | Compiles the Vulkan backend's shaders to SPIR-V during a `-Vulkan` build (`dxcompiler.dll` from the v1.9.2609 release, downloaded with its SHA-256; not placed beside the game) |

## Made on your PC by the setup

| Component | Author | Licence | How it is used |
|---|---|---|---|
| [Real-ESRGAN ncnn-vulkan](https://github.com/xinntao/Real-ESRGAN-ncnn-vulkan) | Xintao Wang | MIT | Upscaling the textures from your disc (downloaded by `tools/make_hd_textures.ps1`) |
| [Real-ESRGAN models](https://github.com/xinntao/Real-ESRGAN) | Xintao Wang | BSD-3-Clause | The upscaling models |
| [TeX Gyre Adventor](https://www.gust.org.pl/projects/e-foundry/tex-gyre) | GUST e-foundry | GUST Font License | Redrawing the game's fonts sharp (`tools/hd_textures/typeface`) |

## Setup program

| Component | Licence |
|---|---|
| [egui / eframe](https://github.com/emilk/egui) | MIT or Apache-2.0 |
| [rfd](https://github.com/PolyMeilex/rfd) | MIT |

## Fonts from Windows

The settings menu uses the Bahnschrift font that comes with Windows. It is read from your Windows installation
and is not included here.

## The game

*Transformers* is a trademark of Hasbro. The game is copyright its owners (Hasbro, Atari, Melbourne House). No part
of the game is included in this repository or in the setup program. It is built from the player's own disc.
