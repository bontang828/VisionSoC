#!/usr/bin/env bash
# Deploy camtest3 bitstream + dtbo to Kria, then launch xvcServer.
# After this, Vivado HW manager can connect via SSH-tunnel TCP/XVC.
#
# Usage:  ./deploy_camtest3.sh
# Prereq: camtest3 build complete in fpga/build/camtest3-<latest>/

set -e

VISIONSOC_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
LATEST=$(ls -dt "${VISIONSOC_DIR}/fpga/build/camtest3-"* 2>/dev/null | head -1)
if [ -z "$LATEST" ] || [ ! -f "${LATEST}/system_top_camtest3_wrapper.bit" ]; then
    echo "ERROR: no camtest3 .bit found in ${LATEST}"; exit 1
fi

echo "=== using build dir: ${LATEST} ==="

# 1. bit -> bit.bin
cd "${LATEST}"
cat > camtest3.bif <<'EOF'
all:
{
	[destination_device = pl] system_top_camtest3_wrapper.bit
}
EOF
/home/cbt22/Xilinx/2025.2/Vivado/bin/bootgen -image camtest3.bif -arch zynqmp \
    -o system_top_camtest3.bit.bin -w 2>&1 | tail -2

# 2. SCP bit.bin + dts to Kria
scp system_top_camtest3.bit.bin \
    "${VISIONSOC_DIR}/fpga/dts/system_top_camtest3.dts" \
    kv260:/tmp/ 2>&1 | tail -2

# 3. Compile dtbo, install, load overlay
ssh kv260 '
set -e
dtc -@ -I dts -O dtb -o /tmp/system_top_camtest3.dtbo /tmp/system_top_camtest3.dts 2>&1 | grep -v Warning
sudo mkdir -p /lib/firmware/xilinx/camtest3
sudo install -m 644 -o root -g root /tmp/system_top_camtest3.bit.bin /lib/firmware/xilinx/visionsoc/system_top_camtest3.bit.bin
sudo install -m 644 -o root -g root /tmp/system_top_camtest3.dtbo /lib/firmware/xilinx/camtest3/system_top_camtest3.dtbo

echo "=== unload current overlay + load camtest3 ==="
sudo rmdir /sys/kernel/config/device-tree/overlays/full 2>/dev/null || true
sleep 1
sudo fpgautil -b /lib/firmware/xilinx/visionsoc/system_top_camtest3.bit.bin \
              -o /lib/firmware/xilinx/camtest3/system_top_camtest3.dtbo 2>&1 | tail -2
sleep 4

echo ""
echo "=== UIO map (find xvc bridge uio) ==="
for u in /sys/class/uio/uio*; do
    name=$(cat $u/name 2>/dev/null)
    addr=$(cat $u/maps/map0/addr 2>/dev/null)
    echo "$(basename $u): name=$name addr=$addr"
done
'

echo ""
echo "=== NEXT STEPS ==="
echo "1. Find which /dev/uioN maps to addr 0x80020000 from output above"
echo "2. On Kria:  /tmp/xvcserver-src/xvcServer_mmap -d /dev/uioN -p 2542 -v &"
echo "3. SSH tunnel from your laptop: ssh -L 2542:localhost:2542 kv260"
echo "4. On dev host:  vivado -mode tcl -source fpga/system/capture_csi_ila.tcl"
