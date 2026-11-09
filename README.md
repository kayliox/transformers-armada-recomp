<p align="center">
  <img src="docs/images/hero.jpg" alt="Optimus Prime on the jungle path in Amazon Basin, native renderer at 21:9" width="100%">
</p>

<h1 align="center">Transformers (PS2): PC Port</h1>

<p align="center">
  The 2004 PlayStation 2 game <i>Transformers</i> (released in North America as
  <i>Transformers Armada: Prelude to Energon</i>), recompiled into a native Windows game.
</p>

<p align="center">
  <a href="https://github.com/kayliox/transformers-armada-recomp/releases/latest/download/tfa-setup.exe"><img src="https://img.shields.io/badge/Download-Setup%20for%20Windows-2b7bd6?style=for-the-badge&logo=windows&logoColor=white" alt="Download the setup for Windows" height="56"></a>
  &nbsp;
  <a href="https://github.com/kayliox/transformers-armada-recomp/releases"><img src="https://img.shields.io/github/downloads/kayliox/transformers-armada-recomp/total?style=for-the-badge&label=downloads&color=444" alt="Total downloads" height="56"></a>
</p>

<p align="center">
  <b>The setup contains no game.</b> You need your own copy of <b>Transformers (USA, SLUS-20668)</b> as an
  <code>.iso</code> disc image. The setup installs the tools it needs and builds the game on your PC from your disc.
</p>

<p align="center">
  <a href="https://github.com/kayliox/transformers-armada-recomp/releases"><img src="https://img.shields.io/github/v/release/kayliox/transformers-armada-recomp?label=version" alt="Latest version"></a>
  <img src="https://img.shields.io/badge/platform-Windows%2010%20%7C%2011-blue" alt="Windows 10 and 11">
  <img src="https://img.shields.io/badge/renderer-Direct3D%2012-purple" alt="Direct3D 12">
  <img src="https://img.shields.io/badge/license-GPL--3.0-green" alt="GPL-3.0">
</p>

---

## Trailer

<p align="center">
  <a href="https://www.youtube.com/watch?v=i5nzhxsKbaU"><img src="docs/images/trailer.jpg" alt="Watch the trailer" width="80%"></a>
</p>

## What you get

- **A native PC game.** The game's own code runs as a Windows program. No emulator to set up.
- **A new Direct3D 12 renderer** with high resolutions, anti-aliasing, supersampling and sharp textures.
- **Widescreen and ultrawide:** 16:9, 21:9 and wider, with the HUD at the screen edges and a field of view slider.
- **High frame rates:** smooth motion up to your screen's refresh rate. The game always runs at its true speed.
- **Longer draw distances:** the forest, the grass and the scenery reach much further than on the PS2.
- **HD textures**, made on your own graphics card from your own disc.
- **Sharp text and new button prompts** for PlayStation, Xbox and keyboard.
- **Keyboard and mouse**, DualSense, DualShock 4 and Xbox controllers, all at the same time.
- **Native PC sound**, a settings menu (F1), photo mode (F8) and built-in updates.

## The PS2 and the PC port

Each pair shows the same place. On the left, the original game as it looks on the PlayStation 2 (4:3).
On the right, the PC port at 21:9 with every graphics setting at its maximum.

<table>
  <tr>
    <th>PlayStation 2</th>
    <th>PC port, 21:9, Ultra</th>
  </tr>
  <tr>
    <td width="36%"><img src="docs/images/ps2-hq.jpg" width="100%" alt="The Autobot HQ on the PS2"></td>
    <td width="64%"><img src="docs/images/pc-hq.jpg" width="100%" alt="The Autobot HQ in the PC port at 21:9"></td>
  </tr>
  <tr>
    <td width="36%"><img src="docs/images/ps2-minicons.jpg" width="100%" alt="The HQ Mini-Con menu on the PS2"></td>
    <td width="64%"><img src="docs/images/pc-minicons.jpg" width="100%" alt="The HQ Mini-Con menu in the PC port at 21:9"></td>
  </tr>
  <tr>
    <td width="36%"><img src="docs/images/ps2-globe.jpg" width="100%" alt="The mission globe, Amazon selected on the PS2"></td>
    <td width="64%"><img src="docs/images/pc-globe.jpg" width="100%" alt="The mission globe, Amazon selected in the PC port at 21:9"></td>
  </tr>
  <tr>
    <td width="36%"><img src="docs/images/ps2-gate.jpg" width="100%" alt="The warp gate to Amazon Basin on the PS2"></td>
    <td width="64%"><img src="docs/images/pc-gate.jpg" width="100%" alt="The warp gate to Amazon Basin in the PC port at 21:9"></td>
  </tr>
  <tr>
    <td width="36%"><img src="docs/images/ps2-jungle.jpg" width="100%" alt="The jungle path in Amazon Basin on the PS2"></td>
    <td width="64%"><img src="docs/images/pc-jungle.jpg" width="100%" alt="The same jungle path in the PC port at 21:9"></td>
  </tr>
  <tr>
    <td width="36%"><img src="docs/images/ps2-pause.jpg" width="100%" alt="The pause menu in Amazon Basin on the PS2"></td>
    <td width="64%"><img src="docs/images/pc-pause.jpg" width="100%" alt="The pause menu in Amazon Basin in the PC port at 21:9"></td>
  </tr>
</table>

### Text and button prompts

The game's fonts are redrawn sharp, and the button prompts follow the device you use.

<table>
  <tr>
    <td width="22%"><b>PlayStation 2</b></td>
    <td><img src="docs/images/prompts-ps2.jpg" width="100%" alt="The original prompt bar on the PS2"></td>
  </tr>
  <tr>
    <td><b>PC, PlayStation controller</b></td>
    <td><img src="docs/images/prompts-playstation.jpg" width="100%" alt="Sharp text with PlayStation buttons"></td>
  </tr>
  <tr>
    <td><b>PC, Xbox controller</b></td>
    <td><img src="docs/images/prompts-xbox.jpg" width="100%" alt="Sharp text with Xbox buttons"></td>
  </tr>
  <tr>
    <td><b>PC, keyboard and mouse</b></td>
    <td><img src="docs/images/prompts-keyboard.jpg" width="100%" alt="Sharp text with keyboard keys"></td>
  </tr>
</table>

### The settings menu

Press **F1** at any time. Most settings change at once, while you play.

<p align="center">
  <img src="docs/images/menu-graphics.jpg" width="100%" alt="The F1 menu, Graphics page">
</p>

## Original PS2 bugs fixed

Some bugs are in the original game on the PlayStation 2 too. This version fixes them:

- **Antarctica, played again after finishing it, has no music from the first warp gate.** The level music starts at
  a spot on the cliff edge that the game places only on a first visit, so a finished Antarctica stays silent to the
  end. Confirmed on the original game. Here a finished Antarctica keeps that spot, for the music only, without the
  story's radio call. `TF_REVISIT_MUSIC=0` in settings.ini gives the PS2's behaviour back.
- **Decepticlones dropped by a dropship can hang frozen in the air.** If a cutscene that freezes the enemies (for
  example a Mini-Con's discovery) starts while a dropship is still carrying Decepticlones in, the game wipes their
  "fall when dropped" instruction, so they stay in mid-air when the ship lets them go. Rare on the PS2 too. Here the
  passengers keep that instruction through the cutscene, so they land and fight.

## Requirements

- Windows 10 or 11, 64-bit.
- A graphics card with DirectX 12 and Vulkan (any recent AMD, NVIDIA or Intel card).
- About 15 GB of free disk space.
- Your own disc image of **Transformers (USA)**, `SLUS-20668`. Other regions do not work yet.

The setup installs everything else for you (Git, Python and the Visual Studio C++ build tools).

**On Linux or a Steam Deck** the game can also run natively, without Windows: see
[Linux and Steam Deck (native)](#linux-and-steam-deck-native) below.

## Install

1. **Download** `tfa-setup.exe` with the button above.
2. **Run it**, pick your disc image and an install folder, then press **Install**.
   The first build takes about 15 to 25 minutes. Making the HD textures takes longer: pick **Fast** for a quicker
   set, or skip them. Once they start, **Play now** lets you play while the rest is made: the ones you see first
   (the menus, the HQ, the first levels) are made first.
3. **Play.** The setup ends with a Play button. Later, start the game with `Play.bat` in the install folder, or
   straight from `out/build/transformers/RelWithDebInfo/TransformersRecomp.exe` (for example as a non-Steam game in
   Steam).

The game checks for updates by itself. When there is one, **F1 > About > Update now** installs it and puts you
back where you were.

Prefer the command line, or want to build it yourself? See **[docs/BUILDING.md](docs/BUILDING.md)**.

## Play from Steam, another launcher, or a Steam Deck

**RecompLauncher:** use the Windows ZIP from Releases. It includes the setup and a
`recomplauncher.json` manifest, so [RecompLauncher](https://github.com/elripalda/RecompLauncher)
can build from your USA disc, show settings and back up saves. See
[the integration guide](docs/RECOMPLAUNCHER.md).

Once the game is built, starting `tfa-setup.exe` again starts the game itself, so a launcher such as
[Quiver](https://quiverlauncher.com) that keeps the setup can use it as the game's Play button. Hold **Shift** while
starting it to open the setup's window instead (updates, repair).

The game can also be started straight from its program file, so any launcher can start it:

`<install folder>/transformers-armada-recomp/out/build/transformers/RelWithDebInfo/TransformersRecomp.exe`

(`<install folder>` is the folder you picked in the setup.)

**Steam on Windows**

1. In Steam: **Games > Add a Non-Steam Game to My Library > Browse**, pick `TransformersRecomp.exe` above, then
   **Add Selected Programs**.
2. Start it from your Steam library like any other game.

**Steam Deck**

The game has to be built on a Windows PC first: the setup needs Windows to compile it from your disc.

1. On your Windows PC, install the game with the setup as described above, and play it once.
2. Copy the whole install folder to the Steam Deck, for example with a USB drive, a microSD card or a network
   share. Keep everything inside it.
3. On the Deck, switch to **Desktop Mode**, open **Steam**, then **Games > Add a Non-Steam Game to My Library >
   Browse**. Set the file type to **All Files**, pick `TransformersRecomp.exe` in the copied folder, then
   **Add Selected Programs**.
4. In your Steam library, open the game's **Properties > Compatibility**, tick **Force the use of a specific Steam
   Play compatibility tool** and choose **Proton Experimental** (or the newest Proton).
5. Go back to **Gaming Mode** and start the game.

Tips for the Deck:

- The settings menu opens with **F1**, or by holding **View** and **Menu** together on the Deck's controls (the
  Deck's buttons work like an Xbox controller).
- In **F1 > Graphics**, the **Low** or **Medium** preset suits the Deck best.
- You can also run the game natively on the Deck, without Proton: see the next section.

## Linux and Steam Deck (native)

The game also builds as a native Linux program, with both renderers: the native one (as on Windows, drawn through
Vulkan) and "Emulated (PS2)". HD textures and sharp fonts work too: make them with `./tools/make_hd_textures.sh`.

**On a Linux PC:**

1. Get the code: `git clone --recursive https://github.com/kayliox/transformers-armada-recomp`
2. In that folder run `./setup.sh --iso "/path/to/Transformers (USA).iso"`. If tools are missing, it lists them with
   the command that installs them on your distribution; install them and run it again. The first build takes about
   15 to 25 minutes.
3. Play with `./play.sh`. Update with `./update.sh`, or in the game with **F1 > About > Update now**.

**On a Steam Deck:** SteamOS cannot install the build tools directly. Either build on a Linux PC and copy the whole
folder to the Deck, or build on the Deck inside a container (Desktop Mode, Konsole):
`distrobox create -n tfa-build -i ubuntu:24.04`, then `distrobox enter tfa-build`, install the tools `setup.sh` asks
for, and run `./setup.sh` there. Then add `play.sh` to Steam as a non-Steam game (**Games > Add a Non-Steam Game to
My Library > Browse**, file type **All Files**) and start it from Gaming Mode.

## Controls

A controller, the keyboard and the mouse all work at the same time. Click into the game window to use the mouse.
Esc gives the mouse back.

| Action | PlayStation | Xbox | Keyboard and mouse |
|---|---|---|---|
| Move | Left stick | Left stick | W A S D |
| Camera | Right stick | Right stick | Mouse |
| Fire | R1 | RB | Left mouse button |
| Jump | L1 | LB | Space |
| Transform | Triangle | Y | T or F |
| Mini-Con abilities | L2 / R2 | LT / RT | Left Shift / E |
| Aiming view | R3 | Right stick click | Right mouse button |
| Confirm / Back | Cross / Circle | A / B | X / C |
| Pause | Options (Start) | Menu | Enter or P |

Every key can be changed in **F1 > Controls**. The full list, with the function keys, is in
**[docs/PLAYING.md](docs/PLAYING.md)**.

## Help

- **Player guide:** [docs/PLAYING.md](docs/PLAYING.md) (settings, controls, updates)
- **Problems:** [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) (common fixes and FAQ)
- **Bugs and ideas:** open an [issue](https://github.com/kayliox/transformers-armada-recomp/issues).
  Never attach disc images, save states or memory card files: they contain game data.

## How it works

The game's PlayStation 2 program (MIPS code for the Emotion Engine) is translated to C++ ahead of time with
[PS2Recomp](https://github.com/ran-j/PS2Recomp), then built into a Windows program. A runtime stands in for the
console hardware. On top of that, this port replaces the PS2's graphics with a native Direct3D 12 renderer and its
sound with native PC sound.

The translation happens on your PC, from your disc. This repository holds only the recipe: the tools, the
runtime and the port's own code.

## No game data

This repository contains no game data: no disc images, no executables, no extracted files and no code generated
from the game. Everything is built from your own copy. A commit check keeps it that way.

## Contributing

Issues and pull requests are welcome. See **[docs/CONTRIBUTING.md](docs/CONTRIBUTING.md)**.

## Credits

- **[PS2Recomp](https://github.com/ran-j/PS2Recomp)** by ran-j: the recompiler and runtime this port is built on (GPL-3.0).
- **kayliox:** the PC port, the renderer and the sound.
- **Jenvery:** the HQ frame-rate fix.
- **TPepperoni666:** the stick response and the Xbox button prompts.
- **SirCebollo:** testing, and finding and reporting bugs.
- The approach follows [Project Amp](https://gitlab.com/ztpeditor/project-amp), which brought *Amplitude* to PC the same way.
- Third-party components and their licences: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Licence

The code in this repository is licensed under the GNU General Public License v3.0, like PS2Recomp. See
[LICENSE](LICENSE).

This is an unofficial fan project. It is not affiliated with or endorsed by Hasbro, Atari, Melbourne House or
Sony Interactive Entertainment. *Transformers* is a trademark of Hasbro. The screenshots are captured from the
game for illustration.
