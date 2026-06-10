#!/usr/bin/env bash
# run_bench.sh -- one-shot build + run + plot for benchmark_instructions_perf.
#
#   ./run_bench.sh [N_ITERS] [--host kv260] [--local|--remote] [--no-plot]
#
# Auto-detects where it is:
#   * ON THE BOARD  (/dev/uio0 present): build (make) + free T1 + run + restore
#     visionsoc_main + plot locally.
#   * ON THE DEV HOST: sync sources to the Kria over ssh, build+run there in
#     --local mode, scp the CSV back, and plot here (dev host has matplotlib).
#
# So from your laptop/VM you can just:  ./run_bench.sh 100
set -u

ITERS=""
HOST=kv260
MODE=auto          # auto | local | remote
NO_PLOT=0
REMOTE_BENCH='vision_software/visionsoc_main/benchmark'   # relative to board $HOME

while [ $# -gt 0 ]; do
    case "$1" in
        --local)   MODE=local;  shift ;;
        --remote)  MODE=remote; shift ;;
        --no-plot) NO_PLOT=1;   shift ;;
        --host)    HOST="$2";   shift 2 ;;
        --help|-h)
            sed -n '2,15p' "$0"; exit 0 ;;
        [0-9]*)    ITERS="$1";  shift ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done
ITERS=${ITERS:-100}

HERE="$(cd "$(dirname "$0")" && pwd)"
TS=$(date +%Y%m%d-%H%M%S)
OUTDIR="$HERE/results/$TS"
mkdir -p "$OUTDIR"
LOG="$OUTDIR/bench.log"
CSV="$OUTDIR/bench.csv"
PNG="$OUTDIR/bench_cycles_fpga.png"

if [ "$MODE" = auto ]; then
    [ -e /dev/uio0 ] && MODE=local || MODE=remote
fi

plot_local() {  # $1 = csv
    local plot="$HERE/plot_bench_cycles_fpga.py"
    if [ "$NO_PLOT" -eq 1 ]; then return; fi
    if [ ! -f "$1" ]; then echo "  (no CSV to plot)"; return; fi
    if [ ! -f "$plot" ]; then echo "  (plot script missing: $plot)"; return; fi
    if ! python3 -c 'import matplotlib' >/dev/null 2>&1; then
        echo "  matplotlib not here; render elsewhere: python3 $plot $1"; return
    fi
    python3 "$plot" "$1" --out "$PNG" || echo "  (plot failed)"
}

# ====================================================================
# LOCAL: we are on the board.
# ====================================================================
if [ "$MODE" = local ]; then
    VSM_NAME=visionsoc_main
    VSM="$(cd "$HERE/.." && pwd)/$VSM_NAME"
    BIN="$HERE/benchmark_instructions_perf"
    run_root() { if [ "$(id -u)" -eq 0 ]; then "$@"; else sudo -n "$@"; fi; }
    WAS_RUNNING=0
    t1_uio_present() {
        for d in /sys/class/uio/uio*; do
            [ "$(cat "$d/name" 2>/dev/null)" = "t1" ] && return 0
        done
        return 1
    }
    require_t1_ready() {
        # The benchmark needs T1 ALREADY INITIALISED: the VisionSoC bitstream
        # loaded AND T1 brought up by a prior visionsoc_main run. libt1's t1_init
        # does NOT reset T1, so issuing to a bare / un-initialised T1 hangs the
        # AXI-Lite read and WEDGES the board (requires a power-cycle). This is the
        # same precondition run_perf.sh's sobel/matmul modes rely on. We
        # deliberately do NOT load the bitstream here -- use
        # run_after_power_cycle.sh, which loads it AND initialises T1 via
        # visionsoc_main.
        if t1_uio_present; then return 0; fi
        cat >&2 <<EOF
  ERROR: no 't1' UIO node -- the VisionSoC bitstream is not loaded / T1 is not
         initialised. Running the benchmark now would wedge the board. Bring it
         up first (this loads the bitstream AND initialises T1):

             ssh $HOST 'sudo ~/vision_software/visionsoc_main/run_after_power_cycle.sh &'

         wait for the camera to come up (visionsoc_main running), then re-run
         this script.
EOF
        return 1
    }
    stop_vsm() {
        PID=$(pgrep -x "$VSM_NAME" || true)
        [ -z "$PID" ] && return
        WAS_RUNNING=1
        echo "  stopping $VSM_NAME (PID $PID)"
        run_root kill -TERM "$PID" 2>/dev/null || true
        for _ in 1 2 3 4 5; do sleep 1; pgrep -x "$VSM_NAME" >/dev/null || return 0; done
        run_root kill -KILL "$PID" 2>/dev/null || true; sleep 1
    }
    start_vsm() {
        # Only restore VSM if it was running before (i.e. the bitstream was
        # already up with a live camera). If we had to load the bitstream fresh,
        # the camera media-ctl isn't set up, so leave VSM down -- the user can
        # run run_after_power_cycle.sh to bring the full camera+display back.
        [ "$WAS_RUNNING" -eq 1 ] || { echo "  ($VSM_NAME was not running; not restarting)"; return; }
        [ -x "$VSM" ] || { echo "  ($VSM_NAME not present)"; return; }
        echo "  relaunching $VSM_NAME"
        run_root "$VSM" /dev/video0 </dev/null >>/tmp/vsm.log 2>&1 &
        sleep 3
    }

    echo "=== [build] ==="
    # -B forces a full rebuild. The board clock lags the dev host, so scp'd
    # sources arrive with "future" mtimes and a plain `make` decides the stale
    # .o is up-to-date ("Clock skew detected") and skips recompiling -- which
    # silently runs OLD code. -B sidesteps that (same trick sync_perf.sh uses).
    # Also drop any stale objects first, belt-and-braces against the skew.
    rm -f "$HERE/benchmark_instructions_perf.o" "$BIN"
    make -B -C "$HERE" || { echo "ERROR: make failed" >&2; exit 1; }
    [ -x "$BIN" ] || { echo "ERROR: $BIN missing" >&2; exit 1; }

    # Deploy into the NOPASSWD-allowed dir. The board sudoers rule only covers
    # /home/ubuntu/vision_software/libt1/test/* (the same rule run_perf.sh uses
    # for sobel_perf etc.), so `sudo -n` on a binary in benchmark/ asks for a
    # password. Running it from libt1/test/ -- with the CSV in /tmp, exactly
    # like run_perf.sh -- stays inside the allowed rule.
    TEST_DIR="$(cd "$HERE/../../libt1" && pwd)/test"
    mkdir -p "$TEST_DIR"
    cp "$BIN" "$TEST_DIR/" || { echo "ERROR: deploy to $TEST_DIR failed" >&2; exit 1; }
    DEPLOYED="$TEST_DIR/benchmark_instructions_perf"
    TMP_CSV=/tmp/benchmark_instructions_perf.csv

    echo "=== [run] iters=$ITERS  (from $TEST_DIR) ==="
    require_t1_ready || exit 1
    stop_vsm
    run_root "$DEPLOYED" --iters "$ITERS" --out "$TMP_CSV" 2>&1 | tee "$LOG"
    RC=${PIPESTATUS[0]}
    start_vsm
    cp "$TMP_CSV" "$CSV" 2>/dev/null || true

    echo "=== [plot] ==="
    [ "$RC" -eq 0 ] && plot_local "$CSV" || echo "  (skipped, exit $RC)"

    # Machine-readable markers for the remote orchestrator + human paths.
    echo "RESULT_CSV=$CSV"
    echo "RESULT_LOG=$LOG"
    echo ""
    echo "=== done (exit $RC) ==="
    echo "  log: $LOG"; echo "  csv: $CSV"; [ -f "$PNG" ] && echo "  png: $PNG"
    exit "$RC"
fi

# ====================================================================
# REMOTE: we are on the dev host; drive the board over ssh.
# ====================================================================
echo "=== orchestrating $HOST (dev host -> board) ==="
echo "  [sync] sources -> $HOST:$REMOTE_BENCH"
ssh "$HOST" "mkdir -p $REMOTE_BENCH/kernels" || { echo "ssh $HOST failed" >&2; exit 1; }
scp -q "$HERE/benchmark_instructions_perf.c" "$HERE/Makefile" "$HERE/run_bench.sh" \
       "$HOST:$REMOTE_BENCH/" || { echo "scp sources failed" >&2; exit 1; }
scp -q "$HERE/kernels/bench_instructions.S" "$HOST:$REMOTE_BENCH/kernels/" \
    || { echo "scp kernel failed" >&2; exit 1; }

echo "  [board] build + run (iters=$ITERS)"
# Board builds the kernel header + binary (it has riscv64-linux-gnu-as + libt1),
# runs in --local --no-plot mode, writes its own results dir there.
ssh "$HOST" "bash $REMOTE_BENCH/run_bench.sh --local --no-plot $ITERS" 2>&1 | tee "$LOG"
RC=${PIPESTATUS[0]}

RCSV=$(grep -m1 '^RESULT_CSV=' "$LOG" | cut -d= -f2-)
if [ -z "$RCSV" ]; then
    echo "  WARNING: board did not report a CSV (build/run failed on the board?)" >&2
else
    echo "  [fetch] $HOST:$RCSV -> $CSV"
    scp -q "$HOST:$RCSV" "$CSV" || echo "  scp CSV back failed" >&2
fi

echo "=== [plot] (on dev host) ==="
plot_local "$CSV"

echo ""
echo "=== done (board exit $RC) ==="
echo "  log: $LOG"; [ -f "$CSV" ] && echo "  csv: $CSV"; [ -f "$PNG" ] && echo "  png: $PNG"
exit "$RC"
