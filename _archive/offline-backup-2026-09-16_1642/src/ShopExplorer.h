#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "RE/T/TESDataHandler.h"

namespace SFE
{
	enum class Theme : int
	{
		kDevilzRed = 0,
		kVoidBlue,
		kStarGold,
		kTerminalGreen,
		kTotal
	};

	enum class BrowseType : std::uint8_t
	{
		kWeapon = 0,
		kArmor,
		kAmmo,
		kMisc,
		kAid,
		kBook,
		kKey,
		kIngr,
		kAllTypes,
		kTotal
	};

	inline constexpr RE::FormType BrowseFormType(BrowseType a_type)
	{
		switch (a_type) {
		case BrowseType::kWeapon: return RE::FormType::kWEAP;
		case BrowseType::kArmor: return RE::FormType::kARMO;
		case BrowseType::kAmmo: return RE::FormType::kAMMO;
		case BrowseType::kMisc: return RE::FormType::kMISC;
		case BrowseType::kAid: return RE::FormType::kALCH;
		case BrowseType::kBook: return RE::FormType::kBOOK;
		case BrowseType::kKey: return RE::FormType::kKEYM;
		case BrowseType::kIngr: return RE::FormType::kINGR;
		default: return RE::FormType::kNONE;
		}
	}

	inline constexpr const char* BrowseTypeName(BrowseType a_type)
	{
		switch (a_type) {
		case BrowseType::kWeapon: return "Weapons";
		case BrowseType::kArmor: return "Armor";
		case BrowseType::kAmmo: return "Ammo";
		case BrowseType::kMisc: return "Misc";
		case BrowseType::kAid: return "Aid";
		case BrowseType::kBook: return "Books";
		case BrowseType::kKey: return "Keys";
		case BrowseType::kIngr: return "Ingredients";
		case BrowseType::kAllTypes: return "All Types";
		default: return "?";
		}
	}

	// Armor subcategories (keyword-driven, from the SFDevilzDump catalog:
	// 123x ArmorTypeSpacesuitBody, 115x SpacesuitHelmet, 107x
	// SpacesuitBackpack, 578x InventoryCategoryApparel). Priority order is
	// Helmet > Pack > Suit > Apparel > Other so shared rigs (ma_armor_
	// Spacesuit covers body+helmet+pack) settle deterministically.
	enum class ArmorSub : std::uint8_t
	{
		kAll = 0,
		kSuit,
		kHelmet,
		kPack,
		kApparel,
		kOther,
		kTotal
	};

	inline constexpr const char* ArmorSubName(ArmorSub a_sub)
	{
		switch (a_sub) {
		case ArmorSub::kAll: return "All Armor";
		case ArmorSub::kSuit: return "Spacesuits";
		case ArmorSub::kHelmet: return "Helmets";
		case ArmorSub::kPack: return "Backpacks";
		case ArmorSub::kApparel: return "Apparel";
		case ArmorSub::kOther: return "Other";
		default: return "?";
		}
	}

	// Weapon subcategories (keyword-driven: DisplayObjectType* primary,
	// WeaponType* fallback). Priority Sniper > Shotgun > Pistol > Rifle >
	// Heavy > Melee > Thrown > Other settles multi-tagged rows (sniper
	// rifles carry both Rifle display and Sniper weapon keywords).
	enum class WeaponSub : std::uint8_t
	{
		kAll = 0,
		kPistol,
		kRifle,
		kShotgun,
		kSniper,
		kHeavy,
		kMelee,
		kThrown,
		kOther,
		kTotal
	};

	inline constexpr const char* WeaponSubName(WeaponSub a_sub)
	{
		switch (a_sub) {
		case WeaponSub::kAll: return "All Weapons";
		case WeaponSub::kPistol: return "Pistols";
		case WeaponSub::kRifle: return "Rifles";
		case WeaponSub::kShotgun: return "Shotguns";
		case WeaponSub::kSniper: return "Snipers";
		case WeaponSub::kHeavy: return "Heavy";
		case WeaponSub::kMelee: return "Melee";
		case WeaponSub::kThrown: return "Thrown";
		case WeaponSub::kOther: return "Other";
		default: return "?";
		}
	}

	// One basket line: a form plus how many of it to transfer. Resolved via
	// FormID at accept time so list rebuilds (Refresh) cannot invalidate it.
	struct BasketEntry
	{
		std::uint32_t formID{ 0 };
		std::string edid;
		std::int32_t count{ 1 };
	};

	class ShopBrowser
	{
	public:
		static ShopBrowser* GetSingleton();

		// SFSE task-thread lifecycle and input polling.
		void FrameUpdate();
		void ProcessPendingActions();
		void Toggle();
		void Open();
		void Close();
		[[nodiscard]] bool IsOpen() const { return open.load(std::memory_order_acquire); }

		// Called only from the native ImGui render path.
		void DrawImGui();

	private:
		enum class State : std::uint8_t { kPlugins, kTypes, kForms, kCount };

		ShopBrowser() = default;
		void BuildPluginList();
		void BuildFormList();
		void ApplyViewOrder();
		void SelectCurrent();
		void ConfirmAdd();
		void ConfirmBasket();
		void Back();
		void StepList(int delta);
		void Notify(const std::string& text) const;
		void RenderList();
		void RefreshKeywords();

		std::atomic_bool open{ false };
		mutable std::mutex stateMutex;
		State state{ State::kPlugins };
		std::vector<std::string> pluginNames;
		std::vector<RE::TESFile*> pluginFiles;
		std::vector<RE::TESForm*> forms;
		std::vector<std::size_t> viewOrder;  // display order into forms (column sort)
		int sortColumn{ -1 };                // -1 catalog order, 0 name, 1 type, 2 value, 3 weight, 4 formid
		bool sortAscending{ true };
		bool allPlugins{ true };     // browse every loaded source at once
		bool hideTestDebug{ true };  // EDID/name markers: NOCLUTTER/DUMMY/TEMPLATE/DEBUG/NOT PLAYABLE
		bool hideShipHair{ true };   // keyword substrings: spaceship/hair/beard/naked/nude
		std::size_t cursor{ 0 };
		std::size_t page{ 0 };
		static constexpr std::size_t pageSize{ 24 };
		std::int32_t selectedPlugin{ -1 };
		BrowseType selectedType{ BrowseType::kWeapon };
		ArmorSub selectedArmorSub{ ArmorSub::kAll };
		WeaponSub selectedWeaponSub{ WeaponSub::kAll };
		std::size_t selectedFormIndex{ 0 };
		std::int32_t addCount{ 1 };
		bool freeTransfer{ true };
		std::vector<BasketEntry> basket;
		int theme{ 0 };
		std::vector<std::string> selectedFormKeywords;
		std::string selectedFormLabel;		// display name of the selected form (offline preview label)
		char search[128]{};
		std::atomic<std::uint32_t> walletCredits{ 0 };
		bool fullscreen{ true };		// start maximized: the window must fit every screen on open

		// ImGui runs from Present; these actions are consumed by the SFSE task.
		bool pendingFree{ true };
		bool pendingAccept{ false };
		bool pendingClear{ false };
		std::int32_t pendingSelection{ -1 };
		bool pendingPlugin{ false };
		bool pendingType{ false };
		bool pendingSelect{ false };
		bool pendingBack{ false };
		mutable std::mutex actionMutex;

		std::atomic_bool wasToggle{ false };
		bool wasPageUp{ false };
		bool wasUp{ false };
		bool wasDown{ false };
		bool wasEnter{ false };
		bool wasEsc{ false };
		bool wasLeft{ false };
		bool wasRight{ false };
	};
}
