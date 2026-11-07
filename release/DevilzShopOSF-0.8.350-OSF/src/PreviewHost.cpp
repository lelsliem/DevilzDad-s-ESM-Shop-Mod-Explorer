#include "PCH.h"

#include "PreviewHost.h"

#include "LiveModel.h"
#include "OfflineModel.h"
#include "PngWrite.h"
#include "ShopExplorer.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace SFE::OsfPreview
{
	namespace
	{
		// One render per whole degree. A drag now streams frames instead of
		// waiting for the pointer to stop, and 5-degree steps read as jumps at
		// that rate; a degree is below what the eye catches in a slow orbit and
		// keeps the cache dense enough that a small nudge is a cache hit.
		constexpr float kAngleStep = 1.0f;
		// Frames kept for the session, keyed by camera stop. A drag visits many
		// neighbouring stops, so this is larger than a 5-degree cache needed;
		// 96 frames of a 640x640 PNG is a few tens of MB at worst and keeps a
		// whole orbit resident.
		constexpr std::size_t kFrameCacheLimit = 96;

		std::mutex g_mutex;
		bool g_tried = false;
		bool g_ready = false;

		ID3D12Device* g_device = nullptr;
		ID3D12CommandQueue* g_queue = nullptr;
		ID3D12CommandAllocator* g_allocator = nullptr;
		ID3D12GraphicsCommandList* g_list = nullptr;
		ID3D12DescriptorHeap* g_srvHeap = nullptr;
		ID3D12Fence* g_fence = nullptr;
		HANDLE g_event = nullptr;

		struct CachedItem
		{
			std::vector<SFE::OfflineModel::DirectMesh> meshes;
			std::string label;
			bool weapon{ false };
			std::string error;
		};
		std::unordered_map<std::uint32_t, CachedItem> g_items;

		struct Frame
		{
			std::vector<std::uint8_t> png;
			std::uint32_t width{ 0 };
			std::uint32_t height{ 0 };
		};
		// Rendered frames, so a repeat request for the same camera stop (the
		// page re-asking after a DOM rebuild) does not pay for another render.
		// g_frameOrder records insertion order for eviction.
		std::unordered_map<std::string, Frame> g_frames;
		std::vector<std::string> g_frameOrder;

		// D3D12 device. The game's own device is not ours to use: it is created
		// with the game's queue and heaps, and a content plugin borrowing it
		// would race the engine. A second device is legal and independent; the
		// renderer only ever touches resources it created on it.
		bool CreateDevice(ID3D12Device** a_out)
		{
			if (SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(a_out)))) {
				return true;
			}
			// Software fallback: a machine whose adapter refuses a second device
			// still gets previews, just slower.
			IDXGIFactory4* factory = nullptr;
			if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
				return false;
			}
			IDXGIAdapter* warp = nullptr;
			const auto hr = factory->EnumWarpAdapter(IID_PPV_ARGS(&warp));
			factory->Release();
			if (FAILED(hr) || !warp) {
				return false;
			}
			const auto ok = SUCCEEDED(D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(a_out)));
			warp->Release();
			return ok;
		}

		bool EnsureDevice()
		{
			if (g_ready) {
				return true;
			}
			if (g_tried) {
				return false;
			}
			g_tried = true;

			OfflineModel::LoadConfig();
			LiveModel::LoadConfig();

			if (!CreateDevice(&g_device)) {
				REX::ERROR("SFShopExplorer OSF preview: no D3D12 device available");
				return false;
			}
			D3D12_COMMAND_QUEUE_DESC qd{};
			qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
			if (FAILED(g_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue))) ||
				FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_allocator))) ||
				FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_allocator, nullptr,
					IID_PPV_ARGS(&g_list))) ||
				FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) {
				REX::ERROR("SFShopExplorer OSF preview: command objects failed");
				return false;
			}
			g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (!g_event) {
				REX::ERROR("SFShopExplorer OSF preview: fence event failed");
				return false;
			}

			D3D12_DESCRIPTOR_HEAP_DESC hd{};
			hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			// OfflineModel publishes its preview into kSrvSlot (20); 64 leaves
			// room for that and for any future slot the renderer adds.
			hd.NumDescriptors = 64;
			hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			if (FAILED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_srvHeap)))) {
				REX::ERROR("SFShopExplorer OSF preview: descriptor heap failed");
				return false;
			}
			OfflineModel::DeviceReady(g_device, g_srvHeap,
				g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
			g_ready = true;
			REX::INFO("SFShopExplorer OSF preview: offscreen renderer ready");
			return true;
		}

		// Geometry for one form, parsed from the game's own asset bytes through
		// LiveModel (the same parser the overlay's Plan B uses). Cached for the
		// session: rotating an item must not re-read its NIFs from the BA2s.
		const CachedItem& Item(std::uint32_t a_formID)
		{
			const auto found = g_items.find(a_formID);
			if (found != g_items.end()) {
				return found->second;
			}
			CachedItem item;
			const auto source = ShopBrowser::GetSingleton()->OsfPreviewSourceOf(a_formID);
			item.label = source.label;
			item.weapon = source.weapon;
			if (!source.ok) {
				item.error = source.error.empty() ? "this record carries no 3D model" : source.error;
				return g_items.emplace(a_formID, std::move(item)).first->second;
			}
			std::vector<SFE::LiveModel::LiveMesh> meshes;
			if (!SFE::LiveModel::BuildModels(source.paths, meshes) || meshes.empty()) {
				item.error = "the model could not be read from this install";
				return g_items.emplace(a_formID, std::move(item)).first->second;
			}
			item.meshes.reserve(meshes.size());
			for (const auto& mesh : meshes) {
				if (mesh.pos.size() < 9 || mesh.indices.size() < 3) {
					continue;
				}
				SFE::OfflineModel::DirectMesh direct;
				direct.name = mesh.name;
				// The item's material swap: the same NIF serves the stock
				// garment and the recoloured variant.
				const auto swapped = SFE::LiveModel::MaterialSwapFor(a_formID, mesh.matPath);
				direct.matPath = swapped.empty() ? mesh.matPath : swapped;
				direct.pos = mesh.pos;
				direct.uv = mesh.uv;
				direct.indices = mesh.indices;
				item.meshes.push_back(std::move(direct));
			}
			if (item.meshes.empty()) {
				item.error = "the model has no drawable geometry";
			}
			REX::INFO("SFShopExplorer OSF preview: {:08X} '{}' {} mesh(es) from {} path(s)", a_formID, item.label,
				item.meshes.size(), source.paths.size());
			return g_items.emplace(a_formID, std::move(item)).first->second;
		}

		// --- PNG ------------------------------------------------------------
		//
		// Written by hand (src/PngWrite.h) rather than through WIC. WIC's encoder
		// negotiates its own pixel format and can answer with something other
		// than the 32-bit RGBA it was handed; that refusal was a silent "could
		// not be encoded" with no way to see why from inside the game.
		bool EncodePng(const std::vector<std::uint8_t>& a_rgba, const std::uint32_t a_width,
			const std::uint32_t a_height, std::vector<std::uint8_t>& a_out)
		{
			return SFE::Png::EncodeRgba(a_rgba, a_width, a_height, a_out);
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

		std::string FileNameFor(std::uint32_t a_formID, int a_yaw, int a_pitch, int a_zoom)
		{
			return std::format("{:08X}_{}_{}_{}.png", a_formID, a_yaw, a_pitch, a_zoom);
		}

		void ResetList()
		{
			if (!g_allocator || !g_list) {
				return;
			}
			g_allocator->Reset();
			g_list->Reset(g_allocator, nullptr);
		}

		void RememberFrame(const std::string& a_name, Frame a_frame)
		{
			if (g_frames.find(a_name) == g_frames.end()) {
				if (g_frames.size() >= kFrameCacheLimit && !g_frameOrder.empty()) {
					g_frames.erase(g_frameOrder.front());
					g_frameOrder.erase(g_frameOrder.begin());
				}
				g_frameOrder.push_back(a_name);
			}
			g_frames[a_name] = std::move(a_frame);
		}

	}

	Image Render(const std::uint32_t a_formID, const float a_yaw, const float a_pitch, const float a_zoom)
	{
		std::scoped_lock lock(g_mutex);
		Image out;
		if (!EnsureDevice()) {
			out.error = "the preview renderer could not start";
			return out;
		}
		const auto& item = Item(a_formID);
		if (!item.error.empty()) {
			out.error = item.error;
			return out;
		}

		// Quantise the camera so a slow drag lands on the same frame twice
		// rather than re-rendering a fraction of a degree apart.
		const auto yaw = static_cast<int>(std::lround(a_yaw / kAngleStep) * kAngleStep);
		const auto pitch = static_cast<int>(std::lround(a_pitch / kAngleStep) * kAngleStep);
		const auto zoomPercent = static_cast<int>(std::lround(std::clamp(a_zoom, 0.35f, 4.0f) * 100.0f));
		const auto name = FileNameFor(a_formID, yaw, pitch, zoomPercent);
		if (const auto cached = g_frames.find(name); cached != g_frames.end()) {
			out.ok = true;
			out.width = cached->second.width;
			out.height = cached->second.height;
			out.dataUrl = "data:image/png;base64," + Base64(cached->second.png);
			return out;
		}

		OfflineModel::RequestPreviewDirect(item.meshes, item.label, item.weapon, false);
		// Camera AFTER the request: RequestPreviewDirect resets the orbit so a
		// new item starts from the INI framing, and it would otherwise throw
		// away the angles this frame asked for.
		OfflineModel::ResetOrbit();
		OfflineModel::AddOrbit(static_cast<float>(yaw), static_cast<float>(pitch));
		OfflineModel::AddZoom(static_cast<float>(zoomPercent) / 100.0f);
		// First pass publishes the geometry and asks the task thread for its
		// textures; the second pass draws with whatever is now resident.
		OfflineModel::ServicePending();
		ResetList();
		OfflineModel::UploadGPU(g_list);
		OfflineModel::ServicePending();
		OfflineModel::AddOrbit(0.0f, 0.0f);  // force one more frame, textures now warm
		OfflineModel::UploadGPU(g_list);

		// A first request in a session can arrive while the model is still being
		// published, in which case UploadGPU above had nothing to draw and there
		// is no frame to read. Rather than answering "no preview" for a screen
		// the page will keep showing, rebuild the list and draw once more.
		if (!OfflineModel::HasRenderableFrame()) {
			REX::INFO("SFShopExplorer OSF preview: {:08X} had no frame on the first pass; drawing again", a_formID);
			ResetList();
			OfflineModel::ServicePending();
			OfflineModel::AddOrbit(0.0f, 0.0f);
			OfflineModel::UploadGPU(g_list);
			OfflineModel::ServicePending();
			OfflineModel::AddOrbit(0.0f, 0.0f);
			OfflineModel::UploadGPU(g_list);
		}

		std::vector<std::uint8_t> rgba;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		if (!OfflineModel::CaptureFramePixels(g_queue, g_list, g_fence, g_event, rgba, width, height)) {
			// Silent on purpose before: a page that only ever saw "no preview"
			// could not be told apart from a renderer that never drew.
			REX::WARN("SFShopExplorer OSF preview: {:08X} produced no frame (the renderer did not publish a drawable frame)", a_formID);
			out.error = "the preview render produced no frame";
			return out;
		}
		std::vector<std::uint8_t> png;
		if (!EncodePng(rgba, width, height, png)) {
			REX::WARN("SFShopExplorer OSF preview: {:08X} frame {}x{} could not be encoded", a_formID, width, height);
			out.error = "the preview frame could not be encoded";
			return out;
		}
		const auto bytes = png.size();
		// The page cannot load a file this mod writes: OSF UI serves a view
		// from a snapshot it takes at startup (and, in developer mode, from
		// a mirror it watches), so a PNG saved beside the page is never
		// served - and writing into that folder makes the watcher reload the
		// interface mid-session. The bytes ride back in the reply instead.
		RememberFrame(name, Frame{ std::move(png), width, height });
		const auto& frame = g_frames.at(name);
		out.ok = true;
		out.width = frame.width;
		out.height = frame.height;
		out.dataUrl = "data:image/png;base64," + Base64(frame.png);
		REX::INFO("SFShopExplorer OSF preview: {:08X} yaw={} pitch={} zoom={}% -> {}x{} ({} bytes)", a_formID, yaw,
			pitch, zoomPercent, frame.width, frame.height, bytes);
		return out;
	}

	void Shutdown()
	{
		std::scoped_lock lock(g_mutex);
		g_items.clear();
		g_frames.clear();
		g_frameOrder.clear();
		if (g_list) { g_list->Release(); g_list = nullptr; }
		if (g_allocator) { g_allocator->Release(); g_allocator = nullptr; }
		if (g_srvHeap) { g_srvHeap->Release(); g_srvHeap = nullptr; }
		if (g_fence) { g_fence->Release(); g_fence = nullptr; }
		if (g_queue) { g_queue->Release(); g_queue = nullptr; }
		if (g_device) { g_device->Release(); g_device = nullptr; }
		if (g_event) { CloseHandle(g_event); g_event = nullptr; }
		g_ready = false;
		g_tried = false;
	}
}
