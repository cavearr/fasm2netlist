// Minimal models for exactly the primitives these two netlists use.
// yosys's own cells_sim.v is richer than a general SV frontend will take.
// The IO parameters are carried by the source netlist and are irrelevant to
// the logic; they have to be declared or the defparams have nowhere to land.
module IBUF #(parameter IOSTANDARD = "", parameter IBUF_LOW_PWR = "")
             (input I, output O);            assign O = I;  endmodule
module OBUF #(parameter IOSTANDARD = "", parameter DRIVE = 12, parameter SLEW = "")
             (input I, output O);            assign O = I;  endmodule
module BUFG  (input I, output O);            assign O = I;  endmodule
module IBUFDS #(parameter IOSTANDARD = "", parameter DIFF_TERM = "FALSE",
                parameter IBUF_LOW_PWR = "")
               (input I, input IB, output O); assign O = I;  endmodule

module FDRE #(parameter INIT = 1'b0) (output reg Q = INIT, input C, input CE, input D, input R);
  always @(posedge C) if (R) Q <= 1'b0; else if (CE) Q <= D;
endmodule

module FDSE #(parameter INIT = 1'b1) (output reg Q = INIT, input C, input CE, input D, input S);
  always @(posedge C) if (S) Q <= 1'b1; else if (CE) Q <= D;
endmodule

module LUT2 #(parameter [3:0] INIT = 4'h0) (output O, input I0, I1);
  assign O = INIT[{I1, I0}];
endmodule
module LUT3 #(parameter [7:0] INIT = 8'h0) (output O, input I0, I1, I2);
  assign O = INIT[{I2, I1, I0}];
endmodule
module LUT4 #(parameter [15:0] INIT = 16'h0) (output O, input I0, I1, I2, I3);
  assign O = INIT[{I3, I2, I1, I0}];
endmodule
module LUT5 #(parameter [31:0] INIT = 32'h0) (output O, input I0, I1, I2, I3, I4);
  assign O = INIT[{I4, I3, I2, I1, I0}];
endmodule
module LUT6 #(parameter [63:0] INIT = 64'h0) (output O, input I0, I1, I2, I3, I4, I5);
  assign O = INIT[{I5, I4, I3, I2, I1, I0}];
endmodule

module LUT6_2 #(parameter [63:0] INIT = 64'h0) (output O6, output O5, input I0, I1, I2, I3, I4, I5);
  assign O6 = INIT[{I5, I4, I3, I2, I1, I0}];
  assign O5 = INIT[{1'b0, I4, I3, I2, I1, I0}];
endmodule
