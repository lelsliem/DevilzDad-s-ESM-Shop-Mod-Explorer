#include "PCH.h"
#include "LiveModel.h"
#include "OfflineModel.h"

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
				const std::wstring wide{ value };
				const std::string narrow(wide.begin(), wide.end());
				return std::stod(narrow);
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
		std::unordered_map<std::string, std::string> g_aliases;

		std::filesystem::path g_cacheDir;
		std::uint32_t g_width{ 512 };
		std::uint32_t g_height{ 384 };
		float g_pitchDeg{ -18.0f };
		float g_yawDeg{ 35.0f };
		float g_zoom{ 1.8f };
		// Weapon camera defaults ([weaponview] wyaw/wpitch): user-calibrated
		// against the inventory scene. Applied to weapon-oriented models
		// instead of the generic pitch/yaw above.
		float g_weaponYaw{ 40.0f };
		float g_weaponPitch{ 20.0f };
		std::atomic<bool> g_orbitWeapon{ false };
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
		// Software rasterizer — CPU fill into an RGBA buffer. Deterministic,
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
		std::string g_pendingKey;      // RESOLVED cache key (written at publish)
		std::string g_pendingLabel;
		std::vector<std::string> g_pendingPaths;  // every form NIF for live builds
		std::string g_pendingFallback; // EDID fallback cache key
		bool g_requestWeapon{ false };   // request-side weapon orient flag
		bool g_publishedWeapon{ false }; // orient flag of the published model
		// What the shop asked for (model-path key + label). Resolution may
		// settle on a DIFFERENT key (alias/EDID fallback), so the request
		// pair — never the resolved key — is the stability comparison.
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

		// Per-mesh accent colors (stylized shading — real PBR textures are a
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
		// depth buffer. Any failure falls back to the CPU rasterizer.
		// ------------------------------------------------------------------
		struct GpuPipe
		{
			static constexpr std::uint32_t kVersion = 3;
			::ID3D12RootSignature* rootSig{ nullptr };
			::ID3D12PipelineState* pso{ nullptr };
			::ID3D12PipelineState* psoTextured{ nullptr };
			::ID3D12PipelineState* psoTexturedAlpha{ nullptr };
			::ID3D12DescriptorHeap* rtvHeap{ nullptr };
			::ID3D12DescriptorHeap* dsvHeap{ nullptr };
			::ID3D12DescriptorHeap* srvHeap{ nullptr };  // own table: slot 0 white, 1..64 model maps
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

		static constexpr const char kVsSrc[] = R"(
cbuffer Frame : register(b0)
{
    row_major float4x4 g_mvp;
    row_major float4x4 g_mv;
    float3 g_color;
    float g_pad;
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
    float shade = 0.42 + 0.48 * kd + 0.16 * kf + 0.06 * kd * kd + spec;
    return float4(i.color * shade, 1.0);
}
)";

		static constexpr const char kPsTexSrc[] = R"(
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
    float spec = pow(abs(dot(n, hv)), 24.0) * 0.18;
    float shade = 0.42 + 0.48 * kd + 0.16 * kf + 0.06 * kd * kd + spec;
    return float4(albedo * i.color * shade, 1.0);
}
)";

		// Decal variant: transparent sheets (markings, labels, card faces)
		// punch through where the texture is transparent instead of paving
		// the surface beneath with background color. Opaque textures read
		// alpha 1 everywhere, so this is a no-op for them by construction.
		static constexpr const char kPsTexAlphaSrc[] = R"(
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
    float4 tex = g_albedo.Sample(g_samp, i.uv);
    clip(tex.a - 0.5);
    float3 n = normalize(i.normal + 1e-9);
    float3 viewDir = normalize(-i.viewPos + 1e-9);
    float3 key = normalize(float3(0.45, 0.65, 0.62));
    float3 fill = normalize(float3(-0.45, 0.25, 0.55));
    float kd = abs(dot(n, key));
    float kf = abs(dot(n, fill));
    float3 hv = normalize(key + viewDir);
    float spec = pow(abs(dot(n, hv)), 24.0) * 0.18;
    float shade = 0.42 + 0.48 * kd + 0.16 * kf + 0.06 * kd * kd + spec;
    return float4(tex.rgb * i.color * shade, 1.0);
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
			REL(g_pipe.psoTexturedAlpha);
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
			::ID3DBlob *vs = nullptr, *ps = nullptr, *pstex = nullptr, *pstexalpha = nullptr;
			bool ok = CompileShader(kVsSrc, "vs_5_0", &vs) && CompileShader(kPsSrc, "ps_5_0", &ps) &&
				CompileShader(kPsTexSrc, "ps_5_0", &pstex) && CompileShader(kPsTexAlphaSrc, "ps_5_0", &pstexalpha);
			if (ok) {
				D3D12_ROOT_PARAMETER params[2]{};
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
				D3D12_STATIC_SAMPLER_DESC samp{};
				samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
				samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
				samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
				samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
				samp.MaxAnisotropy = 1;
				samp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
				samp.MaxLOD = 0.0f;
				samp.ShaderRegister = 0;
				samp.RegisterSpace = 0;
				samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
				D3D12_ROOT_SIGNATURE_DESC rsDesc{};
				rsDesc.NumParameters = 2;
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
				pso.PS = { pstexalpha->GetBufferPointer(), pstexalpha->GetBufferSize() };
				if (ok && FAILED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_pipe.psoTexturedAlpha)))) {
					REX::ERROR("SFShopExplorer offline GPU: decal PSO create failed");
					ok = false;
				}
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
				// 64 constant-buffer slots (256B each), persistently mapped.
				D3D12_RESOURCE_DESC cd{};
				cd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				cd.Width = 64 * 256;
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
			// Own shader-visible SRV table: slot 0 = 1x1 white (plain
			// draws), slots 1..64 = per-model diffuse maps.
			g_pipe.srvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			{
				D3D12_DESCRIPTOR_HEAP_DESC sheap{};
				sheap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
				sheap.NumDescriptors = 65;
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

		void BuildMvp(float a_outMvp[16], float a_outMv[16], float a_radius, bool a_weapon)
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
			const float camDist = 5.0f / (g_zoom * std::clamp(g_zoomMul.load(std::memory_order_relaxed), 0.35f, 4.0f));  // model pre-normalized to r=1
			(void)a_radius;
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

		// Transparent sheets (markings, labels, card faces, letter/line
		// overlays, blanks): on GPU they punch through transparent texels
		// (alpha PSO) instead of paving the surface beneath; on the
		// texture-less CPU path they never render. Glass is structural and
		// deliberately NOT in this set (reads fine opaque).
		bool IsDecalSheet(const std::string& a_matPath)
		{
			std::string ml = LowerAscii(a_matPath);
			return ml.find("decal") != std::string::npos || ml.find("label") != std::string::npos ||
				ml.find("overlay") != std::string::npos || ml.find("blank") != std::string::npos;
		}

		bool BuildGpuModel(const Model& a_model, const std::string& a_key, bool a_weapon)
		{
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
		// clutter cube), the second pass renders unfiltered — a tiny render
		// beats an empty viewport and a needless GPU ban.
		auto accumulate = [&](bool a_skipTiny) {
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
				// quads). A soil disc spans the pot — it must survive.
				if (a_skipTiny && (triCount < 24 || vertCount < 12) && !MeshSpread(mesh, a_model.radius)) {
					continue;
				}
				if (vertCount > 4'000'000) {
					continue;
				}
				float base[3];
				MeshColor(meshIndex, base);
			const std::uint32_t baseV = static_cast<std::uint32_t>(pos.size() / 3);
			if (baseV + vertCount > 4'000'000) {
				return;
			}
				const bool hasUv = mesh.uv.size() == vertCount * 2;
				for (std::size_t v = 0; v < vertCount; ++v) {
					float lx = (mesh.pos[v * 3] - a_model.center[0]) * inv;
					float ly = (mesh.pos[v * 3 + 1] - a_model.center[1]) * inv;
					float lz = (mesh.pos[v * 3 + 2] - a_model.center[2]) * inv;
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
					pos.push_back(lx);
					pos.push_back(ly);
					pos.push_back(lz);
					uvs.push_back(hasUv ? mesh.uv[v * 2] : 0.0f);
					uvs.push_back(hasUv ? mesh.uv[v * 2 + 1] : 0.0f);
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
				}
				range.color[0] = base[0];
				range.color[1] = base[1];
				range.color[2] = base[2];
				range.matPath = mesh.matPath;
			range.nodeName = mesh.name;
			g_gpuModel.ranges.push_back(range);
			if (g_gpuModel.ranges.size() >= 64) {
				return;
			}
		}
		};
		accumulate(true);
		if (g_gpuModel.ranges.empty()) {
			REX::INFO("SFShopExplorer offline GPU: '{}' below size filter, rendering unfiltered", a_key);
			accumulate(false);
		}
			if (g_gpuModel.ranges.empty() || pos.empty() || idx.empty()) {
				ReleaseGpuModel();
				return false;
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
		// chunks — seconds of file I/O that must NEVER run on the Present
		// thread (a 28-mesh rifle froze Presents solid: "guns don't rotate").
		// The task thread warms a memory cache per model key; the render
		// thread only ever creates GPU objects from memory, and renders
		// plain until the prefetch lands (completion re-arms a frame, so
		// colors pop in on their own). Render thread takes g_texMutex only —
		// never the LiveModel I/O mutex, never the filesystem.
		struct PrefetchedTex
		{
			std::string matPath;    // matched against GpuDrawRange at upload
			std::string nodeName;   // (range indices diverge: tiny meshes are
			std::uint32_t format{ 0 };  // skipped when ranges are built, so no
			std::uint32_t width{ 0 };   // positional coupling is possible)
			std::uint32_t height{ 0 };
			std::size_t rowPitch{ 0 };
			std::vector<std::uint8_t> bytes;
		};
		std::mutex g_texMutex;
		std::unordered_map<std::string, std::vector<PrefetchedTex>> g_texCache;
		std::unordered_set<std::string> g_texReady;
		std::string g_prefetchReq;  // model key the task thread should warm

		// Task thread: resolve + decode every diffuse for a model into memory.
		void PrefetchTextures(const Model& a_model, const std::string& a_key)
		{
			{
				std::scoped_lock lock(g_texMutex);
				if (g_texReady.find(a_key) != g_texReady.end()) {
					return;
				}
			}
			std::vector<PrefetchedTex> out;
			for (const auto& mesh : a_model.meshes) {
				if (out.size() >= 64 || mesh.matPath.empty()) {
					continue;
				}
				// Exact first: the .mat JSON's Albedo File (mod materials
				// borrow cross-family maps no fuzzy matcher can guess).
				// Fuzzy FindDiffuse stays as the fallback (base-game mats
				// live in the CDB, not as files).
				std::string ddsKey;
				int matchScore = 0;
				bool exact = false;
				{
					std::string albedo;
					if (LiveModel::FindMatAlbedo(mesh.matPath, albedo)) {
						ddsKey = albedo;
						matchScore = -10;
						exact = true;
					} else if (!LiveModel::FindDiffuse(mesh.matPath, mesh.name, ddsKey, &matchScore)) {
						continue;
					}
				}
				LiveModel::TexImage img;
				if (!LiveModel::ReadTexture(ddsKey, img) || img.bytes.empty() || img.format == 0) {
					if (exact) {
						// Exact ref unreadable (missing archive, bad data):
						// one fuzzy retry before giving up on the mesh.
						exact = false;
						if (!LiveModel::FindDiffuse(mesh.matPath, mesh.name, ddsKey, &matchScore) ||
							!LiveModel::ReadTexture(ddsKey, img) || img.bytes.empty() || img.format == 0) {
							REX::WARN("SFShopExplorer offline GPU: texture unreadable for {} ({})", mesh.matPath, ddsKey);
							continue;
						}
					} else {
						REX::WARN("SFShopExplorer offline GPU: texture unreadable for {} ({})", mesh.matPath, ddsKey);
						continue;
					}
				}
				PrefetchedTex t;
				t.matPath = mesh.matPath;
				t.nodeName = mesh.name;
				t.format = img.format;
				t.width = img.width;
				t.height = img.height;
				t.rowPitch = img.rowPitch;
				t.bytes = std::move(img.bytes);
				out.push_back(std::move(t));
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
			{
				std::scoped_lock lock(g_texMutex);
				g_texCache[a_key] = std::move(out);
				g_texReady.insert(a_key);
				// Bound the session cache; the prefetch request path below
				// re-warms evicted keys on demand, so eviction is safe.
				if (g_texCache.size() > 24) {
					for (auto it = g_texCache.begin(); it != g_texCache.end();) {
						if (it->first != a_key) {
							g_texReady.erase(it->first);
							it = g_texCache.erase(it);
						} else {
							++it;
						}
					}
				}
			}
			// Colors pop in on their own once decoded.
			g_orbitDirty.store(true, std::memory_order_release);
		}

		// Render thread: take the warmed textures when ready. Returns false
		// while the task thread is still working (caller renders plain and
		// queues a warm request); true with possibly-empty out means the
		// model is confirmed (un)textured — never retry.
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
			}
		}

		// Render thread, GPU objects from memory only: no file I/O, no
		// LiveModel mutex, no archive access. Textures bound here come
		// exclusively from the task-thread prefetch above.
		bool UploadModelTextures(NativeList* a_list, const std::vector<PrefetchedTex>& a_pre)
		{
			auto* device = g_gpu.device;
			REX::INFO("SFShopExplorer offline GPU: texturing '{}' ({} ranges, {} prefetched)", g_gpuModel.key, g_gpuModel.ranges.size(), a_pre.size());
			bool any = false;
			std::uint32_t slot = 1;  // slot 0 = white
			std::size_t rangeIndex = 0;
			for (auto& range : g_gpuModel.ranges) {
				range.texSlot = -1;
				if (range.matPath.empty() || slot >= 65) {
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
					REX::INFO("SFShopExplorer offline GPU: range {} '{}' <- UNTEXTURED (no prefetch)", rangeIndex, range.matPath);
					++rangeIndex;
					continue;
				}
				const PrefetchedTex& img = *found;
				const DXGI_FORMAT fmt = static_cast<DXGI_FORMAT>(img.format);
				D3D12_RESOURCE_DESC td{};
				td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
				td.Width = img.width;
				td.Height = img.height;
				td.DepthOrArraySize = 1;
				td.MipLevels = 1;
				td.Format = fmt;
				td.SampleDesc.Count = 1;
				td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
				D3D12_HEAP_PROPERTIES dh{};
				dh.Type = D3D12_HEAP_TYPE_DEFAULT;
				::ID3D12Resource* tex = nullptr;
				if (FAILED(device->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &td,
						D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex)))) {
					continue;
				}
				D3D12_RESOURCE_DESC sd{};
				sd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				sd.Width = img.bytes.size();
				sd.Height = 1;
				sd.DepthOrArraySize = 1;
				sd.MipLevels = 1;
				sd.Format = DXGI_FORMAT_UNKNOWN;
				sd.SampleDesc.Count = 1;
				sd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
				D3D12_HEAP_PROPERTIES sh{};
				sh.Type = D3D12_HEAP_TYPE_UPLOAD;
				::ID3D12Resource* stage = nullptr;
				if (FAILED(device->CreateCommittedResource(&sh, D3D12_HEAP_FLAG_NONE, &sd,
						D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&stage)))) {
					tex->Release();
					continue;
				}
				void* mapped = nullptr;
				D3D12_RANGE noRead{ 0, 0 };
				if (FAILED(stage->Map(0, &noRead, &mapped))) {
					stage->Release();
					tex->Release();
					continue;
				}
				std::memcpy(mapped, img.bytes.data(), img.bytes.size());
				stage->Unmap(0, nullptr);
				D3D12_TEXTURE_COPY_LOCATION src{};
				src.pResource = stage;
				src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
				src.PlacedFootprint.Footprint.Format = fmt;
				src.PlacedFootprint.Footprint.Width = img.width;
				src.PlacedFootprint.Footprint.Height = img.height;
				src.PlacedFootprint.Footprint.Depth = 1;
				src.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(img.rowPitch);
				D3D12_TEXTURE_COPY_LOCATION dst{};
				dst.pResource = tex;
				dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				a_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
				Barrier(a_list, tex, D3D12_RESOURCE_STATE_COPY_DEST,
					D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
				srv.Format = fmt;
				srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				srv.Texture2D.MipLevels = 1;
				D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_pipe.srvHeap->GetCPUDescriptorHandleForHeapStart();
				cpu.ptr += static_cast<SIZE_T>(slot) * g_pipe.srvIncrement;
				device->CreateShaderResourceView(tex, &srv, cpu);
				g_gpuModel.tex.push_back(tex);
				g_gpuModel.texStage.push_back(stage);
				range.texSlot = static_cast<int>(slot);
				range.color[0] = range.color[1] = range.color[2] = 1.0f;  // albedo carries color
				REX::INFO("SFShopExplorer offline GPU: range {} '{}' <- slot {} fmt={} {}x{} pitch={}",
					rangeIndex, range.matPath, slot, img.format, img.width, img.height, img.rowPitch);
				++slot;
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
			const float clear[4]{ 0.72f, 0.75f, 0.82f, 1.0f };
			a_list->ClearRenderTargetView(rtv, clear, 0, nullptr);
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
			const D3D12_GPU_DESCRIPTOR_HANDLE texBase = g_pipe.srvHeap->GetGPUDescriptorHandleForHeapStart();
			float mvp[16], mv[16];
			BuildMvp(mvp, mv, a_model.radius, a_weapon);
			const D3D12_GPU_VIRTUAL_ADDRESS cbBase = g_pipe.cb->GetGPUVirtualAddress();
			std::uint32_t slot = 0;
			std::uint32_t textured = 0, texturedTotal = 0;
			for (const auto& range : g_gpuModel.ranges) {
				if (slot >= 64) {
					break;
				}
				if (range.texSlot < 0 && IsDecalSheet(range.matPath)) {
					continue;  // untextured transparent sheet: don't hide the surface below
				}
				++texturedTotal;
				if (range.texSlot >= 0) {
					++textured;
				}				std::uint8_t* dst = g_pipe.cbMapped + static_cast<std::size_t>(slot) * 256;
				std::memcpy(dst, mvp, 64);
				std::memcpy(dst + 64, mv, 64);
				const float col[4]{ range.color[0], range.color[1], range.color[2], 0.0f };
				std::memcpy(dst + 128, col, 16);
				a_list->SetGraphicsRootConstantBufferView(0, cbBase + static_cast<std::uint64_t>(slot) * 256);
				const int texSlot = range.texSlot >= 0 ? range.texSlot : 0;
				D3D12_GPU_DESCRIPTOR_HANDLE th = texBase;
				th.ptr += static_cast<std::uint64_t>(texSlot) * g_pipe.srvIncrement;
				a_list->SetGraphicsRootDescriptorTable(1, th);
				// Decal sheets punch through their transparent texels instead
				// of paving the surface beneath (opaque textures are
				// unaffected: alpha reads 1 everywhere).
				if (range.texSlot < 0) {
					a_list->SetPipelineState(g_pipe.pso);
				} else if (IsDecalSheet(range.matPath)) {
					a_list->SetPipelineState(g_pipe.psoTexturedAlpha);
				} else {
					a_list->SetPipelineState(g_pipe.psoTextured);
				}
				a_list->DrawIndexedInstanced(range.count, 1, range.start, 0, 0);
				++slot;
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

			const float camDist = a_model.radius * 5.0f /
				(g_zoom * std::clamp(g_zoomMul.load(std::memory_order_relaxed), 0.35f, 4.0f));
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
		// mesh passes it — otherwise the whole item would vanish (e.g. a
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
		// wood -Z onto -Y, rear -Y onto +X — the inventory side profile
		// at the user-calibrated weapon camera.
		const bool layFlat = !a_weapon && a_model.extents[1] > 1.8f * a_model.extents[0] &&
			a_model.extents[1] > 1.8f * a_model.extents[2];
		for (const auto& mesh : a_model.meshes) {
				++meshIndex;
				// Skip tiny sub-meshes (attachment points, effects, decals)
				// that overlap the main geometry when rendered together —
				// unless they sprawl (soil discs, sights: MeshSpread).
				const std::size_t vertCount = mesh.pos.size() / 3;
				const std::size_t triCount = mesh.indices.size() / 3;
				if (anyBig && (triCount < 24 || vertCount < 12) && !MeshSpread(mesh, a_model.radius)) {
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
		const auto ini = DllDirectory() / L"SFShopExplorer.ini";
		g_enabled.store(GetPrivateProfileIntW(L"offlinepreview", L"enabled", 1, ini.c_str()) != 0, std::memory_order_release);
		g_gpuMode = GetPrivateProfileIntW(L"offlinepreview", L"gpu", 1, ini.c_str()) != 0;
		g_width = std::clamp(static_cast<std::uint32_t>(GetPrivateProfileIntW(L"offlinepreview", L"width", 512, ini.c_str())), 128u, 2048u);
		g_height = std::clamp(static_cast<std::uint32_t>(GetPrivateProfileIntW(L"offlinepreview", L"height", 384, ini.c_str())), 96u, 2048u);
		g_zoom = static_cast<float>(std::clamp(ParseFloat(ini, L"offlinepreview", L"zoom", 2.05), 1.0, 6.0));
		g_pitchDeg = static_cast<float>(ParseFloat(ini, L"offlinepreview", L"pitch", -22.0));
		g_yawDeg = static_cast<float>(ParseFloat(ini, L"offlinepreview", L"yaw", 32.0));
		g_weaponYaw = static_cast<float>(ParseFloat(ini, L"weaponview", L"wyaw", 40.0));
		g_weaponPitch = static_cast<float>(ParseFloat(ini, L"weaponview", L"wpitch", 20.0));
		wchar_t dir[MAX_PATH]{};
		GetPrivateProfileStringW(L"offlinepreview", L"dir", L"SFShopExplorerAssets\\models", dir, MAX_PATH, ini.c_str());
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
			if (g_requestKey == key && g_requestLabel == a_label && g_pendingPaths == a_modelPaths &&
				g_requestWeapon == a_weapon) {
				return;
			}
			g_requestKey = key;
			g_requestLabel = a_label;
			g_pendingPaths = a_modelPaths;
			g_pendingFallback = a_fallbackKey;
			g_requestWeapon = a_weapon;
			g_pendingModel.reset();  // stale model must not render for the new key
		}
		// New model, new inspection: restart from the INI camera (weapons
		// start at [weaponview] wyaw/wpitch, user-calibrated against the
		// inventory scene).
		ResetOrbit();
		g_orbitWeapon.store(a_weapon, std::memory_order_release);
		REX::INFO("SFShopExplorer offline: request '{}' <- '{}' ({} path(s))", a_label,
			a_modelPaths.empty() ? std::string{} : a_modelPaths.front(), a_modelPaths.size());
		g_requestGen.fetch_add(1, std::memory_order_acq_rel);
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
		// sniffing can't detect a prior merge — without this the mag
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
		// live-build FIRST — substring fallback would otherwise pick the
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
			// is namespaced asm_<edid> — NEVER the front NIF's key, which
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
					const auto it = g_cache.find(warm);
					if (it != g_cache.end()) {
						cached = it->second;
					}
				}
				if (cached) {
					PrefetchTextures(*cached, warm);
				}
			}
		}
		// Task thread: resolve the newest request (disk cache, aliases,
		// live in-game build) and rasterize it. Raster touches no GPU
		// state, so this never risks the render thread; the Present path
		// only copies finished pixels.
		const auto gen = g_requestGen.load(std::memory_order_acquire);
		if (gen == g_rasterGen) {
			return;
		}
		// Snapshot the request under the lock, then release before the
		// expensive resolve+rasterize so the render thread can still run.
		// NOTE: resolve from the REQUEST key, not the resolved pending key.
		std::string key, label, fallback;
		std::vector<std::string> paths;
		bool weapon = false;
		{
			std::scoped_lock lock(g_renderMutex);
			key = g_requestKey;
			label = g_requestLabel;
			paths = g_pendingPaths;
			fallback = g_pendingFallback;
			weapon = g_requestWeapon;
		}
		auto [model, resolvedKey] = ResolveModel(key, paths, fallback);
		REX::INFO("SFShopExplorer offline: resolve '{}' -> {} ('{}')", key,
			model ? "ready" : "empty", resolvedKey);
		if (model) {
			Rasterize(*model, weapon);
		} else {
			g_pixels.clear();
		}
		// Publish the completed request under the lock so UploadGPU sees
		// it atomically with the pixel data.
		{
			std::scoped_lock lock(g_renderMutex);
			g_pendingModel = model;
			g_pendingKey = resolvedKey;
			g_pendingLabel = label;
			g_publishedWeapon = weapon;
			g_rasterGen = gen;
		}
		// Textures decode after the pixels are published, so first paint
		// never waits on BA2 I/O; colors pop in via the dirty flag.
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
		// Interactive orbit asks for one more frame with the new camera.
		// Consumed every Present so a drag renders continuously; when a
		// fresh request is still rasterizing, the flag simply re-arms on
		// the next frame via the needUpload path below.
		const bool orbit = g_orbitDirty.exchange(false, std::memory_order_acq_rel);
		if (!needUpload && !orbit) {
			return;	// nothing new since the last upload
		}
		std::shared_ptr<const Model> model;
		std::vector<std::uint32_t> pixels;
		std::string key;
		bool weapon = false;
		{
			std::scoped_lock lock(g_renderMutex);
			if (g_rasterGen != gen && !g_pendingModel) {
				return;	// fresh request still resolving; retry next frame
			}
			model = g_pendingModel;
			pixels = g_pixels;
			key = g_pendingKey;
			weapon = g_publishedWeapon;
		}
		// CPU fallback needs fresh pixels when orbiting: the GPU path
		// re-renders itself below. Safe on this thread: with a settled
		// generation ServicePending is idle, so no concurrent Rasterize.
		const bool gpuActive = g_gpuMode && !IsGpuBanned(key);
		if (orbit && model && !gpuActive) {
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
			g_snapshot.height = g_height;
			if (!model) {
				g_snapshot.ready = false;
				g_snapshot.srvHandle = 0;
				g_snapshot.textured = 0;
				g_snapshot.texturedTotal = 0;
				g_snapshot.message = g_pendingKey.empty() ? std::string("this form carries no 3D model (ship / creature / ammo records usually have none)")
					: "no cached model for '" + g_pendingKey + "' (run build_model_cache.py)";
			}
		}
		// Independent GPU renderer first (own PSO/VB/RT, zero engine calls);
		// the CPU upload below stays as the fail-closed fallback.
		if (model && gpuActive) {
			CreateGpuTexture();
			if (g_gpu.texture && RenderGpuModel(list, *model, key, weapon)) {
				EnsurePreviewSrv();
				PublishReadySnapshot();
				return;
			}
			BanGpuKey(key);
			// GPU failed: the CPU pixels below carry no textures.
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
