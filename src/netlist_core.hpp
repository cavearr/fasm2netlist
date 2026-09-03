// Shared extraction core for fasm2netlist: the fixed prjxray technology
// database, the FASM tokenizer, the union-find net reconstruction, and the
// little netlist accumulator every cell family emits into.
//
// This is the part that knows nothing about what a cell *is* -- it only knows
// what the silicon's fixed tables say is connected to what, and which FASM
// features are set where. The per-family decoders live in cells_*.cpp and see
// the fabric only through FasmDesign.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "json.hpp"

namespace f2n {

// ---------------------------------------------------------------------
// small utilities
// ---------------------------------------------------------------------

std::string readFile(const std::string& path);
json::Value readJson(const std::string& path);
std::string toLower(std::string s);

// legal Verilog identifier from a tile/wire name -- matches bit2verilog.py /
// bit2gates.py's vname() exactly.
std::string vname(const std::string& s);

std::vector<std::string> splitAll(const std::string& s, char sep);
bool endsWith(const std::string& s, const std::string& suffix);

// ---------------------------------------------------------------------
// fixed prjxray technology database (tilegrid / tileconn / tile_type / ppips)
// ---------------------------------------------------------------------

struct Site {
	std::string type;
	double x_coord = 0;
	double y_coord = 0;
	std::map<std::string, std::string> pins;  // pin name -> wire name
};

struct TileType {
	std::set<std::string> wires;
	std::set<std::pair<std::string, std::string>> pips;  // (dst_wire, src_wire)
	std::vector<Site> sites;                             // in file order
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

	Database(const std::string& db_root, const std::string& family, const std::string& device);

	const TileType& tileType(const std::string& type);

       private:
	void loadTilegrid(const std::string& path);
	void loadTileconn(const std::string& path);
};

// ---------------------------------------------------------------------
// FASM features
// ---------------------------------------------------------------------

// A FASM feature is either a bare flag ("FFSYNC", "CARRY4.ACY0") or a value
// line ("ALUT.INIT[63:0] = 64'b...."). prjxray trims leading zero bits off a
// value's index range, so [hi:lo] is NOT always [width-1:0] -- e.g. a DSP
// MASK whose top two bits are zero is emitted as "MASK[45:0] = 46'b...".
// Getting that wrong silently shifts every wide constant, so the range is
// kept and applied rather than assumed.
struct FeatureValue {
	bool hasValue = false;
	int width = 0;
	int hi = 0, lo = 0;
	std::string bits;  // MSB-first, exactly `width` characters
};
using FeatMap = std::map<std::string, FeatureValue>;

// Assemble `width` bits (MSB-first, zero-filled) for a feature that FASM may
// present either as one value line ("MASK[45:0] = 46'b...") or as a set of
// individual set-bit flags ("ZIS_OPMODE_INVERTED[0]", "...[1]", ...). An
// absent bit reads as 0.
std::string featBitString(const FeatMap& feat, const std::string& name, int width);
// ...same, for a feature narrow enough to fit an integer.
uint64_t featBits(const FeatMap& feat, const std::string& name, int width);
// A feature is "present" if it appears as a flag, an indexed flag, or a value.
bool featPresent(const FeatMap& feat, const std::string& name);

// ---------------------------------------------------------------------
// union-find over (tile, wire)
// ---------------------------------------------------------------------

class DisjointSet {
       public:
	std::string find(const std::string& x);
	void unite(const std::string& a, const std::string& b);
	// every node the set has been told about, for a pass that has to look
	// at all of them rather than at one it already holds a key for
	const std::unordered_map<std::string, std::string>& nodes() const { return parent_; }

       private:
	std::unordered_map<std::string, std::string> parent_;
};

// "1'b1" / "1'b0" if this wire is one of the fabric's power or ground
// pseudo-sources, "" otherwise.
std::string constantOfWire(const std::string& wire);

// key for a (tile,wire) pair used throughout as a DSU / map key
std::string tw(const std::string& tile, const std::string& wire);

// ---------------------------------------------------------------------
// FasmDesign: the union-find net reconstruction + feature classification --
// the C++ mirror of bit2gates.py's FasmDesign class.
// ---------------------------------------------------------------------

class FasmDesign {
       public:
	Database& db;
	std::map<std::string, std::string> used_tiles;
	std::map<std::string, std::vector<std::pair<std::string, std::string>>> real_pips;

	// (tile, site-suffix) -> features, one map per kind of site the tool
	// models. The suffix is FASM's own local site name, not the device-wide
	// one: "SLICEM_X0", "IOB_Y1", "RAMB18_Y0"/"RAMB36", "DSP_0"/"DSP_1".
	std::map<std::pair<std::string, std::string>, FeatMap> slice_feats;
	std::map<std::pair<std::string, std::string>, std::set<std::string>> iob_feats;
	std::map<std::pair<std::string, std::string>, std::set<std::string>> bufg_feats;
	std::map<std::pair<std::string, std::string>, FeatMap> bram_feats;
	std::map<std::pair<std::string, std::string>, FeatMap> dsp_feats;
	// tile-wide features with no site component at all (BRAM's EN_SYN,
	// CASCOUT_*_ACTIVE, ZALMOST_*_OFFSET).
	std::map<std::string, FeatMap> tile_feats;

	DisjointSet dsu;
	std::unordered_map<std::string, std::string> netname;   // dsu-root -> emitted wire name
	std::unordered_map<std::string, std::string> constNet;  // dsu-root -> "1'b0"/"1'b1"

	FasmDesign(const std::string& fasmPath, Database& database);

	std::string netOf(const std::string& tile, const std::string& wire);
	std::string fasmSiteSuffix(const std::string& tile, const std::string& site);
	// inverse of fasmSiteSuffix(): "SLICEM_X0" -> real site name
	std::string siteFromSuffix(const std::string& tile, const std::string& suffix);
	int siteOrdinal(const std::string& tile, const std::string& site);
	std::map<std::string, std::string> sitePins(const std::string& tileType, int ordinal);

	// IOB18S/IOB18M (and similarly-paired site types) both report x_coord 0
	// in the tile_type JSON -- sitePins()'s x_coord ranking can't tell them
	// apart, and picking the wrong one silently swaps which physical wire an
	// OBUF/IBUF resolves to. Use this instead whenever the real site type is
	// already known (from the tilegrid, cross-referenced via a package pin).
	std::map<std::string, std::string> sitePinsByType(const std::string& tileType,
	                                                  const std::string& siteType);

	// real site name -> FASM's "IOB_Y0"/"IOB_Y1" suffix.
	std::string fasmIobSuffix(const std::string& tile, const std::string& site);
	// FASM addresses a BUFGCTRL by a tile-LOCAL index; resolve it.
	std::string bufgctrlRealSite(const std::string& tile, const std::string& fasmLocalSite);

       private:
	void buildUnionFind(const std::set<std::string>& usedTypes);
};

// ---------------------------------------------------------------------
// the netlist being built
// ---------------------------------------------------------------------

// Every emitter records the nets it drives explicitly rather than leaving the
// writer to recognise driver pins by name: with CARRY4/SRL/RAM/DSP/BRAM in
// the mix the set of output pin names is far too large (and too easy to
// confuse with input pins that merely end in the same letters) for a
// pattern-match over the emitted text to stay honest.
struct Netlist {
	std::vector<std::string> lines;
	std::vector<std::string> warnings;
	std::set<std::string> allNames;      // instance names handed out so far
	std::set<std::string> internalNets;  // nets invented by an emitter (not DB wires)
	std::set<std::string> driven;        // nets some cell or assign drives
	std::set<std::string> clkNetsUsed;   // SLICE clock nets, for the clock tie-off
	std::set<std::string> srNetsUsed;    // SLICE set/reset nets, likewise
	std::map<std::string, int> cellCounts;

	std::string uniqName(std::string base);
	void markDriven(const std::string& net);
	void emit(const std::string& line) { lines.push_back(line); }
	void warn(const std::string& w) { warnings.push_back(w); }
	void count(const std::string& cellType) { cellCounts[cellType]++; }
	// emit "  <prim> #(params) \<name> (conns);", counting it
	void instance(const std::string& prim, const std::string& params, const std::string& name,
	              const std::string& conns);
	// "assign a = b;" -- also marks a driven
	void assign(const std::string& lhs, const std::string& rhs);
};

// hex literal of a bit string (MSB-first, any length) as "<n>'h...."
std::string hexLiteral(const std::string& bitsMsbFirst);

}  // namespace f2n
