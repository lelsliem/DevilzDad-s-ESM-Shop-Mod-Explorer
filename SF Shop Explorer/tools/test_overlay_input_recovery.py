"""Guards for the ESC-close/search-dead regression (2026-10-18).

The report: closing the shop with ESC left the catalog search box unable to take
characters on the next open; closing with the toggle key or the window X was
fine. The difference was traced through the log:

  * ESC leaked to the game (one `existing menu callback name=PauseMenu` 0.8 s
    after the only ESC close of the session), so a game menu opened.
  * While a game menu is up the engine's own message pump runs and calls
    TranslateMessage, so a real WM_CHAR reached the overlay once.
  * That WM_CHAR tripped the session-wide `g_hostTranslatesChars` latch, which
    turned off the overlay's synthesized characters for the rest of the process.
    When the menu closed the pump stopped, the host produced no more characters,
    and typing produced nothing at all.

The fixes are structural and are pinned here:

  1. The latch decays: the host counts as translating only while its characters
     keep arriving, so synthesis comes back once they stop.
  2. A swallowed key-down keeps its key-up swallowed, so the game never sees an
     orphan ESC up edge (which is what opened PauseMenu).
  3. Opening the shop clears stale ImGui keyboard state on the render thread.
  4. ESC leaves the search box first instead of exiting while typing.
  5. The ESC close path performs the same ItemCapture teardown as Close().

A later session added the same report with the preview popped out, and the log
showed a second, independent way to lose the keyboard: between `ui: preview
popped out` and `ui: preview returned inline` there is not a single `catalog
key` line, i.e. ImGui saw no key events at all, while the mouse kept working
(it drove the click-away). ImGui drops every key and every queued character
while `io.AppFocusLost` is set, and that is invisible to this overlay because
the pointer comes from raw input and its own virtual cursor. Hence:

  6. A stale focus loss is re-synced against the real foreground window, and
     focus transitions are logged.
  7. ESC dismisses the preview pop-out first, instead of paging back under it.
  8. The pop-out cannot own keyboard navigation, and catalog arrow keys are not
     disabled by an active item in it.
"""
from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OVERLAY = ROOT / "src" / "ImguiOverlay.cpp"
EXPLORER = ROOT / "src" / "ShopExplorer.cpp"


class HostTranslationLatchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = OVERLAY.read_text(encoding="utf-8-sig")

    def test_translation_state_decays_instead_of_latching_for_the_session(self) -> None:
        self.assertNotIn("g_hostTranslatesChars", self.source)
        self.assertIn("g_lastHostCharTick", self.source)
        self.assertIn("kHostTranslateWindowMs", self.source)
        body = self.source[self.source.index("bool HostTranslatingRecently()"):]
        self.assertIn("GetTickCount() - last < kHostTranslateWindowMs", body)

    def test_real_host_characters_refresh_the_timestamp(self) -> None:
        self.assertIn("g_lastHostCharTick.store(GetTickCount()", self.source)

    def test_synthesis_runs_while_the_host_is_silent(self) -> None:
        self.assertIn("if (a_message == WM_KEYDOWN && !HostTranslatingRecently())", self.source)
        self.assertIn("QueueTypedCharacter(a_wparam);", self.source)


class SwallowedKeyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = OVERLAY.read_text(encoding="utf-8-sig")
        start = cls.source.index("LRESULT WINAPI WindowProc(")
        end = cls.source.index("bool InitImGui()", start)
        cls.wndproc = cls.source[start:end]

    def test_key_up_is_consumed_before_the_capture_gate(self) -> None:
        # The shop may already be closed when the player releases ESC, so the
        # capture check must not be what decides whether the up edge is eaten.
        key_up = self.wndproc.index("ConsumeSwallowedKeyUp(a_wparam)")
        capture = self.wndproc.index("IsOverlayInputMessage(a_message)")
        self.assertLess(key_up, capture)
        self.assertIn("WM_KEYUP || a_message == WM_SYSKEYUP", self.wndproc)

    def test_key_downs_are_recorded_when_they_are_swallowed(self) -> None:
        self.assertIn("RememberSwallowedKeyDown(a_wparam)", self.wndproc)

    def test_swallowed_key_up_is_still_released_to_imgui(self) -> None:
        """Reported: one Backspace press in the search box or the quantity
        field deleted the text and kept deleting until the shop was reopened.

        Keeping the up away from the game is the point of the swallow, but the
        up also has to reach ImGui: it clears a key only on the matching up, and
        a key it still believes is held auto-repeats. tools/renderer_recon/
        backspace_latch_harness.cpp measures it on the vendored ImGui: 1 char
        deleted when the up is delivered, all 15 when it is not.
        """
        key_up = self.wndproc.index("if (a_message == WM_KEYUP || a_message == WM_SYSKEYUP) {")
        released = self.wndproc.index("QueueInputMessage(a_message, a_wparam, a_lparam);", key_up)
        swallows = self.wndproc.index("if (swallowed) {", key_up)
        # Queued first, then dropped for the game.
        self.assertLess(released, swallows)
        block = self.wndproc[key_up:swallows]
        self.assertIn("g_drawingEnabled.load", block)
        self.assertIn("g_captureInput.load", block)

    def test_bitset_is_atomic_and_per_key(self) -> None:
        self.assertIn("std::atomic<std::uint64_t> g_swallowedKeyDowns[4]", self.source)
        self.assertIn("fetch_or(1ull << (vk & 63)", self.source)
        self.assertIn("fetch_and(~mask", self.source)


class InputResetOnOpenTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = OVERLAY.read_text(encoding="utf-8-sig")

    def test_capture_enable_requests_a_reset(self) -> None:
        start = self.source.index("void SetInputEnabled(")
        end = self.source.index("void QueueInputMessage(", start)
        block = self.source[start:end]
        self.assertIn("if (inputEnabled && !wasInputEnabled) {", block)
        self.assertIn("g_resetImGuiInput.store(true", block)

    def test_reset_runs_on_the_render_thread_before_new_frame(self) -> None:
        reset = self.source.index("g_resetImGuiInput.exchange(false")
        new_frame = self.source.index("ImGui::NewFrame();", reset)
        self.assertLess(reset, new_frame)
        block = self.source[reset:new_frame]
        self.assertIn("io.ClearInputKeys()", block)
        self.assertIn("g_inputMessages.clear()", block)
        self.assertIn("g_lastHostCharTick.store(0", block)


class FocusLossTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = OVERLAY.read_text(encoding="utf-8-sig")

    def test_stale_focus_loss_is_resynced_before_new_frame(self) -> None:
        resync = self.source.index("ImGui::GetIO().AppFocusLost && g_hwnd && GetForegroundWindow() == g_hwnd")
        new_frame = self.source.index("ImGui::NewFrame();", resync)
        block = self.source[resync:new_frame]
        self.assertIn("AddFocusEvent(true)", block)

    def test_resync_only_when_the_game_window_really_is_foreground(self) -> None:
        # Never invent focus: ImGui must keep dropping input while another
        # application owns the keyboard.
        self.assertIn("GetForegroundWindow() == g_hwnd", self.source)

    def test_focus_transitions_are_logged(self) -> None:
        start = self.source.index("LRESULT WINAPI WindowProc(")
        end = self.source.index("bool InitImGui()", start)
        wndproc = self.source[start:end]
        self.assertIn("WM_SETFOCUS || a_message == WM_KILLFOCUS", wndproc)
        self.assertIn("game window focus {} (drawing={} capture={})", wndproc)


class PreviewPopoutKeysTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = EXPLORER.read_text(encoding="utf-8-sig")

    def test_escape_dismisses_the_popout_before_any_page_back(self) -> None:
        dismissed = self.source.index("preview returned inline (escape)")
        page_back = self.source.index("pendingBack = true;", dismissed)
        self.assertLess(dismissed, page_back)
        # The same press must not also page back / close the shop.
        handler = self.source[page_back - 200:page_back]
        self.assertIn("!escapeConsumed", handler)

    def test_popout_cannot_own_keyboard_navigation(self) -> None:
        self.assertIn("ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoNav", self.source)

    def test_catalog_arrows_are_not_gated_on_an_active_item(self) -> None:
        start = self.source.index("const bool keyboardReady")
        end = self.source.index(";", start)
        gate = self.source[start:end]
        self.assertNotIn("IsAnyItemActive", gate)
        self.assertIn("WantTextInput", gate)
        self.assertIn("IsPopupOpen", gate)

    def test_key_diagnostic_names_the_popout_and_focus_owners(self) -> None:
        start = self.source.index("SFShopExplorer ui: catalog key")
        line = self.source[start:start + 600]
        self.assertIn("popout={}", line)
        self.assertIn("itemActive={}", line)
        self.assertIn("textInput={}", line)


class RawKeyPathTests(unittest.TestCase):
    """The shop's Escape/arrow handling must not depend on ImGui's key state.

    The popped-out-preview log of 15:20 shows the whole problem: pop-out opens
    at 15:20:20.781, the window then loses focus at 15:20:23.965, and between
    those two timestamps there is not one `catalog key` line - ImGui saw no key
    events while mouse messages kept arriving. No amount of ImGui-side gating
    can fix that, so those keys now come from the window's own key stream.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cls.overlay = OVERLAY.read_text(encoding="utf-8-sig")
        cls.header = (ROOT / "src" / "ImguiOverlay.h").read_text(encoding="utf-8-sig")
        cls.explorer = EXPLORER.read_text(encoding="utf-8-sig")

    def test_overlay_exposes_an_edge_triggered_key_read(self) -> None:
        self.assertIn("bool ConsumeKeyPress(int a_vk);", self.header)
        self.assertIn("bool ConsumeKeyPress(int a_vk)", self.overlay)
        # Reading the bit must clear it, so one press is one action.
        self.assertIn("fetch_and(~mask", self.overlay)

    def test_raw_bits_are_set_from_wm_keydown(self) -> None:
        start = self.overlay.index("LRESULT WINAPI WindowProc(")
        end = self.overlay.index("bool InitImGui()", start)
        wndproc = self.overlay[start:end]
        self.assertIn("g_rawKeyPresses[vk >> 6].fetch_or(1ull << (vk & 63)", wndproc)
        self.assertIn("WM_KEYDOWN || a_message == WM_SYSKEYDOWN", wndproc)

    def test_escape_auto_repeat_is_ignored(self) -> None:
        # Holding Escape must not walk back through the pages.
        self.assertIn("!repeat || vk != VK_ESCAPE", self.overlay)

    def test_key_arrival_is_logged_for_the_next_session(self) -> None:
        self.assertIn("overlay key down vk={}", self.overlay)

    def test_shop_reads_escape_and_arrows_from_the_raw_path(self) -> None:
        self.assertIn("Overlay::ConsumeKeyPress(VK_ESCAPE)", self.explorer)
        for key in ("VK_UP", "VK_DOWN", "VK_LEFT", "VK_RIGHT"):
            self.assertIn(f"Overlay::ConsumeKeyPress({key})", self.explorer)
        self.assertNotIn("ImGui::IsKeyPressed(ImGuiKey_UpArrow", self.explorer)
        self.assertNotIn("ImGui::IsKeyPressed(ImGuiKey_Escape", self.explorer)


class EscapeCloseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = EXPLORER.read_text(encoding="utf-8-sig")

    def test_escape_leaves_the_search_box_first(self) -> None:
        self.assertIn("escapePressed && !escapeConsumed && !ImGui::GetIO().WantTextInput", self.source)

    def test_escape_close_tears_down_capture_like_close_does(self) -> None:
        start = self.source.index("void ShopBrowser::Back()")
        end = self.source.index("void ShopBrowser::StepList(", start)
        back = self.source[start:end]
        self.assertIn("open.store(false", back)
        self.assertIn("SFE::ItemCapture::SetShopWantsCapture(false);", back)


if __name__ == "__main__":
    unittest.main()
