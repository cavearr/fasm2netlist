#!/usr/bin/env python3
"""Rewrite an LVS extraction with the source netlist's own names.

The extraction names every cell after the physical site it was found in.
The placement says which gold cell sits at each site, and the gold netlist's
netnames say which source signal each of those cells drives.  Composing the
two labels the extraction in the designer's vocabulary without changing a
single connection.
"""
import json, os, re, sys

R = os.environ.get('XC7_BITSTREAM_TOOLS_DIR', os.path.expanduser('~/xc7-bitstream-tools'))
ex = R + '/examples/vc707-johnson'
gold = json.load(open(ex + '/johnson.json'))['modules']['top']
place = json.load(open(ex + '/johnson_placement.json'))
gates = open(sys.argv[1]).read()

FF_BEL = re.compile(r'^([ABCD])(5?)FF$')
LUT_BEL = re.compile(r'^([ABCD])[56]?LUT$')

CELL_SUFFIX = '_i'   # a cell and a net may not share a name

def vname(s):
    return re.sub(r'[^A-Za-z0-9_]', '_', s)

# net id -> source signal label, preferring named (non-hidden) signals
label = {}
for name, n in gold.get('netnames', {}).items():
    if n.get('hide_name'):
        continue
    bits = n['bits']
    for i, b in enumerate(bits):
        if isinstance(b, int) and b not in label:
            label[b] = name if len(bits) == 1 else '%s[%d]' % (name, i)

OUT_PIN = {'FDRE': 'Q', 'FDSE': 'Q', 'FDCE': 'Q', 'FDPE': 'Q'}

cell_rename, net_rename = {}, {}
for gname, info in place.items():
    cell = gold['cells'].get(gname)
    if cell is None:
        continue
    tile, site, bel = info['tile'], info['site'], info['bel']
    m = FF_BEL.match(bel)
    if m and info['type'] == 'SLICE_FFX':
        extracted = 'ff_%s_%s_%s%s' % (vname(tile), site, m.group(1), '5' if m.group(2) else '')
    else:
        m = LUT_BEL.match(bel)
        if not m or info['type'] != 'SLICE_LUTX':
            continue
        extracted = 'lut_%s_%s_%s' % (vname(tile), site, m.group(1))

    out_net = None
    pin = OUT_PIN.get(cell['type'])
    if pin:
        conns = cell['connections'].get(pin) or []
        if conns and isinstance(conns[0], int):
            out_net = conns[0]
    sig = label.get(out_net)
    if sig:
        cell_rename[extracted] = sig + CELL_SUFFIX
    else:
        cell_rename[extracted] = gname + CELL_SUFFIX

# rename the extraction's net for a matched FF output to the source signal
for gname, info in place.items():
    if info['type'] != 'SLICE_FFX':
        continue
    m = FF_BEL.match(info['bel'])
    if not m:
        continue
    extracted = 'ff_%s_%s_%s%s' % (vname(info['tile']), info['site'], m.group(1), '5' if m.group(2) else '')
    inst = re.search(r'\\' + re.escape(extracted) + r' \(([^;]*)\);', gates)
    if not inst:
        continue
    q = re.search(r'\.Q\(([^)]*)\)', inst.group(1))
    if q and q.group(1).strip() not in ('', "1'b0"):
        net = q.group(1).strip()
        if net.startswith('n_'):
            nm = cell_rename.get(extracted, net)
            net_rename[net] = nm[:-len(CELL_SUFFIX)] if nm.endswith(CELL_SUFFIX) else nm

def sub_escaped(text, table, prefix):
    def repl(m):
        name = m.group(1)
        return '\\' + prefix + table[name] + ' ' if name in table else m.group(0)
    return re.sub(r'\\([A-Za-z0-9_$.\[\]]+) ', repl, text)

out = sub_escaped(gates, cell_rename, '')
# gold net names contain '.' and '[]', so they have to go in escaped-identifier
# form -- backslash, name, terminating space -- everywhere they appear
for old, new in sorted(net_rename.items(), key=lambda kv: -len(kv[0])):
    out = re.sub(r'\b' + re.escape(old) + r'\b', lambda m, n=new: '\\' + n + ' ', out)

open(sys.argv[2], 'w').write(out)
print('renamed %d cells, %d nets' % (len(cell_rename), len(net_rename)))
