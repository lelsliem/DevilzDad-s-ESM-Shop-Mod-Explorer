#include "PCH.h"
#include "ImguiOverlay.h"
#include "ShopExplorer.h"
#include "ItemCapture.h"
#include "OfflineModel.h"

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
		std::atomic<float> g_virtualX{ 0.0f };
		std::atomic<float> g_virtualY{ 0.0f };
		std::atomic<int> g_renderMode{ 0 };
		std::atomic<bool> g_drawStageLogged{ false };
		std::atomic<bool> g_captureInput{ false };
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
			*a_cpu = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
			*a_gpu = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
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

		bool LoadTexturePixels(NativeTexture& a_texture)
		{
			IWICImagingFactory* factory = nullptr;
			if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) || !factory) {
				return false;
			}
			IWICBitmapDecoder* decoder = nullptr;
			IWICBitmapFrameDecode* frame = nullptr;
			IWICFormatConverter* converter = nullptr;
			bool success = false;
			if (SUCCEEDED(factory->CreateDecoderFromFilename(a_texture.path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)) && decoder &&
				SUCCEEDED(decoder->GetFrame(0, &frame)) && frame &&
				SUCCEEDED(factory->CreateFormatConverter(&converter)) && converter &&
				SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
				UINT width = 0;
				UINT height = 0;
				if (SUCCEEDED(converter->GetSize(&width, &height)) && width > 0 && height > 0) {
					a_texture.width = width;
					a_texture.height = height;
					a_texture.pixels.resize(static_cast<std::size_t>(width) * height * 4);
					success = SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(a_texture.pixels.size()), a_texture.pixels.data()));
				}
			}
			if (converter) converter->Release();
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
			for (const auto name : { L"ShopLogo.png", L"Chest.png", L"CreditIcon.png", L"space.png" }) {
				NativeTexture texture{};
				texture.path = root / name;
				if (!LoadTexturePixels(texture)) {
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
			REX::INFO("SFShopExplorer: settings loaded from {} (toggleKey={}, uiScale={:.2f}, renderMode={}, input={})", path.string(), g_toggleKey, g_uiScale, g_renderMode.load(std::memory_order_acquire), g_captureInput.load(std::memory_order_acquire));
		}

		void SetInputEnabled(bool a_enabled)
		{
			const bool drawing = a_enabled && g_renderMode.load(std::memory_order_acquire) >= 4;
			const bool inputEnabled = drawing && g_captureInput.load(std::memory_order_acquire);
			const bool wasInputEnabled = g_mouseDrawCursor.exchange(inputEnabled, std::memory_order_acq_rel);
			g_drawingEnabled.store(drawing, std::memory_order_release);
			if (inputEnabled && !wasInputEnabled) {
				LoadSettings();
				POINT point{};
				if (g_hwnd && GetCursorPos(&point) && ScreenToClient(g_hwnd, &point)) {
					g_virtualX.store(static_cast<float>(point.x), std::memory_order_relaxed);
					g_virtualY.store(static_cast<float>(point.y), std::memory_order_relaxed);
				}
				ClipCursor(nullptr);
				while (ShowCursor(TRUE) < 0) {}
			} else if (!inputEnabled && wasInputEnabled) {
				while (ShowCursor(FALSE) >= 0) {}
			}
		}

		void QueueInputMessage(UINT a_message, WPARAM a_wparam, LPARAM a_lparam)
		{
			std::scoped_lock lock(g_inputMutex);
			if (g_inputMessages.size() >= 256) {
				g_inputMessages.pop_front();
			}
			g_inputMessages.push_back({ a_message, a_wparam, a_lparam });
		}

		void ConsumeInputMessages()
		{
			std::deque<InputMessage> messages;
			{
				std::scoped_lock lock(g_inputMutex);
				messages.swap(g_inputMessages);
			}
			for (const auto& input : messages) {
				if (input.message != WM_MOUSEMOVE && input.message != WM_MOUSELEAVE && input.message != WM_NCMOUSELEAVE) {
					ImGui_ImplWin32_WndProcHandler(g_hwnd, input.message, input.wparam, input.lparam);
				}
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
			if (g_drawingEnabled.load(std::memory_order_acquire) && g_captureInput.load(std::memory_order_acquire) && IsOverlayInputMessage(a_message)) {
				QueueInputMessage(a_message, a_wparam, a_lparam);
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
			ImGui_ImplDX12_NewFrame();
			ImGui::GetIO().MouseDrawCursor = false;
			ImGui_ImplWin32_NewFrame();
			if (g_mouseDrawCursor.load(std::memory_order_acquire)) {
				SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
				while (ShowCursor(TRUE) < 0) {}
			}
			if (g_appliedUiScale != g_uiScale) {
				ImGui::GetStyle().ScaleAllSizes(g_uiScale / g_appliedUiScale);
				g_appliedUiScale = g_uiScale;
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
