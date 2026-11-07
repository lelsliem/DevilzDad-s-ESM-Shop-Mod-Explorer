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
		roots.insert(roots.end(), g_moDirs.begin(), g_moDirs.end());
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
