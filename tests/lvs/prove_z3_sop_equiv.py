#!/usr/bin/env python3
"""Prove, with Z3, that fasm2netlist's from-the-bitstream reconstruction of
the VC707 Johnson counter is functionally equivalent to the design's own
gold synthesis -- by converting every LUT on both sides to the same
sum-of-products (SOP) form and composing full per-register next-state
Boolean functions over it, then asking Z3 to prove each pair's XOR is
unsatisfiable. This is a from-scratch combinational miter built directly on
each side's own truth tables, independent of (and complementary to) the
Yosys-based equiv_make/equiv_induct approach xc7-bitstream-tools's own
check_equiv.py uses.

Two netlists, two representations of the same primitives, one SOP form:
  - "gold": xc7-bitstream-tools's examples/vc707-johnson/johnson.json, the
    Yosys `synth_xilinx` output nextpnr placed/routed for real (generic
    LUT2/LUT4/LUT5/LUT6 cells -- Xilinx unisim convention
    O = INIT[{I(k-1),...,I1,I0}]).
  - "gate": fasm2netlist's own reconstruction of the SAME bitstream, run
    fresh by this script with --xdc/--part (see the "enhance with the .xdc
    file" work this follows) so it now also has IBUF/IBUFDS/OBUF/BUFG and
    real top-level ports -- LUT6_2 cells (same INIT convention, O6 over
    INIT[63:0]/I0..I5, O5 over INIT[31:0]/I0..I4).

Both sides' LUT truth tables are expanded into literal minterm sum-of-
products Z3 BoolRefs (see lut_sop()) -- not handed to Z3 as opaque
truth-table lookups -- so the two netlists are, in a real sense, converted
to the same canonical SOP form before Z3 ever sees them.

Register identity is the same tile/site/column key test_johnson_lvs.py's
name-matching already established: fasm2netlist names every FF cell
"ff_<tile>_<site>_<col>[5]" directly from the physical column it
reconstructed, and xc7-bitstream-tools's own johnson_placement.json (from
the SAME nextpnr run that produced johnson.json) gives, for every gold FF
cell, the exact tile/site/bel it was placed at -- so the identical name is
recomputable on the gold side too. Every one of the 36 registers gets its
own persistent Z3 Bool state symbol under that shared name, so a D-cone
referencing another register's Q resolves to literally the same symbol on
both sides.

For each of the 36 matched registers, this builds the full next-state
function (SR/CE/D mux, matching techlibs/xilinx/cells_sim.v's FDRE/FDSE/
FDCE/FDPE semantics) on both sides and asks Z3 to prove
`next_gold != next_gate` is UNSAT -- i.e. the two functions agree for every
assignment of primary inputs and register state, not just the specific
inputs a simulation run would have exercised. The 8 "led" output cones get
the same treatment as a bonus.

Skips (exit 77) if the sibling xc7-bitstream-tools checkout isn't present,
or if the z3-solver package isn't installed.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

SKIP = 77

FF_BEL_RE = re.compile(r'^([ABCD])(5?)FF$')

OUTPUT_PINS = {
    'LUT6_2': {'O6', 'O5'},
    'FDRE': {'Q'}, 'FDSE': {'Q'}, 'FDCE': {'Q'}, 'FDPE': {'Q'},
    'IBUF': {'O'}, 'IBUFDS': {'O'}, 'BUFG': {'O'}, 'OBUF': {'O'},
}


def skip(msg):
    print('SKIP: %s' % msg)
    sys.exit(SKIP)


def vname(s):
    return re.sub(r'[^A-Za-z0-9_]', '_', s)


def expected_ff_name(tile, site, col, is5):
    return 'ff_%s_%s_%s%s' % (vname(tile), site, col, '5' if is5 else '')


# ---------------------------------------------------------------------
# shared SOP construction -- both sides' LUTs funnel through this
# ---------------------------------------------------------------------

def lut_sop(z3mod, invars, init_int):
    """Sum-of-products Z3 BoolRef for a k-input LUT: one AND-clause per
    minterm whose INIT bit is 1, matching the Xilinx unisim convention
    O = INIT[{I(k-1),...,I1,I0}]."""
    n = len(invars)
    terms = []
    for m in range(1 << n):
        if (init_int >> m) & 1:
            lits = [invars[b] if (m >> b) & 1 else z3mod.Not(invars[b]) for b in range(n)]
            terms.append(z3mod.And(*lits) if lits else z3mod.BoolVal(True))
    return z3mod.Or(*terms) if terms else z3mod.BoolVal(False)


# ---------------------------------------------------------------------
# gold side: xc7-bitstream-tools's own Yosys-synthesized johnson.json
# ---------------------------------------------------------------------

class GoldNetlist:
    def __init__(self, json_path, placement, module='top'):
        d = json.load(open(json_path))
        self.mod = d['modules'][module]
        self.cells = self.mod['cells']
        self.ports = self.mod['ports']
        self.placement = placement

        self.driver = {}  # net id -> (cellname, pin)
        for cname, c in self.cells.items():
            for pin, direction in c.get('port_directions', {}).items():
                if direction not in ('output', 'inout'):
                    continue
                for bit in c['connections'].get(pin, []):
                    if isinstance(bit, int):
                        self.driver[bit] = (cname, pin)

        self.input_label = {}  # net id -> top-level input port label
        self.output_bits = {}  # port name -> [net id, ...]
        for pname, p in self.ports.items():
            bits = p['bits']
            if p['direction'] == 'input':
                for i, b in enumerate(bits):
                    if isinstance(b, int):
                        self.input_label[b] = pname if len(bits) == 1 else '%s[%d]' % (pname, i)
            elif p['direction'] == 'output':
                self.output_bits[pname] = bits

    def ff_state_name(self, cellname):
        info = self.placement.get(cellname)
        if info is None or info['type'] != 'SLICE_FFX':
            raise ValueError('gold cell %r is not a placed SLICE_FFX' % cellname)
        m = FF_BEL_RE.match(info['bel'])
        if not m:
            raise ValueError('unrecognised FF bel %r for %r' % (info['bel'], cellname))
        return expected_ff_name(info['tile'], info['site'], m.group(1), bool(m.group(2)))

    def eval_pin(self, z3mod, cell, pin, memo, visiting):
        conns = cell['connections'].get(pin)
        if not conns:
            return z3mod.BoolVal(False)
        v = conns[0]
        if isinstance(v, str):
            return z3mod.BoolVal(v == '1')
        return self.eval_net(z3mod, v, memo, visiting)

    def eval_net(self, z3mod, net_id, memo, visiting):
        if net_id in memo:
            return memo[net_id]
        if net_id in self.input_label:
            expr = z3mod.Bool('in_' + self.input_label[net_id])
            memo[net_id] = expr
            return expr
        if net_id in visiting:
            raise RuntimeError('gold: combinational loop at net %r' % net_id)
        driver = self.driver.get(net_id)
        if driver is None:
            memo[net_id] = z3mod.BoolVal(False)  # undriven -> tie to 0, matches fasm2netlist's own policy
            return memo[net_id]
        visiting.add(net_id)
        cname, pin = driver
        cell = self.cells[cname]
        ctype = cell['type']
        if re.match(r'^LUT\d+$', ctype):
            k = int(ctype[3:])
            invars = [self.eval_pin(z3mod, cell, 'I%d' % i, memo, visiting) for i in range(k)]
            init_int = int(cell['parameters']['INIT'], 2)
            expr = lut_sop(z3mod, invars, init_int)
        elif ctype in ('FDRE', 'FDSE', 'FDCE', 'FDPE'):
            expr = z3mod.Bool('st_' + self.ff_state_name(cname))
        elif ctype in ('IBUF', 'IBUFDS', 'BUFG', 'OBUF'):
            expr = self.eval_pin(z3mod, cell, 'I', memo, visiting)
        else:
            raise ValueError('gold: unhandled cell type %r (%r)' % (ctype, cname))
        visiting.discard(net_id)
        memo[net_id] = expr
        return expr

    def ff_next(self, z3mod, cellname, memo, visiting):
        cell = self.cells[cellname]
        ctype = cell['type']
        D = self.eval_pin(z3mod, cell, 'D', memo, visiting)
        CE = self.eval_pin(z3mod, cell, 'CE', memo, visiting)
        self_sym = z3mod.Bool('st_' + self.ff_state_name(cellname))
        hold_or_load = z3mod.If(CE, D, self_sym)
        if ctype == 'FDRE':
            return z3mod.If(self.eval_pin(z3mod, cell, 'R', memo, visiting), z3mod.BoolVal(False), hold_or_load)
        if ctype == 'FDSE':
            return z3mod.If(self.eval_pin(z3mod, cell, 'S', memo, visiting), z3mod.BoolVal(True), hold_or_load)
        if ctype == 'FDCE':
            return z3mod.If(self.eval_pin(z3mod, cell, 'CLR', memo, visiting), z3mod.BoolVal(False), hold_or_load)
        if ctype == 'FDPE':
            return z3mod.If(self.eval_pin(z3mod, cell, 'PRE', memo, visiting), z3mod.BoolVal(True), hold_or_load)
        raise ValueError('gold: not a FF type: %r' % ctype)


# ---------------------------------------------------------------------
# gate side: fasm2netlist's own generated Verilog, parsed back
# ---------------------------------------------------------------------

class GateNetlist:
    CELL_RE = re.compile(r'^\s*(\w+)\s*(?:#\(\.INIT\(([^)]*)\)\))?\s*\\(\S+)\s*\((.*)\);\s*$')
    ASSIGN_RE = re.compile(r"^\s*assign\s+(\S+)\s*=\s*(\S+?)\s*;\s*$")

    def __init__(self, v_path):
        src = open(v_path).read()
        m = re.search(r'module\s+\w+\s*\(([^)]*)\)\s*;', src)
        header = m.group(1) if m else ''
        self.input_ports = set()
        for decl in [d.strip() for d in header.split(',') if d.strip()]:
            dm = re.match(r'(input|output)\s*(?:\[(\d+):0\])?\s*(\w+)', decl)
            if not dm:
                continue
            direction, hi, name = dm.groups()
            if direction != 'input':
                continue
            if hi is not None:
                for i in range(int(hi) + 1):
                    self.input_ports.add('%s[%d]' % (name, i))
            else:
                self.input_ports.add(name)

        self.driver = {}      # net name -> (celltype, cellname, pin)
        self.cell_conns = {}  # cellname -> {'type', 'init', 'conns': {pin: net}}
        self.alias = {}       # net name -> rhs net-or-const (from `assign`)

        for line in src.splitlines():
            am = self.ASSIGN_RE.match(line)
            if am:
                self.alias[am.group(1)] = am.group(2)
                continue
            cm = self.CELL_RE.match(line)
            if not cm:
                continue
            ctype, init, cname, portlist = cm.groups()
            if ctype not in OUTPUT_PINS:
                continue
            conns = dict(re.findall(r'\.(\w+)\(([^()]*)\)', portlist))
            self.cell_conns[cname] = {'type': ctype, 'init': init, 'conns': conns}
            in_nets = {n for p, n in conns.items() if p not in OUTPUT_PINS[ctype]}
            for pin in OUTPUT_PINS[ctype]:
                net = conns.get(pin)
                if not net:
                    continue
                if net in in_nets:
                    # self-loop alias (e.g. BUFG's I/O collapsed onto the
                    # same net by the union-find's GCLK-mesh join, see
                    # main.cpp's clock-tie-off comment) -- never register a
                    # cell as its own driver, or eval() would loop forever;
                    # whatever real driver already claimed this net stands.
                    continue
                self.driver[net] = (ctype, cname, pin)

    def pin_val(self, z3mod, cname, pin, memo, visiting):
        net = self.cell_conns[cname]['conns'].get(pin)
        if net is None:
            return z3mod.BoolVal(False)
        return self.eval_net(z3mod, net, memo, visiting)

    def eval_net(self, z3mod, net, memo, visiting):
        if net in memo:
            return memo[net]
        if net == "1'b0":
            return z3mod.BoolVal(False)
        if net == "1'b1":
            return z3mod.BoolVal(True)
        if net in self.input_ports:
            expr = z3mod.Bool('in_' + net)
            memo[net] = expr
            return expr
        if net in visiting:
            raise RuntimeError('gate: combinational loop at net %r' % net)
        if net in self.alias:
            visiting.add(net)
            expr = self.eval_net(z3mod, self.alias[net], memo, visiting)
            visiting.discard(net)
            memo[net] = expr
            return expr
        driver = self.driver.get(net)
        if driver is None:
            memo[net] = z3mod.BoolVal(False)  # genuinely unrouted -> tied to 0 by fasm2netlist itself
            return memo[net]
        visiting.add(net)
        ctype, cname, pin = driver
        cell = self.cell_conns[cname]
        if ctype == 'LUT6_2':
            init_int = int(cell['init'].split("'h", 1)[1], 16)
            invars5 = [self.pin_val(z3mod, cname, 'I%d' % i, memo, visiting) for i in range(5)]
            if pin == 'O6':
                invars6 = invars5 + [self.pin_val(z3mod, cname, 'I5', memo, visiting)]
                expr = lut_sop(z3mod, invars6, init_int & ((1 << 64) - 1))
            else:  # O5
                expr = lut_sop(z3mod, invars5, init_int & ((1 << 32) - 1))
        elif ctype in ('FDRE', 'FDSE', 'FDCE', 'FDPE'):
            expr = z3mod.Bool('st_' + cname)  # fasm2netlist already names FF cells canonically
        elif ctype in ('IBUF', 'IBUFDS', 'BUFG', 'OBUF'):
            expr = self.pin_val(z3mod, cname, 'I', memo, visiting)
        else:
            raise ValueError('gate: unhandled driver cell type %r' % ctype)
        visiting.discard(net)
        memo[net] = expr
        return expr

    def ff_next(self, z3mod, cellname, memo, visiting):
        cell = self.cell_conns[cellname]
        ctype = cell['type']
        D = self.pin_val(z3mod, cellname, 'D', memo, visiting)
        CE = self.pin_val(z3mod, cellname, 'CE', memo, visiting)
        self_sym = z3mod.Bool('st_' + cellname)
        hold_or_load = z3mod.If(CE, D, self_sym)
        if ctype == 'FDRE':
            return z3mod.If(self.pin_val(z3mod, cellname, 'R', memo, visiting), z3mod.BoolVal(False), hold_or_load)
        if ctype == 'FDSE':
            return z3mod.If(self.pin_val(z3mod, cellname, 'S', memo, visiting), z3mod.BoolVal(True), hold_or_load)
        if ctype == 'FDCE':
            return z3mod.If(self.pin_val(z3mod, cellname, 'CLR', memo, visiting), z3mod.BoolVal(False), hold_or_load)
        if ctype == 'FDPE':
            return z3mod.If(self.pin_val(z3mod, cellname, 'PRE', memo, visiting), z3mod.BoolVal(True), hold_or_load)
        raise ValueError('gate: not a FF type: %r' % ctype)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--exe', required=True, help='path to the built fasm2netlist binary')
    ap.add_argument('--xc7-tools-dir',
                     default=os.environ.get('XC7_BITSTREAM_TOOLS_DIR',
                                             os.path.expanduser('~/xc7-bitstream-tools')))
    ap.add_argument('--family', default='virtex7')
    ap.add_argument('--device', default='xc7vx485t')
    ap.add_argument('--part', default='xc7vx485tffg1761-2')
    args = ap.parse_args()

    try:
        import z3
    except ImportError:
        skip('z3-solver not installed (pip install z3-solver)')

    xc7 = args.xc7_tools_dir
    db = os.path.join(xc7, '.deps', 'prjxray-db')
    example_dir = os.path.join(xc7, 'examples', 'vc707-johnson')
    fasm_path = os.path.join(example_dir, 'johnson.fasm')
    xdc_path = os.path.join(example_dir, 'top.xdc')
    placement_path = os.path.join(example_dir, 'johnson_placement.json')
    gold_json_path = os.path.join(example_dir, 'johnson.json')

    for path, what in ((db, 'prjxray-db checkout'), (fasm_path, 'johnson.fasm'), (xdc_path, 'top.xdc'),
                        (placement_path, 'johnson_placement.json'), (gold_json_path, 'johnson.json')):
        if not os.path.exists(path):
            skip('%s not found at %s -- xc7-bitstream-tools is an external, un-vendored checkout; '
                 'run `make vc707-johnson PRJXRAY_DB=...` there first' % (what, path))

    placement = json.load(open(placement_path))

    with tempfile.TemporaryDirectory() as tmp:
        out_v = os.path.join(tmp, 'gate.v')
        proc = subprocess.run(
            [args.exe, '--fasm', fasm_path, '--db', db, '--family', args.family, '--device', args.device,
             '--xdc', xdc_path, '--part', args.part, '--out', out_v, '--module', 'gate'],
            capture_output=True, text=True)
        print(proc.stdout, end='')
        print(proc.stderr, end='', file=sys.stderr)
        assert proc.returncode == 0, 'fasm2netlist exited %d' % proc.returncode
        gate = GateNetlist(out_v)

    gold = GoldNetlist(gold_json_path, placement)

    ff_cells = sorted(name for name, info in placement.items() if info['type'] == 'SLICE_FFX')
    assert ff_cells, 'ground-truth placement has no SLICE_FFX entries -- stale or unexpected file'

    solver = z3.Solver()
    mismatches = []
    proven = 0
    for cellname in ff_cells:
        ff_name = gold.ff_state_name(cellname)
        if ff_name not in gate.cell_conns:
            mismatches.append((ff_name, 'not found in gate netlist'))
            continue
        next_gold = gold.ff_next(z3, cellname, {}, set())
        next_gate = gate.ff_next(z3, ff_name, {}, set())
        solver.push()
        solver.add(next_gold != next_gate)
        result = solver.check()
        solver.pop()
        if result != z3.unsat:
            mismatches.append((ff_name, 'Z3 found a counterexample: %s -> %s' % (result, solver.model())))
        else:
            proven += 1

    led_proven = 0
    for i, net_id in enumerate(gold.output_bits.get('led', [])):
        label = 'led[%d]' % i
        gold_expr = gold.eval_net(z3, net_id, {}, set())
        if label not in gate.driver and label not in gate.alias:
            mismatches.append((label, 'not found in gate netlist'))
            continue
        gate_expr = gate.eval_net(z3, label, {}, set())
        solver.push()
        solver.add(gold_expr != gate_expr)
        result = solver.check()
        solver.pop()
        if result != z3.unsat:
            mismatches.append((label, 'Z3 found a counterexample: %s -> %s' % (result, solver.model())))
        else:
            led_proven += 1

    if mismatches:
        print('prove_z3_sop_equiv: FAILED -- %d mismatch(es):' % len(mismatches))
        for name, reason in mismatches:
            print('  %s: %s' % (name, reason))
        sys.exit(1)

    print('prove_z3_sop_equiv: OK -- %d/%d FF next-state functions and %d/%d led output cones '
          'proven equivalent (Z3 SOP miter per cone, all UNSAT)' %
          (proven, len(ff_cells), led_proven, len(gold.output_bits.get('led', []))))


if __name__ == '__main__':
    main()
