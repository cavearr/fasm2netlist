// DSP48E1: a registered 18x18 multiply, which synthesis maps onto the hard
// multiplier rather than fabric. Exercises the site-pin wiring and the
// register-enable/inversion attributes, which are stored complemented.
module top (input clk, input [3:0] sw, output [11:0] led);
    reg [17:0] a = 0;
    reg [17:0] b = 0;
    always @(posedge clk) begin
        a <= a + {14'b0, sw};
        b <= b + 18'd3;
    end
    reg [35:0] p;
    always @(posedge clk) p <= a * b;
    assign led = p[35:24];
endmodule
