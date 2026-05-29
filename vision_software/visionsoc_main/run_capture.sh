#!/usr/bin/env bash
# run_capture.sh -- kick off a screenshot-capture run on the board.
#
# Usage:
#   ./run_capture.sh <OUT_DIR_ON_BOARD> [N_FRAMES] [EVERY_M]
#
# Runs on the board (typical invocation: ssh kv260 ~/.../run_capture.sh ...).
# Same kill/mask/reapply/relaunch dance as run_perf.sh pipeline mode --
# main_capture is also the only DRM client AND comes up after a fresh
# media-ctl topology.
#
# Captures land in $OUT_DIR_ON_BOARD as PPM files. Cleans /tmp/capture*
# before the run so previous output never accumulates. After
# visionsoc_main_capture exits, the production visionsoc_main is
# relaunched so the HDMI feed returns to the live kernel.

set -u

OUT_DIR=${1:-}
FRAMES=${2:-300}
EVERY=${3:-30}

if [ -z "$OUT_DIR" ]; then
    cat >&2 <<EOF
usage: $0 <OUT_DIR_ON_BOARD> [N_FRAMES] [EVERY_M]
  OUT_DIR_ON_BOARD  directory the capture binary writes PPMs into
                    (any leftover /tmp/capture* is purged before the run)
  N_FRAMES          frames to run before exit (default 300)
  EVERY_M           snapshot frame 0, M, 2M, ... (default 30)
EOF
    exit 2
fi

CAPTURE_BIN=/home/ubuntu/vision_software/libt1/test/visionsoc_main_capture
VSM=/home/ubuntu/vision_software/visionsoc_main/visionsoc_main
BIT=/lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin
DTBO=/lib/firmware/xilinx/visionsoc/system_top_wrapper.dtbo

stop_vsm() {
    PID=$(pgrep -x visionsoc_main || true)
    if [ -n "$PID" ]; then
        echo "  killing visionsoc_main PID $PID"
        sudo -n kill -TERM "$PID" 2>/dev/null || true
        for _ in 1 2 3 4 5; do
            sleep 1
            pgrep -x visionsoc_main >/dev/null || return 0
        done
        sudo -n kill -KILL "$PID" 2>/dev/null || true
        sleep 1
    fi
}

start_vsm() {
    echo "  relaunching visionsoc_main"
    nohup sudo -n "$VSM" /dev/video0 </dev/null >>/tmp/vsm.log 2>&1 &
    sleep 3
    pgrep -x visionsoc_main >/dev/null && echo "  visionsoc_main back up" ||
        echo "  WARNING: visionsoc_main did not start (check /tmp/vsm.log)"
}

reapply_overlay_and_media_ctl() {
    echo "  reapply overlay"
    sudo -n rmdir /sys/kernel/config/device-tree/overlays/full 2>/dev/null
    sudo -n rmdir /sys/kernel/config/device-tree/overlays/k26-starter-kits_image_1 2>/dev/null
    sudo -n fpgautil -b "$BIT" -o "$DTBO" 2>&1 | tail -2

    echo "  wait /dev/video0"
    for _ in $(seq 1 40); do
        [ -e /dev/media0 ] && [ -e /dev/video0 ] && \
            [ -e /dev/v4l-subdev1 ] && [ -e /dev/v4l-subdev2 ] && break
        sleep 0.5
    done

    echo "  apply media-ctl topology"
    sudo -n media-ctl -d /dev/media0 \
        -V '"ap1302.4-003c":2 [fmt:VYYUYY8_1X24/128x128 field:none colorspace:srgb]'
    sudo -n media-ctl -d /dev/media0 \
        -V '"80000000.csiss":0 [fmt:VYYUYY8_1X24/128x128 field:none colorspace:srgb]'
    sudo -n media-ctl -d /dev/media0 \
        -V '"80000000.csiss":1 [fmt:VYYUYY8_1X24/128x128 field:none colorspace:srgb]'
}

mask_gdm_if_active() {
    if systemctl is-active --quiet gdm 2>/dev/null; then
        echo "  mask gdm (was active)"
        sudo -n systemctl stop gdm 2>&1 | tail -2 || true
    fi
    sudo -n systemctl mask gdm 2>&1 | tail -2 || true
}

purge_stale_captures() {
    # User directive: never accumulate captures on the board between
    # runs. Any /tmp/capture* left from a previous (possibly crashed)
    # run is removed before the new run creates its own dir.
    echo "  purge /tmp/capture* (stale)"
    sudo -n rm -rf /tmp/capture /tmp/capture-* /tmp/capture_* 2>/dev/null || true
    rm -rf /tmp/capture /tmp/capture-* /tmp/capture_* 2>/dev/null || true
}

echo "=== run_capture out=$OUT_DIR frames=$FRAMES every=$EVERY ==="
purge_stale_captures
stop_vsm
mask_gdm_if_active
reapply_overlay_and_media_ctl

mkdir -p "$OUT_DIR"

sudo -n "$CAPTURE_BIN" \
    --frames "$FRAMES" --every "$EVERY" --out-dir "$OUT_DIR"
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "  ERROR: visionsoc_main_capture failed with rc=$rc" >&2
    start_vsm
    exit "$rc"
fi

NCAP=$(ls -1 "$OUT_DIR"/capture_*.ppm 2>/dev/null | wc -l)
echo "  PPMs written: $NCAP in $OUT_DIR"
start_vsm
