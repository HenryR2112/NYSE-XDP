#!/bin/bash
# reproduce.sh - Full reproducible pipeline from simulation to analysis
#
# Runs the complete chain:
#   1. Build the simulator
#   2. Run full-day simulation with CSV output
#   3. Run statistical analysis (analyze_fills.py)
#   4. Run hypothesis tests (test_hypotheses.py)
#   5. Run parameter sensitivity (parameter_sensitivity.py)
#
# Usage:
#   ./scripts/reproduce.sh                    # Full pipeline
#   ./scripts/reproduce.sh --skip-sim         # Skip simulation (reuse existing output)
#   ./scripts/reproduce.sh --skip-sensitivity # Skip parameter sensitivity (slow)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"
DATA_DIR="$PROJECT_DIR/data/uncompressed-ny4-xnyx-pillar-a-20230822"
SYMBOL_FILE="$PROJECT_DIR/data/symbol_nyse_parsed.csv"
OUTPUT_DIR="$PROJECT_DIR/documentation/analysis_output"
RESULTS_FILE="$PROJECT_DIR/documentation/results.txt"

SKIP_SIM=false
SKIP_SENSITIVITY=false

for arg in "$@"; do
    case "$arg" in
        --skip-sim) SKIP_SIM=true ;;
        --skip-sensitivity) SKIP_SENSITIVITY=true ;;
        --help|-h)
            echo "Usage: $0 [--skip-sim] [--skip-sensitivity]"
            exit 0
            ;;
    esac
done

echo "========================================"
echo "NYSE-XDP Reproducible Pipeline"
echo "========================================"
echo "Project dir: $PROJECT_DIR"
echo "Data dir:    $DATA_DIR"
echo "Output dir:  $OUTPUT_DIR"
echo ""

# Step 1: Build
echo "[1/5] Building market_maker_sim..."
cd "$BUILD_DIR"
cmake --build . --target market_maker_sim 2>&1 | tail -3
echo ""

# Step 2: Run simulation
if [ "$SKIP_SIM" = false ]; then
    echo "[2/5] Running full-day simulation..."
    PCAP_FILES=$(ls "$DATA_DIR"/*.pcap 2>/dev/null)
    if [ -z "$PCAP_FILES" ]; then
        echo "ERROR: No PCAP files found in $DATA_DIR"
        exit 1
    fi
    NUM_FILES=$(echo "$PCAP_FILES" | wc -l | tr -d ' ')
    echo "  Processing $NUM_FILES PCAP files..."
    mkdir -p "$OUTPUT_DIR"

    "$BUILD_DIR/market_maker_sim" "$DATA_DIR"/*.pcap \
        -s "$SYMBOL_FILE" \
        --output-dir "$OUTPUT_DIR" \
        --online-learning \
        2>"$RESULTS_FILE.stderr" | tee "$RESULTS_FILE"

    echo "  Simulation complete. Output in $RESULTS_FILE"
else
    echo "[2/5] Skipping simulation (--skip-sim)"
fi
echo ""

# Step 3: Statistical analysis
echo "[3/5] Running statistical analysis..."
python3 "$SCRIPT_DIR/analyze_fills.py" --output-dir "$OUTPUT_DIR"
echo ""

# Step 4: Hypothesis tests
echo "[4/5] Running hypothesis tests..."
python3 "$SCRIPT_DIR/test_hypotheses.py" "$RESULTS_FILE"
echo ""

# Step 5: Parameter sensitivity
if [ "$SKIP_SENSITIVITY" = false ]; then
    echo "[5/5] Running parameter sensitivity grid..."
    echo "  (This runs multiple simulations and may take 30+ minutes)"
    python3 "$SCRIPT_DIR/parameter_sensitivity.py" \
        --sim-binary "$BUILD_DIR/market_maker_sim" \
        --data-dir "$DATA_DIR" \
        --symbol-file "$SYMBOL_FILE" \
        --output "$PROJECT_DIR/documentation/parameter_sensitivity.json" \
        --online-learning
else
    echo "[5/5] Skipping parameter sensitivity (--skip-sensitivity)"
fi
echo ""

echo "========================================"
echo "Pipeline complete."
echo ""
echo "Outputs:"
echo "  $RESULTS_FILE"
echo "  $OUTPUT_DIR/advanced_analysis.json"
echo "  $PROJECT_DIR/documentation/hypothesis_test_results.json"
if [ "$SKIP_SENSITIVITY" = false ]; then
    echo "  $PROJECT_DIR/documentation/parameter_sensitivity.json"
fi
echo "========================================"
