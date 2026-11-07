#!/usr/bin/env python3
"""Guard for the CDB class-layout / array-block decoder.

The decoder exists because the previous attempt at the layered-material walk
guessed strides and produced a graph that did not join. So the tests pin the
two facts that make it a decode instead of a guess: a block's declared size
must equal `8 + count * layout.size`, and every row of the 12-byte array must
carry the one edge type.

The last test is the finding itself: no class in this file has enough id-typed
fields to be an `ObjectInfo`, which is why the edge table cannot be bound to
material names. If a future CDB does serialise the index, that test fails and
the walk becomes possible - which is exactly what we want to know.
"""
from __future__ import annotations

import os
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import cdb_dbindex as db  # noqa: E402

PRESENT = os.path.isfile(db.CDB)


@unittest.skipUnless(PRESENT, "extracted/materialsbeta.cdb is not present")
class LayoutTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.blob = db.load()
        cls.table = db.layouts(cls.blob)

    def test_every_class_has_a_usable_layout(self) -> None:
        # Two classes (0x00e3, 0x20c8) are empty markers with no fields at all;
        # they are the only ones a block can never hold, which is why blocks()
        # skips a class whose size is 0 instead of dividing by it.
        self.assertEqual(len(self.table), 97)
        empty = [cid for cid, l in self.table.items() if not l.fields]
        self.assertEqual(sorted(empty), [0x00E3, 0x20C8])
        self.assertEqual(sum(1 for l in self.table.values() if l.size), 95)

    def test_the_id_class_is_a_single_u32(self) -> None:
        # BSComponentDB2::ID is { Value: u32 }; every object id in the file is
        # one of these, which is what makes ComponentInfo 8 bytes and the edge
        # record 12.
        ident = self.table[db.ID_CLASS]
        self.assertEqual(ident.size, 4)
        self.assertEqual(len(ident.fields), 1)

    def test_component_info_layout_is_id_plus_two_u16(self) -> None:
        layout = self.table[db.COMPONENT_INFO]
        self.assertEqual(layout.size, 8)
        self.assertEqual(layout.id_fields, 1)
        self.assertEqual([f.size for f in layout.fields], [4, 2, 2])

    def test_edge_layout_is_two_ids_plus_index_and_type(self) -> None:
        layout = self.table[db.EDGE_RECORD]
        self.assertEqual(layout.size, 12)
        self.assertEqual(layout.id_fields, 2)
        self.assertEqual([f.size for f in layout.fields], [4, 4, 2, 2])


@unittest.skipUnless(PRESENT, "extracted/materialsbeta.cdb is not present")
class BlockTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.blob = db.load()
        cls.table = db.layouts(cls.blob)
        cls.blocks = db.blocks(cls.blob, cls.table)
        cls.big = [b for b in cls.blocks if b.count > 1000]

    def test_a_block_never_contradicts_its_class_size(self) -> None:
        for b in self.blocks:
            self.assertEqual(b.stride, self.table[b.class_id].size)
            self.assertGreater(b.count, 0)

    def test_exactly_two_large_arrays_exist(self) -> None:
        # ComponentInfo and the single-relation edge table. Anything else that
        # looked like an array was a "LIST" inside a string or a payload.
        self.assertEqual(sorted(b.class_id for b in self.big),
                         sorted([db.COMPONENT_INFO, db.EDGE_RECORD]))

    def test_component_info_rows_decode_in_range(self) -> None:
        rows = db.components(self.blob, self.table)
        self.assertEqual(len(rows), 1438778)
        object_ids = {r[0] for r in rows}
        self.assertEqual(len(object_ids), 410721)
        self.assertEqual((min(object_ids), max(object_ids)), (8, 613651))

    def test_every_edge_row_is_the_same_relation(self) -> None:
        rows = db.edges(self.blob, self.table)
        self.assertEqual(len(rows), 451529)
        self.assertEqual({r[3] for r in rows}, {db.EDGE_TYPE})
        self.assertEqual({r[2] for r in rows}, {0})   # Index is always 0

    def test_no_layout_could_describe_object_info(self) -> None:
        # ObjectInfo{PersistentID, ID, DBID, Parent, ParentPersistentID, HasData}
        # needs three id-typed fields; the widest class here has two. This is
        # the measurement behind "the edge table cannot be named".
        self.assertEqual(max(l.id_fields for l in self.table.values()), 2)
        self.assertGreater(db.OBJECT_INFO_IDS, 2)


if __name__ == "__main__":
    unittest.main()
