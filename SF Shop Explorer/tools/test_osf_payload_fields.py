"""Guard: every request field the native OSF front end reads is sent by the page.

The page posts JSON objects and OsfFrontend.cpp reads them by name with
in.Str("x") / in.Bool("x") / in.Int("x") / in.FormId("x") / in.Has("x"). If one
side renames a field the other keeps sending or reading the old name, and the
failure is silent: the default value is used. This test pins the names on both
sides so a rename fails here instead of in game.

Only the request direction is checked. Native reply keys are not compared.
"""
from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OSF_SRC = ROOT / "src" / "OsfFrontend.cpp"
# Every script the view loads, not just app.js: audio.js owns the audio panel
# and sends fields app.js never mentions (`persist`), so scanning one file
# reported a field as unsent while the page was sending it all along.
OSF_VIEW = (
    ROOT.parent
    / "OSF Shop Explorer"
    / "SFSE"
    / "Plugins"
    / "OSFUI"
    / "views"
    / "devilzdad.osfshop"
    / "shop"
)

READ = re.compile(r'\b(?:in|a_in)\.(?:Bool|Str|Int|FormId|Has)\(\s*"(\w+)"')

# Keys the native side reads that the page does not send by design. Each one
# needs a reason; the list should stay empty or near-empty.
NOT_SENT_BY_PAGE: set[str] = set()


def native_fields(text: str) -> set[str]:
    return set(READ.findall(text))


def page_sends(text: str, key: str) -> bool:
    # A key in an object literal: `key: value`, `'key': value` or `"key": value`.
    return re.search(rf"(?<![\w.])['\"]?{re.escape(key)}['\"]?\s*:", text) is not None


class PayloadFieldTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.native = native_fields(OSF_SRC.read_text(encoding="utf-8-sig"))
        cls.page = "\n".join(
            path.read_text(encoding="utf-8-sig")
            for path in sorted(OSF_VIEW.glob("*.js"))
        )
        cls.page_files = [path.name for path in sorted(OSF_VIEW.glob("*.js"))]

    def test_native_reads_fields(self) -> None:
        # Guards the regex itself: if the reader pattern stops matching, the
        # check below would pass vacuously.
        self.assertIn("formId", self.native)
        self.assertIn("op", self.native)
        self.assertGreater(len(self.native), 20)

    def test_page_scripts_were_found(self) -> None:
        # Guards the glob above: an empty set of files would pass vacuously.
        self.assertIn("app.js", self.page_files)
        self.assertIn("audio.js", self.page_files)

    def test_page_sends_every_field_native_reads(self) -> None:
        missing = sorted(
            key
            for key in self.native - NOT_SENT_BY_PAGE
            if not page_sends(self.page, key)
        )
        self.assertEqual(
            missing,
            [],
            "OsfFrontend.cpp reads these fields but app.js never sends them",
        )


if __name__ == "__main__":
    unittest.main()
