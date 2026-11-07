"""Guards for the "PgUp starts the music and the screen freezes" report.

Four users on the Nexus thread described the same thing: Page Up starts the
score, the world goes still, and no interface appears; pressing Page Up again
resumes play. The sticky post adds the other half - "there's a bug with PgUp,
it sometimes requires multiple presses".

Both come from the same place. `ShopBrowser::Open` froze the world and started
the music unconditionally, but drawing is conditional in two other places:
`SetInputEnabled` only enables it when `[overlay] render` is 4, and
`RenderOnPresent` returns early unless the swap chain is the one the DXGI hook
recorded. When either is false the shop is open, unpaused, audible, and
invisible. The toggle itself was sampled once per tick with `GetAsyncKeyState`,
so a press that began and ended inside one long frame was never seen at all.

Pinned here:

  1. `Open()` refuses before it touches the pause counter when the overlay has
     no draw path, and names the cause once instead of every press.
  2. A session that opens but is never drawn is closed by a watchdog, so the
     world cannot stay frozen.
  3. The toggle has an edge from the window procedure, so one press is one
     toggle; auto-repeat is dropped so holding the key cannot flip the shop.
  4. The drawn-frame counter advances only inside the successful draw path.
"""
from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OVERLAY = ROOT / "src" / "ImguiOverlay.cpp"
OVERLAY_H = ROOT / "src" / "ImguiOverlay.h"
EXPLORER = ROOT / "src" / "ShopExplorer.cpp"
EXPLORER_H = ROOT / "src" / "ShopExplorer.h"


class OpenGuardTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = EXPLORER.read_text(encoding="utf-8-sig")
        cls.open_body = cls.source[
            cls.source.index("void ShopBrowser::Open()") : cls.source.index(
                "void ShopBrowser::ReleaseSession()"
            )
        ]

    def test_open_refuses_when_the_overlay_cannot_draw(self) -> None:
        self.assertIn("if (!Overlay::CanDraw())", self.open_body)
        self.assertIn("Overlay::RenderMode()", self.open_body)
        self.assertIn("Overlay::PresentHooked()", self.open_body)

    def test_refusal_runs_before_the_world_is_frozen(self) -> None:
        # The freeze is what made the failure unrecoverable; the gate has to be
        # above it, not beside it.
        self.assertLess(
            self.open_body.index("if (!Overlay::CanDraw())"),
            self.open_body.index("ShopPause::SetFrozen(true)"),
        )
        self.assertLess(
            self.open_body.index("if (!Overlay::CanDraw())"),
            self.open_body.index("ShopAudio::PlaySfx"),
        )

    def test_refusal_is_logged_once_not_per_press(self) -> None:
        gate = self.open_body[
            self.open_body.index("if (!Overlay::CanDraw())") : self.open_body.index(
                "std::scoped_lock lock(stateMutex)"
            )
        ]
        self.assertIn("static bool loggedRefusal = false;", gate)
        self.assertIn("REX::ERROR(", gate)
        self.assertIn("return;", gate)

    def test_the_osf_build_is_not_gated_on_the_overlay(self) -> None:
        # The OSF front end has no overlay at all; the guard must not close the
        # only door it has.
        gate_start = self.source.index("void ShopBrowser::Open()")
        gate = self.source[gate_start : gate_start + 400]
        self.assertIn("#ifndef SFE_OSF_FRONTEND", gate)


class DrawWatchdogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = EXPLORER.read_text(encoding="utf-8-sig")
        cls.header = EXPLORER_H.read_text(encoding="utf-8-sig")
        cls.update_body = cls.source[
            cls.source.index("void ShopBrowser::FrameUpdate()") : cls.source.index(
                "void ShopBrowser::ProcessPendingActions()"
            )
        ]

    def test_watchdog_closes_a_session_that_never_drew(self) -> None:
        self.assertIn("Overlay::FramesDrawn()", self.update_body)
        self.assertIn("kDrawGraceMs", self.update_body)
        self.assertIn("Close();", self.update_body)

    def test_watchdog_disarms_once_a_frame_lands(self) -> None:
        self.assertIn("drawWatchFrames", self.update_body)
        self.assertIn("} else if (drawn != drawWatchFrames) {", self.update_body)
        self.assertIn("drawWatchArmed = false;", self.update_body)

    def test_close_clears_the_watch(self) -> None:
        close_body = self.source[
            self.source.index("void ShopBrowser::Close()") : self.source.index(
                "const std::filesystem::path& ShopBrowser::IniPath()"
            )
        ]
        self.assertIn("drawWatchArmed = false;", close_body)

    def test_state_lives_on_the_browser(self) -> None:
        self.assertIn("bool drawWatchArmed{ false };", self.header)
        self.assertIn("std::uint64_t drawWatchFrames{ 0 };", self.header)


class ToggleEdgeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.overlay = OVERLAY.read_text(encoding="utf-8-sig")
        cls.proc = cls.overlay[
            cls.overlay.index("LRESULT WINAPI WindowProc(") : cls.overlay.index(
                "bool InitImGui()"
            )
        ]
        cls.explorer = EXPLORER.read_text(encoding="utf-8-sig")

    def test_toggle_edge_is_recorded_while_the_shop_is_closed(self) -> None:
        # The block has to sit above the drawing-gated swallow, otherwise the
        # key is only seen while it is already open - which is the bug.
        swallow = self.proc.index("&& IsOverlayInputMessage(a_message)) {")
        edge = self.proc.index("g_toggleKey.load(std::memory_order_acquire)")
        self.assertLess(edge, swallow)
        self.assertIn(
            "g_rawKeyPresses[vk >> 6].fetch_or(1ull << (vk & 63), std::memory_order_acq_rel);",
            self.proc[edge:swallow],
        )

    def test_auto_repeat_cannot_flip_the_shop(self) -> None:
        swallow = self.proc.index("&& IsOverlayInputMessage(a_message)) {")
        block = self.proc[self.proc.index("g_toggleKey.load(std::memory_order_acquire)") : swallow]
        self.assertIn("(1u << 30)", block)
        self.assertIn("if (!repeat) {", block)

    def test_task_consumes_the_edge_and_keeps_the_poll_as_fallback(self) -> None:
        update = self.explorer[
            self.explorer.index("void ShopBrowser::FrameUpdate()") : self.explorer.index(
                "void ShopBrowser::ProcessPendingActions()"
            )
        ]
        self.assertIn("Overlay::ConsumeKeyPress(toggleKey)", update)
        self.assertIn("GetAsyncKeyState(toggleKey)", update)
        self.assertIn("const bool toggle = pressed || held;", update)

    def test_toggle_key_is_atomic_read_across_threads(self) -> None:
        self.assertIn("std::atomic<int> g_toggleKey{ VK_PRIOR };", self.overlay)


class LivenessCounterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.overlay = OVERLAY.read_text(encoding="utf-8-sig")
        cls.header = OVERLAY_H.read_text(encoding="utf-8-sig")

    def test_counter_advances_only_after_a_submitted_frame(self) -> None:
        draw = self.overlay[
            self.overlay.index("void RenderImGuiFrame()") : self.overlay.index(
                "bool WaitForGpu("
            )
        ]
        self.assertIn("ImGui_ImplDX12_RenderDrawData", draw)
        self.assertLess(
            draw.index("ImGui_ImplDX12_RenderDrawData"),
            draw.index("g_shopFramesDrawn.fetch_add(1"),
        )

    def test_draw_gate_is_the_recoverable_state_not_the_initialised_one(self) -> None:
        # Gating on g_imguiReady would refuse the first press of every session,
        # which is the complaint this change exists to remove.
        can_draw = self.overlay[
            self.overlay.index("bool CanDraw()") : self.overlay.index("bool PresentHooked()")
        ]
        self.assertIn("g_factoryInstalled.load", can_draw)
        self.assertIn("g_renderMode.load(std::memory_order_acquire) >= 4", can_draw)
        self.assertNotIn("g_imguiReady", can_draw)

    def test_probes_are_declared(self) -> None:
        self.assertIn("[[nodiscard]] bool CanDraw();", self.header)
        self.assertIn("[[nodiscard]] bool PresentHooked();", self.header)
        self.assertIn("[[nodiscard]] int RenderMode();", self.header)
        self.assertIn("[[nodiscard]] std::uint64_t FramesDrawn();", self.header)


if __name__ == "__main__":
    unittest.main(verbosity=2)
