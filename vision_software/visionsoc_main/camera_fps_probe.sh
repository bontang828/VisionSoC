#!/usr/bin/env bash
# Probe whether the KV260 AP1302/AR1335 stack can deliver 60 fps frames.
#
# This is intentionally separate from visionsoc_main: it stops the live app,
# runs short mediasrcbin captures at requested frame rates, reports actual
# bytes/time-derived fps, and then restarts visionsoc_main if it was running.

set -u

MEDIA_DEV=${MEDIA_DEV:-/dev/media0}
VIDEO_DEV=${VIDEO_DEV:-/dev/video0}
AP1302_DEV=${AP1302_DEV:-/dev/v4l-subdev1}
APP=${APP:-/home/ubuntu/vision_software/visionsoc_main/visionsoc_main}
BIT=${BIT:-/lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin}
DTBO=${DTBO:-/lib/firmware/xilinx/visionsoc/system_top_wrapper.dtbo}
REAPPLY_OVERLAY=${REAPPLY_OVERLAY:-1}
RESET_BETWEEN_CAPTURES=${RESET_BETWEEN_CAPTURES:-1}
CAPTURE_SECONDS=${CAPTURE_SECONDS:-6}
FPS_LIST=${FPS_LIST:-"30 60"}
WIDTH=${WIDTH:-128}
HEIGHT=${HEIGHT:-128}
FORMAT=${FORMAT:-NV12}
BYTES_PER_FRAME=$((WIDTH * HEIGHT * 3 / 2))
TMPDIR=${TMPDIR:-/tmp}

have() {
    command -v "$1" >/dev/null 2>&1
}

run_nonfatal() {
    echo "+ $*"
    "$@" 2>&1 || true
}

log_status() {
    local label=$1
    echo "=== AP1302 status: $label ==="
    if have v4l2-dbg && [ -e "$AP1302_DEV" ]; then
        sudo -n v4l2-dbg --log-status -d "$AP1302_DEV" 2>&1 | \
            grep -Ei 'Frame counters|ICP|HINF|BRAC|error|warn|sys_start' || true
    else
        echo "v4l2-dbg or $AP1302_DEV not available"
    fi
}

capture_one() {
    local fps=$1
    local out="$TMPDIR/visionsoc_cam_${WIDTH}x${HEIGHT}_${fps}fps.nv12"
    local log="$TMPDIR/visionsoc_cam_${WIDTH}x${HEIGHT}_${fps}fps.gst.log"
    local start_ns end_ns elapsed_us size frames actual

    rm -f "$out" "$log"
    echo "=== capture request: ${WIDTH}x${HEIGHT} ${FORMAT} ${fps}/1 for ${CAPTURE_SECONDS}s ==="
    log_status "before ${fps}/1"

    start_ns=$(date +%s%N)
    timeout -s INT "${CAPTURE_SECONDS}s" \
        gst-launch-1.0 -q -e mediasrcbin name=videosrc media-device="$MEDIA_DEV" \
        ! "video/x-raw,width=${WIDTH},height=${HEIGHT},format=${FORMAT},framerate=${fps}/1" \
        ! filesink location="$out" >"$log" 2>&1
    local rc=$?
    end_ns=$(date +%s%N)

    elapsed_us=$(( (end_ns - start_ns) / 1000 ))
    size=0
    if [ -f "$out" ]; then
        size=$(stat -c '%s' "$out")
    fi
    frames=$(( size / BYTES_PER_FRAME ))
    actual=$(awk -v f="$frames" -v us="$elapsed_us" 'BEGIN { if (us > 0) printf "%.2f", f * 1000000.0 / us; else printf "0.00" }')

    echo "gst rc=$rc elapsed_us=$elapsed_us bytes=$size frames=$frames actual_fps=$actual"
    if [ -s "$log" ]; then
        echo "--- gst log tail (${fps}/1) ---"
        tail -40 "$log"
    fi
    log_status "after ${fps}/1"
    echo
}

apply_overlay_and_media() {
    echo "=== reapplying VisionSoC overlay ==="
    sudo -n rmdir /sys/kernel/config/device-tree/overlays/full 2>/dev/null || true
    sudo -n rmdir /sys/kernel/config/device-tree/overlays/k26-starter-kits_image_1 2>/dev/null || true
    sudo -n fpgautil -b "$BIT" -o "$DTBO"

    echo "=== waiting for media devices ==="
    for _ in $(seq 1 40); do
        if [ -e "$MEDIA_DEV" ] && [ -e "$VIDEO_DEV" ] &&
           [ -e "$AP1302_DEV" ] && [ -e /dev/v4l-subdev2 ]; then
            break
        fi
        sleep 0.5
    done

    echo "=== applying 128x128 media topology ==="
    sudo -n media-ctl -d "$MEDIA_DEV" \
        -V '"ap1302.4-003c":2 [fmt:VYYUYY8_1X24/128x128 field:none colorspace:srgb]'
    sudo -n media-ctl -d "$MEDIA_DEV" \
        -V '"80000000.csiss":0 [fmt:VYYUYY8_1X24/128x128 field:none colorspace:srgb]'
    sudo -n media-ctl -d "$MEDIA_DEV" \
        -V '"80000000.csiss":1 [fmt:VYYUYY8_1X24/128x128 field:none colorspace:srgb]'
}

echo "=== camera_fps_probe ==="
echo "device: media=$MEDIA_DEV video=$VIDEO_DEV ap1302=$AP1302_DEV"
echo "capture: ${WIDTH}x${HEIGHT} ${FORMAT}, ${BYTES_PER_FRAME} bytes/frame"

if ! have gst-launch-1.0 || ! gst-inspect-1.0 mediasrcbin >/dev/null 2>&1; then
    echo "ERROR: gst-launch-1.0 or mediasrcbin is missing" >&2
    exit 1
fi

was_running=0
if pgrep -x visionsoc_main >/dev/null 2>&1; then
    was_running=1
    echo "=== stopping visionsoc_main ==="
    sudo -n pkill -TERM visionsoc_main 2>/dev/null || true
    sleep 1
    sudo -n pkill -KILL visionsoc_main 2>/dev/null || true
fi

echo "=== clearing camera device owners ==="
sudo -n fuser -v "$VIDEO_DEV" 2>&1 || true
sudo -n fuser -k "$VIDEO_DEV" 2>/dev/null || true
sleep 1

if [ "$REAPPLY_OVERLAY" -ne 0 ]; then
    apply_overlay_and_media
fi

restart_app() {
    if [ "$was_running" -eq 1 ] && [ -x "$APP" ]; then
        echo "=== restarting visionsoc_main ==="
        nohup sudo -n "$APP" "$VIDEO_DEV" </dev/null >>/tmp/vsm.log 2>&1 &
    fi
}
trap restart_app EXIT

echo "=== current V4L2/media state ==="
run_nonfatal v4l2-ctl -d "$VIDEO_DEV" --all
run_nonfatal v4l2-ctl -d "$VIDEO_DEV" --list-formats-ext
run_nonfatal v4l2-ctl -d "$VIDEO_DEV" --get-parm
run_nonfatal media-ctl -p -d "$MEDIA_DEV"

echo "=== nonfatal frame-rate API probes ==="
run_nonfatal v4l2-ctl -d "$AP1302_DEV" --set-subdev-fps pad=2,fps=60
run_nonfatal v4l2-ctl -d "$AP1302_DEV" --get-subdev-fps 2
run_nonfatal v4l2-ctl -d "$VIDEO_DEV" --set-parm=60
run_nonfatal v4l2-ctl -d "$VIDEO_DEV" --get-parm

for fps in $FPS_LIST; do
    if [ "$RESET_BETWEEN_CAPTURES" -ne 0 ]; then
        apply_overlay_and_media
    fi
    capture_one "$fps"
done

echo "=== done ==="
