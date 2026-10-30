# Translated texture labels: takes the HD replacement of a texture (or the original, scaled up),
# removes English labels and draws German ones in a similar style. Output goes to
# lang/de_tex/<key>.png, which the renderer applies even with the HD textures switched off.
#   python make_de_tex.py <texture pack dir> <original textures dir>... <out dir> [--preview file.png] [--lang fr]
# Original textures: work/tex_src (static extraction) or a texdump folder (runtime keys).
# Coordinates in SPECS are original texture pixels; the image is worked on at the pack's scale.
import os, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter

FONTS = 'C:/Windows/Fonts/'


def lum(a):
    return a[..., 0] + a[..., 1] + a[..., 2]


# background pixels (kept) for the 'plate' and 'inpaint' modes
KEEP = {
    'blue': lambda a: (a[..., 2] > a[..., 0] + 25) & (a[..., 3] > 200) & (lum(a) > 150),
    'red': lambda a: (a[..., 0] > a[..., 2] + 60) & (a[..., 3] > 200),
    'translucent': lambda a: a[..., 3] < 200,
    'light': lambda a: lum(a) > 520,
    'black': lambda a: (lum(a) < 60) & (a[..., 3] > 200),
}

YELLOW = ((255, 242, 136), (236, 152, 64))
PLATE = dict(fill=YELLOW, line=((20, 20, 22), 1))

SPECS = {
    # sys_parts00 (GR3.MDT): help window and status screen
    '4de6c1d475a7416b': [
        dict(bg='plate', axis='x', keep='blue', box=(3, 2, 58, 15), body=(1, 14), text='Typ', x=4, cap=(2, 11), **PLATE),
        dict(bg='plate', axis='x', keep='blue', box=(3, 20, 58, 33), body=(19, 32), text='Bereich', x=4, cap=(20, 29), w=52, **PLATE),
        dict(bg='plate', axis='x', keep='blue', box=(3, 38, 58, 51), body=(37, 50), text='Ziel', x=4, cap=(38, 47), **PLATE),
        dict(bg='clear', box=(67, 36, 110, 48), text='HILFE', x=67, cap=(36, 47), w=43,
             fill=((141, 254, 240), (111, 198, 188)), line=((7, 8, 9), 1.25)),
        dict(bg='clear', box=(0, 110, 49, 128), text='GESAMT', x=1, cap=(112, 125), w=47, font='bahnschrift.ttf', var=b'Bold',
             fill=((17, 76, 98), (29, 87, 107)), line=((198, 226, 231), 1.5)),
        dict(bg='clear', box=(49, 110, 89, 128), text='NOCH', x=50, cap=(112, 125), w=37, font='bahnschrift.ttf', var=b'Bold',
             fill=((17, 76, 98), (29, 87, 107)), line=((198, 226, 231), 1.5)),
        dict(bg='clear', box=(89, 110, 113, 128), text='EP', x=91, cap=(111, 125), w=21,
             fill=((115, 0, 0), (164, 66, 28)), line=((251, 231, 114), 1)),
    ],
    # bt_parts00 (GR3.MDT): battle
    '6a87dc3c5e1fd296': [
        dict(bg='clear', box=(113, 29, 173, 46), text='AUSWEICHEN', x=114, cap=(30, 44), w=58, font='arialbi.ttf',
             fill=((120, 230, 250), (20, 150, 190)), line=((250, 250, 255), 1), shadow=(1, 1, (20, 20, 40))),
        dict(bg='clear', box=(76, 46, 112, 58), text='Gesamt', x=77, cap=(47, 55), w=34,
             fill=((150, 20, 20), (110, 0, 0)), line=((245, 235, 235), 1)),
        dict(bg='clear', box=(117, 46, 173, 58), text='Schaden', x=118, cap=(47, 55), w=54,
             fill=((150, 20, 20), (110, 0, 0)), line=((245, 235, 235), 1)),
        dict(bg='clear', box=(0, 55, 45, 68), text='Höhe', x=1, cap=(56, 64), w=43,
             fill=((250, 250, 255), (205, 210, 225)), line=((20, 20, 40), 1)),
        dict(bg='clear', box=(68, 58, 124, 70), text='Schaden', x=69, cap=(59, 67), w=54,
             fill=((20, 120, 130), (10, 90, 100)), line=((240, 245, 245), 1)),
        dict(bg='clear', box=(125, 58, 163, 70), text='Anteil', x=126, cap=(59, 67), w=36,
             fill=((20, 120, 130), (10, 90, 100)), line=((240, 245, 245), 1)),
        dict(bg='plate', axis='y', keep='translucent', box=(4, 86, 82, 96), text='NÄCHSTE AKTION', x=5, cap=(87, 95), w=76,
             fill=((190, 95, 75), (235, 120, 120)), line=((0, 0, 0), 1)),
        dict(bg='clear', box=(67, 113, 139, 135), text='Abwehr', x=70, cap=(116, 131), w=66, font='arialbi.ttf',
             fill=((30, 40, 170), (10, 20, 120)), glow=((255, 255, 255), 2)),
        dict(bg='clear', box=(117, 135, 185, 156), text='Angriff', x=120, cap=(138, 153), w=62, font='arialbi.ttf',
             fill=((160, 40, 15), (120, 20, 5)), glow=((255, 255, 255), 2)),
        dict(bg='clear', box=(118, 157, 177, 179), text='Tempo', x=121, cap=(160, 175), w=53, font='arialbi.ttf',
             fill=((130, 130, 20), (90, 90, 0)), glow=((255, 255, 255), 2)),
        dict(bg='clear', box=(201, 128, 254, 147), text='NORMAL', x=203, cap=(131, 144), w=49, font='arialbi.ttf',
             fill=((255, 230, 120), (240, 170, 50)), glow=((140, 60, 0), 2)),
        dict(bg='clear', box=(221, 147, 255, 166), text='HOCH', x=223, cap=(150, 163), w=31, font='arialbi.ttf',
             fill=((150, 240, 255), (60, 200, 235)), glow=((10, 40, 120), 2)),
        dict(bg='clear', box=(189, 166, 255, 185), text='RUNTER', x=191, cap=(169, 182), w=63, font='arialbi.ttf',
             fill=((250, 100, 120), (220, 40, 70)), glow=((100, 0, 20), 2)),
        dict(bg='clear', box=(1, 138, 115, 174), text='Abbruch', x=3, cap=(140, 172), w=110, font='impact.ttf',
             fill=((255, 255, 255), (235, 235, 240)), first=(170, 200, 250)),
        dict(bg='holes', box=(1, 176, 115, 212), hole=(250, 250, 250), text='Abbruch', x=7, cap=(180, 208), w=103, font='impact.ttf'),
        dict(bg='inpaint', keep='black', box=(118, 181, 187, 191), text='Neuer Rekord!', x=119, cap=(181, 190), w=67,
             fill=((255, 200, 120), (240, 120, 50)), line=((40, 10, 0), 1)),
        dict(bg='clear', box=(116, 192, 190, 210), text='KONTER', x=117, cap=(193, 208), w=71, font='impact.ttf',
             fill=((250, 222, 232), (240, 200, 215)), first=(220, 40, 60)),
        dict(bg='clear', box=(121, 211, 176, 229), text='LUFT', x=122, cap=(212, 227), w=53, font='impact.ttf',
             fill=((205, 240, 252), (180, 225, 245)), first=(40, 130, 220)),
    ],
    # bt_parts_result00 (P1WIN.DAT): battle results
    '9070334be3aa17c5': [
        dict(bg='clear', box=(200, 0, 241, 18), text='NOCH', x=201, cap=(2, 16), w=38, font='bahnschrift.ttf', var=b'Bold',
             fill=((17, 76, 98), (29, 87, 107)), line=((198, 226, 231), 1.5)),
        dict(bg='clear', box=(241, 0, 264, 18), text='EP', x=243, cap=(2, 16), w=20,
             fill=((115, 0, 0), (164, 66, 28)), line=((251, 231, 114), 1)),
        dict(bg='plate', axis='y', keep='red', box=(424, 18, 462, 28), font='arialbi.ttf', text='ANG', x=427, cap=(18, 27), w=33,
             fill=((235, 245, 250), (215, 228, 240)), line=((8, 1, 99), 1)),
        dict(bg='plate', axis='y', keep='red', box=(470, 18, 508, 28), font='arialbi.ttf', text='VER', x=473, cap=(18, 27), w=33,
             fill=((235, 245, 250), (215, 228, 240)), line=((8, 1, 99), 1)),
        dict(bg='plate', axis='y', keep='red', box=(470, 33, 508, 43), font='arialbi.ttf', text='WID', x=473, cap=(33, 42), w=33,
             fill=((235, 245, 250), (215, 228, 240)), line=((8, 1, 99), 1)),
        dict(bg='clear', box=(445, 46, 512, 63), text='EP erhalten', x=452, cap=(47, 59), w=58, font='bahnschrift.ttf', var=b'Bold Condensed',
             fill=((250, 250, 255), (220, 225, 240)), line=((40, 40, 90), 1)),
        dict(bg='clear', box=(428, 67, 512, 87), text='Geld erhalten', x=431, cap=(69, 82), w=79, font='bahnschrift.ttf', var=b'Bold Condensed',
             fill=((250, 250, 255), (220, 225, 240)), line=((40, 40, 90), 1)),
        dict(bg='clear', box=(437, 88, 512, 108), text='Geld gesamt', x=440, cap=(90, 103), w=70, font='bahnschrift.ttf', var=b'Bold Condensed',
             fill=((250, 250, 255), (220, 225, 240)), line=((40, 40, 90), 1)),
        # "Level Up!" (x 2-118) stays as in the original
        dict(bg='clear', box=(121, 70, 343, 108), text='Experten-Level-Up!', x=124, cap=(74, 101), w=216, font='arialbi.ttf',
             fill=((250, 90, 40), (200, 20, 10)), line=((255, 230, 210), 1)),
        dict(bg='inpaint', keep='light', box=(125, 110, 226, 127), clear_rows=(110, 120), text='Magiestufe', x=129, cap=(111, 124), w=95,
             font='bahnschrift.ttf', var=b'SemiBold', fill=((250, 250, 255), (230, 235, 245)), line=((40, 50, 90), 1)),
        dict(bg='inpaint', keep='light', box=(125, 128, 226, 145), clear_rows=(128, 138), text='Skill-Stufe', x=129, cap=(129, 142), w=95,
             font='bahnschrift.ttf', var=b'SemiBold', fill=((250, 250, 255), (230, 235, 245)), line=((40, 50, 90), 1)),
        dict(bg='inpaint', keep='light', box=(125, 146, 226, 163), clear_rows=(146, 156), text='Spezialstufe', x=129, cap=(147, 160), w=95,
             font='bahnschrift.ttf', var=b'SemiBold', fill=((250, 250, 255), (230, 235, 245)), line=((40, 50, 90), 1)),
        dict(bg='clear', box=(0, 174, 162, 206), text='ERGEBNIS', x=5, cap=(180, 205), w=152, font='georgiab.ttf',
             fill=((250, 245, 150), (200, 160, 40)), shadow=(3, 2, (80, 50, 20))),
    ],
    # sys_parts_ipmark00 (GR3.MDT): IP gauge marks
    '18086150807bb821': [
        dict(bg='clear', box=(0, 82, 34, 99), text='K.O.', x=2, cap=(83, 97), w=30,
             fill=((255, 190, 90), (240, 120, 30)), line=((60, 20, 0), 1)),
        dict(bg='clear', box=(0, 109, 94, 128), text='Übersicht', x=1, cap=(111, 123), w=91,
             fill=((150, 190, 250), (60, 110, 220)), line=((15, 20, 60), 1)),
    ],
    # parts_place (30000000.MDT): world map
    '5b59d8730a741ed9': [
        dict(bg='clear', box=(23, 8, 95, 21), text='DRÜCKEN', x=24, cap=(9, 19), w=68,
             fill=((235, 245, 250), (200, 220, 235)), line=((20, 40, 70), 1)),
    ],
    # press_start (00000200.MDT): title screen, drawn as "Press START button" on one line
    'b06517f8d3d6e93d': [
        dict(bg='clear', box=(0, 1, 64, 17), text='Taste', x=1, cap=(3, 14), w=41, font='timesbd.ttf',
             fill=((255, 255, 255), (235, 235, 235))),
        dict(bg='clear', box=(0, 33, 64, 50), text='drücken', x=2, cap=(35, 46), w=48, font='timesbd.ttf',
             fill=((255, 255, 255), (235, 235, 235))),
    ],
    # title menu (00000200.MDT, runtime key)
    '1717ef0abed60ade': [
        dict(bg='clear', box=(0, 0, 128, 16), text='Neues Spiel', x=2, cap=(2, 13), w=85, font='timesbd.ttf',
             fill=((255, 255, 255), (240, 240, 240))),
        dict(bg='clear', box=(0, 17, 128, 34), text='Fortsetzen', x=1, cap=(20, 31), w=69, font='timesbd.ttf',
             fill=((255, 255, 255), (240, 240, 240))),
        dict(bg='clear', box=(0, 35, 128, 54), text='Optionen', x=1, cap=(38, 49), w=59, font='timesbd.ttf',
             fill=((255, 255, 255), (240, 240, 240))),
    ],
}


# other languages: same layout, German label -> translated label (--lang <code>)
TEXTS = {
    'fr': {
        'Typ': 'Type', 'Bereich': 'Portée', 'Ziel': 'Cible', 'HILFE': 'AIDE', 'GESAMT': 'TOTAL', 'NOCH': 'SUIV.',
        'EP': 'EXP', 'AUSWEICHEN': 'ESQUIVE', 'Gesamt': 'Total', 'Schaden': 'Dégâts', 'Höhe': 'Hauteur',
        'Anteil': 'Part', 'NÄCHSTE AKTION': 'ACTION SUIV.', 'Abwehr': 'Défense', 'Angriff': 'Attaque',
        'Tempo': 'Vitesse', 'NORMAL': 'NORMAL', 'HOCH': 'HAUT', 'RUNTER': 'BAS', 'Abbruch': 'Annule',
        'Neuer Rekord!': 'Record !', 'KONTER': 'CONTRE', 'LUFT': 'AIR', 'ANG': 'ATQ', 'VER': 'DÉF', 'WID': 'RÉS',
        'EP erhalten': 'EXP gagnée', 'Geld erhalten': 'Or gagné', 'Geld gesamt': 'Or total',
        'Experten-Level-Up!': 'Niveau expert !', 'Magiestufe': 'Niv. magie', 'Skill-Stufe': 'Niv. compét.',
        'Spezialstufe': 'Niv. spécial', 'ERGEBNIS': 'RÉSULTAT', 'K.O.': 'K.O.', 'Übersicht': 'Vue du ciel',
        'DRÜCKEN': 'APPUYEZ', 'Taste': 'Appuyez', 'drücken': 'pour jouer', 'Neues Spiel': 'Nouvelle partie',
        'Fortsetzen': 'Continuer', 'Optionen': 'Options',
    },
}


def load_font(s, size):
    f = ImageFont.truetype(FONTS + s.get('font', 'ariblk.ttf'), size)
    if 'var' in s:
        f.set_variation_by_name(s['var'])
    return f


def fill_holes(cols):
    xs = [i for i, c in enumerate(cols) if c is not None]
    if not xs:
        return None
    out = []
    for i, c in enumerate(cols):
        if c is not None:
            out.append(c)
            continue
        l = max([j for j in xs if j < i], default=None)
        r = min([j for j in xs if j > i], default=None)
        if l is None: out.append(cols[r])
        elif r is None: out.append(cols[l])
        else: out.append(cols[l] + (cols[r] - cols[l]) * (i - l) / (r - l))
    return np.array(out)


def plate_rebuild(img, s, k):
    """replace the box by the plate color of its column (axis x) or row (axis y)"""
    a = np.array(img).astype(np.float32)
    x0, y0, x1, y1 = [v * k for v in s['box']]
    keep = KEEP[s['keep']]
    if s['axis'] == 'x':
        b0, b1 = [v * k for v in s.get('body', (s['box'][1], s['box'][3]))]
        cols = []
        for x in range(x0, x1):
            px = a[b0:b1, x]
            ok = keep(px)
            cols.append(np.median(px[ok], axis=0) if ok.sum() >= 2 * k else None)
        cols = fill_holes(cols)
        r = 2 * k  # medians of neighbouring columns differ a little
        sm = np.array([cols[max(0, i - r):i + r + 1].mean(axis=0) for i in range(len(cols))])
        for i, c in enumerate(sm):
            a[y0:y1, x0 + i] = c
    else:
        rows = []
        for y in range(y0, y1):
            px = a[y, x0:x1]
            ok = keep(px)
            rows.append(np.median(px[ok], axis=0) if ok.sum() >= k else None)
        rows = fill_holes(rows)
        for i, c in enumerate(rows):
            a[y0 + i, x0:x1] = c
    return Image.fromarray(a.clip(0, 255).astype(np.uint8), 'RGBA')


def inpaint(img, s, k):
    """clear_rows become transparent; elsewhere in the box non-background pixels are interpolated per row"""
    a = np.array(img).astype(np.float32)
    x0, y0, x1, y1 = [v * k for v in s['box']]
    keep = KEEP[s['keep']]
    if 'clear_rows' in s:
        r0, r1 = [v * k for v in s['clear_rows']]
        a[r0:r1, x0:x1] = 0
    for y in range(y0, y1):
        row = a[y, x0:x1]
        if row[:, 3].max() == 0:
            continue
        ok = keep(row)
        if ok.all() or not ok.any():
            continue
        idx = np.nonzero(ok)[0]
        for c in range(4):
            row[~ok, c] = np.interp(np.nonzero(~ok)[0], idx, row[idx, c])
        a[y, x0:x1] = row
    return Image.fromarray(a.clip(0, 255).astype(np.uint8), 'RGBA')


def render(s, k):
    """the label as an RGBA image, the x offset of its visible left edge, the y offset of the top of the
    capitals' outline and the letter mask"""
    cap0, cap1 = s['cap']
    caph = (cap1 - cap0 + 1) * k
    lc, lw = s.get('line', (None, 0))
    line = int(round(k * lw)) if lc else 0
    glow = s.get('glow')
    gr = int(round(k * glow[1])) if glow else 0
    size = 8
    while True:
        bb = load_font(s, size + 1).getbbox('H')
        if bb[3] - bb[1] + 2 * line > caph: break
        size += 1
    font = load_font(s, size)
    bb = font.getbbox('H')
    pad = line + 3 * gr + 4 * k
    text = s['text']
    tw = int(font.getlength(text)) + 2 * pad + size // 2
    th = size * 2 + 2 * pad
    oy = pad - bb[1]
    mask = Image.new('L', (tw, th), 0)
    ImageDraw.Draw(mask).text((pad, oy), text, font=font, fill=255)
    first = Image.new('L', (tw, th), 0)
    if 'first' in s:
        ImageDraw.Draw(first).text((pad, oy), text[0], font=font, fill=255)
    if line:
        stroke = Image.new('L', (tw, th), 0)
        ImageDraw.Draw(stroke).text((pad, oy), text, font=font, fill=255, stroke_width=line, stroke_fill=255)
    else:
        stroke = mask.copy()
    # fit the width
    l, t, r, b = stroke.getbbox()
    maxw = s.get('w', 0) * k
    sx = min(1.0, maxw / (r - l)) if maxw else 1.0
    nw = max(1, int(round(tw * sx)))
    mask, stroke, first = [m.resize((nw, th), Image.LANCZOS) for m in (mask, stroke, first)]
    left = int(l * sx)
    out = Image.new('RGBA', (nw, th), (0, 0, 0, 0))
    if glow:
        g = stroke.filter(ImageFilter.MaxFilter(2 * (gr // 2) + 1)).filter(ImageFilter.GaussianBlur(gr * 0.6))
        g = Image.eval(g, lambda v: min(255, int(v * 1.6)))
        gl = Image.new('RGBA', (nw, th), glow[0] + (0,))
        gl.putalpha(g)
        out.alpha_composite(gl)
    if 'shadow' in s:
        dx, dy, col = s['shadow']
        sh = Image.new('RGBA', (nw, th), col + (0,))
        sh.putalpha(stroke)
        out.alpha_composite(sh, (int(dx * k), int(dy * k)))
    if line:
        ol = Image.new('RGBA', (nw, th), lc + (0,))
        ol.putalpha(stroke)
        out.alpha_composite(ol)
    if 'fill' in s:
        top, bot = s['fill']
        grad = np.zeros((th, nw, 3), np.float32)
        for y in range(th):
            t = min(1.0, max(0.0, (y - pad) / max(1, caph - 2 * line)))
            grad[y] = np.array(top) * (1 - t) + np.array(bot) * t
        f = Image.fromarray(grad.astype(np.uint8), 'RGB').convert('RGBA')
        f.putalpha(mask)
        out.alpha_composite(f)
        if 'first' in s:
            fc = Image.new('RGBA', (nw, th), s['first'] + (0,))
            fc.putalpha(first)
            out.alpha_composite(fc)
    return out, left, pad - line, mask


def draw_label(img, s, k):
    out, left, top, mask = render(s, k)
    px = s['x'] * k - left
    py = s['cap'][0] * k - top
    x0, y0, x1, y1 = [v * k for v in s['box']]
    if s['bg'] == 'holes':
        # letters cut out of a filled box
        a = np.array(img)
        a[y0:y1, x0:x1] = s['hole'] + (255,)
        m = Image.new('L', img.size, 0)
        m.paste(mask, (px, py))
        a[..., 3] = np.minimum(a[..., 3], 255 - np.array(m))
        return Image.fromarray(a, 'RGBA')
    layer = Image.new('RGBA', img.size, (0, 0, 0, 0))
    layer.paste(out, (px, py))
    la = np.array(layer)
    keep = np.zeros(la.shape[:2], bool)
    keep[max(0, y0 - k):y1 + k, x0:x1] = True  # stay inside the label's box
    la[~keep] = 0
    img.alpha_composite(Image.fromarray(la, 'RGBA'))
    return img


def find(dirs, key):
    for d in dirs:
        p = os.path.join(d, key + '.png')
        if os.path.exists(p):
            return p
    return None


def main():
    args = sys.argv[1:]
    prev = None
    if '--preview' in args:
        i = args.index('--preview')
        prev = args[i + 1]
        del args[i:i + 2]
    texts = {}
    if '--lang' in args:
        i = args.index('--lang')
        texts = TEXTS[args[i + 1]]
        del args[i:i + 2]
    pack, srcs, out = args[0], args[1:-1], args[-1]
    only = os.environ.get('ONLY')
    os.makedirs(out, exist_ok=True)
    previews = []
    for key, specs in SPECS.items():
        if only and key not in only:
            continue
        sp = find(srcs, key)
        if not sp:
            print('no original for', key)
            continue
        orig = Image.open(sp).convert('RGBA')
        p = find([pack], key)
        img = Image.open(p).convert('RGBA') if p else orig.resize((orig.width * 4, orig.height * 4), Image.LANCZOS)
        k = img.width // orig.width
        before = img.copy()
        for s in specs:
            s = dict(s, text=texts.get(s['text'], s['text']))
            if s['bg'] == 'plate':
                img = plate_rebuild(img, s, k)
            elif s['bg'] == 'inpaint':
                img = inpaint(img, s, k)
            elif s['bg'] == 'clear':
                a = np.array(img)
                x0, y0, x1, y1 = [v * k for v in s['box']]
                a[y0:y1, x0:x1] = 0
                img = Image.fromarray(a, 'RGBA')
            img = draw_label(img, s, k)
        img.save(os.path.join(out, key + '.png'))
        print(key, img.size, len(specs), 'labels')
        previews.append((before, img))
    if prev:
        sc = 0.5
        W = sum(int(b.width * sc) * 2 + 8 for b, _ in previews)
        H = max(int(b.height * sc) for b, _ in previews)
        S = Image.new('RGBA', (W, H), (90, 60, 110, 255))
        x = 0
        for b, a in previews:
            for im in (b, a):
                t = im.resize((int(im.width * sc), int(im.height * sc)), Image.LANCZOS)
                S.alpha_composite(t, (x, 0))
                x += t.width + 4
        S.save(prev)


if __name__ == '__main__':
    main()
