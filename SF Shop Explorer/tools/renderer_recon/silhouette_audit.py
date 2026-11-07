#!/usr/bin/env python3
"""Flag transparent props whose rim still draws more than one closed contour.

The renderer groups rim parts by world-space bounds, closes nearby panes into
one island, then fills enclosed holes. A prop whose panes are still apart after
that draws two or more loops. This audit scans every record with a resolved
NIF - the seven item forms, containers, and other modeled records - builds the
same rim mask render_model builds at a chosen tile size, and counts the
connected loops per rim group.

    python tools/renderer_recon/silhouette_audit.py --out reports/silhouette-audit.tsv
    python tools/renderer_recon/silhouette_audit.py --type ARMO --limit 100

Texture decode is skipped: the rim decision comes from the material path,
BSGeometry name and CDB alpha, not from the colour map. `material_files=False`
also skips the .mat colour fallback, which is the other expensive per-item step.
A scan is for silhouette coverage, not for a tile's colour.
"""
from __future__ import annotations

import argparse
import sys
import time
from collections import Counter
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE.parent / "ba2_audit"))

import aid_colour_audit as audit  # noqa: E402
import contact_sheet as sheet  # noqa: E402

TYPES = ("ARMO", "WEAP", "MISC", "AMMO", "ALCH", "KEYM", "BOOK")
# Display cases and other transparent props are containers, not Misc. The
# catalog does not carry a model path for most object types, but CONT records are
# resolved by the plugin walk, so include the type by default.
DEFAULT_TYPES = TYPES + ("CONT",)


def has_catalog_model(item: dict) -> bool:
    """Whether the dump resolved a NIF for a record outside the item types.

    CONT is the important one here: display cases are containers, not Misc, and
    most object types have no modelPath in the dump at all.
    """
    model = (item.get("model") or "").lower().replace("/", "\\")
    return model.endswith(".nif") or bool(item.get("nif_paths"))


class NoTextures:
    """A Textures stand-in: material resolution must not decode a colour map."""

    def get(self, key: str):
        return None


def contour_count(mask) -> int:
    """Connected components of an edge mask, 8-connected (Pillow's polygons).

    A closed silhouette is one loop, so a prop that still has two islands in
    the rim mask reports two.
    """
    pixels = np.asarray(mask) > 0
    height, width = pixels.shape
    seen = np.zeros_like(pixels, dtype=bool)
    count = 0
    for y in range(height):
        for x in range(width):
            if not pixels[y, x] or seen[y, x]:
                continue
            count += 1
            stack = [(y, x)]
            seen[y, x] = True
            while stack:
                cy, cx = stack.pop()
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        ny, nx = cy + dy, cx + dx
                        if (0 <= ny < height and 0 <= nx < width
                                and pixels[ny, nx] and not seen[ny, nx]):
                            seen[ny, nx] = True
                            stack.append((ny, nx))
    return count


def island_count(mask) -> int:
    """Connected coverage components, before the contour is taken."""
    return contour_count(mask)


def item_parts(geoms: list[dict]) -> list[dict]:
    """The triangles render_model rasterises, without asking for colours."""
    parts: list[dict] = []
    for geom in geoms:
        mesh = geom.get("mesh")
        mat = geom.get("mat")
        if mesh is None or mat is None:
            continue
        raw = mesh["idx"]
        if raw.size < 3:
            continue
        idx = raw[:(raw.size // 3) * 3].reshape(-1, 3)
        if idx.shape[0] > sheet.MAX_TRIS:
            idx = idx[np.linspace(0, idx.shape[0] - 1, sheet.MAX_TRIS).astype(np.int64)]
        world = sheet.world_vertices(mesh, geom["world"])
        parts.append({"tris": world[idx], "rim": sheet.needs_rim(mat),
                      "nif": geom.get("nif", ""), "name": geom.get("name", ""),
                      "mat": mat})
    return parts


def group_masks(parts: list[dict], size: int):
    """Yield (members, coverage mask, closed mask) per rim group."""
    groups = sheet.rim_groups(parts)
    if not groups:
        return
    all_world = np.concatenate([p["tris"] for p in parts], axis=0)
    screen, _depth = sheet.camera(all_world.reshape(-1, 3))
    px = sheet.fit(screen, size).reshape(-1, 3, 2)
    starts: list[int] = []
    at = 0
    for part in parts:
        starts.append(at)
        at += part["tris"].shape[0]
    for members in groups:
        mask = Image.new("L", (size, size), 0)
        draw = ImageDraw.Draw(mask)
        for index in members:
            tri_px = px[starts[index]:starts[index] + parts[index]["tris"].shape[0]]
            for k in range(tri_px.shape[0]):
                tri = tri_px[k]
                draw.polygon([(tri[0, 0], tri[0, 1]), (tri[1, 0], tri[1, 1]),
                              (tri[2, 0], tri[2, 1])], fill=255)
        scale = sheet.pane_merge_scale([parts[i].get("nif", "") for i in members])
        yield members, mask, sheet.merge_panes(mask, size, scale)


def audit_item(item: dict, runtime: dict, size: int) -> dict | None:
    geoms, meta = sheet.prepare_item(
        item, runtime["store"], runtime["plugins"], runtime["swaps"],
        runtime["cdb"], runtime["textures"], runtime["meshes"],
        material_files=False)
    if not geoms:
        return None
    parts = item_parts(geoms)
    if not any(p["rim"] for p in parts):
        return None
    rows = []
    for members, before, after in group_masks(parts, size):
        width = max(1, int(round(size / 320)))
        edge = sheet.outline_mask(after, width)
        rows.append({
            "members": members,
            "islands_before": island_count(before),
            "islands_after": island_count(after),
            "contours": contour_count(edge),
            "shapes": sorted({parts[i]["name"] for i in members if parts[i]["name"]}),
            "materials": sorted({parts[i]["mat"]["mat"] for i in members}),
            "nifs": sorted({parts[i]["nif"] for i in members if parts[i]["nif"]}),
        })
    if not rows:
        return None
    return {"meta": meta, "groups": rows, "parts": parts}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="reports/silhouette-audit.tsv")
    ap.add_argument("--size", type=int, default=160)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--type", action="append", default=[], choices=DEFAULT_TYPES,
                   help="restrict to a form type (repeatable; default: the seven "
                        "item types, containers, and modeled object records)")
    ap.add_argument("--catalog", default=None)
    ap.add_argument("--cdb", default=str(audit.CDB))
    ap.add_argument("--data", default=str(audit.GAME_DATA))
    ap.add_argument("--mods-root", default=str(audit.MODS_ROOT))
    ap.add_argument("--swaps", default=str(audit.SWAPS))
    a = ap.parse_args()

    catalog_path = audit.find_catalog(a.catalog)
    if catalog_path is None:
        print("catalog dump not found", file=sys.stderr)
        return 2
    catalog = audit.parse_catalog(catalog_path)
    types = set(a.type or DEFAULT_TYPES)
    items = [r for r in catalog if r["type"] in types]
    if not a.type:
        # Do not miss a modeled object whose form type is outside the item set;
        # most such types have no modelPath in the dump, so this stays small.
        items += [r for r in catalog
                  if r["type"] not in types and has_catalog_model(r)]
    if a.limit:
        items = items[:a.limit]

    store = audit.AssetStore([Path(a.data)],
                             Path(a.mods_root) if a.mods_root else None, False)
    runtime = {
        "store": store,
        "plugins": audit.Plugins(store),
        "swaps": audit.Swaps(Path(a.swaps)),
        "cdb": audit.Cdb(Path(a.cdb)),
        "textures": NoTextures(),
        "meshes": {},
    }

    started = time.perf_counter()
    store.index_loose()
    print(f"[index] loose files in {time.perf_counter() - started:.1f}s", file=sys.stderr)

    rows: list[dict] = []
    scanned = rimmed = 0
    stats = Counter()
    for number, item in enumerate(items, 1):
        scanned += 1
        result = audit_item(item, runtime, a.size)
        if result is not None:
            rimmed += 1
            groups = result["groups"]
            worst = max(g["contours"] for g in groups)
            islands = max(g["islands_after"] for g in groups)
            flagged = worst > 1
            stats["items"] += 1
            stats["groups"] += len(groups)
            stats["flagged" if flagged else "clean"] += 1
            if len(groups) > 1:
                stats["multi_group"] += 1
            rows.append({
                "plugin": item["plugin"],
                "formID": f"{item['form']:08X}",
                "type": item["type"],
                "name": item["name"],
                "model": result["meta"].get("model", ""),
                "groups": len(groups),
                "max_contours": worst,
                "max_islands": islands,
                "flag": "FLAG" if flagged else "",
                "detail": " | ".join(
                    f"g{gi}: contours={g['contours']} islands={g['islands_after']} "
                    f"shapes={','.join(g['shapes'])} mats={';'.join(g['materials'])}"
                    for gi, g in enumerate(groups)),
            })
        if number % 250 == 0 or number == len(items):
            elapsed = time.perf_counter() - started
            print(f"[scan] {number}/{len(items)} ({rimmed} rimmed, "
                  f"{stats['flagged']} flagged) {elapsed:.0f}s", file=sys.stderr)

    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    columns = ["plugin", "formID", "type", "name", "model", "groups",
               "max_contours", "max_islands", "flag", "detail"]
    with out.open("w", encoding="utf-8", newline="") as f:
        f.write("\t".join(columns) + "\n")
        for row in rows:
            f.write("\t".join(str(row[c]).replace("\t", " ") for c in columns) + "\n")

    elapsed = time.perf_counter() - started
    print(f"scanned {scanned} item(s), {rimmed} with rim geometry, "
          f"{stats['items']} rimmed / {stats['groups']} group(s), "
          f"{stats['flagged']} flagged, {stats['multi_group']} multi-prop, "
          f"{elapsed:.0f}s")
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
