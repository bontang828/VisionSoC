#Vivado Block Design TCL Script - CAMERA-ONLY TEST BITSTREAM

# Purpose: validate the 1/256/1 LUT trim at pl_clk0=100 MHz.
# Camtest3 v8 (2/4096/2 smartcam-canonical) works end-to-end with
# mediasrcbin (2026-05-14, see fyp_doc/camera_handoff_2026-05-13.md
# § 4.22-4.23). camtest4 keeps everything else identical to v8 but
# drops to CMN_NUM_PIXELS=1, CSI_BUF_DEPTH=256, SAMPLES_PER_CLOCK=1.
# Expected savings vs v8: ~2.5-3.5k LUTs (out of v8's 8053). This
# trim was tested at pl_clk0=60 in the 5o era and failed, but that
# failure was SUPERSEDED by the FVCO finding (60 was the real bug,
# not the trim). pl_clk0=100 is preserved here.

# T1 / DMA / BRAM / smartconnect_hb / smartconnect_idx are all removed
# so synth/route is ~25 min for camera-only.

# Usage:
#   vivado -mode batch -source system_top_camtest4.tcl -tclargs <build_dir>
# Launched by build_camtest4.sh.

if {$argc < 1} {
    puts "ERROR: Usage: system_top_camtest4.tcl <build_dir>"
    exit 1
}

set build_dir [lindex $argv 0]

puts "========================================"
puts "Camera-only test BD"
puts "Build dir: $build_dir"
puts "========================================"

# Project setup - DISTINCT project name from production t1_*_system to
# avoid colliding with 5p's running build in fpga/build/.
set project_name "camtest4_system"
set project_dir [file dirname [file dirname [file normalize [info script]]]]/build/${project_name}
set part "xck26-sfvc784-2LV-c"
set board "xilinx.com:kv260_som:part0:1.4"

create_project ${project_name} ${project_dir} -part ${part} -force
set_property board_part ${board} [current_project]
set_property board_connections {som240_1_connector xilinx.com:kv260_carrier:som240_1_connector:1.3} [current_project]

# Block design
set bd_name "system_top_camtest4"
create_bd_design ${bd_name}

# Zynq UltraScale+ PS
create_bd_cell -type ip -vlnv xilinx.com:ip:zynq_ultra_ps_e:3.5 zynq_ps
apply_bd_automation -rule xilinx.com:bd_rule:zynq_ultra_ps_e \
    -config {apply_board_preset "1"} [get_bd_cells zynq_ps]

# Minimal PS config for camera + new EMIO GPIO for AP1302 reset:
#   M_AXI_HPM0_FPD (60 MHz)  -> sensor_iic via smartconnect_ctrl
#   M_AXI_HPM0_LPD (100 MHz) -> mipi_csi2_rx CSR + v_frmbuf_wr CSR
#   S_AXI_HP1_FPD  (300 MHz) -> v_frmbuf_wr data plane (-> DDR)
#   GPIO_EMIO width 2        -> emio_gpio_o[1] drives ap1302_rst_b (dts gpio 79)
#   IRQ0                     -> camera/iic interrupts
# Width 128 on MAXIGP0 retained from Fix 2 (§ 6.1.5) even though we don't
# need it for this test - keeping it doesn't cost anything and avoids re-
# triggering the AXI4-Lite read-lane bug in case something on smartconnect_ctrl
# does a 32-bit access.
set_property -dict [list \
    CONFIG.PSU__USE__M_AXI_GP0          {1} \
    CONFIG.PSU__USE__M_AXI_GP1          {0} \
    CONFIG.PSU__USE__M_AXI_GP2          {1} \
    CONFIG.PSU__USE__S_AXI_GP0          {0} \
    CONFIG.PSU__USE__S_AXI_GP2          {0} \
    CONFIG.PSU__USE__S_AXI_GP3          {1} \
    CONFIG.PSU__USE__IRQ0               {1} \
    CONFIG.PSU__MAXIGP0__DATA_WIDTH     {128} \
    CONFIG.PSU__MAXIGP2__DATA_WIDTH     {128} \
    CONFIG.PSU__SAXIGP3__DATA_WIDTH     {128} \
    CONFIG.PSU__CRL_APB__PL0_REF_CTRL__FREQMHZ {100} \
    CONFIG.PSU__GPIO_EMIO__PERIPHERAL__ENABLE  {1} \
    CONFIG.PSU__GPIO_EMIO__PERIPHERAL__IO      {2} \
] [get_bd_cells zynq_ps]

# proc_sys_reset for pl_clk0 (60 MHz)
create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 proc_sys_reset
connect_bd_net [get_bd_pins zynq_ps/pl_clk0]     [get_bd_pins proc_sys_reset/slowest_sync_clk]
connect_bd_net [get_bd_pins zynq_ps/pl_resetn0]  [get_bd_pins proc_sys_reset/ext_reset_in]

# PS AXI clocks
connect_bd_net [get_bd_pins zynq_ps/pl_clk0]     [get_bd_pins zynq_ps/maxihpm0_fpd_aclk]

# clk_wiz: derive 100/200/300 MHz from pl_clk0 (60 MHz)
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

# proc_sys_reset for 100 MHz + 300 MHz domains
create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 proc_sys_reset_100M
connect_bd_net [get_bd_pins clk_wiz_0/clk_100M]  [get_bd_pins proc_sys_reset_100M/slowest_sync_clk]
connect_bd_net [get_bd_pins zynq_ps/pl_resetn0]  [get_bd_pins proc_sys_reset_100M/ext_reset_in]
connect_bd_net [get_bd_pins clk_wiz_0/locked]    [get_bd_pins proc_sys_reset_100M/dcm_locked]

create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 proc_sys_reset_300M
connect_bd_net [get_bd_pins clk_wiz_0/clk_300M]  [get_bd_pins proc_sys_reset_300M/slowest_sync_clk]
connect_bd_net [get_bd_pins zynq_ps/pl_resetn0]  [get_bd_pins proc_sys_reset_300M/ext_reset_in]
connect_bd_net [get_bd_pins clk_wiz_0/locked]    [get_bd_pins proc_sys_reset_300M/dcm_locked]

# AXI IIC for AP1302 sensor (60 MHz pl_clk0)
create_bd_cell -type ip -vlnv xilinx.com:ip:axi_iic:2.1 sensor_iic
set_property -dict [list \
    CONFIG.IIC_FREQ_KHZ          {400} \
    CONFIG.IIC_BOARD_INTERFACE   {som240_1_connector_hda_iic_switch} \
    CONFIG.USE_BOARD_FLOW        {true} \
] [get_bd_cells sensor_iic]
connect_bd_net [get_bd_pins zynq_ps/pl_clk0]                   [get_bd_pins sensor_iic/s_axi_aclk]
connect_bd_net [get_bd_pins proc_sys_reset/peripheral_aresetn] [get_bd_pins sensor_iic/s_axi_aresetn]

# smartconnect_ctrl: PS HPM0_FPD (60 MHz) -> sensor_iic only
create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 smartconnect_ctrl
set_property -dict [list \
    CONFIG.NUM_SI   {1} \
    CONFIG.NUM_MI   {1} \
    CONFIG.NUM_CLKS {1} \
] [get_bd_cells smartconnect_ctrl]
connect_bd_net [get_bd_pins zynq_ps/pl_clk0]                     [get_bd_pins smartconnect_ctrl/aclk]
connect_bd_net [get_bd_pins proc_sys_reset/interconnect_aresetn] [get_bd_pins smartconnect_ctrl/aresetn]

connect_bd_intf_net [get_bd_intf_pins zynq_ps/M_AXI_HPM0_FPD]  [get_bd_intf_pins smartconnect_ctrl/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins smartconnect_ctrl/M00_AXI] [get_bd_intf_pins sensor_iic/S_AXI]

# Camera pipeline IPs - camtest4: 1/256/1 LUT trim at pl_clk0=100 MHz.
# The 1/256/1 trim was originally tested at pl_clk0=60 in the 5o era and
# failed; that failure was SUPERSEDED by the FVCO finding (pl_clk0=60 was
# the real cause, not the trim). At pl_clk0=100 + mediasrcbin runtime,
# this trim should still capture frames while saving ~2.5-3.5k LUTs vs
# the smartcam-canonical 2/4096/2.
create_bd_cell -type ip -vlnv xilinx.com:ip:mipi_csi2_rx_subsystem mipi_csi2_rx
set_property -dict [list \
    CONFIG.DPHYRX_BOARD_INTERFACE        {som240_1_connector_mipi_csi_isp} \
    CONFIG.CMN_NUM_PIXELS                {1} \
    CONFIG.CMN_VC                        {0} \
    CONFIG.CSI_BUF_DEPTH                 {256} \
    CONFIG.C_CSI_FILTER_USERDATATYPE     {true} \
    CONFIG.C_HS_LINE_RATE                {896} \
    CONFIG.C_HS_SETTLE_NS                {146} \
    CONFIG.DPY_LINE_RATE                 {896} \
    CONFIG.SupportLevel                  {1} \
] [get_bd_cells mipi_csi2_rx]

create_bd_cell -type ip -vlnv xilinx.com:ip:axis_data_fifo axis_data_fifo_cap
# 5q-trim: FIFO_DEPTH 1024 -> 256 (over-provisioned at 1024 for 128px lines).
set_property -dict [list \
    CONFIG.FIFO_DEPTH {256} \
] [get_bd_cells axis_data_fifo_cap]

create_bd_cell -type ip -vlnv xilinx.com:ip:axis_subset_converter axis_subset_converter_cap
# 1/256/1 chain: csiss output is 2 bytes/cycle (1 pixel @ 16 bpp UYVY-equiv);
# v_frmbuf_wr/s_axis_video at SAMPLES_PER_CLOCK=1 expects 3 bytes (Vivado
# pads upper byte for format alignment). TDATA_REMAP zero-pads upper 8 bits.
set_property -dict [list \
    CONFIG.M_TDATA_NUM_BYTES {3} \
    CONFIG.M_TDEST_WIDTH     {1} \
    CONFIG.S_TDATA_NUM_BYTES {2} \
    CONFIG.S_TDEST_WIDTH     {10} \
    CONFIG.TDATA_REMAP       {8'b00000000,tdata[15:0]} \
    CONFIG.TDEST_REMAP       {tdest[0:0]} \
] [get_bd_cells axis_subset_converter_cap]

# v_frmbuf_wr: 1/256/1 trim with NV12 (HAS_Y_UV8_420=1) only.
# AXIMM 128 -> 64 (saves LUTs in the M_AXI burst converter).
create_bd_cell -type ip -vlnv xilinx.com:ip:v_frmbuf_wr v_frmbuf_wr
set_property -dict [list \
    CONFIG.AXIMM_DATA_WIDTH               {64} \
    CONFIG.C_M_AXI_MM_VIDEO_DATA_WIDTH    {64} \
    CONFIG.HAS_BGR8                       {0} \
    CONFIG.HAS_BGRX8                      {0} \
    CONFIG.HAS_RGB8                       {0} \
    CONFIG.HAS_UYVY8                      {0} \
    CONFIG.HAS_YUV8                       {0} \
    CONFIG.HAS_Y8                         {0} \
    CONFIG.HAS_Y_UV8                      {0} \
    CONFIG.HAS_Y_UV8_420                  {1} \
    CONFIG.MAX_COLS                       {256} \
    CONFIG.MAX_ROWS                       {256} \
    CONFIG.MAX_NR_PLANES                  {2} \
    CONFIG.SAMPLES_PER_CLOCK              {1} \
] [get_bd_cells v_frmbuf_wr]

# AP1302 standby = constant 0 (never standby)
create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant ap1302_standby_const
set_property -dict [list CONFIG.CONST_VAL {0} CONFIG.CONST_WIDTH {1}] [get_bd_cells ap1302_standby_const]

# Top-level interface ports for camera
create_bd_intf_port -mode Slave  -vlnv xilinx.com:interface:mipi_phy_rtl:1.0 mipi_phy_if
create_bd_intf_port -mode Master -vlnv xilinx.com:interface:iic_rtl:1.0       iic
create_bd_port -dir O ap1302_rst_b
create_bd_port -dir O ap1302_standby

# smartconnect_lpd: PS HPM0_LPD (100 MHz) -> csi2 lite (100 MHz) + frmbuf CSR (300 MHz)
create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 smartconnect_lpd
set_property -dict [list CONFIG.NUM_SI {1} CONFIG.NUM_MI {2} CONFIG.NUM_CLKS {2}] [get_bd_cells smartconnect_lpd]
connect_bd_net [get_bd_pins clk_wiz_0/clk_100M]                       [get_bd_pins smartconnect_lpd/aclk]
connect_bd_net [get_bd_pins proc_sys_reset_100M/interconnect_aresetn] [get_bd_pins smartconnect_lpd/aresetn]
connect_bd_net [get_bd_pins clk_wiz_0/clk_300M]                       [get_bd_pins smartconnect_lpd/aclk1]

# smartconnect_video: frmbuf m_axi_mm_video (300 MHz) -> PS HP1 (300 MHz)
create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 smartconnect_video
set_property -dict [list CONFIG.NUM_SI {1} CONFIG.NUM_MI {1} CONFIG.NUM_CLKS {1}] [get_bd_cells smartconnect_video]
connect_bd_net [get_bd_pins clk_wiz_0/clk_300M]                       [get_bd_pins smartconnect_video/aclk]
connect_bd_net [get_bd_pins proc_sys_reset_300M/interconnect_aresetn] [get_bd_pins smartconnect_video/aresetn]

# All smartconnects use LOW_AREA strategy (consistent with main BD)
foreach sc {smartconnect_ctrl smartconnect_lpd smartconnect_video} {
    set_property CONFIG.STRATEGY {LOW_AREA} [get_bd_cells $sc]
}

# Camera clocks/resets
connect_bd_net [get_bd_pins clk_wiz_0/clk_100M]                     [get_bd_pins mipi_csi2_rx/lite_aclk]
connect_bd_net [get_bd_pins proc_sys_reset_100M/peripheral_aresetn] [get_bd_pins mipi_csi2_rx/lite_aresetn]
connect_bd_net [get_bd_pins clk_wiz_0/clk_200M]                     [get_bd_pins mipi_csi2_rx/dphy_clk_200M]
connect_bd_net [get_bd_pins clk_wiz_0/clk_300M]                     [get_bd_pins mipi_csi2_rx/video_aclk]
connect_bd_net [get_bd_pins proc_sys_reset_300M/peripheral_aresetn] [get_bd_pins mipi_csi2_rx/video_aresetn]

connect_bd_net [get_bd_pins clk_wiz_0/clk_300M]                     [get_bd_pins axis_data_fifo_cap/s_axis_aclk]
connect_bd_net [get_bd_pins proc_sys_reset_300M/peripheral_aresetn] [get_bd_pins axis_data_fifo_cap/s_axis_aresetn]
connect_bd_net [get_bd_pins clk_wiz_0/clk_300M]                     [get_bd_pins axis_subset_converter_cap/aclk]
connect_bd_net [get_bd_pins proc_sys_reset_300M/peripheral_aresetn] [get_bd_pins axis_subset_converter_cap/aresetn]

connect_bd_net [get_bd_pins clk_wiz_0/clk_300M]                     [get_bd_pins v_frmbuf_wr/ap_clk]
connect_bd_net [get_bd_pins proc_sys_reset_300M/peripheral_aresetn] [get_bd_pins v_frmbuf_wr/ap_rst_n]

# PS HP1 + LPD HPM0 clocks
connect_bd_net [get_bd_pins clk_wiz_0/clk_300M] [get_bd_pins zynq_ps/saxihp1_fpd_aclk]
connect_bd_net [get_bd_pins clk_wiz_0/clk_100M] [get_bd_pins zynq_ps/maxihpm0_lpd_aclk]

# Camera data path: csi2 -> data_fifo -> subset_converter -> frmbuf_wr
connect_bd_intf_net [get_bd_intf_pins mipi_csi2_rx/video_out]              [get_bd_intf_pins axis_data_fifo_cap/S_AXIS]
connect_bd_intf_net [get_bd_intf_pins axis_data_fifo_cap/M_AXIS]           [get_bd_intf_pins axis_subset_converter_cap/S_AXIS]
connect_bd_intf_net [get_bd_intf_pins axis_subset_converter_cap/M_AXIS]    [get_bd_intf_pins v_frmbuf_wr/s_axis_video]

# DPHY pins from carrier interface
connect_bd_intf_net [get_bd_intf_ports mipi_phy_if]                        [get_bd_intf_pins mipi_csi2_rx/mipi_phy_if]

# Sensor IIC interface
connect_bd_intf_net [get_bd_intf_ports iic]                                [get_bd_intf_pins sensor_iic/IIC]

# ===== THE FIX =====
# AP1302 reset_b driven by PS EMIO GPIO bit 1 (= dts gpio 79 in standard
# zynqmp gpio numbering where MIO=0..77, EMIO=78..173).
# Smartcam slices bit 0 of a 92-bit EMIO_o; we use bit 1 to match
# the dts reset-gpios = <&gpio 79 1> property already in
# fpga/dts/system_top_wrapper.dts.
create_bd_cell -type ip -vlnv xilinx.com:ip:xlslice ap1302_rst_slice
set_property -dict [list \
    CONFIG.DIN_WIDTH  {2} \
    CONFIG.DIN_FROM   {1} \
    CONFIG.DIN_TO     {1} \
    CONFIG.DOUT_WIDTH {1} \
] [get_bd_cells ap1302_rst_slice]
connect_bd_net [get_bd_pins zynq_ps/emio_gpio_o]      [get_bd_pins ap1302_rst_slice/Din]
connect_bd_net [get_bd_pins ap1302_rst_slice/Dout]    [get_bd_ports ap1302_rst_b]
connect_bd_net [get_bd_pins ap1302_standby_const/dout] [get_bd_ports ap1302_standby]

# CSI-2 lite CSR + frmbuf CSR (PS LPD GP -> smartconnect_lpd)
# NUM_MI=2 (no debug_bridge - diagnostic-only, removed for slim build).
connect_bd_intf_net [get_bd_intf_pins zynq_ps/M_AXI_HPM0_LPD]  [get_bd_intf_pins smartconnect_lpd/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins smartconnect_lpd/M00_AXI] [get_bd_intf_pins mipi_csi2_rx/csirxss_s_axi]
connect_bd_intf_net [get_bd_intf_pins smartconnect_lpd/M01_AXI] [get_bd_intf_pins v_frmbuf_wr/s_axi_CTRL]

# frmbuf m_axi_mm_video -> smartconnect_video -> PS HP1
connect_bd_intf_net [get_bd_intf_pins v_frmbuf_wr/m_axi_mm_video] [get_bd_intf_pins smartconnect_video/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins smartconnect_video/M00_AXI] [get_bd_intf_pins zynq_ps/S_AXI_HP1_FPD]

# IRQs: 6-port concat for dts compat (In0..2 stubbed for absent T1/DMA).
# Camera dts expects iic at In3 (GIC SPI 92), csi2 at In4 (SPI 93),
# frmbuf at In5 (SPI 94).
create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant irq_const0
set_property -dict [list CONFIG.CONST_VAL {0} CONFIG.CONST_WIDTH {1}] [get_bd_cells irq_const0]

create_bd_cell -type ip -vlnv xilinx.com:ip:xlconcat:2.1 irq_concat
set_property CONFIG.NUM_PORTS {6} [get_bd_cells irq_concat]
connect_bd_net [get_bd_pins irq_const0/dout]              [get_bd_pins irq_concat/In0]
connect_bd_net [get_bd_pins irq_const0/dout]              [get_bd_pins irq_concat/In1]
connect_bd_net [get_bd_pins irq_const0/dout]              [get_bd_pins irq_concat/In2]
connect_bd_net [get_bd_pins sensor_iic/iic2intc_irpt]     [get_bd_pins irq_concat/In3]
connect_bd_net [get_bd_pins mipi_csi2_rx/csirxss_csi_irq] [get_bd_pins irq_concat/In4]
connect_bd_net [get_bd_pins v_frmbuf_wr/interrupt]        [get_bd_pins irq_concat/In5]
connect_bd_net [get_bd_pins irq_concat/dout]              [get_bd_pins zynq_ps/pl_ps_irq0]

# Address map
# mipi_csi2_rx CSR @ 0x80000000 (LPD aperture, matches dts isp_csiss)
assign_bd_address -target_address_space /zynq_ps/Data \
    [get_bd_addr_segs mipi_csi2_rx/csirxss_s_axi/Reg] -range 64K -offset 0x80000000

# v_frmbuf_wr CSR @ 0x80010000 (LPD aperture, matches 5o-era dts fb_wr)
assign_bd_address -target_address_space /zynq_ps/Data \
    [get_bd_addr_segs v_frmbuf_wr/s_axi_CTRL/Reg] -range 64K -offset 0x80010000

# sensor_iic @ 0xA0050000 (FPD aperture, matches dts axi_iic_sensor)
assign_bd_address -target_address_space /zynq_ps/Data \
    [get_bd_addr_segs sensor_iic/S_AXI/Reg] -range 64K -offset 0xA0050000


# frmbuf data -> DDR via HP1
assign_bd_address -target_address_space /v_frmbuf_wr/Data_m_axi_mm_video \
    [get_bd_addr_segs zynq_ps/SAXIGP3/HP1_DDR_LOW] -range 2G -offset 0x00000000

# Validate and save
regenerate_bd_layout
validate_bd_design

# Create HDL wrapper
make_wrapper -files [get_files ${bd_name}.bd] -top
set bd_wrapper [file join ${project_dir} \
    ${project_name}.gen sources_1 bd ${bd_name} hdl ${bd_name}_wrapper.v]
add_files -norecurse ${bd_wrapper}
update_compile_order -fileset sources_1
set_property top ${bd_name}_wrapper [current_fileset]

# AP1302 standby / rst_b pin constraints (J10 / J11 on KV260 IAS connector)
set xdc_pin [file join [file dirname [file normalize [info script]]] pin.xdc]
add_files -fileset constrs_1 -norecurse ${xdc_pin}

save_bd_design

puts ""
puts "========================================"
puts "Camtest block design created successfully"
puts "Project: ${project_dir}"
puts "BD top:  ${bd_name}_wrapper"
puts "Differences from system_top.tcl 5o:"
puts "  - REMOVED: T1, smartconnect_hb, smartconnect_idx, axi_dma,"
puts "             axi_register_slice (T1 hb), bram_ctrl, blk_mem_gen,"
puts "             axis_register_slice (DMA loopback)"
puts "  - KEPT:    mipi_csi2_rx, axis chain, v_frmbuf_wr, sensor_iic"
puts "             smartconnect_lpd, smartconnect_video, smartconnect_ctrl"
puts "  - CHANGED: ap1302_rst_b wired via PS EMIO GPIO bit 1 (was"
puts "             peripheral_aresetn). Matches smartcam pattern;"
puts "             dts gpio 79 now drives a real reset pin."
puts "Address map (smaller - T1/DMA segments gone):"
puts "  mipi_csi2_rx CSR @ 0x80000000 (64KB)"
puts "  v_frmbuf_wr CSR  @ 0x80010000 (64KB)"
puts "  sensor_iic       @ 0xA0050000 (64KB)"
puts "  frmbuf -> DDR    @ HP1, 2 GB"
puts "========================================"
