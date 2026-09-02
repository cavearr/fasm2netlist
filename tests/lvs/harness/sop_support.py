#!/usr/bin/env python3
"""Compose both sides' next-state functions in SOP form and compare their support.

Two decompositions of the same comparator share no intermediate nodes, so
pairing internal nets is hopeless.  Composing each side to a single function
over the primary state bits sidesteps that: every LUT becomes a sum of products
over its inputs, substituted through until only register outputs remain.  The
support -- which registers the result actually depends on -- is then directly
comparable, and a difference in it names the mis-connected signal.

  sop_support.py <fabric.v> <gold_cells.v> <ffmap.txt> <register>
"""
import re, sys
import z3

fab_p, gold_p, ffmap_p, target = sys.argv[1:5]
WITH_FF = '--with-ff' in sys.argv        # include CE/SR, as lvs_equiv does

# ---- the fabric: xcol instances + assign aliases ------------------------
src = open(fab_p).read()
IDENT = r'\\\\[^\s]+ |\S+'
def _n(x): return x[1:].strip() if x.startswith('\\') else x.strip()
alias = {_n(a): _n(b) for a, b in
         re.findall(r'^  assign (%s)\s*=\s*(%s)\s*;' % (IDENT, IDENT), src, re.M)}
tied = {_n(a): b for a, b in re.findall(r"^  wire (%s)\s*=\s*1'b(\d);" % IDENT, src, re.M)}
inst, byout = {}, {}
for m in re.finditer(r"xcol #\((.*?)\)\s*\\(\S+) \((.*?)\);", src, re.S):
    params, name, pins = m.groups()
    P = dict(re.findall(r'\.(\w+)\(([^)]*)\)', params))
    conn = {k: _n(v) for k, v in re.findall(r'\.(\w+)\((%s|1\'b\d)\)' % IDENT, pins)}
    def bit(k, dflt='0'):
        v = P.get(k, dflt)
        return v.strip()[-1]
    inst[name] = dict(init=int(re.search(r"h([0-9a-f]+)", P.get('INIT', "64'h0")).group(1), 16),
                      ff=P.get('FF_SRC', '"none"').strip('"'),
                      ff5=P.get('FF5_SRC', '"none"').strip('"'),
                      om=P.get('OUTMUX', '"none"').strip('"'),
                      srval=bit('FF_SRVAL'), srval5=bit('FF5_SRVAL'),
                      pins=conn)
    for pin in ('O6', 'O5', 'Q', 'MUX'):
        if conn.get(pin) and conn[pin] != "1'b0":
            byout.setdefault(conn[pin], (name, pin))

ffmap = {}
for line in open(ffmap_p):
    lab, net = line.rstrip('\n').split('\t')
    ffmap[net] = lab

def resolve(n):
    seen = set()
    while n in alias and n not in seen and n not in byout:
        seen.add(n); n = alias[n]
    return n

def lut(init, ins, width):
    """INIT as a sum of products over `ins` (ins[0] is the LSB of the index)."""
    terms = []
    for m in range(1 << width):
        if not (init >> m) & 1:
            continue
        terms.append(z3.And([ins[i] if (m >> i) & 1 else z3.Not(ins[i]) for i in range(width)]))
    return z3.Or(terms) if terms else z3.BoolVal(False)

memo = {}
def fab_val(net, depth=0):
    n = resolve(net)
    if n in memo: return memo[n]
    if depth > 200: return z3.BoolVal(False)
    if n in tied: return z3.BoolVal(tied[n] == '1')
    if n not in byout:
        return z3.Bool(n)                      # a port or an undriven net
    name, pin = byout[n]
    d = inst[name]
    if pin in ('Q', 'MUX'):
        # a register output: a free state variable, named as the gold side names it
        if pin == 'MUX' and d['om'] != '5Q':
            return fab_val(d['pins']['O6']) if d['om'] == 'O6' else z3.BoolVal(False)
        return z3.Bool(ffmap.get(n, n))
    memo[n] = None
    ins = [fab_val(d['pins'].get('A%d' % i, "1'b1"), depth + 1) if d['pins'].get('A%d' % i)
           else z3.BoolVal(True) for i in range(1, 7)]
    r = lut(d['init'], ins, 6) if pin == 'O6' else lut(d['init'] & 0xffffffff, ins[:5], 5)
    memo[n] = r
    return r

def fab_next(label):
    net = next(n for n, l in ffmap.items() if l == label)
    name, pin = byout[net]
    d = inst[name]
    src_sel = d['ff5'] if pin == 'MUX' else d['ff']
    if src_sel == 'O6':   dv = fab_val(d['pins']['O6'])
    elif src_sel == 'O5': dv = fab_val(d['pins']['O5']) if d['pins'].get('O5') else lut(d['init'] & 0xffffffff, [fab_val(d['pins']['A%d'%i]) for i in range(1,6)], 5)
    elif src_sel == 'X':  dv = fab_val(d['pins']['X'])
    else:                 dv = z3.BoolVal(False)
    if not WITH_FF:
        return dv
    # the full next state, as lvs_equiv models it: CE holds, SR forces
    q  = z3.Bool(ffmap.get(d['pins'][pin], d['pins'][pin]))
    ce = fab_val(d['pins']['CE']) if d['pins'].get('CE') not in (None, "1'b1") else z3.BoolVal(True)
    sr = fab_val(d['pins']['SR']) if d['pins'].get('SR') not in (None, "1'b0") else z3.BoolVal(False)
    nxt = z3.If(ce, dv, q)
    srval = (d.get('srval5' if pin == 'MUX' else 'srval') or '0') == '1'
    return z3.Or(sr, nxt) if srval else z3.And(z3.Not(sr), nxt)

# ---- the gold netlist: LUTn / FDxx instances ----------------------------
gsrc = open(gold_p).read()
gcells = [(t, p or '', n, c) for t, p, n, c in
          re.findall(r'\n  (\w+)\s*(?:#\(\s*(.*?)\s*\)\s*)?(\S+) \(\s*(.*?)\s*\);', gsrc, re.S)]
gdrv = {}
for typ, params, name, conns in gcells:
    c = {k: v.strip() for k, v in re.findall(r'\.(\w+)\((.*?)\)', conns)}
    for pin in ('O', 'Q'):
        if pin in c: gdrv[c[pin]] = (typ, params, c)
gplain = dict(re.findall(r'^  assign (\S+) = (\S+);', gsrc, re.M))
gmemo = {}
# the two sides alias the counter differently: yosys writes the top-level
# wire (led_int[i]), the placement labels it by the RTL name (core.johnson[i]).
# Same net -- normalise so the two supports are comparable.
def norm(n):
    n = n.replace('\\', '').strip()
    m = re.match(r'^led_int\[(\d+)\]$', n)
    return 'core.johnson[%s]' % m.group(1) if m else n

def gold_val(net, depth=0):
    net = net.strip()
    if net in ('1\'h0', '1\'b0'): return z3.BoolVal(False)
    if net in ('1\'h1', '1\'b1'): return z3.BoolVal(True)
    while net in gplain: net = gplain[net]
    if net in gmemo: return gmemo[net]
    if net not in gdrv or depth > 200:
        return z3.Bool(norm(net))
    typ, params, c = gdrv[net]
    if typ.startswith('FD'):
        return z3.Bool(norm(net))
    m = re.match(r'LUT(\d)$', typ)
    if not m:   # IBUF/OBUF/BUFG and friends pass through
        return gold_val(c.get('I', c.get('O', "1'h0")), depth + 1)
    w = int(m.group(1))
    init = int(re.search(r"INIT\((\d+)'([hd])([0-9a-f]+)", params).group(3),
               16 if re.search(r"INIT\(\d+'h", params) else 10) if re.search(r"INIT\(", params) else 0
    ins = [gold_val(c.get('I%d' % i, "1'h0"), depth + 1) for i in range(w)]
    gmemo[net] = None
    r = lut(init, ins, w)
    gmemo[net] = r
    return r

def gold_next(label):
    # gold's own FF wrapper, so both sides are asked the same question
    for typ, params, name, conns in gcells:
        if not typ.startswith('FD'): continue
        c = {k: v.strip() for k, v in re.findall(r'\.(\w+)\((.*?)\)', conns)}
        if norm(c.get('Q', '')) == label:
            d = gold_val(c['D'])
            if not WITH_FF:
                return d
            q  = z3.Bool(label)
            ce = gold_val(c.get('CE', "1'h1"))
            srpin = 'R' if typ == 'FDRE' else 'S' if typ == 'FDSE' else 'CLR' if typ == 'FDCE' else 'PRE'
            sr = gold_val(c.get(srpin, "1'h0"))
            nxt = z3.If(ce, d, q)
            return z3.Or(sr, nxt) if typ in ('FDSE', 'FDPE') else z3.And(z3.Not(sr), nxt)
    raise SystemExit('no gold register drives ' + label)

if target == 'ALL':
    labels = sorted(ffmap.values())
    eq = dif = err = 0
    for lab in labels:
        try:
            f, g = fab_next(lab), gold_next(lab)
        except SystemExit:
            print('  %-20s no gold register' % lab); err += 1; continue
        s = z3.Solver(); s.add(f != g)
        if s.check() == z3.unsat: eq += 1
        else:
            dif += 1
            print('  %-20s DIFFER' % lab)
    print('%d equal, %d differ, %d unmatched  (of %d)' % (eq, dif, err, len(labels)))
    sys.exit(0 if dif == 0 and err == 0 else 1)

f, g = fab_next(target), gold_next(target)
s = z3.Solver()
s.add(f != g)
print('%s: %s' % (target, 'EQUAL' if s.check() == z3.unsat else 'DIFFER'))

def support(expr):
    seen, out = set(), set()
    stack = [expr]
    while stack:
        e = stack.pop()
        if e.get_id() in seen: continue
        seen.add(e.get_id())
        if e.num_args() == 0 and e.decl().kind() == z3.Z3_OP_UNINTERPRETED:
            out.add(e.decl().name())
        stack.extend(e.children())
    return sorted(out)
sf, sg = set(support(f)), set(support(g))
print('  fabric depends on %d signals, gold on %d' % (len(sf), len(sg)))
print('  only in fabric: %s' % sorted(sf - sg)[:12])
print('  only in gold  : %s' % sorted(sg - sf)[:12])
