// ============================================================================
// Parcel Fix - clear the parcels that shop builds before 0.9.0 left in a save.
//
// What is being cleaned up
// ------------------------
// Until 0.9.0 the shop delivered a purchase by dropping a real world reference
// at the player's feet and then moving its contents into the inventory. The
// reference itself stayed behind. The engine makes a reference the player can
// interact with persistent, and a persistent reference is written into the
// save, so every purchase from those builds left one parcel in the file. 0.9.0
// hides its own drop before the save is written, which is why new purchases no
// longer leave anything - but the references already in an old save are data,
// not code, so no later version of the shop can remove them from there.
//
// This plugin does exactly that one job, in the save, and nothing else. It is
// meant to be installed, run once per affected save, and then removed.
//
// Numbering: the 1.0.0-1.0.9 in this history are test builds. The first public
// release is 1.0.0 and is behaviourally the last of them (test build 1.0.9,
// which cleared the leftovers in game). Where the history below says 1.0.8 or
// 1.0.9 it means those test builds.
//
// What it looks for
// -----------------
//   1. the reference was created at runtime (`IsCreated()`: the game assigns
//      FFxxxxxx ids, a plugin's placed references keep their own id). Every
//      reference the shop ever placed is one of these;
//   2. its base object is a type the shop sells (weapon, armour, misc, aid,
//      ammo, book, key) - the drop is a reference to the item itself;
//   3. it is within the configured radius of the player, so the candidate list
//      is the pad the parcels were dropped on rather than every dynamic
//      reference in the cell.
//
// 1.0.2-1.0.4 also required the reference to be persistent, deleted or
// disabled, on the theory that only a reference a save would keep can be a
// leftover. Three runs against the affected save killed that test: the cell the
// player loads into holds eleven runtime-created references, and not one of
// them is a sellable record carrying a save flag (the single flagged one is the
// deleted 'Artifact ETA' activator). Every purchase's parcel is still on the
// pad, so the flag test was rejecting exactly what it was meant to find. It is
// now reported in the log instead of filtering: [ParcelFix] bFlagsOnly=1 brings
// the old behaviour back.
//
// Where it looks
// --------------
// 1.0.4 walked only the player's own cell (`TESObjectCELL::references`, count
// u32 at +0x80 and pointer array at +0x88 - the walk the shop has used in game
// for months). A layer of Starfield keeps the references it treats as
// persistent in a cell of the worldspace rather than in the local cell
// (`TESWorldSpace::unk0D0` and `unk1D0`, both commented "persistent cell?" in
// commonlib), and a reference promoted out of the local cell would be there.
// So a sweep now walks the player's cell and both of those worldspace cells,
// deduplicates by reference id, and says in the log which cell a line came
// from. The worldspace cells are read past a size guard, because one of them
// can hold a whole worldspace's persistent references.
//
// What it does
// ------------
// Every candidate is logged in full - reference id, base form, name, distance
// and the three flags - before anything is touched. Then the reference is
// parked twenty thousand units below the cell and made non-persistent and
// disabled: out of view, no collision, not activatable. 1.0.5 flipped the two
// flags only, and the parcels were still on the pad when the player looked: a
// disabled reference stops being drawn when the cell next builds its scene, so
// a flag-only build reads as "nothing happened". Parking alone was not enough
// either: `data.location` is what a 3D node was built from, not a handle to it,
// so the record moved and the mesh stayed - the parcels were then drawn but not
// clickable, which is the state 1.0.7 shipped. 1.0.8 shrinks the reference with
// `SetScale`, the engine's own call, so the attached node follows it and the box
// disappears on the spot; the scale is a record field as well, so a save keeps
// it. Nothing is freed, so nothing here can leave another list holding a
// dangling reference or inventory instance. Everything is reversible: the log
// carries the id, and the console can put a reference back (`FFxxxxxx.enable`,
// `FFxxxxxx.setscale 1`).
//
// When it runs, and why that matters
// ---------------------------------
// 1.0.0 walked the player's cell 1.5 s after a save load began, crashed, and
// left no line to say where. 1.0.1 logged "scan start" and then died inside
// the walk: the cell it was handed still belonged to the loading screen, and
// walking a cell's reference array while the game is streaming that cell in is
// not a safe thing to do. So the scan now waits for the loading menu to close
// and *then* waits the settle frames - the timer restarts whenever the game is
// busy - and every read of a cell or a reference is guarded, so a stale object
// is counted and skipped instead of taking the process with it.
//
// [ParcelFix] bEnabled=0 stops the plugin working without removing it.
// [ParcelFix] bAct=0 keeps the scan and the log but changes nothing (dry run).
// [ParcelFix] bDiagnose=1 lists every runtime-created reference it saw.
// [ParcelFix] bFlagsOnly=1 restores the old persistent/deleted/disabled test.
// [ParcelFix] iRadius=1500 sets the radius around the player, in engine units.
// ============================================================================

#include "PCH.h"

#include "RE/B/BSFixedString.h"
#include "RE/F/FormTypes.h"
#include "RE/P/PlayerCharacter.h"
#include "RE/RTTI.h"
#include "RE/T/TESFullName.h"
#include "RE/T/TESObjectCELL.h"
#include "RE/T/TESObjectREFR.h"
#include "RE/T/TESWorldSpace.h"
#include "RE/U/UI.h"

#include "SFSE/SFSE.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{
	constexpr const wchar_t* kIniName = L"ParcelFix.ini";
	constexpr const char*    kVersion = "1.0.0";

	// The pad a drop lands on is a few metres across; 1500 engine units covers
	// the shop front in every cell this was tested in and stays far short of a
	// whole city block. Overridable with [ParcelFix] iRadius.
	constexpr float kDefaultRadius = 1500.0f;

	// A cell is not fully attached on the frame it becomes the player's, so the
	// scan waits this many frames after the cell changes *and* after the loading
	// screen is gone. 90 is about a second and a half at 60 fps: long enough for
	// the reference array to settle, short enough that it still happens while
	// the player is on the pad.
	constexpr int kSettleFrames = 90;

	// A worldspace's persistent cell can hold a whole worldspace. Walking one
	// that big would freeze the game for minutes, so it is reported and skipped
	// rather than walked.
	constexpr std::uint32_t kMaxCellRefs = 60000;

	// Diagnostics are capped per cell and per kind, but the runtime-created list
	// comes first: the 1.0.3 and 1.0.4 caps filled up on placed scenery and cut
	// off the created references the log was read for.
	constexpr std::size_t kMaxCreatedLines = 200;
	constexpr std::size_t kMaxNearLines = 200;

	struct Config
	{
		bool  enabled{ true };
		bool  act{ true };
		bool  diagnose{ false };
		bool  flagsOnly{ false };
		float radius{ kDefaultRadius };
	};

	// Everything one reference is asked, as plain data. Nothing here owns
	// memory, so it can be filled inside an SEH guard.
	struct RefProbe
	{
		RE::TESBoundObject* base{ nullptr };  // stable: a base form is not unloaded
		std::uint32_t refID{ 0 };
		std::uint32_t baseID{ 0 };
		std::int32_t  type{ 0 };
		float         distance{ 0.0f };
		// Position, so a later run can say whether the park was written back: a
		// reference that comes back at its own z was not moved by us at all.
		float         x{ 0.0f };
		float         y{ 0.0f };
		float         z{ 0.0f };
		bool          created{ false };
		bool          persistent{ false };
		bool          deleted{ false };
		bool          disabled{ false };
		bool          sellable{ false };
		std::uint32_t fault{ 0 };
	};

	struct Candidate
	{
		RE::TESObjectREFR* ref{ nullptr };
		RefProbe           probe;
		std::string        name;
	};

	// One cell walk's worth of counters, and the run's running total.
	struct SweepTotals
	{
		std::size_t cells{ 0 };
		std::size_t walked{ 0 };
		std::size_t created{ 0 };
		std::size_t candidates{ 0 };
		std::size_t hidden{ 0 };
		std::size_t outOfRange{ 0 };
		std::size_t notSellable{ 0 };
		std::size_t flaggedOut{ 0 };  // bFlagsOnly only
		std::size_t faults{ 0 };
	};

	std::filesystem::path PluginDirectory()
	{
		wchar_t path[MAX_PATH]{};
		const auto module = GetModuleHandleW(L"ParcelFix.dll");
		const auto length = module ? GetModuleFileNameW(module, path, MAX_PATH) : 0;
		if (length == 0 || length >= MAX_PATH) {
			return {};
		}
		return std::filesystem::path(path).parent_path();
	}

	Config ReadConfig()
	{
		Config config;
		const auto ini = PluginDirectory() / kIniName;
		config.enabled = GetPrivateProfileIntW(L"ParcelFix", L"bEnabled", 1, ini.c_str()) != 0;
		config.act = GetPrivateProfileIntW(L"ParcelFix", L"bAct", 1, ini.c_str()) != 0;
		config.diagnose = GetPrivateProfileIntW(L"ParcelFix", L"bDiagnose", 0, ini.c_str()) != 0;
		config.flagsOnly = GetPrivateProfileIntW(L"ParcelFix", L"bFlagsOnly", 0, ini.c_str()) != 0;
		const int radius = GetPrivateProfileIntW(L"ParcelFix", L"iRadius",
			static_cast<int>(kDefaultRadius), ini.c_str());
		config.radius = radius > 0 ? static_cast<float>(radius) : kDefaultRadius;
		return config;
	}

	// Every type the shop catalogue can draw, from BoundObjectOf() in the shop.
	// A base object outside this set was never something the shop sold, so a
	// reference to one cannot be one of its leftovers.
	bool ShopSells(const RE::TESForm* a_base)
	{
		if (!a_base) {
			return false;
		}
		switch (a_base->GetFormType()) {
		case RE::FormType::kWEAP:
		case RE::FormType::kARMO:
		case RE::FormType::kMISC:
		case RE::FormType::kALCH:
		case RE::FormType::kAMMO:
		case RE::FormType::kBOOK:
		case RE::FormType::kKEYM:
			return true;
		default:
			return false;
		}
	}

	// --- guarded reads -------------------------------------------------------
	//
	// The bodies below are deliberately object-free: an SEH frame around code
	// that owns a std::string or a vector is C2712 (no unwinding through
	// __try), so each one answers with a pointer or a POD and the caller builds
	// the C++ object.

	const char* RawFullName(RE::TESForm* a_form)
	{
		const char* name = nullptr;
		__try {
			if (auto* full = starfield_cast<RE::TESFullName*>(a_form); full) {
				name = full->GetFullName();
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			name = nullptr;
		}
		return name;
	}

	const char* RawEditorID(RE::TESForm* a_form)
	{
		const char* edid = nullptr;
		__try {
			edid = a_form->GetFormEditorID();
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			edid = nullptr;
		}
		return edid;
	}

	std::string DisplayName(RE::TESForm* a_form)
	{
		if (!a_form) {
			return {};
		}
		if (const char* name = RawFullName(a_form); name && *name) {
			return name;
		}
		if (const char* edid = RawEditorID(a_form); edid && *edid) {
			return edid;
		}
		return {};
	}

	std::uint32_t CellID(RE::TESObjectCELL* a_cell)
	{
		std::uint32_t id = 0;
		__try {
			id = a_cell->GetFormID();
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			id = 0;
		}
		return id;
	}

	bool CellAttached(RE::TESObjectCELL* a_cell)
	{
		bool attached = false;
		__try {
			attached = a_cell->IsAttached();
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			attached = false;
		}
		return attached;
	}

	// The worldspace a cell belongs to, or nullptr in an interior.
	RE::TESWorldSpace* RawWorldspace(RE::TESObjectCELL* a_cell)
	{
		RE::TESWorldSpace* space = nullptr;
		__try {
			space = a_cell->cellWorldspace;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			space = nullptr;
		}
		return space;
	}

	// A worldspace's persistent cell. commonlib marks both of these "persistent
	// cell?", so both are tried, read as a plain pointer: nothing here assumes
	// the field is initialised, and the caller validates what comes back.
	RE::TESObjectCELL* RawPersistentCell(RE::TESWorldSpace* a_space, const std::uint32_t a_which)
	{
		RE::TESObjectCELL* cell = nullptr;
		__try {
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_space);
			std::uintptr_t raw = 0;
			std::memcpy(&raw, bytes + (a_which == 0 ? 0x0D0 : 0x1D0), sizeof(raw));
			cell = reinterpret_cast<RE::TESObjectCELL*>(raw);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			cell = nullptr;
		}
		return cell;
	}

	// Takes the name by reference so the BSFixedString is built in the caller
	// (an object with a destructor cannot live inside __try - C2712). A fault
	// here answers "busy", which only delays a scan.
	bool RawIsMenuOpen(RE::UI* a_ui, const RE::BSFixedString& a_name)
	{
		bool open = true;
		__try {
			open = a_ui->IsMenuOpen(a_name);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			open = true;
		}
		return open;
	}

	// One reference, one guard. A reference that is being unloaded faults on its
	// first field read; that is recorded in `fault`, and the caller skips it.
	RefProbe ProbeRef(RE::TESObjectREFR* a_ref, const RE::NiPoint3& a_origin)
	{
		RefProbe p;
		__try {
			p.refID = a_ref->GetFormID();
			p.created = a_ref->IsCreated();
			p.persistent = a_ref->IsPersistent();
			p.deleted = a_ref->IsDeleted();
			p.disabled = a_ref->IsDisabled();
			auto* base = a_ref->data.objectReference.get();
			p.base = base;
			if (base) {
				p.baseID = base->GetFormID();
				p.type = static_cast<std::int32_t>(base->GetFormType());
				p.sellable = ShopSells(base);
			}
			const auto position = a_ref->GetPosition();
			p.x = position.x;
			p.y = position.y;
			p.z = position.z;
			const float dx = position.x - a_origin.x;
			const float dy = position.y - a_origin.y;
			const float dz = position.z - a_origin.z;
			p.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			p.fault = GetExceptionCode();
		}
		return p;
	}

	bool Readable(const void* a_ptr)
	{
		if (!a_ptr) {
			return false;
		}
		MEMORY_BASIC_INFORMATION info{};
		if (VirtualQuery(a_ptr, &info, sizeof(info)) != sizeof(info)) {
			return false;
		}
		if (info.State != MEM_COMMIT) {
			return false;
		}
		const DWORD protect = info.Protect & 0xFF;
		return protect != PAGE_NOACCESS && protect != PAGE_GUARD;
	}

	// The cell's reference array, read the way the shop reads it in
	// SpikeWalkCell: count u32 at +0x80, pointer array at +0x88.
	//
	// Deliberately not `ForEachReferenceInRange`. That helper takes the cell's
	// read-write lock (cell+0x120) and iterates through a std::function; the
	// 1.0.1 build died inside it with no line after "scan start", which is what
	// a wrong lock offset looks like. The two offsets below are the ones the
	// shop has walked in game for months.
	struct RefList
	{
		std::uint32_t count{ 0 };
		std::uint64_t data{ 0 };
		std::uint32_t fault{ 0 };
	};

	RefList ReadRefList(RE::TESObjectCELL* a_cell)
	{
		RefList list;
		__try {
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_cell);
			std::memcpy(&list.count, bytes + 0x80, sizeof(list.count));
			std::memcpy(&list.data, bytes + 0x88, sizeof(list.data));
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			list.fault = GetExceptionCode();
		}
		return list;
	}

	RE::TESObjectREFR* RefAt(std::uint64_t a_data, std::uint32_t a_index, std::uint32_t* a_fault = nullptr)
	{
		std::uintptr_t ref = 0;
		__try {
			std::memcpy(&ref, reinterpret_cast<const void*>(a_data + a_index * sizeof(std::uintptr_t)),
				sizeof(ref));
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			ref = 0;
			if (a_fault) {
				*a_fault = GetExceptionCode();
			}
		}
		// Same reachability test the shop uses: a plausible heap pointer, aligned,
		// in committed memory. Anything else is a stale slot, not a reference.
		if (ref < 0x100000000ULL || (ref & 7) != 0 || !Readable(reinterpret_cast<const void*>(ref))) {
			return nullptr;
		}
		return reinterpret_cast<RE::TESObjectREFR*>(ref);
	}

	// Park it far below the cell, then clear the persistent flag, mark it deleted
	// and disable it - the same set the console's markfordelete would leave,
	// reached from inside the plugin.
	//
	// The history here is the whole reason all four are set. 1.0.5 cleared the
	// persistence flag and set disabled; the next run of the same save showed the
	// same seven parcels with new runtime ids (FF02FBD0-D6 against FF02FB6B-71)
	// and disabled=0, so the flag had not been written back. 1.0.6 also parked
	// them 20,000 units down, and they were still on the pad. What those runs
	// prove is that the copy of these references the engine rebuilds on load
	// ignores a flag cleared while the cell is live - the record in the file is
	// what wins. The delete bit is the one the file itself respects: a deleted
	// reference is not drawn, and it is the documented manual fix for exactly
	// this leftover (click it, `markfordelete`).
	//
	// The park stays as well. It is a plain write to the ref's own position, so
	// a reference that does still attach is out of sight twenty thousand units
	// below the cell even before any flag is looked at.
	//
	// 1.0.8 adds the one thing 1.0.5-1.0.7 all missed, and the reason the
	// parcels were still drawn after the delete flag was set: a reference's 3D
	// node keeps its own world transform, and `data.location` is the record the
	// node was built from, not a handle to it. Writing the position moves the
	// record; the mesh stays where the cell put it. So the parcels sat on the
	// pad, drawn but not clickable - exactly the state the 21:36 screenshot
	// showed. `SetScale` is the engine's own call on the reference, so it walks
	// the attached node and moves it with the record; at 0.1% the box is a
	// fraction of a millimetre and reads as gone on the spot, with no cell
	// rebuild and nothing freed. Scale is a record field too, so - like the rest
	// of this - it is restored by `setscale 1` on the id in the log. 1.0.9 keeps
	// the call and drops the readback that killed the game (see below).
	// 1.0.8 crashed the game on its first candidate, and the log said why:
	//
	//   [C] ParcelFix.dll
	//   Failed to find offset for Address Library ID!
	//   Invalid ID: 0
	//   Game Version: 1.16.244.0
	//
	// That text is CommonLibSF's ID lookup giving up, not a fault this code could
	// catch. It came from `GetScale()`, the readback added in 1.0.8: unlike the
	// flag writes, it is not a field read - it is an out-of-line stub that calls
	// the engine through `REL::Relocation{ ID::TESObjectREFR::GetScale }`, and
	// this Address Library has no entry for that id. The lookup raises a fatal
	// error, which ends the process before any __try can see it, so the game
	// closed the moment the first ref was hidden and the log stopped there.
	//
	// Rule for this file: plain field writes and virtual calls only. Any RE
	// method that is a `REL::Relocation` stub is a hard dependency on the
	// Address Library being right for the player's exact build - `GetScale` is
	// one, and it is gone. The scale is not read back any more; `setscale` on
	// the id in the log still restores it.
	constexpr float kHiddenScale = 0.001f;

	bool HasAttached3D(RE::TESObjectREFR* a_ref)
	{
		auto loaded = a_ref->loadedData.LockRead();
		return *loaded && (*loaded)->data3D;
	}

	// `a_shrunk` reports whether the engine's scale call ran. The call walks the
	// reference's attached node, so it is only handed refs that have one; a ref
	// with no node has nothing to move and nothing drawn, so skipping it changes
	// nothing on screen. The node check is the only guard on purpose: the refs
	// that are still visible in an existing save are exactly the ones a previous
	// run already flagged deleted, and refusing to touch those would leave the
	// boxes the player is complaining about. The order below keeps the flags for
	// last, so the call always sees the reference as it was found.
	std::uint32_t HideRef(RE::TESObjectREFR* a_ref, bool& a_shrunk)
	{
		a_shrunk = false;
		__try {
			a_ref->data.location.z -= 20000.0f;
			if (HasAttached3D(a_ref)) {
				a_ref->SetScale(kHiddenScale);
				a_shrunk = true;
			}
			a_ref->SetPersistent(false);
			a_ref->SetDelete(true);
			a_ref->formFlags.set(true, RE::TESForm::FormFlags::kDisabled);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return GetExceptionCode();
		}
		return 0;
	}

	void LogProbe(const char* a_kind, const RefProbe& a_probe)
	{
		REX::INFO("ParcelFix: {} ref {:08X} base {:08X} type={} created={} persistent={} deleted={} "
				  "disabled={} sold={} distance {:.0f} '{}'",
			a_kind, a_probe.refID, a_probe.baseID, a_probe.type, a_probe.created ? 1 : 0,
			a_probe.persistent ? 1 : 0, a_probe.deleted ? 1 : 0, a_probe.disabled ? 1 : 0,
			a_probe.sellable ? 1 : 0, a_probe.distance, DisplayName(a_probe.base));
	}

	// One cell: walk every slot, list what is worth listing, and hide the
	// candidates that cell produced.
	void SweepCell(const Config& a_config, RE::TESObjectCELL* a_cell, const char* a_why,
		const RE::NiPoint3& a_origin, std::unordered_set<std::uint32_t>& a_seen, SweepTotals& a_totals)
	{
		const auto cellID = CellID(a_cell);
		if (cellID == 0) {
			REX::WARN("ParcelFix: cell for '{}' has no form id; skipped", a_why);
			return;
		}
		if (!CellAttached(a_cell)) {
			REX::WARN("ParcelFix: cell {:08X} ('{}') is not attached; skipped", cellID, a_why);
			return;
		}
		const auto list = ReadRefList(a_cell);
		if (list.fault || list.data < 0x100000000ULL || list.count > kMaxCellRefs ||
			!Readable(reinterpret_cast<const void*>(list.data))) {
			REX::WARN("ParcelFix: cell {:08X} ('{}') reference array not usable (count={} data=0x{:X} "
					  "fault=0x{:X}); skipped, nothing changed",
				cellID, a_why, list.count, list.data, list.fault);
			return;
		}

		++a_totals.cells;
		const auto cellName = DisplayName(a_cell);
		REX::INFO("ParcelFix: cell {:08X} '{}' ({}) - {} slot(s)", cellID, cellName, a_why, list.count);

		std::vector<Candidate> candidates;
		std::vector<RefProbe> createdDiag;
		std::vector<RefProbe> nearDiag;
		std::size_t walked = 0;
		std::size_t created = 0;
		std::size_t unreadable = 0;
		std::size_t slotFaults = 0;
		std::size_t outOfRange = 0;
		std::size_t notCreated = 0;
		std::size_t flaggedOut = 0;
		std::size_t notSellable = 0;
		const auto walkStart = std::chrono::steady_clock::now();

		for (std::uint32_t i = 0; i < list.count; ++i) {
			++walked;
			std::uint32_t slotFault = 0;
			auto* ref = RefAt(list.data, i, &slotFault);
			if (slotFault) {
				++slotFaults;
				continue;
			}
			if (!ref) {
				continue;
			}
			auto probe = ProbeRef(ref, a_origin);
			if (probe.fault) {
				++unreadable;
				continue;
			}
			// The player's own reference is not a parcel, but it can be a created
			// one; it is dropped before the filters so the counts stay readable.
			if (probe.created) {
				++created;
				if (a_config.diagnose && createdDiag.size() < kMaxCreatedLines) {
					createdDiag.push_back(probe);
				}
			} else if (a_config.diagnose && probe.distance <= 400.0f && nearDiag.size() < kMaxNearLines) {
				nearDiag.push_back(probe);
			}

			if (probe.distance > a_config.radius) {
				++outOfRange;
				continue;
			}
			if (!probe.created) {
				++notCreated;
				continue;
			}
			// bFlagsOnly: the 1.0.2-1.0.4 test, kept for the case where the broad
			// rule turns out to be picking up something it should not.
			if (a_config.flagsOnly && !probe.persistent && !probe.deleted && !probe.disabled) {
				++flaggedOut;
				continue;
			}
			if (!probe.sellable) {
				++notSellable;
				continue;
			}
			// The same reference can be in the local cell and in the worldspace's
			// persistent cell; it is judged once.
			if (!a_seen.insert(probe.refID).second) {
				continue;
			}

			Candidate candidate;
			candidate.ref = ref;
			candidate.probe = probe;
			// The base object carries the name; the reference itself usually has
			// none.
			candidate.name = DisplayName(probe.base);
			candidates.push_back(std::move(candidate));
		}

		// Created references first: they are the list this log exists for, and the
		// near-player filler must not be able to push them off the page.
		for (const auto& probe : createdDiag) {
			LogProbe("created", probe);
		}
		for (const auto& probe : nearDiag) {
			LogProbe("near", probe);
		}

		const auto cellMs = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - walkStart).count();
		REX::INFO("ParcelFix: cell {:08X} walked {} slot(s) in {} ms - {} runtime-created, "
				  "{} unreadable, {} slot fault(s), {} candidate(s)",
			cellID, walked, cellMs, created, unreadable, slotFaults, candidates.size());
		REX::INFO("ParcelFix: cell {:08X} rejected - out of range {}, not runtime-created {}, "
				  "without a save flag {}, base type not sold {}",
			cellID, outOfRange, notCreated, flaggedOut, notSellable);

		a_totals.walked += walked;
		a_totals.created += created;
		a_totals.faults += unreadable + slotFaults;
		a_totals.outOfRange += outOfRange;
		a_totals.flaggedOut += flaggedOut;
		a_totals.notSellable += notSellable;

		for (auto& candidate : candidates) {
			const auto& p = candidate.probe;
			REX::INFO("ParcelFix: candidate cell {:08X} ref {:08X} base {:08X} type={} '{}' distance {:.0f} "
					  "at {:.0f},{:.0f},{:.0f} persistent={} deleted={} disabled={}",
				cellID, p.refID, p.baseID, p.type, candidate.name, p.distance,
				p.x, p.y, p.z, p.persistent ? 1 : 0, p.deleted ? 1 : 0, p.disabled ? 1 : 0);
			++a_totals.candidates;
			if (!a_config.act) {
				continue;
			}
			// Non-persistent keeps it out of the next save; disabled keeps it
			// out of the world whether or not the flag survives. Neither frees
			// the reference, so no other list can be left holding a dangling
			// pointer by this.
			bool shrunk = false;
			if (const auto fault = HideRef(candidate.ref, shrunk); fault == 0) {
				++a_totals.hidden;
				// Read it back: this proves the write landed on the live object, so a
				// later run that still shows the old position is a save that did not
				// keep it rather than a write that never happened.
				const auto after = ProbeRef(candidate.ref, a_origin);
				REX::INFO("ParcelFix: hidden ref {:08X} now at {:.0f},{:.0f},{:.0f} persistent={} "
						  "deleted={} disabled={} shrunk={}",
					p.refID, after.x, after.y, after.z, after.persistent ? 1 : 0,
					after.deleted ? 1 : 0, after.disabled ? 1 : 0, shrunk ? 1 : 0);
			} else {
				REX::WARN("ParcelFix: ref {:08X} could not be hidden (fault 0x{:08X}); left as it was",
					p.refID, fault);
			}
		}
	}

	void Sweep(const Config& a_config)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			REX::WARN("ParcelFix: no player; nothing to scan");
			return;
		}
		auto* cell = player->parentCell;
		if (!cell) {
			REX::WARN("ParcelFix: player has no parent cell; nothing to scan");
			return;
		}

		// Logged before the walk, and without touching a name: this line is what
		// proves the scan started, whatever happens next.
		const auto cellID = CellID(cell);
		REX::INFO("ParcelFix: scan start cell={:08X} radius={:.0f} act={} flagsOnly={}",
			cellID, a_config.radius, a_config.act ? 1 : 0, a_config.flagsOnly ? 1 : 0);
		if (cellID == 0) {
			REX::WARN("ParcelFix: player cell has no form id; scan skipped");
			return;
		}

		const auto origin = player->GetPosition();

		struct Target
		{
			RE::TESObjectCELL* cell{ nullptr };
			const char*        why{ "" };
		};
		std::vector<Target> targets;
		targets.push_back({ cell, "player cell" });

		if (auto* space = RawWorldspace(cell); space && Readable(space)) {
			for (std::uint32_t which = 0; which < 2; ++which) {
				auto* persistent = RawPersistentCell(space, which);
				if (!persistent || persistent == cell || !Readable(persistent)) {
					continue;
				}
				if (CellID(persistent) == 0) {
					continue;
				}
				targets.push_back({ persistent, which == 0 ? "worldspace persistent cell (0x0D0)" :
															 "worldspace persistent cell (0x1D0)" });
			}
		} else {
			REX::INFO("ParcelFix: cell {:08X} has no worldspace (interior); only the player cell is walked", cellID);
		}

		REX::INFO("ParcelFix: walking {} cell(s)", targets.size());

		std::unordered_set<std::uint32_t> seen;
		SweepTotals totals;
		for (const auto& target : targets) {
			SweepCell(a_config, target.cell, target.why, origin, seen, totals);
		}

		REX::INFO("ParcelFix: sweep done - {} cell(s), {} slot(s), {} runtime-created, {} fault(s), "
				  "{} candidate(s), {} hidden",
			totals.cells, totals.walked, totals.created, totals.faults, totals.candidates, totals.hidden);
		if (a_config.act) {
			REX::INFO("ParcelFix: {} reference(s) hidden. Save now, then the plugin can be removed.",
				totals.hidden);
		} else {
			REX::INFO("ParcelFix: dry run (bAct=0) - {} candidate(s) listed, nothing changed",
				totals.candidates);
		}
	}

	// One sweep per cell per session. A load replaces the player's cell, so a
	// second save loaded without restarting the game is swept too; a cell that
	// has already been handled is not scanned again when the player walks back
	// into it.
	class SweepTask : public SFSE::ITaskDelegate
	{
	public:
		void Run() override
		{
			if (!config.enabled) {
				return;
			}
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return;
			}
			// Loading screen still up: the player and a cell both exist by then,
			// but that cell is a placeholder whose reference array the game is
			// still filling. Walking it here is what killed 1.0.1. The settle
			// timer restarts from the moment the screen goes away.
			if (GameIsBusy()) {
				pendingCell = 0;
				wait = kSettleFrames;
				return;
			}
			auto* cell = player->parentCell;
			if (!cell) {
				// Between a load and the world coming back: start the settle
				// timer over, so the scan never runs against a half-built cell.
				pendingCell = 0;
				return;
			}
			const auto id = CellID(cell);
			if (id == 0) {
				pendingCell = 0;
				return;
			}
			if (handled.contains(id)) {
				return;
			}
			if (id != pendingCell) {
				pendingCell = id;
				wait = kSettleFrames;
				return;
			}
			if (--wait > 0) {
				return;
			}
			handled.insert(id);
			Sweep(config);
		}

		void Destroy() override { delete this; }

	private:
		bool GameIsBusy()
		{
			auto* ui = RE::UI::GetSingleton();
			// No UI yet means the game has not finished starting either.
			return !ui || RawIsMenuOpen(ui, loadingMenu);
		}

		Config                            config{ ReadConfig() };
		RE::BSFixedString                 loadingMenu{ "LoadingMenu" };
		std::unordered_set<std::uint32_t> handled;
		std::uint32_t                     pendingCell{ 0 };
		int                               wait{ kSettleFrames };
	};

	void MessageHandler(SFSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}
		if (a_msg->type == SFSE::MessagingInterface::kPostPostLoad) {
			if (const auto tasks = SFSE::GetTaskInterface()) {
				tasks->AddPermanentTask(new SweepTask());
				REX::INFO("ParcelFix: sweep task registered");
			} else {
				REX::ERROR("ParcelFix: failed to get the task interface; nothing will run");
			}
		}
	}
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
	SFSE::Init(a_sfse);
	const auto messaging = SFSE::GetMessagingInterface();
	if (!messaging) {
		return false;
	}
	messaging->RegisterListener(MessageHandler);
	REX::INFO("ParcelFix v{}: loaded; clears shop parcels left by builds before 0.9.0", kVersion);
	return true;
}
