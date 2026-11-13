# Frame rate: the patched sites

*The site-by-site check behind the 69 instruction patches for the frame rate in
`transformers/config/overrides.toml` (P2 of [FRAME_RATE.md](FRAME_RATE.md), section 7, items
10-14), as the analysis agent wrote it on 2026-10-04. Since then: all 67 sites below are in
`overrides.toml`, the words are written by `setFrameWords` in `transformers/src/overrides.cpp`
(the first five from float literals as before, the rest copied from the game's constants), and
Starscream's cooldown (item 3 of "Not patched") is patched as well, to reload by 0.1 + (1/30 s -
a frame) from the spare word `0x6CD2FC` - its remainder carries over from cycle to cycle, so on
average a cycle lasts as at 30. Its helper scripts are not kept.*

*2026-10-04. Static analysis only (nothing built or run). Original words are read from
`game/SLUS_206.68`; the generated listing (`work/generated/*.cpp`) gives the same word at every
site below (none is patched yet). For every site the loaded register was followed over the
function's control flow (both sides of every branch, delay slots, calls) until it is
overwritten: helper scripts in the agent's scratch folder.*

## Summary

- **67 patchable sites**, each a single `lwc1 $fN, off($gp)` whose register feeds only the
  per-frame operation: **33 share**, **9 damp**, **20 increment**, **5 "grow"** (the physics
  smoothing's add term, which must stay paired with its damp word). By item: 10: 21, 11: 8,
  12: 8, 13: 14, 14: 16. All are gp-relative loads; no inline-built (`lui`/`mtc1`) constant
  needs patching.
- **Words**: 18 of the 19 free words (`0x6CD2B4`..`0x6CD2F8`, `0x6CD2FC` left spare) plus the
  two existing share words, reused for the same value and kind: `0x6CD2A0` = share(0.2) and
  `0x6CD2A4` = share(0.3), which `setBlendShares` already writes every frame.
- **Not patched after checking** (wrong or unnecessary): 14 entries. Five of the survey's
  "per call" amounts run per event, not per frame: Starscream's `+= 0.1` reloads a timer kept
  in seconds, the twist rocket's x0.75 runs per launch, the patrol shield arm's x0.75 per
  patrol decision, the Autobot health component's -0.1 per heartbeat, and the melee area
  damage x0.5 per hit. Two are search loops inside one call: the 0.01 steps of `0x423BA0`,
  `0x489E50` and `0x498280`, and the x0.8 of `0x438EB0`. The rest are a load-time routine
  (`0x4316C0`), values recomputed every call (`0x2F0C00`), front-end menus (`0x335E00`) and
  a spatial filter (`0x247020`). Three more need care: the 0.1 at `0x48000C` in the flight
  code is a mapping, not a blend; the 0.3 weight blends in TransformToVehicle and
  TransformToRobot are only partly per-frame; Doppler is already handled. Details are in
  the "Not patched" section.
- **Unsure / medium**: the 8 physics-smoothing sites (the arithmetic is certain; what the
  smoothed pose decides was followed only far enough to see it pins resting bodies) and the
  ambience fade `0x37FEF4`, whose register is also the fade's "done" tolerance.
  **Special:** the shader drift `0x26AC14` loads a *variable* (`0x6CDA44`, written by
  `0x360600`), not a constant. Its word must be computed every frame from the live value.
- **Integer frame counters** (need another treatment): the HUD 4-frame cycle (`gp-0x3B64`),
  the rumble history ring of 300 samples and its 90-sample minimum, and the wind generator's
  gust interval and loop-sound retry count. Two are rate-neutral: the creature effect
  switcher's hold counter, and `taGame+0xA0` as used by the drift shader. The per-frame
  LCG steps of `0x533010` also belong here.

### The words

Formulas, with k = 30/R and R the level's rate. At 30 (`g_levelRate == 0`) every word must
hold the game's own bits. The simplest way is to compute each word from the game's constant
word in memory, listed as "from"; none of them is ever written, except `0x6CDA44`.

| Word | gp offset | Value, kind | Formula | At 30 (bits) | From | At 60 | At 120 | At 144 |
|---|---|---|---|---|---|---|---|---|
| `0x6CD2A0` | `-0x5E50` | 0.2 share (existing `kBlendShare02`; read by PickFinalSpeed 0x476320's walk/run band at `0x4763CC` / `0x476430`, patched in config/overrides.toml since the first 60 fps build, and by human_mode.cpp's own copy of it) | 1-(1-s)^k | `3E4CCCCD` | (as now) | 0.105573 | 0.0542584 | 0.0454242 |
| `0x6CD2A4` | `-0x5E4C` | 0.3 share (existing `kBlendShare03`) | 1-(1-s)^k | `3E99999A` | (as now) | 0.16334 | 0.0853088 | 0.0716136 |
| `0x6CD2B4` | `-0x5E3C` | 0.1 share | 1-(1-s)^k | `3DCCCCCD` | `0x6CBC64` | 0.0513167 | 0.0259963 | 0.021711 |
| `0x6CD2B8` | `-0x5E38` | 0.05 share | 1-(1-s)^k | `3D4CCCCD` | `0x6CBD88` | 0.0253206 | 0.0127415 | 0.0106292 |
| `0x6CD2BC` | `-0x5E34` | 0.06 share | 1-(1-s)^k | `3D75C28F` | `0x6CBE18` | 0.030464 | 0.0153498 | 0.012808 |
| `0x6CD2C0` | `-0x5E30` | 0.1 grow (physics c) | 1/d' - 1, d' = word `0x6CD2C4` | `3DCCCCCD` | `0x6CB3F0` | 0.0488089 | 0.0241137 | 0.0200547 |
| `0x6CD2C4` | `-0x5E2C` | 0.9090909 damp (physics d) | d^k | `3F68BA2E` | `0x6CBADC` | 0.953463 | 0.976454 | 0.98034 |
| `0x6CD2C8` | `-0x5E28` | 0.9 damp | f^k | `3F666666` | `0x6CBA4C` | 0.948683 | 0.974004 | 0.978289 |
| `0x6CD2CC` | `-0x5E24` | 0.995 damp | f^k | `3F7EB852` | `0x6CCE1C` | 0.997497 | 0.998748 | 0.998956 |
| `0x6CD2D0` | `-0x5E20` | 0.97 damp | f^k | `3F7851EC` | `0x6CE194` | 0.984886 | 0.992414 | 0.993674 |
| `0x6CD2D4` | `-0x5E1C` | 0.1 increment | c*k | `3DCCCCCD` | `0x6CCCA4` | 0.05 | 0.025 | 0.0208333 |
| `0x6CD2D8` | `-0x5E18` | 0.01 increment | c*k | `3C23D70A` | `0x6CE064` | 0.005 | 0.0025 | 0.00208333 |
| `0x6CD2DC` | `-0x5E14` | 0.0872665 (5 deg) increment | c*k | `3DB2B8C2` | `0x6CCDAC` | 0.0436332 | 0.0218166 | 0.0181805 |
| `0x6CD2E0` | `-0x5E10` | drift speed increment (0.07 in the ELF) | **live** [`0x6CDA44`]*k | `3D8F5C29`* | `0x6CDA44` (variable) | 0.035* | 0.0175* | 0.0145833* |
| `0x6CD2E4` | `-0x5E0C` | 0.2 increment | c*k | `3E4CCCCD` | `0x6CE198` | 0.1 | 0.05 | 0.0416667 |
| `0x6CD2E8` | `-0x5E08` | 0.45 increment | c*k | `3EE66666` | `0x6CB2A4` | 0.225 | 0.1125 | 0.09375 |
| `0x6CD2EC` | `-0x5E04` | 0.007 increment | c*k | `3BE56042` | `0x6CB468` | 0.0035 | 0.00175 | 0.00145833 |
| `0x6CD2F0` | `-0x5E00` | 0.0003 increment | c*k | `399D4952` | `0x6CBF04` | 0.00015 | 7.5e-05 | 6.25e-05 |
| `0x6CD2F4` | `-0x5DFC` | 0.0005 increment | c*k | `3A03126F` | `0x6CBE10` | 0.00025 | 0.000125 | 0.000104167 |
| `0x6CD2F8` | `-0x5DF8` | 0.005 increment | c*k | `3BA3D70A` | `0x6CBD5C` | 0.0025 | 0.00125 | 0.00104167 |
| `0x6CD2FC` | `-0x5DF4` | spare | | | | | | |

\* With the ELF default 0.07; areas set their own value.

Do not write the 30 values as float literals. `0.9090909f`, for example, rounds to
`3F68BA2D`, not the game's `3F68BA2E`. Copy them from the "from" words, or use the bits. The
physics pair must be written together, so that (s + c'p)d' = d's + (1-d')p: c' = 1/d' - 1.

### The sites

"Runs": how often the operation runs; "frame" means once a frame per object.
Conf.: **high** means the operation, the register's uses and the call chain were all
checked; **medium** is explained in the item's details; **high\*** means patchable, but the
word must be computed from the live variable.

| # | Item | Function [context] | Site | Original word | New word | Free word | Value at 30 (const @) | Kind | Runs | Conf. |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 10 | 0x471620 Fall(+Base)+0x8 | `0x471650` | `C7808B74` `lwc1 $f0, -0x748C($gp)` | `C780A1C4` `lwc1 $f0, -0x5E3C($gp)` | `0x6CD2B4` | 0.1 `3DCCCCCD` (`0x6CBC64`) | share | frame | high |
| 2 | 10 | 0x471620 Fall(+Base)+0x8 | `0x4716C4` | `C7818558` `lwc1 $f1, -0x7AA8($gp)` | `C781A1B4` `lwc1 $f1, -0x5E4C($gp)` | `0x6CD2A4` | 0.3 `3E99999A` (`0x6CB648`) | share | frame | high |
| 3 | 10 | 0x47A440 TransformToVehicle+0x8 | `0x47A484` | `C7808B74` `lwc1 $f0, -0x748C($gp)` | `C780A1C4` `lwc1 $f0, -0x5E3C($gp)` | `0x6CD2B4` | 0.1 `3DCCCCCD` (`0x6CBC64`) | share | frame | high |
| 4 | 10 | 0x47A440 TransformToVehicle+0x8 | `0x47A5D8` | `C78C8B74` `lwc1 $f12, -0x748C($gp)` | `C78CA1C4` `lwc1 $f12, -0x5E3C($gp)` | `0x6CD2B4` | 0.1 `3DCCCCCD` (`0x6CBC64`) | share | frame | high |
| 5 | 10 | 0x47A440 TransformToVehicle+0x8 | `0x47A608` | `C78C8B74` `lwc1 $f12, -0x748C($gp)` | `C78CA1C4` `lwc1 $f12, -0x5E3C($gp)` | `0x6CD2B4` | 0.1 `3DCCCCCD` (`0x6CBC64`) | share | frame | high |
| 6 | 10 | 0x47A440 TransformToVehicle+0x8 | `0x47A6D4` | `C7808B74` `lwc1 $f0, -0x748C($gp)` | `C780A1C4` `lwc1 $f0, -0x5E3C($gp)` | `0x6CD2B4` | 0.1 `3DCCCCCD` (`0x6CBC64`) | share | frame | high |
| 7 | 10 | 0x47B010 TransformToRobot+0x8 | `0x47B054` | `C7808B74` `lwc1 $f0, -0x748C($gp)` | `C780A1C4` `lwc1 $f0, -0x5E3C($gp)` | `0x6CD2B4` | 0.1 `3DCCCCCD` (`0x6CBC64`) | share | frame | high |
| 8 | 10 | 0x47B010 TransformToRobot+0x8 | `0x47B110` | `C7808B74` `lwc1 $f0, -0x748C($gp)` | `C780A1C4` `lwc1 $f0, -0x5E3C($gp)` | `0x6CD2B4` | 0.1 `3DCCCCCD` (`0x6CBC64`) | share | frame | high |
| 9 | 10 | 0x47EA10 GlideFall+0x8 | `0x47EA2C` | `C7808B74` `lwc1 $f0, -0x748C($gp)` | `C780A1C4` `lwc1 $f0, -0x5E3C($gp)` | `0x6CD2B4` | 0.1 `3DCCCCCD` (`0x6CBC64`) | share | frame | high |
| 10 | 10 | 0x47F0D0 PoweredGlide+0x8 | `0x47F11C` | `C7808B74` `lwc1 $f0, -0x748C($gp)` | `C780A1C4` `lwc1 $f0, -0x5E3C($gp)` | `0x6CD2B4` | 0.1 `3DCCCCCD` (`0x6CBC64`) | share | frame | high |
| 11 | 10 | 0x47F0D0 PoweredGlide+0x8 | `0x47F194` | `C7808B74` `lwc1 $f0, -0x748C($gp)` | `C780A1C4` `lwc1 $f0, -0x5E3C($gp)` | `0x6CD2B4` | 0.1 `3DCCCCCD` (`0x6CBC64`) | share | frame | high |
| 12 | 10 | 0x47F0D0 PoweredGlide+0x8 | `0x47F208` | `C7808B74` `lwc1 $f0, -0x748C($gp)` | `C780A1C4` `lwc1 $f0, -0x5E3C($gp)` | `0x6CD2B4` | 0.1 `3DCCCCCD` (`0x6CBC64`) | share | frame | high |
| 13 | 10 | 0x47FF90 <- Flight 0x480830 | `0x480314` | `C7818B74` `lwc1 $f1, -0x748C($gp)` | `C781A1C4` `lwc1 $f1, -0x5E3C($gp)` | `0x6CD2B4` | 0.1 `3DCCCCCD` (`0x6CBC64`) | share | N=state+0xD0 calls a frame (substeps of dt/N) | high |
| 14 | 10 | 0x469510 MotionShaderGlidePack+0x10 | `0x469724` | `C7808A6C` `lwc1 $f0, -0x7594($gp)` | `C780A1C4` `lwc1 $f0, -0x5E3C($gp)` | `0x6CD2B4` | 0.1 `3DCCCCCD` (`0x6CBB5C`) | share | frame | high |
| 15 | 10 | 0x469510 MotionShaderGlidePack+0x10 | `0x46974C` | `C7808A6C` `lwc1 $f0, -0x7594($gp)` | `C780A1C4` `lwc1 $f0, -0x5E3C($gp)` | `0x6CD2B4` | 0.1 `3DCCCCCD` (`0x6CBB5C`) | share | frame | high |
| 16 | 10 | 0x469510 MotionShaderGlidePack+0x10 | `0x4697C0` | `C7808A6C` `lwc1 $f0, -0x7594($gp)` | `C780A1C4` `lwc1 $f0, -0x5E3C($gp)` | `0x6CD2B4` | 0.1 `3DCCCCCD` (`0x6CBB5C`) | share | frame | high |
| 17 | 10 | 0x46C6F0 MotionShaderFootslip+0xC | `0x46C740` | `C7808A6C` `lwc1 $f0, -0x7594($gp)` | `C780A1C4` `lwc1 $f0, -0x5E3C($gp)` | `0x6CD2B4` | 0.1 `3DCCCCCD` (`0x6CBB5C`) | share | frame | high |
| 18 | 10 | 0x46CA80 MotionShaderFootslip+0x8 | `0x46CAD8` | `C7808A6C` `lwc1 $f0, -0x7594($gp)` | `C780A1C4` `lwc1 $f0, -0x5E3C($gp)` | `0x6CD2B4` | 0.1 `3DCCCCCD` (`0x6CBB5C`) | share | frame | high |
| 19 | 10 | 0x46CA80 MotionShaderFootslip+0x8 | `0x46CB0C` | `C7818A6C` `lwc1 $f1, -0x7594($gp)` | `C781A1C4` `lwc1 $f1, -0x5E3C($gp)` | `0x6CD2B4` | 0.1 `3DCCCCCD` (`0x6CBB5C`) | share | frame | high |
| 20 | 10 | 0x46CA80 MotionShaderFootslip+0x8 | `0x46CB30` | `C7808A6C` `lwc1 $f0, -0x7594($gp)` | `C780A1C4` `lwc1 $f0, -0x5E3C($gp)` | `0x6CD2B4` | 0.1 `3DCCCCCD` (`0x6CBB5C`) | share | frame | high |
| 21 | 10 | 0x46CA80 MotionShaderFootslip+0x8 | `0x46CB48` | `C7808A6C` `lwc1 $f0, -0x7594($gp)` | `C780A1C4` `lwc1 $f0, -0x5E3C($gp)` | `0x6CD2B4` | 0.1 `3DCCCCCD` (`0x6CBB5C`) | share | frame | high |
| 22 | 11 | 0x2D0B50 ThirdPersonCameraScriptThree+0xC | `0x2D1180` | `C7808558` `lwc1 $f0, -0x7AA8($gp)` | `C780A1B4` `lwc1 $f0, -0x5E4C($gp)` | `0x6CD2A4` | 0.3 `3E99999A` (`0x6CB648`) | share | frame | high |
| 23 | 11 | 0x2CD670 FirstPersonCameraScript+0xC | `0x2CD84C` | `C7808558` `lwc1 $f0, -0x7AA8($gp)` | `C780A1B4` `lwc1 $f0, -0x5E4C($gp)` | `0x6CD2A4` | 0.3 `3E99999A` (`0x6CB648`) | share | frame | high |
| 24 | 11 | 0x2CBBB0 WarpOutSequenceScript+0x10 | `0x2CBE14` | `C7808558` `lwc1 $f0, -0x7AA8($gp)` | `C780A1B4` `lwc1 $f0, -0x5E4C($gp)` | `0x6CD2A4` | 0.3 `3E99999A` (`0x6CB648`) | share | frame | high |
| 25 | 11 | 0x2CBBB0 WarpOutSequenceScript+0x10 | `0x2CBD20` | `C78C8DA0` `lwc1 $f12, -0x7260($gp)` | `C78CA1C4` `lwc1 $f12, -0x5E3C($gp)` | `0x6CD2B4` | 0.1 `3DCCCCCD` (`0x6CBE90`) | share | frame | high |
| 26 | 11 | 0x2CE0A0 GlideCameraScript+0xC | `0x2CE5A4` | `C7808C78` `lwc1 $f0, -0x7388($gp)` | `C780A1B0` `lwc1 $f0, -0x5E50($gp)` | `0x6CD2A0` | 0.2 `3E4CCCCD` (`0x6CBD68`) | share | frame | high |
| 27 | 11 | 0x2CD9D0 JetPackCameraScript+0xC | `0x2CDDC4` | `C7808C98` `lwc1 $f0, -0x7368($gp)` | `C780A1C8` `lwc1 $f0, -0x5E38($gp)` | `0x6CD2B8` | 0.05 `3D4CCCCD` (`0x6CBD88`) | share | frame | high |
| 28 | 11 | 0x2CD9D0 JetPackCameraScript+0xC | `0x2CDDE4` | `C7808C98` `lwc1 $f0, -0x7368($gp)` | `C780A1C8` `lwc1 $f0, -0x5E38($gp)` | `0x6CD2B8` | 0.05 `3D4CCCCD` (`0x6CBD88`) | share | frame | high |
| 29 | 11 | 0x2CA0B0 <- 0x2CA9D0 <- taCamera+0x34 | `0x2CA194` | `C7808D28` `lwc1 $f0, -0x72D8($gp)` | `C780A1CC` `lwc1 $f0, -0x5E34($gp)` | `0x6CD2BC` | 0.06 `3D75C28F` (`0x6CBE18`) | share | frame | high |
| 30 | 12 | 0x1B3AE0 <- physics step 0x1B5E50 | `0x1B3BA4` | `C7828300` `lwc1 $f2, -0x7D00($gp)` | `C782A1D0` `lwc1 $f2, -0x5E30($gp)` | `0x6CD2C0` | 0.1 `3DCCCCCD` (`0x6CB3F0`) | grow | frame (per body) | medium |
| 31 | 12 | 0x1B3AE0 <- physics step 0x1B5E50 | `0x1B3BC0` | `C7828300` `lwc1 $f2, -0x7D00($gp)` | `C782A1D0` `lwc1 $f2, -0x5E30($gp)` | `0x6CD2C0` | 0.1 `3DCCCCCD` (`0x6CB3F0`) | grow | frame (per body) | medium |
| 32 | 12 | 0x1B3AE0 <- physics step 0x1B5E50 | `0x1B3BD8` | `C7828300` `lwc1 $f2, -0x7D00($gp)` | `C782A1D0` `lwc1 $f2, -0x5E30($gp)` | `0x6CD2C0` | 0.1 `3DCCCCCD` (`0x6CB3F0`) | grow | frame (per body) | medium |
| 33 | 12 | 0x1B3AE0 <- physics step 0x1B5E50 | `0x1B3BF4` | `C7828300` `lwc1 $f2, -0x7D00($gp)` | `C782A1D0` `lwc1 $f2, -0x5E30($gp)` | `0x6CD2C0` | 0.1 `3DCCCCCD` (`0x6CB3F0`) | grow | frame (per body) | medium |
| 34 | 12 | 0x1B3AE0 <- physics step 0x1B5E50 | `0x1B3E20` | `C7838300` `lwc1 $f3, -0x7D00($gp)` | `C783A1D0` `lwc1 $f3, -0x5E30($gp)` | `0x6CD2C0` | 0.1 `3DCCCCCD` (`0x6CB3F0`) | grow | frame (per body) | medium |
| 35 | 12 | 0x1B3AE0 <- physics step 0x1B5E50 | `0x1B3E64` | `C78089EC` `lwc1 $f0, -0x7614($gp)` | `C780A1D4` `lwc1 $f0, -0x5E2C($gp)` | `0x6CD2C4` | 0.9090909 `3F68BA2E` (`0x6CBADC`) | damp | frame (per body) | medium |
| 36 | 12 | 0x1B3AE0 <- physics step 0x1B5E50 | `0x1B3E74` | `C78089EC` `lwc1 $f0, -0x7614($gp)` | `C780A1D4` `lwc1 $f0, -0x5E2C($gp)` | `0x6CD2C4` | 0.9090909 `3F68BA2E` (`0x6CBADC`) | damp | frame (per body) | medium |
| 37 | 12 | 0x1B3AE0 <- physics step 0x1B5E50 | `0x1B3E84` | `C78089EC` `lwc1 $f0, -0x7614($gp)` | `C780A1D4` `lwc1 $f0, -0x5E2C($gp)` | `0x6CD2C4` | 0.9090909 `3F68BA2E` (`0x6CBADC`) | damp | frame (per body) | medium |
| 38 | 13 | 0x577E70 ActorHeavyGrunt+0x14 | `0x578D04` | `C7809BB4` `lwc1 $f0, -0x644C($gp)` | `C780A1E4` `lwc1 $f0, -0x5E1C($gp)` | `0x6CD2D4` | 0.1 `3DCCCCCD` (`0x6CCCA4`) | increment | frame x2 parts | high |
| 39 | 13 | 0x57D280 SearchCommand_Pivot+0xC | `0x57D39C` | `C781B2C8` `lwc1 $f1, -0x4D38($gp)` | `C781A1E4` `lwc1 $f1, -0x5E1C($gp)` | `0x6CD2D4` | 0.1 `3DCCCCCD` (`0x6CE3B8`) | increment | frame | high |
| 40 | 13 | 0x57D280 SearchCommand_Pivot+0xC | `0x57D43C` | `C781B2C8` `lwc1 $f1, -0x4D38($gp)` | `C781A1E4` `lwc1 $f1, -0x5E1C($gp)` | `0x6CD2D4` | 0.1 `3DCCCCCD` (`0x6CE3B8`) | increment | frame | high |
| 41 | 13 | 0x3CF750 WeaponLogic2_Vortex+0x10 | `0x3CF8AC` | `C780895C` `lwc1 $f0, -0x76A4($gp)` | `C780A1D8` `lwc1 $f0, -0x5E28($gp)` | `0x6CD2C8` | 0.9 `3F666666` (`0x6CBA4C`) | damp | frame (in contact) | high |
| 42 | 13 | 0x46F0D0 TransformerStateCar+0x8 | `0x46F920` | `C780AF74` `lwc1 $f0, -0x508C($gp)` | `C780A1E8` `lwc1 $f0, -0x5E18($gp)` | `0x6CD2D8` | 0.01 `3C23D70A` (`0x6CE064`) | increment | frame | high |
| 43 | 13 | 0x46F0D0 TransformerStateCar+0x8 | `0x46F950` | `C780AF74` `lwc1 $f0, -0x508C($gp)` | `C780A1E8` `lwc1 $f0, -0x5E18($gp)` | `0x6CD2D8` | 0.01 `3C23D70A` (`0x6CE064`) | increment | frame | high |
| 44 | 13 | 0x426280 <- 0x4254E0 <- ActorDropShip+0x14 | `0x426384` | `C7809D2C` `lwc1 $f0, -0x62D4($gp)` | `C780A1DC` `lwc1 $f0, -0x5E24($gp)` | `0x6CD2CC` | 0.995 `3F7EB852` (`0x6CCE1C`) | damp | frame | high |
| 45 | 13 | 0x426280 <- 0x4254E0 <- ActorDropShip+0x14 | `0x4263F8` | `C7809D2C` `lwc1 $f0, -0x62D4($gp)` | `C780A1DC` `lwc1 $f0, -0x5E24($gp)` | `0x6CD2CC` | 0.995 `3F7EB852` (`0x6CCE1C`) | damp | frame | high |
| 46 | 13 | 0x494F90 <- 0x495390 <- CreatureBossUnicron+0x3C | `0x495334` | `C7809A28` `lwc1 $f0, -0x65D8($gp)` | `C780A1D8` `lwc1 $f0, -0x5E28($gp)` | `0x6CD2C8` | 0.9 `3F666666` (`0x6CCB18`) | damp | frame | high |
| 47 | 13 | 0x495390 <- CreatureBossUnicron+0x3C | `0x4955E4` | `C7809A28` `lwc1 $f0, -0x65D8($gp)` | `C780A1D8` `lwc1 $f0, -0x5E28($gp)` | `0x6CD2C8` | 0.9 `3F666666` (`0x6CCB18`) | damp | frame | high |
| 48 | 13 | 0x483FC0 <- 0x484130 <- ActorBossMegatron+0x14 | `0x484030` | `C7809CBC` `lwc1 $f0, -0x6344($gp)` | `C780A1EC` `lwc1 $f0, -0x5E14($gp)` | `0x6CD2DC` | 0.0872665 `3DB2B8C2` (`0x6CCDAC`) | increment | frame | high |
| 49 | 13 | 0x483FC0 <- 0x484130 <- ActorBossMegatron+0x14 | `0x4840C8` | `C7819CBC` `lwc1 $f1, -0x6344($gp)` | `C781A1EC` `lwc1 $f1, -0x5E14($gp)` | `0x6CD2DC` | 0.0872665 `3DB2B8C2` (`0x6CCDAC`) | increment | frame | high |
| 50 | 13 | 0x52A440 <- SidekickStateFall+0x8 | `0x52A5EC` | `C7808C78` `lwc1 $f0, -0x7388($gp)` | `C780A1B0` `lwc1 $f0, -0x5E50($gp)` | `0x6CD2A0` | 0.2 `3E4CCCCD` (`0x6CBD68`) | share | frame | high |
| 51 | 13 | 0x52A440 <- SidekickStateFall+0x8 | `0x52A51C` | `C7828B74` `lwc1 $f2, -0x748C($gp)` | `C782A1E4` `lwc1 $f2, -0x5E1C($gp)` | `0x6CD2D4` | 0.1 `3DCCCCCD` (`0x6CBC64`) | increment | frame | high |
| 52 | 14 | 0x26ABE0 vlShader_Drift+0xC | `0x26AC14` | `C781A954` `lwc1 $f1, -0x56AC($gp)` | `C781A1F0` `lwc1 $f1, -0x5E10($gp)` | `0x6CD2E0` | 0.07 `3D8F5C29` (`0x6CDA44`) | increment | frame (guarded by taGame+0xA0) | high* |
| 53 | 14 | 0x4E2120 SnowClusterMotion+0xC | `0x4E2320` | `C781B0A4` `lwc1 $f1, -0x4F5C($gp)` | `C781A1E0` `lwc1 $f1, -0x5E20($gp)` | `0x6CD2D0` | 0.97 `3F7851EC` (`0x6CE194`) | damp | frame | high |
| 54 | 14 | 0x4E2120 SnowClusterMotion+0xC | `0x4E2308` | `C781B0A8` `lwc1 $f1, -0x4F58($gp)` | `C781A1F4` `lwc1 $f1, -0x5E0C($gp)` | `0x6CD2E4` | 0.2 `3E4CCCCD` (`0x6CE198`) | increment | frame | high |
| 55 | 14 | 0x4C2FA0 RibbonBeamProjectile+0x10 | `0x4C3054` | `C78081B4` `lwc1 $f0, -0x7E4C($gp)` | `C780A1F8` `lwc1 $f0, -0x5E08($gp)` | `0x6CD2E8` | 0.45 `3EE66666` (`0x6CB2A4`) | increment | frame | high |
| 56 | 14 | 0x4C5830 <- taGame update | `0x4C5858` | `C7838300` `lwc1 $f3, -0x7D00($gp)` | `C783A1C4` `lwc1 $f3, -0x5E3C($gp)` | `0x6CD2B4` | 0.1 `3DCCCCCD` (`0x6CB3F0`) | share | frame | high |
| 57 | 14 | 0x4C5830 <- taGame update | `0x4C58C0` | `C7838300` `lwc1 $f3, -0x7D00($gp)` | `C783A1C4` `lwc1 $f3, -0x5E3C($gp)` | `0x6CD2B4` | 0.1 `3DCCCCCD` (`0x6CB3F0`) | share | frame | high |
| 58 | 14 | 0x37DEF0 <- 0x37E060 <- listener 0x2804C0 | `0x37DF4C` | `C7808378` `lwc1 $f0, -0x7C88($gp)` | `C780A1FC` `lwc1 $f0, -0x5E04($gp)` | `0x6CD2EC` | 0.007 `3BE56042` (`0x6CB468`) | increment | frame | high |
| 59 | 14 | 0x37DEF0 <- 0x37E060 <- listener 0x2804C0 | `0x37DF74` | `C7808378` `lwc1 $f0, -0x7C88($gp)` | `C780A1FC` `lwc1 $f0, -0x5E04($gp)` | `0x6CD2EC` | 0.007 `3BE56042` (`0x6CB468`) | increment | frame | high |
| 60 | 14 | 0x37FEE0 <- AmbienceUnPhasePL2+0x10 | `0x37FEF4` | `C7818E14` `lwc1 $f1, -0x71EC($gp)` | `C781A200` `lwc1 $f1, -0x5E00($gp)` | `0x6CD2F0` | 0.0003 `399D4952` (`0x6CBF04`) | increment | frame | medium |
| 61 | 14 | 0x3804C0 <- AreaRandomSequence+0x10 | `0x380510` | `C7808D20` `lwc1 $f0, -0x72E0($gp)` | `C780A204` `lwc1 $f0, -0x5DFC($gp)` | `0x6CD2F4` | 0.0005 `3A03126F` (`0x6CBE10`) | increment | per playing slot, each frame | high |
| 62 | 14 | 0x3804C0 <- AreaRandomSequence+0x10 | `0x380534` | `C7808D20` `lwc1 $f0, -0x72E0($gp)` | `C780A204` `lwc1 $f0, -0x5DFC($gp)` | `0x6CD2F4` | 0.0005 `3A03126F` (`0x6CBE10`) | increment | per playing slot, each frame | high |
| 63 | 14 | 0x381050 <- AreaRandomSound+0x10 | `0x3810A0` | `C7808D20` `lwc1 $f0, -0x72E0($gp)` | `C780A204` `lwc1 $f0, -0x5DFC($gp)` | `0x6CD2F4` | 0.0005 `3A03126F` (`0x6CBE10`) | increment | up to 3 a frame (loop of 3) | high |
| 64 | 14 | 0x381050 <- AreaRandomSound+0x10 | `0x3810C4` | `C7808D20` `lwc1 $f0, -0x72E0($gp)` | `C780A204` `lwc1 $f0, -0x5DFC($gp)` | `0x6CD2F4` | 0.0005 `3A03126F` (`0x6CBE10`) | increment | up to 3 a frame (loop of 3) | high |
| 65 | 14 | 0x381A80 <- WindGenerator+0x10 | `0x381AD8` | `C7808C6C` `lwc1 $f0, -0x7394($gp)` | `C780A208` `lwc1 $f0, -0x5DF8($gp)` | `0x6CD2F8` | 0.005 `3BA3D70A` (`0x6CBD5C`) | increment | frame | high |
| 66 | 14 | 0x381A80 <- WindGenerator+0x10 | `0x381B00` | `C7808C6C` `lwc1 $f0, -0x7394($gp)` | `C780A208` `lwc1 $f0, -0x5DF8($gp)` | `0x6CD2F8` | 0.005 `3BA3D70A` (`0x6CBD5C`) | increment | frame | high |
| 67 | 14 | 0x279DA0 input manager (rumble) | `0x279F4C` | `C7808210` `lwc1 $f0, -0x7DF0($gp)` | `C780A1C4` `lwc1 $f0, -0x5E3C($gp)` | `0x6CD2B4` | 0.1 `3DCCCCCD` (`0x6CB300`) | share | frame (once per VBlank) | high |


### For config/overrides.toml (`[patches] instructions`)

```toml
  { address = "0x471650", value = "0xc780a1c4" }, # 0x471620 controller +0x1C8 toward 0.75: share 0.1 (was lwc1 $f0, -0x748C($gp))
  { address = "0x4716c4", value = "0xc781a1b4" }, # 0x471620 anim blend weight (0x2D87F0 on state+0x30) toward f21: share 0.3 (was lwc1 $f1, -0x7AA8($gp))
  { address = "0x47a484", value = "0xc780a1c4" }, # 0x47A440 controller +0x1F4 toward 1: share 0.1 (was lwc1 $f0, -0x748C($gp))
  { address = "0x47a5d8", value = "0xc78ca1c4" }, # 0x47A440 slerp of the actor's orientation toward a target: share 0.1 (was lwc1 $f12, -0x748C($gp))
  { address = "0x47a608", value = "0xc78ca1c4" }, # 0x47A440 same slerp, path without a node: share 0.1 (was lwc1 $f12, -0x748C($gp))
  { address = "0x47a6d4", value = "0xc780a1c4" }, # 0x47A440 state +0x58 (anim rate) toward its target: share 0.1 (was lwc1 $f0, -0x748C($gp))
  { address = "0x47b054", value = "0xc780a1c4" }, # 0x47B010 controller +0x1F4 toward 1: share 0.1 (was lwc1 $f0, -0x748C($gp))
  { address = "0x47b110", value = "0xc780a1c4" }, # 0x47B010 state +0x58 (anim rate) toward its target: share 0.1 (was lwc1 $f0, -0x748C($gp))
  { address = "0x47ea2c", value = "0xc780a1c4" }, # 0x47EA10 controller +0x118 toward 0: share 0.1 (was lwc1 $f0, -0x748C($gp))
  { address = "0x47f11c", value = "0xc780a1c4" }, # 0x47F0D0 state +0x100 toward 0 (input 0x3A): share 0.1 (was lwc1 $f0, -0x748C($gp))
  { address = "0x47f194", value = "0xc780a1c4" }, # 0x47F0D0 state +0x100 toward 1 (input 0x38): share 0.1 (was lwc1 $f0, -0x748C($gp))
  { address = "0x47f208", value = "0xc780a1c4" }, # 0x47F0D0 state +0x100 toward 0.25 (neither): share 0.1 (was lwc1 $f0, -0x748C($gp))
  { address = "0x480314", value = "0xc781a1c4" }, # 0x47FF90 +0x34 toward clamp(-v/k, +-0.35): share 0.1 (was lwc1 $f1, -0x748C($gp))
  { address = "0x469724", value = "0xc780a1c4" }, # 0x469510 +0xB0 toward -(c+0x184)-(c+0x180): share 0.1 (was lwc1 $f0, -0x7594($gp))
  { address = "0x46974c", value = "0xc780a1c4" }, # 0x469510 +0xB4 toward -(c+0x184)+(c+0x180): share 0.1 (was lwc1 $f0, -0x7594($gp))
  { address = "0x4697c0", value = "0xc780a1c4" }, # 0x469510 +0xB8 toward f5: share 0.1 (was lwc1 $f0, -0x7594($gp))
  { address = "0x46c740", value = "0xc780a1c4" }, # 0x46C6F0 +0x94 toward +0x90: share 0.1 (was lwc1 $f0, -0x7594($gp))
  { address = "0x46cad8", value = "0xc780a1c4" }, # 0x46CA80 +0x8C toward 0 (state A): share 0.1 (was lwc1 $f0, -0x7594($gp))
  { address = "0x46cb0c", value = "0xc781a1c4" }, # 0x46CA80 +0x88 toward 0 (state B): share 0.1 (was lwc1 $f1, -0x7594($gp))
  { address = "0x46cb30", value = "0xc780a1c4" }, # 0x46CA80 +0x88 toward 0 (other states): share 0.1 (was lwc1 $f0, -0x7594($gp))
  { address = "0x46cb48", value = "0xc780a1c4" }, # 0x46CA80 +0x8C toward 0 (other states): share 0.1 (was lwc1 $f0, -0x7594($gp))
  { address = "0x2d1180", value = "0xc780a1b4" }, # 0x2D0B50 gp-0x3C6C toward 0.7: share 0.3 (was lwc1 $f0, -0x7AA8($gp))
  { address = "0x2cd84c", value = "0xc780a1b4" }, # 0x2CD670 +0x4 (fov) toward its target: share 0.3 (was lwc1 $f0, -0x7AA8($gp))
  { address = "0x2cbe14", value = "0xc780a1b4" }, # 0x2CBBB0 gp-0x3C6C toward 0.7: share 0.3 (was lwc1 $f0, -0x7AA8($gp))
  { address = "0x2cbd20", value = "0xc78ca1c4" }, # 0x2CBBB0 position +0x10 = lerp(+0x10, target, 0.1): share 0.1 (was lwc1 $f12, -0x7260($gp))
  { address = "0x2ce5a4", value = "0xc780a1b0" }, # 0x2CE0A0 gp-0x3C6C toward 1: share 0.2 (was lwc1 $f0, -0x7388($gp))
  { address = "0x2cddc4", value = "0xc780a1c8" }, # 0x2CD9D0 +0xE8 toward clamp((h-1300)/1200,0,1): share 0.05 (was lwc1 $f0, -0x7368($gp))
  { address = "0x2cdde4", value = "0xc780a1c8" }, # 0x2CD9D0 +0xE8 toward 0: share 0.05 (was lwc1 $f0, -0x7368($gp))
  { address = "0x2ca194", value = "0xc780a1cc" }, # 0x2CA0B0 boom length +0x31C toward the wanted distance (capped): share 0.06 (was lwc1 $f0, -0x72D8($gp))
  { address = "0x1b3ba4", value = "0xc782a1d0" }, # 0x1B3AE0 q_s.y += c*q_body.y (then q_s normalised): grow 0.1 (was lwc1 $f2, -0x7D00($gp))
  { address = "0x1b3bc0", value = "0xc782a1d0" }, # 0x1B3AE0 q_s.z += c*q_body.z: grow 0.1 (was lwc1 $f2, -0x7D00($gp))
  { address = "0x1b3bd8", value = "0xc782a1d0" }, # 0x1B3AE0 q_s.w += c*q_body.w: grow 0.1 (was lwc1 $f2, -0x7D00($gp))
  { address = "0x1b3bf4", value = "0xc782a1d0" }, # 0x1B3AE0 q_s.x += c*q_body.x: grow 0.1 (was lwc1 $f2, -0x7D00($gp))
  { address = "0x1b3e20", value = "0xc783a1d0" }, # 0x1B3AE0 s += c*p (x,y,z: three uses): grow 0.1 (was lwc1 $f3, -0x7D00($gp))
  { address = "0x1b3e64", value = "0xc780a1d4" }, # 0x1B3AE0 s.x *= d: damp 0.9090909 (was lwc1 $f0, -0x7614($gp))
  { address = "0x1b3e74", value = "0xc780a1d4" }, # 0x1B3AE0 s.y *= d: damp 0.9090909 (was lwc1 $f0, -0x7614($gp))
  { address = "0x1b3e84", value = "0xc780a1d4" }, # 0x1B3AE0 s.z *= d: damp 0.9090909 (was lwc1 $f0, -0x7614($gp))
  { address = "0x578d04", value = "0xc780a1e4" }, # 0x577E70 part +0x8 follows its target by at most 0.1 a call: increment 0.1 (was lwc1 $f0, -0x644C($gp))
  { address = "0x57d39c", value = "0xc781a1e4" }, # 0x57D280 +0x30 += 0.1 up to 1, a fade: increment 0.1 (was lwc1 $f1, -0x4D38($gp))
  { address = "0x57d43c", value = "0xc781a1e4" }, # 0x57D280 +0x30 -= 0.1 down to 0: increment 0.1 (was lwc1 $f1, -0x4D38($gp))
  { address = "0x3cf8ac", value = "0xc780a1d8" }, # 0x3CF750 v.z (+0x68) *= 0.9 while in contact: damp 0.9 (was lwc1 $f0, -0x76A4($gp))
  { address = "0x46f920", value = "0xc780a1e8" }, # 0x46F0D0 +0x11C follows its target by at most 0.01 a frame: increment 0.01 (was lwc1 $f0, -0x508C($gp))
  { address = "0x46f950", value = "0xc780a1e8" }, # 0x46F0D0 same, upper bound: increment 0.01 (was lwc1 $f0, -0x508C($gp))
  { address = "0x426384", value = "0xc780a1dc" }, # 0x426280 spin +0x154 *= 0.995 (angle +0x150 += spin*dt): damp 0.995 (was lwc1 $f0, -0x62D4($gp))
  { address = "0x4263f8", value = "0xc780a1dc" }, # 0x426280 spin +0x184 *= 0.995 (angle +0x180 += spin*dt): damp 0.995 (was lwc1 $f0, -0x62D4($gp))
  { address = "0x495334", value = "0xc780a1d8" }, # 0x494F90 +0xD8 *= 0.9 (no named anim state): damp 0.9 (was lwc1 $f0, -0x65D8($gp))
  { address = "0x4955e4", value = "0xc780a1d8" }, # 0x495390 +0xD8 *= 0.9 (+0x100 clear): damp 0.9 (was lwc1 $f0, -0x65D8($gp))
  { address = "0x484030", value = "0xc780a1ec" }, # 0x483FC0 +0x164 += 5 deg (bone 0x1F0 opens to 90 deg): increment 0.0872665 (was lwc1 $f0, -0x6344($gp))
  { address = "0x4840c8", value = "0xc781a1ec" }, # 0x483FC0 +0x164 -= 5 deg (closes to 0): increment 0.0872665 (was lwc1 $f1, -0x6344($gp))
  { address = "0x52a5ec", value = "0xc780a1b0" }, # 0x52A440 gp-0x34C4 (vertical accel) toward state+0x10: share 0.2 (was lwc1 $f0, -0x7388($gp))
  { address = "0x52a51c", value = "0xc782a1e4" }, # 0x52A440 v.xy += 0.1*dir (both components), then speed cap: increment 0.1 (was lwc1 $f2, -0x748C($gp))
  { address = "0x26ac14", value = "0xc781a1f0" }, # 0x26ABE0 phase += drift speed (word = live 0x6CDA44 x 30/R): increment 0.07 (was lwc1 $f1, -0x56AC($gp))
  { address = "0x4e2320", value = "0xc781a1e0" }, # 0x4E2120 spin +0xB0..+0xB8 *= 0.97 (three uses): damp 0.97 (was lwc1 $f1, -0x4F5C($gp))
  { address = "0x4e2308", value = "0xc781a1f4" }, # 0x4E2120 spin z += 0.2/(d+0.1) when d+0.1 < 0.8: increment 0.2 (was lwc1 $f1, -0x4F58($gp))
  { address = "0x4c3054", value = "0xc780a1f8" }, # 0x4C2FA0 beam spiral phase +0x40 += 0.45 (wrap +0x44): increment 0.45 (was lwc1 $f0, -0x7E4C($gp))
  { address = "0x4c5858", value = "0xc783a1c4" }, # 0x4C5830 tree fade near0/near1 (0x61DFD0/4) toward 0 (two uses): share 0.1 (was lwc1 $f3, -0x7D00($gp))
  { address = "0x4c58c0", value = "0xc783a1c4" }, # 0x4C5830 tree fade near0/near1 toward +0x164/+0x168 (two uses): share 0.1 (was lwc1 $f3, -0x7D00($gp))
  { address = "0x37df4c", value = "0xc780a1fc" }, # 0x37DEF0 master fade +0x10 -= 0.007 toward +0x14: increment 0.007 (was lwc1 $f0, -0x7C88($gp))
  { address = "0x37df74", value = "0xc780a1fc" }, # 0x37DEF0 master fade +0x10 += 0.007: increment 0.007 (was lwc1 $f0, -0x7C88($gp))
  { address = "0x37fef4", value = "0xc781a200" }, # 0x37FEE0 volume +0x44 +-0.0003 toward +0x40: increment 0.0003 (was lwc1 $f1, -0x71EC($gp))
  { address = "0x380510", value = "0xc780a204" }, # 0x3804C0 volume +0xB0 -= 0.0005 toward +0xAC: increment 0.0005 (was lwc1 $f0, -0x72E0($gp))
  { address = "0x380534", value = "0xc780a204" }, # 0x3804C0 volume +0xB0 += 0.0005: increment 0.0005 (was lwc1 $f0, -0x72E0($gp))
  { address = "0x3810a0", value = "0xc780a204" }, # 0x381050 volume +0x50 -= 0.0005 toward +0x4C: increment 0.0005 (was lwc1 $f0, -0x72E0($gp))
  { address = "0x3810c4", value = "0xc780a204" }, # 0x381050 volume +0x50 += 0.0005: increment 0.0005 (was lwc1 $f0, -0x72E0($gp))
  { address = "0x381ad8", value = "0xc780a208" }, # 0x381A80 volume +0x70 -= 0.005 toward +0x74: increment 0.005 (was lwc1 $f0, -0x7394($gp))
  { address = "0x381b00", value = "0xc780a208" }, # 0x381A80 volume +0x70 += 0.005: increment 0.005 (was lwc1 $f0, -0x7394($gp))
  { address = "0x279f4c", value = "0xc780a1c4" }, # 0x279DA0 rumble baseline +0x4C8 toward the 90-entry minimum: share 0.1 (was lwc1 $f0, -0x7DF0($gp))
```

### Writing the words (sketch for `setBlendShares`, called every frame)

```cpp
// P2 words (config/overrides.toml). From the game's own constant words, so exact at 30.
auto rd = [&](uint32_t a) { float f; std::memcpy(&f, rdram + a, 4); return f; };
auto wr = [&](uint32_t a, float f) { std::memcpy(rdram + a, &f, 4); };
const float k = g_levelRate != 0 ? 30.0f / static_cast<float>(g_levelRate) : 1.0f;
auto share = [&](uint32_t src) { return shareAtLevelRate(rd(src)); };            // 1-(1-s)^k
auto damp  = [&](uint32_t src) { return g_levelRate != 0 ? std::pow(rd(src), k) : rd(src); };
auto step  = [&](uint32_t src) { return g_levelRate != 0 ? rd(src) * k : rd(src); };
wr(0x6CD2B4, share(0x6CBC64)); wr(0x6CD2B8, share(0x6CBD88)); wr(0x6CD2BC, share(0x6CBE18));
const float d = damp(0x6CBADC);                                  // physics: written together
wr(0x6CD2C4, d); wr(0x6CD2C0, g_levelRate != 0 ? 1.0f / d - 1.0f : rd(0x6CB3F0));
wr(0x6CD2C8, damp(0x6CBA4C)); wr(0x6CD2CC, damp(0x6CCE1C)); wr(0x6CD2D0, damp(0x6CE194));
wr(0x6CD2D4, step(0x6CCCA4)); wr(0x6CD2D8, step(0x6CE064)); wr(0x6CD2DC, step(0x6CCDAC));
wr(0x6CD2E0, step(0x6CDA44));  // drift speed: a variable (0x360600 writes it), read live
wr(0x6CD2E4, step(0x6CE198)); wr(0x6CD2E8, step(0x6CB2A4)); wr(0x6CD2EC, step(0x6CB468));
wr(0x6CD2F0, step(0x6CBF04)); wr(0x6CD2F4, step(0x6CBE10)); wr(0x6CD2F8, step(0x6CBD5C));
```

The menus run these functions too (`0x279DA0` input manager, `0x2804C0` listener). With
`g_levelRate == 0` the words hold the 30 values, as the existing shares do.

## Details per item

How the functions are reached (checked in the code):

- **Player states and motion shaders.** `taTransformerCreature+0x3C` (`0x349910`) calls
  `0x28E260`, the actor update, with f21 = dt. It calls the current state's slot +0x8
  through `0x46D1E0`, and for every motion shader in the list at +0xE4 it calls slot +0x8
  (`0x46D1E0`), +0xC (`0x46D190`) and +0x10 (`0x46D150`), once each per update. The object's
  vtable pointer is at +0x18 and its enable bit at +0xC bit 0.
- **Flight (`0x480830`).** Also called by PoweredGlide `0x47F0D0` and Glide `0x47F700`. It
  calls `0x47FF90` N = state+0xD0 times, with dt/N each time. N is a constant per state:
  6 for flight (`0x481654`), 20 for the powered glide (`0x47F650`). Because N does not depend
  on the rate, the per-call share 1-(1-s)^k is still right.
- **Cameras.** The taGame update calls `0x307B90` (f21), which calls `0x2C4CE0` for each
  view, which calls the camera's slot +0x34 (`0x2CAB30`). That calls `0x2CB940` for each
  script with +0x78 set: slot +0xC, then +0x10, with f12 = dt. While taGame+0x18 is set only
  the scripts' weight transition is skipped, not these calls. The boom path is `0x2CA9D0`,
  called once while the byte `gp-0x54E0` is set, then `0x2CA0B0`.
- **Physics.** The taGame update calls `0x2C60D0` once (f22), which calls `0x1B5E50`, which
  calls `0x1B3AE0` for each body. No substeps.
- **Actors.** The +0x14 slots (heavy grunt, Starscream, Megatron, drop ship) and
  `taCreatureBossUnicron+0x3C` take the frame's dt in f12 and advance timers by it. Their
  common dispatcher was not found; "frame" rests on that.

### 10. Player states and motion shaders (21 sites)

- **`0x471620` Fall (+ DoubleJump, Jump, both transforms and GlideFall call it).**
  `0x471650` (0.1): `+0x1C8 += (0.75 - +0x1C8)*0.1`.
  `0x4716C4` (0.3, not in the survey): an animation blend weight. `w = 0x2D8760(state+0x30)`;
  then `0x2D87F0(w + 0.3*(f21 - w))`, where f21 = 1 - (0.5 + 0.5*clamp(v/vmax, -1, 1)) does
  not depend on dt. A pure share, so it can use the existing 0.3 word.
- **`0x47A440` TransformToVehicle.**
  `0x47A484`: controller +0x1F4 toward 1.
  `0x47A6D4`: state +0x58, the animation rate, toward (1-w)*len/t_land.
  `0x47A5D8` and `0x47A608` (not in the survey): the same 0.1 loaded into f12 on the two
  paths (with or without the node at +0xC+0x14). It is the t of `0x1AF4C0`, a quaternion
  slerp (q1 = current orientation, q2 = target, a3 = 0 spins). The result goes to the actor
  through `0x28CED0`: the orientation turns 10% of the way to its target a frame. f12 is
  dead after the call.
- **`0x47B010` TransformToRobot.** `0x47B054`: +0x1F4 toward 1. `0x47B110`: +0x58 toward its
  target.
- **`0x47EA10` GlideFall.** `0x47EA2C`: controller +0x118 toward 0.
- **`0x47F0D0` PoweredGlide.** `0x47F11C`, `0x47F194`, `0x47F208` are three exclusive
  branches (input 0x3A, input 0x38, neither): state +0x100 toward 0, 1 or 0.25. Then
  controller +0x1EC = +0x100.
- **`0x47FF90` flight step.** `0x480314`: +0x34 toward clamp(-v/k, -0.35, 0.35); runs N times a
  frame, see above. **Leave `0x48000C`** (same constant, same function): it is
  f20 = 0.1 + 0.9*controller+0x1E8, a force scale, not a blend.
- **`0x469510` GlidePack (+0x10).** `0x469724`: +0xB0 toward -(c+0x184) - (c+0x180).
  `0x46974C`: +0xB4 toward -(c+0x184) + (c+0x180). `0x4697C0`: +0xB8 toward a speed fraction
  (0, (s-20)/20, or 1). The +0xA4 ramp is already in dt (1.6667*dt).
- **`0x46C6F0` Footslip +0xC.** `0x46C740`: +0x94 toward +0x90. Later, +0x94 mixes two
  displacements of the same frame (`0x46C878`..`0x46C8A8`). That use is not a blend over
  time, so it does not depend on the rate.
- **`0x46CA80` Footslip +0x8.** Three exclusive cases, by which animation +0x28 is:
  `0x46CAD8` (+0x8C toward 0), `0x46CB0C` (+0x88 toward 0), and `0x46CB30` plus `0x46CB48`
  (both toward 0).

### 11. Cameras (8 sites)

- **`0x2D0B50` third person.** `0x2D1180` (0.3): `gp-0x3C6C` (`0x6CF484`, a camera mix shared
  by three scripts) rises toward 0.7 by 0.3 a frame. It falls by 0.5*dt when the time scale
  `taGame+0x64` is at most 0.5, or when the creature's +0x228 is at most 0.
- **`0x2CD670` first person.** `0x2CD84C` (0.3): the field of view +0x4 toward its target,
  which comes from the data at +0x7C and +0xEC.
- **`0x2CBBB0` warp-out (+0x10).** `0x2CBE14` (0.3): the same `gp-0x3C6C` rise.
  `0x2CBD20` (0.1, a separate copy at `gp-0x7260`, not in the survey): +0x10 =
  lerp(+0x10, target position + offset, 0.1) through `0x1B0440`. +0x10 is reset to the
  camera's position only on the script's first update (+0x7A), so after that it is a
  recursive blend.
- **`0x2CE0A0` glide.** `0x2CE5A4` (0.2, `gp-0x7388`): `gp-0x3C6C` toward 1. Uses the
  existing 0.2 word.
- **`0x2CD9D0` jet pack.** `0x2CDDC4` (mode 0x10: toward clamp((h-1300)/1200, 0, 1)) and
  `0x2CDDE4` (toward 0): +0xE8 by 0.05.
- **`0x2CA0B0` boom.** `0x2CA194` (0.06): +0x31C += (wanted - +0x31C)*0.06. It is set
  straight to the limit f2 when it would pass it or when the flag t1 is up.

### 12. Physics smoothing `0x1B3AE0` (8 sites, medium)

Per body, once a frame:

- **Orientation.** Runs when the setting [+0xC]+0x2C is not 0. If flag bit 3 is set and the body
  has turned less than +0x2C from the smoothed orientation, the body is put back on it
  (`0x1C7480` sets the body's quaternion; its slot +0x24 is given a zero vector, probably
  the spin). Then q_s = normalise(q_s + 0.1*q_body) (`0x1B3BA4`, `0x1B3BC0`, `0x1B3BD8`,
  `0x1B3BF4`).
- **Position.** Runs when [+0xC]+0x28 > 0. If flag bit 3 is set and the body is within it of
  s, the body is put back on s (slot +0xC). Then s = (s + 0.1*p)*0.9090909 (`0x1B3E20` for
  the add, its register used three times for x, y and z; `0x1B3E64`, `0x1B3E74`, `0x1B3E84`
  for the multiply).

Both are averages with a share of 0.1/1.1 = 1/11 a frame (about 0.37 s at 30). A body moving
slowly enough to stay within the radius of its lagging average is held there, so it comes to
rest. At 60 the average lags half as far, so bodies are held at up to twice the speed. With
d' = 0.9090909^k and c' = 1/d' - 1, both parts get the share 1 - d' a frame: as far a second
as at 30.

What holds a body was read from the code (`0x1C7480`, slots +0xC and +0x24 of the body).
That this is the rest/anti-jitter behaviour is an inference, hence "medium".

### 13. Gameplay (14 sites)

- **`0x577E70` heavy grunt.** `0x578D04` (0.1, `gp-0x644C`): for each of two parts
  (s1 = this + 0x190 + 12*i, two indices), the part's angle +0x8 follows its target by at
  most 0.1 a frame. The register is both clamp bounds: f20 = x - 0.1, f2 = x + 0.1. The angle
  then turns a bone (`0x156100`): an aim rate limit of 3 rad/s at 30.
- **`0x57D280` taSearchCommand_Pivot +0xC.** `0x57D39C` (+0.1, up to 1) and `0x57D43C` (-0.1,
  down to 0) are two states of a jump-table switch. +0x30 is the fade whose smoothstep sets
  [+0x8]+0x154 through `0x4DA6B0`, probably a searchlight's intensity: 10 frames at 30.
- **`0x3CF750` vortex.** `0x3CF8AC` (0.9, `gp-0x76A4`): v.z (+0x68) *= 0.9 while it touches
  something (`0x3CF5D0` returns 4 or more). Gravity (5*-9.8*dt) and the move are already
  in dt.
- **`0x46F0D0` car state +0x11C.** The target is clamp((x - 0.18)/0.82 - speed/70, 0, 1), and
  +0x11C follows it by at most 0.01 a frame; +0x124 = +0x11C. `0x46F920` loads the lower
  bound and negates it (`neg.s`), after which it is used only by the clamp. `0x46F950` loads
  the upper bound. These are not the truck shares already patched at
  `0x46F6E0`/`0x46F6F4`.
- **`0x426280` drop ship** (`0x4254E0` while +0x11F is 0). `0x426384` and `0x4263F8` (0.995):
  the spins +0x154 and +0x184 decay by 0.995 a frame; the angles +0x150 and +0x180 add
  spin*dt; `0x423F90` is called once +0x128 > 10 s. The total turn ω0*dt/(1-f) stays the
  same with f' = f^k.
- **Unicron.** `0x495334` (in `0x494F90`) and `0x4955E4` (in `0x495390`), both 0.9
  (`gp-0x65D8`): +0xD8 decays by 0.9 a frame. In three named animation states it is set
  instead, to 500, 900 or 1300, or to 0 (`0x495190`..`0x495360`). What reads +0xD8 was not
  found.
- **`0x483FC0` Megatron** (from `0x484130`, from `taActorBossMegatron+0x14` while flag 0x800
  is set). `0x484030` (+5 deg) and `0x4840C8` (-5 deg): a bone (+0x1F0) opens to 90 deg
  (state 1 to 2) or closes to 0 (state 3 to 0). That takes 18 frames, 0.6 s at 30.
- **`0x52A440` sidekick fall** (`taSidekickStateFall+0x8`, also via Jump). `0x52A5EC` (0.2):
  the vertical acceleration `gp-0x34C4` moves toward state+0x10, then v.z += a*dt; uses the
  existing 0.2 word. `0x52A51C` (0.1): v.xy += 0.1*direction, in both components, before
  the speed cap max(20, +0x58). This is an acceleration a frame, an increment.

### 14. Cosmetic (16 sites)

- **`0x26ABE0` vlShader_Drift.** `0x26AC14`: phase `gp-0x3FD4` = frac(phase + [`0x6CDA44`]),
  once per level frame (guarded by taGame+0xA0 through `0x2921C0`). **`0x6CDA44` is
  written at `0x360768`** by `0x360600`, which also sets the drift shader's other parameters
  `gp-0x56C4`..`gp-0x56B0` from a settings object ([`gp-0x351C`]+0x110 and its own fields).
  Its word must be live*k.
- **`0x4E2120` snow cluster.** `0x4E2320` (0.97): the spin +0xB0..+0xB8 decays by 0.97, one
  register for all three. `0x4E2308` (0.2): spin z += 0.2/(d + 0.1) while d + 0.1 < 0.8. The
  0.1 at `0x4E22EC` softens the division, so leave it. The wind drift and the phase are in dt.
- **`0x4C2FA0` ribbon beam (+0x10).** `0x4C3054`: the spiral phase +0x40 advances 0.45 a
  frame and wraps at +0x44. It sets the beam's spiral shape (`0x4C2924`, in slot +0x14) and a
  sound parameter (`0x282510`, probably the pitch). The damage is in dt.
- **`0x4C5830` tree fade distances** (taGame update). `0x4C5858` fades `0x61DFD0`/`0x61DFD4`
  toward 0; `0x4C58C0` fades them toward +0x164/+0x168. Each load is used twice, for near0
  and near1. Both snap at 0.005, which is harmless.
- **Sound fades.** `0x37DEF0`, the ambience manager's master fade, called by `0x37E060` from
  the listener update: `0x37DF4C`/`0x37DF74`, +-0.007. `0x37FEE0` (UnPhasePL2):
  `0x37FEF4`, +-0.0003. **Medium**: the same register is also the "done" test
  (|d| <= 0.0003). Scaling it with the step only makes that tolerance one scaled step; the
  separate 0.0006 snap is unchanged. `0x3804C0` (AreaRandomSequence): `0x380510`/`0x380534`,
  +-0.0005, called once per playing slot of the same object each frame. `0x381050`
  (AreaRandomSound): `0x3810A0`/`0x3810C4`, +-0.0005, up to 3 calls a frame (a loop of 3
  around the call). `0x381A80` (WindGenerator): `0x381AD8`/`0x381B00`, +-0.005. The number
  of calls per frame does not depend on the rate, so the per-call increment c*k is right.
- **`0x279DA0` rumble.** `0x279F4C` (0.1, `gp-0x7DF0`): the baseline +0x4C8 moves toward the
  minimum of the last 90 samples. The update runs once per VBlank (`GetTimeSlot(6)`), which
  is once a frame at a level rate. The sample windows are integer counts: see the counters
  below.

## Not patched (checked: per event, inside one call, already handled, or not a share)

1. **`0x48000C`** in the flight step (`C7808B74`, 0.1): a mapping (0.1 + 0.9*glide), not a
   blend.
2. **`0x47A708` (TransformToVehicle) and `0x47B144` (TransformToRobot)** (`C7818558`, 0.3):
   w' = w + 0.3*(max(w + dt*rate(+0x58), floor) - w). While the dt term wins, the step is
   0.3*rate*dt, already in time. A share word would slow it to 54% of the speed at 60.
   Only while the floor (time-in-state progress) wins is it a share. Needs a hook; leave it.
3. **Starscream `0x48D7D0`: `0x48DD88` and `0x48DDB4`** (`C7809D08`, `gp-0x62F8` = 0.1). The
   fire cooldowns +0x1EC/+0x1F0 start at 0.1/0.05 (`0x48E28C`); the AI (`0x4881C0`,
   `0x48C0C0`) fires +0x1D0/+0x1D4 when they are at most 0. The update is
   `if (t > 0) t -= dt; else t += 0.1`: a reload in seconds, **not** an increment a frame.
   Scaling it would double the fire rate at 60. What depends on the rate is one frame per
   cycle: about 4 frames (0.133 s) at 30, 7 (0.117 s) at 60, 13 (0.108 s) at 120. A reload
   of 0.1 + (1/30 - 1/R) might restore the 30 period, but that depends on float rounding at
   the boundary. Unsure; not proposed.
4. **Twist rocket `0x3D36A0`** (slot +0x18; `lui 0x3F40` and `mtc1` at `0x3D36A0`/`0x3D36A8`):
   reached from `0x3CF250` (`taWeaponBase2+0x3C`, firing), on a logic object taken from a
   pool (`0x3D4F50` calls `0x3D5850`). It does +0x58 += 1 and +0x34 = clamp(+0x34*0.75,
   50, 150) once per launch.
5. **Patrol shield arm `0x39D520`** (slot +0x54): picks the next patrol point, from
   `0x4CC6D0` when the path runs out. +0x40 *= 0.75 (`0x39D6C8`/`0x39D6CC`) or
   +0x40 = 1 - 0.68*(1 - +0x40) (`0x39D754`). These are per decision.
6. **Autobot health `0x303260`**: +0x7C -= 0.1 (`0x303480`) once per heartbeat (latched by
   +0x78). The beat period is time-based (+0x44 += dt).
7. **`0x423BA0`, `0x489E50`, `0x498280`**: the 0.01 at `0x423C90`, `0x489F40` and `0x498370`.
   Three copies of a search loop that steps the path parameter +0x54 by 0.01 until the point
   is far enough (a distance proportional to dt). It is a search resolution, not a per-frame
   amount.
8. **`0x4316C0`** (0.95 at `0x43191C`..): the ecoSpriteGroup loader (strings "Version:",
   "spriteCount:", "HasMesh:"), which runs once.
9. **`0x438EB0`** (0.8 at `0x438FA4`..): squad goals shrink an offset up to 20 times within one
   call, looking for a valid point.
10. **`0x3ECA40`** (melee area damage): x0.5 on the damage (`0x3ECB54`/`0x3ECB58`) per new
    target hit; the 0.1 at `0x3ECF20` tilts the knockback up.
11. **`0x2F0C00`** (creature AI, x0.5 at `0x2F0D28`/`0x2F0D2C`): +0x38 = attribute², halved at
    difficulty 1. It is recomputed every call, so nothing accumulates.
12. **`0x335E00`** (x0.95): taScreenBG, PrGManagement, PrGLegal and PrGTitle are front-end
    screens and never run at a level rate.
13. **`0x247020`** (0.35 at `0x247234`): smooths a 256-entry colour table, four passes over
    neighbours (+0x1000 is a stack offset). Spatial, not temporal.
14. **Doppler `0x2801D0`** (Δpos * [`gp-0x5BF8`] * 0.5; `lui $v0, 0x3F00` at `0x2802B4`,
    `mtc1 $v0, $f3` at `0x2802BC`): already handled. `switchFrameRate` writes
    `kSoundFrameRate` (`0x6CD4F8`) = 2 * the level's rate, so Δpos*R. Should an instruction
    patch ever be wanted: replace only the `mtc1` at `0x2802BC` with `lwc1 $f3, off($gp)`.
    `$v0` is not read after it on any path (next written at `0x280300`, or clobbered by the
    call at `0x280308`), so the `lui` becomes dead code. The same one-instruction trick
    (patch the `mtc1`, keep the `lui` once its register is dead) works for any inline
    constant. The `$v1` of `0x3D36A0` and `0x39D6C8` are dead too, but those sites run per
    event.

## Other frame-rate dependencies met here (not a constant load: need a hook or a data change)

- **`0x533010`**, the creature effect switcher (P2 item 14). It adds data values to the
  texture scroll every frame: +0xC/+0x10 += +0x4/+0x8 (effects 5, 0x15 and 0x10, at
  `0x5330F8`, `0x533250` and `0x533454`), and for effect 0xF, +0x10/+0x14 += +0x8/+0xC
  (`0x533550`, `0x53356C`). The added amounts need *k when the data loads or in a hook. The
  timers already use FramePeriod.
- **`0x381CB0`**, the wind generator: the pitch +0x38 += +0x34 every frame (`0x381DE4`).
  +0x34 is a random value from data, picked at each gust.
- **Recursive lerps whose share changes over time:**
  - third-person camera: +0x50 = lerp(+0x50, target, +0xC4) at `0x2D1118`, with +0xC4
    ramped by dt;
  - glide camera: +0xA0 = lerp(+0xA0, target, +0xB0) at `0x2CE248`, with +0xB0 rising
    0.5 a second to 1;
  - flight: a slerp of state+0xE0 by state+0xDC (data) at `0x480BAC`.

  They depend on the rate only while the share is between 0 and 1.
- The 0.3 weight blends of the two transforms (item 2 of the list above).

## Integer frame counters (another treatment needed)

1. **HUD cycle.** `gp-0x3B64` (`0x6CF58C`) gets +1 per HUD component draw (`0x2FCAF4`..
   `0x2FCB10`; slots +0x20..+0x2C of `taHUDComponent` go to `0x2FC848`) and wraps at 4. It
   picks one of four alpha/flag sets (0.9, 0.7, 0.8, 0.9): a 4-frame shimmer, R/30 times
   faster.
2. **Rumble `0x279DA0`.** The ring index +0x4C4 holds 300 samples of the summed strength
   (`0x279EC0`..`0x279EE8`, 10 s at 30); the minimum is taken over the last 90 (`slti 0x5A` at
   `0x279F3C`, 3 s at 30). One sample a frame, so at R both windows shrink by 30/R. Sample
   every R/30 frames, or keep a time accumulator: the ring is fixed at 300, so raising the
   bound does not work at 120.
3. **Wind generator `0x381CB0`.** +0x40 += 1 per update (`0x382064`..`0x382074`); a gust comes
   when +0x40 > +0x3C (`0x381DF4`..`0x381E00`); +0x3C = a random integer in [+0x4C, +0x50)
   frames (`0x381FD0`..`0x382028`). +0x78 counts down one a frame (`0x381D70`) before the loop
   sound is retried.
4. **`0x533010`.** +0x134 counts the frames an effect stays requested, then counts down before
   leaving effect 1 (`0x53302C`, `0x533060`). It counts up and down in frames, so it is
   **rate-neutral**. The shared LCG (seed at [`gp-0x3DD0`]) advances once a frame per active
   effect (`0x533120`.., `0x533278`.., `0x53347C`..): R/30 times as many draws a second from
   the generator the rest of the game uses.
5. **`taGame+0xA0`** (+1 a frame at `0x292F64`). Its only direct reader is `0x2921C0`, called
   from vlShader_Drift alone as a once-a-frame guard (rate-neutral). The
   `vlShaderContextSource<int>` at taGame+0xA4 (vtable `0x6B4A80`) feeds data-driven shaders;
   not traced.

## Checks done

- Every site: the ELF word decodes as `lwc1 ft, off($gp)`, and the word at the effective
  address equals the constant (all 0.1 sources are `3DCCCCCD`, all 0.9 are `3F666666`). The
  generated listing shows the same word, so no site is patched already. The new word is
  `(0x31<<26)|(28<<21)|(ft<<16)|(off&0xFFFF)` with the same ft.
- Every site's register was tracked to its overwrite or to the end of its paths: used only
  in the per-frame operation. The cases with more than one use are both bounds of one clamp
  (`0x578D04`, `0x46F950`), the x/y/z of one vector (`0x1B3E20`, `0x4E2320`, `0x52A51C`),
  near0 and near1 of the tree fade (`0x4C5858`, `0x4C58C0`), or the fade's own tolerance
  (`0x37FEF4`).
- The free words `0x6CD2B4`..`0x6CD2FC` are 0 in the ELF, and nothing in the code or the
  port refers to them. Apart from `0x6CDA44`, no source constant has a store or a pointer
  taken to it.
- The new sites overlap none of the existing patches.
