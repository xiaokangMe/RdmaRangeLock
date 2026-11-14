# Local Precision Locking CAS specialization

Branch: `codex/record2pl-compact-20261112`, machine `.66`.

## What was removed

The original Precision executor already has no `LockKind::Record` layer.
Record S/X added for the common multi-operation harness belongs to Griffin,
which this change does not modify. Jordan's implicit point predicate before a
write is required, not an extra record lock.

The compact local Precision path bypasses the generic lock manager's shard
mutexes, interval-holder search, dynamic holders, virtual acquire calls,
transaction maps and generic per-acquire bookkeeping. It also avoids physical
B+ tree neighbor discovery, record enumeration for locking, and rescanning.
Scan protection is registered before the single data scan, including empty
results. Read-your-own-write/delete behavior and prepare-before-writeback are
preserved.

## Representation and conflict equivalence

This is a fixed-admission benchmark specialization of Jordan's single-key
interval subset, not a replacement for an unrestricted online predicate
service or Griffin-PPL. The immutable trace declares every possible update
key. No physical record is inserted by registration. No conflict outcome,
transaction order, or scheduling decision is precomputed.

Before measurement, `CompactPrecisionPlans` constructs a sorted unique
directory of **all possible Lu keys** (Insert, Update, Delete), then compiles
each operation to IDs in this directory. It discards the directory once IDs
are compiled. For each admitted update key there is one 8-byte atomic word:

- S bits represent the contexts whose Lp contains that key.
- X represents a writer's fused implicit point Lp plus point Lu.
- Read predicates register S only on admitted Lu keys they contain. A key that
  can never be updated has no Lu counterpart and needs no CAS state.

For admitted operations the conflict condition is exactly `Lp.contains(Lu.key)`.
Two readers coexist. Two writes conflict because each includes a point Lp,
not because Jordan introduces Lu/Lu conflicts. Fusing this inseparable pair
avoids a separate S acquisition/upgrade and its upgrade livelock.

An empty scan `[a,b)` still registers every admitted writer key in that
interval, so an insert into a currently empty interval cannot bypass it.
Half-open right bounds exclude `b`, and UINT64_MAX point writes need no
`key+1` arithmetic. Acquisitions remain in operation order; transactions are
not globally presorted, prelocked or batched. All granted protection remains
until finish/abort. Partial acquisition, failed upgrades and budget failures
release only the current context's preallocated pointer list.

The lower-level atomic authority is reused from `RecordLockTable`; this is
code reuse of generation/age/wound/commit/CAS mechanics, **not an added record
protocol**. The specialized executor issues no Record requests, and the CSV
record-lock space fields remain zero. Complexity is proportional to admitted
update keys covered by each predicate, rather than all returned rows or all
active transaction intervals. This advantage depends on the frozen write
universe; compare it explicitly as a specialization, not as a general online
PL improvement.

## Configuration and limits

```
build-precision/rrl_workload_runner --index v2 --cc precision-lock \
  --precision-lock-table compact --workload ycsb-e --keys 2097152 \
  --txns 5000 --ops 10 --range 32 --threads 32 --zipf 0.9 --seed 8 \
  --execution-mode threads --wait-policy no-wait \
  --max-attempts 1000 --wait-timeout-ms 100 --warmup 1 --duration 3 \
  --precision-latency 0 --precision-statistics 0 --format csv
```

`precision_lock_table` / `--precision-lock-table`: auto, generic, compact.
Auto chooses compact only for supported local execution. Unsupported remote
state or more than 63 active contexts stays generic; explicit compact rejects
it. The 63-context cap preserves exact shared compatibility instead of turning
the 64th reader into a writer. It is a state encoding limit, not a CPU limit.

`precision_latency` / `--precision-latency` and `precision_statistics` /
`--precision-statistics` default to 1. Setting both to 0 selects an executor
instantiation without optional latency histograms, operation/row/checksum
counting. Commit/abort/retry/unfinished/error accounting remains enabled.
Histogram output requires latency enabled. Generic remains a reference backend
and does not accept compact-only metric disabling.

CSV records the actual backend, CAS word bytes, immutable plan bytes,
transaction slot bytes, held pointer capacity and metric flags. Source/trace
compilation and index initialization are outside the timed window; all raw
commands and source snapshots accompany the benchmark. No-wait avoids global
age allocation; wait-die/wound-wait retain an age per logical transaction across
retries. B+ tree structure latches, private writes, commit checks and durable
value lifetimes are not removed.

## Validation

- 20,000 random transaction-pair comparisons against the original Lp/Lu
  `RangeLock` rules, including implicit reads and UINT64_MAX.
- 200 forced concurrent predicate/update registration races; both sides may
  never simultaneously acquire a conflicting pair.
- Empty-range insertion protection, half-open bounds, shared readers, write
  conflicts, no-writer traces, mixed insert/delete/reinsert/upsert/read-own-write
  behavior and final-tree equivalence against the generic executor.
- Runner coverage uses YCSB-E, all three wait policies, 1/4/32/63 contexts,
  coroutine execution, warmup, timed runs, budgets, metrics, JSON options,
  explicit rejection and auto fallback.
- Release full CTest: 74 registered, 53 executed passed, 21 environment-dependent
  RDMA tests skipped, zero failures. This is local validation, not a new RDMA run.

The benchmark script `scripts/benchmark_compact_precision.py` keeps editable
CONFIG variables at the top. It compares generic/full, compact/full and
compact/fast with the same trace parameters and three repeats. It waits for
other benchmark campaigns and marks a sample invalid if another campaign
overlaps. Exact-range and other earlier work in this branch are retained.
