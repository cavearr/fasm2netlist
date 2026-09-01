#!/usr/bin/env python3
"""Resolve each register's data source through the fabric netlist.

Numeric simulation says "these 14 registers differ at cycle 6"; this says what
each register's D is actually connected to.  Walk the assign graph back from
every column's D source to whatever really drives it -- another register's
output, a module port, or a tie-off -- and print that beside the register's
name from the placement.  For a plain shift stage the answer should be the
previous stage, and anything else is the bug.

  symsim_fabric.py <fabric.v> <ffmap.txt> [register ...]
"""
import re, sys, collections

fab_p, ffmap_p = sys.argv[1], sys.argv[2]
want = set(sys.argv[3:])
src = open(fab_p).read()

IDENT = r'(?:\\\\[^\s]+ |\w+)'
def _clean(n):
    return n[1:].rstrip() if n.startswith('\\') else n
alias = {_clean(a): _clean(b)
         for a, b in re.findall(r'^  assign (%s)= ?(%s);' % (IDENT, IDENT), src, re.M)}
tied = {_clean(a): b for a, b in re.findall(r"^  wire (%s)= ?1'b(\d);" % IDENT, src, re.M)}
ports = {_clean(a) for a in re.findall(r'^  (?:input|output) wire (%s)' % IDENT, src, re.M)}

# xcol instances: parameters and pin connections
inst = {}
for m in re.finditer(r'xcol #\(\.INIT\((\w+\'h[0-9a-f]+)\), \.FF_SRC\("(\w+)"\), \.FF5_SRC\("(\w+)"\), '
                     r'\.OUTMUX\("(\w+)"\),.*?\\(\S+) \((.*?)\);', src, re.S):
    init, ffsrc, ff5src, outmux, name, pins = m.groups()
    conn = {k: _clean(v) for k, v in re.findall(r'\.(\w+)\((%s|1\'b\d)\)' % IDENT, pins)}
    inst[name] = dict(init=init, ff=ffsrc, ff5=ff5src, outmux=outmux, pins=conn)

driver = {}                      # net -> (instance, pin)
for name, d in inst.items():
    for pin in ('Q', 'MUX'):
        n = d['pins'].get(pin)
        if n and not n.startswith("1'b"):
            driver[n] = (name, pin)

def resolve(net, depth=0):
    """Follow aliases to whatever really drives this net."""
    seen = set()
    while net in alias and net not in seen and net not in driver:
        seen.add(net)
        net = alias[net]
    if net in driver:
        return net, 'reg %s.%s' % driver[net]
    if net in ports:
        return net, 'PORT'
    if net in tied:
        return net, "tie %s" % tied[net]
    return net, 'undriven'

label = {}
for line in open(ffmap_p):
    lab, net = line.rstrip('\n').split('\t')
    label[net] = lab

for net, lab in sorted(label.items(), key=lambda kv: kv[1]):
    if want and lab not in want:
        continue
    if net not in driver:
        print('%-18s %s  <- NO DRIVER' % (lab, net))
        continue
    iname, pin = driver[net]
    d = inst[iname]
    src_sel = d['ff5'] if pin == 'MUX' else d['ff']
    print('%-18s %s' % (lab, iname))
    print('   %s from %s, INIT=%s' % ('5FF' if pin == 'MUX' else 'FF', src_sel, d['init']))
    if src_sel == 'X':
        n, what = resolve(d['pins'].get('X', ''))
        print('   D <- X pin -> %s (%s)' % (n, what))
    else:  # O5/O6: the LUT inputs are what matter
        for i in range(1, 7):
            p = d['pins'].get('A%d' % i)
            if not p:
                continue
            n, what = resolve(p)
            tag = label.get(n)
            print('   A%d <- %s%s' % (i, what, ('  = ' + tag) if tag else ''))
