# Developer tool: renders the keyboard/mouse button pictures (art.py) into
# art.png and writes art.txt, the list of where glyphgen pastes each one.
#
#   python make_art.py <game>/packfiles [--font DejaVuSans-Bold.ttf] [--reference out.bin]
#
# Reads the game's packfiles only to learn the size and position of each
# controller button picture. art.png and art.txt contain no game data: the
# pictures are drawn from scratch, and the list holds texture names and
# rectangles. glyphgen (run by setup on the player's PC) combines them with the
# player's own game files into dist/kbm_ui.bin.
#
# Needs Pillow and numpy. --reference also writes kbm_ui.bin the slow way, to
# compare with glyphgen's output.
import argparse, os, struct, sys, zlib
from PIL import Image
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import art as artmod

# Contexts, in the order the game code numbers them (project/src/glyphs.h).
CTX = ['menu', 'foot', 'vehicle', 'creator']
ARROWS4 = ('arrows', ['up', 'left', 'down', 'right'])
MAP = {
    'menu':    {'a': ('key', 'ENTER'), 'b': ('key', 'ESC'), 'select': ('key', 'TAB'), 'x': ('key', 'X'), 'y': ('key', 'Y'),
                'lt': ('plate', 'Q'), 'rt': ('plate', 'E'), 'lt_tab': ('plate', 'Q'), 'rt_tab': ('plate', 'E'),
                'ls': ('mouse', 'move'), 'rs': ('mouse', 'move'), 'dpad': ARROWS4},
    'foot':    {'a': ('key', 'R'), 'b': ('key', 'Q'), 'x': ('key', 'SPACE'), 'y': ('key', 'E'), 'lb': ('key', 'F'),
                'rb': ('key', 'SHIFT'), 'lt': ('mouse', 'right'), 'rt': ('mouse', 'left'), 'ls': ('mouse', 'move'),
                'rs': ('mouse', 'move'), 'dpad': ARROWS4, 'select': ('key', 'TAB')},
    'vehicle': {'a': ('key', 'W'), 'b': ('key', 'Q'), 'x': ('key', 'S'), 'y': ('key', 'E'), 'lb': ('key', 'Z'),
                'rb': ('key', 'C'), 'lt': ('key', 'SPACE'), 'rt': ('mouse', 'left'), 'ls': ('mouse', 'move'),
                'rs': ('mouse', 'move'), 'dpad': ARROWS4, 'select': ('key', 'TAB')},
    'creator': {'a': ('mouse', 'left'), 'b': ('mouse', 'right'), 'y': ('key', 'Y'), 'x': ('key', 'X'),
                'lt': ('plate', 'Q'), 'rt': ('plate', 'E'), 'lt_tab': ('plate', 'Q'), 'rt_tab': ('plate', 'E'),
                'ls': ('key', 'W/S'), 'rs': ('mouse', 'move'), 'dpad': ('arrows', ['up', 'down'])},
}
# D-pad pictures that show particular directions keep showing those.
DPAD_DIRS = {'dpad': None, 'dpad_up': ['up'], 'dpad_down': ['down'], 'dpad_left': ['left'], 'dpad_right': ['right'],
             'dpad_updown': ['up', 'down'], 'dpad_leftright': ['left', 'right']}

# HUD sprites in interface-backend.peg_xbox2: (sheet, name, x, y, w, h).
SPRITES = [
    ('hud_sheet_15', 'ls', 4, 386, 39, 39), ('hud_sheet_15', 'rs', 49, 386, 39, 39),
    ('hud_sheet_15', 'lb', 93, 386, 39, 29), ('hud_sheet_15', 'rb', 137, 386, 39, 29),
    ('hud_sheet_15', 'lt', 94, 420, 37, 29), ('hud_sheet_15', 'rt', 137, 420, 37, 29),
    ('hud_sheet_15', 'lt_tab', 22, 438, 39, 26), ('hud_sheet_15', 'rt_tab', 2, 477, 40, 26),
    ('hud_sheet_15', 'a', 68, 457, 28, 27), ('hud_sheet_15', 'a_off', 102, 457, 28, 27),
    ('hud_sheet_15', 'b', 136, 457, 28, 27), ('hud_sheet_15', 'b_off', 170, 459, 28, 27),
    ('hud_sheet_15', 'x', 204, 459, 28, 27), ('hud_sheet_15', 'x_off', 238, 459, 28, 27),
    ('hud_sheet_02', 'y', 476, 391, 28, 27), ('hud_sheet_02', 'y_off', 480, 427, 28, 27),
    ('hud_sheet_02', 'dpad_updown', 387, 387, 39, 41), ('hud_sheet_02', 'dpad_leftright', 430, 388, 40, 39),
    ('hud_sheet_02', 'dpad_down', 391, 432, 39, 39), ('hud_sheet_02', 'dpad_right', 435, 432, 39, 39),
    ('hud_sheet_02', 'dpad', 259, 452, 39, 39), ('hud_sheet_02', 'dpad_up', 303, 451, 39, 40),
    ('hud_sheet_02', 'dpad_left', 346, 452, 40, 39),
    ('hud_sheet_01', 'ls', 300, 396, 43, 44), ('hud_sheet_01', 'rs', 460, 460, 43, 44),
    ('hud_sheet_01', 'dpad', 112, 400, 31, 31),
    # Minigames (tagging: rotate the left stick as shown, press buttons).
    ('hud_sheet_10', 'a', 106, 107, 48, 48), ('hud_sheet_10', 'b', 166, 107, 48, 48),
    ('hud_sheet_10', 'x', 106, 167, 48, 48), ('hud_sheet_10', 'y', 166, 167, 48, 48),
    ('hud_sheet_10', 'ls', 437, 106, 56, 56),
    ('hud_sheet_10', 'ls', 137, 324, 25, 26), ('hud_sheet_10', 'ls', 178, 324, 26, 26),
    ('hud_sheet_10', 'x', 215, 324, 24, 26),
]
# Button textures in px_base / px_pause_menu_base.
PX_BTN = {'px_btna.tga': 'a', 'px_btnb.tga': 'b', 'px_btnx.tga': 'x', 'px_btny.tga': 'y',
          'px_btnl1.tga': 'lb', 'px_btnr1.tga': 'rb', 'px_btnl1b.tga': 'lb', 'px_btnr1b.tga': 'rb',
          'px_btnl2.tga': 'lt', 'px_btnr2.tga': 'rt', 'px_btnthumbl.tga': 'ls', 'px_btnthumbr.tga': 'rs',
          'px_btnselect.tga': 'select', 'px_btnaroall4.tga': 'dpad', 'px_btnaroup.tga': 'dpad_up',
          'px_btnarodown.tga': 'dpad_down', 'px_btnaroleft.tga': 'dpad_left', 'px_btnaroright.tga': 'dpad_right',
          'px_btnaroupdwn.tga': 'dpad_updown', 'px_btnarolftrght.tga': 'dpad_leftright'}
# Button characters in the px_thin fonts.
FONT_ICONS = {0x7F: 'a', 0x81: 'b', 0x82: 'y', 0x83: 'x', 0x84: 'lt', 0x86: 'rt', 0x87: 'lb', 0x88: 'rb',
              0x89: 'ls', 0x8B: 'rs', 0x9B: 'dpad'}

PEGS = 'pegfiles.vpp_xbox2'
FONTS = 'misc.vpp_xbox2'


def spec_for(ctx, name):
    m = MAP[ctx]
    base = name[:-4] if name.endswith('_off') else name
    if base.startswith('dpad'):
        if 'dpad' not in m: return None
        dirs = DPAD_DIRS[base]
        return ('arrows', dirs) if dirs else m['dpad']
    if base not in m: return None
    return m[base] + (('off',) if name.endswith('_off') else ())


# ---- game files (read only for names, sizes and positions) -----------------------
def pack_read(path, name):
    """First file called `name` in a packfile (the game uses the first too)."""
    b = open(path, 'rb').read()
    u = lambda i: struct.unpack_from('>I', b, i)[0]
    al = lambda n: (n + 2047) & ~2047
    names = al(2048 + u(0x15c)); data = al(names + u(0x160)); stored = data; comp = u(0x14c) & 1
    for i in range(u(0x154)):
        f = struct.unpack_from('>7I', b, 2048 + 28 * i)
        s = names + f[0]
        n = b[s:b.find(b'\0', s)].decode('latin1')
        at = stored if comp else data + f[2]
        if comp: stored = al(stored + f[5])
        if n.lower() == name.lower():
            raw = b[at:at + (f[5] if comp else f[4])]
            return zlib.decompressobj().decompress(raw) if comp else raw
    raise KeyError(name)


def peg_textures(peg):
    """{name without .tga: (offset, w, h, fmt)} and the list in stored order."""
    cnt = struct.unpack('>H', peg[0x10:0x12])[0]
    out = []
    for j in range(cnt):
        e = 0x18 + j * 0x48
        off, w, h, fmt = struct.unpack('>IHHH', peg[e:e + 10])
        nm = peg[e + 0x16:e + 0x46].split(b'\0')[0].decode('latin1').strip(''.join(map(chr, range(1, 32))))
        out.append((nm, off, w, h, fmt))
    return out


def vf3_cells(d):
    n = struct.unpack('>I', d[8:12])[0]; first = struct.unpack('>I', d[12:16])[0]
    nk = struct.unpack('>I', d[0x20:0x24])[0]
    rec = 0xBC + nk * 4
    W = [struct.unpack('>I', d[rec + i * 16 + 4:rec + i * 16 + 8])[0] for i in range(n)]
    xo = rec + n * 16; yo = xo + n * 4
    X = [struct.unpack('>I', d[xo + i * 4:xo + i * 4 + 4])[0] for i in range(n)]
    Y = [struct.unpack('>I', d[yo + i * 4:yo + i * 4 + 4])[0] for i in range(n)]
    return {first + i: (X[i], Y[i], W[i]) for i in range(n)}


def texture_entries(packdir):
    """(label, pack, file, texture, nbytes, fmt, w, h, small, dark, [(name, x, y, w, h)])"""
    out = []
    pegs = os.path.join(packdir, PEGS)
    backend = pack_read(pegs, 'interface-backend.peg_xbox2')
    sheets = {nm[:-4]: (nm, off, w, h, fmt) for nm, off, w, h, fmt in peg_textures(backend)}
    for sheet in sorted(set(s[0] for s in SPRITES)):
        nm, off, w, h, fmt = sheets[sheet]
        size = ((w + 127) // 128 * 128 // 4) * ((h + 127) // 128 * 128 // 4) * 16
        rects = [(s[1], s[2], s[3], s[4], s[5]) for s in SPRITES if s[0] == sheet]
        out.append(dict(label=sheet, pack=PEGS, file='interface-backend.peg_xbox2', tex=nm, nbytes=size, fmt=fmt,
                        w=w, h=h, small=0, dark=0, rects=rects, data=backend[off:off + size]))
    seen = set()
    for pegname in ('px_base', 'px_pause_menu_base'):
        pp = pack_read(pegs, pegname + '.peg_xbox2')
        for nm, off, w, h, fmt in peg_textures(pp):
            if nm not in PX_BTN or fmt != 0x191: continue
            tex = pp[off:off + 0x4000]
            if tex in seen: continue
            seen.add(tex)
            out.append(dict(label=pegname + ':' + nm[:-4], pack=PEGS, file=pegname + '.peg_xbox2', tex=nm, nbytes=0x4000,
                            fmt=fmt, w=w, h=h, small=1, dark=0, rects=[(PX_BTN[nm], 0, 0, w, h)], data=tex))
    fonts = os.path.join(packdir, FONTS)
    for base in ('px_thin', 'px_thinoutline', 'px_thinvar'):
        chars = vf3_cells(pack_read(fonts, base + '.vf3_xbox2'))
        ys = sorted(set(v[1] for v in chars.values() if v[0] != 0xFFFF))
        row_h = min(b - a for a, b in zip(ys, ys[1:])) - 1
        fpeg = pack_read(fonts, base + '.peg_xbox2')
        nm, off, w, h, fmt = peg_textures(fpeg)[0]
        size = (w // 4) * (h // 4) * 16
        rects = [(FONT_ICONS[c], chars[c][0], chars[c][1], chars[c][2], row_h) for c in FONT_ICONS
                 if c in chars and chars[c][2] >= 12 and chars[c][0] != 0xFFFF]
        out.append(dict(label=base, pack=FONTS, file=base + '.peg_xbox2', tex='#0', nbytes=size, fmt=fmt, w=w, h=h,
                        small=0, dark=int(base != 'px_thin'), rects=rects, data=fpeg[off:off + size]))
    return out


# ---- atlas -------------------------------------------------------------------------
def build(packdir):
    entries = texture_entries(packdir)
    pics = {}    # (spec, w, h) -> image
    pastes = []  # per entry: [(ctx, x, y, w, h, key)]
    for e in entries:
        lst = []
        for ci, ctx in enumerate(CTX):
            for name, x, y, rw, rh in e['rects']:
                spec = spec_for(ctx, name)
                if not spec: continue
                key = (repr(spec), rw, rh)
                if key not in pics: pics[key] = artmod.art(spec, rw, rh).convert('RGBA')
                lst.append((ci, x, y, rw, rh, key))
        pastes.append(lst)
    # shelf packing, tallest first
    order = sorted(pics, key=lambda k: (-k[2], -k[1], k[0]))
    AW = 512; x = y = shelf = 0; pos = {}
    for k in order:
        w, h = k[1], k[2]
        if x + w > AW: x, y, shelf = 0, y + shelf + 1, 0
        pos[k] = (x, y); x += w + 1; shelf = max(shelf, h)
    atlas = Image.new('RGBA', (AW, y + shelf), (0, 0, 0, 0))
    for k, (px, py) in pos.items(): atlas.paste(pics[k], (px, py))
    return entries, pastes, pos, atlas


# Textures found in memory by a 4 KB stretch other than their start. The top of
# hud_sheet_15 holds the main menu logo, which the Saints Reborn logo mod
# changes, so it is found by pixel rows 256-383 (block row 64 onwards).
KEY_OFFSETS = {'hud_sheet_15': 196608}


def write_index(path, entries, pastes, pos):
    with open(path, 'w', newline='\n') as f:
        f.write('# glyphgen art list (made by make_art.py; no game data).\n'
                '# texture <label> <packfile> <file> <texture name or #index> <bytes> <format> <width> <height> <small> <dark> [key]\n'
                '#   key: byte offset of the 4 KB the game finds the texture by (default 0, its start)\n'
                '# paste <context> <x> <y> <width> <height> <art x> <art y>\n'
                'contexts %d\n' % len(CTX))
        for e, lst in zip(entries, pastes):
            key = KEY_OFFSETS.get(e['label'], 0)
            f.write('texture %s %s %s %s %d 0x%X %d %d %d %d%s\n' % (e['label'], e['pack'], e['file'], e['tex'], e['nbytes'],
                                                                      e['fmt'], e['w'], e['h'], e['small'], e['dark'],
                                                                      ' %d' % key if key else ''))
            for ci, x, y, w, h, key in lst:
                ax, ay = pos[key]
                f.write('paste %d %d %d %d %d %d %d\n' % (ci, x, y, w, h, ax, ay))


# ---- reference encoder (same algorithm as glyphgen.cpp) ----------------------------
def tiled(x, y, w, lb):
    aw = (w + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (aw >> 5)) << (lb + 7)
    micro = ((x & 7) + ((y & 6) << 2)) << lb
    off = macro + ((micro & ~15) << 1) + (micro & 15) + ((y & 8) << (3 + lb)) + ((y & 1) << 4)
    return (((off & ~511) << 3) + ((off & 448) << 2) + (off & 63) + ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> lb


def swap16(b): return b''.join(b[i + 1:i + 2] + b[i:i + 1] for i in range(0, len(b), 2))


def c565(c): return (((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)


def decode(data, w, h, fmt):
    bw, bh = max(1, w // 4), max(1, h // 4)
    out = np.zeros((h, w, 4), dtype=np.uint8)
    for by in range(bh):
        for bx in range(bw):
            o = tiled(bx, by, bw, 4) * 16
            b = swap16(data[o:o + 16])
            c0, c1, idx = struct.unpack('<HHI', b[8:16])
            p = [c565(c0), c565(c1)]
            p += [tuple((2 * a + q) // 3 for a, q in zip(p[0], p[1])), tuple((a + 2 * q) // 3 for a, q in zip(p[0], p[1]))]
            if fmt == 0x191:
                a = int.from_bytes(b[:8], 'little'); al = [((a >> (4 * i)) & 15) * 17 for i in range(16)]
            else:
                a0, a1 = b[0], b[1]; alpha_bits = int.from_bytes(b[2:8], 'little')
                pal = [a0, a1] + ([((6 - i) * a0 + (i + 1) * a1) // 7 for i in range(6)] if a0 > a1 else
                                  [((4 - i) * a0 + (i + 1) * a1) // 5 for i in range(4)] + [0, 255])
                al = [pal[(alpha_bits >> (3 * i)) & 7] for i in range(16)]
            for i in range(16):
                x, y = bx * 4 + i % 4, by * 4 + i // 4
                if x < w and y < h: out[y, x] = p[(idx >> (2 * i)) & 3] + (al[i],)
    return out


def to565(c):
    r, g, b = [int(v) for v in c]
    return ((r * 31 + 127) // 255) << 11 | ((g * 63 + 127) // 255) << 5 | ((b * 31 + 127) // 255)


def from565(v):
    return np.array([((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31], dtype=float)


def color_block(px):
    rgb = px[:, :3].astype(float)
    wgt = px[:, 3] > 8
    pts = rgb[wgt] if wgt.any() else rgb
    mean = pts.mean(0)
    cov = np.cov((pts - mean).T) if len(pts) > 1 else np.eye(3)
    ev = np.linalg.eigh(cov)[1][:, -1]
    proj = (pts - mean) @ ev
    lo, hi = mean + ev * proj.min(), mean + ev * proj.max()
    c0, c1 = to565(np.clip(hi, 0, 255)), to565(np.clip(lo, 0, 255))
    if c0 < c1: c0, c1 = c1, c0
    p0, p1 = from565(c0), from565(c1)
    pal = [p0, p1, (2 * p0 + p1) / 3, (p0 + 2 * p1) / 3]
    idx = 0
    for i in range(16):
        d = [((rgb[i] - p) ** 2).sum() for p in pal]
        idx |= int(np.argmin(d)) << (2 * i)
    return struct.pack('<HHI', c0, c1, idx)


def dxt3_block(px):
    a = np.clip(np.round(px[:, 3] / 17), 0, 15).astype(int)
    alpha = 0
    for i in range(16): alpha |= int(a[i]) << (4 * i)
    return struct.pack('<Q', alpha) + color_block(px)


def dxt5_block(px):
    a = px[:, 3].astype(int)
    a0, a1 = int(a.max()), int(a.min())
    if a0 == a1: a1 = max(0, a0 - 1) if a0 > 0 else 0; a0 = max(a0, a1 + 1)
    pal = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
    bits = 0
    for i in range(16):
        k = min(range(8), key=lambda j: abs(pal[j] - a[i]))
        bits |= k << (3 * i)
    return bytes([a0, a1]) + bits.to_bytes(6, 'little') + color_block(px)


def write_reference(path, entries, pastes, pos, atlas):
    A = np.array(atlas)
    with open(path, 'wb') as f:
        f.write(b'SRG3' + struct.pack('<II', len(entries), len(CTX) + 1))
        for e, lst in zip(entries, pastes):
            tex, w, h, fmt = e['data'], e['w'], e['h'], e['fmt']
            orig = decode(tex, w, h, fmt)
            versions, blocks = [], set()
            for ci in range(len(CTX)):
                a = orig.copy()
                for c, x, y, rw, rh, key in lst:
                    if c != ci: continue
                    ax, ay = pos[key]
                    pic = A[ay:ay + rh, ax:ax + rw].copy()
                    if e['dark']: pic[:, :, :3] = 0
                    a[y:y + rh, x:x + rw] = pic
                    for by in range(y // 4, (y + rh - 1) // 4 + 1):
                        for bx in range(x // 4, (x + rw - 1) // 4 + 1): blocks.add((bx, by))
                versions.append(a)
            blocks = sorted(blocks)
            offs = [tiled(bx, by, w // 4, 4) * 16 for bx, by in blocks]
            data = [b''.join(tex[o:o + 16] for o in offs)]
            for a in versions:
                enc = b''
                for bx, by in blocks:
                    px16 = a[by * 4:by * 4 + 4, bx * 4:bx * 4 + 4].reshape(16, 4)
                    enc += swap16(dxt3_block(px16) if fmt == 0x191 else dxt5_block(px16))
                data.append(enc)
            size = max(offs) + 16 if e['small'] else len(tex)
            f.write(e['label'].encode().ljust(48, b'\0') + struct.pack('<III', size, len(offs), e['small']))
            f.write(tex[:4096]); f.write(struct.pack('<%dI' % len(offs), *offs))
            for dv in data: f.write(dv)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('packfiles', help='the game\'s packfiles folder (dist/game/packfiles)')
    ap.add_argument('--font', default=artmod.FONT)
    ap.add_argument('--out', default=HERE)
    ap.add_argument('--reference', help='also write kbm_ui.bin here (slow)')
    a = ap.parse_args()
    artmod.set_font(a.font)
    entries, pastes, pos, atlas = build(a.packfiles)
    atlas.save(os.path.join(a.out, 'art.png'), optimize=True)
    write_index(os.path.join(a.out, 'art.txt'), entries, pastes, pos)
    print('%d textures, %d pictures, art.png %dx%d' % (len(entries), len(pos), atlas.width, atlas.height))
    if a.reference:
        write_reference(a.reference, entries, pastes, pos, atlas)
        print('reference written to', a.reference)


if __name__ == '__main__':
    main()
