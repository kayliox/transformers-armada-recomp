"""Install the recompiler's output so rebuilds only recompile what changed.

ps2_recomp rewrites every file on each run. This tool copies its output from a
staging folder into the build's source folder and:

  * leaves files whose content did not change untouched (timestamps kept), so the
    build system skips them;
  * replaces the include of the generated all-functions header with forward
    declarations of just the functions each file names, so adding a function
    elsewhere does not invalidate every file;
  * groups the function files into batch files by fixed address ranges, so a new
    or changed function only rebuilds the batch that covers its address;
  * puts the functions of transformers/config/scratchpad_hot.txt between
    scratchpad_hot_begin.inc and _end.inc in their batch: their loads and stores test
    the scratchpad inline (transformers/src/scratchpad_hot.h, TODO 94).

Usage: python sync_generated.py <staging_dir> <generated_dir> <batch_dir>
"""

import hashlib
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

BUCKET_SHIFT = 14                     # 16 KB of guest code per batch
FUNC_FILE = re.compile(r"_0x([0-9a-f]+)\.cpp$")
# (a function called, or handed on as an argument: Ps2CallKnown(..., name) - FPS round 3; not the
# emitter's branch_taken_0x... locals, nor an inline leaf's own static functions leaf_<name> and
# Ps2CallLeaf_<name>, defined in the file - FPS round 4)
FUNC_CALL = re.compile(rb"\b(?!branch_taken_|leaf_|Ps2CallLeaf_)([A-Za-z_][A-Za-z0-9_]*_0x[0-9a-f]+)\s*[(),]")
# (a function's address: an inline leaf's test that its table entry is still the generated function - FPS round 4)
FUNC_ADDR = re.compile(rb"&([A-Za-z_][A-Za-z0-9_]*_0x[0-9a-f]+)\b")
# The recompiler writes CRLF on Windows.
SHARED_HEADERS = re.compile(rb"#include <ps2_recompiled_(?:functions|stubs)\.h>\r?\n")
RUNTIME_INCLUDE = re.compile(rb'#include "ps2_runtime\.h"\r?\n')
DECL = b"void %s(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime);\n"
# Files that need the full generated headers.
KEEP_AS_IS = {"register_functions.cpp"}


def transform(name, data):
    """Swap the shared generated headers for per-file forward declarations."""
    if name in KEEP_AS_IS or not name.endswith(".cpp"):
        return data
    own = name[:-4].encode()
    others = sorted({m for m in FUNC_CALL.findall(data) + FUNC_ADDR.findall(data) if m != own})
    data = SHARED_HEADERS.sub(b"", data)
    if not others:
        return data
    decls = b"".join(DECL % m for m in others)
    m = RUNTIME_INCLUDE.search(data)
    if m:
        return data[:m.end()] + decls + data[m.end():]
    return b"#include <cstdint>\nstruct R5900Context;\nclass PS2Runtime;\n" + decls + data


def scratchpad_hot():
    """The function names of transformers/config/scratchpad_hot.txt (TODO 94)."""
    from ps2elf import config_dir
    path = config_dir() / "scratchpad_hot.txt"
    names = set()
    if path.exists():
        for line in path.read_text().splitlines():
            line = line.split("#", 1)[0].strip()
            if line:
                names.add(line)
    return names


def write_if_changed(path, data):
    if path.exists() and path.read_bytes() == data:
        return False
    path.write_bytes(data)
    return True


def main():
    staging, generated, batches = (Path(p) for p in sys.argv[1:4])
    generated.mkdir(parents=True, exist_ok=True)
    batches.mkdir(parents=True, exist_ok=True)
    cache_path = generated / ".sync_cache.json"
    try:
        cache = json.loads(cache_path.read_text())
    except (OSError, ValueError):
        cache = {}

    new_cache = {}
    changed = 0
    names = []
    for src in sorted(staging.iterdir()):
        if not src.is_file():
            continue
        name = src.name
        names.append(name)
        raw = src.read_bytes()
        digest = hashlib.sha1(raw).hexdigest()
        new_cache[name] = digest
        dest = generated / name
        if cache.get(name) == digest and dest.exists():
            continue
        if write_if_changed(dest, transform(name, raw)):
            changed += 1

    current = set(names)
    removed = 0
    for old in generated.iterdir():
        if old.is_file() and old.name != cache_path.name and old.name not in current:
            old.unlink()
            removed += 1
    cache_path.write_text(json.dumps(new_cache))

    # Batches by guest address.
    buckets = defaultdict(list)
    for name in names:
        m = FUNC_FILE.search(name)
        if m and name not in KEEP_AS_IS:
            buckets[int(m.group(1), 16) >> BUCKET_SHIFT].append(name)
    wanted = set()
    batch_changed = 0
    rel = Path("..") / generated.name
    hot = scratchpad_hot()
    hot_found = 0

    def include(f):
        nonlocal hot_found
        line = f'#include "{(rel / f).as_posix()}"\n'
        if f.rsplit("_0x", 1)[0] in hot:
            hot_found += 1
            return '#include "scratchpad_hot_begin.inc"\n' + line + '#include "scratchpad_hot_end.inc"\n'
        return line

    for bucket, files in sorted(buckets.items()):
        batch = batches / f"batch_{bucket << BUCKET_SHIFT:08x}.cpp"
        wanted.add(batch.name)
        body = "".join(include(f) for f in sorted(files))
        batch_changed += write_if_changed(batch, body.encode())
    for old in batches.glob("batch_*.cpp"):
        if old.name not in wanted:
            old.unlink()

    print(f"{len(names)} generated files: {changed} changed, {removed} removed; "
          f"{len(buckets)} batches, {batch_changed} changed; the scratchpad inline in {hot_found} of "
          f"{len(hot)} listed functions")


if __name__ == "__main__":
    main()
