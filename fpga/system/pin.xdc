# KV260 IAS connector pin constraints for AP1302 ISP control signals.
# Source: kria-vitis-platforms/kv260/platforms/vivado/kv260_ispMipiRx_vcu_DP/xdc/pin.xdc
# These two ports are created bare in system_top.tcl (not bound to a board
# interface), so they need explicit LOC/IOSTANDARD here or write_bitstream
# fails with DRC NSTD-1 / UCIO-1.

set_property PACKAGE_PIN J11      [get_ports {ap1302_rst_b[0]}]
set_property IOSTANDARD  LVCMOS33 [get_ports {ap1302_rst_b[0]}]
set_property SLEW        SLOW     [get_ports {ap1302_rst_b[0]}]
set_property DRIVE       4        [get_ports {ap1302_rst_b[0]}]

set_property PACKAGE_PIN J10      [get_ports {ap1302_standby[0]}]
set_property IOSTANDARD  LVCMOS33 [get_ports {ap1302_standby[0]}]
set_property SLEW        SLOW     [get_ports {ap1302_standby[0]}]
set_property DRIVE       4        [get_ports {ap1302_standby[0]}]
