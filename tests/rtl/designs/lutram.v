// Distributed RAM: a 32-deep, 2-bit-wide memory with an asynchronous read
// port is exactly the shape that maps onto a SLICEM's four LUTs as one
// RAM32M, which is the mode the width/depth bits have to be decoded into.
module top (input clk, input [3:0] sw, output [11:0] led);
    reg [4:0] wa = 0;
    always @(posedge clk) wa <= wa + 1'b1;
    reg [1:0] mem [0:31];
    // Uninitialised for the same reason as srl.v: nextpnr writes the
    // distributed RAM's LUT INIT as all zeros, so its contents never reach
    // the bitstream to be extracted.
    always @(posedge clk) if (sw[3]) mem[wa] <= sw[1:0];
    assign led = {10'b0, mem[{sw[2:0], wa[1]}]};
endmodule
