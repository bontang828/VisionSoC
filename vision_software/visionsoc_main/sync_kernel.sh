#!/usr/bin/env bash
# sync_kernel.sh -- swap the active T1 kernel in one command.
#
# Usage:  ./sync_kernel.sh <kernel_name>
#
# Pre-reqs per kernel (one-time, hand-written):
#   * kernels/<name>.S          -- the vector-only assembly. NO csrw 0x7c0;
#                                  vertical mode is set per-issue via
#                                  op.vertical_mode in the select.h instead.
#   * kernels/<name>_select.h   -- per-word issue function declaring
#                                  static inline int issue_active_kernel(src, dst)
#                                  + ACTIVE_KERNEL_NEUTRAL_UV (1=grayscale,
#                                  0=keep camera chroma). See sobel_select.h
#                                  or frame_passthrough_select.h.
#
# What this script does, in order:
#   [1] rewrite kernels/active_kernel.h locally to #include "<name>_select.h"
#   [2] scp the source tree (main.c, *.h, Makefile, all kernels/*.S/.h) to the Kria
#   [3] on the board: run build_kernel.sh to generate kernels/<name>.h, then make.
#       The build happens NATIVELY on aarch64 -- the dev host's x86-64 gcc would
#       produce an unrunnable binary (ENOEXEC). Make running on the board uses
#       its own gcc + libdrm-dev + riscv64-linux-gnu-as.
#   [4] scp the assembler-verified kernels/<name>.h back to local so the repo
#       stays in sync.
#
# After this finishes, on the board (PuTTY/SSH):
#   cd ~/vision_software/visionsoc_main
#   ./run_after_power_cycle.sh
# (cold reload: drops overlays, reloads VisionSoC, configures media-ctl, stops
# GDM, runs ./visionsoc_main; Ctrl-C / SIGTERM restores GDM via trap.)
#
# Notes
#   * Make sure visionsoc_main is NOT running on the board when you invoke this
#     script -- step 3 overwrites the binary, which fails with "Text file busy"
#     if a running instance is holding it.

set -euo pipefail

if [ $# -ne 1 ]; then
    echo "usage: $0 <kernel_name>" >&2
    exit 2
fi
NAME=$1
case "$NAME" in
    [A-Za-z_][A-Za-z0-9_]*) ;;
    *) echo "invalid kernel name: $NAME" >&2 ; exit 2 ;;
esac

HERE=$(cd "$(dirname "$0")" && pwd)
LOCAL_S=$HERE/kernels/$NAME.S
LOCAL_SEL=$HERE/kernels/${NAME}_select.h
LOCAL_H=$HERE/kernels/$NAME.h
LOCAL_ACTIVE=$HERE/kernels/active_kernel.h

# Quoting matters: unquoted `:~/` in a bash assignment triggers tilde
# expansion to the LOCAL $HOME, which is the wrong account on the board.
# Quoted, the ~ stays literal and the remote shell expands it for ubuntu.
REMOTE="kv260:~/vision_software/visionsoc_main"
REMOTE_KERNELS="$REMOTE/kernels"

if [ ! -f "$LOCAL_S" ]; then
    echo "missing: $LOCAL_S -- write the kernel assembly first." >&2
    exit 1
fi
if [ ! -f "$LOCAL_SEL" ]; then
    echo "missing: $LOCAL_SEL" >&2
    echo "Hand-write the per-word issue schedule (rs1 + op.vertical_mode)" >&2
    echo "in kernels/${NAME}_select.h. Use sobel_select.h as a template." >&2
    exit 1
fi

echo "[1/4] active_kernel.h -> ${NAME}_select.h"
# Match only the actual #include directive so the comment block above (which
# legitimately mentions multiple select.h files as examples) is untouched.
sed -i -E 's|^#include "[^"]+_select\.h"|#include "'"${NAME}"'_select.h"|' "$LOCAL_ACTIVE"
grep -E '^#include' "$LOCAL_ACTIVE"

echo "[2/4] scp sources -> board"
# Sources at the visionsoc_main root.
scp "$HERE"/main.c "$HERE"/camera.c "$HERE"/camera.h \
    "$HERE"/display.c "$HERE"/display.h "$HERE"/display_smoke.c \
    "$HERE"/cnn2d_decoder.c "$HERE"/cnn2d_decoder.h \
    "$HERE"/Makefile \
    "$REMOTE/"
# Kernels (all .S and .h, including active_kernel.h, all *_select.h, and the
# already-built .h's). On first-time use of a new kernel its .h won't exist
# locally yet -- step 3 generates it on the board, step 4 copies it back.
scp "$HERE"/kernels/*.S "$HERE"/kernels/*.h "$REMOTE_KERNELS/"

echo "[3/4] build_kernel.sh + make on board (native aarch64)"
ssh kv260 "cd ~/vision_software/visionsoc_main && \
           ../libt1/build_kernel.sh kernels/$NAME.S kernels/$NAME.h $NAME && \
           make -B"

echo "[4/4] scp $NAME.h -> local (assembler-verified)"
scp "$REMOTE_KERNELS/$NAME.h" "$LOCAL_H"
if ssh kv260 "test -f ~/vision_software/visionsoc_main/kernels/flow_color_rgb565.h"; then
    scp "$REMOTE_KERNELS/flow_color_rgb565.h" "$HERE/kernels/flow_color_rgb565.h"
fi

cat <<EOF

== done -- active kernel is now: $NAME ==

To start the pipeline on the board (PuTTY/SSH):
    cd ~/vision_software/visionsoc_main
    ./run_after_power_cycle.sh

Ctrl-C / SIGTERM stops visionsoc_main and the script restores GDM.
EOF
