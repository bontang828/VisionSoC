#!/usr/bin/env bash
# run_viz.sh -- kick off a stage-by-stage attention_self visualisation on the
# board.
#
# Usage:
#   ./run_viz.sh <OUT_DIR_ON_BOARD> [WARMUP_FRAMES]
#
# Runs on the board (typical invocation: ssh kv260 ~/.../run_viz.sh ...).
# attention_self_viz grabs ONE settled live camera frame and dumps the 128x128
# data-plane at every pipeline stage to PPMs. It uses the camera but NOT the
# display, so this is the run_perf.sh-pipeline camera dance minus the gdm/DRM
# parts: stop VSM, reset the camera pipeline, run the viz binary, relaunch VSM.
#
# PPMs land in $OUT_DIR_ON_BOARD. Any leftover /tmp/attention_self_viz* is
# purged first so nothing accumulates on the Kria.

set -u

OUT_DIR=${1:-}
WARMUP=${2:-30}

if [ -z "$OUT_DIR" ]; then
    cat >&2 <<EOF
usage: $0 <OUT_DIR_ON_BOARD> [WARMUP_FRAMES]
  OUT_DIR_ON_BOARD  directory the viz binary writes stage PPMs into
  WARMUP_FRAMES     camera frames to discard before the captured one (default 30)
EOF
    exit 2
fi

VIZ_BIN=/home/ubuntu/vision_software/libt1/test/attention_self_viz
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
    # Killing VSM mid-stream wedges the CSI subdev, so a full pipeline reset is
    # the only sure way to get a clean camera for the viz binary.
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

echo "=== run_viz out=$OUT_DIR warmup=$WARMUP ==="
echo "  purge stale /tmp/attention_self_viz*"
sudo -n rm -rf /tmp/attention_self_viz /tmp/attention_self_viz-* /tmp/attention_self_viz_* 2>/dev/null || true
rm -rf /tmp/attention_self_viz /tmp/attention_self_viz-* /tmp/attention_self_viz_* 2>/dev/null || true

stop_vsm
reapply_overlay_and_media_ctl

mkdir -p "$OUT_DIR"
sudo -n "$VIZ_BIN" --dev /dev/video0 --warmup "$WARMUP" --out-dir "$OUT_DIR"
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "  ERROR: attention_self_viz failed with rc=$rc" >&2
    start_vsm
    exit "$rc"
fi

NPPM=$(ls -1 "$OUT_DIR"/*.ppm 2>/dev/null | wc -l)
echo "  PPMs written: $NPPM in $OUT_DIR"
start_vsm
