#pragma once

namespace SFE::ShopPause
{
	// Ducks the game's own audio while the shop is open, by writing the live
	// settings the engine reads for its mix
	// (fAudioVolumeMaster / Music / Voice / Effects).
	// Sound::Wwise keeps rendering through the pause counter, so without this the
	// shop music sits under full-volume ambience.
	//
	// They are INI prefs (StarfieldPrefs.ini [Audio]), not game settings:
	// 0.8.334 looked in GameSettingCollection and found nothing. Both INI
	// collections are walked by commonlib and Setting::SetFloat is an in-memory
	// union store, so no address is guessed and nothing is written to the
	// profile; the captured values are restored exactly on resume. Returns false
	// (and changes nothing) if no key resolves.
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
	// Verified from two frame sources. The 0.8.334 session held the counter for
	// 2m45s while the SFSE task stopped ticking - consistent with the world
	// being paused, but with no log line to prove it, because the task tick is
	// part of what the pause stops. 0.8.336 adds TickRender() from the Present
	// hook and two freeze-check samples: `worldDelta` read once ~1.5s and once
	// ~2.5s in, which tells "held" from "still advancing" in the log.
	//
	// Fails soft: if the UI singleton is absent the request is dropped and
	// retried next frame. Never calls an unresolved address.
	void SetFrozen(bool a_frozen);

	// Call once per frame from the SFSE task. Re-asserts the counter floor
	// while frozen, and samples diagnostics when enabled.
	void Tick();

	// Same hold from the Present hook. The task tick stops when the world is
	// paused; Present does not, so this keeps the floor asserted and makes the
	// freeze check reach its second sample.
	void TickRender();

	// Logs counter / isGameMenuPaused / world delta / real delta / mode byte /
	// blocking gate every frame into a ring buffer, dumped on unfreeze.
	void SetDiag(bool a_enabled);

	[[nodiscard]] bool IsFrozen();
}