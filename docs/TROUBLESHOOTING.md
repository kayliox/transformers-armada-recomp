# Troubleshooting and FAQ

## Common problems

### The setup says my disc is not supported

**Transformers (USA)**, `SLUS-20668`, works, and the European release, `SLES-52388`, experimentally. The Japanese
release does not work yet. Make sure the file is a full `.iso` image of the disc, not a compressed or partial copy.

### "cmake not found" or "python not found" right after the setup installed it

Windows only sees newly installed tools in new windows. Close the setup (or PowerShell), open it again and run
the setup again. Finished steps are kept.

### The build fails

- Run the setup again. A network hiccup during a download is the most common cause.
- After an update, run `.\update.ps1 -Force` to rebuild everything.
- If it still fails, open an [issue](https://github.com/kayliox/transformers-armada-recomp/issues) with the end
  of the output.

### The game shows the original, blurry textures

The HD textures were skipped or are not finished. Run `setup.ps1` again without `-SkipHdTextures`. Making them
takes from tens of minutes to a few hours, depending on your graphics card.

### The frame rate is low

- Try a lower **Preset** in **F1 > Graphics**: **High** or **Medium**.
- The most costly settings are **Trees** (every tree in full 3D), **Supersampling** and **Multisampling 8x**.
- Close other programs that use the graphics card.
- On a laptop, make sure the game runs on the dedicated graphics card and the laptop is plugged in.

### The picture stutters on a FreeSync or G-Sync screen

Try **F1 > Display > Frame rate limit** at your screen's refresh rate, or a little below it. Under Advanced,
**Frame pacing** has fixed steps for screens without FreeSync or G-Sync.

### The mouse does not turn the camera

Click into the game window first. **Esc** gives the mouse back to Windows.

### My controller does not work, or shows the wrong buttons

- DualSense, DualShock 4 and Xbox controllers should work right away. Others work through Steam Input.
- If Steam Input shows your PlayStation controller as an Xbox controller, set **F1 > Display > Button prompts**
  to **PlayStation**.
- If your robot walks, your car steers or the view turns on its own (stick drift), set **F1 > Controls > Left / Right
  stick dead zone** to a larger size. Auto already covers how far your sticks rest from the centre when you let go.
- If Steam and the game both read a Sony controller, set **F1 > Controls > Controller support** (under Advanced)
  so only one of them does.

### There is no sound

Check **F1 > Sound > Volume**, and that Windows plays sound on the right device. The game uses the Windows default
output device.

### The update says "Could not check"

The game checks for updates with Git in the background. Check your internet connection. You can always update by
hand with `.\update.ps1` in the install folder.

### The game crashes

Open an [issue](https://github.com/kayliox/transformers-armada-recomp/issues) and say:

- your version (**F1 > About**);
- your graphics card and Windows version;
- what you were doing;
- your `settings.ini`.

Never attach disc images, save states, memory cards or play recordings: they contain game data.

## FAQ

### Is this an emulator?

No. The game's own program is translated to C++ and built into a Windows program on your PC. A small runtime
stands in for the PS2 hardware, and the graphics and sound are replaced by native PC code.

### Why do I need my own disc?

The game belongs to its owners. This project shares only the tools and the port's own code. The game itself
(its program and all its data) comes from your copy, and stays on your PC.

### Can I download the game here?

No. This repository and the setup contain no game files. Please do not ask for them or share them in issues.

### Do my PS2 saves work?

Yes. **F1 > Saves** imports and exports PS2 memory card saves through the `Saves` folder in the install folder.

### Does it run on Linux, macOS or the Steam Deck?

Not yet. The port is a Windows program, and the renderer uses Direct3D 12. Other systems are not tested.

### How much faster than the PS2 is it?

The game logic runs at the same speed as on the PS2, so it never plays too fast. The picture is drawn as often as
your PC and screen allow, so motion is much smoother.

### Can I use a PCSX2 texture pack?

Yes. Put the pack's files in a `replacements` folder in the install folder and turn on **F1 > Graphics > Texture
pack** (under Advanced). Texture packs are other people's work and are not included.

### Where are my settings and saves?

In the install folder: `settings.ini`, the memory cards in `game\mc0` and `game\mc1`, and save states in `states`.
Updates never touch them.

### How do I uninstall it?

Delete the install folder. The tools the setup installed (Git, Python, the Visual Studio build tools) stay
installed; remove them from Windows Settings if you do not need them.
