# Contributing

Issues and pull requests are welcome. Thank you for helping.

## Reporting a bug

Open an [issue](https://github.com/kayliox/transformers-armada-recomp/issues) and include:

- your version (**F1 > About**);
- your graphics card, Windows version and screen resolution;
- the renderer (Native or Emulated) and your `settings.ini`;
- what you did, what happened, and what you expected;
- where in the game it happens (level, area, what was on screen).

One problem per issue makes it easier to fix and to close.

**Never attach game data:** no disc images, no extracted files, no save states, no memory cards and no play
recordings. Screenshots and short videos are fine.

## Pull requests

1. Fork the repository and make a branch for your change.
2. Build it (see [BUILDING.md](BUILDING.md)) and play the part of the game your change touches.
3. Keep each pull request to one topic. Small pull requests are reviewed faster.
4. Describe what you changed and how you tested it. For a change you can see, add a before and after screenshot.

`main` must always build and play. A pull request is merged once it builds and the game still plays with it.

### Changes to PS2Recomp

The `PS2Recomp` folder is a submodule and is never committed to directly. Changes to it live as patch files in
`patches/ps2recomp/`, which `build.ps1` and `build.sh` apply in sorted order. Write one patch file per change
(upstream's patches keep their names through `0171`; this port adds `1001` and up), then check the whole series:

```powershell
python tools\make_patches.py --verify
```

It applies every patch to a clean checkout of the pinned commit and compares the result with the submodule's
working tree. It never rewrites the patch files.

The bare command is upstream's writer: it deletes all existing `.patch` files and replaces the series with five
grouped area patches (`0001`–`0005`). Do not use it to update this hand-written series.

### No game data, ever

This repository must never contain anything from the game: no disc images, no executables, no extracted or
converted files, no generated code, and no textures, sounds or movies made from them. Everything is built from the
player's own disc.

A commit check enforces this. Turn it on once in your clone:

```powershell
git config core.hooksPath .githooks
```

### Portable code

The game is meant to reach Linux, the Steam Deck and later Android, so new code should not lock it to Windows or
to x86:

- Keep Windows-only calls inside the platform layer, not in game or renderer code.
- When you write SSE or other x86-only code, add a plain C++ path next to it (an ARM build will need one).
- Prefer code that builds with both MSVC and Clang.

Small steps toward this are welcome in any pull request, especially Linux and Steam Deck ones, which share most
of the work an Android build needs.

## Code style

- Follow the style of the file you are editing.
- Write comments in plain words: what the code does and why.
- New settings go through the settings menu (F1) so players can find them, with a short help line.

## Licence

By contributing, you agree that your contribution is licensed under the GNU General Public License v3.0, like the
rest of the project.
