#!/usr/bin/env python3
"""Guard for the DBFileIndex locator.

The scanner is the only thing standing between the dumper and a wrong pointer, so
the tests are all about the predicate: a well-formed index is found, and every
lookalike is refused. Each decoy below is a mistake that would otherwise produce
a plausible-looking dump - a header that does not divide by its stride, capacity
below last, an edge array from a different structure, an empty vector.
"""
from __future__ import annotations

import os
import struct
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import db2_instance as inst  # noqa: E402
import db2_layout as LAYOUT  # noqa: E402

EXE = LAYOUT.DEFAULT_EXE
HAVE_EXE = os.path.exists(EXE)

# The measured layout, so a test failure names the real offsets rather than a guess.
LAYOUT_DOC = LAYOUT.recover(EXE) if HAVE_EXE else {
    "classes": {
        "BSComponentDB2::DBFileIndex": {
            "size": 0x88,
            "fields": [{"name": n, "offset": o} for n, o in
                       (("ComponentTypes", 0x00), ("Objects", 0x38),
                        ("Components", 0x50), ("Edges", 0x68),
                        ("Optimized", 0x80))],
        },
        "BSComponentDB2::DBFileIndex::ComponentTypeInfo": {"size": 0x10},
        "BSComponentDB2::DBFileIndex::ObjectInfo": {"size": 0x28},
        "BSComponentDB2::DBFileIndex::ComponentInfo": {"size": 0x08},
        "BSComponentDB2::DBFileIndex::EdgeInfo": {"size": 0x0c},
    },
}

BASE = 0x10000000


class Builder:
    """Lays out one index in a buffer: header, then the three arrays."""

    def __init__(self, objects=8, components=40, edges=64, types=3,
                 edge_index=0, edge_types=(105,), vector_aligned=True):
        self.objects = objects
        self.components = components
        self.edges = edges
        self.types = types
        self.edge_index = edge_index
        self.edge_types = edge_types
        self.aligned = vector_aligned
        self.buf = bytearray(0x2000)

    def scalar(self, off: int, value: int, size: int = 8) -> None:
        raw = value.to_bytes(size, "little")
        self.buf[off:off + size] = raw

    def header(self, off: int, count: int, stride: int, at: int) -> None:
        first = BASE + at
        last = first + count * stride
        cap = last + (stride if self.aligned else 5)  # 5 bytes breaks the stride rule
        for i, v in enumerate((first, last, cap)):
            self.scalar(off + i * 8, v)

    def build(self) -> tuple[int, bytes]:
        off_types, off_objects, off_components, off_edges = 0x00, 0x38, 0x50, 0x68
        at_types = 0x100
        at_objects = at_types + self.types * 0x10
        at_components = at_objects + self.objects * 0x28
        at_edges = at_components + self.components * 0x08
        self.header(off_types, self.types, 0x10, at_types)
        self.header(off_objects, self.objects, 0x28, at_objects)
        self.header(off_components, self.components, 0x08, at_components)
        self.header(off_edges, self.edges, 0x0c, at_edges)
        for i in range(self.edges):
            row = at_edges + i * 0x0c
            kind = self.edge_types[i % len(self.edge_types)]
            struct.pack_into("<IIHH", self.buf, row, 1000 + i, 2000 + i,
                             self.edge_index, kind)
        return BASE, bytes(self.buf)


def reader_for(*built: tuple[int, bytes]) -> inst.BufferReader:
    return inst.BufferReader({base: data for base, data in built})


class PredicateTests(unittest.TestCase):
    def test_a_well_formed_index_is_accepted(self) -> None:
        base, data = Builder().build()
        hit = inst.looks_like_index(reader_for((base, data)), base, LAYOUT_DOC)
        self.assertIsNotNone(hit)
        self.assertEqual((hit["objects"], hit["components"], hit["edges"], hit["edge_type"]),
                         (8, 40, 64, 105))

    def test_the_edge_sampler_reads_the_vectors_own_first_pointer(self) -> None:
        # The header says where the rows are; a candidate whose `first` points at
        # memory this process cannot read must fail rather than return garbage.
        base, data = Builder().build()
        reader = reader_for((base, data))
        head = bytearray(data)
        struct.pack_into("<Q", head, 0x68, 0x7FFF0000)   # outside every region
        hit = inst.looks_like_index(reader_for((base, bytes(head))), base, LAYOUT_DOC)
        self.assertIsNone(hit)

    def test_an_edge_index_other_than_zero_is_refused(self) -> None:
        base, data = Builder(edge_index=3).build()
        self.assertIsNone(inst.looks_like_index(reader_for((base, data)), base, LAYOUT_DOC))

    def test_two_edge_types_in_one_array_are_refused(self) -> None:
        base, data = Builder(edge_types=(105, 106)).build()
        self.assertIsNone(inst.looks_like_index(reader_for((base, data)), base, LAYOUT_DOC))

    def test_a_capacity_that_does_not_divide_by_the_stride_is_refused(self) -> None:
        base, data = Builder(vector_aligned=False).build()
        self.assertIsNone(inst.looks_like_index(reader_for((base, data)), base, LAYOUT_DOC))

    def test_empty_vectors_are_refused(self) -> None:
        for kwargs in ({"objects": 0}, {"components": 0}, {"edges": 0}):
            base, data = Builder(**kwargs).build()
            with self.subTest(**kwargs):
                self.assertIsNone(inst.looks_like_index(reader_for((base, data)), base,
                                                        LAYOUT_DOC))

    def test_capacity_below_last_is_refused(self) -> None:
        base, data = Builder().build()
        broken = bytearray(data)
        first = struct.unpack_from("<Q", broken, 0x38)[0]
        struct.pack_into("<Q", broken, 0x48, first)      # capacity == first
        self.assertIsNone(inst.looks_like_index(reader_for((base, bytes(broken))), base,
                                                LAYOUT_DOC))

    def test_a_magazine_of_ordinary_data_is_not_an_index(self) -> None:
        base = BASE
        blob = bytearray((i * 37) & 0xFF for i in range(0x2000))
        self.assertIsNone(inst.looks_like_index(reader_for((base, bytes(blob))), base,
                                                LAYOUT_DOC))


class ScanTests(unittest.TestCase):
    def test_scan_finds_an_index_that_is_not_at_the_region_start(self) -> None:
        base, data = Builder().build()
        padded = bytes(0x800) + data
        hits = inst.scan(reader_for((base, padded)), [(base, base + len(padded))], LAYOUT_DOC)
        self.assertEqual(len(hits), 1)
        self.assertEqual(hits[0]["address"], base + 0x800)

    def test_scan_finds_each_region_s_own_index(self) -> None:
        # One per loaded database - base game plus creations - so the dumper has to
        # enumerate, and the scanner must not stop at the first hit.
        base_a, data_a = Builder(edges=64).build()
        base_b, data_b = Builder(edges=32).build()
        reader = reader_for((base_a, data_a), (base_b + 0x40000000, data_b))
        hits = inst.scan(reader, [(base_a, base_a + len(data_a)),
                                  (base_b + 0x40000000,
                                   base_b + 0x40000000 + len(data_b))], LAYOUT_DOC)
        self.assertEqual(sorted(h["edges"] for h in hits), [32, 64])

    def test_scan_of_empty_memory_finds_nothing(self) -> None:
        self.assertEqual(inst.scan(reader_for(), [], LAYOUT_DOC), [])

    def test_the_minimum_edge_floor_is_honoured(self) -> None:
        base, data = Builder(edges=8).build()
        regions = [(base, base + len(data))]
        self.assertEqual(len(inst.scan(reader_for((base, data)), regions, LAYOUT_DOC,
                                       min_edges=8)), 1)
        self.assertEqual(inst.scan(reader_for((base, data)), regions, LAYOUT_DOC,
                                   min_edges=9), [])


@unittest.skipUnless(HAVE_EXE, "Starfield.exe is not present")
class ShippedBuildTests(unittest.TestCase):
    def test_the_class_has_no_rtti_and_no_static_construction(self) -> None:
        evidence = inst.static_evidence(EXE)
        self.assertFalse(evidence["index_is_polymorphic"])
        self.assertEqual(evidence["rtti_names_in_family"],
                         ["IDFixupVisitor@DBFileIndex@BSComponentDB2"])
        self.assertEqual(evidence["lea_sites_to_db_vtable"], 0)
        self.assertEqual(evidence["image_sections_holding_db_vtable_pointer"], 0)
        self.assertEqual(evidence["layout_problems"], [])

    def test_the_emitted_scanner_uses_the_recovered_offsets(self) -> None:
        evidence = inst.static_evidence(EXE)
        head, impl = inst._cpp(evidence, LAYOUT_DOC)
        for needle in ("kEdges               = 0x68",
                       "kObjects             = 0x38",
                       "kEdgeStride          = 0xc",
                       "kObjectStride        = 0x28",
                       "kEdgeType            = 0xa",
                       "FindAll()"):
            self.assertIn(needle, head + impl)
        self.assertIn("1.16.244.0", head)


if __name__ == "__main__":
    unittest.main()
