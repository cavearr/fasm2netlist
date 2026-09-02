// Structural Verilog reader checks.  The embedded netlist is the shape
// fasm2netlist itself emits -- escaped instance names, an INIT parameter
// override, named pin connections, constants and an alias assign -- because
// that is the input the LVS matcher has to read back.
//
// Pass a path as argv[1] to parse a real netlist instead and print a summary.
#include "lvs/netlist.hpp"

#include <iostream>
#include <string>

using namespace lvs;

namespace {

int failures = 0;

void check(bool ok, const std::string &what)
{
    std::cout << (ok ? "  ok   " : "  FAIL ") << what << '\n';
    if (!ok)
        failures++;
}

const char *SAMPLE = R"(
// a comment
module johnson_lvs(input clk, input rst, output [1:0] led);
  wire n_a, n_b;
  wire [3:0] bus;
  LUT6_2 #(.INIT(64'hf0f0f0f0f0f0f0f0)) \lut_CLBLM_R_X31Y134_SLICE_X46Y134_B (
      .O6(n_a), .O5(n_b), .I0(rst), .I1(1'b0), .I2(bus[2]), .I3(n_b), .I4(n_a), .I5(clk));
  FDRE #(.INIT(1'b0)) \ff_CLBLM_R_X31Y134_SLICE_X46Y134_A (
      .C(clk), .CE(n_a), .D(n_b), .R(rst), .Q(led[0]));
  /* block
     comment */
  assign led[1] = n_a;
endmodule
)";

} // namespace

int main(int argc, char **argv)
{
    if (argc > 1) {
        Netlist nl = parse_verilog_file(argv[1]);
        for (const auto &m : nl.modules)
            std::cout << "module " << m.name << ": " << m.ports.size() << " ports, " << m.nets.size()
                      << " nets, " << m.instances.size() << " instances, " << m.assigns.size()
                      << " assigns\n";
        return 0;
    }

    Netlist nl = parse_verilog_string(SAMPLE, "<sample>");
    check(nl.modules.size() == 1, "one module parsed");
    const Module *m = nl.find("johnson_lvs");
    check(m != nullptr, "module found by name");
    if (!m)
        return 1;

    check(m->ports.size() == 3, "three ports (got " + std::to_string(m->ports.size()) + ")");
    check(m->ports[2].name == "led" && !m->ports[2].range.scalar && m->ports[2].range.width() == 2,
          "output led is [1:0]");
    check(m->ports[0].dir == PortDecl::Dir::Input, "clk is an input");

    check(m->instances.size() == 2, "two instances (got " + std::to_string(m->instances.size()) + ")");
    const Instance &lut = m->instances[0];
    // the escaped \name form must survive with the backslash and the
    // terminating space both removed
    check(lut.name == "lut_CLBLM_R_X31Y134_SLICE_X46Y134_B", "escaped instance name is unescaped: " + lut.name);
    check(lut.type == "LUT6_2", "cell type");
    auto init = lut.param("INIT");
    check(init && *init == "64'hf0f0f0f0f0f0f0f0", "INIT parameter kept verbatim");
    check(lut.pins.size() == 8, "eight pins (got " + std::to_string(lut.pins.size()) + ")");
    const Pin *i1 = lut.find_pin("I1");
    check(i1 && i1->conn.kind == Expr::Kind::Const && i1->conn.const_text == "1'b0", "constant pin connection");
    const Pin *i2 = lut.find_pin("I2");
    check(i2 && i2->conn.kind == Expr::Kind::BitSel && i2->conn.name == "bus" && i2->conn.index == 2,
          "bit-select pin connection");

    check(m->instances[1].type == "FDRE", "second instance is the FF");
    check(m->assigns.size() == 1, "one continuous assign");
    check(m->assigns[0].lhs.kind == Expr::Kind::BitSel && m->assigns[0].lhs.index == 1, "assign lhs is led[1]");

    bool threw = false;
    try {
        parse_verilog_string("module broken(; endmodule", "<bad>");
    } catch (const std::exception &) {
        threw = true;
    }
    check(threw, "a syntax error is reported, not silently ignored");

    return failures == 0 ? 0 : 1;
}
