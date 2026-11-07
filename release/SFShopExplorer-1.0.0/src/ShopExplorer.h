// ============================================================================
// ShopExplorer.h - ShopBrowser, the catalog and the shop state
//
// One class holds everything a session needs: the plugin list and the visible
// rows, the filters, the basket, the preview choice, the theme and the answers
// the OSF page asks for. The two front ends read it from the same mutex, so they
// cannot disagree about what the catalog contains.
//
// Sections, in order: enums (themes, browse types, subcategories), the small row
// and option structs, the OSF reply structs, then ShopBrowser - lifecycle and
// state first, then catalog construction, then the OSF API.
//
// Concurrency: state is guarded by stateMutex; anything the task thread sets for
// the UI is a pending* flag, consumed at the top of the next drawn frame.
// ============================================================================

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "RE/B/BSCoreTypes.h"
#include "RE/T/TESDataHandler.h"

namespace SFE
{
	inline constexpr char kVersion[] = "1.0.0";
	// The OSF front end is its own mod with its own release, so it reports its
	// own label: the page footer, the release notes and the archive name then
	// agree with each other. kVersion still stamps the persisted model index,
	// which the two builds share, so that cache is not invalidated by a label.
	inline constexpr char kVersionOsf[] = "1.0.0-OSF";
	// True when [logging] verbose=1 in this build's INI. Defined in main.cpp.
	// The diagnostics that write outside spdlog (the DRED block, the %TEMP%
	// prefetch trace) check it, so one switch turns every probe off for a
	// release build and every one back on for a support log.
	bool VerboseLogging();

	// File names of this build's own module and INI. The OSF front-end is built
	// from the same sources as the standalone overlay, so each build names its
	// files after itself; reading the other build's ini would import theme and
	// audio state the user never set here.
#ifdef SFE_OSF_FRONTEND
	inline constexpr wchar_t kIniFileName[] = L"DevilzShopOSF.ini";
	inline constexpr wchar_t kDllFileName[] = L"DevilzShopOSF.dll";
#else
	inline constexpr wchar_t kIniFileName[] = L"SFShopExplorer.ini";
	inline constexpr wchar_t kDllFileName[] = L"SFShopExplorer.dll";
#endif

	// Folder beside the DLL that holds this build's art and model index. Named
	// per build for the same reason as the ini: the two shops are separate mods
	// that can be installed side by side, and neither should read or write the
	// other's cache. [offlinepreview] dir= in the ini still overrides it.
#ifdef SFE_OSF_FRONTEND
	inline constexpr wchar_t kAssetsFolderName[] = L"DevilzShopOSFAssets";
	inline constexpr wchar_t kAssetsModelsDir[] = L"DevilzShopOSFAssets\\models";
#else
	inline constexpr wchar_t kAssetsFolderName[] = L"SFShopExplorerAssets";
	inline constexpr wchar_t kAssetsModelsDir[] = L"SFShopExplorerAssets\\models";
#endif

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
		// The tenth: built by the player, unlocked by earning all three above.
		// Appended, never inserted - the INI stores this value directly.
		kCustom,
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
	// Misc buckets. Order is the classifier's priority order, and the eight
	// model-driven buckets after Containers exist because the 2026-10-24 census
	// left 1417 records in Other: whole shelves of one NIF folder each
	// (SetDressing\ScienceGlass 223, tankgirlck\xenologymesh 132, the
	// manufactured-goods / tech-clutter tools 126, the FoodDrink_Set packs 57).
	enum class MiscSub : std::uint8_t {
		kAll = 0,
		kFlora,
		kToys,
		kCards,
		kResources,
		kContainers,
		kDecor,
		kLab,
		kCreature,
		kTech,
		kSlates,
		kFood,
		kArtifacts,
		kAnimal,
		kOther,
		kTotal
	};
	enum class AidSub : std::uint8_t { kAll = 0, kFood, kMeds, kChems, kOther, kTotal };
	// Book subcategories. The NIF decides the first split: a dataslate model is
	// a slate, a book model is a book, and a notepad/paper model is a note.
	// Names then pull notes, logs and guides out of the book pile. Anything
	// unmatched stays reachable under Misc / Other.
	//
	// There is deliberately no Posters bucket: the items that looked like
	// posters (Paradiso Resort Advertisement and friends) are dataslates, and
	// the flat art lives in the misc catalogue, not here.
	enum class BookSub : std::uint8_t { kAll = 0, kSlates, kNotes, kLogs, kGuides, kOther, kTotal };
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
		case MiscSub::kDecor: return "Décor";
		case MiscSub::kLab: return "Lab / Samples";
		case MiscSub::kCreature: return "Creature Parts";
		case MiscSub::kTech: return "Tech / Parts";
		case MiscSub::kSlates: return "Data Slates";
		case MiscSub::kFood: return "Food / Drink";
		case MiscSub::kArtifacts: return "Artifacts";
		case MiscSub::kAnimal: return "Animal Parts";
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

	// -----------------------------------------------------------------------
	// OSF front-end adapter (src/OsfFrontend.cpp).
	//
	// The OSF build shows the same shop through a WebView2 page instead of the
	// ImGui overlay, so the page must drive the engine the overlay drives: the
	// same hide toggles, the same keyword/model classifiers, the same basket and
	// the same delivery path. Nothing here re-implements a decision - each call
	// lands on the function the overlay already uses, which is why the two
	// front ends cannot drift into showing different catalogs.
	//
	// All of these run on the game main thread, which is where OSF UI dispatches
	// its request callbacks.
	// -----------------------------------------------------------------------
	struct OsfCategory
	{
		int                        index{ 0 };
		std::string                name;
		std::vector<std::string>   subs;
	};

	struct OsfPluginRow
	{
		std::string name;
		std::string kind;  // ESM / ESP / ESL, from the file extension
	};

	struct OsfItemRow
	{
		std::uint32_t formID{ 0 };
		std::string   name;
		std::string   edid;
		std::string   type;    // row type name, e.g. "Weapon"
		std::string   sub;     // the bucket the row sorts into, e.g. "Rifles"
		std::string   plugin;
		std::uint32_t value{ 0 };
		float         weight{ 0.0f };
		// Whether the overlay's star list holds this record. The same list, so a
		// star set in either front end is set in both.
		bool          favourite{ false };
		// How many catalog records this row stands for. 1 for almost every row;
		// more when the same name and stats are defined twice (a master's item
		// beside a mod's copy of it, or Starfield's pre-ammo-weight record beside
		// the current one). They are separate records with separate FormIDs, not
		// a listing fault, so the page says so instead of hiding the second one.
		std::uint32_t copies{ 1 };
	};

	struct OsfBasketRow
	{
		std::uint32_t formID{ 0 };
		std::string   name;
		std::string   sub;
		std::int32_t  count{ 1 };
		std::uint32_t unitPrice{ 0 };
		// Unit weight; the page multiplies by count. Credits alone made a
		// load-out that would not fit in cargo look free.
		float         weight{ 0.0f };
		std::size_t   omods{ 0 };
	};

	// One selector entry: a weapon part in its slot, a quality tier, or a
	// rarity sentinel (0 / Rare / Epic / Legendary).
	struct OsfOption
	{
		std::string   slot;
		std::string   label;
		std::uint32_t fid{ 0 };
		bool          selected{ false };
		bool          isDefault{ false };
	};

	struct OsfDetail
	{
		bool        ok{ false };
		OsfItemRow  row;
		std::string keywords;
		std::string model;
		std::string formType;  // raw record type: WEAP / ARMO / MISC ...
		std::vector<OsfOption> mods;      // weapon parts, in slot order
		std::vector<OsfOption> qualities;
		std::vector<OsfOption> rarities;
		std::string            rarityNote;
		// The other records that share this item's name and stats, so the page
		// can list and select them. Empty for an item defined once.
		std::vector<OsfItemRow> duplicates;
	};

	class ShopBrowser
	{
	public:
		static ShopBrowser* GetSingleton();

		// ---- OSF front-end adapter (implemented in ShopExplorer.cpp) ---------
		// Builds the catalog the way Open() does, minus the overlay-only side
		// effects: the OSF host owns the pause (manifest pausesGame) and there is
		// no ImGui window to size or theme.
		void OsfBeginSession();
		void OsfRefresh();
		// The OSF host owns opening and closing: the view is registered with OSF
		// UI and is opened from its interface list or by an OSF-dispatched key,
		// and OSF's Escape/Back closes it. The page reports which of those two
		// states the view is in and the engine session follows, so the shop can
		// never be half open (session closed, view on screen) the way a second,
		// local key poll made it.
		void OsfViewShown();
		void OsfViewHidden();
		[[nodiscard]] bool OsfViewIsOpen() const { return open.load(std::memory_order_acquire); }
		// Category tree: display name plus its sub-buckets, in the same order and
		// wording the overlay's category rail uses. `index` is the BrowseType
		// value, so it is what the page sends back in a filter.
		static std::vector<OsfCategory> OsfCategoryTree();
		std::vector<OsfPluginRow> OsfPlugins();
		void OsfSetFilter(int a_type, int a_sub, int a_plugin, bool a_hideTest, bool a_hideHair,
			bool a_hideCrew, bool a_hideTurrets, bool a_hideBug, bool a_favouritesOnly,
			bool a_hideDuplicates);
		// The page's stars and the overlay's star column are one list, held and
		// persisted by the same code; the page only ever asks to flip one id.
		std::vector<std::uint32_t> OsfFavourites();
		bool OsfToggleFavourite(std::uint32_t a_formID, bool& a_nowFavourite);
		// Applies the search and the requested window; returns the total matches.
		// The sort is the page's header click, and it applies to the whole
		// filtered catalog rather than the page: column 0 name, 1 shelf, 2 type,
		// 3 value, 4 weight, 5 FormID, -1 catalog order (the default).
		std::size_t OsfPage(const std::string& a_search, std::size_t a_offset, std::size_t a_limit,
			int a_sortColumn, bool a_sortAscending, std::vector<OsfItemRow>& a_out);
		// The page's sort, kept in the same INI the overlay keeps its own in, so
		// the page has the memory its headers did not have until 0.9.0-OSF.
		// Column numbers are the page's own: 0 name, 1 shelf, 2 type, 3 value,
		// 4 weight, 5 FormID, -1 catalogue order.
		static int  OsfStoredSortColumn();
		static bool OsfStoredSortAscending();
		static void OsfStoreSort(int a_column, bool a_ascending);
		std::vector<OsfBasketRow> OsfBasket();
		std::uint32_t OsfCredits();
		std::uint32_t OsfBasketCost(bool a_free);
		bool OsfBasketAdd(std::uint32_t a_formID, std::int32_t a_count, std::string& a_error);
		bool OsfBasketSet(std::uint32_t a_formID, std::int32_t a_count);
		bool OsfBasketRemove(std::size_t a_index);
		void OsfBasketClear();
		bool OsfAccept(bool a_free, std::vector<std::string>& a_lines);
		OsfDetail OsfDetailOf(std::uint32_t a_formID);
		bool OsfSetMod(std::uint32_t a_formID, const std::string& a_slot, std::uint32_t a_fid);
		bool OsfSetQuality(std::uint32_t a_formID, std::uint32_t a_fid);
		bool OsfSetRarity(std::uint32_t a_formID, std::uint32_t a_fid);
		// Delivery/refusal messages produced since the last call, so a failed
		// transfer can be shown in the page instead of only in the log.
		std::vector<std::string> OsfTakeMessages();

		// ---- OSF page extras: 3D preview source, audio, themes ---------------
		// The page cannot sample a D3D12 texture, so it asks for the model paths
		// behind one record and the native preview host renders them offscreen
		// into a PNG. Paths come from the same FormModelPaths the overlay uses,
		// so an assembled weapon previews the parts the overlay would show.
		struct OsfPreviewSource
		{
			bool                     ok{ false };
			std::uint32_t            formID{ 0 };
			std::string              label;
			std::vector<std::string> paths;
			bool                     weapon{ false };
			std::string              error;
		};
		OsfPreviewSource OsfPreviewSourceOf(std::uint32_t a_formID);

		// Music and sound controls, mirroring the overlay's Audio popup: the
		// same [audio] keys, the same volume semantics, and the same "(theme
		// track)" meaning for an empty track name.
		struct OsfAudioState
		{
			int                      musicVolume{ 45 };
			int                      sfxVolume{ 55 };
			bool                     musicMuted{ false };
			bool                     sfxMuted{ false };
			std::string              track;   // chosen file, empty = theme track
			std::vector<std::string> tracks;  // Audio\*.wav, ui_* excluded
			// True while `track` names a soundtrack folder rather than one file.
			// The page shows its previous/next buttons only then, because a lone
			// file loops and has nothing to step to.
			bool playlist{ false };
		};
		OsfAudioState OsfAudio();
		void OsfSetAudio(int a_musicVolume, int a_sfxVolume, bool a_musicMuted, bool a_sfxMuted,
			const std::string& a_track, bool a_persist);

		// The custom theme as the page edits it. The overlay's CUSTOMISE window
		// keeps all nineteen colours and the layout presets; the page renders a
		// subset of them, so the page gets the subset rather than nineteen
		// pickers it cannot show. Saving from either side writes the same
		// [custom] / [customcolours] keys, so the two editors agree.
		struct OsfCustomThemeState
		{
			std::string                                      name;
			std::string                                      suffix;   // backdrop suffix, empty = shared art
			std::string                                      music;
			std::string                                      layout;
			std::vector<std::pair<std::string, std::string>> colours;  // key -> #rrggbb
		};
		OsfCustomThemeState OsfCustomTheme();
		bool OsfSetCustomTheme(const std::string& a_name, const std::string& a_suffix,
			const std::vector<std::pair<std::string, std::string>>& a_colours);

		// The nine themes with the palette the page needs, plus lock state and
		// progress for the three earned ones. Colours travel as #rrggbb so the
		// page cannot drift from the overlay's palette.
		struct OsfThemeRow
		{
			int         index{ 0 };
			std::string name;
			std::string suffix;
			// True for the one theme the page can edit. Its index and its name
			// are both things the player can change, so the page is told
			// outright which card opens the editor.
			bool        custom{ false };
			bool        premium{ false };
			bool        unlocked{ true };
			bool        active{ false };
			std::string progress;  // "12000 / 25000 credits", empty when unlocked
			std::string accent;
			std::string accentBright;
			std::string gold;
			std::string panel;
			std::string panelAlt;
			std::string window;
			std::string ink;
			std::string muted;
			std::string border;
		};
		std::vector<OsfThemeRow> OsfThemes();
		bool OsfSetTheme(int a_index);

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
		// Folded search identity per row, built once per catalog rebuild. The
		// overlay's item-pane search used to derive it per row per frame.
		void EnsureSearchKeys();
		void SelectCurrent();
		void ConfirmAdd();
		void ConfirmBasket();
		void Back();
		void StepList(int delta);
		void Notify(const std::string& text) const;
		// The plugin's own SFShopExplorer.ini, resolved once beside the DLL.
		static const std::filesystem::path& IniPath();
		// Only the OSF page fills notifyLines; the overlay ignores it. The capture
		// is in Notify() so every refusal path is reported without touching a
		// single one of its call sites.
		void CaptureNotify(const std::string& a_text) const;

		// Persists a [shop] bool immediately (fullscreen).
		static void SaveShopIniBool(const wchar_t* key, bool value);
		static bool LoadShopIniBool(const wchar_t* key, bool defaultValue);
		static int LoadShopIniInt(const wchar_t* section, const wchar_t* key, int defaultValue);
		static std::string LoadShopIniString(const wchar_t* section, const wchar_t* key);
		static void SaveShopIniInt(const wchar_t* section, const wchar_t* key, int value);
		static void SaveShopIniString(const wchar_t* section, const wchar_t* key, const std::string& value);

		// ---- Favourites ------------------------------------------------------
		// Starred records live in their own file beside the INI, one 8-digit
		// FormID per line. Not the INI: a star is catalog content rather than a
		// setting, and a profile that stars a few hundred records would run into
		// the profile-string length limit. One FormID per line also means a
		// dropped plugin shows up as an id that no longer resolves instead of
		// silently shifting every later entry.
		static const std::filesystem::path& FavouritesPath();
		void LoadFavourites();
		void SaveFavourites();
		[[nodiscard]] bool IsFavourite(RE::TESFormID a_formID) const;
		void ToggleFavourite(RE::TESFormID a_formID, const std::string& a_label);

		// Audio preferences live under [audio]: musicVolume, sfxVolume,
		// musicMuted, sfxMuted and musicTrack. Volumes and mutes are applied to
		// ShopAudio on every change so a drag is audible immediately, but only
		// the slider release writes the INI - a drag is not 200 settings writes.
		void LoadAudioSettings();
		void SaveAudioSettings();
		void ApplyAudioSettings();
		// The track the shop should play: the chosen file, else the theme's own.
		[[nodiscard]] std::string MusicTrackFile() const;
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
		bool hideReportBug{ true };   // records whose own name carries the author's "(report this as a bug)" marker
		// Collapse records the game defines more than once (same type, name and
		// stats) to one row. Distinct records - separate FormIDs, both spawnable -
		// so this is a display choice, not a filter on what exists: the row keeps
		// the count and the item pane lists the siblings.
		bool hideDuplicateNames{ true };
		// Every member FormID of a duplicate group, mapped to the whole group. The
		// first entry is the record the row stands for. Rebuilt with the catalog.
		std::unordered_map<std::uint32_t, std::vector<RE::TESFormID>> duplicateGroupOf;
		// Favourites: the star column writes to favourites, and the Favourites
		// filter in the plugins pane turns the item pane into that list (every
		// plugin, every category - a want-list is not a shelf).
		bool favouritesOnly{ false };
		std::vector<RE::TESFormID> favourites;
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
		// Item-pane search state: one folded key per row (the OSF adapter's
		// osfSearchKeys is the same idea for its side), and the visible-row list
		// rebuilt only when the input it derives from changes. Both used to be
		// recomputed inside DrawImGui, i.e. 60 times a second while a term was
		// typed.
		std::vector<std::string> searchKeys;
		// NAME sort key per row, built once per list. Both it and searchKeys are
		// keyed on formsStamp - the counter only BuildFormList bumps - and not on
		// listStamp, which ApplyViewOrder bumps on every header click: a sort
		// changes the order of the rows, never the rows themselves.
		std::vector<std::string> nameSortKeys;
		std::uint64_t listStamp{ 1 };      // bumped by BuildFormList / ApplyViewOrder
		std::uint64_t formsStamp{ 1 };     // bumped by BuildFormList only
		std::uint64_t searchKeysStamp{ 0 };
		std::uint64_t nameSortKeysStamp{ 0 };
		std::uint64_t visRowsStamp{ 0 };
		std::vector<std::size_t> visRows;
		// OSF row order. The page asks for one window at a time, but a sort has
		// to be applied to the whole list before the window is cut, so the order
		// is built once per (column, direction, list) and paged afterwards.
		std::vector<std::size_t> osfOrder;
		std::uint64_t osfOrderStamp{ 0 };
		int osfOrderColumn{ -1 };
		bool osfOrderAscending{ true };
		std::string visRowsSearch;
		std::atomic<std::uint32_t> walletCredits{ 0 };
		bool fullscreen{ true };		// start maximized: the window must fit every screen on open
		int musicVolume{ 45 };          // [audio] 0-100
		int sfxVolume{ 55 };            // [audio] 0-100
		bool musicMuted{ false };
		bool sfxMuted{ false };
		std::string musicTrack;         // [audio] musicTrack; empty = the theme's own
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

		// OSF adapter state. Search keys are per-form folded strings built once
		// per catalog rebuild and only when the page actually searches, because
		// building them costs a display-name and model lookup per record.
		std::vector<std::string> osfSearchKeys;
		bool osfSearchDirty{ true };
		// One catalog build per view session: the page asks for the catalog
		// before its shown report can arrive, and the shown report itself calls
		// this path, so both must land on the same work.
		bool osfSessionBegun{ false };
		mutable std::mutex notifyMutex;
		mutable std::vector<std::string> notifyLines;

		std::atomic_bool wasToggle{ false };
		// Open watchdog. Set when a session opens, cleared on the first drawn
		// frame; if it survives the grace period the overlay never presented, so
		// the session is closed again instead of leaving the world frozen.
		bool drawWatchArmed{ false };
		std::uint64_t drawWatchTick{ 0 };
		std::uint64_t drawWatchFrames{ 0 };
		bool wasPageUp{ false };
		bool wasUp{ false };
		bool wasDown{ false };
		bool wasEnter{ false };
		bool wasEsc{ false };
		bool wasLeft{ false };
		bool wasRight{ false };
	};
}
