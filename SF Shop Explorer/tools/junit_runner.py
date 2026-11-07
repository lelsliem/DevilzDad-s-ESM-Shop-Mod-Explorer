#!/usr/bin/env python3
"""Run one unittest discovery and write its results as JUnit XML.

This is the per-suite worker for `tools/check.py`. It uses only the standard
library so CI does not need an extra XML-reporting package.

    python tools/junit_runner.py --start tools --name tools --out tools.xml
"""
from __future__ import annotations

import argparse
import sys
import time
import traceback
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

_SEVERITY = {"success": 0, "skipped": 1, "failure": 2, "error": 3}


def clean_xml(text: str) -> str:
    """Remove characters XML 1.0 cannot represent."""
    def allowed(ch: str) -> bool:
        point = ord(ch)
        return (ch in "\t\n\r"
                or 0x20 <= point <= 0xD7FF
                or 0xE000 <= point <= 0xFFFD
                or 0x10000 <= point <= 0x10FFFF)
    return "".join(ch for ch in text if allowed(ch))


def exception_text(err) -> str:
    if not err:
        return ""
    return clean_xml("".join(traceback.format_exception(*err)))


class JUnitResult(unittest.TextTestResult):
    """Collect one entry per test (and class-level setup/teardown error)."""

    def __init__(self, stream, descriptions, verbosity) -> None:
        super().__init__(stream, descriptions, verbosity)
        self.cases: list[dict] = []
        self._active: dict | None = None
        self._started = 0.0
        self.elapsed = 0.0

    def startTestRun(self) -> None:
        self._started = time.perf_counter()
        super().startTestRun()

    def stopTestRun(self) -> None:
        self.elapsed = time.perf_counter() - self._started
        super().stopTestRun()

    def startTest(self, test) -> None:
        super().startTest(test)
        self._active = {
            "test": test,
            "start": time.perf_counter(),
            "elapsed": 0.0,
            "outcome": "success",
            "message": "",
            "detail": "",
        }
        self.cases.append(self._active)

    def stopTest(self, test) -> None:
        if self._active is not None:
            self._active["elapsed"] = time.perf_counter() - self._active["start"]
            self._active = None
        super().stopTest(test)

    def _record(self, test, outcome: str, message: str = "", detail: str = "") -> None:
        case = self._active
        temporary = case is None
        if case is None:
            # setUpClass/tearDownClass errors arrive without a startTest event.
            case = {
                "test": test,
                "start": time.perf_counter(),
                "elapsed": 0.0,
                "outcome": "success",
                "message": "",
                "detail": "",
            }
            self.cases.append(case)
        message = clean_xml(str(message))
        detail = clean_xml(detail)
        if _SEVERITY[outcome] > _SEVERITY[case["outcome"]]:
            case["outcome"] = outcome
            case["message"] = message
            case["detail"] = detail
        elif _SEVERITY[outcome] == _SEVERITY[case["outcome"]]:
            if message and not case["message"]:
                case["message"] = message
            if detail and detail not in case["detail"]:
                case["detail"] = (case["detail"] + "\n" + detail).strip()
        if temporary:
            case["elapsed"] = 0.0

    def addSuccess(self, test) -> None:
        super().addSuccess(test)

    def addFailure(self, test, err) -> None:
        super().addFailure(test, err)
        message = str(err[1]) if err else ""
        self._record(test, "failure", message, exception_text(err))

    def addError(self, test, err) -> None:
        super().addError(test, err)
        message = str(err[1]) if err else ""
        self._record(test, "error", message, exception_text(err))

    def addSkip(self, test, reason) -> None:
        super().addSkip(test, reason)
        self._record(test, "skipped", reason)

    def addExpectedFailure(self, test, err) -> None:
        super().addExpectedFailure(test, err)
        self._record(test, "skipped", "expected failure", exception_text(err))

    def addUnexpectedSuccess(self, test) -> None:
        super().addUnexpectedSuccess(test)
        self._record(test, "failure", "unexpected success")

    def addSubTest(self, test, subtest, err) -> None:
        super().addSubTest(test, subtest, err)
        if err is not None:
            outcome = ("failure" if issubclass(err[0], test.failureException)
                       else "error")
            self._record(test, outcome, str(err[1]), exception_text(err))


def write_junit(path: Path, name: str, result: JUnitResult) -> None:
    failures = sum(case["outcome"] == "failure" for case in result.cases)
    errors = sum(case["outcome"] == "error" for case in result.cases)
    skipped = sum(case["outcome"] == "skipped" for case in result.cases)
    duration = result.elapsed or sum(case["elapsed"] for case in result.cases)
    suite = ET.Element("testsuite", {
        "name": name,
        "tests": str(len(result.cases)),
        "failures": str(failures),
        "errors": str(errors),
        "skipped": str(skipped),
        "time": f"{duration:.3f}",
    })
    for case in result.cases:
        test = case["test"]
        raw = test.id()
        if "." in raw:
            classname, test_name = raw.rsplit(".", 1)
        else:
            classname, test_name = name, raw
        node = ET.SubElement(suite, "testcase", {
            "classname": classname,
            "name": test_name,
            "time": f"{case['elapsed']:.3f}",
        })
        outcome = case["outcome"]
        if outcome == "skipped":
            ET.SubElement(node, "skipped", {"message": case["message"] or "skipped"})
        elif outcome in ("failure", "error"):
            child = ET.SubElement(node, outcome, {
                "message": case["message"] or outcome,
                "type": outcome,
            })
            child.text = case["detail"] or None
    path.parent.mkdir(parents=True, exist_ok=True)
    ET.ElementTree(suite).write(path, encoding="utf-8", xml_declaration=True)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--start", required=True,
                        help="unittest discovery start directory")
    parser.add_argument("--name", required=True, help="JUnit suite name")
    parser.add_argument("--out", required=True, help="JUnit XML output path")
    parser.add_argument("--pattern", default="test_*.py",
                        help="test file pattern (default: test_*.py)")
    args = parser.parse_args()

    try:
        suite = unittest.defaultTestLoader.discover(args.start, pattern=args.pattern)
        runner = unittest.TextTestRunner(verbosity=0, resultclass=JUnitResult)
        result = runner.run(suite)
        write_junit(Path(args.out), args.name, result)
    except Exception as exc:  # discovery or XML I/O failure
        print(f"[junit] {args.name}: FAILED to run or write XML: {exc}",
              file=sys.stderr)
        return 2

    ok = result.wasSuccessful() and not result.unexpectedSuccesses
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
