#include "solver.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unistd.h>

namespace lvs {

const char *to_string(Result r)
{
    switch (r) {
    case Result::Unsat:
        return "unsat";
    case Result::Sat:
        return "sat";
    default:
        return "unknown";
    }
}

Result run_solver(const Solver &solver, const std::string &text)
{
    namespace fs = std::filesystem;
    fs::path path = fs::temp_directory_path() /
                    ("lvs_miter_" + std::to_string(::getpid()) + "_" + std::to_string(rand()) +
                     (solver.format == Format::Dimacs ? ".cnf" : ".smt2"));
    {
        std::ofstream out(path);
        if (!out)
            throw std::runtime_error("cannot write " + path.string());
        out << text;
    }

    std::string cmd = solver.command + " " + path.string() + " 2>/dev/null";
    std::string output;
    if (FILE *pipe = ::popen(cmd.c_str(), "r")) {
        std::array<char, 4096> buf;
        while (std::fgets(buf.data(), int(buf.size()), pipe))
            output += buf.data();
        ::pclose(pipe);
    }
    fs::remove(path);

    // Both answer conventions in one pass: SAT solvers print "s SATISFIABLE",
    // SMT solvers print "sat".  Check unsat first -- "sat" is a substring of
    // "unsat", and of "UNSATISFIABLE" once lowercased.
    for (auto &c : output)
        c = char(::tolower(c));
    if (output.find("unsat") != std::string::npos)
        return Result::Unsat;
    if (output.find("sat") != std::string::npos)
        return Result::Sat;
    return Result::Unknown;
}

} // namespace lvs
