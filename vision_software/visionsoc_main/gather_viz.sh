#!/usr/bin/env bash
# gather_viz.sh -- build + run the attention_self stage visualiser on the
# board, pull the stage PPMs back, and compose a labelled flow diagram.
#
# Usage:
#   ./gather_viz.sh [WARMUP_FRAMES]
#
#   WARMUP_FRAMES   camera frames discarded before the captured one (default 30)
#
# This is the viz sibling of gather_capture.sh / gather_perf.sh. It:
#   [1] scp the viz source + camera deps + Makefile + run_viz.sh + the
#       attention kernel headers to the Kria
#   [2] native aarch64 build of attention_self_viz, staged in libt1/test/
#   [3] run_viz.sh: reset the camera pipeline, grab ONE live frame, dump the
#       128x128 data plane at every stage to PPMs, relaunch visionsoc_main
#   [4] pull PPMs into viz/<UTC>-attention_self/, clean the board
#   [5] convert each PPM -> PNG and compose flow_diagram.png
#
# It does NOT change the attention_self kernel: stage capture uses the
# kernel's own dbg checkpoints (attention_patch_issue dbg=1).
#
# NOTE: the ACTIVE T1 kernel is irrelevant here -- attention_self_viz is a
# standalone binary that runs the attention_self pipeline itself. But the
# camera/bitstream must be the deployed VisionSoC overlay (run_viz.sh
# re-applies it).

set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
REMOTE="kv260:~/vision_software/visionsoc_main"
VIZ_DIR="$HERE/viz"
PLOTTER="$HERE/perf/plot_viz.py"

WARMUP=${1:-30}

TS=$(date -u +%Y%m%d-%H%M)
RUN="viz_attention_self-${TS}"
OUT="$VIZ_DIR/$RUN"
REMOTE_OUT="/tmp/$RUN"
mkdir -p "$OUT"

echo "[1/5] scp viz sources -> board"
scp "$HERE"/attention_self_viz.c "$HERE"/Makefile "$HERE"/run_viz.sh \
    "$HERE"/camera.c "$HERE"/camera.h \
    "$REMOTE/"
# libt1 + attention kernel headers the viz binary includes.
scp "$HERE"/../libt1/libt1.c "$HERE"/../libt1/libt1.h "$HERE"/../libt1/libt1_regs.h \
    "kv260:~/vision_software/libt1/"
scp "$HERE"/kernels/attention_patch_weights.h \
    "$HERE"/kernels/attention_patch_im2col.h \
    "$HERE"/kernels/attention_patch_im2col_issue.h \
    "$HERE"/kernels/attention_self.h \
    "$HERE"/kernels/attention_self_transpose.h \
    "$HERE"/kernels/attention_patch_issue.h \
    "kv260:~/vision_software/visionsoc_main/kernels/"

echo "[2/5] native build on board (libt1.a first, then attention_self_viz)"
ssh kv260 "make -C ~/vision_software/libt1 libt1.a && \
           cd ~/vision_software/visionsoc_main && \
           chmod +x run_viz.sh && \
           make attention_self_viz && \
           cp attention_self_viz ~/vision_software/libt1/test/"

# meta.txt
BOARD_HOST=$(ssh kv260 hostname 2>/dev/null || echo unknown)
BITSTREAM=$(ssh kv260 'sudo -n sha256sum /lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin 2>/dev/null | cut -d" " -f1' || echo unknown)
{
    echo "kind=attention_self_viz"
    echo "warmup=$WARMUP"
    echo "board_host=$BOARD_HOST"
    echo "bitstream_sha256=$BITSTREAM"
    echo "timestamp_utc=$(date -u +%Y%m%dT%H%M%SZ)"
    echo "host=$(hostname)"
    echo "git_head=$(git -C "$HERE" rev-parse --short HEAD 2>/dev/null || echo unknown)"
} > "$OUT/meta.txt"

echo "[3/5] run_viz.sh $REMOTE_OUT $WARMUP (on kv260) -> $OUT/"
set +e
ssh kv260 "~/vision_software/visionsoc_main/run_viz.sh $REMOTE_OUT $WARMUP" \
        2>&1 | tee "$OUT/run.log"
SSH_RC=${PIPESTATUS[0]}
set -e
if [ "$SSH_RC" -ne 0 ]; then
    echo "WARNING: run_viz.sh exited $SSH_RC; see $OUT/run.log" >&2
fi

echo "[4/5] fetch PPMs + clean board"
if ssh kv260 "ls -1 $REMOTE_OUT/*.ppm 2>/dev/null | head -1 | grep -q ."; then
    scp -p "kv260:$REMOTE_OUT/*.ppm" "$OUT/"
else
    echo "WARNING: no PPMs found on board at $REMOTE_OUT/" >&2
fi
ssh kv260 "rm -rf $REMOTE_OUT 2>/dev/null; sudo -n rm -rf $REMOTE_OUT 2>/dev/null; true"

NPPM=$(ls -1 "$OUT"/*.ppm 2>/dev/null | wc -l)
if [ "$NPPM" -eq 0 ]; then
    echo "no PPMs to render; exiting rc=$SSH_RC" >&2
    exit "${SSH_RC:-1}"
fi

echo "[5/5] compose flow diagrams + convert PPM -> PNG ($NPPM PPMs)"
python3 "$PLOTTER" "$OUT" --style landscape --out "$OUT/flow_landscape.png"
python3 "$PLOTTER" "$OUT" --style flow      --out "$OUT/flow_diagram.png"
# Also keep per-stage PNGs for individual inspection.
python3 - "$OUT" <<'PYEOF'
import sys, os, glob
from PIL import Image
out_dir = sys.argv[1]
for p in sorted(glob.glob(os.path.join(out_dir, "*.ppm"))):
    try:
        with Image.open(p) as im:
            im.save(p[:-4] + ".png", "PNG", optimize=True)
    except Exception as e:
        print(f"  WARN: {os.path.basename(p)}: {e}", file=sys.stderr)
print("  per-stage PNGs written")
PYEOF

cat <<EOF

== done -- $OUT ==
$(ls -1 "$OUT")

EOF
exit "$SSH_RC"
