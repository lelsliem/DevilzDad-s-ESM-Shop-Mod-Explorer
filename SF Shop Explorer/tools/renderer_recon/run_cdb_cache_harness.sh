#!/usr/bin/env bash
# Extract the persisted-CDB-index block out of src/LiveModel.cpp and build the
# round-trip harness against it. The block is NOT copied into the harness: it
# is sliced out at run time, so the harness tests the shipped bytes and cannot
# drift from the source.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
cpp="$root/src/LiveModel.cpp"
out="$here/cdb_cache_block.inc"

start=$(grep -n "persisted CDB index (6r-10)" "$cpp" | head -1 | cut -d: -f1)
if [ -z "$start" ]; then
  echo "FAIL: marker 'persisted CDB index (6r-10)' not found in $cpp" >&2
  exit 1
fi
# Block runs from the marker line to the closing brace of
# SaveCdbIndexCacheIfCurrent, which is the first line that is exactly a tab
# and '}' after that function's signature.
sig=$(awk -v s="$start" 'NR>s && /void SaveCdbIndexCacheIfCurrent\(\)/ {print NR; exit}' "$cpp")
if [ -z "$sig" ]; then
  echo "FAIL: SaveCdbIndexCacheIfCurrent not found after line $start" >&2
  exit 1
fi
end=$(awk -v s="$sig" 'NR>s && /^\t\}$/ {print NR; exit}' "$cpp")
if [ -z "$end" ]; then
  echo "FAIL: end of SaveCdbIndexCacheIfCurrent not found" >&2
  exit 1
fi

awk -v s="$((start - 1))" -v e="$end" 'NR>s && NR<=e' "$cpp" > "$out"
echo "extracted lines $((start))-$end of LiveModel.cpp -> $(wc -l < "$out") lines"

exe="$here/cdb_cache_harness.exe"
cd "$here"
rm -f "$exe"
# Spaces in the compiler path, so the launch goes through cmd.exe and a .bat
# rather than through a quoted argument list.
if ! cmd.exe //c "$(cygpath -w "$here/build_cache_harness.bat")"; then
  echo "FAIL: harness did not compile" >&2
  exit 1
fi
if [ ! -f "$exe" ]; then
  echo "FAIL: harness executable not produced" >&2
  exit 1
fi
"$exe"