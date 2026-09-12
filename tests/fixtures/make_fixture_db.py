#!/usr/bin/env python3
"""Regenerate the synthetic "device" the smoke test runs against.

The real Project X-Ray database is multiple gigabytes, so this repo carries a
hand-built stand-in instead: a handful of tile types with SLICE-, DSP- and
BRAM-shaped sites, wired so that every cell family fasm2netlist knows how to
extract has something to be extracted from. It is not a model of any real
silicon -- it exists so that "does the tool still build and run end to end"
can be answered without a database download.

The site pin NAMES and the wire-naming conventions the extractor keys off
(prjxray's "DSP_<n>_<pin>" and "BRAM_<kind>_<pin>" wire prefixes) are copied
from the real database, since those are what the decoders read.

Run from anywhere: python3 tests/fixtures/make_fixture_db.py
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
DB = os.path.join(HERE, 'db', 'testfam')


def wires(names):
    return {n: None for n in names}


def site(type_, x, pins, prefix=''):
    return {
        'type': type_,
        'x_coord': x,
        'y_coord': 0,
        'site_pins': {p: {'wire': prefix + p} for p in pins},
    }


# ---------------------------------------------------------------- SLICEM
SLICE_PINS = []
for col in 'ABCD':
    SLICE_PINS += [col] + ['%s%d' % (col, i) for i in range(1, 7)]
    SLICE_PINS += [col + 'I', col + 'MUX', col + 'Q', col + 'X']
SLICE_PINS += ['CE', 'CIN', 'CLK', 'COUT', 'SR', 'WE']
# "CI" is the C column's write-data pin and collides with nothing: the carry
# input is "CIN". Both are in the list above via the loop and the tail.

NSLICE = 4
clbm = {
    'tile_type': 'TESTCLBM',
    'wires': wires(['S%d_%s' % (i, p) for i in range(NSLICE) for p in SLICE_PINS]),
    'pips': {},
    'sites': [site('SLICEM', i, SLICE_PINS, 'S%d_' % i) for i in range(NSLICE)],
}

# ------------------------------------------------------------------- DSP
# A representative slice of the DSP48E1 interface: enough of each vector to
# exercise the bus assembly, plus the scalar control pins.
DSP_PINS = (['A%d' % i for i in range(3)] + ['B%d' % i for i in range(3)] +
            ['C%d' % i for i in range(3)] + ['D%d' % i for i in range(3)] +
            ['P%d' % i for i in range(3)] + ['ALUMODE%d' % i for i in range(4)] +
            ['OPMODE%d' % i for i in range(7)] + ['INMODE%d' % i for i in range(5)] +
            ['CARRYOUT%d' % i for i in range(4)] +
            ['CLK', 'CARRYIN', 'CEA1', 'CEA2', 'CEB1', 'CEB2', 'CEC', 'CEM', 'CEP',
             'RSTA', 'RSTB', 'RSTC', 'RSTM', 'RSTP', 'MULTSIGNOUT', 'OVERFLOW',
             'UNDERFLOW', 'PATTERNDETECT'])
dsp = {
    'tile_type': 'TESTDSP',
    'wires': wires(['DSP_%d_%s' % (n, p) for n in (0, 1) for p in DSP_PINS]),
    'pips': {},
    'sites': [site('DSP48E1', n, DSP_PINS, 'DSP_%d_' % n) for n in (0, 1)],
}

# ------------------------------------------------------------------ BRAM
# Canonical (RAMB18E1) pin names; the lower 18Kb site carries them under
# FIFO-flavoured pin names in the real database, which is exactly why the
# decoder recovers the canonical name from the WIRE rather than the pin.
BRAM18_PINS = (['DOADO%d' % i for i in range(4)] + ['DOBDO%d' % i for i in range(4)] +
               ['DIADI%d' % i for i in range(4)] + ['DIBDI%d' % i for i in range(4)] +
               ['ADDRARDADDR%d' % i for i in range(4)] +
               ['ADDRBWRADDR%d' % i for i in range(4)] + ['WEA0', 'WEA1'] +
               ['WEBWE%d' % i for i in range(4)] +
               ['CLKARDCLK', 'CLKBWRCLK', 'ENARDEN', 'ENBWREN', 'REGCEAREGCE',
                'REGCEB', 'REGCLKARDRCLK', 'REGCLKB', 'RSTRAMARSTRAM', 'RSTRAMB',
                'RSTREGARSTREG', 'RSTREGB'])
BRAM36_PINS = (['DOADO%d' % i for i in range(4)] + ['DIADI%d' % i for i in range(4)] +
               ['ADDRARDADDRL%d' % i for i in range(4)] +
               ['ADDRARDADDRU%d' % i for i in range(4)] +
               ['ADDRBWRADDRL%d' % i for i in range(4)] +
               ['ADDRBWRADDRU%d' % i for i in range(4)] +
               ['WEAL0', 'WEAL1', 'WEAU0', 'WEAU1'] +
               ['CLKARDCLKL', 'CLKARDCLKU', 'CLKBWRCLKL', 'CLKBWRCLKU',
                'ENARDENL', 'ENARDENU', 'ENBWRENL', 'ENBWRENU',
                'REGCEAREGCEL', 'REGCEAREGCEU', 'REGCEBL', 'REGCEBU',
                'REGCLKARDRCLKL', 'REGCLKARDRCLKU', 'REGCLKBL', 'REGCLKBU',
                'RSTRAMARSTRAMLRST', 'RSTRAMARSTRAMU', 'RSTRAMBL', 'RSTRAMBU',
                'RSTREGARSTREGL', 'RSTREGARSTREGU', 'RSTREGBL', 'RSTREGBU',
                'CASCADEINA', 'CASCADEOUTA'])
bram_wires = (['BRAM_FIFO18_' + p for p in BRAM18_PINS] +
              ['BRAM_RAMB18_' + p for p in BRAM18_PINS] +
              ['BRAM_FIFO36_' + p for p in BRAM36_PINS] + ['EXT0'])
# In 36Kb mode the two 18Kb halves see the same control and address
# signals, so give the fixture a pip for each L/U pair to join them -- that
# is what real routing does, and it is what the extractor cross-checks.
LU_PIPS = {}
for p in BRAM36_PINS:
    u = None
    if p == 'RSTRAMARSTRAMLRST':
        u = 'RSTRAMARSTRAMU'
    elif 'L' in p:
        # ...L, ...L<n>: the U twin is the same name with that L flipped
        for i in range(len(p) - 1, -1, -1):
            if p[i] == 'L' and (p[:i] + 'U' + p[i + 1:]) in BRAM36_PINS:
                u = p[:i] + 'U' + p[i + 1:]
                break
    if u:
        LU_PIPS['BRAM_FIFO36_%s.BRAM_FIFO36_%s' % (u, p)] = {
            'dst_wire': 'BRAM_FIFO36_' + u,
            'src_wire': 'BRAM_FIFO36_' + p,
        }
# plus one routing pip onto the 36Kb site's data input: the presence of any
# pip on this site is what tells the decoder the tile is one RAMB36 rather
# than two RAMB18s.
LU_PIPS['BRAM_FIFO36_DIADI0.EXT0'] = {'dst_wire': 'BRAM_FIFO36_DIADI0', 'src_wire': 'EXT0'}

bram = {
    'tile_type': 'TESTBRAM',
    'wires': wires(bram_wires),
    'pips': LU_PIPS,
    'sites': [
        site('FIFO18E1', 0, BRAM18_PINS, 'BRAM_FIFO18_'),
        site('RAMBFIFO36E1', 1, BRAM36_PINS, 'BRAM_FIFO36_'),
        site('RAMB18E1', 2, BRAM18_PINS, 'BRAM_RAMB18_'),
    ],
}

tilegrid = {
    'TESTCLB_X0Y0': {'type': 'TESTCLB', 'grid_x': 0, 'grid_y': 0,
                     'sites': {'SLICE_X0Y0': 'SLICEX'}},
    'TESTCLBM_X0Y1': {'type': 'TESTCLBM', 'grid_x': 0, 'grid_y': 1,
                      'sites': {'SLICE_X%dY1' % i: 'SLICEM' for i in range(NSLICE)}},
    'TESTDSP_X1Y0': {'type': 'TESTDSP', 'grid_x': 1, 'grid_y': 0,
                     'sites': {'DSP48_X0Y0': 'DSP48E1', 'DSP48_X0Y1': 'DSP48E1'}},
    'TESTBRAM_X2Y0': {'type': 'TESTBRAM', 'grid_x': 2, 'grid_y': 0,
                      'sites': {'RAMB18_X0Y0': 'FIFO18E1', 'RAMB18_X0Y1': 'RAMB18E1',
                                'RAMB36_X0Y0': 'RAMBFIFO36E1'}},
    'TESTBRAM_X2Y1': {'type': 'TESTBRAM', 'grid_x': 2, 'grid_y': 1,
                      'sites': {'RAMB18_X0Y2': 'FIFO18E1', 'RAMB18_X0Y3': 'RAMB18E1',
                                'RAMB36_X0Y1': 'RAMBFIFO36E1'}},
}


def dump(path, obj):
    with open(path, 'w') as f:
        json.dump(obj, f, indent=1, sort_keys=True)
        f.write('\n')


dump(os.path.join(DB, 'tile_type_TESTCLBM.json'), clbm)
dump(os.path.join(DB, 'tile_type_TESTDSP.json'), dsp)
dump(os.path.join(DB, 'tile_type_TESTBRAM.json'), bram)
dump(os.path.join(DB, 'device0', 'tilegrid.json'), tilegrid)
print('wrote fixture tile types and tilegrid under', DB)
print('L/U join pips for the 36Kb site (design.fasm carries one line each):')
for v in sorted(LU_PIPS.values(), key=lambda v: v['dst_wire']):
    if v['src_wire'] != 'EXT0':
        print('TESTBRAM_X2Y1.%s.%s' % (v['dst_wire'], v['src_wire']))
