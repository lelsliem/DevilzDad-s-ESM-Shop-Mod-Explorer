#include "PCH.h"
#include "ImguiOverlay.h"
#include "ShopExplorer.h"
#include "ItemCapture.h"
#include "OfflineModel.h"
#include "ShopAudio.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>
#ifdef ERROR
#undef ERROR
#endif
#include <wincodec.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include "RE/C/CreationRenderer.h"
#include "imgui.h"
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace SFE::Overlay
{
	HRESULT STDMETHODCALLTYPE PresentThunk(IDXGISwapChain*, UINT, UINT);

	namespace
	{
		using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
		using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
		using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

		constexpr std::size_t kPresentSlot = 8;
		constexpr std::size_t kCreateSwapChainSlot = 10;
		constexpr std::size_t kCreateSwapChainForHwndSlot = 15;

		std::mutex g_mutex;
		std::mutex g_inputMutex;
struct InputMessage
	{
		UINT message;
		WPARAM wparam;
		LPARAM lparam;
		// Set for WM_CHAR that QueueTypedCharacter() produced. Those are only a
		// fallback, so they are dropped when the host supplies its own.
		bool synthesized{ false };
	};
		std::deque<InputMessage> g_inputMessages;
		std::atomic<bool> g_factoryInstalled{ false };
		std::atomic<bool> g_presentInstalled{ false };
		std::atomic<CreateSwapChainFn> g_originalCreateSwapChain{ nullptr };
		std::atomic<CreateSwapChainForHwndFn> g_originalCreateSwapChainForHwnd{ nullptr };
		std::atomic<PresentFn> g_originalPresent{ nullptr };

		ID3D12Device* g_device{ nullptr };
		ID3D12CommandQueue* g_queue{ nullptr };
		IDXGISwapChain* g_swapChain{ nullptr };
		HWND g_hwnd{ nullptr };
		ID3D12DescriptorHeap* g_rtvHeap{ nullptr };
		ID3D12DescriptorHeap* g_srvHeap{ nullptr };
		ID3D12CommandAllocator* g_allocator{ nullptr };
		ID3D12GraphicsCommandList* g_commandList{ nullptr };
		ID3D12Fence* g_fence{ nullptr };
		HANDLE g_fenceEvent{ nullptr };
		UINT64 g_fenceValue{ 0 };
		std::atomic<bool> g_imguiReady{ false };
		std::atomic<bool> g_drawingEnabled{ false };
		bool g_frameInProgress{ false };
		std::atomic<WNDPROC> g_originalWndProc{ nullptr };
		int g_toggleKey{ VK_PRIOR };
float g_uiScale{ 1.0f };
	float g_appliedUiScale{ 1.0f };
	float g_backdropAlpha{ 150.0f };

		std::atomic<float> g_virtualX{ 0.0f };
		std::atomic<float> g_virtualY{ 0.0f };
		std::atomic<int> g_renderMode{ 0 };
		std::atomic<bool> g_drawStageLogged{ false };
		std::atomic<bool> g_captureInput{ false };
		// Set when input capture turns on; consumed at the top of the next ImGui
		// frame on the render thread, where touching the context is safe. Every
		// close path can therefore leave whatever keys it likes behind without
		// poisoning the next session's text input.
		std::atomic<bool> g_resetImGuiInput{ false };
		// Log-once latch for the stale-focus re-sync below.
		std::atomic<bool> g_focusResyncLogged{ false };
		std::atomic<bool> g_mouseDrawCursor{ false };
		std::uint32_t g_srvIncrement{ 0 };
		std::uint32_t g_nextSrvSlot{ 1 };
		bool g_settingsLoaded{ false };

		D3D12_CPU_DESCRIPTOR_HANDLE RtvHandle()
		{
			return g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
		}

		void AllocateFontDescriptor(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* a_cpu, D3D12_GPU_DESCRIPTOR_HANDLE* a_gpu)
		{
			// Every font ImGui loads is baked into one atlas texture, so this is
			// called once no matter how many sizes were added below. Slot 0 stays
			// reserved for it.
			*a_cpu = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
			*a_gpu = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
		}

// Extra sizes for the header banner, plus the three body sizes the font-size
	// selector switches between. ImGui has no runtime font scaling
	// (SetWindowFontScale does not exist in this version), so every size the UI
	// can show is a separate face baked into the atlas at load time and chosen
	// with PushFont. That is also what makes the selector instant: no atlas
	// rebuild, no device resources touched.
	ImFont* g_fontTitle{ nullptr };
	ImFont* g_fontCaption{ nullptr };
	ImFont* g_bodyFonts[4]{};
	int g_bodyFontSize{ 1 };  // 0 small, 1 medium, 2 large, 3 extra large

		ImFont* LoadUiFont(const ImWchar* a_ranges, float a_px)
		{
			ImFontConfig cfg;
			cfg.OversampleH = 4;
			cfg.OversampleV = 4;
			cfg.PixelSnapH = false;
			for (const wchar_t* file : { L"segoeui.ttf", L"seguisb.ttf", L"arial.ttf" }) {
				const auto path = std::filesystem::path(L"C:\\Windows\\Fonts") / file;
				if (std::filesystem::exists(path)) {
					if (auto* f = ImGui::GetIO().Fonts->AddFontFromFileTTF(path.string().c_str(), a_px, &cfg, a_ranges)) {
						return f;
					}
				}
			}
			return nullptr;
		}

		struct NativeTexture
		{
			std::filesystem::path path;
			UINT width{ 0 };
			UINT height{ 0 };
			std::vector<std::uint8_t> pixels;
			ID3D12Resource* resource{ nullptr };
			ID3D12Resource* upload{ nullptr };
			D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
			bool uploaded{ false };
		};
		std::vector<NativeTexture> g_textures;
		bool g_texturesAttempted{ false };

		std::filesystem::path SettingsPath();
		NativeTexture* FindTexture(const char* a_name);

		void FreeFontDescriptor(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE)
		{
			// The native shop owns a fixed descriptor heap for the lifetime of the
			// overlay. ImGui's font descriptor stays in slot zero.
		}

		ImTextureID TextureId(const char* a_name)
		{
			const auto* texture = FindTexture(a_name);
			return texture ? static_cast<ImTextureID>(texture->gpu.ptr) : static_cast<ImTextureID>(0);
		}

		NativeTexture* FindTexture(const char* a_name)
		{
			for (auto& texture : g_textures) {
				if (texture.path.filename() == a_name) {
					return &texture;
				}
			}
			return nullptr;
		}

		bool LoadTexturePixels(NativeTexture& a_texture, UINT a_maxEdge)
		{
			IWICImagingFactory* factory = nullptr;
			if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) || !factory) {
				return false;
			}
			IWICBitmapDecoder* decoder = nullptr;
			IWICBitmapFrameDecode* frame = nullptr;
			IWICBitmapScaler* scaler = nullptr;
			IWICFormatConverter* converter = nullptr;
			bool success = false;
			if (SUCCEEDED(factory->CreateDecoderFromFilename(a_texture.path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)) && decoder &&
				SUCCEEDED(decoder->GetFrame(0, &frame)) && frame) {
				// The art ships at 1024px but draws at 96-160px. Uploading the
				// full-size source makes the GPU minify without mips (jagged,
				// shimmering logo edges), so cap the long edge per draw size
				// with WIC's cubic scaler instead.
				IWICBitmapSource* source = frame;
				UINT srcWidth = 0;
				UINT srcHeight = 0;
				if (SUCCEEDED(frame->GetSize(&srcWidth, &srcHeight)) && srcWidth > 0 && srcHeight > 0 &&
					a_maxEdge > 0 && (srcWidth > a_maxEdge || srcHeight > a_maxEdge)) {
					const double scale = static_cast<double>(a_maxEdge) / (std::max)(srcWidth, srcHeight);
					const UINT newWidth = (std::max)(1u, static_cast<UINT>(srcWidth * scale + 0.5));
					const UINT newHeight = (std::max)(1u, static_cast<UINT>(srcHeight * scale + 0.5));
					if (SUCCEEDED(factory->CreateBitmapScaler(&scaler)) && scaler &&
						SUCCEEDED(scaler->Initialize(frame, newWidth, newHeight, WICBitmapInterpolationModeHighQualityCubic))) {
						source = scaler;
					}
				}
				if (SUCCEEDED(factory->CreateFormatConverter(&converter)) && converter &&
					SUCCEEDED(converter->Initialize(source, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
					UINT width = 0;
					UINT height = 0;
					if (SUCCEEDED(converter->GetSize(&width, &height)) && width > 0 && height > 0) {
						a_texture.width = width;
						a_texture.height = height;
						a_texture.pixels.resize(static_cast<std::size_t>(width) * height * 4);
						success = SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(a_texture.pixels.size()), a_texture.pixels.data()));
					}
				}
			}
			if (converter) converter->Release();
			if (scaler) scaler->Release();
			if (frame) frame->Release();
			if (decoder) decoder->Release();
			factory->Release();
			return success;
		}

		void CreateNativeTextureResources()
		{
			if (g_texturesAttempted || !g_device || !g_srvHeap) {
				return;
			}
			g_texturesAttempted = true;
			const auto root = SettingsPath().parent_path() / L"SFShopExplorerAssets";
			// Long-edge upload caps matched to the draw sizes in ShopExplorer
			// (logo 96, chest <=160, credit 40x20); the space backdrop fills
			// the window, so it keeps its native resolution.
			const std::pair<const wchar_t*, UINT> assets[]{
				{ L"ShopLogo.png", 256 },
				{ L"Chest.png", 320 },
				{ L"CreditIcon.png", 128 },
				{ L"space.png", 0 },
				// Per-theme variants. Naming is <Stem>_<themelowercase>.png; a
				// missing file only warns and the shared texture is used, so
				// these are additive and safe to add one theme at a time.
				{ L"Chest_heaven.png", 320 },
				{ L"space_heaven.png", 0 },
				{ L"Chest_hell.png", 320 },
				{ L"space_hell.png", 0 },
				{ L"Chest_starfield.png", 320 },
				{ L"space_starfield.png", 0 },
			};
			for (const auto& [name, maxEdge] : assets) {
				NativeTexture texture{};
				texture.path = root / name;
				if (!LoadTexturePixels(texture, maxEdge)) {
					REX::WARN("SFShopExplorer: optional image asset unavailable: {}", texture.path.string());
					continue;
				}
				D3D12_RESOURCE_DESC desc{};
				desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
				desc.Width = texture.width;
				desc.Height = texture.height;
				desc.DepthOrArraySize = 1;
				desc.MipLevels = 1;
				desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
				desc.SampleDesc.Count = 1;
				desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
				D3D12_HEAP_PROPERTIES defaultHeap{};
				defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
				if (FAILED(g_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture.resource)))) {
					continue;
				}
				const auto uploadPitch = (static_cast<std::size_t>(texture.width) * 4u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) & ~(static_cast<std::size_t>(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) - 1u);
				const auto uploadSize = uploadPitch * texture.height;
				D3D12_RESOURCE_DESC uploadDesc{};
				uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				uploadDesc.Width = uploadSize;
				uploadDesc.Height = 1;
				uploadDesc.DepthOrArraySize = 1;
				uploadDesc.MipLevels = 1;
				uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
				uploadDesc.SampleDesc.Count = 1;
				uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
				D3D12_HEAP_PROPERTIES uploadHeap{};
				uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
				if (FAILED(g_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&texture.upload)))) {
					texture.resource->Release();
					texture.resource = nullptr;
					continue;
				}
				void* mapped = nullptr;
				D3D12_RANGE range{ 0, 0 };
				if (FAILED(texture.upload->Map(0, &range, &mapped))) {
					texture.upload->Release(); texture.upload = nullptr;
					texture.resource->Release(); texture.resource = nullptr;
					continue;
				}
				std::byte* mappedBytes = static_cast<std::byte*>(mapped);
				const auto sourcePitch = static_cast<std::size_t>(texture.width) * 4u;
				for (UINT row = 0; row < texture.height; ++row) {
					std::memcpy(mappedBytes + static_cast<std::size_t>(row) * uploadPitch,
						texture.pixels.data() + static_cast<std::size_t>(row) * sourcePitch, sourcePitch);
				}
				texture.upload->Unmap(0, nullptr);
				D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
				srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
				srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				srv.Texture2D.MipLevels = 1;
				auto cpu = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
				cpu.ptr += static_cast<SIZE_T>(g_nextSrvSlot) * g_srvIncrement;
				texture.gpu = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
				texture.gpu.ptr += static_cast<UINT64>(g_nextSrvSlot) * g_srvIncrement;
				g_device->CreateShaderResourceView(texture.resource, &srv, cpu);
				++g_nextSrvSlot;
				g_textures.push_back(std::move(texture));
			}
		}

		void UploadNativeTextures(ID3D12GraphicsCommandList* a_list)
		{
			for (auto& texture : g_textures) {
				if (texture.uploaded || !texture.resource || !texture.upload) continue;
				const auto uploadPitch = (static_cast<std::size_t>(texture.width) * 4u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) & ~(static_cast<std::size_t>(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) - 1u);
				D3D12_TEXTURE_COPY_LOCATION source{};
				source.pResource = texture.upload;
				source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
				source.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
				source.PlacedFootprint.Footprint.Width = texture.width;
				source.PlacedFootprint.Footprint.Height = texture.height;
				source.PlacedFootprint.Footprint.Depth = 1;
				source.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(uploadPitch);
				D3D12_TEXTURE_COPY_LOCATION target{};
				target.pResource = texture.resource;
				target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				target.SubresourceIndex = 0;
				a_list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
				D3D12_RESOURCE_BARRIER barrier{};
				barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				barrier.Transition.pResource = texture.resource;
				barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
				barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
				barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
				a_list->ResourceBarrier(1, &barrier);
				texture.uploaded = true;
			}
		}

		std::filesystem::path SettingsPath()
		{
			HMODULE self = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&Install), &self);
			wchar_t modulePath[MAX_PATH]{};
			GetModuleFileNameW(self, modulePath, MAX_PATH);
			return std::filesystem::path(modulePath).parent_path() / L"SFShopExplorer.ini";
		}

		void LoadSettings()
		{
			if (g_settingsLoaded) {
				return;
			}
			g_settingsLoaded = true;
			const auto path = SettingsPath();
			g_toggleKey = GetPrivateProfileIntW(L"overlay", L"toggleKey", VK_PRIOR, path.c_str());
		g_renderMode.store(std::clamp(static_cast<int>(GetPrivateProfileIntW(L"overlay", L"render", 0, path.c_str())), 0, 4), std::memory_order_release);
		g_captureInput.store(GetPrivateProfileIntW(L"overlay", L"input", 0, path.c_str()) != 0, std::memory_order_release);
			wchar_t scale[32]{};
			GetPrivateProfileStringW(L"overlay", L"uiScale", L"1.0", scale, static_cast<DWORD>(std::size(scale)), path.c_str());
			try {
				g_uiScale = std::clamp(std::stof(scale), 0.75f, 2.0f);
} catch (...) {
			g_uiScale = 1.0f;
		}
		// Backdrop strength. The themed space_*.png art ranges from a very dark
		// nebula to a bright inferno, so one fixed alpha cannot serve them all:
		// a value that looks right behind the dark art blows a bright one out
		// through the semi-transparent panels and the result is unreadable
		// text. Exposed so it can be tuned per theme without a rebuild.
		const int backdrop = GetPrivateProfileIntW(L"overlay", L"backdropAlpha", 150, path.c_str());
		g_backdropAlpha = static_cast<float>(std::clamp(backdrop, 0, 255));
		REX::INFO("SFShopExplorer: settings loaded from {} (toggleKey={}, uiScale={:.2f}, backdropAlpha={:.0f}, renderMode={}, input={})",
			path.string(), g_toggleKey, g_uiScale, g_backdropAlpha,
			g_renderMode.load(std::memory_order_acquire), g_captureInput.load(std::memory_order_acquire));
		}

		// Build 94: settle the Win32 ShowCursor display count at "hidden"
		// without drift. A bare while (ShowCursor(FALSE) >= 0) loop
		// decrements once more every frame even when already hidden,
		// pushing the count ever lower and leaving the game without a
		// usable OS cursor later; this converges to a hidden state and
		// nets zero when already hidden, while still re-hiding if the
		// game bumped the count back up.
		void HideHwCursor()
		{
			int depth = ShowCursor(FALSE);
			while (depth > 0) {
				depth = ShowCursor(FALSE);
			}
			if (depth < -1) {
				ShowCursor(TRUE);
			}
		}

		void SetInputEnabled(bool a_enabled)
		{
			const bool drawing = a_enabled && g_renderMode.load(std::memory_order_acquire) >= 4;
			const bool inputEnabled = drawing && g_captureInput.load(std::memory_order_acquire);
			const bool wasInputEnabled = g_mouseDrawCursor.exchange(inputEnabled, std::memory_order_acq_rel);
			g_drawingEnabled.store(drawing, std::memory_order_release);
			if (inputEnabled && !wasInputEnabled) {
				g_resetImGuiInput.store(true, std::memory_order_release);
				LoadSettings();
				POINT point{};
				if (g_hwnd && GetCursorPos(&point) && ScreenToClient(g_hwnd, &point)) {
					g_virtualX.store(static_cast<float>(point.x), std::memory_order_relaxed);
					g_virtualY.store(static_cast<float>(point.y), std::memory_order_relaxed);
				}
				ClipCursor(nullptr);
				// Build 94: the ImGui virtual cursor (raw-input driven,
				// drawn by DrawVirtualCursor) is the only cursor while
				// input is enabled - showing a HW cursor here produced
				// a second, stale OS arrow over the overlay.
				HideHwCursor();
			} else if (!inputEnabled && wasInputEnabled) {
				HideHwCursor();
			}
		}

		void QueueInputMessage(UINT a_message, WPARAM a_wparam, LPARAM a_lparam, bool a_synthesized = false)
		{
			std::scoped_lock lock(g_inputMutex);
			if (g_inputMessages.size() >= 256) {
				g_inputMessages.pop_front();
			}
			g_inputMessages.push_back({ a_message, a_wparam, a_lparam, a_synthesized });
		}

		// GetTickCount of the last real WM_CHAR the host produced. The game does
		// call TranslateMessage on some code paths (just not the one the shop is
		// open on), and when it does, synthesizing as well gives every keystroke
		// two characters.
		//
		// This used to be a session-wide latch, which broke search after an ESC
		// close: ESC leaked to the game, the game opened PauseMenu, and while a
		// game menu is up its message pump runs TranslateMessage. One real
		// WM_CHAR in that window latched synthesis off for good, so once the
		// menu closed - and with it the pump - typing produced nothing. A
		// timestamp decays instead: the host counts as translating only while
		// its characters keep arriving, and synthesis comes back when they stop.
		std::atomic<std::uint32_t> g_lastHostCharTick{ 0 };
		constexpr std::uint32_t kHostTranslateWindowMs = 500;

		bool HostTranslatingRecently()
		{
			const auto last = g_lastHostCharTick.load(std::memory_order_relaxed);
			return last != 0 && GetTickCount() - last < kHostTranslateWindowMs;
		}

		// Keys whose key-down this overlay swallowed stay swallowed until their
		// matching key-up, even after the shop has closed and input capture is
		// off. Swallowing the down but passing the up leaves the game with an
		// orphan ESC up edge, which is what opened PauseMenu right after an ESC
		// close (one `existing menu callback name=PauseMenu` in the log, 0.8 s
		// after the only ESC close of the session).
		std::atomic<std::uint64_t> g_swallowedKeyDowns[4];

		// Raw key edges for the shop's own handling (Escape, catalog arrows).
		// ImGui can lose key events outright - a stale focus loss makes it clear
		// every key and every queued character each frame, and the log from the
		// popped-out preview session shows whole minutes with no key events at all
		// while mouse messages kept arriving. These bits are set from the same
		// WM_KEYDOWN stream the ImGui backend reads and are consumed by the shop
		// directly, so those two keys cannot be lost with it.
		std::atomic<std::uint64_t> g_rawKeyPresses[4];
		// First few key-downs of a session are logged: this is what distinguishes
		// "the key never reached the game window" from "ImGui dropped it".
		std::atomic<std::uint32_t> g_rawKeyLogBudget{ 12 };

		void RememberSwallowedKeyDown(WPARAM a_wparam)
		{
			const auto vk = static_cast<std::uint32_t>(a_wparam) & 0xFFu;
			g_swallowedKeyDowns[vk >> 6].fetch_or(1ull << (vk & 63), std::memory_order_acq_rel);
		}

		bool ConsumeSwallowedKeyUp(WPARAM a_wparam)
		{
			const auto vk = static_cast<std::uint32_t>(a_wparam) & 0xFFu;
			const std::uint64_t mask = 1ull << (vk & 63);
			return (g_swallowedKeyDowns[vk >> 6].fetch_and(~mask, std::memory_order_acq_rel) & mask) != 0;
		}

		// Fallback character translation. ImGui got key *events* from WM_KEYDOWN
		// all along (which is why the arrow keys and Enter worked) but an
		// InputText needs characters, and WM_CHAR only exists if something
		// calls TranslateMessage. While the host is not translating (see
		// HostTranslatingRecently), this stands in for that.
		//
		// Translates from the physical key state rather than the wparam virtual
		// key, because the character depends on the active layout, Shift,
		// CapsLock and AltGr. ToUnicode consumes dead-key state, so it is only
		// called from the make (key-down) side.
		void QueueTypedCharacter(WPARAM a_wparam)
		{
			const BYTE vk = static_cast<BYTE>(a_wparam & 0xFF);
			BYTE keyboard[256]{};
			// GetKeyboardState fills the *calling thread's* state, so the toggles
			// have to be folded in explicitly - Shift/CapsLock live there.
			if (GetKeyboardState(keyboard) == 0) {
				return;
			}
			const BYTE vkShift = static_cast<BYTE>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
			const bool shifted = (keyboard[VK_SHIFT] & 0x80) != 0 || (keyboard[VK_CONTROL] & 0x80) != 0;
			if (shifted) {
				keyboard[vkShift] |= 0x80;
			}
			wchar_t buffer[4]{};
			const int produced = ToUnicodeEx(vk, keyboard[vkShift], keyboard, buffer, 4, 0, GetKeyboardLayout(0));
			if (produced > 0) {
				for (int i = 0; i < produced; ++i) {
					QueueInputMessage(WM_CHAR, buffer[i], 1, true);
				}
			}
		}

		void ConsumeInputMessages()
		{
			std::deque<InputMessage> messages;
			{
				std::scoped_lock lock(g_inputMutex);
				messages.swap(g_inputMessages);
			}
			// The latch only catches the host's translation from the second keystroke
			// onwards, because the first one may already have been doubled. Drop
			// any synthesized character that is followed in the same batch by a
			// real WM_CHAR carrying the same code point, which closes that gap
			// without delaying anything.
			for (std::size_t i = 0; i < messages.size(); ++i) {
				const auto& input = messages[i];
				if (input.message == WM_MOUSEMOVE || input.message == WM_MOUSELEAVE || input.message == WM_NCMOUSELEAVE) {
					continue;
				}
				if (input.synthesized) {
					const bool hostSuppliedIt = std::any_of(
						messages.begin() + static_cast<std::ptrdiff_t>(i) + 1, messages.end(),
						[&](const InputMessage& later) {
							return (later.message == WM_CHAR || later.message == WM_UNICHAR) &&
								!later.synthesized && later.wparam == input.wparam;
						});
					if (hostSuppliedIt) {
						continue;
					}
				}
ImGui_ImplWin32_WndProcHandler(g_hwnd, input.message, input.wparam, input.lparam);
			}
		}

	bool IsOverlayInputMessage(UINT a_message)
		{
			switch (a_message) {
			case WM_INPUT:
			case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
			case WM_CHAR: case WM_UNICHAR:
			case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
			case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
			case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
			case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
			case WM_MOUSEMOVE: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
			case WM_MOUSELEAVE: case WM_NCMOUSELEAVE:
				return true;
			default:
				return false;
			}
		}

		LRESULT WINAPI WindowProc(HWND a_hwnd, UINT a_message, WPARAM a_wparam, LPARAM a_lparam)
		{
			// SFSE's supported messaging API has no unload event. WM_DESTROY is a
			// normal game-thread callback, so stop/join audio here before SFSE's
			// final FreeLibrary rather than blocking from DllMain/under loader lock.
			if (a_message == WM_DESTROY && a_hwnd == g_hwnd) {
				ShopAudio::Shutdown();
			}
			// Focus transitions are the one input event that silently disables all
			// keyboard handling in ImGui (ClearInputKeys + dropped characters), so
			// they are logged as they happen. Rare enough to be free.
			if ((a_message == WM_SETFOCUS || a_message == WM_KILLFOCUS) && a_hwnd == g_hwnd) {
				REX::INFO("SFShopExplorer: game window focus {} (drawing={} capture={})",
					a_message == WM_SETFOCUS ? "gained" : "lost",
					g_drawingEnabled.load(std::memory_order_acquire),
					g_captureInput.load(std::memory_order_acquire));
			}
			if (g_drawingEnabled.load(std::memory_order_acquire) && g_captureInput.load(std::memory_order_acquire) && a_message == WM_SETCURSOR) {
				SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
				return TRUE;
			}
			if (g_drawingEnabled.load(std::memory_order_acquire) && g_captureInput.load(std::memory_order_acquire) && a_message == WM_INPUT) {
				UINT size = sizeof(RAWINPUT);
				BYTE buffer[sizeof(RAWINPUT)]{};
				if (GetRawInputData(reinterpret_cast<HRAWINPUT>(a_lparam), RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1)) {
					auto* raw = reinterpret_cast<RAWINPUT*>(buffer);
					if (raw->header.dwType == RIM_TYPEMOUSE) {
						std::scoped_lock lock(g_inputMutex);
						const auto& mouse = raw->data.mouse;
						if (mouse.usFlags == MOUSE_MOVE_RELATIVE) {
							g_virtualX.store(g_virtualX.load(std::memory_order_relaxed) + static_cast<float>(static_cast<short>(mouse.lLastX)), std::memory_order_relaxed);
							g_virtualY.store(g_virtualY.load(std::memory_order_relaxed) + static_cast<float>(static_cast<short>(mouse.lLastY)), std::memory_order_relaxed);
						} else if (mouse.usFlags & MOUSE_MOVE_ABSOLUTE) {
							RECT rect{};
							GetClientRect(a_hwnd, &rect);
							g_virtualX.store(static_cast<float>(mouse.lLastX) / 65535.0f * static_cast<float>(rect.right), std::memory_order_relaxed);
							g_virtualY.store(static_cast<float>(mouse.lLastY) / 65535.0f * static_cast<float>(rect.bottom), std::memory_order_relaxed);
						}
						RECT rect{};
						GetClientRect(a_hwnd, &rect);
						g_virtualX.store(std::clamp(g_virtualX.load(std::memory_order_relaxed), 0.0f, static_cast<float>(std::max<LONG>(rect.right - 1, 0))), std::memory_order_relaxed);
						g_virtualY.store(std::clamp(g_virtualY.load(std::memory_order_relaxed), 0.0f, static_cast<float>(std::max<LONG>(rect.bottom - 1, 0))), std::memory_order_relaxed);
					}
				}
				return 0;
			}
			// Key-ups of keys this overlay swallowed are consumed here, before the
			// capture check: the shop may already be closed by the time the player
			// releases ESC, and the game must not see that up edge on its own.
			if ((a_message == WM_KEYUP || a_message == WM_SYSKEYUP) && ConsumeSwallowedKeyUp(a_wparam)) {
				return 0;
			}
			if (g_drawingEnabled.load(std::memory_order_acquire) && g_captureInput.load(std::memory_order_acquire) && IsOverlayInputMessage(a_message)) {
				// A real WM_CHAR means the host translates right now, so stand down
				// for the next few hundred milliseconds (see g_lastHostCharTick).
				if (a_message == WM_CHAR || a_message == WM_UNICHAR) {
					g_lastHostCharTick.store(GetTickCount(), std::memory_order_relaxed);
				}
				if (a_message == WM_KEYDOWN || a_message == WM_SYSKEYDOWN) {
					const auto vk = static_cast<std::uint32_t>(a_wparam) & 0xFFu;
					// Bit 30 of lParam is "the key was already down", i.e. this is an
					// auto-repeat. Repeats are the navigation behaviour for the arrow
					// keys, but Escape must stay one press = one action, otherwise
					// holding it walks back through the pages.
					const bool repeat = (static_cast<std::uintptr_t>(a_lparam) & (1u << 30)) != 0;
					if (!repeat || vk != VK_ESCAPE) {
						g_rawKeyPresses[vk >> 6].fetch_or(1ull << (vk & 63), std::memory_order_acq_rel);
					}
					if (g_rawKeyLogBudget.fetch_sub(1, std::memory_order_acq_rel) > 0) {
						REX::INFO("SFShopExplorer: overlay key down vk={}{} (drawing={} capture={})",
							vk, repeat ? " repeat" : "",
							g_drawingEnabled.load(std::memory_order_acquire),
							g_captureInput.load(std::memory_order_acquire));
					}
					RememberSwallowedKeyDown(a_wparam);
				}
				QueueInputMessage(a_message, a_wparam, a_lparam);
				// Synthesize the character this key-down produces, unless the host
				// has already shown that it does its own translation.
				//
				// Only on the make side, and never for keys that have no
				// character - control keys, function keys and the arrows are
				// already handled as key events, and mapping them through
				// ToUnicode would emit stray control bytes into the text field.
				if (a_message == WM_KEYDOWN && !HostTranslatingRecently()) {
					const BYTE vk = static_cast<BYTE>(a_wparam & 0xFF);
					const bool isControlKey =
						(vk >= VK_F1 && vk <= VK_F24) ||
						vk == VK_SPACE || vk == VK_RETURN || vk == VK_TAB || vk == VK_ESCAPE ||
						vk == VK_BACK || vk == VK_DELETE || vk == VK_INSERT ||
						vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT ||
						vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN ||
						(vk >= VK_LSHIFT && vk <= VK_RMENU) || vk == VK_PAUSE || vk == VK_SCROLL;
					if (!isControlKey) {
						QueueTypedCharacter(a_wparam);
					}
				}
				return 0;
			}
			const auto original = g_originalWndProc.load(std::memory_order_acquire);
			return original ? CallWindowProcW(original, a_hwnd, a_message, a_wparam, a_lparam) : DefWindowProcW(a_hwnd, a_message, a_wparam, a_lparam);
		}

		bool InitImGui()
		{
			if (g_imguiReady) {
				return true;
			}
			if (!g_device || !g_queue || !g_hwnd) {
				return false;
			}

				const auto descriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
				if (descriptorSize == 0) {
					REX::ERROR("SFShopExplorer: invalid SRV descriptor increment");
					return false;
				}
				g_srvIncrement = descriptorSize;

			D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
			srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			// 0 = ImGui font, 1..4 = static UI art, 12..19 reserved for the
			// item-capture module (ItemCapture::kMaxSlots = 8), 20 = offline 3D
			// preview (OfflineModel::kSrvSlot). Sized to cover all three with
			// headroom.
			srvDesc.NumDescriptors = 12 + ItemCapture::kMaxSlots + 4 + 1;
			srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			if (FAILED(g_device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&g_srvHeap)))) {
				REX::ERROR("SFShopExplorer: could not create ImGui SRV heap");
				return false;
			}

			D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
			rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
			rtvDesc.NumDescriptors = 1;
			if (FAILED(g_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&g_rtvHeap)))) {
				REX::ERROR("SFShopExplorer: could not create ImGui RTV heap");
				return false;
			}

			if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_allocator))) ||
				FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_allocator, nullptr, IID_PPV_ARGS(&g_commandList))) ||
				FAILED(g_commandList->Close()) ||
				FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) {
				REX::ERROR("SFShopExplorer: could not create ImGui command resources");
				return false;
			}
			g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (!g_fenceEvent) {
				return false;
			}

			IMGUI_CHECKVERSION();
			ImGui::CreateContext();
			ImGui::GetIO().IniFilename = nullptr;
			ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
			// Text quality: the bundled ProggyClean is a fixed 13px bitmap that
			// pixelates on modern resolutions and never follows [overlay] uiScale,
			// while the style metrics do (ScaleAllSizes in RenderImGuiFrame).
			// Load Segoe UI at 16px * uiScale with the symbol ranges the shop
			// draws (sort arrows, box drawing, star, skull) so those glyphs stop
			// falling back to mismatched bitmap rendering.
			{
				const ImWchar ranges[]{
					0x0020, 0x00FF,  // Basic Latin + Latin-1 Supplement
					0x0100, 0x017F,  // Latin Extended-A
					0x2190, 0x21FF,  // Arrows
					0x2500, 0x257F,  // Box Drawing
					0x25A0, 0x25FF,  // Geometric Shapes (sort triangles)
					0x2600, 0x26FF,  // Miscellaneous Symbols (star, skull)
					0,
				};
				const float fontPx = 16.0f * g_uiScale;
				g_bodyFonts[0] = LoadUiFont(ranges, fontPx * 0.875f);
				g_bodyFonts[1] = LoadUiFont(ranges, fontPx);
				g_bodyFonts[2] = LoadUiFont(ranges, fontPx * 1.125f);
				g_bodyFonts[3] = LoadUiFont(ranges, fontPx * 1.40f);
				const bool fontLoaded = g_bodyFonts[1] != nullptr;
				// Banner faces. The title is meant to read as shop signage and
				// fill the banner, not as a heading inside empty space - the
				// earlier sizes left most of the row empty and read as
				// ordinary UI text at a glance. The tagline is set large for
				// the same reason; both lines are read together, so a shrunken
				// tagline is just a second line nobody can read.
				//
				// If a face fails to load the caller falls back to the default
				// font rather than drawing nothing, so a missing system TTF
				// degrades to single-size text instead of an empty banner.
				g_fontTitle = LoadUiFont(ranges, fontPx * 2.60f);
				g_fontCaption = LoadUiFont(ranges, fontPx * 1.55f);
				if (fontLoaded) {
					REX::INFO("SFShopExplorer: UI font loaded at {:.1f}px (uiScale={:.2f}), title {:.1f}px, caption {:.1f}px",
						fontPx, g_uiScale, fontPx * 2.60f, fontPx * 1.55f);
				} else {
					REX::WARN("SFShopExplorer: no system TTF found; keeping default bitmap font");
				}
			}
			ImGui::StyleColorsDark();
			if (!ImGui_ImplWin32_Init(g_hwnd)) {
				REX::ERROR("SFShopExplorer: ImGui Win32 initialization failed");
				return false;
			}

			ImGui_ImplDX12_InitInfo info{};
			info.Device = g_device;
			info.CommandQueue = g_queue;
			info.NumFramesInFlight = 2;
			info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
			info.DSVFormat = DXGI_FORMAT_UNKNOWN;
			info.SrvDescriptorHeap = g_srvHeap;
			info.SrvDescriptorAllocFn = AllocateFontDescriptor;
			info.SrvDescriptorFreeFn = FreeFontDescriptor;
			if (!ImGui_ImplDX12_Init(&info)) {
				REX::ERROR("SFShopExplorer: ImGui D3D12 initialization failed");
				return false;
			}

			SetLastError(ERROR_SUCCESS);
			g_originalWndProc.store(reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WindowProc))), std::memory_order_release);
			if (!g_originalWndProc.load(std::memory_order_acquire) && GetLastError() != ERROR_SUCCESS) {
				REX::ERROR("SFShopExplorer: could not subclass Starfield window");
				return false;
			}
			RAWINPUTDEVICE rawInput{};
			rawInput.usUsagePage = 0x01;
			rawInput.usUsage = 0x02;
			rawInput.dwFlags = RIDEV_INPUTSINK;
			rawInput.hwndTarget = g_hwnd;
			if (!RegisterRawInputDevices(&rawInput, 1, sizeof(rawInput))) {
				REX::WARN("SFShopExplorer: raw mouse registration failed ({})", GetLastError());
			}
			g_imguiReady.store(true, std::memory_order_release);
			REX::INFO("SFShopExplorer: pure native ImGui renderer initialized");
			// Item capture: config first (mode 0 leaves this a no-op), then the
			// RTV observer on the game device. Nothing else changes until the
			// [itemcapture] mode is raised in the INI.
			ItemCapture::LoadConfig();
			ItemCapture::Install(g_device);
			OfflineModel::LoadConfig();
			OfflineModel::DeviceReady(g_device, g_srvHeap, g_srvIncrement);
			return true;
		}

		void DrawVirtualCursor()
		{
			if (!g_mouseDrawCursor.load(std::memory_order_acquire)) {
				return;
			}
			const ImVec2 cursor{
				g_virtualX.load(std::memory_order_relaxed),
				g_virtualY.load(std::memory_order_relaxed)
			};
			if (cursor.x < 0.0f || cursor.y < 0.0f) {
				return;
			}
			// Starfield may hide/recenter the hardware cursor after WM_SETCURSOR.
			// Draw a small arrow in the foreground layer at the raw-input position
			// so the pointer remains visible above every native shop child.
			auto* draw = ImGui::GetForegroundDrawList();
			const ImVec2 tip = cursor;
			const ImVec2 p1{ cursor.x + 4.0f, cursor.y + 21.0f };
			const ImVec2 p2{ cursor.x + 10.0f, cursor.y + 16.0f };
			draw->AddTriangleFilled(tip, p1, p2, IM_COL32(8, 8, 12, 245));
			draw->AddTriangle(tip, p1, p2, IM_COL32(255, 236, 220, 255), 1.2f);
			draw->AddLine(ImVec2(cursor.x + 5.0f, cursor.y + 18.0f), ImVec2(cursor.x + 14.0f, cursor.y + 27.0f), IM_COL32(8, 8, 12, 245), 3.0f);
			draw->AddLine(ImVec2(cursor.x + 5.0f, cursor.y + 18.0f), ImVec2(cursor.x + 14.0f, cursor.y + 27.0f), IM_COL32(255, 236, 220, 255), 1.2f);
		}

		void RenderImGuiFrame()
		{
			if (!g_imguiReady || !g_drawingEnabled.load(std::memory_order_acquire)) {
				return;
			}
			// The game recenters the OS cursor while in gameplay. PostFX uses a
			// virtual cursor driven by raw input; submit that position after the
			// Win32 backend's NewFrame fallback so it cannot overwrite it.
			ConsumeInputMessages();
			// ImGui drops every key and every character while it believes the app is
			// unfocused: UpdateKeyboardInputs() calls ClearInputKeys() on each frame
			// where AppFocusLost is set, and that also empties the character queue.
			// The visible symptom is exactly "arrow keys do nothing and typing in the
			// search box does nothing", while the mouse - which this overlay drives
			// from raw input and its own virtual cursor - keeps working. A focus loss
			// whose WM_SETFOCUS never arrived, or arrived on a path the subclass did
			// not see, therefore poisons input for the rest of the session. Trust the
			// actual foreground window instead of the last focus message.
			if (ImGui::GetIO().AppFocusLost && g_hwnd && GetForegroundWindow() == g_hwnd) {
				ImGui::GetIO().AddFocusEvent(true);
				if (!g_focusResyncLogged.exchange(true, std::memory_order_acq_rel)) {
					REX::INFO("SFShopExplorer: overlay input focus re-synced (ImGui believed the app was unfocused while the game window is foreground)");
				}
			}
			ImGui_ImplDX12_NewFrame();
			ImGui::GetIO().MouseDrawCursor = false;
			ImGui_ImplWin32_NewFrame();
			if (g_mouseDrawCursor.load(std::memory_order_acquire)) {
				SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
				// Build 94: keep the HW cursor hidden every frame -
				// DrawVirtualCursor below paints the visible arrow at
				// the raw-input position; showing the OS cursor here
				// produced two arrows (OS one parked at a stale spot).
				HideHwCursor();
			}
			if (g_appliedUiScale != g_uiScale) {
				ImGui::GetStyle().ScaleAllSizes(g_uiScale / g_appliedUiScale);
				g_appliedUiScale = g_uiScale;
			}
			if (g_resetImGuiInput.exchange(false, std::memory_order_acq_rel)) {
				ImGuiIO& io = ImGui::GetIO();
				io.ClearInputKeys();  // also drops the current frame's character queue
				g_lastHostCharTick.store(0, std::memory_order_relaxed);
				{
					std::scoped_lock lock(g_inputMutex);
					g_inputMessages.clear();
				}
				REX::INFO("SFShopExplorer: overlay input state reset on open (stale keys and queued messages dropped)");
			}
			ImGui::GetIO().AddMousePosEvent(g_virtualX.load(std::memory_order_relaxed), g_virtualY.load(std::memory_order_relaxed));
			ImGui::NewFrame();
			ShopBrowser::GetSingleton()->DrawImGui();
			DrawVirtualCursor();
			ImGui::Render();
			ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_commandList);
		}

		void WaitForGpu()
		{
			if (!g_fence || !g_fenceEvent || !g_queue || g_fenceValue == 0) {
				return;
			}
			if (g_fence->GetCompletedValue() < g_fenceValue) {
				g_fence->SetEventOnCompletion(g_fenceValue, g_fenceEvent);
				WaitForSingleObject(g_fenceEvent, 1000);
			}
		}

		void RenderOnPresent(IDXGISwapChain* a_swapChain)
		{
			std::scoped_lock lock(g_mutex);
			if (g_frameInProgress || g_renderMode.load(std::memory_order_acquire) < 3 || a_swapChain != g_swapChain || !InitImGui()) {
				return;
			}
			if (g_renderMode.load(std::memory_order_acquire) == 3) {
				if (!g_drawStageLogged.exchange(true, std::memory_order_acq_rel)) {
					REX::INFO("SFShopExplorer: ImGui/device/window initialization verified; draw path disabled by diagnostic mode");
				}
				return;
			}
			if (g_renderMode.load(std::memory_order_acquire) < 4) {
				return;
			}
			if (!g_drawStageLogged.exchange(true, std::memory_order_acq_rel)) {
				REX::INFO("SFShopExplorer: first native draw submission beginning (input={})", g_captureInput.load(std::memory_order_acquire));
			}
			g_frameInProgress = true;
			WaitForGpu();
			if (FAILED(g_allocator->Reset()) || FAILED(g_commandList->Reset(g_allocator, nullptr))) {
				g_frameInProgress = false;
				return;
			}
			CreateNativeTextureResources();
			UploadNativeTextures(g_commandList);
			// Back-buffer identity registration (13:06-run fix): the capture
			// filter must exclude the swap chain's buffers — their RTVs are
			// recreated every frame (they flooded the census) and with the
			// format filter open they would fill every display slot with the
			// whole-screen image instead of the item scene.
			{
				ID3D12Resource* buffers[4]{};
				std::uint32_t count = 0;
				for (UINT i = 0; i < 4 && count < 4; ++i) {
					ID3D12Resource* buffer = nullptr;
					if (FAILED(a_swapChain->GetBuffer(i, IID_PPV_ARGS(&buffer))) || !buffer) {
						if (buffer) {
							buffer->Release();
						}
						continue;
					}
					buffers[count++] = buffer;	// ownership moves to the array
				}
				ItemCapture::RegisterSwapChainResources(buffers, count);
				for (std::uint32_t i = 0; i < count; ++i) {
					buffers[i]->Release();
				}
			}
			// Capture SRV (re)binding runs before the ImGui frame so the
			// PREVIEW viewport samples current descriptors. The offline 3D
			// preview uploads through the same command list (slot 20).
			ItemCapture::Update(g_device, g_commandList, g_srvHeap, g_srvIncrement, 12, ItemCapture::kMaxSlots);
			OfflineModel::UploadGPU(g_commandList);

			UINT index = 0;
			IDXGISwapChain3* swapChain3 = nullptr;
			if (SUCCEEDED(a_swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3))) && swapChain3) {
				index = swapChain3->GetCurrentBackBufferIndex();
				swapChain3->Release();
			}
			ID3D12Resource* backBuffer = nullptr;
			if (FAILED(a_swapChain->GetBuffer(index, IID_PPV_ARGS(&backBuffer))) || !backBuffer) {
				g_commandList->Close();
				g_frameInProgress = false;
				return;
			}

			D3D12_RESOURCE_BARRIER toTarget{};
			toTarget.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			toTarget.Transition.pResource = backBuffer;
			toTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
			toTarget.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
			toTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			g_commandList->ResourceBarrier(1, &toTarget);
			const auto rtv = RtvHandle();
			g_device->CreateRenderTargetView(backBuffer, nullptr, rtv);
			g_commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
			ID3D12DescriptorHeap* heaps[] = { g_srvHeap };
			g_commandList->SetDescriptorHeaps(1, heaps);
			RenderImGuiFrame();

			D3D12_RESOURCE_BARRIER toPresent = toTarget;
			toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
			toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
			g_commandList->ResourceBarrier(1, &toPresent);
			if (SUCCEEDED(g_commandList->Close())) {
				ID3D12CommandList* lists[] = { g_commandList };
				g_queue->ExecuteCommandLists(1, lists);
				++g_fenceValue;
				g_queue->Signal(g_fence, g_fenceValue);
			}
			backBuffer->Release();
			g_frameInProgress = false;
		}

		void OnSwapChainCreated(IDXGISwapChain* a_swapChain, IUnknown* a_device)
		{
			if (!a_swapChain) {
				return;
			}
			DXGI_SWAP_CHAIN_DESC desc{};
			if (FAILED(a_swapChain->GetDesc(&desc)) || desc.BufferDesc.Width <= 2 || desc.BufferDesc.Height <= 2) {
				return;
			}
			std::scoped_lock lock(g_mutex);
			if (!g_swapChain) {
				g_swapChain = a_swapChain;
				g_hwnd = desc.OutputWindow;
				if (a_device) {
					a_device->QueryInterface(IID_PPV_ARGS(&g_queue));
				}
				if (g_queue) {
					g_queue->GetDevice(IID_PPV_ARGS(&g_device));
				}
			}
			auto** vtable = *reinterpret_cast<void***>(a_swapChain);
			const auto current = reinterpret_cast<PresentFn>(vtable[kPresentSlot]);
			if (current != &PresentThunk && !g_presentInstalled.exchange(true)) {
				g_originalPresent.store(current, std::memory_order_release);
				if (!REL::WriteSafeData(&vtable[kPresentSlot], reinterpret_cast<void*>(&PresentThunk))) {
					g_presentInstalled.store(false);
					g_originalPresent.store(nullptr, std::memory_order_release);
					REX::ERROR("SFShopExplorer: could not hook IDXGISwapChain::Present");
				}
			}
		}

		HRESULT STDMETHODCALLTYPE PresentThunk(IDXGISwapChain* a_swapChain, UINT a_syncInterval, UINT a_flags);

		HRESULT STDMETHODCALLTYPE CreateSwapChainThunk(IDXGIFactory* a_factory, IUnknown* a_device, DXGI_SWAP_CHAIN_DESC* a_desc, IDXGISwapChain** a_swapChain)
		{
			const auto original = g_originalCreateSwapChain.load(std::memory_order_acquire);
			const auto hr = original ? original(a_factory, a_device, a_desc, a_swapChain) : E_FAIL;
			if (SUCCEEDED(hr) && a_swapChain && *a_swapChain) {
				OnSwapChainCreated(*a_swapChain, a_device);
			}
			return hr;
		}

		HRESULT STDMETHODCALLTYPE CreateSwapChainForHwndThunk(IDXGIFactory2* a_factory, IUnknown* a_device, HWND a_hwnd, const DXGI_SWAP_CHAIN_DESC1* a_desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* a_fs, IDXGIOutput* a_output, IDXGISwapChain1** a_swapChain)
		{
			const auto original = g_originalCreateSwapChainForHwnd.load(std::memory_order_acquire);
			const auto hr = original ? original(a_factory, a_device, a_hwnd, a_desc, a_fs, a_output, a_swapChain) : E_FAIL;
			if (SUCCEEDED(hr) && a_swapChain && *a_swapChain) {
				OnSwapChainCreated(*a_swapChain, a_device);
			}
			return hr;
		}

		void InstallFactory(IDXGIFactory2* a_factory)
		{
			if (!a_factory || g_factoryInstalled.exchange(true)) {
				return;
			}
			auto** vtable = *reinterpret_cast<void***>(a_factory);
			const auto createSwapChain = reinterpret_cast<CreateSwapChainFn>(vtable[kCreateSwapChainSlot]);
			g_originalCreateSwapChain.store(createSwapChain, std::memory_order_release);
			if (!REL::WriteSafeData(&vtable[kCreateSwapChainSlot], reinterpret_cast<void*>(&CreateSwapChainThunk))) {
				g_factoryInstalled.store(false);
				g_originalCreateSwapChain.store(nullptr, std::memory_order_release);
				REX::WARN("SFShopExplorer: could not hook CreateSwapChain");
				return;
			}
			const auto createForHwnd = reinterpret_cast<CreateSwapChainForHwndFn>(vtable[kCreateSwapChainForHwndSlot]);
			g_originalCreateSwapChainForHwnd.store(createForHwnd, std::memory_order_release);
			if (!REL::WriteSafeData(&vtable[kCreateSwapChainForHwndSlot], reinterpret_cast<void*>(&CreateSwapChainForHwndThunk))) {
				REX::WARN("SFShopExplorer: could not hook CreateSwapChainForHwnd; base factory path remains active");
			}
		}
	}

		ImTextureID ShopLogoTexture()
		{
			return TextureId("ShopLogo.png");
		}

		ImTextureID ChestTexture()
		{
			return TextureId("Chest.png");
		}

		ImTextureID CreditIconTexture()
		{
			return TextureId("CreditIcon.png");
		}

		ImTextureID SpaceTexture()
		{
			return TextureId("space.png");
		}

		// Per-theme overrides: <Stem>_<suffix>.png, falling back to the shared
		// asset. Resolved by string rather than a lookup table so a new theme
		// needs no change here.
		ImTextureID ThemedTexture(const char* a_stem, const char* a_suffix, const char* a_fallback)
		{
			if (!a_suffix || !a_suffix[0]) {
				return TextureId(a_fallback);
			}
			std::string name = a_stem;
			name += "_";
			name += a_suffix;
			name += ".png";
			if (const auto id = TextureId(name.c_str())) {
				return id;
			}
			return TextureId(a_fallback);
		}

		[[nodiscard]] ImTextureID ThemeChestTexture(const char* a_suffix)
		{
			return ThemedTexture("Chest", a_suffix, "Chest.png");
		}

		[[nodiscard]] ImTextureID ThemeSpaceTexture(const char* a_suffix)
		{
			return ThemedTexture("space", a_suffix, "space.png");
		}

		float UiScale()
		{
			// [overlay] uiScale: shared by the font size (InitImGui) and the
			// fixed shop heights (header/toolbar/status bar) so text, padding,
			// and panels all grow together instead of only the paddings.
			return g_uiScale;
		}

float BackdropAlpha()
	{
		// [overlay] backdropAlpha: strength of the themed space_*.png backdrop.
		// Kept as a setting because the art varies hugely in brightness between
		// themes; see LoadSettings().
		return g_backdropAlpha;
	}

	ImFont* TitleFont()
	{
		return g_fontTitle;
	}	ImFont* CaptionFont()
	{
		return g_fontCaption;
	}

	ImFont* BodyFont()
	{
		return g_bodyFonts[std::clamp(g_bodyFontSize, 0, 3)];
	}

	void SetBodyFontSize(int a_size)
	{
		g_bodyFontSize = std::clamp(a_size, 0, 3);
	}


		bool Install()
	{
		std::scoped_lock lock(g_mutex);
		if (g_factoryInstalled.load(std::memory_order_acquire)) {
			return true;
		}
		LoadSettings();
			if (g_renderMode.load(std::memory_order_acquire) < 2) {
				REX::INFO("SFShopExplorer: renderer hook disabled by settings; browser-only diagnostic mode");
				return false;
			}
		REX::INFO("SFShopExplorer: install begin");
		if (const auto renderer = RE::CreationRendererPrivate::Renderer::GetSingleton()) {
			auto* factory = reinterpret_cast<IDXGIFactory2*>(renderer->GetDXGIFactory());
			g_queue = reinterpret_cast<ID3D12CommandQueue*>(renderer->GetGraphicsQueue());
			g_device = reinterpret_cast<ID3D12Device*>(renderer->GetDevice());
			REX::INFO("SFShopExplorer: renderer access succeeded (factory={}, queue={}, device={})", static_cast<void*>(factory), static_cast<void*>(g_queue), static_cast<void*>(g_device));
			InstallFactory(factory);
			if (factory) {
				REX::INFO("SFShopExplorer: native DX12 factory hooks installed");
				return true;
			}
		}
		REX::INFO("SFShopExplorer: install end (factoryInstalled={})", g_factoryInstalled.load(std::memory_order_acquire));
		return false;
	}

	void ProcessPendingActions()
	{
		ShopBrowser::GetSingleton()->ProcessPendingActions();
	}

	void SetDrawingEnabled(const bool a_enabled)
	{
		std::scoped_lock lock(g_mutex);
		SetInputEnabled(a_enabled);
	}

	bool ConsumeKeyPress(int a_vk)
	{
		if (a_vk < 0 || a_vk > 255) {
			return false;
		}
		const auto vk = static_cast<std::uint32_t>(a_vk);
		const std::uint64_t mask = 1ull << (vk & 63);
		return (g_rawKeyPresses[vk >> 6].fetch_and(~mask, std::memory_order_acq_rel) & mask) != 0;
	}

	int ToggleKey()
	{
		std::scoped_lock lock(g_mutex);
		LoadSettings();
		return g_toggleKey;
	}

	bool IsDrawingEnabled()
	{
		return g_drawingEnabled.load(std::memory_order_acquire);
	}

	ItemCapture::Snapshot ItemCaptureSnapshot()
	{
		return ItemCapture::GetSnapshot();
	}

	void DrawFrame()
	{
		std::scoped_lock lock(g_mutex);
		if (!g_imguiReady || !g_drawingEnabled.load(std::memory_order_acquire)) {
			return;
		}
		RenderImGuiFrame();
	}

	void Shutdown()
	{
		std::scoped_lock lock(g_mutex);
		g_drawingEnabled.store(false, std::memory_order_release);
		ItemCapture::Shutdown();
		OfflineModel::Shutdown();
		if (g_imguiReady) {
				WaitForGpu();
				ImGui_ImplDX12_Shutdown();
				ImGui_ImplWin32_Shutdown();
				ImGui::DestroyContext();
				g_imguiReady.store(false, std::memory_order_release);
			}
			const auto originalWndProc = g_originalWndProc.load(std::memory_order_acquire);
			if (originalWndProc && g_hwnd) {
				SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(originalWndProc));
				g_originalWndProc.store(nullptr, std::memory_order_release);
			}
		if (g_fenceEvent) {
			CloseHandle(g_fenceEvent);
			g_fenceEvent = nullptr;
		}
	}

	HRESULT STDMETHODCALLTYPE PresentThunk(IDXGISwapChain* a_swapChain, UINT a_syncInterval, UINT a_flags)
	{
		RenderOnPresent(a_swapChain);
		const auto original = g_originalPresent.load(std::memory_order_acquire);
		return original ? original(a_swapChain, a_syncInterval, a_flags) : E_FAIL;
	}
}
