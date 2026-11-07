"""Guards for how a modded (rarity/OMOD) purchase reaches the inventory.

The route cannot be unit-tested against a live game from here, so what is
pinned is the shape of the part that kept failing:

* 0.8.334-0.8.337 delivered the modified world ref with a RemoveItem
  request, which looks for an inventory entry on the source. A dropped stack
  has none, so nothing moved and the item stayed on the floor.
* 0.8.338 delivers it by activating the ref with the player as the activator
  - the engine's own take path, and the only one that carries the instance
  (with its attached OMODs) into the container.

The invariants below are the ones a refactor would silently drop: the
activation runs BEFORE the legacy move, the ref is not touched after a clean
activation (the handler may free it), and the inventory count - not a void
call's return - decides the outcome.
"""

from __future__ import annotations

import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
EXPLORER_CPP = ROOT / "src" / "ShopExplorer.cpp"


class ActivationDeliveryTests(unittest.TestCase):
    def setUp(self) -> None:
        self.source = EXPLORER_CPP.read_text(encoding="utf-8-sig")
        self.deliver = self.source[
            self.source.index("SpikeOutcome SpikeDeliverWithMods("):
        ]
        self.deliver = self.deliver[: self.deliver.index("\tShopBrowser* ShopBrowser::GetSingleton()")]

    def test_activation_stage_exists_and_is_seh_guarded(self) -> None:
        self.assertIn("static std::uint32_t SpikeActivateStage(", self.source)
        stage = self.source[
            self.source.index("static std::uint32_t SpikeActivateStage("):
        ]
        stage = stage[: stage.index("static std::uint32_t SpikeInventoryObjectCount(")]
        self.assertIn("RE::OBJ_ACTIVATE_DATA data{};", stage)
        self.assertIn("data.activatedRef = a_ref;", stage)
        self.assertIn("data.actionRef = a_actor;", stage)
        self.assertIn("a_ref->Activate(data)", stage)
        self.assertIn("__try", stage)
        self.assertIn("__except (EXCEPTION_EXECUTE_HANDLER)", stage)

    def test_take_request_supplies_the_entry_identity(self) -> None:
        # 0.8.340: build 85's move carried object + count only, so RemoveItem
        # matched no entry. A loose ref does own an inventory list; the entry
        # is identified by the ref's own instance data (+0x98) and extra list
        # (+0xC8), which is what carries the attached OMODs.
        self.assertIn("static SpikeTakeReport SpikeTakeToInventory(", self.source)
        take = self.source[self.source.index("static SpikeTakeReport SpikeTakeToInventory("):]
        take = take[: take.index("static std::uint32_t SpikeInventoryObjectCount(")]
        self.assertIn("std::memcpy(&inst, rb + 0x98, sizeof(inst));", take)
        self.assertIn("std::memcpy(&extra, rb + 0xC8, sizeof(extra));", take)
        self.assertIn("req[1] = inst;", take)
        self.assertIn("req[2] = extra;", take)
        self.assertIn("const std::int32_t indexCount[2]{ -1, static_cast<std::int32_t>(a_count) };", take)
        self.assertIn("req[5] = reinterpret_cast<std::uintptr_t>(a_player);", take)
        self.assertIn("a_ref->RemoveItem(handle,", take)
        self.assertIn("__try", take)
        self.assertIn("__except (EXCEPTION_EXECUTE_HANDLER)", take)
        # Order: the instance add, then the take request, then the activate
        # handler - each fallback gated on the count and on the ref still
        # naming the dropped form.
        add = self.deliver.index("SpikeAddWithExtra(a_player, a_bound, a_count,")
        take = self.deliver.index("SpikeTakeToInventory(dropped, a_bound, a_count, a_player)")
        activate = self.deliver.index("SpikeActivateStage(dropped, a_player)")
        self.assertLess(add, take)
        self.assertLess(take, activate)
        self.assertEqual(self.deliver.count("if (!deliveryConfirmed && SpikeRefIsBound(dropped, a_bound)) {"), 2)

    def test_instance_add_reuses_the_refs_extra_list(self) -> None:
        # 0.8.342: every engine take entry failed on 1.16.244. The add call is
        # the one that works, and it accepts an ExtraDataList - the ref's own,
        # which is where the attach wrote extra 0x33. Build 88's hang came from
        # sharing that list as a RAW pointer; ExtraDataList is intrusive
        # refcounted, so a BSTSmartPointer keeps it alive.
        self.assertIn("static std::uint32_t SpikeAddWithExtra(", self.source)
        add = self.source[self.source.index("static std::uint32_t SpikeAddWithExtra("):]
        add = add[: add.index("struct SpikeInvStack")]
        self.assertIn("RE::BSTSmartPointer<RE::ExtraDataList>(a_extra)", add)
        self.assertIn("AddObjectToContainer(a_bound", add)
        # The ref's list is read from +0xC8.
        self.assertIn("static std::uint64_t SpikeRefExtraList(", self.source)
        self.assertIn("+ 0xC8, sizeof(dl)", self.source)

        # The ref is never deleted when its instance data is the same object the
        # inventory kept, and that decision lives in the one helper every path
        # routes through.
        dispose = self.source[self.source.index("static void SpikeDisposeDeliveryRef("):]
        dispose = dispose[: dispose.index("SpikeOutcome SpikeDeliverWithMods(")]
        self.assertIn("stack.inst == refInst", dispose)
        self.assertIn("SpikeRetireRef(a_ref)", dispose)

        # BOTH ways a delivery can be confirmed must dispose of the drop ref.
        # Only the instance-add branch did before 0.9.0, so a purchase confirmed
        # by the take request or by activation left its ref standing in the
        # world - the copy that reappeared on the floor after a save load
        # (2026-10-26). Retiring the ref directly here would bypass the
        # shared-instance guard, so the delivery body may only call the helper.
        self.assertEqual(self.deliver.count("SpikeDisposeDeliveryRef(a_player, dropped, a_bound, baseFid,"), 2)
        self.assertIn('"delivered"', self.deliver)
        self.assertIn('"delivered-late"', self.deliver)
        self.assertNotIn("SpikeRetireRef(dropped)", self.deliver)

    def test_pickup_virtual_is_gone(self) -> None:
        # Actor::PickUpObject faulted 0xC0000005 on this build's vtable in
        # 0.8.341; it must not be called again.
        self.assertNotIn("PickUpObject(", self.source)
        self.assertNotIn("SpikePickUpStage", self.source)

    def test_activation_runs_before_the_legacy_move(self) -> None:
        activate = self.deliver.index("SpikeActivateStage(dropped, a_player)")
        move = self.deliver.index("SpikeRemoveStage(dropped, &transfer")
        self.assertLess(activate, move,
                        "the take path must not wait on the request that never moves")

    def test_legacy_move_only_runs_when_activation_faulted(self) -> None:
        move = self.deliver.index("SpikeRemoveStage(dropped, &transfer")
        guard = self.deliver.rindex("else {", 0, move)
        self.assertGreater(guard, self.deliver.index("if (activateCode == 0) {"),
                           "a clean activation must not be followed by a ref touch")

    def test_inventory_count_is_the_ground_truth(self) -> None:
        self.assertIn("const bool inventoryNowConfirmed = inventoryAfter >= expectedInventory;",
                      self.deliver)
        self.assertIn("const bool confirmed = after >= expected;", self.deliver)
        # The count is read before and after the delivery so the basket can
        # report a confirmed outcome rather than a void call's word.
        self.assertIn("const std::uint64_t expectedInventory = "
                      "static_cast<std::uint64_t>(inventoryBefore) + a_count;", self.deliver)

    def test_cellwalk_resolves_before_the_handle(self) -> None:
        # 0.8.338's handle return was accepted on a position-float match and
        # the whole purchase ran against a pointer into a string table (attach
        # AV, activation no-op, item left on the floor as Common). The walk is
        # structural, so it goes first and the handle is only a fallback.
        walk = self.deliver.index("SpikeWalkCell(cell, &dropPos, preCount, a_bound")
        handle = self.deliver.index("SpikeResolveHandle(handle)")
        self.assertLess(walk, handle)

    def test_a_candidate_must_name_the_dropped_form(self) -> None:
        # OBJ_REFR.objectReference is at ref+0x98 (TESObjectREFR::data at 0x80
        # + OBJ_REFR::objectReference at 0x18). Reading +0x18 instead is a
        # TESForm field and rejects every real ref.
        self.assertIn("static bool SpikeRefIsBound(", self.source)
        bound = self.source[
            self.source.index("static bool SpikeRefIsBound("):
        ]
        bound = bound[: bound.index("// Cell-walk fallback")]
        self.assertIn("+ 0x98, sizeof(base)", bound)
        self.assertNotIn("+ 0x18, sizeof(base)", bound)
        self.assertIn("return base == reinterpret_cast<std::uintptr_t>(a_bound);", bound)
        # The walk's gate carries the same test, and no site accepts a
        # position match on its own any more.
        walk = self.source[self.source.index("static RE::TESObjectREFR* SpikeWalkCell("):]
        walk = walk[: walk.index("static std::uint32_t SpikeRemoveStage(")]
        self.assertIn("if (base != reinterpret_cast<std::uintptr_t>(a_bound)) {", walk)
        self.assertNotIn("if (sig != 0) {", self.deliver)

    def test_spike_probes_are_off_by_default(self) -> None:
        # The per-pick record dumps wrote eleven lines each; they are behind
        # [shop] debugSpike now, so a normal purchase log stays readable.
        self.assertIn('GetPrivateProfileIntW(L"shop", L"debugSpike", 0', self.source)
        record = self.source[self.source.index("static void SpikeLogRecord("):]
        record = record[: record.index("REX::INFO", record.index("if (!SpikeDebug())"))]
        self.assertIn("if (!SpikeDebug()) {", record)
        self.assertIn("return;", record)

    def test_pending_is_not_reported_as_a_floor_drop(self) -> None:
        pending = self.deliver[self.deliver.index("if (activateCode == 0) {"):]
        self.assertIn("SpikeOutcome::kTransferPendingMods", pending)
        self.assertLess(pending.index("SpikeOutcome::kTransferPendingMods"),
                        pending.index("SpikeOutcome::kOnFloorMods"),
                        "an accepted activation is pending, not a floor result")


if __name__ == "__main__":
    unittest.main()
