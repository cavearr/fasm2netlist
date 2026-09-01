#!/usr/bin/env python3
"""Map the gold netlist's registers to nets in the tile-model fabric.

The placement says which (tile, site, bel) each gold cell sits on; the tile-type
database says which wire a site pin lands on.  Composing them gives, for every
gold register, the fabric net carrying its output:

  main FF  (AFF..DFF)   -> the slice's xQ pin
  second FF (A5FF..)    -> the xMUX pin, and only when OUTMUX selects x5Q --
                           otherwise the 5FF's output leaves no observable net

Prints `gold_label fabric_net` pairs; unobservable registers are reported on
stderr rather than silently dropped.
"""
import json, os, re, sys

XC7 = os.environ.get('XC7_BITSTREAM_TOOLS_DIR', os.path.expanduser('~/xc7-bitstream-tools'))
DB = os.environ.get('PRJXRAY_DB', os.path.join(XC7, '.deps', 'prjxray-db', 'virtex7'))
DEVICE = os.environ.get('DEVICE', 'xc7vx485t')
EX = os.path.join(XC7, 'examples', 'vc707-johnson')

place = json.load(open(os.path.join(EX, 'johnson_placement.json')))
gold = json.load(open(os.path.join(EX, 'johnson.json')))['modules']['top']
grid = json.load(open(os.path.join(DB, DEVICE, 'tilegrid.json')))

label = {}
for name, n in gold.get('netnames', {}).items():
    if n.get('hide_name'):
        continue
    bits = n['bits']
    for i, b in enumerate(bits):
        if isinstance(b, int) and b not in label:
            label[b] = name if len(bits) == 1 else '%s[%d]' % (name, i)

def site_pins(tile_type):
    p = os.path.join(DB, 'tile_type_%s.json' % tile_type)
    tt = json.load(open(p))
    return [{k: v['wire'] for k, v in s['site_pins'].items()} for s in tt['sites']]

pins_cache = {}
def pins_for(tile, site):
    # the placement names a site absolutely (SLICE_X47Y137); the tile type lists
    # its sites in X order, so the ordinal is that site's rank within the tile
    tt = grid[tile]['type']
    if tt not in pins_cache:
        pins_cache[tt] = site_pins(tt)
    sites = grid[tile].get('sites') or {}
    order = sorted((s for s in sites if s.startswith('SLICE_')),
                   key=lambda s: int(re.match(r'SLICE_X(\d+)Y', s).group(1)))
    if site not in order:
        return {}
    ordinal = order.index(site)
    per = pins_cache[tt]
    return per[ordinal] if ordinal < len(per) else {}

def sanitise(s):
    return re.sub(r'[^A-Za-z0-9]', '_', s)

FF = re.compile(r'^([ABCD])(5?)FF$')
out, skipped = [], []
for cell, info in sorted(place.items()):
    if info['type'] != 'SLICE_FFX':
        continue
    m = FF.match(info['bel'])
    if not m:
        continue
    col, is5 = m.group(1), bool(m.group(2))
    c = gold['cells'].get(cell)
    if not c:
        continue
    q = (c['connections'].get('Q') or [None])[0]
    lab = label.get(q)
    if lab is None:
        skipped.append((cell, 'no name for its output net'))
        continue
    pins = pins_for(info['tile'], info['site'])
    pin = col + ('MUX' if is5 else 'Q')
    if pin not in pins:
        skipped.append((cell, 'no %s pin' % pin))
        continue
    out.append((lab, sanitise(info['tile'] + '/' + pins[pin])))

for lab, net in out:
    print('%s\t%s' % (lab, net))
for cell, why in skipped:
    print('unmapped: %s (%s)' % (cell, why), file=sys.stderr)
print('%d of %d registers mapped' % (len(out), len(out) + len(skipped)), file=sys.stderr)
