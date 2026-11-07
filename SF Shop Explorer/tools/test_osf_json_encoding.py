"""The OSF bridge's JSON has to be UTF-8, not Windows-1252.

The report: \"sorter name and type fail\", with the log saying

    OSF shop page: Catalog query failed: legacy handler returned invalid JSON

only for those two columns. A NAME sort puts \"Action Figure: Fleet Admiral
Francois Sanon\" (one cp1252 byte in the name) on page 1, and a TYPE sort puts
the Aid shelf first - whose \"Cafe et the Prime\" carries the same. Every other
column happened not to land a cp1252 name on the first page, so the reply parsed
and the click looked like it worked.

`Esc` used to copy every byte >= 0x20 straight into the JSON string. A cp1252
accent is a single byte, which is not valid UTF-8, so the whole reply was
rejected by the bridge before the page ever saw it. These tests pin the fix: the
conversion table is there, valid UTF-8 is passed through untouched, and every
text field of a row goes through `Esc`.

They are source-level; the socket itself needs the game. The behaviour verified
in game is the one that matters: clicking NAME and TYPE now returns rows instead
of the invalid-JSON error.
"""
from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FRONTEND = ROOT / "src" / "OsfFrontend.cpp"


class OsfJsonEncodingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if not FRONTEND.is_file():
            raise unittest.SkipTest(f"missing {FRONTEND}")
        cls.text = FRONTEND.read_text(encoding="utf-8-sig")
        start = cls.text.index("std::uint32_t Cp1252Punct(")
        end = cls.text.index("std::string RowJson(", start)
        cls.esc = cls.text[start:end]

    def test_cp1252_punctuation_is_mapped(self) -> None:
        # The 0x80-0x9F range is where cp1252 differs from ISO-8859-1; a quote
        # or a trademark sign in a mod name arrives as one of these bytes.
        self.assertIn("case 0x91: return 0x2018;", self.esc)
        self.assertIn("case 0x92: return 0x2019;", self.esc)
        self.assertIn("case 0x93: return 0x201C;", self.esc)
        self.assertIn("case 0x99: return 0x2122;", self.esc)
        self.assertIn("case 0x80: return 0x20AC;", self.esc)
        # Undefined bytes in that range answer 0 so the caller can write '?'.
        self.assertIn("default: return 0;", self.esc)

    def test_high_bytes_become_two_byte_utf8(self) -> None:
        # 0xE9 (accent aigu) has to leave as C3 A9, not as a raw E9.
        self.assertIn("if (c >= 0xA0) {", self.esc)
        self.assertIn("push(c);", self.esc)
        self.assertIn("out += static_cast<char>(0xC0 | (a_cp >> 6));", self.esc)
        self.assertIn("out += static_cast<char>(0x80 | (a_cp & 0x3F));", self.esc)

    def test_valid_utf8_is_passed_through(self) -> None:
        # A record that really is UTF-8 must not be double-encoded.
        self.assertIn("if (c >= 0xC2 && c <= 0xF4) {", self.esc)
        self.assertIn("out.append(a_text.substr(i, len));", self.esc)
        self.assertIn("(static_cast<unsigned char>(a_text[i + k]) & 0xC0) == 0x80", self.esc)

    def test_escaping_still_happens_after_the_conversion(self) -> None:
        self.assertIn('case \'"\': out += "\\\\\\""; break;', self.esc)
        self.assertIn("case '\\\\': out += \"\\\\\\\\\"; break;", self.esc)
        self.assertIn('out += std::format("\\\\u{:04x}", c);', self.esc)

    def test_every_row_field_is_escaped(self) -> None:
        body = self.text[self.text.index("std::string RowJson("):]
        body = body[:body.index("std::string DetailJson(")]
        for field in ("name", "edid", "type", "sub", "plugin"):
            self.assertIn(r'\"' + field + r'\":\"{}\"', body)
        self.assertEqual(body.count("Esc(a_row."), 5, "every text field of a row goes through Esc")


if __name__ == "__main__":
    unittest.main(verbosity=2)
