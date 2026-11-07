"""Source-level regression checks for live catalog subcategory filters."""
from __future__ import annotations

import csv
import re
import unittest
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1] / "src" / "ShopExplorer.cpp"
ARMO_FIXTURE = Path(__file__).resolve().parent / "fixtures" / "armo_catalog.csv"
AMMO_FIXTURE = Path(__file__).resolve().parent / "fixtures" / "ammo_catalog.csv"


def load_csv_rows(path: Path) -> list[dict[str, str]]:
    """Rows captured from the test profile, minus the `#` comment lines."""
    with path.open(encoding="utf-8-sig", newline="") as handle:
        rows = [row for row in csv.reader(handle) if row and not row[0].startswith("#")]
    header, data = rows[0], rows[1:]
    return [dict(zip(header, row)) for row in data]


def load_armo_rows() -> list[dict[str, str]]:
    """ARMO name/editor ID/keyword rows captured from the test profile."""
    with ARMO_FIXTURE.open(encoding="utf-8", newline="") as handle:
        rows = [row for row in csv.reader(handle) if row and not row[0].startswith("#")]
    header, data = rows[0], rows[1:]
    return [dict(zip(header, row)) for row in data]


def is_actor_skin(row: dict[str, str]) -> bool:
    """Mirror of IsActorSkinArmor: no InventoryCategory*/ArmorType* keyword."""
    keywords = row["keywords"].lower()
    return "inventorycategory" not in keywords and "armortype" not in keywords


def has_numeric_caliber(text: str) -> bool:
    """Mirror of HasNumericCaliber: a number followed by mm/gauge/caliber."""
    for at, char in enumerate(text):
        if not char.isdigit():
            continue
        end = at
        while end < len(text) and (text[end].isdigit() or text[end] == "."):
            end += 1
        while end < len(text) and text[end] == " ":
            end += 1
        rest = text[end:]
        if rest.startswith("mm") or rest.startswith("gauge") or rest.startswith("caliber") \
                or rest.startswith("calibre"):
            return True
    return False


def ammo_sub(row: dict[str, str]) -> str:
    """Mirror of ClassifyAmmoSub, in the same priority order.

    The C++ side runs this over name + editor ID + keywords; the fixture carries
    all three, so the mirror matches it exactly.
    """
    ident = f'{row["name"]} {row["editorId"]} {row["keywords"]}'.lower()

    def has(*terms: str) -> bool:
        return any(has_term(ident, term) for term in terms)

    if has("spaceship", "ship ammo", "shipammo", "ship", "creatureammo", "creature ammo",
           "alien ammo", "beast ammo"):
        return "ShipCreature"
    if has("missile", "missiles", "torpedo", "torpedoes", "guided rocket", "guided rockets"):
        return "Missile"
    if has("mine", "mines", "grenade", "grenades", "explosive round", "frag round"):
        return "Explosive"
    if has("particle", "laser", "lzr", "energy", "plasma", "fusion", "electron", "antimatter",
           "neutron", "x-ray", "xray"):
        return "Energy"
    if has("caseless", "cartridge", "bullet", "ballistic", "flechette", "shot shell",
           "shotgun shell", "slug", "slugs") or has_numeric_caliber(ident):
        return "Ballistic"
    return "Other"


def is_unbuyable_ammo(row: dict[str, str]) -> bool:
    """Mirror of IsUnbuyableAmmo: unnamed, model-less, or a ship magazine.

    `modelPath` here is the record's MODL (the crate). The AMMO `NAM1` (spent
    shell casing, `shellCasing` in the fixture) is a different field and is
    empty for most rounds, so it must never stand in for the model.
    """
    if row["name"].strip().lower() in ("", "(unnamed)", "unnamed"):
        return True
    model = row["modelPath"].strip()
    if not model:
        return True
    if "testspaceshiprocketprojectile" in model.lower():
        return True
    return ammo_sub(row) in ("Missile", "ShipCreature")


def has_term(text: str, term: str) -> bool:
    """Mirror of HasTextTerm: a word-boundary match, not a raw substring.

    The boundary rule is why "apparel" does not match the glued keyword atom
    InventoryCategoryApparel - hats keep their own bucket that way.
    """
    def is_word(char: str) -> bool:
        return "a" <= char <= "z" or "0" <= char <= "9"

    at = text.find(term)
    while at >= 0:
        end = at + len(term)
        left = at == 0 or not is_word(text[at - 1]) or not is_word(term[0])
        right = end >= len(text) or not is_word(text[end]) or not is_word(term[-1])
        if left and right:
            return True
        at = text.find(term, at + 1)
    return False


def armor_sub(row: dict[str, str]) -> str:
    """Mirror of ClassifyArmorSub, in the same priority order."""
    ident = f'{row["name"]} {row["editorId"]} {row["keywords"]}'.lower()

    def has(*terms: str) -> bool:
        return any(has_term(ident, term) for term in terms)

    if has("armortypeneuroamp", "glasses", "eyewear", "goggles", "monocle", "sunglasses",
           "spectacles", "eyepatch", "neuroamp", "neuroboost", "neurocom", "neurotac"):
        return "Accessories"
    if "spacesuitbody" in ident or "categoryarmorsuit" in ident:
        return "Spacesuits"
    if ("spacesuithelmet" in ident or "categoryarmorhelmet" in ident
            or "ma_armor_spacesuit_helmet" in ident or has("helmet", "helmets")):
        return "Helmets"
    if "spacesuitbackpack" in ident or "categoryarmorbackpack" in ident or has("backpack", "jetpack"):
        return "Backpacks"
    if "inventorycategoryclothing" in ident or has("apparel", "clothing", "outfit", "uniform",
                                                  "jumpsuit", "suit", "jacket", "pants", "dress"):
        return "Apparel"
    if "apparelhead" in ident or has("cap", "hat", "beret", "beanie", "hood", "headwear",
                                     "headgear", "bandana"):
        return "Hats"
    if "apparel" in ident:
        return "Apparel"
    if has("spacesuit", "space suit"):
        return "Spacesuits"
    return "Other"


class CatalogFilterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = SOURCE.read_text(encoding="utf-8-sig")

    def function_body(self, name: str) -> str:
        match = re.search(rf"\b{name}\([^)]*\)\s*\{{(?P<body>.*?)\n\t\t\}}", self.source, re.S)
        self.assertIsNotNone(match, f"{name} implementation not found")
        return match.group("body")

    def test_weapon_filters_use_display_name_edid_and_keywords(self) -> None:
        search_text = self.function_body("CategorySearchText")
        self.assertIn("FormDisplayName(a_form)", search_text)
        self.assertIn("GetFormEditorID()", search_text)
        self.assertIn("FormKeywords(a_form, 2048)", search_text)
        self.assertIn("ClassifyWeaponSub(catText())", self.source)
        # ... fed from the record's one folded name+EDID+keyword text.
        self.assertIn("categoryText = CategorySearchText(form);", self.source)

    def test_armor_slot_filters_use_searchable_identity(self) -> None:
        self.assertIn("ClassifyArmorSub(catText())", self.source)
        self.assertIn("categoryText = CategorySearchText(form);", self.source)
        classifier = self.function_body("ClassifyArmorSub")
        for term in ("helmet", "backpack", "spacesuit", "clothing", "outfit"):
            self.assertIn(f'"{term}"', classifier)

    def test_armor_headwear_is_split_into_helmets_hats_and_accessories(self) -> None:
        header = (SOURCE.parent / "ShopExplorer.h").read_text(encoding="utf-8-sig")
        for sub, label in (("kHelmet", "Helmets"), ("kHat", "Hats / Caps"), ("kAccessory", "Accessories")):
            self.assertIn(f'case ArmorSub::{sub}: return "{label}"', header)
        # ArmorTypeApparelHead is the head SLOT: caps, hats, masks, glasses and
        # every Neuroamp share it. The classifiers have to separate them by
        # their own keyword or name before that slot test runs.
        classifier = self.function_body("ClassifyArmorSub")
        self.assertIn('find("armortypeneuroamp")', classifier)
        for term in ("glasses", "neuroboost", "cap", "hat", "beanie", "headwear"):
            self.assertIn(f'"{term}"', classifier)
        self.assertLess(classifier.index('find("armortypeneuroamp")'),
                        classifier.index('find("apparelhead")'))
        self.assertLess(classifier.index('find("inventorycategoryclothing")'),
                        classifier.index('find("apparelhead")'))

    def test_full_suits_win_over_their_helmet_keyword(self) -> None:
        # Starborn Spacesuit Tempus and Solis were filed under Helmets because a
        # full suit carries ArmorTypeSpacesuitHelmet (it includes the helmet).
        # All 24 catalog records holding body+helmet keywords are suits.
        classifier = self.function_body("ClassifyArmorSub")
        self.assertLess(classifier.index('find("spacesuitbody")'),
                        classifier.index('find("spacesuithelmet")'),
                        "body must be tested before the helmet it ships with")
        solis = ("starborn spacesuit solis spacesuit_starborn_unity_01_playerorfollower "
                 "manufacturerstarborn;armortypespacesuitbody;inventorycategoryarmorsuit;"
                 "armortypespacesuithelmet;armortypespacesuitbackpack")
        self.assertIn("spacesuitbody", solis)
        self.assertIn("spacesuithelmet", solis)

    def test_actor_skins_are_hidden_without_a_toggle(self) -> None:
        # Armor/Other was nothing but actor skins: Terrormorph, Groat, MiniBot,
        # turret corpse and SkinModel* records. They are ARMO forms with no
        # InventoryCategory*/ArmorType* keyword, so the rule is structural and
        # is not gated by "Hide crew/enemy/creature" - a skin is never an item.
        classifier = self.function_body("IsActorSkinArmor")
        self.assertIn("a_form->GetFormType() != RE::FormType::kARMO", classifier)
        self.assertIn('keywords.find("inventorycategory")', classifier)
        self.assertIn('keywords.find("armortype")', classifier)
        self.assertNotIn("HasPlayerVisibleName", classifier)
        start = self.source.index("void ShopBrowser::BuildFormList()")
        end = self.source.index("void ShopBrowser::ApplyViewOrder()", start)
        form_list = self.source[start:end]
        self.assertIn("if (IsActorSkinArmor(form))", form_list)
        self.assertNotIn("hideCrewEnemyCreature && IsActorSkinArmor", form_list)
        self.assertNotIn("hideTurrets && IsActorSkinArmor", form_list)
        # Must run before the armor subcategory filter, otherwise the skins are
        # still counted as Armor/Other rows.
        self.assertLess(form_list.index("if (IsActorSkinArmor(form))"),
                        form_list.index("selectedArmorSub != ArmorSub::kAll"))

    def test_actor_skin_rule_never_hides_a_wearable(self) -> None:
        rows = load_armo_rows()
        self.assertGreater(len(rows), 1000, "fixture should hold the whole ARMO catalog")
        wearable = [row for row in rows if not is_actor_skin(row)]
        self.assertGreater(len(wearable), 800)
        for row in wearable:
            keywords = row["keywords"].lower()
            self.assertTrue("inventorycategory" in keywords or "armortype" in keywords,
                            row["editorId"])
            self.assertNotEqual(armor_sub(row), "Other", row["editorId"])

    def test_actor_skin_rule_empties_the_armor_other_bucket(self) -> None:
        rows = load_armo_rows()
        other = [row for row in rows if armor_sub(row) == "Other"]
        self.assertTrue(other, "fixture should still contain the skin rows that filled Other")
        undeclared = [row for row in other if not is_actor_skin(row)]
        self.assertEqual(undeclared, [], "Armor/Other must be all actor skins")
        # Every record the rule hides is recognisable as a non-item: a Skin_*
        # editor ID, a NON PLAYABLE marker, or the Minibot default body. The
        # Redeemed suits name themselves ".Spacesuit." but are non-playable
        # destructible-armour shells (DisplayObjectTypeMiscMedium +
        # DestructibleArmorKeyword), so they belong with the skins.
        skins = [row for row in rows if is_actor_skin(row)]

        def non_item_marker(row: dict[str, str]) -> bool:
            ident = f'{row["editorId"]} {row["name"]}'.replace(" ", "").lower()
            return "skin" in ident or "notplayable" in ident or "nonplayable" in ident

        odd = [row["editorId"] for row in skins if not non_item_marker(row)]
        self.assertEqual(odd, ["SFBGS022_MiniBotA_ModelG_Default"])
        redeemed = [row for row in rows if row["editorId"].startswith("SFBGS001_Spacesuit_Redeemed_")]
        self.assertEqual(len(redeemed), 4)
        for row in redeemed:
            self.assertTrue(is_actor_skin(row), row["editorId"])
            self.assertIn("non playable", row["name"].lower())

    def test_weapon_fallbacks_cover_common_item_types(self) -> None:
        classifier = self.function_body("ClassifyWeaponSub")
        for term in ("pistol", "shotgun", "rifle", "sniper", "heavy weapon", "melee", "grenade"):
            self.assertIn(f'"{term}"', classifier)
        self.assertLess(classifier.index("WeaponSub::kHeavy"), classifier.index("WeaponSub::kSniper"))

    def test_hide_test_debug_uses_token_boundaries_across_editor_id_and_name(self) -> None:
        classifier = self.function_body("IsTestDebugIdentity")
        self.assertIn("CaseFold(a_editorId)", classifier)
        self.assertIn("CaseFold(a_displayName)", classifier)
        self.assertIn('hasEditorMarker("test")', classifier)
        self.assertIn("!std::isalnum", classifier)
        for marker in ("noclutter", "dummy", "template", "debug", "test", "not playable", "notplayable"):
            self.assertIn(f'"{marker}"', classifier)
        self.assertIn("IsTestDebugIdentity(ed, name)", self.source)
        self.assertIn("const std::string name = named ? FormDisplayName(form) : std::string{};",
                      self.source)
        # The negative regression below documents why raw substring matching is wrong.
        self.assertNotIn('upper.find("test")', self.source)

    def test_hide_crew_enemy_creature_keeps_melee_with_unarmed_block_keyword(self) -> None:
        classifier = self.function_body("IsCrewEnemyCreatureIdentity")
        for marker in ("crew", "companion", "follower", "enemy", "enemies", "hostile", "opponent", "creature", "animal", "beast", "monster"):
            self.assertIn(f'"{marker}"', classifier)
        self.assertNotIn("unarmed", classifier)
        # Regression identities reconstructed from the captured form table:
        # Crucible is an ordinary melee record carrying CanUnarmedBlockKeyword;
        # hostile records are named as such and remain hidden.
        crucible_identity = "kgcdoom_weap_crucibleblade crucible blade canunarmedblockkeyword"
        vortex_hostile_identity = "sfbgs001_vortex_painblade vortex enemies only ma_painblade"
        creature_attack_identity = "sfbgs001_unarmedparasitea_vortexspawn weapontypeunarmed"
        self.assertFalse(any(marker in crucible_identity for marker in ("enemy", "creature", "crew")))
        self.assertTrue(any(marker in vortex_hostile_identity for marker in ("enemy", "enemies", "hostile")))
        # Creature-attack classification is provided by its explicit attack
        # identity, not its common WeaponTypeUnarmed keyword.
        self.assertTrue("unarmedparasite" in creature_attack_identity)
        self.assertTrue("weapontypeunarmed" in creature_attack_identity)
        self.assertIn("IsCrewEnemyCreatureIdentity(identity)", self.source)
        self.assertIn('HasAnyText(a_identity, "crew", "companion", "follower", "enemy", "enemies", "hostile",', classifier)

    def test_hide_turrets_is_its_own_toggle_over_weapon_and_armor_turrets(self) -> None:
        classifier = self.function_body("IsTurretIdentity")
        # Stem match, not a word match: the keyword atoms glue the stem to the
        # rest of the token ("SpaceshipTurretWeapon", "s_40_ShipTurret_LaserA").
        self.assertIn('a_identity.find("turret")', classifier)
        self.assertNotIn("HasAnyText", classifier)
        start = self.source.index("void ShopBrowser::BuildFormList()")
        end = self.source.index("void ShopBrowser::ApplyViewOrder()", start)
        form_list = self.source[start:end]
        self.assertIn("if (hideTurrets && HasPlayerVisibleName(form))", form_list)
        turret_block_start = form_list.index("if (hideTurrets && HasPlayerVisibleName(form))")
        # Must not be folded into the ship/hair filter: the whole point is a
        # separate switch, and the ship identity check only sees whole words.
        self.assertLess(turret_block_start, form_list.index("if (hideShipHair && HasPlayerVisibleName(form))"))
        self.assertIn("IsTurretIdentity(identity)", form_list)
        # Captured identities: ship turret WEAPs (name/EDID/keyword forms) and
        # robot turret skins. A weapon family name that merely contains "tur"
        # must not match.
        turret_identities = (
            "pulse laser turret swl_laser_spaceturret attachableembeddedweaponkeyword",
            "pulse laser swl_laser_shinigami_800_3l3w3h_turret spaceshipkineticweapon",
            "obliterator 250mev alpha turret swc_particle_lightscythe_obliterator_250mev_alpha_turret_lvl76 spaceshipturretweapon",
            "skin_turret_laser_01_short skin_turret_laser_01_short",
        )
        for identity in turret_identities:
            self.assertIn("turret", identity)
        self.assertNotIn("turret", "rattler receiver textures/weapons/rattler/receiver_color.dds")

    def test_hide_turrets_also_drops_unbuyable_ammo(self) -> None:
        # Two kinds of AMMO record can never be sold or previewed: the dummy
        # form each crate NIF is registered under ("(unnamed) [002184A8] ·
        # typee_xlarge") and the rounds ship weapons/creatures fire, which
        # carry no model because the tracer comes from the weapon.
        classifier = self.function_body("IsUnbuyableAmmo")
        self.assertIn("a_form->GetFormType() != RE::FormType::kAMMO", classifier)
        self.assertIn("BGSModelMaterialSwap::model.c_str()", classifier)
        self.assertIn("HasPlayerVisibleName(a_form)", classifier)
        start = self.source.index("void ShopBrowser::BuildFormList()")
        end = self.source.index("void ShopBrowser::ApplyViewOrder()", start)
        form_list = self.source[start:end]
        self.assertIn("if (hideTurrets && IsUnbuyableAmmo(form))", form_list)
        # Inside the turret toggle, and before the armor subcategory split, so
        # the rows never reach a bucket they do not belong to.
        self.assertLess(form_list.index("if (hideTurrets && IsUnbuyableAmmo(form))"),
                        form_list.index("if (hideTurrets && HasPlayerVisibleName(form))"))
        self.assertLess(form_list.index("if (hideTurrets && IsUnbuyableAmmo(form))"),
                        form_list.index("selectedArmorSub != ArmorSub::kAll"))

    def test_ammo_rule_hides_only_rows_a_shop_cannot_sell(self) -> None:
        rows = load_csv_rows(AMMO_FIXTURE)
        self.assertEqual(len(rows), 150, "fixture holds every AMMO record in Starfield.esm")
        hidden = [row for row in rows if is_unbuyable_ammo(row)]
        kept = [row for row in rows if not is_unbuyable_ammo(row)]
        # 51 unnamed + 18 with an empty MODL + 9 test-rocket + 27 missile-named.
        self.assertEqual(len(hidden), 105)
        self.assertEqual(len(kept), 45)
        for row in hidden:
            unnamed = row["name"].strip().lower() in ("", "(unnamed)", "unnamed")
            self.assertTrue(
                unnamed or not row["modelPath"].strip()
                or "testspaceshiprocketprojectile" in row["modelPath"].lower()
                or ammo_sub(row) in ("Missile", "ShipCreature"), row["formID"])
        # Every surviving row is a real crate with a name and a model: the
        # shoppable cartridges, the mines and the grenade round.
        for row in kept:
            self.assertTrue(row["modelPath"].strip(), row["name"])
            self.assertNotIn(row["name"].strip().lower(), ("", "(unnamed)", "unnamed"))
            self.assertNotIn(ammo_sub(row), ("Missile", "ShipCreature"), row["name"])
            self.assertNotIn("testspaceshiprocketprojectile", row["modelPath"].lower())
        # All but the bare `Mine` round point at a crate NIF under ammo_crates
        # (`Mine` uses Weapons\spacemine\SpaceMine.nif, still a real mesh).
        crates = [row for row in kept if "ammo_crates" in row["modelPath"].lower()]
        self.assertEqual(len(crates), 44)
        kept_names = {row["name"] for row in kept}
        for cartridge in (".27 Caliber", "1.5kV LZR Cartridge", "11mm Caseless", "7.77mm Caseless"):
            self.assertIn(cartridge, kept_names)
        # Mines and the grenade are thrown by the player, not fired by a ship,
        # so they stay even though they share the generic crate meshes.
        for explosive in ("Mine, CE-14", "Mine, CE-155", "Grenade"):
            self.assertIn(explosive, kept_names)
        # The rows the catalog displayed as noise are the hidden ones.
        hidden_ids = {row["formID"] for row in hidden}
        for form_id in ("002184A8", "00004249", "001504F0", "00087588", "000297F4"):
            self.assertIn(form_id, hidden_ids)
        # NAM1 (shell casing) is empty for most rounds and must not be the
        # test's idea of "this record has no model": .27 Caliber ejects a
        # casing, Missile, CE-13 does not, and both have a crate.
        by_id = {row["formID"]: row for row in rows}
        self.assertTrue(by_id["0023155F"]["shellCasing"])
        self.assertFalse(by_id["00004249"]["shellCasing"])
        self.assertTrue(by_id["00004249"]["modelPath"])

    def test_ship_hair_filter_is_token_aware_and_covers_bare_forms(self) -> None:
        start = self.source.index("if (hideShipHair && HasPlayerVisibleName(form))")
        end = self.source.index("// Armor subcategory", start)
        block = self.source[start:end]
        self.assertIn('HasAnyText(identity, "spaceship", "ship", "hair", "beard")', block)
        self.assertIn('tok == "naked" || tok == "nude"', block)
        self.assertIn("const std::string edFold = CaseFold(ed)", block)

    def test_plugin_hide_toggles_and_audio_controls_are_wired(self) -> None:
        start = self.source.index('ImGui::BeginChild("reference_plugins"')
        end = self.source.index('ImGui::EndChild();', start)
        plugin_panel = self.source[start:end]
        # 0.9.0 moved the five hide filters into the catalog's own toolbar popup:
        # they decide what the catalog contains, which is not a property of the
        # selected plugin, and the plugins pane now carries the favourites view.
        for label in ("Hide test/debug", "Hide ship/hair/etc",
                      "Hide crew/enemy/creature", "Hide turrets/ship ammo",
                      "Hide 'report as bug' items"):
            self.assertNotIn(label, plugin_panel)
        self.assertIn("###favourites", plugin_panel)
        self.assertIn("&favouritesOnly", plugin_panel)
        toolbar_start = self.source.index("// Show one subtype selector only for the active catalog type.")
        toolbar_end = self.source.index("// Use a globally selected atlas face", toolbar_start)
        toolbar = self.source[toolbar_start:toolbar_end]
        self.assertIn('ImGui::Button("Hide")', toolbar)
        self.assertIn('ImGui::BeginPopup("##shop_hide")', toolbar)
        for label, flag in (("Hide test/debug", "hideTestDebug"),
                            ("Hide 'report as bug' items", "hideReportBug"),
                            ("Hide ship/hair/etc", "hideShipHair"),
                            ("Hide crew/enemy/creature", "hideCrewEnemyCreature"),
                            ("Hide turrets/ship ammo", "hideTurrets")):
            self.assertIn(f'hideToggle("{label}", {flag})', toolbar)
        # 0.8.350 replaced the single Music checkbox with an Audio popup: one
        # volume slider and one mute per channel, plus a track picker that is
        # independent of the visual theme.
        self.assertIn('ImGui::Button("Audio")', toolbar)
        self.assertIn('ImGui::BeginPopup("##shop_audio")', toolbar)
        self.assertIn('ImGui::SliderInt("##music_volume"', toolbar)
        self.assertIn('ImGui::SliderInt("##sfx_volume"', toolbar)
        self.assertIn('ImGui::Checkbox("Mute##music_mute"', toolbar)
        self.assertIn('ImGui::Checkbox("Mute##sfx_mute"', toolbar)
        self.assertNotIn('musicEnabled', toolbar)
        self.assertNotIn('musicEnabled', plugin_panel)
        self.assertIn("if (hideCrewEnemyCreature && HasPlayerVisibleName(form))", self.source)
        crew_filter = self.function_body("IsCrewEnemyCreatureIdentity")
        self.assertIn('"companion"', crew_filter)
        self.assertIn('"hostile"', crew_filter)
        self.assertIn('"animal"', crew_filter)
        self.assertIn('"enemies"', crew_filter)
        self.assertNotIn('"unarmed"', crew_filter)
        self.assertNotIn('a_identity.find("unarmed")', crew_filter)
        self.assertNotIn('identity.find("enemy")', self.source)
        self.assertNotIn('identity.find("creature")', self.source)
        self.assertNotIn('identity.find("crew")', self.source)
        self.assertIn('HasAnyText(identity, "spaceship", "ship", "hair", "beard")', self.source)
        self.assertIn("CaseFold(ed + \" \" + name + \" \" + FormKeywords(form, 512))",
                      self.source)
        self.assertIn("ImGui::SetNextItemOpen(false, ImGuiCond_Always)", self.source)
        self.assertIn("materialHeaderLabel != snap.label", self.source)
        self.assertIn('HasAnyText(identity, "spaceship", "ship", "hair", "beard")', self.source)
        self.assertIn("CaseFold(ed + \" \" + name + \" \" + FormKeywords(form, 512))",
                      self.source)
        self.assertIn("ImGui::SetNextItemOpen(false, ImGuiCond_Always)", self.source)
        self.assertIn("materialHeaderLabel != snap.label", self.source)

    def test_subcategory_controls_exist_for_each_item_group(self) -> None:
        for kind in ("armor", "weapon", "ammo", "misc", "aid", "book", "key"):
            self.assertIn(f'"##toolbar_{kind}sub"', self.source)
            self.assertIn(f"Classify{kind.title()}Sub(", self.source)

    # -- list/search hot path ----------------------------------------------

    def test_item_search_and_visible_rows_are_cached_not_rebuilt_per_frame(self) -> None:
        # Searching used to re-derive FormIdentitySearchText - a display name, a
        # model path and two std::format calls - for every row, on every frame,
        # so typing into a 9,000-row catalog paid for 9,000 records 60 times a
        # second. The keys are built once per catalog rebuild and the visible
        # set only when the input it came from changes.
        header = (SOURCE.parent / "ShopExplorer.h").read_text(encoding="utf-8-sig")
        self.assertIn("std::vector<std::string> searchKeys;", header)
        self.assertIn("std::vector<std::string> nameSortKeys;", header)
        self.assertIn("std::vector<std::size_t> visRows;", header)
        self.assertIn("void EnsureSearchKeys();", header)
        draw = self.source[self.source.index("void ShopBrowser::DrawImGui()"):]
        gate = "if (visRowsStamp != listStamp || visRowsSearch != search)"
        self.assertIn(gate, draw)
        block = draw[draw.index(gate):]
        block = block[:block.index("shownRows =")]
        self.assertIn("EnsureSearchKeys();", block)
        self.assertEqual(block.count("std::format"), 0)
        self.assertNotIn("std::vector<std::size_t> visRows;", block)
        # The keys depend on the rows, not on their order: ApplyViewOrder bumps
        # listStamp on every header click, so a cache gated on that rebuilt all
        # 9,161 search keys after every sort while a term was in the box.
        self.assertIn("std::uint64_t formsStamp{ 1 };", header)
        keys = self.source[self.source.index("void ShopBrowser::EnsureSearchKeys()"):]
        keys = keys[:keys.index("\n\tvoid ", 10)]
        self.assertIn("searchKeysStamp == formsStamp", keys)
        self.assertIn("searchKeysStamp = formsStamp;", keys)
        self.assertNotIn("searchKeysStamp = listStamp;", keys)

    def test_list_rebuild_looks_up_each_records_name_and_keywords_once(self) -> None:
        # One owner table, one display name, one identity fold and one
        # classifier text per record: the five hide toggles used to rebuild
        # "edid + name + 512 chars of keywords" separately for each of them.
        start = self.source.index("void ShopBrowser::BuildFormList()")
        end = self.source.index("void ShopBrowser::ApplyViewOrder()", start)
        form_list = self.source[start:end]
        self.assertEqual(form_list.count("FormDisplayName(form)"), 1)
        self.assertEqual(form_list.count("FormKeywords(form, 512)"), 1)
        self.assertEqual(form_list.count("CategorySearchText(form)"), 1)
        self.assertEqual(form_list.count("CategoryModelText(form)"), 1)
        self.assertNotIn("RuntimeOwnerOfFormID(dataHandler, form->GetFormID())", form_list)
        self.assertIn("ownerOfFormID(form->GetFormID())", form_list)
        self.assertIn("++listStamp;", form_list)
        # A rebuild is the only thing that changes the rows, so it is the only
        # thing that invalidates the per-row keys.
        self.assertIn("++formsStamp;", form_list)
        # The gather half of the rebuild used to sit outside every timer, so
        # the total could not say which half a change moved.
        self.assertIn("list phases: gather {:.2f} ms, filter {:.2f} ms", form_list)
        self.assertLess(form_list.index("const auto gatherStarted"),
                        form_list.index("std::vector<RE::TESForm*> typed;"))
        order = self.source[self.source.index("void ShopBrowser::ApplyViewOrder()"):]
        order = order[:order.index("\n\tvoid ", 10)]
        self.assertIn("++listStamp;", order)

    def test_search_keys_do_not_run_the_weapon_assembly_scan(self) -> None:
        # The first keystroke after a list rebuild cost 450 ms: 9,161 keys, and
        # SearchKeyFor called FormModelPath, which for each of the 202
        # dummy-base weapons in the catalog runs CollectWeaponAssemblyNifs (the
        # "dummy weapon ... OMODs: N owner, 264 cast" scan). The key wants the
        # model string the record declares, not the preview's assembled parts.
        key_start = self.source.index("std::string SearchKeyFor(")
        key = self.source[key_start:self.source.index("std::string FormPreviewLabel(", key_start)]
        self.assertIn("RawModelPath(a_form)", key)
        for assembled in ("FormModelPath(", "FormModelPaths("):
            self.assertNotIn(assembled, key)
        raw_start = self.source.index("std::string RawModelPath(")
        raw = self.source[raw_start:key_start]
        for forbidden in ("CollectWeaponAssemblyNifs", "SkipModelRequested", "EnsureSkipModels", "FormModelPaths"):
            self.assertNotIn(forbidden, raw, forbidden)
        # The build is timed, so the number can be read in the next session's log.
        keys = self.source[self.source.index("void ShopBrowser::EnsureSearchKeys()"):]
        keys = keys[:keys.index("\n\tvoid ", 10)]
        self.assertIn("search key(s) built in {:.2f} ms", keys)

    def test_light_and_medium_file_owners_are_not_crossed(self) -> None:
        # Reported: "all plugins" lists every Amara item, clicking AmaraArmor.esm
        # shows 0 records. AmaraArmor.esm is an ESL-flagged .esm, so its forms are
        # FE06Exxx - 0xFE names a light file and 0xFD a medium one, each indexed
        # by the middle 12 bits into its own table. The lookup had the two tables
        # swapped, so a light plugin indexed the 13-entry medium table (out of
        # range, no owner, 0 records) and a medium plugin showed whichever light
        # file happened to sit at its index.
        start = self.source.index("void ShopBrowser::BuildFormList()")
        end = self.source.index("void ShopBrowser::ApplyViewOrder()", start)
        form_list = self.source[start:end]
        light = form_list.index("if (top == 0xFE) {")
        medium = form_list.index("if (top == 0xFD) {")
        light_branch = form_list[light:medium]
        medium_branch = form_list[medium:form_list.index("return ownerByIndex[top];", medium)]
        self.assertIn("ownerSmall[middle]", light_branch)
        self.assertNotIn("ownerMedium[middle]", light_branch)
        self.assertIn("ownerMedium[middle]", medium_branch)
        self.assertNotIn("ownerSmall[middle]", medium_branch)
        # Slot order is the index the FormID carries, so a file-less hole has to
        # keep its place instead of shifting every later plugin up one.
        self.assertIn("ownerSmall.push_back(file && file->fileName[0] ? file->fileName : nullptr);", form_list)
        self.assertIn("ownerMedium.push_back(file && file->fileName[0] ? file->fileName : nullptr);", form_list)
        # RuntimeOwnerOfFormID is the canonical resolver the rest of the file
        # calls; the inline table was a copy of it that drifted. Both must read
        # 0xFE as the light file, or they will disagree again.
        canonical = self.source[self.source.index("std::string RuntimeOwnerOfFormID("):]
        canonical = canonical[:canonical.index("\n\t\t}", 10)]
        self.assertIn("top == 0xFE ? a_data->compiledFileCollection.smallFiles", canonical)
        self.assertIn("a_data->compiledFileCollection.mediumFiles", canonical)

    def test_all_types_view_applies_per_category_subfilters(self) -> None:
        start = self.source.index("void ShopBrowser::BuildFormList()")
        end = self.source.index("void ShopBrowser::ApplyViewOrder()", start)
        all_types = self.source[start:end]
        self.assertIn("if (selectedType == BrowseType::kAllTypes)", all_types)
        # Books, Misc and Aid also read the NIF: the model, not the keyword,
        # decides whether a record is a dataslate or a printed book, a Sabacc
        # card or an ore chunk, a QuickSnack or a chem inhaler.
        model_args = {
            "Book": "catText(), FormModelPath(form)",
            "Misc": "catText(), catModel()",
            "Aid": "catText(), catModel()",
        }
        for kind in ("Weapon", "Armor", "Ammo", "Misc", "Aid", "Book", "Key"):
            args = model_args.get(kind, "catText()")
            self.assertIn(f"Classify{kind}Sub({args})", all_types)

    def test_toolbar_shows_exactly_the_selected_types_subcategory(self) -> None:
        start = self.source.index("// Show one subtype selector only for the active catalog type.")
        end = self.source.index("placeInline(60.0f * uiScale);", start)
        toolbar = self.source[start:end]
        self.assertIn("switch (selectedType)", toolbar)
        for kind in ("armor", "weapon", "ammo", "misc", "aid", "book", "key"):
            self.assertIn(f"case BrowseType::k{kind.title()}:", toolbar)
            self.assertIn(f'drawSubcategory("##toolbar_{kind}sub"', toolbar)
        self.assertEqual(toolbar.count("drawSubcategory("), 7)
        self.assertIn("All Types and Ingredients have no subtype control", toolbar)
        self.assertNotIn("selectedType == BrowseType::kAllTypes", toolbar)

    def test_all_types_filter_choices_do_not_require_multiple_subcategory_controls(self) -> None:
        start = self.source.index("void ShopBrowser::BuildFormList()")
        end = self.source.index("void ShopBrowser::ApplyViewOrder()", start)
        form_list = self.source[start:end]
        self.assertIn("if (selectedType == BrowseType::kAllTypes)", form_list)
        model_args = {
            "Book": "catText(), FormModelPath(form)",
            "Misc": "catText(), catModel()",
            "Aid": "catText(), catModel()",
        }
        for kind in ("Weapon", "Armor", "Ammo", "Misc", "Aid", "Book", "Key"):
            args = model_args.get(kind, "catText()")
            self.assertIn(f"Classify{kind}Sub({args})", form_list)
        self.assertIn("type == RE::FormType::kWEAP && selectedWeaponSub != WeaponSub::kAll", form_list)
        self.assertIn("type == RE::FormType::kARMO && selectedArmorSub != ArmorSub::kAll", form_list)

    def test_selected_mod_route_never_drops_item_before_direct_inventory_add(self) -> None:
        start = self.source.index("void ShopBrowser::ConfirmBasket()")
        end = self.source.index("void ShopBrowser::RefreshKeywords()", start)
        transfer = self.source[start:end]
        self.assertIn("SpikeDeliverWithMods(player, item.bound, item.count, picks)", transfer)
        self.assertNotIn("route=direct-inventory", transfer)
        # The mod route is chosen by "has OMODs", not by form type: armor
        # rarity rolls ride the same AttachMod path (see test_armor_rarity).
        self.assertIn("const bool hasSelectedMods = !item.selectedOmods.empty();", transfer)
        self.assertIn('" - selected mods attached; pick it up"', transfer)
        self.assertIn("SpikeOutcome::kOnFloorMods", transfer)
        self.assertIn("SpikeOutcome::kTransferPendingMods", transfer)

    def test_report_bug_placeholders_have_their_own_toggle(self) -> None:
        start = self.source.index("void ShopBrowser::BuildFormList()")
        end = self.source.index("void ShopBrowser::ApplyViewOrder()", start)
        form_list = self.source[start:end]
        self.assertIn("if (hideReportBug && named && IsReportBugName(name))", form_list)
        self.assertIn("const std::string name = named ? FormDisplayName(form) : std::string{};",
                      form_list)
        # Run before the test/debug filter: the ESM shop gives these rows real
        # names ("DL-19 (report this as a bug)"), so IsTestDebugIdentity misses
        # them and only the explicit marker can drop them.
        self.assertLess(form_list.index("IsReportBugName"), form_list.index("IsTestDebugIdentity"))
        helper = self.function_body("IsReportBugName")
        self.assertIn('find("report this as a bug")', helper)
        popup = self.source[self.source.index('ImGui::BeginPopup("##shop_hide")'):]
        popup = popup[:popup.index("ImGui::EndPopup();")]
        self.assertIn('hideToggle("Hide \'report as bug\' items", hideReportBug)', popup)

    def test_keyword_list_is_a_default_closed_header(self) -> None:
        anchor = self.source.index("const std::string keywordHeader")
        block = self.source[anchor - 900:anchor + 1300]
        # 15+ keywords pushed the parts selectors below the fold; the list now
        # lives behind a header that starts closed for every new selection.
        self.assertIn("ImGui::CollapsingHeader(keywordHeader.c_str())", block)
        self.assertLess(block.index("SetNextItemOpen(false, ImGuiCond_Always)"), block.index("ForEachKeyword"))
        self.assertNotIn('ImGui::Text("Keywords:")', block)

    def test_books_split_by_model_then_name(self) -> None:
        header = (SOURCE.parent / "ShopExplorer.h").read_text(encoding="utf-8-sig")
        self.assertIn("kAll = 0, kSlates, kNotes, kLogs, kGuides, kOther", header)
        self.assertIn('case BookSub::kOther: return "Misc / Other";', header)
        # Removed on request: the "posters" were dataslates, not art.
        self.assertNotIn("kPosters", header)
        self.assertIn("ClassifyBookSub(catText(), FormModelPath(form))", self.source)

        body = self.function_body("ClassifyBookSub")
        # The NIF decides the first split. ObjectTypeDataSlate is carried by
        # printed books too (Race to the Heavens is BookLarge01.nif), so a
        # dataslate model must win before any keyword/name test.
        self.assertIn('model.find("dataslate")', body)
        self.assertLess(body.index('model.find("dataslate")'), body.index('BookSub::kNotes'))
        self.assertLess(body.index('model.find("notepad")'), body.index('BookSub::kLogs'))
        # A book model with no note/log word is a book, not Misc / Other.
        self.assertIn('model.find("book")', body)
        self.assertLess(body.index('model.find("book")'), body.rindex("BookSub::kGuides"))
        for term in ('"interview"', '"communique"'):
            self.assertIn(term, body)

    # -- favourites ---------------------------------------------------------

    def test_favourites_view_is_built_from_ids_not_from_the_current_shelf(self) -> None:
        start = self.source.index("void ShopBrowser::BuildFormList()")
        end = self.source.index("void ShopBrowser::ApplyViewOrder()", start)
        body = self.source[start:end]
        self.assertIn("if (favouritesOnly) {", body)
        # Before the selected-plugin guard: a favourite is listed whatever
        # plugin and category are selected, so those must not be consulted.
        self.assertLess(body.index("if (favouritesOnly) {"), body.index("if (!dataHandler) {"))
        self.assertIn("RE::TESForm::LookupByID(id)", body)
        self.assertIn("IsCatalogRecordType(form->GetFormType())", body)
        # An id that no longer resolves is counted, never drawn as a blank row.
        self.assertIn("no longer resolve in this load order", body)

    def test_star_shares_the_name_cell_and_cannot_steal_the_row_select(self) -> None:
        # The star used to own a fixed column beside NAME with the row label in
        # the next cell, and after a NAME sort the two could land on different
        # rows: a star alone on a blank line, its name one row lower, and that
        # row twice as tall as every other. One cell cannot disagree with
        # itself, so there is no STAR column and no per-row star width.
        self.assertIn('ImGui::BeginTable("reference_form_table", 5', self.source)
        self.assertNotIn('ImGui::TableSetupColumn("STAR"', self.source)
        rows = self.source[self.source.index("while (clipper.Step())"):]
        self.assertIn('ImGui::InvisibleButton("##favourite", ImVec2(kStarSlot, lineHeight))', rows)
        # Submitted before the SpanAllColumns row selectable, and laid out on
        # the same line as the label, so it owns the hover: clicking the star
        # must not select or basket the row.
        self.assertLess(rows.index("ToggleFavourite("),
                        rows.index("ImGuiSelectableFlags_SpanAllColumns"))
        self.assertLess(rows.index('ImGui::InvisibleButton("##favourite"'),
                        rows.index("ImGui::SameLine(0.0f, kStarGap);"))
        self.assertLess(rows.index("ImGui::SameLine(0.0f, kStarGap);"),
                        rows.index("ImGuiSelectableFlags_SpanAllColumns"))

    def test_the_star_is_drawn_not_typed(self) -> None:
        """U+2605 is in no font this overlay loads, so it must not be a literal.

        Segoe UI, Segoe UI Semibold, Arial and Segoe UI Symbol all report the
        code point missing under GetGlyphIndicesW; the atlas then bakes the
        notdef diamond, which is exactly what the first favourites build showed
        on every row and beside All Plugins.
        """
        self.assertNotIn("\u2605", self.source)
        self.assertNotIn("\u2606", self.source)
        self.assertIn("void DrawStarGlyph(", self.source)
        self.assertGreaterEqual(self.source.count("DrawStarGlyph(ImGui::GetWindowDrawList()"), 4)
        # A hollow star is concave, so its outline is segments rather than one
        # polyline with joins.
        body = self.source[self.source.index("void DrawStarGlyph("):]
        body = body[:body.index("ArmorSub ClassifyArmorSub")]
        self.assertIn("AddConvexPolyFilled", body)
        self.assertIn("% (kPoints * 2)", body)
        # The header star shares the NAME cell with the NAME sort button, so
        # the star column of rows and header line up without a table column.
        self.assertIn("ImVec2(kStarSlot, lineHeight)", self.source)
        self.assertIn("ImGui::SameLine(0.0f, kStarGap);", self.source)
        # The star is visual only; sortColumn keeps its 0-4 meaning, so the
        # column indices written by earlier builds still name the right column.
        self.assertIn("sortColumn = c;", self.source)

    def test_favourites_survive_a_restart_beside_the_ini(self) -> None:
        self.assertIn('L"SFShopExplorerFavourites.txt"', self.source)
        self.assertIn('out << std::format("{:08X}\\n", id);', self.source)
        self.assertIn("LoadFavourites();", self.source)
        # Unstarring inside the favourites view rebuilds the list after the
        # table, never inside the loop the clipper is walking.
        self.assertIn("rebuildAfterTable = rebuildAfterTable || favouritesOnly;", self.source)
        after = self.source[self.source.index("if (rebuildAfterTable) {"):]
        self.assertIn("BuildFormList();", after[:200])


if __name__ == "__main__":
    unittest.main()
