#!/usr/bin/env python3
"""Check that every complete three-part outfit can be assembled and built.

The C++ fallback (`LiveModel::BuildModels` / `AssembleSiblings`) merges
`upperbody`, `lowerbody` and `sleeves` NIFs from one folder and stem. This
command mirrors that rule against the installed assets and the catalog dump:

  * find every complete three-part group in loose files and BA2 name tables;
  * collect each ARMO record's declared part types and resolve the missing
    siblings with the same filename-only replacement and declared-part guard;
  * parse every NIF in the resulting set and require at least one usable LOD0
    `.mesh`.

It writes a Markdown report and exits `1` when a referenced complete outfit is
missing a part or a part NIF cannot be built, so it can be used as a check.

    python tools/renderer_recon/outfit_assembly_audit.py
    python tools/renderer_recon/outfit_assembly_audit.py --out reports/audit.md
    python tools/renderer_recon/outfit_assembly_audit.py --junitxml reports/audit.xml

The JUnit report has one case per failed record (or one passing case when the
installed set is clean), so CI names the record and its missing or non-buildable
parts instead of only reporting that the audit failed.

The report is generated from the current install; it does not launch Starfield.
Geometry uses the same NIF / `.mesh` path as the offline contact sheet. Vertex
alignment and material appearance are outside this check.
"""
from __future__ import annotations

import argparse
import sys
import time
import xml.etree.ElementTree as ET
from collections import Counter, defaultdict
from datetime import date
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "ba2_audit"))

import aid_colour_audit as audit  # noqa: E402
import contact_sheet as sheet  # noqa: E402
import junit_runner  # noqa: E402
from nif_uv import Nif  # noqa: E402

PARTS = ("upperbody", "lowerbody", "sleeves")
DEFAULT_REPORT = HERE.parent.parent / "reports" / "outfit-render-all-audit-2026-11-01.md"


def norm_path(raw: str) -> str:
    """Catalog/archive spelling -> slash-separated path below `meshes/`."""
    path = str(raw).replace("\\", "/").lower().lstrip("/")
    while path.startswith("data/"):
        path = path[5:]
    while path.startswith("meshes/"):
        path = path[7:]
    return path


def part_token(path: str) -> str | None:
    """The first outfit part token in the basename, in C++ priority order."""
    base = norm_path(path).rsplit("/", 1)[-1]
    for token in PARTS:
        if token in base:
            return token
    return None


def group_key(path: str) -> tuple[str, str] | None:
    """(folder, stem with `{}` in place of the part token), or None."""
    path = norm_path(path)
    token = part_token(path)
    if token is None or "/" not in path:
        return None
    folder, base = path.rsplit("/", 1)
    return folder, base.replace(token, "{}", 1)


def sibling_paths(path: str) -> list[str]:
    """The two sibling names the C++ fallback would try, filename only."""
    rel = norm_path(path)
    token = part_token(rel)
    if token is None:
        return []
    at = rel.find(token, rel.rfind("/") + 1)
    return [rel[:at] + other + rel[at + len(token):]
            for other in PARTS if other != token]


def group_paths(group: tuple[str, str]) -> list[str]:
    folder, base = group
    return [f"{folder}/{base.replace('{}', token)}" for token in PARTS]


def unique(paths) -> list[str]:
    seen: set[str] = set()
    out: list[str] = []
    for raw in paths:
        path = norm_path(raw)
        if path and path not in seen:
            seen.add(path)
            out.append(path)
    return out


class Probe:
    """Cached presence and LOD0 buildability checks for one asset store."""

    def __init__(self, store: audit.AssetStore) -> None:
        self.store = store
        self.blobs: dict[str, bytes | None] = {}
        self.meshes: dict[str, dict | None] = {}
        self.usable_cache: dict[str, tuple[bool, str]] = {}

    def read(self, key: str) -> bytes | None:
        if key not in self.blobs:
            self.blobs[key] = self.store.read(key)
        return self.blobs[key]

    def exists(self, path: str) -> bool:
        return self.read("meshes/" + norm_path(path)) is not None

    def usable(self, path: str) -> tuple[bool, str]:
        """Whether the NIF yields at least one parseable LOD0 mesh."""
        key = norm_path(path)
        cached = self.usable_cache.get(key)
        if cached is not None:
            return cached
        blob = self.read("meshes/" + key)
        if not blob:
            result = (False, "missing")
            self.usable_cache[key] = result
            return result
        try:
            nif = Nif(blob)
            geoms = sheet.walk_geometries(blob, nif)
        except Exception as exc:  # a malformed NIF is a finding, not a crash
            result = (False, f"NIF parse failed: {exc}")
            self.usable_cache[key] = result
            return result
        for geom in geoms:
            if not geom.get("lods"):
                continue
            for raw in (geom.get("inline"), geom.get("inline_alt")):
                if raw and sheet.parse_mesh(raw):
                    result = (True, "")
                    self.usable_cache[key] = result
                    return result
            mesh_key = sheet.mesh_key(geom["lods"][0][2])
            if mesh_key not in self.meshes:
                self.meshes[mesh_key] = sheet.parse_mesh(self.read(mesh_key) or b"")
            if self.meshes[mesh_key]:
                result = (True, "")
                self.usable_cache[key] = result
                return result
        result = (False, "no usable LOD0 mesh")
        self.usable_cache[key] = result
        return result


def complete_groups(store: audit.AssetStore) -> dict[tuple[str, str], set[str]]:
    """Every folder/stem with all three part NIFs in loose files or BA2s."""
    names = set(store.name_owner)
    for files in store.loose_index or []:
        names.update(files)
    groups: dict[tuple[str, str], set[str]] = defaultdict(set)
    for name in names:
        path = norm_path(name)
        if not path.endswith(".nif"):
            continue
        key = group_key(path)
        token = part_token(path)
        if key is not None and token is not None:
            groups[key].add(token)
    return {key: tokens for key, tokens in groups.items() if len(tokens) == 3}


def audit_records(catalog: list[dict], complete: dict[tuple[str, str], set[str]],
                  probe: Probe) -> dict:
    """Apply the C++ assembly rule to every ARMO record and collect findings."""
    armo = [r for r in catalog if r.get("type") == "ARMO"]
    stats: Counter = Counter()
    failures: list[dict] = []
    all_missing: list[tuple[dict, list[str]]] = []
    missing_part_paths: list[tuple[dict, list[str]]] = []
    one_part_no_sibling: list[tuple[dict, str]] = []
    used_paths: set[str] = set()
    referenced_groups: set[tuple[str, str]] = set()
    declared_counts: Counter = Counter()
    pairs = fallback_pairs = incomplete_pairs = covered_elsewhere = 0
    some_missing = 0
    missing_records: set[tuple[str, int]] = set()
    nonbuildable_records: set[tuple[str, int]] = set()

    for record in armo:
        paths = unique(record.get("nif_paths") or [])
        if not paths:
            stats["no_paths"] += 1
            continue
        present = [path for path in paths if probe.exists(path)]
        missing = [path for path in paths if path not in present]
        if not present:
            stats["all_missing"] += 1
            all_missing.append((record, paths))
        elif missing:
            stats["some_missing"] += 1
            some_missing += 1
        else:
            stats["all_present"] += 1

        missing_part = [path for path in missing if part_token(path)]
        if missing_part:
            missing_part_paths.append((record, missing_part))
        part_paths = [path for path in paths if part_token(path)]
        if part_paths and not any(probe.exists(path) for path in part_paths):
            stats["part_paths_all_missing"] += 1
        declared_types = {part_token(path) for path in part_paths}
        if len(part_paths) == 1:
            candidates = sibling_paths(part_paths[0])
            if not any(probe.exists(path) for path in candidates):
                one_part_no_sibling.append((record, part_paths[0]))

        groups = sorted({key for path in paths
                         if (key := group_key(path)) in complete})
        record_types = set(declared_types)
        record_paths: set[str] = set()
        for group in groups:
            declared = [path for path in paths if group_key(path) == group]
            added: list[str] = []
            for path in declared:
                for sibling in sibling_paths(path):
                    if (sibling in declared
                            or part_token(sibling) in declared_types):
                        continue
                    if probe.exists(sibling):
                        added.append(sibling)
            final = unique(declared + added)
            final_types = {part_token(path) for path in final}
            group_types = {part_token(path) for path in declared}
            pairs += 1
            declared_counts[len(group_types)] += 1
            missing_in_group = set(PARTS) - group_types
            if missing_in_group:
                incomplete_pairs += 1
                if missing_in_group <= declared_types:
                    covered_elsewhere += 1
            referenced_groups.add(group)
            if added:
                fallback_pairs += 1
            used_paths.update(final)
            record_paths.update(final)
            record_types.update(final_types)

        if not groups:
            continue
        record_key = (record.get("plugin", ""), record.get("form", 0))
        missing_types = sorted(set(PARTS) - record_types)
        if missing_types:
            missing_records.add(record_key)
            failures.append({
                "kind": "missing",
                "record": record,
                "groups": groups,
                "missing": missing_types,
            })
        for path in record_paths:
            ok, status = probe.usable(path)
            if not ok:
                nonbuildable_records.add(record_key)
                failures.append({
                    "kind": "nonbuildable",
                    "record": record,
                    "path": path,
                    "status": status,
                })

    nonbuildable: dict[str, str] = {}
    for path in used_paths:
        ok, status = probe.usable(path)
        if not ok:
            nonbuildable[path] = status
    unreferenced: list[tuple[str, str]] = []
    for group in complete:
        for path in group_paths(group):
            if path in used_paths:
                continue
            ok, status = probe.usable(path)
            if not ok:
                unreferenced.append((path, status))
    return {
        "armo": len(armo),
        "complete": complete,
        "referenced": referenced_groups,
        "pairs": pairs,
        "declared_counts": declared_counts,
        "fallback_pairs": fallback_pairs,
        "incomplete_pairs": incomplete_pairs,
        "covered_elsewhere": covered_elsewhere,
        "used_paths": used_paths,
        "nonbuildable": nonbuildable,
        "missing_records": len(missing_records),
        "nonbuildable_records": len(nonbuildable_records),
        "failures": failures,
        "all_missing": all_missing,
        "some_missing": some_missing,
        "missing_part_paths": missing_part_paths,
        "one_part_no_sibling": one_part_no_sibling,
        "unreferenced_nonbuildable": unreferenced,
        "stats": stats,
    }


def failure_cases(result: dict) -> list[dict]:
    """Group findings by record so each failed record is one JUnit case."""
    cases: dict[tuple[str, int], dict] = {}
    ordered: list[dict] = []
    for failure in result["failures"]:
        record = failure["record"]
        key = (str(record.get("plugin", "?")), int(record.get("form", 0)))
        case = cases.get(key)
        if case is None:
            case = {"record": record, "missing": [], "groups": [], "nonbuildable": []}
            cases[key] = case
            ordered.append(case)
        if failure["kind"] == "missing":
            for part in failure["missing"]:
                if part not in case["missing"]:
                    case["missing"].append(part)
            for group in failure["groups"]:
                if group not in case["groups"]:
                    case["groups"].append(group)
        else:
            item = (failure["path"], failure["status"])
            if item not in case["nonbuildable"]:
                case["nonbuildable"].append(item)
    return ordered


def record_case_name(record: dict) -> str:
    form = int(record.get("form", 0))
    return (f"{record.get('plugin', '?')} {form:08X} "
            f"{record.get('name', '')}").strip()


def write_junit(path: Path, result: dict, elapsed: float = 0.0) -> None:
    """Write one JUnit case per failed outfit record."""
    cases = failure_cases(result)
    suite = ET.Element("testsuite", {
        "name": "outfit_assembly_audit",
        "tests": str(len(cases) or 1),
        "failures": str(len(cases)),
        "errors": "0",
        "skipped": "0",
        "time": f"{elapsed:.3f}",
    })
    if not cases:
        ET.SubElement(suite, "testcase", {
            "classname": "outfit_assembly_audit",
            "name": "all referenced outfits are complete and buildable",
            "time": "0.000",
        })
    for case in cases:
        missing = case["missing"]
        nonbuildable = case["nonbuildable"]
        messages: list[str] = []
        details: list[str] = []
        if missing:
            parts = ", ".join(missing)
            messages.append(f"missing: {parts}")
            groups = ", ".join(f"{folder}/{base}"
                               for folder, base in case["groups"])
            details.append(f"missing parts: {parts}")
            if groups:
                details.append(f"referenced groups: {groups}")
        if nonbuildable:
            ordered = sorted(nonbuildable)
            messages.append("non-buildable: " + ", ".join(
                path for path, _status in ordered))
            details.append("non-buildable parts:\n" + "\n".join(
                f"  {path} ({status})" for path, status in ordered))
        node = ET.SubElement(suite, "testcase", {
            "classname": "outfit_assembly_audit",
            "name": junit_runner.clean_xml(record_case_name(case["record"])),
            "time": "0.000",
        })
        failure = ET.SubElement(node, "failure", {
            "message": junit_runner.clean_xml("; ".join(messages)),
            "type": "outfit-assembly",
        })
        failure.text = junit_runner.clean_xml("\n".join(details)) or None
    path.parent.mkdir(parents=True, exist_ok=True)
    ET.ElementTree(suite).write(path, encoding="utf-8", xml_declaration=True)


def md_code(value: object) -> str:
    return "`" + str(value).replace("`", "'") + "`"


def render_report(result: dict, catalog_path: Path) -> str:
    """A self-contained Markdown report of the current install."""
    failures = result["failures"]
    status = "FAIL" if failures else "PASS"
    generated = date.today().isoformat()
    d1 = result["declared_counts"].get(1, 0)
    d2 = result["declared_counts"].get(2, 0)
    d3 = result["declared_counts"].get(3, 0)

    def record_head(record: dict) -> str:
        form = record.get("form", 0)
        return (f"{md_code(record.get('plugin', '?'))} "
                f"{md_code(f'{form:08X}')} "
                f"**{record.get('name', '')}**")

    lines = [
        f"# Outfit \"render all\" audit — {generated}",
        "",
        f"Status: **{status}**. The command exits "
        f"{1 if failures else 0} when run against this install.",
        "",
        "Scope: every installed ARMO record and every complete `upperbody` / "
        "`lowerbody` / `sleeves` NIF set in the active profile. The check "
        "mirrors `LiveModel::BuildModels` and `AssembleSiblings`, including "
        "the filename-only part token and the declared-part guard. It parses "
        "each NIF's LOD0 mesh with the same offline path as the contact sheet; "
        "it does not verify vertex alignment or material appearance.",
        "",
        f"Catalog: {md_code(catalog_path)}",
        "",
        "## Result",
        "",
        "| Measure | Count |",
        "|---|---:|",
        f"| ARMO records | {result['armo']:,} |",
        f"| Complete 3-part asset groups | {len(result['complete']):,} |",
        f"| Complete groups referenced by an ARMO | {len(result['referenced']):,} |",
        f"| Record/group pairs checked | {result['pairs']:,} |",
        f"| Pairs declaring 3 parts | {d3:,} |",
        f"| Pairs declaring 2 parts | {d2:,} |",
        f"| Pairs declaring 1 part | {d1:,} |",
        f"| Pairs with an in-group missing part | {result['incomplete_pairs']:,} |",
        f"| Pairs where the fallback supplied a part | {result['fallback_pairs']:,} |",
        f"| Pairs already covered by another group | {result['covered_elsewhere']:,} |",
        f"| Unique NIFs in the assembled sets | {len(result['used_paths']):,} |",
        f"| Records missing a part | **{result['missing_records']}** |",
        f"| Records with a non-buildable part | **{result['nonbuildable_records']}** |",
        f"| Unique non-buildable NIFs | **{len(result['nonbuildable'])}** |",
        "",
        "An in-group missing part is not automatically a failed outfit: a record "
        "can take that type from another complete group it declares. The failure "
        "check is the record's assembled union, not each group in isolation.",
        "",
    ]
    if failures:
        lines += [
            "## Failures",
            "",
            "The command exits 1 until every entry below is fixed.",
            "",
        ]
        for failure in failures:
            record = failure["record"]
            head = record_head(record)
            if failure["kind"] == "missing":
                groups = ", ".join(
                    md_code(f"{folder}/{base}") for folder, base in failure["groups"])
                lines.append(
                    f"- **missing part** {head}: missing "
                    f"{', '.join(failure['missing'])}; referenced group(s) "
                    f"{groups}.")
            else:
                lines.append(
                    f"- **non-buildable** {head}: {md_code(failure['path'])} "
                    f"({failure['status']}).")
        lines.append("")
    else:
        lines += [
            "## Failures",
            "",
            "None. Every referenced record has all three part types and every "
            "assembled part NIF yields a usable LOD0 mesh.",
            "",
        ]

    warnings: list[str] = []
    if result["all_missing"]:
        warnings.append(
            f"- **{len(result['all_missing'])} records have every declared NIF "
            "missing.** These are not outfit-assembly failures by themselves."
        )
        for record, paths in result["all_missing"]:
            warnings.append(
                f"  - {record_head(record)}: "
                + ", ".join(md_code(path) for path in paths)
            )
    if result["missing_part_paths"]:
        warnings.append(
            f"- **{len(result['missing_part_paths'])} records have a missing "
            "token-defined part path.** The remaining parts still render."
        )
        for record, paths in result["missing_part_paths"]:
            warnings.append(
                f"  - {record_head(record)}: "
                + ", ".join(md_code(path) for path in paths)
            )
    if result["one_part_no_sibling"]:
        warnings.append(
            f"- **{len(result['one_part_no_sibling'])} one-part records have no "
            "sibling on disk.** They are standalone or single-part assets, not "
            "failed half-outfits."
        )
    if result["stats"]["part_paths_all_missing"]:
        warnings.append(
            f"- **{result['stats']['part_paths_all_missing']} records declare "
            "part paths but none of those paths exist.**"
        )
    if result["unreferenced_nonbuildable"]:
        warnings.append(
            f"- **{len(result['unreferenced_nonbuildable'])} complete-group "
            "NIFs are unused by any record and do not build.** They are orphan "
            "assets, not record gaps."
        )
        for path, status in result["unreferenced_nonbuildable"]:
            warnings.append(f"  - {md_code(path)} ({status})")
    lines += [
        "## Path health",
        "",
        "| Declared paths | Records |",
        "|---|---:|",
        f"| All present | {result['stats']['all_present']:,} |",
        f"| Some missing | {result['some_missing']:,} |",
        f"| All missing | {result['stats']['all_missing']:,} |",
        f"| No NIF paths | {result['stats']['no_paths']:,} |",
        "",
        "## Warnings",
        "",
        *(warnings or ["None."]),
        "",
        "Missing optional/alternate paths are not assembly failures. A record "
        "can combine a custom upper body from one group with a lower body from "
        "another; the declared-part guard deliberately leaves such a group "
        "mixed. The failure check is record-level: the record's union of "
        "declared and fallback parts must contain all three types, and every "
        "part NIF in that union must build. A referenced record that fails "
        "either check is listed above. "
        "`xmake build -y` and the unit tests cover the C++ rule; this command "
        "covers the installed asset set.",
        "",
        "## Sibling rule",
        "",
        "`OutfitPart` takes the token from the basename; `AssembleSiblings` "
        "replaces that exact position. `BuildModels` collects the record's "
        "declared part types and only fills a type the record does not already "
        "have. This avoids both the `SWCLMT/upperbodyaddon/...lowerbody.nif` "
        "folder-token mismatch and adding a second torso to a record that "
        "already supplies a custom upper body. `test_inline_nif_geometry."
        "SiblingAssemblyTests` pins both rules.",
        "",
        "## Reproduce",
        "",
        "```bash",
        "python tools/renderer_recon/outfit_assembly_audit.py",
        "```",
        "",
        "Use `--out` to write a different report, `--junitxml` to write one "
        "JUnit case per failed record, `--catalog` to use another dump, and "
        "`--data` / `--mods-root` to point at another install. The command "
        "exits 1 on a missing or non-buildable referenced part, 2 when the "
        "catalog or complete asset groups cannot be found.",
        "",
        "## Verification limit",
        "",
        "The report is an asset/source audit, not a live Starfield capture. "
        "NIF geometry is parsed offline; part alignment and material appearance "
        "remain visually untested. Run `xmake build -y` and "
        "`python -X utf8 -m unittest tools.test_inline_nif_geometry` separately "
        "for the C++ rule.",
        "",
    ]
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=str(DEFAULT_REPORT),
                    help="Markdown report path")
    ap.add_argument("--catalog", default=None,
                    help="catalog CSV (default: the Explorer's SFDevilzDump)")
    ap.add_argument("--data", default=str(audit.GAME_DATA),
                    help="Starfield Data directory")
    ap.add_argument("--mods-root", default=str(audit.MODS_ROOT),
                    help="MO2 mods directory")
    ap.add_argument("--junitxml", default=None,
                    help="JUnit XML output (one case per failed record)")
    args = ap.parse_args()

    catalog_path = audit.find_catalog(args.catalog)
    if catalog_path is None:
        print("catalog dump not found", file=sys.stderr)
        return 2
    catalog = audit.parse_catalog(catalog_path)
    if not any(record.get("type") == "ARMO" for record in catalog):
        print("catalog has no ARMO records", file=sys.stderr)
        return 2
    if not Path(args.data).is_dir():
        print(f"Data directory not found: {args.data}", file=sys.stderr)
        return 2

    started = time.perf_counter()
    store = audit.AssetStore([Path(args.data)],
                             Path(args.mods_root) if args.mods_root else None,
                             False)
    store.index_loose()
    store._build_index()
    complete = complete_groups(store)
    print(f"[index] {len(complete)} complete 3-part group(s), "
          f"{time.perf_counter() - started:.1f}s", file=sys.stderr)
    if not complete:
        print("no complete three-part outfit groups found", file=sys.stderr)
        return 2

    probe = Probe(store)
    result = audit_records(catalog, complete, probe)
    elapsed = time.perf_counter() - started
    report_path = Path(args.out)
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(render_report(result, catalog_path), encoding="utf-8")
    if args.junitxml:
        write_junit(Path(args.junitxml), result, elapsed)

    print(f"ARMO={result['armo']:,} groups={len(complete)} "
          f"referenced={len(result['referenced'])} pairs={result['pairs']} "
          f"fallback={result['fallback_pairs']} "
          f"coveredElsewhere={result['covered_elsewhere']} "
          f"incomplete={result['incomplete_pairs']} "
          f"uniqueNIFs={len(result['used_paths'])} "
          f"missingRecords={result['missing_records']} "
          f"nonbuildableRecords={result['nonbuildable_records']} "
          f"pathHealth={dict(result['stats'])}")
    if result["failures"]:
        for failure in result["failures"][:20]:
            record = failure["record"]
            if failure["kind"] == "missing":
                groups = ",".join(f"{folder}/{base}"
                                  for folder, base in failure["groups"])
                print(f"FAIL missing {failure['missing']} "
                      f"{record.get('plugin')} {record.get('form'):08X} "
                      f"{record.get('name')} [{groups}]",
                      file=sys.stderr)
            else:
                print(f"FAIL non-buildable {failure['path']} ({failure['status']}) "
                      f"{record.get('plugin')} {record.get('form'):08X} "
                      f"{record.get('name')}", file=sys.stderr)
        print(f"wrote {report_path} (FAIL)", file=sys.stderr)
        return 1
    print(f"wrote {report_path} (PASS)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
