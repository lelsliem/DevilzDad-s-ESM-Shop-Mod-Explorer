#!/usr/bin/env python3
"""Why does a flat material have no CDB record, and which key shape would match?

For every material the resolver left without a colour map, this tests the key
shapes the index actually holds against the material path, so the gap is either
"the set does not exist" or "the lookup does not try this spelling".

    python -X utf8 tools/_tmp_cdb_gap.py --limit 250
"""
from __future__ import annotations

import argparse
import collections
import csv
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
RECON = HERE / "renderer_recon"
sys.path[:0] = [str(RECON), str(HERE)]

import aid_colour_audit as audit  # noqa: E402
import contact_sheet as sheet  # noqa: E402


def candidates(p: str) -> dict[str, str]:
    """Key shapes to try for one normalised material path."""
    low = p.casefold()
    base = low.rsplit("/", 1)[-1]
    stem = base[:-4] if base.endswith(".mat") else base
    folder = low.rsplit("/", 1)[0] if "/" in low else ""
    out = {
        "path": low,
        "data/path": "data/" + low if not low.startswith("data/") else low,
        "path-noext": low[:-4] if low.endswith(".mat") else low,
        "data/path-noext": ("data/" + low if not low.startswith("data/") else low).removesuffix(".mat"),
        "basename": stem,
        "data/basename": "data/" + stem,
        "basename-nodigits": stem.rstrip("0123456789").rstrip("_"),
        "family": stem.rsplit("_", 1)[0],
        "folder": folder.rsplit("/", 1)[-1] if folder else "",
    }
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--tsv", default="reports/item-render-audit/combined.tsv")
    ap.add_argument("--limit", type=int, default=250)
    ap.add_argument("--out", default="reports/item-render-audit-2026-11-03/cdb-gap.txt")
    args = ap.parse_args()

    cdb = audit.Cdb(audit.CDB)
    keys = cdb.sets
    rows = [r for r in csv.DictReader(open(args.tsv, encoding="utf-8"), delimiter="\t")
            if r.get("status", "") == "" and r.get("textured", "0") in ("0", "")][: args.limit]
    items = audit.parse_catalog(audit.find_catalog(None))
    by_form: dict[int, list[dict]] = collections.defaultdict(list)
    for it in items:
        by_form[it["form"]].append(it)

    store = audit.AssetStore([audit.GAME_DATA], audit.MODS_ROOT, False)
    plugins = audit.Plugins(store)
    swaps = audit.Swaps(audit.SWAPS)
    textures = sheet.Textures(store)
    meshes: dict = {}

    hits = collections.Counter()
    examples: dict[str, list[str]] = collections.defaultdict(list)
    total = 0
    for row in rows:
        form = int(row["form"], 16)
        item = by_form[form][0] if by_form[form] else None
        if item is None:
            continue
        try:
            _geoms, meta = sheet.prepare_item(item, store, plugins, swaps, cdb, textures, meshes)
        except Exception:
            continue
        for mat in meta.get("materials") or []:
            if mat.get("texture"):
                continue
            total += 1
            p = audit.norm(mat["mat"])
            cand = candidates(p)
            matched = ""
            for name, key in cand.items():
                record = keys.get(key)
                if record is None:
                    continue
                has = bool(record.get("color") or record.get("opacity") or record.get("maps"))
                matched = f"{name} ({'art' if has else 'empty'})"
                break
            if not matched:
                matched = "no key shape"
            hits[matched] += 1
            if len(examples[matched]) < 4:
                examples[matched].append(f"{row['name'][:28]:28} {mat['mat'][:70]}")

    lines = [f"flat materials probed: {total}", "", "first matching key shape:"]
    for name, count in hits.most_common():
        lines.append(f"  {count:5}  {name}")
        for ex in examples[name]:
            lines.append(f"         {ex}")
    text = "\n".join(lines)
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_text(text, encoding="utf-8")
    print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
