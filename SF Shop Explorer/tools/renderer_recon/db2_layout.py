#!/usr/bin/env python3
"""Recover the field layout of `BSComponentDB2::DBFileIndex` from Starfield.exe.

`parent_index.py` reads `extracted/parent_index.tsv`, and only the game can write
it: the id -> name -> parent pairing exists in `BSComponentDB2::DBFileIndex` at
runtime and nowhere in `materialsbeta.cdb`. To write that sidecar a dumper has to
dereference the index's arrays, which means it needs the member offsets and the
element strides for a specific build. Those are not in the SFSE SDK, not in
CommonLibSF, and not in the Address Library, so they are read out of the binary
here - once, reproducibly, with the evidence kept.

How the engine states its own layout
------------------------------------
Every reflected class is registered by one function that fills a class schema:

    lea  rax, [rip + class name]     ; "BSComponentDB2::DBFileIndex"
    mov  [rcx], rax
    mov  dword [rcx + 8], 0x88       ; sizeof(class)
    ...
    lea  rdx, [rip + field name]     ; "Edges"
    lea  rcx, [rip + type info]      ; the reflected type of that member
    mov  dword [rsp + A], 0x68       ; its offset inside the class
    call <ClassSchema::AddField>
    ...
    lea  edx, [r14 + 0x38]           ; sizeof(field descriptor)
    lea  r8d, [r14 + 5]              ; how many fields were added
    call <register class>

So the triple (name, type, offset) is written by the code that owns the truth.
Two traps cost time here:

  * An offset is not always an immediate. `ComponentTypes` sits at offset 0 and
    the compiler stores it from a register that was zeroed earlier, so a scanner
    that only reads `mov dword [rsp + A], imm` silently loses every first member
    of every class - and ObjectInfo's PersistentID, ComponentInfo's ObjectID and
    EdgeInfo's SourceID with them. `_slots` therefore carries a small constant
    tracker, and maps the 32-bit register a compiler uses for a zero (`r14d`)
    onto the 64-bit one that stores it (`r14`).
  * The order of the three writes is not fixed: `DBFileIndex` writes name, then
    type, then offset, while `ObjectInfo` loads the type into `r9`/`r10` *before*
    the name block starts. Pairing "the next `lea`" with "the next small store"
    therefore reports `Parent@0x14` and `EdgeInfo.TargetID@0x08`. `_slots` keys
    every store by stack slot instead, and `_fields_from_slots` reads the triple
    as `[offset - 0x10]` name, `[offset - 0x08]` type, `[offset]` offset.

The container fields
--------------------
`Objects`, `Components` and `Edges` are 0x18 bytes and their reflected type's
vtable reports `std::vector`, so the header is MSVC's three pointers
`{first, last, capacity}` and the element count is `(last - first) / sizeof(T)`.
`vector_evidence()` proves both halves of that from the binary (the name, and a
method that zeroes exactly those three members before calling the allocator),
rather than trusting the name alone.

Usage
-----
    python db2_layout.py                       # print the recovered layout
    python db2_layout.py --json extracted/db2_layout.json
    python db2_layout.py --cpp                 # constants for the dumper
    python db2_layout.py --exe "D:\\...\\Starfield.exe"

Exits non-zero when the binary does not corroborate the results (a field outside
its class, two fields overlapping, or a container whose type is not a vector) -
the dumper is gated on this layout, so a wrong answer must fail here instead of
writing offsets into the game.
"""
from __future__ import annotations

import argparse
import bisect
import hashlib
import json
import os
import re
import struct
import sys

import capstone

import rtti_vtable as R

HERE = os.path.dirname(os.path.abspath(__file__))

DEFAULT_EXE = r"B:\SteamLibrary\steamapps\common\Starfield\Starfield.exe"

# The classes whose layout the sidecar dump needs. Order matters only for the
# output; each is found by its own def-use chain.
CLASSES = [
    "BSComponentDB2::DBFileIndex",
    "BSComponentDB2::DBFileIndex::ObjectInfo",
    "BSComponentDB2::DBFileIndex::ComponentTypeInfo",
    "BSComponentDB2::DBFileIndex::ComponentInfo",
    "BSComponentDB2::DBFileIndex::EdgeInfo",
]

# Which reflected class each std::vector holds. The engine does not put this in
# the field descriptor - a container's element type is not reflected - so it is
# named here and then *checked*: the dumper divides (last - first) by the stride
# it finds and must get a whole number, and the element offsets were recovered
# from these classes' own registration functions.
CONTAINER_ELEMENTS = {
    "BSComponentDB2::DBFileIndex.Objects": "BSComponentDB2::DBFileIndex::ObjectInfo",
    "BSComponentDB2::DBFileIndex.Components": "BSComponentDB2::DBFileIndex::ComponentInfo",
    "BSComponentDB2::DBFileIndex.Edges": "BSComponentDB2::DBFileIndex::EdgeInfo",
}

SMALLEST_PLAUSIBLE_SIZE = 0x10000  # a class in this family is far smaller


# ── reading the binary ───────────────────────────────────────────────────────

def _cstr(img: R.Image, rva: int, limit: int = 120) -> str | None:
    """The printable NUL-terminated string at `rva`, or None."""
    if rva is None or rva < 0:
        return None
    raw = img.read(rva, limit)
    if not raw:
        return None
    end = raw.find(b"\x00")
    if end < 0:
        return None
    text = raw[:end]
    if len(text) < 2:
        return None
    if not all(32 <= c < 127 for c in text):
        return None
    return text.decode("ascii")


def _u32(img: R.Image, rva: int) -> int | None:
    raw = img.read(rva, 4)
    return struct.unpack("<I", raw)[0] if len(raw) == 4 else None


def _u64(img: R.Image, rva: int) -> int | None:
    raw = img.read(rva, 8)
    return struct.unpack("<Q", raw)[0] if len(raw) == 8 else None


def _function_of(img: R.Image, rva: int, funcs: list[tuple[int, int]]) -> tuple[int, int] | None:
    i = bisect.bisect_right(funcs, (rva, 0xFFFFFFFF)) - 1
    if i < 0:
        return None
    begin, end = funcs[i]
    return (begin, end) if begin <= rva < end else None


_LEA_RIP = re.compile(rb"\x48\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d](.{4})|"
                      rb"\x4c\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d](.{4})", re.S)


def _lea_xrefs(img: R.Image) -> dict[int, int]:
    """target RVA -> the .text RVA of a `lea reg, [rip + target]` that names it.

    Cheaper than disassembling 15M instructions: the encoding is fixed at 7
    bytes, so the displacement can be resolved without decoding. Only the first
    site per target is kept, which is all a class-name lookup needs.
    """
    va, vsize, _raw, _rawsize = img.sections[".text"]
    base_off = img.off(va)
    code = img.data[base_off:base_off + vsize]
    out: dict[int, int] = {}
    for m in _LEA_RIP.finditer(code):
        site = va + m.start()
        disp_off = m.start() + 3
        target = site + 7 + struct.unpack_from("<i", code, disp_off)[0]
        out.setdefault(target, site)
    return out


def _class_registrations(img: R.Image, funcs: list[tuple[int, int]]) -> dict[str, tuple[int, int]]:
    """class name -> (begin, end) of the function that registers it."""
    xrefs = _lea_xrefs(img)
    found: dict[str, tuple[int, int]] = {}
    missing: list[str] = []
    for name in CLASSES:
        rva = img.data.find(b"\x00" + name.encode() + b"\x00")
        if rva < 0:
            missing.append(name)
            continue
        site = xrefs.get(img.rva(rva + 1))
        span = _function_of(img, site, funcs) if site is not None else None
        if span is None:
            missing.append(name)
        else:
            found[name] = span
    if missing:
        raise SystemExit("Starfield.exe does not register: %s" % ", ".join(missing))
    return found


# ── the field triples ────────────────────────────────────────────────────────

# Field descriptors are built on the stack as three adjacent members:
#
#     [slot - 0x10]  const char* name
#     [slot - 0x08]  type-info object (or a named type)
#     [slot]         uint32 offset
#
# and which of the three is written first differs per class: DBFileIndex writes
# name then type, ObjectInfo loads the type into r9/r10 before the name block
# even starts. So the pairing is done by slot arithmetic, not by order - an
# order-based scanner produced `Parent@0x14` and `EdgeInfo.TargetID@0x08`.
SLOT_NAME = 0x10
SLOT_TYPE = 0x08

STACK_BASES = (capstone.x86.X86_REG_RSP, capstone.x86.X86_REG_RBP, capstone.x86.X86_REG_R11)


def _widen(reg: int) -> int:
    """Map a 32-bit register id onto its 64-bit form (r14d and r14 are one)."""
    for wide, narrow in ((capstone.x86.X86_REG_R15, capstone.x86.X86_REG_R15D),
                         (capstone.x86.X86_REG_R14, capstone.x86.X86_REG_R14D),
                         (capstone.x86.X86_REG_R13, capstone.x86.X86_REG_R13D),
                         (capstone.x86.X86_REG_R12, capstone.x86.X86_REG_R12D),
                         (capstone.x86.X86_REG_RAX, capstone.x86.X86_REG_EAX),
                         (capstone.x86.X86_REG_RBX, capstone.x86.X86_REG_EBX),
                         (capstone.x86.X86_REG_RCX, capstone.x86.X86_REG_ECX),
                         (capstone.x86.X86_REG_RDX, capstone.x86.X86_REG_EDX),
                         (capstone.x86.X86_REG_RSI, capstone.x86.X86_REG_ESI),
                         (capstone.x86.X86_REG_RDI, capstone.x86.X86_REG_EDI),
                         (capstone.x86.X86_REG_R8, capstone.x86.X86_REG_R8D),
                         (capstone.x86.X86_REG_R9, capstone.x86.X86_REG_R9D),
                         (capstone.x86.X86_REG_R10, capstone.x86.X86_REG_R10D),
                         (capstone.x86.X86_REG_R11, capstone.x86.X86_REG_R11D)):
        if reg == narrow:
            return wide
    return reg


def _slots(img: R.Image, begin: int, end: int) -> dict[tuple[int, int], tuple[str, object]]:
    """Stack slot -> what was stored there, for one registration function."""
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    start, stop = img.off(begin), img.off(end)
    consts: dict[int, int] = {}
    lits: dict[int, tuple[str, object]] = {}
    slots: dict[tuple[int, int], tuple[str, object]] = {}
    for insn in md.disasm(img.data[start:stop], img.base + begin):
        ops = insn.operands
        if len(ops) < 2:
            continue
        dst, src = ops[0], ops[1]
        if insn.mnemonic in ("xor", "sub") and dst.type == capstone.x86.X86_OP_REG and \
                src.type == capstone.x86.X86_OP_REG and _widen(dst.reg) == _widen(src.reg):
            consts[_widen(dst.reg)] = 0
            lits.pop(_widen(dst.reg), None)
            continue
        if insn.mnemonic == "lea" and dst.type == capstone.x86.X86_OP_REG:
            reg = _widen(dst.reg)
            if src.type == capstone.x86.X86_OP_MEM and src.mem.base == capstone.x86.X86_REG_RIP:
                target = insn.address + insn.size + src.mem.disp - img.base
                text = _cstr(img, target)
                lits[reg] = ("name", text) if text else ("ptr", target)
                consts.pop(reg, None)
            elif src.type == capstone.x86.X86_OP_MEM and src.mem.base in consts:
                consts[reg] = consts[src.mem.base] + src.mem.disp
                lits.pop(reg, None)
            continue
        if insn.mnemonic != "mov":
            continue
        if dst.type == capstone.x86.X86_OP_REG:
            reg = _widen(dst.reg)
            if src.type == capstone.x86.X86_OP_IMM:
                consts[reg] = src.imm
                lits.pop(reg, None)
            elif src.type == capstone.x86.X86_OP_REG:
                other = _widen(src.reg)
                if other in consts:
                    consts[reg] = consts[other]
                else:
                    consts.pop(reg, None)
                if other in lits:
                    lits[reg] = lits[other]
                else:
                    lits.pop(reg, None)
            continue
        if dst.type != capstone.x86.X86_OP_MEM or dst.mem.base not in STACK_BASES:
            continue
        key = (_widen(dst.mem.base), dst.mem.disp)
        if src.type == capstone.x86.X86_OP_REG and _widen(src.reg) in lits:
            slots[key] = lits[_widen(src.reg)]
        elif src.type == capstone.x86.X86_OP_IMM and 0 <= src.imm < 0x400:
            slots[key] = ("int", src.imm)
        elif src.type == capstone.x86.X86_OP_REG and _widen(src.reg) in consts:
            value = consts[_widen(src.reg)]
            slots[key] = ("int", value) if 0 <= value < 0x400 else ("other", None)
        else:
            slots.pop(key, None)
    return slots


def _fields_from_slots(slots: dict[tuple[int, int], tuple[str, object]]) -> list[dict]:
    """The (name, type, offset) triple centred on each small immediate store."""
    out: dict[tuple[str, int], dict] = {}
    for (base, disp), (kind, value) in slots.items():
        if kind != "int":
            continue
        name = slots.get((base, disp - SLOT_NAME))
        ftype = slots.get((base, disp - SLOT_TYPE))
        if not name or name[0] != "name" or not ftype or ftype[0] not in ("name", "ptr"):
            continue
        field = {
            "name": name[1],
            "offset": value,
            "type_rva": ftype[1] if ftype[0] == "ptr" else None,
            "type_name": ftype[1] if ftype[0] == "name" else None,
        }
        # The descriptor is copied into a second buffer before the call, which
        # stores the same triple again a few slots away: first spelling wins.
        out.setdefault((field["name"], field["offset"]), field)
    return sorted(out.values(), key=lambda f: f["offset"])


def _type_of(img: R.Image, field: dict) -> dict:
    """Describe a member's reflected type as far as the file can be read."""
    rva = field.get("type_rva")
    if rva is None:
        return {"kind": "named", "name": field.get("type_name"), "size": None}
    size = _u32(img, rva + 8)
    if size is None or size > SMALLEST_PLAUSIBLE_SIZE:
        # The type-info object lives in uninitialised data and is filled by the
        # engine's static initialisers, so the file itself has nothing to say.
        return {"kind": "unreadable", "rva": rva, "name": None, "size": None}
    return {"kind": "info", "rva": rva, "name": _type_name(img, rva), "size": size}


def _type_name(img: R.Image, type_rva: int) -> str | None:
    """The name a type-info reports through its vtable, e.g. `std::vector`."""
    vptr = _u64(img, type_rva)
    if vptr is None or vptr < img.base:
        return None
    vt_rva = vptr - img.base
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    for slot in range(12):
        method = _u64(img, vt_rva + 8 * slot)
        if method is None or method < img.base:
            continue
        start = img.off(method - img.base)
        if start is None:
            continue
        first = next(md.disasm(img.data[start:start + 0x20], method), None)
        if first is None or first.mnemonic != "lea":
            continue
        ops = first.operands
        if len(ops) == 2 and ops[1].type == capstone.x86.X86_OP_MEM and \
                ops[1].mem.base == capstone.x86.X86_REG_RIP:
            text = _cstr(img, first.address + first.size + ops[1].mem.disp - img.base)
            if text:
                return text
    return None


def vector_evidence(img: R.Image, type_rva: int) -> dict:
    """Prove the 0x18-byte header is MSVC's `std::vector`.

    Two independent facts: the type-info's name method returns "std::vector", and
    a method of the same type-info zeroes exactly [obj], [obj+8], [obj+0x10]. A
    container that kept its size inline would show a further member here, and a
    `BSScrapArray`/`BSTArray` shape would not zero three pointers in a row.
    """
    named = _type_name(img, type_rva)
    vptr = _u64(img, type_rva)
    zeroed: set[int] = set()
    if vptr is not None and vptr >= img.base:
        vt_rva = vptr - img.base
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        md.detail = True
        for slot in range(12):
            method = _u64(img, vt_rva + 8 * slot)
            if method is None or method < img.base:
                continue
            start = img.off(method - img.base)
            if start is None:
                continue
            members: set[int] = set()
            for insn in md.disasm(img.data[start:start + 0x120], method):
                if insn.mnemonic == "mov" and len(insn.operands) == 2:
                    dst, src = insn.operands
                    if dst.type == capstone.x86.X86_OP_MEM and src.type == capstone.x86.X86_OP_IMM \
                            and src.imm == 0 and dst.mem.disp in (0, 8, 0x10) and \
                            dst.mem.base not in (capstone.x86.X86_REG_RSP, capstone.x86.X86_REG_RBP):
                        members.add(dst.mem.disp)
                if insn.mnemonic.startswith("ret"):
                    break
            if members == {0, 8, 0x10}:
                zeroed = members
                break
    return {"name": named, "header": sorted(zeroed),
            "is_vector": named == "std::vector" and zeroed == {0, 8, 0x10}}


def _validate(name: str, size: int, fields: list[dict]) -> list[str]:
    """Everything the binary must agree with for the layout to be usable."""
    problems: list[str] = []
    if not 0 < size <= SMALLEST_PLAUSIBLE_SIZE:
        problems.append("%s: implausible sizeof %#x" % (name, size))
    seen: dict[int, str] = {}
    for field in fields:
        off = field["offset"]
        if not 0 <= off < size:
            problems.append("%s.%s: offset %#x is outside the %#x-byte class"
                            % (name, field["name"], off, size))
        if off in seen:
            problems.append("%s: %s and %s share offset %#x"
                            % (name, seen[off], field["name"], off))
        seen[off] = field["name"]
        t = field["type"]
        if t.get("size") and off + t["size"] > size:
            problems.append("%s.%s: %s bytes at %#x overrun the %#x-byte class"
                            % (name, field["name"], t["size"], off, size))
    return problems


def _declared_field_count(img: R.Image, begin: int, end: int) -> int | None:
    """How many fields the registration says it added, when it says so at all.

    The count is the `lea r8d, [reg + N]` that precedes the call which installs
    the schema. EdgeInfo computes it in a register instead, so None is normal -
    but when it *is* an immediate it is the engine stating its own field count,
    and comparing it against what was recovered is the check that catches a
    silently dropped member (it caught two while this tool was being written).
    """
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    start, stop = img.off(begin), img.off(end)
    count = None
    recent: list[int | None] = []
    for insn in md.disasm(img.data[start:stop], img.base + begin):
        if insn.mnemonic == "call":
            immediate = [v for v in recent if v is not None]
            if immediate:
                count = immediate[-1]
            recent = []
            continue
        value = None
        if insn.mnemonic == "lea" and insn.op_str.startswith("r8d,"):
            ops = insn.operands
            if ops[1].type == capstone.x86.X86_OP_MEM and ops[1].mem.disp >= 0:
                value = ops[1].mem.disp
        recent.append(value)
        if len(recent) > 8:
            recent.pop(0)
    return count


def recover(exe: str) -> dict:
    """The layout, plus everything needed to argue that it is the right one."""
    img = R.Image(exe)
    funcs = R.function_ranges(img)
    spans = _class_registrations(img, funcs)
    classes: dict[str, dict] = {}
    problems: list[str] = []
    cache: dict[int, dict] = {}
    for name in CLASSES:
        begin, _end = spans[name]
        size = None
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        md.detail = True
        start = img.off(begin)
        prologue = img.data[start:start + 0x60]
        for insn in md.disasm(prologue, img.base + begin):
            if insn.mnemonic == "mov" and len(insn.operands) == 2:
                dst, src = insn.operands
                if dst.type == capstone.x86.X86_OP_MEM and dst.mem.base == capstone.x86.X86_REG_RCX \
                        and dst.mem.disp == 8 and src.type == capstone.x86.X86_OP_IMM:
                    size = src.imm
                    break
        fields = _fields_from_slots(_slots(img, begin, _end))
        for field in fields:
            key = field["type_rva"]
            if key is not None and key in cache:
                field["type"] = cache[key]
            else:
                described = _type_of(img, field)
                if key is not None:
                    cache[key] = described
                field["type"] = described
        if size is None:
            problems.append("%s: no sizeof in the registration prologue" % name)
            size = 0
        problems += _validate(name, size, fields)
        declared = _declared_field_count(img, begin, _end)
        if declared is not None and declared != len(fields):
            problems.append("%s: the registration declares %d fields but %d were recovered"
                            % (name, declared, len(fields)))
        classes[name] = {
            "registration_rva": begin,
            "register_rva_end": _end,
            "size": size,
            "declared_fields": declared,
            "fields": [{"name": f["name"], "offset": f["offset"], "type": f["type"]}
                       for f in fields],
        }

    containers: dict[str, dict] = {}
    for key, element in CONTAINER_ELEMENTS.items():
        cls, _, member = key.partition(".")
        field = next((f for f in classes[cls]["fields"] if f["name"] == member), None)
        if field is None:
            problems.append("%s.%s was not recovered" % (cls, member))
            continue
        t = field["type"]
        if t.get("kind") != "info":
            problems.append("%s.%s: no readable type-info (%s)" % (cls, member, t.get("kind")))
            continue
        evidence = vector_evidence(img, t["rva"])
        if not evidence["is_vector"]:
            problems.append("%s.%s: %r with header %s is not a std::vector"
                            % (cls, member, evidence["name"], evidence["header"]))
        containers[key] = {
            "element": element,
            "element_size": classes.get(element, {}).get("size"),
            "type_name": evidence["name"],
            "header_bytes": t["size"],
            "evidence": evidence,
            "count": "(last - first) / %s" % (hex(classes.get(element, {}).get("size") or 0)),
        }

    return {
        "tool": "db2_layout.py",
        "exe": {
            "path": os.path.abspath(exe),
            "sha256": hashlib.sha256(img.data).hexdigest(),
            "bytes": len(img.data),
            "image_base": hex(img.base),
            "file_version": file_version(img) or "unknown",
        },
        "classes": classes,
        "containers": containers,
        "problems": problems,
    }


# ── provenance ───────────────────────────────────────────────────────────────

def file_version(img: R.Image) -> str | None:
    """The VS_FIXEDFILEINFO file version, e.g. "1.16.244.0"."""
    try:
        pe = struct.unpack_from("<I", img.data, 0x3C)[0]
        optsz = struct.unpack_from("<H", img.data, pe + 20)[0]
        opt = pe + 24
        magic = struct.unpack_from("<H", img.data, opt)[0]
        dd = opt + (112 if magic == 0x20B else 96)
        rsrc_rva, _size = struct.unpack_from("<II", img.data, dd + 2 * 8)
        if not rsrc_rva:
            return None
        base = img.off(rsrc_rva)
        if base is None:
            return None

        def children(rel: int) -> list[tuple[int, int]]:
            at = base + rel
            named, ids = struct.unpack_from("<HH", img.data, at + 12)
            out = []
            for i in range(named + ids):
                name, off = struct.unpack_from("<II", img.data, at + 16 + 8 * i)
                out.append((name, off))
            return out

        # root -> RT_VERSION, then name, then language; a directory entry carries
        # the high bit, the language level is the IMAGE_RESOURCE_DATA_ENTRY.
        entry = next((off for name, off in children(0)
                      if not name & 0x80000000 and name == 16), None)
        if entry is None:
            return None
        while entry & 0x80000000:
            kids = children(entry & 0x7FFFFFFF)
            if not kids:
                return None
            entry = kids[0][1]
        rva, size = struct.unpack_from("<II", img.data, base + entry)
        blob = img.off(rva)
        if blob is None:
            return None
        # The payload is a VS_VERSIONINFO: wLength, wValueLength, wType, then the
        # UTF-16 key "VS_VERSION_INFO", then the fixed info. Which means the
        # signature is not at +0 - the first dword here is 0x340314 (length 0x314,
        # value length 0x340). Look for it inside the block instead.
        window = img.data[blob:blob + min(size, 0x400)]
        at = window.find(struct.pack("<I", 0xFEEF04BD))
        if at < 0:
            return None
        ms, ls = struct.unpack_from("<II", img.data, blob + at + 8)
        return "%d.%d.%d.%d" % (ms >> 16, ms & 0xFFFF, ls >> 16, ls & 0xFFFF)
    except (struct.error, IndexError):
        return None


# ── output ───────────────────────────────────────────────────────────────────

def _fmt(layout: dict) -> str:
    out = ["%s  (%s, sha256 %s)" % (layout["exe"]["file_version"], layout["exe"]["path"],
                                    layout["exe"]["sha256"][:16]), ""]
    for name, cls in layout["classes"].items():
        out.append("%s  sizeof %#x   [registration %#x]"
                   % (name, cls["size"], cls["registration_rva"]))
        for field in cls["fields"]:
            t = field["type"]
            detail = t.get("name") or ("type-info %#x" % t["rva"] if t.get("rva") else t.get("kind"))
            out.append("    +%#04x  %-22s %s" % (field["offset"], field["name"], detail))
        out.append("")
    for key, box in layout["containers"].items():
        out.append("%s: %s of %s (stride %s), count = %s"
                   % (key, box["type_name"], box["element"],
                      hex(box["element_size"] or 0), box["count"]))
    if layout["problems"]:
        out.append("")
        out += ["PROBLEM: " + p for p in layout["problems"]]
    return "\n".join(out)


def _cpp(layout: dict) -> str:
    """The constants a build-gated dumper needs, as C++."""
    def ident(text: str) -> str:
        return re.sub(r"[^A-Za-z0-9]", "_", text)

    lines = [
        "// Generated by tools/renderer_recon/db2_layout.py - do not edit.",
        "// Starfield %s, sha256 %s" % (layout["exe"]["file_version"], layout["exe"]["sha256"]),
        "// Every offset below was read from the class-registration function quoted",
        "// beside it, and is valid only on that build: gate the dump on BuildGate.",
        "namespace SFE::Db2Layout",
        "{",
    ]
    for name, cls in layout["classes"].items():
        lines.append("\t// %s (%#x bytes, registration %#x)" % (name, cls["size"], cls["registration_rva"]))
        short = ident(name.split("::")[-1])
        lines.append("\tconstexpr std::size_t k%sSize = %#x;" % (short, cls["size"]))
        for field in cls["fields"]:
            lines.append("\tconstexpr std::ptrdiff_t k%s%s = %#x;"
                         % (short, ident(field["name"]), field["offset"]))
        lines.append("")
    for key, box in layout["containers"].items():
        short = ident(key.replace("::", "_").replace(".", "_"))
        lines.append("\t// %s: %s, count = %s" % (key, box["type_name"], box["count"]))
        lines.append("\tconstexpr std::size_t k%sStride = %s;" % (short, hex(box["element_size"] or 0)))
    lines.append("}")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--json", default="")
    ap.add_argument("--cpp", action="store_true")
    args = ap.parse_args()
    if not os.path.exists(args.exe):
        print("no Starfield.exe at %s" % args.exe, file=sys.stderr)
        return 2
    layout = recover(args.exe)
    print(_cpp(layout) if args.cpp else _fmt(layout))
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(layout, fh, indent=2)
            fh.write("\n")
        print("\nwrote %s" % args.json)
    return 1 if layout["problems"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
