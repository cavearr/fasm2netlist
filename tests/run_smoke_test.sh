#!/usr/bin/env bash
# Minimal end-to-end smoke test: run fasm2netlist against a small,
# hand-written synthetic "device" (see tests/fixtures/make_fixture_db.py) so
# this repo can prove it builds *and runs* without needing the real,
# multi-gigabyte Project X-Ray database. Not a re-validation of the whole
# algorithm -- that was done against real designs (a VC707 Johnson counter,
# and the CARRY4/SRL/RAM/DSP/BRAM designs the later families were developed
# against) in the xc7-bitstream-tools project this was ported from; this
# checks that every cell family still comes out of the extractor, with the
# decoded values the fixture's FASM asks for.
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

fail=0
want() {
  if ! grep -qF "$1" "$OUT"; then
    echo "FAIL: expected to find in $OUT: $1"
    fail=1
  fi
}

# LUT and flip-flop: the original case, unchanged.
want "LUT6_2 #(.INIT(64'hffffffff00000000))"
want "FDRE #(.INIT(1'b0))"

# CARRY4: DI[0]/DI[1] come from those columns' O5 (CARRY4.ACY0/BCY0 are set)
# while DI[2]/DI[3] come in on the C/D X pins, and PRECYINIT.C0 starts a new
# chain rather than continuing one.
want "CARRY4 \\carry4_TESTCLBM_X0Y1_SLICE_X0Y1 "
want ".CI(1'b0), .CYINIT(1'b0), .DI({n_TESTCLBM_X0Y1__S0_DX, n_TESTCLBM_X0Y1__S0_CX, w5_TESTCLBM_X0Y1_SLICE_X0Y1_B, w5_TESTCLBM_X0Y1_SLICE_X0Y1_A})"

# Shift registers: the 32-bit INIT is recovered from the doubled-up LUT INIT,
# the 16-deep pair splits it upper/lower half, and the D column's cascade
# output feeds the C column's data input.
want "SRLC32E #(.INIT(32'hdeadbeef))"
want "SRLC32E #(.INIT(32'h0f0f1234))"
want "SRL16E #(.INIT(16'haaaa))"
want "SRL16E #(.INIT(16'h5555))"
want ".Q31(wmc31_TESTCLBM_X0Y1_SLICE_X1Y1_D)"
want ".D(wmc31_TESTCLBM_X0Y1_SLICE_X1Y1_D)"

# Distributed RAM: a 64-deep single-port RAM keeps the LUT INIT verbatim.
want "RAM64X1S #(.INIT(64'h0123456789abcdef))"

# Wide muxes: BOUTMUX.F8 alone implies the F8 and both F7s beneath it.
want "MUXF8 \\muxf8_TESTCLBM_X0Y1_SLICE_X3Y1"
want "MUXF7 \\muxf7a_TESTCLBM_X0Y1_SLICE_X3Y1"
want "MUXF7 \\muxf7b_TESTCLBM_X0Y1_SLICE_X3Y1"

# DSP48E1: register-count and inversion attributes are stored complemented,
# so a present "Z" tag reads as 0 and an absent one as 1.
want "DSP48E1 #(.A_INPUT(\"DIRECT\"), .B_INPUT(\"DIRECT\"), .AREG(0), .ACASCREG(0), .BREG(0), .BCASCREG(0), .ADREG(0),"
want ".USE_DPORT(\"TRUE\")"
want ".MASK(48'h3ffffffffffc)"
want ".IS_OPMODE_INVERTED(7'h00)"
want ".IS_CLK_INVERTED(1'b0)"

# BRAM: the tile with no routing on its 36Kb site is two RAMB18E1s, the one
# with routing there is a single RAMB36E1 whose contents are the two halves'
# rows interleaved (Y0's INIT_01 = 1001..., Y1's = 1100..., giving
# 0xe1e1... across the 36Kb block's INIT_02/INIT_03).
want "RAMB18E1 #("
want ".READ_WIDTH_A(9), .READ_WIDTH_B(1), .WRITE_WIDTH_A(9)"
want ".INIT_00(256'h6666666666666666666666666666666666666666666666666666666666666666)"
want "RAMB36E1 #("
want ".READ_WIDTH_A(9)"
want ".INIT_02(256'he1e1e1e1"
want ".INIT_03(256'he1e1e1e1"

if [ "$fail" -ne 0 ]; then
  cat "$OUT"
  exit 1
fi
echo "smoke test OK"
