#!/usr/bin/env python3
"""The shop catalog dump must publish real model paths, not placeholders.

`SFDevilzDump-catalog.csv` used to write `armor-addon-present` and
`weapon-data-present` into `modelPath`, which is a classification marker, not a
NIF. The MO2 audit therefore could not compare those 3,532 records against mod
overrides at all. The dump now resolves the ARMO's own biped models plus every
linked ARMA addon (male and female), the WEAP's BGSModelMaterialSwap model, and
the same carrier for the other sellable record types, with the complete list in
a new `nifPaths` column.

The test pins the dump source and the audit's parsing of that column.

    python -m unittest test_dump_catalog_paths -v
"""
from __future__ import annotations

import io
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import mo2_audit  # noqa: E402

DUMP = ROOT.parent / "SF Starfield MCM" / "src" / "Dump.cpp"


class DumpSourceTests(unittest.TestCase):
    def test_catalog_writes_every_resolved_nif(self) -> None:
        src = DUMP.read_text(encoding="utf-8")
        self.assertIn("renderCandidate,nifPaths", src)
        self.assertIn("ModelNifPathsOf", src)
        self.assertIn("armor->worldModels", src)
        self.assertIn("addon.armorAddon->bipedModel", src)
        self.assertIn("weap->BGSModelMaterialSwap::model", src)
        # The old markers told the audit nothing and must stay gone.
        self.assertNotIn('out = "armor-addon-present"', src)
        self.assertNotIn('out = "weapon-data-present"', src)


class CatalogPathTests(unittest.TestCase):
    def test_audit_reads_every_armor_path_from_nif_paths(self) -> None:
        row = {
            "formID": "00000D64",
            "type": "ARMO",
            "modelPath": "Armor\\Thing\\Male.nif",
            "nifPaths": ("Armor\\Thing\\Male.nif | "
                         "Armor\\Thing\\Female.nif | "
                         "Armor\\Thing\\Addon.nif | Armor\\Thing\\Addon.nif"),
        }
        self.assertEqual(mo2_audit.row_nifs(row, {}), [
            "meshes/armor/thing/male.nif",
            "meshes/armor/thing/female.nif",
            "meshes/armor/thing/addon.nif",
        ])

    def test_catalog_marks_armor_changed_when_one_addon_nif_is_a_mod(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / "profiles" / "P").mkdir(parents=True)
            (tmp / "profiles" / "P" / "modlist.txt").write_text("+SW\n", encoding="utf-8")
            mod = tmp / "mods" / "SW"
            (mod / "meshes" / "armor" / "thing").mkdir(parents=True)
            (mod / "meshes" / "armor" / "thing" / "female.nif").write_bytes(b"x")
            (tmp / "data").mkdir()
            catalog = tmp / "catalog.csv"
            catalog.write_text(
                "plugin,formID,type,name,editorId,modelPath,nifPaths\n"
                "A.esm,00000D64,ARMO,Thing,Thing,Armor\\Thing\\Male.nif,"
                "Armor\\Thing\\Male.nif | Armor\\Thing\\Female.nif\n",
                encoding="utf-8")
            out = tmp / "out.csv"
            args = type("Args", (), {
                "profile": "P", "catalog": str(catalog), "catalog_out": str(out),
                "log": "", "limit": 10,
            })()
            buf = io.StringIO()
            with redirect_stdout(buf):
                mo2_audit.run_catalog(args, ["SW"], tmp / "mods", str(tmp / "data"))
            self.assertIn("items any mod changes: 1", buf.getvalue())
            self.assertIn("yes", out.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
