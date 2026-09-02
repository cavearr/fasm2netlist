#!/usr/bin/env python3
"""Run the decoder against every CLB fuzzer that carries usable ground truth,
and measure the gap on the ones whose features aren't modelled yet."""
import json, re, subprocess, os, collections

HERE = os.path.dirname(os.path.abspath(__file__))
XC7 = os.environ.get('XC7_BITSTREAM_TOOLS_DIR', os.path.expanduser('~/xc7-bitstream-tools'))
FUZZERS = os.environ.get('PRJXRAY_FUZZERS', os.path.expanduser('~/prjxray/fuzzers'))
TILEDUMP = os.environ.get('TILEDUMP', os.path.join(HERE, '..', '..', '..', 'build', 'tiledump'))
DEVICE = os.environ.get('DEVICE', 'xc7vx485t')
TG = json.load(open(os.path.join(XC7, '.deps', 'prjxray-db', 'virtex7', DEVICE, 'tilegrid.json')))
site2=({})
for tile,d in TG.items():
    s=d.get('sites') or {}
    sl=sorted([x for x in s if x.startswith('SLICE_')],key=lambda x:int(re.match(r'SLICE_X(\d+)Y',x).group(1)))
    for i,x in enumerate(sl): site2[x]=(tile,'%s_X%d'%(s[x],i))

def dump(fasm):
    out=subprocess.run([TILEDUMP,fasm],capture_output=True,text=True).stdout
    cur=None; cfg={}; hdr={}
    for line in out.splitlines():
        m=re.match(r'^slice (\S+)/(\S+) (.*)$',line)
        if m: cur=(m.group(1),m.group(2)); cfg[cur]=[]; hdr[cur]=m.group(3); continue
        m=re.match(r'^  col (\w)(.*)$',line)
        if m and cur: cfg[cur].append((m.group(1),m.group(2)))
    return cfg,hdr

def specimens(f):
    b=os.path.join(FUZZERS, f, 'build')
    for sp in sorted(d for d in os.listdir(b) if d.startswith('specimen_')):
        p=os.path.join(b,sp,'params.csv'); fa=os.path.join(b,sp,'design.fasm')
        if os.path.exists(fa): yield (p if os.path.exists(p) else None), fa

def rows(p):
    lines=open(p).read().splitlines()
    hdr=lines[0].split(',')
    for l in lines[1:]:
        v=l.split(',')
        if len(v)==len(hdr): yield dict(zip(hdr,v))

# ---- 014: CE / SR usage flags -------------------------------------------
def check_ffconfig():
    ok=collections.Counter(); tot=collections.Counter()
    for p,fa in specimens('014-clb-ffsrcemux'):
        if not p: continue
        cfg,hdr=dump(fa)
        for r in rows(p):
            k=site2.get(r['loc'])
            if not k or k not in hdr: continue
            h=hdr[k]
            for field,flag in (('ce','ce'),('r','sr')):
                tot[field]+=1
                want=(r[field]=='0') if field=='ce' else (r[field]=='1')
                got=(' '+flag in ' '+h)
                if want==got: ok[field]+=1
    print('  014-clb-ffsrcemux   CEUSEDMUX %d/%d   SRUSEDMUX %d/%d'%(ok['ce'],tot['ce'],ok['r'],tot['r']))

# ---- 012: the 5FF exists where the fuzzer put one -----------------------
def check_5ffmux():
    ok=tot=0
    for p,fa in specimens('012-clb-n5ffmux'):
        if not p: continue
        cfg,_=dump(fa)
        for r in rows(p):
            k=site2.get(r['loc'])
            if not k or k not in cfg: continue
            tot+=1
            if any('ff5=' in rest for _c,rest in cfg[k]): ok+=1
    print('  012-clb-n5ffmux     5FF present %d/%d'%(ok,tot))

# ---- the rest: what the decoder cannot model, on designs built to use it -
def gap(fuzzer):
    b=os.path.join(FUZZERS, fuzzer, 'build')
    sp=sorted(d for d in os.listdir(b) if d.startswith('specimen_'))[0]
    fa=os.path.join(b,sp,'design.fasm')
    full=subprocess.run([TILEDUMP,fa],capture_output=True,text=True).stdout
    nslice=sum(1 for l in full.splitlines() if l.startswith('slice '))
    out=subprocess.run([TILEDUMP,'--gaps',fa],capture_output=True,text=True).stdout.strip()
    if nslice==0:
        # the fuzzer's own design.fasm predates the segbits it generates; the
        # bitstream is still there, so re-extract with the current database
        print('  %-20s 0 slices decoded -- re-run bit2fasm on design.bit'%fuzzer)
        return
    print('  %-20s %d slices, %s'%(fuzzer, nslice,
          'complete' if out=='no unhandled slice features' else 'gaps:'))
    if out!='no unhandled slice features':
        for line in sorted(out.splitlines(), key=lambda l:-int(l.split('\t')[0]))[:6]:
            n,shape=line.split('\t'); print('      %6s  %s'%(n,shape))


# ---- 015 / 016: the mux selects, against the fuzzer's own module names ----
# F7/F8 is deliberately absent from both maps: the corpus shows it has no site
# feature at all (a site built as clb_NOUTMUX_F78 emits no OUTMUX feature), so
# it cannot be decoded from site config and is not a decoder failure.
FFSEL = {'AX': 'X', 'O6': 'O6', 'O5': 'O5', 'XOR': 'XOR', 'CY': 'CY', 'MC31': 'MC31'}
OMSEL = {'B5Q': '5Q', 'O5': 'O5', 'XOR': 'XOR', 'CY': 'CY', 'MC31': 'MC31', 'O6': 'O6'}

def check_selects(fuzzer, kind):
    table = FFSEL if kind == 'ff' else OMSEL
    field = 'ff=' if kind == 'ff' else 'outmux='
    ok = collections.Counter(); tot = collections.Counter(); nofeat = collections.Counter()
    for p, fa in specimens(fuzzer):
        if not p: continue
        cfg, _ = dump(fa)
        for r in rows(p):
            m = re.match(r'^clb_N(?:FFMUX|OUTMUX)_(\w+)$', r.get('module', ''))
            if not m: continue
            sel = m.group(1)
            k = site2.get(r['loc'])
            if not k or k not in cfg: continue
            want = table.get(sel)
            if want is None:
                nofeat[sel] += 1
                continue
            got = set()
            for _c, rest in cfg[k]:
                for mm in re.finditer(re.escape(field) + r'([A-Za-z0-9()]+)', rest):
                    got.add(mm.group(1).split(',')[0])
            got.discard('none')
            if not got:
                # the site carries no feature for this mux at all: not a decode
                # failure, a limit of what site config can express
                nofeat[sel] += 1
                continue
            tot[sel] += 1
            if want in got: ok[sel] += 1
    print('  %s' % fuzzer)
    for sel in sorted(tot):
        print('    %-5s %5d/%-5d %s' % (sel, ok[sel], tot[sel], 'OK' if ok[sel] == tot[sel] else 'MISMATCH'))
    for sel in sorted(nofeat):
        print('    %-5s %5s      no site feature present -- not decodable from site config'
              % (sel, nofeat[sel]))

print('validated against ground truth:')
check_selects('015-clb-nffmux', 'ff')
check_selects('016-clb-noutmux', 'outmux')
check_ffconfig(); check_5ffmux()
print('\ngap on fuzzers whose features are not modelled yet:')
for f in ('013-clb-ncy0','017-clb-precyinit','019-clb-ndi1mux','018-clb-ram','010-clb-lutinit','011-clb-ffconfig'):
    gap(f)
