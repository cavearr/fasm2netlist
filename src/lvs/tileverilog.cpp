// Prototype: FASM -> Verilog by INSTANTIATING a tile model, rather than by
// inferring what each slice "means".
//
// The whole point is that the only understanding of the FPGA lives in one
// place -- the model emitted at the top of the output -- so a fix there fixes
// every design.  This file just wires it up:
//
//   * nets come from the database: (tile, wire) pairs, joined across tile
//     boundaries by tileconn.json, so connectivity is a property of the
//     silicon rather than of this program;
//   * every routing feature in the FASM becomes one `assign`;
//   * every occupied slice becomes one `xclb` instance whose parameters are
//     the decoded configuration.
//
// BEST GUESSES, all of them in the model and none of them elsewhere:
//   - O6 = INIT[{A6..A1}], O5 = the low half read with A6 low.
//   - the main FF takes O6 / O5 / the X bypass; XOR, CY, MC31 and F7/F8 are
//     NOT modelled and select a tied 0 (the instance reports it).
//   - the xMUX carries O6, O5 or the 5FF's Q; the same four selects above are
//     not modelled.  F7/F8 has no site feature at all, so it cannot be seen
//     from here even in principle -- see tests/lvs/harness/README.md.
//   - CE defaults to 1 and SR to 0 unless CEUSEDMUX / SRUSEDMUX say otherwise.
//   - carry, distributed RAM and SRL are absent: a slice using them still gets
//     an instance, and its unmodelled features are listed on stderr.
#include "json.hpp"
#include "lvs/regmap.hpp"
#include "lvs/tileconfig.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <array>
#include <map>
#include <set>
#include <regex>
#include <cstring>
#include <sstream>

using namespace lvs;

namespace {

std::string readFile(const std::string &p)
{
    std::ifstream f(p);
    if (!f) throw std::runtime_error("cannot open " + p);
    std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}

std::string sanitise(const std::string &s)
{
    std::string r;
    for (char c : s) r.push_back(isalnum((unsigned char)c) ? c : '_');
    return r;
}

struct Dsu
{
    std::map<std::string, std::string> parent;
    const std::string &find(const std::string &x)
    {
        auto it = parent.find(x);
        if (it == parent.end()) return parent[x] = x;
        if (it->second == x) return it->second;
        return parent[x] = find(it->second);
    }
    void unite(const std::string &a, const std::string &b)
    {
        std::string ra = find(a), rb = find(b);
        if (ra != rb) parent[ra] = rb;
    }
};

struct TileInfo
{
    std::string type;
    int x = 0, y = 0;
    std::vector<std::pair<std::string, std::string>> sites; // (site name, site type) in x order
};

} // namespace

int main(int argc, char **argv)
{
    std::string fasm, db, device = "xc7vx485t", out_path, model_path, xdc_path, part;
    std::string placement_path, gold_json_path;
    std::vector<std::string> input_pats;   // substrings naming genuinely external nets
    int hops = 0;                          // rounds of reachability growth
    bool simple_clock = true;              // see the note on the clock tree below
    bool use_default_ppips = false;        // `default` ppips: off until justified
    int pullup = 1;                        // value an undriven routing net reads
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&] { return (i + 1 < argc) ? std::string(argv[++i]) : std::string(); };
        if (a == "--fasm") fasm = next();
        else if (a == "--db") db = next();
        else if (a == "--device") device = next();
        else if (a == "--out") out_path = next();
        else if (a == "--model-out") model_path = next();
        else if (a == "--xdc") xdc_path = next();
        else if (a == "--part") part = next();
        else if (a == "--input") input_pats.push_back(next());
        else if (a == "--hops") hops = atoi(next().c_str());
        else if (a == "--routed-clock") simple_clock = false;
        else if (a == "--pullup") pullup = atoi(next().c_str());
        else if (a == "--default-ppips") use_default_ppips = true;
        else if (a == "--placement") placement_path = next();
        else if (a == "--gold-json") gold_json_path = next();
        else { std::cerr << "usage: tileverilog --fasm f.fasm --db <prjxray-db>/<family> "
                            "[--device xc7vx485t] [--out out.v]\n"
                            "                   [--input <substring>]...  nets driven from outside\n"; return 2; }
    }
    if (fasm.empty() || db.empty()) { std::cerr << "need --fasm and --db\n"; return 2; }

    DesignConfig dc = read_fasm(fasm);

    // ---- database -------------------------------------------------------
    json::Value grid = json::parse(readFile(db + "/" + device + "/tilegrid.json"));
    std::map<std::string, TileInfo> tiles;
    std::map<std::pair<int, int>, std::string> by_xy;
    for (const auto &kv : grid.members()) {
        TileInfo ti;
        ti.type = kv.second.get("type").asString();
        ti.x = int(kv.second.get("grid_x").asInt());
        ti.y = int(kv.second.get("grid_y").asInt());
        std::vector<std::pair<int, std::pair<std::string, std::string>>> ss;
        for (const auto &sv : kv.second.get("sites").members()) {
            int sx = 0;
            auto p = sv.first.find('X');
            if (p != std::string::npos) sx = atoi(sv.first.c_str() + p + 1);
            ss.push_back({sx, {sv.first, sv.second.asString()}});
        }
        std::sort(ss.begin(), ss.end());
        for (auto &s : ss) ti.sites.push_back(s.second);
        tiles[kv.first] = ti;
        by_xy[{ti.x, ti.y}] = kv.first;
    }

    // which tiles matter: those the FASM mentions
    std::set<std::string> used;
    for (const auto &kv : dc.slices) used.insert(kv.second.tile);
    for (const auto &kv : dc.other_tiles) used.insert(kv.first);

    // tileconn lives per-device in some families and per-family in others
    auto tileconn_path = [&] {
        std::string per_device = db + "/" + device + "/tileconn.json";
        std::ifstream probe(per_device);
        return probe ? per_device : db + "/tileconn.json";
    }();
    json::Value tc = json::parse(readFile(tileconn_path));

    // The XDC names the design's pads; pull their tiles in before the net
    // closure, or the pad wires never enter the graph and cannot be labelled.
    std::vector<std::pair<std::string, std::string>> xdc_ports;   // (port, pin)
    std::map<std::string, std::pair<std::string, std::string>> pin_site;  // pin -> (site, tile)
    if (!xdc_path.empty() && !part.empty()) {
        std::ifstream pf(db + "/" + part + "/package_pins.csv");
        std::string line;
        std::getline(pf, line);
        while (std::getline(pf, line)) {
            std::vector<std::string> f;
            std::string cur;
            for (char c : line) { if (c == ',') { f.push_back(cur); cur.clear(); } else cur.push_back(c); }
            f.push_back(cur);
            if (f.size() >= 4) pin_site[f[0]] = {f[2], f[3]};
        }
        std::ifstream xf(xdc_path);
        // `set_property PACKAGE_PIN <pin> [get_ports <name>]`, plain or -dict,
        // with the port optionally braced: {led[0]} keeps its subscript.
        // The port name may be braced ({led[0]}) or bare (led[0]).  Bare, it
        // can still carry a bus index, so a complete [digits] group is part of
        // the name while a lone ] closes the [get_ports ...] around it.
        static const std::regex re(R"((?:PACKAGE_PIN|LOC)\s+(\S+).*?get_ports\s*(?:\{\s*([^\}]+?)\s*\}|((?:[^\[\]\s]|\[\d+\])+)))");
        while (std::getline(xf, line)) {
            std::smatch m;
            if (!std::regex_search(line, m, re)) continue;
            std::string port = m[2].matched ? m[2].str() : m[3].str();
            xdc_ports.push_back({port, m[1].str()});
            auto ps = pin_site.find(m[1].str());
            if (ps != pin_site.end()) used.insert(ps->second.second);
        }
        std::cerr << "  XDC: " << xdc_ports.size() << " pin constraint(s)\n";
    }

    // site pins per tile type, from tile_type_*.json
    std::map<std::string, std::vector<std::map<std::string, std::string>>> site_pins; // type -> per-site pin->wire
    auto load_type = [&](const std::string &type) {
        if (site_pins.count(type)) return;
        std::string path = db + "/tile_type_" + type + ".json";
        std::ifstream probe(path);
        if (!probe) { site_pins[type] = {}; return; }
        json::Value tt = json::parse(readFile(path));
        std::vector<std::map<std::string, std::string>> per;
        for (const auto &s : tt.get("sites").items()) {
            std::map<std::string, std::string> m;
            for (const auto &pk : s.get("site_pins").members())
                m[pk.first] = pk.second.get("wire").asString();
            per.push_back(m);
        }
        site_pins[type] = per;
    };
    for (const auto &t : used) if (tiles.count(t)) load_type(tiles[t].type);

    // ---- nets: (tile,wire), joined across tile boundaries ---------------
    Dsu dsu;
    auto tw = [](const std::string &t, const std::string &w) { return t + "/" + w; };

    // Which (tile,wire) endpoints the design actually touches.  A route runs
    // through tiles that set no bit and so appear nowhere in the FASM, which
    // is why joining only the named tiles leaves the clock spine in pieces.
    // Grow from the known endpoints instead: join a wire pair when either end
    // is already reachable, and repeat.  That follows the design's own
    // connectivity rather than a radius, so it closes the chain without
    // dragging in the rest of the die.
    std::set<std::string> known;
    for (const auto &kv : dc.other_tiles)
        for (const auto &feat : kv.second) {
            auto dot = feat.find('.');
            if (dot == std::string::npos || feat.find('.', dot + 1) != std::string::npos) continue;
            known.insert(tw(kv.first, feat.substr(0, dot)));
            known.insert(tw(kv.first, feat.substr(dot + 1)));
        }
    for (const auto &kv : dc.slices) {
        auto ti = tiles.find(kv.second.tile);
        if (ti == tiles.end()) continue;
        int ordinal = kv.second.site.back() - '0';
        const auto &per = site_pins[ti->second.type];
        if (ordinal < int(per.size()))
            for (const auto &pw : per[ordinal]) known.insert(tw(kv.second.tile, pw.second));
    }

    std::map<std::string, std::vector<std::string>> tiles_by_type;
    for (const auto &kv : tiles) tiles_by_type[kv.second.type].push_back(kv.first);

    int joins = 0;
    for (int round = 0; round < (hops > 0 ? hops : 4); round++) {
        size_t before = known.size();
        for (const auto &e : tc.items()) {
            const auto &types = e.get("tile_types").items();
            const auto &deltas = e.get("grid_deltas").items();
            if (types.size() != 2 || deltas.size() != 2) continue;
            std::string ta = types[0].asString(), tb = types[1].asString();
            int dx = int(deltas[0].asInt()), dy = int(deltas[1].asInt());
            auto lst = tiles_by_type.find(ta);
            if (lst == tiles_by_type.end()) continue;
            for (const auto &t : lst->second) {
                const TileInfo &ti = tiles[t];
                auto nb = by_xy.find({ti.x + dx, ti.y + dy});
                if (nb == by_xy.end() || tiles[nb->second].type != tb) continue;
                for (const auto &wp : e.get("wire_pairs").items()) {
                    if (wp.items().size() != 2) continue;
                    std::string a = tw(t, wp.items()[0].asString());
                    std::string b = tw(nb->second, wp.items()[1].asString());
                    bool ka = known.count(a), kb = known.count(b);
                    if (!ka && !kb) continue;
                    dsu.unite(a, b);
                    known.insert(a); known.insert(b);
                    joins++;
                }
            }
        }
        if (known.size() == before) break;
    }

    // ---- pseudo-PIPs: the connections that carry no bits ------------------
    // ppips_<type>.db lists hardwired paths.  `always` is permanently on and
    // appears in no bitstream, so a net graph built only from FASM features is
    // missing them -- which leaves, among others, every slice's X bypass
    // undriven and sitting at the pull-up.  `default` is on unless the
    // destination is driven by a real PIP, so it is applied only where nothing
    // else drives it.  `hint` is documentation and is ignored.
    std::map<std::string, std::vector<std::array<std::string, 3>>> ppips; // type -> (dst,src,kind)
    auto load_ppips = [&](const std::string &type) {
        if (ppips.count(type)) return;
        std::string lower;
        for (char c : type) lower.push_back(char(tolower(c)));
        std::ifstream in(db + "/ppips_" + lower + ".db");
        std::vector<std::array<std::string, 3>> v;
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream ls(line);
            std::string feat, kind;
            if (!(ls >> feat >> kind)) continue;
            auto d1 = feat.find('.');
            if (d1 == std::string::npos) continue;
            auto d2 = feat.find('.', d1 + 1);
            if (d2 == std::string::npos) continue;
            v.push_back({feat.substr(d1 + 1, d2 - d1 - 1), feat.substr(d2 + 1), kind});
        }
        ppips[type] = v;
    };
    for (const auto &t : used) if (tiles.count(t)) load_ppips(tiles[t].type);

    // ---- I/O: the fabric's edge ------------------------------------------
    // An IOB or IOI site is a buffer this prototype does not model, so the nets
    // on its pins have no driver inside the fabric and would otherwise sit at
    // the pull-up.  They are the design's actual boundary: expose them as
    // ports, taking the direction from whether the fabric drives the net or
    // reads it.  This is what turns "everything reads 1" into a testbench that
    // can drive a reset and observe an LED.
    std::set<std::string> io_endpoints;
    for (const auto &t : used) {
        auto it = tiles.find(t);
        if (it == tiles.end()) continue;
        const std::string &ty = it->second.type;
        bool is_io = ty.find("IOB") != std::string::npos || ty.find("IOI") != std::string::npos;
        if (!is_io) continue;
        load_type(ty);
        for (const auto &per : site_pins[ty])
            for (const auto &pw : per)
                io_endpoints.insert(tw(t, pw.second));
    }

    // ---- the clock tree, simplified --------------------------------------
    // Not reconstructed: every slice clock is joined straight to the BUFG
    // output that drives it.  The real path (BUFGCTRL -> CLK_HROW -> HCLK ->
    // INT -> CLB) crosses tiles that set no bits and so appear nowhere in the
    // FASM, which leaves the chain in pieces however the net graph is grown.
    // Treating the buffer output as the clock source is what the rest of the
    // toolchain does too, and it is honest about being a simplification --
    // --routed-clock turns it off and leaves the pieces visible.
    std::vector<std::string> bufg_outs;
    for (const auto &kv : dc.other_tiles) {
        if (kv.first.rfind("CLK_BUFG", 0) != 0) continue;
        for (const auto &feat : kv.second) {
            auto dot = feat.find('.');
            if (dot == std::string::npos) continue;
            for (const std::string &w : {feat.substr(0, dot), feat.substr(dot + 1)}) {
                if (w.rfind("CLK_BUFG_BUFGCTRL", 0) == 0 && w.size() > 2 &&
                    w.compare(w.size() - 2, 2, "_O") == 0)
                    bufg_outs.push_back(tw(kv.first, w));
            }
        }
    }
    std::sort(bufg_outs.begin(), bufg_outs.end());
    bufg_outs.erase(std::unique(bufg_outs.begin(), bufg_outs.end()), bufg_outs.end());

    int clocked = 0;
    if (simple_clock && bufg_outs.empty()) {
        // No BUFG output is named in the FASM.  The simplification still
        // holds -- every slice clock is the same net, arriving from outside --
        // so unify them on the first one and let it become a port.
        for (const auto &kv : dc.slices) {
            auto ti = tiles.find(kv.second.tile);
            if (ti == tiles.end()) continue;
            int ordinal = kv.second.site.back() - '0';
            const auto &per = site_pins[ti->second.type];
            if (ordinal >= int(per.size())) continue;
            auto pin = per[ordinal].find("CLK");
            if (pin == per[ordinal].end()) continue;
            bufg_outs.push_back(tw(kv.second.tile, pin->second));
        }
        if (!bufg_outs.empty()) bufg_outs.resize(1);
    }
    // Joining every slice clock to one BUFG is only sound when there is one
    // BUFG.  A design with several has several clock domains, and merging
    // them would say two registers share a clock when the bitstream says they
    // do not -- a claim the checker would then happily prove.  With more than
    // one, the routing has to answer the question instead.
    bool one_clock = bufg_outs.size() == 1;
    if (simple_clock && one_clock) {
        for (const auto &kv : dc.slices) {
            auto ti = tiles.find(kv.second.tile);
            if (ti == tiles.end()) continue;
            int ordinal = kv.second.site.back() - '0';
            const auto &per = site_pins[ti->second.type];
            if (ordinal >= int(per.size())) continue;
            auto pin = per[ordinal].find("CLK");
            if (pin == per[ordinal].end()) continue;
            dsu.unite(tw(kv.second.tile, pin->second), bufg_outs.front());
            clocked++;
        }
    }

    // ---- routing features: one assign each -------------------------------
    std::vector<std::pair<std::string, std::string>> assigns; // dst, src
    int skipped_site_cfg = 0;
    for (const auto &kv : dc.other_tiles) {
        const std::string &tile = kv.first;
        for (const auto &feat : kv.second) {
            auto dot = feat.find('.');
            if (dot == std::string::npos || feat.find('.', dot + 1) != std::string::npos) {
                skipped_site_cfg++;   // site config for a non-slice site, not a PIP
                continue;
            }
            std::string dstw = feat.substr(0, dot), srcw = feat.substr(dot + 1);
            assigns.push_back({tw(tile, dstw), tw(tile, srcw)});
            dsu.find(tw(tile, dstw));
            dsu.find(tw(tile, srcw));
        }
    }


    // ---- pseudo-PIPs -----------------------------------------------------
    // Hardwired paths that carry no configuration bit, so they appear in no
    // FASM.  `always` is permanently on -- without it every slice's X bypass
    // is undriven.  `default` applies only where no real PIP drives the
    // destination.  `hint` is documentation.
    {
        std::set<std::string> pip_driven;
        for (const auto &a : assigns) pip_driven.insert(a.first);
        int n_always = 0, n_default = 0;
        for (const auto &t : used) {
            auto it = tiles.find(t);
            if (it == tiles.end()) continue;
            for (const auto &pp : ppips[it->second.type]) {
                if (pp[2] == "hint") continue;
                if (pp[2] == "default" && !use_default_ppips) continue;
                std::string dst = tw(t, pp[0]), src = tw(t, pp[1]);
                if (!known.count(dst) && !known.count(src)) continue;
                if (pip_driven.count(dst)) continue;
                assigns.push_back({dst, src});
                pip_driven.insert(dst);
                dsu.find(dst); dsu.find(src);
                (pp[2] == "always" ? n_always : n_default)++;
            }
        }
        std::cerr << "  pseudo-PIPs applied: " << n_always << " always, " << n_default << " default\n";
    }

    // With the tree simplified, the clock net's own routing features -- real
    // and pseudo alike -- drive it from pieces of a path we have replaced.
    std::string clock_root;
    if (simple_clock && one_clock) {
        clock_root = dsu.find(bufg_outs.front());
        std::vector<std::pair<std::string, std::string>> kept;
        for (const auto &a : assigns)
            if (dsu.find(a.first) != clock_root) kept.push_back(a);
        if (kept.size() != assigns.size())
            std::cerr << "  dropped " << (assigns.size() - kept.size())
                      << " assign(s) onto the clock net\n";
        assigns.swap(kept);
    }

    // ---- port names from the XDC -----------------------------------------
    // The design already names its own boundary: PACKAGE_PIN ties a port to a
    // package pin, package_pins.csv ties that pin to an IOB site, and the site
    // pin `I`/`O` gives the wire the fabric sees.  Naming the ports after the
    // design rather than after a routing wire is what lets a comparison match
    // them without a hand-written map.
    std::map<std::string, std::string> friendly;   // net root -> XDC port name
    {
        int named = 0;
        for (const auto &[port, pin] : xdc_ports) {
            auto ps = pin_site.find(pin);
            if (ps == pin_site.end()) continue;
            const std::string &tile = ps->second.second, &site = ps->second.first;
            auto ti = tiles.find(tile);
            if (ti == tiles.end()) continue;
            load_type(ti->second.type);
            std::vector<std::string> ss;
            for (const auto &kv : ti->second.sites) ss.push_back(kv.first);
            std::sort(ss.begin(), ss.end(), [](const std::string &a, const std::string &b) {
                auto y = [](const std::string &n) { auto p = n.rfind('Y'); return p == std::string::npos ? 0 : atoi(n.c_str() + p + 1); };
                return y(a) < y(b);
            });
            int ordinal = int(std::find(ss.begin(), ss.end(), site) - ss.begin());
            const auto &per = site_pins[ti->second.type];
            if (ordinal >= int(per.size())) continue;
            for (const char *pn : {"I", "O"}) {
                auto it = per[ordinal].find(pn);
                if (it == per[ordinal].end()) continue;
                std::string ep = tw(tile, it->second);
                if (!dsu.parent.count(ep)) continue;
                friendly[dsu.find(ep)] = port;
                named++;
            }
        }
        if (!xdc_ports.empty())
            std::cerr << "  XDC: labelled " << named << " of " << xdc_ports.size() << " pads\n";
    }

    // The same three files that tell the equivalence checker which fabric net
    // is which register also let the dump be written in the designer's own
    // vocabulary.  A pad's XDC name wins where the two collide: a port name is
    // a fact about the board, a register name only about the synthesis.
    std::map<std::string, std::string> reg_label;   // tile/wire -> source signal
    if (!placement_path.empty() && !gold_json_path.empty()) {
        lvs::RegMap rm = lvs::build_regmap(placement_path, gold_json_path, db, device);
        reg_label = rm.net;
        int named = 0;
        for (const auto &kv : rm.net) {
            if (!dsu.parent.count(kv.first)) continue;
            std::string root = dsu.find(kv.first);
            if (friendly.count(root)) continue;
            friendly[root] = kv.second;
            named++;
        }
        std::cerr << "  placement: labelled " << named << " of " << rm.mapped << " registers\n";
    }

    // ---- I/O logic: the bypass between the fabric and a pad ---------------
    // A design that wants a plain output still has to configure the OLOGIC
    // site sitting in the way, and what it configures is a pass-through:
    // OMUX picking D1, OQ used on the way out.  Without this the pad's net
    // has no driver at all and every bus-shaped output reads as a free
    // variable -- which is what made nextpnr's arty example's twelve LEDs
    // differ while all thirty of its registers proved.
    {
        // Most of these paths the database already declares hardwired: the
        // OLOGIC bypass is a pseudo-PIP, so it arrives with the routing.  The
        // ILOGIC one is not in the database at all, and without it an input
        // pad's net stops at the site boundary.  Emitting a connection that
        // already exists would leave the net with two drivers, so only what
        // is missing is added.
        std::set<std::string> already;
        for (const auto &a : assigns)
            already.insert(a.first);
        int bypassed = 0, unmodelled_io = 0, hardwired = 0;
        for (const auto &kv : dc.iologic) {
            const IoLogicConfig &io = kv.second;
            auto ti = tiles.find(io.tile);
            if (ti == tiles.end()) continue;
            load_type(ti->second.type);
            // The FASM names the site by a Y index and the tile type names
            // its pins after the same index, but the two orders do not agree
            // (the tile's X0Y0 site carries the OLOGIC1 pins), so the wire
            // name is what the two are matched on.
            std::string want = std::string(io.is_output ? "OLOGIC" : "ILOGIC") +
                               io.site.substr(io.site.size() - 1) + "_";
            const char *dst_pin = io.is_output ? "OQ" : "O";
            const char *src_pin = io.is_output ? "D1" : "D";
            for (const auto &pins : site_pins[ti->second.type]) {
                auto d = pins.find(dst_pin), sp = pins.find(src_pin);
                if (d == pins.end() || sp == pins.end()) continue;
                if (d->second.find(want) == std::string::npos) continue;
                if (io.is_bypass()) {
                    std::string dst = tw(io.tile, d->second);
                    if (already.count(dst)) {
                        hardwired++;
                    } else {
                        assigns.push_back({dst, tw(io.tile, sp->second)});
                        already.insert(dst);
                        bypassed++;
                    }
                } else {
                    unmodelled_io++;
                }
                break;
            }
        }
        if (!dc.iologic.empty()) {
            std::cerr << "  I/O logic: " << bypassed << " pass-through added, " << hardwired
                      << " already hardwired";
            if (unmodelled_io)
                std::cerr << ", " << unmodelled_io << " doing more than a wire (not modelled)";
            std::cerr << "\n";
        }
    }

    // One place decides how a net is written: its XDC name where it has one,
    // otherwise the sanitised (tile, wire).  Used by the declarations, the
    // routing assigns and the instance pins alike -- writing a net one way in
    // one place and another way elsewhere silently splits it in two.
    // A column carries a cell of the source design where one was placed on
    // its flip-flop; naming the instance after that cell makes the dump
    // readable.  The suffix keeps the instance out of the net namespace.
    // A column is named after the register it holds.  The placement and the
    // emitter disagree about what to call a site -- one names it absolutely
    // (SLICE_X0Y100), the other by its position in the tile -- but they agree
    // about wires, so the column's output endpoints are what they match on:
    // the main flip-flop leaves on xQ, the second one on xMUX.
    auto inst_name = [&](const std::string &tile, const std::string &site, const std::string &col,
                         const std::string &q, const std::string &mux) {
        for (const std::string &ep : {q, mux}) {
            auto it = reg_label.find(ep);
            if (it != reg_label.end()) return it->second + "_i";
        }
        return sanitise(tile + "_" + site + "_" + col);
    };

    auto emit_net = [&](const std::string &endpoint) {
        std::string root = dsu.find(endpoint);
        auto f = friendly.find(root);
        return f == friendly.end() ? sanitise(root) : ("\\" + f->second + " ");
    };

    // ---- emit ------------------------------------------------------------
    std::ofstream fout;
    std::ostream &os = out_path.empty() ? std::cout : (fout.open(out_path), fout);

    os << "// Generated by tileverilog: one instance per occupied slice, one\n"
          "// assign per routing feature.  All device knowledge is in xclb below.\n"
          "`default_nettype none\n\n";

    std::ofstream mfout;
    std::ostream &ms = model_path.empty() ? os : (mfout.open(model_path), mfout);
    ms << R"(// ---- the tile model: one CLB column, four to a slice --------------------
// O6 is the 6-input read of INIT; O5 is the low half, i.e. the same read with
// A6 held low.  FF_SRC / FF5_SRC / OUTMUX are the decoded selects; the values
// this model does not implement resolve to 0 and are reported by the emitter.
module xcol #(
    parameter [63:0] INIT = 64'h0,
    parameter FF_SRC  = "none",   // O6 | O5 | X | XOR | CY | none
    parameter FF5_SRC = "none",   // O5 | X | none
    parameter OUTMUX  = "none",   // O6 | O5 | 5Q | XOR | CY | none
    parameter CY0     = "X",      // the carry mux data input: O5 or the X bypass
    parameter FF_INIT = 1'b0, parameter FF_SRVAL = 1'b0,
    parameter FF5_INIT = 1'b0, parameter FF5_SRVAL = 1'b0,
    parameter SYNC = 1'b1
) (
    input  wire A1, A2, A3, A4, A5, A6, X,
    input  wire CLK, CE, SR, CI,
    output wire O6, O5, Q, MUX, CO
);
    wire [5:0] idx6 = {A6, A5, A4, A3, A2, A1};
    wire [5:0] idx5 = {1'b0, A5, A4, A3, A2, A1};
    assign O6 = INIT[idx6];
    assign O5 = INIT[idx5];

    // CARRY4, one bit of it: O6 is the propagate select, and the data input
    // is O5 or the bypass depending on CY0.  CO ripples to the next column,
    // and the XOR output is the sum bit.
    wire di  = (CY0 == "O5") ? O5 : X;
    assign CO = O6 ? CI : di;
    wire xo  = O6 ^ CI;

    wire ff_d  = (FF_SRC  == "O6")  ? O6 : (FF_SRC  == "O5") ? O5 :
                 (FF_SRC  == "X")   ? X  : (FF_SRC  == "XOR") ? xo :
                 (FF_SRC  == "CY")  ? CO : 1'b0;
    wire ff5_d = (FF5_SRC == "O5") ? O5 : (FF5_SRC == "X")  ? X  : 1'b0;

    reg q = FF_INIT, q5 = FF5_INIT;
    always @(posedge CLK)
        if (SYNC) begin
            if (SR) q <= FF_SRVAL; else if (CE) q <= ff_d;
        end else begin
            if (CE) q <= ff_d;
        end
    always @(posedge CLK)
        if (SYNC) begin
            if (SR) q5 <= FF5_SRVAL; else if (CE) q5 <= ff5_d;
        end else begin
            if (CE) q5 <= ff5_d;
        end

    assign Q   = q;
    assign MUX = (OUTMUX == "O6")  ? O6 : (OUTMUX == "O5") ? O5 :
                 (OUTMUX == "5Q")  ? q5 : (OUTMUX == "XOR") ? xo :
                 (OUTMUX == "CY")  ? CO : 1'b0;
endmodule

)";
    if (!model_path.empty())
        os << "// tile model written separately to " << model_path << "\n\n";

    // A net nothing in this design drives is something outside it drives: a
    // pad, a clock arriving from a tile we do not model.  Those become module
    // inputs, which is what makes the result simulatable at all.
    // Record raw endpoints, not resolved roots: resolving a slice's pins can
    // merge nets further on, and a root taken too early goes stale.
    std::set<std::string> driven_raw;
    for (const auto &a : assigns) driven_raw.insert(a.first);

    // slice instances (built first: slice outputs count as driven)
    std::ostringstream body;
    std::vector<std::string> carry_wires;
    std::vector<std::pair<std::string, std::string>> carry_assigns;
    int inst = 0, unmodelled = 0;
    for (const auto &kv : dc.slices) {
        const SliceConfig &sc = kv.second;
        auto ti = tiles.find(sc.tile);
        if (ti == tiles.end()) continue;
        int ordinal = sc.site.back() - '0';
        const auto &per = site_pins[ti->second.type];
        if (ordinal >= int(per.size())) continue;
        const auto &pins = per[ordinal];
        auto raw = [&](const std::string &pin) -> std::string {
            auto it = pins.find(pin);
            return it == pins.end() ? std::string() : tw(sc.tile, it->second);
        };
        auto net = [&](const std::string &pin) -> std::string {
            std::string r = raw(pin);
            if (r.empty()) return "1'b0";
            return emit_net(r);
        };
        unmodelled += int(sc.unhandled.size());

        // The carry chain runs A -> B -> C -> D and on to the slice above.
        // Its start comes from PRECYINIT; its end drives the COUT site pin,
        // which tileconn joins to the CIN of the slice above.
        std::string prefix = sanitise(sc.tile + "_" + sc.site);
        bool has_carry = false;
        for (const auto &cc : sc.columns) has_carry |= cc.second.carry_used;
        std::map<char, std::string> ci_of, co_of;
        if (has_carry) {
            std::string start;
            switch (sc.precyinit) {
            case PreCyInit::Zero: start = "1'b0"; break;
            case PreCyInit::One:  start = "1'b1"; break;
            case PreCyInit::AX:   start = net("AX"); break;
            case PreCyInit::CIN:  start = net("CIN"); break;
            default:              start = "1'b0"; break;
            }
            std::string prev = start;
            for (char c : {'A', 'B', 'C', 'D'}) {
                std::string co = prefix + "_CO_" + c;
                carry_wires.push_back(co);
                ci_of[c] = prev;
                co_of[c] = co;
                prev = co;
            }
            std::string cout_pin = net("COUT");
            if (cout_pin != "1'b0") carry_assigns.push_back({cout_pin, co_of['D']});
        }

        for (const auto &cc : sc.columns) {
            char c = cc.first;
            const ColumnConfig &col = cc.second;
            if (!col.init && !col.ff_used && !col.ff5_used && col.outmux == OutMux::None) continue;
            std::string C(1, c);
            const char *ffsrc = col.ff_used ? (col.ff_src_explicit ? to_string(col.ff_src) : "O6") : "none";
            const char *ff5src = col.ff5_used ? (col.ff5_src_explicit ? to_string(col.ff5_src) : "O5") : "none";
            std::ostringstream initv;
            initv << "64'h" << std::hex << (col.init ? *col.init : 0);
            body << "  xcol #(." << "INIT(" << initv.str() << "), .FF_SRC(\"" << ffsrc
               << "\"), .FF5_SRC(\"" << ff5src << "\"), .OUTMUX(\"" << to_string(col.outmux)
               << "\"), .CY0(\"" << (col.cy0_o5 ? "O5" : "X") << "\"),\n"
               << "        .FF_INIT(1'b" << col.ff_init << "), .FF_SRVAL(1'b" << col.ff_srval
               << "), .FF5_INIT(1'b" << col.ff5_init << "), .FF5_SRVAL(1'b" << col.ff5_srval
               << "), .SYNC(1'b" << (sc.ffsync ? 1 : 0) << "))\n"
               << "    \\" << inst_name(sc.tile, sc.site, C, raw(C + "Q"), raw(C + "MUX")) << " (";
            for (int i = 1; i <= 6; i++) body << ".A" << i << "(" << net(C + std::to_string(i)) << "), ";
            body << ".X(" << net(C + "X") << "), .CLK(" << net("CLK") << "), "
               << ".CE(" << (sc.ceusedmux ? net("CE") : std::string("1'b1")) << "), "
               << ".SR(" << (sc.srusedmux ? net("SR") : std::string("1'b0")) << "),\n"
               // O6 leaves the slice on the column's own site pin (A..D); O5
               // has no pin of its own -- it reaches the world through the 5FF
               // or the xMUX, both of which are wired below.
               // The carry into this column: the previous column's CO, or
               // whatever PRECYINIT selected at the bottom of the slice.  A
               // column outside a chain still has the pin, tied low, because
               // the model's XOR and CY paths read it unconditionally.
               << ".CI(" << (ci_of.count(c) ? ci_of[c] : std::string("1'b0")) << "),\n"
               << "     .O6(" << net(C) << "), .O5(), .Q(" << net(C + "Q")
               << "), .MUX(" << net(C + "MUX") << "), .CO("
               << (co_of.count(c) ? co_of[c] : std::string()) << "));\n";
            if (!raw(C).empty()) driven_raw.insert(raw(C));
            if (!raw(C + "Q").empty()) driven_raw.insert(raw(C + "Q"));
            if (!raw(C + "MUX").empty()) driven_raw.insert(raw(C + "MUX"));
            inst++;
        }
    }

    // now the module, in order: ports, internal nets, routing, instances.
    // Taken here, not earlier: resolving a slice's pins adds endpoints.
    std::set<std::string> roots;
    for (const auto &kv : dsu.parent) roots.insert(dsu.find(kv.first));
    std::set<std::string> driven;
    for (const auto &x : driven_raw) driven.insert(sanitise(dsu.find(x)));
    // The interconnect's constant rails are named, not undriven.  A net that
    // reaches GND_WIRE reads 0 and one that reaches VCC_WIRE reads 1; only a
    // net that reaches nothing at all takes the pull-up.  Getting this wrong
    // is invisible in a design without carries -- an unused LUT input reads
    // the same either way -- and fatal in one with them, because a carry
    // chain starting from a grounded CYINIT then starts from 1.
    std::map<std::string, int> rail;   // sanitised root -> the value it is held at
    for (const auto &kv : dsu.parent) {
        const std::string &ep = kv.first;
        auto ends_with = [&](const char *suffix) {
            size_t n = strlen(suffix);
            return ep.size() >= n && ep.compare(ep.size() - n, n, suffix) == 0;
        };
        int v = ends_with("GND_WIRE") ? 0 : ends_with("VCC_WIRE") ? 1 : -1;
        if (v < 0)
            continue;
        std::string root = sanitise(dsu.find(ep));
        auto it = rail.find(root);
        if (it != rail.end() && it->second != v)
            std::cerr << "  warning: " << root << " reaches both rails\n";
        rail[root] = v;
    }

    std::vector<std::string> ports, outs, tied;
    for (const auto &r : roots) {
        std::string n = sanitise(r);
        bool clock = (!clock_root.empty() && r == clock_root);
        if (driven.count(n) && !clock) {
            // driven inside the fabric: only interesting if it reaches a pad
            for (const auto &e : io_endpoints)
                if (dsu.find(e) == r) { outs.push_back(n); break; }
            continue;
        }
        bool is_port = (!clock_root.empty() && r == clock_root);
        if (!is_port)
            for (const auto &e : io_endpoints)
                if (dsu.find(e) == r) { is_port = true; break; }
        for (const auto &pat : input_pats)
            if (r.find(pat) != std::string::npos) { is_port = true; break; }
        (is_port ? ports : tied).push_back(n);
    }
    int railed = 0;
    for (const auto &t : tied)
        railed += rail.count(t) != 0;

    // a port keeps the design's own name where the XDC gave one
    std::vector<std::string> port_decl, out_decl;
    std::map<std::string, std::string> emit_as;
    for (const auto &r : roots) {
        std::string n = sanitise(r);
        auto f = friendly.find(r);
        if (f != friendly.end()) emit_as[n] = "\\" + f->second + " ";
    }
    for (const auto &n : ports) port_decl.push_back(emit_as.count(n) ? emit_as[n] : n);
    for (const auto &n : outs) out_decl.push_back(emit_as.count(n) ? emit_as[n] : n);
    os << "// " << dsu.parent.size() << " (tile,wire) endpoints -> " << roots.size()
       << " nets, joined by " << joins << " tileconn pairs\n"
       << "// " << ports.size() << " inputs, " << outs.size() << " outputs, "
       << (tied.size() - size_t(railed)) << " undriven nets at the interconnect pull-up, "
       << railed << " held at a GND/VCC rail\n"
       << "module fabric (\n";
    for (size_t i = 0; i < ports.size(); i++)
        os << "  input wire " << port_decl[i] << (i + 1 < ports.size() || !outs.empty() ? ",\n" : "\n");
    for (size_t i = 0; i < outs.size(); i++)
        os << "  output wire " << out_decl[i] << (i + 1 < outs.size() ? ",\n" : "\n");
    os << ");\n";
    std::set<std::string> is_port(ports.begin(), ports.end());
    is_port.insert(outs.begin(), outs.end());
    for (const auto &r : roots) {
        std::string n = sanitise(r);
        if (driven.count(n) && !is_port.count(n)) os << "  wire " << emit_net(r) << ";\n";
    }
    for (const auto &t : tied) {
        auto rv = rail.find(t);
        bool on_rail = rv != rail.end();
        os << "  wire " << (emit_as.count(t) ? emit_as[t] : t) << " = 1'b"
           << (on_rail ? rv->second : pullup) << ";   // "
           << (on_rail ? (rv->second ? "VCC rail" : "GND rail") : "undriven") << "\n";
    }
    os << "\n";
    for (const auto &a : assigns)
        os << "  assign " << emit_net(a.first) << " = " << emit_net(a.second) << ";\n";
    for (const auto &w : carry_wires) os << "  wire " << w << ";\n";
    for (const auto &a : carry_assigns) os << "  assign " << a.first << " = " << a.second << ";\n";
    os << "\n" << body.str() << "endmodule\n";

    if (simple_clock && one_clock)
        std::cerr << "  clock tree simplified: " << clocked << " slice clocks joined to "
                  << bufg_outs.size() << " BUFG output\n";
    else if (simple_clock && bufg_outs.size() > 1)
        std::cerr << "  " << bufg_outs.size() << " BUFG outputs: clock tree left to the routing,"
                  << " since one clock per design is what the simplification assumes\n";
    std::cerr << "tileverilog: " << dc.slices.size() << " slices, " << inst << " column instances, "
              << assigns.size() << " routing assigns, " << roots.size() << " nets\n";
    if (unmodelled) std::cerr << "  " << unmodelled << " slice features not modelled (see tiledump --gaps)\n";
    if (skipped_site_cfg) std::cerr << "  " << skipped_site_cfg << " non-slice site features skipped\n";
    return 0;
}
