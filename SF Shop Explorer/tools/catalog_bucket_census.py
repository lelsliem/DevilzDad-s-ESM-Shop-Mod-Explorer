"""Census of the Misc and Aid subcategory buckets over a live catalog dump.

This mirrors `ClassifyMiscSub` and `ClassifyAidSub` in `src/ShopExplorer.cpp`,
including the model-path half those two classifiers read. Before widening a
term list this tool measures what each bucket actually holds, so a fix is a
concrete miss instead of a guess.

Usage:
    python tools/catalog_bucket_census.py
    python tools/catalog_bucket_census.py --catalog "<log dir>/SFDevilzDump-catalog.csv"
    python tools/catalog_bucket_census.py --type misc --top 40
"""
from __future__ import annotations

import argparse
import csv
from collections import Counter
from pathlib import Path

DEFAULT_CATALOG = Path.home() / "Documents" / "My Games" / "Starfield" / "SFSE" / "Logs" / "SFDevilzDump-catalog.csv"

# Model-path tokens, mirrored from the `a_model` half of ClassifyMiscSub /
# ClassifyAidSub. Names are marketing copy ("QuickSnack", "Tranquilitea"), so
# the NIF a record ships is the only reliable signal for a whole product line.
MISC_MODEL_TERMS = {
    "Toys": ["collectables", "snowglobe", "figurine", "\\toys\\", "desktoy", "cutetoy"],
    "Resources": ["landscape\\resources", "lootcontainers\\"],
    # Measured from the 1417 records that stayed in Other after the first model
    # pass: each bucket is one NIF family, so the tally moves only when a shelf
    # does. Mirrors the second half of ClassifyMiscSub.
    "Décor": ["setdressing\\office", "setdressing\\kitchen", "pegasus_homegarden",
              "privatelivingquarters", "posters", "interactivehavok", "desktop_items",
              "bar_glassware", "baritems", "setdressing\\neon", "setdressing\\decoration", "dining_set",
              "setdressing\\lodge", "trackeralliancehq",
              # SetDressing\SFBGS003 holds the mission board terminals and, in a
              # sibling folder, the ammo primer boxes: the sub-folder is the
              # token, or the boards and Charged Diodes land in one bucket.
              "mission_board_terminals"],
    "Lab": ["scienceglass"],
    # The xenology folder also ships the Old Earth PC props; they are tech, and
    # this token is read before the folder rule in ClassifyMiscSub.
    "Creature": ["xenologymesh"],
    "Tech": ["manufactured_goods", "tech_clutter", "tools_kit", "prototype_tech", "powercell",
             "xtech_misc", "ammoprimerbox"],
    "Slates": ["items\\dataslate"],
    "Food": ["fooddrink_", "coffeetogo"],
    "Artifacts": ["setdressing\\artifacts", "setdressing\\earth", "armisticedataobject"],
    "Animal": ["animalgenericingredients"],
}

# Model checks that sit at a fixed point in ClassifyMiscSub's chain rather than
# inside their own bucket block: the Old Earth PC props share the xenology
# folder with the creature parts, and the PC file name is read just before the
# folder rule. (bucket, tokens, the bucket it is evaluated in front of).
MISC_MODEL_GUARDS = [("Tech", ["aaa_base_pc"], "Creature")]

AID_MODEL_TERMS = {
    "Meds": ["affliction_cures", "bacta", "trauma_pack", "panacaea", "medstim"],
    "Food": ["fooddrink", "xenoplantpod", "\\xenologymesh\\food\\", "coffeetogo", "tranquilitea",
             "foodanddrink", "moonshine", "cocktailstation", "foodpouch", "foodpackets", "itm_milk"],
    "Chems": ["\\chem_", "inhailer", "itm_potion"],
}

MISC_TERMS = [
    ("Flora", ["fertile", "flora", "plant:", "plant ", "seed", "tissue", "tree", "aquatic", "root",
               "spore", "fungus", "fungal", "leaf", "flower"]),
    ("Toys", ["plushie", "toy", "toys", "action figure", "actionfigure", "figurine", "doll"]),
    ("Cards", ["playing card", "playing cards", "card deck", "card pack", "collectible card",
               "collectible cards", "card collectible", "cards", "sabacc"]),
    ("Containers", ["crate", "container", "containers", "empty pack", "empty canister", "storage box",
                    "cargo case", "case"]),
    ("Resources", ["resource", "resources", "resinorg", "resorg", "aluminum", "aluminium", "antimony",
                   "beryllium", "caesium", "cesium", "chlorine", "chlorosilanes", "cobalt", "copper",
                   "dysprosium", "europium", "fluorine", "gold", "helium-3", "iron", "iridium", "lithium",
                   "neodymium", "neon", "nickel", "palladium", "platinum", "plutonium", "silver",
                   "tantalum", "tetrafluorides", "titanium", "tungsten", "uranium", "vanadium",
                   "ytterbium", "caelumite", "indicite", "rothicite", "vytinium", "aldumite", "tasine",
                   "sealant", "adhesive", "polymer", "solvent", "lubricant", "spidroin", "biosuppressant",
                   "microsecond regulator", "zero wire", "structural material", "ore", "mineral",
                   "inorganic", "organic material", "amino acids", "antimicrobial", "cosmetic", "fiber",
                   "membrane", "nutrient", "pigment", "sedative", "toxin", "metabolic agent",
                   "ornamental material", "sterile nanotubes", "analgesic", "hypercatalyst",
                   "molecular sieve"]),
    # Model-only buckets: the shelf names them, so no name terms are listed.
    ("Décor", []),
    ("Lab", []),
    ("Creature", []),
    ("Tech", []),
    ("Slates", []),
    ("Food", []),
    ("Artifacts", []),
    ("Animal", []),
]

AID_TERMS = [
    ("Meds", ["med pack", "medpack", "bandage", "bandages", "injector", "antibiotic", "antibiotics",
              "heal gel", "healing", "immobilizer", "immobilizers", "cure", "trauma", "emergency kit",
              "sterile", "medical treatment", "panacea", "penicillin", "bacta", "kolto", "poultice",
              "salve", "medkit"]),
    ("Food", ["food", "meat", "fresh", "milk", "drink", "meal", "steak", "sandwich", "cream", "bread",
              "fruit", "vegetable", "water", "soup", "juice", "coffee", "tea", "chunks", "chocolate",
              "egg", "cheese", "fish", "chowder", "curry", "wrap", "cake", "cookie", "burger", "pizza",
              "pasta", "rice", "yam", "snack", "soda", "wine", "beer", "liquor", "beverage", "ale",
              "brew", "whiskey", "whisky", "cocktail", "heart", "pie", "toast", "baguette", "cereal",
              "seaweed", "ichor", "vinegar", "alcohol"]),
    ("Chems", ["aurora", "stimulant", "chem", "poison", "hallucinogen", "combat enhancer", "red trench",
               "battlestim", "frostwolf", "whiteout", "panopticon", "amp", "blend", "synapse", "otme",
               "synephrine", "potion", "potions"]),
]


def is_word(char: str) -> bool:
    return "a" <= char <= "z" or "0" <= char <= "9"


def has_term(text: str, term: str) -> bool:
    """Mirror of HasTextTerm: a word-boundary match, not a raw substring."""
    if not term:
        return False
    at = text.find(term)
    while at >= 0:
        end = at + len(term)
        left = at == 0 or not is_word(text[at - 1]) or not is_word(term[0])
        right = end >= len(text) or not is_word(text[end]) or not is_word(term[-1])
        if left and right:
            return True
        at = text.find(term, at + 1)
    return False


def classify(text: str, table: list[tuple[str, list[str]]], model: str = "") -> str:
    """Mirror of the Classify*Sub chain: name terms first, then model tokens."""
    model_terms = MISC_MODEL_TERMS if table is MISC_TERMS else AID_MODEL_TERMS
    guards = MISC_MODEL_GUARDS if table is MISC_TERMS else ()
    for bucket, terms in table:
        for guard_bucket, tokens, before in guards:
            if bucket == before and model and any(token in model for token in tokens):
                return guard_bucket
        if any(has_term(text, term) for term in terms):
            return bucket
        tokens = model_terms.get(bucket, ())
        if model and any(token in model for token in tokens):
            return bucket
    return "Other"


def search_text(row: dict[str, str]) -> str:
    """Mirror of CategorySearchText: display name + EDID + keywords, folded."""
    return f'{row["name"]} {row["editorId"]} {row["keywords"]}'.lower()


def model_text(row: dict[str, str]) -> str:
    """Mirror of the folded FormModelPath the Misc/Aid classifiers receive."""
    return row["modelPath"].lower().replace("/", "\\")


def load_rows(path: Path, form_type: str) -> list[dict[str, str]]:
    with path.open(encoding="utf-8-sig", newline="") as handle:
        return [row for row in csv.DictReader(handle) if row["type"] == form_type]


def shop_visible(rows: list[dict[str, str]]) -> list[dict[str, str]]:
    """Named records with a model: the rows a buyer can actually find."""
    return [row for row in rows
            if row["name"].strip().lower() not in ("", "(unnamed)", "unnamed") and row["modelPath"].strip()]


def report(label: str, rows: list[dict[str, str]], table, top: int) -> None:
    buckets = Counter(classify(search_text(row), table, model_text(row)) for row in rows)
    total = sum(buckets.values()) or 1
    print(f"\n== {label}: {total} records ==")
    order = [name for name, _ in table] + ["Other"]
    for name in order:
        count = buckets.get(name, 0)
        print(f"  {name:<12} {count:>5}  {100.0 * count / total:5.1f}%")
    others = [row for row in rows if classify(search_text(row), table, model_text(row)) == "Other"]
    if others:
        print(f"\n  -- Other, top {min(top, len(others))} by plugin --")
        for row in sorted(others, key=lambda r: (r["plugin"], r["name"]))[:top]:
            name = row["name"].strip() or "(unnamed)"
            print(f"  {row['plugin'][:28]:<28} {row['editorId'][:32]:<32} {name[:38]:<38} "
                  f"{row['modelPath'].split(chr(92))[-1][:28]}")

    # Which terms carry the most weight inside each bucket: the first match wins,
    # so a bucket fed by an accidental substring shows up here.
    print("\n  -- first-matching term, top 8 per bucket --")
    model_terms = MISC_MODEL_TERMS if table is MISC_TERMS else AID_MODEL_TERMS
    for name, terms in table:
        hits: Counter[str] = Counter()
        for row in rows:
            text = search_text(row)
            model = model_text(row)
            if not any(has_term(text, term) for term in terms) and \
                    not any(token in model for token in model_terms.get(name, ())):
                continue
            for term in terms:
                if has_term(text, term):
                    hits[term] += 1
                    break
            else:
                hits["<model>"] += 1
        if hits:
            top_hits = ", ".join(f"{term}={count}" for term, count in hits.most_common(8))
            print(f"  {name:<12} {top_hits}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalog", type=Path, default=DEFAULT_CATALOG)
    parser.add_argument("--type", choices=["misc", "aid", "both"], default="both")
    parser.add_argument("--top", type=int, default=25)
    parser.add_argument("--all", action="store_true",
                        help="count every row, including unnamed records and records with no model")
    args = parser.parse_args()

    if args.type in ("misc", "both"):
        rows = load_rows(args.catalog, "MISC")
        if not args.all:
            rows = shop_visible(rows)
        report("Misc", rows, MISC_TERMS, args.top)
    if args.type in ("aid", "both"):
        rows = load_rows(args.catalog, "ALCH")
        if not args.all:
            rows = shop_visible(rows)
        report("Aid", rows, AID_TERMS, args.top)


if __name__ == "__main__":
    main()
