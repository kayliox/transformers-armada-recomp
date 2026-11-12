"""Assemble the PS2Recomp config for Transformers from the generated inputs.

Inputs (all produced from the user's own ELF by build.ps1):
  analyzer.toml     ps2_analyzer output (SDK signature matches, MMIO sites)
  functions.csv     find_functions.py function map
  jump_tables.json  find_functions.py switch tables

Hand-maintained settings live in config/overrides.toml and are merged last. Another release uses its own
config/versions/<serial>/overrides.toml if it has one, else the USA one with its addresses translated by the
release's "code_shift" (game.json), each patched instruction checked against the release's ELF.

Usage: python make_config.py <work_dir> <out.toml>
"""

import json
import os
import sys
import tomllib
from pathlib import Path

from ps2elf import DEFAULT_VERSION, Elf, code_address, config_dir, load_game_config, release_file

ROOT = Path(__file__).resolve().parents[1]


def q(s):
    return '"' + s.replace("\\", "/") + '"'


def selector_addr(sel):
    return int(sel.split("@")[1], 16) if "@" in sel else None


def same_registers(original, value):
    """A patch keeps the register the instruction writes: the same rt (I-type: a load may read another base, an addiu
    become a load), the same rd (R-type), the same opcode (j / jal). Replacing with a nop (0) is always allowed."""
    if value == 0:
        return True
    if value >> 26 in (2, 3):
        return original >> 26 == value >> 26
    if value >> 26 == 0:
        return original >> 26 == 0 and (original >> 11) & 0x1F == (value >> 11) & 0x1F
    return original >> 26 != 0 and (original >> 16) & 0x1F == (value >> 16) & 0x1F


def translate_overrides(overrides, cfg, elf):
    """The USA overrides.toml's addresses moved to this release (code_shift); stops on one that cannot be."""
    def move(usa, what):
        a = code_address(usa, cfg)
        if a is None:
            sys.exit(f"make_config: {what} at USA 0x{usa:08X} lies where the releases' code differs: "
                     f"give it in config/versions/{cfg['serial']}/overrides.toml")
        return a

    def move_selector(sel):
        if "@" not in sel:
            return sel
        name, addr = sel.split("@")
        return f"{name}@0x{move(int(addr, 16), sel):08X}"

    og = overrides.setdefault("general", {})
    for key in ("stubs", "entry_points", "skip"):
        og[key] = [move_selector(s) for s in og.get(key, [])]
    moved = []
    for p in overrides.get("patches", {}).get("instructions", []):
        usa, value = int(p["address"], 16), int(p["value"], 16)
        a = move(usa, "a patch")
        if value >> 26 in (2, 3):  # j / jal: the target moves too
            value = (value & 0xFC000000) | (move((value & 0x3FFFFFF) << 2, "a patched call's target") >> 2)
        original = elf.word(a)
        if "replaces" in p:
            # (a patch that moves instructions around - another register written - names the word it replaces:
            # checked as it is, which also proves the address moved right)
            if original != int(p["replaces"], 16):
                sys.exit(f"make_config: patch USA 0x{usa:08X} -> 0x{a:08X}: {cfg['elf_name']} has 0x{original:08X} there, "
                         f"not the 0x{int(p['replaces'], 16):08X} the patch replaces")
        elif not same_registers(original, value):
            sys.exit(f"make_config: patch USA 0x{usa:08X} -> 0x{a:08X}: {cfg['elf_name']} has 0x{original:08X} there, "
                     f"not an instruction the patch 0x{value:08X} replaces (different registers)")
        moved.append({"address": f"0x{a:x}", "value": f"0x{value:08x}"})
    overrides.setdefault("patches", {})["instructions"] = moved
    print(f"{len(moved)} patches translated from {DEFAULT_VERSION} to {cfg['serial']}, each checked in {cfg['elf_name']}")


def main():
    work, out = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    cfg = load_game_config()
    regions = cfg["code_regions"]
    in_code = lambda a: a is not None and any(lo <= a < hi for lo, hi in regions)

    analyzer = tomllib.loads((work / "analyzer.toml").read_text())
    path = release_file("overrides.toml")
    overrides = tomllib.loads(path.read_text())
    if path.parent != config_dir() and cfg["code_shift"]:
        translate_overrides(overrides, cfg, Elf(work / cfg["elf_name"]))
    tables = json.loads((work / "jump_tables.json").read_text())
    ag = analyzer["general"]
    og = dict(overrides.get("general", {}))
    # Build-time A/B of the generated code's stores (FPS round 3; the next build.ps1 translates again):
    # PS2X_CODEGEN_FULL_STORES=1 - every instruction stores the pc and every register write goes
    # through to the context (minimal_pc_stores and register_write_back off), as before.
    if os.environ.get("PS2X_CODEGEN_FULL_STORES") == "1":
        og["minimal_pc_stores"] = False
        og["register_write_back"] = False

    starts = set()
    for line in (work / "functions.csv").read_text().splitlines()[1:]:
        starts.add(int(line.split(",")[1], 16))

    def keep(sel):
        a = selector_addr(sel)
        return a is None or (in_code(a) and a in starts)

    stubs = [s for s in ag.get("stubs", []) if keep(s)]
    removed = {s.split("@")[0] for s in og.get("remove_stubs", [])}
    stubs = [s for s in stubs if s.split("@")[0] not in removed] + og.get("stubs", [])
    entry_points = [s for s in ag.get("entry_points", []) if keep(s)] + og.get("entry_points", [])

    lines = [
        "# Generated by transformers/tools/make_config.py - do not edit.",
        "# Hand-maintained settings belong in transformers/config/overrides.toml.",
        "",
        "[general]",
        f"input = {q(str(work / cfg['elf_name']))}",
        f"ghidra_output = {q(str(work / 'functions.csv'))}",
        f"output = {q(str(work / 'staging'))}",  # sync_generated.py installs it
        "single_file_output = false",
        f"output_worker_threads = {og.get('output_worker_threads', 0)}",
        "patch_syscalls = false",
        "patch_cop0 = true",
        "patch_cache = true",
        f"register_cache = {str(bool(og.get('register_cache', False))).lower()}",
        f"indirect_call_resume_only = {str(bool(og.get('indirect_call_resume_only', False))).lower()}",
        f"minimal_pc_stores = {str(bool(og.get('minimal_pc_stores', False))).lower()}",
        f"register_write_back = {str(bool(og.get('register_write_back', False))).lower()}",
        f"direct_calls = {str(bool(og.get('direct_calls', False))).lower()}",
        f"direct_call_sites = {str(bool(og.get('direct_call_sites', False))).lower()}",
        f"inline_leaves = {int(og.get('inline_leaves', 0))}",
        f"inline_loop_checkpoints = {str(bool(og.get('inline_loop_checkpoints', False))).lower()}",
        f"indirect_call_sites = {str(bool(og.get('indirect_call_sites', False))).lower()}",
        f"inline_scratchpad_constants = {str(bool(og.get('inline_scratchpad_constants', False))).lower()}",
        f"vu0_register_cache = {str(bool(og.get('vu0_register_cache', False))).lower()}",
        f"vu0_fold_blends = {str(bool(og.get('vu0_fold_blends', False))).lower()}",
        f"fpu_clamp_elision = {str(bool(og.get('fpu_clamp_elision', False))).lower()}",
        f"minimal_goto_pc_stores = {str(bool(og.get('minimal_goto_pc_stores', False))).lower()}",
        f"direct_calls_unchecked = {str(bool(og.get('direct_calls_unchecked', False))).lower()}",
        f"fast_codegen_only = [{', '.join(q(s) for s in og.get('fast_codegen_only', []))}]",
        "stubs = [",
        *[f"  {q(s)}," for s in stubs],
        "]",
        "entry_points = [",
        *[f"  {q(s)}," for s in entry_points],
        "]",
        f"skip = [{', '.join(q(s) for s in og.get('skip', []))}]",
        "",
        "[mmio]",
        *[f"{q(k)} = {q(v)}" for k, v in analyzer.get("mmio", {}).items() if in_code(int(k, 16))],
        "",
        "[jump_tables]",
    ]
    for addr, targets in tables.items():
        lines.append("[[jump_tables.table]]")
        lines.append(f"address = {q(addr)}")
        lines.append("entries = [")
        lines += [f"  {{ index = {i}, target = {q(t)} }}," for i, t in enumerate(targets)]
        lines.append("]")

    patches = overrides.get("patches", {}).get("instructions", [])
    lines += ["", "[patches]", "instructions = ["]
    lines += [f"  {{ address = {q(p['address'])}, value = {q(p['value'])} }}," for p in patches]
    lines.append("]")

    out.write_text("\n".join(lines) + "\n")
    print(f"{len(stubs)} stubs, {len(entry_points)} entry points, {len(tables)} jump tables, "
          f"{len(patches)} patches -> {out}")


if __name__ == "__main__":
    main()
