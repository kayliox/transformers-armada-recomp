"""Check that every guest code address the C++ uses is translated for the PAL release (SLES-52388).

SLES_523.88 has the same size and layout as SLUS_206.68; its code sits at the same addresses up to 0x33B690,
16 bytes lower from 0x33FFD8 and 32 bytes lower from 0x3EF1F0 to the end of the code (transformers/src/
game_version.h, TF_GA). Between those points (the two places where the releases' code differs) each address is
mapped by hand. This lists every hexadecimal literal in the shifted part of the code (0x33B690 - 0x5DCB80) that is
not inside TF_GA(...) or TF_VA(...), outside comments and strings, and exits 1 if there is one: an address added
for the USA build that the PAL build would get wrong.

Usage: python transformers/tools/check_pal_addresses.py [--list] [--fix]
  --list  every literal found, wrapped or not
  --fix   wrap each one found in TF_GA(...) and include game_version.h where needed (after merging upstream)
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DIRS = ('src', 'decomp', 'native_render')
LOW, HIGH = 0x33B690, 0x5DCB80
# Literals in that range that are not code addresses.
NOT_ADDRESSES = {
    0x3F01FF,  # native_render/nr_thread.cpp: a bit mask
    0x400000,  # src/pool_growth.cpp: a size (4 MB)
}
HEX = re.compile(r'\b0[xX]([0-9A-Fa-f]{6,8})[uU]?\b')
WRAP = re.compile(r'\bTF_(GA|VA)\s*\(')


def strip(text):
    """Blank out comments and string / character literals, keeping offsets and newlines."""
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            out[i:j] = ' ' * (j - i)
            i = j
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out[i:j] = [ch if ch == '\n' else ' ' for ch in text[i:j]]
            i = j
        elif c in '"\'':
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            j = min(j + 1, n)
            out[i:j] = [ch if ch == '\n' else ' ' for ch in text[i:j]]
            i = j
        else:
            i += 1
    return ''.join(out)


def wrapped(code, pos):
    """True when the literal at pos is inside the parentheses of a TF_GA(...) or TF_VA(...) on its line."""
    start = code.rfind('\n', 0, pos) + 1
    for m in WRAP.finditer(code, start, pos):
        depth = 1
        for ch in code[m.end():pos]:
            depth += 1 if ch == '(' else -1 if ch == ')' else 0
            if depth == 0:
                break
        if depth > 0:
            return True
    return False


INCLUDE = '#include "game_version.h" // (TF_GA: the code addresses that differ in the PAL release)\n'


def add_include(text):
    """Include game_version.h after the file's first block of #include lines (or after #pragma once)."""
    if '#include "game_version.h"' in text:
        return text
    nl = '\r\n' if '\r\n' in text else '\n'
    lines = text.split(nl)
    incs = [i for i, l in enumerate(lines[:120]) if l.startswith('#include')]
    if incs:
        at = incs[-1] + 1
    else:
        at = next((i + 1 for i, l in enumerate(lines) if l.startswith('#pragma once')), 0)
    lines.insert(at, INCLUDE.rstrip('\n'))
    return nl.join(lines)


def main():
    show_all = '--list' in sys.argv
    fix = '--fix' in sys.argv
    bad = 0
    for sub in DIRS:
        for f in sorted((ROOT / sub).rglob('*')):
            if f.suffix not in ('.cpp', '.h', '.inc') or f.name == 'game_version.h':  # (the steps themselves)
                continue
            text = f.read_bytes().decode('utf-8')
            code = strip(text)
            edits = []
            for m in HEX.finditer(code):
                a = int(m.group(1), 16)
                if not (LOW < a < HIGH) or a in NOT_ADDRESSES:
                    continue
                ok = wrapped(code, m.start())
                if ok and not show_all:
                    continue
                line = code.count('\n', 0, m.start()) + 1
                rel = f.relative_to(ROOT).as_posix()
                print(f'{"ok " if ok else "BAD"} {rel}:{line}: 0x{a:08X}  {text.splitlines()[line - 1].strip()[:110]}')
                bad += 0 if ok else 1
                if not ok:
                    edits.append((m.start(), m.end()))
            if fix and edits:
                for b, e in reversed(edits):
                    text = text[:b] + 'TF_GA(' + text[b:e] + ')' + text[e:]
                f.write_bytes(add_include(text).encode('utf-8'))
    if bad and fix:
        print(f'{bad} code address(es) wrapped in TF_GA(...)')
        return 0
    if bad:
        print(f'{bad} code address(es) not translated for PAL: wrap each in TF_GA(...) (transformers/src/game_version.h)')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
