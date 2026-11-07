// Headless harness for the backdrop art encode path: no game, no ImGui.
//
// It mirrors the sequence in src/BackdropArt.cpp - WIC decode, cubic scale to a
// 1920 long edge, format convert to 24bpp BGR, JPEG encode, read the bytes back
// out of the stream - because that path cannot be exercised from inside the
// plugin: the only way to see it there is to launch Starfield and open the
// theme editor. A refusal here is the same "the image could not be decoded or
// encoded" the page would show, so it is worth knowing before a game launch.
//
// Usage: backdrop_art_probe <image> [maxEdge]
// Prints the decoded size, the encoded size and the JPEG magic bytes.
#include <windows.h>
#include <wincodec.h>

#include <cstdio>
#include <vector>

namespace
{
	std::vector<std::uint8_t> g_bytes;

	bool EncodeJpeg(const wchar_t* a_path, const unsigned a_maxEdge, int& a_width, int& a_height)
	{
		const auto hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		(void)hr;
		IWICImagingFactory* factory = nullptr;
		if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) || !factory) {
			std::printf("FAIL: no WIC factory\n");
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
		if (SUCCEEDED(factory->CreateDecoderFromFilename(a_path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)) && decoder &&
			SUCCEEDED(decoder->GetFrame(0, &frameDecode)) && frameDecode) {
			IWICBitmapSource* source = frameDecode;
			UINT sourceWidth = 0;
			UINT sourceHeight = 0;
			if (SUCCEEDED(frameDecode->GetSize(&sourceWidth, &sourceHeight)) && sourceWidth > 0 && sourceHeight > 0) {
				std::printf("source %ux%u\n", sourceWidth, sourceHeight);
				if (a_maxEdge > 0 && (sourceWidth > a_maxEdge || sourceHeight > a_maxEdge)) {
					const double scale = static_cast<double>(a_maxEdge) / (std::max)(sourceWidth, sourceHeight);
					const UINT targetWidth = (std::max)(1u, static_cast<UINT>(sourceWidth * scale + 0.5));
					const UINT targetHeight = (std::max)(1u, static_cast<UINT>(sourceHeight * scale + 0.5));
					if (SUCCEEDED(factory->CreateBitmapScaler(&scaler)) && scaler &&
						SUCCEEDED(scaler->Initialize(frameDecode, targetWidth, targetHeight, WICBitmapInterpolationModeHighQualityCubic))) {
						source = scaler;
						std::printf("scaled to %ux%u\n", targetWidth, targetHeight);
					}
				}
			}
			if (SUCCEEDED(factory->CreateFormatConverter(&converter)) && converter &&
				SUCCEEDED(converter->Initialize(source, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
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
							g_bytes.resize(static_cast<std::size_t>(stat.cbSize.QuadPart));
							LARGE_INTEGER start{};
							ULONG read = 0;
							if (SUCCEEDED(memory->Seek(start, STREAM_SEEK_SET, nullptr)) &&
								SUCCEEDED(memory->Read(g_bytes.data(), static_cast<ULONG>(g_bytes.size()), &read)) && read > 0) {
								g_bytes.resize(read);
								a_width = static_cast<int>(width);
								a_height = static_cast<int>(height);
								ok = true;
							}
						}
					}
				}
			}
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
}

int main(int argc, char** argv)
{
	if (argc < 2) {
		std::printf("usage: backdrop_art_probe <image> [maxEdge]\n");
		return 2;
	}
	wchar_t path[MAX_PATH]{};
	MultiByteToWideChar(CP_ACP, 0, argv[1], -1, path, MAX_PATH);
	const unsigned maxEdge = argc > 2 ? static_cast<unsigned>(atoi(argv[2])) : 1920u;
	int width = 0;
	int height = 0;
	if (!EncodeJpeg(path, maxEdge, width, height)) {
		std::printf("FAIL: the image could not be decoded or encoded\n");
		return 1;
	}
	// The base64 the bridge would carry, so the 8 MiB message limit is checked
	// here rather than by a reply that never arrives in game.
	const std::size_t base64 = (g_bytes.size() + 2) / 3 * 4 + 23;
	std::printf("encoded %dx%d, %zu bytes, %zu as base64\n", width, height, g_bytes.size(), base64);
	if (g_bytes.size() > 2 && g_bytes[0] == 0xFF && g_bytes[1] == 0xD8) {
		std::printf("PASS: JPEG SOI present\n");
	} else {
		std::printf("FAIL: not a JPEG (no SOI)\n");
		return 1;
	}
	if (base64 > 8u * 1024u * 1024u) {
		std::printf("FAIL: the message would exceed the bridge's 8 MiB limit\n");
		return 1;
	}
	return 0;
}
