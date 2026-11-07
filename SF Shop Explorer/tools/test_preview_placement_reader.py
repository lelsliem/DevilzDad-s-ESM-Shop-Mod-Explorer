"""Guard for the per-form preview-placement reader.

TESForm::Is<T>() is implemented as `Is(T::FORMTYPE)` - a form-type comparison,
not an RTTI test. TESBoundObject declares SF_RTTI_VTABLE but no SF_FORMTYPE, so
`As<RE::TESBoundObject>()` returns nullptr for every concrete bound form.

Measured in game 2026-10-17 (first run of the reader):

    preview-placement: 00000FD6 is not a TESBoundObject; no authored placement

00000FD6 is the Razorback - a weapon that carries exactly the data the reader
exists to read. The reader must therefore reach TESBoundObject through the
concrete class, and the two near-misses below are pinned too:

  * a KEYM form reports KEYM even though TESKey derives TESObjectMISC, so an
    As<TESObjectMISC>() on a key is false as well;
  * INGR has no mapped class in this SDK, so it cannot be cast at all and must
    be named in the log rather than reported as absent data.
"""
from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "ShopExplorer.cpp"


class PreviewPlacementReaderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.src = SOURCE.read_text(encoding="utf-8-sig")

    def test_the_unusable_base_cast_is_absent(self) -> None:
        self.assertNotIn("As<RE::TESBoundObject>()", self.src)
        self.assertIn("RE::TESBoundObject* BoundObjectOf(RE::TESForm* a_form)", self.src)

    def test_key_forms_are_cast_to_teskey_not_the_misc_base(self) -> None:
        resolver = self.src.split("RE::TESBoundObject* BoundObjectOf(RE::TESForm* a_form)", 1)[1]
        resolver = resolver[: resolver.index("\n\t\t}")]
        key_case = resolver.split("kKEYM):", 1)[1].split("\n", 1)[0]
        self.assertIn("RE::TESKey", key_case)
        self.assertNotIn("TESObjectMISC", key_case)
        # And the classes each case reaches really do carry the member.
        for cls in ("TESObjectWEAP", "TESObjectARMO", "TESObjectMISC", "TESObjectBOOK", "AlchemyItem", "TESAmmo"):
            self.assertIn(f"As<RE::{cls}>()", resolver)

    def test_unmapped_form_types_are_named_not_reported_as_missing_data(self) -> None:
        self.assertIn("form type {} has no mapped bound-object class", self.src)
        self.assertNotIn("is not a TESBoundObject", self.src)

    def test_reader_still_reads_the_authored_transform_records(self) -> None:
        reader = self.src.split("void LogPreviewPlacement(const RE::TESForm* a_form)", 1)[1]
        reader = reader[: reader.index("bool TryNativeVendorPreview")]
        self.assertIn("bound->previewTransform.transforms[ctx]", reader)
        self.assertIn("d.position.x", reader)
        # One line per form, not per frame: the guard must stay in place.
        self.assertIn("if (fid == s_lastForm) return;", reader)
        self.assertIn("LogPreviewPlacement(previewForm);", self.src)


if __name__ == "__main__":
    unittest.main()
