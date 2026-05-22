#!/usr/bin/env bash
# gather_perf.sh -- run a perf test on the board, pull CSVs, render charts
# into a per-run timestamped directory.
#
# Usage:
#   ./gather_perf.sh sobel    [N_ITERS]   [--t1-hz HZ]
#   ./gather_perf.sh optical_flow [N_ITERS] [--t1-hz HZ]
#   ./gather_perf.sh matmul_8bitraw_short [N_ITERS] [--t1-hz HZ]
#   ./gather_perf.sh pipeline [N_FRAMES]  [--t1-hz HZ]
#   ./gather_perf.sh both     [N]         [--t1-hz HZ]
#
# Layout produced under perf/:
#   perf/
#     <ISO-UTC>-<mode>/
#       sobel_perf.csv            (if mode in {sobel, both})
#       optical_flow_perf.csv     (if mode=optical_flow)
#       matmul_8bitraw_short_perf.csv (if mode=matmul_8bitraw_short)
#       sobel_breakdown.per_instr.png
#       sobel_breakdown.grouped.png
#       pipeline_perf.csv         (if mode in {pipeline, both})
#       pipeline_breakdown.png
#       run.log                   (captured stdout/stderr of run_perf.sh)
#       meta.txt                  (mode, N, t1-hz, board hostname, bitstream)
#
# The board-side run_perf.sh is invoked via ssh; it puts the CSVs at
# /tmp/{sobel,pipeline}_perf.csv as a side effect. This script just orchestrates.

set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
PERF_DIR="$HERE/perf"
PLOTTER="$PERF_DIR/plot_perf.py"

usage() {
    cat >&2 <<EOF
usage: $0 {sobel|optical_flow|matmul_8bitraw_short|pipeline|both} [N] [--t1-hz HZ]
  N       iters (standalone kernels) / frames (pipeline)     default 100 / 30
  --t1-hz T1 clock in Hz for the libt1-overhead sliver       default 60e6
EOF
    exit 2
}

MODE=${1:-help}
case "$MODE" in
    sobel|optical_flow|matmul_8bitraw_short|pipeline|both) ;;
    *) usage ;;
esac
shift

N=""
T1_HZ="60e6"
while [ $# -gt 0 ]; do
    case "$1" in
        --t1-hz) T1_HZ=$2; shift 2;;
        --t1-hz=*) T1_HZ=${1#*=}; shift;;
        -h|--help) usage;;
        *) N=$1; shift;;
    esac
done

TS=$(date -u +%Y%m%dT%H%M%SZ)
OUT="$PERF_DIR/$TS-$MODE"
mkdir -p "$OUT"

# meta.txt -- everything someone re-opening the dir later needs to reproduce.
BOARD_HOST=$(ssh kv260 hostname 2>/dev/null || echo unknown)
BITSTREAM=$(ssh kv260 'sudo -n sha256sum /lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin 2>/dev/null | cut -d" " -f1' || echo unknown)
{
    echo "mode=$MODE"
    echo "n=${N:-<default>}"
    echo "t1_hz=$T1_HZ"
    echo "board_host=$BOARD_HOST"
    echo "bitstream_sha256=$BITSTREAM"
    echo "timestamp_utc=$TS"
    echo "host=$(hostname)"
    echo "git_head=$(git -C "$HERE" rev-parse --short HEAD 2>/dev/null || echo unknown)"
} > "$OUT/meta.txt"

echo "=== run_perf.sh $MODE $N (on kv260) -> $OUT/ ==="
# Forward stdout+stderr to both terminal and run.log; capture exit status.
set +e
if [ -n "$N" ]; then
    ssh kv260 "~/vision_software/visionsoc_main/run_perf.sh $MODE $N" 2>&1 | tee "$OUT/run.log"
    SSH_RC=${PIPESTATUS[0]}
else
    ssh kv260 "~/vision_software/visionsoc_main/run_perf.sh $MODE" 2>&1 | tee "$OUT/run.log"
    SSH_RC=${PIPESTATUS[0]}
fi
set -e
if [ "$SSH_RC" -ne 0 ]; then
    echo "WARNING: run_perf.sh exited $SSH_RC; see $OUT/run.log" >&2
    exit "$SSH_RC"
fi

echo "=== fetch CSVs ==="
case "$MODE" in
    sobel)        scp kv260:/tmp/sobel_perf.csv        "$OUT/" ;;
    optical_flow) scp kv260:/tmp/optical_flow_perf.csv "$OUT/" ;;
    matmul_8bitraw_short)
                  scp kv260:/tmp/matmul_8bitraw_short_perf.csv "$OUT/" ;;
    pipeline)     scp kv260:/tmp/pipeline_perf.csv     "$OUT/" ;;
    both)
        scp kv260:/tmp/sobel_perf.csv    "$OUT/"
        scp kv260:/tmp/pipeline_perf.csv "$OUT/"
        ;;
esac

echo "=== render charts ==="
if [ -f "$OUT/sobel_perf.csv" ]; then
    python3 "$PLOTTER" sobel "$OUT/sobel_perf.csv" \
        --out "$OUT/sobel_breakdown.png" --both
fi
if [ -f "$OUT/optical_flow_perf.csv" ]; then
    python3 "$PLOTTER" optical_flow "$OUT/optical_flow_perf.csv" \
        --out "$OUT/optical_flow_breakdown.png" --both
fi
if [ -f "$OUT/matmul_8bitraw_short_perf.csv" ]; then
    python3 "$PLOTTER" matmul_8bitraw_short "$OUT/matmul_8bitraw_short_perf.csv" \
        --out "$OUT/matmul_8bitraw_short_breakdown.png" --both
fi
if [ -f "$OUT/pipeline_perf.csv" ]; then
    python3 "$PLOTTER" pipeline "$OUT/pipeline_perf.csv" \
        --out "$OUT/pipeline_breakdown.png" --t1-hz "$T1_HZ"
fi

cat <<EOF

== done -- $OUT ==
$(ls -1 "$OUT")

EOF
