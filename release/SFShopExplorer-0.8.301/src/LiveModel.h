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

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
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

	// Build-81: BSConnectPoint attach data read straight from the NIF
	// blocks ("BSConnectPoint::Parents" / "BSConnectPoint::Children").
	// Layouts validated offline against all 657 base-game weapon NIFs:
	// Parents = u32 ver, u32 count, then {u32-len host node name,
	// u32-len "P-<Slot>", 8f = quat(w,x,y,z)+translation+scale};
	// Children = u32 ver, 5 pad bytes, then {u32-len "C-<Slot>"} to block
	// end. m is the 4x4 row-major entry matrix (column-vector convention,
	// translation in m[3], m[7], m[11]).
	struct ConnectPt
	{
		std::string node;           // host node name ("" -> file root frame)
		std::string name;           // "P-<Slot>" key
		std::array<float, 16> m{};  // entry transform (quat + t + scale)
	};

	struct ConnectInfo
	{
		std::vector<ConnectPt> parents;                        // this file hosts
		std::vector<std::string> childs;                       // this file mounts
		std::unordered_map<std::string, std::array<float, 16>> nodeWorlds;  // node -> world
		std::array<float, 16> rootWorld{ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
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

	// Single-NIF parse WITHOUT the [livemodel] enabled gate and without
	// cache/sibling work: same own-NIF + .mesh LOD0 + BA2 chain BuildModel
	// uses. NativeModel calls this as Stage A2 — engine Clone3D returns the
	// scene tree (nodes/materials/shader RTTI) but Starfield stores vertex
	// and index data OUTSIDE the NIF: each BSGeo LOD entry holds a GUID key
	// resolved to geometries/<guid>.mesh (separate archive stream the
	// runtime loads through MeshLODDB stream entries), so any in-memory
	// buffer probe on the cloned tree pairs nothing. This parses the same
	// bytes the engine itself streams in. Empty output = parse failure.
	bool ParseNifMeshes(const std::string& a_modelPath, std::vector<LiveMesh>& a_out);

	// Build-81: BSConnectPoint attach data for one NIF: this file's "P-"
	// parents (host node + slot + entry transform), its "C-" children, the
	// world matrices of its named nodes and the root frame world. Returns
	// false when the NIF is missing/unparseable or carries no connect
	// blocks (a_out is still reset). NativeModel resolves assembly part
	// placement from these; geometry-only stubs (518-byte markers like
	// lawgiver_standardbarrel) still yield valid connects.
	bool ParseNifConnects(const std::string& a_modelPath, ConnectInfo& a_out);

	// Raw asset bytes by game-relative key ("meshes/...", "geometries/...",
	// "textures/..."). Same resolver BuildModel uses; safe on task thread.
	bool ReadAsset(const std::string& a_key, std::vector<std::uint8_t>& a_out);

	// Fuzzy material/node -> diffuse _color.dds lookup against the real
	// texture index (exact base wins, else longest containment, family
	// scope preferred). Empty result = shaded render for that mesh.
	// a_outScore receives the winning score when non-null (log auditing).
	// a_outOpacity receives the material database's companion opacity map
	// when the resolved texture set names one (it can live under a different
	// file stem than the colour map). Left empty when there is none, in which
	// case the caller falls back to same-stem pairing.
	bool FindDiffuse(const std::string& a_matPath, const std::string& a_nodeName, std::string& a_outKey,
		int* a_outScore = nullptr, std::string* a_outOpacity = nullptr);

	// Exact albedo from the material definition itself. Starfield .mat
	// files are JSON ("Albedo" object with a "File" path, proven on the
	// AA-43 Stalwart mod whose mats borrow base-game maps across families
	// — e.g. aa43_mag_drum.mat -> RocketLauncher_Receiver_color — which no
	// fuzzy matcher can ever guess). Base-game materials live in the CDB,
	// not as files, so this only fires for mod/loose .mats; everything
	// else falls through to FindDiffuse. Session-memoized (hit and miss).
	bool FindMatAlbedo(const std::string& a_matPath, std::string& a_outKey);

	// Authoritative base-game material -> colour map from the shipped
	// material database (materialsbeta.cdb). Base-game .mat files do not
	// exist on disk, so this is the game's own answer for produce, aid and
	// set-dressing families whose maps the fuzzy matcher cannot reach. The
	// companion opacity map is optional and can live under a different file
	// stem than the colour map, so it is returned separately. Fail closed
	// when the database has no set for the material.
	bool FindCdbMaterial(const std::string& a_matPath, std::string& a_outColor,
		std::string* a_outOpacity = nullptr);

	// MaterialOverrideColorTypeComponent::Color.w decoded from the same
	// database's DIFF records: the material's own override alpha. Returns
	// false when the database stored none, so "absent" is never confused
	// with "opaque".
	//
	// a_outLayerKind, when given, receives the object's TextureSetKind:
	// 0 = none recorded, 1 = "Multiply", 2 = "Lerp", 3 = some other named
	// kind. A layer kind means the texture set composites over a layer below
	// it, so the alpha is that layer's blend weight and NOT the surface's
	// opacity - see CdbOverride in LiveModel.cpp.
	bool FindCdbAlpha(const std::string& a_matPath, float& a_outAlpha, std::uint8_t* a_outLayerKind = nullptr);

	// ...and the same component's rgb, reported only when it is visibly
	// tinted. Feeds the flat, textureless ranges whose colour is otherwise
	// guessed from the material name.
	bool FindCdbTint(const std::string& a_matPath, float a_out[3]);

	// Layered material swap for one item form (BGSLayeredMaterialSwap).
	//
	// Starfield clothes do not carry their colour on the mesh: an ARMO names
	// swap records that rename materials, e.g. Resort Wear's
	//   Clothes\Outfit_Trucker_Tee\Outfit_Trucker_Tee_Upperbody.mat
	//     -> ...Outfit_Trucker_Tee_Resort_Upperbody.mat
	// and Star Roamer's SpaceSuit_SpaceTrucker_* -> ...\Generic\*.mat. A
	// preview built from the NIF's own material names therefore shows the
	// stock garment (grey tee, blue SpaceTrucker suit) instead of the item.
	//
	// The table is generated from the plugin files by
	// tools/gen_material_swaps.py into <cacheDir>/materialswaps.tsv, one
	// "<ARMO formID hex>\t<key material>\t<override material>" per line.
	// Returns the override path for that item's material, or an empty string
	// when the item has no swap for it (the common case). Reads the table
	// once, lazily; a missing file keeps the previous behaviour.
	std::string MaterialSwapFor(std::uint32_t a_formID, const std::string& a_matPath);

	// Unpacked contiguous mip chain for a texture key (archive chunks or loose
	// .dds). Format is render-ready (sRGB sources mapped to UNORM to match
	// the preview look); mip 0 is first and rows/blocks are tightly packed.
	struct TexImage
	{
		std::uint32_t format{ 0 };  // DXGI_FORMAT value
		std::uint32_t width{ 0 };
		std::uint32_t height{ 0 };
		std::uint32_t mipLevels{ 1 };
		std::size_t rowPitch{ 0 };  // tightly packed mip-0 row
		std::vector<std::uint8_t> bytes;
	};

	// Optional source trace for diagnostics (loose directory or archive path).
	// Leave null on the normal texture-prefetch path to avoid extra work.
	bool ReadTexture(const std::string& a_key, TexImage& a_out, std::string* a_outSource = nullptr);

	// Persist meshes in SFM1 layout so later selections hit the disk cache.
	bool WriteSfModel(const std::filesystem::path& a_path, const std::vector<LiveMesh>& a_meshes);

	// NIF discovery for path-less forms (dummy/empty model field): scan
	// mesh-archive name tables for <token>.nif candidates and return the
	// best game-relative key. Exact basenames win (largest packed entry
	// first — marker NIFs are tiny); otherwise the shortest non-part
	// basename; mods/ subdirs (attachment parts) never qualify. Session-
	// memoized, hit and miss. Safe on the task thread (opens archives).
	bool DiscoverNif(const std::string& a_edid, std::string& a_outNifKey);

	// Build-116: one background warm-up step (call every task tick, task
	// thread only). Opens exactly one BA2 per call — mesh archives first
	// (EnsureBa2List order) — then builds the color/opacity indexes once
	// all archives are cached. Without this the first selections paid the
	// archive TOC indexing inside ParseNifMeshes ON THE PRESENT THREAD
	// (46-680ms per cold archive: a visible freeze per click) and the
	// first FindDiffuse held g_mutex across a full ~6s archive scan,
	// stalling every click during it. No-ops after completion.
	void WarmTick();
}
