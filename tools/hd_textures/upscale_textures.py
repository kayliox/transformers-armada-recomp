#!/usr/bin/env python3
"""Batch AI-upscale converted PS2 textures (ncnn-Vulkan or ONNX/DirectML).

Purpose: Phase-2 pipeline driver. Walks a tree of RGBA PNGs (output of
itf_to_png.py), classifies each into a tier (tier_rules.py), dedups
byte-identical files by SHA-1, drives the chosen backend in chained 4x
passes, then finalizes to exact per-tier target size (Lanczos
supersample-downscale when overshooting) into --out, mirroring source
relative paths 1:1. Resumable via a JSONL manifest stored with the outputs;
--dry-run plans without touching the GPU; --audit verifies a finished run.

Works in batches (--batch-files / --batch-mp), the textures a player sees
first made first (tier_rules.priority_for), each finished batch usable at
once; --match-colour matches each image's colour as it is made. Beside the
game (TODO 219): it waits while <hd-dir>/.game_running says the game runs,
unless <hd-dir>/hd_options.json says "while_playing": true (then the
upscaler runs at idle CPU and GPU priority), and keeps
<hd-dir>/upscale_status.json up to date. --quality fast: tier_rules.FAST_TIERS.

Backends (--backend, default = the model's own backend, else ncnn):
  ncnn      realesrgan-ncnn-vulkan.exe, batch directory mode (AMD via Vulkan).
  onnx-dml  onnxruntime InferenceSession(providers=[DmlExecutionProvider,
            CPUExecutionProvider]) — runs modern CNN SR (SPAN/Compact/
            RealPLKSR) and PS2-era ESRGAN (HDcube/RealisticRescaler) on the
            AMD GPU with NO CUDA. Import-guarded: onnxruntime + the model
            file are the USER's install step (pip install onnxruntime-directml
            + drop .onnx under ThirdParty/onnx_models); absent -> a clear
            message + non-zero exit, never a traceback, and the ncnn path is
            unaffected.
See --list-models for the registry; --model NAME overrides tier routing.

Input : Extracted/converted/textures (PNG tree)
Output: Extracted/upscaled/... (mirrored PNG tree) + upscale_manifest.jsonl
Deps  : Python 3.8+, Pillow; ncnn backend -> realesrgan-ncnn-vulkan.exe;
        onnx-dml backend -> onnxruntime-directml + numpy (both optional,
        import-guarded). None needed for pil-* tiers, --dry-run, --audit,
        --list-models.
"""

import argparse
import hashlib
import io
import json
import math
import os
import random
import re
import shutil
import subprocess
import sys
import time
from collections import Counter, defaultdict
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

from PIL import Image, ImageChops, ImageStat

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tier_rules

PROJECT_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SRC = PROJECT_ROOT / "Extracted" / "converted" / "textures"
DEFAULT_OUT = PROJECT_ROOT / "Extracted" / "upscaled"
DEFAULT_WORK = PROJECT_ROOT / "Extracted" / "work" / "upscale"
DEFAULT_EXE = PROJECT_ROOT / "ThirdParty" / "realesrgan" / ("realesrgan-ncnn-vulkan.exe" if sys.platform == "win32" else "realesrgan-ncnn-vulkan")
MANIFEST_NAME = "upscale_manifest.jsonl"
SIZE_BUCKETS = (32, 64, 128, 256, 512, 1024)
# Reference counts from the 2026-07-02 source scan; drift = warn, not fail.
REF_ALPHA = {"opaque": 1410, "binary": 168, "soft": 215}

# User-supplied ONNX models live here (git-ignored, like the rest of ThirdParty).
ONNX_MODEL_DIR = PROJECT_ROOT / "ThirdParty" / "onnx_models"

# Model registry (research MODERNIZATION_RESEARCH.md §1.1-1.2). name -> metadata:
#   arch           network family (documentation only).
#   scale          per-pass upscale factor (all wired models are 4x; the pass
#                  chaining math in plan_jobs assumes 4x).
#   backend        native backend: ncnn (built into ThirdParty/realesrgan) |
#                  onnx-dml (user .onnx under ThirdParty/onnx_models) | pil.
#   class_affinity which texture class it's best on (routing guidance).
#   file           onnx filename under --onnx-model-dir (None = ncnn/pil built-in).
#   note           where to obtain / how to supply it.
# The ncnn/pil models keep the current behavior. The onnx-dml models are
# USER-SUPPLIED (convert a .pth from OpenModelDB/Phhofm/Venomalia to .onnx, or
# grab a prebuilt .onnx) — the code degrades gracefully when they are absent.
MODELS = {
    "realesrgan-x4plus": dict(
        arch="RRDBNet", scale=4, backend="ncnn",
        class_affinity="general / photographic (current default)",
        file=None, note="ncnn built-in (ThirdParty/realesrgan model dir)"),
    "realesr-animevideov3": dict(
        arch="SRVGG", scale=4, backend="ncnn",
        class_affinity="tiny <=32px / soft sources (tier small_model)",
        file=None, note="ncnn built-in"),
    "HDcube4": dict(
        arch="ESRGAN", scale=4, backend="onnx-dml",
        class_affinity="BC1/DXT1 GC/Wii/PS2-era compression artifacts (bulk)",
        file="HDcube4.onnx",
        note="USER-SUPPLIED: Venomalia/HDcube .pth->.onnx into ThirdParty/onnx_models/"),
    "4xRealisticRescaler": dict(
        arch="ESRGAN", scale=4, backend="onnx-dml",
        class_affinity="compressed GameCube/PS2-era BC1/JPEG textures",
        file="4xRealisticRescaler.onnx",
        note="USER-SUPPLIED: OpenModelDB 4x-RealisticRescaler .onnx"),
    "4xNomosUni-SPAN": dict(
        arch="SPAN", scale=4, backend="onnx-dml",
        class_affinity="env diffuse, fast bulk (~21fps)",
        file="4xNomosUni_span.onnx",
        note="USER-SUPPLIED: Phhofm 4xNomosUni SPAN .onnx"),
    "4xCompact": dict(
        arch="Compact/SRVGG", scale=4, backend="onnx-dml",
        class_affinity="fastest bulk CNN (~26fps)",
        file="4xCompact.onnx",
        note="USER-SUPPLIED: a Compact/SRVGG .onnx (e.g. 4xNomos8kSCCompact)"),
    "pil-lanczos": dict(
        arch="lanczos", scale=4, backend="pil",
        class_affinity="tiny geometric UI (never hallucinate)",
        file=None, note="Pillow resample, no GPU"),
    "pil-nearest": dict(
        arch="nearest", scale=4, backend="pil",
        class_affinity="pixel-exact", file=None, note="Pillow resample, no GPU"),
}

ONNX_GUIDANCE = (
    "error: --backend onnx-dml requested but onnxruntime is not installed.\n"
    "  Install (AMD/DirectML, no CUDA):  pip install onnxruntime-directml\n"
    "  Then drop user-supplied .onnx models under ThirdParty/onnx_models/\n"
    "  (see --list-models). The default --backend ncnn path needs none of this."
)


def resolve_backend(model, requested_backend):
    """Effective backend for a model. pil-* is always pil. An explicit
    --backend forces the choice (e.g. HDcube ships both ncnn + onnx forms);
    otherwise the registry's native backend, defaulting to ncnn for unknown
    names so the existing --model-override pilot use is unchanged."""
    if model.startswith("pil-"):
        return "pil"
    if requested_backend:
        return requested_backend
    m = MODELS.get(model)
    return m["backend"] if m else "ncnn"


def onnx_model_path(model, model_dir):
    """Path to the .onnx file for a model (None if the model has no onnx build)."""
    m = MODELS.get(model)
    fname = m["file"] if m else f"{model}.onnx"
    return Path(model_dir) / fname if fname else None


def import_onnxruntime():
    """Import-guard: return the onnxruntime module, or None if unavailable
    (not installed, or a DLL/load failure) so callers can degrade cleanly."""
    try:
        import onnxruntime as ort
        return ort
    except Exception:  # noqa: BLE001 - any import/DLL failure = "not available"
        return None


def list_models():
    print("Upscale model registry  (--model NAME [--backend {ncnn,onnx-dml}])")
    print(f"onnx model dir: {ONNX_MODEL_DIR}\n")
    for name, m in MODELS.items():
        loc = "built-in" if not m["file"] else f"onnx_models/{m['file']}"
        print(f"  {name:22s} {m['backend']:8s} {m['arch']:14s} x{m['scale']}  "
              f"{m['class_affinity']}")
        print(f"      {loc}  -- {m['note']}")


@dataclass
class Src:
    relpath: str  # posix-style, relative to the source root
    path: Path
    sha1: str
    w: int
    h: int
    alpha: str  # opaque | binary | soft
    tier: str


@dataclass
class Job:
    sha1: str
    srcs: list  # all byte-identical Srcs (>=1)
    tier: str
    model: str
    passes: int  # chained 4x passes (0 for pil-* pseudo-models)
    out_w: int
    out_h: int
    alpha: str  # class of the (identical) sources
    alpha_mode: str  # rgb | direct | split | pil
    backend: str = "ncnn"  # ncnn | onnx-dml | pil

    @property
    def relpaths(self):
        return sorted(s.relpath for s in self.srcs)


def alpha_class(img):
    a = img.getchannel("A")
    lo, _hi = a.getextrema()
    if lo == 255:
        return "opaque"
    if sum(a.histogram()[1:255]) == 0:
        return "binary"
    return "soft"


def size_bucket(long_edge):
    for b in SIZE_BUCKETS:
        if long_edge <= b:
            return b
    return SIZE_BUCKETS[-1]


def scan_sources(root, errors):
    srcs = []
    for p in sorted(root.rglob("*.png")):
        rel = p.relative_to(root).as_posix()
        try:
            sha1 = hashlib.sha1(p.read_bytes()).hexdigest()
            with Image.open(p) as im:
                if im.mode != "RGBA":
                    im = im.convert("RGBA")
                w, h = im.size
                ac = alpha_class(im)
            srcs.append(Src(rel, p, sha1, w, h, ac, tier_rules.tier_for(rel)))
        except Exception as e:  # noqa: BLE001 - per-file capture, batch continues
            errors.append((rel, f"scan: {e}"))
    return srcs


def sample_srcs(srcs, n, seed):
    """Seeded per-tier sample, stratified across (size bucket, alpha class)
    so pilots see every content type, not just the dominant 128px-opaque."""
    rnd = random.Random(seed)
    per_tier = defaultdict(lambda: defaultdict(list))
    for s in srcs:
        per_tier[s.tier][(size_bucket(max(s.w, s.h)), s.alpha)].append(s)
    keep = []
    for _tier, strata in sorted(per_tier.items()):
        for lst in strata.values():
            lst.sort(key=lambda s: s.relpath)
            rnd.shuffle(lst)
        order = sorted(strata.keys())
        picked = []
        while len(picked) < n and any(strata[k] for k in order):
            for k in order:
                if strata[k] and len(picked) < n:
                    picked.append(strata[k].pop())
        keep.extend(picked)
    return keep


def plan_jobs(srcs, model_override, alpha_mode, notes, requested_backend=None, quality="best"):
    by_sha = defaultdict(list)
    for s in srcs:
        by_sha[s.sha1].append(s)
    jobs = []
    coerced = False
    for sha1, group in sorted(by_sha.items()):
        tiers = {s.tier for s in group}
        tier = min(tiers, key=lambda t: tier_rules.TIER_RANK[t])
        if len(tiers) > 1:
            notes.append(f"dup spans tiers {sorted(tiers)} -> {tier}: "
                         f"{group[0].relpath} (+{len(group) - 1})")
        cfg = tier_rules.tiers_for(quality)[tier]
        w, h = group[0].w, group[0].h
        long_edge = max(w, h)
        model = cfg["model"]
        if long_edge <= cfg.get("small_max", 0):
            model = cfg["small_model"]
        model = model_override or model
        backend = resolve_backend(model, requested_backend)
        target = cfg["target_long"]
        if backend == "pil":
            passes = 0
            final_long = min(target, long_edge * 4 ** cfg["passes_cap"])
        else:
            needed = target / long_edge
            passes = 1 if needed <= 1 else min(
                cfg["passes_cap"], max(1, math.ceil(math.log(needed, 4))))
            final_long = min(target, long_edge * 4 ** passes)
        final_long = max(final_long, long_edge)  # never below source size
        f = final_long / long_edge
        out_w = max(1, round(w * f))
        out_h = max(1, round(h * f))
        if backend == "pil":
            amode = "pil"
        elif group[0].alpha == "opaque":
            amode = "rgb"
        elif backend == "onnx-dml" and alpha_mode == "direct":
            # A generic RGB onnx model can't carry a 4th channel through, so
            # non-opaque textures upscale RGB + alpha separately (research §1.3).
            amode = "split"
            coerced = True
        else:
            amode = alpha_mode
        jobs.append(Job(sha1, group, tier, model, passes,
                        out_w, out_h, group[0].alpha, amode, backend))
    if coerced:
        notes.append("onnx-dml: non-opaque textures forced to 'split' "
                     "alpha mode (RGB + alpha upscaled separately)")
    return jobs


def load_manifest(out_root):
    recs = {}
    mf = out_root / MANIFEST_NAME
    if mf.exists():
        for line in mf.read_text(encoding="utf-8").splitlines():
            try:
                r = json.loads(line)
                recs[r["sha1"]] = r  # last record wins
            except (ValueError, KeyError):
                continue
    return recs


def job_done(job, recs, out_root, settings, also_good=()):
    r = recs.get(job.sha1)
    if not r:
        return False
    # (TODO 241: a texture this card could not make well is left to the game's own image, not tried each run)
    if r.get("fallback") == "none" and r.get("settings") == settings:
        return True
    # (A Fast run keeps the images a Best run made already: better ones, not made again.)
    if (r.get("settings") in also_good and sorted(r.get("relpaths", [])) == job.relpaths
            and all((out_root / rp).is_file() for rp in r["relpaths"])):
        return True
    if (r.get("settings") != settings or r.get("model") != job.model
            or r.get("passes") != job.passes
            or r.get("alpha_mode") != job.alpha_mode
            or r.get("out") != [job.out_w, job.out_h]
            or sorted(r.get("relpaths", [])) != job.relpaths):
        return False
    # Backward-compat: pre-backend manifests lack "backend"; only invalidate
    # when a record explicitly records a different backend (same model name
    # run under both ncnn and onnx, e.g. HDcube's dual build).
    if "backend" in r and r["backend"] != job.backend:
        return False
    return all((out_root / rp).is_file() for rp in r["relpaths"])


def expected_stems(job):
    if job.alpha_mode == "split":
        return [f"{job.sha1}.rgb", f"{job.sha1}.a"]
    return [job.sha1]


def stage_job(job, in_dir):
    with Image.open(job.srcs[0].path) as im:
        im = im.convert("RGBA")
        if job.alpha_mode == "rgb":
            # Opaque: strip alpha so the binary runs its native 3-channel
            # path; constant-255 alpha is re-attached at finalize.
            im.convert("RGB").save(in_dir / f"{job.sha1}.png")
        elif job.alpha_mode == "direct":
            im.save(in_dir / f"{job.sha1}.png")
        else:  # split
            im.convert("RGB").save(in_dir / f"{job.sha1}.rgb.png")
            a = im.getchannel("A")
            Image.merge("RGB", (a, a, a)).save(in_dir / f"{job.sha1}.a.png")


def run_binary(exe, in_dir, out_dir, model, tile, gpu, stop=None, gentle=False):
    """The upscaler over a directory. stop(): polled every second - True ends the run at once (the game
    started: its batch is made again later). gentle: idle CPU priority and the graphics card's lowest
    scheduling class for the upscaler (made while the game runs), small tiles. Returns
    (returncode, stderr text, stopped)."""
    cmd = [str(exe), "-i", str(in_dir), "-o", str(out_dir),
           "-n", model, "-s", "4", "-f", "png"]
    if sys.platform != "win32":
        cmd += ["-m", str(exe.resolve().parent / "models")]
    if tile or gentle:
        cmd += ["-t", str(tile or 128)]
    if gpu is not None:
        cmd += ["-g", str(gpu)]
    flags = 0
    if os.name == "nt":
        flags |= 0x08000000  # CREATE_NO_WINDOW
        flags |= 0x00000040 if gentle else 0x00004000  # IDLE / BELOW_NORMAL priority class
    errf = io.open(os.path.join(str(out_dir), "..", f"stderr_{os.getpid()}.txt"), "w+", encoding="utf-8", errors="replace")
    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=errf, creationflags=flags)
        if gentle:
            gpu_priority_idle(proc)
        stopped = False
        while True:
            try:
                rc = proc.wait(timeout=1.0)
                break
            except subprocess.TimeoutExpired:
                if stop and stop():
                    proc.kill()
                    rc = proc.wait()
                    stopped = True
                    break
        errf.seek(0)
        return rc, errf.read(), stopped
    finally:
        errf.close()
        try:
            os.remove(errf.name)
        except OSError:
            pass


def gpu_priority_idle(proc):
    """The upscaler's work on the graphics card at Windows' lowest scheduling class (the game's frames
    go first). Best effort: nothing happens where Windows does not offer it."""
    if os.name != "nt":
        return
    try:
        import ctypes
        gdi = ctypes.WinDLL("gdi32")
        fn = gdi.D3DKMTSetProcessSchedulingPriorityClass
        fn.argtypes = [ctypes.c_void_p, ctypes.c_int]
        fn(ctypes.c_void_p(int(proc._handle)), 0)  # D3DKMT_SCHEDULINGPRIORITYCLASS_IDLE
    except Exception:  # noqa: BLE001
        pass


# ---- Working beside the game (make_hd_textures.ps1 / the setup) ----
# The game holds <hd_textures>/.game_running (its process id inside) while it runs; Windows deletes the file
# when the game ends, a crash too. While it is there the upscale waits (the two share the graphics card),
# unless hd_options.json says "while_playing": true - then it carries on gently.

def pid_alive(pid):
    if pid <= 0:
        return False
    if os.name != "nt":
        try:
            os.kill(pid, 0)
            return True
        except OSError:
            return False
    import ctypes
    k = ctypes.WinDLL("kernel32")
    k.OpenProcess.restype = ctypes.c_void_p
    h = k.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return False
    try:
        code = ctypes.c_ulong()
        ok = k.GetExitCodeProcess(ctypes.c_void_p(h), ctypes.byref(code))
        return bool(ok) and code.value == 259  # STILL_ACTIVE
    finally:
        k.CloseHandle(ctypes.c_void_p(h))


def game_running(lock):
    if not lock or not lock.exists():
        return False
    try:
        text = lock.read_text(encoding="utf-8", errors="replace").strip()
    except OSError:
        return True  # (held and unreadable: the game is there)
    try:
        return pid_alive(int(text.split()[0]))
    except (ValueError, IndexError):
        return True


def read_options(path):
    try:
        with open(path, encoding="utf-8-sig") as fp:
            o = json.load(fp)
        return o if isinstance(o, dict) else {}
    except (OSError, ValueError):
        return {}


def write_status(path, **fields):
    if not path:
        return
    fields["updated"] = datetime.now(timezone.utc).isoformat(timespec="seconds")
    tmp = path.with_name(path.name + ".tmp")
    try:
        tmp.write_text(json.dumps(fields, indent=1), encoding="utf-8")
        os.replace(tmp, path)
    except OSError:
        pass


def job_priority(job):
    return min(tier_rules.priority_for(rp) for rp in job.relpaths)


def make_batches(jobs, max_files, max_mp):
    """The jobs in the order they are made (the textures seen first, first), cut into batches of one
    (backend, model, passes) each: at most max_files images and max_mp megapixels of output."""
    order = sorted(jobs, key=lambda j: (job_priority(j), j.backend, j.model, j.passes, j.relpaths[0]))
    batches, cur, key, mp = [], [], None, 0.0
    for j in order:
        k = (j.backend, j.model, j.passes)
        jmp = j.out_w * j.out_h / 1e6
        if cur and (k != key or len(cur) >= max_files or mp + jmp > max_mp):
            batches.append(cur)
            cur, mp = [], 0.0
        cur.append(j)
        key = k
        mp += jmp
    if cur:
        batches.append(cur)
    return batches


def make_onnx_session(ort, model_path):
    """Load an ONNX model on DirectML (AMD GPU, no CUDA), CPU fallback."""
    return ort.InferenceSession(
        str(model_path),
        providers=["DmlExecutionProvider", "CPUExecutionProvider"])


def _onnx_infer_tiled(session, inp_name, out_name, arr, scale, tile, pad):
    """One image (H,W,3 float32 in [0,1]) -> upscaled (H*scale,W*scale,3).
    Tiled with padded borders so large images fit VRAM and tile seams are
    hidden (each padded tile's valid centre is cropped back out)."""
    import numpy as np
    h, w, _ = arr.shape
    out = np.zeros((h * scale, w * scale, 3), dtype=np.float32)
    step = tile if tile and tile > 0 else max(h, w)
    for y0 in range(0, h, step):
        for x0 in range(0, w, step):
            y1, x1 = min(y0 + step, h), min(x0 + step, w)
            py0, py1 = max(0, y0 - pad), min(h, y1 + pad)
            px0, px1 = max(0, x0 - pad), min(w, x1 + pad)
            patch = arr[py0:py1, px0:px1, :]
            inp = np.transpose(patch, (2, 0, 1))[None].astype(np.float32)
            res = session.run([out_name], {inp_name: inp})[0]
            res = np.clip(res[0].transpose(1, 2, 0), 0.0, 1.0)
            cy0, cx0 = (y0 - py0) * scale, (x0 - px0) * scale
            out[y0 * scale:y1 * scale, x0 * scale:x1 * scale, :] = \
                res[cy0:cy0 + (y1 - y0) * scale, cx0:cx0 + (x1 - x0) * scale, :]
    return out


def run_onnx_dir(session, scale, in_dir, out_dir, tile, pad):
    """ONNX per-image super-res over a staged directory, mirroring
    run_binary's directory contract so the pass-chain + finalize logic stays
    backend-agnostic (same input/output filenames)."""
    import numpy as np
    inp_name = session.get_inputs()[0].name
    out_name = session.get_outputs()[0].name
    n = 0
    for p in sorted(in_dir.glob("*.png")):
        with Image.open(p) as im:
            arr = np.asarray(im.convert("RGB"), dtype=np.float32) / 255.0
        up = _onnx_infer_tiled(session, inp_name, out_name, arr, scale, tile, pad)
        pix = np.clip(up * 255.0 + 0.5, 0, 255).astype("uint8")
        Image.fromarray(pix, "RGB").save(out_dir / p.name)
        n += 1
    return n


# ---- The output check (TODO 219 / 241: a player's set with black and junk-striped images) ----
# On some graphics cards the upscaler gives broken images: all black, black with a row of coloured junk at the top
# (a GTX 1660 SUPER, nearly every image of its run; likely the 16-series' half-precision maths in
# realesrgan-ncnn-vulkan, which has no switch to turn that off and no CPU mode), or red and blue exchanged.
# A black image even passes the renderer's own check for a dark texture (Hot Shot's dark-red thighs: off by about
# 19, the renderer allows 20), so his red parts showed navy. Every image is judged against its source, box-averaged
# to the source's size, over the texels the game draws (alpha >= 16):
#  - red and blue exchanged when that copy is clearly closer (exchanged back; once most clear cases on this card were,
#    the unclear ones too);
#  - broken when, against a source with something in it, it is nearly black (under 20% of its brightness) or nearly
#    flat (under 10% of its variation): what a broken upscale gives, never a good one.
# The card is tried first (probe_card): a few bright, varied textures upscaled and judged before the run. When they
# come out broken, the safer ways are tried on them, in order: a small tile (32), each other graphics card the
# upscaler sees (a processor's built-in one), the lighter network with a small tile. The first that gives good
# images makes the whole run. When none does, nothing is made on this card (exit 3, the status says so) and the
# game's own textures are drawn; every later run (F1 > Check HD textures, an update) tries the card again, so a
# new graphics driver can fix it. A batch that still comes out mostly broken tries the card again the same way.
# A single broken image is made again: a small tile, then the lighter network; failing both, it gets no HD image.
COLOUR_BAD = 20.0
SWAP_CLEAR = 8.0
COLOUR_STATS = {"swapped": 0, "ok": 0, "broken": 0, "good": 0}
CARD_BROKEN_EXIT = 3
PROBE_N = 4


def _src_stats(path):
    """(brightness, variation) of a source over the texels the game draws."""
    with Image.open(path) as im:
        s = im.convert("RGBA")
    mask = s.getchannel("A").point(lambda v: 255 if v >= 16 else 0)
    if ImageStat.Stat(mask).mean[0] < 1.0:
        mask = None
    st = ImageStat.Stat(s.convert("RGB"), mask)
    return 0.299 * st.mean[0] + 0.587 * st.mean[1] + 0.114 * st.mean[2], sum(st.stddev[:3]) / 3.0


def pick_probe_jobs(jobs, n=PROBE_N):
    """A few small textures with plenty in them (bright and varied: what a broken upscale cannot pass), of the
    network most of the run uses."""
    cands = [j for j in jobs if j.backend == "ncnn" and j.passes >= 1 and j.alpha_mode in ("rgb", "direct")
             and j.srcs[0].w * j.srcs[0].h >= 32 * 32]
    if not cands:
        return []
    model = Counter(j.model for j in cands).most_common(1)[0][0]
    picked = []
    for job in sorted((j for j in cands if j.model == model), key=lambda j: (j.srcs[0].w * j.srcs[0].h, j.sha1)):
        try:
            luma, std = _src_stats(job.srcs[0].path)
        except Exception:  # noqa: BLE001
            continue
        if luma >= 40.0 and std >= 12.0:
            picked.append(job)
            if len(picked) >= n:
                break
    return picked


def parse_gpus(err):
    """{index: name} of the graphics cards the upscaler lists in its log ("[0 NVIDIA GeForce ...]  queueC=...")."""
    gpus = {}
    for ln in err.splitlines():
        m = re.match(r"^\[(\d+) ([^\]]+)\]", ln)
        if m:
            gpus.setdefault(int(m.group(1)), m.group(2).strip())
    return gpus


def probe_once(exe, jobs, workdir, model, tile, gpu):
    """The probe textures upscaled once this way: (good, broken, made any output, upscaler log)."""
    shutil.rmtree(workdir, ignore_errors=True)
    in_dir, out_dir = workdir / "in", workdir / "out"
    in_dir.mkdir(parents=True)
    out_dir.mkdir()
    for j in jobs:
        stage_job(j, in_dir)
    _rc, err, _ = run_binary(exe, in_dir, out_dir, model, tile, gpu)
    good = bad = 0
    made = False
    for j in jobs:
        p = out_dir / f"{j.sha1}.png"
        if not p.is_file():
            bad += 1
            continue
        made = True
        with Image.open(p) as im:
            img = im.convert("RGBA")
        bad, good = (bad + 1, good) if judge_output(img, j.srcs[0].path)[2] else (bad, good + 1)
    shutil.rmtree(workdir, ignore_errors=True)
    return good, bad, made, err


def probe_card(exe, jobs, workdir, tile, gpu, skip=()):
    """(setup, card names, made any output): setup = None (the probe images are fine as the run is set, or there are
    none to try), a dict {model, tile, gpu, what} (a safer way that gives good images), or False (no way does)."""
    probes = pick_probe_jobs(jobs)
    if not probes:
        return None, {}, False
    model = probes[0].model
    good, bad, made, err = probe_once(exe, probes, workdir, model, tile, gpu)
    gpus = parse_gpus(err)
    if bad == 0 and "plain" not in skip:
        return None, gpus, made
    print(f"note: the upscaler's images on this graphics card ({gpus.get(gpu or 0, 'unknown')}) come out broken "
          f"({bad} of {len(probes)} tried): trying safer ways", flush=True)
    ways = [{"model": model, "tile": 32, "gpu": gpu, "what": "tile 32"}]
    for g, name in sorted(gpus.items()):
        if g != (gpu or 0):
            ways.append({"model": model, "tile": 0, "gpu": g, "what": f"graphics card {g} ({name})"})
    if model != "realesr-animevideov3":
        ways.append({"model": "realesr-animevideov3", "tile": 32, "gpu": gpu,
                     "what": "realesr-animevideov3, tile 32"})
    for w in ways:
        if w["what"] in skip:
            continue
        good, bad, m, _ = probe_once(exe, probes, workdir, w["model"], w["tile"], w["gpu"])
        made = made or m
        print(f"  {w['what']}: {good} good, {bad} broken", flush=True)
        if bad == 0:
            print(f"note: the rest is made with {w['what']}", flush=True)
            return w, gpus, made
    return False, gpus, made


class BadColours(Exception):
    pass


def judge_output(out_rgba, src_path):
    """(distance as it is, distance with red and blue exchanged, why it is broken or "") against the source."""
    with Image.open(src_path) as im:
        src = im.convert("RGBA")
    small = out_rgba.convert("RGBA").resize(src.size, Image.BOX)
    mask = src.getchannel("A").point(lambda v: 255 if v >= 16 else 0)
    if ImageStat.Stat(mask).mean[0] < 1.0:
        mask = None
    src_rgb = src.convert("RGB")
    r, g, b_ = small.convert("RGB").split()
    as_is, swapped = Image.merge("RGB", (r, g, b_)), Image.merge("RGB", (b_, g, r))

    def dist(img):
        st = ImageStat.Stat(ImageChops.difference(img, src_rgb), mask)
        return sum(st.mean[:3]) / 3.0 if st.count and st.count[0] else 0.0

    d, d_swapped = dist(as_is), dist(swapped)
    s_stat, o_stat = ImageStat.Stat(src_rgb, mask), ImageStat.Stat(as_is, mask)
    luma = lambda m: 0.299 * m[0] + 0.587 * m[1] + 0.114 * m[2]  # noqa: E731
    s_luma, o_luma = luma(s_stat.mean), luma(o_stat.mean)
    s_std, o_std = sum(s_stat.stddev[:3]) / 3.0, sum(o_stat.stddev[:3]) / 3.0
    best = min(d, d_swapped)
    why = ""
    # (Calibrated on a whole good set, 1,911 images made on another card: none of them broken by these two. Off by
    # more than 20 alone is no sign - noisy ground and snow, redrawn art: the renderer judges those itself.)
    if s_luma >= 10.0 and o_luma < 0.2 * s_luma:
        why = f"dark ({o_luma:.0f} against the game's {s_luma:.0f})"
    elif s_std >= 10.0 and o_std < 0.1 * s_std:
        why = f"flat (variation {o_std:.1f} against {s_std:.1f})"
    else:
        # (TODO 259) Its colours shifted: a GTX 1660 SUPER's set had Antarctica's floor plates in pale green and purple
        # bands - the same picture and brightness, but red and blue against green moved. Box-averaged down, a right
        # upscale keeps the source's chroma (red - green, blue - green per texel): over a whole good set off by 5.6 on
        # average (median); more than 20 + a quarter of the source's own colourfulness only for three very colourful
        # effect textures (each off by over 20 overall too). The same rule as the renderer's (nr_hires.cpp looksLikeGame).
        chroma_off, chroma_allowed = _chroma_off(small, src, mask)
        if chroma_off > chroma_allowed:
            why = f"colours shifted (red and blue against green off by {chroma_off:.1f}, at most {chroma_allowed:.1f})"
    return d, d_swapped, why


def _chroma_off(small, src, mask):
    """(how far the image's chroma - red and blue against green - is from the source's per texel on average, the most
    allowed: 20 + a quarter of the source's own) over the texels the game draws."""
    a = small.convert("RGB").getdata()
    b = src.convert("RGB").getdata()
    m = mask.getdata() if mask is not None else None
    off = own = 0.0
    n = 0
    for i, (lc, gc) in enumerate(zip(a, b)):
        if m is not None and m[i] == 0:
            continue
        gr, gb = gc[0] - gc[1], gc[2] - gc[1]
        off += abs((lc[0] - lc[1]) - gr) + abs((lc[2] - lc[1]) - gb)
        own += abs(gr) + abs(gb)
        n += 1
    if n < 16:
        return 0.0, 1e9
    return off / n, 20.0 + 0.25 * own / n


def check_colours(out, src_path):
    """The image as the game's (red and blue exchanged back when they came out exchanged). Returns (image, note);
    raises BadColours when it is broken."""
    d, d_swapped, why = judge_output(out, src_path)
    if why:
        COLOUR_STATS["broken"] += 1
        raise BadColours(f"broken: {why}")
    COLOUR_STATS["good"] += 1
    clear_swap = d >= SWAP_CLEAR and d_swapped < 0.6 * d
    clear_ok = d_swapped >= SWAP_CLEAR and d < 0.6 * d_swapped
    if clear_ok:
        COLOUR_STATS["ok"] += 1
    if clear_swap:
        COLOUR_STATS["swapped"] += 1
    card_swaps = COLOUR_STATS["swapped"] >= 3 and COLOUR_STATS["swapped"] > 4 * COLOUR_STATS["ok"]
    note = ""
    if clear_swap or (card_swaps and not clear_ok and d_swapped <= d + 1.0):
        r, g, b_, a = out.split()
        out = Image.merge("RGBA", (b_, g, r, a))
        note = f"red and blue exchanged back ({d:.1f} -> {d_swapped:.1f})"
    return out, note


def finalize(job, last_dir, out_root, settings, fallback=""):
    """Recombine channels, restore exact target size + alpha semantics,
    write to every duplicate relpath atomically. Returns manifest record."""
    if job.alpha_mode == "pil":
        with Image.open(job.srcs[0].path) as im:
            resample = (Image.NEAREST if job.model == "pil-nearest"
                        else Image.LANCZOS)
            out = im.convert("RGBA").resize((job.out_w, job.out_h), resample)
    else:
        if job.alpha_mode == "split":
            with Image.open(last_dir / f"{job.sha1}.rgb.png") as rgb, \
                 Image.open(last_dir / f"{job.sha1}.a.png") as araw:
                out = rgb.convert("RGBA")
                out.putalpha(araw.convert("L"))
        else:
            with Image.open(last_dir / f"{job.sha1}.png") as im:
                out = im.convert("RGBA")
        if out.size != (job.out_w, job.out_h):
            out = out.resize((job.out_w, job.out_h), Image.LANCZOS)
    if job.alpha == "binary":
        # Model output softens cutout edges; re-threshold restores a hard
        # mask with the higher-res silhouette.
        out.putalpha(out.getchannel("A").point(lambda v: 255 if v >= 128 else 0))
    got = alpha_class(out)
    if got != job.alpha and job.alpha == "soft" and job.alpha_mode == "direct":
        # Some GPUs' upscaler hands soft alpha back flattened to 0/255 (an NVIDIA install,
        # 2026-10-30: 28 of 223 soft textures; never on the AMD card the set was made on). The
        # upscaled colours are kept and the source's own alpha, resized on the CPU, put back:
        # the same on every GPU, and textures that come out soft are untouched.
        with Image.open(job.srcs[0].path) as src:
            out.putalpha(src.convert("RGBA").getchannel("A").resize(out.size, Image.LANCZOS))
        got = alpha_class(out)
    if got != job.alpha:
        raise RuntimeError(f"alpha class changed {job.alpha} -> {got}")
    colour_note = ""
    if job.alpha_mode != "pil":
        out, colour_note = check_colours(out, job.srcs[0].path)
        if colour_note:
            print(f"  {job.srcs[0].relpath}: {colour_note}", flush=True)

    buf = io.BytesIO()
    out.save(buf, "PNG")
    data = buf.getvalue()
    for rp in job.relpaths:
        dest = out_root / rp
        dest.parent.mkdir(parents=True, exist_ok=True)
        tmp = dest.with_name(dest.name + ".tmp")
        tmp.write_bytes(data)
        for attempt in range(20):  # (the game reading the old file a moment: Windows refuses the swap meanwhile)
            try:
                os.replace(tmp, dest)
                break
            except PermissionError:
                if attempt == 19:
                    raise
                time.sleep(0.1)
    return {
        "sha1": job.sha1, "relpaths": job.relpaths, "tier": job.tier,
        "model": job.model, "backend": job.backend, "passes": job.passes,
        "alpha": job.alpha, "alpha_mode": job.alpha_mode,
        "src": [job.srcs[0].w, job.srcs[0].h], "out": [job.out_w, job.out_h],
        "out_sha1": hashlib.sha1(data).hexdigest(), "settings": settings,
        "ts": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        **({"colour": colour_note} if colour_note else {}),
        **({"fallback": fallback} if fallback else {}),
    }


def no_image_record(job, settings, why):
    """The record of a texture the upscaler could not make on this card: no HD image (the game's own is drawn); not
    tried again by later runs with the same settings (job_done)."""
    return {
        "sha1": job.sha1, "relpaths": job.relpaths, "tier": job.tier,
        "model": job.model, "backend": job.backend, "passes": job.passes,
        "alpha": job.alpha, "alpha_mode": job.alpha_mode,
        "src": [job.srcs[0].w, job.srcs[0].h], "out": [job.out_w, job.out_h],
        "settings": settings, "ts": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "fallback": "none", "why": why,
    }


def _judge_file(args):
    """(rel, output, source) -> (rel, d, d_swapped, why) or (rel, None, None, error) - for check_set's workers."""
    rel, judged, src = args
    try:
        with Image.open(judged) as im:
            img = im.convert("RGBA")
        d, d_swapped, why = judge_output(img, src)
        return rel, d, d_swapped, why
    except Exception as e:  # noqa: BLE001
        return rel, None, None, str(e)


def check_set(src_root, out_root, backup_root, status_path=None, lock=None):
    """(TODO 241) An existing set checked as the maker now checks each image: red and blue exchanged back in place
    (its colour-match original too, its ledger entry dropped so match_colour matches it again); a broken image (black,
    flat, junk) removed with its manifest record, so the next run makes it again. Returns (fixed, removed)."""
    errors = []
    srcs = {s.relpath: s for s in scan_sources(src_root, errors)}
    recs = load_manifest(out_root)
    ledger_path = backup_root / "colour_gains.json"
    ledger = {}
    if ledger_path.exists():
        try:
            ledger = json.loads(ledger_path.read_text(encoding="utf-8"))
        except ValueError:
            ledger = {}
    work = []
    by_rel = {}
    for sha1, r in sorted(recs.items()):
        rels = r.get("relpaths") or []
        if not rels or r.get("alpha_mode") == "pil" or r.get("fallback") == "none":
            continue
        src = srcs.get(rels[0])
        out_path = out_root / rels[0]
        if not src or not out_path.is_file():
            continue
        # (a level image's colour-match original is what the upscaler made: judged on that one)
        judged = backup_root / rels[0]
        judged = judged if judged.is_file() else out_path
        work.append((rels[0], str(judged), str(src.path)))
        by_rel[rels[0]] = (sha1, rels)
    from concurrent.futures import ProcessPoolExecutor
    # (two workers while the game runs: the check goes on beside it without taking its processor)
    workers = 2 if lock is not None and game_running(lock) else max(1, min(8, (os.cpu_count() or 2) - 2))
    results = []
    print(f"check: {len(work)} images to check", flush=True)
    with ProcessPoolExecutor(max_workers=workers) as pool:
        for r in pool.map(_judge_file, work, chunksize=8):  # (in order, as they come)
            results.append(r)
            if len(results) % 100 == 0 or len(results) == len(work):
                write_status(status_path, state="checking", done=len(results), total=len(work))
                print(f"check: {len(results)} of {len(work)} checked", flush=True)
    fixed = removed = 0
    drop = set()
    for rel, d, d_swapped, why in results:
        sha1, rels = by_rel[rel]
        if d is None:
            print(f"check: {rel}: {why}", file=sys.stderr)
            continue
        if why:
            for rp in rels:
                for path in (out_root / rp, backup_root / rp):
                    if path.is_file():
                        path.unlink()
                ledger.pop(rp, None)
            drop.add(sha1)
            removed += 1
            print(f"check: {rel}: broken ({why}): made again", flush=True)
        elif d >= SWAP_CLEAR and d_swapped < 0.6 * d:
            for rp in rels:
                for path in (out_root / rp, backup_root / rp):
                    if path.is_file():
                        with Image.open(path) as im:
                            x = im.convert("RGBA")
                        rr, gg, bb, aa = x.split()
                        tmp = path.with_name(path.name + ".tmp.png")
                        Image.merge("RGBA", (bb, gg, rr, aa)).save(tmp)
                        os.replace(tmp, path)
                ledger.pop(rp, None)
            fixed += 1
            print(f"check: {rel}: red and blue exchanged back ({d:.1f} -> {d_swapped:.1f})", flush=True)
    if drop:
        mf = out_root / MANIFEST_NAME
        lines = [ln for ln in mf.read_text(encoding="utf-8").splitlines()
                 if not any(f'"sha1": "{h}"' in ln for h in drop)]
        mf.write_text("\n".join(lines) + ("\n" if lines else ""), encoding="utf-8")
    if (fixed or removed) and ledger_path.exists():
        ledger_path.write_text(json.dumps(ledger, indent=0, sort_keys=True), encoding="utf-8")
    print(f"check: {len(work)} images checked, {fixed} with red and blue exchanged back, {removed} broken (made again)")
    return fixed, removed


def print_plan(srcs, jobs):
    tiers = Counter(s.tier for s in srcs)
    alphas = Counter(s.alpha for s in srcs)
    print(f"files: {len(srcs)}  uniques: {len(jobs)}  "
          f"duplicates: {len(srcs) - len(jobs)}")
    print("tiers:", dict(sorted(tiers.items())))
    print("alpha:", dict(sorted(alphas.items())))
    for k, ref in REF_ALPHA.items():
        if alphas.get(k, 0) != ref and len(srcs) > 1700:
            print(f"WARNING: alpha class '{k}' = {alphas.get(k, 0)}, "
                  f"reference scan said {ref}")
    groups = defaultdict(list)
    for j in jobs:
        groups[(j.backend, j.model, j.passes)].append(j)
    print("groups (backend:model, passes -> uniques, out-long-edge histogram):")
    for (backend, model, passes), gj in sorted(groups.items()):
        hist = Counter(max(j.out_w, j.out_h) for j in gj)
        print(f"  {backend}:{model} p{passes}: {len(gj)}  "
              f"{dict(sorted(hist.items()))}")


def audit(src_root, out_root):
    errors = []
    srcs = scan_sources(src_root, errors)
    recs = load_manifest(out_root)
    out_files = {p.relative_to(out_root).as_posix()
                 for p in out_root.rglob("*.png")}
    src_rel = {s.relpath for s in srcs}
    for rp in sorted(src_rel - out_files):
        errors.append((rp, "missing output"))
    for rp in sorted(out_files - src_rel):
        errors.append((rp, "extra output with no source"))
    per_tier = Counter()
    for s in srcs:
        if s.relpath not in out_files:
            continue
        r = recs.get(s.sha1)
        if not r:
            errors.append((s.relpath, "no manifest record for source sha1"))
            continue
        try:
            with Image.open(out_root / s.relpath) as im:
                if list(im.size) != r["out"]:
                    errors.append((s.relpath,
                                   f"dims {list(im.size)} != manifest {r['out']}"))
                elif alpha_class(im.convert("RGBA")) != s.alpha:
                    errors.append((s.relpath, "alpha class mismatch vs source"))
                else:
                    per_tier[s.tier] += 1
        except Exception as e:  # noqa: BLE001
            errors.append((s.relpath, f"audit read: {e}"))
    print("audit OK per tier:", dict(sorted(per_tier.items())))
    for rp, msg in errors[:50]:
        print(f"AUDIT FAIL {rp}: {msg}", file=sys.stderr)
    if len(errors) > 50:
        print(f"... and {len(errors) - 50} more", file=sys.stderr)
    print(f"audit: {sum(per_tier.values())} verified, {len(errors)} problems")
    return 1 if errors else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("src", nargs="?", default=DEFAULT_SRC, type=Path,
                    help="source PNG tree (default Extracted/converted/textures)")
    ap.add_argument("--out", default=DEFAULT_OUT, type=Path)
    ap.add_argument("--work", default=DEFAULT_WORK, type=Path)
    ap.add_argument("--exe", default=DEFAULT_EXE, type=Path)
    ap.add_argument("--tier", help="comma list; only process these tiers")
    ap.add_argument("--backend", choices=("ncnn", "onnx-dml"), default=None,
                    help="inference backend for AI models (default: the model's "
                         "own backend, else ncnn = existing behavior)")
    ap.add_argument("--model",
                    help="model name from the registry (see --list-models); "
                         "overrides tier_rules routing + selects the backend")
    ap.add_argument("--list-models", action="store_true",
                    help="print the model registry and exit")
    ap.add_argument("--onnx-model-dir", default=ONNX_MODEL_DIR, type=Path,
                    help="dir holding user-supplied .onnx files "
                         "(default ThirdParty/onnx_models)")
    ap.add_argument("--onnx-tile", type=int, default=256,
                    help="onnx tile size in px (0 = whole image; lower on OOM)")
    ap.add_argument("--onnx-overlap", type=int, default=16,
                    help="onnx tile padding/overlap px to hide seams")
    ap.add_argument("--model-override",
                    help="force one model for all selected tiers (pilot use); "
                         "arbitrary name allowed, --model prefers the registry")
    ap.add_argument("--alpha-mode", choices=("auto", "direct", "split"),
                    default="auto", help="non-opaque handling (auto = tier_rules)")
    ap.add_argument("--sample", type=int,
                    help="process only N stratified samples per tier")
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--tile", type=int, default=0,
                    help="ncnn tile size, 0=auto (try 512/256 on Vulkan OOM)")
    ap.add_argument("--gpu", type=int, help="ncnn GPU index (default auto)")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--audit", action="store_true",
                    help="verify outputs against sources + manifest, then exit")
    ap.add_argument("--force", action="store_true",
                    help="redo jobs even if manifest says done")
    ap.add_argument("--keep-work", action="store_true")
    ap.add_argument("--quality", choices=tier_rules.QUALITIES, default="best",
                    help="best: characters 4K, two passes where they help; fast: characters 2K, one pass, "
                         "a lighter network for the levels")
    ap.add_argument("--batch-files", type=int, default=64,
                    help="images per upscaler run (the order and the pause work batch by batch)")
    ap.add_argument("--batch-mp", type=float, default=160.0,
                    help="megapixels of output per upscaler run")
    ap.add_argument("--hd-dir", type=Path, default=None,
                    help="the HD set's folder (default: --out's parent): .game_running, hd_options.json, "
                         "upscale_status.json")
    ap.add_argument("--no-wait-for-game", action="store_true",
                    help="never wait while the game runs")
    ap.add_argument("--game-pid", type=int, default=0,
                    help="(TODO 267) made only while this game process runs: gently beside it, never waiting for it; "
                         "when it ends the upscale stops (exit 4) and the next start carries on")
    ap.add_argument("--check-colours", action="store_true",
                    help="check an existing set's colours against the sources (red and blue exchanged back, far-off "
                         "images removed to be made again), then exit")
    ap.add_argument("--match-colour", action="store_true",
                    help="match each image's colour to the game's as it is made (match_colour.py)")
    args = ap.parse_args()

    if args.list_models:
        list_models()
        sys.exit(0)
    if args.model and args.model not in MODELS:
        ap.error(f"unknown --model '{args.model}'; run --list-models "
                 f"(or use --model-override for an arbitrary ncnn model name)")
    model_override = args.model or args.model_override

    if args.audit:
        sys.exit(audit(args.src, args.out))
    if args.check_colours:
        hd_dir = args.hd_dir or args.out.parent
        check_set(args.src, args.out, args.out.parent / "upscaled_precolour", hd_dir / "upscale_status.json",
                  None if args.no_wait_for_game else hd_dir / ".game_running")
        sys.exit(0)

    errors = []
    srcs = scan_sources(args.src, errors)
    if args.tier:
        wanted = set(args.tier.split(","))
        unknown = wanted - set(tier_rules.TIERS)
        if unknown:
            ap.error(f"unknown tier(s): {sorted(unknown)}")
        srcs = [s for s in srcs if s.tier in wanted]
    if args.sample:
        srcs = sample_srcs(srcs, args.sample, args.seed)

    alpha_mode = (tier_rules.ALPHA_MODE if args.alpha_mode == "auto"
                  else args.alpha_mode)
    notes = []
    jobs = plan_jobs(srcs, model_override, alpha_mode, notes, args.backend, args.quality)
    print_plan(srcs, jobs)
    for n in notes:
        print("note:", n)
    if args.dry_run:
        print("dry run - nothing executed")
        sys.exit(1 if errors else 0)

    settings = tier_rules.settings_hash(args.quality)
    also_good = {tier_rules.settings_hash("best")} if args.quality == "fast" else set()
    recs = load_manifest(args.out)
    todo = [j for j in jobs
            if args.force or not job_done(j, recs, args.out, settings, also_good)]
    print(f"to do: {len(todo)} uniques ({len(jobs) - len(todo)} already done)")

    hd_dir = args.hd_dir or args.out.parent
    lock = None if args.no_wait_for_game else hd_dir / ".game_running"
    options_path = hd_dir / "hd_options.json"
    status_path = hd_dir / "upscale_status.json"
    total = len(jobs)
    done_before = total - len(todo)

    def status(state, made):
        write_status(status_path, state=state, done=made, total=total, quality=args.quality)

    needs_exe = any(j.backend == "ncnn" for j in todo)
    if needs_exe and not args.exe.is_file():
        print(f"error: upscaler not found: {args.exe}", file=sys.stderr)
        sys.exit(1)

    # ONNX/DirectML backend: import-guard onnxruntime + the user-supplied model
    # files up front so a missing dep prints a clear message (never a traceback)
    # and never touches the working ncnn path.
    sessions = {}
    onnx_models = sorted({j.model for j in todo if j.backend == "onnx-dml"})
    if onnx_models:
        ort = import_onnxruntime()
        if ort is None:
            print(ONNX_GUIDANCE, file=sys.stderr)
            sys.exit(2)
        if "DmlExecutionProvider" not in ort.get_available_providers():
            print("warning: onnxruntime present but DmlExecutionProvider "
                  "unavailable; using CPUExecutionProvider (slow). Install "
                  "onnxruntime-directml for the AMD GPU.", file=sys.stderr)
        for model in onnx_models:
            mf = onnx_model_path(model, args.onnx_model_dir)
            if mf is None:
                print(f"error: model '{model}' has no onnx build; pick an onnx "
                      f"model (see --list-models) or use --backend ncnn",
                      file=sys.stderr)
                sys.exit(2)
            if not mf.is_file():
                print(f"error: onnx model file not found: {mf}\n"
                      f"  supply it: {MODELS.get(model, {}).get('note', '.onnx')}\n"
                      f"  (see --list-models)", file=sys.stderr)
                sys.exit(2)
            try:
                sessions[model] = make_onnx_session(ort, mf)
            except Exception as e:  # noqa: BLE001
                print(f"error: failed to load onnx model '{model}': {e}",
                      file=sys.stderr)
                sys.exit(2)

    # Each image's colour matched as it is made (match_colour.py's rules and ledger): the images the game
    # takes while the rest is still being made look as they will at the end.
    mc = ledger = None
    native_root = args.src
    backup_root = args.out.parent / "upscaled_precolour"
    if args.match_colour:
        import match_colour as mc
        ledger = mc.load_ledger(str(backup_root))

    args.out.mkdir(parents=True, exist_ok=True)
    done = 0
    status("working", done_before)
    from concurrent.futures import ThreadPoolExecutor
    finisher = ThreadPoolExecutor(max_workers=1)  # (a batch's CPU part beside the next batch's GPU part)
    pending = None
    t_start = time.time()
    paused_s = [0.0]  # (the time spent waiting for the game: not in the estimate)
    gpu_name = [""]   # (the upscaler's card, from its own log: for the notes)
    setup = [None]    # (probe_card's safer way for this card, once one was needed)
    tried = set()     # (the safer ways tried so far)
    probed = [False]
    card_broken = [False]
    batch_bad = [False]
    with open(args.out / MANIFEST_NAME, "a", encoding="utf-8") as mf:

        redo = []

        def finish(job, last_dir):
            nonlocal done
            try:
                rec = finalize(job, last_dir, args.out, settings,
                               fallback=setup[0]["what"] if setup[0] and job.backend == "ncnn" else "")
                if mc is not None:
                    for rp in job.relpaths:
                        mc.match_file(rp, str(args.out / rp), str(native_root), str(backup_root), ledger,
                                      note_checked=True)
                mf.write(json.dumps(rec) + "\n")
                mf.flush()
                done += 1
            except BadColours as e:
                print(f"  {job.srcs[0].relpath}: {e}: made again another way", flush=True)
                redo.append(job)
            except Exception as e:  # noqa: BLE001
                errors.append((job.srcs[0].relpath, f"finalize: {e}"))

        def finish_batch(staged, cur, gdir):
            n0 = len(redo)
            for job in staged:
                missing = [s for s in expected_stems(job)
                           if not (cur / f"{s}.png").is_file()]
                if missing:
                    errors.append((job.srcs[0].relpath,
                                   f"{job.backend} produced no output ({missing})"))
                else:
                    finish(job, cur)
            nb = len(redo) - n0
            if nb >= 4 and 2 * nb >= len(staged):  # (mostly broken still: the card tried again before the next one)
                batch_bad[0] = True
            if ledger is not None:
                mc.save_ledger(str(backup_root), ledger)
            if not args.keep_work and gdir is not None:
                shutil.rmtree(gdir, ignore_errors=True)
            made = done_before + done
            status("working", made)
            el = time.time() - t_start - paused_s[0]
            left = ""
            if done >= 24 and len(todo) > done:
                left = f", about {max(1, round(el / done * (len(todo) - done) / 60))} min left"
            print(f"HD textures: {made} of {total} made{left}", flush=True)

        def game_gone():
            return bool(args.game_pid) and not pid_alive(args.game_pid)

        def wait_turn():
            """While the game runs (and the options do not say to go on): wait. True: go on gently."""
            if args.game_pid:
                # (TODO 267, the user's choice: made only while the game is open - beside it, gently; the game closed,
                # this stops: what is made is kept, the next start carries on)
                if game_gone():
                    if pending:
                        pending.result()
                    status("working", done_before + done)
                    print(f"the game closed: stopped at {done_before + done} of {total}; the next start carries on", flush=True)
                    sys.exit(4)
                return True
            told = False
            t_wait = time.time()
            while True:
                if not game_running(lock):
                    paused_s[0] += time.time() - t_wait
                    if told:
                        print("the game closed: carrying on", flush=True)
                        status("working", done_before + done)
                    return False
                if read_options(options_path).get("while_playing"):
                    return True
                if not told:
                    print(f"paused while the game runs ({done_before + done} of {total} made)", flush=True)
                    status("paused", done_before + done)
                    told = True
                time.sleep(2.0)

        def should_stop():
            return game_running(lock) and not read_options(options_path).get("while_playing")

        batches = make_batches(todo, max(1, args.batch_files), max(1.0, args.batch_mp))
        bi = 0
        while bi < len(batches):
            gjobs = batches[bi]
            backend, model, passes = gjobs[0].backend, gjobs[0].model, gjobs[0].passes
            if backend == "pil":  # pil-* pseudo-models: no GPU involved
                if pending:
                    pending.result()
                    pending = None
                for job in gjobs:
                    finish(job, None)
                status("working", done_before + done)
                bi += 1
                continue
            if card_broken[0]:  # (nothing this card makes is good: left for a later run, after a driver update)
                bi += 1
                continue
            gentle = wait_turn()
            if backend == "ncnn" and (not probed[0] or batch_bad[0]):
                again = batch_bad[0]
                probed[0] = True
                batch_bad[0] = False
                left = [j for b in batches[bi:] for j in b]
                got, gpus, made_any = probe_card(args.exe, left, args.work / "probe", args.tile, args.gpu,
                                                 skip=({"plain"} | tried) if again else ())
                if gpus and not gpu_name[0]:
                    gpu_name[0] = gpus.get(args.gpu or 0, next(iter(gpus.values())))
                if got:
                    setup[0] = got
                    tried.add(got["what"])
                elif got is False and made_any:
                    card_broken[0] = True
                    print(f"note: this graphics card ({gpu_name[0] or 'unknown'}) gives broken images in every way "
                          f"the upscaler has: no HD textures are made on it (the game's own are drawn). A newer "
                          f"graphics driver may fix it: then F1 > Graphics > Check HD textures tries "
                          f"again.", flush=True)
                    write_status(status_path, state="card_broken", done=done_before + done, total=total,
                                 quality=args.quality, gpu=gpu_name[0])
                    bi += 1
                    continue
            if backend == "ncnn" and setup[0] and model != "realesr-animevideov3":
                model = setup[0]["model"]
            gdir = args.work / f"{backend}_{model}_p{passes}_b{bi}"
            if gdir.exists():
                shutil.rmtree(gdir)
            in_dir = gdir / "in"
            in_dir.mkdir(parents=True)
            staged = []
            for job in gjobs:
                try:
                    stage_job(job, in_dir)
                    staged.append(job)
                except Exception as e:  # noqa: BLE001
                    errors.append((job.srcs[0].relpath, f"stage: {e}"))
            cur = in_dir
            stopped = False
            for i in range(1, passes + 1):
                nxt = gdir / f"p{i}"
                nxt.mkdir()
                t0 = time.time()
                if backend == "ncnn":
                    tile, gpu = (setup[0]["tile"], setup[0]["gpu"]) if setup[0] else (args.tile, args.gpu)
                    rc, err, stopped = run_binary(args.exe, cur, nxt, model, tile, gpu,
                                                  stop=game_gone if args.game_pid else (None if gentle else should_stop),
                                                  gentle=gentle)
                    if not gpu_name[0]:
                        gpus = parse_gpus(err)
                        if gpus:
                            gpu_name[0] = gpus.get(args.gpu or 0, next(iter(gpus.values())))
                    if rc != 0 and not stopped:
                        tail = err.strip().splitlines()[-3:]
                        print("  stderr: " + " | ".join(tail), file=sys.stderr)
                else:  # onnx-dml
                    scale = MODELS.get(model, {}).get("scale", 4)
                    run_onnx_dir(sessions[model], scale, cur, nxt,
                                 args.onnx_tile, args.onnx_overlap)
                    rc = 0
                if stopped:
                    break
                n_out = sum(1 for _ in nxt.glob("*.png"))
                print(f"  {backend}:{model} p{passes} pass {i}: {n_out} files "
                      f"in {time.time() - t0:.0f}s (rc={rc})", flush=True)
                cur = nxt
            if stopped:  # (the game started: this batch again, once it may)
                shutil.rmtree(gdir, ignore_errors=True)
                continue
            if pending:
                pending.result()
            pending = finisher.submit(finish_batch, staged, cur, gdir)
            bi += 1
        if pending:
            pending.result()

        # The broken images: again with a small tile (another path through the upscaler on the card), then with the
        # lighter network; failing both, no HD image (the game's own texture is drawn).
        # (A card that gives nothing good: they stay to be made by a later run.)
        for job in [] if card_broken[0] else redo:
            rec = None
            gpu = setup[0]["gpu"] if setup[0] else args.gpu
            tries = [(job.model, 32, "tile 32")]
            if job.model != "realesr-animevideov3":
                tries.append(("realesr-animevideov3", 32, "realesr-animevideov3, tile 32"))
            tries = [t for t in tries if not (setup[0] and t[2] == setup[0]["what"])]
            for model, tile, what in tries if job.backend == "ncnn" else []:
                gdir = args.work / f"redo_{job.sha1}"
                shutil.rmtree(gdir, ignore_errors=True)
                in_dir = gdir / "in"
                in_dir.mkdir(parents=True)
                try:
                    stage_job(job, in_dir)
                    cur = in_dir
                    for i in range(1, job.passes + 1):
                        nxt = gdir / f"p{i}"
                        nxt.mkdir()
                        run_binary(args.exe, cur, nxt, model, tile, gpu)
                        cur = nxt
                    rec = finalize(job, cur, args.out, settings, fallback=what)
                    print(f"  {job.srcs[0].relpath}: made again ({what})", flush=True)
                except Exception as e:  # noqa: BLE001
                    print(f"  {job.srcs[0].relpath}: again ({what}): {e}", flush=True)
                shutil.rmtree(gdir, ignore_errors=True)
                if rec is not None:
                    break
            if rec is None:
                rec = no_image_record(job, settings, "broken on this graphics card")
                for rp in job.relpaths:  # (an older broken image of it goes too)
                    for path in (args.out / rp, backup_root / rp):
                        if path.is_file():
                            path.unlink()
                    if ledger is not None:
                        ledger.pop(rp, None)
                print(f"  {job.srcs[0].relpath}: no HD image (this graphics card's upscales of it are broken): "
                      f"the game's own is drawn", flush=True)
            else:
                if mc is not None:
                    for rp in job.relpaths:
                        mc.match_file(rp, str(args.out / rp), str(native_root), str(backup_root), ledger, note_checked=True)
            mf.write(json.dumps(rec) + "\n")
            mf.flush()
            done += 1
        if redo and ledger is not None:
            mc.save_ledger(str(backup_root), ledger)
    finisher.shutdown()
    if COLOUR_STATS["swapped"]:
        print(f"note: this graphics card's upscales came back with red and blue exchanged ({COLOUR_STATS['swapped']} "
              f"clear cases): exchanged back")

    if card_broken[0]:
        write_status(status_path, state="card_broken", done=done_before + done, total=total, quality=args.quality,
                     gpu=gpu_name[0])
        print(f"upscaled {done}/{len(todo)} uniques: this graphics card gives broken images "
              f"({gpu_name[0] or 'unknown'})")
        sys.exit(CARD_BROKEN_EXIT)
    status("upscaled" if not errors else "upscaled_with_errors", done_before + done)
    total_outputs = sum(len(j.relpaths) for j in todo) - len(errors)
    print(f"upscaled {done}/{len(todo)} uniques -> ~{total_outputs} files, "
          f"{len(errors)} errors")
    for rp, msg in errors[:20]:
        print(f"ERROR {rp}: {msg}", file=sys.stderr)
    if len(errors) > 20:
        print(f"... and {len(errors) - 20} more", file=sys.stderr)
    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()
