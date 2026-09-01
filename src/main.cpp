// fasm2netlist: FASM -> anonymous Xilinx-primitive Verilog netlist, in C++.
//
// Standalone C++ port of a Python prototype's FasmDesign core (from the
// xc7-bitstream-tools project's scripts/bit2gates.py) -- the "no cheating"
// logic: LUT/FF existence and connectivity derived purely from the FASM +
// fixed prjxray tile/pip tables (tilegrid.json / tileconn.json /
// tile_type_*.json / ppips_*.db, all properties of the silicon, not of any
// specific build), never from a placement or routed-JSON dump; cell names
// are anonymous/auto-generated, not carried over from any toolchain-internal
// source. This first pass covers the SLICE fabric (LUT6_2 + FDRE/FDSE/
// FDCE/FDPE) plus top-level IO (IBUF/IBUFDS/OBUF/BUFG, resolved via .xdc +
// package_pins.csv when --xdc and --part/--bit are given).
//
// Scope decisions (flagged explicitly rather than silently assumed):
//  - FASM tokenizing is a small hand-written line parser scoped to the
//    regular subset prjxray's own FASM output actually uses (plain feature
//    lines, two-wire PIP lines, "SITE.xLUT.INIT[hi:lo] = N'bBITS" value
//    lines) -- not a full grammar implementation of the FASM language.
//    Project X-Ray vendors a real ANTLR-grammar FASM parser
//    (github.com/chipsalliance/fasm), which would be the "proper parser"
//    choice; its C++ runtime wasn't wired into this tool because pulling in
//    the full antlr4-cpp-runtime as a fresh, unproven build dependency was
//    judged not worth it for this pass. Swapping it in later is a contained
//    change (parseFasm() is the only place that would need to change).
//  - JSON parsing uses this project's own minimal json.hpp -- deliberately
//    self-contained (no vendored third-party dependency at all right now).
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
#include <unordered_map>
#include <vector>

#include "json.hpp"

namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------
// small utilities
// ---------------------------------------------------------------------

std::string readFile(const std::string& path) {
	std::ifstream f(path, std::ios::binary);
	if (!f) throw std::runtime_error("cannot open file: " + path);
	std::ostringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

json::Value readJson(const std::string& path) {
	return json::parse(readFile(path));
}

std::string toLower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
	return s;
}

// legal Verilog identifier from a tile/wire name -- matches bit2verilog.py /
// bit2gates.py's vname() exactly.
std::string vname(const std::string& s) {
	std::string out = s;
	for (auto& c : out)
		if (!std::isalnum((unsigned char)c) && c != '_') c = '_';
	return out;
}

std::vector<std::string> splitAll(const std::string& s, char sep) {
	std::vector<std::string> out;
	size_t start = 0;
	while (true) {
		size_t pos = s.find(sep, start);
		if (pos == std::string::npos) {
			out.push_back(s.substr(start));
			break;
		}
		out.push_back(s.substr(start, pos - start));
		start = pos + 1;
	}
	return out;
}

bool endsWith(const std::string& s, const std::string& suffix) {
	return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

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

// ---------------------------------------------------------------------
// fixed prjxray technology database (tilegrid / tileconn / tile_type / ppips)
// ---------------------------------------------------------------------

struct Site {
	std::string type;
	double x_coord = 0;
	std::map<std::string, std::string> pins;  // pin name -> wire name
};

struct TileType {
	std::set<std::string> wires;
	std::set<std::pair<std::string, std::string>> pips;  // (dst_wire, src_wire)
	std::vector<Site> sites;                              // in file order
};

struct Tile {
	std::string type;
	int grid_x = 0, grid_y = 0;
	std::map<std::string, std::string> sites;  // site name -> site type
};

struct TileConnEntry {
	std::string t0, t1;
	int dx = 0, dy = 0;
	std::vector<std::pair<std::string, std::string>> wire_pairs;
};

class Database {
       public:
	std::map<std::string, Tile> tilegrid;
	std::vector<TileConnEntry> tileconn;
	std::map<std::string, TileType> tile_types;  // loaded on demand, keyed by tile TYPE string
	std::string family_dir;

	Database(const std::string& db_root, const std::string& family, const std::string& device) {
		family_dir = db_root + "/" + family;
		loadTilegrid(family_dir + "/" + device + "/tilegrid.json");
		loadTileconn(family_dir + "/tileconn.json");
	}

	const TileType& tileType(const std::string& type) {
		auto it = tile_types.find(type);
		if (it != tile_types.end()) return it->second;
		TileType tt;
		std::string path = family_dir + "/tile_type_" + type + ".json";
		std::ifstream probe(path);
		if (probe.good()) {
			json::Value j = readJson(path);
			for (auto& kv : j.get("wires").members()) tt.wires.insert(kv.first);
			for (auto& kv : j.get("pips").members()) {
				const json::Value& p = kv.second;
				tt.pips.emplace(p.get("dst_wire").asString(), p.get("src_wire").asString());
			}
			for (auto& sv : j.get("sites").items()) {
				Site s;
				s.type = sv.get("type").asString();
				s.x_coord = sv.get("x_coord").asDouble();
				for (auto& pkv : sv.get("site_pins").members()) {
					const json::Value& w = pkv.second.get("wire");
					if (w.isString()) s.pins[pkv.first] = w.asString();
				}
				tt.sites.push_back(std::move(s));
			}
		}
		auto ins = tile_types.emplace(type, std::move(tt));
		return ins.first->second;
	}

       private:
	void loadTilegrid(const std::string& path) {
		json::Value j = readJson(path);
		for (auto& kv : j.members()) {
			Tile t;
			t.type = kv.second.get("type").asString();
			t.grid_x = (int)kv.second.get("grid_x").asInt();
			t.grid_y = (int)kv.second.get("grid_y").asInt();
			for (auto& skv : kv.second.get("sites").members())
				t.sites[skv.first] = skv.second.asString();
			tilegrid.emplace(kv.first, std::move(t));
		}
	}

	void loadTileconn(const std::string& path) {
		json::Value j = readJson(path);
		for (auto& e : j.items()) {
			TileConnEntry c;
			c.t0 = e.get("tile_types").items()[0].asString();
			c.t1 = e.get("tile_types").items()[1].asString();
			c.dx = (int)e.get("grid_deltas").items()[0].asInt();
			c.dy = (int)e.get("grid_deltas").items()[1].asInt();
			for (auto& wp : e.get("wire_pairs").items())
				c.wire_pairs.emplace_back(wp.items()[0].asString(), wp.items()[1].asString());
			tileconn.push_back(std::move(c));
		}
	}
};

// ---------------------------------------------------------------------
// FASM tokenizing -- see the scope note in the file header.
// ---------------------------------------------------------------------

struct RawFasm {
	std::map<std::string, std::string> used_tiles;                              // tile -> type
	std::map<std::string, std::vector<std::pair<std::string, std::string>>> pips;  // tile -> [(dst,src)]
	std::map<std::string, std::vector<std::string>> site_feats;                 // tile -> ["rest", ...]
};

RawFasm parseFasm(const std::string& path, const Database& db) {
	RawFasm r;
	std::ifstream f(path);
	if (!f) throw std::runtime_error("cannot open fasm: " + path);
	// tile.WIRE1.WIRE2 with nothing else on the line (no '[', '=', ' ') --
	// same shape as bit2gates.py's pip_re.
	static const std::regex pipRe(R"(^([A-Z0-9_]+_X\d+Y\d+)\.([A-Za-z0-9_]+)\.([A-Za-z0-9_]+)\s*$)");
	std::string line;
	while (std::getline(f, line)) {
		// strip trailing comment + whitespace
		size_t hash = line.find('#');
		if (hash != std::string::npos) line.resize(hash);
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
			line.pop_back();
		if (line.empty()) continue;
		size_t dot = line.find('.');
		if (dot == std::string::npos) continue;
		std::string tile = line.substr(0, dot);
		if (!db.tilegrid.count(tile)) continue;
		r.used_tiles[tile] = db.tilegrid.at(tile).type;
		std::string rest = line.substr(dot + 1);
		std::smatch m;
		bool plainPip = std::regex_match(line, m, pipRe) && rest.find('[') == std::string::npos &&
		                rest.find('=') == std::string::npos && rest.find(' ') == std::string::npos;
		if (plainPip) {
			r.pips[tile].emplace_back(m[2].str(), m[3].str());
		} else {
			r.site_feats[tile].push_back(rest);
		}
	}
	return r;
}

// ---------------------------------------------------------------------
// FasmDesign: the union-find net reconstruction + feature classification --
// this is the direct C++ mirror of bit2gates.py's FasmDesign class.
// ---------------------------------------------------------------------

struct FeatureValue {
	bool isInit = false;
	int width = 0;
	std::string bits;  // MSB-first, matches bit2gates.py's convention
};
using FeatMap = std::map<std::string, FeatureValue>;

class DisjointSet {
       public:
	std::string find(const std::string& x) {
		auto it = parent_.find(x);
		if (it == parent_.end()) {
			parent_[x] = x;
			return x;
		}
		std::string root = x;
		while (parent_[root] != root) root = parent_[root];
		// path compression
		std::string cur = x;
		while (parent_[cur] != root) {
			std::string next = parent_[cur];
			parent_[cur] = root;
			cur = next;
		}
		return root;
	}
	void unite(const std::string& a, const std::string& b) {
		std::string ra = find(a), rb = find(b);
		if (ra != rb) parent_[ra] = rb;
	}

       private:
	std::unordered_map<std::string, std::string> parent_;
};

// key for a (tile,wire) pair used throughout as a DSU / map key
std::string tw(const std::string& tile, const std::string& wire) { return tile + "\x1f" + wire; }

class FasmDesign {
       public:
	Database& db;
	std::map<std::string, std::string> used_tiles;
	std::map<std::string, std::vector<std::pair<std::string, std::string>>> real_pips;
	std::map<std::pair<std::string, std::string>, FeatMap> slice_feats;  // (tile,suffix) -> feature map
	std::map<std::pair<std::string, std::string>, std::set<std::string>> iob_feats;
	std::map<std::pair<std::string, std::string>, std::set<std::string>> bufg_feats;

	DisjointSet dsu;
	std::unordered_map<std::string, std::string> netname;  // dsu-root -> emitted wire name

	FasmDesign(const std::string& fasmPath, Database& database) : db(database) {
		RawFasm raw = parseFasm(fasmPath, db);
		used_tiles = raw.used_tiles;

		// ---- classify raw pip-shaped lines: real routing pip, or
		// bel-internal route-through (both endpoints are real wires of this
		// tile type but the pip isn't in the general routing table), or
		// -- the case an earlier version of this port silently dropped --
		// neither, in which case it was never really a pip at all: it's a
		// plain two-level site feature (e.g. "SLICEM_X0.FFSYNC") that only
        // LOOKED like a pip to the tokenizer's shape-only regex. Mirrors
		// bit2gates.py's own reclassification exactly: push it back as a
		// "dst.src" site-feature line rather than discarding it -- losing
		// this fallback silently dropped every site-wide boolean feature
		// (FFSYNC, CEUSEDMUX, SRUSEDMUX, NOCLKINV, ...), which don't have
		// their own per-column INIT to independently confirm they parsed.
		std::set<std::string> usedTypes;
		for (auto& kv : used_tiles) usedTypes.insert(kv.second);
		for (auto& t : usedTypes) db.tileType(t);  // pre-load

		for (auto& kv : raw.pips) {
			const std::string& tile = kv.first;
			const std::string& ty = used_tiles[tile];
			const TileType& tt = db.tileType(ty);
			for (auto& ds : kv.second) {
				bool isRealPip = tt.pips.count(ds);
				bool bothRealWires = tt.wires.count(ds.first) && tt.wires.count(ds.second);
				if (isRealPip || bothRealWires) {
					real_pips[tile].push_back(ds);
					if (!isRealPip) raw.site_feats[tile].push_back(ds.first + "." + ds.second);
				} else {
					raw.site_feats[tile].push_back(ds.first + "." + ds.second);
				}
			}
		}

		// ---- classify each (now-complete) site-feature line ----
		static const std::regex initRe(R"(^([A-D])LUT\.INIT\[\d+:\d+\]\s*=\s*(\d+)'b([01]+))");
		static const std::regex bufgRe(R"(^BUFGCTRL\.(BUFGCTRL_X\d+Y\d+)\.(.+)$)");
		for (auto& kv : raw.site_feats) {
			const std::string& tile = kv.first;
			for (auto& fe : kv.second) {
				size_t dot = fe.find('.');
				if (dot == std::string::npos) continue;
				std::string site = fe.substr(0, dot);
				std::string restField = fe.substr(dot + 1);
				if (site.rfind("SLICE", 0) == 0) {
					std::smatch m;
					if (std::regex_search(restField, m, initRe)) {
						FeatureValue v;
						v.isInit = true;
						v.width = std::stoi(m[2].str());
						v.bits = m[3].str();
						slice_feats[{tile, site}][m[1].str() + "INIT"] = v;
					} else {
						slice_feats[{tile, site}][restField] = FeatureValue{};
					}
				} else if (site.rfind("IOB_Y", 0) == 0) {
					iob_feats[{tile, site}].insert(restField);
				}
				std::smatch bm;
				if (std::regex_match(fe, bm, bufgRe)) bufg_feats[{tile, bm[1].str()}].insert(bm[2].str());
			}
		}

		buildUnionFind(usedTypes);
	}

	std::string netOf(const std::string& tile, const std::string& wire) {
		if (endsWithAny(wire, {"VCC_WIRE", "DSP_VCC_L", "DSP_VCC_R"})) return "1'b1";
		if (endsWithAny(wire, {"GND_WIRE", "DSP_GND_L", "DSP_GND_R"})) return "1'b0";
		std::string root = dsu.find(tw(tile, wire));
		auto it = netname.find(root);
		if (it != netname.end()) return it->second;
		size_t sep = root.find('\x1f');
		std::string name = "n_" + vname(root.substr(0, sep)) + "__" + vname(root.substr(sep + 1));
		netname[root] = name;
		return name;
	}

	std::string fasmSiteSuffix(const std::string& tile, const std::string& site) {
		const std::string& realType = db.tilegrid.at(tile).sites.at(site);
		int ord = siteOrdinal(tile, site);
		return realType + "_X" + std::to_string(ord);
	}

	// inverse of fasmSiteSuffix(): "SLICEM_X0" -> real site name
	std::string siteFromSuffix(const std::string& tile, const std::string& suffix) {
		static const std::regex sufRe(R"(^([A-Z0-9]+)_X(\d+)$)");
		std::smatch m;
		if (!std::regex_match(suffix, m, sufRe)) return "";
		std::string realType = m[1].str();
		int ordinal = std::stoi(m[2].str());
		const auto& sites = db.tilegrid.at(tile).sites;
		std::map<std::string, std::vector<std::string>> byPrefix;
		static const std::regex nameRe(R"(^([A-Z]+)_X(\d+)Y\d+$)");
		for (auto& kv : sites) {
			std::smatch nm;
			if (std::regex_match(kv.first, nm, nameRe)) byPrefix[nm[1].str()].push_back(kv.first);
		}
		for (auto& kv : byPrefix) {
			auto& group = kv.second;
			std::sort(group.begin(), group.end(), [](const std::string& a, const std::string& b) {
				static const std::regex xRe(R"(_X(\d+)Y)");
				std::smatch ma, mb;
				std::regex_search(a, ma, xRe);
				std::regex_search(b, mb, xRe);
				return std::stoi(ma[1].str()) < std::stoi(mb[1].str());
			});
			if (ordinal < (int)group.size() && sites.at(group[ordinal]) == realType) return group[ordinal];
		}
		return "";
	}

	int siteOrdinal(const std::string& tile, const std::string& site) {
		static const std::regex nameRe(R"(^([A-Z]+)_X\d+Y\d+$)");
		std::smatch m;
		std::string prefix = site;
		if (std::regex_match(site, m, nameRe)) prefix = m[1].str();
		const auto& sites = db.tilegrid.at(tile).sites;
		std::vector<std::string> same;
		for (auto& kv : sites)
			if (kv.first.rfind(prefix + "_X", 0) == 0) same.push_back(kv.first);
		static const std::regex xRe(R"(_X(\d+)Y)");
		std::sort(same.begin(), same.end(), [](const std::string& a, const std::string& b) {
			std::smatch ma, mb;
			std::regex_search(a, ma, xRe);
			std::regex_search(b, mb, xRe);
			return std::stoi(ma[1].str()) < std::stoi(mb[1].str());
		});
		for (size_t i = 0; i < same.size(); i++)
			if (same[i] == site) return (int)i;
		return -1;
	}

	std::map<std::string, std::string> sitePins(const std::string& tileType, int ordinal) {
		std::map<std::string, std::string> out;
		if (ordinal < 0) return out;
		const TileType& tt = db.tileType(tileType);
		std::vector<const Site*> sorted;
		for (auto& s : tt.sites) sorted.push_back(&s);
		std::stable_sort(sorted.begin(), sorted.end(),
		                  [](const Site* a, const Site* b) { return a->x_coord < b->x_coord; });
		if (ordinal >= (int)sorted.size()) return out;
		return sorted[ordinal]->pins;
	}

	// IOB18S/IOB18M (and similarly-paired site types) both report x_coord 0
	// in the tile_type JSON -- sitePins()'s x_coord ranking can't tell them
	// apart, and picking the wrong one silently swaps which physical wire an
	// OBUF/IBUF resolves to. Use this instead whenever the real site type is
	// already known (from the tilegrid, cross-referenced via a package pin).
	std::map<std::string, std::string> sitePinsByType(const std::string& tileType, const std::string& siteType) {
		const TileType& tt = db.tileType(tileType);
		for (auto& s : tt.sites)
			if (s.type == siteType) return s.pins;
		return {};
	}

	// real site name -> FASM's "IOB_Y0"/"IOB_Y1" suffix. Fixed, silicon-level
	// rule (verified against golden Vivado FASM, mirrors bit2gates.py's
	// fasm_iob_suffix() exactly): a paired M/S tile's "M" (master) site is
	// suffix Y0, "S" (slave) is Y1; a SING (single, unpaired) tile has
	// exactly one site and it is always suffix Y1.
	std::string fasmIobSuffix(const std::string& tile, const std::string& site) {
		const auto& sites = db.tilegrid.at(tile).sites;
		if (sites.size() == 1) return "IOB_Y1";
		auto it = sites.find(site);
		std::string styp = it != sites.end() ? it->second : "";
		if (!styp.empty() && styp.back() == 'M') return "IOB_Y0";
		if (!styp.empty() && styp.back() == 'S') return "IOB_Y1";
		return "";
	}

	// FASM addresses a BUFGCTRL by a tile-LOCAL index ("BUFGCTRL_X0Y5"), not
	// its real device-wide site name -- sort this tile's real BUFGCTRL sites
	// by Y ascending, the local index selects directly into that list.
	std::string bufgctrlRealSite(const std::string& tile, const std::string& fasmLocalSite) {
		static const std::regex re(R"(^BUFGCTRL_X\d+Y(\d+)$)");
		std::smatch m;
		if (!std::regex_match(fasmLocalSite, m, re)) return "";
		int localIdx = std::stoi(m[1].str());
		std::vector<std::string> cands;
		for (auto& [s, ty] : db.tilegrid.at(tile).sites)
			if (ty == "BUFGCTRL") cands.push_back(s);
		static const std::regex yRe(R"(Y(\d+)$)");
		std::sort(cands.begin(), cands.end(), [](const std::string& a, const std::string& b) {
			std::smatch ma, mb;
			std::regex_search(a, ma, yRe);
			std::regex_search(b, mb, yRe);
			return std::stoi(ma[1].str()) < std::stoi(mb[1].str());
		});
		return localIdx < (int)cands.size() ? cands[localIdx] : "";
	}

       private:
	static bool endsWithAny(const std::string& s, std::initializer_list<const char*> suffixes) {
		for (auto suf : suffixes) {
			size_t n = std::string(suf).size();
			if (s.size() >= n && s.compare(s.size() - n, n, suf) == 0) return true;
		}
		return false;
	}

	void buildUnionFind(const std::set<std::string>& usedTypes) {
		std::map<std::pair<int, int>, std::string> bycoord;
		for (auto& kv : db.tilegrid) bycoord[{kv.second.grid_x, kv.second.grid_y}] = kv.first;

		auto tileconnNeighbors = [&](const std::string& tile, const std::string& ty,
		                              const std::function<void(const std::string&, const std::string&, const std::string&)>& cb) {
			const Tile& t = db.tilegrid.at(tile);
			for (auto& conn : db.tileconn) {
				if (ty == conn.t0) {
					auto it = bycoord.find({t.grid_x + conn.dx, t.grid_y + conn.dy});
					if (it != bycoord.end() && db.tilegrid.at(it->second).type == conn.t1)
						for (auto& wp : conn.wire_pairs) cb(it->second, wp.first, wp.second);
				}
				if (ty == conn.t1) {
					auto it = bycoord.find({t.grid_x - conn.dx, t.grid_y - conn.dy});
					if (it != bycoord.end() && db.tilegrid.at(it->second).type == conn.t0)
						for (auto& wp : conn.wire_pairs) cb(it->second, wp.second, wp.first);
				}
			}
		};

		// region expansion: start from used tiles, follow tileconn outward
		// 10 hops to also include passive (no-FASM-feature) tiles on the path.
		std::map<std::string, std::string> region = used_tiles;
		std::vector<std::pair<std::string, std::string>> frontier(used_tiles.begin(), used_tiles.end());
		for (int hop = 0; hop < 10 && !frontier.empty(); hop++) {
			std::vector<std::pair<std::string, std::string>> next;
			for (auto& [tile, ty] : frontier) {
				tileconnNeighbors(tile, ty, [&](const std::string& nb, const std::string&, const std::string&) {
					if (!region.count(nb)) {
						std::string nty = db.tilegrid.at(nb).type;
						region[nb] = nty;
						next.emplace_back(nb, nty);
					}
				});
			}
			frontier = next;
		}

		for (auto& [tile, ty] : region) {
			tileconnNeighbors(tile, ty, [&](const std::string& nb, const std::string& wHere, const std::string& wThere) {
				if (region.count(nb)) dsu.unite(tw(tile, wHere), tw(nb, wThere));
			});
		}
		for (auto& [tile, plist] : real_pips)
			for (auto& [dst, src] : plist) dsu.unite(tw(tile, dst), tw(tile, src));

		// always-on pseudo-pips (permanent, non-configurable routing)
		for (auto& t : usedTypes) {
			std::string path = db.family_dir + "/ppips_" + toLower(t) + ".db";
			std::ifstream f(path);
			if (!f) continue;
			std::vector<std::pair<std::string, std::string>> always;
			std::string line;
			while (std::getline(f, line)) {
				std::istringstream iss(line);
				std::string feat, kind;
				iss >> feat >> kind;
				if (kind != "always") continue;
				auto seg = splitAll(feat, '.');
				if (seg.size() >= 3) always.emplace_back(seg[seg.size() - 2], seg[seg.size() - 1]);
			}
			for (auto& [tile, ty] : used_tiles) {
				if (ty != t) continue;
				for (auto& [dst, src] : always) dsu.unite(tw(tile, dst), tw(tile, src));
			}
		}

		// ILOGIC/OLOGIC D<->O bypass route-throughs (implicit default
		// state, no FASM feature bit -- see bit2gates.py's own comment).
		static const std::regex bypassRe(R"(^(?:[A-Z0-9]*_)?(I|O)LOGIC(\d*)_(D1?)$)");
		static const std::regex bypassOutRe(R"(^(?:[A-Z0-9]*_)?(I|O)LOGIC(\d*)_(O|OQ)$)");
		for (auto& [tile, ty] : used_tiles) {
			const TileType& tt = db.tileType(ty);
			std::map<std::pair<std::string, std::string>, std::string> dPins, oPins;
			for (auto& w : tt.wires) {
				std::smatch m;
				if (std::regex_match(w, m, bypassRe)) dPins[{m[1].str(), m[2].str()}] = w;
				if (std::regex_match(w, m, bypassOutRe)) oPins[{m[1].str(), m[2].str()}] = w;
			}
			for (auto& [key, dw] : dPins) {
				auto it = oPins.find(key);
				if (it != oPins.end()) dsu.unite(tw(tile, dw), tw(tile, it->second));
			}
		}

		// SLICE column OUTMUX: forwards O6 when NOT selecting the secondary
		// 5FF's Q -- the fix found via AIG-level structural-constant proof
		// (see scripts/bit2gates.py's matching comment for the full story).
		for (auto& kv : slice_feats) {
			const std::string& tile = kv.first.first;
			const std::string& suffix = kv.first.second;
			std::string site = siteFromSuffix(tile, suffix);
			if (site.empty()) continue;
			const std::string& ty = used_tiles[tile];
			int ordinal = siteOrdinal(tile, site);
			auto pins = sitePins(ty, ordinal);
			for (char col : {'A', 'B', 'C', 'D'}) {
				std::string key = std::string(1, col) + "OUTMUX.O6";
				if (!kv.second.count(key)) continue;
				auto o6it = pins.find(std::string(1, col));
				auto muxit = pins.find(std::string(1, col) + "MUX");
				if (o6it != pins.end() && muxit != pins.end())
					dsu.unite(tw(tile, o6it->second), tw(tile, muxit->second));
			}
		}
	}
};

std::string ffType(bool sync, int srval, std::string& srPin) {
	if (sync) {
		if (srval == 0) {
			srPin = "R";
			return "FDRE";
		}
		srPin = "S";
		return "FDSE";
	}
	if (srval == 0) {
		srPin = "CLR";
		return "FDCE";
	}
	srPin = "PRE";
	return "FDPE";
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

	std::vector<std::string> lines, warnings;
	std::set<std::string> allNames;
	auto uniqName = [&](std::string base) {
		std::string name = base;
		int i = 1;
		while (allNames.count(name)) name = base + "_" + std::to_string(i++);
		allNames.insert(name);
		return name;
	};

	// ---- LUTs: enumerate purely from fd.slice_feats (bitstream + fixed
	// topology), mirroring bit2gates.py exactly (including uncelled
	// route-throughs -- a real, configured INIT with no "intended" logical
	// cell is still real silicon and must be emitted). ----
	std::map<std::tuple<std::string, std::string, char>, std::string> lutCellName;  // (tile,site,col)->name
	for (auto& kv : fd.slice_feats) {
		const std::string& tile = kv.first.first;
		const std::string& suffix = kv.first.second;
		std::string site = fd.siteFromSuffix(tile, suffix);
		if (site.empty()) continue;
		for (char col : {'A', 'B', 'C', 'D'}) {
			if (kv.second.count(std::string(1, col) + "INIT"))
				lutCellName[{tile, site, col}] = uniqName("lut_" + vname(tile) + "_" + site + "_" + col);
		}
	}

	// ---- FFs: main + secondary(5FF), existence from ANY of that FF's own
	// feature names being present (ZINI/ZRST/FFMUX for main; ZINI/ZRST/
	// 5FFMUX/OUTMUX.%s5Q for 5FF) -- mirrors the LUT rule; verified this
	// recovers the same FF count as ground truth for the validated example.
	std::vector<std::tuple<std::string, std::string, char, bool>> ffSlots;  // tile,site,col,is5
	for (auto& kv : fd.slice_feats) {
		const std::string& tile = kv.first.first;
		const std::string& suffix = kv.first.second;
		std::string site = fd.siteFromSuffix(tile, suffix);
		if (site.empty()) continue;
		const FeatMap& feat = kv.second;
		for (char col : {'A', 'B', 'C', 'D'}) {
			std::string c(1, col);
			bool main = feat.count(c + "FF.ZINI") || feat.count(c + "FF.ZRST");
			if (!main)
				for (auto& f : feat)
					if (f.first.rfind(c + "FFMUX.", 0) == 0) { main = true; break; }
			if (main) ffSlots.emplace_back(tile, site, col, false);

			bool ff5 = feat.count(c + "5FF.ZINI") || feat.count(c + "5FF.ZRST") ||
			           feat.count(c + "OUTMUX." + c + "5Q");
			if (!ff5)
				for (auto& f : feat)
					if (f.first.rfind(c + "5FFMUX.", 0) == 0) { ff5 = true; break; }
			if (ff5) ffSlots.emplace_back(tile, site, col, true);
		}
	}

	std::map<std::tuple<std::string, std::string, char>, std::string> lutO5Net;
	for (auto& kv : lutCellName) {
		const auto& [tile, site, col] = kv.first;
		const std::string& ttype = fd.used_tiles[tile];
		int ordinal = fd.siteOrdinal(tile, site);
		std::string suffix = fd.fasmSiteSuffix(tile, site);
		auto fit = fd.slice_feats.find({tile, suffix});
		if (fit == fd.slice_feats.end()) continue;
		auto initIt = fit->second.find(std::string(1, col) + "INIT");
		if (initIt == fit->second.end()) {
			warnings.push_back(std::string(1, col) + "LUT.INIT missing for " + tile + "/" + site);
			continue;
		}
		if (initIt->second.width != 64)
			warnings.push_back("unexpected " + std::string(1, col) + "LUT.INIT width on " + tile + "/" + site);
		unsigned long long init64 = std::stoull(initIt->second.bits, nullptr, 2);
		auto pins = fd.sitePins(ttype, ordinal);
		std::string ins[6];
		for (int i = 1; i <= 6; i++) {
			auto pit = pins.find(std::string(1, col) + std::to_string(i));
			ins[i - 1] = pit != pins.end() ? fd.netOf(tile, pit->second) : "1'b0";
		}
		std::string o6net;
		auto o6pin = pins.find(std::string(1, col));
		if (o6pin != pins.end()) o6net = fd.netOf(tile, o6pin->second);
		std::string o5net = "w5_" + vname(tile) + "_" + site + "_" + col;
		lutO5Net[{tile, site, col}] = o5net;

		char buf[32];
		snprintf(buf, sizeof(buf), "64'h%016llx", init64);
		lines.push_back("  LUT6_2 #(.INIT(" + std::string(buf) + ")) \\" + kv.second + " (.O6(" + o6net +
		                 "), .O5(" + o5net + "), .I0(" + ins[0] + "), .I1(" + ins[1] + "), .I2(" + ins[2] +
		                 "), .I3(" + ins[3] + "), .I4(" + ins[4] + "), .I5(" + ins[5] + "));");
	}

	std::set<std::string> clkNetsUsed, srNetsUsed;
	for (auto& [tile, site, col, is5] : ffSlots) {
		std::string name = uniqName("ff_" + vname(tile) + "_" + site + "_" + col + (is5 ? "5" : ""));
		const std::string& ttype = fd.used_tiles[tile];
		int ordinal = fd.siteOrdinal(tile, site);
		std::string suffix = fd.fasmSiteSuffix(tile, site);
		auto fit = fd.slice_feats.find({tile, suffix});
		if (fit == fd.slice_feats.end()) {
			warnings.push_back(name + ": no slice_feats found for " + tile + "/" + suffix);
			continue;
		}
		const FeatMap& feat = fit->second;
		std::string c(1, col);
		auto pins = fd.sitePins(ttype, ordinal);
		auto pin = [&](const std::string& p) -> std::string {
			auto it = pins.find(p);
			return it != pins.end() ? fd.netOf(tile, it->second) : "";
		};

		bool sync = feat.count("FFSYNC") > 0;
		std::string dNet, qNet;
		int srval, init;
		if (is5) {
			srval = feat.count(c + "5FF.ZRST") ? 0 : 1;
			init = feat.count(c + "5FF.ZINI") ? 0 : 1;
			if (feat.count(c + "5FFMUX.IN_B")) dNet = pin(c + "X");
			else {
				auto it = lutO5Net.find({tile, site, col});
				if (it != lutO5Net.end()) dNet = it->second;
			}
			if (dNet.empty()) {
				warnings.push_back(name + ": 5FF with no D source resolved");
				continue;
			}
			if (feat.count(c + "OUTMUX." + c + "5Q")) qNet = pin(c + "MUX");
		} else {
			srval = feat.count(c + "FF.ZRST") ? 0 : 1;
			init = feat.count(c + "FF.ZINI") ? 0 : 1;
			std::set<std::string> ffmuxKeys;
			for (auto& f : feat)
				if (f.first.rfind(c + "FFMUX.", 0) == 0) ffmuxKeys.insert(f.first.substr(c.size() + 6));
			if (ffmuxKeys.count("XOR")) {
				warnings.push_back(name + ": FFMUX.XOR (carry) D-source not modelled, skipped");
				continue;
			}
			if (ffmuxKeys.count("O5")) {
				auto it = lutO5Net.find({tile, site, col});
				if (it != lutO5Net.end()) dNet = it->second;
			} else if (ffmuxKeys.count("AX") || ffmuxKeys.count("BX") || ffmuxKeys.count("CX") ||
			           ffmuxKeys.count("DX")) {
				dNet = pin(c + "X");
			} else {
				dNet = pin(c);
			}
			qNet = pin(c + "Q");
		}
		if (dNet.empty()) {
			warnings.push_back(name + ": could not resolve D net");
			continue;
		}

		std::string clkNet = pin("CLK");
		std::string ceNet = feat.count("CEUSEDMUX") ? pin("CE") : "1'b1";
		std::string srNet = feat.count("SRUSEDMUX") ? pin("SR") : "1'b0";
		clkNetsUsed.insert(clkNet);
		if (feat.count("SRUSEDMUX")) srNetsUsed.insert(srNet);
		std::string srPin;
		std::string prim = ffType(sync, srval, srPin);
		lines.push_back("  " + prim + " #(.INIT(1'b" + std::to_string(init) + ")) \\" + name + " (.C(" + clkNet +
		                 "), .CE(" + ceNet + "), .D(" + dNet + "), ." + srPin + "(" + srNet + "), .Q(" + qNet + "));");
	}

	// ---- top-level IO: OBUF/IBUF/IBUFDS/BUFG, resolved purely from FASM site
	// features (IBUF_HP_BANK_GLUE/OBUF_.../IBUFDS_BANK_GLUE, BUFGCTRL.*.IN_USE)
	// at sites located via the .xdc's PACKAGE_PIN constraints + prjxray's
	// package_pins.csv (pin -> tile/site) -- never from a placement/routed
	// dump. The .xdc is also the sole source of the top-level port list
	// itself. Direct C++ port of bit2gates.py's own IO section. Only runs
	// when both --xdc and --part were given; otherwise the module has no
	// ports, as before. ----
	std::vector<std::string> portDecls;
	std::string clkPortName, plainIbufOutNet;
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
				lines.push_back("  IBUFDS \\" + inst + " (.I(" + pname + "), .IB(" + ibPname + "), .O(" + oNet +
				                 "));");
				clkPortName = pname;
			} else if (kind == "ibuf") {
				auto pins = fd.sitePinsByType(ttype, styp);
				auto pit = pins.find("I");
				std::string oNet = pit != pins.end() ? fd.netOf(r.tile, pit->second) : "";
				lines.push_back("  IBUF \\" + inst + " (.I(" + pname + "), .O(" + oNet + "));");
				plainIbufOutNet = oNet;
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
			for (auto& n : clkNetsUsed) lines.push_back("  assign " + n + " = " + clkPortName + ";");
		if (!plainIbufOutNet.empty())
			for (auto& n : srNetsUsed)
				if (n != plainIbufOutNet) lines.push_back("  assign " + n + " = " + plainIbufOutNet + ";");
	}

	// ---- tie off genuinely-unrouted nets (no IO/clock reconstruction in
	// this pass -- see file header) ----
	std::set<std::string> allWires;
	for (auto& kv : fd.netname) allWires.insert(kv.second);
	for (auto& kv : lutO5Net) allWires.insert(kv.second);
	std::set<std::string> driven;
	static const std::regex drivenRe(R"(\.(?:O6|O5|O|Q)\(([^()]+)\))");
	static const std::regex assignRe(R"(^\s*assign\s+(\S+)\s*=)");
	for (auto& l : lines) {
		for (auto it = std::sregex_iterator(l.begin(), l.end(), drivenRe); it != std::sregex_iterator(); ++it)
			driven.insert((*it)[1].str());
		std::smatch m;
		if (std::regex_search(l, m, assignRe)) driven.insert(m[1].str());
	}
	for (auto& n : allWires)
		if (!driven.count(n)) lines.push_back("  assign " + n + " = 1'b0;");

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

	std::cerr << "fasm2netlist: " << lutCellName.size() << " LUT6_2, " << ffSlots.size() << " FF, wrote " << outPath
	          << "\n";
	for (auto& w : warnings) std::cerr << "warning: " << w << "\n";
	return 0;
}
