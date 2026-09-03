#include "lvs/cone.hpp"

#include <cstdlib>
#include <stdexcept>

namespace lvs {

namespace {

const std::set<std::string> FF_TYPES = {"FDRE", "FDSE", "FDCE", "FDPE"};
const std::set<std::string> PASSTHROUGH = {"IBUF", "OBUF", "BUFG", "IBUFDS", "OBUFDS"};
// A bidirectional buffer's O pin carries what the pad is receiving, which
// comes from the pad and not from anything inside the design.  Treating it as
// a connection to the IO pin lets it resolve to the port the constraints name,
// the same way an IBUF's O does.  The drive direction is a different question:
// what the design puts ON the pad is gated by T, and a tri-stated pad does not
// hold a boolean value, so an inout is not compared as an output.
const std::map<std::string, std::string> BIDIR_RECEIVE = {{"IOBUF", "IO"}, {"IOBUFDS", "IO"}};

// The synthesis-side distributed RAMs, and how wide one port's data is.  Each
// port is 64 stored bits either way -- 64 entries of one bit, or 32 of two --
// which is exactly one SLICEM column, and is why the two sides can be matched
// column for column.  The write address is port D's for both.
const std::map<std::string, int> RAM_PORTS = {{"RAM64M", 1}, {"RAM32M", 2}};

// Which stored bit port `p` reads at address `a` on data bit `d`: the address
// for a 64-deep port, and for a 32-deep one the two data bits are the two
// halves of the same column -- the same split the fabric makes between a
// column's O5 and O6 reads.  Used by the state model, which is the flagged
// alternative to cutting at the boundary.
[[maybe_unused]] int ram_bit(int width, int addr, int d) { return width == 1 ? addr : d * 32 + addr; }

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

// How many bits a connection carries.  After `splitnets` every net is a
// scalar, so the only wide things left are constants, part selects and the
// concatenations that carry logic is written with.
int expr_width(const Expr &e)
{
    switch (e.kind) {
    case Expr::Kind::Const: {
        auto tick = e.const_text.find('\'');
        if (tick == std::string::npos || tick == 0)
            return 1;
        int w = std::atoi(e.const_text.substr(0, tick).c_str());
        return w > 0 ? w : 1;
    }
    case Expr::Kind::PartSel:
        return std::abs(e.range.msb - e.range.lsb) + 1;
    case Expr::Kind::Concat: {
        int w = 0;
        for (const auto &p : e.parts)
            w += expr_width(p);
        return w;
    }
    default:
        return 1;
    }
}

// The net carrying bit `bit` of a connection, or "" if that bit is not a
// plain net (a constant, say).  Used to record what an instance drives.
std::string net_of_bit(const Expr &e, int bit)
{
    switch (e.kind) {
    case Expr::Kind::Id:
        return bit == 0 ? e.name : bit_name(e.name, bit);
    case Expr::Kind::BitSel:
        return bit == 0 ? bit_name(e.name, e.index) : std::string();
    case Expr::Kind::PartSel:
        return bit_name(e.name, std::min(e.range.msb, e.range.lsb) + bit);
    case Expr::Kind::Concat: {
        // parts are written most significant first, so bit 0 is at the end
        int seen = 0;
        for (auto it = e.parts.rbegin(); it != e.parts.rend(); ++it) {
            int w = expr_width(*it);
            if (bit < seen + w)
                return net_of_bit(*it, bit - seen);
            seen += w;
        }
        return std::string();
    }
    default:
        return std::string();
    }
}

} // namespace

Lit Cones::sym_state(const std::string &x)
{
    auto r = rename_.find(x);
    if (r != rename_.end())
        return net_.input(r->second);
    // A stored bit is named after the net its column reads onto, so it
    // inherits that net's renaming: rename the column and its whole contents
    // follow.  Without this the two sides would agree on the register that
    // reads a memory and disagree on every bit inside it.
    auto at = x.rfind("$m");
    if (at != std::string::npos) {
        auto rr = rename_.find(x.substr(0, at));
        if (rr != rename_.end())
            return net_.input(rr->second + x.substr(at));
    }
    return net_.input(x);
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

Cones::Cones(const Module &m, BoolNet &net, const std::map<std::string, std::string> &rename,
             bool memory_as_state)
    : memory_as_state_(memory_as_state), mod_(m), net_(net), rename_(rename)
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
        // A carry cell drives eight nets from two four-bit pins, so its
        // outputs are recorded bit by bit; everything else here drives one
        // net from one pin.
        if (inst.type == "CARRY4") {
            for (const char *bus : {"O", "CO"}) {
                const Pin *p = inst.find_pin(bus);
                if (!p)
                    continue;
                for (int i = 0; i < 4; i++) {
                    std::string n = net_of_bit(p->conn, i);
                    if (!n.empty())
                        driver_[n] = Driver{&inst, bit_name(bus, i)};
                }
            }
            continue;
        }

        for (const auto &pin : inst.pins) {
            bool is_out = (is_xcol(inst.type) && (pin.name == "Q" || pin.name == "MUX" ||
                                                  pin.name == "O6" || pin.name == "O5" ||
                                                  pin.name == "CO")) ||
                          (FF_TYPES.count(inst.type) && pin.name == "Q") ||
                          (inst.type == "LUT6_2" && (pin.name == "O6" || pin.name == "O5")) ||
                          (inst.type.rfind("LUT", 0) == 0 && inst.type != "LUT6_2" && pin.name == "O") ||
                          (PASSTHROUGH.count(inst.type) && (pin.name == "O" || pin.name == "OB")) ||
                          (inst.type == "INV" && pin.name == "O") ||
                          (BIDIR_RECEIVE.count(inst.type) && pin.name == "O") ||
                          (inst.type == "IDELAYE2" && pin.name == "DATAOUT") ||
                          (RAM_PORTS.count(inst.type) && pin.name.rfind("DO", 0) == 0);
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
            // A synthesis-side RAM port stores 64 bits, like the column it
            // will be matched against.  Anchor them on the read output the
            // fabric calls O6 -- the top data bit, which is the half a
            // column's O6 reads -- so the two sides name the same thing.
            if (memory_as_state_ && RAM_PORTS.count(inst.type) && pin.name.rfind("DO", 0) == 0) {
                int width = RAM_PORTS.at(inst.type);
                std::string anchor = ram_anchor(inst, pin.name, width);
                if (!anchor.empty())
                    for (int b = 0; b < 64; b++) {
                        std::string sn = mem_state_name(anchor, b);
                        states_.insert(sn);
                        mem_by_state_[sn] = &inst;
                        mem_bit_[sn] = b;
                        mem_port_[sn] = pin.name.size() > 2 ? pin.name[2] : 'A';
                    }
            }
            // A writable column stores 64 bits of state.  Enumerating them
            // here is what puts them in states(), so the prover asks for a
            // next-state function per stored bit the same way it does per
            // flip-flop -- there is no separate kind of thing.
            if (memory_as_state_ && is_xcol(inst.type) && pin.name == "O6" &&
                param_str(inst, "RAM", "0") != "0") {
                for (int b = 0; b < 64; b++) {
                    std::string sn = mem_state_name(n, b);
                    states_.insert(sn);
                    mem_by_state_[sn] = &inst;
                    mem_bit_[sn] = b;
                }
            }
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

void Cones::collect_mem_ports()
{
    auto bits_of = [&](const Instance &inst, const std::string &pin, int n) {
        std::vector<Lit> v;
        const Pin *p = inst.find_pin(pin);
        for (int i = 0; i < n; i++) v.push_back(p ? eval_bit(p->conn, i, 0) : LIT_FALSE);
        return v;
    };
    auto lit_of = [&](const Instance &inst, const std::string &pin, Lit dflt) {
        const Pin *p = inst.find_pin(pin);
        return p ? eval_expr(p->conn, 0) : dflt;
    };

    for (const auto &inst : mod_.instances) {
        if (RAM_PORTS.count(inst.type)) {
            // One MemPort per data port; each is 64 stored bits, which is one
            // fabric column, which is what makes them pairable one to one.
            int width = RAM_PORTS.at(inst.type);
            int abits = (width == 1) ? 6 : 5;
            for (char port = 'A'; port <= 'D'; port++) {
                std::string dopin = std::string("DO") + port;
                if (!inst.find_pin(dopin)) continue;
                MemPort mp;
                mp.where = inst.name + " port " + port;
                mp.read_addr = bits_of(inst, std::string("ADDR") + port, abits);
                mp.write_addr = bits_of(inst, "ADDRD", abits);
                mp.write_data = bits_of(inst, std::string("DI") + port, width);
                {
                    const Pin *dp = inst.find_pin(std::string("DI") + port);
                    bool dc = !dp || (dp->conn.kind == Expr::Kind::Const &&
                                      dp->conn.const_text.find('x') != std::string::npos) ||
                              dp->conn.kind == Expr::Kind::Unconnected;
                    mp.write_dontcare.assign(width, dc);
                }
                mp.write_enable = lit_of(inst, "WE", LIT_FALSE);
                for (int d = 0; d < width; d++)
                    mp.out_sym.push_back(mem_cut_name(inst.name, dopin, d));
                mem_ports_.push_back(std::move(mp));
            }
            continue;
        }
        if (!is_xcol(inst.type) || param_str(inst, "RAM", "0") == "0") continue;
        bool small = param_str(inst, "RAM32", "0") != "0";
        int abits = small ? 5 : 6;
        MemPort mp;
        mp.where = inst.name;
        for (int i = 0; i < abits; i++) {
            const Pin *a = inst.find_pin("A" + std::to_string(i + 1));
            mp.read_addr.push_back(a ? eval_expr(a->conn, 0) : LIT_FALSE);
        }
        mp.write_addr = bits_of(inst, "WA", abits);
        mp.write_data.push_back(lit_of(inst, "DI", LIT_FALSE));
        if (small) mp.write_data.push_back(lit_of(inst, "DI2", LIT_FALSE));
        mp.write_enable = lit_of(inst, "WE", LIT_FALSE);
        for (int d = 0; d < (small ? 2 : 1); d++)
            mp.out_sym.push_back(mem_cut_name(inst.name, "DO", d));
        mem_ports_.push_back(std::move(mp));
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

Lit Cones::eval_bit(const Expr &e, int bit, int depth)
{
    switch (e.kind) {
    case Expr::Kind::Unconnected:
        return LIT_FALSE;
    case Expr::Kind::Const:
        return ((parse_init(e.const_text) >> bit) & 1) ? LIT_TRUE : LIT_FALSE;
    case Expr::Kind::Concat: {
        int seen = 0;
        for (auto it = e.parts.rbegin(); it != e.parts.rend(); ++it) {
            int w = expr_width(*it);
            if (bit < seen + w)
                return eval_bit(*it, bit - seen, depth);
            seen += w;
        }
        return LIT_FALSE;
    }
    default: {
        std::string n = net_of_bit(e, bit);
        return n.empty() ? LIT_FALSE : eval_net(n, depth);
    }
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

    if (inst.type == "INV")
        return negate(in("I"));

    {
        auto bd = BIDIR_RECEIVE.find(inst.type);
        if (bd != BIDIR_RECEIVE.end() && pin == "O")
            return in(bd->second.c_str());
    }

    // A delay line carries its input to its output unchanged.  It changes
    // WHEN a value arrives, never what it is, so for any boolean statement
    // about the design it is a wire -- and that is exactly the extent of what
    // checking a design containing one establishes.  DELAY_SRC says which of
    // the two inputs is being delayed.
    if (inst.type == "IDELAYE2" && pin == "DATAOUT")
        return in(param_str(inst, "DELAY_SRC", "IDATAIN") == "DATAIN" ? "DATAIN" : "IDATAIN");

    if (inst.type == "CARRY4") {
        // Four stages of the same cell.  The carry into the chain is CYINIT
        // at the bottom of a column and CI above it; the unused one is tied
        // low, which is why the primitive can simply take both.  Then per
        // stage: the sum bit is the propagate signal against the incoming
        // carry, and the outgoing carry either propagates it or takes the
        // generate input DI -- the same mux the fabric's own column model
        // makes out of O6, CI and the O5/X choice.
        const Pin *sp = inst.find_pin("S"), *dp = inst.find_pin("DI");
        Lit carry = net_.mk_or(in("CI"), in("CYINIT"));
        for (int i = 0; i < 4; i++) {
            Lit s = sp ? eval_bit(sp->conn, i, depth + 1) : LIT_FALSE;
            Lit di = dp ? eval_bit(dp->conn, i, depth + 1) : LIT_FALSE;
            Lit sum = net_.mk_xor(s, carry);
            Lit co = net_.mk_or(net_.mk_and(s, carry), net_.mk_and(negate(s), di));
            if (pin == bit_name("O", i))
                return sum;
            if (pin == bit_name("CO", i))
                return co;
            carry = co;
        }
        return LIT_FALSE;
    }

    if (RAM_PORTS.count(inst.type) && pin.rfind("DO", 0) == 0) {
        // A cut point.  What a memory reads is not derived here at all: it is
        // a free symbol, and the paired memory on the other side is given the
        // SAME one.  Everything downstream then references identical
        // variables and cancels, so the proof reduces to the boundary --
        // which is the thing that can actually be checked, and the thing that
        // needs no agreement about what anything inside is called.
        std::string base = pin;
        int d = 0;
        auto br = pin.find('[');
        if (br != std::string::npos) { d = atoi(pin.c_str() + br + 1); base = pin.substr(0, br); }
        std::string key = mem_cut_name(inst.name, base, d);
        auto c = mem_cut_.find(key);
        return net_.input(c == mem_cut_.end() ? key : c->second);
    }

    if (is_xcol(inst.type)) {
        std::vector<Lit> ins;
        for (int i = 1; i <= 6; i++) {
            const Pin *p = inst.find_pin("A" + std::to_string(i));
            ins.push_back(p ? eval_expr(p->conn, depth + 1) : LIT_TRUE);   // unused inputs pull up
        }
        uint64_t init = parse_init(param_str(inst, "INIT", "0"));
        bool param_ram = param_str(inst, "RAM", "0") != "0";
        if (pin == "O6" || pin == "O5") {
            int width = (pin == "O6") ? 6 : 5;
            std::vector<Lit> sel(ins.begin(), ins.begin() + width);
            (void)sel;
            if (!param_ram) {
                // A fixed LUT: the contents are a constant, so the read is a
                // truth table and no state is involved.
                return (pin == "O6") ? net_.mk_lut(ins, init)
                                     : net_.mk_lut(sel, init & 0xffffffffull);
            }
            if (!memory_as_state_) {
                // A cut point, the same as the synthesis side's RAM: what a
                // writable column reads is a free symbol, and the column it is
                // paired with gets the same one.  O6 and O5 are the two halves
                // of the one memory, so they are two different bits of it.
                // Which data bit this read is.  A 64-deep column has one --
                // O6, the whole address -- and matches a RAM64M port's single
                // DOx.  A 32-deep column has two halves, and they are the two
                // bits a RAM32M port calls DOx[1] and DOx[0].
                bool small = param_str(inst, "RAM32", "0") != "0";
                int d = small ? (pin == "O6" ? 1 : 0) : 0;
                std::string key = mem_cut_name(inst.name, "DO", d);
                auto c = mem_cut_.find(key);
                return net_.input(c == mem_cut_.end() ? key : c->second);
            }
            // A writable column: the contents are STATE, so the read is a mux
            // over the stored bits rather than over a constant. Built as a
            // balanced tree from the low address bit up, which keeps it the
            // same shape as mk_lut would have produced.
            std::string anchor = n_for_output(inst, "O6");
            std::vector<Lit> level;
            level.reserve(1u << width);
            for (int b = 0; b < (1 << width); b++)
                level.push_back(sym_state(mem_state_name(anchor, b)));
            for (int i = 0; i < width; i++) {
                std::vector<Lit> next;
                next.reserve(level.size() / 2);
                for (size_t j = 0; j + 1 < level.size(); j += 2)
                    next.push_back(net_.mk_or(net_.mk_and(negate(sel[i]), level[j]),
                                              net_.mk_and(sel[i], level[j + 1])));
                level.swap(next);
            }
            return level.front();
        }
        // The carry cell a column contributes to the chain.  O6 is the
        // propagate signal: it either passes the incoming carry along or
        // replaces it with the generate input, which is O5 or the X bypass
        // according to CY0.  XOR is the sum, and has no pin of its own -- it
        // reaches the world only through the flip-flop or the output mux.
        if (pin == "CO" || pin == "XOR") {
            Lit o6 = eval_cell_output(inst, "O6", depth);
            Lit ci = in("CI");
            if (pin == "XOR")
                return net_.mk_xor(o6, ci);
            Lit di = param_str(inst, "CY0", "X") == "O5" ? eval_cell_output(inst, "O5", depth)
                                                         : in("X");
            return net_.mk_or(net_.mk_and(o6, ci), net_.mk_and(negate(o6), di));
        }
        if (pin == "Q")
            return sym_state(n_for_output(inst, "Q"));
        if (pin == "MUX") {
            std::string sel = param_str(inst, "OUTMUX", "none");
            if (sel == "5Q") return sym_state(n_for_output(inst, "MUX"));
            if (sel == "O6") return eval_cell_output(inst, "O6", depth);
            if (sel == "O5") return eval_cell_output(inst, "O5", depth);
            if (sel == "XOR") return eval_cell_output(inst, "XOR", depth);
            if (sel == "CY") return eval_cell_output(inst, "CO", depth);
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
// A stored bit's state symbol.  Anchored on the column's read output so the
// name exists on both sides of a comparison: match the columns and the memory
// bits match with them, exactly as matching a register matches its state.
// A memory's read output, as a primary input: "ramname:pinname[bit]".
// One predictable pattern, used by both sides, so that pairing two memories is
// nothing more than agreeing on the ram name -- everything inside stays
// anonymous, which is the point.
std::string Cones::mem_cut_name(const std::string &ram, const std::string &pin, int bit)
{
    return ram + ":" + pin + "[" + std::to_string(bit) + "]";
}

// The net a RAM port reads its TOP data bit onto: DOx for a one-bit port,
// DOx[1] for a two-bit one.  That is the bit a fabric column reads on O6, and
// anchoring both sides there is what lets one rename match a whole memory.
std::string Cones::ram_anchor(const Instance &inst, const std::string &pin, int width) const
{
    const Pin *p = inst.find_pin(pin);
    if (!p) return {};
    if (width == 1)
        return p->conn.kind == Expr::Kind::Id ? p->conn.name
             : p->conn.kind == Expr::Kind::BitSel ? bit_name(p->conn.name, p->conn.index)
             : std::string();
    return net_of_bit(p->conn, 1);
}

std::string Cones::mem_state_name(const std::string &anchor, int bit)
{
    return anchor + "$m" + std::to_string(bit);
}

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
    // A stored bit holds its value unless this cycle writes THIS bit: the
    // write enable is asserted and the write address selects it.  That is the
    // whole of a distributed RAM's state behaviour, and it is why the bits
    // belong in states() rather than in some parallel mechanism.
    auto mem = mem_by_state_.find(state_name);
    if (mem != mem_by_state_.end()) {
        const Instance &col = *mem->second;
        int bit = mem_bit_.at(state_name);
        auto pin_lit = [&](const char *p, Lit dflt) {
            const Pin *q = col.find_pin(p);
            return q ? eval_expr(q->conn, 0) : dflt;
        };
        Lit held = sym_state(state_name);
        Lit we = pin_lit("WE", LIT_FALSE);
        bool small = param_str(col, "RAM32", "0") != "0";

        // The address this bit answers to. 32-deep columns are two memories
        // sharing five address bits, so bit 5 picks the half rather than
        // forming part of the address, and each half takes its own data.
        int addr_bits = small ? 5 : 6;
        Lit sel = LIT_TRUE;
        for (int i = 0; i < addr_bits; i++) {
            const Pin *w = col.find_pin("WA");
            Lit a = w ? eval_bit(w->conn, i, 0) : LIT_FALSE;
            sel = net_.mk_and(sel, ((bit >> i) & 1) ? a : negate(a));
        }
        Lit di = pin_lit(small && (bit & 32) ? "DI2" : "DI", LIT_FALSE);
        Lit write = net_.mk_and(we, sel);
        return net_.mk_or(net_.mk_and(write, di), net_.mk_and(negate(write), held));
    }

    auto gram = mem_by_state_.find(state_name);
    if (gram != mem_by_state_.end() && RAM_PORTS.count(gram->second->type)) {
        // The synthesis side of the same statement.  Every port writes at
        // port D's address -- one write address for the whole primitive, which
        // is the slice's single write address seen from the other side.
        const Instance &ram = *gram->second;
        int width = RAM_PORTS.at(ram.type);
        int bit = mem_bit_.at(state_name);
        int abits = (width == 1) ? 6 : 5;
        int addr = (width == 1) ? bit : (bit & 31);
        int d = (width == 1) ? 0 : (bit >> 5);
        char port = mem_port_.count(state_name) ? mem_port_.at(state_name) : 'A';
        Lit held = sym_state(state_name);
        const Pin *wp = ram.find_pin("ADDRD");
        Lit sel = LIT_TRUE;
        for (int i = 0; i < abits; i++) {
            Lit a = wp ? eval_bit(wp->conn, i, 0) : LIT_FALSE;
            sel = net_.mk_and(sel, ((addr >> i) & 1) ? a : negate(a));
        }
        const Pin *wep = ram.find_pin("WE");
        Lit we = wep ? eval_expr(wep->conn, 0) : LIT_FALSE;
        const Pin *dip = ram.find_pin(std::string("DI") + port);
        Lit di = dip ? eval_bit(dip->conn, d, 0) : LIT_FALSE;
        Lit write = net_.mk_and(we, sel);
        return net_.mk_or(net_.mk_and(write, di), net_.mk_and(negate(write), held));
    }

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
        else if (src == "XOR") d = eval_cell_output(ff, "XOR", 0);
        else if (src == "CY") d = eval_cell_output(ff, "CO", 0);
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
