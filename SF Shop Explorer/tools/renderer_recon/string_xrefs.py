"""Find every code/data reference to a string (or any blob) in Starfield.exe.

The menu item-preview path is event-driven, and each event is identified by a
name string the UI layer passes around ("BarterMenu_LoadModel",
"InventoryMenu_LoadModel", "UpdateSceneRectEvent", ...). The registration code
that binds a C++ handler to one of those names is therefore reachable by
finding the references to its string.

The scan decodes each `.pdata` function range (a single linear pass stops at
the first undecodable byte - see the notes on that) and reports, for every
rip-relative memory operand whose target equals one of the requested RVAs:

  * the referencing function's RVA (so it can be fed to func_probe.py),
  * the instruction, and
  * the nearest `call`/`jmp` target after it, which is usually the handler
    being registered.

Usage:
    python string_xrefs.py --exe <Starfield.exe> "BarterMenu_LoadModel" "UpdateSceneRectEvent"
    python string_xrefs.py --exe <Starfield.exe> --rva 0x4c7fed0
"""
from __future__ import annotations

import argparse
import re
import struct
import sys
from pathlib import Path

import capstone

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rtti_vtable import Image, descriptors, function_ranges  # noqa: E402


def find_string(img: Image, text: str) -> list[int]:
    """RVAs of an ASCII string in the image (single-byte, printable)."""
    out = []
    data = img.data
    start = 0
    while True:
        start = data.find(text.encode("ascii"), start)
        if start < 0:
            return out
        rva = img.rva(start)
        if rva is not None:
            out.append(rva)
        start += 1


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("strings", nargs="*", help="ASCII strings to look for")
    ap.add_argument("--rva", type=lambda s: int(s, 0), action="append", default=[],
                    help="target RVA to find references to")
    ap.add_argument("--limit", type=int, default=40, help="references printed per target")
    args = ap.parse_args()

    img = Image(args.exe)
    targets: dict[int, str] = {}
    for s in args.strings:
        for rva in find_string(img, s):
            targets[rva] = s
    for rva in args.rva:
        targets.setdefault(rva, f"rva {rva:#x}")
    if not targets:
        sys.exit("no targets: pass a string or --rva")

    print("targets:")
    for rva, label in sorted(targets.items()):
        print(f"  {rva:#x} {label!r}")

    funcs = function_ranges(img)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True

    hits: dict[int, list[tuple[int, int, str]]] = {t: [] for t in targets}
    for begin, end in funcs:
        off, stop = img.off(begin), img.off(end)
        if off is None or stop is None or stop <= off:
            continue
        for insn in md.disasm(img.data[off:stop], img.base + begin):
            for op in insn.operands:
                if op.type == capstone.x86.X86_OP_IMM and insn.mnemonic in ("call", "jmp"):
                    target = op.imm - img.base
                    if target in hits:
                        hits[target].append((begin, insn.address - img.base, "direct"))
                    continue
                if op.type != capstone.x86.X86_OP_MEM or not op.mem.base:
                    continue
                if insn.reg_name(op.mem.base) != "rip":
                    continue
                target = insn.address + insn.size + op.mem.disp - img.base
                if target in hits:
                    hits[target].append((begin, insn.address - img.base,
                                         f"{insn.mnemonic} {insn.op_str}"))

    for rva, label in sorted(targets.items()):
        rows = hits[rva]
        print(f"\n== {rva:#x} {label!r}: {len(rows)} reference(s) ==")
        for fn, addr, text in rows[: args.limit]:
            print(f"  fn {fn:#x}  @{addr:#x}  {text}")
            # The next direct call/jmp within the same function is the
            # candidate handler being wired up.
            off, stop = img.off(fn), img.off(function_ranges(img) and 0 or 0)
            following = []
            o = img.off(addr)
            if o is not None:
                for insn in md.disasm(img.data[o: o + 96], img.base + addr):
                    if insn.mnemonic in ("call", "jmp") and insn.operands and \
                            insn.operands[0].type == capstone.x86.X86_OP_IMM:
                        following.append(f"{insn.mnemonic} {insn.operands[0].imm - img.base:#x}")
                        break
            if following:
                print(f"        -> {following[0]}")


if __name__ == "__main__":
    main()
