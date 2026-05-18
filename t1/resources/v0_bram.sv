// v0_bram.sv -- Verilator-friendly behavioural model of v0_bram.
//
// Pair with fpga/wrapper/v0_bram.v (XPM `xpm_memory_sdpram` instance for
// Vivado synthesis). Both modules expose the same port list / parameters
// and the same observable behaviour:
//   * Synchronous (clk) write port A with per-byte strobe.
//   * Synchronous (clk) read port B with 1-cycle latency.
//   * Read does NOT observe a concurrent write to the same address
//     ("no_change" / write-first-from-port-A semantics).
//   * Reset content is all-zero (matches MEMORY_INIT_PARAM="0").
//
// XPM cells (`xpm_memory_sdpram` etc.) are Xilinx-library primitives
// that Verilator cannot resolve, so this file replaces the wrapper for
// the t1emu simulation flow. The Chisel BlackBox declaration
// (V0BramBlackBox in t1/src/mask/MaskUnitFpga.scala) ships this file
// via HasBlackBoxResource("/v0_bram.sv"); Vivado picks up the XPM
// version from fpga/wrapper/v0_bram.v via add_files in system_top.tcl.
//
// Keep this module's port/parameter signature in sync with the XPM
// wrapper -- the Chisel BlackBox only declares one shape and elaborates
// against whichever copy the downstream tool resolves.

`timescale 1ns / 1ps

module v0_bram #(
    parameter integer DATA_WIDTH = 1024,
    parameter integer ADDR_WIDTH = 7,
    parameter integer MEM_DEPTH  = 128
) (
    input  wire                       clk,

    // Write port A (lane v0Update path)
    input  wire                       wr_en,
    input  wire [ADDR_WIDTH-1:0]      wr_addr,
    input  wire [DATA_WIDTH-1:0]      wr_data,
    input  wire [DATA_WIDTH/8-1:0]    wr_strb,

    // Read port B (active-row refill path) -- 1-cycle latency
    input  wire                       rd_en,
    input  wire [ADDR_WIDTH-1:0]      rd_addr,
    output reg  [DATA_WIDTH-1:0]      rd_data
);

    reg [DATA_WIDTH-1:0] mem [0:MEM_DEPTH-1];

    integer init_i;
    initial begin
        for (init_i = 0; init_i < MEM_DEPTH; init_i = init_i + 1) begin
            mem[init_i] = {DATA_WIDTH{1'b0}};
        end
        rd_data = {DATA_WIDTH{1'b0}};
    end

    integer wb_i;
    always @(posedge clk) begin
        if (wr_en) begin
            for (wb_i = 0; wb_i < DATA_WIDTH/8; wb_i = wb_i + 1) begin
                if (wr_strb[wb_i]) begin
                    mem[wr_addr][wb_i*8 +: 8] <= wr_data[wb_i*8 +: 8];
                end
            end
        end
        if (rd_en) begin
            rd_data <= mem[rd_addr];
        end
    end

endmodule
