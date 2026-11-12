#!/usr/bin/env python3
"""How much upscaling each kind of texture actually needs (PR #20, lean HD textures).

Purpose: for every unique game texture, make the upscaled versions the HD maker would make
(1x, 2x, 4x, 8x, 16x of the source), look at each one the way the player sees it from a set
of simulated distances, and measure how much the picture on screen changes from one 2x step to
the next. Where a step no longer changes what reaches the screen, more upscaling only costs
making time and disk space. The result is a per-category table and a proposed ratio per
category and display size.

How a texture is "seen" at a distance: at a given distance a texture covers some number of
screen pixels. The GPU magnifies a texture smaller than that with bilinear filtering, and
minifies a larger one through its mipmaps (an area average). So each upscale level is resized
to the on-screen size with the filter the GPU would use, composited over grey by its alpha
(the same alpha the HD maker keeps), and compared to the next level seen the same way. The
difference is 1 - SSIM on luma plus mean colour difference: 1 - SSIM below about 0.01 is
generally not visible.

The levels: realesrgan-x4plus is a 4x network, and the HD maker reaches any other size by
running it once or twice and resizing the result with Lanczos (finalize() in
upscale_textures.py). The levels here are made the same way: 4x = one pass, 16x = two chained
passes, 2x and 8x = those Lanczos-halved. Levels above 4096 px on the long edge (the largest
target any tier uses) are not made. The model per texture is the one tier_rules.py gives it.
Where the player's HD set (hd_textures/upscaled) already holds a texture at a full 16x from two
passes of that model, that image is used for 16x and 8x instead of running pass 2 again.

Views (simulated distances), two kinds:
  S<n>  the texture's long edge covers n screen pixels (world textures: the closer the camera,
        the larger n);
  M<x>  the texture is drawn x screen pixels per source texel (UI drawn at a fixed size: the PS2
        drew it at 448 lines, so 1440p shows it at about 3.2x, 4K at 4.8x, the Steam Deck at 1.8x).

Stages (each one cached under --work, so a rerun only does what is missing):
  plan      list the unique textures, their categories and levels
  upscale   run the upscaler (pass 1 on every texture, pass 2 where 8x/16x fit in 4096 px)
  evaluate  render every level at every view and measure the differences (all CPU cores)
  report    per-category tables, proposed ratios, report.md / report.json / per_texture.csv
  sheets    optional: side-by-side pictures per category for a look by eye (stay local)

The pictures this makes are derived from the game's art: they stay in --work (git-ignored) and
are never committed or shared. The report holds only numbers and file names.

Input : hd_textures/converted/textures (the game's textures as PNG, made by the setup);
        hd_textures/upscaled + upscaled_precolour (the HD set, optional: saves most of pass 2)
Output: --work (default hd_textures/work/ratio_study)
Deps  : Python 3.8+, Pillow, numpy, scipy; realesrgan-ncnn-vulkan for the upscale stage
Usage : python tools/hd_textures/measure_upscale_ratios.py [all|plan|upscale|evaluate|report|sheets]
"""

import argparse
import hashlib
import json
import math
import os
import shutil
import subprocess
import sys
import time
from collections import defaultdict
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np
from PIL import Image
from scipy.ndimage import gaussian_filter

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tier_rules  # noqa: E402

PORT = Path(__file__).resolve().parents[2]
SRC_ROOT = PORT / "hd_textures" / "converted" / "textures"
EXE = PORT / "hd_textures" / "realesrgan" / "realesrgan-ncnn-vulkan.exe"
WORK = PORT / "hd_textures" / "work" / "ratio_study"
BUILD = PORT / "hd_textures" / "upscaled"                 # the player's HD set (made by setup)
BUILD_RAW = PORT / "hd_textures" / "upscaled_precolour"   # its images before the colour match

LEVELS = (1, 2, 4, 8, 16)
MAX_LONG = 4096            # the largest target_long of any tier
MAX_VIEW = 4096           # views larger than this on screen are not measured
VISIBLE = 0.01             # 1 - SSIM above this: a step the eye can see
GREY = 128                 # background the alpha is composited over

# Simulated distances. S = long edge on screen in px, M = screen px per source texel.
VIEWS = ["S256", "S384", "S512", "S768", "S1024", "S1536", "S2048", "M1.8", "M3.2", "M4.8"]

# ---- Categories (finer than the HD maker's tiers; first match wins) ----
CATEGORY_RULES = [
    ("font",    "prefix",   "TFA2/FONTS/"),
    ("ui",      "contains", "/USERINTERFACE/"),
    ("ui",      "prefix",   "TFA2/HUD/"),
    ("ui",      "prefix",   "TA_XTRAS/"),
    ("minicon", "contains", "/MINICONS/"),
    ("player",  "prefix",   "TFA/ANIMATION/TRANSFORMERS/"),   # Hot Shot, Optimus, Red Alert (+ select screen)
    ("enemy",   "prefix",   "TFA/ANIMATION/"),                # Decepticons, drones, bosses, ships
    ("enemy",   "prefix",   "TFA2/ANIMATION/MISCELLANEOUS/"),
    ("fx",      "prefix",   "TFA2/ANIMATION/EXPLOSIONS/"),
    ("fx",      "prefix",   "TFA2/FXTXTR/"),
    ("fx",      "prefix",   "TFA/PICKUPS/"),
    ("foliage", "contains", "/ECOSYSTEM/"),
    ("foliage", "prefix",   "TFA2/ECOTEXTURES/"),
    ("sky",     "prefix",   "TFA2/SKY/"),
    ("sky",     "prefix",   "TFA2/REFLECTIONS/"),
    ("terrain", "prefix",   "TFA/LEVELS/"),                   # level geometry and detail maps
]
CATEGORY_ORDER = ["player", "enemy", "minicon", "terrain", "foliage", "sky", "fx", "ui", "font"]
CATEGORY_RANK = {c: i for i, c in enumerate(CATEGORY_ORDER)}

# The closest typical view of each category at 1440p (assumptions: change them here). World
# categories scale with the display's height; UI is drawn at a fixed size per PS2 pixel.
CLOSE_VIEW_1440 = {
    "player":  ("S", 1024),   # third-person camera, select screen, cut-scenes: a body part fills a big share
    "enemy":   ("S", 768),    # fought up close, bosses fill the screen, most seen at mid range
    "minicon": ("S", 512),    # small figures; the Mini-Con screens show them at about half the height
    "terrain": ("S", 1024),   # the ground at the player's feet and walls next to the camera
    "foliage": ("S", 768),    # plants and rocks the player walks past
    "sky":     ("S", 2048),   # a sky dome spans more than the screen
    "fx":      ("S", 512),    # particles and flashes: brief, blended, mostly small
    "ui":      ("M", 3.2),    # 448 PS2 lines -> 1440
    "font":    ("M", 3.2),
}
DISPLAYS = {"deck_800p": 800, "1080p": 1080, "1440p": 1440, "4k_2160p": 2160}


def category_for(relpath):
    r = relpath.replace("\\", "/").upper()
    probe = "/" + r
    for cat, kind, pat in CATEGORY_RULES:
        if (kind == "prefix" and r.startswith(pat)) or (kind == "contains" and pat in probe):
            return cat
    return tier_rules.tier_for(relpath)


def close_view(cat, display_h):
    kind, v = CLOSE_VIEW_1440[cat]
    if kind == "S":
        return ("S", v * display_h / 1440)
    return ("M", display_h / 448)


def view_size(view, w, h):
    """On-screen (w, h) of a w x h source texture at a view ("S512", "M3.2" or a tuple)."""
    kind, v = (view[0], float(view[1:])) if isinstance(view, str) else view
    f = v / max(w, h) if kind == "S" else v
    return max(1, round(w * f)), max(1, round(h * f))


# ---- plan ----

def alpha_class(a):
    lo = a.min()
    if lo == 255:
        return "opaque"
    if not ((a > 0) & (a < 255)).any():
        return "binary"
    return "soft"


def build_16x():
    """The textures the player's HD set already holds at a full 16x from two passes:
    {sha1: path}. Those are taken from the set instead of being made again. The copy from
    before the colour match is used where there is one, so every level is the model's own
    output (the levels made here are not colour-matched either)."""
    found = {}
    man = BUILD / "upscale_manifest.jsonl"
    if not man.exists():
        return found
    recs = {}
    for line in man.read_text().splitlines():
        r = json.loads(line)
        recs[r["sha1"]] = r   # the last record of a texture is the one in the set
    for sha1, r in recs.items():
        if r.get("passes") != 2 or r.get("fallback") or max(r["out"]) != 16 * max(r["src"]):
            continue
        rel = r["relpaths"][0]
        p = BUILD_RAW / rel if (BUILD_RAW / rel).exists() else BUILD / rel
        if p.exists():
            found[sha1] = (str(p), r["model"])
    return found


def stage_plan(work):
    built = build_16x()
    groups = defaultdict(list)
    for p in sorted(SRC_ROOT.rglob("*.png")):
        rel = p.relative_to(SRC_ROOT).as_posix()
        groups[hashlib.sha1(p.read_bytes()).hexdigest()].append(rel)
    textures = []
    for sha1, rels in sorted(groups.items()):
        with Image.open(SRC_ROOT / rels[0]) as im:
            im = im.convert("RGBA")
            w, h = im.size
            ac = alpha_class(np.asarray(im)[..., 3])
        cats = sorted({category_for(r) for r in rels}, key=lambda c: CATEGORY_RANK.get(c, 99))
        tiers = sorted({tier_rules.tier_for(r) for r in rels}, key=lambda t: tier_rules.TIER_RANK[t])
        tier = tiers[0]
        cfg = tier_rules.tiers_for("best")[tier]
        model = cfg["model"]
        if max(w, h) <= cfg.get("small_max", 0):
            model = cfg["small_model"]
        levels = [L for L in LEVELS if L == 1 or max(w, h) * L <= MAX_LONG]
        b = built.get(sha1)
        textures.append({"sha1": sha1, "rel": rels[0], "dups": len(rels), "w": w, "h": h,
                         "alpha": ac, "category": cats[0], "tier": tier, "model": model,
                         "levels": levels,
                         "build16": b[0] if b and b[1] == model and 16 in levels else None})
    (work / "plan.json").write_text(json.dumps(textures, indent=1))
    by = defaultdict(int)
    for t in textures:
        by[t["category"]] += 1
    print(f"plan: {sum(bool(t['build16']) for t in textures)} textures' 16x taken from the HD set")
    print(f"plan: {len(textures)} unique textures: " +
          ", ".join(f"{c} {by[c]}" for c in CATEGORY_ORDER if by[c]))
    return textures


def load_plan(work):
    return json.loads((work / "plan.json").read_text())


# ---- upscale ----

def run_upscaler(model, in_dir, out_dir, log):
    out_dir.mkdir(parents=True, exist_ok=True)
    cmd = [str(EXE), "-i", str(in_dir), "-o", str(out_dir), "-n", model, "-s", "4", "-f", "png"]
    t0 = time.perf_counter()
    r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                       creationflags=0x08000000 if os.name == "nt" else 0)
    dt = time.perf_counter() - t0
    if r.returncode != 0:
        log.write(r.stderr.decode("utf-8", "replace"))
        raise RuntimeError(f"upscaler failed ({model}, {in_dir}): rc {r.returncode}")
    return dt


BATCH_FILES = 64     # the HD maker's defaults (--batch-files / --batch-mp): one run over a folder
BATCH_MP = 160.0     # of 1,569 images came out mostly broken (wrong pictures under the names)


def broken(out_path, t):
    """Why an upscaled image is broken against its source ("" if it is fine): the HD maker's
    own check (judge_output: dark, flat, colours shifted, another picture)."""
    import upscale_textures  # (Pillow-only import; kept local so the tests need no upscaler)
    try:
        with Image.open(out_path) as im:
            return upscale_textures.judge_output(im.convert("RGBA"), SRC_ROOT / t["rel"])[2]
    except Exception as e:  # noqa: BLE001
        return f"unreadable: {e}"


def stage_upscale(work, textures):
    """Pass 1 (4x) on every AI texture, pass 2 (16x) where 8x fits in MAX_LONG and the HD set
    has no 16x of it. Run in the HD maker's batch sizes; every output is checked against its
    source with the maker's own check, a broken one is made again on its own, and one still
    broken is listed in broken.json and left out of the study. Times each pass per model so
    the report can give making time per category."""
    timing_path = work / "timing.json"
    timing = json.loads(timing_path.read_text()) if timing_path.exists() else {}
    bad_path = work / "broken.json"
    bad = json.loads(bad_path.read_text()) if bad_path.exists() else {}
    log = open(work / "upscaler_stderr.txt", "a", encoding="utf-8")
    # The HD set's own 16x images are checked the same way; a broken one is made here instead.
    for t in textures:
        if t.get("build16"):
            why = broken(t["build16"], t)
            if why:
                print(f"HD set image broken, made here instead: {t['rel']}: {why}")
                t["build16"] = None
    (work / "plan.json").write_text(json.dumps(textures, indent=1))

    def stage_in(t, pass_no, d):
        if pass_no == 1:
            with Image.open(SRC_ROOT / t["rel"]) as im:
                im = im.convert("RGBA")
                (im.convert("RGB") if t["alpha"] == "opaque" else im).save(d / f"{t['sha1']}.png")
            return t["w"] * t["h"]
        shutil.copyfile(work / "p1" / f"{t['sha1']}.png", d / f"{t['sha1']}.png")
        return t["w"] * t["h"] * 16

    def run(batch, pass_no, model):
        stage = work / f"stage_p{pass_no}"
        shutil.rmtree(stage, ignore_errors=True)
        stage.mkdir(parents=True)
        in_px = sum(stage_in(t, pass_no, stage) for t in batch)
        dt = run_upscaler(model, stage, work / f"p{pass_no}", log)
        shutil.rmtree(stage, ignore_errors=True)
        k = f"p{pass_no}/{model}"
        prev = timing.get(k, {"seconds": 0, "in_mp": 0, "count": 0})
        timing[k] = {"seconds": prev["seconds"] + dt, "in_mp": prev["in_mp"] + in_px / 1e6,
                     "count": prev["count"] + len(batch)}
        timing_path.write_text(json.dumps(timing, indent=1))
        failed = []
        for t in batch:
            out = work / f"p{pass_no}" / f"{t['sha1']}.png"
            why = broken(out, t) if out.exists() else "no output"
            if why:
                out.unlink(missing_ok=True)
                failed.append((t, why))
        return failed

    for pass_no in (1, 2):
        by_model = defaultdict(list)
        for t in textures:
            if t["model"].startswith("pil-") or t["sha1"] in bad:
                continue
            if pass_no == 2 and (8 not in t["levels"] or t.get("build16")):
                continue
            out = work / f"p{pass_no}" / f"{t['sha1']}.png"
            if out.exists() and not broken(out, t):
                continue
            out.unlink(missing_ok=True)
            by_model[t["model"]].append(t)
        for model, todo in sorted(by_model.items()):
            batches, cur, mp = [], [], 0.0
            for t in todo:
                omp = t["w"] * t["h"] * (16 if pass_no == 1 else 256) / 1e6
                if cur and (len(cur) >= BATCH_FILES or mp + omp > BATCH_MP):
                    batches.append(cur)
                    cur, mp = [], 0.0
                cur.append(t)
                mp += omp
            if cur:
                batches.append(cur)
            print(f"upscale pass {pass_no}: {len(todo)} textures with {model} in {len(batches)} batches",
                  flush=True)
            n_fail = 0
            for i, batch in enumerate(batches, 1):
                for t, why in run(batch, pass_no, model):
                    again = run([t], pass_no, model)
                    if again:
                        print(f"  broken twice, left out: {t['rel']}: {again[0][1]}", flush=True)
                        bad[t["sha1"]] = {"rel": t["rel"], "pass": pass_no, "why": again[0][1]}
                        bad_path.write_text(json.dumps(bad, indent=1))
                    n_fail += 1
                if i % 10 == 0:
                    print(f"  batch {i}/{len(batches)}", flush=True)
            print(f"  done; {n_fail} images were broken in their batch and made again", flush=True)
    log.close()


# ---- levels, as the HD maker would finish them ----

def finish_alpha(out, src, alpha):
    """finalize()'s alpha rules: binary re-thresholded, soft alpha the GPU flattened restored
    from the source, opaque forced to 255."""
    if alpha == "opaque":
        out.putalpha(255)
        return out
    a = np.asarray(out.getchannel("A"))
    if alpha == "binary":
        out.putalpha(Image.fromarray(np.where(a >= 128, 255, 0).astype(np.uint8)))
    elif alpha_class(a) != "soft":
        out.putalpha(src.getchannel("A").resize(out.size, Image.LANCZOS))
    return out


def make_level(t, L, work):
    with Image.open(SRC_ROOT / t["rel"]) as im:
        src = im.convert("RGBA")
    w, h = t["w"], t["h"]
    if L == 1:
        return src
    size = (w * L, h * L)
    if t["model"].startswith("pil-"):
        rs = Image.NEAREST if t["model"] == "pil-nearest" else Image.LANCZOS
        return finish_alpha(src.resize(size, rs), src, t["alpha"])
    if L <= 4:
        p = work / "p1" / f"{t['sha1']}.png"
    else:
        p = Path(t["build16"]) if t.get("build16") else work / "p2" / f"{t['sha1']}.png"
    with Image.open(p) as im:
        out = im.convert("RGBA")
    if out.size != size:
        out = out.resize(size, Image.LANCZOS)
    return finish_alpha(out, src, t["alpha"])


# ---- evaluate ----

def mip_chain(img):
    """The mipmaps the renderer builds: halved with a 2x2 average down to 1 px."""
    chain = [img]
    while max(chain[-1].size) > 1:
        w, h = chain[-1].size
        chain.append(chain[-1].resize((max(1, w // 2), max(1, h // 2)), Image.BOX))
    return chain


# Note: Pillow's BILINEAR resize from a mip level down to the screen averages over the area
# (slightly softer than the GPU's 4-tap sample). It is the same for every level, so it can
# only hide a little difference, never add one.
def on_screen(chain, size):
    """What the GPU puts on screen at this size: bilinear magnification, trilinear
    minification (two mip levels sampled bilinearly and blended); then composited over grey.
    Returns float32 RGB in 0..255."""
    base = chain[0]
    s = max(size) / max(base.size)
    if s >= 1:
        img = base if size == base.size else base.resize(size, Image.BILINEAR)
        a = np.asarray(img, dtype=np.float32)
    else:
        lod = math.log2(1 / s)
        k = min(int(lod), len(chain) - 1)
        f = lod - k
        def at(m):  # noqa: E306
            im = chain[min(m, len(chain) - 1)]
            return np.asarray(im if im.size == size else im.resize(size, Image.BILINEAR), dtype=np.float32)
        a = at(k) if f < 1e-6 else at(k) * (1 - f) + at(k + 1) * f
    al = a[..., 3:4] / 255.0
    return a[..., :3] * al + GREY * (1 - al)


def luma(rgb):
    return rgb[..., 0] * 0.299 + rgb[..., 1] * 0.587 + rgb[..., 2] * 0.114


def ssim(x, y):
    """Mean SSIM of two luma images (Wang et al. 2004, Gaussian window sigma 1.5)."""
    if min(x.shape) < 8:   # too small for a window: fall back to a plain comparison
        return float(1 - np.abs(x - y).mean() / 255.0)
    C1, C2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    g = lambda v: gaussian_filter(v, 1.5, truncate=3.5)  # noqa: E731
    mx, my = g(x), g(y)
    sxx = g(x * x) - mx * mx
    syy = g(y * y) - my * my
    sxy = g(x * y) - mx * my
    m = ((2 * mx * my + C1) * (2 * sxy + C2)) / ((mx * mx + my * my + C1) * (sxx + syy + C2))
    return float(m.mean())


def compare(a, b):
    """(1 - SSIM on luma, mean absolute colour difference in 0..255 units)."""
    return 1.0 - ssim(luma(a), luma(b)), float(np.abs(a - b).mean())


def evaluate_one(args):
    t, work_s = args
    work = Path(work_s)
    try:
        return _evaluate(t, work)
    except Exception as e:  # noqa: BLE001 - one bad texture must not end the study
        return {"sha1": t["sha1"], "error": f"{type(e).__name__}: {e}"}


def _evaluate(t, work):
    levels = {L: make_level(t, L, work) for L in t["levels"]}
    chains = {L: mip_chain(img) for L, img in levels.items()}
    top = max(levels)
    res = {"sha1": t["sha1"], "views": {}}
    for v in VIEWS:
        size = view_size(v, t["w"], t["h"])
        if max(size) > MAX_VIEW or min(size) < 8:
            continue   # larger than any screen, or too small for the SSIM window
        shown = {L: on_screen(ch, size) for L, ch in chains.items()}
        step, ref = {}, {}
        Ls = sorted(shown)
        for lo, hi in zip(Ls, Ls[1:]):
            step[f"{lo}-{hi}"] = compare(shown[lo], shown[hi])
        for L in Ls:
            ref[str(L)] = compare(shown[L], shown[top])
        rec = {"size": size, "step": step, "vs_top": ref}
        # Where the screen shows no more than one pass gives (4x covers the view), any difference
        # between 4x and 16x is the second pass changing the look, not added resolution.
        if 16 in shown and max(size) <= 4 * max(t["w"], t["h"]):
            rec["pass2_look"] = compare(shown[4], shown[16])
        res["views"][v] = rec
    return res


def stage_evaluate(work, textures, jobs):
    out_path = work / "metrics.jsonl"
    done = set()
    if out_path.exists():
        for r in read_jsonl(out_path):
            if "error" not in r:
                done.add(r["sha1"])
    bad_path = work / "broken.json"
    bad = json.loads(bad_path.read_text()) if bad_path.exists() else {}
    todo = [t for t in textures if t["sha1"] not in done and t["sha1"] not in bad]
    print(f"evaluate: {len(todo)} textures x {len(VIEWS)} views on {jobs} processes", flush=True)
    t0 = time.perf_counter()
    with open(out_path, "a") as f, ProcessPoolExecutor(jobs) as ex:
        for i, r in enumerate(ex.map(evaluate_one, [(t, str(work)) for t in todo], chunksize=4), 1):
            f.write(json.dumps(r) + "\n")
            f.flush()
            if "error" in r:
                print(f"  error {r['sha1']}: {r['error']}", flush=True)
            if i % 100 == 0:
                print(f"  {i}/{len(todo)} ({time.perf_counter() - t0:.0f} s)", flush=True)


def load_metrics(work):
    return {r["sha1"]: r for r in read_jsonl(work / "metrics.jsonl") if "error" not in r}


def read_jsonl(path):
    """Records of a JSON-lines file; a line torn by a killed run is skipped."""
    out = []
    for line in path.read_text().splitlines():
        try:
            out.append(json.loads(line))
        except json.JSONDecodeError:
            pass
    return out


# ---- report ----

def interp_view(r, t, view):
    """Metrics of texture t at an arbitrary close view (kind, value): interpolated (in log of
    the on-screen size) between the two measured views around it; clamped at the ends."""
    target = max(view_size(view, t["w"], t["h"]))
    pts = sorted(r["views"].values(), key=lambda v: max(v["size"]))
    sizes = [max(v["size"]) for v in pts]
    if target <= sizes[0]:
        return pts[0]
    if target >= sizes[-1]:
        return pts[-1]
    i = next(k for k in range(1, len(pts)) if sizes[k] >= target)
    a, b = pts[i - 1], pts[i]
    if sizes[i] == sizes[i - 1]:
        return b
    f = math.log(target / sizes[i - 1]) / math.log(sizes[i] / sizes[i - 1])
    mix = lambda x, y: [x[0] + (y[0] - x[0]) * f, x[1] + (y[1] - x[1]) * f]  # noqa: E731
    return {"size": [target, target],
            "step": {k: mix(a["step"][k], b["step"][k]) for k in a["step"] if k in b["step"]},
            "vs_top": {k: mix(a["vs_top"][k], b["vs_top"][k]) for k in a["vs_top"] if k in b["vs_top"]}}


def needed_level(view_metrics, levels):
    """Smallest level after which no further 2x step is visible at this view."""
    Ls = sorted(levels)
    need = Ls[0]
    for lo, hi in zip(Ls, Ls[1:]):
        d, _ = view_metrics["step"][f"{lo}-{hi}"]
        if d > VISIBLE:
            need = hi
    return need


def pct(vals, q):
    return float(np.percentile(vals, q)) if vals else float("nan")


def bytes_bc7(w, h):
    return w * h * 4 / 3   # BC7: 1 byte per pixel, plus a third for the mipmaps


def stage_report(work, textures):
    metrics = load_metrics(work)
    timing = json.loads((work / "timing.json").read_text()) if (work / "timing.json").exists() else {}
    sec_per_mp = {k: v["seconds"] / v["in_mp"] for k, v in timing.items() if v["in_mp"]}
    for t in textures:
        for k in ([f"p1/{t['model']}"] + ([f"p2/{t['model']}"] if 8 in t["levels"] else [])):
            if not t["model"].startswith("pil-") and k not in sec_per_mp:
                print(f"report: no time measured for {k}: its GPU seconds count as 0")
                sec_per_mp[k] = 0.0
    tex = [t for t in textures if t["sha1"] in metrics]
    by_cat = defaultdict(list)
    for t in tex:
        by_cat[t["category"]].append(t)

    report = {"visible_threshold": VISIBLE, "views": VIEWS, "close_view_1440": CLOSE_VIEW_1440,
              "timing": timing, "categories": {}}
    rows = []
    for cat in CATEGORY_ORDER:
        ts = by_cat.get(cat, [])
        if not ts:
            continue
        c = {"count": len(ts), "src_long": sorted({max(t["w"], t["h"]) for t in ts}), "steps": {},
             "needed": {}, "proposal": {}}
        # Step visibility per view: median / p90 of 1 - SSIM, share of textures where it shows.
        for v in VIEWS:
            per = {}
            for step in ("1-2", "2-4", "4-8", "8-16"):
                vals = [metrics[t["sha1"]]["views"][v]["step"][step][0] for t in ts
                        if v in metrics[t["sha1"]]["views"]
                        and step in metrics[t["sha1"]]["views"][v]["step"]]
                if vals:
                    per[step] = {"n": len(vals), "median": pct(vals, 50), "p90": pct(vals, 90),
                                 "visible_share": sum(x > VISIBLE for x in vals) / len(vals)}
            c["steps"][v] = per
        # Level each texture needs at the category's close view, per display.
        for disp, hgt in DISPLAYS.items():
            view = close_view(cat, hgt)
            needs = [needed_level(interp_view(metrics[t["sha1"]], t, view), t["levels"]) for t in ts]
            hist = {str(L): needs.count(L) for L in LEVELS}
            # The category ratio: the smallest level that covers 90% of its textures.
            srt = sorted(needs)
            ratio = srt[min(len(srt) - 1, math.ceil(0.9 * len(srt)) - 1)]
            c["needed"][disp] = {"view": list(view), "hist": hist, "p90_ratio": ratio}
        # Cost of each uniform ratio for the category (capped per texture by its levels / 4096).
        cost = {}
        for L in LEVELS:
            px = mp_in = secs = 0.0
            residual = []
            for t in ts:
                lv = max(x for x in t["levels"] if x <= L)
                px += bytes_bc7(t["w"] * lv, t["h"] * lv)  # duplicates are made once
                if lv > 1 and not t["model"].startswith("pil-"):
                    secs += t["w"] * t["h"] / 1e6 * sec_per_mp.get(f"p1/{t['model']}", 0)
                    if lv > 4:
                        secs += t["w"] * t["h"] * 16 / 1e6 * sec_per_mp.get(f"p2/{t['model']}", 0)
                # How far this ratio leaves the texture from the best level at 1440p's close view.
                vm = interp_view(metrics[t["sha1"]], t, close_view(cat, 1440))
                residual.append(vm["vs_top"][str(lv)][0])
            cost[str(L)] = {"bc7_mb": px / 2**20, "gpu_s": secs,
                            "residual_vs_best_1440_median": pct(residual, 50),
                            "residual_vs_best_1440_p90": pct(residual, 90)}
        c["cost"] = cost
        report["categories"][cat] = c
        for t in ts:
            r = metrics[t["sha1"]]
            row = {"rel": t["rel"], "category": cat, "tier": t["tier"], "w": t["w"], "h": t["h"],
                   "alpha": t["alpha"], "model": t["model"]}
            for disp, hgt in DISPLAYS.items():
                row[f"need_{disp}"] = needed_level(interp_view(r, t, close_view(cat, hgt)), t["levels"])
            for v in ("S512", "S1024", "M3.2"):
                for step, (d, _) in r["views"].get(v, {"step": {}})["step"].items():
                    row[f"{v}_{step}"] = round(d, 5)
            rows.append(row)

    (work / "report.json").write_text(json.dumps(report, indent=1))
    keys = sorted({k for r in rows for k in r}, key=lambda k: (k not in ("rel", "category"), k))
    with open(work / "per_texture.csv", "w", encoding="utf-8") as f:
        f.write(",".join(keys) + "\n")
        for r in rows:
            f.write(",".join(str(r.get(k, "")) for k in keys) + "\n")
    md = render_markdown(report)
    (work / "report.md").write_text(md, encoding="utf-8")
    print(f"report: {work / 'report.md'}")


def render_markdown(rep):
    L = []
    w = L.append
    w(f"# Upscale ratio study\n\nA step is counted as visible when 1 - SSIM > {rep['visible_threshold']} "
      "between the two levels as seen on screen.\n")
    t = rep["timing"]
    if t:
        w("## Upscaler time on this PC\n\n| Pass / model | Textures | MP in | Seconds | s per MP in |\n|---|---|---|---|---|")
        for k, v in sorted(t.items()):
            w(f"| {k} | {v['count']} | {v['in_mp']:.1f} | {v['seconds']:.0f} | {v['seconds'] / max(v['in_mp'], 1e-9):.2f} |")
        w("")
    w("## Proposed ratio per category (smallest 2x level that leaves 90% of the category with no visible next step)\n")
    w("| Category | Textures | Source long edges | " + " | ".join(DISPLAYS) + " |")
    w("|---|---|---|" + "---|" * len(DISPLAYS))
    for cat, c in rep["categories"].items():
        w(f"| {cat} | {c['count']} | {', '.join(map(str, c['src_long']))} | " +
          " | ".join(f"{c['needed'][d]['p90_ratio']}x" for d in DISPLAYS) + " |")
    w("\nClose view assumed at 1440p (scaled with the display height for S views):\n")
    for cat, (k, v) in rep["close_view_1440"].items():
        w(f"- {cat}: {'long edge on screen ' + str(v) + ' px' if k == 'S' else str(v) + ' screen px per texel'}")
    w("\n## Cost and remaining difference per uniform ratio (1440p close view)\n")
    w("| Category | Ratio | BC7 MB | GPU s | 1-SSIM to best, median | p90 |\n|---|---|---|---|---|---|")
    for cat, c in rep["categories"].items():
        for Lv, v in c["cost"].items():
            w(f"| {cat} | {Lv}x | {v['bc7_mb']:.0f} | {v['gpu_s']:.0f} | "
              f"{v['residual_vs_best_1440_median']:.4f} | {v['residual_vs_best_1440_p90']:.4f} |")
    w("\n## Step visibility by view (share of textures where the step shows; median 1-SSIM)\n")
    for cat, c in rep["categories"].items():
        w(f"### {cat}\n\n| View | 1x→2x | 2x→4x | 4x→8x | 8x→16x |\n|---|---|---|---|---|")
        for v in rep["views"]:
            cells = []
            for s in ("1-2", "2-4", "4-8", "8-16"):
                x = c["steps"][v].get(s)
                cells.append(f"{x['visible_share']:.0%} ({x['median']:.4f})" if x else "–")
            w(f"| {v} | " + " | ".join(cells) + " |")
        w("")
    return "\n".join(L)


# ---- sheets (by-eye check, local only) ----

def stage_sheets(work, textures, per_cat=6):
    out = work / "sheets"
    out.mkdir(exist_ok=True)
    metrics = load_metrics(work)
    by_cat = defaultdict(list)
    for t in textures:
        if t["sha1"] in metrics:
            by_cat[t["category"]].append(t)
    for cat, ts in by_cat.items():
        ts = sorted(ts, key=lambda t: t["sha1"])[:: max(1, len(ts) // per_cat)][:per_cat]
        view = close_view(cat, 1440)
        tiles = []
        for t in ts:
            size = view_size(view, t["w"], t["h"])
            size = (min(size[0], 768), min(size[1], 768))
            row = [on_screen(mip_chain(make_level(t, L, work)), size) for L in t["levels"]]
            tiles.append(np.concatenate(row, axis=1))
        wmax = max(x.shape[1] for x in tiles)
        tiles = [np.pad(x, ((0, 8), (0, wmax - x.shape[1]), (0, 0)), constant_values=GREY) for x in tiles]
        Image.fromarray(np.concatenate(tiles).clip(0, 255).astype(np.uint8)).save(out / f"{cat}.png")
    print(f"sheets: {out}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("stage", nargs="?", default="all",
                    choices=["all", "plan", "upscale", "evaluate", "report", "sheets"])
    ap.add_argument("--work", type=Path, default=WORK)
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument("--limit", type=int, default=0, help="only the first N textures (a quick try)")
    a = ap.parse_args()
    a.work.mkdir(parents=True, exist_ok=True)
    textures = stage_plan(a.work) if a.stage in ("all", "plan") or not (a.work / "plan.json").exists() \
        else load_plan(a.work)
    if a.limit:
        textures = textures[:: max(1, len(textures) // a.limit)][: a.limit]
    if a.stage in ("all", "upscale"):
        stage_upscale(a.work, textures)
    if a.stage in ("all", "evaluate"):
        stage_evaluate(a.work, textures, a.jobs)
    if a.stage in ("all", "report"):
        stage_report(a.work, textures)
    if a.stage == "sheets":
        stage_sheets(a.work, textures)


if __name__ == "__main__":
    main()
