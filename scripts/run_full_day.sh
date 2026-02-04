#!/bin/bash
# run_full_day.sh - Run market maker simulation on all PCAP segments for a full trading day
# Aggregates results and produces a summary report

set -e

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"
DATA_DIR="$PROJECT_DIR/data"
SYMBOL_FILE="$DATA_DIR/symbol_nyse_parsed.csv"
MM_SIM="$BUILD_DIR/market_maker_sim"

# Default PCAP directory
PCAP_DIR="$DATA_DIR/uncompressed-ny4-xnyx-pillar-a-20230822"

# Parse command line arguments
FILTER_TICKER=""
EXTRA_ARGS=""
MIN_FILE_SIZE_MB=50  # Skip files smaller than this

while [[ $# -gt 0 ]]; do
    case $1 in
        -t|--ticker)
            FILTER_TICKER="$2"
            shift 2
            ;;
        -d|--data-dir)
            PCAP_DIR="$2"
            shift 2
            ;;
        --trading-hours-only)
            TRADING_HOURS_ONLY=1
            shift
            ;;
        --min-size)
            MIN_FILE_SIZE_MB="$2"
            shift 2
            ;;
        --include-small)
            MIN_FILE_SIZE_MB=0
            shift
            ;;
        *)
            EXTRA_ARGS="$EXTRA_ARGS $1"
            shift
            ;;
    esac
done

# Check if binary exists
if [[ ! -f "$MM_SIM" ]]; then
    echo "Error: market_maker_sim binary not found at $MM_SIM"
    echo "Please build it first: cmake --build build --target market_maker_sim"
    exit 1
fi

# Check if PCAP directory exists
if [[ ! -d "$PCAP_DIR" ]]; then
    echo "Error: PCAP directory not found at $PCAP_DIR"
    exit 1
fi

# Find all PCAP files and sort by timestamp
PCAP_FILES=$(find "$PCAP_DIR" -name "*.pcap" -type f | sort)
TOTAL_FILES=$(echo "$PCAP_FILES" | wc -l | tr -d ' ')

if [[ $TOTAL_FILES -eq 0 ]]; then
    echo "Error: No PCAP files found in $PCAP_DIR"
    exit 1
fi

echo "=============================================="
echo "NYSE XDP Market Maker - Full Day Simulation"
echo "=============================================="
echo "PCAP Directory: $PCAP_DIR"
echo "Total segments: $TOTAL_FILES"
echo "Symbol file: $SYMBOL_FILE"
if [[ -n "$FILTER_TICKER" ]]; then
    echo "Filter ticker: $FILTER_TICKER"
fi
echo "=============================================="
echo ""

# Initialize aggregation variables
TOTAL_BASELINE_PNL=0
TOTAL_TOXICITY_PNL=0
TOTAL_ADVERSE_PNL=0
TOTAL_BASELINE_FILLS=0
TOTAL_TOXICITY_FILLS=0
TOTAL_ADVERSE_FILLS=0
TOTAL_QUOTES_SUPPRESSED=0
TOTAL_EXECUTIONS=0
TOTAL_SYMBOLS_TRADED=0
TOTAL_SYMBOLS_INELIGIBLE=0
SEGMENTS_PROCESSED=0

# Create a temporary file for results
RESULTS_FILE=$(mktemp)
trap "rm -f $RESULTS_FILE" EXIT

# Process each PCAP file
for PCAP_FILE in $PCAP_FILES; do
    SEGMENTS_PROCESSED=$((SEGMENTS_PROCESSED + 1))
    SEGMENT_NAME=$(basename "$PCAP_FILE" .pcap)

    # Extract timestamp from filename for progress display
    TIMESTAMP=$(echo "$SEGMENT_NAME" | grep -oE 'T[0-9]{6}' | sed 's/T//')
    HOUR=${TIMESTAMP:0:2}
    MIN=${TIMESTAMP:2:2}

    # Skip non-trading hours if flag is set
    if [[ -n "$TRADING_HOURS_ONLY" ]]; then
        # NYSE regular hours: 9:30 AM - 4:00 PM ET (093000 - 160000)
        if [[ "$TIMESTAMP" < "093000" ]] || [[ "$TIMESTAMP" > "160000" ]]; then
            echo "[$SEGMENTS_PROCESSED/$TOTAL_FILES] Skipping $HOUR:$MIN (outside trading hours)"
            continue
        fi
    fi

    # Get file size for display and filtering
    FILE_SIZE=$(ls -lh "$PCAP_FILE" | awk '{print $5}')
    FILE_SIZE_BYTES=$(stat -f%z "$PCAP_FILE" 2>/dev/null || stat --printf="%s" "$PCAP_FILE" 2>/dev/null)
    FILE_SIZE_MB=$((FILE_SIZE_BYTES / 1048576))

    # Skip small files (likely incomplete/no executions)
    if [[ $FILE_SIZE_MB -lt $MIN_FILE_SIZE_MB ]]; then
        echo "[$SEGMENTS_PROCESSED/$TOTAL_FILES] Skipping $HOUR:$MIN ($FILE_SIZE, < ${MIN_FILE_SIZE_MB}MB)"
        continue
    fi

    echo "[$SEGMENTS_PROCESSED/$TOTAL_FILES] Processing $HOUR:$MIN ($FILE_SIZE)..."

    # Build command
    CMD="$MM_SIM $PCAP_FILE $SYMBOL_FILE"
    if [[ -n "$FILTER_TICKER" ]]; then
        CMD="$CMD -t $FILTER_TICKER"
    fi
    if [[ -n "$EXTRA_ARGS" ]]; then
        CMD="$CMD $EXTRA_ARGS"
    fi

    # Run simulation and capture output
    OUTPUT=$($CMD 2>&1) || true

    # Parse key metrics from output using more robust extraction
    # Format: "Baseline Total PnL: $-2732.17" or "Baseline Total PnL: $206.45"
    BASELINE_PNL=$(echo "$OUTPUT" | grep "Baseline Total PnL:" | sed 's/.*\$\([-0-9.]*\).*/\1/' || echo "0")
    TOXICITY_PNL=$(echo "$OUTPUT" | grep "Toxicity Total PnL:" | sed 's/.*\$\([-0-9.]*\).*/\1/' || echo "0")
    ADVERSE_PNL=$(echo "$OUTPUT" | grep "Total adverse selection penalty:" | sed 's/.*\$\([-0-9.]*\).*/\1/' || echo "0")
    BASELINE_FILLS=$(echo "$OUTPUT" | grep "^Baseline fills:" | sed 's/[^0-9]//g' || echo "0")
    TOXICITY_FILLS=$(echo "$OUTPUT" | grep "^Toxicity fills:" | sed 's/[^0-9]//g' || echo "0")
    QUOTES_SUPPRESSED=$(echo "$OUTPUT" | grep "Quotes suppressed" | sed 's/[^0-9]//g' || echo "0")
    EXECUTIONS=$(echo "$OUTPUT" | grep "Total executions processed:" | sed 's/[^0-9]//g' || echo "0")
    SYMBOLS_TRADED=$(echo "$OUTPUT" | grep "^Symbols traded:" | sed 's/[^0-9]//g' || echo "0")
    SYMBOLS_INELIGIBLE=$(echo "$OUTPUT" | grep "^Symbols ineligible:" | sed 's/[^0-9]//g' || echo "0")
    ADVERSE_FILLS=$(echo "$OUTPUT" | grep "Fills with adverse movement:" | sed 's/.*: \([0-9]*\) \/.*/\1/' || echo "0")

    # Handle empty values
    : ${BASELINE_PNL:=0}
    : ${TOXICITY_PNL:=0}
    : ${ADVERSE_PNL:=0}
    : ${BASELINE_FILLS:=0}
    : ${TOXICITY_FILLS:=0}
    : ${QUOTES_SUPPRESSED:=0}
    : ${EXECUTIONS:=0}
    : ${SYMBOLS_TRADED:=0}
    : ${SYMBOLS_INELIGIBLE:=0}
    : ${ADVERSE_FILLS:=0}

    # Aggregate totals using bc for floating point
    TOTAL_BASELINE_PNL=$(echo "$TOTAL_BASELINE_PNL + $BASELINE_PNL" | bc 2>/dev/null || echo "$TOTAL_BASELINE_PNL")
    TOTAL_TOXICITY_PNL=$(echo "$TOTAL_TOXICITY_PNL + $TOXICITY_PNL" | bc 2>/dev/null || echo "$TOTAL_TOXICITY_PNL")
    TOTAL_ADVERSE_PNL=$(echo "$TOTAL_ADVERSE_PNL + $ADVERSE_PNL" | bc 2>/dev/null || echo "$TOTAL_ADVERSE_PNL")
    TOTAL_BASELINE_FILLS=$((TOTAL_BASELINE_FILLS + BASELINE_FILLS))
    TOTAL_TOXICITY_FILLS=$((TOTAL_TOXICITY_FILLS + TOXICITY_FILLS))
    TOTAL_QUOTES_SUPPRESSED=$((TOTAL_QUOTES_SUPPRESSED + QUOTES_SUPPRESSED))
    TOTAL_EXECUTIONS=$((TOTAL_EXECUTIONS + EXECUTIONS))
    TOTAL_SYMBOLS_TRADED=$((TOTAL_SYMBOLS_TRADED + SYMBOLS_TRADED))
    TOTAL_SYMBOLS_INELIGIBLE=$((TOTAL_SYMBOLS_INELIGIBLE + SYMBOLS_INELIGIBLE))
    TOTAL_ADVERSE_FILLS=$((TOTAL_ADVERSE_FILLS + ADVERSE_FILLS))

    # Write segment result to file
    echo "$HOUR:$MIN,$BASELINE_PNL,$TOXICITY_PNL,$BASELINE_FILLS,$TOXICITY_FILLS" >> "$RESULTS_FILE"

    # Brief status
    echo "         Toxicity PnL: \$$TOXICITY_PNL | Fills: $TOXICITY_FILLS"
done

echo ""
echo "=============================================="
echo "        FULL DAY AGGREGATED RESULTS"
echo "=============================================="
echo ""
echo "--- PORTFOLIO TOTALS ---"
printf "Baseline Total PnL:  \$%'.2f\n" "$TOTAL_BASELINE_PNL"
printf "Toxicity Total PnL:  \$%'.2f\n" "$TOTAL_TOXICITY_PNL"

# Calculate improvement
IMPROVEMENT=$(echo "$TOTAL_TOXICITY_PNL - $TOTAL_BASELINE_PNL" | bc)
if [[ $(echo "$TOTAL_BASELINE_PNL != 0" | bc) -eq 1 ]]; then
    ABS_BASELINE=$(echo "if ($TOTAL_BASELINE_PNL < 0) -1 * $TOTAL_BASELINE_PNL else $TOTAL_BASELINE_PNL" | bc)
    IMPROVEMENT_PCT=$(echo "scale=2; ($IMPROVEMENT / $ABS_BASELINE) * 100" | bc)
else
    IMPROVEMENT_PCT="N/A"
fi
printf "PnL Improvement:     \$%'.2f (%s%%)\n" "$IMPROVEMENT" "$IMPROVEMENT_PCT"

echo ""
echo "--- ADVERSE SELECTION ---"
printf "Total Adverse Penalty: \$%'.2f\n" "$TOTAL_ADVERSE_PNL"
echo "Fills with adverse:    $TOTAL_ADVERSE_FILLS / $TOTAL_TOXICITY_FILLS"

echo ""
echo "--- EXECUTION STATS ---"
echo "Baseline fills:        $TOTAL_BASELINE_FILLS"
echo "Toxicity fills:        $TOTAL_TOXICITY_FILLS"
echo "Quotes suppressed:     $TOTAL_QUOTES_SUPPRESSED"
echo "Total executions:      $TOTAL_EXECUTIONS"

if [[ $TOTAL_BASELINE_FILLS -gt 0 ]]; then
    AVG_BASELINE=$(echo "scale=4; $TOTAL_BASELINE_PNL / $TOTAL_BASELINE_FILLS" | bc)
    printf "Avg PnL/fill (base):   \$%s\n" "$AVG_BASELINE"
fi

if [[ $TOTAL_TOXICITY_FILLS -gt 0 ]]; then
    AVG_TOXICITY=$(echo "scale=4; $TOTAL_TOXICITY_PNL / $TOTAL_TOXICITY_FILLS" | bc)
    printf "Avg PnL/fill (tox):    \$%s\n" "$AVG_TOXICITY"
fi

echo ""
echo "--- SYMBOL STATS ---"
echo "Symbols traded (cumulative): $TOTAL_SYMBOLS_TRADED"
echo "Symbols ineligible (cumul.): $TOTAL_SYMBOLS_INELIGIBLE"

echo ""
echo "=============================================="
echo "          SEGMENT-BY-SEGMENT RESULTS"
echo "=============================================="
echo "Time,Baseline,Toxicity,Base Fills,Tox Fills"
cat "$RESULTS_FILE"
echo ""
