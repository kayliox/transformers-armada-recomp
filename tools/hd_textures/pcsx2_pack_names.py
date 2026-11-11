"""pcsx2_pack_names.py - the PCSX2 texture-pack name table (tools/hd_textures/pcsx2_names.json), made from a pack.

A PCSX2 replacement pack names each image by PCSX2's hash of the texture as the game uploads it:
<texture hash>-<palette hash>-<TEX0 bits>.dds|png (true-colour textures: <hash>-<bits>), the bits being
PSM | TW << 6 | TH << 10. The same texture gives the same name in every pack of the disc, so a table of
name -> our texture name made once from one pack serves any pack (the game finds a pack's images by it).

Each pack image is shrunk to its GS size and compared with every texture of hd_textures/converted/textures of
that size (premultiplied colour and alpha, each side's alpha scaled to its own maximum); a weak match is tried
again as a mip level (a texture 2, 4 or 8 times the size, shrunk). Only confident, same-size, non-mip matches
(mean difference under 12 of 255) go in the table. The table holds hashes and names only: no image data.

  python tools/hd_textures/pcsx2_pack_names.py <pack replacements folder> <converted textures folder> <out.json>

Needs Pillow and numpy. The pack is the player's own and is never copied.
"""
import sys, os, re, glob, json, numpy as np
from PIL import Image
from collections import defaultdict
pack, conv, out = sys.argv[1], sys.argv[2], sys.argv[3]
raw_out = out + '.raw.json'
SEP = chr(92)
bysize = defaultdict(list)
for f in glob.glob(os.path.join(conv, '**', '*.png'), recursive=True):
    bysize[Image.open(f).size].append(f)
cache = {}
def norm(a):
    a = a.astype(np.float32); am = a[..., 3:4].max(axis=(-3, -2), keepdims=True); am = np.where(am < 1, 255, am)
    al = a[..., 3:4] / am
    return np.concatenate([a[..., :3] * al, al * 255], axis=-1)
def group(size):
    if size not in cache:
        fs = bysize.get(size, [])
        cache[size] = (fs, norm(np.stack([np.asarray(Image.open(f).convert('RGBA')) for f in fs])) if fs else None)
    return cache[size]
rx = re.compile(r'^([0-9a-f]{1,16})-([0-9a-f]{1,16})-([0-9a-f]{8})\.(dds|png)$', re.I)
rx2 = re.compile(r'^([0-9a-f]{1,16})-([0-9a-f]{8})\.(dds|png)$', re.I)  # (no CLUT: true-colour textures)
table = {}
for f in sorted(glob.glob(os.path.join(pack, '**', '*.*'), recursive=True)):
    b = os.path.basename(f); m = rx.match(b) or rx2.match(b)
    if not m: continue
    bits = int(m.groups()[-2], 16); W = 1 << ((bits >> 6) & 0xF); H = 1 << ((bits >> 10) & 0xF)
    try: im = Image.open(f).convert('RGBA')
    except Exception: table[b] = {'unreadable': True}; continue
    full = np.asarray(im.resize((W, H), Image.BOX))
    best = None
    for (w, h) in bysize:
        if w > W or h > H: continue
        fs, a = group((w, h))
        if a is None: continue
        p = norm(full[None, :h, :w])
        err = np.abs(a - p).mean(axis=(1, 2, 3))
        k = int(np.argmin(err)); e = float(err[k])
        if best is None or e < best[0]:
            srt = np.sort(err); best = (e, fs[k], float(srt[1]) if len(srt) > 1 else 999.0, w, h)
    if best:
        table[b] = {'name': os.path.relpath(best[1], conv).replace(SEP, '/'), 'err': round(best[0], 2), 'next': round(best[2], 2),
                    'w': best[3], 'h': best[4], 'W': W, 'H': H}
json.dump(table, open(raw_out, 'w'), indent=0)
v = [x for x in table.values() if 'err' in x]
print(len(table), 'files;', 'unreadable', sum(1 for x in table.values() if x.get('unreadable')),
      '; err<4', sum(x['err'] < 4 for x in v), '<8', sum(4 <= x['err'] < 8 for x in v), '<12', sum(8 <= x['err'] < 12 for x in v),
      '<16', sum(12 <= x['err'] < 16 for x in v), '>=16', sum(x['err'] >= 16 for x in v))

tin = raw_out
t = json.load(open(tin))
cache = {}
def group(W, H, k):
    key = (W, H, k)
    if key not in cache:
        fs = bysize.get((W << k, H << k), [])
        cache[key] = (fs, norm(np.stack([np.asarray(Image.open(f).convert('RGBA').resize((W, H), Image.BOX)) for f in fs])) if fs else None)
    return cache[key]
changed = 0
for b, v in t.items():
    if 'err' not in v or v['err'] < 12: continue
    p = norm(np.asarray(Image.open(os.path.join(pack, b)).convert('RGBA').resize((v['W'], v['H']), Image.BOX))[None])
    best = None
    for k in (1, 2, 3):
        fs, a = group(v['W'], v['H'], k)
        if a is None: continue
        err = np.abs(a - p).mean(axis=(1, 2, 3)); i = int(np.argmin(err))
        if best is None or err[i] < best[0]: best = (float(err[i]), fs[i], k)
    if best and best[0] < v['err'] and best[0] < 12:
        v.update({'name': os.path.relpath(best[1], conv).replace(SEP, '/'), 'err': round(best[0], 2), 'mip': best[2]}); changed += 1

vals = [x for x in t.values() if 'err' in x]
print('mip matches added', changed, '; err<12', sum(x['err'] < 12 for x in vals), '12-16', sum(12 <= x['err'] < 16 for x in vals), '>=16', sum(x['err'] >= 16 for x in vals),
      '; of them mip levels', sum(1 for x in vals if x.get('mip')))

names = {}
for k, v in sorted(t.items()):
    if 'err' in v and not v.get('mip') and v['w'] == v['W'] and v['h'] == v['H'] and v['err'] < 12:
        names[os.path.splitext(k)[0].lower()] = v['name'][:-4] if v['name'].lower().endswith('.png') else v['name']
doc = {"_about": "PCSX2 texture replacement names (texture hash - palette hash - TEX0 bits) of SLUS-20668 / SLES-52388 mapped "
                  "to this port's texture names (hd_textures/converted/textures relpaths, no extension). Hashes and names only, no "
                  "image data. Made by tools/hd_textures/pcsx2_pack_names.py.", "names": names}
json.dump(doc, open(out, 'w'), indent=0, sort_keys=True)
print(len(names), 'names in the table')
