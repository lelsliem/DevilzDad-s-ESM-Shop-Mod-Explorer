#!/usr/bin/env python3
"""Why is a tile flat, and is there art next to it that the rules did not take?

Reads the flat rows of an audit TSV, re-resolves each record through
`contact_sheet.prepare_item`, and groups the per-material decisions. For every
material that ended with no colour map it also reports whether the archive index
holds an albedo-like map in the material's own folder, which is the only class
of flat that a resolver change could fix.

    python -X utf8 tools/_tmp_probe_flats.py --limit 200
"""
from __future__ import annotations

import argparse
import collections
import csv
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
RECON = HERE / "renderer_recon"
sys.path[:0] = [str(RECON), str(HERE), str(HERE / "ba2_audit")]

import aid_colour_audit as audit  # noqa: E402
import contact_sheet as sheet  # noqa: E402


def flat_rows(tsv: Path) -> list[dict]:
    rows = list(csv.DictReader(open(tsv, encoding="utf-8"), delimiter="\t"))
    return [r for r in rows if r.get("status", "") == "" and r.get("textured", "0") in ("0", "")]


def texture_folder(mat_path: str) -> str:
    """The textures/ folder a material's art would live in.

    A material is named `Materials\\Items\\...\\x.mat` and its maps are named
    `textures/items/.../x_color.dds`: the two trees are parallel, not identical,
    so searching archive names under the material's own spelling finds nothing.
    """
    low = mat_path.replace("\\", "/").casefold()
    if low.startswith("materials/"):
        low = "textures/" + low[len("materials/"):]
    return low.rsplit("/", 1)[0]


def nearby_maps(keys, folder: str, stem: str) -> list[str]:
    """Colour-like maps in the material's texture folder that name its stem."""
    prefix = folder + "/"
    stem = stem.casefold()
    out = []
    for key in keys:
        if not key.startswith(prefix):
            continue
        base = key.rsplit("/", 1)[-1]
        if not base.endswith(".dds"):
            continue
        name = base[:-4]
        if stem and stem not in name:
            continue
        if (name.endswith("_color") or name.endswith("_colour")
                or name == "color" or name.endswith("_albedo")
                or name.endswith("_diffuse")):
            out.append(key)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--tsv", default="reports/item-render-audit/combined.tsv")
    ap.add_argument("--catalog", default=None)
    ap.add_argument("--limit", type=int, default=200)
    ap.add_argument("--out", default="reports/item-render-audit-2026-11-03/flat-probe.txt")
    args = ap.parse_args()

    items = audit.parse_catalog(audit.find_catalog(args.catalog))
    by_key = {(it["form"], it.get("type")): it for it in items}
    by_form = collections.defaultdict(list)
    for it in items:
        by_form[it["form"]].append(it)

    rows = flat_rows(Path(args.tsv))
    print(f"flat rows in {args.tsv}: {len(rows)}")
    pick = rows[: args.limit] if args.limit else rows

    store = audit.AssetStore([audit.GAME_DATA], audit.MODS_ROOT, False)
    plugins = audit.Plugins(store)
    swaps = audit.Swaps(audit.SWAPS)
    cdb = audit.Cdb(audit.CDB)
    textures = sheet.Textures(store)
    meshes: dict = {}

    reasons = collections.Counter()
    clusters: dict[str, list[dict]] = collections.defaultdict(list)
    missing_materials = 0
    resolved = 0
    no_geom = 0

    for row in pick:
        form = int(row["form"], 16)
        item = by_key.get((form, row["type"])) or (by_form[form][0] if by_form[form] else None)
        if item is None:
            continue
        try:
            geoms, meta = sheet.prepare_item(item, store, plugins, swaps, cdb, textures, meshes)
        except Exception as exc:                       # a crash here is a finding
            reasons[f"EXCEPTION {type(exc).__name__}"] += 1
            continue
        if not geoms:
            no_geom += 1
            reasons[f"status: {meta['status']}"] += 1
            continue
        resolved += 1
        mats = meta.get("materials") or []
        if not mats:
            missing_materials += 1
            reasons["no materials on any geometry"] += 1
            continue
        for mat in mats:
            if mat.get("texture"):
                continue
            why = (mat.get("color_why") or "?").strip()
            head = why.split(" -> ")[0].split("(")[0].strip()[:70]
            reasons[head] += 1
            stem = str(mat["mat"]).replace("\\", "/").rsplit("/", 1)[-1]
            stem = stem[:-4] if stem.casefold().endswith(".mat") else stem
            clusters[head].append({"name": row["name"], "form": row["form"],
                                   "model": row["model"], "mat": mat["mat"],
                                   "why": why, "set": mat.get("set", ""),
                                   "folder": texture_folder(str(mat["mat"])),
                                   "stem": stem})
    # Archive names are only needed for the "is there art beside it" question.
    store.name_owner  # ensure the attribute exists
    try:
        keys = list(store.name_owner.keys())
    except Exception:
        keys = []
    print(f"probed {len(pick)} rows: {resolved} with geometry, {no_geom} without, "
          f"{missing_materials} with no material, {len(keys):,} indexed names")

    lines = []
    lines.append(f"probed {len(pick)} of {len(rows)} flat rows; {resolved} with geometry, "
                 f"{no_geom} without geometry, {missing_materials} with no material")
    lines.append("")
    lines.append("material decisions with no colour map:")
    for why, count in reasons.most_common(24):
        lines.append(f"  {count:5}  {why}")
    lines.append("")
    for why, rows_ in sorted(clusters.items(), key=lambda kv: -len(kv[1]))[:8]:
        lines.append(f"== {why} ({len(rows_)} materials) ==")
        seen_folders = set()
        shown = 0
        for entry in rows_:
            if shown >= 6:
                break
            if entry["folder"] in seen_folders:
                continue
            seen_folders.add(entry["folder"])
            sibling = nearby_maps(keys, entry["folder"], entry["stem"])
            lines.append(f"  {entry['form']} {entry['name'][:34]:34} {entry['mat'][:64]}")
            lines.append(f"      why: {entry['why'][:110]}")
            lines.append(f"      set: {entry['set'][:40]:40} folder: {entry['folder']}  maps: {len(sibling)}"
                         + (f" e.g. {sibling[0]}" if sibling else ""))
            shown += 1
        lines.append("")

    text = "\n".join(lines)
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_text(text, encoding="utf-8")
    print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
