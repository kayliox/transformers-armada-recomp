"""Extract your own Transformers disc image for the recompilation.

Copies every file from the ISO 9660 filesystem into <out_dir>, keeping the
disc's folder layout, and checks that the executable is the build of a release
this project has a config for (USA SLUS-20668: config/; others:
config/versions/<serial>/). Nothing from the disc is ever part of the repository.

Usage: python extract_iso.py "Transformers (USA).iso" game
       python extract_iso.py --detect "Transformers (Europe).iso"   (prints the release's serial)
       python extract_iso.py --check "Transformers (USA).iso"    (checks the release's executable inside it, nothing written:
                                                                 exit 0 right, 2 not a known release or a bad copy, 1 unreadable)
       python extract_iso.py --check-elf game/SLUS_206.68      (checks an executable already extracted; the same exit codes)

The executable is checked BEFORE anything is written (TODO 441): a wrong one used to be copied with every other file and
only then refused, so the next run found the game folder filled and built from the wrong executable without checking it again.
"""

import argparse
import struct
import sys
import zlib
from pathlib import Path

import os

from ps2elf import known_versions, load_game_config

SECTOR = 2048


class Iso:
    def __init__(self, path):
        self.f = open(path, "rb")
        pvd = self.read(16, SECTOR)
        if pvd[1:6] != b"CD001" or pvd[0] != 1:
            raise ValueError(f"{path} is not an ISO 9660 image (no primary volume descriptor)")
        self.volume_id = pvd[40:72].decode("ascii", "replace").strip()
        self.root = self.parse_record(pvd[156:156 + 34])

    def read(self, lba, size):
        self.f.seek(lba * SECTOR)
        return self.f.read(size)

    @staticmethod
    def parse_record(r):
        lba = struct.unpack_from("<I", r, 2)[0]
        size = struct.unpack_from("<I", r, 10)[0]
        flags = r[25]
        name = r[33:33 + r[32]]
        return {"lba": lba, "size": size, "dir": bool(flags & 2), "name": name}

    def listdir(self, entry):
        data = self.read(entry["lba"], entry["size"])
        i = 0
        while i < len(data):
            length = data[i]
            if length == 0:                      # records never span sectors
                i = (i // SECTOR + 1) * SECTOR
                continue
            rec = self.parse_record(data[i:i + length])
            i += length
            if rec["name"] in (b"\x00", b"\x01"):
                continue
            rec["name"] = rec["name"].decode("ascii", "replace").split(";")[0].rstrip(".")
            yield rec

    def walk(self, entry=None, prefix=Path()):
        for rec in self.listdir(entry or self.root):
            path = prefix / rec["name"]
            if rec["dir"]:
                yield from self.walk(rec, path)
            else:
                yield path, rec

    def copy(self, rec, dest):
        dest.parent.mkdir(parents=True, exist_ok=True)
        remaining = rec["size"]
        crc = 0
        self.f.seek(rec["lba"] * SECTOR)
        with open(dest, "wb") as out:
            while remaining:
                chunk = self.f.read(min(remaining, 8 << 20))
                if not chunk:
                    raise IOError(f"image ended early while reading {dest.name}")
                crc = zlib.crc32(chunk, crc)
                out.write(chunk)
                remaining -= len(chunk)
        return crc & 0xFFFFFFFF


def wrong_elf_text(crc, cfg, where):
    return (f"{cfg['elf_name']} {where} has the checksum {crc:08x}, not {cfg['elf_crc32']}: this is not the {cfg['serial']} "
            "release the build needs (another version of the game, a modified disc or a bad copy). Make a fresh copy of your disc.")


def check_elf_file(path):
    """(TODO 441) An executable already extracted (the game folder's SLUS_206.68): exit 0 when it is the release's, 2 when not, 1 when
    unreadable. The release: TF_GAME_VERSION, else the one whose executable has this file's name."""
    p = Path(path)
    version = os.environ.get("TF_GAME_VERSION") or next((v for v, elf in known_versions().items() if elf == p.name.upper()), None)
    if not version:
        print(f"{p.name} is not the executable of a release this project knows ({', '.join(known_versions().values())}).")
        return 2
    cfg = load_game_config(version)
    try:
        crc = zlib.crc32(p.read_bytes()) & 0xFFFFFFFF
    except OSError as e:
        print(f"{p} cannot be read ({e}).")
        return 1
    if f"{crc:08x}" != cfg["elf_crc32"]:
        print(wrong_elf_text(crc, cfg, f"in {p.parent}"))
        return 2
    print(f"Transformers ({cfg['region']}, {cfg['serial']}), its executable checked")
    return 0


def image_elf_crc(iso, rec):
    """The CRC-32 of a file on the image, read in 8 MB pieces; None when the image ends before it."""
    crc = 0
    iso.f.seek(rec["lba"] * SECTOR)
    remaining = rec["size"]
    while remaining:
        chunk = iso.f.read(min(remaining, 8 << 20))
        if not chunk:
            return None
        crc = zlib.crc32(chunk, crc)
        remaining -= len(chunk)
    return crc & 0xFFFFFFFF


def check(path):
    """(build robustness) The setup's check before any install: the image is a disc of a known release and its
    executable has that release's checksum. Reads only the directory records and the executable (a few MB)."""
    try:
        iso = Iso(path)
        files = list(iso.walk())
    except Exception as e:
        print(f"{path} cannot be read as a disc image ({e}).")
        return 2 if isinstance(e, ValueError) else 1
    names = {str(p).upper(): rec for p, rec in files}
    version = os.environ.get("TF_GAME_VERSION") or next((v for v, elf in known_versions().items() if elf in names), None)
    if not version:
        print(f"{path} is not a disc of Transformers (2004) this project knows: none of {', '.join(known_versions())} is on it. "
              "The game is built from your own copy of the USA disc (SLUS-20668) or the European disc (SLES-52388).")
        return 2
    cfg = load_game_config(version)
    rec = names.get(cfg["elf_name"])
    if rec is None:
        print(f"{path} does not contain {cfg['elf_name']}: this is not Transformers ({cfg['region']}, {cfg['serial']}).")
        return 2
    try:
        crc = image_elf_crc(iso, rec)
    except OSError as e:
        print(f"{path}: {cfg['elf_name']} cannot be read ({e}).")
        return 1
    if crc is None:
        print(f"{path} ends before the end of {cfg['elf_name']}: the copy is cut short (a bad or unfinished copy of the disc).")
        return 2
    if f"{crc:08x}" != cfg["elf_crc32"]:
        print(wrong_elf_text(crc, cfg, f"on {path}"))
        return 2
    print(f"Transformers ({cfg['region']}, {cfg['serial']}), its executable checked")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("iso", help="your Transformers disc image")
    ap.add_argument("out_dir", nargs="?", help="folder to extract into (e.g. game)")
    ap.add_argument("--force", action="store_true", help="extract even if the executable does not match")
    ap.add_argument("--detect", action="store_true", help="only print the serial of the disc's release")
    ap.add_argument("--check", action="store_true", help="only check the release's executable inside the image (nothing written)")
    ap.add_argument("--check-elf", action="store_true", help="the path given is an extracted executable: check it (nothing written)")
    args = ap.parse_args()

    if args.check:
        sys.exit(check(args.iso))
    if args.check_elf:
        sys.exit(check_elf_file(args.iso))

    iso = Iso(args.iso)
    files = list(iso.walk())
    names = {str(p).upper() for p, _ in files}
    # (the release: TF_GAME_VERSION when set, else the one whose executable is on the disc)
    version = os.environ.get("TF_GAME_VERSION")
    if not version:
        version = next((v for v, elf in known_versions().items() if elf in names), None)
    if args.detect:
        if not version:
            sys.exit(f"{args.iso}: no known Transformers release ({', '.join(known_versions())}).")
        print(version)
        return
    if not args.out_dir:
        ap.error("out_dir is needed")
    cfg = load_game_config(version)
    if cfg["elf_name"] not in names and not args.force:
        sys.exit(f"{args.iso} does not contain {cfg['elf_name']}; this is not Transformers ({cfg['region']}, {cfg['serial']}).")
    # (TODO 441) The executable checked first, on the image: nothing is written for a wrong one. (Copied with every other
    # file and refused only at the end, it stayed in the folder: the next run saw the game folder filled, skipped the disc and built
    # from it without checking it again.)
    rec = next((r for p, r in files if str(p).upper() == cfg["elf_name"]), None)
    if rec is not None and not args.force:
        crc = image_elf_crc(iso, rec)
        if crc is None:
            sys.exit(f"{args.iso} ends before the end of {cfg['elf_name']}: the copy is cut short (a bad or unfinished copy of the disc).")
        if f"{crc:08x}" != cfg["elf_crc32"]:
            sys.exit(wrong_elf_text(crc, cfg, f"on {args.iso}"))

    out = Path(args.out_dir)
    total = sum(r["size"] for _, r in files)
    done = 0
    elf_crc = None
    print(f"Extracting {len(files)} files ({total / 2**30:.2f} GiB) from volume '{iso.volume_id}' to {out}/")
    for path, rec in files:
        dest = out / path
        if dest.exists() and dest.stat().st_size == rec["size"] and str(path).upper() != cfg["elf_name"]:
            done += rec["size"]
            continue
        crc = iso.copy(rec, dest)
        if str(path).upper() == cfg["elf_name"]:
            elf_crc = crc
        done += rec["size"]
        print(f"  [{done * 100 // max(total, 1):3d}%] {path}")

    if elf_crc is not None and f"{elf_crc:08x}" != cfg["elf_crc32"]:
        msg = (f"{cfg['elf_name']} has CRC32 {elf_crc:08x}, expected {cfg['elf_crc32']}. "
               f"This is not the {cfg['serial']} release this build expects.")
        if not args.force:
            # (it read differently from the check above: a failing drive or disc. Not left behind for the next run.)
            (out / cfg["elf_name"]).unlink(missing_ok=True)
            sys.exit(msg)
        print("Warning: " + msg)
    print("Done.")


if __name__ == "__main__":
    main()
