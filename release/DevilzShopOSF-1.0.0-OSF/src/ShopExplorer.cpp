// ============================================================================
// ShopExplorer.cpp - the catalog and the shop behind it
// ============================================================================
// Everything the player sees sits behind ShopBrowser, a singleton guarded by
// one mutex: which records the catalog holds, how they are filtered sorted and
// priced, the basket, the transaction, and the two front ends. Nothing here
// owns a window or a device - ImguiOverlay.cpp draws this object, and
// OsfFrontend.cpp turns its state into JSON for the OSF page.
//
// Layout, top to bottom:
//   helpers        text folding/trimming/width, star glyph, and the identity
//                  heuristics that hide test records, turrets and actor skins
//   classifiers    Classify*Sub: which shelf a record belongs on, keywords
//                  first and names second
//   row data       FormValue / FormWeight / FormKeywords / display names, and
//                  the cached search and sort keys
//   preview        model paths, OMOD assembly scan, authored orientation
//   purchase       the "spike": OMODs and rarity attached to a world reference
//                  before the item reaches the inventory, each engine call in
//                  its own guarded helper
//   ShopBrowser    lifecycle, persistence, catalog build, sorting, basket
//   DrawImGui      the whole interface, in one function
//   OSF adapter    the page's answers, delegated to the same logic
//
// Rules that are easy to break:
//   * ImGui calls happen only inside DrawImGui, on the render thread, with
//     stateMutex held. The task thread sets pending* flags and nothing else.
//   * A row is identified by FormID: two plugins can share an editor ID.
//   * Filters and sorts read cached keys, rebuilt once per list change - never
//     per frame, never per keystroke.
//   * Opening freezes the world, so every path into Open() is gated on the
//     overlay being able to draw (see §0o in HANDOFF.md).
// ============================================================================

#include "PCH.h"
#include "ShopExplorer.h"
#include "BuildGate.h"
#include "ImguiOverlay.h"
#include "ItemCapture.h"
#include "ShopAudio.h"
#include "ShopPause.h"
#include "OfflineModel.h"
#include "NativeModel.h"
#include "LiveModel.h"
#include "OmodMnamTable.h"
#include "OmodClassTable.h"

#include "RE/G/GameMenuBase.h"
#include "RE/I/IMenu.h"
#include "RE/U/UI.h"
#include "RE/B/BSTEvent.h"
#include "RE/B/BGSInventoryInterface.h"
// BGSPreviewTransform.h only forward-declares BGSTransform; the preview
// placement reader dereferences the record itself.
#include "RE/B/BGSTransform.h"
#include "RE/B/BGSPreviewTransform.h"
#include "REL/Relocation.h"
#include "RE/IDs.h"
#include "REX/TEnumSet.h"
#include "RE/S/ScaleformGFxValue.h"
#include <cstring>
#include <atomic>
#include <chrono>  // OSF session pacing for the theme trip count

#include <algorithm>
#include <bit>
#include <filesystem>
#include <fstream>
#include <format>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <string_view>

#include "RE/B/BSLock.h"

#include <windows.h>
// wingdi.h defines ERROR as 0, which breaks REX::ERROR below.
#ifdef ERROR
#undef ERROR
#endif

#include "RE/P/PlayerCharacter.h"
#include "RE/A/AlchemyItem.h"
#include "RE/B/BGSInventoryItem.h"
#include "RE/B/BGSKeyword.h"
#include "RE/B/BGSKeywordForm.h"
#include "RE/B/BGSModelMaterialSwap.h"
#include "RE/B/BSContainer.h"
#include "RE/E/ExtraDataList.h"
#include "RE/T/TESFullName.h"
#include "RE/T/TESModel.h"
#include "RE/T/TESAmmo.h"
#include "RE/T/TESBoundObject.h"
#include "RE/T/TESObjectARMO.h"
#include "RE/T/TESObjectARMA.h"
#include "RE/T/TESObjectBOOK.h"
#include "RE/T/TESObjectMISC.h"
#include "RE/T/TESObjectWEAP.h"
// The preview-placement reader casts a key to TESKey (a KEYM form reports
// KEYM, not MISC, even though TESKey derives TESObjectMISC) and an aid form to
// AlchemyItem; neither class was included by this translation unit before.
#include "RE/A/AlchemyItem.h"
#include "RE/T/TESKey.h"
#include "RE/T/TESValueForm.h"
#include "RE/T/TESWeightForm.h"
#include "RE/T/TESFile.h"
#include "RE/T/TESForm.h"
#include "RE/T/TESObjectREFR.h"
#include "RE/T/TESObjectCELL.h"
#include "RE/S/Script.h"
#include "RE/RTTI.h"
#include "imgui.h"
#include <cctype>

namespace SFE
{
	namespace
	{
		// BGSKeywordForm is a BaseFormComponent (not TESForm-derived); reach it via
		// the concrete item types only. Defined before first use.
		RE::BGSKeywordForm* KeywordFormOf(const RE::TESForm* a_form)
		{
			if (!a_form) {
				return nullptr;
			}
			auto* form = const_cast<RE::TESForm*>(a_form);
			switch (std::to_underlying(form->GetFormType())) {
			case std::to_underlying(RE::FormType::kARMO):
				return form->As<RE::TESObjectARMO>();
			case std::to_underlying(RE::FormType::kWEAP):
				return form->As<RE::TESObjectWEAP>();
			case std::to_underlying(RE::FormType::kAMMO):
				return form->As<RE::TESAmmo>();
			case std::to_underlying(RE::FormType::kMISC):
				return form->As<RE::TESObjectMISC>();
			case std::to_underlying(RE::FormType::kBOOK):
				return form->As<RE::TESObjectBOOK>();
			case std::to_underlying(RE::FormType::kALCH):
				return form->As<RE::AlchemyItem>();
			default:
				return nullptr;
			}
		}

		// The seven record types the catalog can list and hand to the basket -
		// the same set BuildFormList walks for All Types. A favourite is stored
		// as a FormID, and LookupByID only proves the form still exists, so a
		// starred id is checked against this before it can become a row.

		// -------------------------------------------------------------------
		// Record identity and text helpers
		// -------------------------------------------------------------------
		bool IsCatalogRecordType(RE::FormType a_type)
		{
			switch (a_type) {
			case RE::FormType::kWEAP:
			case RE::FormType::kARMO:
			case RE::FormType::kAMMO:
			case RE::FormType::kMISC:
			case RE::FormType::kALCH:
			case RE::FormType::kBOOK:
			case RE::FormType::kKEYM:
				return true;
			default:
				return false;
			}
		}


		// -------------------------------------------------------------------
		// Text: folding, the invisible name padding, sort keys, drawing
		// -------------------------------------------------------------------
		std::string CaseFold(std::string a_text)
		{
			for (auto& ch : a_text) {
				ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			}
			return a_text;
		}

		struct Utf8Char
		{
			std::uint32_t code;
			std::size_t length;
		};

		Utf8Char DecodeUtf8(std::string_view a_text, std::size_t a_index)
		{
			const auto lead = static_cast<unsigned char>(a_text[a_index]);
			if (lead < 0x80) {
				return { lead, 1 };
			}
			const std::size_t length = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
			if (length == 1 || a_index + length > a_text.size()) {
				return { lead, 1 };
			}
			std::uint32_t code = lead & (0xFFu >> (length + 1));
			for (std::size_t i = 1; i < length; ++i) {
				const auto next = static_cast<unsigned char>(a_text[a_index + i]);
				if ((next & 0xC0u) != 0x80u) {
					return { lead, 1 };
				}
				code = (code << 6) | (next & 0x3Fu);
			}
			return { code, length };
		}

		// Whitespace and the game's icon placeholders: Starfield pads some names
		// with a leading space (tankgirlsxenologyexpanded's " Golden Glow") or a
		// private-use glyph the font cannot draw (U+F225, the food/drink marker:
		// "\uF225 Hydrated"). Left in the string those bytes are invisible in the
		// row but decide the sort - U+F225 sorts above every letter, so NAME (desc)
		// lifted the two marked Aid records above the whole Z block, and a leading
		// space sorts below everything, so NAME (asc) put " Golden Glow" above the
		// A block. A reader sees a list that is plainly out of order.
		bool IsNamePadding(std::uint32_t a_code)
		{
			if (a_code <= 0x20 || a_code == 0xA0 || a_code == 0x202F || a_code == 0x205F || a_code == 0x3000 ||
				a_code == 0xFEFF || (a_code >= 0x2000 && a_code <= 0x200F) || a_code == 0x2028 || a_code == 0x2029) {
				return true;
			}
			// BMP private use area: Starfield's inline icons (U+E000..U+F8FF).
			return a_code >= 0xE000 && a_code <= 0xF8FF;
		}

		std::string TrimNamePadding(std::string a_text)
		{
			std::size_t begin = 0;
			while (begin < a_text.size()) {
				const auto c = DecodeUtf8(a_text, begin);
				if (!IsNamePadding(c.code)) {
					break;
				}
				begin += c.length;
			}
			std::size_t end = a_text.size();
			while (end > begin) {
				// Walk back to the first byte of the last code point.
				std::size_t start = end - 1;
				while (start > begin && (static_cast<unsigned char>(a_text[start]) & 0xC0u) == 0x80u) {
					--start;
				}
				const auto c = DecodeUtf8(a_text, start);
				if (!IsNamePadding(c.code)) {
					break;
				}
				end = start;
			}
			return a_text.substr(begin, end - begin);
		}

		// ImGui measures a Selectable's label over every line but draws only up to
		// the first one, so a name carrying a newline or another control byte is a
		// blank cell twice the height of its neighbours - and, sorted on raw
		// bytes, it sits at the top of the NAME column. Mod-authored names reach
		// this code as written, so the control bytes are folded to spaces once,
		// here, rather than trusted not to exist.
		std::string SanitizeLabel(std::string a_text)
		{
			for (auto& ch : a_text) {
				const auto byte = static_cast<unsigned char>(ch);
				if (byte < 0x20u || byte == 0x7Fu) {
					ch = ' ';
				}
			}
			return a_text;
		}

		// The NAME column sorts on what a player reads. Leading punctuation would
		// otherwise collect above the A block - '"The Great Serpent" Lie' after a
		// name starting with a space, "(unnamed) [000127BE]" before every letter -
		// so the key starts at the first character that can begin a word. The label
		// itself is untouched; only the sort key skips the prefix.
		std::string NameSortKey(std::string a_text)
		{
			std::size_t begin = 0;
			while (begin < a_text.size()) {
				const auto c = DecodeUtf8(a_text, begin);
				const bool startsWord = c.code >= 0x80u || std::isalnum(static_cast<int>(c.code)) != 0;
				if (startsWord) {
					break;
				}
				begin += c.length;
			}
			if (begin >= a_text.size()) {
				return CaseFold(TrimNamePadding(a_text));
			}
			return CaseFold(a_text.substr(begin));
		}

		std::string FitTextToWidth(std::string_view a_text, float a_width)
		{
			std::string text(a_text);
			if (a_width <= 0.0f) {
				return {};
			}
			if (ImGui::CalcTextSize(text.c_str()).x <= a_width) {
				return text;
			}
			std::size_t end = text.size();
			while (end > 0) {
				--end;
				while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0u) == 0x80u) {
					--end;
				}
				std::string clipped = text.substr(0, end) + "...";
				if (ImGui::CalcTextSize(clipped.c_str()).x <= a_width) {
					return clipped;
				}
			}
			return "...";
		}

		void DrawClippedText(ImDrawList* a_draw, const ImVec2& a_pos, float a_width, ImU32 a_color, std::string_view a_text)
		{
			const auto text = FitTextToWidth(a_text, a_width);
			a_draw->AddText(a_pos, a_color, text.c_str());
		}

		// The star is geometry, not text. Every UI face this overlay loads - Segoe
		// UI, Segoe UI Semibold, Arial and even Segoe UI Symbol - reports U+2605
		// missing (checked with GetGlyphIndicesW), so the atlas baked the notdef
		// diamond and the favourites column showed it on every row. Ten vertices,
		// alternating radius; a filled star is convex-batched, a hollow one is ten
		// segments because the inner points make the outline concave.
		void DrawStarGlyph(ImDrawList* a_draw, const ImVec2& a_center, float a_radius, ImU32 a_color,
			bool a_filled, float a_thickness)
		{
			constexpr float kPi = 3.14159265358979323846f;
			constexpr int kPoints = 5;
			const float inner = a_radius * 0.44f;
			ImVec2 points[kPoints * 2];
			for (int i = 0; i < kPoints * 2; ++i) {
				const float angle = -kPi * 0.5f + static_cast<float>(i) * kPi / static_cast<float>(kPoints);
				const float radius = (i % 2 == 0) ? a_radius : inner;
				points[i] = ImVec2(a_center.x + std::cos(angle) * radius, a_center.y + std::sin(angle) * radius);
			}
			if (a_filled) {
				a_draw->AddConvexPolyFilled(points, kPoints * 2, a_color);
				return;
			}
			for (int i = 0; i < kPoints * 2; ++i) {
				a_draw->AddLine(points[i], points[(i + 1) % (kPoints * 2)], a_color, a_thickness);
			}
		}

		// The favourite star's hit area and the gap to the label beside it. The
		// header star and every row star use the same slot, so the column of
		// stars lines up without a table column of its own.
		constexpr float kStarSlot = 22.0f;
		constexpr float kStarGap = 6.0f;

		bool HasTextTerm(const std::string& a_text, std::string_view a_term);
		template <class... Terms>
		bool HasAnyText(const std::string& a_text, Terms... a_terms);

		ArmorSub ClassifyArmorSub(const std::string& a_searchText)
		{
			// Search text includes live display name, EDID, and keywords. Prefer
			// explicit equipment-slot keywords; names/EDIDs fill gaps in plugin
			// records that do not carry Starfield's InventoryCategory keywords.
			//
			// Accessories first. ArmorTypeApparelHead is the head SLOT, so the
			// glasses and every Neuroamp share it with 84 caps, hats and masks;
			// their own keyword (ArmorTypeNeuroamp) or name has to win, otherwise
			// NeuroBoost Mark II files as headwear.
			if (a_searchText.find("armortypeneuroamp") != std::string::npos ||
				HasAnyText(a_searchText, "glasses", "eyewear", "goggles", "monocle", "sunglasses",
					"spectacles", "eyepatch", "neuroamp", "neuroboost", "neurocom", "neurotac")) {
				return ArmorSub::kAccessory;
			}
			// Body before helmet. A full suit carries ArmorTypeSpacesuitHelmet and
			// ArmorTypeSpacesuitBackpack because those parts ship with it; the old
			// helmet-first order put every Starborn suit (Tempus, Solis, ...) in
			// Helmets. Every record holding both keywords is a suit body.
			if (a_searchText.find("spacesuitbody") != std::string::npos ||
				a_searchText.find("categoryarmorsuit") != std::string::npos) {
				return ArmorSub::kSuit;
			}
			if (a_searchText.find("spacesuithelmet") != std::string::npos ||
				a_searchText.find("categoryarmorhelmet") != std::string::npos ||
				a_searchText.find("ma_armor_spacesuit_helmet") != std::string::npos ||
				HasAnyText(a_searchText, "helmet", "helmets")) {
				return ArmorSub::kHelmet;
			}
			if (a_searchText.find("spacesuitbackpack") != std::string::npos ||
				a_searchText.find("categoryarmorbackpack") != std::string::npos ||
				HasAnyText(a_searchText, "backpack", "backpacks", "jetpack")) {
				return ArmorSub::kPack;
			}
			// A garment that names itself (outfit, jumpsuit, uniform, dress) stays
			// Apparel even when its name also says hat: "Miner Hard Hat Outfit"
			// is the outfit, not the hardhat.
			if (a_searchText.find("inventorycategoryclothing") != std::string::npos ||
				HasAnyText(a_searchText, "apparel", "clothing", "outfit", "uniform", "jumpsuit",
					"suit", "jacket", "pants", "dress")) {
				return ArmorSub::kApparel;
			}
			// Hats, caps and other headwear share ArmorTypeApparelHead with the
			// accessories above. Crowd records (the "Beanie" outfits) instead
			// carry ArmorTypeApparelOrNakedBody, so the name tokens have to be
			// consulted before that body keyword.
			if (a_searchText.find("apparelhead") != std::string::npos ||
				HasAnyText(a_searchText, "cap", "caps", "hat", "hats", "beret", "beanie", "hood",
					"headwear", "headgear", "bandana")) {
				return ArmorSub::kHat;
			}
			if (a_searchText.find("apparel") != std::string::npos) {
				return ArmorSub::kApparel;
			}
			if (HasAnyText(a_searchText, "spacesuit", "spacesuits", "space suit", "space suits")) {
				return ArmorSub::kSuit;
			}
			return ArmorSub::kOther;
		}

		// Classify from the searchable identity (display name + EDID +
		// keywords), not keywords alone: DLC/plugin weapons often lack the
		// vanilla DisplayObjectType tags. Explicit type tags win over name
		// fallbacks, with a deterministic family priority for multi-tagged
		// records (Heavy > Sniper > Shotgun > Pistol > Rifle > Melee > Thrown).
		WeaponSub ClassifyWeaponSub(const std::string& a_searchText)
		{
			if (a_searchText.find("toolandheavyweapon") != std::string::npos ||
				a_searchText.find("weapontypeheavy") != std::string::npos ||
				a_searchText.find("weapontypeheavyweapon") != std::string::npos ||
				a_searchText.find("displayobjecttypeheavy") != std::string::npos ||
				HasAnyText(a_searchText, "heavy weapon", "heavy weapons", "microgun", "magstorm", "va'ruun inflictor")) {
				return WeaponSub::kHeavy;
			}
			if (a_searchText.find("sniper") != std::string::npos || HasAnyText(a_searchText, "longshot", "hard target")) {
				return WeaponSub::kSniper;
			}
			if (a_searchText.find("displayobjecttypeshotgun") != std::string::npos ||
				a_searchText.find("weapontypeshotgun") != std::string::npos ||
				HasAnyText(a_searchText, "shotgun", "shotguns")) {
				return WeaponSub::kShotgun;
			}
			if (a_searchText.find("displayobjecttypepistol") != std::string::npos ||
				a_searchText.find("weapontypepistol") != std::string::npos ||
				HasAnyText(a_searchText, "pistol", "pistols", "sidearm", "sidearms")) {
				return WeaponSub::kPistol;
			}
			if (a_searchText.find("displayobjecttyperifle") != std::string::npos ||
				a_searchText.find("weapontyperifle") != std::string::npos ||
				a_searchText.find("weapontypeassaultrifle") != std::string::npos ||
				a_searchText.find("weapontypeautomatic") != std::string::npos ||
				HasAnyText(a_searchText, "rifle", "rifles", "carbine", "smg", "submachine gun", "assault rifle")) {
				return WeaponSub::kRifle;
			}
			if (a_searchText.find("displayobjecttypemelee") != std::string::npos ||
				a_searchText.find("weapontypemelee") != std::string::npos ||
				a_searchText.find("animknife") != std::string::npos ||
				a_searchText.find("animonehanded") != std::string::npos ||
				HasAnyText(a_searchText, "melee", "sword", "blade", "knife", "dagger", "wakizashi", "tanto")) {
				return WeaponSub::kMelee;
			}
			if (a_searchText.find("throwable") != std::string::npos ||
				a_searchText.find("weapontypethrown") != std::string::npos ||
				a_searchText.find("weapontypeexplosive") != std::string::npos ||
				a_searchText.find("weapontypemine") != std::string::npos ||
				a_searchText.find("animgrenade") != std::string::npos ||
				HasAnyText(a_searchText, "grenade", "grenades", "mine", "mines", "throwing knife")) {
				return WeaponSub::kThrown;
			}
			return WeaponSub::kOther;
		}


		bool HasTextTerm(const std::string& a_text, std::string_view a_term)
		{
			const auto isWord = [](char c) {
				return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
			};
			if (a_term.empty()) {
				return false;
			}
			std::size_t at = 0;
			while ((at = a_text.find(a_term, at)) != std::string::npos) {
				const std::size_t end = at + a_term.size();
				const bool leftBoundary = at == 0 || !isWord(a_text[at - 1]) || !isWord(a_term.front());
				const bool rightBoundary = end >= a_text.size() || !isWord(a_text[end]) || !isWord(a_term.back());
				if (leftBoundary && rightBoundary) {
					return true;
				}
				++at;
			}
			return false;
		}

		template <class... Terms>
		bool HasAnyText(const std::string& a_text, Terms... a_terms)
		{
			return (HasTextTerm(a_text, a_terms) || ...);
		}

		bool IsTestDebugIdentity(const std::string& a_editorId, const std::string& a_displayName)
		{
			const auto editorId = CaseFold(a_editorId);
			const auto displayName = CaseFold(a_displayName);
			const auto hasEditorMarker = [&editorId](std::string_view a_marker) {
				std::size_t at = 0;
				while ((at = editorId.find(a_marker, at)) != std::string::npos) {
					const bool leftBoundary = at == 0 ||
						!std::isalnum(static_cast<unsigned char>(editorId[at - 1]));
					if (leftBoundary) {
						return true;
					}
					++at;
				}
				return false;
			};
			// EDIDs concatenate marker words with the noun that follows
			// (ENV_TestSpacesuit) but separate them from prefixes with `_`.
			// Requiring a left boundary catches those markers without treating
			// unrelated words such as "contest" as test/debug records.
			return hasEditorMarker("noclutter") || hasEditorMarker("dummy") || hasEditorMarker("template") ||
				hasEditorMarker("debug") || hasEditorMarker("test") || hasEditorMarker("notplayable") ||
				HasAnyText(displayName, "noclutter", "dummy", "template", "debug", "test", "not playable", "notplayable");
		}


		// The ESM shop names every record it could not resolve "DL-19 (report this
		// as a bug)". The marker lives in the record's own FULL name, so it is
		// the only way to tell these placeholder rows from real items; a whole
		// weapon category was drowned in them (see the 2026-10-24 screenshots).
		bool IsReportBugName(const std::string& a_displayName)
		{
			return CaseFold(a_displayName).find("report this as a bug") != std::string::npos;
		}

		bool IsCrewEnemyCreatureIdentity(const std::string& a_identity)
		{
			// "Enemies" is the plural spelling used by Shattered Space records
			// (e.g. "VORTEX ENEMIES ONLY"), not a substring of "enemy".
			// Do not treat WeaponTypeUnarmed as an identity marker: ordinary
			// melee weapons carry that combat keyword too (including Crucible).
			return HasAnyText(a_identity, "crew", "companion", "follower", "enemy", "enemies", "hostile",
				"opponent", "creature", "animal", "beast", "monster");
		}

		bool IsTurretIdentity(const std::string& a_identity)
		{
			// Turrets are not one form type: ship turrets are WEAP records
			// ("Pulse Laser Turret", SWL_Laser_SpaceTurret, Obliterator Alpha
			// Turret) whose model is ship hardware, and the robot turret skins
			// are ARMO records (Skin_Turret_Laser_01_Short). A word-boundary
			// match would miss the keyword atoms, where the stem is glued to the
			// rest of the name ("SpaceshipTurretWeapon", "s_40_ShipTurret_LaserA"),
			// so match the stem anywhere. No unrelated catalog term contains it.
			return a_identity.find("turret") != std::string::npos;
		}

		bool HasNumericCaliber(const std::string& a_text)

		{
			for (std::size_t i = 0; i < a_text.size(); ++i) {
				if (a_text[i] < '0' || a_text[i] > '9') continue;
				std::size_t end = i;
				while (end < a_text.size() && ((a_text[end] >= '0' && a_text[end] <= '9') || a_text[end] == '.')) ++end;
				while (end < a_text.size() && a_text[end] == ' ') ++end;
				const bool mm = a_text.compare(end, 2, "mm") == 0 &&
					(end + 2 >= a_text.size() || (a_text[end + 2] < 'a' || a_text[end + 2] > 'z'));
				const bool unit = mm || a_text.compare(end, 5, "gauge") == 0 ||
					a_text.compare(end, 7, "caliber") == 0 || a_text.compare(end, 7, "calibre") == 0;
				if (unit) return true;
			}
			return false;
		}


		// -------------------------------------------------------------------
		// Subcategory classifiers. One per record family; each reads the
		// folded keyword blob and model path, then falls back to the name.
		// -------------------------------------------------------------------
		AmmoSub ClassifyAmmoSub(const std::string& a_text)
		{
			if (HasAnyText(a_text, "spaceship", "ship ammo", "shipammo", "ship", "creatureammo", "creature ammo", "alien ammo", "beast ammo")) return AmmoSub::kShipCreature;
			if (HasAnyText(a_text, "missile", "missiles", "torpedo", "torpedoes", "guided rocket", "guided rockets")) return AmmoSub::kMissile;
			if (HasAnyText(a_text, "mine", "mines", "grenade", "grenades", "explosive round", "frag round")) return AmmoSub::kExplosive;
			if (HasAnyText(a_text, "particle", "laser", "lzr", "energy", "plasma", "fusion", "electron", "antimatter", "neutron", "x-ray", "xray")) return AmmoSub::kEnergy;
			if (HasAnyText(a_text, "caseless", "cartridge", "bullet", "ballistic", "flechette", "shot shell", "shotgun shell", "slug", "slugs") || HasNumericCaliber(a_text)) return AmmoSub::kBallistic;
			return AmmoSub::kOther;
		}

		// Names are all these two classifiers used to see, and the live census of
		// 2579 Misc / 1189 Aid records (`tools/catalog_bucket_census.py`) showed
		// why that is not enough: whole product lines ship under names that carry
		// no category word at all (Sabacc cards, Star Wars ore chunks, Holocrons in
		// a snow-globe NIF, "QuickSnack", "Tranquilitea"). The NIF a record ships
		// is the reliable half, so each bucket also checks the folded model path -
		// the same "the model decides" rule the Book split already uses. Every
		// model token below is a measured group, not a substring guess; the census
		// drops Misc/Other from 61% to 55% and Aid/Other from 58% to 6%.
		MiscSub ClassifyMiscSub(const std::string& a_text, const std::string& a_model = {})
		{
			if (HasAnyText(a_text, "fertile", "flora", "plant:", "plant ", "seed", "tissue", "tree", "aquatic", "root", "spore", "fungus", "fungal", "leaf", "flower")) return MiscSub::kFlora;
			// Collectible display pieces: plushies and action figures name
			// themselves, Holocrons, snow globes and desk toys do not.
			if (HasAnyText(a_text, "plushie", "toy", "toys", "action figure", "actionfigure", "figurine", "doll") ||
				HasAnyText(a_model, "collectables", "snowglobe", "figurine", "\\toys\\", "desktoy", "cutetoy")) return MiscSub::kToys;
			if (HasAnyText(a_text, "playing card", "playing cards", "card deck", "card pack", "collectible card", "collectible cards", "card collectible", "cards", "sabacc")) return MiscSub::kCards;
			// Bulk shipping containers: the same NIF as the loose resource, so
			// this stays name-driven and leaves the crates to the crate words.
			if (HasAnyText(a_text, "crate", "container", "containers", "empty pack", "empty canister", "storage box", "cargo case", "case")) return MiscSub::kContainers;
			// Ore chunks carry the resource they are mined as, not the ore name:
			// the Star Wars plugins call them Lead, Neutronium, Coaxium ...
			if (HasAnyText(a_text, "resource", "resources", "resinorg", "resorg", "aluminum", "aluminium", "antimony", "beryllium", "caesium", "cesium", "chlorine", "chlorosilanes", "cobalt", "copper", "dysprosium", "europium", "fluorine", "gold", "helium-3", "iron", "iridium", "lithium", "neodymium", "neon", "nickel", "palladium", "platinum", "plutonium", "silver", "tantalum", "tetrafluorides", "titanium", "tungsten", "uranium", "vanadium", "ytterbium", "caelumite", "indicite", "rothicite", "vytinium", "aldumite", "tasine", "sealant", "adhesive", "polymer", "solvent", "lubricant", "spidroin", "biosuppressant", "microsecond regulator", "zero wire", "structural material", "ore", "mineral", "inorganic", "organic material", "amino acids", "antimicrobial", "cosmetic", "fiber", "membrane", "nutrient", "pigment", "sedative", "toxin", "metabolic agent", "ornamental material", "sterile nanotubes", "analgesic", "hypercatalyst", "molecular sieve") ||
				HasAnyText(a_model, "landscape\\resources", "lootcontainers\\")) return MiscSub::kResources;
			// The eight buckets below are model-folder groups measured from the
			// 1417 records that stayed in Other after the model half landed
			// (tools/catalog_bucket_census.py, 2026-10-24). Each token is a NIF
			// family, not a substring guess: the shelf a record ships on is what
			// names it (SetDressing\ScienceGlass, Items\DataSlate,
			// Items\AnimalGenericIngredients, ...). Names add nothing here.
			if (HasAnyText(a_model, "setdressing\\office", "setdressing\\kitchen", "pegasus_homegarden",
					"privatelivingquarters", "posters", "interactivehavok", "desktop_items", "bar_glassware",
					"baritems", "setdressing\\neon", "setdressing\\decoration", "dining_set", "setdressing\\lodge",
					"trackeralliancehq",
					// SetDressing\SFBGS003 ships two unrelated things: the mission
					// board terminals (Bounty: ... / Guild Network) and the ammo
					// primer boxes. Only the first is set dressing, so the token is
					// the sub-folder - a bare `sfbgs003` would sweep Charged Diodes
					// in with the boards.
					"mission_board_terminals")) return MiscSub::kDecor;
			// One folder, 223 records: vials, beakers, tongs and the sample vials.
			if (HasAnyText(a_model, "scienceglass")) return MiscSub::kLab;
			// The same folder also carries the Old Earth PC props (AAA_Base_Pc,
			// _Monitor, _Tower): a desktop tower is not a creature part, so the PC
			// file name wins before the folder rule.
			if (HasAnyText(a_model, "aaa_base_pc")) return MiscSub::kTech;
			// tankgirlck\xenologymesh: tusks, mandibles, plates, pressed insects.
			if (HasAnyText(a_model, "xenologymesh")) return MiscSub::kCreature;
			// ammoprimerbox: the four ammo sub-components (primers, Charged and
			// Magnetic Diodes) that share the SFBGS003 shelf with the boards.
			if (HasAnyText(a_model, "manufactured_goods", "tech_clutter", "tools_kit", "prototype_tech",
					"powercell", "xtech_misc", "ammoprimerbox")) return MiscSub::kTech;
			// 48 records all share DataSlate01.nif; the name never says slate.
			if (HasAnyText(a_model, "items\\dataslate")) return MiscSub::kSlates;
			// FoodDrink_Set packs, coffee bags and CoffeeToGo cups that stayed Misc.
			if (HasAnyText(a_model, "fooddrink_", "coffeetogo")) return MiscSub::kFood;
			// Earth relics (Droid Head, Holodisc, Antique Video Tape) and the
			// Armistice data objects. SetDressing\Earth is its own shelf, so the
			// token is the folder, not the artifact_*.nif files only.
			if (HasAnyText(a_model, "setdressing\\artifacts", "setdressing\\earth",
					"armisticedataobject")) return MiscSub::kArtifacts;
			if (HasAnyText(a_model, "animalgenericingredients")) return MiscSub::kAnimal;
			return MiscSub::kOther;
		}

		AidSub ClassifyAidSub(const std::string& a_text, const std::string& a_model = {})
		{
			// Meds first: bacta, bandages and affliction cures share one model
			// folder, so the folder settles them even when the name is a proper
			// noun ("Kolto-Bacta Patch", "Salve").
			if (HasAnyText(a_text, "med pack", "medpack", "bandage", "bandages", "injector", "antibiotic", "antibiotics", "heal gel", "healing", "immobilizer", "immobilizers", "cure", "trauma", "emergency kit", "sterile", "medical treatment", "panacea", "penicillin", "bacta", "kolto", "poultice", "salve", "medkit") ||
				HasAnyText(a_model, "affliction_cures", "bacta", "trauma_pack", "panacaea", "medstim")) return AidSub::kMeds;
			// Food and drink: the FoodDrink / CoffeeToGo / Tranquilitea model
			// families cover the names that glue the category word (QuickSnack,
			// Synthameat) or drop it entirely (Ripe Jellyfruit, Koza).
			if (HasAnyText(a_text, "food", "meat", "fresh", "milk", "drink", "meal", "steak", "sandwich", "cream", "bread", "fruit", "vegetable", "water", "soup", "juice", "coffee", "tea", "chunks", "chocolate", "egg", "cheese", "fish", "chowder", "curry", "wrap", "cake", "cookie", "burger", "pizza", "pasta", "rice", "yam", "snack", "soda", "wine", "beer", "liquor", "tea", "beverage", "ale", "brew", "whiskey", "whisky", "cocktail", "heart", "pie", "toast", "baguette", "cereal", "seaweed", "ichor", "vinegar", "alcohol") ||
				HasAnyText(a_model, "fooddrink", "xenoplantpod", "\\xenologymesh\\food\\", "coffeetogo", "tranquilitea", "foodanddrink", "moonshine", "cocktailstation", "foodpouch", "foodpackets", "itm_milk")) return AidSub::kFood;
			if (HasAnyText(a_text, "aurora", "stimulant", "chem", "poison", "hallucinogen", "combat enhancer", "red trench", "battlestim", "frostwolf", "whiteout", "panopticon", "amp", "blend", "synapse", "otme", "synephrine", "potion", "potions") ||
				HasAnyText(a_model, "\\chem_", "inhailer", "itm_potion")) return AidSub::kChems;
			return AidSub::kOther;
		}

		BookSub ClassifyBookSub(const std::string& a_text, const std::string& a_model)
		{
			const auto model = CaseFold(a_model);
			// The model decides the first split. Every printed book in the base
			// game carries ObjectTypeDataSlate as well (Race to the Heavens and
			// The Ancient Civilization of Egypt are BookLarge01.nif), so the
			// keyword cannot tell a slate from a book cover - but the NIF can.
			if (model.find("dataslate") != std::string::npos) return BookSub::kSlates;
			if (model.find("notepad") != std::string::npos || model.find("paper") != std::string::npos) {
				return BookSub::kNotes;
			}
			if (HasAnyText(a_text, "note", "notes", "report", "reports", "memo", "memos", "evidence", "letter",
					"message", "interview")) return BookSub::kNotes;
			if (HasAnyText(a_text, "log", "logs", "journal", "journals", "diary", "recording", "transcript",
					"audio log", "communique", "communication")) return BookSub::kLogs;
			if (HasAnyText(a_text, "slate", "slates")) return BookSub::kSlates;
			// A book model with no note/log word is a book: that is where the
			// untitled reads (Hope Family Tree, Race to the Heavens) belong
			// instead of falling through to Misc / Other.
			if (model.find("book") != std::string::npos) return BookSub::kGuides;
			if (HasAnyText(a_text, "guide", "manual", "textbook", "encyclopedia", "survey", "catalog", "handbook", "book", "magazine", "comic")) return BookSub::kGuides;
			return BookSub::kOther;
		}

		KeySub ClassifyKeySub(const std::string& a_text)
		{
			if (HasAnyText(a_text, "keycard", "key card", "access card", "badge", "credential", "security card")) return KeySub::kKeycards;
			if (HasAnyText(a_text, "passcode", "password", "code", "codes", "cipher", "combination")) return KeySub::kCodes;
			if (HasAnyText(a_text, "key", "keys", "access pass")) return KeySub::kKeys;
			return KeySub::kOther;
		}

		std::string FormDisplayName(const RE::TESForm* a_form);
		std::string FormModelPath(const RE::TESForm* a_form);
		std::string FormKeywords(const RE::TESForm* a_form, std::size_t a_max);

		// Skinned attachments (Hands_3rd_F, NakedHands) and the bare body skin an
		// outfit's ARMA set carries (Skin_F) are stored in bind space around the
		// rig origin, not in the outfit's rest pose: the engine attaches them to
		// the skeleton at draw time, so a static preview draws the hands as a pair
		// of detached forearms and the head as a bald, hairless scalp taken from
		// the skin atlas (hair is a separate part the set does not carry). That is
		// what Andreja's outfit and the other apparel previews were showing. Drop
		// such a part only when the same load also carries rest-pose geometry - an
		// item whose ONLY mesh is a skin part keeps it, because a small render
		// still beats an empty viewport.
		void PruneSkinnedAttachmentParts(SFE::NativeModel::PreviewResult& a_native)
		{
			// Whole tokens only: garment names glue words together (Bodysuit_Dress)
			// but never contain a standalone "skin" or "hands", and the mesh names
			// are Outfit_..._Skin_F / Hands_3rd_F.
			const auto isBindSpaceSkinPart = [](const std::string& a_name) {
				const auto folded = CaseFold(a_name);
				std::size_t i = 0;
				while (i < folded.size()) {
					while (i < folded.size() && !std::isalnum(static_cast<unsigned char>(folded[i]))) {
						++i;
					}
					std::size_t j = i;
					while (j < folded.size() && std::isalnum(static_cast<unsigned char>(folded[j]))) {
						++j;
					}
					if (j > i) {
						const std::string token = folded.substr(i, j - i);
						if (token == "hands" || token == "skin") {
							return true;
						}
					}
					i = j + 1;
				}
				return false;
			};
			const bool hasRestPoseBody = std::any_of(a_native.meshes.begin(), a_native.meshes.end(),
				[&](const SFE::NativeModel::NativeMesh& a_mesh) {
					return a_mesh.pos.size() >= 9 && !isBindSpaceSkinPart(a_mesh.name);
				});
			if (!hasRestPoseBody) {
				return;
			}
			for (auto it = a_native.meshes.begin(); it != a_native.meshes.end();) {
				if (isBindSpaceSkinPart(it->name)) {
					REX::INFO("SFShopExplorer native preview: dropped skinned attachment '{}' (bind-space geometry, not the outfit's pose)",
						it->name);
					it = a_native.meshes.erase(it);
				} else {
					++it;
				}
			}
		}

		std::string CategorySearchText(const RE::TESForm* a_form)
		{
			std::string text = CaseFold(FormDisplayName(a_form));
			if (a_form) {
				if (const char* edid = a_form->GetFormEditorID(); edid && *edid) {
					text += ' ';
					text += CaseFold(edid);
				}
				text += ' ';
				text += CaseFold(FormKeywords(a_form, 2048));
			}
			return text;
		}

		// Folded model path for the two classifiers that read it (Misc, Aid).
		// Only those call this, so the weapon/armor/ammo/book/key paths never pay
		// for the extra model enumeration.
		std::string CategoryModelText(const RE::TESForm* a_form)
		{
			return a_form ? CaseFold(FormModelPath(a_form)) : std::string{};
		}

		template <class Sub, class Name>
		bool DrawSubcategoryCombo(const char* a_id, Sub& a_selected, Sub a_total, Name a_name)
		{
			bool changed = false;
			if (ImGui::BeginCombo(a_id, a_name(a_selected))) {
				for (std::size_t i = 0; i < static_cast<std::size_t>(std::to_underlying(a_total)); ++i) {
					const auto sub = static_cast<Sub>(i);
					if (ImGui::Selectable(a_name(sub), a_selected == sub)) {
						a_selected = sub;
						changed = true;
					}
				}
				ImGui::EndCombo();
			}
			return changed;
		}


		// Live form value/weight from the typed instance data. Tested against the
		// DevilzDump CSV ground truth (e.g. Swimsuit 00002FA4 = 94 / 0.300).

		// -------------------------------------------------------------------
		// Row data: value, weight, keywords
		// -------------------------------------------------------------------
		std::uint32_t FormValue(const RE::TESForm* a_form)
		{
			if (!a_form) {
				return 0;
			}
			auto* form = const_cast<RE::TESForm*>(a_form);
			switch (std::to_underlying(form->GetFormType())) {
			case std::to_underlying(RE::FormType::kARMO):
				if (auto* armor = form->As<RE::TESObjectARMO>(); armor && armor->instanceData.has_value()) {
					return armor->instanceData.value().value > 0 ? static_cast<std::uint32_t>(armor->instanceData.value().value) : 0;
				}
				break;
			case std::to_underlying(RE::FormType::kWEAP):
				if (auto* weapon = form->As<RE::TESObjectWEAP>(); weapon && weapon->weaponData) {
					if (const auto* general = weapon->weaponData->WeapDataGeneral) {
						return general->baseValue > 0 ? static_cast<std::uint32_t>(general->baseValue) : 0;
					}
				}
				break;
			case std::to_underlying(RE::FormType::kAMMO):
				if (auto* ammo = form->As<RE::TESAmmo>()) {
					return ammo->TESValueForm::value > 0 ? static_cast<std::uint32_t>(ammo->TESValueForm::value) : 0;
				}
				break;
			case std::to_underlying(RE::FormType::kMISC):
				if (auto* misc = form->As<RE::TESObjectMISC>()) {
					return misc->TESValueForm::value > 0 ? static_cast<std::uint32_t>(misc->TESValueForm::value) : 0;
				}
				break;
			case std::to_underlying(RE::FormType::kBOOK):
				if (auto* book = form->As<RE::TESObjectBOOK>()) {
					return book->TESValueForm::value > 0 ? static_cast<std::uint32_t>(book->TESValueForm::value) : 0;
				}
				break;
			case std::to_underlying(RE::FormType::kALCH):
				if (auto* alch = form->As<RE::AlchemyItem>(); alch && (alch->data.flags & std::to_underlying(RE::AlchemyItem::AlchemyFlag::kCostOverride)) != 0) {
					return alch->data.costOverride > 0.0f ? static_cast<std::uint32_t>(alch->data.costOverride) : 0;
				}
				break;
			default:
				break;
			}
			return 0;
		}

		float FormWeight(const RE::TESForm* a_form)
		{
			if (!a_form) {
				return 0.0f;
			}
			auto* form = const_cast<RE::TESForm*>(a_form);
			switch (std::to_underlying(form->GetFormType())) {
			case std::to_underlying(RE::FormType::kARMO):
				if (auto* armor = form->As<RE::TESObjectARMO>(); armor && armor->instanceData.has_value()) {
					return armor->instanceData.value().weight;
				}
				break;
			case std::to_underlying(RE::FormType::kWEAP):
				if (auto* weapon = form->As<RE::TESObjectWEAP>(); weapon && weapon->weaponData) {
					if (const auto* general = weapon->weaponData->WeapDataGeneral) {
						return general->baseWeight;
					}
				}
				break;
			case std::to_underlying(RE::FormType::kAMMO):
				if (auto* ammo = form->As<RE::TESAmmo>()) {
					return ammo->TESWeightForm::weight;
				}
				break;
			case std::to_underlying(RE::FormType::kMISC):
				if (auto* misc = form->As<RE::TESObjectMISC>()) {
					return misc->TESWeightForm::weight;
				}
				break;
			case std::to_underlying(RE::FormType::kBOOK):
				if (auto* book = form->As<RE::TESObjectBOOK>()) {
					return book->TESWeightForm::weight;
				}
				break;
			case std::to_underlying(RE::FormType::kALCH):
				if (auto* alch = form->As<RE::AlchemyItem>()) {
					return alch->TESWeightForm::weight;
				}
				break;
			default:
				break;
			}
			return 0.0f;
		}

		std::string FormKeywords(const RE::TESForm* a_form, std::size_t a_max)
		{
			std::string joined;
			if (a_form) {
				if (auto* kwForm = KeywordFormOf(a_form); kwForm) {
					kwForm->ForEachKeyword([&](RE::BGSKeyword* kw) {
						if (kw && !kw->formEditorID.empty()) {
							if (!joined.empty()) {
								joined += ", ";
							}
							joined += kw->formEditorID.c_str();
							if (joined.size() >= a_max) {
								return RE::BSContainer::ForEachResult::kStop;
							}
						}
						return RE::BSContainer::ForEachResult::kContinue;
					});
				}
			}
			return joined.substr(0, a_max);
		}

		// An ARMO record is a player item only when it carries the game's
		// inventory type keywords. Every other armour-typed record is the
		// engine's per-actor armour shell - the record an actor's skin uses:
		// Skin_Terrormorph_Alpha, Skin_ModelS_Combat_Generic,
		// Skin_QuadrupedA_Groat_Male, Skin_MinorBotA_Security,
		// Skin_Turret_Laser_01_Short. Those carry only DisplayObjectType*/Anim*
		// keywords, weigh 0, cost 0 and cannot be equipped by the player; the
		// preview renders them as a bare creature or robot chassis. They were
		// the whole Armor/Other bucket (Terrormorphs, Groat, MiniBots, turret
		// corpses), which is not a shop catalog.
		//
		// The split is exact over the 75-plugin test profile: all 332 ARMO
		// records without either keyword family are actor skins, and all 881
		// wearable records carry InventoryCategory* and/or ArmorType* (the
		// longest keyword list is 360 chars, well inside the 1024 cap). No
		// toggle gates this: a skin is not an inventory item at any setting.
		bool IsActorSkinArmor(const RE::TESForm* a_form)
		{
			if (!a_form || a_form->GetFormType() != RE::FormType::kARMO) {
				return false;
			}
			const auto keywords = CaseFold(FormKeywords(a_form, 1024));
			return keywords.find("inventorycategory") == std::string::npos &&
				keywords.find("armortype") == std::string::npos;
		}

		bool FilterMatches(const std::string& a_text, const char* a_filter)
		{
			if (!a_filter || !a_filter[0]) {
				return true;
			}
			auto needle = CaseFold(a_text);
			auto hay = CaseFold(a_filter);
			return needle.find(hay) != std::string::npos;
		}

		struct UiTheme
		{
			const char* name;
			// Track played while this theme is active. Resolved against the
			// Audio\ folder, falling back to the DLL directory.
			const char* music;
			// Lowercase id used to look up per-theme art: Chest_<suffix>.png /
			// space_<suffix>.png. Empty means "use the shared assets".
			const char* suffix;
			ImVec4 accent;
			ImVec4 accentBright;
			ImVec4 gold;
			ImVec4 muted;
			ImVec4 ink;
			ImVec4 windowBg;
			ImVec4 panel;
			ImVec4 panelAlt;
			ImVec4 border;
			ImVec4 header;
			ImVec4 headerHovered;
			ImVec4 button;
			ImVec4 buttonHovered;
			ImVec4 frame;
			ImVec4 frameHovered;
			// These ImGui colors were never themed, which is why every theme
			// shared the same grey table header and default scrollbar. Needed
			// for a theme to read as genuinely different rather than tinted.
			ImVec4 tableHeader;
			ImVec4 tableRowAlt;
			ImVec4 disabled;
			ImVec4 separator;
		};

		// Multiply a colour's alpha by the user's window-opacity setting, leaving the
	// RGB untouched so themes stay recognisable while running see-through.
		ImVec4 Fade(ImVec4 a_color, float a_alpha)
	{
		a_color.w *= a_alpha;
		return a_color;
	}

		// Text drawn on top of a filled widget. ImGuiCol_Text is the theme's
		// body ink, which is right for the dark panels but wrong for the bright
		// accent-filled buttons and table headers: on Hazard that is red ink on
		// hazard yellow, two saturated hues at similar lightness, and the label
		// all but vanishes. Pick black or white from the fill's own luminance
		// instead, so this stays correct for every theme rather than special-
		// casing one.
		//
		// The fallback argument is unused - a caller with no fill to sample
		// still gets a correct contrast choice from any theme, since the
		// decision only needs the fill - but it is kept so callers read as
		// "my ink, unless the fill is bright".
		ImVec4 InkOn(ImVec4 a_fill, ImVec4)
	{
		const float lum = 0.2126f * a_fill.x + 0.7152f * a_fill.y + 0.0722f * a_fill.z;
		return lum > 0.45f ? ImVec4(0.04f, 0.04f, 0.05f, 1.0f) : ImVec4(0.97f, 0.97f, 0.98f, 1.0f);
	}

	// -----------------------------------------------------------------------
		// Premium themes. The six basics are always available; these three start
		// Forward declaration: the premium helpers below need theme names.
		const UiTheme& ActiveTheme(int a_index);
		// Progress is loaded lazily, and the custom theme's unlock asks for it
		// before the definition further down.
		void LoadProgress();

		// Premium themes. The six basics are always available; these three start
		// locked and are earned by actually using the shop, so they measure the
		// shop working rather than a settings toggle.
		//
		// Starfield  - trip count (opening the shop is the thing being measured)
		// Hell       - credits actually spent
		// Heaven     - free hauls
		//
		// Progress persists in the INI, so it survives relaunch. INI is a poor
		// place for save state but it is what this build already uses and keeps
		// the unlock out of the savegame.
		// -----------------------------------------------------------------------
		enum class Unlock : std::uint8_t
		{
			kOpens = 0,
			kSpent,
			kFree
		};

		struct PremiumTheme
		{
			Theme       theme;
			Unlock      metric;
			std::uint64_t goal;
			const char* line;
		};

		const PremiumTheme kPremium[] = {
			{ Theme::kStarfield, Unlock::kOpens, 8, "the void opens" },
			{ Theme::kHell, Unlock::kSpent, 25000, "hellfire breaks loose" },
			{ Theme::kHeaven, Unlock::kFree, 12, "the choir sings" },
		};
		constexpr std::size_t kPremiumCount = std::size(kPremium);

		// -----------------------------------------------------------------------
		// The custom theme.
		//
		// A tenth theme the player builds out of the same nineteen colours every
		// built-in theme is made of, so it needs no rendering path of its own:
		// ActiveTheme() hands back this struct and the whole shop re-colours.
		//
		// It is the reward for finishing the set - unlocked only when all three
		// earned themes are earned. The name, track, backdrop and colours are
		// kept in [custom] / [customcolours] beside the rest of the shop's
		// settings, so keeping the INI keeps the look.
		// -----------------------------------------------------------------------
		// Where the three panes sit. A preset only changes which column a pane
		// draws into - or, for the stacked one, that the catalog and the basket
		// are rows instead of columns - so every arrangement is the same
		// workspace code, drawn in the same order.
		enum class Layout : int
		{
			kClassic = 0,   // plugins | items | basket
			kBasketCentre,  // plugins | basket | items
			kBasketLeft,    // basket | plugins | items
			kCatalogTop,    // plugins + items above, basket across the bottom
			kTotal
		};

		inline constexpr const char* LayoutName(Layout a_layout)
		{
			switch (a_layout)
			{
			case Layout::kClassic: return "Classic - basket right";
			case Layout::kBasketCentre: return "Basket centre";
			case Layout::kBasketLeft: return "Basket left";
			case Layout::kCatalogTop: return "Catalog top, basket bottom";
			default: return "?";
			}
		}

		inline constexpr const char* LayoutHint(Layout a_layout)
		{
			switch (a_layout)
			{
			case Layout::kClassic: return "Plugins left, item list in the middle, basket and preview on the right.";
			case Layout::kBasketCentre: return "Basket in the middle; the item list moves right and shrinks to make room.";
			case Layout::kBasketLeft: return "Basket on the left, plugins beside it, item list on the right.";
			case Layout::kCatalogTop: return "Plugins and item list share the top; the basket runs full width along the bottom.";
			default: return "";
			}
		}

		struct CustomColour
		{
			const char* key;
			ImVec4 UiTheme::* field;
		};

		// Every colour a theme owns, in the order the editor groups them.
		const CustomColour kCustomColours[] = {
			{ "accent", &UiTheme::accent },
			{ "accentBright", &UiTheme::accentBright },
			{ "gold", &UiTheme::gold },
			{ "windowBg", &UiTheme::windowBg },
			{ "panel", &UiTheme::panel },
			{ "panelAlt", &UiTheme::panelAlt },
			{ "ink", &UiTheme::ink },
			{ "muted", &UiTheme::muted },
			{ "disabled", &UiTheme::disabled },
			{ "button", &UiTheme::button },
			{ "buttonHovered", &UiTheme::buttonHovered },
			{ "frame", &UiTheme::frame },
			{ "frameHovered", &UiTheme::frameHovered },
			{ "header", &UiTheme::header },
			{ "headerHovered", &UiTheme::headerHovered },
			{ "border", &UiTheme::border },
			{ "separator", &UiTheme::separator },
			{ "tableHeader", &UiTheme::tableHeader },
			{ "tableRowAlt", &UiTheme::tableRowAlt },
		};

		struct CustomThemeState
		{
			UiTheme    ui{};
			std::string name{ "Custom" };
			std::string music{ "shop_music_dark.wav" };
			std::string backdrop{};
			Layout      layout{ Layout::kClassic };
			float       pluginsWidth{ 280.0f };
			float       detailsWidth{ 340.0f };
			bool        loaded{ false };
		};

		CustomThemeState& Custom()
		{
			static CustomThemeState state;
			return state;
		}

		// UiTheme holds const char*, so the strings have to outlive every edit:
		// this re-points them at the std::string members after any change.
		void SyncCustomThemeStrings()
		{
			auto& custom = Custom();
			custom.ui.name = custom.name.c_str();
			custom.ui.music = custom.music.c_str();
			custom.ui.suffix = custom.backdrop.c_str();
		}

		void LoadCustomTheme();

		bool IsPremium(Theme a_theme)
		{
			// The custom theme is locked until the set is complete, so it is
			// premium for every purpose except the progress table itself.
			if (a_theme == Theme::kCustom) {
				return true;
			}
			for (std::size_t i = 0; i < kPremiumCount; ++i) {
				if (kPremium[i].theme == a_theme) {
					return true;
				}
			}
			return false;
		}

		std::uint64_t& ProgressStat(Unlock a_metric)
		{
			static std::uint64_t opens{ 0 };
			static std::uint64_t spent{ 0 };
			static std::uint64_t frees{ 0 };
			switch (a_metric) {
			case Unlock::kOpens: return opens;
			case Unlock::kSpent: return spent;
			default: return frees;
			}
		}

		// True when all three premium themes are earned - the custom theme's gate.
		bool AllThemesEarned()
		{
			for (std::size_t i = 0; i < kPremiumCount; ++i) {
				if (ProgressStat(kPremium[i].metric) < kPremium[i].goal) {
					return false;
				}
			}
			return true;
		}

		const char* MetricLabel(Unlock a_metric)
		{
			switch (a_metric) {
			case Unlock::kOpens: return "opens";
			case Unlock::kSpent: return "credits";
			default: return "free hauls";
			}
		}

		bool IsUnlocked(Theme a_theme)
		{
			if (a_theme == Theme::kCustom) {
				LoadProgress();
				return AllThemesEarned();
			}
			for (std::size_t i = 0; i < kPremiumCount; ++i) {
				if (kPremium[i].theme == a_theme) {
					return ProgressStat(kPremium[i].metric) >= kPremium[i].goal;
				}
			}
			return true;  // basics are never locked
		}

		void SaveProgress()
		{
			wchar_t modulePath[MAX_PATH]{};
			HMODULE self = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&SaveProgress), &self);
			GetModuleFileNameW(self, modulePath, MAX_PATH);
			const auto ini = std::filesystem::path(modulePath).parent_path() / kIniFileName;
			auto write = [&](const wchar_t* key, std::uint64_t value) {
				const auto text = std::to_wstring(value);
				WritePrivateProfileStringW(L"premium", key, text.c_str(), ini.c_str());
			};
			write(L"opens", ProgressStat(Unlock::kOpens));
			write(L"spent", ProgressStat(Unlock::kSpent));
			write(L"free", ProgressStat(Unlock::kFree));
		}

		void LoadProgress()
		{
			static bool loaded = false;
			if (loaded) {
				return;
			}
			loaded = true;
			wchar_t modulePath[MAX_PATH]{};
			HMODULE self = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&LoadProgress), &self);
			GetModuleFileNameW(self, modulePath, MAX_PATH);
			const auto ini = std::filesystem::path(modulePath).parent_path() / kIniFileName;
			ProgressStat(Unlock::kOpens) = GetPrivateProfileIntW(L"premium", L"opens", 0, ini.c_str());
			ProgressStat(Unlock::kSpent) = GetPrivateProfileIntW(L"premium", L"spent", 0, ini.c_str());
			ProgressStat(Unlock::kFree) = GetPrivateProfileIntW(L"premium", L"free", 0, ini.c_str());
			REX::INFO("SFShopExplorer: premium progress opens={} spent={} free={}", ProgressStat(Unlock::kOpens),
				ProgressStat(Unlock::kSpent), ProgressStat(Unlock::kFree));
		}

		// Record progress and announce anything that just unlocked.
		void AddProgress(Unlock a_metric, std::uint64_t a_amount)
		{
			LoadProgress();
			if (a_amount == 0) {
				return;
			}
			auto& value = ProgressStat(a_metric);
			for (std::size_t i = 0; i < kPremiumCount; ++i) {
				if (kPremium[i].metric == a_metric) {
					if (value < kPremium[i].goal && value + a_amount >= kPremium[i].goal) {
						const auto name = ActiveTheme(static_cast<int>(kPremium[i].theme)).name;
						REX::INFO("SFShopExplorer: UNLOCKED '{}' - {}", name, kPremium[i].line);
					}
				}
			}
			value += a_amount;
			SaveProgress();
		}

		// Combo label: locked themes show their progress instead of a bare name.
		std::string ThemeLabel(Theme a_theme)
		{
			const auto* const base = ActiveTheme(static_cast<int>(a_theme)).name;
			if (!IsPremium(a_theme) || IsUnlocked(a_theme)) {
				return base;
			}
			if (a_theme == Theme::kCustom) {
				std::size_t earned = 0;
				for (std::size_t i = 0; i < kPremiumCount; ++i) {
					earned += ProgressStat(kPremium[i].metric) >= kPremium[i].goal ? 1 : 0;
				}
				return std::format("{} [LOCKED {}/{} earned]", base, earned, kPremiumCount);
			}
			for (std::size_t i = 0; i < kPremiumCount; ++i) {
				if (kPremium[i].theme == a_theme) {
					const auto have = ProgressStat(kPremium[i].metric);
					return std::format("{} [LOCKED {}/{} {}]", base, have, kPremium[i].goal,
						MetricLabel(kPremium[i].metric));
				}
			}
			return base;
		}

	const UiTheme& ActiveTheme(int a_index)
		{
			if (a_index == static_cast<int>(Theme::kCustom)) {
				LoadCustomTheme();
				return Custom().ui;
			}
			static const UiTheme themes[] = {
				{ "Devilz Red", "shop_music_dark.wav", "",
					{ 0.73f, 0.15f, 0.20f, 1.0f }, { 0.95f, 0.28f, 0.32f, 1.0f }, { 0.78f, 0.58f, 0.25f, 1.0f },
					{ 0.58f, 0.55f, 0.55f, 1.0f }, { 0.93f, 0.89f, 0.86f, 1.0f }, { 0.025f, 0.022f, 0.028f, 0.985f },
					{ 0.055f, 0.045f, 0.052f, 0.96f }, { 0.10f, 0.055f, 0.065f, 0.96f }, { 0.33f, 0.12f, 0.15f, 0.8f },
					{ 0.34f, 0.08f, 0.11f, 0.85f }, { 0.60f, 0.12f, 0.17f, 0.9f },
					{ 0.25f, 0.07f, 0.10f, 0.9f }, { 0.55f, 0.12f, 0.16f, 0.95f },
					{ 0.11f, 0.07f, 0.08f, 1.0f }, { 0.28f, 0.10f, 0.13f, 1.0f },
					{ 0.20f, 0.09f, 0.11f, 1.0f }, { 0.07f, 0.05f, 0.06f, 0.5f },
					{ 0.48f, 0.42f, 0.42f, 1.0f }, { 0.30f, 0.12f, 0.14f, 0.9f } },
				{ "Void Blue", "shop_music_dark.wav", "",
					{ 0.16f, 0.38f, 0.75f, 1.0f }, { 0.35f, 0.60f, 0.95f, 1.0f }, { 0.80f, 0.70f, 0.35f, 1.0f },
					{ 0.55f, 0.58f, 0.62f, 1.0f }, { 0.88f, 0.91f, 0.96f, 1.0f }, { 0.020f, 0.025f, 0.040f, 0.985f },
					{ 0.045f, 0.055f, 0.095f, 0.96f }, { 0.055f, 0.090f, 0.160f, 0.96f }, { 0.12f, 0.22f, 0.42f, 0.8f },
					{ 0.10f, 0.18f, 0.38f, 0.85f }, { 0.16f, 0.30f, 0.60f, 0.9f },
					{ 0.07f, 0.12f, 0.26f, 0.9f }, { 0.12f, 0.22f, 0.46f, 0.95f },
					{ 0.07f, 0.10f, 0.18f, 1.0f }, { 0.12f, 0.18f, 0.30f, 1.0f },
					{ 0.10f, 0.15f, 0.26f, 1.0f }, { 0.06f, 0.09f, 0.16f, 0.5f },
					{ 0.40f, 0.44f, 0.50f, 1.0f }, { 0.10f, 0.18f, 0.32f, 0.9f } },
				{ "Star Gold", "shop_music_elevator.wav", "",
					{ 0.78f, 0.60f, 0.20f, 1.0f }, { 0.98f, 0.82f, 0.40f, 1.0f }, { 0.85f, 0.68f, 0.30f, 1.0f },
					{ 0.60f, 0.55f, 0.45f, 1.0f }, { 0.96f, 0.92f, 0.82f, 1.0f }, { 0.045f, 0.038f, 0.020f, 0.985f },
					{ 0.085f, 0.070f, 0.040f, 0.96f }, { 0.140f, 0.110f, 0.055f, 0.96f }, { 0.45f, 0.35f, 0.12f, 0.8f },
					{ 0.42f, 0.32f, 0.10f, 0.85f }, { 0.62f, 0.48f, 0.16f, 0.9f },
					{ 0.30f, 0.23f, 0.08f, 0.9f }, { 0.50f, 0.38f, 0.12f, 0.95f },
					{ 0.16f, 0.13f, 0.06f, 1.0f }, { 0.30f, 0.24f, 0.10f, 1.0f },
					{ 0.20f, 0.16f, 0.07f, 1.0f }, { 0.10f, 0.08f, 0.04f, 0.5f },
					{ 0.45f, 0.41f, 0.32f, 1.0f }, { 0.30f, 0.24f, 0.09f, 0.9f } },
				{ "Terminal Green", "shop_music_dark.wav", "",
					{ 0.15f, 0.75f, 0.35f, 1.0f }, { 0.35f, 0.95f, 0.55f, 1.0f }, { 0.60f, 0.85f, 0.45f, 1.0f },
					{ 0.45f, 0.58f, 0.48f, 1.0f }, { 0.85f, 0.95f, 0.88f, 1.0f }, { 0.015f, 0.030f, 0.018f, 0.985f },
					{ 0.035f, 0.060f, 0.042f, 0.96f }, { 0.055f, 0.100f, 0.065f, 0.96f }, { 0.10f, 0.35f, 0.18f, 0.8f },
					{ 0.08f, 0.28f, 0.14f, 0.85f }, { 0.14f, 0.45f, 0.22f, 0.9f },
					{ 0.06f, 0.18f, 0.10f, 0.9f }, { 0.10f, 0.32f, 0.16f, 0.95f },
					{ 0.06f, 0.12f, 0.08f, 1.0f }, { 0.10f, 0.22f, 0.13f, 1.0f },
					{ 0.08f, 0.16f, 0.10f, 1.0f }, { 0.04f, 0.08f, 0.05f, 0.5f },
					{ 0.32f, 0.42f, 0.34f, 1.0f }, { 0.08f, 0.20f, 0.11f, 0.9f } },
				// Paper Ledger: the only light theme. Warm cream stock, iron-gall
				// ink, oxblood stamps - the deliberate opposite of the other five,
				// which are all dark-on-dark.
				{ "Paper Ledger", "shop_music_elevator.wav", "",
					{ 0.55f, 0.13f, 0.12f, 1.0f }, { 0.75f, 0.20f, 0.16f, 1.0f }, { 0.45f, 0.34f, 0.14f, 1.0f },
					{ 0.45f, 0.42f, 0.36f, 1.0f }, { 0.13f, 0.12f, 0.10f, 1.0f }, { 0.94f, 0.92f, 0.87f, 1.0f },
					{ 0.90f, 0.88f, 0.82f, 1.0f }, { 0.86f, 0.83f, 0.76f, 1.0f }, { 0.52f, 0.47f, 0.39f, 0.7f },
					{ 0.84f, 0.79f, 0.68f, 1.0f }, { 0.79f, 0.71f, 0.56f, 1.0f },
					{ 0.55f, 0.13f, 0.12f, 1.0f }, { 0.68f, 0.18f, 0.16f, 1.0f },
					{ 0.97f, 0.96f, 0.93f, 1.0f }, { 0.99f, 0.98f, 0.95f, 1.0f },
					{ 0.76f, 0.70f, 0.59f, 1.0f }, { 0.885f, 0.865f, 0.80f, 1.0f },
					{ 0.60f, 0.57f, 0.51f, 1.0f }, { 0.58f, 0.53f, 0.44f, 0.8f } },
				// Hazard: industrial utility, deliberately NOT another dark-gold
				// theme. Heaven is navy-black with gold and this used to be
				// near-black with gold, so the two read as the same idea. It is
				// now a light steel-grey shell with hazard yellow only as a
				// signal accent - the value structure alone separates it.
				//
				// Hazard: industrial utility, deliberately NOT another dark-gold
				// theme. Heaven is navy-black with gold and this used to be
				// near-black with gold, so the two read as the same idea.
				//
				// Writing is signal red, and getting it readable took three goes:
				//
				//   1. ink 0.10 grey - near-black on a 0.24 panel, unreadable.
				//   2. ink 1.00,0.44,0.38 - readable but "too hot" at 3.22:1.
				//   3. ink 0.80,0.45,0.42 - desaturated, but only 2.59:1, still
				//      short of comfortable.
				//
				// The mistake in step 3 was treating this as an ink problem. On a
				// mid-grey panel no red reaches 4.5:1 without going neon, because
				// red is intrinsically dim: the fix is to darken the panel so the
				// same red has room. panel 0.285 -> 0.135 lifts ink to 5.4:1 and
				// muted to 4.5:1 while *reducing* apparent saturation, because
				// the darker surround stops the red vibrating against it.
				//
				// gold was the real casualty: at 0.20 grey on a 0.24 panel it
				// measured 1.03:1, which is why the TYPE/VALUE/WT/FORMID columns
				// vanished entirely. It is now a pale steel that holds ~7:1 -
				// deliberately NOT red, so the numeric columns stay scannable and
				// do not compete with the names.
				{ "Hazard", "shop_music_dark.wav", "",
					{ 0.98f, 0.78f, 0.06f, 1.0f }, { 1.00f, 0.90f, 0.24f, 1.0f }, { 0.72f, 0.77f, 0.82f, 1.0f },
					{ 0.74f, 0.47f, 0.44f, 1.0f }, { 0.86f, 0.52f, 0.48f, 1.0f }, { 0.105f, 0.112f, 0.125f, 1.0f },
					{ 0.135f, 0.142f, 0.155f, 1.0f }, { 0.170f, 0.178f, 0.192f, 1.0f }, { 0.30f, 0.32f, 0.34f, 0.9f },
					{ 0.185f, 0.193f, 0.208f, 1.0f }, { 0.250f, 0.260f, 0.278f, 1.0f },
					{ 0.930f, 0.740f, 0.060f, 1.0f }, { 1.000f, 0.870f, 0.180f, 1.0f },
					{ 0.160f, 0.168f, 0.180f, 1.0f }, { 0.225f, 0.235f, 0.252f, 1.0f },
					{ 0.205f, 0.213f, 0.228f, 1.0f }, { 0.128f, 0.135f, 0.148f, 1.0f },
					{ 0.52f, 0.40f, 0.38f, 1.0f }, { 0.62f, 0.48f, 0.08f, 0.9f } },
				// Starfield: nebula indigo, cyan core with a violet counter-accent,
				// deep enough to read as sky rather than UI chrome.
				{ "Starfield", "theme_starfield.wav", "starfield",
					{ 0.72f, 0.32f, 0.92f, 1.0f }, { 0.90f, 0.55f, 1.00f, 1.0f }, { 0.35f, 0.85f, 0.95f, 1.0f },
					{ 0.55f, 0.50f, 0.66f, 1.0f }, { 0.94f, 0.90f, 1.00f, 1.0f }, { 0.030f, 0.014f, 0.050f, 0.97f },
					{ 0.055f, 0.026f, 0.092f, 0.95f }, { 0.085f, 0.042f, 0.135f, 0.95f }, { 0.48f, 0.28f, 0.72f, 0.7f },
					{ 0.120f, 0.055f, 0.230f, 1.0f }, { 0.210f, 0.095f, 0.380f, 1.0f },
					{ 0.480f, 0.200f, 0.800f, 1.0f }, { 0.640f, 0.320f, 0.950f, 1.0f },
					{ 0.070f, 0.030f, 0.130f, 1.0f }, { 0.130f, 0.060f, 0.230f, 1.0f },
					{ 0.150f, 0.070f, 0.290f, 1.0f }, { 0.062f, 0.028f, 0.115f, 1.0f },
					{ 0.44f, 0.38f, 0.58f, 1.0f }, { 0.40f, 0.22f, 0.62f, 0.9f } },
				// Hell: inferno. Bone-white text on soot, ember red through molten
				// orange, soot-black borders so the fire reads as the only light.
				{ "Hell", "theme_hell.wav", "hell",
					{ 0.95f, 0.30f, 0.08f, 1.0f }, { 1.00f, 0.55f, 0.12f, 1.0f }, { 1.00f, 0.55f, 0.10f, 1.0f },
					{ 0.55f, 0.42f, 0.35f, 1.0f }, { 1.00f, 0.92f, 0.85f, 1.0f }, { 0.045f, 0.012f, 0.008f, 0.98f },
					{ 0.090f, 0.025f, 0.012f, 0.96f }, { 0.140f, 0.040f, 0.015f, 0.96f }, { 0.62f, 0.20f, 0.05f, 0.85f },
					{ 0.180f, 0.050f, 0.015f, 1.0f }, { 0.320f, 0.100f, 0.025f, 1.0f },
					{ 0.800f, 0.250f, 0.060f, 1.0f }, { 1.000f, 0.420f, 0.100f, 1.0f },
					{ 0.110f, 0.035f, 0.020f, 1.0f }, { 0.200f, 0.070f, 0.030f, 1.0f },
					{ 0.220f, 0.070f, 0.020f, 1.0f }, { 0.100f, 0.030f, 0.015f, 1.0f },
					{ 0.50f, 0.32f, 0.24f, 1.0f }, { 0.55f, 0.20f, 0.05f, 0.9f } },
				// Heaven: stained glass at night. Navy-black with radiant gold, the
				// one theme where gold is the primary rather than a highlight.
				{ "Heaven", "theme_heaven.wav", "heaven",
					{ 0.98f, 0.85f, 0.45f, 1.0f }, { 1.00f, 0.95f, 0.75f, 1.0f }, { 0.95f, 0.90f, 0.72f, 1.0f },
					{ 0.60f, 0.62f, 0.60f, 1.0f }, { 0.96f, 0.95f, 0.90f, 1.0f }, { 0.035f, 0.045f, 0.080f, 0.98f },
					{ 0.055f, 0.070f, 0.120f, 0.96f }, { 0.085f, 0.105f, 0.170f, 0.96f }, { 0.55f, 0.45f, 0.22f, 0.8f },
					{ 0.100f, 0.120f, 0.200f, 1.0f }, { 0.180f, 0.210f, 0.320f, 1.0f },
					{ 0.850f, 0.700f, 0.300f, 1.0f }, { 1.000f, 0.880f, 0.500f, 1.0f },
					{ 0.080f, 0.090f, 0.140f, 1.0f }, { 0.140f, 0.160f, 0.240f, 1.0f },
					{ 0.140f, 0.160f, 0.260f, 1.0f }, { 0.070f, 0.080f, 0.130f, 1.0f },
					{ 0.45f, 0.46f, 0.48f, 1.0f }, { 0.45f, 0.38f, 0.20f, 0.9f } },
			};
			constexpr int count = static_cast<int>(std::size(themes));
			return themes[std::clamp(a_index, 0, count - 1)];
		}

		// Window/panel alpha multiplier, [shop] alpha (0-100). Lets any theme be run
		// see-through so the game shows behind the shop, without the user having
		// to hand-edit 20 colour values. 100 = fully opaque (default).
	float LoadAlpha()
	{
		static float cached = -1.0f;
		if (cached < 0.0f) {
			HMODULE self = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&LoadAlpha), &self);
			wchar_t modulePath[MAX_PATH]{};
			GetModuleFileNameW(self, modulePath, MAX_PATH);
			const auto ini = std::filesystem::path(modulePath).parent_path() / kIniFileName;
			const int pct = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"shop", L"alpha", 60, ini.c_str())), 0, 100);
			cached = static_cast<float>(pct) / 100.0f;
			if (true) {
				REX::INFO("SFShopExplorer: [shop] alpha={} -> window alpha {:.2f}", pct, cached);
			}
		}
		return cached;
	}

		// Per-row type label for the All-Types view (the toolbar type is one
		// of many there, so the constant BrowseTypeName(selectedType) lies).
		const char* RowTypeName(const RE::TESForm* a_form)
		{
			if (!a_form) {
				return "?";
			}
			switch (std::to_underlying(a_form->GetFormType())) {
			case std::to_underlying(RE::FormType::kWEAP): return BrowseTypeName(BrowseType::kWeapon);
			case std::to_underlying(RE::FormType::kARMO): return BrowseTypeName(BrowseType::kArmor);
			case std::to_underlying(RE::FormType::kAMMO): return BrowseTypeName(BrowseType::kAmmo);
			case std::to_underlying(RE::FormType::kMISC): return BrowseTypeName(BrowseType::kMisc);
			case std::to_underlying(RE::FormType::kALCH): return BrowseTypeName(BrowseType::kAid);
			case std::to_underlying(RE::FormType::kBOOK): return BrowseTypeName(BrowseType::kBook);
			case std::to_underlying(RE::FormType::kKEYM): return BrowseTypeName(BrowseType::kKey);
			case std::to_underlying(RE::FormType::kINGR): return BrowseTypeName(BrowseType::kIngr);
			default: return "?";
			}
		}

		bool HasPlayerVisibleName(const RE::TESForm* a_form)
		{
			if (!a_form) {
				return false;
			}
			if (auto* full = starfield_cast<RE::TESFullName*>(const_cast<RE::TESForm*>(a_form)); full) {
				if (const char* name = full->GetFullName(); name && *name) {
					return true;
				}
			}
			return false;
		}

		// Ammo records a shop does not list. Three families, all ship/crate
		// hardware rather than a cartridge the player buys:
		//
		//   - the unnamed dummy forms Starfield registers per crate NIF
		//     ("(unnamed) [002184A8] · typee_xlarge");
		//   - the rounds that carry no model at all, because the engine draws
		//     them from the creature or weapon that fires them;
		//   - the ship magazines: missiles, torpedoes and ship/creature rounds,
		//     which the Ammo category already classifies as kMissile or
		//     kShipCreature, plus the ones whose only "model" is
		//     `testspaceshiprocketprojectile.nif` - the debug projectile, not a
		//     crate.
		//
		// Measured from Starfield.esm (150 AMMO records): 51 unnamed, 18 with an
		// empty MODL, 9 pointing at the test rocket, 27 missile-named - 105 in
		// all, leaving 45 real crates. Note MODL is the model: the audit dump's
		// "modelPath" for an AMMO record is NAM1, the spent shell casing, so an
		// empty value there does NOT mean the record has no mesh.
		//
		// Gated by Hide turrets rather than Hide test/debug: none of them carries
		// a test/debug marker, and all of them are ship/crate hardware.
		bool IsUnbuyableAmmo(const RE::TESForm* a_form)
		{
			if (!a_form || a_form->GetFormType() != RE::FormType::kAMMO) {
				return false;
			}
			const auto* ammo = a_form->As<RE::TESAmmo>();
			if (!ammo) {
				return false;
			}
			if (!HasPlayerVisibleName(a_form)) {
				return true;
			}
			const char* model = ammo->BGSModelMaterialSwap::model.c_str();
			if (model == nullptr || *model == '\0') {
				return true;
			}
			if (CaseFold(model).find("testspaceshiprocketprojectile") != std::string::npos) {
				return true;
			}
			const auto sub = ClassifyAmmoSub(CategorySearchText(a_form));
			return sub == AmmoSub::kMissile || sub == AmmoSub::kShipCreature;
		}


		// -------------------------------------------------------------------
		// Naming, search keys and model paths
		// -------------------------------------------------------------------
		std::string FormDisplayName(const RE::TESForm* a_form)
		{
			if (!a_form) {
				return "Unnamed form";
			}
			// One cleaned string feeds the row label, the search keys, the sort key
			// and the preview caption, so the padding is dropped once, here.
			if (auto* full = starfield_cast<RE::TESFullName*>(const_cast<RE::TESForm*>(a_form)); full) {
				if (const char* name = full->GetFullName(); name && *name) {
					if (auto trimmed = TrimNamePadding(SanitizeLabel(name)); !trimmed.empty()) {
						return trimmed;
					}
				}
			}
			if (const char* edid = a_form->GetFormEditorID(); edid && *edid) {
				if (auto trimmed = TrimNamePadding(SanitizeLabel(edid)); !trimmed.empty()) {
					return trimmed;
				}
			}
			return std::format("(unnamed) [{:08X}]", a_form->GetFormID());
		}

		std::string FormRowLabel(const RE::TESForm* a_form)
		{
			if (!a_form) {
				return "(unnamed form)";
			}
			const bool named = HasPlayerVisibleName(a_form);
			const auto model = FormModelPath(a_form);
			const auto slash = model.find_last_of("\\/");
			std::string modelName = slash == std::string::npos ? model : model.substr(slash + 1);
			const auto dot = modelName.find_last_of('.');
			if (dot != std::string::npos) {
				modelName.erase(dot);
			}

			std::string variant;
			switch (a_form->GetFormID()) {
			case 0x000A9010: variant = "Note 02 · BookLarge02"; break;
			case 0x000A9011: variant = "Note 03 · Notebook01"; break;
			case 0x000A9012: variant = "Note 01 · BookLarge02"; break;
			default: break;
			}

			const std::string name = FormDisplayName(a_form);
			if (!named) {
				if (!modelName.empty()) {
					return std::format("{} · {} · {:08X}", name, modelName, a_form->GetFormID());
				}
				return std::format("{} · {:08X}", name, a_form->GetFormID());
			}
			if (!variant.empty()) {
				return std::format("{} — {}", name, variant);
			}
			return name;
		}

		// One folded search key per row, when the catalog is rebuilt rather than
		// once per row per frame. The fields are the ones the item pane's search
		// used to probe separately - display label, editor ID, model path, model
		// file stem and the FormID in hex - joined with spaces so a key reads like
		// the text a player sees. The overlay's old shape built a display name, a
		// model path and two std::format calls for every one of 9,000 rows on
		// every frame the search box held a character.
		// The model path a record declares, with no assembly scan and no INI
		// skip filter. FormModelPath is the preview's answer, and for a
		// dummy-base weapon it runs CollectWeaponAssemblyNifs - the whole OMOD
		// family scan behind those "dummy weapon ... OMODs: N owner, 264 cast"
		// log lines. That is fine once for the row on screen; it is ruinous 202
		// times inside one search-key build, which is what made the first
		// keystroke after a list rebuild cost 450 ms ("9161 search key(s)
		// built", then "search 'a' ... in 450.32 ms" at 14:54:34). The search
		// box wants the string the record carries: the assembled part NIFs are
		// not shown in any row and are not what a player types. For a dummy
		// weapon the declared model is the dummy NIF, which is the word someone
		// troubleshooting would actually search for.
		std::string RawModelPath(const RE::TESForm* a_form)
		{
			if (!a_form) {
				return {};
			}
			auto* form = const_cast<RE::TESForm*>(a_form);
			switch (std::to_underlying(form->GetFormType())) {
			case std::to_underlying(RE::FormType::kARMO):
				if (auto* armor = form->As<RE::TESObjectARMO>()) {
					for (const auto& addon : armor->modelArray) {
						if (!addon.armorAddon) {
							continue;
						}
						const auto& female = addon.armorAddon->bipedModel[RE::SEX::kFemale].model;
						if (!female.empty()) {
							return female.c_str();
						}
						const auto& male = addon.armorAddon->bipedModel[RE::SEX::kMale].model;
						if (!male.empty()) {
							return male.c_str();
						}
					}
				}
				return {};
			case std::to_underlying(RE::FormType::kWEAP):
				if (auto* typed = form->As<RE::TESObjectWEAP>()) {
					return typed->BGSModelMaterialSwap::model.c_str();
				}
				return {};
			case std::to_underlying(RE::FormType::kAMMO):
				if (auto* typed = form->As<RE::TESAmmo>()) {
					if (!typed->BGSModelMaterialSwap::model.empty()) {
						return typed->BGSModelMaterialSwap::model.c_str();
					}
					return typed->shellCasing.model.c_str();
				}
				return {};
			case std::to_underlying(RE::FormType::kMISC):
			case std::to_underlying(RE::FormType::kKEYM):
				if (auto* typed = form->As<RE::TESObjectMISC>()) {
					return typed->BGSModelMaterialSwap::model.c_str();
				}
				return {};
			case std::to_underlying(RE::FormType::kALCH):
				if (auto* typed = form->As<RE::AlchemyItem>()) {
					return typed->BGSModelMaterialSwap::model.c_str();
				}
				return {};
			case std::to_underlying(RE::FormType::kBOOK):
				if (auto* typed = form->As<RE::TESObjectBOOK>()) {
					return typed->BGSModelMaterialSwap::model.c_str();
				}
				return {};
			default:
				return {};
			}
		}

		std::string SearchKeyFor(const RE::TESForm* a_form)
		{
			if (!a_form) {
				return {};
			}
			std::string key = CaseFold(FormDisplayName(a_form));
			if (const char* edid = a_form->GetFormEditorID(); edid && *edid) {
				key += ' ';
				key += CaseFold(edid);
			}
			const auto model = RawModelPath(a_form);
			if (!model.empty()) {
				const auto slash = model.find_last_of("\\/");
				key += ' ';
				key += CaseFold(model);
				key += ' ';
				key += CaseFold(slash == std::string::npos ? model : model.substr(slash + 1));
			}
			key += ' ';
			key += std::format("{:08X}", a_form->GetFormID());
			return key;
		}

		std::string FormPreviewLabel(const RE::TESForm* a_form)
		{
			if (!a_form) {
				return "Unnamed form";
			}
			return std::format("{} [FormID {:08X}]", FormRowLabel(a_form), a_form->GetFormID());
		}

		// Armor can wear several addons at once (body + sleeves + helmet ...),
		// each with its own NIF — and either sex slot may carry the model.
		// Return ALL of them (female first, male fallback per addon) so the
		// preview assembles the full item instead of the first hit. The
		// Mantis backpack (empty female slot) and the glove-bodied
		// "Deepseeker Spacesuit" (body + gloves addons) both come from here.
		// INI skiplist ([shop] skipmodels=a;b;c): matched forms (EDID,
		// model key or model path, casefolded, substring) render nothing —
		// the escape hatch for weird one-offs without killing the preview.
		std::unordered_set<std::string> g_skipModels;
		bool g_skipLoaded{ false };
		void EnsureSkipModels()
		{
			if (g_skipLoaded) {
				return;
			}
			g_skipLoaded = true;
			HMODULE self = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&EnsureSkipModels), &self);
			wchar_t modulePath[MAX_PATH]{};
			GetModuleFileNameW(self, modulePath, MAX_PATH);
			const auto ini = std::filesystem::path(modulePath).parent_path() / kIniFileName;
			wchar_t buf[4096]{};
			GetPrivateProfileStringW(L"shop", L"skipmodels", L"", buf, 4096, ini.c_str());
			std::wstring cur;
			for (const wchar_t* p = buf; ; ++p) {
				if (*p == L';' || *p == L'\0') {
					if (!cur.empty()) {
						g_skipModels.insert(CaseFold(NarrowUtf8(cur)));
						cur.clear();
					}
					if (*p == L'\0') {
						break;
					}
				} else {
					cur.push_back(*p);
				}
			}
			if (!g_skipModels.empty()) {
				REX::INFO("SFShopExplorer: skipping {} offline models by INI", g_skipModels.size());
			}
		}
		// [shop] theme persistence: DrawImGui loads the saved index once on
		// first open and the theme combo writes on every change, so the
		// chosen theme survives restarts.

		// -------------------------------------------------------------------
		// Per-form overrides and the INI accessors used before ShopBrowser
		// exists (theme/text size, skip-models, orientation)
		// -------------------------------------------------------------------
		std::filesystem::path ShopIniPath()
		{
			HMODULE self = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&ShopIniPath), &self);
			wchar_t modulePath[MAX_PATH]{};
			GetModuleFileNameW(self, modulePath, MAX_PATH);
			return std::filesystem::path(modulePath).parent_path() / kIniFileName;
		}

		// The custom theme's three settings and nineteen colours, all in the same
		// INI as the rest of the shop's preferences: keeping the file keeps the
		// look. Colours are written as "r,g,b,a" floats so a hand-edit is still
		// readable.
		void LoadCustomTheme()
		{
			auto& custom = Custom();
			if (custom.loaded) {
				return;
			}
			custom.loaded = true;
			// Seeded from a built-in rather than black, so the editor opens on a
			// palette worth adjusting instead of an empty canvas.
			custom.ui = ActiveTheme(static_cast<int>(Theme::kStarfield));
			custom.name = "Custom";
			custom.music = "shop_music_dark.wav";
			custom.backdrop.clear();
			custom.layout = Layout::kClassic;
			custom.pluginsWidth = 280.0f;
			custom.detailsWidth = 340.0f;

			const auto ini = ShopIniPath();
			auto       readString = [&ini](const wchar_t* a_key, const wchar_t* a_default) {
				wchar_t value[512]{};
				GetPrivateProfileStringW(L"custom", a_key, a_default, value, 512, ini.c_str());
				return NarrowUtf8(value);
			};
			if (const auto name = readString(L"name", L""); !name.empty()) {
				custom.name = name;
			}
			if (const auto music = readString(L"music", L""); !music.empty()) {
				custom.music = music;
			}
			custom.backdrop = readString(L"backdrop", L"");
			// Layout is a shop preference that happens to be edited here, so it
			// loads even when the custom colours are not in use: the workspace
			// reads it every frame whatever theme is selected.
			const auto layout = static_cast<int>(GetPrivateProfileIntW(L"custom", L"layout", 0, ini.c_str()));
			custom.layout = static_cast<Layout>(std::clamp(layout, 0, static_cast<int>(Layout::kTotal) - 1));
			custom.pluginsWidth = static_cast<float>(std::clamp(static_cast<int>(GetPrivateProfileIntW(L"custom", L"panewidth", 280, ini.c_str())), 180, 520));
			custom.detailsWidth = static_cast<float>(std::clamp(static_cast<int>(GetPrivateProfileIntW(L"custom", L"basketwidth", 340, ini.c_str())), 240, 620));
			for (const auto& colour : kCustomColours) {
				wchar_t keyWide[64]{};
				MultiByteToWideChar(CP_UTF8, 0, colour.key, -1, keyWide, 64);
				wchar_t value[128]{};
				GetPrivateProfileStringW(L"customcolours", keyWide, L"", value, 128, ini.c_str());
				float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
				if (swscanf_s(value, L"%f,%f,%f,%f", &r, &g, &b, &a) == 4) {
					custom.ui.*(colour.field) = ImVec4(std::clamp(r, 0.0f, 1.0f), std::clamp(g, 0.0f, 1.0f),
						std::clamp(b, 0.0f, 1.0f), std::clamp(a, 0.0f, 1.0f));
				}
			}
			SyncCustomThemeStrings();
		}

		void SaveCustomTheme()
		{
			auto&      custom = Custom();
			const auto ini = ShopIniPath();
			auto       writeString = [&ini](const wchar_t* a_key, const std::string& a_value) {
				wchar_t wide[MAX_PATH]{};
				if (MultiByteToWideChar(CP_UTF8, 0, a_value.c_str(), -1, wide, MAX_PATH) > 0) {
					WritePrivateProfileStringW(L"custom", a_key, wide, ini.c_str());
				}
			};
			writeString(L"name", custom.name);
			writeString(L"music", custom.music);
			writeString(L"backdrop", custom.backdrop);
			auto writeInt = [&ini](const wchar_t* a_key, int a_value) {
				const auto text = std::to_wstring(a_value);
				WritePrivateProfileStringW(L"custom", a_key, text.c_str(), ini.c_str());
			};
			writeInt(L"layout", static_cast<int>(custom.layout));
			writeInt(L"panewidth", static_cast<int>(std::lround(custom.pluginsWidth)));
			writeInt(L"basketwidth", static_cast<int>(std::lround(custom.detailsWidth)));
			for (const auto& colour : kCustomColours) {
				wchar_t keyWide[64]{};
				MultiByteToWideChar(CP_UTF8, 0, colour.key, -1, keyWide, 64);
				const auto& value = custom.ui.*(colour.field);
				const auto  text = std::format("{:.4f},{:.4f},{:.4f},{:.4f}", value.x, value.y, value.z, value.w);
				wchar_t     wide[128]{};
				if (MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide, 128) > 0) {
					WritePrivateProfileStringW(L"customcolours", keyWide, wide, ini.c_str());
				}
			}
			REX::INFO("SFShopExplorer: custom theme saved ('{}', {} colours)", custom.name, std::size(kCustomColours));
		}

		void ResetCustomTheme()
		{
			auto& custom = Custom();
			custom.ui = ActiveTheme(static_cast<int>(Theme::kStarfield));
			custom.name = "Custom";
			custom.music = "shop_music_dark.wav";
			custom.backdrop.clear();
			custom.layout = Layout::kClassic;
			custom.pluginsWidth = 280.0f;
			custom.detailsWidth = 340.0f;
			SyncCustomThemeStrings();
		}

		// Hard gate for every pre-native preview painter, so a test of the native
		// route cannot be confused by a picture the plugin itself produced.
		// One switch, not two: with only [offlinepreview] enabled=0 the
		// captured-scene route still paints, because it is the else-arm of "the
		// direct render is not current" - an ItemCapture game frame would then
		// appear exactly like a native result. Both painters are suppressed
		// together and the viewport reports the native dispatch instead.
		// [preview] legacyRenderer=0 disables; 1 (the default) restores.
		bool LegacyPreviewDisabled()
		{
			static const bool s_disabled = [] {
				const bool disabled = GetPrivateProfileIntW(L"preview", L"legacyRenderer", 1, ShopIniPath().c_str()) == 0;
				if (disabled) {
					REX::INFO("SFShopExplorer preview: legacy renderer DISABLED ([preview] legacyRenderer=0); no plugin-side render and no captured game frame will be drawn");
				}
				return disabled;
			}();
			return s_disabled;
		}
		void EnsureThemeIni(int& a_theme)
		{
			static bool s_loaded = false;
			if (s_loaded) {
				return;
			}
			s_loaded = true;
			const int saved = static_cast<int>(GetPrivateProfileIntW(L"shop", L"theme", static_cast<UINT>(a_theme), ShopIniPath().c_str()));
			a_theme = std::clamp(saved, 0, static_cast<int>(Theme::kTotal) - 1);
			if (saved != a_theme) {
				REX::INFO("SFShopExplorer: [shop] theme={} out of range; clamped", saved);
			}
		}
		void SaveThemeIni(int a_theme)
		{
			const auto value = std::to_wstring(a_theme);
			if (WritePrivateProfileStringW(L"shop", L"theme", value.c_str(), ShopIniPath().c_str())) {
				REX::INFO("SFShopExplorer: [shop] theme={} saved to INI", a_theme);
			} else {
				REX::WARN("SFShopExplorer: could not save [shop] theme to INI ({})", GetLastError());
			}
		}
		void EnsureTextSizeIni(int& a_size)
		{
			static bool s_loaded = false;
			if (s_loaded) {
				return;
			}
			s_loaded = true;
			const int saved = static_cast<int>(GetPrivateProfileIntW(L"shop", L"textSize", 1, ShopIniPath().c_str()));
			a_size = std::clamp(saved, 0, 3);
			if (saved != a_size) {
				REX::INFO("SFShopExplorer: [shop] textSize={} out of range; clamped", saved);
			}
		}
		void SaveTextSizeIni(int a_size)
		{
			const auto value = std::to_wstring(std::clamp(a_size, 0, 3));
			if (!WritePrivateProfileStringW(L"shop", L"textSize", value.c_str(), ShopIniPath().c_str())) {
				REX::WARN("SFShopExplorer: could not save [shop] textSize to INI ({})", GetLastError());
			}
		}
		bool SkipModelRequested(const RE::TESForm* a_form, const std::vector<std::string>& a_paths)
		{
			if (g_skipModels.empty() || !a_form) {
				return false;
			}
			const auto* edid = a_form->GetFormEditorID();
			std::string hay = edid ? CaseFold(edid) : std::string{};
			for (const auto& p : a_paths) {
				hay += '\x01';
				hay += CaseFold(p);
			}
			for (const auto& skip : g_skipModels) {
				if (!skip.empty() && hay.find(skip) != std::string::npos) {
					return true;
				}
			}
			return false;
		}
		bool DoVendorNotify(void* a_src, const void* a_ev)
		{
			struct BarterMenu_LoadModel{};
			auto* src = static_cast<RE::BSTEventSource<BarterMenu_LoadModel>*>(a_src);
			const auto* ev = static_cast<const BarterMenu_LoadModel*>(a_ev);
			__try { src->Notify(*ev); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		// The engine's own item-preview framing lives on the form, not in the menu:
		// TESBoundObject::previewTransform holds up to eight BGSTransform records
		// (position/rotation/scale/min-max zoom), one per preview context. The
		// BSMenu3D::SceneManager scene applies the record for its context when it
		// frames the item. The panel preview has always derived framing from the
		// model bounds instead, so record the authored values once per selected
		// form and compare them against the framing the direct render uses.
		// TESForm::Is<T>() compares T::FORMTYPE, and TESBoundObject declares
		// SF_RTTI_VTABLE but no SF_FORMTYPE - so As<TESBoundObject>() is nullptr
		// for every concrete bound form. That is not theoretical: the first run
		// with the placement reader logged the Razorback (00000FD6) as "is not a
		// TESBoundObject". Cast to the concrete class, which does carry a form
		// type, and let the upcast follow: every one of these derives
		// TESBoundObject at offset 0. Key forms need TESKey specifically - it
		// derives TESObjectMISC but reports KEYM, so the MISC cast is false for
		// it too. Types with no mapped class in the SDK (INGR) stay unmapped and
		// are named in the log instead of being reported as absent data.
		RE::TESBoundObject* BoundObjectOf(RE::TESForm* a_form)
		{
			if (!a_form) {
				return nullptr;
			}
			switch (std::to_underlying(a_form->GetFormType())) {
			case std::to_underlying(RE::FormType::kWEAP): return a_form->As<RE::TESObjectWEAP>();
			case std::to_underlying(RE::FormType::kARMO): return a_form->As<RE::TESObjectARMO>();
			case std::to_underlying(RE::FormType::kMISC): return a_form->As<RE::TESObjectMISC>();
			case std::to_underlying(RE::FormType::kKEYM): return a_form->As<RE::TESKey>();
			case std::to_underlying(RE::FormType::kBOOK): return a_form->As<RE::TESObjectBOOK>();
			case std::to_underlying(RE::FormType::kALCH): return a_form->As<RE::AlchemyItem>();
			case std::to_underlying(RE::FormType::kAMMO): return a_form->As<RE::TESAmmo>();
			default: return nullptr;
			}
		}
		void LogPreviewPlacement(const RE::TESForm* a_form)
		{
			if (!a_form) return;
			static std::uint32_t s_lastForm = 0;
			const std::uint32_t fid = a_form->GetFormID();
			if (fid == s_lastForm) return;
			s_lastForm = fid;
			auto* bound = BoundObjectOf(const_cast<RE::TESForm*>(a_form));
			if (!bound) {
				REX::INFO("SFShopExplorer preview-placement: {:08X} form type {} has no mapped bound-object class; no authored placement", fid, std::to_underlying(a_form->GetFormType()));
				return;
			}
			int emitted = 0;
			for (int ctx = 0; ctx < 8; ++ctx) {
				const auto* transform = bound->previewTransform.transforms[ctx];
				if (!transform) continue;
				const auto& d = transform->data;
				REX::INFO("SFShopExplorer preview-placement: {:08X} ctx={} pos=({:.3f},{:.3f},{:.3f}) rot=({:.3f},{:.3f},{:.3f}) scale={:.3f} zoom=[{:.3f},{:.3f}]",
					fid, ctx, d.position.x, d.position.y, d.position.z, d.rotation.x, d.rotation.y, d.rotation.z, d.scale, d.minZoom, d.maxZoom);
				++emitted;
			}
			if (emitted == 0) {
				REX::INFO("SFShopExplorer preview-placement: {:08X} has no BGSTransform entries; engine falls back to auto-fit", fid);
			}
		}
		// [preview] authoredOrientation=1 frames items with the engine's own
		// authored preview transform instead of the renderer's bound-derived
		// guess. Default off so one build can be compared A/B in game.
		bool AuthoredOrientationEnabled()
		{
			static const bool s_enabled = [] {
				const bool on = GetPrivateProfileIntW(L"preview", L"authoredOrientation", 0, ShopIniPath().c_str()) != 0;
				REX::INFO("SFShopExplorer preview: authored orientation {} ([preview] authoredOrientation)", on ? "ON" : "off");
				return on;
			}();
			return s_enabled;
		}
		// Authored item framing. Slot 4 is the general item transform -- 5,486
		// of the catalog's authored forms, the dominant one by far -- and slot 0
		// is the icon transform, used only when 4 is absent. Forms with neither
		// (14.7%, mostly weapons) keep the bounds-derived framing.
		struct AuthoredPlacement
		{
			bool  found{ false };
			int   slot{ -1 };
			float rot[3]{ 0.0f, 0.0f, 0.0f };
			float scale{ 1.0f };
		};
		AuthoredPlacement ReadAuthoredPlacement(const RE::TESForm* a_form)
		{
			AuthoredPlacement out;
			if (!a_form) return out;
			auto* bound = BoundObjectOf(const_cast<RE::TESForm*>(a_form));
			if (!bound) return out;
			for (const int slot : { 4, 0 }) {
				const auto* transform = bound->previewTransform.transforms[slot];
				if (!transform) continue;
				out.found = true;
				out.slot = slot;
				const auto& d = transform->data;
				out.rot[0] = d.rotation.x;
				out.rot[1] = d.rotation.y;
				out.rot[2] = d.rotation.z;
				out.scale = d.scale > 1e-4f ? d.scale : 1.0f;
				break;
			}
			return out;
		}
		// Rotation about the model's own centre. The order (Z, then Y, then X)
		// is the guess this build is testing: the authored angles are radians
		// and land on right angles (1.571, 3.142, 4.712), which is consistent
		// with radians but says nothing about order.
		void RotateAboutCenter(std::vector<float>& a_pos, const float a_center[3], const float a_rot[3])
		{
			const float cx = std::cos(a_rot[0]), sx = std::sin(a_rot[0]);
			const float cy = std::cos(a_rot[1]), sy = std::sin(a_rot[1]);
			const float cz = std::cos(a_rot[2]), sz = std::sin(a_rot[2]);
			for (std::size_t i = 0; i + 2 < a_pos.size(); i += 3) {
				const float x = a_pos[i] - a_center[0];
				const float y = a_pos[i + 1] - a_center[1];
				const float z = a_pos[i + 2] - a_center[2];
				const float x1 = cz * x - sz * y;
				const float y1 = sz * x + cz * y;
				const float x2 = cy * x1 + sy * z;
				const float z2 = -sy * x1 + cy * z;
				const float y3 = cx * y1 - sx * z2;
				const float z3 = sx * y1 + cx * z2;
				a_pos[i] = x2 + a_center[0];
				a_pos[i + 1] = y3 + a_center[1];
				a_pos[i + 2] = z3 + a_center[2];
			}
		}
		bool TryNativeVendorPreview(const RE::TESForm* a_form)
		{
			if (!a_form) return false;
			// The panel prints this result as "native dispatch: ok/FAILED", so it
			// has to be the outcome for the form on screen, not "a form was seen
			// once". Returning a bare true from the cached path did exactly the
			// latter: after a crash the notice read ok on the very next frame, and
			// a re-selected form read ok without dispatching at all. Keep the
			// per-form cache (one warning per form, no per-frame notify) but store
			// the result alongside it.
			// The menu state is part of the cache key: BarterMenu registers its
			// LoadModel sink when it opens, so a dispatch sent while it is closed
			// reaches nothing and must be repeated once the vendor is up -- even
			// though the selected form never changed.
			static std::uint32_t lastForm=0; static bool lastOk=false; static bool lastOpen=false;
			const std::uint32_t cur=a_form->GetFormID();
			const bool open = SFE::NativeModel::BarterMenuAvailable();
			if(cur==lastForm && open==lastOpen) return lastOk;
			lastForm=cur; lastOpen=open;
			if(!open) {
				REX::INFO("SFShopExplorer: native dispatch deferred for {:08X}; BarterMenu is not open", cur);
				lastOk=false; return false;
			}
			struct BarterMenu_LoadModel{};
			auto getSource=[]()->RE::BSTEventSource<BarterMenu_LoadModel>*{using func_t=RE::BSTEventSource<BarterMenu_LoadModel>*(*)(); static REL::Relocation<func_t> func{REL::ID(91377)}; return func();};
			auto* src=getSource();
			if(!src) { REX::WARN("SFShopExplorer: BarterMenu_LoadModel event source is NULL for {:08X}", cur); lastOk=false; return false; }
			struct RawVendorEvent{std::uint32_t handle{0}; std::uint32_t pad{0}; std::uint64_t extra{0};} ev{cur,0,0};
			const bool ok=DoVendorNotify(src,&ev);
			lastOk=ok;
			if(ok) REX::INFO("SFShopExplorer: native vendor preview dispatched for '{}' handle {:08X}",a_form->GetFormEditorID(),cur); else REX::WARN("SFShopExplorer: native vendor preview crashed for '{}'",a_form->GetFormEditorID());
			return ok;
		}
		template <typename PushFn>
		void CollectWeaponAssemblyNifs(const RE::TESForm* a_weapon, const PushFn& a_push);
		std::vector<std::string> FormModelPaths(const RE::TESForm* a_form)
		{
			std::vector<std::string> out;
			if (!a_form) {
				return out;
			}
			EnsureSkipModels();
			auto* form = const_cast<RE::TESForm*>(a_form);
			const auto pushUnique = [&out](const char* p) {
				if (p && p[0] && std::find(out.begin(), out.end(), p) == out.end()) {
					out.emplace_back(p);
				}
			};
			switch (std::to_underlying(form->GetFormType())) {
			case std::to_underlying(RE::FormType::kARMO):
				if (auto* armor = form->As<RE::TESObjectARMO>()) {
					for (const auto& addon : armor->modelArray) {
						if (addon.armorAddon) {
							const auto& female = addon.armorAddon->bipedModel[RE::SEX::kFemale].model;
							const auto& male = addon.armorAddon->bipedModel[RE::SEX::kMale].model;
							pushUnique(female.c_str());
							if (female.empty()) {
								pushUnique(male.c_str());
							}
						}
					}
				}
				break;
			case std::to_underlying(RE::FormType::kWEAP):
				if (auto* typed = form->As<RE::TESObjectWEAP>()) {
					// Placeholder receivers (BullpupDummyReceiver and friends)
					// carry no geometry (verified: notypes NIF, no BSGeometry)
					// and the engine assembles the real gun from its mods. Some
					// melee dummy housings carry P-Blade/P-Handle connect points;
					// retain that empty NIF as the assembly's placement root.
					// Dummy marker only: EMPTY-model rows are overwhelmingly
					// creature attacks (Unarmed*/Creature* WEAPs) — assembling
					// those from Starfield.esm OMODs previewed random base
					// guns (a stinger for a flyer head). They stay empty and
					// report "no 3D model" instead.
					const char* mp = typed->BGSModelMaterialSwap::model.c_str();
					if (mp && mp[0] && CaseFold(mp).find("dummy") == std::string::npos) {
						std::string foldedModel = CaseFold(mp);
						std::replace(foldedModel.begin(), foldedModel.end(), '\\', '/');
						if (foldedModel.find("/breechblock/") != std::string::npos) {
							// Breech Block's stock NIF is already its complete
							// receiver, but its standard barrel/grip/mag are separate
							// OMOD meshes. Put the receiver host first, then append
							// the standard picks resolved by the assembly scan.
							CollectWeaponAssemblyNifs(form, pushUnique);
							if (std::find(out.begin(), out.end(), mp) == out.end()) {
								out.insert(out.begin(), mp);
							}
						} else {
							pushUnique(mp);
						}
					} else if (mp && mp[0]) {
						CollectWeaponAssemblyNifs(form, pushUnique);
						// The dummy melee housing has no geometry, but its
						// P-Blade/P-Handle points place the OMOD meshes. Keep it
						// first only when a real assembly was found; otherwise the
						// empty connector would mask the normal no-model result.
						if (!out.empty() && CaseFold(mp).find("meleedummyhousing") != std::string::npos) {
							out.insert(out.begin(), mp);
						}
					}
				}
				break;
			case std::to_underlying(RE::FormType::kMISC):
				if (auto* typed = form->As<RE::TESObjectMISC>()) {
					pushUnique(typed->BGSModelMaterialSwap::model.c_str());
				}
				break;
			case std::to_underlying(RE::FormType::kAMMO):
				// Ammo box + shell casing assemble together (caseless rounds
				// simply carry no casing path and render the box alone).
				if (auto* typed = form->As<RE::TESAmmo>()) {
					pushUnique(typed->BGSModelMaterialSwap::model.c_str());
					pushUnique(typed->shellCasing.model.c_str());
				}
				break;
			case std::to_underlying(RE::FormType::kALCH):
				if (auto* typed = form->As<RE::AlchemyItem>()) {
					pushUnique(typed->BGSModelMaterialSwap::model.c_str());
				}
				break;
			case std::to_underlying(RE::FormType::kKEYM):
				// TESKey derives TESObjectMISC: same model carrier. Door keys
				// and keycards carry no model string of their own, so render
				// the generic keycard NIF as an honest stand-in (logged once
				// per session per key below in DrawImGui's Model: line —
				// the path on display names the stand-in).
				if (auto* typed = form->As<RE::TESObjectMISC>()) {
					pushUnique(typed->BGSModelMaterialSwap::model.c_str());
				}
				if (out.empty()) {
					out.emplace_back("SetDressing\\Keycards\\KeycardGen01.nif");
				}
				break;
			case std::to_underlying(RE::FormType::kBOOK):
				if (auto* typed = form->As<RE::TESObjectBOOK>()) {
					pushUnique(typed->BGSModelMaterialSwap::model.c_str());
				}
				break;
			default:
				break;				}
				if (!out.empty() && SkipModelRequested(a_form, out)) {
				static std::unordered_set<std::uint32_t> logged;
				if (logged.insert(a_form->GetFormID()).second) {
					REX::INFO("SFShopExplorer: offline model skipped by INI for '{}'", a_form->GetFormEditorID());
				}
				out.clear();
			}
			return out;
		}

		std::string FormModelPath(const RE::TESForm* a_form)
		{
			const auto paths = FormModelPaths(a_form);
			return paths.empty() ? std::string{} : paths.front();
		}

		std::uint32_t PlayerCredits(RE::PlayerCharacter* a_player)
		{
			if (!a_player) {
				return 0;
			}
			std::uint32_t credits = 0;
			auto guard = a_player->inventoryList.LockRead();
			const auto* inventory = *guard;
			if (!inventory) {
				return 0;
			}
			for (const auto& item : inventory->data) {
				if (item.object && item.object->GetFormID() == 0x0000000F) {
					for (const auto& stack : item.stacks) {
						if (stack.count > 0) {
							credits += static_cast<std::uint32_t>(stack.count);
						}
					}
				}
			}
			return credits;
		}

		std::string RuntimeOwnerOfFormID(const RE::TESDataHandler* a_data, const std::uint32_t a_formID)
		{
			if (!a_data) {
				return {};
			}
			const auto top = static_cast<std::uint8_t>(a_formID >> 24);
			const auto middle = static_cast<std::uint32_t>((a_formID >> 12) & 0x0FFF);
			if (top == 0xFE || top == 0xFD) {
				const auto& files = top == 0xFE ? a_data->compiledFileCollection.smallFiles : a_data->compiledFileCollection.mediumFiles;
				if (middle < files.size() && files[middle] && files[middle]->fileName[0]) {
					return files[middle]->fileName;
				}
				return {};
			}
			for (const auto* file : a_data->compiledFileCollection.files) {
				if (file && file->compileIndex == top && file->fileName[0]) {
					return file->fileName;
				}
			}
			return {};
		}

		// CommonLibSF's typed TESDataHandler::formArrays member is not laid out
		// at the runtime offset used by this Starfield build. The older
		// DevilzESMShop/SFDevilzDump implementation found the live arrays by
		// validating the raw BSTArray headers — that content-driven heuristic is
		// the PROVEN path and stays primary (the 01:05 attempt that scanned by
		// declared offsets 0x070..0x1498 broke every tab at runtime: the real
		// stride/origin evidently differs from the declaration, so offset-driven
		// reading misaligned everything). Only two deltas from the original:
		// 1) a null element[0] no longer disqualifies an array — probe forward
		//    to the first live element (arrays with a null first slot were
		//    silently dropped); BuildFormListForType still filters each element
		//    by exact form type, so a probe cannot inject wrong-typed rows.
		// 2) one INFO line per discovered array so future regressions are
		//    diagnosable from the log instead of a blank tab.
		std::unordered_map<std::uint32_t, std::uintptr_t> RefreshRuntimeFormArrays(const RE::TESDataHandler* a_data)
		{
			std::unordered_map<std::uint32_t, std::uintptr_t> arrays;
			if (!a_data) {
				return arrays;
			}
			const auto base = std::bit_cast<std::uintptr_t>(a_data);
			// The form arrays live on the data handler and never move during a
			// session, so the 0x0E0..0x1510 walk runs once. It used to run on
			// every BuildFormList and log one line per type: 27,030 of the
			// 35,199 lines in the 2026-10-24 session log were these INFO lines,
			// and every list rebuild paid ~400 window probes for nothing.
			static std::mutex cacheMutex;
			static std::uintptr_t cachedBase = 0;
			static std::unordered_map<std::uint32_t, std::uintptr_t> cached;
			{
				std::scoped_lock cacheLock(cacheMutex);
				if (cachedBase == base && !cached.empty()) {
					return cached;
				}
			}
			for (std::uintptr_t offset = 0x0E0; offset < 0x1510; offset += 0x18) {
				const auto size = *reinterpret_cast<const std::uint32_t*>(base + offset);
				const auto capacity = *reinterpret_cast<const std::uint32_t*>(base + offset + 4);
				const auto data = *reinterpret_cast<const std::uintptr_t*>(base + offset + 8);
				if (size == 0 || size > 200000 || capacity < size) {
					continue;
				}
				if (data < 0x10000000000 || data >= 0x800000000000) {
					continue;
				}
				const auto* elements = reinterpret_cast<const RE::TESForm* const*>(data);
				const RE::TESForm* first = nullptr;
				const auto probe = std::min<std::uint32_t>(size, 8);
				for (std::uint32_t index = 0; index < probe && !first; ++index) {
					const auto* form = elements[index];
					if (!form) {
						continue;
					}
					const auto vtable = *reinterpret_cast<const std::uintptr_t*>(form);
					if (vtable < 0x7FF000000000 || vtable >= 0x800000000000) {
						break;
					}
					first = form;
				}
				if (!first) {
					continue;
				}
				const auto type = static_cast<std::uint32_t>(std::to_underlying(first->GetFormType()));
				if (!arrays.contains(type)) {
					arrays.emplace(type, offset);
					// One line per form type, ~50 per session, useful only when the
					// table layout is what is being investigated: DEBUG.
					REX::DEBUG("SFShopExplorer: form array for type {} found at offset 0x{:X} ({} entries)", type, offset, size);
				}
			}
			{
				std::scoped_lock cacheLock(cacheMutex);
				cachedBase = base;
				cached = arrays;
			}
			return arrays;
		}

		void BuildFormListForType(RE::TESDataHandler* a_data, RE::FormType a_type, std::vector<RE::TESForm*>& a_out,
			bool a_boundOnly = true)
		{
			a_out.clear();
			if (!a_data) {
				return;
			}
			const auto type = static_cast<std::uint32_t>(std::to_underlying(a_type));
			if (type >= static_cast<std::uint32_t>(std::to_underlying(RE::FormType::kTotal))) {
				return;
			}
			const auto arrays = RefreshRuntimeFormArrays(a_data);
			const auto array = arrays.find(type);
			if (array == arrays.end()) {
				return;
			}				const auto base = std::bit_cast<std::uintptr_t>(a_data);
				const auto offset = array->second;
				const auto size = *reinterpret_cast<const std::uint32_t*>(base + offset);

			const auto data = *reinterpret_cast<const std::uintptr_t*>(base + offset + 8);
			if (size == 0 || size > 200000 || data < 0x10000000000 || data >= 0x800000000000) {
				return;
			}
			const auto* elements = reinterpret_cast<const RE::TESForm* const*>(data);
			for (std::uint32_t index = 0; index < size; ++index) {
				const auto* form = elements[index];
				// OMODs are collected unbound (they are not bound objects);
				// everything else keeps the bound-object gate.
				if (form && (!a_boundOnly || form->IsBoundObject()) &&
					static_cast<std::uint32_t>(std::to_underlying(form->GetFormType())) == type) {
					a_out.push_back(const_cast<RE::TESForm*>(form));
				}
			}
		}

		// Dummy-base weapons (BullpupDummyReceiver and friends carry no
		// geometry): the engine assembles the real gun from its OMODs.
		// Assemble receiver + one default per slot, restricted to ONE
		// family: the receiver's own NIF directory anchors it (AA-43 all
		// lives in aa43_eva/; a Cathode must not wear another gun's parts
		// that happen to share its plugin file). Slot identity comes from
		// EDID/display/NIF/attach text; defaults prefer "standard", else
		// first-seen. "No Mod"/quality/modgroup OMODs carry no model and
		// drop out on their own. No receiver anchor => no assembly (an
		// honest unavailable beats a franken-gun). Capped at 11 NIFs.
		// Session-cached per weapon (the preview calls FormModelPaths per
		// frame).
		std::unordered_map<std::uint32_t, std::vector<std::string>> g_assemblyCache;
		std::mutex g_assemblyMutex;
		// One default-assembly pick (slot + the OMOD that fills it),
		// recorded beside g_assemblyCache so a purchase can attach exactly
		// what the preview renders (build 85 spike).
		struct AssemblyPick
		{
			std::string   slot;
			std::uint32_t fid{ 0 };
		};
		std::unordered_map<std::uint32_t, std::vector<AssemblyPick>> g_assemblyPicks;
		// Phase 2 selector UI: per-weapon parts catalog built during the
		// assembly scan - every slot's options (the dropdown contents)
		// plus the family's no-model tier OMODs (rarity). Choices are
		// the user's current picks; g_choiceVersion invalidates the
		// resolved cache/picks whenever one changes.
		struct SlotOption
		{
			std::string   label;
			std::string   nif;
			std::uint32_t fid{ 0 };
			bool          standard{ false };
		};
		struct TierOption
		{
			std::string   label;
			std::uint32_t fid{ 0 };
			// Build 94: 0 for plain/legacy entries; 1-4 for quality
			// tiers (Common/Calibrated/Refined/Advanced); 1-3 for the
			// legendary-effect pool entries (roll rank).
			std::uint32_t rank{ 0 };
		};
		// Rarity sentinels: Rare/Epic/Legendary are not OMOD forms - they
		// roll 1/2/3 legendary-effect OMODs at assembly resolution time.
		// Kept separate from the weapon's Common/Calibrated/Refined/Advanced
		// quality OMOD, which is independently selected and attached.
		inline constexpr std::uint32_t kRaritySentinelBase = 0xFF000000u;
		inline constexpr std::uint32_t kRaritySentinelRare = kRaritySentinelBase + 1u;
		inline constexpr std::uint32_t kRaritySentinelEpic = kRaritySentinelBase + 2u;
		inline constexpr std::uint32_t kRaritySentinelLegendary = kRaritySentinelBase + 3u;
		struct WeaponCatalog
		{
			std::vector<std::string> slotOrder;
			std::unordered_map<std::string, std::vector<SlotOption>> slots;
			std::unordered_map<std::string, std::uint32_t> defaultFid;
			std::vector<TierOption> qualities;
			// Optional in-game legendary effects, kept separate from item quality.
			std::vector<TierOption> effects;
		};
		std::unordered_map<std::uint32_t, WeaponCatalog> g_assemblyCatalogs;  // g_assemblyMutex
		struct WeaponChoice
		{
			std::unordered_map<std::string, std::uint32_t> slotFid;
			std::uint32_t qualityFid{ 0 };
			std::uint32_t rarityFid{ 0 };
		};
		std::unordered_map<std::uint32_t, WeaponChoice> g_weaponChoice;       // g_assemblyMutex
		std::atomic<std::uint32_t> g_choiceVersion{ 1 };
		std::unordered_map<std::uint32_t, std::uint32_t> g_assemblyBuiltVer;  // g_assemblyMutex

		std::string OptionLabel(const RE::TESForm* a_form, const std::string& a_mesh)
		{
			std::string s = FormDisplayName(a_form);
			if (s.empty() && a_form) {
				if (const char* edid = a_form->GetFormEditorID(); edid && edid[0]) {
					s = edid;
				}
			}
			if (s.empty() && !a_mesh.empty()) {
				const auto slash = a_mesh.find_last_of("\\/");
				const std::string base = slash == std::string::npos ? a_mesh : a_mesh.substr(slash + 1);
				const auto dot = base.find_last_of('.');
				s = dot == std::string::npos ? base : base.substr(0, dot);
			}
			if (s.empty() && a_form) {
				s = std::format("{:08X}", a_form->GetFormID());
			}
			return s;
		}
		void SetWeaponSlotChoice(std::uint32_t a_wid, const std::string& a_slot, std::uint32_t a_fid)
		{
			{
				std::scoped_lock lock(g_assemblyMutex);
				g_weaponChoice[a_wid].slotFid[a_slot] = a_fid;
			}
			g_choiceVersion.fetch_add(1, std::memory_order_acq_rel);
		}
		void SetWeaponQualityChoice(std::uint32_t a_wid, std::uint32_t a_fid)
		{
			{
				std::scoped_lock lock(g_assemblyMutex);
				g_weaponChoice[a_wid].qualityFid = a_fid;
			}
			g_choiceVersion.fetch_add(1, std::memory_order_acq_rel);
		}
		void SetWeaponRarityChoice(std::uint32_t a_wid, std::uint32_t a_fid)
		{
			{
				std::scoped_lock lock(g_assemblyMutex);
				g_weaponChoice[a_wid].rarityFid = a_fid;
			}
			g_choiceVersion.fetch_add(1, std::memory_order_acq_rel);
		}
		// Armor rarity. A weapon's effect pool is discovered by walking its own
		// OMOD family during assembly; armor has no assembly pass at all, so its
		// pool is the generated per-class table instead: suit/apparel body,
		// helmet/hat, backpack. Vanilla keeps those apart (a helmet can only
		// roll helmet effects), so the class gate is what makes the attach land
		// on the right slot. The draw is made once, when the rarity is picked,
		// and kept with the form so the preview, the basket line and the
		// delivery all carry the same effects.
		struct ArmorRarityChoice
		{
			std::uint32_t              rarityFid{ 0 };
			std::vector<std::uint32_t> roll;
		};
		std::unordered_map<std::uint32_t, ArmorRarityChoice> g_armorRarity;  // g_assemblyMutex

		// 0 = body, 1 = helmet, 2 = backpack, 3 = not an armor form (no pool).
		std::uint32_t ArmorLegendarySlot(const RE::TESForm* a_form)
		{
			if (!a_form || a_form->GetFormType() != RE::FormType::kARMO) {
				return 3u;
			}
			// The split hat/accessory buckets keep the head-slot pool they had
			// before the filter was divided (1 = helmet). Changing which pool a
			// hat rolls from is not part of a filter change.
			switch (ClassifyArmorSub(CategorySearchText(a_form))) {
			case ArmorSub::kHelmet:
			case ArmorSub::kHat:
			case ArmorSub::kAccessory: return 1u;
			case ArmorSub::kPack: return 2u;
			default: return 0u;
			}
		}

		const std::vector<TierOption>& ArmorLegendaryPool(std::uint32_t a_slot)
		{
			static std::vector<TierOption> pools[3];
			static std::once_flag once[3];
			static const std::vector<TierOption> kNoPool;
			if (a_slot >= 3) {
				return kNoPool;
			}
			std::call_once(once[a_slot], [a_slot]() {
				for (const auto& entry : SFE::kOmodArmorLegendaryTable) {
					if (entry.slot != a_slot) {
						continue;
					}
					auto* form = RE::TESForm::LookupByID(entry.omod);
					if (form) {
						pools[a_slot].push_back(TierOption{ OptionLabel(form, ""), entry.omod, entry.rank });
					}
				}
				REX::INFO("SFShopExplorer: armor legendary pool slot={} -> {} effects", a_slot, pools[a_slot].size());
			});
			return pools[a_slot];
		}

		void SetArmorRarityChoice(std::uint32_t a_formID, std::uint32_t a_rarityFid, std::uint32_t a_slot)
		{
			ArmorRarityChoice choice{ a_rarityFid, {} };
			if (a_rarityFid >= kRaritySentinelBase + 1u && a_rarityFid <= kRaritySentinelBase + 3u) {
				const std::uint32_t nRoll = a_rarityFid - kRaritySentinelBase;
				std::vector<std::uint32_t> pool;
				for (const auto& effect : ArmorLegendaryPool(a_slot)) {
					if (effect.rank >= 1u && effect.rank <= nRoll) {
						pool.push_back(effect.fid);
					}
				}
				if (pool.empty()) {
					// Backpacks publish no rank-1 effect at all (all three are rank
					// 2-3), so a "Rare" pick would otherwise promise one effect and
					// attach none. Fall back to the class's published effects.
					for (const auto& effect : ArmorLegendaryPool(a_slot)) {
						pool.push_back(effect.fid);
					}
				}
				if (!pool.empty()) {
					static std::mt19937 rng{ std::random_device{}() };
					std::shuffle(pool.begin(), pool.end(), rng);
					if (pool.size() > nRoll) {
						pool.resize(nRoll);
					}
					choice.roll = pool;
					std::string rollIds;
					for (const auto fid : pool) {
						if (!rollIds.empty()) {
							rollIds += ',';
						}
						rollIds += std::format("{:08X}", fid);
					}
					REX::INFO("SFShopExplorer: armor rarity roll {:08X} slot={} -> {} effects [{}]",
						a_formID, a_slot, nRoll, rollIds);
				}
			}
			{
				std::scoped_lock lock(g_assemblyMutex);
				g_armorRarity.insert_or_assign(a_formID, std::move(choice));
			}
		}
		// One basket line per configuration: the weapon assembly picks, or the
		// armor rarity roll. Both are captured at ADD time, never re-rolled at
		// checkout.
		std::vector<std::uint32_t> SelectedOmodsFor(RE::TESForm* a_form)
		{
			std::vector<std::uint32_t> selected;
			if (!a_form) {
				return selected;
			}
			const auto formID = a_form->GetFormID();
			std::scoped_lock lock(g_assemblyMutex);
			if (const auto it = g_assemblyPicks.find(formID); it != g_assemblyPicks.end()) {
				selected.reserve(it->second.size());
				for (const auto& pick : it->second) {
					if (pick.fid != 0) {
						selected.push_back(pick.fid);
					}
				}
			}
			if (a_form->GetFormType() == RE::FormType::kARMO) {
				if (const auto it = g_armorRarity.find(formID); it != g_armorRarity.end()) {
					selected.insert(selected.end(), it->second.roll.begin(), it->second.roll.end());
				}
			}
			return selected;
		}
		// Family coherence: an assembly must be ONE gun, not one slot per
		// random family (every Starfield dummy funneled into M1919 parts).
		// Kept words (>=3 chars, non-generic) from folded text; a weapon
		// token and an OMOD word match when either contains the other.

		// -------------------------------------------------------------------
		// Weapon OMOD assembly: which parts a gun is made of, and in what
		// order they attach
		// -------------------------------------------------------------------
		bool AssemblyWord(const std::string& a_word)
		{
			if (a_word.size() < 3) {
				return false;
			}
			// DLC/pack prefixes (sfbgs001, sfta06, s_40) span whole files,
			// never one gun.
			if ((a_word.compare(0, 5, "sfbgs") == 0 || a_word.compare(0, 4, "sfta") == 0) &&
				a_word.find_first_not_of("0123456789", 5) == std::string::npos) {
				return false;
			}
			if ((a_word[0] == 's' && a_word[1] == '_' ) ||
				a_word.find_first_not_of("0123456789") == std::string::npos) {
				return false;
			}
			static const char* kStop[] = { "mod", "gun", "weapon", "pistol", "rifle", "shotgun", "smg",
				"sniper", "scope", "sight", "optic", "reflex", "laser", "light", "lamp", "glow", "dot",
				"barrel", "muzzle", "brake", "compensator", "suppressor", "supressor", "silencer",
				"mag", "magazine", "drum", "clip", "ammo", "stock", "grip", "trigger", "internal",
				"receiver", "bolt", "action", "knife", "sword", "blade", "handle", "frame", "body",
				"standard", "tactical", "null", "empty", "none", "no", "ui", "the", "and", "for",
				"with", "old", "elite", "crew", "only", "noclutter", "research", "companion",
				"full", "fully", "semi", "auto", "single", "double", "shot", "burst", "lever",
				"pump", "break", "top", "side", "under", "over", "mark", "hand", "guard",
				"mkii", "mkiii", "mki", "mk2", "mk3", "sfbgs", "sfta", "tes", "esd", "esm", "cc", "sw", "dlc", "ng", "xb", "ps" };
			for (const char* s : kStop) {
				if (a_word == s) {
					return false;
				}
			}
			return true;
		}
		void SplitWords(const std::string& a_folded, std::vector<std::string>& a_out)
		{
			std::size_t i = 0;
			while (i <= a_folded.size()) {
				while (i < a_folded.size() && !((a_folded[i] >= 'a' && a_folded[i] <= 'z') ||
					(a_folded[i] >= '0' && a_folded[i] <= '9'))) {
					++i;
				}
				std::size_t j = i;
				while (j < a_folded.size() && ((a_folded[j] >= 'a' && a_folded[j] <= 'z') ||
					(a_folded[j] >= '0' && a_folded[j] <= '9'))) {
					++j;
				}
				if (j > i) {
					const std::string tok = a_folded.substr(i, j - i);
					if (AssemblyWord(tok) && std::find(a_out.begin(), a_out.end(), tok) == a_out.end()) {
						a_out.push_back(tok);
					}
				}
				i = j + 1;
			}
		}
		// Slot key from folded text, "" when unknown. Receiver first: a
		// receiver NIF never carries other slot words.
		std::string WeaponPartSlot(const std::string& a_folded)
		{
			if (a_folded.find("receiver") != std::string::npos) {
				return "receiver";
			}
			if (a_folded.find("blade") != std::string::npos) {
				return "blade";
			}
			if (a_folded.find("barrel") != std::string::npos) {
				return "barrel";
			}
			if (a_folded.find("muzzle") != std::string::npos || a_folded.find("brake") != std::string::npos ||
				a_folded.find("compensator") != std::string::npos || a_folded.find("suppressor") != std::string::npos ||
				a_folded.find("silencer") != std::string::npos) {
				return "muzzle";
			}
			if (a_folded.find("magazine") != std::string::npos || a_folded.find("mag_") != std::string::npos ||
				a_folded.find("_mag") != std::string::npos || a_folded.find("drum") != std::string::npos ||
				a_folded.find("clip") != std::string::npos) {
				return "mag";
			}
			if (a_folded.find("stock") != std::string::npos) {
				return "stock";
			}
			if (a_folded.find("handle") != std::string::npos) {
				return "handle";
			}
			if (a_folded.find("grip") != std::string::npos || a_folded.find("foregrip") != std::string::npos) {
				return "grip";
			}
			if (a_folded.find("coating") != std::string::npos) {
				return "coating";
			}
			if (a_folded.find("scope") != std::string::npos || a_folded.find("sight") != std::string::npos ||
				a_folded.find("optic") != std::string::npos || a_folded.find("reflex") != std::string::npos ||
				a_folded.find("iron") != std::string::npos) {
				return "scope";
			}
			if (a_folded.find("laser") != std::string::npos) {
				return "laser";
			}
			if (a_folded.find("battery") != std::string::npos || a_folded.find("cell") != std::string::npos) {
				return "battery";
			}
			if (a_folded.find("cover") != std::string::npos) {
				return "cover";
			}
			if (a_folded.find("trigger") != std::string::npos || a_folded.find("internal") != std::string::npos) {
				return "internal";
			}
			return {};
		}
		bool IsMandatoryDummyAssemblySlot(const std::string& a_slot, const bool a_meleeDummy)
		{
			return a_slot == "receiver" ||
				(a_meleeDummy && (a_slot == "blade" || a_slot == "handle"));
		}
		struct MnamScanResult
		{
			std::uint32_t ptrSlot;
			std::uint32_t rawSlot;
		};
		// One-time MNAM member hunt (build 78). Scans the runtime Mod
		// object for shotty OMOD 000079CD's MNAM keyword 001F70BB: as a
		// raw u32 anywhere, or behind any pointer-looking member (+0x28 =
		// TESForm::formID). POD-only under __try: every deref of a
		// plausible-but-wrong pointer is swallowed per slot.
		static MnamScanResult ScanForMnam(const void* a_obj)
		{
			MnamScanResult r{ 0xFFFFFFFFu, 0xFFFFFFFFu };
			const auto* base = static_cast<const std::uint8_t*>(a_obj);
			if (!base) {
				return r;
			}
			for (std::uint32_t o = 0x30; o + 8 <= 0x108; o += 4) {
				__try {
					const auto raw32 = *reinterpret_cast<const std::uint32_t*>(base + o);
					if (raw32 == 0x001F70BBu) {
						r.rawSlot = o;
					}
					const auto v = *reinterpret_cast<const std::uint64_t*>(base + o);
					if (v >= 0x100000000ull && v < 0x0000800000000000ull && (v & 7) == 0) {
						if (*reinterpret_cast<const std::uint32_t*>(v + 0x28) == 0x001F70BBu) {
							r.ptrSlot = o;
						}
					}
				} __except (EXCEPTION_EXECUTE_HANDLER) {
				}
			}
			return r;
		}
		template <typename PushFn>
		void CollectWeaponAssemblyNifs(const RE::TESForm* a_weapon, const PushFn& a_push)
		{
			if (!a_weapon) {
				return;
			}
			const std::uint32_t wid = a_weapon->GetFormID();
			const auto ver = g_choiceVersion.load(std::memory_order_acquire);
			WeaponChoice choice;
			{
				std::scoped_lock lock(g_assemblyMutex);
				const auto cached = g_assemblyCache.find(wid);
				const auto builtVer = g_assemblyBuiltVer.find(wid);
				if (cached != g_assemblyCache.end() && builtVer != g_assemblyBuiltVer.end() &&
					builtVer->second == ver) {
					for (const auto& p : cached->second) {
						a_push(p.c_str());
					}
					return;
				}
				if (const auto ch = g_weaponChoice.find(wid); ch != g_weaponChoice.end()) {
					choice = ch->second;
				}
			}
			// Dummy-base weapons preview through the assembly; a real-model
			// weapon previews its own NIF and carries only explicit user
			// choices (defaults must not silently attach parts the preview
			// never shows). Breech Block is an exception: its stock NIF is
			// already a complete receiver but the crafting parts are separate
			// OMODs that must be included in the preview assembly.
			bool dummyBase = false;
			bool meleeDummy = false;
			bool breechBlockAssembly = false;
			if (auto* typed = a_weapon->As<RE::TESObjectWEAP>()) {
				const char* bmp = typed->BGSModelMaterialSwap::model.c_str();
				std::string foldedBase = bmp ? CaseFold(bmp) : std::string{};
				std::replace(foldedBase.begin(), foldedBase.end(), '\\', '/');
				dummyBase = !foldedBase.empty() && foldedBase.find("dummy") != std::string::npos;
				breechBlockAssembly = foldedBase.find("/breechblock/") != std::string::npos;
				dummyBase = dummyBase || breechBlockAssembly;
				meleeDummy = dummyBase && foldedBase.find("meleedummyhousing") != std::string::npos;
			}
			auto* dataHandler = RE::TESDataHandler::GetSingleton();
			if (!dataHandler) {
				return;
			}
			const std::string owner = RuntimeOwnerOfFormID(dataHandler, wid);
			if (owner.empty()) {
				{
					std::scoped_lock lock(g_assemblyMutex);
					g_assemblyCache.insert_or_assign(wid, std::vector<std::string>{});
					g_assemblyPicks.insert_or_assign(wid, std::vector<AssemblyPick>{});
					g_assemblyCatalogs.insert_or_assign(wid, WeaponCatalog{});
					g_assemblyBuiltVer[wid] = ver;
				}
				return;
			}
			struct Part
			{
				std::string slot;
				std::string nif;
				std::string dir;
				std::string folded;
				bool standard{ false };
				bool kwKin{ false };
				// Bound to this weapon only through the MNAM keyword, in
				// another plugin (the vanilla stock set of a mod copy).
				bool crossBound{ false };
				std::uint32_t fid{ 0 };
				std::string label;
			};
			std::vector<Part> parts;
			std::size_t nOwner = 0, nCast = 0, nAp = 0, nPar = 0, nMnam = 0;
			const auto* weapKW = a_weapon->As<RE::TESObjectWEAP>();
			// Is this keyword ID on the weapon? Hoisted out of the OMOD loop
			// because the owner gate below now needs it too.
			const auto onWeaponKeyword = [weapKW](std::uint32_t a_kid) {
				if (!a_kid || !weapKW) {
					return false;
				}
				for (const auto* wk : weapKW->keywords) {
					if (wk && wk->formID == a_kid) {
						return true;
					}
				}
				return false;
			};
			std::vector<std::string> famTok;
			{
				const auto* wedid = a_weapon->GetFormEditorID();
				if (wedid) {
					SplitWords(CaseFold(wedid), famTok);
				}
				SplitWords(CaseFold(FormDisplayName(a_weapon)), famTok);
			}
			struct RawTier
			{
				std::string   label;
				std::uint32_t fid{ 0 };
				bool          kin{ false };
				// Build 94: 1 = table-quality (mod_<Weapon>_Quality_0N
				// bound to this weapon), 2 = legendary effect pool
				// entry, 0 = legacy/unknown no-model candidate (kept
				// only as a fallback for plugin weapons the base-game
				// quality table does not know).
				std::uint32_t kind{ 0 };
				std::uint32_t rank{ 0 };
			};
			std::vector<RawTier> tierRaw;
			std::vector<RawTier> effRaw;
			{
				std::vector<RE::TESForm*> omods;
				BuildFormListForType(dataHandler, RE::FormType::kOMOD, omods, false);
				for (auto* form : omods) {
					if (!form) {
						break;
					}
					const auto formID = form->GetFormID();
					const bool catalogQuality = LookupOmodQuality(formID) != nullptr;
					const bool catalogEffect = LookupOmodLegendary(formID) != nullptr;
					const bool sameOwner = RuntimeOwnerOfFormID(dataHandler, formID) == owner;
					// A plugin that copies a vanilla dummy-base weapon ("MagPulse"
					// -> MagPulse Fanisha, Razorback -> Razorback Trophy) owns no
					// model OMOD of its own: 0 family, 0 assembly, blank preview.
					// Its real geometry is the vanilla stock set, which it names
					// through the same ma_<Weapon> MNAM keyword the vanilla weapon
					// uses. Admit a cross-owner OMOD only when that keyword is on
					// this weapon; the family gate keeps these as a last resort so
					// a plugin with its own art is never repainted vanilla.
					const bool crossBound = !sameOwner && onWeaponKeyword(LookupOmodMnam(formID));
					// Quality/effect tables are base-game records shared by
					// third-party weapons too. Scan those across plugin owners;
					// keep geometry and fuzzy fallbacks owner-scoped.
					if (!sameOwner && !catalogQuality && !catalogEffect && !crossBound) {
						continue;
					}
					if (sameOwner) {
						++nOwner;
					}
					auto* mod = form->As<RE::BGSMod::Attachment::Mod>();
					if (!mod) {
						continue;
					}
					++nCast;
					// Fold every identity text; first slot hit wins.
					std::string folded;
					std::string meshId;
					{
						const auto* edid = form->GetFormEditorID();
						if (edid) {
							folded += CaseFold(edid);
							folded += ' ';
						}
					}
					folded += CaseFold(FormDisplayName(form));
					folded += ' ';
					// Family via the game's own mod-association keyword: the
					// OMOD's MNAM keyword (ma_<Weapon>) must be one of the
					// weapon's KWDA keywords. Covers forms whose editor ID
					// is absent at runtime (OMOD stores no BGSEditorID
					// member, so GetFormEditorID returns null). MNAM is not
					// a direct runtime Mod member (build-78 probe scanned
					// every slot: mnamPtr/mnamRaw=-1), so the base-game
					// OMOD->MNAM census table supplies it; attachPoint and
					// attachParents stay as fallbacks for plugin forms.
					bool kwKin = false;
					if (weapKW) {
						const auto& onWeaponId = onWeaponKeyword;
						if (onWeaponId(LookupOmodMnam(form->GetFormID()))) {
							kwKin = true;
							++nMnam;
						}
						if (!kwKin && mod->attachPoint && onWeaponId(mod->attachPoint->formID)) {
							kwKin = true;
							++nAp;
						}
						if (!kwKin) {
							for (auto* kw : mod->attachParents) {
								if (onWeaponId(kw ? kw->formID : 0)) {
									kwKin = true;
									++nPar;
									break;
								}
							}
						}
					}
					const char* mp = mod->BGSModelMaterialSwap::model.c_str();
					if (!mp || !mp[0]) {
						// No-model OMOD: quality/tier/legendary candidate for
						// the rarity dropdown. "No Mod" and modgroup
						// placeholders are not rarities.
					const bool excluded =
						folded.find("no mod") != std::string::npos ||
						folded.find("nomod") != std::string::npos ||
						folded.find("modgroup") != std::string::npos;
					if (!excluded) {
						const auto fid = form->GetFormID();
						// Build 94 rarity v2: classify before the fuzzy
						// kin heuristic. Quality OMODs (no MNAM, no
						// model) bind to this weapon only through the
						// base-game table, and legendary effects are
						// universal pool entries - both leave the old
						// path, which stays as a fallback for plugin
						// weapons the table does not know.
						if (const auto* q = LookupOmodQuality(fid); q) {
							if (q->weapon == wid && q->rank >= 1u && q->rank <= 4u) {
								static constexpr const char* kQualityName[] = { "", "Common", "Calibrated", "Refined", "Advanced" };
								tierRaw.push_back(RawTier{ kQualityName[q->rank], fid, true, 1, q->rank });
							}
							// Never let another weapon's quality record leak into
							// this weapon through fuzzy EDID token matching.
							continue;
						}
						if (const auto* e = LookupOmodLegendary(fid)) {
							effRaw.push_back(RawTier{ OptionLabel(form, ""), fid, true, 2, e->rank });
							continue;
						}
						bool kin = kwKin;
						if (!kin) {
							std::vector<std::string> ow;
							SplitWords(folded, ow);
							for (const auto& tok : famTok) {
								for (const auto& w : ow) {
									if (tok.find(w) != std::string::npos || w.find(tok) != std::string::npos) {
										kin = true;
										break;
									}
								}
								if (kin) {
									break;
								}
							}
						}
						tierRaw.push_back(RawTier{ OptionLabel(form, ""), fid, kin, 0, 0 });
					}
					continue;
					}
					std::string dir;
					{
						std::string base = mp;
						const auto slash = base.find_last_of("\\/");
						std::string full = CaseFold(base);
						if (slash != std::string::npos) {
							dir = full.substr(0, slash);
							base.erase(0, slash + 1);
						}
						meshId = CaseFold(base);
						folded += meshId;
						folded += ' ';
					}
					if (mod->attachPoint) {
						const auto* ap = mod->attachPoint->GetFormEditorID();
						if (ap) {
							const auto apFold = CaseFold(ap);
							folded += apFold;
							meshId += ' ';
							meshId += apFold;
						}
					}
				 static bool s_omodProbe = false;
					if (!s_omodProbe && (mod->attachPoint || !mod->attachParents.empty())) {
						s_omodProbe = true;
						std::uint32_t parN = 0;
						std::uint32_t pars[3] = { 0, 0, 0 };
						for (auto* kw : mod->attachParents) {
							if (parN < 3) {
								pars[parN] = kw ? kw->formID : 0;
							}
							++parN;
						}
						// Decides where MNAM (ma_<Weapon>, e.g. shotty
						// 001F70BB) lives in the runtime Mod object:
						// ptrSlot = member holding a keyword* whose formID
						// is the MNAM; rawSlot = member holding it as a raw
						// u32. Full member hex follows for offline decode.
						const auto scan = ScanForMnam(mod);
						REX::INFO("SFShopExplorer: omod probe fid={:08X} ap={:08X} parN={} par0={:08X} par1={:08X} par2={:08X} c0={:X} c8={:X} d0={:X} mnamPtr=+0x{:X} mnamRaw=+0x{:X}",
							form->GetFormID(), mod->attachPoint ? mod->attachPoint->formID : 0U,
							parN, pars[0], pars[1], pars[2],
							mod->unkC0, mod->unkC8, mod->unkD0,
							scan.ptrSlot, scan.rawSlot);
						std::string mem;
						char tmp[4];
						const auto* raw = reinterpret_cast<const std::uint8_t*>(mod);
						for (std::uint32_t o = 0x30; o < 0x108; ++o) {
							snprintf(tmp, sizeof(tmp), "%02x", raw[o]);
							mem += tmp;
						}
						REX::INFO("SFShopExplorer: omod mem {}", mem);
					}
				 // Slot from the mesh filename + attach point first (strongest
				 // identity); the full folded string (adds the localized
				 // display name) is only consulted when that has no slot
				 // word. Single-pass let display names like "Recon Sight"
				 // hijack lightlaser.nif parts into the scope slot - which
				 // is why the Eon/EquiNOX assemblies lost their laser.
				 std::string slot = WeaponPartSlot(meshId);
				 if (slot.empty()) {
					 slot = WeaponPartSlot(folded);
				 }
					if (slot.empty()) {
						continue;
					}
					const bool standard = IsStandardModelOmod(form->GetFormID());
					bool dup = false;
					for (auto& p : parts) {
						if (p.nif == mp) {
							// Different OMODs can share an identical NIF.
							// Union family membership and keep the stable standard
							// record for the default, independent of iteration order.
							p.kwKin = p.kwKin || kwKin;
							if (standard && !p.standard) {
								p.standard = true;
								p.fid = form->GetFormID();
								p.label = OptionLabel(form, mp);
							}
							dup = true;
							break;
						}
					}
					if (!dup) {
						parts.push_back(Part{ slot, mp, dir, folded, standard, kwKin, crossBound, form->GetFormID(), OptionLabel(form, mp) });
					}
				}
				// Family gate: the weapon's kept tokens (EDID + display name)
			// must meet the OMOD somewhere (either side contains the
			// other), or share the NIF dir. Small plugins (<150 OMODs)
			// fall back to all-owner (single-gun mods are coherent by
			// construction). Big plugins with no token match assemble
			// NOTHING - honest unavailable beats cross-family franken
			// (every Starfield dummy funneled into M1919 parts).
			std::vector<const Part*> family;
			for (const auto& p : parts) {
				if (p.crossBound) {
					continue;
				}
				std::vector<std::string> ow;
				SplitWords(p.folded, ow);
				bool kin = false;
				for (const auto& tok : famTok) {
					if (p.dir.find(tok) != std::string::npos) {
						kin = true;
						break;
					}
					for (const auto& w : ow) {
						if (tok.find(w) != std::string::npos || w.find(tok) != std::string::npos) {
							kin = true;
							break;
						}
					}
					if (kin) {
						break;
					}
				}
				if (kin || p.kwKin) {
					family.push_back(&p);
				}
			}
			if (family.empty() && nOwner < 150) {
				for (const auto& p : parts) {
					if (!p.crossBound) {
						family.push_back(&p);
					}
				}
			}
			std::size_t nCross = 0;
			for (const auto& p : parts) {
				if (p.crossBound) {
					++nCross;
				}
			}
			if (family.empty() && nCross > 0) {
				// The plugin's own records cannot draw at all. This is the
				// blank-preview family (199 dummy-base weapons that copy a
				// vanilla gun); draw the vanilla stock set instead.
				for (const auto& p : parts) {
					if (p.crossBound) {
						family.push_back(&p);
					}
				}
				REX::INFO("SFShopExplorer: dummy weapon '{}' owns no model OMOD; assembled from the {} vanilla stock part(s) its MNAM keyword binds it to",
					a_weapon->GetFormEditorID(), family.size());
			}
			// `kStandardModelOmods` is a table of BASE-GAME OMOD formIDs, so a
			// Creation Kit weapon has no standard OMOD in it at all: every part
			// of SFBGS050's BreechBlock, the Razorback packs and the rest scores
			// false. The per-slot default pass then had an authored stock for
			// the mandatory receiver only and attached nothing else, which is
			// the bare frame the shop preview showed - 13 meshes against the
			// assembled 18. When the family declares no standard OMOD, its
			// first part per slot IS the stock part; the existing receiver
			// fallback already relies on that.
			std::size_t stdCount = 0;
			for (const auto* p : family) {
				if (p->standard) {
					++stdCount;
				}
			}
			// Slots that had to be filled from the family's first part because
			// the database declares no usable stock OMOD for them.
			std::size_t autoFilled = 0;
			// Anchor: most common NIF dir among receiver parts. Melee
			// dummy housings have no receiver, so use the family blade and
			// handle directories instead; their NIF connect points are
			// positioned by the housing connector retained in FormModelPaths.
			// Everything outside the chosen directory is another weapon's
			// furniture sharing the plugin file.
			std::string anchor;
			{
				std::unordered_map<std::string, std::size_t> votes;
				std::unordered_map<std::string, std::size_t> presence;
				for (const auto* p : family) {
					++presence[p->dir];
					if (p->slot == "receiver") {
						++votes[p->dir];
					}
				}
				if (votes.empty() && meleeDummy) {
					for (const auto* p : family) {
						if (p->slot == "blade" || p->slot == "handle") {
							++votes[p->dir];
						}
					}
				}
				// Tie-break: primary-part votes, then family part presence in
				// the dir (kills the MagPulse magstorm/magpulse and
				// EquiNOX pirate/laser_rifle 1-1 flips), then
				// lexicographic for determinism.
				std::size_t bestVotes = 0, bestPres = 0;
				for (const auto& [dir, count] : votes) {
					const auto pres = presence[dir];
					if (anchor.empty()) {
						anchor = dir;
						bestVotes = count;
						bestPres = pres;
						continue;
					}
					if (count > bestVotes ||
						(count == bestVotes && pres > bestPres) ||
						(count == bestVotes && pres == bestPres && dir < anchor)) {
						anchor = dir;
						bestVotes = count;
						bestPres = pres;
					}
				}
			}
			std::size_t famKw = 0;
			for (const auto* p : family) {
				if (p->kwKin) {
					++famKw;
				}
			}
			// wk = weapon KWDA keyword count read through
			// BGSKeywordForm::keywords; wk=0 means that store is empty or
			// misoffset at runtime and the formID gate can never match.
			const auto wkCount = weapKW ? weapKW->keywords.size() : std::size_t{ 0 };
			REX::INFO("SFShopExplorer: dummy weapon '{}' OMODs: {} owner, {} cast, {} slotted, {} family ({} kw, ap {} par {} mnam {} wk {}), anchor '{}'",
				a_weapon->GetFormEditorID(), nOwner, nCast, parts.size(), family.size(), famKw, nAp, nPar, nMnam, wkCount, anchor);
			// Assemble the receiver or melee core plus explicit standard
			// part defaults, restricted to the selected family. Slots without
			// a standard OMOD remain at stock. No compatible primary-part
			// anchor => no assembly: fall back to EDID resolution.
			static constexpr const char* kSlots[] = { "receiver", "blade", "barrel", "muzzle", "mag",
				"battery", "stock", "grip", "handle", "coating", "scope", "laser", "cover", "internal" };
			std::vector<std::string> found;
			std::vector<AssemblyPick> picks;
			std::string composition;
			WeaponCatalog catalog;
			// Build 94 rarity v2: table-quality entries drive the list
			// (rank-sorted, own weapon only); the legacy kin fallback
			// applies only when the base-game table has nothing for
			// this weapon (plugin weapons). Rare/Epic/Legendary
			// sentinels are always present and the universal effects
			// pool feeds their rolls.
			{
				std::size_t nQuality = 0;
				for (const auto& rt : tierRaw) {
					if (rt.kind == 1) {
						catalog.qualities.push_back(TierOption{ rt.label, rt.fid, rt.rank });
						++nQuality;
					}
				}
				if (nQuality == 0) {
					// Base-game quality is table-covered; this fallback is for
					// plugin weapons only. Don't mistake effect-pool entries
					// for quality ranks.
					for (const auto& rt : tierRaw) {
						if (rt.kind == 0 && (rt.kin || nOwner < 150)) {
							catalog.qualities.push_back(TierOption{ rt.label, rt.fid, 0 });
						}
					}
				}
				std::stable_sort(catalog.qualities.begin(), catalog.qualities.end(),
					[](const TierOption& a, const TierOption& b) { return a.rank < b.rank; });
				for (const auto& e : effRaw) {
					catalog.effects.push_back(TierOption{ e.label, e.fid, e.rank });
				}
			}
			if (!anchor.empty()) {
				for (const char* slot : kSlots) {
					const Part* pick = nullptr;
					const Part* firstUsable = nullptr;
					std::vector<SlotOption> opts;
					for (const auto* pp : family) {
						const auto& p = *pp;
						if (p.slot != slot) {
							continue;
						}
						if (p.dir != anchor) {
							// Dirs come from model strings with backslash
							// separators; the old anchor + "/" compare
							// never matched and silently dropped anchor
							// subdirs. Accept both separators.
							const bool under = p.dir.size() > anchor.size() &&
								p.dir.compare(0, anchor.size(), anchor) == 0 &&
								(p.dir[anchor.size()] == '\\' || p.dir[anchor.size()] == '/');
							if (!under) {
								continue;
							}
						}
						opts.push_back(SlotOption{ p.label, p.nif, p.fid, p.standard });
						// Only an explicitly standard OMOD is a stock default.
						// Dummy_Barrel is a connector/placeholder, not a real
						// installed Rattler barrel; keep it selectable but don't
						// add it to the stock preview.
						const bool usableDefault = !(p.slot == "barrel" &&
							CaseFold(p.nif).find("dummy_barrel") != std::string::npos);
						if (usableDefault && !firstUsable) {
							firstUsable = &p;
						}
						if (p.standard && usableDefault && (!pick || !pick->standard)) {
							pick = &p;
						} else if (!pick && (std::string_view(slot) == "receiver" ||
							(dummyBase && IsMandatoryDummyAssemblySlot(slot, meleeDummy)))) {
							// Dummy bases need their core geometry to render. Keep
							// receiver catalogs' historical first-option fallback
							// for real-base guns.
							pick = &p;
						}
					}
					if (!pick && dummyBase && firstUsable) {
						// A Creation Kit family often declares no standard OMOD for
						// a slot (stdlib table holds BASE-GAME formIDs only), or
						// declares a placeholder connector for it. The pass above
						// then left the slot at stock, which is why the shop showed
						// BreechBlock as a bare receiver (13 meshes / 29506 tris)
						// until the barrel, magazine and grip were each picked by
						// hand (18 / 49019 fully assembled). Attach the slot's
						// first real part so the first render is already whole.
						pick = firstUsable;
						++autoFilled;
					}
			if (!opts.empty()) {
				catalog.slotOrder.push_back(slot);
						catalog.slots[slot] = std::move(opts);
						// Zero means the stock weapon has no separate NIF
						// for this slot (e.g. its muzzle or sight).
						catalog.defaultFid[slot] = pick ? pick->fid : 0;
					}
				}
			}
			// Resolve against the user's choices: a chosen slot fid
			// replaces that slot's default; the tier OMOD rides along as
			// a "tier" pick (no model, stats/name only). Non-assembly
			// weapons carry only explicit choices - defaults would attach
			// parts the base model never renders.
			for (const std::string& slot : catalog.slotOrder) {
				const auto choiceIt = choice.slotFid.find(slot);
				const bool fromChoice = choiceIt != choice.slotFid.end();
				std::uint32_t fid = 0;
				if (fromChoice) {
					fid = choiceIt->second;
				} else if (dummyBase) {
					fid = catalog.defaultFid[slot];
				}
				// Receiver and melee core geometry are mandatory for a dummy
				// base. Ignore stale/zero "no part" selections and use stock.
				if (dummyBase && IsMandatoryDummyAssemblySlot(slot, meleeDummy) && fid == 0) {
					if (const auto dit = catalog.defaultFid.find(slot); dit != catalog.defaultFid.end()) {
						fid = dit->second;
					}
				}
				const SlotOption* opt = nullptr;
				for (const auto& o : catalog.slots[slot]) {
					if (o.fid == fid) {
						opt = &o;
						break;
					}
				}
				if (!opt && dummyBase && fromChoice && fid != 0) {
					fid = catalog.defaultFid[slot];
					for (const auto& o : catalog.slots[slot]) {
						if (o.fid == fid) {
							opt = &o;
							break;
						}
					}
				}
				if (!opt || found.size() >= 11) {
					continue;
				}
				found.push_back(opt->nif);
				picks.push_back(AssemblyPick{ slot, fid });
				if (!composition.empty()) {
					composition += '+';
				}
				composition += slot;
			}
			if (choice.qualityFid != 0) {
				for (const auto& quality : catalog.qualities) {
					if (quality.fid == choice.qualityFid) {
						picks.push_back(AssemblyPick{ "quality", quality.fid });
						if (!composition.empty()) {
							composition += '+';
						}
						composition += "quality";
						break;
					}
				}
			}
			if (choice.rarityFid >= kRaritySentinelBase + 1u && choice.rarityFid <= kRaritySentinelBase + 3u) {
				// Rarity controls how many legendary effects roll; quality is
				// attached independently above. The selected roll is cached with
				// the assembled picks so the preview and purchase match.
				const std::uint32_t nRoll = choice.rarityFid - kRaritySentinelBase;
				std::vector<std::uint32_t> pool;
				for (const auto& effect : catalog.effects) {
					if (effect.rank >= 1u && effect.rank <= nRoll) {
						pool.push_back(effect.fid);
					}
				}
				if (!pool.empty()) {
					static std::mt19937 rng{ std::random_device{}() };
					std::shuffle(pool.begin(), pool.end(), rng);
					if (pool.size() > nRoll) pool.resize(nRoll);
					std::string rollIds;
					for (const auto fid : pool) {
						picks.push_back(AssemblyPick{ "legendary", fid });
						if (!rollIds.empty()) rollIds += ',';
						rollIds += std::format("{:08X}", fid);
					}
					if (!composition.empty()) composition += '+';
					composition += "rarity";
					REX::INFO("SFShopExplorer: rarity roll {:08X} -> {} effects [{}]", wid, nRoll, rollIds);
				}
			}
			{
				std::scoped_lock lock(g_assemblyMutex);
				g_assemblyCache.insert_or_assign(wid, found);
				g_assemblyPicks.insert_or_assign(wid, picks);
				g_assemblyCatalogs.insert_or_assign(wid, catalog);
				g_assemblyBuiltVer[wid] = ver;
			}
			for (const auto& p : found) {
				a_push(p.c_str());
			}
			REX::INFO("SFShopExplorer: dummy weapon '{}' assembly: {} [{}] ({} slots, {} tiers, {} effects, {})",
				a_weapon->GetFormEditorID(), found.size(), composition,
				catalog.slotOrder.size(), catalog.qualities.size(), catalog.effects.size(), dummyBase ? "dummy" : "base");
			if (dummyBase && autoFilled > 0) {
				// Say it out loud: these slots were filled from the family's
				// first part because the database has no stock flag for this
				// weapon, not because the OMOD was authored as the stock one.
				REX::INFO("SFShopExplorer: dummy weapon '{}' declares no usable standard OMOD for {} slot(s) ({} standard part(s) in family); previewed with the family's first part [{}]",
					a_weapon->GetFormEditorID(), autoFilled, stdCount, composition);
			}
			if (!catalog.qualities.empty()) {

				std::string qualityList;
				for (const auto& quality : catalog.qualities) {
					if (!qualityList.empty()) qualityList += ", ";
					qualityList += std::format("{}={:08X}", quality.label, quality.fid);
				}
				REX::INFO("SFShopExplorer: weapon quality options {:08X}: {}", wid, qualityList);
			}
		}
			}

		// True when FormModelPaths assembled OMOD NIFs for this weapon
		// (session cache hit with a non-empty assembly result). Used by
		// the preview (EDID-first resolution) and the details Model line.
		bool FormModelsAssembled(const RE::TESForm* a_form)
		{
			if (!a_form) {
				return false;
			}
			std::scoped_lock lock(g_assemblyMutex);
			const auto it = g_assemblyCache.find(a_form->GetFormID());
			return it != g_assemblyCache.end() && !it->second.empty();
		}


		// -------------------------------------------------------------------
		// Purchase spike. A modded weapon only arrives with its parts if the
		// OMODs are attached to a real world reference first; the inventory
		// transfer is the engine's, so every stage is probed and logged.
		// -------------------------------------------------------------------
		enum class SpikeOutcome
		{
			kNotAttempted,       // nothing mutated; caller plain-adds
			kInventoryOnly,      // added directly to inventory; weapon OMOD picks are not applied
			kInventoryAddFailed, // direct inventory add did not raise the weapon count
			kDeliveredMods,      // inventory count confirmed, OMODs attached
			kDeliveredPlain,     // inventory count confirmed, attach failed but recovered
			kTransferPendingMods,  // move request returned; next-tick count is pending
			kTransferPendingPlain, // move request returned; next-tick count is pending
			kOnFloor,              // move request faulted; dropped without mods
			kOnFloorMods,          // move request faulted; dropped with OMODs attached
		};

		// Build 86 hardening of the build-85 mod-application spike: every
		// stage that touches an unverified CommonLib offset or an unknown
		// game path runs in its own POD-only __try helper (the ScanForMnam
		// / LogChain pattern). An access violation becomes a logged stage
		// code instead of a CTD, and the purchase degrades gracefully. The
		// orchestrator itself never uses __try (it owns C++ locals).
		static bool SpikeReadable(const void* a_ptr, const bool a_writable)
		{
			if (!a_ptr) {
				return false;
			}
			MEMORY_BASIC_INFORMATION mbi{};
			if (VirtualQuery(a_ptr, &mbi, sizeof(mbi)) == 0) {
				return false;
			}
			if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) != 0) {
				return false;
			}
			const auto prot = mbi.Protect & 0xFFu;
			if (prot == PAGE_NOACCESS || prot == 0) {
				return false;
			}
			if (!a_writable) {
				return true;
			}
			return prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
			       prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
		}

		static void SpikeCopyStr(const char* a_src, char* a_dst, const std::size_t a_cap)
		{
			__try
			{
				if (!a_src) {
					a_dst[0] = '\0';
					return;
				}
				std::size_t i = 0;
				for (; i + 1 < a_cap && a_src[i] != '\0'; ++i) {
					a_dst[i] = a_src[i];
				}
				a_dst[i] = '\0';
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				a_dst[0] = '\0';
			}
		}

		// Dump a console SCRIPT_FUNCTION plus every parameter (name/type/
		// optional): the [spike] log alone then explains how AttachMod
		// wants its arguments. Also probes AttachModToInventoryItem (may
		// not exist) - a potential no-drop route for the next build.
		static void SpikeLogCommand(const char* a_what, RE::SCRIPT_FUNCTION* a_fn)
		{
			if (!a_fn) {
				REX::INFO("SFShopExplorer: [spike] {} command not found", a_what);
				return;
			}
			char name[64]{};
			char shortName[64]{};
			std::uint16_t nparams = 0;
			std::uint8_t refFn = 0;
			std::uintptr_t exec = 0;
			__try
			{
				SpikeCopyStr(a_fn->functionName, name, sizeof(name));
				SpikeCopyStr(a_fn->shortName, shortName, sizeof(shortName));
				nparams = a_fn->numParams;
				refFn = a_fn->referenceFunction;
				exec = reinterpret_cast<std::uintptr_t>(a_fn->executeFunction);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				REX::WARN("SFShopExplorer: [spike] {} header read raised 0x{:X}", a_what, GetExceptionCode());
				return;
			}
			REX::INFO("SFShopExplorer: [spike] {} name='{}' short='{}' nparams={} refFn={} exec=0x{:X}",
				a_what, name, shortName, static_cast<std::uint32_t>(nparams), static_cast<std::uint32_t>(refFn), exec);
			if (!a_fn->params) {
				REX::INFO("SFShopExplorer: [spike] {} has no parameter table", a_what);
				return;
			}
			const std::uint16_t show = nparams > 8 ? 8 : nparams;
			for (std::uint16_t i = 0; i < show; ++i) {
				char pn[64]{};
				std::uint32_t pt = 0;
				std::uint8_t opt = 0;
				__try
				{
					const auto& p = a_fn->params[i];
					SpikeCopyStr(p.paramName, pn, sizeof(pn));
					pt = p.paramType;
					opt = p.optional ? 1 : 0;
				}
				__except (EXCEPTION_EXECUTE_HANDLER)
				{
					REX::WARN("SFShopExplorer: [spike] {} param[{}] read raised 0x{:X}", a_what, static_cast<std::uint32_t>(i), GetExceptionCode());
					break;
				}
				REX::INFO("SFShopExplorer: [spike] {} param[{}] name='{}' type=0x{:X} optional={}",
					a_what, static_cast<std::uint32_t>(i), pn, pt, static_cast<std::uint32_t>(opt));
			}
		}

		// Stage C (build 87): resolve RemoveItem's outHandle to the fresh
		// world ref via Misc::LookupReferenceByHandle (Address Library ID
		// 72986 - commonlib zeroed the entry but the comment ID resolves
		// to a .text RVA in versionlib-1-16-244-0). The exact signature
		// is unverified, so the result is hedged: a pointer-sized return
		// is taken from RAX (viaRet), a bool-style true selects the out
		// parameter (viaOut). Whole call under SEH; the caller validates
		// candidates (readable + base-form hunt) before using either.
		struct SpikeHandleProbe
		{
			RE::TESObjectREFR* viaRet;
			RE::TESObjectREFR* viaOut;
			std::uintptr_t     rawRet;
			bool               sehFault;
		};

		using SpikeHandleFn = RE::TESObjectREFR* (*)(std::uint32_t, RE::TESObjectREFR*&);
		static REL::Relocation<SpikeHandleFn>& SpikeHandleRel()
		{
			static REL::Relocation<SpikeHandleFn> fn{ REL::ID{ 72986 } };
			return fn;
		}

		static SpikeHandleProbe SpikeResolveHandle(const std::uint32_t a_handle)
		{
			auto& rel = SpikeHandleRel();  // magic-static init outside the __try region
			SpikeHandleProbe p{ nullptr, nullptr, 0, false };
			__try
			{
				RE::TESObjectREFR* out = nullptr;
				auto* ret = rel(a_handle, out);
				p.rawRet = reinterpret_cast<std::uintptr_t>(ret);
				if (p.rawRet > 0x10000) {
					p.viaRet = ret;
				}
				p.viaOut = out;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				p.sehFault = true;
				p.rawRet = static_cast<std::uintptr_t>(GetExceptionCode());
			}
			return p;
		}

		// Identity check for a handle-resolved candidate (build 88 - the
		// 87 single-signal hunt rejected real refs because Starfield may
		// not store the base as a raw pointer). Independent signals ORed
		// into the returned mask:
		//   bit0 (1): base formID found INLINE as a u32 in +0x00..+0x150
		//   bit1 (2): drop-position float triple found (data.location)
		//   bit2 (4): a pointer slot's pointee carries the base formID at
		//             +0x28 (raw-pointer storage)
		//   bit3 (8): a pointer slot equals the known parentCell
		// Non-zero = accepted. Full hex dump + level-1 pointee+0x28 dump
		// always go to the caller buffers for the log. 0 = rejected or
		// SEH fault (POD-only under __try).
		// Build 96: hunt range 0xD8 -> 0x150. Build-95 rejected BOTH
		// handle candidates (sig=0x0) even though each drop resolved a
		// distinct object (temp-formID-like FF07B334/FF07B336, +0x98
		// instance set) - the fresh ref evidently keeps its position/
		// identity fields past 0xD8, so the old window was too short.
		static std::uint32_t SpikeValidateRef(RE::TESObjectREFR* a_ref, const std::uint32_t a_baseFid,
			const RE::NiPoint3* a_pos, char* a_hexOut, const std::size_t a_hexCap,
			char* a_ptrOut, const std::size_t a_ptrCap)
		{
			a_hexOut[0] = '\0';
			a_ptrOut[0] = '\0';
			__try
			{
				constexpr std::uint32_t kRange = 0x150;
				const auto* b = reinterpret_cast<const std::uint8_t*>(a_ref);
				std::size_t pos = 0;
				for (std::size_t o = 0; o < kRange && pos + 20 < a_hexCap; o += 8) {
					std::uint64_t q = 0;
					std::memcpy(&q, b + o, sizeof(q));
					pos += static_cast<std::size_t>(std::snprintf(a_hexOut + pos, a_hexCap - pos, "%016llX ", q));
				}
				std::uint32_t sig = 0;
				for (std::uint32_t o = 0; o + 4 <= kRange; o += 4) {
					std::uint32_t u = 0;
					std::memcpy(&u, b + o, sizeof(u));
					if (u == a_baseFid) {
						sig |= 1u;
					}
					if (o + 12 <= 0xD8) {
						float f[3]{};
						std::memcpy(f, b + o, sizeof(f));
						const float dx = f[0] - a_pos->x;
						const float dy = f[1] - a_pos->y;
						const float dz = f[2] - a_pos->z;
						if (dx * dx + dy * dy + dz * dz < 25.f) {
							sig |= 2u;
						}
					}
				}
				std::size_t ppos = 0;
				for (std::uint32_t o = 0; o + 8 <= kRange; o += 8) {
					std::uintptr_t q = 0;
					std::memcpy(&q, b + o, sizeof(q));
					if (q < 0x100000000ULL || (q & 7) != 0) {
						continue;
					}
					if (!SpikeReadable(reinterpret_cast<const void*>(q), false)) {
						continue;
					}
					std::uint32_t fid28 = 0;
					std::memcpy(&fid28, reinterpret_cast<const void*>(q + 0x28), sizeof(fid28));
					if (fid28 == a_baseFid) {
						sig |= 4u;
					}
					if (ppos + 24 < a_ptrCap) {
						ppos += static_cast<std::size_t>(std::snprintf(a_ptrOut + ppos, a_ptrCap - ppos, "%X:%08X ", o, fid28));
					}
				}
				return sig;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return 0;
			}
		}

		// Diagnostic only (never fatal): hex-dump cell+0x70..+0xB0 so the
		// offline analysis can pin down the real references-list layout
		// (commonlib's BSTArray claim is known-stale for this game).
		static void SpikeDumpCellLayout(RE::TESObjectCELL* a_cell, char* a_hexOut, const std::size_t a_hexCap)
		{
			a_hexOut[0] = '\0';
			__try
			{
				const auto* b = reinterpret_cast<const std::uint8_t*>(a_cell);
				std::size_t pos = 0;
				for (std::size_t o = 0x70; o < 0xB0 && pos + 20 < a_hexCap; o += 8) {
					std::uint64_t q = 0;
					std::memcpy(&q, b + o, sizeof(q));
					pos += static_cast<std::size_t>(std::snprintf(a_hexOut + pos, a_hexCap - pos, "%016llX ", q));
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				// leave the buffer empty - caller logs the fault
			}
		}

		// Cell reference-count snapshot (cell+0x80, same layout the
		// walk uses) taken BEFORE the drop so the walk can tell which
		// array entries are new. 0 = unknown (null/unreadable cell).
		static std::uint32_t SpikeCellCount(RE::TESObjectCELL* a_cell)
		{
			std::uint32_t count = 0;
			__try
			{
				if (a_cell) {
					const auto* b = reinterpret_cast<const std::uint8_t*>(a_cell);
					std::memcpy(&count, b + 0x80, sizeof(count));
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				count = 0;
			}
			return count;
		}

		// First inline float triple within 25 units of a_pos over the
		// ref's raw bytes (build-96 range 0x150). Returns the match
		// offset or 0xFFFFFFFF. Caller has already readability-checked
		// the object and sits inside its own __try.
		static std::uint32_t SpikePosOffset(const std::uint8_t* a_rb, const RE::NiPoint3* a_pos)
		{
			for (std::uint32_t o = 0; o + 12 <= 0x150; o += 4) {
				float f[3]{};
				std::memcpy(f, a_rb + o, sizeof(f));
				const float dx = f[0] - a_pos->x;
				const float dy = f[1] - a_pos->y;
				const float dz = f[2] - a_pos->z;
				if (dx * dx + dy * dy + dz * dz < 25.f) {
					return o;
				}
			}
			return 0xFFFFFFFF;
		}

		// Debug switch for the mod-delivery spike, read the same way the shop
		// settings are (the ini sits next to the DLL). Off by default: the
		// probes it guards wrote a dozen lines per purchase, and 0.8.338's
		// log was mostly these dumps.
		static bool SpikeDebug()
		{
			static const bool value = []() {
				HMODULE self = nullptr;
				GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCWSTR>(&SpikeDebug), &self);
				wchar_t modulePath[MAX_PATH]{};
				GetModuleFileNameW(self, modulePath, MAX_PATH);
				const auto ini = std::filesystem::path(modulePath).parent_path() / kIniFileName;
				return GetPrivateProfileIntW(L"shop", L"debugSpike", 0, ini.c_str()) != 0;
			}();
			return value;
		}

		// Identity test for a resolved reference: its OBJ_REFR.objectReference
		// must be the form that was just dropped. TESObjectREFR::data sits at
		// +0x80, and OBJ_REFR's members are angle at +0x00, location at +0x0C
		// and objectReference at +0x18, so the base pointer is ref+0x98 - the
		// exact qword the attach executor gates on. (+0x18 is a TESForm field
		// and says nothing about the base; the first version of this check
		// read it and rejected every real ref.)
		//
		// This is the check the handle probe never had. On 0.8.338 a candidate
		// was accepted on a loose position-float match alone, and the purchase
		// then ran against a pointer into a string table: attach AV 0xC0000005,
		// activation a no-op, and the real item left on the floor as Common.
		static bool SpikeRefIsBound(const RE::TESObjectREFR* a_ref, const RE::TESBoundObject* a_bound)
		{
			if (!a_ref || !a_bound) {
				return false;
			}
			__try
			{
				std::uintptr_t base = 0;
				std::memcpy(&base, reinterpret_cast<const std::uint8_t*>(a_ref) + 0x98, sizeof(base));
				return base == reinterpret_cast<std::uintptr_t>(a_bound);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// Cell-walk fallback for the dropped ref. The 87 cell dump gave
		// the real references layout: count u32 @cell+0x80, capacity u32
		// @cell+0x84, data* @cell+0x88. Build 96 rewrote the selection:
		// build-95 returned the FIRST position match, and because both
		// purchases dropped at the same feet position it returned the
		// SAME pre-existing floor ref both times (count=571 idx=569,
		// count=572 idx=569 - the second delivery wiped the first
		// delivery's repairs on that shared ref). Selection now:
		//   (a) tail route - the caller snapshots the count before the
		//       drop, so entries[preCount..count) were appended by OUR
		//       drop; the OLDEST new gated entry (index preCount
		//       upward) is the ref RemoveItem just created.
		//   (b) position scan only when the count did not grow, and it
		//       keeps the LAST gated match (newest) instead of the
		//       first. A match must carry ref+0x98 (the attach
		//       executor's gate) - position alone is not enough.
		// Diagnostics (route, idx, count/pre, instance, posAt) go to
		// a_hexOut. POD-only under __try.
		static RE::TESObjectREFR* SpikeWalkCell(RE::TESObjectCELL* a_cell, const RE::NiPoint3* a_pos,
			const std::uint32_t a_preCount, const RE::TESBoundObject* a_bound,
			char* a_hexOut, const std::size_t a_hexCap)
		{
			a_hexOut[0] = '\0';
			__try
			{
				const auto* b = reinterpret_cast<const std::uint8_t*>(a_cell);
				std::uint32_t count = 0;
				std::memcpy(&count, b + 0x80, sizeof(count));
				std::uintptr_t data = 0;
				std::memcpy(&data, b + 0x88, sizeof(data));
				if (count > 4096 || data < 0x100000000ULL || !SpikeReadable(reinterpret_cast<const void*>(data), false)) {
					std::snprintf(a_hexOut, a_hexCap, "count=%u data=0x%llX unreadable", count, static_cast<unsigned long long>(data));
					return nullptr;
				}
				const auto* entries = reinterpret_cast<const std::uintptr_t*>(data);
				const auto gate = [a_bound](std::uintptr_t r) -> std::uint64_t {
					if (r < 0x100000000ULL || (r & 7) != 0 || !SpikeReadable(reinterpret_cast<const void*>(r), false)) {
						return 0;
					}
					// Identity first: the entry's base (OBJ_REFR.objectReference at
					// +0x98) must be the form we dropped, or a neighbouring floor
					// ref of the same type would be chosen (the build-95 failure)
					// or, worse, anything at all (0.8.338's string-table match).
					std::uint64_t base = 0;
					std::memcpy(&base, reinterpret_cast<const void*>(r + 0x98), 8);
					if (base != reinterpret_cast<std::uintptr_t>(a_bound)) {
						return 0;
					}
					return base;
				};
				// (a) tail route: entries appended after the snapshot
				if (a_preCount > 0 && a_preCount < count) {
					for (std::uint32_t i = a_preCount; i < count; ++i) {
						const auto r = entries[i];
						const auto inst = gate(r);
						if (!inst) {
							continue;
						}
						const auto posAt = SpikePosOffset(reinterpret_cast<const std::uint8_t*>(r), a_pos);
						std::snprintf(a_hexOut, a_hexCap, "count=%u pre=%u tail idx=%u inst=0x%llX posAt=0x%X",
							count, a_preCount, i, static_cast<unsigned long long>(inst), posAt);
						return reinterpret_cast<RE::TESObjectREFR*>(r);
					}
					// new entries exist but none passed the gate -
					// fall through and say so in the diagnostics.
				}
				// (b) position scan, keep the LAST gated match (newest)
				RE::TESObjectREFR* last = nullptr;
				std::uint32_t lastIdx = 0;
				std::uint32_t lastPos = 0;
				std::uint64_t lastInst = 0;
				for (std::uint32_t i = 0; i < count; ++i) {
					const auto r = entries[i];
					const auto inst = gate(r);
					if (!inst) {
						continue;
					}
					const auto posAt = SpikePosOffset(reinterpret_cast<const std::uint8_t*>(r), a_pos);
					if (posAt != 0xFFFFFFFF) {
						last = reinterpret_cast<RE::TESObjectREFR*>(r);
						lastIdx = i;
						lastPos = posAt;
						lastInst = inst;
					}
				}
				if (last) {
					std::snprintf(a_hexOut, a_hexCap, "count=%u pre=%u pos idx=%u inst=0x%llX posAt=0x%X",
						count, a_preCount, lastIdx, static_cast<unsigned long long>(lastInst), lastPos);
					return last;
				}
				std::snprintf(a_hexOut, a_hexCap, "count=%u pre=%u no match", count, a_preCount);
				return nullptr;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				std::snprintf(a_hexOut, a_hexCap, "seh=0x%X", GetExceptionCode());
				return nullptr;
			}
		}

		// Stage B: guarded RemoveItem for creating the temporary drop.
		// Returns 0 or the SEH code.
		static std::uint32_t SpikeRemoveStage(RE::TESObjectREFR* a_ref, const RE::RemoveItemRequest* a_req, std::uint32_t* a_outHandle)
		{
			__try
			{
				a_ref->RemoveItem(*a_outHandle, *a_req);
				return 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return GetExceptionCode();
			}
		}

		// Stage E (0.8.338): the game's own take path.
		//
		// A move request cannot deliver a loose world ref: RemoveItem looks for
		// an inventory entry on the source, and a dropped stack has none, which
		// is why every modded purchase on 1.16.244 stayed at the player's feet.
		// Activation is the path the game itself runs when the player presses
		// the take prompt - the ref's own activate handler decides what happens,
		// and for a loose item that is the pickup. The container then receives
		// the INSTANCE the ref holds (the one carrying the attached OMODs), not
		// a fresh plain object.
		//
		// `objectToGet`/`count` stay empty: the console `activate` command - the
		// engine's own caller - passes only the ref and the activator. The
		// return value is discarded the same way that caller does; the
		// inventory count is the ground truth.
		static std::uint32_t SpikeActivateStage(RE::TESObjectREFR* a_ref, RE::TESObjectREFR* a_actor)
		{
			__try
			{
				RE::OBJ_ACTIVATE_DATA data{};
				data.activatedRef = a_ref;
				data.actionRef = a_actor;
				static_cast<void>(a_ref->Activate(data));
				return 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return GetExceptionCode();
			}
		}

		// Stage E1 (0.8.342): deliver the instance the ref holds.
		//
		// Every engine "take" entry has failed on 1.16.244: Activate moves
		// nothing, RemoveItem finds no list entry on a loose ref, and
		// Actor::PickUpObject (the commonlib virtual the take prompt is
		// assumed to run) faults with 0xC0000005 on this build's vtable.
		//
		// What is left is the one call this plugin already uses successfully:
		// AddObjectToContainer. A dropped stack's ExtraDataList is the ref's at
		// ref+0xC8 - the same object the attach wrote extra 0x33 into - and
		// AddObjectToContainer accepts one. Passing a REFCOUNTED copy of that
		// list is what build 88 got wrong: it shared a raw pointer, so the
		// ref's SetDelete freed the list out from under the inventory item and
		// the game hung. ExtraDataList is intrusive-refcounted, so a real
		// BSTSmartPointer holds it alive here.
		static std::uint64_t SpikeRefExtraList(RE::TESObjectREFR* a_ref)
		{
			std::uint64_t dl = 0;
			__try
			{
				std::memcpy(&dl, reinterpret_cast<const std::uint8_t*>(a_ref) + 0xC8, sizeof(dl));
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				dl = 0;
			}
			return dl;
		}

		static std::uint64_t SpikeRefInstance(RE::TESObjectREFR* a_ref)
		{
			std::uint64_t inst = 0;
			__try
			{
				std::memcpy(&inst, reinterpret_cast<const std::uint8_t*>(a_ref) + 0x98, sizeof(inst));
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				inst = 0;
			}
			return inst;
		}

		// Same Address Library entry as SpikeFindExtraRel (37878 =
		// FindExtraByType), duplicated because this stage is compiled before
		// that accessor.
		static REL::Relocation<void* (*)(void*, std::uint8_t)>& SpikeExtraFindRel()
		{
			static REL::Relocation<void* (*)(void*, std::uint8_t)> fn{ REL::ID{ 37878 } };
			return fn;
		}

		static std::uint32_t SpikeExtraModCount(const std::uint64_t a_dl)
		{
			std::uint32_t count = 0;
			if (!a_dl) {
				return 0;
			}
			auto& findRel = SpikeExtraFindRel();
			__try
			{
				if (findRel.get()) {
					void* e33 = findRel(reinterpret_cast<void*>(static_cast<std::uintptr_t>(a_dl)), 0x33);
					if (e33) {
						std::memcpy(&count, static_cast<const std::uint8_t*>(e33) + 0x18, sizeof(count));
					}
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				count = 0xFFFFFFFFu;
			}
			return count;
		}

		static std::uint32_t SpikeAddWithExtra(RE::PlayerCharacter* a_player, RE::TESBoundObject* a_bound,
			std::uint32_t a_count, RE::ExtraDataList* a_extra)
		{
			if (!a_player || !a_bound || !a_extra) {
				return 1;
			}
			// No __try: the guarded object is a copy of the list the ref itself
			// owns, and this exact virtual is the proven plain-add call.
			a_player->AddObjectToContainer(a_bound, RE::BSTSmartPointer<RE::ExtraDataList>(a_extra),
				static_cast<std::int32_t>(a_count), nullptr, RE::ITEM_TRANSFER_REASON::kNone);
			return 0;
		}

		// The player's newest stack for a form: its extra list, instance data and
		// count. Used to prove the mods arrived in the inventory and to tell
		// whether the world ref may be retired (a shared instance data pointer
		// must keep the ref alive).
		struct SpikeInvStack
		{
			std::uint64_t dl;
			std::uint64_t inst;
			std::uint32_t count;
		};

		static SpikeInvStack SpikeFindInventoryStack(RE::PlayerCharacter* a_owner, const std::uint32_t a_formID)
		{
			SpikeInvStack out{};
			if (!a_owner) {
				return out;
			}
			auto guard = a_owner->inventoryList.LockRead();
			const auto* inventory = *guard;
			if (!inventory) {
				return out;
			}
			for (const auto& item : inventory->data) {
				if (!item.object || item.object->GetFormID() != a_formID) {
					continue;
				}
				for (const auto& stack : item.stacks) {
					if (stack.count > 0 && stack.extra) {
						out.dl = reinterpret_cast<std::uint64_t>(stack.extra.get());
						out.inst = reinterpret_cast<std::uint64_t>(item.instanceData.get());
						out.count = stack.count;
					}
				}
			}
			return out;
		}

		static std::uint32_t SpikeRetireRef(RE::TESObjectREFR* a_ref)
		{
			__try
			{
				// Move first, delete second.
				//
				// SetDelete is honoured for rendering straight away, but this ref
				// was placed dynamically by our own drop, and the deleted flag does
				// not reliably survive being written to a save and read back. That
				// is what put a copy of a purchased item back on the ground after a
				// load (2026-10-26: a ring of them around the player). By the time
				// this runs the item itself is already in the inventory, so parking
				// the ref far below the cell costs nothing and makes the leftover
				// harmless whether or not the delete is persisted.
				//
				// A plain write to the ref's position, before the delete, so there
				// is no use of a possibly-freed object: the ref has already passed
				// SpikeRefIsBound, and no engine call is involved.
				a_ref->data.location.z -= 20000.0f;
				// The flag that actually decides whether it comes back.
				//
				// The engine makes a ref persistent when the player drops it,
				// and a persistent ref is serialised into the save. SetDelete only
				// marks it; the record still goes into the file, so on load the
				// engine rebuilds the ref and the parcel is back at its drop
				// position as a second copy of the item. That is what the
				// 2026-10-26 test showed exactly: nothing on the floor after the
				// purchase, the whole ring back after the load.
				//
				// Clearing the persistent flag makes it a temporary ref instead:
				// the cell owns it, the save does not carry it, and the cell
				// discards it. By this point the item is in the inventory and
				// this ref's instance data is not the inventory's (the caller
				// checks that before getting here), so nothing else refers to it.
				a_ref->SetPersistent(false);
				a_ref->SetDelete(true);
				// And the flag that decides whether the player ever sees it again.
				//
				// The 2026-10-26 test bought four items and all four refs logged
				// `gone; nothing of this drop is left in the cell` one frame later,
				// and the same four parcels were still on the pad after the load.
				// The handle lookup answers nothing for a ref we just deleted, so
				// "gone" never proved the ref was gone - and the two flags above,
				// whatever they say, are not enough to keep a ref the engine has
				// already promoted out of a save it was written into.
				//
				// A disabled ref is not drawn, has no collision and cannot be
				// activated, and that survives the save whether or not the other
				// two flags do. The item is already in the inventory by now, so
				// nothing that reads this ref still has a use for it - the worst
				// case is an invisible record the cell eventually discards.
				a_ref->formFlags.set(true, RE::TESForm::FormFlags::kDisabled);
				return 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return GetExceptionCode();
			}
		}

		// One place that disposes of the drop reference once the item is in the
		// inventory, whichever stage actually delivered it.
		//
		// Two leftovers came from the delivery block, both reported on 2026-10-26
		// as purchased items reappearing on the ground after a save and load:
		//
		//  * the ref was only retired inside the "instance add confirmed" branch,
		//    so a delivery confirmed by the take request or by activation - the
		//    fallbacks below that branch - left its ref standing at the player's
		//    feet with nothing to remove it;
		//  * a retired ref could still return on load (see SpikeRetireRef).
		//
		// So: dispose of it after the outcome is known, and park it far below the
		// cell as well as deleting it. The exception is the shared-instance case,
		// where the ref cannot be deleted without freeing the object the inventory
		// entry now holds; parking it is all that is safe, and it is enough.
		static void SpikeDisposeDeliveryRef(RE::PlayerCharacter* a_player, RE::TESObjectREFR* a_ref,
			RE::TESBoundObject* a_bound, std::uint32_t a_formID, const char* a_why)
		{
			if (!a_ref || !SpikeRefIsBound(a_ref, a_bound)) {
				return;
			}
			const auto stack = SpikeFindInventoryStack(a_player, a_formID);
			const std::uint64_t refInst = SpikeRefInstance(a_ref);
			if (refInst && stack.inst == refInst) {
				a_ref->data.location.z -= 20000.0f;
				// Kept alive because the inventory holds this instance, but it is
				// still a world ref the player could see, so it is hidden the same
				// way a retired one is. Disabling touches rendering only; the
				// inventory entry is unaffected.
				a_ref->formFlags.set(true, RE::TESForm::FormFlags::kDisabled);
				REX::WARN("SFShopExplorer: [spike] {}: world ref shares the inventory instance so it was "
					"kept, not deleted; parked at z={:.0f} and disabled so it cannot read as a dropped duplicate",
					a_why, a_ref->data.location.z);
				return;
			}
			const std::uint32_t code = SpikeRetireRef(a_ref);
			// persistent=0 is the part that keeps it out of the save; it is read
			// back so a log can prove the flag actually changed.
			REX::INFO("SFShopExplorer: [spike] {}: world ref retired code=0x{:X} persistent={} parked z={:.0f}",
				a_why, code, a_ref->IsPersistent() ? 1 : 0, a_ref->data.location.z);
		}

		// Second pass over the same ref, one task later.
		//
		// The retire above clears both flags, and the log proves it did. A
		// 2026-10-26 report still had items come back after a load, with every
		// purchase logged identically - so the ref was a temporary one at the
		// moment of the purchase and something put it back before the save was
		// written. The only thing that runs in between is the engine's own
		// end-of-frame cell update, which is why the second pass happens in a
		// queued task and not in the same tick.
		//
		// The ref is reached through the drop's handle rather than the pointer
		// captured at delivery: the first pass deleted it, so the pointer may be
		// stale by now, while the handle lookup is SEH-guarded. A candidate is
		// only touched when it still names the same base form.
		//
		// What this line is NOT is proof of disposal. The 17:46 run on 2026-10-26
		// logged `gone` for all four purchases and the same four parcels were on
		// the pad after the load: a deleted ref stops answering its handle, so
		// `gone` cannot tell "the cell dropped it" from "we just deleted it and
		// the lookup went blind". The real guarantee is the disabled flag set in
		// SpikeRetireRef; this pass exists to apply the same three flags after the
		// cell update, and its line says which state the ref is actually in.
		static void SpikeReRetireLate(const std::uint32_t a_handle, const std::uint32_t a_formID)
		{
			if (a_handle == 0) {
				return;
			}
			const auto probe = SpikeResolveHandle(a_handle);
			RE::TESObjectREFR* cands[2]{ probe.viaRet, probe.viaOut };
			for (auto* ref : cands) {
				if (!ref) {
					continue;
				}
				// POD only under __try: reading a freed ref is the risk here, and
				// a smart pointer local would need unwinding (C2712).
				bool ours = false;
				bool wasPersistent = false;
				bool wasDeleted = false;
				bool wasDisabled = false;
				bool nowPersistent = false;
				bool nowDeleted = false;
				bool nowDisabled = false;
				float z = 0.0f;
				__try
				{
					if (SpikeReadable(ref, false)) {
						const auto* base = ref->data.objectReference.get();
						if (base && base->GetFormID() == a_formID) {
							ours = true;
							wasPersistent = ref->IsPersistent();
							wasDeleted = ref->IsDeleted();
							wasDisabled = ref->IsDisabled();
							z = ref->data.location.z;
							ref->SetPersistent(false);
							ref->SetDelete(true);
							ref->formFlags.set(true, RE::TESForm::FormFlags::kDisabled);
							nowPersistent = ref->IsPersistent();
							nowDeleted = ref->IsDeleted();
							nowDisabled = ref->IsDisabled();
						}
					}
				}
				__except (EXCEPTION_EXECUTE_HANDLER)
				{
					ours = false;
				}
				if (!ours) {
					continue;
				}
				REX::INFO("SFShopExplorer: [spike] leftover recheck handle={:08X} persistent={}->{} deleted={}->{} disabled={}->{} z={:.0f}",
					a_handle, wasPersistent ? 1 : 0, nowPersistent ? 1 : 0,
					wasDeleted ? 1 : 0, nowDeleted ? 1 : 0,
					wasDisabled ? 1 : 0, nowDisabled ? 1 : 0, z);
				return;
			}
			REX::INFO("SFShopExplorer: [spike] leftover recheck handle={:08X} no answer from the handle "
				"(the ref is deleted, so this says nothing either way)", a_handle);
		}

		// Stage E2 (0.8.340): a COMPLETE take request.
		//
		// Build 85's move request carried only object + count, so the source
		// lookup matched nothing and the stack stayed on the floor. A loose
		// ref does own an inventory list; what the request was missing is the
		// identity of the entry it should take - the ref's own instance data
		// (ref+0x98) and extra data list (ref+0xC8, the one carrying the
		// attached OMODs). With both supplied, RemoveItem moves THAT stack to
		// moveTo, so the modified instance lands in the player's inventory
		// instead of needing a manual pickup.
		//
		// The request is assembled in a flat buffer: RemoveItemRequest's
		// instance-data field is a one-word smart pointer, and the ref itself
		// holds that reference for the duration of the synchronous call, so
		// nothing here increments or releases a refcount (and no non-POD
		// object lives across the __try, which C2712 forbids).
		struct SpikeTakeReport
		{
			std::uint32_t code;      // SEH code (0 = clean)
			std::uint32_t handle;    // RemoveItem's out handle
			std::uint8_t  inst;      // instance data supplied
			std::uint8_t  extra;     // extra data list supplied
		};

		static SpikeTakeReport SpikeTakeToInventory(RE::TESObjectREFR* a_ref,
			RE::TESBoundObject* a_bound, std::uint32_t a_count, RE::TESObjectREFR* a_player)
		{
			SpikeTakeReport r{};
			__try
			{
				const auto* rb = reinterpret_cast<const std::uint8_t*>(a_ref);
				std::uint64_t inst = 0;
				std::uint64_t extra = 0;
				std::memcpy(&inst, rb + 0x98, sizeof(inst));
				std::memcpy(&extra, rb + 0xC8, sizeof(extra));
				r.inst = inst ? 1 : 0;
				r.extra = extra ? 1 : 0;
				// object, instanceData, extra, {index,count}, reason, pad,
				// moveTo, dropLocation, dropRotation.
				std::uint64_t req[8]{};
				req[0] = reinterpret_cast<std::uintptr_t>(a_bound);
				req[1] = inst;
				req[2] = extra;
				const std::int32_t indexCount[2]{ -1, static_cast<std::int32_t>(a_count) };
				std::memcpy(&req[3], indexCount, sizeof(indexCount));
				const std::int32_t reason = static_cast<std::int32_t>(RE::ITEM_TRANSFER_REASON::kTakeFromActivator);
				std::memcpy(&req[4], &reason, sizeof(reason));
				req[5] = reinterpret_cast<std::uintptr_t>(a_player);
				std::uint32_t handle = 0;
				a_ref->RemoveItem(handle, *reinterpret_cast<const RE::RemoveItemRequest*>(&req[0]));
				r.handle = handle;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				r.code = GetExceptionCode();
			}
			return r;
		}

		static std::uint32_t SpikeInventoryObjectCount(RE::TESObjectREFR* a_owner, const std::uint32_t a_formID)
		{
			if (!a_owner) {
				return 0;
			}
			auto guard = a_owner->inventoryList.LockRead();
			const auto* inventory = *guard;
			if (!inventory) {
				return 0;
			}
			std::uint32_t count = 0;
			for (const auto& item : inventory->data) {
				if (!item.object || item.object->GetFormID() != a_formID) {
					continue;
				}
				for (const auto& stack : item.stacks) {
					if (stack.count > 0) {
						count += stack.count;
					}
				}
			}
			return count;
		}

		// Stage D (build 89): direct call into the game's AttachMod
		// implementation. The console parse path (executeFunction)
		// expects a compiled script buffer (type 0x3D params route
		// through a 'r' marker plus a scriptLocals variable resolve)
		// that a plugin cannot produce reliably, so the executor's
		// actual work function is called instead: Address Library ID
		// 40714 = RVA 0x4024F0 = AttachMod(ref, omodForm, index, arg4),
		// decoded from the AttachMod executor at RVA 0xC78BB0. The
		// executor's own gates are mirrored before the call: ref+0x98
		// non-null, and the byte at form+0x2E == 0x98 (the executor
		// logs "Parameter must be object mod type" otherwise). The
		// console's one-argument form passes index=0, arg4=1 (the
		// executor's +0x50/+0x54 defaults; index > 0xFF logs "Invalid
		// attach index, defaulting to 1"). The executor discards the
		// impl's return, so success surfaces as a pre/post diff of the
		// ref+0x90..+0xA8 qword window in the caller's log. POD-only
		// under __try; the relocation lives outside it (C2712).
		struct SpikeAttachReport
		{
			std::uint32_t code;      // SEH code (0 = clean)
			bool          called;    // impl invoked
			bool          gate98;    // ref+0x98 non-null (executor gate)
			bool          gateType;  // formType == 0x98 (executor gate)
			std::uint8_t  formType;  // raw byte at form+0x2E
			std::uint64_t win[4][2]; // ref+0x90..+0xA8 before/after
		};

		using SpikeAttachFn = void (*)(RE::TESObjectREFR*, RE::TESForm*, std::int32_t, std::int32_t);
		static REL::Relocation<SpikeAttachFn>& SpikeAttachRel()
		{
			static REL::Relocation<SpikeAttachFn> fn{ REL::ID{ 40714 } };
			return fn;
		}

		static SpikeAttachReport SpikeAttachStage(RE::TESObjectREFR* a_ref, RE::TESForm* a_omod,
			const std::int32_t a_index, const std::int32_t a_arg4)
		{
			auto& rel = SpikeAttachRel();  // magic-static init outside the __try region
			SpikeAttachReport r{};
			__try
			{
				const auto* rb = reinterpret_cast<const std::uint8_t*>(a_ref);
				const auto* fb = reinterpret_cast<const std::uint8_t*>(a_omod);
				std::uint64_t ref98 = 0;
				std::memcpy(&ref98, rb + 0x98, sizeof(ref98));
				r.gate98 = ref98 != 0;
				r.formType = fb[0x2E];
				r.gateType = r.formType == 0x98;
				for (std::size_t i = 0; i < 4; ++i) {
					std::memcpy(&r.win[i][0], rb + 0x90 + i * 8, 8);
				}
				if (r.gate98 && r.gateType) {
					rel(a_ref, a_omod, a_index, a_arg4);
					r.called = true;
				}
				for (std::size_t i = 0; i < 4; ++i) {
					std::memcpy(&r.win[i][1], rb + 0x90 + i * 8, 8);
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				r.code = GetExceptionCode();
			}
			return r;
		}

		// Build 90 ground truth (Rattler preview-vs-delivery mismatch):
		// the attach worker at RVA 0x403110 fetches or creates the ref's
		// ExtraDataList entry of type 0x33 through the game's own finder,
		// and the object instance rides ref+0x98. Address Library ID
		// 37878 = RVA 0x347170 = FindExtraByType(extraDataList, type),
		// decoded from the worker's first instructions. Two guarded hex
		// dumps (the extra, the instance) plus per-pick byte presence
		// and pointer-scanned formID/type reads tell the log exactly
		// WHICH picks are present after the attach loop - the ref+0x90
		// qword window (diff=0 every time) cannot see mod lists.
		// POD-only under __try; the relocation lives outside it.
		struct SpikeModsDump
		{
			std::uint32_t code;                   // SEH code (0 = clean)
			std::uint64_t e33;                    // FindExtraByType(dl, 0x33)
			std::uint64_t inst;                   // raw qword at ref+0x98
			std::uint64_t hex33[16];              // extra33 bytes 0x00..0x80
			std::uint64_t hexInst[16];            // instance bytes 0x00..0x80
			std::uint8_t  in33[16];               // pick[i] bytes in extra33
			std::uint8_t  inInst[16];             // pick[i] bytes in instance
			std::uint32_t pickCount;
			std::uint32_t live[24];               // pointer-scanned formIDs
			std::uint32_t liveCount;
			// Build 91: the list the store writes and every consumer
			// iterates lives at extra+0x18 {u32 count, u32 cap, data*};
			// build 90's post dump showed count=0 while cap=4 and data*
			// were set - each attach should bump count via the find
			// fast path (0x1422CC0D0), so the trajectory across the
			// loop and the physical slots behind data* tell whether the
			// count is never raised or raised then reset, and which
			// form pointers landed.
			std::uint32_t cnt18;                  // u32 extra+0x18 (count)
			std::uint32_t cnt1C;                  // u32 extra+0x1C (cap)
			std::uint64_t data1;                  // qword extra+0x20
			std::uint32_t slotCount;              // physical slots dumped
			std::uint64_t slotForm[8];            // entry form pointer
			std::uint32_t slotFid[8];             // resolved formID
			std::uint8_t  slotB[8][3];            // entry bytes +8,+9,+A
		};

		using SpikeFindExtraFn = void* (*)(void*, std::uint8_t);
		static REL::Relocation<SpikeFindExtraFn>& SpikeFindExtraRel()
		{
			static REL::Relocation<SpikeFindExtraFn> fn{ REL::ID{ 37878 } };
			return fn;
		}

		static SpikeModsDump SpikeCollectMods(RE::TESObjectREFR* a_ref,
			const std::uint32_t* a_picks, std::uint32_t a_pickCount)
		{
			auto& findRel = SpikeFindExtraRel();  // magic-static init outside the __try
			SpikeModsDump d{};
			d.pickCount = a_pickCount > 16 ? 16 : a_pickCount;
			__try
			{
				const auto* rb = reinterpret_cast<const std::uint8_t*>(a_ref);
				void* dl = nullptr;
				std::memcpy(&dl, rb + 0xC8, sizeof(dl));
				void* e33 = nullptr;
				if (dl && findRel.get()) {
					e33 = findRel(dl, 0x33);
				}
				d.e33 = reinterpret_cast<std::uintptr_t>(e33);
				if (e33) {
					const auto* b = reinterpret_cast<const std::uint8_t*>(e33);
					for (std::size_t o = 0; o < 0x80; o += 8) {
						if (!SpikeReadable(b + o, false)) {
							break;
						}
						std::memcpy(d.hex33 + (o / 8), b + o, 8);
					}
					// Build 91: {count,cap,data*} of the store list.
					std::memcpy(&d.cnt18, b + 0x18, 4);
					std::memcpy(&d.cnt1C, b + 0x1C, 4);
					std::memcpy(&d.data1, b + 0x20, 8);
					// Physical slots behind data*: bounded by cap (the
					// allocator's own extent), clamped to 8, each guarded
					// - stale slot bytes still name the forms even when
					// the live count reads 0.
					std::uint32_t cap = d.cnt1C > 8 ? 8 : d.cnt1C;
					if (d.data1 >= 0x100000000ULL && cap > 0) {
						const auto* sb = reinterpret_cast<const std::uint8_t*>(d.data1);
						for (std::uint32_t s = 0; s < cap; ++s) {
							const auto* e = sb + static_cast<std::size_t>(s) * 0x10;
							if (!SpikeReadable(e, false) || !SpikeReadable(e + 0xF, false)) {
								break;
							}
							d.slotCount = s + 1;
							std::memcpy(&d.slotForm[s], e, 8);
							d.slotB[s][0] = e[8];
							d.slotB[s][1] = e[9];
							d.slotB[s][2] = e[0xA];
							const auto fp = d.slotForm[s];
							if (fp >= 0x100000000ULL && SpikeReadable(reinterpret_cast<const void*>(fp), false)) {
								const auto* fb = reinterpret_cast<const std::uint8_t*>(fp);
								if (SpikeReadable(fb + 0x2E, false)) {
									std::memcpy(&d.slotFid[s], fb + 0x28, 4);
								}
							}
						}
					}
				}
				std::uint64_t inst = 0;
				std::memcpy(&inst, rb + 0x98, sizeof(inst));
				d.inst = inst;
				if (inst >= 0x100000000ULL) {
					const auto* b = reinterpret_cast<const std::uint8_t*>(inst);
					for (std::size_t o = 0; o < 0x80; o += 8) {
						if (!SpikeReadable(b + o, false)) {
							break;
						}
						std::memcpy(d.hexInst + (o / 8), b + o, 8);
					}
				}
				// Per-pick 4-byte presence (fid is stored LE somewhere in
				// either blob if the pick is held there as a raw formID).
				for (std::uint32_t p = 0; p < d.pickCount; ++p) {
					std::uint8_t f[4]{};
					std::memcpy(f, &a_picks[p], 4);
					for (std::size_t o = 0; o + 4 <= 0x80; ++o) {
						const auto* b = reinterpret_cast<const std::uint8_t*>(d.hex33);
						if (b[o] == f[0] && b[o + 1] == f[1] && b[o + 2] == f[2] && b[o + 3] == f[3]) {
							d.in33[p] = 1;
							break;
						}
					}
					for (std::size_t o = 0; o + 4 <= 0x80; ++o) {
						const auto* b = reinterpret_cast<const std::uint8_t*>(d.hexInst);
						if (b[o] == f[0] && b[o + 1] == f[1] && b[o + 2] == f[2] && b[o + 3] == f[3]) {
							d.inInst[p] = 1;
							break;
						}
					}
				}
				// Pointer scan: 8-aligned qwords that look like heap
				// pointers and read back as forms of type 0x98 (the
				// AttachMod gate byte at form+0x2E) get their formID
				// (TESForm+0x28) logged. This finds mods even when they
				// are stored as form pointers, not raw formIDs.
				const std::uint8_t* blobs[2]{
					reinterpret_cast<const std::uint8_t*>(d.hex33),
					reinterpret_cast<const std::uint8_t*>(d.hexInst)
				};
				for (const auto* blob : blobs) {
					for (std::size_t o = 0; o + 8 <= 0x80; o += 8) {
						std::uint64_t q = 0;
						std::memcpy(&q, blob + o, 8);
						if (q < 0x100000000ULL || q > 0x7FFFFFFFFFFFULL ||
							!SpikeReadable(reinterpret_cast<const void*>(q), false)) {
							continue;
						}
						const auto* fb = reinterpret_cast<const std::uint8_t*>(q);
						if (fb[0x2E] != 0x98) {
							continue;
						}
						std::uint32_t fid = 0;
						std::memcpy(&fid, fb + 0x28, 4);
						if (fid == 0) {
							continue;
						}
						bool seen = false;
						for (std::uint32_t k = 0; k < d.liveCount && k < 24; ++k) {
							if (d.live[k] == fid) {
								seen = true;
								break;
							}
						}
						if (!seen && d.liveCount < 24) {
							d.live[d.liveCount++] = fid;
						}
					}
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				d.code = GetExceptionCode();
			}
			return d;
		}

		// Four [spike] lines per phase (pre/post): state, extra33 hex,
		// instance hex, per-pick presence + pointer-scanned live fids.
		// String building happens OUTSIDE __try (C2712).
		static void SpikeLogMods(const char* a_phase, const SpikeModsDump& d,
			const std::uint32_t* a_picks, std::uint32_t a_pickCount)
		{
			std::string h33;
			std::string hInst;
			std::string pk;
			std::string live;
			for (std::size_t i = 0; i < 16; ++i) {
				h33 += std::format("{:016X} ", d.hex33[i]);
				hInst += std::format("{:016X} ", d.hexInst[i]);
			}
			const auto n = a_pickCount > 16 ? 16 : a_pickCount;
			for (std::uint32_t p = 0; p < n; ++p) {
				if (!pk.empty()) {
					pk += ' ';
				}
				pk += std::format("{:08X}:{}{}", a_picks[p],
					d.in33[p] ? 'E' : '-', d.inInst[p] ? 'I' : '-');
			}
			for (std::uint32_t i = 0; i < d.liveCount; ++i) {
				if (!live.empty()) {
					live += ',';
				}
				live += std::format("{:08X}", d.live[i]);
			}
			std::string slots;
			for (std::uint32_t s = 0; s < d.slotCount; ++s) {
				if (!slots.empty()) {
					slots += ' ';
				}
				slots += std::format("[{:08X} {:02X}{:02X}{:02X}]",
					d.slotFid[s], d.slotB[s][0], d.slotB[s][1], d.slotB[s][2]);
			}
			REX::INFO("SFShopExplorer: [spike] mods {} e33=0x{:X} inst=0x{:X} seh=0x{:X}",
				a_phase, d.e33, d.inst, d.code);
			if (SpikeDebug()) {
				REX::INFO("SFShopExplorer: [spike] mods {} hex33={}", a_phase, h33);
				REX::INFO("SFShopExplorer: [spike] mods {} hexInst={}", a_phase, hInst);
				REX::INFO("SFShopExplorer: [spike] mods {} pick={} live=[{}]", a_phase, pk, live);
			}
			REX::INFO("SFShopExplorer: [spike] mods {} cnt18={} cnt1C={} data1=0x{:X} slots={}",
				a_phase, d.cnt18, d.cnt1C, d.data1, slots.empty() ? "-" : slots);
		}

		// Build 93: pin the reconcile keep/drop decision that zeroes
		// extra+0x18 after every attach. The worker's sync (RVA
		// 0x403B00) runs phase 4 (RVA 0x404160) over the extra33 list:
		// NOT-FOUND in the reconcile set calls remove-at (RVA
		// 0x403440) on extra33 and stops after one entry, which is
		// exactly the observed trajectory (store bumps count 0->1,
		// sync drops it back to 0). FOUND needs [form+0xE0] inside
		// the set, and the set is the union of two records fetched
		// by the keyed get-or-create lookup at RVA 0x5DDBD0: one on
		// the object instance (the worker's own post-store call) and
		// one per entry form (the collect pass at RVA 0x406390).
		// Both use the same resolver RVA 0x42C7B0 and the same key
		// word at image RVA 0x61DD4FF (verified from both call
		// sites). This probe dumps each record raw (out pair, header
		// qwords, the [ptr+8, ptr+0x10) qword range when it parses
		// as one) plus every pick's [form+0xE0]/+0x20/+0x28 and the
		// membership verdict, pre and post the attach loop - the
		// next build seeds whichever record the set is missing.
		// POD-only under __try; the relocations and GetModuleHandle
		// live outside it, string building happens in the logger.
		struct SpikeRecordDump
		{
			std::uint32_t code;        // SEH code (0 = clean)
			std::uint32_t phase;       // 0 = pre, 1 = post
			std::uint16_t keyWord;     // u16 at image+0x61DD4FF
			std::uint64_t res;         // RVA 0x42C7B0 return
			std::uint64_t instPtr;     // raw ref+0x98
			std::uint64_t inst28;      // u64 [instance+0x28] (lookup arg)
			std::uint64_t instOut[2];  // 0x5DDBD0(instance) out pair
			std::uint64_t instHdr[6];  // raw [instOut0 + 0x00..0x2F]
			std::uint8_t  instVecOk;   // hdr+8/hdr+0x10 read as [begin,end)
			std::uint32_t instVecN;
			std::uint64_t instVec[16];
			std::uint32_t nPicks;
			std::uint32_t pickFid[16];  // input fid (log correlation)
			std::uint64_t pickE0[16];  // [form+0xE0] (phase-4 key)
			std::uint32_t pick20[16];  // [form+0x20] (flag bits)
			std::uint32_t pick28[16];  // [form+0x28] (worker containment)
			std::uint64_t omodOut[16][2];
			std::uint64_t omodHdr0[6]; // pick 0 only
			std::uint32_t omodVecN[16];
			std::uint8_t  omodVecOk[16];
			std::uint8_t  e0InInst[16];
			std::uint8_t  e0InOmod[16];
		};

		using SpikeKeyResFn = void* (*)();
		using SpikeKeyLookupFn = void* (*)(void*, void*, std::uint32_t, void*);

		// Parse [begin,end) as a qword range: both non-null, end past
		// begin, byte delta divisible by 8 and within 0x400 (the
		// reconcile vectors hold a handful of mod keys). Returns the
		// count read; 0 with false when the header is not a range.
		static bool SpikeRangeRead(std::uint64_t a_begin, std::uint64_t a_end,
			std::uint64_t* a_out, std::uint32_t a_cap, std::uint32_t* a_n)
		{
			*a_n = 0;
			if (!a_begin || a_end <= a_begin || (a_end - a_begin) % 8 != 0 || a_end - a_begin > 0x400) {
				return false;
			}
			auto* b = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(a_begin));
			auto* e = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(a_end));
			for (std::uint32_t i = 0; i < a_cap && b < e; ++i, b += 8) {
				if (!SpikeReadable(b, false)) {
					return false;
				}
				std::memcpy(a_out + i, b, 8);
				++*a_n;
			}
			return true;
		}

		static bool SpikeRangeHas(const std::uint64_t* a_vec, std::uint32_t a_n, std::uint64_t a_key)
		{
			for (std::uint32_t i = 0; i < a_n; ++i) {
				if (a_vec[i] == a_key) {
					return true;
				}
			}
			return false;
		}

		static SpikeRecordDump SpikeRecordProbe(RE::TESObjectREFR* a_ref,
			const std::uint32_t* a_picks, std::uint32_t a_pickCount, std::uint32_t a_phase)
		{
			// Image base and the two static-code pointers resolve
			// before __try (raw RVAs - these functions never move,
			// and a plain pointer keeps any constructor out of the
			// guarded region, C2712 - same posture as CollectMods).
			//
			// Raw RVAs are right for exactly one build. An SEH frame catches an
			// access violation here, but it cannot make calling into an
			// unrelated .text address safe, so the probe refuses itself unless
			// the running build is the one the addresses came from.
			SpikeRecordDump refused{};
			refused.phase = a_phase;
			if (!BuildGate::Verified()) {
				BuildGate::Refuse("mod-application probe (raw .text addresses)");
				refused.code = 2;  // logged as seh=0x2 = refused, not an exception
				return refused;
			}
			auto* base = GetModuleHandleA(nullptr);
			auto* img = reinterpret_cast<const std::uint8_t*>(base);
			auto* resFn = reinterpret_cast<SpikeKeyResFn>(const_cast<std::uint8_t*>(img) + 0x42C7B0);
			auto* lookFn = reinterpret_cast<SpikeKeyLookupFn>(const_cast<std::uint8_t*>(img) + 0x5DDBD0);
			SpikeRecordDump d{};
			d.phase = a_phase;
			d.nPicks = a_pickCount > 16 ? 16 : a_pickCount;
			if (!base) {
				d.code = 1;  // no image - logged as seh=0x1
				return d;
			}
			__try
			{
				d.keyWord = *reinterpret_cast<const std::uint16_t*>(img + 0x61DD4FF);
				d.res = reinterpret_cast<std::uint64_t>(resFn());
				const auto* rb = reinterpret_cast<const std::uint8_t*>(a_ref);
				std::memcpy(&d.instPtr, rb + 0x98, 8);
				auto* inst = reinterpret_cast<void*>(static_cast<std::uintptr_t>(d.instPtr));
				if (inst && SpikeReadable(static_cast<const std::uint8_t*>(inst) + 0x28, false)) {
					std::memcpy(&d.inst28, static_cast<const std::uint8_t*>(inst) + 0x28, 8);
					lookFn(inst, d.instOut, static_cast<std::uint32_t>(d.keyWord),
						reinterpret_cast<void*>(static_cast<std::uintptr_t>(d.res)));
					if (d.instOut[0] && SpikeReadable(reinterpret_cast<const std::uint8_t*>(
							static_cast<std::uintptr_t>(d.instOut[0])), false)) {
						auto* hb = reinterpret_cast<const std::uint8_t*>(
							static_cast<std::uintptr_t>(d.instOut[0]));
						for (std::size_t o = 0; o < 0x30; o += 8) {
							if (!SpikeReadable(hb + o, false)) {
								break;
							}
							std::memcpy(d.instHdr + (o / 8), hb + o, 8);
						}
						d.instVecOk = SpikeRangeRead(d.instHdr[1], d.instHdr[2],
							d.instVec, 16, &d.instVecN) ? 1 : 0;
					}
				}
				for (std::uint32_t p = 0; p < d.nPicks; ++p) {
					d.pickFid[p] = a_picks[p];
					auto* form = RE::TESForm::LookupByID(a_picks[p]);
					if (!form) {
						continue;
					}
					auto* fb = reinterpret_cast<const std::uint8_t*>(form);
					if (!SpikeReadable(fb + 0xE0, false)) {
						continue;
					}
					std::memcpy(&d.pickE0[p], fb + 0xE0, 8);
					std::memcpy(&d.pick20[p], fb + 0x20, 4);
					std::memcpy(&d.pick28[p], fb + 0x28, 4);
					lookFn(form, d.omodOut[p], static_cast<std::uint32_t>(d.keyWord),
						reinterpret_cast<void*>(static_cast<std::uintptr_t>(d.res)));
					if (p == 0 && d.omodOut[p][0] && SpikeReadable(reinterpret_cast<const std::uint8_t*>(
							static_cast<std::uintptr_t>(d.omodOut[p][0])), false)) {
						auto* ohb = reinterpret_cast<const std::uint8_t*>(
							static_cast<std::uintptr_t>(d.omodOut[p][0]));
						for (std::size_t o = 0; o < 0x30; o += 8) {
							if (!SpikeReadable(ohb + o, false)) {
								break;
							}
							std::memcpy(d.omodHdr0 + (o / 8), ohb + o, 8);
						}
					}
					if (d.omodOut[p][0]) {
						std::uint64_t tmp[16]{};
						std::uint32_t n = 0;
						const auto* hdr = p == 0 ? d.omodHdr0 : nullptr;
						std::uint64_t begin = 0;
						std::uint64_t end = 0;
						if (hdr) {
							begin = hdr[1];
							end = hdr[2];
						} else {
							auto* hb = reinterpret_cast<const std::uint8_t*>(
								static_cast<std::uintptr_t>(d.omodOut[p][0]));
							if (SpikeReadable(hb + 0x10, false)) {
								std::memcpy(&begin, hb + 8, 8);
								std::memcpy(&end, hb + 0x10, 8);
							}
						}
						d.omodVecOk[p] = SpikeRangeRead(begin, end, tmp, 16, &n) ? 1 : 0;
						d.omodVecN[p] = n;
						d.e0InOmod[p] = SpikeRangeHas(tmp, n, d.pickE0[p]) ? 1 : 0;
					}
					d.e0InInst[p] = SpikeRangeHas(d.instVec, d.instVecN, d.pickE0[p]) ? 1 : 0;
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				d.code = GetExceptionCode();
			}
			return d;
		}

		// [spike] rec lines per phase: record header + parsed vector,
		// then one line per pick with [form+0xE0], the omod record
		// pair/vector and both membership verdicts. Strings are built
		// here, outside any __try.
		static void SpikeLogRecord(const SpikeRecordDump& d)
		{
			// These dumps are per pick (eleven lines each on a full weapon),
			// so they live behind [shop] debugSpike now that the resolution
			// and delivery lines answer the question the log is read for.
			if (!SpikeDebug()) {
				return;
			}
			const char* phase = d.phase ? "post" : "pre";
			std::string hdr;
			std::string vec;
			for (std::size_t i = 0; i < 6; ++i) {
				hdr += std::format("{:016X} ", d.instHdr[i]);
			}
			for (std::uint32_t i = 0; i < d.instVecN; ++i) {
				vec += std::format("{:016X} ", d.instVec[i]);
			}
			REX::INFO("SFShopExplorer: [spike] rec {} seh=0x{:X} key={} res=0x{:X} inst=0x{:X} inst28=0x{:X} out={:016X} {:016X}",
				phase, d.code, d.keyWord, d.res, d.instPtr, d.inst28, d.instOut[0], d.instOut[1]);
			REX::INFO("SFShopExplorer: [spike] rec {} hdr={} vecOk={} vecN={} vec=[{}]",
				phase, hdr, d.instVecOk ? 1 : 0, d.instVecN, vec.empty() ? "-" : vec);
			std::string oh0;
			for (std::size_t i = 0; i < 6; ++i) {
				oh0 += std::format("{:016X} ", d.omodHdr0[i]);
			}
			REX::INFO("SFShopExplorer: [spike] rec {} omod0hdr={}", phase, oh0);
			for (std::uint32_t p = 0; p < d.nPicks; ++p) {
				// Recompute nothing here - the vector contents were
				// only needed for membership; log the verdict and the
				// count (full per-pick ranges would bury the log).
				REX::INFO("SFShopExplorer: [spike] rec {} pick{} fid={:08X} e0={:016X} f20={:08X} f28={:08X} out={:016X} {:016X} vecOk={} vecN={} inInst={} inOmod={}",
					phase, p, d.pickFid[p], d.pickE0[p], d.pick20[p], d.pick28[p],
					d.omodOut[p][0], d.omodOut[p][1], d.omodVecOk[p] ? 1 : 0, d.omodVecN[p],
					d.e0InInst[p] ? 1 : 0, d.e0InOmod[p] ? 1 : 0);
			}
		}

		// Build 95 (notes 6r-4o): post-attach reconcile repair - the
		// mod-persistence fix ("selected legendary, got common").
		// The attach worker (RVA 0x403110) calls store (Address
		// Library ID 1015131 = RVA 0x403530) then sync (ID 40721 =
		// RVA 0x403B00) once PER PICK, with the pair built from the
		// object instance: phase 4 (RVA 0x404160) Source A is that
		// pair's vector at [first+8, first+0x10) and the build-94
		// probe shows it is EMPTY on a fresh drop (vecN=0, inInst=0
		// for every pick, `rec pre` lines). Phase 4 removes any
		// record entry whose [form+0xE0] is not in the reconcile set
		// (set count 0 -> remove all; null-e0 entries early-keep),
		// so after the loop only the null-e0 entry survives (the
		// observed `mods step#` cnt18 trajectory 0,0,0,0,0,0,1,1)
		// and the delivered item loses its quality/slot/legendary
		// rolls.
		// The repair re-runs the tail ONCE with a fake pair whose
		// vector carries every pick's [form+0xE0] key (the exact
		// layout Source A walks - [first+8]=begin, [first+0x10)=end,
		// stride 8, raw e0 values, same parse as SpikeRecordProbe's
		// instVec): each entry is then FOUND (its key is in the
		// set), the entry is kept, and the category-group lookup
		// misses the registry for the fake source address -> keep.
		// Sequence mirrors the worker tail under the same lock
		// (recursive BSReadWriteLock, ID 123863/35983 - the worker
		// itself holds it across these very calls):
		//   store xN -> sync -> mark 0xB6 (ID 44741 = RVA 0x501450,
		//   pure existence check, creates nothing, return unused)
		//   -> flush (ID 45140 = RVA 0x533AC0: creates extra 0xB6
		//   and, only when it had to create it, runs the extra-0x96
		//   builder; when 0xB6 already exists the flush early-
		//   returns and SKIPS the builder) -> builder (ID 45153 =
		//   RVA 0x534D20) called DIRECTLY so extra 0x96 exists
		//   either way (a second run is an idempotent payload
		//   rewrite through RVA 0x54EB30).
		// store signature (record, rdx, form, index, arg4,
		// replace): the impl never reads rdx (its prologue spill is
		// overwritten by a local before any use - verified on the
		// disassembly), and the worker passes index=0, arg4=1,
		// replace=1; store erases the e0+index key when replace=1
		// then appends, growing the list (cap at record+0x1C) via
		// the allocator vfunc when full. sync takes only (record,
		// ref, pair) - r9 is never read. All within-pick e0 keys
		// are distinct (build-94 `rec pre` shows one null-e0 pick
		// per item), so the replace-erase cannot collide.
		// POD-only under __try; relocations, static fake storage
		// and string building live outside it (C2712). The fake
		// storage is function-static (lives for the process) so
		// any registry entry keyed by the fake source address can
		// never dangle.
		struct SpikeRepairReport
		{
			std::uint32_t code;      // SEH code (0 = clean)
			std::uint32_t stores;    // store calls made
			std::uint32_t nE0;       // non-null [form+0xE0] keys collected
			std::uint32_t cntBefore; // record+0x18 before the stores
			std::uint32_t cntMid;    // record+0x18 after the stores
			std::uint32_t cntAfter;  // record+0x18 after sync
			std::uint32_t cap;       // record+0x1C after sync
			std::uint8_t  locked;    // DL+0x20 acquired (unlock in handler)
			std::uint8_t  b6Before;  // FindExtra(dl, 0xB6) before tail
			std::uint8_t  b6After;   // FindExtra(dl, 0xB6) after tail
			std::uint8_t  e96Before; // FindExtra(dl, 0x96) before tail
			std::uint8_t  e96After;  // FindExtra(dl, 0x96) after builder
			std::uint64_t dl;        // ExtraDataList* (ref+0xC8)
			std::uint64_t inst;      // instance (ref+0x98)
			std::uint64_t record;    // FindExtra(dl, 0x33)
			std::uint64_t lockPtr;   // dl+0x20
			std::uint64_t out;       // first qword of the builder out buffer
		};

		using SpikeStoreFn = void (*)(void*, void*, RE::TESForm*, std::uint8_t, std::uint8_t, std::uint8_t);
		using SpikeSyncFn = void (*)(void*, RE::TESObjectREFR*, const void*);
		using SpikeMarkFn = void (*)(void*, std::uint8_t, RE::TESObjectREFR*);
		using SpikeFlushFn = void (*)(void*, RE::TESObjectREFR*, void*, std::uint8_t);
		using SpikeBuilderFn = void (*)(void*, RE::TESObjectREFR*, void*, void*, void*);
		using SpikeLockFn = void (*)(void*);

		static REL::Relocation<SpikeStoreFn>& SpikeStoreRel()
		{
			static REL::Relocation<SpikeStoreFn> fn{ REL::ID{ 1015131 } };
			return fn;
		}

		static REL::Relocation<SpikeSyncFn>& SpikeSyncRel()
		{
			static REL::Relocation<SpikeSyncFn> fn{ REL::ID{ 40721 } };
			return fn;
		}

		static REL::Relocation<SpikeMarkFn>& SpikeMarkRel()
		{
			static REL::Relocation<SpikeMarkFn> fn{ REL::ID{ 44741 } };
			return fn;
		}

		static REL::Relocation<SpikeFlushFn>& SpikeFlushRel()
		{
			static REL::Relocation<SpikeFlushFn> fn{ REL::ID{ 45140 } };
			return fn;
		}

		static REL::Relocation<SpikeBuilderFn>& SpikeBuilderRel()
		{
			static REL::Relocation<SpikeBuilderFn> fn{ REL::ID{ 45153 } };
			return fn;
		}

		static REL::Relocation<SpikeLockFn>& SpikeLockRel()
		{
			static REL::Relocation<SpikeLockFn> fn{ REL::ID{ 123863 } };
			return fn;
		}

		static REL::Relocation<SpikeLockFn>& SpikeUnlockRel()
		{
			static REL::Relocation<SpikeLockFn> fn{ REL::ID{ 35983 } };
			return fn;
		}

		static SpikeRepairReport SpikeRepairMods(RE::TESObjectREFR* a_ref,
			const std::uint32_t* a_picks, std::uint32_t a_pickCount)
		{
			// Magic-static relocations and the static fake-pair
			// storage initialize before __try (C2712; POD with
			// constant initialization needs no unwind guard).
			auto& storeRel = SpikeStoreRel();
			auto& syncRel = SpikeSyncRel();
			auto& markRel = SpikeMarkRel();
			auto& flushRel = SpikeFlushRel();
			auto& builderRel = SpikeBuilderRel();
			auto& lockRel = SpikeLockRel();
			auto& unlockRel = SpikeUnlockRel();
			auto& findRel = SpikeFindExtraRel();
			static std::uint64_t s_e0s[16]{};  // Source A keys (raw [form+0xE0])
			static std::uint64_t s_fake[3]{};  // fake source: {pad, begin, end}
			static std::uint64_t s_pair[2]{};  // {&s_fake, 0}
			SpikeRepairReport r{};
			__try
			{
				const auto* rb = reinterpret_cast<const std::uint8_t*>(a_ref);
				std::memcpy(&r.dl, rb + 0xC8, 8);
				std::memcpy(&r.inst, rb + 0x98, 8);
				auto* dl = reinterpret_cast<void*>(static_cast<std::uintptr_t>(r.dl));
				auto* inst = reinterpret_cast<void*>(static_cast<std::uintptr_t>(r.inst));
				if (!dl || !inst || !findRel.get() || !storeRel.get() || !syncRel.get() ||
					!markRel.get() || !flushRel.get() || !builderRel.get() ||
					!lockRel.get() || !unlockRel.get()) {
					// Missing ref pointer or unresolved relocation ->
					// logged as seh=0x1 with stores=0, no mutation.
					r.code = 1;
					return r;
				}
				// Same lock the worker holds across store/sync/mark/
				// flush (recursive: mark and flush re-lock the word).
				auto* lock = static_cast<std::uint8_t*>(dl) + 0x20;
				r.lockPtr = reinterpret_cast<std::uintptr_t>(lock);
				lockRel(lock);
				r.locked = 1;
				auto* record = findRel(dl, 0x33);
				r.record = reinterpret_cast<std::uintptr_t>(record);
				r.b6Before = findRel(dl, 0xB6) ? 1 : 0;
				r.e96Before = findRel(dl, 0x96) ? 1 : 0;
				if (record) {
					const auto* rec = static_cast<const std::uint8_t*>(record);
					std::memcpy(&r.cntBefore, rec + 0x18, 4);
					// (a) Re-store every pick with the worker's arguments
					// (index=0, arg4=1) EXCEPT replace: build 95 passed
					// replace=1, whose e0+index erase drops an EARLIER
					// entry that shares the slot descriptor - legendary
					// omods roll the same [form+0xE0] key (build-95
					// Rattler: 8 stores -> 7 entries, Frenzy lost to
					// Titanium; Tombstone: Handloading lost to
					// Swashbuckling). replace=0 appends only, so both
					// same-key picks survive the store (cnt18 mid vs
					// after in the log shows whether sync's set-consume
					// then drops the duplicate key).
					const auto n = a_pickCount > 16 ? 16 : a_pickCount;
					for (std::uint32_t p = 0; p < n; ++p) {
						auto* form = RE::TESForm::LookupByID(a_picks[p]);
						if (!form) {
							continue;
						}
						storeRel(record, nullptr, form, 0, 1, 0);
						++r.stores;
						// (b) Collect the phase-4 keys for the fake
						// Source A vector. Null e0s are skipped -
						// phase 4 early-keeps null-e0 entries anyway.
						const auto* fb = reinterpret_cast<const std::uint8_t*>(form);
						if (SpikeReadable(fb + 0xE0, false) && r.nE0 < 16) {
							std::uint64_t e0 = 0;
							std::memcpy(&e0, fb + 0xE0, 8);
							if (e0) {
								s_e0s[r.nE0++] = e0;
							}
						}
					}
					std::memcpy(&r.cntMid, rec + 0x18, 4);
					// (c) One sync over the fake pair: the reconcile
					// set now holds every pick key, so each record
					// entry is FOUND and kept (each key is consumed
					// once). Build 96 note: same-slot keys DUPLICATE
					// across picks (two legendary omods can share one
					// [form+0xE0]) - if the set collapses them, the
					// second entry reads NOT-FOUND and cntAfter
					// < cntMid; the log of both numbers decides
					// whether a follow-up must skip this sync.
					s_fake[0] = 0;
					s_fake[1] = reinterpret_cast<std::uintptr_t>(s_e0s);
					s_fake[2] = reinterpret_cast<std::uintptr_t>(s_e0s + r.nE0);
					s_pair[0] = reinterpret_cast<std::uintptr_t>(s_fake);
					s_pair[1] = 0;
					syncRel(record, a_ref, s_pair);
					std::memcpy(&r.cntAfter, rec + 0x18, 4);
					std::memcpy(&r.cap, rec + 0x1C, 4);
				}
				// (d) Worker tail: mark 0xB6 (existence check only),
				// then flush - which creates extra 0xB6 and runs the
				// 0x96 builder only on that create path.
				markRel(dl, 0xB6, a_ref);
				flushRel(dl, a_ref, inst, 1);
				// (e) Direct builder call so extra 0x96 exists even
				// when 0xB6 was already present (flush early-returns
				// in that case). Out buffer is oversized because the
				// callee routes it into RVA 0x9EB8D0, whose store
				// extent is only pinned by the game's own 8-byte
				// readback.
				std::uint64_t outBuf[8]{};
				builderRel(dl, a_ref, record, inst, outBuf);
				r.out = outBuf[0];
				r.b6After = findRel(dl, 0xB6) ? 1 : 0;
				r.e96After = findRel(dl, 0x96) ? 1 : 0;
				unlockRel(lock);
				r.locked = 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				r.code = GetExceptionCode();
				// Best-effort release: the lock is recursive and
				// this thread owns it, but leaving the depth raised
				// would hang any later taker - the handler has no
				// unwind objects, so calling the already-resolved
				// unlock here is legal (C2712 applies to __try).
				if (r.locked && unlockRel.get()) {
					unlockRel(reinterpret_cast<void*>(static_cast<std::uintptr_t>(r.lockPtr)));
				}
			}
			return r;
		}

		// [spike] repair line: one-shot summary (strings built here,
		// outside __try).
		static void SpikeLogRepair(const SpikeRepairReport& r)
		{
			REX::INFO("SFShopExplorer: [spike] repair seh=0x{:X} dl=0x{:X} inst=0x{:X} rec=0x{:X} stores={} e0s={} cnt18={}->{}->{} cnt1C={} b6={}->{} e96={}->{} out=0x{:X}",
				r.code, r.dl, r.inst, r.record, r.stores, r.nE0,
				r.cntBefore, r.cntMid, r.cntAfter, r.cap,
				r.b6Before, r.b6After, r.e96Before, r.e96After, r.out);
		}

		// Build 85 spike (86 hardening, 89 direct-attach, 98 native
		// pickup, 0.8.338 activation): attach the preview OMODs to a fresh
		// world ref, then have the player ACTIVATE it - the engine's own
		// take path - so the modified instance is what reaches inventory.
		// Do not alias its raw ExtraDataList into AddObjectToContainer
		// (build-88 double-owned it and hung), and do not move it with a
		// RemoveItem request (it never moved a loose ref on 1.16.244).
		// Every unsafe game call is SEH-guarded and logged [spike].
		SpikeOutcome SpikeDeliverWithMods(RE::PlayerCharacter* a_player, RE::TESBoundObject* a_bound,
			std::uint32_t a_count, const std::vector<AssemblyPick>& a_picks)
		{
			if (!a_player || !a_bound || a_count == 0 || a_picks.empty()) {
				return SpikeOutcome::kNotAttempted;
			}
			// The route below drops the stack on the floor and calls into the
			// engine through raw .text addresses. On a build those were not read
			// from, that is a call into arbitrary code - refuse it and let the
			// caller take the plain inventory add instead.
			if (!BuildGate::Verified()) {
				BuildGate::Refuse("item-mod delivery (raw .text addresses)");
				return SpikeOutcome::kNotAttempted;
			}
			// Fail fast before anything mutates: the direct-attach
			// target (Address Library ID 40714 = AttachMod impl) must
			// resolve. The console command table dump is diagnostic
			// only - build 89 never routes through executeFunction.
			if (SpikeAttachRel().get() == nullptr) {
				REX::ERROR("SFShopExplorer: [spike] AttachMod impl (ID 40714) unresolved for {:08X} - plain add",
					a_bound->GetFormID());
				return SpikeOutcome::kNotAttempted;
			}
			auto* fn = RE::Script::LocateConsoleCommand("AttachMod");
			if (!fn) {
				fn = RE::Script::LocateConsoleCommand("amod");
			}
			SpikeLogCommand("AttachMod", fn);

			// Cell diagnostics only - never gates the mutation (the old
			// references[] walk AVs; this dumps raw qwords for offline
			// layout analysis instead).
			auto* cell = a_player->parentCell;
			char cellHex[192]{};
			if (cell && SpikeReadable(cell, false)) {
				SpikeDumpCellLayout(cell, cellHex, sizeof(cellHex));
				REX::INFO("SFShopExplorer: [spike] cell ptr={} layout {}", static_cast<const void*>(cell), cellHex);
			} else {
				REX::INFO("SFShopExplorer: [spike] parentCell {} - cell dump skipped", cell ? "readable" : "null");
			}
			// Build 96: snapshot the reference count BEFORE anything is
			// added/dropped - entries appended after this point are
			// candidates for OUR fresh ref (the walk's tail route).
			const std::uint32_t preCount = SpikeCellCount(cell);
			REX::INFO("SFShopExplorer: [spike] cellcount pre={}", preCount);

			// Add a clean base instance so the game's own drop path can
			// create a reference for AttachMod. Record the count beforehand
			// so the final native return can be verified without guessing.
			const std::uint32_t inventoryBefore = SpikeInventoryObjectCount(a_player, a_bound->GetFormID());
			a_player->AddObjectToContainer(a_bound, RE::BSTSmartPointer<RE::ExtraDataList>(new RE::ExtraDataList()),
				static_cast<std::int32_t>(a_count), nullptr, RE::ITEM_TRANSFER_REASON::kNone);
			REX::INFO("SFShopExplorer: [spike] inventory add x{} {:08X} done weapon='{}' count={}->{}",
				a_count, a_bound->GetFormID(), a_bound->GetFormEditorID() ? a_bound->GetFormEditorID() : "?",
				inventoryBefore, SpikeInventoryObjectCount(a_player, a_bound->GetFormID()));

			// 2) Drop the stack at the player's feet -> creates a world ref.
			RE::NiPoint3 dropPos = a_player->GetPosition();
			RE::RemoveItemRequest drop{};
			drop.object = a_bound;
			drop.count = static_cast<std::int32_t>(a_count);
			drop.reason = RE::ITEM_TRANSFER_REASON::kDropping;
			drop.dropLocation = &dropPos;
			std::uint32_t handle = 0;
			const auto dropCode = SpikeRemoveStage(a_player, &drop, &handle);
			if (dropCode != 0) {
				REX::ERROR("SFShopExplorer: [spike] drop stage AV=0x{:X} - item state uncertain, check floor AND inventory", dropCode);
				return SpikeOutcome::kOnFloor;
			}
			REX::INFO("SFShopExplorer: [spike] dropped x{} {:08X} handle={:08X} at {:.1f},{:.1f},{:.1f}",
				a_count, a_bound->GetFormID(), handle, dropPos.x, dropPos.y, dropPos.z);

			// 3) Resolve the fresh world ref. The CELL WALK runs first: it is
			//    structural (cell+0x80 count / +0x88 array), it can tell which
			//    entries the drop appended, and every entry it accepts must
				//    name the dropped form itself (SpikeRefIsBound's 0x18 test).
			//    The handle route is a fallback only, and a candidate from it is
			//    treated as a guess until the same test passes: the 0.8.338 log
			//    showed LookupReferenceByHandle's return accepted on a position
			//    match alone, after which the attach ran against a string table
			//    (AV 0xC0000005) and the purchase stayed on the floor as Common.
			const std::uint32_t baseFid = a_bound->GetFormID();
			RE::TESObjectREFR* dropped = nullptr;
			const char* resolvedBy = "none";
			if (cell && SpikeReadable(cell, false)) {
				char walkHex[192]{};
				dropped = SpikeWalkCell(cell, &dropPos, preCount, a_bound, walkHex, sizeof(walkHex));
				REX::INFO("SFShopExplorer: [spike] cellwalk ptr={} {}", static_cast<const void*>(dropped), walkHex);
				if (dropped) {
					resolvedBy = "cellwalk";
				}
			}
			if (!dropped) {
				if (handle == 0) {
					REX::ERROR("SFShopExplorer: [spike] drop returned no handle for {:08X}", baseFid);
				} else {
					const auto probe = SpikeResolveHandle(handle);
					REX::INFO("SFShopExplorer: [spike] handle resolve handle={:08X} rawRet=0x{:X} viaRet={} viaOut={} seh={}",
						handle, probe.rawRet, static_cast<const void*>(probe.viaRet),
						static_cast<const void*>(probe.viaOut), probe.sehFault ? 1 : 0);
					RE::TESObjectREFR* const cands[2]{ probe.viaRet, probe.viaOut };
					for (auto* c : cands) {
						const bool bound = SpikeRefIsBound(c, a_bound);
						REX::INFO("SFShopExplorer: [spike] candidate ptr={} readable={} bound={}",
							static_cast<const void*>(c), c && SpikeReadable(c, false) ? 1 : 0, bound ? 1 : 0);
						if (bound) {
							dropped = c;
							resolvedBy = "handle";
							break;
						}
					}
				}
			}
			if (!dropped) {
				REX::ERROR("SFShopExplorer: [spike] no ref naming base {:08X} resolved (handle={:08X}) - item is on the floor",
					baseFid, handle);
				return SpikeOutcome::kOnFloor;
			}
			REX::INFO("SFShopExplorer: [spike] dropped ref ptr={} via={} preCount={}",
				static_cast<const void*>(dropped), resolvedBy, preCount);

			// 4) Full hex of the resolved ref (the cellwalk hit was never
			//    dumped in 88; the executor's gates - ref+0x98 non-null
			//    and formType 0x98 - show up in these qwords), then the
			//    direct AttachMod impl call for every default pick.
			if (SpikeDebug()) {
				char resolvedHex[780]{};
				char resolvedPtrs[768]{};
				const auto resolvedSig = SpikeValidateRef(dropped, baseFid, &dropPos, resolvedHex, sizeof(resolvedHex), resolvedPtrs, sizeof(resolvedPtrs));
				REX::INFO("SFShopExplorer: [spike] resolved sig=0x{:X} hex={}", resolvedSig, resolvedHex);
				REX::INFO("SFShopExplorer: [spike] resolved pointees {}", resolvedPtrs);
			}
			bool attachedAny = false;
			bool attachAv = false;
			bool mutated = false;
			std::uint32_t pickFids[16]{};
			std::uint32_t nPicks = 0;
			for (const auto& fp : a_picks) {
				if (nPicks < 16) {
					pickFids[nPicks++] = fp.fid;
				}
			}
			// Build 90: ground truth before the first attach (defaults)
			// and after the last one (did every pick land?).
			SpikeLogMods("pre", SpikeCollectMods(dropped, pickFids, nPicks), pickFids, nPicks);
			// Build 93: dump the reconcile set sources before any
			// attach mutates them (instance record + per-pick record
			// + [form+0xE0] membership verdict).
			SpikeLogRecord(SpikeRecordProbe(dropped, pickFids, nPicks, 0));
			std::uint32_t stepNo = 0;
			for (const auto& pick : a_picks) {
				auto* omod = RE::TESForm::LookupByID(pick.fid);
				if (!omod) {
					REX::ERROR("SFShopExplorer: [spike] omod {:08X} slot='{}' does not resolve - skipped", pick.fid, pick.slot);
					continue;
				}
				const auto rep = SpikeAttachStage(dropped, omod, 0, 1);
				const bool diff = rep.win[0][0] != rep.win[0][1] || rep.win[1][0] != rep.win[1][1] ||
					rep.win[2][0] != rep.win[2][1] || rep.win[3][0] != rep.win[3][1];
				mutated = mutated || diff;
				REX::INFO("SFShopExplorer: [spike] attach slot='{}' omod={:08X} ed='{}' called={} av=0x{:X} gate98={} formType=0x{:X} gateType={} diff={}",
					pick.slot, pick.fid, omod->GetFormEditorID() ? omod->GetFormEditorID() : "?",
					rep.called ? 1 : 0, rep.code, rep.gate98 ? 1 : 0,
					static_cast<std::uint32_t>(rep.formType), rep.gateType ? 1 : 0, diff ? 1 : 0);
				if (SpikeDebug()) {
					REX::INFO("SFShopExplorer: [spike] attach window b={:016X} {:016X} {:016X} {:016X} a={:016X} {:016X} {:016X} {:016X}",
						rep.win[0][0], rep.win[1][0], rep.win[2][0], rep.win[3][0],
						rep.win[0][1], rep.win[1][1], rep.win[2][1], rep.win[3][1]);
				}
				// Build 91: count trajectory after EVERY attach - if the
				// find fast path runs, cnt18 climbs 1,2,3...; a drop back
				// to 0 pinpoints which step resets the list.
				{
					const auto step = SpikeCollectMods(dropped, pickFids, nPicks);
					REX::INFO("SFShopExplorer: [spike] mods step#{} slot='{}' e33=0x{:X} cnt18={} cnt1C={} data1=0x{:X}",
						++stepNo, pick.slot, step.e33, step.cnt18, step.cnt1C, step.data1);
				}
				if (rep.code != 0) {
					attachAv = true;
					break;
				}
				attachedAny = attachedAny || rep.called;
			}

			// Build 90: same ground truth after the loop - which picks
			// actually exist on the ref's extra/instance now.
			SpikeLogMods("post", SpikeCollectMods(dropped, pickFids, nPicks), pickFids, nPicks);
			// Build 93: same dump after the loop - did any attach put
			// its pick into either record, or are they still empty?
			SpikeLogRecord(SpikeRecordProbe(dropped, pickFids, nPicks, 1));
			// Build 95: reconcile repair - re-store every pick (build
			// 96: replace=0 so same-slot legendary keys do not erase
			// each other), sync once over the fake pair carrying all
			// [form+0xE0] keys, then the worker tail (mark/flush +
			// direct builder) so the entries survive the set (mod
			// persistence) and extra 0x96 gets built. Skipped when
			// the attach loop itself faulted (record state uncertain
			// - the log says so).
			if (attachAv) {
				REX::INFO("SFShopExplorer: [spike] repair skipped - attach AV");
			} else {
				SpikeLogRepair(SpikeRepairMods(dropped, pickFids, nPicks));
				SpikeLogMods("repair", SpikeCollectMods(dropped, pickFids, nPicks), pickFids, nPicks);
			}

			// 5) Deliver the modified item into the player's inventory.
			//
			// 5a (0.8.342) adds the stack with the ref's OWN extra data list, so
			// the inventory item is the modified instance instead of a plain
			// copy. Every engine take entry is a dead end on 1.16.244: Activate
			// moves nothing, RemoveItem finds no list entry on a loose ref, and
			// Actor::PickUpObject faults on this build's vtable. The one call
			// that works is the plain add, and it accepts an ExtraDataList.
			//
			// Reconcile before adding the modified instance.
			//
			// Step 1 put a PLAIN stack in the inventory so the drop had something
			// to move; step 2 was supposed to move it out again and leave only the
			// world ref. If that removal did not happen the plain stack is still
			// here, and adding the modified instance on top leaves the player
			// holding BOTH: the item they bought and a Common copy of it - the
			// duplicate pair reported on 2026-10-26, which only became obvious
			// after the save was reloaded and the game re-serialised two stacks
			// that had different extra data. The count test below cannot catch it:
			// "at least what I expect" is already true once the plain stack is
			// present, so the surplus is taken out here, before anything is added.
			// In the normal case the drop did move the stack, this reads exactly
			// inventoryBefore and nothing happens.
			const auto afterDrop = SpikeInventoryObjectCount(a_player, baseFid);
			if (afterDrop > inventoryBefore) {
				const auto surplus = static_cast<std::int32_t>(afterDrop - inventoryBefore);
				RE::RemoveItemRequest reclaim{};
				reclaim.object = a_bound;
				reclaim.count = surplus;
				reclaim.reason = RE::ITEM_TRANSFER_REASON::kNone;
				std::uint32_t reclaimHandle = 0;
				const auto reclaimCode = SpikeRemoveStage(a_player, &reclaim, &reclaimHandle);
				REX::WARN("SFShopExplorer: [spike] drop left {} plain {:08X} in the inventory "
					"(count {}->{}); surplus removed so the purchase cannot duplicate (code=0x{:X} handle={:08X})",
					surplus, baseFid, inventoryBefore, afterDrop, reclaimCode, reclaimHandle);
			}
			// (The ref used below is retired only when its instance data pointer
			// differs from the inventory item's; if they are the same object,
			// deleting the ref would free it out from under the inventory entry.)
			const std::uint64_t expectedInventory = static_cast<std::uint64_t>(inventoryBefore) + a_count;
			const std::uint64_t refDl = SpikeRefExtraList(dropped);
			const std::uint32_t addCode = SpikeAddWithExtra(a_player, a_bound, a_count,
				reinterpret_cast<RE::ExtraDataList*>(static_cast<std::uintptr_t>(refDl)));
			std::uint64_t afterDelivery = SpikeInventoryObjectCount(a_player, baseFid);
			bool deliveryConfirmed = afterDelivery >= expectedInventory;
			const auto invStack = SpikeFindInventoryStack(a_player, baseFid);
			REX::INFO("SFShopExplorer: [spike] instance add code=0x{:X} refDl=0x{:X} playerCount={}->{} expected={} confirmed={} invDl=0x{:X} invInst=0x{:X}",
				addCode, refDl, inventoryBefore, afterDelivery, expectedInventory, deliveryConfirmed ? 1 : 0,
				invStack.dl, invStack.inst);
			// Whether the drop ref has already been disposed of. The instance add
			// usually confirms first, but the fallbacks below can confirm instead,
			// and the ref has to be dealt with in both cases - it used to be
			// handled only here, which is how a late-confirmed purchase left its
			// ref standing in the world.
			bool refDisposed = false;
			if (deliveryConfirmed) {
				REX::INFO("SFShopExplorer: [spike] inventory mods={} delivery confirmed by the instance add",
					SpikeExtraModCount(invStack.dl));
				SpikeDisposeDeliveryRef(a_player, dropped, a_bound, baseFid, "delivered");
				refDisposed = true;
			}

			SpikeTakeReport take{};
			if (!deliveryConfirmed && SpikeRefIsBound(dropped, a_bound)) {
				take = SpikeTakeToInventory(dropped, a_bound, a_count, a_player);
				afterDelivery = SpikeInventoryObjectCount(a_player, baseFid);
				deliveryConfirmed = afterDelivery >= expectedInventory;
				REX::INFO("SFShopExplorer: [spike] take request code=0x{:X} inst={} extra={} out={:08X} playerCount={}->{} expected={} confirmed={}",
					take.code, take.inst, take.extra, take.handle, inventoryBefore, afterDelivery, expectedInventory,
					deliveryConfirmed ? 1 : 0);
			}

			std::uint32_t activateCode = 0;
			std::uint32_t transferHandle = 0;
			std::uint32_t transferCode = 0;
			// Whether the activate handler was actually called. Needed below
			// because a clean activation may free the ref, and activateCode == 0
			// alone cannot tell that apart from "the block never ran".
			bool activationRan = false;
			if (!deliveryConfirmed && SpikeRefIsBound(dropped, a_bound)) {
				activationRan = true;
				activateCode = SpikeActivateStage(dropped, a_player);
				afterDelivery = SpikeInventoryObjectCount(a_player, baseFid);
				deliveryConfirmed = afterDelivery >= expectedInventory;
				REX::INFO("SFShopExplorer: [spike] activate pickup code=0x{:X} playerCount={}->{} expected={} confirmed={}",
					activateCode, inventoryBefore, afterDelivery, expectedInventory, deliveryConfirmed ? 1 : 0);

				// 5d) Legacy move request, kept only for the fault case: an
				// activation that raised leaves the ref in place, so one more
				// attempt costs nothing. transferCode 1 marks "activation ran,
				// outcome pending" because a clean activation is never followed
				// by a second call against a possibly-freed ref.
				if (activateCode == 0) {
					transferCode = deliveryConfirmed ? 0 : 1;
				} else {
					RE::RemoveItemRequest transfer{};
					transfer.object = a_bound;
					transfer.count = static_cast<std::int32_t>(a_count);
					transfer.reason = RE::ITEM_TRANSFER_REASON::kTakeFromActivator;
					transfer.moveTo = a_player;
					transferCode = SpikeRemoveStage(dropped, &transfer, &transferHandle);
					REX::INFO("SFShopExplorer: [spike] inventory move request code=0x{:X} out={:08X} ref={}",
						transferCode, transferHandle, static_cast<const void*>(dropped));
				}
			}
			// A delivery confirmed by the take request reaches here with its drop ref
			// still in the world: the disposal above only runs for the instance add.
			// This is the path that left a copy of the item on the floor for a
			// purchase that otherwise succeeded.
			//
			// Deliberately excluded: a CLEAN activation (activationRan &&
			// activateCode == 0). The engine's own take path may already have freed
			// the ref, which is why nothing below that call touches `dropped` - the
			// legacy move runs only when the activation faulted. Disposing of the
			// ref there would be a use-after-free. A faulted activation leaves the
			// ref in place, so that case is safe and is handled.
			if (deliveryConfirmed && !refDisposed && !(activationRan && activateCode == 0)) {
				SpikeDisposeDeliveryRef(a_player, dropped, a_bound, baseFid, "delivered-late");
				refDisposed = true;
			}
			if (auto* tasks = SFSE::GetTaskInterface()) {
				const auto formID = baseFid;
				const auto before = inventoryBefore;
				const auto count = a_count;
				const auto activation = activateCode;
				tasks->AddTask([a_player, formID, before, count, activation, transferCode, dropHandle = handle]() {
					const auto after = SpikeInventoryObjectCount(a_player, formID);
					const std::uint64_t expected = static_cast<std::uint64_t>(before) + count;
					// The ref the drop created, one task after the engine's own cell
					// update - see SpikeReRetireLate for why this is not done inline.
					SpikeReRetireLate(dropHandle, formID);
					// The count is the ground truth: either route landing is a
					// success, and the codes only say which one had to run.
					const bool confirmed = after >= expected;
					REX::INFO("SFShopExplorer: [spike] delivery verify activate=0x{:X} move=0x{:X} playerCount={}->{} expected={} confirmed={}",
						activation, transferCode, before, after, expected, confirmed ? 1 : 0);
					if (!confirmed) {
						REX::WARN("SFShopExplorer: [spike] delivery not confirmed for {:08X}; check floor and inventory before retrying",
							formID);
					}
				});
			}
			const auto inventoryAfter = afterDelivery;
			const bool inventoryNowConfirmed = inventoryAfter >= expectedInventory;
			REX::INFO("SFShopExplorer: [spike] delivery immediate instance=0x{:X} take=0x{:X} activate=0x{:X} move=0x{:X} playerCount={}->{} expected={} confirmed={}",
				addCode, take.code, activateCode, transferCode, inventoryBefore, inventoryAfter, expectedInventory,
				inventoryNowConfirmed ? 1 : 0);
			REX::INFO("SFShopExplorer: [spike] delivery x{} {:08X} destination={} ref={} attachedAny={} mutated={} attachAv={}",
				a_count, baseFid, inventoryNowConfirmed ? "inventory" : "pending/floor",
				static_cast<const void*>(dropped), attachedAny ? 1 : 0, mutated ? 1 : 0, attachAv ? 1 : 0);
			if (inventoryNowConfirmed) {
				return (attachedAny && !attachAv) ? SpikeOutcome::kDeliveredMods : SpikeOutcome::kDeliveredPlain;
			}
			if (activateCode == 0) {
				// The activation was accepted; the engine may finish the move on
				// its own task. Report pending rather than a floor claim.
				return (attachedAny && !attachAv) ? SpikeOutcome::kTransferPendingMods : SpikeOutcome::kTransferPendingPlain;
			}
			if (transferCode != 0) {
				return (attachedAny && !attachAv) ? SpikeOutcome::kOnFloorMods : SpikeOutcome::kOnFloor;
			}
			return (attachedAny && !attachAv) ? SpikeOutcome::kTransferPendingMods : SpikeOutcome::kTransferPendingPlain;
		}
	}


	// -----------------------------------------------------------------------
	// Session lifecycle: tick, toggle, open, close
	// -----------------------------------------------------------------------
	ShopBrowser* ShopBrowser::GetSingleton()
	{
		static ShopBrowser instance;
		return &instance;
	}

	void ShopBrowser::FrameUpdate()
	{
		// Pause hold: re-assert the counter floor every frame while the shop is
		// open, and pump diagnostics. Must run before the toggle check so the
		// ring buffer captures the frame the shop opens on.
		ShopPause::Tick();

#ifndef SFE_OSF_FRONTEND
		// The OSF build has no key of its own: OSF opens and closes the view and
		// reports that to the page, which drives OsfViewShown/OsfViewHidden.
		// Polling [overlay] toggleKey here as well is what made one Page Up
		// press both close the engine session and ask OSF to open the view.
		const int toggleKey = Overlay::ToggleKey();
		// Edge first, poll second. The window procedure records the key-down, so
		// a press can no longer be lost inside one long frame (which is what made
		// PgUp "sometimes need two presses"); GetAsyncKeyState stays as the
		// fallback for presses that arrive before that hook exists.
		const bool pressed = toggleKey != 0 && Overlay::ConsumeKeyPress(toggleKey);
		const bool held = toggleKey != 0 && (GetAsyncKeyState(toggleKey) & 0x8000) != 0;
		const bool toggle = pressed || held;
		if (toggle && !wasToggle.exchange(true, std::memory_order_acq_rel)) {
			Toggle();
		}
		if (!toggle) {
			wasToggle.store(false, std::memory_order_release);
		}
#endif
		ProcessPendingActions();
#ifndef SFE_OSF_FRONTEND
		// Open watchdog. The overlay draws on Present, so a healthy session
		// advances FramesDrawn within a frame or two of opening. A count that
		// stands still past the grace period means the player is staring at a
		// frozen world: close the session so the game resumes, and log why.
		// Normally this does nothing at all - the watch disarms on the first
		// drawn frame.
		constexpr std::uint64_t kDrawGraceMs = 3000;
		if (open.load(std::memory_order_acquire)) {
			const auto drawn = Overlay::FramesDrawn();
			if (!drawWatchArmed) {
				drawWatchArmed = true;
				drawWatchTick = GetTickCount64();
				drawWatchFrames = drawn;
			} else if (drawn != drawWatchFrames) {
				drawWatchArmed = false;
			} else if (GetTickCount64() - drawWatchTick >= kDrawGraceMs) {
				const auto waited = GetTickCount64() - drawWatchTick;
				drawWatchArmed = false;
				REX::ERROR("SFShopExplorer: shop closed without ever being drawn ({} ms, "
					"renderMode={}, canDraw={}, presentHook={}); the world is unpaused "
					"rather than left frozen",
					waited, Overlay::RenderMode(), Overlay::CanDraw(), Overlay::PresentHooked());
				Close();
			}
		} else {
			drawWatchArmed = false;
		}
#endif
		// Preview service: publish direct meshes and prefetch texture data on
		// this task thread; actual direct geometry rendering runs on Present.
		OfflineModel::ServicePending();
		// Build-116: background archive warm-up (one BA2 per tick) so the
		// first selection clicks never index archives on the Present thread.
		LiveModel::WarmTick();
	}

	void ShopBrowser::ProcessPendingActions()
	{
		std::scoped_lock lock(actionMutex, stateMutex);
		if (pendingBack) {
			pendingBack = false;
			Back();
		}
		if (pendingSelect) {
			pendingSelect = false;
			cursor = pendingSelection >= 0 ? static_cast<std::size_t>(pendingSelection) : cursor;
			if (pendingPlugin) {
				pendingPlugin = false;
				if (cursor < pluginFiles.size()) {
					selectedPlugin = static_cast<std::int32_t>(cursor);
					selectedFormIndex = 0;
					page = 0;
					BuildFormList();
				}
			} else if (pendingType) {
				pendingType = false;
				if (cursor < static_cast<std::size_t>(BrowseType::kTotal)) {
					selectedType = static_cast<BrowseType>(cursor);
					selectedArmorSub = ArmorSub::kAll;  // new type, new sub filter
					selectedWeaponSub = WeaponSub::kAll;
					selectedAmmoSub = AmmoSub::kAll;
					selectedMiscSub = MiscSub::kAll;
					selectedAidSub = AidSub::kAll;
					selectedBookSub = BookSub::kAll;
					selectedKeySub = KeySub::kAll;
					selectedFormIndex = 0;
					page = 0;
					BuildFormList();
				}
			} else {
				SelectCurrent();
			}
			pendingSelection = -1;
		}		if (pendingAccept) {
			pendingAccept = false;
			freeTransfer = pendingFree;
			ConfirmBasket();
		}
		if (pendingClear) {
			pendingClear = false;
			basket.clear();
		}
	}

	void ShopBrowser::Toggle()
	{
		if (open) {
			Close();
		} else {
			Open();
		}
	}

	void ShopBrowser::Open()
	{
#ifndef SFE_OSF_FRONTEND
		// Opening freezes the world and starts the score, and the overlay is the
		// only thing that can then show anything. When it has no draw path - the
		// DXGI hook never landed on this machine, or [overlay] render is below 4 -
		// the player gets a still world and music with no interface, and the key
		// that got them there is the only way out. That is the "PgUp starts the
		// music, the screen freezes, nothing shows" report from four users on the
		// Nexus thread. Refuse the open instead, and name the cause once.
		if (!Overlay::CanDraw()) {
			static bool loggedRefusal = false;
			if (!loggedRefusal) {
				loggedRefusal = true;
				REX::ERROR("SFShopExplorer: shop not opened - the overlay cannot draw "
					"(renderMode={}, presentHook={}); the world is left unpaused. Set "
					"[overlay] render=4 in SFShopExplorer.ini and remove any other DX12 "
					"overlay (ENB, ReShade, a proxy dxgi.dll). The OSF version of this mod "
					"does not need the overlay at all.",
					Overlay::RenderMode(), Overlay::PresentHooked());
			}
			return;
		}
#endif
		std::scoped_lock lock(stateMutex);
		BuildPluginList();
		LoadAudioSettings();
		LoadFavourites();
		// The favourites view is a browsing mode, not a preference: every open
		// starts from the catalog, so a shop opened weeks later does not appear
		// to have lost its stock.
		favouritesOnly = false;
		fullscreen = LoadShopIniBool(L"fullscreen", true);
		{
			std::scoped_lock choiceLock(g_assemblyMutex);
			g_weaponChoice.clear();
			g_choiceVersion.fetch_add(1, std::memory_order_acq_rel);
		}
		state = State::kForms;
		cursor = page = 0;
		selectedPlugin = pluginFiles.empty() ? -1 : 0;
		allPlugins = true;  // full catalog by default; pick a source below to narrow
		selectedType = BrowseType::kAllTypes;  // every record type at once
		selectedArmorSub = ArmorSub::kAll;
		selectedWeaponSub = WeaponSub::kAll;
		selectedAmmoSub = AmmoSub::kAll;
		selectedMiscSub = MiscSub::kAll;
		selectedAidSub = AidSub::kAll;
		selectedBookSub = BookSub::kAll;
		selectedKeySub = KeySub::kAll;
		selectedFormIndex = 0;
		addCount = 1;
		freeTransfer = true;
		basket.clear();
		walletCredits.store(PlayerCredits(RE::PlayerCharacter::GetSingleton()), std::memory_order_release);
		BuildFormList();
		open.store(true, std::memory_order_release);
		// Trip count: opening the shop is the thing Starfield measures.
		AddProgress(Unlock::kOpens, 1);
		// Freeze first, then play: the roar and the loop start together, and the
		// world is already still by the time the first frame is drawn.
		ShopPause::SetFrozen(true);
		ShopAudio::PlaySfx(ShopAudio::Sfx::kOpen);
		// The chosen track wins over the theme's own; "(theme track)" is the
		// empty selection. Volume is applied before the fade starts, so the first
		// buffer is already at the user's level instead of ramping from zero.
		ApplyAudioSettings();
		ShopAudio::SetMusicFile(MusicTrackFile().c_str());
		if (musicMuted) {
			ShopAudio::StopMusic();
		} else {
			ShopAudio::StartMusic();
		}
		Notify("opened");
	}

	void ShopBrowser::ReleaseSession()
	{
		// Gate on the pause state so a redundant teardown (ImGui X and Close
		// button in the same frame) cannot double-play the close sting.
		if (!ShopPause::IsFrozen()) {
			ShopAudio::StopMusic();
			return;
		}
		ShopAudio::PlaySfx(ShopAudio::Sfx::kClose);
		ShopAudio::StopMusic();
		ShopPause::SetFrozen(false);
		// The shop is a standalone ImGui overlay: it opens no game menu and owns
		// no console host, so closing it must not touch whatever menu the player
		// has open (the console-in-the-background idea was dropped on 2026-10-24).
	}

	void ShopBrowser::Close()
	{
		std::scoped_lock lock(stateMutex);
		drawWatchArmed = false;
		open.store(false, std::memory_order_release);
		SFE::ItemCapture::SetShopWantsCapture(false);
		// Music fades out and the world unfreezes on close. The pause release is
		// reference counted, so this cannot unpause a real menu the player opened
		// while shopping.
		ReleaseSession();
		Notify("closed");
	}

	// Shop-bar preferences (windowed/maximised, audio) persist the moment they
	// change rather than at exit, so a crash or a force-close does not lose them.
	// WritePrivateProfileString on a click is cheap enough not to matter.

	// -----------------------------------------------------------------------
	// Persistence: the INI, favourites, audio settings, notifications
	// -----------------------------------------------------------------------
	const std::filesystem::path& ShopBrowser::IniPath()
	{
		static const std::filesystem::path path = [] {
			HMODULE self = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&ShopBrowser::IniPath), &self);
			wchar_t modulePath[MAX_PATH]{};
			GetModuleFileNameW(self, modulePath, MAX_PATH);
			return std::filesystem::path(modulePath).parent_path() / kIniFileName;
		}();
		return path;
	}

	bool ShopBrowser::LoadShopIniBool(const wchar_t* a_key, bool a_default)
	{
		return GetPrivateProfileIntW(L"shop", a_key, a_default ? 1 : 0, IniPath().c_str()) != 0;
	}

	void ShopBrowser::SaveShopIniBool(const wchar_t* a_key, bool a_value)
	{
		const auto text = a_value ? L"1" : L"0";
		WritePrivateProfileStringW(L"shop", a_key, text, IniPath().c_str());
		// Narrow the key first: fmt has no formatter for const wchar_t*.
		REX::INFO("SFShopExplorer: [shop] {}={} saved", NarrowUtf8(a_key), a_value ? 1 : 0);
	}

	int ShopBrowser::LoadShopIniInt(const wchar_t* a_section, const wchar_t* a_key, int a_default)
	{
		return GetPrivateProfileIntW(a_section, a_key, a_default, IniPath().c_str());
	}

	std::string ShopBrowser::LoadShopIniString(const wchar_t* a_section, const wchar_t* a_key)
	{
		wchar_t buffer[MAX_PATH]{};
		const auto len = GetPrivateProfileStringW(a_section, a_key, L"", buffer, MAX_PATH, IniPath().c_str());
		return len ? NarrowUtf8(buffer) : std::string{};
	}

	void ShopBrowser::SaveShopIniInt(const wchar_t* a_section, const wchar_t* a_key, int a_value)
	{
		WritePrivateProfileStringW(a_section, a_key, std::to_wstring(a_value).c_str(), IniPath().c_str());
	}

	void ShopBrowser::SaveShopIniString(const wchar_t* a_section, const wchar_t* a_key, const std::string& a_value)
	{
		wchar_t wide[MAX_PATH]{};
		if (MultiByteToWideChar(CP_UTF8, 0, a_value.c_str(), -1, wide, MAX_PATH) <= 0) {
			return;
		}
		WritePrivateProfileStringW(a_section, a_key, wide, IniPath().c_str());
	}

	// Starred records live beside the INI, so a mod manager update that keeps
	// SFShopExplorer.ini keeps the favourites with it. Written on every star,
	// like the shop-bar prefs: a crash must not lose the list.
	const std::filesystem::path& ShopBrowser::FavouritesPath()
	{
		static const std::filesystem::path path = [] {
			auto file = IniPath();
			file.replace_filename(L"SFShopExplorerFavourites.txt");
			return file;
		}();
		return path;
	}

	void ShopBrowser::LoadFavourites()
	{
		favourites.clear();
		std::ifstream in(FavouritesPath());
		if (!in) {
			return;
		}
		std::string line;
		std::size_t skipped = 0;
		while (std::getline(in, line)) {
			// The header is commented and blank lines are tolerated, so the file
			// stays readable and a hand-edit cannot break the load.
			if (line.empty() || line[0] == '#') {
				continue;
			}
			std::uint32_t id = 0;
			try {
				id = static_cast<std::uint32_t>(std::stoul(line, nullptr, 16));
			} catch (...) {
				++skipped;
				continue;
			}
			if (id != 0) {
				favourites.push_back(static_cast<RE::TESFormID>(id));
			}
		}
		REX::INFO("SFShopExplorer favourites: {} starred record(s) loaded{}", favourites.size(),
			skipped ? std::format(" ({} unreadable line(s) ignored)", skipped) : std::string{});
	}

	void ShopBrowser::SaveFavourites()
	{
		std::ofstream out(FavouritesPath(), std::ios::trunc);
		if (!out) {
			REX::WARN("SFShopExplorer favourites: could not write SFShopExplorerFavourites.txt");
			return;
		}
		out << "# DevilzDad's Shop + Explorer - starred records, one 8-digit FormID per line.\n"
			   "# Written by the shop; edit only with the game closed.\n";
		for (const auto id : favourites) {
			out << std::format("{:08X}\n", id);
		}
	}

	bool ShopBrowser::IsFavourite(RE::TESFormID a_formID) const
	{
		return std::find(favourites.begin(), favourites.end(), a_formID) != favourites.end();
	}

	void ShopBrowser::ToggleFavourite(RE::TESFormID a_formID, const std::string& a_label)
	{
		const auto existing = std::find(favourites.begin(), favourites.end(), a_formID);
		if (existing != favourites.end()) {
			favourites.erase(existing);
			REX::INFO("SFShopExplorer favourites: unstarred {:08X} ('{}'), {} left", a_formID, a_label, favourites.size());
			Notify(std::format("unstarred {}", a_label));
		} else {
			favourites.push_back(a_formID);
			REX::INFO("SFShopExplorer favourites: starred {:08X} ('{}'), {} total", a_formID, a_label, favourites.size());
			Notify(std::format("starred {}", a_label));
		}
		SaveFavourites();
	}

	void ShopBrowser::LoadAudioSettings()
	{
		musicVolume = std::clamp(LoadShopIniInt(L"audio", L"musicVolume", 45), 0, 100);
		sfxVolume = std::clamp(LoadShopIniInt(L"audio", L"sfxVolume", 55), 0, 100);
		sfxMuted = LoadShopIniInt(L"audio", L"sfxMuted", 0) != 0;
		// 0.8.350 replaced the [shop] music on/off toggle with a per-channel
		// mute. An ini written by an older build has no musicMuted key, so its
		// toggle is honoured: an update must not start playing over someone who
		// had music off, which is exactly what this profile had set.
		const int mute = LoadShopIniInt(L"audio", L"musicMuted", -1);
		musicMuted = mute < 0 ? !LoadShopIniBool(L"music", true) : mute != 0;
		musicTrack = LoadShopIniString(L"audio", L"musicTrack");
		// A chosen track that has since been deleted must not leave the shop
		// silent with a name the picker cannot show; fall back to the theme's
		// own score and let the next open re-test the folder.
		if (!musicTrack.empty()) {
			const auto tracks = ShopAudio::ListMusicTracks();
			if (std::find(tracks.begin(), tracks.end(), musicTrack) == tracks.end()) {
				REX::WARN("SFShopExplorer: chosen track '{}' is gone; using the theme track", musicTrack);
				musicTrack.clear();
			}
		}
	}

	void ShopBrowser::SaveAudioSettings()
	{
		SaveShopIniInt(L"audio", L"musicVolume", musicVolume);
		SaveShopIniInt(L"audio", L"sfxVolume", sfxVolume);
		SaveShopIniInt(L"audio", L"musicMuted", musicMuted ? 1 : 0);
		SaveShopIniInt(L"audio", L"sfxMuted", sfxMuted ? 1 : 0);
		SaveShopIniString(L"audio", L"musicTrack", musicTrack);
		// Keep the pre-0.8.350 key in step so an older build reading this ini
		// still finds music off when the shop has it muted.
		SaveShopIniInt(L"shop", L"music", musicMuted ? 0 : 1);
		REX::INFO("SFShopExplorer: audio prefs saved (music {}% muted={}, sound {}% muted={}, track '{}')",
			musicVolume, musicMuted ? 1 : 0, sfxVolume, sfxMuted ? 1 : 0,
			musicTrack.empty() ? std::string("(theme)") : musicTrack);
	}

	void ShopBrowser::ApplyAudioSettings()
	{
		ShopAudio::SetMusicVolume(musicMuted ? 0.0f : static_cast<float>(musicVolume) / 100.0f);
		ShopAudio::SetSfxVolume(sfxMuted ? 0.0f : static_cast<float>(sfxVolume) / 100.0f);
	}

	std::string ShopBrowser::MusicTrackFile() const
	{
		return musicTrack.empty() ? std::string(ActiveTheme(theme).music) : musicTrack;
	}

	void ShopBrowser::Notify(const std::string& a_text) const
	{
		REX::INFO("SFShopExplorer: {}", a_text);
		CaptureNotify(a_text);
	}

	// The OSF page can only show what the engine tells it, and every refusal in
	// ConfirmBasket was written for a log the player never reads. Keep the last
	// few messages so one accept can report them all.
	void ShopBrowser::CaptureNotify(const std::string& a_text) const
	{
		std::scoped_lock lock(notifyMutex);
		notifyLines.push_back(a_text);
		if (notifyLines.size() > 32) {
			notifyLines.erase(notifyLines.begin(), notifyLines.begin() + 16);
		}
	}


	// -----------------------------------------------------------------------
	// Catalog construction: plugins, search keys, the visible list, sorting
	// -----------------------------------------------------------------------
	void ShopBrowser::BuildPluginList()
	{
		pluginNames.clear();
		pluginFiles.clear();
		const auto dataHandler = RE::TESDataHandler::GetSingleton();
		if (!dataHandler) {
			return;
		}
		// Full + small (ESL-flagged light, FE:xxx) + medium (FD:xxx):
		// files-only missed every light plugin (AA-43 Stalwart lived in
		// smallFiles while the pane showed 16 sources). Owner mapping
		// already resolves all three; the pane must list them too.
		const RE::BSTArray<RE::TESFile*>* parts[3] = { &dataHandler->compiledFileCollection.files,
			&dataHandler->compiledFileCollection.smallFiles, &dataHandler->compiledFileCollection.mediumFiles };
		for (const auto* part : parts) {
			for (auto* file : *part) {
				if (file && file->fileName[0]) {
					pluginNames.emplace_back(file->fileName);
					pluginFiles.push_back(file);
				}
			}
		}
		REX::INFO("SFShopExplorer: loaded {} plugins (full+small+medium)", pluginNames.size());
	}

	void ShopBrowser::EnsureSearchKeys()
	{
		// formsStamp, not listStamp: ApplyViewOrder bumps listStamp on every
		// header click, and a sort does not change a single key. Keyed on
		// listStamp this rebuilt all 9,161 keys after every sort while a
		// search term was in the box.
		if (searchKeysStamp == formsStamp && searchKeys.size() == forms.size()) {
		return;
		}		// Timed, because this build is paid inside a keystroke: the first search
		// after a list rebuild used to take 450 ms here (202 dummy-base weapons,
		// each running the OMOD assembly scan through FormModelPath).
		const auto started = std::chrono::steady_clock::now();
		searchKeys.clear();
		searchKeys.reserve(forms.size());
		for (const auto* form : forms) {
			searchKeys.push_back(SearchKeyFor(form));
		}
		searchKeysStamp = formsStamp;
		const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
		REX::INFO("SFShopExplorer: {} search key(s) built in {:.2f} ms", searchKeys.size(), ms);
	}

	void ShopBrowser::BuildFormList()
	{
		// The column the player last sorted by used to reset every session, which
		// made a deliberate VALUE or WT sort look like it never took. Read once,
		// before the first order is applied, and write it back on every header
		// click below.
		static bool s_sortRestored = false;
		if (!s_sortRestored) {
			s_sortRestored = true;
			const auto ini = ShopIniPath();
			// 0 means "nothing saved": the ini cannot hold -1 for catalog order.
			const int saved = static_cast<int>(GetPrivateProfileIntW(L"shop", L"sortColumn", 0, ini.c_str()));
			if (saved > 0) {
				sortColumn = std::clamp(saved - 1, 0, 4);
				sortAscending = GetPrivateProfileIntW(L"shop", L"sortAscending", 1, ini.c_str()) != 0;
				REX::INFO("SFShopExplorer: [shop] sortColumn={} sortAscending={} restored", sortColumn, sortAscending);
			}
		}
		++listStamp;   // a list change invalidates the visible rows
		++formsStamp;  // ...and the per-row keys, which a sort does not
		forms.clear();
		duplicateGroupOf.clear();
		// Favourites is a view of its own: the starred records across every
		// plugin and category, because a star is a want-list entry and not a
		// position on a shelf. The toolbar's category combos and the plugin list
		// are ignored while it is on, and the item pane says so. Ids that no
		// longer resolve (plugin dropped from the load order, record no longer a
		// catalog type) are counted and logged rather than drawn as blank rows.
		if (favouritesOnly) {
			auto* dataHandler = RE::TESDataHandler::GetSingleton();
			std::size_t lost = 0;
			for (const auto id : favourites) {
				auto* form = dataHandler ? RE::TESForm::LookupByID(id) : nullptr;
				if (!form || !IsCatalogRecordType(form->GetFormType())) {
					++lost;
					continue;
				}
				forms.push_back(form);
			}
			if (lost) {
				REX::WARN("SFShopExplorer favourites: {} starred record(s) no longer resolve in this load order", lost);
			}
			if (selectedFormIndex >= forms.size()) {
				selectedFormIndex = forms.empty() ? 0 : forms.size() - 1;
			}
			ApplyViewOrder();
			REX::INFO("SFShopExplorer: {} favourite(s) listed", forms.size());
			return;
		}
		auto* dataHandler = RE::TESDataHandler::GetSingleton();
		if (!dataHandler) {
			ApplyViewOrder();
			return;
		}
		std::string ownerFilter;
		if (!allPlugins) {
			if (selectedPlugin < 0 || selectedPlugin >= static_cast<std::int32_t>(pluginFiles.size())) {
				ApplyViewOrder();
				return;
			}
			const auto* selectedFile = pluginFiles[static_cast<std::size_t>(selectedPlugin)];
			if (!selectedFile) {
				ApplyViewOrder();
				return;
			}
			ownerFilter = selectedFile->fileName;
		}
		// Use the same validated runtime-array discovery as DevilzESMShop and
		// SFDevilzDump; the generated typed member is not reliable in this build.
		// All-Types merges every browsable record type (INGR contributes nothing:
		// zero INGR rows exist in Starfield's data).
		static constexpr BrowseType kAllBrowsable[] = { BrowseType::kWeapon, BrowseType::kArmor,
			BrowseType::kAmmo, BrowseType::kMisc, BrowseType::kAid, BrowseType::kBook, BrowseType::kKey };
		const auto gatherStarted = std::chrono::steady_clock::now();
		std::vector<RE::TESForm*> typed;
		if (selectedType == BrowseType::kAllTypes) {
			for (const auto t : kAllBrowsable) {
				std::vector<RE::TESForm*> part;
				BuildFormListForType(dataHandler, BrowseFormType(t), part);
				typed.insert(typed.end(), part.begin(), part.end());
			}
		} else {
			BuildFormListForType(dataHandler, BrowseFormType(selectedType), typed);
		}
		// The gather used to sit outside every timer, so "list 9,161 rows in
		// 33 ms" silently excluded the runtime-array walk that feeds it. Two
		// numbers now say which half a change moved.
		const auto gatherMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - gatherStarted).count();
		const auto listStarted = std::chrono::steady_clock::now();
		std::size_t hiddenTest = 0, hiddenTighten = 0, hiddenCrewEnemyCreature = 0, hiddenTurret = 0;
		std::size_t hiddenActorSkin = 0, hiddenReportBug = 0;
		// Rows that pass every filter, in catalog order, held until the duplicate
		// grouping below decides which of them the list keeps, plus the identity
		// each one groups under. The favourites view returns before this and is
		// left alone on purpose: a starred record is a request, so a star that
		// shares a name with a base-game item must not vanish under it.
		std::vector<RE::TESForm*> accepted;
		std::vector<std::string> groupKeys;
		accepted.reserve(typed.size());
		groupKeys.reserve(typed.size());
		// One owner table instead of one call per record: RuntimeOwnerOfFormID
		// returns a std::string and walks the compiled-file array, so a 9,000-row
		// plugin view paid 9,000 allocations for at most 75 distinct answers. The
		// mapping is the function's own: compile index for ESM/ESP, slot for ESL.
		std::vector<const char*> ownerByIndex(256, nullptr);
		std::vector<const char*> ownerSmall;
		std::vector<const char*> ownerMedium;
		if (!allPlugins && dataHandler) {
			for (const auto* file : dataHandler->compiledFileCollection.files) {
				if (file && file->fileName[0] && file->compileIndex < ownerByIndex.size()) {
					ownerByIndex[file->compileIndex] = file->fileName;
				}
			}
			for (const auto* file : dataHandler->compiledFileCollection.smallFiles) {
				ownerSmall.push_back(file && file->fileName[0] ? file->fileName : nullptr);
			}
			for (const auto* file : dataHandler->compiledFileCollection.mediumFiles) {
				ownerMedium.push_back(file && file->fileName[0] ? file->fileName : nullptr);
			}
		}
		// SFSE GetModByFormId semantics: 0xFE is a light (ESL-flagged) file and
		// 0xFD a medium file, each indexed by the middle 12 bits into its own
		// table. These two used to be crossed, so every light plugin was looked
		// up in the 13-entry medium table - out of range, no owner, and a plugin
		// holding hundreds of armors listed as "0 records" while a medium plugin
		// showed whichever light file happened to sit at its index.
		// AmaraArmor.esm is the reported case: its armors are FE06Exxx, slot
		// 0x06E, while the medium table stops long before 0x06E.
		// Cross-checked against the MCM dump, which resolves owners correctly for
		// every one of 79k catalog rows (SF Starfield MCM/src/Dump.cpp).
		auto ownerOfFormID = [&](std::uint32_t a_id) -> const char* {
			const auto top = static_cast<std::uint8_t>(a_id >> 24);
			const auto middle = static_cast<std::size_t>((a_id >> 12) & 0x0FFF);
			if (top == 0xFE) {
				return middle < ownerSmall.size() ? ownerSmall[middle] : nullptr;
			}
			if (top == 0xFD) {
				return middle < ownerMedium.size() ? ownerMedium[middle] : nullptr;
			}
			return ownerByIndex[top];
		};
		for (auto* form : typed) {
			if (!form) {
				continue;
			}
			if (!allPlugins) {
				const char* owner = ownerOfFormID(form->GetFormID());
				if (!owner || ownerFilter != owner) {
					continue;
				}
			}
			const auto* edid = form->GetFormEditorID();
			const std::string ed = edid ? edid : "";
			// One display name and one folded identity per record. Three of the
			// hide toggles each rebuilt "edid + name + 512 chars of keywords" from
			// scratch, and the report-bug/test-debug checks called FormDisplayName
			// again, so a rebuild paid for the same lookups several times per row.
			const bool named = HasPlayerVisibleName(form);
			const std::string name = named ? FormDisplayName(form) : std::string{};
			std::string foldedIdentity;
			bool identityBuilt = false;
			auto identityOf = [&]() -> const std::string& {
				if (!identityBuilt) {
					identityBuilt = true;
					foldedIdentity = CaseFold(ed + " " + name + " " + FormKeywords(form, 512));
				}
				return foldedIdentity;
			};
			// Classifier text is per record too, and shared by every sub-filter
			// that reads it (All Types checks up to seven in one pass).
			std::string categoryText;
			std::string categoryModel;
			bool categoryTextBuilt = false, categoryModelBuilt = false;
			auto catText = [&]() -> const std::string& {
				if (!categoryTextBuilt) {
					categoryTextBuilt = true;
					categoryText = CategorySearchText(form);
				}
				return categoryText;
			};
			auto catModel = [&]() -> const std::string& {
				if (!categoryModelBuilt) {
					categoryModelBuilt = true;
					categoryModel = CategoryModelText(form);
				}
				return categoryModel;
			};
			// Do not filter records with no player-facing name. These are the
			// unnamed forms users need to inspect; the hide toggles apply only
			// when the game actually supplies a display name.
			if (hideReportBug && named && IsReportBugName(name)) {
				++hiddenReportBug;
				continue;
			}
			if (hideTestDebug && named &&
				IsTestDebugIdentity(ed, name)) {
				++hiddenTest;
				continue;
			}
			// Actor skins (creature, robot and turret bodies) are not items and
			// have no inventory keywords to filter on, so no toggle guards them.
			if (IsActorSkinArmor(form)) {
				++hiddenActorSkin;
				continue;
			}
			if (hideTurrets && IsUnbuyableAmmo(form)) {
				++hiddenTurret;
				continue;
			}
			if (hideTurrets && HasPlayerVisibleName(form)) {
				// Its own toggle, so turrets can be shown without un-hiding
				// ships, hair and bare bodies at the same time.
				const auto& identity = identityOf();
				if (IsTurretIdentity(identity)) {
					++hiddenTurret;
					continue;
				}
			}
			if (hideCrewEnemyCreature && HasPlayerVisibleName(form)) {
				const auto& identity = identityOf();
				if (IsCrewEnemyCreatureIdentity(identity)) {
					++hiddenCrewEnemyCreature;
					continue;
				}
			}
			if (hideShipHair && HasPlayerVisibleName(form)) {
				// Check the complete searchable identity, not only keywords:
				// plugin records often put "Hair" or "Ship" in the visible
				// name/EDID and omit the matching keyword entirely.
				const auto& identity = identityOf();
				// naked/nude must be EDID tokens: nearly every garment
				// carries ArmorTypeApparelOrNakedBody, so substring would
				// hide 494/582 apparel rows (all clothes!). Skin/nude forms
				// carry it in the EDID (Skin_Naked) and still hide.
				if (HasAnyText(identity, "spaceship", "ship", "hair", "beard")) {
					++hiddenTighten;
					continue;
				}
				const std::string edFold = CaseFold(ed);
				bool bare = false;
				{
					std::size_t i = 0;
					while (i <= edFold.size() && !bare) {
						while (i < edFold.size() && !(edFold[i] >= 'a' && edFold[i] <= 'z') &&
							!(edFold[i] >= '0' && edFold[i] <= '9')) {
							++i;
						}
						std::size_t j = i;
						while (j < edFold.size() && ((edFold[j] >= 'a' && edFold[j] <= 'z') ||
							(edFold[j] >= '0' && edFold[j] <= '9'))) {
							++j;
						}
						if (j > i) {
							const std::string tok = edFold.substr(i, j - i);
							bare = (tok == "naked" || tok == "nude");
						}
						i = j + 1;
					}
				}
				if (bare) {
					++hiddenTighten;
					continue;
				}
			}
			// Armor subcategory (toolbar combo, Armor type only). Wide
			// keyword cap: subtype keywords sit behind long ma_ lists.
			if (selectedType == BrowseType::kArmor && selectedArmorSub != ArmorSub::kAll) {
				if (ClassifyArmorSub(catText()) != selectedArmorSub) {
					continue;
				}
			}
			// Keyword-first categories use live display names and EDIDs as a fallback;
			// unknown records stay available under each category's Other filter.
			if (selectedType == BrowseType::kWeapon && selectedWeaponSub != WeaponSub::kAll) {
				if (ClassifyWeaponSub(catText()) != selectedWeaponSub) continue;
			}
			if (selectedType == BrowseType::kAllTypes) {
				const auto type = form->GetFormType();
				if (type == RE::FormType::kWEAP && selectedWeaponSub != WeaponSub::kAll &&
					ClassifyWeaponSub(catText()) != selectedWeaponSub) continue;
				if (type == RE::FormType::kARMO && selectedArmorSub != ArmorSub::kAll &&
					ClassifyArmorSub(catText()) != selectedArmorSub) continue;
				if (type == RE::FormType::kAMMO && selectedAmmoSub != AmmoSub::kAll &&
					ClassifyAmmoSub(catText()) != selectedAmmoSub) continue;
				if (type == RE::FormType::kMISC && selectedMiscSub != MiscSub::kAll &&
					ClassifyMiscSub(catText(), catModel()) != selectedMiscSub) continue;
				if (type == RE::FormType::kALCH && selectedAidSub != AidSub::kAll &&
					ClassifyAidSub(catText(), catModel()) != selectedAidSub) continue;
				if (type == RE::FormType::kBOOK && selectedBookSub != BookSub::kAll &&
					ClassifyBookSub(catText(), FormModelPath(form)) != selectedBookSub) continue;
				if (type == RE::FormType::kKEYM && selectedKeySub != KeySub::kAll &&
					ClassifyKeySub(catText()) != selectedKeySub) continue;
			}
			if (selectedType == BrowseType::kAmmo && selectedAmmoSub != AmmoSub::kAll) {
				if (ClassifyAmmoSub(catText()) != selectedAmmoSub) continue;
			}
			if (selectedType == BrowseType::kMisc && selectedMiscSub != MiscSub::kAll) {
				if (ClassifyMiscSub(catText(), catModel()) != selectedMiscSub) continue;
			}
			if (selectedType == BrowseType::kAid && selectedAidSub != AidSub::kAll) {
				if (ClassifyAidSub(catText(), catModel()) != selectedAidSub) continue;
			}
			if (selectedType == BrowseType::kBook && selectedBookSub != BookSub::kAll) {
				if (ClassifyBookSub(catText(), FormModelPath(form)) != selectedBookSub) continue;
			}
			if (selectedType == BrowseType::kKey && selectedKeySub != KeySub::kAll) {
				if (ClassifyKeySub(catText()) != selectedKeySub) continue;
			}
			// The duplicate key is what the player can see - type, name and stats -
			// never the record's identity. Two records are only candidates when the
			// player could mistake one for the other *as merchandise*, which is why
			// the key needs a price and a name:
			//  - unnamed forms all share one placeholder label, so grouping them
			//    would fold hundreds of unrelated records into a single row;
			//  - a free record is a plot object, and identically-labelled plot
			//    objects are different things: 62 "Crew Member's Slate"s, 13 "Dying
			//    Words", seven "Lab Key"s. Collapsing those would hide the one slate
			//    or key the player came for, which is a worse bug than the duplicate
			//    row this feature removes.
			std::string groupKey;
			const auto recordValue = FormValue(form);
			if (named && recordValue > 0) {
				const auto typeValue = static_cast<std::uint32_t>(std::to_underlying(form->GetFormType()));
				// AMMO ignores weight: Starfield keeps a pre-ammo-weight record of a
				// round beside the one that replaced it, and a player reading "11mm
				// Caseless, 32 credits" twice is looking at the same box of rounds.
				// Every other type keeps weight in the key, so a heavier variant of
				// the same name stays its own row.
				const long milliWeight = typeValue == static_cast<std::uint32_t>(std::to_underlying(RE::FormType::kAMMO))
					? 0L
					: static_cast<long>(std::lround(FormWeight(form) * 1000.0f));
				groupKey = std::format("{}|{}|{}|{}", typeValue, CaseFold(name), recordValue, milliWeight);
			}
			accepted.push_back(form);
			groupKeys.push_back(std::move(groupKey));
		}
		// Records the game defines more than once. Two rows with the same name are
		// not a listing fault: their FormIDs differ and both can be spawned. A mod
		// that clones an item for display keeps its copy beside the master's
		// (tailoredspaces' SPP_TS_RazorbackTrophy next to Starfield.esm's
		// Razorback), and Bethesda leaves replaced records in the data. Grouping
		// records the group on every member, so a row says how many records it
		// stands for and the item pane can list them, and collapses the group to
		// one row while hideDuplicateNames is on.
		{
			std::unordered_map<std::string, std::size_t> groupIndex;
			std::vector<std::vector<RE::TESFormID>> groups(accepted.size());
			for (std::size_t index = 0; index < accepted.size(); ++index) {
				if (groupKeys[index].empty()) {
					continue;
				}
				const auto [it, added] = groupIndex.emplace(groupKeys[index], index);
				groups[added ? index : it->second].push_back(accepted[index]->GetFormID());
			}
			// Which member stands for its group: the earliest plugin's newest
			// record. Earliest, because the master's own definition is the item
			// and a later plugin's copy is a clone of it; newest, because within one
			// file an appended record is the replacement for the one before it.
			// Load-order rank of the plugin that owns an id, and nothing else: the
			// middle 12 bits are a file's own index only for the indexed forms
			// (0xFE light, 0xFD medium); in a full plugin they are part of the record
			// id, so mixing them in would have ordered two records by their local
			// id and quietly defeated the "newest record" half of the rule.
			const auto pluginRank = [](const RE::TESFormID a_id) {
				const auto top = static_cast<std::uint32_t>(a_id >> 24);
				const auto middle = static_cast<std::uint32_t>((a_id >> 12) & 0x0FFFu);
				const bool indexed = top == 0xFE || top == 0xFD;
				return indexed ? ((top << 12) | middle) : (top << 12);
			};
			std::unordered_map<std::uint32_t, RE::TESFormID> survivorOf;
			std::size_t collapsed = 0;
			for (std::size_t index = 0; index < accepted.size(); ++index) {
				if (groups[index].size() < 2) {
					continue;
				}
				RE::TESFormID survivor = groups[index].front();
				for (const auto id : groups[index]) {
					const auto rank = pluginRank(id);
					const auto best = pluginRank(survivor);
					if (rank < best || (rank == best && id > survivor)) {
						survivor = id;
					}
				}
				for (const auto id : groups[index]) {
					survivorOf[id] = survivor;
					duplicateGroupOf[id] = groups[index];
				}
			}
			for (std::size_t index = 0; index < accepted.size(); ++index) {
				const auto id = accepted[index]->GetFormID();
				if (hideDuplicateNames) {
					const auto it = survivorOf.find(id);
					if (it != survivorOf.end() && it->second != id) {
						++collapsed;
						continue;
					}
				}
				forms.push_back(accepted[index]);
			}
			if (!duplicateGroupOf.empty()) {
				REX::INFO("SFShopExplorer: {} record(s) share a name and stats with another ({}), {} row(s) collapsed",
					duplicateGroupOf.size(), hideDuplicateNames ? "duplicates hidden" : "duplicates shown", collapsed);
			}
		}
		if (selectedFormIndex >= forms.size()) {
			selectedFormIndex = forms.empty() ? 0 : forms.size() - 1;
		}
		const auto filterMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - listStarted).count();
		REX::INFO("SFShopExplorer: list phases: gather {:.2f} ms, filter {:.2f} ms", gatherMs, filterMs);
		ApplyViewOrder();
		REX::INFO("SFShopExplorer: list {} row(s) from {} candidate(s) in {:.2f} ms",
			forms.size(), typed.size(),
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - listStarted).count());
		if (selectedType == BrowseType::kArmor && selectedArmorSub != ArmorSub::kAll) {
			REX::INFO("SFShopExplorer: {} Armor/{} forms for {} (hidden test={} tighten={} turret={} actor-skin={})", forms.size(),
				ArmorSubName(selectedArmorSub), allPlugins ? "all plugins" : ownerFilter, hiddenTest, hiddenTighten, hiddenTurret, hiddenActorSkin);
		} else if (selectedType == BrowseType::kWeapon && selectedWeaponSub != WeaponSub::kAll) {
			REX::INFO("SFShopExplorer: {} Weapons/{} forms for {} (hidden test={} tighten={} turret={})", forms.size(),
				WeaponSubName(selectedWeaponSub), allPlugins ? "all plugins" : ownerFilter, hiddenTest, hiddenTighten, hiddenTurret);
		} else if (selectedType == BrowseType::kAmmo && selectedAmmoSub != AmmoSub::kAll) {
			REX::INFO("SFShopExplorer: {} Ammo/{} forms (hidden turret={})", forms.size(), AmmoSubName(selectedAmmoSub), hiddenTurret);
		} else if (selectedType == BrowseType::kMisc && selectedMiscSub != MiscSub::kAll) {
			REX::INFO("SFShopExplorer: {} Misc/{} forms", forms.size(), MiscSubName(selectedMiscSub));
		} else if (selectedType == BrowseType::kAid && selectedAidSub != AidSub::kAll) {
			REX::INFO("SFShopExplorer: {} Aid/{} forms", forms.size(), AidSubName(selectedAidSub));
		} else if (selectedType == BrowseType::kBook && selectedBookSub != BookSub::kAll) {
			REX::INFO("SFShopExplorer: {} Books/{} forms", forms.size(), BookSubName(selectedBookSub));
		} else if (selectedType == BrowseType::kKey && selectedKeySub != KeySub::kAll) {
			REX::INFO("SFShopExplorer: {} Keys/{} forms", forms.size(), KeySubName(selectedKeySub));
		} else {
			REX::INFO("SFShopExplorer: {} {} forms for {} (hidden test={} tighten={} turret={} actor-skin={} report-bug={})", forms.size(),
				BrowseTypeName(selectedType), allPlugins ? "all plugins" : ownerFilter, hiddenTest, hiddenTighten, hiddenTurret, hiddenActorSkin, hiddenReportBug);
		}
	}

	void ShopBrowser::ApplyViewOrder()
	{
		++listStamp;
		viewOrder.resize(forms.size());
		for (std::size_t i = 0; i < forms.size(); ++i) {
			viewOrder[i] = i;
		}
		if (sortColumn < 0) {
			return;
		}
		// The keys used to be built inside the comparator: FormDisplayName plus
		// CaseFold for NAME, a std::string type name for TYPE. With 9,155 rows
		// that is ~120k comparisons and two heap strings each, on every click of
		// a sort header and on every list rebuild. Compute one key per row here
		// and let the comparator read it.
		const int col = sortColumn;
		const bool asc = sortAscending;
		const bool textColumn = col == 0 || col == 1;
		const std::vector<std::string>* textKey = nullptr;
		std::vector<std::string> typeKey;
		std::vector<double> numKey;
		if (col == 0) {
			// One name key per row, refreshed only when the rows change. Folding
			// every name again on each click is what made NAME cost 4.4-5.0 ms in
			// the 21:41 log while the numeric columns cost 0.5-1.1 ms: the keys
			// are cached against formsStamp, which ApplyViewOrder does not bump.
			if (nameSortKeysStamp != formsStamp || nameSortKeys.size() != forms.size()) {
				const auto keyStarted = std::chrono::steady_clock::now();
				nameSortKeys.resize(forms.size());
				for (std::size_t i = 0; i < forms.size(); ++i) {
					// NameSortKey, not CaseFold: the catalog carries names padded with
					// a space or an undrawable icon (see IsNamePadding), and punctuation
					// prefixes like '"The Great Serpent" Lie'. Sorted on raw bytes those
					// rows floated to the top of the list and looked unsorted.
					nameSortKeys[i] = forms[i] ? NameSortKey(FormDisplayName(forms[i])) : std::string{};
				}
				nameSortKeysStamp = formsStamp;
				REX::INFO("SFShopExplorer: {} name sort key(s) built in {:.2f} ms", nameSortKeys.size(),
					std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - keyStarted).count());
			}
			textKey = &nameSortKeys;
		} else if (col == 1) {
			// TYPE keys stay per sort: RowTypeName is a switch, not a lookup, and
			// the column already sorts in ~1 ms.
			typeKey.resize(forms.size());
			for (std::size_t i = 0; i < forms.size(); ++i) {
				typeKey[i] = forms[i] ? RowTypeName(forms[i]) : std::string{};
			}
			textKey = &typeKey;
		} else {
			// double keeps a 32-bit FormID exact; a float would round above 2^24.
			numKey.resize(forms.size());
			for (std::size_t i = 0; i < forms.size(); ++i) {
				const auto* form = forms[i];
				if (!form) {
					continue;
				}
				if (col == 2) {
					numKey[i] = static_cast<double>(FormValue(form));
				} else if (col == 3) {
					numKey[i] = static_cast<double>(FormWeight(form));
				} else {
					numKey[i] = static_cast<double>(form->GetFormID());
				}
			}
		}
		// The clock starts once the keys exist, so this line times the sort. A
		// key build has its own line above.
		const auto started = std::chrono::steady_clock::now();
		std::stable_sort(viewOrder.begin(), viewOrder.end(), [&](std::size_t a, std::size_t b) {
			// A comparison has to be a total order or stable_sort is free to return
			// a sequence that is not sorted at all; it used to fall back to `a < b`
			// for a missing form, which is unrelated to the key order every other
			// pair uses. A row with no form now takes an empty key and FormID 0.
			auto* fa = a < forms.size() ? forms[a] : nullptr;
			auto* fb = b < forms.size() ? forms[b] : nullptr;
			int cmp = 0;
			if (textColumn) {
				cmp = (*textKey)[a].compare((*textKey)[b]);
			} else {
				const double ka = numKey[a], kb = numKey[b];
				cmp = ka < kb ? -1 : ka > kb ? 1 : 0;
			}
			if (cmp == 0) {
				const auto ia = fa ? fa->GetFormID() : 0u;
				const auto ib = fb ? fb->GetFormID() : 0u;
				cmp = ia < ib ? -1 : ia > ib ? 1 : 0;
			}
			return asc ? cmp < 0 : cmp > 0;
		});
		const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
		REX::INFO("SFShopExplorer: view order {} row(s) by column {} {} in {:.2f} ms", forms.size(), col, asc ? "asc" : "desc", ms);
	}


	// -----------------------------------------------------------------------
	// Selection, the basket, and back/step navigation
	// -----------------------------------------------------------------------
	void ShopBrowser::SelectCurrent()
	{
		if (state == State::kPlugins && cursor < pluginFiles.size()) {
			selectedPlugin = static_cast<std::int32_t>(cursor);
			state = State::kTypes;
		} else if (state == State::kTypes && cursor < static_cast<std::size_t>(BrowseType::kTotal)) {
			selectedType = static_cast<BrowseType>(cursor);
			selectedArmorSub = ArmorSub::kAll;
			selectedWeaponSub = WeaponSub::kAll;
			selectedAmmoSub = AmmoSub::kAll;
			selectedMiscSub = MiscSub::kAll;
			selectedAidSub = AidSub::kAll;
			selectedBookSub = BookSub::kAll;
			selectedKeySub = KeySub::kAll;
			BuildFormList();
			state = State::kForms;
		} else if (state == State::kForms && cursor < forms.size() && forms[cursor]) {
			selectedFormIndex = cursor;
			addCount = 1;
			state = State::kForms;
		} else if (state == State::kCount) {
			ConfirmAdd();
		}
		cursor = page = 0;
	}

	void ShopBrowser::ConfirmAdd()
	{
		if (selectedFormIndex >= forms.size()) {
			return;
		}
		auto* const form = forms[selectedFormIndex];
		if (!form) {
			return;
		}
		// Append to the basket instead of transferring immediately: ACCEPT
		// moves every entry at once (ConfirmBasket). Capture the resolved
		// OMOD selection now so changing the preview later cannot mutate an
		// earlier basket line. One chosen weapon configuration per line.
		const auto count = std::clamp(addCount, 1, 999);
		const auto formID = form->GetFormID();
		if (form->GetFormType() == RE::FormType::kWEAP) {
			CollectWeaponAssemblyNifs(form, [](const char*) {});
		}
		auto selectedOmods = SelectedOmodsFor(form);
		for (auto& entry : basket) {
			if (entry.formID == formID && entry.selectedOmods == selectedOmods) {
				entry.count = std::clamp(entry.count + count, 1, 999);
				return;
			}
		}
		const auto* edid = form->GetFormEditorID();
		basket.push_back(BasketEntry{ formID, edid ? edid : "", count, std::move(selectedOmods) });
	}

	void ShopBrowser::ConfirmBasket()
	{
		if (basket.empty()) {
			return;
		}
		auto* const player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			Notify("transfer failed: player unavailable");
			return;
		}

		// Resolve and price every entry first so a stale FormID or a credit
		// shortfall aborts the whole transfer before anything moves.
		struct Resolved
		{
			RE::TESBoundObject* bound;
			std::uint32_t count;
			std::uint32_t unitPrice;
			std::uint32_t formID;
			std::vector<std::uint32_t> selectedOmods;
		};
		std::vector<Resolved> resolved;
		resolved.reserve(basket.size());
		std::uint32_t totalCost = 0;
		for (const auto& entry : basket) {
			auto* form = RE::TESForm::LookupByID(entry.formID);
			if (!form || !form->IsBoundObject()) {
				Notify(std::format("transfer refused: {:08X} is unavailable", entry.formID));
				return;
			}
			const auto count = static_cast<std::uint32_t>(std::clamp(entry.count, 1, 999));
			const auto unitPrice = FormValue(form);
			totalCost += unitPrice * count;
			resolved.push_back({ static_cast<RE::TESBoundObject*>(form), count, unitPrice, entry.formID, entry.selectedOmods });
		}

		if (!freeTransfer && totalCost > 0) {
			const auto credits = PlayerCredits(player);
			walletCredits.store(credits, std::memory_order_release);
			if (credits < totalCost) {
				Notify(std::format("transfer refused: need {} credits, have {}", totalCost, credits));
				return;
			}
			auto* const creditsForm = RE::TESForm::LookupByID(0x0000000F);
			if (!creditsForm) {
				Notify("transfer failed: credits form unavailable");
				return;
			}
			RE::RemoveItemRequest request{};
			request.object = static_cast<RE::TESBoundObject*>(const_cast<RE::TESForm*>(creditsForm));
			request.count = static_cast<std::int32_t>(totalCost);
			request.reason = RE::ITEM_TRANSFER_REASON::kNone;
			std::uint32_t handle = 0;
			player->RemoveItem(handle, request);
		}

		// Modded delivery stays on (0.8.340). Selected parts and rarity only
		// exist as OMODs, and OMODs attach to a world reference, so the route
		// has to pass through one: add, drop, attach, then take. What changed
		// is the take: a complete RemoveItemRequest names the stack by its
		// instance data and extra list and moves it to the player, which is the
		// transfer the take prompt performs. If that does not move the stack,
		// the ref's activate handler runs, and only a faulting activation falls
		// back to the object+count request. [shop] attachMods=0 keeps the plain
		// inventory add (no OMODs applied) for troubleshooting.
		const bool attachMods = LoadShopIniBool(L"attachMods", true);
		for (const auto& item : resolved) {
			const bool hasSelectedMods = !item.selectedOmods.empty();
			const bool useWorldRoute = hasSelectedMods && attachMods;
			const auto inventoryBefore = SpikeInventoryObjectCount(player, item.formID);
			SpikeOutcome outcome = SpikeOutcome::kNotAttempted;
			if (useWorldRoute) {
				std::vector<AssemblyPick> picks;
				picks.reserve(item.selectedOmods.size());
				for (const auto fid : item.selectedOmods) {
					picks.push_back(AssemblyPick{ "selected", fid });
				}
				outcome = SpikeDeliverWithMods(player, item.bound, item.count, picks);
			} else if (hasSelectedMods) {
				outcome = SpikeOutcome::kInventoryOnly;
				REX::INFO("SFShopExplorer: mods skipped for {:08X} x{} (attachMods=0; direct inventory)",
					item.formID, item.count);
			}
			if (outcome == SpikeOutcome::kNotAttempted || outcome == SpikeOutcome::kInventoryOnly) {
				player->AddObjectToContainer(item.bound, RE::BSTSmartPointer<RE::ExtraDataList>(new RE::ExtraDataList()),
					static_cast<std::int32_t>(item.count), nullptr, RE::ITEM_TRANSFER_REASON::kNone);
				const auto inventoryAfter = SpikeInventoryObjectCount(player, item.formID);
				const bool confirmed = static_cast<std::uint64_t>(inventoryAfter) >=
					static_cast<std::uint64_t>(inventoryBefore) + item.count;
				const char* route = outcome == SpikeOutcome::kInventoryOnly ? "mods-skipped" : "plain";
				REX::INFO("SFShopExplorer: inventory add x{} {:08X} route={} mods={} count={}->{} confirmed={}",
					item.count, item.formID, route, hasSelectedMods ? (attachMods ? "world" : "skipped") : "none",
					inventoryBefore, inventoryAfter, confirmed ? 1 : 0);
				if (!confirmed) {
					outcome = SpikeOutcome::kInventoryAddFailed;
				}
			}
			std::string modsNote;
			switch (outcome) {
			case SpikeOutcome::kInventoryOnly:
				modsNote = hasSelectedMods ?
					" (inventory delivered; selected mods not applied)" : " (inventory only)";
				break;
			case SpikeOutcome::kInventoryAddFailed:
				modsNote = " - inventory delivery not confirmed (check before retrying)";
				break;
			case SpikeOutcome::kDeliveredMods:
				modsNote = " +mods";
				break;
			case SpikeOutcome::kDeliveredPlain:
				modsNote = " (mods failed)";
				break;
			case SpikeOutcome::kTransferPendingMods:
			case SpikeOutcome::kTransferPendingPlain:
				modsNote = " (pickup accepted; verification pending)";
				break;
			case SpikeOutcome::kOnFloor:
				modsNote = hasSelectedMods ? " - mod route failed; check floor/inventory" : " - left on floor (see log)";
				break;
			case SpikeOutcome::kOnFloorMods:
				modsNote = " - selected mods attached; pick it up";
				break;
			default:
				break;
			}
			// A plain item that could not use the OMOD route has not been
			// safely confirmed either; only report "added" for inventory outcomes.
			const bool addedToInventory = outcome == SpikeOutcome::kNotAttempted || outcome == SpikeOutcome::kInventoryOnly ||
				outcome == SpikeOutcome::kDeliveredMods || outcome == SpikeOutcome::kDeliveredPlain;
			Notify(std::format("{} x{} {:08X} {}{}", addedToInventory ? "added" : "bought", item.count, item.formID,
				freeTransfer ? "free" : std::format("({} credits)", item.unitPrice * item.count), modsNote));
		}
		walletCredits.store(PlayerCredits(player), std::memory_order_release);
		// Premium progress: a free haul counts toward Heaven, a paid one toward
		// Hell. Counted per completed basket, not per line, so one bulk order is
		// one action.
		if (freeTransfer) {
			AddProgress(Unlock::kFree, 1);
		} else {
			AddProgress(Unlock::kSpent, totalCost);
		}
		basket.clear();
		state = State::kForms;
	}

	void ShopBrowser::RefreshKeywords()
	{
		selectedFormKeywords.clear();
		if (selectedFormIndex >= forms.size()) {
			return;
		}
		auto* const form = forms[selectedFormIndex];
		if (!form) {
			return;
		}			if (auto* kwForm = KeywordFormOf(form); kwForm) {
				kwForm->ForEachKeyword([&](RE::BGSKeyword* kw) {
					if (kw && !kw->formEditorID.empty()) {
						selectedFormKeywords.emplace_back(kw->formEditorID.c_str());
					}
					return RE::BSContainer::ForEachResult::kContinue;
				});
			}
	}

	void ShopBrowser::Back()
	{
		if (state == State::kTypes) {
			state = State::kPlugins;
		} else if (state == State::kForms) {
			state = State::kTypes;
		} else if (state == State::kCount) {
			state = State::kForms;
		} else {
			open.store(false, std::memory_order_release);
			// Same teardown as Close(): the ESC path used to skip this and leave
			// ItemCapture wanting a frame for a shop that was already gone.
			SFE::ItemCapture::SetShopWantsCapture(false);
			ReleaseSession();
			Notify("closed");
			return;
		}
		cursor = page = 0;
	}

	void ShopBrowser::StepList(const int a_delta)
	{
		const auto total = state == State::kPlugins ? pluginNames.size() :
			state == State::kTypes ? static_cast<std::size_t>(BrowseType::kTotal) :
			state == State::kForms ? forms.size() : 1;
		if (!total) {
			return;
		}
		const auto next = static_cast<std::int64_t>(cursor) + a_delta;
		cursor = next < 0 ? 0 : next >= static_cast<std::int64_t>(total) ? total - 1 : static_cast<std::size_t>(next);
		page = cursor / pageSize;
	}


	// -----------------------------------------------------------------------
	// The ImGui front end. One function by design: the layout reads top to
	// bottom like the window does, and every widget shares this scope.
	// -----------------------------------------------------------------------
	void ShopBrowser::DrawImGui()
	{
		std::scoped_lock lock(stateMutex);
		if (!open.load(std::memory_order_acquire)) {
			return;
		}

		EnsureThemeIni(theme);
		EnsureTextSizeIni(textSize);
		Overlay::SetBodyFontSize(textSize);
		const auto& th = ActiveTheme(theme);
		const auto& ink = th.ink;
		const auto& muted = th.muted;
		const auto& accent = th.accent;
		const auto& accentBright = th.accentBright;
		const auto& panel = th.panel;
		const auto& gold = th.gold;

		ImGui::SetNextWindowSize(ImVec2(1500.0f, 880.0f), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
		ImGui::PushStyleColor(ImGuiCol_WindowBg, Fade(th.windowBg, LoadAlpha()));
		// The window itself stays at the user's alpha, but the child panels are
		// drawn *over* the themed backdrop. Thinning them by an extra factor is
		// what actually lets the art read through without making the whole UI
		// harder to read than the user asked for.
		ImGui::PushStyleColor(ImGuiCol_ChildBg, Fade(panel, LoadAlpha() * 0.72f));
		ImGui::PushStyleColor(ImGuiCol_PopupBg, Fade(panel, LoadAlpha()));
		ImGui::PushStyleColor(ImGuiCol_Border, Fade(th.border, LoadAlpha()));
		ImGui::PushStyleColor(ImGuiCol_Header, Fade(th.header, LoadAlpha()));
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, Fade(th.headerHovered, LoadAlpha()));
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, accent);
		ImGui::PushStyleColor(ImGuiCol_Button, Fade(th.button, LoadAlpha()));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Fade(th.buttonHovered, LoadAlpha()));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, accentBright);
		ImGui::PushStyleColor(ImGuiCol_FrameBg, Fade(th.frame, LoadAlpha()));
		ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Fade(th.frameHovered, LoadAlpha()));
		ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, Fade(th.tableHeader, LoadAlpha()));
		ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, Fade(th.tableRowAlt, LoadAlpha()));
		ImGui::PushStyleColor(ImGuiCol_TextDisabled, th.disabled);
		ImGui::PushStyleColor(ImGuiCol_Separator, Fade(th.separator, LoadAlpha()));
		ImGui::PushStyleColor(ImGuiCol_CheckMark, accent);
		ImGui::PushStyleColor(ImGuiCol_Text, ink);

		ImGui::SetNextWindowSizeConstraints(ImVec2(900.0f, 600.0f), ImVec2(FLT_MAX, FLT_MAX));
		bool windowOpen = true;
		ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
		if (fullscreen) {
			const auto* viewport = ImGui::GetMainViewport();
			ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
			ImGui::SetNextWindowSize(viewport->WorkSize, ImGuiCond_Always);
			windowFlags |= ImGuiWindowFlags_NoDecoration;
		} else {
			ImGui::SetNextWindowSize(ImVec2(1500.0f, 880.0f), ImGuiCond_FirstUseEver);
		}
		if (!ImGui::Begin("DEVILZDAD'S SHOP + EXPLORER", &windowOpen, windowFlags)) {
			ImGui::End();
			ImGui::PopStyleColor(18);
			ImGui::PopStyleVar(2);
			return;
		}
		const bool bodyFontPushed = [&]() {
			if (auto* bodyFont = Overlay::BodyFont()) {
				ImGui::PushFont(bodyFont, bodyFont->LegacySize);
				return true;
			}
			return false;
		}();
		if (!windowOpen) {
			open.store(false, std::memory_order_release);
			ReleaseSession();
		}
		// One ESC edge per frame, shared by the preview pop-out and the page-back
		// handler at the bottom of this frame: dismissing the preview must not
		// also navigate or close the shop on the same press. Read from the
		// window's key stream rather than ImGui's key state: ImGui drops key
		// events outright while it believes the app is unfocused (see
		// Overlay::ConsumeKeyPress), and this key is the only way out of the
		// preview pop-out.
		const bool escapePressed = Overlay::ConsumeKeyPress(VK_ESCAPE);
		bool escapeConsumed = false;

		const auto space = Overlay::ThemeSpaceTexture(th.suffix);
		if (space != static_cast<ImTextureID>(0)) {
			const auto pos = ImGui::GetWindowPos();
			const auto size = ImGui::GetWindowSize();
			// The backdrop is drawn into the window's own draw list, so every
			// BeginChild panel paints over it. What survives is roughly
			// backdropAlpha * (1 - panelAlpha), and at the old value of 44/255
			// that was ~7% - invisible. It now reads as a real backdrop while
			// the panels stay comfortably legible on top.
			//
			// Strength is a setting, not a constant: the per-theme space art
			// ranges from a near-black nebula to a bright inferno, and a single
			// hardcoded alpha that flatters the dark art washes a bright one
			// out through the 72%-alpha panels until the text is unreadable.
			const auto backdropA = static_cast<int>(Overlay::BackdropAlpha());
			ImGui::GetWindowDrawList()->AddImage(space, pos, ImVec2(pos.x + size.x, pos.y + size.y),
				ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, backdropA));
		}

		// Banner. Title left, logo dead centre over the top edge, search +
		// credits right.
		//
		// The logo used to be a third table column sized to fit it. That forced
		// the row taller, and the extra height showed nothing - the title and
		// tagline are single lines - so it now overlaps the side columns and is
		// drawn from the window draw list after EndTable. Two consequences:
		// the logo can be any size at no cost in row height, and because it is
		// centred on the banner's own width rather than on the leftover middle
		// column, it sits truly centred even though it covers both sides.
const float uiScaleHeader = Overlay::UiScale();
		const float logoSize = 104.0f * uiScaleHeader;
		const float bannerAvail = ImGui::GetContentRegionAvail().x;
		// Search is capped rather than filling its column: it was tracking the
		// full side width and reading as a giant empty bar on wide windows.
		const float searchWidth = std::clamp(bannerAvail * 0.16f, 150.0f, 240.0f);
		// Row height is driven by whichever is taller, the logo or the title at
		// its signage size. Driving it from the logo alone clipped the title once
		// the title grew, and from the title alone left a band of dead space when
		// the logo was larger.
		const float bannerTop = ImGui::GetCursorScreenPos().y;
		const float bannerLogoX = ImGui::GetCursorScreenPos().x +
			(bannerAvail - logoSize) * 0.5f;
		// ImFont::FontSize does not exist in this ImGui; LegacySize is the value
		// handed to AddFontFromFileTTF, which is also what PushFont needs.
		// Note PushFont(font, 0.0f) means "keep the *current* size", so passing
		// 0 drew the title face at body size - which is why loading a bigger
		// atlas entry had no visible effect on the rendered text.
		float titleHeight = ImGui::GetTextLineHeight();
		if (auto* tf = Overlay::TitleFont()) {
			titleHeight = tf->LegacySize * 1.25f;
		}
		const float rowHeight = (logoSize > titleHeight ? logoSize : titleHeight) + 12.0f * uiScaleHeader;
		// Two equal stretch columns around one fixed centre column. The earlier
		// version sized the side columns to (avail - logo) / 2 and used fixed
		// widths for them; on a wide window those two widths plus inter-column
		// padding exceeded the available space, and ImGui resolved the overflow
		// by collapsing the middle column - which dragged the actions column
		// back under the title and pushed the logo against the right edge.
		// Equal stretch weights cannot overflow: the fixed centre column takes
		// exactly logoSize and the remainder splits evenly, so the logo is
		// centred by construction.
		if (ImGui::BeginTable("reference_header", 3, ImGuiTableFlags_NoSavedSettings, ImVec2(0.0f, rowHeight))) {
			// Fourth argument is ImGuiID, not a weight: the stretch weight is the
			// third (init_width_or_weight). Passing 1.0f there was a float->ImGuiID
			// narrowing, and both columns defaulted to weight 1 anyway, which is
			// what the centring relies on.
			ImGui::TableSetupColumn("brand", ImGuiTableColumnFlags_WidthStretch, 1.0f);
			ImGui::TableSetupColumn("logo", ImGuiTableColumnFlags_WidthFixed, logoSize);
			ImGui::TableSetupColumn("actions", ImGuiTableColumnFlags_WidthStretch, 1.0f);
			ImGui::TableNextColumn();
			// Title at signage size. This ImGui has no runtime font scaling, so the
			// size comes from a separate atlas entry, and PushFont must be given
			// that entry's LegacySize - 0.0f would keep the current (body) size.
			if (auto* titleFont = Overlay::TitleFont()) {
				ImGui::PushFont(titleFont, titleFont->LegacySize);
			}
			ImGui::TextColored(accentBright, "DevilzDad's Shop + Explorer");
			if (Overlay::TitleFont()) {
				ImGui::PopFont();
			}
			ImGui::TableNextColumn();
			// Empty centre column: the logo is drawn centred from the window draw
			// list after EndTable, so it is not clipped to this column and can
			// overhang without changing the row height.
			ImGui::TableNextColumn();
			// Right column is laid out from the top, with the tagline directly
			// under the search row, both flush to the right margin. Measuring the
			// block first and shifting the cursor is how the right edge gets
			// pinned: ImGui has no right-to-left flow, so the pieces still have
			// to be emitted left-to-right to keep their order (search, credit,
			// balance, Refresh).
			//
			// Widths for the search row and the tagline are measured under their
			// own fonts, because CalcTextSize uses the currently pushed font and
			// the tagline is a different face from the search hint.
			float blockW = 0.0f;
			{
				const auto& style = ImGui::GetStyle();
				const float creditW = Overlay::CreditIconTexture() != static_cast<ImTextureID>(0)
					? 40.0f * uiScaleHeader : 0.0f;
				const auto balanceText = std::format("{}", walletCredits.load(std::memory_order_acquire));
				blockW = searchWidth + style.ItemSpacing.x + creditW +
					(creditW > 0.0f ? style.ItemSpacing.x : 0.0f) +
					ImGui::CalcTextSize(balanceText.c_str()).x +
					style.ItemSpacing.x + ImGui::CalcTextSize("Refresh").x +
					style.FramePadding.x * 2.0f;
				const float columnAvail = ImGui::GetContentRegionAvail().x;
				if (blockW < columnAvail) {
					ImGui::SetCursorPosX(ImGui::GetCursorPosX() + columnAvail - blockW);
				}
			}
			// The tagline is right-aligned independently, against the column's own right
			// edge rather than the search block's, so the two rows stay flush with
			// each other even when one is wider than the other.
			ImGui::SetNextItemWidth(searchWidth);
			ImGui::InputTextWithHint("##shop_search", "Search plugins or items...", search, sizeof(search));
			ImGui::SameLine();
			const auto credit = Overlay::CreditIconTexture();
			if (credit != static_cast<ImTextureID>(0)) {
				ImGui::Image(credit, ImVec2(40.0f * uiScaleHeader, 20.0f * uiScaleHeader));
				ImGui::SameLine();
			}
			ImGui::TextColored(gold, "%u", walletCredits.load(std::memory_order_acquire));
			ImGui::SameLine();
			// Yellow-filled buttons take dark text: see InkOn() below.
			ImGui::PushStyleColor(ImGuiCol_Text, InkOn(th.button, ink));
			if (ImGui::Button("Refresh")) {
				BuildPluginList();
				if (selectedPlugin >= 0) BuildFormList();
			}
			ImGui::PopStyleColor();
			// Tagline on its own line under the search row, right-aligned to the
			// same margin. It is pushed after the search row is emitted, so it
			// lands below it naturally.
			auto* captionFont = Overlay::CaptionFont();
			if (captionFont) {
				ImGui::PushFont(captionFont, captionFont->LegacySize);
			}
			const char* tagline = "Loaded worlds | forbidden wares | one inventory";
			const auto taglineW = ImGui::CalcTextSize(tagline).x;
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - taglineW);
			ImGui::TextColored(muted, "%s", tagline);
			if (captionFont) {
				ImGui::PopFont();
			}
			ImGui::EndTable();

			// Logo, centred on the banner row. Deferred to the draw list so it is
			// not clipped to the centre column. The row's screen top is captured
			// before EndTable, because after it the cursor has already advanced
			// past the row.
			if (const auto logo = Overlay::ShopLogoTexture(); logo != static_cast<ImTextureID>(0)) {
				ImGui::GetWindowDrawList()->AddImage(
					static_cast<ImTextureID>(logo),
					ImVec2(bannerLogoX, bannerTop),
					ImVec2(bannerLogoX + logoSize, bannerTop + logoSize));
			}
		}
		ImGui::Separator();
		ImGui::BeginChild("reference_toolbar", ImVec2(0.0f, 72.0f * Overlay::UiScale()), ImGuiChildFlags_Borders);
		const float uiScale = Overlay::UiScale();
		// Left-to-right list: a control stays on the current row while it
		// fits and wraps to the next line when it does not, so the row
		// neither runs off a narrow panel nor stretches to full width.
		const auto placeInline = [](float a_width) {
			if (ImGui::GetContentRegionAvail().x >= a_width) {
				ImGui::SameLine();
			} else {
				ImGui::NewLine();
			}
		};
		const auto comboWidth = [&]() {
			return std::clamp(ImGui::GetContentRegionAvail().x * 0.30f, 110.0f * uiScale, 210.0f * uiScale);
		};
		ImGui::TextColored(accentBright, "CATALOG");
		placeInline(ImGui::CalcTextSize(BrowseTypeName(selectedType)).x + 60.0f * uiScale);
		ImGui::SetNextItemWidth(comboWidth());

		if (ImGui::BeginCombo("##toolbar_type", BrowseTypeName(selectedType))) {
			for (std::size_t i = 0; i < static_cast<std::size_t>(BrowseType::kTotal); ++i) {
				const auto type = static_cast<BrowseType>(i);
				if (ImGui::Selectable(BrowseTypeName(type), selectedType == type)) {
					std::scoped_lock actionLock(actionMutex);
					pendingSelection = static_cast<std::int32_t>(i);
					pendingPlugin = false;
					pendingType = true;
					pendingSelect = true;
				}
			}
			ImGui::EndCombo();
		}
		auto drawSubcategory = [&](const char* id, auto& selected, auto total, auto name) {
			placeInline(comboWidth() + 40.0f * uiScale);
			ImGui::SetNextItemWidth(comboWidth());
			if (DrawSubcategoryCombo(id, selected, total, name)) {
				selectedFormIndex = 0;
				cursor = page = 0;
				BuildFormList();
			}
		};

		// Show one subtype selector only for the active catalog type. All
		// Types has no single meaningful subtype taxonomy, so it has none.
		switch (selectedType) {
		case BrowseType::kWeapon:
			drawSubcategory("##toolbar_weaponsub", selectedWeaponSub, WeaponSub::kTotal, WeaponSubName);
			break;
		case BrowseType::kArmor:
			drawSubcategory("##toolbar_armorsub", selectedArmorSub, ArmorSub::kTotal, ArmorSubName);
			break;
		case BrowseType::kAmmo:
			drawSubcategory("##toolbar_ammosub", selectedAmmoSub, AmmoSub::kTotal, AmmoSubName);
			break;
		case BrowseType::kMisc:
			drawSubcategory("##toolbar_miscsub", selectedMiscSub, MiscSub::kTotal, MiscSubName);
			break;
		case BrowseType::kAid:
			drawSubcategory("##toolbar_aidsub", selectedAidSub, AidSub::kTotal, AidSubName);
			break;
		case BrowseType::kBook:
			drawSubcategory("##toolbar_booksub", selectedBookSub, BookSub::kTotal, BookSubName);
			break;
		case BrowseType::kKey:
			drawSubcategory("##toolbar_keysub", selectedKeySub, KeySub::kTotal, KeySubName);
			break;
		default:  // All Types and Ingredients have no subtype control.
			break;
		}
		placeInline(60.0f * uiScale);
		ImGui::TextColored(muted, "Qty");						ImGui::SameLine();
						ImGui::SetNextItemWidth(60.0f * uiScale);
						ImGui::InputInt("##toolbar_quantity", &addCount, 1, 10);

		addCount = std::clamp(addCount, 1, 999);
		placeInline(72.0f * uiScale);
		ImGui::Checkbox("Free", &freeTransfer);
		// The five filters that decide what the catalog contains now live in the
		// catalog's own row instead of the plugins pane: they hide records, which
		// is a property of the list, not of which plugin is selected. A popup
		// keeps one row of controls from wrapping a second time on a small
		// window, which is what moving five checkboxes inline would have done.
		placeInline(70.0f * uiScale);
		ImGui::PushStyleColor(ImGuiCol_Text, InkOn(th.button, ink));
		if (ImGui::Button("Hide")) {
			ImGui::OpenPopup("##shop_hide");
		}
		ImGui::PopStyleColor();
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Which records the catalog leaves out: test/debug, 'report as bug', ships/hair, crew/enemy/creature, turrets, duplicate names");
		}
		if (ImGui::BeginPopup("##shop_hide")) {
			ImGui::TextColored(accentBright, "HIDE FROM THE CATALOG");
			// Same behaviour the checkboxes had in the plugins pane: every toggle
			// rebuilds the list and drops the selection to the top, because the
			// selected row may be one of the records that just vanished.
			const auto hideToggle = [&](const char* a_label, bool& a_flag) {
				if (ImGui::Checkbox(a_label, &a_flag)) {
					selectedFormIndex = 0;
					cursor = page = 0;
					BuildFormList();
				}
			};
			hideToggle("Hide test/debug", hideTestDebug);
			// The ESM shop marks every record it could not name with an
			// "(report this as a bug)" suffix; those rows flood the weapon
			// catalog with bare "DL-19"-style placeholders.
			hideToggle("Hide 'report as bug' items", hideReportBug);
			hideToggle("Hide ship/hair/etc", hideShipHair);
			hideToggle("Hide crew/enemy/creature", hideCrewEnemyCreature);
			// Turrets and the ammunition they fire are the same ship hardware:
			// the ammo clause drops the unnamed crate dummy forms and every round
			// with no mesh (missiles, torpedoes, creature and ship rounds).
			hideToggle("Hide turrets/ship ammo", hideTurrets);
			// Not a class of record like the toggles above: two records with the
			// same name and stats are both real and both spawnable, so this only
			// collapses them to one row. The row keeps the count, the item pane
			// lists the records, and turning this off shows every one of them.
			hideToggle("Hide duplicate names", hideDuplicateNames);
			ImGui::EndPopup();
		}
		placeInline(150.0f * uiScale);
		if (ImGui::Button("Audio")) {
			ImGui::OpenPopup("##shop_audio");
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Music and sound levels, mute, and the track picker");
		}
		// The track list is a directory scan, so refresh it only when the popup
		// opens. A track dropped in mid-session therefore appears the next time
		// the popup is opened, without doing filesystem work every frame.
		static std::vector<std::string> audioTracks;
		static bool audioTracksFresh{ false };
		const bool audioPopupOpen = ImGui::IsPopupOpen("##shop_audio");
		if (audioPopupOpen && !audioTracksFresh) {
			audioTracks = ShopAudio::ListMusicTracks();
			audioTracksFresh = true;
		} else if (!audioPopupOpen) {
			audioTracksFresh = false;
		}
		if (ImGui::BeginPopup("##shop_audio")) {
			ImGui::TextColored(accentBright, "AUDIO");
			ImGui::SetNextItemWidth(190.0f * uiScale);
			if (ImGui::SliderInt("##music_volume", &musicVolume, 0, 100, "Music %d%%")) {
				ApplyAudioSettings();
			}
			if (ImGui::IsItemDeactivatedAfterEdit()) {
				SaveAudioSettings();
			}
			ImGui::SameLine();
			if (ImGui::Checkbox("Mute##music_mute", &musicMuted)) {
				if (!musicMuted) {
					ShopAudio::SetMusicFile(MusicTrackFile().c_str());
					ShopAudio::StartMusic();
				}
				ApplyAudioSettings();
				SaveAudioSettings();
			}
			ImGui::SetNextItemWidth(190.0f * uiScale);
			if (ImGui::SliderInt("##sfx_volume", &sfxVolume, 0, 100, "Sound %d%%")) {
				ApplyAudioSettings();
			}
			if (ImGui::IsItemDeactivatedAfterEdit()) {
				SaveAudioSettings();
			}
			ImGui::SameLine();
			if (ImGui::Checkbox("Mute##sfx_mute", &sfxMuted)) {
				ApplyAudioSettings();
				SaveAudioSettings();
			}
			// Music is chosen independently of the visual theme: "(theme track)"
			// follows the theme's own score, any other entry overrides it - so a
			// player can drop their own track into Audio\ (wav, mp3, flac, m4a,
			// wma) and shop to it across every theme.
			ImGui::SetNextItemWidth(240.0f * uiScale);
			if (ImGui::BeginCombo("##music_track", musicTrack.empty() ? "(theme track)" : musicTrack.c_str())) {
				if (ImGui::Selectable("(theme track)", musicTrack.empty())) {
					musicTrack.clear();
					SaveAudioSettings();
					ShopAudio::SetMusicFile(MusicTrackFile().c_str());
					if (!musicMuted) {
						ShopAudio::StartMusic();
					}
				}
				for (const auto& track : audioTracks) {
					if (ImGui::Selectable(track.c_str(), musicTrack == track)) {
						musicTrack = track;
						SaveAudioSettings();
						ShopAudio::SetMusicFile(musicTrack.c_str());
						if (!musicMuted) {
							ShopAudio::StartMusic();
						}
					}
				}
				ImGui::EndCombo();
			}
			// A soundtrack folder is the only selection with a next track. Two
			// compact buttons beside the name, not a transport bar under the
			// list: the mixer already advances on its own at the end of a track,
			// so these are "not this one" - the only control an album needs.
			if (ShopAudio::MusicIsPlaylist()) {
				ImGui::SameLine();
				ImGui::PushStyleColor(ImGuiCol_Text, InkOn(th.button, ink));
				if (ImGui::Button("<##music_prev")) {
					ShopAudio::SkipMusicTrack(-1);
				}
				ImGui::PopStyleColor();
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Previous track in the soundtrack");
				}
				ImGui::SameLine();
				ImGui::PushStyleColor(ImGuiCol_Text, InkOn(th.button, ink));
				if (ImGui::Button(">##music_next")) {
					ShopAudio::SkipMusicTrack(1);
				}
				ImGui::PopStyleColor();
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Next track in the soundtrack");
				}
			}
			if (audioTracks.empty()) {
				ImGui::TextColored(muted, "No tracks in Audio\\ - drop a .wav, .mp3 or .flac in");
			}
			ImGui::EndPopup();
		}
		// Use a globally selected atlas face for the body so every control and
		// child window gets the same size without a frame-local font push/pop
		// pair that could leak when a clipped table is skipped.
		placeInline(comboWidth());
		ImGui::SetNextItemWidth(comboWidth());
		static constexpr const char* kTextSizes[]{ "Small", "Medium", "Large", "Extra Large" };
		if (ImGui::BeginCombo("##shop_text_size", kTextSizes[std::clamp(textSize, 0, 3)])) {
			for (int size = 0; size < 4; ++size) {
				if (ImGui::Selectable(kTextSizes[size], textSize == size)) {
					textSize = size;
					Overlay::SetBodyFontSize(size);
					SaveTextSizeIni(size);
				}
			}
			ImGui::EndCombo();
		}
		placeInline(comboWidth());
		ImGui::SetNextItemWidth(comboWidth());

		LoadProgress();
		if (ImGui::BeginCombo("##shop_theme", ThemeLabel(static_cast<Theme>(theme)).c_str())) {
			for (int t = 0; t < static_cast<int>(Theme::kTotal); ++t) {
				const auto candidate = static_cast<Theme>(t);
				const bool locked = IsPremium(candidate) && !IsUnlocked(candidate);
				if (locked) {
					// Kept selectable-looking but inert: clicking explains why
					// rather than silently doing nothing.
					ImGui::PushStyleColor(ImGuiCol_Text, th.disabled);
					ImGui::Selectable(ThemeLabel(candidate).c_str(), false);
					ImGui::PopStyleColor();
					if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
						Notify("locked - keep shopping to earn it");
					}
					continue;
				}
				if (ImGui::Selectable(ThemeLabel(candidate).c_str(), theme == t)) {
					theme = t;
					SaveThemeIni(t);
					// The theme change was only wired into Open(), so switching
					// themes while the shop was already up kept the old score.
					// A track the user picked deliberately is left alone; only the
					// "(theme track)" selection follows the new theme.
					if (musicTrack.empty()) {
						ShopAudio::SetMusicFile(ActiveTheme(t).music);
						if (!musicMuted) {
							ShopAudio::StartMusic();
						}
					}
				}
			}
			ImGui::EndCombo();
		}
		// The custom theme is the one theme a player owns, so its only control is
		// the editor. It appears once that theme is selected, next to the combo.
		if (theme == static_cast<int>(Theme::kCustom)) {
			ImGui::SameLine();
			ImGui::PushStyleColor(ImGuiCol_Text, InkOn(th.button, ink));
			if (ImGui::Button("CUSTOMISE##theme_customise")) {
				ImGui::OpenPopup("##custom_theme_editor");
			}
			ImGui::PopStyleColor();
			ImGui::SetNextWindowSize(ImVec2(430.0f * uiScale, 0.0f), ImGuiCond_Appearing);
			if (ImGui::BeginPopupModal("##custom_theme_editor", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
				auto& custom = Custom();
				static char nameBuffer[64]{};
				static std::vector<std::string> editorTracks;
				if (ImGui::IsWindowAppearing()) {
					strncpy_s(nameBuffer, custom.name.c_str(), _TRUNCATE);
					editorTracks = ShopAudio::ListMusicTracks();
				}
				ImGui::TextColored(accentBright, "CUSTOM THEME");
				ImGui::TextColored(muted, "Earned by unlocking all three premium themes.");
				ImGui::Separator();
				ImGui::SetNextItemWidth(250.0f * uiScale);
				if (ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer))) {
					custom.name = nameBuffer;
					SyncCustomThemeStrings();
				}
				// Every backdrop the overlay actually loaded: the shared space.png
				// as the empty suffix, then each space_<name>.<png|jpg|bmp> it
				// found beside the plugin - the shipped theme art and anything
				// the player dropped in. A fixed list here would have meant the
				// folder could hold images the picker never offered.
				const auto backdrops = Overlay::BackdropSuffixes();
				ImGui::SetNextItemWidth(250.0f * uiScale);
				if (ImGui::BeginCombo("Background", custom.backdrop.empty() ? "(shared)" : custom.backdrop.c_str())) {
					if (ImGui::Selectable("(shared)", custom.backdrop.empty())) {
						custom.backdrop.clear();
						SyncCustomThemeStrings();
					}
					for (const auto& candidate : backdrops) {
						if (ImGui::Selectable(candidate.c_str(), custom.backdrop == candidate)) {
							custom.backdrop = candidate;
							SyncCustomThemeStrings();
						}
					}
					ImGui::EndCombo();
				}
				if (!custom.backdrop.empty() && std::find(backdrops.begin(), backdrops.end(), custom.backdrop) == backdrops.end()) {
					ImGui::TextColored(muted, "Saved backdrop '%s' is gone; the shared art is used.", custom.backdrop.c_str());
				} else {
					ImGui::TextColored(muted, "Drop space_<name>.png or .jpg beside the plugin to add one.");
				}
				ImGui::SetNextItemWidth(250.0f * uiScale);
				if (ImGui::BeginCombo("Music", custom.music.c_str())) {
					for (const auto& track : editorTracks) {
						if (ImGui::Selectable(track.c_str(), custom.music == track)) {
							custom.music = track;
							SyncCustomThemeStrings();
							if (musicTrack.empty() && !musicMuted) {
								ShopAudio::SetMusicFile(custom.music.c_str());
								ShopAudio::StartMusic();
							}
						}
					}
					ImGui::EndCombo();
				}
				ImGui::Separator();
				// Layout: where each pane sits, and how wide the two fixed ones are.
				// The shop behind the window redraws as these change, so a preset
				// is chosen by looking at it rather than by its name.
				ImGui::TextColored(accentBright, "LAYOUT");
				ImGui::SetNextItemWidth(250.0f * uiScale);
				if (ImGui::BeginCombo("Arrangement", LayoutName(custom.layout))) {
					for (int l = 0; l < static_cast<int>(Layout::kTotal); ++l) {
						const auto candidate = static_cast<Layout>(l);
						if (ImGui::Selectable(LayoutName(candidate), custom.layout == candidate)) {
							custom.layout = candidate;
						}
					}
					ImGui::EndCombo();
				}
				ImGui::TextColored(muted, "%s", LayoutHint(custom.layout));
				ImGui::SetNextItemWidth(250.0f * uiScale);
				ImGui::SliderFloat("Plugins pane", &custom.pluginsWidth, 180.0f, 520.0f, "%.0f px");
				ImGui::SetNextItemWidth(250.0f * uiScale);
				ImGui::SliderFloat("Basket pane", &custom.detailsWidth, 240.0f, 620.0f, "%.0f px");
				ImGui::TextColored(muted, "The divider between panes still drags; SAVE keeps these widths.");
				ImGui::Separator();
				auto colours = [&](const char* a_header, std::size_t a_from, std::size_t a_to) {
					if (!ImGui::CollapsingHeader(a_header, ImGuiTreeNodeFlags_DefaultOpen)) {
						return;
					}
					for (std::size_t i = a_from; i < a_to; ++i) {
						const auto& colour = kCustomColours[i];
						auto&       value = custom.ui.*(colour.field);
						ImGui::ColorEdit4(colour.key, &value.x, ImGuiColorEditFlags_NoInputs);
					}
				};
				colours("Accents", 0, 3);
				colours("Surfaces", 3, 6);
				colours("Text", 6, 9);
				colours("Controls", 9, 15);
				colours("Structure", 15, 19);
				ImGui::Separator();
				ImGui::PushStyleColor(ImGuiCol_Text, InkOn(th.button, ink));
				if (ImGui::Button("SAVE##custom_save")) {
					SaveCustomTheme();
				}
				ImGui::PopStyleColor();
				ImGui::SameLine();
				if (ImGui::Button("RESET##custom_reset")) {
					ResetCustomTheme();
					strncpy_s(nameBuffer, custom.name.c_str(), _TRUNCATE);
				}
				ImGui::SameLine();
				if (ImGui::Button("CLOSE##custom_close")) {
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			}
		}
		placeInline(90.0f * uiScale);
		ImGui::PushStyleColor(ImGuiCol_Text, InkOn(th.button, ink));
		if (ImGui::Button(fullscreen ? "WINDOWED" : "MAXIMIZE")) {
			fullscreen = !fullscreen;
			SaveShopIniBool(L"fullscreen", fullscreen);
		}
		ImGui::PopStyleColor();
		placeInline(64.0f * uiScale);
		if (ImGui::Button("Close")) {
			open.store(false, std::memory_order_release);
			ReleaseSession();
		}
		ImGui::EndChild();

		// NOTE: do not wrap this table in PushFont/PopFont. BeginTable returns
		// false when the table is skipped, so a pop placed inside the block
		// would be skipped too and the font stack would grow every frame -
		// see the note where the font-size selector used to be.
		// The workspace is one table and the three panes always draw in the same
		// order; an arrangement only says which column each one lands in, so a
		// preset cannot disagree with the pane code. The stacked arrangement is
		// the single exception: there the top row is its own plugins/items table
		// and the basket gets the second row, full width.
		LoadCustomTheme();
		const auto&  layoutState = Custom();
		const int    layoutMode = static_cast<int>(layoutState.layout);
		const bool   stacked = layoutMode == static_cast<int>(Layout::kCatalogTop);
		int          pluginColumn = 0;
		int          itemsColumn = 1;
		int          detailsColumn = 2;
		if (layoutMode == static_cast<int>(Layout::kBasketCentre)) {
			detailsColumn = 1;
			itemsColumn = 2;
		} else if (layoutMode == static_cast<int>(Layout::kBasketLeft)) {
			detailsColumn = 0;
			pluginColumn = 1;
			itemsColumn = 2;
		}
		if (stacked) {
			// The outer table has one column, so the basket's row is column 0
			// and the plugins/items pair lives in the nested top-row table.
			pluginColumn = 0;
			itemsColumn = 1;
			detailsColumn = 0;
		}
		const float pluginWidth = std::clamp(layoutState.pluginsWidth, 180.0f, 520.0f);
		const float detailsWidth = std::clamp(layoutState.detailsWidth, 240.0f, 620.0f);
		const float workspaceOffset = -32.0f * Overlay::UiScale();
		const float workspaceBody = std::max(120.0f, ImGui::GetContentRegionAvail().y + workspaceOffset);
		// One table ID per arrangement. Sharing the ID made ImGui reuse the first
		// arrangement's column widths, so switching presets kept the old widths.
		ImGui::PushID(layoutMode);
		if (ImGui::BeginTable("reference_workspace", stacked ? 1 : 3, ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable, ImVec2(0.0f, workspaceOffset))) {
			if (stacked) {
				ImGui::TableSetupColumn("workspace", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				// The top row is capped by a child, not by the nested table's
				// height argument: a table only honours that height when it
				// scrolls, so an uncapped nested table takes the whole pane and
				// leaves the basket a few pixels underneath it.
				const float splitHeight = std::clamp(workspaceBody * 0.6f, 140.0f, std::max(140.0f, workspaceBody - 180.0f));
				ImGui::BeginChild("workspace_catalog_top", ImVec2(0.0f, splitHeight), 0);
				ImGui::BeginTable("workspace_catalog_split", 2, ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable, ImVec2(0.0f, 0.0f));
				ImGui::TableSetupColumn("plugins", ImGuiTableColumnFlags_WidthFixed, pluginWidth);
				ImGui::TableSetupColumn("items", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableNextRow();
			} else {
				for (int column = 0; column < 3; ++column) {
					if (column == pluginColumn) {
						ImGui::TableSetupColumn("plugins", ImGuiTableColumnFlags_WidthFixed, pluginWidth);
					} else if (column == detailsColumn) {
						// The expanded preview pops out into its own window, so the
						// details column keeps its normal width in both states.
						ImGui::TableSetupColumn("details", ImGuiTableColumnFlags_WidthFixed, detailsWidth);
					} else {
						ImGui::TableSetupColumn("items", ImGuiTableColumnFlags_WidthStretch);
					}
				}
				// A row has to exist before a column can be selected: TableNextColumn
				// used to start row 0 implicitly, but TableSetColumnIndex does not and
				// TableNextRow is also what locks the table layout. Without this the
				// first cell is drawn at an unset row position and the shop dies on
				// its first frame.
				ImGui::TableNextRow();
			}

			ImGui::TableSetColumnIndex(pluginColumn);
			ImGui::BeginChild("reference_plugins", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
			ImGui::TextColored(accentBright, "PLUGINS");
			ImGui::SameLine();
			const auto sourceCount = FitTextToWidth(std::format("%zu loaded sources", pluginNames.size()), ImGui::GetContentRegionAvail().x);
			ImGui::TextColored(muted, "%s", sourceCount.c_str());

			// Where the five hide filters used to sit. They belong to the catalog,
			// so they moved to its toolbar; this pane keeps the one control that
			// governs the item pane itself. The count is in the label because the
			// star column is the only other place a favourite is visible.
			{
				const auto favLabel = std::format("Favourites ({})###favourites", favourites.size());
				if (ImGui::Checkbox(favLabel.c_str(), &favouritesOnly)) {
					selectedFormIndex = 0;
					cursor = page = 0;
					BuildFormList();
				}
				// Drawn after the checkbox, and filled only when the filter is on,
				// so the tick and the star say the same thing.
				{
					const ImVec2 boxMin = ImGui::GetItemRectMin();
					const ImVec2 boxMax = ImGui::GetItemRectMax();
					const float radius = ImGui::GetTextLineHeight() * 0.42f;
					DrawStarGlyph(ImGui::GetWindowDrawList(),
						ImVec2(boxMax.x + radius * 1.4f, (boxMin.y + boxMax.y) * 0.5f), radius,
						ImGui::GetColorU32(favouritesOnly ? gold : muted), favouritesOnly, 1.5f);
				}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip(favouritesOnly
						? "Showing only the starred records - untick to go back to the catalog"
						: "List every starred record, from any plugin and any category");
				}
			}
			ImGui::Separator();
			{
				ImGui::PushID(-1);
				if (ImGui::Selectable("All Plugins", allPlugins, 0, ImVec2(0.0f, 29.0f))) {
					allPlugins = true;
					selectedFormIndex = 0;
					cursor = page = 0;
					BuildFormList();
				}
				ImGui::PopID();
			}
			for (std::size_t i = 0; i < pluginNames.size(); ++i) {
				if (search[0] && !FilterMatches(pluginNames[i], search)) {
					continue;
				}
				ImGui::PushID(static_cast<int>(i));
				const bool selected = !allPlugins && selectedPlugin == static_cast<std::int32_t>(i);
				if (ImGui::Selectable(pluginNames[i].c_str(), selected, 0, ImVec2(0.0f, 29.0f))) {
					allPlugins = false;
					selectedFormIndex = 0;
					cursor = page = 0;
					std::scoped_lock actionLock(actionMutex);
					pendingSelection = static_cast<std::int32_t>(i);
					pendingPlugin = true;
					pendingType = false;
					pendingSelect = true;
				}
				ImGui::PopID();
			}
			ImGui::EndChild();

			ImGui::TableSetColumnIndex(itemsColumn);
			// Empty-category honesty: Starfield's data carries no INGR records at
			// all (SFDevilzDump-all-forms.csv: zero INGR rows across every plugin).
			// Consumables live under Aid; say so instead of a silently blank list.
			ImGui::BeginChild("reference_items", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
				// The favourites view spans every source, so naming a plugin there
				// would be a lie; the row count beside it is the useful number.
				const char* pluginLabel = favouritesOnly ? "Favourites" : (allPlugins ? "All Plugins" :
				(selectedPlugin >= 0 && static_cast<std::size_t>(selectedPlugin) < pluginNames.size() ? pluginNames[static_cast<std::size_t>(selectedPlugin)].c_str() : "No plugin"));
			if (favouritesOnly) {
				const ImVec2 start = ImGui::GetCursorScreenPos();
				const float lineHeight = ImGui::GetTextLineHeight();
				const float radius = lineHeight * 0.42f;
				ImGui::Dummy(ImVec2(radius * 2.2f, lineHeight));
				DrawStarGlyph(ImGui::GetWindowDrawList(), ImVec2(start.x + radius, start.y + lineHeight * 0.5f),
					radius, ImGui::GetColorU32(gold), true, 1.5f);
				ImGui::SameLine(0.0f, 4.0f);
			}
			ImGui::TextColored(ink, "%s", pluginLabel);
			ImGui::SameLine();
			ImGui::TextColored(muted, "(%zu records)", forms.size());
			ImGui::SameLine();
			ImGui::TextColored(muted, "↑/← prev  ↓/→ next  Enter add");
			// The pop-out preview must not cost the catalog its keys. This used to
			// require !IsAnyItemActive(), and the pop-out's orbit button becomes
			// the active item as soon as it is clicked or nav-focused, so arrow
			// navigation silently stopped working for as long as the preview was
			// popped out - the report that came with the armor screen. Only things
			// that genuinely own keys should block: a text field (WantTextInput)
			// and an open combo popup. A held or focused button does not.
			const bool keyboardReady = !ImGui::GetIO().WantTextInput &&
				!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup);
			if (selectedType == BrowseType::kIngr) {
				ImGui::SameLine();
				ImGui::TextColored(gold, "— no Ingredients exist in Starfield's data; use Aid");
			}
			// Armor/Other used to be nothing but actor skins; say why it is empty
			// instead of leaving a blank list that looks like a filter failure.
			if (selectedType == BrowseType::kArmor && selectedArmorSub == ArmorSub::kOther && forms.empty()) {
				ImGui::SameLine();
				ImGui::TextColored(gold, "— actor skins (creature, robot and turret bodies) are not items; hidden");
			}
			// An empty favourites list is the first thing a new profile sees after
			// ticking the box, so say how a record gets there.
			if (favouritesOnly && forms.empty()) {
				ImGui::TextColored(gold, favourites.empty()
					? "No starred records yet - the star at the left of any catalog row adds one"
					: "The starred records are not in this load order any more");
			}
			ImGui::Separator();
			std::size_t shownRows = 0;
			// Unstarring inside the favourites view changes the list the clipper is
			// walking, so the rebuild is deferred until the table has finished.
			bool rebuildAfterTable = false;
			if (ImGui::BeginTable("reference_form_table", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 0.0f))) {
				ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableSetupColumn("TYPE", ImGuiTableColumnFlags_WidthFixed, 62.0f);
				ImGui::TableSetupColumn("VALUE", ImGuiTableColumnFlags_WidthFixed, 70.0f);
				ImGui::TableSetupColumn("WT", ImGuiTableColumnFlags_WidthFixed, 56.0f);
				ImGui::TableSetupColumn("FORMID", ImGuiTableColumnFlags_WidthFixed, 96.0f);
			ImGui::TableSetupScrollFreeze(0, 1);
			// Clickable sort header: name/type alphabetical, value/wt/formid
			// numeric. Click again to flip direction.
			ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
			{
				// The star sits in the NAME cell, header and rows alike, instead of
				// owning a fixed column beside it. With a column of its own the
				// star and the label were two independent cells, and after a NAME
				// sort they could land on different rows: a star alone on a blank
				// line, its name one row below, and that row twice as tall as every
				// other. One cell cannot disagree with itself. The star is still
				// submitted first, so it keeps the hover the row selectable would
				// otherwise take.
				ImGui::TableSetColumnIndex(0);
				{
					const ImVec2 start = ImGui::GetCursorScreenPos();
					const float lineHeight = ImGui::GetTextLineHeight();
					ImGui::Dummy(ImVec2(kStarSlot, lineHeight));
					DrawStarGlyph(ImGui::GetWindowDrawList(), ImVec2(start.x + kStarSlot * 0.5f, start.y + lineHeight * 0.5f),
						lineHeight * 0.36f, ImGui::GetColorU32(muted), true, 1.0f);
				}
				ImGui::SameLine(0.0f, kStarGap);
				// The star has no sort of its own: favourites keep the order they
				// were starred in. The five headers are the old column set, so
				// sortColumn keeps its 0-4 meaning (and the INI values written by
				// earlier builds still name the right column).
				const char* titles[5]{ "NAME", "TYPE", "VALUE", "WT", "FORMID" };
				for (int c = 0; c < 5; ++c) {
					if (c > 0) {
						ImGui::TableSetColumnIndex(c);
					}
					std::string head = titles[c];
					if (sortColumn == c) {
						head += sortAscending ? " ▲" : " ▼";
					}
					ImGui::PushID(100 + c);
					// Sort headers are accent-filled too, so same rule as the
					// toolbar buttons: dark text on the bright fill.
					ImGui::PushStyleColor(ImGuiCol_Text, InkOn(th.button, ink));
					if (ImGui::SmallButton(head.c_str())) {
							if (sortColumn == c) {
								sortAscending = !sortAscending;
							} else {
								sortColumn = c;
								sortAscending = true;
							}
							ApplyViewOrder();
							const auto ini = ShopIniPath();
							const auto colText = std::to_wstring(sortColumn + 1);
							WritePrivateProfileStringW(L"shop", L"sortColumn", colText.c_str(), ini.c_str());
							WritePrivateProfileStringW(L"shop", L"sortAscending", sortAscending ? L"1" : L"0", ini.c_str());
						}
					ImGui::PopStyleColor();
					ImGui::PopID();
				}
			}
			// Visible-row collection, then a clipper over the matches: only
			// on-screen rows pay for Selectable/Text widgets (6575 unclipped
			// rows lagged hard). The list is derived from viewOrder, forms and
			// the search text, so it is rebuilt when one of those changes rather
			// than every frame: searching used to re-derive a display name, a
			// model path and two std::format calls for each of 9,000 rows, 60
			// times a second, while a term was being typed.
			if (visRowsStamp != listStamp || visRowsSearch != search) {
				visRowsStamp = listStamp;
				visRowsSearch = search;
				const auto searchStarted = std::chrono::steady_clock::now();
				visRows.clear();
				visRows.reserve(viewOrder.size());
				if (!search[0]) {
					for (const auto v : viewOrder) {
						if (v < forms.size()) {
							visRows.push_back(v);
						}
					}
				} else {
					EnsureSearchKeys();
					const auto needle = CaseFold(search);
					for (const auto v : viewOrder) {
						if (v < searchKeys.size() && searchKeys[v].find(needle) != std::string::npos) {
							visRows.push_back(v);
						}
					}
					REX::INFO("SFShopExplorer: search '{}' matched {} of {} row(s) in {:.2f} ms", search,
						visRows.size(), forms.size(),
						std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - searchStarted).count());
				}
			}
			shownRows = visRows.size();
			// This overlay receives game-window key messages only while input
			// capture is enabled. Keep catalog navigation available with the
			// preview pop-out covering the table, but leave text fields, active
			// controls, and combo popups in charge of their own keys.
			// Same source as Escape: catalog navigation must not depend on ImGui's
			// key state, which the popped-out preview could leave dead. Auto-repeat
			// is included here on purpose - holding an arrow walks the list.
			const bool up = Overlay::ConsumeKeyPress(VK_UP);
			const bool down = Overlay::ConsumeKeyPress(VK_DOWN);
			const bool left = Overlay::ConsumeKeyPress(VK_LEFT);
			const bool right = Overlay::ConsumeKeyPress(VK_RIGHT);
			// Win32 reports the keypad's Enter as VK_RETURN too, so one lookup
			// covers both (the ImGui backend mapped them to two keys; the raw
			// message stream does not).
			const bool enter = Overlay::ConsumeKeyPress(VK_RETURN);
			const bool navigatePrev = (up || left) && !(down || right);
			const bool navigateNext = (down || right) && !(up || left);
			if (up || down || left || right || enter) {
				REX::INFO("SFShopExplorer ui: catalog key up={} down={} left={} right={} enter={} ready={} rows={} selected={} popout={} itemActive={} textInput={}",
					up, down, left, right, enter, keyboardReady, visRows.size(), selectedFormIndex,
					previewExpanded, ImGui::IsAnyItemActive(), ImGui::GetIO().WantTextInput);
			}
			bool keyboardSelectionMoved = false;
			if (keyboardReady && !visRows.empty()) {
				auto rowIt = std::find(visRows.begin(), visRows.end(), selectedFormIndex);
				std::size_t row = rowIt == visRows.end() ? 0 : static_cast<std::size_t>(rowIt - visRows.begin());
				if (rowIt == visRows.end()) {
					if (navigatePrev && !enter) row = visRows.size() - 1;
				} else if (navigatePrev) {
					if (row > 0) --row;
				} else if (navigateNext) {
					if (row + 1 < visRows.size()) ++row;
				}
				const std::size_t nextSelection = visRows[row];
				keyboardSelectionMoved = nextSelection != selectedFormIndex;
				selectedFormIndex = nextSelection;
				cursor = selectedFormIndex;				if (keyboardSelectionMoved) {
					addCount = 1;
					if (const auto* selected = forms[selectedFormIndex]) {
						selectedFormLabel = FormPreviewLabel(selected);
					}
				}
				if (enter && selectedFormIndex < forms.size() && forms[selectedFormIndex]) {
					const auto* added = forms[selectedFormIndex];
					ConfirmAdd();
					// Same act as the ADD TO BASKET button and the row double-click,
					// so it gets the same sting; pressing Enter was the one add path
					// that made no sound at all.
					ShopAudio::PlaySfx(ShopAudio::Sfx::kSelect);
					Notify(std::format("added {} to basket", FormDisplayName(added)));
				}
			}
			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(visRows.size()));
			if (keyboardReady && !visRows.empty()) {
				const auto selected = std::find(visRows.begin(), visRows.end(), selectedFormIndex);
				if (selected != visRows.end()) {
					clipper.IncludeItemByIndex(static_cast<int>(selected - visRows.begin()));
				}
			}
			while (clipper.Step()) {

				for (int d = clipper.DisplayStart; d < clipper.DisplayEnd; ++d) {
					const std::size_t i = visRows[static_cast<std::size_t>(d)];
					auto* form = i < forms.size() ? forms[i] : nullptr;
					if (!form) {
						continue;
					}
					ImGui::TableNextRow();
					ImGui::PushID(static_cast<int>(i));
					ImGui::TableNextColumn();
					// Star and label share the cell, star first: its own item claims
					// hover before the SpanAllColumns row Selectable below is
					// submitted, so a click here toggles the favourite without
					// selecting the row, adding it to the basket, or affecting the
					// double-click add.
					{
						const bool starred = IsFavourite(form->GetFormID());
						const ImVec2 cellPos = ImGui::GetCursorScreenPos();
						const float lineHeight = ImGui::GetTextLineHeight();
						// InvisibleButton, not Selectable: the hit area is the slot without
						// the row-height padding a text label would add, and there is no
						// glyph to mis-measure. Outline when it is not a favourite, filled
						// when it is. Fixed width, not the cell's available width: the
						// label is laid out on the same line, beside it.
						if (ImGui::InvisibleButton("##favourite", ImVec2(kStarSlot, lineHeight))) {
							ToggleFavourite(form->GetFormID(), FormDisplayName(form));
							ShopAudio::PlaySfx(ShopAudio::Sfx::kClick);
							// In the favourites view an unstarred row no longer belongs to
							// the list; the rebuild happens after the table, not inside the
							// loop the clipper is walking.
							rebuildAfterTable = rebuildAfterTable || favouritesOnly;
						}
						DrawStarGlyph(ImGui::GetWindowDrawList(),
							ImVec2(cellPos.x + kStarSlot * 0.5f, cellPos.y + lineHeight * 0.5f),
							lineHeight * 0.36f, ImGui::GetColorU32(starred ? gold : muted), starred, 1.5f);
						if (ImGui::IsItemHovered()) {
							ImGui::SetTooltip(starred ? "Remove from favourites" : "Add to favourites");
						}
					}
					ImGui::SameLine(0.0f, kStarGap);
					// A row that stands for more than one record says so: the same name
					// listed twice is two records, not a listing fault (see the grouping
					// in BuildFormList), and the item pane names them.
					std::string rowLabel = FormRowLabel(form);
					if (const auto group = duplicateGroupOf.find(form->GetFormID()); group != duplicateGroupOf.end()) {
						rowLabel += std::format("  ({} records)", group->second.size());
					}
					const auto visibleRowLabel = FitTextToWidth(rowLabel, ImGui::GetContentRegionAvail().x);
					if (ImGui::Selectable(visibleRowLabel.c_str(), selectedFormIndex == i, ImGuiSelectableFlags_SpanAllColumns)) {
					selectedFormIndex = i;
					cursor = i;
					addCount = 1;
					// Cache the selected form's display name for the direct preview.

					selectedFormLabel = FormPreviewLabel(form);
					ShopAudio::PlaySfx(ShopAudio::Sfx::kSelect);
				}
				if (keyboardSelectionMoved && selectedFormIndex == i) {
					ImGui::SetScrollHereY(0.5f);
				}
				// Build 94: a second press on the same row within the
				// double-click interval adds it to the basket. The
				// tracker is manual (not ImGui's IsMouseDoubleClicked)
				// because Selectable activates on release while
				// IsMouseDoubleClicked inspects the press frame - the
				// two never line up for a row that re-selects itself.
				if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
					static double s_lastClickTime = -1.0;
					static std::size_t s_lastClickRow = static_cast<std::size_t>(-1);
					const double now = ImGui::GetTime();
					if (s_lastClickRow == i && now - s_lastClickTime <= ImGui::GetIO().MouseDoubleClickTime) {
						s_lastClickRow = static_cast<std::size_t>(-1);
						s_lastClickTime = -1.0;
						ConfirmAdd();
						ShopAudio::PlaySfx(ShopAudio::Sfx::kClick);
					} else {
						s_lastClickRow = i;
						s_lastClickTime = now;
					}
				}
				// Hover tick, fired only when the hovered row actually changes.
				// Gating on the row index rather than IsItemHovered() alone keeps
				// this from retriggering every frame the pointer rests on a row.
				{
					static std::size_t s_hoverRow = static_cast<std::size_t>(-1);
					if (ImGui::IsItemHovered()) {
						if (s_hoverRow != i) {
							s_hoverRow = i;
							ShopAudio::PlaySfx(ShopAudio::Sfx::kHover);
						}
					} else if (s_hoverRow == i) {
						s_hoverRow = static_cast<std::size_t>(-1);
					}
				}
					ImGui::TableNextColumn();
					ImGui::TextColored(gold, "%s", RowTypeName(form));
					ImGui::TableNextColumn();
					ImGui::TextColored(gold, "%u", FormValue(form));
					ImGui::TableNextColumn();
					ImGui::TextColored(muted, "%.2f", FormWeight(form));
					ImGui::TableNextColumn();
					ImGui::TextColored(muted, "%08X", form->GetFormID());
					ImGui::PopID();
				}
			}
				ImGui::EndTable();
			}
			if (rebuildAfterTable) {
				selectedFormIndex = 0;
				cursor = page = 0;
				BuildFormList();
			}
			if (search[0]) {
				ImGui::TextColored(muted, "%zu of %zu records match '%s'", shownRows, forms.size(), search);
			}
			ImGui::EndChild();

			if (stacked) {
				// Close the catalog table and its height cap, then start the
				// basket's own row; the basket draws into column 0 of the outer
				// table, full width.
				ImGui::EndTable();
				ImGui::EndChild();
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
			}
			ImGui::TableSetColumnIndex(detailsColumn);
			ImGui::BeginChild("reference_details", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
			auto* form = selectedFormIndex < forms.size() ? forms[selectedFormIndex] : nullptr;
			ImGui::TextColored(accentBright, "PREVIEW");
			ImGui::Separator();
			// Metadata-only viewport. Starfield's native 3D scene cannot be attached
			// to this independent ImGui child through a verified public API.
			// (No wrapping block: drawViewport must stay visible until the
			// lightbox after EndTable, build 117.)
			// Frame-level preview work: request/render state is shared by the
			// compact child and the workspace lightbox below.
			auto* previewForm = form;
			LogPreviewPlacement(previewForm);
			const bool legacyPreview = !LegacyPreviewDisabled();
			const bool nativeDispatched = TryNativeVendorPreview(previewForm);
			// Both plugin-side painters are gated together; see
			// LegacyPreviewDisabled for why one INI key is not enough.
			SFE::ItemCapture::SetShopWantsCapture(legacyPreview && previewForm != nullptr);
			std::vector<std::string> models;
			SFE::OfflineModel::Snapshot offline{};
			bool offlineCurrent = false;
			static SFE::NativeModel::PreviewResult s_native{};
			// Extraction runs in both modes; only the source differs. With the
			// legacy painters gated off the engine's own loaded model is the only
			// source, so the read-back must happen exactly then -- that is the
			// native route, and it is a read of the menu, not a plugin guess.
			if (previewForm) {
				// Game-native model loader: Clone3D the form and extract geometry.
				// Runs once per form change; result kept for the direct-live wire.
				// Pass FormModelPaths so dummy-base weapons can swap in
				// OMOD assembly NIFs (form+0x100 is BullpupDummyReceiver). The
				// gated route needs no paths: the engine already resolved them.
				if (legacyPreview) {
					models = FormModelPaths(previewForm);
				}
				{
					static std::uint32_t s_lastNativeForm = 0;
					static std::uint32_t s_lastNativeChoice = 0;
					static bool s_lastNativeMenu = false;
					const auto fid = previewForm->GetFormID();
					const auto choiceVer = g_choiceVersion.load(std::memory_order_acquire);
					// Opening the menu after the shop must re-read: the same form now
					// has an engine-loaded model where before there was none.
					const bool menuNow = SFE::NativeModel::BarterMenuAvailable();
					if (fid != s_lastNativeForm || choiceVer != s_lastNativeChoice || menuNow != s_lastNativeMenu) {
						s_lastNativeForm = fid;
						s_lastNativeChoice = choiceVer;
						s_lastNativeMenu = menuNow;
						s_native = legacyPreview ? SFE::NativeModel::LoadFromForm(fid, models)
						                         : SFE::NativeModel::LoadFromBarterMenu(fid);
						// Runs once per form change, like the geometry log below, so a
						// dropped part is reported once instead of once per frame.
						PruneSkinnedAttachmentParts(s_native);
						REX::INFO("SFShopExplorer native preview: source={} loaded={} extracted={} nodes={} geo={} tri={} center=({:.1f},{:.1f},{:.1f}) r={:.1f} msg='{}'",
							legacyPreview ? "clone3d" : "barter-readback",
							s_native.loaded, s_native.extracted, s_native.nodeCount, s_native.geoCount, s_native.triCount,
							s_native.center[0], s_native.center[1], s_native.center[2], s_native.radius, s_native.message);
						for (const auto& m : s_native.meshes) {
							float lo[3]{ 1e30f, 1e30f, 1e30f };
							float hi[3]{ -1e30f, -1e30f, -1e30f };
							for (std::size_t p = 0; p + 2 < m.pos.size(); p += 3) {
								for (std::size_t axis = 0; axis < 3; ++axis) {
									lo[axis] = std::min(lo[axis], m.pos[p + axis]);
									hi[axis] = std::max(hi[axis], m.pos[p + axis]);
								}
							}
							REX::INFO("  geo leaf: '{}' (pos={} uv={} idx={}) bounds=({:.3f}..{:.3f}, {:.3f}..{:.3f}, {:.3f}..{:.3f})",
								m.name, m.pos.size(), m.uv.size(), m.indices.size(),
								lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
						}
					}
				}
				const std::string previewLabel = FormPreviewLabel(previewForm);

				const bool isWeapon = previewForm->GetFormType() == RE::FormType::kWEAP;
				// Preview labels identify the base weapon, not a requested
				// purchase tier: quality/effect OMOD picks do not change the
				// model and must not make every hovered copy look Legendary.
				// Direct live: NativeModel Clone3D meshes → independent GPU renderer.
				// BA2 reads below only provide the GPU path's material textures.
				// INI skiplist: drop paths AND native meshes so a skipped form
				// can never render (curated caches included).
				const bool skipped = !models.empty() && SkipModelRequested(previewForm, models);
				// Publish a preview request only when the request actually changes.
				//
				// RequestPreviewDirect is not cheap: it copies every vertex of the
				// item into its own meshes and content-hashes every byte to make
				// the key, and the caller has already copied the geometry once
				// more on the way in. This is DrawImGui, so before this guard all
				// of that ran once per frame while an item merely sat selected,
				// only to reach the identical request and throw the work away.
				// The cost scales with the item's part count and vertex count,
				// which is why a weapon built from many OMOD assembly NIFs was
				// the heaviest thing in the list to select and to scroll past.
				//
				// Everything the request is derived from is in the signature, so
				// none of the real changes are lost: a new form, a new OMOD or
				// rarity pick (g_choiceVersion, which Open() also bumps, so a
				// reopen republishes), the skip decision, an INI toggle behind
				// legacyPreview/authoredOrientation, or the label. The published
				// model is kept in the renderer's own cache and is only replaced
				// when its key changes, so an identical request has nothing to do.
				// Orbit and zoom are unaffected: they live in the renderer and are
				// applied to the frame, not to the request.
				struct DirectRequestSignature
				{
					std::uint32_t form{};
					std::uint32_t choice{};
					bool skipped{};
					bool legacy{};
					bool authoredWanted{};
					bool weapon{};
					// The barter menu appearing re-reads the native geometry from the
					// engine's own loaded model (LoadFromBarterMenu) instead of Clone3D,
					// so the meshes can change with the form and the choice unchanged.
					// Without this in the signature that re-read would never reach the
					// renderer and the viewport would keep the pre-menu geometry.
					bool barterMenu{};
					std::string label;
				};
				static DirectRequestSignature s_lastDirect{};
				static std::uint64_t s_directRequests{ 0 };
				static std::uint64_t s_directSkippedRequests{ 0 };
				const DirectRequestSignature requestNow{
					previewForm->GetFormID(),
					g_choiceVersion.load(std::memory_order_acquire),
					skipped,
					legacyPreview,
					!isWeapon && AuthoredOrientationEnabled(),
					isWeapon,
					SFE::NativeModel::BarterMenuAvailable(),
					previewLabel };
				const bool requestChanged =
					requestNow.form != s_lastDirect.form || requestNow.choice != s_lastDirect.choice ||
					requestNow.skipped != s_lastDirect.skipped || requestNow.legacy != s_lastDirect.legacy ||
					requestNow.authoredWanted != s_lastDirect.authoredWanted ||
					requestNow.weapon != s_lastDirect.weapon || requestNow.barterMenu != s_lastDirect.barterMenu ||
					requestNow.label != s_lastDirect.label;
				if (requestChanged) {
					s_lastDirect = requestNow;
					// Frame-skipped count, so the saving is visible in a log: the
					// difference between these two numbers is what this guard
					// removed. DEBUG, because it is a per-selection diagnostic.
					REX::DEBUG("SFShopExplorer direct request: rebuild {} for '{}' ({} frame(s) reused)",
						s_directRequests, previewLabel, s_directSkippedRequests);
					s_directSkippedRequests = 0;
				} else {
					++s_directSkippedRequests;
				}
				if (requestChanged) {
					++s_directRequests;
					std::vector<SFE::OfflineModel::DirectMesh> direct;
					if (!skipped) {
						direct.reserve(s_native.meshes.size());
						// One pass over the native meshes, here and nowhere else. This block
						// used to appear twice - an identical loop above the book logging -
						// and every item was therefore handed to the renderer twice: the
						// Alien Genetic Material's 6 NIF shapes arrived as 12 ranges /
						// 9440 tris, the Pils can's 7 as 14, the wine's 3 as 6. Doubled
						// geometry also doubled every translucent shell, which is what made
						// the coolant jar read as a dark tub instead of clear glass.
						// Books, notes and dataslates have no cover art in the shipped
						// material database (GenBookA01/B01 sets are flat, B04 has no set
						// at all), so "the cover looks wrong" can only be chased from the
						// material this preview actually asked for. One line per mesh,
						// naming the NIF material and the record swap when one applied.
						const bool bookPreview = previewForm->GetFormType() == RE::FormType::kBOOK;
						static std::uint32_t loggedBookForm{ 0 };
						const bool logBook = bookPreview && loggedBookForm != previewForm->GetFormID();
						std::string bookMaterials;
						if (logBook) {
							loggedBookForm = previewForm->GetFormID();
							bookMaterials = std::format("SFShopExplorer book: {:08X} '{}' model='{}'", previewForm->GetFormID(),
								FormDisplayName(previewForm), FormModelPath(previewForm));
						}
						for (const auto& m : s_native.meshes) {
							if (m.pos.size() < 9 || m.indices.size() < 3) {
								continue;
							}
							SFE::OfflineModel::DirectMesh d;
							d.name = m.name;
							// The item's layered material swap: the same NIF serves the
							// stock garment and the recoloured variant (grey tee vs
							// Resort Wear, blue SpaceTrucker vs Star Roamer), so the
							// NIF's own material name is the wrong one for this item.
							// Empty when the item declares no swap for this material.
							const auto swapped = SFE::LiveModel::MaterialSwapFor(previewForm->GetFormID(), m.matPath);
							d.matPath = swapped.empty() ? m.matPath : swapped;
							if (logBook) {
								bookMaterials += std::format("\n  '{}' nif='{}' used='{}'", m.name, m.matPath, d.matPath);
							}
							d.pos = m.pos;
							d.uv = m.uv;
							d.indices = m.indices;
							direct.push_back(std::move(d));
						}
						if (logBook) {
							REX::INFO("{}", bookMaterials);
						}
					}
					// Authored framing: rotate the geometry by the engine's own
					// per-form preview transform, then tell the renderer not to
					// lay its own guess on top (that would double-orient).
					// Weapons keep the renderer's cyclic orientation. That mapping was
					// confirmed in game (muzzle +X, slide up, wood down) and applies to
					// every weapon, while only 169 of 467 author a preview transform
					// at all -- so authored rotation would regress far more weapons
					// than it improves. The authored transform is for the rest.
					bool authored = false;
					if (!direct.empty() && !isWeapon && AuthoredOrientationEnabled()) {
						const auto placement = ReadAuthoredPlacement(previewForm);
						if (placement.found) {
							float lo[3]{ 1e30f, 1e30f, 1e30f };
							float hi[3]{ -1e30f, -1e30f, -1e30f };
							for (const auto& d : direct) {
								for (std::size_t p = 0; p + 2 < d.pos.size(); p += 3) {
									for (int a = 0; a < 3; ++a) {
										lo[a] = std::min(lo[a], d.pos[p + a]);
										hi[a] = std::max(hi[a], d.pos[p + a]);
									}
								}
							}
							const float center[3]{ (lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f };
							for (auto& d : direct) {
								RotateAboutCenter(d.pos, center, placement.rot);
							}
							authored = true;
							static std::uint32_t s_loggedFraming = 0;
							const std::uint32_t fid = previewForm->GetFormID();
							if (fid != s_loggedFraming) {
								s_loggedFraming = fid;
								REX::INFO("SFShopExplorer authored framing: {:08X} slot={} rot=({:.3f},{:.3f},{:.3f}) scale={:.3f}",
									fid, placement.slot, placement.rot[0], placement.rot[1], placement.rot[2], placement.scale);
							}
						}
					}
					SFE::OfflineModel::RequestPreviewDirect(std::move(direct), previewLabel, isWeapon, authored);
				}
				offline = SFE::OfflineModel::GetSnapshot();
				offlineCurrent = offline.ready && offline.srvHandle != 0 && offline.label == previewLabel;
			}
			if (!form) {
				previewExpanded = false;
				previewPress = false;
			}
			auto drawViewport = [&](const ImVec2& center, const ImVec2& size, const bool a_popout) {
				auto* draw = ImGui::GetWindowDrawList();
				draw->PushClipRect(center, ImVec2(center.x + size.x, center.y + size.y), true);
				if (!form) {
					DrawClippedText(draw, ImVec2(center.x + 8.0f, center.y + 8.0f), size.x - 16.0f,
						IM_COL32(150, 150, 160, 200), "Select an item to render it here.");
					draw->PopClipRect();
					return;
				}
				// The viewport child starts its layout cursor at center, so this
				// hit target is a normal layout item (no cursor teleport needed).
				const ImVec2 hitSize{ std::min(size.x, ImGui::GetContentRegionAvail().x), std::min(size.y, ImGui::GetContentRegionAvail().y) };
				ImGui::InvisibleButton("##preview_orbit", hitSize,
					ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
				const bool viewportHovered = ImGui::IsItemHovered();
				const auto& delta = ImGui::GetIO().MouseDelta;
				// Hovered (not Active): a press that starts on a row and drags
				// into the preview still rotates once it reaches the viewport.
				if (viewportHovered && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
					SFE::OfflineModel::AddOrbit(delta.x * 0.45f, delta.y * 0.45f);
				}
				// Middle-drag zoom remains independent of the game's wheel routing.
				if (viewportHovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
					SFE::OfflineModel::AddZoom(1.0f - delta.y * 0.004f);
				}
				if (viewportHovered) {
					const float wheel = ImGui::GetIO().MouseWheel;
					if (wheel > 0.0f) {
						SFE::OfflineModel::AddZoom(1.12f);
					} else if (wheel < 0.0f) {
						SFE::OfflineModel::AddZoom(1.0f / 1.12f);
					}
					if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
						SFE::OfflineModel::ResetOrbit();
					}
				}
				// Click pops the preview out into its own window; only a short
				// press+release on this hit target counts, dragging keeps
				// rotating. Inside the popped-out window a click must NOT
				// toggle anything back: that would make every rotate-click
				// dismiss the preview. Returning it inline is the click-away
				// handler's job alone.
				if (!a_popout && viewportHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
					previewPress = true;
				}
				if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
					if (previewPress && viewportHovered &&
						ImGui::GetIO().MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < 16.0f) {
						previewExpanded = true;
						// Force a placement on this open only, so the pop-out
						// starts beside the shop but stays where it is dragged.
						popJustOpened = true;
						REX::INFO("SFShopExplorer ui: preview popped out ({}x{}) for '{}'",
							static_cast<int>(size.x), static_cast<int>(size.y),
							form ? FormPreviewLabel(form) : std::string("(none)"));
					}
					previewPress = false;
				}
				draw->AddRectFilled(center, ImVec2(center.x + size.x, center.y + size.y), IM_COL32(10, 10, 16, 160), 6.0f);
				const auto capture = SFE::Overlay::ItemCaptureSnapshot();
				const float lineHeight = ImGui::GetTextLineHeight();
				std::string captureStatus;
				if (legacyPreview && capture.mode != SFE::ItemCapture::Mode::kDisabled &&
					(capture.engineMenuOpen || capture.copiedCount > 0 || capture.candidateCount > 0)) {
					captureStatus = !capture.hookInstalled ? std::string("capture: RTV hook not installed") :
						!capture.engineMenuOpen && capture.copiedCount > 0 ? std::format("capture: frozen scene (cand={}) - open InventoryMenu to refresh", capture.candidateCount) :
						!capture.engineMenuOpen ? std::format("capture: waiting for InventoryMenu (cand={} dup={} bb={})", capture.candidateCount, capture.duplicateRtvs, capture.rejectedSwapChain) :
						capture.copiedCount == 0 ? std::format("capture: menu open, 0 targets bound (cand={} dup={} bb={})", capture.candidateCount, capture.duplicateRtvs, capture.rejectedSwapChain) :
						std::format("capture: {} targets bound, primary slot {}{}", capture.copiedCount, capture.primarySlot,
							capture.fullTexture ? " (grid view)" : "");
				}
				if (offlineCurrent) {
					// The shop's own render: aspect-fitted into the viewport.
					const float texW = offline.width > 0 ? static_cast<float>(offline.width) : 512.0f;					const float texH = offline.height > 0 ? static_cast<float>(offline.height) : 384.0f;
					const float scale = std::min(size.x / texW, size.y / texH);
					const float dw = texW * scale;
					const float dh = texH * scale;
					const ImVec2 p0{ center.x + (size.x - dw) * 0.5f, center.y + (size.y - dh) * 0.5f };
					draw->AddImage(static_cast<ImTextureID>(offline.srvHandle), p0, ImVec2(p0.x + dw, p0.y + dh));
					const ImVec2 topMax{ center.x + size.x, center.y + std::min(size.y, lineHeight * 2.0f + 8.0f) };
					const ImVec2 bottomMin{ center.x, center.y + std::max(0.0f, size.y - lineHeight - 7.0f) };
					draw->AddRectFilled(center, topMax, IM_COL32(5, 8, 12, 220), 4.0f, ImDrawFlags_RoundCornersTop);
					draw->AddRectFilled(bottomMin, ImVec2(center.x + size.x, center.y + size.y), IM_COL32(5, 8, 12, 220), 4.0f, ImDrawFlags_RoundCornersBottom);
					DrawClippedText(draw, ImVec2(center.x + 6.0f, center.y + 4.0f), size.x - 12.0f,
						IM_COL32(245, 245, 250, 255),
						a_popout ? "ESC or click outside to bring it back | drag: rotate | wheel: zoom"
								 : "Click to pop out | drag: rotate | wheel: zoom");
					const std::string orbitTag = std::format("yaw {:+.0f}  pitch {:+.0f}  zoom x{:.2f}",
						SFE::OfflineModel::OrbitYawDeg(), SFE::OfflineModel::OrbitPitchDeg(), SFE::OfflineModel::ZoomFactor());
					DrawClippedText(draw, ImVec2(center.x + 6.0f, center.y + 4.0f + lineHeight + 1.0f), size.x - 12.0f,
						IM_COL32(205, 210, 220, 255), orbitTag);
				} else if (legacyPreview && !offline.enabled) {
						// Runs only when the direct render is allowed but its module is
					// off. With the gate off this must stay silent: the snapshot is
					// default-initialised (enabled=false), so this message would
					// otherwise name [offlinepreview] enabled=0 - a key nobody set.
						DrawClippedText(draw, ImVec2(center.x + 6.0f, center.y + 4.0f), size.x - 12.0f,
							IM_COL32(150, 150, 160, 200), "direct preview disabled (INI enabled=0)");
				}
				if (legacyPreview && !offlineCurrent && capture.copiedCount > 0 && capture.srvHandles[capture.primarySlot] != 0) {
						if (capture.fullTexture) {
							// Diagnostic grid: every bound target's full [0,1] extent
							// with the INI sub-rect outlined on the primary slot. The
							// session's fullscreen HDR candidates are interchangeable
							// to every filter field, so eye-identification picks the
							// winner and INI slot=N pins it (no rebuild needed).
							const float gap = 6.0f;
							const std::uint32_t n = std::min<std::uint32_t>(capture.copiedCount, SFE::ItemCapture::kMaxSlots);
							const float cellW = std::max((size.x - gap * static_cast<float>(n - 1)) / static_cast<float>(n), 32.0f);
							const float cellH = std::max(size.y - ImGui::GetTextLineHeight() - 16.0f, 32.0f);

							for (std::uint32_t s = 0; s < n; ++s) {
								if (capture.srvHandles[s] == 0) {
									continue;
								}
								const ImVec2 cell0{ center.x + static_cast<float>(s) * (cellW + gap), center.y + 16.0f };
								const ImVec2 cell1{ cell0.x + cellW, cell0.y + cellH };
								draw->AddImage(static_cast<ImTextureID>(capture.srvHandles[s]), cell0, cell1);
								const bool primary = s == capture.primarySlot;
								draw->AddRect(cell0, cell1, primary ? IM_COL32(255, 200, 90, 255) : IM_COL32(120, 120, 130, 200), 0.0f, 0, primary ? 2.0f : 1.0f);
								if (primary) {
									const ImVec2 r0{ cell0.x + (cell1.x - cell0.x) * capture.srvUV0[s][0], cell0.y + (cell1.y - cell0.y) * capture.srvUV0[s][1] };
									const ImVec2 r1{ cell0.x + (cell1.x - cell0.x) * capture.srvUV1[s][0], cell0.y + (cell1.y - cell0.y) * capture.srvUV1[s][1] };
									draw->AddRect(r0, r1, IM_COL32(90, 200, 255, 255), 0.0f, 0, 1.5f);
								}
								const std::string tag = std::format("{}:{}x{} f{}", s, capture.srvWidths[s], capture.srvHeights[s], capture.srvFormat[s]);
								DrawClippedText(draw, ImVec2(cell0.x + 2.0f, cell1.y + 1.0f), cellW - 4.0f,
									primary ? IM_COL32(255, 200, 90, 255) : IM_COL32(150, 150, 160, 220), tag);
							}
						} else {
							// Production view: the sub-rect crop of the primary slot.
							const auto s = capture.primarySlot;
							const auto texId = static_cast<ImTextureID>(capture.srvHandles[s]);
							const float texW = capture.srvWidths[s] > 0 ? static_cast<float>(capture.srvWidths[s]) : 2560.0f;
							const float texH = capture.srvHeights[s] > 0 ? static_cast<float>(capture.srvHeights[s]) : 1440.0f;
							const ImVec2 uv0{ capture.srvUV0[s][0], capture.srvUV0[s][1] };
							const ImVec2 uv1{ capture.srvUV1[s][0], capture.srvUV1[s][1] };
							const float rw = std::max((uv1.x - uv0.x) * texW, 1.0f);
							const float rh = std::max((uv1.y - uv0.y) * texH, 1.0f);
							const float scale = std::min(size.x / rw, size.y / rh);
							const float dw = rw * scale;
							const float dh = rh * scale;
							const ImVec2 p0{ center.x + (size.x - dw) * 0.5f, center.y + (size.y - dh) * 0.5f };
							draw->AddImage(texId, p0, ImVec2(p0.x + dw, p0.y + dh), uv0, uv1);
							const std::string captureLabel = std::format("slot {} live item scene {}x{} (sub-rect of {}x{})",
								s, static_cast<int>(rw), static_cast<int>(rh), static_cast<int>(texW), static_cast<int>(texH));
							DrawClippedText(draw, ImVec2(center.x + 6.0f, center.y + 4.0f + ImGui::GetTextLineHeight() * 1.2f),
								size.x - 12.0f,								IM_COL32(150, 150, 160, 200), captureLabel);
						}
				} else if (legacyPreview && !offlineCurrent) {

						const std::string previewStatus = !offline.enabled ? std::string("direct preview disabled") :
							offline.message.empty() ? std::string("building direct GPU preview...") : offline.message;						const std::string titleText = FormPreviewLabel(previewForm);
						const auto titleLabel = FitTextToWidth(titleText, size.x - 16.0f);
						const ImVec2 ts = ImGui::CalcTextSize(titleLabel.c_str());
						DrawClippedText(draw, ImVec2(center.x + (size.x - ts.x) * 0.5f, center.y + (size.y - ts.y) * 0.5f - ImGui::GetTextLineHeight() * 1.4f), size.x - 16.0f, IM_COL32(200, 200, 210, 220), titleLabel);
						const char* label2 = models.empty() ? "(no model path on this form)" : models.front().c_str();
						const auto modelLabel = FitTextToWidth(label2, size.x - 16.0f);
						const ImVec2 ts2 = ImGui::CalcTextSize(modelLabel.c_str());
						DrawClippedText(draw, ImVec2(center.x + (size.x - ts2.x) * 0.5f, center.y + (size.y - ts2.y) * 0.5f + ImGui::GetTextLineHeight() * 0.4f),
							size.x - 16.0f, IM_COL32(150, 150, 160, 200), modelLabel);
						const auto statusLabel = FitTextToWidth(previewStatus, size.x - 16.0f);
						const ImVec2 ts3 = ImGui::CalcTextSize(statusLabel.c_str());
						DrawClippedText(draw, ImVec2(center.x + (size.x - ts3.x) * 0.5f, center.y + (size.y - ts3.y) * 0.5f + ImGui::GetTextLineHeight() * 1.6f), size.x - 16.0f, IM_COL32(255, 200, 90, 220), statusLabel);

						if (!models.empty()) {
							const std::string formModel = FormModelPath(previewForm);
						const auto baseName = [](const std::string& path) {
							const auto slash = path.find_last_of("\\/");
							std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
							const auto dot = base.find_last_of('.');
							if (dot != std::string::npos) base.erase(dot);
							return CaseFold(std::move(base));
						};
						if (formModel.empty() || baseName(offline.model) != baseName(formModel)) {
							const std::string provenance = std::format("Form model: {} | {}", formModel.empty() ? "none" : formModel,
							FormPreviewLabel(previewForm));
							const auto provenanceLabel = FitTextToWidth(provenance, size.x - 16.0f);
							DrawClippedText(draw, ImVec2(center.x + 8.0f, center.y + (size.y + ImGui::GetTextLineHeight()) * 0.5f + ImGui::GetTextLineHeight() * 2.0f),
								size.x - 16.0f, IM_COL32(180, 180, 190, 220), provenanceLabel);
						}
					}
				}
				const float bottomY = center.y + size.y - lineHeight - 6.0f;
				if (offlineCurrent) {
					const float metadataY = captureStatus.empty() ? bottomY : bottomY - lineHeight - 2.0f;
					const ImVec2 metadataMin{ center.x + 4.0f, metadataY };
					const ImVec2 metadataMax{ center.x + size.x - 4.0f, captureStatus.empty() ? center.y + size.y - 2.0f : bottomY };
					draw->AddRectFilled(metadataMin, metadataMax, IM_COL32(5, 8, 12, 215));
					// Same rasterizer either way; the label names the source, because
					// "direct 3D" and "engine model" differ in where the geometry came
					// from and only one of them is a native result.
					DrawClippedText(draw, ImVec2(center.x + 6.0f, metadataY + 1.0f), size.x - 12.0f,
						IM_COL32(140, 235, 150, 255), std::format("{} | {} meshes | {} tris | {}/{} textured",
							legacyPreview ? "direct 3D" : "engine model (barter)",
							offline.meshes, offline.tris, offline.textured, offline.texturedTotal));
				}
				if (!captureStatus.empty()) {
					const ImVec2 statusMin{ center.x + 4.0f, bottomY };
					const ImVec2 statusMax{ center.x + size.x - 4.0f, center.y + size.y - 2.0f };
					draw->AddRectFilled(statusMin, statusMax, IM_COL32(5, 8, 12, 215));
					DrawClippedText(draw, ImVec2(center.x + 6.0f, statusMin.y + 1.0f), size.x - 12.0f,
						IM_COL32(255, 200, 90, 255), captureStatus);
				}
				if (!legacyPreview && !offlineCurrent) {
					// Gate off means: the only image allowed here is geometry the
					// engine itself loaded, drawn by the plugin's rasterizer. When
					// the read-back finds nothing there is no picture at all, and
					// this notice says why rather than letting a stale or
					// plugin-assembled model stand in for it.
					const char* line1 = "NATIVE PREVIEW   ([preview] legacyRenderer=0)";
					const std::string line2 = s_native.message.empty()
						? std::string("No engine-loaded model was read back, so this viewport is empty.")
						: std::format("Engine read-back: {}", s_native.message);
					const std::string line3 = previewForm
						? std::format("native dispatch: {} for {:08X}", nativeDispatched ? "ok" : "FAILED", previewForm->GetFormID())
						: std::string("native dispatch: no form selected");
					const ImVec2 s1 = ImGui::CalcTextSize(line1);
					const ImVec2 s2 = ImGui::CalcTextSize(line2.c_str());
					const ImVec2 s3 = ImGui::CalcTextSize(line3.c_str());
					const float gap = ImGui::GetTextLineHeight() * 0.6f;
					float y = center.y + (size.y - (s1.y + s2.y + s3.y + gap * 2.0f)) * 0.5f;
					DrawClippedText(draw, ImVec2(center.x + std::max((size.x - s1.x) * 0.5f, 4.0f), y), size.x - 8.0f,
						IM_COL32(255, 170, 90, 255), line1);
					y += s1.y + gap;
					DrawClippedText(draw, ImVec2(center.x + std::max((size.x - s2.x) * 0.5f, 4.0f), y), size.x - 8.0f,
						IM_COL32(200, 200, 210, 235), line2.c_str());
					y += s2.y + gap;
					DrawClippedText(draw, ImVec2(center.x + std::max((size.x - s3.x) * 0.5f, 4.0f), y), size.x - 8.0f,
						IM_COL32(140, 235, 150, 255), line3);
				}
				draw->PopClipRect();
			};
			// Collapsed: the viewport renders in the details column. Expanded: it
			// pops out into its own window below (drawViewport owns a fixed
			// ImGui item id, so only one instance may exist per frame) and
			// this slot keeps the strip's height so the panel does not jump.
			{
				const float availY = ImGui::GetContentRegionAvail().y;
				const float viewHeight = std::clamp(availY * 0.34f, 120.0f, 260.0f);
				ImGui::BeginChild("item_render_viewport", ImVec2(0.0f, viewHeight), ImGuiChildFlags_Borders | ImGuiChildFlags_FrameStyle);
				if (!previewExpanded) {
					const ImVec2 viewportPos = ImGui::GetCursorScreenPos();
					const ImVec2 viewportSize = ImGui::GetContentRegionAvail();
					drawViewport(viewportPos, viewportSize, false);
				} else {
					auto* stripDraw = ImGui::GetWindowDrawList();
					const ImVec2 stripMin = ImGui::GetWindowPos();
					const ImVec2 stripMax{ stripMin.x + ImGui::GetWindowSize().x, stripMin.y + ImGui::GetWindowSize().y };
					stripDraw->AddRectFilled(stripMin, stripMax, IM_COL32(10, 10, 16, 160), 6.0f);
					DrawClippedText(stripDraw, ImVec2(stripMin.x + 6.0f, stripMin.y + 6.0f),
						ImGui::GetWindowSize().x - 12.0f, IM_COL32(150, 150, 160, 220),
						"Preview popped out - click away from it to bring it back");
				}
				ImGui::EndChild();
			}
			if (form) {
				const float detailTextWidth = std::max(ImGui::GetContentRegionAvail().x, 0.0f);
				const auto full = FormDisplayName(form);
				const auto* edid = form->GetFormEditorID();
				if (form->GetFormType() == RE::FormType::kWEAP) {
					const auto snap = SFE::OfflineModel::GetSnapshot();
					if (snap.ready && snap.label == FormPreviewLabel(form) && !snap.materials.empty()) {
						// Keep the potentially long material diagnostics tucked
						// away when moving to a different weapon; users can open
						// the header on demand without it obscuring item details.
						static std::string materialHeaderLabel;
						if (materialHeaderLabel != snap.label) {
							ImGui::SetNextItemOpen(false, ImGuiCond_Always);
							materialHeaderLabel = snap.label;
						}
						if (ImGui::CollapsingHeader("Resolved weapon materials")) {
							ImGui::PushTextWrapPos(0.0f);
							const std::size_t shown = std::min<std::size_t>(snap.materials.size(), 48);
							for (std::size_t i = 0; i < shown; ++i) {
								const auto& material = snap.materials[i];
								const std::string heading = std::format("{} | {} | {}", i + 1,
									material.mesh.empty() ? "unnamed mesh" : material.mesh,
									material.colorStatus.empty() ? "no color" : material.colorStatus);
								if (ImGui::TreeNodeEx(heading.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth)) {
									ImGui::TextWrapped("Material: %s", material.material.empty() ? "(none)" : material.material.c_str());
									ImGui::TextWrapped("Color: %s%s%s", material.colorMap.empty() ? "(none)" : material.colorMap.c_str(),
										material.colorMap.empty() ? "" : " | ", material.colorMap.empty() ? "" :
										std::format("{}x{} fmt {}", material.colorWidth, material.colorHeight, material.colorFormat).c_str());
									ImGui::TextWrapped("Opacity: %s%s%s", material.opacityMap.empty() ? "(none)" : material.opacityMap.c_str(),
										material.opacityMap.empty() ? "" : " | ", material.opacityStatus.empty() ? "not reported" : material.opacityStatus.c_str());
									if (!material.opacityMap.empty()) {
										ImGui::Text("Opacity format: %ux%u fmt %u", material.opacityWidth,
											material.opacityHeight, material.opacityFormat);
									}
									ImGui::TreePop();
								}
							}
							if (snap.materials.size() > shown) {
								ImGui::TextColored(muted, "Showing %zu of %zu material records.", shown, snap.materials.size());
							}
							ImGui::PopTextWrapPos();
						}
					}
				}
				const auto displayLabel = FitTextToWidth(FormRowLabel(form), detailTextWidth);
				ImGui::TextColored(ink, "%s", displayLabel.c_str());
				if (HasPlayerVisibleName(form) && edid && edid[0] && full != edid) {
					const auto edidDisplay = FitTextToWidth(std::format("EDID: {}", edid), detailTextWidth);
					ImGui::TextColored(muted, "%s", edidDisplay.c_str());
				}
				ImGui::TextColored(muted, "FormID: %08X", form->GetFormID());
				ImGui::TextColored(muted, "Type: %s", RowTypeName(form));
				const auto pluginDisplay = FitTextToWidth(pluginLabel, detailTextWidth);
				ImGui::TextColored(muted, "Plugin: %s", pluginDisplay.c_str());
				if (const auto group = duplicateGroupOf.find(form->GetFormID()); group != duplicateGroupOf.end()) {
					// Say it plainly: these are separate records with separate FormIDs,
					// so the second "11mm Caseless" is not the list repeating itself.
					ImGui::TextColored(gold, "Defined %zu times", group->second.size());
					ImGui::PushStyleColor(ImGuiCol_Text, muted);
					ImGui::PushTextWrapPos(0.0f);
					const std::size_t listed = std::min<std::size_t>(group->second.size(), 6);
					for (std::size_t i = 0; i < listed; ++i) {
						const auto id = group->second[i];
						const auto* dataHandler = RE::TESDataHandler::GetSingleton();
						const std::string owner = dataHandler ? RuntimeOwnerOfFormID(dataHandler, id) : std::string{};
						ImGui::BulletText("%08X  %s%s", id, owner.empty() ? "(unknown plugin)" : owner.c_str(),
							id == form->GetFormID() ? "  (this row)" : "");
					}
					if (group->second.size() > listed) {
						ImGui::BulletText("and %zu more", group->second.size() - listed);
					}
					ImGui::PopTextWrapPos();
					ImGui::PopStyleColor();
				}
				// Which body part this is: armor/outfit items are one NIF per
				// part (upperbody/lowerbody/sleeves/hat are separate forms),
				// so the filename tells whether a "missing half" is a sibling
				// record rather than a broken preview. OMOD assemblies name
				// the part count instead of one misleading NIF.
				if (FormModelsAssembled(form)) {
					const auto it = g_assemblyCache.find(form->GetFormID());
					const std::size_t n = it != g_assemblyCache.end() ? it->second.size() : 0;
					ImGui::TextColored(muted, "Model: OMOD assembly (%zu parts, EDID fallback)", n);				} else if (const auto modelPath = FormModelPath(form); !modelPath.empty()) {
					const auto modelDisplay = FitTextToWidth(std::format("Model: {}", modelPath), detailTextWidth);
					ImGui::TextColored(muted, "%s", modelDisplay.c_str());
				}
				{
					// Name the actual render source even when the form carries
					// no path (placeholder-model weapons resolve via EDID, so
					// there is no Model: line at all — the snapshot key is
					// the only provenance).
					const auto snap = SFE::OfflineModel::GetSnapshot();
					if (snap.ready && !snap.model.empty()) {
						const auto modelPath = FormModelPath(form);
						const auto baseName = [](const std::string& path) {
							const auto slash = path.find_last_of("\\/");
							std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
							const auto dot = base.find_last_of('.');
							if (dot != std::string::npos) base.erase(dot);
							return CaseFold(std::move(base));
						};
						if (modelPath.empty() || baseName(snap.model) != baseName(modelPath)) {
							const auto renderDisplay = FitTextToWidth(std::format("Render: {}", snap.model), detailTextWidth);
							ImGui::TextColored(gold, "%s", renderDisplay.c_str());
						}
					}
				}
				if (auto* kwForm = KeywordFormOf(form); kwForm && kwForm->GetNumKeywords() > 0) {
					// Star Wars weapons carry 15+ keywords, which pushed the part
					// selectors below the fold. Collapsed by default, once per
					// selected form: SetNextItemOpen(Always) would otherwise
					// re-close the list on every frame and fight the click.
					static std::uint32_t keywordHeaderForm{ 0 };
					if (keywordHeaderForm != form->GetFormID()) {
						ImGui::SetNextItemOpen(false, ImGuiCond_Always);
						keywordHeaderForm = form->GetFormID();
					}
					const std::string keywordHeader = std::format("Keywords ({})", kwForm->GetNumKeywords());
					if (ImGui::CollapsingHeader(keywordHeader.c_str())) {
						ImGui::PushStyleColor(ImGuiCol_Text, muted);
						ImGui::PushTextWrapPos(0.0f);						kwForm->ForEachKeyword([&](RE::BGSKeyword* kw) {
							if (kw && !kw->formEditorID.empty()) {
								ImGui::BulletText("%s", kw->formEditorID.c_str());
							}
							return RE::BSContainer::ForEachResult::kContinue;
						});
						ImGui::PopTextWrapPos();
						ImGui::PopStyleColor();
					}
				}
			} else {
				ImGui::TextColored(muted, "Select an item to inspect it.");
			}
			if (form && form->GetFormType() == RE::FormType::kWEAP) {
				CollectWeaponAssemblyNifs(form, [](const char*) {});
				WeaponCatalog cat;
				WeaponChoice choice;
				bool haveCat = false;
				{
					std::scoped_lock choiceLock(g_assemblyMutex);
					if (const auto it = g_assemblyCatalogs.find(form->GetFormID()); it != g_assemblyCatalogs.end()) {
						cat = it->second;
						haveCat = true;
					}
					if (const auto it = g_weaponChoice.find(form->GetFormID()); it != g_weaponChoice.end()) {
						choice = it->second;
					}
				}
				// Show catalogued mod choices even for weapons with a real base
				// NIF; the old assembled-only gate hid their per-weapon options.
				const bool haveParts = haveCat && !cat.slotOrder.empty();
				const bool haveQualities = haveCat && !cat.qualities.empty();
				const bool haveEffects = haveCat && !cat.effects.empty();
				if (haveParts || haveQualities || haveEffects) {
					ImGui::TextColored(accentBright, "PARTS");
					if (haveParts) {
						for (const auto& slot : cat.slotOrder) {
							const auto sit = cat.slots.find(slot);
							if (sit == cat.slots.end() || sit->second.empty()) {
								continue;
							}
							const auto& opts = sit->second;
							std::uint32_t cur = 0;
							if (const auto cit = choice.slotFid.find(slot); cit != choice.slotFid.end()) {
								cur = cit->second;
							} else if (const auto dit = cat.defaultFid.find(slot); dit != cat.defaultFid.end()) {
								cur = dit->second;
							}
							const auto* typedWeapon = form->As<RE::TESObjectWEAP>();
							const char* weaponModel = typedWeapon ? typedWeapon->BGSModelMaterialSwap::model.c_str() : nullptr;
							const std::string foldedModel = weaponModel ? CaseFold(weaponModel) : std::string{};
							const bool dummyBase = !foldedModel.empty() && foldedModel.find("dummy") != std::string::npos;
							const bool meleeDummy = dummyBase && foldedModel.find("meleedummyhousing") != std::string::npos;
							if ((slot == "receiver" || (dummyBase && IsMandatoryDummyAssemblySlot(slot, meleeDummy))) && cur == 0) {
								if (const auto dit = cat.defaultFid.find(slot); dit != cat.defaultFid.end()) {
									cur = dit->second;
								}
							}
							const SlotOption* curOpt = nullptr;
							for (const auto& o : opts) {
								if (o.fid == cur) {
									curOpt = &o;
									break;
								}
							}
							std::string upper = slot == "mag" ? "MAGAZINE" : (slot == "scope" ? "OPTIC / SIGHT" : slot);
							for (auto& ch : upper) {
								ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
							}
							ImGui::TextColored(muted, "%s", upper.c_str());
							ImGui::SameLine();
							ImGui::SetNextItemWidth(-1.0f);
							ImGui::PushID(slot.c_str());
							const char* curLabel = curOpt ? curOpt->label.c_str() : "Stock / no extra part";
							if (ImGui::BeginCombo("##slot", curLabel)) {
								const auto dit = cat.defaultFid.find(slot);
								if (slot != "receiver" && !(dummyBase && IsMandatoryDummyAssemblySlot(slot, meleeDummy))) {
									std::string noneLabel = "Stock / no extra part";
									if (dit != cat.defaultFid.end() && dit->second == 0) {
										noneLabel += " (default)";
									}
									const bool noneSelected = cur == 0;
									if (ImGui::Selectable(noneLabel.c_str(), noneSelected) && !noneSelected) {
										SetWeaponSlotChoice(form->GetFormID(), slot, 0);
									}
									if (noneSelected) {
									ImGui::SetItemDefaultFocus();
									}
								}
								for (const auto& opt : opts) {
									std::string text = opt.label;
									if (dit != cat.defaultFid.end() && dit->second == opt.fid) {
										text += " (default)";
									}
									const bool selected = opt.fid == cur;
									if (ImGui::Selectable(text.c_str(), selected)) {
										if (!selected) {
											SetWeaponSlotChoice(form->GetFormID(), slot, opt.fid);
										}
									}
									if (selected) {
										ImGui::SetItemDefaultFocus();
									}
								}
								ImGui::EndCombo();
							}
							ImGui::PopID();
						}
					}
					if (haveQualities) {
						std::string curLabel = "Standard";
						if (choice.qualityFid != 0) {
							curLabel = "(unknown quality)";
							for (const auto& quality : cat.qualities) {
								if (quality.fid == choice.qualityFid) {
									curLabel = quality.label;
									break;
								}
							}
						}
						ImGui::TextColored(muted, "QUALITY");
						ImGui::SameLine();
						ImGui::SetNextItemWidth(-1.0f);
						ImGui::PushID("quality");
						if (ImGui::BeginCombo("##quality", curLabel.c_str())) {
							if (ImGui::Selectable("Standard", choice.qualityFid == 0) && choice.qualityFid != 0) {
								SetWeaponQualityChoice(form->GetFormID(), 0);
							}
							if (choice.qualityFid == 0) ImGui::SetItemDefaultFocus();
							for (const auto& quality : cat.qualities) {
								const bool selected = quality.fid == choice.qualityFid;
								if (ImGui::Selectable(quality.label.c_str(), selected) && !selected) {
									SetWeaponQualityChoice(form->GetFormID(), quality.fid);
								}
								if (selected) ImGui::SetItemDefaultFocus();
							}
							ImGui::EndCombo();
						}
						ImGui::PopID();
					}
					if (haveEffects) {
						std::uint32_t rarityFid = choice.rarityFid;
						const char* rarityLabel = "Common (no effects)";
						if (rarityFid == kRaritySentinelRare) rarityLabel = "Rare (1 effect)";
						else if (rarityFid == kRaritySentinelEpic) rarityLabel = "Epic (2 effects)";
						else if (rarityFid == kRaritySentinelLegendary) rarityLabel = "Legendary (3 effects)";
						ImGui::TextColored(muted, "RARITY / EFFECTS");
						ImGui::SameLine();
						ImGui::SetNextItemWidth(-1.0f);
						ImGui::PushID("weapon-rarity");
						if (ImGui::BeginCombo("##rarity", rarityLabel)) {
							struct RarityPick { const char* label; std::uint32_t fid; };
							static constexpr RarityPick kRarities[] = {
								{ "Common (no effects)", 0 }, { "Rare (1 effect)", kRaritySentinelRare },
								{ "Epic (2 effects)", kRaritySentinelEpic }, { "Legendary (3 effects)", kRaritySentinelLegendary }
							};
							for (const auto& rarity : kRarities) {
								const bool selected = rarity.fid == choice.rarityFid;
								if (ImGui::Selectable(rarity.label, selected) && !selected) {
									SetWeaponRarityChoice(form->GetFormID(), rarity.fid);
								}
								if (selected) ImGui::SetItemDefaultFocus();
							}
							ImGui::EndCombo();
						}
						ImGui::PopID();
						ImGui::TextColored(muted, "%zu legendary effects in this weapon's pool", cat.effects.size());
					}
				}
			} else if (form && form->GetFormType() == RE::FormType::kARMO) {
				// Armor has no assembly catalog, so its pool comes from the
				// generated per-class table. Name the rolled effects: unlike a
				// weapon's pool size, which effect an armor piece got is the
				// whole point of buying it at a rarity.
				const auto slot = ArmorLegendarySlot(form);
				const auto& pool = ArmorLegendaryPool(slot);
				if (!pool.empty()) {
					ArmorRarityChoice choice;
					{
						std::scoped_lock choiceLock(g_assemblyMutex);
						if (const auto it = g_armorRarity.find(form->GetFormID()); it != g_armorRarity.end()) {
							choice = it->second;
						}
					}
					const char* rarityLabel = "Common (no effects)";
					if (choice.rarityFid == kRaritySentinelRare) rarityLabel = "Rare (1 effect)";
					else if (choice.rarityFid == kRaritySentinelEpic) rarityLabel = "Epic (2 effects)";
					else if (choice.rarityFid == kRaritySentinelLegendary) rarityLabel = "Legendary (3 effects)";
					ImGui::TextColored(muted, "RARITY / EFFECTS");
					ImGui::SameLine();
					ImGui::SetNextItemWidth(-1.0f);
					ImGui::PushID("armor-rarity");
					if (ImGui::BeginCombo("##rarity", rarityLabel)) {
						struct RarityPick { const char* label; std::uint32_t fid; };
						static constexpr RarityPick kRarities[] = {
							{ "Common (no effects)", 0 }, { "Rare (1 effect)", kRaritySentinelRare },
							{ "Epic (2 effects)", kRaritySentinelEpic }, { "Legendary (3 effects)", kRaritySentinelLegendary }
						};
						for (const auto& rarity : kRarities) {
							const bool selected = rarity.fid == choice.rarityFid;
							if (ImGui::Selectable(rarity.label, selected) && !selected) {
								SetArmorRarityChoice(form->GetFormID(), rarity.fid, slot);
							}
							if (selected) ImGui::SetItemDefaultFocus();
						}
						ImGui::EndCombo();
					}
					ImGui::PopID();
					ImGui::TextColored(muted, "%zu legendary effects in this %s pool", pool.size(),
						slot == 1u ? "helmet" : (slot == 2u ? "backpack" : "armor"));
					if (!choice.roll.empty()) {
						std::string names;
						for (const auto fid : choice.roll) {
							auto* effect = RE::TESForm::LookupByID(fid);
							if (!effect) {
								continue;
							}
							if (!names.empty()) {
								names += ", ";
							}
							names += FormDisplayName(effect);
						}
						if (!names.empty()) {
							ImGui::TextWrapped("Rolled: %s", names.c_str());
						}
					}
				}
			}
			ImGui::TextColored(gold, "TRANSFER");
			const auto unitPrice = form ? FormValue(form) : 0;
			ImGui::TextWrapped("%s transfer — price: %u credits/unit", freeTransfer ? "Free" : "Paid", unitPrice);
			ImGui::TextColored(muted, "Quantity");
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::InputInt("##shop_qty", &addCount, 1, 10);
			addCount = std::clamp(addCount, 1, 999);
			if (form && unitPrice > 0 && !freeTransfer) {
				ImGui::TextColored(gold, "Total: %u credits", unitPrice * static_cast<std::uint32_t>(addCount));
			}
			if (form && ImGui::Button("ADD TO BASKET", ImVec2(-1.0f, 30.0f))) {
				// Capture the same resolved picks as the row-double-click
				// ConfirmAdd route, keeping render-thread basket changes local.
				const auto count = std::clamp(addCount, 1, 999);
				const auto formID = form->GetFormID();
				if (form->GetFormType() == RE::FormType::kWEAP) {
					CollectWeaponAssemblyNifs(form, [](const char*) {});
				}
				auto selectedOmods = SelectedOmodsFor(form);
				bool merged = false;
				for (auto& entry : basket) {
					if (entry.formID == formID && entry.selectedOmods == selectedOmods) {
						entry.count = std::clamp(entry.count + count, 1, 999);
						merged = true;
						break;
					}
				}
				if (!merged) {
					const auto* edid = form->GetFormEditorID();
					basket.push_back(BasketEntry{ formID, edid ? edid : "", count, std::move(selectedOmods) });
				}
				ShopAudio::PlaySfx(ShopAudio::Sfx::kSelect);
			}
			ImGui::Separator();
			ImGui::TextColored(accentBright, "BASKET (%zu)", basket.size());
			if (!basket.empty()) {
				std::uint32_t basketTotal = 0;
				double basketWeight = 0.0;
				for (std::size_t i = 0; i < basket.size(); ++i) {
					auto& entry = basket[i];
					const auto* entryForm = RE::TESForm::LookupByID(entry.formID);
					const auto price = entryForm ? FormValue(entryForm) * static_cast<std::uint32_t>(entry.count) : 0;
					// The basket used to report credits only, so a load-out that
					// would not fit in the player's cargo looked free.
					const auto lineWeight = entryForm ? static_cast<double>(FormWeight(entryForm)) * entry.count : 0.0;
					basketTotal += price;
					basketWeight += lineWeight;
					std::string line = std::format("{} x{}", entry.edid.empty() ? std::format("{:08X}", entry.formID) : entry.edid, entry.count);
					if (price > 0) {
						line += std::format(" — {} cr", price);
					}
					if (lineWeight > 0.0) {
						line += std::format(" — {:.2f} wt", lineWeight);
					}
					ImGui::PushID(static_cast<int>(i));
					ImGui::TextWrapped("%s", line.c_str());
					if (ImGui::SmallButton("-")) {
						entry.count = std::max(1, entry.count - 1);
						ShopAudio::PlaySfx(ShopAudio::Sfx::kClick);
					}
					ImGui::SameLine();
					if (ImGui::SmallButton("+")) {
						entry.count = std::min(999, entry.count + 1);
						ShopAudio::PlaySfx(ShopAudio::Sfx::kClick);
					}
					ImGui::SameLine();
					if (ImGui::SmallButton("x")) {
						basket.erase(basket.begin() + static_cast<std::ptrdiff_t>(i));
						// Its own sound: taking a line out is not the same act as
						// adding one, and every other basket button already clicked.
						ShopAudio::PlaySfx(ShopAudio::Sfx::kRemove);
						ImGui::PopID();
						break;
					}
					ImGui::PopID();
				}
				if (basketTotal > 0 && !freeTransfer) {
					ImGui::TextColored(gold, "Total: %u credits  ·  %.2f wt", basketTotal, basketWeight);
				} else if (basketWeight > 0.0) {
					ImGui::TextColored(gold, "Total: %.2f wt", basketWeight);
				}
				if (ImGui::Button("ACCEPT", ImVec2(-1.0f, 30.0f))) {
					std::scoped_lock actionLock(actionMutex);
					pendingFree = freeTransfer;
					pendingAccept = true;
					ShopAudio::PlaySfx(ShopAudio::Sfx::kSelect);
				}
				if (ImGui::Button("CANCEL", ImVec2(-1.0f, 30.0f))) {
					basket.clear();
					ShopAudio::PlaySfx(ShopAudio::Sfx::kRemove);
				}
			} else {
				ImGui::TextColored(muted, "Basket is empty.");
			}
			ImGui::Separator();
			const auto chestArt = Overlay::ThemeChestTexture(th.suffix);
			if (chestArt != static_cast<ImTextureID>(0)) {
				ImGui::TextColored(gold, "SHOP STORAGE");
				const float chestSize = std::min(160.0f, ImGui::GetContentRegionAvail().x);
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - chestSize) * 0.5f);
				ImGui::ImageWithBg(chestArt, ImVec2(chestSize, chestSize), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), ImVec4(0.0f, 0.0f, 0.0f, 0.0f), ImVec4(1.0f, 1.0f, 1.0f, 0.38f));
			}
			ImGui::EndChild();
			ImGui::EndTable();
			// Popped-out preview: clicking the inline viewport opens this
			// window over the catalog, and clicking anywhere outside it puts
			// the preview back where it came from (the inline strip above).
			// Sized from the display, not the fullscreen shop window.
			// ESC dismisses the popped-out preview before any other use of the key.
			// Without this the pop-out had no keyboard exit at all: ESC fell through
			// to the page-back handler below, so the preview stayed on screen while
			// the catalog or shop changed underneath it, and the next ESC closed the
			// shop - which is what left the session with a search box that took no
			// characters. One press, one visible change, and the press is consumed
			// so it cannot also page back.
			if (previewExpanded && escapePressed && !ImGui::GetIO().WantTextInput) {
				previewExpanded = false;
				previewPress = false;
				escapeConsumed = true;
				REX::INFO("SFShopExplorer ui: preview returned inline (escape)");
			}
			if (previewExpanded) {
				const ImGuiViewport* vp = ImGui::GetMainViewport();
				const float popW = std::clamp(vp->WorkSize.x * 0.34f, 420.0f, 720.0f);
				const float popH = std::clamp(vp->WorkSize.y * 0.62f, 420.0f, 820.0f);
				const ImVec2 anchor = ImGui::GetWindowPos();
				// Movable: the position is only forced on the frame the window
				// first appears (popJustOpened). Forcing it every frame with
				// ImGuiCond_Always is what made dragging impossible - ImGui
				// would apply the drag, then immediately overwrite it.
				//
				// Once it exists, position and size are left alone so the user
				// can drag and resize it, and ImGui's own ini persistence keeps
				// it there between openings. That also means NoSavedSettings
				// has to come off, otherwise nothing is remembered.
				if (popJustOpened) {
					ImGui::SetNextWindowPos(
						ImVec2(anchor.x + 40.0f * Overlay::UiScale(), anchor.y + 80.0f * Overlay::UiScale()),
						ImGuiCond_Always);
					ImGui::SetNextWindowSize(ImVec2(popW, popH), ImGuiCond_Always);
					popJustOpened = false;
				}
				// From here on, no SetNextWindowPos/Size at all: ImGui keeps the
				// dragged geometry and persists it through its own ini.
				ImGui::SetNextWindowBgAlpha(0.97f);
				const ImGuiWindowFlags popFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoNav
					| ImGuiWindowFlags_NoScrollWithMouse;
				// Titled so it reads as its own window (and can be dragged),
				// but the "##" suffix keeps the ImGui id stable at
				// "preview_popout" regardless of the visible text.
				if (ImGui::Begin("Preview##preview_popout", nullptr, popFlags)) {
					const ImVec2 winPos = ImGui::GetWindowPos();
					const ImVec2 winSize = ImGui::GetWindowSize();
					const ImVec2 contentSize = ImGui::GetContentRegionAvail();
					const ImVec2 contentMin = ImGui::GetCursorScreenPos();
					drawViewport(contentMin, contentSize, true);
					ImGui::End();
					// Click away from the pop-out -> return it inline.
					const ImVec2 mouse = ImGui::GetIO().MousePos;
					const bool outside = mouse.x < winPos.x || mouse.y < winPos.y ||
						mouse.x > winPos.x + winSize.x || mouse.y > winPos.y + winSize.y;
					if (outside && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
						previewExpanded = false;
						previewPress = false;
						REX::INFO("SFShopExplorer ui: preview returned inline (click away)");
					}
				} else {
					ImGui::End();
				}
			}
		}
		// Closes the PushID that gives each arrangement its own table state.
		// Placed here, after the workspace block, because the pop-out preview
		// lives inside it and captures drawViewport from that scope.
		ImGui::PopID();

		// ESC leaves the search box first. The field deactivates itself on the
		// same key press, so closing the shop too made typing-then-ESC exit by
		// accident; the next ESC (with no text field focused) goes back one page
		// and, from the plugin page, closes. A press already spent dismissing the
		// preview pop-out is not spent twice here.
		if (escapePressed && !escapeConsumed && !ImGui::GetIO().WantTextInput) {
			std::scoped_lock actionLock(actionMutex);
			pendingBack = true;
		}
		if (bodyFontPushed) {
			ImGui::PopFont();
		}
		ImGui::End();
		ImGui::PopStyleColor(18);
		ImGui::PopStyleVar(2);
	}
}
namespace SFE
{
	// -----------------------------------------------------------------------
	// OSF front-end adapter.
	//
	// Every decision below is delegated: the classifiers, the hide toggles,
	// the basket merge and the delivery path are the overlay's own functions.
	// What is new is only the shape of the answer - rows and lines the page can
	// read - so the two front ends cannot disagree about what the catalog holds.
	// -----------------------------------------------------------------------
	namespace
	{
		template <class Enum>
		Enum ClampSub(const int a_index)
		{
			const int total = static_cast<int>(Enum::kTotal);
			return static_cast<Enum>(std::clamp(a_index, 0, total - 1));
		}

		std::string PluginKindOf(const std::string& a_name)
		{
			const std::string lower = CaseFold(a_name);
			if (lower.size() >= 4) {
				if (lower.compare(lower.size() - 4, 4, ".esl") == 0) {
					return "ESL";
				}
				if (lower.compare(lower.size() - 4, 4, ".esp") == 0) {
					return "ESP";
				}
			}
			return "ESM";
		}

		// The bucket a row sorts into, in the page's own words. Uses the same
		// classifier the sub-filter uses, so the label on a row and the filter
		// that keeps it can never point at different buckets.
		std::string OsfSubFor(const RE::TESForm* a_form)
		{
			if (!a_form) {
				return {};
			}
			switch (a_form->GetFormType()) {
			case RE::FormType::kWEAP: return WeaponSubName(ClassifyWeaponSub(CategorySearchText(a_form)));
			case RE::FormType::kARMO: return ArmorSubName(ClassifyArmorSub(CategorySearchText(a_form)));
			case RE::FormType::kAMMO: return AmmoSubName(ClassifyAmmoSub(CategorySearchText(a_form)));
			case RE::FormType::kMISC: return MiscSubName(ClassifyMiscSub(CategorySearchText(a_form), CategoryModelText(a_form)));
			case RE::FormType::kALCH: return AidSubName(ClassifyAidSub(CategorySearchText(a_form), CategoryModelText(a_form)));
			case RE::FormType::kBOOK: return BookSubName(ClassifyBookSub(CategorySearchText(a_form), FormModelPath(a_form)));
			case RE::FormType::kKEYM: return KeySubName(ClassifyKeySub(CategorySearchText(a_form)));
			default: return "Other";
			}
		}

		std::string OsfOwnerOf(const RE::TESForm* a_form)
		{
			auto* dataHandler = RE::TESDataHandler::GetSingleton();
			if (!dataHandler || !a_form) {
				return {};
			}
			return RuntimeOwnerOfFormID(dataHandler, a_form->GetFormID());
		}

		std::string OsfRecordCode(const RE::TESForm* a_form)
		{
			if (!a_form) {
				return {};
			}
			switch (a_form->GetFormType()) {
			case RE::FormType::kWEAP: return "WEAP";
			case RE::FormType::kARMO: return "ARMO";
			case RE::FormType::kAMMO: return "AMMO";
			case RE::FormType::kMISC: return "MISC";
			case RE::FormType::kALCH: return "ALCH";
			case RE::FormType::kBOOK: return "BOOK";
			case RE::FormType::kKEYM: return "KEYM";
			case RE::FormType::kINGR: return "INGR";
			default: return "FORM";
			}
		}

		OsfItemRow OsfRowFor(const RE::TESForm* a_form)
		{
			OsfItemRow row;
			if (!a_form) {
				return row;
			}
			row.formID = a_form->GetFormID();
			row.name = FormDisplayName(a_form);
			if (const char* edid = a_form->GetFormEditorID()) {
				row.edid = edid;
			}
			row.type = RowTypeName(a_form);
			row.sub = OsfSubFor(a_form);
			row.plugin = OsfOwnerOf(a_form);
			row.value = FormValue(a_form);
			row.weight = FormWeight(a_form);
			return row;
		}
	}

	void ShopBrowser::OsfViewShown()
	{
		// One press, one session: a state replay or a second shown report must
		// not restart the music or count another visit.
		if (open.exchange(true, std::memory_order_acq_rel)) {
			return;
		}
		OsfBeginSession();
		// OSF presents this view as a menu (manifest pausesGame), so the pause
		// counter is already OSF's to hold and release. Only the game's audio
		// session is muted here, because the shop mixes its own: that is a
		// single call to undo, and it is undone the moment the view hides.
		ShopAudio::SetGameSessionMuted(true);
		ShopAudio::PlaySfx(ShopAudio::Sfx::kOpen);
		ApplyAudioSettings();
		ShopAudio::SetMusicFile(MusicTrackFile().c_str());
		if (musicMuted) {
			ShopAudio::StopMusic();
		} else {
			ShopAudio::StartMusic();
		}
		Notify("opened");
	}

	void ShopBrowser::OsfViewHidden()
	{
		if (!open.exchange(false, std::memory_order_acq_rel)) {
			return;
		}
		osfSessionBegun = false;
		// Same teardown as Close(), minus the pause release the standalone
		// overlay owns: the close sting, the music fade and the game's audio
		// session back on.
		ShopAudio::PlaySfx(ShopAudio::Sfx::kClose);
		ShopAudio::StopMusic();
		ShopAudio::SetGameSessionMuted(false);
		Notify("closed");
	}

	void ShopBrowser::OsfBeginSession()
	{
		std::scoped_lock lock(stateMutex);
		if (osfSessionBegun) {
			return;  // catalog request and shown report both land here
		}
		osfSessionBegun = true;
		BuildPluginList();
		LoadAudioSettings();
		// The page can be the first thing to open the shop, so the star list is
		// read here as well as in Open().
		LoadFavourites();
		favouritesOnly = false;
		allPlugins = true;
		selectedPlugin = pluginFiles.empty() ? -1 : 0;
		// Same opening state as Open(): the whole catalog, every type.
		selectedType = BrowseType::kAllTypes;
		selectedArmorSub = ArmorSub::kAll;
		selectedWeaponSub = WeaponSub::kAll;
		selectedAmmoSub = AmmoSub::kAll;
		selectedMiscSub = MiscSub::kAll;
		selectedAidSub = AidSub::kAll;
		selectedBookSub = BookSub::kAll;
		selectedKeySub = KeySub::kAll;
		selectedFormIndex = 0;
		freeTransfer = true;
		basket.clear();
		walletCredits.store(PlayerCredits(RE::PlayerCharacter::GetSingleton()), std::memory_order_release);
		BuildFormList();
		osfSearchDirty = true;
		// Trip count for the Starfield theme: one per open. This used to be
		// throttled to one per five minutes, on the belief that a catalog
		// request might arrive more often than a visit - but OsfBeginSession
		// is only reachable through OsfViewShown, which is guarded by the same
		// open.exchange(true) transition test that stops a replayed state from
		// restarting the music. So one open reaches here exactly once, and the
		// throttle could only ever undercount: open the shop six times in five
		// minutes and the theme read "1 / 8 opens" no matter what the player
		// did.
		AddProgress(Unlock::kOpens, 1);
		REX::INFO("SFShopExplorer OSF: session opened, {} plugins / {} records", pluginFiles.size(), forms.size());
	}

	void ShopBrowser::OsfRefresh()
	{
		std::scoped_lock lock(stateMutex);
		// A refresh is a full reset, and the bridge resets its own filter with it:
		// leaving the starred view on would rebuild the catalog from the favourite
		// ids while the page believed it had asked for the ordinary list.
		favouritesOnly = false;
		BuildPluginList();
		walletCredits.store(PlayerCredits(RE::PlayerCharacter::GetSingleton()), std::memory_order_release);
		BuildFormList();
		osfSearchDirty = true;
		REX::INFO("SFShopExplorer OSF: refreshed, {} plugins / {} records", pluginFiles.size(), forms.size());
	}

	std::vector<OsfCategory> ShopBrowser::OsfCategoryTree()
	{
		std::vector<OsfCategory> tree;
		auto add = [&](const BrowseType a_type) {
			OsfCategory cat;
			cat.index = static_cast<int>(a_type);
			cat.name = BrowseTypeName(a_type);
			switch (a_type) {
			case BrowseType::kWeapon:
				for (int i = 0; i < static_cast<int>(WeaponSub::kTotal); ++i) {
					cat.subs.emplace_back(WeaponSubName(static_cast<WeaponSub>(i)));
				}
				break;
			case BrowseType::kArmor:
				for (int i = 0; i < static_cast<int>(ArmorSub::kTotal); ++i) {
					cat.subs.emplace_back(ArmorSubName(static_cast<ArmorSub>(i)));
				}
				break;
			case BrowseType::kAmmo:
				for (int i = 0; i < static_cast<int>(AmmoSub::kTotal); ++i) {
					cat.subs.emplace_back(AmmoSubName(static_cast<AmmoSub>(i)));
				}
				break;
			case BrowseType::kMisc:
				for (int i = 0; i < static_cast<int>(MiscSub::kTotal); ++i) {
					cat.subs.emplace_back(MiscSubName(static_cast<MiscSub>(i)));
				}
				break;
			case BrowseType::kAid:
				for (int i = 0; i < static_cast<int>(AidSub::kTotal); ++i) {
					cat.subs.emplace_back(AidSubName(static_cast<AidSub>(i)));
				}
				break;
			case BrowseType::kBook:
				for (int i = 0; i < static_cast<int>(BookSub::kTotal); ++i) {
					cat.subs.emplace_back(BookSubName(static_cast<BookSub>(i)));
				}
				break;
			case BrowseType::kKey:
				for (int i = 0; i < static_cast<int>(KeySub::kTotal); ++i) {
					cat.subs.emplace_back(KeySubName(static_cast<KeySub>(i)));
				}
				break;
			default:
				cat.subs.emplace_back(BrowseTypeName(BrowseType::kAllTypes));
				break;
			}
			tree.push_back(std::move(cat));
		};
		for (const auto type : { BrowseType::kWeapon, BrowseType::kArmor, BrowseType::kAmmo, BrowseType::kMisc,
				 BrowseType::kAid, BrowseType::kBook, BrowseType::kKey, BrowseType::kAllTypes }) {
			add(type);
		}
		return tree;
	}

	std::vector<OsfPluginRow> ShopBrowser::OsfPlugins()
	{
		std::scoped_lock lock(stateMutex);
		if (pluginFiles.empty()) {
			BuildPluginList();
		}
		std::vector<OsfPluginRow> rows;
		rows.reserve(pluginNames.size());
		for (std::size_t i = 0; i < pluginNames.size(); ++i) {
			OsfPluginRow row;
			row.name = pluginNames[i];
			row.kind = PluginKindOf(pluginNames[i]);
			rows.push_back(std::move(row));
		}
		return rows;
	}

	void ShopBrowser::OsfSetFilter(const int a_type, const int a_sub, const int a_plugin, const bool a_hideTest,
		const bool a_hideHair, const bool a_hideCrew, const bool a_hideTurrets, const bool a_hideBug,
		const bool a_favouritesOnly, const bool a_hideDuplicates)
	{
		std::scoped_lock lock(stateMutex);
		if (pluginFiles.empty()) {
			BuildPluginList();
		}
		// The page's Favourites toggle is the overlay's: BuildFormList already
		// knows how to list the starred ids across every plugin and category, so
		// the page sets the same flag instead of filtering its own page of rows.
		favouritesOnly = a_favouritesOnly;
		const int typeIndex = std::clamp(a_type, 0, static_cast<int>(BrowseType::kTotal) - 1);
		selectedType = static_cast<BrowseType>(typeIndex);
		allPlugins = a_plugin < 0 || a_plugin >= static_cast<int>(pluginFiles.size());
		if (!allPlugins) {
			selectedPlugin = a_plugin;
		} else if (pluginFiles.empty()) {
			selectedPlugin = -1;
		}
		hideTestDebug = a_hideTest;
		hideShipHair = a_hideHair;
		hideCrewEnemyCreature = a_hideCrew;
		hideTurrets = a_hideTurrets;
		hideReportBug = a_hideBug;
		hideDuplicateNames = a_hideDuplicates;
		// The page has one sub selector for every category, and the index means
		// the same thing the overlay's rail means: a value into that category's
		// own bucket list. All Types applies it per record type, exactly as
		// BuildFormList already does for the overlay.
		if (selectedType == BrowseType::kAllTypes) {
			selectedArmorSub = ClampSub<ArmorSub>(a_sub);
			selectedWeaponSub = ClampSub<WeaponSub>(a_sub);
			selectedAmmoSub = ClampSub<AmmoSub>(a_sub);
			selectedMiscSub = ClampSub<MiscSub>(a_sub);
			selectedAidSub = ClampSub<AidSub>(a_sub);
			selectedBookSub = ClampSub<BookSub>(a_sub);
			selectedKeySub = ClampSub<KeySub>(a_sub);
		} else {
			switch (selectedType) {
			case BrowseType::kWeapon: selectedWeaponSub = ClampSub<WeaponSub>(a_sub); break;
			case BrowseType::kArmor: selectedArmorSub = ClampSub<ArmorSub>(a_sub); break;
			case BrowseType::kAmmo: selectedAmmoSub = ClampSub<AmmoSub>(a_sub); break;
			case BrowseType::kMisc: selectedMiscSub = ClampSub<MiscSub>(a_sub); break;
			case BrowseType::kAid: selectedAidSub = ClampSub<AidSub>(a_sub); break;
			case BrowseType::kBook: selectedBookSub = ClampSub<BookSub>(a_sub); break;
			case BrowseType::kKey: selectedKeySub = ClampSub<KeySub>(a_sub); break;
			default: break;
			}
		}
		selectedFormIndex = 0;
		page = 0;
		BuildFormList();
		osfSearchDirty = true;
	}

	std::size_t ShopBrowser::OsfPage(const std::string& a_search, const std::size_t a_offset,
		const std::size_t a_limit, const int a_sortColumn, const bool a_sortAscending,
		std::vector<OsfItemRow>& a_out)
	{
		std::scoped_lock lock(stateMutex);
		a_out.clear();
		if (forms.size() != osfSearchKeys.size()) {
			osfSearchDirty = true;
		}
		const std::string needle = CaseFold(a_search);
		if (!needle.empty() && osfSearchDirty) {
			// Names, editor IDs, model file stems and FormIDs - the four things
			// the overlay's search covers, folded once instead of per keystroke.
			osfSearchKeys.clear();
			osfSearchKeys.reserve(forms.size());
			for (const auto* form : forms) {
				if (!form) {
					osfSearchKeys.emplace_back();
					continue;
				}
				std::string key = CaseFold(FormDisplayName(form));
				if (const char* edid = form->GetFormEditorID(); edid && edid[0]) {
					key += ' ';
					key += CaseFold(edid);
				}
				const auto model = FormModelPath(form);
				if (!model.empty()) {
					const auto slash = model.find_last_of("\\/");
					key += ' ';
					key += CaseFold(slash == std::string::npos ? model : model.substr(slash + 1));
				}
				key += ' ';
				key += std::format("{:08X}", form->GetFormID());
				osfSearchKeys.push_back(std::move(key));
			}
			osfSearchDirty = false;
		}
		// The page's header click. The order is kept here, not rebuilt per
		// request: turning a page repeats the same sort, and the overlay's
		// viewOrder belongs to a list the page does not page with.
		if (a_sortColumn >= 0) {
			const bool stale = osfOrder.size() != forms.size() || osfOrderStamp != formsStamp ||
				osfOrderColumn != a_sortColumn || osfOrderAscending != a_sortAscending;
			if (stale) {
				const auto started = std::chrono::steady_clock::now();
				osfOrder.resize(forms.size());
				for (std::size_t row = 0; row < forms.size(); ++row) {
					osfOrder[row] = row;
				}
				const bool textColumn = a_sortColumn <= 2;
				std::vector<std::string> textKey;
				std::vector<double> numKey;
				if (textColumn) {
					textKey.resize(forms.size());
					for (std::size_t row = 0; row < forms.size(); ++row) {
						const auto* form = forms[row];
						if (!form) {
							continue;
						}
						if (a_sortColumn == 0) {
							// NameSortKey, not raw bytes: the catalog carries names padded
							// with a space or an icon, and punctuation prefixes would float
							// to the top exactly as they did in the overlay.
							textKey[row] = NameSortKey(FormDisplayName(form));
						} else if (a_sortColumn == 1) {
							textKey[row] = CaseFold(OsfSubFor(form));
						} else {
							textKey[row] = CaseFold(RowTypeName(form));
						}
					}
				} else {
					// double keeps a 32-bit FormID exact; a float would round it.
					numKey.resize(forms.size());
					for (std::size_t row = 0; row < forms.size(); ++row) {
						const auto* form = forms[row];
						if (!form) {
							continue;
						}
						if (a_sortColumn == 3) {
							numKey[row] = static_cast<double>(FormValue(form));
						} else if (a_sortColumn == 4) {
							numKey[row] = static_cast<double>(FormWeight(form));
						} else {
							numKey[row] = static_cast<double>(form->GetFormID());
						}
					}
				}
				std::stable_sort(osfOrder.begin(), osfOrder.end(), [&](std::size_t a, std::size_t b) {
					auto* fa = a < forms.size() ? forms[a] : nullptr;
					auto* fb = b < forms.size() ? forms[b] : nullptr;
					int cmp = 0;
					if (textColumn) {
						cmp = textKey[a].compare(textKey[b]);
					} else {
						const double ka = numKey[a], kb = numKey[b];
						cmp = ka < kb ? -1 : ka > kb ? 1 : 0;
					}
					if (cmp == 0) {
						const auto ia = fa ? fa->GetFormID() : 0u;
						const auto ib = fb ? fb->GetFormID() : 0u;
						cmp = ia < ib ? -1 : ia > ib ? 1 : 0;
					}
					return a_sortAscending ? cmp < 0 : cmp > 0;
				});
				osfOrderStamp = formsStamp;
				osfOrderColumn = a_sortColumn;
				osfOrderAscending = a_sortAscending;
				// The order was rebuilt, so this is a header click or a new list -
				// not a page turn. Persist here as well as in the bridge: the
				// engine is what knows the order changed, and an INI write on a
				// click is cheap enough not to matter.
				OsfStoreSort(a_sortColumn, a_sortAscending);
				REX::INFO("SFShopExplorer: OSF view order {} row(s) by column {} {} in {:.2f} ms",
					forms.size(), a_sortColumn, a_sortAscending ? "asc" : "desc",
					std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
			}
		}

		std::size_t matched = 0;
		for (std::size_t r = 0; r < forms.size(); ++r) {
			const std::size_t i = (a_sortColumn >= 0 && r < osfOrder.size()) ? osfOrder[r] :
				(r < viewOrder.size() ? viewOrder[r] : r);
			if (i >= forms.size()) {
				continue;
			}
			const auto* form = forms[i];
			if (!form) {
				continue;
			}
			if (!needle.empty()) {
				if (i >= osfSearchKeys.size() || osfSearchKeys[i].find(needle) == std::string::npos) {
					continue;
				}
			}
			++matched;
			if (matched <= a_offset || a_out.size() >= a_limit) {
				continue;
			}			{
				auto row = OsfRowFor(form);
				row.favourite = IsFavourite(form->GetFormID());
				if (const auto group = duplicateGroupOf.find(row.formID); group != duplicateGroupOf.end()) {
					row.copies = static_cast<std::uint32_t>(group->second.size());
				}
				a_out.push_back(std::move(row));
			}
		}
		return matched;
	}

	// The OSF page's sort lives under its own keys. The overlay's `[shop]
	// sortColumn` cannot be shared: it is written 1-based and has no Shelf
	// column, so the same number would mean a different heading in each front
	// end, and the two builds each read their own INI file anyway.
	int ShopBrowser::OsfStoredSortColumn()
	{
		const int column = LoadShopIniInt(L"shop", L"osfSortColumn", -1);
		return column >= 0 && column <= 5 ? column : -1;
	}

	bool ShopBrowser::OsfStoredSortAscending()
	{
		return LoadShopIniInt(L"shop", L"osfSortAscending", 1) != 0;
	}

	void ShopBrowser::OsfStoreSort(const int a_column, const bool a_ascending)
	{
		SaveShopIniInt(L"shop", L"osfSortColumn", a_column);
		SaveShopIniInt(L"shop", L"osfSortAscending", a_ascending ? 1 : 0);
	}

	std::vector<std::uint32_t> ShopBrowser::OsfFavourites()
	{
		std::scoped_lock lock(stateMutex);
		return favourites;
	}

	bool ShopBrowser::OsfToggleFavourite(const std::uint32_t a_formID, bool& a_nowFavourite)
	{
		std::scoped_lock lock(stateMutex);
		auto* form = RE::TESForm::LookupByID(a_formID);
		if (!form || !IsCatalogRecordType(form->GetFormType())) {
			return false;
		}
		ToggleFavourite(a_formID, FormDisplayName(form));
		a_nowFavourite = IsFavourite(a_formID);
		return true;
	}

	std::vector<OsfBasketRow> ShopBrowser::OsfBasket()
	{
		std::scoped_lock lock(stateMutex);
		std::vector<OsfBasketRow> rows;
		rows.reserve(basket.size());
		for (const auto& entry : basket) {
			OsfBasketRow row;
			row.formID = entry.formID;
			row.count = entry.count;
			row.omods = entry.selectedOmods.size();
			auto* form = RE::TESForm::LookupByID(entry.formID);
			if (form) {
				row.name = FormDisplayName(form);
				row.sub = OsfSubFor(form);
				row.unitPrice = FormValue(form);
				row.weight = FormWeight(form);
			} else {
				row.name = entry.edid.empty() ? std::format("{:08X}", entry.formID) : entry.edid;
				row.sub = "Missing";
			}
			rows.push_back(std::move(row));
		}
		return rows;
	}

	std::uint32_t ShopBrowser::OsfCredits()
	{
		const auto credits = PlayerCredits(RE::PlayerCharacter::GetSingleton());
		walletCredits.store(credits, std::memory_order_release);
		return credits;
	}

	std::uint32_t ShopBrowser::OsfBasketCost(const bool a_free)
	{
		std::scoped_lock lock(stateMutex);
		if (a_free) {
			return 0;
		}
		std::uint64_t total = 0;
		for (const auto& entry : basket) {
			if (auto* form = RE::TESForm::LookupByID(entry.formID)) {
				total += static_cast<std::uint64_t>(FormValue(form)) * static_cast<std::uint64_t>(std::clamp(entry.count, 1, 999));
			}
		}
		return total > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(total);
	}

	bool ShopBrowser::OsfBasketAdd(const std::uint32_t a_formID, const std::int32_t a_count, std::string& a_error)
	{
		std::scoped_lock lock(stateMutex);
		auto* form = RE::TESForm::LookupByID(a_formID);
		if (!form || !form->IsBoundObject()) {
			a_error = std::format("{:08X} is not an inventory item", a_formID);
			return false;
		}
		const auto count = std::clamp(a_count, 1, 999);
		if (form->GetFormType() == RE::FormType::kWEAP) {
			CollectWeaponAssemblyNifs(form, [](const char*) {});
		}
		auto selectedOmods = SelectedOmodsFor(form);
		for (auto& entry : basket) {
			if (entry.formID == a_formID && entry.selectedOmods == selectedOmods) {
				entry.count = std::clamp(entry.count + count, 1, 999);
				return true;
			}
		}
		if (basket.size() >= 25) {
			a_error = "basket is full (25 lines) - accept or clear it first";
			return false;
		}
		const auto* edid = form->GetFormEditorID();
		basket.push_back(BasketEntry{ a_formID, edid ? edid : "", count, std::move(selectedOmods) });
		return true;
	}

	bool ShopBrowser::OsfBasketSet(const std::uint32_t a_formID, const std::int32_t a_count)
	{
		std::scoped_lock lock(stateMutex);
		for (auto& entry : basket) {
			if (entry.formID == a_formID) {
				entry.count = std::clamp(a_count, 1, 999);
				return true;
			}
		}
		return false;
	}

	bool ShopBrowser::OsfBasketRemove(const std::size_t a_index)
	{
		std::scoped_lock lock(stateMutex);
		if (a_index >= basket.size()) {
			return false;
		}
		basket.erase(basket.begin() + static_cast<std::ptrdiff_t>(a_index));
		return true;
	}

	void ShopBrowser::OsfBasketClear()
	{
		std::scoped_lock lock(stateMutex);
		basket.clear();
	}

	bool ShopBrowser::OsfAccept(const bool a_free, std::vector<std::string>& a_lines)
	{
		std::scoped_lock lock(stateMutex);
		OsfTakeMessages();
		if (basket.empty()) {
			a_lines = { "Basket is empty." };
			return false;
		}
		freeTransfer = a_free;
		ConfirmBasket();
		a_lines = OsfTakeMessages();
		return basket.empty();
	}

	OsfDetail ShopBrowser::OsfDetailOf(const std::uint32_t a_formID)
	{
		std::scoped_lock lock(stateMutex);
		OsfDetail detail;
		auto* form = RE::TESForm::LookupByID(a_formID);
		if (!form) {
			return detail;
		}
		detail.ok = true;
		detail.row = OsfRowFor(form);
		detail.row.favourite = IsFavourite(a_formID);
		if (const auto it = duplicateGroupOf.find(a_formID); it != duplicateGroupOf.end()) {
			// The page can select any member, including one the collapsed list is
			// not showing, so the count and the sibling list follow the record.
			detail.row.copies = static_cast<std::uint32_t>(it->second.size());
			for (const auto id : it->second) {
				if (id == a_formID) {
					continue;
				}
				if (auto* sibling = RE::TESForm::LookupByID(id)) {
					detail.duplicates.push_back(OsfRowFor(sibling));
				}
			}
		}
		detail.formType = OsfRecordCode(form);
		detail.keywords = FormKeywords(form, 512);
		detail.model = FormModelPath(form);
		if (form->GetFormType() == RE::FormType::kWEAP) {
			CollectWeaponAssemblyNifs(form, [](const char*) {});
			WeaponCatalog cat;
			WeaponChoice choice;
			{
				std::scoped_lock choiceLock(g_assemblyMutex);
				if (const auto it = g_assemblyCatalogs.find(a_formID); it != g_assemblyCatalogs.end()) {
					cat = it->second;
				}
				if (const auto it = g_weaponChoice.find(a_formID); it != g_weaponChoice.end()) {
					choice = it->second;
				}
			}
			const auto* typedWeapon = form->As<RE::TESObjectWEAP>();
			const std::string foldedModel = typedWeapon ? CaseFold(typedWeapon->BGSModelMaterialSwap::model.c_str()) : std::string{};
			const bool dummyBase = !foldedModel.empty() && foldedModel.find("dummy") != std::string::npos;
			const bool meleeDummy = dummyBase && foldedModel.find("meleedummyhousing") != std::string::npos;
			for (const auto& slot : cat.slotOrder) {
				const auto sit = cat.slots.find(slot);
				if (sit == cat.slots.end() || sit->second.empty()) {
					continue;
				}
				std::uint32_t cur = 0;
				if (const auto cit = choice.slotFid.find(slot); cit != choice.slotFid.end()) {
					cur = cit->second;
				} else if (const auto dit = cat.defaultFid.find(slot); dit != cat.defaultFid.end()) {
					cur = dit->second;
				}
				if ((slot == "receiver" || (dummyBase && IsMandatoryDummyAssemblySlot(slot, meleeDummy))) && cur == 0) {
					if (const auto dit = cat.defaultFid.find(slot); dit != cat.defaultFid.end()) {
						cur = dit->second;
					}
				}
				const auto dit = cat.defaultFid.find(slot);
				if (slot != "receiver" && !(dummyBase && IsMandatoryDummyAssemblySlot(slot, meleeDummy))) {
					detail.mods.push_back(OsfOption{ slot, "Stock / no extra part", 0, cur == 0, dit != cat.defaultFid.end() && dit->second == 0 });
				}
				for (const auto& option : sit->second) {
					detail.mods.push_back(OsfOption{ slot, option.label, option.fid, option.fid == cur,
						dit != cat.defaultFid.end() && dit->second == option.fid });
				}
			}
			detail.qualities.push_back(OsfOption{ "quality", "Standard", 0, choice.qualityFid == 0, false });
			for (const auto& quality : cat.qualities) {
				detail.qualities.push_back(OsfOption{ "quality", quality.label, quality.fid, quality.fid == choice.qualityFid, false });
			}
			if (!cat.effects.empty()) {
				struct RarityPick
				{
					const char*   label;
					std::uint32_t fid;
				};
				static constexpr RarityPick kRarities[] = {
					{ "Common (no effects)", 0 },
					{ "Rare (1 effect)", kRaritySentinelRare },
					{ "Epic (2 effects)", kRaritySentinelEpic },
					{ "Legendary (3 effects)", kRaritySentinelLegendary },
				};
				for (const auto& rarity : kRarities) {
					detail.rarities.push_back(OsfOption{ "rarity", rarity.label, rarity.fid, rarity.fid == choice.rarityFid, false });
				}
				detail.rarityNote = std::format("{} legendary effects in this weapon's pool", cat.effects.size());
			}
		} else if (form->GetFormType() == RE::FormType::kARMO) {
			const auto slot = ArmorLegendarySlot(form);
			const auto& pool = ArmorLegendaryPool(slot);
			if (!pool.empty()) {
				ArmorRarityChoice choice;
				{
					std::scoped_lock choiceLock(g_assemblyMutex);
					if (const auto it = g_armorRarity.find(a_formID); it != g_armorRarity.end()) {
						choice = it->second;
					}
				}
				struct RarityPick
				{
					const char*   label;
					std::uint32_t fid;
				};
				static constexpr RarityPick kRarities[] = {
					{ "Common (no effects)", 0 },
					{ "Rare (1 effect)", kRaritySentinelRare },
					{ "Epic (2 effects)", kRaritySentinelEpic },
					{ "Legendary (3 effects)", kRaritySentinelLegendary },
				};
				for (const auto& rarity : kRarities) {
					detail.rarities.push_back(OsfOption{ "rarity", rarity.label, rarity.fid, rarity.fid == choice.rarityFid, false });
				}
				detail.rarityNote = std::format("{} legendary effects in this {} pool", pool.size(),
					slot == 1u ? "helmet" : (slot == 2u ? "backpack" : "armor"));
			}
		}
		return detail;
	}

	bool ShopBrowser::OsfSetMod(const std::uint32_t a_formID, const std::string& a_slot, const std::uint32_t a_fid)
	{
		std::scoped_lock lock(stateMutex);
		auto* form = RE::TESForm::LookupByID(a_formID);
		if (!form || form->GetFormType() != RE::FormType::kWEAP || a_slot.empty()) {
			return false;
		}
		SetWeaponSlotChoice(a_formID, a_slot, a_fid);
		return true;
	}

	bool ShopBrowser::OsfSetQuality(const std::uint32_t a_formID, const std::uint32_t a_fid)
	{
		std::scoped_lock lock(stateMutex);
		auto* form = RE::TESForm::LookupByID(a_formID);
		if (!form || form->GetFormType() != RE::FormType::kWEAP) {
			return false;
		}
		SetWeaponQualityChoice(a_formID, a_fid);
		return true;
	}

	bool ShopBrowser::OsfSetRarity(const std::uint32_t a_formID, const std::uint32_t a_fid)
	{
		std::scoped_lock lock(stateMutex);
		auto* form = RE::TESForm::LookupByID(a_formID);
		if (!form) {
			return false;
		}
		if (form->GetFormType() == RE::FormType::kWEAP) {
			SetWeaponRarityChoice(a_formID, a_fid);
			return true;
		}
		if (form->GetFormType() == RE::FormType::kARMO) {
			SetArmorRarityChoice(a_formID, a_fid, ArmorLegendarySlot(form));
			return true;
		}
		return false;
	}

	std::vector<std::string> ShopBrowser::OsfTakeMessages()
	{
		std::scoped_lock lock(notifyMutex);
		auto lines = notifyLines;
		notifyLines.clear();
		return lines;
	}

	// -----------------------------------------------------------------------
	// OSF page extras.
	// -----------------------------------------------------------------------
	namespace
	{
		// The page paints with CSS, so every theme colour crosses the bridge as
		// #rrggbb. Alpha is dropped deliberately: the page is opaque-backed and
		// a faded panel over a photograph is what the overlay's opacity setting
		// is for, not what a page needs.
		std::string HexOf(const ImVec4& a_color)
		{
			const auto channel = [](const float v) {
				return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
			};
			return std::format("#{:02x}{:02x}{:02x}", channel(a_color.x), channel(a_color.y), channel(a_color.z));
		}

		// #rrggbb or #rrggbbaa, as the page's colour inputs write it. Alpha is
		// kept from the current colour when the page sends six digits, because
		// the panel alphas are part of the palette's look and a colour input
		// has no alpha column.
		bool ColourFromHex(const std::string& a_text, ImVec4& a_out)
		{
			if (a_text.size() != 7 && a_text.size() != 9) {
				return false;
			}
			if (a_text[0] != '#') {
				return false;
			}
			unsigned value = 0;
			for (std::size_t i = 1; i < a_text.size(); ++i) {
				const char c = a_text[i];
				unsigned     digit = 0;
				if (c >= '0' && c <= '9') {
					digit = static_cast<unsigned>(c - '0');
				} else if (c >= 'a' && c <= 'f') {
					digit = static_cast<unsigned>(c - 'a') + 10;
				} else if (c >= 'A' && c <= 'F') {
					digit = static_cast<unsigned>(c - 'A') + 10;
				} else {
					return false;
				}
				value = (value << 4) | digit;
			}
			a_out.x = static_cast<float>((value >> (a_text.size() == 9 ? 24 : 16)) & 0xFF) / 255.0f;
			a_out.y = static_cast<float>((value >> (a_text.size() == 9 ? 16 : 8)) & 0xFF) / 255.0f;
			a_out.z = static_cast<float>((value >> (a_text.size() == 9 ? 8 : 0)) & 0xFF) / 255.0f;
			if (a_text.size() == 9) {
				a_out.w = static_cast<float>(value & 0xFF) / 255.0f;
			}
			return true;
		}
	}

	ShopBrowser::OsfPreviewSource ShopBrowser::OsfPreviewSourceOf(const std::uint32_t a_formID)
	{
		// Reads the catalog's own arrays and the assembly cache, so it takes the
		// same lock every other catalog read does.
		std::scoped_lock lock(stateMutex);
		OsfPreviewSource out;
		out.formID = a_formID;
		auto* form = RE::TESForm::LookupByID(a_formID);
		if (!form) {
			out.error = "that record is not in the load order";
			return out;
		}
		out.weapon = form->GetFormType() == RE::FormType::kWEAP;
		out.label = FormPreviewLabel(form);
		out.paths = FormModelPaths(form);
		if (out.paths.empty()) {
			out.error = "this record carries no 3D model";
			return out;
		}
		if (SkipModelRequested(form, out.paths)) {
			out.paths.clear();
			out.error = "this model is on the preview skiplist";
			return out;
		}
		out.ok = true;
		return out;
	}

	ShopBrowser::OsfAudioState ShopBrowser::OsfAudio()
	{
		std::scoped_lock lock(stateMutex);
		OsfAudioState audio;
		audio.musicVolume = musicVolume;
		audio.sfxVolume = sfxVolume;
		audio.musicMuted = musicMuted;
		audio.sfxMuted = sfxMuted;			audio.track = musicTrack;
			audio.tracks = ShopAudio::ListMusicTracks();
			audio.playlist = ShopAudio::MusicIsPlaylist();
			return audio;
	}

	void ShopBrowser::OsfSetAudio(const int a_musicVolume, const int a_sfxVolume, const bool a_musicMuted,
		const bool a_sfxMuted, const std::string& a_track, const bool a_persist)
	{
		std::scoped_lock lock(stateMutex);
		const bool wasMuted = musicMuted;
		musicVolume = std::clamp(a_musicVolume, 0, 100);
		sfxVolume = std::clamp(a_sfxVolume, 0, 100);
		musicMuted = a_musicMuted;
		sfxMuted = a_sfxMuted;
		const bool trackChanged = a_track != musicTrack;
		musicTrack = a_track;
		ApplyAudioSettings();
		if (trackChanged || (wasMuted && !musicMuted)) {
			// The chosen file only takes effect once it is handed to the player;
			// an empty name means "follow the theme" again. Unmuting restarts
			// the track, which is what the overlay's popup does.
			ShopAudio::SetMusicFile(musicTrack.empty() ? MusicTrackFile().c_str() : musicTrack.c_str());
			if (!musicMuted) {
				ShopAudio::StartMusic();
			}
		}
		if (a_persist) {
			SaveAudioSettings();
		}
	}

	std::vector<ShopBrowser::OsfThemeRow> ShopBrowser::OsfThemes()
	{
		std::scoped_lock lock(stateMutex);
		LoadProgress();
		// Same one-time read the overlay does when it first draws: the page can
		// be the first thing to ask for the theme, before any ImGui frame.
		EnsureThemeIni(theme);
		std::vector<OsfThemeRow> rows;
		for (int i = 0; i < static_cast<int>(Theme::kTotal); ++i) {
			const auto& ui = ActiveTheme(i);
			OsfThemeRow row;
			row.index = i;
			row.name = ui.name;
			row.suffix = ui.suffix ? ui.suffix : "";
			row.premium = IsPremium(static_cast<Theme>(i));
			row.unlocked = IsUnlocked(static_cast<Theme>(i));
			row.active = theme == i;
			row.custom = static_cast<Theme>(i) == Theme::kCustom;
			row.accent = HexOf(ui.accent);
			row.accentBright = HexOf(ui.accentBright);
			row.gold = HexOf(ui.gold);
			row.panel = HexOf(ui.panel);
			row.panelAlt = HexOf(ui.panelAlt);
			row.window = HexOf(ui.windowBg);
			row.ink = HexOf(ui.ink);
			row.muted = HexOf(ui.muted);
			row.border = HexOf(ui.border);
			if (row.premium && !row.unlocked) {
				for (std::size_t p = 0; p < kPremiumCount; ++p) {
					if (kPremium[p].theme == static_cast<Theme>(i)) {
						row.progress = std::format("{} / {} {}", ProgressStat(kPremium[p].metric),
							kPremium[p].goal, MetricLabel(kPremium[p].metric));
						break;
					}
				}
			}
			rows.push_back(std::move(row));
		}
		return rows;
	}

	bool ShopBrowser::OsfSetTheme(const int a_index)
	{
		std::scoped_lock lock(stateMutex);
		if (a_index < 0 || a_index >= static_cast<int>(Theme::kTotal)) {
			return false;
		}
		LoadProgress();
		if (!IsUnlocked(static_cast<Theme>(a_index))) {
			REX::INFO("SFShopExplorer OSF: theme {} is still locked", a_index);
			return false;
		}
		theme = a_index;
		SaveThemeIni(theme);
		REX::INFO("SFShopExplorer OSF: theme -> {} ({})", ActiveTheme(theme).name, theme);
		return true;
	}

	ShopBrowser::OsfCustomThemeState ShopBrowser::OsfCustomTheme()
	{
		std::scoped_lock lock(stateMutex);
		// The page can be the first thing to ask, before any ImGui frame has
		// loaded the stored theme.
		LoadCustomTheme();
		const auto& custom = Custom();
		OsfCustomThemeState out;
		out.name = custom.name;
		out.suffix = custom.backdrop;
		out.music = custom.music;
		out.layout = LayoutName(custom.layout);
		for (const auto& colour : kCustomColours) {
			out.colours.emplace_back(colour.key, HexOf(custom.ui.*(colour.field)));
		}
		return out;
	}

	bool ShopBrowser::OsfSetCustomTheme(const std::string& a_name, const std::string& a_suffix,
		const std::vector<std::pair<std::string, std::string>>& a_colours)
	{
		std::scoped_lock lock(stateMutex);
		LoadCustomTheme();
		auto& custom = Custom();
		if (!a_name.empty()) {
			custom.name = a_name;
		}
		// The backdrop is offered as a fixed list on both sides, so an empty
		// suffix (the shared art) is a real choice and not a missing value.
		custom.backdrop = a_suffix;
		for (const auto& [key, hex] : a_colours) {
			for (const auto& colour : kCustomColours) {
				if (key == colour.key) {
					auto& value = custom.ui.*(colour.field);
					ColourFromHex(hex, value);
					break;
				}
			}
		}
		SyncCustomThemeStrings();
		SaveCustomTheme();
		REX::INFO("SFShopExplorer OSF: custom theme saved ('{}', {} colour(s) from the page)",
			custom.name, a_colours.size());
		return true;
	}
}
