"""Decode the materialsbeta.cdb header: string table + TYPE/CLAS class schema.

Container layout (all little-endian), verified byte-by-byte against
`Abstract_Poster2x1_Material1`:

    0x00  "BETH"
    0x04  u32 8
    0x08  u32 4
    0x0c  u32 <string table end>
    0x10  "STRT" u32 count, then `count` NUL-terminated names
    ...   "TYPE" u32 version, u32 classCount, then classCount blobs, each
          "CLAS" u32 size + `size` bytes
    ...   "OBJT" ...
    ...   records: "DIFF" u32 len + payload, where payload is
          u32 fieldId, u16 kind, u16 count, then `count` elements.

A class blob is a list of 12-byte field descriptors:

    u32 fieldId, u16 typeCode, u16 flags(0xffff), u16 byteOffset, u16 byteSize

The offset/size pair is a packed field layout inside the component struct:
the alpha/opacity/blend fields therefore carry the exact byte width and the
exact stride, which is what tells us how to read a DIFF record's payload.

    python cdb_schema.py --list          # one line per class
    python cdb_schema.py --class 60      # field table for class 60
    python cdb_schema.py --find translucency
"""
from __future__ import annotations

import argparse
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
CDB = os.path.join(HERE, "extracted", "materialsbeta.cdb")

# typeCode -> (name, bytes) for the codes observed in the class blobs.
TYPES = {
    0x0000: ("inline", 0),
    0x0001: ("struct", 0),
    0x0002: ("string", 0),
    0xff02: ("?", 0),
    0xff10: ("bool", 1),
    0xff11: ("u32", 4),
    0xff12: ("u8", 1),
    0xff13: ("f32", 4),
}


def load(path: str = CDB) -> bytes:
    with open(path, "rb") as fh:
        return fh.read()


def names(b: bytes) -> list[str]:
    out, o = [], 0x18
    while True:
        j = b.find(b"\x00", o)
        if j < 0:
            break
        s = b[o:j]
        if any(c < 32 or c > 126 for c in s):
            break
        out.append(s.decode("latin-1"))
        o = j + 1
    return out


def classes(b: bytes) -> list[list[tuple]]:
    """Return per-class field descriptors, parsed from the TYPE section."""
    tag = b.find(b"TYPE", 0x2000, 0x4000)
    if tag < 0:
        raise SystemExit("TYPE section not found")
    _, _ver, count = struct.unpack_from("<III", b, tag)
    p = tag + 12
    out = []
    for _ in range(count):
        if b[p:p + 4] != b"CLAS":
            raise SystemExit("bad CLAS at 0x%x" % p)
        size = struct.unpack_from("<I", b, p + 4)[0]
        blob = b[p + 8:p + 8 + size]
        recs = []
        for k in range(0, len(blob) - 11, 12):
            fid, typ, flags, off, sz = struct.unpack_from("<IHHHH", blob, k)
            recs.append((fid, typ, flags, off, sz))
        out.append(recs)
        p += 8 + size
    return out


def hx(b: bytes, off: int, n: int) -> None:
    for i in range(0, n, 16):
        c = b[off + i:off + i + 16]
        if not c:
            break
        print("%08x  %-47s  |%s|" % (off + i, " ".join("%02x" % x for x in c),
                                      "".join(chr(x) if 32 <= x < 127 else "." for x in c)))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--class", dest="cls", type=int, default=-1)
    ap.add_argument("--find", default="")
    ap.add_argument("--hex", type=int, default=-1)
    a = ap.parse_args()

    b = load()
    cl = classes(b)
    if a.hex >= 0:
        hx(b, a.hex, 256)
        return 0
    if a.cls >= 0:
        for fid, typ, flags, off, sz in cl[a.cls]:
            tn, tw = TYPES.get(typ, ("0x%04x" % typ, 0))
            print("  id=0x%04x (%5d) type=%-7s/%dB flags=0x%04x off=%3d size=%d"
                  % (fid, fid, tn, tw, flags, off, sz))
        return 0
    if a.find:
        needle = a.find.lower()
        for i, nm in enumerate(names(b)):
            if needle in nm.lower():
                print("%3d %s" % (i, nm))
        return 0
    for i, recs in enumerate(cl):
        print("class %2d: %2d fields  ids=%s"
              % (i, len(recs), " ".join("0x%04x" % r[0] for r in recs[:8])))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())