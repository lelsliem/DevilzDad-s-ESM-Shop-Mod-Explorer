"""Walk every DIFF record in materialsbeta.cdb and census field ids by material.

With the container framing solved (see cdb_schema.py), each record is

    "DIFF" u32 len, u32 fieldId, u16 kind, u16 count, count elements, 0xffff...

and records for one object are contiguous. An object is named: exactly one of
its records has kind 0 (string) whose bytes are the object's name, e.g.
`Abstract_Poster2x1_Material1`. That name is the ONLY key the plugin has, so
the useful output is: for every material name, the field ids it carries.

    python cdb_records.py --census          # field id -> how many objects, value histogram
    python cdb_records.py --name glass      # every field id for matching objects
    python cdb_records.py --dump <substr>   # raw record bytes for one object
"""
from __future__ import annotations

import argparse
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cdb_schema as S  # noqa: E402


def iter_records(b: bytes):
    """Yield (offset, fieldId, kind, count, data_bytes) for every DIFF.

    `len` counts everything after itself, so consecutive records are exactly
    `8 + len` apart and the record never overlaps the next one. Verified on
    `VegetationTranslucen...` at 0x20DD75D: len 48 = 4+2+2+38+2 terminator.
    """
    p = b.find(b"DIFF")
    n = len(b)
    while p >= 0:
        if p + 16 <= n:
            ln = struct.unpack_from("<I", b, p + 4)[0]
            fid, kind, cnt = struct.unpack_from("<IHH", b, p + 8)
            if 12 <= ln <= 1 << 16 and p + 8 + ln <= n and kind <= 4 and cnt <= 4096:
                yield (p, fid, kind, cnt, b[p + 16:p + 8 + ln])
                p = b.find(b"DIFF", p + 8)
                continue
        p = b.find(b"DIFF", p + 1)


def objects(b: bytes):
    """Group records into objects keyed by their name record.

    Exactly one record per object carries field id 0x1f4
    (BSComponentDB::CTName::Name). The name record sits in the MIDDLE of the
    group - a material object stores its float fields, THEN the name, THEN
    its persistent id - so a group runs from just after the previous name
    record up to and including this one. The trailing id record therefore
    lands in the next group; it carries no material data.
    """
    cur = []
    for rec in iter_records(b):
        off, fid, kind, cnt, pay = rec
        if fid == 0x01F4 and kind == 0 and cnt >= 3:
            t = pay[:cnt].split(b"\x00")[0]
            if t and all(32 <= c < 127 for c in t):
                cur.append(rec)
                yield t.decode("latin-1"), cur
                cur = []
                continue
        cur.append(rec)


def decode(kind: int, cnt: int, pay: bytes):
    """Decode a record payload.

    kind 0 + cnt>0 is a length-prefixed string. kind 0 + cnt 0 is an inline
    packed array: floats come as (f32 value, u16 indexOfNext) at stride 6,
    terminated by an index of 0xffff, and sub-structs as (u16 bitOffset,
    NUL-terminated name) followed by 0xffffffff. Verified on
    `ComTextileBlindsTranslucent05_Material1` whose override colour is
    `00 00 80 3f 01 00 | 00 00 80 3f 02 00 | 00 00 80 3f 03 00 | a7 e8 e8 3d
    ff ff` = (1, 1, 1, 0.1097).
    """
    if kind == 0 and cnt:
        return pay[:cnt].split(b"\x00")[0].decode("latin-1", "replace")
    if kind != 0 and len(pay) >= 4 * cnt:
        return [struct.unpack_from("<f", pay, 4 * i)[0] for i in range(cnt)]
    vals, i = [], 0
    while i + 6 <= len(pay):
        v = struct.unpack_from("<f", pay, i)[0]
        if v != v or abs(v) > 1e30:
            break
        vals.append(v)
        if struct.unpack_from("<H", pay, i + 4)[0] == 0xFFFF:
            break
        i += 6
    if vals:
        return vals
    out, i = [], 0
    while i + 2 <= len(pay):
        if pay[i:i + 4] == b"\xff\xff\xff\xff":
            break
        bit = struct.unpack_from("<H", pay, i)[0]
        j = pay.find(b"\x00", i + 2)
        if j < 0 or j - (i + 2) > 48:
            break
        out.append((bit, pay[i + 2:j].decode("latin-1", "replace")))
        i = j + 1
    if out:
        return out
    return pay[:24].hex()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--census", action="store_true")
    ap.add_argument("--name", default="")
    ap.add_argument("--dump", default="")
    ap.add_argument("--limit", type=int, default=40)
    a = ap.parse_args()
    b = S.load()

    if a.dump:
        for name, recs in objects(b):
            if a.dump.lower() in name.lower():
                print("== %s ==" % name)
                for off, fid, kind, cnt, pay in recs:
                    print("  0x%04x kind=%d cnt=%-4d %s"
                          % (fid, kind, cnt, decode(kind, cnt, pay)))
                break
        return 0

    if a.census:
        per = collections.defaultdict(lambda: [0, collections.Counter()])
        nobjects = 0
        for name, recs in objects(b):
            nobjects += 1
            for off, fid, kind, cnt, pay in recs:
                e = per[fid]
                e[0] += 1
                v = decode(kind, cnt, pay)
                e[1][repr(v)[:40]] += 1
        print("objects %d distinct field ids %d" % (nobjects, len(per)))
        for fid, (n, hist) in sorted(per.items(), key=lambda kv: -kv[1][0]):
            top = ", ".join("%s:%d" % kv for kv in hist.most_common(4))
            print("  0x%04x (%5d) n=%-7d %s" % (fid, fid, n, top))
        return 0

    hits = 0
    for name, recs in objects(b):
        if a.name.lower() not in name.lower():
            continue
        hits += 1
        if hits > a.limit:
            break
        print("== %s ==" % name)
        for off, fid, kind, cnt, pay in recs:
            print("  0x%04x kind=%d cnt=%-4d %s"
                  % (fid, kind, cnt, decode(kind, cnt, pay)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())