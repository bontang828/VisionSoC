// vrf_bank_bram.v -- XPM block-RAM backing for a SharedVRF bank.
//
// True dual-port, byte-masked RAM forced to BRAM via MEMORY_PRIMITIVE="block".
// Replaces the chisel-inferred `SRAM.masked` bank ONLY for configs elaborated
// with --bankBramPrimitive true (the dLen / bank-count report sweep). It exists
// because the large sweep config's bank is 8192 x 128 = 1,048,576 bits, which
// exceeds Vivado's 1,000,000-bit limit for an *inferred* behavioural memory
// ([Synth 8-4556]); an XPM primitive has no such limit and also guarantees BRAM
// (not URAM/distributed) for every bank in the sweep.
//
// Two read/write ports (one per lane), each: synchronous, per-byte write strobe
// (we==0 => read), 1-cycle read latency -- matching the chisel SRAM.masked RW
// port semantics this replaces. Following the fpga/wrapper/v0_bram.v precedent
// (xpm_memory_sdpram); here xpm_memory_tdpram for the dual read/write ports.
//
// Sim note: the --bankBramPrimitive configs are synthesis-only (not in the
// t1emu TestBench toml), so no Verilator behavioural twin is shipped; the Chisel
// BlackBox (VrfBankBram in SharedVRF.scala) is resource-less and Vivado resolves
// this module via add_files in system_top.tcl.

`timescale 1ns / 1ps

module vrf_bank_bram #(
    parameter integer DATA_WIDTH = 128,
    parameter integer ADDR_WIDTH = 13,
    parameter integer MEM_DEPTH  = 8192
) (
    input  wire                       clk,

    // RW port 0 (lane 0)
    input  wire                       a_en,
    input  wire [DATA_WIDTH/8-1:0]    a_we,     // per-byte write enable; 0 => read
    input  wire [ADDR_WIDTH-1:0]      a_addr,
    input  wire [DATA_WIDTH-1:0]      a_din,
    output wire [DATA_WIDTH-1:0]      a_dout,   // READ_LATENCY_A=1

    // RW port 1 (lane 1)
    input  wire                       b_en,
    input  wire [DATA_WIDTH/8-1:0]    b_we,
    input  wire [ADDR_WIDTH-1:0]      b_addr,
    input  wire [DATA_WIDTH-1:0]      b_din,
    output wire [DATA_WIDTH-1:0]      b_dout    // READ_LATENCY_B=1
);
    xpm_memory_tdpram #(
        .ADDR_WIDTH_A           (ADDR_WIDTH),
        .ADDR_WIDTH_B           (ADDR_WIDTH),
        .AUTO_SLEEP_TIME        (0),
        .BYTE_WRITE_WIDTH_A     (8),                     // per-byte write enable
        .BYTE_WRITE_WIDTH_B     (8),
        .CASCADE_HEIGHT         (0),                     // 0 = let Vivado pick
        .CLOCKING_MODE          ("common_clock"),
        .ECC_MODE               ("no_ecc"),
        .MEMORY_INIT_FILE       ("none"),
        .MEMORY_INIT_PARAM      ("0"),                   // zero-init
        .MEMORY_OPTIMIZATION    ("true"),
        .MEMORY_PRIMITIVE       ("block"),               // BRAM (not URAM/distributed)
        .MEMORY_SIZE            (MEM_DEPTH * DATA_WIDTH),
        .MESSAGE_CONTROL        (0),
        .READ_DATA_WIDTH_A      (DATA_WIDTH),
        .READ_DATA_WIDTH_B      (DATA_WIDTH),
        .READ_LATENCY_A         (1),                     // 1-cycle BRAM read
        .READ_LATENCY_B         (1),
        .READ_RESET_VALUE_A     ("0"),
        .READ_RESET_VALUE_B     ("0"),
        .RST_MODE_A             ("SYNC"),
        .RST_MODE_B             ("SYNC"),
        .SIM_ASSERT_CHK         (0),
        .USE_EMBEDDED_CONSTRAINT(0),
        .USE_MEM_INIT           (1),
        .USE_MEM_INIT_MMI       (0),
        .WAKEUP_TIME            ("disable_sleep"),
        .WRITE_DATA_WIDTH_A     (DATA_WIDTH),
        .WRITE_DATA_WIDTH_B     (DATA_WIDTH),
        .WRITE_MODE_A           ("no_change"),
        .WRITE_MODE_B           ("no_change")
    ) u_tdpram (
        // Port A
        .clka           (clk),
        .ena            (a_en),
        .wea            (a_we),
        .addra          (a_addr),
        .dina           (a_din),
        .douta          (a_dout),
        .rsta           (1'b0),
        .regcea         (1'b1),
        // Port B
        .clkb           (clk),
        .enb            (b_en),
        .web            (b_we),
        .addrb          (b_addr),
        .dinb           (b_din),
        .doutb          (b_dout),
        .rstb           (1'b0),
        .regceb         (1'b1),
        // ECC / power (unused with no_ecc)
        .sleep          (1'b0),
        .injectdbiterra (1'b0),
        .injectsbiterra (1'b0),
        .injectdbiterrb (1'b0),
        .injectsbiterrb (1'b0),
        .dbiterra       (),
        .sbiterra       (),
        .dbiterrb       (),
        .sbiterrb       ()
    );

endmodule
