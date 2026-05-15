# Camera Pipeline Restore Handoff (2026-05-09)

## Why this doc exists

The camera capture pipeline (mipi_csi2_rx, v_frmbuf_wr, sensor_iic, axis FIFO/converter,
clk_wiz, two ancillary smartconnects, and the LPD/HP1 PS connections) was **temporarily
removed** from `fpga/system/system_top.tcl` on 2026-05-09 to:

  1. Free ~9k LUTs and resolve the routing congestion that was blocking the
     `MAXIGP0/MAXIGP2 = 128-bit` debug rebuild (timing-driven rip-up looping
     after 11h of impl).
  2. Reduce the BD's surface area while we diagnose the
     `araddr[3:0] != 0000` read-zero bug (see `camera_bringup_status.md` §
     6.1.4). With camera removed, only T1 + DMA remain on the control plane,
     making the AXI4-Lite read-path debug cleaner.

Once the read-zero bug is resolved, paste the verbatim blocks below back into
`system_top.tcl` at the appropriate insertion points. Order matters — IPs
must be created before they're connected. The order below matches the
original layout.

## Restoration steps (high level)

1. Re-add PS config knobs in the `set_property -dict ... [get_bd_cells zynq_ps]` block.
2. Re-add `clk_wiz_0` + `proc_sys_reset_100M` + `proc_sys_reset_300M`.
3. Re-add `sensor_iic` IP cell.
4. Re-add the camera capture pipeline block (mipi_csi2_rx, axis_data_fifo_cap,
   axis_subset_converter_cap, v_frmbuf_wr, ap1302_standby_const, top-level
   ports, smartconnect_lpd, smartconnect_video).
5. Bump `smartconnect_ctrl` NUM_MI from 2 → 3 (re-add M02 = sensor_iic).
6. Re-add `smartconnect_lpd` / `smartconnect_video` to the LOW_AREA strategy
   foreach.
7. Re-add the camera clock/reset wiring.
8. Re-add the camera AXI/AXIS connections.
9. Bump `irq_concat` from NUM_PORTS=3 → 5 and re-add In3 (sensor_iic) + In4
   (csi2).
10. Re-add address-map entries for `mipi_csi2_rx`, `sensor_iic`,
    `v_frmbuf_wr/Data_m_axi_mm_video`.
11. Re-add `pin.xdc` inclusion at the bottom of system_top.tcl.

## Section-by-section verbatim restore content

### 1. PS config knobs to re-enable

In the `set_property -dict [list ... ] [get_bd_cells zynq_ps]` block (around
line 110), add back:

```tcl
    CONFIG.PSU__USE__M_AXI_GP2          {1} \
    CONFIG.PSU__USE__S_AXI_GP3          {1} \
    CONFIG.PSU__MAXIGP2__DATA_WIDTH     {128} \
    CONFIG.PSU__SAXIGP3__DATA_WIDTH     {128} \
```

(`MAXIGP2_DATA_WIDTH=128` matches Fix-2; if the bug fix landed at 32-bit MAXIGP0
instead, set this back to 32 to match.)

### 2. clk_wiz + proc_sys_reset_100M/_300M

Insert before the `sensor_iic` cell (originally above line 338):

```tcl
#----- clk_wiz: derive 100/200/300 MHz from pl_clk0 ----------------------
create_bd_cell -type ip -vlnv xilinx.com:ip:clk_wiz clk_wiz_0
set_property -dict [list \
    CONFIG.PRIM_SOURCE                {Global_buffer} \
    CONFIG.RESET_PORT                 {resetn} \
    CONFIG.RESET_TYPE                 {ACTIVE_LOW} \
    CONFIG.NUM_OUT_CLKS               {3} \
    CONFIG.CLK_OUT1_PORT              {clk_100M} \
    CONFIG.CLKOUT1_REQUESTED_OUT_FREQ {100.000} \
    CONFIG.CLK_OUT2_PORT              {clk_200M} \
    CONFIG.CLKOUT2_USED               {true} \
    CONFIG.CLKOUT2_REQUESTED_OUT_FREQ {200.000} \
    CONFIG.CLK_OUT3_PORT              {clk_300M} \
    CONFIG.CLKOUT3_USED               {true} \
    CONFIG.CLKOUT3_REQUESTED_OUT_FREQ {300.000} \
] [get_bd_cells clk_wiz_0]

connect_bd_net [get_bd_pins zynq_ps/pl_clk0]    [get_bd_pins clk_wiz_0/clk_in1]
connect_bd_net [get_bd_pins zynq_ps/pl_resetn0] [get_bd_pins clk_wiz_0/resetn]

#proc_sys_reset for the 100 MHz and 300 MHz domains
create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 proc_sys_reset_100M
connect_bd_net [get_bd_pins clk_wiz_0/clk_100M]  [get_bd_pins proc_sys_reset_100M/slowest_sync_clk]
connect_bd_net [get_bd_pins zynq_ps/pl_resetn0]  [get_bd_pins proc_sys_reset_100M/ext_reset_in]
connect_bd_net [get_bd_pins clk_wiz_0/locked]    [get_bd_pins proc_sys_reset_100M/dcm_locked]

create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 proc_sys_reset_300M
connect_bd_net [get_bd_pins clk_wiz_0/clk_300M]  [get_bd_pins proc_sys_reset_300M/slowest_sync_clk]
connect_bd_net [get_bd_pins zynq_ps/pl_resetn0]  [get_bd_pins proc_sys_reset_300M/ext_reset_in]
connect_bd_net [get_bd_pins clk_wiz_0/locked]    [get_bd_pins proc_sys_reset_300M/dcm_locked]
```

### 3. sensor_iic (AP1302 IIC)

```tcl
#----- AXI IIC for AP1302 sensor (60 MHz pl_clk0 ctrl) -------------------
# IIC pins routed via the carrier's "hda_iic_switch" board interface
# (per kv260_carrier 1.3 board.xml — this is the I2C switch that sits in
# front of the AP1302 + AR1335 sensor).
create_bd_cell -type ip -vlnv xilinx.com:ip:axi_iic:2.1 sensor_iic
set_property -dict [list \
    CONFIG.IIC_FREQ_KHZ          {400} \
    CONFIG.IIC_BOARD_INTERFACE   {som240_1_connector_hda_iic_switch} \
    CONFIG.USE_BOARD_FLOW        {true} \
] [get_bd_cells sensor_iic]
connect_bd_net [get_bd_pins zynq_ps/pl_clk0]                   [get_bd_pins sensor_iic/s_axi_aclk]
connect_bd_net [get_bd_pins proc_sys_reset/peripheral_aresetn] [get_bd_pins sensor_iic/s_axi_aresetn]
```

### 4. Camera capture pipeline IPs (the original block, re-add as-is)

Insert after `bram_ctrl` block, before the smartconnect strategy foreach.
This is the **slimmed** version (256x256, UYVY-only, 64-bit AXIMM) from the
2026-05-08 work — the comment block at the top documents the original
1080p reference values if you ever want to bump back up.

```tcl
#----- Camera capture pipeline IPs (300 MHz video, 100 MHz lite, 200 MHz DPHY) -----
# CONFIG dicts originally copied verbatim from kria-vitis-platforms /
# kv260_ispMipiRx_vcu_DP / scripts / config_bd.tcl :: create_hier_cell_capture_pipeline.
#
# 2026-05-08 SLIM: trimmed for our actual use case (UYVY 128x128 from
# AP1302 ISP @ ~30 fps). The reference targeted 1080p RGB; we don't
# need that capacity. Changes:
#   * MAX_COLS/MAX_ROWS 1920/1080 -> 256/256 (line buffer / counter
#     storage scales with these). 256 keeps 2x headroom over the
#     128x128 actual frame size.
#   * MAX_NR_PLANES 2 -> 1 (only Y_UV8_420 needed 2 planes; that
#     format is no longer enabled).
#   * Drop HAS_RGB8 / HAS_Y8 / HAS_Y_UV8_420 — only HAS_UYVY8 is
#     wired through the camera path.
#   * AXIMM_DATA_WIDTH 128 -> 64 (m_axi_mm_video). At 128*128*UYVY*30
#     = ~1 MB/s, 64-bit at 300 MHz is 100x over-provisioned.
#   * mipi_csi2_rx CSI_BUF_DEPTH 4096 -> 1024 (line buffer).
#   * axis_data_fifo_cap FIFO_DEPTH 1024 -> 256.
#   * axis_subset_converter_cap M_TDATA_NUM_BYTES 6 -> 4 and
#     TDATA_REMAP simplified to a passthrough.
# NOTE: there's a known BD-validation CRITICAL WARNING [BD 41-237]
# about M_AXIS(4) ≠ s_axis_video(6) — once unused frmbuf formats are
# dropped the s_axis_video port should narrow to 4 bytes, but in
# practice the warning persisted in build 5e. Investigate before
# enabling the camera path. v_frmbuf_wr's required s_axis_video width
# is the real source of truth — re-test after re-add.

create_bd_cell -type ip -vlnv xilinx.com:ip:mipi_csi2_rx_subsystem mipi_csi2_rx
set_property -dict [list \
    CONFIG.DPHYRX_BOARD_INTERFACE        {som240_1_connector_mipi_csi_isp} \
    CONFIG.CMN_NUM_PIXELS                {2} \
    CONFIG.CMN_VC                        {0} \
    CONFIG.CSI_BUF_DEPTH                 {1024} \
    CONFIG.C_CSI_FILTER_USERDATATYPE     {true} \
    CONFIG.C_HS_LINE_RATE                {896} \
    CONFIG.C_HS_SETTLE_NS                {146} \
    CONFIG.DPY_LINE_RATE                 {896} \
    CONFIG.SupportLevel                  {1} \
] [get_bd_cells mipi_csi2_rx]

create_bd_cell -type ip -vlnv xilinx.com:ip:axis_data_fifo axis_data_fifo_cap
set_property -dict [list CONFIG.FIFO_DEPTH {256}] [get_bd_cells axis_data_fifo_cap]

create_bd_cell -type ip -vlnv xilinx.com:ip:axis_subset_converter axis_subset_converter_cap
set_property -dict [list \
    CONFIG.M_TDATA_NUM_BYTES {4} \
    CONFIG.M_TDEST_WIDTH     {1} \
    CONFIG.S_TDATA_NUM_BYTES {4} \
    CONFIG.S_TDEST_WIDTH     {10} \
    CONFIG.TDATA_REMAP       {tdata[31:0]} \
    CONFIG.TDEST_REMAP       {tdest[0:0]} \
] [get_bd_cells axis_subset_converter_cap]

create_bd_cell -type ip -vlnv xilinx.com:ip:v_frmbuf_wr v_frmbuf_wr
set_property -dict [list \
    CONFIG.AXIMM_DATA_WIDTH               {64} \
    CONFIG.C_M_AXI_MM_VIDEO_DATA_WIDTH    {64} \
    CONFIG.HAS_BGR8                       {0} \
    CONFIG.HAS_BGRX8                      {0} \
    CONFIG.HAS_RGB8                       {0} \
    CONFIG.HAS_UYVY8                      {1} \
    CONFIG.HAS_YUV8                       {0} \
    CONFIG.HAS_Y8                         {0} \
    CONFIG.HAS_Y_UV8                      {0} \
    CONFIG.HAS_Y_UV8_420                  {0} \
    CONFIG.MAX_COLS                       {256} \
    CONFIG.MAX_ROWS                       {256} \
    CONFIG.MAX_NR_PLANES                  {1} \
    CONFIG.SAMPLES_PER_CLOCK              {2} \
] [get_bd_cells v_frmbuf_wr]

#----- Tie AP1302 standby low (constant) and rst_b to peripheral_aresetn ----
create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant ap1302_standby_const
set_property -dict [list CONFIG.CONST_VAL {0} CONFIG.CONST_WIDTH {1}] [get_bd_cells ap1302_standby_const]

#----- Top-level interface ports for camera ------------------------------
# Board interface ports auto-mapped to KV260 carrier som240 connector pins.
create_bd_intf_port -mode Slave  -vlnv xilinx.com:interface:mipi_phy_rtl:1.0 mipi_phy_if
create_bd_intf_port -mode Master -vlnv xilinx.com:interface:iic_rtl:1.0       iic
create_bd_port -dir O ap1302_rst_b
create_bd_port -dir O ap1302_standby

#----- New SmartConnects ------------------------------------------------
# smartconnect_lpd: PS LPD GP master (100 MHz) -> CSI-2 lite control
create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 smartconnect_lpd
set_property -dict [list CONFIG.NUM_SI {1} CONFIG.NUM_MI {1} CONFIG.NUM_CLKS {1}] [get_bd_cells smartconnect_lpd]
connect_bd_net [get_bd_pins clk_wiz_0/clk_100M]                  [get_bd_pins smartconnect_lpd/aclk]
connect_bd_net [get_bd_pins proc_sys_reset_100M/interconnect_aresetn] [get_bd_pins smartconnect_lpd/aresetn]

# smartconnect_video: frmbuf m_axi_mm_video (300 MHz) -> PS HP1 (300 MHz)
create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 smartconnect_video
set_property -dict [list CONFIG.NUM_SI {1} CONFIG.NUM_MI {1} CONFIG.NUM_CLKS {1}] [get_bd_cells smartconnect_video]
connect_bd_net [get_bd_pins clk_wiz_0/clk_300M]                  [get_bd_pins smartconnect_video/aclk]
connect_bd_net [get_bd_pins proc_sys_reset_300M/interconnect_aresetn] [get_bd_pins smartconnect_video/aresetn]
```

### 5. smartconnect_ctrl NUM_MI bump (2 → 3)

Change the line:

```tcl
set_property -dict [list CONFIG.NUM_MI {2} CONFIG.NUM_CLKS {1}] [get_bd_cells smartconnect_ctrl]
```

back to:

```tcl
set_property -dict [list CONFIG.NUM_MI {3} CONFIG.NUM_CLKS {1}] [get_bd_cells smartconnect_ctrl]
```

### 6. SmartConnect strategy foreach

Re-add `smartconnect_lpd` and `smartconnect_video` to the foreach list:

```tcl
foreach sc {smartconnect_ctrl smartconnect_lpd smartconnect_hb smartconnect_idx smartconnect_video} {
    set_property CONFIG.STRATEGY {LOW_AREA} [get_bd_cells $sc]
}
```

### 7. Camera clock/reset wiring

```tcl
#----- Camera pipeline clocks/resets ------------------------------------
# CSI-2 RX
connect_bd_net [get_bd_pins clk_wiz_0/clk_100M]                  [get_bd_pins mipi_csi2_rx/lite_aclk]
connect_bd_net [get_bd_pins proc_sys_reset_100M/peripheral_aresetn] [get_bd_pins mipi_csi2_rx/lite_aresetn]
connect_bd_net [get_bd_pins clk_wiz_0/clk_200M]                  [get_bd_pins mipi_csi2_rx/dphy_clk_200M]
connect_bd_net [get_bd_pins clk_wiz_0/clk_300M]                  [get_bd_pins mipi_csi2_rx/video_aclk]
connect_bd_net [get_bd_pins proc_sys_reset_300M/peripheral_aresetn] [get_bd_pins mipi_csi2_rx/video_aresetn]

# axis_data_fifo + axis_subset_converter (300 MHz video domain)
connect_bd_net [get_bd_pins clk_wiz_0/clk_300M]                  [get_bd_pins axis_data_fifo_cap/s_axis_aclk]
connect_bd_net [get_bd_pins proc_sys_reset_300M/peripheral_aresetn] [get_bd_pins axis_data_fifo_cap/s_axis_aresetn]
connect_bd_net [get_bd_pins clk_wiz_0/clk_300M]                  [get_bd_pins axis_subset_converter_cap/aclk]
connect_bd_net [get_bd_pins proc_sys_reset_300M/peripheral_aresetn] [get_bd_pins axis_subset_converter_cap/aresetn]

# v_frmbuf_wr
connect_bd_net [get_bd_pins clk_wiz_0/clk_300M]                  [get_bd_pins v_frmbuf_wr/ap_clk]
connect_bd_net [get_bd_pins proc_sys_reset_300M/peripheral_aresetn] [get_bd_pins v_frmbuf_wr/ap_rst_n]

# PS HP1 slave aclk = clk_300M (matches frmbuf master)
connect_bd_net [get_bd_pins clk_wiz_0/clk_300M] [get_bd_pins zynq_ps/saxihp1_fpd_aclk]
# PS LPD HPM0 master aclk = clk_100M (matches CSI-2 lite_aclk)
connect_bd_net [get_bd_pins clk_wiz_0/clk_100M] [get_bd_pins zynq_ps/maxihpm0_lpd_aclk]
```

### 8. Camera AXI/AXIS interface connections

```tcl
#----- Camera AXI interface connections ---------------------------------
# Camera data path: csi2 -> data_fifo -> subset_converter -> frmbuf_wr
connect_bd_intf_net [get_bd_intf_pins mipi_csi2_rx/video_out]              [get_bd_intf_pins axis_data_fifo_cap/S_AXIS]
connect_bd_intf_net [get_bd_intf_pins axis_data_fifo_cap/M_AXIS]           [get_bd_intf_pins axis_subset_converter_cap/S_AXIS]
connect_bd_intf_net [get_bd_intf_pins axis_subset_converter_cap/M_AXIS]    [get_bd_intf_pins v_frmbuf_wr/s_axis_video]

# DPHY pins from carrier interface
connect_bd_intf_net [get_bd_intf_ports mipi_phy_if]                        [get_bd_intf_pins mipi_csi2_rx/mipi_phy_if]

# Sensor IIC interface to top-level board IIC port
connect_bd_intf_net [get_bd_intf_ports iic]                                [get_bd_intf_pins sensor_iic/IIC]

# AP1302 control GPIOs
connect_bd_net [get_bd_pins proc_sys_reset/peripheral_aresetn]   [get_bd_ports ap1302_rst_b]
connect_bd_net [get_bd_pins ap1302_standby_const/dout]           [get_bd_ports ap1302_standby]

# CSI-2 lite csr (PS LPD GP -> smartconnect_lpd -> csi2 lite)
connect_bd_intf_net [get_bd_intf_pins zynq_ps/M_AXI_HPM0_LPD]              [get_bd_intf_pins smartconnect_lpd/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins smartconnect_lpd/M00_AXI]            [get_bd_intf_pins mipi_csi2_rx/csirxss_s_axi]

# frmbuf m_axi_mm_video -> smartconnect_video -> PS HP1
connect_bd_intf_net [get_bd_intf_pins v_frmbuf_wr/m_axi_mm_video]          [get_bd_intf_pins smartconnect_video/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins smartconnect_video/M00_AXI]          [get_bd_intf_pins zynq_ps/S_AXI_HP1_FPD]

# sensor_iic on smartconnect_ctrl/M02
connect_bd_intf_net [get_bd_intf_pins smartconnect_ctrl/M02_AXI]           [get_bd_intf_pins sensor_iic/S_AXI]
```

### 9. IRQ_concat extension

Change `irq_concat`:

```tcl
set_property CONFIG.NUM_PORTS {3} [get_bd_cells irq_concat]
```

back to:

```tcl
set_property CONFIG.NUM_PORTS {5} [get_bd_cells irq_concat]
connect_bd_net [get_bd_pins sensor_iic/iic2intc_irpt]   [get_bd_pins irq_concat/In3]
connect_bd_net [get_bd_pins mipi_csi2_rx/csirxss_csi_irq] [get_bd_pins irq_concat/In4]
```

(Note: the previous configuration also had In5 for `v_frmbuf_wr/interrupt`,
but that was dropped on 2026-05-07 when v_frmbuf_wr/s_axi_CTRL was detached.
If the camera path is fully re-enabled with frmbuf control, also bump
NUM_PORTS to 6 and add `connect_bd_net [get_bd_pins v_frmbuf_wr/interrupt]
[get_bd_pins irq_concat/In5]`.)

### 10. Address-map entries

Re-add to the address-map block:

```tcl
#mipi_csi2_rx CSR (100 MHz LPD control plane via smartconnect_lpd).
#PS LPD GP master can only reach 0x8000_0000-0x9FFF_FFFF apertures
#(distinct from the FPD GP aperture at 0xA000_0000+). 0x80000000 matches
#the reference platform address.
assign_bd_address -target_address_space /zynq_ps/Data \
    [get_bd_addr_segs mipi_csi2_rx/csirxss_s_axi/Reg] -range 64K -offset 0x80000000

#AP1302 IIC control (60 MHz control plane via smartconnect_ctrl/M02)
assign_bd_address -target_address_space /zynq_ps/Data \
    [get_bd_addr_segs sensor_iic/S_AXI/Reg] -range 64K -offset 0xA0050000

#v_frmbuf_wr m_axi_mm_video -> DDR via HP1 (full 2GB DDR-low range)
assign_bd_address -target_address_space /v_frmbuf_wr/Data_m_axi_mm_video \
    [get_bd_addr_segs zynq_ps/SAXIGP3/HP1_DDR_LOW] -range 2G -offset 0x00000000
```

(The `v_frmbuf_wr/s_axi_CTRL` address-map at 0xA0020000 was already detached
2026-05-07 and is intentionally not in this restore list — re-add only if
you also re-add M03 to smartconnect_ctrl. See § 6.1.3 in
camera_bringup_status.md for that decision.)

### 11. pin.xdc inclusion

Re-add at the bottom of system_top.tcl, before `save_bd_design`:

```tcl
# AP1302 standby / rst_b pin constraints (J10 / J11 on KV260 IAS connector).
# Required because these two ports are not bound to a Vivado board interface,
# so they have no auto-supplied LOC / IOSTANDARD.
set xdc_pin [file join [file dirname [file normalize [info script]]] pin.xdc]
add_files -fileset constrs_1 -norecurse ${xdc_pin}
```

`fpga/system/pin.xdc` is preserved on disk; no need to re-create it.

### 12. dts-side notes

`fpga/dts/system_top_wrapper.dts` still has nodes for `axi_iic_sensor`,
`isp_csiss`, `isp_fb_wr_csi`, `isp_vcap_csi` — all currently
`status = "disabled"`. After re-enabling the camera pipeline in the BD,
flip those to `status = "okay"` (one node at a time during bringup, in
the order recommended by § 6.3 of camera_bringup_status.md). The
addresses in the dts already match what's documented above
(0x80000000, 0xA0050000, etc.).

### 13. Driver-side notes

Camera-touching code in `vision_software/`:

  * `vision_software/visionsoc_main/camera.c` — V4L2 capture from
    `/dev/video0` (xilinx-frmbuf) is unconditionally compiled and was
    never gated on bitstream features. After camera re-enable, no source
    edits needed for this file.
  * `libt1` / `triage_t1` — entirely bitstream-agnostic; no changes needed.

## Resource impact when re-added

Approximate utilization of the camera pipeline at the slimmed config (per
build 5e's `utilization_synth.rpt`, modulo the 64-bit AXIMM-data-width
changes):

| Cell | LUTs | FFs | RAMB | DSPs |
|---|---:|---:|---:|---:|
| mipi_csi2_rx | ~4400 | ~6200 | 4 + 1 URAM | 0 |
| v_frmbuf_wr | ~3700 | ~4600 | 4 | 1 |
| smartconnect_lpd | ~600 | ~720 | 0 | 0 |
| smartconnect_video | ~670 | ~670 | 0 | 0 |
| sensor_iic | ~380 | ~360 | 0 | 0 |
| axis_data_fifo_cap | ~60 | ~60 | 1 | 0 |
| axis_subset_converter_cap | tiny | tiny | 0 | 0 |
| clk_wiz_0 + 2× proc_sys_reset | tiny | tiny | 0 | 0 |
| **Total** | **~9.8k** | **~12k** | **~9 + 1 URAM** | **1** |

If post-restore utilization pushes the design back over the routing cliff,
the priorities for re-trimming are: (a) reduce mipi_csi2_rx CSI_BUF_DEPTH
(currently 1024) further, (b) drop v_frmbuf_wr SAMPLES_PER_CLOCK from 2 to
1 (requires re-deriving the AXIS chain widths — non-trivial, but ~30%
v_frmbuf_wr saving), (c) re-evaluate whether the camera path actually
needs 300 MHz video clock or whether 60 MHz is sufficient at 128x128 / 30
fps (it is — that'd let camera live entirely on pl_clk0 and remove
clk_wiz_0 + the 100M/300M reset domains).
