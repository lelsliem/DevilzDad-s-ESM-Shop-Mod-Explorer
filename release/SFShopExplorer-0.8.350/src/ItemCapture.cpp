#include "PCH.h"
#include "ItemCapture.h"

#include <algorithm>
#include <format>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "RE/U/UI.h"

#include <Windows.h>
#include <d3d12.h>

#ifdef ERROR
#undef ERROR
#endif
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

// ItemCapture.h defines kMaxSlots as inline constexpr; the atomic members of
// the anonymous-namespace state below need the full <atomic> definition,
// which PCH.h already provides before this include.

namespace SFE::ItemCapture
{
	namespace
	{
		// Root-qualified SDK types: unqualified D3D12 interface names can be
		// shadowed by REX::W32 declarations pulled in through CommonLibSF
		// headers (the telemetry probe uses the same aliasing).
		using NativeDevice = ::ID3D12Device;
		using NativeResource = ::ID3D12Resource;
		using NativeGraphicsList = ::ID3D12GraphicsCommandList;
		using NativeDescriptorHeap = ::ID3D12DescriptorHeap;

		// ID3D12Device::CreateRenderTargetView — the classic-runtime slot used
		// by the telemetry probe and SFPostFX (DepthHunt patches DSV=21 right
		// next to it). Do not "correct" this against SDK header parses that
		// include UWP #if variants; 20 is the live, proven numbering.
		constexpr std::size_t kDeviceCreateRenderTargetViewSlot = 20;

		using CreateRenderTargetViewFn = void(STDMETHODCALLTYPE*)(
			NativeDevice*, NativeResource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);

		std::atomic<CreateRenderTargetViewFn> g_originalCreateRenderTargetView{ nullptr };

		// LIFETIME MODEL (the 02:26 CTD lesson): the engine frees transient
		// render targets between frames, and a D3D12 descriptor does NOT keep
		// a resource alive. Every candidate therefore takes a COM reference
		// inside the RTV hook — the one moment the pointer is provably alive —
		// and releases it on session clear/eviction. After AddRef, no engine
		// free can turn a candidate pointer stale, so SRV creation and GPU
		// sampling are always safe. VRAM cost is bounded by the ring below.
		struct Candidate
		{
			NativeResource* resource{ nullptr };
			D3D12_RESOURCE_DESC desc{};
			DXGI_FORMAT viewFormat{ DXGI_FORMAT_UNKNOWN };
		};

		// Per-session census budget: the first N RTVs after a menu-open edge
		// are logged verbatim (resource dims, resource format, view format) so
		// verify runs yield complete ground truth. FramePoll refills it; the
		// StoreCandidate band check absorbs fetch_sub underflow.
		std::atomic<std::uint32_t> g_censusBudget{ 0 };

		// Runtime-captured candidate item-scene render targets, each held by
		// COM reference. Written by the device hook (render side), read by
		// Update (Present side); both are on the render thread, but the mutex
		// keeps future callers safe. Recency ring: the newest matching targets
		// are the current session's item pool.
		constexpr std::size_t kMaxCandidates = 8;
		std::mutex g_dataMutex;
		std::vector<Candidate> g_candidates;

		void ReleaseCandidate(Candidate& a_candidate)
		{
			if (a_candidate.resource) {
				a_candidate.resource->Release();
				a_candidate.resource = nullptr;
			}
		}

		std::atomic<std::int32_t> g_mode{ 0 };
		std::atomic<bool> g_installed{ false };
		std::atomic<bool> g_shopWantsCapture{ false };
		std::atomic<bool> g_sessionLogged{ false };
		std::atomic<bool> g_lastMenuOpen{ false };
		std::atomic<std::uint64_t> g_captureFrames{ 0 };
		// Session diagnostics for the snapshot status line: RTVs rejected as
		// registered back buffers and repeat RTVs on already-tracked resources
		// (PostFX recreates back-buffer RTVs per frame; UI atlases take many
		// RTVs). Both were silent before the 13:06 census starvation.
		std::atomic<std::uint32_t> g_rejectedSwapChain{ 0 };
		std::atomic<std::uint32_t> g_duplicateRtvs{ 0 };
		std::chrono::steady_clock::time_point g_lastCensusRefill{};
		// Session summary logging waits out the post-open RT-pool burst
		// (~80 ms measured) before reporting candidates. Atomic: set on the
		// task thread (FramePoll), consumed on the render thread (Update).
		std::atomic<bool> g_sessionLogDelay{ false };
		std::chrono::steady_clock::time_point g_sessionLogDue{};
		std::string g_menuName{ "InventoryMenu" };
		// [itemcapture] formatFilter: when non-zero, only targets whose RTV
		// view format equals this DXGI_FORMAT numeric value become candidates
		// (26 = R11G11B10_FLOAT, the probe-identified item-scene format).
		// 0 = any.
		// 2026-09-10 13:06 run: the main HDR chain moved from fmt 26 to fmt 28
		// (R8G8B8A8 — the game's tonemapped output now flows through the same
		// fullscreen-size chain; the two fullscreen RTVs in that run's census
		// are literally SFPostFX's swap-chain back buffers). The fixed format
		// discriminator no longer holds, so the default is 0 (any) and identity
		// (back buffers, dedupe) replaces format as the discriminator.
		std::atomic<std::uint32_t> g_formatFilter{ 0 };
		// [itemcapture] minAspect/maxAspect: width/height band applied to the
		// RESOURCE dimensions. The probe's item passes render into
		// fullscreen-size HDR textures (2560x1440 = 1.78); the census showed
		// the only fmt-26 square in the pool is a UI atlas (256x256), so the
		// aspect band is the discriminator that removes it.
		double g_minAspect{ 1.45 };
		double g_maxAspect{ 2.15 };
		// [itemcapture] maxDimension: resources larger than this are excluded
		// (keeps degenerate atlases like 1024x5608 out).
		std::uint32_t g_maxDimension{ 8192 };

		// Registered swap-chain back buffers (identity set, filled every Present
		// by the overlay). Their RTVs are recreated every frame and with the
		// format filter open they would otherwise win every slot — and sampling
		// the presenting buffer shows the whole screen, never the item scene.
		constexpr std::size_t kMaxSwapChainResources = 8;
		std::mutex g_swapMutex;
		NativeResource* g_swapResources[kMaxSwapChainResources]{};
		std::uint32_t g_swapResourceCount{ 0 };

		bool IsRegisteredSwapChainResource(NativeResource* a_resource)
		{
			std::scoped_lock lock(g_swapMutex);
			for (std::uint32_t i = 0; i < g_swapResourceCount; ++i) {
				if (g_swapResources[i] == a_resource) {
					return true;
				}
			}
			return false;
		}

		// Display slots (mode 2). Slot i holds an SRV created directly on the
		// engine's candidate resource — no copies, no state writes. Slots are
		// fed newest-candidate-first, so slot 0 holds the most recently
		// created HDR target. Slots are invalidated whenever candidate
		// references are released (session clear), so a descriptor can never
		// outlive the texture it points at.
		struct DisplaySlot
		{
			NativeResource* source{ nullptr };
			D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
			D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
			std::uint32_t width{ 0 };
			std::uint32_t height{ 0 };
			std::uint32_t viewFormat{ 0 };
			bool valid{ false };
		};
		DisplaySlot g_slots[kMaxSlots]{};

		// [itemcapture] fullTexture (default 1): when true the PREVIEW grid
		// shows each bound target's full [0,1] extent with the INI sub-rect
		// outlined on the primary slot. When false it shows the sub-rect crop
		// itself. Full-extent drawing is the decisive diagnostic: the seven
		// fullscreen HDR candidates are interchangeable by every filter
		// field, so only eye-identification can find which one currently
		// carries the item image.
		std::atomic<bool> g_fullTexture{ true };
		// [itemcapture] slot: pin the primary PREVIEW slot by index (0-7),
		// -1 = follow the newest candidate (slot 0). Lets the grid run pick a
		// winner without a rebuild.
		std::atomic<std::int32_t> g_preferredSlot{ -1 };
		// Snapshot-resolved primary slot (INI pin when valid, else 0).
		std::atomic<std::uint32_t> g_primarySlot{ 0 };

		// Measured PreviewSceneRect geometry from the telemetry probe: the
		// item viewport carves this rect out of the shared fullscreen HDR
		// chain. UI scale can move it, so these are default UV bounds,
		// configurable from the INI (uvX/uvY/uvW/uvH). Defaults match the
		// 2560x1440 run: 400x266 @(173,193).
		double g_uvX{ 0.0676 };
		double g_uvY{ 0.1340 };
		double g_uvW{ 0.1563 };
		double g_uvH{ 0.1847 };

		Snapshot g_snapshot;
		std::mutex g_snapshotMutex;

		bool IsCandidate(const D3D12_RESOURCE_DESC& a_desc, const DXGI_FORMAT a_viewFormat)
		{
			if (g_formatFilter.load(std::memory_order_acquire) != 0 &&
				static_cast<unsigned>(a_viewFormat) != g_formatFilter.load(std::memory_order_acquire)) {
				return false;
			}
			if (a_desc.SampleDesc.Count != 1) {
				return false;
			}
			if (a_desc.Width > g_maxDimension || a_desc.Height > g_maxDimension) {
				return false;
			}
			// Aspect band: fullscreen HDR chain (1.78) passes; square UI
			// atlases (1.0) and degenerate strips fail.
			const auto aspect = static_cast<double>(a_desc.Width) / static_cast<double>(a_desc.Height);
			return aspect >= g_minAspect && aspect <= g_maxAspect;
		}

		void StoreCandidate(NativeResource* a_resource, const D3D12_RENDER_TARGET_VIEW_DESC* a_viewDesc)
		{
			if (!a_resource) {
				return;
			}
			// ID3D12Resource::GetDesc returns D3D12_RESOURCE_DESC by value.
			const auto desc = a_resource->GetDesc();
			// The RTV view format is the probe-measured discriminator; fall
			// back to the resource format when the view desc is omitted.
			const auto viewFormat = a_viewDesc && a_viewDesc->Format != DXGI_FORMAT_UNKNOWN
				? a_viewDesc->Format
				: desc.Format;
			if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
				return;
			}
			// Census before filtering: bounded per session, one line per RTV,
			// so a failed capture still yields complete ground truth in the
			// log. The band check also absorbs the fetch_sub underflow when
			// the budget is already exhausted.
			const auto budget = g_censusBudget.fetch_sub(1, std::memory_order_acq_rel);
			if (budget >= 1 && budget <= 48) {
				REX::INFO("SFShopExplorer: itemcapture rtv res=0x{:X} {}x{} resFmt={} viewFmt={} samples={}",
					reinterpret_cast<std::uintptr_t>(a_resource),
					static_cast<unsigned>(desc.Width), desc.Height,
					static_cast<unsigned>(desc.Format), static_cast<unsigned>(viewFormat),
					desc.SampleDesc.Count);
			}
			// Back-buffer identity exclusion BEFORE dedupe accounting: PostFX
			// recreates the swap-chain RTVs every frame, so without this the
			// back buffers would both flood the dedupe counter and (with the
			// format filter open) fill every display slot with the whole-screen
			// image. The census line above still records them for ground truth.
			if (IsRegisteredSwapChainResource(a_resource)) {
				g_rejectedSwapChain.fetch_add(1, std::memory_order_acq_rel);
				return;
			}
			if (!IsCandidate(desc, viewFormat)) {
				return;
			}
			// Take the COM reference BEFORE any other thread can observe the
			// candidate: the engine is inside CreateRenderTargetView on this
			// resource right now, so the pointer is guaranteed alive here.
			// Skip if the resource is already tracked (8-RTV atlas pattern).
			bool duplicate = false;
			{
				std::scoped_lock lock(g_dataMutex);
				for (auto& existing : g_candidates) {
					if (existing.resource == a_resource) {
						duplicate = true;
						break;
					}
				}
			}
			if (duplicate) {
				g_duplicateRtvs.fetch_add(1, std::memory_order_acq_rel);
				return;
			}
			a_resource->AddRef();
			g_candidates.push_back(Candidate{ a_resource, desc, viewFormat });
			if (g_candidates.size() > kMaxCandidates) {
				// Recency ring: drop the oldest matching target (releases it).
				auto oldest = g_candidates.begin();
				ReleaseCandidate(*oldest);
				g_candidates.erase(oldest);
			}
		}

		// Releases every candidate reference and invalidates all display
		// slots so no descriptor can outlive its texture. Called when the
		// tracked engine session ends (next menu-open edge) and at shutdown.
		void ClearCandidatesAndSlots()
		{
			std::scoped_lock lock(g_dataMutex);
			for (auto& candidate : g_candidates) {
				ReleaseCandidate(candidate);
			}
			g_candidates.clear();
			for (auto& slot : g_slots) {
				slot = DisplaySlot{};
			}
		}

		std::string CandidateSummaryLocked()
		{
			// Oldest-to-newest; the last entries are the current session's pool.
			std::string text;
			std::size_t shown = 0;
			for (const auto& candidate : g_candidates) {
				if (shown >= 10) {
					break;
				}
				text += std::format("{}0x{:X}[{}x{} f{}v{}]", shown ? " " : "",
					reinterpret_cast<std::uintptr_t>(candidate.resource),
					static_cast<unsigned>(candidate.desc.Width), candidate.desc.Height,
					static_cast<unsigned>(candidate.desc.Format),
					static_cast<unsigned>(candidate.viewFormat));
				++shown;
			}
			if (g_candidates.size() > shown) {
				text += std::format(" ...(+{})", g_candidates.size() - shown);
			}
			return text;
		}

		void LogSessionSnapshot(const std::string& a_reason)
		{
			if (g_sessionLogged.exchange(true, std::memory_order_acq_rel)) {
				return;
			}
			std::scoped_lock lock(g_dataMutex);
			REX::INFO("SFShopExplorer: itemcapture {} candidates={}{}", a_reason,
				g_candidates.size(), CandidateSummaryLocked());
		}

		void UpdateSnapshot(Mode a_mode)
		{
			std::uint32_t bound = 0;
			std::uint64_t handles[kMaxSlots]{};
			std::uint32_t widths[kMaxSlots]{};
			std::uint32_t heights[kMaxSlots]{};
			float uv0[kMaxSlots][2]{};
			float uv1[kMaxSlots][2]{};
			std::uint64_t resources[kMaxSlots]{};
			std::uint32_t formats[kMaxSlots]{};
			{
				std::scoped_lock lock(g_dataMutex);
				for (std::uint32_t i = 0; i < kMaxSlots; ++i) {
					if (g_slots[i].valid && g_slots[i].gpu.ptr != 0) {
						handles[bound] = g_slots[i].gpu.ptr;
						widths[bound] = g_slots[i].width;
						heights[bound] = g_slots[i].height;
						resources[bound] = reinterpret_cast<std::uint64_t>(g_slots[i].source);
						formats[bound] = g_slots[i].viewFormat;
						uv0[bound][0] = static_cast<float>(g_uvX);
						uv0[bound][1] = static_cast<float>(g_uvY);
						uv1[bound][0] = static_cast<float>(g_uvX + g_uvW);
						uv1[bound][1] = static_cast<float>(g_uvY + g_uvH);
						++bound;
					}
				}
			}
			Snapshot snap{};
			snap.mode = a_mode;
			snap.engineMenuOpen = g_lastMenuOpen.load(std::memory_order_acquire);
			snap.hookInstalled = g_installed.load(std::memory_order_acquire);
			snap.copiedCount = bound;
			snap.lastCaptureFrame = g_captureFrames.load(std::memory_order_acquire);
			for (std::uint32_t i = 0; i < kMaxSlots; ++i) {
				snap.srvHandles[i] = handles[i];
				snap.srvWidths[i] = widths[i];
				snap.srvHeights[i] = heights[i];
				snap.srvUV0[i][0] = uv0[i][0];
				snap.srvUV0[i][1] = uv0[i][1];
				snap.srvUV1[i][0] = uv1[i][0];
				snap.srvUV1[i][1] = uv1[i][1];
				snap.srvResource[i] = resources[i];
				snap.srvFormat[i] = formats[i];
			}
			// Primary slot: the INI pin when that slot is live this frame,
			// otherwise the newest bound candidate (slot 0).
			const auto preferred = g_preferredSlot.load(std::memory_order_acquire);
			const auto primary = preferred >= 0 && static_cast<std::uint32_t>(preferred) < bound
				? static_cast<std::uint32_t>(preferred)
				: 0u;
			g_primarySlot.store(primary, std::memory_order_release);
			snap.primarySlot = primary;
			snap.fullTexture = g_fullTexture.load(std::memory_order_acquire);
			snap.rejectedSwapChain = g_rejectedSwapChain.load(std::memory_order_acquire);
			snap.duplicateRtvs = g_duplicateRtvs.load(std::memory_order_acquire);
			{
				std::scoped_lock lock(g_dataMutex);
				snap.candidateCount = static_cast<std::uint32_t>(g_candidates.size());
				snap.candidateSummary = CandidateSummaryLocked();
			}
			std::scoped_lock lock(g_snapshotMutex);
			g_snapshot = std::move(snap);
		}

		std::filesystem::path IniPath()
		{
			HMODULE self = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&Install), &self);
			wchar_t modulePath[MAX_PATH]{};
			GetModuleFileNameW(self, modulePath, MAX_PATH);
			return std::filesystem::path(modulePath).parent_path() / L"SFShopExplorer.ini";
		}

		double ParseDouble(const wchar_t* a_value, double a_fallback)
		{
			try {
				std::size_t idx = 0;
				// stod has a wstring overload; narrowing first truncated any
				// non-ASCII digit separator and tripped C4244.
				const std::wstring wide{ a_value };
				const auto value = std::stod(wide, &idx);
				if (idx == 0) {
					return a_fallback;
				}
				return value;
			} catch (...) {
				return a_fallback;
			}
		}
	}

	void STDMETHODCALLTYPE CreateRenderTargetViewHook(NativeDevice* a_device, NativeResource* a_resource,
		const D3D12_RENDER_TARGET_VIEW_DESC* a_viewDesc, D3D12_CPU_DESCRIPTOR_HANDLE a_descriptor)
	{
		const auto original = g_originalCreateRenderTargetView.load(std::memory_order_acquire);
		if (original) {
			original(a_device, a_resource, a_viewDesc, a_descriptor);
		}
		if (a_resource && g_mode.load(std::memory_order_acquire) > 0) {
			StoreCandidate(a_resource, a_viewDesc);
		}
	}

	bool Install(ID3D12Device* a_device)
	{
		if (g_installed.load(std::memory_order_acquire)) {
			return true;
		}
		const auto mode = static_cast<Mode>(g_mode.load(std::memory_order_acquire));
		if (mode == Mode::kDisabled || !a_device) {
			return false;
		}
		// In-place shared-vtable slot patch: chain over whatever occupies the
		// slot (engine method, PostFX thunk, ...) and never repoint object
		// vptrs. This is the only pattern the stack has proven stable.
		auto* device = static_cast<NativeDevice*>(a_device);
		auto** vtable = *reinterpret_cast<void***>(device);
		const auto current = reinterpret_cast<CreateRenderTargetViewFn>(vtable[kDeviceCreateRenderTargetViewSlot]);
		g_originalCreateRenderTargetView.store(current, std::memory_order_release);
		if (!REL::WriteSafeData(&vtable[kDeviceCreateRenderTargetViewSlot], reinterpret_cast<void*>(&CreateRenderTargetViewHook))) {
			g_originalCreateRenderTargetView.store(nullptr, std::memory_order_release);
			REX::ERROR("SFShopExplorer: itemcapture could not hook device slot {}", kDeviceCreateRenderTargetViewSlot);
			return false;
		}
		g_installed.store(true, std::memory_order_release);
		REX::INFO("SFShopExplorer: itemcapture RTV observer installed (slot {}, mode {}, menu '{}')",
			kDeviceCreateRenderTargetViewSlot, static_cast<int>(mode), g_menuName);
		return true;
	}

	void LoadConfig()
	{
		const auto ini = IniPath();
		const auto modeInt = static_cast<std::int32_t>(GetPrivateProfileIntW(L"itemcapture", L"mode", 0, ini.c_str()));
		g_mode.store(std::clamp(modeInt, 0, 2), std::memory_order_release);
		// Default 0 (any): the fixed fmt-26 discriminator died with the
		// display-config change that moved the chain to fmt 28 (see the
		// g_formatFilter comment). Identity exclusion replaces it.
		g_formatFilter.store(static_cast<std::uint32_t>(GetPrivateProfileIntW(L"itemcapture", L"formatFilter", 0, ini.c_str())), std::memory_order_release);
		g_maxDimension = static_cast<std::uint32_t>(GetPrivateProfileIntW(L"itemcapture", L"maxDimension", 8192, ini.c_str()));

		wchar_t menu[64]{};
		GetPrivateProfileStringW(L"itemcapture", L"menuName", L"InventoryMenu", menu, static_cast<DWORD>(std::size(menu)), ini.c_str());
		g_menuName = NarrowUtf8(menu);

		wchar_t buf[32]{};
		auto readDouble = [&](const wchar_t* a_key, double a_fallback) {
			GetPrivateProfileStringW(L"itemcapture", a_key, L"", buf, static_cast<DWORD>(std::size(buf)), ini.c_str());
			return buf[0] == L'\0' ? a_fallback : ParseDouble(buf, a_fallback);
		};
		g_minAspect = readDouble(L"minAspect", 1.45);
		g_maxAspect = readDouble(L"maxAspect", 2.15);
		g_uvX = readDouble(L"uvX", 0.0676);
		g_uvY = readDouble(L"uvY", 0.1340);
		g_uvW = readDouble(L"uvW", 0.1563);
		g_uvH = readDouble(L"uvH", 0.1847);
		g_fullTexture.store(readDouble(L"fullTexture", 1.0) >= 0.5, std::memory_order_release);
		g_preferredSlot.store(static_cast<std::int32_t>(GetPrivateProfileIntW(L"itemcapture", L"slot", static_cast<UINT>(-1), ini.c_str())), std::memory_order_release);

		REX::INFO("SFShopExplorer: itemcapture config mode={} menu={} formatFilter={} aspect={:.2f}-{:.2f} fullTex={} slot={}",
			g_mode.load(std::memory_order_acquire), g_menuName, g_formatFilter.load(std::memory_order_acquire), g_minAspect, g_maxAspect,
			g_fullTexture.load(std::memory_order_acquire) ? 1 : 0, g_preferredSlot.load(std::memory_order_acquire));
	}

	void FramePoll()
	{
		if (g_mode.load(std::memory_order_acquire) <= 0) {
			return;
		}
		bool open = false;
		if (auto* ui = RE::UI::GetSingleton()) {
			open = ui->IsMenuOpen(RE::BSFixedString{ g_menuName.c_str() });
		}
		const bool previous = g_lastMenuOpen.exchange(open, std::memory_order_acq_rel);
		if (open && !previous) {
			g_sessionLogged.store(false, std::memory_order_release);
			// Fresh engine session: the probe proved the item RT pool is
			// recreated per session. DO NOT clear here — the 13:06 run proved
			// the pool burst can land BEFORE this edge is polled (census
			// .498, open edge .530), so a clear-on-open wipes the just-created
			// pool (that run ended with candidates=0). The recency ring evicts
			// the previous session's entries as the new pool lands.
			g_rejectedSwapChain.store(0, std::memory_order_release);
			g_duplicateRtvs.store(0, std::memory_order_release);
			g_censusBudget.store(48, std::memory_order_release);
			g_lastCensusRefill = std::chrono::steady_clock::now();
			g_sessionLogDue = std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
			// The 02:04 census measured the session pool burst ~80 ms after
			// the open edge; log the session summary only once that window has
			// elapsed so the reported candidate set is the real pool, not the
			// pre-burst stragglers (the 08:17 run logged "candidates=0"
			// before the burst landed).
			g_sessionLogDelay = true;
		}
		if (open) {
			// Trickle: item RTVs can be created before the open edge (the
			// probe saw session RTVs predate the telemetry window), so keep a
			// small census allowance flowing for the whole menu session.
			const auto now = std::chrono::steady_clock::now();
			if (now - g_lastCensusRefill > std::chrono::seconds(2) && g_censusBudget.load(std::memory_order_acquire) == 0) {
				g_censusBudget.store(8, std::memory_order_release);
				g_lastCensusRefill = now;
			}
		}
		// NO clear-on-close: the 23:00 run proved the shop must keep showing
		// the last-seen scene after the inventory closes (its PREVIEW reverted
		// to "waiting for InventoryMenu" and blank when the clear fired). The
		// candidates hold COM references, so the textures stay valid and
		// sample-safe; the recency ring evicts them naturally as the next
		// session's pool arrives. Only Shutdown clears now.
	}

	void RegisterSwapChainResources(ID3D12Resource* const* a_resources, std::uint32_t a_count)
	{
		if (!a_resources || a_count == 0) {
			return;
		}
		// Identity registration only — no references are taken: back buffers
		// are owned by the swap chain and outlive every RTV hook call that
		// mentions them within a frame. Comparing addresses against the RTV
		// hook's resource pointer is the whole contract.
		std::scoped_lock lock(g_swapMutex);
		g_swapResourceCount = 0;
		for (std::uint32_t i = 0; i < a_count && g_swapResourceCount < kMaxSwapChainResources; ++i) {
			if (a_resources[i]) {
				g_swapResources[g_swapResourceCount++] =
					static_cast<NativeResource*>(a_resources[i]);
			}
		}
	}

	void Update(ID3D12Device* a_device, ID3D12GraphicsCommandList* a_list, ID3D12DescriptorHeap* a_srvHeap,
		std::uint32_t a_srvIncrement, std::uint32_t a_firstSrvSlot, std::uint32_t a_maxSrvSlots)
	{
		const auto mode = static_cast<Mode>(g_mode.load(std::memory_order_acquire));
		if (mode == Mode::kDisabled || !g_installed.load(std::memory_order_acquire)) {
			return;
		}

		const bool menuOpen = g_lastMenuOpen.load(std::memory_order_acquire);
		if (menuOpen && g_sessionLogDelay.load(std::memory_order_acquire) &&
			std::chrono::steady_clock::now() >= g_sessionLogDue) {
			// One verify line per engine menu session, fired AFTER the
			// render thread's post-open pool burst has landed.
			g_sessionLogDelay.store(false, std::memory_order_release);
			LogSessionSnapshot("session");
		}

		// GPU binding happens ONLY while the engine menu is open — or when the
		// shop wants an offscreen Barter preview for the hovered FormID.
		// Outside it the candidate set is either empty or holds the frozen
		// previous session (kept alive by our references, so the shop can
		// display the last-seen item scene), and binding then would sample
		// textures the engine is actively rewriting for other passes.
		const bool shopWants = g_shopWantsCapture.load(std::memory_order_acquire);
		if (mode != Mode::kDisplay || (!menuOpen && !shopWants) || !a_device || !a_srvHeap || a_srvIncrement == 0) {
			UpdateSnapshot(mode);
			return;
		}
		(void)a_list;	// direct-SRV design: no command-list work, no copies

		auto* device = static_cast<NativeDevice*>(a_device);
		auto* srvHeap = static_cast<NativeDescriptorHeap*>(a_srvHeap);
		const auto heapCpuStart = srvHeap->GetCPUDescriptorHandleForHeapStart();
		const auto heapGpuStart = srvHeap->GetGPUDescriptorHandleForHeapStart();

		// Copy the candidate list, then release the lock before doing device
		// work: the RTV hook can append while we bind. Every candidate holds
		// a COM reference, so its pointer is valid for the whole loop.
		std::vector<Candidate> targets;
		{
			std::scoped_lock lock(g_dataMutex);
			targets.reserve(g_candidates.size());
			for (const auto& candidate : g_candidates) {
				targets.push_back(candidate);
				if (targets.back().resource) {
					targets.back().resource->AddRef();	// pin for the loop
				}
			}
		}
		auto unpin = [&]() {
			for (auto& target : targets) {
				if (target.resource) {
					target.resource->Release();
					target.resource = nullptr;
				}
			}
		};

		// Newest candidate first: with the recency ring the newest entries
		// are the current session's item pool, and slot 0 feeds PREVIEW.
		// Slot writes hold the data mutex: FramePoll (task thread) can clear
		// slots concurrently, and unsynchronized POD writes would tear.
		std::uint32_t slotIndex = 0;
		{
			std::scoped_lock lock(g_dataMutex);
			for (auto it = targets.rbegin(); it != targets.rend() && slotIndex < kMaxSlots && slotIndex < a_maxSrvSlots; ++it) {
				const auto& target = *it;
				auto& slot = g_slots[slotIndex];
				if (!slot.valid || slot.source != target.resource) {
					auto* resource = target.resource;
					const auto desc = target.desc;
					const auto viewFormat = target.viewFormat;
					// Explicit SRV desc: robust against typeless resource
					// formats (a null desc would be rejected there) and pins
					// the exact view the probe measured.
					D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
					srv.Format = viewFormat;
					srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
					srv.Texture2D.MostDetailedMip = 0;
					srv.Texture2D.MipLevels = desc.MipLevels == 0 ? D3D12_REQ_MIP_LEVELS : desc.MipLevels;
					srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
					const auto offset = static_cast<std::uint64_t>(a_firstSrvSlot + slotIndex) * a_srvIncrement;
					const D3D12_CPU_DESCRIPTOR_HANDLE cpu{ heapCpuStart.ptr + static_cast<SIZE_T>(offset) };
					device->CreateShaderResourceView(resource, &srv, cpu);
					slot.source = resource;
					slot.cpu = cpu;
					slot.gpu.ptr = heapGpuStart.ptr + offset;
					slot.width = static_cast<std::uint32_t>(desc.Width);
					slot.height = desc.Height;
					slot.viewFormat = static_cast<std::uint32_t>(viewFormat);
					slot.valid = true;
					REX::INFO("SFShopExplorer: itemcapture slot {} bound res=0x{:X} {}x{} viewFmt={}",
						slotIndex, reinterpret_cast<std::uintptr_t>(resource),
						slot.width, slot.height, slot.viewFormat);
				}
				++slotIndex;
			}
			// Invalidate any stale slots beyond the current candidate count.
			for (auto i = slotIndex; i < kMaxSlots; ++i) {
				if (g_slots[i].valid) {
					g_slots[i] = DisplaySlot{};
				}
			}
		}
		unpin();
		g_captureFrames.fetch_add(1, std::memory_order_acq_rel);
		UpdateSnapshot(mode);
	}

	Snapshot GetSnapshot()
	{
		std::scoped_lock lock(g_snapshotMutex);
		return g_snapshot;
	}

	void SetShopWantsCapture(bool a_want)
	{
		g_shopWantsCapture.store(a_want, std::memory_order_release);
	}

	void Shutdown()
	{
		g_mode.store(0, std::memory_order_release);
		ClearCandidatesAndSlots();
	}
}
