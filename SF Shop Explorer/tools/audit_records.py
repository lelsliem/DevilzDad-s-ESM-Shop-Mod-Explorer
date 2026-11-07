#!/usr/bin/env python3
"""Audit a form dump for records the catalog will group as duplicates.

The shop collapses records that a player cannot tell apart (see
`hideDuplicateNames` in ShopExplorer.h / `BuildFormList` in ShopExplorer.cpp).
The rule lives in C++ and only runs in game, so this tool re-derives it from a
form dump and shows what the collapse will do before a launch:

  * the key is what a player sees - record type, folded display name, value and
    weight - never the FormID. Unnamed records are excluded: they all share one
    placeholder label, so grouping them would fold hundreds of unrelated
    records into one row.
  * a free record is not merchandise. Identically-labelled plot objects are
    different things (62 "Crew Member's Slate"s, 13 "Dying Words", seven "Lab
    Key"s), so grouping needs a price as well as a name and `--include-free`
    is the opt-in that shows what the rule deliberately leaves alone.
  * AMMO ignores weight. Starfield keeps a pre-ammo-weight record of a round
    beside the one that replaced it (`11mm Caseless` 00230679 weight 0.000 next
    to 002B5595 weight 0.004), and those are the same box of rounds. Every
    other type keeps weight, so a genuinely heavier variant stays its own row.
  * the surviving record is the earliest plugin's newest FormID: the master's
    current definition, never a later plugin's clone, and within one file the
    appended record rather than the one it replaced. A plugin's load-order rank
    is the top byte of the FormID, plus the middle 12 bits only for the indexed
    forms (0xFE light, 0xFD medium) - in a full plugin those bits are part of
    the record id.

Two kinds of group come out of that, and they mean different things:

  same-plugin    one file defines the name twice. Either the author duplicated a
                 record, or the game appended a replacement and left the old one
                 (the ammo families). "Not real" in the sense that only one of
                 them is the item anyone means.
  cross-plugin   a later plugin clones an earlier one's item (tailoredspaces'
                 SPP_TS_RazorbackTrophy beside Starfield.esm's Razorback). Both
                 records are real and spawnable; the clone usually exists for
                 display.

    python tools/audit_records.py                     # the seven catalog types
    python tools/audit_records.py --type WEAP AMMO     # just those
    python tools/audit_records.py --all-types          # every type in the dump
    python tools/audit_records.py --name razorback --show 50
    python tools/audit_records.py --out reports/duplicates.tsv

Exit code is 0 when the audit ran, 2 when it could not (no dump, no rows).
`--fail-over N` turns it into a gate: non-zero when more than N rows would be
collapsed.
"""
from __future__ import annotations

import argparse
import csv
import json
import sys
from dataclasses import dataclass, field
from pathlib import Path

# The record types the catalog browses (BrowseType in ShopExplorer.h). INGR is
# absent on purpose: Starfield's data carries none.
CATALOG_TYPES = ("WEAP", "ARMO", "AMMO", "MISC", "ALCH", "BOOK", "KEYM")
# The dump writes this for a record with no player-visible name; FormDisplayName
# returns an empty string for the same records in game.
UNNAMED = {"", "(unnamed)", "<unnamed>", "none", "null"}
DEFAULT_DUMP = "SFDevilzDump-all-forms.csv"


@dataclass
class Record:
    plugin: str
    form: int
    type: str
    name: str
    edid: str
    value: int
    weight: float
    group: tuple = field(default=(), compare=False)

    @property
    def fid(self) -> str:
        return f"{self.form:08X}"

    @property
    def weight_key(self) -> int:
        """Weight to the milligram, as an int: the plugin compares integers."""
        return round(self.weight * 1000)


def plugin_rank(form: int) -> int:
    """Load-order rank of the plugin that owns a FormID (ShopExplorer.cpp)."""
    top = form >> 24
    middle = (form >> 12) & 0x0FFF
    return ((top << 12) | middle) if top in (0xFE, 0xFD) else (top << 12)


def group_key(rec: Record, include_free: bool = False) -> tuple | None:
    """The duplicate identity, or None for a record the rule never groups.

    Type, name, value, and weight - except for ammo, whose legacy weightless
    record and its replacement are the same box of rounds. A record with no
    price is a plot object, so it stays listed however it is named.
    """
    if rec.value <= 0 and not include_free:
        return None
    weight = 0 if rec.type == "AMMO" else rec.weight_key
    return (rec.type, rec.name.casefold(), rec.value, weight)


def survivor(members: list[Record]) -> Record:
    """Earliest plugin, then the newest FormID inside it."""
    return min(members, key=lambda r: (plugin_rank(r.form), -r.form))


def load_dump(path: Path, types: set[str] | None) -> tuple[list[Record], int]:
    """Read the dump. Returns (named catalog records, count of unnamed rows)."""
    records: list[Record] = []
    unnamed = 0
    with path.open(encoding="cp1252", newline="", errors="replace") as fh:
        for row in csv.DictReader(fh):
            kind = (row.get("type") or "").strip().upper()
            if types is not None and kind not in types:
                continue
            name = (row.get("name") or "").strip()
            if name.casefold() in UNNAMED:
                unnamed += 1
                continue
            try:
                form = int((row.get("formID") or "0").strip(), 16)
            except ValueError:
                continue
            try:
                value = int(float((row.get("value") or "0").strip() or 0))
            except ValueError:
                value = 0
            try:
                weight = float((row.get("weight") or "0").strip() or 0)
            except ValueError:
                weight = 0.0
            records.append(Record(
                plugin=(row.get("plugin") or "").strip(),
                form=form,
                type=kind,
                name=name,
                edid=(row.get("editorId") or "").strip(),
                value=value,
                weight=weight,
            ))
    return records, unnamed


def find_dump(start: Path) -> Path | None:
    """The dump is written at the SF-Shop root, three levels above this tool."""
    for base in (start, *start.parents):
        candidate = base / DEFAULT_DUMP
        if candidate.is_file():
            return candidate
    return None


def build_groups(records: list[Record],
                 include_free: bool = False) -> dict[tuple, list[Record]]:
    groups: dict[tuple, list[Record]] = {}
    for rec in records:
        key = group_key(rec, include_free)
        if key is None:
            continue
        groups.setdefault(key, []).append(rec)
    return {key: members for key, members in groups.items() if len(members) > 1}


def free_groups(records: list[Record]) -> dict[tuple, list[Record]]:
    """What the price rule leaves alone, so the exclusion is visible, not silent."""
    return build_groups([r for r in records if r.value <= 0], include_free=True)


def is_cross_plugin(members: list[Record]) -> bool:
    return len({plugin_rank(m.form) for m in members}) > 1


def summarise(groups: dict[tuple, list[Record]]) -> dict:
    by_type: dict[str, dict[str, int]] = {}
    cross = 0
    largest = 0
    for key, members in groups.items():
        kind = key[0]
        entry = by_type.setdefault(kind, {"groups": 0, "surplus": 0})
        entry["groups"] += 1
        entry["surplus"] += len(members) - 1
        largest = max(largest, len(members))
        if is_cross_plugin(members):
            cross += 1
    return {
        "groups": len(groups),
        "surplus": sum(len(m) - 1 for m in groups.values()),
        "crossPlugin": cross,
        "samePlugin": len(groups) - cross,
        "largest": largest,
        "byType": dict(sorted(by_type.items())),
    }


def rows_for_tsv(groups: dict[tuple, list[Record]]) -> list[list[str]]:
    out = []
    for key, members in sorted(groups.items(), key=lambda kv: (-len(kv[1]), kv[0])):
        keep = survivor(members)
        for rec in sorted(members, key=lambda r: (plugin_rank(r.form), r.form)):
            out.append([
                key[0], rec.name, rec.fid, rec.plugin, rec.edid,
                str(rec.value), f"{rec.weight:.3f}",
                "keep" if rec is keep else "collapsed",
                "cross" if is_cross_plugin(members) else "same",
                str(len(members)),
            ])
    return out


def print_report(groups: dict[tuple, list[Record]], stats: dict, unnamed: int,
                 total: int, a) -> None:
    print(f"records audited: {total:,} named"
          + (f" ({unnamed:,} unnamed excluded: they share one placeholder label)" if unnamed else ""))
    if not groups:
        print("no duplicate groups: every named record is unique by type, name, value and weight")
        return
    print(f"duplicate groups: {stats['groups']:,}   rows the shop collapses: {stats['surplus']:,}"
          f"   ({stats['crossPlugin']:,} cross-plugin clones, {stats['samePlugin']:,} same-plugin"
          f"   largest group {stats['largest']})")
    print()
    print(f"{'type':<6}{'groups':>8}{'rows':>8}")
    for kind, entry in stats["byType"].items():
        print(f"{kind:<6}{entry['groups']:>8}{entry['surplus']:>8}")
    if a.quiet or a.show == 0:
        return
    shown = 0
    for key, members in sorted(groups.items(), key=lambda kv: (-len(kv[1]), kv[0])):
        if shown >= a.show:
            print(f"\n... {len(groups) - shown:,} more group(s); --show 0 lists all, --out writes a TSV")
            break
        keep = survivor(members)
        kind = "clone" if is_cross_plugin(members) else "same-file"
        print(f"\n{key[0]}  {members[0].name!r}  x{len(members)}  ({kind})")
        for rec in sorted(members, key=lambda r: (plugin_rank(r.form), r.form)):
            mark = "keep     " if rec is keep else "collapsed"
            print(f"  {mark} {rec.fid}  {rec.plugin:<28} {rec.value:>7} cr  {rec.weight:>6.3f}"
                  + (f"  {rec.edid}" if rec.edid else ""))
        shown += 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--forms", type=Path, default=None, help=f"form dump (default: {DEFAULT_DUMP})")
    ap.add_argument("--type", action="append", default=[], metavar="TYPE",
                    help="record type, repeatable (default: the seven catalog types)")
    ap.add_argument("--all-types", action="store_true", help="every type in the dump")
    ap.add_argument("--include-free", action="store_true",
                    help="also group records with no price (quest slates, keys): what the rule skips")
    ap.add_argument("--plugin", default=None, help="only groups with a member from a plugin matching this substring")
    ap.add_argument("--name", default=None, help="only groups whose name contains this substring")
    ap.add_argument("--show", type=int, default=10, help="groups to print (0 = none)")
    ap.add_argument("--out", type=Path, default=None, help="write one row per duplicate record (TSV)")
    ap.add_argument("--json", type=Path, default=None, help="write the summary as JSON")
    ap.add_argument("--fail-over", type=int, default=None,
                    help="exit 1 when more than this many rows would collapse")
    ap.add_argument("--quiet", action="store_true", help="summary only, no group listing")
    a = ap.parse_args()

    if a.forms is None:
        found = find_dump(Path(__file__).resolve().parent)
        if not found:
            print(f"no {DEFAULT_DUMP} found; pass --forms", file=sys.stderr)
            return 2
        a.forms = found
    if not a.forms.is_file():
        print(f"no such dump: {a.forms}", file=sys.stderr)
        return 2

    types = None if a.all_types else set(x.upper() for x in (a.type or CATALOG_TYPES))
    records, unnamed = load_dump(a.forms, types)
    if not records:
        print(f"no named records of type {sorted(types) if types else 'any'} in {a.forms}", file=sys.stderr)
        return 2

    groups = build_groups(records, a.include_free)
    free = free_groups(records)
    if a.name:
        needle = a.name.casefold()
        groups = {k: v for k, v in groups.items() if needle in v[0].name.casefold()}
    if a.plugin:
        needle = a.plugin.casefold()
        groups = {k: v for k, v in groups.items()
                  if any(needle in m.plugin.casefold() for m in v)}

    stats = summarise(groups)
    print_report(groups, stats, unnamed, len(records), a)
    if free and not a.include_free:
        print(f"\nleft alone (no price, so not merchandise): {len(free):,} group(s), "
              f"{sum(len(v) - 1 for v in free.values()):,} row(s) - --include-free lists them")

    if a.out:
        a.out.parent.mkdir(parents=True, exist_ok=True)
        with a.out.open("w", encoding="utf-8", newline="") as fh:
            fh.write("type\tname\tformID\tplugin\teditorId\tvalue\tweight\trole\tkind\tgroup\n")
            for row in rows_for_tsv(groups):
                fh.write("\t".join(row) + "\n")
        print(f"\nwrote {a.out}")
    if a.json:
        a.json.parent.mkdir(parents=True, exist_ok=True)
        payload = dict(stats, records=len(records), unnamed=unnamed,
                       dump=str(a.forms), types=sorted(types) if types else "all",
                       filtered=dict(name=a.name, plugin=a.plugin))
        a.json.write_text(json.dumps(payload, indent=2), encoding="utf-8")
        print(f"wrote {a.json}")
    if a.fail_over is not None and stats["surplus"] > a.fail_over:
        print(f"FAIL: {stats['surplus']} row(s) collapse, over the {a.fail_over} allowed",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
