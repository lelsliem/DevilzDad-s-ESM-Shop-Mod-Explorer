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
#include "ShopExplorer.h"  // kVersion - part of the persisted CDB index stamp

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <string_view>
#include <unordered_map>
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
		// Asset roots are the Data directory as this process sees it (the mod
		// manager's VFS has already merged and priority-resolved it), plus any
		// INI extradirs/extraba2. No manager configuration is consulted.

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
			std::unordered_map<std::string, TexFile> texFiles;			bool isTex{ false };
			std::uint32_t method{ 0 };  // 0 = zlib, 3 = LZ4 (v3 header; v2 is always zlib/raw)
			// End of the bulk data region (the name table starts here).
			// No entry can legitimately read past it, so this bounds the
			// compressed read length in Ba2Read.
			std::uint64_t dataEnd{ 0 };
			bool valid{ false };
		};

		std::unordered_map<std::string, std::shared_ptr<Ba2Archive>> g_ba2;
		std::unordered_set<std::string> g_failed;  // session negative cache
		std::vector<std::filesystem::path> g_ba2List;
		bool g_ba2ListBuilt{ false };
		bool g_ba2ListComplete{ false };
		// Build-116 warm-up cursor: written only by WarmTick on the task
		// thread (one archive per tick), read there as well.
		std::size_t g_warmIdx = 0;
		bool g_warmDone = false;


		// -----------------------------------------------------------------------
// 		BA2 archives: open, index the name table, read one entry
		// -----------------------------------------------------------------------
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
				// The original BTDX header is 24 bytes; v2 grew it to 36 and v3
				// put the compression method at +32. A v1 archive therefore
				// starts its TOC at 24, not 36: reading 36 consumed the first
				// 12 bytes of the table and every later entry parsed garbage.
				// Star Wars Genesis ships its whole armour compendium that way
				// (31 archives in one profile), so those textures were simply
				// invisible to the preview while the engine rendered them.
				char hdr[36]{};
				ar->file.read(hdr, 24);
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
				ar->dataEnd = ntOff;
				if (num == 0 || num > 4'000'000) {
					break;
				}
				ar->isTex = std::memcmp(htyp, "DX10", 4) == 0;
				const bool legacyHeader = hver == 1;
				if (!legacyHeader) {
					ar->file.read(hdr + 24, 12);
					if (!ar->file) {
						break;
					}
					if (hver >= 3) {
						std::memcpy(&ar->method, hdr + 32, 4);  // 0 = zlib, 3 = LZ4
					}
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
						// v1 general records lead with a name hash, which shifts
						// the offset and length fields by four bytes relative to
						// v2/v3. One archive in the current profile is v1 GNRL
						// (118 textures in the Genesis compendium); with the v2
						// offsets it listed the names and then failed every read.
						const bool legacyGnrl = hver == 1;
						Ba2Entry ent;
						std::memcpy(&ent.off, e + (legacyGnrl ? 16 : 12), 8);
						std::memcpy(&ent.size, e + (legacyGnrl ? 28 : 24), 4);
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
				// v1 textures share v3's body and chunk layout (4B prefix,
				// absolute chunk offsets, zlib); only the header size differs.
				const bool isV3 = hver >= 3 || legacyHeader;
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
			if (!bad && ar->isTex && hver == 2) {
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
				// Per-archive: DEBUG. A big modlist opens ~900 of these and the
				// player's log is not the place for them; EnsureBa2List and the
				// warm pass both print an INFO summary with the totals.
				REX::DEBUG("SFShopExplorer live: archive {} ({} entries)", a_path.filename().string(),
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
			// Beth's GNRL size field (entry +24) is the DECOMPRESSED
			// length, not the on-disk one. Starfield - Materials.ba2 proves
			// it: its single materialsbeta.cdb entry sits at offset 68 with
			// size 105,037,616 while the stream is only 17,576,587 bytes
			// (68 + 17,576,587 = the name-table offset, exactly). Reading
			// size bytes short-reads, and the old code turned that into a
			// cleared 0-byte buffer, so the whole CDB index silently
			// vanished and every material fell back to the fuzzy scorer.
			// The bulk region ends at the name table, so min(size, that
			// span) is the real stream length for both conventions.
			const std::uint64_t avail = a_ar->dataEnd > a_e.off ? a_ar->dataEnd - a_e.off : a_e.size;
			const std::size_t streamLen = static_cast<std::size_t>(std::min<std::uint64_t>(a_e.size, avail));
			if (streamLen == 0) {
				a_out.clear();
				return false;
			}
			a_ar->file.clear();
			a_ar->file.seekg(static_cast<std::streamoff>(a_e.off));
			a_out.resize(streamLen);
			a_ar->file.read(reinterpret_cast<char*>(a_out.data()), static_cast<std::streamsize>(streamLen));
			if (!a_ar->file) {
				a_out.clear();
				return false;
			}
			if (a_out[0] != 0x78) {
				return true;  // stored uncompressed
			}
			// zlib stream: inflate with a growing destination. When
			// size > streamLen the field is provably the decompressed
			// length, so the exact output buffer is known up front and
			// the 4x grow loop (420MB for the CDB) is skipped entirely.
			const bool exactSize = a_e.size > streamLen;
			std::size_t cap = exactSize ? static_cast<std::size_t>(a_e.size)
										: static_cast<std::size_t>(a_e.size) * 4 + 1024;
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
				if (zr != Z_BUF_ERROR || exactSize) {
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
			bool complete = true;
			std::vector<std::filesystem::path> dataPart;
			std::error_code ec;
			if (g_dataDir.empty() || !std::filesystem::is_directory(g_dataDir, ec)) {
				REX::WARN("SFShopExplorer live: cannot enumerate base Data BA2s at '{}' ({})",
					g_dataDir.string(), ec ? ec.message() : "directory missing");
				complete = false;
			} else {
				std::filesystem::directory_iterator it(g_dataDir, ec), end;
				if (ec) {
					REX::WARN("SFShopExplorer live: cannot enumerate base Data BA2s at '{}' ({})",
						g_dataDir.string(), ec.message());
					complete = false;
				} else {
					for (; it != end; it.increment(ec)) {
						if (ec) {
							break;
						}
						std::error_code entryEc;
					const bool regular = it->is_regular_file(entryEc);
					if (entryEc) {
						complete = false;
						REX::WARN("SFShopExplorer live: cannot inspect Data entry '{}' ({})",
							it->path().string(), entryEc.message());
						break;
					}
					if (regular && LowerAscii(it->path().extension().string()) == ".ba2") {
						dataPart.push_back(it->path());
					}
					}
					if (ec) {
						complete = false;
						REX::WARN("SFShopExplorer live: Data archive enumeration stopped at '{}' ({})",
							g_dataDir.string(), ec.message());
					}
				}
			}
			std::sort(dataPart.begin(), dataPart.end());
			// Mod-override semantics: explicit INI extraba2, then MO2
			// auto-discovered mod archives, resolve BEFORE base game data
			// (a mod replacing a base NIF/texture must win the lookup).
			std::vector<std::filesystem::path> modsPart;
			for (const auto& p : g_extraBa2) {
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
			g_ba2ListComplete = complete;
			g_ba2ListBuilt = true;
			REX::INFO("SFShopExplorer live: {} BA2 archives indexed ({} mod override, complete={})",
				g_ba2List.size(), modsPart.size(), complete);
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
			if (!g_dataDir.empty() && ReadLoose(g_dataDir / rel, a_out)) {
				return true;
			}
			EnsureBa2List();
			// Archive name tables are stored lower-case with '/' separators
			// (OpenBa2 normalizes them). NIF material strings and form model
			// paths arrive as "Materials\\SWProps\\Datapad.mat", so looking up
			// the caller's spelling missed every archive-shipped asset even
			// though the loose-file walk above accepted it (Windows is case-
			// and separator-insensitive on disk, not in the map).
			const auto archiveKey = LowerAscii(a_key);
			for (const auto& ap : g_ba2List) {
				auto ar = OpenBa2(ap);
				if (!ar->valid) {
					continue;
				}
				const auto it = ar->entries.find(archiveKey);
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
			// Some exporters (Blender's Starfield mesh plugin) write the LOD0
			// .mesh payload straight into the BSGeometry block instead of
			// naming a geometries/<guid>.mesh archive entry. Those NIFs parse
			// fine but every factory lookup misses, so the item drew grey.
			std::vector<std::uint8_t> inlineMesh;
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


		// -----------------------------------------------------------------------
// 		NIF 20.2.0.7 parser: node tree, local transforms, BSGeometry, connects
		// -----------------------------------------------------------------------
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

		// Build-81: quat(w,x,y,z) + translation + scale -> 4x4 entry matrix
		// (row-major, column-vector convention; same formulas as the offline
		// python validator that placed every part of all 13 assemblies).
		// Fails closed on non-finite fields or degenerate scale.
		bool QuatEntry(Reader& a_r, std::array<float, 16>& a_m)
		{
			float f[8];
			for (int k = 0; k < 8; ++k) {
				f[k] = a_r.F32();
			}
			if (!a_r.ok) {
				return false;
			}
			for (int k = 0; k < 8; ++k) {
				if (!std::isfinite(f[k])) {
					return false;
				}
			}
			const float w = f[0], x = f[1], y = f[2], z = f[3];
			const float s = f[7];
			if (!(s > 0.0f) || s > 1e6f) {
				return false;
			}
			a_m = { (1 - 2 * (y * y + z * z)) * s, (2 * (x * y - z * w)) * s, (2 * (x * z + y * w)) * s, f[4],
				(2 * (x * y + z * w)) * s, (1 - 2 * (x * x + z * z)) * s, (2 * (y * z - x * w)) * s, f[5],
				(2 * (x * z - y * w)) * s, (2 * (y * z + x * w)) * s, (1 - 2 * (x * x + y * y)) * s, f[6],
				0.0f, 0.0f, 0.0f, 1.0f };
			return true;
		}

		// Build-81: scan the block table for BSConnectPoint::Parents /
		// BSConnectPoint::Children and fill a_out. Block layouts proven
		// offline against every base-game weapon NIF; a malformed block is
		// skipped (its hosts never appear -> parts fall back to identity),
		// a malformed Children run stops at the first bad entry exactly like
		// the offline validator.
		void ParseConnectBlocks(const std::vector<std::uint8_t>& a_bytes, const NifDoc& a_doc,
			std::uint32_t a_nb, ConnectInfo& a_out)
		{
			for (std::uint32_t i = 0; i < a_nb; ++i) {
				const auto& tn = a_doc.types[a_doc.typeIdx[i]];
				if (tn != "BSConnectPoint::Parents" && tn != "BSConnectPoint::Children") {
					continue;
				}
				Reader r{ a_bytes.data() + a_doc.offsets[i], a_doc.sizes[i], 0, true };
				(void)r.U32();  // block format version (parents=3, children=2)
				if (tn == "BSConnectPoint::Parents") {
					const std::uint32_t n = r.U32();
					if (!r.ok || n > 1024) {
						continue;
					}
					for (std::uint32_t e = 0; e < n && r.ok; ++e) {
						ConnectPt pt;
						pt.node = r.HStr(4096);
						pt.name = r.HStr(4096);
						if (!r.ok) {
							break;
						}
						if (QuatEntry(r, pt.m)) {
							a_out.parents.push_back(std::move(pt));
						}
					}
				} else {
					for (int k = 0; k < 5; ++k) {  // 5-byte pad after the version
						(void)r.U8();
					}
					while (r.ok && r.o + 4 <= r.n) {
						const std::uint32_t ln = r.U32();
						if (!r.ok || ln == 0 || ln > 512 || r.o + ln > r.n) {
							break;
						}
						bool ascii = true;
						for (std::uint32_t k = 0; k < ln; ++k) {
							const char c = static_cast<char>(r.p[r.o + k]);
							if (c < 32 || c > 126) {
								ascii = false;
								break;
							}
						}
						if (!ascii) {
							break;
						}
						a_out.childs.emplace_back(reinterpret_cast<const char*>(r.p + r.o), ln);
						r.o += ln;
					}
				}
			}
		}

		bool ParseNifGeos(const std::vector<std::uint8_t>& a_bytes, std::vector<NifGeo>& a_out,
			std::string& a_stage, std::string& a_detail, ConnectInfo* a_conn = nullptr)
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
			// Connect-only callers tolerate geometry-less stubs (lawgiver
			// standard barrel is a 518-byte marker whose "barrel" lives in
			// the receiver body); the walk then collects no geos.
			if (doc.niNode < 0 || (doc.bsGeo < 0 && a_conn == nullptr)) {
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
			if (a_conn != nullptr) {
				ParseConnectBlocks(a_bytes, doc, nb, *a_conn);
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
				if (static_cast<int>(doc.typeIdx[0]) != doc.niNode && a_conn == nullptr) {
					a_stage = "noroot";
					return false;
				}
				// Connect callers mirror the offline validator: fall back to
				// block 0 even when its type is unexpected (the walk then
				// records nothing from a non-NiNode root).
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
						(void)cr.U32();  // unk (0x40 in every shipped file)
						const std::size_t pathAt = cr.o;
						const std::uint32_t ln = cr.U32();
						if (!cr.ok || ni == 0 || ni > 20'000'000 || nv == 0 || nv > 10'000'000) {
							break;
						}
						// A Bethesda LOD entry continues with u32 pathLen + a
						// printable "guid\\guid" factory path. The Blender exporter
						// writes no path at all and follows the 13-byte entry with
						// the raw .mesh LOD0 blob, so the field here is the blob's
						// own leading u32 (2) and the "length" is meaningless.
						// Test the bytes that would be the path: non-printable
						// means inline geometry, and the rest of the block is the
						// .mesh file verbatim (verified on All Trilogy Lightsabers
						// and Star Wars Genesis - Lightsabers: ni/nv match the
						// entry header and the UV stream lands in 0..1).
						bool isPath = ln != 0 && ln <= 512 && cr.Need(ln);
						for (std::uint32_t k = 0; isPath && k < ln; ++k) {
							const char c = static_cast<char>(blk[pathAt + 4 + k]);
							isPath = c >= 32 && c <= 126;
						}
						if (!isPath) {
							geo.lodIndices = ni;
							geo.lodVerts = nv;
							geo.inlineMesh.assign(blk + pathAt, blk + blen);
							got = true;
							break;
						}
						std::string path(reinterpret_cast<const char*>(blk + pathAt + 4), ln);
						cr.o = pathAt + 4 + ln;
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
					if (a_conn != nullptr) {
						// Build-81: record node worlds for connect hosts
						// (later same-named node overwrites, as offline) and
						// the root frame world for empty host names.
						const std::string nm =
							np.name < doc.strings.size() ? doc.strings[np.name] : std::string{};
						if (!nm.empty()) {
							std::array<float, 16> w{};
							std::memcpy(w.data(), world.m, sizeof(world.m));
							a_conn->nodeWorlds.insert_or_assign(nm, w);
						}
						if (fr.block == root) {
							std::memcpy(a_conn->rootWorld.data(), world.m, sizeof(world.m));
						}
					}
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

		// A UV stream is not confined to the 0..1 tile. The Mark I backpack
		// (geometries/657478c21f1e48a01425/52b4455363dc68a70393.mesh,
		// 2497v) authors whole-tile wrap offsets: 72 of its vertices reach
		// u=-11.30 and v=26.17, which the sampler resolves under
		// D3D12_TEXTURE_ADDRESS_MODE_WRAP into ordinary atlas coordinates.
		// The old tight bound rejected the WHOLE set on one such vertex, so
		// the pack sampled texel (0,0) over every face and drew flat grey.
		// The bound therefore only has to separate "a UV pair" from "some
		// other attribute stream": finite, and wide enough for wrap offsets
		// and for texel-space UVs (a 4096 atlas).
		constexpr float kMaxAbsUv = 4096.0f;

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
			}// UV stream: u32 count + f16 pairs, walked as a LIST rather than
		// taken from stream 0. A shape's LOD0 carries several attribute
		// streams and only the UV ones have count == nv, but the first UV
		// stream is not always the one the material samples: the action
		// figure packaging front cover (282i/96v) has stream 0 with count
		// 96 and every vertex at u=0, v=1 - a degenerate set that samples
		// a single texel and paints the whole front cover one colour,
		// while later streams carry the real atlas sub-rectangles. Skip a
		// stream whose u and v both collapse to a point and keep looking;
		// the accepted set is the first one that actually spans a range.
			std::vector<float> uv;
			{
				Reader u{ r.p, r.n, posEnd, true };
				std::uint32_t degenerate = 0;
				for (std::uint32_t guard = 0; guard < 16 && u.ok; ++guard) {
					const std::size_t streamAt = u.o;
					const std::uint32_t nuv = u.U32();
					if (!u.ok || nuv == 0 || nuv > 4'000'000) {
						break;
					}
					if (nuv != nv || !u.Need(static_cast<std::size_t>(nv) * 4)) {
						// Not a UV set. Its payload width is not derivable
						// from here, so stop rather than mis-walk the rest.
						(void)streamAt;
						break;
					}
					std::vector<float> cand(static_cast<std::size_t>(nv) * 2);
					float minU = 3.4e38f, maxU = -3.4e38f;
					float minV = 3.4e38f, maxV = -3.4e38f;
					bool finite = true;
					for (std::uint32_t q = 0; q < nv && finite; ++q) {
						// Preserve the UV convention from Starfield's raw .mesh
						// stream. The offline OBJ cache needs its own V flip, but
						// that conversion does not apply to this native format.
						const float su = HalfToFloat(u.U16());
						const float sv = HalfToFloat(u.U16());
						cand[static_cast<std::size_t>(q) * 2 + 0] = su;
						cand[static_cast<std::size_t>(q) * 2 + 1] = sv;
						finite = finite && std::isfinite(su) && std::isfinite(sv) &&
							std::abs(su) <= kMaxAbsUv && std::abs(sv) <= kMaxAbsUv;
						minU = std::min(minU, su);
						maxU = std::max(maxU, su);
						minV = std::min(minV, sv);
						maxV = std::max(maxV, sv);
					}
					if (!u.ok || !finite) {
						break;
					}
					const bool spans = (maxU - minU) > 1.0e-4f || (maxV - minV) > 1.0e-4f;
					if (spans) {
						uv = std::move(cand);
						if (degenerate != 0) {
							REX::INFO("SFShopExplorer live: skipped {} degenerate UV stream(s), "
								"using stream {}", degenerate, guard);
						}
						break;
					}
					++degenerate;
				}
			}
			a_pos = std::move(pos);
			a_uv = std::move(uv);
			a_idx = std::move(idx);
			return true;
	}



	void LoadConfig()
	{
		std::scoped_lock lock(g_mutex);
		if (g_configLoaded) {
			return;
		}
		g_configLoaded = true;
		const auto ini = DllDirectory() / kIniFileName;
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
		wchar_t dir[MAX_PATH]{};
		GetPrivateProfileStringW(L"offlinepreview", L"dir", kAssetsModelsDir, dir, MAX_PATH, ini.c_str());
		const std::filesystem::path rel(dir);
		g_cacheDir = rel.is_absolute() ? rel : DllDirectory() / rel;
		REX::INFO("SFShopExplorer live: enabled={} data={} extradirs={} extraba2={} (assets from the loaded Data view)",
			g_enabled, g_dataDir.string(), g_extraDirs.size(), g_extraBa2.size());
		REX::INFO("SFShopExplorer cdb: cache directory configured as '{}' (index '{}')",
			g_cacheDir.string(), (g_cacheDir / L"cdbindex.bin").string());
	}

	// Assets are whatever this process can see in Data.
	//
	// A mod manager (MO2, Vortex) presents its enabled mods through a VFS, so
	// the Data directory read here is already the merged, priority-resolved
	// view: for any given path exactly one file exists, and it is the one the
	// game itself would load. That is the definition of "what is loaded".
	//
	// Earlier versions parsed ModOrganizer.ini, guessed the selected profile
	// and read its modlist.txt to pick mod directories out of the manager's
	// own tree. That guessed a second time at an answer the VFS had already
	// resolved, and it was wrong whenever the profile name, the manager, or
	// the install layout differed - which is how the shop ended up
	// disagreeing with the game and looking like a load-order fault.


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

	// ── Layered material swaps (BGSLayeredMaterialSwap) ──────────────────
	// Starfield garments do not carry their colour on the NIF: the ARMO names
	// swap records that RENAME a material (Resort Wear's tee becomes
	// Outfit_Trucker_Tee_Resort_Upperbody.mat, Star Roamer's suit becomes the
	// white Generic one), and only that rename makes the item look like itself.
	// The preview reads the NIF's own names, so every recoloured garment was
	// previewed as its stock parent - grey tee, camouflage jacket, blue suit.
	// The table is generated offline (tools/gen_material_swaps.py); this only
	// reads it. No file means the previous behaviour, not an error.
	namespace
	{
		// formID -> {normalised key material, override material}. The override
		// keeps the file's own spelling so it can be logged and compared
		// against the NIF path the user can see.
		std::unordered_map<std::uint32_t, std::vector<std::pair<std::string, std::string>>> g_matSwaps;
		bool g_matSwapsLoaded{ false };

		// Compare both sides the way the CDB keys materials: lowercase, forward
		// slashes, no Data\ or Materials\ prefix, no .mat suffix.
		std::string SwapKey(const std::string& a_path)
		{
			std::string s = LowerAscii(a_path);
			for (char& ch : s) {
				if (ch == static_cast<char>(92)) {
					ch = '/';
				}
			}
			while (!s.empty() && (s.front() == '/' || s.front() == ' ')) {
				s.erase(s.begin());
			}
			if (s.rfind("data/", 0) == 0) {
				s.erase(0, 5);
			}
			if (s.rfind("materials/", 0) == 0) {
				s.erase(0, 10);
			}
			// Geometry suffix such as the ":0" a NIF appends to a shader ref.
			const auto colon = s.find_last_of(':');
			if (colon != std::string::npos) {
				s.erase(colon);
			}
			if (s.size() > 4 && s.compare(s.size() - 4, 4, ".mat") == 0) {
				s.erase(s.size() - 4);
			}
			return s;
		}

		// g_mutex must be held (reads g_cacheDir, which CacheDir() would lock).
		void LoadMaterialSwapsLocked()
		{
			if (g_matSwapsLoaded) {
				return;
			}
			g_matSwapsLoaded = true;
			if (g_cacheDir.empty()) {
				return;
			}
			const auto path = g_cacheDir / L"materialswaps.tsv";
			std::ifstream in(path, std::ios::binary);
			if (!in) {
				REX::INFO("SFShopExplorer swaps: no '{}' - previews use stock NIF materials (run tools/gen_material_swaps.py)",
					path.string());
				return;
			}
			std::string line;
			std::size_t rows = 0;
			while (std::getline(in, line)) {
				if (!line.empty() && line.back() == '\r') {
					line.pop_back();
				}
				if (line.empty() || line.front() == '#') {
					continue;
				}
				const auto tab1 = line.find('\t');
				if (tab1 == std::string::npos) {
					continue;
				}
				const auto tab2 = line.find('\t', tab1 + 1);
				if (tab2 == std::string::npos) {
					continue;
				}
				std::uint32_t form = 0;
				try {
					form = static_cast<std::uint32_t>(std::stoul(line.substr(0, tab1), nullptr, 16));
				} catch (...) {
					continue;
				}
				if (form == 0) {
					continue;
				}
				const auto key = SwapKey(line.substr(tab1 + 1, tab2 - tab1 - 1));
				std::string over = line.substr(tab2 + 1);
				if (key.empty() || over.empty() || key == SwapKey(over)) {
					continue;
				}
				// The ESM stores "Clothes\\X.mat"; the preview's material paths
				// carry the "Materials\\" root, so hand back the same shape.
				if (SwapKey(over).rfind("materials/", 0) != 0) {
					over.insert(0, "Materials\\");
				}
				g_matSwaps[form].emplace_back(key, std::move(over));
				++rows;
			}
			REX::INFO("SFShopExplorer swaps: {} material override(s) for {} item form(s) from '{}'",
				rows, g_matSwaps.size(), path.string());
		}
	}

	std::string MaterialSwapFor(std::uint32_t a_formID, const std::string& a_matPath)
	{
		if (a_formID == 0 || a_matPath.empty()) {
			return {};
		}
		std::scoped_lock lock(g_mutex);
		LoadMaterialSwapsLocked();
		const auto it = g_matSwaps.find(a_formID);
		if (it == g_matSwaps.end()) {
			return {};
		}
		const auto key = SwapKey(a_matPath);
		for (const auto& [swapKey, override] : it->second) {
			if (swapKey == key) {
				return override;
			}
		}
		return {};
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
		}			std::vector<NifGeo> geos;
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
			// Factory strings vary by authoring tool: vanilla stores the
			// bare "guid/guid" form, some plugins embed backslashes and a
			// ".mesh" suffix ("BubBow\bbow_body.mesh"). Normalize to the
			// archive key form "geometries/<lower>/<name>.mesh". If the
			// transform changed anything, fall back to the raw legacy key
			// too, so archives that store the unnormalized name still hit;
			// vanilla factories normalize to themselves (single lookup,
			// byte-identical to the old behavior).
			std::string fkey = LowerAscii(g.factory);
			std::replace(fkey.begin(), fkey.end(), '\\', '/');
			if (fkey.size() > 5 && fkey.compare(fkey.size() - 5, 5, ".mesh") == 0) {
				fkey.resize(fkey.size() - 5);
			}
			if (fkey.compare(0, 11, "geometries/") == 0) {
				fkey.erase(0, 11);
			}
			const std::string legacy = "geometries/" + LowerAscii(g.factory) + ".mesh";
			const std::string mkey = "geometries/" + fkey + ".mesh";
			std::vector<std::uint8_t> meshBytes;
			if (!g.inlineMesh.empty()) {
				// Blender-exported NIFs carry the LOD0 blob themselves; there is
				// no archive entry to look up (the lightsaber pack reported
				// "3 geos, 3 mesh misses" and drew a grey hilt).
				meshBytes = g.inlineMesh;
			} else if (!ResolveAsset(mkey, meshBytes) &&
				(mkey == legacy || !ResolveAsset(legacy, meshBytes))) {
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
			REX::DEBUG("SFShopExplorer live: assembled {} ({} meshes, {} sibling parts)",
				a_key, a_out.size(), added);
		}
	}

	// Normalize a form model path to a game-relative BA2 key.
	// Empty when the path is not a NIF.
	static std::string NormalizeNifKey(const std::string& a_modelPath)
	{
		std::string key = LowerAscii(a_modelPath);
		// Form model paths use Windows separators while BA2 name-table keys
		// are always slash-separated. Canonicalize before prefix checks and
		// archive lookup (loose-file resolution handles either form).
		std::replace(key.begin(), key.end(), '\\', '/');
		while (!key.empty() && key.front() == '/') {
			key.erase(key.begin());
		}

		// Engine model fields are normally relative paths, but some mods store
		// a Data-rooted mesh key. Strip repeated known virtual roots before
		// interpreting the asset root; do not accept absolute paths as keys.
		// Some form fields contain Data/Data/meshes/... prefixes.
		while (key.compare(0, 5, "data/") == 0) {
			key.erase(0, 5);
		}
		// Collapse repeated mesh roots before deciding whether a path needs
		// the default meshes/ prefix.
		while (key.compare(0, 14, "meshes/meshes/") == 0) {
			key.erase(0, 7);
		}
		// Do not reinterpret another rooted asset namespace as a model path.
		if (key.compare(0, 11, "geometries/") == 0) {
			return {};
		}
		if (key.compare(0, 7, "meshes/") != 0) {
			key = "meshes/" + key;
		}
		// Reject traversal components and drive-qualified paths after
		// separator/root normalization, before using the key for any lookup.
		for (std::size_t pos = 0; pos <= key.size();) {
			const auto end = key.find('/', pos);
			const auto stop = end == std::string::npos ? key.size() : end;
			const auto part = key.substr(pos, stop - pos);
			if (part == ".." || part.find(':') != std::string::npos) {
				return {};
			}
			if (end == std::string::npos) {
				break;
			}
			pos = end + 1;
		}
		if (key.compare(0, 7, "meshes/") != 0) {
			return {};
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

	// Stage A2 support for NativeModel: BuildSingleNif's full chain without
	// the [livemodel] enabled gate (BuildModels refuses when enabled=0, and
	// the shop runs with enabled=0 — direct-live only). Loads config on
	// first call, then the same ResolveAsset -> ParseNifGeos ->
	// geometries/<guid>.mesh -> ParseMeshLod0 -> world-transform path the
	// historical weapon flow validated byte-for-byte against MeshConverter.
	bool ParseNifMeshes(const std::string& a_modelPath, std::vector<LiveMesh>& a_out)
	{
		a_out.clear();
		LoadConfig();
		std::scoped_lock lock(g_mutex);
		if (!g_configLoaded) {
			return false;
		}
		const auto key = NormalizeNifKey(a_modelPath);
		if (key.empty()) {
			return false;
		}
		std::size_t totalVerts = 0;
		return BuildSingleNif(key, a_out, totalVerts, false) && !a_out.empty();
	}

	// Build-81: connect-point pass for NativeModel's assembly placement.
	// Same asset resolution as ParseNifMeshes, but only the doc + node-walk
	// part of ParseNifGeos runs for the ConnectInfo output; geometry-less
	// stubs are accepted (they still carry Parents/Children blocks).
	bool ParseNifConnects(const std::string& a_modelPath, ConnectInfo& a_out)
	{
		a_out = ConnectInfo{};
		LoadConfig();
		std::scoped_lock lock(g_mutex);
		if (!g_configLoaded) {
			return false;
		}
		const auto key = NormalizeNifKey(a_modelPath);
		if (key.empty()) {
			return false;
		}
		std::vector<std::uint8_t> bytes;
		if (!ResolveAsset(key, bytes)) {
			return false;
		}
		std::vector<NifGeo> geos;
		std::string stage, detail;
		(void)ParseNifGeos(bytes, geos, stage, detail, &a_out);
		return !a_out.parents.empty() || !a_out.childs.empty();
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
				REX::DEBUG("SFShopExplorer live: built {} ({} meshes)", key, a_out.size());
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


	// -----------------------------------------------------------------------
// 	Textures: DDS decode and DXGI format mapping
	// -----------------------------------------------------------------------
	std::uint32_t MapTexFormat(std::uint32_t a_dxgi)
	{
		// Match the preview's original UNORM sampling: converting these
		// sRGB variants darkened weapon colors in the last runtime test.
		// Alpha remains intact in the BC1 punch-through / BC2 / BC3 data.
		switch (a_dxgi) {
		case 71:
		case 72: return 71;  // BC1
		case 74:
		case 75: return 74;  // BC2
		case 77:
		case 78: return 77;  // BC3 preserves alpha
		case 80: return 80;  // BC4
		case 83: return 83;  // BC5
		// BC5_UNORM_SRGB (84). The archives tag most normal maps sRGB even
		// though the two channels are vector components, not colour: gamma-
		// decoding X and Y would bend every slope towards the axis. The
		// Erdebrau can's pull ring lives in exactly such a map - the lid is a
		// flat 16-gon plate with no tab geometry and a plain foil albedo - so
		// it could never be drawn until this format was readable.
		case 84: return 83;  // BC5, sampled as linear vectors
		// BC7_TYPELESS (97), BC7_UNORM (98) and BC7_UNORM_SRGB (99) all
		// become BC7_UNORM. Texture packs repacked for Starfield tag almost
		// every map sRGB - 10,230 entries across one mod list - and the old
		// switch answered 0, so the whole archive's textures were dropped as
		// unsupported. Same policy as the other sRGB variants above: read the
		// UNORM bytes, because the preview's sampling is linear and
		// gamma-decoding here darkened the art.
		//
		// This used to return 97, which is BC7_TYPELESS, not BC7. The texture
		// resource accepted it, but UploadTextureImage then built the SRV
		// with format 97 too, and a typeless view format is invalid: the
		// descriptor was never created, so the first draw that sampled an
		// atlas read a garbage descriptor and removed the device. A-300 is
		// the first preview carrying a BC7 atlas - 1024x1024 with 11 mips -
		// which is why it was the first item to crash.
		case 97:
		case 98:
		case 99: return 98;  // BC7_UNORM
		default: return 0;
		}
	}

	std::size_t TexRowPitch(std::uint32_t a_fmt, std::uint32_t a_w)
	{
		if (a_fmt == 28) {
			return static_cast<std::size_t>(a_w) * 4;
		}
		const std::size_t bb = (a_fmt == 71 || a_fmt == 72 || a_fmt == 80) ? 8 : 16;
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
		// DDS_PIXELFORMAT begins at byte 76 of the file (4-byte magic + 72
		// header bytes): dwSize 76, dwFlags 80, dwFourCC 84. Reading the
		// pixel-format size from 72 landed in dwReserved1 (always 0) and the
		// FOURCC from 80 read DDPF_FOURCC itself, so every DDS that was not
		// already decoded from a DX10 BA2 texture chunk failed the pfSize
		// check. That is every loose mod texture and every DDS packed into a
		// GNRL archive - the whole Star Wars weapon set among them.
		std::memcpy(&pfSize, a_dds.data() + 76, 4);
		std::memcpy(&fourCC, a_dds.data() + 84, 4);
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
		} else if (fourCC == 0x33545844u) {
			fmt = 74;
		} else if (fourCC == 0x35545844u) {
			fmt = 77;  // DXT5 is BC3 and carries alpha
		} else if (fourCC == 0 && bitCount == 32 && rMask == 0x00FF0000u && gMask == 0x0000FF00u &&
			bMask == 0x000000FFu && aMask == 0xFF000000u) {
			fmt = 28;
		}
		if (fmt == 0) {
			return false;
		}
		const std::size_t pitch = TexRowPitch(fmt, width);
		const std::size_t rows = fmt == 28 ? height : (height + 3) / 4;
		const std::size_t topMipBytes = pitch * rows;
		std::uint32_t ddsFlags = 0, declaredMips = 0;
		std::memcpy(&ddsFlags, a_dds.data() + 8, 4);
		std::memcpy(&declaredMips, a_dds.data() + 28, 4);
		const std::size_t mipLevels = (ddsFlags & 0x20000u) != 0 && declaredMips > 1
			? std::min<std::size_t>(declaredMips, 16) : 1;
		std::size_t totalBytes = 0;
		std::size_t mipWidth = width, mipHeight = height;
		for (std::size_t mip = 0; mip < mipLevels; ++mip) {
			const std::size_t mipPitch = TexRowPitch(fmt, static_cast<std::uint32_t>(mipWidth));
			const std::size_t mipRows = fmt == 28 ? mipHeight : (mipHeight + 3) / 4;
			if (mipRows == 0 || mipPitch > (std::numeric_limits<std::size_t>::max() - totalBytes) / mipRows) {
				return false;
			}
			totalBytes += mipPitch * mipRows;
			mipWidth = std::max<std::size_t>(1, mipWidth / 2);
			mipHeight = std::max<std::size_t>(1, mipHeight / 2);
		}
		if (totalBytes > a_dds.size() - dataOff) {
			// Some loose mod DDS files declare an incomplete mip chain;
			// keep the valid base level rather than rejecting the albedo.
			if (topMipBytes > a_dds.size() - dataOff) {
				return false;
			}
			totalBytes = topMipBytes;
		}
		a_out.format = fmt;
		a_out.width = width;
		a_out.height = height;
		a_out.mipLevels = totalBytes == topMipBytes ? 1 : static_cast<std::uint32_t>(mipLevels);
		a_out.rowPitch = pitch;
		a_out.bytes.assign(a_dds.begin() + dataOff, a_dds.begin() + dataOff + totalBytes);
			return true;
	}

	bool ReadTexture(const std::string& a_key, TexImage& a_out, std::string* a_outSource)
	{
		a_out = TexImage{};
		if (a_outSource) {
			a_outSource->clear();
		}
		std::scoped_lock lock(g_mutex);
		// Build-76: pure file IO (loose dirs + archives) - the directory
		// lists LoadConfig builds unconditionally, and the offline GPU
		// prefetch needs textures while [livemodel] enabled=0 (the shop
		// mode). Gating this on g_enabled made every prefetch silently
		// empty: all ranges UNTEXTURED, items rendered grey.
		if (!g_configLoaded) {
			if (a_outSource) {
				*a_outSource = "<texture paths not configured>";
			}
			return false;
		}
		const std::string key = LowerAscii(a_key);
		// Session negative cache, shared with BuildSingleNif's. Without it
		// every unresolved texture re-walked all 383 archives (opening and
		// hashing each entry table) on every hover, which is pure stall on
		// the Present thread. Keyed by the same normalized name, and only
		// ever populated by a full miss - a texture that exists is found on
		// the first walk and never enters the cache.
		if (g_failed.find(key) != g_failed.end()) {
			if (a_outSource) {
				*a_outSource = "<cached unresolved>";
			}
			return false;
		}
		const auto rememberMiss = [&](const std::string& a_k) {
			// Bounded: a pathological content set could otherwise grow the
			// set without limit across a long session.
			if (g_failed.size() < 16384) {
				g_failed.insert(a_k);
			}
		};
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
				if (a_outSource) {
					*a_outSource = (dir / rel).string();
				}
				return true;
			}
		}
		if (!g_dataDir.empty()) {
			std::vector<std::uint8_t> dds;
			if (ReadLoose(g_dataDir / rel, dds) && DdsToImage(dds, a_out)) {
				if (a_outSource) {
					*a_outSource = (g_dataDir / rel).string();
				}
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
					if (a_outSource) {
					*a_outSource = ap.string();
				}
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
			const std::size_t topRows = fmt == 28 ? tf.height : (tf.height + 3) / 4;
			const std::size_t topMipBytes = pitch * topRows;
			std::vector<std::pair<const TexChunk*, std::size_t>> mipChunks;
			std::uint32_t nextMip = 0;
			std::size_t chainBytes = 0;
			std::size_t mipWidth = tf.width, mipHeight = tf.height;
			while (nextMip < tf.mips) {
				const auto chunkIt = std::find_if(tf.chunks.begin(), tf.chunks.end(),
					[nextMip](const TexChunk& c) { return c.startMip == nextMip; });
				if (chunkIt == tf.chunks.end() || chunkIt->endMip < chunkIt->startMip || chunkIt->endMip >= tf.mips) {
					break;
				}
				std::size_t chunkBytes = 0;
				for (std::uint32_t mip = chunkIt->startMip; mip <= chunkIt->endMip; ++mip) {
					const std::size_t mipPitch = TexRowPitch(fmt, static_cast<std::uint32_t>(mipWidth));
					const std::size_t mipRows = fmt == 28 ? mipHeight : (mipHeight + 3) / 4;
					if (mipRows == 0 || mipPitch > (std::numeric_limits<std::size_t>::max() - chunkBytes) / mipRows) {
						chunkBytes = 0;
						break;
					}
					chunkBytes += mipPitch * mipRows;
					mipWidth = std::max<std::size_t>(1, mipWidth / 2);
					mipHeight = std::max<std::size_t>(1, mipHeight / 2);
				}
				if (chunkBytes == 0 || chunkIt->size < chunkBytes ||
					chunkBytes > std::numeric_limits<std::size_t>::max() - chainBytes) {
					break;
				}
				mipChunks.emplace_back(&*chunkIt, chunkBytes);
				chainBytes += chunkBytes;
				nextMip = static_cast<std::uint32_t>(chunkIt->endMip) + 1;
			}
			if (mipChunks.empty() || mipChunks.front().first->startMip != 0 || chainBytes < topMipBytes) {
				continue;
			}
			const std::uint32_t mipLevels = nextMip;
			// The name table bounds the bulk region, so a corrupt chunk
			// record must never read past it into the string table. Ba2Read
			// already bounds the GNRL side this way; the offline harness
			// (tools/ba2_audit) flagged the DX10 side as the odd one out.
			std::vector<std::uint8_t> raw;
			raw.reserve(chainBytes);
			bool readOk = true;
			for (const auto& [chunk, chunkBytes] : mipChunks) {
				const std::size_t want = static_cast<std::size_t>(chunk->packed ? chunk->packed : chunk->size);
				if (ar->dataEnd == 0 || chunk->off >= ar->dataEnd || want > ar->dataEnd - chunk->off) {
					readOk = false;
					break;
				}
				ar->file.clear();
				ar->file.seekg(static_cast<std::streamoff>(chunk->off));
				std::vector<std::uint8_t> packed(want);
				ar->file.read(reinterpret_cast<char*>(packed.data()), static_cast<std::streamsize>(packed.size()));
				if (!ar->file) {
					readOk = false;
					break;
				}
				std::vector<std::uint8_t> chunkRaw;
				if (chunk->packed != 0 && chunk->packed != chunk->size) {
					chunkRaw.resize(chunk->size);
					bool ok = false;
					if (ar->method == 3) {
						ok = LZ4_decompress_safe(reinterpret_cast<const char*>(packed.data()),
								reinterpret_cast<char*>(chunkRaw.data()), static_cast<int>(packed.size()),
								static_cast<int>(chunk->size)) == static_cast<int>(chunk->size);
					} else {
						uLongf dl = static_cast<uLongf>(chunk->size);
						ok = uncompress(chunkRaw.data(), &dl, packed.data(), static_cast<uLong>(packed.size())) == Z_OK &&
							dl == chunk->size;
					}
					if (!ok) {
						readOk = false;
						break;
					}
				} else {
					chunkRaw.swap(packed);
				}
				if (chunkRaw.size() < chunk->size) {
					readOk = false;
					break;
				}
				raw.insert(raw.end(), chunkRaw.begin(), chunkRaw.begin() + chunkBytes);
			}
			if (!readOk || raw.size() != chainBytes) {
				continue;
			}
			a_out.format = fmt;
			a_out.width = tf.width;
			a_out.height = tf.height;
			a_out.mipLevels = mipLevels;
			a_out.rowPitch = pitch;
			a_out.bytes = std::move(raw);
			if (a_outSource) {
				*a_outSource = ap.string();
			}
			return true;
		}
		// Reached only when every loose dir and every archive was tried and
		// none had it: a true session miss, so remember it.
		rememberMiss(key);
		if (a_outSource) {
			*a_outSource = "<no successful source; absent or unreadable>";
		}
		return false;
	}

	struct ColorEntry
	{
		std::string key;   // full lowercased "textures/..._color.dds"
		std::string base;  // file base without suffix
		std::string family;// directory portion
	};

	// Is this texture key an albedo (a colour map), whatever the database
	// spells it? The game does not reserve one suffix. Measured in the base
	// game's own database: 41,000+ sets use "_color.dds", every canned-beverage
	// Menuitem_* set uses "_BaseColor.dds", the flora bases "...Purplecolor.dds",
	// a few detail sets "_albedo.dds"/"_diffuse.dds". Accepting only
	// "_color.dds" left those sets with no colour at all - and an empty set is
	// the game's own statement that the material is flat-shaded, so the item
	// drew untextured instead of failing through to the fuzzy scorer. The
	// Blend can (Aid) was one of them: its set names
	// textures/items/fooddrink_singleitems/all_packaged_consumables/
	// menuitem_blend/menuitem_blend_basecolor.dds.
	//
	// The role is read from the LAST underscore token of the FILE NAME, so a
	// directory that happens to contain "color" can never qualify a map.
	bool CdbKeyIsAlbedo(const std::string& a_key)
	{
		if (a_key.size() < 9 || a_key.compare(a_key.size() - 4, 4, ".dds") != 0) {
			return false;
		}
		const auto slash = a_key.find_last_of('/');
		const std::size_t fileAt = slash == std::string::npos ? 0 : slash + 1;
		const std::string stem = a_key.substr(fileAt, a_key.size() - fileAt - 4);
		const auto underscore = stem.find_last_of('_');
		const std::string token = underscore == std::string::npos ? stem : stem.substr(underscore + 1);
		const auto endsWith = [&](const char* const a_tail) {
			const std::size_t n = std::strlen(a_tail);
			return token.size() >= n && token.compare(token.size() - n, n, a_tail) == 0;
		};
		static constexpr const char* kNonAlbedo[] = { "opacity", "alpha", "normal", "rough",
			"roughness", "metal", "metalness", "mask", "emissive", "glow", "gloss",
			"specular", "height", "ao" };
		for (const char* const tail : kNonAlbedo) {
			if (endsWith(tail)) {
				return false;
			}
		}
		static constexpr const char* kAlbedo[] = { "color", "colour", "albedo", "diffuse" };
		for (const char* const tail : kAlbedo) {
			if (endsWith(tail)) {
				return true;
			}
		}
		return false;
	}

	// The same file name with its albedo annotation removed
	// ("menuitem_blend_basecolor.dds" -> "menuitem_blend"), so the fuzzy colour
	// index compares like with like whatever spelling the map uses. For the
	// "_color.dds" maps this is byte-identical to the old 10-character erase.
	std::string AlbedoBaseName(const std::string& a_fileName)
	{
		std::string s = a_fileName;
		// Bethesda ships texture files with spaces where the material name has
		// underscores ("erdebrau light_decal_color.dds"). Without folding them
		// the exact-base score never fires, and the shorter-base tiebreak then
		// hands the light can the DARK decal - the reported Erdebrau Light can.
		std::replace(s.begin(), s.end(), ' ', '_');
		if (s.size() > 4 && s.compare(s.size() - 4, 4, ".dds") == 0) {
			s.erase(s.size() - 4);
		}
		static constexpr const char* kAlbedo[] = { "color", "colour", "albedo", "diffuse" };
		for (const char* const tail : kAlbedo) {
			const std::size_t n = std::strlen(tail);
			if (s.size() > n && s.compare(s.size() - n, n, tail) == 0) {
				s.erase(s.size() - n);
				break;
			}
		}
		if (s.size() > 4 && s.compare(s.size() - 4, 4, "base") == 0) {
			s.erase(s.size() - 4);
		}
		if (!s.empty() && s.back() == '_') {
			s.pop_back();
		}
		return s;
	}

	std::vector<ColorEntry> g_colors;
	bool g_colorsBuilt{ false };
	// The session memo has to carry BOTH halves of a verdict. A material
	// whose shipped texture set names only an opacity map answers "flat" to
	// FindDiffuse - there genuinely is no albedo - yet it must still hand
	// back that mask so the caller can promote it (see the promoted-mask
	// path in OfflineModel). Memoizing the colour alone stored "" for that
	// case, so the FIRST lookup promoted the mask correctly and every
	// later lookup of the same material+node returned false with an EMPTY
	// opacity, silently dropping the mask and leaving the range flat. The
	// symptom is order-dependent, which is why it never showed up in a log
	// where each material was resolved once.
	struct DiffuseVerdict
	{
		std::string color;    // resolved albedo key, "" = none
		std::string opacity;  // companion mask, "" = none
	};
	std::unordered_map<std::string, DiffuseVerdict> g_diffuseMemo;
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
			if (!CdbKeyIsAlbedo(name)) {
				return;
			}
			ColorEntry e;
			e.key = name;
			const auto slash = name.find_last_of('/');
			e.family = slash == std::string::npos ? std::string{} : name.substr(0, slash);
			e.base = AlbedoBaseName(slash == std::string::npos ? name : name.substr(slash + 1));
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

	// materialsbeta.cdb (Starfield - Materials.ba2) stores every base-game
	// material's texture set as a "<name>_TextureSetN" record followed by
	// that set's texture-path fields in slot order: colour, opacity, normal,
	// rough, metal, ... Base-game .mat files do not exist on disk,
	// so this table is the authoritative material -> map answer. Verified
	// examples: Food_Produce_Tomato_TextureSet1 ->
	// textures/items/.../ProduceBox_ProduceOnly/Tomato_Base_color.dds and	//   Abstract_Poster2x1_TextureSet1 -> .../512x1024/Poster_512x1024_02_color.dds.
	// A DIFF object's name record opens its group and its fields follow it.
	// Texture paths are buffered after a name and committed when the NEXT name
	// closes the object; only a _TextureSetN object consumes them. Without this
	// boundary, neighbouring
	// material albedos leak into textureless book covers and texture-set paths
	// can be lost before decal materials ever reach the lookup table.
	struct CdbTexSet
	{
		std::string color;
		std::string opacity;
	};
	std::unordered_map<std::string, CdbTexSet> g_cdbSets;
	bool g_cdbIndexed{ false };

	// Per-material alpha straight out of the compiled database.
	//
	// materialsbeta.cdb is a BETH container whose records are
	//   "DIFF" u32 len, u32 fieldId, u16 kind, u16 count, <count elements>
	// with len counting everything after itself, so records for one object
	// are contiguous and the object's name arrives FIRST, followed by its own
	// components (0x020f id, 0x022b kind, 0x025a colour) until the next name. Across
	// the whole 105 MB file only 43 distinct field ids ever appear, and the
	// only one a _Material object ever carries besides its name is
	// 0x25a - MaterialOverrideColorTypeComponent::Color, an XMFLOAT4
	// stored as (f32 value, u16 indexOfNext) pairs at stride 6, 0xffff
	// terminated. Its w channel is the material's own override alpha:
	// across 15,226 materials that carry a colour it is 1.0 for 7,010 of
	// them, 0.95/0.91/0.80/0.12/0.08/0.04 for tinted variants, and the
	// decal family sits at a median of 0.21. That is a real per-material
	// value, so it replaces the flat 0.30 blend constant wherever the
	// database supplies one.
	// (6r-7) The rgb is kept too, not just w. It is the material's own tint and
	// 79% of the 10,666 materials that carry an override have a visibly
	// tinted one (max channel < 0.97); 4,014 of those have NO colour map at
	// all and therefore render today as an untextured blob whose colour is
	// guessed from a name word, or as neutral grey when the name has none.
	struct CdbOverride
	{
		float rgba[4]{ 1.0f, 1.0f, 1.0f, 1.0f };
	};
	std::unordered_map<std::string, CdbOverride> g_cdbAlpha;

	// TextureSetKind per object (field 0x22b, BSMaterial::TextureSetKindComponent).
	// Only two values exist in the whole base database - "Multiply" (5,014
	// objects) and "Lerp" (277) - and both name how a material's texture set
	// composites over the layer beneath it. For those the override alpha is
	// that layer's blend weight, NOT the surface's opacity.
	//
	// It cannot live inside CdbOverride. The kind is authored in the base
	// database while the override colour can come from a Creation Kit database
	// parsed separately, and the two must be joined by object name: the
	// Razorback's receiver has its "Multiply" in materialsbeta.cdb and its
	// (0.67, 0.697, 1.0, 0.635) in SFBGS007's. That pairing is what stops a
	// solid rifle being drawn at 63.5% opacity; 1,681 of the 6,205 objects
	// carrying an override alpha below 1.0 are Multiply layers, so it is not a
	// per-item patch. The 4,458 with no kind record (the Tranquilitea cup base
	// among them, 0.608) keep the alpha as opacity, which is what the glass
	// needs.
	std::unordered_map<std::string, std::uint8_t> g_cdbKind;

	// "data\materials\...\x.mat_material1" -> "data/materials/.../x.mat"
	std::string CdbAlphaKey(const std::string& a_objectName)
	{
		std::string key = LowerAscii(a_objectName);
		for (char& ch : key) {
			if (ch == static_cast<char>(92)) {
				ch = '/';
			}
		}
		// Strip the _Material<N> object suffix the database appends.
		for (;;) {
			const auto us = key.rfind('_');
			if (us == std::string::npos) {
				break;
			}
			const std::string tail = key.substr(us + 1);
			if (tail.size() < 8 || tail.compare(0, 8, "material") != 0) {
				break;
			}
			bool digits = tail.size() > 8;
			for (std::size_t k = 8; k < tail.size(); ++k) {
				digits = digits && tail[k] >= '0' && tail[k] <= '9';
			}
			if (!digits) {
				break;
			}
			key.erase(us);
		}
		while (!key.empty() && key.front() == '/') {
			key.erase(key.begin());
		}
		return key;
	}

	// "Data\Textures\a\b\X_color.dds" -> "textures/a/b/x_color.dds"
	std::string CdbTextureKey(const std::string& a_path)
	{
		std::string flat;
		flat.reserve(a_path.size());
		for (const char c : a_path) {
			flat.push_back(c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
		}
		while (!flat.empty() && (flat.front() == '/' || flat.front() == ' ')) {
			flat.erase(flat.begin());
		}
		if (flat.compare(0, 5, "data/") == 0) {
			flat.erase(0, 5);
		}
		while (!flat.empty() && flat.front() == '/') {
			flat.erase(flat.begin());
		}
		if (flat.size() < 15 || flat.compare(flat.size() - 4, 4, ".dds") != 0 ||
			flat.compare(0, 9, "textures/") != 0) {
			return {};
		}
		return flat;
	}	// CDB slots are ordered; the colour slot is the first "_color" entry,

	// and the opacity slot the first "_opacity"/"_alpha" entry. Auxiliary
	// maps (normal/rough/metal/mask) are never colour candidates.
	//
	// One blob, two passes, in IndexCdbBlob. Both passes write into the
	// shared tables and neither ever overwrites an existing value, so
	// whichever blob is indexed FIRST wins and a later one can only fill
	// gaps. Indexing the base game first is therefore the whole precedence
	// policy - a mod that redefines a material the base game already knows
	// cannot shadow the game's own answer.
	struct CdbIndexStats
	{
		std::size_t sets{ 0 };        // texture-set keys this blob ADDED
		std::size_t overrides{ 0 };  // override-colour records decoded
	};

	// Is this record payload a texture path ("Data\Textures\x\y_color.dds")?
	//
	// The field id that carries texture maps is a property of the DATABASE,
	// not of the format. Measured across the installed Data directory, the
	// same kind of record appears as: 0x038A in the base materialsbeta.cdb
	// (212,877 records), 0x03A9 in sfbgs007 (210,301), 0x031F in
	// shatteredspace (21,466), 0x0EE8 in sfbgs050 (2,754), 0x02E7 in
	// sfbgs00d (1,876), 0x0DB1 in sfbgs003 (386), 0x0DB5 in sfbgs008 (40)
	// and 0x09E3 in sfbgs047 (20). Reading only 0x038A therefore left every
	// Creation's texture sets with no colour at all - and an empty set is
	// authoritative "flat shaded", which is why BreechBlock, the Antique
	// SMG and Revolver, the Cathode and the Shattered Space shelf all drew
	// as bare white geometry. Identify the record by what it holds instead;
	// CdbTextureKey still applies the real validation (textures/ prefix, a
	// .dds suffix) and rejects every other string field outright.
	bool LooksLikeTexturePath(const char* const a_data, const std::size_t a_count)
	{
		const auto matches = [&](const std::size_t a_at, const char* const a_needle, const std::size_t a_len) {
			if (a_at + a_len > a_count) {
				return false;
			}
			for (std::size_t i = 0; i < a_len; ++i) {
				const auto a = static_cast<unsigned char>(a_data[a_at + i]);
				const auto b = static_cast<unsigned char>(a_needle[i]);
				if (std::tolower(a) != std::tolower(b)) {
					return false;
				}
			}
			return true;
		};
		const auto isSep = [&](const std::size_t a_at) {
			return a_at < a_count &&
				(a_data[a_at] == static_cast<char>(92) || a_data[a_at] == '/');
		};
		std::size_t i = 0;
		while (i < a_count && (isSep(i) || a_data[i] == ' ')) {
			++i;
		}
		if (i + 5 <= a_count && matches(i, "data", 4) && isSep(i + 4)) {
			i += 5;
		}
		return i + 9 <= a_count && matches(i, "textures", 8) && isSep(i + 8);
	}

	// Several numbered texture sets collapse onto one material stem, and they
	// are NOT that material's maps: they are the material plus every per-object
	// swap variant the database ships. Merging them field by field (the old
	// first-blob, first-field rule) built frankenstein sets -
	// `plasticplainsmooth02` took its colour from TextureSet3
	// (MetalCopperAged01_color.dds) and its mask from TextureSet2
	// (UnitedColoniesLogo_01_opacity.dds), so the plastic plate on every food
	// item drew as bronze metal punched through by a mostly-black logo mask.
	//
	// A donor is therefore chosen whole. The numbering is the database's own
	// statement about which set is the material and which are swaps, so the
	// order is: a set that names the stem AND carries a colour, then a set that
	// names the stem, then any set with maps (lowest number = TextureSet1),
	// then an empty set. Records of the SAME number are one split set and are
	// unioned before ranking, which is what keeps decallabelsletters03's
	// authored colour.
	// One numbered texture set, plus whether it carried ANY map at all. The
	// colour and mask slots alone cannot express that, and it decides real
	// cases: `plasticplainsmooth02` TextureSet1 ships a `_rough` companion and
	// nothing else, so counting only the two kept slots made the unrelated
	// logo-mask set look richer than the material's own.
	struct CdbVariant
	{
		CdbTexSet maps;
		bool hasMaps{ false };
	};

	int CdbVariantQuality(const std::string& a_stem, const CdbVariant& a_variant)
	{
		const CdbTexSet& set = a_variant.maps;
		const bool named =
			(!set.color.empty() && set.color.find(a_stem) != std::string::npos) ||
			(!set.opacity.empty() && set.opacity.find(a_stem) != std::string::npos);
		if (named && !set.color.empty()) {
			return 3;
		}
		if (named) {
			return 2;
		}
		return a_variant.hasMaps ? 1 : 0;
	}

	CdbTexSet ChooseCdbVariant(const std::string& a_stem,
		const std::unordered_map<int, CdbVariant>& a_variants)
	{
		const CdbVariant* best = nullptr;
		int bestQuality = -1;
		int bestNum = 0;
		for (const auto& [num, variant] : a_variants) {
			const int quality = CdbVariantQuality(a_stem, variant);
			if (quality > bestQuality ||
				(quality == bestQuality && best != nullptr && num < bestNum)) {
				bestQuality = quality;
				bestNum = num;
				best = &variant;
			}
		}
		return best != nullptr ? best->maps : CdbTexSet{};
	}

	CdbIndexStats IndexCdbBlob(const char* const a_base, const std::size_t a_size)
	{
		CdbIndexStats stats;
		const std::string_view kSuffix = "_textureset";
		CdbTexSet pendingSet;
		bool pendingHasMaps = false;
		// stem -> (variant number -> that variant's own maps)
		std::unordered_map<std::string, std::unordered_map<int, CdbVariant>> variants;
		std::string activeTextureSet;
		int activeVariant = 0;
		const auto commitPendingSet = [&]() {
			if (!activeTextureSet.empty()) {
				// Same stem AND same number = one set seen twice: union it.
				auto& slot = variants[activeTextureSet][activeVariant];
				if (slot.maps.color.empty()) {
					slot.maps.color = pendingSet.color;
				}
				if (slot.maps.opacity.empty()) {
					slot.maps.opacity = pendingSet.opacity;
				}
				slot.hasMaps = slot.hasMaps || pendingHasMaps;
			}
			pendingSet = {};
			pendingHasMaps = false;
		};
		std::size_t pos = 0;
		while (pos + 16 <= a_size) {
			const auto tag = a_base + pos;
			if (std::memcmp(tag, "DIFF", 4) != 0) {
				++pos;
				continue;
			}
			std::uint32_t len = 0;
			std::memcpy(&len, tag + 4, 4);
			if (len < 12 || len > 65536 || pos + 8 + len > a_size) {
				pos += 4;
				continue;
			}
			std::uint32_t fieldId = 0;
			std::uint16_t kind = 0, count = 0;
			std::memcpy(&fieldId, tag + 8, 4);
			std::memcpy(&kind, tag + 12, 2);
			std::memcpy(&count, tag + 14, 2);
			const char* const data = tag + 16;
			const std::size_t textLen = kind == 0 && count > 0 && 8u + count <= len ?
				strnlen(data, count) : 0;
			if (textLen != 0 && LooksLikeTexturePath(data, textLen)) {
				const std::string raw(data, textLen);
				const auto key = CdbTextureKey(raw);
				if (!key.empty() && !activeTextureSet.empty()) {
					pendingHasMaps = true;
					if (pendingSet.color.empty() && CdbKeyIsAlbedo(key)) {
						pendingSet.color = key;
					} else if (pendingSet.opacity.empty() &&
						(key.find("_opacity.dds") != std::string::npos || key.find("_alpha.dds") != std::string::npos)) {
						pendingSet.opacity = key;
					}
				}
			} else if (fieldId == 0x01F4 && kind == 0 && count >= 3 && 8u + count <= len) {
				// A name starts an object's record group; following texture paths
				// belong to it until the next name. Flush the preceding group's
				// paths before changing owners. Assigning pre-name paths to this
				// incoming name shifts real weapon maps by one texture set.
				commitPendingSet();
				activeTextureSet.clear();
				std::string objectName(data, strnlen(data, count));
				for (char& ch : objectName) {
					ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
					if (ch == static_cast<char>(92)) {
						ch = '/';
					}
				}
				// A planet's terrain layer IS that planet's colour. Every planet
				// material in the database is a stack of named layers, and the
				// albedo sits on the `_terrain` one: `HoudiniPlanet_Alchiba II_
				// textureset_1_terrain` owns BiomeMasks/Alchiba II_color.dds, and
				// the desktop globes ask for the layer itself - their NIF names
				// `Desktop_PlanetGlobe_Planet_Alchiba II.mat`, whose record is
				// `Desktop_PlanetGlobe_Planet_Alchiba II_terrain` with the same
				// map. With `_terrain` still on the name, no candidate in
				// FindCdbMaterialLocked can reach either one (the digits in
				// `_textureset_1_terrain` are not the end of the string, and the
				// layer name is not a texture set at all), so the globe's sphere
				// drew as flat grey next to the engine's textured planet. Strip
				// the word before matching, so both forms key as the material
				// they belong to. Only the layers that can own art: the
				// `_material_`/`_uvstream_` records beside them are node graphs
				// and HDAs, and keying those would invent thousands of sets.
				static constexpr std::string_view kTerrainLayer = "_terrain";
				const bool terrainLayer = objectName.size() > kTerrainLayer.size() &&
					objectName.compare(objectName.size() - kTerrainLayer.size(), kTerrainLayer.size(),
						kTerrainLayer) == 0;
				std::string setKey = terrainLayer ?
					objectName.substr(0, objectName.size() - kTerrainLayer.size()) : objectName;
				const auto suffixAt = setKey.rfind(kSuffix);
				if (suffixAt != std::string::npos) {
					const auto digitsAt = suffixAt + kSuffix.size();
					const bool numbered = digitsAt < setKey.size() &&
						std::all_of(setKey.begin() + static_cast<std::ptrdiff_t>(digitsAt), setKey.end(),
							[](char ch) { return ch >= '0' && ch <= '9'; });
					// suffixAt == 0 means the name IS "_textureset<n>", with no
					// material stem in front of it. Substringing there yields an
					// empty key, which the cache writer used to persist and the
					// loader rejects outright - costing a full index rebuild on
					// every launch. Such a name identifies nothing, so skip it.
					if (numbered && suffixAt > 0) {
						activeTextureSet = setKey.substr(0, suffixAt);
						activeVariant = 0;
						for (std::size_t d = digitsAt; d < setKey.size(); ++d) {
							// Bounded like the digit walk above: a variant number is a
							// small ordinal, never a wild value.
							if (activeVariant > 100000) {
								break;
							}
							activeVariant = activeVariant * 10 + (setKey[d] - '0');
						}
						variants.try_emplace(activeTextureSet);
					}
				} else if (terrainLayer &&
					setKey.find("_material_") == std::string::npos &&
					setKey.find("_uvstream_") == std::string::npos &&
					setKey.find(kSuffix) == std::string::npos &&
					setKey.size() >= 3) {
					// A standalone terrain layer names its own material's art.
					activeTextureSet = setKey;
					activeVariant = 1;
					variants.try_emplace(activeTextureSet);
				}
			}

			pos += 8 + len;
		}
		commitPendingSet();

		// Reduce each stem to one whole donor, then merge it into the shared
		// table under the established cross-blob precedence: this blob owns the
		// keys it is first to fill and a later database can only fill gaps (the
		// base game's answer must not be shadowed by a mod that redefines the
		// same material). A stem whose group carried no maps at all still gets
		// its entry - an empty set is authoritative "flat shaded" and dropping
		// it would hand those materials to the fuzzy scorer.
		for (auto& [stem, stemVariants] : variants) {
			const CdbTexSet donor = ChooseCdbVariant(stem, stemVariants);
			auto [it, inserted] = g_cdbSets.try_emplace(stem);
			if (inserted) {
				++stats.sets;
			}
			CdbTexSet& set = it->second;
			if (set.color.empty()) {
				set.color = donor.color;
			}
			if (set.opacity.empty()) {
				set.opacity = donor.opacity;
			}
		}
		// Tens of thousands of variant records: release them before the alpha
		// pass walks the same blob again.
		variants.clear();

		// Second pass over the same blob for the per-material override
		// colour and its layer kind. The stream is NAME-FIRST: the CTName
		// record opens an object and its component records follow (0x020f
		// id, 0x022b kind, 0x025a colour) until the next name. Attaching
		// them to the preceding name shifted every value onto the NEXT
		// material. That single off-by-one produced all six items still
		// reported as "looks the same" after 0.8.304:
		//   Velocity_Beverage_Yellow took the violet (0.981, 0, 1.0) instead
		//     of its own (1.0, 0.917, 0);
		//   Tranquilitea_ToGoCup_Base01 took the lid's grey instead of its
		//     cyan (0.139, 0.764, 0.810);
		//   HeartPlus (the Hypergiant Heart body) took the previous chem's
		//     alpha instead of none;
		//   PackagedFood_Cheese and ErdeBrau_Can took the previous material's
		//     alpha, and Erdebrau_Light_Decal the previous tint;
		//   Captain Galaxy's BookB04 cover (Note 02) took the next book's
		//     colour instead of its teal (0.301, 0.484, 0.418).
		// Values seen before a name are kept as a fallback for the rare
		// records that do order components first.
		float pendingRgba[4]{ 1.0f, 1.0f, 1.0f, 1.0f };
		bool havePending = false;
		std::string currentKey;
		bool colorStoredForCurrent = false;
		bool kindStoredForCurrent = false;
		// TextureSetKind for the same object, read in this pass so the two
		// cannot disagree about which object they describe.
		std::uint8_t pendingKind = 0;

		// Index one override colour under the name spellings FindCdbAlpha
		// asks for (path, .mat, bare stem, materials/... and data/materials/...).
		// emplace keeps the FIRST record seen for a name: the base layer of a
		// layered material, and the base variant of a recoloured one.
		const auto StoreCdbAlpha = [&](const std::string& a_name, const float a_rgba[4]) {
			if (a_name.empty()) {
				return;
			}
			CdbOverride ov{};
			for (std::size_t c = 0; c < 4; ++c) {
				ov.rgba[c] = a_rgba[c];
			}
			g_cdbAlpha.emplace(a_name, ov);
			if (a_name.size() > 4 && a_name.compare(a_name.size() - 4, 4, ".mat") == 0) {
				return;
			}
			g_cdbAlpha.try_emplace(a_name + ".mat", ov);
			if (a_name.find('/') == std::string::npos) {
				g_cdbAlpha.try_emplace("materials/" + a_name + ".mat", ov);
				g_cdbAlpha.try_emplace("data/materials/" + a_name + ".mat", ov);
			}
		};
		const auto StoreCdbKind = [&](const std::string& a_name, std::uint8_t a_kind) {
			if (a_name.empty() || a_kind == 0) {
				return;
			}
			g_cdbKind.try_emplace(a_name, a_kind);
			if (a_name.size() > 4 && a_name.compare(a_name.size() - 4, 4, ".mat") == 0) {
				return;
			}
			g_cdbKind.try_emplace(a_name + ".mat", a_kind);
			if (a_name.find('/') == std::string::npos) {
				g_cdbKind.try_emplace("materials/" + a_name + ".mat", a_kind);
				g_cdbKind.try_emplace("data/materials/" + a_name + ".mat", a_kind);
			}
		};
		pos = 0;
		while (pos + 16 <= a_size) {
			const auto tag = a_base + pos;
			if (tag[0] != 'D' || tag[1] != 'I' || tag[2] != 'F' || tag[3] != 'F') {
				++pos;
				continue;
			}
			std::uint32_t len = 0;
			std::memcpy(&len, tag + 4, 4);
			if (len < 12 || len > 65536 || pos + 8 + len > a_size) {
				pos += 4;
				continue;
			}
			std::uint32_t fieldId = 0;
			std::uint16_t kind = 0, count = 0;
			std::memcpy(&fieldId, tag + 8, 4);
			std::memcpy(&kind, tag + 12, 2);
			std::memcpy(&count, tag + 14, 2);
			const char* const data = tag + 16;
			if (fieldId == 0x025A && kind == 0 && count == 0) {
				// (f32 value, u16 indexOfNext) at stride 6.
				float f[4]{};
				std::uint32_t got = 0;
				// `len` counts from the field id (offset 8), not from the
				// "DIFF" tag, so the in-record bound is 8-based.
				for (std::size_t q = 0; q < 4 && 8 + q * 6 + 6 <= len; ++q) {
					std::memcpy(&f[q], data + q * 6, 4);
					std::uint16_t next = 0;
					std::memcpy(&next, data + q * 6 + 4, 2);
					if (next == 0xffff) {
						++got;
						break;
					}
					++got;
				}
				if (got >= 4) {
					if (!currentKey.empty()) {
						// The name opened this object and the colour follows
						// it. First wins: a layered material carries more
						// than one MaterialOverrideColorTypeComponent (the
						// base plus a permutation layer), and the first is
						// the one the base layer renders with.
						if (!colorStoredForCurrent) {
							StoreCdbAlpha(currentKey, f);
							const auto slash = currentKey.find_last_of('/');
							if (slash != std::string::npos) {
								StoreCdbAlpha(currentKey.substr(slash + 1), f);
							}
							colorStoredForCurrent = true;
							++stats.overrides;
						}
					} else if (!havePending) {
						for (std::size_t c = 0; c < 4; ++c) {
							pendingRgba[c] = f[c];
						}
						havePending = true;
					}
				}
			} else if (fieldId == 0x022B && kind == 0 && count >= 2 &&
				count <= 200 && 8u + count <= len) {
				// TextureSetKindComponent: a plain string. "Multiply" and
				// "Lerp" mean the texture set is a layer over another one.
				const std::string layerKind(data, strnlen(data, count));
				std::uint8_t kindCode = 0;
				if (layerKind == "Multiply") {
					kindCode = 1;
				} else if (layerKind == "Lerp") {
					kindCode = 2;
				} else if (!layerKind.empty()) {
					kindCode = 3;
				}
				if (kindCode != 0) {
					if (!currentKey.empty()) {
						if (!kindStoredForCurrent) {
							StoreCdbKind(currentKey, kindCode);
							const auto slash = currentKey.find_last_of('/');
							if (slash != std::string::npos) {
								StoreCdbKind(currentKey.substr(slash + 1), kindCode);
							}
							kindStoredForCurrent = true;
						}
					} else {
						pendingKind = kindCode;
					}
				}
			} else if (fieldId == 0x01F4 && kind == 0 && count >= 4 &&
				count <= 200 && 8u + count <= len) {
				bool printable = true;
				for (std::size_t q = 0; q < count; ++q) {
					const auto c = data[q];
					if (c == '\0') {
						break;
					}
					printable = printable && c >= 0x20 && c <= 0x7e;
				}
				if (printable) {
					const std::string objName(data, strnlen(data, count));
					const std::string key = CdbAlphaKey(objName);
					// Values seen BEFORE this name belong to an object whose
					// components precede its name (the minority layout). Store
					// them as this object's fallback, then open a new current
					// object for the components that follow it.
					if (!key.empty() && havePending) {
						StoreCdbAlpha(key, pendingRgba);
						const auto slash = key.find_last_of('/');
						if (slash != std::string::npos) {
							StoreCdbAlpha(key.substr(slash + 1), pendingRgba);
						}
						++stats.overrides;
					}
					// A kind without an alpha is still information: the
					// override colour may live in another database for the
					// same object. Both maps must answer under every spelling
					// FindCdbAlpha/FingCdbTint can ask for, or the join fails
					// at runtime while the replicas - comparing bare names -
					// say it worked.
					if (!key.empty() && pendingKind != 0) {
						StoreCdbKind(key, pendingKind);
						const auto slash = key.find_last_of('/');
						if (slash != std::string::npos) {
							StoreCdbKind(key.substr(slash + 1), pendingKind);
						}
					}
					currentKey = key;
					colorStoredForCurrent = false;
					kindStoredForCurrent = false;
					havePending = false;
					pendingKind = 0;
				}
			}
			pos += 8 + len;
		}
		return stats;
	}

	// Sets with no texture path at all (glass, card plastic, card box
	// trim, the whole SupplyContainers family, Isotopic_Coolant_Container)
	// are KEPT, not dropped. An empty set is the game's own statement
	// that the material is flat-shaded from a base colour - dropping it
	// made those materials indistinguishable from "not in the database",
	// so the fuzzy scorer ran and handed back whatever unrelated map it
	// scored highest (a BC4 mask, which is why the crate's metal panels
	// came out black-and-red). FindCdbMaterialLocked reports the hit
	// with an empty colour and FindDiffuse keeps the range neutral.

	// 6r-10: the index survives between sessions, so a warm start reads a
	// few MB instead of parsing 105 MB and re-scanning every mod archive.
	// Set when the loaded cache already carries the mod merge, which is
	// what lets a warm session skip EnsureModCdbIndex entirely.
	bool g_cdbCacheHasMods = false;
	std::string CdbIndexStamp();
	bool LoadCdbIndexCache(const std::string& a_stamp);
	void SaveCdbIndexCacheIfCurrent();


	// -----------------------------------------------------------------------
// 	Material database: the colour and opacity index that decides what a
// 	texture actually contributes
	// -----------------------------------------------------------------------
	void EnsureCdbIndex()
	{
		if (g_cdbIndexed) {
			return;
		}
		g_cdbIndexed = true;
		// Fingerprint archive paths, sizes, and timestamps without opening the
		// archives. Any profile or same-size BA2 replacement invalidates the cache.
		EnsureBa2List();
		const std::string stamp = CdbIndexStamp();
		if (LoadCdbIndexCache(stamp)) {
			return;
		}
		REX::INFO("SFShopExplorer cdb: cache miss; rebuilding index ({} BA2 archives, stamp='{}')",
			g_ba2List.size(), stamp.empty() ? "unavailable" : stamp);
		std::vector<std::uint8_t> blob;
		// ~105MB decompressed: read, index, then release. This runs on the
		// task thread (first material lookup), never on the Present thread.
		if (!ResolveAsset("materials/materialsbeta.cdb", blob) || blob.size() < 4096) {
			REX::WARN("SFShopExplorer cdb: materialsbeta.cdb unavailable ({} bytes)", blob.size());
			return;
		}
		const auto stats = IndexCdbBlob(reinterpret_cast<const char*>(blob.data()), blob.size());
		std::size_t textureless = 0;
		for (const auto& [k, v] : g_cdbSets) {
			(void)k;
			if (v.color.empty()) {
				++textureless;
			}
		}
		REX::INFO("SFShopExplorer cdb: {} material texture sets indexed ({} flat/textureless)",
			g_cdbSets.size(), textureless);
		REX::INFO("SFShopExplorer cdb: {} material override alphas decoded", stats.overrides);
		// Persist the base index as a recovery point. A real mod-only material
		// miss will merge Creation Kit databases and replace this cache below.
		SaveCdbIndexCacheIfCurrent();
	}

	// ---- 6r-9: per-mod material databases -------------------------------
	//
	// A Creation Kit mod ships its OWN materialsbeta.cdb, at
	// materials/creations/<mod>/materialsbeta.cdb, and that file is the
	// only statement the mod makes about its own materials: the base-game
	// database has no record of them whatsoever. Verified on
	// "Official CK/kgcdoom - main.ba2": 1,190,891 bytes decompressed,
	// 102 texture sets (49 with a colour map), keyed exactly the way
	// FindCdbMaterialLocked wants - doom_mancubus_plush ->
	// textures/setdressing/kgcdoom/plush/doom_mancubus_plush_color.dds,
	// alongside doom_arachnotron_plush, crucibleblade, cyberthornmantis.
	// 23 of the 384 installed mod archives ship one.
	//
	// Deliberately LAZY. Indexing these eagerly would mean opening every
	// mod archive on the first material lookup of the session, which is
	// the same cold-start cost the mesh-path work went to lengths to
	// avoid. Instead this fires on the first material the base game does
	// NOT know, which for a base-game item essentially never happens and
	// for a mod item is exactly the miss that needs answering. One-shot,
	// on the task thread, under the lock the caller already holds.
	bool g_cdbModsScanned{ false };
	bool g_cdbModScanAttempted{ false };
	// The real population is every installed Creation - each ships its own
	// database at materials/creations/<name>/materialsbeta.cdb - plus any
	// mod that republishes materials. That is ~30 files here, and they do
	// NOT all come from the MO2 mod folders: the official Creations live in
	// the game's own Data and sort last in g_ba2List. The old cap of 32 was
	// spent on mod archives and Cut off exactly those last entries, so
	// SFBGS050's BreechBlock and Shattered Space's Vortex Grenade had no
	// indexed database at all and every one of their materials fell through
	// to the fuzzy scorer - which handed back a sibling atlas of the same
	// weapon (breechblock_receiver_2 wore breechblock_mods_color.dds, mean
	// RGB 69/70/69 against the authored receiver_2_color.dds's 115/99/95).
	// Bound the cost the cap existed to bound - the decompressed bytes - and
	// keep the file count only as a backstop.
	constexpr std::size_t kMaxModCdbs = 256;
	constexpr std::uint64_t kMaxModCdbBytes = 512ull << 20;

	// Archive entry names are already LowerAscii'd (and backslash-folded
	// to '/') by OpenBa2, so both separators and all case forms match.
	bool IsModCdbEntry(const std::string& a_name)
	{
		return a_name.size() > 17 &&
			a_name.compare(a_name.size() - 17, 17, "materialsbeta.cdb") == 0 &&
			a_name.find("/creations/") != std::string::npos;
	}

	void EnsureModCdbIndex()
	{
		// Already merged in the loaded cache - re-scanning would only
		// re-add keys that are already there.
		if (g_cdbModsScanned || g_cdbCacheHasMods || g_cdbModScanAttempted) {
			return;
		}
		g_cdbModScanAttempted = true;
		EnsureBa2List();
		CdbIndexStats totals;
		std::size_t blobs = 0;
		std::uint64_t blobBytes = 0;
		bool capped = false;
		std::vector<std::uint8_t> blob;
		// What got indexed is the only thing that decides whether a Creation
		// weapon wears its authored maps, and a wrong answer here is silent
		// (the fuzzy scorer returns a plausible sibling), so name every
		// database in the log instead of reporting one aggregate count.
		const auto indexBlob = [&](const std::string& a_source, const std::size_t a_size) {
			++blobs;
			blobBytes += a_size;
			const auto stats = IndexCdbBlob(reinterpret_cast<const char*>(blob.data()), a_size);
			totals.sets += stats.sets;
			totals.overrides += stats.overrides;
			REX::INFO("SFShopExplorer cdb: indexed '{}' ({} new texture sets, {} override colours, {} bytes)",
				a_source, stats.sets, stats.overrides, a_size);
		};
		const auto capReached = [&]() {
			return blobs >= kMaxModCdbs || blobBytes >= kMaxModCdbBytes;
		};

		// The scan is capped, so source order matters. MO2 directory
		// enumeration is not stable between launches; sort the candidate set
		// first so precisely the same databases enter the cache each time.
		std::vector<std::filesystem::path> roots = g_extraDirs;
		if (!g_dataDir.empty()) {
			roots.push_back(g_dataDir);
		}
		std::vector<std::filesystem::path> looseCdbs;
		std::unordered_set<std::string> seenLoose;
		bool enumerationFailed = false;
		for (const auto& root : roots) {
			if (root.empty()) continue;
			const auto creations = root / "materials/creations";
			std::error_code dirEc;
			if (!std::filesystem::exists(creations, dirEc)) {
				// Most enabled mods don't ship Creation Kit material databases;
				// an absent folder is expected, not a failed/incomplete scan.
				if (dirEc) {
					REX::WARN("SFShopExplorer cdb: cannot inspect loose CDB root '{}' ({})", creations.string(), dirEc.message());
					enumerationFailed = true;
				}
				continue;
			}
			if (!std::filesystem::is_directory(creations, dirEc)) {
				REX::WARN("SFShopExplorer cdb: cannot inspect loose CDB root '{}' ({})",
					creations.string(), dirEc ? dirEc.message() : "not a directory");
				enumerationFailed = true;
				continue;
			}
			std::filesystem::directory_iterator it(creations, dirEc), end;
			if (dirEc) {
				REX::WARN("SFShopExplorer cdb: cannot enumerate loose CDB root '{}' ({})", creations.string(), dirEc.message());
				enumerationFailed = true;
				continue;
			}
			for (; it != end; it.increment(dirEc)) {
				if (dirEc) {
					REX::WARN("SFShopExplorer cdb: loose CDB enumeration stopped under '{}' ({})", creations.string(), dirEc.message());
					enumerationFailed = true;
					break;
				}
				std::error_code entryEc;
				if (!it->is_directory(entryEc)) {
					if (entryEc) {
						REX::WARN("SFShopExplorer cdb: cannot inspect loose CDB entry under '{}' ({})", creations.string(), entryEc.message());
						enumerationFailed = true;
						break;
					}
					continue;
				}
				const auto p = it->path() / "materialsbeta.cdb";
				const bool regular = std::filesystem::is_regular_file(p, entryEc);
				if (entryEc) {
					REX::WARN("SFShopExplorer cdb: cannot inspect loose database '{}' ({})", p.string(), entryEc.message());
					enumerationFailed = true;
					break;
				}
				if (regular && seenLoose.insert(LowerAscii(p.lexically_normal().string())).second) {
					looseCdbs.push_back(p);
				}
			}
			if (dirEc) {
				REX::WARN("SFShopExplorer cdb: loose CDB enumeration stopped under '{}' ({})", creations.string(), dirEc.message());
				enumerationFailed = true;
			}
		}
		if (enumerationFailed) {
			REX::WARN("SFShopExplorer cdb: mod database scan incomplete; not caching a partial merge");
			return;
		}
		std::sort(looseCdbs.begin(), looseCdbs.end(), [](const auto& a, const auto& b) {
			return LowerAscii(a.lexically_normal().string()) < LowerAscii(b.lexically_normal().string());
		});
		bool readFailed = false;
		for (const auto& p : looseCdbs) {
			if (capReached()) { capped = true; break; }
			if (ReadLoose(p, blob)) {
				indexBlob(LowerAscii(p.lexically_normal().string()), blob.size());
			} else {
				REX::WARN("SFShopExplorer cdb: could not read loose mod database '{}'", p.string());
				readFailed = true;
			}
		}

		// (b) Archived copies. As with loose CDBs, order the candidates
		// explicitly rather than inheriting nondeterministic MO2 enumeration.
		std::vector<std::filesystem::path> archives = g_ba2List;
		std::sort(archives.begin(), archives.end(), [](const auto& a, const auto& b) {
			return LowerAscii(a.lexically_normal().string()) < LowerAscii(b.lexically_normal().string());
		});
		for (const auto& ap : archives) {
			auto ar = OpenBa2(ap);
			if (!ar->valid) continue;
			std::vector<std::pair<std::string, Ba2Entry>> candidates;
			for (const auto& [name, ent] : ar->entries) {
				if (IsModCdbEntry(name)) candidates.emplace_back(name, ent);
			}
			std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
			for (const auto& [name, ent] : candidates) {
				if (capReached()) { capped = true; break; }
				if (Ba2Read(ar, ent, blob)) {
					indexBlob(name + " <- " + LowerAscii(ap.lexically_normal().string()), blob.size());
				} else {
					REX::WARN("SFShopExplorer cdb: could not read mod database '{}' from '{}'", name, ap.string());
					readFailed = true;
				}
			}
			if (capped) break;
		}
		if (readFailed) {
			REX::WARN("SFShopExplorer cdb: mod database scan incomplete; keeping successful reads for this session but not caching a partial merge");
			return;
		}
		g_cdbModsScanned = true;
		REX::INFO("SFShopExplorer cdb: {} mod materialsbeta.cdb indexed ({} texture sets, {} override colours, {} bytes{})",
			blobs, totals.sets, totals.overrides, blobBytes, capped ? ", deterministic cap reached" : "");
		if (capped) {
			REX::WARN("SFShopExplorer cdb: mod material database cap of {} files / {} bytes reached; this deterministic subset is what the cache records",
				kMaxModCdbs, kMaxModCdbBytes);
		}
		// Re-stamped here, so a mod change between the base parse and this
		// scan invalidates rather than persists a mixture.
		SaveCdbIndexCacheIfCurrent();
	}

	// ---- persisted CDB index (6r-10) ------------------------------------
	//
	// Building the index means parsing a 105 MB database and, on first
	// miss, opening every mod archive looking for their own. That is a
	// fixed cost paid again in every session, and it lands on the very
	// first hover - the one moment the preview has to feel instant. The
	// result is a pure function of (plugin build, game archives, mod set),
	// so it is worth keeping between sessions.
	//
	// Validation is deliberately cheap: it fingerprints the archive LIST by
	// stat(), never by opening an archive. Opening 494 archives to decide
	// whether a cache is valid would cost everything the cache saves. A mod
	// added, removed or re-downloaded changes the archive count or the total
	// byte count, so the stamp moves and the cache is rebuilt - which is the
	// failure mode we want. The one we must never have is serving a stale
	// index, and that is what the stamp exists to prevent.
	constexpr char kCdbMagic[8] = { 'S', 'F', 'E', 'C', 'D', 'B', '1', '\0' };
	// 6 adds one byte of TextureSetKind per override record (see CdbOverride).
	// 7 covers the Creation-database coverage fix: a cache written under the
	// old 32-blob cap records modsMerged=1 while missing the official
	// Creations entirely, and a warm start would happily serve it forever.
	// 8: texture-map records are identified structurally (a textures/*.dds
	// path in any string field) instead of by the base game's 0x038A, which
	// the Creation databases do not use. A schema bump discards the cached
	// index built by the field-id-only build, which held an empty colour for
	// every Creation material and is what made them render untextured.
	// 9: numbered texture-set variants are reduced to one whole donor instead
	// of being merged field by field, and an albedo is recognised by its role
	// token rather than one hard-coded "_color.dds" spelling. Both change what
	// a cached entry means, so a v8 index must be discarded rather than reused:
	// it holds frankenstein sets (plasticplainsmooth02 = MetalCopperAged01_color
	// + UnitedColoniesLogo_01_opacity) and an empty colour for every
	// "_BaseColor.dds" material, the whole canned-beverage family included.
	// 10: override colours and layer kinds are grouped NAME-FIRST, the order
	// the container actually uses. A v9 cache holds every value one object
	// early - Velocity Yellow's violet, the cup lid's grey, the cheese block's
	// neighbour alpha - so it must be rebuilt rather than reused.
	// 11: a planet's `_terrain` layer is indexed as the material it belongs to,
	// so the desktop globes (Desktop_PlanetGlobe_Planet_Alchiba II and the
	// Cassiopeia variant) resolve their planet colour map. A v10 index has no
	// such keys at all, and a warm start would serve it - the sphere would stay
	// flat grey while the fresh build rendered it textured.
	constexpr std::uint32_t kCdbSchema = 11;


	// -----------------------------------------------------------------------
// 	CDB index cache: fingerprint the sources, reuse the index while they match
	// -----------------------------------------------------------------------
	std::string CdbIndexStamp()
	{
		EnsureBa2List();
		if (!g_ba2ListBuilt || !g_ba2ListComplete) {
			REX::WARN("SFShopExplorer cdb: cache stamp unavailable (archive discovery incomplete: built={} complete={})",
				g_ba2ListBuilt, g_ba2ListComplete);
			return {};
		}
		struct SourceIdentity
		{
			std::string path;
			std::uint64_t size;
			std::uint64_t writeTime;
			std::uint8_t kind;  // 0 = archive, 1 = loose Creation Kit database
		};
		std::vector<SourceIdentity> sources;
		sources.reserve(g_ba2List.size());
		std::uint64_t total = 0;
		for (const auto& ap : g_ba2List) {
			std::error_code statEc;
			const auto size = std::filesystem::file_size(ap, statEc);
			if (statEc) {
				REX::WARN("SFShopExplorer cdb: cannot fingerprint archive size '{}' ({})", ap.string(), statEc.message());
				return {};
			}
			statEc.clear();
			const auto writeTime = std::filesystem::last_write_time(ap, statEc);
			if (statEc) {
				REX::WARN("SFShopExplorer cdb: cannot fingerprint archive timestamp '{}' ({})", ap.string(), statEc.message());
				return {};
			}
			sources.push_back(SourceIdentity{
				LowerAscii(ap.lexically_normal().string()),
				size,
				static_cast<std::uint64_t>(writeTime.time_since_epoch().count()),
				0
			});
			total += size;
		}
		const auto looseSourceSet = [&sources, &total](const std::filesystem::path& a_path) {
			std::error_code statEc;
			const auto size = std::filesystem::file_size(a_path, statEc);
			if (statEc) {
				REX::WARN("SFShopExplorer cdb: cannot fingerprint loose database '{}' ({})", a_path.string(), statEc.message());
				return false;
			}
			statEc.clear();
			const auto writeTime = std::filesystem::last_write_time(a_path, statEc);
			if (statEc) {
				REX::WARN("SFShopExplorer cdb: cannot fingerprint loose database timestamp '{}' ({})", a_path.string(), statEc.message());
				return false;
			}
			sources.push_back(SourceIdentity{ LowerAscii(a_path.lexically_normal().string()), size,
				static_cast<std::uint64_t>(writeTime.time_since_epoch().count()), 1 });
			total += size;
			return true;
		};
		std::size_t looseCount = 0;
		std::uint64_t looseTotal = 0;
		std::vector<std::filesystem::path> roots = g_extraDirs;
		if (!g_dataDir.empty()) {
			roots.push_back(g_dataDir);
		}
		std::unordered_set<std::string> seenRoots;
		bool enumerationFailed = false;
		for (const auto& root : roots) {
			if (root.empty()) {
				continue;
			}
			const auto rootKey = LowerAscii(root.lexically_normal().string());
			if (!seenRoots.insert(rootKey).second) {
				continue;
			}
			const auto creations = root / "materials/creations";
			std::error_code iterEc;
			if (!std::filesystem::exists(creations, iterEc)) {
				if (iterEc) {
					REX::WARN("SFShopExplorer cdb: cannot inspect loose database root '{}' ({})", creations.string(), iterEc.message());
					return {};
				}
				continue;
			}
			std::error_code dirEc;
			const bool isDirectory = std::filesystem::is_directory(creations, dirEc);
			if (dirEc) {
				REX::WARN("SFShopExplorer cdb: cannot inspect loose database root '{}' ({})", creations.string(), dirEc.message());
				return {};
			}
			if (!isDirectory) {
				continue;
			}
			std::filesystem::directory_iterator it(creations, iterEc), end;
			if (iterEc) {
				REX::WARN("SFShopExplorer cdb: cannot enumerate loose database root '{}' ({})", creations.string(), iterEc.message());
				return {};
			}
			for (; it != end; it.increment(iterEc)) {
				if (iterEc) {
					REX::WARN("SFShopExplorer cdb: loose database enumeration failed under '{}' ({})", creations.string(), iterEc.message());
					enumerationFailed = true;
					break;
				}
				if (!it->is_directory(iterEc)) {
					if (iterEc) {
						REX::WARN("SFShopExplorer cdb: cannot inspect loose database entry under '{}' ({})", creations.string(), iterEc.message());
						return {};
					}
					continue;
				}
				const auto cdb = it->path() / "materialsbeta.cdb";
				iterEc.clear();
				const bool regular = std::filesystem::is_regular_file(cdb, iterEc);
				if (iterEc) {
					REX::WARN("SFShopExplorer cdb: cannot inspect loose database '{}' ({})", cdb.string(), iterEc.message());
					return {};
				}
				if (!regular) {
					continue;
				}
				if (!looseSourceSet(cdb)) {
					return {};
				}
				std::error_code countEc;
				const auto size = std::filesystem::file_size(cdb, countEc);
				if (countEc) {
					REX::WARN("SFShopExplorer cdb: cannot total loose database '{}' ({})", cdb.string(), countEc.message());
					return {};
				}
				++looseCount;
				looseTotal += size;
			}
		}
		if (enumerationFailed) {
			REX::WARN("SFShopExplorer cdb: loose database source enumeration incomplete; cache stamp unavailable");
			return {};
		}
		std::sort(sources.begin(), sources.end(), [](const SourceIdentity& a, const SourceIdentity& b) {
			if (a.path != b.path) return a.path < b.path;
			if (a.kind != b.kind) return a.kind < b.kind;
			if (a.size != b.size) return a.size < b.size;
			return a.writeTime < b.writeTime;
		});
		// Cache identity is independent of lookup precedence. Canonicalize a
		// copy so MO2 directory-iteration order cannot cause cold misses.
		std::uint64_t fingerprint = 14695981039346656037ULL;
		const auto mix = [&fingerprint](std::uint64_t value) {
			for (int i = 0; i < 8; ++i) {
				fingerprint ^= static_cast<std::uint8_t>(value & 0xFFu);
				fingerprint *= 1099511628211ULL;
				value >>= 8;
			}
		};
		for (const auto& source : sources) {
			for (const auto ch : source.path) {
				fingerprint ^= static_cast<std::uint8_t>(ch);
				fingerprint *= 1099511628211ULL;
			}
			mix(source.path.size());
			mix(source.size);
			mix(source.writeTime);
			mix(source.kind);
		}
		char buf[1024]{};
		const int n = std::snprintf(buf, sizeof(buf), "v%s;s%u;a%zu;t%llu;l%zu;q%llu;f%016llx;d%s", kVersion, kCdbSchema,
			g_ba2List.size(), static_cast<unsigned long long>(total), looseCount, static_cast<unsigned long long>(looseTotal),
			static_cast<unsigned long long>(fingerprint), g_dataDir.string().c_str());
		if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(buf)) {
			REX::WARN("SFShopExplorer cdb: archive fingerprint stamp exceeded its buffer");
			return {};
		}
		return std::string(buf, static_cast<std::size_t>(n));
	}

	std::filesystem::path CdbCachePath()
	{
		return g_cacheDir / L"cdbindex.bin";
	}

	// A corrupt or hostile file must never be able to over-allocate, so every
	// length read from disk is bounded before it is trusted.
	constexpr std::uint32_t kCdbMaxStr = 4096;

	bool ReadStr(std::istream& a_in, std::string& a_out)
	{
		std::uint32_t n = 0;
		a_in.read(reinterpret_cast<char*>(&n), 4);
		if (!a_in || n > kCdbMaxStr) {
			return false;
		}
		a_out.assign(n, '\0');
		if (n != 0) {
			a_in.read(a_out.data(), n);
		}
		return static_cast<bool>(a_in);
	}

	void WriteStr(std::ostream& a_out, const std::string& a_value)
	{
		const auto n = static_cast<std::uint32_t>(std::min<std::size_t>(a_value.size(), kCdbMaxStr));
		a_out.write(reinterpret_cast<const char*>(&n), 4);
		if (n != 0) {
			a_out.write(a_value.data(), n);
		}
	}

	// Fills g_cdbSets / g_cdbAlpha from disk. Fail-closed: any bad magic,
	// wrong stamp, truncated record or absurd count discards everything
	// already read and returns false, and the caller simply parses the
	// database as before. Never throws - a cache miss must not be an error.
	bool LoadCdbIndexCache(const std::string& a_stamp)
	{
		const auto path = CdbCachePath();
		const auto miss = [&path](const char* a_reason) {
			REX::WARN("SFShopExplorer cdb: cache miss ({}) at '{}'", a_reason, path.string());
			return false;
		};
		if (a_stamp.empty()) {
			REX::WARN("SFShopExplorer cdb: cache bypassed (archive fingerprint unavailable; path='{}')", path.string());
			return false;
		}
		REX::INFO("SFShopExplorer cdb: checking cache '{}' (stamp='{}')", path.string(), a_stamp);
		std::error_code ec;
		if (!std::filesystem::is_regular_file(path, ec)) {
			if (ec) {
				REX::WARN("SFShopExplorer cdb: cache miss (cannot inspect '{}': {})", path.string(), ec.message());
			} else {
				REX::INFO("SFShopExplorer cdb: cache miss (file absent at '{}')", path.string());
			}
			return false;
		}
		std::ifstream f(path, std::ios::binary);
		if (!f) {
			REX::WARN("SFShopExplorer cdb: cache miss (cannot open '{}')", path.string());
			return false;
		}
		try {
			char magic[8]{};
			f.read(magic, 8);
			if (!f || std::memcmp(magic, kCdbMagic, 8) != 0) {
				REX::WARN("SFShopExplorer cdb: cache miss (bad/missing magic at {})", path.string());
				return false;
			}
			std::uint32_t schema = 0, modsMerged = 0;
			std::uint64_t setCount = 0, alphaCount = 0;
			std::string stamp;
			f.read(reinterpret_cast<char*>(&schema), 4);
			f.read(reinterpret_cast<char*>(&modsMerged), 4);
			f.read(reinterpret_cast<char*>(&setCount), 8);
			f.read(reinterpret_cast<char*>(&alphaCount), 8);
			std::uint64_t kindCount = 0;
			f.read(reinterpret_cast<char*>(&kindCount), 8);
			if (!f || schema != kCdbSchema || modsMerged > 1 || !ReadStr(f, stamp)) {
				REX::WARN("SFShopExplorer cdb: cache miss (invalid header/schema) at {}", path.string());
				return false;
			}
			if (stamp != a_stamp) {
				REX::INFO("SFShopExplorer cdb: cache miss (archive fingerprint changed; saved='{}' current='{}')", stamp, a_stamp);
				return false;
			}
			// 42,934 sets and ~11,000 override keys is the whole
			// population; anything past that is a corrupt header.
			if (setCount > 1000000ULL || alphaCount > 1000000ULL || kindCount > 1000000ULL) {
				return miss("absurd record counts");
			}
			std::unordered_map<std::string, CdbTexSet> sets;
			std::unordered_map<std::string, CdbOverride> alphas;
			std::unordered_map<std::string, std::uint8_t> kinds;
			sets.reserve(static_cast<std::size_t>(setCount));
			alphas.reserve(static_cast<std::size_t>(alphaCount));
			for (std::uint64_t i = 0; i < setCount; ++i) {
				std::string key, color, opacity;
				if (!ReadStr(f, key) || !ReadStr(f, color) || !ReadStr(f, opacity)) {
					return miss("truncated or invalid texture-set record");
				}
				if (key.empty()) {
					return miss("empty texture-set key");
				}
				CdbTexSet& set = sets[key];
				set.color = std::move(color);
				set.opacity = std::move(opacity);
			}
			for (std::uint64_t i = 0; i < alphaCount; ++i) {
				std::string key;
				float rgba[4]{};
				if (!ReadStr(f, key)) {
					return miss("truncated or invalid override record");
				}
				f.read(reinterpret_cast<char*>(rgba), sizeof(rgba));
				if (!f || key.empty()) {
					return miss("truncated override value or empty key");
				}
				if (!std::all_of(std::begin(rgba), std::end(rgba), [](float c) { return std::isfinite(c); })) {
					return miss("non-finite override value");
				}
				CdbOverride ov{};
				for (std::size_t c = 0; c < 4; ++c) {
					ov.rgba[c] = rgba[c];
				}
				alphas.emplace(key, ov);
			}
			for (std::uint64_t i = 0; i < kindCount; ++i) {
				std::string key;
				char kind = 0;
				if (!ReadStr(f, key) || !f.read(&kind, 1) || key.empty()) {
					return miss("truncated or invalid texture-set kind record");
				}
				if (kind < 0 || kind > 3) {
					return miss("invalid texture-set kind");
				}
				kinds.emplace(key, static_cast<std::uint8_t>(kind));
			}
			// A cache that survived every check but carries nothing is not
			// worth keeping; treat it as a miss so the parse still runs.
			if (sets.empty() && alphas.empty()) {
				return miss("valid header but no records");
			}
			// A valid cache has exactly the serialized payload; reject trailing
			// bytes so partial/concatenated files can't be accepted as hits.
			if (f.peek() != std::char_traits<char>::eof()) {
				return miss("unexpected trailing bytes");
			}
			g_cdbSets = std::move(sets);
			g_cdbAlpha = std::move(alphas);
			g_cdbKind = std::move(kinds);
			g_cdbCacheHasMods = modsMerged != 0;
			REX::INFO("SFShopExplorer cdb: index loaded from cache ({} texture sets, {} override "
				  "colours, mod databases {})",
				g_cdbSets.size(), g_cdbAlpha.size(), g_cdbCacheHasMods ? "merged" : "not merged");
			if (!g_cdbCacheHasMods) {
				REX::WARN("SFShopExplorer cdb: cached base-only index; mod Creation Kit databases will load on a material miss");
			}

			return true;
		} catch (const std::exception& e) {
			// A cache is an optimisation. Never let it become a failure.
			g_cdbCacheHasMods = false;
			REX::WARN("SFShopExplorer cdb: cache miss (exception reading '{}': {})", path.string(), e.what());
			return false;
		} catch (...) {
			g_cdbCacheHasMods = false;
			REX::WARN("SFShopExplorer cdb: cache miss (unknown exception reading '{}')", path.string());
			return false;
		}
	}

	// Written via a temporary and a rename so a crash or a concurrent second
	// instance cannot leave a half-written index behind.
	void SaveCdbIndexCache(const std::string& a_stamp, bool a_modsMerged)
	{
		if (a_stamp.empty() || g_cacheDir.empty()) {
			return;
		}
		try {
			if (g_cdbSets.empty() && g_cdbAlpha.empty()) {
				REX::WARN("SFShopExplorer cdb: refusing to cache an empty index");
				return;
			}
			std::error_code ec;
			std::filesystem::create_directories(g_cacheDir, ec);
			if (ec) {
				REX::WARN("SFShopExplorer cdb: cannot create cache directory '{}' ({})", g_cacheDir.string(), ec.message());
				return;
			}
			const auto finalPath = CdbCachePath();
			const auto tmpPath = g_cacheDir / L"cdbindex.bin.tmp";
			{
				std::ofstream f(tmpPath, std::ios::binary | std::ios::trunc);
				if (!f) {
					REX::WARN("SFShopExplorer cdb: cannot open cache temp '{}' for writing", tmpPath.string());
					return;
				}
				f.write(kCdbMagic, 8);
				const std::uint32_t schema = kCdbSchema;
				const std::uint32_t mods = a_modsMerged ? 1U : 0U;
				// Counts must match the filtered records written below, or the
				// reader sees a header promising more records than the file holds.
				std::uint64_t setCount = 0, alphaCount = 0, kindCount = 0;
				for (const auto& [key, set] : g_cdbSets) {
					if (!key.empty()) {
						++setCount;
					}
				}
				for (const auto& [key, ov] : g_cdbAlpha) {
					if (!key.empty()) {
						++alphaCount;
					}
				}
				for (const auto& [key, kind] : g_cdbKind) {
					if (!key.empty()) {
						++kindCount;
					}
				}
				f.write(reinterpret_cast<const char*>(&schema), 4);
				f.write(reinterpret_cast<const char*>(&mods), 4);
				f.write(reinterpret_cast<const char*>(&setCount), 8);
				f.write(reinterpret_cast<const char*>(&alphaCount), 8);
				f.write(reinterpret_cast<const char*>(&kindCount), 8);
				WriteStr(f, a_stamp);
				// Empty keys are written as zero-length strings, and the loader
				// treats one as a corrupt file. Skip them here as well so a single
				// odd CDB name can never invalidate the cache again.
				for (const auto& [key, set] : g_cdbSets) {
					if (key.empty()) {
						continue;
					}
					WriteStr(f, key);
					WriteStr(f, set.color);
					WriteStr(f, set.opacity);
				}
				for (const auto& [key, ov] : g_cdbAlpha) {
					if (key.empty()) {
						continue;
					}
					WriteStr(f, key);
					f.write(reinterpret_cast<const char*>(ov.rgba), sizeof(ov.rgba));
				}
				for (const auto& [key, kind] : g_cdbKind) {
					if (key.empty()) {
						continue;
					}
					WriteStr(f, key);
					const char k = static_cast<char>(kind);
					f.write(&k, 1);
				}
				f.flush();
				if (!f) {
					REX::WARN("SFShopExplorer cdb: cache write failed while flushing temp '{}'", tmpPath.string());
					std::filesystem::remove(tmpPath, ec);
					return;
				}
			}
			// Replace atomically on Windows. Removing the old cache before
			// rename left a crash window where neither valid copy existed.
			if (!MoveFileExW(tmpPath.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
				const auto error = GetLastError();
				REX::WARN("SFShopExplorer cdb: cache replace failed '{}' -> '{}' (Win32 {})",
					tmpPath.string(), finalPath.string(), error);
				std::error_code rmEc;
				std::filesystem::remove(tmpPath, rmEc);
				return;
			}
		REX::INFO("SFShopExplorer cdb: index cached at '{}' ({} texture sets, {} override colours, modsMerged={})",
			finalPath.string(), g_cdbSets.size(), g_cdbAlpha.size(), a_modsMerged ? 1 : 0);
		} catch (const std::exception& e) {
			REX::WARN("SFShopExplorer cdb: cache write failed for '{}' ({})", CdbCachePath().string(), e.what());
		} catch (...) {
			REX::WARN("SFShopExplorer cdb: cache write failed for '{}' (unknown exception)", CdbCachePath().string());
		}
	}

	// Stamp is recomputed rather than remembered, so a mod change between
	// the base parse and the mod merge invalidates the half-written cache
	// instead of persisting a mixture.
	void SaveCdbIndexCacheIfCurrent()
	{
		EnsureBa2List();
		const auto stamp = CdbIndexStamp();
		if (stamp.empty()) {
			REX::WARN("SFShopExplorer cdb: cache write skipped (could not build a complete archive fingerprint)");
			return;
		}
		SaveCdbIndexCache(stamp, g_cdbModsScanned);
	}

	// The normalized key both FindCdbMaterialLocked and the colour-family
	// lookup below are written against: "Materials\\A\\B.mat:0" becomes
	// path "materials/a/b.mat" and stem "b".
	void CdbPathParts(const std::string& a_matPath, std::string& a_outPath, std::string& a_outStem)
	{
		a_outPath = LowerAscii(a_matPath);
		for (char& ch : a_outPath) {
			if (ch == static_cast<char>(92)) {
				ch = '/';
			}
		}
		while (!a_outPath.empty() && a_outPath.front() == '/') {
			a_outPath.erase(a_outPath.begin());
		}
		const auto colon = a_outPath.find_last_of(':');
		if (colon != std::string::npos) {
			a_outPath.erase(colon);  // geometry suffix such as ":0"
		}
		a_outStem = a_outPath;
		const auto slash = a_outStem.find_last_of('/');
		a_outStem = slash == std::string::npos ? a_outStem : a_outStem.substr(slash + 1);
		if (a_outStem.size() > 4 && a_outStem.compare(a_outStem.size() - 4, 4, ".mat") == 0) {
			a_outStem.erase(a_outStem.size() - 4);
		}
	}

	// The colour word a material variant is named after, as a suffix of the
	// stem: `Crafted_InHailerYellow` -> base `crafted_inhailer`. The list
	// mirrors FlatBaseColor's table in OfflineModel.cpp, because a material
	// that takes its colour from its name is exactly the material whose
	// sibling holds the art.
	bool ColourWordSplit(const std::string& a_stem, std::string& a_outBase)
	{
		static constexpr const char* kWords[] = {
			"white", "silver", "grey", "gray", "green", "red", "bronze", "gold",
			"black", "blue", "yellow", "orange", "brown", "tan", "beige", "purple",
		};
		for (const char* word : kWords) {
			const std::size_t n = std::strlen(word);
			if (a_stem.size() <= n + 2 || a_stem.compare(a_stem.size() - n, n, word) != 0) {
				continue;
			}
			std::string base = a_stem.substr(0, a_stem.size() - n);
			if (!base.empty() && base.back() == '_') {
				base.pop_back();
			}
			if (base.size() < 3) {
				continue;
			}
			a_outBase = base;
			return true;
		}
		return false;
	}

	// Caller holds g_mutex (FindDiffuse does); the public FindCdbMaterial
	// below wraps this with the lock. g_mutex is a plain std::mutex, so a
	// locking call from inside a locked path would deadlock.
	// Returns true when the database HAS a texture set for this material,
	// even if that set names no colour map: an empty set is authoritative
	// "flat shaded, no albedo", and must stop the caller falling through to
	// the fuzzy scorer. Check a_outColor, not the return value, for "has an
	// albedo". a_outOpacity may still be set on an empty set - the icon and
	// letter sheets are exactly that case.
	bool FindCdbMaterialLocked(const std::string& a_matPath, std::string& a_outColor, std::string* a_outOpacity)
	{
		a_outColor.clear();
		if (a_outOpacity) {
			a_outOpacity->clear();
		}
		// Normalized full path, e.g. a NIF's
		//   Materials\Items\FoodDrink_SingleItems\...\PackagedFood_Tomato.mat
		// becomes "materials/items/.../packagedfood_tomato.mat".
		std::string path;
		std::string stem;
		CdbPathParts(a_matPath, path, stem);
		if (stem.size() < 3) {
			return false;
		}
		EnsureCdbIndex();
		const auto lookup = [](const std::string& a_key) -> const CdbTexSet* {
			const auto it = g_cdbSets.find(a_key);
			return it == g_cdbSets.end() ? nullptr : &it->second;
		};
		// 26,748 of the database's 36,438 mappable materials are keyed by
		// FULL PATH, not by bare stem, and the bare stem is usually absent
		// as a key of its own (26,745 of them). Every packaged food and
		// drink lives in this group - packagedfood_tomato/celery/onion/
		// potato and the whole crafted-consumables and menuitem sets - which
		// is why stem-only lookup left the produce and aid shelves grey.
		// Try the path first, then the stem, then the numbered family base.
		const auto resolve = [&]() -> const CdbTexSet* {
			const CdbTexSet* s = lookup(path);
			if (!s && path.compare(0, 5, "data/") != 0) {
				s = lookup("data/" + path);  // some records carry the Data prefix
			}
			if (!s) {
				s = lookup(stem);
			}
			if (!s) {
				// Numbered instances inherit the family base set, the same way
				// Abstract_Poster2x1_02 falls back to Abstract_Poster2x1.
				std::string base = stem;
				while (!base.empty() && base.back() >= '0' && base.back() <= '9') {
					base.pop_back();
				}
				if (!base.empty() && base.back() == '_') {
					base.pop_back();
				}
				if (base != stem) {
					s = lookup(base);
				}
			}
			if (!s) {
				// A colour-named variant shares its family's atlas. Every chem
				// inhaler NIF names `Crafted_InHailer.mat` and
				// `Crafted_InHailerYellow.mat` for its two case halves, but the
				// database records only the base. Without this step the yellow
				// half had no set, fell through to the fuzzy scorer and was
				// handed `inhailer_liquid_decal_color.dds` - the ampoule's
				// greyscale bubble sheet - so the Hypergiant Heart's case drew
				// white with black pentagons where its panels belong. The tint
				// stays the name's: FindCdbTint only reads a record of the
				// material itself, so the base's colour cannot leak in.
				std::string base;
				if (ColourWordSplit(stem, base)) {
					s = lookup(base);
					if (s) {
						REX::INFO("SFShopExplorer live: cdb colour-family '{}' borrows '{}' -> {}",
							stem, base, s->color);
					}
				}
			}
			return s;
		};
		// Rejected rule, kept here because it is the obvious next idea: give
		// every empty-set part under `setdressing/ammobox01/` the shared neutral
		// paint atlas so its database colour survives the neutral-base gate.
		// It was built and looked at in game, and every crate came back with the
		// same yellow body as `.27 Caliber` - `AmmoBoxLarge_DarkGreen01` is in
		// all 22 per-calibre crate NIFs and its override is (0.750,0.609,0.188),
		// so painting it repaints the whole ammo shelf one colour. The crate
		// variety lives on the per-crate *paint* part
		// (`PaintedMetalshine01_Orange` for .27, `PaintedMetalShine01_Green` for
		// 1.5kV) and on each mesh's stencil UVs, not on this shared body
		// material. Left as the folder fallback until a rule distinguishes the
		// two - the per-range audit line prints the atlas and tint each part uses.
		const CdbTexSet* set = resolve();
		if (!set) {
			// 6r-9: a Creation Kit mod's materials have no base-game
			// record at all, so this is the one miss that a per-mod
			// materialsbeta.cdb can still answer. Pull them in and ask
			// again; the scan is one-shot, so this costs nothing on the
			// second and later unknown material.
			EnsureModCdbIndex();
			set = resolve();
		}
		if (!set) {
			return false;
		}
		// A part can carry an authored-but-EMPTY set while its family base holds
		// the only art. The Miner Utility Outfit is the measured case: its shirt,
		// sleeves and pants all resolve to empty sets and drew as bare geometry,
		// while Outfit_Miner_UtilitySuit itself names the suit's atlas (under
		// Bethesda's own misspelled 'outfit_miner_utililtysuit' folder, which no
		// name-based match can reach). Ask the material's folder before declaring
		// the part textureless - the empty set stays authoritative for the many
		// materials whose family has no art either.
		if (set->color.empty() && set->opacity.empty()) {
			// Ammo crates (6r-10). Every part of a crate NIF either names its own
			// art or has none at all: the folder's `ammobox01` set is the crate
			// ATLAS, and binding it because a part is textureless painted that
			// part with whichever island the atlas happens to hold - measured
			// (106,87,57) on the 3kV LZR body, the raw orange in the screenshot.
			// The game draws those parts as paint: `AmmoBoxLarge_DarkGreen01`
			// reads blue-grey on both .27 CAL and 3kV LZR. So a textureless crate
			// part takes the shelf's shared neutral paint map (`paint01_color`,
			// measured (197,198,198)); its database colour still multiplies in
			// through the paint-tint path, which keeps .27 CAL's cream lid.
			//
			// Scoped to the crate folder on purpose. The same substitution was
			// tried for every empty-set part under `setdressing/ammobox01/`
			// WITH the body's saturated override and repainted the whole ammo
			// shelf .27-yellow; OfflineModel now drops a saturated override on
			// these parts, so the body stays the neutral paint.
			if (path.find("setdressing/ammobox01/") != std::string::npos) {
				a_outColor = "textures/common/paint/paint01_color.dds";
				if (a_outOpacity) {
					a_outOpacity->clear();
				}
				REX::INFO("SFShopExplorer live: crate part '{}' has no authored art; "
					"binding the shared paint map", stem);
				return true;
			}
			const auto fileSlash = path.find_last_of('/');
			if (fileSlash != std::string::npos && fileSlash > 0) {
				const auto dirSlash = path.find_last_of('/', fileSlash - 1);
				const auto dirStart = dirSlash == std::string::npos ? 0 : dirSlash + 1;
				const auto dirEnd = dirSlash == std::string::npos ? fileSlash : fileSlash;
				std::string dir = path.substr(dirStart, dirEnd - dirStart);
				if (dir.size() >= 4) {
					const CdbTexSet* family = lookup(dir);
					if (!family) {
						family = lookup("data/" + dir);
					}
					if (family && !family->color.empty()) {
						a_outColor = family->color;
						if (a_outOpacity) {
							*a_outOpacity = family->opacity;
						}
						REX::INFO("SFShopExplorer live: cdb family set '{}' supplies '{}' (part set is empty)",
							dir, family->color);
						return true;
					}
				}
			}
		}
		a_outColor = set->color;
		if (a_outOpacity) {
			*a_outOpacity = set->opacity;
			// An opacity companion is the same authored set's mask - but the
			// DIFF walk attaches every path that follows a set name, and for a
			// layered item the NEXT object's paths can follow it:
			// PackagedFood_Cheese's set owns cheese_color.dds and inherited
			// DrinkPack_SingleBarrelBourbon's _opacity mask, so the cheese block
			// was drawn through a bourbon wrapper's cutout (the reported "Cheese
			// still looks the same"). Accept a mask only when it shares a token
			// with the colour map or with the set's own name: 377 of the
			// database's 23,045 colour/opacity pairs fail that, and every one of
			// them pairs two different families (a root-cactus mask on an arctic
			// cactus, a snowy-cypress mask on a plain cypress, and so on).
			const auto tokens = [](const std::string& a_text) {
				std::vector<std::string> out;
				std::string cur;
				const auto flush = [&]() {
					if (cur.size() >= 4) {
						out.push_back(cur);
					}
					cur.clear();
				};
				for (const char ch : LowerAscii(a_text)) {
					if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
						cur.push_back(ch);
					} else {
						flush();
					}
				}
				flush();
				return out;
			};
			const auto sharesToken = [&](const std::string& a_left, const std::string& a_right) {
				for (const auto& l : tokens(a_left)) {
					for (const auto& r : tokens(a_right)) {
						if (l == r) {
							return true;
						}
					}
				}
				return false;
			};
			const auto stemOf = [](const std::string& a_path) {
				const auto cut = a_path.find_last_of('/');
				return cut == std::string::npos ? a_path : a_path.substr(cut + 1);
			};
			if (!a_outOpacity->empty() && !set->color.empty() &&
				!sharesToken(stemOf(set->color), stemOf(*a_outOpacity)) &&
				!sharesToken(stem, stemOf(*a_outOpacity))) {
				REX::INFO("SFShopExplorer live: dropped cross-family opacity '{}' for '{}' (colour '{}')",
					*a_outOpacity, stem, stemOf(set->color));
				a_outOpacity->clear();
			}
		}
		return true;
	}

	// Opacity companions of e-ink ammo counters (fix 6r-4b D): those
	// families ship only _opacity glyph maps — the _color variant exists
	// nowhere — so without this pool every digit mesh stayed grey. Scored
	// with the same rules as the color loops, strictly family-scoped.
	std::vector<ColorEntry> g_opacs;
	bool g_opacsBuilt{ false };

	void EnsureOpacityIndex()
	{
		if (g_opacsBuilt) {
			return;
		}
		g_opacsBuilt = true;
		EnsureBa2List();
		const auto indexOpacity = [](const std::string& name) {
			if (name.size() < 13 || name.compare(name.size() - 12, 12, "_opacity.dds") != 0) {
				return;
			}
			ColorEntry e;
			e.key = name;
			const auto slash = name.find_last_of('/');
			e.family = slash == std::string::npos ? std::string{} : name.substr(0, slash);
			e.base = slash == std::string::npos ? name : name.substr(slash + 1);
			e.base.erase(e.base.size() - 12);
			g_opacs.push_back(std::move(e));
		};
		for (const auto& ap : g_ba2List) {
			auto ar = OpenBa2(ap);
			if (!ar->valid) {
				continue;
			}
			for (const auto& [name, ent] : ar->entries) {
				(void)ent;
				indexOpacity(name);
			}
			for (const auto& [name, tf] : ar->texFiles) {
				(void)tf;
				indexOpacity(name);
			}
		}
		// Sorted: the pool is new in this build, so unlike g_colors (legacy
		// archive order) it can be deterministic.
		std::sort(g_opacs.begin(), g_opacs.end(),
			[](const ColorEntry& a, const ColorEntry& b) { return a.key < b.key; });
		REX::INFO("SFShopExplorer live: opacity index ({} _opacity maps)", g_opacs.size());
	}

	// One-shot startup diagnosis: run only on the BrowserTask thread after the
	// archive indexes are warm. It distinguishes a real BC4 read from a known
	// absent key and the Razorback's filename-derived companion, without
	// adding any work to Present or changing texture admission policy.
	void RunTextureReadProbe()
	{
		struct ProbeCase
		{
			const char* label;
			const char* key;
		};
		static constexpr ProbeCase cases[] = {
			{ "known-bc4", "textures/weapons/tombstone/tombstone_decals_opacity.dds" },
			{ "definitely-absent", "textures/sfshopexplorer/diagnostic_missing_opacity_6r12.dds" },
			{ "razorback-derived", "textures/weapons/razorback/razorback_receiver_opacity.dds" },
		};
		for (const auto& probe : cases) {
			TexImage image;
			std::string source;
			const bool ok = ReadTexture(probe.key, image, &source);
			// Self-test only: DEBUG, so a normal log does not carry three
			// "ok=false" lines that look like failures but are the expected
			// absent-key controls.
			REX::DEBUG("SFShopExplorer live: texture read probe label='{}' key='{}' ok={} source='{}' fmt={} {}x{} pitch={} bytes={}",
				probe.label, probe.key, ok, source, image.format, image.width, image.height,
				image.rowPitch, image.bytes.size());
		}
	}

	// Build-116: background archive warm-up, one BA2 per task tick. Each
	// OpenBa2 holds g_mutex only for that one archive's TOC parse, so the
	// render thread interleaves freely between ticks — instead of paying
	// 46-680ms of indexing per cold archive inside ParseNifMeshes during a
	// selection click (the click-to-render freeze). Mesh archives come
	// first (EnsureBa2List partition), which is exactly the ResolveAsset
	// order NIF/.mesh lookups walk, so the browse path warms first. The
	// color/opacity indexes build once at the end, while every archive is
	// still in the g_ba2 cache — the same work the first FindDiffuse/
	// opacity lookup would otherwise run under g_mutex (~6s in the 6r-4z
	// session log), blocking every selection made during it.
	void WarmTick()
	{
		if (g_warmDone) {
			return;
		}
		LoadConfig();  // same unlocked first-call pattern as ParseNifMeshes
		{
			std::scoped_lock lock(g_mutex);
			if (!g_configLoaded) {
				return;  // INI not read yet (or disabled): retry next tick
			}
			EnsureBa2List();
		}
		if (g_warmIdx < g_ba2List.size()) {
			const auto ap = g_ba2List[g_warmIdx++];
			std::scoped_lock lock(g_mutex);
			(void)OpenBa2(ap);  // logs per archive; cached entries no-op later
			if (g_warmIdx == g_ba2List.size()) {
				REX::INFO("SFShopExplorer live: archive warm {} of {} done; building indexes",
					g_warmIdx, g_ba2List.size());
			}
			return;
		}
		{
			std::scoped_lock lock(g_mutex);  // same caller discipline FindDiffuse uses
			EnsureColorIndex();
			EnsureOpacityIndex();
			// materialsbeta.cdb is ~105MB of material database. Building the
			// texture-set index here pays it during startup warm-up; left to
			// the first lookup it would hold g_mutex on the Present thread
			// for a multi-second stall right when the user clicks an item.
			EnsureCdbIndex();
			g_warmDone = true;
			REX::INFO("SFShopExplorer live: archive warm complete ({} archives, {} color, {} opacity maps)",
				g_ba2List.size(), g_colors.size(), g_opacs.size());
		}
		RunTextureReadProbe();  // ReadTexture takes g_mutex; run after releasing warm-up lock.
	}

	// Bethesda typo dirs, canonicalized on both sides of every family
	// comparison (materials say ComatKnife, textures say combatknife).
	void CanonFam(std::string& a_fam)
	{
		for (std::size_t at = 0; (at = a_fam.find("comatknife", at)) != std::string::npos; ++at) {
			a_fam.replace(at, 10, "combatknife");
		}
	}		// Functional kind synonyms: an ammo-counter SCREEN wears the DISPLAY

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
		// Build-76: material lookups are file IO (the .mat JSON via
		// ResolveAsset) - not the live-model build hook. Keep them working
		// at [livemodel] enabled=0 or the prefetch resolves nothing and
		// every preview range renders grey.
		if (!g_configLoaded) {
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
				REX::DEBUG("SFShopExplorer live: mat memo miss '{}'", a_matPath);
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
				REX::DEBUG("SFShopExplorer live: mat lookup miss '{}' (archives={})", a_matPath, g_ba2List.size());
				break;
			}
			const std::string text(bytes.begin(), bytes.end());
			if (text.empty() || text.front() != '{') {
				REX::INFO("SFShopExplorer live: mat is not JSON '{}' ({} bytes, first 0x{:02X})",
					a_matPath, text.size(), text.empty() ? 0u : static_cast<unsigned>(text.front()));
				break;  // JSON .mats only; binary blobs are not parsed
			}
			std::string low = text;
			for (auto& ch : low) {
				ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			}
			// A Summary/Albedo block, when the exporter wrote one (Creation
			// .mats). Its ABSENCE is not fatal: Blender exports describe the
			// set only as BSMaterial::MRTextureFile components, and the scan
			// below reads those. Breaking here on a missing key made that
			// scan unreachable, so every Blender-material item - the Star
			// Wars blasters, the Clone Wars outfits - resolved nothing and
			// drew grey while the engine rendered it textured.
			std::string path;
			if (const auto alb = low.find("\"albedo\""); alb != std::string::npos) {
				const auto brace = low.find('{', alb);
				std::size_t end = std::string::npos;
				if (brace != std::string::npos && brace - alb <= 64) {
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
				// First "file" : "path" inside the Albedo object.
				if (end != std::string::npos) {
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
			}
			if (path.empty()) {
				// Blender exports often omit the Summary block entirely: the
				// only texture statement left is the component list, whose
				// BSMaterial::MRTextureFile objects carry `"FileName":
				// "Data\\Textures\\Weapons\\e5\\color.DDS"`. Without this the
				// whole Star Wars blaster set (31 preview ranges in the
				// 2026-10-23 run) fell through to fuzzy matching under
				// `textures/weapons/swweapons/` and stayed grey.
				std::string suffixAlbedo;  // `_d.DDS`: the SGTexSet diffuse name
				for (std::size_t f = low.find("\"filename\""); f != std::string::npos;
					f = low.find("\"filename\"", f + 10)) {
					std::size_t p = f + 10;
					while (p < low.size() && (low[p] == ' ' || low[p] == '\t' ||
						low[p] == '\r' || low[p] == '\n')) {
						++p;
					}
					if (p >= low.size() || low[p] != ':') {
						continue;
					}
					++p;
					while (p < low.size() && (low[p] == ' ' || low[p] == '\t' ||
						low[p] == '\r' || low[p] == '\n')) {
						++p;
					}
					if (p >= low.size() || low[p] != '"') {
						continue;
					}
					++p;
					std::string raw;
					while (p < low.size() && low[p] != '"') {
						if (low[p] == '\\' && p + 1 < low.size()) {
							raw.push_back(low[p + 1]);
							p += 2;
						} else {
							raw.push_back(low[p]);
							++p;
						}
					}
					if (!raw.empty() && raw.size() > 4 &&
						raw.compare(raw.size() - 4, 4, ".dds") == 0) {
						const auto slash = raw.find_last_of("/\\");
						const std::string base = slash == std::string::npos ? raw : raw.substr(slash + 1);
						if (base.find("color") != std::string::npos ||
							base.find("colour") != std::string::npos ||
							base.find("albedo") != std::string::npos ||
							base.find("diffuse") != std::string::npos) {
							path = raw;
							break;
						}
						// The generated SGTexSet sheets name their albedo `_d.DDS`
						// (with `_n`, `_r`, `_l` siblings). Hold it as a second
						// choice rather than taking it immediately: a material that
						// also ships a colour-word map must keep that one, or a
						// upright glove would wear its normal map.
						if (suffixAlbedo.empty() && base.size() > 6 &&
							base.compare(base.size() - 6, 6, "_d.dds") == 0) {
							suffixAlbedo = raw;
						}
					}
				}
				if (path.empty() && !suffixAlbedo.empty()) {
					path = suffixAlbedo;
				}
			}
			if (path.empty()) {
				REX::INFO("SFShopExplorer live: mat names no colour map '{}'", a_matPath);
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
				REX::INFO("SFShopExplorer live: mat albedo rejected '{}' -> '{}'", a_matPath, flat);
				break;
			}
			best = flat;
		} while (false);
		g_albedoMemo.emplace(memoKey, best);
		if (best.empty()) {
			return false;
		}
		a_outKey = best;
		REX::INFO("SFShopExplorer live: mat albedo '{}' <- '{}'", a_matPath, best);
		return true;
	}

	// Authoritative base-game material -> colour/opacity map, straight from
	// the shipped material database. This is the answer for produce, aid
	// and set-dressing families whose maps live in a sibling directory the
	// fuzzy matcher cannot reach (Tomato -> ProduceBox_ProduceOnly/
	// Tomato_Base_color.dds). Runs after the exact overrides and before any
	// fuzzy guess, so it beats both. Session-memoized via the index.
	// Note the return value follows FindCdbMaterialLocked: true means "the
	// database knows this material", and a_outColor may still be EMPTY when
	// the shipped set names no colour map (a flat-shaded material).
	bool FindCdbMaterial(const std::string& a_matPath, std::string& a_outColor, std::string* a_outOpacity)
	{
		std::scoped_lock lock(g_mutex);
		// Same exemption as FindMatAlbedo/ReadTexture: this is file IO.
		if (!g_configLoaded) {
			a_outColor.clear();
			if (a_outOpacity) {
				a_outOpacity->clear();
			}
			return false;
		}
		return FindCdbMaterialLocked(a_matPath, a_outColor, a_outOpacity);
	}

	// True when this material has NO record of its own and the map a caller
	// just bound came from a colour-word sibling instead - the borrowed-set
	// case above. The preview needs the distinction because an own map is
	// the material's art (draw it at white) while a sibling's map is a
	// stand-in (its shade still modulates, but the name's colour rides on
	// top: `Crafted_InHailerYellow` is gold, not the case atlas's white).
	bool CdbColourFamilyBase(const std::string& a_matPath, std::string& a_outBase)
	{
		a_outBase.clear();
		std::scoped_lock lock(g_mutex);
		if (!g_configLoaded) {
			return false;
		}
		std::string path;
		std::string stem;
		CdbPathParts(a_matPath, path, stem);
		if (stem.size() < 3) {
			return false;
		}
		EnsureCdbIndex();
		// A record of its own means the map belongs to this material.
		if (g_cdbSets.count(path) || (path.compare(0, 5, "data/") != 0 && g_cdbSets.count("data/" + path)) ||
			g_cdbSets.count(stem)) {
			return false;
		}
		std::string base;
		if (!ColourWordSplit(stem, base)) {
			return false;
		}
		if (g_cdbSets.count(base) == 0) {
			return false;
		}
		a_outBase = base;
		return true;
	}

	// MaterialOverrideColorTypeComponent::Color.w, decoded from the CDB's
	// DIFF records. Returns true only when the database stored an alpha
	// below 1.0 for this material, so a caller can fall back to its own
	// approximation without mistaking "no record" for "fully opaque".
	bool FindCdbAlpha(const std::string& a_matPath, float& a_outAlpha, std::uint8_t* a_outLayerKind)
	{
		a_outAlpha = 1.0f;
		if (a_outLayerKind) {
			*a_outLayerKind = 0;
		}
		std::scoped_lock lock(g_mutex);
		if (!g_configLoaded) {
			return false;
		}
		EnsureCdbIndex();
		if (g_cdbAlpha.empty()) {
			return false;
		}
		std::string path = LowerAscii(a_matPath);
		for (char& ch : path) {
			if (ch == static_cast<char>(92)) {
				ch = '/';
			}
		}
		while (!path.empty() && path.front() == '/') {
			path.erase(path.begin());
		}
		const auto colon = path.find_last_of(':');
		if (colon != std::string::npos) {
			path.erase(colon);
		}
		std::string stem = path;
		const auto slash = stem.find_last_of('/');
		stem = slash == std::string::npos ? stem : stem.substr(slash + 1);
		if (stem.size() < 3) {
			return false;
		}
		const auto tryKey = [&](const std::string& a_key, float& a_out) {
			const auto it = g_cdbAlpha.find(a_key);
			if (it == g_cdbAlpha.end()) {
				return false;
			}
			a_out = it->second.rgba[3];
			if (a_outLayerKind) {
				// Kind and override alpha are joined by object name, not by
				// which database they came from: the Razorback receiver's
				// "Multiply" is authored in materialsbeta.cdb while its
				// alpha arrives from a Creation Kit database.
				const auto kind = g_cdbKind.find(a_key);
				if (kind != g_cdbKind.end()) {
					*a_outLayerKind = kind->second;
				}
			}
			return true;
		};
		// Same candidate order as FindCdbMaterialLocked: full path, the
		// Data-prefixed variant, the bare stem (indexed by the scan), then
		// the numbered family base.
		if (tryKey(path, a_outAlpha)) {
			return true;
		}
		if (path.compare(0, 5, "data/") != 0 && tryKey("data/" + path, a_outAlpha)) {
			return true;
		}
		if (tryKey(stem, a_outAlpha)) {
			return true;
		}
		std::string base = stem;
		while (!base.empty() && base.back() >= '0' && base.back() <= '9') {
			base.pop_back();
		}
		if (!base.empty() && base.back() == '_') {
			base.pop_back();
		}
		return base != stem && tryKey(base, a_outAlpha);
	}

	// MaterialOverrideColorTypeComponent::Color.rgb: the material's own tint.
	// Only reports a VISIBLY tinted value (max channel below 0.97), because
	// "white with a rounding error" is not information and would otherwise
	// override FlatBaseColor for 21% of materials with nothing to say. A
	// SATURATED colour is information even when one channel is 1.0:
	// Velocity_Beverage_Yellow is (1.0, 0.917, 0.0) and has no albedo map,
	// so rejecting it left the drink as a translucent white box.
	//
	// This is what the flat, textureless materials were missing: 4,014
	// materials have no albedo map at all but do carry a tinted override, and
	// until now their colour came from a guess at the last token of the
	// material name, or from neutral grey when the name had no colour word.
	// UploadModelTextures resets range.color to white when a real albedo
	// does land, so this only survives on the ranges that stay untextured -
	// which is exactly the set it is for.
	bool FindCdbTint(const std::string& a_matPath, float a_out[3])
	{
		a_out[0] = a_out[1] = a_out[2] = 1.0f;
		std::scoped_lock lock(g_mutex);
		if (!g_configLoaded) {
			return false;
		}
		EnsureCdbIndex();
		if (g_cdbAlpha.empty()) {
			return false;
		}
		std::string path = LowerAscii(a_matPath);
		for (char& ch : path) {
			if (ch == static_cast<char>(92)) {
				ch = '/';
			}
		}
		while (!path.empty() && path.front() == '/') {
			path.erase(path.begin());
		}
		const auto colon = path.find_last_of(':');
		if (colon != std::string::npos) {
			path.erase(colon);
		}
		std::string stem = path;
		const auto slash = stem.find_last_of('/');
		stem = slash == std::string::npos ? stem : stem.substr(slash + 1);
		if (stem.size() < 3) {
			return false;
		}
		const auto tryKey = [&](const std::string& a_key) -> const CdbOverride* {
			const auto it = g_cdbAlpha.find(a_key);
			return it == g_cdbAlpha.end() ? nullptr : &it->second;
		};
		const CdbOverride* ov = tryKey(path);
		if (!ov && path.compare(0, 5, "data/") != 0) {
			ov = tryKey("data/" + path);
		}
		if (!ov) {
			ov = tryKey(stem);
		}
		if (!ov) {
			// Full-path materials can share a CDB Material1 record keyed by
			// its bare family name (for example NotebookBlue01's first layer).
			// Check the .mat-stripped bare stem before numbered-family fallback.
			std::string bareStem = stem;
			if (bareStem.size() > 4 && bareStem.compare(bareStem.size() - 4, 4, ".mat") == 0) {
				bareStem.erase(bareStem.size() - 4);
			}
			ov = tryKey(bareStem);
		}
		if (!ov) {
			std::string base = stem;
			while (!base.empty() && base.back() >= '0' && base.back() <= '9') {
				base.pop_back();
			}
			if (!base.empty() && base.back() == '_') {
				base.pop_back();
			}
			if (base != stem) {
				ov = tryKey(base);
			}
		}
		if (!ov) {
			return false;
		}
		const float mx = std::max({ ov->rgba[0], ov->rgba[1], ov->rgba[2] });
		const float mn = std::min({ ov->rgba[0], ov->rgba[1], ov->rgba[2] });
		// Reject only near-white NEUTRAL values. A spread of 0.20 between the
		// brightest and darkest channel is plainly a colour, not a white with
		// rounding error, and it must survive the 1.0 channel that most
		// saturated hues have (yellow 1.0/0.917/0.0, green 1.0/0.60/0.0).
		const bool nearWhiteNeutral = mx >= 0.97f && mx - mn < 0.20f;
		if (nearWhiteNeutral || mx <= 0.0f) {
			return false;
		}
		for (int c = 0; c < 3; ++c) {
			a_out[c] = ov->rgba[c];
		}
		return true;
	}

	// True when a directory under a /weapons/ root names the material family,
	// exactly or by extension. The game does not always file a weapon's maps in
	// a directory named after its material folder: the Coachman materials live
	// in Materials\Weapons\Coachman\ while their shipped maps sit in
	// textures/weapons/coachmanshotgun/. Demanding the literal
	// /weapons/<family>/ directory rejected those authored answers - color and
	// opacity together, which cost the decal set its authored mask - and sent
	// every node back through a fuzzy archive scan for a map the database had
	// already named. Only directory segments are evidence; the part-name tokens
	// in the file name still are not, and the cross-family traps the exact rule
	// was built for stay rejected because magstorm neither equals nor extends
	// regulator.
	bool WeaponDirNamesFamily(const std::string& a_texture, const std::vector<std::string>& a_familyTokens)
	{
		// A one- or two-character tail is a different weapon, not a variant
		// directory, so require the segment to continue the token by at least
		// this many characters (coachman -> coachmanshotgun).
		constexpr std::size_t kMinFamilyStemExtension = 3;
		constexpr std::string_view kWeaponRoot = "/weapons/";
		for (std::size_t root = a_texture.find(kWeaponRoot); root != std::string::npos;
			root = a_texture.find(kWeaponRoot, root + 1)) {
			for (std::size_t i = root + kWeaponRoot.size(); i < a_texture.size();) {
				const auto slash = a_texture.find('/', i);
				const std::size_t end = slash == std::string::npos ? a_texture.size() : slash;
				if (end != i && a_texture.find('.', i) >= end) {
					// A dot marks the file name, which is not family evidence.
					const std::size_t segmentLength = end - i;
					for (const auto& family : a_familyTokens) {
						if (segmentLength < family.size() ||
							a_texture.compare(i, family.size(), family) != 0) {
							continue;
						}
						if (segmentLength == family.size() ||
							segmentLength >= family.size() + kMinFamilyStemExtension) {
							return true;
						}
					}
				}
				if (slash == std::string::npos) {
					break;
				}
				i = slash + 1;
			}
		}
		return false;
	}

	bool CdbWeaponTextureMatchesFamily(const std::string& a_matPath, const std::string& a_texturePath)
	{
		std::string material = LowerAscii(a_matPath);
		std::replace(material.begin(), material.end(), '\\', '/');
		constexpr std::string_view kMaterialPrefix = "materials/weapons/";
		if (material.compare(0, kMaterialPrefix.size(), kMaterialPrefix) != 0) {
			return true;
		}
		const auto materialEnd = material.find_last_of('/');
		if (materialEnd == std::string::npos || materialEnd < kMaterialPrefix.size()) {
			return true;
		}
		const std::string materialFamily = material.substr(kMaterialPrefix.size(),
			materialEnd - kMaterialPrefix.size());
		const bool awpSharedMaterial = materialFamily == "awp_master_shareable" || materialFamily == "awp_shareable";
		// Part names are not evidence that two weapon materials share a
		// texture family. CDB texture sets are often keyed only by a bare
		// material stem (for example `barrel`), so allowing these tokens made
		// Regulator/barrel accept an unrelated MagStorm barrel map.
		static const std::unordered_set<std::string> kGenericTokens{
			"master", "shared", "shareable", "mods", "mod", "color", "normal", "rough",
			"metal", "ao", "opacity", "mask", "weapons", "textures", "actors", "modela",
			"barrel", "grip", "receiver", "stock", "slide", "lower", "upper", "trigger",
			"bolt", "mag", "magazine", "decal", "decals", "sight", "scope", "optic",
			"muzzle", "compensator", "suppressor", "laser", "light", "foregrip", "handguard",
			"rail", "body", "frame", "handle", "triggerguard", "chamber", "iron", "front", "rear"
		};
		// A path such as materials/weapons/regulator/barrel.mat still has the
		// meaningful `regulator` directory token; the generic leaf no longer
		// makes an unrelated .../magstorm/barrel_color.dds look like a match.
		std::vector<std::string> familyTokens;
		const auto addTokens = [](const std::string& a_path, std::size_t a_begin, std::size_t a_end,
			std::vector<std::string>& a_out) {
			std::string token;
			for (std::size_t i = a_begin; i <= a_end; ++i) {
				const char ch = i == a_end ? '/' : a_path[i];
				if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
					token.push_back(ch);
				} else if (!token.empty()) {
					if (token.size() >= 4 && !kGenericTokens.contains(token)) {
						a_out.push_back(token);
					}
					token.clear();
				}
			}
		};
		addTokens(material, kMaterialPrefix.size(), materialEnd, familyTokens);
		// Require a weapon-family token on the map path, not just any token
		// overlap: generic CDB keys such as "decals" were first-wins and
		// resolved Breach, Razorback, Bridger, and Microgun to Solstice Ember.
		// Keep genuinely shared AWP maps when the CDB path explicitly marks
		// that family as shareable.
		std::string texture = LowerAscii(a_texturePath);
		std::replace(texture.begin(), texture.end(), '\\', '/');
		std::vector<std::string> textureTokens;
		addTokens(texture, 0, texture.size(), textureTokens);
		const bool sharedAwP = texture.find("/awp_shareable/") != std::string::npos ||
			texture.find("/awp_master_shareable/") != std::string::npos;
		if (awpSharedMaterial) {
			// This common-material family intentionally borrows the explicitly
			// shareable AWP maps, but not whichever bare CDB key happened to win
			// first (notably MagStorm's barrel under actors/modela).
			return sharedAwP;
		}
		if (familyTokens.empty()) {
			return true;  // No identifiable weapon family; preserve other CDB results.
		}
		if (sharedAwP) {
			return true;
		}
		const bool textureHasWeaponFamily = texture.find("/weapons/") != std::string::npos;
		if (textureHasWeaponFamily && WeaponDirNamesFamily(texture, familyTokens)) {
			return true;
		}
		for (const auto& family : familyTokens) {
			if (std::find(textureTokens.begin(), textureTokens.end(), family) != textureTokens.end() &&
				(!textureHasWeaponFamily || texture.find("/weapons/" + family + "/") != std::string::npos ||
					texture.find("/weapons/" + family + "_") != std::string::npos)) {
				return true;
			}
		}
		return false;
	}

	// A garment's shared "layer base" art. When a clothes texture set's albedo
	// is a generic library map (textures/common/...) it is the floor the
	// garment composites over, not the item's own surface art: Ryujin R&D's
	// upper body is authored as foam (textileinsulatedfoam01) with the red
	// apron blended on top, and Tomisar's coat as bare silk (textilesilk01).
	// Taking the floor as the albedo painted both garments with the shared
	// library sheet - cream foam and white silk stripes where the game shows a
	// red apron and a dark coat. Such a base is used only when the garment's
	// own family offers nothing better.
	// Clothes only: elsewhere a common map is often the correct whole answer
	// (painted metal on crates), and re-scoring those risks trading a right
	// answer for a family neighbour that merely shares a word.
	bool CdbColorIsGarmentLayerBase(const std::string& a_matPath, const std::string& a_texturePath)
	{
		std::string m = LowerAscii(a_matPath);
		std::replace(m.begin(), m.end(), '\\', '/');
		if (m.find("materials/clothes/") == std::string::npos && m.find("/clothes/") == std::string::npos) {
			return false;
		}
		std::string t = LowerAscii(a_texturePath);
		std::replace(t.begin(), t.end(), '\\', '/');
		return t.rfind("textures/common/", 0) == 0;
	}

	bool FindDiffuse(const std::string& a_matPath, const std::string& a_nodeName, std::string& a_outKey,
		int* a_outScore, std::string* a_outOpacity)
	{
		a_outKey.clear();
		if (a_outScore) {
			*a_outScore = 0;
		}
		if (a_outOpacity) {
			a_outOpacity->clear();
		}
		std::scoped_lock lock(g_mutex);
		// Build-76: fuzzy diffuse resolution is file IO, same exemption
		// as FindMatAlbedo/ReadTexture - see there.
		if (!g_configLoaded) {
			return false;
		}
		// Memoized per material+node pair (session cache).
		const std::string memoKey = LowerAscii(a_matPath) + '\x01' + LowerAscii(a_nodeName);
		{
			const auto it = g_diffuseMemo.find(memoKey);
			if (it != g_diffuseMemo.end()) {
				// Re-emit the mask even on the "flat" verdict - that is
				// the whole reason DiffuseVerdict exists.
				if (a_outOpacity) {
					*a_outOpacity = it->second.opacity;
				}
				if (!it->second.color.empty()) {
					a_outKey = it->second.color;
					if (a_outScore) {
						*a_outScore = -1;  // cached verdict, score logged at first hit
					}
					return true;
				}
				return false;
			}
		}
		std::string normalizedMat = LowerAscii(a_matPath);
		for (char& ch : normalizedMat) {
			if (ch == static_cast<char>(92)) {
				ch = '/';
			}
		}
		while (!normalizedMat.empty() && normalizedMat.front() == '/') {
			normalizedMat.erase(normalizedMat.begin());
		}
		const std::string normalizedNode = LowerAscii(a_nodeName);
		const auto exactTexture = [&](const std::string& a_key, const char* a_reason, const char* a_opacity = nullptr) {
			DiffuseVerdict verdict;
			verdict.color = a_key;
			if (a_opacity != nullptr) {
				verdict.opacity = a_opacity;
				if (a_outOpacity) {
					*a_outOpacity = a_opacity;
				}
			}
			g_diffuseMemo.emplace(memoKey, std::move(verdict));
			a_outKey = a_key;
			if (a_outScore) {
				*a_outScore = 2000;
			}
			REX::INFO("SFShopExplorer live: exact material map '{}' -> {} ({})", a_matPath, a_key, a_reason);
		};
		// These archive-backed exact exceptions have different material and
		// texture directory layouts, so fuzzy family scoring cannot discover
		// them safely. The diffuse and (where present) opacity companions are
		// shipped in the installed game's texture BA2s.
		static constexpr std::string_view kPlasmaCutterPrefix = "materials/weapons/sfbgs003/plasmacutter/";
		if (normalizedMat.compare(0, kPlasmaCutterPrefix.size(), kPlasmaCutterPrefix) == 0) {
			static const std::pair<const char*, const char*> kPlasmaCutterMaps[] = {
				{ "plasmacutter_accelerant.mat", "textures/weapons/sfbgs003/plasmacutter/plasmacutter_accelerant_color.dds" },
				{ "plasmacutter_barrel.mat", "textures/weapons/sfbgs003/plasmacutter/plasmacutter_barrel_color.dds" },
				{ "plasmacutter_base_1.mat", "textures/weapons/sfbgs003/plasmacutter/plasmacutter_base_1_color.dds" },
				{ "plasmacutter_base_2.mat", "textures/weapons/sfbgs003/plasmacutter/plasmacutter_base_2_color.dds" },
				{ "plasmacutter_decals.mat", "textures/weapons/sfbgs003/plasmacutter/plasmacutter_decals_color.dds" },
				{ "plasmacutter_flapslong.mat", "textures/weapons/sfbgs003/plasmacutter/plasmacutter_flapslong_color.dds" },
				{ "plasmacutter_handle.mat", "textures/weapons/sfbgs003/plasmacutter/plasmacutter_handle_color.dds" },
				{ "plasmacutter_largeshieldedgrip.mat", "textures/weapons/sfbgs003/plasmacutter/plasmacutter_largeshieldedgrip_color.dds" },
				{ "plasmacutter_magazine.mat", "textures/weapons/sfbgs003/plasmacutter/plasmacutter_magazine_color.dds" },
				{ "plasmacutter_magazine_large.mat", "textures/weapons/sfbgs003/plasmacutter/plasmacutter_magazinelarge_color.dds" },
				{ "plasmacutter_muzzlfocus.mat", "textures/weapons/sfbgs003/plasmacutter/plasmacutter_muzzlefocus_color.dds" },
				{ "plasmacutter_muzzlspread.mat", "textures/weapons/sfbgs003/plasmacutter/plasmacutter_muzzlespread_color.dds" },
			};
			const auto materialName = normalizedMat.substr(kPlasmaCutterPrefix.size());
			for (const auto& [material, texture] : kPlasmaCutterMaps) {
				if (materialName == material) {
					static const std::string kPlasmaCutterDecalsOpacity =
						"textures/weapons/sfbgs003/plasmacutter/plasmacutter_decals_opacity.dds";
					exactTexture(texture, "Shattered Space Plasma Cutter material map",
						normalizedMat.ends_with("plasmacutter_decals.mat") ? kPlasmaCutterDecalsOpacity.c_str() : nullptr);
					return true;
				}
			}
		}
		// Shattered Space Vortex Grenades share one authored color atlas.
		// OfflineModel loads the name-matched emissive atlas separately as a
		// shader detail; it is not an opacity mask. The NIF material directory
		// says "vortexgrenade", but the texture BA2 spells it "vortextgrenade".
		static constexpr std::string_view kVortexGrenadeMaterialPrefix =
			"materials/weapons/dlc001/vortexgrenade/";
		if (normalizedMat.compare(0, kVortexGrenadeMaterialPrefix.size(), kVortexGrenadeMaterialPrefix) == 0) {
			static const std::string kVortexGrenadeColor =
				"textures/weapons/dlc001/vortextgrenade/lambert1_color.dds";
			exactTexture(kVortexGrenadeColor, "Shattered Space Vortex Grenade shared authored color atlas");
			// The matching emissive atlas is a shading detail, not opacity.
			// Do not return it as a guessed _opacity map: the generic reader
			// will (correctly) reject its BC1 color format as a mask.
			return true;
		}
		if (normalizedMat == "materials/weapons/kgcdoom/crucible/cruciblehilt.mat") {
			static const std::string kCrucibleHilt =
				"textures/weapons/kgcdoom/crucible/crucible_color.dds";
			exactTexture(kCrucibleHilt, "Crucible hilt authored color atlas");
			return true;
		}
		// These blade materials have intentionally empty/textureless CDB sets
		// because the game shaders animate their glow. The independent preview
		// still needs the authored color atlases (and the edge-flow opacity
		// mask), so resolve them above the generic CDB-flat early return.
		if (normalizedMat == "materials/weapons/kgcdoom/crucible/crucible_edge_flow.mat") {
			static const std::string kCrucibleBlade =
				"textures/weapons/kgcdoom/crucible/crucible_blade_color.dds";
			static const std::string kCrucibleBladeOpacity =
				"textures/weapons/kgcdoom/crucible/crucible_blade_energy_opacity.dds";
			exactTexture(kCrucibleBlade, "Crucible blade edge-flow color atlas", kCrucibleBladeOpacity.c_str());
			return true;
		}
		if (normalizedMat == "materials/weapons/kgcdoom/crucible/crucible_test_flicker.mat") {
			static const std::string kCrucibleBlade =
				"textures/weapons/kgcdoom/crucible/crucible_blade_graid_color.dds";
			exactTexture(kCrucibleBlade, "Crucible blade flicker color atlas");
			return true;
		}
		if (normalizedMat == "materials/weapons/breach/decals.mat") {
			static const std::string kBreachDecals = "textures/weapons/breach/decals_color.dds";
			static const std::string kBreachDecalsOpacity = "textures/weapons/breach/decals_opacity.dds";
			exactTexture(kBreachDecals, "Breach decal atlas", kBreachDecalsOpacity.c_str());
			return true;
		}
		// Decals_Grip_R1 is the small embossed medallion on the wooden
		// Razorback grip. Keep the standard atlas for the authored wood and
		// coverage; the Fancy atlas is sampled separately as grayscale detail
		// by the direct preview shader, not used as a pale replacement albedo.
		if (normalizedMat == "materials/weapons/razorback/decals.mat" &&
			normalizedNode.find("decals_grip") != std::string::npos) {
			static const std::string kRazorbackGripDecals = "textures/weapons/razorback/decals_color.dds";
			static const std::string kRazorbackGripOpacity = "textures/weapons/razorback/decals_opacity.dds";
			exactTexture(kRazorbackGripDecals, "Razorback grip wood atlas", kRazorbackGripOpacity.c_str());
			return true;
		}
		if (normalizedMat == "materials/weapons/razorback/decals.mat") {
			static const std::string kRazorbackDecals = "textures/weapons/razorback/decals_color.dds";
			static const std::string kRazorbackDecalsOpacity = "textures/weapons/razorback/decals_opacity.dds";
			exactTexture(kRazorbackDecals, "Razorback standard decal atlas", kRazorbackDecalsOpacity.c_str());
			return true;
		}

		// Regulator decals have an authored CDB set under the basename
		// regulator_decal (rather than the NIF stem "Regulator_Decal").
		// Use the exact shipped color/opacity pair before fuzzy matching so
		// this decal never borrows a different weapon's art.
		if (normalizedMat == "materials/weapons/regulator/regulator_decal.mat") {
			static const std::string kRegulatorDecals = "textures/weapons/regulator/regulator_decals_color.dds";
			static const std::string kRegulatorDecalsOpacity = "textures/weapons/regulator/regulator_decals_opacity.dds";
			exactTexture(kRegulatorDecals, "Regulator decal CDB texture set", kRegulatorDecalsOpacity.c_str());
			return true;
		}
		if (normalizedMat == "materials/items/fooddrink_singleitems/all_packaged_consumables/food_produce/food_produce_celery.mat") {
			static const std::string kCelery =
				"textures/items/fooddrink_singleitems/all_packaged_consumables/producebox_produceonly/celery_base_color.dds";
			exactTexture(kCelery, "celery produce material");
			return true;
		}
		if (normalizedMat == "materials/setdressing/office/plants/plants_matswaps/succulents_sm_plant04_swap01.mat") {
			static const std::string kSucculent =
				"textures/setdressing/office/plants/succulents_sm/succulents_sm04_color.dds";
			exactTexture(kSucculent, "small succulent plant 04 material swap");
			return true;
		}
		if (normalizedMat == "materials/setdressing/office/plants/plants_matswaps/succuluents_lg03_swap01.mat") {
			static const std::string kSucculent =
				"textures/setdressing/office/plants/succulents_lg/succulents_lg03_color.dds";
			exactTexture(kSucculent, "large succulent plant 03 material swap");
			return true;
		}
		if (normalizedMat == "materials/setdressing/office/plants/officedesk_plants/officedeskplant_container.mat" ||
			normalizedMat == "materials/setdressing/office/plants/pottedplant_trim/pottedplant_trim.mat") {
			static const std::string kPot =
				"textures/setdressing/office/plants/pottedplant/pottedplant_trim_color.dds";
			exactTexture(kPot, "potted plant container/trim material");
			return true;
		}
		if (normalizedMat == "materials/setdressing/sfbgs00d/actionfigures/uc_armysoldier.mat") {
			static const std::string kSoldier =
				"textures/setdressing/sfbgs00d/actionfigures/uc_armysoldier/ucarmysoldier_color.dds";
			exactTexture(kSoldier, "UC Army Soldier figure body");
			return true;
		}
		static const std::string kActionFigureMatPrefix =
			"materials/setdressing/sfbgs00d/actionfigurepackaging/freestarrangerfigure_packaging/";
		static const std::string kActionFigurePackArt =
			"textures/setdressing/sfbgs00d/actionfigurepackaging/actionfigurepackagingart_color.dds";
		// SFBGS00D - Textures.ba2 also ships actionfigurepackagingart_opacity.dds.
		// Every printed face (MainColor01/02, BackArt, UCBackground, UCLogo)
		// is a sub-rectangle of that ONE atlas and the sub-rectangles are
		// only legible through the mask: without it the printed panel, the
		// blister window and the backing card all read as the same flat
		// artwork, which is what left the front cover looking wrong while
		// the rest of the box read fine. Pairing it also routes these
		// ranges through the alpha pipeline (prefetcher pairs it) instead of
		// the opaque one.
		static const std::string kActionFigurePackArtOpacity =
			"textures/setdressing/sfbgs00d/actionfigurepackaging/actionfigurepackagingart_opacity.dds";
		if (normalizedMat.compare(0, kActionFigureMatPrefix.size(), kActionFigureMatPrefix) == 0) {
			// All printed package faces, logo sheets, backing art and trim
			// reference the same atlas in the shipped NIF. Transparent
			// blister glass and aluminum are separate materials/families.
			exactTexture(kActionFigurePackArt, "SFBGS00D action figure packaging atlas",
				kActionFigurePackArtOpacity.c_str());
			return true;
		}
		// Numbered abstract poster material. CDB-verified: the exotic playing
		// card NIF authors its back surface with exactly this material
		// ("Materials" + SetDressing/Posters/Abstract_Paintings/Abstract_Poster2x1/
		// Abstract_Poster2x1_02.mat on range :1), and materialsbeta.cdb's base
		// texture set for the family (Abstract_Poster2x1_TextureSet1) names
		// Poster_512x1024_02_color.DDS as its colour map. Applying it here is
		// the game's own albedo for that surface, for posters and cards alike,
		// so it must win before the card-neutral rule below.
		if (normalizedMat == "materials/setdressing/posters/abstract_paintings/abstract_poster2x1/abstract_poster2x1_02.mat") {
			static const std::string kPoster =
				"textures/setdressing/posters/abstractpaintings/512x1024/poster_512x1024_02_color.dds";
			exactTexture(kPoster, "numbered abstract poster material (CDB base texture set)");
			return true;
		}
		// materialsbeta.cdb contains a stray TestExplosionFire colour field
		// immediately before the card-symbol texture-set name. The actual card
		// atlas and cutout mask follow the name (see the two authored paths).
		// This exact NIF material override must precede CDB/fuzzy resolution so
		// the King-of-Hearts face never samples the unrelated effect texture.
		if (normalizedMat == "materials/setdressing/exotic_clutter/exoticplayingcard/exoticplayingcard_symbols.mat") {
			static const std::string kCardSymbols =
				"textures/setdressing/exotic_clutter/exoticplayingcards/exoticplayingcard_symbols_decal_color.dds";
			static const std::string kCardSymbolsOpacity =
				"textures/setdressing/exotic_clutter/exoticplayingcards/exoticplayingcard_symbols_decal_opacity.dds";
			exactTexture(kCardSymbols, "authored playing-card symbols atlas", kCardSymbolsOpacity.c_str());
			return true;
		}
		// The playing-card NIF's poster material and symbol atlas cover the
		// face layers. Its plastic backing resolves through unrelated CDB
		// data, so do not let the fuzzy fallback paint it with atlas fragments.
		// Preserve the poster and symbol exact routes above, and let the
		// colored box trim continue through its real CDB material set below.
		if (normalizedNode.find("exoticplayingcard") != std::string::npos &&
			normalizedMat.find("exoticplayingcard_plastic01.mat") != std::string::npos) {
			g_diffuseMemo.emplace(memoKey, DiffuseVerdict{});
			REX::INFO("SFShopExplorer live: card plastic backing kept neutral mat='{}' node='{}'", a_matPath, a_nodeName);
			return false;
		}
		if (normalizedNode.find("exoticplayingcard") != std::string::npos &&
			normalizedMat.find("exoticplayingcardbox01_trim_color02.mat") != std::string::npos) {
			static const std::string kCardBoxTrim = "textures/common/metal/metalpainted01_color.dds";
			exactTexture(kCardBoxTrim, "playing-card box trim material");
			return true;
		}
		// The note item's NIF is BookLarge02; NativeModel applies the authored
		// LMSW substitution from B01 to B04 before preview. The CDB's numbered
		// B04 set is empty, so use the shared book family cover atlas only on
		// this book node. PatternF and the page material have their own authored
		// texture routes, likewise restricted to BookLarge02's Book01 meshes.
		if (normalizedNode.find("book01") != std::string::npos &&
			normalizedMat == "materials/setdressing/books/genbookb01_bookb04.mat") {
			static const std::string kBookCover = "textures/setdressing/books/genbooka01_color.dds";
			exactTexture(kBookCover, "Captain Galaxy note cover material swap");
			return true;
		}
		if (normalizedNode.find("book01") != std::string::npos &&
			normalizedMat == "materials/setdressing/books/genbookb01_pagesb01.mat") {
			static const std::string kBookPages = "textures/setdressing/books/genbooka01_color.dds";
			exactTexture(kBookPages, "Captain Galaxy note pages material");
			return true;
		}
		// The Tranquilitea to-go cup's print. The family ships exactly one cup
		// atlas (`tranquilitea_logo01_color.dds`: blue paper band, leaf pattern,
		// the 'tranquilitea' wordmark) and the CDB has no set at all for the
		// sleeve, so the family scorer handed back the nearest 3D sign texture -
		// `tranquilitea_sign01_color.dds`, a wall sign - and the cup read as a
		// grey cone carrying a picture of a sign.
		if (normalizedMat == "materials/setdressing/tranquilitea/tranquilitea_togocup/tranquilitea_togocup_sleeve01.mat") {
			static const std::string kToGoCupPrint = "textures/setdressing/tranquilitea/tranquilitea_logo01_color.dds";
			exactTexture(kToGoCupPrint, "Tranquilitea to-go cup sleeve print");
			return true;
		}
		// The Erdebrau can's printed label. The database has a set for the
		// family material (`Erdebrau_Material1` -> the pilsner decal) but none
		// at all for the per-beer decal materials, and the shipped files are
		// named after those materials - so the family scorer never reached
		// them and the Light can previewed as a bare cream cylinder. The file
		// names are the material stems with `_color`: measured in
		// `Starfield - Textures05.ba2`.
		if (normalizedMat == "materials/items/fooddrink_singleitems/erdebrau_beers/erdebrau_light_decal.mat") {
			static const std::string kErdebrauLight =
				"textures/items/fooddrink_singleitems/erdebrau_beers/erdebrau light_decal_color.dds";
			exactTexture(kErdebrauLight, "Erdebrau Light label");
			return true;
		}
		if (normalizedMat == "materials/items/fooddrink_singleitems/erdebrau_beers/erdebrau_pilsner_decal.mat") {
			static const std::string kErdebrauPilsner =
				"textures/items/fooddrink_singleitems/erdebrau_beers/erdebrau pilsner_decal_color.dds";
			exactTexture(kErdebrauPilsner, "Erdebrau Pilsner label");
			return true;
		}
		// The Lager can ships its art in ONE wrap map rather than a label decal:
		// `erdebrau lager_color.dds` is the olive/gold can with the pentagon
		// logo, LAGER and the ERDEBRAU band, and `erdebrau lager_aluminium_color.dds`
		// is the bare aluminium top that the Mesh material covers. Neither
		// material has a CDB record (the Lager NIF's materials are absent from
		// the database, unlike the family `Erdebrau_Material1`), so without
		// these the preview drew a grey cylinder.
		if (normalizedMat == "materials/items/fooddrink_singleitems/erdebrau_beers/erdebrau_lager_decal.mat") {
			static const std::string kErdebrauLager =
				"textures/items/fooddrink_singleitems/erdebrau_beers/erdebrau lager_color.dds";
			exactTexture(kErdebrauLager, "Erdebrau Lager wrap");
			return true;
		}
		if (normalizedMat == "materials/items/fooddrink_singleitems/erdebrau_beers/erdebrau_lager_metal.mat") {
			static const std::string kErdebrauLagerTop =
				"textures/items/fooddrink_singleitems/erdebrau_beers/erdebrau lager_aluminium_color.dds";
			exactTexture(kErdebrauLagerTop, "Erdebrau Lager aluminium top");
			return true;
		}
		if (normalizedNode.find("book01") != std::string::npos &&
			normalizedMat == "materials/setdressing/books/decalletterlineoverlay01_patternf.mat") {
			static const std::string kBookPattern = "textures/effects/gobos/lift02_color.dds";
			exactTexture(kBookPattern, "Captain Galaxy note PatternF material swap");
			return true;
		}
		// Base-game materials are not files on disk: the shipped material
		// database is the game's own material -> map table and the only
		// answer for families the fuzzy scorer cannot reach. Consult it after
		// exact overrides and before fuzzy guessing.
		std::string cdbLayerColor;
		{
			std::string cdbColor;
			std::string cdbOpacity;
			if (FindCdbMaterialLocked(a_matPath, cdbColor, &cdbOpacity)) {
				const bool rejectCrossFamilyColor = !cdbColor.empty() &&
					!CdbWeaponTextureMatchesFamily(a_matPath, cdbColor);
				const bool rejectCrossFamilyOpacity = !cdbOpacity.empty() &&
					!CdbWeaponTextureMatchesFamily(a_matPath, cdbOpacity);
				const bool rejectCrossFamilyMap = rejectCrossFamilyColor || rejectCrossFamilyOpacity;
				if (rejectCrossFamilyMap) {
					REX::INFO("SFShopExplorer live: rejected cross-family CDB weapon map '{}' -> color='{}' opacity='{}'; resolving within material family",
						a_matPath, cdbColor, cdbOpacity);
				} else if (!cdbColor.empty() && CdbColorIsGarmentLayerBase(a_matPath, cdbColor)) {
					// The set names only the layer floor. Hand the pick to the
					// family scorer, and keep the floor for the last resort below.
					// Its opacity companion belongs to the same layer stack (Ryujin
					// _upperbody_02 borrows _upperbody_01's cutout), so it is dropped
					// with the colour: pairing it with the item's own art would blend
					// one mesh's holes over another mesh's UVs.
					cdbLayerColor = cdbColor;
					REX::INFO("SFShopExplorer live: cdb garment layer base '{}' -> {} held back for the family scorer",
						a_matPath, cdbColor);
				} else {
					if (a_outOpacity) {
						*a_outOpacity = cdbOpacity;
					}
					if (!cdbColor.empty()) {
						DiffuseVerdict verdict;
						verdict.color = cdbColor;
						verdict.opacity = cdbOpacity;
						g_diffuseMemo.emplace(memoKey, std::move(verdict));
						a_outKey = cdbColor;
						if (a_outScore) {
							*a_outScore = 1900;
						}
						REX::INFO("SFShopExplorer live: cdb material map '{}' -> {}", a_matPath, cdbColor);
						return true;
					}
					DiffuseVerdict verdict;
					verdict.opacity = cdbOpacity;
					g_diffuseMemo.emplace(memoKey, std::move(verdict));
					REX::INFO("SFShopExplorer live: cdb flat material '{}' (textureless set{})",
						a_matPath, cdbOpacity.empty() ? "" : ", opacity only");
					return false;
				}
			}
		}
		// Keep non-symbol card surfaces neutral when their own CDB set is
		// empty; the symbol atlas and poster materials have explicit routes.
		static const std::string kCardMatDir = "materials/setdressing/exotic_clutter/exoticplayingcard/";
		if ((normalizedMat.compare(0, kCardMatDir.size(), kCardMatDir) == 0 ||
				normalizedNode.find("exoticplayingcard") != std::string::npos) &&
			normalizedMat != "materials/setdressing/exotic_clutter/exoticplayingcard/exoticplayingcard_symbols.mat") {
			g_diffuseMemo.emplace(memoKey, DiffuseVerdict{});
			REX::INFO("SFShopExplorer live: card non-symbol surface kept neutral mat='{}' node='{}'", a_matPath, a_nodeName);
			return false;
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
			} else if (mp.size() >= 4 && mp.compare(mp.size() - 4, 4, ".mat") == 0) {
				// Mod paths without the materials/ prefix (BubBow\bbow_body.mat)
				// still map their dirname: without a family nothing in any
				// archive can ever match and every mesh stays grey.
				const auto slash = mp.find_last_of('/');
				if (slash != std::string::npos) {
					family = "textures/" + mp.substr(0, slash);
				}
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
			const std::string dirRaw = slash == std::string::npos ? family : family.substr(slash + 1);
			const std::string tok = noUS(dirRaw);
			const auto eslash = efam.find_last_of('/');
			const std::string eparent = eslash == std::string::npos ? std::string{} : efam.substr(0, eslash);
			const std::string edirRaw = eslash == std::string::npos ? efam : efam.substr(eslash + 1);
			const std::string edir = noUS(edirRaw);
			// >= : dirnames identical after underscore stripping
			// (modelscrimsonfleet vs models_crimsonfleet) count.
			if (!parent.empty() && eparent == parent && edir.size() >= tok.size() &&
				edir.compare(0, tok.size(), tok) == 0) {
				return true;
			}
			// B1 (fix 6r-4b): same parent, underscore-token subset after
			// dropping generic dir words — awp_master_shareable family finds
			// textures/awp_shareable (master/shared are packaging words, not
			// art words).
			if (!parent.empty() && eparent == parent && dirRaw.size() >= 4 && edirRaw.size() >= 4) {
				const auto toks = [](const std::string& s) {
					std::vector<std::string> out;
					std::size_t i = 0;
					while (i <= s.size()) {
						const auto u = s.find('_', i);
						std::string t = s.substr(i, u == std::string::npos ? u : u - i);
						if (!t.empty()) {
							out.push_back(std::move(t));
						}
						if (u == std::string::npos) {
							break;
						}
						i = u + 1;
					}
					return out;
				};
				static const char* kGen[] = { "master", "shared", "common", "variant" };
				const auto dropGen = [&](std::vector<std::string> v) {
					v.erase(std::remove_if(v.begin(), v.end(), [&](const std::string& t) {
						for (const char* g : kGen) {
							if (t == g) {
								return true;
							}
						}
						return false;
					}), v.end());
					return v;
				};
				const auto subset = [](const std::vector<std::string>& a, const std::vector<std::string>& b) {
					for (const auto& t : a) {
						if (std::find(b.begin(), b.end(), t) == b.end()) {
							return false;
						}
					}
					return true;
				};
				const auto ta = dropGen(toks(dirRaw));
				const auto tb = dropGen(toks(edirRaw));
				if (!ta.empty() && !tb.empty() && (subset(ta, tb) || subset(tb, ta))) {
					return true;
				}
			}
			// B2 (fix 6r-4b): dlc/pack subdir parents (weapons ->
			// weapons/dlc001, weapons -> weapons/sfbgs045) whose dirname is
			// related by whole-path containment (equinox ~ varuunequinox).
			// Segment-prefix + depth<=1 keeps Test_Herb's effects parents out.
			if (!parent.empty() && !eparent.empty() && dirRaw.size() >= 4 && edirRaw.size() >= 4 &&
				(dirRaw.find(edirRaw) != std::string::npos || edirRaw.find(dirRaw) != std::string::npos)) {
				const auto segPrefix = [](const std::string& a, const std::string& b) {
					return a == b ||
						(a.size() > b.size() && a.compare(0, b.size(), b) == 0 && a[b.size()] == '/') ||
						(b.size() > a.size() && b.compare(0, a.size(), a) == 0 && b[a.size()] == '/');
				};
				const auto slashes = [](const std::string& p) {
					return static_cast<int>(std::count(p.begin(), p.end(), '/'));
				};
				const int d1 = slashes(eparent);
				const int d2 = slashes(parent);
				const int dd = d1 > d2 ? d1 - d2 : d2 - d1;
				if (segPrefix(eparent, parent) && dd <= 1) {
					return true;
				}
			}
			return false;
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
		// Distinctive bonus tokens (fix 6r-4b C): the material's last token
		// plus the node's kind-normalized stem tokens. Grendel_ScopeDecals
		// now prefers bullpup_decals_color over bullpup_barrel_color, and
		// MagPowerLabel prefers the display map over the grip map.
		std::vector<std::pair<char, std::string>> bonusToks;
		if (!matToken.empty()) {
			bonusToks.emplace_back('m', matToken);
		}
		{
			const std::string nl = LowerAscii(a_nodeName);
			for (const auto& t : StemTokens(nl)) {
				std::string kn = KindNorm(t);
				if (kn.empty()) {
					continue;
				}
				bool dup = false;
				for (const auto& b : bonusToks) {
					if (b.second == kn) {
						dup = true;
						break;
					}
				}
				if (!dup) {
					bonusToks.emplace_back('n', std::move(kn));
				}
			}
		}
		// Cross-family exact hits are allowed ONLY for shared generics
		// (glass). Everything else exact must be family-scoped: the old
		// rule let "receiver"/"decals" (prefix fragments) match bigbang
		// and industrial textures across families.
		auto crossFamilyExactOk = [](const std::string& a_base) {
			return a_base.find("glass") != std::string::npos;
		};
		// A label is not a body. Several shipped families ship ONLY the
		// printed sheet in their folder - the exotics resource containers
		// have lubricant/polymer/solvent/biosuppressant_decal_color.dds and
		// nothing else, the desk fan has deskfan_decal_color.dds - so the
		// scorer's best candidate for a METAL or PLASTIC body is the label
		// atlas of the same family. Bound as albedo it prints the label all
		// over the canister wall and the fan housing: the 2026-10-23 12:53
		// session log shows `Lubricant_Metal01.mat <- lubricant_decal_color`
		// (score 1004) on Xenon/Caesium/Fluorine/... and
		// `Deskfan_BrightWhitePlastic01.mat <- deskfan_decal_color` (1307).
		// The label has its own mesh and its own material (the material name
		// says Decal/Label); a material that does not name a printed sheet
		// must fail closed (flat paint) instead of wearing one.
		const auto namesDecalSheet = [](const std::string& a_name) {
			static const char* kSheetTokens[] = { "decal", "label", "letter", "symbol",
				"poster", "overlay", "blank", nullptr };
			for (const char* const* t = kSheetTokens; *t != nullptr; ++t) {
				if (a_name.find(*t) != std::string::npos) {
					return true;
				}
			}
			return false;
		};
		const bool matIsPrintedSheet = namesDecalSheet(normalizedMat);
		// One veto for every pool: the scorer, the same-folder generic fallback
		// and the family singleton all read candidates through it.
		const auto candidateAllowed = [&](const std::string& a_base) {
			return matIsPrintedSheet || !namesDecalSheet(a_base);
		};
		auto scoreOne = [&](const std::string& c, bool a_prefixDerived, bool a_strictScope, int a_loop,
			const std::vector<ColorEntry>& a_pool) {
			auto inScope = [&](const std::string& a_efam) {
				return a_strictScope ? familyScoped(a_efam) : familyScopedExt(a_efam);
			};
			for (const auto& e : a_pool) {
				if (!candidateAllowed(e.base)) {
					continue;
				}
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
				// a wrong part that happens to share a word. Extended scope
				// (sibling/dlc packs) scores 100 less so the material's OWN
				// family wins equal-score ties — camo packs and dlc dirs
				// sort earlier by path and would otherwise take every tie.
				if (inScope(e.family)) {
					score += familyScoped(e.family) ? 1000 : 900;
					// Distinctive-token bonus (fix 6r-4b C): the material's
					// own token at full weight, the node's kind-normalized
					// stems at half — decal hits decals, label hits display.
					int award = 0;
					// Suffix match with explicit size guards: the naive
					// size() - other.size() underflows size_t when the other
					// token is longer, and compare() then throws
					// std::out_of_range (build-82 CTD on first FindDiffuse).
					const auto endsWith = [](const std::string& a, const std::string& b) {
						return a.size() >= b.size() &&
							a.compare(a.size() - b.size(), b.size(), b) == 0;
					};
					for (const auto& bt : bonusToks) {
						for (const auto& t : StemTokens(e.base)) {
							const std::string kn = KindNorm(t);
							const bool eq = bt.second == kn;
							const bool suf = bt.second.size() >= 4 && kn.size() >= 4 &&
								(endsWith(bt.second, kn) || endsWith(kn, bt.second));
							if (eq || suf) {
								award = std::max(award, bt.first == 'm' ? 600 : 300);
								break;
							}
						}
					}
					score += award;
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
			scoreOne(c, false, false, 1, g_colors);
		}
		for (const auto& c : prefixCands) {
			scoreOne(c, true, false, 2, g_colors);
		}
		// Dir-dirname candidates run ONLY when nothing specific matched:
		// they are the generic last resort (DarkGreen01 -> ammobox01), and
		// must never outbid a real token hit (Eon_Decals wore the family
		// generic instead of the decals map). STRICTLY scoped (own family +
		// subdirs): a sibling dirname would otherwise hijack the generic
		// (books -> booksailing instead of the genbooka01 fallback).
		if (best.empty()) {
			for (const auto& c : dirCands) {
				scoreOne(c, false, true, 3, g_colors);
			}
		}
		// Opacity fallback (fix 6r-4b D): e-ink counter families ship only
		// _opacity glyph maps. Scored strictly family-scoped but as loop 1
		// so the flat-kind gate still judges the pick on its merits.
		if (best.empty()) {
			EnsureOpacityIndex();
			for (const auto& c : cands) {
				scoreOne(c, false, true, 1, g_opacs);
			}
		}
		// Same-folder generic fallback (BookLarge02): GenBookB01 has no
		// B01 _color map — the family's only color is genbooka01. When
		// nothing matched, take the shortest family-scoped map that shares
		// a 5-letter prefix with any candidate instead of rendering grey. A
		// 3-letter prefix let AKWoodVeneerWorn01 take akwrightsshopsign.
		if (best.empty() && !family.empty()) {
			for (const auto& e : g_colors) {
				if (!familyScopedExt(e.family) || !candidateAllowed(e.base)) {
					continue;
				}
				bool shares = false;
				for (const auto& c : cands) {
					if (c.size() >= 5 && e.base.size() >= 5 &&
						c.compare(0, 5, e.base, 0, 5) == 0) {
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
			if (!familyScoped(e.family) || !candidateAllowed(e.base)) {
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
		// The shared layer base is the last resort: the family scorer, the
		// dir-dirname pass and the family singleton all run first, so an item
		// with its own art (Ryujin's apron, Tomisar's coat) never wears the
		// library sheet - while a material that really is painted with a
		// common map still gets it instead of staying grey.
		if (best.empty() && !cdbLayerColor.empty()) {
			best = cdbLayerColor;
			bestBaseLen = best.size();
			bestScore = 1900;
			bestLoop = 1;  // a database answer, trusted like the direct hit
			REX::INFO("SFShopExplorer live: cdb garment layer base '{}' -> {} (no family art)", a_matPath, best);
		}
		{
			DiffuseVerdict verdict;
			verdict.color = best;
			g_diffuseMemo.emplace(memoKey, std::move(verdict));
		}
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
				// Lens glass on optics/readouts wears the sight/display map
				// (fix 6r-4b E): this material IS a lens surface — refusing
				// the scope texture to fail closed left every reflex sight
				// grey. Context words come from the material path only.
				const bool glassOptic = ml.find("glass") != std::string::npos &&
					(ml.find("sight") != std::string::npos || ml.find("scope") != std::string::npos ||
						ml.find("optic") != std::string::npos || ml.find("reflex") != std::string::npos ||
						ml.find("lens") != std::string::npos || ml.find("display") != std::string::npos ||
						ml.find("screen") != std::string::npos || ml.find("counter") != std::string::npos ||
						ml.find("meter") != std::string::npos);
				if (glassOptic) {
					// fall through: keep the pick
				} else {
				std::string bb = LowerAscii(best);
				const auto slash = bb.find_last_of('/');
				std::string bfam = slash == std::string::npos ? std::string{} : bb.substr(0, slash);
				bb = slash == std::string::npos ? bb : bb.substr(slash + 1);
				// Token comparison wants the name without its albedo annotation,
				// whatever spelling the database used for it.
				if (CdbKeyIsAlbedo(bb)) {
					bb = AlbedoBaseName(bb);
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
				// need a long shared token or art on both sides. Strict also
				// keeps a long shared token now (fix 6r-4b F): "label" must
				// accept mag_pulse_display instead of failing the gate.
				bool keep = false;
				if (strict) {
					keep = hasTok || bestArt || tokMax >= 8;
				} else {
					keep = (tokMax >= 8) || (matArt && bestArt);
				}
				if (bestLoop >= 3 || !keep) {
					REX::INFO("SFShopExplorer live: '{}' stays flat (no strong token hit)", a_matPath);
					g_diffuseMemo[memoKey].color.clear();
					return false;
				}
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
		REX::DEBUG("SFShopExplorer live: discovered NIF '{}' <- '{}'", best, a_edid);
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
			// Bounded copy into the zeroed 64-byte field: 63 chars + NUL.
			const std::size_t nameLen = m.name.size() < 63 ? m.name.size() : 63;
			std::memcpy(nm, m.name.data(), nameLen);
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
