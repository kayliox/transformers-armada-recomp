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

## HD textures and sharp fonts

```powershell
.\tools\make_hd_textures.ps1
```

This makes every texture of the game again at a higher resolution on your graphics card (with Real-ESRGAN, on
Vulkan), and redraws the fonts and button prompts sharp. It writes into `hd_textures\`, where the game finds them.
It takes from about ten minutes to an hour or more. `-Quality` picks how much each kind of texture is enlarged
(`tier_rules.PRESETS`). The sizes were measured per kind of texture: how far each one can be enlarged before a
larger image stops looking any different at the distance it is usually seen from, on a 1080p, 1440p or 4K screen.

| Preset | For | Player / enemies, Mini-Cons / levels (terrain, foliage, sky) / effects / UI | HD set (renderer's form) | Making time, RTX 3070 |
|---|---|---|---|---|
| `fast` | Steam Deck, 1080p | 8x / 4x / 4x / 4x / 2x | about 0.6 GB | about 10 min |
| `balanced` (default) | 1440p | 8x / 4x / 8x / 4x / 2x | about 1.4 GB | about 25 min |
| `best` | 4K | 16x / 8x / 8x / 8x / 4x | about 2.3 GB | about 35 min |

8x and 16x take two upscale passes, 2x and 4x one; nothing goes past 4096 px. An average PC takes about 1.5-2 times
as long. Images made larger by another preset, or before the presets, are shrunk to the chosen one on the CPU (a few
minutes, the same picture a fresh run makes); only the ones it needs larger are made on the graphics card. To change
the preset later: F1 > Graphics > HD texture preset in the game, the setup window's update screen, or
`-Quality` here.
`-FontsOnly` makes only the fonts and prompts, in a few minutes.

You can play while they are made: the ones seen first (the menus, the HQ, the characters, the first levels) come
first, and the game uses each one as soon as it is ready. While the game runs the upscale waits (the two share the
graphics card) and carries on when you close it; `"while_playing": true` in `hd_textures\hd_options.json` keeps it
going. If you close the window before it is done, the game carries on with it the next time it starts, in a small
window of its own (F1 shows how far it is).

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
