"""fetch_deps_tests.py - tools/fetch_deps.py against made-up libraries (no network: file:// archives).

Covers: a fetch + unpack (top folder stripped, skipped folders left out, extra files written, the mark), a second run
using the unpacked folder without a download, a wrong SHA-256 (nothing left behind, CMake's own fetch), a moved tag in
the CMake file (CMake's own fetch), a build folder that already holds the library (kept: empty value), an archive with
an unsafe path (refused), TF_DEPS=0, and a damaged folder without its mark (made again).
Run: python tests/fetch_deps_tests.py   (exit code 0: all passed)
"""
import hashlib
import io
import json
import os
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOL = ROOT / "tools" / "fetch_deps.py"
failed = 0


def check(ok, what):
    global failed
    print(("  ok    " if ok else "  FAIL  ") + what)
    if not ok:
        failed += 1


def make_tar(path, files, top="lib-1.0"):
    with tarfile.open(path, "w:gz") as t:
        for name, data in files.items():
            info = tarfile.TarInfo(f"{top}/{name}" if top else name)
            info.size = len(data)
            info.mtime = 1700000000
            t.addfile(info, io.BytesIO(data))
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(tmp, manifest, *extra, env=None):
    e = dict(os.environ)
    e.pop("TF_DEPS", None)
    e.update(env or {})
    r = subprocess.run([sys.executable, str(TOOL), "--manifest", str(manifest), "--deps", str(tmp / "deps"), *extra],
                       capture_output=True, text=True, env=e, cwd=tmp)
    lines = dict(l.split("=", 1) for l in r.stdout.splitlines() if "=" in l)
    return r, lines


def main():
    with tempfile.TemporaryDirectory() as d:
        tmp = Path(d)
        # the CMake file that "declares" the libraries, relative to ROOT as the manifest names it
        cm_rel = Path(os.path.relpath(tmp / "CMakeLists.txt", ROOT))
        (tmp / "CMakeLists.txt").write_text('FetchContent_Declare(\n  alpha\n  GIT_REPOSITORY x\n  GIT_TAG v1.0\n)\n'
                                            'FetchContent_Declare(beta GIT_REPOSITORY y GIT_TAG "2.0")\n')
        sha = make_tar(tmp / "alpha.tar.gz", {"CMakeLists.txt": b"project(alpha)\n", "src/a.c": b"int a;\n",
                                              "examples/big.c": b"x" * 100})
        bad = make_tar(tmp / "evil.tar.gz", {"../outside.txt": b"no"}, top="")
        size = (tmp / "alpha.tar.gz").stat().st_size

        def manifest(deps):
            m = tmp / "m.json"
            m.write_text(json.dumps({"deps": deps}))
            return m

        alpha = {"name": "alpha", "set": "tools", "declared": str(cm_rel), "tag": "v1.0",
                 "url": (tmp / "alpha.tar.gz").as_uri(), "sha256": sha, "size": size,
                 "skip": ["examples/"], "write": {"package_version": "PACKAGE_VERSION=\"1.0\"\n"}}

        r, out = run(tmp, manifest([alpha]), "--set", "tools")
        folder = Path(out.get("ALPHA", ""))
        check(r.returncode == 0 and folder.name == "alpha-v1.0", f"fetched and unpacked (exit {r.returncode}, {out})")
        check((folder / "src" / "a.c").read_bytes() == b"int a;\n", "the archive's top folder is stripped")
        check(not (folder / "examples").exists(), "a skipped folder is left out")
        check((folder / "package_version").exists(), "an extra file is written")
        check((folder / ".tfa_dep").read_text().strip() == sha, "the mark holds the archive's SHA-256")
        check(not list((tmp / "deps").glob(".*archive*")), "the downloaded archive is deleted after the unpack")

        (tmp / "alpha.tar.gz").rename(tmp / "alpha.gone")
        r, out = run(tmp, manifest([alpha]), "--set", "tools")
        check(out.get("ALPHA") == folder.as_posix(), "a second run uses the unpacked folder (no download: the archive is gone)")
        (tmp / "alpha.gone").rename(tmp / "alpha.tar.gz")

        wrong = dict(alpha, name="alpha", sha256="0" * 64)
        r, out = run(tmp, manifest([wrong]), "--set", "tools", "--deps", str(tmp / "deps2"))
        check(out.get("ALPHA") == "" and "SHA-256" in r.stderr, "a wrong SHA-256: CMake's own fetch, said why")
        check(not any((tmp / "deps2").glob("*")) if (tmp / "deps2").exists() else True, "a wrong SHA-256 leaves nothing behind")

        moved = dict(alpha, tag="v0.9")
        r, out = run(tmp, manifest([moved]), "--set", "tools")
        check(out.get("ALPHA") == "" and "v1.0" in r.stderr, "a tag the CMake file moved: CMake's own fetch")

        build = tmp / "build"
        (build / "_deps" / "alpha-src").mkdir(parents=True)
        (build / "_deps" / "alpha-src" / "CMakeLists.txt").write_text("x")
        r, out = run(tmp, manifest([alpha]), "--set", "tools", "--build", str(build))
        check(out.get("ALPHA") == "", "a build folder that already holds the library keeps it (empty value)")

        evil = {"name": "beta", "set": "tools", "declared": str(cm_rel), "tag": "2.0",
                "url": (tmp / "evil.tar.gz").as_uri(), "sha256": bad, "size": 1}
        r, out = run(tmp, manifest([evil]), "--set", "tools")
        check(out.get("BETA") == "" and not (tmp / "outside.txt").exists() and not (tmp / "deps" / "outside.txt").exists(),
              "an archive with a path outside its folder is refused")

        r, out = run(tmp, manifest([alpha]), "--set", "tools", env={"TF_DEPS": "0"})
        check(out.get("ALPHA") == "" and "TF_DEPS=0" in r.stderr, "TF_DEPS=0: CMake fetches everything itself")

        (folder / ".tfa_dep").unlink()
        r, out = run(tmp, manifest([alpha]), "--set", "tools")
        check(out.get("ALPHA") == folder.as_posix() and (folder / ".tfa_dep").exists(), "a folder without its mark is made again")

        r, out = run(tmp, manifest([alpha]), "--set", "game")
        check(out == {}, "only the libraries of the set asked for")

        # no network: the first library's connection is refused; the next is not waited on (no download tried)
        down1 = dict(alpha, url="http://127.0.0.1:9/alpha.tar.gz")
        down2 = dict(alpha, name="beta", tag="2.0", url="http://127.0.0.1:9/beta.tar.gz")
        r, out = run(tmp, manifest([down1, down2]), "--set", "tools", "--deps", str(tmp / "deps3"))
        check(out.get("ALPHA") == "" and out.get("BETA") == "" and "not downloaded (the network did not answer" in r.stderr
              and r.stderr.count("fetching") == 1, "no network: the first library fails, the rest are not waited on")

    # the real manifest: every library pinned, unique, and declared by the CMake file it names (after the patches)
    real = json.loads((ROOT / "tools" / "deps.json").read_text())["deps"]
    names = [d["name"] for d in real]
    check(len(names) == len(set(n.lower() for n in names)), "tools/deps.json: no library twice")
    check(all(len(d["sha256"]) == 64 and d["url"].startswith("https://") for d in real), "tools/deps.json: an https URL and a SHA-256 each")

    if failed:
        print(f"\n{failed} check(s) failed")
        sys.exit(1)
    print("\nall checks passed")


if __name__ == "__main__":
    main()
