"""Report which base-game materials will still render flat, wrong or noisy.

`material_sweep.py` answers "does this material's colour map resolve". That
is not what a shopper sees. This groups the whole database by **what the
preview will actually do**, so a family can be fixed once instead of item by
item - the same grouping discipline the 6r-5n fixes used, applied to the
remaining exposure instead of to one report.

Verdicts, in the order the plugin applies them:

    OK            colour map resolves, format is colour-capable
    FLAT          the database gives the material NO colour map by design, so
                  the preview draws its name-derived base colour. Correct
                  behaviour, listed so the count is not mistaken for a bug.
    CUTOUT        no colour map but an opacity mask: promoted to a tinted
                  cutout. Correct behaviour.
    MASK_AS_ALBEDO  the resolved map is BC4/BC5 - single-channel, no colour.
                  The preview now REFUSES these (6r-5n), so the range falls
                  back to flat. Listed because a refusal is a fallback, not
                  a resolution.
    MISSING       the colour map the database names is not in any archive.

The first three are fine. The last two are the work queue, and the report
prints them grouped by the directory the texture lives in, which is the
unit a fix can be written against.

    python material_report.py --data "C:\\...\\Data" --report flat.tsv
"""

from __future__ import annotations

import argparse
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ba2lib import map_tex_format  # noqa: E402
from material_sweep import TextureIndex, build_cdb_index  # noqa: E402

# DXGI formats with fewer than three channels, per MapTexFormat in LiveModel.cpp.
NO_COLOUR_FMT = {80, 83}


def family_of(key: str) -> str:
    """The directory the texture lives in: the unit a fix can target."""
    i = key.rfind("/")
    return key[:i] if i > 0 else key


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", required=True)
    ap.add_argument("--cdb")
    ap.add_argument("--report", help="write the full per-material verdict table here")
    ap.add_argument("--limit", type=int, default=25, help="families to print per bucket")
    args = ap.parse_args()

    data = Path(args.data)
    cdb = Path(args.cdb) if args.cdb else Path(
        __file__).resolve().parent.parent / "renderer_recon" / "extracted" / "materialsbeta.cdb"
    index = build_cdb_index(cdb)
    flat_total = sum(1 for v in index.values() if not v.get("color"))
    print(f"cdb: {len(index):,} texture sets indexed ({flat_total:,} textureless by design)")

    archives = sorted(p for p in data.glob("*.ba2") if "textures" in p.name.lower())
    print(f"indexing {len(archives)} texture archive(s)...")
    tidx = TextureIndex(archives)
    if tidx.unparsed:
        print(f"  [WARN] {len(tidx.unparsed)} archive(s) did not parse: {', '.join(tidx.unparsed[:6])}")
    print(f"  {len(tidx.map):,} unique texture names")

    verdict: dict[str, list[str]] = defaultdict(list)
    rows: list[str] = []
    for mat, slot in index.items():
        color = slot.get("color", "")
        opacity = slot.get("opacity", "")
        if not color:
            verdict["CUTOUT" if opacity else "FLAT"].append(mat)
            rows.append(f"{mat}\t{'CUTOUT' if opacity else 'FLAT'}\t-\t{opacity or '-'}")
            continue
        hit = tidx.map.get(color)
        if hit is None:
            verdict["MISSING"].append(mat)
            rows.append(f"{mat}\tMISSING\t{color}\t-")
            continue
        obj = hit[1]
        if not isinstance(obj, tuple) and map_tex_format(obj.dxgi) in NO_COLOUR_FMT:
            verdict["MASK_AS_ALBEDO"].append(mat)
            rows.append(f"{mat}\tMASK_AS_ALBEDO\t{color}\t{hit[0].path.name} dxgi={obj.dxgi}")
            continue
        verdict["OK"].append(mat)
        rows.append(f"{mat}\tOK\t{color}\t{hit[0].path.name}")

    total = len(index)
    print()
    for k in ("OK", "FLAT", "CUTOUT", "MASK_AS_ALBEDO", "MISSING"):
        n = len(verdict[k])
        print(f"  {k:<15} {n:>7,}  ({n * 100 // total}%)")

    work = ("MASK_AS_ALBEDO", "MISSING")
    print("\n=== work queue, grouped by texture family ===")
    for k in work:
        fams = defaultdict(list)
        for mat in verdict[k]:
            color = index[mat].get("color", "")
            fams[family_of(color) if color else "(no albedo)"].append(mat)
        print(f"\n{k}: {len(verdict[k])} materials in {len(fams)} families")
        for fam, mats in sorted(fams.items(), key=lambda kv: -len(kv[1]))[: args.limit]:
            print(f"  {len(mats):>4}  {fam}")
            for m in sorted(mats)[:4]:
                print(f"          {m.rsplit('/', 1)[-1]}")
            if len(mats) > 4:
                print(f"          ... +{len(mats) - 4} more")
        if len(fams) > args.limit:
            print(f"  ... {len(fams) - args.limit} more families")

    if args.report:
        Path(args.report).write_text("\n".join(rows) + "\n", encoding="utf-8")
        print(f"\nfull table written to {args.report} ({len(rows):,} rows)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())