#!/usr/bin/env python3
"""The I/O site of a *_SING tile, extracted and paired by the half the FASM names.

A SING tile sits at the end of an I/O column and holds ONE OLOGIC (one ILOGIC,
one IDELAY) where a full IOI tile holds two.  Its tile type spells that site's
wires with index 0 -- IOI_OLOGIC0_D1, RIOI_OLOGIC0_OQ -- whichever half of a
full tile it stands for, while the FASM names the site by the half: OLOGIC_Y1
in the SING above its HCLK row, OLOGIC_Y0 in the one below.  LiteX's DDR3 PHY
on an Arty S7-50 puts an OSERDESE2 in each (ddram_reset_n at the top of the
right-hand column, ddram_we_n at the bottom), which is how the two ways this
goes wrong were found:

  - taking the index from the wire name, the extractor looks up OLOGIC_Y0 in
    the upper tile, finds no configuration, and emits the bypass wire where
    the OSERDESE2 is;
  - the hard-block census cannot tell from the placement which half a SING
    site is, so it names neither, and the one the extractor does instantiate
    never pairs with its synthesis cell.

Both are checked here on a hand-written device two tiles big, so nothing
external is needed: tileverilog must instantiate each OSERDESE2 under the half
its FASM names, and lvs_equiv's census must find both.  The proof itself is
not the point and is not graded -- the gold netlist has no register for it to
compare -- only the census, which is printed whatever the proof says.

Usage: test_sing_site.py --tileverilog <exe> --lvs-equiv <exe>
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

TOP, BOTTOM = 'RIOI3_SING_X43Y49', 'RIOI3_SING_X43Y0'
DEVICE = 'device0'

# The OLOGIC site's ports the OSERDESE2 cut reads, and the wire each lands
# on, spelled as the real tile_type_RIOI3_SING.json spells them.
SITE_WIRES = {p: 'IOI_OLOGIC0_' + p for p in
        ['D1', 'D2', 'D3', 'D4', 'D5', 'D6', 'D7', 'D8', 'T1', 'T2', 'T3', 'T4',
         'OCE', 'TCE', 'CLK', 'CLKDIV', 'SR']}
SITE_WIRES['OQ'] = 'RIOI_OLOGIC0_OQ'
SITE_WIRES['TQ'] = 'RIOI_OLOGIC0_TQ'
# The interconnect wire the routing drives D1 from.  Routing D1 is what puts
# the site in the design, and it is also what lets the database's hardwired
# bypass (OQ from D1) apply -- the wire a missed OSERDESE2 is extracted as.
IMUX = 'IOI_IMUX34_0'

# What nextpnr writes for a LiteX DDR3 output: the two halves differ in the
# site name and in nothing else.
SERDES_FEATURES = [
    'ODDR.DDR_CLK_EDGE.SAME_EDGE', 'ODDR.SRUSED', 'OQUSED',
    'OSERDES.DATA_RATE_OQ.DDR', 'OSERDES.DATA_RATE_TQ.BUF',
    'OSERDES.DATA_WIDTH.DDR.W8', 'OSERDES.IN_USE', 'OSERDES.SRTYPE.SYNC',
    'ZINIT_OQ', 'ZINV_CLK', 'ZSRVAL_OQ',
]


def write_json(path, value):
    with open(path, 'w') as f:
        json.dump(value, f, indent=1)


def build_fixture(d):
    fam = os.path.join(d, 'db')
    os.makedirs(os.path.join(fam, DEVICE))
    write_json(os.path.join(fam, 'tile_type_RIOI3_SING.json'), {
        'tile_type': 'RIOI3_SING',
        'wires': {w: None for w in list(SITE_WIRES.values()) + [IMUX]},
        'pips': {},
        'sites': [{
            'type': 'OLOGICE3', 'prefix': 'OLOGIC', 'name': 'X0Y0',
            'x_coord': 0, 'y_coord': 0,
            'site_pins': {p: {'wire': w} for p, w in SITE_WIRES.items()},
        }],
    })
    write_json(os.path.join(fam, DEVICE, 'tilegrid.json'), {
        TOP: {'type': 'RIOI3_SING', 'grid_x': 113, 'grid_y': 105,
              'sites': {'OLOGIC_X1Y49': 'OLOGICE3'}},
        BOTTOM: {'type': 'RIOI3_SING', 'grid_x': 113, 'grid_y': 155,
                 'sites': {'OLOGIC_X1Y0': 'OLOGICE3'}},
    })
    write_json(os.path.join(fam, 'tileconn.json'), [])
    # as ppips_rioi3_sing.db has it
    with open(os.path.join(fam, 'ppips_rioi3_sing.db'), 'w') as f:
        f.write('RIOI3_SING.RIOI_OLOGIC0_OQ.IOI_OLOGIC0_D1 always\n')

    with open(os.path.join(d, 'design.fasm'), 'w') as f:
        for tile, half in ((TOP, 'OLOGIC_Y1'), (BOTTOM, 'OLOGIC_Y0')):
            f.write('%s.%s.%s\n' % (tile, SITE_WIRES['D1'], IMUX))
            for feat in SERDES_FEATURES:
                f.write('%s.%s.%s\n' % (tile, half, feat))

    # The synthesis side: one OSERDESE2 per pad, placed where the FASM says.
    ports = ['D1', 'D2', 'D3', 'D4', 'D5', 'D6', 'D7', 'D8', 'OCE', 'RST']
    with open(os.path.join(d, 'gold.v'), 'w') as f:
        f.write('module top(input wire clk, input wire [9:0] a, input wire [9:0] b,\n'
                '           output wire reset_n, output wire we_n);\n')
        for cell, bus, q in (('ser_top', 'a', 'reset_n'), ('ser_bot', 'b', 'we_n')):
            conns = ', '.join('.%s(%s[%d])' % (p, bus, i) for i, p in enumerate(ports))
            f.write('  OSERDESE2 %s (%s, .CLK(clk), .CLKDIV(clk), .OQ(%s));\n'
                    % (cell, conns, q))
        f.write('endmodule\n')
    write_json(os.path.join(d, 'gold.json'), {'modules': {'top': {
        'attributes': {'top': '00000000000000000000000000000001'},
        'ports': {}, 'netnames': {},
        'cells': {'ser_top': {'type': 'OSERDESE2', 'connections': {}},
                  'ser_bot': {'type': 'OSERDESE2', 'connections': {}}},
    }}})
    write_json(os.path.join(d, 'placement.json'), {
        'ser_top': {'tile': TOP, 'site': 'OLOGIC_X1Y49', 'bel': 'OSERDESE2',
                    'type': 'OSERDESE2_OSERDESE2'},
        'ser_bot': {'tile': BOTTOM, 'site': 'OLOGIC_X1Y0', 'bel': 'OSERDESE2',
                    'type': 'OSERDESE2_OSERDESE2'},
    })
    return fam


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tileverilog', required=True)
    ap.add_argument('--lvs-equiv', required=True)
    args = ap.parse_args()

    failures = []
    with tempfile.TemporaryDirectory() as d:
        fam = build_fixture(d)
        fabric = os.path.join(d, 'fabric.v')
        tv = subprocess.run([args.tileverilog, '--fasm', os.path.join(d, 'design.fasm'),
                             '--db', fam, '--device', DEVICE, '--out', fabric,
                             '--model-out', os.path.join(d, 'tile_model.v')],
                            capture_output=True, text=True)
        if tv.returncode != 0:
            print(tv.stderr)
            print('FAIL: tileverilog exited %d' % tv.returncode)
            return 1
        text = open(fabric).read()

        # Each SING site is instantiated under the half its FASM names, and
        # its output is the primitive's, not the bypass wire past it.
        for tile, half in ((TOP, 'OLOGIC_Y1'), (BOTTOM, 'OLOGIC_Y0')):
            want = 'OSERDESE2 \\%s_%s (' % (tile, half)
            if want not in text:
                failures.append('fabric.v has no %s' % want.strip(' ('))
            bypass = 'assign %s_RIOI_OLOGIC0_OQ = %s_IOI_OLOGIC0_D1;' % (tile, tile)
            if bypass in text:
                failures.append('fabric.v still bypasses %s: %s' % (tile, bypass))

        eq = subprocess.run([args.lvs_equiv, '--gold', os.path.join(d, 'gold.v'),
                             '--gold-top', 'top', '--gate', fabric, '--gate-top', 'fabric',
                             '--placement', os.path.join(d, 'placement.json'),
                             '--gold-json', os.path.join(d, 'gold.json'),
                             '--db', fam, '--device', DEVICE, '--quiet'],
                            capture_output=True, text=True)
        out = eq.stdout
        if 'hard blocks: 2 placed by the synthesis' not in out:
            failures.append('lvs_equiv did not count the two OSERDESE2')
        if 'all present in the extraction' not in out:
            failures.append('the census does not find both SING OSERDESE2 in the extraction')

        if failures:
            print(tv.stderr)
            print(out)
            print(eq.stderr)

    for f in failures:
        print('FAIL: ' + f)
    if not failures:
        print('PASS: both SING OSERDESE2 extracted under their FASM half and paired')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
