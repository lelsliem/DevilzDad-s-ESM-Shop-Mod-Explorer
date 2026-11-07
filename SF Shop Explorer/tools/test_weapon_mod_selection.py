"""Regression checks for weapon OMOD catalog choices and quality/effect separation."""
from __future__ import annotations

import unittest
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1] / "src" / "ShopExplorer.cpp"


class WeaponModSelectionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = SOURCE.read_text(encoding="utf-8-sig")
        cls.live_model = (SOURCE.parent / "LiveModel.cpp").read_text(encoding="utf-8-sig")

    def test_catalog_includes_weapon_and_melee_mod_categories(self) -> None:
        slots = self.source.split('static constexpr const char* kSlots[] = {', 1)[1].split('};', 1)[0]
        for slot in ("receiver", "blade", "barrel", "muzzle", "mag", "battery", "stock", "grip", "handle", "coating", "scope", "laser", "cover", "internal"):
            self.assertIn(f'"{slot}"', slots)

    def test_live_model_path_normalization_handles_form_separators_and_data_root(self) -> None:
        normalize = self.live_model.split("static std::string NormalizeNifKey(", 1)[1].split("bool BuildModel(", 1)[0]
        self.assertIn("std::replace(key.begin(), key.end(),", normalize)
        self.assertIn("key.end(),", normalize)
        self.assertIn("'\\\\', '/'", normalize)
        self.assertIn("key.front() == '/'", normalize)
        self.assertIn('key.compare(0, 5, "data/") == 0', normalize)
        self.assertIn('key.compare(0, 14, "meshes/meshes/") == 0', normalize)
        self.assertIn('key.compare(0, 11, "geometries/") == 0', normalize)
        self.assertIn('part == ".." || part.find(\':\') != std::string::npos', normalize)
        self.assertLess(normalize.index('key.compare(0, 5, "data/")'), normalize.index('key.compare(0, 7, "meshes/") != 0'))
        self.assertIn('key.compare(key.size() - 4, 4, ".nif") != 0', normalize)

    def test_live_model_path_normalization_rejects_traversal_components(self) -> None:
        normalize = self.live_model.split("static std::string NormalizeNifKey(", 1)[1].split("bool BuildModel(", 1)[0]
        self.assertIn('part == ".."', normalize)
        self.assertIn("part.find(':')", normalize)
        self.assertIn('key.compare(0, 11, "geometries/") == 0', normalize)
        self.assertIn("pos = end + 1;", normalize)
        self.assertIn("if (end == std::string::npos)", normalize)
        self.assertIn('key.compare(0, 7, "meshes/") != 0', normalize)

    def test_live_model_path_normalization_collapses_data_and_mesh_roots(self) -> None:
        normalize = self.live_model.split("static std::string NormalizeNifKey(", 1)[1].split("bool BuildModel(", 1)[0]
        self.assertIn('while (key.compare(0, 5, "data/") == 0)', normalize)
        self.assertIn('while (key.compare(0, 14, "meshes/meshes/") == 0)', normalize)
        self.assertIn('key = "meshes/" + key;', normalize)
        self.assertLess(normalize.index('key.compare(0, 5, "data/")'), normalize.index('key.compare(0, 7, "meshes/") != 0'))
        self.assertIn('key.compare(key.size() - 4, 4, ".nif") != 0', normalize)

    def test_melee_dummy_assembly_uses_connecting_housing_and_primary_part_anchor(self) -> None:
        paths = self.source.split("std::vector<std::string> FormModelPaths(", 1)[1].split("std::string FormModelPath(", 1)[0]
        self.assertIn('CaseFold(mp).find("meleedummyhousing")', paths)
        self.assertIn('CaseFold(mp).find("dummy") == std::string::npos', paths)
        self.assertIn("out.empty() && CaseFold(mp).find(\"meleedummyhousing\")", paths)
        self.assertIn("out.insert(out.begin(), mp);", paths)
        self.assertIn('if (!out.empty() && CaseFold(mp).find("meleedummyhousing") != std::string::npos)', paths)

        anchor = self.source.split("std::string anchor;", 1)[1].split("std::size_t famKw", 1)[0]
        self.assertIn("if (votes.empty() && meleeDummy)", anchor)
        self.assertIn('p->slot == "blade" || p->slot == "handle"', anchor)

        slot_classifier = self.source.split("std::string WeaponPartSlot(", 1)[1].split("struct MnamScanResult", 1)[0]
        for slot in ("blade", "handle", "coating"):
            self.assertIn(f'return "{slot}";', slot_classifier)

    def test_dummy_melee_core_parts_cannot_be_disabled_as_optional(self) -> None:
        self.assertIn("bool IsMandatoryDummyAssemblySlot(const std::string& a_slot, const bool a_meleeDummy)", self.source)
        self.assertIn('a_meleeDummy && (a_slot == "blade" || a_slot == "handle")', self.source)
        self.assertIn('std::string_view(slot) == "receiver" ||', self.source)
        self.assertIn('if (dummyBase && IsMandatoryDummyAssemblySlot(slot, meleeDummy) && fid == 0)', self.source)
        self.assertIn('!(dummyBase && IsMandatoryDummyAssemblySlot(slot, meleeDummy))', self.source)

    def test_part_only_dummy_weapons_autofill_stock_parts(self) -> None:
        # Breech Block's own NIF is a bare receiver; its barrel/mag/grip live
        # only as OMODs, so a dummy-base weapon with no catalogue default must
        # still render assembled instead of arriving bare.
        self.assertIn("std::size_t stdCount = 0;", self.source)
        self.assertIn("if (p->standard) {\n\t\t\t\t\t++stdCount;", self.source)
        # Per-slot fallback, not a family-wide one: a family with a stock part
        # in one slot must still auto-fill a slot that has none.
        self.assertIn("const Part* firstUsable = nullptr;", self.source)
        self.assertIn("if (usableDefault && !firstUsable)", self.source)
        self.assertIn("if (!pick && dummyBase && firstUsable) {", self.source)
        self.assertIn("pick = firstUsable;\n\t\t\t\t\t\t++autoFilled;", self.source)
        self.assertIn("declares no usable standard OMOD for {} slot(s)", self.source)
        # A family that does declare standard OMODs keeps the old rule.
        self.assertIn("if (p.standard && usableDefault && (!pick || !pick->standard))", self.source)

    def test_real_model_weapons_can_browse_their_mod_catalog(self) -> None:
        self.assertIn("const bool haveParts = haveCat && !cat.slotOrder.empty();", self.source)
        self.assertIn("SetWeaponSlotChoice(form->GetFormID(), slot, opt.fid)", self.source)

    def test_quality_and_rarity_are_separate_choices(self) -> None:
        self.assertIn("std::uint32_t qualityFid{ 0 };", self.source)
        self.assertIn("std::uint32_t rarityFid{ 0 };", self.source)
        self.assertIn("SetWeaponQualityChoice(form->GetFormID(), quality.fid)", self.source)
        self.assertIn("SetWeaponRarityChoice(form->GetFormID(), rarity.fid)", self.source)
        self.assertIn('AssemblyPick{ "quality", quality.fid }', self.source)
        self.assertIn('AssemblyPick{ "legendary", fid }', self.source)

    def test_effect_pool_scan_is_not_owner_limited(self) -> None:
        # 0.9.0 added the cross-owner MNAM rescue (crossBound) to the same gate:
        # a part from another plugin is still skipped unless the weapon itself
        # declares the family keyword the OMOD's MNAM names.
        self.assertIn("if (!sameOwner && !catalogQuality && !catalogEffect && !crossBound)", self.source)
        self.assertIn("const bool catalogEffect = LookupOmodLegendary(formID) != nullptr;", self.source)

    def test_configured_weapon_purchase_runs_omod_delivery(self) -> None:
        start = self.source.index("void ShopBrowser::ConfirmBasket()")
        end = self.source.index("void ShopBrowser::RefreshKeywords()", start)
        purchase = self.source[start:end]
        self.assertIn("SpikeDeliverWithMods(player, item.bound, item.count, picks)", purchase)
        self.assertIn("for (const auto fid : item.selectedOmods)", purchase)
        self.assertIn("SpikeOutcome::kOnFloorMods", purchase)
        self.assertIn("selected mods attached; pick it up", purchase)
        self.assertNotIn("route=direct-inventory", purchase)
        self.assertIn("SpikeOutcome::kTransferPendingMods", purchase)
        self.assertIn("SpikeOutcome::kOnFloorMods", purchase)

    def test_rarity_and_quality_omods_are_captured_for_basket_purchase(self) -> None:
        start = self.source.index("void ShopBrowser::ConfirmAdd()")
        end = self.source.index("void ShopBrowser::ConfirmBasket()", start)
        capture = self.source[start:end]
        # The capture itself is shared with armor rarity now, so the pick-map
        # read lives in SelectedOmodsFor; the basket line still stores whatever
        # that selector returns.
        self.assertIn("auto selectedOmods = SelectedOmodsFor(form);", capture)
        self.assertIn('BasketEntry{ formID, edid ? edid : "", count, std::move(selectedOmods) }', capture)
        selector_start = self.source.index("std::vector<std::uint32_t> SelectedOmodsFor(RE::TESForm* a_form)")
        selector = self.source[selector_start:selector_start + 1200]
        self.assertIn("g_assemblyPicks.find(formID)", selector)
        self.assertIn("selected.push_back(pick.fid)", selector)
        self.assertIn('AssemblyPick{ "quality", quality.fid }', self.source)
        self.assertIn('AssemblyPick{ "legendary", fid }', self.source)
        self.assertIn("rarity roll {:08X} -> {} effects", self.source)


if __name__ == "__main__":
    unittest.main()
