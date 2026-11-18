# nr_core S5: the stored reference of the core's smoke test (nr_core_smoke.cpp, kReferenceHash) - FNV-1a 64 of the expected picture's
# RGB, row by row from the top, made from the same definition: 256 x 144; x < 128 the pattern (R = 2x & 255, G = y * 255 // 143,
# B = 200 on odd 8-pixel checker squares else 40), the rest (nr_core S7) the PS2-format texture's texel at the pixel.
# (nr_core S7) The PS2-format texture (kTexPixelHash, kTexContentKey): 256 x 256, index (x + 2y) & 255, palette entry j as uploaded
# (R j, G 255 - j, B 7j & 255, A j >> 1), read at the index with bits 3 and 4 exchanged (CSM1), alpha doubled (min 255); its
# pixelHash (nr_texture.cpp: FNV over the RGBA8 texels 8 bytes at a time, R in the low byte) and its HD-pack content key
# (hdContentKey: FNV-1a over the name's bytes, then width, height, game mips as 32-bit, then the pixelHash's 8 bytes).
# Run: python nr_core_smoke_ref.py
import struct

W, H, COPY_W, T = 256, 144, 128, 256
M = 0xFFFFFFFFFFFFFFFF


def texel(x, y):
    i = (x + 2 * y) & 255
    s = (i & ~0x18) | ((i & 8) << 1) | ((i & 0x10) >> 1)
    return s | (255 - s) << 8 | ((7 * s) & 255) << 16 | min((s >> 1) * 2, 255) << 24


h = 0xCBF29CE484222325
for y in range(H):
    for x in range(W):
        if x < COPY_W:
            rgb = ((2 * x) & 255, y * 255 // (H - 1), 200 if (x // 8 + y // 8) & 1 else 40)
        else:
            t = texel(x, y)
            rgb = (t & 255, (t >> 8) & 255, (t >> 16) & 255)
        for b in rgb:
            h = ((h ^ b) * 0x100000001B3) & M
print(f"kReferenceHash 0x{h:016X}ull")

px = [texel(x, y) for y in range(T) for x in range(T)]
ph = 1469598103934665603
for i in range(0, len(px), 2):
    ph = ((ph ^ (px[i] | px[i + 1] << 32)) * 1099511628211) & M
ph = ph or 1
ck = 1469598103934665603
for b in b"SMOKE_T8" + struct.pack("<III", T, T, 0) + struct.pack("<Q", ph):
    ck = ((ck ^ b) * 1099511628211) & M
print(f"kTexPixelHash 0x{ph:016X}ull, kTexContentKey 0x{ck:016X}ull")
