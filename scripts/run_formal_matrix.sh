#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Same executor for local and cross-host; old lock-only matrix is retired.
exec python3 "$ROOT/scripts/run_shared_matrices.py" --environment-config \
  --build "${RRL_BUILD_DIR:-$ROOT/build-shared}" "$@"
