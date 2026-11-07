#!/usr/bin/env python3
"""Run the Python regression suites and the outfit-assembly audit together.

    python -X utf8 tools/check.py
    python -X utf8 tools/check.py --junitxml reports/check.xml
    python -X utf8 tools/check.py --no-audit
    python -X utf8 tools/check.py --report reports/outfit-audit.md

The three unittest trees are discovered separately because `tools/` does not
use `__init__.py`: one discovery pass for `tools`, one for `tools/ba2_audit`, and
one for `tools/renderer_recon`. The audit runs after the suites and writes its
normal report unless `--report` names another path.

The audit needs Starfield's Data directory and a SFDevilzDump catalog. Set
`SFSE_STARFIELD_DATA_OVERRIDE`, `SFSE_CATALOG_OVERRIDE`, or
`SFSE_MODS_ROOT_OVERRIDE` to point at another install. When Data or the catalog
is absent, the audit is reported as SKIPPED rather than failing a checkout that
has never run the game; the suites still decide the command's result.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
RENDERER = HERE / "renderer_recon"
sys.path.insert(0, str(RENDERER))
sys.path.insert(0, str(HERE / "ba2_audit"))

import aid_colour_audit as audit  # noqa: E402
import junit_runner  # noqa: E402

SUITES = (
    ("tools", HERE),
    ("ba2_audit", HERE / "ba2_audit"),
    ("renderer_recon", RENDERER),
)


def run_suite(name: str, directory: Path, junit_path: Path | None = None) -> int:
    relative = directory.relative_to(ROOT).as_posix()
    print(f"[check] suite: {name}", flush=True)
    if junit_path is not None:
        command = [sys.executable, "-X", "utf8", str(HERE / "junit_runner.py"),
                   "--start", relative, "--name", name, "--out", str(junit_path)]
    else:
        command = [sys.executable, "-X", "utf8", "-m", "unittest", "discover",
                   "-s", relative, "-p", "test_*.py", "-q"]
    return subprocess.run(command, cwd=ROOT).returncode


def audit_paths() -> tuple[Path, Path | None, Path | None]:
    """The audit's inputs, including the build.cmd environment overrides."""
    data_override = os.environ.get("SFSE_STARFIELD_DATA_OVERRIDE")
    data = Path(data_override) if data_override else Path(audit.GAME_DATA)

    catalog_override = os.environ.get("SFSE_CATALOG_OVERRIDE")
    catalog = (Path(catalog_override) if catalog_override
               else audit.find_catalog(None))

    mods_override = os.environ.get("SFSE_MODS_ROOT_OVERRIDE")
    mods = Path(mods_override) if mods_override else Path(audit.MODS_ROOT)
    return data, catalog, mods if mods.is_dir() else None


def run_audit(report: str | None, junit_path: Path | None = None) -> int | None:
    data, catalog, mods = audit_paths()
    missing = []
    if not data.is_dir():
        missing.append(f"Data ({data})")
    if catalog is None or not catalog.is_file():
        missing.append("catalog dump")
    if missing:
        print(f"[check] outfit audit SKIPPED: {', '.join(missing)} not found",
              flush=True)
        return None

    print("[check] outfit assembly audit", flush=True)
    command = [sys.executable, "-X", "utf8",
               str(RENDERER / "outfit_assembly_audit.py"),
               "--data", str(data), "--catalog", str(catalog)]
    if mods is not None:
        command += ["--mods-root", str(mods)]
    if report:
        command += ["--out", report]
    if junit_path is not None:
        command += ["--junitxml", str(junit_path)]
    return subprocess.run(command, cwd=ROOT).returncode


def synthetic_suite(name: str, case_name: str, outcome: str,
                    message: str = "", detail: str = "") -> ET.Element:
    """One JUnit suite for a step that is not a normal unittest run."""
    suite = ET.Element("testsuite", {
        "name": name,
        "tests": "1",
        "failures": "1" if outcome == "failure" else "0",
        "errors": "1" if outcome == "error" else "0",
        "skipped": "1" if outcome == "skipped" else "0",
        "time": "0.000",
    })
    case = ET.SubElement(suite, "testcase", {
        "classname": name,
        "name": case_name,
        "time": "0.000",
    })
    message = junit_runner.clean_xml(message)
    detail = junit_runner.clean_xml(detail)
    if outcome == "skipped":
        ET.SubElement(case, "skipped", {"message": message or "skipped"})
    elif outcome in ("failure", "error"):
        child = ET.SubElement(case, outcome, {
            "message": message or outcome,
            "type": outcome,
        })
        child.text = detail or None
    return suite


def add_suite(root: ET.Element, totals: dict, suite: ET.Element) -> None:
    root.append(suite)
    for key in ("tests", "failures", "errors", "skipped"):
        totals[key] += int(suite.get(key, "0"))
    totals["time"] += float(suite.get("time", "0"))


def merge_junit(paths: dict[str, Path], codes: list[tuple[str, int]],
                destination: Path, audit_code: int | None,
                no_audit: bool) -> None:
    """Combine the per-suite files, including the audit's per-record cases."""
    root = ET.Element("testsuites", {"name": "SF Shop Explorer"})
    totals = {"tests": 0, "failures": 0, "errors": 0, "skipped": 0, "time": 0.0}

    for name, code in codes:
        suite = None
        path = paths.get(name)
        if path is not None:
            try:
                suite = ET.parse(path).getroot()
            except (ET.ParseError, OSError) as exc:
                suite = synthetic_suite(
                    name, "junit_runner", "error",
                    f"suite produced unusable JUnit XML (exit {code})", str(exc))
        if suite is None:
            suite = synthetic_suite(
                name, "junit_runner", "error",
                f"suite produced no JUnit XML (exit {code})",
                "The unittest runner did not write its report; "
                "inspect the check output for the discovery or I/O failure.")
        add_suite(root, totals, suite)

    if not no_audit:
        audit_path = paths.get("outfit_assembly_audit")
        suite = None
        if audit_path is not None:
            try:
                suite = ET.parse(audit_path).getroot()
            except (ET.ParseError, OSError) as exc:
                suite = synthetic_suite(
                    "outfit_assembly_audit", "junitxml", "error",
                    "audit produced unusable JUnit XML", str(exc))
        if audit_code is None:
            suite = synthetic_suite("outfit_assembly_audit", "assembly",
                                    "skipped", "assets unavailable", "")
        elif suite is None:
            if audit_code:
                suite = synthetic_suite(
                    "outfit_assembly_audit", "assembly", "failure",
                    f"audit exited {audit_code}",
                    "The outfit assembly audit did not pass.")
            else:
                suite = synthetic_suite(
                    "outfit_assembly_audit", "junitxml", "error",
                    "audit produced no JUnit XML",
                    "The audit passed but did not write its JUnit report.")
        add_suite(root, totals, suite)

    for key, value in totals.items():
        root.set(key, f"{value:.3f}" if key == "time" else str(value))
    destination.parent.mkdir(parents=True, exist_ok=True)
    ET.ElementTree(root).write(destination, encoding="utf-8", xml_declaration=True)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--no-audit", action="store_true",
                        help="run only the Python unittest suites")
    parser.add_argument("--report", default=None,
                        help="write the audit report here instead of its default")
    parser.add_argument("--junitxml", default=None,
                        help="write combined JUnit XML for the suites and the "
                             "per-record audit")
    args = parser.parse_args()

    junit_paths: dict[str, Path] = {}
    test_codes: list[tuple[str, int]] = []
    with tempfile.TemporaryDirectory(prefix="sfse-check-") as temp:
        for name, directory in SUITES:
            junit_path = Path(temp) / f"{name}.xml" if args.junitxml else None
            code = run_suite(name, directory, junit_path)
            test_codes.append((name, code))
            if junit_path is not None and junit_path.is_file():
                junit_paths[name] = junit_path
        audit_junit = (Path(temp) / "outfit_assembly_audit.xml"
                       if args.junitxml and not args.no_audit else None)
        audit_code = (None if args.no_audit
                      else run_audit(args.report, audit_junit))
        if audit_junit is not None and audit_junit.is_file():
            junit_paths["outfit_assembly_audit"] = audit_junit
        if args.junitxml:
            merge_junit(junit_paths, test_codes, Path(args.junitxml),
                        audit_code, args.no_audit)
            print(f"[check] JUnit XML: {args.junitxml}", flush=True)

    failed_tests = [name for name, code in test_codes if code]
    print("", flush=True)
    print("[check] summary", flush=True)
    for name, code in test_codes:
        print(f"  tests {name}: {'PASS' if code == 0 else f'FAIL ({code})'}",
              flush=True)
    if args.no_audit:
        print("  outfit audit: SKIPPED (--no-audit)", flush=True)
    elif audit_code is None:
        print("  outfit audit: SKIPPED (assets unavailable)", flush=True)
    else:
        print(f"  outfit audit: {'PASS' if audit_code == 0 else f'FAIL ({audit_code})'}",
              flush=True)

    if failed_tests or (audit_code is not None and audit_code != 0):
        print("[check] FAILED", flush=True)
        return 1
    print("[check] PASS", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
