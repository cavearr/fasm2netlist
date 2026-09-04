#include "lvs/regmap.hpp"

#include "json.hpp"

#include "bram_ports.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <set>
#include <vector>

namespace lvs {

namespace {

std::string slurp(const std::string &p)
{
    std::ifstream f(p);
    if (!f)
        throw std::runtime_error("cannot open " + p);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// A block RAM's data outputs, which are the pins a comparison cuts at.  The
// widths come from the same table the extractor and the tile model use, so a
// port that is 32 bits wide is 32 bits wide in all three.  Everything else a
// block RAM drives -- ECC, cascade, FIFO status -- is not data and is not cut.
std::vector<std::pair<std::string, int>> data_outs(bool is36)
{
    std::vector<std::pair<std::string, int>> v;
    if (is36) {
        for (const auto &p : bram::kRamb36)
            if (p.out && std::string(p.port).rfind("DO", 0) == 0) v.push_back({p.port, p.width});
    } else {
        for (const auto &p : bram::kRamb18)
            if (p.out && std::string(p.name).rfind("DO", 0) == 0) v.push_back({p.name, p.width});
    }
    return v;
}

}  // namespace

RegMap build_regmap(const std::string &placement_path, const std::string &gold_json_path,
                    const std::string &db, const std::string &device)
{
    RegMap out;
    json::Value place = json::parse(slurp(placement_path));
    json::Value goldj = json::parse(slurp(gold_json_path));
    json::Value grid = json::parse(slurp(db + "/" + device + "/tilegrid.json"));

    // The gold module: the one flagged top, else the sole non-blackbox.
    const json::Value *mod = nullptr;
    for (const auto &kv : goldj.get("modules").members())
        if (!kv.second.get("attributes").get("top").isNull()) {
            mod = &kv.second;
            out.module = kv.first;
        }
    if (!mod)
        for (const auto &kv : goldj.get("modules").members())
            if (kv.second.get("attributes").get("blackbox").isNull()) {
                mod = &kv.second;
                out.module = kv.first;
            }
    if (!mod)
        throw std::runtime_error("no top module in " + gold_json_path);

    // Net id -> the source signal's name for it.  Hidden names are yosys's own
    // invention ($abc$...) and tell the reader nothing, so they are skipped;
    // the first real name to claim a bit wins.
    std::map<int64_t, std::string> label;
    std::map<int64_t, std::vector<std::string>> alt_label;
    for (const auto &nn : mod->get("netnames").members()) {
        const json::Value &hide = nn.second.get("hide_name");
        bool hidden = !hide.isNull() && hide.asInt() != 0;
        const auto &bits = nn.second.get("bits").items();
        // A vector need not start at zero.  `wire [4:2] o_cnt` is bits 0..2 of
        // the JSON's list and o_cnt[2]..o_cnt[4] of every name anyone writes,
        // and yosys records the difference as "offset" -- which, ignored, does
        // not lose a name so much as invent a wrong one: o_cnt[0] labels the
        // register the design calls o_cnt[2].  `upto` says the declaration ran
        // the other way, [2:4], so the list is most significant first.
        // Only three netnames in the LiteX SoC carry a non-zero offset, and
        // one of them is the CPU's state counter, which most of the design
        // reads; a single mislabelled register is not one bad cone but every
        // cone downstream of it.
        int64_t offset = 0;
        {
            const json::Value &o = nn.second.get("offset");
            if (!o.isNull()) offset = o.asInt();
        }
        const json::Value &up = nn.second.get("upto");
        bool upto = !up.isNull() && up.asInt() != 0;
        for (size_t i = 0; i < bits.size(); i++) {
            if (bits[i].type != json::Type::Int)
                continue;
            int64_t b = bits[i].asInt();
            int64_t idx = offset + (upto ? int64_t(bits.size()) - 1 - int64_t(i) : int64_t(i));
            std::string nm = bits.size() == 1 && offset == 0
                                 ? nn.first
                                 : nn.first + "[" + std::to_string(idx) + "]";
            // Every name, hidden or not, is a CANDIDATE: the caller has to
            // find whichever one its own netlist emitted, and write_verilog
            // does not always choose the same one as this does.  Only the
            // preferred label skips hidden names, and only because a report
            // reads better with "wdata0_r[0]" in it than "_0565_".
            alt_label[b].push_back(nm);
            // A one-bit VECTOR is written both ways: the JSON netname is bare
            // ("wdata0_r"), and write_verilog emits it indexed
            // ("wdata0_r[0]") because it was declared [0:0].  Neither spelling
            // is more correct, and the caller cannot know which its netlist
            // used, so offer both.
            if (bits.size() == 1)
                alt_label[b].push_back(nn.first + "[" + std::to_string(idx) + "]");
            if (bits.size() == 1 && offset == 0)
                alt_label[b].push_back(nn.first);
            if (!hidden && !label.count(b))
                label[b] = nm;
        }
    }
    // A bit no real name claimed still needs one, or its register goes
    // unmatched -- and an unmatched register is not one missing cone, it makes
    // every cone downstream of it incomparable too.
    for (const auto &[b, names] : alt_label)
        if (!label.count(b) && !names.empty())
            label[b] = names.front();

    // site pins, per tile type, in the order the tile lists its sites
    std::map<std::string, std::vector<std::map<std::string, std::string>>> tt_pins;
    auto pins_for = [&](const std::string &tile, const std::string &site) {
        std::map<std::string, std::string> none;
        const json::Value &tv = grid.get(tile);
        if (tv.isNull())
            return none;
        std::string type = tv.get("type").asString();
        if (!tt_pins.count(type)) {
            std::vector<std::map<std::string, std::string>> per;
            try {
                json::Value tt = json::parse(slurp(db + "/tile_type_" + type + ".json"));
                for (const auto &sv : tt.get("sites").items()) {
                    std::map<std::string, std::string> m;
                    for (const auto &pk : sv.get("site_pins").members())
                        m[pk.first] = pk.second.get("wire").asString();
                    per.push_back(m);
                }
            } catch (const std::exception &) {
                // a tile type we have no model for contributes no labels
            }
            tt_pins[type] = per;
        }
        // The placement names a site absolutely (SLICE_X0Y100); the tile type
        // describes its sites positionally.  Ranking the tile's slices by X
        // recovers the ordinal the tile type is indexed by.
        std::vector<std::pair<int, std::string>> slices;
        for (const auto &sv : tv.get("sites").members()) {
            if (sv.first.rfind("SLICE_X", 0) != 0)
                continue;
            slices.push_back({atoi(sv.first.c_str() + 7), sv.first});
        }
        std::sort(slices.begin(), slices.end());
        for (size_t i = 0; i < slices.size(); i++)
            if (slices[i].second == site && i < tt_pins[type].size())
                return tt_pins[type][i];
        return none;
    };

    // Memories.  A placement cell called "<ram>/DPR<n>" at a site's <col>6LUT
    // is port n of that synthesis RAM held in that fabric column.  The
    // netlists name the site differently -- the placement absolutely
    // (SLICE_X42Y136), the fabric by the local suffix FASM uses (SLICEM_X0) --
    // so the ordinal has to be recovered here, where the tile grid is already
    // open.
    auto sanitise = [](const std::string &in) {
        std::string r;
        for (char c : in) r.push_back(isalnum((unsigned char)c) ? c : '_');
        return r;
    };
    {
        static const std::regex dpr(R"(^(.*)/DPR(\d)(?:_(\d))?$)");
        for (const auto &pv : place.members()) {
            if (pv.second.get("type").asString() != "SLICE_LUTX") continue;
            std::smatch m;
            const std::string cell = pv.first;
            if (!std::regex_match(cell, m, dpr)) continue;
            std::string bel = pv.second.get("bel").asString();
            if (bel.empty() || bel[0] < 'A' || bel[0] > 'D') continue;
            std::string tile = pv.second.get("tile").asString();
            std::string site = pv.second.get("site").asString();
            const json::Value &tv = grid.get(tile);
            if (tv.isNull()) continue;
            std::vector<std::pair<int, std::string>> sl;
            for (const auto &sv : tv.get("sites").members())
                if (sv.first.rfind("SLICE_X", 0) == 0)
                    sl.push_back({atoi(sv.first.c_str() + 7), sv.first});
            std::sort(sl.begin(), sl.end());
            int ord = -1;
            for (size_t i = 0; i < sl.size(); i++)
                if (sl[i].second == site) ord = int(i);
            if (ord < 0) continue;
            std::string stype = tv.get("sites").get(site).asString();
            std::string gate = sanitise(tile + "_" + stype + "_X" + std::to_string(ord) + "_" +
                                        std::string(1, bel[0]));
            char port = char('A' + (m[2].str()[0] - '0'));
            int nbits = m[3].matched ? 2 : 1;
            for (int d = 0; d < nbits; d++)
                out.mem[gate + ":DO[" + std::to_string(d) + "]"] =
                    m[1].str() + ":DO" + port + "[" + std::to_string(d) + "]";
        }
    }

    // Block RAM.  Nothing here has to understand what the memory does: the
    // placement says which synthesis cell sits in which site, the tile model
    // names its instance after that site, and pairing the two is enough for
    // the checker to cut both at the same place and then prove the boundary.
    // A RAMB18 site is the tile's lower or upper 18Kb half, which FASM -- and
    // so the tile model -- calls RAMB18_Y0 and RAMB18_Y1; the placement names
    // it absolutely (RAMB18_X2Y58), so the half has to be recovered here,
    // where the tile grid is already open.
    {
        for (const auto &pv : place.members()) {
            std::string bel = pv.second.get("bel").asString();
            bool is36 = bel == "RAMB36E1";
            if (bel != "RAMB18E1" && !is36) continue;
            std::string tile = pv.second.get("tile").asString();
            std::string site = pv.second.get("site").asString();
            const json::Value &tv = grid.get(tile);
            if (tv.isNull()) continue;
            std::string fasm_site = "RAMB36_Y0";
            if (!is36) {
                std::vector<std::pair<int, std::string>> halves;
                for (const auto &sv : tv.get("sites").members()) {
                    if (sv.first.rfind("RAMB18_", 0) != 0) continue;
                    auto y = sv.first.rfind('Y');
                    if (y == std::string::npos) continue;
                    halves.push_back({atoi(sv.first.c_str() + y + 1), sv.first});
                }
                std::sort(halves.begin(), halves.end());
                int ord = -1;
                for (size_t i = 0; i < halves.size(); i++)
                    if (halves[i].second == site) ord = int(i);
                if (ord < 0) continue;
                fasm_site = "RAMB18_Y" + std::to_string(ord);
            }
            std::string gate = sanitise(tile + "_" + fasm_site);
            for (const auto &dp : data_outs(is36))
                for (int b = 0; b < dp.second; b++) {
                    std::string suffix =
                        std::string(":") + dp.first + "[" + std::to_string(b) + "]";
                    out.mem[gate + suffix] = pv.first + suffix;
                }
        }
    }

    // The clock manager.  Only its LOCKED pin is paired, and only because it
    // is the one Boolean thing about an MMCM; see the note in src/lvs/cone.cpp
    // for what that does and does not establish.
    for (const auto &pv : place.members()) {
        std::string bel = pv.second.get("bel").asString();
        if (bel != "MMCME2_ADV" && bel != "PLLE2_ADV") continue;
        std::string gate = sanitise(pv.second.get("tile").asString() + "_MMCME2_ADV");
        out.mem[gate + ":LOCKED[0]"] = pv.first + ":LOCKED[0]";
    }

    // The hard-block census.  Every block the placement put somewhere, with
    // the name the tile model would give it if it models that kind at all.
    // Nothing here compares behaviour -- a hard block has no cones to compare
    // -- but a block the synthesis asked for and the bitstream does not
    // configure is a real fault that register matching cannot see.
    {
        static const std::set<std::string> kHardBels = {
            "RAMB18E1", "RAMB36E1", "DSP48E1",  "MMCME2_ADV",    "PLLE2_ADV",
            "BUFGCTRL", "BUFR",     "BUFIO",    "IBUFDS_GTE2",   "GTXE2_CHANNEL",
            "GTXE2_COMMON", "GTPE2_CHANNEL", "GTPE2_COMMON", "IDELAYCTRL",
        };
        // Enumerated from the SYNTHESIS, not from the placement.  The
        // placement is written at the end of place-and-route, so a cell that
        // was dropped along the way is simply not in it -- which is precisely
        // the case worth catching, and a census built from the placement
        // cannot see it by construction.
        for (const auto &cv : mod->get("cells").members()) {
            std::string bel = cv.second.get("type").asString();
            if (!kHardBels.count(bel)) continue;
            RegMap::HardBlock hb;
            hb.type = bel;
            const json::Value &pv2 = place.get(cv.first);
            if (pv2.isNull()) {
                // In the synthesis, nowhere in the placement: it did not
                // survive packing.
                hb.site = "";
                out.hard[cv.first] = hb;
                continue;
            }
            hb.site = pv2.get("site").asString();
            std::string tile = pv2.get("tile").asString();
            // Only the kinds the tile model actually emits get a gate name; the
            // rest are reported as unmodelled, which is the honest answer.
            if (bel == "RAMB18E1" || bel == "RAMB36E1") {
                const json::Value &tv = grid.get(tile);
                std::string fasm_site = bel == "RAMB36E1" ? "RAMB36_Y0" : "";
                if (fasm_site.empty() && !tv.isNull()) {
                    std::vector<std::pair<int, std::string>> halves;
                    for (const auto &sv : tv.get("sites").members()) {
                        if (sv.first.rfind("RAMB18_", 0) != 0) continue;
                        auto y = sv.first.rfind('Y');
                        if (y != std::string::npos)
                            halves.push_back({atoi(sv.first.c_str() + y + 1), sv.first});
                    }
                    std::sort(halves.begin(), halves.end());
                    for (size_t i = 0; i < halves.size(); i++)
                        if (halves[i].second == hb.site) fasm_site = "RAMB18_Y" + std::to_string(i);
                }
                if (!fasm_site.empty()) hb.gate_name = sanitise(tile + "_" + fasm_site);
            } else if (bel == "MMCME2_ADV" || bel == "PLLE2_ADV") {
                hb.gate_name = sanitise(tile + "_MMCME2_ADV");
            }
            out.hard[cv.first] = hb;
        }
    }

    for (const auto &pv : place.members()) {
        if (pv.second.get("type").asString() != "SLICE_FFX")
            continue;
        std::string bel = pv.second.get("bel").asString();
        // AFF..DFF and A5FF..D5FF.  The second flip-flop of a column is
        // observable on the xMUX pin, the main one on xQ.
        if (bel.size() < 3 || bel[0] < 'A' || bel[0] > 'D') {
            out.skipped++;
            continue;
        }
        bool is5 = (bel[1] == '5');
        if (bel.substr(is5 ? 2 : 1) != "FF") {
            out.skipped++;
            continue;
        }
        const json::Value &cell = mod->get("cells").get(pv.first);
        if (cell.isNull()) {
            out.skipped++;
            continue;
        }
        const auto &q = cell.get("connections").get("Q").items();
        if (q.empty() || q[0].type != json::Type::Int) {
            out.skipped++;
            continue;
        }
        auto lb = label.find(q[0].asInt());
        if (lb == label.end()) {
            out.skipped++;
            continue;
        }
        std::string tile = pv.second.get("tile").asString();
        std::string site = pv.second.get("site").asString();
        auto pins = pins_for(tile, site);
        auto wire = pins.find(std::string(1, bel[0]) + (is5 ? "MUX" : "Q"));
        if (wire == pins.end()) {
            out.skipped++;
            continue;
        }
        out.net[tile + "/" + wire->second] = lb->second;
        {
            auto a = alt_label.find(q[0].asInt());
            if (a != alt_label.end()) out.alt[tile + "/" + wire->second] = a->second;
        }
        out.mapped++;
    }
    return out;
}

}  // namespace lvs
