"""Recover function boundaries from the stripped Transformers ELF.

The retail executable has no symbols, and its code and data share one section,
so the recompiler cannot tell where functions start or where code stops. This
tool builds a Ghidra-style function map (Name,Start,End,Size CSV) without
needing Ghidra or IDA:

  1. Seeds: the ELF entry point, every JAL/BAL target, and code pointers found
     in data (vtables, callback tables) or built with LUI/ADDIU pairs.
  2. Each seed is explored by recursive descent (branches, jumps, jump tables)
     to find the instructions that belong to it.
  3. Unreferenced code left between functions is picked up as extra functions.

Names come from the PS2Recomp analyzer's SDK signature matches when available.

Usage: python find_functions.py <SLUS_206.68> <out.csv> [--names analyzer.toml]
"""

import argparse
import json
import re
import sys
import tomllib
from collections import Counter
from pathlib import Path

from ps2elf import (CALL, JALR, JR, JR_RA, JUMP, BRANCH, NORMAL, STOP, UNCOND, Elf,
                    config_dir, decode_cf, is_valid, load_game_config, release_file, sext16)


def gpr_written(w):
    """The general-purpose register an instruction writes, or None."""
    op = w >> 26
    if op == 0:
        fn = w & 0x3F
        if fn in (0x08, 0x0C, 0x0D, 0x0F, 0x11, 0x13, 0x29) or 0x30 <= fn <= 0x37:
            return None                              # jr, syscall, break, sync, mthi/lo, mtsa, traps
        return (w >> 11) & 31
    if op in (0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x18, 0x19, 0x1A, 0x1B, 0x1E, 0x37) \
            or 0x20 <= op <= 0x27:
        return (w >> 16) & 31
    if op == 0x03:
        return 31
    if op == 0x1C:                                   # MMI: most write rd
        return (w >> 11) & 31
    if op == 0x10 and ((w >> 21) & 31) == 0:         # mfc0
        return (w >> 16) & 31
    if op == 0x11 and ((w >> 21) & 31) in (0, 2):    # mfc1 / cfc1
        return (w >> 16) & 31
    if op == 0x12 and ((w >> 21) & 31) in (1, 2):    # qmfc2 / cfc2
        return (w >> 16) & 31
    return None


class Finder:
    def __init__(self, elf, regions):
        self.elf = elf
        self.regions = regions
        self.starts = set()
        self.sources = {}          # start -> why we think it is a function
        self.bodies = {}           # start -> set of instruction addresses
        self.jump_tables = {}      # table address -> list of targets
        self.bad = set()           # seeds that decoded into invalid instructions

    # -- helpers -----------------------------------------------------------
    def in_code(self, a):
        return (a & 3) == 0 and any(lo <= a < hi for lo, hi in self.regions)

    def region_end(self, a):
        for lo, hi in self.regions:
            if lo <= a < hi:
                return hi
        return a

    def add_seed(self, a, why):
        if self.in_code(a) and a not in self.starts and a not in self.bad:
            self.starts.add(a)
            self.sources[a] = why
            return True
        return False

    def looks_like_boundary(self, a):
        """True when `a` is where a new function could plausibly begin."""
        if not self.in_code(a) or not is_valid(self.elf.word(a)):
            return False
        if self.elf.word(a) == 0:
            return False
        if any(lo == a for lo, _ in self.regions):
            return True
        # Skip alignment padding, then expect `jr ra` / `j` / `b` and its delay slot.
        b = a - 4
        while b > a - 64 and self.in_code(b) and self.elf.word(b) == 0:
            b -= 4
        if not self.in_code(b - 4):
            return False
        eret = 0x42000018                           # the one terminator without a delay slot
        if self.elf.word(b) == eret:
            return True
        if self.elf.word(b - 4) != eret and self.is_terminator(self.elf.word(b - 4)):
            return True
        # A jump right before the zeros had a nop for its delay slot. Without zeros in
        # between, `a` itself is the delay slot: splitting there cuts it off the function
        # before (0x3E646C held the `addiu sp` of sub_003E61E0's return, and a data word
        # that happened to equal it lost that function its stack restore).
        return b != a - 4 and self.is_terminator(self.elf.word(b))

    @staticmethod
    def is_terminator(w):
        # jr <any reg> covers vtable thunks that tail-call through $t9
        is_jr = (w >> 26) == 0 and (w & 0x1FFFFF) == 0x08
        return is_jr or (w >> 26) == 2 or (w >> 16) == 0x1000 or w == 0x42000018

    # -- jump tables -------------------------------------------------------
    def resolve_jump_table(self, jr_pc, reg, fn_start, limit):
        """Match the compiler's switch sequence and read the table:
             sltiu at, idx, N ; lui rB, hi ; sll idx, idx, 2 ; addiu rB, rB, lo
             addu idx, idx, rB ; lw reg, off(idx) ; jr reg
        (the lo half may instead be folded into the lw offset)."""
        lw_off = None
        lw_base = None
        sum_regs = None                                      # the addu that forms lw's address
        lui, addiu = {}, {}
        count = None
        for back in range(1, 16):
            pc = jr_pc - 4 * back
            if pc < fn_start:
                break
            w = self.elf.word(pc)
            op, rs, rt = w >> 26, (w >> 21) & 31, (w >> 16) & 31
            if lw_off is None and op == 0x23 and rt == reg:
                lw_off = sext16(w)
                lw_base = rs
            elif op == 0x0F:
                lui.setdefault(rt, (w & 0xFFFF) << 16)
            elif op == 0x09 and rs == rt:
                addiu.setdefault(rt, sext16(w))
            elif op == 0x0B and count is None:               # sltiu at, idx, N
                count = w & 0xFFFF
            elif (sum_regs is None and lw_base is not None and op == 0 and (w & 0x3F) == 0x21
                  and ((w >> 11) & 31) == lw_base):
                sum_regs = {rs, rt}
        if lw_off is None:
            return None
        if len(lui) > 1 and sum_regs:
            # Another lui in the window (an unrelated load scheduled near the switch, e.g.
            # 0x258690's `lui at, 0x7000`): the table base is the one the addu feeding the lw adds.
            lui = {r: v for r, v in lui.items() if r in sum_regs}
        if len(lui) != 1:
            return None
        base_reg, hi = next(iter(lui.items()))
        table = (hi + addiu.get(base_reg, 0) + lw_off) & 0xFFFFFFFF
        if not self.elf.contains(table):
            return None
        targets = []
        n = count if count else 1024
        for i in range(n):
            t_addr = table + 4 * i
            if not self.elf.contains(t_addr):
                break
            t = self.elf.word(t_addr)
            if not (fn_start <= t < limit) or not self.in_code(t):
                break
            targets.append(t)
        if not targets or (count and len(targets) != count):
            return None
        self.jump_tables[table] = targets
        return targets

    # -- recursive descent -------------------------------------------------
    def explore(self, start, limit):
        """Collect the body of the function at `start`. Instructions at or past
        `limit` (the next known function start) belong to someone else."""
        body = set()
        calls, tails = set(), set()
        work = [start]
        while work:
            pc = work.pop()
            while True:
                if pc in body or not self.in_code(pc) or (pc >= limit and pc != start):
                    break
                w = self.elf.word(pc)
                if not is_valid(w):
                    if pc == start:
                        return None
                    break
                body.add(pc)
                kind, tgt = decode_cf(w, pc)
                if kind in (CALL, JUMP, BRANCH, UNCOND, JR, JALR):
                    if self.in_code(pc + 4) and is_valid(self.elf.word(pc + 4)):
                        body.add(pc + 4)             # delay slot
                if kind == CALL:
                    calls.add(tgt)
                    pc += 8
                elif kind == BRANCH:
                    if start <= tgt < limit:
                        work.append(tgt)
                    else:
                        tails.add(tgt)
                    pc += 8
                elif kind == UNCOND:
                    if start <= tgt < limit:
                        pc = tgt
                        continue
                    tails.add(tgt)
                    break
                elif kind == JUMP:
                    if start < tgt < limit:
                        pc = tgt
                        continue
                    tails.add(tgt)
                    break
                elif kind == JR:
                    if tgt != 31:
                        targets = self.resolve_jump_table(pc, tgt, start, limit)
                        if targets:
                            work.extend(targets)
                    break
                elif kind == JALR:
                    pc += 8
                elif kind == STOP:
                    break
                else:
                    pc += 4
        return body, calls, tails

    def prescan_jump_tables(self):
        """Find switch tables up front so their case labels are not mistaken
        for function pointers when scanning data."""
        for lo, hi in self.regions:
            for a in range(lo, hi, 4):
                w = self.elf.word(a)
                if (w >> 26) == 0 and (w & 0x3F) == 8 and ((w >> 21) & 31) != 31:
                    self.resolve_jump_table(a, (w >> 21) & 31, max(lo, a - 64), hi)
        self.table_words = set()
        self.case_labels = set()
        for table, targets in self.jump_tables.items():
            self.table_words.update(range(table, table + 4 * len(targets), 4))
            self.case_labels.update(targets)
        print(f"  {len(self.jump_tables)} switch tables", file=sys.stderr)

    def run(self, extra_seeds=()):
        self.prescan_jump_tables()
        self.add_seed(self.elf.entry, "entry")
        for a in extra_seeds:
            self.add_seed(a, "named")
        self.scan_pointers()
        for it in range(60):
            before = len(self.starts)
            self.pass_once()
            self.fill_gaps()
            print(f"  pass {it + 1}: {len(self.starts)} functions", file=sys.stderr)
            if len(self.starts) == before:
                break

    def pass_once(self):
        ordered = sorted(self.starts)
        new_calls = set()
        self.bodies.clear()
        for i, s in enumerate(ordered):
            limit = ordered[i + 1] if i + 1 < len(ordered) else self.region_end(s)
            limit = min(limit, self.region_end(s))
            res = self.explore(s, limit)
            if res is None:
                self.starts.discard(s)
                self.bad.add(s)
                continue
            body, calls, tails = res
            self.bodies[s] = body
            new_calls |= calls
            new_calls |= {t for t in tails if self.looks_like_boundary(t)}
        for c in new_calls:
            self.add_seed(c, "call")

    def scan_pointers(self):
        """Code addresses stored in data (vtables, tables of callbacks)."""
        elf = self.elf
        found = 0
        for i, w in enumerate(elf.words):
            addr = elf.base + 4 * i
            if self.in_code(addr) or addr in self.table_words or w in self.case_labels:
                continue
            if self.in_code(w) and self.looks_like_boundary(w):
                found += self.add_seed(w, "data-ptr")
        # LUI/ADDIU (or ORI) pairs that build a code address in a register. A value that is only
        # ever reached by a conditional branch from just before it (and never called) is a
        # constant that happens to look like code inside a function: 0x00200010 (`lui 0x20;
        # addiu 0x10`) cut sub_001FFC70's loop off into two pieces of its own.
        called, branched = set(), set()
        for lo, hi in self.regions:
            for a in range(lo, hi, 4):
                kind, tgt = decode_cf(elf.word(a), a)
                if kind == CALL:
                    called.add(tgt)
                elif kind == BRANCH and tgt - 4096 <= a < tgt:
                    branched.add(tgt)

        def inside_function(t):
            return t in branched and t not in called

        for lo, hi in self.regions:
            for a in range(lo, hi - 4, 4):
                w = elf.word(a)
                if w >> 26 != 0x0F:
                    continue
                rt, upper = (w >> 16) & 31, (w & 0xFFFF) << 16
                stop = min(a + 40, hi)
                for b in range(a + 4, stop, 4):
                    w2 = elf.word(b)
                    op2 = w2 >> 26
                    if op2 in (0x09, 0x0D) and ((w2 >> 21) & 31) == rt:
                        t = upper + (sext16(w2) if op2 == 0x09 else (w2 & 0xFFFF))
                        if self.in_code(t) and t not in self.case_labels and self.looks_like_boundary(t) \
                                and not inside_function(t):
                            found += self.add_seed(t, "lui-pair")
                        break
                    if gpr_written(w2) == rt:
                        break                        # register overwritten
                    if decode_cf(w2, b)[0] != NORMAL:
                        stop = min(stop, b + 8)      # still check the delay slot
                        if b + 4 >= stop:
                            break
        print(f"  {found} functions referenced by pointer", file=sys.stderr)

    def fill_gaps(self):
        """Pick up code not reached from any seed (dead or computed-call code)."""
        covered = []
        for s, body in self.bodies.items():
            covered.append((s, max(body) + 4))
        covered.sort()
        spans = []
        for lo, hi in self.regions:
            cur = lo
            for s, e in covered:
                if e <= lo or s >= hi:
                    continue
                if s > cur:
                    spans.append((cur, s))
                cur = max(cur, e)
            if cur < hi:
                spans.append((cur, hi))
        for lo, hi in spans:
            a = lo
            while a < hi and self.elf.word(a) == 0:
                a += 4
            if a < hi and self.looks_like_boundary(a) and self.plausible_code(a, hi):
                self.add_seed(a, "gap")

    def plausible_code(self, a, hi):
        """At least a few valid instructions ending in a return or jump."""
        n = 0
        while a < hi and n < 4096:
            w = self.elf.word(a)
            if not is_valid(w):
                return False
            if Finder.is_terminator(w):
                return n >= 1
            a += 4
            n += 1
        return False

    def functions(self):
        ordered = sorted(self.bodies)
        out = []
        for i, s in enumerate(ordered):
            end = max(self.bodies[s]) + 4
            nxt = ordered[i + 1] if i + 1 < len(ordered) else self.region_end(s)
            end = min(end, nxt)
            # A function must not end on a jump whose delay slot belongs to the next one:
            # the recompiled jump would skip that instruction (often the stack restore).
            if end - s >= 4 and decode_cf(self.elf.word(end - 4), end - 4)[0] not in (NORMAL, STOP):
                print(f"  warning: function at 0x{s:08X} ends on a jump at 0x{end - 4:08X} without its "
                      f"delay slot (next function starts there)", file=sys.stderr)
            out.append((s, end))
        return out


def load_names(path):
    """SDK signature matches from the analyzer. Tiny functions (empty
    destructors, syscall wrappers) match in many places; those names are
    dropped so they cannot mislabel unrelated code. So are the matches listed
    under remove_stubs in config/overrides.toml: the recompiler binds a function
    to the runtime's HLE version by its name, so those keep a sub_ name and are
    translated from the game's own code."""
    names = {}
    if not path:
        return names, set()
    overrides = release_file("overrides.toml")  # (a release without its own: the USA list, by name)
    removed = set()
    if overrides.exists():
        removed = set(tomllib.loads(overrides.read_text()).get("general", {}).get("remove_stubs", []))
    seen = Counter()
    for m in re.finditer(r'"([^"@]+)@0x([0-9A-Fa-f]+)"', open(path).read()):
        name, addr = m.group(1), int(m.group(2), 16)
        if not name.startswith("sub_") and addr not in names:
            names[addr] = name
            seen[name] += 1
    return {a: n for a, n in names.items() if seen[n] == 1 and n not in removed}, set(names)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf")
    ap.add_argument("csv")
    ap.add_argument("--names", help="analyzer TOML with name@0xADDR selectors")
    ap.add_argument("--jump-tables", help="write resolved switch tables to this JSON file")
    args = ap.parse_args()

    cfg = load_game_config()
    elf = Elf(args.elf)
    if f"{elf.crc32:08x}" != cfg["elf_crc32"]:
        sys.exit(f"{args.elf}: CRC32 {elf.crc32:08x} does not match the expected "
                 f"{cfg['serial']} executable ({cfg['elf_crc32']}).")

    names, matched = load_names(args.names)
    finder = Finder(elf, cfg["code_regions"])
    finder.run(extra_seeds=sorted(matched))
    funcs = finder.functions()

    with open(args.csv, "w", newline="\n") as f:
        f.write("Name,Start,End,Size\n")
        for s, e in funcs:
            name = names.get(s, f"sub_{s:08X}")
            f.write(f"{name},0x{s:08X},0x{e:08X},{e - s}\n")

    if args.jump_tables:
        with open(args.jump_tables, "w") as f:
            json.dump({f"0x{t:08X}": [f"0x{x:08X}" for x in v]
                       for t, v in sorted(finder.jump_tables.items())}, f, indent=0)

    why = Counter(finder.sources.get(s, "?") for s, _ in funcs)
    code = sum(e - s for s, e in funcs)
    total = sum(hi - lo for lo, hi in cfg["code_regions"])
    print(f"{len(funcs)} functions ({dict(why)}), {len(finder.jump_tables)} jump tables, "
          f"{code * 100 / total:.1f}% of code regions covered -> {args.csv}")


if __name__ == "__main__":
    main()
