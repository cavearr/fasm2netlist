#include "lvs/cone.hpp"

#include <cstdlib>
#include <stdexcept>

namespace lvs {

namespace {

const std::set<std::string> FF_TYPES = {"FDRE", "FDSE", "FDCE", "FDPE"};
const std::set<std::string> PASSTHROUGH = {"IBUF", "OBUF", "BUFG", "IBUFDS", "OBUFDS"};

// The tile model emitted by tileverilog.  One CLB column: a 6-input LUT read
// two ways, a main flip-flop, a second flip-flop, and the output mux.  Its
// selects arrive as string parameters, exactly as the FASM decoded them.
bool is_xcol(const std::string &t) { return t == "xcol"; }
std::string param_str(const Instance &i, const std::string &n, const std::string &dflt)
{
    auto v = i.param(n);
    if (!v) return dflt;
    std::string s = *v;
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    return s;
}

// A LUT INIT as written in the netlist: 64'hf0f0..., 16'b0000...  Only the
// value matters here; the declared width is redundant with the pin count.
uint64_t parse_init(const std::string &text)
{
    auto tick = text.find('\'');
    std::string body = (tick == std::string::npos) ? text : text.substr(tick + 1);
    if (body.empty())
        return 0;
    char base = char(::tolower(body[0]));
    std::string digits = body.substr(base == 'b' || base == 'h' || base == 'o' || base == 'd' ? 1 : 0);
    std::string clean;
    for (char c : digits)
        if (c != '_')
            clean.push_back(c);
    int radix = base == 'b' ? 2 : base == 'o' ? 8 : base == 'd' ? 10 : 16;
    return std::strtoull(clean.c_str(), nullptr, radix);
}

std::string bit_name(const std::string &port, int bit) { return port + "[" + std::to_string(bit) + "]"; }

} // namespace

Lit Cones::sym_state(const std::string &x)
{
    auto r = rename_.find(x);
    return net_.input(r == rename_.end() ? x : r->second);
}

std::string Cones::resolve(std::string n) const
{
    std::set<std::string> seen;
    while (alias_.count(n) && !seen.count(n)) {
        seen.insert(n);
        n = alias_.at(n);
    }
    return n;
}

Cones::Cones(const Module &m, BoolNet &net, const std::map<std::string, std::string> &rename)
    : mod_(m), net_(net), rename_(rename)
{
    for (const auto &p : m.ports)
        if (p.dir == PortDecl::Dir::Input || p.dir == PortDecl::Dir::Inout) {
            if (p.range.scalar)
                inputs_.insert(p.name);
            else
                for (int i = 0; i < p.range.width(); i++)
                    inputs_.insert(bit_name(p.name, i));
        }

    // `assign n = 1'b0;` is a tie-off, not a free net.  The extraction uses
    // these for genuinely-unrouted inputs, and missing them turns each one
    // into a free variable -- which makes any miter trivially satisfiable.
    for (const auto &a : m.assigns) {
        if (a.rhs.kind != Expr::Kind::Const)
            continue;
        bool v = parse_init(a.rhs.const_text) & 1;
        if (a.lhs.kind == Expr::Kind::Id)
            const_net_[a.lhs.name] = v;
        else if (a.lhs.kind == Expr::Kind::BitSel)
            const_net_[bit_name(a.lhs.name, a.lhs.index)] = v;
    }

    // `assign a = b;` is an alias, not logic
    for (const auto &a : m.assigns)
        if (a.lhs.kind == Expr::Kind::Id && a.rhs.kind == Expr::Kind::Id)
            alias_[a.lhs.name] = a.rhs.name;
        else if (a.lhs.kind == Expr::Kind::BitSel && a.rhs.kind == Expr::Kind::Id)
            alias_[bit_name(a.lhs.name, a.lhs.index)] = a.rhs.name;
        else if (a.lhs.kind == Expr::Kind::Id && a.rhs.kind == Expr::Kind::BitSel)
            alias_[a.lhs.name] = bit_name(a.rhs.name, a.rhs.index);
        else if (a.lhs.kind == Expr::Kind::BitSel && a.rhs.kind == Expr::Kind::BitSel)
            alias_[bit_name(a.lhs.name, a.lhs.index)] = bit_name(a.rhs.name, a.rhs.index);

    for (const auto &inst : m.instances) {
        for (const auto &pin : inst.pins) {
            bool is_out = (is_xcol(inst.type) && (pin.name == "Q" || pin.name == "MUX" ||
                                                  pin.name == "O6" || pin.name == "O5")) ||
                          (FF_TYPES.count(inst.type) && pin.name == "Q") ||
                          (inst.type == "LUT6_2" && (pin.name == "O6" || pin.name == "O5")) ||
                          (inst.type.rfind("LUT", 0) == 0 && inst.type != "LUT6_2" && pin.name == "O") ||
                          (PASSTHROUGH.count(inst.type) && (pin.name == "O" || pin.name == "OB"));
            if (!is_out)
                continue;
            std::string n;
            if (pin.conn.kind == Expr::Kind::Id)
                n = pin.conn.name;
            else if (pin.conn.kind == Expr::Kind::BitSel)
                n = bit_name(pin.conn.name, pin.conn.index);
            else
                continue;
            driver_[n] = Driver{&inst, pin.name};
            if (FF_TYPES.count(inst.type)) {
                states_.insert(n);
                ff_by_state_[n] = &inst;
            }
            // an xcol column holds two registers: the main FF on Q, and the
            // second FF observable on MUX when the output mux selects it
            if (is_xcol(inst.type)) {
                if (pin.name == "Q" && param_str(inst, "FF_SRC", "none") != "none") {
                    states_.insert(n);
                    ff_by_state_[n] = &inst;
                } else if (pin.name == "MUX" && param_str(inst, "OUTMUX", "none") == "5Q") {
                    states_.insert(n);
                    ff_by_state_[n] = &inst;
                }
            }
        }
    }
}

Lit Cones::eval_expr(const Expr &e, int depth)
{
    switch (e.kind) {
    case Expr::Kind::Unconnected:
        return LIT_FALSE;
    case Expr::Kind::Const: {
        // 1'b1 / 1'b0 -- anything wider has no business on a scalar pin
        uint64_t v = parse_init(e.const_text);
        return (v & 1) ? LIT_TRUE : LIT_FALSE;
    }
    case Expr::Kind::Id:
        return eval_net(e.name, depth);
    case Expr::Kind::BitSel:
        return eval_net(bit_name(e.name, e.index), depth);
    default:
        return LIT_FALSE;
    }
}

Lit Cones::eval_cell_output(const Instance &inst, const std::string &pin, int depth)
{
    auto in = [&](const char *p) {
        const Pin *q = inst.find_pin(p);
        return q ? eval_expr(q->conn, depth + 1) : LIT_FALSE;
    };

    if (PASSTHROUGH.count(inst.type)) {
        Lit i = in("I");
        return (pin == "OB") ? negate(i) : i;
    }

    if (is_xcol(inst.type)) {
        std::vector<Lit> ins;
        for (int i = 1; i <= 6; i++) {
            const Pin *p = inst.find_pin("A" + std::to_string(i));
            ins.push_back(p ? eval_expr(p->conn, depth + 1) : LIT_TRUE);   // unused inputs pull up
        }
        uint64_t init = parse_init(param_str(inst, "INIT", "0"));
        if (pin == "O6")
            return net_.mk_lut(ins, init);
        if (pin == "O5") {
            std::vector<Lit> five(ins.begin(), ins.begin() + 5);
            return net_.mk_lut(five, init & 0xffffffffull);
        }
        if (pin == "Q")
            return sym_state(n_for_output(inst, "Q"));
        if (pin == "MUX") {
            std::string sel = param_str(inst, "OUTMUX", "none");
            if (sel == "5Q") return sym_state(n_for_output(inst, "MUX"));
            if (sel == "O6") return eval_cell_output(inst, "O6", depth);
            if (sel == "O5") return eval_cell_output(inst, "O5", depth);
            return LIT_FALSE;
        }
        return LIT_FALSE;
    }

    if (inst.type == "LUT6_2" || inst.type.rfind("LUT", 0) == 0) {
        std::vector<Lit> ins;
        for (int i = 0;; i++) {
            const Pin *p = inst.find_pin("I" + std::to_string(i));
            if (!p)
                break;
            ins.push_back(eval_expr(p->conn, depth + 1));
        }
        auto init = inst.param("INIT");
        uint64_t val = init ? parse_init(*init) : 0;
        if (inst.type == "LUT6_2") {
            if (pin == "O5") {
                // O5 is the same INIT read with I5 held low: the low half
                std::vector<Lit> five(ins.begin(), ins.begin() + std::min<size_t>(5, ins.size()));
                return net_.mk_lut(five, val & 0xffffffffull);
            }
            return net_.mk_lut(ins, val);
        }
        return net_.mk_lut(ins, val);
    }

    throw std::runtime_error("cone: no model for cell type " + inst.type + " (pin " + pin + ")");
}

Lit Cones::eval_net(const std::string &raw, int depth)
{
    if (depth > 4096)
        throw std::runtime_error("cone: combinational loop at " + raw);
    std::string n = resolve(raw);

    auto it = memo_.find(n);
    if (it != memo_.end())
        return it->second;

    Lit result;
    auto c = const_net_.find(n);
    if (c != const_net_.end()) {
        result = c->second ? LIT_TRUE : LIT_FALSE;
        memo_[n] = result;
        return result;
    }
    auto sym = [&](const std::string &x) {
        auto r = rename_.find(x);
        return net_.input(r == rename_.end() ? x : r->second);
    };
    if (states_.count(n)) {
        // a register output is a free symbol -- shared with the other side
        result = sym(n);
    } else if (inputs_.count(n)) {
        result = sym(n);
    } else {
        auto d = driver_.find(n);
        if (d == driver_.end()) {
            // undriven: a free variable, and worth counting -- an undriven net
            // that exists on only one side is how a comparison goes wrong
            free_nets_.insert(n);
            result = sym(n);
        } else {
            memo_[n] = LIT_FALSE; // break loops while descending
            result = eval_cell_output(*d->second.inst, d->second.pin, depth);
        }
    }
    memo_[n] = result;
    return result;
}

std::set<std::string> Cones::synonyms(const std::string &net) const
{
    std::set<std::string> r{net};
    for (const auto &[from, to] : alias_) {
        (void)to;
        if (resolve(from) == net)
            r.insert(from);
    }
    return r;
}

// the net an xcol drives from a given pin, as a state symbol name
std::string Cones::n_for_output(const Instance &inst, const std::string &pin) const
{
    const Pin *p = inst.find_pin(pin);
    if (!p) return inst.name + "." + pin;
    if (p->conn.kind == Expr::Kind::Id) return p->conn.name;
    if (p->conn.kind == Expr::Kind::BitSel) return bit_name(p->conn.name, p->conn.index);
    return inst.name + "." + pin;
}

Lit Cones::next_state(const std::string &state_name)
{
    auto it = ff_by_state_.find(state_name);
    if (it == ff_by_state_.end())
        throw std::runtime_error("cone: no register drives " + state_name);
    const Instance &ff = *it->second;

    if (is_xcol(ff.type)) {
        auto pin_lit = [&](const char *p, Lit dflt) {
            const Pin *q = ff.find_pin(p);
            return q ? eval_expr(q->conn, 0) : dflt;
        };
        bool second = (n_for_output(ff, "MUX") == state_name);
        std::string src = param_str(ff, second ? "FF5_SRC" : "FF_SRC", "none");
        Lit d = LIT_FALSE;
        if (src == "O6") d = eval_cell_output(ff, "O6", 0);
        else if (src == "O5") d = eval_cell_output(ff, "O5", 0);
        else if (src == "X") d = pin_lit("X", LIT_FALSE);
        Lit ce = pin_lit("CE", LIT_TRUE);
        Lit sr = pin_lit("SR", LIT_FALSE);
        Lit q = sym_state(state_name);
        Lit next = net_.mk_or(net_.mk_and(ce, d), net_.mk_and(negate(ce), q));
        std::string srval = param_str(ff, second ? "FF5_SRVAL" : "FF_SRVAL", "1'b0");
        bool sv = !srval.empty() && srval.back() == '1';
        next = sv ? net_.mk_or(sr, next) : net_.mk_and(negate(sr), next);
        return next;
    }

    auto pin_lit = [&](const char *p, Lit dflt) {
        const Pin *q = ff.find_pin(p);
        return q ? eval_expr(q->conn, 0) : dflt;
    };

    Lit d = pin_lit("D", LIT_FALSE);
    Lit ce = pin_lit("CE", LIT_TRUE);
    Lit q = net_.input(state_name);
    // CE low holds the current value
    Lit next = net_.mk_or(net_.mk_and(ce, d), net_.mk_and(negate(ce), q));

    if (ff.type == "FDRE") {
        Lit r = pin_lit("R", LIT_FALSE);
        next = net_.mk_and(negate(r), next);
    } else if (ff.type == "FDSE") {
        Lit s = pin_lit("S", LIT_FALSE);
        next = net_.mk_or(s, next);
    } else if (ff.type == "FDCE") {
        Lit clr = pin_lit("CLR", LIT_FALSE);
        next = net_.mk_and(negate(clr), next);
    } else if (ff.type == "FDPE") {
        Lit pre = pin_lit("PRE", LIT_FALSE);
        next = net_.mk_or(pre, next);
    }
    return next;
}

Lit Cones::output_bit(const std::string &port, int bit)
{
    for (const auto &p : mod_.ports)
        if (p.name == port && p.range.scalar)
            return eval_net(port, 0);
    return eval_net(bit_name(port, bit), 0);
}

std::vector<std::pair<std::string, int>> Cones::output_bits() const
{
    std::vector<std::pair<std::string, int>> r;
    for (const auto &p : mod_.ports)
        if (p.dir == PortDecl::Dir::Output) {
            if (p.range.scalar)
                r.emplace_back(p.name, -1);
            else
                for (int i = 0; i < p.range.width(); i++)
                    r.emplace_back(p.name, i);
        }
    return r;
}

} // namespace lvs
