#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${1:-19890}"
cmake -S "$ROOT" -B "$ROOT/build-rdma" -DCMAKE_BUILD_TYPE=Release
cmake --build "$ROOT/build-rdma" -j2
SERVER_LOG="/tmp/rrl-rdma-lock-server.$$"
"$ROOT/build-rdma/rrl_rdma_lock_server" "$PORT" >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!
trap 'kill "$SERVER_PID" 2>/dev/null || true; rm -f "$SERVER_LOG"' EXIT
sleep 1
RESULTS="$ROOT/experiments/results"
mkdir -p "$RESULTS"
HEADER="workload,index,cc,threads,transactions,committed,aborted,operations,reads,updates,inserts,deletes,range_reads,rows_found,rows_scanned,rows_rechecked,lock_acquires,lock_conflicts,throughput_committed_txn_s,throughput_attempt_txn_s,checksum"
printf '%s\n' "$HEADER" > "$RESULTS/rdma_v5_v6_smoke.csv"
"$ROOT/build-rdma/rrl_workload_runner" --index v2 --cc rdma-next-key --rdma-host 127.0.0.1 --rdma-port "$PORT" --workload ycsb-e --keys 1000 --txns 200 --ops 4 --range 16 --threads 4 --zipf 0.99 --format csv | tail -n 1 >> "$RESULTS/rdma_v5_v6_smoke.csv"
"$ROOT/build-rdma/rrl_workload_runner" --index v2 --cc rdma-hierarchical --rdma-host 127.0.0.1 --rdma-port "$PORT" --workload ycsb-e --keys 1000 --txns 200 --ops 4 --range 16 --bucket 64 --threads 4 --zipf 0.99 --format csv | tail -n 1 >> "$RESULTS/rdma_v5_v6_smoke.csv"
cat "$RESULTS/rdma_v5_v6_smoke.csv"
