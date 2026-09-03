// Per-family cell extraction. Each of these turns the FASM features of one
// kind of site into instantiated Xilinx primitives, using only the fixed
// prjxray tables for connectivity (via FasmDesign) -- never a placement or
// routed-JSON dump.
#pragma once

#include "netlist_core.hpp"

namespace f2n {

// SLICEL/SLICEM: LUT6_2, FDRE/FDSE/FDCE/FDPE/LDCE/LDPE, CARRY4, MUXF7/MUXF8,
// SRLC32E/SRL16E/SRLC16E and the distributed-RAM family.
void emitSliceCells(FasmDesign& fd, Netlist& nl);

// DSP_L/DSP_R: DSP48E1.
void emitDspCells(FasmDesign& fd, Netlist& nl);

// BRAM_L/BRAM_R: RAMB18E1 and RAMB36E1.
void emitBramCells(FasmDesign& fd, Netlist& nl);

}  // namespace f2n
