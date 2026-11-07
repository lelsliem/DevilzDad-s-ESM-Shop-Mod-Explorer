"""The Aid shelf's colour comes from the database, not from a stand-in map.

Six items were still unchanged after the layer-kind join (release 0.8.304):
Tranquilitea, Hypergiant Heart, Velocity Yellow, Cheese, Erdebrau Light Can
and Captain Galaxy's Note 02. The causes measured here, one by one, are not
the layered-blend join - they are the two places that run *after* it:

  * a stand-in albedo threw the authored override colour away. The
    Tranquilitea cup's base stores teal (0.139,0.764,0.810) and the
    Hypergiant inhaler's ampoule stores yellow-green (0.983,1.0,0.0), but
    the shipped database names no colour map for either material, so the
    family scorer substituted a neighbour's atlas and
    `UploadModelTextures` reset the range to white;
  * the override alpha was still read as surface opacity when no layer kind
    was recorded. The cup's base and lid both store 0.607843 (= 155/255)
    and the Velocity beverage stores 0.501961 (= 128/255); the engine draws
    all three opaque, and a byte-exact value shared by two unrelated
    surfaces is a layer weight, not physical transparency.

Velocity Yellow adds a third: its only map is `grungeoilslick01_color`
(a blue/purple oil slick) while the database authors the surface
(1.0,0.917,0.0) yellow, so multiplying the two made the bottle pale.

The Erdebrau Light can's label has files named after the *decal material*
(`erdebrau light_decal_color.dds`) but no database set for that material, so
it needs an exact route like the Tranquilitea sleeve's.

These tests pin the rules at source level and, when the extracted database is
on the machine, against the shipped values themselves.
"""
from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OFFLINE = ROOT / "src" / "OfflineModel.cpp"
LIVE = ROOT / "src" / "LiveModel.cpp"
CDB = ROOT / "tools" / "renderer_recon" / "extracted" / "materialsbeta.cdb"


def _alpha_block() -> str:
    source = OFFLINE.read_text(encoding="utf-8-sig")
    start = source.index("float MaterialBlendAlpha(")
    return source[start:source.index("bool BuildGpuModel(", start)]


def _upload_block() -> str:
    source = OFFLINE.read_text(encoding="utf-8-sig")
    start = source.index("bool UploadModelTextures(")
    return source[start:source.index("bool EnsureWhite(", start)]


class ConsumableAlphaTests(unittest.TestCase):
    def test_consumable_materials_ignore_the_override_alpha(self) -> None:
        block = _alpha_block()
        self.assertIn('ml.find("/fooddrink_singleitems/")', block)
        self.assertIn('ml.find("/drugs_or_medical/")', block)
        self.assertIn("const bool consumableItem", block)
        # The layer-weight branch must cover both families.
        self.assertIn("if ((garment || consumableItem) && !garmentSheerToken)", block)

    def test_glass_keeps_its_token_fallback(self) -> None:
        # `garmentSheerToken` is what still lets the Velocity bottle's glass
        # cap take the translucent pass instead of becoming opaque. The value
        # itself is 0 now (a clear shell: coverage comes from Fresnel), so the
        # guard is that the token still routes to `a_translucent = true` and
        # does NOT return the opaque 1.0 the decal/game branches return.
        block = _alpha_block()
        self.assertIn('ml.find("glass")', block)
        token_scan = block[block.index("static constexpr const char* kTokens[]"):]
        token_scan = token_scan[: token_scan.index("a_translucent = false;")]
        self.assertIn("a_translucent = true;", token_scan)
        self.assertIn("return 0.00f;", token_scan)
        self.assertNotIn("return 1.0f;", token_scan)

    def test_the_measured_alphas_are_byte_quantised(self) -> None:
        # Both values are exact n/255 weights, which is the evidence for the
        # rule above. If a future database shifts them, the comment is stale.
        self.assertAlmostEqual(155 / 255, 0.607843, places=6)
        self.assertAlmostEqual(128 / 255, 0.501961, places=6)


class StandInMapTests(unittest.TestCase):
    def test_a_stand_in_map_no_longer_erases_the_authored_tint(self) -> None:
        upload = _upload_block()
        # A borrowed colour-family set counts as a stand-in too.
        self.assertIn("const bool ownMap = ownSet && !borrowedFamily && !ownColor.empty()", upload)
        self.assertIn("authoredTint && !ownMap", upload)
        self.assertIn("LinearToSrgb(range.color[c])", upload)

    def test_grunge_overlays_do_not_erase_a_saturated_authored_colour(self) -> None:
        upload = _upload_block()
        self.assertIn('map.rfind("textures/common/grunge/", 0) == 0', upload)
        self.assertIn(">= 0.30f", upload)
        # The range must be left untextured, not merely recoloured.
        grunge = upload.index('map.rfind("textures/common/grunge/", 0) == 0')
        self.assertLess(grunge, upload.index("UploadTextureImage", grunge))

    def test_a_name_colour_survives_a_neutral_stand_in_map(self) -> None:
        # `Crafted_InHailerYellow` has no database record at all, so the name is
        # the only place its colour is written down. The fuzzy matcher hands it
        # `inhailer_liquid_decal_color.dds` - a greyscale bubble sheet (mean
        # 0.783, spread 0.000) - and drawing that at white made the Hypergiant
        # Heart's gold body a white panel with speckles (2026-10-23 capture).
        source = OFFLINE.read_text(encoding="utf-8-sig")
        self.assertIn("bool flatNameTint{ false };", source)
        self.assertIn("range.flatNameTint = nameTint;", source)
        # The database colour beats the name word, so the flag is cleared there.
        tint = source.split("LiveModel::FindCdbTint(mesh.matPath, tint)) {", 1)[1]
        tint = tint[: tint.index("++cdbTintCandidates")]
        self.assertIn("nameTint = false;", tint)
        upload = _upload_block()
        keep = upload.split("if (range.flatNameTint && !ownMap && neutralBase) {", 1)
        self.assertEqual(len(keep), 2, "the name-colour arm is missing")
        arm = keep[1][: keep[1].index("else if")]
        self.assertIn("name colour", arm)
        # It must not whiten: that reset is exactly the defect being fixed.
        self.assertNotIn("1.0f;", arm)
        # A neutral measurement is what licenses the rule.
        self.assertIn("neutralBase", upload)
        self.assertIn("IsNeutralPaintBase(albedoMean)", upload)

    def test_a_colour_named_variant_borrows_its_family_atlas(self) -> None:
        # The two case halves of every chem inhaler are `Crafted_InHailer.mat`
        # and `Crafted_InHailerYellow.mat`, and only the base has a database
        # record. The yellow half used to fall through to the fuzzy scorer and
        # got the ampoule's bubble sheet.
        live = LIVE.read_text(encoding="utf-8-sig")
        self.assertIn("bool ColourWordSplit(", live)
        self.assertIn("bool CdbColourFamilyBase(", live)
        resolve = live.split("const auto resolve = [&]()", 1)[1]
        resolve = resolve[: resolve.index("return s;")]
        self.assertIn("ColourWordSplit(stem, base)", resolve)
        # A material with its own record is never a borrow, whatever its name
        # ends in: the sibling's art must not replace a material's own map.
        body = live.split("bool CdbColourFamilyBase(", 1)[1]
        body = body[: body.index("std::string base;")]
        self.assertIn("g_cdbSets.count(stem)", body)
        upload = _upload_block()
        self.assertIn("LiveModel::CdbColourFamilyBase(range.matPath, familyBase)", upload)
        self.assertIn("!borrowedFamily", upload)

    def test_erdebrau_light_label_has_an_exact_route(self) -> None:
        live = LIVE.read_text(encoding="utf-8-sig")
        self.assertIn("erdebrau_light_decal.mat", live)
        self.assertIn("erdebrau light_decal_color.dds", live)


class ShippedDatabaseTests(unittest.TestCase):
    def setUp(self) -> None:
        if not CDB.is_file():
            self.skipTest("materialsbeta.cdb not extracted")
        import sys
        sys.path.insert(0, str(ROOT / "tools" / "renderer_recon"))
        import cdb_alpha_replica as replica
        self.rep = replica.scan(str(CDB))
        self.kinds = replica.scan_kinds(str(CDB))

    def test_tranquilitea_cup_is_teal_with_a_shared_weight(self) -> None:
        base = self.rep["tranquilitea_togocup_base01"]
        top = self.rep["tranquilitea_togocup_top01"]
        for got, want in zip(base[:3], (0.139, 0.764, 0.810)):
            self.assertAlmostEqual(got, want, places=2)
        # Two unrelated surfaces, one byte-exact alpha: a layer weight.
        self.assertAlmostEqual(base[3], 155 / 255, places=5)
        self.assertAlmostEqual(top[3], 155 / 255, places=5)
        self.assertIsNone(self.kinds.get("tranquilitea_togocup_base01"))

    def test_velocity_yellow_is_the_authored_yellow(self) -> None:
        ov = self.rep["velocity_beverage_yellow"]
        for got, want in zip(ov[:3], (1.0, 0.917, 0.0)):
            self.assertAlmostEqual(got, want, places=2)
        self.assertAlmostEqual(ov[3], 128 / 255, places=5)
        self.assertIsNone(self.kinds.get("velocity_beverage_yellow"))

    def test_the_inhaler_body_material_has_no_record(self) -> None:
        # The premise of the name-colour rule: the base database names every
        # per-chem inhaler material but NOT the generic yellow body, so only the
        # filename token can colour it.
        self.assertIn("crafted_inhailer", self.rep)
        self.assertNotIn("crafted_inhaileryellow", self.rep)

    def test_yellow_body_borrows_the_case_atlas(self) -> None:
        # The real database: only the base half has a record, so the yellow
        # variant takes the case atlas through the colour word - not the
        # liquid decal sheet, which belongs to the ampoule material.
        import sys
        sys.path.insert(0, str(ROOT / "tools" / "renderer_recon"))
        import aid_colour_audit as audit
        cdb = audit.Cdb()
        own, how = cdb.own_set(r"Materials\Items\Drugs_Or_Medical\Crafted_InHailerYellow.mat")
        self.assertIsNotNone(own, how)
        self.assertTrue(how.startswith("colour-family"), how)
        self.assertTrue(own["color"].endswith("medical/inhaler_color.dds"), own["color"])
        liquid, _ = cdb.own_set(
            r"Materials\Items\Drugs_Or_Medical\Inhailers\Inhailer_Paramour.mat")
        self.assertTrue(liquid["color"].endswith("inhailer_liquid_decal_color.dds"), liquid)
        # The name still owns the colour: the base's red override is not read.
        self.assertIsNone(cdb.tint(r"Materials\Items\Drugs_Or_Medical\Crafted_InHailerYellow.mat"))

    def test_hypergiant_ampoule_carries_the_yellow_green(self) -> None:
        ov = self.rep["inhailer_amp"]
        self.assertAlmostEqual(max(ov[:3]) - min(ov[:3]), 1.0, places=2)
        self.assertGreater(ov[1], 0.9)
        self.assertLess(ov[2], 0.1)


if __name__ == "__main__":
    unittest.main()
