// Per-register equivalence between two structural netlists, through whatever
// SAT or SMT solver you point it at.
//
// Both sides' cones are built over the same variable names -- primary inputs
// by port name, register state by the net the register drives -- so a matched
// pair's miter asks whether the two next-state functions differ for ANY input
// and ANY state.  That is the question yosys's equiv_simple asks per point,
// and the one a plain equiv_miter does not: without the shared symbols a
// solver is free to start the two designs in different states and every
// register "differs".
#include "lvs/boolnet.hpp"
#include "lvs/cone.hpp"
#include "lvs/netlist.hpp"
#include "lvs/solver.hpp"
#include "lvs/regmap.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include "json.hpp"

#include <regex>
#include <sstream>
#include <string>

using namespace lvs;

namespace {

void usage()
{
    std::cerr << "usage: lvs_equiv --gold <a.v> --gate <b.v> [--top NAME]\n"
                 "                 [--solver libz3|'cmd'] [--format dimacs|smt2]\n"
                 "                 [--dump-prefix PATH] [--quiet]\n"
                 "\n"
                 "  --solver libz3  the linked library, one incremental session"
              << (have_linked_z3() ? " (default)\n" : " -- NOT IN THIS BUILD\n")
              << "  --solver <cmd>  any binary reading the chosen format, run per question\n";
}

std::string emit(const BoolNet &net, Lit target, Format fmt)
{
    std::ostringstream os;
    if (fmt == Format::Dimacs)
        write_dimacs(net, target, os);
    else
        write_smtlib2(net, target, os);
    return os.str();
}

} // namespace

static int run(int argc, char **argv)
{
    std::string gold_path, gate_path, top = "top", gold_top, gate_top, dump_prefix, map_path;
    std::vector<std::pair<std::string, std::string>> compare;   // arbitrary net pairs
    std::string placement_p, gold_json_p, db, device;
    Solver solver{have_linked_z3() ? linked_z3_name() : "z3", Format::SmtLib2};
    bool quiet = false;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() { return (i + 1 < argc) ? std::string(argv[++i]) : std::string(); };
        if (a == "--gold") gold_path = next();
        else if (a == "--gate") gate_path = next();
        else if (a == "--top") top = next();
        else if (a == "--gold-top") gold_top = next();
        else if (a == "--gate-top") gate_top = next();
        else if (a == "--solver") solver.command = next();
        else if (a == "--dump-prefix") dump_prefix = next();
        else if (a == "--map") map_path = next();
        else if (a == "--compare") { std::string g = next(); compare.push_back({g, next()}); }
        else if (a == "--placement") placement_p = next();
        else if (a == "--gold-json") gold_json_p = next();
        else if (a == "--db") db = next();
        else if (a == "--device") device = next();
        else if (a == "--quiet") quiet = true;
        else if (a == "--format") solver.format = (next() == "dimacs") ? Format::Dimacs : Format::SmtLib2;
        else { usage(); return 2; }
    }
    if (gold_path.empty() || gate_path.empty()) { usage(); return 2; }

    Netlist gold_nl, gate_nl;
    try {
        gold_nl = parse_verilog_file(gold_path);
        gate_nl = parse_verilog_file(gate_path);
    } catch (const std::exception &e) {
        std::cerr << "parse error: " << e.what() << "\n";
        return 2;
    }
    const Module *gold_m = gold_nl.find(gold_top.empty() ? top : gold_top);
    const Module *gate_m = gate_nl.find(gate_top.empty() ? top : gate_top);
    if (!gold_m || !gate_m) {
        std::cerr << "module " << top << " not found in both netlists\n";
        return 2;
    }

    // gold_name <TAB> gate_name.  Both sides' cones are built over one symbol
    // table, so renaming the gate's symbols to the gold's names is what makes a
    // miter between them ask about the same variables.  Without it the two
    // netlists share no names at all and every register "differs".
    std::map<std::string, std::string> gate_to_gold;
    std::map<std::string, std::string> mem_cuts;   // gate read symbol -> gold's

    // Build the register correspondence here rather than in a helper script:
    // it is a filter on this program's own input, and it needs nothing but the
    // placement, the gold netlist and the database.
    if (!placement_p.empty() && !gold_json_p.empty() && !db.empty()) {
        lvs::RegMap rm = lvs::build_regmap(placement_p, gold_json_p, db, device);
        auto sanitise = [](const std::string &in) {
            std::string r;
            for (char c : in) r.push_back(isalnum((unsigned char)c) ? c : '_');
            return r;
        };
        for (const auto &kv : rm.net) gate_to_gold[sanitise(kv.first)] = kv.second;
        mem_cuts = rm.mem;
        if (!mem_cuts.empty())
            std::cout << "memory map: " << mem_cuts.size() << " read symbols from the placement\n";
        std::cout << "register map: " << rm.mapped << " from the placement";
        if (rm.skipped) std::cout << ", " << rm.skipped << " unmapped";
        std::cout << " (gold module " << rm.module << ")\n";
    }

    if (!map_path.empty()) {
        std::ifstream mf(map_path);
        if (!mf) { std::cerr << "cannot open " << map_path << "\n"; return 2; }
        std::string line;
        while (std::getline(mf, line)) {
            auto tab = line.find('\t');
            if (tab == std::string::npos) continue;
            gate_to_gold[line.substr(tab + 1)] = line.substr(0, tab);
        }
        std::cout << "name map: " << gate_to_gold.size() << " pairs\n";
    }

    BoolNet net;
    Cones gold(*gold_m, net);

    // The map's gold-side label need not be the name gold's own netlist uses:
    // a net can answer to several names (yosys writes the top-level `led_int`,
    // the placement labels it by the RTL name `core.johnson`).  Resolve each
    // label to the state name gold actually carries, or the two sides end up
    // with different free variables for the same register and every cone
    // "differs".
    {
        std::map<std::string, std::string> resolved;
        for (const auto &[gate_net, label] : gate_to_gold) {
            std::string target = label;
            for (const auto &g : gold.states())
                if (g == label || gold.synonyms(g).count(label)) { target = g; break; }
            resolved[gate_net] = target;
        }
        gate_to_gold.swap(resolved);
    }
    Cones gate(*gate_m, net, gate_to_gold);
    gate.set_mem_cuts(mem_cuts);

    // Match by any shared name, not just an identical one.  With --map the
    // gate's states already carry the gold's names, so this is an equality.
    std::vector<std::pair<std::string, std::string>> common;   // (gold, gate)
    for (const auto &g : gold.states()) {
        auto gsyn = gold.synonyms(g);
        for (const auto &t : gate.states()) {
            // with a map, a gate state carries the gold's name for it
            auto mapped = gate_to_gold.find(t);
            if (mapped != gate_to_gold.end()) {
                if (gsyn.count(mapped->second)) { common.emplace_back(g, t); break; }
                continue;
            }
            auto tsyn = gate.synonyms(t);
            bool shared = false;
            for (const auto &n : gsyn)
                if (tsyn.count(n)) { shared = true; break; }
            if (shared) { common.emplace_back(g, t); break; }
        }
    }
    std::sort(common.begin(), common.end());

    std::cout << "registers: " << gold.states().size() << " gold, " << gate.states().size()
              << " gate, " << common.size() << " matched by name\n";
    if (common.size() != gold.states().size() || common.size() != gate.states().size())
        std::cout << "  (unmatched registers are skipped -- their cones are not comparable)\n";
    // One session for the whole run: both designs' cones live in `net`, and a
    // solver told that once can keep what it learns from one register to the
    // next instead of meeting the same network again per question.
    std::unique_ptr<Session> session = make_session(net, solver);
    std::cout << "solver: " << session->describe() << "\n\n";

    int proved = 0, differ = 0, unknown = 0;
    auto t0 = std::chrono::steady_clock::now();

    auto check = [&](const std::string &label, Lit a, Lit b) {
        Lit miter = net.mk_xor(a, b);
        if (!dump_prefix.empty()) {
            // the same question in a form another solver can be handed
            std::string safe = label;
            for (char &c : safe) if (!isalnum(c)) c = '_';
            std::ofstream(dump_prefix + safe + (solver.format == Format::Dimacs ? ".cnf" : ".smt2"))
                << emit(net, miter, solver.format);
        }
        Result r = session->check(miter);
        if (r == Result::Unsat) { proved++; if (!quiet) std::cout << "  proved  " << label << "\n"; }
        else if (r == Result::Sat) { differ++; std::cout << "  DIFFER  " << label << "\n"; }
        else { unknown++; std::cout << "  unknown " << label << "\n"; }
    };

    for (const auto &[gn, tn] : compare)
        check(gn + " vs " + tn, gold.value_of(gn), gate.value_of(tn));
    if (!compare.empty() && common.empty()) {
        auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::cout << "\n" << proved << " proved, " << differ << " differ, " << unknown
                  << " unknown   (" << secs << "s)\n";
        return differ == 0 && unknown == 0 ? 0 : 1;
    }

    for (const auto &[g, t] : common)
        check(g == t ? g : (g + " = " + t), gold.next_state(g), gate.next_state(t));
    auto gate_outs = gate.output_bits();
    for (const auto &[port, bit] : gold.output_bits()) {
        // The two sides need not agree on bus-ness: gold declares an 8-bit
        // `led`, while the fabric names each pad separately from the XDC and so
        // declares scalars called `led[0]`..`led[7]`.  Accept either shape.
        std::string scalar = bit < 0 ? port : port + "[" + std::to_string(bit) + "]";
        bool gate_has = false;
        for (const auto &[gp, gb] : gate_outs)
            if ((gp == port && gb == bit) || (gp == scalar && gb < 0)) { gate_has = true; break; }
        if (!gate_has) {
            std::cout << "  skipped " << port << " -- the gate has no such port"
                      << " (map its pin from the XDC to compare it)\n";
            continue;
        }
        std::string label = bit < 0 ? port : port + "[" + std::to_string(bit) + "]";
        check(label, gold.output_bit(port, bit < 0 ? 0 : bit), gate.output_bit(port, bit < 0 ? 0 : bit));
    }

    // The memories.  Their contents were cut, so what is left to prove is the
    // boundary: read address, write address, write data, write enable.  Prove
    // those equal and the contents are equal by construction -- which is both
    // why the cut is sound and what checks the pairing that made it.
    if (!mem_cuts.empty()) {
        const auto &gp = gold.mem_ports();
        const auto &tp = gate.mem_ports();
        // Pair by the symbol a memory's first read produces: the placement
        // already renamed the gate's to the gold's, so equal symbols are the
        // paired memories, whatever either side calls anything else.
        std::map<std::string, const Cones::MemPort *> by_sym;
        for (const auto &m : gp)
            if (!m.out_sym.empty()) by_sym[m.out_sym.front()] = &m;
        int mem_pairs = 0;
        for (const auto &t : tp) {
            if (t.out_sym.empty()) continue;
            auto want = mem_cuts.find(t.out_sym.front());
            if (want == mem_cuts.end()) continue;
            auto g = by_sym.find(want->second);
            if (g == by_sym.end()) continue;
            mem_pairs++;
            const Cones::MemPort &G = *g->second;
            auto vec = [&](const char *what, const std::vector<Lit> &a, const std::vector<Lit> &b) {
                size_t n = std::max(a.size(), b.size());
                for (size_t i = 0; i < n; i++)
                    check(t.where + " " + what + "[" + std::to_string(i) + "]",
                          i < b.size() ? b[i] : LIT_FALSE, i < a.size() ? a[i] : LIT_FALSE);
            };
            vec("read address", t.read_addr, G.read_addr);
            vec("write address", t.write_addr, G.write_addr);
            {
                size_t n = std::max(t.write_data.size(), G.write_data.size());
                for (size_t i = 0; i < n; i++) {
                    if (i < G.write_dontcare.size() && G.write_dontcare[i]) continue;
                    check(t.where + " write data[" + std::to_string(i) + "]",
                          i < G.write_data.size() ? G.write_data[i] : LIT_FALSE,
                          i < t.write_data.size() ? t.write_data[i] : LIT_FALSE);
                }
            }
            check(t.where + " write enable", G.write_enable, t.write_enable);
        }
        std::cout << "memories: " << gp.size() << " gold, " << tp.size() << " gate, "
                  << mem_pairs << " paired and checked at the boundary\n";
    }

    auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "\n" << proved << " proved, " << differ << " differ, " << unknown << " unknown"
              << "   (" << secs << "s)\n";
    if (!gold.free_nets().empty() || !gate.free_nets().empty())
        std::cout << "undriven nets treated as free: " << gold.free_nets().size() << " gold, "
                  << gate.free_nets().size() << " gate\n";
    return differ == 0 && unknown == 0 ? 0 : 1;
}

// A setup fault -- no solver on the machine, an unreadable database -- is not
// a verdict about the design, and must not be printed as one.
int main(int argc, char **argv)
{
    try {
        return run(argc, argv);
    } catch (const std::exception &e) {
        std::cerr << "lvs_equiv: " << e.what() << "\n";
        return 2;
    }
}
