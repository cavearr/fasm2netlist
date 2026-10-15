#!/bin/bash
# vivado_oracle.sh GOLD.json TOP XDC OUTDIR [PART] -- a Vivado reference layout of a yosys netlist, extracted and proved against it.
#
# The open flow's own layout (nextpnr) is what lvs_equiv normally checks.  When it reports a difference, the question is whether the
# extraction or the layout is wrong.  Vivado placing and routing the SAME yosys netlist, then taking its bitstream through the SAME
# extraction, answers it: a difference that survives is the extraction's (or the netlist's), not nextpnr's.  Synthesis is held fixed:
# the netlist goes in as EDIF and is linked, not re-synthesised.
#
#   yosys -> EDIF -> Vivado place+route+bitstream -> placement dump -> bit2fasm -> tileverilog -> lvs_equiv
#
# Environment: YOSYS, VIVADO, PRJXRAY (checkout with build/tools/bitread), DB (database/virtex7), FASM2NETLIST_BUILD.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
gold=$(readlink -f "$1"); top=$2; xdc=$(readlink -f "$3"); out=$4; part=${5:-xc7vx485tffg1761-2}
YOSYS=${YOSYS:-$HOME/xc7-bitstream-tools/yosys-install/bin/yosys}
VIVADO=${VIVADO:-/opt/Xilinx/Vivado/2020.1/bin/vivado}
PRJXRAY=${PRJXRAY:-$HOME/prjxray}
DB=${DB:-$PRJXRAY/database/virtex7}
B=${FASM2NETLIST_BUILD:-$HERE/../build}
dev=${DEVICE:-xc7vx485t}
mkdir -p "$out"; out=$(readlink -f "$out"); cd "$out"

step() { echo "=== $*"; }
step "EDIF from the yosys netlist"
# hilomap and $scopeinfo/$print deletion are required: Vivado rejects constant nodes and an undefined $scopeinfo cell.
$YOSYS -q -p "read_json $gold; hierarchy -top $top; hilomap -hicell VCC P -locell GND G; delete t:\$scopeinfo; delete t:\$print; write_edif -pvector bra $top.edif" > yosys_edif.log 2>&1

# yosys writes VCC and GND as `(rename idNNNNN "VCC")` and instantiates them by that id, which Vivado cannot resolve to its primitives.
python3 - "$top.edif" <<'PY'
import re, sys
t = open(sys.argv[1]).read()
for m in list(re.finditer(r'\(cell \(rename (id\d+) "(VCC|GND)"\)', t)):
    start = m.start()
    depth, i = 0, start
    while True:                                  # drop the whole duplicate definition by matching parentheses
        depth += (t[i] == "(") - (t[i] == ")")
        i += 1
        if depth == 0: break
    t = t[:start] + t[i:]
    t = t.replace("cellRef " + m.group(1) + " ", "cellRef " + m.group(2) + " ")
open(sys.argv[1], "w").write(t)
PY

step "Vivado place and route"
$VIVADO -mode batch -nojournal -log vivado_pnr.log -source "$HERE/vivado_pnr_edif.tcl" \
        -tclargs "$out/$top.edif" "$xdc" "$out/viv" "$top" "$part" > /dev/null 2>&1 || true
grep -q NETLIST_PNR_DONE vivado_pnr.log || { echo "Vivado did not finish; see $out/vivado_pnr.log"; exit 2; }

step "placement of every primitive"
$VIVADO -mode batch -nojournal -log vivado_place.log -source "$HERE/vivado_placement.tcl" -tclargs "$out/viv/routed.dcp" "$out/place.tsv" > /dev/null 2>&1
python3 "$HERE/vivado_place2json.py" place.tsv "$DB/$dev/tilegrid.json" placement.json

step "bitstream -> FASM"
PYTHONPATH=$PRJXRAY python3 $PRJXRAY/utils/bit2fasm.py --db-root "$DB" --part "$dev" --bitread $PRJXRAY/build/tools/bitread viv/design.bit > design.fasm 2> bit2fasm.log

step "extract and prove"
$B/tileverilog --fasm design.fasm --db "$DB" --device $dev --xdc "$xdc" --part "$part" --out fabric.v --model-out tile_model.v > tv.log 2>&1
$YOSYS -q -p "read_json $gold; hierarchy -top $top; splitnets; select $top; write_verilog -noattr -norename -selected gold.v" > gold.log 2>&1
$B/lvs_equiv --gold gold.v --gold-top $top --gate fabric.v --gate-top fabric --placement placement.json --gold-json "$gold" \
             --db "$DB" --device $dev --quiet > lvs.log 2>&1 && rc=0 || rc=$?
grep -E "proved,|solver time|hard blocks" lvs.log
grep DIFFER lvs.log | head -20 | cut -c1-150
echo "oracle exit $rc"
exit $rc
