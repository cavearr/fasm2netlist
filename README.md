# fasm2netlist

Reconstructs a Xilinx Series-7 bitstream's SLICE-fabric logic (LUT6_2 +
FDRE/FDSE/FDCE/FDPE) as a flat, anonymously-named Verilog netlist, from
**only**:

- the design's FASM (from a `.bit` via [Project X-Ray](https://github.com/f4pga/prjxray)'s
  `bit2fasm`, itself using nothing but the fixed prjxray tile/pip tables), and
- the fixed prjxray technology database for the target part
  (`tilegrid.json`, `tileconn.json`, `tile_type_*.json`, `ppips_*.db`) --
  properties of the silicon, not of any specific build.

This is an LVS-style ("layout versus schematic") extraction, not an export
of some other tool's run: it never reads a placement or routed-JSON dump.
Every LUT/FF cell gets an auto-generated name -- correspondence to a
reference netlist's own cell names is a separate, downstream matching
problem (structural/SOP-based signature matching), not something this tool
hands over for free.

## Status / scope

This is a first pass, ported from a Python prototype (`bit2gates.py`, part
of a separate `xc7-bitstream-tools` project) that was validated end-to-end
(direct simulation + structural equivalence checks) against a real VC707
Johnson-counter bitstream.

- **Covered:** SLICE LUT6/LUT5-pair (LUT6_2) + FDRE/FDSE/FDCE/FDPE
  (FFMUX default O6/O5/bypass; no CARRY4/XOR path).
- **Not yet ported:** top-level IO (IBUF/IBUFDS/OBUF/BUFG resolution via
  `.xdc` + `package_pins.csv`), CARRY4/distributed-RAM/SRL/DSP48/BRAM.

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
             --out design_gates.v [--module NAME]
```

- `PRJXRAY_DB` is the root of a Project X-Ray database checkout (e.g.
  `prjxray-db/`).
- `FAMILY` / `DEVICE` name the subdirectories under it, e.g.
  `--family virtex7 --device xc7vx485t` (the directory holding
  `tilegrid.json` for that exact part).

Example, against a VC707 Johnson counter:

```sh
fasm2netlist --fasm johnson.fasm --db /path/to/prjxray-db \
             --family virtex7 --device xc7vx485t --out johnson_gates.v
```

The output is a flat Verilog module instantiating `LUT6_2` and
`FDRE`/`FDSE`/`FDCE`/`FDPE` primitives (matching Yosys's own
`techlibs/xilinx/cells_sim.v` semantics), with genuinely-unrouted nets tied
to `1'b0` and no top-level ports (IO is not yet resolved -- see Scope
above).

## Tests

`tests/` has a small hand-written synthetic "device" (one tile type, one
SLICE-shaped site -- see `tests/fixtures/`) so the test suite doesn't need
the real, multi-gigabyte Project X-Ray database. It checks the tool builds
and runs end-to-end correctly, not correctness against real silicon --
that validation was done separately against a real design.
