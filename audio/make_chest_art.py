"""Higher-fidelity themed chest art for SFShopExplorer.

Replaces the flat geometric crate from make_theme_art.py with a painted,
lit chest: bevelled lid, plank shading, metal corner brackets with rivets,
a latch plate, a warm inner glow from the seam, contents bulging over the top,
cast shadow and bloom.

Run:  python make_chest_art.py [suffix ...]   (default: all three premium)
"""
import importlib.util
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = r"B:\MO2\Starfield\mods\Shop Test Render\SFSE\Plugins\SFShopExplorerAssets"

spec = importlib.util.spec_from_file_location("mta", os.path.join(HERE, "make_theme_art.py"))
mta = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mta)

THEMES = mta.THEMES


def _shade(col, k):
    return tuple(max(0, min(255, int(c * k))) for c in col)


def _mix(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def draw_chest(suffix, pal, S=1024):
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    accent, accent2, ink = pal["accent"], pal["accent2"], pal["ink"]
    base, deep = pal["base"], pal["deep"]

    # Wood/plank body: mid tone with a lighter top face and darker base.
    wood_hi = _mix(_shade(base, 2.1), (255, 255, 255), 0.06)
    wood_mid = _mix(_shade(base, 1.5), accent2, 0.10)
    wood_lo = _shade(deep, 1.25)

    cx = S * 0.5
    L, R = S * 0.11, S * 0.89
    body_top = S * 0.42
    body_bot = S * 0.855
    lid_top = body_top - S * 0.235
    lid_flat = body_top + S * 0.045

    # ---- cast shadow on the ground -------------------------------------
    sh = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(sh).ellipse([cx - S * 0.44, body_bot - S * 0.055, cx + S * 0.44, body_bot + S * 0.10],
                               fill=(0, 0, 0, 165))
    sh = sh.filter(ImageFilter.GaussianBlur(S * 0.026))
    img.alpha_composite(sh)

    # ---- contents bulging over the open seam ---------------------------
    rng = np.random.default_rng(abs(hash(suffix)) % (2**31))
    contents = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    cd = ImageDraw.Draw(contents)
    for _ in range(16):
        w = S * rng.uniform(0.035, 0.085)
        h = S * rng.uniform(0.05, 0.13)
        x = rng.uniform(L + S * 0.06, R - S * 0.06 - w)
        y = lid_flat - h + rng.uniform(-S * 0.02, S * 0.02)
        if rng.random() < 0.42:
            # bottle-ish: body + neck
            cd.rectangle([x, y + h * 0.25, x + w, y + h], fill=_shade(accent2, 0.85) + (255,))
            cd.rectangle([x + w * 0.34, y, x + w * 0.66, y + h * 0.28], fill=_shade(accent2, 1.05) + (255,))
        elif rng.random() < 0.5:
            # crate-ish box with a lighter lid band
            cd.rectangle([x, y, x + w, y + h], fill=_mix(wood_mid, accent2, rng.uniform(0.0, 0.35)) + (255,))
            cd.rectangle([x, y, x + w, y + h * 0.22], fill=_shade(accent2, 1.0) + (255,))
        else:
            # rounded relic
            cd.ellipse([x, y, x + w, y + h], fill=_mix(wood_hi, accent2, 0.25) + (255,))
    contents = contents.filter(ImageFilter.GaussianBlur(S * 0.0016))
    img.alpha_composite(contents)

    # ---- lid dome -------------------------------------------------------
    d.pieslice([L, lid_top, R, lid_flat + S * 0.10], 180, 360, fill=wood_hi + (255,))
    d.rectangle([L, lid_flat - S * 0.02, R, lid_flat + S * 0.055], fill=wood_hi + (255,))
    # inner shading on the dome for volume
    dome_sh = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(dome_sh).pieslice([L, lid_top, R, lid_flat + S * 0.10], 180, 360, fill=(0, 0, 0, 90))
    dome_sh = dome_sh.filter(ImageFilter.GaussianBlur(S * 0.030))
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).pieslice([L, lid_top, R, lid_flat + S * 0.10], 180, 360, fill=255)
    img.alpha_composite(Image.composite(dome_sh, Image.new("RGBA", (S, S)), mask))

    # lid rim (metal)
    d.arc([L, lid_top, R, lid_flat + S * 0.10], 180, 360, fill=_mix(accent2, (255, 255, 255), 0.25) + (255,),
          width=int(S * 0.016))

    # ---- body -----------------------------------------------------------
    d.rectangle([L, body_top, R, body_bot], fill=wood_mid + (255,))
    # Vertical plank seams. Each plank also gets its own slight value shift so
    # the front reads as separate boards instead of one flat panel.
    planks = 5
    pw = (R - L) / planks
    for i in range(planks):
        if i % 2 == 1:
            d.rectangle([L + pw * i, body_top, L + pw * (i + 1), body_bot],
                        fill=_mix(wood_mid, wood_hi, 0.16) + (90,))
    for i in range(1, planks):
        x = L + pw * i
        d.line([x, body_top, x, body_bot], fill=_shade(wood_lo, 0.62) + (225,), width=int(S * 0.007))
        d.line([x + S * 0.008, body_top, x + S * 0.008, body_bot], fill=_shade(wood_hi, 1.12) + (140,), width=int(S * 0.004))
    # base shadow gradient
    grad = np.zeros((int(body_bot - body_top), int(R - L), 4), dtype=np.float64)
    yy = np.linspace(0, 1, grad.shape[0])[:, None]
    grad[:, :, 0:3] = 0
    grad[:, :, 3] = np.clip((yy - 0.45) * 210, 0, 150)
    gimg = Image.fromarray(grad.astype(np.uint8), "RGBA")
    img.alpha_composite(gimg, (int(L), int(body_top)))

    # warm inner glow leaking from the open seam
    glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(glow).rectangle([L + S * 0.03, lid_flat - S * 0.005, R - S * 0.03, lid_flat + S * 0.05],
                                   fill=accent + (150,))
    img.alpha_composite(glow.filter(ImageFilter.GaussianBlur(S * 0.020)))

    # ---- metal banding + corner brackets --------------------------------
    band = int(S * 0.020)
    for y in (body_top + S * 0.055, body_bot - band * 2.6):
        d.rectangle([L, y, R, y + band], fill=_mix(accent2, (60, 60, 66), 0.35) + (255,))
        d.line([L, y, R, y], fill=_mix(accent2, (255, 255, 255), 0.45) + (255,), width=int(S * 0.004))
    for bx in (L, R - S * 0.055):
        d.rectangle([bx, body_top, bx + S * 0.055, body_bot], fill=_mix(accent2, (55, 55, 60), 0.42) + (255,))
        for k in range(4):
            ry = body_top + S * 0.10 + k * (body_bot - body_top - S * 0.18) / 3.0
            d.ellipse([bx + S * 0.018, ry - S * 0.008, bx + S * 0.037, ry + S * 0.011],
                      fill=_mix(accent2, (255, 255, 255), 0.35) + (235,))
    # lid brackets
    for bx in (L + S * 0.012, R - S * 0.062):
        d.rectangle([bx, lid_top + S * 0.085, bx + S * 0.050, lid_flat + S * 0.02],
                    fill=_mix(accent2, (55, 55, 60), 0.42) + (255,))

    # ---- latch ----------------------------------------------------------
    lx = cx
    ly = lid_flat + S * 0.010
    d.rounded_rectangle([lx - S * 0.058, ly - S * 0.028, lx + S * 0.058, ly + S * 0.070],
                        radius=S * 0.014, fill=_mix(accent2, (255, 255, 255), 0.30) + (255,))
    d.rounded_rectangle([lx - S * 0.026, ly - S * 0.004, lx + S * 0.026, ly + S * 0.046],
                        radius=S * 0.008, fill=_shade(deep, 1.6) + (255,), outline=ink + (230,), width=int(S * 0.005))
    d.ellipse([lx - S * 0.010, ly + S * 0.012, lx + S * 0.010, ly + S * 0.032], fill=accent + (255,))

    # ---- sparkles / motes ----------------------------------------------
    sp = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    sd = ImageDraw.Draw(sp)
    for _ in range(11):
        ang = rng.uniform(0, math.tau)
        rad = rng.uniform(0.16, 0.44) * S
        px = cx + math.cos(ang) * rad * 1.22
        py = lid_flat + math.sin(ang) * rad * 0.40 - S * 0.015
        r = S * rng.uniform(0.007, 0.016)
        sd.line([px - r, py, px + r, py], fill=ink + (225,), width=int(S * 0.0032))
        sd.line([px, py - r, px, py + r], fill=ink + (225,), width=int(S * 0.0032))
    img.alpha_composite(sp.filter(ImageFilter.GaussianBlur(S * 0.0011)))

    # ---- bloom + specular sheen ----------------------------------------
    bloom = img.filter(ImageFilter.GaussianBlur(S * 0.014))
    img.alpha_composite(Image.blend(img, bloom, 0.80))
    # A soft sheen across the dome. Built from a blurred ellipse rather than a
    # pieslice wedge: the wedge left a hard-edged white triangle that read as a
    # polygon, not a highlight.
    spec = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(spec).ellipse([L + S * 0.06, lid_top - S * 0.02, cx + S * 0.10, lid_flat - S * 0.01],
                                 fill=(255, 255, 255, 70))
    spec = spec.filter(ImageFilter.GaussianBlur(S * 0.038))
    # keep the sheen inside the lid silhouette
    lid_mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(lid_mask).pieslice([L, lid_top, R, lid_flat + S * 0.10], 180, 360, fill=255)
    img.alpha_composite(Image.composite(spec, Image.new("RGBA", (S, S)), lid_mask))

    arr = np.asarray(img, dtype=np.float64)
    lum = arr[:, :, 0] * 0.30 + arr[:, :, 1] * 0.59 + arr[:, :, 2] * 0.11
    warm = np.clip((lum - 96) / 160.0, 0, 1)
    arr[:, :, 0] = np.clip(arr[:, :, 0] + warm * 24, 0, 255)
    arr[:, :, 1] = np.clip(arr[:, :, 1] + warm * 16, 0, 255)
    arr[:, :, 2] = np.clip(arr[:, :, 2] + warm * 3, 0, 255)
    return Image.fromarray(arr.astype(np.uint8), "RGBA")


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    wanted = sys.argv[1:] or list(THEMES)
    for s in wanted:
        draw_chest(s, THEMES[s]).save(os.path.join(OUT, f"Chest_{s}.png"))
        print(f"Chest_{s}.png")