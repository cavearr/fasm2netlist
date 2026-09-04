# fasm2netlist

Reconstructs a Xilinx Series-7 bitstream's fabric logic -- the SLICEs
(LUT6_2, flip-flops and latches, CARRY4, MUXF7/MUXF8, the shift registers
and the distributed RAMs), the DSP48E1s and the block RAMs -- plus top-level
IO (IBUF/IBUFDS/OBUF/BUFG) as a flat, anonymously-named Verilog netlist,
from **only**:

- the design's FASM (from a `.bit` via [Project X-Ray](https://github.com/f4pga/prjxray)'s
  `bit2fasm`, itself using nothing but the fixed prjxray tile/pip tables),
- the fixed prjxray technology database for the target part
  (`tilegrid.json`, `tileconn.json`, `tile_type_*.json`, `ppips_*.db`) --
  properties of the silicon, not of any specific build, and
- (optionally, to also resolve top-level IO) the design's own `.xdc` plus
  prjxray's `package_pins.csv` for the exact part -- see Usage below.

This is an LVS-style ("layout versus schematic") extraction, not an export
of some other tool's run: it never reads a placement or routed-JSON dump.
Every cell gets an auto-generated name -- correspondence to a
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

Started as a port of a Python prototype (`bit2gates.py`, part of a separate
`xc7-bitstream-tools` project) that was validated end-to-end (direct
simulation + structural equivalence checks) against a real VC707
Johnson-counter bitstream, and has since grown the rest of the fabric.

**Covered:**

| Family | Primitives |
| --- | --- |
| SLICE logic | `LUT6_2`, `FDRE`/`FDSE`/`FDCE`/`FDPE`, `LDCE`/`LDPE` |
| Carry | `CARRY4` (chained across slices through the COUT/CIN wires) |
| Wide muxes | `MUXF7`, `MUXF8` |
| Shift registers | `SRLC32E`, `SRL16E`, `SRLC16E`, including the D->C->B->A cascade |
| Distributed RAM | `RAM32X1S`/`RAM32X1D`, `RAM64X1S`/`RAM64X1D`, `RAM128X1S`/`RAM128X1D`, `RAM256X1S`, `RAM32M`, `RAM64M` |
| DSP | `DSP48E1` (all site pins wired, all attributes prjxray has bits for decoded) |
| Block RAM | `RAMB18E1`, `RAMB36E1` (config plus the full `INIT_xx`/`INITP_xx` contents) |
| Top-level IO | `IBUF`, `IBUFDS`, `OBUF`, `BUFG` (with `--xdc` plus `--part`/`--bit`) |

Two things are worth being explicit about, since they are decisions rather
than omissions:

- **A hard block is extracted as a configured instance, not as gates.** For a
  `DSP48E1` or a `RAMB18E1` the bitstream carries the block's configuration;
  the behaviour that configuration selects lives inside the block. So those
  emitters wire every site pin from the fixed routing tables and decode every
  attribute prjxray has solved bits for -- and where the bits genuinely do not
  determine something, they say so on stderr rather than inventing a value.
- **The clock tree (GCLK/HROW/HCLK) is still not reconstructed.** See the
  clock tie-off note under Usage.

**Not modelled:** clock/IO-logic beyond the above (ISERDES/OSERDES, MMCM/PLL,
IDELAY), the FIFO modes of the block RAMs (a `FIFO_MODE` site is emitted as a
plain `RAMB18E1`, with a warning), and gigabit transceivers.

## Proving the extraction: the tile model and `lvs_equiv`

Extracting a netlist and proving it correct are different jobs, and this
repository does both. `tileverilog` builds a second, independent netlist
straight from the FASM -- every net named after the silicon it was read out of,
so nothing can match by coincidence -- and `lvs_equiv` proves that netlist
equal to the synthesis the bitstream was built from, register by register, with
a SAT miter per cone.

What the checker covers, and how:

| | treated as |
| --- | --- |
| `LUT6_2`, `FDRE`/`FDSE`/`FDCE`/`FDPE` | modelled |
| `CARRY4` | modelled, chained across slices |
| `MUXF7`/`MUXF8` | modelled, built between columns as nextpnr packs them |
| Distributed RAM | **cut** at its boundary |
| Block RAM (`RAMB18E1`/`RAMB36E1`) | **cut** at its boundary |
| I/O buffers, I/O logic, `IDELAYE2` | modelled as connections |
| `MMCME2_ADV` | only `LOCKED` is cut; see below |

A memory is *cut*, not modelled: its data outputs become free variables and
every one of its inputs becomes an obligation, so what gets proved is that both
sides wired the same nets to the same pins. That is what makes a design with
memory in it provable at all without modelling the array, and the placement is
what pairs one side's memory with the other's. The contents are **not** compared
here -- `fasm2netlist` reads them out of the bitstream and
`tests/rtl/build_and_check.py` compares them, which is where a Boolean
equivalence check cannot help.

Three things are assumed rather than reconstructed, and each is deliberate:

- **The clock tree.** With one BUFG in the design there is only one thing a
  clock pin can be, so they are joined to it. `--routed-clock` turns that off.
  Clock pins are therefore not obligations, for the same reason a flip-flop's
  `C` is never compared.
- **The MMCM's configuration.** A frequency is not a Boolean fact. Its `LOCKED`
  pin *is*, and is cut like any other hard-block output -- which matters
  because a reset synchroniser waits on it.
- **Memory contents**, as above.

Not covered by the checker: `SRL`s, latches (`LDCE`/`LDPE`) and `DSP48E1`,
all of which the extractor does decode.

`lvs_equiv --explain` reports, for a failing cone, which named variables each
side reads. Most failures turn out to be correspondence errors rather than
logic errors, and the two look identical until you can see the supports.

### What it has been run against

`scripts/verify_examples.sh` in the parent repository builds each example from
source and proves it. The largest is a LiteX SoC -- a SERV CPU with its BIOS in
block RAM, its register file in distributed RAM, carry chains throughout and an
MMCM:

    2820 proved, 0 differ    (688 of 688 registers matched, 56 memory boundaries)

Which yosys built a design changes what that proof is asking, so the parent
repository pins yosys as a submodule and refuses to run the sweep with a
different one.

### How the trickier decodes are pinned down

Everything below is read off the fixed prjxray database and its own fuzzers,
which describe the silicon rather than any particular build:

- **Which RAM a SLICEM is.** The three per-column bits (`RAM`, `SRL`,
  `SMALL`) together with `WA7USED`/`WA8USED` and the DI1 mux settings name
  exactly one primitive -- see `decodeDram()` in `src/cells_slice.cpp`.
- **Shift-register contents.** A LUT in SRL mode ties its A1 input high, so
  the 64-bit LUT INIT holds each of the 32 real bits twice; the extractor
  checks that duplication actually holds before trusting it. The two 16-deep
  registers that can share one LUT split it by A6: the x6LUT half (read with
  A6 high) holds the upper 16 bits and the x5LUT half the lower 16.
- **18Kb vs 36Kb block RAM.** This has no configuration bit of its own --
  prjxray's `RAMB36.*` tags are all "this bit is clear" patterns, so a real
  RAMB36 emits none of them. What does distinguish the two is the routing: a
  36Kb block reaches the fabric through the tile's `RAMBFIFO36E1` site pins,
  a pair of 18Kb blocks through their own. The mode is read off which site
  the tile's pips touch, and the two halves' control and address nets are
  then cross-checked against each other.
- **Site pin names that differ from the primitive's ports.** prjxray types
  the lower 18Kb site `FIFO18E1`, so its pins carry FIFO names. Each pin's
  *wire*, though, is named `BRAM_<kind>_<canonical pin>` in all three sites
  (and `DSP_<n>_<pin>` in a DSP tile), so the canonical name -- and, for the
  DSP, which of the two sites FASM's `DSP_0`/`DSP_1` means -- comes out of
  the database rather than a hand-written translation table.
- **Inverted-sense attributes.** A prjxray tag spelled with a leading `Z`
  stores the complement, so an absent tag reads as 1 and a present one as 0.
  That is how `IS_*_INVERTED`, the DSP register-enable attributes and the
  BRAM `INIT`/`SRVAL` values are recovered.

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

The output is a flat Verilog module instantiating the primitives listed
under Status above (matching Yosys's own
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

`tests/` has a small hand-written synthetic "device" (SLICE-, DSP- and
BRAM-shaped sites -- see `tests/fixtures/`, regenerated by
`tests/fixtures/make_fixture_db.py`) so the test suite doesn't need the real,
multi-gigabyte Project X-Ray database. The smoke test drives one instance of
every cell family through it and checks the decoded INIT/attribute values,
not correctness against real silicon.

`tests/rtl/` goes the other way round: instead of starting from a FASM
someone else produced, it starts from Verilog. Five small designs -- one per
cell family, under `tests/rtl/designs/` -- are put through yosys and
nextpnr-himbaechel to produce a bitstream, and the extraction is then graded
against nextpnr's own placement dump from that same run. So the bits under
test were never seen before the run, and the grading is relational rather
than against recorded constants: for the families where a placed BEL and an
extracted cell correspond one to one (`CARRY4`, the flip-flops, `DSP48E1`,
the block RAMs) it is an equality; the LUT-ish BELs have no such
correspondence, so those are checked by site recovery. It also requires the
extraction to emit no warnings, and requires each design to still produce the
primitive it was written for -- so a synthesis change that quietly stops
inferring, say, a DSP shows up as a failure rather than a silent pass.

| design | exercises |
| --- | --- |
| `carry.v` | a 32-bit accumulator: four `CARRY4`s deep, so `COUT`->`CIN` has to be followed from slice to slice |
| `srl.v` | `SRLC32E` and `SRL16E`: contents recovered from the doubled-up LUT INIT, and a 16-deep column split into halves |
| `lutram.v` | `RAM32M`: a 32x2 asynchronous-read memory across a SLICEM's four LUTs |
| `dsp.v` | `DSP48E1`: a registered 18x18 multiply, site pins plus the complement-stored attributes |
| `bram.v` | `RAMB18E1`: 1K x 12 synchronous-read, so config, port widths and `INIT_xx` contents all have to come back |

It skips (rather than fails) when yosys, a built `nextpnr-himbaechel` or the
Project X-Ray database is absent, none of which is vendored here.

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
