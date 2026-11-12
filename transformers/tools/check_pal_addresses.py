"""Check that every guest code address the C++ uses is translated for the PAL release (SLES-52388).

SLES_523.88 has the same size and layout as SLUS_206.68; its code sits at the same addresses up to 0x33B690,
16 bytes lower from 0x33FFD8 and 32 bytes lower from 0x3EF1F0 to the end of the code (transformers/src/
game_version.h, TF_GA). Between those points (the two places where the releases' code differs) each address is
mapped by hand. This lists every hexadecimal literal in the shifted part of the code (0x33B690 - 0x5DCB80) that is
not inside TF_GA(...) or TF_VA(...), outside comments and strings, and exits 1 if there is one: an address added
for the USA build that the PAL build would get wrong.

The private tree's mods (overlay/src, when the tree has it) are scanned too, but only reported, never failed: the
overlay is built for the USA disc alone (docs/PAL_ADDRESSES.md), so a European build does not stop on it. The
literals the overlay already had are listed in overlay/pal_known_addresses.txt (kept with the overlay, out of the
public tree): those are counted in one line, and only a literal that is not in that list is printed, as "new", so new
mod code that would need TF_GA shows at once.

Usage: python transformers/tools/check_pal_addresses.py [--list] [--fix] [--overlay-known]
  --list           every literal found, wrapped or not
  --fix            wrap each one found in TF_GA(...) and include game_version.h where needed (after merging upstream);
                   the overlay is left as it is
  --overlay-known  write overlay/pal_known_addresses.txt from the overlay as it is now (the list of known ones)
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DIRS = ('src', 'decomp', 'native_render')
OVERLAY = ROOT.parent / 'overlay'
OVERLAY_DIRS = ('src',)
OVERLAY_KNOWN = OVERLAY / 'pal_known_addresses.txt'
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


def overlay_unwrapped():
    """The overlay's literals in the shifted range that are not wrapped: (path from the tree's root, line, address,
    the line's text)."""
    for sub in OVERLAY_DIRS:
        folder = OVERLAY / sub
        if not folder.is_dir():
            continue
        for f in sorted(folder.rglob('*')):
            if f.suffix not in ('.cpp', '.h', '.inc'):
                continue
            text = f.read_bytes().decode('utf-8', errors='replace')
            code = strip(text)
            for m in HEX.finditer(code):
                a = int(m.group(1), 16)
                if not (LOW < a < HIGH) or a in NOT_ADDRESSES or wrapped(code, m.start()):
                    continue
                line = code.count('\n', 0, m.start()) + 1
                yield f.relative_to(OVERLAY.parent).as_posix(), line, a, text.splitlines()[line - 1].strip()[:110]


def read_known():
    """overlay/pal_known_addresses.txt: "<path> 0x<address> <count>" a line ('#' starts a note)."""
    known = {}
    if OVERLAY_KNOWN.is_file():
        for row in OVERLAY_KNOWN.read_text(encoding='utf-8').splitlines():
            parts = row.split('#', 1)[0].split()
            if len(parts) == 3:
                known[(parts[0], int(parts[1], 16))] = int(parts[2])
    return known


def check_overlay(write_known):
    """Reports the overlay (never fails: it is built for the USA disc alone). Returns the new literals' count."""
    if not OVERLAY.is_dir():
        return 0
    found = list(overlay_unwrapped())
    counts = {}
    for path, _, a, _ in found:
        counts[(path, a)] = counts.get((path, a), 0) + 1
    if write_known:
        lines = ['# Code addresses the overlay (USA only) uses without TF_GA, known when this list was written',
                 '# (transformers/tools/check_pal_addresses.py --overlay-known): "<path> <address> <count>". The check',
                 '# reports only the ones beyond this list. A mod made to work on the European disc wraps its own and',
                 '# leaves this list.']
        lines += [f'{p} 0x{a:08X} {n}' for (p, a), n in sorted(counts.items())]
        OVERLAY_KNOWN.write_text('\n'.join(lines) + '\n', encoding='utf-8', newline='\n')
        print(f'overlay: {len(found)} unwrapped literal(s) written to {OVERLAY_KNOWN.relative_to(OVERLAY.parent).as_posix()}')
        return 0
    known = read_known()
    new = {key for key, n in counts.items() if n > known.get(key, 0)}
    shown = 0
    for path, line, a, text in found:
        if (path, a) in new:
            print(f'NEW {path}:{line}: 0x{a:08X}  {text}  (overlay: reported, not failed)')
            shown += 1
    old = len(found) - shown
    if shown:
        print(f'overlay: {shown} new code address(es) without TF_GA (above), {old} known (the mods are USA only for now; '
              f'a mod made for the European disc wraps them: docs/PAL_ADDRESSES.md)')
    elif old:
        print(f'overlay: {old} known code address(es) without TF_GA, none new (the mods are USA only for now)')
    return shown


def main():
    show_all = '--list' in sys.argv
    fix = '--fix' in sys.argv
    if '--overlay-known' in sys.argv:
        check_overlay(True)
        return 0
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
    check_overlay(False)  # (reported only: the exit code stays the core's)
    if bad and fix:
        print(f'{bad} code address(es) wrapped in TF_GA(...)')
        return 0
    if bad:
        print(f'{bad} code address(es) not translated for PAL: wrap each in TF_GA(...) (transformers/src/game_version.h)')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
