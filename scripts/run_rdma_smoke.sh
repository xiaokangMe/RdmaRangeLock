#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${1:-19890}"
SERVER_LOG="/tmp/rrl-rdma-lock-server.$$"
"$ROOT/build-rdma/rrl_rdma_lock_server" "$PORT" >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!
trap 'kill "$SERVER_PID" 2>/dev/null || true; rm -f "$SERVER_LOG"' EXIT
sleep 1
"$ROOT/build-rdma/rrl_workload_runner" --index v2 --cc rdma-next-key --rdma-host 127.0.0.1 --rdma-port "$PORT" --workload ycsb-e --keys 1000 --txns 200 --ops 4 --range 16 --threads 4 --zipf 0.99
"$ROOT/build-rdma/rrl_workload_runner" --index v2 --cc rdma-hierarchical --rdma-host 127.0.0.1 --rdma-port "$PORT" --workload ycsb-e --keys 1000 --txns 200 --ops 4 --range 16 --bucket 64 --threads 4 --zipf 0.99