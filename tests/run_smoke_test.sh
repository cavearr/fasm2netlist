#!/usr/bin/env bash
# Minimal end-to-end smoke test: run fasm2netlist against a small,
# hand-written synthetic "device" (one tile type, one SLICE-shaped site) so
# this repo can prove it builds *and runs* without needing the real,
# multi-gigabyte Project X-Ray database. Not a re-validation of the whole
# algorithm -- that was done against a real design (a VC707 Johnson counter)
# in the xc7-bitstream-tools project this was ported from; this just checks
# nothing broke in extraction (missing file, wrong path, etc).
set -euo pipefail

EXE="$1"
FIXTURES_DIR="$2"
OUT="$3"

"$EXE" \
  --fasm "$FIXTURES_DIR/design.fasm" \
  --db "$FIXTURES_DIR/db" \
  --family testfam \
  --device device0 \
  --out "$OUT" \
  --module smoke_test

grep -q "LUT6_2 #(.INIT(64'hffffffff00000000))" "$OUT" || {
  echo "FAIL: expected LUT6_2 INIT not found in $OUT"
  cat "$OUT"
  exit 1
}
grep -q "FDRE #(.INIT(1'b0))" "$OUT" || {
  echo "FAIL: expected FDRE not found in $OUT"
  cat "$OUT"
  exit 1
}
echo "smoke test OK"
