#!/usr/bin/env bash

# Camera-only test bitstream build (no T1, no DMA, no BRAM).
# Drives system_top_camtest4.tcl. Tests the AP1302 reset-via-EMIO-GPIO
# hypothesis from 2026-05-12 in isolation. Build dir is timestamped
# under fpga/build/camtest-<timestamp>/ so it does NOT collide with
# a production T1 build that may be running in parallel.
#
# Usage:
#   ./build_camtest.sh         # BD only (validate the tcl)
#   ./build_camtest.sh -b      # BD + synth + impl + bitstream

set -e

START_TIME=$(date +%s)

finish() {
    local exit_code=$?
    local end_time
    end_time=$(date +%s)
    local elapsed=$(( end_time - START_TIME ))
    local mins=$(( elapsed / 60 ))
    local secs=$(( elapsed % 60 ))
    echo ""
    echo "----------------------------------------"
    printf "Time elapsed: %dm %ds" "$mins" "$secs"
    if [ "$exit_code" -ne 0 ]; then
        printf "  (exit code: %d)\n" "$exit_code"
    else
        printf "  (success)\n"
    fi
    echo "----------------------------------------"
}
trap finish EXIT

# Find Vivado
if ! command -v vivado &>/dev/null; then
    VIVADO_SEARCH=(
        "$HOME/Xilinx"/*/Vivado/bin
        /opt/Xilinx/Vivado/*/bin
        /tools/Xilinx/Vivado/*/bin
    )
    for d in "${VIVADO_SEARCH[@]}"; do
        if [ -x "$d/vivado" ]; then
            export PATH="$d:$PATH"
            echo "Found Vivado at: $d/vivado"
            break
        fi
    done
    if ! command -v vivado &>/dev/null; then
        echo "ERROR: Vivado not found. Set PATH or install Vivado."
        exit 1
    fi
fi

RUN_FULL=0
while [[ $# -gt 0 ]]; do
    case $1 in
        -b) RUN_FULL=1; shift ;;
        -h|--help) echo "Usage: $0 [-b]"; exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VISIONSOC_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${VISIONSOC_DIR}/fpga/build"
TIMESTAMP=$(date +%Y%m%d-%H%M%S)
LOG_DIR="${BUILD_DIR}/camtest4-${TIMESTAMP}"

mkdir -p "${LOG_DIR}"
BUILD_LOG="${LOG_DIR}/build.log"

{
    echo "========================================"
    echo "Camera-only test bitstream build"
    echo "========================================"
    echo "Build dir: ${LOG_DIR}"
    echo "Started:   $(date)"
    echo "Mode:      $([ "$RUN_FULL" -eq 1 ] && echo 'BD + synth + impl + bitstream' || echo 'BD only')"
    echo "========================================"
    echo ""
} > "$BUILD_LOG"

exec > >(tee -a "$BUILD_LOG") 2>&1

VIVADO_TCL="${SCRIPT_DIR}/system_top_camtest4.tcl"
if [ ! -f "$VIVADO_TCL" ]; then
    echo "ERROR: system_top_camtest4.tcl not found at ${VIVADO_TCL}"
    exit 1
fi

# Step 1: create BD project
echo "Launching Vivado block-design creation..."
vivado -mode batch \
    -source "${VIVADO_TCL}" \
    -tclargs "${LOG_DIR}" \
    -journal "${LOG_DIR}/vivado.jou" \
    -log "${LOG_DIR}/vivado.log"
echo ""
echo "Block design created successfully."

# Step 2 (optional): synth + impl + write_bitstream
if [ "$RUN_FULL" -eq 1 ]; then
    PROJECT_DIR="${VISIONSOC_DIR}/fpga/build/camtest4_system"
    PROJECT_FILE="${PROJECT_DIR}/camtest4_system.xpr"

    if [ ! -f "$PROJECT_FILE" ]; then
        echo "ERROR: Vivado project not found at ${PROJECT_FILE}"
        exit 1
    fi

    # Cap parallelism to half the cores so we play nice with a
    # potentially-running 5p production build on the same host.
    NCORES=$(nproc)
    JOBS=$(( NCORES / 2 ))
    if [ "$JOBS" -lt 1 ]; then JOBS=1; fi
    echo ""
    echo "Running synthesis... (using $JOBS jobs / nproc=$NCORES - half-cores to leave headroom for parallel builds)"

    vivado -mode batch -source /dev/stdin \
        -journal "${LOG_DIR}/vivado_synth.jou" \
        -log "${LOG_DIR}/vivado_synth.log" <<SYNTH_TCL
open_project ${PROJECT_FILE}
launch_runs synth_1 -jobs ${JOBS}
wait_on_run synth_1
if {[get_property STATUS [get_runs synth_1]] != "synth_design Complete!"} {
    puts "ERROR: Synthesis failed"
    exit 1
}
puts "Synthesis completed successfully."
open_run synth_1
report_utilization -file ${LOG_DIR}/utilization_synth.rpt
report_timing_summary -file ${LOG_DIR}/timing_synth.rpt
SYNTH_TCL
    echo "Synthesis reports saved to ${LOG_DIR}/"

    echo ""
    echo "Running implementation and bitstream generation..."
    vivado -mode batch -source /dev/stdin \
        -journal "${LOG_DIR}/vivado_impl.jou" \
        -log "${LOG_DIR}/vivado_impl.log" <<IMPL_TCL
open_project ${PROJECT_FILE}
launch_runs impl_1 -to_step write_bitstream -jobs ${JOBS}
wait_on_run impl_1
if {[get_property STATUS [get_runs impl_1]] != "write_bitstream Complete!"} {
    puts "ERROR: Implementation/bitstream failed"
    exit 1
}
puts "Bitstream generated successfully."
open_run impl_1
report_utilization -file ${LOG_DIR}/utilization_impl.rpt
report_timing_summary -file ${LOG_DIR}/timing_impl.rpt
IMPL_TCL

    # Copy bitstream to log dir
    BIT_FILE=$(find "${PROJECT_DIR}" -name "*.bit" -newer "${PROJECT_FILE}" | head -1)
    if [ -n "$BIT_FILE" ]; then
        cp "$BIT_FILE" "${LOG_DIR}/"
        echo ""
        echo "Bitstream copied to: ${LOG_DIR}/$(basename ${BIT_FILE})"
    fi
fi

echo ""
echo "All logs saved to: ${LOG_DIR}/"
