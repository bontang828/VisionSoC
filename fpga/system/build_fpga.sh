#!/usr/bin/env bash

#This is the FPGA Build script, taking the T1 AXI Lite wrapper and placing it into the Vivado Block design
#Launch Vivado to create hte block design + run synthesis.
# ./build_fpga.sh -c <config> [-r <rtl_dir>] [-s] [-b] [-a]
# -c <config> T1 config name e.g. mudkip2d64
# -r <rtl_dir> gives explicity rtl directory, else auto look for hte newest one
# -s synthesis only
# -b synthesis + implementation + bit stream generation
# -a full analysis: flatten_hierarchy none + hierarchical utilisation report + all messages

#Example
#./build_fpga.sh -c mudkip2d64 -s -a

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

show_usage() {
    echo "Usage: $0 -c <config> [-r <rtl_dir>] [-s] [-b] [-a]"
    exit 1
}

#Look for vivado on syspath
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

CONFIG=""
RTL_DIR=""
RUN_SYNTH=0
RUN_FULL=0
ANALYSIS_MODE=0

while [[ $# -gt 0 ]]; do
    case $1 in
        -c) CONFIG="$2";  shift 2 ;;
        -r) RTL_DIR="$2"; shift 2 ;;
        -s) RUN_SYNTH=1;  shift ;;
        -b) RUN_FULL=1;   shift ;;
        -a) ANALYSIS_MODE=1; shift ;;
        -h|--help) show_usage ;;
        *) echo "Unknown option: $1"; show_usage ;;
    esac
done

if [ -z "$CONFIG" ]; then
    echo "ERROR: -c <config> is required"
    show_usage
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VISIONSOC_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${VISIONSOC_DIR}/fpga/build"
TIMESTAMP=$(date +%Y%m%d-%H%M%S)
LOG_DIR="${BUILD_DIR}/${CONFIG}-${TIMESTAMP}"

mkdir -p "${LOG_DIR}"
BUILD_LOG="${LOG_DIR}/build.log"

#Find the RTL directory or use the specificed one
if [ -z "$RTL_DIR" ]; then
    #Find the latest rtl-* directory for this config
    TEST_OUTPUT="${VISIONSOC_DIR}/test_output/${CONFIG}"
    if [ ! -d "$TEST_OUTPUT" ]; then
        echo "ERROR: No test_output directory found for config '${CONFIG}'"
        echo "       Expected: ${TEST_OUTPUT}"
        echo "       Run build_rtl.sh -c ${CONFIG} first."
        exit 1
    fi

    LATEST_RTL=$(ls -dt "${TEST_OUTPUT}"/rtl-* 2>/dev/null | head -1)
    if [ -z "$LATEST_RTL" ]; then
        echo "ERROR: No rtl-* builds found in ${TEST_OUTPUT}"
        echo "       Run build_rtl.sh -c ${CONFIG} first."
        exit 1
    fi

    RTL_DIR="${LATEST_RTL}/result"
fi

if [ ! -f "${RTL_DIR}/T1.sv" ]; then
    echo "ERROR: T1.sv not found in ${RTL_DIR}"
    exit 1
fi

#Extract rowNumber from build log
BUILD_LOG_SRC="$(dirname "${RTL_DIR}")/build.log"
ROW_NUMBER=1

if [ -f "$BUILD_LOG_SRC" ]; then
    EXTRACTED_ROW=$(grep -oP '(?<=--rowNumber )\d+' "$BUILD_LOG_SRC" 2>/dev/null | head -1 || true)
    if [ -n "$EXTRACTED_ROW" ]; then
        ROW_NUMBER=$EXTRACTED_ROW
    fi
fi

#Log header
{
    echo "========================================"
    echo "T1 FPGA Build Log"
    echo "========================================"
    echo "Config:       ${CONFIG}"
    echo "Row number:   ${ROW_NUMBER}"
    echo "RTL dir:      ${RTL_DIR}"
    echo "Build dir:    ${LOG_DIR}"
    echo "Started:      $(date)"
    echo "========================================"
    echo ""
} > "$BUILD_LOG"

exec > >(tee -a "$BUILD_LOG") 2>&1

echo "Config:       ${CONFIG}"
echo "Row number:   ${ROW_NUMBER}"
echo "RTL dir:      ${RTL_DIR}"
echo "Build output: ${LOG_DIR}"
echo ""

#Generate Verilog wrapper
GEN_WRAPPER="${SCRIPT_DIR}/gen_wrapper.sh"
FPGA_TOP_V="${LOG_DIR}/t1_fpga_top.v"

if [ ! -f "$GEN_WRAPPER" ]; then
    echo "ERROR: gen_wrapper.sh not found at ${GEN_WRAPPER}"
    exit 1
fi

echo "Generating t1_fpga_top.v wrapper..."
bash "${GEN_WRAPPER}" "${RTL_DIR}" "${FPGA_TOP_V}"
echo ""

#Run Vivado
VIVADO_TCL="${SCRIPT_DIR}/system_top.tcl"

if [ ! -f "$VIVADO_TCL" ]; then
    echo "ERROR: system_top.tcl not found at ${VIVADO_TCL}"
    exit 1
fi

echo "Launching Vivado block design creation..."
echo ""

vivado -mode batch \
    -source "${VIVADO_TCL}" \
    -tclargs "${RTL_DIR}" "${CONFIG}" "${ROW_NUMBER}" "${LOG_DIR}" \
    -journal "${LOG_DIR}/vivado.jou" \
    -log "${LOG_DIR}/vivado.log"

echo ""
echo "Block design created successfully."

#run synthesis
if [ "$RUN_SYNTH" -eq 1 ] || [ "$RUN_FULL" -eq 1 ]; then
    PROJECT_DIR="${VISIONSOC_DIR}/fpga/build/t1_${CONFIG}_system"
    PROJECT_FILE="${PROJECT_DIR}/t1_${CONFIG}_system.xpr"

    if [ ! -f "$PROJECT_FILE" ]; then
        echo "ERROR: Vivado project not found at ${PROJECT_FILE}"
        exit 1
    fi

    echo ""
    echo "Running synthesis..."
    if [ "$ANALYSIS_MODE" -eq 1 ]; then
        echo "Analysis mode: flatten_hierarchy=none, hierarchical report, all messages enabled"
    fi
    vivado -mode batch -source /dev/stdin \
        -journal "${LOG_DIR}/vivado_synth.jou" \
        -log "${LOG_DIR}/vivado_synth.log" <<SYNTH_TCL
open_project ${PROJECT_FILE}
$(if [ "$ANALYSIS_MODE" -eq 1 ]; then echo "
# Analysis mode: disable all message limits and preserve hierarchy
set_msg_config -severity {WARNING} -limit 0
set_msg_config -severity {INFO} -limit 0
set_msg_config -severity {STATUS} -limit 0
set_property STEPS.SYNTH_DESIGN.ARGS.FLATTEN_HIERARCHY none [get_runs synth_1]
"; fi)
launch_runs synth_1 -jobs [exec nproc]
wait_on_run synth_1
if {[get_property STATUS [get_runs synth_1]] != "synth_design Complete!"} {
    puts "ERROR: Synthesis failed"
    exit 1
}
puts "Synthesis completed successfully."
open_run synth_1
$(if [ "$ANALYSIS_MODE" -eq 1 ]; then echo "
report_utilization -hierarchical -hierarchical_depth 20 -file ${LOG_DIR}/utilization_synth.rpt
"; else echo "
report_utilization -file ${LOG_DIR}/utilization_synth.rpt
"; fi)
report_timing_summary -file ${LOG_DIR}/timing_synth.rpt
SYNTH_TCL

    echo "Synthesis reports saved to ${LOG_DIR}/"
    echo "  - utilization_synth.rpt"
    echo "  - timing_synth.rpt"
fi

#Run implementation + bitstream generation
if [ "$RUN_FULL" -eq 1 ]; then
    echo ""
    echo "Running implementation and bitstream generation..."
    vivado -mode batch -source /dev/stdin \
        -journal "${LOG_DIR}/vivado_impl.jou" \
        -log "${LOG_DIR}/vivado_impl.log" <<IMPL_TCL
open_project ${PROJECT_FILE}
launch_runs impl_1 -to_step write_bitstream -jobs [exec nproc]
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

#Copy bitstream to build log directory
    BIT_FILE=$(find "${PROJECT_DIR}" -name "*.bit" -newer "${PROJECT_FILE}" | head -1)
    if [ -n "$BIT_FILE" ]; then
        cp "$BIT_FILE" "${LOG_DIR}/"
        echo ""
        echo "Bitstream copied to: ${LOG_DIR}/$(basename ${BIT_FILE})"
    fi

    echo "Implementation reports saved to ${LOG_DIR}/"
    echo "  - utilization_impl.rpt"
    echo "  - timing_impl.rpt"
fi

echo ""
echo "All logs saved to: ${LOG_DIR}/"
