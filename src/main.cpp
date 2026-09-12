// fasm2netlist: FASM -> anonymous Xilinx-primitive Verilog netlist, in C++.
//
// Standalone C++ port of a Python prototype's FasmDesign core (from the
// xc7-bitstream-tools project's scripts/bit2gates.py) -- the "no cheating"
// logic: cell existence and connectivity derived purely from the FASM + fixed
// prjxray tile/pip tables (tilegrid.json / tileconn.json / tile_type_*.json /
// ppips_*.db, all properties of the silicon, not of any specific build),
// never from a placement or routed-JSON dump; cell names are anonymous/
// auto-generated, not carried over from any toolchain-internal source.
//
// This file is the driver plus the top-level IO (IBUF/IBUFDS/OBUF/BUFG,
// resolved via .xdc + package_pins.csv when --xdc and --part/--bit are
// given). The extraction core -- database, FASM tokenizer, union-find net
// reconstruction -- is in netlist_core.{hpp,cpp}; the fabric cell families
// each have their own decoder in cells_*.cpp:
//
//   cells_slice.cpp  LUT6_2, FDRE/FDSE/FDCE/FDPE/LDCE/LDPE, CARRY4,
//                    MUXF7/MUXF8, SRLC32E/SRL16E/SRLC16E and the
//                    distributed-RAM family (RAM32X1S ... RAM256X1S,
//                    RAM32M/RAM64M)
//   cells_dsp.cpp    DSP48E1
//   cells_bram.cpp   RAMB18E1, RAMB36E1
//
// Scope decisions (flagged explicitly rather than silently assumed):
//  - FASM tokenizing is a small hand-written line parser scoped to the
//    regular subset prjxray's own FASM output actually uses (plain feature
//    lines, two-wire PIP lines, "SITE.FEATURE[hi:lo] = N'bBITS" value lines)
//    -- not a full grammar implementation of the FASM language. Project X-Ray
//    vendors a real ANTLR-grammar FASM parser (github.com/chipsalliance/fasm),
//    which would be the "proper parser" choice; its C++ runtime wasn't wired
//    into this tool because pulling in the full antlr4-cpp-runtime as a fresh,
//    unproven build dependency was judged not worth it for this pass. Swapping
//    it in later is a contained change (parseFasm() in netlist_core.cpp is the
//    only place that would need to change).
//  - JSON parsing uses this project's own minimal json.hpp -- deliberately
//    self-contained (no vendored third-party dependency at all right now).
//  - A hard block (DSP48E1, RAMB18E1/RAMB36E1) is extracted as a configured
//    instance, not as gates: the bitstream carries its configuration, and the
//    behaviour that configuration selects is inside the block. Where the bits
//    genuinely do not determine an attribute the decoder says so on stderr
//    rather than inventing a value.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "cells.hpp"
#include "netlist_core.hpp"

using namespace f2n;

namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------
// top-level IO identity -- .xdc (PACKAGE_PIN <-> port name) + prjxray's
// package_pins.csv (pin -> tile/site), the direct C++ port of
// xc7-bitstream-tools's scripts/bit2gates.py own XDC handling. Only
// PACKAGE_PIN<->port and pin->(tile,site) are needed here; no other XDC
// constraint (IOSTANDARD, timing, ...) is parsed.
// ---------------------------------------------------------------------

std::map<std::string, std::string> parseXdcPins(const std::string& path) {
	std::map<std::string, std::string> pinForPort;
	std::ifstream f(path);
	if (!f) throw std::runtime_error("cannot open xdc: " + path);
	static const std::regex re(R"(set_property\s+PACKAGE_PIN\s+(\S+)\s+\[get_ports\s*(\{[^}]+\}|\S+)\])");
	std::string line;
	while (std::getline(f, line)) {
		std::smatch m;
		if (!std::regex_search(line, m, re)) continue;
		std::string port = m[2].str();
		if (port.size() >= 2 && port.front() == '{' && port.back() == '}') port = port.substr(1, port.size() - 2);
		pinForPort[port] = m[1].str();
	}
	return pinForPort;
}

struct PortBit {
	int idx;  // -1 for a scalar (non-bussed) port
	std::string pin;
};

// base port name -> its bits (each either a scalar, idx==-1, or one bus bit)
std::map<std::string, std::vector<PortBit>> busGroups(const std::map<std::string, std::string>& pinForPort) {
	std::map<std::string, std::vector<PortBit>> groups;
	static const std::regex bitRe(R"(^(.*)\[(\d+)\]$)");
	for (auto& [name, pin] : pinForPort) {
		std::smatch m;
		if (std::regex_match(name, m, bitRe)) groups[m[1].str()].push_back({std::stoi(m[2].str()), pin});
		else groups[name].push_back({-1, pin});
	}
	return groups;
}

std::string portLabel(const std::string& base, int idx) {
	return idx < 0 ? base : base + "[" + std::to_string(idx) + "]";
}

struct PkgPin {
	std::string tile, site;
};

std::map<std::string, PkgPin> loadPackagePins(const std::string& path) {
	std::map<std::string, PkgPin> out;
	std::ifstream f(path);
	if (!f) throw std::runtime_error("cannot open package_pins.csv: " + path);
	std::string line;
	std::getline(f, line);  // header
	while (std::getline(f, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		auto cols = splitAll(line, ',');
		if (cols.size() < 4) continue;
		out[cols[0]] = {cols[3], cols[2]};  // pin -> (tile, site)
	}
	return out;
}

// package_pins.csv lives under the FULL part directory (chip+package+speed
// grade, e.g. "xc7vx485tffg1761-2"), one level more specific than
// --device's directory (tilegrid.json etc). If `part` already names that
// exact directory, use it as-is; otherwise glob for any speed-grade variant
// (pin/site layout is identical across speed grades of the same
// chip+package).
std::string resolvePartDir(const std::string& familyDir, const std::string& part) {
	if (fs::is_directory(familyDir + "/" + part)) return part;
	std::vector<std::string> matches;
	if (fs::is_directory(familyDir)) {
		for (auto& entry : fs::directory_iterator(familyDir)) {
			std::string name = entry.path().filename().string();
			if (name.rfind(part + "-", 0) == 0) matches.push_back(name);
		}
	}
	if (matches.empty())
		throw std::runtime_error("no package_pins directory matching " + part + "(-*) under " + familyDir);
	std::sort(matches.begin(), matches.end());
	return matches.front();
}

// Read the part name straight out of a .bit file's own header (key 'b') --
// the part is a property of the bitstream itself, not something an
// independent verification tool should need to be told out-of-band. Direct
// C++ port of bit2gates.py's parse_bit_part(), same standard Xilinx .bit
// TLV header (verified there against a real VC707 golden bitstream): a
// 2-byte length + that many magic bytes, then every field is a key -- for
// the first field only, itself a 2-byte length + that many bytes (always
// just "a", i.e. field 'a' is doubly length-wrapped in real .bit files;
// every later key is a single bare byte) -- followed by a 2-byte length and
// that many bytes of value, until key 'e' (the raw bitstream payload,
// which this function never reaches: it returns as soon as it sees 'b').
//
// Vivado does not record the speed grade in this field (only chip+package,
// e.g. "7vx485tffg1761"), so the caller must still resolve which exact
// "-N" package_pins.csv directory to use (see resolvePartDir()) -- pin and
// site layout do not vary by speed grade, only timing does.
std::string parseBitPart(const std::string& path) {
	std::ifstream f(path, std::ios::binary);
	if (!f) throw std::runtime_error("cannot open .bit: " + path);
	std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

	auto need = [&](size_t pos, size_t n) {
		if (pos + n > data.size())
			throw std::runtime_error(".bit header truncated or not a Xilinx .bit file: " + path);
	};
	auto readU16 = [&](size_t pos) -> uint16_t {
		need(pos, 2);
		return (uint16_t)(((uint8_t)data[pos] << 8) | (uint8_t)data[pos + 1]);
	};

	size_t pos = 0;
	uint16_t n = readU16(pos);
	pos += 2 + n;  // skip the fixed magic block
	uint16_t wrapLen = readU16(pos);
	pos += 2;
	need(pos, wrapLen);
	std::string key = data.substr(pos, wrapLen);
	pos += wrapLen;
	while (key != "e") {
		uint16_t flen = readU16(pos);
		pos += 2;
		need(pos, flen);
		std::string val = data.substr(pos, flen);
		pos += flen;
		if (key == "b") {
			while (!val.empty() && val.back() == '\0') val.pop_back();
			if (val.rfind("xc", 0) != 0) val = "xc" + val;
			return val;
		}
		need(pos, 1);
		key = std::string(1, data[pos]);
		pos += 1;
	}
	throw std::runtime_error("could not find part-name field ('b') in .bit header: " + path);
}

// IBUF_HP_BANK_GLUE/IBUFDS_BANK_GLUE unambiguously mark an input buffer, but
// there is no equivalent "OBUF_..._BANK_GLUE" marker for a plain
// single-ended output in this tile family. What IS unambiguous: an
// output-configured site carries a DRIVE.<strength> feature (meaningless
// for an input); an input-configured site carries a plain ".IN"/".IN_ONLY"
// feature. Verified against every used IOB site in the validated Johnson
// example: the two sets never overlap. Mirrors bit2gates.py's
// classify_iob() exactly.
std::string classifyIob(const std::set<std::string>& feat) {
	if (feat.count("IBUFDS_BANK_GLUE")) return "ibufds";
	for (auto& f : feat)
		if (f.find(".DRIVE.") != std::string::npos || endsWith(f, ".DRIVE")) return "obuf";
	if (feat.count("IBUF_HP_BANK_GLUE")) return "ibuf";
	for (auto& f : feat)
		if (endsWith(f, ".IN") || endsWith(f, ".IN_ONLY")) return "ibuf";
	return "";
}

}  // namespace

int main(int argc, char** argv) {
	std::string fasmPath, dbRoot, family, device, outPath, moduleName = "top";
	std::string xdcPath, part, bitPath;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		auto need = [&](const char* flag) { return a == flag && i + 1 < argc; };
		if (need("--fasm")) fasmPath = argv[++i];
		else if (need("--db")) dbRoot = argv[++i];
		else if (need("--family")) family = argv[++i];
		else if (need("--device")) device = argv[++i];
		else if (need("--out")) outPath = argv[++i];
		else if (need("--module")) moduleName = argv[++i];
		else if (need("--xdc")) xdcPath = argv[++i];
		else if (need("--part")) part = argv[++i];
		else if (need("--bit")) bitPath = argv[++i];
	}
	if (fasmPath.empty() || dbRoot.empty() || family.empty() || device.empty() || outPath.empty()) {
		std::cerr << "usage: fasm2netlist --fasm F --db PRJXRAY_DB --family FAMILY --device DEVICE --out OUT.v "
		             "[--module NAME] [--xdc DESIGN.xdc (--part PART | --bit DESIGN.bit)]\n"
		             "  (--family/--device instead of --part for the fabric DB -- e.g. "
		             "--family virtex7 --device xc7vx485t)\n"
		             "  (--xdc together with --part or --bit enables top-level IO: IBUF/IBUFDS/OBUF/BUFG\n"
		             "   resolution via the XDC's PACKAGE_PIN constraints + prjxray's package_pins.csv for\n"
		             "   that part, e.g. --part xc7vx485tffg1761-2 -- or, if you don't already know the\n"
		             "   exact part, --bit DESIGN.bit reads it straight out of that bitstream's own header\n"
		             "   (speed grade isn't recorded there, but pin/site layout doesn't vary by speed grade\n"
		             "   so this is still enough); without --xdc the module has no ports, as before)\n";
		return 2;
	}
	if (!xdcPath.empty() && part.empty() && bitPath.empty()) {
		std::cerr << "usage: --xdc also needs --part or --bit (top-level IO needs the exact part)\n";
		return 2;
	}
	if (xdcPath.empty() && (!part.empty() || !bitPath.empty())) {
		std::cerr << "usage: --part/--bit only matter together with --xdc (top-level IO)\n";
		return 2;
	}
	if (part.empty() && !bitPath.empty()) part = parseBitPart(bitPath);

	Database db(dbRoot, family, device);
	FasmDesign fd(fasmPath, db);

	Netlist nl;
	emitSliceCells(fd, nl);
	emitDspCells(fd, nl);
	emitBramCells(fd, nl);

	std::vector<std::string>& lines = nl.lines;
	std::vector<std::string>& warnings = nl.warnings;
	auto uniqName = [&](std::string base) { return nl.uniqName(std::move(base)); };

	// ---- top-level IO: OBUF/IBUF/IBUFDS/BUFG, resolved purely from FASM site
	// features (IBUF_HP_BANK_GLUE/OBUF_.../IBUFDS_BANK_GLUE, BUFGCTRL.*.IN_USE)
	// at sites located via the .xdc's PACKAGE_PIN constraints + prjxray's
	// package_pins.csv (pin -> tile/site) -- never from a placement/routed
	// dump. The .xdc is also the sole source of the top-level port list
	// itself. Direct C++ port of bit2gates.py's own IO section. Only runs
	// when both --xdc and --part were given; otherwise the module has no
	// ports, as before. ----
	std::vector<std::string> portDecls;
	std::string clkPortName;
	// Every plain (single-ended) input's buffered output.  The SR tie-off
	// below wants THE reset, and with one candidate that is what this is;
	// with several there is nothing in the bitstream that says which, so it
	// says so rather than picking whichever the map happened to visit last
	// and quietly tying every register's reset to it.
	std::vector<std::string> plainIbufOutNets;
	if (!xdcPath.empty()) {
		auto sitePin = [&](const std::string& tile, const std::string& ttype, int ordinal,
		                    const std::string& pin) -> std::string {
			auto pins = fd.sitePins(ttype, ordinal);
			auto it = pins.find(pin);
			return it != pins.end() ? fd.netOf(tile, it->second) : "";
		};

		auto pinForPort = parseXdcPins(xdcPath);
		auto portBus = busGroups(pinForPort);
		std::string partDir = resolvePartDir(db.family_dir, part);
		auto pkgPins = loadPackagePins(db.family_dir + "/" + partDir + "/package_pins.csv");

		struct Resolved {
			std::string pin, tile, site;
		};
		std::map<std::pair<std::string, int>, Resolved> resolved;
		for (auto& [base, bits] : portBus) {
			for (auto& b : bits) {
				auto pit = pkgPins.find(b.pin);
				if (pit == pkgPins.end()) {
					warnings.push_back("XDC port " + portLabel(base, b.idx) + ": package pin " + b.pin +
					                    " not found in package_pins.csv");
					continue;
				}
				resolved[{base, b.idx}] = {b.pin, pit->second.tile, pit->second.site};
			}
		}

		std::map<std::pair<std::string, std::string>, std::pair<std::string, int>> siteToPortbit;
		for (auto& [key, r] : resolved) siteToPortbit[{r.tile, r.site}] = key;

		auto otherSite = [&](const std::string& tile, const std::string& site) -> std::string {
			for (auto& [s, ty] : db.tilegrid.at(tile).sites)
				if (s != site) return s;
			return "";
		};
		auto featAt = [&](const std::string& tile, const std::string& site) -> std::set<std::string> {
			std::string suffix = fd.fasmIobSuffix(tile, site);
			if (suffix.empty()) return {};
			auto it = fd.iob_feats.find({tile, suffix});
			return it != fd.iob_feats.end() ? it->second : std::set<std::string>{};
		};

		// The N-leg's OWN features can incidentally look like a plain
		// single-ended input; checking sibling-consumption FIRST is what
		// stops it from ALSO getting its own independent IBUF.
		std::map<std::pair<std::string, int>, std::string> portDir;
		std::set<std::pair<std::string, int>> consumed;
		for (auto& [key, r] : resolved) {
			std::string sib = otherSite(r.tile, r.site);
			std::set<std::string> sibFeat = sib.empty() ? std::set<std::string>{} : featAt(r.tile, sib);
			std::set<std::string> myFeat = featAt(r.tile, r.site);
			if (classifyIob(sibFeat) == "ibufds" && classifyIob(myFeat) != "ibufds") {
				portDir[key] = "input";
				consumed.insert(key);
				continue;
			}
			std::string kind = classifyIob(myFeat);
			if (kind == "ibuf" || kind == "ibufds") portDir[key] = "input";
			else if (kind == "obuf") portDir[key] = "output";
			else
				warnings.push_back("IO port " + portLabel(key.first, key.second) + " (pin " + r.pin + ", " +
				                    r.tile + "/" + r.site + "): no IBUF/OBUF/IBUFDS FASM marker found");
		}

		for (auto& [base, bits] : portBus) {
			bool anyOutput = false;
			int maxIdx = -1;
			for (auto& b : bits) {
				auto it = portDir.find({base, b.idx});
				if (it != portDir.end() && it->second == "output") anyOutput = true;
				if (b.idx > maxIdx) maxIdx = b.idx;
			}
			std::string direction = anyOutput ? "output" : "input";
			if (bits.size() == 1 && bits[0].idx < 0) portDecls.push_back(direction + " " + base);
			else portDecls.push_back(direction + " [" + std::to_string(maxIdx) + ":0] " + base);
		}

		for (auto& [key, r] : resolved) {
			if (consumed.count(key)) continue;
			std::set<std::string> feat = featAt(r.tile, r.site);
			std::string ttype = db.tilegrid.at(r.tile).type;
			std::string styp = db.tilegrid.at(r.tile).sites.at(r.site);
			std::string pname = portLabel(key.first, key.second);
			std::string inst = uniqName("io_" + vname(r.tile) + "_" + r.site);
			std::string kind = classifyIob(feat);
			if (kind == "ibufds") {
				std::string sib = otherSite(r.tile, r.site);
				auto sit = siteToPortbit.find({r.tile, sib});
				if (sit == siteToPortbit.end()) {
					warnings.push_back("IBUFDS at " + r.tile + "/" + r.site +
					                    ": no XDC-constrained sibling (N-leg) port found");
					continue;
				}
				std::string ibPname = portLabel(sit->second.first, sit->second.second);
				auto pins = fd.sitePinsByType(ttype, styp);
				auto pit = pins.find("I");
				std::string oNet = pit != pins.end() ? fd.netOf(r.tile, pit->second) : "";
				nl.markDriven(oNet);
				lines.push_back("  IBUFDS \\" + inst + " (.I(" + pname + "), .IB(" + ibPname + "), .O(" + oNet +
				                 "));");
				clkPortName = pname;
			} else if (kind == "ibuf") {
				auto pins = fd.sitePinsByType(ttype, styp);
				auto pit = pins.find("I");
				std::string oNet = pit != pins.end() ? fd.netOf(r.tile, pit->second) : "";
				nl.markDriven(oNet);
				lines.push_back("  IBUF \\" + inst + " (.I(" + pname + "), .O(" + oNet + "));");
				plainIbufOutNets.push_back(oNet);
			} else if (kind == "obuf") {
				auto pins = fd.sitePinsByType(ttype, styp);
				auto pit = pins.find("O");
				std::string iNet = pit != pins.end() ? fd.netOf(r.tile, pit->second) : "";
				lines.push_back("  OBUF \\" + inst + " (.I(" + iNet + "), .O(" + pname + "));");
			}
			// unclassifiable ports were already warned about above.
		}

		// ---- BUFG/BUFGCTRL: found purely from FASM (BUFGCTRL.<local-site>.IN_USE) ----
		for (auto& [key, feat] : fd.bufg_feats) {
			const std::string& tile = key.first;
			const std::string& localSite = key.second;
			if (!feat.count("IN_USE")) continue;
			const std::string& ttype = fd.used_tiles[tile];
			std::string realSite = fd.bufgctrlRealSite(tile, localSite);
			if (realSite.empty()) {
				warnings.push_back("could not resolve real BUFGCTRL site for " + tile + "/" + localSite);
				continue;
			}
			int ordinal = fd.siteOrdinal(tile, realSite);
			std::string i0 = sitePin(tile, ttype, ordinal, "I0");
			std::string o = sitePin(tile, ttype, ordinal, "O");
			nl.markDriven(o);
			lines.push_back("  BUFG \\" + uniqName("bufg_" + vname(tile) + "_" + realSite) + " (.I(" + i0 +
			                 "), .O(" + o + "));");
		}

		// ---- clock distribution tie-off ----
		// The clock tree (GCLK/HROW/HCLK) has real buffering/muxing at
		// multiple points -- treating it like ordinary point-to-point
		// routing via the union-find is actively wrong (BUFG's I0 and O can
		// end up unioned into ONE net via the long GCLK-mesh chain both
		// legitimately pass through, silently shorting the buffer's input to
		// its own output and freezing every register). Instead tie every
		// SLICE clock net straight to the top-level clock port, bypassing
		// the IBUFDS/BUFG/mesh reconstruction entirely.
		if (!clkPortName.empty())
			for (auto& n : nl.clkNetsUsed) nl.assign(n, clkPortName);
		if (plainIbufOutNets.size() > 1 && !nl.srNetsUsed.empty()) {
			std::string names;
			for (auto& n : plainIbufOutNets) names += (names.empty() ? "" : ", ") + n;
			warnings.push_back("several single-ended input ports (" + names +
			                    "); which one drives the slices' set/reset is not in the "
			                    "bitstream, so no SR tie-off was made");
		} else if (plainIbufOutNets.size() == 1) {
			const std::string& rst = plainIbufOutNets.front();
			for (auto& n : nl.srNetsUsed)
				if (n != rst) nl.assign(n, rst);
		}
	}

	// ---- tie off genuinely-unrouted nets (no clock-tree reconstruction in
	// this pass -- see the clock tie-off note above) ----
	//
	// Which nets have a driver is recorded by the emitters as they go rather
	// than recovered by pattern-matching the emitted text: with CARRY4, the
	// SRLs, the RAMs, DSP48E1 and the BRAMs in the mix, the set of output
	// pin names is far too large -- and far too easily confused with input
	// pins whose names merely end the same way -- for a regex over the
	// output to stay honest.
	std::set<std::string> allWires;
	for (auto& kv : fd.netname) allWires.insert(kv.second);
	for (auto& n : nl.internalNets) allWires.insert(n);
	for (auto& n : allWires)
		if (!nl.driven.count(n)) nl.assign(n, "1'b0");

	std::string portList;
	for (size_t i = 0; i < portDecls.size(); i++) {
		if (i) portList += ", ";
		portList += portDecls[i];
	}

	std::ofstream out(outPath);
	out << "// AUTO-GENERATED by fasm2netlist (C++) -- anonymous bitstream-reconstructed netlist\n";
	out << "module " << moduleName << "(" << portList << ");\n";
	for (auto& n : allWires) out << "  wire " << n << ";\n";
	out << "\n";
	for (auto& l : lines) out << l << "\n";
	out << "endmodule\n";

	// headline summary first (LUT/FF counts are what the LVS regression
	// cross-checks against an independent placement), then every cell family
	// the extraction actually produced.
	int ffCount = 0;
	for (auto& [cell, n] : nl.cellCounts)
		if (cell.rfind("FD", 0) == 0 || cell.rfind("LD", 0) == 0) ffCount += n;
	std::cerr << "fasm2netlist: " << nl.cellCounts["LUT6_2"] << " LUT6_2, " << ffCount << " FF, wrote "
	          << outPath << "\n";
	std::cerr << "fasm2netlist: cells:";
	for (auto& [cell, n] : nl.cellCounts)
		if (n) std::cerr << " " << cell << "=" << n;
	std::cerr << "\n";
	for (auto& w : warnings) std::cerr << "warning: " << w << "\n";
	return 0;
}
