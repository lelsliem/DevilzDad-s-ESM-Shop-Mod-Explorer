"""Regression checks for the Misc and Aid subcategory filters.

The two classifiers used to see names only, and a live census of the 2026-10-24
catalog (2579 Misc, 1189 Aid records) showed two thirds of Misc and over half of
Aid landing in Other: the Sabacc deck, every Star Wars ore, the Holocron and
snow-globe collectibles, and the whole FoodDrink / Tranquilitea / Bacta / chem
inhailer model families carried no category word in their names. The fix reads
the model path as well, so this test pins both halves:

  * the Python mirror over a captured catalog fixture (bucket counts and the
    concrete rows that were mis-filed), and
  * the C++ source, so deleting the model half or the model argument at a call
    site fails here instead of in game.

    python -m unittest test_misc_aid_filters -v
"""
from __future__ import annotations

import csv
import re
import sys
import unittest
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import catalog_bucket_census as census  # noqa: E402

SOURCE = ROOT / "src" / "ShopExplorer.cpp"
FIXTURE = ROOT / "tools" / "fixtures" / "misc_aid_catalog.csv"


def load_fixture() -> list[dict[str, str]]:
    with FIXTURE.open(encoding="utf-8", newline="") as handle:
        rows = [row for row in csv.reader(handle) if row and not row[0].startswith("#")]
    header, data = rows[0], rows[1:]
    return [dict(zip(header, row)) for row in data]


class MiscAidBucketTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.rows = load_fixture()
        cls.misc = [row for row in cls.rows if row["type"] == "MISC"]
        cls.aid = [row for row in cls.rows if row["type"] == "ALCH"]
        cls.source = SOURCE.read_text(encoding="utf-8-sig")

    def bucket(self, row: dict[str, str]) -> str:
        table = census.MISC_TERMS if row["type"] == "MISC" else census.AID_TERMS
        return census.classify(census.search_text(row), table, census.model_text(row))

    def test_misc_bucket_distribution(self) -> None:
        # The 2026-10-24 census after both model passes. Before the first one,
        # Cards was empty, Toys held 88 and Other held 1574; after it Other
        # still held 1417 because whole NIF shelves name no category at all.
        counts = Counter(self.bucket(row) for row in self.misc)
        self.assertEqual(counts["Flora"], 654)
        self.assertEqual(counts["Toys"], 120)
        self.assertEqual(counts["Cards"], 108)
        self.assertEqual(counts["Containers"], 94)
        self.assertEqual(counts["Resources"], 200)
        self.assertEqual(counts["Décor"], 321)
        self.assertEqual(counts["Lab"], 223)
        self.assertEqual(counts["Creature"], 125)
        self.assertEqual(counts["Tech"], 137)
        self.assertEqual(counts["Slates"], 48)
        self.assertEqual(counts["Food"], 57)
        self.assertEqual(counts["Artifacts"], 48)
        self.assertEqual(counts["Animal"], 28)
        self.assertEqual(counts["Other"], 416)

    def test_aid_bucket_distribution(self) -> None:
        # Before the model half: Meds 25, Food 460, Chems 12, Other 692.
        counts = Counter(self.bucket(row) for row in self.aid)
        self.assertEqual(counts["Meds"], 51)
        self.assertEqual(counts["Food"], 1039)
        self.assertEqual(counts["Chems"], 53)
        self.assertEqual(counts["Other"], 46)

    def test_sabacc_deck_files_as_cards(self) -> None:
        sabacc = [row for row in self.misc if row["name"].lower().startswith("sabacc card")]
        self.assertGreaterEqual(len(sabacc), 100)
        self.assertEqual({self.bucket(row) for row in sabacc}, {"Cards"})

    def test_ore_chunks_file_as_resources_without_an_ore_word(self) -> None:
        # "Lead" names the metal, not the category; the ore NIF settles it.
        lead = next(row for row in self.misc if row["name"] == "Lead")
        self.assertIn("resourceleadchunk", lead["modelPath"].lower())
        self.assertEqual(self.bucket(lead), "Resources")

    def test_collectibles_follow_their_nif(self) -> None:
        holocron = next(row for row in self.misc if row["name"] == "Holocron")
        self.assertEqual(self.bucket(holocron), "Toys")
        wilby = next(row for row in self.misc if row["name"] == "My Friend Wilby")
        self.assertEqual(self.bucket(wilby), "Toys")

    def test_glued_food_names_file_as_food(self) -> None:
        # "QuickSnack" is one word, so the word-boundary matcher cannot see
        # "snack"; the FoodDrink model can.
        quick = [row for row in self.aid if row["name"].startswith("QuickSnack")]
        self.assertGreaterEqual(len(quick), 20)
        self.assertEqual({self.bucket(row) for row in quick}, {"Food"})
        tea = next(row for row in self.aid if row["name"] == "Tranquilitea Classic")
        self.assertEqual(self.bucket(tea), "Food")

    def test_meds_beat_the_chem_inhailer_they_share_a_model_with(self) -> None:
        # Bacta goes through inhailer and trauma-pack NIFs, so Meds must be
        # decided before Chems reads the model.
        spray = next(row for row in self.aid if row["name"] == "Bacta Spray")
        self.assertEqual(self.bucket(spray), "Meds")
        patch = next(row for row in self.aid if row["name"] == "Imperial Bacta Patch")
        self.assertEqual(self.bucket(patch), "Meds")

    def test_plural_cures_and_chem_inhailers(self) -> None:
        bandages = next(row for row in self.aid if row["name"] == "Bandages")
        self.assertEqual(self.bucket(bandages), "Meds")
        addijack = next(row for row in self.aid if row["name"] == "AddiJack")
        self.assertEqual(self.bucket(addijack), "Chems")

    def test_hearts_stay_food_while_the_inhailer_reads_as_chem(self) -> None:
        # Giant Heart ships the chem inhaler model but is a cooking ingredient;
        # the name has to win before the model does.
        heart = next(row for row in self.aid if row["name"] == "Giant Heart")
        self.assertIn("inhailer", heart["modelPath"].lower())
        self.assertEqual(self.bucket(heart), "Food")

    def test_other_keeps_unnamed_category_items(self) -> None:
        # Clutter with no category word and no product-line NIF stays reachable
        # under Other rather than being guessed into a bucket. Data Spike is the
        # canonical case: a single prop with a one-off NIF.
        spike = next(row for row in self.misc if row["name"] == "Data Spike")
        self.assertEqual(self.bucket(spike), "Other")

    def test_lab_glass_comes_from_one_nif_folder(self) -> None:
        # SetDressing\ScienceGlass: vials, beakers, tongs and sample vials.
        lab = [row for row in self.misc if self.bucket(row) == "Lab"]
        self.assertEqual(len(lab), 223)
        self.assertEqual({row["modelPath"].lower().replace("/", "\\").find("scienceglass") >= 0
                          for row in lab}, {True})
        for name in ("Beaker", "Beaker Tongs", "Anomalous Sample"):
            row = next(row for row in self.misc if row["name"] == name)
            self.assertEqual(self.bucket(row), "Lab", name)

    def test_dataslates_are_named_by_their_model(self) -> None:
        slates = [row for row in self.misc if self.bucket(row) == "Slates"]
        self.assertEqual(len(slates), 48)
        self.assertEqual({row["modelPath"].lower().replace("/", "\\").find("items\\dataslate") >= 0
                          for row in slates}, {True})
        # Nearly every one is named for its contents, not its form: the model
        # is what puts "Overseer Program" in the slates bucket.
        self.assertLess(sum("slate" in row["name"].lower() for row in slates), 10)

    def test_creature_parts_follow_the_xenology_shelf(self) -> None:
        for name in ("Aceles Beak", "Aceles Horn", "Alien Centiskull Display"):
            row = next(row for row in self.misc if row["name"] == name)
            self.assertIn("xenologymesh", row["modelPath"].lower())
            self.assertEqual(self.bucket(row), "Creature", name)

    def test_power_cells_and_tool_kits_file_as_tech(self) -> None:
        cell = next(row for row in self.misc if row["name"] == "Emergency Power Cell")
        self.assertEqual(self.bucket(cell), "Tech")
        tool = next(row for row in self.misc if row["name"] == "Advanced Laser Cutter")
        self.assertEqual(self.bucket(tool), "Tech")

    def test_bounty_boards_are_set_dressing_not_ammo_primers(self) -> None:
        # SetDressing\SFBGS003 holds both the mission board terminals and the
        # ammo primer boxes. A bare `sfbgs003` token would file "Bounty: The
        # Vulture" beside "Charged Diodes"; the sub-folder keeps them apart.
        boards = [row for row in self.misc
                  if "mission_board_terminals" in row["modelPath"].lower()]
        self.assertEqual(len(boards), 22)
        self.assertEqual({self.bucket(row) for row in boards}, {"Décor"})
        primers = [row for row in self.misc if "ammoprimerbox" in row["modelPath"].lower()]
        self.assertEqual(len(primers), 4)
        self.assertEqual({self.bucket(row) for row in primers}, {"Tech"})

    def test_old_earth_pc_props_leave_the_creature_bucket(self) -> None:
        # The xenology folder ships the Old Earth PC set beside the tusks and
        # mandibles; the PC file name is read before the folder rule.
        tower = next(row for row in self.misc if row["name"] == "Old Earth PC Tower")
        self.assertIn("xenologymesh", tower["modelPath"].lower())
        self.assertEqual(self.bucket(tower), "Tech")
        pcs = [row for row in self.misc if "aaa_base_pc" in row["modelPath"].lower()
               and self.bucket(row) == "Tech"]
        self.assertEqual(len(pcs), 7)

    def test_inorganic_loot_containers_file_as_resources(self) -> None:
        # SetDressing\Outpost\lootcontainers: the Inorg* resource containers
        # name a gas or a liquid, not a resource word.
        for name in ("Vintrium", "Hypermatter", "Tibanna"):
            row = next(row for row in self.misc if row["name"] == name)
            self.assertIn("lootcontainers", row["modelPath"].lower())
            self.assertEqual(self.bucket(row), "Resources", name)

    def test_misc_food_packs_split_off_from_decor(self) -> None:
        tea = next(row for row in self.misc if row["name"].startswith("Bulk Tranquilitea"))
        self.assertEqual(self.bucket(tea), "Food")
        pack = next(row for row in self.misc if row["name"] == "Empty Drink Pack")
        self.assertEqual(self.bucket(pack), "Food")

    def test_earth_relics_and_greek_letter_artifacts_are_artifacts(self) -> None:
        # Two shelves, one bucket: SetDressing\Earth holds the recognisable
        # relics, SetDressing\Artifacts the numeric ones.
        head = next(row for row in self.misc if row["name"] == "Droid Head")
        self.assertEqual(self.bucket(head), "Artifacts")
        alpha = next(row for row in self.misc if row["name"] == "Artifact ALPHA")
        self.assertEqual(self.bucket(alpha), "Artifacts")

    def test_animal_glands_hide_and_bone_are_their_own_bucket(self) -> None:
        bone = next(row for row in self.misc if row["name"] == "Structural (Bone)")
        self.assertEqual(self.bucket(bone), "Animal")

    def test_desk_and_kitchen_clutter_files_as_decor(self) -> None:
        for name in ("Succulent", "Jam Jar", "Tea Pot"):
            row = next((row for row in self.misc if row["name"] == name), None)
            if row is None:
                continue
            self.assertEqual(self.bucket(row), "Décor", name)


class MiscAidSourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = SOURCE.read_text(encoding="utf-8-sig")

    def function_body(self, name: str) -> str:
        match = re.search(rf"\b{name}\(const std::string& a_text,[^)]*\)\s*\{{(?P<body>.*?)\n\t\t\}}",
                          self.source, re.S)
        self.assertIsNotNone(match, f"{name} implementation not found")
        return match.group("body")

    def test_classifiers_take_the_model_path(self) -> None:
        for name in ("ClassifyMiscSub", "ClassifyAidSub"):
            body = self.function_body(name)
            self.assertIn("a_model", body, name)

    def test_call_sites_pass_the_model_text(self) -> None:
        self.assertIn("ClassifyMiscSub(catText(), catModel())", self.source)
        self.assertIn("ClassifyAidSub(catText(), catModel())", self.source)
        # The two shared reads are still the folded name+keywords and the
        # folded model path; the call sites just fetch each one once.
        self.assertIn("categoryText = CategorySearchText(form);", self.source)
        self.assertIn("categoryModel = CategoryModelText(form);", self.source)
        search = re.search(r"\bCategoryModelText\(const RE::TESForm\* a_form\)\s*\{(?P<body>.*?)\n\t\t\}",
                           self.source, re.S)
        self.assertIsNotNone(search)
        self.assertIn("FormModelPath(a_form)", search.group("body"))

    def test_measured_model_tokens_are_present(self) -> None:
        misc = self.function_body("ClassifyMiscSub")
        for token in ("sabacc", "collectables", "snowglobe", "landscape\\\\resources",
                      "scienceglass", "xenologymesh", "manufactured_goods",
                      "items\\\\dataslate", "fooddrink_", "animalgenericingredients",
                      "setdressing\\\\earth", "setdressing\\\\kitchen",
                      "lootcontainers\\\\", "mission_board_terminals",
                      "ammoprimerbox", "aaa_base_pc"):
            self.assertIn(f'"{token}"', misc, token)
        aid = self.function_body("ClassifyAidSub")
        for token in ("affliction_cures", "fooddrink", "tranquilitea", "inhailer", "itm_potion"):
            self.assertIn(f'"{token}"', aid, token)

    def test_new_misc_buckets_are_named_and_ordered_before_other(self) -> None:
        header = (ROOT / "src" / "ShopExplorer.h").read_text(encoding="utf-8-sig")
        for case in ("kDecor", "kLab", "kCreature", "kTech", "kSlates", "kFood",
                     "kArtifacts", "kAnimal"):
            self.assertIn(f"case MiscSub::{case}: return", header, case)
        misc = self.function_body("ClassifyMiscSub")
        for case in ("kDecor", "kLab", "kCreature", "kTech", "kSlates", "kFood",
                     "kArtifacts", "kAnimal"):
            self.assertLess(misc.index(f"MiscSub::{case}"), misc.index("MiscSub::kOther"), case)
        # The shelf buckets are model-only: a name term list here would be a
        # second, unmeasured classifier sitting on top of the folder rule.
        shelf = misc[misc.index("// The eight buckets below"):misc.index("return MiscSub::kOther;")]
        self.assertNotIn("a_text", shelf)
        self.assertIn("a_model", shelf)

    def test_meds_are_checked_before_food_and_chems(self) -> None:
        aid = self.function_body("ClassifyAidSub")
        self.assertLess(aid.index("AidSub::kMeds"), aid.index("AidSub::kFood"))
        self.assertLess(aid.index("AidSub::kFood"), aid.index("AidSub::kChems"))
        # The Food name terms are what keep the hearts out of Chems, so they
        # have to sit in the Food block, before the Chems return.
        chem_start = aid.index("AidSub::kChems")
        food_terms_start = aid.index('"meat"')
        self.assertIn('"heart"', aid[food_terms_start:chem_start])


if __name__ == "__main__":
    unittest.main()
