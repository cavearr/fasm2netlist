// Block RAM: 1K x 12 with a synchronous read port is too large for
// distributed RAM, so it lands in a RAMB18E1 -- config, port widths and the
// full INIT_xx contents all have to come back out of the bitstream.
module top (input clk, input [3:0] sw, output [11:0] led);
    reg [9:0] addr = 0;
    always @(posedge clk) addr <= addr + 1'b1;
    reg [11:0] mem [0:1023];
    reg [11:0] dout;
    // Initialised contents, so the extraction has something to get WRONG:
    // these bytes have to survive synthesis, place-and-route, the bitstream
    // and the read-back as INIT_xx rows of the block RAM.
    integer i;
    initial for (i = 0; i < 1024; i = i + 1) mem[i] = i[11:0] ^ 12'ha5c;
    always @(posedge clk) begin
        if (sw[3]) mem[addr] <= {8'b0, sw};
        dout <= mem[{sw[2:0], addr[6:0]}];
    end
    assign led = dout;
endmodule
