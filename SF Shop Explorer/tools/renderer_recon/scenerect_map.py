"""Map the engine's `UpdateSceneRectEvent` plumbing in Starfield.exe.

The native menu item preview (the 400x266 viewport measured earlier) is placed
by a per-frame screen rectangle the UI layer publishes as
`UpdateSceneRectEvent`: the AS3 side declares `sSceneRectName`, `fTopLeftX/Y`,
`fBottomRightX/Y`, `fMaskTopLeftX/Y`, `fMaskBottomRightX/Y`, `bHasMask`, and the
C++ side is a `BSTGlobalEvent` source plus whatever class implements
`BSTEventSink<UpdateSceneRectEvent>`.

This tool answers, from the image only:

  1. Which RTTI describes the event, its sink base, its global source and the
     UI-side dispatcher (the descriptor set is the plugin's runtime handle).
  2. Which class implements `BSTEventSink<UpdateSceneRectEvent>` - derived by
     walking each COL's class-hierarchy base array, not by name guessing.
  3. What that consumer's `ProcessEvent` does with the event, disassembled.
  4. Whether a static instance of the global source is present in `.data` at
     rest (the plugin finds it by RTTI scan at runtime either way).

Usage:
    python scenerect_map.py --exe <Starfield.exe>
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rtti_vtable import Image, collections, descriptors, disassemble, vtables  # noqa: E402

EVENT = "UpdateSceneRectEvent"


def col_bases(img: Image, col_rva: int) -> list[int]:
    """TypeDescriptor RVAs in a COL's class-hierarchy base array."""
    raw = img.off(col_rva)
    if raw is None:
        return []
    cd_off = struct.unpack_from("<i", img.data, raw + 8)[0]
    cd = img.off(col_rva + cd_off)
    if cd is None:
        return []
    num = struct.unpack_from("<I", img.data, cd + 4)[0]
    arr_rva = col_rva + cd_off + struct.unpack_from("<i", img.data, cd + 8)[0]
    if num > 64 or img.off(arr_rva) is None:
        return []
    out = []
    for i in range(num):
        bcd_rva = arr_rva + i * 4
        bcd = img.off(bcd_rva)
        if bcd is None:
            break
        out.append(bcd_rva + struct.unpack_from("<i", img.data, bcd)[0])
    return out


def data_slots(img: Image, vtable_rva: int) -> list[int]:
    """RVAs in .data holding a pointer to this vtable (static instances)."""
    sec = img.sections.get(".data")
    if not sec:
        return []
    _va, _vs, raw, rawsize = sec
    pat = struct.pack("<Q", img.base + vtable_rva)
    out = []
    j = raw
    while True:
        j = img.data.find(pat, j, raw + rawsize)
        if j < 0:
            break
        out.append(img.rva(j))
        j += 1
    return out


def describe(img: Image, td: int, label: str) -> None:
    cols = collections(img, td)
    if not cols:
        print(f"{label}: td={td:#x} no COL")
        return
    for col in cols:
        for vt, methods in vtables(img, col):
            slots = data_slots(img, vt)
            print(f"{label}: td={td:#x} COL={col:#x} vtable={vt:#x} slots={len(methods)} "
                  f"dataStatics={[hex(s) for s in slots] or 'none'}")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--disasm", type=int, default=70)
    args = ap.parse_args()
    img = Image(args.exe)
    d = descriptors(img)
    by_name = {n: td for n, td in d.items()}

    print(f"== RTTI for {EVENT} ==")
    wanted = [n for n in by_name if EVENT in n]
    for n in sorted(wanted, key=lambda x: by_name[x]):
        print(f"  td={by_name[n]:#x} {n}")

    print()
    print(f"== event type ==")
    if EVENT in by_name:
        describe(img, by_name[EVENT], "UpdateSceneRectEvent")

    print()
    print("== global source (BSTSingletonImplicit<BSTGlobalEvent::EventSource<..>>) ==")
    for n in wanted:
        if "BSTSingleton" in n or n == "?$BSTEventSource@" + EVENT + "@@":
            describe(img, by_name[n], n)

    print()
    print("== sink implementers (COL hierarchy contains BSTEventSink<..>) ==")
    sink_td = by_name.get(f"?$BSTEventSink@{EVENT}@@")
    impls = []
    if sink_td:
        for n, td in d.items():
            for col in collections(img, td):
                if sink_td in col_bases(img, col):
                    impls.append((n, td, col))
                    break
    print(f"  {len(impls)} class(es): {[n for n, _, _ in impls]}")
    for n, td, col in impls:
        for vt, methods in vtables(img, col):
            print(f"  {n} vtable={vt:#x} slots={len(methods)}")
            for i, m in enumerate(methods[:3]):
                if i == 0:
                    continue
                print(f"    slot {i} rva={m:#x}")
                for line in disassemble(img, m, args.disasm, stop_at_ret=True):
                    print(f"      {line}")

    print()
    print("== UI-side dispatcher ==")
    for n in wanted:
        if "TUIEventDispatcher" in n or "AutoRegisterEvent" in n:
            describe(img, by_name[n], n)


if __name__ == "__main__":
    main()
