#include "boolnet.hpp"

#include <sstream>

namespace lvs {

namespace {

// Every node index gets a CNF variable.  Node 0 (the constant) becomes
// variable 1, pinned true by a unit clause, so LIT_TRUE/LIT_FALSE need no
// special handling at the point of use.
int cnf_lit(Lit l)
{
    int var = int(node_of(l)) + 1;
    return is_inverted(l) ? -var : var;
}

} // namespace

void write_dimacs(const BoolNet &net, Lit assert_true, std::ostream &os)
{
    std::ostringstream clauses;
    int nclauses = 0;

    clauses << cnf_lit(LIT_TRUE) << " 0\n"; // the constant node is true
    nclauses++;

    uint32_t max_node = 0;
    for (const auto &[idx, n] : net.ands()) {
        max_node = std::max(max_node, idx);
        int z = cnf_lit(idx << 1), a = cnf_lit(n.a), b = cnf_lit(n.b);
        clauses << -z << ' ' << a << " 0\n";
        clauses << -z << ' ' << b << " 0\n";
        clauses << z << ' ' << -a << ' ' << -b << " 0\n";
        nclauses += 3;
    }
    for (const auto &name : net.input_order())
        max_node = std::max(max_node, node_of(net.input_lit(name)));

    clauses << cnf_lit(assert_true) << " 0\n";
    nclauses++;

    // A comment block naming the inputs: DIMACS itself has no notion of names,
    // and without them a returned model is unreadable.
    os << "c fasm2netlist LVS miter\n";
    for (const auto &name : net.input_order())
        os << "c var " << (node_of(net.input_lit(name)) + 1) << ' ' << name << '\n';
    os << "p cnf " << (max_node + 1) << ' ' << nclauses << '\n';
    os << clauses.str();
}

void write_smtlib2(const BoolNet &net, Lit assert_true, std::ostream &os)
{
    auto ref = [](Lit l) {
        std::string s = (node_of(l) == 0) ? "false" : ("n" + std::to_string(node_of(l)));
        return is_inverted(l) ? "(not " + s + ")" : s;
    };

    os << "(set-logic QF_UF)\n";
    for (const auto &name : net.input_order())
        os << "(declare-const n" << node_of(net.input_lit(name)) << " Bool) ; " << name << '\n';
    for (const auto &[idx, n] : net.ands())
        os << "(define-fun n" << idx << " () Bool (and " << ref(n.a) << ' ' << ref(n.b) << "))\n";
    os << "(assert " << ref(assert_true) << ")\n";
    os << "(check-sat)\n";
}

} // namespace lvs
