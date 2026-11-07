"""Where do a creation's weapon texture paths live in its materialsbeta.cdb?

The per-creation databases do not follow the base game's ordering: objects are
named after the material ('Breechblock_Receiver_2') and NOT '_textureset<n>',
so a walk that only keys texture paths off '_textureset' names finds no colour
map for any of them. This prints the surrounding records for the objects that
do carry a 0x038A texture path, plus the neighbourhood of every '_color.dds'
string, so the real owning convention can be read off the file instead of
guessed.

    python probe_cdb_texture_owner.py --ba2 "B:/.../sfbgs050 - main.ba2" breechblock
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
    ap.add_argument("--inner", default="")
    ap.add_argument("--limit", type=int, default=6)
    a = ap.parse_args()

    inner = a.inner
    if not inner:
        inner = "materials/creations/%s/materialsbeta.cdb" % (
            os.path.basename(a.ba2).split(" - ")[0].lower())
    blob = load_from_ba2(a.ba2, inner)
    if not blob:
        return 1
    print("blob", len(blob), "bytes")

    objs = list(R.objects(blob))
    print("objects:", len(objs))
    suffix = sum(1 for n, _ in objs if "_textureset" in n.lower())
    print("objects named *_textureset*:", suffix)

    matched = [o for o in objs if a.match.lower() in o[0].lower()]
    print("objects matching %r: %d" % (a.match, len(matched)))
    for name, recs in matched[: a.limit]:
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

    # Any record at all whose payload mentions the match? That is the object
    # that owns the maps, whatever it is called.
    hits = 0
    for name, recs in objs:
        for off, fid, kind, cnt, pay in recs:
            if a.match.lower().encode() in pay.lower():
                print("OWNER %-40s 0x%04x kind=%d cnt=%d %r"
                      % (name[:40], fid, kind, cnt, pay[:60]))
                hits += 1
                break
        if hits >= a.limit:
            break
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
