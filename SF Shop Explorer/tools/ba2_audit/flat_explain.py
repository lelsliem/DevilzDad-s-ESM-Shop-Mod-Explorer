"""Which flat materials can the new CDB override tint explain, and which not?

6r-7c shipped the override colour's rgb for textureless ranges. This asks the
follow-up: for every material the sweep called FLAT (no colour map in the
database, so the preview draws a flat colour), does the database actually
carry a tinted override for it? The ones it cannot explain are the remaining
work, and naming them is the point of this script.

    python flat_explain.py --verdicts "D:\\...\\material-verdicts.tsv"
"""
from __future__ import annotations

import argparse
import collections
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "renderer_recon"))
import cdb_records as R  # noqa: E402
import cdb_schema as S  # noqa: E402

BS = chr(92)
SUF = re.compile(r"_(material|textureset|layer|blender|uvstream[0-9a-z]*|cell|permutationlayer)[0-9]*$", re.I)


def base_of(name: str) -> str:
    return SUF.sub("", name).lower().replace(BS, "/")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--verdicts", required=True)
    ap.add_argument("--limit", type=int, default=40)
    a = ap.parse_args()

    color: dict[str, list] = {}
    mats: set[str] = set()
    for name, recs in R.objects(S.load()):
        bs = base_of(name)
        if "_material" in name.lower():
            mats.add(bs)
        for _, fid, _k, _c, pay in recs:
            if fid == 0x025A:
                v = R.decode(0, 0, pay)
                if isinstance(v, list) and len(v) == 4:
                    color.setdefault(bs, v)

    def tinted(bs: str) -> bool:
        v = color.get(bs)
        return bool(v) and max(v[:3]) < 0.97 and max(v[:3]) > 0.0

    rows: list[tuple[str, str, str]] = []
    counts: collections.Counter = collections.Counter()
    # The table has NO header row - the first line is already data, which
    # cost this script a silent zero-row run the first time. Columns are
    # positional: material, verdict, colour key, opacity key.
    with open(a.verdicts, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            p = line.rstrip("\n").split("\t")
            if len(p) < 2:
                continue
            verdict = p[1].upper()
            if verdict not in ("FLAT", "MISSING", "MASK_AS_ALBEDO", "CUTOUT"):
                continue
            rows.append((verdict, p[0], "\t".join(p)))

    print("rows scanned %d" % len(rows))
    print()
    tally: collections.Counter = collections.Counter()
    unexplained: list[tuple[str, str]] = []
    for verdict, key, _raw in rows:
        stem = key.lower().replace(BS, "/").rsplit("/", 1)[-1]
        if stem.endswith(".mat"):
            stem = stem[:-4]
        bs = "data/" + stem + ".mat" if "/" in key.replace(BS, "/") else stem
        hit = any(tinted(c) for c in (stem, bs, stem + ".mat"))
        tally["%s/%s" % (verdict, "tint-explained" if hit else "NOT explained")] += 1
        if not hit:
            unexplained.append((verdict, stem))
    for k, v in sorted(tally.items()):
        print("   %-34s %6d" % (k, v))
    print()
    print("work-queue materials the CDB override cannot explain (%d):" % len(unexplained))
    for verdict, stem in sorted(unexplained)[:a.limit]:
        print("   %-14s %s" % (verdict, stem))
    if len(unexplained) > a.limit:
        print("   ... %d more" % (len(unexplained) - a.limit))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())