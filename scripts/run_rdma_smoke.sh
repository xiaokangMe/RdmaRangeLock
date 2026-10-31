#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${1:-19890}"
GID_INDEX="${RRL_RDMA_GID_INDEX:-0}"
LOOPBACK_HOST="${RRL_RDMA_LOOPBACK_HOST:-127.0.0.1}"
cmake -S "$ROOT" -B "$ROOT/build-rdma" -DCMAKE_BUILD_TYPE=Release
cmake --build "$ROOT/build-rdma" -j2
SERVER_LOG="/tmp/rrl-rdma-lock-server.$$"
env RRL_RDMA_GID_INDEX="$GID_INDEX" "$ROOT/build-rdma/rrl_rdma_lock_server" "$PORT" >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!
trap 'kill "$SERVER_PID" 2>/dev/null || true; rm -f "$SERVER_LOG"' EXIT
sleep 1
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
  cat "$SERVER_LOG" >&2
  exit 2
fi
# Exercise one complete local RC-QP handshake before the workload runner.
env RRL_RDMA_GID_INDEX="$GID_INDEX" "$ROOT/build-rdma/rrl_rdma_lock_client" "$LOOPBACK_HOST" "$PORT"
sleep 0.2
RESULTS="$ROOT/experiments/results"
mkdir -p "$RESULTS"
HEADER="workload,index,cc,threads,transactions,committed,aborted,operations,reads,updates,inserts,deletes,range_reads,rows_found,rows_scanned,rows_rechecked,lock_acquires,lock_conflicts,lock_capacity_aborts,lock_gate_aborts,throughput_committed_txn_s,throughput_attempt_txn_s,checksum"
printf '%s\n' "$HEADER" > "$RESULTS/rdma_v5_v6_smoke.csv"
env RRL_RDMA_GID_INDEX="$GID_INDEX" "$ROOT/build-rdma/rrl_workload_runner" --index v2 --cc rdma-next-key --rdma-host "$LOOPBACK_HOST" --rdma-port "$PORT" --workload ycsb-e --keys 1000 --txns 200 --ops 4 --range 16 --threads 4 --zipf 0.99 --format csv | tail -n 1 >> "$RESULTS/rdma_v5_v6_smoke.csv"
env RRL_RDMA_GID_INDEX="$GID_INDEX" "$ROOT/build-rdma/rrl_workload_runner" --index v2 --cc rdma-hierarchical --rdma-host "$LOOPBACK_HOST" --rdma-port "$PORT" --workload ycsb-e --keys 1000 --txns 200 --ops 4 --range 16 --bucket 64 --threads 4 --zipf 0.99 --format csv | tail -n 1 >> "$RESULTS/rdma_v5_v6_smoke.csv"
cat "$RESULTS/rdma_v5_v6_smoke.csv"
