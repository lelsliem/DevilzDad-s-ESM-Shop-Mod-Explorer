// ============================================================================
// OsfFrontend.cpp - the OSF UI page's other end
//
// In the OSF build the page is the interface and this file is what answers it.
// It attaches to OSF UI, registers the view's endpoints, and turns every request
// into a call on ShopBrowser - the same functions the ImGui front end uses - then
// replies with JSON. No catalog logic lives here; if a row or a price ever looks
// different between the two front ends, the bug is not in this file.
//
// Layout: escaping, JSON builders, one handler per endpoint, attach/tick.
// ============================================================================

#include "PCH.h"

#include "OsfFrontend.h"
#include "PreviewHost.h"
#include "ShopExplorer.h"
#include "ShopAudio.h"

#include "OSFUI_API.h"

#include "RE/P/PlayerCharacter.h"

#include "SFSE/SFSE.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// The OSF build's front end.
//
// The page is a WebView2 document, so every row, price and refusal has to be
// asked for over OSF UI's bridge. This file owns that conversation and nothing
// else: each answer is produced by ShopBrowser, which is the same engine the
// ImGui overlay uses, so the two front ends cannot show different catalogs.
//
// Registered endpoints (the page calls them by these exact names):
//   devilzdad.osfshop.catalog    categories, plugins, basket, filter
//   devilzdad.osfshop.rows       one page of the filtered catalog
//   devilzdad.osfshop.basket     add / set / remove / clear
//   devilzdad.osfshop.accept     deliver the basket (free or paid)
//   devilzdad.osfshop.detail     one item, plus its parts and rarity
//   devilzdad.osfshop.configure  choose a part, a quality or a rarity
//   devilzdad.osfshop.refresh    rebuild the plugin and form lists
//   devilzdad.osfshop.preview    render one item and return the PNG's URL
//   devilzdad.osfshop.audio      music/sound state, and setting it
//   devilzdad.osfshop.themes     the nine themes, their palettes and lock state
//   devilzdad.osfshop.themepick  switch theme (refused while locked)
//   devilzdad.osfshop.favourite  star or unstar one record (the overlay's list)

namespace SFE::Osf
{
	namespace
	{
		constexpr const char* kModId = "devilzdad.osfshop";
		constexpr const char* kViewId = "devilzdad.osfshop/shop";

		constexpr const char* kEpCatalog = "devilzdad.osfshop.catalog";
		constexpr const char* kEpRows = "devilzdad.osfshop.rows";
		constexpr const char* kEpBasket = "devilzdad.osfshop.basket";
		constexpr const char* kEpAccept = "devilzdad.osfshop.accept";
		constexpr const char* kEpDetail = "devilzdad.osfshop.detail";
		constexpr const char* kEpConfigure = "devilzdad.osfshop.configure";
		constexpr const char* kEpRefresh = "devilzdad.osfshop.refresh";
		constexpr const char* kEpPreview = "devilzdad.osfshop.preview";
		constexpr const char* kEpAudio = "devilzdad.osfshop.audio";
		constexpr const char* kEpThemes = "devilzdad.osfshop.themes";
		constexpr const char* kEpThemePick = "devilzdad.osfshop.themepick";
		constexpr const char* kEpFavourite = "devilzdad.osfshop.favourite";
		// The page's own words. A WebView cannot write a log file, so a failure
		// there is invisible from outside: it reports into this one-way endpoint
		// instead and the line lands next to the rest of the session.
		constexpr const char* kEpNote = "devilzdad.osfshop.note";

		// Answers ride two ways on purpose. The frozen request/reply pair
		// (Request::Respond) is the documented one, but OSF UI 2.0 hosts this
		// legacy-root view and, measured in the field, that answer never reached
		// the page: the plugin logged "catalog answered" while the page timed out
		// after 10 s. A typed push is what OSF UI's own shipped views consume
		// (SendToWeb -> osfui.on), so every answer is also sent as an event that
		// carries the request id. The page settles on the first one that names
		// its id; a duplicate is dropped there, so sending both is free.
		constexpr const char* kEpReply = "devilzdad.osfshop.reply";

		OSFUI::API::Client g_bridge;

		// ------------------------------------------------------- view lifecycle --
		//
		// OSF owns the shop's front door. The view is registered with `hub` on, so
		// it is an interface in OSF's own menu (Mod Settings -> Interfaces), and
		// the open key is a `key` setting in the schema this mod ships: OSF
		// Settings lists it under Keybindings, dispatches it through its hotkey
		// service (rebindable, and never fired while typing or while a menu is
		// up), and the player can unbind it. Nothing here polls the keyboard.
		//
		// The engine session then follows the view. The page watches OSF's own
		// `views` state and reports shown/hidden on this endpoint, so one press
		// opens the shop, Escape / the page's close button / pad B close it, and
		// the music, the game-session mute and the `open` flag all move with the
		// screen instead of with a second, independent key.
		constexpr const char* kCmdView = "devilzdad.osfshop.view";
		constexpr const char* kCmdClose = "devilzdad.osfshop.close";
		constexpr const char* kHotkeyKey = "openKey";

		// The binding itself, declared to OSF UI at attach time: a `key` setting
		// in a native schema is what its hotkey service binds, so the key exists
		// even when no settings file was installed. Page Up is the key the
		// standalone build and the Nexus page use.
		constexpr const char* kSchemaJson = R"({
  "id": "devilzdad.osfshop",
  "title": "DevilzDad's ESM Shop + Explorer",
  "description": "In-game shop and mod explorer for every loaded plugin.",
  "version": 1,
  "groups": [
    {
      "id": "shop",
      "label": "Shop",
      "settings": [
        {
          "key": "openKey",
          "label": "Open the shop",
          "type": "key",
          "default": "PageUp",
          "allowUnbound": true,
          "hint": "Opens the shop view. Escape, pad B or the page's close button closes it."
        }
      ]
    }
  ]
})";

		std::uint32_t g_hotkeyToken{ 0 };
		// The player's bound open key, as OSF names it. The page closes on the
		// same key, so this is set at attach and sent on every catalog reply.
		std::string g_openKey;

		std::atomic_bool g_attached{ false };
		bool g_session = false;
		std::mutex g_mutex;

		// The last filter the page asked for. Rows requests carry it, so the form
		// list is rebuilt only when a value actually changed - a page turn must
		// not re-scan every record in the load order.
		struct FilterState
		{
			int  type{ 8 };   // BrowseType::kAllTypes
			int  sub{ 0 };
			int  plugin{ -1 };
			bool hideTest{ true };
			bool hideHair{ true };
			bool hideCrew{ true };
			bool hideTurrets{ true };
			bool hideBug{ true };
			bool favouritesOnly{ false };
			bool valid{ false };
		};
		FilterState g_filter;

		// The page's sort, kept in the same INI the page's audio and theme use.
		// The overlay has persisted `[shop] sortColumn` for its own headers since
		// 0.9.0; the page had no memory of it at all, so every open dropped back
		// to catalogue order. Distinct keys on purpose: the overlay writes its
		// column 1-based and the page's numbering includes the Shelf column, so
		// one key for both would be a meaning change, not a shared setting.
		struct SortState
		{
			int  column{ -1 };      // -1 catalogue order, else the page's 0-5
			bool ascending{ true };
			bool loaded{ false };
		};
		SortState g_sort;

		void LoadSort()
		{
			if (g_sort.loaded) {
				return;
			}
			g_sort.loaded = true;
			g_sort.column = ShopBrowser::OsfStoredSortColumn();
			g_sort.ascending = ShopBrowser::OsfStoredSortAscending();
			if (g_sort.column >= 0) {
				REX::INFO("OSF shop: [shop] osfSortColumn={} osfSortAscending={} restored",
					g_sort.column, g_sort.ascending ? 1 : 0);
			}
		}

		void SaveSort(const int a_column, const bool a_ascending)
		{
			LoadSort();
			if (g_sort.column == a_column && g_sort.ascending == a_ascending) {
				return;
			}
			g_sort.column = a_column;
			g_sort.ascending = a_ascending;
			// Written on the click, like the page's audio and theme settings: a
			// crash or a force-close must not lose the choice.
			ShopBrowser::OsfStoreSort(a_column, a_ascending);
			REX::INFO("OSF shop: sort saved - column {} {}", a_column, a_ascending ? "asc" : "desc");
		}

		std::string SortJson()
		{
			LoadSort();
			return std::format("\"sortColumn\":{},\"sortAscending\":{}",
				g_sort.column, g_sort.ascending ? "true" : "false");
		}

		// cp1252 bytes 0x80-0x9F, which are the punctuation Windows uses where
		// ISO-8859-1 has control codes. 0 means "undefined in cp1252".
		std::uint32_t Cp1252Punct(const unsigned char a_byte)
		{
			switch (a_byte) {
			case 0x80: return 0x20AC;  // euro
			case 0x82: return 0x201A;
			case 0x83: return 0x0192;
			case 0x84: return 0x201E;
			case 0x85: return 0x2026;
			case 0x86: return 0x2020;
			case 0x87: return 0x2021;
			case 0x88: return 0x02C6;
			case 0x89: return 0x2030;
			case 0x8A: return 0x0160;
			case 0x8B: return 0x2039;
			case 0x8C: return 0x0152;
			case 0x8E: return 0x017D;
			case 0x91: return 0x2018;
			case 0x92: return 0x2019;
			case 0x93: return 0x201C;
			case 0x94: return 0x201D;
			case 0x95: return 0x2022;
			case 0x96: return 0x2013;
			case 0x97: return 0x2014;
			case 0x98: return 0x02DC;
			case 0x99: return 0x2122;
			case 0x9A: return 0x0161;
			case 0x9B: return 0x203A;
			case 0x9C: return 0x0153;
			case 0x9E: return 0x017E;
			case 0x9F: return 0x0178;
			default: return 0;
			}
		}

		// A JSON string has to be UTF-8, and Starfield's string tables hand back
		// Windows-1252: an accented name is one 0xE9, not the two-byte UTF-8 form.
		// Copying those bytes through made the whole reply invalid UTF-8, and the
		// OSF bridge rejected it as "legacy handler returned invalid JSON" - which
		// is why a NAME or TYPE sort failed while VALUE and WEIGHT worked: only
		// those two pages happened to carry such a record (an Aid name with an
		// accent sorts first in TYPE order, an "Action Figure: ... Francois ..."
		// in NAME order). Convert first, then escape.
		std::string Esc(std::string_view a_text)
		{
			std::string out;
			out.reserve(a_text.size() + 16);
			const auto push = [&out](const std::uint32_t a_cp) {
				if (a_cp < 0x80) {
					out += static_cast<char>(a_cp);
				} else if (a_cp < 0x800) {
					out += static_cast<char>(0xC0 | (a_cp >> 6));
					out += static_cast<char>(0x80 | (a_cp & 0x3F));
				} else {
					out += static_cast<char>(0xE0 | (a_cp >> 12));
					out += static_cast<char>(0x80 | ((a_cp >> 6) & 0x3F));
					out += static_cast<char>(0x80 | (a_cp & 0x3F));
				}
			};
			for (std::size_t i = 0; i < a_text.size();) {
				const auto c = static_cast<unsigned char>(a_text[i]);
				if (c < 0x80) {
					switch (c) {
					case '"': out += "\\\""; break;
					case '\\': out += "\\\\"; break;
					case '\n': out += "\\n"; break;
					case '\r': out += "\\r"; break;
					case '\t': out += "\\t"; break;
					default:
						if (c < 0x20) {
							out += std::format("\\u{:04x}", c);
						} else {
							out += static_cast<char>(c);
						}
						break;
					}
					++i;
					continue;
				}
				// Already valid UTF-8: the bytes are kept as they are, so a record
				// that does carry proper UTF-8 keeps its own encoding.
				if (c >= 0xC2 && c <= 0xF4) {
					const std::size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
					bool valid = i + len <= a_text.size();
					for (std::size_t k = 1; valid && k < len; ++k) {
						valid = (static_cast<unsigned char>(a_text[i + k]) & 0xC0) == 0x80;
					}
					if (valid) {
						out.append(a_text.substr(i, len));
						i += len;
						continue;
					}
				}
				if (c >= 0xA0) {
					push(c);
				} else if (const auto cp = Cp1252Punct(c); cp != 0) {
					push(cp);
				} else {
					out += '?';
				}
				++i;
			}
			return out;
		}

		std::string Unescape(std::string_view a_text)
		{
			std::string out;
			out.reserve(a_text.size());
			for (std::size_t i = 0; i < a_text.size(); ++i) {
				if (a_text[i] != '\\' || i + 1 >= a_text.size()) {
					out += a_text[i];
					continue;
				}
				switch (a_text[++i]) {
				case 'n': out += '\n'; break;
				case 'r': out += '\r'; break;
				case 't': out += '\t'; break;
				case '"': out += '"'; break;
				case '\\': out += '\\'; break;
				case '/': out += '/'; break;
				case 'u':
					// Only our own ASCII search text is read back; a real escape
					// here would be a name the page never sends, so a placeholder
					// is honest rather than silently wrong.
					i += 4;
					out += '?';
					break;
				default: out += a_text[i]; break;
				}
			}
			return out;
		}

		// Flat payload reader. The payloads are ours: one object, no nesting, so
		// a scan for "key": is exact and a JSON library would only add weight to
		// a plugin that already carries a renderer.
		struct JsonIn
		{
			std::string_view text;

			[[nodiscard]] std::string_view Raw(const char* a_key) const
			{
				const std::string needle = std::format("\"{}\":", a_key);
				const auto pos = text.find(needle);
				if (pos == std::string_view::npos) {
					return {};
				}
				auto cur = pos + needle.size();
				while (cur < text.size() && text[cur] == ' ') {
					++cur;
				}
				if (cur >= text.size()) {
					return {};
				}
				if (text[cur] == '"') {
					const auto start = cur + 1;
					auto end = start;
					bool escaped = false;
					for (; end < text.size(); ++end) {
						const char c = text[end];
						if (escaped) {
							escaped = false;
							continue;
						}
						if (c == '\\') {
							escaped = true;
							continue;
						}
						if (c == '"') {
							break;
						}
					}
					return text.substr(start, end - start);
				}
				auto end = cur;
				while (end < text.size() && text[end] != ',' && text[end] != '}' && text[end] != ']') {
					++end;
				}
				while (end > cur && text[end - 1] == ' ') {
					--end;
				}
				return text.substr(cur, end - cur);
			}

			[[nodiscard]] bool Has(const char* a_key) const
			{
				const std::string needle = std::format("\"{}\":", a_key);
				return text.find(needle) != std::string_view::npos;
			}

			[[nodiscard]] std::string Str(const char* a_key, std::string a_default = {}) const
			{
				const auto raw = Raw(a_key);
				if (raw.empty()) {
					return a_default;
				}
				return Unescape(raw);
			}

			[[nodiscard]] long long Int(const char* a_key, const long long a_default) const
			{
				const auto raw = Raw(a_key);
				if (raw.empty() || raw == "null") {
					return a_default;
				}
				return std::strtoll(std::string(raw).c_str(), nullptr, 10);
			}

			[[nodiscard]] bool Bool(const char* a_key, const bool a_default) const
			{
				const auto raw = Raw(a_key);
				if (raw.empty()) {
					return a_default;
				}
				if (raw == "true") {
					return true;
				}
				if (raw == "false") {
					return false;
				}
				return std::strtoll(std::string(raw).c_str(), nullptr, 10) != 0;
			}

			// FormIDs travel as 8-digit hex strings, which is also how the page
			// shows them; a bare number is accepted for hand-written callers.
			[[nodiscard]] std::uint32_t FormId(const char* a_key) const
			{
				const auto raw = Raw(a_key);
				if (raw.empty() || raw == "null") {
					return 0;
				}
				return static_cast<std::uint32_t>(std::strtoul(std::string(raw).c_str(), nullptr, 16));
			}
		};

		// Shown/hidden, as the page observed it. A one-way send, not a request:
		// there is no answer to give and the page must never wait for one.
		void NoteCommand(const char*, const char* a_payload, const char* a_source, void*) noexcept
		{
			try {
				// Only this view may speak for this mod.
				if (a_source && *a_source && std::string_view(a_source) != kViewId) {
					return;
				}
				const JsonIn in{ a_payload ? a_payload : "" };
				std::string text = std::string(in.Str("text"));
				if (text.size() > 400) {
					text.resize(400);
				}
				for (auto& ch : text) {
					if (ch == '\n' || ch == '\r') {
						ch = ' ';
					}
				}
				if (!text.empty()) {
					REX::WARN("OSF shop page: {}", text);
				}
			} catch (...) {
				// A diagnostic must never take the page down with it.
			}
		}

		void ViewStateCommand(const char*, const char* a_payload, const char* a_source, void*) noexcept
		{
			try {
				// Only this view may report this view.
				if (a_source && *a_source && std::string_view(a_source) != kViewId) {
					return;
				}
				const JsonIn in{ a_payload ? a_payload : "" };
				if (!in.Has("open")) {
					return;
				}
				const bool open = in.Bool("open", false);
				// The engine's own flag is the one truth: OSF replays state on every
				// greeting and a hidden document reports twice, and neither is a
				// transition. The transition methods are idempotent for the same
				// reason, so a duplicated report costs nothing either way.
				auto* shop = ShopBrowser::GetSingleton();
				if (open == shop->OsfViewIsOpen()) {
					REX::DEBUG("OSF shop: view report '{}' repeats the session state", open ? "shown" : "hidden");
					return;
				}
				if (open) {
					REX::INFO("OSF shop: view shown; opening the session");
					shop->OsfViewShown();
				} else {
					REX::INFO("OSF shop: view hidden; closing the session");
					shop->OsfViewHidden();
				}
			} catch (const std::exception& e) {
				REX::ERROR("OSF shop: view state failed: {}", e.what());
			} catch (...) {
				REX::ERROR("OSF shop: view state failed");
			}
		}

		void CloseCommand(const char*, const char*, const char* a_source, void*) noexcept
		{
			try {
				if (a_source && *a_source && std::string_view(a_source) != kViewId) {
					return;
				}
				if (!ShopBrowser::GetSingleton()->OsfViewIsOpen()) {
					return;
				}
				if (!g_bridge.RequestMenu(kViewId, false)) {
					REX::WARN("OSF shop: close request refused by OSF UI for '{}'", kViewId);
				}
			} catch (...) {
				REX::ERROR("OSF shop: close command failed");
			}
		}

		// OSF's hotkey service delivers this on the main thread, the same thread
		// its menu is presented from. It fires during gameplay only (OSF drops it
		// while a menu is up), so the open half is the only one that matters; the
		// close half is kept for OSF builds that deliver it anyway.
		void OpenHotkey(const char*, const char* a_key, void*) noexcept
		{
			try {
				// Main menu and the loading transition have no player yet; opening
				// the catalog there would read a null singleton for credits.
				if (!RE::PlayerCharacter::GetSingleton()) {
					REX::DEBUG("OSF shop: hotkey '{}' ignored before a player exists", a_key ? a_key : "");
					return;
				}
				const bool open = ShopBrowser::GetSingleton()->OsfViewIsOpen();
				if (g_bridge.RequestMenu(kViewId, !open)) {
					REX::INFO("OSF shop: hotkey '{}' asked OSF UI to {} view '{}'", a_key ? a_key : "",
						open ? "close" : "open", kViewId);
				} else {
					REX::WARN("OSF shop: hotkey '{}' pressed, but OSF UI refused '{}' (another OSF menu is up, or the view was not discovered)",
						a_key ? a_key : "", kViewId);
				}
			} catch (...) {
				REX::ERROR("OSF shop: hotkey handler failed");
			}
		}


		// -----------------------------------------------------------------------
// 		JSON builders: rows, options, filters, basket, state
		// -----------------------------------------------------------------------
		std::string RowJson(const OsfItemRow& a_row)
		{
			return std::format("{{\"formId\":\"{:08X}\",\"name\":\"{}\",\"edid\":\"{}\",\"type\":\"{}\",\"sub\":\"{}\","
							   "\"plugin\":\"{}\",\"value\":{},\"weight\":{:.1f},\"favourite\":{}}}",
				a_row.formID, Esc(a_row.name), Esc(a_row.edid), Esc(a_row.type), Esc(a_row.sub),
				Esc(a_row.plugin), a_row.value, static_cast<double>(a_row.weight),
				a_row.favourite ? "true" : "false");
		}

		std::string OptionJson(const OsfOption& a_option)
		{
			return std::format("{{\"slot\":\"{}\",\"label\":\"{}\",\"fid\":\"{:08X}\",\"selected\":{},\"default\":{}}}",
				Esc(a_option.slot), Esc(a_option.label), a_option.fid,
				a_option.selected ? "true" : "false", a_option.isDefault ? "true" : "false");
		}

		template <class Row>
		void AppendOptions(std::string& a_json, const std::vector<Row>& a_options)
		{
			a_json += '[';
			for (std::size_t i = 0; i < a_options.size(); ++i) {
				if (i) {
					a_json += ',';
				}
				a_json += OptionJson(a_options[i]);
			}
			a_json += ']';
		}

		std::string FilterJson()
		{
			return std::format("{{\"type\":{},\"sub\":{},\"plugin\":{},\"hideTest\":{},\"hideHair\":{},\"hideCrew\":{},"
							   "\"hideTurrets\":{},\"hideBug\":{},\"favouritesOnly\":{}}}",
				g_filter.type, g_filter.sub, g_filter.plugin,
				g_filter.hideTest ? "true" : "false", g_filter.hideHair ? "true" : "false",
				g_filter.hideCrew ? "true" : "false", g_filter.hideTurrets ? "true" : "false",
				g_filter.hideBug ? "true" : "false", g_filter.favouritesOnly ? "true" : "false");
		}

		std::string BasketJson()
		{
			const auto rows = ShopBrowser::GetSingleton()->OsfBasket();
			std::string out = "[";
			for (std::size_t i = 0; i < rows.size(); ++i) {
				if (i) {
					out += ',';
				}
				out += std::format("{{\"formId\":\"{:08X}\",\"name\":\"{}\",\"sub\":\"{}\",\"count\":{},\"unitPrice\":{},\"weight\":{:.2f},\"omods\":{}}}",
					rows[i].formID, Esc(rows[i].name), Esc(rows[i].sub), rows[i].count, rows[i].unitPrice,
					static_cast<double>(rows[i].weight), rows[i].omods);
			}
			out += ']';
			return out;
		}

		// Pushes one answer to the page, tagged with the id the page chose. The
		// id travels in the request payload because the frozen Request struct
		// exposes no request id to a plugin.
		void PushReply(const OSFUI::API::Request& a_request, const char* a_json) noexcept
		{
			try {
				const JsonIn in{ a_request.payloadJson ? a_request.payloadJson : "" };
				const auto id = in.Str("requestId");
				if (id.empty()) {
					return;
				}
				std::string push = "{\"id\":\"";
				push += Esc(id);
				push += "\",\"payload\":";
				push += a_json;
				push += '}';
				// The first accepted push is logged once. A refusal is logged for
				// the first one and then every 250th: a preview frame is the
				// biggest message this mod sends, so a size refusal would show up
				// here as a run of them rather than as a page stuck on "no
				// preview".
				static bool pushed = false;
				static std::size_t refused = 0;
				if (g_bridge.SendToWeb(kViewId, kEpReply, push.c_str())) {
					if (!pushed) {
						pushed = true;
						REX::INFO("OSF shop: answers are pushed to view '{}' (first: request '{}', {} bytes)", kViewId, id, push.size());
					}
				} else {
					++refused;
					if (refused == 1 || refused % 250 == 0) {
						REX::WARN("OSF shop: OSF UI refused the reply push to '{}' (request '{}', {} bytes, {} refused so far)",
							kViewId, id, push.size(), refused);
					}
				}
			} catch (...) {
				// A reply must never take the game thread down.
			}
		}

		// Answers one request: the documented reply, plus the push the page
		// actually reads. A duplicate is dropped by id on the page, so both are
		// cheap.
		void RespondWith(const OSFUI::API::Request& a_request, const char* a_json) noexcept
		{
			a_request.Respond(a_json);
			PushReply(a_request, a_json);
		}

		void RejectWith(const OSFUI::API::Request& a_request, const char* a_code, const char* a_message) noexcept
		{
			try {
				a_request.Reject(a_code, a_message ? a_message : "");
				std::string json = "{\"ok\":false,\"code\":\"";
				json += Esc(a_code ? a_code : "shop-error");
				json += "\",\"error\":\"";
				json += Esc(a_message ? a_message : "request failed");
				json += "\"}";
				PushReply(a_request, json.c_str());
			} catch (...) {
				// Same rule as the reply: never throw out of a handler.
			}
		}

		std::string StateJson(const bool a_free)
		{
			std::string out = "\"credits\":";
			out += std::to_string(ShopBrowser::GetSingleton()->OsfCredits());
			out += ",\"cost\":";
			out += std::to_string(ShopBrowser::GetSingleton()->OsfBasketCost(a_free));
			out += ",\"basket\":";
			out += BasketJson();
			// Every answer carries the star list: the page's toggle label, the
			// row stars and the overlay's star column all read this one array.
			out += ",\"favourites\":[";
			const auto favourites = ShopBrowser::GetSingleton()->OsfFavourites();
			for (std::size_t i = 0; i < favourites.size(); ++i) {
				if (i) {
					out += ',';
				}
				out += std::format("\"{:08X}\"", favourites[i]);
			}
			out += ']';
			return out;
		}

		void EnsureSession()
		{
			if (g_session) {
				return;
			}
			ShopBrowser::GetSingleton()->OsfBeginSession();
			g_filter = FilterState{};
			g_session = true;
		}

		std::string CategoriesJson()
		{
			std::string out = "[";
			bool first = true;
			for (const auto& category : ShopBrowser::OsfCategoryTree()) {
				if (!first) {
					out += ',';
				}
				first = false;
				out += std::format("{{\"index\":{},\"name\":\"{}\",\"subs\":[", category.index, Esc(category.name));
				for (std::size_t i = 0; i < category.subs.size(); ++i) {
					if (i) {
						out += ',';
					}
					out += '"';
					out += Esc(category.subs[i]);
					out += '"';
				}
				out += "]}";
			}
			out += ']';
			return out;
		}

		std::string BuildCatalogJson(const bool a_free)
		{
			std::string json;
			json.reserve(8192);
			json += "{\"ok\":true,";
			// The page's stored sort travels with the catalog, so the first
			// reply after an open is enough to put the list back the way it was
			// left. It is only adopted on the page's first reply; a later
			// re-sync must not undo a click the player just made.
			json += SortJson();
			json += ",\"version\":\"";
			json += Esc(kVersionOsf);
			json += "\",\"view\":\"";
			json += kViewId;
			json += "\",\"openKey\":\"";
			json += Esc(g_openKey);
			json += "\",\"categories\":";
			json += CategoriesJson();
			json += ",\"plugins\":[";
			const auto plugins = ShopBrowser::GetSingleton()->OsfPlugins();
			for (std::size_t i = 0; i < plugins.size(); ++i) {
				if (i) {
					json += ',';
				}
				json += std::format("{{\"name\":\"{}\",\"kind\":\"{}\"}}", Esc(plugins[i].name), Esc(plugins[i].kind));
			}
			json += "],\"filter\":";
			json += FilterJson();
			json += ',';
			json += StateJson(a_free);
			json += ",\"total\":";
			{
				std::vector<OsfItemRow> none;
				json += std::to_string(ShopBrowser::GetSingleton()->OsfPage("", 0, 0, -1, true, none));
			}
			json += '}';
			return json;
		}

		// Applies the filter the page sent, if it differs from the one the form
		// list was built with. A page turn repeats the same filter, so this is
		// usually a comparison and nothing more.
		void ApplyFilter(const JsonIn& a_in)
		{
			FilterState next = g_filter;
			if (a_in.Has("type")) next.type = static_cast<int>(a_in.Int("type", next.type));
			if (a_in.Has("sub")) next.sub = static_cast<int>(a_in.Int("sub", next.sub));
			if (a_in.Has("plugin")) next.plugin = static_cast<int>(a_in.Int("plugin", next.plugin));
			if (a_in.Has("hideTest")) next.hideTest = a_in.Bool("hideTest", next.hideTest);
			if (a_in.Has("hideHair")) next.hideHair = a_in.Bool("hideHair", next.hideHair);
			if (a_in.Has("hideCrew")) next.hideCrew = a_in.Bool("hideCrew", next.hideCrew);
			if (a_in.Has("hideTurrets")) next.hideTurrets = a_in.Bool("hideTurrets", next.hideTurrets);
			if (a_in.Has("hideBug")) next.hideBug = a_in.Bool("hideBug", next.hideBug);
			if (a_in.Has("favouritesOnly")) next.favouritesOnly = a_in.Bool("favouritesOnly", next.favouritesOnly);
			const bool changed = !g_filter.valid || next.type != g_filter.type || next.sub != g_filter.sub ||
				next.plugin != g_filter.plugin || next.hideTest != g_filter.hideTest || next.hideHair != g_filter.hideHair ||
				next.hideCrew != g_filter.hideCrew || next.hideTurrets != g_filter.hideTurrets || next.hideBug != g_filter.hideBug ||
				next.favouritesOnly != g_filter.favouritesOnly;
			if (!changed) {
				return;
			}
			next.valid = true;
			ShopBrowser::GetSingleton()->OsfSetFilter(next.type, next.sub, next.plugin, next.hideTest, next.hideHair,
				next.hideCrew, next.hideTurrets, next.hideBug, next.favouritesOnly);
			g_filter = next;
		}

		std::string BuildRowsJson(const std::string& a_search, const std::size_t a_offset, const std::size_t a_limit,
			const int a_sortColumn, const bool a_sortAscending, std::size_t& a_total)
		{
			std::vector<OsfItemRow> rows;
			a_total = ShopBrowser::GetSingleton()->OsfPage(a_search, a_offset, a_limit, a_sortColumn,
				a_sortAscending, rows);
			std::string json;
			json.reserve(rows.size() * 220 + 96);
			json += "{\"ok\":true,\"total\":";
			json += std::to_string(a_total);
			json += ",\"offset\":";
			json += std::to_string(a_offset);
			json += ",\"rows\":[";
			for (std::size_t i = 0; i < rows.size(); ++i) {
				if (i) {
					json += ',';
				}
				json += RowJson(rows[i]);
			}
			json += "],";
			json += StateJson(true);
			json += '}';
			return json;
		}

		std::string DetailJson(const OsfDetail& a_detail)
		{
			if (!a_detail.ok) {
				return "{\"ok\":false}";
			}
			std::string json;
			json.reserve(4096);
			json += "{\"ok\":true,\"item\":";
			json += RowJson(a_detail.row);
			json += ",\"formType\":\"";
			json += Esc(a_detail.formType);
			json += "\",\"model\":\"";
			json += Esc(a_detail.model);
			json += "\",\"keywords\":\"";
			json += Esc(a_detail.keywords);
			json += "\",\"mods\":";
			AppendOptions(json, a_detail.mods);
			json += ",\"qualities\":";
			AppendOptions(json, a_detail.qualities);
			json += ",\"rarities\":";
			AppendOptions(json, a_detail.rarities);
			json += ",\"rarityNote\":\"";
			json += Esc(a_detail.rarityNote);
			json += "\"}";
			return json;
		}


		// -----------------------------------------------------------------------
// 		Request handlers, one per page endpoint
		// -----------------------------------------------------------------------
		void CatalogRequest(const OSFUI::API::Request& a_request, void*) noexcept
		{
			try {
				std::scoped_lock lock(g_mutex);
				EnsureSession();
				const JsonIn in{ a_request.payloadJson ? a_request.payloadJson : "" };
				const auto json = BuildCatalogJson(in.Bool("free", true));
				RespondWith(a_request, json.c_str());
				REX::INFO("OSF shop: catalog answered ({} plugins, {} bytes)",
					ShopBrowser::GetSingleton()->OsfPlugins().size(), json.size());
			} catch (const std::exception& e) {
				REX::ERROR("OSF shop: catalog failed: {}", e.what());
				RejectWith(a_request, "shop-error", e.what());
			} catch (...) {
				RejectWith(a_request, "shop-error", "catalog failed");
			}
		}

		void RefreshRequest(const OSFUI::API::Request& a_request, void*) noexcept
		{
			try {
				std::scoped_lock lock(g_mutex);
				ShopBrowser::GetSingleton()->OsfRefresh();
				g_session = true;
				g_filter = FilterState{};
				const JsonIn in{ a_request.payloadJson ? a_request.payloadJson : "" };
				const auto json = BuildCatalogJson(in.Bool("free", true));
				RespondWith(a_request, json.c_str());
				REX::INFO("OSF shop: refresh answered ({} plugins, {} bytes)",
					ShopBrowser::GetSingleton()->OsfPlugins().size(), json.size());
			} catch (const std::exception& e) {
				REX::ERROR("OSF shop: refresh failed: {}", e.what());
				RejectWith(a_request, "shop-error", e.what());
			} catch (...) {
				RejectWith(a_request, "shop-error", "refresh failed");
			}
		}

		void RowsRequest(const OSFUI::API::Request& a_request, void*) noexcept
		{
			try {
				std::scoped_lock lock(g_mutex);
				EnsureSession();
				const JsonIn in{ a_request.payloadJson ? a_request.payloadJson : "" };
				ApplyFilter(in);
				const auto offset = static_cast<std::size_t>(std::max<long long>(0, in.Int("offset", 0)));
				const auto limit = static_cast<std::size_t>(std::clamp<long long>(in.Int("limit", 100), 1, 200));
				// The header the page clicked: 0 name, 1 shelf, 2 type, 3 value,
				// 4 weight, 5 FormID; -1 (or absent) keeps catalogue order.
				const auto sort = static_cast<int>(std::clamp<long long>(in.Int("sort", -1), -1, 5));
				const auto ascending = in.Bool("asc", true);
				SaveSort(sort, ascending);
				std::size_t total = 0;
				const auto json = BuildRowsJson(in.Str("search"), offset, limit, sort, ascending, total);
				RespondWith(a_request, json.c_str());
			} catch (const std::exception& e) {
				REX::ERROR("OSF shop: rows failed: {}", e.what());
				RejectWith(a_request, "shop-error", e.what());
			} catch (...) {
				RejectWith(a_request, "shop-error", "rows failed");
			}
		}

		void BasketRequest(const OSFUI::API::Request& a_request, void*) noexcept
		{
			try {
				std::scoped_lock lock(g_mutex);
				EnsureSession();
				const JsonIn in{ a_request.payloadJson ? a_request.payloadJson : "" };
				const auto op = in.Str("op");
				const auto free = in.Bool("free", true);
				auto* shop = ShopBrowser::GetSingleton();
				std::string error;
				bool ok = true;
				if (op == "add") {
					ok = shop->OsfBasketAdd(in.FormId("formId"), static_cast<std::int32_t>(in.Int("count", 1)), error);
				} else if (op == "set") {
					ok = shop->OsfBasketSet(in.FormId("formId"), static_cast<std::int32_t>(in.Int("count", 1)));
					if (!ok) {
						error = "that line is no longer in the basket";
					}
				} else if (op == "remove") {
					ok = shop->OsfBasketRemove(static_cast<std::size_t>(std::max<long long>(0, in.Int("index", -1))));
					if (!ok) {
						error = "that line is no longer in the basket";
					}
				} else if (op == "clear") {
					shop->OsfBasketClear();
				} else {
					ok = false;
					error = "unknown basket operation";
				}
				std::string json = "{\"ok\":";
				json += ok ? "true" : "false";
				json += ",\"error\":\"";
				json += Esc(error);
				json += "\",";
				json += StateJson(free);
				json += '}';
				RespondWith(a_request, json.c_str());
			} catch (const std::exception& e) {
				REX::ERROR("OSF shop: basket failed: {}", e.what());
				RejectWith(a_request, "shop-error", e.what());
			} catch (...) {
				RejectWith(a_request, "shop-error", "basket failed");
			}
		}

		void FavouriteRequest(const OSFUI::API::Request& a_request, void*) noexcept
		{
			try {
				std::scoped_lock lock(g_mutex);
				EnsureSession();
				const JsonIn in{ a_request.payloadJson ? a_request.payloadJson : "" };
				bool now = false;
				const bool ok = ShopBrowser::GetSingleton()->OsfToggleFavourite(in.FormId("formId"), now);
				std::string json = "{\"ok\":";
				json += ok ? "true" : "false";
				json += ",\"favourite\":";
				json += now ? "true" : "false";
				if (!ok) {
					json += ",\"error\":\"that record is not something the shop can star\"";
				}
				json += ',';
				json += StateJson(in.Bool("free", true));
				json += '}';
				RespondWith(a_request, json.c_str());
			} catch (const std::exception& e) {
				REX::ERROR("OSF shop: favourite failed: {}", e.what());
				RejectWith(a_request, "shop-error", e.what());
			} catch (...) {
				RejectWith(a_request, "shop-error", "favourite failed");
			}
		}

		void AcceptRequest(const OSFUI::API::Request& a_request, void*) noexcept
		{
			try {
				std::scoped_lock lock(g_mutex);
				EnsureSession();
				const JsonIn in{ a_request.payloadJson ? a_request.payloadJson : "" };
				const auto free = in.Bool("free", true);
				std::vector<std::string> lines;
				const auto ok = ShopBrowser::GetSingleton()->OsfAccept(free, lines);
				std::string json = "{\"ok\":";
				json += ok ? "true" : "false";
				json += ",\"lines\":[";
				for (std::size_t i = 0; i < lines.size(); ++i) {
					if (i) {
						json += ',';
					}
					json += '"';
					json += Esc(lines[i]);
					json += '"';
				}
				json += "],";
				json += StateJson(free);
				json += '}';
				RespondWith(a_request, json.c_str());
			} catch (const std::exception& e) {
				REX::ERROR("OSF shop: accept failed: {}", e.what());
				RejectWith(a_request, "shop-error", e.what());
			} catch (...) {
				RejectWith(a_request, "shop-error", "accept failed");
			}
		}

		void DetailRequest(const OSFUI::API::Request& a_request, void*) noexcept
		{
			try {
				std::scoped_lock lock(g_mutex);
				EnsureSession();
				const JsonIn in{ a_request.payloadJson ? a_request.payloadJson : "" };
				const auto detail = ShopBrowser::GetSingleton()->OsfDetailOf(in.FormId("formId"));
				const auto json = DetailJson(detail);
				RespondWith(a_request, json.c_str());
			} catch (const std::exception& e) {
				REX::ERROR("OSF shop: detail failed: {}", e.what());
				RejectWith(a_request, "shop-error", e.what());
			} catch (...) {
				RejectWith(a_request, "shop-error", "detail failed");
			}
		}

		void ConfigureRequest(const OSFUI::API::Request& a_request, void*) noexcept
		{
			try {
				std::scoped_lock lock(g_mutex);
				EnsureSession();
				const JsonIn in{ a_request.payloadJson ? a_request.payloadJson : "" };
				const auto formID = in.FormId("formId");
				const auto kind = in.Str("kind");
				const auto fid = in.FormId("fid");
				auto* shop = ShopBrowser::GetSingleton();
				bool ok = false;
				if (kind == "mod") {
					ok = shop->OsfSetMod(formID, in.Str("slot"), fid);
				} else if (kind == "quality") {
					ok = shop->OsfSetQuality(formID, fid);
				} else if (kind == "rarity") {
					ok = shop->OsfSetRarity(formID, fid);
				}
				std::string json = "{\"ok\":";
				json += ok ? "true" : "false";
				json += ",\"detail\":";
				json += DetailJson(shop->OsfDetailOf(formID));
				json += '}';
				RespondWith(a_request, json.c_str());
			} catch (const std::exception& e) {
				REX::ERROR("OSF shop: configure failed: {}", e.what());
				RejectWith(a_request, "shop-error", e.what());
			} catch (...) {
				RejectWith(a_request, "shop-error", "configure failed");
			}
		}

		std::string AudioJson(const ShopBrowser::OsfAudioState& a_audio)
		{
			std::string json = std::format("{{\"ok\":true,\"musicVolume\":{},\"sfxVolume\":{},\"musicMuted\":{},\"sfxMuted\":{},"
							   "\"track\":\"{}\",\"tracks\":[",
				a_audio.musicVolume, a_audio.sfxVolume, a_audio.musicMuted ? "true" : "false",
				a_audio.sfxMuted ? "true" : "false", Esc(a_audio.track));
			for (std::size_t i = 0; i < a_audio.tracks.size(); ++i) {
				if (i) {
					json += ',';
				}
				json += '"';
				json += Esc(a_audio.tracks[i]);
				json += '"';
			}
			json += "]}";
			return json;
		}

		// Body only, so the pick handler can answer with the same theme list
		// without producing a second "ok" key and an ambiguous document.
		std::string ThemesBody()
		{
			const auto rows = ShopBrowser::GetSingleton()->OsfThemes();
			int active = 0;
			for (const auto& row : rows) {
				if (row.active) {
					active = row.index;
				}
			}
			std::string json = "\"active\":" + std::to_string(active) + ",\"themes\":[";
			for (std::size_t i = 0; i < rows.size(); ++i) {
				const auto& row = rows[i];
				if (i) {
					json += ',';
				}
				json += std::format("{{\"index\":{},\"name\":\"{}\",\"suffix\":\"{}\",\"premium\":{},\"unlocked\":{},"
									"\"active\":{},\"progress\":\"{}\",\"accent\":\"{}\",\"accentBright\":\"{}\",\"gold\":\"{}\","
									"\"panel\":\"{}\",\"panelAlt\":\"{}\",\"window\":\"{}\",\"ink\":\"{}\",\"muted\":\"{}\",\"border\":\"{}\"}}",
					row.index, Esc(row.name), Esc(row.suffix), row.premium ? "true" : "false", row.unlocked ? "true" : "false",
					row.active ? "true" : "false", Esc(row.progress), row.accent, row.accentBright, row.gold, row.panel,
					row.panelAlt, row.window, row.ink, row.muted, row.border);
			}
			json += ']';
			return json;
		}

		std::string ThemesJson()
		{
			return std::string("{\"ok\":true,") + ThemesBody() + '}';
		}

		void PreviewRequest(const OSFUI::API::Request& a_request, void*) noexcept
		{
			try {
				std::scoped_lock lock(g_mutex);
				EnsureSession();
				const JsonIn in{ a_request.payloadJson ? a_request.payloadJson : "" };
				const auto yaw = static_cast<float>(std::strtod(std::string(in.Raw("yaw")).c_str(), nullptr));
				const auto pitch = static_cast<float>(std::strtod(std::string(in.Raw("pitch")).c_str(), nullptr));
				const auto zoom = static_cast<float>(std::strtod(std::string(in.Raw("zoom")).c_str(), nullptr));
				const auto image = SFE::OsfPreview::Render(in.FormId("formId"), yaw, pitch, zoom <= 0.0f ? 1.0f : zoom);
				std::string json = "{\"ok\":";
				json += image.ok ? "true" : "false";
				json += ",\"data\":\"";
				json += Esc(image.dataUrl);
				json += std::format("\",\"width\":{},\"height\":{},\"error\":\"", image.width, image.height);
				json += Esc(image.error);
				json += "\"}";
				RespondWith(a_request, json.c_str());
			} catch (const std::exception& e) {
				REX::ERROR("OSF shop: preview failed: {}", e.what());
				RejectWith(a_request, "shop-error", e.what());
			} catch (...) {
				RejectWith(a_request, "shop-error", "preview failed");
			}
		}

		void AudioRequest(const OSFUI::API::Request& a_request, void*) noexcept
		{
			try {
				std::scoped_lock lock(g_mutex);
				EnsureSession();
				auto* shop = ShopBrowser::GetSingleton();
				const JsonIn in{ a_request.payloadJson ? a_request.payloadJson : "" };
				const auto op = in.Str("op");
				if (op == "sfx") {
					// The page asks for one UI sound; the plugin owns the bank, so an
					// unknown name is silence, not an error the page must handle.
					// PlaySfx only queues state for the audio thread, and it already
					// refuses while the sound level is zero, so muting stays here.
					const auto name = in.Str("name");
					if (name == "click") {
						ShopAudio::PlaySfx(ShopAudio::Sfx::kClick);
					} else if (name == "hover") {
						ShopAudio::PlaySfx(ShopAudio::Sfx::kHover);
					} else if (name == "select") {
						ShopAudio::PlaySfx(ShopAudio::Sfx::kSelect);
					} else if (name == "denied") {
						ShopAudio::PlaySfx(ShopAudio::Sfx::kDenied);
					} else if (name == "remove") {
						// Taking a basket line out, or clearing the list.
						ShopAudio::PlaySfx(ShopAudio::Sfx::kRemove);
					}
					// A one-line ack: the audio state carries the whole track list,
					// which would mean a directory scan per click and per hover.
					RespondWith(a_request, "{\"ok\":true}");
					return;
				}
				if (op == "set") {
					const auto current = shop->OsfAudio();
					shop->OsfSetAudio(static_cast<int>(in.Int("musicVolume", current.musicVolume)),
						static_cast<int>(in.Int("sfxVolume", current.sfxVolume)),
						in.Bool("musicMuted", current.musicMuted), in.Bool("sfxMuted", current.sfxMuted),
						in.Str("track", current.track), in.Bool("persist", true));
				}
				const auto json = AudioJson(shop->OsfAudio());
				RespondWith(a_request, json.c_str());
			} catch (const std::exception& e) {
				REX::ERROR("OSF shop: audio failed: {}", e.what());
				RejectWith(a_request, "shop-error", e.what());
			} catch (...) {
				RejectWith(a_request, "shop-error", "audio failed");
			}
		}

		void ThemesRequest(const OSFUI::API::Request& a_request, void*) noexcept
		{
			try {
				std::scoped_lock lock(g_mutex);
				EnsureSession();
				const auto json = ThemesJson();
				RespondWith(a_request, json.c_str());
			} catch (const std::exception& e) {
				REX::ERROR("OSF shop: themes failed: {}", e.what());
				RejectWith(a_request, "shop-error", e.what());
			} catch (...) {
				RejectWith(a_request, "shop-error", "themes failed");
			}
		}

		void ThemePickRequest(const OSFUI::API::Request& a_request, void*) noexcept
		{
			try {
				std::scoped_lock lock(g_mutex);
				EnsureSession();
				const JsonIn in{ a_request.payloadJson ? a_request.payloadJson : "" };
				const bool ok = ShopBrowser::GetSingleton()->OsfSetTheme(static_cast<int>(in.Int("index", -1)));
				std::string json = "{\"ok\":";
				json += ok ? "true" : "false";
				json += ",\"error\":\"";
				json += ok ? "" : "that theme is still locked";
				json += "\",";
				json += ThemesBody();
				json += '}';
				RespondWith(a_request, json.c_str());
			} catch (const std::exception& e) {
				REX::ERROR("OSF shop: theme pick failed: {}", e.what());
				RejectWith(a_request, "shop-error", e.what());
			} catch (...) {
				RejectWith(a_request, "shop-error", "theme pick failed");
			}
		}
	}


	// -----------------------------------------------------------------------
// 	Lifecycle: attach to OSF UI, tick until it lands
	// -----------------------------------------------------------------------
	bool IsAttached()
	{
		return g_attached.load(std::memory_order_acquire);
	}

	void Tick()
	{
		if (IsAttached()) {
			return;
		}
		if (!g_bridge.Init()) {
			return;  // OSF UI is not loaded yet; retried every task tick
		}
		// The view folder must already be on disk: RegisterView reads the
		// manifest and its page. A miss is retried, so installing the plugin and
		// the view in the same mod folder (as shipped) is enough.
		if (!g_bridge.RegisterView(kViewId)) {
			return;
		}
		g_bridge.RegisterRequest(kEpCatalog, &CatalogRequest, nullptr);
		g_bridge.RegisterRequest(kEpRows, &RowsRequest, nullptr);
		g_bridge.RegisterRequest(kEpBasket, &BasketRequest, nullptr);
		g_bridge.RegisterRequest(kEpAccept, &AcceptRequest, nullptr);
		g_bridge.RegisterRequest(kEpDetail, &DetailRequest, nullptr);
		g_bridge.RegisterRequest(kEpConfigure, &ConfigureRequest, nullptr);
		g_bridge.RegisterRequest(kEpRefresh, &RefreshRequest, nullptr);
		g_bridge.RegisterRequest(kEpPreview, &PreviewRequest, nullptr);
		g_bridge.RegisterRequest(kEpAudio, &AudioRequest, nullptr);
		g_bridge.RegisterRequest(kEpThemes, &ThemesRequest, nullptr);
		g_bridge.RegisterRequest(kEpThemePick, &ThemePickRequest, nullptr);
		g_bridge.RegisterRequest(kEpFavourite, &FavouriteRequest, nullptr);
		// The page reports the view's shown/hidden state here; the open key is
		// OSF's own binding, subscribed below rather than polled.
		g_bridge.RegisterCommand(kCmdView, &ViewStateCommand, nullptr);
		g_bridge.RegisterCommand(kCmdClose, &CloseCommand, nullptr);
		g_bridge.RegisterCommand(kEpNote, &NoteCommand, nullptr);
		// Schema before subscription: the hotkey service binds what the schema
		// declares, so subscribing first would resolve to nothing.
		if (!g_bridge.RegisterSettingsSchema(kSchemaJson)) {
			REX::WARN("OSF shop: OSF UI refused the settings schema; falling back to the interface list for opening");
		}
		g_hotkeyToken = g_bridge.SubscribeHotkey(kModId, kHotkeyKey, &OpenHotkey, nullptr);
		// SubscribeHotkey accepts a binding whether or not the setting exists, so
		// ask OSF what the binding actually is: an empty answer means the schema
		// was refused (or the player unbound it), and the hub is then the only
		// way in - which is a state worth a line in the log, not a silent miss.
		char boundKey[64]{};
		const auto keyBytes = g_bridge.GetSettingString(kModId, kHotkeyKey, boundKey, sizeof(boundKey));
		if (g_hotkeyToken != 0 && keyBytes > 0 && boundKey[0]) {
			g_openKey = boundKey;
			REX::INFO("OSF shop: open key '{}' bound through OSF UI ('{}.{}')", boundKey, kModId, kHotkeyKey);
		} else {
			REX::WARN("OSF shop: OSF UI has no '{}.{}' binding (schema refused or unbound); open the shop from OSF's interface list",
				kModId, kHotkeyKey);
		}
		std::uint32_t major = 0, minor = 0, patch = 0;
		g_bridge.GetPluginVersion(major, minor, patch);
		g_attached.store(true, std::memory_order_release);
		REX::INFO("OSF shop: view {} registered with OSF UI {}.{}.{} (bridge ABI 0x{:08X})", kViewId, major, minor, patch,
			g_bridge.GetInterfaceVersion());
	}
}
