# Vivado TCL: connect to Kria xvcServer via SSH-tunneled TCP/XVC,
# configure csi_ila trigger on mipi_csi2_rx/video_out:TVALID,
# capture, dump to file.
#
# Prereqs:
#   * camtest3 bitstream deployed on Kria
#   * xvcServer_mmap running on Kria, port 2542
#   * SSH port-forward: ssh -p 2222 -L 2542:localhost:2542 -N -f ubuntu@localhost
#
# Usage:  vivado -mode tcl -source capture_csi_ila.tcl

open_hw_manager

# connect_hw_server with no -url auto-starts a local hw_server.
connect_hw_server

# Tell hw_server to dial into the SSH-tunneled XVC server
set xvc_url "localhost:2542"
puts "INFO: opening hw_target via XVC at $xvc_url"
open_hw_target -xvc_url $xvc_url

set targets [get_hw_targets]
puts "INFO: targets = $targets"

# Display devices found
set devices [get_hw_devices]
puts "INFO: hw_devices = $devices"

# Find ILA
set ilas [get_hw_ilas -quiet]
puts "INFO: ILAs = $ilas"

if {[llength $ilas] == 0} {
    puts "ERROR: no ILA found in hw target."
    close_hw_target
    disconnect_hw_server
    exit 1
}

set csi_ila [lindex $ilas 0]
puts "INFO: using ILA: $csi_ila"

# List probes
set probes [get_hw_probes -of_objects $csi_ila]
puts "INFO: probes available:"
foreach p $probes { puts "  - $p" }

# Trigger on TVALID == 1
set tvalid_probe [get_hw_probes -of $csi_ila -filter {NAME =~ "*tvalid*"} -quiet]
if {[llength $tvalid_probe] == 0} {
    set tvalid_probe [get_hw_probes -of $csi_ila -filter {NAME =~ "*TVALID*"} -quiet]
}

if {[llength $tvalid_probe] > 0} {
    set p [lindex $tvalid_probe 0]
    puts "INFO: triggering on $p == 1"
    set_property COMPARE_VALUE eq1'b1 $p
} else {
    puts "WARN: TVALID probe not auto-found; will capture without trigger filter"
}

# Configure ILA: capture all samples (no condition), trigger at start
set_property CONTROL.TRIGGER_POSITION 0 $csi_ila
set_property CONTROL.CAPTURE_MODE BASIC $csi_ila

run_hw_ila $csi_ila
puts "INFO: ILA armed. Now start a camera stream on the Kria within 30s..."
puts "      Example: ssh kv260 'sudo timeout 5 v4l2-ctl --device=/dev/video0 --stream-mmap --stream-count=1 ...'"

# Wait for trigger / timeout
wait_on_hw_ila -timeout 30 $csi_ila

set status [get_property CORE_STATUS $csi_ila]
puts "INFO: ILA status = $status"

# Upload + save
upload_hw_ila_data $csi_ila
write_hw_ila_data -force /tmp/csi_ila_data.ila $csi_ila
puts "INFO: capture dumped to /tmp/csi_ila_data.ila"

set num_samples [get_property NUMBER_SAMPLES [current_hw_ila_data]]
puts "INFO: captured $num_samples samples"

# Decode TVALID to count beats
set tvalid_count 0
set non_zero_tdata 0
for {set i 0} {$i < $num_samples} {incr i} {
    set tv [get_hw_probe_data_at_index -hw_ila_data [current_hw_ila_data] -name "SLOT_0_AXIS_tvalid" $i 2>&1]
    if {[string equal $tv "1"]} {
        incr tvalid_count
    }
}

puts ""
puts "=== INTERPRETATION ==="
puts "samples=$num_samples  TVALID=1 count=$tvalid_count"
if {$tvalid_count > 0} {
    puts "CSI2RX IS producing AXIS beats — bug is downstream (frmbuf/handshake/DDR)."
} else {
    puts "CSI2RX is SILENT — bug is at DPHY decode or before."
}

close_hw_target
disconnect_hw_server
