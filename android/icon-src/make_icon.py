#!/usr/bin/env python3
"""HearBridge Browser launcher icon: navy bg, glowing blue ring, headphones + waves, name text.
Writes the 1024/512 masters and every mipmap (legacy, round, adaptive fg/bg/monochrome)."""
import os, math
from PIL import Image, ImageDraw, ImageFilter, ImageFont, ImageChops

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, '..', 'app', 'src', 'main', 'res')
DOCS = os.path.join(HERE, '..', '..', 'docs')
S = 2048                      # supersampled canvas
C = S / 2
RING_R, RING_W = 0.372 * S, 0.024 * S
FONT = '/usr/share/fonts/truetype/sand-box/google/Inter/Inter-VariableFont_opsz,wght.ttf'

def font(px, weight):
    f = ImageFont.truetype(FONT, int(px))
    try:
        f.set_variation_by_axes([32, weight])   # opsz, wght
    except Exception:
        pass
    return f

def background():
    bg = Image.new('RGBA', (S, S), (5, 11, 30, 255))
    rad = Image.new('L', (S, S), 0); d = ImageDraw.Draw(rad)
    for i in range(80):
        r = S * 0.75 * (1 - i / 80)
        d.ellipse([C - r, C - r, C + r, C + r], fill=int(255 * (i / 80) ** 1.3))
    hi = Image.new('RGBA', (S, S), (16, 34, 78, 255))
    bg = Image.composite(hi, bg, rad.filter(ImageFilter.GaussianBlur(S * 0.03)))
    # faint grid like the other project icons
    g = Image.new('RGBA', (S, S), (0, 0, 0, 0)); gd = ImageDraw.Draw(g)
    step = S // 32
    for x in range(0, S, step):
        gd.line([x, 0, x, S], fill=(60, 110, 220, 14), width=2)
        gd.line([0, x, S, x], fill=(60, 110, 220, 14), width=2)
    bg.alpha_composite(g)
    return bg

def glow(layer, radius, strength=1.0):
    out = Image.new('RGBA', layer.size, (0, 0, 0, 0))
    for r, a in ((radius * 2.2, 0.55), (radius, 0.8), (radius * 0.35, 1.0)):
        b = layer.filter(ImageFilter.GaussianBlur(r))
        if strength != 1.0 or a != 1.0:
            al = b.getchannel('A').point(lambda v: min(255, int(v * a * strength)))
            b.putalpha(al)
        out.alpha_composite(b)
    return out

def glyph_layer(color):
    """Headphones + sound waves, line style. Returns RGBA."""
    L = Image.new('RGBA', (S, S), (0, 0, 0, 0)); d = ImageDraw.Draw(L)
    cy = C - S * 0.105          # glyph centre (above the text)
    w = int(S * 0.026)          # stroke
    br = S * 0.165              # headband radius
    # headband arc
    d.arc([C - br, cy - br, C + br, cy + br], 180, 360, fill=color, width=w)
    # side stems down to the cups
    for sx in (-1, 1):
        x = C + sx * br
        d.line([x, cy, x, cy + S * 0.04], fill=color, width=w)
        d.ellipse([x - w / 2, cy - w / 2, x + w / 2, cy + w / 2], fill=color)
        # ear cup (rounded rect, filled)
        cw, ch = S * 0.062, S * 0.135
        x0 = x - cw / 2 if sx < 0 else x - cw / 2
        d.rounded_rectangle([x0, cy + S * 0.005, x0 + cw, cy + S * 0.005 + ch], radius=cw * 0.45, fill=color)
    # sound bars between the cups
    hs = [0.30, 0.62, 1.0, 0.62, 0.30]
    bw, gap, H = S * 0.021, S * 0.020, S * 0.14
    tot = len(hs) * bw + (len(hs) - 1) * gap
    x = C - tot / 2; my = cy + S * 0.072
    for h in hs:
        hh = H * h
        d.rounded_rectangle([x, my - hh / 2, x + bw, my + hh / 2], radius=bw / 2, fill=color)
        x += bw + gap
    # outer wave arcs ")) (("
    for k, rr in enumerate((S * 0.235, S * 0.275)):
        ww = int(S * 0.016)
        box = [C - rr, my - rr, C + rr, my + rr]
        span = 22 - k * 4
        d.arc(box, -span, span, fill=color, width=ww)
        d.arc(box, 180 - span, 180 + span, fill=color, width=ww)
    return L

def text_layer(main_col, sub_col):
    L = Image.new('RGBA', (S, S), (0, 0, 0, 0)); d = ImageDraw.Draw(L)
    f1 = font(S * 0.098, 800)
    t = 'HearBridge'
    bb = d.textbbox((0, 0), t, font=f1)
    tw = bb[2] - bb[0]
    y1 = C + S * 0.095
    d.text((C - tw / 2 - bb[0], y1 - bb[1]), t, font=f1, fill=main_col)
    f2 = font(S * 0.046, 700)
    t2 = 'BROWSER'; track = S * 0.014
    widths = [d.textbbox((0, 0), ch, font=f2)[2] for ch in t2]
    tot = sum(widths) + track * (len(t2) - 1)
    x = C - tot / 2; y2 = y1 + (bb[3] - bb[1]) + S * 0.028
    for ch, cw in zip(t2, widths):
        d.text((x, y2), ch, font=f2, fill=sub_col); x += cw + track
    return L

def ring_layer():
    R = Image.new('RGBA', (S, S), (0, 0, 0, 0)); d = ImageDraw.Draw(R)
    r = RING_R
    d.ellipse([C - r, C - r, C + r, C + r], outline=(48, 132, 255, 255), width=int(RING_W))
    core = Image.new('RGBA', (S, S), (0, 0, 0, 0)); cd = ImageDraw.Draw(core)
    cd.ellipse([C - r + RING_W * 0.3, C - r + RING_W * 0.3, C + r - RING_W * 0.3, C + r - RING_W * 0.3],
               outline=(150, 205, 255, 255), width=int(RING_W * 0.35))
    out = glow(R, S * 0.022, 1.0)
    out.alpha_composite(R); out.alpha_composite(core)
    return out

def content():
    """Everything except the background (for the adaptive foreground)."""
    out = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    # dark disc inside the ring so text stays crisp on any wallpaper/bg
    disc = Image.new('RGBA', (S, S), (0, 0, 0, 0)); dd = ImageDraw.Draw(disc)
    r = RING_R - RING_W / 2
    dd.ellipse([C - r, C - r, C + r, C + r], fill=(6, 14, 38, 235))
    out.alpha_composite(disc)
    out.alpha_composite(ring_layer())
    g = glyph_layer((225, 240, 255, 255))
    out.alpha_composite(glow(glyph_layer((70, 150, 255, 255)), S * 0.010, 0.9))
    out.alpha_composite(g)
    t = text_layer((255, 255, 255, 255), (90, 165, 255, 255))
    out.alpha_composite(glow(text_layer((40, 110, 255, 255), (40, 110, 255, 255)), S * 0.006, 0.5))
    out.alpha_composite(t)
    return out

def mono():
    out = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    R = Image.new('RGBA', (S, S), (0, 0, 0, 0)); d = ImageDraw.Draw(R)
    d.ellipse([C - RING_R, C - RING_R, C + RING_R, C + RING_R], outline=(255, 255, 255, 255), width=int(RING_W))
    out.alpha_composite(R)
    out.alpha_composite(glyph_layer((255, 255, 255, 255)))
    out.alpha_composite(text_layer((255, 255, 255, 255), (255, 255, 255, 255)))
    return out

BG, FG, MONO = background(), content(), mono()
full = BG.copy(); full.alpha_composite(FG)

def shaped(img, shape, size):
    m = Image.new('L', (S, S), 0); d = ImageDraw.Draw(m)
    if shape == 'round': d.ellipse([0, 0, S - 1, S - 1], fill=255)
    else: d.rounded_rectangle([0, 0, S - 1, S - 1], radius=S * 0.18, fill=255)
    o = Image.new('RGBA', (S, S), (0, 0, 0, 0)); o.paste(img, (0, 0), m)
    return o.resize((size, size), Image.LANCZOS)

def adaptive(img, px):
    """Place the ring (outer edge + glow) inside the 66dp safe zone of the 108dp canvas."""
    ring_outer = 2 * (RING_R + RING_W / 2) * 1.04
    scale = (66 / 108) / (ring_outer / S)
    c = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    sm = img.resize((int(S * scale), int(S * scale)), Image.LANCZOS)
    o = (S - sm.size[0]) // 2
    c.alpha_composite(sm, (o, o))
    return c.resize((px, px), Image.LANCZOS)

os.makedirs(DOCS, exist_ok=True)
full.resize((1024, 1024), Image.LANCZOS).save(os.path.join(DOCS, 'icon-1024.png'), optimize=True)
full.resize((512, 512), Image.LANCZOS).save(os.path.join(HERE, 'icon-512.png'), optimize=True)
for k, f in {'mdpi': 1, 'hdpi': 1.5, 'xhdpi': 2, 'xxhdpi': 3, 'xxxhdpi': 4}.items():
    d = os.path.join(RES, 'mipmap-' + k); os.makedirs(d, exist_ok=True)
    shaped(full, 'square', int(48 * f)).save(os.path.join(d, 'ic_launcher.png'), optimize=True)
    shaped(full, 'round', int(48 * f)).save(os.path.join(d, 'ic_launcher_round.png'), optimize=True)
    adaptive(FG, int(108 * f)).save(os.path.join(d, 'ic_launcher_foreground.png'), optimize=True)
    BG.resize((int(108 * f),) * 2, Image.LANCZOS).save(os.path.join(d, 'ic_launcher_background.png'), optimize=True)
    adaptive(MONO, int(108 * f)).save(os.path.join(d, 'ic_launcher_monochrome.png'), optimize=True)
print('ok')
