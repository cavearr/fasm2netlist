// BRAM_L / BRAM_R extraction: RAMB18E1 and RAMB36E1.
//
// A BRAM tile holds two 18Kb halves that are either two independent RAMB18E1s
// or, ganged, one RAMB36E1. Which of the two it is has no feature bit of its
// own -- prjxray's RAMB36 tags are all "this bit is CLEAR" patterns, so a
// plain RAMB36 emits none of them (confirmed against a real nextpnr build
// whose eight RAMB36s carry no RAMB36.* line at all). What DOES distinguish
// them is unambiguous and comes from the routing rather than the config: a
// RAMB36 reaches the fabric through the tile's RAMBFIFO36E1 site pins, a pair
// of RAMB18s through the two 18Kb sites' own pins. So the mode is read off
// which site the tile's pips actually touch.
//
// Site pin names differ between the two 18Kb sites (prjxray types the lower
// one FIFO18E1, so its pins carry FIFO names), but each pin's WIRE is named
// "BRAM_<kind>_<canonical pin>" -- the same canonical name in all three
// sites. Stripping that prefix therefore recovers the primitive's own port
// name from the database instead of a hand-written translation table.
//
// Attribute encodings are prjxray's fuzzers 025-bram-config / 026-bram-data /
// 027-bram36-config, read as a specification.
#include "cells.hpp"

#include "bram_ports.hpp"

#include <algorithm>
#include <regex>

namespace f2n {

namespace {


// canonical pin name -> tile wire, for the site whose pins live on wires
// named "<prefix><canonical>".
std::map<std::string, std::string> canonPins(FasmDesign& fd, const std::string& ttype,
                                             const std::string& prefix) {
	std::map<std::string, std::string> out;
	const TileType& tt = fd.db.tileType(ttype);
	for (auto& s : tt.sites) {
		bool match = !s.pins.empty();
		for (auto& [p, w] : s.pins)
			if (w.rfind(prefix, 0) != 0) { match = false; break; }
		if (!match) continue;
		for (auto& [p, w] : s.pins) out[w.substr(prefix.size())] = w;
		break;
	}
	return out;
}

std::string invertBits(std::string b) {
	for (char& c : b) c = c == '1' ? '0' : '1';
	return b;
}

// A width tag group: exactly one of NAME_1/_2/_4/_9/_18 is set, so the width
// reads straight off whichever appeared.
int widthTag(const FeatMap& f, const std::string& base) {
	for (int w : {2, 4, 9, 18})
		if (featPresent(f, base + "_" + std::to_string(w))) return w;
	return 1;
}

std::string quoted(const std::string& s) { return "\"" + s + "\""; }

std::string writeMode(const FeatMap& f, const std::string& ab) {
	if (featPresent(f, "WRITE_MODE_" + ab + "_READ_FIRST")) return "READ_FIRST";
	if (featPresent(f, "WRITE_MODE_" + ab + "_NO_CHANGE")) return "NO_CHANGE";
	return "WRITE_FIRST";
}

// The inversion attributes are stored complemented ("ZINV_"), so an absent
// tag is an inverted pin.
void invParams(const FeatMap& f, std::vector<std::string>& params) {
	static const char* pins[] = {"CLKARDCLK",     "CLKBWRCLK", "ENARDEN",       "ENBWREN",
	                             "REGCLKARDRCLK", "REGCLKB",   "RSTRAMARSTRAM", "RSTRAMB",
	                             "RSTREGARSTREG", "RSTREGB"};
	for (const char* p : pins)
		params.push_back(".IS_" + std::string(p) + "_INVERTED(1'b" +
		                 (featPresent(f, "ZINV_" + std::string(p)) ? "0" : "1") + ")");
}

// The memory contents: 64 rows of 256 bits plus 8 parity rows. Rows that are
// all zero are left off -- that is the primitive's own default, so the result
// is exact and the netlist stays readable.
void contentParams(const FeatMap& f, std::vector<std::string>& params, const char* prefix, int nInit,
                   int nInitP) {
	auto row = [&](const char* kind, int idx) {
		char key[32];
		snprintf(key, sizeof(key), "%s_%02X", kind, idx);
		std::string bits = featBitString(f, key, 256);
		if (bits.find('1') == std::string::npos) return;
		params.push_back("." + std::string(prefix) + key + "(" + hexLiteral(bits) + ")");
	};
	for (int i = 0; i < nInit; i++) row("INIT", i);
	for (int i = 0; i < nInitP; i++) row("INITP", i);
}

// ---------------------------------------------------------------------

void emitRamb18(FasmDesign& fd, Netlist& nl, const std::string& tile, const std::string& ttype,
                const std::string& siteName, const std::string& wirePrefix, const FeatMap& f) {
	auto pins = canonPins(fd, ttype, wirePrefix);
	std::string conns;
	auto add = [&](const std::string& port, const std::string& net) {
		if (!conns.empty()) conns += ", ";
		conns += "." + port + "(" + net + ")";
	};
	for (auto& p : bram::kRamb18) {
		if (p.width == 0) {
			auto it = pins.find(p.name);
			if (it == pins.end()) continue;
			std::string n = fd.netOf(tile, it->second);
			if (p.out) nl.markDriven(n);
			add(p.name, n);
			continue;
		}
		std::vector<std::string> bits;
		bool any = false;
		for (int i = p.width - 1; i >= 0; i--) {
			auto it = pins.find(std::string(p.name) + std::to_string(i));
			std::string n = it == pins.end() ? std::string() : fd.netOf(tile, it->second);
			if (!n.empty()) any = true;
			bits.push_back(n.empty() ? "1'b0" : n);
		}
		if (!any) continue;
		std::string cat = "{";
		for (size_t i = 0; i < bits.size(); i++) {
			cat += (i ? ", " : "") + bits[i];
			if (p.out) nl.markDriven(bits[i]);
		}
		add(p.name, cat + "}");
	}

	std::vector<std::string> params;
	bool sdpRead = featPresent(f, "SDP_READ_WIDTH_36");
	bool sdpWrite = featPresent(f, "SDP_WRITE_WIDTH_36");
	params.push_back(".RAM_MODE(" + quoted(sdpRead || sdpWrite ? "SDP" : "TDP") + ")");
	params.push_back(".READ_WIDTH_A(" + std::to_string(sdpRead ? 36 : widthTag(f, "READ_WIDTH_A")) + ")");
	params.push_back(".READ_WIDTH_B(" + std::to_string(sdpRead ? 0 : widthTag(f, "READ_WIDTH_B")) + ")");
	params.push_back(".WRITE_WIDTH_A(" + std::to_string(sdpWrite ? 0 : widthTag(f, "WRITE_WIDTH_A")) + ")");
	params.push_back(".WRITE_WIDTH_B(" + std::to_string(sdpWrite ? 36 : widthTag(f, "WRITE_WIDTH_B")) + ")");
	params.push_back(".DOA_REG(" + std::to_string(featPresent(f, "DOA_REG") ? 1 : 0) + ")");
	params.push_back(".DOB_REG(" + std::to_string(featPresent(f, "DOB_REG") ? 1 : 0) + ")");
	for (const char* ab : {"A", "B"}) {
		params.push_back(".INIT_" + std::string(ab) + "(" +
		                 hexLiteral(invertBits(featBitString(f, "ZINIT_" + std::string(ab), 18))) + ")");
		params.push_back(".SRVAL_" + std::string(ab) + "(" +
		                 hexLiteral(invertBits(featBitString(f, "ZSRVAL_" + std::string(ab), 18))) + ")");
		params.push_back(".WRITE_MODE_" + std::string(ab) + "(" + quoted(writeMode(f, ab)) + ")");
		params.push_back(".RSTREG_PRIORITY_" + std::string(ab) + "(" +
		                 quoted(featPresent(f, "RSTREG_PRIORITY_" + std::string(ab) + "_REGCE") ? "REGCE"
		                                                                                       : "RSTREG") +
		                 ")");
	}
	params.push_back(".RDADDR_COLLISION_HWCONFIG(" +
	                 quoted(featPresent(f, "RDADDR_COLLISION_HWCONFIG_PERFORMANCE") ? "PERFORMANCE"
	                                                                                : "DELAYED_WRITE") +
	                 ")");
	invParams(f, params);
	contentParams(f, params, "", 0x40, 8);

	if (featPresent(f, "FIFO_MODE"))
		nl.warn(tile + "/" + siteName + ": FIFO_MODE is set; emitted as a plain RAMB18E1");

	std::string paramStr;
	for (size_t i = 0; i < params.size(); i++) paramStr += (i ? ", " : "") + params[i];
	nl.instance("RAMB18E1", paramStr, nl.uniqName("bram18_" + vname(tile) + "_" + siteName), conns);
}

// The 36Kb cell's rows are the two 18Kb halves' rows interleaved bit by bit:
// the lower half supplies the even bits of each output row and the upper half
// the odd ones, with each 256-bit source row spread across two output rows.
// (prjxray stores the halves as they physically sit; this is the inverse of
// how Vivado splits a RAMB36E1's contents across them.)
std::map<std::string, std::string> deinterleave36(const FeatMap& lo, const FeatMap& hi) {
	auto src = [&](const FeatMap& f, const char* kind, int idx) {
		char key[32];
		snprintf(key, sizeof(key), "%s_%02X", kind, idx);
		return featBitString(f, key, 256);
	};
	auto bit = [](const std::string& s, int i) { return s[255 - i]; };

	std::map<std::string, std::string> out;
	auto build = [&](const char* kind, int half) {
		for (int idx = 0; idx < half; idx++) {
			std::string a = src(lo, kind, idx), b = src(hi, kind, idx);
			std::string r0(256, '0'), r1(256, '0');
			for (int k = 0; k < 128; k++) {
				r0[255 - 2 * k] = bit(a, k);
				r0[255 - (2 * k + 1)] = bit(b, k);
				r1[255 - 2 * k] = bit(a, 128 + k);
				r1[255 - (2 * k + 1)] = bit(b, 128 + k);
			}
			char k0[32], k1[32];
			snprintf(k0, sizeof(k0), "%s_%02X", kind, idx * 2);
			snprintf(k1, sizeof(k1), "%s_%02X", kind, idx * 2 + 1);
			out[k0] = r0;
			out[k1] = r1;
		}
	};
	build("INIT", 0x40);
	build("INITP", 8);
	return out;
}

void emitRamb36(FasmDesign& fd, Netlist& nl, const std::string& tile, const std::string& ttype,
                const std::string& siteName, const FeatMap& y0, const FeatMap& y1, const FeatMap& f36) {
	auto pins = canonPins(fd, ttype, "BRAM_FIFO36_");
	std::string conns;
	auto add = [&](const std::string& port, const std::string& net) {
		if (!conns.empty()) conns += ", ";
		conns += "." + port + "(" + net + ")";
	};
	auto netAt = [&](const std::string& p) -> std::string {
		auto it = pins.find(p);
		return it == pins.end() ? std::string() : fd.netOf(tile, it->second);
	};
	// The two halves see the same signal in 36Kb mode, so their nets
	// disagreeing is worth reporting -- but only as a note, because net
	// IDENTITY is stricter than the thing that actually matters. A router is
	// free to buffer one leg through a spare LUT, which leaves the two pins
	// on different nets carrying the same value; that is the common case
	// when this fires. It is still worth saying, since a genuine mismatch
	// means the tile is not really one 36Kb block.
	//
	// The output pipeline register's clock is a don't-care when that
	// register is bypassed, and the tile's default routing does leave the
	// two halves' REGCLK pins on different clock-mesh wires -- so do not
	// cross-check that one at all unless the register is in use.
	auto crossCheck = [&](const std::string& port) {
		if (port == "REGCLKARDRCLK") return featPresent(y0, "DOA_REG");
		if (port == "REGCLKB") return featPresent(y0, "DOB_REG");
		return true;
	};
	for (auto& p : bram::kRamb36) {
		std::vector<std::string> bits;
		bool any = false;
		int n = p.width == 0 ? 1 : p.width;
		for (int i = n - 1; i >= 0; i--) {
			std::string suffix = p.width == 0 ? "" : std::to_string(i);
			std::string net = netAt(p.pin + suffix);
			if (p.pinU) {
				std::string u = netAt(p.pinU + suffix);
				// In 36Kb mode both halves see the same signal; if the
				// routing says otherwise this tile is not really one RAMB36.
				if (!u.empty() && !net.empty() && u != net && crossCheck(p.port))
					nl.warn(tile + "/" + siteName + ": " + p.port + suffix +
					        " reaches the two 18Kb halves on different nets (" + net + " vs " + u +
					        "); equal values buffered apart look like this too, so this is a note "
					        "rather than a fault");
				if (net.empty()) net = u;
			}
			if (!net.empty()) any = true;
			bits.push_back(net.empty() ? "1'b0" : net);
		}
		if (!any) continue;
		if (p.out)
			for (auto& b : bits) nl.markDriven(b);
		if (p.width == 0) {
			add(p.port, bits[0]);
		} else {
			std::string cat = "{";
			for (size_t i = 0; i < bits.size(); i++) cat += (i ? ", " : "") + bits[i];
			add(p.port, cat + "}");
		}
	}

	// A 36Kb port is one rung further up the width ladder than the 18Kb tag
	// its lower half carries -- 2 -> 4, 4 -> 9, 9 -> 18, 18 -> 36, since the
	// 9/18 rungs carry parity bits and so do not simply double. The bottom
	// rung is the only ambiguous one (an 18Kb tag of 1 means either 1 or 2),
	// and the tile's own BRAM36_..._1 tag is exactly what resolves it.
	auto width36 = [&](const std::string& which) {
		switch (widthTag(y0, which)) {
			case 2: return 4;
			case 4: return 9;
			case 9: return 18;
			case 18: return 36;
			default: return featPresent(f36, "BRAM36_" + which + "_1") ? 1 : 2;
		}
	};
	bool sdpRead = featPresent(y0, "SDP_READ_WIDTH_36");
	bool sdpWrite = featPresent(y0, "SDP_WRITE_WIDTH_36");

	std::vector<std::string> params;
	params.push_back(".RAM_MODE(" + quoted(sdpRead || sdpWrite ? "SDP" : "TDP") + ")");
	params.push_back(".READ_WIDTH_A(" + std::to_string(sdpRead ? 72 : width36("READ_WIDTH_A")) + ")");
	params.push_back(".READ_WIDTH_B(" + std::to_string(sdpRead ? 0 : width36("READ_WIDTH_B")) + ")");
	params.push_back(".WRITE_WIDTH_A(" + std::to_string(sdpWrite ? 0 : width36("WRITE_WIDTH_A")) + ")");
	params.push_back(".WRITE_WIDTH_B(" + std::to_string(sdpWrite ? 72 : width36("WRITE_WIDTH_B")) + ")");
	params.push_back(".DOA_REG(" + std::to_string(featPresent(y0, "DOA_REG") ? 1 : 0) + ")");
	params.push_back(".DOB_REG(" + std::to_string(featPresent(y0, "DOB_REG") ? 1 : 0) + ")");
	for (const char* ab : {"A", "B"}) {
		// the 36-bit values are the upper half's 18 bits above the lower's
		for (const char* what : {"INIT", "SRVAL"}) {
			std::string tag = std::string("Z") + what + "_" + ab;
			std::string bits = invertBits(featBitString(y1, tag, 18) + featBitString(y0, tag, 18));
			params.push_back("." + std::string(what) + "_" + ab + "(" + hexLiteral(bits) + ")");
		}
		params.push_back(".WRITE_MODE_" + std::string(ab) + "(" + quoted(writeMode(y0, ab)) + ")");
		params.push_back(".RSTREG_PRIORITY_" + std::string(ab) + "(" +
		                 quoted(featPresent(y0, "RSTREG_PRIORITY_" + std::string(ab) + "_REGCE") ? "REGCE"
		                                                                                        : "RSTREG") +
		                 ")");
		params.push_back(".RAM_EXTENSION_" + std::string(ab) + "(" +
		                 quoted(featPresent(f36, "RAM_EXTENSION_" + std::string(ab) + "_LOWER") ? "LOWER"
		                                                                                       : "NONE") +
		                 ")");
	}
	params.push_back(".RDADDR_COLLISION_HWCONFIG(" +
	                 quoted(featPresent(y0, "RDADDR_COLLISION_HWCONFIG_PERFORMANCE") ? "PERFORMANCE"
	                                                                                 : "DELAYED_WRITE") +
	                 ")");
	params.push_back(".EN_ECC_READ(" + quoted(featPresent(f36, "EN_ECC_READ") ? "TRUE" : "FALSE") + ")");
	params.push_back(".EN_ECC_WRITE(" + quoted(featPresent(f36, "EN_ECC_WRITE") ? "TRUE" : "FALSE") + ")");
	invParams(y0, params);

	for (auto& [key, bits] : deinterleave36(y0, y1))
		if (bits.find('1') != std::string::npos) params.push_back("." + key + "(" + hexLiteral(bits) + ")");

	std::string paramStr;
	for (size_t i = 0; i < params.size(); i++) paramStr += (i ? ", " : "") + params[i];
	nl.instance("RAMB36E1", paramStr, nl.uniqName("bram36_" + vname(tile) + "_" + siteName), conns);
}

}  // namespace

void emitBramCells(FasmDesign& fd, Netlist& nl) {
	// gather the tiles that have any BRAM feature at all
	std::map<std::string, std::map<std::string, const FeatMap*>> byTile;
	for (auto& kv : fd.bram_feats) byTile[kv.first.first][kv.first.second] = &kv.second;

	static const FeatMap kEmpty;
	for (auto& [tile, sites] : byTile) {
		const std::string& ttype = fd.used_tiles[tile];
		auto at = [&](const char* k) -> const FeatMap& {
			auto it = sites.find(k);
			return it == sites.end() ? kEmpty : *it->second;
		};
		const FeatMap& y0 = at("RAMB18_Y0");
		const FeatMap& y1 = at("RAMB18_Y1");
		const FeatMap& f36 = at("RAMB36");

		// 36Kb mode: read off which site the tile's routing actually
		// touches, not off the configuration (see the file header).
		bool is36 = false;
		auto pit = fd.real_pips.find(tile);
		if (pit != fd.real_pips.end())
			for (auto& [dst, src] : pit->second)
				if (dst.rfind("BRAM_FIFO36_", 0) == 0 || src.rfind("BRAM_FIFO36_", 0) == 0) {
					is36 = true;
					break;
				}

		// device-wide site names, so the instance names locate the cell
		std::vector<std::string> r18;
		std::string r36Name;
		for (auto& [sn, sty] : fd.db.tilegrid.at(tile).sites) {
			if (sn.rfind("RAMB18_", 0) == 0) r18.push_back(sn);
			else if (sn.rfind("RAMB36_", 0) == 0) r36Name = sn;
		}
		static const std::regex yRe(R"(Y(\d+)$)");
		std::sort(r18.begin(), r18.end(), [](const std::string& a, const std::string& b) {
			std::smatch ma, mb;
			std::regex_search(a, ma, yRe);
			std::regex_search(b, mb, yRe);
			return std::stoi(ma[1].str()) < std::stoi(mb[1].str());
		});

		if (is36) {
			emitRamb36(fd, nl, tile, ttype, r36Name.empty() ? "RAMB36" : r36Name, y0, y1, f36);
			continue;
		}
		if (featPresent(y0, "IN_USE"))
			emitRamb18(fd, nl, tile, ttype, r18.size() > 0 ? r18[0] : "RAMB18_Y0", "BRAM_FIFO18_", y0);
		if (featPresent(y1, "IN_USE"))
			emitRamb18(fd, nl, tile, ttype, r18.size() > 1 ? r18[1] : "RAMB18_Y1", "BRAM_RAMB18_", y1);
	}
}

}  // namespace f2n
