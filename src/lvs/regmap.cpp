#include "lvs/regmap.hpp"

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
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
    for (const auto &nn : mod->get("netnames").members()) {
        const json::Value &hide = nn.second.get("hide_name");
        if (!hide.isNull() && hide.asInt() != 0)
            continue;
        const auto &bits = nn.second.get("bits").items();
        for (size_t i = 0; i < bits.size(); i++) {
            if (bits[i].type != json::Type::Int)
                continue;
            int64_t b = bits[i].asInt();
            if (!label.count(b))
                label[b] = bits.size() == 1 ? nn.first : nn.first + "[" + std::to_string(i) + "]";
        }
    }

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
        out.mapped++;
    }
    return out;
}

}  // namespace lvs
