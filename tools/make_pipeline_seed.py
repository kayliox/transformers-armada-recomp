"""Make transformers/native_render/nr_pipeline_seed.h: the D3D12 pipelines a first run makes ahead (TODO 317).

The native renderer keeps the pipelines a machine has used in its pipeline library (pipelines.bin) and their keys
(pipelines.txt), and makes the known ones ahead on a thread of their own at the next start (TODO 164). A first run knows
none, so on a cold graphics-driver cache every new pipeline is compiled by the driver the moment a draw needs it (on
Intel ~40 ms each: TODO 317's 1.8 s frame). The seed is a list of keys the game uses, built into the exe and made ahead
at the start like the machine's own (the render state's bits only: no game data, nothing machine-specific).

Usage: python tools/make_pipeline_seed.py <pipelines.txt> [<pipelines.txt> ...]
    Merges the keys of the given lists (one 16-digit hex key a line, as the renderer writes them) into the header.
    Gather the lists by playing (or scripted runs) with TF_SHADER_CACHE pointed at one folder: its pipelines.txt grows.
    Each key also goes in with its "picture" bit (24) the other way: whether a draw goes straight into the picture or
    into the GS's buffers depends on the machine's settings (measured: the Testbench at its defaults and the ThinkPad at
    50% render scale used the same draws with the bit the other way), so both kinds are made ahead.
"""
import pathlib
import re
import sys

PICTURE = 1 << 24  # (the key's "picture" bit: nr_d3d.cpp pipelineKey)

HEADER = pathlib.Path(__file__).resolve().parent.parent / "transformers" / "native_render" / "nr_pipeline_seed.h"


def main(paths):
    keys = set()
    for p in paths:
        for line in pathlib.Path(p).read_text(encoding="ascii", errors="ignore").splitlines():
            line = line.strip()
            if re.fullmatch(r"[0-9a-fA-F]{16}", line):
                keys.add(int(line, 16))
    if not keys:
        sys.exit("no keys in " + ", ".join(paths))
    keys |= {k ^ PICTURE for k in keys}
    base = sorted(k for k in keys if (k >> 32) == 0)
    variants = sorted(k for k in keys if (k >> 32) != 0)
    out = [
        "#pragma once",
        "// The pipelines a first run makes ahead (TODO 317): the keys of the pipelines the game used in its levels, the HQ",
        "// and the menus, gathered by tools/make_pipeline_seed.py (which made this file - run it again after a change to",
        "// the pipelines' keys or shaders). Base pipelines first (a draw waits for those), then the shader variants.",
        "#include <cstdint>",
        "",
        "namespace nr::pipeline_seed",
        "{",
        f"    inline constexpr uint64_t kKeys[] = {{ // {len(base)} base, {len(variants)} variants",
    ]
    allkeys = base + variants
    for i in range(0, len(allkeys), 4):
        out.append("        " + ", ".join(f"0x{k:016x}ull" for k in allkeys[i:i + 4]) + ",")
    out += ["    };", "}", ""]
    HEADER.write_text("\n".join(out), encoding="ascii", newline="\n")
    print(f"{HEADER}: {len(base)} base + {len(variants)} variant keys")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1:])
