#!/usr/bin/env python3
"""RTL -> bitstream -> extraction, once per cell family.

Every other test in this repo starts from a FASM someone else produced. This
one starts from Verilog: for each small design under designs/ it runs yosys,
then nextpnr-himbaechel to place, route and emit the FASM, then fasm2netlist
to extract the netlist back out of it. So the thing under test is the whole
round trip, on bits that were never seen before the run.

The designs are deliberately one-idea-each -- a wide accumulator, two shift
registers, a distributed RAM, a multiply, a block RAM -- because a design that
uses everything tells you only that something broke, while five that use one
thing each tell you which.

Grading is relational, in the same spirit as test_johnson_lvs.py: the counts
are checked against nextpnr's own placement dump from the same run, never
against constants recorded from an earlier one. A different place-and-route
result is not a failure; failing to account for the one you were given is.
For the families where a placed BEL and an extracted cell correspond one to
one -- CARRY4, the flip-flops, DSP48E1, the block RAMs -- that is an equality.
The LUT-ish BELs have no such correspondence (one LUT6_2 covers a column's two
LUT BELs, an SRL or a RAM covers others), so those are checked by site
recovery instead: every site nextpnr placed something in must appear in the
extraction.

Skips (exit 77) rather than failing when the external tools this needs are
absent: yosys, a built nextpnr-himbaechel, and a Project X-Ray database. None
of them is vendored here.

Environment / flags:
  YOSYS, NEXTPNR_BIN, PRJXRAY_DB    override tool and database locations
  --xc7-tools-dir                   sibling checkout to find nextpnr and the
                                    database under (default ~/xc7-bitstream-tools,
                                    or $XC7_BITSTREAM_TOOLS_DIR)
  --keep DIR                        keep the build products for inspection
"""
import argparse
import collections
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

SKIP = 77
HERE = os.path.dirname(os.path.abspath(__file__))
DESIGNS_DIR = os.path.join(HERE, 'designs')

# name -> the primitive the design exists to exercise. The extraction must
# produce at least one, or the design has stopped testing what it was written
# for -- a synthesis or packing change that quietly stops inferring the
# primitive would otherwise look like a pass.
DESIGNS = {
    'carry':  ['CARRY4'],
    # A column configured as 2x16 holds both halves' contents in one INIT, so
    # both are emitted even where the design only wired one up: whether the
    # second is reachable is a property of the routing, not of the bits. The
    # extra cell drives a net nothing reads.
    'srl':    ['SRLC32E', 'SRL16E'],
    'lutram': ['RAM32M'],
    'dsp':    ['DSP48E1'],
    'bram':   ['RAMB18E1'],
}

PART, DEVICE, FAMILY = 'xc7a35tcsg324-1', 'xc7a50t', 'artix7'

# nextpnr BEL type -> the extracted cell types that stand one-to-one with it.
EXACT = {
    'CARRY4': ('CARRY4',),
    'DSP48E1_DSP48E1': ('DSP48E1',),
    'RAMB18E1_RAMB18E1': ('RAMB18E1',),
    'RAMB36E1_RAMB36E1': ('RAMB36E1',),
    'SLICE_FFX': ('FDRE', 'FDSE', 'FDCE', 'FDPE', 'LDCE', 'LDPE'),
}


def skip(msg):
    print('SKIP: %s' % msg)
    sys.exit(SKIP)


def find_tool(env, default_names, search_dirs, what):
    if os.environ.get(env):
        p = os.environ[env]
        return p if (os.path.isabs(p) and os.access(p, os.X_OK)) or shutil.which(p) else None
    for name in default_names:
        p = shutil.which(name)
        if p:
            return p
    for d in search_dirs:
        for name in default_names:
            p = os.path.join(d, name)
            if os.access(p, os.X_OK):
                return p
    return None


# Cells whose INIT parameters are memory CONTENTS rather than a truth table.
# LUTs are deliberately absent: yosys emits LUT1..LUT6 where the extraction
# emits LUT6_2, so their INITs have no one-to-one correspondence to compare.
MEM_CELLS = {
    'RAMB18E1', 'RAMB36E1',
    'RAM32M', 'RAM64M',
    'RAM32X1S', 'RAM32X1D', 'RAM64X1S', 'RAM64X1D',
    'RAM128X1S', 'RAM128X1D', 'RAM256X1S',
    'SRLC32E', 'SRL16E', 'SRLC16E',
}
MEM_PARAM = re.compile(r'INIT(P?_[0-9A-F]{2}|_[A-D])?$')


def _as_int(value):
    """A yosys JSON parameter value as an integer, whatever shape it arrived in."""
    if isinstance(value, int):
        return value
    v = str(value).strip()
    if not v:
        return 0
    if set(v) <= set('01xz'):          # bit string, x/z read as 0
        return int(v.replace('x', '0').replace('z', '0'), 2)
    try:
        return int(v, 2)
    except ValueError:
        return 0


def mem_contents_gold(json_path, top):
    """Memory contents yosys put in, as a multiset of (cell type, param, value)."""
    out = collections.Counter()
    cells = json.load(open(json_path))['modules'][top]['cells']
    for c in cells.values():
        if c['type'] not in MEM_CELLS:
            continue
        for k, v in c.get('parameters', {}).items():
            if not MEM_PARAM.match(k):
                continue
            n = _as_int(v)
            if n:
                out[(c['type'], k, n)] += 1
    return out


def mem_contents_gate(verilog_path):
    """...and the same, read back out of the extracted netlist."""
    out = collections.Counter()
    head = re.compile(r'^\s{2}([A-Z][A-Z0-9_]*)\s+#\(')
    param = re.compile(r"\.(INIT(?:P?_[0-9A-F]{2}|_[A-D])?)\(\d+'h([0-9a-f]+)\)")
    for line in open(verilog_path):
        m = head.match(line)
        if not m or m.group(1) not in MEM_CELLS:
            continue
        for k, v in param.findall(line):
            n = int(v, 16)
            if n:
                out[(m.group(1), k, n)] += 1
    return out


def census(verilog_path):
    """Cell type -> count, and the set of sites the extraction names."""
    types = collections.Counter()
    sites = set()
    cell_re = re.compile(r'^\s{2}([A-Z][A-Z0-9_]*)\s.*\\(\S+)\s*\(')
    site_re = re.compile(r'_(SLICE_X\d+Y\d+|DSP48_X\d+Y\d+|RAMB18_X\d+Y\d+|RAMB36_X\d+Y\d+)')
    for line in open(verilog_path):
        m = cell_re.match(line)
        if not m:
            continue
        types[m.group(1)] += 1
        s = site_re.search(m.group(2))
        if s:
            sites.add(s.group(1))
    return types, sites


def run(cmd, log, **kw):
    with open(log, 'ab') as f:
        f.write(b'\n$ ' + ' '.join(cmd).encode() + b'\n')
        f.flush()
        return subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, **kw)


def build_one(name, work, tools, verbose):
    d = os.path.join(work, name)
    os.makedirs(d, exist_ok=True)
    log = os.path.join(d, 'build.log')
    src = os.path.join(DESIGNS_DIR, name + '.v')
    xdc = os.path.join(DESIGNS_DIR, 'arty.xdc')
    gold, fasm, place = (os.path.join(d, f) for f in ('gold.json', 'design.fasm', 'placement.json'))
    gates = os.path.join(d, 'gates.v')

    r = run([tools['yosys'], '-q', '-p',
             'synth_xilinx -flatten -abc9 -arch xc7 -top top; write_json %s' % gold, src], log)
    if r.returncode != 0:
        return name, 'FAIL', 'synthesis failed, see %s' % log, None

    r = run([tools['nextpnr'], '--device', PART, '-o', 'xdc=' + xdc, '--json', gold,
             '-o', 'fasm=' + fasm, '-o', 'placement=' + place, '--router', 'router2'], log)
    if r.returncode != 0:
        return name, 'FAIL', 'place and route failed, see %s' % log, None

    proc = subprocess.run([tools['fasm2netlist'], '--fasm', fasm, '--db', tools['db'],
                           '--family', FAMILY, '--device', DEVICE, '--out', gates],
                          capture_output=True, text=True)
    with open(log, 'a') as f:
        f.write(proc.stdout + proc.stderr)
    if proc.returncode != 0:
        return name, 'FAIL', 'extraction failed, see %s' % log, None

    warnings = [l for l in proc.stderr.splitlines() if l.startswith('warning:')]
    types, sites = census(gates)
    placed = json.load(open(place))
    problems = []

    # (1) the design still exercises what it was written for
    for want in DESIGNS[name]:
        if not types[want]:
            problems.append('no %s extracted' % want)

    # (2) exact counts for the one-to-one families
    by_type = collections.Counter(v['type'] for v in placed.values())
    for bel, cells in EXACT.items():
        want = by_type.get(bel, 0)
        got = sum(types[c] for c in cells)
        if want != got:
            problems.append('%s: nextpnr placed %d, extraction has %d %s'
                            % (bel, want, got, '/'.join(cells)))

    # (3) every site nextpnr used is named somewhere in the extraction
    fabric = {'CARRY4', 'SLICE_FFX', 'SLICE_LUTX', 'DSP48E1_DSP48E1',
              'RAMB18E1_RAMB18E1', 'RAMB36E1_RAMB36E1'}
    placed_sites = {v['site'] for v in placed.values() if v['type'] in fabric}
    missing = sorted(placed_sites - sites)
    if missing:
        problems.append('%d placed site(s) absent from the extraction, e.g. %s'
                        % (len(missing), ', '.join(missing[:3])))

    # (4) memory CONTENTS, not just cell counts. Every ROM/RAM/shift-register
    #     value yosys put in has to come back out of the bitstream: without
    #     this, a design whose block RAM held the wrong bytes would pass every
    #     check above, since the cell counts and sites would all still line up.
    #
    #     Directional on purpose. The extraction may legitimately hold MORE
    #     than the synthesis did: a SLICEM column configured as two 16-deep
    #     shift registers stores both halves in one LUT INIT, so the unused
    #     half comes back as a real cell driving a net nothing reads. Values
    #     going missing is the failure; extra ones are a note.
    gold_mem = mem_contents_gold(gold, 'top')
    gate_mem = mem_contents_gate(gates)
    lost = gold_mem - gate_mem
    if lost:
        ty, key, val = next(iter(lost))
        problems.append('%d memory value(s) did not survive to the extraction, '
                        'e.g. %s %s' % (sum(lost.values()), ty, key))
    extra = sum((gate_mem - gold_mem).values())
    mem_note = '%d/%d memory values round-tripped%s' % (
        sum(gold_mem.values()) - sum(lost.values()), sum(gold_mem.values()),
        '' if not extra else ' (+%d extra in the extraction)' % extra)

    # (5) a decoder that had to guess says so; treat that as a failure here,
    #     since these designs are small enough that nothing should be unclear
    for w in warnings:
        problems.append(w)

    summary = ' '.join('%s=%d' % kv for kv in sorted(types.items()))
    if gold_mem:
        summary += '  |  ' + mem_note
    if verbose:
        print('  %-10s log: %s' % (name, log))
    return name, ('OK' if not problems else 'FAIL'), '; '.join(problems), summary


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True, help='path to the fasm2netlist binary')
    ap.add_argument('--xc7-tools-dir',
                    default=os.environ.get('XC7_BITSTREAM_TOOLS_DIR',
                                           os.path.expanduser('~/xc7-bitstream-tools')))
    ap.add_argument('--keep', help='keep build products in this directory')
    ap.add_argument('--designs', nargs='*', default=sorted(DESIGNS))
    ap.add_argument('-v', '--verbose', action='store_true')
    args = ap.parse_args()

    xc7 = args.xc7_tools_dir
    yosys = find_tool('YOSYS', ['yosys'],
                      [os.path.expanduser('~/.apio/packages/oss-cad-suite/bin')], 'yosys')
    if not yosys:
        skip('yosys not found (set YOSYS, or put it on PATH)')
    nextpnr = find_tool('NEXTPNR_BIN', ['nextpnr-himbaechel'],
                        [os.path.join(xc7, 'build')], 'nextpnr-himbaechel')
    if not nextpnr:
        skip('nextpnr-himbaechel not found -- build it in %s (set NEXTPNR_BIN to override)' % xc7)
    db = os.environ.get('PRJXRAY_DB', os.path.join(xc7, '.deps', 'prjxray-db'))
    if not os.path.isdir(os.path.join(db, FAMILY, DEVICE)):
        skip('no Project X-Ray database for %s/%s under %s' % (FAMILY, DEVICE, db))

    tools = {'yosys': yosys, 'nextpnr': nextpnr, 'db': db, 'fasm2netlist': args.exe}
    print('yosys:   %s' % yosys)
    print('nextpnr: %s' % nextpnr)
    print('db:      %s' % db)
    print()

    work = args.keep or tempfile.mkdtemp(prefix='f2n-rtl-')
    os.makedirs(work, exist_ok=True)
    rows, failed = [], 0
    print('%-10s %-6s %s' % ('DESIGN', 'RESULT', 'NOTE'))
    print('-' * 78)
    for name in args.designs:
        if name not in DESIGNS:
            skip('unknown design %r' % name)
        name, result, note, summary = build_one(name, work, tools, args.verbose)
        if result != 'OK':
            failed += 1
        print('%-10s %-6s %s' % (name, result, note or summary or ''))
        rows.append((name, result))

    print()
    print('%d of %d designs round-tripped RTL -> bitstream -> netlist' %
          (len(rows) - failed, len(rows)))
    if args.keep:
        print('build products kept in %s' % work)
    elif failed:
        print('build products in %s (not cleaned up, since something failed)' % work)
    else:
        shutil.rmtree(work, ignore_errors=True)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
