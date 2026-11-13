# Steam Deck performance

This page separates current Linux validation from older PCSX2 comparisons. Results from different versions are not a
matched before-and-after test. The native Linux build does not use Proton.

## Current source and device

| Item | Configuration |
|---|---|
| Source | `9f31d503a7c36e6814bc001828732471378af2ff`, based on upstream 0.6.6, `60a1f788c055fc535e0298e326c617217b32bb49` |
| Device | Steam Deck OLED, Gaming Mode, 1280x800 panel at 90 Hz |
| System | SteamOS 3.9.5, kernel 7.2.7-valve2, Mesa RADV 26.1.2 |
| Build | glibc 2.35 target, native Direct3D 12 renderer through static vkd3d 2.1 |
| Game | Transformers (USA), SLUS-20668, from the player's own disc |
| Settings | High, AA 2, draw distance 3, FOV 1.2, HD textures off, aspect Match screen (`TF_ASPECT=screen`) |

The native measurement executable is unstripped, with SHA-256
`a37d8482d65f61d6b6177d17f77dc060d59fe035049dd0da787f41bd7d314ca5`.
The separate emulated-only executable has no vkd3d and has SHA-256
`cadf735c295d47d6024e6c8d6b1e2313263c3415575b54ff1fb963081188e318`.
Neither replaces the player's installed executable. The installed binary and settings were read before and after each
run; both were unchanged.

The current runs use Match screen, so the picture follows the game window rather than a 16:9 target. The native runs
report a 1280x801 gameplay picture (`live picture: 1280x801`) and `picture shape 1.598:1`; the historical 16:9 runs
below used 1280x720. These are not equal-resolution comparisons.

### Compatibility checks

- The native, emulated-only and optional Vulkan Linux variants were built and fully linked, one at a time, from this
  source. The optional Vulkan backend also passes its current Vulkan mailbox full-module check. It was not used for
  the Deck measurements and was not device-played.
- [Linux gcc and clang CI](https://github.com/ravioli-gh/transformers-armada-recomp/actions/runs/43141382731) passes on
  this source.
- On the Deck, a live 45 → 240 → 45 change under the active 90 Hz governor was applied and logged without a restart.
  At 240 the host fell behind and the log records 71, then 77, frames a second, so this is a setting-handover check,
  not a claim that this scene sustains 240.
- A separate `TF_FPS_LOCK=60` run applied 60 → 45 → 60. Its log records `[tf] 45 frames a second, 45 pictures`, then
  `[tf] 60 frames a second, 60 pictures`; two-second pacing windows at the 60 and 45 steps read 60.0 and 45.0 new
  pictures a second.
- Both live cap handovers completed in the running process without a restart; the logs record every applied setting.
- The 45 option, below-60 picture rules, window-size correction and CPU picture hand-over are Linux-only. Windows
  keeps upstream's vector/newest-picture hand-over and movie-band geometry.
  [Automatic Windows compile CI](https://github.com/ravioli-gh/transformers-armada-recomp/actions/runs/43147820608)
  passed on the integration PR; Windows runtime/hardware testing was not done.

### Surface checks

Both renderer paths were actually run, not inferred from builds. The native build ran the route with a warm shader
cache and again for the 60-limit profile; the emulated-only build ran it too. Each produced an inspected 1280x801
loaded screenshot: native pre-profile and warm-start pictures, and the emulated-only loaded-start picture. The native
runs use Direct3D 12 through vkd3d on RADV; the emulated-only run uses the GS path through zink. The emulated-only run
verifies that renderer path and the route; it is not used for a native-pacing or performance claim.

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

## Current 0.6.6 60-limit profile

One native run was profiled at the 60-picture setting. `TF_FPS_CAP=60` was read from the owned process's
`/proc/<pid>/environ`; the post-HQ applied line `[tf] 60 frames a second` arrived at 131.03 s. The three 2-second
windows before attach averaged 59.97 new pictures a second (59.0, 60.9, 60.0). These are game-window telemetry, not
physical panel presents.

`perf record -p <pid> -F999 -g --call-graph dwarf,8192` ran only on the owned game PID. Its enabled interval was
20.0001 s, from enable acknowledgment at 150.1 s to the disable request at 170.1 s. The raw `perf.data` is 239,021,548
bytes, SHA-256 `9c436ab174005e3a6e69c6e5f2942d8f514d4cd4eba056e9bfb9c2cf322f48b6`; it is preserved and its hash was
verified. The exact native ELF used for the run is preserved with it.

### Capture method and quality

The Deck's perf 7.1.4, used alone, decoded no native-module frames from this ELF. That is a tool-decoding failure, not
evidence that the native renderer did no work. The ELF contains CFI, but the decoder mis-reported the ET_EXEC module
as duplicated. The capture was therefore analysed offline with perf 7.3.3 plus an analysis-only ET_EXEC module-lookup
repair (`4c69650822aa7e1e38bffa64cb43ae80be3884f502d8456165c26684765ddeef`), which returns the first DWFL module for a
repeated report. It never touched the game: the game files, raw capture, captured registers, stack and ELF are
unchanged, and the analysis shim was never loaded into the game process. A lifetime check ran 32 DWFL sessions; 31
module addresses were reused across sessions, with a purge between sessions. The fix still resolved the real addresses
rather than a stale module.

The accepted analysis covers 28,336 samples and 25,519 recovered callchains. 24,034 chains (94.2%) contain at least one
address in the native module; 2,817 samples (9.9%) have no chain, and there were 0 unparsed lines. The printed output
contains 389,928 callchain frames including inline expansions. Those are printed frames, not physical frames or
cycles, and inline frames can repeat the same real program counter; do not sum them or add them to the copy sample
counts below. The denominators here are samples and recovered chains only. CPU and APU power samples from this
profiled run are context only, not an unprofiled performance baseline.

### Sound mixer share

In the same capture, `perf report --stdio --no-children -g none --percent-limit 0.5` puts
`native_sound::mixer::mixFrames` at 7.25% self overhead. This is the report's weighted event-count share across
28,336 `cpu/cycles/Pu` samples (event count approximately 85,076,798,755), not a count of inline frames.
The older pre-0.6 bit-identical mixer optimization remains in the tree; this profile does not measure a saving from it.

### Copy hot spot and ownership

The `std::vector<…>::__insert_with_size` family is the leaf in 906 of 28,336 samples (3.2%).
Of these, 903 are byte-vector inserts:

| Logical caller | Thread / TID | Byte-vector leaf samples |
|---|---|---:|
| `PS2Memory::writeIORegister` lambda | `GameThread` / 151874 | 537 |
| `PS2Memory::writeIORegister` directly | `GameThread` / 151874 | 57 |
| `GSThreadedBackend::UploadImage` | `GsFeed` / 151880 | 309 |

The other 3 samples are 2 `EeScheduler::ScheduledEvent` inserts and 1 `unsigned int` microcode-prefix insert.
These are leaf sample counts, not total copy/memcpy work, inclusive frames, cycles or removable CPU savings.

A source-ownership review found the two byte copies necessary, not duplication. The EE copy keeps a guest-memory
snapshot that an asynchronous thread reads after the game thread continues; the GS `UploadImage` copy is the shadow
retained after its batch is recycled, for replay and readback. The current PS2 copy code is unchanged from that
review; the profile produced no production change.

## Current 0.6.6 40-minute loaded soak

The same native executable completed 2,400.6 loaded seconds at the 45 cap, without perf sampling.
All 81 compositor focus checks matched the owned game. The five inspected screenshots, at loaded start and
10/20/30/40 minutes, show real 1280x801 gameplay. The route ends about 188 seconds after launch; most of this
run therefore observes a static scene, not level traversal.

| Metric | Observed result |
|---|---|
| New-picture rate, time weighted | 45.00/s across 1,193 windows; window means 44.30–45.50/s |
| Repeated / never-shown pictures | 0 / 0 |
| Worst logged window's longest 1% interval | 62.15 ms; lowest reported 1% low 16.10/s |
| Process CPU, time weighted | 124.26% of one core |
| APU sensor, sparse point readings | Mean 7.08 W; range 0–9.87 W |
| Device GPU busy, sparse point readings | Mean 72.50%; range 0–100% |
| Native readback skips | 0 across 360 renderer-profile log lines |

The rate is not perfectly uniform; its mean does not establish smooth physical-panel cadence.
There are 82 loaded resource readings, normally 30 seconds apart. The sensor means include all reported zeros:
two APU readings and twelve GPU-busy readings. These are sparse sensor observations, not a calibrated energy
measurement or continuous utilization average. No battery-life or power-saving comparison is inferred.

| Memory account | Loaded start | End | Total change | Late change | Late slope |
|---|---:|---:|---:|---:|---:|
| Process RSS | 680.5 MiB | 700.9 MiB | +20.4 MiB | +0.8 MiB | +0.028 MiB/min |
| Device VRAM | 493.8 MiB | 489.4 MiB | −4.4 MiB | −4.4 MiB | −0.167 MiB/min |

The late window excludes the first three loaded minutes; its page-fault rate was 5.5/min.
These small late changes do not rule out slower leaks or growth during repeated level loads.
The installed executable/settings hashes and both Steam launch-option fields were restored after every run;
the shortcut executable/start directory remained unchanged and no owned game process remained.

## Historical comparison: upstream 0.4.6 era

**Not remeasured on 0.6.6.** The port rows below are from `a4de169`, before the later Linux sizing, pacing, virtual-heap
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
including 73 Amazon textures. These rows establish the historical HD trade-off only; the current source's HD
cold-load peak, HD frame rate and HD shader-start time have not been remeasured here.
