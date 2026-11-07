"""The duplicate audit must agree with the catalog's own rule, exactly.

`audit_records.py` re-derives what `BuildFormList` (ShopExplorer.cpp) does, and
its whole value is being right about edge cases: if it grouped records the
plugin keeps apart, the audit would report rows that never disappear, and if it
kept apart records the plugin groups, a real duplicate would go unseen. Each
test below pins one clause of the rule with a hand-written dump.
"""
from __future__ import annotations

import csv
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import audit_records as audit  # noqa: E402

HEADER = ["plugin", "formID", "type", "name", "editorId", "value", "weight"]


def write_dump(rows: list[list]) -> Path:
    handle = tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False,
                                         newline="", encoding="cp1252")
    with handle as fh:
        writer = csv.writer(fh)
        writer.writerow(HEADER)
        writer.writerows(rows)
    return Path(handle.name)


def group(rows: list[list], include_free: bool = False) -> dict:
    records, unnamed = audit.load_dump(write_dump(rows), {"WEAP", "ARMO", "AMMO", "MISC", "ALCH", "BOOK", "KEYM"})
    return audit.build_groups(records, include_free)


class DuplicateRuleTests(unittest.TestCase):
    def test_same_name_value_and_weight_is_one_group(self) -> None:
        # The reported case: a master's gun and a mod's display clone of it.
        groups = group([
            ["Starfield.esm", "00000FD6", "WEAP", "Razorback", "Razorback", 5300, "2.300"],
            ["tailoredspaces.esm", "07000B0D", "WEAP", "Razorback", "SPP_TS_RazorbackTrophy", 5300, "2.300"],
        ])
        self.assertEqual(len(groups), 1)
        members = next(iter(groups.values()))
        self.assertEqual({m.plugin for m in members}, {"Starfield.esm", "tailoredspaces.esm"})
        self.assertTrue(audit.is_cross_plugin(members))

    def test_ammo_ignores_weight(self) -> None:
        # Starfield keeps the pre-ammo-weight record beside its replacement.
        groups = group([
            ["Starfield.esm", "00230679", "AMMO", "11mm Caseless", "", 32, "0.000"],
            ["Starfield.esm", "002B5595", "AMMO", "11mm Caseless", "", 32, "0.004"],
        ])
        self.assertEqual(len(groups), 1)
        self.assertFalse(audit.is_cross_plugin(next(iter(groups.values()))))

    def test_other_types_keep_weight_in_the_key(self) -> None:
        # A heavier variant of the same name is a different thing to buy.
        groups = group([
            ["Starfield.esm", "00000001", "ARMO", "Space Suit", "", 100, "12.000"],
            ["Starfield.esm", "00000002", "ARMO", "Space Suit", "", 100, "18.500"],
        ])
        self.assertEqual(groups, {})

    def test_a_different_price_is_not_a_duplicate(self) -> None:
        groups = group([
            ["Starfield.esm", "00000001", "MISC", "Coffee Mug", "", 55, "0.250"],
            ["Starfield.esm", "00000002", "MISC", "Coffee Mug", "", 75, "0.250"],
        ])
        self.assertEqual(groups, {})

    def test_free_records_stay_apart(self) -> None:
        # Plot objects: 62 "Crew Member's Slate"s are 62 different slates, so the
        # rule needs a price before it treats two records as merchandise.
        rows = [
            ["Starfield.esm", "000B767D", "BOOK", "Crew Member's Slate", "", 0, "0.000"],
            ["Starfield.esm", "000F346A", "BOOK", "Crew Member's Slate", "", 0, "0.000"],
            ["Starfield.esm", "000F346B", "BOOK", "Lab Key", "", 0, "0.000"],
            ["Starfield.esm", "000F346C", "BOOK", "Lab Key", "", 0, "0.000"],
        ]
        self.assertEqual(group(rows), {})
        self.assertEqual(len(group(rows, include_free=True)), 2)

    def test_unnamed_records_never_group(self) -> None:
        rows = [
            ["Starfield.esm", "00000001", "AMMO", "(unnamed)", "", 0, "0.000"],
            ["Starfield.esm", "00000002", "AMMO", "", "", 0, "0.000"],
            ["Starfield.esm", "00000003", "AMMO", "(unnamed)", "", 100, "0.000"],
            ["Starfield.esm", "00000004", "AMMO", "(unnamed)", "", 100, "0.000"],
        ]
        self.assertEqual(group(rows, include_free=True), {})

    def test_survivor_is_the_earliest_plugins_newest_record(self) -> None:
        # Earliest plugin: the master's own definition, not a later clone.
        cross = next(iter(group([
            ["Starfield.esm", "00000FD6", "WEAP", "Razorback", "", 5300, "2.300"],
            ["tailoredspaces.esm", "07000B0D", "WEAP", "Razorback", "", 5300, "2.300"],
        ]).values()))
        self.assertEqual(audit.survivor(cross).fid, "00000FD6")
        # Same plugin: the appended record is the replacement for the one before.
        same = next(iter(group([
            ["Starfield.esm", "00230679", "AMMO", "11mm Caseless", "", 32, "0.000"],
            ["Starfield.esm", "002B5595", "AMMO", "11mm Caseless", "", 32, "0.004"],
        ]).values()))
        self.assertEqual(audit.survivor(same).fid, "002B5595")

    def test_plugin_rank_orders_full_medium_and_light_files(self) -> None:
        # Full files by top byte, then 0xFD medium, then 0xFE light, each by slot.
        ordered = [0x00000001, 0x07000001, 0xFD000001, 0xFD000002, 0xFE000001, 0xFE000010]
        ranks = [audit.plugin_rank(f) for f in ordered]
        self.assertEqual(ranks, sorted(ranks))

    def test_summary_counts_groups_and_rows(self) -> None:
        records = [
            audit.Record("Starfield.esm", 0x1234, "MISC", "Mug", "", 55, 0.25),
            audit.Record("Starfield.esm", 0x1235, "MISC", "Mug", "", 55, 0.25),
            audit.Record("Starfield.esm", 0x1236, "MISC", "Mug", "", 55, 0.25),
            audit.Record("Starfield.esm", 0x1237, "MISC", "Vase", "", 55, 0.25),
        ]
        stats = audit.summarise(audit.build_groups(records))
        self.assertEqual(stats["groups"], 1)      # one group of three mugs
        self.assertEqual(stats["surplus"], 2)     # two rows the shop would drop
        self.assertEqual(stats["largest"], 3)


if __name__ == "__main__":
    unittest.main()
