#pragma once

// Live in-game model builder (Plan B).
//
// The prebuilt .sfmodel cache can never cover mod-added weapons, so the
// plugin resolves and parses the game's own asset bytes at runtime, using
// only its own code inside the game process:
//
//   form model path ("Weapons\...\X.nif")
//     -> loose file under <game>/Data or INI extradirs (MO2 mod folders)
//     -> else "meshes/..." entry in Data/*.ba2 (+ INI extraba2)
//     -> own NIF 20.2.0.7 parser: node tree, local transforms, BSGeometry
//        factory GUIDs, material refs (validated byte-for-byte against
//        MeshConverter ground truth on rattler/ar99)
//     -> own .mesh LOD0 parser: u16 indices + s16-normalized positions
//        (bounds match the NIF bounding spheres exactly)
//     -> world-space vertices -> written to the .sfmodel cache on disk
//
// No game functions are called, no foreign DLLs load in-process, no GPU
// state is touched here: output is plain CPU vertex data that flows into
// the existing OfflineModel rasterizer. Every parse step fails closed
// (returns false) on unexpected bytes; the shop then shows the usual
// "no cached model" message for that item.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace SFE::LiveModel
{
	struct LiveMesh
	{
		std::string name;      // mesh name (matches .sfmodel name field)
		std::string matPath;   // material path from the shader block
		std::vector<float> pos;// xyz triplets, world-space, engine units
		std::vector<float> uv; // uv pairs (empty when unavailable)
		std::vector<std::uint32_t> indices;
	};

	// Reads [livemodel] from SFShopExplorer.ini (enabled/extradirs/extraba2).
	// Must be called (once) before BuildModel; safe on any thread.
	void LoadConfig();

	bool Enabled();

	// Model cache directory (same dir OfflineModel reads .sfmodel files from).
	std::filesystem::path CacheDir();

	// Build world-space meshes for a form model path. Returns false when the
	// NIF cannot be found or fails validation. Safe on the task thread.
	bool BuildModel(const std::string& a_modelPath, std::vector<LiveMesh>& a_out);

	// Multi-NIF build for armor forms wearing several addons at once: every
	// path is built (with its outfit siblings) exactly once, deduplicated by
	// normalized NIF key. Merged output, one call.
	bool BuildModels(const std::vector<std::string>& a_modelPaths, std::vector<LiveMesh>& a_out);

	// Raw asset bytes by game-relative key ("meshes/...", "geometries/...",
	// "textures/..."). Same resolver BuildModel uses; safe on task thread.
	bool ReadAsset(const std::string& a_key, std::vector<std::uint8_t>& a_out);

	// Fuzzy material/node -> diffuse _color.dds lookup against the real
	// texture index (exact base wins, else longest containment, family
	// scope preferred). Empty result = shaded render for that mesh.
	// a_outScore receives the winning score when non-null (log auditing).
	bool FindDiffuse(const std::string& a_matPath, const std::string& a_nodeName, std::string& a_outKey,
		int* a_outScore = nullptr);

	// Exact albedo from the material definition itself. Starfield .mat
	// files are JSON ("Albedo" object with a "File" path, proven on the
	// AA-43 Stalwart mod whose mats borrow base-game maps across families
	// — e.g. aa43_mag_drum.mat -> RocketLauncher_Receiver_color — which no
	// fuzzy matcher can ever guess). Base-game materials live in the CDB,
	// not as files, so this only fires for mod/loose .mats; everything
	// else falls through to FindDiffuse. Session-memoized (hit and miss).
	bool FindMatAlbedo(const std::string& a_matPath, std::string& a_outKey);

	// Unpacked top-mip image for a texture key (archive chunks or loose
	// .dds). Format is render-ready (sRGB sources mapped to UNORM to match
	// the preview look); empty bytes = unsupported.
	struct TexImage
	{
		std::uint32_t format{ 0 };  // DXGI_FORMAT value
		std::uint32_t width{ 0 };
		std::uint32_t height{ 0 };
		std::size_t rowPitch{ 0 };
		std::vector<std::uint8_t> bytes;
	};
	bool ReadTexture(const std::string& a_key, TexImage& a_out);

	// Persist meshes in SFM1 layout so later selections hit the disk cache.
	bool WriteSfModel(const std::filesystem::path& a_path, const std::vector<LiveMesh>& a_meshes);

	// NIF discovery for path-less forms (dummy/empty model field): scan
	// mesh-archive name tables for <token>.nif candidates and return the
	// best game-relative key. Exact basenames win (largest packed entry
	// first — marker NIFs are tiny); otherwise the shortest non-part
	// basename; mods/ subdirs (attachment parts) never qualify. Session-
	// memoized, hit and miss. Safe on the task thread (opens archives).
	bool DiscoverNif(const std::string& a_edid, std::string& a_outNifKey);
}
