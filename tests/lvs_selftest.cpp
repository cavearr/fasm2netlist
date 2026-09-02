// Checks the miter plumbing, not any real silicon: that a Boolean net is
// exported correctly in both interchange formats and that a solver reached
// through either one answers the same way.
//
// Exits 77 (CTest's SKIP_RETURN_CODE) when no solver is on PATH -- the point
// of going through a file format is that the solver is optional and
// replaceable, so its absence must not be a build failure.
#include "../src/lvs/boolnet.hpp"
#include "../src/lvs/solver.hpp"

#include <cstdlib>
#include <iostream>
#include <sstream>

using namespace lvs;

namespace {

int failures = 0;

void check(bool ok, const std::string &what)
{
    std::cout << (ok ? "  ok   " : "  FAIL ") << what << '\n';
    if (!ok)
        failures++;
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

bool have(const std::string &cmd)
{
    return std::system(("command -v " + cmd + " >/dev/null 2>&1").c_str()) == 0;
}

} // namespace

int main()
{
    // Two LUTs computing the same function, decomposed differently: a 2-input
    // AND written directly, against a 3-input LUT whose third input does not
    // matter.  This is the shape a real comparison takes -- the same function
    // arrived at from two different decompositions.
    BoolNet net;
    Lit a = net.input("a"), b = net.input("b"), c = net.input("c");
    Lit lhs = net.mk_lut({a, b}, 0x8);              // a & b
    Lit rhs = net.mk_lut({a, b, c}, 0x88);          // a & b, with c present but irrelevant
    Lit differ = net.mk_xor(lhs, rhs);

    // ...and a genuinely different pair, to prove the check can fail.
    Lit wrong = net.mk_lut({a, b}, 0xE);            // a | b
    Lit differ_bad = net.mk_xor(lhs, wrong);

    check(!emit(net, differ, Format::Dimacs).empty(), "DIMACS export is non-empty");
    check(emit(net, differ, Format::Dimacs).find("p cnf ") != std::string::npos, "DIMACS has a header");
    check(emit(net, differ, Format::SmtLib2).find("(check-sat)") != std::string::npos, "SMT-LIB2 asks for a check");

    std::vector<Solver> solvers;
    if (have("z3")) {
        solvers.push_back(Solver{"z3", Format::SmtLib2});
        // the same solver through the other format, so both exporters are
        // covered even when no dedicated SAT solver is installed
        solvers.push_back(Solver{"z3 -dimacs", Format::Dimacs});
    }
    if (have("cadical"))
        solvers.push_back(Solver{"cadical", Format::Dimacs});
    if (have("kissat"))
        solvers.push_back(Solver{"kissat", Format::Dimacs});
    if (have("cryptominisat5"))
        solvers.push_back(Solver{"cryptominisat5", Format::Dimacs});

    if (solvers.empty()) {
        std::cout << "no solver on PATH (tried z3, cadical, kissat, cryptominisat5) -- skipping\n";
        return 77;
    }

    for (const auto &s : solvers) {
        Result equal = run_solver(s, emit(net, differ, s.format));
        Result unequal = run_solver(s, emit(net, differ_bad, s.format));
        check(equal == Result::Unsat, s.command + ": equivalent pair proves UNSAT (got " +
                                              to_string(equal) + ")");
        check(unequal == Result::Sat, s.command + ": different pair is falsified SAT (got " +
                                              to_string(unequal) + ")");
    }
    return failures == 0 ? 0 : 1;
}
