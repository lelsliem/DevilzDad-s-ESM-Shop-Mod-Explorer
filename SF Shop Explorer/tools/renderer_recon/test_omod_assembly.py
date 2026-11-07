"""The OMOD pick, without a game install.

The standard tier is not always a complete gun: Microgun marks its magazine,
grip and barrel standard but leaves the receiver plain. Picking only `_Standard`
then hands the placement resolver a set with no C-Receiver root, and every
accessory is drawn at the origin. These tests pin the receiver inclusion and the
no-duplicate rule.
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE.parent / "ba2_audit"))

import omod_assembly as omod  # noqa: E402

FAMILY = 0x001F70BF
DUMMY = "Weapons\\Test\\TestDummyReceiver.nif"


def omod_row(edid: str, model: str) -> dict:
    return {"form": 0, "edid": edid, "model": model, "mnam": FAMILY}


class ReceiverPickTests(unittest.TestCase):
    def assemblies(self, rows: list[dict]) -> omod.Assemblies:
        asm = omod.Assemblies(None)
        # The scan cache is the public seam used by nifs_for; seeding it keeps
        # this test on the pick logic instead of a plugin file.
        asm._plugins["test.esm"] = {"omods": rows, "kwda": {0x800: [FAMILY]}}
        return asm

    def test_a_non_standard_receiver_is_added_to_a_standard_set(self) -> None:
        asm = self.assemblies([
            omod_row("mod_Microgun_Mag_Standard", "mag.nif"),
            omod_row("mod_Microgun_Grip_Standard", "grip.nif"),
            omod_row("mod_Microgun_Barrel_Standard", "barrel.nif"),
            omod_row("mod_Microgun_Receiver_FullAuto", "receiver.nif"),
        ])
        self.assertEqual(asm.nifs_for("test.esm", 0x800, DUMMY),
                         ["receiver.nif", "mag.nif", "grip.nif", "barrel.nif"])

    def test_a_standard_receiver_is_not_added_twice(self) -> None:
        asm = self.assemblies([
            omod_row("mod_Razorback_Receiver_Standard", "receiver.nif"),
            omod_row("mod_Razorback_Barrel_Standard", "barrel.nif"),
        ])
        self.assertEqual(asm.nifs_for("test.esm", 0x800, DUMMY),
                         ["receiver.nif", "barrel.nif"])

    def test_a_non_dummy_model_is_not_assembled(self) -> None:
        asm = self.assemblies([
            omod_row("mod_Razorback_Receiver_Standard", "receiver.nif"),
        ])
        self.assertEqual(asm.nifs_for("test.esm", 0x800, "Weapons\\Rifle.nif"), [])


if __name__ == "__main__":
    unittest.main()
