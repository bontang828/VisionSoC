module SRAM1R1W #(
  parameter depth,
  parameter width,
  parameter addrWidth
)(
  input                  clock,
  input                  readEnable,
  input  [addrWidth-1:0] readAddress,
  output [width-1:0]     readData,
  input                  writeEnable,
  input  [addrWidth-1:0] writeAddress,
  input  [width-1:0]     writeData
);

  reg [width-1:0] Memory [0:depth-1];
  reg [addrWidth-1:0] read_addr_reg;
  reg                  read_enable_reg;

  always @(posedge clock) begin
    if (writeEnable)
      Memory[writeAddress] <= writeData;

    if (readEnable) begin
      read_addr_reg   <= readAddress;
      read_enable_reg <= 1'b1;
    end else begin
      read_enable_reg <= 1'b0;
    end
  end

  assign readData = read_enable_reg ? Memory[read_addr_reg] : {width{1'bx}};

endmodule
