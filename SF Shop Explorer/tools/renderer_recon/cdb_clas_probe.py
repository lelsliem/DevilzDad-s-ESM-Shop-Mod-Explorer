"""Is the 12-byte CLAS descriptor really the start of the blob?

cdb_schema.py parses each CLAS blob as a flat list of 12-byte records:
    u32 fieldId, u16 typeCode, u16 flags, u16 byteOffset, u16 byteSize
If that is wrong by even one field - i.e. the blob really begins with a
header (a name index, a field count, a class index) - then EVERY field id
this project has inferred for three turns is read from the wrong offset,
and 6r-8d's "the descriptor carries no name index" conclusion is an
artefact of the misalignment rather than a property of the format.

The test is cheap and decisive: print the raw bytes of several blobs with
several plausible header sizes marked, and check whether size == n*12 for
each class.  If it does not, the extra bytes are the missing header.

    python cdb_clas_probe.py [--raw]
"""
from __future__ import annotations

import argparse
import collections
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CDB = os.path.join(HERE, "extracted", "materialsbeta.cdb")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--raw", action="store_true")
    a = ap.parse_args()

    with open(CDB, "rb") as fh:
        b = fh.read()

    tag = b.find(b"TYPE", 0x2000, 0x4000)
    _, ver, count = struct.unpack_from("<III", b, tag)
    print("TYPE at 0x%x version %d classCount %d" % (tag, ver, count))

    p = tag + 12
    blobs = []
    for ci in range(count):
        assert b[p:p + 4] == b"CLAS", "bad CLAS at 0x%x" % p
        size = struct.unpack_from("<I", b, p + 4)[0]
        blobs.append((p + 8, size, b[p + 8:p + 8 + size]))
        p += 8 + size
    print("section ends at 0x%x" % p)

    # --- the size test ------------------------------------------------
    print("\n== blob size vs 12*k ==")
    hist = collections.Counter()
    for off, size, blob in blobs:
        r = size % 12
        hist[r] += 1
    print("size %% 12 histogram: %s" % dict(hist))
    tail = collections.Counter()
    for off, size, blob in blobs:
        for h in (0, 4, 8, 12, 16):
            if size >= h and (size - h) % 12 == 0:
                tail[h] += 1
    print("classes whose size fits 12*k after a header of: %s" % dict(tail))

    if a.raw:
        print("\n== raw blob bytes for the first 12 classes ==")
        for off, size, blob in blobs[:12]:
            print("\nCLAS at 0x%x size %d  (size%%12=%d, (size-8)%%12=%d)"
                  % (off, size, size % 12, (size - 8) % 12))
            for i in range(0, min(len(blob), 96), 12):
                rec = blob[i:i + 12]
                fid, typ, fl, o, sz = struct.unpack_from("<IHHHH", rec, 0)
                print("   +%3d  %-24s  as12: id=0x%04x type=0x%04x fl=0x%04x off=%-4d sz=%-4d"
                      % (i, rec.hex(" "), fid, typ, fl, o, sz))

    # --- does a leading header look like a string index? ---------------
    # The pool is 456 strings. If the first u32 of each blob is an index
    # into it and the blobs are in pool order, that is the map.
    import cdb_schema as S
    pool = S.names(b)
    print("\n== first u32 of each blob, tested as a pool index ==")
    hits = 0
    samples = []
    for off, size, blob in blobs:
        if size < 4:
            continue
        v = struct.unpack_from("<I", blob, 0)[0]
        if v < len(pool):
            hits += 1
            if len(samples) < 12:
                samples.append((v, pool[v]))
    print("blobs whose first u32 is a valid pool index: %d / %d" % (hits, len(blobs)))
    for v, nm in samples:
        print("   %4d -> %s" % (v, nm))

    # --- or is the name index the LAST field of the previous record? ---
    print("\n== is the class name perhaps in the 4 bytes before the blob? ==")
    for off, size, blob in blobs[:8]:
        pre = b[off - 4:off]
        print("   pre=%s  first12=%s" % (pre.hex(" "), blob[:12].hex(" ")))

    # --- flags field check --------------------------------------------
    print("\n== flags distribution in the 12-byte records ==")
    flags = collections.Counter()
    for off, size, blob in blobs:
        for i in range(0, len(blob) - 11, 12):
            f = struct.unpack_from("<H", blob, i + 6)[0]
            flags[f] += 1
    print("  %s" % flags.most_common(10))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())