#!/usr/bin/env python3
"""Render the 19 Bacta-family vials as one 160px regression sheet.

    python tools/renderer_recon/bacta_vial_sheet.py
    python tools/renderer_recon/bacta_vial_sheet.py --size 320 --out reports/bacta-vials-320.png

The sheet is a visual check of the model-keyed pane close: every vial should
read as one silhouette. The command exits non-zero if a form is missing, draws
no rimmed geometry, or still reports more than one closed contour, so it can be
run after a change to `merge_panes`, `pane_merge_scale` or `outline_mask`.
The PNG is written even when the check fails, so the split tile can be inspected.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE.parent / "ba2_audit"))

import aid_colour_audit as audit  # noqa: E402
import contact_sheet as sheet  # noqa: E402
import silhouette_audit as silhouette  # noqa: E402

# The 19 ALCH records the 2026-11-01 silhouette audit flagged before the
# Bacta-family close. Keyed by plugin as well as form: a FormID is not unique
# across plugins, and several Bacta vials are mod records.
VIALS = (
    ("Starfield.esm", "0003D3A9"),       # Antibiotic Cocktail
    ("Starfield.esm", "002995A3"),       # Bacta Vial (Panacaea)
    ("Starfield.esm", "0029A84F"),       # Infection X (crafted inhaler)
    ("Starfield.esm", "0029A850"),       # Gungan Oil (crafted inhaler)
    ("Starfield.esm", "0029A859"),       # Bacta Vial (Panacaea)
    ("Starfield.esm", "002A9DE8"),       # Emergency Bacta Vial
    ("Starfield.esm", "002F4436"),       # Antibiotics
    ("SW Resources.esm", "100008E2"),    # Bacta Vial (Bacta.nif)
    ("SW Resources.esm", "100008E3"),    # Addichrone
    ("SW Resources.esm", "100008E5"),    # Antibiotic Vial (Bacta_Pink)
    ("SW Resources.esm", "100008E8"),    # Heal Paste (Bacta_Orange)
    ("SW Resources.esm", "100008EB"),    # Anchored Immobilizers
    ("SW Resources.esm", "100008ED"),    # Boosted Injector
    ("SW Resources.esm", "100008F2"),    # Repairing Immobilizer
    ("SW Resources.esm", "100008F3"),    # Boudicca
    ("SW Resources.esm", "100008F4"),    # Junk Flush
    ("SW Resources.esm", "100008F6"),    # Penicillin X
    ("SW Resources.esm", "100008F7"),    # Snake Oil
    ("SW Resources.esm", "100008F8"),    # Heart+
)


def font(size: int):
    try:
        return ImageFont.load_default(size)
    except TypeError:
        return ImageFont.load_default()


def blank_tile(size: int, label: str) -> Image.Image:
    """A labelled placeholder, so a missing record is not a silent hole."""
    img = Image.new("RGB", (size, size), sheet.BACKGROUND)
    draw = ImageDraw.Draw(img)
    draw.text((6, size // 2 - 6), label[:30], fill=(190, 120, 120), font=font(11))
    return img


def contours(geoms: list[dict], size: int) -> int:
    """The largest closed-loop count in any rim group of the completed mask."""
    parts = silhouette.item_parts(geoms)
    if not any(part["rim"] for part in parts):
        return 0
    width = max(1, int(round(size / 320)))
    counts = [silhouette.contour_count(sheet.outline_mask(after, width))
              for _members, _before, after in silhouette.group_masks(parts, size)]
    return max(counts) if counts else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="reports/bacta-vials-160.png")
    ap.add_argument("--size", type=int, default=160)
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
    records = {(r["plugin"], f"{r['form']:08X}"): r
               for r in audit.parse_catalog(catalog_path)}
    store = audit.AssetStore([Path(a.data)],
                             Path(a.mods_root) if a.mods_root else None, False)
    plugins = audit.Plugins(store)
    swaps = audit.Swaps(Path(a.swaps))
    cdb = audit.Cdb(Path(a.cdb))
    textures = sheet.Textures(store)
    meshes: dict[str, dict] = {}

    size, pad, label_h, cols = a.size, 8, 44, 5
    cell_w, cell_h, header = size + pad * 2, size + label_h + pad, 42
    rows = (len(VIALS) + cols - 1) // cols
    out = Image.new("RGB", (cols * cell_w, header + rows * cell_h), (10, 12, 16))
    draw = ImageDraw.Draw(out)
    title_font, small = font(14), font(11)
    draw.text((pad, 7), f"Bacta vials - {size}px regression sheet",
              fill=(205, 210, 220), font=title_font)
    draw.text((pad, 25), "one silhouette per vial; amber/green is the closed-loop count",
              fill=(140, 145, 155), font=small)

    failures: list[str] = []
    one = 0
    for i, (plugin, form) in enumerate(VIALS):
        rec = records.get((plugin, form))
        x = (i % cols) * cell_w + pad
        y = header + (i // cols) * cell_h
        if rec is None:
            out.paste(blank_tile(size, "record missing"), (x, y))
            draw.text((x, y + size + 4), f"{form} {plugin}", fill=(190, 120, 120), font=small)
            draw.text((x, y + size + 20), "record missing", fill=(190, 120, 120), font=small)
            failures.append(form)
            continue
        geoms, meta = sheet.prepare_item(
            rec, store, plugins, swaps, cdb, textures, meshes)
        count = contours(geoms, size) if geoms else 0
        if geoms:
            img, _tris = sheet.render_model(geoms, size)
        else:
            img = blank_tile(size, meta.get("status") or "no geometry")
        out.paste(img, (x, y))
        colour = (130, 200, 160) if count == 1 else (210, 150, 110)
        draw.text((x, y + size + 4), f"{form}  {rec['type']}",
                  fill=(145, 150, 160), font=small)
        draw.text((x, y + size + 19), f"{count} contour{'s' if count != 1 else ''}"
                  if geoms else (meta.get("status") or "no geometry"),
                  fill=colour, font=small)
        name = rec["name"] if len(rec["name"]) <= 28 else rec["name"][:27] + "..."
        draw.text((x, y + size + 33), name, fill=(205, 210, 220), font=small)
        if count == 1:
            one += 1
        else:
            failures.append(f"{form} {rec['name']} ({count})")

    draw.text((cols * cell_w - 150, 7), f"{one}/{len(VIALS)} one contour",
              fill=(130, 200, 160) if one == len(VIALS) else (210, 150, 110),
              font=title_font)
    out_path = Path(a.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out.save(out_path)
    print(f"wrote {out_path} ({out.size[0]}x{out.size[1]}), {one}/{len(VIALS)} one contour")
    if failures:
        for failure in failures:
            print(f"FAIL {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
