#!/usr/bin/env python3
"""Guard for the override-alpha translucency rule in MaterialBlendAlpha.

Reported in game: the Razorback renders as a see-through rifle. The cause is
neither the grip artwork nor the weapon framing - it is the preview's rule that
any CDB MaterialOverrideColor alpha below 1.0 means "draw this range in the
alpha-blended pass".

The database says otherwise for this material. Across materialsbeta.cdb 6,205
objects carry an override alpha below 1.0; 1,681 of them also declare a
TextureSetKind (field 0x22b), and only two kinds exist in the whole file -
"Multiply" (5,014 objects) and "Lerp" (277). Both name how a texture set
composites over the layer beneath it, so the alpha there is that layer's blend
weight, not the surface's opacity. A rifle receiver is a surface, and the
record walk confirms it stores no override at all (the "kind Multiply, alpha
0.635" pair once pinned here was the preceding object's record):

    Razorback_Receiver_Material1   no 0x22b, no 0x25a in either database
    Tranquilitea_ToGoCup_Base01    no kind, alpha 0.608 (real glass)
    ErdeBrau_Can_Material1         "Multiply" layer, own alpha 0.0

So kind and alpha must be joined by object name, not by which database they
came from, and a Multiply/Lerp material keeps its alpha out of the blend pass.
The cup keeps its alpha, which is what the glass needs.

Source-structure assertions plus data assertions against the shipped
databases (skipped when the extracts are not on this machine).
"""
from __future__ import annotations

import os
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))

LIVE = ROOT / "src" / "LiveModel.cpp"
LIVE_H = ROOT / "src" / "LiveModel.h"
OFFLINE = ROOT / "src" / "OfflineModel.cpp"


class CdbLayerKindTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.live = LIVE.read_text(encoding="utf-8-sig")
        cls.live_h = LIVE_H.read_text(encoding="utf-8-sig")
        cls.offline = OFFLINE.read_text(encoding="utf-8-sig")

    def test_kind_map_is_separate_from_the_override_colours(self) -> None:
        # The kind is authored in the base database and the colour may come
        # from a Creation Kit database, so the two cannot share one record.
        self.assertIn("std::unordered_map<std::string, std::uint8_t> g_cdbKind;", self.live)
        rec = self.live.split("struct CdbOverride", 1)[1]
        rec = rec[: rec.index("};") + 2]
        self.assertIn("float rgba[4]", rec)
        self.assertNotIn("layerKind", rec)

    def test_kind_is_read_in_the_alpha_pass_and_kept_without_an_alpha(self) -> None:
        # Record order inside one object is 0x1f4 (name) then 0x020f,
        # 0x022b (kind), [0x25a] - the NAME OPENS the object. A kind with no
        # alpha in the same blob is still information: the matching colour
        # lives in another database.
        body = self.live.split("float pendingRgba[4]", 1)[1]
        body = body[: body.index("return stats;")]
        self.assertIn("std::uint8_t pendingKind = 0;", body)
        self.assertIn("fieldId == 0x022B", body)
        self.assertIn('if (layerKind == "Multiply") {', body)
        self.assertIn("kindCode = 1;", body)
        self.assertIn("kindCode = 2;", body)
        # Name-first: a kind after the name belongs to that name, and is only
        # buffered when no name has opened the stream yet.
        self.assertIn("if (!currentKey.empty()) {", body)
        self.assertIn("kindStoredForCurrent", body)
        self.assertIn("pendingKind = kindCode;", body)
        # Recorded under both the full object key and its bare name, each with
        # the .mat spellings the override-colour index uses (see
        # test_kind_is_readable_under_every_alpha_spelling).
        self.assertIn("const auto StoreCdbKind = [&](const std::string& a_name, std::uint8_t a_kind) {", body)
        self.assertIn("g_cdbKind.try_emplace(a_name, a_kind);", body)
        self.assertIn("g_cdbKind.try_emplace(\"materials/\" + a_name + \".mat\", a_kind);", body)
        self.assertIn("StoreCdbKind(key, pendingKind);", body)
        self.assertIn("StoreCdbKind(key.substr(slash + 1), pendingKind);", body)
        # Cleared with the alpha, or the next object inherits it.
        tail = body[body.rindex("havePending = false;") :]
        self.assertIn("pendingKind = 0;", tail[:80])

    def test_kind_survives_the_index_cache(self) -> None:
        # A kind that only lives in memory would be lost on every warm start
        # and the rule would silently stop working after the first session.
        # 8: texture paths are identified by content, not by the base game's
        # 0x038A field id (the Creation databases use their own ids).
        # 9: numbered texture-set variants reduce to one whole donor and an
        # albedo is recognised by its role token, so a v8 cache (frankenstein
        # sets, and no colour for every "_BaseColor.dds" material) is rejected
        # and rebuilt - the layer kind is written and read by the same cache.
        # 10: override colours and kinds are NAME-FIRST. A v9 cache carries
        # every alpha/tint one object early (Velocity Yellow's violet, the cup
        # lid's grey, the cheese block's neighbour alpha), so it cannot be
        # reused.
        # 11: a planet's `_terrain` layer indexes as its material, so a v10
        # cache has no key for the desktop globes' planet maps.
        self.assertIn("constexpr std::uint32_t kCdbSchema = 11;", self.live)
        save = self.live.split("f.write(reinterpret_cast<const char*>(&alphaCount), 8);", 1)[1]
        self.assertIn("f.write(reinterpret_cast<const char*>(&kindCount), 8);", save[:160])
        self.assertIn("for (const auto& [key, kind] : g_cdbKind) {", save[:4000])
        load = self.live.split("f.read(reinterpret_cast<char*>(&alphaCount), 8);", 1)[1]
        self.assertIn("f.read(reinterpret_cast<char*>(&kindCount), 8);", load[:160])
        self.assertIn("for (std::uint64_t i = 0; i < kindCount; ++i) {", load[:6000])
        self.assertIn("if (kind < 0 || kind > 3) {", load[:6500])
        self.assertIn("g_cdbKind = std::move(kinds);", self.live)

    def test_lookup_joins_kind_to_the_alpha_by_object_name(self) -> None:
        self.assertIn(
            "bool FindCdbAlpha(const std::string& a_matPath, float& a_outAlpha, std::uint8_t* a_outLayerKind = nullptr)",
            self.live_h,
        )
        body = self.live.split("bool FindCdbAlpha(const std::string& a_matPath, float& a_outAlpha, std::uint8_t* a_outLayerKind)", 1)[1]
        body = body[: body.index("bool FindCdbTint")]
        self.assertIn("*a_outLayerKind = 0;", body)
        self.assertIn("const auto kind = g_cdbKind.find(a_key);", body)
        self.assertIn("*a_outLayerKind = kind->second;", body)

    def test_layer_kinds_do_not_make_a_range_translucent(self) -> None:
        blend = self.offline.split("float MaterialBlendAlpha(", 1)[1]
        blend = blend[: blend.index("bool BuildGpuModel")]
        self.assertIn("std::uint8_t layerKind = 0;", blend)
        self.assertIn("FindCdbAlpha(a_matPath, cdb, &layerKind)", blend)
        gate = blend.split("if (layerKind == 1 || layerKind == 2) {", 1)[1]
        gate = gate[: gate.index("a_translucent = true;")]
        self.assertIn("a_translucent = false;", gate)
        self.assertIn("return 1.0f;", gate)
        # The plain alpha path survives for materials with no layer kind, but
        # only above the surface floor (see test_a_layer_weight_alpha_does_
        # not_delete_the_surface).
        self.assertIn("a_translucent = true;\n\t\t\t\treturn cdb;", blend)
        self.assertIn("constexpr float kMinSurfaceAlpha = 0.05f;", blend)
        self.assertIn("if (cdb >= kMinSurfaceAlpha) {", blend)

    def test_a_layer_weight_alpha_does_not_delete_the_surface(self) -> None:
        """1/255 and 0.025 are weights, not coverage, when no kind is declared.

        Reported as every Aid preview looking wrong next to the engine's own
        render. Two shipped cases carry the signature: MenuItem_Blend's
        printed can body stores 0.025, and painted ship metal, cave floor and
        hardwood materials store 0.0039 (1/255) - the same value as the
        Chunks cap's 8/255 garment weight. Drawing those as opacity makes the
        surface vanish instead of shading it.
        """
        blend = self.offline.split("float MaterialBlendAlpha(", 1)[1]
        blend = blend[: blend.index("bool BuildGpuModel")]
        floor = blend[blend.index("constexpr float kMinSurfaceAlpha") :]
        # Below the floor the function falls through to the token scan, which
        # is what still gives glass its 0.30 fallback, rather than returning
        # an opaque 1.0 outright.
        self.assertIn("below the {:.2f} surface floor", floor)
        # The branch closes without returning, so control falls through to the
        # token scan below it.
        tail = floor[floor.index("kMinSurfaceAlpha);") + len("kMinSurfaceAlpha);") :]
        self.assertNotIn("return", tail[: tail.index("}")])

        import cdb_alpha_replica as A

        if not os.path.exists(A.CDB):
            self.skipTest("materialsbeta.cdb extract not present")
        kinds = A.scan_kinds(A.CDB)
        alphas = A.scan(A.CDB)
        # A furniture wood with no kind record: 0.014 is below the floor, so it
        # is a weight, not coverage. MenuItem_Blend used to be the example, but
        # its 0.0248 belonged to the record before its name; the can's own
        # Material1 stores a 0.0 alpha and a Multiply kind.
        self.assertAlmostEqual(alphas["beddouble03_base.mat"][3], 0.01419, places=4)
        self.assertNotIn(kinds.get("beddouble03_base.mat"), (1, 2))

        implausible = [
            k
            for k, v in alphas.items()
            if k.endswith(".mat") and 0.0 < v[3] < 0.05 and kinds.get(k) not in (1, 2, 3)
        ]
        self.assertGreater(len(implausible), 400, "the floor should cover a population, not one item")

    def test_decal_sheets_still_keep_their_own_coverage_path(self) -> None:
        # Decal sheets return before the CDB alpha is even consulted, so the
        # kind gate cannot reach them.
        blend = self.offline.split("float MaterialBlendAlpha(", 1)[1]
        blend = blend[: blend.index("bool BuildGpuModel")]
        self.assertLess(blend.index("IsDecalSheet(a_matPath)"), blend.index("layerKind == 1"))
        self.assertIn("cdb < 0.995f", blend)

    def test_shipped_databases_separate_the_two_cases(self) -> None:
        # The data claim the rule rests on.
        import cdb_alpha_replica as A

        if not os.path.exists(A.CDB):
            self.skipTest("materialsbeta.cdb extract not present")
        kinds = A.scan_kinds(A.CDB)
        alphas = A.scan(A.CDB)

        # The rifle is opaque because the database says nothing about its
        # receiver, not because a Multiply weight was read: the old "kind 1,
        # alpha 0.635" pair was the object serialized BEFORE the receiver's
        # name, so both were its neighbour's. Erasing that record made the
        # see-through rifle go away for the wrong reason, which is why the
        # refuted claim is pinned here instead of deleted.
        self.assertNotIn("razorback_receiver", kinds)
        self.assertNotIn("razorback_receiver", alphas)

        cup = alphas.get("tranquilitea_togocup_base01")
        self.assertIsNotNone(cup, "Tranquilitea cup base missing from the CDB")
        self.assertAlmostEqual(cup[3], 0.608, places=3)
        self.assertNotIn("tranquilitea_togocup_base01", kinds, "the glass cup declares no layer kind")

        # The other case: a material that really is a Multiply layer and also
        # stores its own alpha in the same object.
        self.assertEqual(kinds.get("erdebrau_can"), 1)
        self.assertAlmostEqual(alphas["erdebrau_can"][3], 0.0, places=6)

        multiply_tinted = [k for k, v in alphas.items() if kinds.get(k) == 1 and 0.0 < v[3] < 0.995]
        self.assertGreater(len(multiply_tinted), 500, "the rule should cover a population, not one item")

    def test_override_colours_belong_to_the_name_that_opens_the_object(self) -> None:
        """The six items reported unchanged after 0.8.304, all one off-by-one.

        The container writes the CTName first (0x1f4), then the object id
        (0x20f), then its components - 0x22b kind, 0x25a override colour.
        Reading the 0x25a that PRECEDES a name gave every material the
        previous object's colour and alpha: Velocity Yellow wore the violet,
        the to-go cup base the lid's grey, the Hypergiant Heart body and the
        cheese block inherited a neighbour's opacity, and Note 02's BookB04
        cover lost its own grey-green.

        The layout is probed, not assumed. Both earlier readings failed here in
        opposite directions; `layout_audit` is the check that settles it, and it
        must find every colour AFTER a name and THAT name's id.
        """
        import cdb_alpha_replica as A

        if not os.path.exists(A.CDB):
            self.skipTest("materialsbeta.cdb extract not present")
        alphas = A.scan(A.CDB)
        truth = A.truth(A.CDB)

        after_name, between, before = A.layout_audit(A.CDB)
        self.assertEqual(between, 0, "no override colour sits between a name and its id")
        self.assertEqual(before, 0, "no override colour precedes the first name")
        self.assertGreater(after_name, 15000, "the layout should cover the whole file")

        for key, want in {
            "velocity_beverage_yellow": (1.0, 0.916671, 0.0, 0.501961),
            "tranquilitea_togocup_base01": (0.138796, 0.763806, 0.809522, 0.607843),
            "heartplus": (0.733349, 0.647913, 0.0, 0.742962),
            "erdebrau_can": (1.0, 0.961075, 0.797738, 0.0),
            "data/materials/setdressing/books/genbookb01_bookb04.mat":
                (0.42282700538635254, 0.43858999013900757, 0.4135200083255768, 0.0),
        }.items():
            got = alphas.get(key)
            self.assertIsNotNone(got, f"{key} missing from the name-first scan")
            for a, b in zip(got, want):
                self.assertAlmostEqual(a, b, places=5, msg=key)
            self.assertEqual(alphas[key], truth[key], f"{key}: both walks must agree")

        # Every Velocity variant must wear the hue its own name claims. This is
        # the strongest available check on the association: under the old
        # order each one wore the NEXT variant's record.
        for key, test in {
            "velocity_beverage_red": lambda r, g, b: r > 0.9 and g < 0.1 and b < 0.1,
            "velocity_beverage_blue": lambda r, g, b: b > 0.9 and r < 0.1,
            "velocity_beverage_orange": lambda r, g, b: r > 0.9 and 0.4 < g < 0.8 and b < 0.1,
            "velocity_beverage_green": lambda r, g, b: g > 0.85 and r < 0.2 and b < 0.3,
            "velocity_beverage_violet": lambda r, g, b: r > 0.9 and b > 0.9 and g < 0.1,
            "velocity_beverage_yellow": lambda r, g, b: r > 0.9 and g > 0.85 and b < 0.05,
        }.items():
            got = alphas[key]
            self.assertTrue(test(got[0], got[1], got[2]), f"{key} wears the wrong hue: {got}")

        # These objects write no 0x25a at all, so any value here is the
        # previous material's alpha - which is what made the cheese block and
        # the rifle disappear.
        for key in ("packagedfood_cheese", "razorback_receiver"):
            self.assertNotIn(key, alphas, f"{key} inherited a neighbour's override")
            self.assertNotIn(key, truth, f"{key} inherited a neighbour's override")

        # The implementation the replica mirrors.
        self.assertIn("NAME-FIRST", self.live)
        body = self.live.split("fieldId == 0x025A", 1)[1]
        body = body[: body.index("fieldId == 0x022B")]
        self.assertIn("if (!currentKey.empty()) {", body)
        self.assertIn("colorStoredForCurrent", body)

    def test_saturated_override_tints_survive_the_near_white_gate(self) -> None:
        # Velocity_Beverage_Yellow is (1.0, 0.917, 0.0): one channel is at 1.0,
        # and the old "max < 0.97" test threw the only colour the drink has
        # away, leaving a translucent white box.
        body = self.live.split("bool FindCdbTint(", 1)[1]
        body = body[: body.index("bool WeaponDirNamesFamily")]
        self.assertIn("const float mn = std::min({", body)
        self.assertIn("mx - mn < 0.20f", body)
        self.assertIn("nearWhiteNeutral", body)
        # A neutral near-white is still rejected rather than tinting a surface.
        self.assertIn("if (nearWhiteNeutral || mx <= 0.0f)", body)

    def test_texture_filenames_fold_spaces_into_underscores(self) -> None:
        # "erdebrau light_decal_color.dds" never exact-matched the material
        # "erdebrau_light_decal", so the shorter-base tiebreak handed the
        # light can the DARK decal.
        self.assertIn("std::replace(s.begin(), s.end(), ' ', '_');", self.live)
        body = self.live.split("std::string AlbedoBaseName(", 1)[1]
        body = body[: body.index("std::vector<ColorEntry> g_colors;")]
        self.assertIn("' '", body)

    def test_kind_is_readable_under_every_alpha_spelling(self) -> None:
        """The runtime lookup asks for '<stem>.mat', never the bare name.

        FindCdbAlpha normalizes Materials\\Common\\Metal\\MetalAluminum01.mat
        to materials/common/metal/metalaluminum01.mat and falls back to the
        stem, which still ends in .mat. The override-colour index stores those
        .mat spellings; the kind map only stored the bare object key, so the
        join failed at runtime while these replica scans - which compare bare
        names - said it worked. The logged symptom was Velocity Yellow at
        'translucent ranges 6 of 6' with no 'layer, not surface opacity' line
        anywhere in the session: a Multiply layer weight drawn as opacity.
        """
        import cdb_alpha_replica as A

        if not os.path.exists(A.CDB):
            self.skipTest("materialsbeta.cdb extract not present")
        kinds = A.scan_kinds(A.CDB)
        alphas = A.scan(A.CDB)

        # Every kind must be joinable by the spelling the lookup uses. A kind
        # for a bare name with no .mat alias is invisible to FindCdbAlpha.
        unjoinable = [
            k
            for k in kinds
            if "/" not in k and not k.endswith(".mat") and k + ".mat" not in kinds
        ]
        self.assertEqual(unjoinable, [], "kinds must carry the .mat alias the lookup asks for")

        # The measured case: a shipped Multiply metal used by consumables. Its
        # own record stores alpha 0.0, so the join must not turn the layer
        # weight that used to be attributed to it (0.392, the neighbour's)
        # back into surface opacity.
        self.assertEqual(kinds.get("metalaluminum01.mat"), 1)
        self.assertAlmostEqual(alphas["metalaluminum01.mat"][3], 0.0, places=6)

        # The join now answers for every Multiply/Lerp layer the alpha index
        # can reach through a .mat spelling, not just for bare names.
        joined = [
            k
            for k, kind in kinds.items()
            if kind in (1, 2) and k.endswith(".mat") and alphas.get(k) and 0.0 < alphas[k][3] < 0.995
        ]
        self.assertGreater(len(joined), 500, "the join should cover the whole layer population")

    def test_the_creation_database_gives_the_receiver_no_override(self) -> None:
        # The rifle used to be pinned as a split record: kind Multiply in the
        # base database, alpha 0.635 in SFBGS007. Name-first reading refutes
        # both - Razorback_Receiver_Material1 writes only its id, in BOTH
        # files - so the material takes no override at all and stays opaque.
        # Keeping the claim under test means a future change cannot quietly
        # reintroduce the ghost by re-attributing a neighbouring record.
        here = Path("B:/SteamLibrary/steamapps/common/Starfield/Data")
        archive = here / "SFBGS007 - Main.ba2"
        if not archive.exists():
            self.skipTest("game data not on this machine")
        sys.path.insert(0, str(ROOT / "tools" / "ba2_audit"))
        from ba2lib import Ba2Archive

        blob, err = Ba2Archive(archive).read_entry("materials/creations/sfbgs007/materialsbeta.cdb")
        self.assertIsNotNone(blob, err)
        tmp = ROOT / "tools" / "renderer_recon" / "_sfbgs007_tmp.cdb"
        try:
            tmp.write_bytes(blob)
            import cdb_alpha_replica as A
            alphas = A.scan(str(tmp))
            after, between, before = A.layout_audit(str(tmp))
        finally:
            tmp.unlink(missing_ok=True)
        self.assertEqual((between, before), (0, 0), "SFBGS007 uses the same name-first layout")
        self.assertGreater(after, 1000)
        self.assertNotIn("razorback_receiver", alphas,
                         "Razorback_Receiver_Material1 carries no override colour")


if __name__ == "__main__":
    unittest.main()
