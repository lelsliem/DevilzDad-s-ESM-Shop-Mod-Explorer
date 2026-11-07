#!/usr/bin/env python3
"""Find the `BSComponentDB2::DBFileIndex` instances the parent-index dumper reads.

`db2_layout.py` recovers the index's *shape*; `parent_index.py` consumes the
sidecar. This module answers the question in between: where, in a running game,
is there an index to read at all?

There is no static one, and that is a measurement rather than an assumption
(`python db2_instance.py --exe <Starfield.exe>` re-derives it):

  * The class is not polymorphic. RTTI in `Starfield.exe` names
    `BSComponentDB2::DBFileIndex::IDFixupVisitor` and nothing else from the
    family, so there is no TypeDescriptor, no vtable and no way to recognise an
    instance by type.
  * No code materialises `BSComponentDB2::DB`'s vtable either: a whole-`.text`
    scan finds **zero** `lea reg,[rip + vtable]` sites for it, and no image
    section holds that pointer as data. Objects of this family are only ever
    reached through pointers that come from the loader, which means the index is
    built on the heap when a database is loaded.
  * There is more than one. The game merges `materials/materialsbeta.cdb` with
    every `materials/creations/<name>/materialsbeta.cdb` (this tree's own
    `LiveModel.cpp` relies on exactly that), and each loaded file has its own
    index. A single hard-coded address would be wrong even if one existed.

So the dumper enumerates instead of dereferencing a constant. What makes that
safe is that an index is very hard to mistake for anything else: three
`std::vector` headers whose element strides are 0x28 / 0x08 / 0x0c, and an edge
array in which every row's `Index` field is 0 and every `Type` field is the same
value (105 in the shipped database). `looks_like_index()` checks all of it on a
candidate address, and `scan()` walks committed, writable, private memory for
addresses that pass.

Usage
-----
    python db2_instance.py --exe <Starfield.exe>          # why there is no static one
    python db2_instance.py --json extracted/db2_instance.json
    python db2_instance.py --cpp                          # the dumper's scanner
    python db2_instance.py --attach Starfield.exe         # locate them, right now
    python db2_instance.py --attach Starfield.exe --dump  # + counts and edge type

`--attach` needs the game running and this process to be allowed to read it (same
user, no anti-cheat interference). It prints one line per index found, with the
address, the three element counts and the edge type, in the form the sidecar
header wants:

    # index <address> objects=<n> components=<n> edges=<n> edge-type=<t>

Exits non-zero when `--exe` cannot corroborate the layout, or when `--attach`
finds nothing - a dumper that reads no index must not look like success.
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import time

import rtti_vtable as R
import db2_layout as LAYOUT

HERE = os.path.dirname(os.path.abspath(__file__))

# The one edge relation the shipped database stores (`cdb_dbindex.EDGE_TYPE`).
# Not required - a creations database could carry another kind - but reported,
# because a dump whose edge type is not this one needs re-reading.
EXPECTED_EDGE_TYPE = 105

# How much of a candidate to trust. Sampling every stride-th row across the whole
# array would cost more than the scan; a spread of samples is enough to reject a
# lookalike, and the dumper re-checks every row it actually writes.
SAMPLE_ROWS = 48

HEADER_BYTES = 0x18  # MSVC std::vector: {first, last, capacity}


# ── the predicate ────────────────────────────────────────────────────────────

class Reader:
    """Uniform access to memory: a process, a dict of regions, or an image."""

    def read(self, address: int, size: int) -> bytes | None:
        raise NotImplementedError

    def u64(self, address: int) -> int | None:
        raw = self.read(address, 8)
        return struct.unpack("<Q", raw)[0] if raw and len(raw) == 8 else None


def _header(reader: Reader, address: int) -> tuple[int, int, int] | None:
    raw = reader.read(address, HEADER_BYTES)
    if not raw or len(raw) < HEADER_BYTES:
        return None
    return struct.unpack("<QQQ", raw)


def _vector(reader: Reader, address: int, stride: int, limit: int) -> dict | None:
    """A `std::vector<T>` header at `address`, validated, or None."""
    head = _header(reader, address)
    if head is None:
        return None
    first, last, cap = head
    if not first or not last or not cap:
        return None
    if not first <= last <= cap:
        return None
    if (last - first) % stride or (cap - first) % stride:
        return None
    count = (last - first) // stride
    if not 0 < count <= limit:
        return None
    return {"first": first, "last": last, "capacity": cap, "count": count}


def edge_sample(reader: Reader, edges: dict, stride: int, samples: int = SAMPLE_ROWS):
    """(index values, type values) over a spread of edge rows.

    The rows are read from the vector's *own* `first` pointer, so a candidate
    whose header points at unreadable memory fails here rather than later.
    """
    count = edges["count"]
    step = max(1, count // samples)
    indexes: set[int] = set()
    types: set[int] = set()
    read_ok = 0
    for i in range(0, count, step):
        raw = reader.read(edges["first"] + i * stride, stride)
        if not raw or len(raw) < stride:
            return None
        _src, _dst, idx, typ = struct.unpack("<IIHH", raw)
        indexes.add(idx)
        types.add(typ)
        read_ok += 1
        if read_ok >= samples:
            break
    if read_ok < min(4, count):
        return None
    return indexes, types


def looks_like_index(reader: Reader, address: int, layout: dict,
                     min_edges: int = 1) -> dict | None:
    """The index's three vectors, if `address` really holds a DBFileIndex."""
    classes = layout["classes"]
    strides = {
        "ComponentTypes": classes["BSComponentDB2::DBFileIndex::ComponentTypeInfo"]["size"],
        "Objects": classes["BSComponentDB2::DBFileIndex::ObjectInfo"]["size"],
        "Components": classes["BSComponentDB2::DBFileIndex::ComponentInfo"]["size"],
        "Edges": classes["BSComponentDB2::DBFileIndex::EdgeInfo"]["size"],
    }
    fields = {f["name"]: f["offset"]
              for f in classes["BSComponentDB2::DBFileIndex"]["fields"]}
    found: dict[str, dict] = {}
    for name, limit in (("ComponentTypes", 0x4000), ("Objects", 0x4000000),
                        ("Components", 0x4000000), ("Edges", 0x4000000)):
        vector = _vector(reader, address + fields[name], strides[name], limit)
        if vector is None:
            return None
        found[name] = vector
    if found["Edges"]["count"] < min_edges:
        return None
    if not found["Objects"]["count"] or not found["Components"]["count"]:
        return None

    sample = edge_sample(reader, found["Edges"], strides["Edges"])
    if sample is None:
        return None
    indexes, types = sample
    # The edge record's `Index` is 0 for every row of every database this tree
    # has read; a candidate where it varies is a different structure.
    if indexes != {0}:
        return None
    if len(types) != 1:
        return None
    return {
        "address": address,
        "objects": found["Objects"]["count"],
        "components": found["Components"]["count"],
        "edges": found["Edges"]["count"],
        "component_types": found["ComponentTypes"]["count"],
        "edge_type": next(iter(types)),
    }


# ── walking memory ───────────────────────────────────────────────────────────

CHUNK = 1 << 20


def _checks(layout: dict) -> list[tuple[int, int]]:
    """(member offset, stride) in the order that rejects fastest.

    `Edges` is the cheapest discriminator - a 0x0c stride survives a random
    offset once in twelve - so checking it first means most offsets cost one
    unpack instead of three. Building this once per scan keeps the dict lookups
    out of the inner loop, which is where nearly all of the time goes.
    """
    classes = layout["classes"]
    fields = {f["name"]: f["offset"]
              for f in classes["BSComponentDB2::DBFileIndex"]["fields"]}
    strides = {
        "Edges": classes["BSComponentDB2::DBFileIndex::EdgeInfo"]["size"],
        "Objects": classes["BSComponentDB2::DBFileIndex::ObjectInfo"]["size"],
        "Components": classes["BSComponentDB2::DBFileIndex::ComponentInfo"]["size"],
    }
    return [(fields[name], stride) for name, stride in strides.items()]


def scan(reader: Reader, regions, layout: dict, min_edges: int = 1,
         step: int = 8, limit: int = 32) -> list[dict]:
    """Every address in `regions` that `looks_like_index` accepts.

    `regions` yields (begin, end) pairs of readable memory. Chunks are read once
    and pre-filtered in place; a per-address read would be one system call per
    qword, which over a few GB of heap is minutes rather than seconds.
    """
    out: list[dict] = []
    checks = _checks(layout)
    unpack = struct.unpack_from
    for begin, end in regions:
        address = begin
        while address < end:
            want = min(CHUNK + 0x88, end - address)
            block = reader.read(address, want)
            if not block:
                address += CHUNK
                continue
            stop = min(len(block) - 0x88, end - address - 0x88)
            for off in range(0, stop, step):
                for at, stride in checks:
                    first, last, cap = unpack("<QQQ", block, off + at)
                    if (first < 0x10000 or not first <= last <= cap
                            or (last - first) % stride or (cap - first) % stride
                            or not 0 < (last - first) // stride <= 0x4000000):
                        break
                else:
                    hit = looks_like_index(reader, address + off, layout,
                                           min_edges=min_edges)
                    if hit is not None:
                        out.append(hit)
                        if len(out) >= limit:
                            return out
            address += CHUNK
    return out


class BufferReader(Reader):
    """Reads from a dict of {base: bytes}; used by the tests and by `--attach`."""

    def __init__(self, regions: dict[int, bytes], block: bool = True) -> None:
        self.regions = regions
        self.block = block   # False = whole-buffer reads only (tests)

    def read(self, address: int, size: int) -> bytes | None:
        for base, data in self.regions.items():
            if base <= address and address + size <= base + len(data):
                return data[address - base:address - base + size]
        return None


# ── the running game ─────────────────────────────────────────────────────────

def attach(process_name: str):
    """(reader, regions) for a running process on Windows, or raise SystemExit."""
    if not sys.platform.startswith("win"):
        raise SystemExit("--attach reads a live process and needs Windows")
    import ctypes
    from ctypes import wintypes

    PROCESS_QUERY_INFORMATION = 0x0400
    PROCESS_VM_READ = 0x0010
    MEM_COMMIT = 0x1000
    PAGE_GUARD = 0x100
    PAGE_NOACCESS = 0x01
    MEM_PRIVATE = 0x20000

    k32 = ctypes.WinDLL("kernel32", use_last_error=True)

    class MEMORY_BASIC_INFORMATION64(ctypes.Structure):
        _fields_ = [("BaseAddress", ctypes.c_ulonglong),
                    ("AllocationBase", ctypes.c_ulonglong),
                    ("AllocationProtect", wintypes.DWORD),
                    ("__alignment1", wintypes.DWORD),
                    ("RegionSize", ctypes.c_ulonglong),
                    ("State", wintypes.DWORD),
                    ("Protect", wintypes.DWORD),
                    ("Type", wintypes.DWORD),
                    ("__alignment2", wintypes.DWORD)]

    class ProcessReader(Reader):
        def __init__(self, handle) -> None:
            self.handle = handle

        def read(self, address: int, size: int) -> bytes | None:
            buffer = ctypes.create_string_buffer(size)
            got = ctypes.c_size_t(0)
            ok = k32.ReadProcessMemory(self.handle, ctypes.c_void_p(address), buffer,
                                       size, ctypes.byref(got))
            if not ok or got.value != size:
                return None
            return buffer.raw

    pids = _pids_by_name(process_name, k32)
    if not pids:
        raise SystemExit("no process named %r is running" % process_name)
    handle = None
    for pid in pids:
        candidate = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                                    False, pid)
        if candidate:
            handle = candidate
            break
    if not handle:
        raise SystemExit("could not open %r (%d) - same user, and try running elevated"
                         % (process_name, pids[0]))

    regions = []
    address = 0
    mbi = MEMORY_BASIC_INFORMATION64()
    while address < (1 << 47):
        got = k32.VirtualQueryEx(handle, ctypes.c_void_p(address), ctypes.byref(mbi),
                                 ctypes.sizeof(mbi))
        if not got:
            break
        base = mbi.BaseAddress
        size = mbi.RegionSize or 0x1000
        readable = (mbi.State == MEM_COMMIT and mbi.Type == MEM_PRIVATE
                    and not mbi.Protect & PAGE_GUARD
                    and mbi.Protect & 0xFF not in (PAGE_NOACCESS, 0x02))
        # 0x200 bytes is the smallest index worth reading; the cap keeps a
        # gigantic committed reservation from being touched end to end.
        if readable and 0x200 <= size <= 1 << 30:
            regions.append((base, base + size))
        address = base + size
    return ProcessReader(handle), regions


def _pids_by_name(name: str, k32) -> list[int]:
    import ctypes
    from ctypes import wintypes

    TH32CS_SNAPPROCESS = 0x2

    class PROCESSENTRY32W(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD),
                    ("cntUsage", wintypes.DWORD),
                    ("th32ProcessID", wintypes.DWORD),
                    ("th32DefaultHeapID", ctypes.c_void_p),
                    ("th32ModuleID", wintypes.DWORD),
                    ("cntThreads", wintypes.DWORD),
                    ("th32ParentProcessID", wintypes.DWORD),
                    ("pcPriClassBase", ctypes.c_long),
                    ("dwFlags", wintypes.DWORD),
                    ("szExeFile", wintypes.WCHAR * 260)]

    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snap in (0, -1):
        return []
    out: list[int] = []
    try:
        entry = PROCESSENTRY32W()
        entry.dwSize = ctypes.sizeof(entry)
        more = k32.Process32FirstW(snap, ctypes.byref(entry))
        while more:
            if entry.szExeFile.lower() == name.lower():
                out.append(entry.th32ProcessID)
            more = k32.Process32NextW(snap, ctypes.byref(entry))
    finally:
        k32.CloseHandle(snap)
    return out


# ── the static question ──────────────────────────────────────────────────────

def static_evidence(exe: str) -> dict:
    """Why a dumper cannot just dereference one address."""
    img = R.Image(exe)
    layout = LAYOUT.recover(exe)
    xrefs = LAYOUT._lea_xrefs(img)

    rtti = {name: td for name, td in R.descriptors(img).items()
            if "DBFileIndex" in name}
    db_vtables = [(name, vt) for name, _td, _col, vt, _m in R.resolve(img, r"^DB@BSComponentDB2$")]

    lea_to_vtable = 0
    holders = 0
    for _name, vt in db_vtables:
        site = xrefs.get(vt)
        if site is not None:
            lea_to_vtable += 1
        if img.data.find(struct.pack("<Q", img.base + vt)) >= 0:
            holders += 1

    return {
        "tool": "db2_instance.py",
        "exe": layout["exe"],
        "index_class": "BSComponentDB2::DBFileIndex",
        "index_is_polymorphic": "BSComponentDB2::DBFileIndex" in rtti,
        "rtti_names_in_family": sorted(rtti),
        "db_vtables": [{"class": n, "vtable": vt} for n, vt in db_vtables],
        "lea_sites_to_db_vtable": lea_to_vtable,
        "image_sections_holding_db_vtable_pointer": holders,
        "layout_problems": layout["problems"],
    }


# ── output ───────────────────────────────────────────────────────────────────

def _fmt_report(evidence: dict) -> str:
    exe = evidence["exe"]
    lines = [
        "%s  (%s, sha256 %s)" % (exe["file_version"], exe["path"], exe["sha256"][:16]),
        "",
        "static instance: none reachable",
        "  class %s is %s" % (evidence["index_class"],
                               "polymorphic" if evidence["index_is_polymorphic"]
                               else "NOT polymorphic - no TypeDescriptor, no vtable"),
        "  RTTI names in this family: %s" % ", ".join(evidence["rtti_names_in_family"]),
        "  BSComponentDB2::DB vtables: %s"
        % ", ".join("%s@%#x" % (v["class"], v["vtable"]) for v in evidence["db_vtables"]),
        "  lea sites that materialise those vtables: %d"
        % evidence["lea_sites_to_db_vtable"],
        "  image sections holding that vtable pointer as data: %d"
        % evidence["image_sections_holding_db_vtable_pointer"],
        "",
        "consequence: the index is built on the heap when a database loads, one per",
        "file (base materialsbeta.cdb plus every materials/creations/*/materialsbeta.cdb),",
        "so the dumper enumerates them instead of reading a fixed address:",
        "    db2_instance.py --attach Starfield.exe --dump",
    ]
    if evidence["layout_problems"]:
        lines += ["", "PROBLEM: the layout is not corroborated:"] + \
                 ["  " + p for p in evidence["layout_problems"]]
    return "\n".join(lines)


CPP_HEAD = """// Generated by tools/renderer_recon/db2_instance.py - do not edit.
// Starfield %(version)s, sha256 %(sha)s
//
// The offsets come from the class-registration functions (db2_layout.py) and are
// valid only on that build, so the caller gates on BuildGate::Verified() first.
//
// There is no address to hard-code: the index is heap-allocated per loaded CDB
// and this build has no RTTI for it. Enumerate instead. %(why)s
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace SFE::Db2Index
{
%(constants)s
	struct Found
	{
		std::uintptr_t address;
		std::size_t    objects;
		std::size_t    components;
		std::size_t    edges;
		std::uint16_t  edgeType;
	};

	// Every DBFileIndex in this process whose arrays validate. Empty when the
	// databases have not loaded yet (call this after the main menu, not at load).
	[[nodiscard]] std::vector<Found> FindAll();
}
"""

CPP_IMPL = """// Generated by tools/renderer_recon/db2_instance.py - do not edit.
#include "Db2Index.h"

#include <windows.h>

#include <cstring>

namespace SFE::Db2Index
{
	namespace
	{
		constexpr std::size_t kSampleRows = %(samples)d;
		constexpr std::size_t kChunkBytes = 1 << 18;
		constexpr std::size_t kMaxRegion  = 1ull << 28;

		bool Readable(const void* a_address, std::size_t a_size)
		{
			auto address = reinterpret_cast<std::uintptr_t>(a_address);
			while (a_size)
			{
				MEMORY_BASIC_INFORMATION mbi{};
				if (!VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)))
				{
					return false;
				}
				if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_NOACCESS) ||
					(mbi.Protect & PAGE_GUARD))
				{
					return false;
				}
				const auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
				const auto span = base + mbi.RegionSize - address;
				if (span >= a_size)
				{
					return true;
				}
				a_size -= span;
				address += span;
			}
			return true;
		}

		// A std::vector header: {first, last, capacity}, all inside readable memory,
		// aligned to the element stride.
		bool VectorAt(std::uintptr_t a_base, std::ptrdiff_t a_member, std::size_t a_stride,
			std::size_t a_limit, std::size_t& a_count)
		{
			std::uintptr_t first = 0, last = 0, cap = 0;
			if (!Readable(reinterpret_cast<const void*>(a_base + a_member), 0x18))
			{
				return false;
			}
			std::memcpy(&first, reinterpret_cast<const void*>(a_base + a_member), 8);
			std::memcpy(&last, reinterpret_cast<const void*>(a_base + a_member + 8), 8);
			std::memcpy(&cap, reinterpret_cast<const void*>(a_base + a_member + 0x10), 8);
			if (!first || !last || !cap || first > last || last > cap)
			{
				return false;
			}
			if ((last - first) %% a_stride || (cap - first) %% a_stride)
			{
				return false;
			}
			a_count = (last - first) / a_stride;
			return a_count > 0 && a_count <= a_limit;
		}

		bool Accepts(std::uintptr_t a_base, Found& a_found)
		{
			std::size_t types = 0, objects = 0, components = 0, edges = 0;
			if (!VectorAt(a_base, kComponentTypes, kComponentTypeStride, 0x4000, types) ||
				!VectorAt(a_base, kObjects, kObjectStride, 0x4000000, objects) ||
				!VectorAt(a_base, kComponents, kComponentStride, 0x4000000, components) ||
				!VectorAt(a_base, kEdges, kEdgeStride, 0x4000000, edges))
			{
				return false;
			}

			std::uintptr_t first = 0;
			std::memcpy(&first, reinterpret_cast<const void*>(a_base + kEdges), 8);
			const auto step = edges / kSampleRows ? edges / kSampleRows : 1;
			int samples = 0;
			std::uint16_t type = 0;
			for (std::size_t i = 0; i < edges && samples < static_cast<int>(kSampleRows); i += step)
			{
				const auto row = first + i * kEdgeStride;
				if (!Readable(reinterpret_cast<const void*>(row), kEdgeStride))
				{
					return false;
				}
				std::uint16_t index = 0, kind = 0;
				std::memcpy(&index, reinterpret_cast<const void*>(row + kEdgeIndex), 2);
				std::memcpy(&kind, reinterpret_cast<const void*>(row + kEdgeType), 2);
				// Every row of every database this project has read carries
				// Index == 0, and all rows of one database share the type.
				if (index != 0 || (samples && kind != type))
				{
					return false;
				}
				type = kind;
				++samples;
			}
			if (samples == 0)
			{
				return false;
			}

			a_found = { a_base, objects, components, edges, type };
			return true;
		}
	}

	// The cheap test, run on bytes already copied out of the region: do three
	// vector headers divide by their strides at this offset? A per-offset
	// VirtualQuery would be ~4 syscalls per qword, which over a heap this size is
	// minutes instead of seconds.
	bool Plausible(const std::byte* a_block, std::size_t a_offset)
	{
		struct Member { std::ptrdiff_t at; std::size_t stride; };
		static constexpr Member kMembers[] = {
			{ kObjects, kObjectStride },
			{ kComponents, kComponentStride },
			{ kEdges, kEdgeStride },
		};
		for (const auto& member : kMembers)
		{
			std::uintptr_t first = 0, last = 0, cap = 0;
			std::memcpy(&first, a_block + a_offset + member.at, 8);
			std::memcpy(&last, a_block + a_offset + member.at + 8, 8);
			std::memcpy(&cap, a_block + a_offset + member.at + 0x10, 8);
			if (!first || !last || !cap || first > last || last > cap)
			{
				return false;
			}
			if ((last - first) %% member.stride || (cap - first) %% member.stride)
			{
				return false;
			}
			const auto count = (last - first) / member.stride;
			if (!count || count > 0x4000000)
			{
				return false;
			}
		}
		return true;
	}

	void ScanRegion(std::uintptr_t a_base, std::size_t a_size, std::vector<Found>& a_out)
	{
		std::vector<std::byte> block(kChunkBytes + kIndexSize);
		for (auto offset = std::size_t{ 0 }; offset + kIndexSize < a_size; offset += kChunkBytes)
		{
			const auto room = a_size - offset;
			const auto want = room < kChunkBytes + kIndexSize ? room : kChunkBytes + kIndexSize;
			// The region was committed when the walk reached it, but a chunk can
			// still cross into a page that has since been freed or guarded.
			if (!Readable(reinterpret_cast<const void*>(a_base + offset), want))
			{
				continue;
			}
			std::memcpy(block.data(), reinterpret_cast<const void*>(a_base + offset), want);
			for (auto at = std::size_t{ 0 }; at + kIndexSize <= want; at += 8)
			{
				if (!Plausible(block.data(), at))
				{
					continue;
				}
				Found hit{};
				if (Accepts(a_base + offset + at, hit))
				{
					a_out.push_back(hit);
				}
			}
		}
	}

	std::vector<Found> FindAll()
	{
		std::vector<Found> found;
		auto address = std::uintptr_t{ 0 };
		MEMORY_BASIC_INFORMATION mbi{};
		while (VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)))
		{
			const auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
			const auto size = static_cast<std::size_t>(mbi.RegionSize);
			const bool heap = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
				!(mbi.Protect & PAGE_GUARD) && !(mbi.Protect & PAGE_NOACCESS);
			// An index is 0x88 bytes plus its arrays; a tiny region cannot hold one
			// and a huge reservation is not worth walking end to end.
			if (heap && size >= 0x200 && size <= kMaxRegion)
			{
				ScanRegion(base, size, found);
			}
			if (base + size <= address)
			{
				break;
			}
			address = base + size;
		}
		return found;
	}
}
"""


def _cpp(evidence: dict, layout: dict) -> tuple[str, str]:
    classes = layout["classes"]
    index = classes["BSComponentDB2::DBFileIndex"]
    fields = {f["name"]: f["offset"] for f in index["fields"]}
    strides = {
        "ComponentTypeInfo": classes["BSComponentDB2::DBFileIndex::ComponentTypeInfo"]["size"],
        "ObjectInfo": classes["BSComponentDB2::DBFileIndex::ObjectInfo"]["size"],
        "ComponentInfo": classes["BSComponentDB2::DBFileIndex::ComponentInfo"]["size"],
        "EdgeInfo": classes["BSComponentDB2::DBFileIndex::EdgeInfo"]["size"],
    }
    edge = {f["name"]: f["offset"]
            for f in classes["BSComponentDB2::DBFileIndex::EdgeInfo"]["fields"]}
    constants = "\n".join([
        "\tconstexpr std::size_t    kIndexSize           = %#x;" % index["size"],
        "\tconstexpr std::ptrdiff_t kComponentTypes      = %#x;" % fields["ComponentTypes"],
        "\tconstexpr std::ptrdiff_t kObjects             = %#x;" % fields["Objects"],
        "\tconstexpr std::ptrdiff_t kComponents          = %#x;" % fields["Components"],
        "\tconstexpr std::ptrdiff_t kEdges               = %#x;" % fields["Edges"],
        "\tconstexpr std::size_t    kComponentTypeStride = %#x;" % strides["ComponentTypeInfo"],
        "\tconstexpr std::size_t    kObjectStride        = %#x;" % strides["ObjectInfo"],
        "\tconstexpr std::size_t    kComponentStride     = %#x;" % strides["ComponentInfo"],
        "\tconstexpr std::size_t    kEdgeStride          = %#x;" % strides["EdgeInfo"],
        "\tconstexpr std::ptrdiff_t kEdgeIndex           = %#x;" % edge["Index"],
        "\tconstexpr std::ptrdiff_t kEdgeType            = %#x;" % edge["Type"],
    ])
    head = CPP_HEAD % {
        "version": evidence["exe"]["file_version"],
        "sha": evidence["exe"]["sha256"],
        "constants": constants,
        "why": "See db2_instance.py --exe for the evidence.",
    }
    return head, CPP_IMPL % {"samples": SAMPLE_ROWS}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--exe", default=LAYOUT.DEFAULT_EXE)
    ap.add_argument("--json", default="")
    ap.add_argument("--cpp", default="", help="write Db2Index.h/.cpp into this directory")
    ap.add_argument("--attach", default="", help="process name to scan (e.g. Starfield.exe)")
    ap.add_argument("--dump", action="store_true", help="per-index counts and edge type")
    ap.add_argument("--min-edges", type=int, default=1)
    args = ap.parse_args()

    if not os.path.exists(args.exe):
        print("no Starfield.exe at %s" % args.exe, file=sys.stderr)
        return 2
    layout = LAYOUT.recover(args.exe)
    evidence = static_evidence(args.exe)
    print(_fmt_report(evidence))
    if args.json:
        blob = {"evidence": evidence, "layout": layout}
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(blob, fh, indent=2)
            fh.write("\n")
        print("\nwrote %s" % args.json)
    if args.cpp:
        head, impl = _cpp(evidence, layout)
        os.makedirs(args.cpp, exist_ok=True)
        for name, text in (("Db2Index.h", head), ("Db2Index.cpp", impl)):
            with open(os.path.join(args.cpp, name), "w", encoding="utf-8") as fh:
                fh.write(text)
        print("\nwrote %s/Db2Index.h and Db2Index.cpp" % args.cpp)

    status = 1 if evidence["layout_problems"] else 0
    if args.attach:
        reader, regions = attach(args.attach)
        t0 = time.time()
        hits = scan(reader, regions, layout, min_edges=args.min_edges)
        print("\nscanned %d heap regions in %.1fs" % (len(regions), time.time() - t0))
        if not hits:
            print("no DBFileIndex found - is the game past the main menu?")
            return 1
        for hit in hits:
            if args.dump:
                print("# index %#x objects=%d components=%d edges=%d edge-type=%d types=%d"
                      % (hit["address"], hit["objects"], hit["components"],
                         hit["edges"], hit["edge_type"], hit["component_types"]))
            else:
                print("# index %#x" % hit["address"])
    return status


if __name__ == "__main__":
    raise SystemExit(main())
