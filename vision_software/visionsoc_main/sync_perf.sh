#!/usr/bin/env bash
# sync_perf.sh -- sync + native-build the perf binaries on the board.
#
# Usage:  ./sync_perf.sh
#
# Sibling of sync_kernel.sh, but for the perf binaries (main_perf,
# sobel_perf, optical_flow_perf, etc.) rather than the T1 vector kernel.
# The active vector
# kernel is left alone -- this only touches host-C sources.
#
# What this does:
#   [1] scp the perf C sources + Makefile + on-board run helper to the Kria
#   [2] on the board: native aarch64 build of visionsoc_main_perf + sobel_perf
#   [3] stage the binaries in ~/vision_software/libt1/test/ (NOPASSWD-eligible
#       under the existing sudoers rule)
#   [4] scp the plot_perf.py to the board (optional; only needed if you
#       want to render charts on-board instead of pulling CSVs back)
#
# After this finishes, kick off a measurement with gather_perf.sh which
# bundles run + pull + plot into a per-run timestamped directory under perf/:
#   ./gather_perf.sh sobel
#   ./gather_perf.sh pipeline
#   ./gather_perf.sh both
#
# (Or, for ad-hoc runs without the per-run dir, invoke run_perf.sh directly:
#   ssh kv260 ~/vision_software/visionsoc_main/run_perf.sh sobel
# CSVs land at /tmp/{sobel,pipeline}_perf.csv on the board.)

set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
REMOTE="kv260:~/vision_software/visionsoc_main"
REMOTE_TEST="kv260:~/vision_software/libt1/test"

echo "[1/4] scp perf sources -> board"
scp "$HERE"/main_perf.c "$HERE"/sobel_perf.c "$HERE"/optical_flow_perf.c \
    "$HERE"/matmul_8bitraw_short_perf.c \
    "$HERE"/Makefile "$HERE"/run_perf.sh \
    "$REMOTE/"
# Also sync the C deps in case they drifted (camera/display/cnn2d are also
# linked into visionsoc_main_perf). Sync standalone-kernel sources too so
# their perf binaries can pick up regenerated .h files via Makefile rules.
scp "$HERE"/camera.c "$HERE"/camera.h \
    "$HERE"/display.c "$HERE"/display.h \
    "$HERE"/cnn2d_decoder.c "$HERE"/cnn2d_decoder.h \
    "$REMOTE/"
scp "$HERE"/kernels/optical_flow.S "$HERE"/kernels/optical_flow_select.h \
    "$HERE"/kernels/flow_color_rgb565.S \
    "$HERE"/kernels/flow_color_rgb565_dispatch.h \
    "$HERE"/kernels/matmul.S \
    "$HERE"/kernels/matmul.h \
    "$HERE"/kernels/matmul_select.h \
    "$HERE"/kernels/matmul_8bitraw.S \
    "$HERE"/kernels/matmul_8bitraw.h \
    "$HERE"/kernels/matmul_8bitraw_select.h \
    "$HERE"/kernels/matmul_8bitraw_short.S \
    "$HERE"/kernels/matmul_8bitraw_short.h \
    "$HERE"/kernels/matmul_8bitraw_short_select.h \
    "$HERE"/kernels/matmul_weights.h \
    "kv260:~/vision_software/visionsoc_main/kernels/"
# The perf binaries link ../libt1/libt1.a, so keep libt1 in lockstep too.
# t1_wait_ready() (per-instruction drain wait) lives here; without this the
# board would build against a stale lib and the perf fix would be a no-op.
scp "$HERE"/../libt1/libt1.c "$HERE"/../libt1/libt1.h "$HERE"/../libt1/libt1_regs.h \
    "kv260:~/vision_software/libt1/"

echo "[2/4] native build on board (libt1.a first, then perf binaries)"
ssh kv260 "make -B -C ~/vision_software/libt1 libt1.a && \
           cd ~/vision_software/visionsoc_main && \
           chmod +x run_perf.sh && \
           make -B visionsoc_main_perf sobel_perf optical_flow_perf \
                   matmul_8bitraw_short_perf"

echo "[3/4] stage binaries in NOPASSWD-eligible dir"
ssh kv260 "cp ~/vision_software/visionsoc_main/visionsoc_main_perf \
              ~/vision_software/visionsoc_main/sobel_perf \
              ~/vision_software/visionsoc_main/optical_flow_perf \
              ~/vision_software/visionsoc_main/matmul_8bitraw_short_perf \
              ~/vision_software/libt1/test/"

echo "[4/4] scp plot_perf.py -> board (optional)"
scp "$HERE"/perf/plot_perf.py "$REMOTE/perf/" 2>/dev/null || \
    ssh kv260 "mkdir -p ~/vision_software/visionsoc_main/perf" && \
    scp "$HERE"/perf/plot_perf.py "$REMOTE/perf/"

cat <<'EOF'

== done -- perf binaries staged at /home/ubuntu/vision_software/libt1/test/{sobel_perf,optical_flow_perf,matmul_8bitraw_short_perf,visionsoc_main_perf} ==

To kick off a measurement run (recommended -- one command, writes a
timestamped subdir under perf/ with CSVs + rendered charts):

  ./gather_perf.sh sobel        [N_ITERS]    # default 100
  ./gather_perf.sh optical_flow [N_ITERS]    # default 100
  ./gather_perf.sh matmul_8bitraw_short [N]  # default 100
  ./gather_perf.sh pipeline     [N_FRAMES]   # default 30
  ./gather_perf.sh both         [N]          # sobel + pipeline, in one dir

Output layout:
  perf/<UTC-timestamp>-<mode>/
    sobel_perf.csv  / optical_flow_perf.csv  / matmul_8bitraw_short_perf.csv  / pipeline_perf.csv
    sobel_breakdown.{per_instr,grouped}.png
    optical_flow_breakdown.{per_instr,grouped}.png
    matmul_8bitraw_short_breakdown.{per_instr,grouped}.png
    pipeline_breakdown.png
    run.log
    meta.txt  (mode, N, t1-hz, board, bitstream sha256, git head)

Ad-hoc (skip the per-run dir) -- runs straight on the board:
  ssh kv260 ~/vision_software/visionsoc_main/run_perf.sh \
      {sobel|optical_flow|matmul_8bitraw_short|pipeline|both} [N]

EOF
