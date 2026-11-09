"""Minimal PS2 ELF loader and R5900 control-flow decoder shared by the tools."""

import json
import os
import struct
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# The release being built (build.ps1 sets TF_GAME_VERSION from the disc): the USA release's
# settings are in config/, every other release's in config/versions/<serial>/.
DEFAULT_VERSION = "SLUS-20668"


def game_version():
    return os.environ.get("TF_GAME_VERSION") or DEFAULT_VERSION


def config_dir(version=None):
    v = version or game_version()
    return ROOT / "config" if v == DEFAULT_VERSION else ROOT / "config" / "versions" / v


def known_versions():
    """Every release with a config: {serial: elf_name}."""
    found = {DEFAULT_VERSION: json.loads((ROOT / "config" / "game.json").read_text())["elf_name"]}
    for p in sorted((ROOT / "config" / "versions").glob("*/game.json")):
        found[p.parent.name] = json.loads(p.read_text())["elf_name"]
    return found


GAME_JSON = config_dir() / "game.json"

JR_RA = 0x03E00008


def load_game_config(version=None):
    cfg = json.loads((config_dir(version) / "game.json").read_text())
    cfg["code_regions"] = [(int(a, 16), int(b, 16)) for a, b in cfg["code_regions"]]
    cfg["code_shift"] = [(int(a, 16), int(b, 16), d) for a, b, d in cfg.get("code_shift", [])]
    return cfg


def release_file(name, version=None):
    """A release's own config file (config/versions/<serial>/<name>), else the USA release's: another release uses
    the USA overrides.toml and scratchpad_hot.txt with their code addresses moved by code_address()."""
    own = config_dir(version) / name
    return own if own.exists() else config_dir(DEFAULT_VERSION) / name


def code_address(usa, cfg):
    """A code address of the USA release, as the release of cfg has it (game.json "code_shift": [first, last, delta]
    ranges of the USA main code region; src/game_version.h holds the same steps for the C++). None when it lies
    between two ranges, where the releases' code differs and the address needs checking by hand."""
    shifts = cfg.get("code_shift")
    if not shifts or usa < shifts[0][0] or usa > shifts[-1][1]:
        return usa
    for lo, hi, d in shifts:
        if lo <= usa <= hi:
            return usa + d
    return None


class Elf:
    def __init__(self, path):
        self.path = Path(path)
        self.data = self.path.read_bytes()
        d = self.data
        if d[:4] != b"\x7fELF":
            raise ValueError(f"{path} is not an ELF file")
        self.crc32 = zlib.crc32(d) & 0xFFFFFFFF
        self.entry, phoff = struct.unpack_from("<II", d, 24)
        phnum = struct.unpack_from("<H", d, 44)[0]
        self.segments = []
        for i in range(phnum):
            ptype, off, vaddr, _, filesz, memsz, _, _ = struct.unpack_from("<8I", d, phoff + 32 * i)
            if ptype == 1 and filesz:
                self.segments.append((vaddr, off, filesz, memsz))
        # The game has one loadable segment holding all code and data.
        self.base, off, filesz, _ = max(self.segments, key=lambda s: s[2])
        self.image = d[off:off + filesz]
        self.end = self.base + filesz
        self.words = struct.unpack_from(f"<{filesz // 4}I", self.image)

    def contains(self, addr):
        return self.base <= addr < self.end

    def word(self, addr):
        return self.words[(addr - self.base) >> 2]


def sext16(x):
    x &= 0xFFFF
    return x - 0x10000 if x & 0x8000 else x


# Opcodes that do not exist on the EE core. Seeing one means we are decoding data.
_INVALID_OPS = {0x13, 0x1D, 0x30, 0x32, 0x34, 0x35, 0x38, 0x3A, 0x3B, 0x3C, 0x3D}
_INVALID_SPECIAL = {0x01, 0x05, 0x0E, 0x15, 0x1C, 0x1D, 0x1E, 0x1F, 0x35, 0x37, 0x39, 0x3D}


def is_valid(w):
    op = w >> 26
    if op in _INVALID_OPS:
        return False
    if op == 0 and (w & 0x3F) in _INVALID_SPECIAL:
        return False
    if op == 1 and ((w >> 16) & 0x1F) not in {0, 1, 2, 3, 8, 9, 10, 11, 12, 14, 0x10, 0x11, 0x12, 0x13, 0x18, 0x19}:
        return False
    return True


# Control-flow kinds returned by decode_cf().
NORMAL, CALL, JUMP, BRANCH, UNCOND, JR, JALR, STOP = range(8)


def decode_cf(w, pc):
    """Return (kind, target) for the instruction word `w` at `pc`."""
    op = w >> 26
    rs = (w >> 21) & 31
    rt = (w >> 16) & 31
    btarget = (pc + 4 + (sext16(w) << 2)) & 0xFFFFFFFF
    if op == 0:
        fn = w & 0x3F
        if fn == 0x08:
            return JR, rs
        if fn == 0x09:
            return JALR, rs
        return NORMAL, None
    if op == 1:
        if rt in (0x10, 0x11, 0x12, 0x13):  # BLTZAL/BGEZAL(L): calls
            return CALL, btarget
        if rt in (0, 1, 2, 3):
            if rs == 0 and rt == 1:  # bgez $zero == unconditional b
                return UNCOND, btarget
            return BRANCH, btarget
        return NORMAL, None
    if op == 2:
        return JUMP, ((pc + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
    if op == 3:
        return CALL, ((pc + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
    if op in (4, 0x14):
        if rs == 0 and rt == 0:
            return UNCOND, btarget
        return BRANCH, btarget
    if op in (5, 6, 7, 0x15, 0x16, 0x17):
        return BRANCH, btarget
    if op in (0x10, 0x11, 0x12) and rs == 8:  # BC0x / BC1x / BC2x
        return BRANCH, btarget
    if w == 0x42000018:  # ERET
        return STOP, None
    return NORMAL, None
