#!/usr/bin/env python3
"""Guard for the parent-index sidecar reader.

The sidecar is the offline half of the layered-material fix, and the only thing
that makes it safe is that its absence is a no-op. So the tests pin: the format
(comments, hex ids, a missing third column), the three spellings a material can
arrive under, the ancestor walk including a cycle, and that an ancestor with no
art lends nothing.
"""
from __future__ import annotations

import os
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE.parent / "ba2_audit"))

import parent_index  # noqa: E402

SIDECAR = """# osf-parent-index 1
# build: 1.16.244.0

271702\tAK_Kitchenware01_Base\t31297
67957\tAK_Kitchenware01_Rubber01\t271702
31297\tData\\MATERIALS\\SetDressing\\Kitchen\\AK_Kitchenware01.mat\t
175637\tAK_Kitchenware01_Wood01\t0x2AF95
"""


class FakeCdb:
    """Stands in for audit.Cdb: only own_set matters to the walk."""

    def __init__(self, sets: dict[str, dict]) -> None:
        self.sets = sets
        self.asked: list[str] = []

    def own_set(self, mat: str):
        key = parent_index.fold(mat)
        self.asked.append(key)
        found = self.sets.get(key)
        return (found, "own set") if found else (None, "no record")


class ParseTests(unittest.TestCase):
    def _write(self, text: str) -> str:
        fd, path = tempfile.mkstemp(suffix=".tsv")
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            fh.write(text)
        self.addCleanup(os.unlink, path)
        return path

    def test_comments_blank_lines_and_hex_ids(self) -> None:
        index = parent_index.load(self._write(SIDECAR))
        self.assertIsNotNone(index)
        self.assertEqual(len(index), 4)
        self.assertEqual(index.names[271702], "AK_Kitchenware01_Base")
        # 0x2AF95
        self.assertIn(175637, index.names)

    def test_a_row_without_a_parent_column_is_kept(self) -> None:
        index = parent_index.load(self._write(SIDECAR))
        self.assertEqual(index.names[31297][:4], "Data")
        self.assertNotIn(31297, index.parents)

    def test_missing_file_is_none_not_an_error(self) -> None:
        self.assertIsNone(parent_index.load(os.path.join(HERE, "no-such-sidecar.tsv")))

    def test_empty_file_is_none(self) -> None:
        self.assertIsNone(parent_index.load(self._write("# only a header\n")))


class WalkTests(unittest.TestCase):
    def setUp(self) -> None:
        fd, path = tempfile.mkstemp(suffix=".tsv")
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            fh.write(SIDECAR)
        self.addCleanup(os.unlink, path)
        self.index = parent_index.load(path)

    def test_a_name_and_its_mat_path_find_the_same_object(self) -> None:
        a = self.index.find("AK_Kitchenware01_Base")
        b = self.index.find("Data\\MATERIALS\\Kitchen\\AK_Kitchenware01_Base.mat")
        self.assertIsNotNone(a)
        self.assertEqual(a, b)

    def test_a_compiled_mat_object_tries_its_object_name_and_its_path(self) -> None:
        # The CDB spells a compiled .mat object `<path>.mat_<Object>`, so a
        # lookup has to try the object name and the file it came from.
        self.assertIn("blender1", parent_index.spellings("Data\\X\\Y.mat_Blender1"))
        self.assertIn("x/y.mat", parent_index.spellings("Data\\X\\Y.mat_Blender1"))

    def test_chain_walks_up_nearest_first(self) -> None:
        self.assertEqual(self.index.chain("AK_Kitchenware01_Rubber01"),
                         ["AK_Kitchenware01_Base",
                          "Data\\MATERIALS\\SetDressing\\Kitchen\\AK_Kitchenware01.mat"])

    def test_chain_stops_at_a_missing_parent(self) -> None:
        self.assertEqual(self.index.chain("AK_Kitchenware01_Base"),
                         ["Data\\MATERIALS\\SetDressing\\Kitchen\\AK_Kitchenware01.mat"])

    def test_chain_survives_a_cycle(self) -> None:
        index = parent_index.ParentIndex({1: "a", 2: "b"}, {1: 2, 2: 1})
        self.assertEqual(index.chain("a", limit=10), ["b"])

    def test_unknown_name_has_no_chain(self) -> None:
        self.assertEqual(self.index.chain("NotAMaterial01"), [])


class ColourTests(unittest.TestCase):
    def setUp(self) -> None:
        self.index = parent_index.ParentIndex(
            {1: "child", 2: "holder", 3: "root"},
            {1: 2, 2: 3})

    def test_nearest_ancestor_with_art_wins(self) -> None:
        cdb = FakeCdb({"holder": {"color": "textures/a_color.dds"}})
        hit = parent_index.colour_via_parent("child", cdb, self.index)
        self.assertEqual(hit, ("textures/a_color.dds", "holder"))
        self.assertEqual(cdb.asked, ["holder"])

    def test_an_empty_set_does_not_lend(self) -> None:
        cdb = FakeCdb({"holder": {"color": ""}, "root": {"color": "textures/r_color.dds"}})
        hit = parent_index.colour_via_parent("child", cdb, self.index)
        self.assertEqual(hit, ("textures/r_color.dds", "root"))

    def test_no_ancestor_with_art_returns_none(self) -> None:
        self.assertIsNone(parent_index.colour_via_parent("child", FakeCdb({}), self.index))

    def test_no_sidecar_is_a_no_op(self) -> None:
        cdb = FakeCdb({"holder": {"color": "textures/a_color.dds"}})
        self.assertIsNone(parent_index.colour_via_parent("child", cdb, None))
        self.assertEqual(cdb.asked, [])


if __name__ == "__main__":
    unittest.main()
