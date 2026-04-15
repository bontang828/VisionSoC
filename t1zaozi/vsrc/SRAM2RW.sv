module SRAM2RW #(
  parameter depth,
  parameter width,
  parameter addrWidth
)(
  input                  clock0,
  input                  enable0,
  input                  isWrite0,
  input  [addrWidth-1:0] address0,
  input  [width-1:0]     writeData0,
  output [width-1:0]     readData0,
  input                  clock1,
  input                  enable1,
  input                  isWrite1,
  input  [addrWidth-1:0] address1,
  input  [width-1:0]     writeData1,
  output [width-1:0]     readData1
);

  reg [width-1:0] Memory [0:depth-1];
  reg [addrWidth-1:0] addr_reg0;
  reg [addrWidth-1:0] addr_reg1;
  reg                  enable_reg0;
  reg                  enable_reg1;

  always @(posedge clock0) begin
    if (enable0) begin
      if (isWrite0)
        Memory[address0] <= writeData0;
      addr_reg0   <= address0;
      enable_reg0 <= 1'b1;
    end else begin
      enable_reg0 <= 1'b0;
    end
  end

  always @(posedge clock1) begin
    if (enable1) begin
      if (isWrite1)
        Memory[address1] <= writeData1;
      addr_reg1   <= address1;
      enable_reg1 <= 1'b1;
    end else begin
      enable_reg1 <= 1'b0;
    end
  end

  assign readData0 = enable_reg0 ? Memory[addr_reg0] : {width{1'bx}};
  assign readData1 = enable_reg1 ? Memory[addr_reg1] : {width{1'bx}};

endmodule
