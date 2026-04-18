#!/usr/bin/env bash
set -e

#RTL build scrip, build the rtl for fpga synthesis flow
#Flags: ./build_rtl.sh [-c <config>]
# -c <config> system high level parameters: e.g. rookidee, blastoise, mudkip, mudkip2d, mudkip2d256small

#Example:
# ./build_rtl.sh -c mudkip


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
    echo "Usage: $0 [-c <config>]"
    exit 1
}

CONFIG="blastoise"

while [[ $# -gt 0 ]]; do
    case $1 in
        -c)
            CONFIG="$2"
            shift 2
            ;;
        -h|--help)
            show_usage
            ;;
        *)
            echo "Unknown option: $1"
            show_usage
            ;;
    esac
done

VISIONSOC_DIR="$(cd "$(dirname "$0")" && pwd)"
TIMESTAMP=$(date +%Y%m%d-%H%M%S)
OUTPUT_DIR="$VISIONSOC_DIR/test_output/$CONFIG/rtl-${TIMESTAMP}"
mkdir -p "$OUTPUT_DIR"
BUILD_LOG="$OUTPUT_DIR/build.log"

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
    echo "T1 RTL Build Log"
    echo "========================================"
    echo "Config:      $CONFIG"
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
} > "$BUILD_LOG"

exec > >(tee -a "$BUILD_LOG") 2>&1

echo "Configuration: $CONFIG"
echo "Output directory: $OUTPUT_DIR"
echo "Build log: $BUILD_LOG"
echo ""
echo "Building T1 RTL..."

if nix build ".#t1.$CONFIG.t1.rtl" --out-link "$OUTPUT_DIR/result" -L; then
    echo ""
    echo "\e[32m[== SUCCESS ==]\e[0m RTL build completed"
    exit 0
else
    echo ""
    echo "\e[31m[== FAILURE ==]\e[0m RTL build failed"
    echo "Check build.log for details: $BUILD_LOG"
    exit 1
fi
