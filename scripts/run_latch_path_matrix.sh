#!/usr/bin/env bash
set -euo pipefail

# Runs the BTreeLatch update/insert path matrix against one prebuilt binary.
#
#   scripts/run_latch_path_matrix.sh --binary build-bench/rrl_latch_path_bench --label 03eb28e
#
# Every case is a fresh process with a fresh BTreeLatch; prefill happens inside the
# process before the timed region, so initialisation never lands in the throughput.

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BENCH=""
LABEL="${RRL_LABEL:-local}"
while (($#)); do
    case "$1" in
        --binary) BENCH="$2"; shift 2 ;;
        --label) LABEL="$2"; shift 2 ;;
        -h|--help) echo "usage: $0 --binary PATH --label NAME"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
if [[ -z "$BENCH" ]]; then
    echo "missing --binary" >&2
    exit 2
fi
if [[ ! -x "$BENCH" ]]; then
    echo "not executable: $BENCH" >&2
    exit 2
fi

THREADS="${RRL_THREADS:-1,2,4,8,16,32}"
RUNS="${RRL_RUNS:-3}"
ORDER="${RRL_ORDER:-128}"
UPDATE_KEYS="${RRL_UPDATE_KEYS:-65536}"
UPDATE_OPS="${RRL_UPDATE_OPS:-2000000}"
INSERT_KEYS="${RRL_INSERT_KEYS:-4096}"
INSERT_OPS="${RRL_INSERT_OPS:-500000}"
TIMEOUT_SECONDS="${RRL_TIMEOUT_SECONDS:-1800}"

IFS=',' read -r -a THREAD_LIST <<< "$THREADS"
STAMP="$(date +%Y%m%d-%H%M%S)"
RESULT_DIR="$ROOT/experiments/results"
LOG_DIR="$ROOT/experiments/logs/latch-path-${LABEL}-${STAMP}"
RESULT_FILE="$RESULT_DIR/latch-path-${LABEL}-${STAMP}.csv"
mkdir -p "$RESULT_DIR" "$LOG_DIR"

HEADER="build,mode,order,threads,run,seed,prefill_keys,ops_per_thread,ops_total,committed,failed,elapsed_ns,ops_per_sec,size_before,height_before,size_after,height_after,validate_ok"
printf '%s\n' "$HEADER" > "$RESULT_FILE"

run_case() {
    local mode="$1" threads="$2" run_id="$3" keys="$4" ops="$5"
    local seed=$((7 + run_id))
    local case_id="${mode}-t${threads}-r${run_id}-s${seed}"
    local case_log="$LOG_DIR/${case_id}.log"
    local output line
    if ! output="$(timeout "$TIMEOUT_SECONDS" "$BENCH" \
            --mode "$mode" --order "$ORDER" --threads "$threads" \
            --keys "$keys" --ops "$ops" --seed "$seed" --run "$run_id" \
            --build "$LABEL" --format csv 2>"$case_log")"; then
        echo "failed: $case_id (see $case_log)" >&2
        return 1
    fi
    line="$(printf '%s\n' "$output" | tail -n 1)"
    case "$line" in
        "$LABEL",*) ;;
        *) echo "unexpected output for $case_id: $line" >&2; return 1 ;;
    esac
    printf '%s\n' "$line" >> "$RESULT_FILE"
    echo "completed $case_id  $(printf '%s' "$line" | cut -d, -f13)"
}

for mode in update insert; do
    if [[ "$mode" == "update" ]]; then keys="$UPDATE_KEYS"; ops="$UPDATE_OPS"
    else keys="$INSERT_KEYS"; ops="$INSERT_OPS"; fi
    for threads in "${THREAD_LIST[@]}"; do
        for ((run_id = 1; run_id <= RUNS; ++run_id)); do
            run_case "$mode" "$threads" "$run_id" "$keys" "$ops"
        done
    done
done

echo "matrix complete: $RESULT_FILE"
