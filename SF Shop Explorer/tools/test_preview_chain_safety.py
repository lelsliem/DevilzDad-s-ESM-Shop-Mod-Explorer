"""Guard for the engine preview-chain walker (NativeModel menu probing).

The walker reads objects the UI owns: a menu, the UICellRenderer inside it, the
previewed TESObjectREFR, its 3D node, and each object's vtable/COL pair for the
RTTI name. Those objects belong to the main thread, which frees them while this
thread (SFSE task thread, ShopBrowser::FrameUpdate) may still be walking them.

`CanRead` is a VirtualQuery gate with a whole-VAD region cache, so it is a fast
pre-filter and *not* a safety net: after the engine decommits a subrange inside
a cached region it still answers "ok".

Measured 2026-10-18 12:56:35, dump Starfield_10-18-11-56.dmp, faulting frame
`SFE::NativeModel::FindTypedMember+0x9d`:

    ReadEnginePreviewChain -> ProbeMenuChain -> ProbeMenuChain::lambda_1
        -> FindTypedMember -> mov rdx,[rsi]   (rsi = released renderer slot)

The user opened the inventory, DataMenu was still in the UI map with a renderer
pointer, and the slot's target had already been released. CanRead vouched for
it, the load faulted, CTD.

So the contract these tests pin is: every load of a menu/engine pointer is
fault-gated (`SafeReadPtr` / `SafeReadU32` / `SafeCopyAscii`), a fault drops the
region cache that lied, and a menu is only walked while the game reports it
open. These are source-structure assertions; reproducing the teardown race needs
a running game.
"""
from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "NativeModel.cpp"

RAW_PTR_LOAD = "*reinterpret_cast<const std::uintptr_t*>("


def body_of(source: str, signature: str) -> str:
    """Source text of the function *definition* that starts at `signature`.

    NativeModel.cpp forward-declares several of these, so the first textual hit
    is often `...;` - those are skipped and the definition's braces matched.
    """
    start = -1
    while True:
        start = source.index(signature, start + 1)
        brace = source.find("{", start)
        semi = source.find(";", start)
        if semi != -1 and semi < brace:
            continue
        break
    depth = 0
    for i in range(brace, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[brace : i + 1]
    raise AssertionError(f"unbalanced braces after {signature!r}")


class PreviewChainSafetyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.src = SOURCE.read_text(encoding="utf-8-sig")

    def test_fault_gated_readers_exist(self) -> None:
        for helper in ("SafeReadPtr", "SafeReadU32", "SafeCopyAscii"):
            self.assertRegex(self.src, rf"bool {helper}\(")

    def test_every_fault_gated_reader_drops_the_region_cache(self) -> None:
        # The cache is what made CanRead answer "readable" for a released page.
        # A fault is proof the entry lied, so it must not survive the handler.
        for helper in ("SafeReadPtr", "SafeReadU32", "SafeCopyAscii"):
            body = body_of(self.src, f"bool {helper}(")
            self.assertIn("__try", body, helper)
            self.assertIn("__except (EXCEPTION_EXECUTE_HANDLER)", body, helper)
            self.assertIn("InvalidateVqCache();", body, helper)
        invalidate = body_of(self.src, "void InvalidateVqCache()")
        self.assertIn("e.start = 0;", invalidate)
        self.assertIn("e.end = 0;", invalidate)
        self.assertIn("e.ok = false;", invalidate)

    def test_typed_member_scan_loads_through_the_gate(self) -> None:
        body = body_of(self.src, "std::uintptr_t FindTypedMember(")
        self.assertIn("SafeReadPtr(a_base + off, cand)", body)
        self.assertIn("SafeReadPtr(cand, vt)", body)
        self.assertNotIn(RAW_PTR_LOAD, body)
        # A fault skips the slot; it must not abort the whole scan.
        self.assertIn("continue;", body)

    def test_rtti_reads_are_fault_gated(self) -> None:
        resolve = body_of(self.src, "std::string ResolveRTTIName(")
        self.assertIn("SafeReadPtr(a_vtable - 8, col)", resolve)
        self.assertIn("SafeReadU32(col + 0x0C, typeDescRVA)", resolve)
        self.assertIn("SafeCopyAscii(typeDesc + 0x10, buf, 255)", resolve)
        self.assertNotIn(RAW_PTR_LOAD, resolve)

        rtti_of = body_of(self.src, "std::string RttiOfObject(")
        self.assertIn("SafeReadPtr(a_obj, vt)", rtti_of)
        self.assertNotIn(RAW_PTR_LOAD, rtti_of)

    def test_menu_is_only_walked_while_the_game_reports_it_open(self) -> None:
        body = body_of(self.src, "EnginePreviewChain ProbeMenuChain(")
        gate = body.index("IsMenuOpen(key)")
        scan = body.index("FindTypedMember(")
        self.assertLess(gate, scan, "the open check must precede every scan")
        self.assertIn("if (!raw) {", body)
        self.assertLess(body.index("if (!raw) {"), gate)

    def test_measured_offset_picks_load_through_the_gate(self) -> None:
        body = body_of(self.src, "EnginePreviewChain ProbeMenuChain(")
        self.assertIn("SafeReadPtr(menuAddr + a_knownOffset, cand)", body)
        self.assertIn("SafeReadPtr(renderer + a_off, cand)", body)
        self.assertNotIn(RAW_PTR_LOAD, body)

    def test_barter_readback_loads_through_the_gate(self) -> None:
        collect = body_of(self.src, "void CollectModelRoots(")
        self.assertIn("SafeReadPtr(a_base + off, ptr)", collect)
        self.assertIn("SafeReadPtr(ptr, vt)", collect)
        self.assertNotIn(RAW_PTR_LOAD, collect)

        refr = body_of(self.src, "void CollectRefrModel(")
        self.assertIn("SafeReadPtr(a_refr + guarded, loaded)", refr)
        self.assertIn("SafeReadPtr(loaded + 0x08, node)", refr)
        self.assertIn("SafeReadPtr(node, nodeVt)", refr)
        self.assertNotIn(RAW_PTR_LOAD, refr)

    def test_node_walk_rtti_read_is_fault_gated(self) -> None:
        walk = body_of(self.src, "void WalkTree(")
        self.assertIn("SafeReadPtr(b, vt)", walk)


if __name__ == "__main__":
    unittest.main()
