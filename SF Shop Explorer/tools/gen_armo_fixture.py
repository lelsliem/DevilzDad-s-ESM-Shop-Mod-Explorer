"""Rebuild tools/fixtures/armo_catalog.csv from an SFDevilzDump-all-forms.csv.

The fixture keeps only the three columns the live armor classifier reads
(display name, editor ID, joined keyword editor IDs) so the offline tests can
check the actor-skin rule without the 15 MB dump or a game launch.

    python tools/gen_armo_fixture.py "C:/Users/me/Documents/My Games/Starfield/SFSE/Logs/SFDevilzDump-all-forms.csv"
"""
from __future__ import annotations

import csv
import sys
from pathlib import Path

DEFAULT_DUMP = Path.home() / "Documents" / "My Games" / "Starfield" / "SFSE" / "Logs" / "SFDevilzDump-all-forms.csv"
FIXTURE = Path(__file__).resolve().parent / "fixtures" / "armo_catalog.csv"

HEADER_NOTE = (
    "# ARMO rows from the 75-plugin test profile (SFDevilzDump-all-forms.csv).",
    "# Only the columns the catalog classifier reads are kept: display name,",
    "# editor ID and the joined keyword editor IDs. Regenerate with",
    "# tools/gen_armo_fixture.py when the load order changes.",
)


def main() -> int:
    dump = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_DUMP
    if not dump.is_file():
        print(f"dump not found: {dump}", file=sys.stderr)
        return 1
    with dump.open(encoding="utf-8-sig", newline="") as handle:
        rows = [row for row in csv.DictReader(handle) if row["type"] == "ARMO"]
    FIXTURE.parent.mkdir(parents=True, exist_ok=True)
    with FIXTURE.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        for line in HEADER_NOTE:
            writer.writerow([line])
        writer.writerow(["name", "editorId", "keywords"])
        for row in rows:
            writer.writerow([row["name"], row["editorId"], row["keywords"]])
    print(f"wrote {len(rows)} ARMO rows to {FIXTURE}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
