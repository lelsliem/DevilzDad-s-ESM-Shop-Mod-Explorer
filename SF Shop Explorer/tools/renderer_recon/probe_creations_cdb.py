"""Dump one material's records out of a creations materialsbeta.cdb.

The base-game index resolves most weapons, but the SFBGS (Creations) families
come from their own per-creation database, and those are the materials that
came back "textureless" in game. This prints the raw DIFF records for objects
whose name matches, so the field ids and payloads can be compared with the
base-game layout the indexer assumes.

    python probe_creations_cdb.py --ba2 "B:/.../sfbgs050 - main.ba2" breechblock
    python probe_creations_cdb.py --cdb extracted/materialsbeta.cdb breechblock
"""
from __future__ import annotations

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "ba2_audit"))

import cdb_records as R  # noqa: E402


def load_from_ba2(path: str, inner: str) -> bytes | None:
    from ba2lib import Ba2Archive  # noqa: E402
    ar = Ba2Archive(path)
    for name in ar.entries:
        if name.lower() == inner.lower():
            data, why = ar.read_entry(name)
            return data
    print("no entry", inner)
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("match", nargs="?", default="")
    ap.add_argument("--ba2", default="")
    ap.add_argument("--cdb", default="")
    ap.add_argument("--inner", default="")
    ap.add_argument("--limit", type=int, default=3)
    a = ap.parse_args()

    if a.ba2:
        inner = a.inner or os.path.join(
            "materials", "creations",
            os.path.basename(a.ba2).split(" - ")[0].lower(), "materialsbeta.cdb")
        blob = load_from_ba2(a.ba2, inner.replace(os.sep, "/"))
    else:
        with open(a.cdb, "rb") as fh:
            blob = fh.read()
    if not blob:
        return 1
    print("blob", len(blob), "bytes")

    hits = 0
    for name, recs in R.objects(blob):
        if a.match and a.match.lower() not in name.lower():
            continue
        hits += 1
        print("== %s (%d records)" % (name, len(recs)))
        for off, fid, kind, cnt, pay in recs:
            if fid == 0x038A:
                raw = pay[:cnt].split(b"\x00")[0].decode("latin-1", "replace")
                print("  0x%04x TEXTURE %r" % (fid, raw))
            elif fid == 0x01F4:
                print("  0x%04x NAME    %r" % (fid, R.decode(kind, cnt, pay)))
            else:
                print("  0x%04x kind=%d cnt=%-4d %s"
                      % (fid, kind, cnt, str(R.decode(kind, cnt, pay))[:70]))
        if hits >= a.limit:
            break
    print("objects matched:", hits)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
