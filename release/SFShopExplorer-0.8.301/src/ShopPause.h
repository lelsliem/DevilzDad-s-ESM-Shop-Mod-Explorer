#pragma once

namespace SFE::ShopPause
{
	// Ducks the game's own audio while the shop is open, by writing the live
	// game settings the engine already reads every frame
	// (fAudioVolumeMaster / Music / Voice / Effects, all ":Audio" settings).
	// Sound::Wwise keeps rendering through the pause counter, so without this the
	// shop music sits under full-volume ambience.
	//
	// Uses only verified REL::IDs (GameSettingCollection::Singleton / GetSetting)
	// and the Setting union, which SetFloat writes in memory. Nothing is written
	// to the profile, so the player's real volumes are untouched and restored
	// exactly on resume. Returns false (and changes nothing) if any lookup fails.
	bool SetGameAudioMuted(bool a_muted);

	[[nodiscard]] bool IsGameAudioMuted();
	// Freezes / resumes world simulation by holding the UI menu pause counter
	// (UI + 0x4B4) at a floor of 1.
	//
	// Why the counter and not Main::isGameMenuPaused (Main + 0x448): the game
	// derives that byte from the counter inside one function and consumes it
	// 0x56 bytes later, so a plugin write has no window to survive. The counter
	// is read at the top of that function, before the byte is written, so a
	// value set from outside survives to be consumed next frame.
	//
	// STATUS: not yet confirmed to actually stop the world. The counter write
	// was observed to persist (it read back as 1 for a full 4-minute shop
	// session) yet the world kept moving, so something downstream is still
	// unknown. Enable the diagnostic below to find out what.
	//
	// Fails soft: if the UI singleton is absent the request is dropped and
	// retried next frame. Never calls an unresolved address.
	void SetFrozen(bool a_frozen);

	// Call once per frame from the SFSE task. Re-asserts the counter floor
	// while frozen, and samples diagnostics when enabled.
	void Tick();

	// Logs counter / isGameMenuPaused / world delta / real delta / mode byte /
	// blocking gate every frame into a ring buffer, dumped on unfreeze.
	void SetDiag(bool a_enabled);

	[[nodiscard]] bool IsFrozen();
}