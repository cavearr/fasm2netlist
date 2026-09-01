// A minimal and-inverter graph, and exporters for it in the two interchange
// formats a solver is likely to accept.
//
// Everything an LVS miter needs here is combinational Boolean: both sides'
// cells are LUTs with an INIT truth table, and register correspondence turns
// each side's next-state function into an expression over primary inputs and
// (already-matched) register symbols.  So the miter is a pure Boolean formula,
// and the solver is replaceable rather than structural: emit DIMACS CNF and
// any SAT solver will take it; emit SMT-LIB 2 and any SMT solver will.  Z3
// reads both, so nothing here is a new dependency -- it is the same solver
// reached through a standard file format instead of a linked API, which is
// what makes trying CaDiCaL, Kissat, cryptominisat or cvc5 a command-line
// change rather than a code change.
#ifndef LVS_BOOLNET_HPP
#define LVS_BOOLNET_HPP

#include <cstdint>
#include <map>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace lvs {

// Literals are AIGER-style: node index << 1, low bit = inverted.  Node 0 is
// the constant, so literal 0 is FALSE and literal 1 is TRUE.
using Lit = uint32_t;

inline Lit negate(Lit l) { return l ^ 1u; }
inline bool is_inverted(Lit l) { return (l & 1u) != 0; }
inline uint32_t node_of(Lit l) { return l >> 1; }

constexpr Lit LIT_FALSE = 0;
constexpr Lit LIT_TRUE = 1;

class BoolNet
{
  public:
    struct AndNode
    {
        Lit a, b;
    };

    // A named free variable.  Both sides of a miter must ask for the same name
    // for the same thing -- that is the whole correspondence the caller is
    // responsible for.
    Lit input(const std::string &name)
    {
        auto it = inputs_.find(name);
        if (it != inputs_.end())
            return it->second;
        uint32_t idx = next_node_++;
        Lit l = idx << 1;
        inputs_.emplace(name, l);
        input_order_.push_back(name);
        return l;
    }

    Lit mk_and(Lit a, Lit b)
    {
        if (a == LIT_FALSE || b == LIT_FALSE)
            return LIT_FALSE;
        if (a == LIT_TRUE)
            return b;
        if (b == LIT_TRUE)
            return a;
        if (a == b)
            return a;
        if (a == negate(b))
            return LIT_FALSE;
        if (a > b)
            std::swap(a, b);
        auto key = (uint64_t(a) << 32) | b;
        auto it = and_hash_.find(key);
        if (it != and_hash_.end())
            return it->second;
        uint32_t idx = next_node_++;
        ands_[idx] = AndNode{a, b};
        Lit l = idx << 1;
        and_hash_.emplace(key, l);
        return l;
    }

    Lit mk_or(Lit a, Lit b) { return negate(mk_and(negate(a), negate(b))); }
    Lit mk_xor(Lit a, Lit b) { return mk_or(mk_and(a, negate(b)), mk_and(negate(a), b)); }

    // Expand a LUT's INIT truth table into sum-of-products form: one AND
    // clause per set minterm.  Both sides get the same canonical form this
    // way, whatever cell type each started from (a generic LUTk on one side,
    // a LUT6_2 on the other).
    Lit mk_lut(const std::vector<Lit> &ins, uint64_t init)
    {
        if (ins.size() > 6)
            throw std::runtime_error("mk_lut: more than 6 inputs");
        Lit acc = LIT_FALSE;
        for (uint32_t m = 0; m < (1u << ins.size()); m++) {
            if (!((init >> m) & 1))
                continue;
            Lit term = LIT_TRUE;
            for (size_t i = 0; i < ins.size(); i++)
                term = mk_and(term, ((m >> i) & 1) ? ins[i] : negate(ins[i]));
            acc = mk_or(acc, term);
        }
        return acc;
    }

    const std::map<uint32_t, AndNode> &ands() const { return ands_; }
    const std::vector<std::string> &input_order() const { return input_order_; }
    Lit input_lit(const std::string &name) const { return inputs_.at(name); }

  private:
    uint32_t next_node_ = 1; // 0 is the constant node
    std::map<uint32_t, AndNode> ands_;
    std::map<uint64_t, Lit> and_hash_;
    std::map<std::string, Lit> inputs_;
    std::vector<std::string> input_order_;
};

// DIMACS CNF via Tseitin.  Variable 1 is pinned true and stands in for the
// constant node, so a literal needs no special case downstream.
void write_dimacs(const BoolNet &net, Lit assert_true, std::ostream &os);

// SMT-LIB 2, QF_UF over Bool.  Same formula, for solvers that would rather
// read that.
void write_smtlib2(const BoolNet &net, Lit assert_true, std::ostream &os);

} // namespace lvs

#endif
