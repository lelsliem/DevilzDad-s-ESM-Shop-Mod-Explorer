#!/usr/bin/env python3
"""Guards for the JUnit XML reporting used by `tools/check.py`.

CI treats this file as the per-suite result, so the writer must count outcomes
the way unittest does (including subtests and unexpected successes), and a
suite that never produced XML must still appear as an error instead of silently
disappearing from the report.
"""
from __future__ import annotations

import io
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE / "ba2_audit"))
sys.path.insert(0, str(HERE / "renderer_recon"))

import check  # noqa: E402
import junit_runner  # noqa: E402


class CleanXmlTests(unittest.TestCase):
    def test_invalid_xml_characters_are_removed_and_unicode_is_kept(self) -> None:
        raw = "a\x00b\x0bc\ud800d\ufffee\x7f\U0001F600"
        self.assertEqual(junit_runner.clean_xml(raw), "abcde\x7f\U0001F600")


class WriterTests(unittest.TestCase):
    def test_each_unittest_outcome_is_recorded_once(self) -> None:
        class Outcomes(unittest.TestCase):
            def test_pass(self) -> None:
                pass

            def test_failure(self) -> None:
                self.fail("boom")

            def test_error(self) -> None:
                raise RuntimeError("kaboom")

            def test_skip(self) -> None:
                self.skipTest("not today")

            @unittest.expectedFailure
            def test_expected_failure(self) -> None:
                self.fail("known")

            @unittest.expectedFailure
            def test_unexpected_success(self) -> None:
                pass

            def test_subtest_failure(self) -> None:
                with self.subTest(value=1):
                    self.assertEqual(1, 2)

            def test_subtest_error(self) -> None:
                with self.subTest(value=1):
                    raise RuntimeError("subtest kaboom")

        suite = unittest.defaultTestLoader.loadTestsFromTestCase(Outcomes)
        result = unittest.TextTestRunner(
            stream=io.StringIO(), verbosity=0,
            resultclass=junit_runner.JUnitResult).run(suite)

        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "outcomes.xml"
            junit_runner.write_junit(path, "outcomes", result)
            root = ET.parse(path).getroot()

        self.assertEqual(root.tag, "testsuite")
        self.assertEqual(root.get("tests"), "8")
        self.assertEqual(root.get("failures"), "3")
        self.assertEqual(root.get("errors"), "2")
        self.assertEqual(root.get("skipped"), "2")
        cases = root.findall("testcase")
        self.assertEqual(len(cases), 8)

        by_name = {case.get("name"): case for case in cases}
        self.assertIsNotNone(by_name["test_failure"].find("failure"))
        self.assertIsNotNone(by_name["test_error"].find("error"))
        self.assertIsNotNone(by_name["test_skip"].find("skipped"))
        self.assertIsNotNone(by_name["test_expected_failure"].find("skipped"))
        self.assertIsNotNone(by_name["test_unexpected_success"].find("failure"))
        self.assertIsNotNone(by_name["test_subtest_failure"].find("failure"))
        self.assertIsNotNone(by_name["test_subtest_error"].find("error"))
        self.assertIsNone(by_name["test_pass"].find("failure"))


class MergeTests(unittest.TestCase):
    def test_a_missing_suite_is_an_error_case(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "check.xml"
            check.merge_junit(
                {}, [("tools", 0), ("ba2_audit", 1), ("renderer_recon", 2)],
                path, None, True)
            root = ET.parse(path).getroot()

        self.assertEqual(root.get("tests"), "3")
        self.assertEqual(root.get("errors"), "3")
        self.assertEqual([suite.get("name") for suite in root],
                         ["tools", "ba2_audit", "renderer_recon"])
        for suite in root:
            case = suite.find("testcase")
            self.assertIsNotNone(case.find("error"))

    def test_malformed_suite_xml_is_an_error_case(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            bad = Path(temp) / "bad.xml"
            bad.write_text("<testsuite", encoding="utf-8")
            path = Path(temp) / "check.xml"
            check.merge_junit({"tools": bad}, [("tools", 0)], path, None, True)
            root = ET.parse(path).getroot()

        self.assertEqual(root.get("errors"), "1")
        self.assertIn("unusable", root.find("testsuite/testcase/error").get("message"))

    def test_the_audit_is_a_failure_or_a_skip(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            failed = Path(temp) / "failed.xml"
            check.merge_junit({}, [("tools", 0)], failed, 2, False)
            skipped = Path(temp) / "skipped.xml"
            check.merge_junit({}, [("tools", 0)], skipped, None, False)

            failed_root = ET.parse(failed).getroot()
            skipped_root = ET.parse(skipped).getroot()

        audit_failure = failed_root.findall("testsuite")[-1]
        self.assertEqual(audit_failure.get("name"), "outfit_assembly_audit")
        self.assertEqual(audit_failure.get("failures"), "1")
        audit_skip = skipped_root.findall("testsuite")[-1]
        self.assertEqual(audit_skip.get("skipped"), "1")

    def test_the_audit_supplies_its_per_record_suite(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            audit = Path(temp) / "audit.xml"
            suite = check.synthetic_suite(
                "outfit_assembly_audit", "Test.esm 00000001 Test Suit",
                "failure", "missing: sleeves")
            ET.ElementTree(suite).write(audit, encoding="utf-8",
                                        xml_declaration=True)
            path = Path(temp) / "check.xml"
            check.merge_junit({"outfit_assembly_audit": audit},
                              [("tools", 0)], path, 1, False)
            root = ET.parse(path).getroot()

        audit_suite = root.findall("testsuite")[-1]
        self.assertEqual(audit_suite.get("name"), "outfit_assembly_audit")
        self.assertEqual(audit_suite.find("testcase").get("name"),
                         "Test.esm 00000001 Test Suit")
        self.assertEqual(root.get("failures"), "1")


if __name__ == "__main__":
    unittest.main()
