"""Attribute BGSPreviewTransform context indices to the engine code that picks them.

The chain this reconstructs:

    caller (a menu)                mov edx, <context 0..7>
                                   mov rcx, <TESForm*>
                                   call qword ptr [rax + 0x2D0]   ; AsPreviewTransform1
    engine virtual                 returns BGSPreviewTransform*
    consumer                       mov rax, [ret + 8 + 8*ctx]     ; transforms[ctx]
                                   ... reads TransformData at +0x30

So the answer to "which menu uses which index" is a property of the *call sites*:
each one loads a constant into edx right before the virtual call. This tool
resolves that constant (following register copies inside the same function),
walks the direct-call graph upward to find the owning RTTI class of the caller,
and prints one row per site.

Infrastructure, each built in a single pass so it stays fast on a 61 MB .text:
  * COL table      : scan .rdata for {1, off, cdOff, td, cd} records whose two
                     descriptors are real TypeDescriptors.
  * vtable owners  : scan .rdata for words pointing at a COL, then walk the
                     method pointers that follow -> function RVA -> class name.
  * call graph     : scan .text for `e8 rel32` and index target -> caller sites.

Usage
-----
    python context_index_attribution.py --exe <Starfield.exe>
    python context_index_attribution.py --exe ... --depth 3 --json out.json
"""
from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from collections import defaultdict, deque
from pathlib import Path

import capstone

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rtti_vtable import Image, descriptors, function_ranges  # noqa: E402

SLOT_DISPS = {0x2D0: "AsPreviewTransform1", 0x2D8: "AsPreviewTransform2"}
MENU_RX = re.compile(r"Menu", re.I)


def build_col_map(img: Image, td_set: set) -> dict:
    """{col_rva: td_rva} for every COL record.

    A COL is {signature=1, offset, cdOffset, pTypeDescriptor, pClassDescriptor}.
    Only pTypeDescriptor points at a `.?AV...@@` TypeDescriptor: pClassDescriptor
    points at a class-hierarchy descriptor, so requiring *both* to be
    descriptors rejects every COL in the image.
    """
    cols = {}
    start, size = img.sections[".rdata"][2], img.sections[".rdata"][3]
    for off in range(start, start + size - 20, 4):
        if struct.unpack_from("<I", img.data, off)[0] != 1:
            continue
        td = struct.unpack_from("<I", img.data, off + 12)[0]
        if td in td_set:
            cols[img.rva(off)] = td
    return cols


def build_vtable_owners(img: Image, cols: dict) -> dict:
    """{method_rva: {td_rva, ...}} for every method slot in a vtable."""
    owners = defaultdict(set)
    start = img.sections[".rdata"][2]
    end = start + img.sections[".rdata"][3]
    for off in range(start, end - 16, 8):
        td = cols.get(struct.unpack_from("<Q", img.data, off)[0] - img.base)
        if td is None:
            continue
        e = off + 8
        while e + 8 <= end:
            target = struct.unpack_from("<Q", img.data, e)[0] - img.base
            if not img.in_text(target):
                break
            owners[target].add(td)
            e += 8
    return owners


def build_call_graph(img: Image, funcs: list) -> dict:
    """{callee_rva: [call site RVAs]} from direct `call rel32` only."""
    text_va, text_vs, text_raw, _sz = img.sections[".text"]
    code = img.data[text_raw : text_raw + text_vs]
    graph = defaultdict(list)
    begins = [b for b, _e in funcs]
    for m in re.finditer(rb"\xe8(....)", code, re.S):
        disp = struct.unpack("<i", m.group(1))[0]
        site = text_va + m.start()
        graph[site + 5 + disp].append(site)
    return graph, begins


def function_of(begins: list, rva: int) -> int:
    import bisect
    i = bisect.bisect_right(begins, rva) - 1
    return begins[i] if i >= 0 else 0


def decode_functions(img: Image, funcs: list):
    """Yield (function begin, [instructions]) with padding resync."""
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    text_va, text_vs, text_raw, _sz = img.sections[".text"]
    text_off = img.off(text_va)
    text_end = min(text_off + text_vs, text_off + img.sections[".text"][3])
    for begin, end in funcs:
        start, stop = img.off(begin), img.off(end)
        if start is None or stop is None:
            continue
        stop = min(stop, text_end)
        out = []
        pos = start
        while pos < stop:
            progressed = False
            for insn in md.disasm(img.data[pos:stop], img.base + text_va + (pos - text_off)):
                pos = text_off + (insn.address + insn.size - img.base - text_va)
                progressed = True
                out.append(insn)
            if not progressed:
                pos += 1
        yield begin, out


def resolve_reg(insns, i, reg, depth=5):
    """Constant in `reg` at instruction index i, following `mov r32, r32`."""
    if depth == 0:
        return None
    for j in range(i - 1, max(-1, i - 120), -1):
        insn = insns[j]
        if len(insn.operands) != 2 or insn.operands[0].type != capstone.x86.X86_OP_REG:
            continue
        dst = insn.reg_name(insn.operands[0].reg)
        if dst != reg:
            continue
        src = insn.operands[1]
        if insn.mnemonic == "mov" and src.type == capstone.x86.X86_OP_IMM:
            return src.imm
        if insn.mnemonic == "mov" and src.type == capstone.x86.X86_OP_REG:
            return resolve_reg(insns, j, insn.reg_name(src.reg), depth - 1)
        return None
    return None


def slot_call(insn):
    """disp when this instruction calls a preview-transform vtable slot."""
    for i, op in enumerate(insn.operands):
        if op.type != capstone.x86.X86_OP_MEM:
            continue
        m = op.mem
        if i == 0 and insn.mnemonic in ("call", "jmp") and m.disp in SLOT_DISPS and not m.index:
            return m.disp
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--exe", required=True)
    ap.add_argument("--depth", type=int, default=3, help="call-graph levels searched upward")
    ap.add_argument("--json")
    args = ap.parse_args()

    img = Image(args.exe)
    name_by_td = {td: n for n, td in descriptors(img).items()}
    print(f"[1/4] {len(name_by_td)} TypeDescriptors")
    cols = build_col_map(img, set(name_by_td))
    print(f"[2/4] {len(cols)} COL records")
    owners = build_vtable_owners(img, cols)
    print(f"[3/4] {len(owners)} vtabled functions")
    funcs = function_ranges(img)
    graph, begins = build_call_graph(img, funcs)
    print(f"[4/4] {len(graph)} distinct callees in the direct-call graph")

    rows = []
    for fn, insns in decode_functions(img, funcs):
        for i, insn in enumerate(insns):
            disp = slot_call(insn)
            if disp is None:
                continue
            ctx = resolve_reg(insns, i, "edx")
            if ctx is None or not (0 <= ctx < 8):
                ctx = None
            # Walk up the call graph for a menu-ish owner.
            chain = []
            frontier = deque([(fn, 0)])
            seen = {(fn, 0)}
            menu_owner = None
            while frontier:
                f, d = frontier.popleft()
                btn = [name_by_td[t] for t in owners.get(f, ()) if t in name_by_td]
                if btn:
                    chain.append((d, f, btn))
                    if menu_owner is None and any(MENU_RX.search(b) for b in btn):
                        menu_owner = next(b for b in btn if MENU_RX.search(b))
                if d >= args.depth:
                    continue
                for site in graph.get(f, ())[:40]:
                    nf = function_of(begins, site)
                    if (nf, d + 1) not in seen:
                        seen.add((nf, d + 1))
                        frontier.append((nf, d + 1))
            rows.append({
                "slot": SLOT_DISPS[disp],
                "site": insn.address - img.base,
                "function": fn,
                "context": ctx,
                "site_owner": [name_by_td[t] for t in owners.get(fn, ()) if t in name_by_td],
                "menu_owner": menu_owner,
                "chain": [{"depth": d, "fn": f, "classes": c} for d, f, c in chain],
                "context_insns": [f"{x.address - img.base:#x}: {x.mnemonic} {x.op_str}"
                                  for x in insns[max(0, i - 4) : i + 2]],
            })

    known = [r for r in rows if r["context"] is not None]
    print(f"\n{len(rows)} virtual call sites, {len(known)} with a resolved context index 0..7\n")
    print(f"{'ctx':>4}  {'owner class or caller chain':46}  site")
    for r in sorted(known, key=lambda r: (r["context"] if r["context"] is not None else 99, r["site"])):
        owner = r["menu_owner"] or (r["site_owner"][0] if r["site_owner"] else None)
        if not owner and r["chain"]:
            owner = "+".join(r["chain"][0]["classes"][:1]) if r["chain"][0]["classes"] else "?"
        print(f"{r['context']:>4}  {(owner or '?')[:46]:46}  {r['site']:#010x}  ({r['slot']})")
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(rows, fh, indent=1)
        print(f"\nwrote {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
