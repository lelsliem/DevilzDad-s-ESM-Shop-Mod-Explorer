"""Find engine code that consumes TESBoundObject::previewTransform by displacement.

Record layout (verified live by the plugin's own reader, and by RTTI here):

    TESBoundObject          previewTransform component at +0x88
      BGSPreviewTransform   BGSTransform* transforms[8] at component +0x08
                            -> array at object +0x90, slot k at +0x90 + 8k
      BGSTransform          TransformData at form +0x30 (0x24 bytes)
                            pos +0x30 | rot +0x3C | scale +0x48
                            minZoom +0x4C | maxZoom +0x50

A slot read is `[reg + 0x90 + 8k]`, but that alone is not a signature: requiring
"the next access also touches +0x48" matches ordinary pointer-then-field code
(Scaleform, actor refs) and produced ~1000 false positives. A real consumer
dereferences the whole TransformData block, so this scanner requires >=3 distinct
loads inside 0x30..0x54 of the slot value, or a >=16-byte vector load at +0x30
plus a second read.

Coverage matters more than the predicate here: decode .text through
`rtti_vtable.iter_code`, which walks every .pdata function and resyncs on
padding. A plain `md.disasm` over .text stops at the first undecodable byte.

The expected answer for a shipped Starfield build is ZERO hits: the engine does
not read the array by displacement at all, it calls TESForm's
AsPreviewTransform1(ctx) virtual (see preview_transform_callers.py). This tool's
value is that negative result plus the diagnostics that keep it honest.

Usage
-----
    python preview_transform_consumer_scan.py --exe <Starfield.exe> --verbose
"""
from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass, field
from pathlib import Path

import capstone

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rtti_vtable import Image, function_ranges, iter_code  # noqa: E402

SLOT_BASE = 0x90
SLOT_COUNT = 8
DATA_DISPS = set(range(0x30, 0x54, 4))  # BGSTransform::TransformData
WINDOW = 16  # instructions a loaded slot pointer stays live
STACK_REGS = {"rsp", "rbp", "rip", "eip", "esp", "ebp"}


@dataclass
class Hit:
    function: int
    slot: int
    slot_disp: int
    disps: set = field(default_factory=set)
    vector_load: bool = False
    lines: list = field(default_factory=list)

    @property
    def score(self) -> int:
        return len(self.disps) + (2 if self.vector_load else 0)


def mem_access(insn):
    """(base, index, scale, disp, size, is_read) for [base + idx*scale + disp].

    is_read is False when the memory operand is the destination, which separates
    `mov dword ptr [rax+0x3c], 0x3f800000` (writing a transform) from
    `vmovups xmm0, [rax+0x3c]` (reading the authored record).
    """
    for i, op in enumerate(insn.operands):
        if op.type != capstone.x86.X86_OP_MEM:
            continue
        m = op.mem
        if m.base == 0:
            return None
        base = insn.reg_name(m.base)
        if base in STACK_REGS:
            # Stack/frame displacements collide with 0x90..0xC8 and generated
            # every false positive in the first (soundness-free) attempt.
            return None
        index = insn.reg_name(m.index) if m.index else None
        if index in STACK_REGS:
            return None
        return (base, index, m.scale, m.disp, op.size, i > 0)
    return None


def dst_register(insn):
    if not insn.operands or insn.operands[0].type != capstone.x86.X86_OP_REG:
        return None
    return insn.reg_name(insn.operands[0].reg)


def rename_target(insn):
    if insn.mnemonic != "mov" or len(insn.operands) != 2:
        return None
    a, b = insn.operands
    if a.type == capstone.x86.X86_OP_REG and b.type == capstone.x86.X86_OP_REG:
        return (insn.reg_name(a.reg), insn.reg_name(b.reg))
    return None


def scan(img: Image, verbose: bool = False):
    funcs = function_ranges(img)
    stats = dict(slot_loads=0, indexed_slot_loads=0, comp_bases=0, data_reads=0, instructions=0)
    hits: dict[tuple[int, int], Hit] = {}
    pending: list[tuple[int, str, int]] = []
    comp: list[tuple[int, str]] = []
    consts: list[tuple[int, str, int]] = []
    samples: list[dict] = []
    open_samples: list[list] = []
    current_func = 0

    def const_value(reg: str, n: int):
        vals = [v for e, r, v in consts if e >= n and r == reg]
        return vals[-1] if vals else None

    for n, (fn, insn) in enumerate(iter_code(img, funcs)):
        stats["instructions"] += 1
        if fn != current_func:
            pending.clear()
            comp.clear()
            consts.clear()
            current_func = fn
        pending[:] = [p for p in pending if p[0] >= n]
        comp[:] = [c for c in comp if c[0] >= n]
        consts[:] = [c for c in consts if c[0] >= n]

        if insn.mnemonic == "mov" and len(insn.operands) == 2:
            a, b = insn.operands
            if a.type == capstone.x86.X86_OP_REG and b.type == capstone.x86.X86_OP_IMM:
                consts.append((n + WINDOW * 4, insn.reg_name(a.reg), b.imm))

        mo = mem_access(insn)
        if mo:
            base, index, _scale, disp, size, is_read = mo
            if is_read and pending:
                for _exp, objreg, slot in pending:
                    if base != objreg or disp not in DATA_DISPS:
                        continue
                    stats["data_reads"] += 1
                    hit = hits.setdefault((fn, slot), Hit(fn, slot, disp))
                    hit.disps.add(disp)
                    if size >= 16:
                        hit.vector_load = True
                    if len(hit.lines) < 6:
                        hit.lines.append(f"{insn.address - img.base:#x}: {insn.mnemonic} {insn.op_str}")
            rn = rename_target(insn)
            if rn:
                dst, src = rn
                if pending:
                    pending[:] = [(e, dst if r2 == src else r2, s) for e, r2, s in pending]
                if comp:
                    comp[:] = [(e, dst if r2 == src else r2) for e, r2 in comp]

            # A slot pointer obtained from the component base (`mov rax,[rcx+8+8k]`).
            dst = dst_register(insn)
            if dst and size == 8 and any(base == c[1] for c in comp):
                slot = None
                if index is None:
                    k = disp - 8
                    if k >= 0 and k % 8 == 0 and k // 8 < SLOT_COUNT:
                        slot = k // 8
                else:
                    slot = const_value(index, n)
                if slot is not None and 0 <= slot < SLOT_COUNT:
                    pending.append((n + WINDOW, dst, slot))

            lo, hi = SLOT_BASE, SLOT_BASE + 8 * SLOT_COUNT
            if size == 8 and lo <= disp < hi:
                if index is None:
                    stats["slot_loads"] += 1
                else:
                    stats["indexed_slot_loads"] += 1
                slot = (disp - lo) // 8 if index is None else const_value(index, n)
                slot = slot if slot is not None and 0 <= slot < SLOT_COUNT else 0
                if len(samples) < 4:
                    s = {"rva": insn.address - img.base,
                         "load": f"{insn.mnemonic} {insn.op_str}", "after": []}
                    samples.append(s)
                    open_samples.append([s, 8])
                pending.append((n + WINDOW, dst_register(insn) or base, slot))
            elif disp == SLOT_BASE - 8 and insn.mnemonic in ("lea", "add"):
                stats["comp_bases"] += 1
                comp.append((n + WINDOW, base if insn.mnemonic == "lea" else (dst_register(insn) or base)))

        for o in open_samples:
            if o[1] > 0:
                o[0]["after"].append(f"{insn.address - img.base:#x}: {insn.mnemonic} {insn.op_str}")
                o[1] -= 1

    if verbose:
        print(f"[scan] {stats}")
        for s in samples:
            print(f"[sample] {s['rva']:#x}: {s['load']}")
            for ln in s["after"]:
                print(f"           {ln}")
    return hits


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--exe", required=True)
    ap.add_argument("--json")
    ap.add_argument("--min-disps", type=int, default=3, help="distinct TransformData loads required")
    ap.add_argument("--top", type=int, default=20)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    img = Image(args.exe)
    hits = scan(img, args.verbose)
    strong = [h for h in hits.values()
              if len(h.disps) >= args.min_disps or (h.vector_load and len(h.disps) >= 2)]
    strong.sort(key=lambda h: (-h.score, h.function))
    print(f"slot-load sites that also read TransformData: {len(hits)}; "
          f"meeting the block-read rule: {len(strong)}")
    for h in strong[: args.top]:
        print(f"\nfn {h.function:#010x} slot {h.slot} disps {sorted(hex(d) for d in h.disps)}"
              f"{' vec' if h.vector_load else ''}")
        for ln in h.lines:
            print(f"    {ln}")
    if args.json:
        payload = [{**h.__dict__, "disps": sorted(h.disps)} for h in strong]
        Path(args.json).write_text(json.dumps(payload, indent=1), encoding="utf-8")
        print(f"\nwrote {args.json} ({len(payload)} hits)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
