#Vivado Block Design TCL Script
#THis creates the fpga system top

#Current block design for KV260:
# - Zynq UltraScale+ PS (ARM Cortex-A53)
# - t1_fpga_top (Verilog module wrapping T1 + AXI Lite wrapper)
# - AXI SmartConnect (data movement: T1 AXI masters -> PS HP slave ports)
# - AXI DMA (for fetching DDR data to the PL fabric)

#Usage:
# vivado -mode batch -source system_top.tcl -tclargs <rtl_dir> <config_name> <row_number> <build_dir>
#Example:
# vivado -mode batch -source system_top.tcl -tclargs /test_output/mudkip2d64/rtl-20260403-125140/result mudkip2d64 64 /build_dir




if {$argc < 4} {
    puts "ERROR: Usage: system_top.tcl <rtl_dir> <config_name> <row_number> <build_dir>"
    puts " rtl_dir     - Path to the T1 RTL result directory"
    puts " config_name - T1 configuration name (e.g., mudkip2d64)"
    puts " row_number  - Number of 2D rows (determines AXI ID width)"
    puts " build_dir   - Build output directory (contains t1_fpga_top.v)"
    exit 1
}

set rtl_dir     [lindex $argv 0]
set config_name [lindex $argv 1]
set row_number  [lindex $argv 2]
set build_dir   [lindex $argv 3]

#derive AXI ID width: sourceWidth(2) + log2(rowNumber)
if {$row_number <= 1} {
    set axi_id_width 2
} else {
    set axi_id_width [expr {2 + int(ceil(log($row_number) / log(2)))}]
}

#log header
puts "========================================"
puts "T1 FPGA System Build"
puts "========================================"
puts "Config:       $config_name"
puts "RTL dir:      $rtl_dir"
puts "Row number:   $row_number"
puts "AXI ID width: $axi_id_width"
puts "========================================"

#project setup
set project_name "t1_${config_name}_system"
set project_dir [file dirname [file dirname [file normalize [info script]]]]/build/${project_name}
set wrapper_dir [file dirname [file normalize [info script]]]/../wrapper
set part "xck26-sfvc784-2LV-c"
set board "xilinx.com:kv260_som:part0:1.4"

#create project
create_project ${project_name} ${project_dir} -part ${part} -force
set_property board_part ${board} [current_project]

#Bind the SOM to the KV260 carrier (needed for som240_1_connector_mipi_csi_isp
#board interface used by mipi_csi2_rx_subsystem to route DPHY pins).
#kv260_carrier 1.3 ships in Vivado 2025.2 board store at
# Xilinx/2025.2/data/xhub/boards/XilinxBoardStore/boards/Xilinx/kv260_carrier/1.3
set_property board_connections {som240_1_connector xilinx.com:kv260_carrier:som240_1_connector:1.3} [current_project]

#RTL sources
# Add T1 RTL files (.sv) 
#reference to avoid copying the large chisel generated RTL filess
set rtl_files [glob -directory ${rtl_dir} *.sv]  
set synth_files {}
foreach f $rtl_files { 
    if {[string match "*/verification/*" $f] || [string match "*ref_T1*" $f]} {
        continue
    }
    lappend synth_files $f
}
add_files -norecurse ${synth_files} 
set_property file_type SystemVerilog [get_files *.sv]

# Add the generated Verilog wrapper t1_fpga_top.v
add_files -norecurse ${build_dir}/t1_fpga_top.v

# Add AXI Lite wrapper
add_files -norecurse ${wrapper_dir}/t1_axi_lite_wrapper.sv

update_compile_order -fileset sources_1



#Create block design
set bd_name "system_top"
create_bd_design ${bd_name}


#Zynq UltraScale+ PS
create_bd_cell -type ip -vlnv xilinx.com:ip:zynq_ultra_ps_e:3.5 zynq_ps

#Apply KV260 board preset (configures DDR, clocks, MIO)
apply_bd_automation -rule xilinx.com:bd_rule:zynq_ultra_ps_e \
    -config {apply_board_preset "1"} [get_bd_cells zynq_ps]

#Configure PS interfaces needed for T1 + camera capture
# Master ports: GP0 (FPD) for the 60 MHz control plane (T1 + DMA + IIC + frmbuf_CTRL),
#               LPD GP0 (MAXIGP2) for the 100 MHz CSI-2 lite control plane.
# Slave ports : HPC0/SAXIGP0 (T1 hb + DMA), HP0/SAXIGP2 (T1 idx 32-bit),
#               HP1/SAXIGP3 (frmbuf video master at 300 MHz, 128-bit).
# IRQ0 carries: T1 IRQ + DMA mm2s/s2mm + sensor_iic + csi2_irq + frmbuf_irq.
# pl_clk0 stays at 60 MHz (T1 clock); clk_wiz derives 100/200/300 MHz from it for camera.
set_property -dict [list \
    CONFIG.PSU__USE__M_AXI_GP0          {1} \
    CONFIG.PSU__USE__M_AXI_GP1          {0} \
    CONFIG.PSU__USE__M_AXI_GP2          {1} \
    CONFIG.PSU__USE__S_AXI_GP0          {1} \
    CONFIG.PSU__USE__S_AXI_GP2          {1} \
    CONFIG.PSU__USE__S_AXI_GP3          {1} \
    CONFIG.PSU__USE__IRQ0               {1} \
    CONFIG.PSU__MAXIGP0__DATA_WIDTH     {32} \
    CONFIG.PSU__MAXIGP2__DATA_WIDTH     {32} \
    CONFIG.PSU__SAXIGP0__DATA_WIDTH     {128} \
    CONFIG.PSU__SAXIGP2__DATA_WIDTH     {32} \
    CONFIG.PSU__SAXIGP3__DATA_WIDTH     {128} \
    CONFIG.PSU__CRL_APB__PL0_REF_CTRL__FREQMHZ {60} \
] [get_bd_cells zynq_ps]






#Individual Blocks are added below

#T1 FPGA Top (single Verilog module: T1 + AXI Lite Wrapper combined)
create_bd_cell -type module -reference t1_fpga_top t1_top


#AXI SmartConnect with highBandwidth port (T1 128-bit master -> PS HPC0 128-bit)
create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 smartconnect_hb
set_property -dict [list \
    CONFIG.NUM_SI           {1} \
    CONFIG.NUM_MI           {1} \
    CONFIG.NUM_CLKS         {1} \
] [get_bd_cells smartconnect_hb]


#AXI SmartConnect with indexed port (T1 32-bit master -> PS HP0 32-bit)
create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 smartconnect_idx
set_property -dict [list \
    CONFIG.NUM_SI           {1} \
    CONFIG.NUM_MI           {1} \
    CONFIG.NUM_CLKS         {1} \
] [get_bd_cells smartconnect_idx]


#AXI DMA (for ARM-initiated data transfers to/from DDR)
create_bd_cell -type ip -vlnv xilinx.com:ip:axi_dma:7.1 axi_dma
set_property -dict [list \
    CONFIG.c_include_sg         {0} \
    CONFIG.c_sg_include_stscntrl_strm {0} \
    CONFIG.c_include_mm2s       {1} \
    CONFIG.c_include_s2mm       {1} \
    CONFIG.c_m_axi_mm2s_data_width {128} \
    CONFIG.c_m_axi_s2mm_data_width {128} \
    CONFIG.c_mm2s_burst_size    {16} \
    CONFIG.c_s2mm_burst_size    {16} \
] [get_bd_cells axi_dma]


#AXI Interconnect for GP0 -> AXI Lite wrapper + DMA control
create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 smartconnect_ctrl
set_property -dict [list \
    CONFIG.NUM_SI   {1} \
    CONFIG.NUM_MI   {2} \
    CONFIG.NUM_CLKS {1} \
] [get_bd_cells smartconnect_ctrl]








#Connection for CLK and RESET pins for design blocks below
#Processor System Reset
create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 proc_sys_reset

#Proc sys reset
connect_bd_net [get_bd_pins zynq_ps/pl_clk0]     [get_bd_pins proc_sys_reset/slowest_sync_clk]
connect_bd_net [get_bd_pins zynq_ps/pl_resetn0]   [get_bd_pins proc_sys_reset/ext_reset_in]

#t1_fpga_top uses active low aclk/aresetn with reset inversion inside the wrapper
connect_bd_net [get_bd_pins zynq_ps/pl_clk0]     [get_bd_pins t1_top/aclk]
connect_bd_net [get_bd_pins proc_sys_reset/peripheral_aresetn] [get_bd_pins t1_top/aresetn]

#SmartConnects
connect_bd_net [get_bd_pins zynq_ps/pl_clk0]     [get_bd_pins smartconnect_hb/aclk]
connect_bd_net [get_bd_pins proc_sys_reset/interconnect_aresetn] [get_bd_pins smartconnect_hb/aresetn]
connect_bd_net [get_bd_pins zynq_ps/pl_clk0]     [get_bd_pins smartconnect_idx/aclk]
connect_bd_net [get_bd_pins proc_sys_reset/interconnect_aresetn] [get_bd_pins smartconnect_idx/aresetn]
connect_bd_net [get_bd_pins zynq_ps/pl_clk0]     [get_bd_pins smartconnect_ctrl/aclk]
connect_bd_net [get_bd_pins proc_sys_reset/interconnect_aresetn] [get_bd_pins smartconnect_ctrl/aresetn]

#DMA
connect_bd_net [get_bd_pins zynq_ps/pl_clk0]     [get_bd_pins axi_dma/s_axi_lite_aclk]
connect_bd_net [get_bd_pins zynq_ps/pl_clk0]     [get_bd_pins axi_dma/m_axi_mm2s_aclk]
connect_bd_net [get_bd_pins zynq_ps/pl_clk0]     [get_bd_pins axi_dma/m_axi_s2mm_aclk]
connect_bd_net [get_bd_pins proc_sys_reset/peripheral_aresetn] [get_bd_pins axi_dma/axi_resetn]

#PS AXI clocks
connect_bd_net [get_bd_pins zynq_ps/pl_clk0]     [get_bd_pins zynq_ps/maxihpm0_fpd_aclk]
connect_bd_net [get_bd_pins zynq_ps/pl_clk0]     [get_bd_pins zynq_ps/saxihpc0_fpd_aclk]
connect_bd_net [get_bd_pins zynq_ps/pl_clk0]     [get_bd_pins zynq_ps/saxihp0_fpd_aclk]







#Control Plane: PS GP0 -> SmartConnect_ctrl -> AXI Lite Wrapper + DMA
#PS GP0 master -> control interconnect
connect_bd_intf_net [get_bd_intf_pins zynq_ps/M_AXI_HPM0_FPD] \
                    [get_bd_intf_pins smartconnect_ctrl/S00_AXI]

#Control interconnect -> t1_fpga_top AXI Lite slave (s_axi_ctrl)
connect_bd_intf_net [get_bd_intf_pins smartconnect_ctrl/M00_AXI] \
                    [get_bd_intf_pins t1_top/s_axi_ctrl]

#Control interconnect -> DMA control registers
connect_bd_intf_net [get_bd_intf_pins smartconnect_ctrl/M01_AXI] \
                    [get_bd_intf_pins axi_dma/S_AXI_LITE]








#Data Plane: t1_fpga_top AXI Masters -> SmartConnect -> PS HP Slave Ports
#highBandwidth port (128-bit data) -> SmartConnect -> PS HPC0
connect_bd_intf_net [get_bd_intf_pins t1_top/m_axi_hb] \
                    [get_bd_intf_pins smartconnect_hb/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins smartconnect_hb/M00_AXI] \
                    [get_bd_intf_pins zynq_ps/S_AXI_HPC0_FPD]

#indexed port (32-bit data) -> SmartConnect -> PS HP0
connect_bd_intf_net [get_bd_intf_pins t1_top/m_axi_idx] \
                    [get_bd_intf_pins smartconnect_idx/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins smartconnect_idx/M00_AXI] \
                    [get_bd_intf_pins zynq_ps/S_AXI_HP0_FPD]







#Interrupt: wrapper IRQ -> PS PL interrupt
#Concat DMA interrupts + wrapper IRQ
create_bd_cell -type ip -vlnv xilinx.com:ip:xlconcat:2.1 irq_concat
set_property CONFIG.NUM_PORTS {3} [get_bd_cells irq_concat]

connect_bd_net [get_bd_pins t1_top/irq]            [get_bd_pins irq_concat/In0]
connect_bd_net [get_bd_pins axi_dma/mm2s_introut]   [get_bd_pins irq_concat/In1]
connect_bd_net [get_bd_pins axi_dma/s2mm_introut]   [get_bd_pins irq_concat/In2]
connect_bd_net [get_bd_pins irq_concat/dout]         [get_bd_pins zynq_ps/pl_ps_irq0]







#========================================================================
# Phase B: streaming-pipeline additions (2026-05-06)
#
# Pipeline scope (mirrors kv260_ispMipiRx_vcu_DP capture_pipeline, minus
# the VCU and audio_ss subsystems):
#   AR1335 -> AP1302 (does crop+downsample to 128x128 in ISP)
#          -> CSI-2 RX subsystem
#          -> axis_data_fifo (rate-match)
#          -> axis_subset_converter (data-width remap)
#          -> v_frmbuf_wr -> DDR (via PS HP1)
#
# Display goes through PS DisplayPort fed from DDR; no PL HDMI/DP IP
# is needed (the carrier handles DP -> HDMI conversion externally).
#
# AP1302 control:
#   - axi_iic at 0xA0050000 (PS GP0 control plane, 60 MHz)
#   - ap1302_standby tied to xlconstant 0 (never standby)
#   - ap1302_rst_b driven from peripheral_aresetn (releases when
#     bitstream loaded; AP1302 driver can also issue soft-reset over
#     I2C for V4L2 probe sequences)
#
# Scratchpad:
#   - axi_bram_ctrl + blk_mem_gen, 32 KB at 0xB0000000, T1 idx side.
#
# Clocking (clk_wiz_0 derives from pl_clk0=60 MHz):
#   - clk_100M : CSI-2 lite_aclk (PS LPD GP control plane)
#   - clk_200M : DPHY clock (CSI-2 IDELAY-controlled D-PHY input)
#   - clk_300M : video AXI (CSI-2 video_aclk, frmbuf ap_clk,
#                frmbuf m_axi_mm_video -> PS HP1)
#========================================================================

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

#----- BRAM scratchpad: 32 KB at 0xB0000000, T1 idx side -----------------
create_bd_cell -type ip -vlnv xilinx.com:ip:axi_bram_ctrl:4.1 bram_ctrl
set_property -dict [list \
    CONFIG.SINGLE_PORT_BRAM      {1} \
    CONFIG.DATA_WIDTH            {128} \
    CONFIG.MEM_DEPTH             {2048} \
] [get_bd_cells bram_ctrl]
create_bd_cell -type ip -vlnv xilinx.com:ip:blk_mem_gen:8.4 bram
set_property -dict [list \
    CONFIG.Memory_Type           {Single_Port_RAM} \
    CONFIG.Use_Byte_Write_Enable {true} \
    CONFIG.Byte_Size             {8} \
    CONFIG.Write_Width_A         {128} \
    CONFIG.Write_Depth_A         {2048} \
    CONFIG.Read_Width_A          {128} \
] [get_bd_cells bram]
connect_bd_net [get_bd_pins zynq_ps/pl_clk0]                   [get_bd_pins bram_ctrl/s_axi_aclk]
connect_bd_net [get_bd_pins proc_sys_reset/peripheral_aresetn] [get_bd_pins bram_ctrl/s_axi_aresetn]
connect_bd_intf_net [get_bd_intf_pins bram_ctrl/BRAM_PORTA] [get_bd_intf_pins bram/BRAM_PORTA]

#----- Camera capture pipeline IPs (300 MHz video, 100 MHz lite, 200 MHz DPHY) -----
# CONFIG dicts copied verbatim from kria-vitis-platforms /
# kv260_ispMipiRx_vcu_DP / scripts / config_bd.tcl :: create_hier_cell_capture_pipeline,
# except v_frmbuf_wr resolution is reduced from 3840x2160 to 1920x1080 since
# AP1302 ISP downsamples to <=128x128 before the FPGA sees pixels (we keep
# 1920x1080 max as headroom for higher-res preview during bringup).

create_bd_cell -type ip -vlnv xilinx.com:ip:mipi_csi2_rx_subsystem mipi_csi2_rx
set_property -dict [list \
    CONFIG.DPHYRX_BOARD_INTERFACE        {som240_1_connector_mipi_csi_isp} \
    CONFIG.CMN_NUM_PIXELS                {2} \
    CONFIG.CMN_VC                        {0} \
    CONFIG.CSI_BUF_DEPTH                 {4096} \
    CONFIG.C_CSI_FILTER_USERDATATYPE     {true} \
    CONFIG.C_HS_LINE_RATE                {896} \
    CONFIG.C_HS_SETTLE_NS                {146} \
    CONFIG.DPY_LINE_RATE                 {896} \
    CONFIG.SupportLevel                  {1} \
] [get_bd_cells mipi_csi2_rx]

create_bd_cell -type ip -vlnv xilinx.com:ip:axis_data_fifo axis_data_fifo_cap
set_property -dict [list CONFIG.FIFO_DEPTH {1024}] [get_bd_cells axis_data_fifo_cap]

create_bd_cell -type ip -vlnv xilinx.com:ip:axis_subset_converter axis_subset_converter_cap
set_property -dict [list \
    CONFIG.M_TDATA_NUM_BYTES {6} \
    CONFIG.M_TDEST_WIDTH     {1} \
    CONFIG.S_TDATA_NUM_BYTES {4} \
    CONFIG.S_TDEST_WIDTH     {10} \
    CONFIG.TDATA_REMAP       {16'b00000000,tdata[31:0]} \
    CONFIG.TDEST_REMAP       {tdest[0:0]} \
] [get_bd_cells axis_subset_converter_cap]

create_bd_cell -type ip -vlnv xilinx.com:ip:v_frmbuf_wr v_frmbuf_wr
set_property -dict [list \
    CONFIG.AXIMM_DATA_WIDTH               {128} \
    CONFIG.C_M_AXI_MM_VIDEO_DATA_WIDTH    {128} \
    CONFIG.HAS_BGR8                       {0} \
    CONFIG.HAS_BGRX8                      {0} \
    CONFIG.HAS_RGB8                       {1} \
    CONFIG.HAS_UYVY8                      {1} \
    CONFIG.HAS_YUV8                       {0} \
    CONFIG.HAS_Y8                         {1} \
    CONFIG.HAS_Y_UV8                      {0} \
    CONFIG.HAS_Y_UV8_420                  {1} \
    CONFIG.MAX_COLS                       {1920} \
    CONFIG.MAX_ROWS                       {1080} \
    CONFIG.MAX_NR_PLANES                  {2} \
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

#----- Extend existing SmartConnects ------------------------------------
# smartconnect_ctrl: NUM_CLKS=2 (60 MHz primary + 300 MHz secondary).
#   M00 t1_top/s_axi_ctrl    (60 MHz)
#   M01 axi_dma/S_AXI_LITE   (60 MHz)
#   M02 sensor_iic/S_AXI     (60 MHz)
#   M03 v_frmbuf_wr/s_axi_CTRL (300 MHz, runs at same clock as frmbuf
#       ap_clk - the HLS-based IP shares ap_clk between control bus and
#       data plane). SmartConnect handles the CDC across aclk/aclk1.
set_property -dict [list CONFIG.NUM_MI {4} CONFIG.NUM_CLKS {2}] [get_bd_cells smartconnect_ctrl]
connect_bd_net [get_bd_pins clk_wiz_0/clk_300M] [get_bd_pins smartconnect_ctrl/aclk1]

# smartconnect_idx: NUM_MI 1 -> 2 (T1 idx -> {HP0, scratchpad})
set_property CONFIG.NUM_MI {2} [get_bd_cells smartconnect_idx]

# smartconnect_hb: NUM_SI 1 -> 3 (T1 hb + DMA mm2s + DMA s2mm) -> HPC0
set_property CONFIG.NUM_SI {3} [get_bd_cells smartconnect_hb]

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

#----- Existing 60 MHz control plane: extended for sensor_iic + frmbuf CTRL -----
connect_bd_intf_net [get_bd_intf_pins smartconnect_ctrl/M02_AXI]           [get_bd_intf_pins sensor_iic/S_AXI]
connect_bd_intf_net [get_bd_intf_pins smartconnect_ctrl/M03_AXI]           [get_bd_intf_pins v_frmbuf_wr/s_axi_CTRL]

# T1 idx 2nd master -> BRAM scratchpad
connect_bd_intf_net [get_bd_intf_pins smartconnect_idx/M01_AXI]            [get_bd_intf_pins bram_ctrl/S_AXI]

# DMA masters: previously unwired in the BD; route through smartconnect_hb
# alongside T1 hb to reach PS HPC0 / DDR.
connect_bd_intf_net [get_bd_intf_pins axi_dma/M_AXI_MM2S]                  [get_bd_intf_pins smartconnect_hb/S01_AXI]
connect_bd_intf_net [get_bd_intf_pins axi_dma/M_AXI_S2MM]                  [get_bd_intf_pins smartconnect_hb/S02_AXI]

#----- IRQs: extend irq_concat from 3 -> 6 ports -------------------------
# Was: In0=t1, In1=mm2s, In2=s2mm.
# Now: In0=t1, In1=mm2s, In2=s2mm, In3=sensor_iic, In4=csi2, In5=frmbuf_wr.
set_property CONFIG.NUM_PORTS {6} [get_bd_cells irq_concat]
connect_bd_net [get_bd_pins sensor_iic/iic2intc_irpt]   [get_bd_pins irq_concat/In3]
connect_bd_net [get_bd_pins mipi_csi2_rx/csirxss_csi_irq] [get_bd_pins irq_concat/In4]
connect_bd_net [get_bd_pins v_frmbuf_wr/interrupt]      [get_bd_pins irq_concat/In5]

#Address Map
#AXI Lite wrapper registers, currently 64KB range and only 128 bytes used)
assign_bd_address -target_address_space /zynq_ps/Data \
    [get_bd_addr_segs t1_top/s_axi_ctrl/reg0] -range 64K -offset 0xA0000000

#DMA control registers
assign_bd_address -target_address_space /zynq_ps/Data \
    [get_bd_addr_segs axi_dma/S_AXI_LITE/Reg] -range 64K -offset 0xA0010000

#v_frmbuf_wr CSR (60 MHz control plane via smartconnect_ctrl/M03)
assign_bd_address -target_address_space /zynq_ps/Data \
    [get_bd_addr_segs v_frmbuf_wr/s_axi_CTRL/Reg] -range 64K -offset 0xA0020000

#mipi_csi2_rx CSR (100 MHz LPD control plane via smartconnect_lpd).
#PS LPD GP master can only reach 0x8000_0000-0x9FFF_FFFF apertures
#(distinct from the FPD GP aperture at 0xA000_0000+). 0x80000000 matches
#the reference platform address.
assign_bd_address -target_address_space /zynq_ps/Data \
    [get_bd_addr_segs mipi_csi2_rx/csirxss_s_axi/Reg] -range 64K -offset 0x80000000

#AP1302 IIC control (60 MHz control plane via smartconnect_ctrl/M02)
assign_bd_address -target_address_space /zynq_ps/Data \
    [get_bd_addr_segs sensor_iic/S_AXI/Reg] -range 64K -offset 0xA0050000

#T1 highBandwidth port -> full DDR range (via HPC0)
assign_bd_address -target_address_space /t1_top/m_axi_hb \
    [get_bd_addr_segs zynq_ps/SAXIGP0/HPC0_DDR_LOW] -range 2G -offset 0x00000000

#T1 indexed port -> full DDR range (via HP0)
assign_bd_address -target_address_space /t1_top/m_axi_idx \
    [get_bd_addr_segs zynq_ps/SAXIGP2/HP0_DDR_LOW] -range 2G -offset 0x00000000

#T1 indexed port -> 32KB scratchpad
assign_bd_address -target_address_space /t1_top/m_axi_idx \
    [get_bd_addr_segs bram_ctrl/S_AXI/Mem0] -range 32K -offset 0xB0000000

#DMA MM2S / S2MM -> full DDR range (via HPC0 too, sharing with T1 hb)
assign_bd_address -target_address_space /axi_dma/Data_MM2S \
    [get_bd_addr_segs zynq_ps/SAXIGP0/HPC0_DDR_LOW] -range 2G -offset 0x00000000
assign_bd_address -target_address_space /axi_dma/Data_S2MM \
    [get_bd_addr_segs zynq_ps/SAXIGP0/HPC0_DDR_LOW] -range 2G -offset 0x00000000

#v_frmbuf_wr m_axi_mm_video -> DDR via HP1 (full 2GB DDR-low range)
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

save_bd_design

puts ""
puts "========================================"
puts "Block design created successfully!!"
puts "Project: ${project_dir}"
puts "Address map:"
puts "  AXI Lite Wrapper:  0xA0000000 (64KB)"
puts "  DMA Control:       0xA0010000 (64KB)"
puts "  v_frmbuf_wr CSR:   0xA0020000 (64KB)"
puts "  mipi_csi2_rx CSR:  0x80000000 (64KB)  via PS LPD"
puts "  AP1302 IIC:        0xA0050000 (64KB)"
puts "  T1 HPC0 -> DDR:    0x00000000 (2GB)"
puts "  T1 HP0  -> DDR:    0x00000000 (2GB)"
puts "  T1 idx -> BRAM:    0xB0000000 (32KB)"
puts "  DMA -> DDR:        0x00000000 (2GB) via HPC0"
puts "  Frmbuf -> DDR:     0x00000000 (2GB) via HP1"
puts "========================================"
