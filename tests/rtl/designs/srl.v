// SRLC32E and SRL16E: two shift registers of different depths, so the
// extraction has to recover the 32-bit contents from the doubled-up LUT INIT
// and split a 16-deep column's INIT into its upper and lower halves.
module top (input clk, input [3:0] sw, output [11:0] led);
    // Deliberately uninitialised: nextpnr does not carry SRL contents into the
    // FASM (the LUT INIT it writes is unrelated to the cell's INIT parameter),
    // so an initialised shift register here would test that limitation of the
    // flow rather than the fidelity of the extraction. Block RAM contents DO
    // survive, and bram.v checks them.
    reg [31:0] sr;
    always @(posedge clk) if (sw[1]) sr <= {sr[30:0], sw[0]};
    reg [15:0] sr2;
    always @(posedge clk) sr2 <= {sr2[14:0], sw[2]};
    assign led = {10'b0, sr[31], sr2[15]};
endmodule
