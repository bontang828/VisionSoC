#!/usr/bin/env bash
# sync_capture.sh -- sync + native-build the capture binary on the board.
#
# Usage:  ./sync_capture.sh
#
# Sibling of sync_perf.sh, but for visionsoc_main_capture (the
# screenshot-saving twin of main.c) plus the on-board runner. The
# active T1 vector kernel is NOT touched -- swap that beforehand with
# ./sync_kernel.sh <name> if needed.
#
# What this does:
#   [1] scp main_capture.c + camera/display/cnn2d deps + run_capture.sh
#       + Makefile to the Kria
#   [2] on the board: native aarch64 build of visionsoc_main_capture
#   [3] stage the binary in ~/vision_software/libt1/test/ (NOPASSWD-eligible
#       under the existing sudoers rule)
#
# After this finishes, kick off a capture run from the host with:
#   ./gather_capture.sh <test_label> [N_FRAMES] [EVERY_M]
# which orchestrates the on-board run, pulls PPMs back into
# capture/<UTC>-<test>/, and converts them to PNGs.

set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
REMOTE="kv260:~/vision_software/visionsoc_main"

echo "[1/3] scp capture sources -> board"
scp "$HERE"/main_capture.c "$HERE"/Makefile "$HERE"/run_capture.sh \
    "$REMOTE/"
# Shared C deps (camera/display/cnn2d) are also linked into
# visionsoc_main_capture, so keep them in lockstep.
scp "$HERE"/camera.c "$HERE"/camera.h \
    "$HERE"/display.c "$HERE"/display.h \
    "$HERE"/cnn2d_decoder.c "$HERE"/cnn2d_decoder.h \
    "$REMOTE/"
# libt1 is also linked in; keep in sync so the board doesn't build
# against a stale lib if libt1.c / headers drifted.
scp "$HERE"/../libt1/libt1.c "$HERE"/../libt1/libt1.h "$HERE"/../libt1/libt1_regs.h \
    "kv260:~/vision_software/libt1/"

echo "[2/3] native build on board (libt1.a first, then visionsoc_main_capture)"
ssh kv260 "make -B -C ~/vision_software/libt1 libt1.a && \
           cd ~/vision_software/visionsoc_main && \
           chmod +x run_capture.sh && \
           make -B visionsoc_main_capture"

echo "[3/3] stage binary in NOPASSWD-eligible dir"
ssh kv260 "cp ~/vision_software/visionsoc_main/visionsoc_main_capture \
              ~/vision_software/libt1/test/"

cat <<'EOF'

== done -- visionsoc_main_capture staged at /home/ubuntu/vision_software/libt1/test/ ==

To kick off a capture run (writes a timestamped subdir under capture/
with PPMs + converted PNGs):

  ./gather_capture.sh <test_label> [N_FRAMES] [EVERY_M]

  N_FRAMES default 300, EVERY_M default 30 -> 11 captures per run
  (frame 0, 30, 60, ..., 300).

Output layout:
  capture/<UTC-timestamp>-<test_label>/
    capture_NNNNNN_camera_y.ppm        + .png
    capture_NNNNNN_camera_color.ppm    + .png
    capture_NNNNNN_t1_output.ppm       + .png
    capture_NNNNNN_display.ppm         + .png
    run.log
    meta.txt   (test label, frames, every, board hostname, bitstream)

The active T1 kernel is whatever ./sync_kernel.sh last set on the
board. Swap kernels beforehand with `./sync_kernel.sh <name>`.

EOF
