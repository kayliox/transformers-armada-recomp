# Build robustness: how the install can fail, and what removes whole classes of failure

The game is built on the player's PC from the player's own disc: no game code is shipped. So every install runs a
real toolchain (Git, Python, CMake, Visual Studio's C++ tools) on a PC we have never seen. This page lists every
install, build and update failure players have reported so far, groups them into classes, and says what the setup
does about each class.

## 1. The failure map (every reported case, by stage)

Stages, in the order the setup runs them: **T** Tools (winget installs, Python, CMake), **D** Download (git clone
or pull, submodule), **P** PS2Recomp patches, **R** recompiler tools build (CMake configure + FetchContent + MSBuild),
**S** scan / function recovery / translation, **G** game configure + compile + link, **H** HD textures, **U** the
update path (update.ps1), **X** first start after the build.

| TODO | Stage | What the player saw | Cause | Detected today by | Class |
|---|---|---|---|---|---|
| 254 | T | "powershell could not be started: program not found" | PATH without Windows' own folders | setup starts PowerShell by full path, repairs PATH | PC state |
| 253 | T | "prerequisites installed, but not found yet: vs" | Visual Studio present without the C++ tools; winget says "already installed" | vswhere -all, adds the C++ tools | toolchain variance |
| (0.2.5) | T | "no newer package versions are available" | Git installed but not on PATH | Git found where it is | PC state |
| (0.2.7) | T | "pip install cmake failed" | another program's Python (Inkscape) first on PATH | real-Python detection | PC state |
| (0.6.x) | T | "not found yet: python" | Store shortcut / per-user Python; winget trusts "already installed" | each tool checked after its install | PC state |
| 438 | T | "Git could not be installed or found" (winget 0x8a15000f) | damaged winget source catalogue | text matcher, `winget source reset` once | PC state |
| 419 | T | "CMake ... still does not know Visual Studio Build Tools 2022" | a [string] parameter turned $null into "" (our bug) | regression test | our regression |
| 407 | T/R | (VS 2026 only) "could not find any instance of Visual Studio" | pip CMake older than the PC's Visual Studio | Get-VsUnknownToCmake, pip upgrade | toolchain variance |
| (0.2.x) | D | "detected dubious ownership" | install on exFAT / FAT32 | safe.directory for our git calls | PC state |
| 261 | R | "No CMAKE_CXX_COMPILER could be found" | CMake picked a Visual Studio without the C++ tools | build.ps1 names the install | toolchain variance |
| (0.6.6) | R | "Cannot convert ... to type System.Version" | two Visual Studio installs, PowerShell 5.1 JSON array | fixed parse | toolchain variance |
| 407 | R | error C7595 in vu_translator.cpp (fmt 12 consteval) | a compiler version we do not have rejects fmt's compile-time check | patch 1005 turns the check off (all releases since 0.8.2) | toolchain variance |
| 375 | G | "Unknown arguments specified" in CopyFfmpegDlls.cmake | `cmake -P` script without its own policy version, CMake 3.x | CMake 3.31 gate on every build-script change | toolchain variance |
| 424 | G | C2065 'queue' in nr_d3d.cpp | Visual Studio 2019's older Windows SDK | header guard + old-toolchain build gate | toolchain variance |
| 378 | G/U | LNK1136 / LNK1106 on a batch .obj, "Try again" never passes | an object file damaged after MSBuild recorded it (crash, power loss, antivirus, disk) | heal by linker error code, sweep of empty objects | damaged outputs |
| 381 | R/G | every build reconfigured from scratch (minutes lost) | cache key read with the wrong type | fixed parse | our regression |
| 371 | G/X/U | "Bad Image ... 0xc0e90002", "An Application Control policy has blocked this file" | Smart App Control refuses unsigned files (DLLs, the built exe) | plain message by exit code; static FFmpeg, fewer DLLs | security software |
| (0.6.x) | R/S | a built tool vanishes | antivirus quarantines ps2_analyzer.exe / ps2_recomp.exe | Assert-NotBlocked after the tools build | security software |
| 441 | S | "Scanning for SDK functions" for over an hour | unknown: antivirus scan of a new exe, an unexpected ELF, a slow disk | nothing yet (stall watchdog in progress, separate work) | silent stall |
| 421 | R/G | the setup window freezes for the whole build | the window stops getting frames during the long step | not detected | silent stall |
| (0.6.0) | G | European disc install failed | PAL addresses not translated | check_pal_addresses.py at every build | disc variance |
| (any) | P | "git apply failed" | a changed or mismatched PS2Recomp tree | the series is reset and applied whole | PC state |
| 425 | U | "every update runs the full compile" | some releases change a header all generated code includes | update_forecast.ps1 says it up front | expectation |
| 439 | all | "30 GB and an insanely long install" | toolchain + build tree + HD textures; FetchContent clones full git histories | not measured per folder yet | size / time |
| 362 | X | black window with sound after install | the window hand-over failed silently | fallback to GL's own swap | first start |
| (0.6.x) | H | "upscale_textures.py failed with exit code 1" | a record file left empty | records written whole | damaged outputs |
| 370 | CI | the setup program's own test failed on the runner | a process id reused by Windows | job-member check | our regression |

## 2. The classes, and how often each one reached players

1. **Toolchain variance** (253, 261, 407 twice, 375, 419, 424, the System.Version case): the player's Visual Studio,
   compiler, Windows SDK or CMake is not the one we built with. The largest class, and the one that keeps
   producing new surprises: every new Visual Studio or CMake release is a new combination.
2. **PC state** (254, 438, Git / Python / PATH cases, dubious ownership): the PC's own tools or settings are broken
   or unusual.
3. **Security software** (371, the antivirus cases, possibly 441): files built or downloaded on the PC are refused,
   removed or held.
4. **Damaged outputs** (378, the HD record files): a file half written by an earlier run poisons every later run.
5. **Silent stalls** (441, 421): nothing fails, nothing moves, the player gives up.
6. **Network during the build**: CMake fetches 11 libraries with `git clone` during the configure. No player has
   reported a failure there yet, but it is the one step that needs GitHub, the player's git settings and long
   paths all at once, and it costs about 1 GB of git history on disk (measured: toml11 338 MB, raylib 413 MB,
   nlohmann_json 172 MB of .git each, the rest smaller, raylib twice when both build folders exist).
7. **Our own regressions** (375, 381, 419, 370): each caught later than it could have been, because the release
   checks build with one toolchain only.


## 3. What removes each class

| Class | Change | Status |
|---|---|---|
| Network during the build | Every library CMake used to clone with git is fetched first, as a pinned source archive with its SHA-256 checked, into one shared folder (out\deps); CMake is pointed at those folders and never goes to the network or runs git for them. A folder an older build already holds is kept as it is (no forced rebuild). A tag the CMake file moved, a failed download or a wrong hash leaves that library to CMake's own fetch, as before. | done, 4.1 |
| PC state | Checks before the long steps (free space, the folder, the disc), and the build leaving the PC's own compiler / CMake / Python environment settings out. | done, 4.2 and 4.4 |
| Damaged outputs | The PS2Recomp files the patch series writes are hashed after it is applied and checked at each build; a changed or restored file applies the series again. (Damaged object files: the TODO 378 heal, unchanged.) | done, 4.5 |
| Every class, faster answers | A report file written when the setup or an update stops: the step, the PC, the tools, the build's state, the ends of the logs; no game files, the user name taken out. | done, 4.3 |
| Toolchain variance | Measured, not changed: within Visual Studio 2022 every player gets the same compiler (17.14's MSVC 14.44). The variance comes from PCs that already have another Visual Studio (2019, 2026). Options in section 5. | decision |
| Our own regressions | A CI job that builds the recompiler tools on two Windows images, CMake 3 and 4, and a path with spaces, also weekly. Public repository: needs a decision. | branch buildrobust-ci |
| Silent stalls | The stall watchdog on the long steps (TODO 441, tools\step_watchdog.ps1). Reused, not forked. | other work |
| Size and time (439) | Measured folder by folder; the libraries' git histories gone (about 1.1 GB less, and faster); the plan said up front and the build's leftovers offered for deletion. | done, section 6 |

## 4. Changes made (branch buildrobust)

### 4.1 The libraries as pinned archives (tools/fetch_deps.py, tools/deps.json, tools/deps.ps1)

CMake's FetchContent cloned 11 libraries with git during the configure (fmt, toml11, ELFIO, libdwarf, rabbitizer,
nlohmann_json for the recompiler tools; raylib, imgui, rlImGui, xbyak, opus for the game), each with its whole history.
Now build.ps1 and build.sh fetch each as a source archive named in tools/deps.json, check its SHA-256, unpack it into
out\deps (atomically: a cut download or unpack never looks finished) and pass `-DFETCHCONTENT_SOURCE_DIR_<NAME>`.

- Same sources: each archive was compared file by file with a git checkout of the same tag: identical, line endings
  aside (raylib differs only in the two files the build itself edits).
- Safe for existing installs: a library a build folder already holds is kept (empty value), so an update never
  rebuilds because of this. A wrong hash, a failed download, or a CMake file whose GIT_TAG no longer matches the pin
  falls back to CMake's own fetch, as before. With no network the first library's failure skips the rest at once
  (no 3 x 60 s wait per library). TF_DEPS=0 turns it off.
- Measured (Testbench, fresh public build, VS 2022 Build Tools 17.14, pip CMake 4.4.5): exit 0 in 10 min 37 s;
  out\ 1.81 GB with no git folder (the same install with git clones: out\ 2.95 GB, 12 min 8 s). out\deps holds all
  11 libraries in 78 MB, shared by every build folder (raylib was cloned once per build folder before).
- Tests: tests/fetch_deps_tests.py (17 checks, no network); an old install rebuilt with this code: nothing fetched,
  tools and translation "as before", nothing compiled (28 s).

### 4.2 Checks before the long steps (tools/preflight.ps1)

setup.ps1 runs them before anything is installed; update.ps1 before a rebuild. Stops, in plain words with the fix:
not enough space on the install's drive or on Windows' drive (the measured needs of section 6), a network share or
drive, a file that is not a known disc or whose executable has the wrong checksum (`extract_iso.py --check` reads only
the executable, so a wrong disc or a bad copy is found before 30 minutes of installs; before, the checksum was checked
only at the extraction, after the programs were installed). Says and goes on: a cloud-synced folder (OneDrive,
Dropbox, Google Drive, iCloud), letters outside plain English, a path too long for Windows' limit with long paths off,
FAT32, Smart App Control on, the antivirus programs, less than 8 GB of memory.
Linux (setup.sh): the same free-space stop and disc check (tested on debianbox).
Tests: tests/preflight_tests.ps1 (simulated drives and folders, the not-a-disc case, dot-sourcing leaves the caller's
variables alone).

### 4.3 The report (tools/setup_report.ps1)

When setup.ps1 or update.ps1 stops, `tfa-report-<date>-<time>.txt` is written to the Desktop and its path is said in
the error the setup window shows. It holds where it stopped, the PC (Windows, memory, free space and format of the
drive, long paths, Smart App Control, antivirus, graphics), the tools (Git, Python, CMake, winget, every Visual
Studio with its C++ tools and MSVC toolsets, the Windows SDKs), the checks of 4.2, the build's state (patches applied,
tools and game present, each CMake cache's generator, toolset and library folders) and the ends of the logs. No game
files; the user name, the user folder and the PC's name are replaced by <user>, <home>, <pc>. One text file, not a
zip: Discord shows a text attachment's preview. It also runs on its own (`tools\setup_report.ps1`).
Tests: tests/preflight_tests.ps1 (content, redaction, setup.ps1 writing it when it stops).

### 4.4 The PC's own build settings left out (build.ps1)

A Visual Studio developer prompt's INCLUDE / LIB, extra compiler options in CL / _CL_, another toolchain's CC / CXX /
CFLAGS / LDFLAGS, a CMake toolchain file, generator, prefix path or project include, HP PCs' Platform variable, a
PYTHONPATH / PYTHONHOME: build.ps1 removes them from its own environment (so from every program it starts; the PC is
not changed) and names them once. Test (Testbench): CL, INCLUDE, CMAKE_GENERATOR, Platform and PYTHONHOME set to
harmful values: all named, the build exit 0.

### 4.5 The patched PS2Recomp tree checked at each build (build.ps1)

out\ps2recomp-patches.txt said which patch series was applied, not that the files were still as it left them. Now
the files the series writes (164) are hashed into out\ps2recomp-tree.txt after it is applied and checked at each
build (about 0.3 s); any difference applies the series again (identical files keep their times, so nothing extra is
compiled). Test (Testbench): patch 1005 undone with `git checkout` and a runtime file cut in half: both named, the
series applied again, 1005 back, exit 0 in 27 s.

This is also the build's answer to the second TODO 407 report: with patch 1005 in place, fmt's compile-time check is
off (FMT_USE_CONSTEVAL=0, so fmt has no consteval function left), and C7595 cannot come from fmt on any compiler. A
player who still sees it builds without 1005: a copy from before 0.8.2, or a tree changed since the patches. The
first needs a fresh download; the second is now repaired by the build itself.

### 4.6 TODO 439 from the setup lane (finished here)

tools/clean_build.ps1/.sh (delete the build's leftovers: temp = work\staging at no cost, all = everything but the
game's own folder, the next update then rebuilds once), the plan said before anything runs (setup.ps1 and the setup
window), update_forecast's "files were deleted" case, update.ps1's plain stop when the Build Tools were removed.
Sizes updated to the measured 2.3 GB build. Its setup-window code compiled green on Windows (cargo test 20 passed,
1 ignored; release build) and Linux (debianbox, 26 passed); tools/clean_build.sh tested on debianbox (the game and
out/deps kept, so the next rebuild needs no network).

### 4.7 Linux parity (branch buildrobust2)

tools/clean_env.sh, sourced by build.sh, setup.sh and update.sh, leaves another project's CMake toolchain,
generator or project include and another Python's PYTHONPATH / PYTHONHOME out of the build and names them once.
Narrower than Windows on purpose: CC / CXX / CFLAGS / LDFLAGS and CMAKE_PREFIX_PATH are how a Linux user or packager
chooses the compiler and the libraries, and build.sh honours them. build.sh hashes the files the patch series
writes (out/ps2recomp-tree.txt) and applies the series again when one changed, as build.ps1 does. update.sh stops in
plain words before a rebuild when under 1.5 GB is free, as update.ps1 does. On SteamOS / Bazzite the same scripts run
in the build container, so this covers the Steam Deck too.

### 4.8 The setup window, seen from its own render

`tfa-setup.exe --shot <file.bmp> --offscreen` (the window placed where no screen is; it writes its own picture,
nothing on any screen is captured) now waits for the threads that find the installed tools and the build's leftovers
before it takes the picture, and `TFA_PLAN_FOUND="git=0,python=0,vs=0"` (a testing aid, as TFA_LOW_END) shows the plan
as a PC without the tools sees it. The first pictures showed the plan in the small font with wrapped lines falling
back to the edge: the lines are now at the normal size and wrap inside their indent. docs/images/setup-plan.jpg (in
the README) is that render.

### 4.9 Two Linux build breaks found by the end-to-end runs (hotfix branch hf-linux-glshare)

A fresh Linux build of a public-shaped tree (no overlay: the public repository's) on debianbox found two breaks in
the released code, each a stand-in in nr_none.cpp (the Linux build's replacement for Windows-only renderer files)
that fell out of step with what it stands in for:

1. Every Linux build since 0.8.9 stopped at the final link: `undefined reference to nr::glshare::finish()`
   (nr_present.cpp called a function nr_none.cpp did not stub). Fixed by 757b36bf, released in 0.9.0.
2. Linux builds without vkd3d (its tools missing, its download or build failing, or `--no-vkd3d`) stopped compiling
   nr_none.cpp since 0.8.9: its NoneBackend still overrode methods nr_backend.h no longer has. Fixed by 77234491, released in 0.9.1.

Why CI missed them: the Linux job compiles the port's objects with vkd3d only, and cannot link without the game's
translated code (a disc). Branch buildrobust-ci adds a no-vkd3d compile pass, which would have caught the second.
The first can only be caught by a real Linux build, so a Linux build belongs in the release checks (as the CMake 3
build on .165 does for Windows).

## 5. Decisions (the user, 2026-11-19: "do your recommendations")

- CI on the public repository: YES. The toolchain job (tools-toolchains.yml) and the Linux no-vkd3d compile pass are
  merged and go public with the next release.
- Players with only Visual Studio 2026 or 2019: LEAVE AS IS until a real player report; then add the 2022 compiler.
- Components-only Build Tools install: NO (it saves only 0.2 to 0.5 GB).
- Release gate added: a public-shaped Linux build that LINKS on debianbox before releases touching shared C++
  (LANE_RULES "Linux release gate").

The options as they were laid out:

1. **The CI job on the public repository** (branch buildrobust-ci, one workflow file): builds the recompiler tools on
   windows-2022 and windows-2025, CMake 4 and 3, and a path with spaces, when what they are built from changes and
   every Monday. It would have caught TODO 375 (CMake 3) and path problems; for TODO 407 it catches a new Visual Studio
   on GitHub's machines, not every player's.
2. **Players with only Visual Studio 2026 or 2019.** The setup installs the 2022 Build Tools only when no Visual Studio
   with the C++ tools exists; such a PC builds with its own (2019 is covered by the Testbench's old-toolchain build;
   2026 is untested anywhere). Options: (a) leave it (today); (b) install the 2022 Build Tools beside any other
   version (about 2 GB download and 6 GB on disk, for those players only), so every player builds with the tested
   compiler; (c) add the MSVC v143 toolset to an existing 2026 install and build with `-T v143` (smaller, untested,
   needs Windows' permission prompt). Suggested: (b), but only if a 2026 player reports a failure.
3. **A components-only Build Tools install** (no `--includeRecommended`): measured, not worth it. In the 2022 Build
   Tools on the Testbench, MSVC is 1.84 GB, the IDE core about 1 GB and the Windows SDK about 2 GB, all needed by the
   build; the extras it does not use (ASAN, test tools, CMake integration, vcpkg) come to about 0.2 to 0.5 GB.
   Suggested: no change (it would need a test install on a worker for little gain).

## 6. Install size and time (TODO 439)

A fresh install measured on the Testbench (Ryzen 5 5600, 12 threads, NVMe), public build, Balanced HD textures:

| Part | Before | Now |
|---|---|---|
| Git | 0.40 GB | same |
| Python, and Pillow, numpy, CMake (pip) | 0.39 GB | same |
| Visual Studio 2022 Build Tools (C++) | 6.1 GB (1.9 GB download) | same (see 5.3) |
| Disc files | 2.70 GB | same |
| The build (out\ and work\) | 3.43 GB, 12 min 8 s | 2.29 GB, 10 min 37 s |
| HD textures, Balanced | about 1.4 GB, about 40 min (GTX 1080) | same |
| In all | about 14.4 GB | about 13.3 GB |

After the build, `tools\clean_build.ps1 -Level all` frees all of the build except the game's own folder (0.30 GB kept);
the next update then rebuilds once. Why the build is needed at all: no game code may be shared, so the game is built
on your PC from your own disc; the three programs are Git, Python and Microsoft's free C++ Build Tools.

## 7. Test evidence (branch buildrobust)

| Where | What | Result |
|---|---|---|
| this PC | tests/fetch_deps_tests.py, preflight_tests.ps1, setup_prerequisites_tests.ps1, update_choice_tests.ps1, clean_build_tests.ps1, build_watchdog_tests.ps1 | all pass |
| this PC | the 11 archives against git checkouts of the same tags | identical files (line endings aside) |
| this PC | installer: cargo test (Windows) + release build | 20 passed, 1 ignored; built |
| debianbox | installer: cargo test (Linux) | 26 passed |
| debianbox | recompiler tools built as build.sh does it, libraries from archives, CMake 3.31.6, g++ | built in 32 s, no git folder |
| Testbench | fresh public build.ps1, VS 2022 17.14, pip CMake 4.4.5 | exit 0, 10 min 37 s, out\ 1.81 GB, no git folder |
| Testbench | update to the new head, nothing else changed | exit 0, 29 s, 1 file compiled |
| Testbench | patch 1005 undone + a patched runtime file cut in half | both named, series applied again, 1005 back, exit 0, 27 s |
| Testbench | CL, INCLUDE, CMAKE_GENERATOR, Platform, PYTHONHOME set to harmful values | all left out and named, exit 0 |
| Testbench | a build stopped mid-compile (9 empty objects left), then run again | the 9 named and made again, 197 compiled, exit 0, 2 min 50 s |
| Testbench | an install from before (libraries cloned by CMake), rebuilt with the new build.ps1 | nothing fetched, "as before", nothing compiled, 28 s |

| Testbench | fresh install through setup.ps1 (PC's own PATH: system Python, pip CMake 4), stopped 60 s into the compile, setup.ps1 again | resumed at the compile ("translated as before"), 2 empty objects made again, "Ready", exit 0 in 7 min 56 s; out\ 1.94 GB, no git folder |
| Testbench | an install made with the current release (0.8.9) by setup.ps1, then update.ps1 -StashLocalChanges to this branch | exit 0 in 41 s, 1 file compiled, its old libraries kept; its out\ 3.07 GB against 1.94 GB for the fresh one above |
| .165 | clean-folder build.ps1 with only Visual Studio's CMake 3.31.6 on PATH, from the disc image | normal exit 0 in 14.2 min, -Public exit 0 in 7.5 min, all caches 3.31, libraries from out\deps (80 MB), no git folder |
| .165 | extract_iso.py --check and the preflight on the real USA disc image | "its executable checked", exit 0; the preflight names the PC's Malwarebytes with the folder to allow |
| Testbench | the setup window's own render (tfa-setup.exe --shot --offscreen): the plan as a PC without the tools sees it, the plan with them, a finished install's clean-up offer | written in 1 to 2 s each; the plan now readable (normal-size lines) |
| debianbox | (branch buildrobust2) a fresh setup.sh of the public-shaped release code with the link fix, then update.sh to this branch | setup exit 0 in 12.7 min (no git folder under out); update exit 0 in 18 s, the tree record written |
| debianbox | patch 1005 undone + a patched file cut in half, CMAKE_TOOLCHAIN_FILE / CMAKE_GENERATOR_PLATFORM / PYTHONHOME set, build.sh --no-vkd3d | the three named and left out, the series applied again, 1005 back, exit 0 |
| debianbox | build.sh stopped 40 s into its compile (1 empty object left), then run again | exit 0, 329 batch objects, none empty |
| debianbox | installer: cargo test (Linux) | 27 passed |
