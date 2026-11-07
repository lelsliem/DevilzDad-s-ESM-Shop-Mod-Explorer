#include "PCH.h"
#include "NativeModel.h"

#include "LiveModel.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cctype>
#include <format>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <Windows.h>

#include "RE/B/BSFixedString.h"
#include "RE/N/NiAVObject.h"
#include "RE/N/NiNode.h"
#include "RE/N/NiSmartPointer.h"
#include "RE/N/NiUpdateData.h"
#include "RE/P/PlayerCharacter.h"
#include "RE/T/TESBoundObject.h"
#include "RE/T/TESDataHandler.h"
#include "RE/T/TESForm.h"
#include "RE/T/TESObjectREFR.h"
#include "RE/U/UI.h"

namespace SFE::NativeModel
{
	namespace
	{
		std::uint32_t g_cachedFormID{ 0 };
		RE::NiPointer<RE::NiAVObject> g_cachedObj;

		// Base-game LMSW records 0033D9F0 / 0033DA02 / 0033D9F5
		// describe these exact material-path overrides for the BookLarge02 Note forms.
		std::string NormalizeBookMaterialPath(std::string a_path)
		{
			for (char& ch : a_path) {
				if (ch == '/') {
					ch = '\\';
				}
				ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			}
			if (a_path.compare(0, 10, "materials\\") == 0) {
				a_path.erase(0, 10);
			}
			return a_path;
		}

		void ApplyCaptainNoteMaterialSwaps(std::uint32_t a_formID, std::vector<NativeMesh>& a_meshes)
		{
			struct MaterialSwap
			{
				const char* from;
				const char* to;
			};
			static constexpr MaterialSwap kCaptainNote02Swaps[] = {
				{ "SetDressing\\Books\\GenBookA01_BookA01.mat", "Materials\\SetDressing\\Books\\GenBookA01_BookA04.mat" },
				{ "SetDressing\\Books\\GenBookB01_BookB01.mat", "Materials\\SetDressing\\Books\\GenBookB01_BookB04.mat" },
				{ "SetDressing\\Books\\DecalLetterLineOverlay01_LinesStripes.mat", "Materials\\SetDressing\\Books\\DecalLetterLineOverlay01_PatternF.mat" },
			};
			static constexpr MaterialSwap kCaptainNote01Swaps[] = {
				{ "SetDressing\\Books\\GenBookB01_BookB01.mat", "Materials\\SetDressing\\Books\\GenBookB01_BookB04.mat" },
				{ "SetDressing\\Books\\DecalLetterLineOverlay01_LinesStripes.mat", "Materials\\SetDressing\\Books\\DecalLetterLineOverlay01_PatternF.mat" },
			};

			const MaterialSwap* swaps = nullptr;
			std::size_t swapCount = 0;
			if (a_formID == 0x000A9010) {
				swaps = kCaptainNote02Swaps;
				swapCount = std::size(kCaptainNote02Swaps);
			} else if (a_formID == 0x000A9012) {
				swaps = kCaptainNote01Swaps;
				swapCount = std::size(kCaptainNote01Swaps);
			} else if (a_formID == 0x000A9011) {
				// Note03 is authored with Notebook01.nif, not BookLarge02;
				// its blue material has no diffuse map in the base CDB.
				// Keep its authored tint rather than applying book cover swaps.
				REX::INFO("SFShopExplorer NativeModel: note form {:08X} uses Notebook01; no BookLarge02 swaps",
					a_formID);
				return;
			} else {
				return;
			}

			std::size_t applied = 0;
			for (auto& mesh : a_meshes) {
				const auto material = NormalizeBookMaterialPath(mesh.matPath);
				for (std::size_t i = 0; i < swapCount; ++i) {
					if (material != NormalizeBookMaterialPath(swaps[i].from)) {
						continue;
					}
					REX::INFO("SFShopExplorer NativeModel: authored book material swap form={:08X} mesh='{}' '{}' -> '{}'",
						a_formID, mesh.name, mesh.matPath, swaps[i].to);
					mesh.matPath = swaps[i].to;
					++applied;
					break;
				}
			}
			REX::INFO("SFShopExplorer NativeModel: authored book swaps form={:08X} applied={}", a_formID, applied);
		}

		// ── Safe memory helpers ──────────────────────────────────────────────


		std::string ResolveRTTIName(std::uintptr_t a_vtable);
		std::string DemangleRTTI(const std::string& a_mangled);
		bool TryParseMeshData(std::uintptr_t a_obj, NativeMesh& a_out,
			const float* a_world, bool a_hasWorld,
			std::uintptr_t a_scanStart = 0x08, std::uintptr_t a_scanEnd = 0x120);

		bool IsValidPtr(std::uintptr_t a_ptr)
		{
			return a_ptr > 0x10000 && a_ptr < 0x7FFFFFFFFFFF;
		}

		// Region cache: VirtualQuery costs ~13ms under this process's
		// fragmented VAD. Scanning one object or re-probing the same heap
		// ptr must not re-query. Bounds from one VQ cover the whole region.
		struct VQRegion
		{
			std::uintptr_t start{ 0 };
			std::uintptr_t end{ 0 };
			bool ok{ false };
		};
		constexpr std::size_t kVQCacheSlots = 64;
		thread_local VQRegion g_vqCache[kVQCacheSlots]{};
		thread_local std::size_t g_vqCacheNext{ 0 };

		bool CanRead(std::uintptr_t a_ptr, std::size_t a_size = 8)
		{
			if (a_ptr < 0x10000 || a_ptr >= 0x7FFFFFFFFFFF) return false;
			if (a_size > (0x7FFFFFFFFFFFULL - a_ptr)) return false;
			const auto need = a_ptr + a_size;

			for (std::size_t i = 0; i < kVQCacheSlots; ++i) {
				const auto& e = g_vqCache[i];
				if (e.start == 0 && e.end == 0) continue;
				if (a_ptr >= e.start && a_ptr < e.end) {
					return e.ok && need <= e.end;
				}
			}

			MEMORY_BASIC_INFORMATION mbi{};
			if (!VirtualQuery(reinterpret_cast<LPCVOID>(a_ptr), &mbi, sizeof(mbi))) return false;
			const auto regionStart = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
			const auto regionEnd = regionStart + mbi.RegionSize;
			bool ok = false;
			if (mbi.State == MEM_COMMIT) {
				const auto prot = mbi.Protect;
				const auto baseType = prot & 0xFF;
				if (!(prot & PAGE_GUARD) && baseType != 0x01 && baseType != 0x10) {
					ok = need <= regionEnd;
				}
			}
			g_vqCache[g_vqCacheNext] = { a_ptr, regionEnd, ok };
			g_vqCacheNext = (g_vqCacheNext + 1) % kVQCacheSlots;
			// Store full region bounds so sibling offsets in the same region
			// hit the cache on the next call (start = a_ptr is enough for
			// containment tests of later reads at/after a_ptr).
			g_vqCache[(g_vqCacheNext + kVQCacheSlots - 1) % kVQCacheSlots].start =
				regionStart < a_ptr ? regionStart : a_ptr;
			return ok;
		}

		// A VirtualQuery gate cannot make a heap read safe. The preview chain is
		// walked from the SFSE task thread while the main thread owns and frees
		// those objects, and the region cache above remembers a whole VAD span:
		// once the engine decommits a subrange, the cached `ok` still says yes.
		// Measured 2026-10-18 12:56:35 (Starfield_10-18-11-56.dmp,
		// FindTypedMember+0x9d): DataMenu's renderer slot held a pointer into
		// memory the menu had already released, CanRead answered from the cache,
		// and the load faulted - a CTD the moment the inventory opened. The load
		// is therefore the gate: a fault skips that slot, and the cache entries
		// that lied are dropped so they cannot vouch for the address again.
		void InvalidateVqCache()
		{
			for (auto& e : g_vqCache) {
				e.start = 0;
				e.end = 0;
				e.ok = false;
			}
			g_vqCacheNext = 0;
		}

		bool SafeReadPtr(std::uintptr_t a_addr, std::uintptr_t& a_out)
		{
			__try {
				a_out = *reinterpret_cast<const std::uintptr_t*>(a_addr);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				a_out = 0;
				InvalidateVqCache();
				return false;
			}
		}

		bool SafeReadU32(std::uintptr_t a_addr, std::uint32_t& a_out)
		{
			__try {
				a_out = *reinterpret_cast<const std::uint32_t*>(a_addr);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				a_out = 0;
				InvalidateVqCache();
				return false;
			}
		}

		// Printable-ASCII string read that stops at the first byte outside
		// 0x20..0x7E, so a type name ending at a section boundary cannot walk
		// into the next (possibly unmapped) page.
		bool SafeCopyAscii(std::uintptr_t a_addr, char* a_out, std::size_t a_cap)
		{
			__try {
				std::size_t n = 0;
				while (n < a_cap) {
					const char c = *reinterpret_cast<const char*>(a_addr + n);
					if (c < 0x20 || c >= 0x7F) {
						break;
					}
					a_out[n] = c;
					++n;
				}
				a_out[n] = '\0';
				return n > 0;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				a_out[0] = '\0';
				InvalidateVqCache();
				return false;
			}
		}

		// ── Game PE sections (RTTI / .rdata range checks) ────────────────────

		struct SectionInfo {
			std::uintptr_t base;
			std::uintptr_t virtualAddress;
			std::uint32_t virtualSize;
			std::uint32_t rawSize;
		};

		struct ModuleSections {
			SectionInfo text{};
			SectionInfo rdata{};
			bool valid = false;
		};

		ModuleSections GetGameSections()
		{
			// PE section parse is identical every call — cache it. Called once
			// per geometry leaf; re-parsing was pure overhead.
			static const ModuleSections cached = [] {
				ModuleSections result{};
				const auto* mod = GetModuleHandleW(nullptr);
				if (!mod) return result;

				const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mod);
				if (dos->e_magic != IMAGE_DOS_SIGNATURE) return result;

				const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
					reinterpret_cast<const std::uint8_t*>(mod) + dos->e_lfanew);
				if (nt->Signature != IMAGE_NT_SIGNATURE) return result;

				const auto* section = IMAGE_FIRST_SECTION(nt);
				for (std::uint16_t i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
					char name[9]{};
					std::memcpy(name, section->Name, 8);

					SectionInfo info{};
					info.base = std::bit_cast<std::uintptr_t>(mod);
					info.virtualAddress = section->VirtualAddress;
					info.virtualSize = section->Misc.VirtualSize;
					info.rawSize = section->SizeOfRawData;

					if (std::strcmp(name, ".text") == 0) {
						result.text = info;
					} else if (std::strcmp(name, ".rdata") == 0) {
						result.rdata = info;
					}
				}

				result.valid = result.text.virtualSize > 0 && result.rdata.virtualSize > 0;
				return result;
			}();
			return cached;
		}

		// ── NiAVObject tree walk ─────────────────────────────────────────────

		constexpr std::uintptr_t kChildrenOff = 0x130;

		bool LooksLikeNiNode(const RE::NiAVObject* a_obj)
		{
			if (!a_obj) return false;
			const auto base = std::bit_cast<std::uintptr_t>(a_obj);
			if (!CanRead(base + kChildrenOff + 0x18, 8)) return false;

			// Starfield runtime children = embedded NiTObjectArray with a
			// vptr (CommonLib's BSTArray claim does not match the live
			// dump): {vptr@+0x130, T* data@+0x138, u16 size@+0x140,
			// u16 cap@+0x142}. Proven on the 70-getmember armor clone.
			const auto q0 = *reinterpret_cast<const std::uintptr_t*>(base + kChildrenOff);
			if (ResolveRTTIName(q0).find("NiTObjectArray") != std::string::npos) {
				const auto data = *reinterpret_cast<const std::uintptr_t*>(base + kChildrenOff + 0x08);
				const auto size = *reinterpret_cast<const std::uint16_t*>(base + kChildrenOff + 0x10);
				const auto cap = *reinterpret_cast<const std::uint16_t*>(base + kChildrenOff + 0x12);
				if (size == 0) return true;
				if (size > cap || cap > 4096) return false;
				if (!IsValidPtr(data)) return false;
				const auto child0 = *reinterpret_cast<const std::uintptr_t*>(data);
				if (!IsValidPtr(child0) || !CanRead(child0, 0x18)) return false;
				const auto rt = ResolveRTTIName(*reinterpret_cast<const std::uintptr_t*>(child0));
				return rt.rfind(".?AV", 0) == 0;
			}

			// Legacy (pre-vptr layout) heuristic kept as fallback.
			const auto data = *reinterpret_cast<const std::uintptr_t*>(base + kChildrenOff + 0x10);
			const auto cap = *reinterpret_cast<const std::uint32_t*>(base + kChildrenOff + 0x08);
			const auto end = *reinterpret_cast<const std::uintptr_t*>(base + kChildrenOff + 0x00);
			if (!IsValidPtr(data)) return false;
			if (cap > 256) return false;
			if (end < data || end > data + cap * 8) return false;
			return true;
		}

		const RE::NiPointer<RE::NiAVObject>* ChildrenData(const RE::NiAVObject* a_obj)
		{
			const auto base = std::bit_cast<std::uintptr_t>(a_obj);
			if (!CanRead(base + kChildrenOff + 0x18, 8)) return nullptr;
			const auto q0 = *reinterpret_cast<const std::uintptr_t*>(base + kChildrenOff);
			std::uintptr_t ptr = 0;
			if (ResolveRTTIName(q0).find("NiTObjectArray") != std::string::npos) {
				ptr = *reinterpret_cast<const std::uintptr_t*>(base + kChildrenOff + 0x08);
			} else {
				ptr = *reinterpret_cast<const std::uintptr_t*>(base + kChildrenOff + 0x10);
			}
			if (!IsValidPtr(ptr) || !CanRead(ptr, 8)) return nullptr;
			return reinterpret_cast<const RE::NiPointer<RE::NiAVObject>*>(ptr);
		}

		std::uint32_t ChildrenCount(const RE::NiAVObject* a_obj)
		{
			const auto base = std::bit_cast<std::uintptr_t>(a_obj);
			if (!CanRead(base + kChildrenOff + 0x18, 8)) return 0;
			const auto q0 = *reinterpret_cast<const std::uintptr_t*>(base + kChildrenOff);
			if (ResolveRTTIName(q0).find("NiTObjectArray") != std::string::npos) {
				const auto size = *reinterpret_cast<const std::uint16_t*>(base + kChildrenOff + 0x10);
				return size > 4096 ? 0 : static_cast<std::uint32_t>(size);
			}
			if (!CanRead(base + kChildrenOff + 0x08, 4)) return 0;
			return *reinterpret_cast<const std::uint32_t*>(base + kChildrenOff + 0x08);
		}

		std::string GetNodeName(const RE::NiAVObject* a_obj)
		{
			if (!a_obj) return {};
			const auto base = std::bit_cast<std::uintptr_t>(a_obj);
			// NiObjectNET::name (BSFixedString) is at +0x10 (CommonLib
			// offsetof assert). Entry: {_left@0, len@+8, refcnt@+0x10,
			// flags@+0x14, inline string@+0x18} - same format TryReadBSFixedString
			// already proved on form fields. The old deref of _left as the
			// string produced garbage names ('??Y-R  2  ').
			if (!CanRead(base + 0x10, 8)) return {};
			const auto entry = *reinterpret_cast<const std::uintptr_t*>(base + 0x10);
			if (entry < 0x10000 || entry > 0x7FFFFFFFFFFF) return {};
			if (!CanRead(entry, 0x20)) return {};
			const auto len = *reinterpret_cast<const std::uint32_t*>(entry + 8);
			const auto flags = *reinterpret_cast<const std::uint8_t*>(entry + 0x14);
			if (len == 0 || len > 260) return {};
			if (flags & 0x2) return {};
			const auto strAddr = entry + 0x18;
			if (!CanRead(strAddr, len)) return {};
			const auto* p = reinterpret_cast<const char*>(strAddr);
			for (std::uint32_t i = 0; i < len; ++i) {
				if (p[i] < 0x20 || p[i] > 0x7E) return {};
			}
			return std::string(p, len);
		}

		void ReadBounding(const RE::NiAVObject* a_obj, float* a_center, float& a_radius)
		{
			if (!a_obj) return;
			const auto base = std::bit_cast<std::uintptr_t>(a_obj);
			if (!CanRead(base + 0x100, 16)) return;
			const auto boundBase = base + 0x100;
			a_center[0] = *reinterpret_cast<const float*>(boundBase + 0x00);
			a_center[1] = *reinterpret_cast<const float*>(boundBase + 0x04);
			a_center[2] = *reinterpret_cast<const float*>(boundBase + 0x08);
			a_radius = *reinterpret_cast<const float*>(boundBase + 0x0C);
		}

		bool LooksLikeTexturePath(const std::string& a_str)
		{
			if (a_str.empty() || a_str.size() > 260) return false;
			for (const auto& c : a_str) {
				if (c == '.') {
					const auto ext = a_str.substr(&c - a_str.data());
					if (ext.size() <= 5) {
						const auto low = ext.substr(0, std::min(ext.size(), std::size_t(4)));
						return low == ".dds" || low == ".png" || low == ".tga";
					}
				}
			}
			return false;
		}

		bool LooksLikeMatPath(const std::string& a_str)
		{
			if (a_str.empty() || a_str.size() > 260) return false;
			return a_str.size() >= 4 &&
				a_str[a_str.size() - 4] == '.' &&
				(a_str[a_str.size() - 3] == 'm' || a_str[a_str.size() - 3] == 'M') &&
				(a_str[a_str.size() - 2] == 'a' || a_str[a_str.size() - 2] == 'A') &&
				(a_str[a_str.size() - 1] == 't' || a_str[a_str.size() - 1] == 'T');
		}

		void ProbeGeometryNode(const RE::NiAVObject* a_obj, NativeMesh& a_out)
		{
			const auto base = std::bit_cast<std::uintptr_t>(a_obj);
			const auto tSetup = std::chrono::steady_clock::now();

			// Fixed scan window. The old maxOff probe did ~108 VirtualQuery
			// calls BEFORE the budget loop — under a fragmented address space
			// that alone cost ~2.5s. Every observed BSGeometry leaf is
			// readable through +0x800.
			constexpr std::uintptr_t maxOff = 0x800;
			if (!CanRead(base + 0x130, maxOff - 0x130)) {
				REX::INFO("  probe: field window unreadable");
				return;
			}

			// World transform for converting local verts to world-space.
			float world[16]{};
			bool hasWorld = false;
			if (CanRead(base + 0x80, 64)) {
				std::memcpy(world, reinterpret_cast<const void*>(base + 0x80), 64);
				hasWorld = true;
			}

			const auto sections = GetGameSections();
			std::uintptr_t rdataLo = 0, rdataHi = 0;
			if (sections.valid) {
				rdataLo = sections.rdata.base + sections.rdata.virtualAddress;
				const auto rdataSize = std::min<std::uint32_t>(sections.rdata.virtualSize, sections.rdata.rawSize);
				rdataHi = rdataLo + rdataSize;
			}
			const auto inRdata = [&](std::uintptr_t p) {
				return rdataHi > rdataLo && p >= rdataLo && p < rdataHi;
			};
			const auto setupMs = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - tSetup).count();

			// Budget starts AFTER setup CanReads — those VirtualQuerys were
			// eating the entire 40ms so field collect never ran (fields=0).
			const auto t0 = std::chrono::steady_clock::now();
			constexpr auto kBudget = std::chrono::milliseconds(40);
			const auto expired = [&] {
				return std::chrono::steady_clock::now() - t0 > kBudget;
			};
			const auto ms = [&] {
				return std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - t0).count();
			};

			bool foundMesh = false;

			// Phase A/B: pure reads, no VQ. Caps are high because collection
			// is free — an earlier kMaxFields=64 starting at +0x130 filled
			// with header junk and NEVER reached the real mesh fields at
			// +0x310 (buf) / +0x390 (count) documented in the RE notes.
			struct FieldPtr { std::uintptr_t off; std::uintptr_t ptr; bool hasRtti; };
			struct FieldCount { std::uintptr_t off; std::uint32_t value; };
			std::vector<FieldPtr> heapPtrs;
			std::vector<FieldCount> counts;
			constexpr std::uint32_t kMaxCount = 200'000;
			constexpr std::uint32_t kMaxIdx = 600'000;
			constexpr std::size_t kMaxFields = 512;

			for (std::uintptr_t off = 0x130; off < maxOff; off += 4) {
				const auto u = *reinterpret_cast<const std::uint32_t*>(base + off);
				if (u < 8 || u > kMaxCount) continue;
				// 6j: u32 at off%8==4 of a heap-ptr slot reads the pointer's
				// HIGH half (0x00000267… → 615). That flooded vertCounts with
				// garbage and crowded out real counts (+0x1B0 = 5772).
				if ((off % 8) == 4) {
					const auto q = *reinterpret_cast<const std::uint64_t*>(base + (off - 4));
					if (IsValidPtr(q) && q >= 0x100000ULL && q <= 0x00007FFFFFFFFFFFULL) continue;
				}
				counts.push_back({ off, u });
				if (counts.size() >= kMaxFields) break;
			}

			// Phase B: heap ptr candidacy — 8-byte-aligned offsets only.
			// Stepping by 4 read overlapping/misaligned u64s (packed u32 pairs,
			// float bit patterns) that flooded the probe list with garbage.
			// IsValidPtr also tightened: real heap/module ptrs only.
			for (std::uintptr_t off = 0x130; off < maxOff; off += 8) {
				if (heapPtrs.size() >= kMaxFields) break;
				const auto val = *reinterpret_cast<const std::uintptr_t*>(base + off);
				if (!IsValidPtr(val)) continue;
				// Reject packed small u32 pairs and non-canonical junk:
				// real ptrs have top bits 0x0000… and are ≥1MB.
				if (val < 0x100000ULL) continue;
				if (val > 0x00007FFFFFFFFFFFULL) continue;
				heapPtrs.push_back({ off, val, false });
			}

			// One-shot inventory on the first geometry leaf only (RE aid).
			{
				static bool s_dumped = false;
				if (!s_dumped) {
					s_dumped = true;
					REX::INFO("  field inventory: counts={} ptrs={}", counts.size(), heapPtrs.size());
					// Every count in the mesh-relevant window (not just first 24).
					for (const auto& c : counts) {
						if (c.off >= 0x180 && c.off <= 0x420) {
							REX::INFO("    count +0x{:X} = {}", c.off, c.value);
						}
					}
					for (const auto& p : heapPtrs) {
						if (p.off >= 0x2C0 && p.off <= 0x3A0) {
							REX::INFO("    band ptr +0x{:X} = {:#x}", p.off, p.ptr);
						}
					}
					// Full u64 field dump +0x130..+0x3C0 (locks Starfield layout offline).
					if (CanRead(base + 0x130, 0x290)) {
						for (std::uintptr_t o = 0x130; o < 0x3C0; o += 8) {
							const auto q = *reinterpret_cast<const std::uint64_t*>(base + o);
							REX::INFO("    geo+0x{:03X}: {:#018x}", o, q);
						}
					}
					// First 16 heap ptrs: what do they point at?
					std::size_t dumped = 0;
					for (const auto& p : heapPtrs) {
						if (dumped >= 16) break;
						if (inRdata(p.ptr)) continue;
						if (!CanRead(p.ptr, 16)) continue;
						const auto a = *reinterpret_cast<const std::uint64_t*>(p.ptr);
						const auto b = *reinterpret_cast<const std::uint64_t*>(p.ptr + 8);
						REX::INFO("    tgt +0x{:X} -> {:#x}: {:#018x} {:#018x}",
							p.off, p.ptr, a, b);
						++dumped;
					}
				}
			}

			// Mesh budget restarts AFTER free collect — Phase B no longer
			// consumes VirtualQuerys, but RTTI/cross-pair still need a few.
			// 80ms ≈ 6 VQs at ~13ms each; shop stays responsive (was 400s).
			const auto tMesh = std::chrono::steady_clock::now();
			constexpr auto kMeshBudget = std::chrono::milliseconds(80);
			const auto meshExpired = [&] {
				return std::chrono::steady_clock::now() - tMesh > kMeshBudget;
			};
			const auto meshMs = [&] {
				return std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - tMesh).count();
			};

			// ── 0) FO4-style rendererData → TriShape{VB,IB} → Buffer.data ──
			// 6i: geo+0x148 is NULL on Starfield; nearby slots are bound
			// floats (0x7f7fffff). Only chase when +00 looks like VertexDesc
			// (small stride nibble), NOT a heap pointer.
			if (!foundMesh && !meshExpired()) {
				const auto tryTriShape = [&](std::uintptr_t shape, const char* tag) -> bool {
					if (!shape || !IsValidPtr(shape) || inRdata(shape) || !CanRead(shape, 0x20)) return false;
					const auto vd = *reinterpret_cast<const std::uint64_t*>(shape + 0x00);
					const auto stride = static_cast<std::uint32_t>(vd & 0xF) * 4;
					// FO4 VertexDesc: low nibble is dword count (stride/4). Reject
					// when +00 is a heap/module pointer (Starfield shifted layout).
					if (stride < 12 || stride > 64) return false;
					const auto vb = *reinterpret_cast<const std::uintptr_t*>(shape + 0x08);
					const auto ib = *reinterpret_cast<const std::uintptr_t*>(shape + 0x10);
					if (!IsValidPtr(vb) || !IsValidPtr(ib) || inRdata(vb) || inRdata(ib)) return false;
					if (!CanRead(vb, 0x40) || !CanRead(ib, 0x40)) return false;
					const auto vbData = *reinterpret_cast<const std::uintptr_t*>(vb + 0x08);
					const auto ibData = *reinterpret_cast<const std::uintptr_t*>(ib + 0x08);
					const auto vbSize = *reinterpret_cast<const std::uint32_t*>(vb + 0x34);
					const auto ibSize = *reinterpret_cast<const std::uint32_t*>(ib + 0x34);
					REX::INFO("  tri-shape {} @ {:#x}: vd={:#x} stride={} vb={:#x} ib={:#x}",
						tag, shape, vd, stride, vb, ib);
					REX::INFO("    VB data={:#x} size={} | IB data={:#x} size={}",
						vbData, vbSize, ibData, ibSize);
					if (!IsValidPtr(vbData) || !IsValidPtr(ibData)) return false;
					if (inRdata(vbData) || inRdata(ibData)) return false;
					const auto vertCount = vbSize / stride;
					const auto idxCount = ibSize / 2;
					if (vertCount < 8 || vertCount > kMaxCount) return false;
					if (!CanRead(vbData, static_cast<std::size_t>(vertCount) * stride)) return false;
					const auto* f = reinterpret_cast<const float*>(vbData);
					float lo[3] = { 1e30f, 1e30f, 1e30f };
					float hi[3] = { -1e30f, -1e30f, -1e30f };
					const auto sampleN = std::min<std::uint32_t>(vertCount, 4);
					bool finite = true;
					const auto fltStride = stride / 4;
					for (std::uint32_t i = 0; i < sampleN && finite; ++i) {
						for (int c = 0; c < 3; ++c) {
							const auto v = f[i * fltStride + c];
							if (!std::isfinite(v)) { finite = false; break; }
							lo[c] = std::min(lo[c], v);
							hi[c] = std::max(hi[c], v);
						}
					}
					const auto extent = std::max({ hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2] });
					REX::INFO("    extent={:.4f} finite={} vertCount={} idxCount16={}",
						extent, finite, vertCount, idxCount);
					if (!finite || extent <= 0.01f || extent >= 1000.f) return false;

					a_out.pos.resize(static_cast<std::size_t>(vertCount) * 3);
					for (std::uint32_t n = 0; n < vertCount; ++n) {
						float x = f[n * fltStride + 0];
						float y = f[n * fltStride + 1];
						float z = f[n * fltStride + 2];
						if (hasWorld) {
							const float wx = world[0] * x + world[4] * y + world[8] * z + world[12];
							const float wy = world[1] * x + world[5] * y + world[9] * z + world[13];
							const float wz = world[2] * x + world[6] * y + world[10] * z + world[14];
							x = wx; y = wy; z = wz;
						}
						a_out.pos[n * 3 + 0] = x;
						a_out.pos[n * 3 + 1] = y;
						a_out.pos[n * 3 + 2] = z;
					}
					bool gotIdx = false;
					if (idxCount >= 3 && (idxCount % 3) == 0 && idxCount <= kMaxIdx &&
						CanRead(ibData, static_cast<std::size_t>(idxCount) * 2)) {
						const auto* u16 = reinterpret_cast<const std::uint16_t*>(ibData);
						std::uint32_t maxIdx = 0;
						bool ok = true;
						for (std::uint32_t n = 0; n < idxCount; ++n) {
							if (u16[n] >= vertCount) { ok = false; break; }
							maxIdx = std::max(maxIdx, static_cast<std::uint32_t>(u16[n]));
						}
						if (ok && maxIdx > 0) {
							a_out.indices.resize(idxCount);
							for (std::uint32_t n = 0; n < idxCount; ++n) a_out.indices[n] = u16[n];
							gotIdx = true;
						}
					}
					if (!gotIdx) {
						const auto idxCount32 = ibSize / 4;
						if (idxCount32 >= 3 && (idxCount32 % 3) == 0 && idxCount32 <= kMaxIdx &&
							CanRead(ibData, static_cast<std::size_t>(idxCount32) * 4)) {
							const auto* u32 = reinterpret_cast<const std::uint32_t*>(ibData);
							std::uint32_t maxIdx = 0;
							bool ok = true;
							for (std::uint32_t n = 0; n < idxCount32; ++n) {
								if (u32[n] >= vertCount) { ok = false; break; }
								maxIdx = std::max(maxIdx, u32[n]);
							}
							if (ok && maxIdx > 0) {
								a_out.indices.resize(idxCount32);
								for (std::uint32_t n = 0; n < idxCount32; ++n) a_out.indices[n] = u32[n];
								gotIdx = true;
							}
						}
					}
					if (!gotIdx) {
						REX::INFO("    index validate failed");
						return false;
					}
					REX::INFO("  mesh from {}: pos={} uv={} idx={} stride={} ({}ms)",
						tag, a_out.pos.size(), a_out.uv.size(), a_out.indices.size(), stride, meshMs());
					return true;
				};

				for (std::uintptr_t off = 0x140; off < 0x1C0 && !meshExpired(); off += 8) {
					if (!CanRead(base + off, 8)) continue;
					const auto cand = *reinterpret_cast<const std::uintptr_t*>(base + off);
					if (!IsValidPtr(cand) || cand < 0x100000ULL || inRdata(cand)) continue;
					if (tryTriShape(cand, std::format("+0x{:X}", off).c_str())) {
						foundMesh = true;
						break;
					}
				}
			}

			// ── 1) RTTI — try every rdata-vt heap ptr (cap 4), log names ──────
			// 6k: "first only" stopped on a non-geometry object and never
			// reached +0x210 (real RTTI child). Budget-gated.
			if (!foundMesh) {
				std::uint32_t rttiTried = 0;
				for (const auto& p : heapPtrs) {
					if (meshExpired() || rttiTried >= 4) break;
					if (p.off >= 0x2C0 && p.off <= 0x3A0) continue; // band last
					if (!CanRead(p.ptr, 8)) continue;
					const auto vt = *reinterpret_cast<const std::uintptr_t*>(p.ptr);
					if (!inRdata(vt)) continue;
					++rttiTried;
					REX::INFO("  RTTI obj +0x{:X} -> {:#x} vt={:#x} name='{}'",
						p.off, p.ptr, vt, DemangleRTTI(ResolveRTTIName(vt)));
					if (TryParseMeshData(p.ptr, a_out, world, hasWorld, 0x08, 0x280)) {
						foundMesh = true;
						REX::INFO("  mesh extracted at +0x{:X}: pos={} uv={} idx={} ({}ms)",
							p.off, a_out.pos.size(), a_out.uv.size(), a_out.indices.size(), meshMs());
					}
				}
			}

			// ── 1b) TryParse on band object headers (nested NiTriShapeData*) ──
			// 6g: +0x310/330/350 are small object headers, not raw buffers.
			// Scan each as a candidate mesh-data object (count+ptr pairs inside).
			if (!foundMesh) {
				std::uint32_t bandTried = 0;
				for (const auto& p : heapPtrs) {
					if (meshExpired() || bandTried >= 6) break;
					if (p.off < 0x2C0 || p.off > 0x3A0) continue;
					if (!CanRead(p.ptr, 8)) continue;
					++bandTried;
					if (TryParseMeshData(p.ptr, a_out, world, hasWorld, 0x08, 0x200)) {
						foundMesh = true;
						REX::INFO("  mesh from band header +0x{:X} -> {:#x}: pos={} uv={} idx={} ({}ms)",
							p.off, p.ptr, a_out.pos.size(), a_out.uv.size(), a_out.indices.size(), meshMs());
						break;
					}
				}
			}

			// ── 2) Cross-pair — pre-filter counts (no VQ); sample top pairs ──
			// Nested 64×64 with per-pair CanRead was the VQ storm. Pre-filter
			// to plausible vert/index counts, cap unique ptrs probed, rely on
			// the region cache so each heap region is VirtualQuery'd once.
			if (!foundMesh && !meshExpired() && !counts.empty() && !heapPtrs.empty()) {
				constexpr std::uint32_t kMaxCand = 8;
				constexpr std::uint32_t kMaxAttempts = 16;
				// Multi-stride: Starfield verts are interleaved (pos+nrm+uv…),
				// not packed float3. Try common strides; first finite non-
				// degenerate extent wins.
				const auto sampleVertExtent = [&](std::uintptr_t ptr, std::uint32_t count, float& outExtent, std::uint32_t& outStride) -> bool {
					if (count < 8 || count > kMaxCount) return false;
					static constexpr std::uint32_t kStrides[] = { 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 64 };
					constexpr std::uint32_t kSample = 4;
					if (!CanRead(ptr, 64)) return false;
					for (const auto stride : kStrides) {
						if (stride * kSample > 64 && !CanRead(ptr, stride * kSample)) continue;
						const auto* baseF = reinterpret_cast<const float*>(ptr);
						const auto flt = stride / 4;
						float lo[3] = { 1e30f, 1e30f, 1e30f };
						float hi[3] = { -1e30f, -1e30f, -1e30f };
						bool finite = true;
						bool hasMag = false;
						for (std::uint32_t i = 0; i < kSample && finite; ++i) {
							for (int c = 0; c < 3; ++c) {
								const auto v = baseF[i * flt + c];
								if (!std::isfinite(v)) { finite = false; break; }
								if (std::fabs(v) > 0.05f) hasMag = true;
								lo[c] = std::min(lo[c], v);
								hi[c] = std::max(hi[c], v);
							}
						}
						if (!finite || !hasMag) continue;
						outExtent = std::max({ hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2] });
						// 6m: extent>0.01 accepted header junk (-0.014,0,0,…).
						// Real weapon meshes have bbox extent ≥ ~0.1.
						if (outExtent > 0.1f && outExtent < 1000.f) {
							outStride = stride;
							return true;
						}
					}
					return false;
				};
				const auto indexCountOk = [](std::uint32_t n) {
					if (n < 3 || n > kMaxIdx || (n % 3) != 0) return false;
					// Packed-flag / size-field pseudo-counts: 0x00030003,
					// 0x00010003, and any 0x000N0000 (65536, 196608, …).
					const auto hi = n >> 16;
					const auto lo16 = n & 0xFFFF;
					if (hi != 0 && hi == lo16) return false;
					if (hi == 1 && lo16 == 3) return false;
					if (hi != 0 && lo16 == 0) return false;
					return true;
				};

				struct VCand { std::uint32_t count; std::uintptr_t ptr; std::uintptr_t countOff; std::uint32_t stride; float extent; };
				struct ICand { std::uint32_t count; std::uintptr_t ptr; std::uintptr_t countOff; bool u16; bool derived; };
				std::vector<VCand> verts;
				std::vector<ICand> inds;
				verts.reserve(kMaxCand);
				inds.reserve(kMaxCand);

				// Prefer real-looking counts: drop packed-flag pairs (0x00030003)
				// and tiny header fields. +0x1B0-class values (1k..20k) first.
				std::vector<const FieldCount*> vertCounts;
				std::vector<const FieldCount*> idxCounts;
				const auto countJunk = [](std::uint32_t v) {
					const auto hi = v >> 16;
					const auto lo16 = v & 0xFFFF;
					if (hi != 0 && hi == lo16) return true;
					if (hi == 1 && lo16 == 3) return true;
					// 0x000N0000 = allocation size / flags (65536, 196608…)
					if (hi != 0 && lo16 == 0) return true;
					// 0x00010002 = 65538 (flags), seen as fake iCount
					if (hi == 1 && lo16 < 8) return true;
					return false;
				};
				for (const auto& c : counts) {
					if (countJunk(c.value)) continue;
					const bool inBand = c.off >= 0x370 && c.off <= 0x420;
					if (inBand && c.value >= 8 && c.value <= 100'000) vertCounts.push_back(&c);
					if (inBand && indexCountOk(c.value)) idxCounts.push_back(&c);
				}
				for (const auto& c : counts) {
					if (countJunk(c.value)) continue;
					if (c.off >= 0x370 && c.off <= 0x420) continue;
					if (vertCounts.size() < 24 && c.value >= 8 && c.value <= 100'000) vertCounts.push_back(&c);
					if (idxCounts.size() < 24 && indexCountOk(c.value)) idxCounts.push_back(&c);
					if (vertCounts.size() >= 24 && idxCounts.size() >= 24) break;
				}

				// Probe order: one-level NESTED ptrs from band targets FIRST
				// (6g: band ptrs are small object headers in one ~344B cluster,
				// not vertex buffers — buffers live one indirection deeper),
				// then non-band ptrs, then raw band ptrs last.
				std::vector<std::uintptr_t> probePtrs;
				probePtrs.reserve(48);
				std::vector<std::uintptr_t> seen;
				seen.reserve(64);
				const auto addProbe = [&](std::uintptr_t ptr) {
					if (!IsValidPtr(ptr) || ptr < 0x100000ULL || ptr > 0x00007FFFFFFFFFFFULL) return;
					if (inRdata(ptr)) return; // module rdata: not a heap buffer
					if (GetGameSections().valid) {
						const auto secs = GetGameSections();
						const auto textLo = secs.text.base + secs.text.virtualAddress;
						const auto textHi = textLo + secs.text.virtualSize;
						const auto rdLo = secs.rdata.base + secs.rdata.virtualAddress;
						const auto rdHi = rdLo + secs.rdata.virtualSize;
						if (ptr >= textLo && ptr < textHi) return;
						if (ptr >= rdLo && ptr < rdHi) return;
					}
					for (auto s : seen) if (s == ptr) return;
					if (probePtrs.size() >= 48) return;
					seen.push_back(ptr);
					probePtrs.push_back(ptr);
				};
				// Field-local count↔ptr pairing (6p): classic {count, ptr} or
				// {ptr, count} layout — the data buffer for a count field usually
				// lives at count±8. 6q: 6p pushed these INTO probePtrs and they
				// ate 5 of the 48-cap slots, truncating the nested walk's
				// non-band group (32→27) — verts dropped 8→0 across every item.
				// Keep them in a SEPARATE list probed first; full nested 48
				// preserved. 6p showed locals resolve to band-cluster headers,
				// so try, don't trust. +0x80/-0x80 per {count @ A, ptr @ A+0x80}.
				std::vector<std::uintptr_t> localPtrs;
				localPtrs.reserve(16);
				const auto addLocalPtr = [&](std::uintptr_t countOff) {
					for (const auto delta : { 8, -8, 16, -16, 0x80, -0x80 }) {
						if (countOff + delta < 0x130 || countOff + delta + 8 > maxOff) continue;
						const auto p = *reinterpret_cast<const std::uintptr_t*>(base + countOff + delta);
						if (!IsValidPtr(p) || p < 0x100000ULL || p > 0x00007FFFFFFFFFFFULL) continue;
						if (inRdata(p)) continue;
						bool dup = false;
						for (auto s : localPtrs) if (s == p) { dup = true; break; }
						if (dup || localPtrs.size() >= 16) continue;
						localPtrs.push_back(p);
					}
				};
				for (const auto* c : idxCounts) addLocalPtr(c->off);
				for (const auto* c : vertCounts) addLocalPtr(c->off);
				REX::INFO("  field-local probes={} (idxCounts={} vertCounts={})",
					localPtrs.size(), idxCounts.size(), vertCounts.size());
				// Nested: TWO levels deep from non-band first (parent/renderer
				// at +0x138/+0x210), then band headers — 6k proved one level
				// only yields the ~0x170 object-header cluster, not buffers.
				std::function<void(std::uintptr_t, int)> addNested;
				addNested = [&](std::uintptr_t root, int depth) {
					if (depth <= 0 || !CanRead(root, 0x80)) return;
					for (std::uintptr_t o = 0; o < 0x80; o += 8) {
						const auto inner = *reinterpret_cast<const std::uintptr_t*>(root + o);
						if (!IsValidPtr(inner) || inner < 0x100000ULL || inRdata(inner)) continue;
						if (std::find(seen.begin(), seen.end(), inner) != seen.end()) continue;
						if (probePtrs.size() >= 48) return;
						addProbe(inner);
						if (depth > 1) addNested(inner, depth - 1);
					}
				};
				for (const auto& p : heapPtrs) {
					if (p.off >= 0x2C0 && p.off <= 0x3A0) continue;
					addNested(p.ptr, 2);
					if (probePtrs.size() >= 32) break;
				}
				for (const auto& p : heapPtrs) {
					if (p.off < 0x2C0 || p.off > 0x3A0) continue;
					addNested(p.ptr, 2);
					if (probePtrs.size() >= 48) break;
				}
				REX::INFO("  nested probe: {} (2-level, non-band+band)", probePtrs.size());
				// Non-band first (original geo fields that may hold raw buffers)
				for (const auto& p : heapPtrs) {
					if (p.off >= 0x2C0 && p.off <= 0x3A0) continue;
					addProbe(p.ptr);
					if (probePtrs.size() >= 32) break;
				}
				// Raw band headers last (unlikely verts, but TryParse may apply)
				for (const auto& p : heapPtrs) {
					if (p.off < 0x2C0 || p.off > 0x3A0) continue;
					addProbe(p.ptr);
					if (probePtrs.size() >= 48) break;
				}
				// 6q: probe list = field-locals (separate, so they never stole
				// nested slots) first, then the full nested/direct set.
				std::vector<std::uintptr_t> allPtrs;
				allPtrs.reserve(probePtrs.size() + localPtrs.size());
				for (auto p : localPtrs) allPtrs.push_back(p);
				for (auto p : probePtrs) {
					bool dup = false;
					for (auto q : allPtrs) if (q == p) { dup = true; break; }
					if (!dup) allPtrs.push_back(p);
				}
				REX::INFO("  allPtrs={} (locals={} nested={})", allPtrs.size(), localPtrs.size(), probePtrs.size());

				for (std::size_t pi = 0; pi < allPtrs.size() && !meshExpired(); ++pi) {
					const auto ptr = allPtrs[pi];
					if (!IsValidPtr(ptr)) continue;
					// One CanRead per ptr covers sample+index samples via region cache.
					if (!CanRead(ptr, 64)) continue;

					if (verts.size() < kMaxCand) {
						for (const auto* c : vertCounts) {
							if (meshExpired() || verts.size() >= kMaxCand) break;
							float extent = 0.f;
							std::uint32_t stride = 0;
							if (sampleVertExtent(ptr, c->value, extent, stride)) {
								verts.push_back({ c->value, ptr, c->off, stride, extent });
							}
						}
					}
					if (inds.size() < kMaxCand) {
						for (const auto* c : idxCounts) {
							if (meshExpired() || inds.size() >= kMaxCand) break;
							const auto n16 = std::min<std::uint32_t>(c->value, 16);
							if (CanRead(ptr, static_cast<std::size_t>(n16) * 2)) {
								const auto* u = reinterpret_cast<const std::uint16_t*>(ptr);
								bool ok = true;
								std::uint32_t maxIdx = 0;
								for (std::uint32_t i = 0; i < n16; ++i) {
									if (u[i] == 0xFFFF) { ok = false; break; }
									maxIdx = std::max(maxIdx, static_cast<std::uint32_t>(u[i]));
								}
								if (ok && maxIdx > 0 && maxIdx < 0xFFFF) {
									inds.push_back({ c->value, ptr, c->off, true, false });
								}
							}
							if (inds.size() >= kMaxCand) break;
						const auto n32 = std::min<std::uint32_t>(c->value, 16);
						if (CanRead(ptr, static_cast<std::size_t>(n32) * 4)) {
							const auto* u = reinterpret_cast<const std::uint32_t*>(ptr);
							bool ok = true;
							std::uint32_t maxIdx = 0;
							for (std::uint32_t i = 0; i < n32; ++i) {
								if (u[i] > kMaxCount) { ok = false; break; }
								maxIdx = std::max(maxIdx, u[i]);
							}
							if (ok && maxIdx > 0) {
								inds.push_back({ c->value, ptr, c->off, false, false });
							}
						}
					}
					}
					if (verts.size() >= kMaxCand && inds.size() >= kMaxCand) break;
				}

				// Derived idx counts: real index buffer often shares magnitude
				// with vert count but lives elsewhere. 6m: field scan's only
				// idx cand was junk 196608 while verts=63698 sat unpaired.
				if (!verts.empty() && inds.size() < 4) {
					std::vector<std::uint32_t> derived;
					for (const auto* vc : vertCounts) {
						const auto v = vc->value;
						const auto r3 = [](std::uint32_t x) { return (x / 3) * 3; };
						for (std::uint32_t cand : { r3(v), r3(v + v / 2), r3(v * 2), r3(v * 3) }) {
							if (cand >= 3 && cand <= 300'000 && (cand % 3) == 0) {
								bool dup = false;
								for (auto d : derived) if (d == cand) { dup = true; break; }
								if (!dup) derived.push_back(cand);
							}
						}
					}
					for (const auto* ic : idxCounts) {
						bool dup = false;
						for (auto d : derived) if (d == ic->value) { dup = true; break; }
						if (!dup && ic->value >= 3 && ic->value <= 300'000) derived.push_back(ic->value);
					}
					for (std::size_t pi = 0; pi < allPtrs.size() && inds.size() < kMaxCand && !meshExpired(); ++pi) {
						const auto ptr = allPtrs[pi];
						if (!CanRead(ptr, 32)) continue;
						for (const auto dcount : derived) {
							if (inds.size() >= kMaxCand) break;
							const auto n16 = std::min<std::uint32_t>(dcount, 16);
							if (CanRead(ptr, static_cast<std::size_t>(n16) * 2)) {
								const auto* u = reinterpret_cast<const std::uint16_t*>(ptr);
								bool ok = true;
								std::uint32_t maxIdx = 0;
								for (std::uint32_t i = 0; i < n16; ++i) {
									if (u[i] == 0xFFFF) { ok = false; break; }
									maxIdx = std::max(maxIdx, static_cast<std::uint32_t>(u[i]));
								}
								if (ok && maxIdx > 0 && maxIdx < 0xFFFF) {
									inds.push_back({ dcount, ptr, 0, true, true });
								}
							}
						}
					}
					REX::INFO("  derived idx probe: {} counts, inds now={}", derived.size(), inds.size());
				}

				// 6m: sort-by-count put junk vCount=63698 first; all 16 attempts
				// burned on derived inds from that same junk before smaller real
				// counts (5336/5215) were tried. Prefer larger bbox extent
				// (real mesh), then field (non-derived) inds first.
				std::sort(verts.begin(), verts.end(), [](const VCand& a, const VCand& b) {
					if (a.extent != b.extent) return a.extent > b.extent;
					return a.count > b.count;
				});
				std::stable_sort(inds.begin(), inds.end(), [](const ICand& a, const ICand& b) {
					return a.derived < b.derived;
				});

				std::uint32_t attempts = 0;
				std::uint32_t rejectFullV = 0, rejectRatio = 0, rejectFullI = 0, rejectIdx = 0;
				for (auto& v : verts) {
					if (foundMesh || attempts >= kMaxAttempts || meshExpired()) break;
					// Cap materialize size — huge garbage counts still page-fault.
					if (v.count > 100'000) continue;
					if (!CanRead(v.ptr, static_cast<std::size_t>(v.count) * v.stride)) { ++rejectFullV; continue; }
					// Per-vert cap so one bad pair cannot exhaust kMaxAttempts.
					std::uint32_t vertTried = 0;
					for (auto& i : inds) {
						if (foundMesh || attempts >= kMaxAttempts || meshExpired()) break;
						if (vertTried >= 3) break;
						if (i.count % 3 != 0 || i.count > 300'000) continue;
						const auto tri = i.count / 3;
						if (tri + 1u < v.count / 8u || tri > v.count * 4u) { ++rejectRatio; continue; }
						++attempts;
						++vertTried;
						if (!CanRead(i.ptr, static_cast<std::size_t>(i.count) * (i.u16 ? 2u : 4u))) { ++rejectFullI; continue; }

						bool fullOk = true;
						std::uint32_t maxIdx = 0;
						if (i.u16) {
							const auto* u16 = reinterpret_cast<const std::uint16_t*>(i.ptr);
							for (std::uint32_t n = 0; n < i.count; ++n) {
								if (u16[n] >= v.count) { fullOk = false; break; }
								maxIdx = std::max(maxIdx, static_cast<std::uint32_t>(u16[n]));
							}
						} else {
							const auto* u32 = reinterpret_cast<const std::uint32_t*>(i.ptr);
							for (std::uint32_t n = 0; n < i.count; ++n) {
								if (u32[n] >= v.count) { fullOk = false; break; }
								maxIdx = std::max(maxIdx, u32[n]);
							}
						}
						if (!fullOk || maxIdx == 0) {
							++rejectIdx;
							if (rejectIdx <= 5) {
								REX::INFO("    rejIdx vCount={} iCount={} u16={} derived={} firstBad/max={}",
									v.count, i.count, i.u16, i.derived, maxIdx);
							}
							continue;
						}

						a_out.pos.resize(static_cast<std::size_t>(v.count) * 3);
						const auto* f = reinterpret_cast<const float*>(v.ptr);
						const auto flt = v.stride / 4;
						for (std::uint32_t n = 0; n < v.count; ++n) {
							float x = f[n * flt + 0];
							float y = f[n * flt + 1];
							float z = f[n * flt + 2];
							if (hasWorld) {
								const float wx = world[0] * x + world[4] * y + world[8] * z + world[12];
								const float wy = world[1] * x + world[5] * y + world[9] * z + world[13];
								const float wz = world[2] * x + world[6] * y + world[10] * z + world[14];
								x = wx; y = wy; z = wz;
							}
							a_out.pos[n * 3 + 0] = x;
							a_out.pos[n * 3 + 1] = y;
							a_out.pos[n * 3 + 2] = z;
						}

						a_out.indices.resize(i.count);
						if (i.u16) {
							const auto* u16 = reinterpret_cast<const std::uint16_t*>(i.ptr);
							for (std::uint32_t n = 0; n < i.count; ++n) a_out.indices[n] = u16[n];
						} else {
							const auto* u32 = reinterpret_cast<const std::uint32_t*>(i.ptr);
							for (std::uint32_t n = 0; n < i.count; ++n) a_out.indices[n] = u32[n];
						}
						foundMesh = true;
						REX::INFO("  mesh from field cross-pair: pos={} uv={} idx={} (vCount@+0x{:X} iCount@+0x{:X} u16={} stride={} {}ms)",
							a_out.pos.size(), a_out.uv.size(), a_out.indices.size(),
							v.countOff, i.countOff, i.u16, v.stride, meshMs());
					}
				}
				// Content-based idx discovery (6o): count fields never yield a
				// real idx buffer (iCands=0), and derived counts sample the
				// wrong ptrs. Given real verts, scan every probe ptr as a raw
				// u16 sequence: if all sampled values < v.count and max>0,
				// walk forward to the natural end of the run.
				if (!foundMesh && !verts.empty() && !meshExpired()) {
					constexpr std::uint32_t kMaxScan = 64;
					std::uint32_t contentTried = 0;
					for (const auto& v : verts) {
						if (foundMesh || meshExpired() || contentTried >= 24) break;
						if (v.count < 8 || v.count > 100'000) continue;
						for (std::size_t pi = 0; pi < allPtrs.size() && !foundMesh && !meshExpired(); ++pi) {
							if (contentTried >= 24) break;
							const auto ptr = allPtrs[pi];
							if (ptr == v.ptr) continue;
							if (!CanRead(ptr, kMaxScan * 2)) continue;
							++contentTried;
							const auto* u = reinterpret_cast<const std::uint16_t*>(ptr);
							std::uint32_t maxIdx = 0;
							bool ok = true;
							for (std::uint32_t n = 0; n < kMaxScan; ++n) {
								if (u[n] >= v.count) { ok = false; break; }
								if (u[n] > maxIdx) maxIdx = u[n];
							}
							if (!ok || maxIdx == 0) continue;
							// Run looks like indices into v — walk to natural end.
							const auto cap = std::min<std::uint32_t>(v.count * 3, 300'000);
							if (!CanRead(ptr, static_cast<std::size_t>(cap) * 2)) continue;
							std::uint32_t run = kMaxScan;
							for (std::uint32_t n = kMaxScan; n < cap; ++n) {
								if (u[n] >= v.count) break;
								run = n + 1;
							}
							auto iCount = (run / 3) * 3;
							// Prefer the longest run; also try run-3 if degenerate.
							if (iCount < 3) continue;
							// Full validate
							bool fullOk = true;
							std::uint32_t fullMax = 0;
							for (std::uint32_t n = 0; n < iCount; ++n) {
								if (u[n] >= v.count) { fullOk = false; break; }
								fullMax = std::max(fullMax, static_cast<std::uint32_t>(u[n]));
							}
							if (!fullOk || fullMax == 0) continue;
							const auto tri = iCount / 3;
							if (tri + 1u < v.count / 8u || tri > v.count * 4u) continue;
							a_out.pos.resize(static_cast<std::size_t>(v.count) * 3);
							const auto* f = reinterpret_cast<const float*>(v.ptr);
							const auto flt = v.stride / 4;
							for (std::uint32_t n = 0; n < v.count; ++n) {
								float x = f[n * flt + 0];
								float y = f[n * flt + 1];
								float z = f[n * flt + 2];
								if (hasWorld) {
									const float wx = world[0] * x + world[4] * y + world[8] * z + world[12];
									const float wy = world[1] * x + world[5] * y + world[9] * z + world[13];
									const float wz = world[2] * x + world[6] * y + world[10] * z + world[14];
									x = wx; y = wy; z = wz;
								}
								a_out.pos[n * 3 + 0] = x;
								a_out.pos[n * 3 + 1] = y;
								a_out.pos[n * 3 + 2] = z;
							}
							a_out.indices.resize(iCount);
							for (std::uint32_t n = 0; n < iCount; ++n) a_out.indices[n] = u[n];
							foundMesh = true;
							REX::INFO("  mesh from idx-content: pos={} idx={} run={} (vCount@+0x{:X} stride={} ptr={:#x} max={} {}ms)",
								a_out.pos.size(), a_out.indices.size(), run, v.countOff, v.stride, ptr, fullMax, meshMs());
						}
					}
					if (!foundMesh) {
						REX::INFO("  idx-content scan: tried={} verts={} ({}ms)", contentTried, verts.size(), meshMs());
					}
				}

				// Buffer-adjacent idx: real index buffers often sit immediately
				// after the vertex allocation (or after its aligned end). 6m
				// proved field/derived idx counts never pair with real verts.
				if (!foundMesh && !verts.empty() && !meshExpired()) {
					constexpr std::uint32_t kAdjOffsets[] = { 0, 4, 8, 16, 32, 64 };
					std::uint32_t adjTried = 0;
					for (const auto& v : verts) {
						if (foundMesh || meshExpired() || adjTried >= 12) break;
						if (v.count < 8 || v.count > 100'000 || v.stride == 0) continue;
						const auto vertBytes = static_cast<std::uintptr_t>(v.count) * v.stride;
						const auto after = v.ptr + vertBytes;
						for (const auto pad : kAdjOffsets) {
							if (foundMesh || meshExpired() || adjTried >= 12) break;
							const auto idxPtr = after + pad;
							if (!CanRead(idxPtr, 64)) continue;
							++adjTried;
							// Content-run FIRST: walk u16 while < v.count to get
							// the natural length — field counts at this location
							// are often wrong (6o: iCount=9 false mesh).
							{
								const auto* ur = reinterpret_cast<const std::uint16_t*>(idxPtr);
								const auto capR = std::min<std::uint32_t>(v.count * 3, 300'000);
								if (CanRead(idxPtr, static_cast<std::size_t>(capR) * 2)) {
									std::uint32_t run = 0;
									std::uint32_t rMax = 0;
									for (std::uint32_t n = 0; n < capR; ++n) {
										if (ur[n] >= v.count) break;
										if (ur[n] > rMax) rMax = ur[n];
										run = n + 1;
									}
									auto runCount = (run / 3) * 3;
									const auto runTri = runCount / 3;
									if (runCount >= 3 && rMax > 0 &&
										runTri + 1u >= v.count / 8u && runTri <= v.count * 4u) {
										a_out.pos.resize(static_cast<std::size_t>(v.count) * 3);
										const auto* f = reinterpret_cast<const float*>(v.ptr);
										const auto flt = v.stride / 4;
										for (std::uint32_t n = 0; n < v.count; ++n) {
											float x = f[n * flt + 0];
											float y = f[n * flt + 1];
											float z = f[n * flt + 2];
											if (hasWorld) {
												const float wx = world[0] * x + world[4] * y + world[8] * z + world[12];
												const float wy = world[1] * x + world[5] * y + world[9] * z + world[13];
												const float wz = world[2] * x + world[6] * y + world[10] * z + world[14];
												x = wx; y = wy; z = wz;
											}
											a_out.pos[n * 3 + 0] = x;
											a_out.pos[n * 3 + 1] = y;
											a_out.pos[n * 3 + 2] = z;
										}
										a_out.indices.resize(runCount);
										for (std::uint32_t n = 0; n < runCount; ++n) a_out.indices[n] = ur[n];
										foundMesh = true;
										REX::INFO("  mesh from adjacent-run: pos={} idx={} run={} (vCount@+0x{:X} stride={} pad={} max={} {}ms)",
											a_out.pos.size(), a_out.indices.size(), run, v.countOff, v.stride, pad, rMax, meshMs());
										break;
									}
								}
							}
							// Sample as u16: need indices < v.count, max>0, no 0xFFFF run.
							// Plausible tri counts: derive from vert count first.
							std::vector<std::uint32_t> tryCounts;
							const auto r3 = [](std::uint32_t x) { return (x / 3) * 3; };
							for (std::uint32_t cand : { r3(v.count), r3(v.count + v.count / 2), r3(v.count * 2), r3(v.count / 2), r3(v.count / 3) }) {
								if (cand >= 3 && cand <= 300'000 && (cand % 3) == 0) {
									bool dup = false;
									for (auto t : tryCounts) if (t == cand) { dup = true; break; }
									if (!dup) tryCounts.push_back(cand);
								}
							}
							// Also accept any field idx count nearby.
							for (const auto* ic : idxCounts) {
								if (tryCounts.size() >= 8) break;
								if (ic->value < 3 || ic->value > 300'000 || (ic->value % 3) != 0) continue;
								bool dup = false;
								for (auto t : tryCounts) if (t == ic->value) { dup = true; break; }
								if (!dup) tryCounts.push_back(ic->value);
							}
							for (const auto iCount : tryCounts) {
								if (foundMesh || meshExpired()) break;
								// 6o false mesh: iCount=9 passed for v=5077 because
								// buffer-adjacent had no ratio check.
								{
									const auto tri = iCount / 3;
									if (tri + 1u < v.count / 8u || tri > v.count * 4u) continue;
								}
								if (!CanRead(idxPtr, static_cast<std::size_t>(iCount) * 2)) continue;
								const auto* u = reinterpret_cast<const std::uint16_t*>(idxPtr);
								const auto nSample = std::min<std::uint32_t>(iCount, 64);
								bool ok = true;
								std::uint32_t maxIdx = 0;
								for (std::uint32_t n = 0; n < nSample; ++n) {
									if (u[n] == 0xFFFF || u[n] >= v.count) { ok = false; break; }
									maxIdx = std::max(maxIdx, static_cast<std::uint32_t>(u[n]));
								}
								if (!ok || maxIdx == 0) continue;
								// Full validate
								const auto* uf = reinterpret_cast<const std::uint16_t*>(idxPtr);
								bool fullOk = true;
								std::uint32_t fullMax = 0;
								for (std::uint32_t n = 0; n < iCount; ++n) {
									if (uf[n] >= v.count) { fullOk = false; break; }
									fullMax = std::max(fullMax, static_cast<std::uint32_t>(uf[n]));
								}
								if (!fullOk || fullMax == 0) continue;
								a_out.pos.resize(static_cast<std::size_t>(v.count) * 3);
								const auto* f = reinterpret_cast<const float*>(v.ptr);
								const auto flt = v.stride / 4;
								for (std::uint32_t n = 0; n < v.count; ++n) {
									float x = f[n * flt + 0];
									float y = f[n * flt + 1];
									float z = f[n * flt + 2];
									if (hasWorld) {
										const float wx = world[0] * x + world[4] * y + world[8] * z + world[12];
										const float wy = world[1] * x + world[5] * y + world[9] * z + world[13];
										const float wz = world[2] * x + world[6] * y + world[10] * z + world[14];
										x = wx; y = wy; z = wz;
									}
									a_out.pos[n * 3 + 0] = x;
									a_out.pos[n * 3 + 1] = y;
									a_out.pos[n * 3 + 2] = z;
								}
								a_out.indices.resize(iCount);
								for (std::uint32_t n = 0; n < iCount; ++n) a_out.indices[n] = uf[n];
								foundMesh = true;
								REX::INFO("  mesh from buffer-adjacent idx: pos={} idx={} (vCount@+0x{:X} stride={} pad={} iCount={} {}ms)",
									a_out.pos.size(), a_out.indices.size(), v.countOff, v.stride, pad, iCount, meshMs());
							}
						}
					}
					if (!foundMesh) {
						REX::INFO("  buffer-adjacent idx: tried={} verts={} ({}ms)", adjTried, verts.size(), meshMs());
					}
				}

				if (!foundMesh) {
					REX::INFO("  cross-pair empty: verts={} inds={} vCands={} iCands={} probes={} attempts={} rejV={} rejRatio={} rejI={} rejIdx={} ({}ms)",
						verts.size(), inds.size(), vertCounts.size(), idxCounts.size(),
						allPtrs.size(), attempts, rejectFullV, rejectRatio, rejectFullI, rejectIdx, meshMs());
					// One-shot: what do the probes actually point at? First 3
					// empties dump the first 10 ptrs' leading floats — grounds
					// whether probe targets are buffers vs object headers.
					{
						static int s_pd = 0;
						if (s_pd < 3) {
							++s_pd;
							const auto nDump = std::min<std::size_t>(10, allPtrs.size());
							for (std::size_t di = 0; di < nDump; ++di) {
								const auto p = allPtrs[di];
								if (!CanRead(p, 24)) {
									REX::INFO("    probe[{}] {:#x}: !CanRead", di, p);
									continue;
								}
								const auto* f = reinterpret_cast<const float*>(p);
								REX::INFO("    probe[{}] {:#x}: {:.3f} {:.3f} {:.3f} | {:.3f} {:.3f} {:.3f}",
									di, p, f[0], f[1], f[2], f[3], f[4], f[5]);
							}
						}
					}
					// One-shot: why is verts empty? List first vert-count cands.
					{
						static bool s_vc = false;
						if (!s_vc) {
							s_vc = true;
							std::size_t shown = 0;
							for (const auto* c : vertCounts) {
								if (shown++ >= 8) break;
								REX::INFO("    vertCount cand +0x{:X} = {}", c->off, c->value);
							}
							if (vertCounts.empty()) REX::INFO("    vertCount cands EMPTY");
							for (const auto* c : idxCounts) {
								REX::INFO("    idxCount cand +0x{:X} = {}", c->off, c->value);
							}
							if (idxCounts.empty()) REX::INFO("    idxCount cands EMPTY");
						}
					}
					if (!verts.empty()) {
						REX::INFO("    vert sample: count={} off=+0x{:X} ptr={:#x} stride={} extent={:.3f}",
							verts[0].count, verts[0].countOff, verts[0].ptr, verts[0].stride, verts[0].extent);
						// First 6 floats — confirms real position data vs header.
						if (CanRead(verts[0].ptr, 24)) {
							const auto* f = reinterpret_cast<const float*>(verts[0].ptr);
							REX::INFO("    vert floats: {:.3f} {:.3f} {:.3f} {:.3f} {:.3f} {:.3f}",
								f[0], f[1], f[2], f[3], f[4], f[5]);
						}
						// Top-3 verts by extent — shows if smaller real meshes exist.
						const auto nShow = std::min<std::size_t>(3, verts.size());
						for (std::size_t vi = 0; vi < nShow; ++vi) {
							REX::INFO("    vert[{}] count={} extent={:.3f} stride={} off=+0x{:X}",
								vi, verts[vi].count, verts[vi].extent, verts[vi].stride, verts[vi].countOff);
						}
					}
					if (!inds.empty()) {
						REX::INFO("    idx sample: count={} off=+0x{:X} ptr={:#x} u16={}",
							inds[0].count, inds[0].countOff, inds[0].ptr, inds[0].u16);
					}
				}
			}

			// ── 3) Embedded object at +0x130 ──────────────────────────────────
			if (!foundMesh && !meshExpired() && CanRead(base + 0x130, 8)) {
				const auto embVt = *reinterpret_cast<const std::uintptr_t*>(base + 0x130);
				if (inRdata(embVt)) {
					REX::INFO("  embedded obj at base+0x130 RTTI='{}' ({}ms)",
						DemangleRTTI(ResolveRTTIName(embVt)), meshMs());
					if (TryParseMeshData(base + 0x130, a_out, world, hasWorld, 0x08, 0x1C0)) {
						foundMesh = true;
						REX::INFO("  mesh from embedded +0x130: pos={} uv={} idx={}",
							a_out.pos.size(), a_out.uv.size(), a_out.indices.size());
					}
				}
			}

			// ── 4) Geo field blob — mesh-budget only ──────────────────────────
			if (!foundMesh && !meshExpired() &&
				TryParseMeshData(base, a_out, world, hasWorld, 0x130, maxOff)) {
				foundMesh = true;
				REX::INFO("  mesh from geo field blob: pos={} uv={} idx={}",
					a_out.pos.size(), a_out.uv.size(), a_out.indices.size());
			}

			// ── 5) Mat/tex peek AFTER mesh — few VQs, only if still room ─────
			// (was interleaved with Phase B and starved mesh of VirtualQuery budget)
			if (!meshExpired() && (a_out.matPath.empty() || a_out.texPath.empty())) {
				std::uint32_t strTried = 0;
				for (const auto& p : heapPtrs) {
					if (meshExpired() || strTried >= 4) break;
					if (!a_out.matPath.empty() && !a_out.texPath.empty()) break;
					if (!CanRead(p.ptr, 8)) continue;
					const auto strPtr = *reinterpret_cast<const std::uintptr_t*>(p.ptr);
					if (!IsValidPtr(strPtr) || !CanRead(strPtr, 128)) continue;
					++strTried;
					const auto* str = reinterpret_cast<const char*>(strPtr);
					if (str[0] < 0x20 || str[0] > 0x7E || str[1] < 0x20 || str[1] > 0x7E) continue;
					std::size_t len = 0;
					while (len < 127 && str[len] >= 0x20 && str[len] <= 0x7E) ++len;
					const std::string candidate(str, len);
					if (a_out.matPath.empty() && LooksLikeMatPath(candidate)) {
						a_out.matPath = candidate;
						REX::INFO("  mat path at +0x{:X}: '{}'", p.off, candidate);
					}
					if (a_out.texPath.empty() && LooksLikeTexturePath(candidate)) {
						a_out.texPath = candidate;
						REX::INFO("  tex path at +0x{:X}: '{}'", p.off, candidate);
					}
				}
			}

			if (!foundMesh) {
				REX::INFO("  no mesh data found in +0x130..+0x{:X} ({}ms setup={}ms mesh={}ms fields={} counts={} expired={})",
					maxOff, ms(), setupMs, meshMs(), heapPtrs.size(), counts.size(), meshExpired());
			}
		}

			// Heuristic mesh-data parser. Reads a candidate object and fills
			// pos/uv/indices only when counts + content look like real geometry
			// (non-degenerate bbox, index count % 3 == 0, indices in range).
			// Candidacy uses sample-sized CanRead only; full-buffer checks
			// happen once immediately before materialize. Attempt-capped so
			// garbage pairs cannot stall. Verbose logs behind kDebugCandidates.
			bool TryParseMeshData(std::uintptr_t a_obj, NativeMesh& a_out,
				const float* a_world, bool a_hasWorld,
				std::uintptr_t a_scanStart, std::uintptr_t a_scanEnd)
			{
				constexpr bool kDebugCandidates = false;
				constexpr std::uint32_t kMaxVerts = 100'000;
				constexpr std::uint32_t kMaxIdx = 300'000;
				constexpr std::uint32_t kMaxCand = 8;
				constexpr std::uint32_t kMaxAttempts = 16;
				const auto t0 = std::chrono::steady_clock::now();
				const auto expired = [&] {
					return std::chrono::steady_clock::now() - t0 >
						std::chrono::milliseconds(25);
				};

				struct Candidate {
					std::uint32_t count;
					std::uintptr_t data;
					std::uintptr_t countOff;
					std::uintptr_t dataOff;
					bool isU16;
					std::uint32_t stride{ 12 };
				};
				std::vector<Candidate> verts;
				std::vector<Candidate> inds;
				std::vector<Candidate> uvs;

				// One window VQ for the object's own field scan — region cache
				// then serves every a_obj+off read (was: VQ per offset ≈13ms).
				const auto scanSpan = a_scanEnd > a_scanStart ? a_scanEnd - a_scanStart : 8;
				if (!CanRead(a_obj + a_scanStart, scanSpan + 0x40)) {
					return false;
				}

				// Sample first N floats for bbox extent — reject degenerate/near-zero.
				// Multi-stride: Starfield verts are interleaved (pos+nrm+uv…).
				const auto sampleVertExtent = [&](std::uintptr_t ptr, std::uint32_t count, float& outExtent, std::uint32_t& outStride) -> bool {
					if (count < 8 || count > kMaxVerts) return false;
					static constexpr std::uint32_t kStrides[] = { 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 64 };
					constexpr std::uint32_t kSample = 4;
					if (!CanRead(ptr, 64)) return false;
					for (const auto stride : kStrides) {
						if (stride * kSample > 64 && !CanRead(ptr, stride * kSample)) continue;
						const auto* baseF = reinterpret_cast<const float*>(ptr);
						const auto flt = stride / 4;
						float lo[3] = { 1e30f, 1e30f, 1e30f };
						float hi[3] = { -1e30f, -1e30f, -1e30f };
						bool finite = true;
						for (std::uint32_t i = 0; i < kSample && finite; ++i) {
							for (int c = 0; c < 3; ++c) {
								const auto v = baseF[i * flt + c];
								if (!std::isfinite(v)) { finite = false; break; }
								lo[c] = std::min(lo[c], v);
								hi[c] = std::max(hi[c], v);
							}
						}
						if (!finite) continue;
						outExtent = std::max({ hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2] });
						if (outExtent > 0.01f && outExtent < 1000.f) {
							outStride = stride;
							return true;
						}
					}
					return false;
				};

				const auto indexCountOk = [](std::uint32_t n) {
					if (n < 3 || n > kMaxIdx || (n % 3) != 0) return false;
					const auto hi = n >> 16;
					const auto lo16 = n & 0xFFFF;
					if (hi != 0 && hi == lo16) return false;
					if (hi == 1 && lo16 == 3) return false;
					if (hi != 0 && lo16 == 0) return false;
					return true;
				};

				// Sample-sized read + non-degenerate first-run (candidacy).
				const auto indexBufOk = [&](std::uintptr_t ptr, std::uint32_t count, bool isU16) -> bool {
					const auto n = std::min<std::uint32_t>(count, 16);
					if (!CanRead(ptr, static_cast<std::size_t>(n) * (isU16 ? 2u : 4u))) return false;
					std::uint32_t maxIdx = 0;
					if (isU16) {
						const auto* u = reinterpret_cast<const std::uint16_t*>(ptr);
						for (std::uint32_t i = 0; i < n; ++i) {
							if (u[i] == 0xFFFF) return false;
							maxIdx = std::max(maxIdx, static_cast<std::uint32_t>(u[i]));
						}
						return maxIdx > 0 && maxIdx < 0xFFFF;
					}
					const auto* u = reinterpret_cast<const std::uint32_t*>(ptr);
					for (std::uint32_t i = 0; i < n; ++i) {
						if (u[i] > kMaxVerts) return false;
						maxIdx = std::max(maxIdx, u[i]);
					}
					return maxIdx > 0;
				};

				// Field scan: object window already validated with ONE CanRead
				// above — raw reads only (per-offset CanRead was the VQ storm).
				// Candidate data ptrs: ONE CanRead each; region cache dedupes
				// when the same ptr is offered for vert/idx/uv roles.
				for (std::uintptr_t off = a_scanStart; off < a_scanEnd; off += 4) {
					if (expired()) return false;
					if (verts.size() >= kMaxCand && inds.size() >= kMaxCand && uvs.size() >= kMaxCand) break;
					const auto lo = *reinterpret_cast<const std::uint32_t*>(a_obj + off);
					if (lo < 3 || lo > kMaxIdx) continue;

					for (std::uintptr_t dOff = 4; dOff <= 24 && off + dOff + 8 <= a_scanEnd + 0x20; dOff += 4) {
						if (expired()) return false;
						if (verts.size() >= kMaxCand && inds.size() >= kMaxCand && uvs.size() >= kMaxCand) break;
						const auto ptr = *reinterpret_cast<const std::uintptr_t*>(a_obj + off + dOff);
						if (!IsValidPtr(ptr) || !CanRead(ptr, 12)) continue;

						// float3 positions with non-degenerate extent
						float extent = 0.f;
						std::uint32_t stride = 0;
						if (verts.size() < kMaxCand &&
							lo >= 8 && lo <= kMaxVerts && sampleVertExtent(ptr, lo, extent, stride)) {
							verts.push_back({ lo, ptr, off, off + dOff, false, stride });
							if (kDebugCandidates) {
								REX::INFO("    verts u32@+0x{:X}={} ptr@+0x{:X}={:#x} extent={:.4f} stride={}",
									off, lo, off + dOff, ptr, extent, stride);
							}
						}

						// u16 indices
						if (inds.size() < kMaxCand && indexCountOk(lo) && indexBufOk(ptr, lo, true)) {
							inds.push_back({ lo, ptr, off, off + dOff, true });
							if (kDebugCandidates) {
								REX::INFO("    idx16 u32@+0x{:X}={} ptr@+0x{:X}={:#x}",
									off, lo, off + dOff, ptr);
							}
						}

						// u32 indices
						if (inds.size() < kMaxCand && indexCountOk(lo) && indexBufOk(ptr, lo, false)) {
							inds.push_back({ lo, ptr, off, off + dOff, false });
							if (kDebugCandidates) {
								REX::INFO("    idx32 u32@+0x{:X}={} ptr@+0x{:X}={:#x}",
									off, lo, off + dOff, ptr);
							}
						}

						// float2 UVs — sample-sized read for candidacy
						if (uvs.size() < kMaxCand && lo >= 8 && lo <= kMaxVerts) {
							const auto sampleUv = std::min<std::uint32_t>(lo, 4);
							if (CanRead(ptr, static_cast<std::size_t>(sampleUv) * 8)) {
								const auto* f = reinterpret_cast<const float*>(ptr);
								bool uvLike = true;
								for (std::uint32_t i = 0; i < sampleUv * 2; ++i) {
									if (!std::isfinite(f[i]) || f[i] < -10.f || f[i] > 10.f) {
										uvLike = false;
										break;
									}
								}
								if (uvLike) {
									uvs.push_back({ lo, ptr, off, off + dOff, false, 8 });
									if (kDebugCandidates) {
										REX::INFO("    uv u32@+0x{:X}={} ptr@+0x{:X}={:#x}",
											off, lo, off + dOff, ptr);
									}
								}
							}
						}
					}
				}

				if (verts.empty() || inds.empty()) {
					return false;
				}

				// Prefer largest vert count, then matching index pairs.
				std::sort(verts.begin(), verts.end(), [](const Candidate& a, const Candidate& b) {
					return a.count > b.count;
				});

				const Candidate* bestV = nullptr;
				const Candidate* bestI = nullptr;
				bool bestU16 = false;
				std::uint32_t attempts = 0;

				for (auto& v : verts) {
					if (attempts >= kMaxAttempts || expired()) break;
					if (v.count > kMaxVerts) continue;
					if (!CanRead(v.data, static_cast<std::size_t>(v.count) * v.stride)) continue;
					for (auto& i : inds) {
						if (attempts >= kMaxAttempts || expired()) break;
						if (i.count % 3 != 0 || i.count > kMaxIdx) continue;
						const auto tri = i.count / 3;
						if (tri + 1u < v.count / 8u || tri > v.count * 4u) continue;
						++attempts;

						// Full buffers must be readable; validate indices fit
						// BEFORE returning so garbage pairs fail here.
						if (!CanRead(i.data, static_cast<std::size_t>(i.count) * (i.isU16 ? 2u : 4u))) continue;
						bool idxOk = true;
						std::uint32_t maxIdx = 0;
						if (i.isU16) {
							const auto* u16 = reinterpret_cast<const std::uint16_t*>(i.data);
							for (std::uint32_t n = 0; n < i.count; ++n) {
								if (u16[n] >= v.count) { idxOk = false; break; }
								maxIdx = std::max(maxIdx, static_cast<std::uint32_t>(u16[n]));
							}
						} else {
							const auto* u32 = reinterpret_cast<const std::uint32_t*>(i.data);
							for (std::uint32_t n = 0; n < i.count; ++n) {
								if (u32[n] >= v.count) { idxOk = false; break; }
								maxIdx = std::max(maxIdx, u32[n]);
							}
						}
						if (!idxOk || maxIdx == 0) continue;

						bestV = &v;
						bestI = &i;
						bestU16 = i.isU16;
						goto found;
					}
				}
				return false;

			found:
				a_out.pos.resize(static_cast<std::size_t>(bestV->count) * 3);
				{
					const auto* f = reinterpret_cast<const float*>(bestV->data);
					const auto flt = bestV->stride / 4;
					for (std::uint32_t i = 0; i < bestV->count; ++i) {
						float x = f[i * flt + 0];
						float y = f[i * flt + 1];
						float z = f[i * flt + 2];
						if (a_hasWorld) {
							const float wx = a_world[0] * x + a_world[4] * y + a_world[8] * z + a_world[12];
							const float wy = a_world[1] * x + a_world[5] * y + a_world[9] * z + a_world[13];
							const float wz = a_world[2] * x + a_world[6] * y + a_world[10] * z + a_world[14];
							x = wx; y = wy; z = wz;
						}
						a_out.pos[i * 3 + 0] = x;
						a_out.pos[i * 3 + 1] = y;
						a_out.pos[i * 3 + 2] = z;
					}
				}

				a_out.indices.resize(bestI->count);
				if (bestU16) {
					const auto* u16 = reinterpret_cast<const std::uint16_t*>(bestI->data);
					for (std::uint32_t i = 0; i < bestI->count; ++i) a_out.indices[i] = u16[i];
				} else {
					const auto* u32 = reinterpret_cast<const std::uint32_t*>(bestI->data);
					for (std::uint32_t i = 0; i < bestI->count; ++i) a_out.indices[i] = u32[i];
				}

			for (auto& u : uvs) {
				if (u.count != bestV->count) continue;
				if (u.count > kMaxVerts) continue;
				if (!CanRead(u.data, static_cast<std::size_t>(u.count) * 8)) continue;
				const auto* f = reinterpret_cast<const float*>(u.data);
				bool ok = true;
				for (std::uint32_t i = 0; i < u.count * 2; ++i) {
					if (!std::isfinite(f[i]) || f[i] < -10.f || f[i] > 10.f) { ok = false; break; }
				}
				if (!ok) continue;
				a_out.uv.resize(static_cast<std::size_t>(u.count) * 2);
				std::memcpy(a_out.uv.data(), f, static_cast<std::size_t>(u.count) * 8);
				break;
			}

			REX::INFO("  TryParseMeshData OK: verts={} tris={} uv={} (v@+0x{:X} i@+0x{:X} u16={} stride={})",
				bestV->count, bestI->count / 3, a_out.uv.size() / 2,
				bestV->countOff, bestI->countOff, bestU16, bestV->stride);
			return true;
		}

		// Build-76 crash guard: a bogus live-buffer probe can yield
		// finite-but-absurd floats (27/09 session: the null-requester Frag
		// Grenade adopt logged center~1e37 r=inf, and rendering that
		// garbage was the last line before the CTD). Real mesh local space
		// never approaches 1e5 units, so anything past that is a probe
		// misread, never authored geometry.
		constexpr float kMaxCoord = 100000.f;

		bool SaneBounds(const float* a_center, float a_radius)
		{
			for (int i = 0; i < 3; ++i) {
				const float v = a_center[i];
				if (!std::isfinite(v) || v < -kMaxCoord || v > kMaxCoord) {
					return false;
				}
			}
			return std::isfinite(a_radius) && a_radius > 0.f && a_radius <= kMaxCoord;
		}

		// Build-78 crash guard: ProbeGeometryNode's deep content scans
		// dereference candidate mesh-data pointers inside foreign objects
		// (session 27/09: AmmoLaserSmall:2 read VPathingCell at +0x750
		// then faulted mid-scan -> CTD). POD-only trampoline: the AV is
		// swallowed per leaf, the walk continues with an empty mesh.
		void ProbeGeometryNodeGuarded(const RE::NiAVObject* a_obj, NativeMesh& a_out, std::uint32_t& a_exc)
		{
			a_exc = 0;
			__try {
				ProbeGeometryNode(a_obj, a_out);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				a_exc = GetExceptionCode();
			}
		}

		void WalkTree(const RE::NiAVObject* a_obj, std::uint32_t& a_nodeCount,
			std::uint32_t& a_geoCount, std::uint32_t& a_triCount,
			std::vector<NativeMesh>& a_meshes, int a_depth = 0)
		{
			if (!a_obj || a_depth > 16) return;
			if (!CanRead(std::bit_cast<std::uintptr_t>(a_obj), 0x130)) return;
			++a_nodeCount;

			const auto name = GetNodeName(a_obj);
			const auto indent = std::string(a_depth * 2, ' ');
			const auto leafRtti = [&]() -> std::string {
				const auto b = std::bit_cast<std::uintptr_t>(a_obj);
				std::uintptr_t vt = 0;
				return SafeReadPtr(b, vt) ? ResolveRTTIName(vt) : std::string();
			};

			if (LooksLikeNiNode(a_obj)) {
				const auto count = ChildrenCount(a_obj);
				const auto* data = ChildrenData(a_obj);
				REX::INFO("SFShopExplorer NativeModel: {} NiNode '{}' children={} rti='{}'", indent, name, count, leafRtti());

				if (data && count > 0 && count < 256) {
					for (std::uint32_t i = 0; i < count; ++i) {
						const auto* child = data[i].get();
						WalkTree(child, a_nodeCount, a_geoCount, a_triCount, a_meshes, a_depth + 1);
					}
				}
				return;
			}

			const std::string rtiName = leafRtti();
			if (rtiName.find("BSGeometry") == std::string::npos) {
				// Build-74 crash guard: only BSGeometry carries vertex/index
				// arrays. A non-geometry leaf (NiCamera, controllers) let the
				// pair-probe accept adjacent-memory garbage (MagShot adopted
				// pos=196632 uv=0 idx=65544 from the ':346' NiCamera) and the
				// render of that garbage was the last line before the CTD.
				REX::INFO("SFShopExplorer NativeModel: {} non-geometry leaf '{}' at node {} rti='{}' - skipped",
					indent, name, a_nodeCount, rtiName);
				return;
			}

			++a_geoCount;
			REX::INFO("SFShopExplorer NativeModel: {} geometry leaf '{}' at node {} rti='{}'",
				indent, name, a_nodeCount, rtiName);

			NativeMesh mesh;
			mesh.name = name;
			std::uint32_t probeExc = 0;
			ProbeGeometryNodeGuarded(a_obj, mesh, probeExc);
			if (probeExc != 0) {
				REX::WARN("SFShopExplorer NativeModel: {} geometry leaf '{}' probe caught SEH 0x{:X} - skipped",
					indent, name, probeExc);
				mesh.pos.clear();
				mesh.uv.clear();
				mesh.nrm.clear();
				mesh.indices.clear();
			}
			if (!mesh.indices.empty()) {
				std::uint32_t maxIdx = 0;
				for (const auto ix : mesh.indices) {
					maxIdx = (std::max)(maxIdx, ix);
				}
				const auto vertCount = mesh.pos.size() / 3;
				if (vertCount == 0 || maxIdx >= vertCount) {
					REX::INFO("SFShopExplorer NativeModel: {} mesh dropped '{}' (idxMax={} verts={})",
						indent, name, maxIdx, vertCount);
				} else {
					// Build-76: index range alone is not enough - probe
					// garbage comes back finite with absurd magnitudes
					// (session 27/09: pos=171348 verts passed the idx check
					// and rendered as center~1e37 r=inf).
					bool sanePos = true;
					for (const float v : mesh.pos) {
						if (!std::isfinite(v) || v < -kMaxCoord || v > kMaxCoord) {
							sanePos = false;
							break;
						}
					}
					if (!sanePos) {
						REX::INFO("SFShopExplorer NativeModel: {} mesh dropped '{}' (insane position)",
							indent, name);
					} else {
						a_triCount += static_cast<std::uint32_t>(mesh.indices.size() / 3);
						a_meshes.push_back(std::move(mesh));
					}
				}
			} else {
				REX::INFO("SFShopExplorer NativeModel: {} geometry leaf EMPTY (pos={} idx={})",
					indent, mesh.pos.size() / 3, mesh.indices.size());
			}
		}

		bool HasNifExtension(const char* str, std::size_t len)
		{
			if (len < 4) return false;
			return str[len - 4] == '.' &&
				(str[len - 3] == 'n' || str[len - 3] == 'N') &&
				(str[len - 2] == 'i' || str[len - 2] == 'I') &&
				(str[len - 1] == 'f' || str[len - 1] == 'F');
		}

		bool LooksLikeModelPath(const char* str, std::size_t len)
		{
			if (len < 8 || len > 260) return false;
			bool printable = true;
			bool hasBackslash = false;
			bool hasDot = false;
			for (std::size_t i = 0; i < len && i < 128; ++i) {
				char c = str[i];
				if (c == '\\') hasBackslash = true;
				if (c == '.') hasDot = true;
				if (c < 0x20 || c > 0x7E) { printable = false; break; }
			}
			return printable && hasBackslash && hasDot;
		}

		// BSFixedString = single BSStringPool::Entry* _data (8 bytes)
		// Entry layout: { Entry* _left, uint32_t _length, uint32_t _refCount, uint8_t _flags, ... string data at Entry+0x18 }
		// String data is at leaf() + 1 = Entry + 0x18 for non-external entries
		bool TryReadBSFixedString(std::uintptr_t a_ptrAddr, std::string& a_out)
		{
			if (!CanRead(a_ptrAddr, 8)) return false;
			const auto entryPtr = *reinterpret_cast<const std::uintptr_t*>(a_ptrAddr);
			if (entryPtr < 0x10000 || entryPtr > 0x7FFFFFFFFFFF) return false;
			if (!CanRead(entryPtr, 0x20)) return false;

			// Read Entry layout. Entry[0] is the pool's _left node pointer; it is
			// not needed here, so it is deliberately not read.
			const auto length = *reinterpret_cast<const std::uint32_t*>(entryPtr + 8);
			const auto refCount = *reinterpret_cast<const std::uint32_t*>(entryPtr + 0x10);
			const auto flags = *reinterpret_cast<const std::uint8_t*>(entryPtr + 0x14);

			if (length < 3 || length > 260) return false;
			if (refCount > 0x10000) return false;

			// For non-external entries, string is at entry+0x18
			if (!(flags & 0x2)) {
				const auto strAddr = entryPtr + 0x18;
				if (!CanRead(strAddr, length + 1)) return false;
				const char* str = reinterpret_cast<const char*>(strAddr);
				bool printable = true;
				for (std::uint32_t c = 0; c < length && c < 64; ++c) {
					if (str[c] < 0x20 || str[c] > 0x7E) { printable = false; break; }
				}
				if (printable) {
					a_out.assign(str, std::min(static_cast<std::size_t>(length), std::size_t(128)));
					return true;
				}
			}
			return false;
		}

		bool IsDummyModelPath(const std::string& a_path)
		{
			std::string lower;
			lower.reserve(a_path.size());
			for (const char c : a_path) {
				lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
			}
			return lower.find("dummy") != std::string::npos;
		}

		// ESM ground truth (Starfield.esm walk, 484 forms): clothes and
		// spacesuit ARMOs carry a SetDressing proxy as their world model
		// (OutfitContainer01.nif / SpaceSuitContainer*.nif - a dressing
		// box). The real garment lives on the ARMA addon links, which
		// FormModelPaths already collects as caller paths. A proxy world
		// model must never render when addons exist.
		bool IsProxyModelPath(const std::string& a_path)
		{
			std::string lower;
			lower.reserve(a_path.size());
			for (const char c : a_path) {
				lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
			}
			return lower.find("setdressing\\") != std::string::npos &&
				lower.find("container") != std::string::npos;
		}

		void ScanForModelPath(std::uintptr_t a_formAddr, std::string& a_out)
		{
			if (!a_out.empty()) return;

			// TESObjectWEAP: BGSModelMaterialSwap at +0xF8, TESModel::model at
			// component+0x08 = form+0x100 (CommonLib: "model BSFixedString at +0x100").
			// WEAP models are tried first; other form types (MISC/AMMO/ALCH) use
			// different component bases — keep the broader scan as fallback.
			static constexpr std::uintptr_t candidates[] = {
				0x100,                    // TESObjectWEAP model
				0xF8,                     // TESObjectMISC/AMMO model
				0xF0, 0x108,              // nearby components
				0xE8, 0xE0, 0xD8,
				0x110, 0x118,
			};

			for (auto off : candidates) {
				std::string tmp;
				if (TryReadBSFixedString(a_formAddr + off, tmp)) {
					REX::INFO("  BSFixedString at form+{:#x}: entry={:#x} -> '{}'",
						off,
						*reinterpret_cast<const std::uintptr_t*>(a_formAddr + off),
						tmp);
					// Commit ONLY on a real path match - previously the
					// first readable string (fullName 'UC AntiXeno' at
					// +0xF0) stayed in a_out and short-circuited every
					// later candidate plus the whole heap scan.
					if (LooksLikeModelPath(tmp.data(), tmp.size()) ||
						HasNifExtension(tmp.data(), tmp.size())) {
						a_out = tmp;
						REX::INFO("  FOUND MODEL PATH at form+{:#x}: '{}'", off, a_out);
						return;
					}
				}
			}

			// Also scan all heap pointers in the form — treat each as a potential
			// BSStringPool::Entry and check if the data at Entry+0x18 is a model path
			for (std::size_t off = 0x38; off + 8 <= 0x280; off += 8) {
				if (!a_out.empty()) break;
				const auto val = *reinterpret_cast<const std::uintptr_t*>(a_formAddr + off);
				if (val < 0x10000 || val > 0x7FFFFFFFFFFF) continue;
				if (!CanRead(val, 0x20)) continue;

				const auto length = *reinterpret_cast<const std::uint32_t*>(val + 8);
				const auto flags = *reinterpret_cast<const std::uint8_t*>(val + 0x14);
				if (length < 8 || length > 260) continue;

				// Non-external: string at entry+0x18
				if (!(flags & 0x2)) {
					const auto strAddr = val + 0x18;
					if (!CanRead(strAddr, length + 1)) continue;
					const char* str = reinterpret_cast<const char*>(strAddr);
					bool printable = true;
					for (std::uint32_t c = 0; c < length && c < 64; ++c) {
						if (str[c] < 0x20 || str[c] > 0x7E) { printable = false; break; }
					}
					if (printable) {
						std::string candidate(str, std::min(static_cast<std::size_t>(length), std::size_t(128)));
						if (LooksLikeModelPath(candidate.data(), candidate.size()) ||
							HasNifExtension(candidate.data(), candidate.size())) {
							a_out = candidate;
							REX::INFO("  FOUND MODEL at form+{:#x} entry={:#x}: '{}'", off, val, a_out);
							return;
						}
					}
				}
			}
		}

		// ── MSVC RTTI name resolver (vtable → mangled type name) ─────────────

		std::string ResolveRTTIName(std::uintptr_t a_vtable)
		{
			// Fault-gated, not merely VirtualQuery-gated: a closing menu frees
			// its vtable-adjacent COL while the task thread is still reading it.
			std::uintptr_t col = 0;
			if (!SafeReadPtr(a_vtable - 8, col)) return {};
			if (!IsValidPtr(col) || !CanRead(col, 0x18)) return {};

			// x64: typeDescriptor at COL+0x0C is an RVA relative to *the module that owns the COL*
			std::uint32_t typeDescRVA = 0;
			if (!SafeReadU32(col + 0x0C, typeDescRVA)) return {};
			if (typeDescRVA == 0) return {};

			HMODULE colMod = nullptr;
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCWSTR>(col), &colMod) ||
			    !colMod) {
				return {};
			}
			const auto modBase = std::bit_cast<std::uintptr_t>(colMod);
			const auto typeDesc = modBase + typeDescRVA;
			if (!CanRead(typeDesc, 0x18)) return {};

			// MSVC TypeDescriptor: +0x00 vftable, +0x08 spare, +0x10 name[]
			char buf[256]{};
			if (!SafeCopyAscii(typeDesc + 0x10, buf, 255)) return {};
			return std::string(buf);
		}

		// Demangle common MSVC patterns: .?AVFoo@@ → Foo, .?AVBSTArray_... → BSTArray_...
		std::string DemangleRTTI(const std::string& a_mangled)
		{
			std::string s = a_mangled;
			// Strip leading .?AV / .?AU / .?AX prefixes
			if (s.rfind(".?A", 0) == 0 && s.size() > 3) {
				s = s.substr(3);
			}
			// Strip trailing @@
			while (s.size() >= 2 && s[s.size() - 1] == '@' && s[s.size() - 2] == '@') {
				s.erase(s.size() - 2);
			}
			// Collapse trailing single @ separators used in nested names
			while (!s.empty() && s.back() == '@') s.pop_back();
			// Replace remaining @ separators with ::
			for (auto& c : s) { if (c == '@') c = ':'; }
			return s;
		}

		// ── Main entry ──────────────────────

		// Clone3D one path into the form's TESModel field (form+0x100 for WEAP),
		// extract geometry, then restore the original BSFixedString.
		// Leaves the form unchanged even if Clone3D throws/nulls.
		PreviewResult Clone3DSwapPath(std::uint32_t a_formID, RE::TESBoundObject* a_bound,
			std::uintptr_t a_weapAddr, const std::string& a_path)
		{
			PreviewResult result;
			auto* player = RE::PlayerCharacter::GetSingleton();

			auto* modelField = reinterpret_cast<RE::BSFixedString*>(a_weapAddr + 0x100);
			RE::BSFixedString saved = *modelField;
			*modelField = a_path.c_str();
			REX::INFO("SFShopExplorer NativeModel: swap form+0x100 -> '{}' (saved '{}')",
				a_path, saved.c_str());

			RE::NiPointer<RE::NiAVObject> obj3D;
			a_bound->Clone3D(player, obj3D);

			*modelField = saved;

			if (obj3D) {
				result.loaded = true;
				g_cachedFormID = a_formID;
				g_cachedObj = obj3D;
				RE::NiUpdateData updateData{};
				updateData.flags = 0x10;
				obj3D->UpdateWorldData(&updateData);
				ReadBounding(obj3D.get(), result.center, result.radius);
				WalkTree(obj3D.get(), result.nodeCount, result.geoCount, result.triCount, result.meshes);
				result.extracted = !result.meshes.empty();
				result.message = result.meshes.empty()
					? std::format("swap Clone3D '{}' OK but no geometry", a_path)
					: std::format("{} meshes ({})", result.meshes.size(), a_path);
			} else {
				result.message = std::format("swap Clone3D '{}' returned null", a_path);
			}
			return result;
		}

		// Find any WEAP bound object to host a temporary +0x100 model-string
		// swap (armor Clone3D returns the race skeleton, so the armor NIF gets
		// loaded through a weapon form instead). Content-driven array scan,
		// same proven heuristic as ShopExplorer RefreshRuntimeFormArrays:
		// CommonLib's typed TESDataHandler::formArrays offsets are stale for
		// this Starfield build, so validate raw BSTArray headers instead.
		RE::TESBoundObject* FindHelperWeapon(std::uintptr_t& a_addr, std::uint32_t& a_id)
		{
			a_addr = 0;
			a_id = 0;
			auto* data = RE::TESDataHandler::GetSingleton();
			if (!data) {
				return nullptr;
			}
			const auto base = std::bit_cast<std::uintptr_t>(data);
			for (std::uintptr_t offset = 0x0E0; offset < 0x1510; offset += 0x18) {
				const auto size = *reinterpret_cast<const std::uint32_t*>(base + offset);
				const auto capacity = *reinterpret_cast<const std::uint32_t*>(base + offset + 4);
				const auto arrayData = *reinterpret_cast<const std::uintptr_t*>(base + offset + 8);
				if (size == 0 || size > 200000 || capacity < size) {
					continue;
				}
				if (arrayData < 0x10000000000 || arrayData >= 0x800000000000) {
					continue;
				}
				const auto* elements = reinterpret_cast<const RE::TESForm* const*>(arrayData);
				RE::TESForm* first = nullptr;
				const auto probe = std::min<std::uint32_t>(size, 8);
				for (std::uint32_t i = 0; i < probe && !first; ++i) {
					auto* form = const_cast<RE::TESForm*>(elements[i]);
					if (!form) {
						continue;
					}
					const auto vtable = *reinterpret_cast<const std::uintptr_t*>(form);
					if (vtable < 0x7FF000000000 || vtable >= 0x800000000000) {
						break;
					}
					first = form;
				}
				if (!first || first->GetFormType() != RE::FormType::kWEAP) {
					continue;
				}
				for (std::uint32_t i = 0; i < size; ++i) {
					auto* form = const_cast<RE::TESForm*>(elements[i]);
					if (!form || form->GetFormType() != RE::FormType::kWEAP || !form->IsBoundObject()) {
						continue;
					}
					a_addr = std::bit_cast<std::uintptr_t>(form);
					a_id = form->GetFormID();
					return static_cast<RE::TESBoundObject*>(form);
				}
			}
			return nullptr;
		}

		// Parse one NIF's external geometry stream (the BSGeo LOD GUID ->
		// geometries/<guid>.mesh archive entry) into NativeMeshes. Shared by
		// the direct-branch Stage A2 and the dummy-branch assembly fallback.
		// Returns adopted count; a_tri accumulates triangle totals.
		std::size_t AdoptFileMeshes(const std::string& a_modelPath, std::vector<NativeMesh>& a_out,
			std::uint32_t& a_tri)
		{
			std::vector<SFE::LiveModel::LiveMesh> fileMeshes;
			if (!SFE::LiveModel::ParseNifMeshes(a_modelPath, fileMeshes)) {
				return 0;
			}
			std::size_t added = 0;
			for (auto& lm : fileMeshes) {
				if (lm.pos.empty() || lm.indices.empty()) {
					continue;
				}
				NativeMesh mesh;
				mesh.name = std::move(lm.name);
				mesh.matPath = std::move(lm.matPath);
				mesh.pos = std::move(lm.pos);
				mesh.uv = std::move(lm.uv);
				mesh.indices = std::move(lm.indices);
				a_tri += static_cast<std::uint32_t>(mesh.indices.size() / 3);
				a_out.push_back(std::move(mesh));
				++added;
			}
			return added;
		}

		// Center/radius straight from mesh positions. OfflineModel recomputes
		// its own bounds for rendering; this only keeps the probe log honest
		// (the helmet's ReadBounding returned r=0.0).
		void ComputeMeshBounds(const std::vector<NativeMesh>& a_meshes, float* a_center, float& a_radius)
		{
			float lo[3]{ 0.f, 0.f, 0.f };
			float hi[3]{ 0.f, 0.f, 0.f };
			bool have = false;
			for (const auto& m : a_meshes) {
				for (std::size_t v = 0; v + 2 < m.pos.size(); v += 3) {
					for (int c = 0; c < 3; ++c) {
						const float val = m.pos[v + c];
						if (!have) {
							lo[c] = hi[c] = val;
						} else {
							lo[c] = (std::min)(lo[c], val);
							hi[c] = (std::max)(hi[c], val);
						}
					}
					have = true;
				}
			}
			if (!have) {
				return;
			}
			for (int c = 0; c < 3; ++c) {
				a_center[c] = (lo[c] + hi[c]) * 0.5f;
			}
			const float dx = hi[0] - lo[0];
			const float dy = hi[1] - lo[1];
			const float dz = hi[2] - lo[2];
			a_radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
			if (a_radius <= 0.f) {
				a_radius = 1.f;
			}
		}

		// ------------------------------------------------------------------
		// Build-81: BSConnectPoint assembly placement.
		//
		// Build-80 gave every OMOD part an identity root, so all parts
		// merged at the receiver origin and buried origin-centered meshes
		// inside the receiver body. The NIFs themselves carry the mounting
		// graph: each part declares "C-<Slot>" children, mounted hosts
		// declare "P-<Slot>" parents on a scene node, and the part's root
		// transform is parentPlacement * hostNodeWorld * entryTransform.
		// The C-Receiver holder (or path 0) stays at identity. Chains
		// resolve iteratively (receiver -> barrel -> muzzle/scope). Any
		// part without a host keeps identity = build-80 behavior, logged.
		// Whole algorithm validated offline: dry-run over all 657 base-game
		// weapon NIFs placed every part of all 13 assemblies, zero
		// fallbacks, with attach points matching receiver geometry to <1mm.
		// ------------------------------------------------------------------
		using PlaceMat = std::array<float, 16>;
		constexpr PlaceMat kPlaceIdent{ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

		PlaceMat MulPlace(const PlaceMat& a, const PlaceMat& b)
		{
			PlaceMat r{};
			for (int i = 0; i < 4; ++i) {
				for (int j = 0; j < 4; ++j) {
					r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] +
						a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
				}
			}
			return r;
		}

		bool IsPlaceIdent(const PlaceMat& a_m)
		{
			for (int i = 0; i < 16; ++i) {
				if (std::fabs(a_m[i] - kPlaceIdent[i]) > 1e-5f) {
					return false;
				}
			}
			return true;
		}

		// p' = M*p for every position from a_from on; normals rotate (no
		// translation) and renormalize. uv/indices untouched.
		void TransformMeshes(std::vector<NativeMesh>& a_meshes, std::size_t a_from, const PlaceMat& a_m)
		{
			if (IsPlaceIdent(a_m) || a_from >= a_meshes.size()) {
				return;
			}
			for (std::size_t mi = a_from; mi < a_meshes.size(); ++mi) {
				auto& mesh = a_meshes[mi];
				for (std::size_t v = 0; v + 2 < mesh.pos.size(); v += 3) {
					const float x = mesh.pos[v], y = mesh.pos[v + 1], z = mesh.pos[v + 2];
					mesh.pos[v + 0] = a_m[0] * x + a_m[1] * y + a_m[2] * z + a_m[3];
					mesh.pos[v + 1] = a_m[4] * x + a_m[5] * y + a_m[6] * z + a_m[7];
					mesh.pos[v + 2] = a_m[8] * x + a_m[9] * y + a_m[10] * z + a_m[11];
				}
				for (std::size_t v = 0; v + 2 < mesh.nrm.size(); v += 3) {
					const float x = mesh.nrm[v], y = mesh.nrm[v + 1], z = mesh.nrm[v + 2];
					float nx = a_m[0] * x + a_m[1] * y + a_m[2] * z;
					float ny = a_m[4] * x + a_m[5] * y + a_m[6] * z;
					float nz = a_m[8] * x + a_m[9] * y + a_m[10] * z;
					const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
					if (len > 1e-8f) {
						nx /= len;
						ny /= len;
						nz /= len;
					} else {
						nx = 0.f;
						ny = 1.f;
						nz = 0.f;
					}
					mesh.nrm[v + 0] = nx;
					mesh.nrm[v + 1] = ny;
					mesh.nrm[v + 2] = nz;
				}
			}
		}

		std::string BaseName(const std::string& a_path)
		{
			const auto s = a_path.find_last_of("/\\");
			return s == std::string::npos ? a_path : a_path.substr(s + 1);
		}

		// One placement pass per assembly load. Mirrors the offline
		// validator exactly: C-Receiver holder (case-insensitive - magshot
		// ships a lowercase key) is the identity root, hosts are keyed
		// first-wins in placement order, parts resolve in path order with
		// hosts from earlier placements visible in the same pass.
		void ResolveAssemblyPlaces(const std::vector<std::string>& a_paths, std::vector<PlaceMat>& a_places)
		{
			const std::size_t n = a_paths.size();
			a_places.assign(n, kPlaceIdent);
			if (n < 2) {
				return;
			}
			std::vector<SFE::LiveModel::ConnectInfo> conns(n);
			for (std::size_t i = 0; i < n; ++i) {
				SFE::LiveModel::ParseNifConnects(a_paths[i], conns[i]);
			}
			std::size_t root = 0;
			for (std::size_t i = 0; i < n; ++i) {
				bool hit = false;
				for (const auto& c : conns[i].childs) {
					if (c.size() == 10 && std::equal(c.begin(), c.end(), "C-Receiver",
							[](char a, char b) {
								return std::tolower(static_cast<unsigned char>(a)) ==
									std::tolower(static_cast<unsigned char>(b));
							})) {
						root = i;
						hit = true;
						break;
					}
				}
				if (hit) {
					break;
				}
			}
			std::vector<bool> placed(n, false);
			placed[root] = true;
			std::unordered_map<std::string, PlaceMat> hosts;
			const auto addHosts = [&](std::size_t i) {
				for (const auto& pt : conns[i].parents) {
					if (hosts.count(pt.name) != 0) {
						continue;
					}
					const auto nit = conns[i].nodeWorlds.find(pt.node);
					const PlaceMat& nw =
						nit != conns[i].nodeWorlds.end() ? nit->second : conns[i].rootWorld;
					hosts.emplace(pt.name, MulPlace(a_places[i], MulPlace(nw, pt.m)));
				}
			};
			addHosts(root);
			REX::INFO("SFShopExplorer NativeModel: connect root '{}' ({} paths)",
				BaseName(a_paths[root]), n);
			bool progress = true;
			while (progress) {
				progress = false;
				for (std::size_t i = 0; i < n; ++i) {
					if (placed[i]) {
						continue;
					}
					for (const auto& c : conns[i].childs) {
						if (c.empty()) {
							continue;
						}
						const auto it = hosts.find("P" + c.substr(1));
						if (it == hosts.end()) {
							continue;
						}
						a_places[i] = it->second;
						placed[i] = true;
						progress = true;
						const auto& t = it->second;
						REX::INFO("SFShopExplorer NativeModel: connect place '{}' {} -> {} t=({:.3f},{:.3f},{:.3f})",
							BaseName(a_paths[i]), c, it->first, t[3], t[7], t[11]);
						addHosts(i);
						break;
					}
				}
			}
			for (std::size_t i = 0; i < n; ++i) {
				if (placed[i] || conns[i].childs.empty()) {
					continue;
				}
				std::string kids;
				for (const auto& c : conns[i].childs) {
					if (!kids.empty()) {
						kids.push_back(',');
					}
					kids += c;
				}
				REX::INFO("SFShopExplorer NativeModel: connect '{}' no host for {} - identity root",
					BaseName(a_paths[i]), kids);
			}
		}

		PreviewResult LoadFromForm_Clone3D(std::uint32_t a_formID, const std::vector<std::string>& a_modelPaths)
		{
			PreviewResult result;

			auto* form = RE::TESForm::LookupByID(a_formID);
			if (!form) { result.message = "form not found"; return result; }

			const auto formType = form->GetFormType();
			const auto weapAddr = std::bit_cast<std::uintptr_t>(form);
			const auto vtableBase = *reinterpret_cast<const std::uintptr_t*>(weapAddr);

			REX::INFO("SFShopExplorer NativeModel: form {:08X} type={} vtable={:#x}",
				a_formID, static_cast<int>(formType), vtableBase);

			// Step 1: Call GetFormEditorID via vtable slot 0x4D
			const char* editorID = nullptr;
			if (CanRead(vtableBase + 0x4D * 8, 8)) {
				using GetEditorID_t = const char* (__fastcall*)(void*);
				auto fn4D = *reinterpret_cast<const GetEditorID_t*>(vtableBase + 0x4D * 8);
				if (fn4D) {
					editorID = fn4D(form);
					if (editorID && CanRead(std::bit_cast<std::uintptr_t>(editorID), 4)) {
						REX::INFO("SFShopExplorer NativeModel: editorID = '{}'", editorID);
					}
				}
			}

			// Step 4: Search for model path — scan form+0xF0 as BSFixedString
			std::string modelPath;
			REX::INFO("SFShopExplorer NativeModel: === searching for model path ===");
			ScanForModelPath(weapAddr, modelPath);

			if (!modelPath.empty()) {
				REX::INFO("SFShopExplorer NativeModel: MODEL PATH = '{}'", modelPath);
			} else {
				REX::INFO("SFShopExplorer NativeModel: no model path found");
			}

			// For inventory-like object forms, the typed TESModel component
			// collected by FormModelPaths is authoritative. The raw memory
			// scan above was designed around WEAP offsets and can select an
			// unrelated BSFixedString on MISC/ALCH/BOOK records.
			const bool preferItemNif = formType == RE::FormType::kMISC ||
				formType == RE::FormType::kALCH ||
				formType == RE::FormType::kBOOK ||
				formType == RE::FormType::kKEYM;
			if (preferItemNif && !a_modelPaths.empty() && !a_modelPaths.front().empty()) {
				modelPath = a_modelPaths.front();
				REX::INFO("SFShopExplorer NativeModel: typed item model path = '{}'", modelPath);
			}

			// Full BSFixedString map of the form (once per load): shows every
			// readable slot in the 0x100..0x378 band so missing model/world-model
			// fields are diagnosable from the log instead of guessed.
			for (std::uintptr_t off = 0x100; off <= 0x378; off += 8) {
				std::string slot;
				if (TryReadBSFixedString(weapAddr + off, slot)) {
					REX::INFO("SFShopExplorer NativeModel: formstr form+{:#x} = '{}'", off, slot);
				}
			}

			// Step 5: Clone3D — dummy base path → swap in FormModelPaths NIFs
			auto* boundObj = static_cast<RE::TESBoundObject*>(form);
			auto* player = RE::PlayerCharacter::GetSingleton();

			const bool baseDummy = !modelPath.empty() && IsDummyModelPath(modelPath);
			REX::INFO("SFShopExplorer NativeModel: baseDummy={} assemblyPaths={}",
				baseDummy, a_modelPaths.size());

			// Item forms often expose a generic or cached Clone3D tree, while
			// the record's NIF and its external geometry stream identify the
			// selected object's actual mesh. Prefer that deterministic source
			// for MISC/Aid/books/keys; if parsing fails, retain Clone3D fallback.
			if (preferItemNif && !baseDummy && !a_modelPaths.empty()) {
				std::uint32_t fileTri = 0;
				const auto added = AdoptFileMeshes(modelPath, result.meshes, fileTri);
				if (added > 0) {
					result.geoCount = static_cast<std::uint32_t>(added);
					result.triCount = fileTri;
					result.extracted = true;
					ComputeMeshBounds(result.meshes, result.center, result.radius);
					result.message = std::format("{} meshes (typed item NIF)", added);
					REX::INFO("SFShopExplorer NativeModel: typed item NIF preferred: {} meshes, {} tris from '{}' (Clone3D bypassed)",
						added, fileTri, modelPath);
					return result;
				}
			}

			// Preserve the original non-dummy fallback flow. If the typed item
			// NIF parser did not produce meshes, proceed through Clone3D and the
			// existing file-parser fallback; successful item NIFs returned above
			// before any WEAP-specific form+0x100 model swap can run.
			if (!baseDummy) {
				RE::NiPointer<RE::NiAVObject> obj3D;
				boundObj->Clone3D(player, obj3D);
				if (obj3D) {
					result.loaded = true;
					g_cachedFormID = a_formID;
					g_cachedObj = obj3D;
					REX::INFO("SFShopExplorer NativeModel: Clone3D OK root={:#x}",
						*reinterpret_cast<const std::uintptr_t*>(obj3D.get()));

					RE::NiUpdateData updateData{};
					updateData.flags = 0x10;
					obj3D->UpdateWorldData(&updateData);
					ReadBounding(obj3D.get(), result.center, result.radius);
					WalkTree(obj3D.get(), result.nodeCount, result.geoCount, result.triCount, result.meshes);
					if (!result.meshes.empty() && !SaneBounds(result.center, result.radius)) {
						// Build-76: the NIF's stored bounding can be probe
						// garbage while the positions are fine - recompute.
						ComputeMeshBounds(result.meshes, result.center, result.radius);
					}
					result.extracted = !result.meshes.empty();
					result.message = result.meshes.empty()
						? "Clone3D OK but no geometry"
						: std::format("{} meshes", result.meshes.size());
				} else {
					result.message = "Clone3D returned null";
				}

				// Stage A: ARMO Clone3D with a live requester returned the RACE
				// skeleton (geometry-less, race ANAM skeleton.nif). Retry with a
				// null requester to test whether the output is requester-driven.
				if (result.meshes.empty() && result.loaded) {
					RE::NiPointer<RE::NiAVObject> retryObj;
					boundObj->Clone3D(nullptr, retryObj);
					if (retryObj) {
						std::vector<NativeMesh> retryMeshes;
						RE::NiUpdateData updateData{};
						updateData.flags = 0x10;
						retryObj->UpdateWorldData(&updateData);
						std::uint32_t retryNodes = 0;
						std::uint32_t retryGeo = 0;
						std::uint32_t retryTri = 0;
						WalkTree(retryObj.get(), retryNodes, retryGeo, retryTri, retryMeshes);
						REX::INFO("SFShopExplorer NativeModel: null requester Clone3D nodes={} geo={} tri={}",
							retryNodes, retryGeo, retryTri);
						if (!retryMeshes.empty()) {
							result.nodeCount = retryNodes;
							result.geoCount = retryGeo;
							result.triCount = retryTri;
							result.meshes = std::move(retryMeshes);
							ReadBounding(retryObj.get(), result.center, result.radius);
							if (result.radius <= 0.f) {
								ComputeMeshBounds(result.meshes, result.center, result.radius);
							}
							if (!SaneBounds(result.center, result.radius)) {
								// Build-76: probe garbage (finite absurd
								// floats) - discard so Stage A2 (geometry
								// files) gets its chance instead.
								REX::INFO("SFShopExplorer NativeModel: null requester bounds rejected (center=({:.1f},{:.1f},{:.1f}) r={:.1f}) - {} mesh(es) discarded",
									result.center[0], result.center[1], result.center[2], result.radius,
									result.meshes.size());
								result.meshes.clear();
								result.extracted = false;
								result.geoCount = 0;
								result.triCount = 0;
							} else {
								result.extracted = true;
								g_cachedObj = retryObj;
								result.message = std::format("{} meshes (null requester)", result.meshes.size());
							}
						}
					}
				}

				// Stage A2: Starfield stores vertex/index data OUTSIDE the NIF.
				// Each BSGeo LOD entry holds a 41-byte GUID key (hex\hex)
				// resolved to geometries/<guid>.mesh - a separate archive
				// stream the runtime loads through MeshLODDB stream entries,
				// which is why the buffer probe on the cloned tree pairs
				// nothing (counts readable, arrays unreachable). Parse the
				// same bytes the engine itself streams: own NIF parser for
				// the keys, own .mesh LOD0 parser for the data. Deterministic
				// file path, no engine calls, no [livemodel] enabled gate.
				const bool proxyWorld = IsProxyModelPath(modelPath);

				// A Clone3D extraction with no UV stream cannot be textured: the
				// render binds a white map and the preview shows a plain shape.
				// 3kV LZR Cartridge was exactly that - 1 mesh, uv=0, so 1 range
				// and 0 prefetched - because Clone3D answered with geometry and
				// Stage A2 (the NIF/.mesh files, which carry UVs) was skipped.
				// The files are the authority for UVs, so a UV-less clone is
				// treated like a failed clone and the files get their chance.
				// The clone is kept and restored if the files parse to nothing,
				// so this can never cost an item its geometry.
				std::vector<NativeMesh> uvlessClone;
				const auto             hasUv = [](const NativeMesh& a_mesh) { return a_mesh.uv.size() >= 6; };
				const bool cloneUvless = !result.meshes.empty() &&
					!std::any_of(result.meshes.begin(), result.meshes.end(), hasUv);
				if (cloneUvless) {
					REX::INFO("SFShopExplorer NativeModel: Clone3D gave {} mesh(es) with no UV stream - deferring to geometry files",
						result.meshes.size());
					uvlessClone = result.meshes;
					result.meshes.clear();
					result.extracted = false;
					result.geoCount = 0;
					result.triCount = 0;
				}
				if (proxyWorld && !a_modelPaths.empty() && !result.meshes.empty()) {
					// The walk cloned the SetDressing dressing box - drop it,
					// the ARMA addon parts below replace it.
					REX::INFO("SFShopExplorer NativeModel: proxy world '{}' ({} meshes) replaced by {} addon path(s)",
						modelPath, result.meshes.size(), a_modelPaths.size());
					result.meshes.clear();
					result.extracted = false;
					result.geoCount = 0;
					result.triCount = 0;
				}
				if (result.meshes.empty()) {
					// Candidate policy: a proxy world form renders ALL caller
					// addon paths merged (clothes = upper+lower+sleeves, suits
					// = up to 9 pieces, all authored in one bind pose); a real
					// world model keeps its single path first (the approved
					// GO.nif look) and only falls back to the addon parts when
					// the world parse fails. Proxy strings never run.
					std::vector<std::string> cands;
					const auto pushCand = [&cands](const std::string& p) {
						if (p.empty() || IsDummyModelPath(p) || IsProxyModelPath(p)) {
							return;
						}
						for (const auto& seen : cands) {
							if (seen.size() == p.size() &&
								std::equal(seen.begin(), seen.end(), p.begin(),
									[](char a, char b) {
										return std::tolower(static_cast<unsigned char>(a)) ==
											std::tolower(static_cast<unsigned char>(b));
									})) {
								return;
							}
						}
						cands.push_back(p);
					};
					if (proxyWorld) {
						for (const auto& p : a_modelPaths) {
							pushCand(p);
						}
						if (cands.empty() && !modelPath.empty()) {
							cands.push_back(modelPath);  // no addons known: box fallback
						}
					} else {
						pushCand(modelPath);
					}
					std::uint32_t fileTri = 0;
					bool any = false;
					const auto adoptOne = [&](const std::string& p) {
						std::uint32_t partTri = 0;
						const auto added = AdoptFileMeshes(p, result.meshes, partTri);
						if (added > 0) {
							any = true;
							fileTri += partTri;
							result.extracted = true;
							REX::INFO("SFShopExplorer NativeModel: geometry files: {} meshes, {} tris from '{}'",
								added, partTri, p);
						} else {
							REX::INFO("SFShopExplorer NativeModel: geometry files: ParseNifMeshes failed for '{}'",
								p);
						}
					};
					for (const auto& p : cands) {
						adoptOne(p);
					}
					if (!any && !proxyWorld) {
						// World model missing or unparseable: addon parts fill in.
						const std::size_t base = cands.size();
						for (const auto& p : a_modelPaths) {
							pushCand(p);
						}
						if (cands.size() > base) {
							// Build-81: place addon parts through BSConnectPoint
							// (the C-Receiver addon becomes the identity root
							// when the world model itself is the failed parse).
							std::vector<PlaceMat> addonPlaces;
							ResolveAssemblyPlaces(cands, addonPlaces);
							for (std::size_t i = base; i < cands.size(); ++i) {
								const std::size_t before = result.meshes.size();
								adoptOne(cands[i]);
								TransformMeshes(result.meshes, before, addonPlaces[i]);
							}
						}
					}
					if (any) {
						ComputeMeshBounds(result.meshes, result.center, result.radius);
						result.geoCount = static_cast<std::uint32_t>(result.meshes.size());
						result.triCount = fileTri;
						result.extracted = true;
						result.message = cands.size() > 1
							? std::format("{} meshes from {} path(s) (geometry files)", result.meshes.size(), cands.size())
							: std::format("{} meshes (geometry files)", result.meshes.size());
					} else if (!cands.empty()) {
						REX::INFO("SFShopExplorer NativeModel: geometry files: no geometry from {} candidate path(s)",
							cands.size());
					}
				}

				// The files had their chance; if they produced nothing, restore
				// the UV-less clone rather than showing an empty viewport. It
				// still renders untextured, but the geometry is the item's own.
				if (result.meshes.empty() && !uvlessClone.empty()) {
					result.meshes = std::move(uvlessClone);
					std::uint32_t tri = 0;
					for (const auto& mesh : result.meshes) {
						tri += static_cast<std::uint32_t>(mesh.indices.size() / 3);
					}
					ComputeMeshBounds(result.meshes, result.center, result.radius);
					result.geoCount = static_cast<std::uint32_t>(result.meshes.size());
					result.triCount = tri;
					result.extracted = true;
					result.message = std::format("{} meshes (UV-less Clone3D; geometry files empty)", result.meshes.size());
					REX::INFO("SFShopExplorer NativeModel: geometry files empty - restored {} UV-less Clone3D mesh(es)",
						result.meshes.size());
				}

				// Stage B: host the model paths on a helper WEAP — swap its
				// +0x100 model string, Clone3D (the proven dummy-weapon
				// loader), restore. Caller assembly paths run first (worn
				// mesh), then the form's own scanned path; stop at first
				// geometry so worn + world pieces are never double-added.
				if (result.meshes.empty()) {
					std::uintptr_t helperAddr = 0;
					std::uint32_t helperID = 0;
					auto* helper = FindHelperWeapon(helperAddr, helperID);
					if (helper) {
						REX::INFO("SFShopExplorer NativeModel: helper WEAP {:08X} at {:#x} hosts armor load",
							helperID, helperAddr);
						std::vector<std::string> candidates;
						candidates.reserve(a_modelPaths.size() + 1);
						for (const auto& path : a_modelPaths) {
							if (!path.empty()) {
								candidates.push_back(path);
							}
						}
						if (!modelPath.empty()) {
							candidates.push_back(modelPath);
						}
						for (const auto& path : candidates) {
							auto part = Clone3DSwapPath(helperID, helper, helperAddr, path);
							if (part.loaded) {
								result.loaded = true;
							}
							if (part.extracted) {
								result.extracted = true;
								result.nodeCount = part.nodeCount;
								result.geoCount = part.geoCount;
								result.triCount = part.triCount;
								result.center[0] = part.center[0];
								result.center[1] = part.center[1];
								result.center[2] = part.center[2];
								result.radius = part.radius;
								result.meshes = std::move(part.meshes);
								result.message = std::format("{} meshes (helper swap {})",
									result.meshes.size(), path);
								break;
							}
						}
						if (result.meshes.empty()) {
							result.message = std::format(
								"no geometry after null requester and helper swap ({} candidate path(s))",
								candidates.size());
						}
					} else {
						result.message = "no geometry; helper WEAP not found";
					}
				}
				return result;
			}

			// Dummy base: load each assembly path via temporary model-string swap.
			// Merge all meshes into one result (multi-part OMOD gun).
			if (a_modelPaths.empty()) {
				result.message = "dummy base model, no assembly paths";
				REX::INFO("SFShopExplorer NativeModel: {}", result.message);
				return result;
			}

			// Build-81: resolve BSConnectPoint placements once, before the
			// merge, so every appended part lands in receiver world space.
			std::vector<PlaceMat> places;
			ResolveAssemblyPlaces(a_modelPaths, places);
			bool anyMoved = false;
			for (const auto& p : places) {
				if (!IsPlaceIdent(p)) {
					anyMoved = true;
					break;
				}
			}
			std::size_t partMeshes = 0;
			const auto logAssemblyPart = [&](const char* a_stage, const std::string& a_path,
				std::size_t a_meshes, std::uint32_t a_tris, const PlaceMat& a_place) {
				REX::INFO("SFShopExplorer NativeModel: assembly {} path='{}' meshes={} tris={} placeIdent={}",
					a_stage, a_path, a_meshes, a_tris, IsPlaceIdent(a_place));
			};

			for (std::size_t pi = 0; pi < a_modelPaths.size(); ++pi) {
				const auto& path = a_modelPaths[pi];
				if (path.empty()) continue;
				auto part = Clone3DSwapPath(a_formID, boundObj, weapAddr, path);
				result.nodeCount += part.nodeCount;
				result.geoCount += part.geoCount;
				result.triCount += part.triCount;
				if (part.loaded) {
					result.loaded = true;
				}
				if (part.extracted) {
					result.extracted = true;
					TransformMeshes(part.meshes, 0, places[pi]);
					logAssemblyPart("clone", path, part.meshes.size(), part.triCount, places[pi]);
					if (result.meshes.empty()) {
						result.center[0] = part.center[0];
						result.center[1] = part.center[1];
						result.center[2] = part.center[2];
						result.radius = part.radius;
					}
					for (auto& m : part.meshes) {
						result.meshes.push_back(std::move(m));
					}
				} else if (!IsDummyModelPath(path)) {
					// Build-74: the swap-Clone3D walk found nothing (SF stores
					// vertex data in the external geometries/*.mesh stream) -
					// parse the part's own NIF geometry files instead, same as
					// Stage A2, so dummy-base weapons get real meshes.
					const std::size_t before = result.meshes.size();
					std::uint32_t fileTri = 0;
					const auto added = AdoptFileMeshes(path, result.meshes, fileTri);
					if (added > 0) {
						TransformMeshes(result.meshes, before, places[pi]);
						result.triCount += fileTri;
						result.geoCount += static_cast<std::uint32_t>(added);
						result.extracted = true;
						partMeshes += added;
						logAssemblyPart("geometry", path, added, fileTri, places[pi]);
						REX::INFO("SFShopExplorer NativeModel: geometry files: {} meshes, {} tris from '{}'",
							added, fileTri, path);
					} else {
						REX::INFO("SFShopExplorer NativeModel: geometry files: ParseNifMeshes failed for '{}'",
							path);
					}
				}
				REX::INFO("SFShopExplorer NativeModel: part walk '{}' loaded={} extracted={} swapMeshes={} swapTri={}",
					path, part.loaded, part.extracted, part.meshes.size(), part.triCount);
			}

			// Build-81: any moved part invalidates the first part's stored
			// bounds - recompute from the merged positions.
			if ((result.radius <= 0.f || anyMoved) && !result.meshes.empty()) {
				ComputeMeshBounds(result.meshes, result.center, result.radius);
			}

			REX::INFO("SFShopExplorer NativeModel: assembly summary paths={} cloneMeshes={} geometryMeshes={} totalMeshes={} tris={} moved={}",
				a_modelPaths.size(), result.meshes.size() - partMeshes, partMeshes, result.meshes.size(), result.triCount, anyMoved);
			if (result.meshes.empty()) {
				result.message = std::format("dummy base, {} assembly path(s), no geometry",
					a_modelPaths.size());
			} else {
				result.message = std::format("{} meshes from {} path(s)",
					result.meshes.size(), a_modelPaths.size());
			}
			return result;
		}
	}

	// ── Engine-loaded preview model (native route) ───────────────────────────
	//
	// The barter menu's preview model is the one the engine assembled: OMOD
	// parts, CDB materials, the lot. Clone3D cannot reach it (documented dead
	// end), but the menu keeps the loaded NiAVObject alive, and the menu is
	// reachable from the event source the plugin already dispatches into.
	//
	// Offsets below are for Starfield.exe 1.16.244 and are all proven from the
	// image, not guessed:
	//   * BarterMenu has 13 RTTI vtables, one per event-sink base. The base at
	//     menu+0x150 is its `BSTEventSink<BarterMenu_LoadModel>` subobject: its
	//     vtable 0x4c808e8 starts with the adjustor thunk
	//     `sub rcx, 0x150; jmp 0x15d0ea0`, and the teardown at 0x15d0c76
	//     unregisters `[menu+0x150]` from the source returned by 0x15da450,
	//     which is the BarterMenu_LoadModel source getter (REL::ID 91377).
	//     `sink - 0x150` is therefore a base-class offset, not a data-member
	//     guess.
	//   * The event source layout is the SDK's own: BSTEventSource<> puts its
	//     `BSTArray<BSTEventSink*>` at +0x08, and that array is size@+0,
	//     capacity@+4, data@+8 (see RE/B/BSTEvent.h and RE/B/BSTArray.h).
	//
	// Everything here is read-only: no engine code is called. The menu comes
	// from the sink array, the model from scanning the menu for an object whose
	// RTTI is a NiNode. Every read is CanRead-gated and the search is bounded,
	// so a misidentified field costs a failed preview rather than a crash.
	inline constexpr std::uintptr_t kBarterLoadSinkOffset = 0x150;
	inline constexpr std::uint32_t kMenuScanBytes = 0x4000;
	inline constexpr std::uint32_t kMaxSinkCount = 64;
	inline constexpr std::uint32_t kMaxModelCandidates = 12;

	// Defined below, used by both the read-back and the chain census. The menu
	// is the same object either way, so the two paths must not drift apart.
	EnginePreviewChain ProbeMenuChain(const char* a_name, std::size_t a_knownOffset);

	// SEH has to live in a function with no objects that need unwinding
	// (C2712), so the guarded call is split out of the sink walk below.
	using get_barter_source_t = void* (*)();

	// The relocation is created on first use, outside any __try: a function-
	// local static with a non-trivial initializer is exactly what makes MSVC
	// reject __try (C2712), even when the static is declared before it.
	REL::Relocation<get_barter_source_t>& BarterLoadModelSourceGetter()
	{
		static REL::Relocation<get_barter_source_t> s_getSource{ REL::ID(91377) };
		return s_getSource;
	}

	bool GetBarterLoadModelSource(void*& a_out, std::uint32_t& a_exception)
	{
		a_exception = 0;
		a_out = nullptr;
		auto& getSource = BarterLoadModelSourceGetter();
		__try {
			a_out = getSource();
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			a_exception = GetExceptionCode();
			return false;
		}
		return true;
	}

	// Whether the engine's BarterMenu is open. This separates the two ways the
	// sink list can be empty: the shop UI is opened from the global form
	// catalog and needs no vendor, so it is perfectly normal to be browsing
	// items with no BarterMenu in existence -- and then nothing can load a
	// preview model, however the dispatch is shaped.
	// The BSFixedString lives outside the __try: constructing one is exactly
	// the object-unwinding case MSVC rejects next to SEH (C2712).
	const RE::BSFixedString& BarterMenuName()
	{
		static const RE::BSFixedString s_name{ "BarterMenu" };
		return s_name;
	}

	bool ProbeBarterMenuOpen(bool& a_open)
	{
		a_open = false;
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			return false;
		}
		const auto& name = BarterMenuName();
		__try {
			a_open = ui->IsMenuOpen(name);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// One-line UI census used on the failure path: registered / instance
	// exists / open. "Never created" and "created but closed" are different
	// problems -- the first means a vendor must be open, the second would mean
	// the sink walk itself is wrong -- so the log has to tell them apart.
	std::string DescribeBarterMenuState()
	{
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			return "UI singleton unavailable";
		}
		const auto& name = BarterMenuName();
		const bool registered = ui->IsMenuRegistered(name);
		bool exists = false;
		if (registered) {
			auto menu = ui->GetMenu(name);
			exists = static_cast<bool>(menu);
		}
		bool open = false;
		const bool probed = ProbeBarterMenuOpen(open);
		return std::format("registered={} instance={} open={}", registered, exists,
			probed ? (open ? "yes" : "no") : "unknown");
	}

	// Returns the BarterMenu object, or 0 when the sink list holds no such
	// sink, filling a_reason with the specific step that failed. Logs every
	// sink so a layout change is visible in one line.
	std::uintptr_t FindBarterMenuFromEventSource(std::string& a_reason)
	{
		void* source = nullptr;
		std::uint32_t exception = 0;
		if (!GetBarterLoadModelSource(source, exception)) {
			a_reason = std::format("LoadModel source getter raised 0x{:X}", exception);
			REX::WARN("SFShopExplorer NativeModel: {}", a_reason);
			return 0;
		}
		if (!source) {
			a_reason = "LoadModel source getter returned null";
			return 0;
		}
		const auto base = std::bit_cast<std::uintptr_t>(source);
		if (!CanRead(base, 0x18)) {
			a_reason = std::format("LoadModel source 0x{:X} is not readable", base);
			return 0;
		}
		const auto count = *reinterpret_cast<const std::uint32_t*>(base + 0x08);
		const auto data = *reinterpret_cast<const std::uintptr_t*>(base + 0x10);
		if (count == 0 || count > kMaxSinkCount || !CanRead(data, static_cast<std::size_t>(count) * sizeof(std::uintptr_t))) {
			bool open = false;
			const bool probed = ProbeBarterMenuOpen(open);
			const auto uiState = DescribeBarterMenuState();
			REX::INFO("SFShopExplorer NativeModel: barter source 0x{:X} sink count={} ({})",
				base, count, uiState);
			a_reason = !probed
				? std::format("LoadModel source has {} sink(s); {}", count, uiState)
				: open
				      ? std::format("BarterMenu is open but registered {} LoadModel sink(s) ({})", count, uiState)
				      : std::format("BarterMenu is not open; nothing loads a preview ({})", uiState);
			return 0;
		}
		for (std::uint32_t i = 0; i < count; ++i) {
			const auto sink = *reinterpret_cast<const std::uintptr_t*>(data + i * sizeof(std::uintptr_t));
			if (!CanRead(sink, sizeof(std::uintptr_t))) {
				continue;
			}
			const auto rtti = DemangleRTTI(ResolveRTTIName(*reinterpret_cast<const std::uintptr_t*>(sink)));
			REX::INFO("SFShopExplorer NativeModel: barter sink[{}] 0x{:X} rtti='{}'", i, sink, rtti);
			if (rtti.find("BarterMenu") == std::string::npos) {
				continue;
			}
			const auto menu = sink - kBarterLoadSinkOffset;
			if (CanRead(menu, 0x200)) {
				REX::INFO("SFShopExplorer NativeModel: BarterMenu at 0x{:X} (sink[{}] 0x{:X} - 0x{:X})",
					menu, i, sink, static_cast<std::uintptr_t>(kBarterLoadSinkOffset));
				return menu;
			}
		}
		a_reason = std::format("{} sink(s) registered, none of them BarterMenu", count);
		return 0;
	}

	// True for the RTTI names a loaded preview root can carry. Deliberately
	// narrow: `BSGeometry` is a leaf, and accepting it here would let the walk
	// miss the parent that carries the transform.
	bool IsPreviewModelRoot(const std::string& a_rtti)
	{
		return a_rtti.find("NiNode") != std::string::npos || a_rtti.find("BSFadeNode") != std::string::npos ||
		       a_rtti.find("NiAVObject") != std::string::npos || a_rtti.find("BSModelNode") != std::string::npos;
	}

	struct ModelCandidate
	{
		std::uintptr_t addr{ 0 };
		std::string    rtti;
	};

	// Collects members of a_base that point at a preview model root (NiNode or
	// one of its siblings). Readability-gated and bounded: a wrong base costs a
	// failed preview, never a crash.
	void CollectModelRoots(std::uintptr_t a_base, std::size_t a_bytes, const char* a_label,
		std::vector<ModelCandidate>& a_out)
	{
		if (!IsValidPtr(a_base)) {
			return;
		}
		std::uint32_t added = 0;
		for (std::size_t off = 0; off + sizeof(std::uintptr_t) <= a_bytes; off += sizeof(std::uintptr_t)) {
			if (!CanRead(a_base + off, sizeof(std::uintptr_t))) {
				break;
			}
			std::uintptr_t ptr = 0;
			if (!SafeReadPtr(a_base + off, ptr)) {
				continue;
			}
			std::uintptr_t vt = 0;
			if (ptr < 0x10000 || !SafeReadPtr(ptr, vt)) {
				continue;
			}
			const auto rtti = DemangleRTTI(ResolveRTTIName(vt));
			if (!IsPreviewModelRoot(rtti)) {
				continue;
			}
			REX::INFO("SFShopExplorer NativeModel: model root {} +0x{:X} -> 0x{:X} rtti='{}'", a_label, off, ptr, rtti);
			a_out.push_back({ ptr, rtti });
			if (++added >= kMaxModelCandidates || a_out.size() >= kMaxModelCandidates) {
				break;
			}
		}
		REX::INFO("SFShopExplorer NativeModel: model scan {} base=0x{:X} bytes=0x{:X} added={} total={}",
			a_label, a_base, a_bytes, added, a_out.size());
	}

	// REFR -> LOADED_REF_DATA -> data3D, which is where a reference keeps the
	// node the engine built for it. TESObjectREFR::loadedData sits at +0xB8 and
	// LOADED_REF_DATA::data3D at +0x08; BSGuarded's member order is unconfirmed
	// in the header (both members are marked "??"), so the lock-first layout at
	// +0xC0 is probed as well and whichever yields a model root is kept.
	void CollectRefrModel(std::uintptr_t a_refr, std::vector<ModelCandidate>& a_out)
	{
		if (!IsValidPtr(a_refr)) {
			return;
		}
		for (const std::size_t guarded : { static_cast<std::size_t>(0xB8), static_cast<std::size_t>(0xC0) }) {
			if (!CanRead(a_refr + guarded, sizeof(std::uintptr_t))) {
				continue;
			}
			std::uintptr_t loaded = 0;
			if (!SafeReadPtr(a_refr + guarded, loaded) || loaded < 0x10000) {
				continue;
			}
			std::uintptr_t node = 0;
			if (!SafeReadPtr(loaded + 0x08, node) || node < 0x10000) {
				continue;
			}
			std::uintptr_t nodeVt = 0;
			if (!SafeReadPtr(node, nodeVt)) {
				continue;
			}
			const auto rtti = DemangleRTTI(ResolveRTTIName(nodeVt));
			REX::INFO("SFShopExplorer NativeModel: refr 3D probe refr=0x{:X} +0x{:X} -> data3D=0x{:X} 0x{:X} rtti='{}'",
				a_refr, guarded, loaded, node, rtti);
			if (IsPreviewModelRoot(rtti)) {
				a_out.push_back({ node, rtti });
				return;
			}
		}
	}

	bool BarterMenuAvailable()
	{
		bool open = false;
		return ProbeBarterMenuOpen(open) && open;
	}

	PreviewResult LoadFromBarterMenu(std::uint32_t a_formID)
	{
		PreviewResult result;
		REX::INFO("SFShopExplorer NativeModel: barter read-back begin form={:08X}", a_formID);
		std::string reason;
		const auto menu = FindBarterMenuFromEventSource(reason);
		if (!menu) {
			result.message = std::move(reason);
			return result;
		}

		// The model is not a member of the menu. The live run of 2026-10-17
		// settled that: while a barter menu was up, a 0x4000-byte scan of the
		// menu found no NiNode at all, yet the chain census found
		// `UICellRenderer_InventoryItem` inside the same menu, with a
		// TESObjectREFR and a BSMenu3D scene hanging off it. So walk the chain
		// the menu actually exposes -- refr's own 3D node first, then the
		// renderer, scene and (as a last resort) the menu.
		// +0x1B0 is measured, not guessed: the F10 live capture of 2026-10-17
		// 22:08 dumped BarterMenu 0x2E0B9B21F80 with
		// `+0x01b0 0x2dcb04c1db0 rtti=VUICellRenderer_InventoryItem@@`, i.e. the
		// renderer at that offset (InventoryMenu keeps its own at +0x198).
		const auto chain = ProbeMenuChain("BarterMenu", 0x1B0);
		std::vector<ModelCandidate> candidates;
		if (chain.found) {
			REX::INFO("SFShopExplorer NativeModel: barter chain menu=0x{:X} renderer=0x{:X} '{}' refr=0x{:X} '{}' scene=0x{:X} loadTask=0x{:X}",
				chain.menu, chain.renderer, chain.rendererType, chain.refr, chain.refrType, chain.scene, chain.loadTask);
			CollectRefrModel(chain.refr, candidates);
			CollectModelRoots(chain.renderer, 0x600, "renderer", candidates);
			CollectModelRoots(chain.scene, 0x400, "scene", candidates);
		}
		if (candidates.empty()) {
			CollectModelRoots(menu, kMenuScanBytes, "menu", candidates);
		}

		if (candidates.empty()) {
			result.message = chain.found
				? std::format("BarterMenu 0x{:X} chain (renderer=0x{:X} refr=0x{:X} scene=0x{:X}) carried no model root",
					menu, chain.renderer, chain.refr, chain.scene)
				: std::format("BarterMenu at 0x{:X} has no preview renderer and no NiNode member in 0x{:X} bytes", menu, kMenuScanBytes);
			REX::INFO("SFShopExplorer NativeModel: barter read-back {}", result.message);
			return result;
		}

		// The menu holds more than one node (its own scene plus whatever the
		// last item left behind). Score each root by extracted triangles and
		// keep the richest, which is the preview model, not an empty scene
		// root or a UI node.
		PreviewResult best;
		ModelCandidate bestCand{};
		for (const auto& candidate : candidates) {
			PreviewResult attempt;
			attempt.loaded = true;
			WalkTree(reinterpret_cast<const RE::NiAVObject*>(candidate.addr), attempt.nodeCount,
				attempt.geoCount, attempt.triCount, attempt.meshes);
			REX::INFO("SFShopExplorer NativeModel: candidate 0x{:X} ('{}') nodes={} geo={} tris={}",
				candidate.addr, candidate.rtti, attempt.nodeCount, attempt.geoCount, attempt.triCount);
			if (attempt.triCount > best.triCount) {
				bestCand = candidate;
				best = std::move(attempt);
			}
		}

		if (best.meshes.empty() || best.triCount == 0) {
			result.message = std::format("{} NiNode candidate(s) in BarterMenu, none carried geometry", candidates.size());
			REX::INFO("SFShopExplorer NativeModel: barter read-back {}", result.message);
			return result;
		}

		ComputeMeshBounds(best.meshes, best.center, best.radius);
		if (!SaneBounds(best.center, best.radius)) {
			result.message = "barter model bounds rejected";
			REX::INFO("SFShopExplorer NativeModel: barter read-back {} (center=({:.1f},{:.1f},{:.1f}) r={:.1f})",
				result.message, best.center[0], best.center[1], best.center[2], best.radius);
			return result;
		}
		best.extracted = true;
		best.message = std::format("{} meshes from BarterMenu 0x{:X} ('{}')",
			best.meshes.size(), bestCand.addr, bestCand.rtti);
		REX::INFO("SFShopExplorer NativeModel: barter read-back ok: {} meshes {} tris root 0x{:X} ({} candidate root(s))",
			best.meshes.size(), best.triCount, bestCand.addr, candidates.size());
		return best;
	}

	PreviewResult LoadFromForm(std::uint32_t a_formID, const std::vector<std::string>& a_modelPaths)
	{
		auto result = LoadFromForm_Clone3D(a_formID, a_modelPaths);
		ApplyCaptainNoteMaterialSwaps(a_formID, result.meshes);
		// Build-76 safety net: every path funnels here, so insane bounds
		// can never reach the preview renderer. Recompute from positions
		// first (cheap, and correct when only the stored bounding was
		// garbage); clear only if the positions themselves are insane.
		if (!result.meshes.empty() && !SaneBounds(result.center, result.radius)) {
			ComputeMeshBounds(result.meshes, result.center, result.radius);
			if (!SaneBounds(result.center, result.radius)) {
				REX::INFO("SFShopExplorer NativeModel: bounds sanity rejected {} mesh(es) (center=({:.1f},{:.1f},{:.1f}) r={:.1f}) - dropped",
					result.meshes.size(), result.center[0], result.center[1], result.center[2], result.radius);
				result.meshes.clear();
				result.extracted = false;
				result.geoCount = 0;
				result.triCount = 0;
				result.message = "bounds sanity rejected";
			}
		}
		return result;
	}

	void ClearCache()
	{
		g_cachedFormID = 0;
		g_cachedObj.reset();
	}

	namespace
	{
		// First member of a_menu (8-byte stride) that points at an object whose
		// RTTI name contains a_needle. Readability-gated and fault-gated, never
		// throws: the slots belong to a live menu the main thread may be tearing
		// down, so a slot can be released between CanRead and the load.
		std::uintptr_t FindTypedMember(std::uintptr_t a_base, std::size_t a_bytes, std::string_view a_needle)
		{
			if (!IsValidPtr(a_base)) {
				return 0;
			}
			for (std::size_t off = 0; off + 8 <= a_bytes; off += 8) {
				if (!CanRead(a_base + off, 8)) {
					continue;
				}
				std::uintptr_t cand = 0;
				if (!SafeReadPtr(a_base + off, cand)) {
					continue;
				}
				if (!IsValidPtr(cand)) {
					continue;
				}
				std::uintptr_t vt = 0;
				if (!SafeReadPtr(cand, vt)) {
					continue;
				}
				if (!IsValidPtr(vt)) {
					continue;
				}
				const auto name = ResolveRTTIName(vt);
				if (name.find(a_needle) != std::string::npos) {
					return cand;
				}
			}
			return 0;
		}

		std::string RttiOfObject(std::uintptr_t a_obj)
		{
			if (!IsValidPtr(a_obj) || !CanRead(a_obj, 8)) {
				return {};
			}
			std::uintptr_t vt = 0;
			if (!SafeReadPtr(a_obj, vt)) {
				return {};
			}
			return IsValidPtr(vt) ? ResolveRTTIName(vt) : std::string{};
		}

		bool ReadPosition(std::uintptr_t a_refr, float* a_out)
		{
			if (a_out) {
				a_out[0] = a_out[1] = a_out[2] = 0.0f;
			}
			if (!a_refr) {
				return false;
			}
			__try {
				const auto pos = reinterpret_cast<RE::TESObjectREFR*>(a_refr)->GetPosition();
				if (a_out) {
					a_out[0] = pos.x;
					a_out[1] = pos.y;
					a_out[2] = pos.z;
				}
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

	}

	// The menu a preview is drawn from: its renderer, and the objects the
	// renderer owns. Used by the legacy read-back (INI `[preview]
	// legacyRenderer`); the shop preview itself no longer reads the engine.
	EnginePreviewChain ProbeMenuChain(const char* a_name, std::size_t a_knownOffset)
	{
		EnginePreviewChain out;
		auto*               ui = RE::UI::GetSingleton();
		if (!ui) {
			return out;
		}
		RE::BSFixedString key{ a_name };
		auto              menu = ui->GetMenu(key);
		auto*             raw = menu.get();
		if (!raw) {
			return out;
		}
		// GetMenu keeps answering during teardown: on 2026-10-18 the DataMenu
		// object was still in the UI map, and still had a renderer, while the
		// game had already freed the AS3 objects behind that renderer - the
		// chain walk then read a released slot and crashed. Only an open menu
		// is a safe thing to walk; a closed one is retried on the next tick.
		if (!ui->IsMenuOpen(key)) {
			return out;
		}
		const auto menuAddr = reinterpret_cast<std::uintptr_t>(raw);

		// Measured offsets first, then a bounded RTTI scan. Barter and Container
		// have no measured offset yet, so they go straight to the scan; they
		// matter because the vendor screen is where the shop previews.
		std::uintptr_t renderer = 0;
		if (a_knownOffset != 0 && CanRead(menuAddr + a_knownOffset, 8)) {
			std::uintptr_t cand = 0;
			if (SafeReadPtr(menuAddr + a_knownOffset, cand) && IsValidPtr(cand) &&
				RttiOfObject(cand).find("UICellRenderer") != std::string::npos) {
				renderer = cand;
			}
		}
		if (renderer == 0) {
			renderer = FindTypedMember(menuAddr, 0x8000, "UICellRenderer");
		}
		if (renderer == 0) {
			return out;
		}

		out.found = true;
		out.menu = menuAddr;
		out.menuName = a_name;
		out.renderer = renderer;
		out.rendererType = RttiOfObject(renderer);

		// Measured slot first; when it does not carry the expected type the
		// renderer is scanned, so a shifted layout costs a scan, not the chain.
		const auto pick = [renderer](std::size_t a_off, const char* a_needle) -> std::uintptr_t {
			if (a_off != 0 && CanRead(renderer + a_off, 8)) {
				std::uintptr_t cand = 0;
				if (SafeReadPtr(renderer + a_off, cand) && IsValidPtr(cand) &&
					RttiOfObject(cand).find(a_needle) != std::string::npos) {
					return cand;
				}
			}
			return FindTypedMember(renderer, 0x600, a_needle);
		};
		out.refr = pick(0xC0, "TESObjectREFR");
		out.scene = pick(0x68, "Scene");
		out.loadTask = pick(0xD0, "ItemModelLoadTask");
		out.refrType = RttiOfObject(out.refr);
		ReadPosition(out.refr, out.refrPosition);
		return out;
	}

}
