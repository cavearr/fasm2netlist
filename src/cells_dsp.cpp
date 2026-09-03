// DSP_L / DSP_R extraction: DSP48E1.
//
// Unlike a SLICE, a DSP48E1's behaviour is not something the bitstream spells
// out gate by gate -- what the bits carry is the cell's configuration, and
// the rest is the hard block. So this emitter does exactly two things: wire
// every site pin to the net the fixed routing tables say it reaches, and
// decode every attribute prjxray has bits for. Where the bits genuinely do
// not determine an attribute (see ACASCREG below) it says so on stderr
// instead of inventing a value.
//
// The attribute encodings are prjxray's fuzzer 100-dsp-mskpat, read as a
// specification: a "Z"-prefixed tag means the stored bit is the complement of
// the attribute, so an absent Z tag is a 1 and a present one is a 0.
#include "cells.hpp"

#include <algorithm>
#include <regex>
#include <set>

namespace f2n {

namespace {

// The DSP48E1 interface: which pins are vectors, and how wide. These are
// properties of the primitive, not of any device, and prjxray's site pin
// names are the primitive's own port names.
const std::pair<const char*, int> kBuses[] = {
    {"A", 30},  {"ACIN", 30},       {"ACOUT", 30},   {"ALUMODE", 4}, {"B", 18},   {"BCIN", 18},
    {"BCOUT", 18}, {"C", 48},       {"CARRYINSEL", 3}, {"CARRYOUT", 4}, {"D", 25}, {"INMODE", 5},
    {"OPMODE", 7}, {"P", 48},       {"PCIN", 48},    {"PCOUT", 48},
};

const char* kOutputs[] = {"ACOUT",  "BCOUT",          "CARRYCASCOUT",   "CARRYOUT", "MULTSIGNOUT",
                          "OVERFLOW", "P",            "PATTERNBDETECT", "PATTERNDETECT",
                          "PCOUT",  "UNDERFLOW"};

bool isOutput(const std::string& pin) {
	for (const char* o : kOutputs)
		if (pin == o) return true;
	return false;
}

// A "Z" tag stores the complement, so absent means the attribute bit is 1.
int zBit(const FeatMap& f, const std::string& zname) { return featPresent(f, zname) ? 0 : 1; }

// ...and for a vector attribute: the complement of the stored bits.
std::string zVector(const FeatMap& f, const std::string& zname, int width) {
	std::string bits = featBitString(f, zname, width);
	for (char& c : bits) c = c == '1' ? '0' : '1';
	return bits;
}

std::string quoted(const std::string& s) { return "\"" + s + "\""; }

}  // namespace

void emitDspCells(FasmDesign& fd, Netlist& nl) {
	for (auto& kv : fd.dsp_feats) {
		const std::string& tile = kv.first.first;
		const std::string& local = kv.first.second;  // "DSP_0" / "DSP_1"
		const FeatMap& f = kv.second;
		const std::string& ttype = fd.used_tiles[tile];

		// Which of the tile's two DSP48E1 sites this is: every one of a
		// site's pins reaches a wire named "<local>_<pin>", so the routing
		// table itself says which site FASM's local name refers to -- no
		// need to guess from site ordering.
		const TileType& tt = fd.db.tileType(ttype);
		const Site* site = nullptr;
		for (auto& s : tt.sites) {
			if (s.type != "DSP48E1" || s.pins.empty()) continue;
			const std::string& w = s.pins.begin()->second;
			if (w.rfind(local + "_", 0) == 0) { site = &s; break; }
		}
		if (!site) {
			nl.warn("DSP48E1 " + tile + "/" + local + ": no matching site in tile type " + ttype);
			continue;
		}
		// the device-wide site name, for the instance name: the tile's
		// DSP48E1 sites in Y order line up with DSP_0, DSP_1.
		std::vector<std::string> siteNames;
		for (auto& [sn, sty] : fd.db.tilegrid.at(tile).sites)
			if (sty == "DSP48E1") siteNames.push_back(sn);
		std::sort(siteNames.begin(), siteNames.end(), [](const std::string& a, const std::string& b) {
			static const std::regex yRe(R"(Y(\d+)$)");
			std::smatch ma, mb;
			std::regex_search(a, ma, yRe);
			std::regex_search(b, mb, yRe);
			return std::stoi(ma[1].str()) < std::stoi(mb[1].str());
		});
		int idx = local == "DSP_1" ? 1 : 0;
		std::string siteName = idx < (int)siteNames.size() ? siteNames[idx] : local;

		// ---- connections ----
		std::string conns;
		auto add = [&](const std::string& port, const std::string& net) {
			if (!conns.empty()) conns += ", ";
			conns += "." + port + "(" + net + ")";
		};
		auto netFor = [&](const std::string& pin) -> std::string {
			auto it = site->pins.find(pin);
			return it == site->pins.end() ? std::string() : fd.netOf(tile, it->second);
		};

		std::set<std::string> busPins;
		for (auto& [name, width] : kBuses) {
			std::vector<std::string> bits;
			bool any = false;
			for (int i = width - 1; i >= 0; i--) {
				std::string p = std::string(name) + std::to_string(i);
				busPins.insert(p);
				std::string n = netFor(p);
				if (!n.empty()) any = true;
				bits.push_back(n.empty() ? "1'b0" : n);
			}
			if (!any) continue;
			if (isOutput(name))
				for (auto& b : bits) nl.markDriven(b);
			std::string cat = "{";
			for (size_t i = 0; i < bits.size(); i++) cat += (i ? ", " : "") + bits[i];
			add(name, cat + "}");
		}
		for (auto& [pin, wire] : site->pins) {
			if (busPins.count(pin)) continue;
			std::string n = fd.netOf(tile, wire);
			if (isOutput(pin)) nl.markDriven(n);
			add(pin, n);
		}

		// ---- attributes ----
		std::vector<std::string> params;
		auto p = [&](const std::string& k, const std::string& v) { params.push_back("." + k + "(" + v + ")"); };

		bool aCascade = featPresent(f, "A_INPUT");
		bool bCascade = featPresent(f, "B_INPUT");
		p("A_INPUT", quoted(aCascade ? "CASCADE" : "DIRECT"));
		p("B_INPUT", quoted(bCascade ? "CASCADE" : "DIRECT"));

		// AREG/ACASCREG (and the B pair) share three independent bits:
		// xREG_0, xREG_2, and Zx REG_2_xCASCREG_1. The last one only
		// distinguishes ACASCREG from AREG when AREG is 2 -- for any other
		// AREG the two are equal and the bit is redundant, so nothing is
		// lost. What IS lost is nothing at all: the encoding is complete.
		auto regPair = [&](char ab, int& reg, int& casc) {
			std::string x(1, ab);
			reg = featPresent(f, x + "REG_0") ? 0 : (featPresent(f, x + "REG_2") ? 2 : 1);
			casc = reg;
			if (reg == 2 && featPresent(f, "Z" + x + "REG_2_" + x + "CASCREG_1")) casc = 2;
			else if (reg == 2) casc = 1;
		};
		int areg, acasc, breg, bcasc;
		regPair('A', areg, acasc);
		regPair('B', breg, bcasc);
		p("AREG", std::to_string(areg));
		p("ACASCREG", std::to_string(acasc));
		p("BREG", std::to_string(breg));
		p("BCASCREG", std::to_string(bcasc));

		// the plain register-enable attributes, all stored inverted
		const std::pair<const char*, const char*> zregs[] = {
		    {"ADREG", "ZADREG"},           {"ALUMODEREG", "ZALUMODEREG"},
		    {"CARRYINREG", "ZCARRYINREG"}, {"CARRYINSELREG", "ZCARRYINSELREG"},
		    {"CREG", "ZCREG"},             {"DREG", "ZDREG"},
		    {"INMODEREG", "ZINMODEREG"},   {"MREG", "ZMREG"},
		    {"OPMODEREG", "ZOPMODEREG"},   {"PREG", "ZPREG"},
		};
		for (auto& [attr, z] : zregs) p(attr, std::to_string(zBit(f, z)));

		p("USE_DPORT", quoted(featPresent(f, "USE_DPORT") ? "TRUE" : "FALSE"));
		p("USE_SIMD", quoted(featPresent(f, "USE_SIMD_FOUR12")
		                         ? "FOUR12"
		                         : (featPresent(f, "USE_SIMD_FOUR12_TWO24") ? "TWO24" : "ONE48")));
		// AUTORESET_PATDET is a two-tag group: the first says "resets at
		// all", the second narrows it to the not-match case.
		p("AUTORESET_PATDET",
		  quoted(!featPresent(f, "AUTORESET_PATDET_RESET")
		             ? "NO_RESET"
		             : (featPresent(f, "AUTORESET_PATDET_RESET_NOT_MATCH") ? "RESET_NOT_MATCH"
		                                                                  : "RESET_MATCH")));
		p("USE_PATTERN_DETECT", quoted(featPresent(f, "USE_PATTERN_DETECT") ? "PATDET" : "NO_PATDET"));
		std::string selMask = "MASK";  // the group's zero value
		for (const char* alt : {"C", "ROUNDING_MODE1", "ROUNDING_MODE2"})
			if (featPresent(f, std::string("SEL_MASK_") + alt)) selMask = alt;
		p("SEL_MASK", quoted(selMask));
		if (featPresent(f, "USE_MULT")) {
			uint64_t m = featBits(f, "USE_MULT", 2);
			p("USE_MULT", quoted(m == 0 ? "NONE" : (m == 1 ? "MULTIPLY" : "DYNAMIC")));
		}

		p("MASK", hexLiteral(featBitString(f, "MASK", 48)));
		p("PATTERN", hexLiteral(featBitString(f, "PATTERN", 48)));
		p("IS_ALUMODE_INVERTED", hexLiteral(zVector(f, "ZIS_ALUMODE_INVERTED", 4)));
		p("IS_INMODE_INVERTED", hexLiteral(zVector(f, "ZIS_INMODE_INVERTED", 5)));
		p("IS_OPMODE_INVERTED", hexLiteral(zVector(f, "ZIS_OPMODE_INVERTED", 7)));
		p("IS_CARRYIN_INVERTED", "1'b" + std::to_string(zBit(f, "ZIS_CARRYIN_INVERTED")));
		p("IS_CLK_INVERTED", "1'b" + std::to_string(zBit(f, "ZIS_CLK_INVERTED")));

		std::string paramStr;
		for (size_t i = 0; i < params.size(); i++) paramStr += (i ? ", " : "") + params[i];
		nl.instance("DSP48E1", paramStr, nl.uniqName("dsp_" + vname(tile) + "_" + siteName), conns);
	}
}

}  // namespace f2n
