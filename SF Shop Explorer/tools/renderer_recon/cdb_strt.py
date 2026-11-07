"""Read the STRT string table exactly as the container declares it, and stop
there.  cdb_schema.names() walks from 0x18 until a non-printable byte,
which can run past the end of the table; the header carries the real end
offset, so use it and see whether the pool size changes.

    0x00 "BETH" | 0x04 u32 8 | 0x08 u32 4 | 0x0c u32 <string table end>
    0x10 "STRT" u32 count, then `count` NUL-terminated names
"""
from __future__ import annotations

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CDB = os.path.join(HERE, "extracted", "materialsbeta.cdb")


def main() -> int:
    with open(CDB, "rb") as fh:
        b = fh.read()
    assert b[:4] == b"BETH"
    a, bb, c = struct.unpack_from("<III", b, 4)
    print("header: 0x04=%d 0x08=%d 0x0c=%d (0x%x)" % (a, bb, c, c))
    print("magic at 0x10: %r" % b[0x10:0x14])
    (count,) = struct.unpack_from("<I", b, 0x14)
    print("declared string count: %d" % count)

    # walk exactly `count` NUL-terminated strings from 0x18
    o = 0x18
    names = []
    for i in range(count):
        j = b.find(b"\x00", o)
        if j < 0:
            print("ran out at string %d" % i)
            break
        names.append(b[o:j].decode("latin-1"))
        o = j + 1
    print("strings read: %d, table ends at 0x%x" % (len(names), o))
    print("0x0c says the string table ends at 0x%x  -> %s"
          % (c, "AGREES" if c == o else "DISAGREES by %d" % (o - c)))

    # what does cdb_schema.names() return?
    sys.path.insert(0, HERE)
    import cdb_schema as S
    loose = S.names(b)
    print("\ncdb_schema.names() returns %d strings" % len(loose))
    print("first %d agree with the exact read: %s"
          % (len(names), names == loose[:len(names)]))
    if len(loose) > len(names):
        print("EXTRA beyond the declared table (%d):" % (len(loose) - len(names)))
        for x in loose[len(names):]:
            print("   %r" % x)

    # composition of the exact pool
    import collections
    typish = [n for n in names if "::" in n]
    print("\nexact pool: %d names, %d contain '::', %d unique"
          % (len(names), len(typish), len(set(names))))
    dup = [n for n, k in collections.Counter(names).items() if k > 1]
    print("duplicates: %d %s" % (len(dup), dup[:10]))
    print("\nlast 12 exact names: %s" % names[-12:])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())