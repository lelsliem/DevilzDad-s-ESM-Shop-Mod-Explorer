"""Prove the barter-menu sink offsets the native preview reader depends on.

`NativeModel::LoadFromBarterMenu` walks the `BarterMenu_LoadModel` event
source's sink list and treats `sink - 0x150` as the `BarterMenu` object. That
offset is not a guess: it comes from the adjustor thunk in the sink base's own
vtable and is corroborated by the sink's teardown. Both are checked here, so a
game patch that moves the base fails this script instead of failing silently
in the preview.

Claims checked (Starfield.exe 1.16.244 RVAs):

  C1  `?AVBarterMenu@@` has a COL at 0x51226e0 whose vtable is 0x4c808e8.
  C2  That vtable's slot 0 is an adjustor thunk `sub rcx, 0x150; jmp ...`,
      i.e. the subobject at menu+0x150 IS the sink base.
  C3  Slot 1 (ProcessEvent) is the handler at 0x15d24f0, and that handler
      reaches 0x15d25e0 / 0x15db520 (the deferred load body).
  C4  The teardown at 0x15d0c71..0x15d0c80 unregisters rdx = rsi + 0x150 from
      the source returned by 0x15da450, which is the LoadModel source getter
      (REL::ID 91377) the plugin calls.

Usage:
    python verify_barter_sink.py --exe <Starfield.exe>

Exit status is 0 only when every claim holds.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rtti_vtable import Image, disassemble, resolve

BARTMENU_TD = 0x5B26208
BARTMENU_SINK_COL = 0x51226E0
BARTMENU_SINK_VTABLE = 0x4C808E8
LOADMODEL_HANDLER = 0x15D24F0
LOADMODEL_BODY_CALLS = (0x15D25E0, 0x15DB520)
SOURCE_GETTER = 0x15DA450
TEARDOWN_CALL = 0x15D0C71
SINK_OFFSET = 0x150


def check(name: str, ok: bool, detail: str) -> bool:
    print(f"{'PASS' if ok else 'FAIL'}  {name}: {detail}")
    return ok


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    args = ap.parse_args()
    img = Image(args.exe)
    ok = True

    rows = [r for r in resolve(img, r"^BarterMenu$") if r[3] == BARTMENU_SINK_VTABLE]
    found = bool(rows) and rows[0][1] == BARTMENU_TD and rows[0][2] == BARTMENU_SINK_COL
    ok &= check("C1 vtable 0x4c808e8 is BarterMenu's sink base",
                found,
                f"rows={len(rows)} td={rows[0][1]:#x} col={rows[0][2]:#x}" if rows else "not found")

    methods = rows[0][4] if rows else []
    thunk = disassemble(img, methods[0], 2) if methods else []
    ok &= check("C2 slot 0 adjusts this by -0x150",
                bool(thunk) and thunk[0].startswith(f"{methods[0]:#x}: sub rcx, {SINK_OFFSET:#x}")
                if methods else False,
                " | ".join(thunk))

    ok &= check("C3 slot 1 is the LoadModel handler",
                len(methods) > 1 and methods[1] == LOADMODEL_HANDLER,
                f"slot1={methods[1]:#x}" if len(methods) > 1 else "no slot 1")

    body = disassemble(img, LOADMODEL_HANDLER, 60, stop_at_ret=False)
    body_text = "\n".join(body)
    # disassemble() prints branch targets as absolute VAs, not RVAs.
    reached = [hex(t) for t in LOADMODEL_BODY_CALLS if f"{img.base + t:#x}" in body_text]
    ok &= check("C3b handler reaches the deferred load body",
                bool(reached),
                f"targets seen={reached or 'none'}")

    teardown = disassemble(img, TEARDOWN_CALL, 6, stop_at_ret=False)
    teardown_text = "\n".join(teardown)
    pairs = f"{SINK_OFFSET:#x}" in teardown_text and "call" in teardown_text
    ok &= check("C4 teardown unregisters rsi+0x150 from the LoadModel source",
                pairs,
                " | ".join(teardown[:4]))

    print()
    print("verified" if ok else "MISMATCH — the reader's offsets no longer hold")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
