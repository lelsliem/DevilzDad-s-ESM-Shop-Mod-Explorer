"""Regression checks for the audio worker's SFSE unload lifecycle."""
from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
AUDIO_SOURCE = ROOT / "src" / "ShopAudio.cpp"
OVERLAY_SOURCE = ROOT / "src" / "ImguiOverlay.cpp"


class ShopAudioLifecycleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.audio = AUDIO_SOURCE.read_text(encoding="utf-8-sig")
        cls.overlay = OVERLAY_SOURCE.read_text(encoding="utf-8-sig")

    def test_worker_is_not_a_global_joinable_std_thread(self) -> None:
        self.assertIsNone(re.search(r"\bstd::thread\s+\w+\s*;", self.audio))
        self.assertIn("_beginthreadex(", self.audio)
        self.assertIn("GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS", self.audio)
        self.assertIn("g_workerModulePin = modulePin;", self.audio)

    def test_worker_exit_always_notifies_waiters_and_releases_com_resources(self) -> None:
        worker = self.audio.split("unsigned __stdcall AudioWorkerEntry(", 1)[1].split("\n\tvoid Init()", 1)[0]
        self.assertIn("catch (const std::exception& e)", worker)
        self.assertIn("catch (...) ", worker)
        self.assertIn("CloseWasapi();", worker)
        self.assertIn("g_workerActive.store(false", worker)
        self.assertIn("g_workerStopped.notify_all();", worker)

    def test_shutdown_waits_for_worker_then_releases_handle_and_module_pin(self) -> None:
        shutdown = self.audio.split("\tvoid Shutdown()", 1)[1].split("\n\tvoid PlaySfx(", 1)[0]
        self.assertIn("g_workerStopped.wait(lock", shutdown)
        self.assertIn("!g_workerActive.load", shutdown)
        self.assertIn("WaitForSingleObject(worker, INFINITE)", shutdown)
        self.assertIn("CloseHandle(worker)", shutdown)
        self.assertIn("Deliberately retain g_workerModulePin", shutdown)
        self.assertIn("SFSE unloads", self.audio)

    def test_window_destroy_stops_audio_outside_dllmain(self) -> None:
        proc = self.overlay.split("LRESULT WINAPI WindowProc(", 1)[1].split("bool InitImGui()", 1)[0]
        self.assertIn("a_message == WM_DESTROY", proc)
        self.assertIn("ShopAudio::Shutdown();", proc)
        self.assertLess(proc.index("ShopAudio::Shutdown();"), proc.index("CallWindowProcW"))
        self.assertIn("DllMain/under loader lock", proc)

    def test_worker_lifecycle_logs_start_stop_and_wasapi_cleanup(self) -> None:
        self.assertIn('REX::INFO("SFShopExplorer audio: worker starting', self.audio)
        self.assertIn('REX::INFO("SFShopExplorer audio: worker stopped")', self.audio)
        self.assertIn('REX::INFO("SFShopExplorer audio: WASAPI resources cleaned up")', self.audio)
        self.assertIn('REX::INFO("SFShopExplorer audio: shutdown complete', self.audio)
        cleanup = self.audio.split("void CloseWasapi()", 1)[1].split("// One-shot voice state.", 1)[0]
        self.assertLess(cleanup.index("if (g_client)"), cleanup.index("WASAPI resources cleaned up"))


if __name__ == "__main__":
    unittest.main()
