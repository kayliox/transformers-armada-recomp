# Building from source

Most players should use the setup program (see the [README](../README.md)). This page is for building by hand,
for example to work on the port.

## What you need

- Windows 10 or 11, 64-bit.
- **Visual Studio 2022** (or 2019) with **Desktop development with C++**. The free Build Tools are enough.
- **CMake 3.21 or newer.** `pip install cmake` works if Visual Studio's own CMake is older.
- **Python 3.11 or newer** with Pillow and numpy (`pip install pillow numpy`).
- **Git.**
- About 15 GB of free space.
- Your own disc image of **Transformers (USA)**, `SLUS-20668`, or of the European release, `SLES-52388`
  (experimental).

`setup.ps1 -InstallPrerequisites` installs the missing tools with winget.

## Build

```powershell
git clone --recursive https://github.com/kayliox/transformers-armada-recomp
cd transformers-armada-recomp
.\build.ps1 -Iso "D:\Games\Transformers (USA).iso"
```

Run it from a normal PowerShell window. CMake finds Visual Studio by itself.

The build:

1. checks that the disc is `SLUS-20668` or `SLES-52388` and extracts it into `game\` (the release's settings are
   in `transformers\config\versions\<serial>`; each build is for one release);
2. finds the functions in the game's program and translates them to C++ with PS2Recomp, into `work\`;
3. applies this port's patches to PS2Recomp and builds the executable into `out\`.

The first build compiles about 22,000 generated functions and takes 15 to 25 minutes on 8 cores.

After that, `.\build.ps1` rebuilds only what changed. `.\build.ps1 -SkipRecompile` skips the translation step,
for changes to the runtime or the port's code only (under a minute).

## Linux (and Steam Deck)

The game also builds as a native Linux program, with both renderers. The native renderer's Direct3D 12 runs through
vkd3d (Wine's Direct3D 12 on Vulkan): `build.sh` downloads vkd3d 2.1 once, checks it, and builds it statically into
`out/vkd3d-2.1`; without vkd3d's tools (or with `--no-vkd3d`) the game has the emulated renderer only.
`./setup.sh` sets up a player's copy (prerequisite checks, then this build), `./update.sh` updates it (also F1 >
About > Update now), `./play.sh` starts it.

You need CMake 3.21 or newer, Ninja, a C++ compiler (GCC or Clang), Python 3, Git, pkg-config, and the development
packages for X11, OpenGL and FFmpeg; for the native renderer also curl, bison, flex, Perl's JSON module and the
Vulkan and SPIR-V headers. On Ubuntu or Debian:

```sh
sudo apt install cmake ninja-build g++ python3 git pkg-config libgl-dev libx11-dev libxrandr-dev libxinerama-dev \
  libxcursor-dev libxi-dev libavcodec-dev libavformat-dev libavutil-dev libswresample-dev libswscale-dev \
  curl bison flex libjson-perl libvulkan-dev spirv-headers
```

Then:

```sh
git clone --recursive https://github.com/kayliox/transformers-armada-recomp
cd transformers-armada-recomp
./build.sh --iso "/path/to/Transformers (USA).iso"
```

`./build.sh --skip-recompile` rebuilds only the executable. Start the game with
`out/build/transformers/TransformersRecomp`.

The build copies the FFmpeg shared libraries it found beside the executable
(`tools/bundle_libraries.cmake`) and the executable looks in its own folder first (`$ORIGIN`), so the build carries
its movie decoding with it: a build made in a distribution's container runs on SteamOS, which has no FFmpeg or
another ABI. glibc, libstdc++ and libgcc stay the host's, and so do the graphics, audio and input libraries (GL,
EGL, Vulkan, X11/xcb, Wayland, DRM, ALSA, Pulse, udev and dbus): because the game's folder is a loader search path
for the host's own GPU driver too, the container's older copies would be picked up first on a newer SteamOS. FFmpeg's
other dependencies (the codec libraries and the like) travel with it. FFmpeg linked statically (no `libav*` in `ldd`)
copies nothing.

The emulated renderer's GPU path needs an OpenGL feature (`GL_ARB_fragment_shader_interlock`) that some drivers lack,
AMD's among them (the Steam Deck too). The game then runs OpenGL through Mesa's Zink driver on Vulkan, which has it.
`TF_ZINK=0` keeps the usual driver (the GS then draws on the CPU, slower); `TF_ZINK=1` always uses Zink.

## HD textures and sharp fonts

```powershell
.\tools\make_hd_textures.ps1
```

This makes every texture of the game again at a higher resolution on your graphics card (with Real-ESRGAN, on
Vulkan), and redraws the fonts and button prompts sharp. It writes into `hd_textures\`, where the game finds them.
It takes from tens of minutes to a few hours. `-Quality fast` makes a quicker set: one upscale pass (characters up to 2K, levels up to 1K), about ten times quicker.
`-FontsOnly` makes only the fonts and prompts, in a few minutes.

You can play while they are made: the ones seen first (the menus, the HQ, the characters, the first levels) come
first, and the game uses each one as soon as it is ready. While the game runs the upscale waits (the two share the
graphics card) and carries on when you close it; `"while_playing": true` in `hd_textures\hd_options.json` keeps it
going. If you close the window before it is done, the game carries on with it the next time it starts, in a small
window of its own (F1 shows how far it is).

On Linux, run `./tools/make_hd_textures.sh` instead (or add `--fonts-only` for fonts and prompts
alone). It needs Python 3 with Pillow and numpy, plus curl, unzip and a Vulkan driver for upscaling.
The native renderer loads them (the emulated one does not, as on Windows).

To check an existing set without upscaling, run `./tools/make_hd_textures.sh --verify`. It needs
Pillow and the existing `hd_textures/converted/textures` and `hd_textures/upscaled` folders.
It writes a report and a check cache, but does not change the images. After a check reports missing
or broken images, `./tools/make_hd_textures.sh --remake` removes those entries and resumes making them;
good images are kept.

On Linux, F1's HD buttons show these terminal commands; they do not start a background job.
Close the game before making or remaking textures to keep the two from sharing the graphics card.

## Play

```powershell
.\Play.bat
```

or start `out\build\transformers\RelWithDebInfo\TransformersRecomp.exe` directly. The game looks for your
extracted disc in `game\`.

## How it works

| Step | Tool | Output |
|---|---|---|
| Extract your disc and check it is `SLUS-20668` | `transformers/tools/extract_iso.py` | `game/` |
| Match Sony SDK library functions by signature | PS2Recomp `ps2_analyzer` | `work/analyzer.toml` |
| Find the function boundaries in the stripped program | `transformers/tools/find_functions.py` | `work/functions.csv` |
| Merge into the recompiler config | `transformers/tools/make_config.py` + `transformers/config/overrides.toml` | `work/transformers.toml` |
| Translate MIPS to C++ | PS2Recomp `ps2_recomp` | `work/staging/` |
| Install only the changed files | `transformers/tools/sync_generated.py` | `work/generated/` |
| Build the executable | CMake | `TransformersRecomp.exe` |

PS2Recomp is used from upstream, plus the fixes in `patches/ps2recomp/`, which `build.ps1` applies to the
submodule. The game-specific code lives in `transformers/src/`, and the Direct3D 12 renderer in
`transformers/native_render/`.

The game's program has no symbols, and it keeps code and data in one section. `find_functions.py` finds the
functions without Ghidra or IDA: it follows calls from the entry point, reads code pointers from data (C++
vtables, callback tables), resolves the compiler's switch tables, and finally sweeps the code nothing references.

## Repository layout

```
build.ps1                 the build
setup.ps1, update.ps1     the player's setup and updates
Play.bat                  starts the game
CMakeLists.txt            top level: the PS2Recomp runtime and the game
PS2Recomp/                the recompiler and runtime (git submodule)
patches/ps2recomp/        this port's changes to PS2Recomp, applied by build.ps1
transformers/
  config/                 the program's identity (CRC32), code regions, recompiler settings
  tools/                  disc extraction, function finding, config generation
  src/                    the port's code: launcher, game fixes, menus, input, sound
  native_render/          the Direct3D 12 renderer
tools/                    test and debugging helpers, the HD texture tools
docs/                     player guide, building, contributing, technical notes

game/       (ignored)     your extracted disc
work/       (ignored)     everything generated from your disc
out/        (ignored)     the build
hd_textures/ (ignored)    the HD textures made from your disc
states/     (ignored)     save states
```

Everything under the ignored folders is made from your disc. Never commit or share it.
