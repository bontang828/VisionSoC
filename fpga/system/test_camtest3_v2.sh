#!/usr/bin/env bash
# Deploy + test camtest3 (pl_clk0=100MHz fix) end-to-end.
# Stops at the first failure; prints CSR.PKTCNT for the verdict.

set -e

VISIONSOC_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
LATEST=$(ls -dt "${VISIONSOC_DIR}/fpga/build/camtest3-"* 2>/dev/null | head -1)
echo "=== build dir: ${LATEST} ==="

# bit -> bit.bin
cd "${LATEST}"
cat > camtest3.bif <<'EOF'
all:
{
	[destination_device = pl] system_top_camtest3_wrapper.bit
}
EOF
/home/cbt22/Xilinx/2025.2/Vivado/bin/bootgen -image camtest3.bif -arch zynqmp -o system_top_camtest3.bit.bin -w 2>&1 | tail -1

# SCP + load
scp -q system_top_camtest3.bit.bin "${VISIONSOC_DIR}/fpga/dts/system_top_camtest3.dts" kv260:/tmp/
ssh kv260 '
dtc -@ -I dts -O dtb -o /tmp/system_top_camtest3.dtbo /tmp/system_top_camtest3.dts 2>&1 | grep -v Warning | head -1
sudo install -m 644 -o root -g root /tmp/system_top_camtest3.bit.bin /lib/firmware/xilinx/visionsoc/system_top_camtest3.bit.bin
sudo install -m 644 -o root -g root /tmp/system_top_camtest3.dtbo /lib/firmware/xilinx/camtest3/system_top_camtest3.dtbo
sudo rmdir /sys/kernel/config/device-tree/overlays/full 2>/dev/null || true
sleep 1
sudo fpgautil -b /lib/firmware/xilinx/visionsoc/system_top_camtest3.bit.bin \
              -o /lib/firmware/xilinx/camtest3/system_top_camtest3.dtbo 2>&1 | tail -1
sleep 4

echo ""
echo "=== AP1302 manual un-stall + CCR enable ==="
sudo i2ctransfer -f -y 4 w4@0x3c 0x60 0x1a 0x83 0x40
sleep 0.5
echo "  SYS_START: $(sudo i2ctransfer -f -y 4 w2@0x3c 0x60 0x1a r2)"
sudo devmem2 0x80000000 w 0x1 2>&1 | tail -1
sleep 2

echo ""
echo "=== CSI2RX state ==="
sudo /home/ubuntu/vision_software/libt1/test/csi_dump

echo ""
echo "=== AP1302 HINF counter (chip emission) ==="
sudo dmesg -c > /dev/null
sudo v4l2-dbg --device=/dev/v4l-subdev1 --log-status > /dev/null 2>&1
sleep 0.3
sudo dmesg | grep "Frame counters" | head -1
'
echo ""
echo "=== VERDICT ==="
echo "If CSR PKTCNT (upper 16 bits) > 0 -> camera path WORKING!"
echo "If still 0x00000000 -> some other layer issue, not FVCO."
