// CARRY4: a 32-bit accumulator is four carry chains deep, so the extraction
// has to follow COUT -> CIN from one slice to the next, and pick up both the
// XOR sum outputs and the CY carry outputs on the way.
module top (input clk, input [3:0] sw, output [11:0] led);
    reg [31:0] ctr = 0;
    always @(posedge clk) ctr <= ctr + {28'b0, sw};
    assign led = ctr[31:20];
endmodule
