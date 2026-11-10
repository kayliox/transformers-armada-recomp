# Steam Deck performance: the Linux build and PCSX2

Measurements of the native Linux build on a Steam Deck, against PCSX2 on the same device and in the same place of the
game. For the Linux port's pull request; the method is written out so that the numbers can be measured again.

## Setup

| | |
|---|---|
| Device | Steam Deck OLED, Gaming Mode, 1280x800 panel at 90 Hz, Steam's frame limit at 90 |
| System | SteamOS 3.9.5 (build 20261023.100), kernel 7.2.7-valve2, Mesa RADV (VANGOGH) |
| The port | `linux-port` at a4de169 (upstream 0.4.6), the Deck bundle (glibc 2.35 build), native renderer through vkd3d 2.1 |
| PCSX2 | 2.8.2, official AppImage, Vulkan renderer, default speed settings (no speed hacks changed), fast boot |
| Disc | Transformers (USA), SLUS-20668, the same image for both |
| BIOS (PCSX2) | the owner's own dump |

The port's settings were the player's own `settings.ini`: frame rate cap Unlimited (`TF_FPS_CAP=0`), draw distance 3,
quality High, anti-aliasing 2, field of view 1.2, the FPS counter on. Its picture is 16:9 at the panel's width
(1280x720 inside the 1280x800 screen).

Texture packs: PCSX2 ran without any texture replacement pack. The port ran without the HD texture pack in the main
table below (its `hd_textures/` held only the sharp fonts and the Deck's button labels; HD textures, `TF_HIRES`, had
nothing to load). The HD texture pack's own measurement is in its section below.

## Method

- Place: Amazon Basin, the start of the level (Optimus on the jungle path), New Game on Recruit.
- PCSX2: reached by playing through the menus once (a virtual keyboard bound to the pad), then saved in a PCSX2
  savestate and started from it for each resolution. While measuring, the robot walked forward, turned left and right
  and fired, in a loop.
- The port: its scripted test route (`TF_INPUT`, the same menus, films skipped with `TF_SKIP_FMV=1`, an empty test
  memory card through `TF_MC_ROOT`), which turns the view and fires in the level from about 150 s after the start;
  measured from 150 to 190 s.
- Each configuration: three samples of about 10 s each. CPU is the whole process (`top`, 100% = one core); GPU busy is
  the average of 40 readings of `gpu_busy_percent`; power is the average of 10 readings of the APU's `power1_average`
  (hwmon); memory is the process's RSS. The frame rate is the on-screen one: PCSX2's OSD, the port's own counter.
- Both ran through the same Steam shortcut, so that gamescope gave each the focus and the same frame limiter.

## Results

CPU, GPU busy, power and RSS are averages of the three samples. Frames a second and PCSX2 speed are single readings,
from one screenshot taken during the samples (PCSX2's OSD, the port's counter).

| Configuration | Frames a second (one reading) | PCSX2 speed (one reading) | Render resolution | CPU % | GPU busy % | Power W | RSS MB |
|---|---|---|---|---|---|---|---|
| PCSX2 1x | 29.98 | 100% | 512x448 | 149.6 | 24.3 | 6.1 | 834 |
| PCSX2 2x | 29.97 | 100% | 1024x894 | 148.3 | 65.3 | 7.9 | 696 |
| PCSX2 3x | 27.52 | 92% | 1536x1341 | 144.7 | 82.0 | 9.1 | 689 |
| PCSX2 4x | 30.64 | 100-102% | 2048x1792 | 141.9 | 87.7 | 12.0 | 708 |
| Port, native renderer, uncapped | 83 | - | 1280x720 | 248.5 | 91.3 | 15.8 | 762 |
| Port, native renderer, capped at 30 | 30 | - | 1280x720 | 117.7 | 57.3 | 7.1 | 756 |
| Port, emulated renderer, uncapped | 45 | - | 512x448, scaled | 271.2 | 76.0 | 9.8 | 565 |

PCSX2's OSD at the screenshot of each scale (EE / GS / GPU load): 1x 71 / 74 / 3 %, 2x 71 / 79 / 54 %, 3x 61 / 82 / 76 %,
4x 62 / 75 / 92 %.

Elsewhere on the route (one reading each, PCSX2 1x): the HQ ran at 96% speed, the Warp Gate's loading screen at
39-49% and the opening film's space scene at 54%. The port loads Amazon Basin in about 2 seconds.

## Reading the numbers

The uncapped port and PCSX2 at 1x do very different amounts of work: 83 frames of 1280x720 (about 76 million pixels a
second, at the player's High settings) against 30 frames of 512x448 (about 7 million). Like for like - 30 frames a
second at nearly the same number of pixels (PCSX2 2x: 1024x894 = 0.92 million, the port: 1280x720 = 0.92 million):

| 30 frames a second | Pixels a frame | CPU % | GPU busy % | Power W |
|---|---|---|---|---|
| PCSX2 2x | 0.92 M | 148.3 | 65.3 | 7.9 |
| Port, native renderer, capped at 30 | 0.92 M | 117.7 | 57.3 | 7.1 |
| PCSX2 1x (for reference) | 0.23 M | 149.6 | 24.3 | 6.1 |

- The game itself runs at 30 frames a second on the PS2. PCSX2 keeps it there at 1x and 2x; from 3x it is held back by
  the Deck's GPU (busy 82-88%), not its CPU (EE 60-70%). The 3x and 4x frame rates differ by the view at the moment of
  the screenshot (the 4x one has a large tree trunk in front); GPU busy and PCSX2's GPU time put 4x above 3x.
- At the same 30 frames a second the port uses about a fifth less CPU than PCSX2 at any scale (it runs the game's code
  instead of emulating the PS2's processors), and at the same pixel count a little less GPU and power, with its higher
  draw distance and anti-aliasing.
- Uncapped, the port draws 83 frames a second with the player's High settings, using about 2.5 cores and nearly all of
  the GPU; it is the GPU that stops it short of the panel's 90.
- The emulated renderer (the PS2's GS emulated, through Zink) is about half the native renderer's frame rate with more
  CPU, less power and less memory.

## The port with its HD texture pack

The pack was made on the Deck itself with the port's own tools (Real-ESRGAN on the Deck's GPU, then the colour match),
for the textures Amazon Basin uses: its level textures and the shared ones (characters, pick-ups, interface, effects,
sky). 1089 images, 2.8 GB, made in 2 h 14 min. PCSX2 has no such pack for this game.

Same route and place as above, the native renderer, the player's settings; HD textures on (the default) and off
(`TF_HIRES=0`, the same pack on disk). Here the frame rate was read from a screenshot at each of the three samples. The
first HD run (not measured) filled the renderer's BC7 cache of the pack: 5 files (28 MB) before, 182 (644 MB) after.

| Configuration | Frames a second (3 readings) | CPU % | GPU busy % | Power W | RSS MB | VRAM MB |
|---|---|---|---|---|---|---|
| HD textures on | 59 / 72 / 72 (68) | 233.6 | 97.3 | 15.0 | 1054 | 1015 |
| HD textures on, again | 63 / 75 / 75 (71) | 242.3 | 92.7 | 15.2 | 1010 | 1015 |
| HD textures off | 76 / 83 / 83 (81) | 243.7 | 91.7 | 15.6 | 782 | 638 |

- The pack costs about 13 frames a second here (12-16%), 230-270 MB of memory and 377 MB of video memory. The GPU is
  the limit with it (busy 93-97%); power stays at the same 15 W, the APU's level for this scene.
- HD off reproduces the run without any pack (83 frames a second, 248% CPU, 91% GPU, 15.8 W, 762 MB).
- That the textures were used, not only found: with `TF_NATIVE_NAMES=1` (in a separate, unmeasured run) the renderer
  chose a large image for 180-186 different textures in the first 190 s, 73 of them Amazon Basin's own (for example
  `MACHU_STONE_WALL`, `MACHU_GROUND`, `HILL_TILE`), each matching the game's texture (difference 0.00); it rejected one
  (a 16x16 grey texture whose alpha differed). With `TF_HIRES=0` the log says "HD textures off (TF_HIRES=0): the game's own textures".

## Caveats

- One place in one level; three samples per configuration. PCSX2's samples and the port's are the same place and the
  same kind of view, not the same frames. The frame rate moves within a run (57-90 frames a second in one run's own
  log), more than three readings can show.
- During the port's capped run the Steam library came to the front over the game for part of the samples; its numbers
  match an earlier capped measurement in the same place (119% CPU, 57% GPU).
- Power readings move a lot from second to second; read them as a guide.

## Raw samples

CPU % / GPU busy % / power W / RSS MB, three samples per configuration.

| Configuration | Sample 1 | Sample 2 | Sample 3 |
|---|---|---|---|
| PCSX2 1x | 150.6 / 15 / 6.3 / 834 | 148.5 / 22 / 6.7 / 834 | 149.6 / 36 / 5.4 / 834 |
| PCSX2 2x | 148.8 / 70 / 8.2 / 692 | 147.1 / 77 / 8.9 / 692 | 149.0 / 49 / 6.7 / 703 |
| PCSX2 3x | 148.0 / 89 / 8.6 / 689 | 143.3 / 69 / 9.5 / 689 | 142.7 / 88 / 9.1 / 689 |
| PCSX2 4x | 143.8 / 81 / 11.9 / 706 | 138.5 / 93 / 12.8 / 709 | 143.4 / 89 / 11.3 / 709 |
| Port, native, uncapped | 247.5 / 88 / 16.3 / 761 | 249.5 / 94 / 15.6 / 763 | 248.5 / 92 / 15.6 / 763 |
| Port, native, cap 30 | 117.1 / 58 / 7.3 / 754 | 118.2 / 59 / 7.0 / 756 | 117.9 / 55 / 6.9 / 757 |
| Port, emulated, uncapped | 306.2 / 73 / 11.0 / 560 | 263.2 / 68 / 9.2 / 567 | 244.1 / 87 / 9.2 / 567 |
