"""Resolve MSVC RTTI vtables in Starfield.exe (TypeDescriptor -> COL -> vtable).

Notes that cost time to learn:
  * The `. ?AV...@@` name strings live in **.data**, while the COLs and vtables
    live in .rdata. Scanning only .rdata finds zero TypeDescriptors.
  * TypeDescriptor layout is { vftable, spare, name[] }, so the descriptor RVA
    is the name RVA minus 16, and the 32-bit pTypeDescriptor field inside a COL
    is that descriptor RVA (which is why it never matches a 64-bit pointer).
  * A vtable starts at the 8-byte pointer to the COL, followed by method
    pointers that all land inside .text.

Usage
-----
    python rtti_vtable.py --exe <Starfield.exe> --class BGSPreviewTransform
    python rtti_vtable.py --exe ... --class BGSPreviewTransform --disasm 8
    python rtti_vtable.py --exe ... --callers 0x1234abcd
"""
from __future__ import annotations

import argparse
import struct
import re
import sys

import capstone


class Image:
    def __init__(self, path: str):
        self.data = open(path, "rb").read()
        pe = struct.unpack_from("<I", self.data, 0x3C)[0]
        if self.data[pe : pe + 4] != b"PE\0\0":
            sys.exit(f"{path}: not a PE image")
        nsec = struct.unpack_from("<H", self.data, pe + 6)[0]
        optsz = struct.unpack_from("<H", self.data, pe + 20)[0]
        opt = pe + 24
        self.base = struct.unpack_from("<Q", self.data, opt + 24)[0]
        self.sections = {}
        for i in range(nsec):
            o = opt + optsz + i * 40
            name = self.data[o : o + 8].rstrip(b"\0").decode("latin1")
            vsize, va, rawsize, raw = struct.unpack_from("<IIII", self.data, o + 8)
            self.sections[name] = (va, vsize, raw, rawsize)

    def rva(self, file_off: int):
        for nm, (va, _vs, raw, rawsize) in self.sections.items():
            if raw <= file_off < raw + rawsize:
                return va + (file_off - raw)
        return None

    def off(self, rva: int):
        for nm, (va, _vs, raw, rawsize) in self.sections.items():
            if va <= rva < va + rawsize:
                return raw + (rva - va)
        return None

    def in_text(self, rva: int) -> bool:
        va, vsize, _raw, _rawsize = self.sections[".text"]
        return va <= rva < va + vsize

    def read(self, rva: int, size: int) -> bytes:
        o = self.off(rva)
        return self.data[o : o + size] if o is not None else b""


def descriptors(img: Image) -> dict:
    """name -> TypeDescriptor RVA, for every `.?AV<name>@@` in the image."""
    out = {}
    for m in re.finditer(rb"\.\?AV([A-Za-z0-9_@?$]{1,140}?)@@\x00", img.data):
        name = m.group(1).decode("latin1")
        rva = img.rva(m.start()) - 16
        out.setdefault(name, rva)
    return out


def collections(img: Image, td_rva: int) -> list[int]:
    """RVA of every COL whose pTypeDescriptor points at td_rva."""
    _va, _vs, raw, rawsize = img.sections[".rdata"]
    pat = struct.pack("<I", td_rva)
    out = []
    start = raw
    end = raw + rawsize
    j = start
    while True:
        j = img.data.find(pat, j, end)
        if j < 0:
            break
        col = j - 12
        if col >= start and struct.unpack_from("<I", img.data, col)[0] == 1:
            out.append(img.rva(col))
        j += 1
    return out


def vtables(img: Image, col_rva: int) -> list[tuple[int, list[int]]]:
    """For a COL, the vtable(s) that begin with its address, plus method RVAs."""
    _va, _vs, raw, rawsize = img.sections[".rdata"]
    pat = struct.pack("<Q", img.base + col_rva)
    out = []
    j = raw
    while True:
        j = img.data.find(pat, j, raw + rawsize)
        if j < 0:
            break
        methods = []
        for e in range(1, 400):
            if j + (e + 1) * 8 > raw + rawsize:
                break
            ptr = struct.unpack_from("<Q", img.data, j + e * 8)[0]
            target = ptr - img.base
            if not img.in_text(target):
                break
            methods.append(target)
        if methods:
            # A vtable's own RVA is the address of the COL pointer slot.
            out.append((img.rva(j), methods))
        j += 1
    return out


def resolve(img: Image, pattern: str):
    """Every (class name, td, col, vtable, methods) matching a name pattern."""
    rx = re.compile(pattern, re.I)
    rows = []
    for name, td in descriptors(img).items():
        if not rx.search(name):
            continue
        for col in collections(img, td):
            for vt, methods in vtables(img, col):
                rows.append((name, td, col, vt, methods))
    return rows


def disassemble(img: Image, rva: int, count: int = 40, stop_at_ret: bool = True):
    _va, _vsize, _raw, _rawsize = img.sections[".text"]
    code = img.data[img.off(rva) : img.off(rva) + 512]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    out = []
    for n, insn in enumerate(md.disasm(code, img.base + rva)):
        out.append(f"{insn.address - img.base:#x}: {insn.mnemonic} {insn.op_str}")
        if n + 1 >= count or (stop_at_ret and insn.mnemonic.startswith("ret")):
            break
    return out


def function_ranges(img: Image) -> list[tuple[int, int]]:
    """(begin, end) for every .pdata RUNTIME_FUNCTION, sorted by begin."""
    pd = img.sections.get(".pdata")
    if not pd:
        return []
    out = []
    for off in range(pd[2], pd[2] + pd[3], 12):
        begin, end, _u = struct.unpack_from("<III", img.data, off)
        if begin and end > begin:
            out.append((begin, end))
    return sorted(set(out))


def iter_code(img: Image, funcs: list[tuple[int, int]] | None = None):
    """Yield (function start RVA, instruction) over ALL of .text.

    A single `md.disasm` over .text silently stops at the first undecodable
    byte: measured on Starfield.exe that ended after 753,086 of roughly 15M
    instructions, so any conclusion drawn from a linear pass covers only a
    prefix. Decoding per .pdata function and resyncing one byte at a time keeps
    full coverage while still attributing every instruction to its function.
    """
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    text_va, text_vsize, _raw, _rawsize = img.sections[".text"]
    text_off = img.off(text_va)
    text_end = min(text_off + text_vsize, text_off + _rawsize)
    funcs = funcs if funcs is not None else function_ranges(img)
    for begin, end in funcs:
        start = img.off(begin)
        stop = img.off(end)
        if start is None or stop is None:
            continue
        stop = min(stop, text_end)
        pos = start
        while pos < stop:
            progressed = False
            for insn in md.disasm(img.data[pos:stop], img.base + text_va + (pos - text_off)):
                pos = text_off + (insn.address + insn.size - img.base - text_va)
                progressed = True
                yield begin, insn
            if not progressed:
                pos += 1  # padding or data inside the range: resync


def callers(img: Image, target_rva: int) -> list[int]:
    """Function starts whose body contains a direct `call` to target_rva."""
    va, vsize, _raw, _rawsize = img.sections[".text"]
    hits = []
    start = img.off(va)
    code = img.data[start : start + vsize]
    for m in re.finditer(rb"\xe8....", code, re.S):
        disp = struct.unpack_from("<i", code, m.start() + 1)[0]
        site = va + m.start()
        if site + 5 + disp == target_rva:
            hits.append(site)
    return hits


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--exe", required=True)
    ap.add_argument("--class", dest="cls", help="regex on the RTTI class name")
    ap.add_argument("--disasm", type=int, default=0, help="disassemble N methods of each vtable")
    ap.add_argument("--callers", help="RVA (0x...) whose call sites should be listed")
    args = ap.parse_args()

    img = Image(args.exe)
    if args.cls:
        rows = resolve(img, args.cls)
        print(f"{len(rows)} TypeDescriptor/COL/vtable matches for /{args.cls}/")
        for name, td, col, vt, methods in rows:
            print(f"\n{name}\n  td={td:#x} col={col:#x} vtable={vt:#x} methods={len(methods)}")
            for i, m in enumerate(methods[:12]):
                print(f"    [{i}] {m:#x}")
            if args.disasm:
                for i, m in enumerate(methods[: args.disasm]):
                    print(f"    --- method [{i}] {m:#x} ---")
                    for ln in disassemble(img, m, count=24):
                        print(f"      {ln}")
    if args.callers:
        target = int(args.callers, 0)
        sites = callers(img, target)
        print(f"\n{len(sites)} call sites -> {target:#x}")
        for s in sites[:40]:
            print(f"  {s:#x}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
