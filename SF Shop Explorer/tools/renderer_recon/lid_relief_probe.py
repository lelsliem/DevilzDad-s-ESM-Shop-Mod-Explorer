"""Does the Pils can's ring pull live in the normal map?

`Erdebrau Pilsner01.nif` has seven geometries and none of them is a tab: the
lid is a flat 16-gon plate at z 0.170-0.174 (110 triangles). Its UV island is
the pale foil column of the decal atlas, and that column carries no line art
either - so if the game shows a pull ring, the relief must be in the material's
BC5 `_normal` map. This probe renders the plate top-down from its own UVs and
shades it with that map, which is exactly what the preview renderer does not
do yet.

    python lid_relief_probe.py --out lid_relief.png

Writes three tiles: albedo only (what the preview draws today), normal-mapped
albedo (what the fix would draw), and the normal map's XY as colour.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "ba2_audit"))

import contact_sheet as sheet  # noqa: E402
import dump_tex as dt  # noqa: E402
from ba2lib import Ba2Archive  # noqa: E402

COLOUR = "textures/items/fooddrink_singleitems/erdebrau_beers/erdebrau pilsner_decal_color.dds"
NORMAL = "textures/items/fooddrink_singleitems/erdebrau_beers/erdebrau pilsner_decal_normal.dds"


def bc4_plane(buf: bytes, w: int, h: int, channel: int) -> np.ndarray:
    """One channel of a BC5 image, as floats in 0..1.

    BC5 is two BC4 blocks per 4x4 block - red then green, 16 bytes in all -
    so the channel selects which half of each block to decode. Splitting the
    buffer in half instead ("red plane, then green plane") decodes block data
    as if it were a different row and turns the map into confetti.
    """
    bw, bh = (w + 3) // 4, (h + 3) // 4
    out = bytearray(w * h * 3)
    step, base = 16, channel * 8
    for by in range(bh):
        for bx in range(bw):
            o = (by * bw + bx) * step + base
            dt.bc4_block(buf[o:o + 8], out, bx, by, w, h)
    return np.frombuffer(bytes(out), np.uint8)[0::3].reshape(h, w).astype(np.float32) / 255.0


def bc5_normal(path: Path) -> np.ndarray:
    """A BC5 normal map as (h, w, 3) tangent-space vectors.

    BC5 is two BC4 blocks per 4x4 block - red then green - and no blue. Z is
    reconstructed from the unit-length constraint, which is why the map stores
    only two channels.
    """
    ar = Ba2Archive(path)
    tf = ar.tex_files[NORMAL]
    data, err = ar.read_texture(NORMAL)
    if data is None:
        raise SystemExit("read_texture: %s" % err)
    w, h = tf.width, tf.height
    x = bc4_plane(data, w, h, 0) * 2.0 - 1.0
    y = bc4_plane(data, w, h, 1) * 2.0 - 1.0
    z = np.sqrt(np.clip(1.0 - x * x - y * y, 0.0, 1.0))
    return np.stack([x, y, z], axis=-1)


def load_colour(path: Path) -> np.ndarray:
    ar = Ba2Archive(path)
    tf = ar.tex_files[COLOUR]
    data, err = ar.read_texture(COLOUR)
    if data is None:
        raise SystemExit("read_texture: %s" % err)
    w, h = tf.width, tf.height
    bw, bh = (w + 3) // 4, (h + 3) // 4
    out = bytearray(w * h * 3)
    for by in range(bh):
        for bx in range(bw):
            o = (by * bw + bx) * 8
            dt.bc1_block(data[o:o + 8], out, bx, by, w, h)
    rgb = np.frombuffer(bytes(out), np.uint8).reshape(h, w, 3).astype(np.float32) / 255.0
    return rgb


def lid_geometry():
    store = sheet.audit.AssetStore([Path(sheet.audit.GAME_DATA)], Path(sheet.audit.MODS_ROOT), False)
    plugins = sheet.audit.Plugins(store)
    swaps = sheet.audit.Swaps(Path(sheet.audit.SWAPS))
    cdb = sheet.audit.Cdb(Path(sheet.audit.CDB))
    textures = sheet.Textures(store)
    item = {"name": "Erdebrau Pils Can", "form": 0x001FF69A, "plugin": "Starfield.esm",
            "type": "ALCH", "keywords": ""}
    geoms, _meta = sheet.prepare_item(item, store, plugins, swaps, cdb, textures, {})
    for geom in geoms:
        if geom["name"] == "ErdebrauPilsner:3":
            return geom
    raise SystemExit("lid geometry not found")


def sample(tex: np.ndarray, u: np.ndarray, v: np.ndarray) -> np.ndarray:
    h, w = tex.shape[:2]
    x = np.clip((np.abs(u) % 1.0) * (w - 1), 0, w - 1).astype(np.int32)
    y = np.clip((np.abs(v) % 1.0) * (h - 1), 0, h - 1).astype(np.int32)
    return tex[y, x]


def rasterise(geom: dict, size: int) -> tuple[np.ndarray, np.ndarray]:
    """Top-down orthographic render of the plate: (uv per pixel, coverage)."""
    mesh = geom["mesh"]
    pos = np.concatenate([mesh["pos"], np.ones((mesh["nv"], 1), np.float32)], axis=1)
    world = (pos @ geom["world"].astype(np.float32))[:, :3]
    uv = np.asarray(mesh["uv"], np.float32).reshape(-1, 2)
    idx = mesh["idx"][:(mesh["idx"].size // 3) * 3].reshape(-1, 3)
    lo = world[:, :2].min(axis=0)
    hi = world[:, :2].max(axis=0)
    span = float(max(hi - lo))
    pad = 0.06 * span
    scale = (size - 2 * pad) / span
    sx = (world[:, 0] - lo[0] + pad) * scale
    sy = size - (world[:, 1] - lo[1] + pad) * scale
    out_uv = np.zeros((size, size, 2), np.float32)
    cover = np.zeros((size, size), bool)
    for tri in idx:
        px = np.stack([sx[tri], sy[tri]], axis=1)
        x0, y0 = np.floor(px.min(axis=0)).astype(int)
        x1, y1 = np.ceil(px.max(axis=0)).astype(int)
        x0, y0 = max(x0, 0), max(y0, 0)
        x1, y1 = min(x1, size - 1), min(y1, size - 1)
        if x1 < x0 or y1 < y0:
            continue
        xs, ys = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        a, b, c = px[0], px[1], px[2]
        det = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
        if abs(det) < 1e-9:
            continue
        w0 = ((b[1] - c[1]) * (xs - c[0]) + (c[0] - b[0]) * (ys - c[1])) / det
        w1 = ((c[1] - a[1]) * (xs - c[0]) + (a[0] - c[0]) * (ys - c[1])) / det
        w2 = 1.0 - w0 - w1
        inside = (w0 >= -1e-4) & (w1 >= -1e-4) & (w2 >= -1e-4)
        if not inside.any():
            continue
        u = w0 * uv[tri[0], 0] + w1 * uv[tri[1], 0] + w2 * uv[tri[2], 0]
        v = w0 * uv[tri[0], 1] + w1 * uv[tri[1], 1] + w2 * uv[tri[2], 1]
        sl = (slice(y0, y1 + 1), slice(x0, x1 + 1))
        out_uv[sl][inside] = np.stack([u[inside], v[inside]], axis=1)
        cover[sl][inside] = True
    return out_uv, cover


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", default=str(sheet.audit.GAME_DATA))
    ap.add_argument("--out", default=str(HERE / "extracted" / "lid_relief.png"))
    ap.add_argument("--size", type=int, default=420)
    args = ap.parse_args()

    archive = Path(args.data) / "Starfield - Textures05.ba2"
    colour = load_colour(archive)
    normal = bc5_normal(archive)
    geom = lid_geometry()
    uv, cover = rasterise(geom, args.size)
    c = sample(colour, uv[..., 0], uv[..., 1])
    nm = sample(normal, uv[..., 0], uv[..., 1])

    # Flat lid: the geometric normal is +z, so the tangent-space vector IS the
    # shading normal (this is the cheap case - no frame needed to test it).
    key = np.array([0.35, 0.45, 0.82], np.float32)
    key /= np.linalg.norm(key)
    flat = np.clip(np.dot(np.array([0.0, 0.0, 1.0], np.float32), key), 0.0, 1.0)
    lit_flat = 0.42 + 0.48 * flat
    lit = (0.42 + 0.48 * np.clip(nm @ key, 0.0, 1.0))[..., None]

    def tile(art: np.ndarray) -> Image.Image:
        rgb = np.where(cover[..., None], np.clip(art * 0.80, 0.0, 1.0), 0.06)
        return Image.fromarray((rgb * 255).astype(np.uint8))

    a = tile(c * lit_flat)
    b = tile(c * lit)
    n = tile((nm * 0.5 + 0.5) * lit_flat)

    sheet_img = Image.new("RGB", (args.size * 3 + 24, args.size + 26), (16, 16, 20))
    draw = ImageDraw.Draw(sheet_img)
    for i, (img, label) in enumerate(((a, "albedo (today)"), (b, "+ normal map"), (n, "normal XY"))):
        sheet_img.paste(img, (i * (args.size + 12), 20))
        draw.text((i * (args.size + 12) + 4, 6), label, fill=(200, 210, 220))
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    sheet_img.save(out)
    print("wrote %s" % out)
    print("normal XY spread on the lid: |x| mean %.3f, |y| mean %.3f" %
          (float(np.abs(nm[..., 0][cover]).mean()), float(np.abs(nm[..., 1][cover]).mean())))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
