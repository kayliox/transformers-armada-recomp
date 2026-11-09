# What's new

## Unreleased

**New**
- Native Linux and Steam Deck version (no Proton needed). Set up with `./setup.sh --iso <your disc>`, play with
  `./play.sh`, update with `./update.sh` (or F1 > About > Update now).
- Linux: the native renderer works, drawn through Vulkan by vkd3d (Wine's Direct3D 12), with the HD textures and
  sharp fonts (`./tools/make_hd_textures.sh`). The build makes vkd3d itself; without the tools it needs (setup.sh
  lists them) the game has the emulated renderer only. vkd3d cannot yet compile the grass shader's double-precision
  version, so the grass uses its single-precision version there (checked on a Steam Deck: the same picture, 0.1% of
  pixels apart at blade edges).
- Linux: OpenGL goes through Mesa's Zink driver when the usual driver lacks a feature the GPU renderer needs (AMD,
  the Steam Deck). `TF_ZINK=0` turns this off.
- Linux: the mouse in the menus, the F1 menu's typing and scrolling, Xbox-controller vibration and the HD movies work
  as on Windows.
- Linux: the game finds its files and memory card saves whatever the case of their names, as on Windows, and the
  text and numbers it formats come out as on Windows.
- Linux: the main menu has a Quit Game item, as PC games do.
- Steam Deck: the button prompts show the Deck's A / B / X / Y (the bundle carries the sharp fonts and prompts).
- Linux: the F1 menu uses the system's own font (Noto Sans or DejaVu Sans) instead of a blocky pixel font, and on a
  handheld (a Steam Deck) its text is a third larger.
- Steam Deck (Gaming Mode): the game starts full screen, at the screen's own resolution (Display mode in F1 still
  decides once you change it).
- Steam Deck: jump and fire are on the triggers (R2 fires, L2 jumps), the Mini-Con abilities on the bumpers. F1 >
  Controls changes it; "Reset to default" comes back to it.
- Linux: the Discord status works too (the Discord app's socket, including the Flatpak and Snap versions).
- Linux: the F1 menu's "Restart now" (for settings that need a restart) works: the game starts again where you were.

**Fixed**
- The first level (Amazon Basin) no longer stops with "Generic fatal error" on Linux: a C library function the game
  uses behaved differently there.
- The game no longer crashes when it closes on Linux with Zink.
- Linux: a game menu no longer keeps scrolling on its own after the left stick is touched (a stick that rests a
  little off centre, as a Steam Deck's can, read as held). Gameplay keeps the stick response.
- Linux: the robot no longer walks on its own in play with a stick that rests a little off centre (a dead zone of
  12%; F1 > Controller > Stick dead zone changes it).
- Linux: the frame rate no longer goes past what the screen shows (a Steam Deck's 90 Hz panel ran parts of the game
  at 175 frames a second, the extra pictures never seen): less CPU, GPU and battery used.
- The F1 menu no longer shows red stripes and broken text with Zink (Mesa's command reordering is turned off
  for the game; `ZINK_DEBUG` set by you still wins).

## 0.4.6

**New**
- While the HD textures are being made, a small line in the bottom-right corner shows how far along they are. When
  they are done it tells you to restart, and F1 has a button for it that brings you back where you were.
- F1 > Graphics can stop making the HD textures for this session, and carry on again.

**Changed**
- The HD textures are now made only while the game is open. They stop when you close it and carry on from where
  they left off the next time you start it.

**Fixed**
- HD textures that came out with the wrong colours (green and purple floors in Antarctica on some graphics cards)
  are no longer used: the game draws its own texture there instead, and the next HD check makes them again.
- With a DualSense, the HQ's first-time messages (the warp gate, the first Data-Con and the like) now come from the
  controller's speaker, like the other radio calls.

## 0.4.5

**Fixed**
- The setup now also checks for the Windows SDK, which the C++ build needs next to Visual Studio's C++ tools.
  Without it the setup stopped at "Disc and game" with "No CMAKE_C_COMPILER could be found". When it is
  missing, the setup adds it to your Visual Studio (Windows asks for permission once). Run the setup again; the
  steps it already finished are kept.

## 0.4.4

**New**
- Render scale (F1 > Graphics, native renderer): draws the picture with fewer pixels and enlarges it to fill the
  window, at 100%, 85%, 75%, 67% or 50%. Softer, but much lighter for the graphics card: on integrated graphics
  that cannot keep up at the screen's resolution it can more than double the frames you see. 100% by default, so
  nothing changes unless you pick it. Thanks to TheBrousse.
- VSync setting (F1 > Graphics). On, as before, the game draws at most one picture per screen refresh. Off, it
  draws as many as your PC can, even on a 60 Hz screen (lower input delay, the picture may tear). The frame rate
  cap still applies.
- Local contrast (F1 > Graphics, native renderer, off by default): gives rocks, ice and metal more depth while
  keeping the colours and the overall brightness. It has a strength slider. The HUD and the menus are not changed.

**Changed**
- Faster start-up with the native renderer: its shaders are now made while the game is built and stored inside
  it, so even the very first start on a PC no longer spends several seconds (up to 20 seconds on low-end PCs)
  preparing them. The grass shaders, which were prepared again at every start, are now kept too. Thanks to
  TheBrousse.

**Fixed**
- On a 60 Hz screen the HQ's Drop Zones screen no longer drops to 15 or 30 frames a second; it now runs at 60 like
  the rest of the HQ.
- The game's film player now finds the sound of films whose sound is marked a little differently. On the European
  disc this made the Melbourne House logo and the intro movie stop the game with an error screen; the US disc's
  films were not affected. Thanks to TheBrousse.

## 0.4.3

**Fixed**
- The setup no longer stops at "Disc and game" with "No CMAKE_CXX_COMPILER could be found" on PCs that have
  more than one Visual Studio installed. It now uses the one that has the C++ tools. Run the setup again; the
  steps it already finished are kept.

## 0.4.2

**New**
- Discord status: while you play, your Discord profile can show the level you are in (or the HQ, the main menu,
  or a film), the Autobot you play with a small picture, and for how long. It needs the Discord app running on
  your PC; without it nothing happens. On by default; switch it off in F1 > Other > Discord status.
- Two new looks for the native renderer in F1 > Graphics, both off by default: "Glow: Modern bloom" draws the
  game's glow at your screen's full resolution, and "Tone mapping: Filmic" gives the 3D world a film-like look with
  more contrast and richer colour. Each has a strength slider under it. The HUD and the menus stay as they are.
- The button pictures in the game's prompts now follow your controller layout (F1 > Controls > Controller
  buttons): with "Triggers fire", a Jump prompt shows L2 and a Fire prompt shows R2. The L1 / L2 / R1 / R2 labels
  of the Mini-Con loadout in the HQ follow it too.

**Fixed**
- Playing with keyboard and mouse, the prompts could show PlayStation buttons (for example "(X) Open" at a
  Mini-Con) when a controller that never rests was plugged in, such as a pedal, a flight stick or a virtual pad
  from a mapping tool. Such a device now only counts once it has been seen at rest, so your keys are shown.
  Thanks to SirCebollo for the report.
- Antarctica: the whole picture could turn bright bluish white for a single frame.
- A texture could sometimes be drawn from an older picture that had already been replaced. This was found while
  looking into SirCebollo's report of a green and purple pattern on Antarctica's canyon floors; if you still see
  that pattern, please tell us.
- With a DualSense, a radio call you replay with Show Objective in the pause menu now plays from the controller's
  speaker again, like the first time.
- If something goes wrong while the game is starting, it now stops right away and its log says why. Before, the
  game could start and then close on its own about 5 seconds later.

**Changed**
- More of the native renderer is now split into a game part and a graphics-card part, on the way to a Vulkan
  renderer. Nothing should look different.

## 0.4.1

**Fixed**
- The films (intro, story and ending movies) now fill the screen at their own wide shape. Before, they sat small in
  the middle inside a 4:3 box.
- With the "one-hit kills" cheat on, the boss fights (Starscream, Megatron, Cyclonus, Tidal Wave) could never end:
  the boss lost all his health at the first hit but was never knocked down, so the level stayed stuck. The bosses
  now take normal hits with that cheat (F1 > Mods & cheats > "One-hit kills on bosses" brings back the original
  behaviour). This was in the original game too.
- Loading a save state made at a higher frame rate while the game is limited to 30 fps no longer makes everything run
  twice as fast.
- The darker patches on some jungle ground have softer edges instead of hard straight lines.

**New**
- RecompLauncher support (thanks to Xarishark): a release now also comes as a small ZIP that RecompLauncher can
  install and start. It uses your own USA disc image and builds the game for you, the same way as the setup, and
  RecompLauncher can show and change the main display, graphics and controller settings. In RecompLauncher's
  Big Picture mode the game starts in full screen, unless you already picked a display mode yourself.
- "Smooth pictures between frames" (off by default) now moves body parts along their turn instead of in a straight
  line, so they no longer shrink for a moment while turning.

**Changed**
- Behind the scenes the native renderer is being split into a game part and a graphics-card part, the first step
  toward a Vulkan renderer for Linux, the Steam Deck and Android. Nothing should look different.

## 0.4.0

**Fixed**
- The setup no longer stops at once with "powershell could not be started: program not found" on PCs whose PATH
  is missing Windows' own folders. Download the new tfa-setup.exe and run it again.

## 0.3.9

**Fixed**
- The setup no longer stops with "prerequisites installed, but not found yet: vs" on PCs that already have Visual
  Studio (or its Build Tools) without the C++ tools. The setup now adds the C++ tools itself; Windows asks for
  permission once. If it still fails, the message explains how to add them by hand.

## 0.3.8

**Fixed**
- HD textures on graphics cards that make them black (some NVIDIA GTX 16 series): the HD texture maker now checks
  every image and tries safer ways on such cards. Existing HD sets are checked once after this update; broken images
  are made again (or, if the card cannot make them, the game's own textures are used).
- The desktop shortcut is now made by the update too (it did not appear for installs updated from inside the game).

**New**
- Controller buttons can be changed: F1 > Controls > Controller buttons, with a "Triggers fire" preset (swaps the
  bumpers and triggers) and "Reset to default". The adaptive triggers follow your layout.
- F1 > Graphics: an "HD textures: On / Off" setting. The "Texture pack" settings only show with the Emulated
  renderer, which they belong to.

## 0.3.7

**Fixed**
- Hot Shot's red parts (and other dark textures) no longer turn black or navy on some NVIDIA cards (GTX 16 series).
  The HD texture maker can produce broken images on those cards; the game now notices a black, colour-missing or
  colour-swapped HD image and uses the game's own texture instead. A repair for the HD set itself comes next.

## 0.3.6

**Fixed**
- HUD Off (F1 > Gameplay) now hides the whole HUD at once, with no restart: the bars, the Mini-Con ring, the insignia,
  the messages and the crosshair. Photo mode hides it too.
- Recon mode on the keyboard: the right mouse button goes in, zooms in two steps, then goes out. Quick clicks are no
  longer missed, and B no longer does recon (you can still bind a key to it in F1 > Controls).
- First person: the car now uses its normal car camera instead of a broken view from inside the car. First person
  comes back when you transform back into the robot.

## 0.3.5

**New**
- Playing with keyboard and mouse, the game's help texts name your keys: "W A S D - Move/Strafe" and "Mouse -
  Look/Aim" instead of the analog sticks, also for steering, recon mode and the Glide Mini-Con. With a controller the
  original words come back.
- Existing installs also get the desktop shortcut, with their next update.

## 0.3.4

**New**
- You can play while the HD textures are being made: the setup has a "Play now" button during that step. The
  textures you see first (menus, the HQ, your Autobots, the first levels) are made first, and new ones appear in the
  game while you play. Making them pauses while the game runs and carries on when you close it, unless you tick
  "Keep making HD textures while I play".
- A "Fast" choice for the HD textures: about 9 minutes instead of about an hour on a GTX 1080, a little less sharp
  on the ground. The setup suggests Fast or Best for your graphics card.
- If the game is started before the HD textures are finished, a small window finishes them in the background.
- F1 > Graphics: "Open texture folder", and "Make HD textures" for anyone who skipped them. F1 shows how many are made.

**Fixed**
- "Sharp text" now says so when its fonts have not been made yet, instead of quietly showing the old text.

## 0.3.3

**Fixed**
- HQ with Hot Shot in a 4:3 picture: the character select showed jumbled letters where his car pictures belong, and
  the QUIT station showed a big grey panel after leaving Options. Both show the right pictures now.

## 0.3.2

**Fixed**
- Closing the setup before it finished and starting it again no longer opens the game. The setup opens instead,
  with a "Finish the setup" button that carries on where it stopped.

**New**
- The setup puts a "Transformers Armada" shortcut on your desktop after the first install (it starts the game).

## 0.3.1

**Fixed**
- The frame rate no longer stays at 60 in levels and the HQ when the frame rate limit is set higher (for example 144
  or 165). The "Frame rate limit" setting now always decides.

**New**
- The settings menu (F1) works with a controller: open it with the PS button (on Xbox, hold View and Menu), move with
  the D-pad, Cross (A) to choose, Circle (B) to go back.
- DualSense and DualShock 4: the light bar shows your Autobot's colour (Optimus red, Hot Shot yellow, Red Alert blue).
- DualSense: adaptive triggers for weapon Mini-Cons (missiles click, cannons click harder, the sniper rifle resists
  steadily), off in car mode. The shot fires at the click. "Trigger strength" in F1 > Controls.
- DualSense: the controller's speaker plays the transform sound, enemies spotting you, the low-health warning and HQ
  calls (Bluetooth or cable). "Surround" plays all sound from both the PC and the controller, with a delay setting
  for Bluetooth. Both are in F1 > Controls.

## 0.3.0

**Fixed**
- Linux and Steam Deck (Proton): the screen no longer turns pink after the Atari logo. The game now shows its picture
  under Proton (checked on Linux with GE-Proton). Building the game still needs Windows.

**New**
- DualSense and DualShock 4: the PS button opens and closes the settings menu (F1). On an Xbox controller, hold View
  and Menu together.
- DualSense and DualShock 4: the light bar blinks blue and red while your health is low, and blinks while your Autobot
  is destroyed. It can be turned off in F1 > Controls ("Light bar warnings").

## 0.2.9

**New**
- Once the game is built, starting the setup again starts the game straight away, so a game launcher (for example
  Quiver) can use the setup as its Play button. Hold Shift while starting it to open the setup's window instead.

**Fixed**
- Emulated renderer: a single garbled frame (red and green bands) that sometimes showed a second or two after
  loading a save state.

## 0.2.8

**Fixed**
- The setup no longer stops at "Disc and game" with "detected dubious ownership" when the game is installed on a
  drive formatted as exFAT or FAT32 (many external drives and memory cards).

## 0.2.7

**New**
- The game can be started straight from its .exe, for example as a non-Steam game in Steam (Steam Deck with Proton).
  It finds its own folder and makes its settings file on the first start, like Play.bat. Building the game still
  needs Windows.

## 0.2.6

**Fixed**
- The setup no longer stops at "Tools" with "pip install cmake failed" when another program (for example Inkscape)
  has put its own Python on your PC. It now uses a full Python install, or installs one.

## 0.2.5

**Fixed**
- The setup no longer stops at "Tools" with "no newer package versions are available" when Git is already
  installed but Windows can't find it. It now finds and uses the Git you have.
- Native renderer: the HQ warp gate's tunnel no longer flickers dark every few frames. It pulses smoothly, like on
  the PS2.

## 0.2.4

**Fixed**
- With the Emulated renderer (Graphics > Renderer), your Autobot's shadow on the ground was missing. It's back.
- The setup program now builds and publishes itself automatically again.

**Changed**
- The setup program now carries proper Windows file details and avoids things antivirus programs tend to dislike,
  so it should be flagged far less often.

## 0.2.3 (first public release)

**New**
- Settings menu (F1) tidied up: presets at the top of Graphics (Low, Medium, High, Ultra), related options merged,
  and the fine details under "Advanced".
- "Trees: Full 3D at any distance" option: no more flat trees popping in the distance (heavy; Ultra turns it on).
- Xbox controllers vibrate, and the button prompts follow your controller (PlayStation or Xbox symbols, or your
  keyboard keys).
- Modern stick response: a light touch walks, running starts at about a third of the stick.
- The game's .exe now has its own icon, made from your disc.

**Fixed**
- Ragdolls behave as on the PS2 at every frame rate: Megatron's kick throws you into the lava again instead of
  kicking you over and over, and falls no longer spin in the air.
- Human mode: enemies now aim at your size instead of shooting over your head.
- First-person view: walking, running and jumping work like in third person.
- The crosshair is no longer cut in half by the ground.
- Movies no longer stutter when the game is in the background (alt-tab).
- Less input delay at 30, 60 and 90 fps frame caps.
- A busy PC (a video render, a big download) no longer turns the main menu black; the game just runs slower.
- The HQ's Options screen no longer flashes pixelated blocks during its camera move.
- The Emulated renderer at very high settings no longer crashes on graphics cards with less memory.
- Antarctica's music plays again when you replay the level after finishing it (an original PS2 bug).
- Decepticlones dropped by a dropship no longer hang frozen in the air after a cutscene (an original PS2 bug).
