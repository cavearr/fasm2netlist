// Register correspondence between a gold synthesis and the physical fabric.
//
// Three files say between them what a bitstream's flip-flops are called in the
// designer's vocabulary: the placement gives each gold cell its (tile, site,
// bel); the tile-type database gives the wire that site pin lands on; the gold
// netlist's netnames give the source signal the cell drives.  Composing them
// labels the extraction without changing a single connection.
//
// Both the equivalence checker (which needs the correspondence to state what
// it is proving) and the emitter (which uses it to write a readable netlist)
// want the same answer, so it is computed once, here.
#pragma once
#include <map>
#include <string>

namespace lvs {

struct RegMap {
    // raw "TILE/WIRE" endpoint -> the source signal name, e.g. "led_int[2]"
    std::map<std::string, std::string> net;
    std::string module;         // the gold module the labels came from
    int mapped = 0, skipped = 0;
};

// Throws std::runtime_error if a file cannot be read or has no top module.
RegMap build_regmap(const std::string &placement_path, const std::string &gold_json_path,
                    const std::string &db, const std::string &device);

}  // namespace lvs
