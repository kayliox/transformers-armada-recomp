#!/usr/bin/env python3
"""Refuses a public snapshot that holds something that must stay private.

Purpose: the public repository holds the static recompilation only: no game data, no machine names or user
         paths, nothing of the team's decompilation, no private overlay (the mods), no save states. This looks
         at every file a snapshot would publish and fails on:
         - the private folders: overlay/ (the mods, the decomp bridge), private/, api/;
         - a game file type or a file over 1 MB (as check_no_game_data.py, stricter on size);
         - an icon (*.ico): the game's icon is made from the player's disc at build time, never committed;
         - a picture anywhere but docs/images/*.jpg (the README's screenshots);
         - text that names a private place or thing: LAN addresses, the test machines and their keys, user
           folders, the decomp repository and its tools (the symbol table, the bridge, its switches), the
           decomp's source paths, the user's save states.
Input:   the tracked files of HEAD (default), the staged files (--staged), or a folder (--tree <dir>, e.g. a
         `git archive` of the snapshot).
Output:  exit 0 = publishable; exit 1 with each finding (file, line, what matched).
Deps:    Python 3.8+ standard library, git.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import re
import subprocess
import sys

PRIVATE_DIRS = ("overlay/", "private/", "api/")
GAME_TYPES = {
    ".iso", ".bin", ".img", ".zip", ".pk", ".7z", ".ps2", ".psu", ".max", ".psv", ".ps2state",
    ".itf", ".vbin", ".tdb", ".bdb", ".tmd", ".bmd", ".cs", ".wpb", ".collisionscene", ".fxb", ".vbn",
    ".tlb", ".tla", ".vac", ".ms", ".bt", ".alb_t", ".vag", ".pss", ".m2v", ".ipu", ".irx", ".act", ".acv",
    ".elf", ".ico",
}
GAME_NAMES = {"slus_206.68", "sles_523.88", "system.cnf"}
# Pictures: only the README's screenshots (docs/images/*.jpg); any other picture may be the game's art.
IMAGE_TYPES = {".png", ".jpg", ".jpeg", ".bmp", ".gif", ".tga", ".dds", ".webp", ".tif", ".tiff"}
IMAGES_ALLOWED_IN = "docs/images/"
MAX_BYTES = 1024 * 1024
# Files of ours or third parties' reviewed as publishable though over 1 MB.
ALLOWED_LARGE = {
    "transformers/native_render/ext/smaa/AreaTex.h",          # SMAA's lookup table (MIT-style licence, LICENSE.txt)
    "patches/ps2recomp/0004-gs-threaded-software-renderer.patch",  # our renderer patch
}
# Third parties' binary files reviewed as publishable (a game file type or over 1 MB), each by its SHA-256.
REVIEWED_BINARIES = {
    # FFmpeg's static libraries for Windows (TODO 371): LGPL 2.1+, built by tools/ffmpeg_static/build.sh from the pinned source
    "tools/ffmpeg_static/prebuilt/ffmpeg-7.1-tfa-static-msvc-x64.zip": "8405647d6c8fdb5cefd35902ddcc89f9416bafe697d4b2d3edd5d2141b225657",
}
# (pattern, what it is) - checked on every text line
PATTERNS = [
    (r"192\.168\.", "a LAN address"),
    (r"[Cc]:[\\/]+Users[\\/]", "a user folder"),
    (r"decomp_bridge|decomp_symbols|bridgegen|\bTF_BRIDGE\b|TF_DECOMP_BRIDGE", "the decomp bridge"),
    (r"\bsrc/(ta|ut|tu|an|vl)\w*/", "a decomp source path"),
    (r"\b[A-Z]{2,3}_PS2\w*\.(cpp|h)\b|\b(sl|vl|ml|ta|ut|an)/\2[A-Z]\w*\.(cpp|h)\b", "a decomp source file"),
    (r"[Dd]ecomp bridge", "the decomp bridge"),
    (r"user_\d+\w*\.ps2state", "a user's save state"),
]
# More names that must never be published (machines, people, other projects) live in the private tree only:
# private/publish_patterns.json, a list of [regex, what] pairs. Absent in the public repository.
_extra = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "private", "publish_patterns.json")
if os.path.isfile(_extra):
    import json
    with open(_extra, encoding="utf-8") as _f:
        PATTERNS += [(str(p), str(w)) for p, w in json.load(_f)]
COMPILED = [(re.compile(p), what) for p, what in PATTERNS]
# Files allowed to name these patterns (this checker itself, the ignore list).
SELF = {"tools/check_publishable.py", ".gitignore"}


def git(*args: str) -> str:
    return subprocess.run(["git", *args], check=True, capture_output=True, text=True, encoding="utf-8").stdout


def is_text(data: bytes) -> bool:
    return b"\0" not in data[:8192]


def check_file(path: str, data: bytes) -> list[str]:
    findings = []
    if path in REVIEWED_BINARIES:  # (a third party's file reviewed for publishing: exactly that file, by its SHA-256)
        if hashlib.sha256(data).hexdigest() != REVIEWED_BINARIES[path]:
            findings.append(f"{path}: not the reviewed file (its SHA-256 differs)")
        return findings
    low = path.lower()
    name = low.rsplit("/", 1)[-1]
    ext = "." + name.rsplit(".", 1)[-1] if "." in name else ""
    if low.startswith(PRIVATE_DIRS):
        findings.append(f"{path}: a private folder")
    if ext in GAME_TYPES or name in GAME_NAMES:
        findings.append(f"{path}: not publishable ({ext or name})")
    if ext in IMAGE_TYPES and not (low.startswith(IMAGES_ALLOWED_IN) and ext == ".jpg"):
        findings.append(f"{path}: a picture outside docs/images/*.jpg ({ext})")
    if len(data) > MAX_BYTES and path not in ALLOWED_LARGE:
        findings.append(f"{path}: {len(data) / 1048576:.1f} MB (over 1 MB)")
    if path in SELF or not is_text(data):
        return findings
    text = data.decode("utf-8", errors="replace")
    for number, line in enumerate(text.splitlines(), 1):
        for pattern, what in COMPILED:
            m = pattern.search(line)
            if m:
                findings.append(f"{path}:{number}: {what} ({m.group(0)!r})")
    return findings


def files_of_head() -> list[tuple[str, bytes]]:
    out = []
    for path in git("ls-files", "-z").split("\0"):
        if not path or path.startswith("PS2Recomp/") or path == "PS2Recomp":
            continue
        try:
            with open(path, "rb") as f:
                out.append((path, f.read()))
        except OSError:
            continue
    return out


def files_staged() -> list[tuple[str, bytes]]:
    out = []
    for path in git("diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z").split("\0"):
        if path and path != "PS2Recomp":
            out.append((path, subprocess.run(["git", "show", f":{path}"], capture_output=True).stdout))
    return out


def files_of_tree(root: str) -> list[tuple[str, bytes]]:
    out = []
    for folder, dirs, names in os.walk(root):
        dirs[:] = [d for d in dirs if d not in (".git", "PS2Recomp")]
        for n in names:
            full = os.path.join(folder, n)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            with open(full, "rb") as f:
                out.append((rel, f.read()))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    group = ap.add_mutually_exclusive_group()
    group.add_argument("--staged", action="store_true", help="the staged files (the pre-commit hook)")
    group.add_argument("--tree", metavar="DIR", help="a folder: a snapshot's files")
    args = ap.parse_args()
    files = files_staged() if args.staged else files_of_tree(args.tree) if args.tree else files_of_head()
    findings = []
    for path, data in files:
        findings.extend(check_file(path, data))
    if findings:
        print(f"Not publishable: {len(findings)} finding(s) in {len(files)} file(s)")
        for f in findings:
            print("  " + f)
        return 1
    print(f"Publishable: {len(files)} file(s) checked")
    return 0


if __name__ == "__main__":
    sys.exit(main())
