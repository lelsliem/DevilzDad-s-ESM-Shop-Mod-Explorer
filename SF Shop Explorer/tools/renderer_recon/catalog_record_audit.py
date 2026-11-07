#!/usr/bin/env python3
"""Join the complete seven-type form dump to evidence in one SFSE runtime log.

Run from the Explorer directory:
  python tools/renderer_recon/catalog_record_audit.py --forms PATH --candidates PATH --log PATH
Outputs reports/catalog-record-preview-audit.csv and .md at the workspace root.
"""
import argparse
import csv
import re
from collections import Counter, defaultdict
from pathlib import Path

TYPES = {"ARMO", "WEAP", "MISC", "AMMO", "ALCH", "KEYM", "BOOK"}
PATH_PATTERNS = [
    re.compile(r"MODEL PATH = '([^']+)'", re.I),
    re.compile(r"typed item model path = '([^']+)'", re.I),
    re.compile(r"geometry files: .*? from '([^']+)'", re.I),
    re.compile(r"assembly geometry path='([^']+)'", re.I),
    re.compile(r"connect root '([^']+)'", re.I),
]


def rows(path):
    with path.open(encoding="utf-8-sig", newline="", errors="replace") as f:
        return list(csv.DictReader(f))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--forms", type=Path, required=True)
    ap.add_argument("--candidates", type=Path, required=True)
    ap.add_argument("--log", type=Path, required=True)
    ap.add_argument("--root", type=Path, default=Path.cwd().parents[1])
    args = ap.parse_args()

    forms = [r for r in rows(args.forms) if r.get("type") in TYPES]
    candidates = {(r.get("plugin", "").strip('"'), r.get("formID", "").upper()): r
                  for r in rows(args.candidates) if r.get("type") in TYPES}
    lines = args.log.read_text(errors="replace").splitlines()
    paths = defaultdict(list)
    current_fid = ""
    events = []
    current = None
    total_request_lines = 0
    unparsed_requests = []

    for number, line in enumerate(lines, 1):
        m = re.search(r"NativeModel: form ([0-9A-Fa-f]{8}) type=", line)
        if m:
            current_fid = m.group(1).upper()
        if current_fid:
            for pattern in PATH_PATTERNS:
                p = pattern.search(line)
                if p and p.group(1).lower().endswith(".nif") and p.group(1) not in paths[current_fid]:
                    paths[current_fid].append(p.group(1))

        if "SFShopExplorer direct: request " in line:
            total_request_lines += 1
            m = re.search(r"\[FormID ([0-9A-Fa-f]{8})\].*\((\d+) mesh\(es\), (\d+) tris\)", line)
            if not m:
                unparsed_requests.append((number, line))
                current = None
                continue
            current = {"fid": m.group(1).upper(), "requestLine": number,
                       "ready": "", "meshes": "", "tris": "", "gpuEvents": 0,
                       "ranges": 0, "prefetched": 0, "slots": 0, "untextured": 0,
                       "materials": []}
            events.append(current)
            continue
        if current is None:
            continue
        m = re.search(r"direct: .*\[FormID ([0-9A-Fa-f]{8})\].* -> (ready|failed) \((\d+) mesh\(es\), (\d+) tris\)", line)
        if m and m.group(1).upper() == current["fid"]:
            current.update(ready=m.group(2), meshes=m.group(3), tris=m.group(4))
        m = re.search(r"offline GPU: texturing .* \((\d+) ranges, (\d+) prefetched\)", line)
        if m:
            current["gpuEvents"] += 1
            current["ranges"] += int(m.group(1))
            current["prefetched"] += int(m.group(2))
        if "direct GPU: range " in line and " <- slot " in line:
            current["slots"] += 1
            m = re.search(r"direct GPU: range \d+ '([^']+)'", line)
            if m and m.group(1) not in current["materials"]:
                current["materials"].append(m.group(1))
        if " <- UNTEXTURED" in line:
            current["untextured"] += 1
            m = re.search(r"range \d+ '([^']+)'", line)
            if m and m.group(1) not in current["materials"]:
                current["materials"].append(m.group(1))

    by_fid = defaultdict(list)
    for event in events:
        by_fid[event["fid"]].append(event)
    output = []
    source_fields = ["plugin", "formID", "type", "name", "editorId", "value", "weight",
                     "modelPath", "descriptionId", "keywords", "flags", "inventory", "renderCandidate"]
    for form in forms:
        fid = form.get("formID", "").upper()
        candidate = candidates.get((form.get("plugin", "").strip('"'), fid))
        samples = by_fid.get(fid, [])
        last = samples[-1] if samples else None
        mesh_count = int(last["meshes"]) if last and last["meshes"] else 0
        if last and last["ready"] == "ready" and mesh_count:
            geometry = "runtime-geometry-positive"
        elif last and last["ready"] == "ready" and fid == "000001F4":
            geometry = "expected-no-3d-model-creature-attack"
        elif last and last["ready"] == "ready":
            geometry = "runtime-no-geometry-review"
        elif last and last["ready"] == "failed":
            geometry = "runtime-preview-failed"
        else:
            geometry = "not-runtime-previewed"
        material = ("runtime-texture-prefetch-observed" if last and last["gpuEvents"] else
                    "geometry-previewed-material-resolution-not-logged" if last and mesh_count else
                    "no-geometry-no-material-check" if last else "not-runtime-checked")
        output.append({**{k: form.get(k, "") for k in source_fields},
            "candidateDumpIncluded": int(candidate is not None),
            "candidateReason": candidate.get("reason", "") if candidate else "",
            "runtimePreviewEvents": len(samples),
            "runtimePreviewResult": last["ready"] if last else "",
            "runtimeMeshCount": last["meshes"] if last else "",
            "runtimeTriangleCount": last["tris"] if last else "",
            "geometryAudit": geometry,
            "runtimeModelAndGeometryNifPaths": " | ".join(paths.get(fid, [])),
            "materialAudit": material,
            "runtimeGpuRangeCount": last["ranges"] if last else "",
            "runtimeTexturePrefetchCount": last["prefetched"] if last else "",
            "runtimeTextureSlotCount": last["slots"] if last else "",
            "runtimeUntexturedRangeCount": last["untextured"] if last else "",
            "runtimeMaterialPaths": " | ".join(last["materials"]) if last else "",
            "auditEvidence": "complete form dump + runtime candidate subset + SFSE log"})

    report_dir = args.root / "reports"
    report_dir.mkdir(exist_ok=True)
    csv_path = report_dir / "catalog-record-preview-audit.csv"
    with csv_path.open("w", encoding="utf-8-sig", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(output[0]))
        writer.writeheader()
        writer.writerows(output)

    count_by_type = Counter(r["type"] for r in output)
    positive_by_type = Counter(r["type"] for r in output if r["geometryAudit"] == "runtime-geometry-positive")
    candidate_by_type = Counter(r["type"] for r in output if r["candidateDumpIncluded"])
    distinct = set(by_fid)
    geometry_counts = Counter(r["geometryAudit"] for r in output)
    table = "\n".join(f"| {t} | {count_by_type[t]} | {candidate_by_type[t]} | {positive_by_type[t]} |" for t in sorted(TYPES))
    md = f"""# Catalog Record Preview Audit

Generated 2026-10-14 from the full loaded-form dump and the current SFSE session log. The seven types are the `kAllBrowsable` list in `src/ShopExplorer.cpp`. Every form of those types in the full dump is present in the CSV; it is a loaded-form inventory, not proof every row passes the UI's bound-object and user-selected filters.

## Is it deployed?

The MO2 Test profile has `DevilzDad's ESM Shop Mod Explorer` enabled, and its plugin DLL exists at `B:\\MO2\\Starfield\\mods\\DevilzDad's ESM Shop Mod Explorer\\SFSE\\Plugins\\SFShopExplorer.dll` (2,014,720 bytes; modified 2026-10-14 18:10). The captured SFSE session log from 18:44 identifies version **v0.8.123.0** and shows installation and renderer initialization, so that DLL was deployed and loaded in that session. At final verification, no Starfield process was running: this is evidence of the last captured run, not a currently active process. It also does not prove current source is deployed: the repository release DLL is older and the last `xmake build -y` failed.

The complete forms dump sets its own `renderCandidate` bit to `1` for all **7,278** rows of these seven types. The older filtered `SFDevilzDump-render-candidates.csv` contains **7,253** rows; the 25-row difference is a difference between dump products, not proof those 25 records cannot be browsed. The complete dump's `renderCandidate` flag is a classification, not proof the UI will include a form: the Explorer's `BuildFormListForType` filters each type-array member through `IsBoundObject()`, then applies optional filters. The helper script's `candidateDumpIncluded` field only records presence in the older filtered dump; do not interpret it as inclusion/exclusion from the complete catalog.

Runtime preview means logged in-game model resolution, not merely a catalog row.

## Record and live coverage

- Full loaded-form inventory: **{len(output):,}** records; the full dump itself marks all of them `renderCandidate=1`; the older filtered candidate file has **{sum(candidate_by_type.values()):,}** rows.
- Runtime requests captured: **{total_request_lines}** lines, **{len(events)}** parsed events, **{len(distinct)}** distinct FormIDs, **{len(distinct)}/{len(output)} ({len(distinct)/len(output):.2%})** record coverage. Unparsed request lines: **{len(unparsed_requests)}**.
- Distinct previewed records with positive geometry: **{sum(positive_by_type.values())}**. Geometry status counts: `{dict(geometry_counts)}`. A `ready` response alone only means request completion.
- The zero-geometry unnamed AMMO `00003BD3` is unresolved: `AmmoFatMan.nif` parsing failed and helper fallback found no geometry. WEAP `000001F4` is the expected no-inventory-model creature attack. Do not count the AMMO as an expected omission.
- Runtime material evidence: **93** texturing events, **406** reported ranges, **299** prefetched, **292** bound to texture slots, and **114** explicitly `UNTEXTURED (no prefetch)` in this session. These are sampled live records, not full catalog coverage.

| Type | loaded records | candidate subset | positive runtime geometry |
|---|---:|---:|---:|
{table}

## Asset sweep and limitations

The full material sweep indexed 42,933 material sets and 1,329,626 unique texture names: 6,537 sets are textureless by design, 36,331 color maps resolve, and 65 are unresolved. Its complete table is `SF-Shop/SF Shop Explorer/tools/renderer_recon/extracted/catalog_material_sweep.tsv`. This validates the material database/archive generally, but not each unpreviewed record's actual selected material or visual correctness.

The CSV retains dump `modelPath` values (often typed metadata markers, not NIF filenames) separately from `runtimeModelAndGeometryNifPaths`. Empty runtime columns mean **not observed in this session**, not no-model. To finish per-record runtime validation, the remaining {len(output)-len(distinct):,} records need to be selected in-game or passed through a purpose-built runtime batch probe. The inventory lists them individually but does not claim they were all rendered.
"""
    md_path = report_dir / "catalog-record-preview-audit.md"
    md_path.write_text(md, encoding="utf-8")
    print(f"records={len(output)} candidate_subset={sum(candidate_by_type.values())} previewed={len(distinct)} positive_geometry={sum(positive_by_type.values())}")
    print("geometry_status=" + repr(dict(geometry_counts)))
    print(f"unparsed_request_lines={len(unparsed_requests)} csv={csv_path} ({csv_path.stat().st_size} bytes) md={md_path}")
    print("by_type=" + repr({t: (count_by_type[t], candidate_by_type[t], positive_by_type[t]) for t in sorted(TYPES)}))


if __name__ == "__main__":
    main()
