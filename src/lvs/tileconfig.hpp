// Stage 1 of the tile-model converter: FASM features -> a tile's configuration,
// decoded into the same vocabulary the tile model takes as input.
//
// The point of this layer is that it makes NO inference about what a slice
// "means".  It reports which mux each select is set to and nothing more; what
// the resulting circuit looks like is the tile model's business.  Anything it
// does not recognise goes in `unhandled` rather than falling through to a
// default -- silently defaulting a mux select is how a wrong netlist gets
// produced that still looks plausible.
//
// The dump is canonical: fixed ordering, one fact per line, no addresses or
// timestamps, so two runs -- or two versions of the model -- can be diffed.
#ifndef LVS_TILECONFIG_HPP
#define LVS_TILECONFIG_HPP

#include <cstdint>
#include <map>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <vector>

namespace lvs {

// The main flip-flop's D source, as `xcol`'s FFSRC names it.
enum class FFSrc { O6, O5, BypassX, Xor, MC31, Wide, Carry };
// The 5FF's D source: IN_A is the LUT's O5, IN_B the column's bypass pin.
enum class FF5Src { O5, BypassX };
// What the column's xMUX output carries.
enum class OutMux { None, O6, O5, Xor, Q5, Carry, F7, F8, MC31 };

const char *to_string(FFSrc s);
const char *to_string(FF5Src s);
const char *to_string(OutMux s);

struct ColumnConfig
{
    char col = 'A';
    std::optional<uint64_t> init;   // xLUT.INIT[63:0]
    bool lut_used = false;

    bool ff_used = false;
    FFSrc ff_src = FFSrc::O6;       // only meaningful when ff_used
    bool ff_src_explicit = false;   // false = no FFMUX feature, i.e. the O6 default
    int ff_init = 1, ff_srval = 1;  // ZINI/ZRST are inverted senses

    bool ff5_used = false;
    FF5Src ff5_src = FF5Src::O5;
    bool ff5_src_explicit = false;
    int ff5_init = 1, ff5_srval = 1;

    OutMux outmux = OutMux::None;
};

struct SliceConfig
{
    std::string tile, tile_type, site;   // e.g. CLBLM_R_X31Y135, CLBLM_R, SLICEM_X0
    bool ffsync = false, clkinv = false, srusedmux = false, ceusedmux = false;
    std::map<char, ColumnConfig> columns;
    std::vector<std::string> unhandled;  // features this decoder does not model
};

struct DesignConfig
{
    std::map<std::string, SliceConfig> slices;      // key: tile + "/" + site
    std::map<std::string, std::vector<std::string>> other_tiles; // tile -> features
    std::vector<std::string> unhandled;

    // Canonical, sorted, one fact per line.
    void dump(std::ostream &os) const;
    // Only the parts no model covers yet, for measuring the gap.
    void dump_gaps(std::ostream &os) const;
};

// Reads a FASM file.  Routing features (tile-level PIPs) are kept verbatim
// against their tile; site features are decoded.
DesignConfig read_fasm(const std::string &path);

} // namespace lvs

#endif
