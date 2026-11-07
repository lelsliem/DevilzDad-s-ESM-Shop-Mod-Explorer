#include "ShopPause.h"

#include "PCH.h"

#include "BuildGate.h"

#include <atomic>
#include <filesystem>
#include <format>
#include <intrin.h>

#include <windows.h>

// Only what is needed: UI::GetSingleton(), and the setting accessors used to
// duck the game's audio. RE/M/Main.h drags in SceneGraph -> NiCamera, which is an
// incomplete type here and does not compile in isolation.
#include "RE/S/Setting.h"
#include "RE/U/UI.h"

#include "REL/Relocation.h"

namespace RE
{
	class Main;
}

namespace SFE::ShopPause
{
	namespace
	{
		// ---------------------------------------------------------------------
		// Addresses verified against Starfield 1.16.244 and cross-checked
		// against the official versionlib-1-16-244-0.bin:
		//   REL::ID::UI::Singleton   (937580) -> RVA 0x05FD9B80
		//   REL::ID::Main::Singleton (883591) -> RVA 0x05E71710
		// Singletons are read through REL::ID so a game patch relocates them.
		// The bare globals below have no REL::ID and are therefore build-specific.
		//
		// Build-specific is the operative word: on any build other than the one
		// they were read from, kUIPauseCounterOffset writes a counter into
		// whatever object the UI singleton now is, and the delta/gate reads
		// sample unrelated memory. Every use below is behind
		// BuildGate::Verified(), which is fail-closed - see BuildGate.h.
		// ---------------------------------------------------------------------
		constexpr std::uintptr_t kUIPauseCounterOffset = 0x4B4;
		constexpr std::uintptr_t kMainSingletonRva = 0x5E71710;  // == REL::ID::Main::Singleton 883591
		constexpr std::uintptr_t kMainPausedOffset = 0x448;
		constexpr std::uintptr_t kWorldDeltaRva = 0x5E716F0;
		constexpr std::uintptr_t kRealDeltaRva = 0x5FBF9A8;
		constexpr std::uintptr_t kModeByteRva = 0x5E6D5BB;
		constexpr std::uintptr_t kBlockingGateRva = 0x5FB4B78;

		std::uint8_t* ImageBase()
		{
			return reinterpret_cast<std::uint8_t*>(GetModuleHandleW(L"Starfield.exe"));
		}

		std::atomic_bool g_frozen{ false };
		std::atomic_bool g_warnedMissingUI{ false };
		std::atomic<bool> g_diag{ false };

		void LoadDiagSetting()
		{
			HMODULE self = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&LoadDiagSetting), &self);
			wchar_t modulePath[MAX_PATH]{};
			GetModuleFileNameW(self, modulePath, MAX_PATH);
			const auto ini = std::filesystem::path(modulePath).parent_path() / L"SFShopExplorer.ini";
			g_diag.store(GetPrivateProfileIntW(L"audio", L"pauseDiag", 0, ini.c_str()) != 0, std::memory_order_release);
		}

		// Set once per open so the ring buffer covers the transition.
		std::int32_t* CounterPtr()
		{
			auto* const ui = RE::UI::GetSingleton();
			if (!ui) {
				if (!g_warnedMissingUI.exchange(true, std::memory_order_acq_rel)) {
					REX::WARN("SFShopExplorer: UI singleton unavailable; pause request dropped");
				}
				return nullptr;
			}
			g_warnedMissingUI.store(false, std::memory_order_release);
			return reinterpret_cast<std::int32_t*>(reinterpret_cast<std::uint8_t*>(ui) + kUIPauseCounterOffset);
		}

		// Enforce a floor rather than hard-setting the counter. Main::Update reads
		// this with an unsigned `ja` (cmp dword [rbx+0x4B4], 0 / ja), so any
		// non-zero value pauses; a floor keeps a genuine player pause (the game's
		// own +1) intact while still restoring ours if the engine decrements it.
		// Compare-and-swap a signed 32-bit field. The intrinsic is typed on
		// `long volatile*`, so the cast is unavoidable; signedness is preserved
		// because the counter is a nesting depth and is compared signed against
		// our floor before we touch it.
		std::int32_t CompareExchangeCounter(std::int32_t* a_field, std::int32_t a_exchange, std::int32_t a_compare)
		{
			return static_cast<std::int32_t>(_InterlockedCompareExchange(
				reinterpret_cast<volatile long*>(a_field),
				static_cast<long>(a_exchange),
				static_cast<long>(a_compare)));
		}

		void RaiseCounterToFloor(std::int32_t a_floor)
		{
			// The floor is written into the UI singleton at an offset only
			// verified for the build the plugin was built against. On any other
			// build this would write into an unrelated object.
			if (!BuildGate::Verified()) {
				BuildGate::Refuse("world freeze (UI pause counter)");
				return;
			}
			auto* const c = CounterPtr();
			if (!c) {
				return;
			}
			auto value = *c;
			for (;;) {
				if (value >= a_floor) {
					return;
				}
				const auto prev = CompareExchangeCounter(c, a_floor, value);
				if (prev == value) {
					return;
				}
				value = prev;
			}
		}

		struct Snapshot
		{
			void*       ui{ nullptr };
			std::int32_t counter{ -1 };
			std::uint8_t paused{ 0xFF };
			float       worldDelta{ -1.0f };
			float       realDelta{ -1.0f };
			std::uint8_t modeByte{ 0xFF };
			void*       blockingGate{ nullptr };
		};

		Snapshot Take()
		{
			Snapshot s{};
			s.ui = RE::UI::GetSingleton();
			// Diagnostics only, but every address below is build-specific; a
			// sample of unrelated memory published as "the pause state" is worse
			// than no sample at all.
			if (!BuildGate::Verified()) {
				return s;
			}
			const auto* base = ImageBase();
			s.counter = s.ui ? *reinterpret_cast<const std::int32_t*>(reinterpret_cast<const std::uint8_t*>(s.ui) + kUIPauseCounterOffset) : -1;
			// Read the singleton straight from its global rather than through
			// RE::Main: the REL is verified to resolve to RVA 0x05E71710, but
			// including RE/M/Main.h here pulls in an incomplete NiCamera.
			if (base) {
				if (auto* const main = *reinterpret_cast<void* const*>(base + kMainSingletonRva)) {
					s.paused = reinterpret_cast<const std::uint8_t*>(main)[kMainPausedOffset];
				}
			}
			if (base) {
				s.worldDelta = *reinterpret_cast<const float*>(base + kWorldDeltaRva);
				s.realDelta = *reinterpret_cast<const float*>(base + kRealDeltaRva);
				s.modeByte = base[kModeByteRva];
				s.blockingGate = *reinterpret_cast<void* const*>(base + kBlockingGateRva);
			}
			return s;
		}

		// Ring buffer of the frames around the toggle. A single sample cannot
		// distinguish "counter dropped" from "counter held but the consumer wants
		// something else" because isGameMenuPaused and the delta are transient.
		struct Sample
		{
			std::int32_t counter{ -1 };
			std::uint8_t paused{ 0xFF };
			std::uint8_t modeByte{ 0xFF };
			float       worldDelta{ -1.0f };
			float       realDelta{ -1.0f };
			bool        blocking{ false };
			bool        uiValid{ false };
		};

		// Running aggregates rather than a ring buffer dumped on close. The first
		// diagnostic run produced nothing because the game was killed with the
		// shop still open, so the dump never fired; periodic summaries survive an
		// abrupt exit and are far cheaper in log volume than one line per frame.
		struct Stats
		{
			std::int32_t   ctrMin{ 0 };
			std::int32_t   ctrMax{ 0 };
			std::uint8_t  pausedMin{ 0xFF };
			std::uint8_t  pausedMax{ 0 };
			float         worldDeltaMin{ 0.0f };
			float         worldDeltaMax{ 0.0f };
			float         realDeltaMax{ 0.0f };
			std::uint8_t  modeByte{ 0xFF };
			bool          blocking{ false };
			bool          uiValid{ false };
			std::uint32_t frames{ 0 };

			void Add(const Sample& s)
			{
				if (frames == 0) {
					ctrMin = ctrMax = s.counter;
					pausedMin = pausedMax = s.paused;
					worldDeltaMin = worldDeltaMax = s.worldDelta;
					realDeltaMax = s.realDelta;
					modeByte = s.modeByte;
					blocking = s.blocking;
					uiValid = s.uiValid;
				} else {
					ctrMin = std::min(ctrMin, s.counter);
					ctrMax = std::max(ctrMax, s.counter);
					pausedMin = std::min(pausedMin, s.paused);
					pausedMax = std::max(pausedMax, s.paused);
					worldDeltaMin = std::min(worldDeltaMin, s.worldDelta);
					worldDeltaMax = std::max(worldDeltaMax, s.worldDelta);
					realDeltaMax = std::max(realDeltaMax, s.realDelta);
				}
				++frames;
			}

			void Log(const char* a_label) const
			{
				const auto mode = modeByte == 0xFF ? std::string("?") : std::to_string(modeByte);
				REX::INFO("SFShopExplorer: [pausediag] {} frames={} ui={} ctr=[{}..{}] isGameMenuPaused=[0x{:02X}..0x{:02X}] worldDelta=[{:.6f}..{:.6f}] realDeltaMax={:.6f} modeByte={} blockingGate={}",
					a_label, frames, uiValid ? "ok" : "NULL", ctrMin, ctrMax, pausedMin, pausedMax, worldDeltaMin, worldDeltaMax,
					realDeltaMax, mode.c_str(), blocking ? "NON-NULL" : "null");
			}
		};

		Stats          g_stats{};
		std::uint32_t g_sinceReport{ 0 };

		void Record()
		{
			const auto s = Take();
			Sample sample;
			sample.counter = s.counter;
			sample.paused = s.paused;
			sample.modeByte = s.modeByte;
			sample.worldDelta = s.worldDelta;
			sample.realDelta = s.realDelta;
			sample.blocking = s.blockingGate != nullptr;
			sample.uiValid = s.ui != nullptr;
			g_stats.Add(sample);

			// Roughly every 2s at 60fps.
			if (++g_sinceReport >= 120) {
				g_sinceReport = 0;
				g_stats.Log("live");
				g_stats = Stats{};
			}
		}
	}

	void SetDiag(bool a_enabled)
	{
		g_diag.store(a_enabled, std::memory_order_release);
	}

	void Tick()
	{
		// The counter hold runs whether or not diagnostics are on: it is the
		// actual freeze attempt. Diagnostics only add the sampling.
		if (g_frozen.load(std::memory_order_acquire)) {
			RaiseCounterToFloor(1);
		}
		if (g_diag.load(std::memory_order_acquire)) {
			Record();
		}
	}

	void SetFrozen(bool a_frozen)
	{
		// Duck the game's own mix alongside the freeze: Wwise never consults the
		// pause counter, so ambience would otherwise keep playing at full volume
		// under the shop's music.
		SetGameAudioMuted(a_frozen);

		static std::atomic_bool diagRead{ false };
		if (!diagRead.exchange(true, std::memory_order_acq_rel)) {
			LoadDiagSetting();
		}
		if (g_frozen.load(std::memory_order_acquire) == a_frozen) {
			return;
		}
		// Playback ducking above uses REL::IDs only and is safe anywhere; the
		// counter itself is not. Refuse the freeze outright rather than leaving
		// the UI claiming a world freeze that never happened.
		if (a_frozen && !BuildGate::Verified()) {
			BuildGate::Refuse("world freeze (UI pause counter)");
			return;
		}
		g_frozen.store(a_frozen, std::memory_order_release);

		if (a_frozen) {
			// Re-assert every frame from Tick(). A single write is not enough: the
			// engine's own menu bookkeeping touches this counter and can cancel a
			// lone increment between frames.
			g_stats = Stats{};
			g_sinceReport = 0;
			RaiseCounterToFloor(1);
			REX::INFO("SFShopExplorer: world frozen request (UI+0x{:X} floor=1, diag={})", kUIPauseCounterOffset,
				g_diag.load(std::memory_order_acquire));
			return;
		}

		// Release: drop our contribution back to 0, which is exactly what the game
		// itself would have after a decrement.
		if (auto* const c = CounterPtr()) {
			auto value = *c;
			for (;;) {
				if (value <= 0) {
					break;
				}
				const auto prev = CompareExchangeCounter(c, 0, value);
				if (prev == value) {
					break;
				}
				value = prev;
			}
		}
		if (g_diag.load(std::memory_order_acquire) && g_stats.frames > 0) {
			g_stats.Log("resume");
		}
		g_stats = Stats{};
		g_sinceReport = 0;
		REX::INFO("SFShopExplorer: world resumed (UI+0x{:X} cleared; Main+0x{:X} is game-derived)", kUIPauseCounterOffset,
			kMainPausedOffset);
	}

	bool IsFrozen()
	{
		return g_frozen.load(std::memory_order_acquire);
	}

	// -----------------------------------------------------------------------
	// Game audio ducking.
	//
	// Probing Starfield.exe shows the engine drives its own mix from live
	// game settings rather than through a single master-volume call:
	//   fAudioVolumeMaster / fAudioVolumeMusic / fAudioVolumeVoice /
	//   fAudioVolumeEffects   (all ":Audio" settings, present as literal keys
	//                          in the exe alongside the RTPC name
	//                          "AudioSettings_Bus_Volume_1Master")
	//
	// So the reliable mute is to write those settings in memory. Both entry
	// points are verified REL::IDs (GameSettingCollection::Singleton 938225,
	// GetSetting 49324) and Setting::SetFloat is a union store, so no address
	// is guessed. WriteSetting/WriteSettings are deliberately NOT called: that
	// would persist the change to the player's profile.
	// -----------------------------------------------------------------------
	namespace
	{
		RE::Setting* FindAudioSetting(const char* a_key)
		{
			using func_t = RE::Setting* (*)(RE::GameSettingCollection*, const char*);
			static REL::Relocation<func_t> func{ RE::ID::GameSettingCollection::GetSetting };

			static REL::Relocation<RE::GameSettingCollection**> singleton{ RE::ID::GameSettingCollection::Singleton };
			auto* const collection = *singleton;
			if (!collection) {
				return nullptr;
			}
			return func(collection, a_key);
		}

		struct Ducked
		{
			RE::Setting* setting{ nullptr };
			float         original{ 0.0f };
		};

		// The values the player's profile held, captured on first mute so resume
		// restores exactly what was there rather than a guessed default.
		std::mutex   g_duckMutex;
		Ducked       g_ducked[4]{};
		std::size_t g_duckedCount{ 0};
		std::atomic_bool g_muted{ false };

		constexpr const char* kVolumeKeys[4] = {
			"fAudioVolumeMaster", "fAudioVolumeMusic", "fAudioVolumeVoice", "fAudioVolumeEffects"
		};
	}

	bool SetGameAudioMuted(bool a_muted)
	{
		if (a_muted == g_muted.load(std::memory_order_acquire)) {
			return true;
		}

		if (a_muted) {
			std::scoped_lock lock(g_duckMutex);
			g_duckedCount = 0;
			for (const auto* key : kVolumeKeys) {
				auto* const setting = FindAudioSetting(key);
				if (!setting || !setting->Is<float>()) {
					REX::WARN("SFShopExplorer: audio setting '{}' not found; that channel stays live", key);
					continue;
				}
				g_ducked[g_duckedCount++] = Ducked{ setting, setting->GetFloat() };
				setting->SetFloat(0.0f);
			}
			if (g_duckedCount == 0) {
				REX::WARN("SFShopExplorer: game-audio duck found no settings; shop audio only");
				return false;
			}
			g_muted.store(true, std::memory_order_release);
			REX::INFO("SFShopExplorer: game audio ducked via {} setting(s)", g_duckedCount);
			return true;
		}

		std::scoped_lock lock(g_duckMutex);
		for (std::size_t i = 0; i < g_duckedCount; ++i) {
			if (g_ducked[i].setting) {
				g_ducked[i].setting->SetFloat(g_ducked[i].original);
			}
		}
		REX::INFO("SFShopExplorer: game audio restored ({} setting(s))", g_duckedCount);
		g_duckedCount = 0;
		g_muted.store(false, std::memory_order_release);
		return true;
	}

	bool IsGameAudioMuted()
	{
		return g_muted.load(std::memory_order_acquire);
	}
}
