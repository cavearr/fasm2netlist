// Distributed RAM modelled as state: the properties that make it a memory.
//
// A stored bit is state, so the prover asks for its next-state function the
// same way it asks for a flip-flop's.  These are the three statements that
// have to hold for that to mean anything, each proved for EVERY address and
// every input assignment rather than for a case someone chose:
//
//   hold     with the write enable low, every bit keeps its value
//   write    with it high, the addressed bit takes the data...
//   others   ...and no other bit moves
//
// The last is the one worth having: a model that writes every bit, or the
// wrong half of a 32-deep column, passes the first two and is still wrong.
#include "lvs/cone.hpp"
#include "lvs/netlist.hpp"
#include "lvs/solver.hpp"

#include <iostream>
#include <sstream>

using namespace lvs;

namespace {

int failures = 0;

void check(bool ok, const std::string &what)
{
    std::cout << (ok ? "  ok   " : "  FAIL ") << what << '\n';
    if (!ok) failures++;
}

// One SLICEM column configured as a 64-deep memory, wired so every input is a
// primary input and therefore free for the solver to choose.
const char *kRam64 = R"(
module fabric(A1, A2, A3, A4, A5, A6, W1, W2, W3, W4, W5, W6, DI, WE, CLK, O6);
  input A1, A2, A3, A4, A5, A6, W1, W2, W3, W4, W5, W6, DI, WE, CLK;
  output O6;
  xcol #(.INIT(64'h0), .RAM(1), .RAM32(0), .FF_SRC("none"), .FF5_SRC("none"),
         .OUTMUX("none"), .CY0("X"))
    col (.A1(A1), .A2(A2), .A3(A3), .A4(A4), .A5(A5), .A6(A6), .X(1'b0),
         .CLK(CLK), .CE(1'b1), .SR(1'b0), .CI(1'b0),
         .WA({W6, W5, W4, W3, W2, W1}), .DI(DI), .DI2(1'b0), .WE(WE),
         .O6(O6), .O5(), .Q(), .MUX(), .CO());
endmodule
)";

// ...and the 32-deep mode, which is TWO memories in one column.
const char *kRam32 = R"(
module fabric(A1, A2, A3, A4, A5, A6, W1, W2, W3, W4, W5, DI, DI2, WE, CLK, O6);
  input A1, A2, A3, A4, A5, A6, W1, W2, W3, W4, W5, DI, DI2, WE, CLK;
  output O6;
  xcol #(.INIT(64'h0), .RAM(1), .RAM32(1), .FF_SRC("none"), .FF5_SRC("none"),
         .OUTMUX("none"), .CY0("X"))
    col (.A1(A1), .A2(A2), .A3(A3), .A4(A4), .A5(A5), .A6(A6), .X(1'b0),
         .CLK(CLK), .CE(1'b1), .SR(1'b0), .CI(1'b0),
         .WA({1'b0, W5, W4, W3, W2, W1}), .DI(DI), .DI2(DI2), .WE(WE),
         .O6(O6), .O5(), .Q(), .MUX(), .CO());
endmodule
)";

Netlist parse(const char *src) { return parse_verilog_string(src, "<test>"); }

Session *session = nullptr;

// UNSAT of (a != b) is "equal for every assignment".
bool prove_equal(BoolNet &net, Lit a, Lit b)
{
    return session->check(net.mk_xor(a, b)) == Result::Unsat;
}

} // namespace

int main()
{
    Solver solver = have_linked_z3() ? Solver{linked_z3_name(), Format::SmtLib2} : Solver::z3();

    {
        Netlist nl = parse(kRam64);
        BoolNet net;
        Cones c(*nl.find("fabric"), net, {}, /*memory_as_state=*/true);
        auto sess = make_session(net, solver);
        session = sess.get();
        check(c.states().size() == 64, "a 64-deep column contributes 64 state bits");

        // hold: WE is a primary input, so pin it low by asking about the
        // function under that assignment -- next == held whenever WE is 0.
        bool all_hold = true, all_write = true, others_still = true;
        for (int bit : {0, 1, 37, 63}) {
            std::string sn = "O6$m" + std::to_string(bit);
            Lit next = c.next_state(sn);
            Lit held = net.input(sn);
            Lit we = net.input("WE");
            // WE low => next == held
            all_hold &= prove_equal(net, net.mk_and(negate(we), next),
                                          net.mk_and(negate(we), held));
        }
        check(all_hold, "with the write enable low, a stored bit keeps its value");

        // write: WE high and the address selecting THIS bit => next == DI
        for (int bit : {0, 5, 63}) {
            std::string sn = "O6$m" + std::to_string(bit);
            Lit next = c.next_state(sn);
            Lit we = net.input("WE");
            Lit sel = we;
            static const char *w[] = {"W1", "W2", "W3", "W4", "W5", "W6"};
            for (int i = 0; i < 6; i++) {
                Lit a = net.input(w[i]);
                sel = net.mk_and(sel, ((bit >> i) & 1) ? a : negate(a));
            }
            all_write &= prove_equal(net, net.mk_and(sel, next),
                                          net.mk_and(sel, net.input("DI")));
        }
        check(all_write, "the addressed bit takes the write data");

        // others: writing address 0 must not move bit 5
        {
            Lit we = net.input("WE");
            Lit addr0 = we;
            static const char *w[] = {"W1", "W2", "W3", "W4", "W5", "W6"};
            for (int i = 0; i < 6; i++) addr0 = net.mk_and(addr0, negate(net.input(w[i])));
            Lit next5 = c.next_state("O6$m5");
            others_still = prove_equal(net, net.mk_and(addr0, next5),
                                            net.mk_and(addr0, net.input("O6$m5")));
        }
        check(others_still, "writing one address leaves the other bits alone");
    }

    {
        // 32-deep: bit 5 of the address picks the HALF, and each half takes
        // its own data. Writing address 0 must put DI in bit 0 and DI2 in
        // bit 32 -- the same cycle, two different memories.
        Netlist nl = parse(kRam32);
        BoolNet net;
        Cones c(*nl.find("fabric"), net, {}, /*memory_as_state=*/true);
        auto sess = make_session(net, solver);
        session = sess.get();
        Lit we = net.input("WE");
        Lit addr0 = we;
        static const char *w[] = {"W1", "W2", "W3", "W4", "W5"};
        for (int i = 0; i < 5; i++) addr0 = net.mk_and(addr0, negate(net.input(w[i])));

        bool lo = prove_equal(net, net.mk_and(addr0, c.next_state("O6$m0")),
                                   net.mk_and(addr0, net.input("DI")));
        bool hi = prove_equal(net, net.mk_and(addr0, c.next_state("O6$m32")),
                                   net.mk_and(addr0, net.input("DI2")));
        check(lo, "32-deep: the low half takes DI");
        check(hi, "32-deep: the high half takes DI2, in the same cycle");
    }

    std::cout << (failures ? "FAILED\n" : "all memory-state properties proved\n");
    return failures ? 1 : 0;
}
