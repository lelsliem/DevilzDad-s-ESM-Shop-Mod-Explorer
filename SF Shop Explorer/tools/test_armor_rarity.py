"""Regression checks for armor/helmet/backpack rarity (legendary effects).

Weapons discover their legendary-effect pool by walking a per-weapon OMOD
family during assembly. Armor has no assembly pass, so its pool is the
generated per-class table in src/OmodClassTable.h. These checks pin the table
contents, the generator rule that produces them, and the runtime wiring that
turns a rarity pick into attached OMODs.
"""
from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TABLE = ROOT / "src" / "OmodClassTable.h"
SOURCE = ROOT / "src" / "ShopExplorer.cpp"
GENERATOR = ROOT / "tools" / "gen_omod_class_table.py"

# slot: 0 = suit/apparel body, 1 = helmet/hat, 2 = backpack
ARMOR_SLOT_BODY = 0
ARMOR_SLOT_HELMET = 1
ARMOR_SLOT_PACK = 2


class ArmorRarityTableTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.table = TABLE.read_text(encoding="utf-8")

    def entries(self) -> list[tuple[int, int, int]]:
        start = self.table.index("kOmodArmorLegendaryTable[]")
        end = self.table.index("};", start)
        rows = re.findall(r"\{ 0x([0-9A-Fa-f]{8})u, (\d)u, (\d)u \}", self.table[start:end])
        self.assertTrue(rows, "armor legendary table not found")
        return [(int(omod, 16), int(slot), int(rank)) for omod, slot, rank in rows]

    def test_table_covers_every_vanilla_armor_class(self) -> None:
        rows = self.entries()
        slots = [slot for _, slot, _ in rows]
        self.assertEqual(slots.count(ARMOR_SLOT_BODY), 23)
        self.assertEqual(slots.count(ARMOR_SLOT_HELMET), 6)
        self.assertEqual(slots.count(ARMOR_SLOT_PACK), 3)
        self.assertEqual(len(rows), 32)
        ranks = {rank for _, _, rank in rows}
        self.assertEqual(ranks, {1, 2, 3})

    def test_table_is_sorted_for_binary_search(self) -> None:
        omids = [omod for omod, _, _ in self.entries()]
        self.assertEqual(omids, sorted(omids), "LookupOmodArmorLegendary binary-searches this table")
        self.assertEqual(len(omids), len(set(omids)))

    def test_known_effects_keep_their_class_and_rank(self) -> None:
        by_id = {omod: (slot, rank) for omod, slot, rank in self.entries()}
        # mod_Legendary_Armor_03_Incendiary
        self.assertEqual(by_id[0x00002983], (ARMOR_SLOT_BODY, 3))
        # mod_Legendary_Armor_01_Chameleon
        self.assertEqual(by_id[0x001336C1], (ARMOR_SLOT_BODY, 1))
        # mod_Legendary_Helmet_01_OxygenFilter
        self.assertEqual(by_id[0x000690AE], (ARMOR_SLOT_HELMET, 1))
        # mod_Legendary_Pack_03_AssistedCarry
        self.assertEqual(by_id[0x002EDE4F], (ARMOR_SLOT_PACK, 3))
        # mod_Legendary_Armor_4_None is an empty placeholder, not an effect.
        self.assertNotIn(0x003198F0, by_id)

    def test_lookup_is_declared_and_binary_searches_the_armor_table(self) -> None:
        lookup = self.table[self.table.index("LookupOmodArmorLegendary"):]
        self.assertIn("kOmodArmorLegendaryTable", lookup)
        self.assertIn("kOmodArmorLegendaryTable[mid].omod < a_omod", lookup)
        self.assertIn("return &kOmodArmorLegendaryTable[lo];", lookup)
        self.assertIn("return nullptr;", lookup)


class ArmorRarityGeneratorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.generator = GENERATOR.read_text(encoding="utf-8")

    def test_generator_derives_slot_and_rank_from_the_editor_id(self) -> None:
        # Armor ranks are written with a leading zero (Armor_03_); weapon ranks
        # are not (Wep_3_). Both shapes must resolve to the same 1..3 scale.
        self.assertIn(r"^mod_Legendary_(Armor|Helmet|Pack)_0?([1-3])_", self.generator)
        self.assertIn("ARMOR_LEGEND_SLOT = {'armor': 0, 'helmet': 1, 'pack': 2}", self.generator)
        self.assertIn("armor_effects.append((fid, ARMOR_LEGEND_SLOT[ma.group(1).lower()], int(ma.group(2))))", self.generator)
        # The existing weapon pool must not be disturbed by the armor branch.
        self.assertIn(r"^mod_Legendary_Wep_([123])_", self.generator)


class ArmorRarityWiringTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = SOURCE.read_text(encoding="utf-8-sig")

    def function_body(self, name: str) -> str:
        match = re.search(rf"\b{name}\([^)]*\)\s*\{{(?P<body>.*?)\n\t\t\}}", self.source, re.S)
        self.assertIsNotNone(match, f"{name} implementation not found")
        return match.group("body")

    def test_slot_class_follows_the_armor_subcategory(self) -> None:
        slot = self.function_body("ArmorLegendarySlot")
        self.assertIn("RE::FormType::kARMO", slot)
        self.assertIn("ArmorSub::kHelmet", slot)
        self.assertIn("ArmorSub::kPack", slot)
        self.assertIn("ClassifyArmorSub(CategorySearchText(a_form))", slot)

    def test_pool_reads_the_generated_table_per_slot(self) -> None:
        pool = self.function_body("ArmorLegendaryPool")
        self.assertIn("SFE::kOmodArmorLegendaryTable", pool)
        self.assertIn("entry.slot != a_slot", pool)
        self.assertIn("std::call_once", pool)

    def test_roll_matches_the_weapon_rank_semantics(self) -> None:
        roll = self.function_body("SetArmorRarityChoice")
        # Rare/Epic/Legendary reuse the weapon sentinels: N effects, ranks <= N.
        self.assertIn("kRaritySentinelBase + 1u", roll)
        self.assertIn("kRaritySentinelBase + 3u", roll)
        self.assertIn("effect.rank >= 1u && effect.rank <= nRoll", roll)
        self.assertIn("std::shuffle", roll)
        self.assertIn("g_armorRarity.insert_or_assign", roll)
        # Backpacks have no rank-1 effect: a Rare pick must still attach one
        # effect instead of promising one and rolling nothing.
        self.assertIn("Backpacks publish no rank-1 effect", roll)
        self.assertIn("if (pool.empty()) {", roll)
        # A re-roll must not invalidate the weapon assembly cache.
        self.assertNotIn("g_choiceVersion", roll)

    def test_rarity_combo_is_shown_for_armor_forms(self) -> None:
        self.assertIn('} else if (form && form->GetFormType() == RE::FormType::kARMO) {', self.source)
        self.assertIn('ImGui::PushID("armor-rarity")', self.source)
        self.assertIn("SetArmorRarityChoice(form->GetFormID(), rarity.fid, slot)", self.source)
        self.assertIn('"Common (no effects)", 0 }, { "Rare (1 effect)", kRaritySentinelRare', self.source)
        self.assertIn('ImGui::TextWrapped("Rolled: %s", names.c_str())', self.source)

    def test_basket_lines_capture_the_armor_roll(self) -> None:
        selector = self.function_body("SelectedOmodsFor")
        self.assertIn("g_assemblyPicks", selector)
        self.assertIn("g_armorRarity", selector)
        self.assertIn("it->second.roll", selector)
        # Every ADD route (row confirm, details button, OSF page basket) shares
        # the selector, so none can store a line without the roll.
        self.assertEqual(self.source.count("auto selectedOmods = SelectedOmodsFor(form);"), 3)
        # Only the selector may read the raw pick map now: both basket routes
        # go through it, so there is exactly one reader left.
        self.assertEqual(self.source.count("g_assemblyPicks.find(formID)"), 1)

    def test_modded_delivery_defaults_on_and_keeps_a_plain_fallback(self) -> None:
        start = self.source.index("void ShopBrowser::ConfirmBasket()")
        end = self.source.index("if (freeTransfer) {", start)
        transfer = self.source[start:end]
        self.assertIn("const bool hasSelectedMods = !item.selectedOmods.empty();", transfer)
        # 0.8.338: the roll is delivered by the engine's activation pickup,
        # so the world route (the only one that can apply OMODs) is on by
        # default. attachMods=0 still skips it for troubleshooting.
        self.assertIn('LoadShopIniBool(L"attachMods", true)', transfer)
        self.assertIn("useWorldRoute = hasSelectedMods && attachMods", transfer)
        self.assertIn("SpikeDeliverWithMods(player, item.bound, item.count, picks)", transfer)
        self.assertNotIn("hasSelectedWeaponMods", transfer)
        # The plain, confirmed inventory add is still the route whenever the
        # world route is off or refused, so a rolled item is never dropped.
        self.assertIn("AddObjectToContainer(item.bound", transfer)
        self.assertIn('" (inventory delivered; selected mods not applied)"', transfer)


if __name__ == "__main__":
    unittest.main()
