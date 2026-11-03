#!/usr/bin/env python3
"""Sequential, ten-second local/RoCE matrices with capacity and result gates."""
import argparse
import csv
import datetime
import hashlib
import io
import json
import os
from pathlib import Path
import shlex
import socket
import subprocess
import tempfile
import traceback


def command(argv, **kwargs):
    return subprocess.run(argv, check=True, **kwargs)


def read_rows(path):
    with Path(path).open(newline="") as handle:
        reader = csv.DictReader(handle)
        rows = list(reader)
    if len(reader.fieldnames) != len(set(reader.fieldnames)) or any(None in r or None in r.values() for r in rows):
        raise RuntimeError("bad CSV schema: " + str(path))
    return rows


ERRORS = ("lock_errors", "index_errors", "lock_release_failures", "failed_transactions",
          "rdma_timeouts", "rdma_completion_errors", "rdma_count_errors", "rdma_local_errors")


def validate(rows, duration, expected=None):
    if expected is not None and len(rows) != expected:
        raise RuntimeError(f"expected {expected} cases; got {len(rows)}")
    for row in rows:
        if int(row["elapsed_ns"]) < duration * 1_000_000_000:
            raise RuntimeError("measurement window below configured duration")
        if int(row["committed"]) + int(row["aborted"]) != int(row["transactions"]):
            raise RuntimeError("transaction accounting mismatch")
        if any(int(row[field]) for field in ERRORS):
            raise RuntimeError("hard measurement error")
        if int(row["rdma_profile_enabled"]):
            raise RuntimeError("formal profile must be disabled")
        if row["cc"].startswith("rdma-") and int(row["rdma_slot_lookup_linear"]) != 1:
            raise RuntimeError("wrong RDMA lookup policy")



def main():
    # Keep read_rows/validate for historical report imports, but expose only the
    # shared-data executor for new campaigns.
    from run_shared_matrices import main as shared_main
    shared_main()


if __name__ == "__main__":
    main()
