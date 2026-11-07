// ============================================================================
// ShopAudio.h - the shop's audio surface
//
// Callable from any thread; the work happens on the audio thread. A missing
// WAV is reported through LastError() and never blocks the shop from opening.
// ============================================================================

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace SFE::ShopAudio
{
	enum class Sfx : std::uint8_t
	{
		kClick = 0,
		kHover,
		kSelect,
		kOpen,
		kClose,
		kDenied,
		// Taking something out: a basket line, a store back, a cleared list.
		// Newer than the other six, so the bank falls back to the click when
		// ui_remove.wav is not in the folder (an older install's Audio\).
		kRemove,
		kCount
	};

	// Loads INI, decodes the WAV bank and starts the dedicated audio worker.
	// Idempotent and fail-soft: if WASAPI or any file is unavailable the shop
	// still runs, silently. The worker pins the plugin module until it exits,
	// because this SFSE API does not provide a plugin-unload callback.
	void Init();

	// Stops the worker and releases WASAPI resources. Call only from a normal
	// thread, never from DllMain/under the Windows loader lock. If SFSE unloads
	// the plugin without an explicit call, the pinned worker is process-lifetime.
	void Shutdown();

	// Non-blocking. Safe from any thread, including the Present thread.
	void PlaySfx(Sfx a_sfx);

	// Music is a single looping stream with a fade. Re-entrant: calling Start
	// while already playing is a no-op, so an open/close/open burst cannot
	// stack voices.
	void StartMusic();
	void StopMusic();

	// Switch the track for the next (or current) playback. Takes effect on the
	// next StartMusic, and mid-playback the current track is faded out and the
	// new one faded in. Unknown files keep the current track and log.
	void SetMusicFile(const char* a_fileName);

	// Step through the selected soundtrack folder: +1 next track, -1 previous,
	// wrapping at both ends. A no-op unless the selection is a folder, so the
	// picker can call it unconditionally. Decoding happens on the audio thread
	// and the next track fades in from zero, exactly like the automatic advance
	// at the end of a track.
	void SkipMusicTrack(int a_delta);

	// True while the selection is a soundtrack folder rather than one file.
	// The picker shows its previous/next buttons only when this is true.
	[[nodiscard]] bool MusicIsPlaylist();

	// Everything the shop can play as a track: `Audio\*.wav` plus
	// `Audio\Music\*.wav`, minus the ui_* effect bank. Names are relative to
	// Audio\ (a nested folder keeps its `Music\` prefix), so the result feeds
	// straight back into SetMusicFile. Sorted, case-insensitive. A single flat
	// Audio\ folder is the deployment, so a user-dropped track needs no setup:
	// any .wav that is not named ui_* becomes a selectable track.
	[[nodiscard]] std::vector<std::string> ListMusicTracks();

	// Mutes the game's own Core Audio session while the shop is open. The shop
	// renders into its own session GUID, so this is "game silent, shop audible".
	// Applied asynchronously by the audio worker; safe from any thread, and a
	// no-op if the endpoint or session is unavailable.
	void SetGameSessionMuted(bool a_muted);

	// Music ducking while a native menu owns the screen. 0 = silent, 1 = full.
	void SetMusicVolume(float a_volume);
	void SetSfxVolume(float a_volume);

	[[nodiscard]] float GetMusicVolume();
	[[nodiscard]] float GetSfxVolume();
	[[nodiscard]] bool MusicIsPlaying();
	[[nodiscard]] const char* LastError();
}