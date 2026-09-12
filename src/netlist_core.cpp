#include "netlist_core.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace f2n {

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

json::Value readJson(const std::string& path) { return json::parse(readFile(path)); }

std::string toLower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
	return s;
}

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

std::string hexLiteral(const std::string& bitsMsbFirst) {
	size_t n = bitsMsbFirst.size();
	std::string padded = bitsMsbFirst;
	if (n % 4) padded.insert(padded.begin(), 4 - (n % 4), '0');
	std::string hex;
	for (size_t i = 0; i < padded.size(); i += 4) {
		int v = 0;
		for (int b = 0; b < 4; b++) v = (v << 1) | (padded[i + b] == '1');
		hex.push_back("0123456789abcdef"[v]);
	}
	return std::to_string(n) + "'h" + hex;
}

// ---------------------------------------------------------------------
// Database
// ---------------------------------------------------------------------

Database::Database(const std::string& db_root, const std::string& family, const std::string& device) {
	family_dir = db_root + "/" + family;
	loadTilegrid(family_dir + "/" + device + "/tilegrid.json");
	// tileconn.json is per-family for some parts and per-device for others
	// (openXC7's database has virtex7's at the family level and artix7's
	// under the device), so look for the more specific one first rather than
	// assuming either layout.
	std::string perDevice = family_dir + "/" + device + "/tileconn.json";
	std::ifstream probe(perDevice);
	loadTileconn(probe.good() ? perDevice : family_dir + "/tileconn.json");
}

const TileType& Database::tileType(const std::string& type) {
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
			const json::Value& yc = sv.get("y_coord");
			if (!yc.isNull()) s.y_coord = yc.asDouble();
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

void Database::loadTilegrid(const std::string& path) {
	json::Value j = readJson(path);
	for (auto& kv : j.members()) {
		Tile t;
		t.type = kv.second.get("type").asString();
		t.grid_x = (int)kv.second.get("grid_x").asInt();
		t.grid_y = (int)kv.second.get("grid_y").asInt();
		for (auto& skv : kv.second.get("sites").members()) t.sites[skv.first] = skv.second.asString();
		tilegrid.emplace(kv.first, std::move(t));
	}
}

void Database::loadTileconn(const std::string& path) {
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

// ---------------------------------------------------------------------
// features
// ---------------------------------------------------------------------

std::string featBitString(const FeatMap& feat, const std::string& name, int width) {
	std::string bits(width, '0');  // MSB-first: bits[width-1-i] is bit i
	auto it = feat.find(name);
	if (it != feat.end() && it->second.hasValue) {
		const FeatureValue& v = it->second;
		for (int i = 0; i < (int)v.bits.size(); i++) {
			int bitIdx = v.hi - i;  // v.bits is MSB-first over [hi:lo]
			if (bitIdx >= 0 && bitIdx < width) bits[width - 1 - bitIdx] = v.bits[i];
		}
		return bits;
	}
	// otherwise: individual set-bit flags, "NAME[i]"
	for (int i = 0; i < width; i++)
		if (feat.count(name + "[" + std::to_string(i) + "]")) bits[width - 1 - i] = '1';
	return bits;
}

uint64_t featBits(const FeatMap& feat, const std::string& name, int width) {
	std::string bits = featBitString(feat, name, width);
	uint64_t v = 0;
	for (char c : bits) v = (v << 1) | (c == '1');
	return v;
}

bool featPresent(const FeatMap& feat, const std::string& name) {
	if (feat.count(name)) return true;
	auto it = feat.lower_bound(name + "[");
	return it != feat.end() && it->first.rfind(name + "[", 0) == 0;
}

// ---------------------------------------------------------------------
// DisjointSet
// ---------------------------------------------------------------------

std::string DisjointSet::find(const std::string& x) {
	auto it = parent_.find(x);
	if (it == parent_.end()) {
		parent_[x] = x;
		return x;
	}
	std::string root = x;
	while (parent_[root] != root) root = parent_[root];
	std::string cur = x;
	while (parent_[cur] != root) {
		std::string next = parent_[cur];
		parent_[cur] = root;
		cur = next;
	}
	return root;
}

void DisjointSet::unite(const std::string& a, const std::string& b) {
	std::string ra = find(a), rb = find(b);
	if (ra != rb) parent_[ra] = rb;
}

std::string tw(const std::string& tile, const std::string& wire) { return tile + "\x1f" + wire; }

// ---------------------------------------------------------------------
// FASM tokenizing -- see the scope note in main.cpp's header.
// ---------------------------------------------------------------------

namespace {

struct RawFasm {
	std::map<std::string, std::string> used_tiles;                                 // tile -> type
	std::map<std::string, std::vector<std::pair<std::string, std::string>>> pips;  // tile -> [(dst,src)]
	std::map<std::string, std::vector<std::string>> site_feats;                    // tile -> ["rest", ...]
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

// "NAME[hi:lo] = W'bBITS" -> (NAME, FeatureValue); returns false if `rest`
// isn't a value line at all.
bool parseValueFeature(const std::string& rest, std::string& name, FeatureValue& out) {
	static const std::regex valRe(R"(^(.*?)\[(\d+):(\d+)\]\s*=\s*(\d+)'b([01]+)\s*$)");
	std::smatch m;
	if (!std::regex_match(rest, m, valRe)) return false;
	name = m[1].str();
	out.hasValue = true;
	out.hi = std::stoi(m[2].str());
	out.lo = std::stoi(m[3].str());
	out.width = std::stoi(m[4].str());
	out.bits = m[5].str();
	// a value shorter than its declared width is left-padded, as Verilog does
	if ((int)out.bits.size() < out.width) out.bits.insert(out.bits.begin(), out.width - out.bits.size(), '0');
	return true;
}

void addFeature(FeatMap& into, const std::string& rest) {
	std::string name;
	FeatureValue v;
	if (parseValueFeature(rest, name, v)) into[name] = v;
	else into[rest] = FeatureValue{};
}

}  // namespace

// ---------------------------------------------------------------------
// FasmDesign
// ---------------------------------------------------------------------

FasmDesign::FasmDesign(const std::string& fasmPath, Database& database) : db(database) {
	RawFasm raw = parseFasm(fasmPath, db);
	used_tiles = raw.used_tiles;

	// ---- classify raw pip-shaped lines: real routing pip, or bel-internal
	// route-through (both endpoints are real wires of this tile type but the
	// pip isn't in the general routing table), or -- the case an earlier
	// version of this port silently dropped -- neither, in which case it was
	// never really a pip at all: it's a plain two-level site feature (e.g.
	// "SLICEM_X0.FFSYNC") that only LOOKED like a pip to the tokenizer's
	// shape-only regex. Mirrors bit2gates.py's own reclassification exactly:
	// push it back as a "dst.src" site-feature line rather than discarding
	// it -- losing this fallback silently dropped every site-wide boolean
	// feature (FFSYNC, CEUSEDMUX, SRUSEDMUX, NOCLKINV, ...), which don't
	// have their own per-column INIT to independently confirm they parsed.
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

	// ---- classify each (now-complete) site-feature line. The leading
	// component names the site within the tile, in FASM's own local
	// vocabulary; which map it lands in follows from that name's shape, so
	// adding a site kind is one more branch here and one more emitter. ----
	static const std::regex sliceRe(R"(^(SLICE[A-Z]*_X\d+)\.(.+)$)");
	static const std::regex iobRe(R"(^(IOB_Y\d+)\.(.+)$)");
	static const std::regex bramRe(R"(^(RAMB18_Y\d+|RAMB36)\.(.+)$)");
	static const std::regex dspRe(R"(^DSP48\.(DSP_\d+)\.(.+)$)");
	static const std::regex bufgRe(R"(^BUFGCTRL\.(BUFGCTRL_X\d+Y\d+)\.(.+)$)");
	for (auto& kv : raw.site_feats) {
		const std::string& tile = kv.first;
		for (auto& fe : kv.second) {
			std::smatch m;
			if (std::regex_match(fe, m, sliceRe)) addFeature(slice_feats[{tile, m[1].str()}], m[2].str());
			else if (std::regex_match(fe, m, iobRe)) iob_feats[{tile, m[1].str()}].insert(m[2].str());
			else if (std::regex_match(fe, m, bramRe)) addFeature(bram_feats[{tile, m[1].str()}], m[2].str());
			else if (std::regex_match(fe, m, dspRe)) addFeature(dsp_feats[{tile, m[1].str()}], m[2].str());
			else if (std::regex_match(fe, m, bufgRe)) bufg_feats[{tile, m[1].str()}].insert(m[2].str());
			else if (fe.find('.') == std::string::npos) addFeature(tile_feats[tile], fe);
		}
	}

	buildUnionFind(usedTypes);
}

std::string constantOfWire(const std::string& wire) {
	static const char* onesuf[] = {"VCC_WIRE", "DSP_VCC_L", "DSP_VCC_R"};
	static const char* zerosuf[] = {"GND_WIRE", "DSP_GND_L", "DSP_GND_R"};
	for (auto s : onesuf)
		if (endsWith(wire, s)) return "1'b1";
	for (auto s : zerosuf)
		if (endsWith(wire, s)) return "1'b0";
	return "";
}

std::string FasmDesign::netOf(const std::string& tile, const std::string& wire) {
	std::string lit = constantOfWire(wire);
	if (!lit.empty()) return lit;
	std::string root = dsu.find(tw(tile, wire));
	// ...and a wire ROUTED to one of those is just as constant. Testing only
	// the name handed in missed the ordinary case -- a block RAM address pin
	// grounded through an interconnect tile resolves to that tile's
	// GND_WIRE, not to one of its own -- which left real constants looking
	// like ordinary nets, and two separately-grounded pins looking like two
	// different nets rather than one.
	auto cit = constNet.find(root);
	if (cit != constNet.end()) return cit->second;
	auto it = netname.find(root);
	if (it != netname.end()) return it->second;
	size_t sep = root.find('\x1f');
	std::string name = "n_" + vname(root.substr(0, sep)) + "__" + vname(root.substr(sep + 1));
	netname[root] = name;
	return name;
}

std::string FasmDesign::fasmSiteSuffix(const std::string& tile, const std::string& site) {
	const std::string& realType = db.tilegrid.at(tile).sites.at(site);
	int ord = siteOrdinal(tile, site);
	return realType + "_X" + std::to_string(ord);
}

std::string FasmDesign::siteFromSuffix(const std::string& tile, const std::string& suffix) {
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

int FasmDesign::siteOrdinal(const std::string& tile, const std::string& site) {
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

std::map<std::string, std::string> FasmDesign::sitePins(const std::string& tileType, int ordinal) {
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

std::map<std::string, std::string> FasmDesign::sitePinsByType(const std::string& tileType,
                                                              const std::string& siteType) {
	const TileType& tt = db.tileType(tileType);
	for (auto& s : tt.sites)
		if (s.type == siteType) return s.pins;
	return {};
}

std::string FasmDesign::fasmIobSuffix(const std::string& tile, const std::string& site) {
	const auto& sites = db.tilegrid.at(tile).sites;
	if (sites.size() == 1) return "IOB_Y1";
	auto it = sites.find(site);
	std::string styp = it != sites.end() ? it->second : "";
	if (!styp.empty() && styp.back() == 'M') return "IOB_Y0";
	if (!styp.empty() && styp.back() == 'S') return "IOB_Y1";
	return "";
}

std::string FasmDesign::bufgctrlRealSite(const std::string& tile, const std::string& fasmLocalSite) {
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

void FasmDesign::buildUnionFind(const std::set<std::string>& usedTypes) {
	std::map<std::pair<int, int>, std::string> bycoord;
	for (auto& kv : db.tilegrid) bycoord[{kv.second.grid_x, kv.second.grid_y}] = kv.first;

	auto tileconnNeighbors =
	    [&](const std::string& tile, const std::string& ty,
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
		tileconnNeighbors(tile, ty,
		                  [&](const std::string& nb, const std::string& wHere, const std::string& wThere) {
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

	// ILOGIC/OLOGIC D<->O bypass route-throughs (implicit default state, no
	// FASM feature bit -- see bit2gates.py's own comment).
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

	// Mark every net that reaches a power or ground pseudo-source, so that
	// netOf() can answer with the literal rather than a wire name. Done once
	// here, over the finished union-find, because which end of a class the
	// root happens to be is arbitrary -- testing the root alone would catch
	// only the classes that happened to root on the constant.
	{
		std::vector<std::string> keys;
		keys.reserve(dsu.nodes().size());
		for (auto& kv : dsu.nodes()) keys.push_back(kv.first);
		for (auto& k : keys) {
			size_t sep = k.find('\x1f');
			if (sep == std::string::npos) continue;
			std::string lit = constantOfWire(k.substr(sep + 1));
			if (!lit.empty()) constNet.emplace(dsu.find(k), lit);
		}
	}

	// SLICE column OUTMUX: forwards O6 when NOT selecting the secondary
	// 5FF's Q -- the fix found via AIG-level structural-constant proof (see
	// scripts/bit2gates.py's matching comment for the full story). The other
	// OUTMUX selects (O5/XOR/CY/MC31/F7/F8) name an output that is internal
	// to the site and so has no wire of its own to be joined to; those are
	// emitted as an explicit assign by the slice emitter instead.
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

// ---------------------------------------------------------------------
// Netlist
// ---------------------------------------------------------------------

std::string Netlist::uniqName(std::string base) {
	std::string name = base;
	int i = 1;
	while (allNames.count(name)) name = base + "_" + std::to_string(i++);
	allNames.insert(name);
	return name;
}

void Netlist::markDriven(const std::string& net) {
	if (net.empty() || net.rfind("1'b", 0) == 0) return;
	driven.insert(net);
}

void Netlist::instance(const std::string& prim, const std::string& params, const std::string& name,
                       const std::string& conns) {
	std::string l = "  " + prim;
	if (!params.empty()) l += " #(" + params + ")";
	l += " \\" + name + " (" + conns + ");";
	lines.push_back(l);
	count(prim);
}

void Netlist::assign(const std::string& lhs, const std::string& rhs) {
	lines.push_back("  assign " + lhs + " = " + rhs + ";");
	markDriven(lhs);
}

}  // namespace f2n
