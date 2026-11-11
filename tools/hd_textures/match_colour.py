"""match_colour.py - the HD (upscaled) images' colour matched to the game's own textures (TODO 124).

The upscaler (Real-ESRGAN x4plus) darkens dark, busy textures: Amazon's tree and fern images came out 10-37%
darker than the game's own texels (measured on the box-averaged image over the opaque texels, deep inside the
opaque parts as well - not an alpha-edge effect), the level geometry by a median of 11%. The PS2's look is the
game's: so each HD image whose average colour, box-averaged to the game's size, is more than --threshold (5%)
off the game's texture in brightness (over the texels the game draws: alpha >= 128) has its colour channels
matched (also when a channel the game uses is off by more than twice that: a tint). Each colour channel goes
through one tone curve, v -> 255 (v/255)^e, its exponent solved so the channel's average over those texels is the
game's: the dark texels the upscaler darkened most come up, the bright ones stay (a plain gain blew the highlights
out to grey-white on the images half the game's brightness). The pixels keep their places and their order of
brightness - the detail as it was; alpha as it was. (--max-gain bounds the ratio the decision allows a channel.)

Input:  the upscaled tree (--upscaled) and the game's textures converted at their own size (--native, the tree
        the renderer compares with: <upscaled>/../converted/textures), relpaths 1:1.
Output: the corrected images in place; each original first copied to --backup (default <upscaled>/../
        upscaled_precolour, never overwritten: the first copy is the original). --restore puts them back.
        The renderer's compressed cache (upscaled_bc7_cache) notices the changed PNG and compresses it again,
        once. A report (--report, JSON): per image the game's / HD mean and the gains.
        A ledger in the backup folder (colour_gains.json) holds each corrected image's gains and the file it
        became: such an image is matched again from its original, so a run again changes nothing (no gain on a
        gain); an image the upscaler made again since is a new original (backed up, then matched).
Needs:  Python 3.11+, Pillow (no numpy).

  python tools\\hd_textures\\match_colour.py --upscaled hd_textures\\upscaled --dry-run
  python tools\\hd_textures\\match_colour.py --upscaled hd_textures\\upscaled
  python tools\\hd_textures\\match_colour.py --upscaled hd_textures\\upscaled --restore
"""
import argparse
import json
import os
import shutil
import sys
import time

from PIL import Image, ImageStat

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from jsonfile import read_json, write_json  # noqa: E402  (the tools' JSON files read and written safely)

# The level's textures (geometry, forest, rocks, sky, the eco system's shared ones). Characters measured within
# 1.5% (left out: their HD images were checked one by one, TODO 57); the UI holds redrawn art on purpose.
DEFAULT_PREFIXES = ["TFA/LEVELS/", "TFA2/ECOTEXTURES/", "TFA2/SKY/"]


def means(img, mask):
    """Per-channel mean of an RGB(A) image over the mask's set texels; None when the mask is empty."""
    stat = ImageStat.Stat(img.convert("RGB"), mask)
    if not stat.count or stat.count[0] == 0:
        return None
    return stat.mean[:3]


def measure(hd_path, native_path):
    """(game means, HD means box-averaged to the game's size, opaque share, the box-averaged HD's histogram over the
    opaque texels: 3 x 256) or None."""
    nat = Image.open(native_path).convert("RGBA")
    hd = Image.open(hd_path).convert("RGBA")
    if hd.width < nat.width or hd.height < nat.height:
        return None
    small = hd.resize(nat.size, Image.BOX)
    mask = nat.getchannel("A").point(lambda a: 255 if a >= 128 else 0)
    opaque = ImageStat.Stat(mask).mean[0] / 255.0
    g = means(nat, mask)
    h = means(small, mask)
    if g is None or h is None:
        return None
    return g, h, opaque, small.convert("RGB").histogram(mask)


def solve_gamma(hist, target, lo=0.2, hi=5.0):
    """The exponent e for which a channel's values v -> 255 (v/255)^e average `target` (its 256-bin histogram).
    A tone curve, not a gain: it lifts the dark texels the upscaler darkened most and leaves the bright ones
    where they are (no clipping, no washed-out highlights)."""
    n = sum(hist)
    if n == 0:
        return 1.0

    def mean(e):
        return sum(c * 255.0 * (v / 255.0) ** e for v, c in enumerate(hist) if c) / n

    if mean(lo) <= target:
        return lo
    if mean(hi) >= target:
        return hi
    a, b = lo, hi  # (the mean falls as e grows)
    for _ in range(40):
        m = (a * b) ** 0.5
        if mean(m) > target:
            a = m
        else:
            b = m
    return (a * b) ** 0.5


def apply_curve(hd_path, curve, out_path):
    """Each colour channel through its tone curve v -> 255 (v/255)^e; alpha as it was."""
    img = Image.open(hd_path)
    mode = img.mode
    img = img.convert("RGBA" if "A" in mode or mode == "P" else "RGB")
    bands = list(img.split())
    for c in range(3):
        e = curve[c]
        if e != 1.0:
            bands[c] = bands[c].point([min(255, int(255.0 * (v / 255.0) ** e + 0.5)) for v in range(256)])
    # (written whole beside it, then put in place: the game may be reading the set meanwhile - TODO 219)
    tmp = out_path + ".tmp.png"
    Image.merge(img.mode, bands).save(tmp)
    for attempt in range(20):  # (the game reading the old file a moment: Windows refuses the swap meanwhile)
        try:
            os.replace(tmp, out_path)
            break
        except PermissionError:
            if attempt == 19:
                raise
            time.sleep(0.1)


def is_matched(rel, path, ledger):
    """The image is the file the ledger says it became (matched, or checked and left as it was)."""
    entry = ledger.get(rel)
    if entry is None:
        return False
    st = os.stat(path)
    return entry.get("size") == st.st_size and entry.get("mtime_ns") == st.st_mtime_ns


def match_file(rel, path, native, backup, ledger, prefixes=None, threshold=0.05, min_mean=6.0, max_gain=4.0,
               dry_run=False, note_checked=False):
    """One HD image matched to the game's texture (the rules above). None: not one of the images treated (another
    prefix, no game texture). Else (row, group): row None when it could not be measured. The ledger is updated in
    place (the caller writes it). note_checked: an image left as it is gets a ledger entry too (curve 1, its size
    and time), so a later full pass with --only-new skips it (upscale_textures.py matches each image as it is made)."""
    prefixes = prefixes or DEFAULT_PREFIXES
    if not any(rel.upper().startswith(p.upper()) for p in prefixes):
        return None
    nat = os.path.join(native, rel)
    if not os.path.exists(nat):
        return None
    bpath = os.path.join(backup, rel)
    st = os.stat(path)
    entry = ledger.get(rel)
    ours = ((entry is not None and entry.get("size") == st.st_size and entry.get("mtime_ns") == st.st_mtime_ns) or
            (entry is None and os.path.exists(bpath)))  # (the backups made before the ledger: the first run's)
    source = bpath if ours and os.path.exists(bpath) else path
    parts = rel.split("/")
    group = parts[3] if rel.upper().startswith("TFA/LEVELS/EPISODES/") and len(parts) > 3 else "/".join(parts[:2])
    m = measure(source, nat)
    if m is None:
        return None, group
    g, h, opaque, hist = m
    gains = []
    for c in range(3):
        if g[c] < min_mean or h[c] < 1.0:
            gains.append(1.0)
        else:
            gains.append(max(1.0 / max_gain, min(max_gain, g[c] / h[c])))
    gains = [round(x, 4) for x in gains]
    lum_g = 0.299 * g[0] + 0.587 * g[1] + 0.114 * g[2]
    lum_h = 0.299 * h[0] + 0.587 * h[1] + 0.114 * h[2]
    before = (lum_h / lum_g - 1.0) if lum_g > 0 else 0.0
    row = {"rel": rel, "game": [round(x, 2) for x in g], "hd": [round(x, 2) for x in h], "opaque": round(opaque, 3),
           "luma_off": round(before, 4), "gains": gains, "curve": [1.0, 1.0, 1.0], "corrected": False, "written": False}
    # (Off: the brightness by more than the threshold, or a channel the game uses (mean >= 24) by more than
    # twice it - a tint. Dark channels alone are noise: 10 against 12 is 20%.)
    tinted = any(abs(gains[c] - 1.0) > 2.0 * threshold and g[c] >= 24.0 for c in range(3))
    if abs(before) > threshold or tinted:
        row["corrected"] = True
        curve = [1.0 if gains[c] == 1.0 else round(solve_gamma(hist[256 * c:256 * (c + 1)], g[c]), 4) for c in range(3)]
        row["curve"] = curve
        same = ours and entry is not None and entry.get("curve") == curve
        if not same:
            row["written"] = True
        if not dry_run and not same:
            if source == path:  # (a new original: kept before it changes)
                os.makedirs(os.path.dirname(bpath), exist_ok=True)
                shutil.copy2(path, bpath)
            apply_curve(bpath, curve, path)
            st2 = os.stat(path)
            ledger[rel] = {"curve": curve, "size": st2.st_size, "mtime_ns": st2.st_mtime_ns}
        m2 = measure(path, nat) if not dry_run else None
        if m2:
            g2, h2, _, _ = m2
            lum_h2 = 0.299 * h2[0] + 0.587 * h2[1] + 0.114 * h2[2]
            row["luma_off_after"] = round(lum_h2 / lum_g - 1.0, 4) if lum_g > 0 else 0.0
    else:
        if ours and not dry_run and os.path.exists(bpath):  # (corrected once, within the threshold now: the original back)
            shutil.copy2(bpath, path)
            ledger.pop(rel, None)
            st = os.stat(path)
        if note_checked and not dry_run:
            ledger[rel] = {"curve": [1.0, 1.0, 1.0], "size": st.st_size, "mtime_ns": st.st_mtime_ns}
    return row, group


def load_ledger(backup):
    """The ledger; empty when it is missing - or empty or damaged (a maker stopped while it wrote it: a player's
    update then stopped here every time). Without it every image is matched again from its original in the backup
    folder (match_file: an image with a backup and no entry is one of ours), never a gain on a gain."""
    return read_json(os.path.join(backup, "colour_gains.json"), {}, "the colour-match ledger (colour_gains.json)")


def save_ledger(backup, ledger):
    os.makedirs(backup, exist_ok=True)
    write_json(os.path.join(backup, "colour_gains.json"), ledger, indent=0, sort_keys=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--upscaled", required=True)
    ap.add_argument("--native", default=None, help="default: <upscaled>/../converted/textures")
    ap.add_argument("--backup", default=None, help="default: <upscaled>/../upscaled_precolour")
    ap.add_argument("--prefix", action="append", default=None, help="relpath prefixes to treat (default: the levels')")
    ap.add_argument("--threshold", type=float, default=0.05, help="largest channel deviation left alone (0.05 = 5%%)")
    ap.add_argument("--min-mean", type=float, default=6.0, help="channels darker than this on the game's side get no gain")
    ap.add_argument("--max-gain", type=float, default=4.0, help="the largest gain a channel gets (and 1/it the smallest)")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--restore", action="store_true", help="put the originals from --backup back")
    ap.add_argument("--report", default=None)
    ap.add_argument("--only-new", action="store_true",
                    help="skip the images the ledger already holds as they are (matched as they were made)")
    a = ap.parse_args()

    up = os.path.abspath(a.upscaled)
    native = os.path.abspath(a.native or os.path.join(up, "..", "converted", "textures"))
    backup = os.path.abspath(a.backup or os.path.join(up, "..", "upscaled_precolour"))
    prefixes = a.prefix or DEFAULT_PREFIXES

    if a.restore:
        n = 0
        for root, _, files in os.walk(backup):
            for f in files:
                if f == "colour_gains.json" and root == backup:
                    continue
                src = os.path.join(root, f)
                dst = os.path.join(up, os.path.relpath(src, backup))
                shutil.copy2(src, dst)
                n += 1
        ledger_path = os.path.join(backup, "colour_gains.json")
        if os.path.exists(ledger_path):
            os.remove(ledger_path)
        print(f"match_colour: {n} originals put back from {backup}")
        return 0

    # The ledger (in the backup folder): per corrected image the gains it got and the file it became (size, mtime). An
    # image that is still that file is matched again from its original (the backup) - so a run again gives the same
    # result, never a gain on a gain; an image the upscaler made again since (another file) is a new original.
    ledger = load_ledger(backup)

    rows, per_group = [], {}
    written = 0
    for root, _, files in os.walk(up):
        for f in files:
            if not f.lower().endswith(".png"):
                continue
            path = os.path.join(root, f)
            rel = os.path.relpath(path, up).replace("\\", "/")
            if a.only_new and is_matched(rel, path, ledger):
                continue
            r = match_file(rel, path, native, backup, ledger, prefixes, a.threshold, a.min_mean, a.max_gain, a.dry_run)
            if r is None:
                continue
            row, group = r
            gs = per_group.setdefault(group, {"images": 0, "corrected": 0, "written": 0, "before": [], "after": []})
            gs["images"] += 1
            if row is None:
                continue
            gs["before"].append(row["luma_off"])
            gs["after"].append(row.get("luma_off_after", row["luma_off"]))
            if row["corrected"]:
                gs["corrected"] += 1
            if row["written"]:
                gs["written"] += 1
                written += 1
            rows.append(row)
    if not a.dry_run:
        save_ledger(backup, ledger)

    def med(v):
        v = sorted(v)
        return v[len(v) // 2] if v else 0.0

    print(f"match_colour: {'dry run, ' if a.dry_run else ''}threshold {a.threshold:.0%}, prefixes {prefixes}")
    for k in sorted(per_group):
        gs = per_group[k]
        print(f"  {k:32s} {gs['images']:4d} images, {gs['corrected']:4d} corrected ({gs['written']} written now); brightness off the game's: "
              f"median {med(gs['before']):+.1%} before, {med(gs['after']):+.1%} after")
    print(f"match_colour: {written} images {'to write' if a.dry_run else 'written'}")
    if a.report:
        write_json(a.report, {"threshold": a.threshold, "groups": {k: {"images": v["images"], "corrected": v["corrected"], "written": v["written"],
                   "median_before": med(v["before"]), "median_after": med(v["after"])} for k, v in per_group.items()},
                   "images": rows}, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
