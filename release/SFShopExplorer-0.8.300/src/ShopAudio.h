#pragma once

#include <cstdint>

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

	// Music ducking while a native menu owns the screen. 0 = silent, 1 = full.
	void SetMusicVolume(float a_volume);
	void SetSfxVolume(float a_volume);

	[[nodiscard]] float GetMusicVolume();
	[[nodiscard]] float GetSfxVolume();
	[[nodiscard]] bool MusicIsPlaying();
	[[nodiscard]] const char* LastError();
}