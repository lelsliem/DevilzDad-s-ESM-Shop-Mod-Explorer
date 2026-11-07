#!/usr/bin/env python3
"""Tests for the MO2 overlay resolver and the catalog comparison.

The catalog mode answers "which shop items will look different with these
mods?" by resolving each item's model through the same order LiveModel uses:
loose mod files first (modlist priority), then Data, then mod archives, then
the base-game archives. These tests pin that order and the material chain.
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
import mo2_overlay  # noqa: E402
from mo2_overlay import (  # noqa: E402
    Overlay,
    log_nif_map,
    mat_albedo,
    material_paths,
    norm_key,
)

LIVE_MODEL = ROOT / "src" / "LiveModel.cpp"

MAT_JSON = b"""{
  "Filename" : "MATERIALS\\\\X\\\\Thing.mat",
  "Summary" : { "Layer1" : { "Textures" : {
      "Albedo" : { "File" : "Data\\\\Textures\\\\X\\\\thing_color.dds" }
  } } }
}"""


class KeyTests(unittest.TestCase):
    def test_norm_key_folds_separators_case_and_data_root(self) -> None:
        self.assertEqual(norm_key("Materials\\SWProps\\Datapad.mat"),
                         "materials/swprops/datapad.mat")
        self.assertEqual(norm_key("Data\\Textures\\X\\a.dds"), "textures/x/a.dds")
        self.assertEqual(norm_key("/Meshes//Thing.nif"), "meshes/thing.nif")

    def test_material_paths_only_mats(self) -> None:
        nif = b"\x00Materials\\Items\\DataSlate\\Screen_01.mat\x00textures/x/color.dds\x00"
        self.assertEqual(material_paths(nif),
                         ["Materials\\Items\\DataSlate\\Screen_01.mat"])

    def test_mat_albedo_summary(self) -> None:
        self.assertEqual(mat_albedo(MAT_JSON), "textures/x/thing_color.dds")
        self.assertEqual(mat_albedo(b'{"Summary":{"Textures":{"Normal":{"File":"Data\\\\Textures\\\\x\\\\n.dds"}}}}'), "")

    def test_mat_albedo_without_summary_uses_mrtexturefile(self) -> None:
        # Star Wars Blasters and Melee ships .mats with no Summary block;
        # only the BSMaterial::MRTextureFile component list names the maps.
        blob = (b'{"Objects":[{"Components":['
                b'{"Data":{"FileName":"Data\\\\Textures\\\\Weapons\\\\e5\\\\normal.DDS"}},'
                b'{"Data":{"FileName":"Data\\\\Textures\\\\Weapons\\\\e5\\\\color.DDS"}}'
                b']}]}')
        self.assertEqual(mat_albedo(blob), "textures/weapons/e5/color.dds")

    def test_mat_albedo_reads_the_generated_diffuse_suffix(self) -> None:
        # SGTexSet sheets (Rubber_Neoprene_TextureSet1_d.DDS) have no colour
        # word at all; `_n`/`_r`/`_l` siblings must never win.
        blob = (b'{"Objects":[{"Components":['
                b'{"Data":{"FileName":"Data\\\\TEXTURES\\\\Generated\\\\SGTexSet\\\\Rubber_Neoprene_TextureSet1_n.dds"}},'
                b'{"Data":{"FileName":"Data\\\\TEXTURES\\\\Generated\\\\SGTexSet\\\\Rubber_Neoprene_TextureSet1_d.dds"}},'
                b'{"Data":{"FileName":"Data\\\\TEXTURES\\\\Generated\\\\SGTexSet\\\\Rubber_Neoprene_TextureSet1_r.dds"}}'
                b']}]}')
        self.assertEqual(mat_albedo(blob),
                         "textures/generated/sgtexset/rubber_neoprene_textureset1_d.dds")

    def test_cpp_reaches_the_mrtexturefile_fallback(self) -> None:
        """A missing Summary block must not abort the material parse.

        0.8.327 broke out of FindMatAlbedo when the "albedo" key was absent,
        which made the MRTextureFile scan unreachable and left every Blender-
        exported material (Star Wars blasters, Clone Wars outfits) grey even
        though the engine rendered it textured. Keep that fallback live.
        """
        source = LIVE_MODEL.read_text(encoding="utf-8-sig")
        body = source[source.index("bool FindMatAlbedo("):]
        body = body[: body.index("g_albedoMemo.emplace")]
        self.assertNotIn("if (alb == std::string::npos)", body)
        self.assertIn('if (const auto alb = low.find("\\"albedo\\"");', body)
        self.assertIn('low.find("\\"filename\\"")', body)
        self.assertIn('base.compare(base.size() - 6, 6, "_d.dds")', body)


class OverlayTests(unittest.TestCase):
    def _tree(self, tmp: Path) -> Path:
        profiles = tmp / "profiles" / "P"
        profiles.mkdir(parents=True)
        # Reversed priority: first line wins. B beats A on the shared file.
        (profiles / "modlist.txt").write_text(
            "+B\n+A\n-Disabled\n", encoding="utf-8")
        mods = tmp / "mods"
        for name in ("A", "B"):
            (mods / name / "meshes").mkdir(parents=True)
            (mods / name / "meshes" / "shared.nif").write_bytes(name.encode())
        (mods / "A" / "meshes" / "only_a.nif").write_bytes(b"A")
        data = tmp / "data"
        (data / "meshes").mkdir(parents=True)
        (data / "meshes" / "data_only.nif").write_bytes(b"data")
        return mods

    def test_first_modlist_line_wins_loose(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            mods = self._tree(Path(tmp))
            ov = Overlay("P", mods, Path(tmp) / "data",
                         profiles_root=Path(tmp) / "profiles")
            src = ov.resolve("meshes\\shared.nif")
            self.assertEqual(src.kind, "mod-loose")
            self.assertEqual(src.mod, "B")
            self.assertEqual(ov.read("meshes/shared.nif"), b"B")

    def test_data_fallback_and_missing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            mods = self._tree(Path(tmp))
            ov = Overlay("P", mods, Path(tmp) / "data",
                         profiles_root=Path(tmp) / "profiles")
            self.assertEqual(ov.resolve("meshes/only_a.nif").mod, "A")
            self.assertEqual(ov.resolve("meshes/data_only.nif").kind, "data-loose")
            self.assertEqual(ov.resolve("meshes/nope.nif").kind, "missing")

    def test_disabled_mod_is_not_part_of_the_overlay(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            mods = self._tree(Path(tmp))
            ov = Overlay("P", mods, Path(tmp) / "data",
                         profiles_root=Path(tmp) / "profiles")
            self.assertNotIn("Disabled", ov.enabled)
            self.assertNotIn("Disabled", set(ov.loose.values()))


class CatalogTests(unittest.TestCase):
    def test_row_nifs_uses_placeholder_fallback(self) -> None:
        row = {"formID": "00000D64", "modelPath": "armor-addon-present"}
        by_form = {"00000D64": {"meshes/actors/x.nif"}}
        self.assertEqual(mo2_audit.row_nifs(row, by_form), ["meshes/actors/x.nif"])
        row2 = {"formID": "00000050", "modelPath": "Meshes\\Ammo\\Box.nif"}
        self.assertEqual(mo2_audit.row_nifs(row2, {}), ["meshes/ammo/box.nif"])
        # Form model fields omit the meshes/ root; bare names are unusable.
        row3 = {"formID": "00000051", "nifPaths": "Weapons\\Razorback\\Razorback.nif | Razorback.nif"}
        self.assertEqual(mo2_audit.row_nifs(row3, {}),
                         ["meshes/weapons/razorback/razorback.nif"])

    def test_catalog_flags_modified_model_and_detects_missing_albedo(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / "profiles" / "P").mkdir(parents=True)
            (tmp / "profiles" / "P" / "modlist.txt").write_text("+SW\n", encoding="utf-8")
            mod = tmp / "mods" / "SW"
            (mod / "meshes").mkdir(parents=True)
            (mod / "meshes" / "thing.nif").write_bytes(
                b"\x00Materials\\X\\Thing.mat\x00Materials\\X\\Gone.mat\x00")
            (mod / "materials" / "x").mkdir(parents=True)
            (mod / "materials" / "x" / "Thing.mat").write_bytes(MAT_JSON)
            (mod / "materials" / "x" / "Gone.mat").write_bytes(
                b'{"Summary":{"Textures":{"Albedo":{"File":"Data\\\\Textures\\\\X\\\\gone_color.dds"}}}}')
            (mod / "textures" / "x").mkdir(parents=True)
            (mod / "textures" / "x" / "thing_color.dds").write_bytes(b"dds")
            (tmp / "data").mkdir()
            catalog = tmp / "catalog.csv"
            catalog.write_text(
                "plugin,formID,type,name,editorId,modelPath\n"
                "A.esm,00000ABC,WEAP,Thing,Thing,Meshes\\Thing.nif\n",
                encoding="utf-8")
            out = tmp / "out.csv"
            args = type("Args", (), {
                "profile": "P", "catalog": str(catalog), "catalog_out": str(out),
                "log": "", "limit": 10,
            })()
            buf = io.StringIO()
            with redirect_stdout(buf):
                mo2_audit.run_catalog(args, ["SW"], tmp / "mods", str(tmp / "data"))
            text = buf.getvalue()
            self.assertIn("items any mod changes: 1", text)
            self.assertIn("albedo resolves nowhere", text)
            csv_text = out.read_text(encoding="utf-8")
            self.assertIn("yes", csv_text)
            self.assertIn("textures/x/thing_color.dds", csv_text)
            self.assertIn("textures/x/gone_color.dds", csv_text)


class LogMapTests(unittest.TestCase):
    def test_requests_collect_assembled_nifs(self) -> None:
        log = (
            "[I] SFShopExplorer direct: request 'A-300 [FormID 00000FD6]' (2 mesh(es))\n"
            "[I] SFShopExplorer NativeModel: MODEL PATH = 'Weapons\\A300\\A300.nif'\n"
            "[I] SFShopExplorer live: archive Something - Main.ba2 (3 entries)\n"
            "[I] SFShopExplorer NativeModel: assembly geometry path='Weapons\\A300\\Scope.nif' meshes=1\n"
            "[I] SFShopExplorer direct: request 'Other [FormID 00000050]' (1 mesh(es))\n"
            "[I] SFShopExplorer NativeModel: connect root 'Ammo\\Box.nif' (2 paths)\n"
        )
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "log.txt"
            p.write_text(log, encoding="utf-8")
            mapping = log_nif_map(p)
        self.assertEqual(mapping["00000FD6"],
                         {"meshes/weapons/a300/a300.nif", "meshes/weapons/a300/scope.nif"})
        self.assertEqual(mapping["00000050"], {"meshes/ammo/box.nif"})


class SourceContractTests(unittest.TestCase):
    def test_dds_header_pixel_format_offsets(self) -> None:
        src = LIVE_MODEL.read_text(encoding="utf-8")
        # Magic (4) + 72 header bytes: dwSize at 76, dwFourCC at 84.
        self.assertIn("std::memcpy(&pfSize, a_dds.data() + 76, 4);", src)
        self.assertIn("std::memcpy(&fourCC, a_dds.data() + 84, 4);", src)
        self.assertNotIn("std::memcpy(&pfSize, a_dds.data() + 72, 4);", src)

    def test_archive_lookup_normalizes_caller_key(self) -> None:
        src = LIVE_MODEL.read_text(encoding="utf-8")
        self.assertIn("const auto archiveKey = LowerAscii(a_key);", src)
        self.assertIn("ar->entries.find(archiveKey)", src)


if __name__ == "__main__":
    unittest.main()
