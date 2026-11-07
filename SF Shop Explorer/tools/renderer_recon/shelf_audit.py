"""Audit a shelf's preview resolution without rendering it.

`contact_sheet.py` answers "what does it look like"; this answers "what is the
pipeline unsure about". Both walk the same helpers, so a row here describes the
same material decisions the tiles were drawn from.

    python tools/renderer_recon/shelf_audit.py --shelf misc --out reports/misc-audit.tsv

Flags per item (see FLAG_HELP):

  no-model     no MODL or no renderable geometry
  untextured   no geometry has an albedo; the item is flat colour only
  flat-ranges  some geometry fell back to a flat colour with no map
  blind-alpha  a range is translucent at alpha 0 without naming itself glass
  ghost        a range is translucent but its material does not name glass
  effect-only  every geometry was skipped as an effect overlay
  decal-body   the item's largest textured range is a decal sheet
  shared-map   two or more differently named parts resolved to one map

The flags are questions, not verdicts: a pickaxe is legitimately flat, and a
printed can is legitimately a decal sheet. They exist so a shelf of 1,482 tiles
can be read in a ranked list instead of one item at a time.
"""
from __future__ import annotations

import argparse
import collections
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "ba2_audit"))

import aid_colour_audit as audit  # noqa: E402
import contact_sheet as sheet  # noqa: E402

FLAG_HELP = {
    "no-model": "no MODL / no renderable geometry",
    "untextured": "no range carries an albedo",
    "flat-ranges": "at least one range is flat colour",
    "blind-alpha": "translucent at alpha 0 without a glass token",
    "ghost": "translucent without a glass token",
    "effect-only": "every geometry skipped as an effect shell",
    "decal-body": "largest textured range is a decal sheet",
    "shared-map": "differently named parts share one map",
}


def classify(geoms: list[dict], meta: dict) -> tuple[list[str], list[str]]:
    """Flags plus the material notes that produced them."""
    flags: set[str] = set()
    notes: list[str] = []
    if not geoms:
        flags.add("no-model")
        return sorted(flags), notes
    textured = [g for g in geoms if g["mat"]["texture"]]
    if not textured:
        flags.add("untextured")
    flat = [g for g in geoms if not g["mat"]["texture"]]
    if flat:
        flags.add("flat-ranges")
        notes.append("flat: " + "; ".join(
            f"{g['mat']['mat'].rsplit(chr(92), 1)[-1]} "
            f"({g['mat']['color_why'][:80]})" for g in flat[:3]))
    for g in geoms:
        mat = g["mat"]
        glassy = any(audit.has_name_word(mat["mat"], t) for t in audit.SHEER_TOKENS)
        if mat["translucent"] and not glassy:
            flags.add("ghost")
            notes.append(f"ghost: {mat['mat']} alpha={mat['alpha']:.2f} "
                         f"({mat['alpha_why']})")
        if mat["translucent"] and mat["alpha"] <= 0.001 and not glassy:
            flags.add("blind-alpha")
    # A mesh that failed to parse has idx=None; it cannot be "the largest
    # range", and letting it into max() aborted the whole shelf run once the
    # Creation index resolved more materials (more ranges reached this test).
    def _tris(g: dict) -> int:
        mesh = g.get("mesh")
        if not isinstance(mesh, dict) or mesh.get("idx") is None:
            return 0
        return len(mesh["idx"]) // 3

    sized = [g for g in (textured or geoms) if _tris(g)]
    if sized:
        biggest = max(sized, key=_tris)
        if audit.is_decal_sheet(biggest["mat"]["mat"]):
            flags.add("decal-body")
    by_map: dict[str, set[str]] = collections.defaultdict(set)
    for g in geoms:
        if g["mat"]["texture"]:
            by_map[g["mat"]["texture"]].add(audit.norm(g["mat"]["mat"]))
    shared = {m: mats for m, mats in by_map.items() if len(mats) > 1}
    if shared:
        flags.add("shared-map")
        for m, mats in list(shared.items())[:2]:
            notes.append(f"shared: {m} <- {', '.join(sorted(mats))[:110]}")
    skipped = [m for m in meta["materials"] if m.get("effect")]
    if skipped and not geoms:
        flags.add("effect-only")
    if skipped:
        notes.append("skipped effect: " + ", ".join(
            m["mat"].rsplit(chr(92), 1)[-1] for m in skipped[:3]))
    return sorted(flags), notes


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--shelf", default="misc", choices=sorted(audit.SHELVES))
    ap.add_argument("--item", action="append", default=[])
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--out", required=True, help="TSV path")
    ap.add_argument("--materials-out", default=None,
                    help="also write one row per resolved material range")
    ap.add_argument("--catalog", default=None)
    ap.add_argument("--cdb", default=str(audit.CDB))
    ap.add_argument("--data", default=str(audit.GAME_DATA))
    ap.add_argument("--mods-root", default=str(audit.MODS_ROOT))
    ap.add_argument("--swaps", default=str(audit.SWAPS))
    a = ap.parse_args()

    catalog = audit.find_catalog(a.catalog)
    if not catalog:
        print("no catalog CSV found; pass --catalog", file=sys.stderr)
        return 2
    items = audit.select_items(audit.parse_catalog(catalog), [], a.item, False,
                               a.limit, [a.shelf])
    store = audit.AssetStore([Path(a.data)], Path(a.mods_root) if a.mods_root else None, False)
    plugins = audit.Plugins(store)
    swaps = audit.Swaps(Path(a.swaps) if a.swaps else None)
    cdb = audit.Cdb(Path(a.cdb))
    textures = sheet.Textures(store)
    meshes: dict = {}

    rows = []
    mat_rows = []
    counts: collections.Counter = collections.Counter()
    for n, item in enumerate(items, 1):
        geoms, meta = sheet.prepare_item(item, store, plugins, swaps, cdb, textures, meshes)
        flags, notes = classify(geoms, meta)
        counts.update(flags)
        for m in meta["materials"]:
            mat_rows.append({
                "item": item["name"], "form": f"{item['form']:08X}",
                "model": meta["model"], "mat": m["mat"], "set": m["set"],
                "texture": m["texture"] or "",
                "rgb": "%.3f,%.3f,%.3f" % tuple(m["color"]),
                "alpha": "%.3f" % m["alpha"],
                "why": m["color_why"], "alpha_why": m["alpha_why"],
                "effect": "yes" if m["effect"] else "",
            })
        if len(geoms) == 0 and meta["status"]:
            notes.insert(0, meta["status"])
        rows.append({
            "item": item["name"], "form": f"{item['form']:08X}",
            "model": meta["model"], "geoms": meta["geoms"], "tris": meta["tris"],
            "textured": meta["textured"], "flags": ",".join(flags) or "-",
            "notes": " | ".join(notes)[:400],
        })
        if n % 100 == 0:
            print(f"[audit] {n}/{len(items)}", file=sys.stderr)

    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with open(out, "w", encoding="utf-8", newline="") as fh:
        cols = ("item", "form", "model", "geoms", "tris", "textured", "flags", "notes")
        fh.write("\t".join(cols) + "\n")
        for r in rows:
            fh.write("\t".join(str(r[c]) for c in cols) + "\n")

    if a.materials_out:
        mout = Path(a.materials_out)
        mcols = ("item", "form", "model", "mat", "set", "texture", "rgb",
                 "alpha", "why", "alpha_why", "effect")
        with open(mout, "w", encoding="utf-8", newline="") as fh:
            fh.write("\t".join(mcols) + "\n")
            for r in mat_rows:
                fh.write("\t".join(str(r[c]) for c in mcols) + "\n")
        print(f"[audit] {len(mat_rows)} material row(s) -> {mout}", file=sys.stderr)

    print(f"[audit] {len(rows)} item(s) -> {out}", file=sys.stderr)
    for flag, n in counts.most_common():
        print(f"  {flag:<12} {n:5d}  {FLAG_HELP.get(flag, '')}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
