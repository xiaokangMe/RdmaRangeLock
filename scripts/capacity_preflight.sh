#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Run from the control host .66. Starts fresh, matched data/lock/coordinator
# services and probes the actual shared-data path; never starts the matrix.
exec python3 "$ROOT/scripts/run_shared_matrices.py" --environment-config \
  --mode cross-host --preflight-only --build "${RRL_BUILD_DIR:-$ROOT/build-shared}" "$@"
