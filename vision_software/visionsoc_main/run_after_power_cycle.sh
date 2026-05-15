#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    exec sudo "$0" "$@"
fi

APP_DIR=/home/ubuntu/vision_software/visionsoc_main
BIT=/lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin
DTBO=/lib/firmware/xilinx/visionsoc/system_top_wrapper.dtbo

pkill -TERM visionsoc_main 2>/dev/null || true
sleep 1
pkill -KILL visionsoc_main 2>/dev/null || true

for ov in full k26-starter-kits_image_1; do
    if [ -d "/sys/kernel/config/device-tree/overlays/$ov" ]; then
        rmdir "/sys/kernel/config/device-tree/overlays/$ov" 2>/dev/null || true
    fi
done

if ! fpgautil -b "$BIT" -o "$DTBO"; then
    echo "ERROR: failed to apply VisionSoC overlay: $DTBO" >&2
    echo "Active overlays:" >&2
    ls -la /sys/kernel/config/device-tree/overlays >&2 || true
    echo "Recent overlay/camera kernel log:" >&2
    dmesg | grep -Ei 'fpga|firmware|overlay|of:|ap1302|csiss|frmbuf|xilinx-video|iic|i2c' | tail -80 >&2 || true
    exit 1
fi

for _ in $(seq 1 40); do
    if [ -e /dev/media0 ] && [ -e /dev/video0 ] &&
       [ -e /dev/v4l-subdev1 ] && [ -e /dev/v4l-subdev2 ]; then
        break
    fi
    sleep 0.5
done

ls -l /dev/media0 /dev/video0 /dev/v4l-subdev1 /dev/v4l-subdev2

media-ctl -d /dev/media0 \
    -V '"ap1302.4-003c":2 [fmt:VYYUYY8_1X24/128x128 field:none colorspace:srgb]'
media-ctl -d /dev/media0 \
    -V '"80000000.csiss":0 [fmt:VYYUYY8_1X24/128x128 field:none colorspace:srgb]'
media-ctl -d /dev/media0 \
    -V '"80000000.csiss":1 [fmt:VYYUYY8_1X24/128x128 field:none colorspace:srgb]'

media-ctl -p -d /dev/media0 | sed -n '1,120p'

systemctl stop gdm

restore_gdm()
{
    systemctl start gdm
}
trap restore_gdm EXIT INT TERM

cd "$APP_DIR"
./visionsoc_main
