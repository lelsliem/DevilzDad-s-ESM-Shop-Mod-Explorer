"""The audit must answer with a Creation's own database, like the plugin does.

`EnsureCdbIndex` indexes the base blob and then every
`materials/creations/<name>/materialsbeta.cdb`, first writer wins. The audit
read only the base blob until 2026-10-23, so every Misc row whose material
came from a Creation reported "no record" - action-figure packaging (SFBGS00D)
and kibble (SFBGS050) were being judged as if the game had never heard of
them, on a shelf where 834 ranges are Creation material.

The fixtures are the shipped bytes (`extract_creation_cdb.py`), so these pin
what the game says rather than a paraphrase. SFBGS007 (104 MB, the only
Creation that carries override colours) is not extracted; its absense is
reported, not silently treated as an answer.

    python -m unittest tools.renderer_recon.test_creation_cdb_audit
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import aid_colour_audit as audit  # noqa: E402

CREATIONS = HERE / "extracted" / "creations"


class CreationAuditTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if not audit.CDB.is_file() or not (CREATIONS / "sfbgs050.cdb").is_file():
            raise unittest.SkipTest("CDB fixtures absent")
        cls.cdb = audit.Cdb()

    def test_creation_databases_are_indexed(self) -> None:
        self.assertGreaterEqual(len(self.cdb.creations), 6)
        # SFBGS050's own key - absent from the base game, and the reason a
        # kibble pouch reported "no record".
        self.assertIn("kibblefoodchunks", self.cdb.sets)
        self.assertIn("breechblock_receiver_2", self.cdb.sets)
        self.assertEqual(
            self.cdb.sets["breechblock_receiver_2"].get("color"),
            "textures/weapons/sfbgs050/breechblock/receiver_2_color.dds",
        )

    def test_leading_backslash_material_paths_reach_creation_records(self) -> None:
        # The shipped NIFs spell a few paths with a leading separator; the
        # plugin strips it before lookup and the audit must too, or SFBGS00D's
        # trim material stays invisible.
        mat = (r"\materials\setdressing\sfbgs00d\actionfigurepackaging"
               r"\freestarrangerfigure_packaging\actionfigurepackaging_trimcolor01.mat")
        own, how = self.cdb.own_set(mat)
        self.assertEqual(how, "own set")
        self.assertEqual(own.get("color"), "textures/common/metal/metalalu_color.dds")

    def test_base_game_answer_keeps_precedence(self) -> None:
        # IndexCdbBlob only fills a gap, so a Creation can never shadow the
        # base game. `grip` exists in both with different answers (the base
        # Regulator grip vs SFBGS050's allied-resonator grip).
        self.assertEqual(
            self.cdb.sets["grip"].get("color"),
            "textures/weapons/regulator/regulator_grip_color.dds",
        )
        # ...while a key only the Creation has is still reachable.
        self.assertEqual(
            self.cdb.sets["kibblefoodchunks"], {},
            "a Creation's textureless set must survive as a flat answer",
        )


if __name__ == "__main__":
    unittest.main()
