#!/usr/bin/env bash
set -euo pipefail

# Run the reproducible performance matrix from the .66 control host.
# This script only starts experiments when explicitly invoked.
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${RRL_BUILD_DIR:-$ROOT/build-formal}"
REMOTE_HOST="${RRL_REMOTE_HOST:-dell@10.102.35.67}"
REMOTE_ROOT="${RRL_REMOTE_ROOT:-/home/dell/RdmaRangeLock}"
REMOTE_BUILD_DIR="${RRL_REMOTE_BUILD_DIR:-/tmp/rrl-formal-build}"
SSH_KEY="${RRL_SSH_KEY:-$HOME/.ssh/rdmarangelock_deploy}"
MODE="local"
PORT="${RRL_RDMA_PORT:-19930}"
GID_INDEX="${RRL_RDMA_GID_INDEX:-3}"
KEYS="${RRL_KEYS:-10000}"
TRANSACTIONS="${RRL_TRANSACTIONS:-5000}"
OPERATIONS="${RRL_OPERATIONS:-10}"
ZIPF="${RRL_ZIPF:-0.99}"
TPCC_ITEMS="${RRL_TPCC_ITEMS:-5}"
RUNS="${RRL_RUNS:-3}"
TIMEOUT_SECONDS="${RRL_TIMEOUT_SECONDS:-900}"
SYNC_REMOTE="${RRL_SYNC_REMOTE:-1}"
BUILD_JOBS="${RRL_BUILD_JOBS:-2}"

IFS=',' read -r -a THREADS <<< "${RRL_THREADS:-1,2,4,8,16,32}"
IFS=',' read -r -a WORKLOADS <<< "${RRL_WORKLOADS:-ycsb-a,ycsb-e,tpcc-payment,tpcc-new-order}"
IFS=',' read -r -a LOCAL_CCS <<< "${RRL_LOCAL_CCS:-none,record-2pl,gap-lock,next-key,hierarchical}"
IFS=',' read -r -a RDMA_CCS <<< "${RRL_RDMA_CCS:-rdma-next-key,rdma-hierarchical}"
IFS=',' read -r -a RANGE_LENGTHS <<< "${RRL_RANGE_LENGTHS:-8,32,128}"
IFS=',' read -r -a BUCKETS <<< "${RRL_BUCKETS:-16,64,256}"

usage() {
    cat <<EOF
Usage: $0 --mode local|cross-host

The matrix is intentionally not run by default. Select a mode explicitly.
All dimensions can be overridden with RRL_* environment variables; list values
are comma separated. Examples are documented in docs/notes/formal-performance-matrix.md.
EOF
}

while (($#)); do
    case "$1" in
        --mode) MODE="$2"; shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

case "$MODE" in
    local|cross-host) ;;
    *) echo "--mode must be local or cross-host" >&2; exit 2 ;;
esac

if [[ "$MODE" == "cross-host" && "$SYNC_REMOTE" == "1" ]]; then
    if [[ ! -f "$SSH_KEY" ]]; then
        echo "missing SSH key: $SSH_KEY" >&2
        exit 2
    fi
fi

cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -j"$BUILD_JOBS"

SSH_OPTS=(-i "$SSH_KEY" -o IdentitiesOnly=yes -o BatchMode=yes
          -o StrictHostKeyChecking=no -o UserKnownHostsFile=/tmp/rrl_formal_known_hosts)
remote_ssh() {
    ssh "${SSH_OPTS[@]}" "$REMOTE_HOST" "$1"
}

if [[ "$MODE" == "cross-host" ]]; then
    if [[ "$SYNC_REMOTE" == "1" ]]; then
        parent="$(dirname "$ROOT")"
        base="$(basename "$ROOT")"
        tar -C "$parent" -czf - \
            --exclude=.git --exclude=build --exclude=build-* \
            --exclude='experiments/results/*' --exclude='experiments/logs/*' \
            "$base" | remote_ssh "mkdir -p '$REMOTE_ROOT'; tar xzf - -C '$(dirname "$REMOTE_ROOT")'"
    fi
    remote_ssh "cmake -S '$REMOTE_ROOT' -B '$REMOTE_BUILD_DIR' -DCMAKE_BUILD_TYPE=Release && cmake --build '$REMOTE_BUILD_DIR' -j$BUILD_JOBS"
    RUNNER_REMOTE="$REMOTE_BUILD_DIR/rrl_workload_runner"
    RDMA_HOST="10.0.0.3"
    CLIENT_LABEL="10.0.0.4"
    TRANSPORT="rdma-cross-host"
else
    RUNNER_LOCAL="$BUILD_DIR/rrl_workload_runner"
    RDMA_HOST="127.0.0.1"
    CLIENT_LABEL="127.0.0.1"
    TRANSPORT="local"
fi

RESULT_DIR="${RRL_RESULT_DIR:-$ROOT/experiments/results}"
STAMP="$(date +%Y%m%d-%H%M%S)"
LOG_DIR="${RRL_LOG_DIR:-$ROOT/experiments/logs/formal-${MODE}-${STAMP}}"
mkdir -p "$RESULT_DIR" "$LOG_DIR"
RESULT_FILE="$RESULT_DIR/formal-${MODE}-${STAMP}.csv"
RUNNER_HEADER="workload,index,cc,threads,transactions,committed,aborted,operations,reads,updates,inserts,deletes,range_reads,rows_found,rows_scanned,rows_rechecked,lock_acquires,lock_conflicts,throughput_committed_txn_s,throughput_attempt_txn_s,checksum"
printf 'transport,server_host,client_host,run,seed,range,bucket,zipf,%s\n' "$RUNNER_HEADER" > "$RESULT_FILE"

SERVER_PID=""
SERVER_LOG="$LOG_DIR/lock-server.log"
cleanup() {
    if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

server_gid="$GID_INDEX"
if [[ "$MODE" == "cross-host" ]]; then
    RRL_RDMA_GID_INDEX="$server_gid" "$BUILD_DIR/rrl_rdma_lock_server" "$PORT" >"$SERVER_LOG" 2>&1 &
    SERVER_PID=$!
    sleep 1
    kill -0 "$SERVER_PID" 2>/dev/null
fi

run_remote() {
    local command="$1"
    remote_ssh "$command"
}

run_case() {
    local workload="$1" cc="$2" threads="$3" run_id="$4" range_length="$5" bucket="$6"
    local seed=$((7 + run_id))
    local case_id="${workload}-${cc}-t${threads}-r${run_id}-g${range_length}-b${bucket}"
    local case_log="$LOG_DIR/${case_id}.log"
    local args=(--index v2 --cc "$cc" --workload "$workload" --keys "$KEYS"
                --txns "$TRANSACTIONS" --ops "$OPERATIONS" --range "$range_length"
                --threads "$threads" --seed "$seed" --zipf "$ZIPF"
                --items "$TPCC_ITEMS" --format csv)
    local output line quoted command
    if [[ "$bucket" != "0" ]]; then args+=(--bucket "$bucket"); fi
    if [[ "$cc" == rdma-* ]]; then
        args+=(--rdma-host "$RDMA_HOST" --rdma-port "$PORT")
    fi

    if [[ "$MODE" == "cross-host" ]]; then
        command="timeout $(printf '%q' "$TIMEOUT_SECONDS") env RRL_RDMA_GID_INDEX=$(printf '%q' "$GID_INDEX") $(printf '%q' "$RUNNER_REMOTE")"
        for arg in "${args[@]}"; do
            printf -v quoted '%q' "$arg"
            command+=" $quoted"
        done
        if ! output="$(remote_ssh "$command" 2>"$case_log")"; then
            echo "failed: $case_id; see $case_log" >&2
            return 1
        fi
    else
        if ! output="$(timeout "$TIMEOUT_SECONDS" env RRL_RDMA_GID_INDEX="$server_gid" "$RUNNER_LOCAL" "${args[@]}" 2>"$case_log")"; then
            echo "failed: $case_id; see $case_log" >&2
            return 1
        fi
    fi
    line="$(printf '%s\n' "$output" | tail -n 1)"
    if [[ "$line" != workload,* ]]; then
        echo "invalid CSV output for $case_id: $line" >&2
        return 1
    fi
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$TRANSPORT" "$RDMA_HOST" "$CLIENT_LABEL" "$run_id" "$seed" \
        "$range_length" "$bucket" "$ZIPF" "$line" >> "$RESULT_FILE"
    echo "completed $case_id"
}

for workload in "${WORKLOADS[@]}"; do
    case "$workload" in
        ycsb-e) ranges=("${RANGE_LENGTHS[@]}") ;;
        *) ranges=(0) ;;
    esac
    for range_length in "${ranges[@]}"; do
        if [[ "$MODE" == "cross-host" ]]; then
            ccs=("${RDMA_CCS[@]}")
        else
            ccs=("${LOCAL_CCS[@]}")
        fi
        for cc in "${ccs[@]}"; do
            if [[ "$cc" == hierarchical || "$cc" == rdma-hierarchical ]]; then
                buckets=("${BUCKETS[@]}")
            else
                buckets=(0)
            fi
            for bucket in "${buckets[@]}"; do
                for threads in "${THREADS[@]}"; do
                    for ((run_id = 1; run_id <= RUNS; ++run_id)); do
                        run_case "$workload" "$cc" "$threads" "$run_id" "$range_length" "$bucket"
                    done
                done
            done
        done
    done
done

echo "matrix complete: $RESULT_FILE"
