// Netlist -> Boolean cone.
//
// Turns each register's next-state function, and each output port bit, into a
// BoolNet expression over primary inputs and register-state symbols.  The
// symbols are named after the NET a register drives, which is the whole
// mechanism by which two netlists get compared: give the matched registers
// the same net name on both sides and their cones are expressed over the same
// variables, so a miter between them asks the right question.  Nothing here
// discovers that correspondence -- it is an input.
#ifndef LVS_CONE_HPP
#define LVS_CONE_HPP

#include "lvs/boolnet.hpp"
#include "lvs/netlist.hpp"

#include <map>
#include <set>
#include <string>

namespace lvs {

class Cones
{
  public:
    Cones(const Module &m, BoolNet &net,
          const std::map<std::string, std::string> &rename = {});

    // Next-state of the register driving `state_name`, as a literal.
    Lit next_state(const std::string &state_name);
    // Value of an output port bit, e.g. ("led", 3).
    Lit output_bit(const std::string &port, int bit);
    // Any net, by name -- for comparing an internal node rather than a
    // register or a port.  Localising a difference means being able to ask
    // about the signals between them.
    Lit value_of(const std::string &net) { return eval_net(net, 0); }

    const std::set<std::string> &states() const { return states_; }
    // Every name this net answers to.  A net can be named more than once --
    // `assign \core.johnson[0] = \led_int[0]` makes both names the same wire
    // -- and the two sides of a comparison need not have picked the same one.
    std::set<std::string> synonyms(const std::string &net) const;
    const std::set<std::string> &free_nets() const { return free_nets_; }
    std::vector<std::pair<std::string, int>> output_bits() const;

  private:
    struct Driver
    {
        const Instance *inst;
        std::string pin;
    };

    Lit eval_net(const std::string &net_name, int depth);
    Lit eval_expr(const Expr &e, int depth);
    // One bit of a possibly-wide connection: a concatenation, a sized
    // constant, or a part select.  Carry logic is written as buses even in a
    // gate-level netlist, so bit-addressing them is not optional.
    Lit eval_bit(const Expr &e, int bit, int depth);
    Lit eval_cell_output(const Instance &inst, const std::string &pin, int depth);
    std::string resolve(std::string n) const;
    std::string n_for_output(const Instance &inst, const std::string &pin) const;
    Lit sym_state(const std::string &x);

    const Module &mod_;
    BoolNet &net_;
    std::map<std::string, Driver> driver_;   // net -> what drives it
    std::map<std::string, std::string> alias_;
    std::map<std::string, bool> const_net_;   // `assign n = 1'b0;` tie-offs
    std::map<std::string, const Instance *> ff_by_state_;
    std::set<std::string> states_, free_nets_, inputs_;
    std::map<std::string, Lit> memo_;
    std::map<std::string, std::string> rename_;   // this side's net -> shared symbol
};

} // namespace lvs

#endif
