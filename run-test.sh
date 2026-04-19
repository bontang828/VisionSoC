#!/usr/bin/env bash
set -e

START_TIME=$(date +%s)

#Print elapsed time on exit
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

#Run script adapted from t1 public repo as nix doesnt work on my copmuter
#Flags: ./run-test.sh <test-case-name> [-c <config>] [-i <top>] [-e <emu-type>] [--check] [--max-cycles <n>]
# -c <config> system high level parameters: e.g. blastoise, machamp, mudkip
# -i <top> top module:                      e.g. t1rocketemu, t1emu
# -e <emu-type> emulator type:              e.g. verilator-emu, verilator-emu-trace
# --check run offline checker after simulation
# --max-cycles <n> max cycles until termination without fatal for waves

#Example usage:
# ./run-test.sh intrinsic.linear_normalization -c machamp -i t1rocketemu
# ./run-test.sh intrinsic.linear_normalization -e verilator-emu-trace --check


if [ -z "$1" ]; then
    echo "Usage: $0 <test-case-name> [-c <config>] [-i <top>] [-e <emu-type>] [--check] [--max-cycles <n>]"
    exit 1
fi

TEST_CASE="$1"
shift

CONFIG="blastoise"
TOP="t1emu"
EMU_TYPE="verilator-emu"
RUN_CHECK=false
MAX_CYCLES=""

while [[ $# -gt 0 ]]; do
    case $1 in
        -c)
            CONFIG="$2"
            shift 2
            ;;
        -i)
            TOP="$2"
            shift 2
            ;;
        -e)
            EMU_TYPE="$2"
            shift 2
            ;;
        --check)
            RUN_CHECK=true
            shift
            ;;
        --max-cycles)
            MAX_CYCLES="$2"
            shift 2
            ;;
        *)
            echo "Unknown option: $1"
            show_usage
            ;;
    esac
done

if [ -n "$MAX_CYCLES" ] && ! [[ "$MAX_CYCLES" =~ ^[0-9]+$ ]]; then
    echo "Error: --max-cycles must be a non-negative integer"
    exit 1
fi

#Enable trace in simulator if trace emulator is requested
ENABLE_TRACE=false
if [[ "$EMU_TYPE" == *"trace"* ]]; then
    ENABLE_TRACE=true
fi

#Get root dir of the repo
VISIONSOC_DIR="$(cd "$(dirname "$0")" && pwd)"

#Create output directory to capture logs
TIMESTAMP=$(date +%Y%m%d-%H%M%S)
TEST_BASE_NAME=$(basename "$TEST_CASE")
OUTPUT_DIR="$VISIONSOC_DIR/test_output/$CONFIG/${TEST_BASE_NAME}-${TIMESTAMP}"
mkdir -p "$OUTPUT_DIR"

#Set up run log to captures everything printed to the terminal from this point on
RUN_LOG="$OUTPUT_DIR/run.log"

#Extract the cmdopt for this config from designs/*.toml
CONFIG_CMDOPT=$(awk -v cfg="[$CONFIG]" '
    $0 == cfg        { found=1; next }
    found && /^cmdopt/ {
        sub(/^cmdopt *= *"/, "")
        sub(/"[[:space:]]*$/, "")
        print
        exit
    }
    /^\[/ && found   { exit }
' "${VISIONSOC_DIR}/designs/"*.toml 2>/dev/null)

{
    echo "========================================"
    echo "T1 Test Run Log"
    echo "========================================"
    echo "Config:      $CONFIG"
    echo "Test:        $TEST_CASE"
    echo "Top module:  $TOP"
    echo "Emulator:    $EMU_TYPE"
    echo "Trace:       $ENABLE_TRACE"
    echo "Started:     $(date)"
    echo ""
    echo "Config parameters:"
    if [ -n "$CONFIG_CMDOPT" ]; then
        echo "$CONFIG_CMDOPT" | sed 's/ --/\n  --/g' | sed 's/^/  /'
    else
        echo "  [not found in designs/*.toml]"
    fi
    echo "========================================"
    echo ""
} > "$RUN_LOG"

# Tee all stdout and stderr to run.log
exec > >(tee -a "$RUN_LOG") 2>&1



echo "Configuration: $CONFIG"
echo "Top module: $TOP"
echo "Emulator type: $EMU_TYPE"
echo "Trace enabled: $ENABLE_TRACE"
echo "Max cycles: ${MAX_CYCLES:-none}"
echo "Output directory: $OUTPUT_DIR"
echo "Build logs: $OUTPUT_DIR/build.log"
echo "Run log:    $RUN_LOG"
echo ""

echo "Resolving simulator path..."
SIMULATOR_PATH=$(nix build ".#t1.$CONFIG.$TOP.$EMU_TYPE" --no-link --print-out-paths -L 2>> "$OUTPUT_DIR/build.log")
if [ -z "$SIMULATOR_PATH" ]; then
    echo "Error: Failed to resolve simulator path for .#t1.$CONFIG.$TOP.$EMU_TYPE"
    echo "Check build.log for details: $OUTPUT_DIR/build.log"
    exit 1
fi

echo "Resolving DPI library path..."
DPI_LIB_PATH=$(nix build ".#t1.$CONFIG.$TOP.verilator-dpi-lib" --no-link --print-out-paths -L 2>> "$OUTPUT_DIR/build.log")
if [ -z "$DPI_LIB_PATH" ]; then
    echo "Error: Failed to resolve DPI library path for .#t1.$CONFIG.$TOP.verilator-dpi-lib"
    echo "Check build.log for details: $OUTPUT_DIR/build.log"
    exit 1
fi
DPI_LIB_PATH="$DPI_LIB_PATH/lib"

echo "Simulator path: $SIMULATOR_PATH"
echo "DPI library path: $DPI_LIB_PATH"
echo ""

echo "Building test case: $TEST_CASE"
nix build ".#t1.$CONFIG.$TOP.cases.$TEST_CASE" -o "$OUTPUT_DIR/test-result" -L 2>> "$OUTPUT_DIR/build.log"

if [ ! -L "$OUTPUT_DIR/test-result" ]; then
    echo "Error: Test case build failed"
    exit 1
fi

TEST_ELF=$(find "$OUTPUT_DIR/test-result/bin" -name "*.elf" | head -1)
if [ -z "$TEST_ELF" ]; then
    echo "Error: Could not find test ELF file"
    exit 1
fi

echo "Running test: $TEST_CASE"
echo "Test ELF: $TEST_ELF"
#Determine simulator binary name based on top module and emulator type
if [ "$ENABLE_TRACE" = true ]; then
    SIMULATOR_BIN="$TOP-verilated-trace-simulator"
else
    SIMULATOR_BIN="$TOP-verilated-simulator"
fi
echo "Simulator: $SIMULATOR_PATH/bin/$SIMULATOR_BIN"
echo ""


#SIM_ARGS is an array of arguments to pass to the simulator
SIM_ARGS=(
    "+t1_elf_file=$(readlink -f $TEST_ELF)"
    "+t1_dev_rtl_event_path=$OUTPUT_DIR/rtl_event.jsonl"
)
if [ -n "$MAX_CYCLES" ]; then
    SIM_ARGS+=("+t1_debug_global_timeout=$MAX_CYCLES")
fi

#Add DRAMsim3 arguments if using t1rocketemu
if [ "$TOP" = "t1rocketemu" ]; then
    DRAMSIM3_CONFIG="$VISIONSOC_DIR/difftest/config/dramsim3-config.ini"
    if [ ! -f "$DRAMSIM3_CONFIG" ]; then
        echo "Error: DRAMsim3 config not found at $DRAMSIM3_CONFIG"
        exit 1
    fi
    DRAMSIM3_OUT="$OUTPUT_DIR/dramsim3_out"
    mkdir -p "$DRAMSIM3_OUT"
    SIM_ARGS+=(
        "+t1_dramsim3_cfg=$DRAMSIM3_CONFIG"
        "+t1_dramsim3_path=$DRAMSIM3_OUT"
    )
    echo "DRAMsim3 config: $DRAMSIM3_CONFIG"
    echo "DRAMsim3 output: $DRAMSIM3_OUT"
fi

#Add trace arguments if trace is enabled
if [ "$ENABLE_TRACE" = true ]; then
    SIM_ARGS+=("+trace" "+t1_wave_path=$OUTPUT_DIR/wave.fst")
    echo "Trace output will be saved to: $OUTPUT_DIR/wave.fst"
fi

export LD_LIBRARY_PATH="$DPI_LIB_PATH:$LD_LIBRARY_PATH"
if [ -n "$MAX_CYCLES" ]; then
    #To handle early termination causing rust panic
    T1_SUPPRESS_PANIC_IN_FINAL=1 "$SIMULATOR_PATH/bin/$SIMULATOR_BIN" "${SIM_ARGS[@]}"
else
    "$SIMULATOR_PATH/bin/$SIMULATOR_BIN" "${SIM_ARGS[@]}"
fi

if [ -f "sim_result.json" ]; then
    mv sim_result.json "$OUTPUT_DIR/"
fi

echo ""
echo "Test completed! Output directory: $OUTPUT_DIR"
echo ""

#Show results
if [ -f "$OUTPUT_DIR/sim_result.json" ]; then
    echo "=== Simulation Results ==="
    cat "$OUTPUT_DIR/sim_result.json"
    echo ""

    #Check test passed
    if grep -q '"success": true' "$OUTPUT_DIR/sim_result.json"; then
        printf '\033[32m[== SUCCESS ==]\033[0m SIMULATION PASSED\n'

        #Run offline checker if --check flag is provided
        if [ "$RUN_CHECK" = true ]; then
            echo ""
            echo "=== Running Offline Checker ==="

            #Build sim-checker if not already built
            if [ ! -e "$VISIONSOC_DIR/sim-checker-result" ]; then
                echo "Building sim-checker..."
                nix build ".#t1.sim-checker" -o "$VISIONSOC_DIR/sim-checker-result"
            fi

            SIM_CHECKER="$VISIONSOC_DIR/sim-checker-result/bin/t1-sim-checker"

            #Run the checker
            if $SIM_CHECKER \
                --sim-result "$OUTPUT_DIR/sim_result.json" \
                --rtl-event-file "$OUTPUT_DIR/rtl_event.jsonl" \
                --log-level info > "$OUTPUT_DIR/checker.log" 2>&1; then

                printf '\033[32m[== SUCCESS ==]\033[0m OFFLINE CHECK PASSED\n'
                echo "Check log: $OUTPUT_DIR/checker.log"
                exit 0
            else
                printf '\033[31m[== FAILURE ==]\033[0m OFFLINE CHECK FAILED\n'
                echo "Check log: $OUTPUT_DIR/checker.log"
                echo ""
                echo "=== Checker Log (last 20 lines) ==="
                tail -20 "$OUTPUT_DIR/checker.log"
                exit 1
            fi
        else
            exit 0
        fi
    else
        printf '\033[31m[== FAILURE ==]\033[0m SIMULATION FAILED\n'
        exit 1
    fi
else
    printf '\033[31m[== ERROR ==]\033[0m  No sim_result.json found\n'
    exit 1
fi
