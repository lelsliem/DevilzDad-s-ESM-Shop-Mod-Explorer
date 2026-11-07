"""Rasterise each drink-bag mesh's UV triangles onto the flavour atlas.

The four meshes of `CraftedFood_Container_AlienTea.nif` each carry one UV set
whose bounding box covers most of the 1024x1024 atlas, so a bounding box says
nothing about what the mesh actually samples. Filling the real triangles does:

    python drinkbag_uv_footprint.py

writes, per mesh, a PNG that is the atlas with that mesh's UV footprint
highlighted, plus the mean/median atlas colour under the footprint - which is
what the shop preview ends up showing for that range.
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from drinkbag_uv_probe import BAGS, MESHES, owner_index, DATA  # noqa: E402

sys.path.insert(0, str(HERE.parents[0] / "ba2_audit"))
from ba2lib import Ba2Archive  # noqa: E402
from nif_uv import half_to_float  # noqa: E402

SIZE = 1024


def geometry(blob: bytes) -> tuple[list[tuple[int, int, int]], list[tuple[float, float]]]:
    """Indices (as vertex triples) and the UV set of a .mesh LOD0."""
    o = 4
    nidx = struct.unpack_from("<I", blob, o)[0]
    o += 4
    idx = list(struct.unpack_from("<%dH" % nidx, blob, o))
    o += nidx * 2
    o += 4                                  # border
    o += 4                                  # weights per vertex
    nv = struct.unpack_from("<I", blob, o)[0]
    o += 4 + nv * 6                         # positions
    cnt = struct.unpack_from("<I", blob, o)[0]
    if cnt != nv:
        raise ValueError("first stream is not the UV set")
    o += 4
    uvs = []
    for _ in range(nv):
        u = half_to_float(struct.unpack_from("<H", blob, o)[0])
        v = half_to_float(struct.unpack_from("<H", blob, o + 2)[0])
        uvs.append((u, v))
        o += 4
    tris = [tuple(idx[i:i + 3]) for i in range(0, len(idx) - 2, 3)]
    return tris, uvs


def footprint(atlas: Image.Image, tris, uvs, flip_v: bool) -> tuple[Image.Image, list]:
    w, h = atlas.size
    mask = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(mask)
    for t in tris:
        pts = []
        for i in t:
            u, v = uvs[i]
            x = min(w - 1, max(0, int(u * w)))
            y = min(h - 1, max(0, int(((1.0 - v) if flip_v else v) * h)))
            pts.append((x, y))
        d.polygon(pts, fill=255)
    return mask, list(mask.getdata())


def mean_under(atlas: Image.Image, mask_px: list) -> tuple[tuple[float, float, float], int]:
    px = list(atlas.convert("RGB").getdata())
    sel = [px[i] for i, m in enumerate(mask_px) if m > 128]
    if not sel:
        return (0.0, 0.0, 0.0), 0
    n = len(sel)
    return tuple(sum(c[i] for c in sel) / n for i in range(3)), n


def mip_chain(img: Image.Image, levels: int = 9) -> list:
    out = [img]
    for _ in range(levels - 1):
        cur = out[-1]
        out.append(cur.resize((max(1, cur.width // 2), max(1, cur.height // 2)), Image.BOX))
    return out


def mip_table(atlases: dict, meshes: dict) -> None:
    """Footprint mean per mip level.

    A 290x192 thumbnail of a whole item maps a UV footprint covering a third
    of a 1024x1024 atlas onto a hundred-odd pixels, so the hardware picks a
    high mip whose value is the atlas average, not the artwork. Printing the
    cascade is how that is told apart from a wrong texture.
    """
    print("\nfootprint mean per mip level (row = mesh, col = mip 0..8)")
    for name, atlas in atlases.items():
        chain = mip_chain(atlas)
        print("  -- %s (mip 8 = whole-atlas average %s)" %
              (name, tuple(round(c) for c in chain[-1].convert("RGB").resize((1, 1)).getpixel((0, 0)))))
        for label, (tris, uvs) in meshes.items():
            row = []
            for mip in chain:
                mask, mask_px = footprint(mip, tris, uvs, False)
                m, n = mean_under(mip, mask_px)
                row.append("(%3.0f,%3.0f,%3.0f)" % m)
            print("     %-20s %s" % (label, " ".join(row)))


def main() -> int:
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--mips", action="store_true", help="print the footprint mean per mip level")
    args = ap.parse_args()

    tea = Image.open(BAGS / "alientea_color.png").convert("RGB")
    tonic = Image.open(BAGS / "alientonic_color.png").convert("RGB")
    owners = owner_index()

    tiles = []
    geoms = {}
    for label, key in MESHES.items():
        ar = Ba2Archive(DATA / owners[key])
        blob, err = ar.read_entry(key)
        if blob is None:
            print("[FAIL] %s: %s" % (label, err))
            return 1
        tris, uvs = geometry(blob)
        geoms[label] = (tris, uvs)
        lines = []
        for name, atlas in (("tea", tea), ("tonic", tonic)):
            for flip in (False, True):
                mask, mask_px = footprint(atlas, tris, uvs, flip)
                m, n = mean_under(atlas, mask_px)
                lines.append("  %-6s flipv=%-5s footprint %5.1f%% mean=(%3.0f,%3.0f,%3.0f)"
                             % (name, flip, 100.0 * n / (SIZE * SIZE), *m))
        print("%s  %d tris, %d verts" % (label, len(tris), len(uvs)))
        print("\n".join(lines))

        # One review tile: the tea atlas with this mesh's footprint marked.
        mask, _ = footprint(tea, tris, uvs, False)
        base = tea.copy()
        tint = Image.new("RGB", base.size, (0, 255, 0))
        marked = Image.composite(Image.blend(base, tint, 0.45), base, mask)
        d = ImageDraw.Draw(marked)
        d.rectangle((0, 0, 320, 18), fill=(0, 0, 0))
        d.text((4, 3), label, fill=(255, 220, 120))
        tiles.append(marked.resize((512, 512), Image.LANCZOS))

    sheet = Image.new("RGB", (512 * 2 + 8, 512 * 2 + 8), (10, 10, 12))
    for i, t in enumerate(tiles):
        sheet.paste(t, ((i % 2) * 520, (i // 2) * 520))
    sheet.save(BAGS / "uv_footprint_tea.png")
    print("wrote uv_footprint_tea.png")
    if args.mips:
        mip_table({"alientea_color": tea, "alientonic_color": tonic}, geoms)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
