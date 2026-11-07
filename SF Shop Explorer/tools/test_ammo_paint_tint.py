"""The ammo crates' paint colour comes from the material database, not the map.

`.27 Caliber` drew a white lid and a yellow band where the game draws a yellow
lid and a blue-grey band. The albedo half of that was never missing: the shell
resolves to `textures/common/paint/paint01_color.dds`, which is plain light
grey paint (measured mean (197,198,197)), and the colour is the CDB's
MaterialOverrideColor - which `UploadModelTextures` threw away by resetting the
range to white as soon as the albedo landed.

Measured with `tools/ba2_audit/ammo_paint_probe.py` against the in-game crate:

    shape  PaintedMetalshine01_Orange  albedo (197,198,197)
           x tint raw                  (167,134,78)     <- too dark
           x tint linear->sRGB         (183,167,130)    <- matches the paint
    game  .27 CAL lid                  (201,186,118) lit / (189,173,102) front

So the rule is multiply, not replace, and the database colour has to be
converted from the engine's linear space into this pipe's sRGB bytes first.The material names a shared neutral paint map (that is why the colour has to
live somewhere else), so "has a colour map" cannot be the qualifier - and the
database does name `paint01_color` for `PaintedMetalShine01_Orange`. What
separates a paint from a layered composite is the layer kind: the ammo and
painted-metal materials record none (kind 0), the Razorback receiver records
"Multiply" (kind 1) with `(0.67,0.697,1.0,0.635)`. On top of that, the
resolved map must measure as a neutral base at upload. These tests pin both
conditions, because a preview-only path cannot be run without the game.

Second revision (6r-10), after the first rule produced no visible change. Shape
bounds settle which material is which: in `AmmoLaser.nif` the :3 Green shape is
the lid (z 0.05-0.09, 58% up-facing) and :2 DarkGreen the body (z 0-0.08, 5%
up-facing); in `Ammo27Caliber.nif` the lid is :0 Orange and the body is the
same DarkGreen. In game both bodies read blue-grey and the lids read cream
(.27, Orange) and pale green (3kV, Green). That only fits one rule:

  * a textureless crate part takes the shared neutral paint map - NOT the
    folder's crate atlas, which is what painted the body orange;
  * a near-neutral override is a shade and multiplies (Green -> pale green,
    Orange -> cream), while a saturated one (DarkGreen, spread 0.56) is a
    pigment for a different variant and does not.

Both halves are pinned below, including the corrected form of the earlier
"every crate looks like .27" rejection.
"""
from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OFFLINE = ROOT / "src" / "OfflineModel.cpp"
LIVE = ROOT / "src" / "LiveModel.cpp"


class PaintTintTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = OFFLINE.read_text(encoding="utf-8-sig")
        start = cls.source.index("bool Bc1MeanRgb(")
        end = cls.source.index("float Bc4Mean(", start)
        cls.helpers = cls.source[start:end]
        start = cls.source.index("bool UploadModelTextures(")
        end = cls.source.index("bool EnsureWhite(", start)
        cls.upload = cls.source[start:end]

    def test_paint_colour_excludes_layered_composite_weights(self) -> None:
        self.assertIn("(void)LiveModel::FindCdbAlpha(mesh.matPath, cdbAlpha, &cdbKind);", self.source)
        self.assertIn("range.cdbPaintTint = cdbKind == 0;", self.source)
        # Kind 1 (Multiply) / 2 (Lerp) must not reach the tint path.
        self.assertNotIn("cdbPaintTint = cdbKind != 1", self.source)

    def test_the_neutral_guard_decides_whether_the_colour_survives(self) -> None:
        self.assertIn("range.cdbPaintTint &&", self.upload)
        self.assertIn("IsNeutralPaintBase(albedoMean)", self.upload)
        # The white reset must stay the else branch: an unmeasured or coloured
        # base keeps the old behaviour exactly.
        paint = self.upload.index("IsNeutralPaintBase(albedoMean)")
        reset = self.upload.index("albedo carries color", paint)
        self.assertLess(paint, reset)

    def test_linear_to_srgb_conversion_is_applied_only_on_the_paint_path(self) -> None:
        self.assertIn("range.color[c] = LinearToSrgb(range.color[c]);", self.upload)
        paint = self.upload.index("LinearToSrgb(range.color[c])")
        reset = self.upload.index("albedo carries color", paint)
        self.assertLess(paint, reset)
        self.assertIn("0.0031308f", self.helpers)
        self.assertIn("1.0f / 2.4f", self.helpers)

    def test_neutral_base_bounds_are_the_measured_ones(self) -> None:
        # paint01_color (0.77,0.78,0.77) must pass; ammobox01_color
        # (0.44,0.35,0.21) - the dark interior atlas - must not.
        self.assertIn("mx > 0.55f", self.helpers)
        self.assertIn("(mx - mn) / mx < 0.12f", self.helpers)

    def test_only_bc_colour_formats_are_measured(self) -> None:
        self.assertIn("a_fmt != 71 && a_fmt != 72 && a_fmt != 77 && a_fmt != 78", self.helpers)
        self.assertIn("return false;", self.helpers)

    def test_drink_bag_override_keeps_its_raw_colour(self) -> None:
        # The tea's measured (0.426,0.426,0.426) darkening is a separate,
        # already-verified case and must not be converted by this rule.
        self.assertIn("range.cdbOverrideTint = IsCraftedDrinkBagMaterial(mesh.matPath);", self.source)
        self.assertNotIn("cdbOverrideTint = IsCraftedDrinkBagMaterial(mesh.matPath) &&", self.source)

    def test_textureless_crate_parts_take_the_shared_paint_map(self) -> None:
        """The crate folder's `ammobox01` set is the atlas, not a part's art.

        Binding it because a part is textureless painted the part with whichever
        atlas island it happened to sample: the 3kV LZR body measured
        (106,87,57) - the raw orange in the 2026-10-18 screenshot - while the
        game draws that body blue-grey on both .27 CAL and 3kV LZR.
        """
        live = LIVE.read_text(encoding="utf-8-sig")
        start = live.index("bool FindCdbMaterialLocked(")
        end = live.index("std::vector<ColorEntry> g_opacs;", start)
        resolver = live[start:end]
        self.assertIn('path.find("setdressing/ammobox01/")', resolver)
        self.assertIn('a_outColor = "textures/common/paint/paint01_color.dds";', resolver)
        # Scoped rule first; the folder family fallback stays for everything
        # else (the Miner Utility Outfit parts depend on it).
        crate = resolver.index('path.find("setdressing/ammobox01/")')
        family = resolver.index("cdb family set '{}' supplies")
        self.assertLess(crate, family)
        self.assertIn("crate part '{}' has no authored art", resolver)

    def test_crate_part_match_normalises_separators(self) -> None:
        """The gate matched '/' while the NIF strings use '\\'.

        Measured in the 20:09 session: the body still logged
        `paint tint 'AmmoBoxLarge_DarkGreen01.mat' kept (0.88,0.80,0.47)`, so
        the saturated pigment stayed on the crate body and every crate still
        drew with the .27 yellow band. Pin the normalisation, not just the
        folder substring.
        """
        start = self.source.index("bool IsAmmoCratePart(")
        body = self.source[start:self.source.index("\n\t}", start)]
        self.assertIn("LowerAscii(a_matPath)", body)
        self.assertIn("static_cast<char>(92)", body)   # backslash -> '/'
        self.assertIn("ch = '/'", body)
        slash = body.index('path.find("setdressing/ammobox01/")')
        self.assertLess(body.index("static_cast<char>(92)"), slash)

        def crate_part(path: str) -> bool:
            return "setdressing/ammobox01/" in path.lower().replace("\\", "/")

        # Both spellings must match; the game hands the helper the first.
        self.assertTrue(crate_part("Materials\\SetDressing\\AmmoBox01\\AmmoBoxLarge_DarkGreen01.mat"))
        self.assertTrue(crate_part("Materials/SetDressing/AmmoBox01/AmmoBoxLarge_DarkGreen01.mat"))
        self.assertFalse(crate_part("Materials\\SetDressing\\AmmoBox02\\AmmoBoxLarge_DarkGreen01.mat"))

    def test_saturated_crate_override_is_not_a_pigment(self) -> None:
        """The corrected half of the rejected rule.

        The first attempt gave every textureless crate part the paint map AND
        a saturated override, so crate bodies went .27-yellow. The kind gate
        now excludes the real layer (`AmmoBoxLarge_DarkGreen01` is a Lerp,
        kind 2) before any colour test, and the spread cut is a backstop at
        0.60 - it has to sit above .27's cream lid (0.562) and the lime lid
        (0.480).
        """
        self.assertIn("IsAmmoCratePart(mesh.matPath)", self.source)
        self.assertIn("mx - mn <= 0.60f", self.source)
        self.assertIn("range.cdbPaintTint = cdbKind == 0;", self.source)

    def test_the_shipped_database_backs_the_crate_rule(self) -> None:
        """Read the real CDB: the crate rule must fire on exactly these parts.

        Skipped when the extracted database is not on this machine, so the
        source-level tests above still run anywhere.
        """
        cdb = ROOT / "tools" / "renderer_recon" / "extracted" / "materialsbeta.cdb"
        if not cdb.is_file():
            self.skipTest("materialsbeta.cdb not extracted")
        import sys as _sys
        _sys.path.insert(0, str(ROOT / "tools" / "ba2_audit"))
        from material_sweep import build_cdb_index
        index = build_cdb_index(cdb)
        # Both crate parts are authored-and-empty: the rule replaces the atlas.
        self.assertEqual(index.get("ammoboxlarge_darkgreen01"), {})
        self.assertEqual(index.get("paintedmetalshine01_green"), {})
        # The decal sheet has its own art and must keep it.
        self.assertIn("ammoboxrevised_color.dds", index["ammobox01_revised"]["color"])
        # The atlas the rule refuses still exists - that is the whole point.
        self.assertIn("ammobox01_color.dds", index["ammobox01"]["color"])

    def test_the_rejected_rule_stays_documented(self) -> None:
        live = LIVE.read_text(encoding="utf-8-sig")
        resolver = live[live.index("bool FindCdbMaterialLocked("):
                        live.index("std::vector<ColorEntry> g_opacs;")]
        self.assertIn("Rejected rule", resolver)
        self.assertIn("AmmoBoxLarge_DarkGreen01", resolver)

    def test_each_range_logs_the_colour_it_actually_uses(self) -> None:
        # Atlas, tint, mask and UV footprint on one line, so the next colour
        # question is answered by reading the log instead of re-deriving it.
        self.assertIn("tint=({:.2f},{:.2f},{:.2f}) mask={} uv={}", self.upload)
        self.assertIn("range.color[0], range.color[1], range.color[2]", self.upload)
        self.assertIn('img.opacityBytes.empty() ? "none" : "yes"', self.upload)
        self.assertIn("range.uvSamples", self.upload)

    def test_lzr_lid_binds_the_crate_family_lid_material(self) -> None:
        """The LZR lid keeps the green paint's OWN database colour.

        AmmoLaser.nif names `PaintedMetalShine01_Green`, which stores
        (0.584,0.800,0.320); this pipe's linear->sRGB conversion turns that
        into (201,231,153), against the measured lid (202,229,162 top,
        98,136,65 shadowed front).

        An earlier build rebound the lid to `AmmoBoxPrimer_Lid` because a
        value-first scan had attributed Green's colour to that object. Read
        name-first, the primer lid is pale blue (0.600,0.698,0.881) and the
        remap would paint a lime lid blue, so the remap is gone.
        """
        self.assertNotIn("IsAmmoCrateLidPaint", self.source)
        self.assertNotIn("kAmmoCrateLidMaterial", self.source)
        # The lid keeps its own tint, so nothing overwrites base[] after the
        # tint assignment in the crate branch.
        crate = self.source[self.source.index("if (range.cdbPaintTint && IsAmmoCratePart"):]
        crate = crate[:crate.index("range.cdbPaintTint = mx - mn")]
        self.assertNotIn("base[0] =", crate)
        self.assertIn("each lid keeps", self.source)

    def test_the_lid_shades_survive_the_pigment_cut(self) -> None:
        # .27's cream lid is PaintedMetalshine01_Orange at spread 0.562, and
        # the LZR's lime lid is Green at 0.480. The body pigment the cut was
        # written for (AmmoBoxLarge_DarkGreen01) is a Lerp layer and never
        # reaches this branch. The cut therefore sits at 0.60, between the
        # brightest lid and the saturated ship/industrial pigments.
        cdb = ROOT / "tools" / "renderer_recon" / "extracted" / "materialsbeta.cdb"
        if not cdb.is_file():
            self.skipTest("materialsbeta.cdb not extracted")
        import sys as _sys
        _sys.path.insert(0, str(ROOT / "tools" / "renderer_recon"))
        import cdb_alpha_replica as replica
        rep = replica.scan(str(cdb))
        for key in ("paintedmetalshine01_green", "paintedmetalshine01_orange"):
            spread = max(rep[key][:3]) - min(rep[key][:3])
            self.assertLessEqual(spread, 0.60, f"{key} must survive the cut")
        # DarkGreen is excluded by its kind, not by its colour.
        self.assertIn(replica.scan_kinds(str(cdb)).get("ammoboxlarge_darkgreen01"), (1, 2))
        self.assertIn("mx - mn <= 0.60f", self.source)

    def test_family_lid_colour_is_the_measured_one(self) -> None:
        """Read the real CDB and compare against the measured in-game lid.

        Skipped when the extracted database is not on this machine.
        """
        cdb = ROOT / "tools" / "renderer_recon" / "extracted" / "materialsbeta.cdb"
        if not cdb.is_file():
            self.skipTest("materialsbeta.cdb not extracted")
        import sys as _sys
        _sys.path.insert(0, str(ROOT / "tools" / "renderer_recon"))
        import cdb_alpha_replica as replica
        rep = replica.scan(str(cdb))
        # The lid is drawn with the family paint, not with its own record: the
        # runtime rebinds IsAmmoCrateLidPaint to kAmmoCrateLidMaterial. The
        # measured (0.584, 0.800, 0.320) is PaintedMetalShine01_Green's own
        # colour; the old test asked for it under AmmoBoxPrimer_Lid, whose
        # record was the object serialized before the lid's name.
        lid = rep.get("paintedmetalshine01_green")
        self.assertIsNotNone(lid, "PaintedMetalShine01_Green missing from the database")
        for got, want in zip(lid[:3], (0.584, 0.800, 0.320)):
            self.assertAlmostEqual(got, want, places=2)
        # The primer lid keeps its own pale blue. It must not be mistaken for
        # the green family paint, which is what the old attribution did.
        primer = rep["ammoboxprimer_lid"]
        self.assertAlmostEqual(primer[0], 0.600, places=2)
        self.assertGreater(primer[2], primer[0], "primer lid is blue, not green")
        green = rep["paintedmetalshine01_green"]
        # The paint is strongly coloured; the neutral guard therefore lives on
        # the ALBEDO (the shared paint01_color map), never on the tint.
        self.assertGreater(max(green[:3]) - min(green[:3]), 0.30)

        def linear_to_srgb(c: float) -> float:
            return 12.92 * c if c <= 0.0031308 else 1.055 * c ** (1.0 / 2.4) - 0.055

        srgb = [linear_to_srgb(c) * 255.0 for c in lid[:3]]
        # Measured on the in-game 3kV lid, 2026-10-18: (202,229,162) top face.
        for got, want in zip(srgb, (202, 229, 162)):
            self.assertLess(abs(got - want), 12.0, f"{srgb} vs measured lid")



class NeutralNameBeatsChromaticOverrideTests(unittest.TestCase):
    """A material named Black/White/Grey/Silver is neutral by definition.

    Measured on the 2026-10-23 session: every snow globe's base is node 1,
    `PlasticPlainTextured02Black` (the only mesh below z = 0.02), and the build
    that logged its resolved colour reported (0.43,0.36,0.32) - a warm tan.
    Its own database record is 0.198 grey and the colour it was painted with
    exists in materialsbeta.cdb under exactly one other object,
    `Akila_CabinetGlass_Swap_GridGrey_Material1` (0.433,0.361,0.324), so the
    override reaching a plastic belongs to somebody else. The same value is
    what made the base brown where the engine's own render is dark neutral.

    The guard is the name, not the record: only a material whose name already
    gives a neutral colour (spread <= 0.02) may reject a chromatic override
    (spread > 0.06). Chromatic names keep their database colour - that is the
    Velocity / inhaler case the override path exists for.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cls.source = OFFLINE.read_text(encoding="utf-8-sig")
        start = cls.source.index("bool BuildGpuModel(")
        end = cls.source.index("bool BuildGpuModel", start + 10) if False else len(cls.source)
        cls.block = cls.source[start:cls.source.index("range.matPath = mesh.matPath;", start)]

    def test_the_guard_reads_the_name_colour_not_the_override(self) -> None:
        self.assertIn("const bool neutralName = nameTint && spreadOf(base) <= 0.02f;", self.block)
        self.assertIn("if (neutralName && spreadOf(tint) > 0.06f) {", self.block)
        # Spread, not luminance: a dark chromatic colour is still chromatic.
        self.assertIn("return std::max({ a[0], a[1], a[2] }) - std::min({ a[0], a[1], a[2] });", self.block)

    def test_the_rejected_override_keeps_the_name_colour(self) -> None:
        guard = self.block.split("if (neutralName && spreadOf(tint) > 0.06f) {", 1)[1]
        rejected, accepted = guard.split("else {", 1)
        self.assertNotIn("base[0] = tint[0];", rejected)
        self.assertIn("keeping", rejected)
        self.assertIn("base[0] = tint[0];", accepted[:400])
        self.assertIn("++cdbTintCandidates;", accepted[:400])

    def test_the_measured_globe_base_values(self) -> None:
        # Both sides of the guard, with the numbers the session logged.
        name_black = (0.17, 0.17, 0.18)
        override_grid_grey = (0.43, 0.36, 0.32)
        spread = lambda c: max(c) - min(c)
        self.assertLessEqual(spread(name_black), 0.02)
        self.assertGreater(spread(override_grid_grey), 0.06)
        # The drink case the override exists for must stay untouched.
        yellow_override = (1.0, 0.917, 0.0)
        name_yellow = (0.80, 0.72, 0.30)
        self.assertGreater(spread(name_yellow), 0.02, "a chromatic name is not neutral")
        self.assertGreater(spread(yellow_override), 0.06)


if __name__ == "__main__":
    unittest.main()
