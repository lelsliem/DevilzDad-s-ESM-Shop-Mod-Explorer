"""Regression-test the plugin's BA2 assumptions without launching the game.

Examples
--------
  python ba2_audit.py "C:\\...\\Data\\Starfield - Materials.ba2"
  python ba2_audit.py --dir "C:\\...\\Data" --glob "*Textures.ba2" --limit 40
  python ba2_audit.py --dir "C:\\...\\Data" --sample 3      # read-check a few files

Exit code is 0 when every archive parses and every probed file decodes,
1 when anything fails, so it can gate a build.
"""

from __future__ import annotations

import argparse
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ba2lib import Ba2Archive, map_tex_format, surface_bytes  # noqa: E402


def audit(path: Path, kind: str, limit: int, sample: int, seed: int, quiet: bool) -> tuple[int, int]:
    ar = Ba2Archive(path)
    failures = 0
    checks = 0
    if not ar.valid:
        print(f"[FAIL] {path.name}: archive did not parse")
        return 1, 0

    if not quiet:
        print(ar.describe())

    # ---- entry-table sanity -------------------------------------------
    # The bug from 6r-5j: a stored size larger than the bytes available
    # before the name table is only legal when it is the DECOMPRESSED
    # length. Flag any entry that is neither an exact on-disk fit nor
    # decompressible, since that combination means the reader guessed.
    if kind in ("all", "gnrl") and ar.entries:
        suspicious = 0
        for name, ent in list(ar.entries.items()):
            span = ar.data_end - ent.offset
            if ent.size <= span:
                continue  # size fits the remaining bulk: plain compressed length
            if ent.on_disk and ent.on_disk <= span:
                continue  # decompressed length, real stream length known
            suspicious += 1
            if suspicious <= 5:
                print(
                    f"  [WARN] {name}: size={ent.size} exceeds bulk span {span} "
                    f"and on-disk hint {ent.on_disk} does not fit either"
                )
        if suspicious:
            print(f"  {suspicious} entr{'y' if suspicious == 1 else 'ies'} with no usable stream length")

    # ---- read-back ----------------------------------------------------
    if sample <= 0:
        return failures, checks

    if ar.entries:
        pool = sorted(ar.entries)
        picks = pool[: min(sample, len(pool))] if sample < len(pool) else pool
        random.seed(seed)
        if sample < len(pool):
            picks = random.sample(pool, sample)
        print(f"  probing {len(picks)} GNRL file(s)...")
        for name in picks:
            checks += 1
            blob, why = ar.read_entry(name)
            if blob is None:
                failures += 1
                print(f"  [FAIL] {name}: {why}")

    if ar.tex_files:
        pool = sorted(ar.tex_files)
        random.seed(seed)
        picks = random.sample(pool, min(sample, len(pool)))
        print(f"  probing {len(picks)} DX10 texture(s)...")
        for name in picks:
            checks += 1
            tf = ar.tex_files[name]
            blob, why = ar.read_texture(name)
            if blob is None:
                failures += 1
                print(f"  [FAIL] {name} ({tf.width}x{tf.height} dxgi={tf.dxgi}): {why}")
                continue
            # The plugin's own acceptance test: the chosen chunk must hold
            # at least pitch*rows bytes for the declared format/size.
            fmt = map_tex_format(tf.dxgi)
            if fmt == 0:
                print(f"  [SKIP] {name}: dxgi {tf.dxgi} unsupported by MapTexFormat")
                continue
            need = surface_bytes(tf)
            if len(blob) < need:
                failures += 1
                print(
                    f"  [FAIL] {name}: decoded {len(blob)} < {need} "
                    f"({tf.width}x{tf.height} fmt {fmt})"
                )

    return failures, checks


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("archive", nargs="?", help="single .ba2 to audit")
    ap.add_argument("--dir", help="directory to scan")
    ap.add_argument("--glob", default="*.ba2", help="glob for --dir (default *.ba2)")
    ap.add_argument("--kind", choices=["all", "gnrl", "tex"], default="all")
    ap.add_argument("--sample", type=int, default=0, help="read-check N files per archive")
    ap.add_argument("--seed", type=int, default=1234)
    ap.add_argument("--limit", type=int, default=0, help="max archives to audit")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    targets: list[Path] = []
    if args.archive:
        targets.append(Path(args.archive))
    if args.dir:
        found = sorted(p for p in Path(args.dir).glob(args.glob) if p.is_file())
        targets.extend(found[: args.limit] if args.limit else found)
    if not targets:
        ap.error("give an archive path or --dir")

    total_fail = total_check = 0
    bad_archives = []
    for path in targets:
        f, c = audit(path, args.kind, args.limit, args.sample, args.seed, args.quiet)
        total_fail += f
        total_check += c
        if f:
            bad_archives.append(path.name)

    print()
    print(f"archives: {len(targets)}   checks: {total_check}   failures: {total_fail}")
    if bad_archives:
        print("failing archives: " + ", ".join(bad_archives))
    return 1 if total_fail else 0


if __name__ == "__main__":
    raise SystemExit(main())
