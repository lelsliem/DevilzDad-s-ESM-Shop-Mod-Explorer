"""Per-theme art for SFShopExplorer (Heaven done, plus Hell and Starfield).

Imports the drawing routines from make_heaven_art.py and drives them with a
palette per theme. Output names follow the loader's convention:
    Chest_<suffix>.png   space_<suffix>.png

Run:  python make_theme_art.py
"""
import importlib.util
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = r"B:\MO2\Starfield\mods\Shop Test Render\SFSE\Plugins\SFShopExplorerAssets"

spec = importlib.util.spec_from_file_location("mha", os.path.join(HERE, "make_heaven_art.py"))
mha = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mha)

# Each palette mirrors the matching UiTheme entry in ShopExplorer.cpp so the
# art and the chrome agree.
THEMES = {
    "heaven": dict(
        base=(11, 18, 38), deep=(5, 9, 20),
        accent=(255, 232, 150), accent2=(170, 132, 45), ink=(245, 240, 225),
        halo_xy=(0.50, 0.46), halo_r=0.52, halo_pow=2.1, halo_gain=0.55,
        glow1=(0.18, 0.20, 0.40), glow2=(0.86, 0.82, 0.44),
        nebula_gain=0.30, nebula_seed=(1337, 4242),
        arc=1.15, arc_gain=0.34, ring_gold=True,
        stars=520, bloom_warm=0.55,
        starfield_pow=2.6,
    ),
    "hell": dict(
        base=(26, 8, 5), deep=(10, 3, 2),
        accent=(255, 140, 40), accent2=(168, 44, 10), ink=(255, 232, 210),
        halo_xy=(0.50, 0.58), halo_r=0.60, halo_pow=1.8, halo_gain=0.62,
        glow1=(0.14, 0.24, 0.30), glow2=(0.88, 0.72, 0.30),
        nebula_gain=0.34, nebula_seed=(6060, 1313),
        arc=0.92, arc_gain=0.40, ring_gold=False,
        stars=300, bloom_warm=0.75,
        starfield_pow=2.9,
    ),
    "starfield": dict(
        base=(17, 9, 26), deep=(7, 4, 12),
        accent=(214, 150, 255), accent2=(120, 70, 200), ink=(240, 232, 255),
        halo_xy=(0.38, 0.34), halo_r=0.50, halo_pow=2.0, halo_gain=0.50,
        glow1=(0.20, 0.62, 0.80), glow2=(0.86, 0.30, 0.70),
        nebula_gain=0.38, nebula_seed=(24601, 80085),
        arc=1.30, arc_gain=0.30, ring_gold=False,
        stars=900, bloom_warm=0.35,
        starfield_pow=2.2,
    ),
}


def paint_backdrop(pal, w=1920, h=1080):
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float64)
    base = np.zeros((h, w, 3), dtype=np.float64)
    # yy is (h, w), so the gradient ramp is 1-D per row.
    t = (yy[:, 0] / (h - 1))[:, None]
    for i in range(3):
        base[:, :, i] = pal["deep"][i] + (pal["base"][i] - pal["deep"][i]) * t

    halo = mha.radial_field(h, w, pal["halo_xy"][0], pal["halo_xy"][1], pal["halo_r"], aspect=w / h) ** pal["halo_pow"]
    for i in range(3):
        base[:, :, i] += halo * (pal["accent2"][i] - 18) * pal["halo_gain"]

    g1 = mha.radial_field(h, w, *pal["glow1"], aspect=w / h) ** pal["starfield_pow"]
    g2 = mha.radial_field(h, w, *pal["glow2"], aspect=w / h) ** pal["starfield_pow"]
    base[:, :, 2] += g1 * 70 + g2 * 46
    base[:, :, 1] += g1 * 40 + g2 * 24

    n1 = mha.value_noise(h, w, 3, 5, seed=pal["nebula_seed"][0], aspect=w / h)
    n2 = mha.value_noise(h, w, 5, 4, seed=pal["nebula_seed"][1], aspect=w / h)
    neb = np.clip((n1 - 0.45) * 2.2, 0, 1) * np.clip((n2 - 0.35) * 1.6, 0, 1)
    for i in range(3):
        base[:, :, i] += neb * (pal["accent2"][i] * pal["nebula_gain"])

    cx, cy = w * 0.5, h * 1.55
    rad = np.sqrt((xx - cx) ** 2 + (yy - cy) ** 2)
    ring = np.exp(-((rad - h * pal["arc"]) ** 2) / (2 * (h * 0.045) ** 2)) * 0.55
    ring *= np.clip((yy / h) * 1.4, 0, 1)
    ring_col = pal["accent"] if pal["ring_gold"] else pal["accent2"]
    for i in range(3):
        base[:, :, i] += ring * (ring_col[i] * pal["arc_gain"])

    rng = np.random.default_rng(90210)
    img = Image.fromarray(np.clip(base, 0, 255).astype(np.uint8), "RGB")
    star = Image.new("L", (w, h), 0)
    sd = ImageDraw.Draw(star)
    for _ in range(pal["stars"]):
        sx, sy = rng.uniform(0, w), rng.uniform(0, h)
        r = rng.choice([0.6, 0.8, 1.0, 1.3, 1.7], p=[0.34, 0.28, 0.20, 0.13, 0.05])
        sd.ellipse([sx - r, sy - r, sx + r, sy + r], fill=int(rng.uniform(90, 255)))
    bloom = star.filter(ImageFilter.GaussianBlur(2.2))
    glow = np.asarray(star.filter(ImageFilter.GaussianBlur(5.0)), dtype=np.float64) / 255.0
    warm = star.filter(ImageFilter.GaussianBlur(1.1))
    wa = np.asarray(warm, dtype=np.float64) / 255.0
    arr = np.asarray(img, dtype=np.float64)
    sa = np.asarray(star, dtype=np.float64) / 255.0
    for i in range(3):
        arr[:, :, i] += sa * 235 + glow * (pal["accent2"][i] * pal["bloom_warm"]) + wa * (pal["accent"][i] * 0.10)
    out = Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8), "RGB")

    vig = mha.radial_field(h, w, 0.5, 0.5, 0.78, aspect=w / h) ** 0.9
    arr = np.asarray(out, dtype=np.float64) * (0.42 + 0.58 * vig[:, :, None])
    return Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8), "RGB")


def paint_chest(pal, size=1024):
    S = size
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx = S * 0.5
    body_top, body_bot = S * 0.36, S * 0.84
    body_l, body_r = S * 0.13, S * 0.87
    lid_h = S * 0.20
    accent, accent2, ink = pal["accent"], pal["accent2"], pal["ink"]
    lid_col = tuple(int((pal["base"][i] + 34) / 2) for i in range(3))
    body_col = tuple(int((pal["deep"][i] * 2 + pal["base"][i]) / 3) for i in range(3))

    sh = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(sh).ellipse([cx - S * 0.40, body_bot - S * 0.05, cx + S * 0.40, body_bot + S * 0.09],
                               fill=(0, 0, 0, 150))
    img.alpha_composite(sh.filter(ImageFilter.GaussianBlur(S * 0.022)))

    d.pieslice([body_l, body_top - lid_h, body_r, body_top + lid_h], 180, 360, fill=lid_col + (255,))
    d.rectangle([body_l, body_top, body_r, body_top + lid_h * 0.42], fill=lid_col + (255,))
    d.arc([body_l, body_top - lid_h, body_r, body_top + lid_h], 180, 360, fill=accent + (255,), width=int(S * 0.014))
    d.line([body_l, body_top, body_r, body_top], fill=accent + (255,), width=int(S * 0.012))
    d.rectangle([body_l, body_top, body_r, body_bot], fill=body_col + (255,))

    band = int(S * 0.018)
    d.rectangle([body_l, body_top + lid_h * 0.30, body_r, body_top + lid_h * 0.30 + band], fill=accent2 + (255,))
    d.rectangle([body_l, body_bot - band * 2.4, body_r, body_bot - band * 2.4 + band], fill=accent2 + (255,))
    for x in (body_l, body_r - band * 0.9):
        d.rectangle([x, body_top, x + band * 0.9, body_bot], fill=accent2 + (255,))

    for i in range(7):
        rx = body_l + (body_r - body_l) * (0.08 + 0.14 * i)
        d.ellipse([rx - S * 0.008, body_top + S * 0.30, rx + S * 0.008, body_top + S * 0.316], fill=accent + (230,))
        d.ellipse([rx - S * 0.008, body_bot - S * 0.075, rx + S * 0.008, body_bot - S * 0.059], fill=accent + (200,))

    lx, ly = cx, body_top + lid_h * 0.30 + band * 0.5
    d.rounded_rectangle([lx - S * 0.048, ly - S * 0.030, lx + S * 0.048, ly + S * 0.052],
                        radius=S * 0.012, fill=accent + (255,))
    d.ellipse([lx - S * 0.022, ly - S * 0.006, lx + S * 0.022, ly + S * 0.038],
              fill=tuple(int(c * 0.5 + pal["base"][i] * 0.5) for i, c in enumerate(body_col)) + (255,),
              outline=ink + (220,), width=int(S * 0.004))

    halo = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(halo).ellipse([lx - S * 0.20, ly - S * 0.20, lx + S * 0.20, ly + S * 0.20], fill=accent + (70,))
    halo = halo.filter(ImageFilter.GaussianBlur(S * 0.055))
    glow = img.copy()
    glow.alpha_composite(halo)
    img.alpha_composite(Image.blend(img, glow, 0.85).filter(ImageFilter.GaussianBlur(S * 0.0015)))

    sp = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    sd = ImageDraw.Draw(sp)
    rng = np.random.default_rng(777)
    for _ in range(9):
        ang = rng.uniform(0, 6.28318)
        rad = rng.uniform(0.15, 0.42) * S
        px = cx + np.cos(ang) * rad * 1.25
        py = body_top + np.sin(ang) * rad * 0.42 - S * 0.02
        r = S * rng.uniform(0.008, 0.017)
        sd.line([px - r, py, px + r, py], fill=ink + (235,), width=int(S * 0.0035))
        sd.line([px, py - r, px, py + r], fill=ink + (235,), width=int(S * 0.0035))
    img.alpha_composite(sp.filter(ImageFilter.GaussianBlur(S * 0.0012)))

    b = img.filter(ImageFilter.GaussianBlur(S * 0.012))
    final = Image.alpha_composite(b, img)
    arr = np.asarray(final, dtype=np.float64)
    lum = arr[:, :, 0] * 0.30 + arr[:, :, 1] * 0.59 + arr[:, :, 2] * 0.11
    warm = np.clip((lum - 90) / 165.0, 0, 1)
    arr[:, :, 0] = np.clip(arr[:, :, 0] + warm * 26, 0, 255)
    arr[:, :, 1] = np.clip(arr[:, :, 1] + warm * 18, 0, 255)
    arr[:, :, 2] = np.clip(arr[:, :, 2] + warm * 4, 0, 255)
    return Image.fromarray(arr.astype(np.uint8), "RGBA")


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    for suffix, pal in THEMES.items():
        paint_backdrop(pal).save(os.path.join(OUT, f"space_{suffix}.png"))
        paint_chest(pal).save(os.path.join(OUT, f"Chest_{suffix}.png"))
        print(f"{suffix}: space_{suffix}.png + Chest_{suffix}.png")