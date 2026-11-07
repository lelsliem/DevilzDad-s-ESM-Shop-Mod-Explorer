// Round-trip harness for the persisted CDB index (6r-10).
//
// The block under test is EXTRACTED verbatim from src/LiveModel.cpp by
// run_cdb_cache_harness.sh (between the "persisted CDB index (6r-10)"
// marker and the end of SaveCdbIndexCacheIfCurrent). Nothing is copied, so
// the harness cannot drift from the shipped code. Everything the block
// touches is stubbed below with the same declarations LiveModel.cpp has.
//
// Cases: miss, hit, deep value compare, stale stamp (mod added / older
// plugin build), stamp sensitivity to the archive list, corrupt magic,
// truncation at seven points, absurd counts, over-long string, empty file,
// zero-record file, save into a missing directory tree, empty record keys.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <Windows.h>

// ---- stubs matching LiveModel.cpp's anonymous namespace ---------------
namespace SFE::LiveModel
{
	namespace
	{
		inline constexpr char kVersion[] = "0.8.122";

		struct CdbTexSet
		{
			std::string color;
			std::string opacity;
		};
		struct CdbOverride
		{
			float rgba[4]{};
		};

		std::unordered_map<std::string, CdbTexSet> g_cdbSets;
		std::unordered_map<std::string, CdbOverride> g_cdbAlpha;
		std::unordered_map<std::string, std::uint8_t> g_cdbKind;
		std::vector<std::filesystem::path> g_ba2List;
		std::vector<std::filesystem::path> g_extraDirs;
		std::vector<std::filesystem::path> g_moDirs;
		std::filesystem::path g_dataDir = "C:\\fake\\Data";
		std::filesystem::path g_cacheDir;
		bool g_ba2ListBuilt = true;
		bool g_ba2ListComplete = true;
		bool g_cdbModsScanned = false;
		bool g_cdbCacheHasMods = false;

		std::string LowerAscii(std::string_view a_text)
		{
			std::string out;
			out.reserve(a_text.size());
			for (const unsigned char c : a_text) {
				const char lower = static_cast<char>(std::tolower(c));
				out.push_back(lower == '\\' ? '/' : lower);
			}
			return out;
		}

		void EnsureBa2List()
		{
			// Real one is a one-shot; the harness treats it as idempotent.
		}

		// fmt-style: the real REX::INFO takes a format string plus args.
		struct Log {
			template <typename... Ts>
			static void INFO(const char*, Ts&&...)
			{
			}
			template <typename... Ts>
			static void WARN(const char*, Ts&&...)
			{
			}
		};
	}  // namespace

	// The test driver needs the same types, and they live in an anonymous
	// namespace, so everything below is part of that namespace too.
#define REX ::SFE::LiveModel::Log
#include "cdb_cache_block.inc"
}  // namespace SFE::LiveModel

// ---- test driver ------------------------------------------------------
namespace
{
	// The stubs are in SFE::LiveModel's anonymous namespace; this makes them
	// visible here by unqualified name.
	using namespace SFE::LiveModel;

	int g_fail = 0;

	void check(bool a_ok, const std::string& a_what)
	{
		std::cout << (a_ok ? "  ok   " : "  FAIL ") << a_what << "\n";
		if (!a_ok) {
			++g_fail;
		}
	}

	// Deterministic pseudo-random so the population is not accidentally
	// uniform - the bug class here is exactly "looks fine on a small set".
	struct Rng {
		std::uint64_t s = 0x9E3779B97F4A7C15ULL;
		std::uint32_t next()
		{
			s ^= s << 13;
			s ^= s >> 7;
			s ^= s << 17;
			return static_cast<std::uint32_t>(s >> 32);
		}
		std::string token()
		{
			static const char* kAlpha = "abcdefghijklmnopqrstuvwxyz0123456789_";
			std::string t;
			for (int i = 0; i < 6; ++i) {
				t += kAlpha[next() % 37];
			}
			return t;
		}
	};

	void Populate(std::size_t a_sets, std::size_t a_alphas, Rng& a_rng)
	{
		SFE::LiveModel::g_cdbSets.clear();
		SFE::LiveModel::g_cdbAlpha.clear();
		for (std::size_t i = 0; i < a_sets; ++i) {
			CdbTexSet set;
			// 1 in 8 textureless, 1 in 4 with a mask: both real shapes.
			if (a_rng.next() % 8 != 0) {
				set.color = "textures/x/" + a_rng.token() + "_color.dds";
			}
			if (a_rng.next() % 4 == 0) {
				set.opacity = "textures/x/" + a_rng.token() + "_opacity.dds";
			}
			SFE::LiveModel::g_cdbSets["materials/" + a_rng.token() + "/" + a_rng.token() +
				"_texture" + std::to_string(i) + ".mat"] = set;
		}
		for (std::size_t i = 0; i < a_alphas; ++i) {
			CdbOverride ov{};
			for (float& c : ov.rgba) {
				c = static_cast<float>(a_rng.next() % 1000) / 1000.0f;
			}
			SFE::LiveModel::g_cdbAlpha["mat/" + a_rng.token() + "/" + a_rng.token() + "_" +
				std::to_string(i)] = ov;
		}
	}

	template <typename M>
	std::size_t CountFlatMask(const M& a_m)
	{
		std::size_t n = 0;
		for (const auto& [k, v] : a_m) {
			(void)k;
			if (v.color.empty() && !v.opacity.empty()) {
				++n;
			}
		}
		return n;
	}
}  // namespace

int main()
{
	using namespace SFE::LiveModel;
	namespace fs = std::filesystem;
	using map_t = std::unordered_map<std::string, CdbTexSet>;

	std::cout << "cdb cache harness\n";
	{
		std::error_code ec;
		fs::remove_all("C:/fake", ec);
		fs::create_directories("C:/fake/assets", ec);
		g_cacheDir = "C:/fake/assets";
	}

	Rng rng;
	Populate(42934, 10673, rng);  // the measured real population
	const std::size_t expectSets = g_cdbSets.size();
	const std::size_t expectAlphas = g_cdbAlpha.size();
	const std::size_t expectFlatMask = CountFlatMask(g_cdbSets);
	std::cout << "populated " << expectSets << " sets / " << expectAlphas << " overrides ("
			  << expectFlatMask << " flat-with-mask)\n";

	const std::string stamp = "v0.8.122;s3;a494;t123456;dC:\\fake\\Data";

	// --- 1. miss -----------------------------------------------------
	check(!LoadCdbIndexCache(stamp), "miss on absent file returns false");

	// --- 2. save then hit --------------------------------------------
	SaveCdbIndexCache(stamp, true);
	check(fs::exists("C:/fake/assets/cdbindex.bin"), "cache file written");
	check(!fs::exists("C:/fake/assets/cdbindex.bin.tmp"), "no .tmp left behind");
	const map_t savedSets = g_cdbSets;
	const auto savedAlphas = g_cdbAlpha;
	g_cdbSets.clear();
	g_cdbAlpha.clear();
	g_cdbModsScanned = false;
	g_cdbCacheHasMods = false;
	check(LoadCdbIndexCache(stamp), "hit on matching stamp");
	check(g_cdbSets.size() == expectSets, "set count round-trips (" +
		std::to_string(g_cdbSets.size()) + "/" + std::to_string(expectSets) + ")");
	check(g_cdbAlpha.size() == expectAlphas, "override count round-trips (" +
		std::to_string(g_cdbAlpha.size()) + "/" + std::to_string(expectAlphas) + ")");
	check(CountFlatMask(g_cdbSets) == expectFlatMask, "flat-with-mask sets survive");
	check(g_cdbCacheHasMods, "merged flag survives the round-trip (warm session skips mod scan)");
	{
		bool same = g_cdbSets.size() == savedSets.size();
		for (const auto& [k, v] : savedSets) {
			const auto it = g_cdbSets.find(k);
			if (it == g_cdbSets.end() || it->second.color != v.color ||
				it->second.opacity != v.opacity) {
				same = false;
				break;
			}
		}
		check(same, "every colour/opacity string identical after reload");
	}
	{
		bool same = g_cdbAlpha.size() == savedAlphas.size();
		for (const auto& [k, v] : savedAlphas) {
			const auto it = g_cdbAlpha.find(k);
			if (it == g_cdbAlpha.end() ||
				std::memcmp(it->second.rgba, v.rgba, sizeof(v.rgba)) != 0) {
				same = false;
				break;
			}
		}
		check(same, "every override rgba identical after reload");
	}

	// --- 3. stale stamp ----------------------------------------------
	check(!LoadCdbIndexCache("v0.8.122;s1;a495;t124456;dC:\\fake\\Data"),
		"archive count change (mod added) is a miss");
	check(!LoadCdbIndexCache("v0.8.121;s1;a494;t123456;dC:\\fake\\Data"),
		"older plugin version is a miss");
	check(!LoadCdbIndexCache(""), "empty stamp is a miss");

	// The stamp really depends on the archive list, not on fed constants.
	{
		std::error_code ec;
		const fs::path a("C:/fake/a.ba2");
		const fs::path b("C:/fake/b.ba2");
		{
			std::ofstream o(a, std::ios::binary);
			o << "aaaa";
		}
		{
			std::ofstream o(b, std::ios::binary);
			o << "bbbbbbbb";
		}
		g_ba2List = { a };
		const std::string s1 = CdbIndexStamp();
		g_ba2List = { a, b };
		const std::string s2 = CdbIndexStamp();
		{
			std::ofstream o(a, std::ios::binary | std::ios::trunc);
			o << "aa";
		}
		const std::string s3 = CdbIndexStamp();
		const std::string s4 = CdbIndexStamp();
		check(s1 != s2, "adding an archive changes the stamp");
		check(s3 != s2, "changing an archive's size changes the stamp");
		check(s4 == s3, "unchanged files give the same stamp");
		g_ba2ListComplete = false;
		check(CdbIndexStamp().empty(), "incomplete archive discovery disables cache stamp");
		g_ba2ListComplete = true;
		check(s1.find("v0.8.122") == 0, "stamp carries the plugin version");
		check(s1.find(";a1;") != std::string::npos, "stamp carries the archive count");
		check(s1.find(";t4;") != std::string::npos, "stamp carries the total archive size");
	}

	// --- 4-9. structural damage --------------------------------------
	{
		const fs::path p = SFE::LiveModel::CdbCachePath();
		std::vector<char> bytes;
		{
			std::ifstream in(p, std::ios::binary);
			bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		}
		const auto writeBytes = [&](const std::vector<char>& a_b) {
			std::ofstream o(p, std::ios::binary | std::ios::trunc);
			o.write(a_b.data(), static_cast<std::streamsize>(a_b.size()));
		};
		const auto resetGlobals = [&]() {
			g_cdbSets.clear();
			g_cdbAlpha.clear();
			g_cdbCacheHasMods = false;
		};

		{
			std::vector<char> bad = bytes;
			bad[0] = 'X';
			writeBytes(bad);
			resetGlobals();
			check(!LoadCdbIndexCache(stamp), "corrupt magic is a miss");
			check(g_cdbSets.empty(), "corrupt magic leaves globals untouched");
		}
		{
			std::vector<char> bad = bytes;
			bad[1] = 0;
			writeBytes(bad);
			resetGlobals();
			check(!LoadCdbIndexCache(stamp), "corrupt magic byte 2 is a miss");
		}
		{
			// Truncation. Accepting a short file is only safe if it is
			// identical to the full one, which it cannot be.
			bool ok = true;
			for (double frac : { 0.02, 0.10, 0.25, 0.50, 0.75, 0.90, 0.999 }) {
				std::vector<char> part(bytes.begin(),
					bytes.begin() + static_cast<std::ptrdiff_t>(bytes.size() * frac));
				writeBytes(part);
				resetGlobals();
				const bool loaded = LoadCdbIndexCache(stamp);
				if (loaded && g_cdbSets.size() != expectSets) {
					ok = false;
				}
				if (!g_cdbSets.empty() && g_cdbSets.size() != expectSets) {
					ok = false;
				}
			}
			check(ok, "truncation at 7 points never yields a partial index");
		}
		{
			std::vector<char> hdr = bytes;
			// magic(8) schema(4) mods(4) setCount(8) alphaCount(8) kindCount(8)
			const std::uint64_t huge = 0xFFFFFFFFFFFFULL;
			std::memcpy(hdr.data() + 16, &huge, 8);
			writeBytes(hdr);
			resetGlobals();
			check(!LoadCdbIndexCache(stamp), "absurd setCount is a miss");
			check(g_cdbSets.empty(), "absurd setCount allocates nothing");
		}
		{
			std::vector<char> hdr = bytes;
			const std::uint64_t huge = 0xFFFFFFFFFFFFULL;
			std::memcpy(hdr.data() + 24, &huge, 8);
			writeBytes(hdr);
			resetGlobals();
			check(!LoadCdbIndexCache(stamp), "absurd alphaCount is a miss");
		}
		{
			std::vector<char> hdr = bytes;
			const std::uint64_t huge = 0xFFFFFFFFFFFFULL;
			std::memcpy(hdr.data() + 32, &huge, 8);
			writeBytes(hdr);
			resetGlobals();
			check(!LoadCdbIndexCache(stamp), "absurd kindCount is a miss");
		}
		{
			// Stamp length prefix lives right after the 32-byte header.
			std::vector<char> hdr = bytes;
			const std::uint32_t tooLong = 100000;
			std::memcpy(hdr.data() + 40, &tooLong, 4);
			writeBytes(hdr);
			resetGlobals();
			check(!LoadCdbIndexCache(stamp), "over-long stamp length is a miss");
		}
		{
			std::vector<char> hdr = bytes;
			const std::uint32_t tooLong = 100000;
			std::memcpy(hdr.data() + 44, &tooLong, 4);  // first record's key
			writeBytes(hdr);
			resetGlobals();
			check(!LoadCdbIndexCache(stamp), "over-long key length is a miss");
		}
		{
			writeBytes({});
			resetGlobals();
			check(!LoadCdbIndexCache(stamp), "empty file is a miss");
		}
		{
			std::vector<char> hdr = bytes;
			const std::uint64_t zero = 0;
			std::memcpy(hdr.data() + 16, &zero, 8);
			std::memcpy(hdr.data() + 24, &zero, 8);
			writeBytes(hdr);
			resetGlobals();
			check(!LoadCdbIndexCache(stamp), "zero-record cache is a miss");
		}
		{
			std::vector<char> hdr = bytes;
			std::memset(hdr.data() + 8, 0xFF, 4);  // schema != kCdbSchema
			writeBytes(hdr);
			resetGlobals();
			// Schema is stamped into the string; keep this fixture aligned
			// with kCdbSchema so a bumped cache can never be a stale pass.
			check(CdbIndexStamp().find(";s11;") != std::string::npos,
				"stamp carries the current cache schema version");
		}
	}

	// --- 10. save into a missing directory tree -----------------------
	{
		std::error_code ec;
		fs::remove_all("C:/fake/deep", ec);
		g_cacheDir = "C:/fake/deep/nested/assets";
		Populate(500, 100, rng);
		SaveCdbIndexCache(stamp, true);
		check(fs::exists("C:/fake/deep/nested/assets/cdbindex.bin"),
			"save creates missing directories");
		g_cacheDir = "C:/fake/assets";
	}

	// --- 11. empty cache dir is a no-op -------------------------------
	{
		g_cacheDir.clear();
		SaveCdbIndexCacheIfCurrent();
		g_cacheDir = "C:/fake/assets";
		check(true, "empty cache dir is a no-op, not a crash");
	}
	{
		SaveCdbIndexCache("", true);
		check(true, "empty stamp write is refused, not a crash");
	}

	// --- 12. an empty key must never poison the cache ------------------
	// IndexCdbBlob inserted an empty set key when a CDB object was named
	// exactly "_textureset<n>" (the suffix starts at offset 0). The writer
	// persisted it and LoadCdbIndexCache then rejected the whole 42k-record
	// file with "empty texture-set key" on every launch, so the index was
	// rebuilt from scratch each session. Both shipped cdbindex.bin files
	// were found to carry exactly one such key. The writer must drop them
	// and the file must still round-trip.
	{
		g_cdbSets.clear();
		g_cdbAlpha.clear();
		CdbTexSet real;
		real.color = "textures/a/b_color.dds";
		g_cdbSets["materials/a/b.mat"] = real;
		g_cdbSets[""] = CdbTexSet{};
		CdbOverride ov{};
		g_cdbAlpha["materials/a/b.mat"] = ov;
		g_cdbAlpha[""] = ov;
		SaveCdbIndexCache(stamp, false);
		g_cdbSets.clear();
		g_cdbAlpha.clear();
		check(LoadCdbIndexCache(stamp), "cache holding an empty key still loads");
		check(g_cdbSets.count("materials/a/b.mat") == 1,
			"real set survives next to an empty key");
		check(g_cdbSets.count("") == 0, "empty set key is not persisted");
		check(g_cdbAlpha.count("") == 0, "empty override key is not persisted");
	}

	std::cout << (g_fail == 0 ? "ALL PASS\n" : "FAILURES: " + std::to_string(g_fail) + "\n");
	return g_fail == 0 ? 0 : 1;
}