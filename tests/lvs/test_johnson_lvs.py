#!/usr/bin/env python3
"""LVS-style regression test: run fasm2netlist against the real VC707
Johnson-counter example from the sibling xc7-bitstream-tools checkout, and
cross-check its extraction -- both cell counts and per-cell name matching --
against nextpnr's own placement dump for that exact design.

This is deliberately an *external* ground truth: examples/vc707-johnson/
johnson.fasm and johnson_placement.json come from the SAME
`make vc707-johnson` nextpnr-himbaechel run in xc7-bitstream-tools (see its
top-level Makefile), so johnson_placement.json names, for every SLICE LUT/FF
nextpnr actually placed, the exact (tile, site, bel) it landed on -- entirely
independent of fasm2netlist's own reconstruction. Comparing against it is
the closest this repo can get to a real LVS ("layout versus schematic")
check without vendoring a multi-gigabyte prjxray-db checkout.

What's checked:
  - FF count: fasm2netlist must recover exactly the 36 flip-flops nextpnr
    placed (8-bit "johnson" register + 28-bit "prbs" LFSR) -- an exact
    match, matching the count bit2gates.py's own header comment records as
    independently verified against this same placement ground truth.
  - LUT6_2 count: fasm2netlist's count is a strict SUPERSET of nextpnr's 11
    placed LUT columns -- by design (see main.cpp's comment on "uncelled
    route-throughs"): a real, bitstream-configured LUT INIT with no
    corresponding placed logical cell is still real silicon and gets
    emitted. The extra count is pinned to a recorded baseline so a
    regression (a silently dropped or newly-invented route-through) still
    fails the test.
  - Name matching: fasm2netlist names every LUT/FF cell purely from its
    physical (tile, site, column) -- see main.cpp's uniqName() calls. For
    each of nextpnr's 36 placed FF cells and 11 placed LUT columns, this
    recomputes that same name from the placement ground truth and asserts
    it appears in fasm2netlist's output. This is exactly the "structural
    signature matching" the top-level README calls a downstream problem --
    here, with the placement as an oracle, it doubles as a correctness
    check on fasm2netlist's own naming convention.

Skips (exit 77 -> ctest SKIPPED) if the sibling xc7-bitstream-tools checkout,
its prjxray-db, or its vc707-johnson example artifacts aren't present --
none of those are vendored into this repo.
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
LUT_BEL_RE = re.compile(r'^([ABCD])[56]?LUT$')


def vname(s):
    # matches fasm2netlist's src/main.cpp vname() exactly.
    return re.sub(r'[^A-Za-z0-9_]', '_', s)


def skip(msg):
    print('SKIP: %s' % msg)
    sys.exit(SKIP)


def expected_ff_name(tile, site, col, is5):
    return 'ff_%s_%s_%s%s' % (vname(tile), site, col, '5' if is5 else '')


def expected_lut_name(tile, site, col):
    return 'lut_%s_%s_%s' % (vname(tile), site, col)


def load_placements(placement_path):
    placement = json.load(open(placement_path))
    ffs = []   # (tile, site, col, is5)
    luts = set()  # (tile, site, col)
    for _cellname, info in placement.items():
        tile, site, bel, ctype = info['tile'], info['site'], info['bel'], info['type']
        if ctype == 'SLICE_FFX':
            m = FF_BEL_RE.match(bel)
            if not m:
                raise ValueError('unrecognised FF bel in placement: %s' % bel)
            ffs.append((tile, site, m.group(1), bool(m.group(2))))
        elif ctype == 'SLICE_LUTX':
            m = LUT_BEL_RE.match(bel)
            if not m:
                raise ValueError('unrecognised LUT bel in placement: %s' % bel)
            luts.add((tile, site, m.group(1)))
    return ffs, luts


def parse_gate_netlist(path):
    src = open(path).read()
    ff_names = set(re.findall(r'\b(?:FDRE|FDSE|FDCE|FDPE)\b[^\n]*\\(\S+)\s*\(', src))
    lut_names = set(re.findall(r'\bLUT6_2\b[^\n]*\\(\S+)\s*\(', src))
    return ff_names, lut_names


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--exe', required=True, help='path to the built fasm2netlist binary')
    ap.add_argument('--xc7-tools-dir',
                     default=os.environ.get('XC7_BITSTREAM_TOOLS_DIR',
                                             os.path.expanduser('~/xc7-bitstream-tools')),
                     help='sibling xc7-bitstream-tools checkout (default: $XC7_BITSTREAM_TOOLS_DIR '
                          'or ~/xc7-bitstream-tools)')
    ap.add_argument('--family', default='virtex7')
    ap.add_argument('--device', default='xc7vx485t')
    # Recorded baseline (see module docstring): real Johnson-counter FASM
    # currently decodes to 28 LUT6_2 instances -- 11 "real" placed columns
    # plus 17 uncelled route-throughs. A change here is worth a human look.
    ap.add_argument('--expected-lut-total', type=int, default=28)
    args = ap.parse_args()

    xc7 = args.xc7_tools_dir
    db = os.path.join(xc7, '.deps', 'prjxray-db')
    example_dir = os.path.join(xc7, 'examples', 'vc707-johnson')
    fasm_path = os.path.join(example_dir, 'johnson.fasm')
    placement_path = os.path.join(example_dir, 'johnson_placement.json')
    tilegrid_path = os.path.join(db, args.family, args.device, 'tilegrid.json')

    for path, what in ((db, 'prjxray-db checkout'), (fasm_path, 'johnson.fasm'),
                        (placement_path, 'johnson_placement.json'), (tilegrid_path, 'tilegrid.json')):
        if not os.path.exists(path):
            skip('%s not found at %s -- xc7-bitstream-tools is an external, un-vendored checkout; '
                 'run `make vc707-johnson PRJXRAY_DB=...` there first (see its README)' % (what, path))

    ffs, luts = load_placements(placement_path)
    if len(ffs) == 0 or len(luts) == 0:
        skip('johnson_placement.json has no SLICE_FFX/SLICE_LUTX entries -- stale or unexpected file')

    with tempfile.TemporaryDirectory() as tmp:
        out_v = os.path.join(tmp, 'johnson_gates.v')
        proc = subprocess.run(
            [args.exe, '--fasm', fasm_path, '--db', db, '--family', args.family, '--device', args.device,
             '--out', out_v, '--module', 'johnson_lvs'],
            capture_output=True, text=True)
        print(proc.stdout, end='')
        print(proc.stderr, end='', file=sys.stderr)
        assert proc.returncode == 0, 'fasm2netlist exited %d' % proc.returncode

        m = re.search(r'(\d+) LUT6_2, (\d+) FF', proc.stderr)
        assert m, 'could not find "N LUT6_2, M FF" summary in fasm2netlist stderr'
        reported_lut, reported_ff = int(m.group(1)), int(m.group(2))

        ff_names, lut_names = parse_gate_netlist(out_v)

    # ---- counts ----
    assert len(ffs) == 36, 'ground-truth placement has %d FFs, expected 36 -- example changed?' % len(ffs)
    assert reported_ff == len(ffs) == len(ff_names), (
        'FF count mismatch: fasm2netlist reported %d, emitted %d named FDxx cells, '
        'nextpnr placed %d' % (reported_ff, len(ff_names), len(ffs)))

    assert len(luts) == 11, 'ground-truth placement has %d LUT columns, expected 11 -- example changed?' % len(luts)
    assert reported_lut == len(lut_names) == args.expected_lut_total, (
        'LUT6_2 count mismatch: fasm2netlist reported %d, emitted %d named LUT6_2 cells, '
        'expected recorded baseline %d' % (reported_lut, len(lut_names), args.expected_lut_total))
    assert reported_lut >= len(luts), (
        'fasm2netlist emitted fewer LUT6_2 (%d) than nextpnr placed columns (%d) -- '
        'real placed logic went missing' % (reported_lut, len(luts)))

    # ---- name matching: every ground-truth placed cell must be recoverable
    # by name from fasm2netlist's purely tile/site/column-derived naming ----
    missing_ff = []
    for tile, site, col, is5 in ffs:
        name = expected_ff_name(tile, site, col, is5)
        if name not in ff_names:
            missing_ff.append((tile, site, col, is5, name))
    assert not missing_ff, 'FF cells placed by nextpnr but not name-matched in fasm2netlist output:\n' + '\n'.join(
        '  %s/%s col %s%s -> expected cell name %r' % (t, s, c, '5' if i5 else '', n)
        for t, s, c, i5, n in missing_ff)

    missing_lut = []
    for tile, site, col in luts:
        name = expected_lut_name(tile, site, col)
        if name not in lut_names:
            missing_lut.append((tile, site, col, name))
    assert not missing_lut, 'LUT columns placed by nextpnr but not name-matched in fasm2netlist output:\n' + '\n'.join(
        '  %s/%s col %s -> expected cell name %r' % (t, s, c, n) for t, s, c, n in missing_lut)

    print('test_johnson_lvs: OK -- %d/%d FF and %d/%d placed-LUT-column names matched '
          '(%d total LUT6_2 incl. route-throughs)' % (
              len(ffs), len(ff_names), len(luts), reported_lut, reported_lut))


if __name__ == '__main__':
    main()
