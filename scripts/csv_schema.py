#!/usr/bin/env python3
"""Shared CSV reader for the experiment results.

`csv.DictReader` silently puts values that have no header entry into the
``None`` key, so a result file whose rows are wider than its header loses those
columns without any error. That is exactly how the ten diagnostic columns of
the formal matrix went missing from every analysis: the runner grew, the
hard-coded header in ``run_formal_matrix.sh`` did not.

This loader refuses that mismatch and names the columns the caller depends on,
so a schema change fails loudly at load time instead of producing a plausible
but wrong table.
"""

import csv
import os
import sys


def load_rows(handle):
    """Reads a result CSV, rejecting rows whose width differs from the header."""
    reader = csv.reader(handle)
    try:
        header = next(reader)
    except StopIteration:
        raise SystemExit("%s: empty CSV" % name_of(handle))
    rows = []
    for lineno, row in enumerate(reader, 2):
        if len(row) != len(header):
            raise SystemExit(
                "%s:%d: %d fields but the header has %d"
                % (name_of(handle), lineno, len(row), len(header)))
        rows.append(dict(zip(header, row)))
    return rows


def name_of(handle):
    return getattr(handle, "name", "<csv>")


def require(rows, names):
    """Fails when a column the caller needs is absent."""
    if not rows:
        return
    missing = [name for name in names if name not in rows[0]]
    if missing:
        raise SystemExit("missing columns: %s" % ", ".join(missing))


def selftest():
    """Round-trips a good file and rejects a bad one; used by CTest."""
    import tempfile

    with tempfile.TemporaryDirectory() as tmp:
        good = os.path.join(tmp, "good.csv")
        with open(good, "w", encoding="utf-8") as handle:
            handle.write("a,b,c\n1,2,3\n4,5,6\n")
        with open(good, newline="", encoding="utf-8") as handle:
            rows = load_rows(handle)
        if len(rows) != 2 or rows[1]["c"] != "6":
            print("FAIL: good file did not parse")
            return 1

        bad = os.path.join(tmp, "bad.csv")
        with open(bad, "w", encoding="utf-8") as handle:
            handle.write("a,b\n1,2,3\n")
        try:
            with open(bad, newline="", encoding="utf-8") as handle:
                load_rows(handle)
        except SystemExit as error:
            if "3 fields but the header has 2" not in str(error):
                print("FAIL: unexpected message: %s" % error)
                return 1
        else:
            print("FAIL: a wider row was accepted")
            return 1

    print("csv schema: OK")
    return 0


if __name__ == "__main__":
    sys.exit(selftest())
