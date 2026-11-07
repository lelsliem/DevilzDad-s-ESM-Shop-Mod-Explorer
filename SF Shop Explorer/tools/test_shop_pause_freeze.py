"""Guards for the shop's world-freeze and game-audio duck.

The freeze writes a build-specific field, so it cannot be unit-tested against a
live game from here. What can be pinned is the part that went wrong twice:

* 0.8.334 asked GameSettingCollection for `fAudioVolume*`, which only exist as
  INI prefs - every lookup returned null and the ambience kept playing.
* the freeze check was scheduled from the SFSE task only, and the freeze is
  what stops that task, so no check ever reached the log. The evidence has to
  come from Present, which keeps running while the world is paused.

These are source assertions on purpose: the values they protect (which
collection is searched, which frame source holds the floor, that the check
takes two samples) are exactly the ones a refactor drops silently.
"""

from __future__ import annotations

import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
PAUSE_CPP = ROOT / "src" / "ShopPause.cpp"
PAUSE_H = ROOT / "src" / "ShopPause.h"
OVERLAY_CPP = ROOT / "src" / "ImguiOverlay.cpp"
AUDIO_CPP = ROOT / "src" / "ShopAudio.cpp"
AUDIO_H = ROOT / "src" / "ShopAudio.h"
EXPLORER_CPP = ROOT / "src" / "ShopExplorer.cpp"


class AudioDuckTests(unittest.TestCase):
    def setUp(self) -> None:
        self.source = PAUSE_CPP.read_text(encoding="utf-8-sig")

    def test_audio_settings_are_read_from_ini_prefs(self) -> None:
        self.assertIn("INIPrefSettingCollection::GetSingleton()", self.source)
        self.assertIn("INISettingCollection::GetSingleton()", self.source)

    def test_game_setting_collection_is_not_used_for_audio(self) -> None:
        # The name may appear in the comment that records the old bug; what
        # must not come back is a lookup through it.
        for call in ("GameSettingCollection::GetSingleton",
                     "ID::GameSettingCollection"):
            self.assertNotIn(call, self.source,
                             "fAudioVolume* are INI prefs; GameSettingCollection "
                             "returns null for all four (0.8.334)")

    def test_all_four_channels_are_ducked_and_restored(self) -> None:
        for key in ("fAudioVolumeMaster", "fAudioVolumeMusic",
                    "fAudioVolumeVoice", "fAudioVolumeEffects"):
            self.assertIn(key, self.source)
        self.assertIn("SetFloat(0.0f)", self.source)
        # Restore writes the captured original, never a constant.
        self.assertIn("SetFloat(g_ducked[i].original)", self.source)


class GameSilenceTests(unittest.TestCase):
    """The setting write is best effort; the session mute is the real silence."""

    def test_shop_has_its_own_core_audio_session(self) -> None:
        source = AUDIO_CPP.read_text(encoding="utf-8-sig")
        self.assertIn("kShopSessionGuid", source)
        self.assertIn("g_mixFormat, &kShopSessionGuid", source,
                      "a NULL session GUID puts the shop in the game's session")

    def test_default_session_is_muted_on_freeze(self) -> None:
        audio = AUDIO_CPP.read_text(encoding="utf-8-sig")
        self.assertIn("GetSimpleAudioVolume(nullptr, 0, &volume)", audio)
        self.assertIn("SetMute(a_muted ? TRUE : FALSE, nullptr)", audio)
        pause = PAUSE_CPP.read_text(encoding="utf-8-sig")
        self.assertIn("ShopAudio::SetGameSessionMuted(a_frozen);", pause)
        self.assertIn("void SetGameSessionMuted(bool a_muted);",
                      AUDIO_H.read_text(encoding="utf-8-sig"))

    def test_settings_probe_reports_what_the_collections_hold(self) -> None:
        pause = PAUSE_CPP.read_text(encoding="utf-8-sig")
        self.assertIn("ProbeSettingCollections", pause)
        self.assertIn("audioKeys=", pause)


class DirectInventoryTests(unittest.TestCase):
    """A bought item must never be silently lost, modded or not."""

    def test_modded_route_is_on_and_keeps_the_plain_add(self) -> None:
        # 0.8.338 turns the world route back on because its delivery is now
        # the engine's activation instead of the move request that never
        # landed (guarded by tools/test_rarity_delivery.py). The plain,
        # confirmed inventory add stays as the path whenever the world route
        # is switched off or refused, so a basket line cannot go missing.
        source = EXPLORER_CPP.read_text(encoding="utf-8-sig")
        basket = source[source.index("void ShopBrowser::ConfirmBasket("):]
        self.assertIn('LoadShopIniBool(L"attachMods", true)', basket)
        self.assertIn("useWorldRoute = hasSelectedMods && attachMods", basket)
        self.assertIn("AddObjectToContainer(item.bound", basket)

    def test_console_host_is_gone(self) -> None:
        self.assertFalse((ROOT / "src" / "ShopMenu.cpp").exists())
        self.assertFalse((ROOT / "src" / "ShopMenu.h").exists())
        xmake = (ROOT / "xmake.lua").read_text(encoding="utf-8-sig")
        self.assertNotIn("ShopMenu.cpp", xmake)
        explorer = EXPLORER_CPP.read_text(encoding="utf-8-sig")
        self.assertNotIn("RE::Console::MENU_NAME", explorer)


class FreezeEvidenceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.source = PAUSE_CPP.read_text(encoding="utf-8-sig")
        self.header = PAUSE_H.read_text(encoding="utf-8-sig")

    def test_present_hook_holds_the_floor(self) -> None:
        self.assertIn("void TickRender();", self.header)
        self.assertIn("void TickRender()", self.source)
        self.assertIn("ShopPause::TickRender();", OVERLAY_CPP.read_text(encoding="utf-8-sig"))

    def test_tick_render_is_before_the_presents_early_outs(self) -> None:
        overlay = OVERLAY_CPP.read_text(encoding="utf-8-sig")
        body = overlay[overlay.index("void RenderOnPresent("):]
        call = body.index("ShopPause::TickRender();")
        lock = body.index("std::scoped_lock lock(g_mutex);")
        self.assertLess(call, lock,
                        "the hold must run before Present can early-out")

    def test_freeze_check_samples_twice(self) -> None:
        # One delta sample cannot tell "paused" from "same value as last frame";
        # the second sample is what makes the log a measurement.
        self.assertIn("kFirstSampleFrame", self.source)
        self.assertIn("kSecondSampleFrame", self.source)
        self.assertIn("world clock held", self.source)
        self.assertIn("world clock still advancing", self.source)

    def test_freeze_check_resets_on_each_open(self) -> None:
        set_frozen = self.source[self.source.index("void SetFrozen("):]
        self.assertIn("g_freezeCheckFrame.store(0", set_frozen)
        self.assertIn("g_freezeFirstSample.store(false", set_frozen)

    def test_hold_is_still_gated(self) -> None:
        raise_counter = self.source[self.source.index("void RaiseCounterToFloor("):]
        guard = raise_counter.index("BuildGate::Verified()")
        write = raise_counter.index("CounterPtr()")
        self.assertLess(guard, write)


if __name__ == "__main__":
    unittest.main()
