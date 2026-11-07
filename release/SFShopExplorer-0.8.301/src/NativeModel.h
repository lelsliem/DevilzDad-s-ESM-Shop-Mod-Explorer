#pragma once

// Game-native 3D model loader — uses the engine's own Clone3D to load a
// TESBoundObject's NiAVObject with correct materials, textures, and scene
// graph.  Extracts geometry from the NiAVObject tree and feeds it into the
// OfflineModel GPU pipeline, replacing the NIF-parser path that can't resolve
// base-game materials (the FindDiffuse fuzzy matcher picks wrong textures for
// weapons like the Rattler because it can't read the materialsbeta.cdb).
//
// Pipeline:
//   FormID → TESForm → TESBoundObject::Clone3D → NiAVObject tree
//   → walk NiNode children → extract BSGeometry verts/UVs/normals
//   → resolve textures via BSLightingShaderProperty
//   → feed into OfflineModel's existing GPU rasterizer (same PSO/RTV/SRV)

#include <cstdint>
#include <string>
#include <vector>

namespace SFE::NativeModel
{
	struct NativeMesh
	{
		std::string name;       // NiObjectNET name
		std::string matPath;    // material path from shader property
		std::string texPath;    // resolved diffuse texture path
		std::vector<float> pos; // xyz triplets, world-space
		std::vector<float> uv;  // uv pairs
		std::vector<float> nrm; // xyz normals
		std::vector<std::uint32_t> indices;
	};

	struct PreviewResult
	{
		bool loaded{ false };     // Clone3D succeeded
		bool extracted{ false };  // geometry extraction succeeded
		float center[3]{ 0, 0, 0 };
		float radius{ 0 };
		std::uint32_t nodeCount{ 0 };
		std::uint32_t geoCount{ 0 };
		std::uint32_t triCount{ 0 };
		std::string message;
		std::vector<NativeMesh> meshes;
	};

	// Load a form's 3D model via the game's own Clone3D and extract geometry.
	// Safe on the task thread.  Returns null meshes when Clone3D fails or the
	// NiAVObject tree contains no extractable geometry.
	//
	// a_modelPaths: FormModelPaths / OMOD assembly NIFs. When the form's own
	// TESModel path is a dummy (BullpupDummyReceiver), each path is written
	// into form+0x100 (TESObjectWEAP model BSFixedString), Clone3D'd, then
	// the original string is restored so the form is left unchanged.
	PreviewResult LoadFromForm(std::uint32_t a_formID, const std::vector<std::string>& a_modelPaths = {});

	// Read back the model the *engine* loaded for the barter menu's preview.
	//
	// This is the native route: the form is dispatched through
	// BarterMenu_LoadModel (see ShopExplorer's TryNativeVendorPreview), the
	// engine loads the item the way it loads its own preview, and this finds
	// the resulting NiAVObject by walking that event source's sink list to the
	// BarterMenu object and scanning it for a NiNode. No engine code is called
	// here -- it is a bounded, CanRead-gated read of the menu's own state.
	//
	// Returns a result with `loaded == false` (and a `message` naming what was
	// missing) when the menu or the model is not present, so the caller can
	// report the failure instead of showing a picture from another source.
	PreviewResult LoadFromBarterMenu(std::uint32_t a_formID);

	// True while the engine's BarterMenu is open. The native read-back can only
	// succeed then: the menu registers its LoadModel sink when it opens and
	// unregisters it on close, so a closed menu has no sink, no scene and
	// nothing for a dispatch to reach. Callers use this as a cache key so a
	// menu opened *after* the shop UI still triggers a fresh dispatch.
	bool BarterMenuAvailable();

	// The engine's own preview objects, read out of whichever menu currently
	// holds them.
	//
	// The offsets come from live F10 captures (SFDevilzDump, 2026-10-17):
	// InventoryMenu +0x198 and DataMenu +0x1358 both hold a
	// UICellRenderer_InventoryItem, and that renderer holds the previewed
	// TESObjectREFR (+0xc0), the BSMenu3D::Scene behind it (+0x68) and the
	// ItemModelLoadTask (+0xd0). When the measured offset does not resolve to a
	// renderer the menu is scanned instead, so a menu whose offset differs still
	// works. Nothing here calls engine code, but the objects it reads belong to
	// the main thread, which frees them while the SFSE task thread walks them:
	// every load is therefore fault-gated (SafeReadPtr) and a menu is only
	// walked while RE::UI reports it open. A readability probe is not enough -
	// its region cache can outlive a partial decommit (2026-10-18 CTD while the
	// inventory opened; see reports/preview-chain-ctd-2026-10-18.md).
	struct EnginePreviewChain
	{
		bool           found{ false };
		std::string    menuName;
		std::uintptr_t menu{ 0 };
		std::uintptr_t renderer{ 0 };
		std::uintptr_t refr{ 0 };      // previewed item's 3D reference
		std::uintptr_t scene{ 0 };     // BSMenu3D::Scene behind the renderer
		std::uintptr_t loadTask{ 0 };  // ItemModelLoadTask
		float          refrPosition[3]{ 0.0f, 0.0f, 0.0f };
		std::string    rendererType;
		std::string    refrType;
	};

}
