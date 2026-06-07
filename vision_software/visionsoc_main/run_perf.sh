#!/usr/bin/env bash
# run_perf.sh -- kick off a perf measurement on the board.
#
# Usage:
#   ./run_perf.sh sobel    [N_ITERS]
#   ./run_perf.sh optical_flow [N_ITERS]
#   ./run_perf.sh matmul_8bitraw_short [N_ITERS]
#   ./run_perf.sh pipeline [N_FRAMES]
#   ./run_perf.sh both     [N]
#
# Runs on the board (typical invocation: ssh kv260 ~/.../run_perf.sh ...).
# Uses NOPASSWD sudo rules for everything that needs root (pkill, fpgautil,
# media-ctl, /home/ubuntu/vision_software/libt1/test/*,
# /home/ubuntu/vision_software/visionsoc_main/visionsoc_main).
#
# Pipeline mode does the full camera/DRM dance because main_perf has to be
# the only DRM client AND has to come up after a fresh media-ctl topology.
# Sobel mode just needs T1 free (no camera/display touch).
#
# After either mode finishes, production visionsoc_main is relaunched so
# the HDMI sobel feed returns. CSVs land in /tmp/*_perf.csv on the board.

set -u

MODE=${1:-help}
N=${2:-}
PERF_BIN_DIR=/home/ubuntu/vision_software/libt1/test
VSM=/home/ubuntu/vision_software/visionsoc_main/visionsoc_main
BIT=/lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin
DTBO=/lib/firmware/xilinx/visionsoc/system_top_wrapper.dtbo

usage() {
    cat >&2 <<EOF
usage: $0 {sobel|optical_flow|matmul_8bitraw_short|pipeline|both} [N]
  sobel        [N_ITERS]   standalone T1 timing of the sobel kernel (default 100)
  optical_flow [N_ITERS]   standalone T1 timing of the 5-direction
                           block-matching optical-flow kernel (default 100)
  matmul_8bitraw_short [N_ITERS]
                          standalone T1 timing of the short raw-8 matmul
                          kernel, with body rows aggregated x128 (default 100)
  pipeline     [N_FRAMES]  full camera+display loop (default 30)
  both         [N]         sobel N iters then pipeline N frames
EOF
    exit 2
}

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
    # When something stale-streams the camera, the only sure reset is to
    # remove the overlay and re-apply the bitstream. Cheap to do (~500ms
    # fpgautil) and idempotent.
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
    # gdm grabs DRM master within ~100ms of the previous owner releasing it,
    # which races with main_perf. Mask so it can't auto-restart for this run.
    if systemctl is-active --quiet gdm 2>/dev/null; then
        echo "  mask gdm (was active)"
        sudo -n systemctl stop gdm 2>&1 | tail -2 || true
    fi
    sudo -n systemctl mask gdm 2>&1 | tail -2 || true
}

run_sobel() {
    local iters=${1:-100}
    local rc
    echo "=== run_sobel iters=$iters ==="
    stop_vsm
    sudo -n "$PERF_BIN_DIR/sobel_perf" --iters "$iters" --out /tmp/sobel_perf.csv
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "  ERROR: sobel_perf failed with rc=$rc" >&2
        start_vsm
        return "$rc"
    fi
    if [ ! -f /tmp/sobel_perf.csv ]; then
        echo "  ERROR: sobel_perf did not create /tmp/sobel_perf.csv" >&2
        start_vsm
        return 1
    fi
    echo "  CSV: /tmp/sobel_perf.csv ($(wc -l </tmp/sobel_perf.csv) lines)"
    start_vsm
}

run_optical_flow() {
    local iters=${1:-100}
    local rc
    echo "=== run_optical_flow iters=$iters ==="
    stop_vsm
    sudo -n "$PERF_BIN_DIR/optical_flow_perf" \
        --iters "$iters" --out /tmp/optical_flow_perf.csv
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "  ERROR: optical_flow_perf failed with rc=$rc" >&2
        start_vsm
        return "$rc"
    fi
    if [ ! -f /tmp/optical_flow_perf.csv ]; then
        echo "  ERROR: optical_flow_perf did not create /tmp/optical_flow_perf.csv" >&2
        start_vsm
        return 1
    fi
    echo "  CSV: /tmp/optical_flow_perf.csv ($(wc -l </tmp/optical_flow_perf.csv) lines)"
    start_vsm
}

run_matmul_8bitraw_short() {
    local iters=${1:-100}
    local rc
    echo "=== run_matmul_8bitraw_short iters=$iters ==="
    stop_vsm
    sudo -n "$PERF_BIN_DIR/matmul_8bitraw_short_perf" \
        --iters "$iters" --out /tmp/matmul_8bitraw_short_perf.csv
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "  ERROR: matmul_8bitraw_short_perf failed with rc=$rc" >&2
        start_vsm
        return "$rc"
    fi
    if [ ! -f /tmp/matmul_8bitraw_short_perf.csv ]; then
        echo "  ERROR: matmul_8bitraw_short_perf did not create /tmp/matmul_8bitraw_short_perf.csv" >&2
        start_vsm
        return 1
    fi
    echo "  CSV: /tmp/matmul_8bitraw_short_perf.csv ($(wc -l </tmp/matmul_8bitraw_short_perf.csv) lines)"
    start_vsm
}

run_pipeline() {
    local frames=${1:-30}
    local rc
    echo "=== run_pipeline frames=$frames ==="
    stop_vsm
    mask_gdm_if_active
    reapply_overlay_and_media_ctl
    sudo -n "$PERF_BIN_DIR/visionsoc_main_perf" --frames "$frames" --out /tmp/pipeline_perf.csv
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "  ERROR: visionsoc_main_perf failed with rc=$rc" >&2
        start_vsm
        return "$rc"
    fi
    if [ ! -f /tmp/pipeline_perf.csv ]; then
        echo "  ERROR: visionsoc_main_perf did not create /tmp/pipeline_perf.csv" >&2
        start_vsm
        return 1
    fi
    echo "  CSV: /tmp/pipeline_perf.csv ($(wc -l </tmp/pipeline_perf.csv) lines)"
    start_vsm
}

run_attention_self() {
    local iters=${1:-20}
    local rc
    echo "=== run_attention_self iters=$iters ==="
    stop_vsm
    sudo -n "$PERF_BIN_DIR/attention_self_perf" --iters "$iters" --out /tmp/attention_self_perf.csv
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "  ERROR: attention_self_perf failed with rc=$rc" >&2
        reapply_overlay_and_media_ctl
        start_vsm
        return "$rc"
    fi
    if [ ! -f /tmp/attention_self_perf.csv ]; then
        echo "  ERROR: attention_self_perf did not create /tmp/attention_self_perf.csv" >&2
        reapply_overlay_and_media_ctl
        start_vsm
        return 1
    fi
    echo "  CSV: /tmp/attention_self_perf.csv ($(wc -l </tmp/attention_self_perf.csv) lines)"
    # Killing VSM mid-stream wedges the CSI subdev (STREAMON -> Broken pipe on
    # the next open), so the camera pipeline must be reset before VSM can come
    # back -- same recovery run_pipeline does. The perf binary itself never
    # touches the camera; this is purely to restore the live display feed.
    reapply_overlay_and_media_ctl
    start_vsm
}

case "$MODE" in
    sobel)        run_sobel                   "${N:-100}" ;;
    optical_flow) run_optical_flow            "${N:-100}" ;;
    matmul_8bitraw_short)
                  run_matmul_8bitraw_short   "${N:-100}" ;;
    attention_self) run_attention_self        "${N:-20}"  ;;
    pipeline)     run_pipeline                "${N:-30}"  ;;
    both)
        run_sobel    "${N:-100}"
        run_pipeline "${N:-30}"
        ;;
    -h|--help|help|*) usage ;;
esac
