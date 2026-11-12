# Steam Deck performance

This page separates current Linux validation from older PCSX2 comparisons. Results from different versions are not a
matched before-and-after test. The native Linux build does not use Proton.

## Current source and device

| Item | Configuration |
|---|---|
| Source | `1f48fca2196676c9042a0b218a29ac2441bce945`, based on upstream 0.6.4, `fe11b3723e634fc3942c1438ce1f5106b56c8d8e` |
| Device | Steam Deck OLED, Gaming Mode, 1280x800 panel at 90 Hz |
| System | SteamOS 3.9.5, kernel 7.2.7-valve2, Mesa RADV |
| Build | glibc 2.35 target, native Direct3D 12 renderer through static vkd3d 2.1 |
| Game | Transformers (USA), SLUS-20668, from the player's own disc |
| Settings | High, AA 2, draw distance 3, FOV 1.2, HD textures off; observed gameplay picture 1280x801 |

The native measurement executable is unstripped, with SHA-256
`7ffb07bb5347a75ab56f1aaa7f18aa115a092fac97eb271ea3790fed96b7deda`.
The separate emulated-only executable has no vkd3d and has SHA-256
`f2d1cd34a378c1ebf21f8209e434b094563493d0e91c909bf7910912393e45a5`.
Neither replaces the player's installed executable.

The current runs match the game window's approximately 16:10 shape. The log reports `live picture: 1280x801`;
the historical 16:9 runs below used 1280x720. These are not equal-resolution comparisons.

### Compatibility checks

- The native, emulated-only and optional Vulkan Linux variants build. The optional Vulkan backend also passes its
  picture-ownership regression. This is not a claim that the experimental backend was used for the Deck measurements.
- [Linux gcc and clang CI](https://github.com/ravioli-gh/transformers-armada-recomp/actions/runs/43109315224) passes on
  this source. The existing stick-response and HD-progress regressions pass too.
- On the Deck, a live 45 → 240 → 45 change restores fast-rate fallback without a restart. A separate
  `TF_FPS_LOCK=60` run restores the 60-picture lock after 60 → 45 → 60. The logs record each applied setting; this does
  not mean this scene sustains 240 frames a second.
- The 45 option, below-60 picture rules, window-size correction and CPU picture hand-over are Linux-only. Windows
  keeps upstream's vector/newest-picture hand-over and movie-band geometry. Windows runtime testing was not done.

### Measurement rules

Launch through Steam, not directly over SSH. Use task-owned executables, settings, memory cards and shader caches.
Check both compositor focus fields and the owned game's window tag on its actual X server. Keep the installed binary,
settings and shortcut unchanged, and verify restoration after each run. A picture of Steam's library is not proof that
an invisible game was measured correctly.

The scripted route reaches Amazon Basin through the HQ. Loaded timing starts only after the HQ-end and post-load
renderer markers, plus a settling interval; it is not backdated if those markers arrive late. The script stops moving
at about 188 seconds after launch. A longer run then observes the same static gameplay scene, not continuous traversal
or repeated level loads.

- CPU is process CPU time; 100% is one core.
- APU power is AMD's `power1_average` sensor, not battery draw or whole-device power.
- GPU busy and VRAM are device-wide. RSS is resident process memory. On this shared-memory APU, these memory accounts
  can overlap: do not add RSS and VRAM into a total game-memory figure.
- New-picture rate, repeats and never-shown counts come from game-window telemetry, not physical panel presents.
  A mean rate alone does not describe frame-time variation.
- A callchain profile is attached only to the owned game PID. Its enabled sampling interval is timed from perf's
  enable acknowledgment to the disable request, not process spawn through finalization. Profiled CPU/power readings
  are not an unprofiled performance baseline.
- A finite soak cannot rule out slower leaks or problems in other places. Report total memory change and the late
  slope together; do not convert a plateau in one scene into a whole-game leak-free claim.

## Historical comparison: upstream 0.4.6 era

**Not remeasured on 0.6.4.** The port rows below are from `a4de169`, before the later Linux sizing, pacing, virtual-heap
and sound-mixer work. They must not be described as current performance or used to assign a gain to any one change.

The device was the same Deck OLED, SteamOS 3.9.5, 1280x800 at 90 Hz. PCSX2 was the official 2.8.2 AppImage, Vulkan,
default speed settings, fast boot, with the player's own BIOS dump and the same USA disc image. No texture replacement
pack was used for PCSX2. The port used High/AA2/draw distance 3/FOV 1.2 and a 1280x720 gameplay picture.

Amazon Basin's opening path was reached through New Game on Recruit. PCSX2 used a savestate made after the menus;
the port used scripted input, skipped films and an empty test memory card. Both walked, turned and fired during
sampling, but did not render identical frames. Each configuration had three approximately 10-second samples around
150–190 seconds after launch. CPU, GPU busy, APU power and RSS below are averages. FPS and PCSX2 speed are single
screenshot readings, not average throughput. Memory units retain the original report's MB labels.

| Configuration | FPS, one reading | PCSX2 speed | Render resolution | CPU % | GPU busy % | APU W | RSS MB |
|---|---:|---:|---|---:|---:|---:|---:|
| PCSX2 1x | 29.98 | 100% | 512x448 | 149.6 | 24.3 | 6.1 | 834 |
| PCSX2 2x | 29.97 | 100% | 1024x894 | 148.3 | 65.3 | 7.9 | 696 |
| PCSX2 3x | 27.52 | 92% | 1536x1341 | 144.7 | 82.0 | 9.1 | 689 |
| PCSX2 4x | 30.64 | 100–102% | 2048x1792 | 141.9 | 87.7 | 12.0 | 708 |
| Port, native, uncapped | 83 | — | 1280x720 | 248.5 | 91.3 | 15.8 | 762 |
| Port, emulated, uncapped | 45 | — | 512x448, scaled | 271.2 | 76.0 | 9.8 | 565 |

The old port cap-30 run lost foreground focus for part of sampling. It is excluded from CPU, GPU and power
comparisons. In particular, it is not a valid current 30-FPS power baseline or evidence of a saving over PCSX2.

PCSX2's EE/GS/GPU OSD readings were 71/74/3% at 1x, 71/79/54% at 2x, 61/82/76% at 3x and 62/75/92% at 4x. The 3x and
4x screenshots had different views, including a large foreground tree in the 4x shot; their single FPS readings are
not a monotonic scaling benchmark. Elsewhere on the route, PCSX2 1x screenshots showed HQ at 96% speed, loading at
39–49%, and the opening film's space scene at 54%. These are historical observations, not measurements of every scene.

### Historical HD texture comparison

The port's tools made the Amazon and shared texture set from the player's disc: 1,089 images, 2.8 GB, in 2 hours
14 minutes. A preliminary unmeasured run filled its BC7 cache from 5 files/28 MB to 182 files/644 MB. The following
0.4.6-era runs used the same route, with the pack on disk and HD on or explicitly off. PCSX2 had no matching pack.

| Configuration | FPS, three readings | CPU % | GPU busy % | APU W | RSS MB | VRAM MB |
|---|---|---:|---:|---:|---:|---:|
| HD on | 59 / 72 / 72 | 233.6 | 97.3 | 15.0 | 1054 | 1015 |
| HD on, again | 63 / 75 / 75 | 242.3 | 92.7 | 15.2 | 1010 | 1015 |
| HD off | 76 / 83 / 83 | 243.7 | 91.7 | 15.6 | 782 | 638 |

A separate `TF_NATIVE_NAMES=1` diagnostic selected large images for 180–186 textures in the first 190 seconds,
including 73 Amazon textures. These rows establish the historical HD trade-off only; the current source's cold-load
peak, HD frame rate and shader-start time have not been remeasured here.
