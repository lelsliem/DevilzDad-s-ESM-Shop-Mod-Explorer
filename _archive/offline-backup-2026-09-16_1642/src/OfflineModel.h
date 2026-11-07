#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12DescriptorHeap;

namespace SFE::OfflineModel
{
	// Offline 3D preview — the answer for arbitrary catalog items the engine
	// will never render (no live handle, nothing owned). The plugin displays
	// models directly from a pre-extracted geometry cache:
	//
	//   tools/renderer_recon/build_model_cache.py  (runs OUTSIDE the game)
	//     BA2/loose -> MeshConverter.dll -> compact .sfmodel files
	//
	// The plugin itself never touches BA2s or foreign DLLs inside Starfield —
	// it only reads the .sfmodel files this tool produced (hard-won project
	// rule: no guessed GPU code, no foreign code in-process). Rasterization is
	// CPU-side into an R8G8B8A8 texture uploaded through the overlay's own
	// command list — zero new GPU pipelines inside the game.

	// The overlay's SRV heap is sized 12 (static) + 8 (capture) + 4 (reserve):
	// slots 0 font, 1..4 static art, 12..19 ItemCapture. Slot 20 is the
	// offline preview's dedicated descriptor.
	inline constexpr std::uint32_t kSrvSlot = 20;

	struct Snapshot
	{
		bool enabled{ false };
		bool ready{ false };       // a model was rasterized for the current request
		std::uint64_t srvHandle{}; // ImTextureID value, 0 when nothing to show
		std::uint32_t width{ 0 };
		std::uint32_t height{ 0 };
		std::uint32_t meshes{ 0 };
		std::uint32_t tris{ 0 };
		std::uint32_t textured{ 0 };      // ranges with a bound diffuse
		std::uint32_t texturedTotal{ 0 }; // textured + plain ranges (decal sheets excluded)
		std::string model;         // cache key the render was built from
		std::string label;         // form editor ID at request time
		std::string message;       // why nothing is shown (no model path, cache miss, ...)
	};

	// Reads [offlinepreview] from SFShopExplorer.ini (enabled/dir/width/...).
	void LoadConfig();

	// Called once from the overlay after its device/heap exist. Rasterized
	// pixels are uploaded through the overlay's own command list each Present.
	void DeviceReady(ID3D12Device* a_device, ID3D12DescriptorHeap* a_srvHeap, std::uint32_t a_srvIncrement);

	// PREVIEW thread: request a render for the browsed form. a_modelPaths
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

	// Task thread (ShopBrowser::FrameUpdate): rasterizes pending requests.
	// Pure CPU work — no GPU state is touched here.
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

	// Present thread, before the ImGui frame: builds pending requests
	// (parse + software rasterize + GPU upload) and refreshes the snapshot.
	void UploadGPU(ID3D12GraphicsCommandList* a_list);

	// Read-only snapshot for the shop UI.
	Snapshot GetSnapshot();

	// Kill switch used on shutdown; releases GPU resources.
	void Shutdown();
}
