"""Find engine call sites of TESForm::AsPreviewTransform (vtable slots 0x5A/0x5B)
and the context index each caller passes.

Why virtual calls
-----------------
Nothing in 61 MB of .text reads `[obj + 0x90 + 8k]`, and BGSPreviewTransform has
no accessor of its own (its vtable is 11 entries: dtor, GetFormComponentType,
InitializeDataComponent, ... all NoOp/ctor/registrar). The component is reached
through TESForm's virtuals instead:

    TESForm.h
      virtual BGSPreviewTransform*       AsPreviewTransform1();  // 5A
      virtual const BGSPreviewTransform* AsPreviewTransform2() const;  // 5B

so a consumer loads the vtable slot and calls it, then indexes the returned
component (`transforms[k]` at +8+8k) and reads TransformData at +0x30. That is
why a displacement search for the array is empty: the array base is a *return
value*, and the slot index is a separate argument the caller chooses.

What this reports
-----------------
site RVA, which slot was called, the nearest constant index (0..7) loaded into a
32-bit register just before the call, the containing function, and the RTTI
class that owns that function when it is a vtable method. Class ownership is
what turns "some function reads slot 2" into "menu X uses context 2".

Usage
-----
    python preview_transform_callers.py --exe <Starfield.exe>
    python preview_transform_callers.py --exe ... --json out.json
"""
from __future__ import annotations

import argparse
import json
import struct
import sys

import capstone

sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parent))
from rtti_vtable import Image, descriptors, iter_code, function_ranges  # noqa: E402

SLOT_DISPS = {0x2D0: "AsPreviewTransform1", 0x2D8: "AsPreviewTransform2"}
LOOKBACK = 30  # instructions searched backwards for the index constant


def slot_access(insn):
    """(disp, dest reg) when this instruction loads/calls a vtable slot."""
    for i, op in enumerate(insn.operands):
        if op.type != capstone.x86.X86_OP_MEM:
            continue
        m = op.mem
        if m.index or m.base == 0 or m.disp not in SLOT_DISPS:
            continue
        if i == 0 and insn.mnemonic == "mov" and insn.operands[0].type == capstone.x86.X86_OP_REG:
            return (m.disp, insn.reg_name(insn.operands[0].reg), "load")
        if i == 0 and insn.mnemonic in ("call", "jmp"):
            return (m.disp, None, insn.mnemonic)
    return None


def index_from(window):
    """Constant index (0..7) loaded into a 32-bit register in a window."""
    out = []
    for insn in window:
        if insn.mnemonic != "mov" or len(insn.operands) != 2:
            continue
        a, b = insn.operands
        if a.type != capstone.x86.X86_OP_REG or b.type != capstone.x86.X86_OP_IMM:
            continue
        if 0 <= b.imm <= 7:
            out.append((insn.reg_name(a.reg), b.imm, f"{insn.address:#x}: {insn.mnemonic} {insn.op_str}"))
    return out[-3:]


def owner_class(img: Image, fn_rva: int, td_names: dict) -> str | None:
    """RTTI class whose vtable contains fn_rva (walk back one slot at a time)."""
    _va, _vs, raw, rawsize = img.sections[".rdata"]
    pat = struct.pack("<Q", img.base + fn_rva)
    j = raw
    while True:
        j = img.data.find(pat, j, raw + rawsize)
        if j < 0:
            return None
        # Walk back over method pointers to the vtable head.
        head = j
        while head - 8 >= raw:
            prev = struct.unpack_from("<Q", img.data, head - 8)[0] - img.base
            if not img.in_text(prev):
                break
            head -= 8
        col_rva = struct.unpack_from("<Q", img.data, head - 8)[0] - img.base if head - 8 >= raw else 0
        if col_rva and img.off(col_rva) and struct.unpack_from("<I", img.data, img.off(col_rva))[0] == 1:
            td = struct.unpack_from("<I", img.data, img.off(col_rva) + 12)[0]
            if td in td_names:
                return td_names[td]
        j += 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--exe", required=True)
    ap.add_argument("--json")
    ap.add_argument("--max-sites", type=int, default=200)
    args = ap.parse_args()

    img = Image(args.exe)
    td_names = {td: name for name, td in descriptors(img).items()}
    funcs = function_ranges(img)
    print(f"[scan] {len(funcs)} .pdata functions, {len(td_names)} TypeDescriptors")

    # Per function: the instructions, so an index constant is never picked up
    # from a neighbouring function.
    sites = []
    window: list = []
    seen = 0
    prev_fn = None
    # `mov rax, [rcx+0x2d0]` seen but its `call rax` not read yet:
    # (expiry address, register, disp, site rva, index candidates, context)
    pending: list = []

    def emit(site_rva, disp, fn, idx, context):
        sites.append({
            "site": site_rva,
            "slot": SLOT_DISPS[disp],
            "function": fn,
            "owner": owner_class(img, fn, td_names),
            "index_candidates": [
                {"value": v, "reg": r, "insn": t} for r, v, t in idx
            ],
            "context": context,
        })

    for fn, insn in iter_code(img, funcs):
        seen += 1
        if fn != prev_fn:
            window = []
            pending = []
            prev_fn = fn
        window = (window + [insn])[-LOOKBACK:]

        if insn.mnemonic == "call" and insn.operands and insn.operands[0].type == capstone.x86.X86_OP_REG:
            called = insn.reg_name(insn.operands[0].reg)
            for exp, preg, pdisp, prva, pidx, pctx in list(pending):
                if preg == called and insn.address <= exp:
                    emit(prva, pdisp, fn, pidx, pctx)
                    pending.remove((exp, preg, pdisp, prva, pidx, pctx))

        acc = slot_access(insn)
        if not acc:
            continue
        disp, reg, kind = acc
        if kind == "load":
            pending.append((insn.address + 64, reg, disp, insn.address - img.base,
                            index_from(window), [f"{x.address - img.base:#x}: {x.mnemonic} {x.op_str}"
                                                  for x in window[-4:]]))
            continue
        emit(insn.address - img.base, disp, fn, index_from(window),
             [f"{x.address - img.base:#x}: {x.mnemonic} {x.op_str}" for x in window[-4:]])
        if len(sites) >= args.max_sites:
            break
    print(f"[scan] decoded {seen} instructions")

    print(f"[scan] {len(sites)} AsPreviewTransform call sites")
    for s in sites:
        idx = ", ".join(f"{c['value']}" for c in s["index_candidates"]) or "-"
        print(f"\n{s['slot']:20} site={s['site']:#010x} fn={s['function']:#010x} "
              f"owner={s['owner'] or '?'}  index~{idx}")
        for ln in s["context"]:
            print(f"      {ln}")
        for c in s["index_candidates"]:
            print(f"      idx {c['value']} via {c['reg']}: {c['insn']}")
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(sites, fh, indent=1)
        print(f"\nwrote {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
