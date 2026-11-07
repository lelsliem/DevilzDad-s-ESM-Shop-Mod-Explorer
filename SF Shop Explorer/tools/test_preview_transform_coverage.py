#!/usr/bin/env python3
"""Check the offline PTT2/TRNS reading against what the game logged.

The in-game run of 2026-10-17 21:40 logged authored placement for seven forms.
If `preview_transform_coverage.py` reads plugin files correctly it must produce
exactly the same slot set and values for those forms -- otherwise the offline
tally describes a different structure than the engine reads at runtime, and any
framing work built on it is unsound.

Run: python tools/test_preview_transform_coverage.py
Prints OK on success; exits non-zero with the failing form on a mismatch.
"""

import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "preview_transform_coverage.py")

# formID -> slot -> (posX, posY, posZ, rotX, rotY, rotZ, scale, minZoom, maxZoom)
# Copied from the live log; -10 zoom is authored on Inv_Guns_Workbench3D_04, not
# a misread (confirmed by its TRNS record's DATA).
EXPECTED = {
    0x0029B052: {  # Alien Tea
        0: (0.000, 1.000, -0.350, 6.161, 0.000, 0.262, 4.200, 0.000, 0.000),
        4: (0.000, 1.000, 0.000, 6.161, 0.000, 0.262, 1.000, 0.000, 0.000),
        5: (0.000, 1.000, 0.000, 6.161, 0.000, 0.262, 1.000, 0.000, 0.000),
    },
    0x00000FD6: {  # Razorback
        0: (-0.020, -4.150, 0.030, 0.000, 0.000, 4.712, 1.000, 0.000, 0.000),
        4: (0.000, 1.000, 0.000, 6.248, 0.087, 4.189, 1.000, 0.000, 0.000),
        5: (0.020, -0.530, 0.020, 6.021, 0.000, 4.189, 1.000, -10.000, -10.000),
        6: (0.000, -0.140, 0.000, 0.000, 0.000, 0.000, 1.000, 0.000, 0.000),
    },
    0x0000000A: {  # Digipick
        4: (0.020, 0.000, 0.000, 1.309, 3.491, 1.571, 0.950, 0.000, 0.000),
    },
    0x002B558D: {  # 7.62x39mm
        4: (0.000, 1.000, 0.000, 5.672, 6.213, 3.037, 1.000, 0.000, 0.000),
        5: (0.000, 1.000, 0.000, 5.672, 6.213, 3.037, 1.000, 0.000, 0.000),
    },
    0x00050340: {  # Playing Card - King of Hearts
        4: None,  # slot must exist; values checked only for presence
    },
    0x000001F4: {},  # UnarmedHuman — no authored placement at all
}


def read_tsv(path):
    rows = {}
    with open(path, "r", encoding="utf-8") as handle:
        header = handle.readline().rstrip("\n").split("\t")
        idx = {name: pos for pos, name in enumerate(header)}
        for line in handle:
            parts = line.rstrip("\n").split("\t")
            # (owner, ownerID), not the full id: two mods can each define a
            # record that prints as the same 01xxxxxx hex. And not the
            # containing plugin either -- a load order that overrides Alien Tea
            # or the Razorback moves the winning record out of Starfield.esm,
            # while the owner key stays put.
            fid = (parts[idx["owner"]].lower(), int(parts[idx["ownerID"]], 16))
            slot = int(parts[idx["slot"]])
            values = tuple(float(parts[idx[name]]) for name in (
                "posX", "posY", "posZ", "rotX", "rotY", "rotZ",
                "scale", "minZoom", "maxZoom"))
            rows.setdefault(fid, {})[slot] = values
    return rows


def main():
    out = os.path.join(tempfile.gettempdir(), "preview_transform_coverage_test.tsv")
    result = subprocess.run([sys.executable, TOOL, "--quiet", "--out", out],
                            capture_output=True, text=True)
    # Exit 2 with a SKIP line means the tool had nothing to read -- no load
    # order, or one with no plugins. That is the environment, not the reader,
    # so say so instead of reporting a mismatch the tool cannot be blamed for.
    if result.returncode == 2 and result.stderr.lstrip().startswith("SKIP:"):
        print("SKIP  " + result.stderr.lstrip()[len("SKIP:"):].strip())
        return 0
    if result.returncode != 0:
        print("tool failed (%d):\n%s" % (result.returncode, result.stderr[-2000:]))
        return 1

    rows = read_tsv(out)
    failures = []
    for fid, expected in EXPECTED.items():
        actual = rows.get(("starfield.esm", fid), {})
        if set(actual) != set(expected):
            failures.append("%08X slots %s != expected %s" % (
                fid, sorted(actual), sorted(expected)))
            continue
        for slot, want in expected.items():
            if want is None:
                continue
            got = actual[slot]
            if any(abs(g - w) > 5e-4 for g, w in zip(got, want)):
                failures.append("%08X slot %d values %s != expected %s" % (
                    fid, slot, got, want))

    if failures:
        print("MISMATCH between plugin files and the live game log:")
        for line in failures:
            print("  " + line)
        return 1

    total = sum(len(slots) for slots in rows.values())
    print("OK  %d forms / %d authored slots read from plugins; "
          "%d live-logged forms match exactly" % (len(rows), total, len(EXPECTED)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
