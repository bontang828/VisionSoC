# FPGA Implementation Handoff (KV260)

**Audience:** future programmer or AI agent (Codex, Claude) implementing
the FPGA-side changes needed to deploy T1 on AMD Kria KV260. This is the
**how-to-build** companion to `fyp_doc/2d_fabric_fpga_design_handoff.md`
(which is the design-rationale doc). Read the design doc first; this one
is a prescriptive task list.

This doc tells you exactly **which files to edit, which IP blocks to
add, and which build commands to run**. It does not re-explain why each
choice was made — see the design doc for that.

---

## 0. Scope and prerequisites

You are implementing two FPGA changes:

  1. **Wrapper extensions** — expose `verticalMode` and perf counters in
     the AXI Lite register space so the ARM driver can drive vertical
     kernels and measure cycles.
  2. **Streaming pipeline** — add MIPI CSI-2 RX, two `v_proc_ss` (down/
     upscale), `v_frmbuf_wr/rd`, HDMI TX, AXI IIC, and a 32 KB BRAM
     scratchpad to the existing block design.

You do **not** need to change `t1/src/T1.scala` or any Chisel source.
All required Scala fields (`T1Issue.verticalMode`) already exist (see
`t1/src/Bundles.scala:659` and `t1/src/T1.scala:566`). You only need
to re-run RTL generation to get a fresh `T1.sv` whose port list reflects
those Scala changes — see § 2.

### 0.1 Tools

  * **Vivado 2025.2** (the current build is on this version, see
    `fpga/build/.../build.log:3`).
  * **Vivado IP catalog** — must include `mipi_csi2_rx_subsystem`,
    `v_proc_ss`, `v_frmbuf_wr`, `v_frmbuf_rd`, `v_hdmi_tx_subsystem`,
    `axi_iic`, `axi_bram_ctrl`, `blk_mem_gen`. These ship with the
    standard Vivado install but you'll need their licences enabled.
  * **Nix + the project's standard build flow** for RTL gen
    (`nix develop` → `build_rtl.sh`).

### 0.2 What you will end up with

A bitstream `system_top_wrapper.bit` plus a device-tree overlay
`system_top_wrapper.dtbo` that the ARM side loads with `fpgautil` or
`xmutil loadapp`. Output goes under
`fpga/build/<config>-<timestamp>/` like the current builds.

---

## 1. Source-of-truth register map (target wrapper)

This is what the wrapper must look like after § 3. The driver
(`fyp_doc/driver_implementation_handoff.md`) keys off these offsets.

| Offset | Name             | R/W | Bits | Description |
|--------|------------------|-----|------|-------------|
| 0x00   | CTRL             | RW  | [0] W1S issue_start | Auto-clears on issue handshake |
|        |                  | RO  | [1] issue_ready     | T1 can accept |
|        |                  | RO  | [2] issue_busy      | Issue in progress, not yet accepted |
| 0x04   | INSTRUCTION      | RW  | [31:0] | RVV instruction encoding |
| 0x08   | RS1_DATA         | RW  | [31:0] | Scalar rs1 (PA for memory ops) |
| 0x0C   | RS2_DATA         | RW  | [31:0] | Scalar rs2 |
| 0x10   | VTYPE            | RW  | [31:0] | vtype CSR |
| 0x14   | VL               | RW  | [31:0] | vector length |
| 0x18   | VSTART           | RW  | [31:0] | vector start |
| 0x1C   | VCSR             | RW  | [31:0] | vxrm[2:1], vxsat[0] (standard RVV) |
| 0x20   | RD_FIFO_STS      | RO  | [3:0] count, [4] !empty |
| 0x24   | RD_POP_DATA      | RO  | [31:0] rd data; read pops FIFO |
| 0x28   | RD_POP_META      | RO  | [4:0] rdAddress, [5] isFp |
| 0x2C   | CSR_FIFO_STS     | RO  | [3:0] count, [4] !empty |
| 0x30   | CSR_POP          | RO  | [31:0] vxsat; read pops |
| 0x34   | CSR_FFLAG        | RO  | [31:0] fflag from last pop |
| 0x38   | MEM_COUNT        | RO  | [7:0] saturating count; W1C bit[0] decrements |
| 0x3C   | IRQ_EN           | RW  | [0] rd, [1] csr, [2] mem |
| 0x40   | IRQ_STATUS       | RO  | [0] rd, [1] csr, [2] mem |
| **0x44** | **VERTICAL_MODE** | **RW**  | **[0] vertical_mode** | **NEW: latched on issue handshake** |
| **0x48** | **PERF_TAG**      | **W**   | **[7:0] tag**         | **NEW: nonzero=START, 0=STOP** |
| **0x4C** | **PERF_DELTA**    | **RO**  | **[31:0] delta**      | **NEW: cycles between most recent START/STOP** |
| **0x50** | **PERF_CYCLES_LO**| **RO**  | **[31:0]**            | **NEW: low 32 of free-running pl_clk counter** |
| **0x54** | **PERF_CYCLES_HI**| **RO**  | **[31:0]**            | **NEW: high 32 of same counter** |

  * 0x00–0x40 already exist in `fpga/wrapper/t1_axi_lite_wrapper.sv`.
  * 0x44–0x54 are NEW; you will add them in § 3.
  * `ADDR_WIDTH` bumps from 7 to 8.

---

## 2. Step 1 — Regenerate T1 RTL

The current `mudkip2d128small1bram1chain2lanescale-20260424-185300`
bitstream uses `test_output/.../rtl-20260424-185036/result/T1.sv`,
which was generated **before** the `T1Issue.verticalMode` change. Its
port list still has a top-level live `verticalMode` IO. The new RTL
will instead have `issue_bits_verticalMode` as part of the issue
bundle ports.

```sh
cd /home/cbt22/code/code_fyp/VisionSoC
nix develop  # if not already in the dev shell
bash tests/run-test.sh --build-only -c mudkip2d128small1bram1chain2lanescale
# or whatever the project's standard "build_rtl" wrapper is
```

(Confirm the exact invocation against the project's existing scripts;
the new RTL output goes under
`test_output/mudkip2d128small1bram1chain2lanescale/rtl-<timestamp>/result/`.)

After RTL gen, **verify** the new T1.sv has `issue_bits_verticalMode`:

```sh
RTL=/home/cbt22/code/code_fyp/VisionSoC/test_output/mudkip2d128small1bram1chain2lanescale/rtl-<NEW_TIMESTAMP>/result
grep "issue_bits_verticalMode" $RTL/T1.sv | head -3
# Expect at least one line; the port should appear in T1's module declaration.
```

If `issue_bits_verticalMode` does not appear, there is a Scala-side
issue and you should stop and read `t1/src/Bundles.scala` and
`t1/src/T1.scala` rather than continuing.

---

## 3. Step 2 — Extend the AXI Lite wrapper

**File:** `fpga/wrapper/t1_axi_lite_wrapper.sv`

Make the following edits, preserving the existing style. The current
file is the source of truth — read it before editing.

### 3.1 Bump address width

```sv
parameter int ADDR_WIDTH = 8,  // was 7; need 0x54 to fit
```

### 3.2 Add output port for issue_bits_verticalMode

In the port list, alongside the existing `issue_bits_*` outputs:

```sv
output logic        issue_bits_verticalMode,
```

### 3.3 Add internal registers

Alongside `reg_instruction`, `reg_rs1_data`, etc.:

```sv
logic        reg_vertical_mode;

assign issue_bits_verticalMode = reg_vertical_mode;
```

### 3.4 Add to the address-decoded write path

In the existing `case (wr_addr)` block (which uses `wr_addr =
aw_addr_reg[6:2]` — when bumping ADDR_WIDTH to 8, this becomes
`wr_addr = aw_addr_reg[7:2]`):

```sv
case (wr_addr)
    // ... existing 5'h01..5'h0F entries
    6'h11: reg_vertical_mode <= w_data_reg[0];   // 0x44
    6'h12: perf_tag_w <= w_data_reg[7:0];        // 0x48
    default: ;
endcase
```

(Bit-widths of `wr_addr` follow ADDR_WIDTH; see § 3.7 for `perf_tag_w`.)

### 3.5 Add to the address-decoded read path

```sv
case (rd_addr)
    // ... existing 5'h00..5'h10 entries
    6'h11: s_axi_rdata <= {31'b0, reg_vertical_mode};  // 0x44
    6'h13: s_axi_rdata <= perf_delta;                  // 0x4C
    6'h14: s_axi_rdata <= perf_cycles[31:0];           // 0x50
    6'h15: s_axi_rdata <= perf_cycles[63:32];          // 0x54
    default: s_axi_rdata <= '0;
endcase
```

### 3.6 Add free-running cycle counter

Place near the IRQ block:

```sv
logic [63:0] perf_cycles;

always_ff @(posedge aclk or negedge aresetn) begin
    if (!aresetn) perf_cycles <= '0;
    else          perf_cycles <= perf_cycles + 64'b1;
end
```

### 3.7 Add tag-driven perf delta

Mimics the simulator's `place_counter(tag)` semantics: writing a
nonzero tag latches start, writing 0 latches the delta.

```sv
logic [7:0]  perf_tag_w;
logic        perf_tag_we;
logic [7:0]  perf_tag;
logic [63:0] perf_start;
logic [31:0] perf_delta;

assign perf_tag_we = wr_en && (wr_addr == 6'h12);  // write at 0x48

always_ff @(posedge aclk or negedge aresetn) begin
    if (!aresetn) begin
        perf_tag    <= '0;
        perf_start  <= '0;
        perf_delta  <= '0;
    end else if (perf_tag_we) begin
        perf_tag <= perf_tag_w;
        if (perf_tag == 0 && perf_tag_w != 0) begin
            // 0 -> nonzero: START
            perf_start <= perf_cycles;
        end else if (perf_tag != 0 && perf_tag_w == 0) begin
            // nonzero -> 0: STOP
            perf_delta <= perf_cycles[31:0] - perf_start[31:0];
        end
    end
end
```

> Note: `perf_delta` is 32 bits, sufficient for ~50 s at 80 MHz pl_clk.
> If you ever need wider, expand to 64 bits and add a `PERF_DELTA_HI`.

### 3.8 Verify before committing

```sh
# Quick lint
verilator --lint-only fpga/wrapper/t1_axi_lite_wrapper.sv 2>&1 | head -50
```

Or open in Vivado, let `synth_design` give you the elab errors. The
wrapper compiles standalone (no T1 dependency) for a quick smoke check.

---

## 4. Step 3 — Update the Verilog wrapper template

**File:** `fpga/system/gen_wrapper.sh`

This script generates `t1_fpga_top.v` (the single Verilog block Vivado
imports). Two edits:

### 4.1 Add a port to the t1_fpga_top declaration (no edit needed)

The wrapper instantiates T1 internally; you do not need to expose
`verticalMode` at the t1_fpga_top boundary. But if the top has a
`verticalMode` port (because the OLD T1.sv exposed it), you'll need
to remove that port. After § 2, the new T1.sv has
`issue_bits_verticalMode` instead, so:

  * If the existing `t1_fpga_top.v` declaration has `verticalMode` as
    an input or output port: remove it.

### 4.2 Wire issue_bits_verticalMode through

In the `T1 u_t1 (...)` instantiation block, alongside the existing
`issue_bits_*` connections, add:

```verilog
.issue_bits_verticalMode  (issue_bits_verticalMode),
```

And declare the internal wire alongside `wire issue_valid; ...`:

```verilog
wire issue_bits_verticalMode;
```

### 4.3 Wire it through the wrapper instance

In the `t1_axi_lite_wrapper u_axi_lite_wrapper (...)` block, add:

```verilog
.issue_bits_verticalMode  (issue_bits_verticalMode),
```

That's all for `gen_wrapper.sh`. The script's other parameters
(`AXI_ID_W`, `HB_DATA_W`) are derived from the new T1.sv automatically.

---

## 5. Step 4 — Extend the Vivado block design

**File:** `fpga/system/system_top.tcl`

Section by section, add the IP blocks listed in
`fyp_doc/2d_fabric_fpga_design_handoff.md` § 2.2 and § 4.3. Below is
the order to add them and the property settings for each.

### 5.1 Add MIPI CSI-2 RX Subsystem

```tcl
create_bd_cell -type ip -vlnv xilinx.com:ip:mipi_csi2_rx_subsystem:5.4 csi2_rx
set_property -dict [list \
    CONFIG.CMN_NUM_LANES        {4} \
    CONFIG.CMN_NUM_PIXELS       {2} \
    CONFIG.CMN_PXL_FORMAT       {YUV422_8bit} \
    CONFIG.C_HS_LINE_RATE       {800} \
    CONFIG.C_DPHY_LANES         {4} \
] [get_bd_cells csi2_rx]
```

(Confirm exact `CONFIG.*` names against your Vivado version — they
shift between releases. Check the `kv260-smartcam` reference design's
TCL for the canonical settings.)

### 5.2 Add downscaler `v_proc_ss #1`

```tcl
create_bd_cell -type ip -vlnv xilinx.com:ip:v_proc_ss:2.3 vpss_down
set_property -dict [list \
    CONFIG.C_TOPOLOGY            {0} \
    CONFIG.C_SCALER_ALGORITHM    {2} \
    CONFIG.C_MAX_DATA_WIDTH      {8} \
    CONFIG.C_MAX_COLS            {1920} \
    CONFIG.C_MAX_ROWS            {1080} \
    CONFIG.C_NUM_VIDEO_COMPONENTS {3} \
] [get_bd_cells vpss_down]
```

### 5.3 Add `v_frmbuf_wr` (capture)

```tcl
create_bd_cell -type ip -vlnv xilinx.com:ip:v_frmbuf_wr:2.4 frmbuf_wr
set_property -dict [list \
    CONFIG.MAX_COLS              {128} \
    CONFIG.MAX_ROWS              {128} \
    CONFIG.HAS_YUV422            {1} \
    CONFIG.AXIMM_DATA_WIDTH      {128} \
] [get_bd_cells frmbuf_wr]
```

### 5.4 Add `v_frmbuf_rd` (display)

```tcl
create_bd_cell -type ip -vlnv xilinx.com:ip:v_frmbuf_rd:2.4 frmbuf_rd
set_property -dict [list \
    CONFIG.MAX_COLS              {128} \
    CONFIG.MAX_ROWS              {128} \
    CONFIG.HAS_YUV422            {1} \
    CONFIG.AXIMM_DATA_WIDTH      {128} \
] [get_bd_cells frmbuf_rd]
```

### 5.5 Add upscaler `v_proc_ss #2`

```tcl
create_bd_cell -type ip -vlnv xilinx.com:ip:v_proc_ss:2.3 vpss_up
# Same property dict as vpss_down, but configured for 128x128 in,
# display res out.
```

### 5.6 Add HDMI TX subsystem

```tcl
create_bd_cell -type ip -vlnv xilinx.com:ip:v_hdmi_tx_ss:3.2 hdmi_tx
# Configure for 1080p60 by default; check v_hdmi_tx_subsystem
# CONFIG.* options for resolution/colour format.
```

### 5.7 Add AXI IIC for AP1302 sensor

```tcl
create_bd_cell -type ip -vlnv xilinx.com:ip:axi_iic:2.1 sensor_iic
set_property -dict [list \
    CONFIG.C_SCL_INERTIAL_DELAY  {0} \
    CONFIG.IIC_FREQ_KHZ          {400} \
] [get_bd_cells sensor_iic]
```

### 5.8 Add scratchpad: `axi_bram_ctrl` + `blk_mem_gen`

```tcl
create_bd_cell -type ip -vlnv xilinx.com:ip:axi_bram_ctrl:4.1 bram_ctrl
set_property -dict [list \
    CONFIG.SINGLE_PORT_BRAM      {0} \
    CONFIG.DATA_WIDTH            {128} \
    CONFIG.MEM_DEPTH             {2048} \
] [get_bd_cells bram_ctrl]

create_bd_cell -type ip -vlnv xilinx.com:ip:blk_mem_gen:8.4 bram
set_property -dict [list \
    CONFIG.Memory_Type           {True_Dual_Port_RAM} \
    CONFIG.Use_Byte_Write_Enable {true} \
    CONFIG.Byte_Size             {8} \
    CONFIG.Write_Width_A         {128} \
    CONFIG.Write_Depth_A         {2048} \
    CONFIG.Read_Width_A          {128} \
] [get_bd_cells bram]
```

(2048 × 16 bytes = 32 KB.)

### 5.9 Routing — clocks and resets

Connect `pl_clk0` and `proc_sys_reset/peripheral_aresetn` to every new
block's `aclk`/`aresetn`/`*_clk` ports. Pattern:

```tcl
connect_bd_net [get_bd_pins zynq_ps/pl_clk0]              [get_bd_pins csi2_rx/aclk]
connect_bd_net [get_bd_pins proc_sys_reset/peripheral_aresetn] [get_bd_pins csi2_rx/aresetn]
# Repeat for vpss_down, frmbuf_wr, frmbuf_rd, vpss_up, hdmi_tx,
# sensor_iic, bram_ctrl.
```

(For pixel-clock domains on `csi2_rx` and `hdmi_tx`, you may need
additional clocking — check the smart-camera reference.)

### 5.10 Routing — data plane

```tcl
# CSI-2 → downscaler → frmbuf_wr → DDR (via PS HP1)
connect_bd_intf_net [get_bd_intf_pins csi2_rx/video_out]   [get_bd_intf_pins vpss_down/s_axis]
connect_bd_intf_net [get_bd_intf_pins vpss_down/m_axis]    [get_bd_intf_pins frmbuf_wr/s_axis_video]

# DDR → frmbuf_rd → upscaler → HDMI TX
connect_bd_intf_net [get_bd_intf_pins frmbuf_rd/m_axis_video] [get_bd_intf_pins vpss_up/s_axis]
connect_bd_intf_net [get_bd_intf_pins vpss_up/m_axis]         [get_bd_intf_pins hdmi_tx/video_in]

# frmbuf_wr/rd AXI4 masters → SmartConnect → PS HP1/HP2
# (Add a new SmartConnect "smartconnect_video" with NUM_SI=2, NUM_MI=2;
#  route frmbuf_wr to S_AXI_HP1, frmbuf_rd to S_AXI_HP2.)

# Scratchpad routing
connect_bd_intf_net [get_bd_intf_pins bram_ctrl/BRAM_PORTA] [get_bd_intf_pins bram/BRAM_PORTA]
# bram_ctrl/S_AXI is reached by the existing smartconnect_idx
# (extend it: NUM_SI=2 to accept both T1 indexed master and DMA mm2s/s2mm).
```

### 5.11 Routing — control plane

```tcl
# Add the AXI Lite wrapper VERTICAL_MODE plumbing — already done by
# u_axi_lite_wrapper internally; no TCL change needed here.

# Sensor IIC control register from PS GP0
# Extend smartconnect_ctrl: NUM_MI=3 (was 2).
connect_bd_intf_net [get_bd_intf_pins smartconnect_ctrl/M02_AXI] \
                    [get_bd_intf_pins sensor_iic/S_AXI]

# Frmbuf and HDMI control regs also need to be reachable from PS GP0.
# Add another SmartConnect or extend the existing one as needed.
```

### 5.12 Routing — interrupts

```tcl
# IRQ concat already exists with 3 ports (T1, dma_mm2s, dma_s2mm).
# Bump NUM_PORTS and connect new IRQs:
set_property CONFIG.NUM_PORTS {7} [get_bd_cells irq_concat]

connect_bd_net [get_bd_pins frmbuf_wr/interrupt]   [get_bd_pins irq_concat/In3]
connect_bd_net [get_bd_pins frmbuf_rd/interrupt]   [get_bd_pins irq_concat/In4]
connect_bd_net [get_bd_pins hdmi_tx/irq]           [get_bd_pins irq_concat/In5]
connect_bd_net [get_bd_pins sensor_iic/iic2intc_irpt] [get_bd_pins irq_concat/In6]
```

### 5.13 Address map updates

```tcl
# Existing
assign_bd_address -target_address_space /zynq_ps/Data \
    [get_bd_addr_segs t1_top/s_axi_ctrl/reg0] -range 64K -offset 0xA0000000
assign_bd_address -target_address_space /zynq_ps/Data \
    [get_bd_addr_segs axi_dma/S_AXI_LITE/Reg]  -range 64K -offset 0xA0010000

# New
assign_bd_address -target_address_space /zynq_ps/Data \
    [get_bd_addr_segs frmbuf_wr/s_axi_CTRL/Reg] -range 64K -offset 0xA0020000
assign_bd_address -target_address_space /zynq_ps/Data \
    [get_bd_addr_segs frmbuf_rd/s_axi_CTRL/Reg] -range 64K -offset 0xA0030000
assign_bd_address -target_address_space /zynq_ps/Data \
    [get_bd_addr_segs hdmi_tx/S_AXI_CPU_IN/Reg] -range 64K -offset 0xA0040000
assign_bd_address -target_address_space /zynq_ps/Data \
    [get_bd_addr_segs sensor_iic/S_AXI/Reg]     -range 64K -offset 0xA0050000
assign_bd_address -target_address_space /zynq_ps/Data \
    [get_bd_addr_segs vpss_down/s_axi_CTRL/Reg] -range 64K -offset 0xA0060000
assign_bd_address -target_address_space /zynq_ps/Data \
    [get_bd_addr_segs vpss_up/s_axi_CTRL/Reg]   -range 64K -offset 0xA0070000

# Scratchpad — visible to T1 indexed master
assign_bd_address -target_address_space /t1_top/m_axi_idx \
    [get_bd_addr_segs bram_ctrl/S_AXI/Mem0] -range 32K -offset 0xB0000000
# Scratchpad — also visible to DMA for prefetch
assign_bd_address -target_address_space /axi_dma/Data_MM2S \
    [get_bd_addr_segs bram_ctrl/S_AXI/Mem0] -range 32K -offset 0xB0000000
assign_bd_address -target_address_space /axi_dma/Data_S2MM \
    [get_bd_addr_segs bram_ctrl/S_AXI/Mem0] -range 32K -offset 0xB0000000
```

The full memory map after these changes:

```
Control plane (PS GP0):
  0xA0000000 – 0xA000FFFF   T1 AXI Lite wrapper (T1 issue + perf)
  0xA0010000 – 0xA001FFFF   AXI DMA control
  0xA0020000 – 0xA002FFFF   v_frmbuf_wr (capture)
  0xA0030000 – 0xA003FFFF   v_frmbuf_rd (display)
  0xA0040000 – 0xA004FFFF   v_hdmi_tx_ss
  0xA0050000 – 0xA005FFFF   AXI IIC (AP1302)
  0xA0060000 – 0xA006FFFF   v_proc_ss (downscaler)
  0xA0070000 – 0xA007FFFF   v_proc_ss (upscaler)

Data plane:
  0x00000000 – 0x7FFFFFFF   DDR (visible to PS, T1 HBM, T1 idx,
                            frmbuf_wr, frmbuf_rd, DMA)
  0xB0000000 – 0xB0007FFF   BRAM scratchpad (visible to T1 idx, DMA)
```

---

## 6. Step 5 — Build the bitstream

```sh
cd /home/cbt22/code/code_fyp/VisionSoC/fpga/system

# Full bitstream build (synthesis + implementation + bit gen)
bash build_fpga.sh -c mudkip2d128small1bram1chain2lanescale -b
```

If timing fails, try lower pl_clk0 in `system_top.tcl` (search for
`PSU__CRL_APB__PL0_REF_CTRL__FREQMHZ`):

```tcl
CONFIG.PSU__CRL_APB__PL0_REF_CTRL__FREQMHZ {50}  # was 80
```

The current 80 MHz build had WNS = 0.003 ns (passing but tight); 50–60
MHz gives plenty of margin for the new IP blocks.

Outputs land under `fpga/build/<config>-<timestamp>/`:

  * `system_top_wrapper.bit` — the bitstream
  * `timing_impl.rpt`, `utilization_impl.rpt` — close them out before
    declaring done

---

## 7. Step 6 — Generate the device-tree overlay

After the build, generate a DTBO that declares both UIO devices and
the camera/HDMI device tree.

### 7.1 Quick path — fork `kv260-smartcam` overlay

```sh
sudo apt install xlnx-firmware-kv260-smartcam
ls /lib/firmware/xilinx/kv260-smartcam/
# Copy its dtbo source as a starting point, edit to:
#  - drop nodes for blocks we don't have (e.g. demosaic if smartcam has one)
#  - add nodes for the T1 wrapper, AXI DMA, BRAM controller, all with
#    compatible = "generic-uio"
#  - retain AP1302 and CSI-2 nodes verbatim
```

### 7.2 Required UIO nodes

```dts
amba_pl: amba_pl@0 {
    #address-cells = <2>;
    #size-cells = <2>;
    compatible = "simple-bus";
    ranges;

    t1_wrapper: t1@a0000000 {
        compatible = "generic-uio";
        reg = <0x0 0xa0000000 0x0 0x10000>;
        interrupt-parent = <&gic>;
        interrupts = <0 89 4>;  // adjust per pl_ps_irq0 mapping
    };

    axi_dma_0: axi_dma@a0010000 {
        compatible = "generic-uio";
        reg = <0x0 0xa0010000 0x0 0x10000>;
        interrupt-parent = <&gic>;
        interrupts = <0 90 4>, <0 91 4>;  // mm2s, s2mm
    };

    // ... frmbuf_wr/rd, hdmi_tx, sensor_iic, vpss_down, vpss_up,
    // bram_ctrl as needed (V4L2 / DRM nodes use their own
    // compatible strings, NOT generic-uio)
};
```

### 7.3 Compile the DTS to DTBO

```sh
dtc -@ -I dts -O dtb -o system_top_wrapper.dtbo system_top_wrapper.dts
```

Place `.bit` and `.dtbo` together for `fpgautil`:

```sh
sudo fpgautil -b system_top_wrapper.bit -o system_top_wrapper.dtbo
```

---

## 8. Step 7 — Verification on hardware

Boot the Kria with Ubuntu Server 24.04, copy the artefacts, load:

```sh
scp system_top_wrapper.bit ubuntu@kria:/tmp/
scp system_top_wrapper.dtbo ubuntu@kria:/tmp/
ssh ubuntu@kria
cd /tmp
sudo fpgautil -b system_top_wrapper.bit -o system_top_wrapper.dtbo
ls /dev/uio*    # expect /dev/uio0 (T1) and /dev/uio1 (DMA) at minimum
```

### 8.1 Wrapper smoke test

Without a driver, you can `devmem`-poke the wrapper:

```sh
sudo apt install devmem2
# Read PERF_CYCLES_LO twice; it should advance
sudo devmem2 0xa0000050 w
sleep 1
sudo devmem2 0xa0000050 w

# Write VERTICAL_MODE = 1 and read it back
sudo devmem2 0xa0000044 w 0x1
sudo devmem2 0xa0000044 w
```

If `PERF_CYCLES_LO` doesn't advance, the wrapper's free-running counter
isn't ticking — check `aclk` connectivity in the BD.

If `VERTICAL_MODE` doesn't read back as `0x1`, the address decoder
edits are wrong — re-check § 3.4–§ 3.5.

### 8.2 Camera bringup (separate from T1)

Once the AP1302 V4L2 driver is loaded (via the camera DT nodes):

```sh
ls /dev/video*
v4l2-ctl --list-devices
gst-launch-1.0 -v xlnxvideosrc src-name=mediasrcbin0 ! kmssink
# Expect to see camera output on the HDMI monitor
```

This validates the camera+HDMI path independently of T1. Once this
works, plug T1 in.

### 8.3 Full pipeline

That's the driver's job — see `fyp_doc/driver_implementation_handoff.md`
§ 7 for the integrated bringup sequence.

### 8.4 Debugging — common failures

The order below mirrors the order things go wrong during bringup.

#### 8.4.1 `fpgautil -b ...` succeeds but `/dev/uio0` does not appear
  * **Most likely:** the DT overlay either didn't load or doesn't
    declare `compatible = "generic-uio"` for the wrapper.
  * Check: `dmesg | tail -50` — look for "OF: overlay" / "uio_pdrv_genirq"
    lines. If there are errors, the DTS is malformed.
  * Check: `cat /sys/devices/platform/.../uevent` for the wrapper node —
    `MODALIAS` must include `of:Nt1...Cgeneric-uio`.
  * Fix: re-author the DTS following § 7.2 verbatim; recompile with `dtc -@`;
    reload.

#### 8.4.2 `PERF_CYCLES_LO` does not advance between two reads
  * **Most likely:** the wrapper's `aclk` is unconnected, or the
    free-running counter logic in § 3.6 was added but reset is held
    asserted.
  * Check: `sudo devmem2 0xa0000054 w` (the HI register). If both LO and
    HI are stuck at 0, `aclk` is dead.
  * Check: in Vivado, open the BD diagram and confirm
    `t1_top/aclk` ← `zynq_ps/pl_clk0` is wired.
  * Check: `sudo devmem2 0xa0000040 w` (IRQ_STATUS). If it returns all
    1s or all 0s with no change, the entire AXI-Lite bus is wedged —
    likely a bad `aresetn`.
  * Fix: re-check § 3.6 instantiation; verify the reset polarity (the
    wrapper uses active-low `aresetn`).

#### 8.4.3 `VERTICAL_MODE` write does not read back
  * **Most likely:** the write-address case constants in § 3.4 don't
    match `wr_addr` width after `ADDR_WIDTH` was bumped 7→8.
  * Check: `sudo devmem2 0xa0000044 w 0x1` then `sudo devmem2 0xa0000044 w`.
    Should return `0x1`. If returns `0x0`, write went somewhere else.
  * Check: write a different value (e.g. `0x0` after writing `0x1`) and
    read other registers (e.g. `0xa0000004` INSTRUCTION). If
    INSTRUCTION changes when you write VERTICAL_MODE, the address
    decoder collapsed two offsets.
  * Fix: in the wrapper's `case (wr_addr)` block, every constant must
    match the bit-width of `wr_addr`. After `ADDR_WIDTH=8`,
    `wr_addr = aw_addr_reg[7:2]` and constants must be `6'h..` not
    `5'h..`.

#### 8.4.4 Synthesis fails: `issue_bits_verticalMode not declared`
  * **Cause:** § 2 (RTL regen) was skipped. The old `T1.sv` has a
    top-level live `verticalMode` IO instead of an
    `issue_bits_verticalMode` issue-bundle port.
  * Fix: re-run `bash tests/run-test.sh --build-only -c <config>` to
    regenerate RTL from the current Scala. Verify the new T1.sv has
    `issue_bits_verticalMode` (`grep issue_bits_verticalMode T1.sv`).

#### 8.4.5 Implementation timing fails (negative WNS)
  * **Cause:** new IP blocks tightened the design and `pl_clk0` at
    80 MHz no longer closes (the existing build had only 0.003 ns of
    slack — there is no headroom).
  * Fix: drop `pl_clk0` in `system_top.tcl`:
    ```tcl
    CONFIG.PSU__CRL_APB__PL0_REF_CTRL__FREQMHZ {50}
    ```
    Re-implement. Move back up if you want, but only after the
    pipeline is end-to-end working.

#### 8.4.6 Camera path won't bring up: `/dev/video0` missing
  * **Most likely:** the AP1302 V4L2 subdev driver is not loaded,
    because the DT didn't bind it.
  * Check: `dmesg | grep -i 'ap1302\|csi2\|video'`.
  * Check: `media-ctl --print-topology` — look for AR1335 / AP1302
    nodes; if absent, the subdev didn't probe.
  * Fix: reuse the `kv260-smartcam` DT verbatim for the AP1302/CSI-2
    subtree. The AP1302 is finicky about firmware load order;
    smart-camera handles this correctly.

#### 8.4.7 HDMI monitor shows "no signal"
  * **Most likely:** the HDMI TX pixel-clock and link-clock domains are
    different from `aclk` and weren't wired.
  * Check: in the BD, `v_hdmi_tx_ss` has separate `s_axi_cpu_in_aclk`,
    `video_clk`, `link_clk`, `s_axis_video_aclk` ports — all need
    drivers.
  * Check: `xrandr` (or `modetest -c`) on the Kria — does it list a
    connector with active modes?
  * Fix: connect the additional clock domains. The smart-camera ref
    has the canonical wiring.

#### 8.4.8 Bitstream loads, all UIOs visible, but T1 drops issues
  * **Most likely:** `gen_wrapper.sh` template was not updated to wire
    `issue_bits_verticalMode` from wrapper to T1, so vertical mode
    flips never reach T1. Compute kernels still work (they read
    verticalMode=0 implicitly), but vertical-mode kernels silently run
    in horizontal mode.
  * Check: read the generated `t1_fpga_top.v` and confirm both:
    - the wrapper instance has `.issue_bits_verticalMode (wire);`
    - the T1 instance has `.issue_bits_verticalMode (wire);`
    - they connect to the same internal wire.
  * Fix: § 4.2 + § 4.3 of this doc.

#### 8.4.9 Cross-reference

If you're hitting an issue that resembles a programmer-rule violation
from `tests/vision_task/benchmark_vadd.c` (R1–R8) — for example a
"verticalMode set but kernel doesn't transpose" — see
`fyp_doc/2d_fabric_fpga_design_handoff.md` § 5 for the mapping of
sim-side rules onto KV260.

---

## 9. Expected utilisation / timing

For sanity-checking, the existing build (without the new blocks)
hit:

  * WNS: 0.003 ns at 80 MHz pl_clk0 (passing)
  * BRAM tiles: see `utilization_impl.rpt` in the existing build
    folder

With the additions in this doc:

  * +8 BRAM36 tiles for the scratchpad
  * +CSI-2 RX, vpss×2, frmbuf×2, HDMI TX — each is a few hundred LUT
    + a handful of BRAMs
  * Likely budget impact: 5–15 % more of each resource, comfortably
    within KV260 limits

If timing degrades to negative WNS, drop `pl_clk0` to 50–60 MHz first;
re-architect later only if needed.

---

## 10. Quick command reference

```sh
# 1. RTL regen (Scala source already has the bundle field)
nix develop
bash tests/run-test.sh --build-only -c mudkip2d128small1bram1chain2lanescale

# 2. Edit the wrapper SV
$EDITOR fpga/wrapper/t1_axi_lite_wrapper.sv

# 3. Edit gen_wrapper.sh (add issue_bits_verticalMode wiring)
$EDITOR fpga/system/gen_wrapper.sh

# 4. Edit system_top.tcl (add IPs + addresses)
$EDITOR fpga/system/system_top.tcl

# 5. Build bitstream
cd fpga/system && bash build_fpga.sh -c mudkip2d128small1bram1chain2lanescale -b

# 6. Generate DTBO (after editing system_top_wrapper.dts)
dtc -@ -I dts -O dtb -o system_top_wrapper.dtbo system_top_wrapper.dts

# 7. Deploy to Kria
scp <build_dir>/system_top_wrapper.bit ubuntu@kria:/tmp/
scp system_top_wrapper.dtbo ubuntu@kria:/tmp/
ssh ubuntu@kria 'sudo fpgautil -b /tmp/system_top_wrapper.bit -o /tmp/system_top_wrapper.dtbo'
```

That's the FPGA side complete. The driver doc (`driver_implementation_handoff.md`) is what runs on top of this.
