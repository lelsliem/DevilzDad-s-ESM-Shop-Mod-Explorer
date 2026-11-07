"""Disassemble one Starfield.exe function with its cross-references resolved.

Written for the native-preview work: the barter menu's model-load path is a
chain of virtual overrides, and reading it as raw mnemonics hides the two
things that matter — which *engine* function is called next, and which field
of the sink object the loaded `NiAVObject*` is stored in.

For every instruction it prints the instruction and, where computable:

  call/jmp  -> the absolute target RVA (direct) or the slot (indirect)
  lea/rip   -> the target RVA, plus the ASCII string or RTTI name it points at
  mov imm   -> the constants, which is how event ids and form types show up
  mov [r+disp], r -> annotated as a candidate field write

Addresses are image RVAs (file-relative virtual addresses), the same units
`rtti_vtable.py` prints, so a target here can be fed back in as --rva.

Usage
-----
    python func_probe.py --exe <Starfield.exe> --rva 0x5e2500
    python func_probe.py --exe <Starfield.exe> --class BarterMenuEventShuttle
"""
from __future__ import annotations

import argparse
import re
import struct
import sys
from pathlib import Path

import capstone

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rtti_vtable import Image, descriptors, function_ranges, resolve  # noqa: E402


def text_at(img: Image, rva: int, limit: int = 96) -> str | None:
    """Printable run at rva, if it looks like a C string or an RTTI name."""
    data = img.read(rva, limit)
    if not data:
        return None
    end = data.find(b"\x00")
    if end < 8:
        return None
    raw = data[:end]
    if not all(32 <= b < 127 for b in raw):
        return None
    return raw.decode("latin1")


def rtti_near(img: Image, rva: int, window: int = 0x40) -> str | None:
    """An RTTI type name whose TypeDescriptor sits within `window` of rva."""
    for name, td in DESCRIPTORS.items():
        if td <= rva < td + window:
            return name
    return None


def function_body(img: Image, start: int) -> bytes:
    for begin, end in FUNCS:
        if begin == start:
            off, stop = img.off(begin), img.off(end)
            return img.data[off:stop] if off is not None and stop is not None else b""
    # Not in .pdata: bounded read, still better than nothing.
    off = img.off(start)
    return img.data[off : off + 1024] if off is not None else b""


def annotate(img: Image, insn) -> list[str]:
    notes: list[str] = []
    md_regs = insn.reg_name
    if insn.mnemonic in ("call", "jmp"):
        op = insn.operands[0] if insn.operands else None
        if op is not None:
            if op.type == capstone.x86.X86_OP_IMM:
                notes.append(f"-> {op.imm - img.base:#x}")
            elif op.type == capstone.x86.X86_OP_MEM:
                m = op.mem
                if m.base and md_regs(m.base) in ("rip", ""):
                    target = insn.address + insn.size + m.disp
                    notes.append(f"-> [{md_regs(m.base)}+{target - img.base:#x}] (indirect)")
                else:
                    notes.append(
                        f"-> [{'/'.join(filter(None, [md_regs(m.base), md_regs(m.index)]))}"
                        f"+{m.disp:#x}*{m.scale or 1}] (indirect)"
                    )
    # RIP-relative operand: a global, string, vtable, or COL.
    for op in insn.operands:
        if op.type == capstone.x86.X86_OP_MEM and op.mem.base and md_regs(op.mem.base) == "rip":
            target = insn.address + insn.size + op.mem.disp - img.base
            s = text_at(img, target)
            rtti = rtti_near(img, target)
            label = f"@{target:#x}"
            if s:
                label += f" = {s!r}"
            elif rtti:
                label += f" = RTTI {rtti}"
            notes.append(label)
        elif op.type == capstone.x86.X86_OP_IMM:
            notes.append(f"#{op.imm:#x}")
    # Field writes: the answer to "where does the model pointer land".
    if insn.mnemonic.startswith("mov") and len(insn.operands) >= 2:
        dst = insn.operands[0]
        if dst.type == capstone.x86.X86_OP_MEM and dst.mem.disp and not (
            dst.mem.base and md_regs(dst.mem.base) == "rip"
        ):
            notes.append(f"[write +{dst.mem.disp:#x}]")
    return notes


def probe(img: Image, start: int, count: int) -> None:
    body = function_body(img, start)
    if not body:
        sys.exit(f"no bytes at RVA {start:#x}")
    end = start + len(body)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    print(f"function {start:#x}..{end:#x} ({len(body)} bytes)")
    print(f"rtti near start: {rtti_near(img, start, 0x200) or '-'}")
    print()
    calls: list[int] = []
    for n, insn in enumerate(md.disasm(body, img.base + start)):
        if n >= count:
            print("... (truncated)")
            break
        rva = insn.address - img.base
        notes = annotate(img, insn)
        detail = f"    ; {'  '.join(notes)}" if notes else ""
        print(f"{rva:#010x}  {insn.mnemonic:<7} {insn.op_str}{detail}")
        for op in insn.operands:
            if (
                insn.mnemonic == "call"
                and op.type == capstone.x86.X86_OP_IMM
            ):
                calls.append(op.imm - img.base)
    if calls:
        print()
        print("direct call targets:")
        for t in dict.fromkeys(calls):
            print(f"  {t:#x}  {rtti_near(img, t, 0x200) or ''}")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--rva", type=lambda s: int(s, 0))
    ap.add_argument("--class", dest="cls")
    ap.add_argument("--vtable", action="store_true", help="print the class vtable")
    ap.add_argument("--count", type=int, default=400)
    args = ap.parse_args()
    img = Image(args.exe)
    global DESCRIPTORS, FUNCS
    DESCRIPTORS = descriptors(img)
    FUNCS = function_ranges(img)
    start = args.rva
    if start is None:
        if not args.cls:
            ap.error("--rva or --class required")
        rows = resolve(img, args.cls)
        if not rows:
            sys.exit(f"no RTTI class matching /{args.cls}/")
        name, _td, col, vt, methods = rows[0]
        print(f"{name}: col={col:#x} vtable={vt:#x} methods={len(methods)}")
        for i, m in enumerate(methods):
            print(f"  [{i}] {m:#x}")
        if not args.vtable:
            return
        start = methods[1] if len(methods) > 1 else methods[0]
        print()
    probe(img, start, args.count)


if __name__ == "__main__":
    main()
