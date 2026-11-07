"""The C++ live model parser substitutes the shipped crate for absent AMMO crates.

The offline audit resolver already repoints an absent
`SetDressing\\ammo_crates\\type{a..f}_{medium,large,xlarge}.nif` placeholder at
the shipped `ammobox_01.nif`. The in-game previews use two C++ entry points -
`ParseNifMeshes` for the overlay's NativeModel and `BuildModels` for the OSF
preview host - so the alias lives in both parsers, after the record's own path
has failed to produce geometry.

These are source-structure assertions because the real path needs the game
open. What is pinned here is that the placeholder family is recognized in one
place, that both loaders retry only after the record's path failed, and that a
real or mod-supplied placeholder still wins.
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LIVE_H = ROOT / "src" / "LiveModel.h"
LIVE_CPP = ROOT / "src" / "LiveModel.cpp"

sys.path.insert(0, str(ROOT / "tools" / "ba2_audit"))

GAME_DATA = Path(r"B:\SteamLibrary\steamapps\common\Starfield\Data")
AMMOBOX_NIF = r"SetDressing\ammo_crates\ammobox_01.nif"


class AmmoCrateFallbackHelperTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.header = LIVE_H.read_text(encoding="utf-8-sig")
        cls.source = LIVE_CPP.read_text(encoding="utf-8-sig")
        start = cls.source.index("const char* AmmoCrateFallbackFor(")
        end = cls.source.index("// -----------------------------------------------------------------------", start)
        cls.helper = cls.source[start:end]

    def test_the_helper_is_declared_and_returns_a_path_only_for_placeholders(self) -> None:
        self.assertIn("AmmoCrateFallbackFor", self.header)
        self.assertIn("Returns nullptr for every other path", self.header)

    def test_the_fallback_is_the_shipped_generic_crate(self) -> None:
        self.assertIn(r'SetDressing\ammo_crates\ammobox_01.nif', self.helper)

    def test_the_placeholder_family_is_exactly_typea_to_typef_and_three_sizes(self) -> None:
        self.assertIn('base.compare(0, 4, "type")', self.helper)
        self.assertIn("family < 'a' || family > 'f'", self.helper)
        for size in ("medium.nif", "large.nif", "xlarge.nif"):
            self.assertIn(f'"{size}"', self.helper)
        self.assertNotIn('"small.nif"', self.helper)

    def test_matching_uses_the_basename_so_a_folder_override_cannot_change_it(self) -> None:
        self.assertIn('find_last_of("\\\\/")', self.helper)


class SingleNifParserFallbackTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = LIVE_CPP.read_text(encoding="utf-8-sig")
        start = cls.source.index("bool ParseNifMeshes(")
        end = cls.source.index("bool ParseNifConnects(", start)
        cls.parser = cls.source[start:end]

    def test_the_record_path_is_tried_before_the_shipped_crate(self) -> None:
        primary = self.parser.index("BuildSingleNif(key, a_out, totalVerts, false)")
        fallback = self.parser.index("AmmoCrateFallbackFor(a_modelPath)")
        retry = self.parser.index("BuildSingleNif(fallbackKey, a_out, totalVerts, false)")
        self.assertLess(primary, fallback)
        self.assertLess(fallback, retry)

    def test_the_retry_uses_the_normalized_fallback_key(self) -> None:
        self.assertIn("const auto fallbackKey = NormalizeNifKey(crate);", self.parser)
        self.assertIn("fallbackKey != key", self.parser)


class MultiNifParserFallbackTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = LIVE_CPP.read_text(encoding="utf-8-sig")
        start = cls.source.index("bool BuildModels(")
        end = cls.source.index("bool ReadAsset(", start)
        cls.parser = cls.source[start:end]

    def test_the_fallback_runs_only_after_the_records_own_path_failed(self) -> None:
        primary = self.parser.index("if (buildOne(key)) {")
        fallback = self.parser.index("AmmoCrateFallbackFor(path)")
        self.assertLess(primary, fallback)

    def test_a_real_placeholder_cannot_be_shadowed_by_the_fallback(self) -> None:
        # buildOne() returns true for a parsed placeholder and the loop
        # continues before the fallback branch is reached.
        self.assertIn("if (buildOne(key)) {\n\t\t\t\tcontinue;", self.parser)
        self.assertIn("used.insert(fallbackKey).second", self.parser)

    def test_the_fallback_goes_through_the_same_sibling_assembly(self) -> None:
        self.assertIn("buildOne(fallbackKey);", self.parser)
        self.assertIn("AssembleSiblings(a_key, a_out, totalVerts, sibKeys, declaredParts);",
                      self.parser)


@unittest.skipUnless(GAME_DATA.is_dir(), "Starfield Data directory not present")
class ShippedCrateGeometryTests(unittest.TestCase):
    def test_the_fallback_nif_has_drawable_shapes(self) -> None:
        import nif_uv  # noqa: PLC0415

        arch = nif_uv.Arch(GAME_DATA)
        key, _blob, nif, err = nif_uv.read_nif(arch, AMMOBOX_NIF)
        self.assertIsNotNone(nif, err)
        shapes = nif_uv.parse_shapes(_blob, nif)
        self.assertGreater(len(shapes), 0, f"{key} has no BSGeometry shapes")


if __name__ == "__main__":
    unittest.main()
