// Tiny AXIS-snoop beat counter exposed via AXI-Lite (read-only at offset 0).
//
// Purpose: diagnose whether mipi_csi2_rx_subsystem produces AXIS beats at
// its video_out. Snoops the AXIS handshake (TVALID && TREADY) and
// increments a 32-bit counter. PS reads the counter via AXI-Lite.
//
// Map:
//   0x00 : beat_count   (32-bit, RO; reset on AXI-Lite write to this offset)
//   0x04 : last_tdata_lo (32-bit, last TDATA captured, lower 32 bits)
//   0x08 : last_tdata_hi (32-bit, last TDATA captured, upper 16 bits if TDATA is 48-bit)
//   0x0C : tlast_count  (32-bit, count of beats with TLAST asserted = frame ends)
//
// All registers operate in the AXIS clock domain; AXI-Lite read crosses
// asynchronously assuming counters are large enough to be stable across
// the brief CDC window (no rigorous CDC because we only read counters
// when bus is quiescent - diagnostic use only).

`default_nettype none

module axis_beat_counter #(
    parameter int TDATA_W = 48
) (
    // AXIS monitor (snoop, no upstream throttle)
    input  wire                   axis_aclk,
    input  wire                   axis_aresetn,
    input  wire                   axis_tvalid,
    input  wire                   axis_tready,
    input  wire                   axis_tlast,
    input  wire [TDATA_W-1:0]     axis_tdata,

    // AXI-Lite slave (32-bit, read-mostly)
    input  wire                   s_axi_aclk,
    input  wire                   s_axi_aresetn,

    input  wire [11:0]            s_axi_araddr,
    input  wire [2:0]             s_axi_arprot,
    input  wire                   s_axi_arvalid,
    output reg                    s_axi_arready,
    output reg  [31:0]            s_axi_rdata,
    output reg  [1:0]             s_axi_rresp,
    output reg                    s_axi_rvalid,
    input  wire                   s_axi_rready,

    input  wire [11:0]            s_axi_awaddr,
    input  wire [2:0]             s_axi_awprot,
    input  wire                   s_axi_awvalid,
    output reg                    s_axi_awready,
    input  wire [31:0]            s_axi_wdata,
    input  wire [3:0]             s_axi_wstrb,
    input  wire                   s_axi_wvalid,
    output reg                    s_axi_wready,
    output reg  [1:0]             s_axi_bresp,
    output reg                    s_axi_bvalid,
    input  wire                   s_axi_bready
);

// ----------------------------------------------------------------------
// AXIS snoop side - counters increment on TVALID && TREADY edges
// ----------------------------------------------------------------------
reg [31:0] beat_count_r;
reg [31:0] tlast_count_r;
reg [TDATA_W-1:0] last_tdata_r;

wire clear_pulse;  // crossed from AXI domain

always_ff @(posedge axis_aclk) begin
    if (!axis_aresetn) begin
        beat_count_r  <= 32'd0;
        tlast_count_r <= 32'd0;
        last_tdata_r  <= '0;
    end else if (clear_pulse) begin
        beat_count_r  <= 32'd0;
        tlast_count_r <= 32'd0;
    end else if (axis_tvalid && axis_tready) begin
        beat_count_r <= beat_count_r + 1'b1;
        last_tdata_r <= axis_tdata;
        if (axis_tlast)
            tlast_count_r <= tlast_count_r + 1'b1;
    end
end

// ----------------------------------------------------------------------
// AXI-Lite slave - single-cycle response, no pipeline
// ----------------------------------------------------------------------
reg clear_req;  // synced into axis domain below
reg clear_req_d, clear_req_dd;

always_ff @(posedge axis_aclk) begin
    clear_req_d  <= clear_req;
    clear_req_dd <= clear_req_d;
end
assign clear_pulse = clear_req_d & ~clear_req_dd;

// Read channel
always_ff @(posedge s_axi_aclk) begin
    if (!s_axi_aresetn) begin
        s_axi_arready <= 1'b0;
        s_axi_rvalid  <= 1'b0;
        s_axi_rresp   <= 2'b00;
        s_axi_rdata   <= 32'd0;
    end else begin
        if (s_axi_arvalid && !s_axi_arready) begin
            s_axi_arready <= 1'b1;
            s_axi_rvalid  <= 1'b1;
            s_axi_rresp   <= 2'b00;
            case (s_axi_araddr[5:2])
                4'h0:    s_axi_rdata <= beat_count_r;
                4'h1:    s_axi_rdata <= last_tdata_r[31:0];
                4'h2:    s_axi_rdata <= (TDATA_W > 32) ? last_tdata_r[TDATA_W-1:32] : 32'd0;
                4'h3:    s_axi_rdata <= tlast_count_r;
                default: s_axi_rdata <= 32'hDEADBEEF;
            endcase
        end else if (s_axi_rvalid && s_axi_rready) begin
            s_axi_rvalid  <= 1'b0;
            s_axi_arready <= 1'b0;
        end else if (!s_axi_arvalid) begin
            s_axi_arready <= 1'b0;
        end
    end
end

// Write channel - only one register: writing anything to offset 0x00 clears
reg awready_r, wready_r, bvalid_r;
always_ff @(posedge s_axi_aclk) begin
    if (!s_axi_aresetn) begin
        s_axi_awready <= 1'b0;
        s_axi_wready  <= 1'b0;
        s_axi_bvalid  <= 1'b0;
        s_axi_bresp   <= 2'b00;
        clear_req     <= 1'b0;
    end else begin
        // Generic accept: one-cycle awready and wready
        s_axi_awready <= s_axi_awvalid && !s_axi_awready && !s_axi_bvalid;
        s_axi_wready  <= s_axi_wvalid  && !s_axi_wready  && !s_axi_bvalid;
        if (s_axi_awready && s_axi_wready) begin
            // Toggle clear_req - synced to axis_aclk for pulse generation
            if (s_axi_awaddr[5:2] == 4'h0) begin
                clear_req <= ~clear_req;
            end
            s_axi_bvalid <= 1'b1;
            s_axi_bresp  <= 2'b00;
        end else if (s_axi_bvalid && s_axi_bready) begin
            s_axi_bvalid <= 1'b0;
        end
    end
end

endmodule

`default_nettype wire
