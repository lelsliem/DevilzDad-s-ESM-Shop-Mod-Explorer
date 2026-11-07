r"""Regression checks for the 0.8.350 audio controls.

The shop ships one flat `Audio\` folder holding six `ui_*` stings and the music
tracks, so the file name is the entire sound/music contract. These checks pin
that contract and the settings that drive it, because a regression here is
silent: the shop still opens, the menu just has no music.

    python tools/test_audio_controls.py
"""
from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
AUDIO_SOURCE = ROOT / "src" / "ShopAudio.cpp"
AUDIO_HEADER = ROOT / "src" / "ShopAudio.h"
EXPLORER_SOURCE = ROOT / "src" / "ShopExplorer.cpp"
EXPLORER_HEADER = ROOT / "src" / "ShopExplorer.h"


class AudioTrackDiscoveryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.audio = AUDIO_SOURCE.read_text(encoding="utf-8-sig")

    def test_ui_prefix_is_excluded_case_insensitively(self) -> None:
        self.assertIn("IsMusicTrackName", self.audio)
        name = self.audio.split("bool IsMusicTrackName(", 1)[1].split("\n\t\tstd::string WidenToUtf8", 1)[0]
        # Lower-cased before the prefix test: a hand-renamed UI_CLICK.WAV must
        # stay a sting, not become a selectable track.
        self.assertIn("std::towlower", name)
        self.assertIn('stem.rfind(L"ui_", 0) != 0', name)
        # The picker accepts every format Media Foundation may decode. This was
        # a .wav-only gate before user tracks were supported; pin the allowlist
        # rather than the old single-extension check.
        for extension in (".wav", ".mp3", ".flac", ".ogg", ".m4a", ".aac", ".wma"):
            self.assertIn(f'L"{extension}"', name)
        self.assertIn("ext == candidate", name)

    def test_flat_folder_and_optional_music_subfolder_are_scanned(self) -> None:
        listing = self.audio.split("std::vector<std::string> ListMusicTracks()", 1)[1]
        listing = listing.split("\n\tvoid SetMusicFile(", 1)[0]
        self.assertIn('g_dir / L"Audio"', listing)
        self.assertIn('audio / L"Music"', listing)
        self.assertIn('"Music\\\\"', listing)

    def test_track_list_is_sorted_and_relative_to_audio(self) -> None:
        listing = self.audio.split("std::vector<std::string> ListMusicTracks()", 1)[1]
        listing = listing.split("\n\tvoid SetMusicFile(", 1)[0]
        self.assertIn("std::sort(tracks.begin(), tracks.end()", listing)
        self.assertIn("std::lexicographical_compare", listing)


class AudioSettingsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.explorer = EXPLORER_SOURCE.read_text(encoding="utf-8-sig")
        cls.header = EXPLORER_HEADER.read_text(encoding="utf-8-sig")

    def test_channels_are_independent_settings(self) -> None:
        for field in ("int musicVolume", "int sfxVolume", "bool musicMuted", "bool sfxMuted"):
            self.assertIn(field, self.header)
        # The old single on/off switch must not survive beside them: two owners
        # of "is music on" is how the theme switch used to fight the toolbar.
        self.assertNotIn("musicEnabled", self.explorer)
        self.assertNotIn("musicEnabled", self.header)

    def test_audio_keys_are_persisted_under_audio_section(self) -> None:
        save = self.explorer.split("void ShopBrowser::SaveAudioSettings()", 1)[1]
        save = save.split("void ShopBrowser::ApplyAudioSettings()", 1)[0]
        for key in ("musicVolume", "sfxVolume", "musicMuted", "sfxMuted", "musicTrack"):
            self.assertIn(f'L"{key}"', save)
        self.assertIn('L"audio"', save)

    def test_old_shop_music_toggle_is_migrated(self) -> None:
        load = self.explorer.split("void ShopBrowser::LoadAudioSettings()", 1)[1]
        load = load.split("void ShopBrowser::SaveAudioSettings()", 1)[0]
        # A -1 default marks the key absent, so an ini from an older build keeps
        # its music off instead of starting to play on first launch.
        self.assertIn('L"musicMuted", -1', load)
        self.assertIn('!LoadShopIniBool(L"music", true)', load)

    def test_missing_chosen_track_falls_back_to_theme(self) -> None:
        load = self.explorer.split("void ShopBrowser::LoadAudioSettings()", 1)[1]
        load = load.split("void ShopBrowser::SaveAudioSettings()", 1)[0]
        # A custom track deleted from Audio\ must not silence the shop forever.
        self.assertIn("is gone; using the theme track", load)
        self.assertIn("musicTrack.clear();", load)

    def test_effective_volume_multiplies_mute(self) -> None:
        apply = self.explorer.split("void ShopBrowser::ApplyAudioSettings()", 1)[1]
        apply = apply.split("std::string ShopBrowser::MusicTrackFile()", 1)[0]
        self.assertIn("musicMuted ? 0.0f", apply)
        self.assertIn("sfxMuted ? 0.0f", apply)
        self.assertIn("ShopAudio::SetMusicVolume", apply)
        self.assertIn("ShopAudio::SetSfxVolume", apply)

    def test_open_applies_volume_before_starting_the_fade(self) -> None:
        opened = self.explorer.split("void ShopBrowser::Open()", 1)[1]
        opened = opened.split("void ShopBrowser::ReleaseSession()", 1)[0]
        self.assertLess(opened.index("ApplyAudioSettings();"), opened.index("ShopAudio::StartMusic();"))
        self.assertIn("ShopAudio::SetMusicFile(MusicTrackFile().c_str());", opened)

    def test_chosen_track_outlives_a_theme_change(self) -> None:
        draw = self.explorer.split("void ShopBrowser::DrawImGui()", 1)[1]
        theme_switch = draw.split("if (ImGui::Selectable(ThemeLabel(candidate).c_str(), theme == t))", 1)[1]
        theme_switch = theme_switch.split("ImGui::EndCombo();", 1)[0]
        # A user-picked track must not be replaced by the theme's score.
        self.assertIn("if (musicTrack.empty())", theme_switch)
        self.assertLess(theme_switch.index("if (musicTrack.empty())"), theme_switch.index("ActiveTheme(t).music"))

    def test_track_picker_lists_theme_default_first(self) -> None:
        draw = self.explorer.split("void ShopBrowser::DrawImGui()", 1)[1]
        popup = draw.split('ImGui::BeginPopup("##shop_audio")', 1)[1]
        popup = popup.split("ImGui::EndPopup();", 1)[0]
        self.assertIn('"(theme track)"', popup)
        self.assertIn("ShopAudio::ListMusicTracks()", draw)
        self.assertIn("SliderInt", popup)
        self.assertIn("Checkbox(\"Mute##music_mute\"", popup)
        self.assertIn("Checkbox(\"Mute##sfx_mute\"", popup)


if __name__ == "__main__":
    unittest.main()
