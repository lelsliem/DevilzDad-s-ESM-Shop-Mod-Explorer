#!/usr/bin/env python3
"""Combine a sharded render audit into one table and a per-type summary.

`render_workbench.py --shard n/8 --once` writes one `workbench.tsv` per shard
and its own tile tree. Nothing joins them, so a full sweep previously needed an
undocumented ad-hoc script to answer "how many records rendered". This is that
step, kept beside the workbench:

    python -X utf8 tools/render_audit_combine.py reports/item-render-audit-2026-11-03

It writes `<dir>/combined.tsv` (one row per record, sorted by type then form)
and `<dir>/summary.txt` (records / rendered / blank / flat / textured per type,
plus the blank reasons). Rows are de-duplicated on `(form, type)`, so a record
split across two shards cannot be counted twice.

Compare two sweeps by loading both `combined.tsv` files and keying on
`(form, type)`; a rule change shows up as textured→flat regressions or flat→
textured gains, and an unchanged renderer must show neither.
"""
from __future__ import annotations

import argparse
import collections
import csv
from pathlib import Path

FIELDS = ["form", "name", "type", "plugin", "status", "model", "geoms",
          "tris", "textured", "delta", "changed", "shard"]


def combine(root: Path) -> tuple[list[dict], int]:
    rows: list[dict] = []
    for tsv in sorted(root.glob("shard-*/workbench.tsv")):
        shard = tsv.parent.name.replace("shard-", "")
        with tsv.open(encoding="utf-8", newline="") as fh:
            for row in csv.DictReader(fh, delimiter="\t"):
                row = {k: (row.get(k) or "") for k in FIELDS if k != "shard"}
                row["shard"] = shard
                rows.append(row)
    seen: set[tuple[str, str]] = set()
    unique: list[dict] = []
    for row in rows:
        key = (row["form"], row["type"])
        if key in seen:
            continue
        seen.add(key)
        unique.append(row)
    unique.sort(key=lambda r: (r["type"], r["form"]))
    return unique, len(rows)


def summarise(unique: list[dict]) -> str:
    by_type: dict[str, collections.Counter] = collections.defaultdict(collections.Counter)
    reasons: dict[str, collections.Counter] = collections.defaultdict(collections.Counter)
    for row in unique:
        stats = by_type[row["type"]]
        stats["records"] += 1
        textured = int(row["textured"] or 0)
        if row["status"]:
            stats["blank"] += 1
            reasons[row["type"]][row["status"]] += 1
        else:
            stats["rendered"] += 1
            stats["flat" if textured == 0 else "textured"] += 1
        if int(row["geoms"] or 0):
            stats["with_geometry"] += 1

    lines = [f"{'type':6}{'records':>9}{'rendered':>10}{'blank':>8}{'flat':>8}{'textured':>10}"]
    total: collections.Counter = collections.Counter()
    for kind in sorted(by_type):
        s = by_type[kind]
        lines.append(f"{kind:6}{s['records']:>9}{s['rendered']:>10}{s['blank']:>8}"
                     f"{s['flat']:>8}{s['textured']:>10}")
        total.update(s)
    lines.append(f"{'total':6}{total['records']:>9}{total['rendered']:>10}{total['blank']:>8}"
                 f"{total['flat']:>8}{total['textured']:>10}")
    lines += ["", "blank reasons:"]
    for kind in sorted(reasons):
        for reason, count in reasons[kind].most_common():
            lines.append(f"  {kind:5} {count:5}  {reason}")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("directory", nargs="?", default="reports/item-render-audit-2026-11-03",
                    help="sweep directory holding shard-*/workbench.tsv")
    ap.add_argument("--combined", default="combined.tsv", help="output table name")
    ap.add_argument("--summary", default="summary.txt", help="output summary name")
    args = ap.parse_args()

    root = Path(args.directory)
    shards = sorted(root.glob("shard-*/workbench.tsv"))
    if not shards:
        print(f"no shard workbench.tsv under {root}")
        return 1
    unique, raw = combine(root)
    out = root / args.combined
    with out.open("w", encoding="utf-8", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=FIELDS, delimiter="\t")
        writer.writeheader()
        writer.writerows(unique)
    text = f"shards: {len(shards)}  rows: {len(unique)} (from {raw} shard rows)\n\n" + summarise(unique)
    (root / args.summary).write_text(text + "\n", encoding="utf-8")
    print(text)
    print(f"\nwrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
