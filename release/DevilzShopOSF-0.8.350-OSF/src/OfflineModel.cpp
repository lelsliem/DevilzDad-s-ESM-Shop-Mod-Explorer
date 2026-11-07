#include "PCH.h"
#include "LiveModel.h"
#include "OfflineModel.h"
#include "ShopExplorer.h"  // kIniFileName: each front end reads its own ini

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <Windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>

#ifdef ERROR
#undef ERROR
#endif
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

namespace SFE::OfflineModel
{
	namespace
	{
		// Root-qualified SDK types (same aliasing discipline as ItemCapture:
		// CommonLibSF headers pull REX::W32 declarations that would shadow
		// unqualified D3D12 names).
		using NativeDevice = ::ID3D12Device;
		using NativeList = ::ID3D12GraphicsCommandList;
		using NativeHeap = ::ID3D12DescriptorHeap;

		double ParseFloat(const std::filesystem::path& a_ini, const wchar_t* a_section, const wchar_t* a_key, double a_fallback)
		{
			wchar_t value[32]{};
			GetPrivateProfileStringW(a_section, a_key, L"", value, static_cast<DWORD>(std::size(value)), a_ini.c_str());
			if (!value[0]) {
				return a_fallback;
			}
			try {
				// stod has a wstring overload; narrowing first truncated any
				// non-ASCII digit separator and tripped C4244.
				const std::wstring wide{ value };
				return std::stod(wide);
			} catch (...) {
				return a_fallback;
			}
		}

		// ------------------------------------------------------------------
		// Geometry cache (written by tools/renderer_recon/build_model_cache.py)
		//
		// SFM1 layout: magic 'SFM1', u32 numMeshes, then per mesh:
		//   char name[64], u32 matPathLen + bytes, u32 numVerts,
		//   u32 numIndices, numVerts * (3f pos + 2f uv), numIndices * u32
		// Vertex positions are engine units; the rasterizer normalizes by the
		// model bounding sphere so every item frames identically.
		// ------------------------------------------------------------------
		struct Mesh
		{
			std::string name;
			std::string matPath;   // shader material (diffuse matching)
			std::vector<float> pos;    // xyz triplets
			std::vector<float> uv;     // uv pairs (may be empty)
			std::vector<std::uint32_t> indices;
		};

		struct Model
		{
			std::vector<Mesh> meshes;
			std::uint32_t tris{ 0 };
			float center[3]{ 0.0f, 0.0f, 0.0f };
			float radius{ 1.0f };
			float extents[3]{ 1.0f, 1.0f, 1.0f };  // full axis spans, for lay-flat framing
		};

		std::mutex g_cacheMutex;
		std::unordered_map<std::string, std::shared_ptr<const Model>> g_cache;
		// Direct NativeModel payloads retained only so evicted texture prefetches
		// can be warmed again; they never enter ResolveModel or disk cache.
		std::unordered_map<std::string, std::shared_ptr<const Model>> g_directModels;
		std::unordered_map<std::string, std::string> g_aliases;

		std::filesystem::path g_cacheDir;
		// The shop viewport is commonly drawn larger than the old 512x384
		// backing texture (especially in the movable Preview pop-out). Render
		// at 1024x768 by default so the UI is not forced to upscale a low-res
		// weapon image; the INI can still tune this for slower systems.
		std::uint32_t g_width{ 1024 };
		std::uint32_t g_height{ 768 };
		float g_pitchDeg{ -18.0f };
		float g_yawDeg{ 35.0f };
		float g_zoom{ 1.8f };
		// Weapon camera defaults ([weaponview] wyaw/wpitch): user-calibrated
		// against the inventory scene. Applied to weapon-oriented models
		// instead of the generic pitch/yaw above.
		float g_weaponYaw{ 40.0f };
		float g_weaponPitch{ 20.0f };
		// Model backdrop. g_bgDefault is a dark slate close to the game's own
		// inventory background, so the rendered model sits in the same world as
		// the shop chrome instead of on a white product-shot field. Per-channel
		// overrides come from [offlinepreview] bg/bgg/bgb as 0-255; any channel
		// left unset (or negative) falls back to the matching default, so a
		// partial override tints rather than replacing.
		constexpr float g_bgDefault[3]{ 0.055f, 0.070f, 0.095f };
		float g_bgR{ -1.0f };
		float g_bgG{ -1.0f };
		float g_bgB{ -1.0f };
		std::atomic<bool> g_orbitWeapon{ false };
		// Set by RequestPreviewDirect when the caller already applied the form's
		// authored preview rotation; BuildGpuModel must then skip its own guess.
		std::atomic<bool> g_authoredOrientation{ false };
		std::atomic<bool> g_enabled{ true };
		std::atomic<bool> g_useCache{ true };  // [offlinepreview] usecache (default on)
		bool g_configLoaded{ false };

		std::filesystem::path DllDirectory()
		{
			HMODULE self = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&DllDirectory), &self);
			wchar_t modulePath[MAX_PATH]{};
			GetModuleFileNameW(self, modulePath, MAX_PATH);
			return std::filesystem::path(modulePath).parent_path();
		}

		std::string LowerAscii(std::string_view a_text)
		{
			std::string out;
			out.reserve(a_text.size());
			for (const char c : a_text) {
				out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
			}
			return out;
		}

		// "Weapons\Foo\ak_47.nif" -> "ak_47"
		std::string ModelCacheKey(std::string_view a_modelPath)
		{
			const auto slash = a_modelPath.find_last_of("\\/");
			if (slash != std::string_view::npos) {
				a_modelPath.remove_prefix(slash + 1);
			}
			const auto dot = a_modelPath.find_last_of('.');
			if (dot != std::string_view::npos) {
				a_modelPath.remove_suffix(a_modelPath.size() - dot);
			}
			return LowerAscii(a_modelPath);
		}

		void ComputeBounds(Model& a_model)
		{
			float lo[3]{ 1e30f, 1e30f, 1e30f };
			float hi[3]{ -1e30f, -1e30f, -1e30f };
			for (const auto& mesh : a_model.meshes) {
				for (std::size_t i = 0; i + 2 < mesh.pos.size(); i += 3) {
					for (int axis = 0; axis < 3; ++axis) {
						lo[axis] = std::min(lo[axis], mesh.pos[i + axis]);
						hi[axis] = std::max(hi[axis], mesh.pos[i + axis]);
					}
				}
			}
			for (int axis = 0; axis < 3; ++axis) {
				a_model.center[axis] = (lo[axis] + hi[axis]) * 0.5f;
				a_model.extents[axis] = std::max(hi[axis] - lo[axis], 1e-4f);
			}
			float radius = 0.0f;
			for (const auto& mesh : a_model.meshes) {
				for (std::size_t i = 0; i + 2 < mesh.pos.size(); i += 3) {
					const float dx = mesh.pos[i] - a_model.center[0];
					const float dy = mesh.pos[i + 1] - a_model.center[1];
					const float dz = mesh.pos[i + 2] - a_model.center[2];
					radius = std::max(radius, std::sqrt(dx * dx + dy * dy + dz * dz));
				}
			}
			a_model.radius = radius > 1e-4f ? radius : 1.0f;
		}

		std::shared_ptr<const Model> LoadModel(const std::string& a_key, bool a_forceDisk = false)
		{
			{
				std::scoped_lock lock(g_cacheMutex);
				const auto it = g_cache.find(a_key);
				if (it != g_cache.end()) {
					return it->second;
				}
			}
			// Cache-free experiment: skip disk reads unless forced (the
			// just-written live reload). Memory cache above always applies.
			if (!a_forceDisk && !g_useCache.load(std::memory_order_acquire)) {
				return nullptr;
			}
			const auto path = g_cacheDir / (a_key + ".sfmodel");
			std::ifstream file(path, std::ios::binary);
			if (!file) {
				return nullptr;
			}
			char magic[4]{};
			file.read(magic, 4);
			if (std::memcmp(magic, "SFM1", 4) != 0) {
				REX::WARN("SFShopExplorer offline: bad model magic in {}", path.string());
				return nullptr;
			}
			std::uint32_t numMeshes = 0;
			file.read(reinterpret_cast<char*>(&numMeshes), 4);
			auto model = std::make_shared<Model>();
			model->meshes.reserve(std::min<std::uint32_t>(numMeshes, 512));
			for (std::uint32_t m = 0; m < numMeshes && file; ++m) {
				char name[64]{};
				file.read(name, 64);
				std::uint32_t matLen = 0;
				file.read(reinterpret_cast<char*>(&matLen), 4);
				if (!file || matLen > 1'000'000) {
					REX::WARN("SFShopExplorer offline: bad matLen {} in {}", matLen, path.string());
					return nullptr;
				}
				std::string matPath(matLen, '\0');
				if (matLen) {
					file.read(matPath.data(), matLen);
					if (!file) {
						return nullptr;
					}
				}
				std::uint32_t numVerts = 0;
				std::uint32_t numIndices = 0;
				file.read(reinterpret_cast<char*>(&numVerts), 4);
				file.read(reinterpret_cast<char*>(&numIndices), 4);
				if (!file || numVerts == 0 || numVerts > 4'000'000 || numIndices == 0 || numIndices > 8'000'000) {
					if (!file) {
						return nullptr;
					}
					REX::WARN("SFShopExplorer offline: bad counts v={} i={} in {}", numVerts, numIndices, path.string());
					return nullptr;
				}
				Mesh mesh;
				mesh.name = name;
				if (matLen > 0 && matLen < 1024) {
					mesh.matPath = matPath;
				}
				// On disk each vertex is interleaved as 5 floats (x y z u v).
				// De-interleave into pos[] + throwaway uvs (textures deferred).
				std::vector<float> interleaved(static_cast<std::size_t>(numVerts) * 5);
				file.read(reinterpret_cast<char*>(interleaved.data()), static_cast<std::streamsize>(interleaved.size() * sizeof(float)));
				if (!file || static_cast<std::size_t>(file.gcount()) != interleaved.size() * sizeof(float)) {
					return nullptr;
				}
				mesh.pos.resize(static_cast<std::size_t>(numVerts) * 3);
				mesh.uv.resize(static_cast<std::size_t>(numVerts) * 2);
				for (std::uint32_t v = 0; v < numVerts; ++v) {
					mesh.pos[static_cast<std::size_t>(v) * 3 + 0] = interleaved[static_cast<std::size_t>(v) * 5 + 0];
					mesh.pos[static_cast<std::size_t>(v) * 3 + 1] = interleaved[static_cast<std::size_t>(v) * 5 + 1];
					mesh.pos[static_cast<std::size_t>(v) * 3 + 2] = interleaved[static_cast<std::size_t>(v) * 5 + 2];
					mesh.uv[static_cast<std::size_t>(v) * 2 + 0] = interleaved[static_cast<std::size_t>(v) * 5 + 3];
					mesh.uv[static_cast<std::size_t>(v) * 2 + 1] = interleaved[static_cast<std::size_t>(v) * 5 + 4];
				}
				mesh.indices.resize(numIndices);
				file.read(reinterpret_cast<char*>(mesh.indices.data()), static_cast<std::streamsize>(numIndices * sizeof(std::uint32_t)));
				if (!file || static_cast<std::size_t>(file.gcount()) != numIndices * sizeof(std::uint32_t)) {
					return nullptr;
				}
				model->tris += numIndices / 3;
				model->meshes.push_back(std::move(mesh));
			}
			if (model->meshes.empty()) {
				return nullptr;
			}
			// Cap: no single model needs >512 meshes or >200K tris in shop.
			if (model->meshes.size() > 512) {
				model->meshes.resize(512);
			}
			ComputeBounds(*model);
			REX::INFO("SFShopExplorer offline: loaded {} ({} meshes, {} tris, r={:.1f})",
				path.filename().string(), model->meshes.size(), model->tris, model->radius);
			std::scoped_lock lock(g_cacheMutex);
			return g_cache.emplace(a_key, std::move(model)).first->second;
		}

		// ------------------------------------------------------------------
		// Software rasterizer â€” CPU fill into an RGBA buffer. Deterministic,
		// engine-independent, and identical on every machine: the "no foreign
		// GPU code inside Starfield" rule kept intact.
		// ------------------------------------------------------------------
		void Rotated(float a_x, float a_y, float a_z, float a_sinYaw, float a_cosYaw, float a_sinPitch, float a_cosPitch, float a_out[3])
		{
			// yaw around world up (Y), then pitch around view X
			const float x1 = a_x * a_cosYaw + a_z * a_sinYaw;
			const float z1 = -a_x * a_sinYaw + a_z * a_cosYaw;
			const float y2 = a_y * a_cosPitch - z1 * a_sinPitch;
			const float z2 = a_y * a_sinPitch + z1 * a_cosPitch;
			a_out[0] = x1;
			a_out[1] = y2;
			a_out[2] = z2;
		}

		std::mutex g_renderMutex;            // task thread rasterizes; Present thread uploads
		std::vector<std::uint32_t> g_pixels; // BGRA8, one per texel
		std::shared_ptr<const Model> g_pendingModel;
		bool g_publishedDirect{ false };
		std::string g_pendingKey;      // RESOLVED cache key (written at publish)
		std::string g_pendingLabel;
		std::vector<std::string> g_pendingPaths;  // every form NIF for legacy cache/live requests
		std::string g_pendingFallback; // EDID fallback cache key
		std::shared_ptr<const Model> g_requestDirectModel;
		bool g_requestDirect{ false };
		bool g_requestWeapon{ false };   // request-side weapon orient flag
		bool g_publishedWeapon{ false }; // orient flag of the published model
		// What the shop asked for (model-path key + label). Resolution may
		// settle on a DIFFERENT key (alias/EDID fallback), so the request
		// pair â€” never the resolved key â€” is the stability comparison.
		// Comparing against the resolved key re-fires every frame for
		// alias-resolved items (most guns), resetting the orbit each time:
		// the "spring back to start" bug.
		std::string g_requestKey;
		std::string g_requestLabel;
		std::atomic<std::uint32_t> g_requestGen{ 0 };     // bumped by RequestPreview
		std::uint32_t g_rasterGen{ 0 };                   // last generation rasterized
		std::uint32_t g_uploadedGen{ 0 };                 // last generation uploaded

		// Interactive orbit (shop UI thread writes, render/task threads read):
		// drag deltas on top of the INI camera + a dirty flag that asks the
		// Present path for one more frame. Lock-free so Present never waits.
		std::atomic<float> g_yawOff{ 0.0f };
		std::atomic<float> g_pitchOff{ 0.0f };
		std::atomic<float> g_zoomMul{ 1.0f };
		std::atomic<bool> g_orbitDirty{ false };

		struct GpuState
		{
			NativeDevice* device{ nullptr };
			NativeHeap* srvHeap{ nullptr };
			std::uint32_t srvIncrement{ 0 };
			::ID3D12Resource* texture{ nullptr };
			::ID3D12Resource* upload{ nullptr };
			std::uint32_t texWidth{ 0 };
			std::uint32_t texHeight{ 0 };
			bool srvCreated{ false };
			bool rtReady{ false };  // texture allows render-target use
			int texState{ 0 };      // 0 copy-dest, 1 render-target, 2 shader-resource
			std::uint64_t srvHandle{ 0 };
		};
		GpuState g_gpu;
		bool g_gpuMode{ true };
		// Diagnostic switches read from previewdebug.ini next to the DLL.
		// Defaults keep the shipping path unchanged; the file is only used to
		// bisect the A-300 invalid call without rebuilding for every stage.
		bool g_debugRender{ true };
		bool g_debugUpload{ true };
		bool g_debugDraw{ true };
		// Per-model GPU ban (session-scoped): one bad model (e.g. a 12-tri
		// test-clutter cube with no renderable range) falls back to CPU for
		// itself only. The old global flag killed GPU for the whole session
		// on the first trivial failure.
		std::mutex g_gpuBanMutex;
		std::unordered_set<std::string> g_gpuBanned;

		bool IsGpuBanned(const std::string& a_key)
		{
			std::scoped_lock lock(g_gpuBanMutex);
			return g_gpuBanned.find(a_key) != g_gpuBanned.end();
		}

		void BanGpuKey(const std::string& a_key)
		{
			std::scoped_lock lock(g_gpuBanMutex);
			if (g_gpuBanned.insert(a_key).second) {
				REX::WARN("SFShopExplorer offline GPU: '{}' banned from GPU path (CPU fallback for this model only)", a_key);
			}
		}

		std::mutex g_snapshotMutex;
		Snapshot g_snapshot;

		// Per-mesh accent colors (stylized shading â€” real PBR textures are a
		// later sub-project: materials live in materialsbeta.cdb).
		// Deliberately NEUTRAL greys: first paint (before the task thread
		// decodes textures) must read as "untextured clay", never as rust
		// or blued steel. Alternating values keep parts distinguishable.
		void MeshColor(std::size_t a_index, float a_out[3])
		{
			static constexpr float palette[][3] = {
				{ 0.62f, 0.62f, 0.63f },  // mid grey
				{ 0.55f, 0.55f, 0.57f },  // dark grey
				{ 0.70f, 0.70f, 0.71f },  // light grey
				{ 0.48f, 0.48f, 0.50f },  // gunmetal grey
				{ 0.66f, 0.66f, 0.68f },  // steel grey
				{ 0.75f, 0.75f, 0.76f },  // pale grey
				{ 0.58f, 0.58f, 0.60f },  // grey accent
				{ 0.82f, 0.82f, 0.83f },  // highlight grey
			};
			const auto& c = palette[a_index % std::size(palette)];
			a_out[0] = c[0];
			a_out[1] = c[1];
			a_out[2] = c[2];
		}

		// ------------------------------------------------------------------
		// Independent GPU renderer: same stages as any mesh renderer
		// (vertex buffers -> MVP -> lighting -> render target -> ImGui SRV)
		// but built entirely from our own objects. No game functions, no
		// engine resources, no hooks: our own root signature, shaders
		// (compiled from the sources below), VB/IB uploads, RTV/DSV and
		// depth buffer. Direct NativeModel requests fail visibly instead of
		// falling back to the removed legacy CPU/cache renderer.
		// ------------------------------------------------------------------
		static constexpr std::uint32_t kMaxGpuRanges = 128;
		static constexpr std::uint32_t kMaxGpuTextures = 128;
		// Per-range coverage probes. 64 triangle centroids are enough to place a
		// range on its map's own scale (measured: within 0.015 of the rasterised
		// UV footprint on the four drink-bag parts).
		static constexpr std::size_t kMaxUvSamples = 64;

		struct GpuPipe
		{
			// 12: the additive effect-shell pass (the jar's fluid/caustic).
			static constexpr std::uint32_t kVersion = 12;

			::ID3D12RootSignature* rootSig{ nullptr };
			::ID3D12PipelineState* pso{ nullptr };
			::ID3D12PipelineState* psoTextured{ nullptr };
			::ID3D12PipelineState* psoTexturedAlpha{ nullptr };
			::ID3D12PipelineState* psoDecalCoverage{ nullptr };
			::ID3D12PipelineState* psoBlend{ nullptr };
			::ID3D12PipelineState* psoAdditive{ nullptr };
			::ID3D12DescriptorHeap* rtvHeap{ nullptr };
			::ID3D12DescriptorHeap* dsvHeap{ nullptr };
			::ID3D12DescriptorHeap* srvHeap{ nullptr };  // slot 0 white, then albedos, opacity masks, and detail maps
			::ID3D12Resource* depth{ nullptr };
			::ID3D12Resource* cb{ nullptr };
			::ID3D12Resource* whiteTex{ nullptr };   // 1x1 DEFAULT texture (list upload)
			::ID3D12Resource* whiteStage{ nullptr }; // retained staging
			std::uint8_t* cbMapped{ nullptr };
			std::uint32_t srvIncrement{ 0 };
			std::uint32_t width{ 0 };
			std::uint32_t height{ 0 };
			std::uint32_t version{ 0 };
			bool ready{ false };
		};
		GpuPipe g_pipe;

		struct GpuDrawRange
		{
			std::uint32_t start{ 0 };
			std::uint32_t count{ 0 };
			float color[3]{ 0.6f, 0.6f, 0.6f };
			int texSlot{ -1 };  // SRV slot when textured, else plain
			int opacitySlot{ -1 };  // companion mask SRV slot, if the material has one
			int detailSlot{ -1 };  // optional detail texture for a material-specific composite
			int normalSlot{ -1 };  // BC5 tangent-space normal map, when the material has one
			bool opacityRedChannel{ false };  // BC4 masks select red; BC3 masks select alpha
			// Metalness and roughness of the surface, as the means of its BC4
			// _metal/_rough companions. metal 0 makes the metal term in the
			// opaque shader vanish exactly, so every material without those maps
			// - cloth, food, wood - renders precisely as it did before.
			float metal{ 0.0f };
			float rough{ 0.5f };
			bool woodEmboss{ false };  // shade the Fancy grip engraving into the wood albedo
			bool razorbackGripBadge{ false };  // composite the Fancy badge over standard wood within its mask
				bool crucibleEmissive{ false };  // static emissive-map contribution for Crucible blade materials
			bool vortexGrenadeEmissive{ false };  // authored DLC001 grenade emissive atlas
			bool forceNeutralGrey{ false };  // exact Breach Tactical Stock preview override
			bool translucent{ false };  // drawn in a second alpha-blended pass
			// A translucent surface the database gives neither an albedo nor an
			// authored opacity: a clear shell. It has to transmit what is behind
			// it instead of painting a body at its Fresnel coverage - that body
			// is what veiled every snow globe's interior behind a milky film,
			// where the engine's own render of the same globe is clear glass
			// with one bright rim (2026-10-23 NY pair).
			bool clearShell{ false };
			// An inside-out hull: a shell whose faces all point at the model's
			// centre and that wraps the rest of it. Every snow globe's backdrop
			// is one. This renderer rasterises two-sided, so the near wall of
			// such a hull is drawn in front of the specimen and hides it; the
			// engine draws the inside of the hull, so this range alone takes a
			// back-face-culled pipeline (see the rule in BuildGpuModel).
			bool interiorShell{ false };
			// Build-time geometry facts the rule above is decided from: the
			// area-weighted mean cosine between each face normal and the
			// direction out of the mesh's own centroid (-1 = every face points
			// inward), and the range's bounds in normalised model space.
			float buildInward{ 0.0f };
			float buildMin[3]{ 0.0f, 0.0f, 0.0f };
			float buildMax[3]{ 0.0f, 0.0f, 0.0f };
			bool filmCoverage{ false };  // blended with its opacity map instead of clipped by it
			bool liquidCoverage{ false };  // the drink inside a drink bag, decided from that same map
			bool noCoverageClip{ false };  // solid part of a drink bag: bound map must not clip it
			bool cdbOverrideTint{ false };  // colour came from the database's MaterialOverrideColor
			// The paint case: the database knows this material but its texture set
			// names no colour map, and the database carries an override colour.
			// The map such a range resolves to is a shared neutral paint base
			// (ammo crates: paint01_color / ammobox01_color), so the database
			// colour IS the paint and must survive the albedo upload.
			bool cdbPaintTint{ false };
			// The colour came from the material's NAME (FlatBaseColor) and no
			// database value overrode it. Kept so the upload can refuse to reset
			// it to white when the only map it found is a neutral stand-in - the
			// Hypergiant Heart's `Crafted_InHailerYellow` case below.
			bool flatNameTint{ false };
			// The material's opacity map is a near-solid wrap (the Erdebrau
			// pilsner label wrap measures 88% coverage) rather than a sheet
			// with holes. Such a map
			// still has to cut its own edges, but it must NOT be drawn through a
			// source-alpha blend: the label IS the can body, and blending it at
			// its coverage turned the whole can see-through.
			bool maskMostlySolid{ false };
			// The albedo bound for this range IS its opacity mask (the material's
			// set names no colour map). An R8/BC4 SRV samples as (r,0,0), so the
			// shader has to read the coverage as a white body - reading it as
				// colour painted the Sparkling Wine's shell maroon-black once the
			// glass around it stopped covering it up.
			bool promotedCutout{ false };
			// The bound albedo is a single-channel mask (BC4/BC8), i.e. coverage
			// with no colour of its own, and this range is not a decal sheet. Read
			// as a white body with the mask's dark pixels cut out, exactly like a
			// promoted cutout. See the upload for the measured case.
			bool maskAlbedo{ false };
			// A glow/gobo/caustic overlay: a full-body shell whose texture is
			// mostly black. It is not a surface - drawn opaque it paints the
			// item black and buries whatever the shell encloses.
			bool effectOverlay{ false };
			// The subset of effect shells that is drawn - additively - rather
			// than skipped: the animated caustic/gobo loop. See
			// IsAdditiveShellMaterial for why a glow shell is not one of them.
			bool additiveShell{ false };
			// Up to kMaxUvSamples triangle-centroid UVs. The part a range is
			// (film, drink, print, cap) is only readable from its coverage map,
			// which arrives with the prefetch, so the sample points travel with
			// the range instead. Measured offline, their mean reproduces the
			// rasterised UV footprint to within 0.015 on all four parts.
			std::vector<float> uvSamples;

			float alpha{ 1.0f };  // this range's blend alpha, fed to the shader
			std::string matPath;
			std::string nodeName;
		};

		struct GpuModel
		{
			std::string key;
			::ID3D12Resource* vb{ nullptr };
			::ID3D12Resource* ib{ nullptr };
			std::vector<::ID3D12Resource*> tex;      // owned diffuse textures
			std::vector<::ID3D12Resource*> texStage; // staging kept until switch
			bool texturesReady{ false };
			std::uint32_t vbStride{ 32 };
			std::uint32_t vbBytes{ 0 };
			std::uint32_t ibCount{ 0 };
			std::vector<GpuDrawRange> ranges;
		};
		GpuModel g_gpuModel;

		// Every pixel shader below shades with the same expression, and every one
		// of them scales it by kShadeScale:
		//
		//     shade = kShadeScale * (0.42 + 0.48*kd + 0.16*kf + 0.06*kd*kd + spec)
		//
		// The inventory is the brightness reference, and its cap is the one
		// surface in the drink bag whose albedo is unambiguous: 236 in the
		// atlas, 186-194 on screen, i.e. ~0.80 on a lit face. This model reached
		// 1.01 there, which put every preview about a quarter too bright and
		// turned the drink-bag film's grey veil into a milky wrapper. The scale
		// is anchored to that one measurement rather than nudged by eye.
		//
		// HLSL has no shared headers here (each literal is compiled on its own),
		// so the constant is repeated per shader; test_film_material_blend.py
		// asserts all of them carry it.
		static constexpr const char kVsSrc[] = R"(
cbuffer Frame : register(b0)
{
    row_major float4x4 g_mvp;
    row_major float4x4 g_mv;
    float3 g_color;
    // Was a pad. Now carries this range's blend alpha: the blended pixel
    // shader reads it, the opaque ones ignore it and force 1.0, so a
    // per-material alpha costs no extra buffer, no extra bind and no
    // shader permutation. Previously it was hardcoded to 0.30 for every
    // translucent range.
    float g_alpha;
};
struct VSIn { float3 pos : POSITION; float2 uv : TEXCOORD0; float3 normal : NORMAL; };
struct VSOut
{
    float4 clip : SV_Position;
    float3 viewPos : TEXCOORD0;
    float3 color : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float3 normal : TEXCOORD3;
};
VSOut main(VSIn i)
{
    VSOut o;
    o.clip = mul(float4(i.pos, 1.0), g_mvp);
    float4 view = mul(float4(i.pos, 1.0), g_mv);
    o.viewPos = view.xyz;
    o.normal = mul(i.normal, (float3x3)g_mv);
    o.color = g_color;
    o.uv = i.uv;
    return o;
}
)";

		static constexpr const char kPsSrc[] = R"(
struct PSIn
{
    float4 clip : SV_Position;
    float3 viewPos : TEXCOORD0;
    float3 color : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float3 normal : TEXCOORD3;
};
float4 main(PSIn i) : SV_Target
{
    // Smooth vertex normals (area-weighted at build time). Two-sided via
    // abs() so winding variants stay lit instead of going black.
    float3 n = normalize(i.normal + 1e-9);
    float3 viewDir = normalize(-i.viewPos + 1e-9);
    float3 key = normalize(float3(0.45, 0.65, 0.62));
    float3 fill = normalize(float3(-0.45, 0.25, 0.55));
    float kd = abs(dot(n, key));
    float kf = abs(dot(n, fill));
    float3 hv = normalize(key + viewDir);
    float spec = pow(abs(dot(n, hv)), 24.0) * 0.18;
    // The textured arm below was re-measured against the inventory screen and
    // moved its floor 0.42 -> 0.62 (see the note there); this arm kept the old
    // weights, so the same albedo rendered ~1.5x darker in shadow when the
    // range had no colour map. That is not a rare case: every textureless CDB
    // set lands here, and the snow globes are almost entirely such ranges -
    // MetalBronze03, PlasticPlainTextured02Black and
    // Snowglobe_Landmark_Buildings all resolve to textureless sets - which is
    // why the 2026-10-23 captures show dark grey shells where the engine's own
    // render of the same item is bright. One rig, one anchor, both arms.
    float lit = 0.62 + 0.34 * kd + 0.10 * kf + 0.04 * kd * kd + spec;
    return float4(0.80 * (i.color * lit), 1.0);  // 0.80 = the inventory anchor
}
)";

		// Alpha-blended variant for the translucent pass. Samples the albedo
	// slot like the textured path, so it covers both cases: an untextured
	// glass range already binds slot 0, which is the white texture, so the
	// same shader renders an untextured tint and a textured one. The alpha
	// is per range, taken from the frame constant buffer's g_alpha slot
	// (see MaterialBlendAlpha for where the value comes from).
	static constexpr const char kPsBlendTexSrc[] = R"(
cbuffer Frame : register(b0)
{
    row_major float4x4 g_mvp;
    row_major float4x4 g_mv;
    float3 g_color;
    float g_alpha;
    // Same per-range tail the opaque shader reads: metalness/roughness of this
    // surface at byte 144 of the 256-byte constant.
    float2 g_metalRough;
    float2 g_metalPad;
    // 1.0 when the database names neither an albedo nor an opacity for this
    // surface: a clear shell, which transmits instead of painting a body.
    float g_clearShell;
    float3 g_clearPad;
};
Texture2D g_albedo : register(t0);
SamplerState g_samp : register(s0);
struct PSIn
{
    float4 clip : SV_Position;
    float3 viewPos : TEXCOORD0;
    float3 color : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float3 normal : TEXCOORD3;
};
float4 main(PSIn i) : SV_Target
{
    float3 albedo = g_albedo.Sample(g_samp, i.uv).rgb;
    float3 n = normalize(i.normal + 1e-9);
    float3 viewDir = normalize(-i.viewPos + 1e-9);
    float3 key = normalize(float3(0.45, 0.65, 0.62));
    float3 fill = normalize(float3(-0.45, 0.25, 0.55));
    float kd = abs(dot(n, key));
    float kf = abs(dot(n, fill));
    float3 hv = normalize(key + viewDir);
    const float metal = saturate(g_metalRough.x);
    const float rough = saturate(g_metalRough.y);
    const float gloss = 1.0 - rough;
    // Glass, in the engine, is a reflection first and a tint second: the game
    // draws these shapes as a shell you look *through*, and what the shell
    // mirrors is the room it sits in -- here the preview's own dark slate
    // backdrop plus one soft key. That is measurable: in the game's top-down
    // inventory render of the Alien Genetic Material jar (2026-10-22 11:29
    // capture) the glass wall reads 18-21/255 against a 11-26 backdrop, i.e.
    // it adds almost nothing, with a single specular arc at 245. The old
    // 0.16..0.72 gradient was an invented bright room, so every wall mirrored
    // pale grey and the jar read as a pewter canister; the lamp below gives
    // the one bright band a real studio has instead.
    // A clear shell mirrors the same measured studio as every other glass
    // surface. An earlier revision gave it a bright (0.62,0.66,0.74) room on
    // the guess that a dome at silhouette mirrors a lit space; the engine's
    // own renders of the Opportunity and NASA globes (2026-10-23 16:03 and
    // 16:05 captures) show the opposite: the dome adds essentially nothing
    // over the starfield and the sand behind it, one thin glint and no fill,
    // because the item is drawn in the same dark room as the coolant jar.
    // Mirroring a bright room there left a pale shell the reference does not
    // have - the dome still read as a milky canopy once its body was gone.
    float3 env = float3(0.055, 0.070, 0.095) +
        float3(1.55, 1.60, 1.75) * pow(saturate(dot(n, key) * 0.5 + 0.5), 24.0);
    // Two-sided, exactly like every other lighting term in this file. A closed
    // glass shell is rasterised with CULL_MODE_NONE, so its far wall presents
    // normals that point AWAY from the eye; saturating dot(n, viewDir) to zero
    // there drove Fresnel to 1 over the whole inner surface and stamped an
    // opaque room-coloured cup over the item. That -- not the tint -- is what
    // made the coolant jar, the Sparkling Wine and the Pils can read as pale
    // white sleeves in the 2026-10-21 captures: the far wall alone covered
    // 100% of the model with 0.72-0.88 grey. abs() gives the far wall the near
    // wall's answer: dim head-on, bright only where the silhouette turns.
    const float ndv = abs(dot(n, viewDir));
    const float fres = pow(1.0 - ndv, 5.0);
    // Schlick reflectance: a dielectric mirrors ~4% of the room head-on and
    // all of it at grazing. Metal keeps its own F0.
    const float f0 = lerp(0.04, 1.0, metal);
    const float F = f0 + (1.0 - f0) * fres;
    // A coloured glass tints what it reflects; a colourless one does not.
    float3 tint = lerp(float3(1.0, 1.0, 1.0), albedo * i.color, lerp(0.55, 1.0, metal));
    float3 reflection = env * tint * F * (0.90 + 0.10 * gloss);
    float spec = pow(abs(dot(n, hv)), lerp(24.0, 96.0, metal * gloss)) * lerp(0.18, 0.55, metal);
    float lit = (1.0 - metal) * (0.62 + 0.34 * kd + 0.10 * kf + 0.04 * kd * kd) + spec;
    // Colour and coverage are separate things and must not be multiplied
    // together: the body is the surface's own colour, the alpha is how much of
    // the target it covers.
    //
    // A clear shell has no body at all. The database naming no albedo and no
    // opacity is the statement that the surface is glass you look through, so
    // it contributes its reflection and nothing else; painting white at 0.62
    // there - which is what binding the white texture to the pale glass tint
    // did - is the milky film that made every snow globe's interior look like
    // frosted plastic instead of the clear dome the engine draws.
    float3 body = albedo * i.color * lit * (1.0 - F) * (1.0 - g_clearShell);
    // Coverage is Schlick's F plus the transmitted share's own opacity, and
    // the colour is split the same way: what the surface *reflects* is opaque
    // at F, what it *transmits* is body at (1 - F).
    //
    // Both halves of that split were wrong in turn. 0.8.312 gave F the whole
    // coverage and painted the body at it too, so a jar seen at a steep angle
    // -- where the entire wall is near-grazing and F saturates -- came out as
    // a pewter canister (measured 108-176/255 against the shader's predicted
    // 153). 0.8.314 kept the body out of the mirror by capping F's coverage at
    // 0.25, which fixed the pewter but left the shell painted grey over the
    // specimen from every angle: the game's own render adds ~10/255 there.
    // The transmission weight is what was missing.
    float a = saturate(F + (1.0 - F) * g_alpha);
    return float4(0.80 * (reflection + body), a);
}
)";

		// The additive pass: the jar's fluid and bubbles. A glow/gobo/caustic
		// shell is a full-body shape whose map is mostly black, so it is not a
		// surface at all - the engine adds its light to what is already there.
		// Skipped, the Alien Genetic Material jar has no fluid (it is an empty
		// glass with a specimen); drawn opaque it blacked the jar out. Blended
		// by alpha it would grey the specimen instead of lighting it. ONE/ONE
		// is what the material's own directory name says (Effects\Gobos) and
		// what the 2026-10-22 inventory capture shows: bright green specks over
		// the specimen, with the black surround adding nothing.
		static constexpr const char kPsAdditiveSrc[] = R"(
cbuffer Frame : register(b0)
{
    row_major float4x4 g_mvp;
    row_major float4x4 g_mv;
    float3 g_color;
    float g_alpha;
    float2 g_metalRough;
    float2 g_metalPad;
};
Texture2D g_albedo : register(t0);
SamplerState g_samp : register(s0);
struct PSIn
{
    float4 clip : SV_Position;
    float3 viewPos : TEXCOORD0;
    float3 color : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float3 normal : TEXCOORD3;
};
float4 main(PSIn i) : SV_Target
{
    float3 ink = g_albedo.Sample(g_samp, i.uv).rgb;
    // Blend is ONE/ONE, so this colour IS the light added. Alpha is unused.
    // 0.80 keeps the brightest caustic ribbon from clipping to pure white
    // while a black texel still adds exactly nothing.
    return float4(ink * i.color * 0.80, 1.0);
}
)";

	static constexpr const char kPsTexSrc[] = R"(
cbuffer Frame : register(b0)
{
    row_major float4x4 g_mvp;
    row_major float4x4 g_mv;
    float3 g_color;
    float g_alpha;
    // This range's surface metalness and roughness, at byte 144 of the same
    // 256-byte per-range constant. Metalness 0 means "no _metal companion",
    // and the metal term below is written to vanish exactly at 0 so every
    // material without those maps renders as it did before.
    float2 g_metalRough;
	    // g_metalPad.x is 1 when this range's detail descriptor holds a BC5
	    // normal map instead of the white texture. It was reserved padding.
	    float2 g_metalPad;
	    // Bytes 160-167 of the same per-range constant, written for every
	    // range: .x marks a clear glass shell (the blend shader's own flag),
	    // .y marks an inside-out hull that has to be drawn from the inside -
	    // every snow globe's backdrop. See the clip below.
	    float2 g_shell;
	};
Texture2D g_albedo : register(t0);
SamplerState g_samp : register(s0);
Texture2D g_detail : register(t2);
struct PSIn
{
    float4 clip : SV_Position;
    float3 viewPos : TEXCOORD0;
    float3 color : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float3 normal : TEXCOORD3;
};
float4 main(PSIn i) : SV_Target
{
    float4 tex = g_albedo.Sample(g_samp, i.uv);
    float3 albedo = tex.rgb;
    // Mode 10 on the opaque pass, where promoted cutouts actually draw: the
    // material's texture set names an opacity mask and no colour map, so the
    // bound albedo IS that mask. A BC4 SRV samples as (r,0,0), and reading that
    // as colour is what paints these bodies maroon-black. The Sparkling Wine's
    // bottle is the capture that showed it: its set is
    // `GenericSpaceWine_Details.mat` plus `genericspacewine_decal_opacity.dds`,
    // a white bottle silhouette with the black net cut out of it. The mask is
    // coverage, so the body is that coverage as white, and the pixels it marks
    // out are the net's gaps, not surface. The alpha pass has carried this arm
    // since the drink bags, but promoted cutouts are routed to pass 0, so it
    // was unreachable there and this is the pass that needs it.
    const bool promotedMode = g_alpha > 9.5 && g_alpha < 10.5;
    // Mode 11: a single-channel albedo on a solid range. It is a *picture*,
    // not coverage: the Sparkling Wine's bottle body is a greyscale sheet of
    // the bottle with the net inked onto it, white where the foil is and dark
    // where the net is. Read as coverage it could only be cut out (which
    // deleted the bottle), never shaded. Read as a greyscale body it is the
    // inventory render exactly: pale bottle, black net.
    const bool maskInkMode = g_alpha > 10.5 && g_alpha < 11.5;
    if (promotedMode)
    {
        if (tex.r < 0.5)
        {
            discard;
        }
        albedo = tex.rrr;
    }
    else if (maskInkMode)
    {
        albedo = tex.rrr;
    }
    float3 n = normalize(i.normal + 1e-9);
    float3 viewDir = normalize(-i.viewPos + 1e-9);
    // A hull is drawn from the inside. Its faces all point at the model's
    // centre (measured -0.84 on the shipped London backdrop, -0.82 .. -0.86
    // across the fifteen globes), so the near wall's normal points AWAY from
    // the eye and the far wall's points at it. Clipping by the normal itself
    // is what removes the near wall: it needs no winding convention, which
    // is not knowable from here. CULL_MODE_BACK on the pipeline was the first
    // attempt and changed nothing on screen (build 2E401C95, 19:19 session) -
    // it kept the near wall and dropped the far one, i.e. this renderer's
    // winding is the opposite of what that guess assumed.
    if (g_shell.y > 0.5)
    {
        clip(dot(n, viewDir));
    }
    float3 key = normalize(float3(0.45, 0.65, 0.62));
    float3 fill = normalize(float3(-0.45, 0.25, 0.55));
    float kd = abs(dot(n, key));
    float kf = abs(dot(n, fill));
    float3 hv = normalize(key + viewDir);
    // Authored relief. A material's BC5 `_normal` companion carries detail
    // that exists nowhere else: the Erdebrau can's pull ring is embossed into
    // it, while the can's lid is a flat 16-gon plate (110 triangles, no tab)
    // sampling a plain foil island of the decal atlas. Drawn without this the
    // lid is a bare disc, which is exactly what every capture so far shows.
    //
    // The meshes ship no tangent stream, so the frame is rebuilt per pixel
    // from the screen-space derivatives of view position and UV (Schuler's
    // cotangent frame): exact for the triangle being shaded, no vertex
    // layout change, and the same construction the wood-engraving arm below
    // already uses. Only X and Y are stored - BC5 is two channels - so Z
    // comes from the unit-length constraint, and the DDS convention (Y down
    // in texture space) is applied once, here.
    if (g_metalPad.x > 0.5)
    {
        // The shipped normal maps have no mip chain - a 1024² BC5 surface is
        // exactly 1 MB, which is what the archives hold - so the hardware
        // cannot filter them. At preview scale one texel of print then becomes
        // one pixel of shading: the salt-and-pepper speckle the 2026-10-22
        // captures show on the Pilsner lid and body, the coolant jar's lid and
        // the wine's cap. Four taps spread by this pixel's own UV footprint are
        // the mip the file does not have; the pull ring (a hundred texels
        // across) survives, the per-texel noise averages out.
        float2 footprint = max(abs(ddx(i.uv)), abs(ddy(i.uv)));
        float2 nm = g_detail.Sample(g_samp, i.uv + footprint * float2(1.0, 1.0)).xy
                  + g_detail.Sample(g_samp, i.uv + footprint * float2(-1.0, -1.0)).xy
                  + g_detail.Sample(g_samp, i.uv + footprint * float2(1.0, -1.0)).xy
                  + g_detail.Sample(g_samp, i.uv + footprint * float2(-1.0, 1.0)).xy;
        nm = nm * 0.5 - 1.0;
        float3 posDx = ddx(i.viewPos);
        float3 posDy = ddy(i.viewPos);
        float2 uvDx = ddx(i.uv);
        float2 uvDy = ddy(i.uv);
        float3 tapPerp = cross(posDy, n);
        float3 bitPerp = cross(n, posDx);
        float3 tangent = tapPerp * uvDx.x + bitPerp * uvDy.x;
        float3 bitangent = tapPerp * uvDx.y + bitPerp * uvDy.y;
        float scale = rsqrt(max(max(dot(tangent, tangent), dot(bitangent, bitangent)), 1e-12));
        // The strength is the engine's, and it is not 1.0. These maps store a
        // strong emboss: over the Pilsner decal normal's lid island the flat
        // plate sits at signed XY (-0.92, -0.63), not near (0, 0), and the
        // level itself is stippled with surface grain. Weighted at 1.0 the
        // plate tilted almost edge-on and every texel of that grain with it,
        // which is the "print" the 2026-10-22 capture shows on the can lid and
        // on the jar lid beside it. At 0.25 the pull ring still reads as
        // embossed - its edges swing the normal by ~14 degrees against the
        // plate's own ~3 - and the plate stays a plate. Perturbation is added
        // to the geometric normal and re-normalised, so strength 0 is exactly
        // the flat surface: no Z reconstruction, no branch for the flat case.
        const float strength = 0.25;
        n = normalize(tangent * (nm.x * scale * strength) - bitangent * (nm.y * scale * strength) + n);
    }
    // kd and kf were measured above, on the unperturbed normal, because this
    // arm sits after them; relief moves the normal, so re-measure here.
    kd = abs(dot(n, key));
    kf = abs(dot(n, fill));
    // A Starfield weapon is mostly metal: its albedo is a flat tint and the
    // rest of the look is a reflection of the room. Rendering the albedo alone
    // made the Vortex Grenade read pale and flat where the game reads dark
    // navy (its atlas measures metalness 0.789) and left the Razorback's
    // glossy cylinder (0.759) and receiver (0.291) matte.
    //
    // Per range, from the means of the material's own BC4 _metal/_rough maps:
    // metalness moves the albedo out of the diffuse term and into a mirror
    // term, roughness widens the highlight and dulls that mirror. The gradient
    // stands in for the environment (cool above, dark below) - this preview
    // has no reflection probe and does not need one to read as metal.
    const float metal = saturate(g_metalRough.x);
    const float rough = saturate(g_metalRough.y);
    const float gloss = 1.0 - rough;
    // The room the metal mirrors. A first pass used 0.10..0.55 and every metal
    // surface went a step darker than the albedo-only render, because a metal
    // has no diffuse to fall back on: its whole value is this reflection. The
    // inventory screen is a bright interior, so the gradient is pegged there -
    // cool daylight above, warm bounce below - and metal lands near its own
    // albedo instead of below it.
    float3 env = lerp(float3(0.16, 0.17, 0.20), float3(0.72, 0.77, 0.88), saturate(n.y * 0.5 + 0.5));
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metal);
    float fres = pow(1.0 - saturate(dot(n, viewDir)), 5.0);
    // Roughness spreads the reflection instead of deleting it: a rough metal
    // still mirrors a blurred room, so the energy is damped, never removed.
    float3 reflection = metal * f0 * env * (1.0 - 0.6 * rough) * (1.0 + 0.6 * fres);
    // Both specular terms are gated on metal, not on roughness: the exponent
    // must stay 24.0 when there is no _metal map, or the highlight would
    // narrow for every cloth, food and wood material in the catalogue.
    float spec = pow(abs(dot(n, hv)), lerp(24.0, 96.0, metal * gloss)) * lerp(0.18, 0.55, metal);
    // The diffuse rig. The floor rose from 0.42 to 0.62: measured against the
    // inventory screen (Screenshot 2026-10-18 021147, pistol) the preview's
    // midtones sat at 0.72 of the game's luma and at half its chroma, and the
    // cause is this term. kd/kf are abs(dot), so a face pointed away from both
    // lights got 0.42 of the albedo - in this gamma-space pipe that is 0.42 of
    // the *stored* value, i.e. a third of the linear energy, which crushes the
    // darks and shrinks every colour difference with them. The directional
    // weights came down by the same amount so a lit face keeps its measured
    // anchor (0.62+0.34+0.10+0.04 = 1.10 against the old 1.12).
    float lit = (1.0 - metal) * (0.62 + 0.34 * kd + 0.10 * kf + 0.04 * kd * kd) + spec;
    // 0.80 = the inventory anchor (the drink cap: 236 in the atlas, 186-194
    // on the inventory screen, at a lit face of 0.42+0.48+0.16+0.06 = 1.12).
    return float4(0.80 * (albedo * i.color * lit + reflection), 1.0);
}
)";

		// Decal variant: transparent sheets (markings, labels, card faces)
		// punch through where the texture is transparent instead of paving
		// the surface beneath with background color. Opaque textures read
		// alpha 1 everywhere, so this is a no-op for them by construction.
		static constexpr const char kPsTexAlphaSrc[] = R"(
cbuffer Frame : register(b0)
{
    row_major float4x4 g_mvp;
    row_major float4x4 g_mv;
    float3 g_color;        float g_alpha; // mask (0/1), engraving (2), Razorback badge (3), decal coverage (4/5), emissive composites (6 Crucible, 7 Vortex)
    // The same per-range tail every other shader reads. Metalness is not used
    // in this pass, but g_metalPad.x is: 1 means the detail descriptor holds
    // this material's BC5 normal map, which the relief arm below samples.
    float2 g_metalRough;
    float2 g_metalPad;
};
Texture2D g_albedo : register(t0);
Texture2D g_opacity : register(t1);
Texture2D g_detail : register(t2);
SamplerState g_samp : register(s0);
struct PSIn
{
    float4 clip : SV_Position;
    float3 viewPos : TEXCOORD0;
    float3 color : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float3 normal : TEXCOORD3;
};
float4 main(PSIn i) : SV_Target
{
    float4 tex = g_albedo.Sample(g_samp, i.uv);
    float4 maskTex = g_opacity.Sample(g_samp, i.uv);
    // BC3 is DXGI 77 and retains the mask in alpha; BC4 (DXGI 80) is red.
    // Modes 4/5 use four derivative-offset taps and source-alpha blending
    // for decal sheets. The old binary clip erased atlas print finer than a
    // preview pixel (Breach's packed warning text is the concrete example).
    // Mode 0 and BC3 coverage mode 5 read alpha; other modes read red.
    float mask = (g_alpha < 0.5 || (g_alpha > 4.5 && g_alpha < 5.5)) ? maskTex.a : maskTex.r;	    const bool coverageBlend = g_alpha > 3.5 && g_alpha < 5.5;
    // Mode 8: the crafted drink-bag film, and the drink inside it, which uses
    // the same map one band higher (0.76 against the film's 0.24) so the
    // submerged leaf prints through. The map is the bag's own coverage - soft
    // film edges, a bright seam and printed panel - so the map IS the output
    // alpha, not a cutout. The cutout this replaces kept the bright atlas band
    // and erased everything below it, which is why Alien Tea read as an opaque
    // gold shell over its own contents.
    //
    // No additive floor. An earlier revision used 0.35 + 0.65 * mask, which
    // kept the pouch 35-100% opaque and turned the bag into a grey-white
    // wrapper around a washed-out drink. The inventory reference measures the
    // opposite: over its own background the film lifts the pixel by only
    // +3..+9 of 255 where the map is dark, and the drink seen through it stays
    // a saturated dark red (65,33,34 against a (11,25,33) background) while
    // the printed creature stays bright red. With the film's own atlas region
    // at a mid grey (175,175,178 at this mesh's UV footprint, in both the tea
    // and the tonic atlas) a floor of 0.35 puts ~60 of grey over everything;
    // the coverage value alone puts +14 of grey over the drink and +38 over
    // the background before this preview's own lighting, against the
    // reference's +3..+9 in its dimmer inventory light. The map's own mean
    // under this footprint is a flat 0.23, and it is 0.98 under the cap's and
    // 0.81 under the drink's - i.e. it really is per-part coverage, so it
    // needs no help from a constant.
    //
    // Drawn once, not twice. The pouch is a closed bag - measured from its own
    // geometry, 314 front triangles against 342 back triangles of equal area
    // (0.3905 / 0.3892) - and this renderer rasters with CULL_MODE_NONE, so
    // both sheets cover the same pixels everywhere outside the drink.
    // Compositing the map twice gives 1-(1-mask)^2, i.e. 0.41 where the map
    // says 0.23, which measured ~40 of grey over the background against the
    // reference's +10. Inverting that composite for two identical layers,
    // a = 1-sqrt(1-mask), lands the two draws on the map's own coverage
    // instead (0.12 each) with no second PSO and no winding assumption.
    const bool filmBlend = g_alpha > 7.5 && g_alpha < 8.5;

    const bool crucibleMode = g_alpha > 5.5 && g_alpha < 6.5;
    const bool vortexMode = g_alpha > 6.5 && g_alpha < 7.5;
    // Mode 10: a promoted cutout - the material's texture set names an opacity
    // mask and no colour map, so the bound albedo IS that mask. An R8/BC4 SRV
    // samples as (r,0,0), and reading that as colour painted the Sparkling
    // Wine's shell maroon-black the moment the glass around it stopped hiding
    // it. The mask is coverage, so the body is the same coverage as white;
    // the cutout below still uses the same channel.
    const bool promotedMode = g_alpha > 9.5 && g_alpha < 10.5;
    // Mode 9: a solid part that happens to have a coverage map bound. The
    // crafted drink bags pair one map with every flavour material, but only
    // the bag and the drink are films - the printed leaf is solid artwork and
    // the map clips 53% of its pixels away, leaving a broken creature.
    const bool noClipMode = g_alpha > 8.5 && g_alpha < 9.5;
    // Mode 11: a single-channel albedo on a solid range - a greyscale sheet,
    // not coverage. Shaded as the picture it is; never clipped. See the
    // opaque shader's copy for the measured case.
    const bool maskInkMode = g_alpha > 10.5 && g_alpha < 11.5;
    float outputAlpha = 1.0;
    float3 albedo = tex.rgb;
    if (promotedMode)
    {
        albedo = tex.rrr;
    }
    else if (maskInkMode)
    {
        albedo = tex.rrr;
    }
    else if (coverageBlend)
    {
        float2 dx = ddx(i.uv) * 0.25;
        float2 dy = ddy(i.uv) * 0.25;
        float coverage = 0.0;
        float3 premultiplied = float3(0.0, 0.0, 0.0);
        [unroll]
        for (int y = -1; y <= 1; y += 2)
        {
            [unroll]
            for (int x = -1; x <= 1; x += 2)
            {
                float2 sampleUv = i.uv + dx * x + dy * y;
                float4 sampleTex = g_albedo.Sample(g_samp, sampleUv);
                float4 sampleMaskTex = g_opacity.Sample(g_samp, sampleUv);
                // Coverage modes encode the mask channel in their mode: 4=BC4 red, 5=BC3 alpha.
                float sampleMask = g_alpha > 4.5 ? sampleMaskTex.a : sampleMaskTex.r;
                float sampleCoverage = saturate(min(sampleTex.a, sampleMask));
                coverage += sampleCoverage * 0.25;
                premultiplied += sampleTex.rgb * (sampleCoverage * 0.25);
            }
        }
        outputAlpha = coverage;
        albedo = coverage > 1e-5 ? premultiplied / coverage : float3(0.0, 0.0, 0.0);
    }
    else if (filmBlend)
    {
        outputAlpha = 1.0 - sqrt(1.0 - mask);
    }
    else if (!crucibleMode && !vortexMode && !noClipMode && !maskInkMode)
    {
        // Crucible's energy opacity and Vortex Grenade's emissive atlas are
        // not surface-coverage maps. Do not cut away their authored color.
        // Neither is a drink bag's printed leaf, hence the mode-9 exclusion.
        //
        // A promoted cutout cuts on its OWN map. Its companion opacity map,
        // where the set names one, belongs to another layer: the Sparkling
        // Wine's body is a white bottle silhouette paired with the net's mask,
        // so clipping on the companion cut the silhouette down to the net's
        // strands and left nothing of the bottle - the 0.8.311 session drew it
        // that way at 10:15. The icons that made this arm work in the first
        // place bind the same map as colour and opacity, so for them the two
        // readings agree and nothing changes.
        clip((promotedMode ? tex.r : min(tex.a, mask)) - 0.5);
    }
    float engravedLines = 0.0f;
    float3 detail = float3(0.0, 0.0, 0.0);
    if (g_alpha > 1.5 && g_alpha < 3.5)
    {
        // The standard atlas supplies the wood face and circular coverage;
        // Fancy color supplies the authored horse badge or engraving detail.
        detail = g_detail.Sample(g_samp, i.uv).rgb;
        if (g_alpha > 2.5)
        {
            // The badge has dark horse linework on a light field. Estimate
            // the local paper level in screen space so minified lines survive.
            float detailLuma = dot(detail, float3(0.2126, 0.7152, 0.0722));
            float2 uvDx = ddx(i.uv);
            float2 uvDy = ddy(i.uv);

            float neighborhood = 0.0;
            float2 texelOffsets[8] = {
                float2(-2.0, -2.0), float2(0.0, -2.0), float2(2.0, -2.0),
                float2(-2.0,  0.0),                         float2(2.0,  0.0),
                float2(-2.0,  2.0), float2(0.0,  2.0), float2(2.0,  2.0)
            };
            [unroll]
            for (int sampleIndex = 0; sampleIndex < 8; ++sampleIndex)
            {
                float2 sampleUv = i.uv + uvDx * texelOffsets[sampleIndex].x + uvDy * texelOffsets[sampleIndex].y;
                float sampleLuma = dot(g_detail.Sample(g_samp, sampleUv).rgb, float3(0.2126, 0.7152, 0.0722));
                neighborhood += sampleLuma;
            }
            neighborhood *= 0.125;
            float badgeInk = saturate((neighborhood - detailLuma - 0.01) * 24.0);
            albedo *= 1.0 - badgeInk * 0.70;
        }
        else
        {
            // Engraving mode: retain wood, sampling Fancy as a grayscale
            // relief/ink hint.
            float detailLuma = dot(detail, float3(0.2126, 0.7152, 0.0722));
            engravedLines = saturate((0.735 - detailLuma) * 16.0);
            albedo *= 1.0 - engravedLines * 0.70;
        }
    }
    else if (crucibleMode)
    {
        // The authored emissive atlas and energy mask identify the blade's
        // hot regions. Move a soft, flickering flame band along the blade's
        // UV length; the per-draw time arrives in g_color.r. Preserve the
        // complete red albedo (the energy mask is not a cutout).
        detail = g_detail.Sample(g_samp, i.uv).rgb;
        float detailLuma = dot(detail, float3(0.2126, 0.7152, 0.0722));
        float glowSignal = saturate((max(mask, detailLuma * 1.4) - 0.08) * 1.6);
        float flowPhase = frac(i.uv.y * 1.5 + i.color.r * 0.14);
        float flameBand = smoothstep(0.0, 0.08, flowPhase) *
            (1.0 - smoothstep(0.08, 0.38, flowPhase));
        float flicker = 0.85 + 0.15 * sin(i.color.r * 17.0 + i.uv.y * 57.0);
        float glow = glowSignal * (0.22 + 0.95 * flameBand) * flicker;
        albedo += detail * (0.25 + flameBand * 0.45);
        albedo += float3(1.35, 0.10, 0.012) * glow;
    }
    else if (vortexMode)
    {
        // The Vortex Grenade uses an emissive color atlas rather than an
        // opacity cutout. Preserve the complete base-color atlas and add a
        // restrained, static emission contribution in the authored glowing
        // UV regions. The emissive map is paired as detail texture by the
        // task-thread prefetch; do not animate the thrown-item preview.
        detail = g_detail.Sample(g_samp, i.uv).rgb;
        float emissiveLuma = dot(detail, float3(0.2126, 0.7152, 0.0722));
        float glow = saturate((emissiveLuma - 0.012) * 1.7);
        albedo += detail * (0.45 + glow * 0.65);
    }
    float3 n = normalize(i.normal + 1e-9);

    if (g_alpha > 1.5 && g_alpha < 2.5)
    {
        // Treat the dark atlas linework as a shallow height field below the
        // wood surface, then perturb the lighting normal in the UV tangent
        // frame. This creates a recessed edge highlight/shadow instead of a
        // flat dark print. Recover the UV frame from screen-space derivatives
        // so no model tangent stream is needed.
        float2 uvDx = ddx(i.uv);
        float2 uvDy = ddy(i.uv);
        float det = uvDx.x * uvDy.y - uvDx.y * uvDy.x;
        if (abs(det) > 1e-8)
        {
            float3 posDx = ddx(i.viewPos);
            float3 posDy = ddy(i.viewPos);
            float3 tangent = (posDx * uvDy.y - posDy * uvDx.y) / det;
            float3 bitangent = (posDy * uvDx.x - posDx * uvDy.x) / det;
            float lineDx = ddx(engravedLines);
            float lineDy = ddy(engravedLines);
            float lineDu = (lineDx * uvDy.y - lineDy * uvDx.y) / det;
            float lineDv = (uvDx.x * lineDy - uvDy.x * lineDx) / det;
            // The extracted line mask is the inverse of surface height: dark
            // detail sits below the surrounding wood. Scale the height to a
            // subtle but legible carved relief in the normalized model space.
            float tangentLength = max(length(tangent), 1e-5);
            float bitangentLength = max(length(bitangent), 1e-5);
            // Scale the sampled engraving into a visible but shallow cut;
            // sub-millimeter relief was lost under the preview's soft lighting.
            float3 reliefNormal = n + 0.006 * (
                normalize(tangent) * (lineDu / tangentLength) +
                normalize(bitangent) * (lineDv / bitangentLength));
            n = normalize(reliefNormal);
        }
    }
    // Authored relief, the same arm as the opaque shader's: this range's
    // material has a BC5 `_normal` companion, and the detail descriptor holds
    // it. The Erdebrau can reaches this line - its lid is a flat plate whose
    // pull ring lives only in that map.
    if (g_metalPad.x > 0.5)
    {
        // The shipped normal maps have no mip chain - a 1024² BC5 surface is
        // exactly 1 MB, which is what the archives hold - so the hardware
        // cannot filter them. At preview scale one texel of print then becomes
        // one pixel of shading: the salt-and-pepper speckle the 2026-10-22
        // captures show on the Pilsner lid and body, the coolant jar's lid and
        // the wine's cap. Four taps spread by this pixel's own UV footprint are
        // the mip the file does not have; the pull ring (a hundred texels
        // across) survives, the per-texel noise averages out.
        float2 footprint = max(abs(ddx(i.uv)), abs(ddy(i.uv)));
        float2 nm = g_detail.Sample(g_samp, i.uv + footprint * float2(1.0, 1.0)).xy
                  + g_detail.Sample(g_samp, i.uv + footprint * float2(-1.0, -1.0)).xy
                  + g_detail.Sample(g_samp, i.uv + footprint * float2(1.0, -1.0)).xy
                  + g_detail.Sample(g_samp, i.uv + footprint * float2(-1.0, 1.0)).xy;
        nm = nm * 0.5 - 1.0;
        float3 posDx = ddx(i.viewPos);
        float3 posDy = ddy(i.viewPos);
        float2 uvDx = ddx(i.uv);
        float2 uvDy = ddy(i.uv);
        float3 tapPerp = cross(posDy, n);
        float3 bitPerp = cross(n, posDx);
        float3 tangent = tapPerp * uvDx.x + bitPerp * uvDy.x;
        float3 bitangent = tapPerp * uvDx.y + bitPerp * uvDy.y;
        float scale = rsqrt(max(max(dot(tangent, tangent), dot(bitangent, bitangent)), 1e-12));
        // Same strength as the opaque arm - see there for the measurement. The
        // two passes must agree or a translucent lid and an opaque one of the
        // same material would carry different relief.
        const float strength = 0.25;
        n = normalize(tangent * (nm.x * scale * strength) - bitangent * (nm.y * scale * strength) + n);
    }
    float3 viewDir = normalize(-i.viewPos + 1e-9);
    float3 key = normalize(float3(0.45, 0.65, 0.62));
    float3 fill = normalize(float3(-0.45, 0.25, 0.55));
    // Ordinary materials retain two-sided shading; grip relief gets directional
    // diffuse so its carved walls can fall into shadow.
    float reliefFace = dot(i.normal, key) < 0.0 ? -1.0 : 1.0;
    float kd = g_alpha > 1.5 && g_alpha < 2.5 ? saturate(dot(n, key) * reliefFace) : abs(dot(n, key));
    float kf = g_alpha > 1.5 && g_alpha < 2.5 ? saturate(dot(n, fill) * reliefFace) : abs(dot(n, fill));
    float3 hv = normalize(key + viewDir);
    float spec = pow(abs(dot(n, hv)), 24.0) * 0.18;
    float shade = 0.80 * (0.42 + 0.48 * kd + 0.16 * kf + 0.06 * kd * kd + spec);  // 0.80 = the inventory anchor
    float3 tint = (crucibleMode || vortexMode) ? float3(1.0, 1.0, 1.0) : i.color;
    return float4(albedo * tint * shade, outputAlpha);
}
)";

		void ReleaseGpuPipe()
		{
			if (g_pipe.cbMapped && g_pipe.cb) {
				g_pipe.cb->Unmap(0, nullptr);
				g_pipe.cbMapped = nullptr;
			}
#define REL(x) \
	if (x) { \
		(x)->Release(); \
		(x) = nullptr; \
	}
			REL(g_pipe.whiteStage);
			REL(g_pipe.whiteTex);
			REL(g_pipe.cb);
			REL(g_pipe.depth);
			REL(g_pipe.srvHeap);
			REL(g_pipe.dsvHeap);
			REL(g_pipe.rtvHeap);
			REL(g_pipe.psoDecalCoverage);
			REL(g_pipe.psoTexturedAlpha);
			REL(g_pipe.psoBlend);
			REL(g_pipe.psoAdditive);
			REL(g_pipe.psoTextured);
			REL(g_pipe.pso);
			REL(g_pipe.rootSig);
#undef REL
			g_pipe.ready = false;
			g_pipe.version = 0;
			g_pipe.width = g_pipe.height = 0;
		}

		void ReleaseGpuModel()
		{
			if (g_gpuModel.vb) {
				g_gpuModel.vb->Release();
				g_gpuModel.vb = nullptr;
			}
			if (g_gpuModel.ib) {
				g_gpuModel.ib->Release();
				g_gpuModel.ib = nullptr;
			}
			for (auto* t : g_gpuModel.tex) {
				if (t) {
					t->Release();
				}
			}
			for (auto* t : g_gpuModel.texStage) {
				if (t) {
					t->Release();
				}
			}
			g_gpuModel.tex.clear();
			g_gpuModel.texStage.clear();
			g_gpuModel.texturesReady = false;
			g_gpuModel.key.clear();
			g_gpuModel.ranges.clear();
			g_gpuModel.vbBytes = 0;
			g_gpuModel.ibCount = 0;
		}

		bool CompileShader(const char* a_src, const char* a_target, ::ID3DBlob** a_out)
		{
			::ID3DBlob* code = nullptr;
			::ID3DBlob* errors = nullptr;
			const HRESULT hr = D3DCompile(a_src, std::strlen(a_src), nullptr, nullptr, nullptr,
				"main", a_target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
			if (FAILED(hr)) {
				const char* msg = errors && errors->GetBufferPointer() ?
					static_cast<const char*>(errors->GetBufferPointer()) : "unknown";
				REX::ERROR("SFShopExplorer offline GPU: {} compile failed: {}", a_target, msg);
				if (errors) {
					errors->Release();
				}
				return false;
			}
			if (errors) {
				errors->Release();
			}
			*a_out = code;
			return true;
		}

		bool EnsureGpuPipe()
		{
			auto* device = g_gpu.device;
			if (!device) {
				REX::WARN("SFShopExplorer offline GPU: pipe stage nodevice");
				return false;
			}
			if (g_pipe.ready && g_pipe.version == GpuPipe::kVersion && g_pipe.width == g_width && g_pipe.height == g_height) {
				return true;
			}
			ReleaseGpuPipe();
			::ID3DBlob *vs = nullptr, *ps = nullptr, *pstex = nullptr, *pstexalpha = nullptr, *psblend = nullptr, *psadditive = nullptr;
			bool ok = CompileShader(kVsSrc, "vs_5_0", &vs) && CompileShader(kPsSrc, "ps_5_0", &ps) &&
				CompileShader(kPsTexSrc, "ps_5_0", &pstex) && CompileShader(kPsTexAlphaSrc, "ps_5_0", &pstexalpha) &&
				CompileShader(kPsBlendTexSrc, "ps_5_0", &psblend) &&
				CompileShader(kPsAdditiveSrc, "ps_5_0", &psadditive);
			if (ok) {
				D3D12_ROOT_PARAMETER params[4]{};
				params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
				params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
				params[0].Descriptor.ShaderRegister = 0;
				params[0].Descriptor.RegisterSpace = 0;
				D3D12_DESCRIPTOR_RANGE srange{};
				srange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
				srange.NumDescriptors = 1;
				srange.BaseShaderRegister = 0;
				srange.RegisterSpace = 0;
				srange.OffsetInDescriptorsFromTableStart = 0;
				params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
				params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
				params[1].DescriptorTable.NumDescriptorRanges = 1;
				params[1].DescriptorTable.pDescriptorRanges = &srange;
				D3D12_DESCRIPTOR_RANGE opacityRange{};
				opacityRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
				opacityRange.NumDescriptors = 1;
				opacityRange.BaseShaderRegister = 1;
				opacityRange.RegisterSpace = 0;
				opacityRange.OffsetInDescriptorsFromTableStart = 0;
				params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
				params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
				params[2].DescriptorTable.NumDescriptorRanges = 1;
				params[2].DescriptorTable.pDescriptorRanges = &opacityRange;
				D3D12_DESCRIPTOR_RANGE detailRange{};
				detailRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
				detailRange.NumDescriptors = 1;
				detailRange.BaseShaderRegister = 2;
				detailRange.RegisterSpace = 0;
				detailRange.OffsetInDescriptorsFromTableStart = 0;
				params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
				params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
				params[3].DescriptorTable.NumDescriptorRanges = 1;
				params[3].DescriptorTable.pDescriptorRanges = &detailRange;
				D3D12_STATIC_SAMPLER_DESC samp{};
				samp.Filter = D3D12_FILTER_ANISOTROPIC;
				samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
				samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
				samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
				samp.MaxAnisotropy = 8;
				samp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
				samp.MaxLOD = D3D12_FLOAT32_MAX;
				samp.ShaderRegister = 0;
				samp.RegisterSpace = 0;
				samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
				D3D12_ROOT_SIGNATURE_DESC rsDesc{};
				rsDesc.NumParameters = 4;
				rsDesc.pParameters = params;
				rsDesc.NumStaticSamplers = 1;
				rsDesc.pStaticSamplers = &samp;
				rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
				::ID3DBlob* rsBlob = nullptr;
				::ID3DBlob* rsErr = nullptr;
				if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsErr))) {
					REX::ERROR("SFShopExplorer offline GPU: root signature serialize failed");
					ok = false;
				} else if (FAILED(device->CreateRootSignature(0, rsBlob->GetBufferPointer(),
								rsBlob->GetBufferSize(), IID_PPV_ARGS(&g_pipe.rootSig)))) {
					REX::ERROR("SFShopExplorer offline GPU: root signature create failed");
					ok = false;
				}
				if (rsBlob) {
					rsBlob->Release();
				}
				if (rsErr) {
					rsErr->Release();
				}
			}
			if (ok) {
				D3D12_INPUT_ELEMENT_DESC layout[3]{};
				layout[0].SemanticName = "POSITION";
				layout[0].Format = DXGI_FORMAT_R32G32B32_FLOAT;
				layout[0].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
				layout[1].SemanticName = "TEXCOORD";
				layout[1].SemanticIndex = 0;
				layout[1].Format = DXGI_FORMAT_R32G32_FLOAT;
				layout[1].AlignedByteOffset = 12;
				layout[1].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
				layout[2].SemanticName = "NORMAL";
				layout[2].SemanticIndex = 0;
				layout[2].Format = DXGI_FORMAT_R32G32B32_FLOAT;
				layout[2].AlignedByteOffset = 20;
				layout[2].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
				D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
				pso.pRootSignature = g_pipe.rootSig;
				pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
				pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
				pso.InputLayout = { layout, 3 };
				pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
				pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
				pso.RasterizerState.DepthClipEnable = TRUE;
				pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
				pso.DepthStencilState.DepthEnable = TRUE;
				pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
				pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
				pso.SampleMask = UINT_MAX;
				pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
				pso.NumRenderTargets = 1;
				pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
				pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
				pso.SampleDesc.Count = 1;
				if (FAILED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_pipe.pso)))) {
					REX::ERROR("SFShopExplorer offline GPU: PSO create failed");
					ok = false;
				}
				pso.PS = { pstex->GetBufferPointer(), pstex->GetBufferSize() };
				if (ok && FAILED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_pipe.psoTextured)))) {
					REX::ERROR("SFShopExplorer offline GPU: textured PSO create failed");
					ok = false;
				}
				// No culling pipeline: an inside-out hull (a snow globe's backdrop)
				// loses its near wall through the clip in kPsTexSrc. CULL_MODE_BACK
				// was the first attempt and kept the wrong wall (build 2E401C95,
				// 19:19 session: no visible change), so winding is not used here at
				// all and every pipeline stays CULL_MODE_NONE.
				pso.PS = { pstexalpha->GetBufferPointer(), pstexalpha->GetBufferSize() };
				if (ok && FAILED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_pipe.psoTexturedAlpha)))) {
					REX::ERROR("SFShopExplorer offline GPU: decal PSO create failed");
					ok = false;
				}
				// Minified printed decals need fractional subpixel coverage, not
				// a binary clip. Keep depth testing, blend coverage over the weapon,
				// and do not let the decal plane occlude later decal islands.
				pso.BlendState.RenderTarget[0].BlendEnable = TRUE;
				pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
				pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
				pso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
				pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
				pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
				pso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
				pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
				pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;  // decal sheets are coplanar with the opaque base they print on
				if (ok && FAILED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_pipe.psoDecalCoverage)))) {
					REX::ERROR("SFShopExplorer offline GPU: decal coverage PSO create failed");
					ok = false;
				}
				// Translucent pass: standard src-alpha blending and no depth
				// write, so a glass shell shows whatever sits behind it and
				// does not occlude later translucent surfaces.
				pso.PS = { psblend->GetBufferPointer(), psblend->GetBufferSize() };
				pso.BlendState.RenderTarget[0].BlendEnable = TRUE;
				pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
				pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
				pso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
				pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
				pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
				pso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
				pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
				pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
				pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;  // translucent glass keeps strict depth
				if (ok && FAILED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_pipe.psoBlend)))) {
					REX::ERROR("SFShopExplorer offline GPU: translucent PSO create failed");
					ok = false;
				}
				// Additive pass: the effect shells. ONE/ONE adds the sampled
				// colour to the target, so the shell's mostly-black map leaves
				// the specimen exactly as it was and only its bright caustic
				// ribbons light it up. No depth write, so the far wall of the
				// shell adds too, exactly like the engine's fluid volume.
				pso.PS = { psadditive->GetBufferPointer(), psadditive->GetBufferSize() };
				pso.BlendState.RenderTarget[0].BlendEnable = TRUE;
				pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
				pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
				pso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
				pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
				pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
				pso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
				pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
				pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
				pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
				if (ok && FAILED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_pipe.psoAdditive)))) {
					REX::ERROR("SFShopExplorer offline GPU: additive PSO create failed");
					ok = false;
				}
				// Restore the opaque defaults so anything created from this
				// descriptor later (there is nothing today) is not blended.
				pso.BlendState.RenderTarget[0].BlendEnable = FALSE;
				pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
				pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
				pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
				pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
				pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
			}
			if (vs) {
				vs->Release();
			}
			if (ps) {
				ps->Release();
			}
			if (pstex) {
				pstex->Release();
			}
			if (pstexalpha) {
				pstexalpha->Release();
			}
			if (psblend) {
				psblend->Release();
			}
			if (psadditive) {
				psadditive->Release();
			}
			if (ok) {
				D3D12_DESCRIPTOR_HEAP_DESC heap{};
				heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
				heap.NumDescriptors = 1;
				if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_pipe.rtvHeap)))) {
					REX::WARN("SFShopExplorer offline GPU: pipe stage rtvheap");
					ok = false;
				}
				heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
				if (ok && FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_pipe.dsvHeap)))) {
					REX::WARN("SFShopExplorer offline GPU: pipe stage dsvheap");
					ok = false;
				}
			}
			if (ok) {
				D3D12_RESOURCE_DESC dd{};
				dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
				dd.Width = g_width;
				dd.Height = g_height;
				dd.DepthOrArraySize = 1;
				dd.MipLevels = 1;
				dd.Format = DXGI_FORMAT_D32_FLOAT;
				dd.SampleDesc.Count = 1;
				dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
				D3D12_HEAP_PROPERTIES heap{};
				heap.Type = D3D12_HEAP_TYPE_DEFAULT;
				D3D12_CLEAR_VALUE clear{};
				clear.Format = DXGI_FORMAT_D32_FLOAT;
				clear.DepthStencil.Depth = 1.0f;
				if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &dd,
						D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(&g_pipe.depth)))) {
					REX::ERROR("SFShopExplorer offline GPU: depth create failed");
					ok = false;
				} else {
					device->CreateDepthStencilView(g_pipe.depth, nullptr,
						g_pipe.dsvHeap->GetCPUDescriptorHandleForHeapStart());
				}
			}
			if (ok) {
				// One constant-buffer slot per possible draw range, persistently mapped.
				D3D12_RESOURCE_DESC cd{};
				cd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				cd.Width = static_cast<std::uint64_t>(kMaxGpuRanges) * 256;
				cd.Height = 1;
				cd.DepthOrArraySize = 1;
				cd.MipLevels = 1;
				cd.Format = DXGI_FORMAT_UNKNOWN;
				cd.SampleDesc.Count = 1;
				cd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
				D3D12_HEAP_PROPERTIES heap{};
				heap.Type = D3D12_HEAP_TYPE_UPLOAD;
				if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &cd,
						D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_pipe.cb)))) {
					REX::ERROR("SFShopExplorer offline GPU: constant buffer create failed");
					ok = false;
				} else {
					D3D12_RANGE noRead{ 0, 0 };
					if (FAILED(g_pipe.cb->Map(0, &noRead, reinterpret_cast<void**>(&g_pipe.cbMapped)))) {
						g_pipe.cbMapped = nullptr;
						ok = false;
					}
				}
			}
			if (!ok) {
				ReleaseGpuPipe();
				return false;
			}
			// Own shader-visible SRV table: slot 0 = white, followed by
			// kMaxGpuTextures albedos, opacity masks, and optional detail maps.
			g_pipe.srvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			{
				D3D12_DESCRIPTOR_HEAP_DESC sheap{};
				sheap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
				sheap.NumDescriptors = 1 + 3 * kMaxGpuTextures;
				sheap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
				if (FAILED(device->CreateDescriptorHeap(&sheap, IID_PPV_ARGS(&g_pipe.srvHeap)))) {
					REX::WARN("SFShopExplorer offline GPU: pipe stage srvheap");
					ReleaseGpuPipe();
					return false;
				}
			}
			g_pipe.width = g_width;
			g_pipe.height = g_height;
			g_pipe.version = GpuPipe::kVersion;
			g_pipe.ready = true;
			REX::INFO("SFShopExplorer offline GPU: pipeline ready ({}x{} v{})", g_width, g_height, GpuPipe::kVersion);
			return true;
		}

		// Row-major helpers (row-vector convention, HLSL mul(v, M)).
		void Mat4Mul(const float a[16], const float b[16], float r[16])
		{
			for (int i = 0; i < 4; ++i) {
				for (int j = 0; j < 4; ++j) {
					r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] +
						a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
				}
			}
		}

		// Camera distance at which the whole bounding box stays inside the
		// viewport at the active orbit. Sphere framing (radius*5/zoom) puts the
		// silhouette of a sphere-bounded item at 0.99 of the frame half-extent at
		// the default zoom 2.05 (d = 2.44r), and a compact but deep item - the
		// Mark I backpack, whose front face is much nearer the camera than its
		// centre - then filled the panel and clipped at the border. Fitting the
		// projected box corners with a 10% margin keeps the item whole at any
		// orbit, and the result never drops below a_fallback, so nothing is
		// framed tighter than the old rule.
		float FitCamDist(const Model& a_model, float a_yaw, float a_pitch, bool a_weapon, float a_fallback,
			float a_scale = 1.0f)
		{
			constexpr float kFov = 45.0f * 3.14159265f / 180.0f;
			// Fit to the *visible* part of the viewport, not the render target.
			// The viewport paints two text lines over its top edge and one caption
			// line over its bottom edge, so a 0.90 fit still put the bottom of a
			// deep pack (Mark I) behind the caption strip and read as clipping.
			// 0.80 leaves ~10% of the height clear at each edge for that chrome.
			constexpr float kMargin = 0.80f;
			const float tanHalf = std::tan(kFov * 0.5f);
			const float aspect = static_cast<float>(g_width) /
				static_cast<float>(std::max<std::uint32_t>(g_height, 1u));
			const bool layFlat = !a_weapon && a_model.extents[1] > 1.8f * a_model.extents[0] &&
				a_model.extents[1] > 1.8f * a_model.extents[2];
			const float sy = std::sin(a_yaw), cy = std::cos(a_yaw);
			const float sp = std::sin(a_pitch), cp = std::cos(a_pitch);
			float dist = a_fallback;
			for (int corner = 0; corner < 8; ++corner) {
				float lx = ((corner & 1) ? 0.5f : -0.5f) * a_model.extents[0] * a_scale;
				float ly = ((corner & 2) ? 0.5f : -0.5f) * a_model.extents[1] * a_scale;
				float lz = ((corner & 4) ? 0.5f : -0.5f) * a_model.extents[2] * a_scale;
				if (a_weapon) {
					const float tx = -ly, ty = lz, tz = -lx;
					lx = tx;
					ly = ty;
					lz = tz;
				} else if (layFlat) {
					const float t = lx;
					lx = ly;
					ly = -t;
				}
				float p[3];
				Rotated(lx, ly, lz, sy, cy, sp, cp, p);
				// CPU and GPU cameras both measure depth as (camDist - z).
				dist = std::max(dist, p[2] + std::abs(p[0]) / (kMargin * tanHalf * aspect));
				dist = std::max(dist, p[2] + std::abs(p[1]) / (kMargin * tanHalf));
			}
			return dist;
		}

		void BuildMvp(float a_outMvp[16], float a_outMv[16], const Model& a_model, bool a_weapon)
		{
		const float baseYaw = a_weapon ? g_weaponYaw : g_yawDeg;
		const float basePitch = a_weapon ? g_weaponPitch : g_pitchDeg;
		const float yaw = (baseYaw + g_yawOff.load(std::memory_order_relaxed)) * 3.14159265f / 180.0f;
		const float pitch = std::clamp(basePitch + g_pitchOff.load(std::memory_order_relaxed), -85.0f, 85.0f) *
			3.14159265f / 180.0f;
			const float cy = std::cos(yaw), sy = std::sin(yaw);
			const float cp = std::cos(pitch), sp = std::sin(pitch);
			// Myaw then Mpitch (matches CPU Rotated()).
			const float myaw[16]{ cy, 0, -sy, 0, 0, 1, 0, 0, sy, 0, cy, 0, 0, 0, 0, 1 };
			const float mpitch[16]{ 1, 0, 0, 0, 0, cp, sp, 0, 0, -sp, cp, 0, 0, 0, 0, 1 };
			float m[16];
			Mat4Mul(myaw, mpitch, m);
			// Model is pre-normalized to r=1, so the fit distance is measured in
			// those units too. The wheel still zooms the fitted framing.
			const float zoomMul = std::clamp(g_zoomMul.load(std::memory_order_relaxed), 0.35f, 4.0f);
			const float camDist = FitCamDist(a_model, yaw, pitch, a_weapon, 5.0f / g_zoom,
				1.0f / (a_model.radius > 1e-6f ? a_model.radius : 1.0f)) / zoomMul;
			// D3D view space looks down +Z (positive w): mirror the CPU's
			// -Z-forward depth so the model sits at +camDist.
			const float view[16]{ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, camDist, 1 };
			Mat4Mul(m, view, a_outMv);
			const float fov = 45.0f * 3.14159265f / 180.0f;
			const float f = 1.0f / std::tan(fov * 0.5f);
			const float aspect = static_cast<float>(g_width) / static_cast<float>(g_height);
			const float zn = 0.1f, zf = 100.0f;
			const float proj[16]{ f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, zf / (zf - zn), 1,
				0, 0, -zn * zf / (zf - zn), 0 };
			Mat4Mul(a_outMv, proj, a_outMvp);
		}

		// A tiny mesh is still real geometry when it sprawls (soil discs,
		// sights, muzzles): skip only compact tiny meshes (attachment
		// points, effect quads) whose diagonal is a fraction of the model.
		bool MeshSpread(const Mesh& a_mesh, float a_radius)
		{
			const std::size_t n = a_mesh.pos.size() / 3;
			if (n == 0 || !(a_radius > 1e-6f)) {
				return false;
			}
			float lo[3]{ 1e30f, 1e30f, 1e30f }, hi[3]{ -1e30f, -1e30f, -1e30f };
			for (std::size_t i = 0; i < n; ++i) {
				for (int a = 0; a < 3; ++a) {
					const float v = a_mesh.pos[i * 3 + a];
					lo[a] = std::min(lo[a], v);
					hi[a] = std::max(hi[a], v);
				}
			}
			const float dx = hi[0] - lo[0], dy = hi[1] - lo[1], dz = hi[2] - lo[2];
			return std::sqrt(dx * dx + dy * dy + dz * dz) >= 0.3f * a_radius;
		}

		// Additive effect shells are not surfaces. A glow, gobo or water-caustic
		// overlay is a full-body shape whose texture is mostly black: drawn as an
		// opaque surface it paints the item black and buries whatever the shell
		// encloses. The offline contact sheet has always skipped these
		// (EFFECT_TOKENS in tools/renderer_recon/contact_sheet.py); the renderer
		// did not, and the Alien Genetic Material's WaterCaustic01 shell covered
		// the specimen with its own smoke - the 2026-10-21 capture where the jar
		// is a black bottle with white wisps inside.
		//
		// The animated caustic/gobo loop is the exception, and it is the reason
		// this predicate is now split: that shell is the jar's fluid, the engine
		// draws it additively (its light on top of what is already there), and
		// skipping it left the Alien Genetic Material with no fluid at all while
		// the game's own render of the same jar is full of bubbling green. See
		// IsAdditiveShellMaterial and the kPsAdditiveSrc pass.
		bool IsEffectOverlayMaterial(const std::string& a_matPath)
		{
			std::string ml = LowerAscii(a_matPath);
			std::replace(ml.begin(), ml.end(), static_cast<char>(92), '/');
			static constexpr const char* kEffectTokens[] = {
				"caustic", "gobo", "idleanimloop", "animloop", nullptr
			};
			for (const char* const* t = kEffectTokens; *t != nullptr; ++t) {
				if (ml.find(*t) != std::string::npos) {
					return true;
				}
			}
			// The jar's own glow shell. Named separately rather than folding
			// "glow" into the list above: a weapon's emissive artwork lives in
			// materials with that word too, and those are handled by their own
			// authored paths.
			return ml.find("items/legendarycatalyst/legendarycatalyst_glow") != std::string::npos;
		}

		// The shells that are drawn rather than skipped. Only the animated
		// caustic/gobo loop qualifies: its map is mostly black, so ONE/ONE adds
		// the bright ribbons and leaves the rest of the item untouched. A glow
		// shell does NOT: it is an emissive companion of the artwork underneath,
		// and the tile this renderer binds for the jar's own glow is the
		// specimen's full-colour map, which added at full strength would wash
		// the specimen out. Glow keeps the old skip; nothing in the files names
		// a separate emissive map for it here.
		bool IsAdditiveShellMaterial(const std::string& a_matPath)
		{
			std::string ml = LowerAscii(a_matPath);
			std::replace(ml.begin(), ml.end(), static_cast<char>(92), '/');
			static constexpr const char* kAdditiveTokens[] = {
				"caustic", "gobo", "idleanimloop", "animloop", nullptr
			};
			for (const char* const* t = kAdditiveTokens; *t != nullptr; ++t) {
				if (ml.find(*t) != std::string::npos) {
					return true;
				}
			}
			return false;
		}

		// Printed/cutout surfaces (weapon markings, card symbols/letters,
		// poster art): use the texture's own alpha/mask instead of the
		// unrelated material-wide CDB override alpha. On the texture-less
		// CPU path these sheets are skipped rather than paving the surface
		// beneath. Glass is structural and deliberately NOT in this set.
		bool IsDecalSheet(const std::string& a_matPath)
		{
			std::string ml = LowerAscii(a_matPath);
			return ml.find("decal") != std::string::npos || ml.find("label") != std::string::npos ||
				ml.find("letter") != std::string::npos || ml.find("symbol") != std::string::npos ||
				ml.find("poster") != std::string::npos ||
				ml.find("overlay") != std::string::npos || ml.find("blank") != std::string::npos;
		}

		// The Breach Tactical Stock's authored albedo is wood-grain, unlike
		// the neutral grey stock shown in the inventory reference. Keep this
		// exact preview mesh untextured and render it as neutral grey without
		// changing the shared game texture or other Breach materials.
		bool IsBreachTacticalStock(const std::string& a_matPath, const std::string& a_nodeName)
		{
			std::string mat = LowerAscii(a_matPath);
			std::replace(mat.begin(), mat.end(), '\\', '/');
			std::string node = LowerAscii(a_nodeName);
			return mat == "materials/weapons/breach/breach_stock_tactical.mat" &&
				(node.empty() || node.starts_with("breach_stock_tactical") ||
					node == a_matPath || node == mat ||
					node == "materials\\weapons\\breach\\breach_stock_tactical.mat");
		}


		// The compact Razorback grip emblem is only eight triangles, below
		// the general tiny-mesh cutoff. Preserve this one authored decal
		// without disabling that filter for every effect/decal material.
		bool IsRazorbackGripDecal(const std::string& a_matPath)
		{
			std::string ml = LowerAscii(a_matPath);
			std::replace(ml.begin(), ml.end(), '\\', '/');
			return ml == "materials/weapons/razorback/decals.mat";
		}

		// A flat-shaded surface has no albedo map: the shipped database gives
		// it an empty texture set and the game's colour lives in the
		// material's NAME ("..._Metal_White", "..._Labels_Green"). When a
		// range has no texture this recovers that base colour instead of the
		// index-cycled grey palette, which is why the crate of medical
		// treatments rendered as an undifferentiated blob. Matched on the
		// LAST name token only: a substring test would call "Predator" red.
		// Returns false when the name carries no colour word, and the
		// caller keeps MeshColor's neutral grey.
		bool FlatBaseColor(const std::string& a_matPath, float a_out[3])
		{
			// The vial liquid, before any name logic: `ScienceGlassLiquid01/02`
			// has no colour word in its name, no map, and no colour anywhere in
			// the database (0.8.346 audit, HANDOFF 0d) - so it drew the neutral
			// grey fallback and the bottle read as empty. The fill family behind
			// it, `ScienceGlass_GlassCleanTransparent01_Base_Green03`, stores
			// (0, 0.6524, 0.5964): that is the shelf's measured liquid green.
			// Authored like kAuthoredGlassTint - the value is not the liquid's
			// own, it is the colour of the fluid this shelf's glass holds.
			struct AuthoredFlat { const char* mat; float rgb[3]; };
			static constexpr AuthoredFlat kAuthoredFlat[] = {
				{ "scienceglassliquid01", { 0.00f, 0.6524f, 0.5964f } },
				{ "scienceglassliquid02", { 0.00f, 0.6524f, 0.5964f } },
				{ nullptr, {} },
			};
			{
				std::string ml = LowerAscii(a_matPath);
				std::replace(ml.begin(), ml.end(), static_cast<char>(92), '/');
				for (const AuthoredFlat* e = kAuthoredFlat; e->mat != nullptr; ++e) {
					if (ml.find(e->mat) != std::string::npos) {
						a_out[0] = e->rgb[0];
						a_out[1] = e->rgb[1];
						a_out[2] = e->rgb[2];
						REX::INFO("SFShopExplorer offline GPU: authored liquid tint '{}' -> ({:.3f},{:.3f},{:.3f}) "
							"(fill-family green; the database stores no colour for the liquid)",
							a_matPath, e->rgb[0], e->rgb[1], e->rgb[2]);
						return true;
					}
				}
			}
			// Keep the authored case: the camelCase split below needs it. Every
			// comparison lowercases its own token.
			std::string stem = a_matPath;
			const auto slash = stem.find_last_of("/\\:");
			if (slash != std::string::npos) {
				stem.erase(0, slash + 1);
			}
			const auto dot = stem.find_last_of('.');
			if (dot != std::string::npos) {
				stem.erase(dot);
			}
			while (!stem.empty() && (stem.back() == '_' || stem.back() == '-' ||
				(stem.back() >= '0' && stem.back() <= '9'))) {
				stem.pop_back();
			}
			const auto us = stem.find_last_of('_');
			// Wood-named flat materials with no colour word (AKWoodVeneerWorn01 on
			// the plant planter) have an empty CDB texture set, so they would
			// otherwise fall back to neutral grey. Approximate the veneer brown.
			if (us == std::string::npos && LowerAscii(stem).find("wood") != std::string::npos) {
				a_out[0] = 0.42f;
				a_out[1] = 0.30f;
				a_out[2] = 0.20f;
				return true;
			}
			// The colour word is the last TOKEN of the stem, not necessarily the
			// text after the last underscore. The chem inhalers name theirs with
			// camelCase (Crafted_InHailerYellow), and reading only after '_' left
			// the Hypergiant Heart's yellow body untextured grey. Tokens split on
			// '_' and on lower->upper humps; a single-token stem stays one token,
			// so "Predator" is still not red and "Bread" is still not red.
			const std::string tail = us == std::string::npos ? stem : stem.substr(us + 1);
			std::string word;
			{
				std::string cur;
				for (std::size_t i = 0; i < tail.size(); ++i) {
					const char ch = tail[i];
					if (i > 0 && ch >= 'A' && ch <= 'Z') {
						const char prev = tail[i - 1];
						if ((prev >= 'a' && prev <= 'z') || (prev >= '0' && prev <= '9')) {
							cur.clear();
						}
					}
					cur.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
				}
				word = cur;
			}
			if (word.empty()) {
				return false;
			}
			struct Named { const char* name; float rgb[3]; };
			static constexpr Named kNamed[] = {
				{ "white",  { 0.86f, 0.87f, 0.84f } },
				{ "silver", { 0.72f, 0.73f, 0.76f } },
				{ "grey",   { 0.55f, 0.56f, 0.57f } },
				{ "gray",   { 0.55f, 0.56f, 0.57f } },
				{ "green",  { 0.42f, 0.55f, 0.45f } },
				{ "red",    { 0.62f, 0.22f, 0.20f } },
				{ "bronze", { 0.60f, 0.44f, 0.26f } },
				{ "gold",   { 0.78f, 0.66f, 0.32f } },
				{ "black",  { 0.17f, 0.17f, 0.18f } },
				{ "blue",   { 0.30f, 0.38f, 0.60f } },
				{ "yellow", { 0.80f, 0.72f, 0.30f } },
				{ "orange", { 0.78f, 0.48f, 0.20f } },
				{ "brown",  { 0.42f, 0.30f, 0.20f } },
				{ "tan",    { 0.76f, 0.70f, 0.58f } },
				{ "beige",  { 0.76f, 0.70f, 0.58f } },
				// Planter soil: DirtPacked01_SoilBowl_Soil01.mat is a textureless
				// set, so the plant stand rendered as a neutral grey mound. The
				// approximations match the wood entry above: a colour recovered
				// from the name, not a value read out of the game.
				{ "soil",   { 0.34f, 0.26f, 0.18f } },
				{ "dirt",   { 0.34f, 0.26f, 0.18f } },
				{ "earth",  { 0.34f, 0.26f, 0.18f } },
				{ "purple", { 0.42f, 0.32f, 0.55f } },
			};
			for (const auto& n : kNamed) {
				if (word == n.name) {
					a_out[0] = n.rgb[0];
					a_out[1] = n.rgb[1];
					a_out[2] = n.rgb[2];
					return true;
				}
			}
			return false;
		}

		// BC4 (one channel) and BC5 (two: a normal map) carry no colour, so
		// sampling one as albedo renders the surface as flat noise. The
		// crate's metal panels hit exactly this: their texture set is empty
		// and the fuzzy scorer handed back a BC4 mask at fmt=80, which is
		// why they came out black-and-red.
		bool IsSingleChannelFormat(std::uint32_t a_fmt)
		{
			return a_fmt == 80 || a_fmt == 83;
		}

		// Mean colour of a BC1 map, decoded the way the GPU does it. Used only to
		// tell a shared neutral paint base from a map that already carries its
		// own colour, which is what decides whether a database override colour
		// may be multiplied in. Sampled on a stride so even a 2048x2048 atlas
		// costs a few thousand blocks on the upload path.
		//
		// Returns false for any format that is not BC1/BC3, or for unreadable
		// bytes, so "could not measure" is never treated as "neutral".
		bool Bc1MeanRgb(const std::vector<std::uint8_t>& a_bytes, std::uint32_t a_w, std::uint32_t a_h,
			std::uint32_t a_fmt, float a_out[3])
		{
			a_out[0] = a_out[1] = a_out[2] = 0.0f;
			if (a_fmt != 71 && a_fmt != 72 && a_fmt != 77 && a_fmt != 78) {
				return false;
			}
			if (a_bytes.size() < 8 || a_w == 0 || a_h == 0) {
				return false;
			}
			const std::size_t blocksX = (a_w + 3) / 4;
			const std::size_t blocksY = (a_h + 3) / 4;
			const std::size_t rowPitch = blocksX * 8;
			const std::size_t step = std::max<std::size_t>(1,
				static_cast<std::size_t>(std::sqrt(static_cast<double>(blocksX * blocksY) / 4096.0)));
			double total[3]{ 0.0, 0.0, 0.0 };
			std::uint64_t samples = 0;
			const auto expand565 = [](std::uint16_t a_c, float a_rgb[3]) {
				const int r = (a_c >> 11) & 0x1F;
				const int g = (a_c >> 5) & 0x3F;
				const int b = a_c & 0x1F;
				a_rgb[0] = static_cast<float>((r << 3) | (r >> 2)) / 255.0f;
				a_rgb[1] = static_cast<float>((g << 2) | (g >> 4)) / 255.0f;
				a_rgb[2] = static_cast<float>((b << 3) | (b >> 2)) / 255.0f;
			};
			for (std::size_t by = 0; by < blocksY; by += step) {
				for (std::size_t bx = 0; bx < blocksX; bx += step) {
					const std::size_t off = by * rowPitch + bx * 8;
					if (off + 8 > a_bytes.size()) {
						return false;
					}
					const std::uint16_t c0 = static_cast<std::uint16_t>(a_bytes[off] | (a_bytes[off + 1] << 8));
					const std::uint16_t c1 = static_cast<std::uint16_t>(a_bytes[off + 2] | (a_bytes[off + 3] << 8));
					float e0[3], e1[3];
					expand565(c0, e0);
					expand565(c1, e1);
					for (int t = 0; t < 16; ++t) {
						const int k = (a_bytes[off + 4 + t / 4] >> (2 * (t % 4))) & 3;
						float rgb[3];
						if (k == 0) {
							rgb[0] = e0[0]; rgb[1] = e0[1]; rgb[2] = e0[2];
						} else if (k == 1) {
							rgb[0] = e1[0]; rgb[1] = e1[1]; rgb[2] = e1[2];
						} else if (c0 > c1) {
							const float w = (k == 2) ? 1.0f / 3.0f : 2.0f / 3.0f;
							rgb[0] = e0[0] + (e1[0] - e0[0]) * w;
							rgb[1] = e0[1] + (e1[1] - e0[1]) * w;
							rgb[2] = e0[2] + (e1[2] - e0[2]) * w;
						} else if (k == 2) {
							rgb[0] = rgb[1] = rgb[2] = 0.0f;
						} else {
							rgb[0] = e0[0] * 0.5f + e1[0] * 0.5f;
							rgb[1] = e0[1] * 0.5f + e1[1] * 0.5f;
							rgb[2] = e0[2] * 0.5f + e1[2] * 0.5f;
						}
						total[0] += rgb[0];
						total[1] += rgb[1];
						total[2] += rgb[2];
						++samples;
					}
				}
			}
			if (samples == 0) {
				return false;
			}
			for (int c = 0; c < 3; ++c) {
				a_out[c] = static_cast<float>(total[c] / static_cast<double>(samples));
			}
			return true;
		}

		// A shared neutral paint base: bright and near-grey. Measured on the two
		// maps that made the ammo crates wrong - paint01_color is (197,198,197)
		// and ammobox01_color is (113,90,53), so this test separates "white
		// paint waiting for a colour" from "a map that already has one".
		bool IsNeutralPaintBase(float a_rgb[3])
		{
			const float mx = std::max({ a_rgb[0], a_rgb[1], a_rgb[2] });
			const float mn = std::min({ a_rgb[0], a_rgb[1], a_rgb[2] });
			return mx > 0.55f && (mx - mn) / mx < 0.12f;
		}

		// The database stores MaterialOverrideColor in the engine's LINEAR space;
		// this preview multiplies SRGB-encoded albedo bytes. Applying the raw
		// value darkened the .27 CAL lid to (167,134,78) - the game measures
		// (201,186,118) there, and the sRGB-converted colour lands at (183,167,130)
		// before the key light, which is the same paint. Every existing consumer
		// of the override (the drink bags) keeps the raw value; only the paint
		// path converts.
		float LinearToSrgb(float a_c)
		{
			if (!(a_c > 0.0f)) {
				return 0.0f;
			}
			return a_c <= 0.0031308f ? 12.92f * a_c : 1.055f * std::pow(a_c, 1.0f / 2.4f) - 0.055f;
		}

		// Mean of a BC4 single-channel map, exactly as the GPU would average it:
		// two 8-bit endpoints plus sixteen 3-bit indices per 4x4 block, with the
		// six interpolants between them. Every _metal/_rough companion in the
		// shipped weapon sets is BC4 (measured on SFBGS050's BreechBlock,
		// Razorback's receiver and DLC001's grenade atlas), and both describe the
		// surface as a whole, so one number per range is all the shading asks
		// for - no extra SRV, no extra texture, no extra bind.
		//
		// Returns -1.0 when the bytes cannot be read as BC4, so "no map" is never
		// mistaken for "metalness 0".
		float Bc4Mean(const std::vector<std::uint8_t>& a_bytes, std::uint32_t a_w, std::uint32_t a_h)
		{
			if (a_bytes.size() < 8 || a_w == 0 || a_h == 0) {
				return -1.0f;
			}
			const std::size_t rowPitch = static_cast<std::size_t>((a_w + 3) / 4) * 8;
			const std::size_t blocksX = (a_w + 3) / 4;
			const std::size_t blocksY = (a_h + 3) / 4;
			double total = 0.0;
			std::uint64_t samples = 0;
			for (std::size_t by = 0; by < blocksY; ++by) {
				for (std::size_t bx = 0; bx < blocksX; ++bx) {
					const std::size_t off = by * rowPitch + bx * 8;
					if (off + 8 > a_bytes.size()) {
						return -1.0f;
					}
					const int e0 = a_bytes[off];
					const int e1 = a_bytes[off + 1];
					std::uint64_t idx = 0;
					for (int i = 0; i < 6; ++i) {
						idx |= static_cast<std::uint64_t>(a_bytes[off + 2 + i]) << (8 * i);
					}
					for (int t = 0; t < 16; ++t) {
						const int k = static_cast<int>((idx >> (3 * t)) & 7u);
						int v = 0;
						if (k == 0) {
							v = e0;
						} else if (k == 1) {
							v = e1;
						} else {
							v = ((8 - k) * e0 + k * e1 + 4) / 8;
						}
						total += v;
						++samples;
					}
				}
			}
			if (samples == 0) {
				return -1.0f;
			}
			return static_cast<float>(total / static_cast<double>(samples) / 255.0);
		}

		// `textures/a/b_color.dds` -> `textures/a/b_metal.dds`. The companions sit
		// beside the colour map with the same stem in every shipped weapon set
		// (`receiver_2_color/metal/rough`, `grip_color/metal/rough`), and the
		// material database names only the colour and opacity slots, so pairing by
		// name is the only route to them.
		std::string CompanionMapKey(const std::string& a_colorKey, const char* a_suffix)
		{
			const auto at = a_colorKey.rfind("_color.dds");
			if (at == std::string::npos) {
				return {};
			}
			return a_colorKey.substr(0, at) + a_suffix;
		}

	// Clear glass, clean plastic and translucent containers are the one
	// class the preview genuinely could not show: the material database
	// gives them an EMPTY texture set (GlassCleanTransparent02 ships
	// only rough/normal maps), so no albedo exists to sample, and all
	// three existing pipelines are opaque. The result was a flat opaque
	// blob where a display case should be see-through.
	//
	// What the compiled database actually holds was decoded in full (see
	// LiveModel.cpp g_cdbAlpha): across the whole 105 MB file only 43
	// distinct field ids appear, and a _Material object ever carries just
	// its name plus 0x25a - MaterialOverrideColorTypeComponent::Color,
	// an XMFLOAT4 whose w is a real per-material override alpha. There is
	// no translucency scalar to recover: the game's own see-through-ness
	// comes from the shader route, not from a stored number.
	//
	// So the alpha is taken from the database when it has one (that also
	// catches the decal family, whose median override alpha is 0.21 and
	// which used to render as an opaque sheet), and only falls back to
	// the old name-keyed approximation when the material stores none -
	// 82% of materials, including most glass. Plastic on its own stays
	// opaque unless the database says otherwise, because making every
	// plastic container see-through would be wrong far more often than
	// right.
	//
	// 6r-7 measured the two obvious upgrades and rejected both, so the
	// name tokens stay. `renderer_recon/route_census.py`: the decoded
	// shader-route fields 0x0472 (UniqueMap/HighResUniqueMap/
	// DetailMapTiling/Tiling) and 0x0543 (Effect/Deferred/Water/
	// PrecomputedScattering) ride on _TextureSet objects, cover only
	// 1,973 of 49,212 materials, and show the SAME distribution for glass
	// as for metal - UniqueMap is top of both lists. Using them would
	// change essentially nothing and could only add false positives.
	// `renderer_recon/translucency_rule.py`: "this material's texture set
	// names an opacity map" reaches 12.8% of the see-through set but
	// fires on 52.2% of the materials the database calls opaque, because
	// decals and masks have opacity maps too - far worse than useless.
	// The name tokens are high-precision/low-recall (0.8% recall, and they
	// fire on only 0.8% of opaque materials), which is the right shape for
	// a fallback that runs only when the database has nothing.
	// The crafted drink-bag folder. Every crafted consumable pouch (Alien Tea,
	// Boba Alien Tea, the drink packs) ships its bag, liquid, printed leaf and
	// cap as separate materials in here, so this one name is shared by the
	// film rule and the override-tint rule below to keep them from drifting.
	bool IsCraftedDrinkBagMaterial(const std::string& a_matPath)
	{
		return LowerAscii(a_matPath).find("crafted_drink_bags") != std::string::npos;
	}

	// Ammo crates share one paint map (`textures/common/paint/paint01_color.dds`,
	// measured (197,198,198)) and carry their colour in the database. The folder
	// also holds the crate atlas, which is why LiveModel has to refuse the
	// folder fallback for a textureless part (see FindCdbMaterialLocked).
	bool IsAmmoCratePart(const std::string& a_matPath)
	{
		// NIF material strings use backslashes ("Materials\\SetDressing\\..."),
		// so the separator has to be normalized before matching. Without this the
		// gate silently never fired and the crate body kept its .27-yellow
		// pigment - the 20:09 session log says exactly that:
		//   paint tint 'AmmoBoxLarge_DarkGreen01.mat' kept (0.88,0.80,0.47)
		std::string path = LowerAscii(a_matPath);
		for (char& ch : path) {
			if (ch == static_cast<char>(92)) {
				ch = '/';
			}
		}
		return path.find("setdressing/ammobox01/") != std::string::npos;
	}

	// There is no lid remap. An earlier build rebound the crate lid to
	// `AmmoBoxPrimer_Lid` because a value-first scan had given that object the
	// green paint's colour. Read name-first, `PaintedMetalShine01_Green` stores
	// (0.584,0.800,0.320), which this pipe's linear->sRGB conversion turns into
	// (201,231,153) - the measured LZR lid (202,229,162 top, 98,136,65 front) -
	// while `AmmoBoxPrimer_Lid` stores pale blue (0.600,0.698,0.881). Binding
	// the primer lid would therefore paint the lime lid blue, so each lid keeps
	// its own authored colour and `PaintedMetalshine01_Orange` keeps .27's
	// cream.

	// The drink fill's hue has to be authored, because the atlas does not carry
	// it. Under the pouch's own footprint the flavour atlas samples olive -
	// (114,73,1) at mip 0 on the tea, and its flipped-v region (134,85,37) is no
	// redder - while the engine's liquid layer reads maroon: the inventory
	// reference measures (77,38,41) at mid-body against this preview's
	// (66,57,36). These factors are the ratio between the two once the 0.426
	// override tint is applied: red up, green down, blue untouched because the
	// atlas has none of it to give.
	static constexpr float kDrinkFillBias[3] = { 1.25f, 0.48f, 1.0f };

	// Glass the database does not mark at all - see the authored-sheer branch
	// in MaterialBlendAlpha. Keys are lowercase substrings of the normalized
	// material path; aid_colour_audit.py mirrors both the list and the value,
	// and test_aid_colour_audit.py fails if they drift apart.
	static constexpr const char* kAuthoredSheer[] = {
		"isotopic_coolant_container", nullptr
	};
	// A named container keeps a small presence, not a film and not nothing.
	// Measured, not judged: the game's own top-down inventory render of this jar
	// (2026-10-22 11:29) has its glass adding about 10/255 over the backdrop,
	// i.e. per wall roughly 5%. 0.30 (a guess) composited to 51% of a pale body
	// over the whole model - the milky sleeve of the 2026-10-21 captures - and
	// 0.14 still painted a grey shell over the specimen at every angle. 0.05
	// per wall leaves the specimen ~90% visible and lets the shader's Fresnel
	// reflection (4% head-on, 1.0 at the silhouette) draw the rim.
	//
	// The generic name-token fallback below stays 0: those materials are mostly
	// a wrapping film over printed art (the Pils can's clear sleeve), where any
	// coverage hides the print underneath instead of reading as glass.
	static constexpr float kAuthoredSheerAlpha = 0.05f;

	// The coolant jar's colour. Its database set is empty - no colour map, no
	// override colour - so the glass fell back to the generic pale tint above
	// and the jar read as a blue-grey tub beside the game's own render, which
	// is green. Authored for the same reason the alpha is: nothing in the
	// files stores it.
	//
	// Measured from the 2026-10-22 12:04 inventory capture. The left glass wall
	// averages (27,43,44) against a (8,23,31) backdrop, i.e. it ADDS
	// (19,20,13)/255 - green above red, blue lowest. This renderer's pale tint
	// (0.62,0.66,0.72) was adding (35,36,37) at the same framing. Scaling each
	// channel by the ratio of the two makes the preview add the colour the game
	// does instead of a neutral blue.
	static constexpr float kAuthoredGlassTint[3] = { 0.34f, 0.37f, 0.25f };

	bool IsAuthoredSheerMat(const std::string& a_matPath)
	{
		std::string ml = LowerAscii(a_matPath);
		std::replace(ml.begin(), ml.end(), static_cast<char>(92), '/');
		for (const char* const* g = kAuthoredSheer; *g != nullptr; ++g) {
			if (ml.find(*g) != std::string::npos) {
				return true;
			}
		}
		return false;
	}

	// The clear-glass family the database leaves completely empty.
	//
	// `Materials\Common\Glass\GlassCleanTransparent01.mat` is an authored EMPTY
	// set - no colour, no opacity, no map (and so is its `_double` sibling and
	// the ScienceGlass `..._Base_Green01/02/03` fills). The token scan below
	// calls any glass-named material a clear shell with alpha 0, which is right
	// for a printed can's sleeve but wrong for a beaker: a beaker IS the glass,
	// so the preview drew nothing at all. Measured on the 2026-10-24 contact
	// sheet, tiles 00265ADA (Beaker) and 00265ADE (Graduated Cylinder) were
	// blank; the filled vials survived only because their coloured fill part is
	// a separate material.
	//
	// The family therefore keeps the jar's measured transmission
	// (kAuthoredSheerAlpha: ~10/255 over two walls, i.e. 0.05 per wall) and the
	// tint the database does write for the `_Base_Green` fills. Everything else
	// in those two folders - `ScienceGlassPlastic02`, the liquids, the decal -
	// is not glass and must keep its own rule, so the match needs both the
	// folder and the `glasscleantransparent` stem: "Glass" alone also appears
	// inside `ScienceGlassPlastic02`.
	bool IsFlatClearGlassMat(const std::string& a_matPath)
	{
		std::string ml = LowerAscii(a_matPath);
		std::replace(ml.begin(), ml.end(), static_cast<char>(92), '/');
		const bool knownFolder = ml.find("/common/glass/") != std::string::npos ||
			ml.find("/setdressing/scienceglass/") != std::string::npos;
		return knownFolder && ml.find("glasscleantransparent") != std::string::npos;
	}

	// The Sparkling Wine's body is not a clear shell, and the audit row that
	// called it "no colour map, only an opacity mask" looked in the wrong
	// component. Its texture set is empty, but the SAME material's Blender
	// layer names exactly one texture: materialsbeta.cdb object
	// `GenericSpaceWine_Glass_Blender1` carries
	// `Data\TEXTURES\Common\Detail\Manmade\DetailAbstractPattern06_mask.DDS`
	// (field 0x038a). Decoded, that map is white cells under a black web -
	// which is the bottle in the 2026-10-22 inventory capture, its silhouette
	// white with the net printed over it. The other candidate,
	// `genericspacewine_decal_opacity.dds`, is the ornate CREST label on the
	// same bottle (a white silhouette on black), so it is the print, not the
	// body.
	//
	// Bound as this range's albedo the mask takes the greyscale-ink path
	// (mode 11) instead of the blend path: the body is opaque white with the
	// web dark. Left as clear glass it sampled as a Fresnel shell over the
	// preview backdrop, which is why the bottle read as a dark grey tube.
	//
	// Scoped to the one material that authors this, the same way kAuthoredSheer
	// and kAuthoredGlassTint are: a general blender-mask pass is a bigger
	// change (the index would have to carry the layer's map for every stem)
	// and nothing else on the shelves has been measured against it yet.
	struct AuthoredPattern
	{
		const char* mat;
		const char* map;
	};
	static constexpr AuthoredPattern kAuthoredPattern[] = {
		{ "genericspacewinebottle/genericspacewine_glass",
			"textures/common/detail/manmade/detailabstractpattern06_mask.dds" },
		{ nullptr, nullptr }
	};

	bool FindAuthoredPatternMap(const std::string& a_matPath, std::string& a_map)
	{
		std::string ml = LowerAscii(a_matPath);
		std::replace(ml.begin(), ml.end(), static_cast<char>(92), '/');
		for (const AuthoredPattern* p = kAuthoredPattern; p->mat != nullptr; ++p) {
			if (ml.find(p->mat) != std::string::npos) {
				a_map = p->map;
				return true;
			}
		}
		return false;
	}

	// The asset-shelf folders whose name is a namespace, not a material word.
	//
	// `Materials\SetDressing\ScienceGlass\` holds the plastic caps and the vial
	// liquid beside the glass fills, and every file repeats the shelf name
	// (`ScienceGlassPlastic02`, `ScienceGlassLiquid01`). The sheer scan read the
	// whole path, so the "Glass" of that repetition was a word start: the caps
	// and the liquid were declared clear shells at alpha 0 and vanished (the
	// Vortex Ichor's casing drew as a film over nothing). On this shelf the
	// prefix is a namespace - the material is `Plastic02`, `Liquid01`.
	//
	// The list is short because the same structure proves the opposite
	// elsewhere: `Materials\Common\Glass\GlassDirtyHeavyFrost01` repeats its
	// folder AND IS glass, so stripping there would make a real frosted pane
	// opaque (measured on the Vortex Ichor, which wears that material). A folder
	// only belongs here when it has been checked to hold materials that are not
	// what it is called.
	static constexpr const char* kNamespaceFolders[] = { "scienceglass", nullptr };

	// A material's OWN name: the stem, with a leading repeat of a namespace
	// folder dropped when something follows it.
	//
	// Only a LEADING repetition is dropped, only for a folder on
	// kNamespaceFolders, and only when at least one character follows it, so a
	// material named exactly after its folder keeps its whole name. Case is
	// preserved because MaterialNameHasWord reads camel humps from the stem.
	std::string MaterialOwnName(const std::string& a_path)
	{
		std::string norm = a_path;
		std::replace(norm.begin(), norm.end(), static_cast<char>(92), '/');
		const std::size_t slash = norm.rfind('/');
		std::string stem = slash == std::string::npos ? norm : norm.substr(slash + 1);
		if (stem.size() > 4 && LowerAscii(stem.substr(stem.size() - 4)) == ".mat") {
			stem.resize(stem.size() - 4);
		}
		if (slash == std::string::npos || slash == 0) {
			return stem;
		}
		const std::size_t prev = norm.rfind('/', slash - 1);
		const std::string folder = LowerAscii(prev == std::string::npos ?
			norm.substr(0, slash) : norm.substr(prev + 1, slash - prev - 1));
		bool namespaced = false;
		for (const char* const* f = kNamespaceFolders; *f != nullptr && !namespaced; ++f) {
			namespaced = folder == *f;
		}
		if (!namespaced || stem.size() <= folder.size() ||
			LowerAscii(stem.substr(0, folder.size())) != folder) {
			return stem;
		}
		std::size_t at = folder.size();
		while (at < stem.size() && (stem[at] == '_' || stem[at] == '-')) {
			++at;
		}
		if (at >= stem.size()) {
			return stem;
		}
		return stem.substr(at);
	}

	// A material NAME word, not a substring of one.
	//
	// The sheer tokens used to be found with find(), so any material whose
	// name merely contains "glass" became a clear shell. The Misc shelf audit
	// (2026-10-23) found the whole hourglass drawing as one: `Hourglass_Case`
	// and `Hourglass_Sand` both carry the "glass" of "hourglass", and the
	// hourglass preview was an empty outline. The same scan would empty any
	// item wearing `PlasticFiberglass01`.
	//
	// A word starts at the beginning of the path, after any non-alphanumeric
	// separator, or at a lower->upper camel hump (`ScienceGlassLiquid`). Case
	// is therefore read before lowercasing: `Hourglass_Case` has a lowercase
	// g inside a word, `Hourglass_Glass` and `GlassHelmets01` start one.
	bool MaterialNameHasWord(const std::string& a_path, const char* a_word)
	{
		const std::string word = LowerAscii(a_word);
		const std::size_t n = word.size();
		if (n == 0 || a_path.size() < n) {
			return false;
		}
		for (std::size_t i = 0; i + n <= a_path.size(); ++i) {
			bool same = true;
			for (std::size_t k = 0; k < n && same; ++k) {
				same = static_cast<char>(std::tolower(static_cast<unsigned char>(a_path[i + k]))) ==
					word[k];
			}
			if (!same) {
				continue;
			}
			if (i == 0) {
				return true;
			}
			const unsigned char prev = static_cast<unsigned char>(a_path[i - 1]);
			if (!std::isalnum(prev)) {
				return true;
			}
			const unsigned char ch = static_cast<unsigned char>(a_path[i]);
			if (ch >= 'A' && ch <= 'Z' && (std::islower(prev) || std::isdigit(prev))) {
				return true;
			}
		}
		return false;
	}

	float MaterialBlendAlpha(const std::string& a_matPath, bool& a_translucent, bool& a_film)
	{
		float cdb = 1.0f;
		a_film = false;
		// MaterialOverrideColor alpha belongs to the game's shader/blender
		// composition, not the diffuse decals' coverage. For decal sheets it
		// is often a tiny unrelated value (Rattler_Decals is 0.0078), which
		// made otherwise-correct lettering effectively invisible in our
		// conventional source-alpha blend. Let the decoded color/opacity maps
		// determine decal coverage instead of applying that scalar.
		if (IsDecalSheet(a_matPath)) {
			a_translucent = false;
			return 1.0f;
		}
		// A MaterialOverrideColor alpha is the surface's opacity only when the
		// material is a surface. When the object declares a TextureSetKind of
		// "Multiply" or "Lerp" (field 0x22b) its texture set composites over a
		// layer beneath it, so the same alpha is that layer's blend weight and
		// the material still draws opaque. The Razorback's receiver is the
		// measured case: a creation database overrides it with
		// (0.67, 0.697, 1.0, 0.635) Multiply, and drawing it at 0.635 made the
		// whole rifle -- 8 of its 46 ranges -- see-through in the preview.
		// 1,681 of the 6,205 objects carrying an override alpha below 1.0 are
		// Multiply, so this is not a per-item patch. The ones with no kind
		// record are NOT automatically opacity either: the Tranquilitea cup's
		// base and lid both store 0.607843 (= 155/255) and the Velocity
		// beverage stores 0.501961 (= 128/255), and the engine draws all three
		// opaque. Two unrelated surfaces sharing one byte-exact value is the
		// signature of a layer weight, so consumable materials are treated the
		// same way as garments below.
		std::string ml = LowerAscii(a_matPath);
		// NIF material strings use backslashes; every path test below is written
		// with forward slashes (LiveModel's LowerAscii normalizes for exactly
		// this reason, OfflineModel's does not). Without this the garment test
		// never matched a real material: the Chunks cap kept the material's
		// 8/255 layer weight as surface opacity and its body drew at 3% alpha.
		std::replace(ml.begin(), ml.end(), '\\', '/');
		// Garments are opaque. Their stored override alpha is a layer weight,
		// not a surface opacity: the Chunks cap's fabric material
		// (Outfit_Baseball_Cap_01_Material1) carries (1,1,1,8/255), and drawing
		// it as 3% opaque made the whole cap body vanish, leaving only the red
		// logo decal floating. Every genuinely see-through garment part names
		// itself (CollarGlass, visor, lens), so those still take their alpha
		// from the token fallback below.
		const bool garment = ml.find("materials/clothes/") != std::string::npos ||
			ml.find("/clothes/") != std::string::npos;
		const bool garmentSheerToken = ml.find("glass") != std::string::npos ||
			ml.find("visor") != std::string::npos || ml.find("lens") != std::string::npos ||
			ml.find("translu") != std::string::npos || ml.find("transpar") != std::string::npos;
		// Consumables are opaque for the same measured reason. The two cases
		// that stayed ghosted after the layer-kind join - the Tranquilitea cup
		// (0.608 on both base and lid) and the Velocity beverage (0.502) - are
		// byte-exact weights, and the engine's own preview beside them is
		// opaque. Scoped to the item folders so set-dressing glass (which names
		// itself glass) keeps the token fallback below.
		const bool consumableItem = ml.find("/fooddrink_singleitems/") != std::string::npos ||
			ml.find("/drugs_or_medical/") != std::string::npos;
		std::uint8_t layerKind = 0;
		if (LiveModel::FindCdbAlpha(a_matPath, cdb, &layerKind) && cdb < 0.995f && cdb > 0.0f) {
			if (layerKind == 1 || layerKind == 2) {
				REX::INFO("SFShopExplorer offline GPU: override alpha {:.3f} ignored for '{}' ({} layer, not surface opacity)",
					cdb, a_matPath, layerKind == 1 ? "Multiply" : "Lerp");
				a_translucent = false;
				return 1.0f;
			}
			if ((garment || consumableItem) && !garmentSheerToken) {
				REX::INFO("SFShopExplorer offline GPU: override alpha {:.3f} ignored for '{}' ({} layer weight, not surface opacity)",
					cdb, a_matPath, garment ? "garment" : "consumable");
				a_translucent = false;
				return 1.0f;
			}
			// A surface is never 1/255 opaque. The small values are layer
			// weights that arrived without a TextureSetKind record, and drawing
			// them as coverage deletes the surface instead of shading it:
			// MenuItem_Blend's printed can body stores 0.025 and a large family
			// of painted ship metal, cave floor and hardwood materials store
			// 0.0039 (1/255) - the same signature as the Chunks cap's 8/255
			// garment weight. Below this floor the value cannot be coverage, so
			// the material keeps its surface and the token scan further down
			// still gets its say (that is what routes glass to the clear-shell
			// fallback instead of an opaque grey surface).
			constexpr float kMinSurfaceAlpha = 0.05f;
			// ...and it is surface opacity only for a material that names
			// itself see-through. That is not a hunch: on the shop's Aid and
			// Ammo shelves exactly four distinct materials ever reach this
			// branch (aid_colour_audit.py counts them), and three are opaque
			// on screen. The Tranquilitea to-go cup's base and lid both store
			// 0.607843 and drew ghosted, and the Isotopic Coolant jar's lid
			// stores 0.251774 and left the jar's metal cap see-through. Both
			// are the game's own inventory view drawn solid. The fourth,
			// GlassHelmets01 at 0.686, does name itself glass and is the one
			// value worth keeping - it is the body of every inhaler, and the
			// engine draws that as smoked glass, darker than a clear shell.
			// So the database value survives only where the name agrees with
			// it; everywhere else the surface stays solid and the token scan
			// below still gets its say.
			const bool sheerName = garmentSheerToken ||
				ml.find("clear") != std::string::npos;
			if (cdb >= kMinSurfaceAlpha && !sheerName) {
				REX::INFO("SFShopExplorer offline GPU: override alpha {:.3f} ignored for '{}' "
					"(material does not name itself see-through; layer weight, not surface opacity)",
					cdb, a_matPath);
				a_translucent = false;
				return 1.0f;
			}
			if (cdb >= kMinSurfaceAlpha) {
				a_translucent = true;
				return cdb;
			}
			REX::INFO("SFShopExplorer offline GPU: override alpha {:.4f} ignored for '{}' "
				"(below the {:.2f} surface floor; layer weight, not surface opacity)",
				cdb, a_matPath, kMinSurfaceAlpha);
		}

		// The crafted drink-bag film. Every crafted consumable pouch (Alien Tea,
		// Boba Alien Tea, the drink packs) is a transparent bag over visible
		// contents in the inventory, and the database stores no alpha for it, so
		// it rendered as an opaque gold shell with the tea and leaf hidden.
		//
		// Scoped to the pouch by BOTH the folder and the material name on
		// purpose: the broader rule "this material names an opacity map" was
		// already measured and rejected (see above -- it fires on 52% of the
		// materials the database calls opaque). Keying on the pouch alone keeps
		// the tea, leaf and tonic materials inside it opaque, which is what they
		// are; only the bag is film.
		//
		// The pouch ships crafteddrinkbags/alienenergydrink_opacity.dds, a BC4
		// 1024x1024 map (measured: mean 87/255, 34.5% near black, 17.5% near
		// white, smooth in between; under this mesh's own UV footprint it is a
		// flat 0.230 mean, 98% of texels inside 0.10-0.30). That is coverage,
		// not a stencil -- so the range is blended with the map (shader mode 8)
		// rather than clipped by it. a_translucent stays false because the
		// constant-alpha pass never samples the map; the caller marks the range
		// filmCoverage instead.
		if (IsCraftedDrinkBagMaterial(a_matPath) && ml.find("pouch") != std::string::npos) {
			a_film = true;
			return 1.0f;
		}
		// Materials that are glass on screen while the database leaves them
		// unmarked. Alien Genetic Material is the measured case: its jar body
		// (materials/setdressing/manufactured_goods/isotopic_coolant_container)
		// has an EMPTY database set - no colour, no opacity, nothing - so it
		// fell through every rule and drew as a solid grey tub with the alien
		// buried inside. The game's inventory view shows a clear jar with the
		// specimen plainly visible through it. Keyed by material, not by the
		// item, because the same coolant jar is reused by every Legendary
		// Catalyst and every one of them is glass.
		if (IsAuthoredSheerMat(a_matPath)) {
			REX::INFO("SFShopExplorer offline GPU: authored glass '{}' -> {:.2f} alpha (database has no opacity for it)",
				a_matPath, kAuthoredSheerAlpha);
			a_translucent = true;
			return kAuthoredSheerAlpha;
		}
		// Clear flat glass (see IsFlatClearGlassMat): the family names itself
		// glass and ships no art, so the token scan would make it invisible.
		// Kept at the jar's measured per-wall transmission instead.
		if (IsFlatClearGlassMat(a_matPath)) {
			REX::INFO("SFShopExplorer offline GPU: flat clear glass '{}' -> {:.2f} alpha (empty database set, family has no art)",
				a_matPath, kAuthoredSheerAlpha);
			a_translucent = true;
			return kAuthoredSheerAlpha;
		}
		static constexpr const char* kTokens[] = {
			"glass", "clear", "translucent", "transpar", nullptr
		};
		// The material's own name, not the whole path: the folder is a
		// namespace, and `ScienceGlassPlastic02` is a plastic cap, not glass.
		// See MaterialOwnName.
		const std::string ownName = MaterialOwnName(a_matPath);
		for (const char* const* t = kTokens; *t != nullptr; ++t) {
			// Word start, not substring: see MaterialNameHasWord. The scan ran
			// on `ml` (lowercased) before, which cannot tell "hourglass" from
			// "GlassHelmets".
			if (MaterialNameHasWord(ownName, *t)) {
				a_translucent = true;
				// Clear, not 30% opaque: see kAuthoredSheerAlpha. The name says
				// "this is glass", it does not say how much light the body stops.
				return 0.00f;
			}
		}
		a_translucent = false;
		return 1.0f;
	}

	bool BuildGpuModel(const Model& a_model, const std::string& a_key, bool a_weapon)
		{
			const bool authoredOrientation = g_authoredOrientation.load(std::memory_order_acquire);
			auto* device = g_gpu.device;
			if (!device) {
				return false;
			}
			if (g_gpuModel.vb && g_gpuModel.key == a_key) {
				return true;
			}
			ReleaseGpuModel();
			// Weapon orientation, closed for good: the muzzle is +Y
			// (ProjectileNode spawns there at y=+0.17; the front drum under
			// the barrel sits at +0.13), rear/grip -Y, slide +Z, wood -Z.
			// Cyclic (x,y,z)->(-y,z,-x) sends the muzzle onto -X (left at
			// the weapon yaw), slide onto +Y (up), wood onto -Y (down),
			// rear onto +X (right). det +1, proper. History: the 46 build
			// had exactly this and the user confirmed inventory positioning;
			// 47 mirrored newX and put the muzzle right ("opposite side").
			// the rest.
			const bool layFlat = !a_weapon && a_model.extents[1] > 1.8f * a_model.extents[0] &&
				a_model.extents[1] > 1.8f * a_model.extents[2];
			const float inv = 1.0f / (a_model.radius > 1e-6f ? a_model.radius : 1.0f);
			std::vector<float> pos;
			std::vector<float> uvs;
			std::vector<std::uint32_t> idx;
			pos.reserve(196608);
			uvs.reserve(131072);
			idx.reserve(131072);
		std::size_t meshIndex = 0;
		// First pass skips tiny sub-meshes (attachment points, decals) that
		// would overlap the main geometry; if NOTHING passes (e.g. a 12-tri
		// clutter cube), the second pass renders unfiltered â€” a tiny render
		// beats an empty viewport and a needless GPU ban.
		// Ranges with a visibly tinted database override candidate (6r-7).
		// Some may get a real albedo later, in which case upload resets their
		// vertex tint to white. Tally per pass so the diagnostic cannot
		// double-count when accumulate() runs twice.
		std::uint32_t cdbTintCandidates = 0;
		auto accumulate = [&](bool a_skipTiny) {
			cdbTintCandidates = 0;
			meshIndex = 0;
			pos.clear();
			uvs.clear();
			idx.clear();
			g_gpuModel.ranges.clear();
			for (const auto& mesh : a_model.meshes) {
				++meshIndex;
				const std::size_t vertCount = mesh.pos.size() / 3;
				const std::size_t triCount = mesh.indices.size() / 3;
				// Small-but-wide parts (soil discs, sights, muzzles) are real
				// geometry: only skip a tiny mesh when it is also compact
				// relative to the whole model (attachment points, effect
				// quads). A soil disc spans the pot â€” it must survive.
				// Compact decal islands carry real printwork (Breach's
				// 8-20-triangle atlas patches disappear entirely if filtered).
				// Keep effect/attachment meshes filtered as before.
				const bool tinyGeometry = triCount < 24 || vertCount < 12;
				const bool tinyDecalSheet = tinyGeometry &&
					(IsDecalSheet(mesh.matPath) || IsRazorbackGripDecal(mesh.matPath));
				if (a_skipTiny && tinyGeometry && !tinyDecalSheet && !MeshSpread(mesh, a_model.radius)) {
					continue;
				}
				if (vertCount > 4'000'000) {
					continue;
				}
				float base[3];
				MeshColor(meshIndex, base);
				const bool breachStock = IsBreachTacticalStock(mesh.matPath, mesh.name);
				const std::uint32_t baseV = static_cast<std::uint32_t>(pos.size() / 3);
				if (baseV + vertCount > 4'000'000) {
					return;
				}
				const bool hasUv = mesh.uv.size() == vertCount * 2;
				// Does every face of this mesh point at its own centre? Area
				// weighted, in source space so the model rotation cannot change
				// the answer: a hull authored to be looked at from the inside
				// answers ~-1, an ordinary surface sits near 0. Measured on the
				// shipped London globe, only its backdrop is a hull
				// (`Snowglobe_Landmark_Backgrounds01`: -0.99; the dome, authored
				// outward, is +0.93).
				float meshInward = 0.0f;
				{
					float cx = 0.0f, cy = 0.0f, cz = 0.0f;
					for (std::size_t v = 0; v < vertCount; ++v) {
						cx += mesh.pos[v * 3];
						cy += mesh.pos[v * 3 + 1];
						cz += mesh.pos[v * 3 + 2];
					}
					if (vertCount != 0) {
						const float n = static_cast<float>(vertCount);
						cx /= n;
						cy /= n;
						cz /= n;
					}
					double dotSum = 0.0;
					double areaSum = 0.0;
					for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
						const auto i0 = mesh.indices[t];
						const auto i1 = mesh.indices[t + 1];
						const auto i2 = mesh.indices[t + 2];
						if (i0 >= vertCount || i1 >= vertCount || i2 >= vertCount) {
							continue;
						}
						const float ax = mesh.pos[i1 * 3] - mesh.pos[i0 * 3];
						const float ay = mesh.pos[i1 * 3 + 1] - mesh.pos[i0 * 3 + 1];
						const float az = mesh.pos[i1 * 3 + 2] - mesh.pos[i0 * 3 + 2];
						const float bx = mesh.pos[i2 * 3] - mesh.pos[i0 * 3];
						const float by = mesh.pos[i2 * 3 + 1] - mesh.pos[i0 * 3 + 1];
						const float bz = mesh.pos[i2 * 3 + 2] - mesh.pos[i0 * 3 + 2];
						const double fx = static_cast<double>(ay) * bz - static_cast<double>(az) * by;
						const double fy = static_cast<double>(az) * bx - static_cast<double>(ax) * bz;
						const double fz = static_cast<double>(ax) * by - static_cast<double>(ay) * bx;
						const double len = std::sqrt(fx * fx + fy * fy + fz * fz);
						if (len <= 1e-12) {
							continue;
						}
						const double mx = (static_cast<double>(mesh.pos[i0 * 3]) + mesh.pos[i1 * 3] + mesh.pos[i2 * 3]) / 3.0 - cx;
						const double my = (static_cast<double>(mesh.pos[i0 * 3 + 1]) + mesh.pos[i1 * 3 + 1] + mesh.pos[i2 * 3 + 1]) / 3.0 - cy;
						const double mz = (static_cast<double>(mesh.pos[i0 * 3 + 2]) + mesh.pos[i1 * 3 + 2] + mesh.pos[i2 * 3 + 2]) / 3.0 - cz;
						const double dlen = std::sqrt(mx * mx + my * my + mz * mz);
						if (dlen <= 1e-9) {
							continue;
						}
						// Area weighting is the length: a face's say in the answer
						// is its size, exactly like the smooth-normal pass below.
						dotSum += ((fx * mx + fy * my + fz * mz) / (len * dlen)) * len;
						areaSum += len;
					}
					meshInward = areaSum > 0.0 ? static_cast<float>(dotSum / areaSum) : 0.0f;
				}
				// The authored pattern is a detail sheet, not an atlas. Its mesh's
				// UV island sits in a corner of the 1024 map - the wine's shell is
				// u=[0.010,0.262] v=[0.749,0.997], a quarter of the texture each
				// way - and sampling that straight draws the web four times too
				// large: the 2026-10-23 capture shows two or three cells across the
				// body where the game shows five or six, with strands as thick as
				// the cells. The island is square while the bottle's circumference
				// and body height are within a few percent of each other (measured
				// on the in-game capture), so mapping it onto [0,1] keeps the
				// sheet's own texel density on the surface. The map is a tiling
				// mask - white cells under a black web - so this repeats it; it
				// does not cut the island up.
				bool patternMesh = false;
				float patternUvLo[2]{ 0.0f, 0.0f };
				float patternUvScale[2]{ 1.0f, 1.0f };
				{
					std::string patternProbe;
					patternMesh = FindAuthoredPatternMap(mesh.matPath, patternProbe) && hasUv;
					if (patternMesh) {
						float ulo = 1e30f, uhi = -1e30f, vlo = 1e30f, vhi = -1e30f;
						for (std::size_t q = 0; q + 1 < mesh.uv.size(); q += 2) {
							ulo = std::min(ulo, mesh.uv[q]);
							uhi = std::max(uhi, mesh.uv[q]);
							vlo = std::min(vlo, mesh.uv[q + 1]);
							vhi = std::max(vhi, mesh.uv[q + 1]);
						}
						if (uhi - ulo > 1e-4f && vhi - vlo > 1e-4f) {
							patternUvLo[0] = ulo;
							patternUvLo[1] = vlo;
							patternUvScale[0] = 1.0f / (uhi - ulo);
							patternUvScale[1] = 1.0f / (vhi - vlo);
							REX::INFO("SFShopExplorer offline GPU: pattern '{}' island u=[{:.3f},{:.3f}] v=[{:.3f},{:.3f}] -> x{:.2f}/x{:.2f}",
								mesh.matPath, ulo, uhi, vlo, vhi, patternUvScale[0], patternUvScale[1]);
						} else {
							patternMesh = false;
						}
					}
				}
				for (std::size_t v = 0; v < vertCount; ++v) {
					float lx = (mesh.pos[v * 3] - a_model.center[0]) * inv;
					float ly = (mesh.pos[v * 3 + 1] - a_model.center[1]) * inv;
					float lz = (mesh.pos[v * 3 + 2] - a_model.center[2]) * inv;
					if (authoredOrientation) {
						// Geometry already carries the form's authored rotation; any
						// guess on top of it would double-orient the item.
					} else if (a_weapon) {
						const float tx = -ly, ty = lz, tz = -lx;
						lx = tx;
						ly = ty;
						lz = tz;
					} else if (layFlat) {
						const float t = lx;
						lx = ly;
						ly = -t;
					}
					pos.push_back(lx);
					pos.push_back(ly);
					pos.push_back(lz);
					uvs.push_back(hasUv ? (patternMesh ? (mesh.uv[v * 2] - patternUvLo[0]) * patternUvScale[0]
						: mesh.uv[v * 2]) : 0.0f);
					uvs.push_back(hasUv ? (patternMesh ? (mesh.uv[v * 2 + 1] - patternUvLo[1]) * patternUvScale[1]
						: mesh.uv[v * 2 + 1]) : 0.0f);
				}
				GpuDrawRange range;
				range.start = static_cast<std::uint32_t>(idx.size());
				for (const auto id : mesh.indices) {
					if (id < vertCount) {
						idx.push_back(baseV + id);
					}
				}
				range.count = static_cast<std::uint32_t>(idx.size()) - range.start;
				if (range.count < 3) {
					pos.resize(static_cast<std::size_t>(baseV) * 3);
					uvs.resize(static_cast<std::size_t>(baseV) * 2);
					continue;
				}				// A textureless material carries its base colour in its
				// name; prefer that over the index-cycled grey. Only the
				// name can say so here - whether the material ends up
				// textureless is not known until the prefetch lands, and
				// UploadModelTextures resets the colour to white for
				// anything that does get an albedo.
				bool nameTint = FlatBaseColor(mesh.matPath, base);
				// The database's own override tint beats a guess at the
				// last token of the material name (6r-7): 4,014 materials
				// have no albedo map at all yet carry a visibly tinted
				// MaterialOverrideColor, and were rendering as a neutral
				// grey blob or a wrong name word. Only reported when the
				// max channel is below 0.97, so a near-white override
				// cannot flatten a material that would otherwise keep a
				// good name-derived colour.
				float tint[3]{ 1.0f, 1.0f, 1.0f };
				if (!breachStock && LiveModel::FindCdbTint(mesh.matPath, tint)) {
					const auto spreadOf = [](const float a[3]) {
						return std::max({ a[0], a[1], a[2] }) - std::min({ a[0], a[1], a[2] });
					};
					// A neutral NAME beats a chromatic override on the same material.
					// Measured on the 2026-10-23 session: every snow globe's base is
					// `PlasticPlainTextured02Black`, whose own database record is
					// 0.198 grey and whose name says Black, yet this build logged it
					// painted (0.43,0.36,0.32) - a warm tan. That exact triple exists
					// in materialsbeta.cdb under ONE object only,
					// `Akila_CabinetGlass_Swap_GridGrey_Material1` (0.433,0.361,
					// 0.324), so the value reaching a plastic is somebody else's
					// colour; it is what made every globe base brown where the
					// engine's own render is dark neutral. The rule is the name: a
					// material named black/white/grey/silver is neutral by
					// definition, so a tint with visible chroma (>0.06 spread) is
					// not describing it. Chromatic names (Velocity_Beverage_Yellow,
					// Crafted_InHailerYellow) keep their database colour untouched.
					const bool neutralName = nameTint && spreadOf(base) <= 0.02f;
					if (neutralName && spreadOf(tint) > 0.06f) {
						REX::INFO("SFShopExplorer offline GPU: chromatic override ({:.2f},{:.2f},{:.2f}) rejected for neutral-named '{}' "
							"(keeping ({:.2f},{:.2f},{:.2f}))", tint[0], tint[1], tint[2], mesh.matPath, base[0], base[1], base[2]);
					} else {
						base[0] = tint[0];
						base[1] = tint[1];
						base[2] = tint[2];
						nameTint = false;  // the database's value beats the name word
						++cdbTintCandidates;
					// Remembered because the albedo upload normally resets
					// the colour to white. For the crafted drink bags that
					// reset is wrong: the database's override is the only
					// place the tea's real colour is written down. Alien
					// Tea's CraftedFood_AlienTea.mat carries (0.426, 0.426,
					// 0.426) - an even darkening the game applies and this
					// preview dropped, which left the liquid at its full
					// atlas colour and far brighter than the inventory
					// reference (measured there at (62,38,38) against the
					// atlas's (114,73,1)). Scoped to the folder: applying
					// every override would re-tint the whole catalogue off
					// a single measured case.
					range.cdbOverrideTint = IsCraftedDrinkBagMaterial(mesh.matPath);
					// A paint colour is the override when the material is a flat
					// painted surface: `PaintedMetalShine01*` and the ammo crates
					// name a SHARED neutral paint map (paint01_color, measured
					// (197,198,197)) and carry the colour only in the database.
					// The upload keeps it only if the resolved map measures as a
					// neutral base, so a map that already has its own colour is
					// never multiplied twice.
					//
					// A layered composite weight is NOT a paint colour: the
					// Razorback receiver carries (0.67,0.697,1.0,0.635) with kind
					// "Multiply", and applying that would tint a solid rifle.
					// Only kind 0 (no layer record) qualifies.
					float cdbAlpha = 1.0f;
					std::uint8_t cdbKind = 0;
					(void)LiveModel::FindCdbAlpha(mesh.matPath, cdbAlpha, &cdbKind);
					range.cdbPaintTint = cdbKind == 0;
					if (range.cdbPaintTint && IsAmmoCratePart(mesh.matPath)) {
						// The lid keeps its OWN authored colour. `AmmoLaser.nif`
						// names `PaintedMetalShine01_Green`, and that object stores
						// (0.584,0.800,0.320); this pipe's linear->sRGB conversion
						// turns it into (201,231,153), which is the measured LZR lid
						// (202,229,162 top, 98,136,65 front). An earlier build
						// rebound the lid to `AmmoBoxPrimer_Lid` because a scan
						// value-first parse had given that object Green's colour; the
						// primer lid actually stores pale blue (0.600,0.698,0.881)
						// and the remap would have painted the lime lid blue.
						// A crate part multiplies only its SHADE into the neutral
						// paint. The pigment this cut was built for,
						// `AmmoBoxLarge_DarkGreen01`, is a Lerp layer (kind 2) and is
						// already excluded above, so the cut is now a backstop rather
						// than the rule. It sits at 0.60 because the corrected values
						// changed shape: .27's cream lid is
						// `PaintedMetalshine01_Orange` at (0.750,0.609,0.188), spread
						// 0.562, and the old 0.50 rejected it once the database was
						// read name-first. No other ammobox01 material sits between
						// the old and new cut, so nothing else moves.
						const float mx = std::max({ tint[0], tint[1], tint[2] });
						const float mn = std::min({ tint[0], tint[1], tint[2] });
						range.cdbPaintTint = mx - mn <= 0.60f;
					}
					}
				}
				if (breachStock) {
					base[0] = 0.60f;
					base[1] = 0.62f;
					base[2] = 0.64f;
				}
				range.color[0] = base[0];
				range.color[1] = base[1];
				range.color[2] = base[2];
				range.flatNameTint = nameTint;
				range.matPath = mesh.matPath;					range.nodeName = mesh.name;
				range.forceNeutralGrey = breachStock;
				range.buildInward = meshInward;
				{
					// Bounds of this range in normalised model space, taken from
					// the vertices just pushed: the hull test needs to know how
					// much of the model the range covers, not just its shape.
					for (int a = 0; a < 3; ++a) {
						range.buildMin[a] = 1e30f;
						range.buildMax[a] = -1e30f;
					}
					for (std::size_t v = baseV; v < pos.size() / 3; ++v) {
						for (int a = 0; a < 3; ++a) {
							range.buildMin[a] = std::min(range.buildMin[a], pos[v * 3 + static_cast<std::size_t>(a)]);
							range.buildMax[a] = std::max(range.buildMax[a], pos[v * 3 + static_cast<std::size_t>(a)]);
						}
					}
				}
				range.effectOverlay = IsEffectOverlayMaterial(mesh.matPath);
				range.additiveShell = IsAdditiveShellMaterial(mesh.matPath);
				range.translucent = false;
				if (hasUv) {
					const std::size_t sampleTris = mesh.indices.size() / 3;
					const std::size_t step = sampleTris > kMaxUvSamples ? sampleTris / kMaxUvSamples : 1;
					for (std::size_t t = 0; t < sampleTris && range.uvSamples.size() < kMaxUvSamples * 2; t += step) {
						float su = 0.0f;
						float sv = 0.0f;
						bool ok = true;
						for (int k = 0; k < 3; ++k) {
							const auto vi = mesh.indices[t * 3 + static_cast<std::size_t>(k)];
							if (vi >= vertCount) {
								ok = false;
								break;
							}
							su += mesh.uv[vi * 2];
							sv += mesh.uv[vi * 2 + 1];
						}
						if (!ok) {
							break;
						}
						range.uvSamples.push_back(su / 3.0f);
						range.uvSamples.push_back(sv / 3.0f);
					}
				}

				range.alpha = MaterialBlendAlpha(mesh.matPath, range.translucent, range.filmCoverage);
				{
					// An authored pattern is the body itself, not a film over a
					// clear shell: the Sparkling Wine's web is printed on an opaque
					// white bottle in the game, so this range leaves the blend pass.
					std::string patternMap;
					if (FindAuthoredPatternMap(mesh.matPath, patternMap)) {
						range.translucent = false;
						range.alpha = 1.0f;
						range.color[0] = 1.0f;
						range.color[1] = 1.0f;
						range.color[2] = 1.0f;
					}
				}
			g_gpuModel.ranges.push_back(range);
			if (g_gpuModel.ranges.size() >= kMaxGpuRanges) {
				return;
			}
		}
		};
		accumulate(true);
		if (g_gpuModel.ranges.empty()) {
			REX::INFO("SFShopExplorer offline GPU: '{}' below size filter, rendering unfiltered", a_key);
			accumulate(false);
		}
		// Interior-shell rule. Every snow globe hangs its specimen inside a
		// backdrop hull, and that hull is authored to be seen from the inside:
		// measured on the shipped London NIF its faces point at the model's
		// centre (mean cosine -0.99), it covers the dome (radius 0.101 against
		// the dome's 0.103), and it wraps the buildings, snow, pad and trees.
		//
		// This renderer rasterises CULL_MODE_NONE, so that hull was drawn with
		// its NEAR wall in front of the specimen, and the 2026-10-23 capture is
		// exactly that: a painted sky dome with no buildings, London Eye or
		// trees inside it - the surface the user reads as opaque glass is the
		// near wall of the backdrop, not the glass at all. The engine draws the
		// inside of the hull. So a range that is inside-out and wraps the rest
		// of the model is the one range that takes the back-face-culled
		// pipeline: the near wall goes, the far wall stays behind everything as
		// the sky. Verified offline on the shipped mesh: with the culling the
		// London globe renders its interior against the sky; without it, the
		// painted dome alone (reports/globe-test-2026-10-23/backdrop-cull.png).
		for (std::size_t r = 0; r < g_gpuModel.ranges.size(); ++r) {
			GpuDrawRange& shell = g_gpuModel.ranges[r];
			if (shell.translucent || shell.buildInward > -0.5f) {
				continue;
			}
			const float halfX = (shell.buildMax[0] - shell.buildMin[0]) * 0.5f;
			const float halfY = (shell.buildMax[1] - shell.buildMin[1]) * 0.5f;
			const float halfZ = (shell.buildMax[2] - shell.buildMin[2]) * 0.5f;
			// The model is normalised to r=1, so half an extent is the share of
			// the item this range spans. A hull covers most of it; a concave
			// inner wall (a mug's inside, a bowl) does not.
			if (std::max({ halfX, halfY, halfZ }) < 0.5f) {
				continue;
			}
			std::uint32_t wrapped = 0;
			for (std::size_t o = 0; o < g_gpuModel.ranges.size(); ++o) {
				if (o == r) {
					continue;
				}
				const GpuDrawRange& other = g_gpuModel.ranges[o];
				const float cx = (other.buildMin[0] + other.buildMax[0]) * 0.5f;
				const float cy = (other.buildMin[1] + other.buildMax[1]) * 0.5f;
				const float cz = (other.buildMin[2] + other.buildMax[2]) * 0.5f;
				if (cx > shell.buildMin[0] && cx < shell.buildMax[0] &&
					cy > shell.buildMin[1] && cy < shell.buildMax[1] &&
					cz > shell.buildMin[2] && cz < shell.buildMax[2]) {
					++wrapped;
				}
			}
			// Two is enough to separate a hull from a part that merely faces
			// inward: the London globe wraps eight.
			if (wrapped < 2) {
				continue;
			}
			shell.interiorShell = true;
			REX::INFO("SFShopExplorer offline GPU: interior shell '{}' node '{}' (faces inward {:.2f}, wraps {} range(s)) -> near wall culled",
				shell.matPath, shell.nodeName, shell.buildInward, wrapped);
		}
		if (g_gpuModel.ranges.empty() || pos.empty() || idx.empty()) {
			ReleaseGpuModel();
			return false;
		}
		// Counted here, not inside the mesh loop: accumulate() may run twice
		// (filtered, then unfiltered fallback) and an in-loop tally would
		// double-count. One line per model, so the translucency pass is
		// provable from the session log without flooding it.
		{
			std::uint32_t translucent = 0;
			std::uint32_t film = 0;
			std::string firstMat;
			std::string firstFilm;
			std::uint32_t liquids = 0;
			std::string firstLiquid;
			for (const auto& r : g_gpuModel.ranges) {
				if (r.translucent) {
					if (translucent == 0) {
						firstMat = r.matPath;
					}
					++translucent;
				}
				if (r.filmCoverage) {
					if (film == 0) {
						firstFilm = r.matPath;
					}
					++film;
				}
				if (r.liquidCoverage) {
					if (liquids == 0) {
						firstLiquid = r.matPath;
					}
					++liquids;
				}
			}
			// The film path is only real if the map was uploaded, so say whether
			// the range can actually blend; a film count with no matching
			// "blended with its map" line would mean the pouch is back to opaque.
			if (film != 0) {
				REX::INFO("SFShopExplorer offline GPU: '{}' {} film range(s) blend with their opacity map (e.g. '{}')",
					a_key, film, firstFilm);
			}
			if (liquids != 0) {
				REX::INFO("SFShopExplorer offline GPU: '{}' {} drink fill(s) take their coverage from the map (e.g. '{}')",
					a_key, liquids, firstLiquid);
			}
			if (translucent != 0) {
				REX::INFO("SFShopExplorer offline GPU: '{}' translucent ranges {} of {} (e.g. '{}')",
					a_key, translucent, g_gpuModel.ranges.size(), firstMat);
			}
			if (cdbTintCandidates != 0) {
				REX::INFO("SFShopExplorer offline GPU: '{}' {} range(s) have a CDB override tint candidate",
					a_key, cdbTintCandidates);
			}
		}
			// Smooth normals: area-weighted face-normal accumulation, then
			// normalize. Uniform-rotation model matrix keeps them valid.
			const std::size_t nv = pos.size() / 3;
			std::vector<float> nrm(nv * 3, 0.0f);
			for (std::size_t t = 0; t + 2 < idx.size(); t += 3) {
				const auto i0 = idx[t], i1 = idx[t + 1], i2 = idx[t + 2];
				if (i0 >= nv || i1 >= nv || i2 >= nv) {
					continue;
				}
				const float ax = pos[i1 * 3] - pos[i0 * 3];
				const float ay = pos[i1 * 3 + 1] - pos[i0 * 3 + 1];
				const float az = pos[i1 * 3 + 2] - pos[i0 * 3 + 2];
				const float bx = pos[i2 * 3] - pos[i0 * 3];
				const float by = pos[i2 * 3 + 1] - pos[i0 * 3 + 1];
				const float bz = pos[i2 * 3 + 2] - pos[i0 * 3 + 2];
				const float fnx = ay * bz - az * by;
				const float fny = az * bx - ax * bz;
				const float fnz = ax * by - ay * bx;
				nrm[i0 * 3] += fnx;
				nrm[i0 * 3 + 1] += fny;
				nrm[i0 * 3 + 2] += fnz;
				nrm[i1 * 3] += fnx;
				nrm[i1 * 3 + 1] += fny;
				nrm[i1 * 3 + 2] += fnz;
				nrm[i2 * 3] += fnx;
				nrm[i2 * 3 + 1] += fny;
				nrm[i2 * 3 + 2] += fnz;
			}
			// Interleave pos/uv/normal (8 floats) for the vertex buffer.
			std::vector<float> verts;
			verts.reserve(nv * 8);
			for (std::size_t v = 0; v < nv; ++v) {
				verts.push_back(pos[v * 3]);
				verts.push_back(pos[v * 3 + 1]);
				verts.push_back(pos[v * 3 + 2]);
				verts.push_back(uvs[v * 2]);
				verts.push_back(uvs[v * 2 + 1]);
				float nx = nrm[v * 3], ny = nrm[v * 3 + 1], nz = nrm[v * 3 + 2];
				const float l = std::sqrt(nx * nx + ny * ny + nz * nz);
				if (l > 1e-12f) {
					nx /= l;
					ny /= l;
					nz /= l;
				} else {
					nx = 0.0f;
					ny = 0.0f;
					nz = 1.0f;
				}
				verts.push_back(nx);
				verts.push_back(ny);
				verts.push_back(nz);
			}
			const auto vbBytes = verts.size() * sizeof(float);
			const auto ibBytes = idx.size() * sizeof(std::uint32_t);
			D3D12_RESOURCE_DESC desc{};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			desc.Height = 1;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.Format = DXGI_FORMAT_UNKNOWN;
			desc.SampleDesc.Count = 1;
			desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			D3D12_HEAP_PROPERTIES heap{};
			heap.Type = D3D12_HEAP_TYPE_UPLOAD;
			desc.Width = vbBytes;
			if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
					D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_gpuModel.vb)))) {
				ReleaseGpuModel();
				return false;
			}
			desc.Width = ibBytes;
			if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
					D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_gpuModel.ib)))) {
				ReleaseGpuModel();
				return false;
			}
			void* dst = nullptr;
			D3D12_RANGE noRead{ 0, 0 };
			if (FAILED(g_gpuModel.vb->Map(0, &noRead, &dst))) {
				ReleaseGpuModel();
				return false;
			}
			std::memcpy(dst, verts.data(), vbBytes);
			g_gpuModel.vb->Unmap(0, nullptr);
			if (FAILED(g_gpuModel.ib->Map(0, &noRead, &dst))) {
				ReleaseGpuModel();
				return false;
			}
			std::memcpy(dst, idx.data(), ibBytes);
			g_gpuModel.ib->Unmap(0, nullptr);
			g_gpuModel.key = a_key;
			g_gpuModel.vbBytes = static_cast<std::uint32_t>(vbBytes);
			g_gpuModel.ibCount = static_cast<std::uint32_t>(idx.size());
			return true;
		}

		void Barrier(NativeList* a_list, ::ID3D12Resource* a_res, int a_before, int a_after)
		{
			D3D12_RESOURCE_BARRIER b{};
			b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b.Transition.pResource = a_res;
			b.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(a_before);
			b.Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(a_after);
			b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			a_list->ResourceBarrier(1, &b);
		}

		// Texture prefetch (the gun-rotation fix): resolving a diffuse means
		// scanning ~100 BA2 archives and reading+decompressing texture
		// chunks â€” seconds of file I/O that must NEVER run on the Present
		// thread (a 28-mesh rifle froze Presents solid: "guns don't rotate").
		// The task thread warms a memory cache per model key; the render
		// thread only ever creates GPU objects from memory, and renders
		// plain until the prefetch lands (completion re-arms a frame, so
		// colors pop in on their own). Render thread takes g_texMutex only â€”
		// never the LiveModel I/O mutex, never the filesystem.
		struct PrefetchedTex
		{
			std::string matPath;    // matched against GpuDrawRange at upload
			std::string nodeName;   // (range indices diverge: tiny meshes are
			std::uint32_t format{ 0 };  // skipped when ranges are built, so no
			std::uint32_t width{ 0 };   // positional coupling is possible)
			std::uint32_t height{ 0 };
			std::uint32_t mipLevels{ 1 };
			std::size_t rowPitch{ 0 };
			std::vector<std::uint8_t> bytes;
			std::uint32_t opacityFormat{ 0 };
			std::uint32_t opacityWidth{ 0 };
			std::uint32_t opacityHeight{ 0 };
			std::uint32_t opacityMipLevels{ 1 };
			std::size_t opacityRowPitch{ 0 };
			std::vector<std::uint8_t> opacityBytes;
			float metal{ 0.0f };
			float rough{ 0.5f };
			// The map this range actually resolved to. UploadModelTextures
			// compares it with the material's own CDB set: a stand-in map must
			// not be allowed to throw the authored override colour away.
			std::string colorMap;
			// The mask this range bound, by path. UploadModelTextures compares
			// it across the model: a decal whose only map is another part's
			// cutout is a leftover print, not this range's artwork.
			std::string opacityMap;
			std::string colorStatus;
			std::string opacityStatus;
			std::uint32_t detailFormat{ 0 };
			std::uint32_t detailWidth{ 0 };
			std::uint32_t detailHeight{ 0 };
			std::uint32_t detailMipLevels{ 1 };
			std::size_t detailRowPitch{ 0 };
			std::vector<std::uint8_t> detailBytes;
			bool woodEmboss{ false };
			bool razorbackGripBadge{ false };
			bool crucibleEmissive{ false };
			bool vortexGrenadeEmissive{ false };
			// The albedo IS the alpha mask (promoted cutout). Kept so the
			// upload does not reset the range to pure white and lose the
			// base colour the material name carried.
			bool maskCutout{ false };
			// ...and the shader has to know, so it can read that single-channel
			// map as a white body instead of as (r,0,0).
			bool promotedMask{ false };
			// The albedo is an authored pattern mask bound as ink, not coverage:
			// see kAuthoredPattern. Kept apart from promotedMask because the two
			// draw differently - a promoted cutout is clipped by its own mask,
			// a pattern is the surface itself.
			bool patternInk{ false };
			// The material's BC5 `_normal` companion. Relief is not decoration:
			// the Erdebrau can's pull ring exists ONLY here (its lid is a flat
			// 16-gon plate, 110 triangles, and the decal atlas' lid island is
			// plain foil), so the preview could never draw one without it.
			std::uint32_t normalFormat{ 0 };
			std::uint32_t normalWidth{ 0 };
			std::uint32_t normalHeight{ 0 };
			std::uint32_t normalMipLevels{ 1 };
			std::size_t normalRowPitch{ 0 };
			std::vector<std::uint8_t> normalBytes;
		};
		std::mutex g_texMutex;
		std::unordered_map<std::string, std::vector<PrefetchedTex>> g_texCache;
		std::unordered_map<std::string, std::vector<MaterialDiagnostic>> g_materialDiagnostics;
		std::unordered_set<std::string> g_texReady;
		std::string g_prefetchReq;  // model key the task thread should warm

		// The plugin log is an async spdlog sink and it is dropping exactly the
		// prefetch lines needed to explain a flat preview: REX::INFO calls that
		// sit at the head of a burst (function entry, per-material verdict,
		// summary) never reach the file, while lines emitted a frame later do.
		// This trace bypasses the logger: plain append to %TEMP%, readable
		// after the session without a debugger.
		void PrefetchTrace(const std::string& a_line)
		{
			static std::mutex s_traceMutex;
			std::scoped_lock lock(s_traceMutex);
			static const std::filesystem::path s_path =
				std::filesystem::temp_directory_path() / "SFShopExplorer-prefetch.txt";
			std::ofstream out(s_path, std::ios::app);
			if (out) {
				out << a_line << '\n';
			}
		}

		// Task thread: resolve + decode every diffuse for a model into memory.
		void PrefetchTextures(const Model& a_model, const std::string& a_key)
		{
			// Unconditional entry line: whatever path asks for textures, the
			// log names it before any early return. Without this, a request
			// that is never served and a request that is served-but-empty
			// look identical from outside (both leave the preview grey).
			REX::INFO("SFShopExplorer offline GPU: prefetch enter '{}' ({} meshes)",
				a_key, a_model.meshes.size());
			PrefetchTrace("enter '" + a_key + "' meshes=" + std::to_string(a_model.meshes.size()));
			std::vector<MaterialDiagnostic> cachedDiagnostics;
			bool alreadyReady = false;
			{
				std::scoped_lock lock(g_texMutex);
				alreadyReady = g_texReady.find(a_key) != g_texReady.end();
				if (alreadyReady) {
					if (const auto it = g_materialDiagnostics.find(a_key); it != g_materialDiagnostics.end()) {
						cachedDiagnostics = it->second;
					}
				}
			}
			if (alreadyReady) {
				// A cached empty answer means the first attempt for this key
				// failed and is never retried: name it, so a model that stays
				// flat says whether its textures were resolved once already.
				{
					std::size_t cachedTextures = 0;
					std::scoped_lock lock(g_texMutex);
					const auto it = g_texCache.find(a_key);
					if (it != g_texCache.end()) {
						cachedTextures = it->second.size();
					}
					REX::INFO("SFShopExplorer offline GPU: prefetch '{}' skipped ({} textures cached)",
						a_key, cachedTextures);
				}
				{
					std::scoped_lock snapshotLock(g_snapshotMutex);
					if (g_snapshot.model == a_key) {
						g_snapshot.materials = std::move(cachedDiagnostics);
					}
				}
				g_orbitDirty.store(true, std::memory_order_release);
				return;
			}
			std::vector<PrefetchedTex> out;
			std::vector<MaterialDiagnostic> diagnostics;
			diagnostics.reserve(a_model.meshes.size());
			{
				std::string mats;
				for (const auto& m : a_model.meshes) {
					mats += "'";
					mats += m.matPath;
					mats += "' ";
				}
				REX::INFO("SFShopExplorer offline GPU: prefetch '{}' ({} meshes) mats=[{}]",
					a_key, a_model.meshes.size(), mats);
			}
			for (const auto& mesh : a_model.meshes) {
				MaterialDiagnostic diagnostic;
				diagnostic.mesh = mesh.name;
				diagnostic.material = mesh.matPath;
				if (out.size() >= kMaxGpuTextures) {
					PrefetchTrace("  limit mat='" + mesh.matPath + "'");
					diagnostic.colorStatus = "not resolved (texture limit)";
					diagnostics.push_back(std::move(diagnostic));
					continue;
				}
				if (mesh.matPath.empty()) {
					PrefetchTrace("  nomat node='" + mesh.name + "'");
					diagnostic.colorStatus = "no material path";
					diagnostics.push_back(std::move(diagnostic));
					REX::INFO("SFShopExplorer offline GPU: mesh '{}' has no material path", mesh.name);
					continue;
				}
				// Exact first: the .mat JSON's Albedo File (mod materials
				// borrow cross-family maps no fuzzy matcher can guess).
				// FindDiffuse then resolves base-game materials through the
				// shipped material database before it ever guesses, and can
				// hand back that set's companion opacity map.
				std::string ddsKey;
				std::string opacityKey;
				int matchScore = 0;
				bool exact = false;
				// A material whose shipped texture set names only an
				// opacity map IS that mask in the game: the supply-container
				// icon and letter sheets are pure cutouts. Rendering the
				// mask as the albedo gives a tinted shape clipped by the
				// alpha pipeline, which is what puts the medical cross and
				// the small text back on the crate. No name test is needed:
				// a set with no colour map but a mask can only mean cutout.
				bool promotedMask = false;
				bool patternInk = false;
				bool razorbackGripDecal = false;
				bool crucibleBladeMaterial = false;
				bool vortexGrenadeMaterial = false;
				std::string normalizedMat = LowerAscii(mesh.matPath);
				std::replace(normalizedMat.begin(), normalizedMat.end(), static_cast<char>(92), '/');
				{
					// The Razorback grip and the barrel markings share one .mat
					// path. Resolve the grip's standard wood atlas before consulting
					// a loose .mat Albedo; its separate Fancy color map is sampled
					// below only as a badge artwork overlay.
					crucibleBladeMaterial = normalizedMat == "materials/weapons/kgcdoom/crucible/crucible_edge_flow.mat" ||
						normalizedMat == "materials/weapons/kgcdoom/crucible/crucible_test_flicker.mat";
					vortexGrenadeMaterial = normalizedMat == "materials/weapons/dlc001/vortexgrenade/vortexgrenade.mat" ||
						normalizedMat == "materials/weapons/dlc001/vortexgrenade/vortexgrenade_binding.mat" ||
						normalizedMat == "materials/weapons/dlc001/vortexgrenade/vortexgrenade_charged.mat" ||
						normalizedMat == "materials/weapons/dlc001/vortexgrenade/vortexgrenade_lure.mat" ||
						normalizedMat == "materials/weapons/dlc001/vortexgrenade/vortexgrenade_phasing.mat";
					razorbackGripDecal =
						normalizedMat == "materials/weapons/razorback/decals.mat" &&
						LowerAscii(mesh.name).find("decals_grip") != std::string::npos;
					std::string albedo;
					if (razorbackGripDecal &&
						LiveModel::FindDiffuse(mesh.matPath, mesh.name, ddsKey, &matchScore, &opacityKey)) {
						// FindDiffuse's exact grip rule returns the standard wood
						// atlas and its coverage mask. It is not a .mat File result,
						// so leave exact=false: an unreadable map must not be mistaken
						// for an authored file.
					} else if (vortexGrenadeMaterial) {
						// All five DLC variants share an authored atlas, despite the
						// .mat's generic/stale Albedo file. Force the exact route.
						if (!LiveModel::FindDiffuse(mesh.matPath, mesh.name, ddsKey, &matchScore, &opacityKey)) {
							diagnostic.colorStatus = "flat (no authored Vortex atlas)";
							diagnostics.push_back(std::move(diagnostic));
							continue;
						}
					} else if (FindAuthoredPatternMap(mesh.matPath, albedo)) {
						// The authored Blender pattern: bound as the albedo, shaded as
						// ink (mode 11). Not a .mat result, so leave exact=false.
						ddsKey = albedo;
						patternInk = true;
						matchScore = -12;
					} else if (LiveModel::FindMatAlbedo(mesh.matPath, albedo)) {
						ddsKey = albedo;
						matchScore = -10;
						exact = true;
					} else if (!LiveModel::FindDiffuse(mesh.matPath, mesh.name, ddsKey, &matchScore, &opacityKey)) {
						if (opacityKey.empty()) {
							PrefetchTrace("  nocolour mat='" + mesh.matPath + "'");
							diagnostic.colorStatus = "flat (no albedo in material set)";
							diagnostics.push_back(std::move(diagnostic));
							REX::INFO("SFShopExplorer offline GPU: no colour map for '{}' node '{}'",
								mesh.matPath, mesh.name);
							continue;  // flat textureless: the range keeps its base colour
						}
						ddsKey = opacityKey;
						promotedMask = true;
						matchScore = -11;
					}
				}

				LiveModel::TexImage img;
				diagnostic.colorMap = ddsKey;
				if (!LiveModel::ReadTexture(ddsKey, img) || img.bytes.empty() || img.format == 0) {
					PrefetchTrace("  unreadable mat='" + mesh.matPath + "' tex='" + ddsKey + "'");
					diagnostic.colorStatus = "missing/unreadable";
					if (exact) {
						// Exact ref unreadable (missing archive, bad data):
						// one fuzzy retry before giving up on the mesh.
						exact = false;
						if (!LiveModel::FindDiffuse(mesh.matPath, mesh.name, ddsKey, &matchScore) ||
							!LiveModel::ReadTexture(ddsKey, img) || img.bytes.empty() || img.format == 0) {
							diagnostic.colorMap = ddsKey;
							diagnostics.push_back(std::move(diagnostic));
							REX::WARN("SFShopExplorer offline GPU: texture unreadable for {} ({})", mesh.matPath, ddsKey);
							continue;
						}
					} else {
						diagnostics.push_back(std::move(diagnostic));
						REX::WARN("SFShopExplorer offline GPU: texture unreadable for {} ({})", mesh.matPath, ddsKey);
						continue;
					}
				}
				// Never sample a mask as diffuse. Only the deliberate
				// promotion above is allowed to reach this point with one.
				diagnostic.colorFormat = img.format;
				diagnostic.colorWidth = img.width;
				diagnostic.colorHeight = img.height;
				if (!promotedMask && !patternInk && IsSingleChannelFormat(img.format)) {
					PrefetchTrace("  rejected mat='" + mesh.matPath + "' tex='" + ddsKey +
						"' fmt=" + std::to_string(img.format));
					diagnostic.colorStatus = "rejected single-channel albedo";
					diagnostics.push_back(std::move(diagnostic));
					REX::WARN("SFShopExplorer offline GPU: rejected single-channel map as albedo {} "
						"(fmt={}) for {}", ddsKey, img.format, mesh.matPath);
					continue;
				}
				diagnostic.colorStatus = patternInk ? "authored pattern ink" :
					(promotedMask ? "promoted opacity cutout" :
						(exact ? "exact material albedo" : "resolved color map"));
				diagnostic.colorMap = ddsKey;
				PrefetchedTex t;
				t.matPath = mesh.matPath;
				t.nodeName = mesh.name;
				t.colorMap = ddsKey;
				t.patternInk = patternInk;
				if (patternInk) {
					REX::INFO("SFShopExplorer offline GPU: authored pattern '{}' <- {} (rendered as ink, not coverage)",
						mesh.matPath, ddsKey);
				}
				t.format = img.format;
					t.width = img.width;
					t.height = img.height;
					t.mipLevels = img.mipLevels;
					t.rowPitch = img.rowPitch;
				t.bytes = std::move(img.bytes);
				if (razorbackGripDecal) {
					// The Fancy color atlas contains the horse artwork at the grip
					// UVs, while its opacity atlas is blank there. Keep the standard
					// wood albedo and coverage; sample Fancy color as badge art.
					// ReadTexture runs on this task
					// thread, never on the Present/render thread.
					static const std::string kRazorbackGripRelief =
						"textures/weapons/razorback/fancy/decals_color.dds";
					LiveModel::TexImage detail;
					if (LiveModel::ReadTexture(kRazorbackGripRelief, detail) &&
						!detail.bytes.empty() && (detail.format == 71 || detail.format == 28) &&
						detail.width == img.width && detail.height == img.height) {
						t.detailFormat = detail.format;
						t.detailWidth = detail.width;
						t.detailHeight = detail.height;
						t.detailMipLevels = detail.mipLevels;
						t.detailRowPitch = detail.rowPitch;
						t.detailBytes = std::move(detail.bytes);
						t.razorbackGripBadge = true;
						REX::INFO("SFShopExplorer offline GPU: loaded Razorback grip badge artwork {}", kRazorbackGripRelief);
					} else {
						REX::WARN("SFShopExplorer offline GPU: Razorback grip badge artwork unreadable {}", kRazorbackGripRelief);
					}
				}
				if (vortexGrenadeMaterial) {
					static const std::string kVortexGrenadeEmissive =
						"textures/weapons/dlc001/vortextgrenade/lambert1_emissive.dds";
					LiveModel::TexImage emissive;
					if (LiveModel::ReadTexture(kVortexGrenadeEmissive, emissive) &&
						!emissive.bytes.empty() && (emissive.format == 71 || emissive.format == 28) &&
						emissive.width == img.width && emissive.height == img.height) {
						t.detailFormat = emissive.format;
						t.detailWidth = emissive.width;
						t.detailHeight = emissive.height;
						t.detailMipLevels = emissive.mipLevels;
						t.detailRowPitch = emissive.rowPitch;
						t.detailBytes = std::move(emissive.bytes);
						t.vortexGrenadeEmissive = true;
						REX::INFO("SFShopExplorer offline GPU: loaded Vortex Grenade emissive atlas {}", kVortexGrenadeEmissive);
					} else {
						REX::WARN("SFShopExplorer offline GPU: Vortex Grenade emissive atlas unreadable {}", kVortexGrenadeEmissive);
					}
				}
				if (crucibleBladeMaterial) {
					// Match the named material to its corresponding authored
					// emissive atlas; both in-game shaders animate it, but the
					// static contribution restores its red blade artwork here.
					const std::string kCrucibleBladeEmissive =
						normalizedMat.ends_with("crucible_edge_flow.mat") ?
							"textures/weapons/kgcdoom/crucible/crucible_blade_graid_emissive.dds" :
							"textures/weapons/kgcdoom/crucible/crucible_emissive.dds";
					LiveModel::TexImage emissive;
					if (LiveModel::ReadTexture(kCrucibleBladeEmissive, emissive) &&
						!emissive.bytes.empty() && (emissive.format == 71 || emissive.format == 28) &&
						emissive.width == img.width && emissive.height == img.height) {
						t.detailFormat = emissive.format;
						t.detailWidth = emissive.width;
						t.detailHeight = emissive.height;
						t.detailMipLevels = emissive.mipLevels;
						t.detailRowPitch = emissive.rowPitch;
						t.detailBytes = std::move(emissive.bytes);
						t.crucibleEmissive = true;
						REX::INFO("SFShopExplorer offline GPU: loaded Crucible blade emissive atlas {}", kCrucibleBladeEmissive);
					} else {
						REX::WARN("SFShopExplorer offline GPU: Crucible blade emissive atlas unreadable {}", kCrucibleBladeEmissive);
					}
				}
				const std::string lowerKey = LowerAscii(ddsKey);
				constexpr std::string_view kColorSuffix = "_color.dds";
				if (opacityKey.empty() && lowerKey.size() >= kColorSuffix.size() &&
					lowerKey.compare(lowerKey.size() - kColorSuffix.size(), kColorSuffix.size(), kColorSuffix) == 0) {
					opacityKey = ddsKey.substr(0, ddsKey.size() - kColorSuffix.size()) + "_opacity.dds";
				}
				if (!opacityKey.empty()) {
					diagnostic.opacityMap = opacityKey;
				}
				if (promotedMask) {
					diagnostic.opacityMap = ddsKey;
					diagnostic.opacityFormat = img.format;
					diagnostic.opacityWidth = img.width;
					diagnostic.opacityHeight = img.height;
					diagnostic.opacityStatus = "same map (promoted cutout)";
					// The promoted map is the mask by construction, so pair
					// it with itself instead of re-reading and re-judging it.
					// Copy from t, not img: img.bytes was already moved out.
					t.opacityFormat = t.format;
					t.opacityWidth = t.width;
					t.opacityHeight = t.height;
					t.opacityMipLevels = t.mipLevels;
					t.opacityRowPitch = t.rowPitch;
					t.opacityBytes = t.bytes;
					t.maskCutout = true;
					t.promotedMask = true;
					t.opacityMap = ddsKey;
				} else if (!opacityKey.empty()) {
					LiveModel::TexImage opacity;
					const bool read = LiveModel::ReadTexture(opacityKey, opacity);
					// BC4 is the single-channel mask the shipped sets use;
					// BC3 is accepted too because it carries the same mask
					// in its alpha channel. BC7 is accepted because texture
					// packs repack the same mask as a greyscale RGB sheet: the
					// 2026-10-24 Test profile ships the Erdebrau pilsner label
					// mask and the coolant jar's lid mask as BC7_UNORM_SRGB, and
					// rejecting the format cost both ranges their cutout (the
					// can printed its atlas' black surround, the jar stamped
					// the lid's mask as ink). Its ink is in red, like BC4 - see
					// opacityRedChannel below.
					const bool maskFmtOk = opacity.format == 80 || opacity.format == 77 ||
						opacity.format == 98;
					// UV sampling is resolution-independent, so the mask need
					// not match the colour map exactly - only its aspect
					// ratio. Requiring equality discarded 11 of the 259
					// colour/opacity pairs the material sweep found in a
					// 400-material sample (decalleavespineneedles01,
					// decalwet09, gl_dirt_light01, both UC Marine spacesuit
					// hand sets and others), costing those ranges their
					// cutout entirely. A smaller real mask remains usable: e.g.
					// the shipped 512x512 Magshear frame uses 128x128 opacity.
					// All audited 4x4 weapon masks share the same placeholder
					// block rather than authored coverage. Require at least 8x8;
					// valid downscaled masks such as Magshear's 128x128 still pass.
					constexpr std::uint32_t kMinAuthoredMaskDimension = 8;
					const bool aspectOk = t.height != 0 && opacity.height != 0 &&
						static_cast<std::uint64_t>(opacity.width) * t.height ==
							static_cast<std::uint64_t>(t.width) * opacity.height;
					const bool degenerateMask = opacity.width < kMinAuthoredMaskDimension ||
						opacity.height < kMinAuthoredMaskDimension;
					diagnostic.opacityFormat = opacity.format;
					diagnostic.opacityWidth = opacity.width;
					diagnostic.opacityHeight = opacity.height;
					if (read && maskFmtOk && aspectOk && !degenerateMask && !opacity.bytes.empty()) {
						diagnostic.opacityStatus = "paired";
						t.opacityFormat = opacity.format;
						t.opacityWidth = opacity.width;
						t.opacityHeight = opacity.height;
						t.opacityMipLevels = opacity.mipLevels;
						t.opacityRowPitch = opacity.rowPitch;
						t.opacityBytes = std::move(opacity.bytes);
						t.opacityMap = opacityKey;
					REX::INFO("SFShopExplorer direct GPU: paired opacity {} fmt={} channel={} ({}x{} vs colour {}x{})",
						opacityKey, opacity.format, opacity.format == 77 ? "alpha" : "red",
						opacity.width, opacity.height, t.width, t.height);
					} else {
						diagnostic.opacityStatus = !read ? "missing/unreadable" :
							(!maskFmtOk ? "unsupported mask format" :
							(!aspectOk ? "aspect mismatch" :
							(degenerateMask ? "degenerate <8x8 mask" : "empty mask data")));
						REX::INFO("SFShopExplorer direct GPU: opacity map rejected {} (fmt={} {}x{} vs {}x{})",
							opacityKey, opacity.format, opacity.width, opacity.height, t.width, t.height);
					}
				}
				if (diagnostic.opacityStatus.empty()) {
					diagnostic.opacityStatus = opacityKey.empty() ? "none (opaque)" : "not paired";
				}
				// Metalness and roughness for the opaque shading path, read from the
				// companions that sit beside the colour map. A starfield weapon is
				// mostly metal lit by a reflection, so sampling only its albedo made
				// the grenade read pale and flat where the game reads dark navy
				// (lambert1 mean metalness 0.789) and cost the Razorback its glossy
				// cylinder (0.759) and receiver (0.291). Painted surfaces come out
				// low-metal with high roughness on their own (BreechBlock's
				// receiver_2 is 0.163/0.708), which is the point: nothing is
				// inferred from the material's name.
				//
				// Decal sheets are skipped: a painted marking is not the metal it
				// sits on, and those ranges take the alpha shader anyway.
				if (!IsDecalSheet(mesh.matPath)) {
					const auto companionMean = [](const std::string& a_key, float& a_out) {
						LiveModel::TexImage map;
						if (a_key.empty() || !LiveModel::ReadTexture(a_key, map) || map.bytes.empty() ||
							map.format != 80) {
							return false;
						}
						const float mean = Bc4Mean(map.bytes, map.width, map.height);
						if (mean < 0.0f) {
							return false;
						}
						a_out = mean;
						return true;
					};
					const std::string metalKey = CompanionMapKey(ddsKey, "_metal.dds");
					const std::string roughKey = CompanionMapKey(ddsKey, "_rough.dds");
					const bool haveMetal = companionMean(metalKey, t.metal);
					const bool haveRough = companionMean(roughKey, t.rough);
					if (!haveRough) {
						t.rough = 0.5f;  // only reaches the shading when metal > 0
					}
					if (haveMetal || haveRough) {
						REX::INFO("SFShopExplorer offline GPU: metalness/roughness '{}' metal={:.3f} rough={:.3f} ({} / {})",
							mesh.matPath, t.metal, t.rough,
							haveMetal ? metalKey : "no _metal map", haveRough ? roughKey : "no _rough map");
					}
				}
				// Relief. The material's BC5 `_normal` companion is the only place
				// some authored detail exists: the Erdebrau can's lid is a flat
				// 16-gon plate whose albedo island is plain foil, and its pull
				// ring is embossed into this map alone. Two channels are stored and
				// Z is reconstructed in the shader, which is why the format is BC5
				// and not a three-channel colour format.
				//
				// Same size as the albedo, always: a companion that disagrees on
				// dimensions is a different asset's map, and using it would shear
				// the relief across the item.
				if (!ddsKey.empty()) {
					LiveModel::TexImage normal;
					const std::string normalKey = CompanionMapKey(ddsKey, "_normal.dds");
					if (!normalKey.empty() && LiveModel::ReadTexture(normalKey, normal) &&
						normal.format == 83 && !normal.bytes.empty() &&
						normal.width == t.width && normal.height == t.height) {
						t.normalFormat = normal.format;
						t.normalWidth = normal.width;
						t.normalHeight = normal.height;
						t.normalMipLevels = normal.mipLevels;
						t.normalRowPitch = normal.rowPitch;
						t.normalBytes = std::move(normal.bytes);
						REX::INFO("SFShopExplorer direct GPU: paired normal {} fmt={} ({}x{})",
							normalKey, normal.format, normal.width, normal.height);
					}
				}
				diagnostics.push_back(std::move(diagnostic));
				out.push_back(std::move(t));
				PrefetchTrace("  ok mat='" + mesh.matPath + "' tex='" + ddsKey + "' score=" +
					std::to_string(matchScore));
				REX::INFO("SFShopExplorer offline GPU: prefetched '{}' <- {} (score {})", mesh.matPath, ddsKey,
					matchScore == -10 ? "albedo" :
					(matchScore < 0 ? (matchScore == -1 ? "cached" : "fallback") : std::to_string(matchScore)));
				if (!mesh.uv.empty()) {
					float ulo = 1e30f, uhi = -1e30f, vlo = 1e30f, vhi = -1e30f;
					for (std::size_t q = 0; q + 1 < mesh.uv.size(); q += 2) {
						ulo = std::min(ulo, mesh.uv[q]);
						uhi = std::max(uhi, mesh.uv[q]);
						vlo = std::min(vlo, mesh.uv[q + 1]);
						vhi = std::max(vhi, mesh.uv[q + 1]);
					}
					REX::INFO("SFShopExplorer offline GPU: uv '{}' v={} u=[{:.3f},{:.3f}] v=[{:.3f},{:.3f}] tex={}x{}",
						mesh.matPath, mesh.pos.size() / 3, ulo, uhi, vlo, vhi, img.width, img.height);
				}
			}
			// One line per model naming the score, so a flat preview states its
			// own cause (0 of N) without correlating the per-mesh WARNs.
			REX::INFO("SFShopExplorer offline GPU: prefetch '{}' resolved {} of {} textures",
				a_key, out.size(), a_model.meshes.size());
			PrefetchTrace("done  '" + a_key + "' resolved=" + std::to_string(out.size()) +
				"/" + std::to_string(a_model.meshes.size()));
			{
				std::scoped_lock lock(g_texMutex);
				g_texCache[a_key] = std::move(out);
				g_materialDiagnostics[a_key] = diagnostics;
				g_texReady.insert(a_key);
				// Bound the session cache; the prefetch request path below
				// re-warms evicted keys on demand, so eviction is safe.
				if (g_texCache.size() > 24) {
					for (auto it = g_texCache.begin(); it != g_texCache.end();) {
						if (it->first != a_key) {
							g_texReady.erase(it->first);
							g_materialDiagnostics.erase(it->first);
							it = g_texCache.erase(it);
						} else {
							++it;
						}
					}
				}
			}
			{
				std::scoped_lock snapshotLock(g_snapshotMutex);
				if (g_snapshot.model == a_key) {
					g_snapshot.materials = std::move(diagnostics);
				}
			}
			// Colors pop in on their own once decoded.
			g_orbitDirty.store(true, std::memory_order_release);
		}

		// Render thread: take the warmed textures when ready. Returns false
		// while the task thread is still working (caller renders plain and
		// queues a warm request); true with possibly-empty out means the
		// model is confirmed (un)textured â€” never retry.
		bool TakePrefetched(const std::string& a_key, std::vector<PrefetchedTex>& a_out)
		{
			std::scoped_lock lock(g_texMutex);
			if (g_texReady.find(a_key) == g_texReady.end()) {
				return false;
			}
			const auto it = g_texCache.find(a_key);
			if (it != g_texCache.end()) {
				a_out = it->second;  // copy; entries stay cached for revisits
			}
			return true;
		}

		void QueuePrefetch(const std::string& a_key)
		{
			std::scoped_lock lock(g_texMutex);
			if (g_texReady.find(a_key) == g_texReady.end() && g_prefetchReq != a_key) {
				g_prefetchReq = a_key;
				REX::INFO("SFShopExplorer offline GPU: warm request '{}'", a_key);
			}
		}

		// Upload one archive-decoded image with D3D12's required 256-byte
		// placed-footprint row alignment. BA2 DDS rows are tightly packed.
		bool UploadTextureImage(NativeList* a_list, std::uint32_t a_format, std::uint32_t a_width,
			std::uint32_t a_height, std::uint32_t a_mipLevels, std::size_t a_sourcePitch,
			const std::vector<std::uint8_t>& a_bytes, std::uint32_t a_slot,
			::ID3D12Resource*& a_texture, ::ID3D12Resource*& a_stage)
		{
			a_texture = nullptr;
			a_stage = nullptr;
			auto* device = g_gpu.device;
			D3D12_RESOURCE_DESC td{};
			td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			td.Width = a_width;
			td.Height = a_height;
			td.DepthOrArraySize = 1;
			td.MipLevels = static_cast<UINT16>(std::clamp(a_mipLevels, 1u, 16u));
			td.Format = static_cast<DXGI_FORMAT>(a_format);
			td.SampleDesc.Count = 1;
			td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			UINT mipLevels = td.MipLevels;
			std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(mipLevels);
			std::vector<UINT> rows(mipLevels);
			std::vector<UINT64> rowBytes(mipLevels);
			UINT64 totalBytes = 0;
			device->GetCopyableFootprints(&td, 0, mipLevels, 0, footprints.data(), rows.data(), rowBytes.data(), &totalBytes);
			std::size_t sourceBytes = 0;
			std::size_t mipWidth = a_width, mipHeight = a_height;
			for (UINT mip = 0; mip < mipLevels; ++mip) {
				if (rows[mip] == 0 || rowBytes[mip] == 0 ||
					rowBytes[mip] > std::numeric_limits<std::size_t>::max() / rows[mip]) {
					return false;
				}
				const std::size_t mipRowBytes = static_cast<std::size_t>(rowBytes[mip]);
				if (mip == 0 && a_sourcePitch < mipRowBytes) {
					return false;
				}
				const std::size_t expectedRowBytes = a_format == 28 ? mipWidth * 4 :
					((mipWidth + 3) / 4) * ((a_format == 71 || a_format == 80) ? 8 : 16);
				const std::size_t expectedRows = a_format == 28 ? mipHeight : (mipHeight + 3) / 4;
				if (mipRowBytes != expectedRowBytes || rows[mip] != expectedRows ||
					expectedRowBytes > (std::numeric_limits<std::size_t>::max() - sourceBytes) / expectedRows) {
					return false;
				}
				sourceBytes += expectedRowBytes * expectedRows;
				mipWidth = std::max<std::size_t>(1, mipWidth / 2);
				mipHeight = std::max<std::size_t>(1, mipHeight / 2);
			}
			// A top-mip-only upload intentionally leaves the rest of the DDS
			// chain unused; only a short buffer is a failure.
			if (sourceBytes > a_bytes.size()) {
				return false;
			}
			D3D12_HEAP_PROPERTIES defaultHeap{};
			defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
			if (FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &td,
					D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&a_texture)))) {
				return false;
			}
			D3D12_RESOURCE_DESC sd{};
			sd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			sd.Width = totalBytes;
			sd.Height = 1;
			sd.DepthOrArraySize = 1;
			sd.MipLevels = 1;
			sd.Format = DXGI_FORMAT_UNKNOWN;
			sd.SampleDesc.Count = 1;
			sd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			D3D12_HEAP_PROPERTIES uploadHeap{};
			uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
			if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &sd,
					D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&a_stage)))) {
				a_texture->Release();
				a_texture = nullptr;
				return false;
			}
			void* mapped = nullptr;
			D3D12_RANGE noRead{ 0, 0 };
			if (FAILED(a_stage->Map(0, &noRead, &mapped))) {
				a_stage->Release();
				a_texture->Release();
				a_stage = nullptr;
				a_texture = nullptr;
				return false;
			}
			std::size_t sourceOffset = 0;
			for (UINT mip = 0; mip < mipLevels; ++mip) {
				const std::size_t mipRowBytes = static_cast<std::size_t>(rowBytes[mip]);
				const std::size_t mipRows = rows[mip];
				for (UINT row = 0; row < rows[mip]; ++row) {
					std::memcpy(static_cast<std::uint8_t*>(mapped) + footprints[mip].Offset +
							static_cast<std::size_t>(row) * footprints[mip].Footprint.RowPitch,
						a_bytes.data() + sourceOffset + static_cast<std::size_t>(row) * mipRowBytes,
						mipRowBytes);
				}
				sourceOffset += mipRowBytes * mipRows;
			}
			a_stage->Unmap(0, nullptr);
			D3D12_TEXTURE_COPY_LOCATION src{};
			src.pResource = a_stage;
			src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			src.PlacedFootprint = footprints[0];
			D3D12_TEXTURE_COPY_LOCATION dst{};
			dst.pResource = a_texture;
			dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			for (UINT mip = 0; mip < mipLevels; ++mip) {
				src.PlacedFootprint = footprints[mip];
				dst.SubresourceIndex = mip;
				a_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
			}
			Barrier(a_list, a_texture, D3D12_RESOURCE_STATE_COPY_DEST,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = static_cast<DXGI_FORMAT>(a_format);
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Texture2D.MipLevels = mipLevels;
			D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_pipe.srvHeap->GetCPUDescriptorHandleForHeapStart();
			cpu.ptr += static_cast<SIZE_T>(a_slot) * g_pipe.srvIncrement;
			device->CreateShaderResourceView(a_texture, &srv, cpu);
			return true;
		}

		// One texel of a BC4 mask, as UNORM. The drink bags' shared coverage map
		// is BC4 (DXGI 80), so a per-range sample has to decode a block rather
		// than read a byte: two endpoints plus sixteen 3-bit selectors packed as
		// one 48-bit little-endian run in raster order. BC4 is not BC3: the
		// palette has eight entries and the selectors are three bits, so reading
		// it as two-bit selectors (as the first draft of this did) misreads a
		// quarter of the texels. Verified against the shipped map: this decode
		// matches the reference BC4 decoder on 3,000 random texels. Used only to
		// tell the bag's parts apart, so it takes one texel and does not filter.
		float SampleBc4Mask(const std::vector<std::uint8_t>& a_bytes, std::uint32_t a_w, std::uint32_t a_h,
			float a_u, float a_v)
		{
			if (a_bytes.size() < 8 || a_w == 0 || a_h == 0) {
				return 1.0f;
			}
			float fu = std::fabs(a_u);
			float fv = std::fabs(a_v);
			fu -= std::floor(fu);
			fv -= std::floor(fv);
			const auto x = std::min<std::uint32_t>(static_cast<std::uint32_t>(fu * static_cast<float>(a_w)), a_w - 1);
			const auto y = std::min<std::uint32_t>(static_cast<std::uint32_t>(fv * static_cast<float>(a_h)), a_h - 1);
			const std::size_t rowPitch = static_cast<std::size_t>((a_w + 3) / 4) * 8;
			const std::size_t off = static_cast<std::size_t>(y / 4) * rowPitch + static_cast<std::size_t>(x / 4) * 8;
			if (off + 8 > a_bytes.size()) {
				return 1.0f;
			}
			const int r0 = static_cast<int>(a_bytes[off]);
			const int r1 = static_cast<int>(a_bytes[off + 1]);
			std::uint64_t bits = 0;
			for (int i = 0; i < 6; ++i) {
				bits |= static_cast<std::uint64_t>(a_bytes[off + 2 + static_cast<std::size_t>(i)]) << (8 * i);
			}
			const unsigned texel = static_cast<unsigned>((x % 4) + 4 * (y % 4));
			const int sel = static_cast<int>((bits >> (3 * texel)) & 7u);
			int value = r0;
			if (r0 > r1) {
				// Eight-entry palette, 7-step interpolation.
				if (sel == 1) {
					value = r1;
				} else if (sel >= 2) {
					value = ((8 - sel) * r0 + (sel - 1) * r1) / 7;
				}
			} else {
				// Six-entry palette, 5-step interpolation, then 0 and 255.
				if (sel == 1) {
					value = r1;
				} else if (sel >= 2 && sel <= 5) {
					value = ((6 - sel) * r0 + (sel - 1) * r1) / 5;
				} else if (sel == 7) {
					value = 255;
				} else {
					value = 0;
				}
			}
			return static_cast<float>(value) / 255.0f;
		}

		// One texel of a BC1 colour map, as 0-1 RGB. Needed because the drink
		// fill's authored hue is only defensible when the map is itself warm.
		// Measured under the liquid mesh's real UV footprint (11.9% of each
		// atlas, tools/renderer_recon/drinkbag_warm_gate.py): tea (114,73,1)
		// and tonic (228,173,155) are warm, Boba Alien Tea (136,151,102) is
		// green and Purified Water (117,118,118) is dead neutral. A maroon bias
		// on either of the last two would invent colour the item never had.
		// Same two-endpoint structure as BC4, plus RGB565.
		void SampleBc1Rgb(const std::vector<std::uint8_t>& a_bytes, std::uint32_t a_w, std::uint32_t a_h,
			float a_u, float a_v, float a_out[3])
		{
			a_out[0] = a_out[1] = a_out[2] = 1.0f;
			if (a_bytes.size() < 8 || a_w == 0 || a_h == 0) {
				return;
			}
			float fu = std::fabs(a_u);
			float fv = std::fabs(a_v);
			fu -= std::floor(fu);
			fv -= std::floor(fv);
			const auto x = std::min<std::uint32_t>(static_cast<std::uint32_t>(fu * static_cast<float>(a_w)), a_w - 1);
			const auto y = std::min<std::uint32_t>(static_cast<std::uint32_t>(fv * static_cast<float>(a_h)), a_h - 1);
			const std::size_t rowPitch = static_cast<std::size_t>((a_w + 3) / 4) * 8;
			const std::size_t off = static_cast<std::size_t>(y / 4) * rowPitch + static_cast<std::size_t>(x / 4) * 8;
			if (off + 8 > a_bytes.size()) {
				return;
			}
			const auto c0 = static_cast<std::uint16_t>(a_bytes[off] | (a_bytes[off + 1] << 8));
			const auto c1 = static_cast<std::uint16_t>(a_bytes[off + 2] | (a_bytes[off + 3] << 8));
			float e0[3], e1[3];
			for (int i = 0; i < 2; ++i) {
				const std::uint16_t c = i == 0 ? c0 : c1;
				float* e = i == 0 ? e0 : e1;
				e[0] = static_cast<float>((c >> 11) & 31) / 31.0f;
				e[1] = static_cast<float>((c >> 5) & 63) / 63.0f;
				e[2] = static_cast<float>(c & 31) / 31.0f;
			}
			const auto sel = (a_bytes[off + 4 + static_cast<std::size_t>(y % 4)] >> (2 * (x % 4))) & 3u;
			if (sel == 0) {
				for (int i = 0; i < 3; ++i) a_out[i] = e0[i];
			} else if (sel == 1) {
				for (int i = 0; i < 3; ++i) a_out[i] = e1[i];
			} else if (c0 > c1) {
				const float w = sel == 2 ? 2.0f / 3.0f : 1.0f / 3.0f;
				for (int i = 0; i < 3; ++i) a_out[i] = e0[i] * w + e1[i] * (1.0f - w);
			} else if (sel == 2) {
				for (int i = 0; i < 3; ++i) a_out[i] = (e0[i] + e1[i]) * 0.5f;
			} else {
				a_out[0] = a_out[1] = a_out[2] = 0.0f;  // 3-colour mode: fourth entry is black
			}
		}

		// Render thread, GPU objects from memory only: no file I/O, no
		// LiveModel mutex, no archive access. Textures bound here come
		// exclusively from the task-thread prefetch above.
		bool UploadModelTextures(NativeList* a_list, const std::vector<PrefetchedTex>& a_pre)
		{
			REX::INFO("SFShopExplorer offline GPU: texturing '{}' ({} ranges, {} prefetched)", g_gpuModel.key, g_gpuModel.ranges.size(), a_pre.size());
			bool any = false;
			// The coolant jar's fluid shell (its caustic loop) is drawn in the
			// jar's own green: see kAuthoredGlassTint. Scoped to a model that
			// actually wears the authored glass, so a caustic gobo anywhere else
			// keeps its own white.
			bool authoredGlass = false;
			for (const auto& r : g_gpuModel.ranges) {
				if (r.translucent && IsAuthoredSheerMat(r.matPath)) {
					authoredGlass = true;
					break;
				}
			}
			std::uint32_t slot = 1;  // slot 0 = white
			std::uint32_t opacityCount = 0;
			std::uint32_t detailCount = 0;
			std::size_t rangeIndex = 0;
			for (auto& range : g_gpuModel.ranges) {
				range.texSlot = -1;
				range.opacitySlot = -1;
				range.detailSlot = -1;
				range.normalSlot = -1;
				range.maskAlbedo = false;
				range.woodEmboss = false;
					range.razorbackGripBadge = false;
					range.crucibleEmissive = false;
					range.vortexGrenadeEmissive = false;

				if (range.forceNeutralGrey) {
					++rangeIndex;
					continue;  // explicit no-wood override: keep the neutral untextured range
				}
				if (range.matPath.empty() || slot > kMaxGpuTextures) {
					++rangeIndex;
					continue;
				}
				const PrefetchedTex* found = nullptr;
				for (const auto& t : a_pre) {
					if (t.matPath == range.matPath && t.nodeName == range.nodeName) {
						found = &t;
						break;
					}
				}
				if (!found) {
					// A see-through surface with no colour map has no albedo to show: the
					// engine draws such glass as transmission plus reflection. Leaving the
					// neutral grey fallback in place is what made the Alien Genetic
					// Material jar a solid tub. Measured in the 2026-10-20 09:41 session:
					// its 6 (node,set) shapes arrive as 12 ranges with the materials
					// repeated in the same order (whether that second set is a real inner
					// wall or a duplicated add is not settled), and two of those ranges
					// are the glass, so the shell is painted twice per wall. Snowglobe_
					// Landmarks_Glass shows the same defect at one wall - a milky dome -
					// so this is the rule, not a per-item patch.
					// The tint below is the engine's own pale glass, and the blend
					// shader uses it to tint what the shell REFLECTS - coverage is
					// the Schlick Fresnel term now, so the specimen stays visible
					// through it. Glass that does carry a colour map keeps its tint
					// untouched.
					if (range.translucent) {
						float authored[3]{ 1.0f, 1.0f, 1.0f };
						if (IsAuthoredSheerMat(range.matPath)) {
							// The coolant jar. Its database set is empty of colour, and
							// a pale tint made it a grey tub beside the game's green
							// one; kAuthoredGlassTint is the measured value.
							for (int c = 0; c < 3; ++c) {
								range.color[c] = kAuthoredGlassTint[c];
							}
							REX::INFO("SFShopExplorer offline GPU: authored glass tint '{}' -> ({:.2f},{:.2f},{:.2f}) (database stores no colour)",
								range.matPath, range.color[0], range.color[1], range.color[2]);
						} else if (IsFlatClearGlassMat(range.matPath) &&
							LiveModel::FindCdbTint(range.matPath, authored)) {
							// The `_Base_Green` fills: coloured glass. Their database tint
							// is the only statement of what colour the contents are, so it
							// has to survive the upload - and a coloured glass body
							// transmits its colour, so it is not a clear shell.
							for (int c = 0; c < 3; ++c) {
								range.color[c] = authored[c];
							}
							range.clearShell = false;
							REX::INFO("SFShopExplorer offline GPU: flat clear glass '{}' keeps its database tint ({:.2f},{:.2f},{:.2f}) at alpha {:.2f}",
								range.matPath, range.color[0], range.color[1], range.color[2], range.alpha);
						} else {
							// Light, very slightly cool: this is the tint the engine's own
							// glass carries, and it is drawn *under* the blend shader's
							// Fresnel sheen. A near-black tint (0.10) made every untextured
							// glass body vanish against the dark preview backdrop - the
							// Sparkling Wine bottle and the coolant jar were both missing
							// for this reason, not for a material-resolution reason.
							// Measured against the game's own inventory render of those
							// two items, whose glass reads as a pale frosted shell.
							range.color[0] = 0.62f;
							range.color[1] = 0.66f;
							range.color[2] = 0.72f;
							// No colour map and no authored opacity: the database is
							// saying "clear glass", so the shader must not paint a
							// body for it - only its reflection.
							range.clearShell = true;
							REX::INFO("SFShopExplorer offline GPU: untextured glass '{}' -> tint ({:.2f},{:.2f},{:.2f}) authored alpha {:.2f} (no colour map; a clear shell, coverage from Fresnel)",
								range.matPath, range.color[0], range.color[1], range.color[2], range.alpha);
						}
					}
					// Node and colour are printed so a range that is painted from its
					// database tint alone can be traced back to the mesh it covers
					// without a rebuild: a globe's base is the only part below z=0.02
					// and it is the one surface a warm tint shows up on immediately.
					REX::INFO("SFShopExplorer offline GPU: range {} '{}' node '{}' colour ({:.2f},{:.2f},{:.2f}) <- UNTEXTURED (no prefetch)",
						rangeIndex, range.matPath, range.nodeName, range.color[0], range.color[1], range.color[2]);
					++rangeIndex;
					continue;
				}					const PrefetchedTex& img = *found;
				// A print needs ink. When a decal sheet's colour resolves to a
				// single-channel map the set names no ink at all - the map is pure
				// coverage - so drawing it can only paint a mask as if it were
				// colour. Skipped, the surface below shows through, which is what
				// the engine composites a print onto in the first place.
				//
				// A decal whose only map is ANOTHER part's mask is a leftover
				// print. The Alien Genetic Material jar is the measured case: its
				// label sleeve (`Isotopic_Coolant_Decals.mat`) has a database set
				// that names exactly one texture, the LID's cutout
				// (`isotopic_coolant_lid_opacity.dds`), which the lid range itself
				// binds as its mask. Drawn as ink, that stamps the lid's star onto
				// the jar's body - the black star of the 0.8.315 capture - and the
				// game's own render of the jar shows no print there at all.
				//
				// The test is deliberately narrow: the other material's copy must
				// carry COLOUR artwork of its own. Two mask-only prints sharing one
				// sheet (the Sparkling Wine's `_Details` and `_Decal`) are the
				// same print, not a borrowed one, and must both keep drawing.
				bool borrowedMask = false;
				if (!img.opacityMap.empty()) {
					for (const auto& other : a_pre) {
						if (other.matPath == range.matPath || other.opacityMap != img.opacityMap) {
							continue;
						}
						if (!IsSingleChannelFormat(other.format)) {
							borrowedMask = true;
							break;
						}
					}
				}
				// The borrowed-mask arm must not depend on the decal's own
				// channel count: a repacked mask can be a multi-channel BC7
				// sheet, and that is exactly how the label sleeve came back
				// (BC7_UNORM_SRGB under the Test profile). A borrowed mask is
				// another part's cutout whatever it is stored as.
				if (IsDecalSheet(range.matPath) &&
					(borrowedMask || (IsSingleChannelFormat(img.format) && !img.promotedMask))) {
					REX::INFO("SFShopExplorer offline GPU: mask-only decal '{}' skipped ({} carries no colour{})",
						range.matPath, img.colorMap,
						borrowedMask ? "; that mask is another material's cutout" : "");
					++rangeIndex;
					continue;
				}
				// A grunge atlas is an overlay, not the item's art. The Velocity
				// beverage's only map is grungeoilslick01_color (a blue/purple oil
				// slick) while the database authors its surface
				// (1.0,0.917,0.0) yellow, and binding the map multiplied the
				// authored colour away - the bottle previewed pale. When the
				// material carries a saturated override colour, leave the range
				// untextured so that colour is what is drawn.
				{
					std::string map = LowerAscii(img.colorMap);
					std::replace(map.begin(), map.end(), static_cast<char>(92), '/');
					float authored[3]{ 1.0f, 1.0f, 1.0f };
					if (range.cdbPaintTint && map.rfind("textures/common/grunge/", 0) == 0 &&
						LiveModel::FindCdbTint(range.matPath, authored) &&
						std::max({ authored[0], authored[1], authored[2] }) -
							std::min({ authored[0], authored[1], authored[2] }) >= 0.30f) {
						REX::INFO("SFShopExplorer direct GPU: grunge overlay '{}' left untextured for '{}'; "
							"authored colour ({:.2f},{:.2f},{:.2f}) kept",
							img.colorMap, range.matPath, authored[0], authored[1], authored[2]);
						++rangeIndex;
						continue;
					}
				}
				::ID3D12Resource* tex = nullptr;
				::ID3D12Resource* stage = nullptr;
				if (!UploadTextureImage(a_list, img.format, img.width, img.height, img.mipLevels, img.rowPitch,
						img.bytes, slot, tex, stage)) {
					continue;
				}
				g_gpuModel.tex.push_back(tex);
				g_gpuModel.texStage.push_back(stage);
				range.texSlot = static_cast<int>(slot);
				// A colour map that is really a mask. A single-channel (BC4/BC8)
				// map stores coverage, not colour, so bound as the albedo it samples
				// as (r,0,0) and paints the surface maroon-black. The Sparkling
				// Wine's bottle is the measured case: its set resolves a colour map
				// that is `genericspacewine_decal_opacity.dds`, a white bottle
				// silhouette with the net cut out of it. That set is not "flat +
				// opacity" (so it never took the promoted-cutout upload), and its
				// 0.749 mask means the wrap rule routes it to the opaque cutout
				// pass - which is why the mode-10 arm added there had nothing to
				// read. Coverage-as-white with the mask's dark pixels as holes is
				// the bottle the inventory render shows: pale body, black net.
				range.maskAlbedo = img.patternInk || IsSingleChannelFormat(img.format);
				range.metal = img.metal;
				range.rough = img.rough;					// DXGI BC3 (77) stores the opacity mask in alpha; BC4 (80)
					// and BC7 (98) keep it as greyscale ink and sample from red.
					// That is measured, not assumed: both masks the Test profile
					// repacks as BC7 decode to R=G=B with alpha at 255.
					range.opacityRedChannel = img.opacityFormat == 80 || img.opacityFormat == 98;
				// Whether this mask is a wrap or a sheet, decided from its own
				// texels: BC4 blocks decode to 16 coverage values (endpoint
				// 0/1 plus the six interpolants, the same interpolation the
				// shader performs). A mean at or above 0.70 means the map is
				// solid over most of its surface, so the material is a printed
				// body - the Erdebrau label wrap measures 0.881 - and drawing it
				// through the coverage blend would delete the body behind its
				// own print. Below that it stays a decal sheet.
				if (img.opacityFormat == 80 && img.opacityBytes.size() >= 8) {
					double sum = 0.0;
					std::size_t blocks = 0;
					for (std::size_t b = 0; b + 8 <= img.opacityBytes.size(); b += 8) {
						const float c0 = static_cast<float>(img.opacityBytes[b]) / 255.0f;
						const float c1 = static_cast<float>(img.opacityBytes[b + 1]) / 255.0f;
						std::uint64_t bits = 0;
						for (int k = 0; k < 6; ++k) {
							bits |= static_cast<std::uint64_t>(img.opacityBytes[b + 2 + static_cast<std::size_t>(k)]) << (8 * k);
						}
						for (int t = 0; t < 16; ++t) {
							const int code = static_cast<int>((bits >> (3 * t)) & 7u);
							const float v = code == 0 ? c0 : (code == 1 ? c1 :
								((8 - code) * c0 + (code - 1) * c1) / 7.0f);
							sum += v;
						}
						++blocks;
					}
					if (blocks != 0) {
						const double mean = sum / static_cast<double>(blocks * 16);
						range.maskMostlySolid = mean >= 0.70;
						if (range.maskMostlySolid) {
							REX::INFO("SFShopExplorer offline GPU: mask is a wrap ({:.3f} coverage) for '{}'; drawn opaque with a cutout instead of coverage-blended",
								mean, range.matPath);
						}
					}
				}
				if (!img.maskCutout && !range.cdbOverrideTint) {
					float albedoMean[3]{ 0.0f, 0.0f, 0.0f };
					// The bound map is this material's own art only when the
					// shipped database names it FOR that material. A stand-in (a
					// fuzzy family guess, a neighbour's atlas) must not throw the
					// authored override colour away: the Tranquilitea cup's teal
					// and the Hypergiant inhaler's yellow-green were both being
					// reset to white by maps the database never named for them,
					// which is why those two items did not move when the
					// layer-kind join fixed the rest of the Aid shelf.
					std::string ownColor;
					const bool ownSet = LiveModel::FindCdbMaterial(range.matPath, ownColor, nullptr);
					auto norm = [](std::string a_s) {
						a_s = LowerAscii(a_s);
						std::replace(a_s.begin(), a_s.end(), static_cast<char>(92), '/');
						return a_s;
					};
					// A map borrowed from a colour-word sibling is a stand-in even
					// though FindCdbMaterial now answers yes for it: the sibling
					// holds the art, the suffix holds the colour.
					std::string familyBase;
					const bool borrowedFamily = LiveModel::CdbColourFamilyBase(range.matPath, familyBase);
					const bool ownMap = ownSet && !borrowedFamily && !ownColor.empty() &&
						norm(ownColor) == norm(img.colorMap);
					float authored[3]{ 1.0f, 1.0f, 1.0f };
					const bool authoredTint = range.cdbPaintTint && LiveModel::FindCdbTint(range.matPath, authored);
					const bool neutralBase =
						Bc1MeanRgb(img.bytes, img.width, img.height, img.format, albedoMean) &&
						IsNeutralPaintBase(albedoMean);
					// A name colour survives a stand-in map. `Crafted_InHailerYellow`
					// has no database record at all, and the fuzzy matcher handed it
					// `inhailer_liquid_decal_color.dds` - a greyscale bubble sheet
					// (measured mean 0.783/0.782/0.783, spread 0.000). Drawing that at
					// white turned the Hypergiant Heart's gold body into a white panel
					// with pale speckles, which is the 2026-10-23 11:26 capture: the
					// game's own render is a gold body with a chrome collar. The map
					// still shades the surface, but it is a neutral stand-in, not this
					// material's art, so the name's colour rides on top of it.
					if (range.flatNameTint && !ownMap && neutralBase) {
						REX::INFO("SFShopExplorer direct GPU: name colour '{}' kept ({:.2f},{:.2f},{:.2f}) "
							"over stand-in neutral map {} ({:.2f},{:.2f},{:.2f})",
							range.matPath, range.color[0], range.color[1], range.color[2], img.colorMap,
							albedoMean[0], albedoMean[1], albedoMean[2]);
					} else if ((authoredTint && !ownMap) ||
						(range.cdbPaintTint && neutralBase)) {
						// Neutral paint base + database colour = the painted surface.
						// The override is authored in linear space, so convert it
						// into this pipe's sRGB bytes before it multiplies the map.
						for (int c = 0; c < 3; ++c) {
							range.color[c] = LinearToSrgb(range.color[c]);
						}
						REX::INFO("SFShopExplorer direct GPU: paint tint '{}' kept ({:.2f},{:.2f},{:.2f}) "
							"over {} ({:.2f},{:.2f},{:.2f})",
							range.matPath, range.color[0], range.color[1], range.color[2],
							ownMap ? "neutral base" : "stand-in map",
							albedoMean[0], albedoMean[1], albedoMean[2]);
					} else {
						range.color[0] = range.color[1] = range.color[2] = 1.0f;  // albedo carries color
					}
				}
				// A promoted cutout has no colour map at all: the bound albedo IS
				// the material's opacity mask, and shader mode 10 draws that
				// coverage as a white body. The palette grey the range was born
				// with is still riding along as the constant's tint, though, and
				// it multiplies the white body straight back down: the Sparkling
				// Wine's foil wrap, whose mask measures 0.90 across its own UV
				// footprint, was landing at 0.28 of white - darker than the clear
				// glass in front of it - and its net left no contrast at all.
				// With no authored colour anywhere in the set, white is the only
				// defensible base for the body.
				if (img.promotedMask) {
					range.color[0] = range.color[1] = range.color[2] = 1.0f;
				}
				if (img.patternInk) {
					// A greyscale pattern is a picture, not coverage (mode 11), so
					// the neutral tint has to be white or the white cells come out
					// palette grey - the same defect the promoted cutout had.
					range.color[0] = range.color[1] = range.color[2] = 1.0f;
				}
				// The bubbles are the fluid, and the fluid is inside the green
				// glass: added at its map's own white they would read as pale
				// steam over a green jar, which is not what the game's render of
				// this item shows (bright green specks, 2026-10-22 capture).
				if (range.additiveShell && authoredGlass) {
					for (int c = 0; c < 3; ++c) {
						range.color[c] = kAuthoredGlassTint[c];
					}
				}
				// One audit line per uploaded range: the atlas, the tint this range
				// actually multiplies with, whether a mask is bound, and the UV
				// footprint the range samples. Colour questions on these materials
				// have cost whole sessions of re-derivation (the ammo crates needed
				// three passes); with this line the answer is in the log.
				{
					float u0 = 0.0f, u1 = 0.0f, v0 = 0.0f, v1 = 0.0f;
					bool haveUv = false;
					for (std::size_t s = 0; s + 1 < range.uvSamples.size(); s += 2) {
						const float su = range.uvSamples[s];
						const float sv = range.uvSamples[s + 1];
						if (!haveUv) {
							u0 = u1 = su;
							v0 = v1 = sv;
							haveUv = true;
							continue;
						}
						u0 = std::min(u0, su);
						u1 = std::max(u1, su);
						v0 = std::min(v0, sv);
						v1 = std::max(v1, sv);
					}
					REX::INFO("SFShopExplorer direct GPU: range {} '{}' <- slot {} fmt={} {}x{} pitch={} "
						"tint=({:.2f},{:.2f},{:.2f}) mask={} uv={}",
						rangeIndex, range.matPath, slot, img.format, img.width, img.height, img.rowPitch,
						range.color[0], range.color[1], range.color[2],
						img.opacityBytes.empty() ? "none" : "yes",
						haveUv ? std::format("[{:.3f},{:.3f}]x[{:.3f},{:.3f}]", u0, u1, v0, v1) : "none");
				}
				++slot;
				if (!img.opacityBytes.empty() && opacityCount < kMaxGpuTextures) {
					::ID3D12Resource* opacity = nullptr;
					::ID3D12Resource* opacityStage = nullptr;
					const auto opacitySlot = kMaxGpuTextures + 1 + opacityCount;
					if (UploadTextureImage(a_list, img.opacityFormat, img.opacityWidth, img.opacityHeight,
							img.opacityMipLevels, img.opacityRowPitch, img.opacityBytes,
							opacitySlot, opacity, opacityStage)) {
						g_gpuModel.tex.push_back(opacity);
						g_gpuModel.texStage.push_back(opacityStage);
						range.opacitySlot = static_cast<int>(opacitySlot);
						range.promotedCutout = img.promotedMask;
						++opacityCount;
						// The drink inside the bag. The shared coverage map is the only
						// thing that tells the bag's parts apart, and it does so cleanly
						// (measured under each mesh's real UV footprint: film 0.24,
						// printed leaf 0.47, drink 0.76, cap 0.96). The drink sits in
						// its own band, and it has to blend: the inventory reference
						// shows the submerged leaf through it, which it cannot do
						// while the drink draws opaque. The leaf stays solid so the
						// print keeps its colour, and the cap stays solid at 0.96.
						if (IsCraftedDrinkBagMaterial(range.matPath) && !range.filmCoverage &&
							!range.uvSamples.empty() && img.opacityFormat == 80) {
							float sum = 0.0f;
							float lo = 1.0f;
							float hi = 0.0f;
							std::size_t samples = 0;
							for (std::size_t i = 0; i + 1 < range.uvSamples.size(); i += 2) {
								const float m = SampleBc4Mask(img.opacityBytes, img.opacityWidth, img.opacityHeight,
									range.uvSamples[i], range.uvSamples[i + 1]);
								sum += m;
								lo = std::min(lo, m);
								hi = std::max(hi, m);
								++samples;
							}
							if (samples != 0) {
								const float mean = sum / static_cast<float>(samples);
								if (mean > 0.55f && mean < 0.90f) {
									range.filmCoverage = true;
									range.liquidCoverage = true;
									// The coverage band alone would also catch Purified Water
									// and Boba Alien Tea - measured under this same footprint,
									// water is (117,118,118) and boba (136,151,102), so the bias
									// would tint them maroon/green-brown. Gate it on the fill's
									// own albedo being warm (R >= G >= B and a real red-minus-
									// blue spread): tea (114,73,1, +0.445) and tonic
									// (228,173,155, +0.286) qualify, water (-0.003) and boba
									// (green-dominant) do not, and all four still blend.
									bool warm = false;
									if (img.format == 71 && !img.bytes.empty()) {
										float rgb[3] = { 0.0f, 0.0f, 0.0f };
										for (std::size_t i = 0; i + 1 < range.uvSamples.size(); i += 2) {
											float px[3];
											SampleBc1Rgb(img.bytes, img.width, img.height, range.uvSamples[i],
												range.uvSamples[i + 1], px);
											rgb[0] += px[0];
											rgb[1] += px[1];
											rgb[2] += px[2];
										}
										for (int c = 0; c < 3; ++c) {
											rgb[c] /= static_cast<float>(samples);
										}
										warm = rgb[0] >= rgb[1] && rgb[1] >= rgb[2] && (rgb[0] - rgb[2]) >= 0.10f;
									}
									if (warm) {
										range.color[0] *= kDrinkFillBias[0];
										range.color[1] *= kDrinkFillBias[1];
										range.color[2] *= kDrinkFillBias[2];
									}
									REX::INFO("SFShopExplorer direct GPU: range {} '{}' is a liquid fill (coverage {:.2f}, span {:.2f}-{:.2f}) - blended like the film, hue bias {}",
										rangeIndex, range.matPath, mean, lo, hi, warm ? "applied" : "skipped (neutral atlas)");
								}
								// Whatever in this folder is not the film or the drink is a
								// solid part - the printed leaf, the cap - and the CDB pairs
								// the same coverage map with every flavour, so the default
								// path would clip it against that map. Half the leaf's
								// pixels sit under 0.5 on it, which is exactly the broken,
								// disconnected creature the capture shows. Mode 9 skips
								// the cutout and keeps the print whole.
								if (!range.filmCoverage) {
									range.noCoverageClip = true;
								}
							}
						}
					}
				}
				// Relief shares the detail region of the heap: a range has at
				// most one extra map, and the composite modes that use
				// `detailSlot` set their own flags, so the two can never collide.
				if (!img.normalBytes.empty() && img.detailBytes.empty() && detailCount < kMaxGpuTextures) {
					::ID3D12Resource* normal = nullptr;
					::ID3D12Resource* normalStage = nullptr;
					const auto normalSlot = 2 * kMaxGpuTextures + 1 + detailCount;
					if (UploadTextureImage(a_list, img.normalFormat, img.normalWidth, img.normalHeight,
							img.normalMipLevels, img.normalRowPitch, img.normalBytes,
							normalSlot, normal, normalStage)) {
						g_gpuModel.tex.push_back(normal);
						g_gpuModel.texStage.push_back(normalStage);
						range.normalSlot = static_cast<int>(normalSlot);
						++detailCount;
					}
				}
				if (!img.detailBytes.empty() && detailCount < kMaxGpuTextures) {
					::ID3D12Resource* detail = nullptr;
					::ID3D12Resource* detailStage = nullptr;
					const auto detailSlot = 2 * kMaxGpuTextures + 1 + detailCount;
					if (UploadTextureImage(a_list, img.detailFormat, img.detailWidth, img.detailHeight,
							img.detailMipLevels, img.detailRowPitch, img.detailBytes,
							detailSlot, detail, detailStage)) {
						g_gpuModel.tex.push_back(detail);
						g_gpuModel.texStage.push_back(detailStage);
						range.detailSlot = static_cast<int>(detailSlot);
						range.woodEmboss = img.woodEmboss;
						range.razorbackGripBadge = img.razorbackGripBadge;
						range.crucibleEmissive = img.crucibleEmissive;
						range.vortexGrenadeEmissive = img.vortexGrenadeEmissive;
						++detailCount;
					}
				}
				++rangeIndex;
				any = true;
			}
			g_gpuModel.texturesReady = true;
			return any;
		}

		// 1x1 white DEFAULT texture in slot 0 (plain draws need a bound
		// table). Uploaded through the render command list on first use.
		bool EnsureWhite(NativeList* a_list)
		{
			if (g_pipe.whiteTex) {
				return true;
			}
			auto* device = g_gpu.device;
			D3D12_RESOURCE_DESC wd{};
			wd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			wd.Width = 1;
			wd.Height = 1;
			wd.DepthOrArraySize = 1;
			wd.MipLevels = 1;
			wd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			wd.SampleDesc.Count = 1;
			wd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			D3D12_HEAP_PROPERTIES dh{};
			dh.Type = D3D12_HEAP_TYPE_DEFAULT;
			if (FAILED(device->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &wd,
					D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_pipe.whiteTex)))) {
				REX::WARN("SFShopExplorer offline GPU: pipe stage whitetex");
				return false;
			}
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
			UINT rows = 0;
			UINT64 rowBytes = 0, total = 0;
			device->GetCopyableFootprints(&wd, 0, 1, 0, &fp, &rows, &rowBytes, &total);
			D3D12_RESOURCE_DESC sd = wd;
			sd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			sd.Width = total;
			sd.Height = 1;
			sd.Format = DXGI_FORMAT_UNKNOWN;
			sd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			D3D12_HEAP_PROPERTIES sh{};
			sh.Type = D3D12_HEAP_TYPE_UPLOAD;
			if (FAILED(device->CreateCommittedResource(&sh, D3D12_HEAP_FLAG_NONE, &sd,
					D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_pipe.whiteStage)))) {
				g_pipe.whiteTex->Release();
				g_pipe.whiteTex = nullptr;
				REX::WARN("SFShopExplorer offline GPU: pipe stage whitestage");
				return false;
			}
			void* wm = nullptr;
			D3D12_RANGE noRead{ 0, 0 };
			if (FAILED(g_pipe.whiteStage->Map(0, &noRead, &wm))) {
				REX::WARN("SFShopExplorer offline GPU: pipe stage whitemap");
				return false;
			}
			std::memset(wm, 0xFF, static_cast<std::size_t>(total));
			g_pipe.whiteStage->Unmap(0, nullptr);
			D3D12_TEXTURE_COPY_LOCATION src{};
			src.pResource = g_pipe.whiteStage;
			src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			src.PlacedFootprint = fp;
			D3D12_TEXTURE_COPY_LOCATION dst{};
			dst.pResource = g_pipe.whiteTex;
			dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			a_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
			Barrier(a_list, g_pipe.whiteTex, D3D12_RESOURCE_STATE_COPY_DEST,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Texture2D.MipLevels = 1;
			device->CreateShaderResourceView(g_pipe.whiteTex, &srv,
				g_pipe.srvHeap->GetCPUDescriptorHandleForHeapStart());
			return true;
		}

		bool RenderGpuModel(NativeList* a_list, const Model& a_model, const std::string& a_key, bool a_weapon)
		{
			if (!g_gpu.texture || !EnsureGpuPipe() || !BuildGpuModel(a_model, a_key, a_weapon) || !EnsureWhite(a_list)) {
				return false;
			}
			// Our texture -> render target.
			if (g_gpu.texState == 2) {
				Barrier(a_list, g_gpu.texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
					D3D12_RESOURCE_STATE_RENDER_TARGET);
			} else if (g_gpu.texState == 0) {
				Barrier(a_list, g_gpu.texture, D3D12_RESOURCE_STATE_COPY_DEST,
					D3D12_RESOURCE_STATE_RENDER_TARGET);
			}
			g_gpu.texState = 1;
			auto* device = g_gpu.device;
			device->CreateRenderTargetView(g_gpu.texture, nullptr,
				g_pipe.rtvHeap->GetCPUDescriptorHandleForHeapStart());
			D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_pipe.rtvHeap->GetCPUDescriptorHandleForHeapStart();
			D3D12_CPU_DESCRIPTOR_HANDLE dsv = g_pipe.dsvHeap->GetCPUDescriptorHandleForHeapStart();
			a_list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
			D3D12_VIEWPORT vp{};
			vp.Width = static_cast<float>(g_width);
			vp.Height = static_cast<float>(g_height);
			vp.MaxDepth = 1.0f;
			a_list->RSSetViewports(1, &vp);
			D3D12_RECT sc{ 0, 0, static_cast<LONG>(g_width), static_cast<LONG>(g_height) };
			a_list->RSSetScissorRects(1, &sc);
			// Backdrop for the rendered model. Defaults to a dark slate that matches the
			// game's own inventory screen rather than the old light grey: a white
			// field behind a weapon read as a product photo and fought every dark
			// shop theme. [offlinepreview] bg / bgg / bgb override it as 0-255
			// components; all three default to -1, meaning "use the built-in".
			// g_bgR/G/B arrive from the INI as 0-255; g_bgDefault is already 0-1.
			const auto channel = [](float v, float fallback) {
				return v >= 0.0f ? std::clamp(v / 255.0f, 0.0f, 1.0f) : fallback;
			};
			const float bg[4]{
				channel(g_bgR, g_bgDefault[0]),
				channel(g_bgG, g_bgDefault[1]),
				channel(g_bgB, g_bgDefault[2]),
				1.0f,
			};
			a_list->ClearRenderTargetView(rtv, bg, 0, nullptr);
			a_list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
			a_list->SetGraphicsRootSignature(g_pipe.rootSig);
			ID3D12DescriptorHeap* texHeap = g_pipe.srvHeap;
			a_list->SetDescriptorHeaps(1, &texHeap);
			a_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			D3D12_VERTEX_BUFFER_VIEW vbv{};
			vbv.BufferLocation = g_gpuModel.vb->GetGPUVirtualAddress();
			vbv.SizeInBytes = g_gpuModel.vbBytes;
			vbv.StrideInBytes = 32;
			a_list->IASetVertexBuffers(0, 1, &vbv);
			D3D12_INDEX_BUFFER_VIEW ibv{};
			ibv.BufferLocation = g_gpuModel.ib->GetGPUVirtualAddress();
			ibv.SizeInBytes = g_gpuModel.ibCount * 4;
			ibv.Format = DXGI_FORMAT_R32_UINT;
			a_list->IASetIndexBuffer(&ibv);
			if (!g_gpuModel.texturesReady) {
				if (!g_debugUpload) {
					REX::WARN("SFShopExplorer previewdebug: upload=0; '{}' draws untextured", g_gpuModel.key);
					g_gpuModel.texturesReady = true;  // do not retry this model
				} else {
					// Memory-only upload; file I/O stays on the task thread.
					// Not-ready yet: render plain this frame and ask the task
					// thread to warm the key (colors pop in when done).
					std::vector<PrefetchedTex> pre;
					if (TakePrefetched(g_gpuModel.key, pre)) {
						UploadModelTextures(a_list, pre);
					} else {
						QueuePrefetch(g_gpuModel.key);  // stays unready: retried
					}                                    // next frame until warmed
				}
			}
			const D3D12_GPU_DESCRIPTOR_HANDLE texBase = g_pipe.srvHeap->GetGPUDescriptorHandleForHeapStart();
			float mvp[16], mv[16];
			BuildMvp(mvp, mv, a_model, a_weapon);
			const D3D12_GPU_VIRTUAL_ADDRESS cbBase = g_pipe.cb->GetGPUVirtualAddress();
			const float animationTime = static_cast<float>(GetTickCount64() % 120000ULL) / 1000.0f;
			std::uint32_t slot = 0;
			std::uint32_t textured = 0, texturedTotal = 0;
			// Three passes: every opaque range first, then the translucent ones
			// with the blended pipeline, then the additive effect shells. The
			// shell pass is last because its light has to land on what is already
			// drawn - the jar's fluid is seen through its own glass, and the
			// specimen has to be there for it to bubble over. The slot counter is
			// deliberately not reset, so each range keeps a unique constant-buffer
			// slot across all passes and the range-sized buffer stays in bounds.
			if (!g_debugDraw) {
				REX::WARN("SFShopExplorer previewdebug: draw=0; '{}' uploads only, no draw", g_gpuModel.key);
			} else
			for (std::uint32_t pass = 0; pass < 3; ++pass) {
			for (const auto& range : g_gpuModel.ranges) {
				if (slot >= kMaxGpuRanges) {
					break;
				}
				// A glow shell is still not a surface: it is an emissive companion
				// of the artwork underneath, and the only map this renderer can bind
				// for it here is that artwork's full colour, which added at full
				// strength would wash the specimen out. Skipped, as before. The
				// animated caustic/gobo loop IS drawn - additively - because it is
				// the jar's fluid; see kPsAdditiveSrc.
				if (range.effectOverlay && !range.additiveShell) {
					continue;
				}
				// ...and a shell with no map has nothing to add: drawing it would
				// add a solid white body over the item, the old opaque defect.
				if (range.additiveShell && range.texSlot < 0) {
					continue;
				}
				// A promoted cutout has no colour map at all, so coverage blending it
				// would only draw its own coverage as colour; it takes the white-body
				// cutout mode instead (shader mode 10).
				const bool decalCoverage = range.texSlot >= 0 && IsDecalSheet(range.matPath) &&
					!range.razorbackGripBadge && !range.maskMostlySolid && !range.promotedCutout;
				// Film (the crafted drink-bag pouch) blends with its own opacity
				// map in the second pass. It needs the map uploaded to mean
				// anything: with opacitySlot < 0 the range stays opaque, which is
				// the old behaviour rather than a bag that vanishes.
				const bool filmBlend = range.filmCoverage && range.opacitySlot >= 0 && range.texSlot >= 0;
				// Pass routing: the shell is pass 2, blended ranges pass 1, and
				// everything else pass 0. (The no-map guard above already dropped a
				// shell that has nothing to add.)
				const bool additiveShell = range.additiveShell && range.texSlot >= 0;
				if (additiveShell) {
					if (pass != 2) {
						continue;
					}
				} else if (range.translucent || decalCoverage || filmBlend) {
					if (pass != 1) {
						continue;
					}
				} else if (pass != 0) {
					continue;
				}
				if (range.texSlot < 0 && !range.forceNeutralGrey && IsDecalSheet(range.matPath)) {
					continue;  // untextured transparent sheet: don't hide the surface below
				}
				++texturedTotal;
				if (range.texSlot >= 0) {
					++textured;
				}
				std::uint8_t* dst = g_pipe.cbMapped + static_cast<std::size_t>(slot) * 256;
				std::memcpy(dst, mvp, 64);
				std::memcpy(dst + 64, mv, 64);
				// BC3 selects alpha below 0.5; BC4 selects red otherwise.
				// Detail modes keep the BC4-red selector and enable their shader path.
				// A single-channel albedo on an opaque range is a greyscale picture,
				// not coverage: mode 11 shades it as such, while a promoted cutout
				// (mode 10) keeps its cutout. Translucent ranges are excluded -
				// their blend shader has its own meaning for the mask.
				const bool maskInkBody = range.maskAlbedo && !range.translucent;
				const float shaderMode = filmBlend ? 8.0f : (range.noCoverageClip ? 9.0f : (range.promotedCutout ? 10.0f : (maskInkBody ? 11.0f : (range.crucibleEmissive ? 6.0f : (range.vortexGrenadeEmissive ? 7.0f : (range.razorbackGripBadge ? 3.0f :
					(decalCoverage ? (range.opacitySlot >= 0 && !range.opacityRedChannel ? 5.0f : 4.0f) :
						(range.woodEmboss ? 2.0f : (range.opacityRedChannel ? 1.0f : 0.0f)))))))));
				const float col[4]{ range.crucibleEmissive ? animationTime : range.color[0],
					range.color[1], range.color[2], range.translucent ? range.alpha : shaderMode };
				std::memcpy(dst + 128, col, 16);
				// Metalness and roughness ride in the free tail of the same 256-byte
				// per-range constant, so this costs no extra buffer, no extra bind
				// and no extra descriptor: the shader reads byte 144, which nothing
				// before it used.
				const float metalRough[2]{ range.metal, range.rough };
				std::memcpy(dst + 144, metalRough, sizeof(metalRough));
				// A normal map is bound in the detail descriptor, so the shaders
				// need one float to know whether that descriptor holds relief or
				// the white texture: the free half of the same tail.
				// Only when the normal map is the descriptor that actually got
				// bound: a composite mode (wood engraving, Crucible, Vortex)
				// owns the same descriptor and would otherwise be sampled as if
				// its colour map were a normal map.
				const float relief[2]{
					range.normalSlot >= 0 && range.detailSlot < 0 ? 1.0f : 0.0f, 0.0f };
				std::memcpy(dst + 152, relief, sizeof(relief));
				// Shell flags at byte 160, the first free pair of floats in the same
				// 256-byte block (128 colour, 144 metal/rough, 152 relief): .x is the
				// blend shader's clear glass, .y tells the opaque shader to draw an
				// inside-out hull from the inside only.
				const float clearShell[2]{ range.clearShell ? 1.0f : 0.0f,
					range.interiorShell ? 1.0f : 0.0f };
				std::memcpy(dst + 160, clearShell, sizeof(clearShell));
				a_list->SetGraphicsRootConstantBufferView(0, cbBase + static_cast<std::uint64_t>(slot) * 256);
				const int texSlot = range.texSlot >= 0 ? range.texSlot : 0;
				D3D12_GPU_DESCRIPTOR_HANDLE th = texBase;
				th.ptr += static_cast<std::uint64_t>(texSlot) * g_pipe.srvIncrement;
				a_list->SetGraphicsRootDescriptorTable(1, th);
				D3D12_GPU_DESCRIPTOR_HANDLE opacityHandle = texBase;
				if (range.opacitySlot >= 0) {
					opacityHandle.ptr += static_cast<std::uint64_t>(range.opacitySlot) * g_pipe.srvIncrement;
				}
				a_list->SetGraphicsRootDescriptorTable(2, opacityHandle);
				D3D12_GPU_DESCRIPTOR_HANDLE detailHandle = texBase;
				if (range.detailSlot >= 0) {
					detailHandle.ptr += static_cast<std::uint64_t>(range.detailSlot) * g_pipe.srvIncrement;
				} else if (range.normalSlot >= 0) {
					detailHandle.ptr += static_cast<std::uint64_t>(range.normalSlot) * g_pipe.srvIncrement;
				}
				a_list->SetGraphicsRootDescriptorTable(3, detailHandle);
				// Decal sheets punch through their transparent texels instead
				// of paving the surface beneath (opaque textures are
				// unaffected: alpha reads 1 everywhere). Translucent ranges
				// take the blended pipeline instead, which is the whole
				// point of the second pass.
				if (range.interiorShell) {
					// The hull is drawn with the textured shader whatever its own
					// material says, because that is the arm carrying the clip that
					// removes its near wall; an untextured hull reads its flat colour
					// through the white slot. See the rule in BuildGpuModel.
					a_list->SetPipelineState(g_pipe.psoTextured);
				} else if (additiveShell) {
					// ONE/ONE: the shell's map is light added to the frame, not a
					// surface over it. This is the jar's fluid and bubbles.
					a_list->SetPipelineState(g_pipe.psoAdditive);
				} else if (filmBlend) {
					// Same blend-enabled pipeline the decal sheets use: source-alpha
					// blending, depth write off, so the tea and leaf behind the bag
					// show through it.
					a_list->SetPipelineState(g_pipe.psoDecalCoverage);
				} else if (decalCoverage) {
					a_list->SetPipelineState(g_pipe.psoDecalCoverage);
				} else if (range.translucent) {
					a_list->SetPipelineState(g_pipe.psoBlend);
				} else if (range.texSlot < 0) {
					a_list->SetPipelineState(g_pipe.pso);
				} else if (IsDecalSheet(range.matPath) || range.opacitySlot >= 0 || range.detailSlot >= 0) {
					a_list->SetPipelineState(g_pipe.psoTexturedAlpha);
				} else {
					a_list->SetPipelineState(g_pipe.psoTextured);
				}
				a_list->DrawIndexedInstanced(range.count, 1, range.start, 0, 0);
				++slot;
			}
			}
			Barrier(a_list, g_gpu.texture, D3D12_RESOURCE_STATE_RENDER_TARGET,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			g_gpu.texState = 2;
			{
				std::scoped_lock lock(g_snapshotMutex);
				g_snapshot.textured = textured;
				g_snapshot.texturedTotal = texturedTotal;
			}
			return true;
		}

		void Rasterize(const Model& a_model, bool a_weapon)
		{
			const std::uint32_t w = g_width;
			const std::uint32_t h = g_height;
			g_pixels.assign(static_cast<std::size_t>(w) * h, 0xFFD1BFB8u);  // == GPU clear (0.72,0.75,0.82)

		const float baseYaw = a_weapon ? g_weaponYaw : g_yawDeg;
		const float basePitch = a_weapon ? g_weaponPitch : g_pitchDeg;
		const float yaw = (baseYaw + g_yawOff.load(std::memory_order_relaxed)) * 3.14159265f / 180.0f;
		const float pitch = std::clamp(basePitch + g_pitchOff.load(std::memory_order_relaxed), -85.0f, 85.0f) *
			3.14159265f / 180.0f;
			const float sinYaw = std::sin(yaw), cosYaw = std::cos(yaw);
			const float sinPitch = std::sin(pitch), cosPitch = std::cos(pitch);

			const float zoomMul = std::clamp(g_zoomMul.load(std::memory_order_relaxed), 0.35f, 4.0f);
			const float camDist = FitCamDist(a_model, yaw, pitch, a_weapon,
				a_model.radius * 5.0f / g_zoom) / zoomMul;
			const float fov = 45.0f * 3.14159265f / 180.0f;
			const float aspect = static_cast<float>(w) / static_cast<float>(h);
			const float halfTan = std::tan(fov * 0.5f);

			std::vector<float> sx, sy, sz;
			sx.reserve(4096);
			sy.reserve(4096);
			sz.reserve(4096);
			std::vector<float> zbuf(static_cast<std::size_t>(w) * h, 1e30f);

			const float kx = 0.5f, ky = 0.7f, kz = 0.5f;
			const float kl = std::sqrt(kx * kx + ky * ky + kz * kz);
			const float flx = -0.3f, fly = 0.2f, flz = 0.6f;
			const float fl = std::sqrt(flx * flx + fly * fly + flz * flz);

		std::size_t meshIndex = 0;
		// The size filter (attachment points/decals) applies only when some
		// mesh passes it â€” otherwise the whole item would vanish (e.g. a
		// 12-tri clutter cube renders unfiltered).
		bool anyBig = false;
		for (const auto& mesh : a_model.meshes) {
			if (mesh.indices.size() / 3 >= 24 && mesh.pos.size() / 3 >= 12) {
				anyBig = true;
				break;
			}
		}
		// NIF weapons stand along +Y; lay the longest axis flat (-90 deg about
		// Z maps +Y onto +X) so rifles present a side profile. Boxy items
		// (no dominant axis) are left untouched. Weapons instead take the
		// cyclic (x,y,z)->(-y,z,-x): muzzle +Y onto -X, slide +Z onto +Y,
		// wood -Z onto -Y, rear -Y onto +X â€” the inventory side profile
		// at the user-calibrated weapon camera.
		const bool layFlat = !a_weapon && a_model.extents[1] > 1.8f * a_model.extents[0] &&
			a_model.extents[1] > 1.8f * a_model.extents[2];
		for (const auto& mesh : a_model.meshes) {
				++meshIndex;
				// Skip tiny sub-meshes (attachment points, effects, decals)
				// that overlap the main geometry when rendered together â€”
				// unless they sprawl (soil discs, sights: MeshSpread).
				const std::size_t vertCount = mesh.pos.size() / 3;
				const std::size_t triCount = mesh.indices.size() / 3;
				const bool tinyGeometry = triCount < 24 || vertCount < 12;
				const bool tinyDecalSheet = tinyGeometry && IsDecalSheet(mesh.matPath);
				if (anyBig && tinyGeometry && !tinyDecalSheet && !MeshSpread(mesh, a_model.radius)) {
					continue;
				}
				// Transparent sheets never render on the texture-less CPU path:
				// without alpha they would pave the surface beneath in flat
				// grey (on GPU they punch through instead). Glass stays:
				// it has no alpha-tested PSO there and reads fine opaque.
				if (IsDecalSheet(mesh.matPath)) {
					continue;
				}
				float base[3];
				MeshColor(meshIndex, base);
				if (IsBreachTacticalStock(mesh.matPath, mesh.name)) {
					base[0] = 0.60f;
					base[1] = 0.62f;
					base[2] = 0.64f;
				}
				sx.resize(vertCount);
				sy.resize(vertCount);
				sz.resize(vertCount);
			for (std::size_t v = 0; v < vertCount; ++v) {
				float p[3];
				float lx = mesh.pos[v * 3] - a_model.center[0];
				float ly = mesh.pos[v * 3 + 1] - a_model.center[1];
				float lz = mesh.pos[v * 3 + 2] - a_model.center[2];
				if (a_weapon) {
					const float tx = -ly, ty = lz, tz = -lx;
					lx = tx;
					ly = ty;
					lz = tz;
				} else if (layFlat) {
					const float t = lx;
					lx = ly;
					ly = -t;
				}
				Rotated(lx, ly, lz, sinYaw, cosYaw, sinPitch, cosPitch, p);
					const float cx = p[0];
					const float cy = p[1];
					const float cz = p[2] - camDist;
					if (cz >= -0.01f) { sx[v] = 0; sy[v] = 0; sz[v] = 1e30f; continue; }
					const float perspScale = -1.0f / cz;
					sx[v] = (cx * perspScale) / halfTan / aspect;
					sy[v] = (cy * perspScale) / halfTan;
					sz[v] = cz;
				}
				for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
					const auto i0 = mesh.indices[t];
					const auto i1 = mesh.indices[t + 1];
					const auto i2 = mesh.indices[t + 2];
					if (i0 >= vertCount || i1 >= vertCount || i2 >= vertCount) { continue; }
					if (sz[i0] >= 1e20f || sz[i1] >= 1e20f || sz[i2] >= 1e20f) { continue; }
					const float x0 = sx[i0], y0 = sy[i0], z0 = sz[i0];
					const float x1 = sx[i1], y1 = sy[i1], z1 = sz[i1];
					const float x2 = sx[i2], y2 = sy[i2], z2 = sz[i2];

					// Screen-space backface cull: positive cross = CCW = front face
					const float ux = x1 - x0, uy = y1 - y0;
					const float vx2 = x2 - x0, vy2 = y2 - y0;
					const float cross = ux * vy2 - uy * vx2;
					if (cross <= 0.0f) { continue; }

					const float px0 = (x0 * 0.5f + 0.5f) * w;
					const float py0 = (0.5f - y0 * 0.5f) * h;
					const float px1 = (x1 * 0.5f + 0.5f) * w;
					const float py1 = (0.5f - y1 * 0.5f) * h;
					const float px2 = (x2 * 0.5f + 0.5f) * w;
					const float py2 = (0.5f - y2 * 0.5f) * h;
					const float area = (px1 - px0) * (py2 - py0) - (px2 - px0) * (py1 - py0);
					if (std::fabs(area) < 0.5f) { continue; }

					// Face normal in view space for lighting
					const float ev0x = x1 - x0, ev0y = y1 - y0, ev0z = z1 - z0;
					const float ev1x = x2 - x0, ev1y = y2 - y0, ev1z = z2 - z0;
					float fnx = ev0y * ev1z - ev0z * ev1y;
					float fny = ev0z * ev1x - ev0x * ev1z;
					float fnz = ev0x * ev1y - ev0y * ev1x;
					const float fnl = std::sqrt(fnx * fnx + fny * fny + fnz * fnz);
					if (fnl < 1e-9f) { continue; }
					fnx /= fnl; fny /= fnl; fnz /= fnl;
					const float kd = std::clamp(-(fnx * kx + fny * ky + fnz * kz) / kl, 0.0f, 1.0f);
					const float kf = std::clamp(-(fnx * flx + fny * fly + fnz * flz) / fl, 0.0f, 1.0f);
					const float shade = 0.40f + 0.50f * kd + 0.18f * kf + 0.08f * kd * kd;

					const float invArea = 1.0f / area;
					const float minX = std::max(std::min({ px0, px1, px2 }), 0.0f);
					const float maxX = std::min(std::max({ px0, px1, px2 }), static_cast<float>(w) - 1.0f);
					const float minY = std::max(std::min({ py0, py1, py2 }), 0.0f);
					const float maxY = std::min(std::max({ py0, py1, py2 }), static_cast<float>(h) - 1.0f);
					if (minX > maxX || minY > maxY) { continue; }
					const int ix0 = static_cast<int>(minX);
					const int ix1 = static_cast<int>(maxX) + 1;
					const int iy0 = static_cast<int>(minY);
					const int iy1 = static_cast<int>(maxY) + 1;
					const std::uint32_t r8 = static_cast<std::uint32_t>(std::clamp(base[0] * shade, 0.0f, 1.0f) * 255.0f);
					const std::uint32_t g8 = static_cast<std::uint32_t>(std::clamp(base[1] * shade, 0.0f, 1.0f) * 255.0f);
					const std::uint32_t b8 = static_cast<std::uint32_t>(std::clamp(base[2] * shade, 0.0f, 1.0f) * 255.0f);
					const std::uint32_t pix = 0xFF000000u | (b8 << 16) | (g8 << 8) | r8;
					for (int iy = iy0; iy < iy1; ++iy) {
						const float fy = static_cast<float>(iy) + 0.5f;
						auto* row = &g_pixels[static_cast<std::size_t>(iy) * w];
						auto* zrow = &zbuf[static_cast<std::size_t>(iy) * w];
						for (int ix = ix0; ix < ix1; ++ix) {
							const float fx = static_cast<float>(ix) + 0.5f;
							const float w0 = ((px1 - fx) * (py2 - fy) - (px2 - fx) * (py1 - fy)) * invArea;
							const float w1 = ((px2 - fx) * (py0 - fy) - (px0 - fx) * (py2 - fy)) * invArea;
							const float w2 = 1.0f - w0 - w1;
							if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) { continue; }
							const float depth = w0 * z0 + w1 * z1 + w2 * z2;
							if (depth >= zrow[ix]) { continue; }
							zrow[ix] = depth;
							row[ix] = pix;
						}
					}
				}
			}
		}

		void CreateGpuTexture()
		{
			auto* device = g_gpu.device;
			const std::uint32_t w = g_width;
			const std::uint32_t h = g_height;
			if (g_gpu.texture && g_gpu.texWidth == w && g_gpu.texHeight == h && g_gpu.rtReady) {
				return;
			}
			if (g_gpu.upload) { g_gpu.upload->Release(); g_gpu.upload = nullptr; }
			if (g_gpu.texture) { g_gpu.texture->Release(); g_gpu.texture = nullptr; }
			g_gpu.srvCreated = false;
			g_gpu.srvHandle = 0;
			g_gpu.rtReady = false;

			D3D12_RESOURCE_DESC desc{};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			desc.Width = w;
			desc.Height = h;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.SampleDesc.Count = 1;
			desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
			D3D12_HEAP_PROPERTIES defaultHeap{};
			defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
			D3D12_CLEAR_VALUE clear{};
			clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			clear.Color[0] = 0.72f;
			clear.Color[1] = 0.75f;
			clear.Color[2] = 0.82f;
			clear.Color[3] = 1.0f;
			if (FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
				D3D12_RESOURCE_STATE_COPY_DEST, &clear, IID_PPV_ARGS(&g_gpu.texture)))) {
				REX::ERROR("SFShopExplorer offline: could not create preview texture");
				return;
			}
			g_gpu.texState = 0;
			g_gpu.rtReady = true;
			const auto pitch = (static_cast<std::size_t>(w) * 4u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) & ~(static_cast<std::size_t>(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) - 1u);
			D3D12_RESOURCE_DESC uploadDesc{};
			uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			uploadDesc.Width = pitch * h;
			uploadDesc.Height = 1;
			uploadDesc.DepthOrArraySize = 1;
			uploadDesc.MipLevels = 1;
			uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
			uploadDesc.SampleDesc.Count = 1;
			uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			D3D12_HEAP_PROPERTIES uploadHeap{};
			uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
			if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
				D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_gpu.upload)))) {
				g_gpu.texture->Release();
				g_gpu.texture = nullptr;
				REX::ERROR("SFShopExplorer offline: could not create preview upload buffer");
				return;
			}
			g_gpu.texWidth = w;
			g_gpu.texHeight = h;
		}
	}

	void LoadConfig()
	{
		if (g_configLoaded) {
			return;
		}
		g_configLoaded = true;
		const auto ini = DllDirectory() / kIniFileName;
		g_enabled.store(GetPrivateProfileIntW(L"offlinepreview", L"enabled", 1, ini.c_str()) != 0, std::memory_order_release);
		g_gpuMode = GetPrivateProfileIntW(L"offlinepreview", L"gpu", 1, ini.c_str()) != 0;
		{
			// Diagnostic bisect file, separate from the user's INI so a test
			// stage can be switched without touching their settings.
			const auto debugIni = DllDirectory() / L"previewdebug.ini";
			g_debugRender = GetPrivateProfileIntW(L"previewdebug", L"render", 1, debugIni.c_str()) != 0;
			g_debugUpload = GetPrivateProfileIntW(L"previewdebug", L"upload", 1, debugIni.c_str()) != 0;
			g_debugDraw = GetPrivateProfileIntW(L"previewdebug", L"draw", 1, debugIni.c_str()) != 0;
			if (!g_debugRender || !g_debugUpload || !g_debugDraw) {
				REX::WARN("SFShopExplorer previewdebug: render={} upload={} draw={}",
					g_debugRender, g_debugUpload, g_debugDraw);
			}
		}
		g_width = std::clamp(static_cast<std::uint32_t>(GetPrivateProfileIntW(L"offlinepreview", L"width", 1024, ini.c_str())), 128u, 2048u);
		g_height = std::clamp(static_cast<std::uint32_t>(GetPrivateProfileIntW(L"offlinepreview", L"height", 768, ini.c_str())), 96u, 2048u);
		g_zoom = static_cast<float>(std::clamp(ParseFloat(ini, L"offlinepreview", L"zoom", 2.05), 1.0, 6.0));
		g_pitchDeg = static_cast<float>(ParseFloat(ini, L"offlinepreview", L"pitch", -22.0));
		g_yawDeg = static_cast<float>(ParseFloat(ini, L"offlinepreview", L"yaw", 32.0));
		g_weaponYaw = static_cast<float>(ParseFloat(ini, L"weaponview", L"wyaw", 40.0));
		g_weaponPitch = static_cast<float>(ParseFloat(ini, L"weaponview", L"wpitch", 20.0));
		// Model backdrop, 0-255 per channel. -1 (the default) means "leave this
		// channel on the built-in dark slate".
		g_bgR = static_cast<float>(ParseFloat(ini, L"offlinepreview", L"bg", -1.0));
		g_bgG = static_cast<float>(ParseFloat(ini, L"offlinepreview", L"bgg", -1.0));
		g_bgB = static_cast<float>(ParseFloat(ini, L"offlinepreview", L"bgb", -1.0));
		if (g_bgR >= 0.0f || g_bgG >= 0.0f || g_bgB >= 0.0f) {
			REX::INFO("SFShopExplorer offline: backdrop overridden to {:.3f},{:.3f},{:.3f}",
				g_bgR / 255.0, g_bgG / 255.0, g_bgB / 255.0);
		}
		wchar_t dir[MAX_PATH]{};
		GetPrivateProfileStringW(L"offlinepreview", L"dir", kAssetsModelsDir, dir, MAX_PATH, ini.c_str());
		const std::filesystem::path rel(dir);
		g_cacheDir = rel.is_absolute() ? rel : DllDirectory() / rel;
		// Load EDID-to-cacheKey aliases from aliases.json (next to index.json).
		// This maps form EditorIDs that don't match the BA2 filename to the
		// correct cache key (e.g. AA99 -> ar99 when the weapon's
		// BGSModelMaterialSwap::model points to a generic shared NIF).
		const auto aliasPath = g_cacheDir / L"aliases.json";
		std::ifstream aliasFile(aliasPath);
		if (aliasFile.is_open()) {
			try {
				std::string json((std::istreambuf_iterator<char>(aliasFile)),
					std::istreambuf_iterator<char>());
				// Minimal JSON parse: find "key": "value" pairs
				std::size_t pos = 0;
				while ((pos = json.find('"', pos)) != std::string::npos) {
					auto keyStart = pos + 1;
					auto keyEnd = json.find('"', keyStart);
					if (keyEnd == std::string::npos) break;
					auto key = json.substr(keyStart, keyEnd - keyStart);
					pos = json.find(':', keyEnd + 1);
					if (pos == std::string::npos) break;
					pos = json.find('"', pos + 1);
					if (pos == std::string::npos) break;
					auto valStart = pos + 1;
					auto valEnd = json.find('"', valStart);
					if (valEnd == std::string::npos) break;
					auto val = json.substr(valStart, valEnd - valStart);
					pos = valEnd + 1;
					g_aliases[LowerAscii(key)] = LowerAscii(val);
				}
				REX::INFO("SFShopExplorer offline: loaded {} aliases", g_aliases.size());
			} catch (...) {
				REX::WARN("SFShopExplorer offline: failed to parse aliases.json");
			}
		}
		REX::INFO("SFShopExplorer offline: config (enabled={}, dir={}, {}x{} zoom={:.2f})",
			g_enabled.load(std::memory_order_acquire), g_cacheDir.string(), g_width, g_height, g_zoom);
		// Cache-free experiment switch ([offlinepreview] usecache=0):
		// disk .sfmodel reads are skipped everywhere except the
		// just-written live reload, so every model builds live from game
		// data (slow first touch, no stale caches). Default 1 (on).
		g_useCache.store(GetPrivateProfileIntW(L"offlinepreview", L"usecache", 1, ini.c_str()) != 0,
			std::memory_order_release);
		if (!g_useCache.load(std::memory_order_acquire)) {
			REX::INFO("SFShopExplorer offline: disk cache DISABLED by INI (live-build only)");
		}
		LiveModel::LoadConfig();
	}

	void DeviceReady(ID3D12Device* a_device, ID3D12DescriptorHeap* a_srvHeap, std::uint32_t a_srvIncrement)
	{
		g_gpu.device = static_cast<NativeDevice*>(a_device);
		g_gpu.srvHeap = static_cast<NativeHeap*>(a_srvHeap);
		g_gpu.srvIncrement = a_srvIncrement;
	}

	void RequestPreview(const std::vector<std::string>& a_modelPaths, const std::string& a_label,
		const std::string& a_fallbackKey, bool a_weapon, bool a_preferFallback)
	{
		if (!g_enabled.load(std::memory_order_acquire)) {
			return;
		}
		// Record only: resolution (disk cache, aliases, live build) happens
		// on the task thread in ServicePending so the render thread never
		// blocks on disk I/O, BA2 scans, or NIF parsing. Assembled OMOD
		// paths resolve EDID-first (empty primary key): a curated hit
		// beats the assembly's first NIF; only an EDID miss live-builds
		// the assembly under the front NIF's key.
		const auto key = (a_preferFallback || a_modelPaths.empty()) ? std::string{} : ModelCacheKey(a_modelPaths.front());
		{
			std::scoped_lock lock(g_renderMutex);
			if (!g_requestDirect && g_requestKey == key && g_requestLabel == a_label && g_pendingPaths == a_modelPaths &&
				g_requestWeapon == a_weapon) {
				return;
			}
			g_requestKey = key;
			g_requestLabel = a_label;
			g_pendingPaths = a_modelPaths;
			g_pendingFallback = a_fallbackKey;
			g_requestDirect = false;
			g_requestDirectModel.reset();
			g_requestWeapon = a_weapon;
			// The legacy path is NIF/Clone3D geometry with no authored rotation
			// applied, so the renderer's own orientation guess must run.
			g_authoredOrientation.store(false, std::memory_order_release);
			g_pendingModel.reset();  // stale model must not render for the new key
			g_requestGen.fetch_add(1, std::memory_order_acq_rel);
		}
		// New model, new inspection: restart from the INI camera (weapons
		// start at [weaponview] wyaw/wpitch, user-calibrated against the
		// inventory scene).
		ResetOrbit();
		g_orbitWeapon.store(a_weapon, std::memory_order_release);
		REX::INFO("SFShopExplorer offline: request '{}' <- '{}' ({} path(s))", a_label,
			a_modelPaths.empty() ? std::string{} : a_modelPaths.front(), a_modelPaths.size());
	}

	void RequestPreviewDirect(std::vector<DirectMesh> a_meshes, const std::string& a_label, bool a_weapon,
		bool a_authoredOrientation)
	{
		if (!g_enabled.load(std::memory_order_acquire)) {
			return;
		}
		g_authoredOrientation.store(a_authoredOrientation, std::memory_order_release);

		auto model = std::make_shared<Model>();
		model->meshes.reserve(a_meshes.size());
		std::uint64_t hash = 14695981039346656037ull;
		const auto hashBytes = [&hash](const void* a_data, std::size_t a_size) {
			const auto* bytes = static_cast<const std::uint8_t*>(a_data);
			for (std::size_t i = 0; i < a_size; ++i) {
				hash ^= bytes[i];
				hash *= 1099511628211ull;
			}
		};
		const auto hashString = [&hash, &hashBytes](const std::string& a_value) {
			const auto size = static_cast<std::uint64_t>(a_value.size());
			hashBytes(&size, sizeof(size));
			hashBytes(a_value.data(), a_value.size());
		};
		hashString(a_label);
		hashBytes(&a_weapon, sizeof(a_weapon));
		for (auto& direct : a_meshes) {
			if (direct.pos.size() < 9 || direct.pos.size() % 3 != 0 || direct.indices.size() < 3) {
				continue;
			}
			Mesh mesh;
			mesh.name = std::move(direct.name);
			mesh.matPath = std::move(direct.matPath);
			mesh.pos = std::move(direct.pos);
			mesh.uv = std::move(direct.uv);
			mesh.indices = std::move(direct.indices);
			const auto vertexCount = mesh.pos.size() / 3;
			if (mesh.uv.size() != vertexCount * 2) {
				mesh.uv.clear();
			}
			mesh.indices.erase(std::remove_if(mesh.indices.begin(), mesh.indices.end(),
				[vertexCount](std::uint32_t a_index) { return a_index >= vertexCount; }), mesh.indices.end());
			mesh.indices.resize((mesh.indices.size() / 3) * 3);
			if (mesh.indices.size() < 3) {
				continue;
			}
			model->tris += static_cast<std::uint32_t>(mesh.indices.size() / 3);
			hashString(mesh.name);
			hashString(mesh.matPath);
			const std::uint64_t sizes[]{ mesh.pos.size(), mesh.uv.size(), mesh.indices.size() };
			hashBytes(sizes, sizeof(sizes));
			hashBytes(mesh.pos.data(), mesh.pos.size() * sizeof(float));
			hashBytes(mesh.uv.data(), mesh.uv.size() * sizeof(float));
			hashBytes(mesh.indices.data(), mesh.indices.size() * sizeof(std::uint32_t));
			model->meshes.push_back(std::move(mesh));
		}
		ComputeBounds(*model);
		const auto meshCount = static_cast<std::uint32_t>(model->meshes.size());
		const auto triCount = model->tris;
		const auto key = std::format("direct:{}#{:016x}", a_label, hash);
		{
			std::scoped_lock lock(g_renderMutex);
			if (g_requestDirect && g_requestKey == key && g_requestWeapon == a_weapon) {
				return;
			}
			g_requestKey = key;
			g_requestLabel = a_label;
			g_pendingPaths.clear();
			g_pendingFallback.clear();
			g_requestDirectModel = meshCount != 0 ? model : nullptr;
			g_requestDirect = true;
			g_requestWeapon = a_weapon;
			g_pendingModel.reset();
			g_requestGen.fetch_add(1, std::memory_order_acq_rel);
		}
		{
			std::scoped_lock lock(g_snapshotMutex);
			g_snapshot.ready = false;
			g_snapshot.srvHandle = 0;
			g_snapshot.label = a_label;
			g_snapshot.model = key;
			g_snapshot.meshes = meshCount;
			g_snapshot.tris = triCount;
			g_snapshot.textured = 0;
			g_snapshot.texturedTotal = 0;
			g_snapshot.materials.clear();
			g_snapshot.message = meshCount == 0 ? std::string("this form carries no direct mesh data")
				: std::string("building direct GPU preview");
		}
		ResetOrbit();
		g_orbitWeapon.store(a_weapon, std::memory_order_release);
		REX::INFO("SFShopExplorer direct: request '{}' ({} mesh(es), {} tris)", a_label, meshCount, triCount);
	}

	// Interactive orbit: accumulate drag deltas (degrees) and zoom steps.
	void AddOrbit(float a_dYawDeg, float a_dPitchDeg)
	{
		const float basePitch = g_orbitWeapon.load(std::memory_order_acquire) ? g_weaponPitch : g_pitchDeg;
		g_yawOff.store(g_yawOff.load(std::memory_order_relaxed) + a_dYawDeg, std::memory_order_relaxed);
		g_pitchOff.store(std::clamp(g_pitchOff.load(std::memory_order_relaxed) + a_dPitchDeg, -85.0f - basePitch, 85.0f - basePitch),
			std::memory_order_relaxed);
		g_orbitDirty.store(true, std::memory_order_release);
	}

	void AddZoom(float a_factor)
	{
		if (!(a_factor > 0.0f)) {
			return;
		}
		g_zoomMul.store(std::clamp(g_zoomMul.load(std::memory_order_relaxed) * a_factor, 0.35f, 4.0f),
			std::memory_order_relaxed);
		g_orbitDirty.store(true, std::memory_order_release);
	}

	void ResetOrbit()
	{
		g_yawOff.store(0.0f, std::memory_order_relaxed);
		g_pitchOff.store(0.0f, std::memory_order_relaxed);
		g_zoomMul.store(1.0f, std::memory_order_relaxed);
		g_orbitDirty.store(true, std::memory_order_release);
	}

	float OrbitYawDeg()
	{
		const float base = g_orbitWeapon.load(std::memory_order_acquire) ? g_weaponYaw : g_yawDeg;
		return base + g_yawOff.load(std::memory_order_relaxed);
	}

	float OrbitPitchDeg()
	{
		const float base = g_orbitWeapon.load(std::memory_order_acquire) ? g_weaponPitch : g_pitchDeg;
		return std::clamp(base + g_pitchOff.load(std::memory_order_relaxed), -85.0f, 85.0f);
	}

	float ZoomFactor()
	{
		return g_zoom * std::clamp(g_zoomMul.load(std::memory_order_relaxed), 0.35f, 4.0f);
	}

	// Task-thread resolution: disk cache -> aliases -> EDID -> live build.
	// Returns the model (possibly null) and the key it was resolved under.
	// a_paths carries every form NIF for the live multi-build (armor addons);
	// the disk/alias/EDID fast paths consult the primary (first) key only.
	//
	// Stock-magazine assembly (weapons are modular: the base NIF carries no
	// magazine, the engine attaches the stock mag): when the resolved base
	// has no mag-like mesh and <base>_mag (else <base>_magazine) exists in
	// the cache, its meshes merge in. Same-frame parts by construction
	// (verified bounds: rattler_mag sits in the grip well, sights on top).
	// Bases already carrying a mag/mag-cover/drum are untouched, as are
	// keys that already are parts. Barrels/muzzles/sights variants are the
	// player's choice and never auto-attach.
	static void AssembleDefaultMag(std::shared_ptr<const Model>& a_model, const std::string& a_key)
	{
		if (!a_model || a_model->meshes.empty() || a_key.empty()) {
			return;
		}
		if (a_key.compare(0, 4, "asm_") == 0) {
			return;  // OMOD assemblies declare their own mags explicitly
		}
		// Assemble once per key per session: mag NIFs share the gun's main
		// materials (rattler_mag wears Slideandlower/Decals), so material
		// sniffing can't detect a prior merge â€” without this the mag
		// appended unboundedly (10 -> 12 -> 14 -> ... in the log).
		static std::unordered_set<std::string> assembled;
		static std::mutex assembledMutex;
		{
			std::scoped_lock lock(assembledMutex);
			if (!assembled.insert(a_key).second) {
				return;
			}
		}
		const std::string lk = LowerAscii(a_key);
		static constexpr const char* kPartKeys[] = { "mag", "scope", "sight", "barrel", "muzzle",
			"stock", "grip", "suppressor", "supressor", "trigger", "casing", "charger" };
		for (const char* p : kPartKeys) {
			if (lk.find(p) != std::string::npos) {
				return;  // already a part, not a base
			}
		}
		auto matHasMag = [](const std::string& a_mat) {
			std::string b = LowerAscii(a_mat);
			const auto slash = b.find_last_of("/\\");
			if (slash != std::string::npos) {
				b.erase(0, slash + 1);
			}
			const auto dot = b.find_last_of('.');
			if (dot != std::string::npos) {
				b.erase(dot);
			}
			std::size_t i = 0;
			while (i <= b.size()) {
				const auto j = b.find('_', i);
				const std::string t = b.substr(i, j == std::string::npos ? j : j - i);
				if (t == "mag" || t == "magazine" || t == "magcover" || t == "drum" || t == "clip") {
					return true;
				}
				if (j == std::string::npos) {
					break;
				}
				i = j + 1;
			}
			return false;
		};
		for (const auto& m : a_model->meshes) {
			if (matHasMag(m.matPath)) {
				return;  // base already carries its magazine
			}
		}
		std::shared_ptr<const Model> mag = LoadModel(a_key + "_mag");
		if (!mag) {
			mag = LoadModel(a_key + "_magazine");
		}
		if (!mag || mag->meshes.empty()) {
			return;
		}
		auto merged = std::make_shared<Model>(*a_model);
		for (const auto& mm : mag->meshes) {
			merged->meshes.push_back(mm);
			merged->tris += static_cast<std::uint32_t>(mm.indices.size() / 3);
		}
		ComputeBounds(*merged);
		// Detached-mag guard: the mag NIF is built in its own frame. Merge
		// only when it sits with the gun (Kraken's floats far away and
		// must render the base alone instead of a floating extra).
		{
			const float dx = mag->center[0] - a_model->center[0];
			const float dy = mag->center[1] - a_model->center[1];
			const float dz = mag->center[2] - a_model->center[2];
			const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
			const float span = a_model->radius + mag->radius;
			if (!(dist <= 2.0f * span)) {
				REX::INFO("SFShopExplorer offline: mag '{}' detached from '{}' (d={:.1f}, span={:.1f}); base renders alone",
					mag->meshes.empty() ? "" : mag->meshes.front().name, a_key, dist, span);
				return;
			}
		}
		{
			std::scoped_lock lock(g_cacheMutex);
			g_cache[a_key] = merged;
		}
		a_model = std::move(merged);
		REX::INFO("SFShopExplorer offline: assembled '{}' + stock mag ({} meshes total)",
			a_key, a_model->meshes.size());
	}

	static std::pair<std::shared_ptr<const Model>, std::string> ResolveModelInner(
		const std::string& a_key, const std::vector<std::string>& a_paths, const std::string& a_fallback)
	{
		const bool assembledEmptyKey = a_key.empty() && !a_paths.empty();
		if (!a_key.empty()) {
			if (auto model = LoadModel(a_key)) {
				return { model, a_key };
			}
		}
		const auto edidLower = LowerAscii(a_fallback);
		// Assembled OMOD weapons (a_key empty, a_paths = 6-part build):
		// live-build FIRST â€” substring fallback would otherwise pick the
		// shortest part cache (m1919 -> m1919_slide) and render one part.
		if (assembledEmptyKey && !edidLower.empty()) {
			std::string liveAsmKey = "asm_" + edidLower;
			std::vector<LiveModel::LiveMesh> live;
			if (LiveModel::BuildModels(a_paths, live)) {
				const auto outPath = LiveModel::CacheDir() / (liveAsmKey + ".sfmodel");
				if (LiveModel::WriteSfModel(outPath, live)) {
					if (auto model = LoadModel(liveAsmKey, true)) {
						return { model, liveAsmKey };
					}
				}
			}
		}
		if (!edidLower.empty()) {
			const auto aliasIt = g_aliases.find(edidLower);
			if (aliasIt != g_aliases.end()) {
				if (auto model = LoadModel(aliasIt->second)) {
					return { model, aliasIt->second };
				}
			}
			if (auto model = LoadModel(edidLower)) {
				return { model, edidLower };
			}
			// Prior-session OMOD assembly product for this EDID.
			if (auto model = LoadModel("asm_" + edidLower)) {
				return { model, "asm_" + edidLower };
			}
			// EDID-token fallback for prefixed EDIDs (Companion_Solstice ->
			// solstice, NOCLUTTER_ResearchUI_Grendel_* -> grendel...): try
			// each underscore-separated token as an exact key, then as a
			// substring. Short/generic tokens (the, ui, 01) match nothing
			// and fall through harmlessly.
			{
				std::size_t i = 0;
				while (i < edidLower.size()) {
					while (i < edidLower.size() && edidLower[i] != '_' &&
						!(edidLower[i] >= 'a' && edidLower[i] <= 'z') &&
						!(edidLower[i] >= '0' && edidLower[i] <= '9')) {
						++i;
					}
					std::size_t j = i;
					while (j < edidLower.size() &&
						((edidLower[j] >= 'a' && edidLower[j] <= 'z') ||
							(edidLower[j] >= '0' && edidLower[j] <= '9'))) {
						++j;
					}
					const std::string tok = edidLower.substr(i, j - i);
					i = j + 1;
					if (tok.size() < 4) {
						continue;
					}
					const auto aliasTok = g_aliases.find(tok);
					if (aliasTok != g_aliases.end()) {
						if (auto model = LoadModel(aliasTok->second)) {
							REX::INFO("SFShopExplorer offline: token-alias '{}' -> '{}'", tok, aliasTok->second);
							return { model, aliasTok->second };
						}
					}
					if (auto model = LoadModel(tok)) {
						REX::INFO("SFShopExplorer offline: token '{}' -> '{}'", edidLower, tok);
						return { model, tok };
					}
				}
			}
			// Substring fallback for placeholder-model weapons (Cutter ->
			// lasercutter): the form's EDID names the gun while its model
			// field is a dummy marker. Task thread: filesystem only.
			// Attachment-only keys (scopes, barrels, mags...) never qualify,
			// so a bare EDID can't resolve to a bare sight. Affixed forms
			// for laser-family parts: plain "laser" would nuke lasercutter.
			// "base" excluded: *_base NIFs are attachment frames/mounts
			// (often material-less), never full guns.
			if (edidLower.size() >= 4) {
				static constexpr const char* kParts[] = { "scope", "sight", "barrel", "_mag", "magazine",
					"grip", "muzzle", "suppressor", "supressor", "compensator", "stock", "trigger",
					"casing", "bullet", "charger", "knife", "sword", "blade", "handle",
					"_laser", "laser_", "_optic", "optic_", "_lens", "lens_", "_reflex", "reflex_",
					"_glow", "glow_", "_dot", "dot_", "flashlight", "base" };
				std::error_code ec;
				std::string sub;
				for (const auto& e : std::filesystem::directory_iterator(g_cacheDir, ec)) {
					if (!e.is_regular_file(ec)) {
						continue;
					}
					if (LowerAscii(e.path().extension().string()) != ".sfmodel") {
						continue;
					}
					const std::string base = LowerAscii(e.path().stem().string());
					if (base.find(edidLower) == std::string::npos) {
						continue;
					}
					bool part = false;
					for (const char* p : kParts) {
						if (base.find(p) != std::string::npos) {
							part = true;
							break;
						}
					}
					if (part || (sub.empty() ? false : base.size() >= sub.size())) {
						continue;
					}
					sub = base;
				}
				if (!sub.empty()) {
					REX::INFO("SFShopExplorer offline: substring '{}' -> '{}'", edidLower, sub);
					if (auto model = LoadModel(sub)) {
						return { model, sub };
					}
				}
			}
		}
		if (!a_key.empty() && !a_paths.empty()) {
			std::vector<LiveModel::LiveMesh> live;
			if (LiveModel::BuildModels(a_paths, live)) {
				const auto outPath = LiveModel::CacheDir() / (a_key + ".sfmodel");
				if (LiveModel::WriteSfModel(outPath, live)) {
					if (auto model = LoadModel(a_key, true)) {
						return { model, a_key };
					}
				}
			}
		} else if (a_key.empty() && a_paths.empty() && !edidLower.empty()) {
			// Path-less forms (dummy/empty model field): discover the NIF
			// by EDID token across mesh archives, then live-build under
			// asm_<edid> (never a curated name). Task thread only.
			std::string nifKey;
			if (LiveModel::DiscoverNif(a_fallback, nifKey)) {
				std::vector<LiveModel::LiveMesh> live;
				if (LiveModel::BuildModels(std::vector<std::string>{ nifKey }, live)) {
					const std::string asmKey = "asm_" + edidLower;
					const auto outPath = LiveModel::CacheDir() / (asmKey + ".sfmodel");
					if (LiveModel::WriteSfModel(outPath, live)) {
						if (auto model = LoadModel(asmKey, true)) {
							return { model, asmKey };
						}
					}
				}
			}
		} else if (a_key.empty() && !a_paths.empty()) {
			// EDID-first assembly (OMOD NIFs): the disk/EDID chain above
			// found nothing curated, so live-build the assembly. The output
			// is namespaced asm_<edid> â€” NEVER the front NIF's key, which
			// would clobber that gun's curated cache on disk and in memory
			// (stinger.sfmodel was overwritten by an 8-NIF franken build).
			std::string liveKey;
			if (!edidLower.empty()) {
				liveKey = "asm_" + edidLower;
			} else {
				const std::string front = ModelCacheKey(a_paths.front());
				if (!front.empty()) {
					liveKey = "asm_" + front;
				}
			}
			if (!liveKey.empty()) {
				std::vector<LiveModel::LiveMesh> live;
				if (LiveModel::BuildModels(a_paths, live)) {
					const auto outPath = LiveModel::CacheDir() / (liveKey + ".sfmodel");
					if (LiveModel::WriteSfModel(outPath, live)) {
						if (auto model = LoadModel(liveKey, true)) {
							return { model, liveKey };
						}
					}
				}
			}
		}
		return { nullptr, a_key };
	}

	static std::pair<std::shared_ptr<const Model>, std::string> ResolveModel(
		const std::string& a_key, const std::vector<std::string>& a_paths, const std::string& a_fallback)
	{
		auto out = ResolveModelInner(a_key, a_paths, a_fallback);
		if (out.first) {
			AssembleDefaultMag(out.first, out.second);
		}
		return out;
	}

	void ServicePending()
	{
		// Serve texture warm requests first, even when the raster
		// generation is settled (revisits, evictions): the model lives in
		// the memory cache, only its textures need (re)decoding.
		{
			std::string warm;
			{
				std::scoped_lock lock(g_texMutex);
				warm = std::move(g_prefetchReq);
				g_prefetchReq.clear();
			}
			if (!warm.empty()) {
				std::shared_ptr<const Model> cached;
				{
					std::scoped_lock lock(g_cacheMutex);
					const auto directIt = g_directModels.find(warm);
					if (directIt != g_directModels.end()) {
						cached = directIt->second;
					} else {
						const auto it = g_cache.find(warm);
						if (it != g_cache.end()) {
							cached = it->second;
						}
					}
				}
				// A warm request whose model is not in either cache is the
				// silent dead end: the key was queued from the render thread
				// but nothing ever decodes it, so the preview stays flat with
				// no error at all. Say so instead of dropping it.
				REX::INFO("SFShopExplorer offline GPU: serving warm '{}' cached={}",
					warm, cached ? 1 : 0);
				if (cached) {
					PrefetchTextures(*cached, warm);
				}
			}
		}
		// Task thread: resolve legacy cache requests, or publish direct mesh
		// payloads without CPU rasterization. Texture archive reads remain
		// here; only the direct GPU path renders those meshes on Present.
		const auto gen = g_requestGen.load(std::memory_order_acquire);
		if (gen == g_rasterGen) {
			return;
		}
		// Snapshot the request under the lock, then release before resolving
		// legacy cache requests so the render thread can still run. Direct
		// requests need only publish their already-extracted geometry.
		// NOTE: resolve from the REQUEST key, not the resolved pending key.
		std::string key, label, fallback;
		std::vector<std::string> paths;
		std::shared_ptr<const Model> directModel;
		bool direct = false;
		bool weapon = false;
		{
			std::scoped_lock lock(g_renderMutex);
			key = g_requestKey;
			label = g_requestLabel;
			paths = g_pendingPaths;
			fallback = g_pendingFallback;
			direct = g_requestDirect;
			directModel = g_requestDirectModel;
			weapon = g_requestWeapon;
		}
		std::shared_ptr<const Model> model;
		std::string resolvedKey;
		if (direct) {
			model = std::move(directModel);
			resolvedKey = key;
			REX::INFO("SFShopExplorer direct: '{}' -> ready ({} mesh(es), {} tris)", label,
				model ? model->meshes.size() : 0, model ? model->tris : 0);
		} else {
			auto resolved = ResolveModel(key, paths, fallback);
			model = std::move(resolved.first);
			resolvedKey = std::move(resolved.second);
			REX::INFO("SFShopExplorer offline: resolve '{}' -> {} ('{}')", key,
				model ? "ready" : "empty", resolvedKey);
		}
		if (g_requestGen.load(std::memory_order_acquire) != gen) {
			return;  // a newer direct selection arrived while resolving
		}
		if (model && !direct) {
			Rasterize(*model, weapon);
		} else {
			std::scoped_lock lock(g_renderMutex);
			g_pixels.clear();  // direct NativeModel meshes are GPU-rendered only
		}
		// Publish the completed request under the lock so UploadGPU sees
		// it atomically with the pixel data; stale generations are discarded.
		{
			std::scoped_lock lock(g_renderMutex);
			if (g_requestGen.load(std::memory_order_acquire) != gen) {
				return;
			}
			g_pendingModel = model;
			g_pendingKey = resolvedKey;
			g_pendingLabel = label;
			g_publishedWeapon = weapon;
			g_publishedDirect = direct;
			g_rasterGen = gen;
		}
		if (direct && model) {
			std::scoped_lock lock(g_cacheMutex);
			g_directModels[resolvedKey] = model;
			if (g_directModels.size() > 24) {
				for (auto it = g_directModels.begin(); it != g_directModels.end();) {
					if (it->first != resolvedKey) {
						it = g_directModels.erase(it);
					} else {
						++it;
					}
				}
			}
		}
		// Textures decode after geometry is published, so first paint never
		// waits on BA2 I/O; materials appear when prefetch completes.
		if (model) {
			PrefetchTextures(*model, resolvedKey);
		}
	}

		void EnsurePreviewSrv();
		void PublishReadySnapshot();

		void UploadGPU(ID3D12GraphicsCommandList* a_list)
	{
		auto* list = static_cast<NativeList*>(a_list);
		if (!g_gpu.device || !g_gpu.srvHeap || !list) {
			return;
		}
		const auto gen = g_requestGen.load(std::memory_order_acquire);
		const bool needUpload = (gen != g_uploadedGen);
		const bool animateCrucible = g_gpuModel.texturesReady &&
			std::any_of(g_gpuModel.ranges.begin(), g_gpuModel.ranges.end(), [](const GpuDrawRange& range) {
				return range.crucibleEmissive && range.detailSlot >= 0;
			});
		// Interactive orbit asks for one more frame with the new camera.
		// Consumed every Present so a drag renders continuously; when a
		// fresh request is still rasterizing, the flag simply re-arms on
		// the next frame via the needUpload path below.
		const bool orbit = g_orbitDirty.exchange(false, std::memory_order_acq_rel);
		if (!needUpload && !orbit && !animateCrucible) {
			return;	// nothing new since the last upload and no animated material
		}
		std::shared_ptr<const Model> model;
		std::vector<std::uint32_t> pixels;
		std::string key;
		bool weapon = false;
		bool direct = false;
		{
			std::scoped_lock lock(g_renderMutex);
			if (g_rasterGen != gen && !g_pendingModel) {
				return;	// fresh request still resolving; retry next frame
			}
			model = g_pendingModel;
			pixels = g_pixels;
			key = g_pendingKey;
			weapon = g_publishedWeapon;
			direct = g_publishedDirect;
		}
		// Legacy CPU fallback needs fresh pixels when orbiting; direct
		// NativeModel requests never enter this branch.
		const bool gpuActive = g_gpuMode && !IsGpuBanned(key);
		if (orbit && model && !gpuActive && !direct) {
			Rasterize(*model, weapon);
			std::scoped_lock lock(g_renderMutex);
			pixels = g_pixels;
		}
		if (needUpload) {
			g_uploadedGen = gen;
		}

		{
			std::scoped_lock lock(g_snapshotMutex);
			g_snapshot.model = g_pendingKey;
			g_snapshot.label = g_pendingLabel;
			g_snapshot.meshes = model ? static_cast<std::uint32_t>(model->meshes.size()) : 0;
			g_snapshot.tris = model ? model->tris : 0;
			g_snapshot.width = g_width;
			g_snapshot.height = g_height;			if (model && direct && g_snapshot.model != g_pendingKey) {
				g_snapshot.materials.clear();
			}

			if (!model) {
				g_snapshot.materials.clear();
				g_snapshot.ready = false;
				g_snapshot.srvHandle = 0;
				g_snapshot.textured = 0;
				g_snapshot.texturedTotal = 0;
				g_snapshot.message = g_publishedDirect ? std::string("this form carries no direct mesh data") :
					(g_pendingKey.empty() ? std::string("this form carries no 3D model (ship / creature / ammo records usually have none)")
						: "no cached model for '" + g_pendingKey + "' (run build_model_cache.py)");
			}
		}
		// Diagnostic bisect: skip the GPU preview entirely without touching
		// the item request, so a crash caused by preview commands disappears
		// while the shop itself stays open.
		if (model && direct && !g_debugRender) {
			std::scoped_lock lock(g_snapshotMutex);
			g_snapshot.ready = false;
			g_snapshot.srvHandle = 0;
			g_snapshot.message = "preview render disabled by previewdebug.ini";
			return;
		}
		// Direct NativeModel previews are rendered only by this independent
		// GPU path; never fall back into the older cache/CPU renderer.
		if (model && direct && !gpuActive) {
			std::scoped_lock lock(g_snapshotMutex);
			g_snapshot.ready = false;
			g_snapshot.srvHandle = 0;
			g_snapshot.message = g_gpuMode ? std::string("direct GPU preview unavailable for this model")
				: std::string("direct GPU preview is disabled in config");
			return;
		}
		// Existing legacy requests retain their GPU-first/CPU fallback.
		if (model && gpuActive) {
			CreateGpuTexture();
			if (g_gpu.texture && RenderGpuModel(list, *model, key, weapon)) {
				EnsurePreviewSrv();
				PublishReadySnapshot();
				return;
			}
			BanGpuKey(key);
			if (direct) {
				std::scoped_lock lock(g_snapshotMutex);
				g_snapshot.ready = false;
				g_snapshot.srvHandle = 0;
				g_snapshot.message = "direct GPU renderer failed for this model";
				return;
			}
			// Legacy GPU failure: its CPU pixels below carry no textures.
			std::scoped_lock lock(g_snapshotMutex);
			g_snapshot.textured = 0;
			g_snapshot.texturedTotal = model ? static_cast<std::uint32_t>(model->meshes.size()) : 0;
		}
		if (!model || pixels.empty()) {
			return;
		}
		CreateGpuTexture();
		if (!g_gpu.texture || !g_gpu.upload) {
			return;
		}
		const auto pitch = (static_cast<std::size_t>(g_width) * 4u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) & ~(static_cast<std::size_t>(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) - 1u);
		void* mapped = nullptr;
		D3D12_RANGE readRange{ 0, 0 };
		if (FAILED(g_gpu.upload->Map(0, &readRange, &mapped))) {
			return;
		}
		auto* bytes = static_cast<std::byte*>(mapped);
		const auto srcPitch = static_cast<std::size_t>(g_width) * 4u;
		for (std::uint32_t row = 0; row < g_height; ++row) {
			std::memcpy(bytes + static_cast<std::size_t>(row) * pitch,
				pixels.data() + static_cast<std::size_t>(row) * g_width, srcPitch);
		}
		g_gpu.upload->Unmap(0, nullptr);

		D3D12_TEXTURE_COPY_LOCATION source{};
		source.pResource = g_gpu.upload;
		source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		source.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		source.PlacedFootprint.Footprint.Width = g_width;
		source.PlacedFootprint.Footprint.Height = g_height;
		source.PlacedFootprint.Footprint.Depth = 1;
		source.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(pitch);
		D3D12_TEXTURE_COPY_LOCATION target{};
		target.pResource = g_gpu.texture;
		target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		target.SubresourceIndex = 0;
		if (g_gpu.texState == 2) {
			// A previous GPU frame left the texture shader-readable;
			// return it to copy-dest before the CPU upload.
			Barrier(list, g_gpu.texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_COPY_DEST);
			g_gpu.texState = 0;
		}
		list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = g_gpu.texture;
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		list->ResourceBarrier(1, &barrier);
		g_gpu.texState = 2;

		EnsurePreviewSrv();
		PublishReadySnapshot();
	}

	void EnsurePreviewSrv()
	{
			if (g_gpu.srvCreated || !g_gpu.texture) {
				return;
			}
			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Texture2D.MipLevels = 1;
			const auto cpuStart = g_gpu.srvHeap->GetCPUDescriptorHandleForHeapStart();
			const auto gpuStart = g_gpu.srvHeap->GetGPUDescriptorHandleForHeapStart();
			const auto offset = static_cast<std::uint64_t>(kSrvSlot) * g_gpu.srvIncrement;
			g_gpu.device->CreateShaderResourceView(g_gpu.texture, &srv, { cpuStart.ptr + static_cast<SIZE_T>(offset) });
			g_gpu.srvHandle = gpuStart.ptr + offset;
			g_gpu.srvCreated = true;
			REX::INFO("SFShopExplorer offline: preview SRV bound (slot {})", kSrvSlot);
		}

		void PublishReadySnapshot()
		{
			std::scoped_lock lock(g_snapshotMutex);
			g_snapshot.ready = true;
			g_snapshot.srvHandle = g_gpu.srvHandle;
			g_snapshot.message.clear();
		}		// Readback state for the OSF build. The staging buffer is kept between
		// captures because the preview texture never changes size within a
		// session; a resize (INI edit or resolution change) recreates it.
		bool CaptureFramePixels(ID3D12CommandQueue* a_queue, ID3D12GraphicsCommandList* a_list,
			ID3D12Fence* a_fence, void* a_event, std::vector<std::uint8_t>& a_outRgba,
			std::uint32_t& a_outWidth, std::uint32_t& a_outHeight)
		{
			auto* list = static_cast<NativeList*>(a_list);
			if (!g_gpu.device || !g_gpu.texture || !list || !a_queue || !a_fence) {
				REX::WARN("SFShopExplorer offline: preview capture has no GPU frame to read (device {}, texture {}, list {})",
					g_gpu.device != nullptr, g_gpu.texture != nullptr, list != nullptr);
				return false;
			}
			// 2 = shader resource: the last UploadGPU left a finished frame.
			if (g_gpu.texState != 2 || !g_gpu.rtReady) {
				REX::WARN("SFShopExplorer offline: preview capture found no renderable frame (state {}, render-target {})",
					g_gpu.texState, g_gpu.rtReady);
				return false;
			}
			const std::uint32_t w = g_gpu.texWidth;
			const std::uint32_t h = g_gpu.texHeight;
			if (w == 0 || h == 0) {
				return false;
			}
			const std::size_t pitch =
				(static_cast<std::size_t>(w) * 4u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) &
				~(static_cast<std::size_t>(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) - 1u);
			static ID3D12Resource* s_stage = nullptr;
			static std::uint32_t s_stageW = 0;
			static std::uint32_t s_stageH = 0;
			if (!s_stage || s_stageW != w || s_stageH != h) {
				if (s_stage) {
					s_stage->Release();
					s_stage = nullptr;
				}
				D3D12_RESOURCE_DESC sd{};
				sd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				sd.Width = static_cast<UINT64>(pitch) * h;
				sd.Height = 1;
				sd.DepthOrArraySize = 1;
				sd.MipLevels = 1;
				sd.Format = DXGI_FORMAT_UNKNOWN;
				sd.SampleDesc.Count = 1;
				sd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
				D3D12_HEAP_PROPERTIES rh{};
				rh.Type = D3D12_HEAP_TYPE_READBACK;
				if (FAILED(g_gpu.device->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &sd,
						D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s_stage)))) {
					REX::ERROR("SFShopExplorer offline: could not create preview readback buffer");
					s_stage = nullptr;
					return false;
				}
				s_stageW = w;
				s_stageH = h;
			}

			Barrier(list, g_gpu.texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_COPY_SOURCE);
			D3D12_TEXTURE_COPY_LOCATION src{};
			src.pResource = g_gpu.texture;
			src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			src.SubresourceIndex = 0;
			D3D12_TEXTURE_COPY_LOCATION dst{};
			dst.pResource = s_stage;
			dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			dst.PlacedFootprint.Offset = 0;
			dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			dst.PlacedFootprint.Footprint.Width = w;
			dst.PlacedFootprint.Footprint.Height = h;
			dst.PlacedFootprint.Footprint.Depth = 1;
			dst.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(pitch);
			list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
			Barrier(list, g_gpu.texture, D3D12_RESOURCE_STATE_COPY_SOURCE,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			g_gpu.texState = 2;

			if (FAILED(list->Close())) {
				return false;
			}
			a_queue->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList* const*>(&list));
			static std::uint64_t s_fenceValue = 0;
			const auto value = ++s_fenceValue;
			if (FAILED(a_queue->Signal(a_fence, value))) {
				return false;
			}
			if (a_fence->GetCompletedValue() < value) {
				if (FAILED(a_fence->SetEventOnCompletion(value, static_cast<HANDLE>(a_event)))) {
					return false;
				}
				if (WaitForSingleObject(static_cast<HANDLE>(a_event), 5000) != WAIT_OBJECT_0) {
					REX::WARN("SFShopExplorer offline: preview readback timed out");
					return false;
				}
			}

			D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(pitch) * h };
			void* mapped = nullptr;
			if (FAILED(s_stage->Map(0, &readRange, &mapped))) {
				return false;
			}
			a_outRgba.resize(static_cast<std::size_t>(w) * h * 4u);
			const auto* bytes = static_cast<const std::uint8_t*>(mapped);
			for (std::uint32_t row = 0; row < h; ++row) {
				std::memcpy(a_outRgba.data() + static_cast<std::size_t>(row) * w * 4u,
					bytes + static_cast<std::size_t>(row) * pitch, static_cast<std::size_t>(w) * 4u);
			}
			s_stage->Unmap(0, nullptr);
			a_outWidth = w;
			a_outHeight = h;
			return true;
		}

	bool HasRenderableFrame()
	{
		return g_gpu.device && g_gpu.texture && g_gpu.texState == 2 && g_gpu.rtReady &&
			g_gpu.texWidth != 0 && g_gpu.texHeight != 0;
	}

	Snapshot GetSnapshot()
	{
		std::scoped_lock lock(g_snapshotMutex);
		Snapshot out = g_snapshot;
		out.enabled = g_enabled.load(std::memory_order_acquire);
		return out;
	}

	void Shutdown()
	{
		ReleaseGpuModel();
		ReleaseGpuPipe();
		{
			std::scoped_lock lock(g_gpuBanMutex);
			g_gpuBanned.clear();
		}
		if (g_gpu.upload) { g_gpu.upload->Release(); g_gpu.upload = nullptr; }
		if (g_gpu.texture) { g_gpu.texture->Release(); g_gpu.texture = nullptr; }
		g_gpu.srvCreated = false;
		g_gpu.srvHandle = 0;
		g_gpu.rtReady = false;
		g_gpu.texState = 0;
		std::scoped_lock lock(g_renderMutex);
		g_pixels.clear();
		g_pixels.shrink_to_fit();
	}
}
