# Player guide

Everything you need to install, set up and play the PC port.

- [What you need](#what-you-need)
- [Install](#install)
- [Starting the game](#starting-the-game)
- [Controls](#controls)
- [Settings (F1)](#settings-f1)
- [Photo mode](#photo-mode)
- [Updates](#updates)
- [Your files](#your-files)

Something not working? See [TROUBLESHOOTING.md](TROUBLESHOOTING.md).

## What you need

- **Windows 10 or 11**, 64-bit.
- **A graphics card with DirectX 12 and Vulkan.** Any recent AMD, NVIDIA or Intel card works.
  Vulkan is used once, to make the HD textures.
- **About 15 GB of free space.**
- **Your own disc image of Transformers (USA)**, `SLUS-20668`, as an `.iso` file.
  You can make one from your own disc with a DVD drive and a tool such as ImgBurn.
  The European release, `SLES-52388`, works too, experimentally (see [The European disc](#the-european-disc)).
  **Other languages need the European disc**: the USA disc has English only.
  The Japanese release does not work yet.

The setup installs the rest: Git, Python and the Visual Studio C++ build tools. Windows may ask for permission.

## Install

### With the setup program (recommended)

1. Download **`tfa-setup.exe`** from the [latest release](https://github.com/kayliox/transformers-armada-recomp/releases/latest).
2. Run it. Pick your disc image and an empty install folder, then press **Install**.
3. Wait. The setup shows each step as it goes:
   - the tools it needs are installed;
   - your disc is checked and the game is built from it (about 15 to 25 minutes the first time);
   - the HD textures are made on your graphics card (from tens of minutes to a few hours; you can skip this
     step and play with the original textures).
4. Press **Play**.

The setup holds no game code. It downloads this repository, and everything else is made on your PC from your disc.

### From the command line

```powershell
git clone --recursive https://github.com/kayliox/transformers-armada-recomp
cd transformers-armada-recomp
.\setup.ps1 -Iso "D:\Games\Transformers (USA).iso"
```

- `-InstallPrerequisites` installs any missing tools with winget.
- `-SkipHdTextures` leaves out the HD textures. You can make them later in the game (F1 > Graphics > Make HD textures) or run `setup.ps1` again.
- If the setup stops, run it again. Finished steps are kept.

For a manual build, see [BUILDING.md](BUILDING.md).

## Starting the game

Start **`Play.bat`** in the install folder. The setup program also has a Play button.

On the first start the game uses good default settings for most PCs. Press **F1** to change them.

## Controls

A controller, the keyboard and the mouse all work at the same time. The game's own hints show the buttons of the
device you used last.

### Controllers

DualSense, DualShock 4 and Xbox controllers work out of the box. Other controllers work through Steam Input or as
Xbox-style controllers.

| Action | PlayStation | Xbox |
|---|---|---|
| Move | Left stick | Left stick |
| Camera | Right stick | Right stick |
| Fire | R1 | RB |
| Jump, movement Mini-Con | L1 | LB |
| Transform | Triangle | Y |
| Mini-Con abilities | L2, R2 | LT, RT |
| Aiming view (toggle) | R3 | Right stick click |
| Confirm | Cross | A |
| Back | Circle | B |
| Pause | Options (Start) | Menu |
| Answer an HQ call (Select) | Create (Select) | View |

On a DualSense or DualShock 4, vibration and the light bar work too. Set them in **F1 > Controls**.

To put the buttons elsewhere, use **F1 > Controls > Controller buttons**: click Change and press the button you want.
**Triggers fire** puts Fire and Jump on the triggers and the Mini-Con abilities on the bumpers; **Reset to default**
undoes it. The F1 menu's own buttons always stay the same. (The game's on-screen button pictures still show the
original buttons.)

### Keyboard and mouse

Click into the game window to give the mouse to the game. **Esc** gives it back.

| Action | Keys |
|---|---|
| Move | W A S D |
| Camera | Mouse (or I J K L) |
| Fire | Left mouse button, or Left Alt |
| Jump, movement Mini-Con | Space, Q, or the middle mouse button |
| Transform | T, F or V |
| Mini-Con ability (L2) | Left Shift or Z |
| Mini-Con ability (R2) | E, Left Ctrl or Right Shift |
| Aiming view (toggle) | Right mouse button, or B |
| Confirm (Cross) | X |
| Back (Circle) | C or Backspace |
| Square | R |
| Pause | Enter or P |
| Select | Tab |
| D-pad | Arrow keys |

Change any key in **F1 > Controls**: click **Change** and press the new key, or **Add** to keep the old one too.
**F1 > Controls > Drive with movement keys** lets W and S drive the car.

### Function keys

| Key | What it does |
|---|---|
| F1 | Opens and closes the settings menu |
| F3 | Shows or hides the frame rate |
| F6 | Saves a quick save state |
| F9 | Loads the quick save state |
| F8 | Photo mode |
| F11 | Switches between a window and full screen |

Save states are kept in the `states` folder. They are a copy of the game's memory, so keep them private, like
your disc image.

## Settings (F1)

Press **F1** to open the menu over the game. Use the mouse, the arrow keys or a controller. Most settings change
at once. A few (marked **RESTART**) need the game to start again: the menu offers **Restart now**, which brings
you back to the same moment.

Your choices are saved in `settings.ini` in the install folder.

### Display

| Setting | What it does |
|---|---|
| Display mode | A window, or full screen. F11 switches too. |
| Screen | Which monitor the game uses. |
| Aspect ratio | 4:3, 16:9, 21:9, Match screen, or Ultrawide 2.4:1. Wider shows more at the sides. |
| Field of view | How wide the camera sees. |
| Frame rate limit | The most frames a second the game draws: 30, 60, 90, 120, 144, 165, 240 or Unlimited. The game always runs at its true speed. |
| Show frame rate | A frame counter in the corner (F3). |
| Sharp text | The game's fonts, redrawn sharp. |
| Button prompts | Match device (your keys, or your controller's buttons), PlayStation, Xbox, or PS2 (original). |
| Crosshair | The size of the aiming crosshair. |
| HQ menu panels | The size of the HQ's hologram menus. |
| Speedrun timer | A timer with a split for each level. |

### Graphics

| Setting | What it does |
|---|---|
| Preset | Low, Medium, High or Ultra: one choice for all the settings below. |
| Renderer | Native (the new Direct3D 12 renderer, faster and sharper) or Emulated (the PS2's look). |
| Draw distance | How far you see detail. Under Advanced: scenery, forest, far trees, grass and ground detail separately. |
| Trees | Every tree as a full 3D model at any distance. Looks best, costs the most. |
| Enemy detail | How detailed enemies look at a distance and in crowds. |
| Anti-aliasing | Smooths jagged edges: edge filter, multisampling (up to 8x) and supersampling (1.5x). |
| Texture quality | Texture sharpness and filtering. Low suits 4 GB graphics cards. |
| Sharp distant textures | Keeps textures sharp into the distance on high-resolution screens. |
| HD textures | The HD textures made from your disc, on or off (native renderer; takes a restart). Its buttons open their folder, **Make HD textures** (says the disk space and time first), **Verify HD textures** (checks images without changing them) and **Remake N broken images** (after a verify that found some). On Linux, the maker buttons show [commands to run in a terminal](BUILDING.md#hd-textures-and-sharp-fonts). |
| Texture pack | Emulated renderer only: PCSX2 texture packs from a replacements folder. Not the HD textures. |

The **Ultra** preset sets everything to its highest: the longest distances, every tree in full 3D, full enemy
detail, 8x multisampling with 1.5x supersampling and Ultra textures. It needs a strong graphics card. If the
frame rate drops, try **High**.

If a graphics card makes the HD textures black (some GTX 16-series cards do), the maker notices, tries safer
ways, and if none works it makes none: F1 says so and the game shows its own textures. After a graphics driver
update, **Make HD textures** tries again.

### Controls

Mouse speed, mouse in menus, vibration, the light bar, the stick dead zones, the stick response, car steering and the
key layout.

- **Left / right stick dead zone, Auto** (the default): the game measures where each stick rests when you let go of it
  and ignores that much, so a worn stick that does not come back to the centre does not make your robot walk or your
  car steer on its own. A stick that rests at the centre keeps the usual 5%. You can also pick a fixed size.
- **Stick response, Modern** (the default): a light touch walks, about a third of the stick runs.
  **Off** gives the original response back.
- **Car steering, Original** steers as on the PS2. **Tuned** is gentler near the centre.

### Sound

Volume, from Off to full.

### Gameplay

- **Camera:** third person, first person, or first person with the game's aiming view on foot.
- **Photo mode** (also F8).
- **Start-up:** what shows when the game starts. You can skip the logos and the intro movies.

### Saves

Your memory card saves, and import or export of PS2 saves (from the `Saves` folder). Save states by name.

### About

Your version, what is new, and updates.

## Photo mode

Press **F8** (or **F1 > Gameplay > Photo mode**). The game freezes and the HUD hides. Look with the mouse and move
the camera with W A S D. Press F8 or Esc to leave.

## Updates

The game checks for a newer version by itself, a little after it starts and then every few hours. When there is
one, a small notice shows in the menus.

**F1 > About > Update now** saves the moment you are at, closes the game, updates and rebuilds it on your PC, and
starts it again right where you were. Your settings, saves and memory cards stay as they are.

You can also update with the setup program (open it on your install folder) or from the command line:

```powershell
.\update.ps1              # gets the new version and rebuilds what changed
.\update.ps1 -CheckOnly   # only says whether there is something new
```

If you changed files in the install folder yourself, the update keeps your changes on a branch of their own
(named like `my-changes-2026-11-01-1430`) and tells you. Nothing is lost.

To turn the automatic check off: **F1 > About > Check for updates automatically**.

## Your files

Everything the setup makes from your disc stays on your PC, in folders that Git ignores:

| Folder or file | What it is |
|---|---|
| `game\` | Your extracted disc, and your memory cards (`game\mc0`, `game\mc1`) |
| `work\`, `out\` | The game built from your disc |
| `hd_textures\` | The HD textures made from your disc |
| `states\` | Your save states |
| `settings.ini` | Your settings |

Never share or upload these: they contain game data.

## The European disc

Support for the European release (`SLES-52388`) is experimental: it plays, but it is played less than the USA disc.
Please report what does not work.

- **Language (European disc only)**: English, French, German, Spanish or Italian, text and voices:
  **F1 > Gameplay > Language**. Auto follows your Windows language. The USA disc has English only, and has no such
  setting.

  > **Changing the language while you play:** the voices and the button bar at the bottom of the screen change at
  > once. Most menu text changes when its screen is shown again. **The HQ's titles under its big icons (Autobots,
  > Options, Quit...) keep the old language until the HQ is loaded again** (after a mission), and so does the bar
  > on **Extras > Movies**. The original game had no language menu: it chose its language once, at start-up.
  > **For everything in one language, set it before you start, or close the game and start it again after
  > changing it.**
- **50 Hz or 60 Hz**: the game runs at the same speed in both, so by default it does not ask and starts in 50 Hz.
  To get the choice back, set **F1 > Gameplay > When the game starts** to **Everything**.
- **Saves**: the European disc keeps its saves in its own folder on the memory card (`BESLES-52388 TRANSFM`).
  **F1 > Saves** imports a USA save into it, and exports either kind.
