#!/usr/bin/env python3
"""Guard for the aid_colour_audit mirror.

The audit prints the colour and alpha decisions the plugin ships, so it is only
useful while its rules still match the C++ they copy. These tests read
src/OfflineModel.cpp and src/LiveModel.cpp and fail when a constant or a token
list drifts, then exercise the mirrored decisions directly: decal sheets ignore
their override alpha, Multiply/Lerp layers are opaque, a consumable layer weight
is not opacity, near-white overrides say nothing, a neutral map still takes the
authored tint, and the Aid selection is ALCH (the shelf's form type), not a
keyword the dump does not carry.
"""
from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))

import aid_colour_audit as audit  # noqa: E402

OFFLINE = ROOT / "src" / "OfflineModel.cpp"
LIVE = ROOT / "src" / "LiveModel.cpp"


class FakeCdb(audit.Cdb):
    """Cdb without a database file: the lookup rules, fed by hand."""

    def __init__(self, rgba=None, kinds=None, sets=None) -> None:
        self.rgba = rgba or {}
        self.kinds = kinds or {}
        self.sets = sets or {}


class RuleMirrorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.offline = OFFLINE.read_text(encoding="utf-8-sig")
        cls.live = LIVE.read_text(encoding="utf-8-sig")

    @staticmethod
    def _slice(text: str, start: str, end: str) -> str:
        begin = text.index(start)
        return text[begin:text.index(end, begin)]

    def test_decal_tokens_match_IsDecalSheet(self) -> None:
        body = self._slice(self.offline, "bool IsDecalSheet(", "// The Breach Tactical Stock")
        found = [t for t in audit.DECAL_TOKENS if f'"{t}"' in body]
        self.assertEqual(found, list(audit.DECAL_TOKENS))

    def test_scorer_never_prints_a_body_with_a_decal_atlas(self) -> None:
        """A label is not a body: the exotics containers and the desk fan.

        The 2026-10-23 12:53 session bound `lubricant_decal_color.dds` to
        `Lubricant_Metal01.mat` on every resource canister and
        `deskfan_decal_color.dds` to the fan's white plastic housing, because
        those folders ship no other map. The guard must reject a sheet-named
        candidate unless the MATERIAL itself names a printed sheet, and it
        must run before any score is awarded.
        """
        body = self._slice(self.live, "bool FindDiffuse(",
                           "// The shared layer base is the last resort")
        self.assertIn("namesDecalSheet", body)
        sheet = self._slice(body, "const auto namesDecalSheet",
                            "const bool matIsPrintedSheet")
        found = [t for t in audit.DECAL_TOKENS if f'"{t}"' in sheet]
        self.assertEqual(found, list(audit.DECAL_TOKENS))
        # One shared veto, applied in the scorer and in both fallback loops:
        # the exotics containers resolve through the same-folder generic
        # fallback and the singleton, not only through scoreOne.
        self.assertGreaterEqual(body.count("!candidateAllowed(e.base)"), 3)
        veto = body.index("candidateAllowed(e.base)")
        score = body.index("if (e.base == c)")
        self.assertLess(veto, score, "the sheet veto must precede the score")
        self.assertNotIn("scoreOne(", body[:veto],
                         "the veto is defined before the scorer, not after it")
        # The material's own path decides. The label meshes
        # (Lubricant_Decal01.mat, DeskFan's decal material) keep their atlas.
        self.assertIn("namesDecalSheet(normalizedMat)", body)

    def test_sheer_tokens_match_the_alpha_token_scan(self) -> None:
        body = self._slice(self.offline, "float MaterialBlendAlpha(", "bool BuildGpuModel(")
        token_block = self._slice(body, "static constexpr const char* kTokens[]", "};")
        found = [t for t in audit.SHEER_TOKENS if f'"{t}"' in token_block]
        self.assertEqual(found, list(audit.SHEER_TOKENS))

    def test_alpha_constants_match_the_source(self) -> None:
        body = self._slice(self.offline, "float MaterialBlendAlpha(", "bool BuildGpuModel(")
        self.assertIn(f"kMinSurfaceAlpha = {audit.MIN_SURFACE_ALPHA:.2f}f;", body)
        self.assertIn(f"return {audit.TOKEN_FALLBACK_ALPHA:.2f}f;", body)

    def test_consumable_marks_match_the_source(self) -> None:
        body = self._slice(self.offline, "float MaterialBlendAlpha(", "bool BuildGpuModel(")
        block = self._slice(body, "const bool consumableItem", "std::uint8_t layerKind")
        self.assertEqual([m for m in audit.CONSUMABLE_MARKS if f'"{m}"' in block],
                         list(audit.CONSUMABLE_MARKS))
        # The cup is under setdressing/tranquilitea, which is not one of these
        # marks; it stays solid because of the see-through-name gate, not
        # because of a folder allowlist. Adding the folder here would hide the
        # real rule behind a one-item patch.
        self.assertNotIn("/tranquilitea/", audit.CONSUMABLE_MARKS)

    def test_authored_sheer_matches_the_source(self) -> None:
        body = self._slice(self.offline, "kAuthoredSheer[]", "bool BuildGpuModel(")
        for token in audit.AUTHORED_SHEER:
            self.assertIn(f'"{token}"', body)
        self.assertIn(f"kAuthoredSheerAlpha = {audit.AUTHORED_SHEER_ALPHA:.2f}f", body)
        # ...and the colour that goes with that glass. Both the constant and the
        # branch that applies it are mirrored here so a tint changed in one
        # file cannot drift from the other.
        tint = ", ".join(f"{c:.2f}f" for c in audit.AUTHORED_GLASS_TINT)
        self.assertIn(f"kAuthoredGlassTint[3] = {{ {tint} }}", body)
        # The name gate the alpha branch uses, listed once here so a token
        # added to the C++ without the mirror fails the test.
        alpha_body = self._slice(self.offline, "float MaterialBlendAlpha(", "bool BuildGpuModel(")
        gate = self._slice(alpha_body, "const bool garmentSheerToken",
                           "if (cdb >= kMinSurfaceAlpha &&")
        for token in audit.SHEER_NAME_TOKENS:
            self.assertIn(f'"{token}"', gate, token)

    def test_colour_gate_constants_match_FindCdbTint(self) -> None:
        body = self._slice(self.live, "bool FindCdbTint(", "bool WeaponDirNamesFamily(")
        self.assertIn(f"mx >= {audit.NEAR_WHITE_MAX:.2f}f", body)
        self.assertIn(f"mx - mn < {audit.NEAR_WHITE_SPREAD:.2f}f", body)

    def test_lager_routes_match_the_source(self) -> None:
        for material, atlas in (
                ("materials/items/fooddrink_singleitems/erdebrau_beers/erdebrau_lager_decal.mat",
                 "textures/items/fooddrink_singleitems/erdebrau_beers/erdebrau lager_color.dds"),
                ("materials/items/fooddrink_singleitems/erdebrau_beers/erdebrau_lager_metal.mat",
                 "textures/items/fooddrink_singleitems/erdebrau_beers/erdebrau lager_aluminium_color.dds")):
            self.assertIn(f'"{material}"', self.live)
            self.assertIn(f'"{atlas}"', self.live)
            self.assertEqual(audit.EXACT_ROUTES[material], atlas)

    def test_book_swap_keys_are_in_the_plugin_spelling(self) -> None:
        # book_key returns lowercase backslash paths; a forward-slash key can
        # never match and silently drops the authored swap from the audit.
        self.assertIn(
            audit.book_key("Materials\\SetDressing\\Books\\GenBookB01_BookB01.mat"),
            audit.AUTHORED_SWAPS[0x000A9010])
        for swaps in audit.AUTHORED_SWAPS.values():
            for key in swaps:
                self.assertNotIn("/", key)

    def test_grunge_rule_constants_match_the_upload_path(self) -> None:
        body = self._slice(self.offline, "const PrefetchedTex& img = *found;", "::ID3D12Resource* tex")
        self.assertIn(f'map.rfind("{audit.GRUNGE_PREFIX}", 0)', body)
        self.assertIn(f">= {audit.GRUNGE_SPREAD:.2f}f", body)


class BlendAlphaTests(unittest.TestCase):
    def test_decal_sheet_ignores_the_override_alpha(self) -> None:
        alpha, translucent, why = audit.blend_alpha(
            "Materials\\Items\\FoodDrink_SingleItems\\Rattler_Decals.mat", 0.0078, None)
        self.assertEqual((alpha, translucent), (1.0, False))
        self.assertIn("decal sheet", why)

    def test_multiply_layer_alpha_is_a_weight(self) -> None:
        alpha, translucent, why = audit.blend_alpha(
            "Materials\\Weapons\\Razorback\\Razorback_Receiver.mat", 0.635, 1)
        self.assertEqual((alpha, translucent), (1.0, False))
        self.assertIn("Multiply", why)

    def test_clear_glass_family_takes_its_authored_transmission(self) -> None:
        # GlassCleanTransparent01's database set is empty, so 0.8.345 gave the
        # family the measured per-wall transmission instead of the bare
        # Fresnel-only 0.0. The mirror constant, not a literal, and still
        # translucent either way.
        alpha, translucent, why = audit.blend_alpha(
            "Materials\\Common\\Glass\\GlassCleanTransparent01.mat", 0.0, None)
        self.assertEqual((alpha, translucent), (audit.AUTHORED_SHEER_ALPHA, True))
        self.assertLess(alpha, 1.0)
        self.assertIn("clear glass family", why)

    def test_glass_takes_the_token_fallback(self) -> None:
        # The mirror constant, not a literal: this file's job is to catch the
        # audit and the C++ drifting apart. 0.0 is the value (a clear shell - a
        # closed glass body drawn twice at 0.30 composited to 51% of a pale
        # film); what must not change is that the token scan stays translucent
        # instead of falling through to the opaque 1.0. Frosted glass is the
        # control: it repeats its folder in its name and is still glass.
        alpha, translucent, why = audit.blend_alpha(
            "Materials\\Common\\Glass\\GlassDirtyHeavyFrost01.mat", 0.0, None)
        self.assertEqual((alpha, translucent), (audit.TOKEN_FALLBACK_ALPHA, True))
        self.assertLess(alpha, 1.0)
        self.assertIn("glass", why)

    def test_consumable_layer_weight_is_opaque(self) -> None:
        alpha, translucent, why = audit.blend_alpha(
            "Materials\\Items\\Drugs_Or_Medical\\Velocity.mat", 0.501961, None)
        self.assertEqual((alpha, translucent), (1.0, False))
        self.assertIn("consumable", why)

    def test_to_go_cup_weight_is_opaque(self) -> None:
        # Measured: the cup is solid in the game's own inventory view (user
        # capture, 2026-10-18), and 0.608 on base and lid made both ghosted.
        for name, alpha in (("Tranquilitea_ToGoCup_Base01", 0.607843),
                            ("Tranquilitea_ToGoCup_Top01", 0.607843)):
            got, translucent, why = audit.blend_alpha(
                f"Materials\\SetDressing\\Tranquilitea\\Tranquilitea_ToGoCup\\{name}.mat",
                alpha, None)
            self.assertEqual((got, translucent), (1.0, False), name)
            self.assertIn("does not name itself see-through", why)

    def test_coolant_lid_weight_is_opaque(self) -> None:
        # Alien Genetic Material's jar cap: solid metal on screen, 0.252 in DB.
        alpha, translucent, why = audit.blend_alpha(
            "Materials\\SetDressing\\Manufactured_Goods\\Isotopic_Coolant_Lid.mat",
            0.251774, None)
        self.assertEqual((alpha, translucent), (1.0, False))
        self.assertIn("does not name itself see-through", why)

    def test_glass_helmet_keeps_its_measured_alpha(self) -> None:
        # The one name-sheer material on the shelves: every inhaler body.
        alpha, translucent, why = audit.blend_alpha(
            "Materials\\Common\\Glass\\GlassHelmets01.mat", 0.686, None)
        self.assertEqual((alpha, translucent), (0.686, True))
        self.assertIn("used as surface alpha", why)

    def test_is_isotopic_coolant_jar_is_glass(self) -> None:
        alpha, translucent, why = audit.blend_alpha(
            "Materials\\SetDressing\\Manufactured_Goods\\Isotopic_Coolant_Container.mat",
            None, None)
        self.assertEqual((alpha, translucent), (audit.AUTHORED_SHEER_ALPHA, True))
        self.assertIn("authored glass", why)

    def test_garment_layer_weight_is_opaque(self) -> None:
        alpha, translucent, _ = audit.blend_alpha(
            "Materials\\Clothes\\BaseballCap\\Outfit_Baseball_Cap_01_Material1.mat", 8 / 255, None)
        self.assertEqual((alpha, translucent), (1.0, False))

    def test_sub_floor_alpha_falls_through_to_the_token_scan(self) -> None:
        alpha, translucent, why = audit.blend_alpha(
            "Materials\\SetDressing\\Cave\\CaveFloor.mat", 0.0039, None)
        self.assertEqual((alpha, translucent), (1.0, False))
        self.assertIn("below the 0.05 floor", why)


class MissingMeshTests(unittest.TestCase):
    """A row that cannot be resolved must name the path it tried."""

    def test_missing_nif_names_the_model(self) -> None:
        class Plugins:
            @staticmethod
            def record(plugin, form):
                return {"model": "SetDressing\\ammo_crates\\typea_xlarge.nif", "molm": []}

        class Store:
            @staticmethod
            def read(rel):
                return None

        rows = audit.audit_item({"name": "Missile", "form": 0x4249, "plugin": "Starfield.esm"},
                                Plugins(), None, Store(), None, False)
        self.assertEqual(rows[0]["model"], "SetDressing\\ammo_crates\\typea_xlarge.nif")
        self.assertIn("typea_xlarge.nif", rows[0]["color_why"])


class OwnSetLookupTests(unittest.TestCase):
    """The texture-set lookup, which decides whether a material has art at all."""

    def test_stem_lookup_drops_the_extension(self) -> None:
        # CdbPathParts erases `.mat` before the stem lookup. The database also
        # ships an extension-named set (`metalbronze02.mat_TextureSet1`) that
        # carries no maps, and stopping there is what made the metal flat.
        cdb = FakeCdb(sets={"metalbronze02.mat": {},
                            "metalbronze02": {"color": "textures/common/metal/metalbronze02_color.dds"}})
        own, how = cdb.own_set("Materials\\Common\\Metal\\MetalBronze02.mat")
        self.assertEqual(own["color"], "textures/common/metal/metalbronze02_color.dds")
        self.assertEqual(how, "own set")

    def test_full_path_key_still_wins_over_the_stem(self) -> None:
        cdb = FakeCdb(sets={
            "materials/x/a/shoe.mat": {"color": "textures/x/a_shoe_color.dds"},
            "shoe": {"color": "textures/x/other_shoe_color.dds"}})
        own, _how = cdb.own_set("Materials\\X\\A\\Shoe.mat")
        self.assertEqual(own["color"], "textures/x/a_shoe_color.dds")

    def test_a_numbered_colour_variant_borrows_its_family(self) -> None:
        # PlasticWornScratched02_Grey02 records an empty set; the colour lives
        # with the base family, so the number has to go before the colour word.
        cdb = FakeCdb(sets={"plasticwornscratched02_grey02": {},
                            "plasticwornscratched02":
                                {"color": "textures/common/paint/paintmetalscratched01_color.dds"}})
        own, how = cdb.own_set("Materials\\SetDressing\\CommonClutter\\PlasticWornScratched02_Grey02.mat")
        self.assertEqual(own["color"], "textures/common/paint/paintmetalscratched01_color.dds")
        self.assertEqual(how, "colour-family 'plasticwornscratched02'")

    def test_a_flat_family_is_not_lent(self) -> None:
        # Nothing to lend: an empty family set must not turn "no record" into a
        # hit, or the row would claim art the database does not have.
        cdb = FakeCdb(sets={"plasticplainmatte02_white01": {},
                            "plasticplainmatte02": {}})
        own, how = cdb.own_set("Materials\\SetDressing\\CommonClutter\\PlasticPlainMatte02_White01.mat")
        self.assertEqual(own, {})
        self.assertEqual(how, "empty set")


class ColourDecisionTests(unittest.TestCase):
    def test_near_white_override_says_nothing(self) -> None:
        # 0.95 is not near-white: the gate starts at 0.97. 0.98/0.98/0.97 is a
        # neutral white with rounding error, which is what the source rejects.
        cdb = FakeCdb(rgba={"materials/x/whiteplate.mat": (0.98, 0.98, 0.97, 1.0)})
        self.assertIsNone(cdb.tint("Materials\\X\\WhitePlate.mat"))

    def test_saturated_override_survives_a_white_channel(self) -> None:
        cdb = FakeCdb(rgba={"materials/x/beer.mat": (1.0, 0.917, 0.0, 1.0)})
        self.assertEqual(cdb.tint("Materials\\X\\Beer.mat"), (1.0, 0.917, 0.0))

    def test_authored_tint_is_kept_when_the_set_has_no_colour_map(self) -> None:
        cdb = FakeCdb(rgba={"materials/x/beer.mat": (1.0, 0.917, 0.0, 1.0)},
                      sets={"materials/x/beer.mat": {"color": "", "maps": {"metal": "m"}}})
        rgb, why = audit.color_decision("Materials\\X\\Beer.mat", cdb)
        self.assertEqual(rgb, (1.0, 0.917, 0.0))
        self.assertIn("no colour map", why)

    def test_authored_tint_multiplies_over_a_neutral_map(self) -> None:
        cdb = FakeCdb(rgba={"materials/x/can.mat": (1.0, 0.961, 0.798, 1.0)},
                      sets={"materials/x/can.mat": {"color": "textures/common/paint/paint01_color.dds"}})
        rgb, why = audit.color_decision("Materials\\X\\Can.mat", cdb,
                                        mean_reader=lambda key: (0.77, 0.78, 0.77),
                                        map_present=lambda key: True)
        self.assertEqual(rgb, (1.0, 0.961, 0.798))
        self.assertIn("multiplied over a neutral own map", why)

    def test_a_layered_override_is_not_a_paint_colour(self) -> None:
        # `Crafted_InHailer` stores kind 1 and (1.0,0.314,0.325); the preview
        # draws its map at white. Only kind 0 is a paint colour.
        cdb = FakeCdb(rgba={"materials/x/half.mat": (1.0, 0.314, 0.325, 1.0)},
                      kinds={"materials/x/half.mat": 1},
                      sets={"materials/x/half.mat": {"color": "textures/x/half_color.dds"}})
        rgb, why = audit.color_decision("Materials\\X\\Half.mat", cdb,
                                        mean_reader=lambda key: (0.59, 0.59, 0.59),
                                        map_present=lambda key: True)
        self.assertEqual(rgb, (1.0, 1.0, 1.0))
        self.assertIn("layer kind 1", why)

    def test_a_colour_map_carries_the_art(self) -> None:
        cdb = FakeCdb(rgba={"materials/x/can.mat": (1.0, 0.961, 0.798, 1.0)},
                      sets={"materials/x/can.mat": {"color": "textures/items/can_color.dds"}})
        rgb, why = audit.color_decision("Materials\\X\\Can.mat", cdb,
                                        mean_reader=lambda key: (0.55, 0.24, 0.20),
                                        map_present=lambda key: True)
        self.assertEqual(rgb, (1.0, 1.0, 1.0))
        self.assertIn("carries the art", why)

    def test_grunge_overlay_leaves_a_saturated_override_untextured(self) -> None:
        cdb = FakeCdb(rgba={"materials/x/velocity.mat": (1.0, 0.917, 0.0, 1.0)},
                      sets={"materials/x/velocity.mat":
                            {"color": "textures/common/grunge/grungeoilslick01_color.dds"}})
        rgb, why = audit.color_decision("Materials\\X\\Velocity.mat", cdb,
                                        mean_reader=lambda key: (0.2, 0.2, 0.6),
                                        map_present=lambda key: True)
        self.assertEqual(rgb, (1.0, 0.917, 0.0))
        self.assertIn("grunge", why)

    def test_opacity_only_set_says_so(self) -> None:
        # Sparkling Wine: a cutout mask and no colour map anywhere in the set.
        cdb = FakeCdb(sets={"materials/x/wine.mat": {"opacity": "textures/x/wine_opacity.dds"}})
        rgb, why = audit.color_decision("Materials\\X\\Wine.mat", cdb)
        self.assertEqual(rgb, (0.70, 0.70, 0.72))
        self.assertIn("only opacity mask", why)

    def test_flat_material_takes_its_name_token_colour(self) -> None:
        cdb = FakeCdb()
        rgb, why = audit.color_decision(
            "Materials\\SetDressing\\SupplyContainers\\SupplyContainer_Generic_Metal_Bronze.mat", cdb)
        self.assertEqual(rgb, audit.NAMED_COLORS["bronze"])
        self.assertIn("name token", why)

    def test_camel_case_colour_word_is_read(self) -> None:
        # Hypergiant Heart's yellow body material, straight from the NIF.
        self.assertEqual(
            audit.flat_base_color("Materials\\Items\\Drugs_Or_Medical\\Crafted_InHailerYellow.mat"),
            audit.NAMED_COLORS["yellow"])

    def test_colour_family_base_mirrors_the_plugin(self) -> None:
        self.assertEqual(audit.colour_family_base("crafted_inhaileryellow"), "crafted_inhailer")
        self.assertEqual(audit.colour_family_base("plate_green"), "plate")
        # A material whose name merely ENDS in a colour word is not a variant
        # of a shorter family, and the base itself has no base.
        self.assertIsNone(audit.colour_family_base("crafted_inhailer"))
        self.assertIsNone(audit.colour_family_base("bread"))
        self.assertIsNone(audit.colour_family_base("predator"))

    def test_colour_word_list_matches_the_plugin(self) -> None:
        live = LIVE.read_text(encoding="utf-8-sig")
        body = live[live.index("bool ColourWordSplit("): live.index("// Caller holds g_mutex")]
        block = body[body.index("static constexpr const char* kWords[]"):]
        block = block[: block.index("};" )]
        found = [w for w in audit.COLOUR_WORDS if f'"{w}"' in block]
        self.assertEqual(found, list(audit.COLOUR_WORDS))

    def test_borrowed_family_map_keeps_the_name_colour(self) -> None:
        # The real case: only `Crafted_InHailer` has a record, and its atlas is
        # the neutral case art. The yellow variant borrows it and stays gold.
        cdb = FakeCdb(sets={"crafted_inhailer":
                            {"color": "textures/items/medical/inhaler_color.dds"}})
        rgb, why = audit.color_decision(
            "Materials\\Items\\Drugs_Or_Medical\\Crafted_InHailerYellow.mat", cdb,
            mean_reader=lambda key: (0.59, 0.59, 0.59), map_present=lambda key: True)
        self.assertEqual(rgb, audit.NAMED_COLORS["yellow"])
        self.assertIn("borrowed colour-family", why)

    def test_camel_token_does_not_invent_colours(self) -> None:
        # The guard the underscore rule existed for: trailing substrings of an
        # ordinary word are not colour tokens.
        for name in ("Materials\\Weapons\\Predator.mat", "Materials\\Food\\Bread.mat"):
            self.assertIsNone(audit.flat_base_color(name), name)

    def test_uninstalled_map_leaves_the_range_flat(self) -> None:
        cdb = FakeCdb(sets={"materials/x/plate.mat": {"color": "textures/x/plate_color.dds"}})
        rgb, why = audit.color_decision("Materials\\X\\Plate.mat", cdb,
                                        map_present=lambda key: False)
        self.assertEqual(rgb, (0.70, 0.70, 0.72))
        self.assertIn("NOT installed", why)


class ShelfTests(unittest.TestCase):
    """--shelf must select the same forms the shop's tabs do."""

    def test_shelves_match_BrowseFormType(self) -> None:
        header = (ROOT / "src" / "ShopExplorer.h").read_text(encoding="utf-8-sig")
        body = header[header.index("BrowseFormType("):]
        body = body[:body.index("BrowseTypeName(")]
        for shelf, types in audit.SHELVES.items():
            for form in types:
                self.assertIn(f"RE::FormType::k{form}", body, shelf)
        # One shelf per switch case the audit models: no AMMO under Misc.
        self.assertEqual(audit.SHELVES["ammo"], ("AMMO",))
        self.assertEqual(audit.SHELVES["books"], ("BOOK",))
        self.assertEqual(audit.SHELVES["misc"], ("MISC",))
        self.assertEqual(audit.SHELVES["aid"], ("ALCH",))

    def test_html_is_labelled_with_the_shelf(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "a.html"
            audit.write_html([], out, "catalog.csv", "Books")
            html = out.read_text(encoding="utf-8")
        self.assertIn("<title>Books colour audit</title>", html)
        self.assertIn("<h1>Books colour audit</h1>", html)
        self.assertIn("0 Books item(s), 0 material row(s)", html)


class CatalogSelectionTests(unittest.TestCase):
    HEAD = ("plugin,formID,type,name,editorId,value,weight,modelPath,"
            "descriptionId,keywords,flags,inventory,renderCandidate")

    def _catalog(self, rows: list[str]) -> list[dict]:
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "catalog.csv"
            p.write_text("\n".join([self.HEAD] + rows) + "\n", encoding="utf-8")
            return audit.parse_catalog(p)

    def test_aid_selects_alch_even_with_no_keywords(self) -> None:
        items = self._catalog([
            '"Starfield.esm",001FF6A0,ALCH,"Erdebrau Light Can","",155,0.500,"",24418,"","",1,1',
            '"Starfield.esm",00012345,MISC,"Ship Parts","",10,0.100,"",0,"","",1,1',
            '"Starfield.esm",0000AAAA,ALCH,"Med Pack","",20,0.100,"",0,"","",1,1',
        ])
        picked = audit.select_items(items, aid=True)
        self.assertEqual([r["name"] for r in picked], ["Erdebrau Light Can", "Med Pack"])

    def test_book_shelf_selects_book_forms_only(self) -> None:
        items = self._catalog([
            '"Starfield.esm",000A9010,BOOK,"Captain Galaxy\'s Note","",0,0.000,"",0,"","",1,1',
            '"Starfield.esm",001FF6A0,ALCH,"Erdebrau Light Can","",155,0.500,"",0,"","",1,1',
            '"Starfield.esm",0000BBBB,MISC,"Ship Parts","",10,0.100,"",0,"","",1,1',
            '"Starfield.esm",0000CCCC,AMMO,"7.77mm Caseless","",1,0.010,"",0,"","",1,1',
        ])
        self.assertEqual([r["name"] for r in audit.select_items(items, shelves=["books"])],
                         ["Captain Galaxy's Note"])
        self.assertEqual([r["name"] for r in audit.select_items(items, shelves=["ammo"])],
                         ["7.77mm Caseless"])
        self.assertEqual([r["name"] for r in audit.select_items(items, shelves=["misc"])],
                         ["Ship Parts"])

    def test_two_shelves_union_without_duplicates(self) -> None:
        items = self._catalog([
            '"Starfield.esm",000A9010,BOOK,"Captain Galaxy\'s Note","",0,0.000,"",0,"","",1,1',
            '"Starfield.esm",0000BBBB,MISC,"Ship Parts","",10,0.100,"",0,"","",1,1',
        ])
        picked = audit.select_items(items, shelves=["books", "misc"])
        self.assertEqual(len(picked), 2)

    def test_item_and_form_selection_dedupe(self) -> None:
        items = self._catalog([
            '"Starfield.esm",001FF6A0,ALCH,"Erdebrau Light Can","",155,0.500,"",0,"","",1,1',
        ])
        picked = audit.select_items(items, forms=["1ff6a0"], needles=["erdebrau"])
        self.assertEqual(len(picked), 1)

    def test_limit_applies_after_deduping(self) -> None:
        items = self._catalog([
            '"Starfield.esm",001FF6A0,ALCH,"Erdebrau Light Can","",155,0.500,"",0,"","",1,1',
            '"Starfield.esm",001FF6A1,ALCH,"Erdebrau Pilsner Can","",155,0.500,"",0,"","",1,1',
        ])
        self.assertEqual(len(audit.select_items(items, aid=True, limit=1)), 1)


if __name__ == "__main__":
    unittest.main()
