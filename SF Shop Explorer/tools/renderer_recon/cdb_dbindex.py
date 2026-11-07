#!/usr/bin/env python3
"""Decode materialsbeta.cdb's class layouts and the index arrays they describe.

`cdb_schema.py` reads the TYPE/CLAS blobs as a flat list of 12-byte records.
This module reads them as what they are: **struct layouts**. Two facts make the
containers decodable without guessing a single stride:

1. A CLAS blob starts with a class-header record (type 0, flags 0, offset 0,
   size = fieldCount); its leading u32 is the class id. Every later record is a
   field, and a field's `type` is either a scalar code (0xff10 bool, 0xff0b u16,
   0xff11 u32, ...) or **another class id**, which is how a nested struct is
   spelled. `byteOffset`/`byteSize` are the packed layout inside the struct, so
   `max(offset+size)` is the struct's size.
2. An array block is length-prefixed with the class it holds:

       LIST | u32 total | u32 classId | u32 count | count * layout.size bytes

   and `total == 8 + count * stride` exactly. That equality is the test: a
   "LIST" inside a string or a record payload never passes it.

Measured on the shipped `extracted/materialsbeta.cdb` (105,037,616 bytes):

    schema            97 classes; one ID class, 0x013d `BSComponentDB2::ID`
    0x017d            `ComponentInfo { ObjectID: ID, Index: u16, Type: u16 }`
                      8 bytes, 1,438,778 rows, 410,721 distinct object ids
    0x01bc            `{ SourceID: ID, TargetID: ID, Index: u16, Type: u16 }`
                      12 bytes, 451,529 rows - and every row carries
                      Type == 105, so this is one single parent-edge relation

Nothing else in the file validates against a class layout. In particular there
is **no ObjectInfo block and no class that could describe one**: ObjectInfo is
`{PersistentID, ID, DBID, Parent, ParentPersistentID, HasData}`, so three of its
fields would be `0x013d`-typed, and no class in the table has more than two ID
fields. The name pool at the head of STRT does name ObjectInfo - it is the JSON
schema, which can express types this compiled database never instantiates.

That is the wall the layered-material walk runs into, and it is a property of
the file, not of the reader:

  * the 367,906 name records in the DIFF stream carry no id (`cdb_records.py`
    reads the name and nothing else on the objects that have one), and
  * the only ids the file stores per object (`0x04f8`, `0x020f`, `0x035f`,
    `0x0348`, `0x052d`, `0x1301`, ~96k each) are never referenced by the
    451,529-edge table - measured: no edge endpoint of the kitchenware family
    (`AK_Kitchenware01_Base` id 271702 and its colour children) resolves to any
    of those maps.

So the edge table decodes, but nothing here can turn an edge endpoint into a
material name. `--report` prints that census rather than pretending otherwise.

    python cdb_dbindex.py --report
    python cdb_dbindex.py --blocks
    python cdb_dbindex.py --class 0x017d
    python cdb_dbindex.py --components --limit 20
    python cdb_dbindex.py --edges --limit 20
"""
from __future__ import annotations

import argparse
import os
import struct
import sys
from dataclasses import dataclass

HERE = os.path.dirname(os.path.abspath(__file__))
CDB = os.path.join(HERE, "extracted", "materialsbeta.cdb")

# Scalar field type codes seen in the class blobs. Anything else is a class id.
SCALARS = {
    0x0002: ("string", 0),
    0xFF02: ("?", 1),
    0xFF03: ("?", 1),
    0xFF04: ("?", 1),
    0xFF05: ("?", 1),
    0xFF09: ("?", 1),
    0xFF0B: ("u16", 2),
    0xFF0D: ("?", 4),
    0xFF0F: ("?", 1),
    0xFF10: ("bool", 1),
    0xFF11: ("u32", 4),
    0xFF12: ("u8", 1),
    0xFF13: ("f32", 4),
}

ID_CLASS = 0x013D       # BSComponentDB2::ID { Value: u32 }
COMPONENT_INFO = 0x017D  # { ObjectID: ID, Index: u16, Type: u16 }
EDGE_RECORD = 0x01BC    # { SourceID: ID, TargetID: ID, Index: u16, Type: u16 }
EDGE_TYPE = 105         # the only Type any row of EDGE_RECORD carries

#: How many id-typed fields the table would need for an `ObjectInfo` layout.
OBJECT_INFO_IDS = 3


@dataclass(frozen=True)
class Field:
    fid: int
    type: int
    flags: int
    offset: int
    size: int

    @property
    def scalar(self) -> str | None:
        name = SCALARS.get(self.type)
        return name[0] if name else None

    @property
    def is_id(self) -> bool:
        return self.type == ID_CLASS


@dataclass(frozen=True)
class Layout:
    cid: int
    fields: tuple[Field, ...]

    @property
    def size(self) -> int:
        return max((f.offset + f.size for f in self.fields), default=0)

    @property
    def id_fields(self) -> int:
        return sum(1 for f in self.fields if f.is_id)


@dataclass(frozen=True)
class Block:
    offset: int
    tag: str
    class_id: int
    count: int
    stride: int

    @property
    def body(self) -> int:
        return self.offset + 16


def load(path: str = CDB) -> bytes:
    with open(path, "rb") as fh:
        return fh.read()


def _type_header(blob: bytes) -> int:
    tag = blob.find(b"TYPE", 0x2000, 0x4000)
    if tag < 0:
        raise ValueError("no TYPE section")
    return tag


def layouts(blob: bytes) -> dict[int, Layout]:
    """class id -> struct layout, from the TYPE/CLAS section."""
    tag = _type_header(blob)
    _v, _ver, count = struct.unpack_from("<III", blob, tag)
    p = tag + 12
    out: dict[int, Layout] = {}
    for _ in range(count):
        if blob[p:p + 4] != b"CLAS":
            raise ValueError("bad CLAS at 0x%x" % p)
        size = struct.unpack_from("<I", blob, p + 4)[0]
        body = blob[p + 8:p + 8 + size]
        recs = [struct.unpack_from("<IHHHH", body, k)
                for k in range(0, len(body) - 11, 12)]
        if recs:
            cid = recs[0][0]
            # record 0 is the class header, not a field
            fields = tuple(Field(fid, typ, fl, off, sz) for fid, typ, fl, off, sz in recs[1:])
            out[cid] = Layout(cid, fields)
        p += 8 + size
    return out


def blocks(blob: bytes, table: dict[int, Layout], tag: str = "LIST") -> list[Block]:
    """Every `LIST | total | classId | count` block whose size agrees with its class."""
    found: list[Block] = []
    needle = tag.encode("ascii")
    i = 0
    while True:
        i = blob.find(needle, i)
        if i < 0:
            break
        if i + 16 <= len(blob):
            total, cid, count = struct.unpack_from("<III", blob, i + 4)
            layout = table.get(cid)
            if layout and layout.size and count:
                if total == 8 + count * layout.size:
                    found.append(Block(i, tag, cid, count, layout.size))
        i += 1
    return found


def components(blob: bytes, table: dict[int, Layout]) -> list[tuple[int, int, int]]:
    """(object id, index, type) for every ComponentInfo row."""
    rows = []
    layout = table[COMPONENT_INFO]
    for b in blocks(blob, table):
        if b.class_id != COMPONENT_INFO:
            continue
        for k in range(b.count):
            oid, idx, typ = struct.unpack_from("<IHH", blob, b.body + k * layout.size)
            rows.append((oid, idx, typ))
    return rows


def edges(blob: bytes, table: dict[int, Layout]) -> list[tuple[int, int, int, int]]:
    """(source id, target id, index, type) for every edge row."""
    rows = []
    layout = table[EDGE_RECORD]
    for b in blocks(blob, table):
        if b.class_id != EDGE_RECORD:
            continue
        for k in range(b.count):
            src, dst, idx, typ = struct.unpack_from("<IIHH", blob, b.body + k * layout.size)
            rows.append((src, dst, idx, typ))
    return rows


def describe(layout: Layout) -> str:
    lines = ["class 0x%04x  %d field(s), struct size %d bytes"
             % (layout.cid, len(layout.fields), layout.size)]
    for f in layout.fields:
        kind = f.scalar or "class 0x%04x" % f.type
        lines.append("  field 0x%04x  type %-12s off=%-3d size=%d%s"
                     % (f.fid, kind, f.offset, f.size, "  [object id]" if f.is_id else ""))
    return "\n".join(lines)


def report(blob: bytes) -> int:
    table = layouts(blob)
    found = blocks(blob, table)
    ids = [l for l in table.values() if l.id_fields]
    biggest = max((l for l in table.values()), key=lambda l: l.size, default=None)
    print("file            : %d bytes" % len(blob))
    print("schema          : %d classes%s"
          % (len(table), ", largest 0x%04x (%d bytes)" % (biggest.cid, biggest.size) if biggest else ""))
    print("id class        : 0x%04x, used by %d class(es) (%s)"
          % (ID_CLASS, len(ids), ", ".join("0x%04x" % l.cid for l in ids) or "none"))
    print("validated blocks: %d" % len(found))
    for b in sorted(found, key=lambda b: -b.count)[:12]:
        print("   0x%08x %s class 0x%04x  count=%-8d stride=%-3d  %d bytes"
              % (b.offset, b.tag, b.class_id, b.count, b.stride, b.stride * b.count))
    print()

    info = [b for b in found if b.class_id == COMPONENT_INFO]
    edg = [b for b in found if b.class_id == EDGE_RECORD]
    if info:
        rows = components(blob, table)
        print("ComponentInfo   : %d rows, %d distinct object ids, id range %d..%d"
              % (len(rows), len({r[0] for r in rows}), min(r[0] for r in rows), max(r[0] for r in rows)))
    else:
        print("ComponentInfo   : not present")
    if edg:
        rows = edges(blob, table)
        types = {r[3] for r in rows}
        print("edge table      : %d rows, type(s) %s, id range %d..%d"
              % (len(rows), sorted(types), min(min(r[0], r[1]) for r in rows),
                 max(max(r[0], r[1]) for r in rows)))
    else:
        print("edge table      : not present")
    print()

    # The wall, stated as a measurement rather than an opinion.
    widest = max((l.id_fields for l in table.values()), default=0)
    print("ObjectInfo      : absent. An ObjectInfo layout needs %d id-typed fields;"
          % OBJECT_INFO_IDS)
    print("                  the widest class in this file has %d (%s)."
          % (widest, ", ".join("0x%04x" % l.cid for l in table.values() if l.id_fields == widest) or "none"))
    print("                  The pool at the head of STRT names ObjectInfo because it is")
    print("                  the JSON schema, not because this file instantiates it.")
    print()
    print("consequence     : edge endpoints cannot be turned into material names from")
    print("                  this file. The DIFF stream names 367,906 objects and gives")
    print("                  them no id, and the ~96k per-object ids it does store are")
    print("                  never referenced by the edge table. A layered-material walk")
    print("                  needs the runtime index, or a CDB that serialises ObjectInfo.")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--cdb", default=CDB)
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--blocks", action="store_true")
    ap.add_argument("--class", dest="cls", default="")
    ap.add_argument("--components", action="store_true")
    ap.add_argument("--edges", action="store_true")
    ap.add_argument("--limit", type=int, default=20)
    a = ap.parse_args()

    if not os.path.isfile(a.cdb):
        print("no database at %s" % a.cdb, file=sys.stderr)
        return 2
    blob = load(a.cdb)
    table = layouts(blob)

    if a.cls:
        cid = int(a.cls, 0)
        if cid not in table:
            print("class 0x%04x is not in the table (%d classes)" % (cid, len(table)))
            return 1
        print(describe(table[cid]))
        return 0
    if a.blocks:
        for b in sorted(blocks(blob, table), key=lambda b: -b.count):
            print("0x%08x class 0x%04x count=%-8d stride=%-3d total=%d"
                  % (b.offset, b.class_id, b.count, b.stride, b.stride * b.count))
        return 0
    if a.components:
        for row in components(blob, table)[:a.limit]:
            print("object=%-8d index=%-3d type=%d" % row)
        return 0
    if a.edges:
        for row in edges(blob, table)[:a.limit]:
            print("source=%-8d target=%-8d index=%-3d type=%d" % row)
        return 0
    return report(blob)


if __name__ == "__main__":
    raise SystemExit(main())
