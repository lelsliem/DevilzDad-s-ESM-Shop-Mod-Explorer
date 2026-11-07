#!/usr/bin/env bash
# Extract the BC4-mean block out of src/OfflineModel.cpp and build the harness
# against it. The block is NOT copied into the harness: it is sliced out at run
# time, so the harness tests the shipped bytes and cannot drift from the source.
#
#   bash tools/renderer_recon/run_bc4_mean_harness.sh
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
cpp="$root/src/OfflineModel.cpp"
out="$here/bc4_mean_block.inc"

start=$(grep -n "float Bc4Mean(const std::vector<std::uint8_t>&" "$cpp" | head -1 | cut -d: -f1)
if [ -z "$start" ]; then
  echo "FAIL: Bc4Mean not found in $cpp" >&2
  exit 1
fi
# The block runs to the closing brace of CompanionMapKey, the function declared
# immediately after it.
sig=$(awk -v s="$start" 'NR>s && /std::string CompanionMapKey\(/ {print NR; exit}' "$cpp")
if [ -z "$sig" ]; then
  echo "FAIL: CompanionMapKey not found after line $start" >&2
  exit 1
fi
end=$(awk -v s="$sig" 'NR>s && /^\t\t\}$/ {print NR; exit}' "$cpp")
if [ -z "$end" ]; then
  echo "FAIL: end of CompanionMapKey not found" >&2
  exit 1
fi

# The sliced block sits inside OfflineModel's anonymous namespace, indented two
# tabs; strip one level so it compiles at file scope in the harness.
awk -v s="$((start - 1))" -v e="$end" 'NR>s && NR<=e' "$cpp" | sed 's/^\t//' > "$out"
echo "extracted lines $start-$end of OfflineModel.cpp -> $(wc -l < "$out") lines"

exe="$here/bc4_mean_harness.exe"
rm -f "$exe"
if ! cmd.exe //c "$(cygpath -w "$here/build_bc4_mean_harness.bat")"; then
  echo "FAIL: harness did not compile" >&2
  exit 1
fi
if [ ! -f "$exe" ]; then
  echo "FAIL: harness executable not produced" >&2
  exit 1
fi
"$exe"
