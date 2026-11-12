#!/usr/bin/env python3
"""Tier/model configuration for the Phase-2 texture upscale pipeline.

Purpose: single source of truth mapping texture relpaths (relative to
Extracted/converted/textures/) to an upscale tier, and each tier to its
target resolution + Real-ESRGAN model. Imported by upscale_textures.py
and make_contact_sheets.py; not a CLI.

Input : relpath strings (any slash style, case-insensitive)
Output: tier name via tier_for(); settings via TIERS / ALPHA_MODE
Deps  : Python 3.8+ stdlib only
"""

import hashlib

# Ordered rules — FIRST match wins. Order matters:
#   1. TFA2/FONTS must beat the generic ui rules (fonts get their own knob).
#   2. USERINTERFACE dirs appear under several parents (TFA/, TFA2/,
#      TA_XTRAS/), so ui must match before hero/env would swallow them.
# Rule kinds: "prefix" tests relpath.startswith(pat);
#             "contains" tests pat in "/" + relpath.
# Coverage regression reference (2026-07-02, 1,793 files, 0 unmatched):
#   hero 325 / env 984 / props 126 / ui+ui_font 358
RULES = [
    ("ui_font", "prefix", "TFA2/FONTS/"),
    ("ui", "contains", "/USERINTERFACE/"),
    ("ui", "prefix", "TFA2/HUD/"),
    ("hero", "prefix", "TFA/ANIMATION/"),
    ("hero", "prefix", "TFA2/ANIMATION/"),
    ("env", "prefix", "TFA/LEVELS/"),
    ("env", "prefix", "TFA2/SKY/"),
    ("env", "prefix", "TFA2/REFLECTIONS/"),
    ("env", "prefix", "TFA2/ECOTEXTURES/"),
    ("props", "prefix", "TFA/PICKUPS/"),
    ("props", "prefix", "TFA2/FXTXTR/"),
]

# Per-tier settings.
#   target_long: desired long-edge of the output (never exceeded; sources
#     too small to reach it after passes_cap chained 4x passes land short).
#   model: realesrgan-ncnn-vulkan model name, or a "pil-*" pseudo-model
#     (pil-lanczos / pil-nearest) handled entirely in Pillow — the escape
#     hatch for content AI models mangle.
#   passes_cap: max chained 4x passes (2 => 16x max — beyond that the
#     model hallucinates mush on PS2-resolution sources).
#   small_max/small_model: sources with long edge <= small_max use
#     small_model instead — pilot showed x4plus invents structure
#     (letterform-like shapes, merged UI bars) on <=32px inputs.
# Models chosen from the 2026-07-02 M2 pilot sheets
# (Extracted/review/pilot/sheets): x4plus is photographic and faithful on
# every tier at >=64px; x4plus-anime cel-shades world textures (rejected);
# animevideov3 is soft but never invents structure (used for tiny world
# textures); pil-lanczos keeps tiny geometric UI elements faithful. Font
# atlases upscaled cleanly with x4plus (no cross-cell bleed), so ui_font
# kept the AI path with a single pass.
TIERS = {
    "hero":    {"target_long": 4096, "model": "realesrgan-x4plus", "passes_cap": 2,
                "small_max": 32, "small_model": "realesr-animevideov3"},
    "env":     {"target_long": 2048, "model": "realesrgan-x4plus", "passes_cap": 2,
                "small_max": 32, "small_model": "realesr-animevideov3"},
    "props":   {"target_long": 1024, "model": "realesrgan-x4plus", "passes_cap": 2,
                "small_max": 32, "small_model": "realesr-animevideov3"},
    "ui":      {"target_long": 1024, "model": "realesrgan-x4plus", "passes_cap": 2,
                "small_max": 32, "small_model": "pil-lanczos"},
    "ui_font": {"target_long": 1024, "model": "realesrgan-x4plus", "passes_cap": 1},
}

# How non-opaque textures go through the AI model:
#   "direct" — feed RGBA straight to the binary (requires it to preserve
#     alpha; verified/decided by the M2 pilot).
#   "split"  — upscale RGB and alpha (as 3-channel gray) separately through
#     the same pass chain, recombine at finalize.
# Fully-opaque textures never depend on this: they are always stripped to
# RGB before the binary and get constant-255 alpha re-attached.
# 2026-07-02 pilot: direct confirmed — the ncnn binary preserved alpha on
# all 35 non-opaque samples (0 class assertions failed, edges clean).
ALPHA_MODE = "direct"

# hero > env > props > ui > ui_font: when byte-identical duplicates of one
# texture live in folders of different tiers, the highest tier wins.
TIER_RANK = {"hero": 0, "env": 1, "props": 2, "ui": 3, "ui_font": 4}


def tier_for(relpath):
    """Map a texture relpath to its tier. Raises ValueError on no match so
    unclassified files fail loudly instead of silently skipping."""
    r = relpath.replace("\\", "/").upper()
    probe = "/" + r
    for tier, kind, pat in RULES:
        if kind == "prefix" and r.startswith(pat):
            return tier
        if kind == "contains" and pat in probe:
            return tier
    raise ValueError(f"no tier rule matches: {relpath}")


# ---- The presets ("HD textures: Fast / Balanced / Best" in the setup, PR #20) ----
# How much each kind of texture is enlarged, from the game's own size, measured in
# docs/HD_UPSCALE_RATIOS.md (measure_upscale_ratios.py): the smallest ratio after which the next
# 2x step is invisible for 90% of a category at its closest typical view, with the second pass
# kept only where the screen has room for it. Fast is aimed at the Steam Deck and 1080p,
# Balanced at 1440p, Best at 4K.
#   2x and 4x: one 4x pass of the tier's model (2x scaled down from it).
#   8x and 16x: two chained passes (8x scaled down from 16x).
# Every result is capped at MAX_LONG on the long edge (as before), and never below the source.
# The categories are finer than the tiers: the tiers still choose the model (TIERS above).
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
# A texture no category rule names takes its tier's category.
TIER_CATEGORY = {"hero": "enemy", "env": "terrain", "props": "fx", "ui": "ui", "ui_font": "font"}
# When byte-identical copies sit in folders of different categories, the first of these wins.
CATEGORY_ORDER = ["player", "enemy", "minicon", "terrain", "foliage", "sky", "fx", "ui", "font"]
CATEGORY_RANK = {c: i for i, c in enumerate(CATEGORY_ORDER)}

MAX_LONG = 4096
PRESETS = {
    #            player enemy minicon terrain foliage sky fx  ui  font
    "fast":     {"player": 8,  "enemy": 4, "minicon": 4, "terrain": 4, "foliage": 4, "sky": 4,
                 "fx": 4, "ui": 2, "font": 2},
    # (Balanced: the characters fought and collected one pass, 4x; the levels around them 8x)
    "balanced": {"player": 8,  "enemy": 4, "minicon": 4, "terrain": 8, "foliage": 8, "sky": 8,
                 "fx": 4, "ui": 2, "font": 2},
    "best":     {"player": 16, "enemy": 8, "minicon": 8, "terrain": 8, "foliage": 8, "sky": 8,
                 "fx": 8, "ui": 4, "font": 4},
}
QUALITIES = ("fast", "balanced", "best")
DEFAULT_QUALITY = "balanced"

# What the setup shows beside each choice (docs/HD_UPSCALE_RATIOS.md, RTX 3070): the HD set in the
# renderer's form (BC7 with mipmaps), and the making time on that card (one pass over everything
# about 9 min, most of it start-up and remaking broken images; plus the second pass). An average
# PC takes about 1.5-2 times as long.
ESTIMATES = {
    "fast":     {"gb": 0.57, "minutes_rtx3070": 10},
    "balanced": {"gb": 1.44, "minutes_rtx3070": 25},
    "best":     {"gb": 2.3,  "minutes_rtx3070": 33},
}


def category_for(relpath):
    """The measured category of a texture (finer than tier_for)."""
    r = relpath.replace("\\", "/").upper()
    probe = "/" + r
    for cat, kind, pat in CATEGORY_RULES:
        if (kind == "prefix" and r.startswith(pat)) or (kind == "contains" and pat in probe):
            return cat
    return TIER_CATEGORY[tier_for(relpath)]


def preset_for(quality):
    if quality not in PRESETS:
        raise ValueError(f"unknown quality: {quality}")
    return PRESETS[quality]


def target_for(quality, category, tier, long_edge):
    """(target long edge, max chained 4x passes) for one texture under a preset."""
    ratio = preset_for(quality)[category]
    passes_cap = min(TIERS[tier]["passes_cap"], 1 if ratio <= 4 else 2)
    return min(MAX_LONG, max(long_edge, long_edge * ratio)), passes_cap


def settings_hash(quality=DEFAULT_QUALITY):
    """Short hash of the effective config; stamped into every manifest
    record so config changes invalidate previously-upscaled files."""
    blob = repr(("presets-1", RULES, sorted(TIERS.items()), CATEGORY_RULES, MAX_LONG,
                 sorted(preset_for(quality).items()), ALPHA_MODE)).encode()
    return hashlib.sha1(blob).hexdigest()[:12]


# ---- Before the presets: "Best / Fast" by tier (kept to recognise the sets they made) ----
# Fast was one 4x pass everywhere: characters up to 2K, levels up to 1K.
LEGACY_FAST_TIERS = {
    "hero":    {"target_long": 2048, "model": "realesrgan-x4plus", "passes_cap": 1,
                "small_max": 32, "small_model": "realesr-animevideov3"},
    "env":     {"target_long": 1024, "model": "realesrgan-x4plus", "passes_cap": 1,
                "small_max": 32, "small_model": "realesr-animevideov3"},
    "props":   {"target_long": 1024, "model": "realesrgan-x4plus", "passes_cap": 1,
                "small_max": 32, "small_model": "realesr-animevideov3"},
    "ui":      {"target_long": 1024, "model": "realesrgan-x4plus", "passes_cap": 1,
                "small_max": 32, "small_model": "pil-lanczos"},
    "ui_font": {"target_long": 1024, "model": "realesrgan-x4plus", "passes_cap": 1},
}


def legacy_settings_hash(quality):
    """The hash a Best / Fast set made before the presets carries (computed exactly as then)."""
    tiers = {"best": TIERS, "fast": LEGACY_FAST_TIERS}[quality]
    blob = repr((RULES, sorted(tiers.items()), ALPHA_MODE)).encode()
    return hashlib.sha1(blob).hexdigest()[:12]


def known_settings():
    """Every settings hash whose images can stand in for another preset's when they are at least as large."""
    return {settings_hash(q) for q in QUALITIES} | {legacy_settings_hash(q) for q in ("best", "fast")}


# ---- The order the textures are made in ----
# The textures a player sees first, first: so that after a few minutes the
# start of the game looks HD while the rest is still being made. Lower = sooner.
# The levels in story order (the folder names sort that way: 01_..08_, and
# 04_AIRCRAFTCARRIERDEFEATED right after 04_AIRCRAFTCARRIER).
PRIORITY_RULES = [
    (0, "prefix", "TFA2/FONTS/"),
    (1, "contains", "/USERINTERFACE/TEXTURES/BACKGROUND/"),
    (9, "contains", "/USERINTERFACE/TEXTURES/EXTRAS/"),
    (9, "prefix", "TA_XTRAS/"),
    (2, "contains", "/USERINTERFACE/TEXTURES/INGAME/"),
    (2, "prefix", "TFA2/HUD/"),
    (1, "contains", "/USERINTERFACE/"),
    (1, "prefix", "TFA/ANIMATION/TRANSFORMERS/UI/"),
    (3, "prefix", "TFA/ANIMATION/TRANSFORMERS/GAME/"),
    (3, "prefix", "TFA2/ANIMATION/MISCELLANEOUS/MINICONS/"),
    (4, "prefix", "TFA/LEVELS/EPISODES/01_"),
    (5, "prefix", "TFA/ANIMATION/"),
    (5, "prefix", "TFA2/"),
    (5, "prefix", "TFA/PICKUPS/"),
    (6, "prefix", "TFA/LEVELS/EPISODES/"),
]


def priority_for(relpath):
    """(rank, sub-key) for a texture: the order make_hd_textures works in."""
    r = relpath.replace("\\", "/").upper()
    probe = "/" + r
    for rank, kind, pat in PRIORITY_RULES:
        if (kind == "prefix" and r.startswith(pat)) or (kind == "contains" and pat in probe):
            if rank == 6:  # the levels after the first: in story order
                return (6, r.split("/")[3] if r.count("/") > 3 else r)
            return (rank, "")
    return (8, "")
