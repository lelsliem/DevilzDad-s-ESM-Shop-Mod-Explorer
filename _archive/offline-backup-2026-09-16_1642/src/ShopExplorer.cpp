#include "PCH.h"
#include "ShopExplorer.h"
#include "ImguiOverlay.h"
#include "ItemCapture.h"
#include "OfflineModel.h"

#include <algorithm>
#include <bit>
#include <filesystem>
#include <format>
#include <unordered_map>
#include <unordered_set>

#include "RE/B/BSLock.h"

#include <windows.h>

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
#include "RE/T/TESValueForm.h"
#include "RE/T/TESWeightForm.h"
#include "RE/T/TESFile.h"
#include "RE/T/TESForm.h"
#include "RE/T/TESObjectREFR.h"
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
			default:
				return nullptr;
			}
		}

		std::string CaseFold(std::string a_text)
		{
			for (auto& ch : a_text) {
				ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			}
			return a_text;
		}

		// Armor subcategory from casefolded keywords. Priority Helmet >
		// Pack > Suit > Apparel > Other settles shared rigs (ma_armor_
		// Spacesuit spans body+helmet+pack) deterministically.
		ArmorSub ClassifyArmorSub(const std::string& a_foldedKeys)
		{
			if (a_foldedKeys.find("spacesuithelmet") != std::string::npos ||
				a_foldedKeys.find("categoryarmorhelmet") != std::string::npos ||
				a_foldedKeys.find("apparelhead") != std::string::npos) {
				return ArmorSub::kHelmet;
			}
			if (a_foldedKeys.find("spacesuitbackpack") != std::string::npos ||
				a_foldedKeys.find("categoryarmorbackpack") != std::string::npos) {
				return ArmorSub::kPack;
			}
			if (a_foldedKeys.find("spacesuitbody") != std::string::npos ||
				a_foldedKeys.find("categoryarmorsuit") != std::string::npos) {
				return ArmorSub::kSuit;
			}
			if (a_foldedKeys.find("apparel") != std::string::npos) {
				return ArmorSub::kApparel;
			}
			return ArmorSub::kOther;
		}

		// Weapon subcategory from casefolded keywords. DisplayObjectType*
		// carries the family (pistol/rifle/shotgun/melee/...); WeaponType*
		// refines (sniper rifles, heavies). Priority Sniper > Shotgun >
		// Pistol > Rifle > Heavy > Melee > Thrown > Other. Unarmed
		// creature attacks (no display type) land in Other.
		WeaponSub ClassifyWeaponSub(const std::string& a_foldedKeys)
		{
			if (a_foldedKeys.find("sniper") != std::string::npos) {
				return WeaponSub::kSniper;
			}
			if (a_foldedKeys.find("displayobjecttypeshotgun") != std::string::npos ||
				a_foldedKeys.find("weapontypeshotgun") != std::string::npos) {
				return WeaponSub::kShotgun;
			}
			if (a_foldedKeys.find("displayobjecttypepistol") != std::string::npos ||
				a_foldedKeys.find("weapontypepistol") != std::string::npos) {
				return WeaponSub::kPistol;
			}
			if (a_foldedKeys.find("displayobjecttyperifle") != std::string::npos ||
				a_foldedKeys.find("weapontyperifle") != std::string::npos ||
				a_foldedKeys.find("weapontypeassaultrifle") != std::string::npos ||
				a_foldedKeys.find("weapontypeautomatic") != std::string::npos) {
				return WeaponSub::kRifle;
			}
			if (a_foldedKeys.find("toolandheavyweapon") != std::string::npos ||
				a_foldedKeys.find("weapontypeheavy") != std::string::npos ||
				a_foldedKeys.find("weapontypeheavyweapon") != std::string::npos ||
				a_foldedKeys.find("displayobjecttypeheavy") != std::string::npos) {
				return WeaponSub::kHeavy;
			}
			if (a_foldedKeys.find("displayobjecttypemelee") != std::string::npos ||
				a_foldedKeys.find("weapontypemelee") != std::string::npos ||
				a_foldedKeys.find("animknife") != std::string::npos ||
				a_foldedKeys.find("animonehanded") != std::string::npos ||
				a_foldedKeys.find("sword") != std::string::npos ||
				a_foldedKeys.find("blade") != std::string::npos ||
				a_foldedKeys.find("knife") != std::string::npos) {
				return WeaponSub::kMelee;
			}
			if (a_foldedKeys.find("throwable") != std::string::npos ||
				a_foldedKeys.find("weapontypethrown") != std::string::npos ||
				a_foldedKeys.find("weapontypeexplosive") != std::string::npos ||
				a_foldedKeys.find("weapontypemine") != std::string::npos ||
				a_foldedKeys.find("animgrenade") != std::string::npos) {
				return WeaponSub::kThrown;
			}
			return WeaponSub::kOther;
		}

		// Live form value/weight from the typed instance data. Tested against the
		// DevilzDump CSV ground truth (e.g. Swimsuit 00002FA4 = 94 / 0.300).
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
		};

		const UiTheme& ActiveTheme(int a_index)
		{
			static const UiTheme themes[] = {
				{ "Devilz Red",
					{ 0.73f, 0.15f, 0.20f, 1.0f }, { 0.95f, 0.28f, 0.32f, 1.0f }, { 0.78f, 0.58f, 0.25f, 1.0f },
					{ 0.58f, 0.55f, 0.55f, 1.0f }, { 0.93f, 0.89f, 0.86f, 1.0f }, { 0.025f, 0.022f, 0.028f, 0.985f },
					{ 0.055f, 0.045f, 0.052f, 0.96f }, { 0.10f, 0.055f, 0.065f, 0.96f }, { 0.33f, 0.12f, 0.15f, 0.8f },
					{ 0.34f, 0.08f, 0.11f, 0.85f }, { 0.60f, 0.12f, 0.17f, 0.9f },
					{ 0.25f, 0.07f, 0.10f, 0.9f }, { 0.55f, 0.12f, 0.16f, 0.95f },
					{ 0.11f, 0.07f, 0.08f, 1.0f }, { 0.28f, 0.10f, 0.13f, 1.0f } },
				{ "Void Blue",
					{ 0.16f, 0.38f, 0.75f, 1.0f }, { 0.35f, 0.60f, 0.95f, 1.0f }, { 0.80f, 0.70f, 0.35f, 1.0f },
					{ 0.55f, 0.58f, 0.62f, 1.0f }, { 0.88f, 0.91f, 0.96f, 1.0f }, { 0.020f, 0.025f, 0.040f, 0.985f },
					{ 0.045f, 0.055f, 0.095f, 0.96f }, { 0.055f, 0.090f, 0.160f, 0.96f }, { 0.12f, 0.22f, 0.42f, 0.8f },
					{ 0.10f, 0.18f, 0.38f, 0.85f }, { 0.16f, 0.30f, 0.60f, 0.9f },
					{ 0.07f, 0.12f, 0.26f, 0.9f }, { 0.12f, 0.22f, 0.46f, 0.95f },
					{ 0.07f, 0.10f, 0.18f, 1.0f }, { 0.12f, 0.18f, 0.30f, 1.0f } },
				{ "Star Gold",
					{ 0.78f, 0.60f, 0.20f, 1.0f }, { 0.98f, 0.82f, 0.40f, 1.0f }, { 0.85f, 0.68f, 0.30f, 1.0f },
					{ 0.60f, 0.55f, 0.45f, 1.0f }, { 0.96f, 0.92f, 0.82f, 1.0f }, { 0.045f, 0.038f, 0.020f, 0.985f },
					{ 0.085f, 0.070f, 0.040f, 0.96f }, { 0.140f, 0.110f, 0.055f, 0.96f }, { 0.45f, 0.35f, 0.12f, 0.8f },
					{ 0.42f, 0.32f, 0.10f, 0.85f }, { 0.62f, 0.48f, 0.16f, 0.9f },
					{ 0.30f, 0.23f, 0.08f, 0.9f }, { 0.50f, 0.38f, 0.12f, 0.95f },
					{ 0.16f, 0.13f, 0.06f, 1.0f }, { 0.30f, 0.24f, 0.10f, 1.0f } },
				{ "Terminal Green",
					{ 0.15f, 0.75f, 0.35f, 1.0f }, { 0.35f, 0.95f, 0.55f, 1.0f }, { 0.60f, 0.85f, 0.45f, 1.0f },
					{ 0.45f, 0.58f, 0.48f, 1.0f }, { 0.85f, 0.95f, 0.88f, 1.0f }, { 0.015f, 0.030f, 0.018f, 0.985f },
					{ 0.035f, 0.060f, 0.042f, 0.96f }, { 0.055f, 0.100f, 0.065f, 0.96f }, { 0.10f, 0.35f, 0.18f, 0.8f },
					{ 0.08f, 0.28f, 0.14f, 0.85f }, { 0.14f, 0.45f, 0.22f, 0.9f },
					{ 0.06f, 0.18f, 0.10f, 0.9f }, { 0.10f, 0.32f, 0.16f, 0.95f },
					{ 0.06f, 0.12f, 0.08f, 1.0f }, { 0.10f, 0.22f, 0.13f, 1.0f } },
			};
			constexpr int count = static_cast<int>(std::size(themes));
			return themes[std::clamp(a_index, 0, count - 1)];
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

		std::string FormDisplayName(const RE::TESForm* a_form)
		{
			// FULL name first (the SFDevilzDump-proven path: Aid/Keys/Books/Misc/
			// Ammo carry their player-visible names in the TESFullName component,
			// not the editor ID), falling back to the editor ID. RTTI cast so it
			// works for every bound-object type without per-class plumbing.
			if (a_form) {
				if (auto* full = starfield_cast<RE::TESFullName*>(const_cast<RE::TESForm*>(a_form)); full) {
					if (const char* name = full->GetFullName(); name && *name) {
						return name;
					}
				}
				const char* edid = a_form->GetFormEditorID();
				if (edid && *edid) {
					return edid;
				}
			}
			return {};
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
			const auto ini = std::filesystem::path(modulePath).parent_path() / L"SFShopExplorer.ini";
			wchar_t buf[4096]{};
			GetPrivateProfileStringW(L"shop", L"skipmodels", L"", buf, 4096, ini.c_str());
			std::wstring cur;
			for (const wchar_t* p = buf; ; ++p) {
				if (*p == L';' || *p == L'\0') {
					if (!cur.empty()) {
						std::string narrow(cur.begin(), cur.end());
						g_skipModels.insert(CaseFold(narrow));
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
					// and the engine assembles the real gun from its mods.
					// Dummy marker only: EMPTY-model rows are overwhelmingly
					// creature attacks (Unarmed*/Creature* WEAPs) — assembling
					// those from Starfield.esm OMODs previewed random base
					// guns (a stinger for a flyer head). They stay empty and
					// report "no 3D model" instead.
					const char* mp = typed->BGSModelMaterialSwap::model.c_str();
					if (mp && mp[0] && CaseFold(mp).find("dummy") == std::string::npos) {
						pushUnique(mp);
					} else if (mp && mp[0]) {
						CollectWeaponAssemblyNifs(form, pushUnique);
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
				break;
			}
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
					REX::INFO("SFShopExplorer: form array for type {} found at offset 0x{:X} ({} entries)", type, offset, size);
				}
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
			}
			const auto base = std::bit_cast<std::uintptr_t>(a_data);
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
		// honest unavailable beats a franken-gun). Capped at 8 NIFs.
		// Session-cached per weapon (the preview calls FormModelPaths per
		// frame).
		std::unordered_map<std::uint32_t, std::vector<std::string>> g_assemblyCache;
		std::mutex g_assemblyMutex;
		// Family coherence: an assembly must be ONE gun, not one slot per
		// random family (every Starfield dummy funneled into M1919 parts).
		// Kept words (>=3 chars, non-generic) from folded text; a weapon
		// token and an OMOD word match when either contains the other.
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
			if (a_folded.find("grip") != std::string::npos || a_folded.find("handle") != std::string::npos ||
				a_folded.find("foregrip") != std::string::npos) {
				return "grip";
			}
			if (a_folded.find("scope") != std::string::npos || a_folded.find("sight") != std::string::npos ||
				a_folded.find("optic") != std::string::npos || a_folded.find("reflex") != std::string::npos ||
				a_folded.find("iron") != std::string::npos) {
				return "scope";
			}
			if (a_folded.find("laser") != std::string::npos) {
				return "laser";
			}
			if (a_folded.find("trigger") != std::string::npos || a_folded.find("internal") != std::string::npos) {
				return "internal";
			}
			return {};
		}
		template <typename PushFn>
		void CollectWeaponAssemblyNifs(const RE::TESForm* a_weapon, const PushFn& a_push)
		{
			if (!a_weapon) {
				return;
			}
			const std::uint32_t wid = a_weapon->GetFormID();
			{
				std::scoped_lock lock(g_assemblyMutex);
				const auto cached = g_assemblyCache.find(wid);
				if (cached != g_assemblyCache.end()) {
					for (const auto& p : cached->second) {
						a_push(p.c_str());
					}
					return;
				}
			}
			auto* dataHandler = RE::TESDataHandler::GetSingleton();
			if (!dataHandler) {
				return;
			}
			const std::string owner = RuntimeOwnerOfFormID(dataHandler, wid);
			if (owner.empty()) {
				return;
			}
			struct Part
			{
				std::string slot;
				std::string nif;
				std::string dir;
				std::string folded;
				bool standard{ false };
			};
			std::vector<Part> parts;
			std::size_t nOwner = 0, nCast = 0;
			{
				std::vector<RE::TESForm*> omods;
				BuildFormListForType(dataHandler, RE::FormType::kOMOD, omods, false);
				for (auto* form : omods) {
					if (!form) {
						break;
					}
					if (RuntimeOwnerOfFormID(dataHandler, form->GetFormID()) != owner) {
						continue;
					}
					++nOwner;
					auto* mod = form->As<RE::BGSMod::Attachment::Mod>();
					if (!mod) {
						continue;
					}
					++nCast;
					const char* mp = mod->BGSModelMaterialSwap::model.c_str();
					if (!mp || !mp[0]) {
						continue;  // "No Mod"/quality/modgroup placeholders
					}
					// Fold every identity text; first slot hit wins.
					std::string folded;
					{
						const auto* edid = form->GetFormEditorID();
						if (edid) {
							folded += CaseFold(edid);
							folded += ' ';
						}
					}
					folded += CaseFold(FormDisplayName(form));
					folded += ' ';
					std::string dir;
					{
						std::string base = mp;
						const auto slash = base.find_last_of("\\/");
						std::string full = CaseFold(base);
						if (slash != std::string::npos) {
							dir = full.substr(0, slash);
							base.erase(0, slash + 1);
						}
						folded += CaseFold(base);
						folded += ' ';
					}
					if (mod->attachPoint) {
						const auto* ap = mod->attachPoint->GetFormEditorID();
						if (ap) {
							folded += CaseFold(ap);
						}
					}
				 const auto slot = WeaponPartSlot(folded);
					if (slot.empty()) {
						continue;
					}
					const bool standard = folded.find("standard") != std::string::npos;
					bool dup = false;
					for (const auto& p : parts) {
						if (p.nif == mp) {
							dup = true;
							break;
						}
					}
					if (!dup) {
						parts.push_back(Part{ slot, mp, dir, folded, standard });
					}
				}
				// Family gate: the weapon's kept tokens (EDID + display name)
			// must meet the OMOD somewhere (either side contains the
			// other), or share the NIF dir. Small plugins (<150 OMODs)
			// fall back to all-owner (single-gun mods are coherent by
			// construction). Big plugins with no token match assemble
			// NOTHING — honest unavailable beats cross-family franken
			// (every Starfield dummy funneled into M1919 parts).
			std::vector<std::string> famTok;
			{
				const auto* wedid = a_weapon->GetFormEditorID();
				if (wedid) {
					SplitWords(CaseFold(wedid), famTok);
				}
				SplitWords(CaseFold(FormDisplayName(a_weapon)), famTok);
			}
			std::vector<const Part*> family;
			for (const auto& p : parts) {
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
				if (kin) {
					family.push_back(&p);
				}
			}
			if (family.empty() && nOwner < 150) {
				for (const auto& p : parts) {
					family.push_back(&p);
				}
			}
			// Anchor: most common NIF dir among receiver parts. Everything
			// outside the anchor (or its subdirs) is another gun's
			// furniture sharing the plugin file.
			std::string anchor;
			{
				std::unordered_map<std::string, std::size_t> votes;
				for (const auto* p : family) {
					if (p->slot == "receiver") {
						++votes[p->dir];
					}
				}
				std::size_t best = 0;
				for (const auto& [dir, count] : votes) {
					if (count > best) {
						best = count;
						anchor = dir;
					}
				}
			}
			REX::INFO("SFShopExplorer: dummy weapon '{}' OMODs: {} owner, {} cast, {} slotted, {} family, anchor '{}'",
				a_weapon->GetFormEditorID(), nOwner, nCast, parts.size(), family.size(), anchor);
			// Assemble: receiver core + one default per slot (standard
			// preferred, else first-seen), anchor family only. No anchor
			// (or no receiver) => no assembly: fall back to EDID
			// resolution instead of a franken-gun.
			static constexpr const char* kSlots[] = { "receiver", "barrel", "muzzle", "mag",
				"stock", "grip", "scope", "laser", "internal" };
			std::vector<std::string> found;
			std::string composition;
			if (!anchor.empty()) {
				for (const char* slot : kSlots) {
					const Part* pick = nullptr;
					for (const auto* pp : family) {
						const auto& p = *pp;
						if (p.slot != slot) {
							continue;
						}
						if (p.dir != anchor && p.dir.compare(0, anchor.size() + 1, anchor + "/") != 0) {
							continue;
						}
						if (!pick || (p.standard && !pick->standard)) {
							pick = &p;
						}
					}
					if (pick && found.size() < 8) {
						found.push_back(pick->nif);
						if (!composition.empty()) {
							composition += '+';
						}
						composition += slot;
					}
				}
			}
			{
				std::scoped_lock lock(g_assemblyMutex);
				g_assemblyCache.emplace(wid, found);
			}
			for (const auto& p : found) {
				a_push(p.c_str());
			}
			REX::INFO("SFShopExplorer: dummy weapon '{}' assembly: {} [{}]",
				a_weapon->GetFormEditorID(), found.size(), composition);
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
	}

	ShopBrowser* ShopBrowser::GetSingleton()
	{
		static ShopBrowser instance;
		return &instance;
	}

	void ShopBrowser::FrameUpdate()
	{
		const int toggleKey = Overlay::ToggleKey();
		const bool toggle = toggleKey != 0 && (GetAsyncKeyState(toggleKey) & 0x8000) != 0;
		if (toggle && !wasToggle.exchange(true, std::memory_order_acq_rel)) {
			Toggle();
		}
		if (!toggle) {
			wasToggle.store(false, std::memory_order_release);
		}
		ProcessPendingActions();
		// Offline 3D preview: rasterize pending model requests on this task
		// thread — pure CPU work, no GPU state touched (the Present path only
		// copies finished pixels), so the render thread is never at risk.
		OfflineModel::ServicePending();
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
		std::scoped_lock lock(stateMutex);
		BuildPluginList();
		state = State::kForms;
		cursor = page = 0;
		selectedPlugin = pluginFiles.empty() ? -1 : 0;
		allPlugins = true;  // full catalog by default; pick a source below to narrow
		selectedType = BrowseType::kAllTypes;  // every record type at once
		selectedArmorSub = ArmorSub::kAll;
		selectedWeaponSub = WeaponSub::kAll;
		selectedFormIndex = 0;
		addCount = 1;
		freeTransfer = true;
		basket.clear();
		walletCredits.store(PlayerCredits(RE::PlayerCharacter::GetSingleton()), std::memory_order_release);
		BuildFormList();
		open.store(true, std::memory_order_release);
		Notify("opened");
	}

	void ShopBrowser::Close()
	{
		std::scoped_lock lock(stateMutex);
		open.store(false, std::memory_order_release);
		Notify("closed");
	}

	void ShopBrowser::Notify(const std::string& a_text) const
	{
		REX::INFO("SFShopExplorer: {}", a_text);
	}

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

	void ShopBrowser::BuildFormList()
	{
		forms.clear();
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
		std::size_t hiddenTest = 0, hiddenTighten = 0;
		for (auto* form : typed) {
			if (!allPlugins) {
				const auto owner = RuntimeOwnerOfFormID(dataHandler, form->GetFormID());
				if (owner != ownerFilter) {
					continue;
				}
			}
			const auto* edid = form->GetFormEditorID();
			const std::string ed = edid ? edid : "";
			if (hideTestDebug) {
				const auto upper = CaseFold(ed);
				bool test = upper.find("noclutter") != std::string::npos ||
					upper.find("dummy") != std::string::npos ||
					upper.find("template") != std::string::npos ||
					upper.find("debug") != std::string::npos;
				if (!test) {
					test = CaseFold(FormDisplayName(form)).find("not playable") != std::string::npos;
				}
				if (test) {
					++hiddenTest;
					continue;
				}
			}
			if (hideShipHair) {
				const auto keys = CaseFold(FormKeywords(form, 512));
				// spaceship/hair/beard are safe substrings (no apparel
				// keyword contains them — verified against the catalog).
				// naked/nude must be EDID tokens: nearly every garment
				// carries ArmorTypeApparelOrNakedBody, so substring would
				// hide 494/582 apparel rows (all clothes!). Skin/nude forms
				// carry it in the EDID (Skin_Naked) and still hide.
				if (keys.find("spaceship") != std::string::npos || keys.find("hair") != std::string::npos ||
					keys.find("beard") != std::string::npos) {
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
				const auto keys = CaseFold(FormKeywords(form, 2048));
				if (ClassifyArmorSub(keys) != selectedArmorSub) {
					continue;
				}
			}
			// Weapon subcategory (toolbar combo, Weapons type only).
			if (selectedType == BrowseType::kWeapon && selectedWeaponSub != WeaponSub::kAll) {
				const auto keys = CaseFold(FormKeywords(form, 2048));
				if (ClassifyWeaponSub(keys) != selectedWeaponSub) {
					continue;
				}
			}
			forms.push_back(form);
		}
		if (selectedFormIndex >= forms.size()) {
			selectedFormIndex = forms.empty() ? 0 : forms.size() - 1;
		}
		ApplyViewOrder();
		if (selectedType == BrowseType::kArmor && selectedArmorSub != ArmorSub::kAll) {
			REX::INFO("SFShopExplorer: {} Armor/{} forms for {} (hidden test={} tighten={})", forms.size(),
				ArmorSubName(selectedArmorSub), allPlugins ? "all plugins" : ownerFilter, hiddenTest, hiddenTighten);
		} else if (selectedType == BrowseType::kWeapon && selectedWeaponSub != WeaponSub::kAll) {
			REX::INFO("SFShopExplorer: {} Weapons/{} forms for {} (hidden test={} tighten={})", forms.size(),
				WeaponSubName(selectedWeaponSub), allPlugins ? "all plugins" : ownerFilter, hiddenTest, hiddenTighten);
		} else {
			REX::INFO("SFShopExplorer: {} {} forms for {} (hidden test={} tighten={})", forms.size(),
				BrowseTypeName(selectedType), allPlugins ? "all plugins" : ownerFilter, hiddenTest, hiddenTighten);
		}
	}

	void ShopBrowser::ApplyViewOrder()
	{
		viewOrder.resize(forms.size());
		for (std::size_t i = 0; i < forms.size(); ++i) {
			viewOrder[i] = i;
		}
		if (sortColumn < 0) {
			return;
		}
		const int col = sortColumn;
		const bool asc = sortAscending;
		std::stable_sort(viewOrder.begin(), viewOrder.end(), [&](std::size_t a, std::size_t b) {
			auto* fa = a < forms.size() ? forms[a] : nullptr;
			auto* fb = b < forms.size() ? forms[b] : nullptr;
			if (!fa || !fb) {
				return a < b;
			}
			int cmp = 0;
			switch (col) {
			case 0: {
				const auto na = CaseFold(FormDisplayName(fa));
				const auto nb = CaseFold(FormDisplayName(fb));
				cmp = na.compare(nb);
				break;
			}
			case 1: {
				const std::string ta = RowTypeName(fa);
				const std::string tb = RowTypeName(fb);
				cmp = ta.compare(tb);
				break;
			}
			case 2: {
				const auto va = FormValue(fa), vb = FormValue(fb);
				cmp = va < vb ? -1 : va > vb ? 1 : 0;
				break;
			}
			case 3: {
				const auto wa = FormWeight(fa), wb = FormWeight(fb);
				cmp = wa < wb ? -1 : wa > wb ? 1 : 0;
				break;
			}
			default: {
				const auto ia = fa->GetFormID(), ib = fb->GetFormID();
				cmp = ia < ib ? -1 : ia > ib ? 1 : 0;
				break;
			}
			}
			if (cmp == 0) {
				const auto ia = fa->GetFormID(), ib = fb->GetFormID();
				cmp = ia < ib ? -1 : ia > ib ? 1 : 0;
			}
			return asc ? cmp < 0 : cmp > 0;
		});
	}

	void ShopBrowser::SelectCurrent()
	{
		if (state == State::kPlugins && cursor < pluginFiles.size()) {
			selectedPlugin = static_cast<std::int32_t>(cursor);
			state = State::kTypes;
		} else if (state == State::kTypes && cursor < static_cast<std::size_t>(BrowseType::kTotal)) {
			selectedType = static_cast<BrowseType>(cursor);
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
		// moves every entry at once (ConfirmBasket). Merge with an existing
		// line for the same form so the list stays readable.
		const auto count = std::clamp(addCount, 1, 999);
		const auto formID = form->GetFormID();
		for (auto& entry : basket) {
			if (entry.formID == formID) {
				entry.count = std::clamp(entry.count + count, 1, 999);
				return;
			}
		}
		const auto* edid = form->GetFormEditorID();
		basket.push_back(BasketEntry{ formID, edid ? edid : "", count });
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
			resolved.push_back({ static_cast<RE::TESBoundObject*>(form), count, unitPrice, entry.formID });
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

		for (const auto& item : resolved) {
			player->AddObjectToContainer(item.bound, RE::BSTSmartPointer<RE::ExtraDataList>(new RE::ExtraDataList()), item.count, nullptr, RE::ITEM_TRANSFER_REASON::kNone);
			Notify(std::format("added x{} {:08X} {}", item.count, item.formID, freeTransfer ? "free" : std::format("({} credits)", item.unitPrice * item.count)));
		}
		walletCredits.store(PlayerCredits(player), std::memory_order_release);
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

	void ShopBrowser::DrawImGui()
	{
		std::scoped_lock lock(stateMutex);
		if (!open.load(std::memory_order_acquire)) {
			return;
		}

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
		ImGui::PushStyleColor(ImGuiCol_WindowBg, th.windowBg);
		ImGui::PushStyleColor(ImGuiCol_ChildBg, panel);
		ImGui::PushStyleColor(ImGuiCol_Border, th.border);
		ImGui::PushStyleColor(ImGuiCol_Header, th.header);
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, th.headerHovered);
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, accent);
		ImGui::PushStyleColor(ImGuiCol_Button, th.button);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, th.buttonHovered);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, accentBright);
		ImGui::PushStyleColor(ImGuiCol_FrameBg, th.frame);
		ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, th.frameHovered);
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
			ImGui::PopStyleColor(12);
			ImGui::PopStyleVar(2);
			return;
		}
		if (!windowOpen) {
			open.store(false, std::memory_order_release);
		}

		const auto space = Overlay::SpaceTexture();
		if (space != static_cast<ImTextureID>(0)) {
			const auto pos = ImGui::GetWindowPos();
			const auto size = ImGui::GetWindowSize();
			ImGui::GetWindowDrawList()->AddImage(space, pos, ImVec2(pos.x + size.x, pos.y + size.y), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, 44));
		}

		if (ImGui::BeginTable("reference_header", 3, ImGuiTableFlags_NoSavedSettings, ImVec2(0.0f, 102.0f))) {
			ImGui::TableSetupColumn("brand", ImGuiTableColumnFlags_WidthFixed, 360.0f);
			ImGui::TableSetupColumn("logo", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("actions", ImGuiTableColumnFlags_WidthFixed, 470.0f);
			ImGui::TableNextColumn();
			ImGui::TextColored(accentBright, "☠  DevilzDad's Shop + Explorer");
			ImGui::TextColored(muted, "Loaded worlds • forbidden wares • one inventory");
			ImGui::TableNextColumn();
			const auto logo = Overlay::ShopLogoTexture();
			if (logo != static_cast<ImTextureID>(0)) {
				const float logoSize = 96.0f;
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - logoSize) * 0.5f);
				ImGui::Image(logo, ImVec2(logoSize, logoSize));
			}
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth(240.0f);
			ImGui::InputTextWithHint("##shop_search", "Search plugins or items...", search, sizeof(search));
			ImGui::SameLine();
			const auto credit = Overlay::CreditIconTexture();
			if (credit != static_cast<ImTextureID>(0)) {
				ImGui::Image(credit, ImVec2(40.0f, 20.0f));
				ImGui::SameLine();
			}
			ImGui::TextColored(gold, "%u", walletCredits.load(std::memory_order_acquire));
			ImGui::SameLine();
			if (ImGui::Button("Refresh")) {
				BuildPluginList();
				if (selectedPlugin >= 0) BuildFormList();
			}
			ImGui::EndTable();
		}
		ImGui::Separator();

		ImGui::BeginChild("reference_toolbar", ImVec2(0.0f, 48.0f), ImGuiChildFlags_Borders);
		ImGui::TextColored(accentBright, "CATALOG");
		ImGui::SameLine();
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
		// Armor subcategory (Armor type only): Spacesuits / Helmets /
		// Backpacks / Apparel / Other, keyword-classified.
		if (selectedType == BrowseType::kArmor) {
			ImGui::SameLine();
			ImGui::SetNextItemWidth(140.0f);
			if (ImGui::BeginCombo("##toolbar_armorsub", ArmorSubName(selectedArmorSub))) {
				for (std::size_t i = 0; i < static_cast<std::size_t>(ArmorSub::kTotal); ++i) {
					const auto sub = static_cast<ArmorSub>(i);
					if (ImGui::Selectable(ArmorSubName(sub), selectedArmorSub == sub)) {
						selectedArmorSub = sub;
						selectedFormIndex = 0;
						cursor = page = 0;
						BuildFormList();
					}
				}
				ImGui::EndCombo();
			}
		}
		// Weapon subcategory (Weapons type only): Pistols / Rifles /
		// Shotguns / Snipers / Heavy / Melee / Thrown / Other.
		if (selectedType == BrowseType::kWeapon) {
			ImGui::SameLine();
			ImGui::SetNextItemWidth(140.0f);
			if (ImGui::BeginCombo("##toolbar_weaponsub", WeaponSubName(selectedWeaponSub))) {
				for (std::size_t i = 0; i < static_cast<std::size_t>(WeaponSub::kTotal); ++i) {
					const auto sub = static_cast<WeaponSub>(i);
					if (ImGui::Selectable(WeaponSubName(sub), selectedWeaponSub == sub)) {
						selectedWeaponSub = sub;
						selectedFormIndex = 0;
						cursor = page = 0;
						BuildFormList();
					}
				}
				ImGui::EndCombo();
			}
		}
		ImGui::SameLine();
		ImGui::TextColored(muted, "Qty");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(60.0f);
		ImGui::InputInt("##toolbar_quantity", &addCount, 1, 10);
		addCount = std::clamp(addCount, 1, 999);
		ImGui::SameLine();
		ImGui::Checkbox("Free", &freeTransfer);
		ImGui::SameLine();
		if (ImGui::BeginCombo("##shop_theme", ActiveTheme(theme).name)) {
			for (int t = 0; t < static_cast<int>(Theme::kTotal); ++t) {
				if (ImGui::Selectable(ActiveTheme(t).name, theme == t)) {
					theme = t;
				}
			}
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		if (ImGui::Button(fullscreen ? "WINDOWED" : "MAXIMIZE")) {
			fullscreen = !fullscreen;
		}
		ImGui::SameLine();
		if (ImGui::Button("Close")) {
			open.store(false, std::memory_order_release);
		}
		ImGui::EndChild();

		if (ImGui::BeginTable("reference_workspace", 3, ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable, ImVec2(0.0f, -32.0f))) {
			ImGui::TableSetupColumn("plugins", ImGuiTableColumnFlags_WidthFixed, 280.0f);
			ImGui::TableSetupColumn("items", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("details", ImGuiTableColumnFlags_WidthFixed, 340.0f);

			ImGui::TableNextColumn();
			ImGui::BeginChild("reference_plugins", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
			ImGui::TextColored(accentBright, "PLUGINS");
			ImGui::TextColored(muted, "%zu loaded sources", pluginNames.size());
			if (ImGui::Checkbox("Hide test/debug", &hideTestDebug)) {
				selectedFormIndex = 0;
				cursor = page = 0;
				BuildFormList();
			}
			if (ImGui::Checkbox("Hide ship/hair/etc", &hideShipHair)) {
				selectedFormIndex = 0;
				cursor = page = 0;
				BuildFormList();
			}
			ImGui::Separator();
			{
				ImGui::PushID(-1);
				if (ImGui::Selectable("★ All Plugins", allPlugins, 0, ImVec2(0.0f, 29.0f))) {
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

			ImGui::TableNextColumn();
		// Empty-category honesty: Starfield's data carries no INGR records at
		// all (SFDevilzDump-all-forms.csv: zero INGR rows across every plugin).
		// Consumables live under Aid; say so instead of a silently blank list.
			ImGui::BeginChild("reference_items", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
			const char* pluginLabel = allPlugins ? "All Plugins" :
				(selectedPlugin >= 0 && static_cast<std::size_t>(selectedPlugin) < pluginNames.size() ? pluginNames[static_cast<std::size_t>(selectedPlugin)].c_str() : "No plugin");
			ImGui::TextColored(ink, "%s", pluginLabel);
			ImGui::SameLine();
			ImGui::TextColored(muted, "(%zu records)", forms.size());
			if (selectedType == BrowseType::kIngr) {
				ImGui::SameLine();
				ImGui::TextColored(gold, "— no Ingredients exist in Starfield's data; use Aid");
			}
			ImGui::Separator();
			std::size_t shownRows = 0;
			std::size_t hoveredFormIndex = static_cast<std::size_t>(-1);
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
				const char* titles[5]{ "NAME", "TYPE", "VALUE", "WT", "FORMID" };
				for (int c = 0; c < 5; ++c) {
					ImGui::TableSetColumnIndex(c);
					std::string head = titles[c];
					if (sortColumn == c) {
						head += sortAscending ? " ▲" : " ▼";
					}
					ImGui::PushID(100 + c);
					if (ImGui::SmallButton(head.c_str())) {
						if (sortColumn == c) {
							sortAscending = !sortAscending;
						} else {
							sortColumn = c;
							sortAscending = true;
						}
						ApplyViewOrder();
					}
					ImGui::PopID();
				}
			}
			// Visible-row collection (cheap string filters, no widgets), then
			// a clipper over the matches: only on-screen rows pay for
			// Selectable/Text widgets (6575 unclipped rows lagged hard).
			std::vector<std::size_t> visRows;
			visRows.reserve(viewOrder.size());
			if (!search[0]) {
				for (const auto v : viewOrder) {
					visRows.push_back(v < forms.size() ? v : visRows.size());
				}
			} else {
				for (std::size_t r = 0; r < viewOrder.size(); ++r) {
					const std::size_t i = viewOrder[r] < forms.size() ? viewOrder[r] : r;
					auto* form = i < forms.size() ? forms[i] : nullptr;
					if (!form) {
						continue;
					}
					const auto full = FormDisplayName(form);
					const auto* edid = form->GetFormEditorID();
					bool rowMatch = FilterMatches(full, search) || FilterMatches(edid ? edid : "", search);
					if (!rowMatch) {
						// Fall through to asset identity: model basename (e.g.
						// "dataslate01", "bullpupdummyreceiver") and FormID hex,
						// so items are findable even when their display
						// name/EDID don't contain the term.
						const auto model = FormModelPath(form);
						const auto slash = model.find_last_of("\\/");
						const std::string base = slash == std::string::npos ? model : model.substr(slash + 1);
						rowMatch = FilterMatches(base, search) ||
							FilterMatches(std::format("{:08X}", form->GetFormID()), search);
					}
					if (rowMatch) {
						visRows.push_back(i);
					}
				}
			}
			shownRows = visRows.size();
			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(visRows.size()));
			while (clipper.Step()) {
				for (int d = clipper.DisplayStart; d < clipper.DisplayEnd; ++d) {
					const std::size_t i = visRows[static_cast<std::size_t>(d)];
					auto* form = i < forms.size() ? forms[i] : nullptr;
					if (!form) {
						continue;
					}
				const auto full = FormDisplayName(form);
				const auto* edid = form->GetFormEditorID();
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::PushID(static_cast<int>(i));
					const std::string rowLabel = !full.empty() ? full : (edid && edid[0] ? edid : "(unnamed)");
				if (ImGui::Selectable(rowLabel.c_str(), selectedFormIndex == i, ImGuiSelectableFlags_SpanAllColumns)) {
					selectedFormIndex = i;
					cursor = i;
					addCount = 1;						// Cache the selected form's display name so PREVIEW can label
					// its offline render without re-reading form memory there.
					selectedFormLabel = rowLabel;
				}
				if (ImGui::IsItemHovered()) {
					hoveredFormIndex = i;
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
			if (search[0]) {
				ImGui::TextColored(muted, "%zu of %zu records match '%s'", shownRows, forms.size(), search);
			}
			ImGui::EndChild();

			ImGui::TableNextColumn();
			ImGui::BeginChild("reference_details", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
			auto* form = selectedFormIndex < forms.size() ? forms[selectedFormIndex] : nullptr;
			ImGui::TextColored(accentBright, "PREVIEW");
			ImGui::Separator();
			// Metadata-only viewport. Starfield's native 3D scene cannot be attached
			// to this independent ImGui child through a verified public API.
			{
				const float viewHeight = std::clamp(ImGui::GetContentRegionAvail().y * 0.34f, 120.0f, 260.0f);
			ImGui::BeginChild("item_render_viewport", ImVec2(0.0f, viewHeight), ImGuiChildFlags_Borders | ImGuiChildFlags_FrameStyle);
			// The 3D viewport follows the hovered row when the mouse is over
			// the list, else the clicked selection — so the image always
			// matches what the user is pointing at. All actions below
			// (info, transfer, basket) stay on the clicked selection.
			auto* previewForm = (hoveredFormIndex < forms.size() && forms[hoveredFormIndex]) ?
				forms[hoveredFormIndex] : form;
			if (previewForm) {
				auto models = FormModelPaths(previewForm);
				const auto* edid = previewForm->GetFormEditorID();
				const auto previewFull = FormDisplayName(previewForm);
				const std::string previewLabel = !previewFull.empty() ? previewFull : (edid && edid[0] ? edid : "(unnamed)");
				std::string fallbackKey = edid ? edid : "";
				// INI skiplist: drop paths AND the EDID fallback so a
				// skipped form can never resolve (curated caches included).
				if (!models.empty() && SkipModelRequested(previewForm, models)) {
					models.clear();
					fallbackKey.clear();
				}
				// Offline 3D preview: any catalog item renders from the
				// pre-extracted geometry cache by model path — no inventory, no
				// engine cooperation. INI kill switch: [offlinepreview] enabled=0.
				// Armor passes every addon NIF (body + gloves + ...) so the
				// full item assembles; single-model types pass one path.
				// Assembled OMOD paths resolve EDID-first (a curated cache
				// hit like rattler beats the assembly's first NIF); only an
				// EDID miss live-builds the assembly. Weapons take the
				// side-profile orientation like the inventory scene.
				const bool isWeapon = previewForm->GetFormType() == RE::FormType::kWEAP;
				const bool assembled = !models.empty() && FormModelsAssembled(previewForm);
				SFE::OfflineModel::RequestPreview(models, previewLabel,
					fallbackKey, isWeapon, assembled);
				const auto offline = SFE::OfflineModel::GetSnapshot();
				const bool offlineCurrent = offline.ready && offline.srvHandle != 0 && offline.label == previewLabel;
				const ImVec2 center = ImGui::GetCursorScreenPos();
				const ImVec2 size = ImGui::GetContentRegionAvail();
					// Orbit control: an invisible button over the whole viewport
					// captures left-drag (rotate), wheel (zoom), double-click
					// (reset). It draws nothing, so the DrawList image/text below
					// stays exactly as before.
					ImGui::SetCursorScreenPos(center);
					ImGui::InvisibleButton("##preview_orbit", size,
						ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
					{
						const auto& delta = ImGui::GetIO().MouseDelta;
						// Hovered (not Active): the press often starts on the
						// list row and drags into the preview, which never
						// activates this button — gating on Active made those
						// drags silently do nothing ("refuses to rotate").
						if (ImGui::IsItemHovered() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
							SFE::OfflineModel::AddOrbit(delta.x * 0.45f, delta.y * 0.45f);
						}
						// Middle-drag zoom: no wheel-event dependency (the game
						// may eat wheel messages before they reach us). Drag up
						// zooms in, down zooms out.
						if (ImGui::IsItemHovered() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
							SFE::OfflineModel::AddZoom(1.0f - delta.y * 0.004f);
						}
					}
					if (ImGui::IsItemHovered()) {
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
				auto* draw = ImGui::GetWindowDrawList();
					draw->AddRectFilled(center, ImVec2(center.x + size.x, center.y + size.y), IM_COL32(10, 10, 16, 160), 6.0f);
					const auto capture = SFE::Overlay::ItemCaptureSnapshot();
					// Capture status line, drawn only while capture is enabled, so
					// a disabled pipeline stays silent instead of suggesting
					// the viewport depends on InventoryMenu. The pure-idle
					// line (menu closed, no pool, nothing bound) is hidden too:
					// session diagnostics stay in the log either way.
					if (capture.mode != SFE::ItemCapture::Mode::kDisabled &&
						(capture.engineMenuOpen || capture.copiedCount > 0 || capture.candidateCount > 0)) {
						const std::string status = !capture.hookInstalled ? std::string("capture: RTV hook not installed") :
							capture.mode == SFE::ItemCapture::Mode::kDisabled ? std::string("capture: disabled (INI mode=0)") :
							!capture.engineMenuOpen && capture.copiedCount > 0 ? std::format("capture: frozen scene from last session (cand={}) — open InventoryMenu to refresh", capture.candidateCount) :
							!capture.engineMenuOpen ? std::format("capture: waiting for InventoryMenu (cand={} dup={} bb={})", capture.candidateCount, capture.duplicateRtvs, capture.rejectedSwapChain) :
							capture.copiedCount == 0 ? std::format("capture: menu open, 0 targets bound (cand={} dup={} bb={})", capture.candidateCount, capture.duplicateRtvs, capture.rejectedSwapChain) :
							std::format("capture: {} target(s) bound, primary slot {}{}", capture.copiedCount, capture.primarySlot,
								capture.fullTexture ? " (grid view, INI fullTexture=0 for crop)" : "");
						draw->AddText(ImVec2(center.x + 6.0f, center.y + size.y - ImGui::GetTextLineHeight() * 2.0f - 8.0f), IM_COL32(255, 200, 90, 230), status.c_str());
					}
					if (offlineCurrent) {
						// The shop's own render: aspect-fitted into the viewport.
						const float texW = offline.width > 0 ? static_cast<float>(offline.width) : 512.0f;
						const float texH = offline.height > 0 ? static_cast<float>(offline.height) : 384.0f;
						const float scale = std::min(size.x / texW, size.y / texH);
						const float dw = texW * scale;
						const float dh = texH * scale;
						const ImVec2 p0{ center.x + (size.x - dw) * 0.5f, center.y + (size.y - dh) * 0.5f };
						draw->AddImage(static_cast<ImTextureID>(offline.srvHandle), p0, ImVec2(p0.x + dw, p0.y + dh));
						const std::string offlineTag = std::format("offline 3D — {} — {} mesh(es), {} tris ({} / {} textured)", offline.model, offline.meshes, offline.tris,
							offline.textured, offline.texturedTotal);
						draw->AddText(ImVec2(center.x + 6.0f, center.y + size.y - ImGui::GetTextLineHeight() - 4.0f), IM_COL32(120, 220, 130, 230), offlineTag.c_str());
						draw->AddText(ImVec2(center.x + 6.0f, center.y + 4.0f), IM_COL32(150, 150, 160, 180), "drag: rotate - wheel/MMB: zoom - double-click: reset");
						{
							const std::string orbitTag = std::format("yaw {:+.0f}  pitch {:+.0f}  zoom x{:.2f}",
								SFE::OfflineModel::OrbitYawDeg(), SFE::OfflineModel::OrbitPitchDeg(), SFE::OfflineModel::ZoomFactor());
							draw->AddText(ImVec2(center.x + 6.0f, center.y + 4.0f + ImGui::GetTextLineHeight() + 1.0f),
								IM_COL32(150, 150, 160, 180), orbitTag.c_str());
						}
					} else if (!offline.enabled) {
						draw->AddText(ImVec2(center.x + 6.0f, center.y + 4.0f), IM_COL32(150, 150, 160, 200), "offline preview disabled (INI enabled=0)");
					}
					if (!offlineCurrent && capture.copiedCount > 0 && capture.srvHandles[capture.primarySlot] != 0) {
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
								draw->AddText(ImVec2(cell0.x + 2.0f, cell1.y + 1.0f), primary ? IM_COL32(255, 200, 90, 255) : IM_COL32(150, 150, 160, 220), tag.c_str());
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
							draw->AddText(ImVec2(center.x + 6.0f, center.y + 4.0f + ImGui::GetTextLineHeight() * 1.2f), IM_COL32(150, 150, 160, 200), captureLabel.c_str());
						}
					} else if (!offlineCurrent) {
						const std::string offlineStatus = !offline.enabled ? std::string("offline preview disabled") :
							offline.message.empty() ? std::string("building offline render...") : offline.message;
						const char* label = "Live 3D preview unavailable in Shop Explorer";
						const ImVec2 ts = ImGui::CalcTextSize(label);
						draw->AddText(ImVec2(center.x + (size.x - ts.x) * 0.5f, center.y + (size.y - ts.y) * 0.5f - ImGui::GetTextLineHeight() * 1.4f), IM_COL32(200, 200, 210, 220), label);
						const char* label2 = models.empty() ? "(this form carries no 3D model)" : models.front().c_str();
						const ImVec2 ts2 = ImGui::CalcTextSize(label2);
						draw->AddText(ImVec2(center.x + (size.x - ts2.x) * 0.5f, center.y + (size.y - ts2.y) * 0.5f + ImGui::GetTextLineHeight() * 0.4f), IM_COL32(150, 150, 160, 200), label2);
						const ImVec2 ts3 = ImGui::CalcTextSize(offlineStatus.c_str());
						draw->AddText(ImVec2(center.x + (size.x - ts3.x) * 0.5f, center.y + (size.y - ts3.y) * 0.5f + ImGui::GetTextLineHeight() * 1.6f), IM_COL32(255, 200, 90, 220), offlineStatus.c_str());
					}
				} else {
					const ImVec2 center = ImGui::GetCursorScreenPos();
					const ImVec2 size = ImGui::GetContentRegionAvail();
					ImGui::GetWindowDrawList()->AddText(ImVec2(center.x + 8.0f, center.y + 8.0f), IM_COL32(150, 150, 160, 200), "Select an item to render it here.");
				}
				ImGui::EndChild();
			}
			if (form) {
				const auto full = FormDisplayName(form);
				const auto* edid = form->GetFormEditorID();
				ImGui::TextColored(ink, "%s", !full.empty() ? full.c_str() : (edid && edid[0] ? edid : "(unnamed)"));
				if (!full.empty() && edid && edid[0] && full != edid) {
					ImGui::TextColored(muted, "EDID: %s", edid);
				}
				ImGui::TextColored(muted, "FormID: %08X", form->GetFormID());
				ImGui::TextColored(muted, "Type: %s", RowTypeName(form));
				ImGui::TextColored(muted, "Plugin: %s", pluginLabel);
				// Which body part this is: armor/outfit items are one NIF per
				// part (upperbody/lowerbody/sleeves/hat are separate forms),
				// so the filename tells whether a "missing half" is a sibling
				// record rather than a broken preview. OMOD assemblies name
				// the part count instead of one misleading NIF.
				if (FormModelsAssembled(form)) {
					const auto it = g_assemblyCache.find(form->GetFormID());
					const std::size_t n = it != g_assemblyCache.end() ? it->second.size() : 0;
					ImGui::TextColored(muted, "Model: OMOD assembly (%zu parts, EDID fallback)", n);
				} else if (const auto modelPath = FormModelPath(form); !modelPath.empty()) {
					ImGui::TextColored(muted, "Model: %s", modelPath.c_str());
				}
				{
					// Name the actual render source even when the form carries
					// no path (placeholder-model weapons resolve via EDID, so
					// there is no Model: line at all — the snapshot key is
					// the only provenance).
					const auto snap = SFE::OfflineModel::GetSnapshot();
					if (snap.ready && !snap.model.empty()) {
						const auto modelPath = FormModelPath(form);
						const auto slash = modelPath.find_last_of("\\/");
						const std::string base = slash == std::string::npos ? modelPath : modelPath.substr(slash + 1);
						const auto dot = base.find_last_of('.');
						std::string a = dot == std::string::npos ? base : base.substr(0, dot);
						std::string b = snap.model;
						for (auto& ch : a) {
							ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
						}
						for (auto& ch : b) {
							ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
						}
						if (a != b) {
							ImGui::TextColored(gold, "Render: %s", snap.model.c_str());
						}
					}
				}
				if (auto* kwForm = KeywordFormOf(form); kwForm && kwForm->GetNumKeywords() > 0) {
					ImGui::PushStyleColor(ImGuiCol_Text, muted);
					ImGui::PushTextWrapPos(0.0f);
					ImGui::Text("Keywords:");
					kwForm->ForEachKeyword([&](RE::BGSKeyword* kw) {
						if (kw && !kw->formEditorID.empty()) {
							ImGui::BulletText("%s", kw->formEditorID.c_str());
						}
						return RE::BSContainer::ForEachResult::kContinue;
					});
					ImGui::PopTextWrapPos();
					ImGui::PopStyleColor();
				}
			} else {
				ImGui::TextColored(muted, "Select an item to inspect it.");
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
				// Append immediately (render thread owns the basket under
				// stateMutex); ACCEPT hands the whole list to the task thread.
				const auto count = std::clamp(addCount, 1, 999);
				const auto formID = form->GetFormID();
				bool merged = false;
				for (auto& entry : basket) {
					if (entry.formID == formID) {
						entry.count = std::clamp(entry.count + count, 1, 999);
						merged = true;
						break;
					}
				}
				if (!merged) {
					const auto* edid = form->GetFormEditorID();
					basket.push_back(BasketEntry{ formID, edid ? edid : "", count });
				}
			}
			ImGui::Separator();
			ImGui::TextColored(accentBright, "BASKET (%zu)", basket.size());
			if (!basket.empty()) {
				std::uint32_t basketTotal = 0;
				for (std::size_t i = 0; i < basket.size(); ++i) {
					auto& entry = basket[i];
					const auto* entryForm = RE::TESForm::LookupByID(entry.formID);
					const auto price = entryForm ? FormValue(entryForm) * static_cast<std::uint32_t>(entry.count) : 0;
					basketTotal += price;
					ImGui::PushID(static_cast<int>(i));
					ImGui::TextWrapped("%s x%d%s", entry.edid.empty() ? std::format("{:08X}", entry.formID).c_str() : entry.edid.c_str(), entry.count, price > 0 ? std::format(" — {} cr", price).c_str() : "");
					if (ImGui::SmallButton("-")) {
						entry.count = std::max(1, entry.count - 1);
					}
					ImGui::SameLine();
					if (ImGui::SmallButton("+")) {
						entry.count = std::min(999, entry.count + 1);
					}
					ImGui::SameLine();
					if (ImGui::SmallButton("x")) {
						basket.erase(basket.begin() + static_cast<std::ptrdiff_t>(i));
						ImGui::PopID();
						break;
					}
					ImGui::PopID();
				}
				if (basketTotal > 0 && !freeTransfer) {
					ImGui::TextColored(gold, "Total: %u credits", basketTotal);
				}
				if (ImGui::Button("ACCEPT", ImVec2(-1.0f, 30.0f))) {
					std::scoped_lock actionLock(actionMutex);
					pendingFree = freeTransfer;
					pendingAccept = true;
				}
				if (ImGui::Button("CANCEL", ImVec2(-1.0f, 30.0f))) {
					basket.clear();
				}
			} else {
				ImGui::TextColored(muted, "Basket is empty.");
			}
			ImGui::Separator();
			const auto chestArt = Overlay::ChestTexture();
			if (chestArt != static_cast<ImTextureID>(0)) {
				ImGui::TextColored(gold, "SHOP STORAGE");
				const float chestSize = std::min(160.0f, ImGui::GetContentRegionAvail().x);
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - chestSize) * 0.5f);
				ImGui::ImageWithBg(chestArt, ImVec2(chestSize, chestSize), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), ImVec4(0.0f, 0.0f, 0.0f, 0.0f), ImVec4(1.0f, 1.0f, 1.0f, 0.38f));
			}
			ImGui::EndChild();
			ImGui::EndTable();
		}

		if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
			std::scoped_lock actionLock(actionMutex);
			pendingBack = true;
		}
		ImGui::End();
		ImGui::PopStyleColor(12);
		ImGui::PopStyleVar(2);
	}
}
