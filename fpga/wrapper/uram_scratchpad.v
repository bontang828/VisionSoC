// uram_scratchpad.v -- 512 KB single-port URAM-backed scratchpad.
//
// Drop-in replacement for the legacy `blk_mem_gen 8.4` instance that
// backed the BRAM scratchpad. Exposes a single `BRAM_PORTA` interface
// (xilinx.com:interface:bram:1.0) so the existing `axi_bram_ctrl 4.1`
// instance drives it without any change to the BD topology.
//
// Storage: 32768 entries * 128 bits = 512 KB, placed in 16 URAM288
// blocks (2-wide * 8-deep cascade on XCK26). Triggered by
// `MEMORY_PRIMITIVE = "ultra"` on the underlying `xpm_memory_spram`.
//
// History note: `emb_mem_gen 1.0` would be a cleaner IP-only swap
// (it accepts CONFIG.MEMORY_PRIMITIVE = URAM as a literal keyword)
// but in Vivado 2025.2 the IP is gated against the Zynq UltraScale+
// part family with [BD 5-683]. XPM is fully supported on ZU+, so we
// use it here.
//
// Port naming + X_INTERFACE_INFO pragmas: the BRAM RTL interface
// (xilinx.com:interface:bram_rtl:1.0) has logical ports
// {EN, DOUT, DIN, WE, ADDR, CLK, RST}. We use the standard blk_mem_gen
// port-letter-'a' suffix names and tag each port so the BD's
// Module-Reference inferencer bundles them into `BRAM_PORTA`.

`timescale 1ns / 1ps

module uram_scratchpad #(
    parameter integer DATA_WIDTH = 128,
    parameter integer ADDR_WIDTH = 19,   // byte addr width: log2(32768 * 16) = 19
    parameter integer MEM_DEPTH  = 32768
) (
    (* X_INTERFACE_INFO = "xilinx.com:interface:bram:1.0 BRAM_PORTA CLK" *)
    (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME BRAM_PORTA, MEM_ECC NONE, MEM_SIZE 524288, MEM_WIDTH 128, READ_LATENCY 2, READ_WRITE_MODE READ_FIRST" *)
    input  wire                       clka,

    (* X_INTERFACE_INFO = "xilinx.com:interface:bram:1.0 BRAM_PORTA RST" *)
    input  wire                       rsta,

    (* X_INTERFACE_INFO = "xilinx.com:interface:bram:1.0 BRAM_PORTA EN" *)
    input  wire                       ena,

    (* X_INTERFACE_INFO = "xilinx.com:interface:bram:1.0 BRAM_PORTA WE" *)
    input  wire [DATA_WIDTH/8-1:0]    wea,

    (* X_INTERFACE_INFO = "xilinx.com:interface:bram:1.0 BRAM_PORTA ADDR" *)
    input  wire [ADDR_WIDTH-1:0]      addra,

    (* X_INTERFACE_INFO = "xilinx.com:interface:bram:1.0 BRAM_PORTA DIN" *)
    input  wire [DATA_WIDTH-1:0]      dina,

    (* X_INTERFACE_INFO = "xilinx.com:interface:bram:1.0 BRAM_PORTA DOUT" *)
    output wire [DATA_WIDTH-1:0]      douta
);
    // axi_bram_ctrl drives addra as a byte address; drop the byte-LSBs
    // (always zero for 128-bit accesses) before indexing the URAM array.
    // Widths hardcoded to 15-bit word addr / bit 4 LSB so the BD IP-Flow
    // HDL parser resolves them at elaboration (it does not chase
    // localparam -> $clog2 chains for port widths in module-reference
    // contexts).
    wire [14:0] word_addr = addra[18:4];

    xpm_memory_spram #(
        .ADDR_WIDTH_A           (15),
        .AUTO_SLEEP_TIME        (0),
        .BYTE_WRITE_WIDTH_A     (8),
        .CASCADE_HEIGHT         (0),                 // 0 = let Vivado pick
        .ECC_MODE               ("no_ecc"),
        .MEMORY_INIT_FILE       ("none"),
        .MEMORY_INIT_PARAM      ("0"),
        .MEMORY_OPTIMIZATION    ("true"),
        .MEMORY_PRIMITIVE       ("ultra"),
        .MEMORY_SIZE            (MEM_DEPTH * DATA_WIDTH),
        .MESSAGE_CONTROL        (0),
        .READ_DATA_WIDTH_A      (DATA_WIDTH),
        .READ_LATENCY_A         (2),
        .READ_RESET_VALUE_A     ("0"),
        .RST_MODE_A             ("SYNC"),
        .SIM_ASSERT_CHK         (0),
        .USE_MEM_INIT           (0),
        .USE_MEM_INIT_MMI       (0),
        .WAKEUP_TIME            ("disable_sleep"),
        .WRITE_DATA_WIDTH_A     (DATA_WIDTH),
        .WRITE_MODE_A           ("read_first"),
        .WRITE_PROTECT          (1)
    ) u_uram (
        .clka           (clka),
        .ena            (ena),
        .wea            (wea),
        .addra          (word_addr),
        .dina           (dina),
        .douta          (douta),
        .rsta           (rsta),
        .sleep          (1'b0),
        .injectdbiterra (1'b0),
        .injectsbiterra (1'b0),
        .dbiterra       (),
        .sbiterra       (),
        .regcea         (1'b1)
    );

endmodule
