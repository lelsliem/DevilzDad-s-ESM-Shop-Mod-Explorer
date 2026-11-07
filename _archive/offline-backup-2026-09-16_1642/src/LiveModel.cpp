// Live in-game model builder — own NIF 20.2.0.7 + .mesh + BA2 parsers.
//
// Format spec (validated byte-for-byte against MeshConverter ground truth):
//   NIF: header line, u32 ver==0x14020007, u8 endian==1, u32 uv, u32 nb,
//        u32 uv2, 16 opaque bytes (verified constant head/tail), u16 ntypes,
//        u32-len type strings, nb u16 typeIdx, nb u32 sizes, u32 nstrings,
//        u32 maxlen, u32-len strings, u32 ngroups (v1: must be 0), blocks.
//   NiNode/BSGeometry prefix: u32 nameIdx, u32 numExtra, i32 extras[],
//        i32 controller, u32 flags, 3f translation, 9f rotation, f32 scale,
//        i32 collision. NiNode adds u32 nchildren + i32 refs.
//   BSGeometry adds: 4f bound sphere, 3f reserved, 3f half-extents,
//        i32, i32 shaderRef, i32, then LOD entries [u8 flag, u32 nidx,
//        u32 nv, u32 unk, u32 len, bytes] to block end; first entry wins.
//   BSLightingShaderProperty (12B): u32 matIdx, u32, i32.
//   .mesh LOD0: u32 unk, u32 nidx, u16 idx[], f32 border, u32 weightsPerVert,
//        u32 nv, s16 xyz[] (pos = s16/32767 * border), then u32 nuv +
//        f16 uv[] and the remaining streams (normals/tangents/colors/weights).
//        NIF counts are advisory (mods ship stale LOD entries — trust the
//        file, still fully range-validated).
//   BA2: BTDX header, 36B entries (ext[4], off u64@12, size u32@24),
//        u16-len name table; zlib stream when first byte is 0x78.

#include "PCH.h"
#include "LiveModel.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <unordered_set>

#include <Windows.h>
#include <zlib.h>

#include <lz4.h>

namespace SFE::LiveModel
{
	namespace
	{
		std::mutex g_mutex;
		bool g_configLoaded{ false };
		bool g_enabled{ true };
		std::filesystem::path g_dataDir;
		std::vector<std::filesystem::path> g_extraDirs;
		std::vector<std::filesystem::path> g_extraBa2;
		std::filesystem::path g_cacheDir;
		// MO2 auto-discovery (mods override base game): BA2s + loose dirs
		// from ENABLED mods only (parsed from the active profile's
		// modlist.txt). Falls back to all present mod dirs when the MO2
		// layout cannot be determined (manual install).
		std::vector<std::filesystem::path> g_moBa2;
		std::vector<std::filesystem::path> g_moDirs;
		bool g_moProfileMode{ false };

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
				char l = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				out.push_back(l == '\\' ? '/' : l);
			}
			return out;
		}

		std::vector<std::filesystem::path> SplitPaths(const wchar_t* a_value)
		{
			std::vector<std::filesystem::path> out;
			std::wstring cur;
			for (const wchar_t* p = a_value; *p; ++p) {
				if (*p == L';') {
					if (!cur.empty()) {
						out.emplace_back(cur);
						cur.clear();
					}
				} else {
					cur.push_back(*p);
				}
			}
			if (!cur.empty()) {
				out.emplace_back(cur);
			}
			return out;
		}
	}

		// ------------------------------------------------------------------
		// Bounded little-endian reader: every access fails closed.
		// ------------------------------------------------------------------
		struct Reader
		{
			const std::uint8_t* p{ nullptr };
			std::size_t n{ 0 };
			std::size_t o{ 0 };
			bool ok{ true };

			bool Need(std::size_t k)
			{
				if (!ok || o + k > n) {
					ok = false;
					return false;
				}
				return true;
			}
			std::uint8_t U8()
			{
				if (!Need(1)) {
					return 0;
				}
				return p[o++];
			}
			std::uint16_t U16()
			{
				if (!Need(2)) {
					return 0;
				}
				std::uint16_t v;
				std::memcpy(&v, p + o, 2);
				o += 2;
				return v;
			}
			std::uint32_t U32()
			{
				if (!Need(4)) {
					return 0;
				}
				std::uint32_t v;
				std::memcpy(&v, p + o, 4);
				o += 4;
				return v;
			}
			std::int32_t I32() { return static_cast<std::int32_t>(U32()); }
			float F32()
			{
				std::uint32_t u = U32();
				float f;
				std::memcpy(&f, &u, 4);
				return f;
			}
			// u32 length-prefixed latin-1 string.
			std::string HStr(std::size_t a_cap = 1 << 20)
			{
				std::uint32_t ln = U32();
				if (!ok || ln > a_cap || !Need(ln)) {
					ok = false;
					return {};
				}
				std::string s(reinterpret_cast<const char*>(p + o), ln);
				o += ln;
				return s;
			}
		};

		// ------------------------------------------------------------------
		// BA2 archives: file tables cached per archive path.
		// ------------------------------------------------------------------
		struct Ba2Entry
		{
			std::uint64_t off{ 0 };
			std::uint32_t size{ 0 };
		};

		struct TexChunk
		{
			std::uint64_t off{ 0 };
			std::uint32_t packed{ 0 };
			std::uint32_t size{ 0 };
			std::uint16_t startMip{ 0 };
			std::uint16_t endMip{ 0 };
		};

		struct TexFile
		{
			std::uint16_t width{ 0 };
			std::uint16_t height{ 0 };
			std::uint8_t mips{ 0 };
			std::uint8_t dxgi{ 0 };
			std::vector<TexChunk> chunks;
		};

		struct Ba2Archive
		{
			std::ifstream file;
			std::unordered_map<std::string, Ba2Entry> entries;
			std::unordered_map<std::string, TexFile> texFiles;
			bool isTex{ false };
			std::uint32_t method{ 0 };  // 0 = zlib, 3 = LZ4 (v3 header; v2 is always zlib/raw)
			bool valid{ false };
		};

		std::unordered_map<std::string, std::shared_ptr<Ba2Archive>> g_ba2;
		std::unordered_set<std::string> g_failed;  // session negative cache
		std::vector<std::filesystem::path> g_ba2List;
		bool g_ba2ListBuilt{ false };

		std::shared_ptr<Ba2Archive> OpenBa2(const std::filesystem::path& a_path)
		{
			const auto key = LowerAscii(a_path.string());
			const auto it = g_ba2.find(key);
			if (it != g_ba2.end()) {
				return it->second;
			}
			auto ar = std::make_shared<Ba2Archive>();
			ar->file.open(a_path, std::ios::binary);
			do {
				if (!ar->file) {
					break;
				}
				char hdr[36]{};
				ar->file.read(hdr, 36);
				if (!ar->file || std::memcmp(hdr, "BTDX", 4) != 0) {
					break;
				}
				std::uint32_t hver = 0;
				char htyp[4]{};
				std::uint32_t num = 0;
				std::uint64_t ntOff = 0;
				std::memcpy(&hver, hdr + 4, 4);
				std::memcpy(htyp, hdr + 8, 4);
				std::memcpy(&num, hdr + 12, 4);
				std::memcpy(&ntOff, hdr + 16, 8);
				if (num == 0 || num > 4'000'000) {
					break;
				}
				ar->isTex = std::memcmp(htyp, "DX10", 4) == 0;
				if (hver >= 3) {
					std::memcpy(&ar->method, hdr + 32, 4);  // 0 = zlib, 3 = LZ4
				}
				struct RawEnt
				{
					char ext[4];
					std::uint64_t off;
					std::uint32_t size;
				};
				std::vector<std::string> names(num);
				std::vector<Ba2Entry> ents(num);
				std::vector<TexFile> texs(num);
				std::vector<char> entOk(num, 0);
				bool bad = false;
				for (std::uint32_t i = 0; i < num; ++i) {
					if (!ar->isTex) {
						char e[36]{};
						ar->file.read(e, 36);
						if (!ar->file) {
							bad = true;
							break;
						}
						Ba2Entry ent;
						std::memcpy(&ent.off, e + 12, 8);
						std::memcpy(&ent.size, e + 24, 4);
						// Archives may set flag bits in the size field;
						// keep the entry with masked size instead of
						// failing the whole archive over one record.
						ent.size &= 0x3FFFFFFFu;
						if (ent.size > (1ULL << 31)) {
							continue;
						}
						ents[i] = ent;
						entOk[i] = 1;
						continue;
					}
				// DX10 texture entry: fixed 24B header + 24B per chunk.
				// Stream discipline: the chunk bytes are ALWAYS consumed
				// first, validation second. Skipping validation BEFORE
				// consuming desyncs every later entry (their dims/dxgi/
				// chunk offsets parse as garbage — the "format 84
				// unsupported" ghost on textures whose header really
				// says 72 — silently dropping textures archive-wide).
				//
				// Two header generations exist (byte-proven against
				// Starfield - Textures11 v3 and SFBGS003 - Textures v2):
				//   v3: 4B prefix + ext/dirHash/flags/nchunks@13/len/
				//       H@16/W@18/mips@20/dxgi@21; chunks carry an
				//       absolute u64 file offset.
				//   v2: NO prefix (ext first), same tail shifted -4
				//       (nchunks@9, H@12, W@14, mips@16, dxgi@17);
				//       chunks carry NO offset:
				//       [zero,packed,size,mips32,flags,crc]. Chunk bytes
				//       are concatenated in record order; packed==0
				//       means stored raw (size bytes). The bulk base is
				//       resolved after the walk as (ntOff - totalBulk).
				const bool isV3 = hver >= 3;
				char h[24]{};
				ar->file.read(h, 24);
				if (!ar->file) {
					bad = true;
					break;
				}
				const std::uint8_t nchunks = static_cast<std::uint8_t>(isV3 ? h[13] : h[9]);
				if (nchunks > 64) {
					bad = true;  // unrecoverable walk; fail the archive loud
					break;
				}
				TexFile tf;
				std::uint16_t H = 0, W = 0;
				std::memcpy(&H, h + (isV3 ? 16 : 12), 2);
				std::memcpy(&W, h + (isV3 ? 18 : 14), 2);
				tf.height = H;
				tf.width = W;
				tf.mips = static_cast<std::uint8_t>(h[isV3 ? 20 : 16]);
				tf.dxgi = static_cast<std::uint8_t>(h[isV3 ? 21 : 17]);
					// Bethesda ships 8K debug/UI maps (debugmips.dds): the old
					// 4K cap failed the whole archive over one texture and
					// silently dropped everything sorted after it.
					bool cok = nchunks != 0 && W != 0 && W <= 16384 && H != 0 && H <= 16384;
					std::vector<TexChunk> readChunks;
					readChunks.reserve(nchunks);
				for (std::uint8_t c = 0; c < nchunks; ++c) {
					char ch[24]{};
					ar->file.read(ch, 24);
					if (!ar->file) {
						cok = false;
						break;
					}
					TexChunk tc;
					if (isV3) {
						std::memcpy(&tc.off, ch, 8);
						std::memcpy(&tc.packed, ch + 8, 4);
						std::memcpy(&tc.size, ch + 12, 4);
						std::memcpy(&tc.startMip, ch + 16, 2);
						std::memcpy(&tc.endMip, ch + 18, 2);
					} else {
						std::uint32_t packed = 0, size = 0, mips = 0;
						std::memcpy(&packed, ch + 4, 4);
						std::memcpy(&size, ch + 8, 4);
						std::memcpy(&mips, ch + 12, 4);
						tc.off = 0;  // resolved to bulkBase + cum below
						tc.packed = packed;
						tc.size = size;
						tc.startMip = static_cast<std::uint16_t>(mips & 0xFFFFu);
						tc.endMip = static_cast<std::uint16_t>((mips >> 16) & 0xFFFFu);
					}
					readChunks.push_back(tc);
				}
				// Validate AFTER consuming: every byte of the entry is
				// behind us either way, so the walk can never desync.
				if (cok) {
					for (const auto& tc : readChunks) {
						if (tc.size == 0 || tc.size > (1ULL << 28) || tc.packed > (1ULL << 28)) {
							cok = false;
							break;
						}
						tf.chunks.push_back(tc);
					}
				}
				// One degenerate texture (8K debug map, data texture with
				// odd dims) skips ITSELF now instead of failing the whole
				// archive (the old `bad = true` here is how every v2
				// DLC texture archive silently lost ALL its textures).
				if (!cok || tf.chunks.empty()) {
					continue;  // entOk[i] stays 0; walk continues aligned
				}
				texs[i] = std::move(tf);
				entOk[i] = 1;
			}
			// v2 bulk resolution: chunk bytes are concatenated in record
			// order with no per-chunk offsets. Self-calibrating base: the
			// bulk ends where the name table begins (byte-exact on
			// SFBGS003/004 and ShatteredSpace; each decode verified with
			// zlib). packed==0 chunks contribute their raw size.
			if (!bad && ar->isTex && hver < 3) {
				std::uint64_t totalBulk = 0;
				for (std::uint32_t i = 0; i < num; ++i) {
					if (!entOk[i]) {
						continue;
					}
					for (const auto& tc : texs[i].chunks) {
						totalBulk += tc.packed != 0 ? tc.packed : tc.size;
					}
				}
				if (totalBulk == 0 || totalBulk > ntOff) {
					REX::WARN("SFShopExplorer live: v2 bulk math failed {} (total {})",
						a_path.filename().string(), totalBulk);
					std::fill(entOk.begin(), entOk.end(), 0);
				} else {
					const std::uint64_t base = ntOff - totalBulk;
					std::uint64_t cum = 0;
					for (std::uint32_t i = 0; i < num; ++i) {
						if (!entOk[i]) {
							continue;
						}
						for (auto& tc : texs[i].chunks) {
							tc.off = base + cum;
							cum += tc.packed != 0 ? tc.packed : tc.size;
						}
					}
				}
			}
				if (bad) {
					break;
				}
				ar->file.seekg(static_cast<std::streamoff>(ntOff));
				for (std::uint32_t i = 0; i < num; ++i) {
					char lnB[2]{};
					ar->file.read(lnB, 2);
					if (!ar->file) {
						bad = true;
						break;
					}
					std::uint16_t ln = 0;
					std::memcpy(&ln, lnB, 2);
					if (ln == 0 || ln > 512) {
						bad = true;
						break;
					}
					std::string nm(ln, '\0');
					ar->file.read(nm.data(), ln);
					if (!ar->file) {
						bad = true;
						break;
					}
					names[i] = LowerAscii(nm);
				}
				if (bad) {
					break;
				}
				for (std::uint32_t i = 0; i < num; ++i) {
					if (!entOk[i]) {
						continue;
					}
					if (ar->isTex) {
						ar->texFiles.emplace(names[i], std::move(texs[i]));
					} else {
						ar->entries.emplace(names[i], ents[i]);
					}
				}
				ar->valid = true;
			} while (false);
			if (!ar->valid) {
				ar->file.close();
			}
			g_ba2.emplace(key, ar);
			if (ar->valid) {
				REX::INFO("SFShopExplorer live: archive {} ({} entries)", a_path.filename().string(),
					ar->entries.size() + ar->texFiles.size());
			} else {
				REX::WARN("SFShopExplorer live: archive failed {}", a_path.string());
			}
			return ar;
		}

		bool Ba2Read(const std::shared_ptr<Ba2Archive>& a_ar, const Ba2Entry& a_e, std::vector<std::uint8_t>& a_out)
		{
			if (!a_ar->valid || a_e.size == 0 || a_e.size > (1ULL << 31)) {
				return false;
			}
			a_ar->file.clear();
			a_ar->file.seekg(static_cast<std::streamoff>(a_e.off));
			a_out.resize(a_e.size);
			a_ar->file.read(reinterpret_cast<char*>(a_out.data()), a_e.size);
			if (!a_ar->file) {
				a_out.clear();
				return false;
			}
			if (a_out[0] != 0x78) {
				return true;  // stored uncompressed
			}
			// zlib stream: inflate with a growing destination.
			std::size_t cap = static_cast<std::size_t>(a_e.size) * 4 + 1024;
			std::vector<std::uint8_t> dst;
			for (int attempt = 0; attempt < 8; ++attempt) {
				if (cap > (1ULL << 29)) {
					return false;
				}
				dst.resize(cap);
				uLongf dstLen = static_cast<uLongf>(cap);
				const int zr = uncompress(dst.data(), &dstLen, a_out.data(), static_cast<uLong>(a_out.size()));
				if (zr == Z_OK) {
					dst.resize(dstLen);
					a_out.swap(dst);
					return true;
				}
				if (zr != Z_BUF_ERROR) {
					return false;
				}
				cap *= 4;
			}
			return false;
		}

		void EnsureBa2List()
		{
			if (g_ba2ListBuilt) {
				return;
			}
			g_ba2ListBuilt = true;
			std::error_code ec;
			std::vector<std::filesystem::path> dataPart;
			if (!g_dataDir.empty() && std::filesystem::is_directory(g_dataDir, ec)) {
				for (const auto& e : std::filesystem::directory_iterator(g_dataDir, ec)) {
					if (e.is_regular_file(ec) && LowerAscii(e.path().extension().string()) == ".ba2") {
						dataPart.push_back(e.path());
					}
				}
				std::sort(dataPart.begin(), dataPart.end());
			}
			// Mod-override semantics: explicit INI extraba2, then MO2
			// auto-discovered mod archives, resolve BEFORE base game data
			// (a mod replacing a base NIF/texture must win the lookup).
			std::vector<std::filesystem::path> modsPart;
			for (const auto& p : g_extraBa2) {
				modsPart.push_back(p);
			}
			for (const auto& p : g_moBa2) {
				modsPart.push_back(p);
			}
			// Meshes-bearing archives first within each part: NIF/.mesh
			// lookups almost always resolve there, so the first hover
			// costs seconds, not minutes. (Voices/animations/etc. are
			// still scanned as a fallback.)
			const auto meshFirst = [](const std::filesystem::path& p) {
				return LowerAscii(p.filename().string()).find("mesh") != std::string::npos;
			};
			std::stable_partition(modsPart.begin(), modsPart.end(), meshFirst);
			std::stable_partition(dataPart.begin(), dataPart.end(), meshFirst);
			g_ba2List.reserve(modsPart.size() + dataPart.size());
			g_ba2List.insert(g_ba2List.end(), modsPart.begin(), modsPart.end());
			g_ba2List.insert(g_ba2List.end(), dataPart.begin(), dataPart.end());
			REX::INFO("SFShopExplorer live: {} BA2 archives indexed ({} mod override, mesh archives first)",
				g_ba2List.size(), modsPart.size());
		}

		bool ReadLoose(const std::filesystem::path& a_path, std::vector<std::uint8_t>& a_out)
		{
			std::error_code ec;
			const auto sz = std::filesystem::file_size(a_path, ec);
			if (ec || sz == 0 || sz > (1ULL << 30)) {
				return false;
			}
			std::ifstream f(a_path, std::ios::binary);
			if (!f) {
				return false;
			}
			a_out.resize(static_cast<std::size_t>(sz));
			f.read(reinterpret_cast<char*>(a_out.data()), static_cast<std::streamsize>(sz));
			return static_cast<bool>(f);
		}

		// Resolve a game-relative asset key ("meshes/..." or "geometries/...").
		// Loose order: explicit INI extradirs, MO2 mod dirs, base Data
		// (mod-override semantics); then the BA2 list (already mod-first).
		bool ResolveAsset(const std::string& a_key, std::vector<std::uint8_t>& a_out)
		{
			std::filesystem::path rel;
			{
				std::string k = a_key;
				std::replace(k.begin(), k.end(), '/', static_cast<char>(std::filesystem::path::preferred_separator));
				rel = std::filesystem::path(k);
			}
			for (const auto& dir : g_extraDirs) {
				if (ReadLoose(dir / rel, a_out)) {
					return true;
				}
			}
			for (const auto& dir : g_moDirs) {
				if (ReadLoose(dir / rel, a_out)) {
					return true;
				}
			}
			if (!g_dataDir.empty() && ReadLoose(g_dataDir / rel, a_out)) {
				return true;
			}
			EnsureBa2List();
			for (const auto& ap : g_ba2List) {
				auto ar = OpenBa2(ap);
				if (!ar->valid) {
					continue;
				}
				const auto it = ar->entries.find(a_key);
				if (it == ar->entries.end()) {
					continue;
				}
				if (Ba2Read(ar, it->second, a_out)) {
					return true;
				}
			}
			return false;
		}

		// ------------------------------------------------------------------
		// 4x4 row-major matrices, row-vector convention (v' = v*M).
		// ------------------------------------------------------------------
		struct Mat4
		{
			float m[16]{ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
		};

		Mat4 Mul(const Mat4& a, const Mat4& b)
		{
			Mat4 r;
			for (int i = 0; i < 4; ++i) {
				for (int j = 0; j < 4; ++j) {
					r.m[i * 4 + j] = a.m[i * 4 + 0] * b.m[0 * 4 + j] + a.m[i * 4 + 1] * b.m[1 * 4 + j] +
						a.m[i * 4 + 2] * b.m[2 * 4 + j] + a.m[i * 4 + 3] * b.m[3 * 4 + j];
				}
			}
			return r;
		}

		Mat4 LocalMat(const float a_t[3], const float a_rot[9], float a_scale)
		{
			Mat4 r;
			r.m[0] = a_rot[0] * a_scale;
			r.m[1] = a_rot[1] * a_scale;
			r.m[2] = a_rot[2] * a_scale;
			r.m[3] = a_t[0];
			r.m[4] = a_rot[3] * a_scale;
			r.m[5] = a_rot[4] * a_scale;
			r.m[6] = a_rot[5] * a_scale;
			r.m[7] = a_t[1];
			r.m[8] = a_rot[6] * a_scale;
			r.m[9] = a_rot[7] * a_scale;
			r.m[10] = a_rot[8] * a_scale;
			r.m[11] = a_t[2];
			r.m[12] = r.m[13] = r.m[14] = 0.0f;
			r.m[15] = 1.0f;
			return r;
		}

		// ------------------------------------------------------------------
		// NIF 20.2.0.7 parser.
		// ------------------------------------------------------------------
		struct NifGeo
		{
			Mat4 world;
			std::string node;     // geometry block name (texture matching)
			std::string factory;  // "guid\guid" as stored
			std::string matPath;
			std::uint32_t lodVerts{ 0 };
			std::uint32_t lodIndices{ 0 };
		};

		struct NifDoc
		{
			std::vector<std::string> types;
			std::vector<std::uint16_t> typeIdx;
			std::vector<std::uint32_t> sizes;
			std::vector<std::size_t> offsets;
			std::vector<std::string> strings;
			int niNode{ -1 };
			int bsGeo{ -1 };
		};

		struct NodePart
		{
			std::uint32_t name{ 0 };
			float t[3]{ 0, 0, 0 };
			float rot[9]{ 1, 0, 0, 0, 1, 0, 0, 0, 1 };
			float scale{ 1.0f };
			std::size_t end{ 0 };
			bool ok{ false };
		};

		bool ParseNodePart(const std::uint8_t* a_blk, std::size_t a_len, NodePart& a_out)
		{
			Reader r{ a_blk, a_len, 0, true };
			a_out.name = r.U32();
			const std::uint32_t ne = r.U32();
			if (!r.ok || ne > 1024) {
				return false;
			}
			for (std::uint32_t i = 0; i < ne; ++i) {
				(void)r.I32();
			}
			(void)r.I32();  // controller
			(void)r.U32();  // flags
			a_out.t[0] = r.F32();
			a_out.t[1] = r.F32();
			a_out.t[2] = r.F32();
			for (int i = 0; i < 9; ++i) {
				a_out.rot[i] = r.F32();
			}
			a_out.scale = r.F32();
			if (!r.ok || !(a_out.scale > 0.0f) || a_out.scale > 1e6f) {
				return false;
			}
			(void)r.I32();  // collision ref
			if (!r.ok) {
				return false;
			}
			a_out.end = r.o;
			a_out.ok = true;
			return true;
		}

		bool ParseNifGeos(const std::vector<std::uint8_t>& a_bytes, std::vector<NifGeo>& a_out,
			std::string& a_stage, std::string& a_detail)
		{
			a_stage = "short";
			if (a_bytes.size() < 64) {
				return false;
			}
			// Header line.
			std::size_t nl = 0;
			while (nl < a_bytes.size() && nl < 128 && a_bytes[nl] != '\n') {
				++nl;
			}
			if (nl >= a_bytes.size() || nl >= 128) {
				return false;
			}
			Reader r{ a_bytes.data(), a_bytes.size(), nl + 1, true };
			a_stage = "version";
			if (r.U32() != 0x14020007u || r.U8() != 1) {
				return false;
			}
			(void)r.U32();  // user version
			const std::uint32_t nb = r.U32();
			if (!r.ok || nb == 0 || nb > 100000) {
				return false;
			}
			(void)r.U32();  // user version 2
			// Post-uv2 region varies by exporter: Bethesda's stamp (8
			// constant head bytes + variant stamp + u16 tail) or a
			// third-party block (e.g. Outfit Studio author strings).
			// Sync by trial over a wide window: the first (offset, width)
			// pair that yields a fully valid type list + block index table
			// wins. Type strings may be u16- or u32-prefixed per file.
			a_stage = "typesync";
			static constexpr std::uint8_t kHead[8]{ 0x01, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00 };
			const bool hasHead = r.Need(8) && std::memcmp(r.p + r.o, kHead, 8) == 0;
			NifDoc doc;
			bool synced = false;
			std::uint16_t syncedNt = 0;
			const std::size_t skipBase = hasHead ? 8 : 0;
			// Wide window covers author-string blocks; every candidate is
			// fully validated (types ASCII + index table + sizes sum later).
			const std::size_t skipMax = skipBase + 192;
			for (std::size_t skip = skipBase; skip <= skipMax && !synced; ++skip) {
				for (int wide = 1; wide >= 0 && !synced; --wide) {
					Reader t{ r.p, r.n, r.o + skip, true };
					const std::uint16_t nt = t.U16();
					if (!t.ok || nt == 0 || nt > 128) {
						continue;
					}
					std::vector<std::string> types;
					bool ok = true;
					for (std::uint16_t i = 0; i < nt && ok; ++i) {
						std::uint32_t ln = 0;
						if (wide) {
							ln = t.U32();
						} else {
							ln = t.U16();
						}
						if (!t.ok || ln == 0 || ln > 96 || !t.Need(ln)) {
							ok = false;
							break;
						}
						bool ascii = true;
						for (std::uint32_t k = 0; k < ln; ++k) {
							const char c = static_cast<char>(t.p[t.o + k]);
							if (c < 32 || c > 126) {
								ascii = false;
								break;
							}
						}
						if (!ascii) {
							ok = false;
							break;
						}
						types.emplace_back(reinterpret_cast<const char*>(t.p + t.o), ln);
						t.o += ln;
					}
					if (!ok) {
						continue;
					}
					// At least one structural type must be present.
					bool hasNode = false, hasGeo = false;
					for (const auto& ty : types) {
						if (ty == "NiNode") {
							hasNode = true;
						} else if (ty == "BSGeometry") {
							hasGeo = true;
						}
					}
					if (!hasNode) {
						continue;
					}
					(void)hasGeo;
					// Block index table must reference only known types.
					if (!t.Need(static_cast<std::size_t>(nb) * 2)) {
						continue;
					}
					for (std::uint32_t i = 0; i < nb; ++i) {
						std::uint16_t ti = 0;
						std::memcpy(&ti, t.p + t.o + i * 2, 2);
						if (ti >= nt) {
							ok = false;
							break;
						}
					}
					if (!ok) {
						continue;
					}
					doc.types = std::move(types);
					syncedNt = nt;
					r.o = t.o;
					synced = true;
				}
			}
			if (!synced) {
				std::string peek;
				{
					char hb[48]{};
					const std::size_t avail = r.n - r.o < 48 ? r.n - r.o : 48;
					for (std::size_t i = 0; i < avail; ++i) {
						char tmp[4]{};
						snprintf(tmp, sizeof(tmp), "%02x", r.p[r.o + i]);
						peek += tmp;
					}
				}
				REX::WARN("SFShopExplorer live: typesync diag size={} nb={} head={}", a_bytes.size(), nb, peek);
				return false;
			}
			a_stage = "tables";
			for (std::uint32_t i = 0; i < nb; ++i) {
				doc.typeIdx.push_back(r.U16());
			}
			for (std::uint32_t i = 0; i < nb; ++i) {
				const std::uint32_t s = r.U32();
				if (s > (1U << 28)) {
					return false;
				}
				doc.sizes.push_back(s);
			}
			if (!r.ok) {
				return false;
			}
			a_stage = "strings";
			const std::uint32_t ns = r.U32();
			(void)r.U32();  // max string length
			if (!r.ok || ns > 200000) {
				return false;
			}
			for (std::uint32_t i = 0; i < ns; ++i) {
				doc.strings.push_back(r.HStr(1 << 16));
			}
			if (!r.ok) {
				return false;
			}
			a_stage = "groups";
			const std::uint32_t ng = r.U32();
			if (!r.ok || ng != 0) {
				return false;  // v1: no block groups
			}
			for (std::uint16_t i = 0; i < syncedNt; ++i) {
				if (doc.types[i] == "NiNode") {
					doc.niNode = i;
				} else if (doc.types[i] == "BSGeometry") {
					doc.bsGeo = i;
				}
			}
			if (doc.niNode < 0 || doc.bsGeo < 0) {
				a_stage = "notypes";
				a_detail.clear();
				for (std::size_t i = 0; i < doc.types.size() && a_detail.size() < 220; ++i) {
					if (i) {
						a_detail.push_back(',');
					}
					a_detail += doc.types[i];
				}
				return false;
			}
			a_stage = "blocks";
			doc.offsets.resize(nb);
			{
				std::size_t p = r.o;
				std::uint64_t sum = 0;
				for (std::uint32_t i = 0; i < nb; ++i) {
					if (p + doc.sizes[i] > a_bytes.size()) {
						return false;
					}
					doc.offsets[i] = p;
					p += doc.sizes[i];
					sum += doc.sizes[i];
				}
				// Block payload must account for the whole file tail
				// (tolerating a small footer; rattler has 8 bytes).
				const std::uint64_t tail = static_cast<std::uint64_t>(a_bytes.size()) - r.o;
				if (sum > tail || tail - sum > 64) {
					return false;
				}
			}
			// Root: the NiNode nobody references (block 0 in practice).
			std::vector<char> referenced(nb, 0);
			for (std::uint32_t i = 0; i < nb; ++i) {
				if (static_cast<int>(doc.typeIdx[i]) != doc.niNode) {
					continue;
				}
				const auto* blk = a_bytes.data() + doc.offsets[i];
				NodePart np;
				if (!ParseNodePart(blk, doc.sizes[i], np)) {
					continue;
				}
				Reader cr{ blk, doc.sizes[i], np.end, true };
				const std::uint32_t nch = cr.U32();
				if (!cr.ok || nch > 4096) {
					continue;
				}
				for (std::uint32_t k = 0; k < nch; ++k) {
					const std::int32_t c = cr.I32();
					if (c >= 0 && static_cast<std::uint32_t>(c) < nb) {
						referenced[c] = 1;
					}
				}
			}
			std::int32_t root = -1;
			for (std::uint32_t i = 0; i < nb; ++i) {
				if (static_cast<int>(doc.typeIdx[i]) == doc.niNode && !referenced[i]) {
					root = static_cast<std::int32_t>(i);
					break;
				}
			}
			if (root < 0) {
				if (static_cast<int>(doc.typeIdx[0]) != doc.niNode) {
					a_stage = "noroot";
					return false;
				}
				root = 0;
			}
			// Walk with world-matrix accumulation.
			struct Frame
			{
				std::int32_t block;
				Mat4 parent;
			};
			std::vector<Frame> stack{ Frame{ root, Mat4{} } };
			std::size_t guard = 0;
			while (!stack.empty()) {
				if (++guard > 100000 || a_out.size() > 2048) {
					return false;
				}
				Frame fr = stack.back();
				stack.pop_back();
				const auto* blk = a_bytes.data() + doc.offsets[fr.block];
				const std::size_t blen = doc.sizes[fr.block];
				NodePart np;
				if (!ParseNodePart(blk, blen, np)) {
					continue;
				}
				const Mat4 world = Mul(fr.parent, LocalMat(np.t, np.rot, np.scale));
				const bool isGeo = static_cast<int>(doc.typeIdx[fr.block]) == doc.bsGeo;
				Reader cr{ blk, blen, np.end, true };
				if (isGeo) {
					NifGeo geo;
					geo.world = world;
					geo.node = np.name < doc.strings.size() ? doc.strings[np.name] : std::string{};
					float sphere[4]{ 0, 0, 0, 0 };
					for (int k = 0; k < 4; ++k) {
						sphere[k] = cr.F32();
					}
					(void)sphere;
					for (int k = 0; k < 6; ++k) {
						(void)cr.F32();  // reserved + half-extents
					}
					(void)cr.I32();
					const std::int32_t shader = cr.I32();
					(void)cr.I32();
					if (!cr.ok) {
						continue;
					}
					// LOD entries to block end; first wins.
					bool got = false;
					while (cr.o + 18 <= blen) {
						(void)cr.U8();  // entry flag
						const std::uint32_t ni = cr.U32();
						const std::uint32_t nv = cr.U32();
						(void)cr.U32();  // unk
						const std::uint32_t ln = cr.U32();
						if (!cr.ok || ln == 0 || ln > 512 || ni == 0 || ni > 20'000'000 || nv == 0 || nv > 10'000'000) {
							break;
						}
						if (!cr.Need(ln)) {
							break;
						}
						std::string path(reinterpret_cast<const char*>(blk + cr.o), ln);
						cr.o += ln;
						if (!got) {
							geo.factory = path;
							geo.lodIndices = ni;
							geo.lodVerts = nv;
							got = true;
						}
					}
					if (!got) {
						continue;
					}
					if (shader < 0 || static_cast<std::uint32_t>(shader) >= nb ||
						static_cast<int>(doc.typeIdx[shader]) < 0) {
						continue;
					}
					const auto* sblk = a_bytes.data() + doc.offsets[shader];
					Reader sr{ sblk, doc.sizes[shader], 0, true };
					const std::uint32_t matIdx = sr.U32();
					if (!sr.ok || matIdx >= doc.strings.size()) {
						continue;
					}
					geo.matPath = doc.strings[matIdx];
					a_out.push_back(std::move(geo));
				} else if (static_cast<int>(doc.typeIdx[fr.block]) == doc.niNode) {
					const std::uint32_t nch = cr.U32();
					if (!cr.ok || nch > 4096) {
						continue;
					}
					for (std::uint32_t k = 0; k < nch; ++k) {
						const std::int32_t c = cr.I32();
						if (c >= 0 && static_cast<std::uint32_t>(c) < nb) {
							stack.push_back(Frame{ c, world });
						}
					}
				}
			}
			if (a_out.empty()) {
				a_stage = "nogeo";
			}
			return !a_out.empty();
		}

		// ------------------------------------------------------------------
		// .mesh LOD0 parser: indices + border-scaled s16 positions + f16 UVs.
		//
		// Verified layout (every vertex byte-matched against MeshConverter
		// ImportMesh ground truth on skinned outfit, rigid dataslate, ore):
		//   u32 unk, u32 nidx, u16 idx[nidx],
		//   f32 border, u32 weightsPerVert, u32 nv, s16 xyz[nv],
		//   u32 nuv, f16 uv[nuv], ...
		// Positions are s16/32767 * border (border = max_border, e.g. 2.0
		// for outfits, 1.0 for small rigid items). The NIF LOD entry gives
		// advisory nidx/nv (mods ship stale entries — trust the file).
		// There is no second layout — the old "+8 index shift for
		// clothing" misread the border/weights header as indices and
		// produced exploded renders (every triangle misconnected).
		// ------------------------------------------------------------------
		float HalfToFloat(std::uint16_t h)
		{
			const std::uint32_t s = (h >> 15) & 1u;
			const std::uint32_t e = (h >> 10) & 0x1fu;
			const std::uint32_t m = h & 0x3ffu;
			std::uint32_t f;
			if (e == 0) {
				f = s << 31;  // zero/subnormal: UVs only need zero here
			} else if (e == 31) {
				f = (s << 31) | 0x7f800000u | (m << 13);
			} else {
				f = (s << 31) | ((e + 112u) << 23) | (m << 13);
			}
			float r;
			std::memcpy(&r, &f, 4);
			return r;
		}

		bool ParseMeshLod0(const std::vector<std::uint8_t>& a_bytes, std::uint32_t a_expectIdx,
			std::uint32_t a_expectVerts, std::vector<float>& a_pos, std::vector<float>& a_uv,
			std::vector<std::uint32_t>& a_idx)
		{
			Reader r{ a_bytes.data(), a_bytes.size(), 0, true };
			(void)r.U32();  // unk header
			const std::uint32_t ni = r.U32();
			if (!r.ok || ni == 0 || ni > 20'000'000) {
				return false;
			}
			// Mod reality check: authors update loose .mesh files without
			// refreshing the NIF LOD entry (proven: 4-Eyes upperbody NIF says
			// 30186i/18744v, its .mesh holds 81156i/28588v). A mismatch warns
			// and trusts the FILE counts — every index is still range-checked
			// and the border still sanity-checked below, so a wrong layout
			// still fails closed. Strict equality would brick mod items.
			if (a_expectIdx != 0 && ni != a_expectIdx) {
				REX::WARN("SFShopExplorer live: mesh index count differs from NIF (nif {}i, mesh {}i); trusting file",
					a_expectIdx, ni);
			}
			if (!r.Need(static_cast<std::size_t>(ni) * 2)) {
				return false;
			}
			std::vector<std::uint32_t> idx;
			idx.reserve(ni);
			for (std::uint32_t k = 0; k < ni; ++k) {
				idx.push_back(r.U16());
			}
			// Vertex header: border scale + weights-per-vert + count.
			const float border = r.F32();
			if (!r.ok || !(border > 0.0f) || !(border < 1e6f)) {
				return false;  // rejects NaN/inf/zero via the negated compares
			}
			(void)r.U32();  // weightsPerVert: stream selector, not needed here
			const std::uint32_t nv = r.U32();
			if (!r.ok) {
				return false;
			}
			if (a_expectVerts != 0 && nv != a_expectVerts) {
				REX::WARN("SFShopExplorer live: mesh vert count differs from NIF (nif {}v, mesh {}v); trusting file",
					a_expectVerts, nv);
			}
			if (nv == 0 || nv > 10'000'000 || !r.Need(static_cast<std::size_t>(nv) * 6)) {
				return false;
			}
			// Every index must address a real vertex — a single OOB index
			// means the layout assumption is wrong for this file, so fail
			// closed instead of rendering shards.
			for (const auto id : idx) {
				if (id >= nv) {
					REX::WARN("SFShopExplorer live: index out of range ({} >= {}v)", id, nv);
					return false;
				}
			}
			std::vector<float> pos(static_cast<std::size_t>(nv) * 3);
			std::size_t posEnd = 0;
			{
				Reader v{ r.p, r.n, r.o, true };
				for (std::uint32_t q = 0; q < nv; ++q) {
					const std::int16_t x = static_cast<std::int16_t>(v.U16());
					const std::int16_t y = static_cast<std::int16_t>(v.U16());
					const std::int16_t z = static_cast<std::int16_t>(v.U16());
					if (!v.ok) {
						break;
					}
					pos[static_cast<std::size_t>(q) * 3 + 0] = static_cast<float>(x) / 32767.0f * border;
					pos[static_cast<std::size_t>(q) * 3 + 1] = static_cast<float>(y) / 32767.0f * border;
					pos[static_cast<std::size_t>(q) * 3 + 2] = static_cast<float>(z) / 32767.0f * border;
				}
				if (!v.ok) {
					return false;
				}
				posEnd = v.o;
			}
		// UV stream: u32 count + f16 pairs. Best-effort: geometry
		// stays valid untextured when UVs are absent.
		std::vector<float> uv;
				{
					Reader u{ r.p, r.n, posEnd, true };
					const std::uint32_t nuv = u.U32();
					if (u.ok && nuv == nv && u.Need(static_cast<std::size_t>(nv) * 4)) {
						uv.resize(static_cast<std::size_t>(nv) * 2);
						for (std::uint32_t q = 0; q < nv; ++q) {
							uv[static_cast<std::size_t>(q) * 2 + 0] = HalfToFloat(u.U16());
							uv[static_cast<std::size_t>(q) * 2 + 1] = HalfToFloat(u.U16());
						}
						if (!u.ok) {
							uv.clear();
						}
					}
				}
			a_pos = std::move(pos);
			a_uv = std::move(uv);
			a_idx = std::move(idx);
			return true;
	}



	static void DiscoverMoMods(const std::filesystem::path& a_dllDir);

	void LoadConfig()
	{
		std::scoped_lock lock(g_mutex);
		if (g_configLoaded) {
			return;
		}
		g_configLoaded = true;
		const auto ini = DllDirectory() / L"SFShopExplorer.ini";
		g_enabled = GetPrivateProfileIntW(L"livemodel", L"enabled", 1, ini.c_str()) != 0;
		// <game>/Data sits three levels above <game>/Data/SFSE/Plugins.
		const auto dllDir = DllDirectory();
		auto root = dllDir;
		for (int i = 0; i < 3 && root.has_parent_path(); ++i) {
			root = root.parent_path();
		}
		const auto dataDir = root.parent_path() / L"Data";
		std::error_code ec;
		if (std::filesystem::is_directory(dataDir, ec)) {
			g_dataDir = dataDir;
		} else if (std::filesystem::is_directory(dllDir.parent_path().parent_path(), ec)) {
			g_dataDir = dllDir.parent_path().parent_path();  // already at Data
		}
		wchar_t buf[8192]{};
		GetPrivateProfileStringW(L"livemodel", L"extradirs", L"", buf, 8192, ini.c_str());
		g_extraDirs = SplitPaths(buf);
		GetPrivateProfileStringW(L"livemodel", L"extraba2", L"", buf, 8192, ini.c_str());
		g_extraBa2 = SplitPaths(buf);
		DiscoverMoMods(dllDir);
		wchar_t dir[MAX_PATH]{};
		GetPrivateProfileStringW(L"offlinepreview", L"dir", L"SFShopExplorerAssets\\models", dir, MAX_PATH, ini.c_str());
		const std::filesystem::path rel(dir);
		g_cacheDir = rel.is_absolute() ? rel : DllDirectory() / rel;
		REX::INFO("SFShopExplorer live: enabled={} data={} extradirs={} extraba2={} momods={}{}",
			g_enabled, g_dataDir.string(), g_extraDirs.size(), g_extraBa2.size(), g_moDirs.size(),
			g_moProfileMode ? " (profile)" : " (all-present)");
	}

	// MO2 mod auto-discovery. The DLL usually lives at
	// <mo2>\<Game>\mods\<ModName>\SFSE\Plugins\, so walking up to a dir
	// named "mods" yields the mods root. Enabled mods come from the active
	// profile's modlist.txt (`+Name` lines; separators/comments skipped);
	// ModOrganizer.ini's selected_profile names the profile. Every enabled
	// mod dir contributes loose files + root *.ba2, searched BEFORE base
	// game data (mod-override semantics). Best-effort throughout: any
	// failure falls back to all present mod dirs, then to nothing.
	static void DiscoverMoMods(const std::filesystem::path& a_dllDir)
	{
		std::error_code ec;
		auto dir = a_dllDir;
		std::filesystem::path modsRoot;
		for (int i = 0; i < 10 && !dir.empty(); ++i) {
			if (LowerAscii(dir.filename().string()) == "mods") {
				modsRoot = dir;
				break;
			}
			dir = dir.parent_path();
		}
		// Fallback: MO2 global instance lives at <MO2>\mods when the DLL is
		// mounted via USVFS the ancestor walk can land on B:\ vs B:\MO2.
		if (modsRoot.empty()) {
			for (const auto& cand : { a_dllDir / L"..\\..\\..\\mods", a_dllDir / L"..\\..\\mods" }) {
				std::error_code e2;
				auto p = std::filesystem::weakly_canonical(cand, e2);
				if (!e2 && std::filesystem::is_directory(p, e2)) {
					modsRoot = p;
					break;
				}
			}
		}
		// Last resort: hard-known path for this install.
		if (modsRoot.empty()) {
			std::error_code e2;
			auto p = std::filesystem::path(L"B:\\MO2\\Starfield\\mods");
			if (std::filesystem::is_directory(p, e2)) {
				modsRoot = p;
			}
		}
		if (modsRoot.empty() || !std::filesystem::is_directory(modsRoot, ec)) {
			REX::WARN("SFShopExplorer live: MO2 mods root not found from {}", a_dllDir.string());
			return;
		}
		// All present mod dirs (fallback set).
		std::vector<std::filesystem::path> present;
		for (const auto& e : std::filesystem::directory_iterator(modsRoot, ec)) {
			if (e.is_directory(ec)) {
				present.push_back(e.path());
			}
		}
		// Active profile: <gameDir>\ModOrganizer.ini or its parent holds
		// selected_profile; profiles live in <gameDir>\profiles\<name>\.
		// gameDir = <mods>\.. → <MO2>\<Game>. Probe three locations: sibling,
		// MO2 root, and the global B:\MO2.
		std::filesystem::path gameDir = modsRoot.parent_path();
		std::vector<std::filesystem::path> moIniCandidates;
		moIniCandidates.push_back(gameDir / L"ModOrganizer.ini");
		moIniCandidates.push_back(gameDir.parent_path() / L"ModOrganizer.ini");
		moIniCandidates.push_back(std::filesystem::path(L"B:\\MO2\\ModOrganizer.ini"));
		std::filesystem::path moIni;
		for (const auto& c : moIniCandidates) {
			if (std::filesystem::is_regular_file(c, ec)) {
				moIni = c;
				break;
			}
		}
		std::wstring profile;
		{
			wchar_t val[512]{};
			if (!moIni.empty() && std::filesystem::is_regular_file(moIni, ec)) {
				GetPrivateProfileStringW(L"General", L"selected_profile", L"", val, 512, moIni.c_str());
				profile = val;
				// @ByteArray(Test profile) encoding.
				if (profile.compare(0, 11, L"@ByteArray(") == 0 && !profile.empty() && profile.back() == L')') {
					profile = profile.substr(11, profile.size() - 12);
				}
				// Trim stray @ByteArray prefix remnants / whitespace.
				while (!profile.empty() && (profile.back() == L'\r' || profile.back() == L'\n' ||
					profile.back() == L' ' || profile.back() == L'\t')) {
					profile.pop_back();
				}
				while (!profile.empty() && (profile.front() == L' ' || profile.front() == L'\t')) {
					profile.erase(profile.begin());
				}
			}
		}
		// Profile dir lives next to moIni's gameDir, not necessarily modsRoot's parent.
		std::filesystem::path profilesDir;
		if (!moIni.empty()) {
			// moIni is <gameDir>/ModOrganizer.ini or B:\MO2/ModOrganizer.ini → gameDir = parent of mods' parent or MO2\*\.
			// Heuristic: <moIni parent>\profiles\<profile>\modlist.txt, else <modsRoot parent>\profiles\...
			auto tryDir = moIni.parent_path() / L"profiles" / profile;
			if (std::filesystem::is_directory(tryDir, ec)) {
				profilesDir = moIni.parent_path() / L"profiles";
			} else {
				profilesDir = gameDir / L"profiles";
			}
		} else {
			profilesDir = gameDir / L"profiles";
		}
		std::unordered_set<std::string> enabled;
		bool haveProfile = false;
		if (!profile.empty()) {
			const auto modlist = profilesDir / profile / L"modlist.txt";
			std::ifstream f(modlist);
			if (f.is_open()) {
				haveProfile = true;
				std::string line;
				while (std::getline(f, line)) {
					if (line.size() < 2 || line[0] != '+') {
						continue;
					}
					std::string name = line.substr(1);
					while (!name.empty() && (name.back() == '\r' || name.back() == '\n' ||
						name.back() == ' ' || name.back() == '\t')) {
						name.pop_back();
					}
					if (name.empty() || (name.size() > 10 &&
						name.compare(name.size() - 10, 10, "_separator") == 0)) {
						continue;
					}
					enabled.insert(LowerAscii(name));
				}
			} else {
				REX::WARN("SFShopExplorer live: MO2 modlist not found {}", modlist.string());
			}
		} else {
			REX::WARN("SFShopExplorer live: MO2 profile empty (ini {})", moIni.string());
		}
		std::unordered_set<std::string> seenBa2;
		for (const auto& p : g_extraBa2) {
			seenBa2.insert(LowerAscii(p.string()));
		}
		auto useDir = [&](const std::filesystem::path& a_modDir) {
			g_moDirs.push_back(a_modDir);
			std::error_code ec2;
			for (const auto& e : std::filesystem::directory_iterator(a_modDir, ec2)) {
				if (e.is_regular_file(ec2) &&
					LowerAscii(e.path().extension().string()) == ".ba2") {
					const std::string k = LowerAscii(e.path().string());
					if (seenBa2.insert(k).second) {
						g_moBa2.push_back(e.path());
					}
				}
			}
		};
		if (haveProfile) {
			for (const auto& modDir : present) {
				if (enabled.find(LowerAscii(modDir.filename().string())) != enabled.end()) {
					useDir(modDir);
				}
			}
			g_moProfileMode = true;
		} else {
			for (const auto& modDir : present) {
				useDir(modDir);
			}
		}
		REX::INFO("SFShopExplorer live: MO2 mods root {} profile '{}' enabled {}/{} dirs, {} ba2",
			modsRoot.string(), std::string(profile.begin(), profile.end()),
			g_moDirs.size(), present.size(), g_moBa2.size());
	}


	bool Enabled()
	{
		std::scoped_lock lock(g_mutex);
		return g_configLoaded && g_enabled;
	}

	std::filesystem::path CacheDir()
	{
		std::scoped_lock lock(g_mutex);
		return g_cacheDir;
	}

	// Single-NIF build. g_mutex must be held. Never fails the caller on a
	// missing/unparseable NIF — siblings are best-effort by design.
	static bool BuildSingleNif(const std::string& a_key, std::vector<LiveMesh>& a_out,
		std::size_t& a_totalVerts, bool a_quiet)
	{
		if (g_failed.find(a_key) != g_failed.end()) {
			return false;  // failed before this session; skip repeat work
		}
		std::vector<std::uint8_t> nifBytes;
		if (!ResolveAsset(a_key, nifBytes)) {
			if (!a_quiet) {
				REX::WARN("SFShopExplorer live: asset not found for {} ({} archives)", a_key, g_ba2List.size());
			}
			g_failed.insert(a_key);  // missing asset: don't rescan BA2s every frame
			return false;
		}
		std::vector<NifGeo> geos;
		std::string stage, detail;
		if (!ParseNifGeos(nifBytes, geos, stage, detail)) {
			if (!a_quiet) {
				if (detail.empty()) {
					REX::WARN("SFShopExplorer live: NIF parse failed for {} (stage {})", a_key, stage);
				} else {
					REX::WARN("SFShopExplorer live: NIF parse failed for {} (stage {}: {})", a_key, stage, detail);
				}
			}
			g_failed.insert(a_key);
			return false;
		}
		std::size_t meshMisses = 0, meshParseFails = 0;
		for (const auto& g : geos) {
			std::string mkey = "geometries/" + LowerAscii(g.factory) + ".mesh";
			std::vector<std::uint8_t> meshBytes;
			if (!ResolveAsset(mkey, meshBytes)) {
				++meshMisses;
				continue;
			}
			std::vector<float> pos, uv;
			std::vector<std::uint32_t> idx;
			if (!ParseMeshLod0(meshBytes, g.lodIndices, g.lodVerts, pos, uv, idx)) {
				++meshParseFails;
				if (meshParseFails <= 3) {
					std::string head;
					for (std::size_t i = 0; i < 16 && i < meshBytes.size(); ++i) {
						char tmp[4]{};
						snprintf(tmp, sizeof(tmp), "%02x", meshBytes[i]);
						head += tmp;
					}
					REX::WARN("SFShopExplorer live: mesh parse failed for {} (want {}v/{}i, got {} bytes, head {})",
						mkey, g.lodVerts, g.lodIndices, meshBytes.size(), head);
				}
				continue;
			}
			LiveMesh m;
			m.name = g.node.empty() ? g.matPath : g.node;
			m.matPath = g.matPath;
			m.indices = std::move(idx);
			m.uv = std::move(uv);
			m.pos.resize(pos.size());
			const float* M = g.world.m;
			const std::size_t nv = pos.size() / 3;
			if (nv == 0 || a_totalVerts + nv > 20'000'000 || a_out.size() >= 2048) {
				continue;
			}
			for (std::size_t v = 0; v < nv; ++v) {
				const float x = pos[v * 3], y = pos[v * 3 + 1], z = pos[v * 3 + 2];
				m.pos[v * 3 + 0] = M[0] * x + M[1] * y + M[2] * z + M[3];
				m.pos[v * 3 + 1] = M[4] * x + M[5] * y + M[6] * z + M[7];
				m.pos[v * 3 + 2] = M[8] * x + M[9] * y + M[10] * z + M[11];
			}
			a_totalVerts += nv;
			a_out.push_back(std::move(m));
		}
		if (a_out.empty()) {
			if (!a_quiet) {
				REX::WARN("SFShopExplorer live: no usable geometry for {} ({} geos, {} mesh misses, {} mesh parse fails)",
					a_key, geos.size(), meshMisses, meshParseFails);
			}
			g_failed.insert(a_key);
			return false;
		}
		return true;
	}

	// Outfit assembly ("render all"): an armor form points at ONE body-part
	// NIF (lowerbody), but the full garment is 2-3 sibling NIFs
	// (upperbody/sleeves) in the same folder, all authored in the same
	// bind-pose body frame. Merging them shows the whole outfit instead of
	// a floating part. Sibling misses are silent (g_failed still records
	// them so they are never rescanned).
	static void AssembleSiblings(const std::string& a_key, std::vector<LiveMesh>& a_out,
		std::size_t& a_totalVerts, std::vector<std::string>& a_sibKeys)
	{
		static constexpr const char* kParts[] = { "upperbody", "lowerbody", "sleeves" };
		const char* self = nullptr;
		for (const char* p : kParts) {
			if (a_key.find(p) != std::string::npos) {
				self = p;
				break;
			}
		}
		if (!self) {
			return;  // not a part-split garment (weapons, creatures, clutter...)
		}
		std::size_t added = 0;
		for (const char* p : kParts) {
			if (p == self) {
				continue;
			}
			std::string sib = a_key;
			const auto at = sib.find(self);
			sib.replace(at, std::strlen(self), p);
			a_sibKeys.push_back(sib);
			const std::size_t before = a_out.size();
			if (BuildSingleNif(sib, a_out, a_totalVerts, true)) {
				added += a_out.size() - before;
			}
		}
		if (added > 0) {
			REX::INFO("SFShopExplorer live: assembled {} ({} meshes, {} sibling parts)",
				a_key, a_out.size(), added);
		}
	}

	// Normalize a form model path to a game-relative BA2 key.
	// Empty when the path is not a NIF.
	static std::string NormalizeNifKey(const std::string& a_modelPath)
	{
		std::string key = LowerAscii(a_modelPath);
		if (key.compare(0, 7, "meshes/") != 0) {
			if (!key.empty() && key[0] == '/') {
				key.erase(0, 1);
			}
			key = "meshes/" + key;
		}
		if (key.size() < 8 || key.compare(key.size() - 4, 4, ".nif") != 0) {
			return {};
		}
		return key;
	}

	bool BuildModel(const std::string& a_modelPath, std::vector<LiveMesh>& a_out)
	{
		return BuildModels(std::vector<std::string>{ a_modelPath }, a_out);
	}

	bool BuildModels(const std::vector<std::string>& a_modelPaths, std::vector<LiveMesh>& a_out)
	{
		a_out.clear();
		std::scoped_lock lock(g_mutex);
		if (!g_configLoaded || !g_enabled) {
			return false;
		}
		// Dedup across paths AND their assembled siblings: two addons of one
		// armor (body + gloves) or two part-NIFs of one outfit must never
		// build the same NIF twice.
		std::unordered_set<std::string> used;
		std::size_t totalVerts = 0;
		bool any = false;
		for (const auto& path : a_modelPaths) {
			if (path.empty()) {
				continue;
			}
			const auto key = NormalizeNifKey(path);
			if (key.empty() || !used.insert(key).second) {
				continue;
			}
			std::vector<std::string> sibKeys;
			if (BuildSingleNif(key, a_out, totalVerts, false)) {
				any = true;
				AssembleSiblings(key, a_out, totalVerts, sibKeys);
				for (auto& s : sibKeys) {
					used.insert(s);
				}
				REX::INFO("SFShopExplorer live: built {} ({} meshes)", key, a_out.size());
			}
		}
		return any && !a_out.empty();
	}

	bool ReadAsset(const std::string& a_key, std::vector<std::uint8_t>& a_out)
	{
		a_out.clear();
		std::scoped_lock lock(g_mutex);
		if (!g_configLoaded || !g_enabled) {
			return false;
		}
		return ResolveAsset(LowerAscii(a_key), a_out);
	}

	std::uint32_t MapTexFormat(std::uint32_t a_dxgi)
	{
		// Sample sRGB sources as UNORM to match the preview look.
		switch (a_dxgi) {
		case 71:
		case 72: return 71;  // BC1
		case 74:
		case 75: return 74;  // BC2 (layout-compatible with BC3 reads here)
		case 77:
		case 78: return 74;  // BC3_UNORM/SRGB: same 16B blocks, alpha ignored
		case 80: return 80;  // BC4
		case 83: return 83;  // BC5
		case 97:
		case 98: return 97;  // BC7
		default: return 0;
		}
	}

	std::size_t TexRowPitch(std::uint32_t a_fmt, std::uint32_t a_w)
	{
		if (a_fmt == 28) {
			return static_cast<std::size_t>(a_w) * 4;
		}
		const std::size_t bb = (a_fmt == 71 || a_fmt == 80) ? 8 : 16;
		return ((static_cast<std::size_t>(a_w) + 3) / 4) * bb;
	}

	bool DdsToImage(const std::vector<std::uint8_t>& a_dds, TexImage& a_out)
	{
		if (a_dds.size() < 4 + 124) {
			return false;
		}
		std::uint32_t magic = 0, hsize = 0, height = 0, width = 0;
		std::uint32_t pfSize = 0, fourCC = 0, bitCount = 0;
		std::uint32_t rMask = 0, gMask = 0, bMask = 0, aMask = 0;
		std::memcpy(&magic, a_dds.data(), 4);
		std::memcpy(&hsize, a_dds.data() + 4, 4);
		std::memcpy(&height, a_dds.data() + 12, 4);
		std::memcpy(&width, a_dds.data() + 16, 4);
		std::memcpy(&pfSize, a_dds.data() + 72, 4);
		std::memcpy(&fourCC, a_dds.data() + 80, 4);
		std::memcpy(&bitCount, a_dds.data() + 88, 4);
		std::memcpy(&rMask, a_dds.data() + 92, 4);
		std::memcpy(&gMask, a_dds.data() + 96, 4);
		std::memcpy(&bMask, a_dds.data() + 100, 4);
		std::memcpy(&aMask, a_dds.data() + 104, 4);
		if (magic != 0x20534444u || hsize != 124 || pfSize != 32 || width == 0 || width > 4096 ||
			height == 0 || height > 4096) {
			return false;
		}
		std::uint32_t fmt = 0;
		std::size_t dataOff = 4 + 124;
		if (fourCC == 0x30315844u) {  // DX10
			std::uint32_t dxgi = 0;
			if (dataOff + 20 > a_dds.size()) {
				return false;
			}
			std::memcpy(&dxgi, a_dds.data() + dataOff, 4);
			dataOff += 20;
			fmt = MapTexFormat(dxgi);
		} else if (fourCC == 0x31545844u) {
			fmt = 71;
		} else if (fourCC == 0x33545844u || fourCC == 0x35545844u) {
			fmt = 74;
		} else if (fourCC == 0 && bitCount == 32 && rMask == 0x00FF0000u && gMask == 0x0000FF00u &&
			bMask == 0x000000FFu && aMask == 0xFF000000u) {
			fmt = 28;
		}
		if (fmt == 0) {
			return false;
		}
		const std::size_t pitch = TexRowPitch(fmt, width);
		const std::size_t rows = fmt == 28 ? height : (height + 3) / 4;
		if (dataOff + pitch * rows > a_dds.size()) {
			return false;
		}
		a_out.format = fmt;
		a_out.width = width;
		a_out.height = height;
		a_out.rowPitch = pitch;
		a_out.bytes.assign(a_dds.begin() + dataOff, a_dds.begin() + dataOff + pitch * rows);
		return true;
	}

	bool ReadTexture(const std::string& a_key, TexImage& a_out)
	{
		a_out = TexImage{};
		std::scoped_lock lock(g_mutex);
		if (!g_configLoaded || !g_enabled) {
			return false;
		}
		const std::string key = LowerAscii(a_key);
		// Loose files first (mods), then archives.
		std::filesystem::path rel;
		{
			std::string k = key;
			std::replace(k.begin(), k.end(), '/', static_cast<char>(std::filesystem::path::preferred_separator));
			rel = std::filesystem::path(k);
		}
		for (const auto& dir : g_extraDirs) {
			std::vector<std::uint8_t> dds;
			if (ReadLoose(dir / rel, dds) && DdsToImage(dds, a_out)) {
				return true;
			}
		}
		for (const auto& dir : g_moDirs) {
			std::vector<std::uint8_t> dds;
			if (ReadLoose(dir / rel, dds) && DdsToImage(dds, a_out)) {
				return true;
			}
		}
		if (!g_dataDir.empty()) {
			std::vector<std::uint8_t> dds;
			if (ReadLoose(g_dataDir / rel, dds) && DdsToImage(dds, a_out)) {
				return true;
			}
		}
		EnsureBa2List();
		for (const auto& ap : g_ba2List) {
			auto ar = OpenBa2(ap);
			if (!ar->valid) {
				continue;
			}
			if (!ar->isTex) {
				std::vector<std::uint8_t> dds;
				const auto it = ar->entries.find(key);
				if (it == ar->entries.end() || !Ba2Read(ar, it->second, dds)) {
					continue;
				}
				if (DdsToImage(dds, a_out)) {
					return true;
				}
				continue;
			}
			const auto it = ar->texFiles.find(key);
			if (it == ar->texFiles.end()) {
				continue;
			}
			const TexFile& tf = it->second;
			if (tf.width == 0 || tf.height == 0 || tf.chunks.empty()) {
				continue;
			}
		 const TexChunk* best = nullptr;
			for (const auto& c : tf.chunks) {
				if (c.startMip == 0) {
					best = &c;
					break;
				}
				if (!best || c.size > best->size) {
					best = &c;
				}
			}
			if (!best || best->size == 0) {
				continue;
			}
			const std::uint32_t fmt = MapTexFormat(tf.dxgi);
			if (fmt == 0) {
				REX::INFO("SFShopExplorer live: texture format {} unsupported ({})", tf.dxgi, key);
				continue;
			}
			const std::size_t pitch = TexRowPitch(fmt, tf.width);
			const std::size_t rows = fmt == 28 ? tf.height : (tf.height + 3) / 4;
			if (best->size < pitch * rows) {
				continue;
			}
			ar->file.clear();
			ar->file.seekg(static_cast<std::streamoff>(best->off));
			std::vector<std::uint8_t> packed(best->packed ? best->packed : best->size);
			ar->file.read(reinterpret_cast<char*>(packed.data()), packed.size());
			if (!ar->file) {
				continue;
			}
			std::vector<std::uint8_t> raw;
			if (best->packed != 0 && best->packed != best->size) {
				raw.resize(best->size);
				bool ok = false;
				if (ar->method == 3) {
					ok = LZ4_decompress_safe(reinterpret_cast<const char*>(packed.data()),
							reinterpret_cast<char*>(raw.data()), static_cast<int>(packed.size()),
							static_cast<int>(best->size)) == static_cast<int>(best->size);
				} else {
					uLongf dl = static_cast<uLongf>(best->size);
					ok = uncompress(raw.data(), &dl, packed.data(), static_cast<uLong>(packed.size())) == Z_OK &&
						dl == best->size;
				}
				if (!ok) {
					continue;
				}
			} else {
				raw.swap(packed);
			}
			a_out.format = fmt;
			a_out.width = tf.width;
			a_out.height = tf.height;
			a_out.rowPitch = pitch;
			a_out.bytes.assign(raw.begin(), raw.begin() + pitch * rows);
			return true;
		}
		return false;
	}

	struct ColorEntry
	{
		std::string key;   // full lowercased "textures/..._color.dds"
		std::string base;  // file base without suffix
		std::string family;// directory portion
	};

	std::vector<ColorEntry> g_colors;
	bool g_colorsBuilt{ false };
	std::unordered_map<std::string, std::string> g_diffuseMemo;
	std::unordered_map<std::string, std::string> g_albedoMemo;  // matPath lower -> textures/... ("" = none)

	void NormalizeBase(std::string& a_s)
	{
		// strip trailing :digits (geometry suffixes like ":0")
		const auto colon = a_s.find_last_of(':');
		if (colon != std::string::npos) {
			bool digits = colon + 1 < a_s.size();
			for (std::size_t i = colon + 1; digits && i < a_s.size(); ++i) {
				digits = a_s[i] >= '0' && a_s[i] <= '9';
			}
			if (digits) {
				a_s.erase(colon);
			}
		}
		static const char* kSuffix[] = { "_low", "_mat", "_01", "_0", "_f" };
		for (const char* suf : kSuffix) {
			const std::size_t n = std::strlen(suf);
			if (a_s.size() > n + 3 && a_s.compare(a_s.size() - n, n, suf) == 0) {
				a_s.erase(a_s.size() - n);
			}
		}
	}

	void BaseVariants(const std::string& a_in, std::vector<std::string>& a_out,
		std::vector<std::string>* a_prefixOut = nullptr)
	{
		std::string s = LowerAscii(a_in);
		const auto slash = s.find_last_of('/');
		if (slash != std::string::npos) {
			s.erase(0, slash + 1);
		}
		const auto dot = s.find_last_of('.');
		if (dot != std::string::npos) {
			s.erase(dot);
		}
		NormalizeBase(s);
		if (s.size() >= 4) {
			a_out.push_back(s);
		}
		static const char* kPre[] = { "ar99_", "rattler_", "r_heater_", "r_", "outfit_", "clothes_" };
		for (const char* pre : kPre) {
			const std::size_t n = std::strlen(pre);
			if (s.size() > n + 3 && s.compare(0, n, pre) == 0) {
				std::string t = s.substr(n);
				NormalizeBase(t);
				if (t.size() >= 4) {
					// Prefix-stripped fragments ("receiver" from
					// "rattler_receiver") are WEAK: they must never grant a
					// cross-family exact hit (that is how Rattler_Receiver
					// wore bigbang/receiver_color and Decals wore an
					// industrial decal). Tracked separately below.
					if (a_prefixOut) {
						a_prefixOut->push_back(t);
					} else {
						a_out.push_back(t);
					}
				}
			}
		}
	}

	// First 2+ digit run in a lowercased base ("plant01" -> "01",
	// "sm01_plants" -> "01", "" when none). Variant-aware tiebreak.
	std::string VariantNum(const std::string& a_s)
	{
		std::size_t i = 0;
		while (i < a_s.size()) {
			if (a_s[i] >= '0' && a_s[i] <= '9') {
				std::size_t j = i;
				while (j < a_s.size() && a_s[j] >= '0' && a_s[j] <= '9') {
					++j;
				}
				if (j - i >= 2) {
					return a_s.substr(i, j - i);
				}
				i = j;
			} else {
				++i;
			}
		}
		return {};
	}

	// Underscore tokens, stemmed (trailing 's' stripped: plants -> plant;
	// trailing digits split: soil01 -> soil, sm01 -> sm). Used for
	// soil/plant/page/book style distinctive-token bonus.
	std::vector<std::string> StemTokens(const std::string& a_s)
	{
		std::vector<std::string> out;
		std::size_t i = 0;
		while (i <= a_s.size()) {
			const auto j = a_s.find('_', i);
			std::string t = a_s.substr(i, j == std::string::npos ? j : j - i);
			// Split trailing digits ("soil01" -> "soil"): variant numbers
			// otherwise pollute the token ("soil01" never equals "soil",
			// so Soil01 fell through to the plant map).
			while (t.size() > 3 && t.back() >= '0' && t.back() <= '9') {
				t.pop_back();
			}
			if (t.size() > 3 && t.back() == 's') {
				t.pop_back();
			}
			if (t.size() >= 3) {
				out.push_back(t);
			}
			if (j == std::string::npos) {
				break;
			}
			i = j + 1;
		}
		return out;
	}

	void EnsureColorIndex()
	{
		if (g_colorsBuilt) {
			return;
		}
		g_colorsBuilt = true;
		EnsureBa2List();
		const auto indexColor = [](const std::string& name) {
			if (name.size() < 11 || name.compare(name.size() - 10, 10, "_color.dds") != 0) {
				return;
			}
			ColorEntry e;
			e.key = name;
			const auto slash = name.find_last_of('/');
			e.family = slash == std::string::npos ? std::string{} : name.substr(0, slash);
			e.base = slash == std::string::npos ? name : name.substr(slash + 1);
			e.base.erase(e.base.size() - 10);
			g_colors.push_back(std::move(e));
		};
		for (const auto& ap : g_ba2List) {
			auto ar = OpenBa2(ap);
			if (!ar->valid) {
				continue;
			}
			for (const auto& [name, ent] : ar->entries) {
				(void)ent;
				indexColor(name);
			}
			// Texture archives are DX10 type: their files live in texFiles,
			// not entries. That is where every _color.dds actually resides —
			// without this the index was empty and all previews stayed grey.
			for (const auto& [name, tf] : ar->texFiles) {
				(void)tf;
				indexColor(name);
			}
		}
		REX::INFO("SFShopExplorer live: color index ({} _color maps)", g_colors.size());
	}

	// Bethesda typo dirs, canonicalized on both sides of every family
	// comparison (materials say ComatKnife, textures say combatknife).
	void CanonFam(std::string& a_fam)
	{
		for (std::size_t at = 0; (at = a_fam.find("comatknife", at)) != std::string::npos; ++at) {
			a_fam.replace(at, 10, "combatknife");
		}
	}

	// Functional kind synonyms: an ammo-counter SCREEN wears the DISPLAY
	// map, a HANDLE wears the GRIP map. Compared (never stored), so exact
	// strings, tiebreaks and logs are unaffected — only token equality
	// gains these edges. Deliberately NOT including glass/plastic/decal
	// (fail-closed kinds decide those separately).
	std::string KindNorm(const std::string& a_tok)
	{
		static constexpr const char* kGroups[][7] = {
			{ "screen", "display", "counter", "monitor", "readout", "indicator", nullptr },
			{ "scope", "sight", "optic", "reticle", "reticule", nullptr },
			{ "grip", "handle", nullptr, nullptr, nullptr, nullptr },
			{ "mag", "magazine", "clip", "drum", nullptr, nullptr },
			{ "barrel", "muzzle", "bore", nullptr, nullptr, nullptr },
			{ "light", "lamp", "glow", "bulb", nullptr, nullptr },
			{ "button", "switch", "knob", nullptr, nullptr, nullptr },
			{ "stock", "buttstock", nullptr, nullptr, nullptr, nullptr },
		};
		for (const auto& g : kGroups) {
			for (int i = 1; g[i]; ++i) {
				if (a_tok == g[i]) {
					return g[0];
				}
			}
			if (a_tok == g[0]) {
				return g[0];
			}
			// Compound tokens carry the kind as a suffix (powerindicator
			// -> screen). The kind words are long enough that accidental
			// suffix hits are rare, and family scope gates everything.
			for (int i = 0; g[i]; ++i) {
				const std::string w = g[i];
				if (w.size() >= 4 && a_tok.size() > w.size() &&
					a_tok.compare(a_tok.size() - w.size(), w.size(), w) == 0) {
					return g[0];
				}
			}
		}
		return a_tok;
	}

	// Exact albedo via the .mat JSON (layered-material "Albedo" block).
	// Fail-closed at every step: missing/unparseable .mat, missing Albedo
	// or File, non-texture path — all memoize as misses so the caller
	// falls through to fuzzy FindDiffuse at zero repeat cost.
	bool FindMatAlbedo(const std::string& a_matPath, std::string& a_outKey)
	{
		a_outKey.clear();
		std::scoped_lock lock(g_mutex);
		if (!g_configLoaded || !g_enabled) {
			return false;
		}
		const std::string memoKey = LowerAscii(a_matPath);
		{
			const auto it = g_albedoMemo.find(memoKey);
			if (it != g_albedoMemo.end()) {
				if (!it->second.empty()) {
					a_outKey = it->second;
					return true;
				}
				return false;
			}
		}
		std::string best;
		do {
			std::string key = memoKey;
			while (!key.empty() && (key.front() == '/' || key.front() == '\\')) {
				key.erase(key.begin());
			}
			if (key.size() < 5 || key.compare(key.size() - 4, 4, ".mat") != 0) {
				break;
			}
			std::vector<std::uint8_t> bytes;
			// g_mutex is held (same discipline as ReadAsset callers).
			if (!ResolveAsset(key, bytes) || bytes.empty() || bytes.size() > (1ULL << 20)) {
				break;
			}
			const std::string text(bytes.begin(), bytes.end());
			if (text.empty() || text.front() != '{') {
				break;  // JSON .mats only; binary blobs are not parsed
			}
			std::string low = text;
			for (auto& ch : low) {
				ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			}
			const auto alb = low.find("\"albedo\"");
			if (alb == std::string::npos) {
				break;
			}
			const auto brace = low.find('{', alb);
			if (brace == std::string::npos || brace - alb > 64) {
				break;
			}
			std::size_t end = std::string::npos;
			{
				int depth = 0;
				for (std::size_t i = brace; i < low.size() && i < brace + 8192; ++i) {
					if (low[i] == '{') {
						++depth;
					} else if (low[i] == '}' && --depth == 0) {
						end = i;
						break;
					}
				}
			}
			if (end == std::string::npos) {
				break;
			}
			// First "file" : "path" inside the Albedo object.
			std::string path;
			{
				auto f = low.find("\"file\"", brace);
				while (f != std::string::npos && f < end) {
					std::size_t p = f + 6;
					while (p < end && (low[p] == ' ' || low[p] == '\t' || low[p] == '\r' || low[p] == '\n')) {
						++p;
					}
					if (p < end && low[p] == ':') {
						++p;
						while (p < end && (low[p] == ' ' || low[p] == '\t' || low[p] == '\r' || low[p] == '\n')) {
							++p;
						}
						if (p < end && low[p] == '"') {
							++p;
							std::string raw;
							while (p < end && low[p] != '"') {
								if (low[p] == '\\' && p + 1 < end) {
									raw.push_back(low[p + 1]);  // JSON escape (\\ -> \)
									p += 2;
								} else {
									raw.push_back(low[p]);
									++p;
								}
							}
							if (p < end) {
								path = raw;
							}
							break;
						}
					}
					f = low.find("\"file\"", f + 6);
				}
			}
			if (path.empty()) {
				break;
			}
			// Normalize: backslashes, Data\ prefix, doubles.
			std::string norm;
			for (const char c : path) {
				norm.push_back(c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
			}
			if (norm.compare(0, 5, "data/") == 0) {
				norm.erase(0, 5);
			}
			std::string flat;
			for (const char c : norm) {
				if (c != '/' || flat.empty() || flat.back() != '/') {
					flat.push_back(c);
				}
			}
			while (!flat.empty() && flat.front() == '/') {
				flat.erase(flat.begin());
			}
			if (flat.size() < 15 || flat.compare(flat.size() - 4, 4, ".dds") != 0 ||
				flat.find("textures/") == std::string::npos) {
				break;
			}
			best = flat;
		} while (false);
		g_albedoMemo.emplace(memoKey, best);
		if (best.empty()) {
			return false;
		}
		a_outKey = best;
		return true;
	}

	bool FindDiffuse(const std::string& a_matPath, const std::string& a_nodeName, std::string& a_outKey,
		int* a_outScore)
	{
		a_outKey.clear();
		if (a_outScore) {
			*a_outScore = 0;
		}
		std::scoped_lock lock(g_mutex);
		if (!g_configLoaded || !g_enabled) {
			return false;
		}
		// Memoized per material+node pair (session cache).
		const std::string memoKey = LowerAscii(a_matPath) + '\x01' + LowerAscii(a_nodeName);
		{
			const auto it = g_diffuseMemo.find(memoKey);
			if (it != g_diffuseMemo.end()) {
				if (!it->second.empty()) {
					a_outKey = it->second;
					if (a_outScore) {
						*a_outScore = -1;  // cached verdict, score logged at first hit
					}
					return true;
				}
				return false;
			}
		}
		// Family dir from the material path (materials/... -> textures/...).
		// Leading slashes (some NIFs store absolute-ish "\materials\...")
		// are stripped: without this the family is empty and nothing in
		// any archive can ever match (dogfight decals went flat).
		std::string family;
		{
			std::string mp = LowerAscii(a_matPath);
			while (!mp.empty() && (mp.front() == '/' || mp.front() == '\\')) {
				mp.erase(mp.begin());
			}
			if (mp.compare(0, 10, "materials/") == 0) {
				const auto slash = mp.find_last_of('/');
				family = "textures/" + (slash == std::string::npos ? mp.substr(10) : mp.substr(10, slash - 10));
			}
			CanonFam(family);
		}
		std::vector<std::string> cands;
		std::vector<std::string> prefixCands;
		std::vector<std::string> dirCands;
		BaseVariants(a_matPath, cands, &prefixCands);
		{
			std::vector<std::string> nodeCands, nodePrefix;
			BaseVariants(a_nodeName, nodeCands, &nodePrefix);
			cands.insert(cands.end(), nodeCands.begin(), nodeCands.end());
			prefixCands.insert(prefixCands.end(), nodePrefix.begin(), nodePrefix.end());
		}
		// Family-dirname fallback: a material like
		// Materials/SetDressing/AmmoBox01/AmmoBoxLarge_DarkGreen01.mat shares
		// no words with its family's generic diffuse (ammobox01_color), so
		// add the dirname itself as an exact-grade candidate. This is what
		// resolves DarkGreen01 to the family base instead of junk.
		if (!family.empty()) {
			const auto slash = family.find_last_of('/');
			std::string dir = slash == std::string::npos ? family : family.substr(slash + 1);
			if (dir.size() >= 4) {
				dirCands.push_back(dir);
			}
		}
		EnsureColorIndex();
		int bestScore = 0;
		std::size_t bestBaseLen = 0;
		std::string best;
		int bestLoop = 0;  // 1 full/node, 2 prefix-derived, 3 dir-dirname
		const auto familyScoped = [&](const std::string& a_efam) {
			std::string efam = a_efam;
			CanonFam(efam);
			return !family.empty() && (efam == family ||
				efam.compare(0, family.size() + 1, family + "/") == 0);
		};
		// Sibling scope: textures split per-item into neighbour dirs
		// (menuitem_chimera next to menuitem, exoticplayingcards next to
		// exoticplayingcard). Accept a family that shares the parent dir
		// and whose dirname extends the material family's own dirname,
		// compared underscore-insensitively (modelscrimsonfleet vs
		// models_crimsonfleet). Containment/token rules still gate every
		// hit, so unrelated neighbours (bigbang vs rattler) stay excluded.
		const auto familyScopedExt = [&](const std::string& a_efam) {
			if (familyScoped(a_efam)) {
				return true;
			}
			if (family.empty()) {
				return false;
			}
			std::string efam = a_efam;
			CanonFam(efam);
			const auto noUS = [](std::string s) {
				s.erase(std::remove(s.begin(), s.end(), '_'), s.end());
				return s;
			};
			const auto slash = family.find_last_of('/');
			const std::string parent = slash == std::string::npos ? std::string{} : family.substr(0, slash);
			const std::string tok = noUS(slash == std::string::npos ? family : family.substr(slash + 1));
			const auto eslash = efam.find_last_of('/');
			const std::string eparent = eslash == std::string::npos ? std::string{} : efam.substr(0, eslash);
			const std::string edir = noUS(eslash == std::string::npos ? efam : efam.substr(eslash + 1));
			// >= : dirnames identical after underscore stripping
			// (modelscrimsonfleet vs models_crimsonfleet) count.
			return !parent.empty() && eparent == parent && edir.size() >= tok.size() &&
				edir.compare(0, tok.size(), tok) == 0;
		};
		// Distinctive last-token of the material (soil/plant/page/book/...):
		// family-scoped token hits beat generic dir-fallback containment
		// (that is how Soil01 stopped wearing the plant map).
		std::string matToken;
		{
			std::string mp = LowerAscii(a_matPath);
			const auto slash = mp.find_last_of('/');
			std::string base = slash == std::string::npos ? mp : mp.substr(slash + 1);
			const auto dot = base.find_last_of('.');
			if (dot != std::string::npos) {
				base.erase(dot);
			}
			NormalizeBase(base);
			const auto u = base.find_last_of('_');
			matToken = u == std::string::npos ? base : base.substr(u + 1);
			while (matToken.size() > 3 && matToken.back() >= '0' && matToken.back() <= '9') {
				matToken.pop_back();
			}
			if (matToken.size() > 3 && matToken.back() == 's') {
				matToken.pop_back();
			}
			matToken = KindNorm(matToken);
			// Raw variant number before NormalizeBase stripped it
			// (Plant01 -> "01"): feeds the variant tiebreak below.
		}
		std::string matVariant = VariantNum(LowerAscii(a_matPath));
		if (matVariant.empty()) {
			matVariant = VariantNum(LowerAscii(a_nodeName));
		}
		// Cross-family exact hits are allowed ONLY for shared generics
		// (glass). Everything else exact must be family-scoped: the old
		// rule let "receiver"/"decals" (prefix fragments) match bigbang
		// and industrial textures across families.
		auto crossFamilyExactOk = [](const std::string& a_base) {
			return a_base.find("glass") != std::string::npos;
		};
		auto scoreOne = [&](const std::string& c, bool a_prefixDerived, bool a_strictScope, int a_loop) {
			auto inScope = [&](const std::string& a_efam) {
				return a_strictScope ? familyScoped(a_efam) : familyScopedExt(a_efam);
			};
			for (const auto& e : g_colors) {
				int score = 0;
				if (e.base == c) {
					if (!inScope(e.family) && (a_prefixDerived || !crossFamilyExactOk(e.base))) {
						continue;
					}
					score = 1000 + static_cast<int>(c.size());
				} else if (e.base.find(c) != std::string::npos || c.find(e.base) != std::string::npos) {
					const std::size_t m = std::min(c.size(), e.base.size());
					if (m < 3) {
						continue;
					}
					// Containment must be family-scoped. Cross-family
					// containment is how "Orange" matched a palm-tree
					// texture and "AmmoCounter" a gunmetal: when nothing in
					// the family matches, failing closed (flat shaded) beats
					// wearing a random part's skin. Exact-base matches may
					// still cross families (shared generics like glass).
					if (!inScope(e.family)) {
						continue;
					}
					score = static_cast<int>(m);
				} else {
					// Token overlap (family-scoped): "soil" hits
					// "succulents_soil" even though neither string contains
					// the other ("_sm" breaks the substring). Without this,
					// Soil01 fell through to the dir-fallback plant map.
					// Token-prefix overlap at half weight: "chimeracap" ~
					// "chimera", "exoticplayingcardbox01" ~
					// "exoticplayingcard".
					if (!inScope(e.family)) {
						continue;
					}
					int shared = 0;
					for (const auto& t : StemTokens(c)) {
						for (const auto& u : StemTokens(e.base)) {
							if (KindNorm(t) == KindNorm(u)) {
								shared = std::max(shared, static_cast<int>(t.size()));
							} else {
								std::size_t k = 0;
								while (k < t.size() && k < u.size() && t[k] == u[k]) {
									++k;
								}
								if (k >= 6) {
									shared = std::max(shared, static_cast<int>(k / 2));
								}
							}
						}
					}
					if (shared < 3) {
						continue;
					}
					score = shared;
				}
				// Family scope (including subdirs like .../variant/)
				// dominates: a cross-family containment is almost always
				// a wrong part that happens to share a word.
				if (inScope(e.family)) {
					score += 1000;
					// Distinctive-token bonus: soil hits soil, plant hits
					// plant, page hits page — not the family's shortest map.
					if (!matToken.empty()) {
						for (const auto& t : StemTokens(e.base)) {
							if (KindNorm(t) == matToken) {
								score += 600;
								break;
							}
						}
					}
					// Variant-number bonus: Plant01 prefers sm01 over sm02.
					if (!matVariant.empty() && !VariantNum(e.base).empty() &&
						VariantNum(e.base) == matVariant) {
						score += 400;
					}
				}
				// Ties pick the SHORTER base (fewer variant tokens): a plain
				// material must resolve to the base texture, not the teal /
				// black / olive variant that merely contains the same words.
				// (The old tiebreak compared against the full key length —
				// meaningless units — so archive order picked the variant.)
				if (score > bestScore || (score == bestScore && (best.empty() || e.base.size() < bestBaseLen))) {
					bestScore = score;
					bestBaseLen = e.base.size();
					best = e.key;
					bestLoop = a_loop;
				}
			}
		};
		for (const auto& c : cands) {
			scoreOne(c, false, false, 1);
		}
		for (const auto& c : prefixCands) {
			scoreOne(c, true, false, 2);
		}
		// Dir-dirname candidates run ONLY when nothing specific matched:
		// they are the generic last resort (DarkGreen01 -> ammobox01), and
		// must never outbid a real token hit (Eon_Decals wore the family
		// generic instead of the decals map). STRICTLY scoped (own family +
		// subdirs): a sibling dirname would otherwise hijack the generic
		// (books -> booksailing instead of the genbooka01 fallback).
		if (best.empty()) {
			for (const auto& c : dirCands) {
				scoreOne(c, false, true, 3);
			}
		}
		// Same-folder generic fallback (BookLarge02): GenBookB01 has no
		// B01 _color map — the family's only color is genbooka01. When
		// nothing matched, take the shortest family-scoped map that shares
		// a 3-letter prefix with any candidate instead of rendering grey.
		if (best.empty() && !family.empty()) {
			for (const auto& e : g_colors) {
				if (!familyScopedExt(e.family)) {
					continue;
				}
				bool shares = false;
				for (const auto& c : cands) {
					if (c.size() >= 3 && e.base.size() >= 3 &&
						c.compare(0, 3, e.base, 0, 3) == 0) {
						shares = true;
						break;
					}
				}
				if (!shares) {
					continue;
				}
				if (best.empty() || e.base.size() < bestBaseLen) {
					bestBaseLen = e.base.size();
					best = e.key;
					bestLoop = 4;  // generic fallback, not a real match
				}
			}
		if (!best.empty()) {
			REX::INFO("SFShopExplorer live: family fallback '{}' -> {}", a_matPath, best);
		}
	}
	// Family-singleton last resort (FragMine.mat -> default_color):
	// when the material's own family ships exactly one _color map and
	// nothing matched, that map IS the diffuse — token/containment
	// rules can never fire inside a one-entry family. STRICTLY scoped
	// (own family + subdirs), logged loudly for auditing. Flat-kind
	// materials (plastic/glass/decals/...) still fail closed below
	// unless the singleton carries a strong token/art hit.
	if (best.empty() && !family.empty()) {
		std::string single;
		int count = 0;
		for (const auto& e : g_colors) {
			if (!familyScoped(e.family)) {
				continue;
			}
			++count;
			single = e.key;
			if (count > 1) {
				break;
			}
		}
		if (count == 1) {
			best = single;
			bestBaseLen = single.size();
			bestLoop = 5;  // singleton fallback, not a scored match
			REX::INFO("SFShopExplorer live: family singleton '{}' -> {}", a_matPath, best);
		}
	}
	g_diffuseMemo.emplace(memoKey, best);
		if (best.empty()) {
			return false;
		}
		// Fail-closed for untextured kinds (plastic pots, transparent glass,
		// blank/letter/line overlay decals, labels): wearing a random family
		// map is worse than flat shading. Art-kind sheets (decal/overlay/
		// blank/letter/stripe/lines/label) belong on art-kind textures;
		// material kinds (plastic/glass) need their own token in the pick.
		{
			const std::string ml = LowerAscii(a_matPath);
			static const char* kFlat[] = { "plastic", "glass", "decal", "overlay", "blank", "letter", "stripe", "lines", "label" };
			static const char* kArt[] = { "decal", "overlay", "blank", "letter", "stripe", "lines", "label" };
			bool flatKind = false;
			for (const char* k : kFlat) {
				if (ml.find(k) != std::string::npos) {
					flatKind = true;
					break;
				}
			}
			if (flatKind) {
				std::string bb = LowerAscii(best);
				const auto slash = bb.find_last_of('/');
				std::string bfam = slash == std::string::npos ? std::string{} : bb.substr(0, slash);
				bb = slash == std::string::npos ? bb : bb.substr(slash + 1);
				if (bb.size() > 10 && bb.compare(bb.size() - 10, 10, "_color.dds") == 0) {
					bb.erase(bb.size() - 10);
				}
				const bool strict = !family.empty() && (bfam == family ||
					bfam.compare(0, family.size() + 1, family + "/") == 0);
				const auto hasArt = [](const std::string& s) {
					for (const char* k : kArt) {
						if (s.find(k) != std::string::npos) {
							return true;
						}
					}
					return false;
				};
				const bool matArt = hasArt(ml);
				const bool bestArt = hasArt(bb);
				const bool hasTok = !matToken.empty() && matToken.size() >= 3 &&
					bb.find(matToken) != std::string::npos;
				int tokMax = 0;
				for (const auto& c : cands) {
					for (const auto& t : StemTokens(c)) {
						for (const auto& u : StemTokens(bb)) {
							if (KindNorm(t) == KindNorm(u)) {
								tokMax = std::max(tokMax, static_cast<int>(t.size()));
							}
						}
					}
				}
				for (const auto& c : prefixCands) {
					for (const auto& t : StemTokens(c)) {
						for (const auto& u : StemTokens(bb)) {
							if (KindNorm(t) == KindNorm(u)) {
								tokMax = std::max(tokMax, static_cast<int>(t.size()));
							}
						}
					}
				}
				// Weak generic/dir wins always fail. Strict wins need the
				// material's own token or art on both sides; sibling wins
				// need a long shared token or art on both sides.
				bool keep = false;
				if (strict) {
					keep = hasTok || bestArt;
				} else {
					keep = (tokMax >= 8) || (matArt && bestArt);
				}
				if (bestLoop >= 3 || !keep) {
					REX::INFO("SFShopExplorer live: '{}' stays flat (no strong token hit)", a_matPath);
					g_diffuseMemo[memoKey].clear();
					return false;
				}
			}
		}
		a_outKey = best;
		if (a_outScore) {
			*a_outScore = bestLoop == 4 ? -2 : (bestLoop == 5 ? -3 : bestScore);
		}
		return true;
	}

	std::unordered_map<std::string, std::string> g_nifMemo;  // edidLower -> nif key ("" = none)

	// Part-ish NIF basenames never qualify as whole guns (mirrors the
	// OfflineModel substring guard; kept local to avoid a TU dependency).
	bool NifBaseIsPart(const std::string& a_base)
	{
		static constexpr const char* kParts[] = { "scope", "sight", "barrel", "_mag", "magazine",
			"grip", "muzzle", "suppressor", "supressor", "compensator", "stock", "trigger",
			"casing", "bullet", "charger", "knife", "sword", "blade", "handle",
			"_laser", "laser_", "_optic", "optic_", "_lens", "lens_", "_reflex", "reflex_",
			"_glow", "glow_", "_dot", "dot_", "flashlight", "base" };
		for (const char* p : kParts) {
			if (a_base.find(p) != std::string::npos) {
				return true;
			}
		}
		return false;
	}

	bool DiscoverNif(const std::string& a_edid, std::string& a_outNifKey)
	{
		a_outNifKey.clear();
		std::scoped_lock lock(g_mutex);
		if (!g_configLoaded || !g_enabled) {
			return false;
		}
		const std::string edidLower = LowerAscii(a_edid);
		{
			const auto it = g_nifMemo.find(edidLower);
			if (it != g_nifMemo.end()) {
				if (!it->second.empty()) {
					a_outNifKey = it->second;
					return true;
				}
				return false;
			}
		}
		// Candidate tokens: full EDID, then long-to-short alnum tokens.
		std::vector<std::string> tokens;
		if (edidLower.size() >= 3) {
			tokens.push_back(edidLower);
		}
		{
			std::size_t i = 0;
			std::vector<std::string> parts;
			while (i <= edidLower.size()) {
				while (i < edidLower.size() &&
					!((edidLower[i] >= 'a' && edidLower[i] <= 'z') || (edidLower[i] >= '0' && edidLower[i] <= '9'))) {
					++i;
				}
				std::size_t j = i;
				while (j < edidLower.size() &&
					((edidLower[j] >= 'a' && edidLower[j] <= 'z') || (edidLower[j] >= '0' && edidLower[j] <= '9'))) {
					++j;
				}
				if (j > i && j - i >= 3) {
					parts.push_back(edidLower.substr(i, j - i));
				}
				i = j + 1;
			}
			std::sort(parts.begin(), parts.end(), [](const std::string& a, const std::string& b) {
				return a.size() > b.size();
			});
			for (auto& t : parts) {
				if (t != edidLower) {
					tokens.push_back(t);
				}
			}
		}
		std::string best;
		if (!tokens.empty()) {
			EnsureBa2List();
			for (const auto& tok : tokens) {
				std::string exact;
				std::uint32_t exactSize = 0;
				std::string sub;
				for (const auto& ap : g_ba2List) {
					auto ar = OpenBa2(ap);
					if (!ar->valid || ar->isTex) {
						continue;
					}
					for (const auto& [name, ent] : ar->entries) {
						if (name.size() < tok.size() + 8 || name.compare(0, 7, "meshes/") != 0 ||
							name.compare(name.size() - 4, 4, ".nif") != 0) {
							continue;
						}
						if (name.find("/mods/") != std::string::npos) {
							continue;  // attachment-part assemblies
						}
						const auto slash = name.find_last_of('/');
						const std::string base = name.substr(slash + 1, name.size() - slash - 5);
						if (base == tok) {
							if (exact.empty() || ent.size > exactSize) {
								exact = name;
								exactSize = ent.size;
							}
						} else if (exact.empty() && base.find(tok) != std::string::npos && !NifBaseIsPart(base) &&
							(sub.empty() || base.size() < sub.size())) {
							// Shortest non-part basename wins; tokens run
							// longest-first so the first hit sticks.
							sub = name;
						}
					}
					if (!exact.empty()) {
						break;
					}
				}
				if (!exact.empty()) {
					best = exact;
					break;
				}
				if (!sub.empty()) {
					best = sub;
					break;
				}
			}
		}
		g_nifMemo.emplace(edidLower, best);
		if (best.empty()) {
			return false;
		}
		REX::INFO("SFShopExplorer live: discovered NIF '{}' <- '{}'", best, a_edid);
		a_outNifKey = best;
		return true;
	}

	bool WriteSfModel(const std::filesystem::path& a_path, const std::vector<LiveMesh>& a_meshes)
	{
		if (a_meshes.empty()) {
			return false;
		}
		std::error_code ec;
		std::filesystem::create_directories(a_path.parent_path(), ec);
		std::ofstream o(a_path, std::ios::binary | std::ios::trunc);
		if (!o) {
			return false;
		}
		o.write("SFM1", 4);
		const std::uint32_t n = static_cast<std::uint32_t>(a_meshes.size());
		o.write(reinterpret_cast<const char*>(&n), 4);
		for (const auto& m : a_meshes) {
			char nm[64]{};
			std::strncpy(nm, m.name.c_str(), 63);
			o.write(nm, 64);
			const std::uint32_t ml = static_cast<std::uint32_t>(m.matPath.size());
			o.write(reinterpret_cast<const char*>(&ml), 4);
			o.write(m.matPath.data(), ml);
			const std::uint32_t nv = static_cast<std::uint32_t>(m.pos.size() / 3);
			const std::uint32_t ni = static_cast<std::uint32_t>(m.indices.size());
			o.write(reinterpret_cast<const char*>(&nv), 4);
			o.write(reinterpret_cast<const char*>(&ni), 4);
			const bool hasUv = m.uv.size() == static_cast<std::size_t>(nv) * 2;
			for (std::uint32_t v = 0; v < nv; ++v) {
				const float u = hasUv ? m.uv[v * 2] : 0.0f;
				const float t = hasUv ? m.uv[v * 2 + 1] : 0.0f;
				const float q[5]{ m.pos[v * 3], m.pos[v * 3 + 1], m.pos[v * 3 + 2], u, t };
				o.write(reinterpret_cast<const char*>(q), sizeof(q));
			}
			o.write(reinterpret_cast<const char*>(m.indices.data()), static_cast<std::streamsize>(ni) * 4);
		}
		o.flush();
		return static_cast<bool>(o);
	}
}
