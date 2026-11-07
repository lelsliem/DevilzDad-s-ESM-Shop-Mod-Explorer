#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12DescriptorHeap;

namespace SFE::OfflineModel
{
	// Preview service. Shop Explorer uses RequestPreviewDirect below: geometry
	// extracted by NativeModel is sent to the independent GPU renderer. That
	// route bypasses the legacy cache/CPU preview API and never falls back to it.

	// The overlay's SRV heap is sized 12 (static) + 8 (capture) + 4 (reserve):
	// slots 0 font, 1..4 static art, 12..19 ItemCapture. Slot 20 is the
	// preview's dedicated descriptor.
	inline constexpr std::uint32_t kSrvSlot = 20;

	struct MaterialDiagnostic
	{
		std::string mesh;
		std::string material;
		std::string colorMap;
		std::string opacityMap;
		std::string colorStatus;
		std::string opacityStatus;
		std::uint32_t colorFormat{ 0 };
		std::uint32_t colorWidth{ 0 };
		std::uint32_t colorHeight{ 0 };
		std::uint32_t opacityFormat{ 0 };
		std::uint32_t opacityWidth{ 0 };
		std::uint32_t opacityHeight{ 0 };
	};

	struct Snapshot
	{
		bool enabled{ false };
		bool ready{ false };       // a model was rendered for the current request
		std::uint64_t srvHandle{}; // ImTextureID value, 0 when nothing to show
		std::uint32_t width{ 0 };
		std::uint32_t height{ 0 };
		std::uint32_t meshes{ 0 };
		std::uint32_t tris{ 0 };
		std::uint32_t textured{ 0 };      // ranges with a bound diffuse
		std::uint32_t texturedTotal{ 0 }; // textured + plain ranges (decal sheets excluded)
		std::string model;         // resolved key the render was built from
		std::string label;         // form display name at request time
		std::string message;       // why nothing is shown (no model path, cache miss, ...)
		std::vector<MaterialDiagnostic> materials; // per-mesh task-thread resolution report
	};

	// Reads [offlinepreview] from SFShopExplorer.ini (enabled/dir/width/...).
	void LoadConfig();

	// Called once from the overlay after its device/heap exist. Direct GPU
	// preview output is published through the overlay's SRV heap.
	void DeviceReady(ID3D12Device* a_device, ID3D12DescriptorHeap* a_srvHeap, std::uint32_t a_srvIncrement);

	// Legacy cache/CPU preview API; the Shop Explorer's direct path does not
	// call this entry point. a_modelPaths
	// holds every NIF to assemble (armor: all addon NIFs; other types: one).
	// Each is resolved to <cacheDir>/<basename>.sfmodel; a_fallbackKey is an
	// alternative cache key (e.g. the form's EDID) tried when the primary
	// key has no cached model. a_weapon selects the weapon orientation
	// (muzzle +Y onto -X, slide +Z onto +Y, wood -Z onto -Y, rear -Y
	// onto +X: the inventory side profile at the weapon camera).
	// a_preferFallback (OMOD assemblies): skip the first-path cache key
	// so the EDID chain runs first — a curated hit (rattler) beats the
	// assembly's first NIF (stinger); only an EDID miss live-builds it.
	void RequestPreview(const std::vector<std::string>& a_modelPaths, const std::string& a_label,
		const std::string& a_fallbackKey = {}, bool a_weapon = false, bool a_preferFallback = false);

	// Direct live path: pre-extracted NativeModel (Clone3D) geometry handed
	// straight to the independent GPU renderer. Bypasses .sfmodel cache and
	// LiveModel NIF parsing; BA2 texture prefetch supplies material data only.
	struct DirectMesh
	{
		std::string name;
		std::string matPath;
		std::vector<float> pos;             // xyz triplets
		std::vector<float> uv;              // uv pairs (may be empty)
		std::vector<std::uint32_t> indices;
	};
	// a_authoredOrientation: the caller already rotated the geometry with the
	// form's authored preview transform (TESBoundObject::previewTransform), so
	// the renderer must NOT re-apply its own weapon/lay-flat guess on top.
	void RequestPreviewDirect(std::vector<DirectMesh> a_meshes, const std::string& a_label,
		bool a_weapon = false, bool a_authoredOrientation = false);

	// Task thread (ShopBrowser::FrameUpdate): resolves legacy requests and
	// prefetches textures for direct GPU requests. No GPU state is touched.
	void ServicePending();

	// Interactive inspection (called from the shop UI thread): orbit the
	// preview camera around the current model. Drag deltas accumulate on
	// top of the INI pitch/yaw; zoom multiplies the INI zoom. Applies to
	// the next rendered frame; double-click reset restores defaults.
	// Switching models resets the orbit automatically.
	void AddOrbit(float a_dYawDeg, float a_dPitchDeg);
	void AddZoom(float a_factor);
	void ResetOrbit();
	// Live camera readout for the viewport hint (INI base + drag offsets).
	float OrbitYawDeg();
	float OrbitPitchDeg();
	float ZoomFactor();

	// Present thread, before the ImGui frame: uploads legacy fallback pixels
	// or renders direct geometry through the independent GPU pipeline.
	void UploadGPU(ID3D12GraphicsCommandList* a_list);

	// Read-only snapshot for the shop UI.
	Snapshot GetSnapshot();

	// Kill switch used on shutdown; releases GPU resources.
	void Shutdown();
}
