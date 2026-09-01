# LVS harness

Everything here is checked in on purpose: these scripts are the regression
regime for the FASM decoder and the netlist comparison, and they took long
enough to get right that losing them to a cleared `/tmp` would hurt.  Their
*outputs* (baked netlists, miters, CNFs, Verilator object dirs) are large and
regenerable, so those belong in a scratch directory and are not tracked.

Paths default to a sibling `~/xc7-bitstream-tools` and `~/prjxray`, overridable
with `XC7_BITSTREAM_TOOLS_DIR`, `PRJXRAY_FUZZERS`, `TILEDUMP`, `DEVICE`.

## check_fuzzers.py -- decoder vs Vivado ground truth

Runs the tile-config decoder against Project X-Ray's fuzzer corpus.  Each
specimen pairs `params.csv` -- the configuration Vivado was *asked* to build,
one row per SLICE -- with the FASM read back out of the resulting bitstream, so
a disagreement is a decoder bug rather than an opinion.  14,640 cases at the
time of writing.

Three distinctions the output keeps separate, because conflating them wastes
hours:

* **MISMATCH** -- the decoder read a feature and got it wrong.
* **no site feature present** -- the select has no encoding in site config at
  all (`F7`/`F8`, and a subset of `OUTMUX.O5`).  Not decodable from a slice's
  own features; needs routing context.  Not a decoder failure.
* **0 slices decoded** -- the fuzzer's committed `design.fasm` predates the
  segbits that same fuzzer generates.  `design.bit` is still there; re-running
  `bit2fasm` with the current database unlocks it (017 goes from 0 to 46,092
  slice features that way).

## The comparison flow

    annotate_lvs.py     rename an extraction's cells/nets to the source
                        netlist's own names, from a placement dump
    expose_ffs.ys.in    yosys: proc, flatten, expose -dff -- makes every
                        flip-flop observable as a module port
    gen_miter_tb.py     Verilator testbench comparing every matched FF and
                        output bit each cycle; reports the FIRST divergence
    yosys_equiv.ys.in   equiv_make/equiv_simple between the two netlists
    cells_min.v         minimal FDxx/LUTx/IO models -- yosys's own cells_sim.v
                        is richer than a general SV frontend will parse
    pysat_solve.py      DIMACS in, SAT-competition answer out, so any pysat
                        solver can be used as `lvs_equiv --solver`

`.ys.in` files carry absolute paths for the design under test; substitute them
rather than editing in place.
