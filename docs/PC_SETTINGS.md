# The PC settings in the game's own Options menus

The common PC settings are rows and pages inside the game's own Options screens, in its own font and style, so a player
finds them where the game's options are. F1 stays the place for everything else ("More settings" opens it).
Code: `transformers/src/pc_settings_row.cpp` / `.h`. On by default; `TF_PC_SETTINGS_ROW=0` leaves the game's menus
alone; `TF_PC_SETTINGS_TRACE=1` logs what it does (and reads each setting back after writing it, and dumps each row with its
choices); `TF_PC_SETTINGS_PROMPT=0` leaves out the restart prompt (the red notice stays).

## Where it shows

- **Start menu, Options:** the hub itself: Display, Graphics, Audio, Controls, Gameplay, Other, More settings. The game's old rows
  moved into it: Widescreen On / Off is the Display page's **Aspect Ratio** row, **Screen Position** is a row of Other (it runs the
  game's own adjust screen), **Sound Mode** is a row of Audio. Back at the hub is the game's own Back (it saves and returns to the
  Start menu). The Start menu itself also gets **QUIT GAME** (a No / Yes page; Yes stops the game cleanly).
- **HQ, Options:** Audio Settings (the game's own), Display, Graphics, Controls, Gameplay, More settings. The game's separate
  Controller Settings row is gone: its options are in Controls. Controls also has **Button Layout** (the game's own button layout
  screen); its Back returns to the Controls menu.
- **Pause menu, Options:** an OPTIONS list: Audio Settings (the game's own in-game audio screen), Display, Graphics, Controls,
  Gameplay, More settings. The audio screen's Back comes back to this list (not to the pause menu), and the screen holds the two
  sound rows only: its Invert Y-axis, Vibration and Look Spring are in Controls > Gamepad. While the pages are up the world
  music plays (the game lets it play on its audio screen, so the volume can be judged: `PauseMusic`), and the footer says
  "X Select  T Back" (the game's own help-text item, set to the HQ list's text and back).

## The shared list

Which of F1's settings the game's menus show is ONE list: `transformers/src/game_menu_list.h`. Both sides read it.

- **The pages are built from it.** A page's rows are the list's entries for that page, in that order; each entry names an F1
  setting by its key, or one of F1's merged rows (`aspect`, `preset`, `antialiasing`, `volume`, `prompts`), with the short name it
  has in the game's menus and a few flags (left out in the pause menu or on the Start menu, a slider, a setting the preset sets,
  and a choice of F1's that the game's row leaves out: Ultra). Nothing else says what a page holds.
- **F1 checks it.** `TF_MOD_MENU_LIST=1` logs F1's tabs once and then `[mod menu] game menus: ...` lines: one for every entry F1 does
  not have in this build, and one for every row F1 shows by default on its settings tabs (outside "Advanced", so with "Show all
  settings" off) that is in neither the game's pages nor the list's F1-only part. A row added to F1's default view is therefore
  decided in the list (a page, or F1Only), not forgotten; the last line says how many differences there were (none is the aim).
- The game's own options (Invert Y, Look Spring, R3 Recon, Sound Mode) are entries too, with their `TF_OPT_*` keys; the hub's
  links, Back, QUIT GAME, Screen Position, Button Layout and the restart prompt are the pages' own structure and are not in it.

## Pages and the settings behind them

The rows follow F1's default list (the rows visible with "Show all settings" off) and use F1's own keys and choices. A screen holds
7 rows, so the hubs and the pages are as full as they can be. The Start menu's pages with a value row keep their first row for the
restart notice (below), so there **Show FPS** (Display) and **Glow** (Graphics) are left out: the HQ, the pause menu and F1 have them.

| Page | Rows (settings key) |
|---|---|
| Display | Display Mode (`PS2X_FULLSCREEN`), Screen (`PS2X_MONITOR`), Aspect Ratio (`PS2X_WIDESCREEN` + `TF_ASPECT`; also the game's own widescreen switch, live), Field of View (`TF_FOV`), Frame Limit (`TF_FPS_CAP`), VSync (`PS2X_PRESENT_SYNC`), Show FPS (`PS2X_SHOW_FPS`) |
| Graphics | Preset (F1's Low, Medium, High; Custom shows once a row is changed by hand), Resolution (`PS2X_GPU_SCALE`), Draw Distance (`TF_DRAW_DISTANCE`), Anti Aliasing (`TF_NATIVE_AA` + `PS2X_GPU_MSAA` + `TF_NATIVE_SSAA`: Off, FXAA, FXAA MSAA 4x, SMAA MSAA 4x), HD Textures (`TF_HIRES`), Smooth Textures (`TF_SMOOTH_TEXTURES`), Glow (`TF_NATIVE_BLOOM`) |
| Audio (Start menu) | Volume (`PS2X_AUDIO` + `PS2X_AUDIO_VOLUME`: Off, then F1's steps), Sound Mode (the game's own option) |
| Controls > Gamepad | Invert Y Axis, Look Spring, R3 Recon (the game's own options), Vibration (`PS2X_SONYPAD_RUMBLE`: Off to Full, and the DualSense's trigger strength, `TF_TRIGGER_STRENGTH`, goes with it; haptic triggers stay on, `TF_ADAPTIVE_TRIGGERS` is for F1; it also sets the game's own Vibration option, off for Off and on for any strength, live, and a short buzz tells the player it has come on) |
| Controls > Sticks | Left / Right Dead Zone (`PS2X_STICK_LEFT_DEADZONE`, `..._RIGHT_...`), Stick Response (`PS2X_STICK_RESPONSE`), Car Steering (`PS2X_STICK_VEHICLE_MODE`) |
| Controls > Mouse | Mouse in Game (`TF_MOUSE`), Mouse Speed (`PS2X_MOUSE_SENSITIVITY`), Invert Mouse (`PS2X_MOUSE_INVERT_Y`), Mouse in Menus (`TF_MENU_MOUSE`), Right-Click Aim (`TF_RIGHT_AIM`), Drive with Keys (`TF_CAR_KEYS`) |
| Gameplay | HUD (`TF_CFG_showHUD`), Sharp Text (`TF_CRISP_TEXT`), Button Prompts (`TF_BUTTON_SYMBOLS` + `TF_KEY_PROMPTS` + `TF_PAD_PROMPTS`, as F1's), Crosshair (`TF_SMALL_CROSSHAIR`), Start-up (`TF_START_SCREENS`), Objective Guide (`TF_OBJECTIVE_GUIDE`) |
| Other (Start menu) | Check for Updates (`TF_UPDATE_CHECK`), Screen Position (the game's own) |

Left out of the default list's rows because they have no setting key or do not fit a list row: Camera (it is F1's own control, not a
setting), Controller buttons (the HQ's Controls has Button Layout, the game's own screen), the DualSense rows.

**How a row gets its choices.** A row with a settings key (a list entry) takes its choices from F1's own
registry at the moment the page is built: whatever F1 offers, the page offers, with a short form of the label (the game's font has
no "x" sign, a value column holds about 16 characters). A setting this build does not have, or one F1 greys out (Glow without the
native renderer), leaves its row out. A value that no choice stands for (a hand-written `settings.ini`; the Anti Aliasing and
Preset rows, which set several keys) is shown as a last choice, Custom. A row of more than four choices has the label of the
choice it is on in all four value texts the game's list holds, and the game steps it as it steps any row (the arrows, the sound);
the choice moves by the move that was given (left or right).

**F1's merged rows.** A row that sets several settings with one choice (Preset, Anti Aliasing, Aspect Ratio, Volume, Button
Prompts) takes the choices from F1's own function for that row (`mod_menu::mergedOptions`: the choices, the settings each one
writes, and the other values that also count), copied by the window thread like the rest of the registry. The pages keep no
copy of F1's tables, so a change to F1's preset or to its Smooth edges levels is in the pages with it. Two details are the
pages' own: the Preset row leaves out Ultra (the list's `omit`: its 8x multisampling and every tree in 3D cost too much for a
basic menu) and adds Custom last, and the Anti Aliasing choices are named by technique from the settings each one writes (the
edge filter, `TF_NATIVE_AA` 1 FXAA or 2 SMAA; the multisampling; the supersampling), so the name is always what the choice does:
F1's Low, Medium and High show as FXAA, FXAA MSAA 4x and SMAA MSAA 4x.

**F1 opened over a page.** F1 (the F1 key, or the More settings row) can change a setting that the page shows. A row keeps the choice
it had when the page was built, and stepping it from there could undo F1's change, so when F1 has been closed
(`mod_menu::closeCount()` moved) the page reads its rows again, on the same row.

A changed value is written through the settings registry as F1 does (so a live setting applies at once and `settings.ini`
follows). Settings that only apply at the next start are written and kept (the file has them) and read back as they were in the
session: `PS2X_GPU_MSAA`, `TF_NATIVE_QUALITY` (the preset), `TF_HIRES`, `TF_CRISP_TEXT`, `TF_BUTTON_SYMBOLS`, `PS2X_WIDESCREEN` (the
game's own widescreen switch is set live as well). The game's own options (Invert Y, Look Spring, R3 Recon, Vibration, Sound Mode)
are also kept in `settings.ini` as `TF_OPT_INVERT_Y`, `TF_OPT_VIBRATION`, `TF_OPT_LOOK_SPRING`, `TF_OPT_R3_RECON`,
`TF_OPT_SOUND_MODE` (default `keep` = what the save has): the game writes its options only from its own save screens, so a change
made from the HQ or the pause menu would otherwise be lost at the next start.

**Slide.** In the HQ and the pause menu a page change slides the old rows out, makes the change and slides the new rows in, as the
game's own screens (its audio screen) do: the bars are placed by the game's own `SetBarPositions` (0x312820), called during the slide
with the "moving in" state and a position of the slide's own timing (the screen's real state and position, which the pause menu's tint
and the screen manager read, are put back after the call). The screen's own transition speeds set its length. Input waits while the
old rows slide out. `TF_PC_SETTINGS_ANIM=0` leaves it out; the Start menu's text lists have no bars and no slide.

**Keys and the mouse.** Escape is Back in the pause menu's pages (the OPTIONS list too): it goes back one page and resumes the
game only at the pause menu's top. The pause screen's own input handler runs for the pages, so `mod_menu`'s "the pause screen's
top is showing" (which turns Escape into Start) leaves them out (`pc_settings_row::pausePagesUp`). The mouse points at the rows in
the Start menu's lists and the pause menu's pages (menu_mouse.cpp); on the HQ's hologram panels it points once the panel is zoomed
in, at the rows' measured places (the panel is placed alike in every widescreen shape, and another with the game's widescreen off),
and anywhere else a click takes the selected row, the wheel steps and the right button goes back. The button bar at the bottom
of a menu ("X Select  T Back", the Start menu's "Change / Save / Cancel") takes clicks too: a click on a glyph or its word presses
that button (Select and Save are Cross, Back and Cancel are Triangle, Change is Right). The bar's place is modelled (centred, about
0.0245 of the picture's height per character, split at the gaps) for the English texts of the Start menu, its Options, the saved
games, the HQ's Options and the pause menu with its pages (`menu_mouse.cpp`, `barText`); other screens' bars take no clicks.

**Sounds.** The game plays its menu tones from its own rows' code, so a page's Cross and Back are given theirs here (in game
0x209 and 0x205, in the front end 0x218 and 0x202); left / right and up / down keep the game's own.

**Sliders.** In the lists with bars (the HQ, the pause menu) the Vibration row and the two dead zones are rows of steps like the
game's volume bars (ten steps; the choices are spread over them, the first at none, the last at all; the choice is in the row's
label). The Start menu's text list shows them as values. The vibration goes from Off (left) to Full (right).

**Restart hint.** A row whose change only applies at the next start (the registry says so: F1 tags the same row RESTART) shows its
value as it is. While such a change is on the page, a red **REQUIRES RESTART** notice sits under the page title: in the Start menu's
text-only list it is the page's first row (the list counts its rows up to its title, so the line needs a row of its own; the
selection skips it, and it is blank while nothing waits), in the rows' own size; in the HQ and the pause menu it is a red tile (a
copy of the first row's bar, drawn with the game's own bar calls). Leaving a page with such a change asks once, on a page like the QUIT GAME one: **RESTART
REQUIRED**, with **Later** (selected) and **Restart to apply**. Leaving the pages for the screen they sit on (Back at the Options hub,
or at a page the HQ's or the pause menu's list opened) asks once more about every change that still waits, on whichever page it was
made (once per setting), so changes on several pages are covered by one restart; Later goes on out of the menu. Restart to apply is F1's own restart (`ps2_settings::restartNow`,
called on the window thread): the game is saved and started again from that state, where the player is. The game's own options
(`TF_OPT_*`) are applied by the game's setters, so they are in effect when chosen and never wait for a restart.

**Restart under Steam.** Restart to apply starts the game again from a state in a new process and ends the first. A launcher that
counts the first process as the game (Steam's Game Mode, Proton) would see the game close at that moment, so under Steam
(`SteamGameId` / `SteamAppId` set) or Wine the first process waits instead (`restart_wait.cpp`): it finds the process that was
started again, holds the exit watchdog, trims its memory, suspends every other thread, waits until the new process ends and then
ends too. A plain Windows launch exits at once, as before.

## How it works

- Each screen is a `taScreenInGMenu` list (up to 7 rows: count at +0x7B0, labels at +0x7C0). The game builds its screens from
  a data file, so no new screen exists: the live screen is reused, its own rows saved (three blocks of its memory) and the
  same list refilled with a page's rows. The game's own base handler steps value rows (arrows, sound, wrap), so left / right
  needs little code; a row of more than four choices is described above.
- Hooked: each screen's input handler (slot 18: Start menu `0x557540`, Start Options `0x5568B0`, HQ Options `0x3862A0`, pause
  menu `0x311A30`), the functions that fill their rows (`0x557220`, `0x386460`, `0x311F30`, and the HQ's shown-again
  `0x386260`), the button layout screen's input (`0x384DE0`) and the string-table lookup (`0x1A9410`: every label is a short
  key - "PC" and the text's hash - and the hook hands the table the label itself as the key, a `std::string` in a frame of
  the guest stack for that one call; the table has no such key and answers with a copy of it, made by the game's own
  string code), and the list screen's two draw functions: the text-only one (`0x3136C0`) gets the greyed-row text style set red
  before its draw (the notice row is a skipped row, which the game draws in that style; `0x232670`, `0x233090`), the one with bars
  (`0x313F30`) gets the notice tile drawn after its own draw with the game's text and bar calls (`0x232670`, `0x2326B0`, `0x233090`,
  `0x2330F0`, `0x233330`; `0x1F8330`, `0x1F8800`, `0x1F8390`), the greyed style put back as it was. The lookup is entered with its
  own address as the return address, so its return comes back to the hook,
  which leaves the frame: the state of the call is in guest registers and memory, and a lookup the scheduler stopped
  half way (or a save state made at that moment) still ends properly. No guest data is added (`guest_code::addData`).
- The game's own options live in a table (pointer at `0x6CF244`) and are written through the game's own setters
  (`0x278010`, `0x277F10`, `0x277EA0`). After the game copies or applies a profile (`0x32EE40`, `0x2780E0`) or sets the defaults
  (`0x32DFD0`, `0x32E0F0`, and the two small reset functions `0x278250` / `0x278260`, which reach them by a jump) the stored
  `TF_OPT_*` values are written again (`installLate()`, registered after the port's other hooks on those functions).
- `mod_menu` gets two small additions: `requestOpen()` (the game thread asks for F1, for the More settings row) and one call of
  `pc_settings_row::pump()` per frame, which writes the queued settings on the window thread.

## Known limits

- **Save states.** The module adds nothing to guest memory, so the states' layout check is the same with it on and off:
  states made before it existed, or with `TF_PC_SETTINGS_ROW=0`, load with it on, and the reverse. What a state made with
  it on carries is what was in the screens at that moment: a row it added (the Start menu's QUIT GAME) or a page that was
  open (F6 on a page, or "restart to apply" from the More settings row). With the module on, a later session shows an added row
  with its text (the keys are the texts' hashes) and closes a page that was open: the screen is filled again by the game's
  own function and shows its own menu (the pages' session state - which page, the screen's rows kept aside - is not in
  the state). With the module off those show as their keys (`PC17F61890`) and do nothing, until the game fills the screen
  again; a page that was open stays on that screen, so make states for a session without the module with the pages closed.
  A state asked for (F6) while a label's lookup is half done holds that lookup, which returns into the module's hook: a build
  without this module has no hook there, accepts such a state (the layout key is the same) and hangs on it. Labels are
  looked up every frame (about 4,000 lookups in 900 ticks with a page open), so a state asked for at a bad moment can hold one.
- After a state is loaded the pages are closed (the page mode is dropped: `stateLoaded()`). The close is the game's own fill of
  the screen; when the scheduler stops that fill half way it is run again at the next input call (up to 16 times), and the
  game's handler gets no input until it has finished.
- The Start menu's footer hints ("Change / Save / Cancel", "Select") stay whatever page is shown (the pause menu's are set to the
  HQ list's, above); the font has no plus sign and draws mixed-case labels as capitals.
- The Start menu's QUIT GAME row and the pages are rebuilt when the Start menu polls the memory card (its state drops for one
  call every 14th call); presses that land in that call are still taken.
