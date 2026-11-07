"""Re-derive the project's engine-level claims with full-coverage analysis.

Every claim here was originally derived from static analysis of Starfield.exe.
Where that analysis disassembled code, it inherited the linear-`md.disasm`
truncation (753,086 of ~14.1M instructions); where it was a byte/string search,
it is sound but is re-derived anyway because the answers are now cheap.

Claims under test
-----------------
C1 (notes 6r-11e, README, ImguiOverlay.cpp): "the game's loop does not call
   TranslateMessage" on the shop's path.
C2 (notes ~5325): "no 4-byte value in the image equals [the SceneManager@BSMenu3D]
   descriptor's RVA, so no _RTTICompleteObjectLocator references it" -> no vtable,
   so the plugin's RTTI-vtable scan cannot find the manager.
C3 (notes ~5337): "BSMenu3D::Scene has no standalone TypeDescriptor
   (`?AVScene@BSMenu3D@@` occurs zero times as a descriptor)".
C4 (HANDOFF.md dead ends): "TESBoundObject::Clone3D vtable slot 0x78 = thunk
   `48 8b 01 45 33 c9 48 ff a0 30 03 00 00` -> slot 0x66 with r9=0".
C5 (notes ~2543): "Barter getter bytes at 0x15DA450 read: guarded getter (init
   check + lea rax,[rip+disp]) - not a plain lea+ret".

Usage
-----
    python rederive_engine_claims.py --exe <Starfield.exe>
"""
from __future__ import annotations

import argparse
import re
import struct
import sys
from pathlib import Path

import capstone

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rtti_vtable import Image, descriptors, disassemble, function_ranges, iter_code, resolve, vtables  # noqa: E402
from context_index_attribution import build_col_map, build_vtable_owners  # noqa: E402

VERDICTS: list[tuple[str, str, str]] = []


def verdict(claim: str, status: str, detail: str) -> None:
    VERDICTS.append((claim, status, detail))
    print(f"\n=== {claim}: {status}\n    {detail}")


def imported_function_iat(img: Image, name: str) -> list[int]:
    """RVA of every IAT slot for a named import, without walking the import dir.

    An import entry is IMAGE_IMPORT_BY_NAME = { WORD hint; CHAR name[]; } and the
    IAT holds its RVA, so the TVA of the slot is the 4-byte occurrence of
    (name_rva - 2). Requiring two NUL bytes before the name (an earlier attempt)
    misses entries whose hint is non-zero.
    """
    slots: list[int] = []
    for m in re.finditer(re.escape(name.encode()) + rb"\x00", img.data):
        entry = img.rva(m.start())
        if entry is None or not 0x1000 <= entry < 0x2000000:
            continue
        want = struct.pack("<I", entry - 2)
        for sec in (".rdata", ".data"):
            if sec not in img.sections:
                continue
            _va, _vs, raw, rawsize = img.sections[sec]
            j = raw
            while True:
                j = img.data.find(want, j, raw + rawsize)
                if j < 0:
                    break
                slot = img.rva(j)
                if slot is not None and slot not in slots:
                    slots.append(slot)
                j += 1
    return slots


def import_directory(img: Image) -> tuple[int, int] | None:
    """(rva, size) of the import table, from the PE data directory."""
    pe = struct.unpack_from("<I", img.data, 0x3C)[0]
    opt = pe + 24
    if struct.unpack_from("<H", img.data, pe + 24)[0] != 0x20B:
        return None  # not PE32+
    rva, size = struct.unpack_from("<II", img.data, opt + 112 + 8)
    return (rva, size) if rva else None


def iat_slots_from_directory(img: Image, func: str) -> list[int]:
    """IAT slot RVAs for a named import, via INT/FirstThunk descriptor walk."""
    d = import_directory(img)
    if d is None:
        return []
    slots = []
    rva, _size = d
    for i in range(0, 64):
        desc = img.read(rva + i * 20, 20)
        if len(desc) < 20:
            break
        int_rva, _ts, _chain, name_rva, iat = struct.unpack("<IIIII", desc)
        if not name_rva and not iat:
            break
        if not int_rva:
            continue  # bound-only descriptor: names are not in the file
        for index in range(0, 4096):
            entry = img.read(int_rva + index * 8, 8)
            if len(entry) < 8:
                break
            value = struct.unpack("<Q", entry)[0]
            if value == 0:
                break
            if value & 0x8000000000000000:
                continue  # imported by ordinal
            raw = img.read((value & 0xFFFFFFFF) + 2, len(func) + 1)
            if raw == func.encode() + b"\x00":
                slots.append(iat + index * 8)
                break
    return slots


def check_translate_message(img: Image) -> None:
    claim = "C1 game never calls TranslateMessage (shop path)"
    iats = iat_slots_from_directory(img, "TranslateMessage") or imported_function_iat(img, "TranslateMessage")
    if not iats:
        probe = {n: img.data.count(n.encode()) for n in
                 ("TranslateMessage", "DispatchMessage", "PeekMessage", "GetMessage",
                  "user32", "GetProcAddress", "MessageBox")}
        verdict(claim, "BLOCKED",
                "no IAT slot found for TranslateMessage. Name-probe counts in the image: "
                + repr(probe) + "; sections: " + repr(sorted(img.sections)))
        return
    wanted = set(iats)
    sites = []
    for fn, insn in iter_code(img):
        for op in insn.operands:
            if op.type != capstone.x86.X86_OP_MEM or op.mem.base != capstone.x86.X86_REG_RIP:
                continue
            if insn.address + insn.size + op.mem.disp - img.base in wanted:
                sites.append((fn, insn.address - img.base, insn.mnemonic, insn.op_str))
                break
    calls = [s for s in sites if s[2] in ("call", "jmp")]
    detail = (f"IAT slot(s) {[hex(i) for i in iats]}; {len(sites)} referencing instruction(s), "
              f"{len(calls)} of them call/jmp\n      "
              + "\n      ".join(f"fn {f:#x} site {s:#x}: {m} {t}" for f, s, m, t in sites[:12]))
    verdict(claim, "PASS (no call site)" if not calls else "CHANGED (call sites exist)", detail)


def verify_col(img: Image, col_rva: int) -> list[str]:
    """Structurally validate a COL through its class-hierarchy descriptor.

    `{1, offset, cdOffset, pTypeDescriptor, pClassDescriptor}` alone can match by
    accident, so walk pClassDescriptor -> BaseClassArray -> BaseClassDescriptor
    and require the first base class to be the same TypeDescriptor.
    """
    out = []
    raw = img.read(col_rva, 20)
    if len(raw) < 20:
        return ["COL unreadable"]
    sig, offset, cd_offset, ptd, pcd = struct.unpack("<IIIII", raw)
    out.append(f"sig={sig} offset={offset:#x} cdOffset={cd_offset:#x} pTD={ptd:#x} pCD={pcd:#x}")
    chd = img.read(pcd, 16)
    if len(chd) < 16:
        out.append("class-hierarchy descriptor unreadable")
        return out
    csig, attrs, num_bases, array_rva = struct.unpack("<IIII", chd)
    out.append(f"CHD sig={csig} attrs={attrs:#x} numBaseClasses={num_bases} array={array_rva:#x}")
    first = img.read(array_rva, 4)
    if len(first) == 4:
        bcd_rva = struct.unpack("<I", first)[0]
        bcd = img.read(bcd_rva, 24)
        if len(bcd) == 24:
            btd, ncb, _pmd1, _pmd2, _pmd3, battrs = struct.unpack("<IIIIII", bcd)
            out.append(f"BaseClass[0] pTD={btd:#x} numContainedBases={ncb} attrs={battrs:#x}"
                       + ("  MATCHES the COL's pTD" if btd == ptd else "  <-- differs"))
    return out


def check_scene_manager(img: Image, td_by_name: dict, cols: dict, owners: dict) -> None:
    claim = "C2 SceneManager@BSMenu3D has no COL/vtable"
    td = td_by_name.get("SceneManager@BSMenu3D")
    if td is None:
        verdict(claim, "BLOCKED", "no SceneManager@BSMenu3D TypeDescriptor at all")
        return
    col_rvas = [c for c, t in cols.items() if t == td]
    if not col_rvas:
        verdict(claim, "PASS", f"descriptor {td:#x} has no COL")
        return
    lines = [f"descriptor SceneManager@BSMenu3D td={td:#x} has {len(col_rvas)} COL(s)"]
    for col_rva in col_rvas:
        lines.append(f"  COL {col_rva:#x}:")
        lines += [f"    {ln}" for ln in verify_col(img, col_rva)]
        for vt_rva, methods in vtables(img, col_rva):
            lines.append(f"    vtable {vt_rva:#x} -> {len(methods)} method(s): "
                         + ", ".join(hex(m) for m in methods[:8]))
            for m in methods[:2]:
                lines.append("      " + "\n      ".join(disassemble(img, m, count=6)))
    verdict(claim, "CHANGED (COL exists -> a vtable is reachable)", "\n    ".join(lines))


def check_scene_descriptor(img: Image, td_by_name: dict) -> None:
    claim = "C3 BSMenu3D::Scene has no standalone TypeDescriptor"
    hits = {n: td for n, td in td_by_name.items() if re.search(r"Scene@BSMenu3D|BSMenu3D", n)}
    if hits:
        detail = "descriptors carrying BSMenu3D: " + ", ".join(
            f"{n}={td:#x}" for n, td in sorted(hits.items()))
        has_exact = any(n.startswith("Scene@BSMenu3D") for n in hits)
        verdict(claim, "PASS (no Scene descriptor)" if not has_exact else "CHANGED", detail)
    else:
        # Fall back to a raw string search: the descriptor table may not cover it.
        raw = img.data.count(b".?AVScene@BSMenu3D@@")
        verdict(claim, "PASS (no Scene descriptor)" if raw == 0 else "CHANGED",
                f"no BSMenu3D descriptor found; raw '.?AVScene@BSMenu3D@@' byte count = {raw}")


def check_clone3d_thunk(img: Image) -> None:
    claim = "C4 Clone3D slot 0x78 is a thunk to slot 0x66 with r9=0"
    expected = bytes.fromhex("488b0145 33c948ff a0300300 00".replace(" ", ""))
    rows = resolve(img, "^TESBoundObject$")
    if not rows:
        verdict(claim, "BLOCKED", "TESBoundObject vtable not resolved from RTTI")
        return
    # Several COLs can exist (multiple inheritance); the primary vtable is the
    # one with the most slots, and slot numbering only makes sense against it.
    print("\n    [C4] TESBoundObject vtables:",
          [(hex(v), len(m)) for _n, _t, _c, v, m in rows])
    name, td, col, vt, methods = max(rows, key=lambda r: len(r[4]))
    if len(methods) <= 0x78:
        verdict(claim, "BLOCKED", f"largest {name} vtable has only {len(methods)} slots")
        return
    slot78 = methods[0x78]
    raw = img.read(slot78, len(expected))
    ok_bytes = raw == expected
    # 48 8b 01 | 45 33 c9 | 48 ff a0 <disp32>  -> the disp32 sits at offset 9.
    target_slot = struct.unpack_from("<i", expected, 9)[0] // 8 if ok_bytes else None
    target_fn = methods[target_slot] if target_slot is not None and target_slot < len(methods) else None
    detail = [f"{name}: td={td:#x} col={col:#x} vtable={vt:#x} slots={len(methods)}",
              f"slot 0x78 = {slot78:#x} bytes {raw.hex()}"
              + ("  MATCHES the recorded thunk" if ok_bytes else "  <-- differs"),
              f"thunk target = [rax+0x330] -> slot {target_slot and hex(target_slot)}"
              + (f" = {target_fn:#x}" if target_fn else " (out of range)")]
    if target_fn:
        detail.append("target body:\n      " + "\n      ".join(disassemble(img, target_fn, count=10)))
    verdict(claim, "PASS" if ok_bytes else "CHANGED", "\n    ".join(detail))


def check_barter_getter(img: Image, rva: int) -> None:
    claim = f"C5 {rva:#x} is a guarded getter, not a plain lea+ret"
    lines = disassemble(img, rva, count=20)
    shape = [ln.split(": ", 1)[1] for ln in lines[:8]]
    guarded = any(ln.startswith(("cmp", "test", "jg", "je", "jne")) for ln in shape)
    verdict(claim, "PASS" if guarded else "CHANGED",
            "first instructions:\n      " + "\n      ".join(lines[:10]))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--exe", required=True)
    ap.add_argument("--barter-getter", default="0x15DA450", help="RVA from notes ~2543")
    ap.add_argument("--skip-heavy", action="store_true", help="omit the C1 instruction sweep")
    args = ap.parse_args()

    img = Image(args.exe)
    td_by_name = descriptors(img)
    print(f"image: {len(img.data)/1e6:.1f} MB, {len(td_by_name)} TypeDescriptors, "
          f"{len(function_ranges(img))} .pdata functions")

    if not args.skip_heavy:
        check_translate_message(img)
    cols = build_col_map(img, set(td_by_name.values()))
    owners = build_vtable_owners(img, cols)
    print(f"\n    [context] {len(cols)} COL records, {len(owners)} vtabled functions")
    check_scene_manager(img, td_by_name, cols, owners)
    check_scene_descriptor(img, td_by_name)
    check_clone3d_thunk(img)
    check_barter_getter(img, int(args.barter_getter, 0))

    print("\n\n==== SUMMARY ====")
    for claim, status, _ in VERDICTS:
        print(f"  {status:32} {claim}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
