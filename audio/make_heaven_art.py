"""Heaven theme art for SFShopExplorer.

Two assets, both matching the Heaven palette in ShopExplorer.cpp
(navy-black base, radiant gold, warm ivory):
  Chest_heaven.png   - basket/storage crate, gilded reliquary chest
  space_heaven.png   - window backdrop, celestial vault

Run:  python make_heaven_art.py
"""
import math
import os
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

OUT = r"B:\MO2\Starfield\mods\Shop Test Render\SFSE\Plugins\SFShopExplorerAssets"

NAVY = (11, 18, 38)
NAVY_DEEP = (5, 9, 20)
GOLD = (255, 232, 150)
GOLD_DEEP = (170, 132, 45)
IVORY = (245, 240, 225)


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def radial_field(h, w, cx, cy, radius, aspect=1.0):
    """Normalised 0..1 falloff from a point, aspect-corrected so it stays round."""
    y, x = np.mgrid[0:h, 0:w].astype(np.float64)
    dx = (x - cx * w) / (radius * w)
    dy = (y - cy * h) / (radius * w * aspect)
    d = np.sqrt(dx * dx + dy * dy)
    return np.clip(1.0 - d, 0.0, 1.0)


def value_noise(h, w, cells, octaves, seed, aspect=1.0):
    """Cheap fractal value noise via bilinear-upsampled random lattices."""
    rng = np.random.default_rng(seed)
    total = np.zeros((h, w), dtype=np.float64)
    amp = 1.0
    norm = 0.0
    c = cells
    for _ in range(octaves):
        lat = rng.random((c + 1, int(c * aspect) + 1))
        yi = np.linspace(0, c, h)
        xi = np.linspace(0, c * aspect, w)
        y0 = np.floor(yi).astype(int)
        x0 = np.floor(xi).astype(int)
        y1 = np.minimum(y0 + 1, c)
        x1 = np.minimum(x0 + 1, int(c * aspect))
        fy = (yi - y0)[:, None]
        fx = (xi - x0)[None, :]
        # smoothstep for smooth gradients
        fy = fy * fy * (3 - 2 * fy)
        fx = fx * fx * (3 - 2 * fx)
        a = lat[np.ix_(y0, x0)]
        b = lat[np.ix_(y0, x1)]
        cc = lat[np.ix_(y1, x0)]
        d = lat[np.ix_(y1, x1)]
        top = a + (b - a) * fx
        bot = cc + (d - cc) * fx
        total += (top + (bot - top) * fy) * amp
        norm += amp
        amp *= 0.5
        c *= 2
    return total / norm


def make_backdrop(w=1920, h=1080):
    """Celestial vault: deep navy, a warm halo, drifting stars, gold arc."""
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float64)

    # Base: navy vertical gradient, darker at the bottom. Done channel-wise
    # with numpy rather than lerp(), whose scalar int() cannot take an array.
    t = (yy / (h - 1))[:, :, None]
    base = np.zeros((h, w, 3), dtype=np.float64)
    for i in range(3):
        base[:, :, i] = NAVY_DEEP[i] + (NAVY[i] - NAVY_DEEP[i]) * t[:, :, 0]

    # Central warm halo behind the logo.
    halo = radial_field(h, w, 0.5, 0.46, 0.52, aspect=w / h) ** 2.1
    for i in range(3):
        base[:, :, i] += halo * (GOLD_DEEP[i] - 18) * 0.55

    # Two cool counter-glows for depth.
    g1 = radial_field(h, w, 0.18, 0.20, 0.40, aspect=w / h) ** 2.6
    g2 = radial_field(h, w, 0.86, 0.82, 0.44, aspect=w / h) ** 2.4
    base[:, :, 2] += g1 * 70 + g2 * 46
    base[:, :, 1] += g1 * 40 + g2 * 24

    # Nebula: two noise layers, gold-warm and violet, kept subtle.
    n1 = value_noise(h, w, 3, 5, seed=1337, aspect=w / h)
    n2 = value_noise(h, w, 5, 4, seed=4242, aspect=w / h)
    neb = np.clip((n1 - 0.45) * 2.2, 0, 1) * np.clip((n2 - 0.35) * 1.6, 0, 1)
    for i in range(3):
        base[:, :, i] += neb * (GOLD_DEEP[i] * 0.30)

    # A faint great arc: a broad ring centred below the window.
    cx, cy = w * 0.5, h * 1.55
    rad = np.sqrt((xx - cx) ** 2 + (yy - cy) ** 2)
    ring = np.exp(-((rad - h * 1.15) ** 2) / (2 * (h * 0.045) ** 2)) * 0.55
    ring *= np.clip((yy / h) * 1.4, 0, 1)
    for i in range(3):
        base[:, :, i] += ring * (GOLD[i] * 0.34)

    # Stars: sparse, brighter ones get a soft bloom.
    rng = np.random.default_rng(90210)
    img = Image.fromarray(np.clip(base, 0, 255).astype(np.uint8), "RGB")
    star = Image.new("L", (w, h), 0)
    sd = ImageDraw.Draw(star)
    count = 520
    for _ in range(count):
        sx = rng.uniform(0, w)
        sy = rng.uniform(0, h)
        r = rng.choice([0.6, 0.8, 1.0, 1.3, 1.7], p=[0.34, 0.28, 0.20, 0.13, 0.05])
        v = int(rng.uniform(90, 255))
        sd.ellipse([sx - r, sy - r, sx + r, sy + r], fill=v)
    bloom = star.filter(ImageFilter.GaussianBlur(2.2))
    glow = np.asarray(star.filter(ImageFilter.GaussianBlur(5.0)), dtype=np.float64) / 255.0

    arr = np.asarray(img, dtype=np.float64)
    sa = np.asarray(star, dtype=np.float64) / 255.0
    warm = star.filter(ImageFilter.GaussianBlur(1.1))
    wa = np.asarray(warm, dtype=np.float64) / 255.0
    for i in range(3):
        arr[:, :, i] += sa * 235 + glow * (GOLD_DEEP[i] * 0.55) + wa * (GOLD[i] * 0.10)
    arr = np.clip(arr, 0, 255).astype(np.uint8)
    out = Image.fromarray(arr, "RGB")

    # Vignette so the UI stays legible over it.
    vig = radial_field(h, w, 0.5, 0.5, 0.78, aspect=w / h) ** 0.9
    arr = np.asarray(out, dtype=np.float64) * (0.42 + 0.58 * vig[:, :, None])
    Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8), "RGB").save(os.path.join(OUT, "space_heaven.png"))
    print("space_heaven.png", w, "x", h)


def make_chest(size=1024):
    """Gilded reliquary chest: the basket/storage icon, Heaven treatment."""
    S = size
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    cx = S * 0.5
    body_top = S * 0.36
    body_bot = S * 0.84
    body_l = S * 0.13
    body_r = S * 0.87
    lid_h = S * 0.20

    # Soft ground shadow.
    sh = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(sh).ellipse([cx - S * 0.40, body_bot - S * 0.05, cx + S * 0.40, body_bot + S * 0.09],
                               fill=(0, 0, 0, 150))
    img.alpha_composite(sh.filter(ImageFilter.GaussianBlur(S * 0.022)))

    # Lid: domed, with a gold rim.
    d.pieslice([body_l, body_top - lid_h, body_r, body_top + lid_h], 180, 360,
               fill=lerp(NAVY, (24, 36, 66), 0.5) + (255,))
    d.rectangle([body_l, body_top, body_r, body_top + lid_h * 0.42],
                fill=lerp(NAVY, (24, 36, 66), 0.5) + (255,))
    d.arc([body_l, body_top - lid_h, body_r, body_top + lid_h], 180, 360,
          fill=GOLD + (255,), width=int(S * 0.014))
    d.line([body_l, body_top, body_r, body_top], fill=GOLD + (255,), width=int(S * 0.012))

    # Body.
    d.rectangle([body_l, body_top, body_r, body_bot], fill=lerp(NAVY_DEEP, NAVY, 0.55) + (255,))

    # Gold banding.
    band = int(S * 0.018)
    d.rectangle([body_l, body_top + lid_h * 0.30, body_r, body_top + lid_h * 0.30 + band], fill=GOLD_DEEP + (255,))
    d.rectangle([body_l, body_bot - band * 2.4, body_r, body_bot - band * 2.4 + band], fill=GOLD_DEEP + (255,))
    for x in (body_l, body_r - band * 0.9):
        d.rectangle([x, body_top, x + band * 0.9, body_bot], fill=GOLD_DEEP + (255,))

    # Rivets.
    for i in range(7):
        rx = body_l + (body_r - body_l) * (0.08 + 0.14 * i)
        d.ellipse([rx - S * 0.008, body_top + S * 0.30, rx + S * 0.008, body_top + S * 0.316],
                  fill=GOLD + (230,))
        d.ellipse([rx - S * 0.008, body_bot - S * 0.075, rx + S * 0.008, body_bot - S * 0.059],
                  fill=GOLD + (200,))

    # Latch + halo medallion.
    lx, ly = cx, body_top + lid_h * 0.30 + band * 0.5
    d.rounded_rectangle([lx - S * 0.048, ly - S * 0.030, lx + S * 0.048, ly + S * 0.052],
                        radius=S * 0.012, fill=GOLD + (255,))
    d.ellipse([lx - S * 0.022, ly - S * 0.006, lx + S * 0.022, ly + S * 0.038],
              fill=lerp(NAVY, (30, 44, 78), 0.6) + (255,), outline=IVORY + (220,), width=int(S * 0.004))

    # Radiant halo behind the medallion (drawn on its own layer then blurred).
    halo = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(halo).ellipse([lx - S * 0.20, ly - S * 0.20, lx + S * 0.20, ly + S * 0.20],
                                 fill=GOLD + (70,))
    halo = halo.filter(ImageFilter.GaussianBlur(S * 0.055))
    glow = img.copy()
    glow.alpha_composite(halo)
    img.alpha_composite(Image.blend(img, glow, 0.85).filter(ImageFilter.GaussianBlur(S * 0.0015)))

    # Star sparkles on the lid.
    sp = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    sd = ImageDraw.Draw(sp)
    rng = np.random.default_rng(777)
    for _ in range(9):
        ang = rng.uniform(0, math.pi * 2)
        rad = rng.uniform(0.15, 0.42) * S
        px = cx + math.cos(ang) * rad * 1.25
        py = body_top + math.sin(ang) * rad * 0.42 - S * 0.02
        r = S * rng.uniform(0.008, 0.017)
        sd.line([px - r, py, px + r, py], fill=IVORY + (235,), width=int(S * 0.0035))
        sd.line([px, py - r, px, py + r], fill=IVORY + (235,), width=int(S * 0.0035))
    img.alpha_composite(sp.filter(ImageFilter.GaussianBlur(S * 0.0012)))

    # Warm glow bloom around the whole chest.
    b = img.filter(ImageFilter.GaussianBlur(S * 0.012))
    final = Image.alpha_composite(b, img)

    # Key colour: an inner-glow pass so the gold reads as lit.
    arr = np.asarray(final, dtype=np.float64)
    lum = (arr[:, :, 0] * 0.30 + arr[:, :, 1] * 0.59 + arr[:, :, 2] * 0.11)
    # Keep a trailing axis: broadcasting into a (h, w) slice needs (h, w, 1).
    warm = np.clip((lum - 90) / 165.0, 0, 1)[:, :, None]
    # Index the trailing axis: adding an (h, w, 1) array to an (h, w) slice would
    # broadcast across a third dimension instead of scaling.
    warm = warm[:, :, 0]
    arr[:, :, 0] = np.clip(arr[:, :, 0] + warm * 26, 0, 255)
    arr[:, :, 1] = np.clip(arr[:, :, 1] + warm * 18, 0, 255)
    arr[:, :, 2] = np.clip(arr[:, :, 2] + warm * 4, 0, 255)
    Image.fromarray(arr.astype(np.uint8), "RGBA").save(os.path.join(OUT, "Chest_heaven.png"))
    print("Chest_heaven.png", S, "x", S)


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    make_backdrop()
    make_chest()