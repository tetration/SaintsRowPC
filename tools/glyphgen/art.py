# Keyboard/mouse button pictures, drawn from scratch (no game artwork).
# Used by make_art.py to render art.png.
from PIL import Image, ImageDraw, ImageFont
import numpy as np

FONT = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'  # DejaVu fonts: free license
S = 4  # supersampling


def set_font(path):
    global FONT
    FONT = path


def font(px):
    return ImageFont.truetype(FONT, px)


def fit_text(draw, text, box_w, box_h, max_px):
    px = max_px
    while px > 6:
        f = font(px)
        l, t, r, b = draw.textbbox((0, 0), text, font=f)
        if r - l <= box_w and b - t <= box_h:
            return f, (l, t, r, b)
        px -= 1
    f = font(6)
    return f, draw.textbbox((0, 0), text, font=f)


def keycap(w, h, label):
    W, H = w * S, h * S
    im = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    dr = ImageDraw.Draw(im)
    m = 1 * S
    r = 6 * S
    # shadow, body, top face
    dr.rounded_rectangle((m, m + S, W - m, H - m), r, fill=(0, 0, 0, 170))
    dr.rounded_rectangle((m, m, W - m, H - m - S), r, fill=(236, 236, 236, 255))
    dr.rounded_rectangle((m + 2 * S, m + 2 * S, W - m - 2 * S, H - m - 4 * S), r - 2 * S, fill=(52, 52, 56, 255))
    bw, bh = W - 2 * m - 8 * S, H - 2 * m - 10 * S
    f, (l, t, rr, b) = fit_text(dr, label, bw, bh, int(bh * 1.05))
    x = (W - (rr - l)) / 2 - l
    y = (H - 2 * S - (b - t)) / 2 - t
    dr.text((x, y), label, font=f, fill=(255, 255, 255, 255))
    return im.resize((w, h), Image.LANCZOS)


def arrow_poly(cx, cy, size, direction):
    s = size
    pts = {'up': [(0, -s), (s, s * 0.2), (-s, s * 0.2)], 'down': [(0, s), (s, -s * 0.2), (-s, -s * 0.2)],
           'left': [(-s, 0), (s * 0.2, -s), (s * 0.2, s)], 'right': [(s, 0), (-s * 0.2, -s), (-s * 0.2, s)]}[direction]
    return [(cx + x, cy + y) for x, y in pts]


def arrowcap(w, h, dirs):
    # one keycap per direction, laid out side by side (or a cross for four)
    W, H = w * S, h * S
    im = Image.new('RGBA', (W, H), (0, 0, 0, 0))

    def cap(x0, y0, x1, y1, d):
        dr = ImageDraw.Draw(im)
        r = 4 * S
        dr.rounded_rectangle((x0, y0 + S, x1, y1), r, fill=(0, 0, 0, 170))
        dr.rounded_rectangle((x0, y0, x1, y1 - S), r, fill=(236, 236, 236, 255))
        dr.rounded_rectangle((x0 + 2 * S, y0 + 2 * S, x1 - 2 * S, y1 - 3 * S), r - S, fill=(52, 52, 56, 255))
        cx, cy = (x0 + x1) / 2, (y0 + y1 - S) / 2
        dr.polygon(arrow_poly(cx, cy, min(x1 - x0, y1 - y0) * 0.22, d), fill=(255, 255, 255, 255))

    if len(dirs) == 4:
        c = W / 3
        cap(c, 0, 2 * c, H / 2, 'up'); cap(0, H / 2, c, H, 'left'); cap(c, H / 2, 2 * c, H, 'down'); cap(2 * c, H / 2, W, H, 'right')
    else:
        n = len(dirs); cw = W / n
        side = min(cw, H)
        y0 = (H - side) / 2
        for i, d in enumerate(dirs):
            x0 = i * cw + (cw - side) / 2
            cap(x0 + S, y0 + S, x0 + side - S, y0 + side - S, d)
    return im.resize((w, h), Image.LANCZOS)


def mouse(w, h, button):
    W, H = w * S, h * S
    im = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    dr = ImageDraw.Draw(im)
    mh = H - 2 * S
    mw = min(W - 2 * S, int(mh * 0.72))
    x0 = (W - mw) / 2
    y0 = S
    x1, y1 = x0 + mw, y0 + mh
    r = mw / 2
    dr.rounded_rectangle((x0, y0 + S, x1, y1), r, fill=(0, 0, 0, 170))
    dr.rounded_rectangle((x0, y0, x1, y1 - S), r, fill=(236, 236, 236, 255))
    ix0, iy0, ix1, iy1 = x0 + 2 * S, y0 + 2 * S, x1 - 2 * S, y1 - 3 * S
    dr.rounded_rectangle((ix0, iy0, ix1, iy1), r - 2 * S, fill=(52, 52, 56, 255))
    midx = (ix0 + ix1) / 2
    split = iy0 + (iy1 - iy0) * 0.45
    hi = (238, 110, 30, 255)
    if button == 'left':
        dr.pieslice((ix0, iy0, ix1, iy0 + (ix1 - ix0)), 180, 270, fill=hi); dr.rectangle((ix0, iy0 + (ix1 - ix0) / 2, midx, split), fill=hi)
    elif button == 'right':
        dr.pieslice((ix0, iy0, ix1, iy0 + (ix1 - ix0)), 270, 360, fill=hi); dr.rectangle((midx, iy0 + (ix1 - ix0) / 2, ix1, split), fill=hi)
    elif button == 'middle':
        dr.rounded_rectangle((midx - 2 * S, iy0 + 2 * S, midx + 2 * S, split - 2 * S), 2 * S, fill=hi)
    dr.line((ix0, split, ix1, split), fill=(236, 236, 236, 255), width=S)
    dr.line((midx, iy0, midx, split), fill=(236, 236, 236, 255), width=S)
    if button == 'move':
        cy = (split + iy1) / 2
        dr.ellipse((midx - 2 * S, cy - 2 * S, midx + 2 * S, cy + 2 * S), fill=hi)
    return im.resize((w, h), Image.LANCZOS)


def plate(w, h, label):
    # A wide dark plate with a large letter, for the Q/E tab buttons of the
    # pause menu and character creator. It fills the picture, with a soft
    # shadow along the bottom edge.
    W, H = w * S, h * S
    im = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    dr = ImageDraw.Draw(im)
    pw, ph = W - 1, H - 2 * S
    x0, y0 = 0, 0
    x1, y1 = x0 + pw, y0 + ph
    r = max(S, min(pw, ph) * 0.14)
    dr.rounded_rectangle((x0, y0 + S, x1, y1 + S), r, fill=(0, 0, 0, 160))       # shadow
    dr.rounded_rectangle((x0, y0, x1, y1), r, fill=(12, 11, 10, 255))             # dark rim
    top = (x0 + 1.5 * S, y0 + 1.5 * S, x1 - 1.5 * S, y1 - 1.5 * S)
    face = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    fd = ImageDraw.Draw(face)
    ih = max(1, int(top[3] - top[1]))
    for i in range(ih):  # warm grey, lighter at the top
        t = i / max(1, ih - 1)
        c = tuple(int(round(a + (b - a) * t)) for a, b in zip((126, 122, 114), (56, 54, 51)))
        fd.line((top[0], top[1] + i, top[2], top[1] + i), fill=c + (255,))
    mask = Image.new('L', (W, H), 0)
    ImageDraw.Draw(mask).rounded_rectangle(top, max(1, r - S), fill=255)
    im.paste(face, (0, 0), mask)
    dr.line((top[0] + r * 0.6, top[1] + S * 0.5, top[2] - r * 0.6, top[1] + S * 0.5), fill=(190, 185, 174, 255), width=S)
    bw, bh = pw * 0.55, ph * 0.62
    f, (l, t, rr, b) = fit_text(dr, label, bw, bh, int(bh * 1.4))
    x = (W - (rr - l)) / 2 - l
    y = (y0 + y1 - (b - t)) / 2 - t
    dr.text((x, y), label, font=f, fill=(255, 255, 255, 255))
    return im.resize((w, h), Image.LANCZOS)


def art(spec, w, h):
    kind, arg = spec[0], spec[1]
    if kind == 'key': im = keycap(w, h, arg)
    elif kind == 'arrows': im = arrowcap(w, h, arg)
    elif kind == 'mouse': im = mouse(w, h, arg)
    elif kind == 'plate': im = plate(w, h, arg)
    else: raise ValueError(kind)
    if len(spec) > 2 and spec[2] == 'off':  # the greyed-out variant
        a = np.array(im).astype(float)
        g = a[:, :, :3].mean(2, keepdims=True) * 0.55
        a[:, :, :3] = g
        im = Image.fromarray(a.astype(np.uint8), 'RGBA')
    return im
