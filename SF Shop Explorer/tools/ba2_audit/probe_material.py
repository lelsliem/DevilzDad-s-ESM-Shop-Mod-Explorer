"""Answer "what will the preview actually load for THIS material?" for a
handful of reported-wrong materials.

`material_sweep.py` answers that question across the whole database but
does not accept a NIF material *path*, and the plugin's real path is the
four-step lookup in FindCdbMaterialLocked (normalized full path, the
`data/`-prefixed form, the bare stem, then the numbered family base).
This script reproduces those four steps exactly, then reports whether the
colour map resolves to a real file, in which archive, at which
dimensions and DXGI format - which is what distinguishes "no texture" from
"a single-channel BC4 mask being used as albedo".

Usage
-----
  python probe_material.py --data "C:\\...\\Data" \\
      --mat "Materials\\SetDressing\\SupplyContainers\\Supplycontainer_Labels_Green.mat" \\
      --mat "Materials\\Items\\FoodDrink_SingleItems\\...\\PackagedFood_Tomato.mat"
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ba2lib import map_tex_format  # noqa: E402
from material_sweep import TextureIndex, build_cdb_index  # noqa: E402

# Single-channel / non-colour DXGI formats. A BC4 map used as albedo is the
# signature of the "the fuzzy scorer handed back the mask" bug.
SINGLE_CHANNEL_DXGI = {80, 81, 95, 96, 97, 98, 99, 113}


def find_cdb_material(index: dict, mat_path: str):
    """Line-for-line replica of FindCdbMaterialLocked in LiveModel.cpp."""
    path = mat_path.lower().replace("\\", "/").lstrip("/")
    colon = path.rfind(":")
    if colon != -1:
        path = path[:colon]
    stem = path.rsplit("/", 1)[-1]
    if len(stem) > 4 and stem.endswith(".mat"):
        stem = stem[:-4]
    if len(stem) < 3:
        return None, []
    tried = ["path", "data/path", "stem"]
    if path.startswith("data/"):
        tried.remove("data/path")
    for label, key in (("path", path), ("data/path", "data/" + path), ("stem", stem)):
        if label not in tried:
            continue
        if key in index:
            return index[key], tried[: tried.index(label) + 1]
    base = stem.rstrip("0123456789").rstrip("_")
    tried.append("base=" + base)
    if base != stem and base in index:
        return index[base], tried
    return None, tried


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", required=True)
    ap.add_argument("--mat", action="append", required=True, help="NIF material path (repeatable)")
    ap.add_argument("--cdb")
    ap.add_argument("--glob", action="append",
                    help="archive filename glob (repeatable, default: every *extures*.ba2)")
    ap.add_argument("--archive-cache", help="text file of 'name<TAB>archive' to reuse across runs")
    args = ap.parse_args()

    data = Path(args.data)
    cdb = Path(args.cdb) if args.cdb else Path(
        __file__).resolve().parent.parent / "renderer_recon" / "extracted" / "materialsbeta.cdb"
    index = build_cdb_index(cdb)
    flat = sum(1 for v in index.values() if not v.get("color"))
    print(f"cdb: {len(index):,} texture sets indexed ({flat:,} textureless)")

    # Case-insensitive by hand: Git Bash globs are case-sensitive, so
    # *[Tt]extures.ba2 silently misses "Starfield - Textures01.ba2".
    archives = []
    for pat in (args.glob or ["*.ba2"]):
        archives += sorted(data.glob(pat))
    seen = set()
    archives = [a for a in archives
                if not (a in seen or seen.add(a))
                and (args.glob or "textures" in a.name.lower())]
    print(f"indexing {len(archives)} texture archive(s)...")
    tidx = TextureIndex(archives)
    print(f"  {len(tidx.map):,} unique texture names")

    bad = 0
    for mat in args.mat:
        slot, tried = find_cdb_material(index, mat)
        print("\n" + "=" * 78)
        print("MATERIAL " + mat)
        if slot is None:
            bad += 1
            print("  MISS  no cdb texture set (tried: %s)" % ", ".join(tried))
            continue
        print("  hit   via %s" % ", ".join(tried))
        color = slot.get("color", "")
        if not color:
            # 6r-5n: an empty set is authoritative "flat shaded, no albedo".
            op = slot.get("opacity", "")
            print("  FLAT   no albedo map in the shipped set%s"
                  % ("; opacity-only cutout %s -> %s" % (op, tidx.describe(op)) if op else ""))
            if op:
                print("         -> promoted to a white/tinted cutout")
            continue
        print("  color   %s\n          -> %s" % (color, tidx.describe(color)))
        obj = tidx.map.get(color, (None, None))[1]
        if obj is not None and not isinstance(obj, tuple) and obj.dxgi in SINGLE_CHANNEL_DXGI:
            bad += 1
            print("  !!     SINGLE-CHANNEL dxgi=%d used as albedo" % obj.dxgi)
        if slot.get("opacity"):
            op = slot["opacity"]
            print("  opacity %s\n          -> %s" % (op, tidx.describe(op)))
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())