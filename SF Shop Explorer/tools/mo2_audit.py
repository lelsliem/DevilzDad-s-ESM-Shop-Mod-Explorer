#!/usr/bin/env python3
"""Pre-flight audit of an MO2 profile for the in-game previewer.

The previewer does not ask the engine for a model: `LiveModel::ResolveAsset`
reads loose files and BA2 name tables itself, mods first. That means every
enabled mod can change what the preview shows, and the places where the
previewer's lookup order disagrees with MO2's are exactly the places where
the preview and the game can show different assets.

This script walks the ENABLED mods of a profile and reports:

  * loose files and archive entries that more than one mod ships - and which
    mod wins under MO2's priority versus the previewer's current order;
  * a mod shipping `materials/materialsbeta.cdb` outside `creations/`, which
    would replace the base material database instead of merging with it;
  * Creation Kit databases and how close the profile is to the merge caps;
  * texture formats and sizes the previewer cannot decode;
  * how many base-game NIFs/materials/textures the profile overrides.

`--catalog` adds the item-level question: for every row of a form dump, does
any enabled mod replace the model, a material, or a material's albedo map?
That is the list of shop items whose preview will look different from
vanilla. Rows whose dump has no model path (ARMO/WEAP placeholders) can be
mapped from a live SFShopExplorer.log with `--log`, which records the NIFs
actually assembled for each preview request.

Run it before a full-mod test so a wrong preview can be told apart from a
wrong model:

    python mo2_audit.py                      # the profile MO2 last launched
    python mo2_audit.py --profile Test --json mo2-audit.json
    python mo2_audit.py --catalog "C:\\...\\SFDevilzDump-catalog.csv" \\
        --log "C:\\...\\SFShopExplorer.log" --catalog-out catalog-overrides.csv
"""
from __future__ import annotations

import argparse
import csv
import json
import os
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "ba2_audit"))
from ba2lib import Ba2Archive  # noqa: E402
from mo2_overlay import (  # noqa: E402
    Overlay,
    log_material_map,
    log_nif_map,
    mat_albedo,
    material_paths,
    nif_key,
    norm_key,
    selected_profile,
)

DEFAULT_LOG = Path(os.path.expandvars(
    r"%USERPROFILE%\Documents\My Games\Starfield\SFSE\Logs\SFShopExplorer.log"))

MODS_ROOT = Path(r"B:\MO2\Starfield\mods")
PROFILES = Path(r"B:\MO2\Starfield\profiles")
BASE_INDEX = HERE / "ba2_audit" / "extracted" / "ba2nameindex.tsv"

# What the previewer ever asks for as a loose file. Anything else in a mod is
# not our business and would make the walk minutes longer for no answer.
# Archives are parsed separately (root .ba2 only, as the plugin does).
ASSET_EXTS = {".nif", ".mat", ".dds", ".mesh", ".cdb"}

# LiveModel.cpp: kMaxModCdbs / kMaxModCdbBytes.
MAX_MOD_CDBS = 256
MAX_MOD_CDB_BYTES = 512 << 20

# LiveModel.cpp: MapTexFormat + DdsToImage's 4096 limit.
SUPPORTED_DXGI = {
    71, 72,   # BC1
    74, 75,   # BC2
    77, 78,   # BC3
    80,       # BC4
    83, 84,   # BC5
    97, 98, 99,   # BC7 (99 = the sRGB tag repacked mods use)
}


def profile_order(profile: str) -> tuple[list[str], list[str]]:
    """Enabled mods in modlist.txt order, then the disabled ones.

    MO2 stores priority implicitly in the line order, reversed: the LAST line
    is priority 0 and the FIRST line has the highest priority, i.e. wins every
    loose-file conflict. Both lists keep that order.
    """
    path = PROFILES / profile / "modlist.txt"
    enabled: list[str] = []
    disabled: list[str] = []
    with path.open("r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.rstrip("\r\n")
            if len(line) < 2:
                continue
            name = line[1:].strip()
            if name.endswith("_separator"):
                continue
            if line[0] == "+":
                enabled.append(name)
            elif line[0] in "-*":
                # '*' is a Creation entry (base game), '-' a disabled mod.
                disabled.append(name)
    return enabled, disabled


def walk_loose(mod_dir: Path) -> dict[str, int]:
    """Game-relative lower-case path -> file size, for asset extensions only."""
    out: dict[str, int] = {}
    root = str(mod_dir)
    for dirpath, dirnames, filenames in os.walk(mod_dir):
        # FOMOD installers and documentation never reach Data.
        dirnames[:] = [d for d in dirnames if d.lower() not in ("fomod", "docs", ".git")]
        rel_dir = os.path.relpath(dirpath, root)
        for name in filenames:
            ext = os.path.splitext(name)[1].lower()
            if ext not in ASSET_EXTS:
                continue
            rel = name.lower() if rel_dir == "." else (
                rel_dir.replace("\\", "/").lower() + "/" + name.lower())
            try:
                out[rel] = os.path.getsize(os.path.join(dirpath, name))
            except OSError:
                out[rel] = -1
    return out


def archive_names(ar: Ba2Archive) -> dict[str, int]:
    """Entry name -> decompressed size for a parsed BA2."""
    names: dict[str, int] = {}
    if not ar.valid:
        return names
    for name, ent in ar.entries.items():
        names[name] = ent.size
    for name, tf in ar.tex_files.items():
        names[name] = tf.width * tf.height
    return names


def read_base_index() -> dict[str, str]:
    out: dict[str, str] = {}
    if not BASE_INDEX.exists():
        return out
    with BASE_INDEX.open("r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            name, _, arc = line.rstrip("\n").rpartition("\t")
            if name and arc:
                out[name] = arc
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--profile", default="",
                    help="MO2 profile (default: the one MO2 last launched)")
    ap.add_argument("--mods-root", default=str(MODS_ROOT))
    ap.add_argument("--json", default="")
    ap.add_argument("--limit", type=int, default=15,
                    help="rows to print per conflict table")
    ap.add_argument("--catalog", default="",
                    help="form-dump CSV; report items any mod changes")
    ap.add_argument("--catalog-out", default="",
                    help="write the per-item comparison here (CSV)")
    ap.add_argument("--log", default=str(DEFAULT_LOG) if DEFAULT_LOG.exists() else "",
                    help="SFShopExplorer.log used to map form IDs to assembled NIFs")
    ap.add_argument("--data-dir", default=r"B:\SteamLibrary\steamapps\common\Starfield\Data")
    args = ap.parse_args()
    args.profile = args.profile or selected_profile() or "Test"

    mods_root = Path(args.mods_root)
    enabled, disabled = profile_order(args.profile)
    enabled_lc = {m.lower(): m for m in enabled}
    folder_by_lc = {p.name.lower(): p for p in mods_root.iterdir() if p.is_dir()}

    missing = [m for m in enabled if m.lower() not in folder_by_lc]
    unlisted = [p.name for lc, p in folder_by_lc.items()
                if lc not in enabled_lc and lc not in {d.lower() for d in disabled}]

    problems: list[str] = []
    report: dict = {
        "profile": args.profile,
        "enabled": len(enabled),
        "disabled": len(disabled),
        "missing_folders": missing,
        "unlisted_folders": unlisted,
        "loose_conflicts": [],
        "archive_conflicts": [],
        "base_cdb_shadow": [],
        "mod_cdbs": [],
        "bad_textures": [],
        "base_asset_overrides": {},
        "stats": {},
    }

    # ── Loose files ────────────────────────────────────────────────────
    loose: dict[str, list[tuple[str, int]]] = defaultdict(list)
    loose_bytes = 0
    loose_files = 0
    for name in enabled:
        mod_dir = folder_by_lc.get(name.lower())
        if mod_dir is None:
            continue
        for rel, size in walk_loose(mod_dir).items():
            loose[rel].append((name, size))
            loose_files += 1
            loose_bytes += max(size, 0)

    # The engine and the previewer both read the FIRST enabled mod in
    # modlist.txt order. Keep the alphabetical winner only as history: it is
    # what the preview used to pick before the 2026-10-23 precedence fix.
    mo2_rank = {name.lower(): i for i, name in enumerate(enabled)}
    alphabetical_rank = {name.lower(): i
                         for i, name in enumerate(sorted(enabled, key=str.lower))}

    for rel, owners in loose.items():
        if len(owners) < 2:
            continue
        mo2_win = min(owners, key=lambda o: mo2_rank.get(o[0].lower(), 1 << 30))
        alpha_win = min(owners, key=lambda o: alphabetical_rank.get(o[0].lower(), 1 << 30))
        sizes = {s for _, s in owners}
        report["loose_conflicts"].append({
            "path": rel,
            "owners": [{"mod": m, "size": s} for m, s in owners],
            "mo2_winner": mo2_win[0],
            "preview_winner": mo2_win[0],
            "alphabetical_winner": alpha_win[0],
            "differs": len(sizes) > 1,
        })

    # ── Archives ───────────────────────────────────────────────────────
    archives: list[dict] = []
    arch_names: dict[str, list[tuple[str, str]]] = defaultdict(list)
    mod_cdbs: list[dict] = []
    base_cdb_shadow: list[dict] = []
    bad_textures: list[dict] = []
    versions: Counter = Counter()
    for name in enabled:
        mod_dir = folder_by_lc.get(name.lower())
        if mod_dir is None:
            continue
        for entry in sorted(mod_dir.glob("*.ba2")):
            ar = Ba2Archive(entry)
            names = archive_names(ar)
            versions["v%d %s" % (ar.version, "DX10" if ar.is_tex else "GNRL")] += 1
            archives.append({
                "mod": name,
                "archive": entry.name,
                "version": ar.version,
                "entries": len(names),
                "bytes": entry.stat().st_size,
                "valid": ar.valid,
            })
            for ename, size in names.items():
                arch_names[ename].append((name, entry.name))
                low = ename.lower()
                if low.endswith("materialsbeta.cdb"):
                    row = {"mod": name, "archive": entry.name, "path": ename, "size": size}
                    if "/creations/" in low:
                        mod_cdbs.append(row)
                    else:
                        base_cdb_shadow.append(row)
                if low.endswith(".dds") and ar.valid:
                    tf = ar.tex_files.get(low)
                    if tf is not None and (
                            tf.dxgi not in SUPPORTED_DXGI or
                            tf.width > 4096 or tf.height > 4096):
                        bad_textures.append({
                            "mod": name, "archive": entry.name, "path": ename,
                            "dxgi": tf.dxgi, "w": tf.width, "h": tf.height,
                            "reason": ("format %d" % tf.dxgi
                                       if tf.dxgi not in SUPPORTED_DXGI
                                       else "size %dx%d" % (tf.width, tf.height)),
                        })

    for ename, owners in arch_names.items():
        if len(owners) < 2:
            continue
        mo2_win = min(owners, key=lambda o: mo2_rank.get(o[0].lower(), 1 << 30))
        alpha_win = min(owners, key=lambda o: alphabetical_rank.get(o[0].lower(), 1 << 30))
        report["archive_conflicts"].append({
            "path": ename,
            "owners": [{"mod": m, "archive": a} for m, a in owners],
            "mo2_winner": mo2_win[0],
            "preview_winner": mo2_win[0],
            "alphabetical_winner": alpha_win[0],
        })

    # Loose beats archives in the plugin and in MO2 alike, but a loose asset
    # that only exists in a mod archive elsewhere is worth knowing about.
    loose_over_archive = sum(1 for rel in loose if rel in arch_names)

    # Loose Creation Kit databases follow the plugin's single-level pattern.
    loose_mod_cdbs: list[dict] = []
    for name in enabled:
        mod_dir = folder_by_lc.get(name.lower())
        if mod_dir is None:
            continue
        creations = mod_dir / "materials" / "creations"
        if creations.is_dir():
            for sub in creations.iterdir():
                cdb = sub / "materialsbeta.cdb"
                if sub.is_dir() and cdb.is_file():
                    loose_mod_cdbs.append({"mod": name, "path": str(cdb),
                                           "size": cdb.stat().st_size})
    loose_base_cdb = [{"mod": n, "path": str(folder_by_lc[n.lower()] / rel)}
                      for rel in loose if rel == "materials/materialsbeta.cdb"
                      for n in [o[0] for o in loose[rel]]]

    cdb_rows = mod_cdbs + loose_mod_cdbs
    cdb_bytes = sum(r["size"] for r in cdb_rows)

    # ── Base-game overrides ────────────────────────────────────────────
    base = read_base_index()
    overrides: dict[str, set[str]] = defaultdict(set)
    for rel, owners in loose.items():
        if rel in base or rel.replace("/", "\\") in base:
            for m, _ in owners:
                overrides[m].add(rel)
    for ename, owners in arch_names.items():
        if ename in base:
            for m, _ in owners:
                overrides[m].add(ename)
    report["base_asset_overrides"] = {
        m: {"count": len(paths),
            "nif": sum(1 for p in paths if p.endswith(".nif")),
            "mat": sum(1 for p in paths if p.endswith(".mat")),
            "dds": sum(1 for p in paths if p.endswith(".dds"))}
        for m, paths in sorted(overrides.items(), key=lambda kv: -len(kv[1]))
    }

    report["mod_cdbs"] = cdb_rows
    report["base_cdb_shadow"] = base_cdb_shadow + loose_base_cdb
    report["bad_textures"] = bad_textures
    report["stats"] = {
        "mods_with_folder": sum(1 for m in enabled if m.lower() in folder_by_lc),
        "loose_assets": loose_files,
        "loose_bytes": loose_bytes,
        "archives": len(archives),
        "archive_invalid": [a for a in archives if not a["valid"]],
        "archive_entries": sum(a["entries"] for a in archives),
        "loose_over_archive": loose_over_archive,
        "mod_cdbs": len(cdb_rows),
        "mod_cdb_bytes": cdb_bytes,
        "bad_textures": len(bad_textures),
        "archive_versions": dict(versions),
    }

    # ── Verdict ────────────────────────────────────────────────────────
    loose_diff = [c for c in report["loose_conflicts"] if c["differs"]]
    # How many conflicts the old alphabetical walk would have settled
    # differently. Informational only since the preview follows modlist order.
    loose_prio = [c for c in loose_diff
                  if c["mo2_winner"].lower() != c["alphabetical_winner"].lower()]
    arch_prio = [c for c in report["archive_conflicts"]
                 if c["mo2_winner"].lower() != c["alphabetical_winner"].lower()]
    report["stats"]["conflicts_old_order_would_flip"] = {
        "loose": len(loose_prio), "archive": len(arch_prio)}
    if base_cdb_shadow:
        problems.append("%d mod file(s) would replace the base material database"
                        % len(base_cdb_shadow))
    if cdb_rows and (len(cdb_rows) > MAX_MOD_CDBS or cdb_bytes > MAX_MOD_CDB_BYTES):
        problems.append("Creation databases exceed the merge caps (%d/%d, %.1f/%.0f MiB)"
                        % (len(cdb_rows), MAX_MOD_CDBS, cdb_bytes / 2**20,
                           MAX_MOD_CDB_BYTES / 2**20))
    if bad_textures:
        problems.append("%d texture entries use a format or size the previewer "
                        "cannot decode" % len(bad_textures))
    if missing:
        problems.append("%d enabled mod(s) have no folder" % len(missing))
    report["problems"] = problems

    # ── Print ──────────────────────────────────────────────────────────
    s = report["stats"]
    print("profile '%s': %d enabled, %d disabled, %d with a folder on disk"
          % (args.profile, len(enabled), len(disabled), s["mods_with_folder"]))
    print("  loose assets indexed : %d (%.1f MiB)"
          % (s["loose_assets"], s["loose_bytes"] / 2**20))
    print("  mod archives         : %d, %d entries, %d invalid  %s"
          % (s["archives"], s["archive_entries"], len(s["archive_invalid"]),
             ", ".join("%s x%d" % (k, v) for k, v in sorted(versions.items()))))
    print("  mod databases        : %d (%.1f MiB) cap %d / %.0f MiB"
          % (s["mod_cdbs"], s["mod_cdb_bytes"] / 2**20, MAX_MOD_CDBS,
             MAX_MOD_CDB_BYTES / 2**20))
    print()

    print("Loose conflicts: %d (%d differ in size); %d flipped by the old "
          "alphabetical order"
          % (len(report["loose_conflicts"]), len(loose_diff), len(loose_prio)))
    for c in sorted(loose_diff, key=lambda c: (c["mo2_winner"] == c["alphabetical_winner"]))[:args.limit]:
        print("  %-70s game=%-38s old-preview=%s"
              % (c["path"][:70], c["mo2_winner"][:38], c["alphabetical_winner"][:38]))
    print()

    print("Archive conflicts: %d ; flipped by the old alphabetical order: %d"
          % (len(report["archive_conflicts"]), len(arch_prio)))
    for c in arch_prio[:args.limit]:
        print("  %-70s game=%-38s old-preview=%s"
              % (c["path"][:70], c["mo2_winner"][:38], c["alphabetical_winner"][:38]))
    print()

    print("Base material database shadowed by mods: %d" % len(base_cdb_shadow))
    for r in base_cdb_shadow[:args.limit]:
        print("  %s :: %s (%d bytes)" % (r.get("mod"), r["path"], r["size"]))
    print()

    print("Textures the previewer cannot decode: %d" % len(bad_textures))
    by_mod = Counter(r["mod"] for r in bad_textures)
    for mod, n in by_mod.most_common(args.limit):
        sample = next(r for r in bad_textures if r["mod"] == mod)
        print("  %-45s %5d  e.g. %s [%s]" % (mod[:45], n, sample["path"][:60],
                                              sample["reason"]))
    print()

    print("Base asset overrides (top mods):")
    for mod, row in list(report["base_asset_overrides"].items())[:args.limit]:
        print("  %-45s %5d  nif %4d mat %4d dds %6d"
              % (mod[:45], row["count"], row["nif"], row["mat"], row["dds"]))
    print()

    if problems:
        print("PROBLEMS")
        for p in problems:
            print("  - " + p)
    else:
        print("No structural problems found.")
    if unlisted:
        print("\nfolders not in the profile's modlist: %d (e.g. %s)"
              % (len(unlisted), ", ".join(unlisted[:5])))

    if args.json:
        Path(args.json).write_text(json.dumps(report, indent=2), encoding="utf-8")
        print("\njson -> %s" % args.json)

    if args.catalog:
        print()
        run_catalog(args, enabled, mods_root, args.data_dir)
    return 0


# ── Catalog comparison ────────────────────────────────────────────────
CATALOG_NIF_COLUMNS = ("runtimeModelAndGeometryNifPaths", "nifPaths", "nifs", "nif")


def form_id(value: str) -> str:
    value = (value or "").strip().upper().lstrip("0")
    try:
        return "%08X" % int(value, 16) if value else ""
    except ValueError:
        return ""


def row_nifs(row: dict[str, str], by_form: dict[str, set[str]]) -> list[str]:
    """Every model path a catalog row names, from the dump or the live log."""
    out: list[str] = []
    model = (row.get("modelPath") or "").strip()
    if model.lower().endswith(".nif"):
        out.append(model)
    for col in CATALOG_NIF_COLUMNS:
        value = (row.get(col) or "").strip()
        if value:
            out.extend(p.strip() for p in re.split(r"[|;]", value)
                       if p.strip().lower().endswith(".nif"))
    out.extend(by_form.get(form_id(row.get("formID", "")), ()))
    seen: set[str] = set()
    keys: list[str] = []
    for path in out:
        key = nif_key(path)
        if key and key not in seen:
            seen.add(key)
            keys.append(key)
    return keys


def run_catalog(args, enabled: list[str], mods_root: Path, data_dir: str) -> None:
    """Report every catalog item whose model/materials a mod replaces."""
    catalog = Path(args.catalog)
    if not catalog.is_file():
        print("catalog: %s not found" % catalog)
        return
    by_form = log_nif_map(args.log) if args.log and Path(args.log).is_file() else {}
    by_form_mats = (log_material_map(args.log)
                    if args.log and Path(args.log).is_file() else {})
    overlay = Overlay(args.profile, mods_root, data_dir, parse_archives=True)

    rows_out: list[dict] = []
    per_mod: Counter = Counter()
    changed_items: list[str] = []
    unmapped = 0
    untextured: list[tuple[str, str, str]] = []
    with catalog.open("r", encoding="utf-8-sig", errors="replace", newline="") as fh:
        for row in csv.DictReader(fh):
            nifs = row_nifs(row, by_form)
            logged_mats = by_form_mats.get(form_id(row.get("formID", "")), set())
            name = (row.get("name") or row.get("editorId") or "").strip()
            if not nifs and not logged_mats:
                unmapped += 1
                continue
            mods: set[str] = set()
            model_mods: set[str] = set()
            mod_mats: list[str] = []
            mod_tex: list[str] = []
            missing_tex: list[str] = []
            unreadable: list[str] = []
            mats_seen: set[str] = set(logged_mats)
            for nif in nifs:
                src = overlay.resolve(nif)
                if src.is_mod:
                    model_mods.add(src.mod)
                    mods.add(src.mod)
                if src.kind == "missing":
                    unreadable.append(nif)
                    continue
                # Read vanilla NIFs too: a mod may replace only the material
                # or the albedo map of an untouched base mesh, and that item
                # still looks different in the preview.
                blob = overlay.read(nif)
                if not blob:
                    unreadable.append(nif)
                    continue
                for mat in material_paths(blob):
                    mats_seen.add(norm_key(mat))
            for mkey in sorted(mats_seen):
                msrc = overlay.resolve(mkey)
                if not msrc.is_mod:
                    continue
                mods.add(msrc.mod)
                mod_mats.append(mkey)
                mblob = overlay.read(mkey)
                albedo = mat_albedo(mblob) if mblob else ""
                if not albedo:
                    continue
                tsrc = overlay.resolve(albedo)
                if tsrc.is_mod:
                    mods.add(tsrc.mod)
                    mod_tex.append(albedo)
                elif tsrc.kind == "missing":
                    missing_tex.append(albedo)
                    untextured.append((name, mkey, albedo))
            changed = bool(model_mods or mod_mats or mod_tex)
            if changed:
                changed_items.append(name or row.get("formID", ""))
                for mod in mods:
                    per_mod[mod] += 1
            rows_out.append({
                "formID": row.get("formID", ""),
                "plugin": row.get("plugin", ""),
                "type": row.get("type", ""),
                "name": name,
                "editorId": row.get("editorId", ""),
                "modelPath": row.get("modelPath", ""),
                "modelFiles": " | ".join(nifs),
                "modelSource": ("mod" if model_mods else
                                ("vanilla" if nifs else "unmapped")),
                "modelMods": " | ".join(sorted(model_mods)),
                "materials": len(mod_mats) + len(mod_tex),
                "modMaterials": " | ".join(mod_mats),
                "modTextures": " | ".join(mod_tex),
                "unreadableModels": " | ".join(unreadable),
                "missingAlbedo": " | ".join(missing_tex),
                "changed": "yes" if changed else "no",
            })

    mapped = len(rows_out)
    changed = sum(1 for r in rows_out if r["changed"] == "yes")
    log_forms = set(by_form) | set(by_form_mats)
    print("catalog '%s': %d row(s), %d mapped to a model/material (%d form(s) "
          "from the log), %d without one"
          % (catalog.name, mapped + unmapped, mapped, len(log_forms), unmapped))
    print("  items any mod changes: %d" % changed)
    for mod, count in per_mod.most_common(args.limit):
        print("    %-45s %4d item(s)" % (mod[:45], count))
    if changed_items:
        print("  e.g. " + "; ".join(changed_items[:min(8, len(changed_items))]))
    if untextured:
        print("  mod materials whose albedo resolves nowhere (grey in preview): %d"
              % len(untextured))
        for name, mat, tex in untextured[:args.limit]:
            print("    %-32s %s -> %s" % (name[:32], mat[:60], tex[:60]))
    elif mapped:
        print("  every mod material's albedo resolves.")

    if args.catalog_out:
        with Path(args.catalog_out).open("w", encoding="utf-8", newline="") as fh:
            writer = csv.DictWriter(fh, fieldnames=list(rows_out[0].keys()) if rows_out else
                                    ["formID", "changed"])
            writer.writeheader()
            writer.writerows(rows_out)
        print("  csv -> %s" % args.catalog_out)


if __name__ == "__main__":
    raise SystemExit(main())
