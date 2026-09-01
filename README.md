# fasm2netlist

Reconstructs a Xilinx Series-7 bitstream's SLICE-fabric logic (LUT6_2 +
FDRE/FDSE/FDCE/FDPE) plus top-level IO (IBUF/IBUFDS/OBUF/BUFG) as a flat,
anonymously-named Verilog netlist, from **only**:

- the design's FASM (from a `.bit` via [Project X-Ray](https://github.com/f4pga/prjxray)'s
  `bit2fasm`, itself using nothing but the fixed prjxray tile/pip tables),
- the fixed prjxray technology database for the target part
  (`tilegrid.json`, `tileconn.json`, `tile_type_*.json`, `ppips_*.db`) --
  properties of the silicon, not of any specific build, and
- (optionally, to also resolve top-level IO) the design's own `.xdc` plus
  prjxray's `package_pins.csv` for the exact part -- see Usage below.

This is an LVS-style ("layout versus schematic") extraction, not an export
of some other tool's run: it never reads a placement or routed-JSON dump.
Every LUT/FF cell gets an auto-generated name -- correspondence to a
reference netlist's own cell names is a separate, downstream matching
problem (structural/SOP-based signature matching), not something this tool
hands over for free. `tests/lvs/` builds exactly that downstream match (see
Tests below) three ways: name-matching against real placement ground truth,
a from-scratch Z3 equivalence proof over each side's LUTs converted to sum-
of-products form, and -- scaling past what per-cone SOP expansion alone
can carry, and without a placement oracle at all -- a structural/SAT
matcher that pairs cells by next-state cone topology and confirms or
falsifies each candidate pairing with a SAT miter.

## Status / scope

This is a first pass, ported from a Python prototype (`bit2gates.py`, part
of a separate `xc7-bitstream-tools` project) that was validated end-to-end
(direct simulation + structural equivalence checks) against a real VC707
Johnson-counter bitstream.

- **Covered:** SLICE LUT6/LUT5-pair (LUT6_2) + FDRE/FDSE/FDCE/FDPE
  (FFMUX default O6/O5/bypass; no CARRY4/XOR path); top-level IO
  (IBUF/IBUFDS/OBUF/BUFG, resolved via `.xdc` + `package_pins.csv` when
  `--xdc`/`--part` are given).
- **Not yet ported:** CARRY4/distributed-RAM/SRL/DSP48/BRAM.

## Building

Requires CMake >= 3.16 and a C++20 compiler. No external library
dependencies -- FASM tokenizing and JSON parsing are both self-contained
(see the scope note at the top of `src/main.cpp` for why, and the trade-off
against using Project X-Ray's own ANTLR-grammar FASM parser).

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure   # runs a small synthetic smoke test
```

## Usage

```sh
fasm2netlist --fasm design.fasm --db PRJXRAY_DB --family FAMILY --device DEVICE \
             --out design_gates.v [--module NAME] \
             [--xdc design.xdc (--part PART | --bit design.bit)]
```

- `PRJXRAY_DB` is the root of a Project X-Ray database checkout (e.g.
  `prjxray-db/`).
- `FAMILY` / `DEVICE` name the subdirectories under it, e.g.
  `--family virtex7 --device xc7vx485t` (the directory holding
  `tilegrid.json` for that exact part).
- `--xdc` together with `--part` or `--bit` (optional) additionally resolve
  top-level IO: `--xdc` is the design's own constraints file (only its
  `set_property PACKAGE_PIN ... [get_ports ...]` lines are read -- no other
  constraint), and `--part`/`--bit` name the exact chip+package(+speed
  grade) whose `package_pins.csv` lives under `PRJXRAY_DB/FAMILY/`:
  - `--part PART`, e.g. `xc7vx485tffg1761-2` (the speed grade suffix can be
    omitted; it's resolved by globbing, since pin/site layout doesn't vary
    by speed grade), or
  - `--bit design.bit`, if you have the actual bitstream and don't already
    know its exact part -- the part is a property of the bitstream itself
    (its header's first string field, byte-for-byte, e.g.
    `xc7vx485tffg1761-2`), so this reads it straight out rather than
    requiring it out-of-band.

  Without `--xdc` (and one of `--part`/`--bit`) the module has no ports, as
  before.

Example, against a VC707 Johnson counter:

```sh
fasm2netlist --fasm johnson.fasm --db /path/to/prjxray-db \
             --family virtex7 --device xc7vx485t \
             --xdc top.xdc --part xc7vx485tffg1761-2 \
             --out johnson_gates.v
```

The output is a flat Verilog module instantiating `LUT6_2`,
`FDRE`/`FDSE`/`FDCE`/`FDPE`, and (when `--xdc` plus `--part`/`--bit` are
given) `IBUF`/`IBUFDS`/`OBUF`/`BUFG` primitives (matching Yosys's own
`techlibs/xilinx/cells_sim.v` semantics), with genuinely-unrouted nets tied
to `1'b0`. IO identity comes purely from FASM site features
(`IBUF_HP_BANK_GLUE`/`IBUFDS_BANK_GLUE`/a `DRIVE.*` feature, and
`BUFGCTRL.*.IN_USE`) at sites located via the `.xdc` + `package_pins.csv`
-- never from a placement/routed dump. The clock tree itself (GCLK/HROW/
HCLK) is not reconstructed -- every SLICE clock net is tied straight to the
top-level clock port instead, bypassing the IBUFDS/BUFG/mesh routing
entirely, since treating it as ordinary point-to-point routing would
silently short a BUFG's input to its own output (see the clock-tie-off
comment in `src/main.cpp`).

## Tests

`tests/` has a small hand-written synthetic "device" (one tile type, one
SLICE-shaped site -- see `tests/fixtures/`) so the test suite doesn't need
the real, multi-gigabyte Project X-Ray database. It checks the tool builds
and runs end-to-end correctly, not correctness against real silicon.

All three of the following are real-silicon checks against a sibling
`xc7-bitstream-tools` checkout (`~/xc7-bitstream-tools` by default, override
with `XC7_BITSTREAM_TOOLS_DIR` or `--xc7-tools-dir`) and are skipped (not
failed) via CTest's `SKIP_RETURN_CODE` when that external, un-vendored
checkout (or, for the second and third, the `z3-solver` package) isn't
present:

- `tests/lvs/test_johnson_lvs.py` runs fasm2netlist against the actual
  VC707 Johnson-counter FASM and a real Project X-Ray database, and
  cross-checks the extraction against that project's own `nextpnr`
  placement dump for the same design -- an LVS-style comparison against
  independent ground truth: exact LUT/FF counts, and, since fasm2netlist
  names every cell purely from its physical tile/site/column, that every
  placed register and LUT is recoverable by name.

- `tests/lvs/prove_z3_sop_equiv.py` runs fasm2netlist with `--xdc`/`--part`
  and proves, with the Z3 SMT solver, that its reconstruction is
  functionally equivalent to the example's own gold Yosys synthesis
  (`johnson.json`). Both sides' LUTs -- generic `LUTk` cells on the gold
  side, `LUT6_2` on fasm2netlist's -- are expanded from their INIT truth
  tables into literal sum-of-products (SOP) Z3 expressions (one AND-clause
  per minterm), so both netlists are converted to the same canonical form
  before Z3 ever sees them; register identity across the two reuses the
  same tile/site/column name-matching as the test above. For each of the
  36 registers (and the 8 `led` outputs), it builds the full next-state
  function and asks Z3 to prove the two sides' XOR is UNSAT -- true for
  every input/state assignment, not just ones a simulation run happened to
  exercise. This is a from-scratch equivalence proof independent of (and
  complementary to) `xc7-bitstream-tools`'s own Yosys-`equiv_make`-based
  `check_equiv.py`.

- `tests/lvs/match_and_prove_sat.py` solves the actual "downstream matching
  problem" the first paragraph above describes, with **no placement.json
  oracle at all** (unlike the previous two: it never reads it as input,
  only uses it afterward to grade its own result against ground truth). Two
  problems this addresses that plain per-cone SOP expansion can't scale
  past on its own: rebuilding shared sub-cones from scratch for every
  register (fixed here with one memoized Z3-expression cache per matching
  round, not one per register), and needing register correspondence as a
  given (fixed by discovering it). The algorithm: (1) free, SAT-free
  anchors by tracing top-level output ports back through pure passthroughs
  to a bare FF; (2) "readiness" rounds -- once every OTHER register a
  register's cone depends on is matched, group same-signature (cone
  primary-inputs + #distinct-register-refs -- deliberately not cone LUT
  count, which isn't portable between the two sides' different
  decompositions of the same logic) candidates and SAT-confirm (miter
  UNSAT) or falsify (SAT) each pair; (3) when rounds stall on a closed
  loop/chain with no resolvable base case (e.g. a plain shift register,
  where every stage's cone is structurally identical -- topology alone
  can't tell them apart), seed a provisional match from any globally
  unique signature and let step 2's cascade resolve the rest, one real SAT
  confirmation at a time; (4) re-verify each seed for real once its own
  dependencies resolve. On the Johnson counter this matches all 36
  registers with zero placement input, agreeing with ground truth 36/36.
