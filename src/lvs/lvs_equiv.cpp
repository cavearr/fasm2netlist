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

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

using namespace lvs;

namespace {

void usage()
{
    std::cerr << "usage: lvs_equiv --gold <a.v> --gate <b.v> [--top NAME]\n"
                 "                 [--solver 'cmd'] [--format dimacs|smt2]\n"
                 "                 [--dump-prefix PATH] [--quiet]\n";
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

int main(int argc, char **argv)
{
    std::string gold_path, gate_path, top = "top", gold_top, gate_top, dump_prefix, map_path;
    Solver solver = Solver::z3();
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
    Cones gold(*gold_m, net), gate(*gate_m, net, gate_to_gold);

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
    std::cout << "solver: " << solver.command << " ("
              << (solver.format == Format::Dimacs ? "DIMACS" : "SMT-LIB2") << ")\n\n";

    int proved = 0, differ = 0, unknown = 0;
    auto t0 = std::chrono::steady_clock::now();

    auto check = [&](const std::string &label, Lit a, Lit b) {
        Lit miter = net.mk_xor(a, b);
        std::string text = emit(net, miter, solver.format);
        if (!dump_prefix.empty()) {
            std::string safe = label;
            for (char &c : safe) if (!isalnum(c)) c = '_';
            std::ofstream(dump_prefix + safe + (solver.format == Format::Dimacs ? ".cnf" : ".smt2")) << text;
        }
        Result r = run_solver(solver, text);
        if (r == Result::Unsat) { proved++; if (!quiet) std::cout << "  proved  " << label << "\n"; }
        else if (r == Result::Sat) { differ++; std::cout << "  DIFFER  " << label << "\n"; }
        else { unknown++; std::cout << "  unknown " << label << "\n"; }
    };

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

    auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "\n" << proved << " proved, " << differ << " differ, " << unknown << " unknown"
              << "   (" << secs << "s)\n";
    if (!gold.free_nets().empty() || !gate.free_nets().empty())
        std::cout << "undriven nets treated as free: " << gold.free_nets().size() << " gold, "
                  << gate.free_nets().size() << " gate\n";
    return differ == 0 && unknown == 0 ? 0 : 1;
}
