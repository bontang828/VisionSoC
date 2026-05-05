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

#Configure PS interfaces needed for T1
#Also set the clk frequency
set_property -dict [list \
    CONFIG.PSU__USE__M_AXI_GP0          {1} \
    CONFIG.PSU__USE__M_AXI_GP1          {0} \
    CONFIG.PSU__USE__S_AXI_GP0          {1} \
    CONFIG.PSU__USE__S_AXI_GP2          {1} \
    CONFIG.PSU__USE__IRQ0               {1} \
    CONFIG.PSU__MAXIGP0__DATA_WIDTH     {32} \
    CONFIG.PSU__SAXIGP0__DATA_WIDTH     {128} \
    CONFIG.PSU__SAXIGP2__DATA_WIDTH     {32} \
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







#Address Map
#AXI Lite wrapper registers, currently 64KB range and only 128 bytes used)
assign_bd_address -target_address_space /zynq_ps/Data \
    [get_bd_addr_segs t1_top/s_axi_ctrl/reg0] -range 64K -offset 0xA0000000

#DMA control registers
assign_bd_address -target_address_space /zynq_ps/Data \
    [get_bd_addr_segs axi_dma/S_AXI_LITE/Reg] -range 64K -offset 0xA0010000

#T1 highBandwidth port -> full DDR range (via HPC0)
assign_bd_address -target_address_space /t1_top/m_axi_hb \
    [get_bd_addr_segs zynq_ps/SAXIGP0/HPC0_DDR_LOW] -range 2G -offset 0x00000000

#T1 indexed port -> full DDR range (via HP0)
assign_bd_address -target_address_space /t1_top/m_axi_idx \
    [get_bd_addr_segs zynq_ps/SAXIGP2/HP0_DDR_LOW] -range 2G -offset 0x00000000





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
puts "  AXI Lite Wrapper: 0xA0000000 (64KB)"
puts "  DMA Control:      0xA0010000 (64KB)"
puts "  T1 HPC0 -> DDR:   0x00000000 (2GB)"
puts "  T1 HP0  -> DDR:   0x00000000 (2GB)"
puts "========================================"
