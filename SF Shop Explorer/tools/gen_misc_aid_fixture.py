"""Rebuild tools/fixtures/misc_aid_catalog.csv from an SFDevilzDump-catalog.csv.

The fixture keeps the Misc and Aid rows the shop can list (a player-visible
name and a model) plus the columns their classifiers read: display name,
editor ID, keywords and the model path. Keywords come back empty for MISC and
ALCH in the dump, which is exactly why the model path had to join the search
text - this fixture pins that.

    python tools/gen_misc_aid_fixture.py
    python tools/gen_misc_aid_fixture.py path/to/SFDevilzDump-catalog.csv
"""
from __future__ import annotations

import csv
import sys
from pathlib import Path

DEFAULT_CATALOG = Path.home() / "Documents" / "My Games" / "Starfield" / "SFSE" / "Logs" / "SFDevilzDump-catalog.csv"
FIXTURE = Path(__file__).resolve().parent / "fixtures" / "misc_aid_catalog.csv"

HEADER_NOTE = (
    "# MISC and ALCH rows from the test profile (SFDevilzDump-catalog.csv),",
    "# named and modelled only - the rows the shop can list. Columns are the",
    "# ones CategorySearchText plus the model half of ClassifyMiscSub /",
    "# ClassifyAidSub read. Regenerate with tools/gen_misc_aid_fixture.py when",
    "# the load order changes.",
)


def main() -> int:
    catalog = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_CATALOG
    if not catalog.is_file():
        print(f"catalog not found: {catalog}", file=sys.stderr)
        return 1
    with catalog.open(encoding="utf-8-sig", newline="") as handle:
        rows = [row for row in csv.DictReader(handle)
                if row["type"] in ("MISC", "ALCH")
                and row["name"].strip().lower() not in ("", "(unnamed)", "unnamed")
                and row["modelPath"].strip()]
    FIXTURE.parent.mkdir(parents=True, exist_ok=True)
    with FIXTURE.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        for line in HEADER_NOTE:
            writer.writerow([line])
        writer.writerow(["type", "name", "editorId", "keywords", "modelPath"])
        for row in rows:
            writer.writerow([row["type"], row["name"], row["editorId"], row["keywords"], row["modelPath"]])
    print(f"wrote {len(rows)} MISC/ALCH rows to {FIXTURE}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
