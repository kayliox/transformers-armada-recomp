"""Fetch the libraries the build uses, once, as pinned archives checked by SHA-256 (no git, no network in CMake).

Purpose: CMake's FetchContent clones each library with git during the configure: it needs GitHub, the player's git
settings and long paths at that moment, and it keeps each library's whole git history (about 1 GB in all). This
script downloads each library's source archive named in tools/deps.json into out/deps, checks its SHA-256, unpacks
it (atomically: a cut download or unpack leaves nothing that looks finished) and prints, per library, the folder
CMake is pointed at (FETCHCONTENT_SOURCE_DIR_<NAME>) - or nothing, and CMake then fetches it itself as before.

Input:  tools/deps.json (the pinned list), the CMake files that declare each library (their GIT_TAG must still be
        the pinned one), --set tools|game, --build <the build folder CMake configures>.
Output: out/deps/<name>-<tag>/ (shared by every build folder), on stdout one line per library: NAME=<folder>
        (empty after "=": CMake's own fetch). Messages for the log go to stderr.
Needs:  Python 3.8+ (stdlib only). Honours HTTPS_PROXY and Windows' proxy setting (urllib).

  python tools/fetch_deps.py --set tools --build out/tools
  python tools/fetch_deps.py --verify          # download every archive into a temp folder and check its hash
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import sys
import tarfile
import tempfile
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MARK = ".tfa_dep"           # in a finished folder: the archive's SHA-256
ATTEMPTS = 3
TIMEOUT = 60


def say(text):
    print(f"deps: {text}", file=sys.stderr, flush=True)


def long_path(p):
    """Windows' MAX_PATH (260) does not apply to a \\\\?\\ path: some libraries nest deep under a long install folder."""
    p = os.path.abspath(str(p))
    if os.name == "nt" and not p.startswith("\\\\?\\"):
        return "\\\\?\\" + p
    return p


def declared_tag(cmake_file, name):
    """The GIT_TAG the CMake file declares for this library now, or None."""
    try:
        text = Path(cmake_file).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    m = re.search(r"FetchContent_Declare\(\s*" + re.escape(name) + r"\b(.*?)\)", text, re.S | re.I)
    if not m:
        return None
    t = re.search(r"GIT_TAG\s+\"?([^\s\")]+)", m.group(1))
    return t.group(1) if t else None


def download(url, dest, sha256, size):
    """Into dest.part, renamed to dest only once whole and its hash is right. Tried ATTEMPTS times.
    Returns (None, False) when done, else (why, network): network when it never got an answer (no connection, a
    timeout) rather than a wrong file, so the libraries after it are not waited on one by one (TIMEOUT x ATTEMPTS)."""
    part = Path(str(dest) + ".part")
    last = None
    network = False
    for attempt in range(1, ATTEMPTS + 1):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "tfa-build-deps"})
            h = hashlib.sha256()
            got = 0
            with urllib.request.urlopen(req, timeout=TIMEOUT) as r, open(part, "wb") as f:
                while True:
                    chunk = r.read(1 << 20)
                    if not chunk:
                        break
                    f.write(chunk)
                    h.update(chunk)
                    got += len(chunk)
            if h.hexdigest() != sha256:
                raise ValueError(f"its SHA-256 is {h.hexdigest()}, not the pinned {sha256} ({got} bytes, {size} expected)")
            os.replace(part, dest)
            return None, False
        except Exception as e:  # (network, proxy, TLS, a changed archive: all said, then tried again)
            last = f"{type(e).__name__}: {e}"
            network = not isinstance(e, (ValueError, urllib.error.HTTPError))
            try:
                part.unlink()
            except OSError:
                pass
            if attempt < ATTEMPTS:
                time.sleep(2 * attempt)
    return last, network


def safe_members(names):
    for n in names:
        if n.startswith(("/", "\\")) or ".." in Path(n).parts or ":" in n:
            raise ValueError(f"unsafe path in the archive: {n}")


def unpack(archive, dest, skip, write):
    """Into a fresh folder beside dest, the archive's top folder stripped, then renamed to dest."""
    tmp = Path(tempfile.mkdtemp(prefix=dest.name + ".", suffix=".tmp", dir=dest.parent))
    try:
        skip = tuple(skip or ())
        if zipfile.is_zipfile(archive):
            with zipfile.ZipFile(archive) as z:
                names = z.namelist()
                safe_members(names)
                for n in names:
                    rel = n.split("/", 1)[1] if "/" in n else ""
                    if not rel or rel.endswith("/") or rel.startswith(skip):
                        continue
                    out = Path(long_path(tmp / rel))
                    out.parent.mkdir(parents=True, exist_ok=True)
                    with z.open(n) as s, open(out, "wb") as d:
                        shutil.copyfileobj(s, d)
        else:
            with tarfile.open(archive) as t:
                members = t.getmembers()
                safe_members([m.name for m in members])
                for m in members:
                    rel = m.name.split("/", 1)[1] if "/" in m.name else ""
                    if not rel or rel.startswith(skip):
                        continue
                    out = Path(long_path(tmp / rel))
                    if m.isdir():
                        out.mkdir(parents=True, exist_ok=True)
                    elif m.isfile():
                        out.parent.mkdir(parents=True, exist_ok=True)
                        with t.extractfile(m) as s, open(out, "wb") as d:
                            shutil.copyfileobj(s, d)
                        # (the file's own time, as a git checkout would have a fixed one: never newer than a build)
                        os.utime(out, (m.mtime, m.mtime))
                    # (links and devices: none in these archives; skipped if one ever appears)
        for rel, text in (write or {}).items():
            (tmp / rel).write_text(text, encoding="utf-8", newline="\n")
        (tmp / MARK).write_text(sha256_of(archive) + "\n", encoding="ascii")
        try:
            os.replace(tmp, dest)
        except OSError:
            # (another build unpacked it at the same time: theirs is used, ours goes)
            if not (dest / MARK).exists():
                raise
        tmp = None
    finally:
        if tmp is not None and tmp.exists():
            shutil.rmtree(long_path(tmp), ignore_errors=True)


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def ensure(dep, deps_dir, may_download=True):
    """The library's folder, fetched and unpacked if needed; (folder or None, why not, the network did not answer)."""
    folder = deps_dir / f"{dep['name'].lower()}-{dep['tag']}"
    mark = folder / MARK
    if mark.exists() and mark.read_text(encoding="ascii").strip() == dep["sha256"]:
        return folder, None, False
    if not may_download:
        return None, "not downloaded (the network did not answer for the libraries before it)", False
    if folder.exists():
        # (a folder without its mark: an unpack from before this script finished it, or damaged: made again)
        shutil.rmtree(long_path(folder), ignore_errors=True)
    deps_dir.mkdir(parents=True, exist_ok=True)
    archive = deps_dir / f".{dep['name'].lower()}-{dep['tag']}.archive"
    say(f"fetching {dep['name']} {dep['tag']} ({dep['size'] / 1e6:.1f} MB)")
    why, network = download(dep["url"], archive, dep["sha256"], dep["size"])
    if why:
        return None, f"could not be downloaded from {dep['url']} ({why})", network
    try:
        unpack(archive, folder, dep.get("skip"), dep.get("write"))
    except Exception as e:
        return None, f"could not be unpacked ({type(e).__name__}: {e})", False
    finally:
        try:
            archive.unlink()
        except OSError:
            pass
    return folder, None, False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--set", choices=["tools", "game"], help="the libraries of this build")
    ap.add_argument("--build", help="the build folder CMake configures (a library it already holds is kept as it is)")
    ap.add_argument("--deps", default=str(ROOT / "out" / "deps"), help="where the libraries go (default out/deps)")
    ap.add_argument("--manifest", default=str(ROOT / "tools" / "deps.json"))
    ap.add_argument("--verify", action="store_true", help="download every archive into a temp folder and check it")
    args = ap.parse_args()

    deps = json.loads(Path(args.manifest).read_text(encoding="utf-8"))["deps"]

    if args.verify:
        bad = 0
        with tempfile.TemporaryDirectory() as d:
            for dep in deps:
                why, _ = download(dep["url"], Path(d) / "a", dep["sha256"], dep["size"])
                tag = declared_tag(ROOT / dep["declared"], dep["name"])
                tag_ok = tag in (dep["tag"], None)
                note = "" if tag == dep["tag"] else ("  (not declared in this tree: the PS2Recomp patches add it)" if tag is None else f"  (CMake now declares {tag})")
                print(f"{dep['name']:14} {'ok' if not why else 'FAILED: ' + why}{note}")
                bad += bool(why) or not tag_ok
        sys.exit(1 if bad else 0)

    if not args.set:
        ap.error("--set is needed")
    if os.environ.get("TF_DEPS", "") == "0":
        say("TF_DEPS=0: CMake fetches the libraries itself")
        for dep in (d for d in deps if d["set"] == args.set):
            print(f"{dep['name'].upper()}=")
        return
    deps_dir = Path(args.deps)
    build = Path(args.build) if args.build else None
    online = True
    for dep in (d for d in deps if d["set"] == args.set):
        name = dep["name"]
        key = name.upper()
        if build and (build / "_deps" / f"{name.lower()}-src").is_dir() and any((build / "_deps" / f"{name.lower()}-src").iterdir()):
            # (fetched by CMake in an earlier build: kept as it is - a new folder would compile everything that uses it again)
            print(f"{key}=")
            continue
        tag = declared_tag(ROOT / dep["declared"], name)
        if tag != dep["tag"]:
            say(f"{name}: CMake declares {tag}, tools/deps.json pins {dep['tag']}: CMake fetches it itself")
            print(f"{key}=")
            continue
        folder, why, network = ensure(dep, deps_dir, online)
        online = online and not network
        if folder is None:
            say(f"{name} {why}: CMake fetches it itself")
            print(f"{key}=")
        else:
            print(f"{key}={folder.as_posix()}")


if __name__ == "__main__":
    main()
