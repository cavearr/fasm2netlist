#!/usr/bin/env python3
"""IOB-site seeding keeps input pads, without naming the receive side of outputs.

7ea8941 seeds every IOB site's own wires so a Vivado bitstream's input pad --
which records only the IOB site features, not the IOB-to-IBUF hops -- still
enters the net graph and can be labelled from the XDC.  The same seed puts
the receive side of an output-only pad in the graph.  Labelling both I and O
from the XDC then makes every output look bidirectional, and the split that
follows takes the name off the side the design drives: LVS reports
`skipped <port> -- the gate has no such port` for every output, which is how
the Arty S7 demos went green with 0 of 33 outputs compared.

The seed stays.  Direction comes from the FASM (DRIVE/.OUT vs .IN/.IN_ONLY).
This fixture is two IOB tiles and nothing else:

  - an INPUT pad in the Vivado style: IOB site features only, no routing PIP.
    Seeding must still bring its I wire into the graph and the XDC must name it.
  - an OUTPUT pad whose FASM uses the SING spelling IOB_Y1 on a tile type that
    spells the one site IOB_Y0.  The XDC name must stay on the drive side, not
    become an inout, and the receive side must still be in the graph (seeded,
    just not labelled).

Usage: test_iob_pad_direction.py --tileverilog <exe>
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

INP, OUTP = 'LIOB33_X0Y10', 'LIOB33_X0Y20'
DEVICE = 'device0'
PART = 'xc7test'


def write_json(path, value):
    with open(path, 'w') as f:
        json.dump(value, f, indent=1)


def build_fixture(d):
    fam = os.path.join(d, 'db')
    os.makedirs(os.path.join(fam, DEVICE))
    os.makedirs(os.path.join(fam, PART))
    write_json(os.path.join(fam, 'tile_type_TESTIOB.json'), {
        'tile_type': 'TESTIOB',
        'wires': {'IOB_IBUF0': None, 'IOB_O0': None, 'IOB_PADOUT0': None, 'IMUX': None},
        'pips': {},
        'sites': [{
            'type': 'IOB33', 'prefix': 'IOB', 'name': 'X0Y0',
            'x_coord': 0, 'y_coord': 0,
            'site_pins': {
                'I': {'wire': 'IOB_IBUF0'},
                'O': {'wire': 'IOB_O0'},
            },
        }],
    })
    write_json(os.path.join(fam, DEVICE, 'tilegrid.json'), {
        INP: {'type': 'TESTIOB', 'grid_x': 0, 'grid_y': 10,
              'sites': {'IOB_X0Y10': 'IOB33'}},
        OUTP: {'type': 'TESTIOB', 'grid_x': 0, 'grid_y': 20,
               'sites': {'IOB_X0Y20': 'IOB33'}},
    })
    write_json(os.path.join(fam, 'tileconn.json'), [])
    # A hardwired hop from the IBUF wire, so a seeded I endpoint enters the DSU
    # the way IOB-to-IOI tileconn does on a real device.  Without the seed the
    # hop never fires -- neither end is known -- which is the Vivado case.
    with open(os.path.join(fam, 'ppips_testiob.db'), 'w') as f:
        f.write('TESTIOB.IOB_PADOUT0.IOB_IBUF0 always\n')

    with open(os.path.join(d, 'design.fasm'), 'w') as f:
        # Input: site features only, no routing.  .IN marks the IBUF.
        f.write('%s.IOB_Y0.LVCMOS25_LVCMOS33_LVTTL.IN\n' % INP)
        f.write('%s.IOB_Y0.PULLTYPE.NONE\n' % INP)
        # Output: fabric drives O, DRIVE marks the OBUF.  IOB_Y1 is the SING
        # spelling nextpnr writes above an HCLK row; the type says IOB_Y0.
        f.write('%s.IOB_O0.IMUX\n' % OUTP)
        f.write('%s.IOB_Y1.LVCMOS33.DRIVE.I16\n' % OUTP)

    with open(os.path.join(d, 'design.xdc'), 'w') as f:
        f.write('set_property PACKAGE_PIN A1 [get_ports rst]\n')
        f.write('set_property PACKAGE_PIN A2 [get_ports led]\n')
    with open(os.path.join(fam, PART, 'package_pins.csv'), 'w') as f:
        f.write('pin,bank,site,tile,pin_function\n')
        f.write('A1,15,IOB_X0Y10,%s,IO\n' % INP)
        f.write('A2,15,IOB_X0Y20,%s,IO\n' % OUTP)
    return fam


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tileverilog', required=True)
    args = ap.parse_args()

    failures = []
    with tempfile.TemporaryDirectory() as d:
        fam = build_fixture(d)
        fabric = os.path.join(d, 'fabric.v')
        tv = subprocess.run(
            [args.tileverilog, '--fasm', os.path.join(d, 'design.fasm'),
             '--db', fam, '--device', DEVICE, '--part', PART,
             '--xdc', os.path.join(d, 'design.xdc'),
             '--out', fabric, '--model-out', os.path.join(d, 'tile_model.v')],
            capture_output=True, text=True)
        err = tv.stderr
        if tv.returncode != 0:
            print(err)
            print('FAIL: tileverilog exited %d' % tv.returncode)
            return 1
        text = open(fabric).read()

        if 'XDC: labelled 2 of 2 pads' not in err:
            failures.append('expected 2 labelled pads, stderr was:\n' + err)
        if 'bidirectional pad' in err:
            failures.append('output-only pad became bidirectional:\n' + err)

        if 'output wire \\led ' not in text:
            failures.append('fabric.v has no output port named led')
        if 'input wire \\rst ' not in text:
            failures.append('fabric.v has no input port named rst '
                            '(input pad was not seeded/labelled)')
        if 'inout wire' in text:
            failures.append('fabric.v declared an inout; the output lost its name')

        # The receive side of the output pad is still in the graph -- seeded,
        # just not labelled -- so the seed is doing the work 7ea8941 added.
        seeded_receive = '%s_IOB_IBUF0' % OUTP
        if seeded_receive not in text:
            failures.append('output pad receive wire %s missing: seed was lost'
                            % seeded_receive)

        if failures:
            print(err)
            print('----- fabric.v -----')
            print(text)

    for f in failures:
        print('FAIL: ' + f)
    if not failures:
        print('PASS: input pad seeded and named, output keeps the drive-side name')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
