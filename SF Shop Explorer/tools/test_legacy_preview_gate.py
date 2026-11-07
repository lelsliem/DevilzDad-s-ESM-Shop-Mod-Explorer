"""Guard for the legacy-preview gate ([preview] legacyRenderer=0).

The trap this pins is a two-painter one. The shop PREVIEW viewport can be
painted by two independent plugin-side routes:

  * the direct GPU render   - OfflineModel's SRV, drawn when `offlineCurrent`;
  * the captured-scene route - ItemCapture's SRVs, drawn in the *else* arm
    ("the direct render is not current").

So disabling only the direct render does not blank the viewport: ItemCapture
would still paint a game frame there, and a game frame in that rectangle is
indistinguishable from a native result to anyone testing the native route.
Every assertion below exists to keep both routes - and the status text that
accompanies them - behind the single `legacyPreview` flag.

These are source-structure assertions: exercising the gate itself needs a
running game with the INI set, which this suite cannot do.
"""
from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "ShopExplorer.cpp"


class LegacyPreviewGateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.src = SOURCE.read_text(encoding="utf-8-sig")

    def test_gate_reads_its_own_ini_key_and_defaults_to_enabled(self) -> None:
        self.assertIn('GetPrivateProfileIntW(L"preview", L"legacyRenderer", 1,', self.src)
        self.assertRegex(self.src, r"bool LegacyPreviewDisabled\(\)")

    def test_capture_module_cannot_be_armed_with_the_gate_off(self) -> None:
        self.assertIn("SetShopWantsCapture(legacyPreview && previewForm != nullptr)", self.src)
        self.assertNotRegex(self.src, r"SetShopWantsCapture\(previewForm != nullptr\)")

    def test_direct_render_request_is_built_from_the_selected_source(self) -> None:
        # Both sources share one request path, so the panel cannot be handed a
        # fresh SRV that came from neither. The gate chooses the source; it no
        # longer blanks the rectangle.
        self.assertIn("if (previewForm) {", self.src)
        block = self.src.split("if (previewForm) {", 1)[1]
        block = block[: block.index("auto drawViewport")]
        # Same single request path; the trailing flag says whether the caller
        # already applied the form's authored rotation.
        self.assertIn("RequestPreviewDirect(std::move(direct), previewLabel, isWeapon, authored);", block)
        self.assertIn("offlineCurrent = offline.ready", block)
        # The snapshot must still be rejected when it belongs to another form.
        self.assertIn('offline.srvHandle != 0 && offline.label == previewLabel', block)

    def test_captured_scene_painter_is_behind_the_gate(self) -> None:
        self.assertRegex(
            self.src,
            r"if \(legacyPreview && !offlineCurrent && capture\.copiedCount > 0 && capture\.srvHandles\[capture\.primarySlot\] != 0\)",
        )

    def test_capture_and_status_text_are_behind_the_gate(self) -> None:
        self.assertRegex(self.src, r"if \(legacyPreview && capture\.mode != SFE::ItemCapture::Mode::kDisabled &&")
        # The "building direct GPU preview..." branch is the third painter of
        # this rectangle (it writes where the image would be).
        self.assertIn("} else if (legacyPreview && !offlineCurrent) {", self.src)

    def test_the_misleading_disabled_message_stays_silent_under_the_gate(self) -> None:
        # Snapshot::enabled defaults to false, so with the gate off this branch
        # would name [offlinepreview] enabled=0 - a key the gate never reads.
        self.assertIn("} else if (legacyPreview && !offline.enabled) {", self.src)
        self.assertNotIn("} else if (!offline.enabled) {", self.src)

    def test_gate_off_reads_the_engine_loaded_model_instead_of_drawing_nothing(self) -> None:
        # The gate no longer means "paint nothing": it selects the native
        # source. Extraction therefore runs in both modes, and the gated arm
        # must ask the barter read-back rather than Clone3D.
        self.assertIn("if (previewForm) {", self.src)
        self.assertIn("s_native = legacyPreview ? SFE::NativeModel::LoadFromForm(fid, models)", self.src)
        self.assertIn(": SFE::NativeModel::LoadFromBarterMenu(fid);", self.src)
        # Legacy paths are still resolved only in legacy mode; the native arm
        # must not invent NIF paths the engine already resolved.
        guarded = self.src.split("if (previewForm) {", 1)[1]
        guarded = guarded[: guarded.index("const auto fid")]
        self.assertIn("if (legacyPreview) {", guarded)
        self.assertIn("models = FormModelPaths(previewForm);", guarded)

    def test_menu_state_is_part_of_both_caches(self) -> None:
        # Measured failure this pins: the shop opens from the global form
        # catalog, so a hotkey-opened session has no BarterMenu and every
        # LoadModel dispatch lands on an empty sink list. Opening a vendor
        # afterwards must re-dispatch and re-read the same form, or the native
        # preview never leaves its first "menu closed" answer.
        self.assertIn("const bool open = SFE::NativeModel::BarterMenuAvailable();", self.src)
        self.assertIn("if(cur==lastForm && open==lastOpen) return lastOk;", self.src)
        self.assertIn("native dispatch deferred", self.src)
        self.assertIn("menuNow != s_lastNativeMenu", self.src)
        self.assertIn("lastOk=false; return false;", self.src)

    def test_read_back_reports_the_ui_census_on_failure(self) -> None:
        native = (ROOT / "src" / "NativeModel.cpp").read_text(encoding="utf-8-sig")
        self.assertIn("bool BarterMenuAvailable()", native)
        self.assertIn("DescribeBarterMenuState()", native)
        self.assertIn("registered={} instance={} open={}", native)
        # The sink count and the UI census are logged together: a zero count
        # alone cannot tell "never created" from "created but closed".
        self.assertIn("barter source 0x{:X} sink count={} ({})", native)

    def test_notice_only_replaces_an_empty_viewport_and_names_the_read_back(self) -> None:
        self.assertIn("NATIVE PREVIEW", self.src)
        self.assertIn("if (!legacyPreview && !offlineCurrent) {", self.src)
        self.assertIn("Engine read-back: {}", self.src)
        self.assertIn("native dispatch: {}", self.src)
        notice = self.src[self.src.index("NATIVE PREVIEW") - 900 :]
        self.assertIn("if (!legacyPreview && !offlineCurrent) {", notice)
        # A picture that is shown must name its source, because only one of
        # the two sources is a native result.
        self.assertIn('legacyPreview ? "direct 3D" : "engine model (barter)"', self.src)


    def test_dispatch_notice_reports_the_outcome_not_a_cached_success(self) -> None:
        # The notice is the only "result" the gated viewport shows, so the
        # cached path must replay the stored outcome. A bare `return true`
        # there made a crashed dispatch read ok one frame later and a
        # re-selected form read ok without dispatching at all.
        body = self.src.split("bool TryNativeVendorPreview(", 1)[1]
        body = body[: body.index("REX::INFO(\"SFShopExplorer: native vendor preview dispatched")]
        self.assertIn("static bool lastOk", body)
        # Menu state was later added as a second cache key; the cached path must
        # still replay the stored outcome rather than assuming success.
        self.assertRegex(body, r"if\s*\(\s*cur\s*==\s*lastForm\s*&&\s*open\s*==\s*lastOpen\s*\)\s*return\s+lastOk;")
        self.assertNotRegex(body, r"if\s*\(\s*cur\s*==\s*lastForm[^)]*\)\s*return\s+true;")
        self.assertIn("lastOk=false; return false;", body)
        self.assertIn("lastOk=ok;", body)


if __name__ == "__main__":
    unittest.main()
