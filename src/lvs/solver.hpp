// Run an external solver over a miter written in a standard format.
//
// The solver is a command, not a link-time dependency: whatever reads DIMACS
// or SMT-LIB 2 on the command line can be dropped in without recompiling.
#ifndef LVS_SOLVER_HPP
#define LVS_SOLVER_HPP

#include <string>

namespace lvs {

enum class Format
{
    Dimacs,
    SmtLib2,
};

enum class Result
{
    Unsat, // the miter is unsatisfiable: the two sides agree
    Sat,   // a counterexample exists: they do not
    Unknown,
};

struct Solver
{
    // e.g. {"z3", Format::SmtLib2} or {"cadical", Format::Dimacs}.  The file
    // is passed as the last argument.
    std::string command;
    Format format;

    // Defaults to the solver already assumed by this project, reached through
    // a file rather than an API so that swapping it is a flag.
    static Solver z3() { return Solver{"z3", Format::SmtLib2}; }
};

// Writes `text` to a temporary file, runs the solver on it, and classifies
// stdout.  Recognises both the SAT-competition ("s SATISFIABLE") and SMT-LIB
// ("sat") answer conventions, so one parser covers both formats.
Result run_solver(const Solver &solver, const std::string &text);

const char *to_string(Result r);

} // namespace lvs

#endif
