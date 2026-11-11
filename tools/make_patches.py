"""Regenerates patches/ps2recomp/ from the PS2Recomp working tree, and checks the result.

    python tools/make_patches.py             # write the patch files
    python tools/make_patches.py --verify    # check the series already on disk (rewrites nothing)

The submodule is pinned at an upstream commit and carries this project's changes as
uncommitted edits and new files. The patches are that difference, split by area; build.ps1
applies them to a fresh clone. Neither mode touches the submodule's files or its index (new
files are added to a temporary index), so nothing gets rebuilt because of it.

The default invocation is the writer: it rewrites the five area patches (0001-0005) from the
working tree. The hand-written series (the later upstream patches and this port's 1001+) is
checked with --verify, which applies every patch already in patches/ps2recomp/ to a clean
checkout of the pinned commit and compares every changed and new file with the working tree,
line endings aside. --verify never writes or removes a patch.
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

# (file name, paths it covers, paths it leaves to a later group). Every changed file must
# fall into exactly one group.
GROUPS = [
    ("0001-iop-emulator", ["ps2xIOP"], []),
    ("0002-recompiler-fpu-and-vu0-translation", ["ps2xRecomp"], []),
    ("0003-vu1-fast-engine-and-jit", ["ps2xRuntime/src/lib/vu", "ps2xRuntime/include/runtime/ps2_vu1.h"], []),
    ("0004-gs-threaded-software-renderer", ["ps2xRuntime/src/lib/gs", "ps2xRuntime/include/runtime/gs"], []),
    ("0005-runtime-scheduler-hle-save-states", ["ps2xRuntime"],
     ["ps2xRuntime/src/lib/vu", "ps2xRuntime/include/runtime/ps2_vu1.h", "ps2xRuntime/src/lib/gs",
      "ps2xRuntime/include/runtime/gs"]),
]


def git(args, cwd=SUBMODULE, env=None, check=True, binary=False):
    result = subprocess.run(["git", *args], cwd=cwd, env=env, capture_output=True)
    if check and result.returncode != 0:
        sys.exit(f"git {' '.join(args)} failed:\n{result.stderr.decode(errors='replace')}")
    return result.stdout if binary else result.stdout.decode("utf-8", errors="replace")


def changed_files():
    modified = [line for line in git(["diff", "--name-only"]).splitlines() if line]
    untracked = [line for line in git(["ls-files", "--others", "--exclude-standard"]).splitlines() if line]
    return modified, untracked


def covers(path, include, exclude):
    inside = lambda prefixes: any(path == prefix or path.startswith(prefix + "/") for prefix in prefixes)
    return inside(include) and not inside(exclude)


def write_patches():
    modified, untracked = changed_files()
    staged = git(["diff", "--cached", "--name-only"]).strip()
    if staged:
        sys.exit(f"the submodule has staged changes; unstage them first:\n{staged}")
    for path in modified + untracked:
        owners = [name for name, include, exclude in GROUPS if covers(path, include, exclude)]
        if len(owners) != 1:
            sys.exit(f"{path} belongs to {len(owners)} patch groups: {owners}")

    # New files only show up in a diff once the index knows them; use a private copy of it.
    index = ROOT / "out" / "patch_index"
    index.parent.mkdir(parents=True, exist_ok=True)
    if index.exists():
        index.unlink()
    env = dict(os.environ, GIT_INDEX_FILE=str(index))
    git(["read-tree", "HEAD"], env=env)
    if untracked:
        git(["add", "--intent-to-add", "--", *untracked], env=env)

    PATCHES.mkdir(parents=True, exist_ok=True)
    for old in PATCHES.glob("*.patch"):
        old.unlink()
    for name, include, exclude in GROUPS:
        pathspec = include + [f":(exclude){path}" for path in exclude]
        diff = git(["diff", "--no-color", "--no-ext-diff", "--no-renames", "--binary", "--", *pathspec], env=env, binary=True)
        files = diff.count(b"\ndiff --git ") + (1 if diff.startswith(b"diff --git ") else 0)
        if not diff:
            continue
        (PATCHES / f"{name}.patch").write_bytes(diff)
        print(f"{name}.patch: {files} files, {len(diff) // 1024} KB")
    index.unlink()
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
    parser.add_argument("--verify", action="store_true",
                        help="check the existing series against the working tree (does not write it)")
    args = parser.parse_args()
    if args.verify:
        if not PATCHES.is_dir() or not any(PATCHES.glob("*.patch")):
            sys.exit(f"no patches in {PATCHES}")
        modified, untracked = changed_files()
        verify(modified, untracked)
    else:
        write_patches()


if __name__ == "__main__":
    main()
