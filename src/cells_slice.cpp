// SLICEL/SLICEM extraction: the LUT/FF pair this tool started with, plus the
// four things a SLICE column can be that are not a LUT -- part of the carry
// chain, part of a wide mux, a shift register, or distributed RAM.
//
// The whole slice is decoded as one unit because the modes interlock: a
// column whose LUT is in RAM or SRL mode is not a LUT6_2 at all, a carry
// chain cannot coexist with either, and the F7/F8 muxes are either cells in
// their own right or are swallowed by a RAM128/RAM256 spanning columns.
// Deciding those one column at a time is how you end up emitting a LUT6_2
// whose INIT is really shift-register contents.
//
// Feature semantics are prjxray's own, cross-checked against its fuzzers
// (010-clb-lutinit, 012-clb-n5ffmux, 013-clb-ncy0, 015-clb-nffmux,
// 016-clb-noutmux, 017-clb-precyinit, 018-clb-ram, 019-clb-ndi1mux): they
// describe the silicon, not any particular build.
#include "cells.hpp"

#include <algorithm>
#include <array>
#include <set>

namespace f2n {

namespace {

const char kCols[] = "ABCD";

std::string concat(const std::vector<std::string>& msbFirst) {
	std::string s = "{";
	for (size_t i = 0; i < msbFirst.size(); i++) {
		if (i) s += ", ";
		s += msbFirst[i];
	}
	return s + "}";
}

bool isLiteral(const std::string& n) { return n.rfind("1'b", 0) == 0; }

// One SLICE site: its features, its pin->net resolution, and the internal
// nets its cells hand to one another.
struct Slice {
	FasmDesign& fd;
	Netlist& nl;
	std::string tile, site, ttype, base;
	const FeatMap& feat;
	std::map<std::string, std::string> pins;
	bool isM = false;
	// a wide RAM contains its own F7/F8 mux, so no separate MUXF7/MUXF8 cell
	// may be emitted for it -- but its output net is still what the OUTMUX
	// and FFMUX downstream see.
	std::array<bool, 3> muxTaken{};  // F7A, F7B, F8

	Slice(FasmDesign& d, Netlist& n, std::string t, std::string s, const FeatMap& f)
	    : fd(d), nl(n), tile(std::move(t)), site(std::move(s)), feat(f) {
		ttype = fd.used_tiles[tile];
		base = vname(tile) + "_" + site;
		pins = fd.sitePins(ttype, fd.siteOrdinal(tile, site));
		isM = fd.db.tilegrid.at(tile).sites.at(site).rfind("SLICEM", 0) == 0;
	}

	bool has(const std::string& f) const { return featPresent(feat, f); }
	bool has(char col, const std::string& suffix) const { return has(std::string(1, col) + suffix); }

	std::string pin(const std::string& p) const {
		auto it = pins.find(p);
		return it != pins.end() ? fd.netOf(tile, it->second) : std::string();
	}
	std::string pinOr0(const std::string& p) const {
		std::string n = pin(p);
		return n.empty() ? "1'b0" : n;
	}

	// Nets internal to the slice. Named, not created, until something
	// actually references them -- an untouched column must not leave a
	// stray wire (and a stray "assign ... = 1'b0") in the output.
	std::string use(const std::string& n) {
		if (!isLiteral(n)) nl.internalNets.insert(n);
		return n;
	}
	std::string drive(const std::string& n) {
		use(n);
		nl.markDriven(n);
		return n;
	}
	// the 6-LUT output: a real site pin, so usually a routed net
	std::string o6(int i) {
		std::string n = pin(std::string(1, kCols[i]));
		return n.empty() ? use("w6_" + base + "_" + kCols[i]) : n;
	}
	std::string o5(int i) { return use("w5_" + base + "_" + kCols[i]); }
	std::string xorN(int i) { return use("wxor_" + base + "_" + kCols[i]); }
	std::string cyN(int i) { return use("wcy_" + base + "_" + kCols[i]); }
	std::string mc31(int i) { return use("wmc31_" + base + "_" + kCols[i]); }
	std::string q5(int i) { return use("w5q_" + base + "_" + kCols[i]); }
	std::string f7a() { return use("wf7a_" + base); }
	std::string f7b() { return use("wf7b_" + base); }
	std::string f8() { return use("wf8_" + base); }

	std::string initBits(char col) const { return featBitString(feat, std::string(1, col) + "LUT.INIT", 64); }
	bool hasInit(char col) const { return feat.count(std::string(1, col) + "LUT.INIT") > 0; }

	// The write-data mux ahead of each LUT's DI1 input. Only A/B/C have one;
	// D is wired straight to the DI site pin.
	std::string diNet(char col) const {
		switch (col) {
			case 'A':
				if (has("ALUT.DI1MUX.AI")) return pinOr0("AI");
				if (has("BLUT.DI1MUX.BI")) return pinOr0("BI");
				return pinOr0("DI");
			case 'B': return has("BLUT.DI1MUX.BI") ? pinOr0("BI") : pinOr0("DI");
			case 'C': return has("CLUT.DI1MUX.CI") ? pinOr0("CI") : pinOr0("DI");
			default: return pinOr0("DI");
		}
	}

	// WEMUX picks whether the memory write enable comes from the slice's own
	// WE pin or is borrowed from CE.
	std::string weNet() const { return has("WEMUX.CE") ? pinOr0("CE") : pinOr0("WE"); }
	std::string clkNet() const { return pinOr0("CLK"); }
	// SRLC32E/SRL16E and the RAM primitives have no clock-inversion
	// parameter, so an inverted site clock cannot be carried on the cell --
	// say so rather than emitting a cell that runs on the wrong edge.
	std::string memClk() {
		if (has("CLKINV"))
			nl.warn(base + ": CLKINV set on a site with memory cells; their clock polarity is not "
			               "representable on the primitive and is emitted uninverted");
		return clkNet();
	}

	// address input i of a column, i.e. site pin <col><i+1>
	std::string addr(char col, int i) const { return pinOr0(std::string(1, col) + std::to_string(i + 1)); }
	std::vector<std::string> addrMsbFirst(char col, int n, int from = 0) const {
		std::vector<std::string> v;
		for (int i = from + n - 1; i >= from; i--) v.push_back(addr(col, i));
		return v;
	}

	std::string name(const std::string& prefix, const std::string& tail) {
		return nl.uniqName(prefix + "_" + base + (tail.empty() ? "" : "_" + tail));
	}
};

// ---------------------------------------------------------------------
// distributed RAM mode decode -- prjxray fuzzer 018-clb-ram's vocabulary.
// The three per-column bits (RAM / SRL / SMALL) plus WA7USED/WA8USED and the
// DI1 mux settings are between them exactly enough to name which RAM
// primitive the slice is configured as.
// ---------------------------------------------------------------------

std::map<char, std::string> decodeDram(const Slice& s) {
	bool ram[4], small[4];
	for (int i = 0; i < 4; i++) {
		ram[i] = s.has(kCols[i], "LUT.RAM");
		small[i] = s.has(kCols[i], "LUT.SMALL");
	}
	auto di = [&](char c) { return s.has(std::string(1, c) + "LUT.DI1MUX." + c + "I"); };

	std::map<char, std::string> modes;
	if (s.has("WA8USED")) {  // all eight address bits: one 256-deep RAM
		for (char c : std::string("ABCD")) modes[c] = "RAM256X1S";
		return modes;
	}
	if (s.has("WA7USED")) {  // seven: 128-deep, either one pair or the whole slice
		if (!ram[0]) {
			modes['A'] = modes['B'] = "LUT";
			modes['C'] = modes['D'] = "RAM128X1S";
			return modes;
		}
		std::string mode = di('B') ? "RAM128X1S" : "RAM128X1D";
		for (char c : std::string("ABCD")) modes[c] = mode;
		return modes;
	}

	bool allRam = ram[0] && ram[1] && ram[2] && ram[3];
	bool allSmall = small[0] && small[1] && small[2] && small[3];
	if (allRam && allSmall) return {{'D', "RAM32M"}};
	if (allRam) return {{'D', "RAM64M"}};

	// remaining: the single- and dual-port 32/64-deep RAMs, occupying either
	// one column (single port, written from its own DI) or a column pair.
	std::set<char> remaining{'A', 'B', 'C', 'D'};
	for (char c : std::string("AC")) {
		int i = c - 'A';
		if (ram[i] && di(c)) {
			remaining.erase(c);
			modes[c] = small[i] ? "RAM32X1S" : "RAM64X1S";
		}
	}
	for (char c : std::string("BD")) {
		int i = c - 'A';
		if (!ram[i]) continue;
		char below = (char)(c - 1);
		if (remaining.count(below) && ram[below - 'A']) {
			remaining.erase(c);
			remaining.erase(below);
			modes[c] = modes[below] = small[i] ? "RAM32X1D" : "RAM64X1D";
			continue;
		}
		if (remaining.count(c)) {
			remaining.erase(c);
			modes[c] = small[i] ? "RAM32X1S" : "RAM64X1S";
		}
	}
	for (char c : remaining) modes[c] = "LUT";
	return modes;
}

// ---------------------------------------------------------------------
// the individual RAM shapes
// ---------------------------------------------------------------------

void emitRam256(Slice& s) {
	std::string init = s.initBits('A') + s.initBits('B') + s.initBits('C') + s.initBits('D');
	std::vector<std::string> a{s.pinOr0("BX"), s.pinOr0("CX")};
	for (auto& n : s.addrMsbFirst('D', 6)) a.push_back(n);
	s.nl.instance("RAM256X1S", ".INIT(" + hexLiteral(init) + ")", s.name("ram256", ""),
	              ".O(" + s.drive(s.f8()) + "), .D(" + s.pinOr0("DI") + "), .WCLK(" + s.memClk() +
	                  "), .WE(" + s.weNet() + "), .A(" + concat(a) + ")");
	s.muxTaken = {true, true, true};
}

void emitRam128S(Slice& s, char hi, char lo, const std::string& out, const std::string& a6,
                 const std::string& tail) {
	std::string conns = ".O(" + s.drive(out) + "), .D(" + s.diNet(hi) + "), .WCLK(" + s.memClk() +
	                    "), .WE(" + s.weNet() + "), .A6(" + a6 + ")";
	for (int i = 5; i >= 0; i--) conns += ", .A" + std::to_string(i) + "(" + s.addr(lo, i) + ")";
	s.nl.instance("RAM128X1S", ".INIT(" + hexLiteral(s.initBits(hi) + s.initBits(lo)) + ")",
	              s.name("ram128s", tail), conns);
}

void emitRam128D(Slice& s) {
	std::string init = s.initBits('C') + s.initBits('D');
	std::string other = s.initBits('A') + s.initBits('B');
	if (init != other)
		s.nl.warn(s.base + ": RAM128X1D read ports hold different contents; using the single-port half");
	std::vector<std::string> a{s.pinOr0("CX")}, dpra{s.pinOr0("AX")};
	for (auto& n : s.addrMsbFirst('D', 6)) a.push_back(n);
	for (auto& n : s.addrMsbFirst('B', 6)) dpra.push_back(n);
	s.nl.instance("RAM128X1D", ".INIT(" + hexLiteral(init) + ")", s.name("ram128d", ""),
	              ".SPO(" + s.drive(s.f7b()) + "), .DPO(" + s.drive(s.f7a()) + "), .D(" + s.pinOr0("DI") +
	                  "), .WCLK(" + s.memClk() + "), .WE(" + s.weNet() + "), .A(" + concat(a) + "), .DPRA(" +
	                  concat(dpra) + ")");
	s.muxTaken[0] = s.muxTaken[1] = true;
}

// RAM32M's INIT parameters are interleaved relative to the raw LUT bits:
// parameter bit 2k is LUT bit k and bit 2k+1 is LUT bit 32+k, because the two
// 32x1 halves of one LUT hold the two bits of each RAM32M word.
std::string ram32mInit(const std::string& lutBits) {
	std::string p(64, '0');
	for (int k = 0; k < 32; k++) {
		p[63 - 2 * k] = lutBits[63 - k];
		p[63 - (2 * k + 1)] = lutBits[63 - (32 + k)];
	}
	return p;
}

void emitRamM(Slice& s, bool small) {
	std::string params, conns;
	for (int i = 0; i < 4; i++) {
		char c = kCols[i];
		std::string cs(1, c);
		if (!params.empty()) params += ", ";
		params += ".INIT_" + cs + "(" + hexLiteral(small ? ram32mInit(s.initBits(c)) : s.initBits(c)) + ")";
		if (!conns.empty()) conns += ", ";
		if (small)
			conns += ".DO" + cs + "(" + concat({s.drive(s.o6(i)), s.drive(s.o5(i))}) + "), .DI" + cs + "(" +
			         concat({s.pinOr0(cs + "X"), s.diNet(c)}) + "), .ADDR" + cs + "(" +
			         concat(s.addrMsbFirst(c, 5)) + ")";
		else
			conns += ".DO" + cs + "(" + s.drive(s.o6(i)) + "), .DI" + cs + "(" + s.diNet(c) + "), .ADDR" +
			         cs + "(" + concat(s.addrMsbFirst(c, 6)) + ")";
	}
	conns += ", .WCLK(" + s.memClk() + "), .WE(" + s.weNet() + ")";
	s.nl.instance(small ? "RAM32M" : "RAM64M", params, s.name(small ? "ram32m" : "ram64m", ""), conns);
}

void emitRam64X1D(Slice& s, char hi) {
	char lo = (char)(hi - 1);
	if (s.initBits(hi) != s.initBits(lo))
		s.nl.warn(s.base + ": RAM64X1D read ports hold different contents; using the single-port column");
	std::string conns = ".SPO(" + s.drive(s.o6(hi - 'A')) + "), .DPO(" + s.drive(s.o6(lo - 'A')) + "), .D(" +
	                    s.diNet(lo) + "), .WCLK(" + s.memClk() + "), .WE(" + s.weNet() + ")";
	for (int i = 5; i >= 0; i--)
		conns += ", .A" + std::to_string(i) + "(" + s.addr(hi, i) + "), .DPRA" + std::to_string(i) + "(" +
		         s.addr(lo, i) + ")";
	s.nl.instance("RAM64X1D", ".INIT(" + hexLiteral(s.initBits(hi)) + ")",
	              s.name("ram64d", std::string(1, lo) + hi), conns);
}

void emitRam32X1D(Slice& s, char hi) {
	char lo = (char)(hi - 1);
	std::string bits = s.initBits(hi);
	if (bits != s.initBits(lo))
		s.nl.warn(s.base + ": RAM32X1D read ports hold different contents; using the single-port column");
	// the x6LUT half holds the upper 32 INIT bits, the x5LUT half the lower
	for (int half = 0; half < 2; half++) {
		bool six = half == 0;
		std::string conns = ".SPO(" + s.drive(six ? s.o6(hi - 'A') : s.o5(hi - 'A')) + "), .DPO(" +
		                    s.drive(six ? s.o6(lo - 'A') : s.o5(lo - 'A')) + "), .D(" +
		                    (six ? s.pinOr0(std::string(1, hi) + "X") : s.diNet(hi)) + "), .WCLK(" +
		                    s.memClk() + "), .WE(" + s.weNet() + ")";
		for (int i = 4; i >= 0; i--)
			conns += ", .A" + std::to_string(i) + "(" + s.addr(hi, i) + "), .DPRA" + std::to_string(i) +
			         "(" + s.addr(lo, i) + ")";
		s.nl.instance("RAM32X1D", ".INIT(" + hexLiteral(bits.substr(six ? 0 : 32, 32)) + ")",
		              s.name("ram32d", std::string(1, lo) + hi + (six ? "6" : "5")), conns);
	}
}

void emitRam64X1S(Slice& s, char c) {
	std::string conns = ".O(" + s.drive(s.o6(c - 'A')) + "), .D(" + s.diNet(c) + "), .WCLK(" + s.memClk() +
	                    "), .WE(" + s.weNet() + ")";
	for (int a = 5; a >= 0; a--) conns += ", .A" + std::to_string(a) + "(" + s.addr(c, a) + ")";
	s.nl.instance("RAM64X1S", ".INIT(" + hexLiteral(s.initBits(c)) + ")", s.name("ram64s", std::string(1, c)),
	              conns);
}

void emitRam32X1S(Slice& s, char c) {
	int i = c - 'A';
	std::string bits = s.initBits(c);
	for (int half = 0; half < 2; half++) {
		bool six = half == 0;
		std::string conns = ".O(" + s.drive(six ? s.o6(i) : s.o5(i)) + "), .D(" +
		                    (six ? s.pinOr0(std::string(1, c) + "X") : s.diNet(c)) + "), .WCLK(" +
		                    s.memClk() + "), .WE(" + s.weNet() + ")";
		for (int a = 4; a >= 0; a--) conns += ", .A" + std::to_string(a) + "(" + s.addr(c, a) + ")";
		s.nl.instance("RAM32X1S", ".INIT(" + hexLiteral(bits.substr(six ? 0 : 32, 32)) + ")",
		              s.name("ram32s", std::string(1, c) + (six ? "6" : "5")), conns);
	}
}

// ---------------------------------------------------------------------
// shift registers
// ---------------------------------------------------------------------

// The 64-bit LUT INIT of a column in SRL mode holds each shift-register bit
// twice (the LUT's A1 input is tied high), so the 32 real bits are every
// other bit of the INIT read MSB-first. Returns "" if that duplication does
// not hold, which would mean the column is not really an SRL.
std::string srlHalf(const std::string& lutBits) {
	std::string s;
	for (int i = 0; i < 64; i += 2) s.push_back(lutBits[i]);
	return s;
}

bool srlDuplicated(const std::string& lutBits) {
	for (int i = 0; i < 64; i += 2)
		if (lutBits[i] != lutBits[i + 1]) return false;
	return true;
}

void emitSrls(Slice& s) {
	// The cascade runs D -> C -> B -> A, each link being one DI1 mux
	// setting; the far end (A's Q31) leaves the slice through the D column's
	// output mux, which is why an A-column cascade shows up as DOUTMUX.MC31.
	bool up[4] = {s.has("ALUT.DI1MUX.BDI1_BMC31"), s.has("BLUT.DI1MUX.DI_CMC31"),
	              s.has("CLUT.DI1MUX.DI_DMC31"), false};
	bool down[4] = {s.has("DOUTMUX.MC31") || s.has("DFFMUX.MC31"), up[0], up[1], up[2]};

	for (int i = 3; i >= 0; i--) {  // D first, so a cascade source exists before its sink
		char c = kCols[i];
		if (!s.has(c, "LUT.SRL")) continue;
		std::string cs(1, c);
		// The A1-high half is what the shift register actually reads. Vivado
		// writes the other half to match, so a mismatch means this bitstream
		// was produced by something that does not -- report that, but emit
		// the bits that ARE there rather than replacing them with zeros: a
		// silent zero is indistinguishable from a genuinely empty register.
		std::string bits = srlHalf(s.initBits(c));
		if (!srlDuplicated(s.initBits(c)))
			s.nl.warn(s.base + "/" + cs +
			          ": SRL INIT halves differ, so this bitstream does not use the duplicated "
			          "encoding; the addressed half is reported as-is");
		std::string d = up[i] ? s.mc31(i + 1) : s.diNet(c);

		if (!s.has(c, "LUT.SMALL")) {
			// SRLC32E: address A[4:0] is the column's inputs 2..6
			std::string conns = ".Q(" + s.drive(s.o6(i)) + "), .D(" + d + "), .CLK(" + s.memClk() +
			                    "), .CE(" + s.weNet() + "), .A(" + concat(s.addrMsbFirst(c, 5, 1)) + ")";
			if (down[i]) conns += ", .Q31(" + s.drive(s.mc31(i)) + ")";
			s.nl.instance("SRLC32E", ".INIT(" + hexLiteral(bits) + ")", s.name("srl32", cs), conns);
			continue;
		}
		// Two 16-deep shift registers share the LUT. The x6LUT half is read
		// with A6 high and so holds the upper 16 bits; the x5LUT half (whose
		// output is O5) holds the lower 16. Only the x6LUT half can cascade.
		for (int half = 0; half < 2; half++) {
			bool six = half == 0;
			std::string dsrc = six ? s.pinOr0(cs + "X") : d;
			bool cascOut = six && down[i];
			std::string conns = ".Q(" + s.drive(six ? s.o6(i) : s.o5(i)) + "), .D(" + dsrc + "), .CLK(" +
			                    s.memClk() + "), .CE(" + s.weNet() + ")";
			for (int a = 3; a >= 0; a--)
				conns += ", .A" + std::to_string(a) + "(" + s.addr(c, a + 1) + ")";
			if (cascOut) conns += ", .Q15(" + s.drive(s.mc31(i)) + ")";
			s.nl.instance(cascOut ? "SRLC16E" : "SRL16E",
			              ".INIT(" + hexLiteral(bits.substr(six ? 0 : 16, 16)) + ")",
			              s.name("srl16", cs + (six ? "6" : "5")), conns);
		}
	}
}

// ---------------------------------------------------------------------
// carry chain
// ---------------------------------------------------------------------

bool carryUsed(const Slice& s) {
	for (char c : std::string("ABCD")) {
		if (s.has("CARRY4." + std::string(1, c) + "CY0")) return true;
		if (s.has(c, "FFMUX.XOR") || s.has(c, "FFMUX.CY")) return true;
		if (s.has(c, "OUTMUX.XOR") || s.has(c, "OUTMUX.CY")) return true;
	}
	for (const char* p : {"PRECYINIT.AX", "PRECYINIT.C0", "PRECYINIT.C1", "PRECYINIT.CIN"})
		if (s.has(p)) return true;
	return false;
}

void emitCarry4(Slice& s) {
	// PRECYINIT names one of four sources for the bottom of the chain, and
	// which of CI / CYINIT the CARRY4 cell takes it on: only the CIN case
	// continues a chain from the slice below, the rest start a new one.
	std::string ci = "1'b0", cyinit = "1'b0";
	if (s.has("PRECYINIT.CIN")) ci = s.pinOr0("CIN");
	else if (s.has("PRECYINIT.AX")) cyinit = s.pinOr0("AX");
	else if (s.has("PRECYINIT.C1")) cyinit = "1'b1";
	else if (!s.has("PRECYINIT.C0"))
		s.nl.warn(s.base + ": CARRY4 in use with no PRECYINIT feature; assuming CYINIT = 0");

	std::vector<std::string> di, sIn, o, co;
	for (int i = 3; i >= 0; i--) {  // MSB-first, for the vector connections
		char c = kCols[i];
		// xCY0 set means this stage's data input is the column's O5 output;
		// otherwise it comes in from outside on the column's X pin.
		di.push_back(s.has("CARRY4." + std::string(1, c) + "CY0") ? s.o5(i)
		                                                         : s.pinOr0(std::string(1, c) + "X"));
		sIn.push_back(s.o6(i));
		o.push_back(s.drive(s.xorN(i)));
		co.push_back(s.drive(s.cyN(i)));
	}
	s.nl.instance("CARRY4", "", s.name("carry4", ""),
	              ".CO(" + concat(co) + "), .O(" + concat(o) + "), .CI(" + ci + "), .CYINIT(" + cyinit +
	                  "), .DI(" + concat(di) + "), .S(" + concat(sIn) + ")");
	// CO[3] leaves the slice on the COUT pin: that is how the chain reaches
	// the slice above.
	std::string cout = s.pin("COUT");
	if (!cout.empty()) s.nl.assign(cout, s.cyN(3));
}

// ---------------------------------------------------------------------
// flip-flops, latches and the output mux
// ---------------------------------------------------------------------

std::string ffType(bool sync, bool latch, int srval, std::string& clkPin, std::string& cePin,
                   std::string& srPin) {
	clkPin = latch ? "G" : "C";
	cePin = latch ? "GE" : "CE";
	if (latch) {
		srPin = srval == 0 ? "CLR" : "PRE";
		return srval == 0 ? "LDCE" : "LDPE";
	}
	if (sync) {
		srPin = srval == 0 ? "R" : "S";
		return srval == 0 ? "FDRE" : "FDSE";
	}
	srPin = srval == 0 ? "CLR" : "PRE";
	return srval == 0 ? "FDCE" : "FDPE";
}

// The net a FFMUX/OUTMUX select names. Returns "" for a select this pass does
// not model, so the caller can say so rather than quietly wiring the wrong
// thing up.
std::string muxSource(Slice& s, char col, const std::string& sel) {
	int i = col - 'A';
	if (sel == "O6") return s.o6(i);
	if (sel == "O5") return s.o5(i);
	if (sel == "XOR") return s.xorN(i);
	if (sel == "CY") return s.cyN(i);
	if (sel == "F7") return col == 'A' ? s.f7a() : s.f7b();
	if (sel == "F8") return s.f8();
	if (sel == "MC31") return s.mc31(0);  // the cascade's far end is A's Q31
	if (sel == std::string(1, col) + "X") return s.pinOr0(std::string(1, col) + "X");
	return "";
}

std::string selectOf(const Slice& s, const std::string& prefix) {
	for (auto& f : s.feat)
		if (f.first.rfind(prefix, 0) == 0) return f.first.substr(prefix.size());
	return "";
}

void emitFfs(Slice& s) {
	bool sync = s.has("FFSYNC");
	bool latch = s.has("LATCH");
	// CLKINV inverts the whole site's clock. A latch's enable is the
	// complement of the flip-flop clock it shares the mux with, so the two
	// primitives take opposite values of the same bit.
	bool clkInv = s.has("CLKINV");

	for (int pass = 0; pass < 2; pass++) {  // 5FFs first: an OUTMUX may select their Q
		bool is5 = pass == 0;
		for (int i = 0; i < 4; i++) {
			char col = kCols[i];
			std::string c(1, col);
			bool present;
			if (is5)
				present = s.has(c + "5FF.ZINI") || s.has(c + "5FF.ZRST") ||
				          s.has(c + "OUTMUX." + c + "5Q") || s.has(c + "5FFMUX.IN_A") ||
				          s.has(c + "5FFMUX.IN_B");
			else
				present = s.has(c + "FF.ZINI") || s.has(c + "FF.ZRST") || !selectOf(s, c + "FFMUX.").empty();
			if (!present) continue;

			std::string nm = s.nl.uniqName("ff_" + s.base + "_" + c + (is5 ? "5" : ""));
			std::string five = is5 ? "5" : "";
			int srval = s.has(c + five + "FF.ZRST") ? 0 : 1;
			int init = s.has(c + five + "FF.ZINI") ? 0 : 1;

			std::string dNet, qNet;
			if (is5) {
				dNet = s.has(c + "5FFMUX.IN_B") ? s.pinOr0(c + "X") : s.o5(i);
				// The 5FF's Q leaves the site only through the column's
				// output mux, so when that mux selects it there is no need
				// for a net of its own -- drive the xMUX wire directly.
				qNet = s.has(c + "OUTMUX." + c + "5Q") ? s.pin(c + "MUX") : std::string();
				if (qNet.empty()) qNet = s.q5(i);
			} else {
				// no FFMUX feature at all is the default: straight off O6
				std::string sel = selectOf(s, c + "FFMUX.");
				if (sel.empty()) sel = "O6";
				dNet = muxSource(s, col, sel);
				if (dNet.empty()) {
					s.nl.warn(nm + ": unmodelled FFMUX select ." + sel + "; D tied low");
					dNet = "1'b0";
				}
				qNet = s.pin(c + "Q");
				if (qNet.empty()) qNet = s.use("wq_" + s.base + "_" + c);
			}
			s.nl.markDriven(qNet);

			std::string clkPin, cePin, srPin;
			std::string prim = ffType(sync, latch && !is5, srval, clkPin, cePin, srPin);
			std::string clk = s.pinOr0("CLK");
			std::string ce = s.has("CEUSEDMUX") ? s.pinOr0("CE") : "1'b1";
			std::string sr = s.has("SRUSEDMUX") ? s.pinOr0("SR") : "1'b0";
			s.nl.clkNetsUsed.insert(clk);
			if (s.has("SRUSEDMUX")) s.nl.srNetsUsed.insert(sr);
			std::string params = ".INIT(1'b" + std::to_string(init) + ")";
			if (clkPin == "G") params += ", .IS_G_INVERTED(1'b" + std::to_string(clkInv ? 0 : 1) + ")";
			else if (clkInv) params += ", .IS_C_INVERTED(1'b1)";
			s.nl.instance(prim, params, nm,
			              "." + clkPin + "(" + clk + "), ." + cePin + "(" + ce + "), .D(" + dNet + "), ." +
			                  srPin + "(" + sr + "), .Q(" + qNet + ")");
		}
	}

	for (int i = 0; i < 4; i++) {
		char col = kCols[i];
		std::string c(1, col);
		std::string sel = selectOf(s, c + "OUTMUX.");
		if (sel.empty() || sel == "O6") continue;  // O6 is joined to the xMUX wire in the union-find
		std::string mux = s.pin(c + "MUX");
		if (mux.empty()) continue;
		if (sel == c + "5Q") continue;  // the 5FF already drives the xMUX wire
		std::string src = muxSource(s, col, sel);
		if (src.empty()) {
			s.nl.warn(s.base + "/" + c + ": unmodelled OUTMUX select ." + sel + "; xMUX left undriven");
			continue;
		}
		s.nl.assign(mux, src);
	}
}

}  // namespace

void emitSliceCells(FasmDesign& fd, Netlist& nl) {
	for (auto& kv : fd.slice_feats) {
		const std::string& tile = kv.first.first;
		std::string site = fd.siteFromSuffix(tile, kv.first.second);
		if (site.empty()) continue;
		Slice s(fd, nl, tile, site, kv.second);

		bool anySrl = false, anyRam = false;
		for (int i = 0; i < 4; i++) {
			anySrl |= s.has(kCols[i], "LUT.SRL");
			anyRam |= s.has(kCols[i], "LUT.RAM");
		}
		std::map<char, std::string> dram;
		if (anyRam) {
			if (!s.isM) nl.warn(s.base + ": distributed-RAM features on a site that is not a SLICEM");
			dram = decodeDram(s);
		}
		auto mode = [&](char c) {
			auto it = dram.find(c);
			return it == dram.end() ? std::string() : it->second;
		};

		// ---- plain LUTs: every column with an INIT that is not really a
		// shift register or a RAM. Uncelled route-throughs count: a real,
		// configured INIT with no "intended" logical cell is still real
		// silicon and must be emitted. ----
		for (int i = 0; i < 4; i++) {
			char col = kCols[i];
			if (!s.hasInit(col) || s.has(col, "LUT.SRL")) continue;
			std::string m = mode(col);
			if (!m.empty() && m != "LUT") continue;
			std::string conns = ".O6(" + s.drive(s.o6(i)) + "), .O5(" + s.drive(s.o5(i)) + ")";
			for (int in = 0; in < 6; in++)
				conns += ", .I" + std::to_string(in) + "(" + s.addr(col, in) + ")";
			nl.instance("LUT6_2", ".INIT(" + hexLiteral(s.initBits(col)) + ")",
			            nl.uniqName("lut_" + s.base + "_" + col), conns);
		}

		if (anySrl) emitSrls(s);

		if (anyRam) {
			if (mode('D') == "RAM256X1S") {
				emitRam256(s);
			} else if (mode('D') == "RAM128X1S") {
				emitRam128S(s, 'C', 'D', s.f7b(), s.pinOr0("CX"), "CD");
				s.muxTaken[1] = true;
				if (mode('B') == "RAM128X1S") {
					emitRam128S(s, 'A', 'B', s.f7a(), s.pinOr0("AX"), "AB");
					s.muxTaken[0] = true;
				}
			} else if (mode('D') == "RAM128X1D") {
				emitRam128D(s);
			} else if (mode('D') == "RAM64M") {
				emitRamM(s, false);
			} else if (mode('D') == "RAM32M") {
				emitRamM(s, true);
			} else {
				std::set<char> done;
				for (char c : std::string("BD")) {
					if (mode(c) == "RAM64X1D") emitRam64X1D(s, c);
					else if (mode(c) == "RAM32X1D") emitRam32X1D(s, c);
					else continue;
					done.insert(c);
					done.insert((char)(c - 1));
				}
				for (char c : std::string("ABCD")) {
					if (done.count(c)) continue;
					if (mode(c) == "RAM64X1S") emitRam64X1S(s, c);
					else if (mode(c) == "RAM32X1S") emitRam32X1S(s, c);
				}
			}
		}

		// ---- wide muxes, where a RAM has not already swallowed them ----
		bool needF7A = s.has("AFFMUX.F7") || s.has("AOUTMUX.F7");
		bool needF7B = s.has("CFFMUX.F7") || s.has("COUTMUX.F7");
		bool needF8 = s.has("BFFMUX.F8") || s.has("BOUTMUX.F8");
		if (needF8) needF7A = needF7B = true;
		if (needF7A && !s.muxTaken[0])
			nl.instance("MUXF7", "", s.name("muxf7a", ""),
			            ".O(" + s.drive(s.f7a()) + "), .I0(" + s.o6(1) + "), .I1(" + s.o6(0) + "), .S(" +
			                s.pinOr0("AX") + ")");
		if (needF7B && !s.muxTaken[1])
			nl.instance("MUXF7", "", s.name("muxf7b", ""),
			            ".O(" + s.drive(s.f7b()) + "), .I0(" + s.o6(3) + "), .I1(" + s.o6(2) + "), .S(" +
			                s.pinOr0("CX") + ")");
		if (needF8 && !s.muxTaken[2])
			nl.instance("MUXF8", "", s.name("muxf8", ""),
			            ".O(" + s.drive(s.f8()) + "), .I0(" + s.f7b() + "), .I1(" + s.f7a() + "), .S(" +
			                s.pinOr0("BX") + ")");

		if (carryUsed(s)) {
			if (anySrl || anyRam)
				nl.warn(s.base + ": carry features alongside SRL/RAM features; CARRY4 not emitted");
			else
				emitCarry4(s);
		}

		emitFfs(s);
	}
}

}  // namespace f2n
