#pragma once

#include <atomic>
#include <cstdint>
#include <string>

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12GraphicsCommandList;
struct ID3D12DescriptorHeap;
struct ID3D12Resource;

namespace SFE::ItemCapture
{
	// Modes:
	//  0 = disabled (no hooks, no GPU work)
	//  1 = verify: device RTV hook installed game-wide, candidates logged
	//      (census + bounded session lines), no GPU display work.
	//  2 = display: while the engine InventoryMenu is open, bind SRVs onto
	//      the engine's own item-scene HDR render targets (probed identity:
	//      fullscreen-size R11G11B10_FLOAT chain, aspect band 1.45-2.15) and
	//      expose them to the PREVIEW viewport. No copies, no engine state
	//      writes: the engine itself keeps those targets in a shader-resource
	//      readable state (telemetry barrier data: 0xC0), and read-state
	//      sampling from COMMON is implicitly promotable in D3D12.

	enum class Mode : std::int32_t { kDisabled = 0, kVerify = 1, kDisplay = 2 };

	// Feed the swap chain's back buffers (from GetBuffer) every Present so the
	// candidate filter can exclude them by identity: the back buffers are the
	// most damaging false positive once the format filter is open — sampling
	// the presenting buffer shows the whole screen, not the item scene.
	void RegisterSwapChainResources(ID3D12Resource* const* a_resources, std::uint32_t a_count);

	// Settings are read from [itemcapture] in SFShopExplorer.ini on install.
	void LoadConfig();

	// Installs the CreateRenderTargetView observer hook on the game device
	// (device vtable slot 20, in-place patch — the pattern proven stable by
	// SFPostFX and the telemetry probe). Returns false when disabled.
	bool Install(ID3D12Device* a_device);

	// Called from the SFSE task thread each tick: samples whether the engine
	// InventoryMenu is open. Game-object access happens only here, never on
	// the render thread.
	void FramePoll();

	// Called from the Present path BEFORE the ImGui frame renders.
	// a_srvSlot is a reserved CBV_SRV_UAV heap slot range for capture SRVs.
	void Update(ID3D12Device* a_device, ID3D12GraphicsCommandList* a_list,
		ID3D12DescriptorHeap* a_srvHeap, std::uint32_t a_srvIncrement,
		std::uint32_t a_firstSrvSlot, std::uint32_t a_maxSrvSlots);

	// Bound display slots. The overlay SRV heap must cover firstSlot +
	// kMaxSlots (heap sized 24, slots 12..19 reserved for capture).
	inline constexpr std::uint32_t kMaxSlots = 8;

	struct Snapshot
	{
		Mode mode{ Mode::kDisabled };
		bool engineMenuOpen{ false };
		bool hookInstalled{ false };
		std::uint32_t candidateCount{ 0 };
		std::uint32_t copiedCount{ 0 };        // textures bound/displayable this frame
		std::uint32_t lastResult{ 0 };         // reserved (no copies in current design)
		std::uint64_t lastCaptureFrame{ 0 };   // frames with textures bound
		// GPU descriptor handles (as ImTextureID) for the bound candidates,
		// zero when not available this frame. Slot 0 = newest candidate.
		std::uint64_t srvHandles[kMaxSlots]{};
		std::uint32_t srvWidths[kMaxSlots]{};
		std::uint32_t srvHeights[kMaxSlots]{};
		// Sub-rect UVs (normalized) of the measured item region within each
		// texture — the item viewport carves a small rect out of the shared
		// fullscreen HDR chain, so PREVIEW draws this range, not [0,1].
		float srvUV0[kMaxSlots][2]{};          // top-left UV per slot
		float srvUV1[kMaxSlots][2]{};          // bottom-right UV per slot
		// Per-slot candidate identity (resource address + view format) so the
		// UI can label a diagnostic grid and a target can be pinned by INI.
		std::uint64_t srvResource[kMaxSlots]{};
		std::uint32_t srvFormat[kMaxSlots]{};
		// Which slot PREVIEW shows by default: the INI-pinned slot when that
		// slot is valid this frame, otherwise slot 0 (newest candidate).
		std::uint32_t primarySlot{ 0 };
		// INI fullTexture: PREVIEW shows each bound target's full extent
		// (sub-rect outlined on the primary slot) instead of the crop.
		bool fullTexture{ false };
		// Session diagnostics: RTVs rejected as registered back buffers and
		// repeat RTVs on already-tracked resources (PostFX recreates
		// back-buffer RTVs every frame; UI atlases take many RTVs).
		std::uint32_t rejectedSwapChain{ 0 };
		std::uint32_t duplicateRtvs{ 0 };
		std::string candidateSummary;
	};

	// Read-only snapshot for the shop UI (render thread).
	Snapshot GetSnapshot();

	// Kill switch used on shutdown.
	void Shutdown();

	// Shop wants a native preview even when its menu is not open — offscreen
	// Barter render for the hovered FormID. When set, Update binds even
	// without engineMenuOpen (ItemCapture already holds the RTV).
	void SetShopWantsCapture(bool a_want);
}
