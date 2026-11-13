"""Crisp text and PS5 button symbols: sharp replacements of the game's font atlases (TODO 119).

The game draws its text as quads cut out of small font atlases (TFA2/FONTS/*.ITF, 256 x 256 mostly),
each glyph's rectangle given by its font's glyph table (TFA2/FONTS/*.VBN). Enlarged to a modern
screen they are soft (the upscaled set's are softer still: an AI upscale of 15-pixel letters). This
tool makes new atlases at 8x their size with every glyph in the SAME rectangle at the SAME place and
size, so the game draws exactly the same quads - nothing moves - only sharper:

- DECK (the menus, button hints and prompts: ITC Avant Garde Gothic) - each letter drawn again from
  a free font of the same design, TeX Gyre Adventor (GUST Font License, typeface/ beside this file),
  fitted into the original's ink box (its height, width and place in the cell) and weight;
- the HUD's, the HQ's and the small fonts (display faces with no free twin: USUZUFONT, HUDFONT,
  LITTLEFONT, TARGW24) - their own letters rebuilt with sharp edges (the coverage enlarged smoothly,
  then its edge narrowed to a pixel or two at the new size); the title fonts are left to the
  upscaled set (sharp already: big letters the upscaler handles well);
- the PlayStation button symbols inside the atlases (DECK's and the HQ font's: cross, circle,
  square, triangle, L1/L2/R1/R2/L3/R3, SELECT) drawn as our own DualSense-style symbols (no Sony
  artwork), at the size and place of the PS2 ones.

The PlayStation sets, selected by the existing settings (TF_CRISP_TEXT x TF_BUTTON_SYMBOLS):
    <out>/text_ps5/<rel>.png   crisp text, PS5 symbols (the default)
    <out>/text_ps2/<rel>.png   crisp text, the PS2's symbols (as upscaled)
    <out>/ps5/<rel>.png        the text as it was (the upscaled atlas), PS5 symbols
(crisp text off with the PS2 symbols = the game's usual images: nothing made). <rel> is the atlas's
path in the converted tree (TFA2/FONTS/DECK) - the native renderer finds the image by the same name
and content checks as the upscaled set (transformers/native_render/nr_hires.cpp).

Xbox sets: text_xbox / xbox (the Xbox One and Series pads) and text_xbox360 / xbox360 (the Xbox 360 pad);
keyboard sets: text_keys / keys. Xbox labels use original geometric line work, without platform artwork or
font files.

Everything it writes is made from YOUR disc (the atlases and glyph tables, read from the extracted
archives and converted textures of tools/make_hd_textures.ps1): game data, never commit or share it.

Usage:
    python make_crisp_fonts.py --archives <hd_textures/archives> --converted <hd_textures/converted/textures>
        [--upscaled <hd_textures/upscaled>] --out <hd_textures/crisp_fonts> [--preview <dir>]

Needs Pillow and numpy.
"""

import argparse
import hashlib
import json
import math
import os
import shutil
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trace_glyphs  # noqa: E402

VERSION = 3
SCALE = 8          # the new atlases' size over the game's
SS = 4             # supersampling of the drawn glyphs and symbols (drawn at SCALE*SS, box-filtered down)
HERE = os.path.dirname(os.path.abspath(__file__))
FONT_DIR = os.path.join(HERE, "typeface")
FONT_REGULAR = os.path.join(FONT_DIR, "texgyreadventor-regular.otf")
FONT_BOLD = os.path.join(FONT_DIR, "texgyreadventor-bold.otf")

# The fonts whose letters are drawn again from the vector font (atlas stem -> glyph tables).
VECTOR_FONTS = {"DECKFONT"}
# The fonts with PlayStation button symbols among their glyphs: code -> symbol.
BUTTONS = {
    "DECKFONT": {123: "square", 124: "triangle", 125: "cross", 126: "circle", 127: "select",
                 128: "l1", 129: "l2", 130: "r1", 131: "r2", 132: "l3", 133: "r3"},
    "UNIV20": {123: "square", 124: "triangle", 125: "cross", 126: "circle"},
    "UNIV14": {123: "square", 124: "triangle", 125: "cross", 126: "circle", 127: "select"},
}
# The D-pad's arrows among the glyphs (code -> the arrows in the cell, left to right): drawn sharp with
# the crisp text, the same white arrows in either symbol set (a D-pad has no other look on the PS5).
ARROWS = {
    "DECKFONT": {60: "left", 62: "right", 94: "up", 95: "down", 134: "left", 135: "right", 136: "up", 137: "down"},
    "UNIV14": {62: "left right", 94: "up down"},
}
# The atlases made again (by their image's name): every font of the game's 2D text but the debug fonts.
ATLASES = {"DECK", "USUZUFONT", "HUDFONT", "LITTLEFONT", "TARGW24", "6MILL", "6MILL_COL", "BANKGOTHIC", "UIFONT", "WHITE18"}


# --------------------------------------------------------------------------------------------
# The glyph tables (FISH ~FontSet .VBN): ~Textures names the atlas, ~GlyphSet holds a u32 length
# then 28-byte records: u32 (low 16 bits the character code), then six 32-bit floats stored with
# their two 16-bit halves swapped - u0, v0, u1, v1 (V counted from the image's bottom), x and y
# offsets.

def _swapped_float(u):
    return float(np.frombuffer(np.uint32(((u & 0xFFFF) << 16) | (u >> 16)).tobytes(), np.float32)[0])


def read_vbn(path):
    b = open(path, "rb").read()
    t = b.find(b"~Textures")
    g = b.find(b"~GlyphSet")
    if t < 0 or g < 0:
        raise ValueError(f"{path}: not a font (no ~Textures / ~GlyphSet)")
    name = b[t + 9:g]
    k = name.lower().find(b"fonts")
    atlas = name[k:].decode("latin-1").replace("\\", "/").split("/")[-1]
    length = int.from_bytes(b[g + 11:g + 15], "little")
    start = g + 15
    glyphs = []
    for r in range((length - 4) // 28):
        q = start + 28 * r
        code = int.from_bytes(b[q:q + 4], "little") & 0xFFFF
        vals = [_swapped_float(int.from_bytes(b[q + 4 + 4 * i:q + 8 + 4 * i], "little")) for i in range(6)]
        u0, v0, u1, v1 = vals[:4]
        # (WHITE18 gives its V's top first: the rectangle either way round)
        if not (0.0 <= min(u0, u1) < max(u0, u1) <= 1.0 and 0.0 <= min(v0, v1) < max(v0, v1) <= 1.0):
            continue  # (the table's last record is not a glyph)
        glyphs.append((code, vals))
    return atlas, glyphs


def cell_of(vals, w, h):
    """A glyph's rectangle in the atlas's pixels (x0, y0, x1, y1; y down)."""
    u0, v0, u1, v1 = vals[:4]
    u0, u1 = min(u0, u1), max(u0, u1)
    v0, v1 = min(v0, v1), max(v0, v1)
    x0, x1 = round(u0 * w), round(u1 * w)
    y0, y1 = round((1.0 - v1) * h), round((1.0 - v0) * h)
    return (max(0, x0), max(0, y0), min(w, x1), min(h, y1))


# --------------------------------------------------------------------------------------------
# Small image helpers (float arrays, alpha in 0..1).

def to_float(img):
    a = np.asarray(img.convert("RGBA"), dtype=np.float32) / 255.0
    return a


def to_image(a):
    return Image.fromarray(np.clip(np.round(a * 255.0), 0, 255).astype(np.uint8), "RGBA")


def resize_plane(plane, size, method=Image.BICUBIC):
    out = Image.fromarray(np.ascontiguousarray(plane, dtype=np.float32), "F").resize(size, method)
    return np.asarray(out, dtype=np.float32)


def fill_rgb(rgba, passes=24):
    """The colour of transparent texels made that of their nearest opaque ones (no grey fringe when
    the enlarged atlas is filtered: the game's transparent texels are a pinkish grey)."""
    rgb = rgba[..., :3].copy()
    known = rgba[..., 3] > (2.0 / 255.0)
    if not known.any():
        return rgba
    for _ in range(passes):
        if known.all():
            break
        acc = np.zeros_like(rgb)
        cnt = np.zeros(known.shape, np.float32)
        for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1), (-1, -1), (-1, 1), (1, -1), (1, 1)):
            k = np.roll(np.roll(known, dy, 0), dx, 1)
            c = np.roll(np.roll(rgb, dy, 0), dx, 1)
            acc += c * k[..., None]
            cnt += k
        new = (~known) & (cnt > 0)
        rgb[new] = acc[new] / cnt[new][:, None]
        known = known | new
    out = rgba.copy()
    out[..., :3] = rgb
    return out


def blur(plane, sigma):
    """A separable Gaussian blur (sigma in pixels)."""
    r = max(1, int(math.ceil(sigma * 3.0)))
    x = np.arange(-r, r + 1, dtype=np.float32)
    k = np.exp(-0.5 * (x / sigma) ** 2)
    k /= k.sum()
    p = np.pad(plane, r, mode="edge")
    h, w = plane.shape
    p = sum(k[i] * p[:, i:i + w] for i in range(2 * r + 1))
    return sum(k[i] * p[i:i + h, :] for i in range(2 * r + 1))


def sharpen_alpha(a, ramp_px, scale):
    """Coverage enlarged smoothly -> a sharp edge: the smooth coverage climbs from 0 to 1 over about one
    original texel (`scale` new pixels); the edge kept is a ramp `ramp_px` new pixels wide around its 0.5."""
    half = 0.5 * ramp_px / scale
    lo, hi = 0.5 - half, 0.5 + half
    t = np.clip((a - lo) / (hi - lo), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def rebuild_sharp(native, scale):
    """The whole atlas at `scale`: RGB enlarged smoothly (from colour-filled texels), alpha rebuilt
    with a sharp edge, relative to each region's own top opacity (the game's opaque = 252 or 255)."""
    h, w = native.shape[:2]
    big = (w * scale, h * scale)
    a = native[..., 3]
    filled = fill_rgb(native)
    rgb = np.stack([resize_plane(filled[..., c], big) for c in range(3)], -1)
    top = float(a.max()) if a.max() > 0 else 1.0
    au = resize_plane(a / top, big)
    # (the texel staircase of a diagonal edge rounded off first: the coverage smoothed over about half a texel)
    au = blur(au, 0.45 * scale)
    sharp = sharpen_alpha(np.clip(au, 0.0, 1.0), 1.5, scale) * top
    # Letters outlined in their colour (USUZUFONT: white inside a dark blue edge): the edge between the two
    # colours rebuilt sharp the same way - the brightness between the outline's and the inside's taken as
    # the inside's coverage.
    opaque = a >= 0.9 * top
    lum = filled[..., :3].mean(-1)
    if opaque.any():
        lo, hi = float(np.percentile(lum[opaque], 5)), float(np.percentile(lum[opaque], 95))
        if hi - lo > 0.4:
            dark = opaque & (lum <= lo + 0.15 * (hi - lo))
            light = opaque & (lum >= hi - 0.15 * (hi - lo))
            c_dark = filled[..., :3][dark].mean(axis=0)
            c_light = filled[..., :3][light].mean(axis=0)
            lu = blur(resize_plane((lum - lo) / (hi - lo), big), 0.45 * scale)
            t = sharpen_alpha(np.clip(lu, 0.0, 1.0), 1.5, scale)[..., None]
            rgb = c_dark + (c_light - c_dark) * t
    out = np.concatenate([np.clip(rgb, 0, 1), sharp[..., None]], -1)
    return out


def trace_cells(native, crisp, cells, scale, check):
    """The glyphs in `cells` (x0, y0, x1, y1) traced from the original into vector outlines and drawn at
    `scale` into `crisp` (trace_glyphs.py): the very shapes of the game's own letters, sharp at any size.
    What a glyph's shape is: its opacity; on an opaque atlas (drawn added onto the picture) its brightness.
    Letters in two colours (a white inside a dark edge: USUZUFONT, WHITE18) get the edge between the two
    traced as well. Each glyph is checked against the original: drawn back at the game's size, the mean
    difference of the coverage (`check` collects (difference, overlap, cell))."""
    h, w = native.shape[:2]
    a = native[..., 3]
    top = float(a.max()) if a.max() > 0 else 1.0
    opaque_atlas = a.min() == a.max()
    filled = fill_rgb(native)
    two_lum = None
    if opaque_atlas:
        lum = native[..., :3].max(-1)
        peak = float(lum.max()) if lum.max() > 0 else 1.0
        field = lum / peak
        inside = native.copy()
        inside[..., 3] = (lum >= 0.6 * peak).astype(np.float32)
        colour = fill_rgb(inside, passes=6)
        # (the faint bevel lines of these letters' insides - a texel band a few levels brighter along the
        # edges - smoothed away; their colour gradient, top to bottom, kept)
        for c in range(3):
            colour[..., c] = blur(colour[..., c], 1.0)
        # Two-tone letters (UIFONT, 6MILL_COL: a light inside within a dark blue edge, which is a colour, not
        # the anti-aliasing - 6MILL's and BANKGOTHIC's mid-bright texels are their own grey): the outline
        # traced through the edge's middle, the edge-to-inside boundary traced too, each part its colour.
        band = (field >= 0.3) & (field < 0.75)
        core = field >= 0.9
        if band.sum() > 50 and core.any():
            cb, cc = native[..., :3][band].mean(axis=0), native[..., :3][core].mean(axis=0)
            if np.abs(cb / max(cb.max(), 1e-6) - cc / max(cc.max(), 1e-6)).max() > 0.15:
                ring_level = float(np.median(field[band]))
                two_lum = (cb, ring_level / 2.0, (ring_level + 1.0) / 2.0)
    else:
        field = a / top
        colour = filled
    # (two colours: as rebuild_sharp finds them)
    two = None
    if not opaque_atlas:
        op = a >= 0.9 * top
        lum2 = filled[..., :3].mean(-1)
        if op.any():
            lo, hi = float(np.percentile(lum2[op], 5)), float(np.percentile(lum2[op], 95))
            if hi - lo > 0.4:
                c_dark = filled[..., :3][op & (lum2 <= lo + 0.15 * (hi - lo))].mean(axis=0)
                c_light = filled[..., :3][op & (lum2 >= hi - 0.15 * (hi - lo))].mean(axis=0)
                two = (np.clip((lum2 - lo) / (hi - lo), 0.0, 1.0), c_dark, c_light)
    P = 2
    for (x0, y0, x1, y1) in cells:
        if x1 <= x0 or y1 <= y0 or field[y0:y1, x0:x1].max() < (two_lum[1] if two_lum else 0.5):
            continue
        X0, Y0, X1, Y1 = max(0, x0 - P), max(0, y0 - P), min(w, x1 + P), min(h, y1 + P)
        org = (x0 - X0, y0 - Y0)
        cw, ch = x1 - x0, y1 - y0
        level = two_lum[1] if two_lum else 0.5
        cov = trace_glyphs.render(trace_glyphs.outline(field[Y0:Y1, X0:X1], level), cw, ch, scale, SS, origin=org)
        # the colour: smooth (the game's own, which these letters have flat or as a gentle gradient)
        col = np.stack([resize_plane(colour[Y0:Y1, X0:X1, c], ((X1 - X0) * scale, (Y1 - Y0) * scale))
                        [org[1] * scale:(org[1] + ch) * scale, org[0] * scale:(org[0] + cw) * scale] for c in range(3)], -1)
        if two is not None:
            t = trace_glyphs.render(trace_glyphs.outline(two[0][Y0:Y1, X0:X1]), cw, ch, scale, SS, origin=org)[..., None]
            col = two[1] + (two[2] - two[1]) * t
        if two_lum is not None:
            t = trace_glyphs.render(trace_glyphs.outline(field[Y0:Y1, X0:X1], two_lum[2]), cw, ch, scale, SS, origin=org)[..., None]
            col = two_lum[0] + (col - two_lum[0]) * t
        sl = crisp[y0 * scale:y1 * scale, x0 * scale:x1 * scale]
        if opaque_atlas:
            sl[..., :3] = np.clip(col, 0, 1) * cov[..., None]
            sl[..., 3] = top
        else:
            sl[..., :3] = np.clip(col, 0, 1)
            sl[..., 3] = cov * top
        down = cov.reshape(ch, scale, cw, scale).mean(axis=(1, 3))
        orig = np.clip(field[y0:y1, x0:x1], 0, 1)
        if two_lum is not None:
            # (the outline is the edge's middle: the original's coverage there, for the check)
            orig = np.clip(orig / (2.0 * level), 0, 1)
        inter = ((down >= 0.5) & (orig >= 0.5)).sum()
        uni = ((down >= 0.5) | (orig >= 0.5)).sum()
        check.append((float(np.abs(down - orig).mean()), float(inter) / max(1, uni), (x0, y0, x1, y1)))
    return crisp


# --------------------------------------------------------------------------------------------
# The vector font fitted into the original glyphs.

def ink_box(mask, thr=0.5):
    ys, xs = np.nonzero(mask >= thr)
    if len(xs) == 0:
        return None
    return (xs.min(), ys.min(), xs.max() + 1, ys.max() + 1)


def render_char(font, ch, stroke):
    """The character as a coverage array (float, 0..1) cropped to its ink, at the font's size."""
    # (Drawn, not getmask2: with a stroke, the mask is the outline alone; the drawing fills it too.)
    try:
        l, t, r, b = font.getbbox(ch, stroke_width=stroke)
    except Exception:
        return None
    if r <= l or b <= t:
        return None
    pad = 4 + stroke
    img = Image.new("L", (r - l + 2 * pad, b - t + 2 * pad), 0)
    ImageDraw.Draw(img).text((pad - l, pad - t), ch, font=font, fill=255, stroke_width=stroke, stroke_fill=255)
    a = np.asarray(img, dtype=np.float32) / 255.0
    bb = ink_box(a)
    if bb is None:
        return None
    return a[bb[1]:bb[3], bb[0]:bb[2]]


def place(glyph, box, size):
    """`glyph` (coverage, cropped to its ink) scaled into `box` (fractional x0, y0, x1, y1 in a canvas of
    `size`) - by box-filtering a canvas SS times larger."""
    W, H = size
    x0, y0, x1, y1 = box
    big = np.zeros((H * SS, W * SS), np.float32)
    bw, bh = max(1, int(round((x1 - x0) * SS))), max(1, int(round((y1 - y0) * SS)))
    g = np.asarray(Image.fromarray((glyph * 255).astype(np.uint8), "L").resize((bw, bh), Image.LANCZOS), np.float32) / 255.0
    ox, oy = int(round(x0 * SS)), int(round(y0 * SS))
    sx0, sy0 = max(0, -ox), max(0, -oy)
    ex, ey = min(W * SS, ox + bw), min(H * SS, oy + bh)
    if ex > ox + sx0 and ey > oy + sy0:
        big[oy + sy0:ey, ox + sx0:ex] = g[sy0:ey - oy, sx0:ex - ox]
    return big.reshape(H, SS, W, SS).mean(axis=(1, 3))


def iou(a, b):
    i = np.logical_and(a >= 0.5, b >= 0.5).sum()
    u = np.logical_or(a >= 0.5, b >= 0.5).sum()
    return i / u if u else 0.0


class VectorFitter:
    """Draws a font's letters again from the vector font: each glyph's ink box measured on the original
    (its coverage enlarged smoothly, the 0.5 contour), the vector glyph scaled into it. The letters of the
    game's font run about a tenth narrower than the vector font's, so the width comes from the original
    too; very thin glyphs (an i, an l: one or two texels wide, so their measured width is mostly
    rounding) take the font's median proportion instead, centred. The stroke weight is matched once for
    the font by the letters' mean coverage."""

    def __init__(self, font_path, em):
        self.font_path = font_path
        self.em = em
        self.fonts = {}

    def font(self, size):
        if size not in self.fonts:
            self.fonts[size] = ImageFont.truetype(self.font_path, size)
        return self.fonts[size]

    def fit(self, orig_cov, ch, stroke, ratio_median):
        """orig_cov: the original cell's coverage enlarged to SCALE (smooth). Returns (coverage, iou)."""
        h, w = orig_cov.shape
        ob = ink_box(orig_cov)
        if ob is None:
            return None, 0.0
        vec = render_char(self.font(self.em), ch, stroke)
        if vec is None:
            return None, 0.0
        tw, th = ob[2] - ob[0], ob[3] - ob[1]
        vh, vw = vec.shape
        # (the box from the original's 0.5 contour, which the vector glyph's own 0.5 contour fills)
        x0, y0, x1, y1 = float(ob[0]), float(ob[1]), float(ob[2]), float(ob[3])
        natural = vw / vh * th * ratio_median
        if tw < 3.5 * SCALE or abs(tw / max(natural, 1e-6) - 1.0) > 0.35:
            cx = (x0 + x1) / 2.0
            x0, x1 = cx - natural / 2.0, cx + natural / 2.0
        cov = place(vec, (x0, y0, x1, y1), (w, h))
        return cov, iou(cov, orig_cov)


# --------------------------------------------------------------------------------------------
# Our own DualSense-style symbols (drawn, not copied): a dark disc with a thin light rim and the
# symbol in light line work, as the PS5 shows its buttons; the shoulder buttons dark keys with
# their name; SELECT as the DualSense's Create button.

DISC = (22, 24, 30, 255)
RIM = (150, 156, 168, 255)
INK = (238, 240, 244, 255)


def _supersampled(w, h):
    img = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
    return img, ImageDraw.Draw(img), float(SS)


def _down(img, w, h):
    a = to_float(img)
    # (premultiplied while box-filtering: no dark fringe from the clear texels' colour)
    pm = a[..., :3] * a[..., 3:4]
    pm = pm.reshape(h, SS, w, SS, 3).mean(axis=(1, 3))
    al = a[..., 3].reshape(h, SS, w, SS).mean(axis=(1, 3))
    rgb = np.where(al[..., None] > 1e-4, pm / np.maximum(al[..., None], 1e-4), 0.0)
    return np.concatenate([rgb, al[..., None]], -1)


def _label(d, text, box, path, fill, height_share=0.62):
    x0, y0, x1, y1 = box
    w, h = x1 - x0, y1 - y0
    size = max(4, int(h * height_share * 1.35))
    while size > 4:
        f = ImageFont.truetype(path, size)
        l, t, r, b = d.textbbox((0, 0), text, font=f)
        if r - l <= w * 0.82 and b - t <= h * height_share:
            break
        size -= 1
    l, t, r, b = d.textbbox((0, 0), text, font=f)
    d.text((x0 + (w - (r - l)) / 2 - l, y0 + (h - (b - t)) / 2 - t), text, font=f, fill=fill)


def draw_arrows(kinds, w, h, ink_box_px, top_alpha, native_cell):
    """D-pad arrows (one, or two side by side) filling the original's ink box: white, with the thin dark
    edge of the game's own."""
    img, d, s = _supersampled(w, h)
    x0, y0, x1, y1 = [v * SS for v in ink_box_px]
    # (the edge's colour: the cell's darkest opaque texels)
    a = native_cell[..., 3]
    op = a > 0.5 * a.max() if a.max() > 0 else a > 0
    lum = native_cell[..., :3].mean(-1)
    edge = (14, 22, 48, 255)
    if op.any() and (lum[op] < 0.5).any():
        e = native_cell[..., :3][op & (lum < 0.5)].mean(axis=0)
        edge = tuple(int(round(c * 255)) for c in e) + (255,)
    n = len(kinds)
    gap = (x1 - x0) * 0.06 if n > 1 else 0.0
    bw = ((x1 - x0) - gap * (n - 1)) / n
    for i, k in enumerate(kinds):
        bx0 = x0 + i * (bw + gap)
        bx1, by0, by1 = bx0 + bw, y0, y1
        cx, cy = (bx0 + bx1) / 2.0, (by0 + by1) / 2.0
        hw, hh = (bx1 - bx0) / 2.0, (by1 - by0) / 2.0
        tri = {"left": [(bx0, cy), (bx1, by0), (bx1, by1)], "right": [(bx1, cy), (bx0, by0), (bx0, by1)],
               "up": [(cx, by0), (bx1, by1), (bx0, by1)], "down": [(cx, by1), (bx0, by0), (bx1, by0)]}[k]
        d.polygon(tri, fill=edge)
        # (the white inside, its edge about a twelfth of the arrow in)
        inset = min(hw, hh) * 0.10
        gx, gy = sum(p[0] for p in tri) / 3.0, sum(p[1] for p in tri) / 3.0
        r0 = max(math.hypot(p[0] - gx, p[1] - gy) for p in tri)
        f = max(0.0, (r0 - inset * 2.0) / r0)
        d.polygon([(gx + (p[0] - gx) * f, gy + (p[1] - gy) * f) for p in tri], fill=(250, 250, 252, 255))
    out = _down(img, w, h)
    out[..., 3] *= top_alpha
    return out


# A tiny geometric alphabet for our Xbox labels; no font or platform artwork is used.
XBOX_LABELS = dict(square="X", triangle="Y", cross="A", circle="B", select="VIEW",
                   start="MENU", l1="LB", r1="RB", l2="LT", r2="RT", l3="LS", r3="RS")
STROKES = {
    "A": [[(0,1),(.5,0),(1,1)],[(.2,.6),(.8,.6)]],
    "B": [[(0,1),(0,0),(.7,0),(1,.2),(.7,.5),(0,.5)],[(.7,.5),(1,.75),(.7,1),(0,1)]],
    "X": [[(0,0),(1,1)],[(1,0),(0,1)]], "Y": [[(0,0),(.5,.5),(1,0)],[(.5,.5),(.5,1)]],
    "L": [[(0,0),(0,1),(1,1)]], "R": [[(0,1),(0,0),(.7,0),(1,.25),(.7,.5),(0,.5)],[(.5,.5),(1,1)]],
    "T": [[(0,0),(1,0)],[(.5,0),(.5,1)]], "S": [[(1,0),(0,0),(0,.5),(1,.5),(1,1),(0,1)]],
    "V": [[(0,0),(.5,1),(1,0)]], "I": [[(0,0),(1,0)],[(.5,0),(.5,1)],[(0,1),(1,1)]],
    "E": [[(1,0),(0,0),(0,1),(1,1)],[(0,.5),(.8,.5)]],
    "W": [[(0,0),(.2,1),(.5,.5),(.8,1),(1,0)]], "M": [[(0,1),(0,0),(.5,.5),(1,0),(1,1)]],
    "N": [[(0,1),(0,0),(1,1),(1,0)]], "U": [[(0,0),(0,1),(1,1),(1,0)]]}
FACE_KINDS = ("cross", "circle", "square", "triangle")
# The two Xbox looks (the pad's model: the renderer picks the set, nr_hires.cpp). "one": the Xbox One and Series pads -
# dark buttons with coloured letters, VIEW and MENU as their small icons. "360": the Xbox 360 pad - coloured buttons
# with a pale letter of their own colour, BACK and START as the small buttons with their arrows. Both keep the grey
# rim of every other prompt; the shoulder keys (LB / RB / LT / RT / LS / RS) are the same in both.
XBOX_LETTER = {"A": (92, 200, 70), "B": (236, 62, 54), "X": (46, 130, 240), "Y": (250, 198, 28)}
XBOX_GEM = {"A": (64, 168, 52), "B": (206, 40, 36), "X": (36, 100, 214), "Y": (232, 176, 18)}


def _strokes(d, label, box, colour, weight):
    """The label in the geometric alphabet, centred in box (x0, y0, x1, y1), its strokes weight x its height."""
    x0, y0, x1, y1 = box
    height = min((y1-y0)*.5, (x1-x0)*.72/(len(label)*.72))
    cw, gap = height*.55, height*.17
    left = (x0+x1-(cw*len(label)+gap*(len(label)-1)))/2
    top = (y0+y1-height)/2
    for i, ch in enumerate(label):
        for stroke in STROKES[ch]:
            d.line([(left+i*(cw+gap)+x*cw, top+y*height) for x,y in stroke],
                   fill=colour, width=max(1,round(height*weight)), joint="curve")


def _shaded_disc(d, cx, cy, r, edge, centre, lift=0.0, steps=20):
    """A disc shaded from its edge's colour to its centre's, the centre lifted up by lift x r (a domed button)."""
    for i in range(steps):
        t = i / (steps - 1)
        rr = r * (1 - .88*t)
        c = tuple(int(round(e + (m - e)*t)) for e, m in zip(edge, centre)) + (255,)
        oy = -r * lift * t
        d.ellipse((cx-rr, cy-rr+oy, cx+rr, cy+rr+oy), fill=c)


def draw_xbox(kind, w, h, ink_box_px, top_alpha, style="one"):
    """Original geometric buttons, clipped to the same cell as the source symbol (style: "one" or "360")."""
    img, d, _ = _supersampled(w, h)
    x0, y0, x1, y1 = [v * SS for v in ink_box_px]
    # Leave space for antialiasing and keep even tiny source cells valid.
    inset = max(SS, min(x1-x0, y1-y0) * .06)
    x0, y0, x1, y1 = x0+inset, y0+inset, x1-inset, y1-inset
    if x1 <= x0 or y1 <= y0:
        return np.zeros((h, w, 4), dtype=np.float32)
    label = XBOX_LABELS[kind]
    rim = max(1, round(min(x1-x0, y1-y0)*.055))
    if kind in FACE_KINDS:
        side = min(x1-x0, y1-y0)
        cx, cy = (x0+x1)/2, (y0+y1)/2
        x0, y0, x1, y1 = cx-side/2, cy-side/2, cx+side/2, cy+side/2
        d.ellipse((x0,y0,x1,y1), fill=RIM)
        r = side/2 - rim
        if style == "360":
            gem = XBOX_GEM[label]
            _shaded_disc(d, cx, cy, r, tuple(c*.55 for c in gem), tuple(min(255, c*1.12 + 18) for c in gem), lift=.12)
            colour = tuple(int(c + (255-c)*.72) for c in gem) + (255,)
            # (a pale letter on a bright button - A and Y most - reads better with a thin edge in a deep shade of it)
            _strokes(d, label, (x0,y0,x1,y1), tuple(int(c*.38) for c in gem) + (255,), .31)
        else:
            _shaded_disc(d, cx, cy, r, (52, 53, 58), (18, 18, 22))
            colour = XBOX_LETTER[label] + (255,)
        _strokes(d, label, (x0,y0,x1,y1), colour, .19)
    elif kind in ("select", "start"):
        if style == "360":
            # BACK / START: a small dark round button with its arrow
            side = min(x1-x0, y1-y0)
            cx, cy = (x0+x1)/2, (y0+y1)/2
            d.ellipse((cx-side/2, cy-side/2, cx+side/2, cy+side/2), fill=DISC, outline=RIM, width=rim)
            a = side*.2
            pts = [(cx-a, cy), (cx+a*.8, cy-a), (cx+a*.8, cy+a)] if kind == "select" else [(cx+a, cy), (cx-a*.8, cy-a), (cx-a*.8, cy+a)]
            d.polygon(pts, fill=INK)
        else:
            d.rounded_rectangle((x0,y0,x1,y1), min(x1-x0,y1-y0)*.18, fill=DISC, outline=RIM, width=rim)
            cx, cy = (x0+x1)/2, (y0+y1)/2
            u = min(x1-x0, y1-y0)*.5
            lw = max(1, round(u*.09))
            if kind == "select":
                # VIEW: two overlapping squares, the front one filled
                b = u*.72
                d.rounded_rectangle((cx-b*.75, cy-b*.75, cx+b*.25, cy+b*.25), b*.12, outline=INK, width=lw)
                d.rounded_rectangle((cx-b*.25, cy-b*.25, cx+b*.75, cy+b*.75), b*.12, fill=DISC, outline=INK, width=lw)
            else:
                # MENU: three lines
                for k in (-1, 0, 1):
                    d.line([(cx-u*.36, cy+k*u*.26), (cx+u*.36, cy+k*u*.26)], fill=INK, width=lw)
    else:
        d.rounded_rectangle((x0,y0,x1,y1), min(x1-x0,y1-y0)*.18, fill=DISC, outline=RIM, width=rim)
        _strokes(d, label, (x0,y0,x1,y1), INK, .10)
    out = _down(img, w, h)
    out[...,3] *= top_alpha
    return out


def draw_symbol(kind, w, h, ink_box_px, top_alpha):
    """The symbol in a w x h cell (at SCALE), filling the original's ink box. RGBA float array."""
    img, d, s = _supersampled(w, h)
    x0, y0, x1, y1 = [v * SS for v in ink_box_px]
    bw, bh = x1 - x0, y1 - y0
    if kind in ("cross", "circle", "square", "triangle", "l3", "r3"):
        dia = min(bw, bh)
        cx, cy = (x0 + x1) / 2.0, (y0 + y1) / 2.0
        r = dia / 2.0
        rim = max(1.0, dia * 0.055)
        d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=RIM)
        d.ellipse((cx - r + rim, cy - r + rim, cx + r - rim, cy + r - rim), fill=DISC)
        lw = max(1.0, dia * 0.085)
        if kind == "cross":
            k = dia * 0.21
            for sx in (-1, 1):
                d.line((cx - k, cy - sx * k, cx + k, cy + sx * k), fill=INK, width=int(round(lw)))
            # (round ends)
            for px, py in ((cx - k, cy - k), (cx + k, cy + k), (cx - k, cy + k), (cx + k, cy - k)):
                d.ellipse((px - lw / 2, py - lw / 2, px + lw / 2, py + lw / 2), fill=INK)
        elif kind == "circle":
            k = dia * 0.215
            d.ellipse((cx - k - lw / 2, cy - k - lw / 2, cx + k + lw / 2, cy + k + lw / 2), fill=INK)
            d.ellipse((cx - k + lw / 2, cy - k + lw / 2, cx + k - lw / 2, cy + k - lw / 2), fill=DISC)
        elif kind == "square":
            k = dia * 0.19
            d.rounded_rectangle((cx - k - lw / 2, cy - k - lw / 2, cx + k + lw / 2, cy + k + lw / 2), lw * 0.6, fill=INK)
            d.rectangle((cx - k + lw / 2, cy - k + lw / 2, cx + k - lw / 2, cy + k - lw / 2), fill=DISC)
        elif kind == "triangle":
            k = dia * 0.32  # circumradius (a triangle looks smaller than a square of the same size)
            oy = dia * 0.045  # (its centroid a little low: optically centred)
            pts = [(cx + k * math.cos(math.radians(a)), cy + oy + k * math.sin(math.radians(a))) for a in (-90, 30, 150)]
            d.polygon(pts, fill=INK)
            inner = k - lw * 1.7  # (the inradius shrinks by 0.85 of the line width: its corners read heavier)
            pts2 = [(cx + inner * math.cos(math.radians(a)), cy + oy + inner * math.sin(math.radians(a))) for a in (-90, 30, 150)]
            d.polygon(pts2, fill=DISC)
        else:
            _label(d, kind.upper(), (cx - r * 0.75, cy - r * 0.6, cx + r * 0.75, cy + r * 0.6), FONT_BOLD, INK, 0.7)
    elif kind in ("l1", "r1", "l2", "r2"):
        rim = max(1.0, min(bw, bh) * 0.07)
        if kind in ("l1", "r1"):
            rad = bh * 0.32
            d.rounded_rectangle((x0, y0, x1, y1), rad, fill=RIM)
            d.rounded_rectangle((x0 + rim, y0 + rim, x1 - rim, y1 - rim), max(0.0, rad - rim), fill=DISC)
            _label(d, kind.upper(), (x0 + rim, y0 + rim, x1 - rim, y1 - rim), FONT_BOLD, INK, 0.62)
        else:
            # (a trigger: a key rounded more at its top)
            rad = bw * 0.30
            d.rounded_rectangle((x0, y0, x1, y1), rad, fill=RIM)
            d.rectangle((x0, y0 + bh * 0.5, x1, y1 - bh * 0.18), fill=RIM)
            d.rounded_rectangle((x0, y0 + bh * 0.5, x1, y1), bh * 0.12, fill=RIM)
            ix0, iy0, ix1, iy1 = x0 + rim, y0 + rim, x1 - rim, y1 - rim
            d.rounded_rectangle((ix0, iy0, ix1, iy1), max(0.0, rad - rim), fill=DISC)
            d.rounded_rectangle((ix0, iy0 + bh * 0.5, ix1, iy1), max(0.0, bh * 0.12 - rim), fill=DISC)
            _label(d, kind.upper(), (ix0, iy0 + bh * 0.08, ix1, iy1), FONT_BOLD, INK, 0.5)
    elif kind == "select":
        # The DualSense's Create button: a small dark key with three rays, its name under it.
        kh = bh * 0.42
        kw = min(bw * 0.42, kh * 2.0)
        cx = (x0 + x1) / 2.0
        ky0 = y0
        d.rounded_rectangle((cx - kw / 2, ky0, cx + kw / 2, ky0 + kh), kh * 0.45, fill=RIM)
        rim = max(1.0, kh * 0.09)
        d.rounded_rectangle((cx - kw / 2 + rim, ky0 + rim, cx + kw / 2 - rim, ky0 + kh - rim), kh * 0.45 - rim, fill=DISC)
        lw = max(1.0, kh * 0.11)
        by = ky0 + kh * 0.72
        for ang in (-50, 0, 50):
            a = math.radians(ang - 90)
            r0, r1 = kh * 0.14, kh * 0.42
            d.line((cx + r0 * math.cos(a), by + r0 * math.sin(a), cx + r1 * math.cos(a), by + r1 * math.sin(a)), fill=INK, width=int(round(lw)))
        _label(d, "CREATE", (x0, ky0 + kh + bh * 0.05, x1, y1), FONT_BOLD, INK, 0.95)
    out = _down(img, w, h)
    out[..., 3] *= top_alpha
    return out


# --------------------------------------------------------------------------------------------
# The keyboard's keys in place of the button symbols (TF_KEY_PROMPTS: while the keyboard and mouse are the
# device last used): each button's key from the player's layout (controls.txt beside the game, written by
# the F1 Controls page; else the port's defaults, ps2_pad.cpp's kDefaultKeymap), drawn as a key cap in the
# PS5 symbols' style, in the symbol's own place and size.

DEFAULT_LAYOUT = (
    "up=UP;down=DOWN;left=LEFT;right=RIGHT;cross=X;circle=C,BACKSPACE;square=R,KP_0;"
    "triangle=T,V,F,KP_1;l1=SPACE,Q,MOUSE_MIDDLE;r1=LEFT_ALT,MOUSE_LEFT;l2=Z,LEFT_SHIFT;"
    "r2=E,LEFT_CONTROL,RIGHT_SHIFT;l3=G;r3=B,MOUSE_RIGHT;start=ENTER,ESCAPE,P;select=TAB")
KEY_SHORT = {"SPACE": "SPACE", "ENTER": "ENTER", "ESCAPE": "ESC", "TAB": "TAB", "BACKSPACE": "BKSP", "INSERT": "INS",
             "DELETE": "DEL", "HOME": "HOME", "END": "END", "PAGE_UP": "PGUP", "PAGE_DOWN": "PGDN", "LEFT_SHIFT": "SHIFT",
             "RIGHT_SHIFT": "SHIFT", "LEFT_CONTROL": "CTRL", "RIGHT_CONTROL": "CTRL", "LEFT_ALT": "ALT", "RIGHT_ALT": "ALT",
             "CAPS_LOCK": "CAPS", "GRAVE": "`", "MINUS": "-", "EQUAL": "=", "LEFT_BRACKET": "[", "RIGHT_BRACKET": "]",
             "BACKSLASH": "\\", "SEMICOLON": ";", "APOSTROPHE": "'", "COMMA": ",", "PERIOD": ".", "SLASH": "/",
             "KP_ENTER": "ENT", "KP_ADD": "+", "KP_SUBTRACT": "-", "KP_MULTIPLY": "*", "KP_DIVIDE": "/"}
KEY_MODIFIERS = {"LEFT_SHIFT", "RIGHT_SHIFT", "LEFT_CONTROL", "RIGHT_CONTROL", "LEFT_ALT", "RIGHT_ALT"}


def key_layout(controls):
    """button -> its keys (the defaults, the player's controls.txt over them)."""
    def parse(text):
        out = {}
        for part in text.split(";"):
            if "=" in part:
                k, v = part.split("=", 1)
                out[k.strip().lower()] = [x.strip().upper() for x in v.split(",") if x.strip()]
        return out
    layout = parse(DEFAULT_LAYOUT)
    if controls and os.path.exists(controls):
        with open(controls, encoding="utf-8", errors="replace") as f:
            layout.update(parse(f.read().strip()))
    return layout


def shown_key(keys):
    """The key a prompt shows for a button: its first, or its mouse button behind a modifier key."""
    if not keys:
        return None
    mouse = [k for k in keys if k.startswith("MOUSE_")]
    if keys[0] in KEY_MODIFIERS and mouse:
        return mouse[0]
    return keys[0]


def draw_keycaps(keys, w, h, ink_box_px, top_alpha):
    """Key caps (one, or two side by side for a two-arrow cell) filling the symbol's ink box: a dark key with
    a thin light rim and its name (an arrow key its arrow; a mouse button the mouse with that button lit)."""
    img, d, s = _supersampled(w, h)
    x0, y0, x1, y1 = [v * SS for v in ink_box_px]
    n = len(keys)
    gap = (x1 - x0) * 0.06 if n > 1 else 0.0
    bw = ((x1 - x0) - gap * (n - 1)) / n
    for i, key in enumerate(keys):
        bx0 = x0 + i * (bw + gap)
        bx1, by0, by1 = bx0 + bw, y0, y1
        side = min(bx1 - bx0, by1 - by0)
        rim = max(1.0, side * 0.07)
        rad = side * 0.22
        d.rounded_rectangle((bx0, by0, bx1, by1), rad, fill=RIM)
        d.rounded_rectangle((bx0 + rim, by0 + rim, bx1 - rim, by1 - rim), max(0.0, rad - rim), fill=DISC)
        fx0, fy0, fx1, fy1 = bx0 + rim, by0 + rim, bx1 - rim, by1 - rim
        if key and key.startswith("MOUSE_"):
            # (on the key: a light mouse, the button that is meant filled in)
            mh = (fy1 - fy0) * 0.80
            mw = min(mh * 0.66, (fx1 - fx0) * 0.8)
            cx, cy = (fx0 + fx1) / 2.0, (fy0 + fy1) / 2.0
            mx0, my0, mx1, my1 = cx - mw / 2.0, cy - mh / 2.0, cx + mw / 2.0, cy + mh / 2.0
            lw = max(1.0, mw * 0.12)
            d.rounded_rectangle((mx0, my0, mx1, my1), mw * 0.48, outline=INK, width=int(round(lw)))
            split = my0 + mh * 0.42
            d.line((mx0 + lw / 2, split, mx1 - lw / 2, split), fill=INK, width=int(round(lw)))
            d.line((cx, my0 + lw / 2, cx, split), fill=INK, width=int(round(lw)))
            if key == "MOUSE_LEFT":
                d.pieslice((mx0, my0, mx1, my0 + mw), 180, 270, fill=INK)
                d.rectangle((mx0 + lw / 2, my0 + mw / 2.0, cx, split), fill=INK)
            elif key == "MOUSE_RIGHT":
                d.pieslice((mx0, my0, mx1, my0 + mw), 270, 360, fill=INK)
                d.rectangle((cx, my0 + mw / 2.0, mx1 - lw / 2, split), fill=INK)
            else:
                d.rounded_rectangle((cx - mw * 0.12, my0 + mh * 0.08, cx + mw * 0.12, split - mh * 0.02), mw * 0.1, fill=INK)
        elif key in ("UP", "DOWN", "LEFT", "RIGHT"):
            cx, cy = (fx0 + fx1) / 2.0, (fy0 + fy1) / 2.0
            k = min(fx1 - fx0, fy1 - fy0) * 0.28
            pts = {"UP": [(cx, cy - k), (cx - k, cy + k * 0.7), (cx + k, cy + k * 0.7)],
                   "DOWN": [(cx, cy + k), (cx - k, cy - k * 0.7), (cx + k, cy - k * 0.7)],
                   "LEFT": [(cx - k, cy), (cx + k * 0.7, cy - k), (cx + k * 0.7, cy + k)],
                   "RIGHT": [(cx + k, cy), (cx - k * 0.7, cy - k), (cx - k * 0.7, cy + k)]}[key]
            d.polygon(pts, fill=INK)
        elif key:
            label = KEY_SHORT.get(key, key.replace("KP_", "NUM"))
            _label(d, label, (fx0, fy0, fx1, fy1), FONT_BOLD, INK, 0.62 if len(label) <= 2 else 0.42)
    out = _down(img, w, h)
    out[..., 3] *= top_alpha
    return out


# --------------------------------------------------------------------------------------------

def find_file(root, rel_dir, stem, exts):
    d = os.path.join(root, rel_dir)
    if not os.path.isdir(d):
        return None
    want = {stem.upper() + e.upper() for e in exts}
    for f in os.listdir(d):
        if f.upper() in want:
            return os.path.join(d, f)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--archives", help="the extracted archives (TFA2/FONTS/*.VBN under it; --keys-only: the last run's)")
    ap.add_argument("--converted", help="the converted textures (TFA2/FONTS/*.png under it; --keys-only: the last run's)")
    ap.add_argument("--upscaled", help="the upscaled set (the PS2 symbols and the text-off base come from it)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--preview", help="also write side-by-side previews here")
    ap.add_argument("--only", help="only this atlas (e.g. DECK)")
    ap.add_argument("--verbose", action="count", default=0, help="list the glyphs kept rebuilt (twice: all)")
    ap.add_argument("--controls", help="the player's keyboard layout (controls.txt beside the game): the key prompts' keys")
    ap.add_argument("--keys-only", action="store_true",
                    help="only the key-prompt sets again (after the layout changed: fast; reuses text_ps5)")
    args = ap.parse_args()
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from jsonfile import read_json, write_json  # (the fonts' manifest read and written safely)
    if args.keys_only and not (args.archives and args.converted):
        # (the game, after the player changed the layout: the folders the full run used, from its manifest)
        src = read_json(os.path.join(args.out, "manifest.json"), {}, "the sharp fonts' manifest (manifest.json)").get("sources", {})
        if isinstance(src, dict):
            args.archives = args.archives or src.get("archives")
            args.converted = args.converted or src.get("converted")
            args.upscaled = args.upscaled or src.get("upscaled")
    if not (args.archives and args.converted):
        sys.exit("--archives and --converted are needed (or --keys-only after a full run)")

    for f in (FONT_REGULAR, FONT_BOLD):
        if not os.path.exists(f):
            sys.exit(f"missing {f}")
    fonts_dir = os.path.join(args.archives, "TFA2", "FONTS")
    if not os.path.isdir(fonts_dir):
        sys.exit(f"no {fonts_dir}: extract the disc's archives first (make_hd_textures.ps1)")

    # The glyph tables, grouped by atlas.
    by_atlas = {}
    for f in sorted(os.listdir(fonts_dir)):
        if not f.upper().endswith(".VBN"):
            continue
        font = os.path.splitext(f)[0].upper()
        atlas, glyphs = read_vbn(os.path.join(fonts_dir, f))
        if atlas.upper() not in ATLASES:
            continue
        by_atlas.setdefault(atlas.upper(), []).append((font, glyphs))

    rel_dir = "TFA2/FONTS"
    manifest = {"version": VERSION, "scale": SCALE, "atlases": {}}
    layout = key_layout(args.controls)
    manifest["controls"] = {b: shown_key(k) for b, k in sorted(layout.items())}
    if not args.only:
        # (made whole again: an atlas an older version made and this one does not is not left behind)
        for name in (("text_keys", "keys") if args.keys_only else ("text_ps5", "text_ps2", "ps5", "text_xbox", "xbox", "text_xbox360", "xbox360", "text_keys", "keys")):
            shutil.rmtree(os.path.join(args.out, name), ignore_errors=True)
    for atlas, fonts in sorted(by_atlas.items()):
        if args.only and atlas != args.only.upper():
            continue
        src = find_file(args.converted, rel_dir, atlas, [".png"])
        if not src:
            print(f"{atlas}: no converted image, skipped")
            continue
        native = to_float(Image.open(src))
        h, w = native.shape[:2]
        W, H = w * SCALE, h * SCALE
        ups = find_file(args.upscaled, rel_dir, atlas, [".png"]) if args.upscaled else None
        if ups:
            base = to_float(Image.open(ups).convert("RGBA").resize((W, H), Image.LANCZOS))
        else:
            base = to_float(Image.open(src).convert("RGBA").resize((W, H), Image.BICUBIC))

        has_keys = any(c in BUTTONS.get(f, {}) or c in ARROWS.get(f, {}) for f, glyphs in fonts for c, v in glyphs)
        prev = os.path.join(args.out, "text_ps5", rel_dir, atlas + ".png")
        if args.keys_only and (not has_keys or not os.path.exists(prev)):
            continue
        # (1) Crisp text: the whole atlas rebuilt sharp (what is between the glyphs), every glyph traced into
        # a vector outline of its own shape, then (DECK) the vector font's letters over them.
        notes = {"vector": 0, "kept": 0, "symbols": 0, "arrows": 0, "traced": 0}
        check = []
        if args.keys_only:
            crisp = to_float(Image.open(prev))
        else:
            crisp = rebuild_sharp(native, SCALE)
            all_cells = sorted({cell_of(v, w, h) for font, glyphs in fonts for c, v in glyphs})
            crisp = trace_cells(native, crisp, all_cells, SCALE, check)
        notes["traced"] = len(check)
        if check:
            worst = max(check)
            notes["trace_check"] = {"mean_difference": round(float(np.mean([c[0] for c in check])), 4),
                                    "worst_difference": round(worst[0], 4), "worst_cell": list(worst[2]),
                                    "min_overlap": round(min(c[1] for c in check), 3)}
        for font, glyphs in fonts:
            if font not in VECTOR_FONTS or args.keys_only:
                continue
            fitter = VectorFitter(FONT_REGULAR, 480)
            # (a letter's code over a symbol's or an arrow's cell - '_' and '^' are DECK's down and up arrows -
            # is not a letter)
            special = {cell_of(v, w, h) for c, v in glyphs if c in BUTTONS.get(font, {}) or c in ARROWS.get(font, {})}
            cells = []
            for code, vals in glyphs:
                if code <= 32 or code in BUTTONS.get(font, {}) or code in ARROWS.get(font, {}) or not (33 <= code <= 255):
                    continue
                if cell_of(vals, w, h) in special:
                    continue
                ch = bytes([code]).decode("cp1252", errors="replace")
                if ch == "�" or not ch.isprintable():
                    continue
                x0, y0, x1, y1 = cell_of(vals, w, h)
                if x1 <= x0 or y1 <= y0:
                    continue
                cells.append((code, ch, (x0, y0, x1, y1)))
            # The font's median proportion (the original's width over the vector glyph's, height-matched).
            ratios = []
            for code, ch, (x0, y0, x1, y1) in cells:
                a = native[y0:y1, x0:x1, 3]
                top = a.max()
                if top <= 0:
                    continue
                cov = resize_plane(a / top, ((x1 - x0) * SCALE, (y1 - y0) * SCALE))
                ob = ink_box(cov)
                vec = render_char(fitter.font(fitter.em), ch, 0)
                if ob is None or vec is None or not ch.isalnum():
                    continue
                if ob[2] - ob[0] >= 3.5 * SCALE:
                    ratios.append(((ob[2] - ob[0]) / (ob[3] - ob[1])) / (vec.shape[1] / vec.shape[0]))
            ratio = float(np.median(ratios)) if ratios else 1.0
            # The stroke matched by coverage: try a few weights, keep the one whose ink matches best.
            best = None
            for stroke in (0, 4, 8, 12, 16, 20, 24):
                tot_o = tot_v = 0.0
                for code, ch, (x0, y0, x1, y1) in cells:
                    if not ch.isalnum():
                        continue
                    a = native[y0:y1, x0:x1, 3]
                    top = a.max()
                    if top <= 0:
                        continue
                    cov = resize_plane(a / top, ((x1 - x0) * SCALE, (y1 - y0) * SCALE))
                    v, q = fitter.fit(cov, ch, stroke, ratio)
                    if v is None:
                        continue
                    tot_o += float(np.clip(cov, 0, 1).sum())
                    tot_v += float(v.sum())
                err = abs(tot_v / max(tot_o, 1e-6) - 1.0)
                if best is None or err < best[0]:
                    best = (err, stroke)
            stroke = best[1]
            for code, ch, (x0, y0, x1, y1) in cells:
                a = native[y0:y1, x0:x1]
                top = float(a[..., 3].max())
                if top <= 0:
                    continue
                cov = resize_plane(a[..., 3] / top, ((x1 - x0) * SCALE, (y1 - y0) * SCALE))
                v, q = fitter.fit(cov, ch, stroke, ratio)
                if v is None or q < 0.55:
                    notes["kept"] += 1  # (not the same shape - an icon under a letter's code: kept traced)
                    if args.verbose:
                        print(f"  {font} {code} {ch!r}: kept rebuilt (overlap {q:.2f})")
                    continue
                if args.verbose > 1:
                    print(f"  {font} {code} {ch!r}: drawn (overlap {q:.2f})")
                opaque = a[..., 3] > 0.5 * top
                color = a[..., :3][opaque].mean(axis=0) if opaque.any() else np.array([1.0, 1.0, 1.0])
                sl = crisp[y0 * SCALE:y1 * SCALE, x0 * SCALE:x1 * SCALE]
                sl[..., :3] = color
                sl[..., 3] = np.clip(v, 0, 1) * top
                notes["vector"] += 1
            manifest_font = {"stroke": stroke, "ratio": round(ratio, 4)}
            manifest["atlases"].setdefault(atlas, {})[font] = manifest_font

        # The D-pad's arrows, drawn sharp with the text.
        arrow_cells = {}
        for font, glyphs in fonts:
            for code, vals in glyphs:
                if code in ARROWS.get(font, {}):
                    arrow_cells[cell_of(vals, w, h)] = ARROWS[font][code]
        for (x0, y0, x1, y1), kinds in (arrow_cells.items() if not args.keys_only else ()):
            a = native[y0:y1, x0:x1]
            top = float(a[..., 3].max()) if a[..., 3].max() > 0 else 1.0
            cov = resize_plane(a[..., 3] / top, ((x1 - x0) * SCALE, (y1 - y0) * SCALE))
            ob = ink_box(cov, 0.5) or (0, 0, (x1 - x0) * SCALE, (y1 - y0) * SCALE)
            crisp[y0 * SCALE:y1 * SCALE, x0 * SCALE:x1 * SCALE] = draw_arrows(kinds.split(), (x1 - x0) * SCALE, (y1 - y0) * SCALE, ob, top, a)
            notes["arrows"] = notes.get("arrows", 0) + 1

        # (2) The symbols: PS5 drawn, PS2 from the base (upscaled).
        ps5_cells = []
        for font, glyphs in fonts:
            for code, vals in glyphs:
                kind = BUTTONS.get(font, {}).get(code)
                if not kind:
                    continue
                x0, y0, x1, y1 = cell_of(vals, w, h)
                ps5_cells.append((kind, (x0, y0, x1, y1)))
        ps5_cells = list(dict.fromkeys(ps5_cells))

        def with_symbols(img, ps5):
            out = img.copy()
            for kind, (x0, y0, x1, y1) in ps5_cells:
                X0, Y0, X1, Y1 = x0 * SCALE, y0 * SCALE, x1 * SCALE, y1 * SCALE
                if not ps5:
                    out[Y0:Y1, X0:X1] = base[Y0:Y1, X0:X1]
                    continue
                a = native[y0:y1, x0:x1, 3]
                top = float(a.max()) if a.max() > 0 else 1.0
                cov = resize_plane(a / top, ((x1 - x0) * SCALE, (y1 - y0) * SCALE))
                ob = ink_box(cov, 0.3) or (0, 0, X1 - X0, Y1 - Y0)
                if ps5 in ("xbox", "xbox360"):
                    sym = draw_xbox(kind, X1 - X0, Y1 - Y0, ob, top, "360" if ps5 == "xbox360" else "one")
                else:
                    sym = draw_symbol(kind, X1 - X0, Y1 - Y0, ob, top)
                out[Y0:Y1, X0:X1] = sym
            return out

        def with_keys(img):
            out = img.copy()
            for kinds, (x0, y0, x1, y1) in [(k, c) for k, c in ps5_cells] + [(v, c) for c, v in arrow_cells.items()]:
                X0, Y0, X1, Y1 = x0 * SCALE, y0 * SCALE, x1 * SCALE, y1 * SCALE
                a = native[y0:y1, x0:x1, 3]
                top = float(a.max()) if a.max() > 0 else 1.0
                cov = resize_plane(a / top, ((x1 - x0) * SCALE, (y1 - y0) * SCALE))
                ob = ink_box(cov, 0.3) or (0, 0, X1 - X0, Y1 - Y0)
                keys = [shown_key(layout.get(k, [])) for k in kinds.split()]
                out[Y0:Y1, X0:X1] = draw_keycaps(keys, X1 - X0, Y1 - Y0, ob, top)
            return out

        sets = {} if args.keys_only else {"text_ps5": with_symbols(crisp, True), "text_ps2": with_symbols(crisp, False)}
        if ps5_cells and not args.keys_only:
            sets["ps5"] = with_symbols(base, True)
            sets["xbox"] = with_symbols(base, "xbox")
            sets["text_xbox"] = with_symbols(crisp, "xbox")
            sets["xbox360"] = with_symbols(base, "xbox360")
            sets["text_xbox360"] = with_symbols(crisp, "xbox360")
            notes["symbols"] = len(ps5_cells)
        if ps5_cells:
            # (the keyboard's keys: with the crisp text, and with the text as it was)
            sets["text_keys"] = with_keys(crisp)
            sets["keys"] = with_keys(base)
        for name, img in sets.items():
            img = fill_rgb(img, passes=4)
            dst = os.path.join(args.out, name, rel_dir, atlas + ".png")
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            to_image(img).save(dst, optimize=False)
        manifest["atlases"].setdefault(atlas, {})["_notes"] = notes
        print(f"{atlas}: {W}x{H}, {notes['traced']} glyphs traced (" + (f"coverage off by {notes['trace_check']['mean_difference']:.3f} on average, "
              f"{notes['trace_check']['worst_difference']:.3f} at most" if check else "none") + f"), {notes['vector']} letters drawn from the font, {notes['kept']} kept traced, "
              f"{notes['symbols']} button symbols ({', '.join(sorted(sets))})")
        if args.preview:
            os.makedirs(args.preview, exist_ok=True)
            bg = Image.new("RGBA", (W * 3, H), (40, 44, 80, 255))
            for i, im in enumerate((base, sets.get("text_ps2", base), sets.get("text_ps5", base))):
                bg.alpha_composite(to_image(im), (i * W, 0))
            bg.save(os.path.join(args.preview, atlas + "_preview.png"))
            for name, im in sets.items():
                preview = Image.new("RGBA", (W, H), (40,44,80,255))
                preview.alpha_composite(to_image(im))
                preview.save(os.path.join(args.preview, atlas + "_" + name + ".png"))

    with open(FONT_REGULAR, "rb") as f:
        manifest["font_sha256"] = hashlib.sha256(f.read()).hexdigest()
    manifest["sources"] = {"archives": os.path.abspath(args.archives), "converted": os.path.abspath(args.converted),
                           "upscaled": os.path.abspath(args.upscaled) if args.upscaled else None}
    if args.keys_only:
        # (the rest of the manifest is the full run's)
        old = read_json(os.path.join(args.out, "manifest.json"), {}, "the sharp fonts' manifest (manifest.json)")
        if old:
            old["controls"] = manifest["controls"]
            manifest = old
    os.makedirs(args.out, exist_ok=True)
    write_json(os.path.join(args.out, "manifest.json"), manifest, indent=1)
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
