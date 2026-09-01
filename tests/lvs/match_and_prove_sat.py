#!/usr/bin/env python3
"""Solve the actual "downstream matching problem" fasm2netlist's own README
flags -- pairing fasm2netlist's anonymous FF cells to the gold synthesis's
own cells -- with NO placement.json oracle, using only: register next-state
cone size/topology as a candidate filter, and a SAT miter (via Z3's SAT
core) to confirm or falsify each candidate. `prove_z3_sop_equiv.py` (kept
as-is) instead takes the correspondence as GIVEN, from real placement
ground truth, and proves functional equivalence given that -- a genuinely
different, complementary check. This script proves the correspondence
itself is discoverable, then (as a self-check only, never fed to the
matcher) grades the result against that same placement ground truth.

Why this scales further than prove_z3_sop_equiv.py's plain SOP-per-query
approach:
  - One shared Z3-expression memo per matching ROUND (not a fresh one per
    register): identical sub-cones across many registers get built once and
    reused, instead of retraversed from scratch 36+ times.
  - LUTs are still bounded (<=64 truth-table rows), but cones are only ever
    evaluated for registers actually under test in a round, not the whole
    design at once.
  - Candidate generation is a cone_size/topology signature match (cheap,
    O(cones)) BEFORE any SAT call, not O(n^2) blind pairwise SAT.

The matching algorithm (see match() below), because pure topology alone
cannot resolve a chain of structurally-IDENTICAL registers (e.g. a plain
shift register -- every stage's cone is a bare wire from its predecessor,
so no stage's SIGNATURE is unique) without a base case:

  1. Free anchors, no SAT: trace every top-level output port back through
     pure passthrough cells (IBUF/IBUFDS/BUFG/OBUF); if it bottoms out at a
     bare FF Q on both sides for the SAME (shared-by-name) port bit, that's
     a certain match -- a wire is a wire, nothing to falsify.
  2. Readiness rounds: a register is "ready" once every OTHER register its
     cone references is already matched. Group ready registers by
     (cone_size, primary-input-names-referenced, #distinct-register-refs);
     within a matching signature group, SAT-confirm (miter UNSAT) or
     falsify (SAT) every gold/gate candidate pair, keep only pairs that are
     BOTH the sole survivor for their gold register AND for their gate
     register. Repeat to a fixpoint.
  3. Structural bootstrap: when readiness rounds stall with registers still
     unmatched (a closed loop/chain with no resolvable base case), find any
     register whose static signature is GLOBALLY UNIQUE (exactly one gold +
     one gate share it) even though it isn't "ready" yet, and accept it as
     a PROVISIONAL hypothesis (labelled as such, not yet SAT-checked in
     isolation -- doing so would be vacuous, see the module-level comment
     in match() for why). This one seed is usually enough to make its
     neighbours ready, which then get REAL SAT confirmations in step 2,
     which makes their neighbours ready, cascading down the whole chain.
  4. Once a hypothesis's own dependencies eventually get matched by that
     cascade, it becomes checkable for real -- re-verify it with an actual
     SAT miter and relabel it confirmed, or flag it if the miter now finds
     a counterexample (the hypothesis was wrong; this script does not
     attempt backtracking/rollback of whatever cascaded from a bad
     hypothesis -- see Limitations in the final report).

Skips (exit 77) if the sibling xc7-bitstream-tools checkout, or the
z3-solver package, isn't present.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from collections import Counter, defaultdict, namedtuple

SKIP = 77

FF_TYPES = {'FDRE', 'FDSE', 'FDCE', 'FDPE'}
SR_PIN = {'FDRE': 'R', 'FDSE': 'S', 'FDCE': 'CLR', 'FDPE': 'PRE'}
SR_ASSERTS_ONE = {'FDRE': False, 'FDSE': True, 'FDCE': False, 'FDPE': True}
PASSTHROUGH_TYPES = ('IBUF', 'IBUFDS', 'BUFG', 'OBUF')

Cone = namedtuple('Cone', 'size primary_inputs state_refs')


def skip(msg):
    print('SKIP: %s' % msg)
    sys.exit(SKIP)


def vname(s):
    return re.sub(r'[^A-Za-z0-9_]', '_', s)


def signature(cone):
    # NOTE: cone.size (LUT-cell count) is deliberately NOT part of this key.
    # It isn't portable between the two sides' different decompositions of
    # the same logic function -- gold's generic LUTk cells vs. gate's
    # LUT6_2 cells (which also carry real "uncelled route-through" LUTs the
    # bitstream's own router introduced, see main.cpp's comment on that) --
    # verified empirically: the same functional group of registers lands at
    # different cone.size values on the two sides, but always the SAME
    # (primary_inputs, #distinct-register-refs). Use cone.size only as a
    # secondary, non-authoritative ranking hint if ever needed.
    return (tuple(sorted(cone.primary_inputs)), len(cone.state_refs))


def lut_sop(z3mod, invars, init_int):
    n = len(invars)
    terms = []
    for m in range(1 << n):
        if (init_int >> m) & 1:
            lits = [invars[b] if (m >> b) & 1 else z3mod.Not(invars[b]) for b in range(n)]
            terms.append(z3mod.And(*lits) if lits else z3mod.BoolVal(True))
    return z3mod.Or(*terms) if terms else z3mod.BoolVal(False)


# ---------------------------------------------------------------------
# gold side: xc7-bitstream-tools's own Yosys-synthesized johnson.json --
# NOTE: unlike prove_z3_sop_equiv.py, this class never reads placement.json.
# ---------------------------------------------------------------------

class GoldNetlist:
    def __init__(self, json_path, module='top'):
        d = json.load(open(json_path))
        self.mod = d['modules'][module]
        self.cells = self.mod['cells']
        self.ports = self.mod['ports']

        self.driver = {}
        for cname, c in self.cells.items():
            for pin, direction in c.get('port_directions', {}).items():
                if direction not in ('output', 'inout'):
                    continue
                for bit in c['connections'].get(pin, []):
                    if isinstance(bit, int):
                        self.driver[bit] = (cname, pin)

        self.input_label = {}
        self.output_bits = {}
        for pname, p in self.ports.items():
            bits = p['bits']
            if p['direction'] == 'input':
                for i, b in enumerate(bits):
                    if isinstance(b, int):
                        self.input_label[b] = pname if len(bits) == 1 else '%s[%d]' % (pname, i)
            elif p['direction'] == 'output':
                self.output_bits[pname] = bits

    def ff_names(self):
        return [n for n, c in self.cells.items() if c['type'] in FF_TYPES]

    def ff_flavour(self, cellname):
        c = self.cells[cellname]
        init = str(c.get('parameters', {}).get('INIT', '')).strip().lower()
        return (c['type'], init[-1] if init else '?')

    def _pin_net(self, cell, pin):
        conns = cell['connections'].get(pin)
        if not conns:
            return None
        return conns[0]  # int net id, or a '0'/'1' constant string

    def trace_bare(self, net_id):
        """Follow pure passthroughs only; return the FF cellname if this net
        is, with zero intervening logic, exactly one FF's Q -- else None."""
        seen = set()
        while True:
            if not isinstance(net_id, int) or net_id in seen:
                return None
            seen.add(net_id)
            driver = self.driver.get(net_id)
            if driver is None:
                return None
            cname, _pin = driver
            ctype = self.cells[cname]['type']
            if ctype in FF_TYPES:
                return cname
            if ctype in PASSTHROUGH_TYPES:
                net_id = self._pin_net(self.cells[cname], 'I')
                continue
            return None

    def cone_analyze(self, cellname):
        cell = self.cells[cellname]
        visited, primary_inputs, state_refs = set(), set(), set()

        def walk(net):
            if not isinstance(net, int):
                return  # constant '0'/'1'
            if net in self.input_label:
                primary_inputs.add(self.input_label[net])
                return
            driver = self.driver.get(net)
            if driver is None:
                return
            cname, _pin = driver
            ctype = self.cells[cname]['type']
            if ctype in FF_TYPES:
                state_refs.add(cname)
                return
            if cname in visited:
                return
            visited.add(cname)
            m = re.match(r'^LUT(\d+)$', ctype)
            if m:
                for i in range(int(m.group(1))):
                    walk(self._pin_net(self.cells[cname], 'I%d' % i))
            elif ctype in PASSTHROUGH_TYPES:
                walk(self._pin_net(self.cells[cname], 'I'))

        pins = ['D', 'CE']
        if cell['type'] in SR_PIN:
            pins.append(SR_PIN[cell['type']])
        for p in pins:
            walk(self._pin_net(cell, p))
        return Cone(len(visited), frozenset(primary_inputs), frozenset(state_refs))

    def eval_net(self, z3mod, net, memo, matched):
        if not isinstance(net, str) and net in memo:
            return memo[net]
        if isinstance(net, str):
            return z3mod.BoolVal(net == '1')
        if net in self.input_label:
            expr = z3mod.Bool('in_' + self.input_label[net])
            memo[net] = expr
            return expr
        driver = self.driver.get(net)
        if driver is None:
            memo[net] = z3mod.BoolVal(False)
            return memo[net]
        cname, _pin = driver
        cell = self.cells[cname]
        ctype = cell['type']
        if ctype in FF_TYPES:
            partner = matched.get(cname)
            expr = z3mod.Bool('st_gate_' + partner) if partner else z3mod.Bool('st_gold_' + cname)
        elif re.match(r'^LUT\d+$', ctype):
            k = int(ctype[3:])
            invars = [self.eval_net(z3mod, self._pin_net(cell, 'I%d' % i), memo, matched) for i in range(k)]
            expr = lut_sop(z3mod, invars, int(cell['parameters']['INIT'], 2))
        elif ctype in PASSTHROUGH_TYPES:
            expr = self.eval_net(z3mod, self._pin_net(cell, 'I'), memo, matched)
        else:
            raise ValueError('gold: unhandled cell type %r (%r)' % (ctype, cname))
        memo[net] = expr
        return expr

    def ff_next(self, z3mod, cellname, memo, matched):
        cell = self.cells[cellname]
        ctype = cell['type']
        D = self.eval_net(z3mod, self._pin_net(cell, 'D'), memo, matched)
        CE = self.eval_net(z3mod, self._pin_net(cell, 'CE'), memo, matched)
        partner = matched.get(cellname)
        self_sym = z3mod.Bool('st_gate_' + partner) if partner else z3mod.Bool('st_gold_' + cellname)
        hold_or_load = z3mod.If(CE, D, self_sym)
        sr = self.eval_net(z3mod, self._pin_net(cell, SR_PIN[ctype]), memo, matched)
        return z3mod.If(sr, z3mod.BoolVal(SR_ASSERTS_ONE[ctype]), hold_or_load)


# ---------------------------------------------------------------------
# gate side: fasm2netlist's own generated Verilog
# ---------------------------------------------------------------------

class GateNetlist:
    OUTPUT_PINS = {
        'LUT6_2': {'O6', 'O5'}, 'FDRE': {'Q'}, 'FDSE': {'Q'}, 'FDCE': {'Q'}, 'FDPE': {'Q'},
        'IBUF': {'O'}, 'IBUFDS': {'O'}, 'BUFG': {'O'}, 'OBUF': {'O'},
    }
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

        self.driver = {}
        self.cell_conns = {}
        self.alias = {}

        for line in src.splitlines():
            am = self.ASSIGN_RE.match(line)
            if am:
                self.alias[am.group(1)] = am.group(2)
                continue
            cm = self.CELL_RE.match(line)
            if not cm:
                continue
            ctype, init, cname, portlist = cm.groups()
            if ctype not in self.OUTPUT_PINS:
                continue
            conns = dict(re.findall(r'\.(\w+)\(([^()]*)\)', portlist))
            self.cell_conns[cname] = {'type': ctype, 'init': init, 'conns': conns}
            in_nets = {n for p, n in conns.items() if p not in self.OUTPUT_PINS[ctype]}
            for pin in self.OUTPUT_PINS[ctype]:
                net = conns.get(pin)
                if not net or net in in_nets:  # self-loop alias (BUFG mesh) -- never self-driver
                    continue
                self.driver[net] = (ctype, cname, pin)

    def ff_names(self):
        return [n for n, c in self.cell_conns.items() if c['type'] in FF_TYPES]

    def ff_flavour(self, cname):
        c = self.cell_conns[cname]
        init = (c.get('init') or '').strip().lower()   # e.g. "1'b1"
        return (c['type'], init[-1] if init else '?')

    def _pin_net(self, cname, pin):
        return self.cell_conns[cname]['conns'].get(pin)

    def _resolve_alias(self, net):
        seen = set()
        while net in self.alias and net not in seen:
            seen.add(net)
            net = self.alias[net]
        return net

    def trace_bare(self, net):
        net = self._resolve_alias(net)
        seen = set()
        while True:
            if net in seen:
                return None
            seen.add(net)
            driver = self.driver.get(net)
            if driver is None:
                return None
            ctype, cname, _pin = driver
            if ctype in FF_TYPES:
                return cname
            if ctype in PASSTHROUGH_TYPES:
                net = self._resolve_alias(self._pin_net(cname, 'I'))
                continue
            return None

    def cone_analyze(self, cellname):
        visited, primary_inputs, state_refs = set(), set(), set()

        def walk(net):
            if net is None or net in ("1'b0", "1'b1"):
                return
            net = self._resolve_alias(net)
            if net in self.input_ports:
                primary_inputs.add(net)
                return
            driver = self.driver.get(net)
            if driver is None:
                return
            ctype, cname, _pin = driver
            if ctype in FF_TYPES:
                state_refs.add(cname)
                return
            if cname in visited:
                return
            visited.add(cname)
            if ctype == 'LUT6_2':
                for i in range(6):
                    walk(self._pin_net(cname, 'I%d' % i))
            elif ctype in PASSTHROUGH_TYPES:
                walk(self._pin_net(cname, 'I'))

        cell = self.cell_conns[cellname]
        pins = ['D', 'CE']
        if cell['type'] in SR_PIN:
            pins.append(SR_PIN[cell['type']])
        for p in pins:
            walk(self._pin_net(cellname, p))
        return Cone(len(visited), frozenset(primary_inputs), frozenset(state_refs))

    def eval_net(self, z3mod, net, memo, matched):
        net = self._resolve_alias(net)
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
        driver = self.driver.get(net)
        if driver is None:
            memo[net] = z3mod.BoolVal(False)
            return memo[net]
        ctype, cname, pin = driver
        if ctype in FF_TYPES:
            expr = z3mod.Bool('st_gate_' + cname)  # gate is always the canonical namespace
        elif ctype == 'LUT6_2':
            init_int = int(self.cell_conns[cname]['init'].split("'h", 1)[1], 16)
            invars5 = [self.eval_net(z3mod, self._pin_net(cname, 'I%d' % i), memo, matched) for i in range(5)]
            if pin == 'O6':
                invars6 = invars5 + [self.eval_net(z3mod, self._pin_net(cname, 'I5'), memo, matched)]
                expr = lut_sop(z3mod, invars6, init_int & ((1 << 64) - 1))
            else:
                expr = lut_sop(z3mod, invars5, init_int & ((1 << 32) - 1))
        elif ctype in PASSTHROUGH_TYPES:
            expr = self.eval_net(z3mod, self._pin_net(cname, 'I'), memo, matched)
        else:
            raise ValueError('gate: unhandled driver cell type %r' % ctype)
        memo[net] = expr
        return expr

    def ff_next(self, z3mod, cellname, memo, matched):
        cell = self.cell_conns[cellname]
        ctype = cell['type']
        D = self.eval_net(z3mod, self._pin_net(cellname, 'D'), memo, matched)
        CE = self.eval_net(z3mod, self._pin_net(cellname, 'CE'), memo, matched)
        self_sym = z3mod.Bool('st_gate_' + cellname)
        hold_or_load = z3mod.If(CE, D, self_sym)
        sr = self.eval_net(z3mod, self._pin_net(cellname, SR_PIN[ctype]), memo, matched)
        return z3mod.If(sr, z3mod.BoolVal(SR_ASSERTS_ONE[ctype]), hold_or_load)


# ---------------------------------------------------------------------
# matching engine
# ---------------------------------------------------------------------

def match(z3mod, gold, gate, gold_ff, gate_ff, gold_cone, gate_cone, log):
    matched_g2t, matched_t2g, method = {}, {}, {}
    solver = z3mod.Solver()

    # Phase 1: free anchors via output-port bare-passthrough tracing --
    # no SAT needed, a wire is a wire, and port names are shared/known.
    for pname, bits in gold.output_bits.items():
        for i, net_id in enumerate(bits):
            label = pname if len(bits) == 1 else '%s[%d]' % (pname, i)
            g = gold.trace_bare(net_id)
            t = gate.trace_bare(label)
            if g and t and g not in matched_g2t and t not in matched_t2g:
                matched_g2t[g] = t
                matched_t2g[t] = g
                method[g] = 'output-port-anchor (%s)' % label
    log('phase 1 (output-port anchors): %d matched' % len(matched_g2t))

    # Phase 1b: flavour anchors.  A register's own flavour -- FDRE/FDSE/
    # FDCE/FDPE plus its INIT -- is recovered on the gate side from the FF's
    # own FASM features, so it is available with no placement and no solver.
    # It is rarely discriminating on its own, but it is decisive exactly where
    # topology is not: a counter that resets to a non-zero value has one
    # set-flavoured bit among its reset-flavoured ones, and that one bit is a
    # free, globally unique anchor into an otherwise symmetric shift chain.
    # Anchors are hypotheses, not proofs -- phase 4 re-verifies them by miter
    # once their dependencies resolve, and a wrong one is reported, not hidden.
    flav_g, flav_t = defaultdict(list), defaultdict(list)
    for g in gold_ff:
        if g not in matched_g2t:
            flav_g[gold.ff_flavour(g)].append(g)
    for t in gate_ff:
        if t not in matched_t2g:
            flav_t[gate.ff_flavour(t)].append(t)
    flavour_anchors = 0
    for flav in sorted(set(flav_g) & set(flav_t)):
        if len(flav_g[flav]) == 1 and len(flav_t[flav]) == 1:
            g, t = flav_g[flav][0], flav_t[flav][0]
            matched_g2t[g], matched_t2g[t] = t, g
            method[g] = 'structural-hypothesis (unverified)'
            flavour_anchors += 1
    log('phase 1b (unique-flavour anchors): %d matched (%d total)' % (flavour_anchors, len(matched_g2t)))

    # NOT worth re-trying: Weisfeiler-Leman colour refinement over the register
    # graph, using who-reads-whom as well as what-is-read.  In principle that
    # is what separates the stages of a tapped shift chain -- distance to the
    # feedback tap is a positional fact fan-in alone cannot see -- and it does
    # split all 36 registers into 36 singleton classes on each side here.  It
    # is still useless, because the classes do not CORRESPOND: refinement can
    # only relate two graphs that are isomorphic, and these two are not.  The
    # LFSR's first stage has 2 distinct register refs in its cone on the gold
    # side and 4 on the gate side (the bitstream's LUT decomposition drags in
    # neighbours gold's does not), so its colour differs from the first round,
    # and refinement then propagates that one disagreement to every register
    # reachable from it -- which, in a ring, is all of them.  The same fact
    # limits signature() itself: len(state_refs) is not as portable between
    # the two decompositions as the note above it assumes.  Anchors that read
    # only a register's OWN attributes (phase 1b) survive this; anything
    # derived from cone shape does not.
    def base_key(side, name):
        cone = gold_cone if side == 'g' else gate_cone
        return signature(cone[name])

    def try_confirm(glist, tlist):
        survivors = []
        for g in glist:
            for t in tlist:
                # Evaluating g's own next-state needs g's OWN "hold" (CE=0)
                # branch to resolve to the SAME symbol gate's next-state
                # uses for t -- i.e. the candidate pairing itself must be in
                # the substitution the cone is built under, not just
                # g/t's OTHER dependencies. Without this, g's hold branch
                # falls back to an unlinked gold-only free symbol and the
                # miter is spuriously SAT (falsified) any time CE isn't a
                # tied constant, even for the truly correct pairing --
                # verified: this was silently blocking every register whose
                # CE isn't always-1 until fixed. A fresh memo per candidate
                # is required too, since the substitution differs per pair.
                trial_g2t = matched_g2t if g in matched_g2t else {**matched_g2t, g: t}
                ng = gold.ff_next(z3mod, g, {}, trial_g2t)
                nt = gate.ff_next(z3mod, t, {}, matched_t2g)
                solver.push()
                solver.add(ng != nt)
                r = solver.check()
                solver.pop()
                if r == z3mod.unsat:
                    survivors.append((g, t))
        return survivors

    def readiness_round(key):
        progress = False
        ready_g = [g for g in gold_ff if g not in matched_g2t and gold_cone[g].state_refs <= matched_g2t.keys()]
        ready_t = [t for t in gate_ff if t not in matched_t2g and gate_cone[t].state_refs <= matched_t2g.keys()]
        groups_g, groups_t = defaultdict(list), defaultdict(list)
        for g in ready_g:
            groups_g[key('g', g)].append(g)
        for t in ready_t:
            groups_t[key('t', t)].append(t)
        for sig in sorted(set(groups_g) & set(groups_t)):
            glist, tlist = groups_g[sig], groups_t[sig]
            survivors = try_confirm(glist, tlist)
            gcount, tcount = Counter(g for g, t in survivors), Counter(t for g, t in survivors)
            for g, t in survivors:
                if gcount[g] == 1 and tcount[t] == 1:
                    matched_g2t[g], matched_t2g[t] = t, g
                    method[g] = 'sat-confirmed'
                    progress = True
        return progress


    # Phase 3: structural bootstrap for deadlocked clusters (e.g. a shift
    # chain with no primary-input-only base case -- see module docstring
    # for why a single globally-unique-signature seed is enough to unstick
    # the whole cascade via phase 2's readiness rounds).
    def bootstrap_once(key):
        sig_g, sig_t = defaultdict(list), defaultdict(list)
        for g in gold_ff:
            if g not in matched_g2t:
                sig_g[key('g', g)].append(g)
        for t in gate_ff:
            if t not in matched_t2g:
                sig_t[key('t', t)].append(t)
        for sig in sorted(set(sig_g) & set(sig_t)):
            if len(sig_g[sig]) == 1 and len(sig_t[sig]) == 1:
                g, t = sig_g[sig][0], sig_t[sig][0]
                matched_g2t[g], matched_t2g[t] = t, g
                method[g] = 'structural-hypothesis (unverified)'
                return True
        return False

    rounds = bootstraps = 0
    while readiness_round(base_key):
        rounds += 1
    while bootstrap_once(base_key):
        bootstraps += 1
        while readiness_round(base_key):
            rounds += 1
    log('phases 2-3: %d readiness passes, %d bootstrap seed(s), %d matched total' %
        (rounds, bootstraps, len(matched_g2t)))

    # Phase 4: re-verify hypotheses whose own dependencies eventually
    # resolved via the cascade they triggered. Loop to a fixpoint: a
    # register's cone can (and, empirically, sometimes does -- e.g. ABC9
    # apparently reusing a structurally-equivalent signal across module
    # boundaries) reference itself, which is trivially fine to check once g
    # is already (provisionally) in matched_g2t -- no need to, and
    # previously-buggily-did, exclude g's own entry from that lookup.
    revalidated = flagged = 0
    progress = True
    while progress:
        progress = False
        for g, t in list(matched_g2t.items()):
            if method.get(g) != 'structural-hypothesis (unverified)':
                continue
            if not (gold_cone[g].state_refs <= matched_g2t.keys()):
                continue
            ng = gold.ff_next(z3mod, g, {}, matched_g2t)
            nt = gate.ff_next(z3mod, t, {}, matched_t2g)
            solver.push()
            solver.add(ng != nt)
            r = solver.check()
            solver.pop()
            if r == z3mod.unsat:
                method[g] = 'sat-confirmed (hypothesis validated)'
                revalidated += 1
            else:
                method[g] = 'UNVERIFIED -- SAT counterexample on re-check, do not trust'
                flagged += 1
            progress = True
    log('phase 4 (hypothesis re-verification): %d validated, %d flagged' % (revalidated, flagged))

    return matched_g2t, method


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--exe', required=True)
    ap.add_argument('--xc7-tools-dir',
                     default=os.environ.get('XC7_BITSTREAM_TOOLS_DIR',
                                             os.path.expanduser('~/xc7-bitstream-tools')))
    ap.add_argument('--family', default='virtex7')
    ap.add_argument('--device', default='xc7vx485t')
    ap.add_argument('--part', default='xc7vx485tffg1761-2')
    ap.add_argument('-v', '--verbose', action='store_true')
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
    gold_json_path = os.path.join(example_dir, 'johnson.json')
    placement_path = os.path.join(example_dir, 'johnson_placement.json')  # grading only, never fed to match()

    for path, what in ((db, 'prjxray-db checkout'), (fasm_path, 'johnson.fasm'), (xdc_path, 'top.xdc'),
                        (gold_json_path, 'johnson.json')):
        if not os.path.exists(path):
            skip('%s not found at %s -- run `make vc707-johnson PRJXRAY_DB=...` in xc7-bitstream-tools first'
                 % (what, path))

    def log(msg):
        if args.verbose:
            print(msg)

    with tempfile.TemporaryDirectory() as tmp:
        out_v = os.path.join(tmp, 'gate.v')
        proc = subprocess.run(
            [args.exe, '--fasm', fasm_path, '--db', db, '--family', args.family, '--device', args.device,
             '--xdc', xdc_path, '--part', args.part, '--out', out_v, '--module', 'gate'],
            capture_output=True, text=True)
        assert proc.returncode == 0, 'fasm2netlist exited %d: %s' % (proc.returncode, proc.stderr)
        gate = GateNetlist(out_v)

    gold = GoldNetlist(gold_json_path)
    gold_ff, gate_ff = gold.ff_names(), gate.ff_names()
    gold_cone = {n: gold.cone_analyze(n) for n in gold_ff}
    gate_cone = {n: gate.cone_analyze(n) for n in gate_ff}
    print('loaded: %d gold FF, %d gate FF' % (len(gold_ff), len(gate_ff)))

    matched, method = match(z3, gold, gate, gold_ff, gate_ff, gold_cone, gate_cone, log)

    unmatched = [g for g in gold_ff if g not in matched]
    method_counts = Counter(m.split(' (')[0] for m in method.values())
    print('matched %d/%d gold FF; by method: %s' % (len(matched), len(gold_ff), dict(method_counts)))
    if unmatched:
        print('UNMATCHED gold FF (%d): %s' % (len(unmatched), unmatched))
    flagged = [g for g, m in method.items() if m.startswith('UNVERIFIED')]
    if flagged:
        print('FLAGGED (structural hypothesis failed re-verification): %s' % flagged)

    # ---- grading only: never used by match() above, purely to report how
    # well the oracle-free result agrees with real placement ground truth ----
    if os.path.exists(placement_path):
        placement = json.load(open(placement_path))
        ff_bel_re = re.compile(r'^([ABCD])(5?)FF$')
        correct = wrong = 0
        for g, t in matched.items():
            info = placement.get(g)
            if not info or info['type'] != 'SLICE_FFX':
                continue
            m = ff_bel_re.match(info['bel'])
            expected = 'ff_%s_%s_%s%s' % (vname(info['tile']), info['site'], m.group(1), '5' if m.group(2) else '')
            if expected == t:
                correct += 1
            else:
                wrong += 1
                print('GRADING MISMATCH: gold %s matched to gate %s, placement ground truth says %s' %
                      (g, t, expected))
        print('grading vs placement.json ground truth: %d correct, %d wrong (%d gold FF have no ground truth)' %
              (correct, wrong, len(matched) - correct - wrong))
        if wrong:
            sys.exit(1)

    # A flagged hypothesis is a real failure regardless of grading: it means
    # the matcher settled on a topological pairing it could not actually
    # prove functions correctly (see phase 4) -- grading can still call the
    # PAIRING itself "correct" (identity isn't what broke), so don't rely on
    # grading alone to catch this.
    if len(matched) < len(gold_ff) or flagged:
        sys.exit(1)

    print('match_and_prove_sat: OK')


if __name__ == '__main__':
    main()
