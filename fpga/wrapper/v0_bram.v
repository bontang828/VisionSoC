// v0_bram.v -- BRAM-backed shadow of the v0 mask register for MaskUnitFpga.
//
// Replaces the per-hardware-row FF replication of v0Vec
// (`t1/src/mask/MaskUnit.scala:194-198`) with a small BRAM that holds
// timeMultiplexBatch (128) rows of vLen-bit v0 state. MaskUnitFpga
// maintains a single 1024-bit "active-row cache" register and refills
// it from this BRAM on hardware-row transitions; reads from the cache
// are combinational (0-cycle), keeping the mask-execute path on its
// existing single-cycle contract.
//
// Storage: 128 rows * 1024 bits = 131,072 bits ~ 4 BRAM36 by capacity.
// Wired as xpm_memory_sdpram (simple dual-port: write port A, read
// port B) so the lane v0Update write (per-byte strobe via wea) and the
// active-row refill read run on independent ports.
//
// Following the uram_scratchpad.v precedent for XPM wrapping; here we
// use MEMORY_PRIMITIVE = "block" (BRAM) rather than "ultra" (URAM).
//
// Reset: WRITE_PROTECT plus MEMORY_INIT_PARAM = "0" guarantees the
// BRAM contents start at all-zero, matching the RegInit(0) semantics
// of the original v0Vec.

`timescale 1ns / 1ps

module v0_bram #(
    parameter integer DATA_WIDTH = 1024,                // vLen bits
    parameter integer ADDR_WIDTH = 7,                   // log2(timeMultiplexBatch=128)
    parameter integer MEM_DEPTH  = 128
) (
    input  wire                       clk,

    // Write port A (lane v0Update path)
    input  wire                       wr_en,
    input  wire [ADDR_WIDTH-1:0]      wr_addr,
    input  wire [DATA_WIDTH-1:0]      wr_data,
    input  wire [DATA_WIDTH/8-1:0]    wr_strb,           // byte-write enable

    // Read port B (active-row refill path)
    input  wire                       rd_en,
    input  wire [ADDR_WIDTH-1:0]      rd_addr,
    output wire [DATA_WIDTH-1:0]      rd_data            // READ_LATENCY_B=1 -> available next cycle
);
    xpm_memory_sdpram #(
        .ADDR_WIDTH_A           (ADDR_WIDTH),
        .ADDR_WIDTH_B           (ADDR_WIDTH),
        .AUTO_SLEEP_TIME        (0),
        .BYTE_WRITE_WIDTH_A     (8),                     // per-byte write enable
        .CASCADE_HEIGHT         (0),                     // 0 = let Vivado pick
        .CLOCKING_MODE          ("common_clock"),
        .ECC_MODE               ("no_ecc"),
        .MEMORY_INIT_FILE       ("none"),
        .MEMORY_INIT_PARAM      ("0"),                   // zero-init for v0
        .MEMORY_OPTIMIZATION    ("true"),
        .MEMORY_PRIMITIVE       ("block"),               // BRAM (not URAM/distributed)
        .MEMORY_SIZE            (MEM_DEPTH * DATA_WIDTH),
        .MESSAGE_CONTROL        (0),
        .READ_DATA_WIDTH_B      (DATA_WIDTH),
        .READ_LATENCY_B         (1),                     // 1-cycle BRAM read
        .READ_RESET_VALUE_B     ("0"),
        .RST_MODE_A             ("SYNC"),
        .RST_MODE_B             ("SYNC"),
        .SIM_ASSERT_CHK         (0),
        .USE_EMBEDDED_CONSTRAINT(0),
        .USE_MEM_INIT           (1),                     // initialise from MEMORY_INIT_PARAM
        .USE_MEM_INIT_MMI       (0),
        .WAKEUP_TIME            ("disable_sleep"),
        .WRITE_DATA_WIDTH_A     (DATA_WIDTH),
        .WRITE_MODE_B           ("no_change"),           // read port doesn't write, no hazard
        .WRITE_PROTECT          (1)
    ) u_v0_bram (
        // Port A: write only (xpm_memory_sdpram exposes no `douta` / `rsta`
        // for the write-only side; do NOT connect them or Vivado synth
        // errors with [Synth 8-11365] "named port connection ... does not
        // exist". Verified against Vivado 2025.2 xpm_memory.sv:9028.)
        .clka           (clk),
        .ena            (wr_en),
        .wea            (wr_strb),
        .addra          (wr_addr),
        .dina           (wr_data),
        // Port B: read only (no `web`/`dinb` ports exist on the read side
        // of xpm_memory_sdpram).
        .clkb           (clk),
        .enb            (rd_en),
        .addrb          (rd_addr),
        .doutb          (rd_data),
        // Port B reset (only port B has a reset on sdpram). Driven low at
        // the wrapper boundary; MaskUnitFpga gates consumers via the
        // active-row cache-valid flag.
        .rstb           (1'b0),
        // Misc
        .sleep          (1'b0),
        .regceb         (1'b1),
        .injectdbiterra (1'b0),
        .injectsbiterra (1'b0),
        .dbiterrb       (),
        .sbiterrb       ()
    );

endmodule
