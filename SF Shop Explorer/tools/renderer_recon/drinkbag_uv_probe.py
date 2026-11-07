"""Sample the drink-bag atlases at the meshes' real UVs.

The Alien Tea container NIF names four materials, each on its own `.mesh`:

    CraftedFood_Pouch.mat        <-> CraftedFood_AlienTea_mesh:0     (film, 518 v)
    CraftedFood_AlienTea.mat     <-> CraftedFood_AlienTea_mesh02:2   (liquid, 274 v)
    CraftedFood_AlienTeaLeaf.mat <-> CraftedFood_AlienTea_mesh03:3   (print, 1868 v)
    CraftedFood_AlientTonic.mat  <-> CraftedFood_AlienTea_mesh04:0   (cap, 430 v)

materialsbeta.cdb maps three of those to `alientea_color.dds` and the pouch to
`alientonic_color.dds`, and the plugin draws exactly that. The inventory
reference shows a dark-red bag with a bright red printed creature, so the
question this tool answers is: at the UVs these meshes actually carry, what
does each atlas contain, and does one of them match the reference?

    python drinkbag_uv_probe.py            # numbers only
    python drinkbag_uv_probe.py --overlay  # also write uv_overlay_*.png
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "ba2_audit"))
from ba2lib import Ba2Archive  # noqa: E402
from nif_uv import half_to_float  # noqa: E402

HERE = Path(__file__).resolve().parent
BAGS = HERE / "extracted" / "drinkbags"
DATA = Path("B:/SteamLibrary/steamapps/common/Starfield/Data")
INDEX = HERE.parent / "ba2_audit" / "extracted" / "ba2nameindex.tsv"

# Both maps are shipped in the same folder; the pouch and the tonic share one,
# the tea and its printed leaf share the other.
MESHES = {
    "film/Pouch": "geometries/8c8859b0c910afc4adb6/4cec2732026327bd7273.mesh",
    "liquid/AlienTea": "geometries/2f5949d4d4675ff49305/1d26937d02df6f463924.mesh",
    "print/AlienTeaLeaf": "geometries/2a892febf40647d1e7d4/1dbef4ca3c4d320d6605.mesh",
    "cap/AlienTonic": "geometries/6e63a8d999db835f9701/f6194c3cdce68e3bd5b5.mesh",
}


def owner_index() -> dict[str, str]:
    out: dict[str, str] = {}
    with INDEX.open("r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            name, _, arc = line.rstrip("\n").rpartition("\t")
            if name and arc:
                out[name] = arc
    return out


def uv_pairs(blob: bytes) -> list[tuple[float, float]]:
    """The single UV stream of a .mesh LOD0: u32 count == nv then nv half pairs."""
    o = 0
    o += 4                      # unknown
    nidx = struct.unpack_from("<I", blob, o)[0]
    o += 4 + nidx * 2
    o += 4                      # border
    o += 4                      # weights per vertex
    nv = struct.unpack_from("<I", blob, o)[0]
    o += 4 + nv * 6             # positions
    cnt = struct.unpack_from("<I", blob, o)[0]
    if cnt != nv:
        raise ValueError("first stream is not the UV set (%d != %d)" % (cnt, nv))
    o += 4
    out = []
    for _ in range(nv):
        u = half_to_float(struct.unpack_from("<H", blob, o)[0])
        v = half_to_float(struct.unpack_from("<H", blob, o + 2)[0])
        out.append((u, v))
        o += 4
    return out


def sample(rgb, mask, uvs, flip_v: bool):
    """Mean/percentile of the atlas texel each UV lands on, and of the mask."""
    w, h = rgb.size
    px = rgb.load()
    mp = mask.load()
    vals = []
    alpha = []
    for u, v in uvs:
        x = min(w - 1, max(0, int(u * w)))
        y = min(h - 1, max(0, int(((1.0 - v) if flip_v else v) * h)))
        vals.append(px[x, y])
        alpha.append(mp[x, y][0] / 255.0)
    n = len(vals)
    mean = tuple(sum(c[i] for c in vals) / n for i in range(3))
    sat = sum(max(c) - min(c) for c in vals) / n
    return mean, sat, sum(alpha) / n


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--overlay", action="store_true", help="write uv_overlay_*.png")
    args = ap.parse_args()

    from PIL import Image, ImageDraw  # noqa: PLC0415

    tea = Image.open(BAGS / "alientea_color.png").convert("RGB")
    tonic = Image.open(BAGS / "alientonic_color.png").convert("RGB")
    mask = Image.open(BAGS / "alienenergydrink_opacity.png").convert("RGB")

    owners = owner_index()
    uvs_by_mesh: dict[str, list[tuple[float, float]]] = {}
    for label, key in MESHES.items():
        arc = owners[key]
        ar = Ba2Archive(DATA / arc)
        blob, err = ar.read_entry(key)
        if blob is None:
            print("[FAIL] %s: %s" % (label, err))
            return 1
        uvs_by_mesh[label] = uv_pairs(blob)

    print("%-20s %-9s %-22s %-22s %s" %
          ("mesh", "verts", "alientea_color mean", "alientonic_color mean", "mask"))
    for label, uvs in uvs_by_mesh.items():
        row = ["%-20s %-9d" % (label, len(uvs))]
        for atlas in (tea, tonic):
            m, sat, a = sample(atlas, mask, uvs, flip_v=False)
            row.append("%-22s" % ("(%3.0f,%3.0f,%3.0f) sat%3.0f" % (*m, sat)))
        _, _, a = sample(tea, mask, uvs, flip_v=False)
        row.append("mean %.2f" % a)
        print(" ".join(row))

    print("\npercentiles over the sampled texels (tea atlas):")
    for label, uvs in uvs_by_mesh.items():
        px = tea.load()
        w, h = tea.size
        vals = sorted(px[min(w - 1, int(u * w)), min(h - 1, int(v * h))] for u, v in uvs)
        lo = vals[len(vals) // 10]
        hi = vals[len(vals) // 10 * 9]
        print("  %-20s p10=(%3d,%3d,%3d) p90=(%3d,%3d,%3d)" % (label, *lo, *hi))

    if args.overlay:
        for name, atlas in (("tea", tea), ("tonic", tonic)):
            img = atlas.copy().resize((1024, 1024))
            d = ImageDraw.Draw(img)
            for i, (label, uvs) in enumerate(uvs_by_mesh.items()):
                col = ((255, 255, 255), (0, 255, 0), (0, 128, 255), (255, 255, 0))[i]
                for u, v in uvs:
                    x = int(u * 1024)
                    y = int(v * 1024)
                    d.point((x, y), fill=col)
                    d.point((min(1023, x + 1), y), fill=col)
                    d.point((x, min(1023, y + 1)), fill=col)
                d.rectangle((4, 4 + i * 14, 18, 14 + i * 14), fill=col)
                d.text((22, 4 + i * 14), label, fill=col)
            img.save(BAGS / ("uv_overlay_%s.png" % name))
            print("wrote uv_overlay_%s.png" % name)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
