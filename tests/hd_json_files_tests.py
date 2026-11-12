"""hd_json_files_tests.py - the HD texture tools with each of their JSON files emptied, cut off or damaged in turn: a
warning and the file taken as missing (made again), never a crash (a player's update stopped at the HD textures every
time on an empty colour_gains.json, 2026-11-11). Also: the writes leave no half-written file.

    python tests/hd_json_files_tests.py          (exit code 0: all passed)
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HD = ROOT / "tools" / "hd_textures"
sys.path.insert(0, str(HD))

failed = 0


def check(ok, what):
    global failed
    print(("  ok    " if ok else "  FAIL  ") + what, flush=True)
    if not ok:
        failed += 1


BAD = {"empty": "", "spaces": "  \r\n", "cut off": '{"TFA/LEVELS/x.png": {"curve": [1.0, 1.0', "not json": "\x00\x00\x00", "a list": "[1, 2]"}


def main():
    import jsonfile
    import match_colour as mc
    import upscale_textures as up

    tmp = Path(tempfile.mkdtemp(prefix="tfa_hd_json_"))
    try:
        print("1. jsonfile")
        for name, text in BAD.items():
            p = tmp / f"bad_{name.replace(' ', '_')}.json"
            p.write_text(text, encoding="utf-8")
            check(jsonfile.read_json(p, {}, "a test file") == {}, f"read_json: {name} file -> taken as missing")
        check(jsonfile.read_json(tmp / "missing.json", {"x": 1}) == {"x": 1}, "read_json: missing file -> the default")
        good = tmp / "good.json"
        jsonfile.write_json(good, {"a": 1}, indent=1)
        check(jsonfile.read_json(good) == {"a": 1} and not (tmp / "good.json.tmp").exists(), "write_json: whole, no .tmp left")

        print("2. each JSON file the tools read, made bad in turn")
        hd = tmp / "hd_textures"
        out = hd / "upscaled"
        backup = hd / "upscaled_precolour"
        for d in (out, backup):
            d.mkdir(parents=True, exist_ok=True)
        for name, text in BAD.items():
            (backup / "colour_gains.json").write_text(text, encoding="utf-8")
            check(mc.load_ledger(str(backup)) == {}, f"colour_gains.json ({name}): match_colour.load_ledger -> empty ledger")
            (out / up.TILE_NAME).write_text(text, encoding="utf-8")
            check(up.load_tile(out / up.TILE_NAME) == 0, f".tile.json ({name}): load_tile -> no tile size")
            (hd / "hd_options.json").write_text(text, encoding="utf-8")
            check(up.read_options(hd / "hd_options.json") == {}, f"hd_options.json ({name}): read_options -> defaults")
            (out / up.MARKER_NAME).write_text(text, encoding="utf-8")
            check(up.load_marker(out) is None, f".made_on.json ({name}): load_marker -> none")
            (hd / "upscale_status.json").write_text(text, encoding="utf-8")
            check(up.reset_left_last_run(hd / "upscale_status.json") is False, f"upscale_status.json ({name}): no crash")
            (hd / up.VERIFY_REPORT).write_text(text, encoding="utf-8")
            check(up.forget_broken(out, hd) == 0, f"verify_report.json ({name}) + colour_gains.json ({name}): forget_broken -> nothing to do")

        print("3. the player's case: upscale_textures.py --match-colour with an empty colour_gains.json")
        src = tmp / "converted"
        src.mkdir()
        (backup / "colour_gains.json").write_text("", encoding="utf-8")
        for f in (hd / "hd_options.json", hd / "upscale_status.json", out / up.MARKER_NAME, out / up.TILE_NAME, hd / up.VERIFY_REPORT):
            f.unlink()
        r = subprocess.run([sys.executable, str(HD / "upscale_textures.py"), str(src), "--out", str(out), "--work", str(hd / "work"),
                            "--exe", str(tmp / "no_upscaler.exe"), "--match-colour", "--hd-dir", str(hd)],
                           capture_output=True, text=True, timeout=300)
        text = r.stdout + r.stderr
        check("JSONDecodeError" not in text and "Traceback" not in text, f"no crash (exit {r.returncode})")
        check("colour_gains.json" in text and "taken as missing" in text, "a warning names the file")
        if "Traceback" in text:
            print(text[-2000:])

        print("4. match_colour.py with an empty ledger: runs, and writes the ledger whole")
        native = tmp / "native"
        rel = "TFA/LEVELS/EPISODES/01_AMAZON/T.png"
        from PIL import Image
        (native / rel).parent.mkdir(parents=True, exist_ok=True)
        (out / rel).parent.mkdir(parents=True, exist_ok=True)
        Image.new("RGB", (8, 8), (100, 140, 60)).save(native / rel)
        Image.new("RGB", (32, 32), (60, 90, 40)).save(out / rel)  # (darker than the game's: corrected)
        (backup / "colour_gains.json").write_text("", encoding="utf-8")
        r = subprocess.run([sys.executable, str(HD / "match_colour.py"), "--upscaled", str(out), "--native", str(native), "--only-new"],
                           capture_output=True, text=True, timeout=300)
        text = r.stdout + r.stderr
        led = json.loads((backup / "colour_gains.json").read_text(encoding="utf-8") or "null")
        check(r.returncode == 0 and "Traceback" not in text, f"match_colour.py runs (exit {r.returncode})")
        check(isinstance(led, dict) and rel in led and not (backup / "colour_gains.json.tmp").exists(), "the ledger is written again, whole")
        if r.returncode:
            print(text[-2000:])
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
    print("\nall checks passed" if not failed else f"\n{failed} check(s) failed")
    sys.exit(1 if failed else 0)
