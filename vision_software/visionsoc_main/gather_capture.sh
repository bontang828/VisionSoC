#!/usr/bin/env bash
# gather_capture.sh -- run a screenshot-capture session on the board,
# pull PPMs, convert to PNGs into a per-run timestamped directory.
#
# Usage:
#   ./gather_capture.sh <test_label> [N_FRAMES] [EVERY_M]
#
#   <test_label>   short tag for the run (used in the output dir name).
#                  Suggested: the active kernel name, e.g. `sobel`,
#                  `optical_flow`, `frame_passthrough`.
#   N_FRAMES       frames to run before exit (default 300)
#   EVERY_M        snapshot frame 0, M, 2M, ... (default 30)
#
# Pre-reqs:
#   * The desired T1 kernel has been deployed via `./sync_kernel.sh <name>`.
#   * `./sync_capture.sh` has been run at least once after the last
#     change to main_capture.c / camera.c / display.c / libt1.
#
# Layout produced under capture/:
#   capture/
#     <ISO-UTC>-<test_label>/
#       capture_NNNNNN_camera_y.png       (128x128 grayscale)
#       capture_NNNNNN_camera_color.png   (128x128 YUV->RGB)
#       capture_NNNNNN_t1_output.png      (128x128, rendered same as display)
#       capture_NNNNNN_display.png        (full HDMI framebuffer, eg 1920x1080)
#       run.log         (captured stdout/stderr of run_capture.sh)
#       meta.txt        (label, frames, every, board, bitstream, git head,
#                        active kernel)
#
# Per the user directive, board-side captures are removed after they
# have been pulled back -- nothing accumulates on the Kria.

set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
CAPTURE_DIR="$HERE/capture"

usage() {
    cat >&2 <<EOF
usage: $0 <test_label> [N_FRAMES] [EVERY_M]
  <test_label>   short name for the run, e.g. sobel / optical_flow / passthrough
  N_FRAMES       default 300
  EVERY_M        default 30
EOF
    exit 2
}

if [ $# -lt 1 ]; then
    usage
fi
case "$1" in
    -h|--help|help) usage ;;
esac

LABEL=$1
case "$LABEL" in
    [A-Za-z0-9_][A-Za-z0-9_-]*) ;;
    *) echo "invalid test label: $LABEL" >&2 ; exit 2 ;;
esac
FRAMES=${2:-300}
EVERY=${3:-30}

TS=$(date -u +%Y%m%d-%H%M)
RUN=capture_${LABEL}-${TS}
OUT="$CAPTURE_DIR/$RUN"
REMOTE_OUT="/tmp/$RUN"
mkdir -p "$OUT"

# meta.txt -- everything someone re-opening the dir later needs to
# reproduce / understand the run.
BOARD_HOST=$(ssh kv260 hostname 2>/dev/null || echo unknown)
BITSTREAM=$(ssh kv260 'sudo -n sha256sum /lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin 2>/dev/null | cut -d" " -f1' || echo unknown)
ACTIVE_KERNEL=$(grep -E '^#include' "$HERE/kernels/active_kernel.h" \
                  | sed -E 's|.*"([^"]+)_select\.h".*|\1|' \
                  | head -1)
{
    echo "label=$LABEL"
    echo "frames=$FRAMES"
    echo "every=$EVERY"
    echo "active_kernel=$ACTIVE_KERNEL"
    echo "board_host=$BOARD_HOST"
    echo "bitstream_sha256=$BITSTREAM"
    echo "timestamp_utc=$(date -u +%Y%m%dT%H%M%SZ)"
    echo "host=$(hostname)"
    echo "git_head=$(git -C "$HERE" rev-parse --short HEAD 2>/dev/null || echo unknown)"
} > "$OUT/meta.txt"

echo "=== rebuild visionsoc_main_capture on board (pick up active kernel) ==="
# Idempotent: make rebuilds only what depends on active_kernel.h or its
# transitively-included kernel select header. Required because sync_kernel.sh
# rewrites active_kernel.h + rebuilds visionsoc_main but does NOT rebuild
# visionsoc_main_capture -- without this step, captures would run against
# the previously-staged kernel.
ssh kv260 "cd ~/vision_software/visionsoc_main && \
           make visionsoc_main_capture && \
           cp visionsoc_main_capture ~/vision_software/libt1/test/" \
        2>&1 | tail -5

echo "=== run_capture.sh $REMOTE_OUT $FRAMES $EVERY (on kv260) -> $OUT/ ==="
set +e
ssh kv260 "~/vision_software/visionsoc_main/run_capture.sh \
           $REMOTE_OUT $FRAMES $EVERY" 2>&1 | tee "$OUT/run.log"
SSH_RC=${PIPESTATUS[0]}
set -e
if [ "$SSH_RC" -ne 0 ]; then
    echo "WARNING: run_capture.sh exited $SSH_RC; see $OUT/run.log" >&2
    # Try to pull whatever did land, then clean board.
fi

echo "=== fetch PPMs ==="
# -p preserves mtimes so frame ordering by timestamp still works locally
# if the PPMs end up with sequential mtimes; -r in case scp pulls a dir.
if ssh kv260 "ls -1 $REMOTE_OUT/capture_*.ppm 2>/dev/null | head -1 | grep -q ."; then
    scp -p "kv260:$REMOTE_OUT/capture_*.ppm" "$OUT/"
else
    echo "WARNING: no PPMs found on board at $REMOTE_OUT/" >&2
fi

echo "=== clean board (no accumulation) ==="
# User directive: never leave captures sitting on the Kria.
ssh kv260 "rm -rf $REMOTE_OUT 2>/dev/null; \
           sudo -n rm -rf $REMOTE_OUT 2>/dev/null; true"

NPPM=$(ls -1 "$OUT"/capture_*.ppm 2>/dev/null | wc -l)
if [ "$NPPM" -eq 0 ]; then
    echo "no PPMs to convert; exiting with run rc=$SSH_RC" >&2
    exit "${SSH_RC:-1}"
fi

echo "=== convert PPM -> PNG ($NPPM files) ==="
# Inline Python (Pillow) conversion. Removes the PPMs after a
# successful PNG write to keep the repo light -- PPMs are ~6MB for
# full-screen display captures, PNGs are ~5-200KB.
python3 - "$OUT" <<'PYEOF'
import sys, os, glob
from PIL import Image
out_dir = sys.argv[1]
ppms = sorted(glob.glob(os.path.join(out_dir, "capture_*.ppm")))
converted = 0
for p in ppms:
    png = p[:-4] + ".png"
    try:
        with Image.open(p) as im:
            im.save(png, "PNG", optimize=True)
        os.remove(p)
        converted += 1
    except Exception as e:
        print(f"  WARN: {os.path.basename(p)}: {e}", file=sys.stderr)
print(f"  converted {converted}/{len(ppms)} files")
PYEOF

cat <<EOF

== done -- $OUT ==
$(ls -1 "$OUT")

EOF

exit "$SSH_RC"
