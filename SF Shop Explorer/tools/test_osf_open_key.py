"""Guards for how the OSF build is opened and closed (2026-10-25).

The report: "its not using OSF as intended like the original did launch through
OSF and hotkey has issues Pgup twice to launch then Pgup and esc to close".

Both halves had one cause. FrameUpdate() still polled `[overlay] toggleKey`
and called Toggle() in the OSF build, while Osf::Tick() polled the same key and
asked OSF to open the view. One press therefore did two things - it closed the
engine session (music off, game audio back on) and asked OSF to show a view that
was usually already up - and the engine session drifted away from the screen.
The hub listing was separate: the shop's view was discovered, but a mod only
gets a card in OSF Settings' hub when it ships a schema file.

Pinned here:

  1. The OSF build polls no key and never calls Toggle(); the toggle block is
     compiled out (SFE_OSF_FRONTEND), so nothing local can race OSF.
  2. OSF is the opening path: the view is registered (hub on), the open key is
     a `key` setting declared in a native schema and dispatched by OSF UI's
     hotkey service (SubscribeHotkey), and a schema file ships so the shop is
     listed under INTERFACES in Mod Settings.
  3. The engine session follows the view: the page reports shown/hidden from
     OSF's own `views` state (plus document visibility) to one command
     endpoint, and OsfViewShown/OsfViewHidden open and close the session once
     per transition.
  4. Closing is OSF's: Escape / pad B / the page's close button. The bound
     open key also closes the view, but the page only sends `close` to OSF
     with the key OSF reports as bound, so nothing guesses whether it is up.
"""
from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OSF_SRC = ROOT / "src" / "OsfFrontend.cpp"
EXPLORER = ROOT / "src" / "ShopExplorer.cpp"
HEADER = ROOT / "src" / "ShopExplorer.h"
OVERLAY_SRC = ROOT / "src" / "ImguiOverlay.cpp"
OSF_MOD = ROOT.parent / "OSF Shop Explorer"
OSF_INI = OSF_MOD / "SFSE" / "Plugins" / "DevilzShopOSF.ini"
OSF_README = OSF_MOD / "README.txt"
OSF_APP = OSF_MOD / "SFSE" / "Plugins" / "OSFUI" / "views" / "devilzdad.osfshop" / "shop" / "app.js"
OSF_VIEW = OSF_MOD / "SFSE" / "Plugins" / "OSFUI" / "views" / "devilzdad.osfshop" / "shop"
OSF_VIEW_MODERN = OSF_MOD / "SFSE" / "Plugins" / "OSF" / "UI" / "views" / "devilzdad.osfshop"


def source(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig")


class NoLocalKeyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.osf = source(OSF_SRC)

    def test_the_osf_front_end_never_polls_the_keyboard(self) -> None:
        self.assertNotIn("GetAsyncKeyState", self.osf)
        self.assertNotIn("PollOpenKey", self.osf)
        self.assertNotIn("toggleKey", self.osf)

    def test_the_engine_toggle_is_compiled_out_of_the_osf_build(self) -> None:
        text = source(EXPLORER)
        frame = text[text.index("void ShopBrowser::FrameUpdate()"):]
        guard = frame.index("#ifndef SFE_OSF_FRONTEND")
        toggle = frame.index("Overlay::ToggleKey()", guard)
        end = frame.index("#endif", toggle)
        # The poll and the Toggle() it drives are inside the guard, so the OSF
        # build cannot open or close the engine session on its own.
        self.assertIn("Toggle();", frame[toggle:end])
        self.assertLess(guard, toggle)

    def test_tick_does_nothing_but_return_once_attached(self) -> None:
        tick = self.osf[self.osf.index("void Tick()"):]
        attached = tick[tick.index("if (IsAttached()) {"):]
        body = attached[:attached.index("}")]
        self.assertNotIn("Poll", body)


class OsfOpeningPathTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.osf = source(OSF_SRC)

    def test_the_open_key_is_declared_as_an_osf_key_setting(self) -> None:
        self.assertIn('"key": "openKey"', self.osf)
        self.assertIn('"type": "key"', self.osf)
        self.assertIn('"default": "PageUp"', self.osf)
        self.assertIn('RegisterSettingsSchema(kSchemaJson)', self.osf)

    def test_the_key_is_dispatched_by_osf_not_read_here(self) -> None:
        self.assertIn("SubscribeHotkey(kModId, kHotkeyKey", self.osf)
        self.assertIn("constexpr const char* kHotkeyKey = \"openKey\"", self.osf)

    def test_the_hotkey_opens_and_closes_the_view_through_osf(self) -> None:
        hotkey = self.osf[self.osf.index("void OpenHotkey("):]
        hotkey = hotkey[:hotkey.index("\n\t\t}")]
        self.assertIn("OsfViewIsOpen()", hotkey)
        self.assertIn("g_bridge.RequestMenu(kViewId, !open)", hotkey)

    def test_the_catalog_reply_carries_the_bound_open_key(self) -> None:
        self.assertIn(r'\",\"openKey\":\"', self.osf)
        self.assertIn("g_openKey = boundKey;", self.osf)
        app = source(OSF_APP)
        self.assertIn("state.openKey = typeof reply.openKey === 'string'", app)
        self.assertIn("event.key === state.openKey", app)

    def test_the_hotkey_refuses_to_open_before_a_player_exists(self) -> None:
        hotkey = self.osf[self.osf.index("void OpenHotkey("):]
        player = hotkey.index("RE::PlayerCharacter::GetSingleton()")
        request = hotkey.index("g_bridge.RequestMenu(")
        self.assertLess(player, request)

    def test_the_view_ships_as_a_legacy_launcher(self) -> None:
        # OSF UI 2.0 reads OSF/UI/views and OSFUI/views, but only the legacy
        # root is registered with OSF Settings as a launcher - and a launcher
        # is what puts a card under INTERFACES in the Mod Settings home screen.
        manifest = OSF_VIEW / "manifest.json"
        self.assertTrue(manifest.exists(), "ship the view in OSFUI/views")
        text = source(manifest)
        self.assertNotIn("manifestVersion", text, "a 2.0 manifest is rejected by the legacy loader")
        self.assertIn('"id": "shop"', text)
        self.assertIn('"entry": "index.html"', text)
        self.assertIn('"hub": true', text)

    def test_there_is_only_one_view_copy(self) -> None:
        # A second, modern copy is skipped with a warning and would collide on
        # the same qualified id.
        self.assertFalse(OSF_VIEW_MODERN.exists(), "drop the OSF/UI/views duplicate")

    def test_no_schema_file_for_osf_settings(self) -> None:
        # The mod card OSF Settings shows comes from the native schema this
        # plugin registers and OSF UI forwards, so a file there would only add
        # a second, competing declaration.
        schemas = OSF_MOD / "SFSE" / "Plugins" / "OSF" / "Settings" / "schemas"
        self.assertFalse((schemas / "devilzdad.osfshop.json").exists())


class ViewLifecycleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.osf = source(OSF_SRC)
        cls.explorer = source(EXPLORER)
        cls.header = source(HEADER)
        cls.app = source(OSF_APP)

    def test_the_page_reports_shown_and_hidden(self) -> None:
        # OSF UI ships two helpers, so the channel is picked at runtime: 2.0
        # calls it `state`, the 1.6 kit beside legacy views calls it `data`.
        self.assertIn("channel.on('osfui/views'", self.app)
        self.assertIn("g.state", self.app)
        self.assertIn("g.data", self.app)
        self.assertIn("send(MOD + '.view', { open: visible })", self.app)
        self.assertIn("visibilitychange", self.app)
        self.assertIn("pagehide", self.app)


    def test_that_endpoint_is_a_one_way_command_and_validates_the_sender(self) -> None:
        self.assertIn('constexpr const char* kCmdView = "devilzdad.osfshop.view"', self.osf)
        self.assertIn("RegisterCommand(kCmdView, &ViewStateCommand", self.osf)
        command = self.osf[self.osf.index("void ViewStateCommand("):]
        command = command[:command.index("\n\t\t}")]
        self.assertIn("std::string_view(a_source) != kViewId", command)
        self.assertIn("OsfViewShown()", command)
        self.assertIn("OsfViewHidden()", command)

    def test_the_session_moves_once_per_transition(self) -> None:
        shown = self.explorer[self.explorer.index("void ShopBrowser::OsfViewShown()"):]
        shown = shown[:shown.index("void ShopBrowser::OsfViewHidden()")]
        self.assertIn("open.exchange(true", shown)
        self.assertIn("OsfBeginSession();", shown)
        hidden = self.explorer[self.explorer.index("void ShopBrowser::OsfViewHidden()"):]
        hidden = hidden[:hidden.index("void ShopBrowser::OsfBeginSession()")]
        self.assertIn("open.exchange(false", hidden)
        self.assertIn("osfSessionBegun = false;", hidden)

    def test_the_catalog_is_built_once_per_view_session(self) -> None:
        begin = self.explorer[self.explorer.index("void ShopBrowser::OsfBeginSession()"):]
        begin = begin[:begin.index("\n\t}")]
        guard = begin.index("if (osfSessionBegun)")
        build = begin.index("BuildPluginList()", guard)
        self.assertLess(guard, build)

    def test_close_tears_down_audio_and_the_session_mute(self) -> None:
        hidden = self.explorer[self.explorer.index("void ShopBrowser::OsfViewHidden()"):]
        hidden = hidden[:hidden.index("void ShopBrowser::OsfBeginSession()")]
        self.assertIn("ShopAudio::PlaySfx(ShopAudio::Sfx::kClose)", hidden)
        self.assertIn("ShopAudio::StopMusic()", hidden)
        self.assertIn("ShopAudio::SetGameSessionMuted(false)", hidden)
        self.assertIn('Notify("closed")', hidden)

    def test_the_header_exposes_the_view_state(self) -> None:
        self.assertIn("void OsfViewShown();", self.header)
        self.assertIn("void OsfViewHidden();", self.header)
        self.assertIn("bool OsfViewIsOpen() const", self.header)


class ReplyTransportTests(unittest.TestCase):
    """The shop's data only exists behind the bridge, so a request the page
    cannot hear answered is an empty shop. Measured on OSF UI 2.0: the plugin
    logged "catalog answered" while every page request timed out, because the
    request/reply half never reached a legacy-root view. Answers are therefore
    also pushed as a typed event that names the request id."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.osf = source(OSF_SRC)
        cls.app = source(OSF_APP)

    def test_the_plugin_pushes_every_answer_to_the_view(self) -> None:
        self.assertIn('kEpReply = "devilzdad.osfshop.reply"', self.osf)
        self.assertIn("SendToWeb(kViewId, kEpReply", self.osf)
        # The documented reply is still sent; the push is the second carrier.
        self.assertIn("a_request.Respond(a_json)", self.osf)

    def test_every_answer_travels_through_the_push_helper(self) -> None:
        # A handler that answered with a bare Respond() would be silent on OSF UI
        # 2.0 again, so the only allowed call site is the helper itself.
        self.assertNotIn("a_request.Respond(json.c_str())", self.osf)
        self.assertEqual(self.osf.count("a_request.Respond("), 1, "only RespondWith may call Respond")

    def test_the_page_names_its_request_and_reads_the_push(self) -> None:
        self.assertIn("const REPLY_TYPE = MOD + '.reply'", self.app)
        self.assertIn("g.on(REPLY_TYPE", self.app)
        self.assertIn("requestId: id", self.app)
        self.assertIn("hookReplies();", self.app)

class SeparateModsTests(unittest.TestCase):
    """The two builds are two mods that do the same thing.

    They ship side by side and can both be enabled, so every file each build
    owns is named per build: DLL, ini, view id, and the asset/model-index
    folder. Nothing may fall back to the other build's copy.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cls.header = (ROOT / "src" / "ShopExplorer.h").read_text(encoding="utf-8-sig")
        cls.live = (ROOT / "src" / "LiveModel.cpp").read_text(encoding="utf-8-sig")
        cls.offline = (ROOT / "src" / "OfflineModel.cpp").read_text(encoding="utf-8-sig")
        cls.overlay = (ROOT / "src" / "ImguiOverlay.cpp").read_text(encoding="utf-8-sig")

    def test_each_build_owns_its_dll_ini_and_asset_folder(self) -> None:
        for name in ("kDllFileName", "kIniFileName", "kAssetsFolderName", "kAssetsModelsDir"):
            self.assertIn(name, self.header)
        blocks = re.findall(r"#ifdef SFE_OSF_FRONTEND(.*?)#else", self.header, re.DOTALL)
        self.assertGreaterEqual(len(blocks), 2, "expected the name and asset blocks")
        osf = "".join(blocks)
        self.assertIn('L"DevilzShopOSF.dll"', osf)
        self.assertIn('L"DevilzShopOSF.ini"', osf)
        self.assertIn('L"DevilzShopOSFAssets"', osf)
        self.assertNotIn("SFShopExplorer", osf)

    def test_the_model_index_is_per_build(self) -> None:
        for text in (self.live, self.offline):
            self.assertIn("L\"offlinepreview\", L\"dir\", kAssetsModelsDir", text)
            self.assertNotIn('L"SFShopExplorerAssets\\\\models"', text)

    def test_overlay_art_reads_the_builds_own_folder(self) -> None:
        self.assertIn("parent_path() / SFE::kAssetsFolderName", self.overlay)

    def test_no_translation_unit_hard_codes_the_other_builds_ini(self) -> None:
        offenders = []
        for path in (ROOT / "src").glob("*.cpp"):
            text = path.read_text(encoding="utf-8-sig")
            if 'L"SFShopExplorer.ini"' in text or 'L"DevilzShopOSF.ini"' in text:
                offenders.append(path.name)
        self.assertEqual(offenders, [], "use SFE::kIniFileName, not a literal")

    def test_capture_settings_are_per_build_too(self) -> None:
        capture = (ROOT / "src" / "ItemCapture.cpp").read_text(encoding="utf-8-sig")
        self.assertIn("parent_path() / SFE::kIniFileName", capture)


class PerBuildIniTests(unittest.TestCase):
    def test_overlay_reads_the_builds_own_ini(self) -> None:
        overlay = source(OVERLAY_SRC)
        self.assertIn("parent_path() / SFE::kIniFileName", overlay)
        settings = overlay[overlay.index("std::filesystem::path SettingsPath()"):]
        settings = settings[:settings.index("\n\t\t}")]
        self.assertNotIn('L"SFShopExplorer.ini"', settings)


class ShippedIniTests(unittest.TestCase):
    def test_the_ini_no_longer_owns_the_key(self) -> None:
        ini = OSF_INI.read_text(encoding="utf-8")
        self.assertNotRegex(ini, r"(?m)^\[overlay\]\s*$")
        self.assertNotRegex(ini, r"(?m)^toggleKey\s*=")
        self.assertIn("openKey", ini)

    def test_readme_points_at_osf_for_opening(self) -> None:
        readme = OSF_README.read_text(encoding="utf-8")
        self.assertIn("INTERFACES", readme)
        self.assertIn("Page Up", readme)
        self.assertIn("Escape", readme)
        self.assertNotIn("toggleKey=33", readme)

if __name__ == "__main__":
    unittest.main()
