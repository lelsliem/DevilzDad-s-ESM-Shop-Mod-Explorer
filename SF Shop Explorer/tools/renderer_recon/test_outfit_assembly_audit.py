#!/usr/bin/env python3
"""Guards for the repeatable outfit-assembly audit.

The command is only useful as a check if a missing part or an unbuildable NIF
actually makes it exit 1. These tests use a synthetic store so they do not need
the game install, and they also pin the declared-part guard that lets a record
take its parts from more than one complete group.
"""
from __future__ import annotations

import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE.parent / "ba2_audit"))

import aid_colour_audit as audit  # noqa: E402
import outfit_assembly_audit as outfit  # noqa: E402

GROUP = ("clothes/test", "suit_{}.nif")
PATHS = {token: f"clothes/test/suit_{token}.nif" for token in outfit.PARTS}


class FakeProbe:
    """Presence/buildability answers for one synthetic asset set."""

    def __init__(self, present=(), missing=(), bad=None) -> None:
        self.present = {outfit.norm_path(path) for path in present}
        self.missing = {outfit.norm_path(path) for path in missing}
        self.bad = {outfit.norm_path(path): status for path, status in (bad or {}).items()}

    def exists(self, path: str) -> bool:
        key = outfit.norm_path(path)
        return key in self.present and key not in self.missing

    def usable(self, path: str) -> tuple[bool, str]:
        key = outfit.norm_path(path)
        if key in self.bad:
            return False, self.bad[key]
        if key in self.missing:
            return False, "missing"
        return True, ""


def record(*paths: str, form: int = 1) -> dict:
    return {
        "type": "ARMO",
        "plugin": "Test.esm",
        "form": form,
        "name": "Test Suit",
        "nif_paths": list(paths),
    }


class AuditRecordTests(unittest.TestCase):
    def test_a_missing_sibling_is_a_missing_part(self) -> None:
        complete = {GROUP: set(outfit.PARTS)}
        probe = FakeProbe(present={PATHS["upperbody"]},
                          missing={PATHS["lowerbody"], PATHS["sleeves"]})
        result = outfit.audit_records([record(PATHS["upperbody"])], complete, probe)

        self.assertEqual(result["missing_records"], 1)
        self.assertEqual(result["incomplete_pairs"], 1)
        self.assertEqual(result["fallback_pairs"], 0)
        self.assertEqual(len(result["failures"]), 1)
        self.assertEqual(result["failures"][0]["kind"], "missing")
        self.assertEqual(result["failures"][0]["missing"], ["lowerbody", "sleeves"])

    def test_an_unbuildable_part_fails_the_record(self) -> None:
        complete = {GROUP: set(outfit.PARTS)}
        probe = FakeProbe(present=PATHS.values(),
                          bad={PATHS["upperbody"]: "no usable LOD0 mesh"})
        result = outfit.audit_records([record(*PATHS.values())], complete, probe)

        self.assertEqual(result["missing_records"], 0)
        self.assertEqual(result["nonbuildable_records"], 1)
        self.assertEqual(len(result["failures"]), 1)
        self.assertEqual(result["failures"][0]["kind"], "nonbuildable")
        self.assertEqual(result["failures"][0]["path"], PATHS["upperbody"])

    def test_a_record_can_take_its_parts_from_more_than_one_group(self) -> None:
        second = ("clothes/other", "other_{}.nif")
        other = {token: f"clothes/other/other_{token}.nif" for token in outfit.PARTS}
        complete = {GROUP: set(outfit.PARTS), second: set(outfit.PARTS)}
        probe = FakeProbe(present=set(PATHS.values()) | set(other.values()))
        # The upper body comes from the first group, the rest from the second.
        # The guard must not add either group's upper body again.
        result = outfit.audit_records(
            [record(PATHS["upperbody"], other["lowerbody"], other["sleeves"])],
            complete, probe)

        self.assertEqual(result["missing_records"], 0)
        self.assertEqual(result["nonbuildable_records"], 0)
        self.assertEqual(result["incomplete_pairs"], 2)
        self.assertEqual(result["covered_elsewhere"], 2)
        self.assertEqual(result["fallback_pairs"], 0)
        self.assertEqual(result["failures"], [])


class JUnitTests(unittest.TestCase):
    """The audit report must name every failed record, not just the audit."""

    def _write(self, result: dict) -> ET.Element:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "audit.xml"
            outfit.write_junit(path, result)
            return ET.parse(path).getroot()

    def test_a_clean_install_has_one_passing_case(self) -> None:
        result = outfit.audit_records(
            [record(*PATHS.values())], {GROUP: set(outfit.PARTS)},
            FakeProbe(present=PATHS.values()))

        root = self._write(result)

        self.assertEqual(root.get("tests"), "1")
        self.assertEqual(root.get("failures"), "0")
        self.assertIsNone(root.find("testcase/failure"))
        self.assertIn("complete and buildable", root.find("testcase").get("name"))

    def test_two_failed_records_are_two_cases(self) -> None:
        result = outfit.audit_records(
            [record(PATHS["upperbody"], form=1),
             record(PATHS["upperbody"], form=2)],
            {GROUP: set(outfit.PARTS)},
            FakeProbe(present={PATHS["upperbody"]},
                      missing={PATHS["lowerbody"], PATHS["sleeves"]}))

        root = self._write(result)

        self.assertEqual(root.get("tests"), "2")
        self.assertEqual(root.get("failures"), "2")
        names = {case.get("name") for case in root.findall("testcase")}
        self.assertEqual(names, {"Test.esm 00000001 Test Suit",
                                 "Test.esm 00000002 Test Suit"})
        for case in root.findall("testcase"):
            message = case.find("failure").get("message")
            self.assertIn("missing: lowerbody, sleeves", message)

    def test_one_record_keeps_missing_and_nonbuildable_findings_together(self) -> None:
        result = outfit.audit_records(
            [record(PATHS["upperbody"], PATHS["lowerbody"])],
            {GROUP: set(outfit.PARTS)},
            FakeProbe(present={PATHS["upperbody"], PATHS["lowerbody"]},
                      missing={PATHS["sleeves"]},
                      bad={PATHS["upperbody"]: "no usable LOD0 mesh",
                           PATHS["lowerbody"]: "NIF parse failed"}))

        root = self._write(result)

        self.assertEqual(root.get("tests"), "1")
        case = root.find("testcase")
        failure = case.find("failure")
        message = failure.get("message")
        self.assertIn("missing: sleeves", message)
        self.assertIn("non-buildable:", message)
        self.assertIn("clothes/test/suit_upperbody.nif", message)
        self.assertIn("clothes/test/suit_lowerbody.nif", message)
        self.assertIn("referenced groups: clothes/test/suit_{}", failure.text)
        self.assertIn("clothes/test/suit_upperbody.nif (no usable LOD0 mesh)",
                      failure.text)
        self.assertIn("clothes/test/suit_lowerbody.nif (NIF parse failed)",
                      failure.text)


class BuildGateTests(unittest.TestCase):
    """The repeatable command must stay wired into the real build gate."""

    def test_build_cmd_runs_the_audit_and_stops_on_failure(self) -> None:
        build = (HERE.parents[1] / "build.cmd").read_text(encoding="utf-8")
        self.assertIn("outfit_assembly_audit.py", build)
        self.assertIn("SFSE_SKIP_OUTFIT_AUDIT", build)
        self.assertIn("goto :outfit_failed", build)
        self.assertIn("exit /b 1", build)
        self.assertIn("%TEMP%\\sfse_outfit_assembly_audit.md", build)


class AuditCommandTests(unittest.TestCase):
    """The CLI contract: 0 on a complete set, 1 on a finding."""

    def _main(self, probe: FakeProbe, junit: bool = False
              ) -> tuple[int, str, str | None]:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            catalog = root / "catalog.csv"
            catalog.write_text("unused\n", encoding="utf-8")
            report = root / "report.md"
            junit_path = root / "audit.xml"

            class Store:
                def index_loose(self) -> None:
                    pass

                def _build_index(self) -> None:
                    pass

            argv = ["outfit_assembly_audit.py", "--out", str(report),
                    "--data", str(root)]
            if junit:
                argv += ["--junitxml", str(junit_path)]
            with (
                mock.patch.object(audit, "find_catalog", return_value=catalog),
                mock.patch.object(audit, "parse_catalog",
                                  return_value=[record(PATHS["upperbody"])]),
                mock.patch.object(audit, "AssetStore", return_value=Store()),
                mock.patch.object(outfit, "complete_groups",
                                  return_value={GROUP: set(outfit.PARTS)}),
                mock.patch.object(outfit, "Probe", return_value=probe),
                mock.patch.object(sys, "argv", argv),
            ):
                code = outfit.main()
            xml = (junit_path.read_text(encoding="utf-8")
                   if junit_path.is_file() else None)
            return code, report.read_text(encoding="utf-8"), xml

    def test_complete_set_passes(self) -> None:
        code, report, _ = self._main(FakeProbe(present=PATHS.values()))
        self.assertEqual(code, 0)
        self.assertIn("Status: **PASS**", report)

    def test_missing_part_exits_one(self) -> None:
        code, report, _ = self._main(
            FakeProbe(present={PATHS["upperbody"]},
                      missing={PATHS["lowerbody"], PATHS["sleeves"]}))
        self.assertEqual(code, 1)
        self.assertIn("Status: **FAIL**", report)
        self.assertIn("missing part", report)

    def test_unbuildable_part_exits_one(self) -> None:
        code, report, _ = self._main(
            FakeProbe(present=PATHS.values(),
                      bad={PATHS["upperbody"]: "no usable LOD0 mesh"}))
        self.assertEqual(code, 1)
        self.assertIn("non-buildable", report)

    def test_junitxml_is_written_by_the_command(self) -> None:
        code, _, xml = self._main(
            FakeProbe(present={PATHS["upperbody"]},
                      missing={PATHS["lowerbody"], PATHS["sleeves"]}),
            junit=True)

        self.assertEqual(code, 1)
        self.assertIsNotNone(xml)
        root = ET.fromstring(xml)
        self.assertEqual(root.get("name"), "outfit_assembly_audit")
        self.assertEqual(root.get("tests"), "1")
        self.assertEqual(root.get("failures"), "1")
        self.assertIn("Test Suit", root.find("testcase").get("name"))


if __name__ == "__main__":
    unittest.main()
