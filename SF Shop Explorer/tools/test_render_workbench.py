"""The workbench's own logic, without a game or a GPU.

Rendering itself is covered by the contact-sheet regression test and by the
tool's own runs; what this file pins is the part that decides *what* gets drawn
and *what the page is told*: the selection rules, the pixel difference that
makes a change visible, and the two page modes (live vs self-contained file).
Getting the selection wrong silently renders the wrong items, which looks
exactly like a broken renderer.
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))

import render_workbench as workbench  # noqa: E402


class FakeRuntime:
    """Just enough of Runtime for select(): a catalog list and nothing else."""

    def __init__(self, items: list[dict]) -> None:
        self.items = items


CATALOG = [
    {"name": "Razorback", "type": "WEAP", "form": 0x00000FD6,
     "plugin": "Starfield.esm", "keywords": ""},
    {"name": "Razorback Penetrator Round", "type": "PROJ", "form": 0x00025FC4,
     "plugin": "Starfield.esm", "keywords": ""},
    {"name": "Coffee Mug", "type": "MISC", "form": 0x0003EDC8,
     "plugin": "Starfield.esm", "keywords": ""},
    {"name": "Taris Shipyard Coffee Mug", "type": "MISC", "form": 0x0004F206,
     "plugin": "Starfield.esm", "keywords": ""},
    {"name": "11mm Caseless", "type": "AMMO", "form": 0x002B5595,
     "plugin": "Starfield.esm", "keywords": ""},
]


def pick(**kwargs) -> dict:
    base = {"item": [], "form": [], "type": [], "search": "", "limit": 0}
    base.update(kwargs)
    return base


class SelectionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.runtime = FakeRuntime(CATALOG)

    def test_a_name_substring_matches_every_shelf(self) -> None:
        # "Razorback" is a weapon and a projectile; a name search is not a shelf.
        rows = workbench.select(self.runtime, pick(item=["Razorback"]))
        self.assertEqual([r["form"] for r in rows], [0x00000FD6, 0x00025FC4])

    def test_a_type_is_a_shelf(self) -> None:
        rows = workbench.select(self.runtime, pick(type=["MISC"]))
        self.assertEqual([r["form"] for r in rows], [0x0003EDC8, 0x0004F206])

    def test_type_and_name_narrow_together(self) -> None:
        rows = workbench.select(self.runtime, pick(type=["MISC"], search="Taris"))
        self.assertEqual([r["name"] for r in rows], ["Taris Shipyard Coffee Mug"])

    def test_a_form_id_is_exact_and_padded(self) -> None:
        rows = workbench.select(self.runtime, pick(form=["0xfd6"]))
        self.assertEqual(rows[0]["form"], 0x00000FD6)
        self.assertEqual(rows[0]["name"], "Razorback")

    def test_a_missing_form_still_renders_a_placeholder_row(self) -> None:
        # Asking for a record the catalog does not carry must not silently
        # redraw nothing: the tile says which form could not be found.
        rows = workbench.select(self.runtime, pick(form=["DEADBEEF"]))
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["form"], 0xDEADBEEF)
        self.assertIn("(form", rows[0]["name"])

    def test_duplicates_are_dropped_and_the_limit_applies_last(self) -> None:
        # A form and a name that resolve to the same record is one tile.
        rows = workbench.select(self.runtime, pick(form=["00000FD6"], item=["Razorback"]))
        self.assertEqual([r["form"] for r in rows], [0x00000FD6, 0x00025FC4])
        self.assertEqual(len(workbench.select(self.runtime, pick(type=["MISC"], limit=1))), 1)

    def test_limit_zero_means_no_cap(self) -> None:
        self.assertEqual(len(workbench.select(self.runtime, pick(type=["MISC"], limit=0))), 2)


class ShardTests(unittest.TestCase):
    def test_shards_partition_the_selection_without_overlap(self) -> None:
        rows = [{"form": n} for n in range(7)]
        parts = [workbench.shard_selection(rows, f"{n}/3") for n in range(3)]
        self.assertEqual([[r["form"] for r in part] for part in parts],
                         [[0, 3, 6], [1, 4], [2, 5]])
        self.assertEqual(sorted(r["form"] for part in parts for r in part), list(range(7)))

    def test_no_shard_is_the_whole_selection(self) -> None:
        rows = [{"form": 1}]
        self.assertIs(workbench.shard_selection(rows, ""), rows)

    def test_a_bad_shard_is_an_error_not_a_silent_skip(self) -> None:
        for bad in ("1", "3/3", "x/2", "0/0"):
            with self.assertRaises(SystemExit):
                workbench.shard_selection([], bad)


class SearchTests(unittest.TestCase):
    """The find box's ranking, without a server or an archive index."""

    def test_a_name_prefix_ranks_before_an_interior_match(self) -> None:
        rows = workbench.search_catalog(CATALOG, "razorback")
        self.assertEqual([r["name"] for r in rows],
                         ["Razorback", "Razorback Penetrator Round"])

    def test_a_formid_suffix_matches_the_padded_record(self) -> None:
        rows = workbench.search_catalog(CATALOG, "FD6")
        self.assertEqual(rows[0]["form"], "00000FD6")
        self.assertEqual(rows[0]["name"], "Razorback")

    def test_a_formid_is_hex_not_a_name(self) -> None:
        # "mug" has no hex digits, so it must never be read as a FormID.
        rows = workbench.search_catalog(CATALOG, "mug", 2)
        self.assertEqual([r["name"] for r in rows], ["Coffee Mug", "Taris Shipyard Coffee Mug"])  # word starts first

    def test_a_type_and_a_plugin_are_searchable(self) -> None:
        self.assertEqual([r["type"] for r in workbench.search_catalog(CATALOG, "ammo")],
                         ["AMMO"])
        self.assertEqual(len(workbench.search_catalog(CATALOG, "starfield.esm")),
                         len(CATALOG))

    def test_an_empty_query_lists_nothing_and_limit_caps(self) -> None:
        self.assertEqual(workbench.search_catalog(CATALOG, "   "), [])
        self.assertEqual(len(workbench.search_catalog(CATALOG, "starfield.esm", 2)), 2)


class DifferenceTests(unittest.TestCase):
    def tile(self, value: int) -> Image.Image:
        return Image.new("RGB", (16, 16), (value, value, value))

    def test_no_previous_image_is_not_a_zero_difference(self) -> None:
        # None means "new", which the page shows as a dash, not as "unchanged".
        self.assertIsNone(workbench.difference(None, self.tile(10)))

    def test_an_identical_redraw_is_zero(self) -> None:
        import io
        buffer = io.BytesIO()
        self.tile(10).save(buffer, format="PNG")
        self.assertEqual(workbench.difference(buffer.getvalue(), self.tile(10)), 0.0)

    def test_a_brightness_change_is_measured(self) -> None:
        import io
        buffer = io.BytesIO()
        self.tile(100).save(buffer, format="PNG")
        delta = workbench.difference(buffer.getvalue(), self.tile(80))
        self.assertAlmostEqual(delta, 20.0, delta=0.6)

    def test_a_resized_tile_is_not_compared(self) -> None:
        import io
        buffer = io.BytesIO()
        Image.new("RGB", (8, 8), (1, 1, 1)).save(buffer, format="PNG")
        self.assertIsNone(workbench.difference(buffer.getvalue(), self.tile(1)))


class PageTests(unittest.TestCase):
    def test_the_live_page_polls_and_has_the_controls(self) -> None:
        html = workbench.page_html(None, {"item": "Razorback"})
        self.assertIn("api/state", html)
        self.assertIn("api/select", html)
        self.assertIn("changed only", html)
        self.assertIn('const STATIC_SNAPSHOT = null;', html)

    def test_the_live_page_has_the_find_box_and_focus_panel(self) -> None:
        html = workbench.page_html(None, {"detailSize": 512})
        self.assertIn("api/search", html)
        self.assertIn("focusPanel", html)
        self.assertIn("id=\"focus\"", html)
        self.assertIn("Find an item", html)
        # Every focused record can be checked against the two reference sites.
        self.assertIn("starfield.fandom.com/wiki/Special:Search?query=", html)
        self.assertIn("starfielddb.com/?s=", html)

    def test_a_snapshot_page_is_self_contained(self) -> None:
        snapshot = {"version": 3, "phase": "idle", "items": [], "total": 0, "done": 0,
                    "size": 160, "label": "x", "log": [], "error": "", "drawn": 0,
                    "elapsed": 1.0}
        html = workbench.page_html(snapshot, None)
        self.assertIn('"version": 3', html)
        # Relative tiles, no polling, and the edit form hidden: the file is a
        # record of one pass, not a client of a server that is not running.
        self.assertIn("'tiles/' + image", html)
        self.assertIn("STATIC_SNAPSHOT", html)
        self.assertIn("<form id=\"pick\" hidden>", html)


if __name__ == "__main__":
    unittest.main()
