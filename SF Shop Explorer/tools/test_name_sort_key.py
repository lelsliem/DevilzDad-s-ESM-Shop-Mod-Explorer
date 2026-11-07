"""Regression checks for the NAME column's sort key and the name padding it strips.

The rows that made the list look unsorted are real: dumping the test profile's
catalog (SFDevilzDump-catalog.csv) finds exactly five names whose first byte is
not the first character a player reads.

    id        type  name
    0400009D  ACTI  " Spidermelon Bloom Cactus (Sapling)"
    040000CF  ACTI  " Asparagus Fruit Pine (Sapling)"
    002EE98F  ALCH  "\uF225 Hydrated"
    00313281  ALCH  "\uF225 Fed"
    04000770  ALCH  " Golden Glow"

U+F225 is a private-use icon marker (food/drink) that no UI face can draw, and
the leading space is invisible. Both sort as bytes: U+F225 (EF 88 A5) is above
every letter, the space (20) below every letter, so NAME (desc) lifted the two
marked Aid records above the Z block and NAME (asc) put " Golden Glow" above the
A block - which is what the sorted list showed.

The C++ mirrors below are deliberately small: they are the same rule the shop
applies (ShopExplorer.cpp: IsNamePadding / TrimNamePadding / NameSortKey), run
over the captured names so the ordering can be checked without the game.
"""
from __future__ import annotations

import re
import unicodedata
import unittest
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1] / "src" / "ShopExplorer.cpp"

# The five catalog names with an invisible prefix, byte for byte.
PADDED_NAMES = [
    " Spidermelon Bloom Cactus (Sapling)",
    " Asparagus Fruit Pine (Sapling)",
    "\uf225 Hydrated",
    "\uf225 Fed",
    " Golden Glow",
]

# A sample of ordinary neighbours, so ordering is checked against a list rather
# than one string at a time.
CATALOG = PADDED_NAMES + [
    '"The Great Serpent" Lie',
    "(unnamed) [000127BE]",
    "Bo-Katan Armor",
    "Clone Trooper Armor",
    "Fed",
    "Golden Glow",
    "Hydrated",
    "Priestess' Gem",
    "Zealot Sith Suit",
    "Zipper Bandages",
]


def is_name_padding(code: int) -> bool:
    """Mirror of IsNamePadding."""
    if code <= 0x20 or code in (0xA0, 0x202F, 0x205F, 0x3000, 0xFEFF, 0x2028, 0x2029):
        return True
    if 0x2000 <= code <= 0x200F:
        return True
    return 0xE000 <= code <= 0xF8FF


def trim_name_padding(text: str) -> str:
    """Mirror of TrimNamePadding."""
    begin = 0
    while begin < len(text) and is_name_padding(ord(text[begin])):
        begin += 1
    end = len(text)
    while end > begin and is_name_padding(ord(text[end - 1])):
        end -= 1
    return text[begin:end]


def name_sort_key(text: str) -> str:
    """Mirror of NameSortKey: fold from the first character that starts a word."""
    begin = 0
    while begin < len(text):
        char = text[begin]
        starts_word = ord(char) >= 0x80 or char.isascii() and char.isalnum()
        if starts_word:
            break
        begin += 1
    return trim_name_padding(text[begin:] if begin < len(text) else text).lower()


class NamePaddingTest(unittest.TestCase):
    def test_every_padded_name_trims_to_what_is_read(self) -> None:
        expected = {
            " Spidermelon Bloom Cactus (Sapling)": "Spidermelon Bloom Cactus (Sapling)",
            " Asparagus Fruit Pine (Sapling)": "Asparagus Fruit Pine (Sapling)",
            "\uf225 Hydrated": "Hydrated",
            "\uf225 Fed": "Fed",
            " Golden Glow": "Golden Glow",
        }
        for raw, shown in expected.items():
            self.assertEqual(trim_name_padding(raw), shown, repr(raw))

    def test_padding_characters_that_shape_a_name_are_kept(self) -> None:
        # Interior spaces, apostrophes and brackets are part of the name.
        for name in ("Priestess' Gem", "(unnamed) [000127BE]", '"The Great Serpent" Lie'):
            self.assertEqual(trim_name_padding(name), name)

    def test_no_padded_name_sorts_outside_the_alphabet(self) -> None:
        keys = sorted(name_sort_key(name) for name in CATALOG)
        # Every key now starts with a letter or digit: nothing sorts above the A
        # block because of an invisible prefix.
        for key in keys:
            self.assertTrue(key[0].isalnum(), repr(key))
        # The two records the icon prefix used to lift above the Z block rank
        # with their letters, and " Golden Glow" with the G names.
        self.assertNotIn("\uf225 hydrated", keys)
        self.assertNotIn(" golden glow", keys)
        self.assertIn("hydrated", keys)
        self.assertEqual(keys.count("golden glow"), 2)

    def test_padded_names_rank_next_to_their_unpadded_twins(self) -> None:
        keys = sorted(name_sort_key(name) for name in CATALOG)
        marked = [k for k in keys if k in ("fed", "hydrated", "golden glow")]
        # Fed/Hydrated/Golden Glow each appear twice: once plain, once padded.
        self.assertEqual(marked, ["fed", "fed", "golden glow", "golden glow", "hydrated", "hydrated"])

    def test_icon_prefix_ranks_with_its_letter(self) -> None:
        marked = name_sort_key("\uf225 Hydrated")
        self.assertEqual(marked, "hydrated")
        neighbours = sorted([name_sort_key("Zipper Bandages"), marked, name_sort_key("Golden Glow")])
        self.assertEqual(neighbours, ["golden glow", "hydrated", "zipper bandages"])

    def test_punctuation_prefix_ranks_by_the_word_after_it(self) -> None:
        self.assertEqual(name_sort_key('"The Great Serpent" Lie'), 'the great serpent" lie')
        self.assertEqual(name_sort_key("(unnamed) [000127BE]"), "unnamed) [000127be]")

    def test_descending_order_starts_with_the_tail_of_the_alphabet(self) -> None:
        keys = sorted((name_sort_key(name) for name in CATALOG), reverse=True)
        self.assertEqual(keys[0], "zipper bandages")
        self.assertEqual(keys[1], "zealot sith suit")
        # The padded records are no longer at either end of the list.
        self.assertNotIn("fed", keys[:5])
        self.assertNotIn("hydrated", keys[:5])


class SortKeySourceTest(unittest.TestCase):
    """The rules above have to be the ones the shop actually calls."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.source = SOURCE.read_text(encoding="utf-8-sig")

    def function_body(self, name: str) -> str:
        match = re.search(rf"\b{name}\((?:[^)]*)\)\s*\{{(?P<body>.*?)\n\t\t\}}", self.source, re.S)
        self.assertIsNotNone(match, f"{name} implementation not found")
        return match.group("body")

    def test_display_name_drops_the_padding(self) -> None:
        body = self.function_body("FormDisplayName")
        self.assertEqual(body.count("TrimNamePadding("), 2, "full name and editor ID")
        self.assertNotIn("return name;", body)
        self.assertIn("return trimmed;", body)

    def test_display_name_folds_control_bytes_before_measuring(self) -> None:
        """A newline in a name would draw a blank row twice as tall as its peers.

        ImGui measures a Selectable's label over every line but draws only up
        to the first one, so a name carrying a control byte became an empty
        cell twice the height of its neighbours - and, sorted on raw bytes, sat
        at the top of the NAME column. The fix is to fold those bytes to spaces
        before the label, the sort key and the search key are built.
        """
        sanitize = self.function_body("SanitizeLabel")
        self.assertIn("byte < 0x20u", sanitize)
        self.assertIn("byte == 0x7Fu", sanitize)
        self.assertIn("ch = ' ';", sanitize)
        display = self.function_body("FormDisplayName")
        self.assertEqual(display.count("SanitizeLabel("), 2, "full name and editor ID")

    def test_name_column_sorts_on_the_readable_text(self) -> None:
        order = self.source[self.source.index("void ShopBrowser::ApplyViewOrder()"):]
        order = order[:order.index("\n\tvoid ", 10)]
        self.assertIn("nameSortKeys[i] = forms[i] ? NameSortKey(FormDisplayName(forms[i])) : std::string{};", order)
        self.assertNotIn("CaseFold(FormDisplayName(", order)

    def test_name_keys_are_built_once_per_list_not_once_per_sort(self) -> None:
        """Toggling NAME folded 9,161 names per click: 4.4-5.0 ms in the 21:41
        log, against 0.5-1.1 ms for the numeric columns.

        ApplyViewOrder bumps listStamp on every header click, so a cache keyed
        on that would never hit. The gate is formsStamp, which only
        BuildFormList bumps.
        """
        order = self.source[self.source.index("void ShopBrowser::ApplyViewOrder()"):]
        order = order[:order.index("\n\tvoid ", 10)]
        self.assertIn("if (nameSortKeysStamp != formsStamp || nameSortKeys.size() != forms.size()) {", order)
        self.assertIn("nameSortKeysStamp = formsStamp;", order)
        self.assertIn("{} name sort key(s) built in {:.2f} ms", order)
        self.assertNotIn("nameSortKeysStamp = listStamp;", order)
        # The sort clock starts after the keys exist, so "view order ... in X ms"
        # is the sort, and the key build logs its own number.
        self.assertLess(order.index("nameSortKeysStamp = formsStamp;"),
                        order.index("const auto started = std::chrono::steady_clock::now();"))

    def test_comparator_is_a_total_order(self) -> None:
        order = self.source[self.source.index("void ShopBrowser::ApplyViewOrder()"):]
        order = order[:order.index("\n\tvoid ", 10)]
        # The index fallback made the comparison inconsistent with the key
        # order for any pair touching a missing form.
        self.assertNotIn("return a < b;", order)
        self.assertIn("fa ? fa->GetFormID() : 0u", order)
        self.assertIn("fb ? fb->GetFormID() : 0u", order)

    def test_padding_set_covers_space_and_private_use(self) -> None:
        body = self.function_body("IsNamePadding")
        for token in ("0x20", "0xA0", "0xE000", "0xF8FF", "0x2000", "0x200F", "0xFEFF"):
            self.assertIn(token, body, token)


if __name__ == "__main__":
    unittest.main()
