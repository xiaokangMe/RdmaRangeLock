#!/usr/bin/env bash
# Cross-host capacity preflight, run on .67 against the lock server on .66.
#
# Before starting the full cross-host matrix, confirm three things at the worst
# operating point (longest range, highest thread count):
#   1. slot capacity is not exhausted  -> lock_capacity_aborts == 0
#   2. gate contention is not the abort source -> lock_gate_aborts == 0
#   3. how many locks a transaction really holds -> lock_acquires / transactions
set -euo pipefail

RUNNER=/tmp/rrl-formal-build/rrl_workload_runner
HOST=10.0.0.3
PORT="${PORT:-20020}"
OUT="${OUT:-/tmp/capacity-preflight.tsv}"
DURATION="${DURATION:-10}"

: > "$OUT"
run() {
    local cc="$1" range="$2" threads="$3" bucket="$4" seed="$5"
    local extra=()
    if [[ "$bucket" != "0" ]]; then extra=(--bucket "$bucket"); fi
    local row
    row=$(RRL_RDMA_GID_INDEX=3 timeout 180 "$RUNNER" \
        --index v2 --cc "$cc" --workload ycsb-e --keys 10000 --txns 5000 \
        --ops 10 --range "$range" --threads "$threads" --seed "$seed" \
        --zipf 0.99 --duration "$DURATION" "${extra[@]}" \
        --rdma-host "$HOST" --rdma-port "$PORT" --format csv 2>/dev/null | tail -1)
    echo -e "$cc\t$range\t$threads\t$bucket\t$seed\t$row" >> "$OUT"
    echo "done $cc range=$range t=$threads b=$bucket seed=$seed"
}

# next-key: the lock-count heavy protocol, swept over the range lengths the
# matrix uses.
for range in 8 32 128; do
    for threads in 1 8 32; do
        run rdma-next-key "$range" "$threads" 0 8
    done
done

# hierarchical: 256-row buckets at the widest range, where a bucket lock spans
# the most rows and the least slots are needed.
for threads in 1 8 32; do
    run rdma-hierarchical 128 "$threads" 256 8
done

# Repeat the single worst point with two more seeds so it is not a one-off.
for seed in 9 10; do
    run rdma-next-key 128 32 0 "$seed"
    run rdma-hierarchical 128 32 256 "$seed"
done

wc -l < "$OUT"
