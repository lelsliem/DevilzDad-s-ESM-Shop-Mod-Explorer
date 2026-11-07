"""Census every distinct CDB field id and how well the header name pool explains it.

Prints one line per distinct field id:
  id  occurrences  classes  candidates
where `candidates` is every name-pool string whose *index* has been
observed anywhere in a descriptor-adjacent context, or - failing that -
whose normalised form matches the id's known typeCode/offset shape.

Run:  python cdb_fields.py            # census
      python cdb_fields.py --ids      # just the id table
"""
from __future__ import annotations

import argparse
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cdb_schema as S  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))


def all_cdbs() -> list[str]:
    """Every materialsbeta.cdb we can reach: the extracted base-game copy
    plus any per-mod copy already pulled out by cdb_mods.py."""
    out = []
    ext = os.path.join(HERE, "extracted")
    for root, _dirs, files in os.walk(ext):
        for f in files:
            if f.lower() == "materialsbeta.cdb":
                out.append(os.path.join(root, f))
    return sorted(out)


def census(path: str) -> tuple[dict, list[str]]:
    b = S.load(path)
    cls = S.classes(b)
    names = S.names(b)
    fid_classes: dict[int, set[int]] = collections.defaultdict(set)
    fid_count: dict[int, int] = collections.Counter()
    fid_shape: dict[int, set[tuple]] = collections.defaultdict(set)
    for ci, recs in enumerate(cls):
        for fid, typ, flags, off, sz in recs:
            fid_classes[fid].add(ci)
            fid_count[fid] += 1
            fid_shape[fid].add((typ, off, sz))
    return {"cls": cls, "names": names, "per_id": fid_classes,
            "count": fid_count, "shape": fid_shape}, names


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ids", action="store_true")
    ap.add_argument("--cdb", default="")
    a = ap.parse_args()

    paths = [a.cdb] if a.cdb else all_cdbs()
    if not paths:
        print("no materialsbeta.cdb found under extracted/", file=sys.stderr)
        return 1
    print("# %d database(s)" % len(paths))
    for p in paths:
        print("#   %s (%d bytes)" % (os.path.relpath(p, HERE), os.path.getsize(p)))

    merged_per_id: dict[int, set[int]] = collections.defaultdict(set)
    merged_count: collections.Counter = collections.Counter()
    merged_shape: dict[int, set[tuple]] = collections.defaultdict(set)
    all_names: list[str] = []
    for p in paths:
        d, names = census(p)
        if not all_names:
            all_names = names
        elif names != all_names:
            print("#  NOTE: %s has a DIFFERENT name pool (%d vs %d strings)"
                  % (os.path.basename(os.path.dirname(p)), len(names), len(all_names)))
        for fid, cis in d["per_id"].items():
            merged_per_id[fid] |= cis
            merged_count[fid] += d["count"][fid]
            merged_shape[fid] |= d["shape"][fid]

    print("\nname pool: %d strings, classes: %d"
          % (len(all_names), len(d["cls"])))
    print("distinct field ids: %d\n" % len(merged_per_id))
    hdr = "  %-6s %-6s %-9s %s" % ("id", "ncls", "type/off/sz", "names-pool-index-hits")
    print(hdr)
    print("  " + "-" * (len(hdr) + 20))
    for fid in sorted(merged_per_id):
        shapes = sorted(merged_shape[fid])
        sdesc = ",".join("0x%04x/%d/%d" % s for s in shapes[:3])
        print("  0x%04x %-6d %-9s cls=%s"
              % (fid, len(merged_per_id[fid]), sdesc,
                 ",".join(str(c) for c in sorted(merged_per_id[fid])[:6])))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())