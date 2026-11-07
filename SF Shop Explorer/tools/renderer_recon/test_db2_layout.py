#!/usr/bin/env python3
"""Guard for the DBFileIndex layout recovery.

Three kinds of test here, because the tool can be wrong in three ways:

* the pairing can put a name against the wrong offset - `_fields_from_slots` is
  exercised on the two descriptor shapes the engine actually emits (name first,
  and type-loaded-into-a-register first) plus a partial triple that must be
  dropped;
* the validation can be too permissive - a member past the end of its class, two
  members at one offset, or a member whose type overruns the class must all be
  refused, because the dumper is gated on this;
* the recovery can drift from the build it was verified against. The integration
  test pins the exact numbers for Starfield 1.16.244.0 and skips when the game is
  not installed. If it fails after a game update, the dump must be re-verified
  before the offsets are used again - that is the failure working, not a flake.
"""
from __future__ import annotations

import os
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import capstone  # noqa: E402
import db2_layout  # noqa: E402

RSP = capstone.x86.X86_REG_RSP


def slots(*writes: tuple[int, str, object]) -> dict:
    out = {}
    for disp, kind, value in writes:
        out[(RSP, disp)] = (kind, value)
    return out


class PairingTests(unittest.TestCase):
    def test_name_then_type_then_offset(self) -> None:
        # The shape DBFileIndex uses: `lea rdx, name; lea rcx, type; mov [rsp+A], off`.
        found = db2_layout._fields_from_slots(slots(
            (0xF8, "name", "Objects"),
            (0x100, "ptr", 0x597AC50),
            (0x108, "int", 0x38)))
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0]["name"], "Objects")
        self.assertEqual(found[0]["offset"], 0x38)
        self.assertEqual(found[0]["type_rva"], 0x597AC50)
        self.assertIsNone(found[0]["type_name"])

    def test_type_is_read_before_the_name(self) -> None:
        # ObjectInfo: the type is loaded into r9 and stored after the name block.
        found = db2_layout._fields_from_slots(slots(
            (0xC0, "name", "Parent"),
            (0xC8, "ptr", 0x5F893C0),
            (0xD0, "int", 0x10)))
        self.assertEqual([(f["name"], f["offset"]) for f in found], [("Parent", 0x10)])

    def test_a_named_type_is_kept_by_name(self) -> None:
        found = db2_layout._fields_from_slots(slots(
            (0x30, "name", "Kind"),
            (0x38, "name", "BSComponentDB2::Kind"),
            (0x40, "int", 0x08)))
        self.assertEqual(found[0]["type_name"], "BSComponentDB2::Kind")
        self.assertIsNone(found[0]["type_rva"])

    def test_a_partial_triple_is_dropped(self) -> None:
        # An offset with no name beside it is some other stack store, not a field.
        found = db2_layout._fields_from_slots(slots(
            (0x108, "int", 0x38),
            (0x118, "name", "orphan"),
            (0x120, "ptr", 0x1234),
            (0x128, "int", 0x40)))
        self.assertEqual([(f["name"], f["offset"]) for f in found], [("orphan", 0x40)])

    def test_the_descriptor_copy_does_not_duplicate_the_field(self) -> None:
        write = ((0xC0, "name", "Edges"), (0xC8, "ptr", 0x1), (0xD0, "int", 0x68))
        copy = ((0x1E0, "name", "Edges"), (0x1E8, "ptr", 0x1), (0x1F0, "int", 0x68))
        found = db2_layout._fields_from_slots(slots(*(write + copy)))
        self.assertEqual(len(found), 1)

    def test_fields_come_back_in_offset_order(self) -> None:
        found = db2_layout._fields_from_slots(slots(
            (0x50, "name", "b"), (0x58, "ptr", 1), (0x60, "int", 0x14),
            (0x80, "name", "a"), (0x88, "ptr", 1), (0x90, "int", 0x0c)))
        self.assertEqual([f["offset"] for f in found], [0x0C, 0x14])


class RegisterTests(unittest.TestCase):
    def test_a_32_bit_zero_is_the_same_register_as_its_64_bit_form(self) -> None:
        # r14d and r14 are one storage location: recovering no field at offset 0
        # was exactly this bug for ObjectInfo.PersistentID and ComponentInfo.ObjectID.
        for narrow, wide in ((capstone.x86.X86_REG_R14D, capstone.x86.X86_REG_R14),
                             (capstone.x86.X86_REG_R15D, capstone.x86.X86_REG_R15),
                             (capstone.x86.X86_REG_EAX, capstone.x86.X86_REG_RAX)):
            self.assertEqual(db2_layout._widen(narrow), wide)
        self.assertEqual(db2_layout._widen(capstone.x86.X86_REG_R14), capstone.x86.X86_REG_R14)


class ValidateTests(unittest.TestCase):
    def field(self, name: str, offset: int, size: int | None) -> dict:
        return {"name": name, "offset": offset,
                "type": {"kind": "info", "size": size, "name": "x"}}

    def test_a_consistent_struct_has_no_problems(self) -> None:
        fields = [self.field("a", 0x00, 4), self.field("b", 0x04, 4)]
        self.assertEqual(db2_layout._validate("T", 0x08, fields), [])

    def test_an_offset_past_the_end_is_a_problem(self) -> None:
        problems = db2_layout._validate("T", 0x08, [self.field("a", 0x0C, 4)])
        self.assertTrue(any("outside" in p for p in problems), problems)

    def test_two_members_at_one_offset_is_a_problem(self) -> None:
        problems = db2_layout._validate(
            "T", 0x10, [self.field("a", 0x04, 4), self.field("b", 0x04, 4)])
        self.assertTrue(any("share offset" in p for p in problems), problems)

    def test_a_type_that_overruns_the_class_is_a_problem(self) -> None:
        problems = db2_layout._validate("T", 0x10, [self.field("a", 0x0C, 8)])
        self.assertTrue(any("overrun" in p for p in problems), problems)


EXE = db2_layout.DEFAULT_EXE


@unittest.skipUnless(os.path.exists(EXE), "Starfield.exe is not installed here")
class RecoveredLayoutTests(unittest.TestCase):
    """The numbers a dumper would compile in, pinned to the build they came from."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.layout = db2_layout.recover(EXE)
        cls.classes = cls.layout["classes"]

    def offsets(self, name: str) -> dict[str, int]:
        return {f["name"]: f["offset"] for f in self.classes[name]["fields"]}

    def test_nothing_contradicts_the_layout(self) -> None:
        self.assertEqual(self.layout["problems"], [])

    def test_the_build_is_the_one_the_plugin_was_verified_against(self) -> None:
        self.assertEqual(self.layout["exe"]["file_version"], "1.16.244.0",
                         "a different game build: re-run db2_layout.py and re-verify before dumping")

    def test_dbfileindex_members(self) -> None:
        self.assertEqual(self.classes["BSComponentDB2::DBFileIndex"]["size"], 0x88)
        self.assertEqual(self.offsets("BSComponentDB2::DBFileIndex"),
                         {"ComponentTypes": 0x00, "Objects": 0x38,
                          "Components": 0x50, "Edges": 0x68, "Optimized": 0x80})

    def test_objectinfo_members(self) -> None:
        name = "BSComponentDB2::DBFileIndex::ObjectInfo"
        self.assertEqual(self.classes[name]["size"], 0x28)
        self.assertEqual(self.offsets(name),
                         {"PersistentID": 0x00, "DBID": 0x0C, "Parent": 0x10,
                          "ParentPersistentID": 0x14, "HasData": 0x25})

    def test_component_and_edge_records(self) -> None:
        invalid = "BSComponentDB2::DBFileIndex::ComponentInfo"
        edge = "BSComponentDB2::DBFileIndex::EdgeInfo"
        self.assertEqual(self.classes[invalid]["size"], 0x08)
        self.assertEqual(self.offsets(invalid), {"ObjectID": 0x00, "Index": 0x04, "Type": 0x06})
        self.assertEqual(self.classes[edge]["size"], 0x0C)
        self.assertEqual(self.offsets(edge),
                         {"SourceID": 0x00, "TargetID": 0x04, "Index": 0x08, "Type": 0x0A})

    def test_the_declared_field_count_matches_what_was_recovered(self) -> None:
        for name, cls in self.classes.items():
            if cls["declared_fields"] is None:
                continue
            self.assertEqual(len(cls["fields"]), cls["declared_fields"], name)

    def test_every_index_array_is_a_std_vector_of_its_record(self) -> None:
        boxes = self.layout["containers"]
        self.assertEqual(set(boxes), set(db2_layout.CONTAINER_ELEMENTS))
        for key, box in boxes.items():
            with self.subTest(key=key):
                self.assertEqual(box["type_name"], "std::vector")
                self.assertEqual(box["header_bytes"], 0x18)
                self.assertEqual(box["evidence"]["header"], [0, 8, 0x10])
                self.assertTrue(box["evidence"]["is_vector"])
                self.assertEqual(box["element_size"],
                                 self.classes[box["element"]]["size"])

    def test_objectinfo_parent_shares_the_id_types_of_its_neighbours(self) -> None:
        # The reason the earlier DIFF-stream join could not work: `Parent` is a
        # DBID (the same type-info as `DBID`), `ParentPersistentID` is a
        # PersistentID (the same type-info as `PersistentID`). Two id spaces in
        # one record - joining edges to DIFF names mixed them.
        fields = {f["name"]: f["type"] for f in self.classes[
            "BSComponentDB2::DBFileIndex::ObjectInfo"]["fields"]}
        self.assertEqual(fields["Parent"]["rva"], fields["DBID"]["rva"])
        self.assertEqual(fields["ParentPersistentID"]["rva"], fields["PersistentID"]["rva"])


if __name__ == "__main__":
    unittest.main()
