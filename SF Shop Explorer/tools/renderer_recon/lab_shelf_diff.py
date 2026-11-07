"""Diff two contact-sheet tile sets and rank the tiles that moved.

The point-of-use is a rule change: render a shelf's tiles before and after,
then look at exactly the items the change touched instead of the whole page.

    python lab_shelf_diff.py --before DIR --after DIR \
        --tsv reports/lab-diff.tsv --sheet reports/lab-diff.png

Every tile that exists in both folders is compared pixel for pixel. The report
is one row per tile, sorted by the number of changed pixels, so the movers are
at the top; the sheet lays out the first `--top` of them as before | after
pairs with the item name and the changed-pixel count.

A tile missing on one side is reported as such instead of being counted as a
change: a shelf render that failed halfway should not read as a rendering
difference.
"""
from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent
sys.path.insert(0, str(TOOLS))

import aid_colour_audit as audit  # noqa: E402

LABEL_H = 22


def catalog_index(catalog: str | None) -> dict[int, dict]:
    path = audit.find_catalog(catalog)
    if not path:
        return {}
    return {item["form"]: item for item in audit.parse_catalog(path)}


def compare(before: Path, after: Path, name: str) -> dict:
    a, b = before / name, after / name
    if not a.exists() or not b.exists():
        return {"state": "missing", "px": 0, "mean": 0.0, "size": ""}
    ia = np.asarray(Image.open(a).convert("RGB"), dtype=np.int16)
    ib = np.asarray(Image.open(b).convert("RGB"), dtype=np.int16)
    if ia.shape != ib.shape:
        return {"state": "size differs", "px": 0, "mean": 0.0,
                "size": f"{ia.shape[1]}x{ia.shape[0]} vs {ib.shape[1]}x{ib.shape[0]}"}
    delta = np.abs(ia - ib)
    changed = int((delta.max(axis=2) > 0).sum())
    return {"state": "identical" if changed == 0 else "changed",
            "px": changed, "mean": float(delta.mean()),
            "size": f"{ia.shape[1]}x{ia.shape[0]}"}


def write_sheet(rows: list[dict], before: Path, after: Path, out: Path,
                top: int, size: int) -> None:
    movers = [r for r in rows if r["state"] == "changed"][:top]
    if not movers:
        return
    cols = 2
    cell_w, cell_h = size, size + LABEL_H
    sheet = Image.new("RGB", (cell_w * cols, cell_h * len(movers)), (16, 18, 22))
    draw = ImageDraw.Draw(sheet)
    for i, r in enumerate(movers):
        y = i * cell_h
        for col, folder in enumerate((before, after)):
            tile = Image.open(folder / r["file"]).convert("RGB")
            if tile.size != (size, size):
                tile = tile.resize((size, size))
            sheet.paste(tile, (col * cell_w, y + LABEL_H))
        draw.text((4, y + 4),
                  f"{r['name']}  {r['form']}  {r['px']} px",
                  fill=(240, 180, 120))
    sheet.save(out)
    print(f"sheet: {out} ({len(movers)} pairs)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--before", required=True, help="tile directory of the old render")
    ap.add_argument("--after", required=True, help="tile directory of the new render")
    ap.add_argument("--tsv", required=True, help="report path")
    ap.add_argument("--sheet", help="before/after PNG of the biggest movers")
    ap.add_argument("--top", type=int, default=12)
    ap.add_argument("--size", type=int, default=192, help="sheet tile size")
    ap.add_argument("--catalog", default=None)
    args = ap.parse_args()

    before, after = Path(args.before), Path(args.after)
    items = catalog_index(args.catalog)
    names = sorted(p.name for p in after.glob("*.png"))
    rows = []
    for name in names:
        res = compare(before, after, name)
        form = int(Path(name).stem, 16) if Path(name).stem.isalnum() else 0
        item = items.get(form, {})
        rows.append({"file": name, "form": Path(name).stem,
                     "name": item.get("name", "?"),
                     "model": (item.get("modelPath") or "").replace("\\", "/"),
                     **res})
    rows.sort(key=lambda r: (-r["px"], r["name"]))
    out = Path(args.tsv)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", encoding="utf-8", newline="") as fh:
        wr = csv.DictWriter(fh, delimiter="\t",
                            fieldnames=["form", "name", "model", "state", "px",
                                        "mean", "size", "file"])
        wr.writeheader()
        wr.writerows(rows)
    changed = [r for r in rows if r["state"] == "changed"]
    missing = [r for r in rows if r["state"] == "missing"]
    print(f"{len(rows)} tile(s), {len(changed)} changed, {len(missing)} missing, "
          f"{len(rows) - len(changed) - len(missing)} identical")
    for r in changed[:args.top]:
        print(f"  {r['px']:6d} px  {r['form']}  {r['name'][:36]}")
    if args.sheet:
        write_sheet(rows, before, after, Path(args.sheet), args.top, args.size)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
