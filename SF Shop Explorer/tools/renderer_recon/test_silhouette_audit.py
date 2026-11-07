#!/usr/bin/env python3
"""Guards for the silhouette audit and the loose-file index it relies on."""
from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE.parent / "ba2_audit"))

import aid_colour_audit as audit  # noqa: E402
import silhouette_audit as silhouette  # noqa: E402


class ContourTests(unittest.TestCase):
    def test_one_island_is_one_contour(self) -> None:
        mask = Image.new("L", (24, 24), 0)
        ImageDraw.Draw(mask).rectangle([4, 4, 18, 18], fill=255)
        self.assertEqual(silhouette.contour_count(mask), 1)

    def test_two_islands_are_two_contours(self) -> None:
        mask = Image.new("L", (40, 20), 0)
        draw = ImageDraw.Draw(mask)
        draw.rectangle([3, 3, 14, 16], fill=255)
        draw.rectangle([25, 3, 36, 16], fill=255)
        self.assertEqual(silhouette.contour_count(mask), 2)

    def test_diagonal_pixels_are_one_contour(self) -> None:
        # Pillow draws diagonals as 8-connected steps; a loop must not be
        # counted twice because of a one-pixel diagonal seam.
        mask = Image.new("L", (12, 12), 0)
        draw = ImageDraw.Draw(mask)
        draw.line([(1, 1), (10, 10)], fill=255)
        self.assertEqual(silhouette.contour_count(mask), 1)


class ScanScopeTests(unittest.TestCase):
    def test_containers_are_in_the_default_scope(self) -> None:
        # Display cases are containers, not Misc; a scan that only walks the
        # seven item forms would miss the glass the rim treatment was added for.
        self.assertIn("CONT", silhouette.DEFAULT_TYPES)
        self.assertTrue(set(silhouette.TYPES) <= set(silhouette.DEFAULT_TYPES))

    def test_only_records_with_a_resolved_nif_extend_the_scope(self) -> None:
        self.assertTrue(silhouette.has_catalog_model({"model": "SetDressing\\Shelf.nif"}))
        self.assertTrue(silhouette.has_catalog_model({"nif_paths": ["a.nif"]}))
        self.assertFalse(silhouette.has_catalog_model({"model": ""}))
        self.assertFalse(silhouette.has_catalog_model({"model": "Clothes\\Suit"}))


class LooseIndexTests(unittest.TestCase):
    def test_index_preserves_root_order_and_finds_data_subdir(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            first = Path(tmp) / "first"
            second = Path(tmp) / "second"
            (first / "meshes").mkdir(parents=True)
            (second / "meshes").mkdir(parents=True)
            (first / "meshes" / "same.txt").write_text("first", encoding="utf-8")
            (second / "meshes" / "same.txt").write_text("second", encoding="utf-8")
            (first / "Data" / "meshes").mkdir(parents=True)
            (first / "Data" / "meshes" / "data.txt").write_text("data", encoding="utf-8")

            store = audit.AssetStore([first, second], None, False)
            store.index_loose()
            self.assertEqual(store.read("meshes/same.txt"), b"first")
            self.assertEqual(store.read("same.txt"), b"first")
            self.assertEqual(store.read("meshes/data.txt"), b"data")


if __name__ == "__main__":
    unittest.main()
