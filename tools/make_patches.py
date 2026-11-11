"""Checks that patches/ps2recomp/ still adds up to the PS2Recomp working tree.

    python tools/make_patches.py           # apply the patches to a clean checkout and compare
    python tools/make_patches.py --verify  # the same (the flag is kept for older instructions)

The submodule is pinned at an upstream commit and carries this project's changes as
uncommitted edits and new files. The patches in patches/ps2recomp/ are that difference, split
by area - upstream's 0001-0166 and this port's 1001+ - and are written by hand, one file per
change: this tool does not write them (an older version regenerated five files from a table of
areas and would now overwrite the whole series). build.ps1 and build.sh apply them in sorted
order to a fresh clone.

--verify checks out the pinned commit into out/patch_verify, applies every patch there and
compares every changed and new file with the working tree (line endings aside). Neither mode
touches the submodule's files or its index, so nothing gets rebuilt because of it.
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SUBMODULE = ROOT / "PS2Recomp"
PATCHES = ROOT / "patches" / "ps2recomp"
VERIFY_DIR = ROOT / "out" / "patch_verify"


def git(args, cwd=SUBMODULE, env=None, check=True, binary=False):
    result = subprocess.run(["git", *args], cwd=cwd, env=env, capture_output=True)
    if check and result.returncode != 0:
        sys.exit(f"git {' '.join(args)} failed:\n{result.stderr.decode(errors='replace')}")
    return result.stdout if binary else result.stdout.decode("utf-8", errors="replace")


def changed_files():
    modified = [line for line in git(["diff", "--name-only"]).splitlines() if line]
    untracked = [line for line in git(["ls-files", "--others", "--exclude-standard"]).splitlines() if line]
    return modified, untracked


def normalized(path):
    return path.read_bytes().replace(b"\r\n", b"\n")


def verify(modified, untracked):
    if VERIFY_DIR.exists():
        git(["worktree", "remove", "--force", str(VERIFY_DIR)], check=False)
        shutil.rmtree(VERIFY_DIR, ignore_errors=True)
    git(["worktree", "prune"])
    git(["worktree", "add", "--detach", str(VERIFY_DIR), "HEAD"])
    stripped = VERIFY_DIR.parent / "patch_apply"  # (some patches were written on Windows with CRLF lines)
    shutil.rmtree(stripped, ignore_errors=True)
    stripped.mkdir(parents=True)
    try:
        for patch in sorted(PATCHES.glob("*.patch")):
            target = stripped / patch.name
            target.write_bytes(patch.read_bytes().replace(b"\r\n", b"\n"))
            git(["apply", "--whitespace=nowarn", str(target)], cwd=VERIFY_DIR)
        different = [path for path in modified + untracked
                     if not (VERIFY_DIR / path).is_file() or normalized(VERIFY_DIR / path) != normalized(SUBMODULE / path)]
        there_modified = {line for line in git(["diff", "--name-only"], cwd=VERIFY_DIR).splitlines() if line}
        there_new = {line for line in git(["ls-files", "--others", "--exclude-standard"], cwd=VERIFY_DIR).splitlines() if line}
        extra = sorted((there_modified | there_new) - set(modified) - set(untracked))
        if different or extra:
            sys.exit(f"patched checkout differs from the working tree:\n  differing: {different}\n  unexpected: {extra}")
        print(f"verified: {len(modified)} changed and {len(untracked)} new files match the working tree")
    finally:
        git(["worktree", "remove", "--force", str(VERIFY_DIR)], check=False)
        shutil.rmtree(VERIFY_DIR, ignore_errors=True)
        git(["worktree", "prune"], check=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--verify", action="store_true", help="kept for older instructions: verifying is all it does now")
    parser.parse_args()
    if not PATCHES.is_dir() or not any(PATCHES.glob("*.patch")):
        sys.exit(f"no patches in {PATCHES}")
    modified, untracked = changed_files()
    verify(modified, untracked)


if __name__ == "__main__":
    main()
