// ============================================================================
// BackdropArt.cpp - the player's own backdrop images, as the OSF page can use
// them
//
// Two jobs: name the space_<name>.<ext> files this install has, and turn one of
// them into a JPEG data URL. The overlay does neither - it decodes the same
// files straight into D3D12 textures - because a WebView gets its bytes over
// the bridge or not at all (see BackdropArt.h).
//
// Layout: paths, name matching, JPEG encode, the cache in front of it.
// ============================================================================

#include "PCH.h"

#include "BackdropArt.h"
#include "ShopExplorer.h"

#include <wincodec.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <map>
#include <mutex>
#include <vector>

namespace SFE::BackdropArt
{
	namespace
	{
		// The same four the overlay's scan accepts, so a file that appears in one
		// picker appears in the other.
		constexpr const wchar_t* kExtensions[]{ L".png", L".jpg", L".jpeg", L".bmp" };

		std::filesystem::path PluginDirectory()
		{
			HMODULE self = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&PluginDirectory), &self);
			wchar_t modulePath[MAX_PATH]{};
			GetModuleFileNameW(self, modulePath, MAX_PATH);
			return std::filesystem::path(modulePath).parent_path();
		}

		// This build's own assets folder - the one its README names. The two shops
		// are separate mods with separate INIs, caches and asset folders, so the
		// page reads only this build's; an image dropped for the overlay is not
		// silently in scope here.
		std::filesystem::path AssetsRoot()
		{
			return PluginDirectory() / SFE::kAssetsFolderName;
		}

		std::wstring WideOfUtf8(const std::string& a_text)
		{
			if (a_text.empty()) {
				return {};
			}
			const int len = MultiByteToWideChar(CP_UTF8, 0, a_text.c_str(), -1, nullptr, 0);
			if (len <= 1) {
				return {};
			}
			std::wstring out(static_cast<std::size_t>(len), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, a_text.c_str(), -1, out.data(), len);
			out.resize(static_cast<std::size_t>(len - 1));
			return out;
		}

		std::wstring LowerOf(std::wstring a_text)
		{
			std::transform(a_text.begin(), a_text.end(), a_text.begin(), [](wchar_t c) {
				return static_cast<wchar_t>(std::towlower(c));
			});
			return a_text;
		}

		std::string LowerAscii(std::string a_text)
		{
			std::transform(a_text.begin(), a_text.end(), a_text.begin(), [](unsigned char c) {
				return static_cast<char>(std::tolower(c));
			});
			return a_text;
		}

		// space_<name>.<one of the four>, already lowercased.
		bool IsBackdropName(const std::wstring& a_lowerName)
		{
			if (a_lowerName.rfind(L"space_", 0) != 0) {
				return false;
			}
			const auto dot = a_lowerName.find_last_of(L'.');
			if (dot == std::wstring::npos || dot <= 6) {
				return false;
			}
			const auto ext = a_lowerName.substr(dot);
			return std::any_of(std::begin(kExtensions), std::end(kExtensions), [&ext](const wchar_t* a_ext) {
				return ext == a_ext;
			});
		}

		// The <name> half, lowercased, in UTF-8. Only called on a name that has
		// already passed IsBackdropName, so the extension is known to be there.
		std::string NameOf(const std::wstring& a_lowerName)
		{
			const auto dot = a_lowerName.find_last_of(L'.');
			return NarrowUtf8(a_lowerName.substr(6, dot - 6));
		}

		// The file for one name. The same image may exist in more than one format,
		// so the alphabetically first match wins - .bmp < .jpeg < .jpg < .png by
		// path, a stable rule rather than a race with the directory order.
		std::filesystem::path FindFile(const std::string& a_name)
		{
			const auto wanted = LowerAscii(a_name);
			if (wanted.empty()) {
				return {};
			}
			std::filesystem::path best;
			std::error_code ec;
			for (std::filesystem::directory_iterator it(AssetsRoot(), ec), end; !ec && it != end; it.increment(ec)) {
				std::error_code fileEc;
				if (!it->is_regular_file(fileEc) || fileEc) {
					continue;
				}
				const auto lower = LowerOf(it->path().filename().wstring());
				if (!IsBackdropName(lower) || NameOf(lower) != wanted) {
					continue;
				}
				if (best.empty() || it->path().wstring() < best.wstring()) {
					best = it->path();
				}
			}
			return best;
		}

		std::string Base64(const std::vector<std::uint8_t>& a_bytes)
		{
			static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
			std::string out;
			out.reserve((a_bytes.size() + 2) / 3 * 4);
			std::size_t i = 0;
			for (; i + 2 < a_bytes.size(); i += 3) {
				const auto n = (static_cast<std::uint32_t>(a_bytes[i]) << 16) |
					(static_cast<std::uint32_t>(a_bytes[i + 1]) << 8) | a_bytes[i + 2];
				out += kAlphabet[(n >> 18) & 63];
				out += kAlphabet[(n >> 12) & 63];
				out += kAlphabet[(n >> 6) & 63];
				out += kAlphabet[n & 63];
			}
			const auto left = a_bytes.size() - i;
			if (left == 1) {
				const auto n = static_cast<std::uint32_t>(a_bytes[i]) << 16;
				out += kAlphabet[(n >> 18) & 63];
				out += kAlphabet[(n >> 12) & 63];
				out += "==";
			} else if (left == 2) {
				const auto n = (static_cast<std::uint32_t>(a_bytes[i]) << 16) |
					(static_cast<std::uint32_t>(a_bytes[i + 1]) << 8);
				out += kAlphabet[(n >> 18) & 63];
				out += kAlphabet[(n >> 12) & 63];
				out += kAlphabet[(n >> 6) & 63];
				out += '=';
			}
			return out;
		}

		void EnsureCom()
		{
			// The overlay's asset upload runs on the render thread, where COM is
			// up. A bridge handler runs on the game thread, which this plugin has
			// never had a reason to initialise. RPC_E_CHANGED_MODE means the game
			// already initialised it differently - COM is usable either way, so the
			// result is deliberately ignored and nothing is ever uninitialised.
			static std::once_flag once;
			std::call_once(once, [] {
				const auto hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
				(void)hr;
			});
		}

		// Decode, scale, encode. JPEG rather than PNG: this is a photographic
		// full-window background, and the PNG the previews use would be several
		// megabytes of base64 against an 8 MiB bridge message limit.
		bool EncodeJpeg(const std::filesystem::path& a_path, const unsigned a_maxEdge,
			std::vector<std::uint8_t>& a_out, int& a_width, int& a_height, std::string& a_error)
		{
			EnsureCom();
			IWICImagingFactory* factory = nullptr;
			if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) || !factory) {
				a_error = "the image decoder could not start";
				return false;
			}
			bool ok = false;
			IWICBitmapDecoder* decoder = nullptr;
			IWICBitmapFrameDecode* frameDecode = nullptr;
			IWICBitmapScaler* scaler = nullptr;
			IWICFormatConverter* converter = nullptr;
			IStream* memory = nullptr;
			IWICStream* stream = nullptr;
			IWICBitmapEncoder* encoder = nullptr;
			IWICBitmapFrameEncode* frameEncode = nullptr;
			IPropertyBag2* properties = nullptr;
			if (SUCCEEDED(factory->CreateDecoderFromFilename(a_path.c_str(), nullptr, GENERIC_READ,
					WICDecodeMetadataCacheOnLoad, &decoder)) && decoder &&
				SUCCEEDED(decoder->GetFrame(0, &frameDecode)) && frameDecode) {
				IWICBitmapSource* source = frameDecode;
				UINT sourceWidth = 0;
				UINT sourceHeight = 0;
				if (SUCCEEDED(frameDecode->GetSize(&sourceWidth, &sourceHeight)) && sourceWidth > 0 && sourceHeight > 0 &&
					a_maxEdge > 0 && (sourceWidth > a_maxEdge || sourceHeight > a_maxEdge)) {
					const double scale = static_cast<double>(a_maxEdge) / (std::max)(sourceWidth, sourceHeight);
					const UINT targetWidth = (std::max)(1u, static_cast<UINT>(sourceWidth * scale + 0.5));
					const UINT targetHeight = (std::max)(1u, static_cast<UINT>(sourceHeight * scale + 0.5));
					if (SUCCEEDED(factory->CreateBitmapScaler(&scaler)) && scaler &&
						SUCCEEDED(scaler->Initialize(frameDecode, targetWidth, targetHeight, WICBitmapInterpolationModeHighQualityCubic))) {
						source = scaler;
					}
				}
				// 24-bit BGR, not 32-bit RGBA: the JPEG encoder refuses alpha, and a
				// format the encoder will not take is the same silent refusal that
				// made the preview's PNG encoder unusable.
				if (SUCCEEDED(factory->CreateFormatConverter(&converter)) && converter &&
					SUCCEEDED(converter->Initialize(source, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone,
						nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
					UINT width = 0;
					UINT height = 0;
					if (SUCCEEDED(converter->GetSize(&width, &height)) && width > 0 && height > 0 &&
						SUCCEEDED(::CreateStreamOnHGlobal(nullptr, TRUE, &memory)) && memory &&
						SUCCEEDED(factory->CreateStream(&stream)) && stream &&
						SUCCEEDED(stream->InitializeFromIStream(memory)) &&
						SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) && encoder &&
						SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
						SUCCEEDED(encoder->CreateNewFrame(&frameEncode, &properties)) && frameEncode) {
						WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
						if (SUCCEEDED(frameEncode->Initialize(properties)) &&
							SUCCEEDED(frameEncode->SetSize(width, height)) &&
							SUCCEEDED(frameEncode->SetResolution(96.0, 96.0)) &&
							SUCCEEDED(frameEncode->SetPixelFormat(&format)) &&
							SUCCEEDED(frameEncode->WriteSource(converter, nullptr)) &&
							SUCCEEDED(frameEncode->Commit()) &&
							SUCCEEDED(encoder->Commit())) {
							STATSTG stat{};
							if (SUCCEEDED(memory->Stat(&stat, STATFLAG_NONAME)) && stat.cbSize.QuadPart > 0) {
								a_out.resize(static_cast<std::size_t>(stat.cbSize.QuadPart));
								LARGE_INTEGER start{};
								ULONG read = 0;
								if (SUCCEEDED(memory->Seek(start, STREAM_SEEK_SET, nullptr)) &&
									SUCCEEDED(memory->Read(a_out.data(), static_cast<ULONG>(a_out.size()), &read)) &&
									read > 0) {
									a_out.resize(read);
									a_width = static_cast<int>(width);
									a_height = static_cast<int>(height);
									ok = true;
								}
							}
						}
					}
				}
			}
			if (!ok) {
				a_error = "the image could not be decoded or encoded";
			}
			if (properties) properties->Release();
			if (frameEncode) frameEncode->Release();
			if (encoder) encoder->Release();
			if (stream) stream->Release();
			if (memory) memory->Release();
			if (converter) converter->Release();
			if (scaler) scaler->Release();
			if (frameDecode) frameDecode->Release();
			if (decoder) decoder->Release();
			factory->Release();
			return ok;
		}

		// One entry per name, so switching themes back and forth, or dragging a
		// colour picker (every change re-applies the theme), does not re-encode a
		// megabyte of JPEG each time. Keyed by the file's write time as well, so a
		// replaced image is picked up without a restart.
		struct Cached
		{
			std::filesystem::file_time_type stamp{};
			std::string dataUrl;
			int         width{ 0 };
			int         height{ 0 };
		};

		std::mutex          g_mutex;
		std::map<std::string, Cached> g_cache;
	}

	std::vector<std::string> Names()
	{
		std::vector<std::string> out;
		std::error_code ec;
		for (std::filesystem::directory_iterator it(AssetsRoot(), ec), end; !ec && it != end; it.increment(ec)) {
			std::error_code fileEc;
			if (!it->is_regular_file(fileEc) || fileEc) {
				continue;
			}
			const auto lower = LowerOf(it->path().filename().wstring());
			if (IsBackdropName(lower)) {
				out.push_back(NameOf(lower));
			}
		}
		std::sort(out.begin(), out.end());
		out.erase(std::unique(out.begin(), out.end()), out.end());
		return out;
	}

	bool DataUrl(const std::string& a_name, const unsigned a_maxEdge, std::string& a_dataUrl,
		int& a_width, int& a_height, std::string& a_error)
	{
		a_dataUrl.clear();
		a_width = 0;
		a_height = 0;
		a_error.clear();
		const auto path = FindFile(a_name);
		if (path.empty()) {
			a_error = "there is no space_" + a_name + ".<png|jpg|jpeg|bmp> beside the plugin";
			return false;
		}
		std::error_code ec;
		const auto stamp = std::filesystem::last_write_time(path, ec);
		{
			std::scoped_lock lock(g_mutex);
			if (const auto cached = g_cache.find(a_name); cached != g_cache.end() && !ec && cached->second.stamp == stamp) {
				a_dataUrl = cached->second.dataUrl;
				a_width = cached->second.width;
				a_height = cached->second.height;
				return true;
			}
		}
		std::vector<std::uint8_t> jpeg;
		int width = 0;
		int height = 0;
		std::string error;
		if (!EncodeJpeg(path, a_maxEdge, jpeg, width, height, error)) {
			a_error = error;
			REX::WARN("SFShopExplorer backdrop art: '{}' failed: {}", path.string(), error);
			return false;
		}
		std::string dataUrl = "data:image/jpeg;base64," + Base64(jpeg);
		REX::DEBUG("SFShopExplorer backdrop art: '{}' -> {}x{} ({} bytes of base64)", path.filename().string(),
			width, height, dataUrl.size());
		{
			std::scoped_lock lock(g_mutex);
			g_cache[a_name] = Cached{ ec ? std::filesystem::file_time_type{} : stamp, dataUrl, width, height };
		}
		a_dataUrl = std::move(dataUrl);
		a_width = width;
		a_height = height;
		return true;
	}
}
