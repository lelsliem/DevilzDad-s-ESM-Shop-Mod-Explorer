#include "ShopAudio.h"

#include "PCH.h"

// kIniFileName: the OSF build names its INI after its own DLL, so audio
// preferences are read from the file this build actually writes.
#include "ShopExplorer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <format>
#include <mutex>
#include <exception>
#include <string>
#include <utility>
#include <vector>

#include <process.h>
#include <windows.h>

#include <audioclient.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>

namespace SFE::ShopAudio
{
	namespace
	{
		struct Pcm
		{
			// Always kept as 32-bit float, interleaved. The decoder converts once
			// and the WASAPI mix format (also float on this machine) is consumed
			// directly, so playback needs no per-sample conversion at all.
			std::vector<float>              samples;
			std::uint32_t                   sampleRate{ 44100 };
			std::uint16_t                   channels{ 2 };
		};

		enum class Slot : std::uint8_t
		{
			kClick = 0,
			kHover,
			kSelect,
			kOpen,
			kClose,
			kDenied,
			kCount
		};

		constexpr std::size_t kClipCount = static_cast<std::size_t>(Slot::kCount);
		const char* const    kSfxNames[kClipCount] = {
			"ui_click.wav", "ui_hover.wav", "ui_select.wav", "ui_open.wav", "ui_close.wav", "ui_denied.wav"
		};

		// ---------------------------------------------------------------------
		// Why WASAPI and not XAudio2: on this machine XAudio2Create succeeds but
		// *every* CreateSourceVoice returns XAUDIO2_E_INVALID_CALL (0x88960001),
		// for every format, every flag, with or without a mastering voice, and
		// standalone in a plain console app with no game running. Verified with a
		// standalone probe. WASAPI shared mode opens and starts cleanly on the
		// same endpoint, so the engine choice was the whole failure - it was
		// never a race with the game's Wwise start-up.
		// ---------------------------------------------------------------------
		IAudioClient*       g_client{ nullptr };
		IAudioRenderClient* g_render{ nullptr };
		WAVEFORMATEX*       g_mixFormat{ nullptr };
		UINT32              g_bufferFrames{ 0 };

		// The current SFSE task/messaging API has no plugin-unload callback.
		// Keep the playback worker process-lifetime unless Shutdown() is called
		// from a normal thread. Pin the DLL for the entire process: SFSE unloads
		// plugins with FreeLibrary but has no unload callback, so releasing the
		// pin from inside a window-procedure callback could unmap this code while
		// that callback is still on the stack. Windows releases it at process exit.
		std::mutex              g_workerMutex;
		std::condition_variable g_workerStopped;
		std::atomic_bool        g_workerActive{ false };
		HANDLE                  g_workerHandle{ nullptr };
		HMODULE                 g_workerModulePin{ nullptr };
		std::mutex              g_mutex;
		std::condition_variable g_wake;
		std::deque<Slot>       g_pendingSfx;
		std::atomic_bool       g_quit{ false };
		std::atomic_bool       g_started{ false };
		std::atomic_bool       g_ready{ false };
		std::atomic_bool       g_failed{ false };
std::atomic_bool       g_musicRequested{ false };
		std::atomic_bool       g_musicDirty{ false };
		std::string            g_musicFile;
		std::atomic_bool       g_musicPlaying{ false };
		// Defaults lowered 2026-10-18: the stings are also generated softer
		// (see audio/make_samples.py), so this is the second half of the same
		// change. A menu should not be the loudest thing in the mix; 0.55/0.75
		// put a click above dialogue.
		std::atomic<float>     g_musicVolume{ 0.45f };
		std::atomic<float>     g_sfxVolume{ 0.55f };
		std::atomic<float>     g_musicFade{ 0.0f };
		// -1 = nothing pending, 0 = unmute, 1 = mute. Applied on the audio
		// thread, which already owns this thread's COM apartment and endpoint.
		std::atomic<int>       g_gameMuteRequest{ -1 };
		bool                   g_gameMuted{ false };

		// The shop gets its own Core Audio session so the game's default session
		// can be muted without silencing the shop. Sessions are identified by
		// GUID; a NULL GUID (the previous code) put the shop in the game's
		// session, which is why muting by session was not an option before.
		constexpr GUID kShopSessionGuid{ 0x5f2c8a11, 0x9b3e, 0x4f6a, { 0x9d, 0x2a, 0x77, 0x51, 0x3b, 0x0c, 0x6e, 0x41 } };

		std::array<Pcm, kClipCount> g_clips{};
		Pcm                         g_music{};

		std::filesystem::path g_dir;
		std::filesystem::path g_musicPath;
		std::string           g_lastError;
		std::mutex            g_errorMutex;
		bool                  g_enabled{ true };

		void SetError(std::string a_text)
		{
			REX::WARN("SFShopExplorer: {}", a_text);
			std::scoped_lock lock(g_errorMutex);
			g_lastError = std::move(a_text);
		}

		std::filesystem::path ModuleDir()
		{
			HMODULE self = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&ModuleDir), &self);
			wchar_t modulePath[MAX_PATH]{};
			GetModuleFileNameW(self, modulePath, MAX_PATH);
			return std::filesystem::path(modulePath).parent_path();
		}

		// Audio\ first, then beside the DLL, so both a tidy deployment and a
		// loose hand-dropped file work.
		std::filesystem::path ResolveClip(const wchar_t* a_name)
		{
			const auto inSub = g_dir / L"Audio" / a_name;
			if (std::filesystem::exists(inSub)) {
				return inSub;
			}
			return g_dir / a_name;
		}

		std::filesystem::path ResolveClip(const char* a_name)
		{
			wchar_t wide[MAX_PATH]{};
			if (MultiByteToWideChar(CP_UTF8, 0, a_name, -1, wide, MAX_PATH) > 0) {
				return ResolveClip(wide);
			}
			return g_dir / a_name;
		}

		// The shop ships one flat Audio\ folder: six ui_* stings and the tracks.
		// The prefix is the whole contract, so a player can drop any .wav in
		// beside them and it shows up in the track list - no second folder, no
		// manifest. Case-insensitive, because a hand-renamed UI_CLICK.WAV must
		// still be treated as a sting rather than offered as music.
		bool IsMusicTrackName(const std::filesystem::path& a_path)
		{
			auto ext = a_path.extension().wstring();
			std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) {
				return static_cast<wchar_t>(std::towlower(c));
			});
			if (ext != L".wav") {
				return false;
			}
			auto stem = a_path.filename().wstring();
			std::transform(stem.begin(), stem.end(), stem.begin(), [](wchar_t c) {
				return static_cast<wchar_t>(std::towlower(c));
			});
			return stem.rfind(L"ui_", 0) != 0;
		}

		std::string WidenToUtf8(const std::wstring& a_text)
		{
			const int len = WideCharToMultiByte(CP_UTF8, 0, a_text.c_str(), -1, nullptr, 0, nullptr, nullptr);
			if (len <= 1) {
				return {};
			}
			// len counts the terminating null, so give the buffer len bytes, write,
			// then resize past it. Writing len bytes into a len-1 string would only
			// land on the null terminator and is not worth relying on.
			std::string out(static_cast<std::size_t>(len), '\0');
			WideCharToMultiByte(CP_UTF8, 0, a_text.c_str(), -1, out.data(), len, nullptr, nullptr);
			out.resize(static_cast<std::size_t>(len - 1));
			return out;
		}

		bool ReadAllBytes(const std::filesystem::path& a_path, std::vector<std::uint8_t>& a_out)
		{
			std::FILE* f = nullptr;
			if (_wfopen_s(&f, a_path.c_str(), L"rb") != 0 || !f) {
				return false;
			}
			_fseeki64(f, 0, SEEK_END);
			const auto len = _ftelli64(f);
			_fseeki64(f, 0, SEEK_SET);
			if (len <= 0 || len > (64 * 1024 * 1024)) {
				fclose(f);
				return false;
			}
			a_out.resize(static_cast<std::size_t>(len));
			const auto read = std::fread(a_out.data(), 1, a_out.size(), f);
			fclose(f);
			return read == a_out.size();
		}

		// RIFF/WAVE reader for PCM 8/16/24/32-bit int and 32/64-bit float,
		// mono or stereo. Everything is converted to float on the way out so the
		// render loop is format-agnostic. Compressed formats are rejected.
		bool DecodeWav(const std::filesystem::path& a_path, Pcm& a_out, std::string& a_error)
		{
			std::vector<std::uint8_t> raw;
			if (!ReadAllBytes(a_path, raw)) {
				a_error = "unreadable";
				return false;
			}
			if (raw.size() < 44 || std::memcmp(raw.data(), "RIFF", 4) != 0 || std::memcmp(raw.data() + 8, "WAVE", 4) != 0) {
				a_error = "not a RIFF/WAVE file";
				return false;
			}

			std::uint16_t format = 0, channels = 0, bits = 0;
			std::uint32_t rate = 0;
			std::vector<std::uint8_t> pcm;
			bool haveFmt = false;

			std::size_t cursor = 12;
			while (cursor + 8 <= raw.size()) {
				char id[5]{};
				std::memcpy(id, raw.data() + cursor, 4);
				std::uint32_t size = 0;
				std::memcpy(&size, raw.data() + cursor + 4, 4);
				const auto body = cursor + 8;
				if (body + size > raw.size()) {
					break;
				}
				if (std::memcmp(id, "fmt ", 4) == 0 && size >= 16) {
					std::memcpy(&format, raw.data() + body, 2);
					std::memcpy(&channels, raw.data() + body + 2, 2);
					std::memcpy(&rate, raw.data() + body + 4, 4);
					std::memcpy(&bits, raw.data() + body + 14, 2);
					if (format == 0xFFFE && size >= 40) {
						std::memcpy(&format, raw.data() + body + 24, 2);
					}
					haveFmt = true;
				} else if (std::memcmp(id, "data", 4) == 0) {
					pcm.assign(raw.begin() + static_cast<std::ptrdiff_t>(body),
						raw.begin() + static_cast<std::ptrdiff_t>(body + size));
				}
				cursor = body + size + (size & 1u);
			}

			if (!haveFmt) {
				a_error = "no fmt chunk";
				return false;
			}
			if (pcm.empty()) {
				a_error = "no data chunk";
				return false;
			}
			if (format != 1 && format != 3) {
				a_error = "compressed WAV (need PCM or float)";
				return false;
			}
			if (channels < 1 || channels > 2) {
				a_error = "need mono or stereo";
				return false;
			}

			const std::size_t bytesPerSample = bits / 8;
			if (bytesPerSample == 0) {
				a_error = "unsupported bit depth";
				return false;
			}
			const std::size_t total = pcm.size() / bytesPerSample;
			a_out.samples.resize(total);
			a_out.sampleRate = rate;
			a_out.channels = channels;

			for (std::size_t i = 0; i < total; ++i) {
				const auto* p = pcm.data() + i * bytesPerSample;
				float v = 0.0f;
				if (format == 1) {
					switch (bits) {
					case 8:  v = (static_cast<float>(*p) - 128.0f) / 128.0f; break;
					case 16: {
						std::int16_t s{};
						std::memcpy(&s, p, 2);
						v = s / 32768.0f;
						break;
					}
					case 24: {
						std::int32_t s = (static_cast<std::int32_t>(p[0]) << 8) | (static_cast<std::int32_t>(p[1]) << 16) |
										 (static_cast<std::int32_t>(p[2]) << 24);
						v = static_cast<float>(s >> 8) / 8388608.0f;
						break;
					}
					case 32: {
						std::int32_t s{};
						std::memcpy(&s, p, 4);
						v = s / 2147483648.0f;
						break;
					}
					default: a_error = "unsupported PCM bit depth"; return false;
					}
				} else {
					if (bits == 32) {
						float f{};
						std::memcpy(&f, p, 4);
						v = f;
					} else if (bits == 64) {
						double f{};
						std::memcpy(&f, p, 8);
						v = static_cast<float>(f);
					} else {
						a_error = "unsupported float bit depth";
						return false;
					}
				}
				a_out.samples[i] = std::clamp(v, -1.0f, 1.0f);
			}
			return true;
		}

		void LoadSettings()
		{
			const auto ini = g_dir / kIniFileName;
			wchar_t music[MAX_PATH]{};
			GetPrivateProfileStringW(L"audio", L"musicFile", L"shop_music_elevator.wav", music, MAX_PATH, ini.c_str());
			g_musicPath = ResolveClip(music);
			g_enabled = GetPrivateProfileIntW(L"audio", L"enabled", 1, ini.c_str()) != 0;
			const int musicVol = GetPrivateProfileIntW(L"audio", L"musicVolume", 45, ini.c_str());
			const int sfxVol = GetPrivateProfileIntW(L"audio", L"sfxVolume", 55, ini.c_str());
			g_musicVolume.store(std::clamp(musicVol / 100.0f, 0.0f, 1.0f), std::memory_order_release);
			g_sfxVolume.store(std::clamp(sfxVol / 100.0f, 0.0f, 1.0f), std::memory_order_release);
		}

		void CloseWasapi();

		// True when the endpoint mix format carries 32/64-bit float samples. The mix
// format is routinely WAVE_FORMAT_EXTENSIBLE, whose real type lives in the
// first two bytes of the SubFormat GUID at offset 24 - comparing the tag
// directly against WAVE_FORMAT_IEEE_FLOAT is what broke the first WASAPI build.
bool IsFloatMixFormat()
	{
		if (!g_mixFormat) {
			return false;
		}
		if (g_mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
			return true;
		}
		if (g_mixFormat->wFormatTag != WAVE_FORMAT_EXTENSIBLE) {
			return false;
		}
// WAVEFORMATEXTENSIBLE: cbSize(2) + validBits(2) + channelMask(4) +
		// SubFormat GUID(16), all after the plain WAVEFORMATEX that
		// g_mixFormat already points at. Read the SubFormat's first WORD as the
		// format tag, and require enough bytes to actually be there.
		const auto* const ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(g_mixFormat);
		if (g_mixFormat->cbSize < 22) {
			return false;
		}
		std::uint16_t subFormat = 0;
		std::memcpy(&subFormat, reinterpret_cast<const std::uint8_t*>(&ext->SubFormat), sizeof(subFormat));
		return subFormat == WAVE_FORMAT_IEEE_FLOAT;
	}

		bool g_comInitialized{ false };

		// Open the shared-mode render endpoint. Shared (never exclusive) because
		// the game already owns the device through Wwise.
		bool OpenWasapi()
		{
			HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
				SetError(std::format("CoInitializeEx failed hr=0x{:08X}", static_cast<unsigned>(hr)));
				return false;
			}
			g_comInitialized = SUCCEEDED(hr);

			IMMDeviceEnumerator* enumerator = nullptr;
			hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
				reinterpret_cast<void**>(&enumerator));
			if (FAILED(hr)) {
				SetError(std::format("MMDeviceEnumerator unavailable hr=0x{:08X}", static_cast<unsigned>(hr)));
				CloseWasapi();
				return false;
			}

			IMMDevice* device = nullptr;
			hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
			enumerator->Release();
			if (FAILED(hr)) {
				SetError(std::format("no default render endpoint hr=0x{:08X}", static_cast<unsigned>(hr)));
				CloseWasapi();
				return false;
			}

			hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&g_client));
			device->Release();
			if (FAILED(hr)) {
				SetError(std::format("IAudioClient activate failed hr=0x{:08X}", static_cast<unsigned>(hr)));
				CloseWasapi();
				return false;
			}

			hr = g_client->GetMixFormat(&g_mixFormat);
			if (FAILED(hr) || !g_mixFormat) {
				SetError(std::format("GetMixFormat failed hr=0x{:08X}", static_cast<unsigned>(hr)));
				CloseWasapi();
				return false;
			}

			// AUTOCONVERTPCM lets the engine accept our float/44.1k content even
			// though the endpoint mix format is float/48k.
			const auto period = static_cast<REFERENCE_TIME>(10000.0 * 10000.0 / g_mixFormat->nSamplesPerSec);
			hr = g_client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, period, period,
				g_mixFormat, &kShopSessionGuid);
			if (FAILED(hr)) {
				SetError(std::format("IAudioClient::Initialize failed hr=0x{:08X}", static_cast<unsigned>(hr)));
				CloseWasapi();
				return false;
			}

			hr = g_client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&g_render));
			if (FAILED(hr)) {
				SetError(std::format("IAudioRenderClient unavailable hr=0x{:08X}", static_cast<unsigned>(hr)));
				CloseWasapi();
				return false;
			}

			hr = g_client->GetBufferSize(&g_bufferFrames);
			if (FAILED(hr) || g_bufferFrames == 0) {
				SetError("GetBufferSize failed");
				CloseWasapi();
				return false;
			}

			hr = g_client->Start();
			if (FAILED(hr)) {
				SetError(std::format("IAudioClient::Start failed hr=0x{:08X}", static_cast<unsigned>(hr)));
				CloseWasapi();
				return false;
			}
			return true;
		}

		void CloseWasapi()
		{
			const bool hadResources = g_client || g_render || g_mixFormat || g_comInitialized;
			if (g_client) {
				g_client->Stop();
			}
			if (g_render) {
				g_render->Release();
				g_render = nullptr;
			}
			if (g_client) {
				g_client->Release();
				g_client = nullptr;
			}
			if (g_mixFormat) {
				CoTaskMemFree(g_mixFormat);
				g_mixFormat = nullptr;
			}
			if (g_comInitialized) {
				CoUninitialize();
				g_comInitialized = false;
			}
			if (hadResources) {
				REX::INFO("SFShopExplorer audio: WASAPI resources cleaned up");
			}
		}

		// One-shot voice state. SFX are short and single-sample; only a handful
		// can overlap, so a fixed pool avoids any allocation on the audio path.
		struct Playing
		{
			const Pcm* clip{ nullptr };
			std::size_t cursor{ 0 };
			bool         active{ false };
		};

		Playing g_playing[16]{};

		float StepFade(float a_current, float a_target, float a_dt)
		{
			constexpr float kSpeed = 1.0f / 0.75f;
			const auto delta = a_target - a_current;
			const auto step = kSpeed * a_dt;
			if (std::abs(delta) <= step) {
				return a_target;
			}
			return a_current + (delta > 0.0f ? step : -step);
		}

		// Read one clip frame into a float stereo pair, resampling on the fly.
		// Linear interpolation is plenty for UI stings and a music bed, and keeps
		// the whole render loop free of per-sample format branching.
// a_cursor counts OUTPUT frames while the clip lives at its own rate, so
		// the input position is the plain rate ratio, wrapped into the clip.
		//
		// This must NOT divide by the loop length as well. Doing so resolved to
		// cursor*clipRate/(outRate^2*frames), which after one second of output
		// read input sample 0.9 instead of 44100 - i.e. it replayed the first
		// sample for the whole loop. Measured effect: music came out at ~3% of its
		// real amplitude (musicPeak 0.02 with fade 0.90) and every click collapsed
		// into a DC blip that merely sounded like a tick.
		void FetchFrame(const Pcm& a_clip, std::size_t a_cursor, const std::uint32_t a_outRate, float& a_left, float& a_right)
		{
			const auto frames = a_clip.samples.size() / a_clip.channels;
			if (frames == 0 || a_outRate == 0) {
				a_left = a_right = 0.0f;
				return;
			}
			const auto posD = static_cast<double>(a_cursor) * (static_cast<double>(a_clip.sampleRate) / a_outRate);
			auto pos = posD;
			if (pos >= static_cast<double>(frames)) {
				pos -= static_cast<double>(frames) * std::floor(pos / frames);
			}
			const auto i0 = static_cast<std::size_t>(pos);
			const auto frac = static_cast<float>(pos - static_cast<double>(i0));
			const auto i1 = (i0 + 1 < frames) ? i0 + 1 : i0;

			if (a_clip.channels == 1) {
				a_left = a_right = a_clip.samples[i0];
				return;
			}
			a_left = a_clip.samples[i0 * 2];
			a_right = a_clip.samples[i0 * 2 + 1];
			if (i1 != i0) {
				a_left = a_left * (1.0f - frac) + a_clip.samples[i1 * 2] * frac;
				a_right = a_right * (1.0f - frac) + a_clip.samples[i1 * 2 + 1] * frac;
			}
		}

		// Output frames per pass of a clip. Cursors advance in output frames, so
		// they wrap here rather than at the input frame count.
		std::uint64_t LoopFrames(const Pcm& a_clip, std::uint32_t a_outRate)
		{
			if (a_clip.channels == 0 || a_outRate == 0) {
				return 0;
			}
			const auto frames = static_cast<double>(a_clip.samples.size() / a_clip.channels);
			if (frames <= 0.0) {
				return 0;
			}
			return static_cast<std::uint64_t>(frames * a_outRate / a_clip.sampleRate);
		}

		// Silence the game's own audio for as long as the shop is open.
		//
		// Writing fAudioVolume* prefs does not work: 0.8.334 and 0.8.336 both
		// logged every key as unresolved while the ambience kept playing, so the
		// engine does not take those values from the collections we can reach.
		// Wwise also ignores the UI pause counter, so pausing the world does not
		// stop sound. Core Audio is the one layer both sides share: the game's
		// stream lives in the process default session, and the shop now has its
		// own GUID session, so muting the default session is exactly "the game,
		// not the shop".
		//
		// Sessions are per process and the game owns this one, so no other
		// application is affected. Runs on the audio thread, which has already
		// called CoInitializeEx and can therefore use the enumerator directly.
		void ApplyGameSessionMute(bool a_muted)
		{
			IMMDeviceEnumerator* enumerator = nullptr;
			auto hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
				reinterpret_cast<void**>(&enumerator));
			if (FAILED(hr) || !enumerator) {
				REX::WARN("SFShopExplorer: audio session mute: enumerator unavailable hr=0x{:08X}", static_cast<unsigned>(hr));
				return;
			}
			IMMDevice* device = nullptr;
			hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
			enumerator->Release();
			if (FAILED(hr) || !device) {
				REX::WARN("SFShopExplorer: audio session mute: no default endpoint hr=0x{:08X}", static_cast<unsigned>(hr));
				return;
			}
			IAudioSessionManager* manager = nullptr;
			hr = device->Activate(__uuidof(IAudioSessionManager), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&manager));
			device->Release();
			if (FAILED(hr) || !manager) {
				REX::WARN("SFShopExplorer: audio session mute: session manager unavailable hr=0x{:08X}", static_cast<unsigned>(hr));
				return;
			}
			ISimpleAudioVolume* volume = nullptr;
			// A NULL session GUID selects the default session, i.e. the game's.
			hr = manager->GetSimpleAudioVolume(nullptr, 0, &volume);
			manager->Release();
			if (FAILED(hr) || !volume) {
				REX::WARN("SFShopExplorer: audio session mute: default session unavailable hr=0x{:08X}", static_cast<unsigned>(hr));
				return;
			}
			const auto setHr = volume->SetMute(a_muted ? TRUE : FALSE, nullptr);
			volume->Release();
			if (FAILED(setHr)) {
				REX::WARN("SFShopExplorer: audio session mute={} refused hr=0x{:08X}", a_muted ? 1 : 0, static_cast<unsigned>(setHr));
				return;
			}
			g_gameMuted = a_muted;
			REX::INFO("SFShopExplorer: game audio session muted={} (shop keeps its own session)", a_muted ? 1 : 0);
		}

		void AudioThread()
		{
			for (int attempt = 1; attempt <= 20; ++attempt) {
				if (g_quit.load(std::memory_order_acquire)) {
					return;
				}
				if (OpenWasapi()) {
					break;
				}
				if (attempt == 1 || attempt % 5 == 0) {
					REX::INFO("SFShopExplorer: audio endpoint retry {}/20", attempt);
				}
				Sleep(500);
			}

			if (!g_render) {
				SetError("no usable audio endpoint; shop runs silent");
				g_failed.store(true, std::memory_order_release);
				return;
			}

			g_ready.store(true, std::memory_order_release);
			REX::INFO("SFShopExplorer: audio session split (shop session GUID 5f2c8a11-9b3e-4f6a-9d2a-77513b0c6e41; default session belongs to the game)");
// Log the real channel layout. Out of game this endpoint is 2ch, but in
			// game the game's Wwise reconfigures it (observed 8ch), and ch0/ch1 may
			// not be front left/right there. Knowing the mask is the difference
			// between guessing and knowing.
			{
				std::uint32_t mask = 0;
				if (g_mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE && g_mixFormat->cbSize >= 22) {
					const auto* const ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(g_mixFormat);
					mask = ext->dwChannelMask;
				}
				REX::INFO("SFShopExplorer: audio ready via WASAPI ({}ch/{}Hz tag=0x{:04X} {}, mask=0x{:08X}, {} clips, music '{}')",
					g_mixFormat->nChannels, g_mixFormat->nSamplesPerSec, g_mixFormat->wFormatTag,
					IsFloatMixFormat() ? "float" : "int", mask, [](std::size_t n) {
						std::size_t live = 0;
						for (std::size_t i = 0; i < n; ++i) {
							live += g_clips[i].samples.empty() ? 0 : 1;
						}
						return live;
					}(kClipCount), g_musicPath.filename().string().c_str());
			}

			const auto outRate = g_mixFormat->nSamplesPerSec;
			const auto outChannels = g_mixFormat->nChannels;
			// Output frames per loop, so the cursor wraps in the same units it advances.
			std::uint64_t musicFrames = LoopFrames(g_music, g_mixFormat->nSamplesPerSec);
			std::uint64_t musicCursor = 0;

			auto last = std::chrono::steady_clock::now();
			std::uint64_t submitted = 0;
			while (!g_quit.load(std::memory_order_acquire)) {
				{
					std::unique_lock lock(g_mutex);
					g_wake.wait_for(lock, std::chrono::milliseconds(20), [] {
						return g_quit.load(std::memory_order_acquire) || !g_pendingSfx.empty() ||
							g_musicRequested.load(std::memory_order_acquire);
					});
				}
				if (g_quit.load(std::memory_order_acquire)) {
					break;
				}

				// Game-audio mute requests are applied here, on the thread that
				// owns the COM initialization.
				{
					const auto request = g_gameMuteRequest.exchange(-1, std::memory_order_acq_rel);
					if (request >= 0 && (request != 0) != g_gameMuted) {
						ApplyGameSessionMute(request != 0);
					}
				}

				const auto now = std::chrono::steady_clock::now();
				const auto dt = std::chrono::duration<float>(now - last).count();
				last = now;

				// Claim pending one-shots.
				while (true) {
					Slot slot{};
					{
						std::scoped_lock lock(g_mutex);
						if (g_pendingSfx.empty()) {
							break;
						}
						slot = g_pendingSfx.front();
						g_pendingSfx.pop_front();
					}
					const auto index = static_cast<std::size_t>(slot);
					if (index >= kClipCount || g_clips[index].samples.empty()) {
						continue;
					}
					// Steal the oldest voice so a rapid run of clicks still sounds.
					std::size_t victim = 0;
					for (std::size_t i = 1; i < std::size(g_playing); ++i) {
						if (!g_playing[i].active) {
							victim = i;
							break;
						}
						if (g_playing[i].cursor > g_playing[victim].cursor) {
							victim = i;
						}
					}
					g_playing[victim].clip = &g_clips[index];
					g_playing[victim].cursor = 0;
					g_playing[victim].active = true;
				}

// Music fade envelope.
				const auto wantMusic = g_musicRequested.load(std::memory_order_acquire);

				// Theme switch: swap the decoded track here, on the audio thread,
				// before it is mixed. The current voice is dropped first so the
				// change lands on the next fade rather than as a click.
				if (g_musicDirty.exchange(false, std::memory_order_acq_rel)) {
					wchar_t wide[MAX_PATH]{};
					Pcm    fresh;
					std::string err;
					if (MultiByteToWideChar(CP_UTF8, 0, g_musicFile.c_str(), -1, wide, MAX_PATH) > 0 &&
						DecodeWav(ResolveClip(wide), fresh, err) && !fresh.samples.empty()) {
						g_music = std::move(fresh);
						g_musicPath = ResolveClip(wide);
						g_musicFade.store(0.0f, std::memory_order_release);
						musicFrames = LoopFrames(g_music, g_mixFormat->nSamplesPerSec);
						musicCursor = 0;
						REX::INFO("SFShopExplorer: music track -> '{}'", g_musicFile);
					} else {
						SetError("music switch to '" + g_musicFile + "' failed (" + err + ")");
					}
				}

				const auto target = wantMusic ? g_musicVolume.load(std::memory_order_acquire) : 0.0f;
				const auto fade = StepFade(g_musicFade.load(std::memory_order_acquire), target, dt);
				g_musicFade.store(fade, std::memory_order_release);
				if (!wantMusic && fade <= 0.0005f) {
					g_musicPlaying.store(false, std::memory_order_release);
				} else if (wantMusic && musicFrames > 0) {
					g_musicPlaying.store(true, std::memory_order_release);
				}

				// Pull whatever the endpoint has room for.
				UINT32 padding = 0;
				if (FAILED(g_client->GetCurrentPadding(&padding))) {
					Sleep(20);
					continue;
				}
				const auto available = g_bufferFrames > padding ? g_bufferFrames - padding : 0;
				if (available == 0) {
					Sleep(10);
					continue;
				}

				BYTE* out = nullptr;
				const auto chunk = std::min<UINT32>(available, 2048);
				if (FAILED(g_render->GetBuffer(chunk, &out))) {
					Sleep(10);
					continue;
				}

				const auto outFramesFloat = static_cast<std::size_t>(chunk);
				std::vector<float> accum(outFramesFloat * outChannels, 0.0f);

// Track the loudest music sample actually handed to the endpoint, so
				// "music is in the mix" stops being an assumption. A sustained bed
				// can be perfectly mixed and still inaudible under full-volume
				// ambience, and that is indistinguishable from a routing fault
				// without a number.
				float musicPeak = 0.0f;
				for (UINT32 f = 0; f < chunk; ++f) {
					float l = 0.0f, r = 0.0f;

					if (musicFrames > 0 && g_musicFade.load(std::memory_order_acquire) > 0.0005f) {
						FetchFrame(g_music, static_cast<std::size_t>(musicCursor), outRate, l, r);
						l *= fade;
						r *= fade;
						musicPeak = std::max(musicPeak, std::max(std::abs(l), std::abs(r)));
						++musicCursor;
						if (musicCursor >= musicFrames) {
							musicCursor = 0;  // seamless loop
						}
					}

					const auto sfxVol = g_sfxVolume.load(std::memory_order_acquire);
					if (sfxVol > 0.0f) {
						for (auto& p : g_playing) {
							if (!p.active || !p.clip) {
								continue;
							}
							float sl = 0.0f, sr = 0.0f;
							FetchFrame(*p.clip, p.cursor, outRate, sl, sr);
							l += sl * sfxVol;
							r += sr * sfxVol;
							++p.cursor;
							// Retire when the source is exhausted.
							const std::uint64_t srcFrames = LoopFrames(*p.clip, outRate);
							if (srcFrames == 0 || p.cursor >= srcFrames) {
								p.active = false;
								p.clip = nullptr;
							}
						}
					}

					auto* dst = &accum[static_cast<std::size_t>(f) * outChannels];
					if (outChannels == 1) {
						dst[0] = (l + r) * 0.5f;
					} else {
						dst[0] = l;
						dst[1] = r;
						for (UINT32 c = 2; c < outChannels; ++c) {
							dst[c] = 0.0f;
						}
					}
				}

				// The mix format is whatever the endpoint hands us, and on this
				// machine that is WAVE_FORMAT_EXTENSIBLE (tag 0xFFFE) at 32-bit,
				// NOT WAVE_FORMAT_IEEE_FLOAT. Testing the tag against IEEE_FLOAT
				// therefore failed and every buffer was discarded, which is why the
				// first deployed WASAPI build was silent. Resolve the real format
				// through the SubFormat GUID when the tag is EXTENSIBLE.
				const bool isFloat = IsFloatMixFormat();
				if (isFloat) {
					std::memcpy(out, accum.data(), accum.size() * sizeof(float));
				} else if (g_mixFormat->wBitsPerSample == 16) {
					auto* p16 = reinterpret_cast<std::int16_t*>(out);
					for (std::size_t i = 0; i < accum.size(); ++i) {
						p16[i] = static_cast<std::int16_t>(std::clamp(accum[i], -1.0f, 1.0f) * 32767.0f);
					}
				} else {
					g_render->ReleaseBuffer(chunk, 0);
					SetError(std::format("unsupported mix format tag=0x{:04X} bits={}", g_mixFormat->wFormatTag,
						g_mixFormat->wBitsPerSample));
					break;
				}

				const auto released = g_render->ReleaseBuffer(chunk, 0);
				if (FAILED(released)) {
					SetError(std::format("ReleaseBuffer failed hr=0x{:08X}", static_cast<unsigned>(released)));
					CloseWasapi();
					break;
				}
if (++submitted % 600 == 0) {
					REX::INFO("SFShopExplorer: audio streaming ok ({} buffers, {}ch/{}Hz, musicFade={:.2f}, musicPeak={:.4f})",
						submitted, outChannels, outRate, g_musicFade.load(std::memory_order_acquire), musicPeak);
					musicPeak = 0.0f;
				}
			}

			for (auto& p : g_playing) {
				p = {};
			}
			CloseWasapi();
			g_ready.store(false, std::memory_order_release);
		}
	}

	unsigned __stdcall AudioWorkerEntry(void*) noexcept
	{
		try {
			AudioThread();
		} catch (const std::exception& e) {
			try {
				SetError(std::format("audio worker exception: {}", e.what()));
			} catch (...) {
			}
			CloseWasapi();
		} catch (...) {
			try {
				SetError("audio worker terminated by an unknown exception");
			} catch (...) {
			}
			CloseWasapi();
		}
		g_ready.store(false, std::memory_order_release);

		{
			std::scoped_lock lock(g_workerMutex);
			g_workerActive.store(false, std::memory_order_release);
		}
		REX::INFO("SFShopExplorer audio: worker stopped");
		g_workerStopped.notify_all();
		return 0;
	}

	void Init()
	{
		if (g_started.exchange(true, std::memory_order_acq_rel)) {
			return;
		}

		g_dir = ModuleDir();
		LoadSettings();
		if (!g_enabled) {
			REX::INFO("SFShopExplorer: audio off ([audio] enabled=0)");
			return;
		}

		for (std::size_t i = 0; i < kClipCount; ++i) {
			std::string err;
			if (!DecodeWav(ResolveClip(kSfxNames[i]), g_clips[i], err)) {
				SetError(std::string("sfx ") + kSfxNames[i] + " skipped (" + err + ")");
			}
		}

		std::string musicErr;
		if (!DecodeWav(g_musicPath, g_music, musicErr)) {
			SetError("music unavailable: " + musicErr);
		}
		// One line per launch answers "why is my track not in the list": the ui_
		// prefix is the only exclusion, so the count is the whole story.
		REX::INFO("SFShopExplorer audio: {} music track(s) available (ui_* files are sfx)",
			ListMusicTracks().size());

		HMODULE modulePin = nullptr;
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
				reinterpret_cast<LPCWSTR>(&AudioThread), &modulePin)) {
			SetError(std::format("could not pin audio worker module ({})", GetLastError()));
			return;
		}

		unsigned workerId = 0;
		HANDLE worker = nullptr;
		{
			std::scoped_lock lock(g_workerMutex);
			g_workerModulePin = modulePin;
			g_workerActive.store(true, std::memory_order_release);
			REX::INFO("SFShopExplorer audio: worker starting (module pinned for process lifetime)");
			worker = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, AudioWorkerEntry, nullptr, 0, &workerId));
			if (worker) {
				g_workerHandle = worker;
			} else {
				g_workerActive.store(false, std::memory_order_release);
				g_workerModulePin = nullptr;
			}
		}
		if (!worker) {
			FreeLibrary(modulePin);
			g_workerStopped.notify_all();
			REX::WARN("SFShopExplorer audio: worker start failed");
			SetError("could not start audio worker; shop runs silent");
			return;
		}
		for (int i = 0; i < 300; ++i) {
			if (g_ready.load(std::memory_order_acquire) || g_failed.load(std::memory_order_acquire)) {
				break;
			}
			Sleep(10);
		}
		if (g_failed.load(std::memory_order_acquire)) {
			SetError("no audio device; shop runs silent");
			g_quit.store(true, std::memory_order_release);
			g_wake.notify_all();
			Shutdown();
		}
	}

	void Shutdown()
	{
		if (!g_started.load(std::memory_order_acquire)) {
			return;
		}
		if (!g_quit.exchange(true, std::memory_order_acq_rel)) {
			REX::INFO("SFShopExplorer audio: shutdown requested");
		}
		g_wake.notify_all();
		HANDLE worker = nullptr;
		{
			std::unique_lock lock(g_workerMutex);
			g_workerStopped.wait(lock, [] {
				return !g_workerActive.load(std::memory_order_acquire);
			});
			worker = std::exchange(g_workerHandle, nullptr);
		}
		if (worker) {
			WaitForSingleObject(worker, INFINITE);
			CloseHandle(worker);
		}
		// Deliberately retain g_workerModulePin until process teardown. Freeing
		// the last DLL reference here could unload code before this caller returns.
		REX::INFO("SFShopExplorer audio: shutdown complete (module pin retained until process exit)");
	}

	void PlaySfx(Sfx a_sfx)
	{
		if (static_cast<std::size_t>(a_sfx) >= kClipCount) {
			return;
		}
		if (g_sfxVolume.load(std::memory_order_acquire) <= 0.0f) {
			return;
		}
		if (g_quit.load(std::memory_order_acquire)) {
			return;
		}
		{
			std::scoped_lock lock(g_mutex);
			if (g_pendingSfx.size() >= 32) {
				g_pendingSfx.clear();
			}
			g_pendingSfx.push_back(static_cast<Slot>(a_sfx));
		}
		g_wake.notify_all();
	}

	std::vector<std::string> ListMusicTracks()
	{
		std::vector<std::string> tracks;
		const auto audio = g_dir / L"Audio";
		const std::filesystem::path dirs[]{ audio, audio / L"Music" };
		const char* const prefixes[]{ "", "Music\\" };
		for (std::size_t d = 0; d < std::size(dirs); ++d) {
			std::error_code ec;
			std::filesystem::directory_iterator it(dirs[d], ec);
			if (ec) {
				continue;
			}
			for (const auto& entry : it) {
				std::error_code entryEc;
				if (!entry.is_regular_file(entryEc) || entryEc || !IsMusicTrackName(entry.path())) {
					continue;
				}
				const auto name = WidenToUtf8(entry.path().filename().wstring());
				if (!name.empty()) {
					tracks.push_back(std::string(prefixes[d]) + name);
				}
			}
		}
		std::sort(tracks.begin(), tracks.end(), [](const std::string& a, const std::string& b) {
			return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
				return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y));
			});
		});
		return tracks;
	}

	void SetMusicFile(const char* a_fileName)
	{
		if (!a_fileName || !a_fileName[0]) {
			return;
		}
		{
			std::scoped_lock lock(g_mutex);
			// Compare before assigning. The previous version compared the
			// assignment against its own result, which is always true, so the
			// "already on this track" early-out fired on every call and the
			// track only ever changed on the next Open(). That is why switching
			// themes mid-session appeared to do nothing.
			if (g_musicFile == a_fileName) {
				return;
			}
			g_musicFile = a_fileName;
		}
		g_musicDirty.store(true, std::memory_order_release);
		g_wake.notify_all();
	}

	void StartMusic()
	{
		if (g_quit.load(std::memory_order_acquire)) {
			return;
		}
		g_musicRequested.store(true, std::memory_order_release);
		g_wake.notify_all();
	}

	void StopMusic()
	{
		g_musicRequested.store(false, std::memory_order_release);
		g_wake.notify_all();
	}

	void SetGameSessionMuted(bool a_muted)
	{
		g_gameMuteRequest.store(a_muted ? 1 : 0, std::memory_order_release);
		g_wake.notify_all();
	}

	void SetMusicVolume(float a_volume)
	{
		g_musicVolume.store(std::clamp(a_volume, 0.0f, 1.0f), std::memory_order_release);
		g_wake.notify_all();
	}

	void SetSfxVolume(float a_volume)
	{
		g_sfxVolume.store(std::clamp(a_volume, 0.0f, 1.0f), std::memory_order_release);
	}

	float GetMusicVolume()
	{
		return g_musicVolume.load(std::memory_order_acquire);
	}

	float GetSfxVolume()
	{
		return g_sfxVolume.load(std::memory_order_acquire);
	}

	bool MusicIsPlaying()
	{
		return g_musicPlaying.load(std::memory_order_acquire);
	}

	const char* LastError()
	{
		std::scoped_lock lock(g_errorMutex);
		return g_lastError.c_str();
	}
}