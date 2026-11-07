"""The build gate: what runs when the game build is not the verified one.

Two features reach the engine through addresses that are in no Address
Library: the world freeze writes the UI pause counter at UI+0x4B4, and the
mod-application route calls two raw .text addresses (0x42C7B0, 0x5DDBD0) and
reads a third (0x61DD4FF). All were read out of Starfield 1.16.244. On any
other build they name different memory, and a SEH frame does not make calling
an unrelated address safe.

These tests pin the gate itself and, more importantly, that every user of those
addresses actually asks it. A gate that one path forgets is worse than none,
because the log then says the plugin checked.

They also check the three places the plugin version is written down, because
those drifted apart before: xmake.lua said 0.8.116 while the runtime said
0.8.117 and the deployed DLL matched neither. A release is the one time that
must not be true.
"""
from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
GATE_H = SRC / "BuildGate.h"
GATE_CPP = SRC / "BuildGate.cpp"
PAUSE = SRC / "ShopPause.cpp"
EXPLORER = SRC / "ShopExplorer.cpp"
MAIN = SRC / "main.cpp"
XMAKE = ROOT / "xmake.lua"
HEADER = SRC / "ShopExplorer.h"


class BuildGateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        for path in (GATE_H, GATE_CPP, PAUSE, EXPLORER, MAIN, XMAKE, HEADER):
            if not path.is_file():
                raise unittest.SkipTest(f"missing {path}")
        cls.gate_h = GATE_H.read_text(encoding="utf-8-sig")
        cls.gate = GATE_CPP.read_text(encoding="utf-8-sig")
        cls.pause = PAUSE.read_text(encoding="utf-8-sig")
        cls.explorer = EXPLORER.read_text(encoding="utf-8-sig")
        cls.main = MAIN.read_text(encoding="utf-8-sig")
        cls.xmake = XMAKE.read_text(encoding="utf-8-sig")
        cls.header = HEADER.read_text(encoding="utf-8-sig")

    # -- the gate itself ---------------------------------------------------

    def test_gate_is_fail_closed(self) -> None:
        """No capture means nothing is verified; an unload path must not open it."""
        body = self.gate[self.gate.index("bool Verified()"):]
        body = body[:body.index("std::string RuntimeString()")]
        self.assertIn("g_captured && g_verified", body)
        # Both start false: a build we never asked about is not the verified one.
        self.assertRegex(self.gate, r"bool\s+g_captured\s*\{\s*false\s*\}")
        self.assertRegex(self.gate, r"bool\s+g_verified\s*\{\s*false\s*\}")

    def test_verified_predicate_accepts_only_the_verified_build(self) -> None:
        """Replica of Capture()'s comparison, run against the real constants."""
        wanted = [int(m) for m in re.search(
            r"kVerifiedMajor = (\d+);\s*.*?kVerifiedMinor = (\d+);\s*.*?"
            r"kVerifiedPatch = (\d+);\s*.*?kVerifiedBuild = (\d+);",
            self.gate, re.S).groups()]
        self.assertEqual(wanted, [1, 16, 244, 0])

        def captures(major, minor, patch, build):
            return (major, minor, patch, build) == tuple(wanted)

        self.assertTrue(captures(1, 16, 244, 0))
        for other in ((1, 16, 245, 0), (1, 16, 243, 0), (1, 15, 244, 0),
                      (2, 0, 0, 0), (0, 0, 0, 0)):
            self.assertFalse(captures(*other), other)

    def test_capture_logs_both_versions(self) -> None:
        # The refusal line names the verified version AND the running one, so a
        # user report says which build broke it.
        self.assertIn("verified against {}.{}.{}", self.gate)
        self.assertIn("VerifiedVersionString()", self.gate)
        self.assertIn("RuntimeString()", self.gate)

    def test_refusals_are_logged_once_per_feature(self) -> None:
        body = self.gate[self.gate.index("void Refuse("):]
        self.assertIn("g_refused", body)
        self.assertIn("return;", body)  # already-warned feature: silent
        # The refusal text lives in exactly one place. ShopPause calls Refuse
        # every frame while the freeze is held, so a second warning there (or
        # in ShopExplorer) would turn the log into a per-frame flood.
        refusal = "this plugin's addresses for it were verified"
        self.assertIn(refusal, self.gate)
        self.assertNotIn(refusal, self.pause)
        self.assertNotIn(refusal, self.explorer)

    def test_capture_reads_the_sfse_runtime_version(self) -> None:
        self.assertIn("BuildGate::Capture(", self.main)
        self.assertIn("a_sfse->RuntimeVersion()", self.main)
        capture = self.main.index("BuildGate::Capture(")
        init = self.main.index("SFSE::Init(a_sfse)")
        self.assertLess(init, capture)
        # No interface: stay unverified rather than assuming the best.
        self.assertIn("unlisted internals stay disabled", self.main)

    # -- every user of an unlisted address asks the gate --------------------

    def test_pause_counter_write_is_gated(self) -> None:
        body = self.pause[self.pause.index("void RaiseCounterToFloor("):]
        body = body[:body.index("struct Snapshot")]
        guard = body.index("BuildGate::Verified()")
        write = body.index("CompareExchangeCounter(")
        self.assertLess(guard, write, "the counter write must be behind the gate")
        self.assertIn("BuildGate::Refuse(", body)

    def test_freeze_request_does_not_claim_an_unwritten_freeze(self) -> None:
        body = self.pause[self.pause.index("void SetFrozen("):]
        body = body[:body.index("bool IsFrozen()")]
        guard = body.index("a_frozen && !BuildGate::Verified()")
        store = body.index("g_frozen.store(a_frozen")
        self.assertLess(guard, store, "refuse before recording a freeze")

    def test_pause_diagnostics_do_not_sample_unknown_addresses(self) -> None:
        body = self.pause[self.pause.index("Snapshot Take()"):]
        body = body[:body.index("// Ring buffer")]
        guard = body.index("if (!BuildGate::Verified())")
        first_read = body.index("kMainSingletonRva")
        self.assertLess(guard, first_read)

    def test_spike_probe_and_delivery_are_gated(self) -> None:
        """The mod route calls raw .text addresses; both entries must refuse.

        A window rather than a parsed function: what matters is that the guard
        is the first thing that touches those addresses, and that the refusal
        returns the value its caller already handles.
        """
        probe = self.explorer[self.explorer.index("SpikeRecordDump SpikeRecordProbe("):][:3000]
        guard = probe.index("BuildGate::Verified()")
        raw = probe.index("0x5DDBD0")
        self.assertLess(guard, raw, "refuse before taking the raw .text addresses")
        self.assertIn("refused.code = 2", probe)

        deliver = self.explorer[self.explorer.index("SpikeOutcome SpikeDeliverWithMods("):][:1200]
        guard = deliver.index("BuildGate::Verified()")
        self.assertIn("BuildGate::Refuse(", deliver)
        # kNotAttempted is what makes the caller fall back to a plain add.
        self.assertIn("return SpikeOutcome::kNotAttempted;", deliver[guard:])
        self.assertLess(guard, deliver.index("AttachMod impl"))

    def test_gate_is_compiled_in(self) -> None:
        self.assertIn("src/BuildGate.cpp", self.xmake)

    # -- version bookkeeping -------------------------------------------------

    def test_the_three_version_declarations_agree(self) -> None:
        runtime = re.search(r'kVersion\[\] = "([\d.]+)"', self.header).group(1)
        xmake_version = re.search(r'set_version\("([\d.]+)"\)', self.xmake).group(1)
        patch = re.search(r'COMMONLIB_PLUGIN_VERSION_PATCH", "(\d+)"', self.xmake).group(1)
        minor = re.search(r'COMMONLIB_PLUGIN_VERSION_MINOR", "(\d+)"', self.xmake).group(1)
        major = re.search(r'COMMONLIB_PLUGIN_VERSION_MAJOR", "(\d+)"', self.xmake).group(1)
        self.assertEqual(runtime, xmake_version,
                         "kVersion and xmake.lua set_version disagree")
        self.assertEqual(f"{major}.{minor}.{patch}", runtime,
                         "the plugin's reported version disagrees with kVersion")


if __name__ == "__main__":
    unittest.main()
