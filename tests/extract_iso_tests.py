"""extract_iso_tests.py - transformers/tools/extract_iso.py's executable check (TODO 441), without a disc image: a small
stand-in image file holds two "files" (the release's executable and one other) at sector offsets, read through the real
Iso class.

A wrong executable must stop the extraction BEFORE anything is written (it used to be copied with every other file and
refused only at the end, so the next run found the game folder filled, skipped the disc and built from it), and an
executable already in the game folder can be checked on its own (--check-elf).

Run: python tests/extract_iso_tests.py   (exit code 0: all passed). Needs no game files and writes only to a temp folder.
"""

import os
import shutil
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "transformers" / "tools"))
import extract_iso  # noqa: E402

failed = 0


def check(ok, what):
    global failed
    print(("  ok    " if ok else "  FAIL  ") + what)
    if not ok:
        failed += 1


SECTOR = extract_iso.SECTOR
ELF = extract_iso.load_game_config("SLUS-20668")["elf_name"]


def make_image(folder, elf_bytes, other_bytes):
    """A stand-in image: the executable at sector 1, another file at sector 4."""
    img = folder / "disc.bin"
    data = bytearray(SECTOR * 8)
    data[SECTOR:SECTOR + len(elf_bytes)] = elf_bytes
    data[4 * SECTOR:4 * SECTOR + len(other_bytes)] = other_bytes
    img.write_bytes(bytes(data))
    return img, [(Path(ELF), {"lba": 1, "size": len(elf_bytes), "dir": False, "name": ELF}),
                 (Path("TFA.ZIP"), {"lba": 4, "size": len(other_bytes), "dir": False, "name": "TFA.ZIP"})]


def run_main(img, files, out, crc):
    """extract_iso.main() on the stand-in image, the release's checksum given (load_game_config stood in for)."""
    class FakeIso(extract_iso.Iso):
        def __init__(self, path):
            self.f = open(path, "rb")
            self.volume_id = "TEST"

        def walk(self, entry=None, prefix=Path()):
            yield from files

    real_iso, real_cfg = extract_iso.Iso, extract_iso.load_game_config

    def cfg(version=None):
        c = real_cfg("SLUS-20668")
        c["elf_crc32"] = f"{crc:08x}"
        return c

    extract_iso.Iso, extract_iso.load_game_config = FakeIso, cfg
    argv = sys.argv
    sys.argv = ["extract_iso.py", str(img), str(out)]
    os.environ["TF_GAME_VERSION"] = "SLUS-20668"
    try:
        extract_iso.main()
        return None
    except SystemExit as e:
        return str(e.code)
    finally:
        sys.argv = argv
        extract_iso.Iso, extract_iso.load_game_config = real_iso, real_cfg
        os.environ.pop("TF_GAME_VERSION", None)


tmp = Path(tempfile.mkdtemp(prefix="tfa_extract_"))
try:
    elf = b"\x7fELF" + bytes(range(256)) * 20
    other = b"PK" + b"z" * 3000
    img, files = make_image(tmp, elf, other)
    good = zlib.crc32(elf) & 0xFFFFFFFF

    out = tmp / "wrong"
    msg = run_main(img, files, out, good ^ 1)
    check(msg is not None and "not the SLUS-20668 release" in msg, "a wrong executable stops the extraction, in plain words")
    check(not out.exists() or not any(out.rglob("*")), "  ...and nothing is written (the next run must not find a filled game folder)")

    out = tmp / "right"
    msg = run_main(img, files, out, good)
    check(msg is None and (out / ELF).read_bytes() == elf and (out / "TFA.ZIP").read_bytes() == other, "the right executable: everything extracted")

    cut = [(files[0][0], dict(files[0][1], size=len(elf) + SECTOR * 20)), files[1]]
    msg = run_main(img, cut, tmp / "cut", good)
    check(msg is not None and "cut short" in msg and not (tmp / "cut").exists(), "an image that ends inside the executable: said, nothing written")

    # --check-elf: an executable already in the game folder
    os.environ["TF_GAME_VERSION"] = "SLUS-20668"
    bad = tmp / "game" / ELF
    bad.parent.mkdir()
    bad.write_bytes(elf)
    check(extract_iso.check_elf_file(bad) == 2, "--check-elf: a game folder's executable that is not the release's: exit 2")
    check(extract_iso.check_elf_file(tmp / "game" / "missing" / ELF) == 1, "--check-elf: an executable that cannot be read: exit 1")
    os.environ.pop("TF_GAME_VERSION", None)
    check(extract_iso.check_elf_file(tmp / "game" / "OTHER.ELF") == 2, "--check-elf: a file named like no release's executable: exit 2")
finally:
    shutil.rmtree(tmp, ignore_errors=True)

if failed:
    print(f"\n{failed} check(s) failed")
    sys.exit(1)
print("\nall checks passed")
