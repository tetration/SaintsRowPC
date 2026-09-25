# Developer tool: turns logo.png into logo.bin for patch.lua.
#
#   python make_logo.py [preview.png]
#
# The main menu shows the top of the legal screen texture (interface-legal_*,
# 1280x768 DXT1). logo.bin holds logo.png on black, fitted into the area of
# that texture the menu shows (REGION below), as DXT1 blocks in the console's
# byte order: a 20-byte header ("SRLG", then the area's first block column,
# first block row, width and height in blocks, little-endian u32) followed by
# 8 bytes per block, row by row. patch.lua writes the blocks into the player's
# own copy of the texture. Needs Pillow and numpy.
import os, struct, sys
from PIL import Image
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REGION = (64, 16, 1216, 272)  # x0, y0, x1, y1 in texture pixels (multiples of 4)


def to565(c):
    r, g, b = [int(v) for v in c]
    return ((r * 31 + 127) // 255) << 11 | ((g * 63 + 127) // 255) << 5 | ((b * 31 + 127) // 255)


def from565(v):
    return np.array([((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31], dtype=float)


def dxt1_block(px):
    rgb = px[:, :3].astype(float)
    mean = rgb.mean(0)
    cov = np.cov((rgb - mean).T)
    ev = np.linalg.eigh(cov)[1][:, -1] if np.abs(cov).sum() > 1e-9 else np.array([1.0, 1.0, 1.0]) / 3 ** .5
    proj = (rgb - mean) @ ev
    c0, c1 = to565(np.clip(mean + ev * proj.max(), 0, 255)), to565(np.clip(mean + ev * proj.min(), 0, 255))
    if c0 < c1:
        c0, c1 = c1, c0
    if c0 == c1:  # four-colour mode needs c0 > c1
        if c0 > 0:
            c1 = c0 - 1
        else:
            c0 = 1
    p0, p1 = from565(c0), from565(c1)
    pal = [p0, p1, (2 * p0 + p1) / 3, (p0 + 2 * p1) / 3]
    idx = 0
    for i in range(16):
        d = [((rgb[i] - p) ** 2).sum() for p in pal]
        idx |= int(np.argmin(d)) << (2 * i)
    b = struct.pack('<HHI', c0, c1, idx)
    return b''.join(b[i + 1:i + 2] + b[i:i + 1] for i in range(0, 8, 2))  # console byte order


def main():
    x0, y0, x1, y1 = REGION
    w, h = x1 - x0, y1 - y0
    logo = Image.open(os.path.join(HERE, 'logo.png')).convert('RGBA')
    logo = logo.crop(logo.getbbox())
    scale = min(w / logo.width, h / logo.height)
    lw, lh = int(logo.width * scale), int(logo.height * scale)
    logo = logo.resize((lw, lh), Image.LANCZOS)
    canvas = Image.new('RGBA', (w, h), (0, 0, 0, 255))
    canvas.alpha_composite(logo, ((w - lw) // 2, (h - lh) // 2))
    a = np.array(canvas.convert('RGB'))
    bw, bh = w // 4, h // 4
    out = [b'SRLG', struct.pack('<4I', x0 // 4, y0 // 4, bw, bh)]
    for by in range(bh):
        for bx in range(bw):
            px = np.concatenate([a[by * 4:by * 4 + 4, bx * 4:bx * 4 + 4].reshape(16, 3), np.full((16, 1), 255)], 1)
            out.append(dxt1_block(px))
    with open(os.path.join(HERE, 'logo.bin'), 'wb') as f:
        f.write(b''.join(out))
    print('logo %dx%d in %dx%d at (%d,%d), %d blocks' % (lw, lh, w, h, x0, y0, bw * bh))
    if len(sys.argv) > 1:
        canvas.save(sys.argv[1])


if __name__ == '__main__':
    main()
