#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${RRL_V2_BUILD_DIR:-/tmp/rrl-v2-build}"
THREADS="${1:-$(nproc)}"
DURATION_SECONDS="${2:-3600}"
KEYS_PER_THREAD="${3:-2048}"
JOBS="${RRL_BUILD_JOBS:-2}"

cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --target rrl_v2_btree_stress_test -j"$JOBS"
exec "$BUILD_DIR/rrl_v2_btree_stress_test" "$THREADS" "$DURATION_SECONDS" "$KEYS_PER_THREAD"
