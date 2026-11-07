"""The offline OMOD assembly rule, pinned without needing a game install.

The renderer only draws a dummy-base gun because omod_assembly reproduces the
family binding CollectWeaponAssemblyNifs uses in game (an OMOD belongs to a
weapon when its MNAM keyword is on the weapon's KWDA list). These tests stub the
record walk, so they pin the *pick* - standard tier first, one per slot
otherwise - rather than re-reading a 1.4 GB master.
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "renderer_recon"))
sys.path.insert(0, str(TOOLS / "ba2_audit"))

import omod_assembly  # noqa: E402

RAZORBACK = {
    "kwda": {0x0FD6: [0x1F70BF, 0x0001AB]},
    "omods": [
        {"form": 0x02B402, "edid": "mod_Razorback_Grip_Standard",
         "model": "Weapons\\Razorback\\Grip.nif", "mnam": 0x1F70BF},
        {"form": 0x044C40, "edid": "mod_Razorback_Receiver_SemiAuto_Standard",
         "model": "Weapons\\Razorback\\Razorback.nif", "mnam": 0x1F70BF},
        {"form": 0x044C45, "edid": "mod_Razorback_Barrel_Standard",
         "model": "Weapons\\Razorback\\Razorback_LongBarrel.nif", "mnam": 0x1F70BF},
        {"form": 0x1298A4, "edid": "mod_Razorback_Optics_ReconScope",
         "model": "Weapons\\Razorback\\ReconScope.nif", "mnam": 0x1F70BF},
        {"form": 0x044C41, "edid": "mod_Razorback_Muzzle_None",
         "model": "", "mnam": 0x1F70BF},
        {"form": 0x099999, "edid": "mod_Other_Thing_Standard",
         "model": "Weapons\\Other\\Part.nif", "mnam": 0x00DEAD},
    ],
}
DUMMY = "Weapons\\Bullpup\\BullpupDummyReceiver.nif"


def stubbed(table: dict) -> "omod_assembly.Assemblies":
    made = omod_assembly.Assemblies(store=None)
    made._plugins["Starfield.esm"] = table
    return made


class OmodAssemblyTests(unittest.TestCase):
    def test_dummy_base_weapon_returns_its_standard_parts(self) -> None:
        parts = stubbed(RAZORBACK).nifs_for("Starfield.esm", 0x00000FD6, DUMMY)
        self.assertEqual(parts, [
            "Weapons\\Razorback\\Grip.nif",
            "Weapons\\Razorback\\Razorback.nif",
            "Weapons\\Razorback\\Razorback_LongBarrel.nif",
        ])

    def test_only_the_family_keyword_is_used(self) -> None:
        # The "Other" OMOD carries a different mnam: it is not this gun.
        parts = stubbed(RAZORBACK).nifs_for("Starfield.esm", 0x00000FD6, DUMMY)
        self.assertNotIn("Weapons\\Other\\Part.nif", parts)

    def test_a_real_model_is_left_alone(self) -> None:
        real = "Weapons\\Razorback\\Razorback.nif"
        self.assertEqual(stubbed(RAZORBACK).nifs_for("Starfield.esm", 0x00000FD6, real), [])

    def test_breech_block_also_assembles(self) -> None:
        table = {"kwda": dict(RAZORBACK["kwda"]), "omods": RAZORBACK["omods"]}
        parts = stubbed(table).nifs_for(
            "Starfield.esm", 0x00000FD6, "Weapons\\BreechBlock\\BreechBlock.nif")
        self.assertEqual(len(parts), 3)

    def test_without_a_standard_tier_one_part_per_slot_is_taken(self) -> None:
        table = {"kwda": {0x0FD6: [0x1F70BF]}, "omods": [
            {"form": 1, "edid": "mod_Gun_Barrel_Long", "model": "a.nif", "mnam": 0x1F70BF},
            {"form": 2, "edid": "mod_Gun_Barrel_Short", "model": "b.nif", "mnam": 0x1F70BF},
            {"form": 3, "edid": "mod_Gun_Grip_Cheap", "model": "c.nif", "mnam": 0x1F70BF},
        ]}
        parts = stubbed(table).nifs_for("Starfield.esm", 0x0FD6, DUMMY)
        self.assertEqual(parts, ["a.nif", "c.nif"])

    def test_a_weapon_with_no_keywords_has_no_assembly(self) -> None:
        self.assertEqual(stubbed({"kwda": {}, "omods": RAZORBACK["omods"]})
                         .nifs_for("Starfield.esm", 0x0FD6, DUMMY), [])

    def test_part_count_is_capped(self) -> None:
        omods = [{"form": i, "edid": f"mod_Gun_Std{i}_Standard", "model": f"{i}.nif",
                  "mnam": 0x1F70BF} for i in range(30)]
        parts = stubbed({"kwda": {0x0FD6: [0x1F70BF]}, "omods": omods}) \
            .nifs_for("Starfield.esm", 0x0FD6, DUMMY)
        self.assertEqual(len(parts), omod_assembly.MAX_PARTS)
        self.assertEqual(len(set(parts)), omod_assembly.MAX_PARTS)


if __name__ == "__main__":
    unittest.main()
