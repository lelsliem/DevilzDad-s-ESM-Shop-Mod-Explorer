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
	inline constexpr char kVersion[] = "0.8.301";

	enum class Theme : int
	{
		kDevilzRed = 0,
		kVoidBlue,
		kStarGold,
		kTerminalGreen,
		kPaperLedger,
		kHazard,
		kStarfield,
		kHell,
		kHeaven,
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
	}	// Armor subcategories (keyword-driven, from the SFDevilzDump catalog:
	// 123x ArmorTypeSpacesuitBody, 115x SpacesuitHelmet, 107x
	// SpacesuitBackpack, 578x InventoryCategoryApparel). Priority order is
	// Accessory > Suit > Helmet > Pack > Apparel > Hat > Other:
	//
	// - Body beats helmet. A full suit carries ArmorTypeSpacesuitHelmet (and
	//   SpacesuitBackpack) because it includes them; the old Helmet-first order
	//   filed every Starborn suit - Tempus and Solis among them - under Helmets.
	//   All 24 records with both keywords are suits, not helmets.
	// - ArmorTypeApparelHead is the head SLOT, not a helmet: it covers caps,
	//   hats, masks and the Neuroamps. Accessories are matched first so the
	//   glasses and Neuroboost items separate from the hats they share it with.
	//
	// Actor skins never reach these buckets: an ARMO without any
	// InventoryCategory*/ArmorType* keyword is a creature/robot/turret body
	// shell, not an item, and BuildFormList drops it before classifying. That
	// empties Other (328 skin rows in the test profile); the bucket stays for a
	// plugin record that has no subtype keyword but is still wearable.
	enum class ArmorSub : std::uint8_t
	{
		kAll = 0,
		kSuit,
		kHelmet,
		kHat,
		kAccessory,
		kPack,
		kApparel,
		kOther,
		kTotal
	};

	inline constexpr const char* ArmorSubName(ArmorSub a_sub)
	{
		switch (a_sub)
		{
		case ArmorSub::kAll: return "All Armor";
		case ArmorSub::kSuit: return "Spacesuits";
		case ArmorSub::kHelmet: return "Helmets";
		case ArmorSub::kHat: return "Hats / Caps";
		case ArmorSub::kAccessory: return "Accessories";
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

	enum class AmmoSub : std::uint8_t { kAll = 0, kBallistic, kEnergy, kMissile, kExplosive, kShipCreature, kOther, kTotal };
	enum class MiscSub : std::uint8_t { kAll = 0, kFlora, kToys, kCards, kResources, kContainers, kOther, kTotal };
	enum class AidSub : std::uint8_t { kAll = 0, kFood, kMeds, kChems, kOther, kTotal };
	// Book subcategories. Slates are the readable data items; posters are the
	// flat art some mods and the base game file as BOOK records (their NIF's
	// front face is a blank poster material and the art arrives as a record
	// material swap); notes/reports are single-page reads; logs are the
	// multi-part journals; guides are the actual bound books. Anything else
	// (comics, magazines, loose records) stays reachable under Misc / Other.
	enum class BookSub : std::uint8_t { kAll = 0, kSlates, kPosters, kNotes, kLogs, kGuides, kOther, kTotal };
	enum class KeySub : std::uint8_t { kAll = 0, kKeycards, kKeys, kCodes, kOther, kTotal };

	inline constexpr const char* AmmoSubName(AmmoSub a_sub)
	{
		switch (a_sub) {
		case AmmoSub::kAll: return "All Ammo";
		case AmmoSub::kBallistic: return "Ballistic";
		case AmmoSub::kEnergy: return "Energy / Particle";
		case AmmoSub::kMissile: return "Missiles";
		case AmmoSub::kExplosive: return "Mines / Grenades";
		case AmmoSub::kShipCreature: return "Ship / Creature";
		case AmmoSub::kOther: return "Other";
		default: return "?";
		}
	}

	inline constexpr const char* MiscSubName(MiscSub a_sub)
	{
		switch (a_sub) {
		case MiscSub::kAll: return "All Misc";
		case MiscSub::kFlora: return "Flora";
		case MiscSub::kToys: return "Toys";
		case MiscSub::kCards: return "Cards";
		case MiscSub::kResources: return "Resources";
		case MiscSub::kContainers: return "Containers";
		case MiscSub::kOther: return "Other";
		default: return "?";
		}
	}

	inline constexpr const char* AidSubName(AidSub a_sub)
	{
		switch (a_sub) {
		case AidSub::kAll: return "All Aid";
		case AidSub::kFood: return "Food / Drink";
		case AidSub::kMeds: return "Medical";
		case AidSub::kChems: return "Chems";
		case AidSub::kOther: return "Other";
		default: return "?";
		}
	}

	inline constexpr const char* BookSubName(BookSub a_sub)
	{
		switch (a_sub) {
		case BookSub::kAll: return "All Books";
		case BookSub::kSlates: return "Slates";
		case BookSub::kPosters: return "Posters";
		case BookSub::kNotes: return "Notes / Reports";
		case BookSub::kLogs: return "Logs / Journals";
		case BookSub::kGuides: return "Guides / Books";
		case BookSub::kOther: return "Misc / Other";
		default: return "?";
		}
	}

	inline constexpr const char* KeySubName(KeySub a_sub)
	{
		switch (a_sub) {
		case KeySub::kAll: return "All Keys";
		case KeySub::kKeycards: return "Keycards / Access";
		case KeySub::kKeys: return "Keys";
		case KeySub::kCodes: return "Codes / Passcodes";
		case KeySub::kOther: return "Other";
		default: return "?";
		}
	}

	static_assert(static_cast<std::size_t>(AmmoSub::kOther) + 1 == static_cast<std::size_t>(AmmoSub::kTotal));
	static_assert(static_cast<std::size_t>(MiscSub::kOther) + 1 == static_cast<std::size_t>(MiscSub::kTotal));
	static_assert(static_cast<std::size_t>(AidSub::kOther) + 1 == static_cast<std::size_t>(AidSub::kTotal));
	static_assert(static_cast<std::size_t>(BookSub::kOther) + 1 == static_cast<std::size_t>(BookSub::kTotal));
	static_assert(static_cast<std::size_t>(KeySub::kOther) + 1 == static_cast<std::size_t>(KeySub::kTotal));

	// Category filters are keyword/name based, so unknown records remain discoverable under Other.
	// For category assignment, item names and live form keywords take priority over fallback patterns.

	// One basket line: a form plus how many of it to transfer. Resolved via
	// FormID at accept time so list rebuilds (Refresh) cannot invalidate it.
	struct BasketEntry
	{
		std::uint32_t formID{ 0 };
		std::string edid;
		std::int32_t count{ 1 };
		std::vector<std::uint32_t> selectedOmods;
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
		// Persists a [shop] bool immediately (music, fullscreen).
		static void SaveShopIniBool(const wchar_t* key, bool value);
		static bool LoadShopIniBool(const wchar_t* key, bool defaultValue);
		void RenderList();
		void RefreshKeywords();

		// Ends the open-shop session: close sting, music fade out, world unfreeze.
		// Deliberately does NOT take stateMutex, because two of the four close
		// paths (the ImGui window X and the Close button) run inside DrawImGui,
		// which already holds it for its whole duration and would deadlock.
		// Idempotent via the reference-counted pause, so a double teardown is
		// harmless and never double-plays the sting.
		void ReleaseSession();

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
		bool hideCrewEnemyCreature{ true };  // plugin-list filter for crew, enemy and creature forms
		bool hideTurrets{ true };     // "turret" identity, turret skins, and ammo with no name or model (crate dummies, ship rounds)
		std::size_t cursor{ 0 };       // source/type/form selection index
		std::size_t page{ 0 };
		std::size_t keyboardRow{ 0 };  // visible row index (respects sort/search)
		static constexpr std::size_t pageSize{ 24 };
		std::int32_t selectedPlugin{ -1 };
		BrowseType selectedType{ BrowseType::kWeapon };
		ArmorSub selectedArmorSub{ ArmorSub::kAll };
		WeaponSub selectedWeaponSub{ WeaponSub::kAll };
		AmmoSub selectedAmmoSub{ AmmoSub::kAll };
		MiscSub selectedMiscSub{ MiscSub::kAll };
		AidSub selectedAidSub{ AidSub::kAll };
		BookSub selectedBookSub{ BookSub::kAll };
		KeySub selectedKeySub{ KeySub::kAll };
		std::size_t selectedFormIndex{ 0 };
		std::int32_t addCount{ 1 };
		bool freeTransfer{ true };
		std::vector<BasketEntry> basket;
		int theme{ 0 };
		int textSize{ 1 };  // 0 small, 1 medium, 2 large, 3 extra large; persisted under [shop]
		std::vector<std::string> selectedFormKeywords;
		std::string selectedFormLabel;		// display name of the selected form (offline preview label)
		char search[128]{};
		std::atomic<std::uint32_t> walletCredits{ 0 };
		bool fullscreen{ true };		// start maximized: the window must fit every screen on open
		bool musicEnabled{ true };      // [shop] music; persisted on toggle
bool previewExpanded{ false };  // click (no drag) on the viewport: tall preview vs compact strip
	bool previewPress{ false };     // current press began on the preview viewport (expand click gate)
	bool popJustOpened{ false };    // pop-out appeared this frame: place it once, then let the user drag

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
