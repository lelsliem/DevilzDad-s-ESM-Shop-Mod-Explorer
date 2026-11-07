#include "ShopPause.h"

#include "PCH.h"

#include "BuildGate.h"
#include "ShopAudio.h"
#include "ShopExplorer.h"  // kIniFileName: each front end reads its own ini

#include <atomic>
#include <cmath>
#include <filesystem>
#include <format>
#include <intrin.h>

#include <windows.h>

// Only what is needed: UI::GetSingleton(), and the setting accessors used to
// duck the game's audio. RE/M/Main.h drags in SceneGraph -> NiCamera, which is an
// incomplete type here and does not compile in isolation.
#include "RE/S/Setting.h"
#include "RE/U/UI.h"
// fAudioVolume* lives in StarfieldPrefs.ini, so the settings the audio engine
// exposes are INI pref settings, not game settings. Looking them up in
// GameSettingCollection is why 0.8.334 reported "not found" for all four keys
// while the ambience kept playing.
#include "RE/I/INIPrefSettingCollection.h"
#include "RE/I/INISettingCollection.h"

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
		// Two INFO lines per freeze, ~1.5s and ~2.5s in: the counter, the game's
		// own pause flag and the world/real deltas. Sound continuing is not
		// evidence the world is still running - Wwise ignores the pause counter -
		// and a single delta sample cannot answer "is it paused?" either,
		// because a paused frame may simply carry the previous delta. The second
		// sample is what makes it a measurement: a world clock that has stopped
		// produces the same delta twice.
		std::atomic<std::uint32_t> g_freezeCheckFrame{ 0 };
		std::atomic_bool g_freezeCheckDone{ false };
		std::atomic_bool g_freezeFirstSample{ false };
		float          g_freezeFirstDelta{ 0.0f };
		std::int32_t   g_freezeFirstCounter{ -1 };
		std::uint8_t   g_freezeFirstPaused{ 0xFF };

		void LoadDiagSetting()
		{
			HMODULE self = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&LoadDiagSetting), &self);
			wchar_t modulePath[MAX_PATH]{};
			GetModuleFileNameW(self, modulePath, MAX_PATH);
			const auto ini = std::filesystem::path(modulePath).parent_path() / kIniFileName;
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

		// Frames, not seconds, because this runs on whichever frame source is
		// alive (task thread or Present) and both are ~60-120 Hz here.
		constexpr std::uint32_t kFirstSampleFrame = 90;
		constexpr std::uint32_t kSecondSampleFrame = 150;

		void FreezeCheckStep()
		{
			if (g_freezeCheckDone.load(std::memory_order_acquire)) {
				return;
			}
			const auto frame = g_freezeCheckFrame.fetch_add(1, std::memory_order_acq_rel) + 1;
			if (frame == kFirstSampleFrame) {
				const auto s = Take();
				g_freezeFirstDelta = s.worldDelta;
				g_freezeFirstCounter = s.counter;
				g_freezeFirstPaused = s.paused;
				g_freezeFirstSample.store(true, std::memory_order_release);
				REX::INFO("SFShopExplorer: freeze check t0 ctr={} isGameMenuPaused=0x{:02X} worldDelta={:.6f} realDelta={:.6f} modeByte=0x{:02X} blockingGate={}",
					s.counter, s.paused, s.worldDelta, s.realDelta, s.modeByte,
					s.blockingGate ? "NON-NULL" : "null");
				return;
			}
			if (frame < kSecondSampleFrame) {
				return;
			}
			g_freezeCheckDone.store(true, std::memory_order_release);
			const auto s = Take();
			const bool first = g_freezeFirstSample.load(std::memory_order_acquire);
			const float moved = first ? s.worldDelta - g_freezeFirstDelta : 0.0f;
			const bool deltaHeld = first && std::abs(moved) <= 1e-7f;
			REX::INFO("SFShopExplorer: freeze check t1 {} ctr={} (t0 {}) isGameMenuPaused=0x{:02X} worldDelta={:.6f} (t0 {:.6f}, moved={:.6f}) realDelta={:.6f} modeByte=0x{:02X} blockingGate={}",
				deltaHeld ? "world clock held" : "world clock still advancing",
				s.counter, g_freezeFirstCounter, s.paused, s.worldDelta, g_freezeFirstDelta, moved,
				s.realDelta, s.modeByte, s.blockingGate ? "NON-NULL" : "null");
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
			FreezeCheckStep();
		}
		if (g_diag.load(std::memory_order_acquire)) {
			Record();
		}
	}

	// Called from the Present hook as well as from the task thread. The two
	// exist because the counter being held is what stops the game's update
	// loop, and the SFSE task the tick rides on stops with it: after the freeze
	// there may be no further task ticks to re-assert the floor or to report.
	// Present keeps running while the world is paused, so it is the only place
	// that can both hold the counter and answer whether the hold worked.
	void TickRender()
	{
		if (g_frozen.load(std::memory_order_acquire)) {
			RaiseCounterToFloor(1);
			FreezeCheckStep();
		}
	}

	void SetFrozen(bool a_frozen)
	{
		// Silence the game alongside the freeze, on two independent levels:
		// the setting write (best effort; 0.8.336 proved the keys are not in
		// either INI collection on this build) and the Core Audio session mute,
		// which is what actually works because the shop has its own session.
		// Wwise ignores the pause counter, so without this the ambience plays
		// at full volume under the shop's music.
		SetGameAudioMuted(a_frozen);
		ShopAudio::SetGameSessionMuted(a_frozen);

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
		g_freezeCheckFrame.store(0, std::memory_order_release);
		g_freezeFirstSample.store(false, std::memory_order_release);
		g_freezeCheckDone.store(!a_frozen, std::memory_order_release);

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
	// settings rather than through a single master-volume call:
	//   fAudioVolumeMaster / fAudioVolumeMusic / fAudioVolumeVoice /
	//   fAudioVolumeEffects   (the keys the RTPC
	//                          "AudioSettings_Bus_Volume_1Master" family reads)
	//
	// They are INI prefs (StarfieldPrefs.ini [Audio]), not game settings, so the
	// mute writes them through INIPrefSettingCollection. Setting::SetFloat is a
	// union store, so nothing is written back to the profile file.
	// -----------------------------------------------------------------------
	namespace
	{
		// fAudioVolume* are written to StarfieldPrefs.ini under [Audio], so they
		// are INI pref settings. GameSettingCollection::GetSetting returns null
		// for every one of them (0.8.334 logged exactly that), which is why the
		// ambience never ducked. Prefs first, then the plain INI collection; both
		// lookups are commonlib virtual-free list walks, so no address is
		// guessed and a missing key simply returns null.
		// One-shot inventory of what the two INI collections actually hold.
		// 0.8.336 logged all four keys as unresolved even through the prefs
		// collection, which means either the singleton is null or the keys are
		// spelled differently on this build. The counts and any key containing
		// "udio" settle that without another guessing round.
		void ProbeSettingCollections()
		{
			static std::atomic_bool once{ false };
			if (once.exchange(true, std::memory_order_acq_rel)) {
				return;
			}
			const auto probe = [](const char* a_label, auto* a_collection) {
				if (!a_collection) {
					REX::WARN("SFShopExplorer: {} collection singleton is null", a_label);
					return;
				}
				std::size_t total = 0;
				std::size_t audio = 0;
				std::string sample;
				for (auto* setting : a_collection->settings) {
					if (!setting) {
						continue;
					}
					++total;
					const std::string key{ setting->GetKey() };
					if (key.find("udio") == std::string::npos) {
						continue;
					}
					++audio;
					if (sample.size() < 240) {
						sample += key;
						sample += ' ';
					}
				}
				REX::INFO("SFShopExplorer: {} settings={} audioKeys={} [{}]", a_label, total, audio, sample.c_str());
			};
			probe("INI prefs", RE::INIPrefSettingCollection::GetSingleton());
			probe("INI", RE::INISettingCollection::GetSingleton());
		}

		RE::Setting* FindAudioSetting(const char* a_key)
		{
			if (auto* const prefs = RE::INIPrefSettingCollection::GetSingleton()) {
				if (auto* const setting = prefs->GetSetting(a_key)) {
					return setting;
				}
			}
			if (auto* const ini = RE::INISettingCollection::GetSingleton()) {
				if (auto* const setting = ini->GetSetting(a_key)) {
					return setting;
				}
			}
			return nullptr;
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

		// Starfield does not expose the Skyrim/FO4 spelling. On 2026-10-24 all
		// four fAudioVolume* keys resolved to nothing while the ambience kept
		// playing under the shop. Both spellings are tried, and the one that
		// actually resolves is logged so the next report says which family the
		// game uses.
		struct VolumeKey
		{
			const char* primary;
			const char* alternate;
		};
		constexpr VolumeKey kVolumeKeys[4] = {
			{ "fAudioVolumeMaster", "fAudioMasterVolume" },
			{ "fAudioVolumeMusic", "fAudioMusicVolume" },
			{ "fAudioVolumeVoice", "fAudioVoiceVolume" },
			{ "fAudioVolumeEffects", "fAudioEffectsVolume" },
		};
	}

	bool SetGameAudioMuted(bool a_muted)
	{
		if (a_muted == g_muted.load(std::memory_order_acquire)) {
			return true;
		}

		if (a_muted) {
			ProbeSettingCollections();
			std::scoped_lock lock(g_duckMutex);
			g_duckedCount = 0;
			for (const auto& key : kVolumeKeys) {
				auto* setting = FindAudioSetting(key.primary);
				const char* used = key.primary;
				if (!setting || !setting->Is<float>()) {
					setting = FindAudioSetting(key.alternate);
					used = key.alternate;
				}
				if (!setting || !setting->Is<float>()) {
					// Once per channel: this was four WARN lines on every open (five
					// opens, twenty lines) for a condition that cannot change during a
					// session. The session mute is the mechanism now; this lookup is
					// only a fallback for a build that exposes the keys.
					static std::atomic_int missingLogged{ 0 };
					if (missingLogged.fetch_add(1, std::memory_order_acq_rel) < 4) {
						REX::WARN("SFShopExplorer: audio setting '{}'/'{}' not found; that channel stays live",
							key.primary, key.alternate);
					}
					continue;
				}
				const auto original = setting->GetFloat();
				g_ducked[g_duckedCount++] = Ducked{ setting, original };
				setting->SetFloat(0.0f);
				REX::INFO("SFShopExplorer: game audio duck '{}' {:.2f} -> 0.00", used, original);
			}
			if (g_duckedCount == 0) {
				static std::atomic_bool warnedNoSettings{ false };
				if (!warnedNoSettings.exchange(true, std::memory_order_acq_rel)) {
					REX::WARN("SFShopExplorer: game-audio duck found no settings; the session mute still applies");
				}
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
