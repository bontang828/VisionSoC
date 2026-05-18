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

# 5r URAM scratchpad backing store: xpm_memory_spram with
# MEMORY_PRIMITIVE="ultra" wrapped in BRAM_PORTA-compatible pins so the
# axi_bram_ctrl drives it the same way it drove blk_mem_gen. We use the
# XPM path because emb_mem_gen 1.0 (which would let us swap by IP only
# with CONFIG.MEMORY_PRIMITIVE=URAM) is in the Vivado catalog but not
# supported for the Zynq UltraScale+ part family. XPM is supported on
# ZU+. See fpga_build_status.md § 0.11.
add_files -norecurse ${wrapper_dir}/uram_scratchpad.v

# MaskUnitFpga v0 mask shadow: xpm_memory_sdpram, 128 x 1024-bit, byte-
# write enable, simple dual-port. Replaces the FF-replicated v0Vec in
# MaskUnit at vLen=1024 (~131k FFs, ~15k LUTs of row-decode). Active only
# when the elaborator was invoked with --useFpgaMaskUnit true (the
# _fpga_maskopt config family); other configs do not instance v0_bram.
# Sim path uses t1/resources/v0_bram.sv (behavioural model) via Chisel
# HasBlackBoxResource. See fpga_build_status.md § 0.13 and
# maskunit_fpga_handoff.md.
add_files -norecurse ${wrapper_dir}/v0_bram.v

update_compile_order -fileset sources_1



#Create block design
set bd_name "system_top"
create_bd_design ${bd_name}


#Zynq UltraScale+ PS
create_bd_cell -type ip -vlnv xilinx.com:ip:zynq_ultra_ps_e:3.5 zynq_ps

#Apply KV260 board preset (configures DDR, clocks, MIO)
apply_bd_automation -rule xilinx.com:bd_rule:zynq_ultra_ps_e \
    -config {apply_board_preset "1"} [get_bd_cells zynq_ps]

#Configure PS interfaces needed for T1 + camera capture (camera restored 2026-05-09)
# Master ports: GP0 (FPD) for the 60 MHz control plane (T1 + DMA + IIC),
#               LPD GP0 (MAXIGP2) for the 100 MHz CSI-2 lite control plane.
# Slave ports : HPC0/SAXIGP0 (T1 hb + DMA), HP0/SAXIGP2 (T1 idx 32-bit),
#               HP1/SAXIGP3 (frmbuf video master at 300 MHz, 128-bit).
# IRQ0 carries: T1 IRQ + DMA mm2s/s2mm + sensor_iic + csi2_irq.
# Both MAXIGP0 and MAXIGP2 must be 128-bit (Fix 2 from § 6.1.5) - at
# 32-bit, the PS internal NIC-400 downsizer mishandles AXI4-Lite read
# response repacking and only the 16-byte-aligned word lane returns
# correct data. Setting both to 128 bypasses the downsizer entirely.
set_property -dict [list \
    CONFIG.PSU__USE__M_AXI_GP0          {1} \
    CONFIG.PSU__USE__M_AXI_GP1          {0} \
    CONFIG.PSU__USE__M_AXI_GP2          {1} \
    CONFIG.PSU__USE__S_AXI_GP0          {1} \
    CONFIG.PSU__USE__S_AXI_GP2          {1} \
    CONFIG.PSU__USE__S_AXI_GP3          {1} \
    CONFIG.PSU__USE__IRQ0               {1} \
    CONFIG.PSU__MAXIGP0__DATA_WIDTH     {128} \
    CONFIG.PSU__MAXIGP2__DATA_WIDTH     {128} \
    CONFIG.PSU__SAXIGP0__DATA_WIDTH     {128} \
    CONFIG.PSU__SAXIGP2__DATA_WIDTH     {32} \
    CONFIG.PSU__SAXIGP3__DATA_WIDTH     {128} \
    CONFIG.PSU__CRL_APB__PL0_REF_CTRL__FREQMHZ {100} \
    CONFIG.PSU__GPIO_EMIO__PERIPHERAL__ENABLE  {1} \
    CONFIG.PSU__GPIO_EMIO__PERIPHERAL__IO      {2} \
] [get_bd_cells zynq_ps]
# 2026-05-14 (5q final): pl_clk0 = 100 MHz (was 60 MHz). DPHY FVCO
# constraint requires 50 or 100; 100 gives more T1 timing headroom via
# clk_wiz_0/clk_60M. EMIO width 2 enables emio_gpio_o[1] -> ap1302_rst_b
# wiring (matches camtest3 v8 / camtest4 pattern).






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
    CONFIG.c_sg_length_width    {23} \
] [get_bd_cells axi_dma]


#AXI Interconnect for GP0 -> AXI Lite wrapper + DMA control
create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 smartconnect_ctrl
set_property -dict [list \
    CONFIG.NUM_SI   {1} \
    CONFIG.NUM_MI   {2} \
    CONFIG.NUM_CLKS {1} \
] [get_bd_cells smartconnect_ctrl]








#----- clk_wiz: derive 60/100/200/300 MHz from pl_clk0 (=100 MHz) -------
# Moved up from § Phase B / camera capture (was line ~340) because T1 now
# needs clk_60M derived from clk_wiz; proc_sys_reset below wires to
# clk_60M and must therefore reference clk_wiz_0 first.
create_bd_cell -type ip -vlnv xilinx.com:ip:clk_wiz clk_wiz_0
set_property -dict [list \
    CONFIG.PRIM_SOURCE                {Global_buffer} \
    CONFIG.RESET_PORT                 {resetn} \
    CONFIG.RESET_TYPE                 {ACTIVE_LOW} \
    CONFIG.NUM_OUT_CLKS               {4} \
    CONFIG.CLK_OUT1_PORT              {clk_100M} \
    CONFIG.CLKOUT1_REQUESTED_OUT_FREQ {100.000} \
    CONFIG.CLK_OUT2_PORT              {clk_200M} \
    CONFIG.CLKOUT2_USED               {true} \
    CONFIG.CLKOUT2_REQUESTED_OUT_FREQ {200.000} \
    CONFIG.CLK_OUT3_PORT              {clk_300M} \
    CONFIG.CLKOUT3_USED               {true} \
    CONFIG.CLKOUT3_REQUESTED_OUT_FREQ {300.000} \
    CONFIG.CLK_OUT4_PORT              {clk_60M} \
    CONFIG.CLKOUT4_USED               {true} \
    CONFIG.CLKOUT4_REQUESTED_OUT_FREQ {60.000} \
] [get_bd_cells clk_wiz_0]
connect_bd_net [get_bd_pins zynq_ps/pl_clk0]    [get_bd_pins clk_wiz_0/clk_in1]
connect_bd_net [get_bd_pins zynq_ps/pl_resetn0] [get_bd_pins clk_wiz_0/resetn]

#proc_sys_reset for the 100 MHz and 300 MHz domains (camera side)
create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 proc_sys_reset_100M
connect_bd_net [get_bd_pins clk_wiz_0/clk_100M]  [get_bd_pins proc_sys_reset_100M/slowest_sync_clk]
connect_bd_net [get_bd_pins zynq_ps/pl_resetn0]  [get_bd_pins proc_sys_reset_100M/ext_reset_in]
connect_bd_net [get_bd_pins clk_wiz_0/locked]    [get_bd_pins proc_sys_reset_100M/dcm_locked]

create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 proc_sys_reset_300M
connect_bd_net [get_bd_pins clk_wiz_0/clk_300M]  [get_bd_pins proc_sys_reset_300M/slowest_sync_clk]
connect_bd_net [get_bd_pins zynq_ps/pl_resetn0]  [get_bd_pins proc_sys_reset_300M/ext_reset_in]
connect_bd_net [get_bd_pins clk_wiz_0/locked]    [get_bd_pins proc_sys_reset_300M/dcm_locked]

#Connection for CLK and RESET pins for design blocks below
#Processor System Reset (T1 domain, 60 MHz from clk_wiz_0/clk_60M)
# 2026-05-14: T1 was previously on pl_clk0; pl_clk0 is now 100 MHz for DPHY.
# T1 is validated at 60 MHz, so we derive a 60 MHz from clk_wiz_0 (see
# above, NUM_OUT_CLKS=4). proc_sys_reset's dcm_locked is wired so T1 stays
# in reset until clk_wiz locks (otherwise t1_top sees a clock glitch).
create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 proc_sys_reset

#Proc sys reset - driven by clk_wiz_0/clk_60M, gated by clk_wiz_0/locked
connect_bd_net [get_bd_pins clk_wiz_0/clk_60M]   [get_bd_pins proc_sys_reset/slowest_sync_clk]
connect_bd_net [get_bd_pins zynq_ps/pl_resetn0]   [get_bd_pins proc_sys_reset/ext_reset_in]
connect_bd_net [get_bd_pins clk_wiz_0/locked]    [get_bd_pins proc_sys_reset/dcm_locked]

#t1_fpga_top uses active low aclk/aresetn with reset inversion inside the wrapper
connect_bd_net [get_bd_pins clk_wiz_0/clk_60M]   [get_bd_pins t1_top/aclk]
connect_bd_net [get_bd_pins proc_sys_reset/peripheral_aresetn] [get_bd_pins t1_top/aresetn]

#SmartConnects (T1 + DMA control plane on 60 MHz)
connect_bd_net [get_bd_pins clk_wiz_0/clk_60M]   [get_bd_pins smartconnect_hb/aclk]
connect_bd_net [get_bd_pins proc_sys_reset/interconnect_aresetn] [get_bd_pins smartconnect_hb/aresetn]
connect_bd_net [get_bd_pins clk_wiz_0/clk_60M]   [get_bd_pins smartconnect_idx/aclk]
connect_bd_net [get_bd_pins proc_sys_reset/interconnect_aresetn] [get_bd_pins smartconnect_idx/aresetn]
connect_bd_net [get_bd_pins clk_wiz_0/clk_60M]   [get_bd_pins smartconnect_ctrl/aclk]
connect_bd_net [get_bd_pins proc_sys_reset/interconnect_aresetn] [get_bd_pins smartconnect_ctrl/aresetn]

#DMA (60 MHz, alongside T1)
connect_bd_net [get_bd_pins clk_wiz_0/clk_60M]   [get_bd_pins axi_dma/s_axi_lite_aclk]
connect_bd_net [get_bd_pins clk_wiz_0/clk_60M]   [get_bd_pins axi_dma/m_axi_mm2s_aclk]
connect_bd_net [get_bd_pins clk_wiz_0/clk_60M]   [get_bd_pins axi_dma/m_axi_s2mm_aclk]
connect_bd_net [get_bd_pins proc_sys_reset/peripheral_aresetn] [get_bd_pins axi_dma/axi_resetn]

#PS AXI clocks (60 MHz for T1/DMA-facing HP ports)
connect_bd_net [get_bd_pins clk_wiz_0/clk_60M]   [get_bd_pins zynq_ps/maxihpm0_fpd_aclk]
connect_bd_net [get_bd_pins clk_wiz_0/clk_60M]   [get_bd_pins zynq_ps/saxihpc0_fpd_aclk]
connect_bd_net [get_bd_pins clk_wiz_0/clk_60M]   [get_bd_pins zynq_ps/saxihp0_fpd_aclk]







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
# F7 (5m): insert axi_register_slice between T1 hb and smartconnect_hb/S00.
# Hypothesis: 5l's port_grid_vadd_scratchpad kernel panic comes from T1's
# vle-from-BRAM tagging v12 with side-band metadata (AxUSER/AxID) that
# leaks into the subsequent vse-to-DDR via HPC0, where the PS-side cache
# coherency walk hits the unexpected metadata and SErrors. The register
# slice provides timing isolation AND zeros out cross-transaction state
# that might propagate from M01 to M00.
create_bd_cell -type ip -vlnv xilinx.com:ip:axi_register_slice axi_reg_slice_hb
set_property -dict [list \
    CONFIG.REG_AW {15} \
    CONFIG.REG_AR {15} \
    CONFIG.REG_W  {15} \
    CONFIG.REG_R  {15} \
    CONFIG.REG_B  {15} \
] [get_bd_cells axi_reg_slice_hb]
connect_bd_net [get_bd_pins clk_wiz_0/clk_60M]                 [get_bd_pins axi_reg_slice_hb/aclk]
connect_bd_net [get_bd_pins proc_sys_reset/peripheral_aresetn] [get_bd_pins axi_reg_slice_hb/aresetn]

#highBandwidth port (128-bit data) -> AXI register slice -> SmartConnect -> PS HPC0
connect_bd_intf_net [get_bd_intf_pins t1_top/m_axi_hb] \
                    [get_bd_intf_pins axi_reg_slice_hb/S_AXI]
connect_bd_intf_net [get_bd_intf_pins axi_reg_slice_hb/M_AXI] \
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
# Phase B: streaming-pipeline additions (camera capture)
#
# Pipeline scope (mirrors kv260_ispMipiRx_vcu_DP capture_pipeline, minus
# the VCU and audio_ss subsystems):
#   AR1335 -> AP1302 (does crop+downsample to 128x128 in ISP)
#          -> CSI-2 RX subsystem (mipi_csi2_rx)
#          -> axis_data_fifo_cap (rate-match)
#          -> axis_subset_converter_cap (data-width remap)
#          -> v_frmbuf_wr -> DDR (via PS HP1)
#
# Display goes through PS DisplayPort fed from DDR; no PL HDMI/DP IP
# is needed (the carrier handles DP -> HDMI conversion externally).
#
# AP1302 control:
#   - axi_iic at 0xA0050000 (PS GP0 control plane, 60 MHz)
#   - ap1302_standby tied to xlconstant 0 (never standby)
#   - ap1302_rst_b driven from peripheral_aresetn
#
# Scratchpad:
#   - axi_bram_ctrl + blk_mem_gen, 32 KB at 0xB0000000, on smartconnect_hb
#     (F4 / 5l: shared by T1 hb + axi_dma both channels - single-cycle
#     128-bit BRAM access for T1, plus DDR<->BRAM prefetch via DMA's
#     M_AXIS_MM2S->S_AXIS_S2MM loopback).
#
# Clocking (clk_wiz_0 derives from pl_clk0 = 60 MHz):
#   - clk_100M : CSI-2 lite_aclk (PS LPD GP control plane)
#   - clk_200M : DPHY clock (CSI-2 IDELAY-controlled D-PHY input)
#   - clk_300M : video AXI (CSI-2 video_aclk, frmbuf ap_clk,
#                frmbuf m_axi_mm_video -> PS HP1)
#========================================================================

#----- AXI IIC for AP1302 sensor (60 MHz, T1 control-plane domain) -------
create_bd_cell -type ip -vlnv xilinx.com:ip:axi_iic:2.1 sensor_iic
set_property -dict [list \
    CONFIG.IIC_FREQ_KHZ          {400} \
    CONFIG.IIC_BOARD_INTERFACE   {som240_1_connector_hda_iic_switch} \
    CONFIG.USE_BOARD_FLOW        {true} \
] [get_bd_cells sensor_iic]
connect_bd_net [get_bd_pins clk_wiz_0/clk_60M]                 [get_bd_pins sensor_iic/s_axi_aclk]
connect_bd_net [get_bd_pins proc_sys_reset/peripheral_aresetn] [get_bd_pins sensor_iic/s_axi_aresetn]

#----- BRAM scratchpad: 32 KB at 0xA0080000, single-controller (F4 / 5m) -----
# F4 (5l, kept in 5m): single 128-bit axi_bram_ctrl on smartconnect_hb/M01.
# Reachable from T1 hb + DMA mm2s + DMA s2mm. F6 (PS direct mmio access)
# is DEFERRED in 5m because both attempted topologies overflowed the LUT
# budget:
#   * Two-bram_ctrl + true-dual-port blk_mem_gen: 93.18% synth util
#   * Single bram_ctrl + smartconnect_bram_arb fanout:  96.51% synth util
# Both are over the ~88.5% routing cliff. Scratchpad is now T1+DMA only;
# PS-side validation will use a DMA round-trip pattern (PS->DDR->DMA->BRAM,
# T1 compute, BRAM->DMA->DDR->PS) rather than direct uio6 mmap of BRAM.
#
# Scratchpad relocation 0xB0000000 -> 0xA0080000 kept anyway: it's
# forward-compatible with a future F6 (when we have LUT headroom) and
# means T1's view of the address never changes between bitstream revs.
# 5r: scratchpad backing memory is now UltraRAM via xpm_memory_spram
# (see fpga/wrapper/uram_scratchpad.v). The legacy blk_mem_gen 8.4 IP
# is BRAM-only; emb_mem_gen (the URAM-capable successor with a
# CONFIG.MEMORY_PRIMITIVE keyword) was the cleanest swap on paper but
# its VLNV is not supported for Zynq UltraScale+ in Vivado 2025.2.
# XPM is supported on ZU+ and `MEMORY_PRIMITIVE="ultra"` forces URAM
# placement. For 32768 * 128 b = 512 KB that's 2-wide * 8-deep = 16
# URAM288 (16 / 64 on XCK26). The 32 KB BRAM scratchpad previously
# consumed ~8 BRAM36 tiles; those are freed.
#
# axi_bram_ctrl is unchanged in role - still a 128-bit AXI4 slave on
# smartconnect_hb/M01, just with MEM_DEPTH 2048 -> 32768. T1's address
# window stays at 0xA0080000; only the range grows 32K -> 512K.
create_bd_cell -type ip -vlnv xilinx.com:ip:axi_bram_ctrl:4.1 bram_ctrl
set_property -dict [list \
    CONFIG.SINGLE_PORT_BRAM      {1} \
    CONFIG.DATA_WIDTH            {128} \
    CONFIG.MEM_DEPTH             {32768} \
    CONFIG.READ_LATENCY          {2} \
] [get_bd_cells bram_ctrl]
# Module-reference instantiation of the URAM wrapper. The Verilog file
# is brought in via `add_files` above; Vivado treats it like any other
# RTL module and auto-groups matching BRAM_PORTA_* ports into the bus
# interface for connect_bd_intf_net.
create_bd_cell -type module -reference uram_scratchpad bram
connect_bd_net [get_bd_pins clk_wiz_0/clk_60M]                 [get_bd_pins bram_ctrl/s_axi_aclk]
connect_bd_net [get_bd_pins proc_sys_reset/peripheral_aresetn] [get_bd_pins bram_ctrl/s_axi_aresetn]
connect_bd_intf_net [get_bd_intf_pins bram_ctrl/BRAM_PORTA] [get_bd_intf_pins bram/BRAM_PORTA]

#----- Camera capture pipeline IPs (300 MHz video, 100 MHz lite, 200 MHz DPHY) -----
# CONFIG dicts originally copied verbatim from kria-vitis-platforms /
# kv260_ispMipiRx_vcu_DP / scripts / config_bd.tcl, then slimmed for
# our actual use case (UYVY 128x128 from AP1302 ISP @ ~30 fps).
# Slim summary:
#   * MAX_COLS/MAX_ROWS 1920/1080 -> 256/256 (line buffer + counter
#     storage scales with these). 256 keeps 2x headroom over 128x128.
#   * MAX_NR_PLANES 2 -> 1 (only Y_UV8_420 needed 2 planes; that
#     format is no longer enabled).
#   * Drop HAS_RGB8/HAS_Y8/HAS_Y_UV8_420 - only HAS_UYVY8 enabled.
#   * AXIMM_DATA_WIDTH 128 -> 64 (m_axi_mm_video).
#   * axis_data_fifo_cap FIFO_DEPTH 1024 -> 256.
#
# 2026-05-10 - 5k aggressive slim (option 1 from fpga_build_status.md
# "5j next steps"). 5j routed-failed at 87.97% LUT util in the
# timing-driven rip-up loop; this targets ~3-4k more LUT savings to
# clear the cliff. Matched-chain edits (the AXIS data path is 1
# pixel/clock end-to-end after these - UYVY8 = 16 bits/pixel * 1):
#   * mipi_csi2_rx CSI_BUF_DEPTH 1024 -> 256 (rows are <=256 wide,
#     deeper buffer was over-provisioned by 4x).
#   * mipi_csi2_rx CMN_NUM_PIXELS 2 -> 1 (~1k LUT saving).
#   * v_frmbuf_wr SAMPLES_PER_CLOCK 2 -> 1 (~1.5-2.5k LUT saving;
#     s_axis_video TDATA shrinks 4 bytes -> 2 bytes).
#   * axis_subset_converter_cap S/M_TDATA_NUM_BYTES 4 -> 2 with
#     TDATA_REMAP {tdata[15:0]} so the chain stays width-matched
#     end-to-end (BD validate will error otherwise, or pixels would
#     drop). The CRITICAL WARNING [BD 41-237] from the 5e/5j eras
#     about M_AXIS != s_axis_video should now disappear since both
#     are 2 bytes.
# Restored 2026-05-09 after § 6.1.5's PSU=128 fix landed and § 6.4's
# CPU-cache fix unblocked the LSU tests; further slimmed 2026-05-10
# to clear the routing cliff for 5k.

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

# 2026-05-18 - replaced axis_data_fifo (256-deep, used 1 BRAM18) with a
# 1-deep axis_register_slice to free one BRAM tile.
# Rationale: the FIFO was only providing rate-match buffering between
# mipi_csi2_rx and v_frmbuf_wr. At 128x128@26fps the per-pixel period is
# ~700 cycles at 300 MHz - far longer than the typical PS HP1 DDR
# write-back latency (10-100 cycles). The 256-deep buffer was 10x the
# realistic need. A 1-deep skid buffer (axis_register_slice) covers the
# odd 1-cycle stall and keeps the BD width-match intact.
# Cell name preserved so downstream connect_bd_intf_net / connect_bd_net
# don't need to change.
create_bd_cell -type ip -vlnv xilinx.com:ip:axis_register_slice axis_data_fifo_cap

create_bd_cell -type ip -vlnv xilinx.com:ip:axis_subset_converter axis_subset_converter_cap
# F1 (5l): output widened 2 -> 3 bytes. v_frmbuf_wr/s_axis_video stays
# 3 bytes wide at SAMPLES_PER_CLOCK=1 + HAS_UYVY8=1 (Vivado pads the
# upper byte for format alignment), so a 2-byte producer triggered
# the BD 41-237 width-mismatch critical warning that's been firing
# since 5e and would have left the chroma channel undefined on hardware.
# Input stays 2 bytes (axis_data_fifo_cap upstream is 16 bpp); we
# explicitly zero-pad the upper byte in TDATA_REMAP.
set_property -dict [list \
    CONFIG.M_TDATA_NUM_BYTES {3} \
    CONFIG.M_TDEST_WIDTH     {1} \
    CONFIG.S_TDATA_NUM_BYTES {2} \
    CONFIG.S_TDEST_WIDTH     {10} \
    CONFIG.TDATA_REMAP       {8'b00000000,tdata[15:0]} \
    CONFIG.TDEST_REMAP       {tdest[0:0]} \
] [get_bd_cells axis_subset_converter_cap]

create_bd_cell -type ip -vlnv xilinx.com:ip:v_frmbuf_wr v_frmbuf_wr
# F9 (5p, 2026-05-12): enable HAS_Y_UV8_420 (NV12 family). NV12 stores
# Y as a contiguous plane at offset 0, which means our T1 kernels can
# `vle8.v v8, (cb.pa)` to load 128*128 = 16384 bytes of greyscale in
# one issue - no stride-2 LSU or vrgather Y-extract. MAX_NR_PLANES
# raised 1 -> 2 because NV12 is 2-plane (Y + UV-interleaved).
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
    CONFIG.HAS_Y_UV8_420                  {1} \
    CONFIG.MAX_COLS                       {256} \
    CONFIG.MAX_ROWS                       {256} \
    CONFIG.MAX_NR_PLANES                  {2} \
    CONFIG.SAMPLES_PER_CLOCK              {1} \
] [get_bd_cells v_frmbuf_wr]

#----- Tie AP1302 standby low (constant); rst_b driven from EMIO GPIO bit 1
# 2026-05-14 (5q final): ap1302_rst_b now wired through xlslice from
# emio_gpio_o[1] (= dts gpio 79). dts has `reset-gpios = <&gpio 79 1>` on
# the AP1302 node. Matches camtest3 v8 / camtest4 pattern; the kernel
# ap1302 driver toggles a real reset pin rather than a stub.
create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant ap1302_standby_const
set_property -dict [list CONFIG.CONST_VAL {0} CONFIG.CONST_WIDTH {1}] [get_bd_cells ap1302_standby_const]

create_bd_cell -type ip -vlnv xilinx.com:ip:xlslice ap1302_rst_slice
set_property -dict [list \
    CONFIG.DIN_WIDTH  {2} \
    CONFIG.DIN_FROM   {1} \
    CONFIG.DIN_TO     {1} \
    CONFIG.DOUT_WIDTH {1} \
] [get_bd_cells ap1302_rst_slice]
connect_bd_net [get_bd_pins zynq_ps/emio_gpio_o]      [get_bd_pins ap1302_rst_slice/Din]

#----- Top-level interface ports for camera ------------------------------
create_bd_intf_port -mode Slave  -vlnv xilinx.com:interface:mipi_phy_rtl:1.0 mipi_phy_if
create_bd_intf_port -mode Master -vlnv xilinx.com:interface:iic_rtl:1.0       iic
create_bd_port -dir O ap1302_rst_b
create_bd_port -dir O ap1302_standby

#----- New SmartConnects ------------------------------------------------
# smartconnect_lpd: PS LPD GP master (100 MHz) -> CSI-2 lite control +
# v_frmbuf_wr/s_axi_CTRL (F8 / 5o, 2026-05-11). Frmbuf CSR was historically
# detached from smartconnect_ctrl (see camera_bringup_status § 6.1.2/6.1.3);
# wiring it here avoids touching smartconnect_ctrl which would re-trigger
# axi_dma's AXIS propagation trap (~+15k LUT regression).
# NUM_MI=2: M00=mipi_csi2_rx/csirxss_s_axi, M01=v_frmbuf_wr/s_axi_CTRL.
create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 smartconnect_lpd
# NUM_CLKS=2: aclk (100 MHz) feeds S00 (PS LPD GP) + M00 (mipi_csi2_rx);
# aclk1 (300 MHz) feeds M01 (v_frmbuf_wr/s_axi_CTRL at ap_clk).
set_property -dict [list CONFIG.NUM_SI {1} CONFIG.NUM_MI {2} CONFIG.NUM_CLKS {2}] [get_bd_cells smartconnect_lpd]
connect_bd_net [get_bd_pins clk_wiz_0/clk_100M]                  [get_bd_pins smartconnect_lpd/aclk]
connect_bd_net [get_bd_pins proc_sys_reset_100M/interconnect_aresetn] [get_bd_pins smartconnect_lpd/aresetn]
connect_bd_net [get_bd_pins clk_wiz_0/clk_300M]                  [get_bd_pins smartconnect_lpd/aclk1]
# smartconnect uses a single aresetn pin even with NUM_CLKS>1 - it
# synchronises that reset to each clock domain internally.

# smartconnect_video: frmbuf m_axi_mm_video (300 MHz) -> PS HP1 (300 MHz)
create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 smartconnect_video
set_property -dict [list CONFIG.NUM_SI {1} CONFIG.NUM_MI {1} CONFIG.NUM_CLKS {1}] [get_bd_cells smartconnect_video]
connect_bd_net [get_bd_pins clk_wiz_0/clk_300M]                  [get_bd_pins smartconnect_video/aclk]
connect_bd_net [get_bd_pins proc_sys_reset_300M/interconnect_aresetn] [get_bd_pins smartconnect_video/aresetn]

#----- Extend existing SmartConnects ------------------------------------
# smartconnect_ctrl: NUM_CLKS=1, NUM_MI=3 (F6 PS->BRAM path deferred).
#   M00 t1_top/s_axi_ctrl       (60 MHz)
#   M01 axi_dma/S_AXI_LITE      (60 MHz)
#   M02 sensor_iic/S_AXI        (60 MHz, AP1302 IIC control)
set_property -dict [list CONFIG.NUM_MI {3} CONFIG.NUM_CLKS {1}] [get_bd_cells smartconnect_ctrl]

# smartconnect_idx (F4 / 5l): NUM_MI=1, T1 idx -> HP0/DDR only.
# Pre-5l this was NUM_MI=2 with M01 -> bram_ctrl. Scratchpad has been
# rewired to smartconnect_hb so DMA can also reach it; idx now only
# routes to DDR via HP0.
set_property CONFIG.NUM_MI {1} [get_bd_cells smartconnect_idx]

# smartconnect_hb (F4 / 5l): NUM_SI=3 (T1 hb + DMA mm2s + DMA s2mm),
# NUM_MI=2 (M00 -> HPC0/DDR, M01 -> bram_ctrl scratchpad).
# Pre-5l this was NUM_MI=1; the new M01 lets T1 hb AND both DMA
# channels reach the 32 KB scratchpad at 0xB0000000.
set_property -dict [list CONFIG.NUM_SI {3} CONFIG.NUM_MI {2}] [get_bd_cells smartconnect_hb]

#----- SmartConnect synthesis strategy ---------------------------------
# Valid values for CONFIG.STRATEGY in Vivado 2025.2: AUTOMATIC,
# PERFORMANCE, LOW_AREA (string, not numeric).
#
# 2026-05-09: keep every SmartConnect in LOW_AREA for the 128-bit HPM
# debug candidate. The 2026-05-08 PERFORMANCE control-plane build hit
# severe route congestion (route level 5, congestion preventing all nets
# from routing). This preserves the main experiment, MAXIGP0/MAXIGP2
# 32->128, while reducing LUT/routing pressure and keeping the control
# fabric closer to the previously deployed structure.
foreach sc {smartconnect_ctrl smartconnect_hb smartconnect_idx smartconnect_lpd smartconnect_video} {
    set_property CONFIG.STRATEGY {LOW_AREA} [get_bd_cells $sc]
}

#----- Camera pipeline clocks/resets ------------------------------------
# CSI-2 RX
connect_bd_net [get_bd_pins clk_wiz_0/clk_100M]                  [get_bd_pins mipi_csi2_rx/lite_aclk]
connect_bd_net [get_bd_pins proc_sys_reset_100M/peripheral_aresetn] [get_bd_pins mipi_csi2_rx/lite_aresetn]
connect_bd_net [get_bd_pins clk_wiz_0/clk_200M]                  [get_bd_pins mipi_csi2_rx/dphy_clk_200M]
connect_bd_net [get_bd_pins clk_wiz_0/clk_300M]                  [get_bd_pins mipi_csi2_rx/video_aclk]
connect_bd_net [get_bd_pins proc_sys_reset_300M/peripheral_aresetn] [get_bd_pins mipi_csi2_rx/video_aresetn]

# axis_register_slice (1-deep skid; was axis_data_fifo before 2026-05-18)
# + axis_subset_converter (300 MHz video domain). axis_register_slice
# uses `aclk`/`aresetn` pin names (no `s_axis_` prefix).
connect_bd_net [get_bd_pins clk_wiz_0/clk_300M]                  [get_bd_pins axis_data_fifo_cap/aclk]
connect_bd_net [get_bd_pins proc_sys_reset_300M/peripheral_aresetn] [get_bd_pins axis_data_fifo_cap/aresetn]
connect_bd_net [get_bd_pins clk_wiz_0/clk_300M]                  [get_bd_pins axis_subset_converter_cap/aclk]
connect_bd_net [get_bd_pins proc_sys_reset_300M/peripheral_aresetn] [get_bd_pins axis_subset_converter_cap/aresetn]

# v_frmbuf_wr
connect_bd_net [get_bd_pins clk_wiz_0/clk_300M]                  [get_bd_pins v_frmbuf_wr/ap_clk]
connect_bd_net [get_bd_pins proc_sys_reset_300M/peripheral_aresetn] [get_bd_pins v_frmbuf_wr/ap_rst_n]

# F8 (5o, 2026-05-11): v_frmbuf_wr runs s_axi_CTRL on its ap_clk (300 MHz)
# in single-clock-domain mode - no separate s_axi_CTRL_aclk pin is exposed.
# smartconnect_lpd handles the 100 MHz S00 (PS LPD GP) to 300 MHz M01 (frmbuf
# CTRL) CDC internally via NUM_CLKS=2 (see smartconnect_lpd config below).

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

# AP1302 control GPIOs (rst_b via EMIO bit 1 - see ap1302_rst_slice above)
connect_bd_net [get_bd_pins ap1302_rst_slice/Dout]               [get_bd_ports ap1302_rst_b]
connect_bd_net [get_bd_pins ap1302_standby_const/dout]           [get_bd_ports ap1302_standby]

# CSI-2 lite csr (PS LPD GP -> smartconnect_lpd -> csi2 lite)
connect_bd_intf_net [get_bd_intf_pins zynq_ps/M_AXI_HPM0_LPD]              [get_bd_intf_pins smartconnect_lpd/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins smartconnect_lpd/M00_AXI]            [get_bd_intf_pins mipi_csi2_rx/csirxss_s_axi]
# F8 (5o): v_frmbuf_wr/s_axi_CTRL on smartconnect_lpd/M01 - frees PS to
# program frmbuf CSR (the missing piece blocking camera streaming on 5m).
connect_bd_intf_net [get_bd_intf_pins smartconnect_lpd/M01_AXI]            [get_bd_intf_pins v_frmbuf_wr/s_axi_CTRL]

# frmbuf m_axi_mm_video -> smartconnect_video -> PS HP1
connect_bd_intf_net [get_bd_intf_pins v_frmbuf_wr/m_axi_mm_video]          [get_bd_intf_pins smartconnect_video/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins smartconnect_video/M00_AXI]          [get_bd_intf_pins zynq_ps/S_AXI_HP1_FPD]

# sensor_iic on smartconnect_ctrl/M02
connect_bd_intf_net [get_bd_intf_pins smartconnect_ctrl/M02_AXI]           [get_bd_intf_pins sensor_iic/S_AXI]

# F4 (5l/5m): scratchpad on hb side, reachable from T1 hb + DMA mm2s + DMA s2mm.
connect_bd_intf_net [get_bd_intf_pins smartconnect_hb/M01_AXI]             [get_bd_intf_pins bram_ctrl/S_AXI]

# DMA masters: previously unwired in the BD; route through smartconnect_hb
# alongside T1 hb to reach PS HPC0 / DDR.
connect_bd_intf_net [get_bd_intf_pins axi_dma/M_AXI_MM2S]                  [get_bd_intf_pins smartconnect_hb/S01_AXI]
connect_bd_intf_net [get_bd_intf_pins axi_dma/M_AXI_S2MM]                  [get_bd_intf_pins smartconnect_hb/S02_AXI]

# F3+F5 (5m): close the longstanding [axi_dma:7.1-11] S_AXIS_S2MM
# unconnected critical warning by looping the streaming sides of
# axi_dma - but via an axis_register_slice rather than a direct
# self-loop. 5l's plain self-loop hit two warnings during synth:
#   [BD 41-3281] axi_dma is connected on both sides by SmartConnects
#                and cannot automatically configure itself...
#   [BD 41-702]  Propagation TCL tries to overwrite USER strength
#                parameter C_M_AXI_S2MM_DATA_WIDTH(128)... Command ignored
# The propagation override ignored our explicit user-set 128-bit width,
# but the AXIS sideband parameters (TID/TUSER widths, TKEEP semantics)
# silently drifted out of sync and dma_loopback timed out. The slice
# breaks the propagation feedback loop (Vivado treats it as two
# separate AXIS channels with independent propagation graphs).
#
# Required for F4's primary use case (DDR<->BRAM via DMA): axi_dma is
# mem-to-stream + stream-to-mem, so any memory-to-memory transfer needs
# M_AXIS_MM2S to feed back into S_AXIS_S2MM. With this loopback + F4's
# address-map, a DDR-source/BRAM-dest descriptor pair runs as:
#   MM2S: M_AXI_MM2S reads DDR -> emits on M_AXIS_MM2S
#   slice: forwards stream
#   S2MM: S_AXIS_S2MM accepts -> M_AXI_S2MM writes BRAM
create_bd_cell -type ip -vlnv xilinx.com:ip:axis_register_slice axis_reg_slice_dma
connect_bd_net [get_bd_pins clk_wiz_0/clk_60M]                 [get_bd_pins axis_reg_slice_dma/aclk]
connect_bd_net [get_bd_pins proc_sys_reset/peripheral_aresetn] [get_bd_pins axis_reg_slice_dma/aresetn]
connect_bd_intf_net [get_bd_intf_pins axi_dma/M_AXIS_MM2S]     [get_bd_intf_pins axis_reg_slice_dma/S_AXIS]
connect_bd_intf_net [get_bd_intf_pins axis_reg_slice_dma/M_AXIS] [get_bd_intf_pins axi_dma/S_AXIS_S2MM]

#----- IRQs: extend irq_concat from 3 -> 6 ports -------------------------
# In0=t1, In1=mm2s, In2=s2mm, In3=sensor_iic, In4=csi2, In5=frmbuf_wr.
# F8 (5o): frmbuf interrupt RE-attached alongside its CSR via smartconnect_lpd.
# pl_ps_irq0[5] -> GIC SPI 94, matches the existing dts isp_fb_wr_csi interrupts.
set_property CONFIG.NUM_PORTS {6} [get_bd_cells irq_concat]
connect_bd_net [get_bd_pins sensor_iic/iic2intc_irpt]   [get_bd_pins irq_concat/In3]
connect_bd_net [get_bd_pins mipi_csi2_rx/csirxss_csi_irq] [get_bd_pins irq_concat/In4]
connect_bd_net [get_bd_pins v_frmbuf_wr/interrupt]        [get_bd_pins irq_concat/In5]

# ILA + debug_bridge (system_ila on smartconnect_hb/S00 + AXI-attached
# debug_bridge for SSH-routed XVC) removed 2026-05-09 - they were
# staged for LSU debug, but the LSU bug was diagnosed without ILA
# (turned out to be CPU cache, see § 6.4). Restoration content lives
# in fyp_doc/camera_pipeline_restore_handoff.md (look for the
# "ILA + debug_bridge" appendix added in the same revision).

#Address Map
#AXI Lite wrapper registers, currently 64KB range and only 128 bytes used)
assign_bd_address -target_address_space /zynq_ps/Data \
    [get_bd_addr_segs t1_top/s_axi_ctrl/reg0] -range 64K -offset 0xA0000000

#DMA control registers
assign_bd_address -target_address_space /zynq_ps/Data \
    [get_bd_addr_segs axi_dma/S_AXI_LITE/Reg] -range 64K -offset 0xA0010000

# debug_bridge_xvc address-map removed alongside ILA removal.

#mipi_csi2_rx CSR (100 MHz LPD control plane via smartconnect_lpd).
#PS LPD GP master only reaches 0x8000_0000-0x9FFF_FFFF apertures (per
#UG1085); 0x80000000 matches the kv260_ispMipiRx_vcu_DP reference.
assign_bd_address -target_address_space /zynq_ps/Data \
    [get_bd_addr_segs mipi_csi2_rx/csirxss_s_axi/Reg] -range 64K -offset 0x80000000

#AP1302 IIC control (60 MHz control plane via smartconnect_ctrl/M02)
assign_bd_address -target_address_space /zynq_ps/Data \
    [get_bd_addr_segs sensor_iic/S_AXI/Reg] -range 64K -offset 0xA0050000

# F8 (5o): frmbuf CSR @ 0x80010000 (LPD aperture, reachable from
# PS LPD GP master M_AXI_HPM0_LPD which addresses 0x8000_0000-0x9FFF_FFFF
# only - UG1085). Cannot use 0xA0020000 (FPD aperture) since smartconnect_lpd
# is fed by HPM0_LPD. Address space neighbour to mipi_csi2_rx (0x80000000).
# dts node moved to fb_wr@80010000 in lockstep with this address change.
assign_bd_address -target_address_space /zynq_ps/Data \
    [get_bd_addr_segs v_frmbuf_wr/s_axi_CTRL/Reg] -range 64K -offset 0x80010000

#T1 highBandwidth port -> full DDR range (via HPC0)
assign_bd_address -target_address_space /t1_top/m_axi_hb \
    [get_bd_addr_segs zynq_ps/SAXIGP0/HPC0_DDR_LOW] -range 2G -offset 0x00000000

#T1 indexed port -> full DDR range (via HP0)
assign_bd_address -target_address_space /t1_top/m_axi_idx \
    [get_bd_addr_segs zynq_ps/SAXIGP2/HP0_DDR_LOW] -range 2G -offset 0x00000000

#F4 (5l/5m): 32KB scratchpad at 0xA0080000, visible to T1 hb + DMA.
#5r: scratchpad grown to 512KB (URAM-backed). Address base unchanged.
#F6 (PS-side direct access) deferred - see top of bram_ctrl block.
assign_bd_address -target_address_space /t1_top/m_axi_hb \
    [get_bd_addr_segs bram_ctrl/S_AXI/Mem0] -range 512K -offset 0xA0080000

#DMA MM2S / S2MM -> full DDR range (via HPC0 too, sharing with T1 hb)
assign_bd_address -target_address_space /axi_dma/Data_MM2S \
    [get_bd_addr_segs zynq_ps/SAXIGP0/HPC0_DDR_LOW] -range 2G -offset 0x00000000
assign_bd_address -target_address_space /axi_dma/Data_S2MM \
    [get_bd_addr_segs zynq_ps/SAXIGP0/HPC0_DDR_LOW] -range 2G -offset 0x00000000

#F4 (5l/5m): also expose the scratchpad to both DMA channels so DMA can
#prefetch DDR -> URAM (and write URAM -> DDR for output).
#5r: range bumped 32K -> 512K (URAM-backed scratchpad).
assign_bd_address -target_address_space /axi_dma/Data_MM2S \
    [get_bd_addr_segs bram_ctrl/S_AXI/Mem0] -range 512K -offset 0xA0080000
assign_bd_address -target_address_space /axi_dma/Data_S2MM \
    [get_bd_addr_segs bram_ctrl/S_AXI/Mem0] -range 512K -offset 0xA0080000

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

# AP1302 standby / rst_b pin constraints (J10 / J11 on KV260 IAS connector).
# Required because these two ports are not bound to a Vivado board interface,
# so they have no auto-supplied LOC / IOSTANDARD.
set xdc_pin [file join [file dirname [file normalize [info script]]] pin.xdc]
add_files -fileset constrs_1 -norecurse ${xdc_pin}

save_bd_design

puts ""
puts "========================================"
puts "Block design created successfully!!"
puts "Project: ${project_dir}"
puts "Address map:"
puts "  AXI Lite Wrapper:  0xA0000000 (64KB)"
puts "  DMA Control:       0xA0010000 (64KB)"
puts "  v_frmbuf_wr m_axi: HP1 -> DDR  (2 GB)"
puts "  mipi_csi2_rx CSR:  0x80000000 (64KB)  via PS LPD"
puts "  AP1302 IIC:        0xA0050000 (64KB)"
puts "  T1 HPC0 -> DDR:    0x00000000 (2GB)"
puts "  T1 HP0  -> DDR:    0x00000000 (2GB)"
puts "  T1 hb  -> URAM:    0xA0080000 (512KB) via smartconnect_hb/M01 (5r: emb_mem_gen URAM)"
puts "  DMA -> DDR:        0x00000000 (2GB) via HPC0"
puts "  DMA -> URAM:       0xA0080000 (512KB) via smartconnect_hb/M01"
puts "  (F6 PS->BRAM deferred - validate via DMA round-trip)"
puts "  Frmbuf -> DDR:     0x00000000 (2GB) via HP1"
puts "========================================"
