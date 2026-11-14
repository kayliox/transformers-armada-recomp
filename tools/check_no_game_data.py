#!/usr/bin/env python3
"""Refuses to commit game data: the git pre-commit check of this repository.

Purpose: nothing from the discs may ever enter the repository (README, "No game data"). This looks
         at what a commit adds or changes and stops it when a file is game data or looks like it:
         - a path under a folder that only ever holds game data (game/, Mods/, hd_textures/, work/, out/,
           states/, cards/, Saves/, runs/ - at the top, or under port/ in a repository that holds this one there);
         - a game file type (disc images and archives, the game's own formats: models, textures,
           databases, sounds, movies, the executable);
         - a file over 5 MB (code and documents are never that big; the build output and dumps are).
         Our own text files under docs/reference (names, sizes, hashes) are allowed.
Input:   the staged files (git diff --cached), or --all for every tracked file.
Output:  exit 0 = clean; exit 1 with the offending files listed.
Deps:    Python 3.8+ standard library, git.
Setup:   setup.ps1 turns it on for your clone (git config core.hooksPath .githooks); by hand:
         git config core.hooksPath .githooks
         A false alarm on a file of ours: rename it or put it in ALLOWED below (with a review).
"""
from __future__ import annotations

import subprocess
import sys

GAME_FOLDERS = ("game/", "mods/", "hd_textures/", "work/", "out/", "states/", "cards/", "saves/", "runs/")
GAME_TYPES = {
    # disc and archives
    ".iso", ".bin", ".img", ".zip", ".pk", ".7z", ".ps2", ".psu", ".max", ".psv", ".ps2state",
    # the game's own formats
    ".itf", ".vbin", ".tdb", ".bdb", ".tmd", ".bmd", ".cs", ".wpb", ".collisionscene", ".fxb", ".vbn",
    ".tlb", ".tla", ".vac", ".ms", ".bt", ".alb_t", ".vag", ".pss", ".m2v", ".ipu", ".irx", ".act", ".acv",
    # the executable
    ".elf",
}
GAME_NAMES = {"slus_206.68", "sles_523.88", "system.cnf"}
MAX_BYTES = 5 * 1024 * 1024
ALLOWED: set[str] = {        # (exact repo paths reviewed as ours)
    "tools/ffmpeg_static/prebuilt/ffmpeg-7.1-tfa-static-msvc-x64.zip",  # FFmpeg's libraries, LGPL (TODO 371: BUILD.txt inside)
}


def git(*args: str) -> str:
    return subprocess.run(["git", *args], check=True, capture_output=True, text=True).stdout


def check(paths: list[str], size_of) -> list[str]:
    bad = []
    for path in paths:
        low = path.lower()
        if low.startswith("port/"):  # (a repository that holds this one under port/)
            low = low[len("port/"):]
        name = low.rsplit("/", 1)[-1]
        ext = "." + name.rsplit(".", 1)[-1] if "." in name else ""
        if path in ALLOWED:
            continue
        if low.startswith(GAME_FOLDERS):
            bad.append(f"{path}  (a folder that holds game data)")
        elif ext in GAME_TYPES or name in GAME_NAMES:
            bad.append(f"{path}  (a game file type: {ext or name})")
        else:
            size = size_of(path)
            if size is not None and size > MAX_BYTES:
                bad.append(f"{path}  ({size / 1048576:.1f} MB: larger than any file of ours)")
    return bad


def main() -> int:
    if "--all" in sys.argv:
        paths = [p for p in git("ls-files", "-z").split("\0") if p]
        sizes = {}
        for line in git("ls-tree", "-r", "-l", "HEAD").splitlines():
            meta, path = line.split("\t", 1)
            size = meta.split()[3]
            sizes[path] = int(size) if size.isdigit() else None
        size_of = sizes.get
    else:
        paths = [p for p in git("diff", "--cached", "--name-only", "--diff-filter=AM", "-z").split("\0") if p]

        def size_of(path: str):
            out = subprocess.run(["git", "cat-file", "-s", f":{path}"], capture_output=True, text=True)
            return int(out.stdout) if out.returncode == 0 else None
    bad = check(paths, size_of)
    if bad:
        print("check_no_game_data: this commit would add game data (or something that looks like it):", file=sys.stderr)
        for b in bad:
            print("  " + b, file=sys.stderr)
        print("Nothing made from the discs may enter the repository. Unstage these (git restore --staged <file>).",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
