# Precision CAS YCSB-E comparison

Each cell is the median of three independent runs; all raw runs retained.
CONFIG: `{'runner': 'build-precision/rrl_workload_runner', 'keys': 2097152, 'transactions': 5000, 'ops': 10, 'range': 32, 'threads': 32, 'seed': 8, 'thetas': [0.0, 0.9, 1.5], 'policies': ['no-wait', 'wait-die', 'wound-wait'], 'rounds': 3, 'warmup': 1, 'duration': 3, 'max_attempts': 1000, 'wait_timeout_ms': 100, 'timeout': 120, 'max_idle_wait': 1800}`

| theta | policy | generic/full TPS | compact/full TPS | compact/fast TPS | full speedup |
|---:|---|---:|---:|---:|---:|
| 0 | no-wait | 40,598.2 | 204,132.0 | 254,624.0 | 5.03x |
| 0 | wait-die | 39,549.9 | 213,601.0 | 235,340.0 | 5.40x |
| 0 | wound-wait | 39,214.9 | 196,740.0 | 263,788.0 | 5.02x |
| 0.9 | no-wait | 733.6 | 8,324.0 | 8,080.2 | 11.35x |
| 0.9 | wait-die | 730.2 | 5,945.3 | 5,096.8 | 8.14x |
| 0.9 | wound-wait | 609.4 | 2,961.4 | 2,477.6 | 4.86x |
| 1.5 | no-wait | 58.6 | 613.8 | 393.6 | 10.47x |
| 1.5 | wait-die | 56.0 | 594.5 | 369.9 | 10.62x |
| 1.5 | wound-wait | 36.0 | 149.6 | 155.6 | 4.16x |

This is a fixed-admission specialization: only possible Lu keys have CAS words. The admitted write universe and per-operation predicate mapping are compiled before measurement. It is not an unrestricted online predicate service.
Fast mode disables optional latency/operation/checksum metrics; commits, aborts, retries, unfinished and errors are still counted.
The finite trace is repeated; warmup may turn inserts into overwrites. No result claims continuous keyspace growth.
Failures: 0. See results.csv for per-trial retries, wait, retry_exhausted and logical_unfinished.


## Completion costs

Per-trial ratios and counts below are medians across the three runs. Fast mode has no optional wait histogram; its wait value is unavailable, not zero.

| theta | policy | backend/metrics | TPS min--max | abort/commit | retry/commit | wait ms/commit | unfinished | retry exhausted | deadline aborts |
|---:|---|---|---:|---:|---:|---:|---:|---:|---:|
| 0 | no-wait | generic/full | 39,671.2--40,835.3 | 0.01 | 0.01 | 0.0000 | 22 | 0 | 22 |
| 0 | no-wait | compact/full | 194,642.0--205,767.0 | 0.32 | 0.32 | 0.0000 | 32 | 0 | 32 |
| 0 | no-wait | compact/fast | 236,000.0--273,544.0 | 0.15 | 0.15 | n/a | 32 | 0 | 31 |
| 0 | wait-die | generic/full | 34,116.0--41,474.8 | 0.01 | 0.01 | 0.0003 | 23 | 0 | 23 |
| 0 | wait-die | compact/full | 171,392.0--226,600.0 | 0.23 | 0.23 | 0.0001 | 31 | 0 | 31 |
| 0 | wait-die | compact/fast | 231,244.0--248,877.0 | 0.19 | 0.19 | n/a | 31 | 0 | 31 |
| 0 | wound-wait | generic/full | 36,958.5--39,981.9 | 0.00 | 0.00 | 0.0012 | 20 | 0 | 20 |
| 0 | wound-wait | compact/full | 182,553.0--220,957.0 | 0.00 | 0.00 | 0.0015 | 32 | 0 | 32 |
| 0 | wound-wait | compact/fast | 241,398.0--295,808.0 | 0.00 | 0.00 | n/a | 31 | 0 | 31 |
| 0.9 | no-wait | generic/full | 584.3--749.9 | 80.60 | 80.53 | 0.0000 | 167 | 135 | 17 |
| 0.9 | no-wait | compact/full | 7,160.1--8,518.4 | 79.51 | 79.44 | 0.0000 | 1,722 | 1,690 | 31 |
| 0.9 | no-wait | compact/fast | 8,065.2--8,533.8 | 79.21 | 79.14 | n/a | 1,714 | 1,682 | 30 |
| 0.9 | wait-die | generic/full | 610.0--745.2 | 82.67 | 82.59 | 4.0822 | 167 | 135 | 96 |
| 0.9 | wait-die | compact/full | 5,014.6--6,183.3 | 101.87 | 101.78 | 0.7371 | 1,703 | 1,671 | 152 |
| 0.9 | wait-die | compact/fast | 3,963.9--7,402.6 | 104.00 | 103.90 | n/a | 1,599 | 1,567 | 132 |
| 0.9 | wound-wait | generic/full | 438.5--615.4 | 111.46 | 111.39 | 24.0255 | 118 | 86 | 260 |
| 0.9 | wound-wait | compact/full | 2,559.5--3,094.9 | 245.98 | 245.87 | 1.2503 | 886 | 854 | 24 |
| 0.9 | wound-wait | compact/fast | 2,430.3--2,734.6 | 269.19 | 269.06 | n/a | 954 | 922 | 24 |
| 1.5 | no-wait | generic/full | 53.3--61.0 | 502.19 | 501.58 | 0.0000 | 104 | 72 | 24 |
| 1.5 | no-wait | compact/full | 413.2--650.9 | 503.76 | 503.26 | 0.0000 | 937 | 905 | 32 |
| 1.5 | no-wait | compact/fast | 206.6--401.2 | 529.05 | 528.51 | n/a | 622 | 590 | 31 |
| 1.5 | wait-die | generic/full | 51.0--57.3 | 494.50 | 493.90 | 17.9045 | 102 | 70 | 52 |
| 1.5 | wait-die | compact/full | 505.9--617.8 | 526.90 | 526.37 | 3.0629 | 955 | 923 | 83 |
| 1.5 | wait-die | compact/fast | 323.2--424.9 | 530.56 | 530.02 | n/a | 601 | 569 | 62 |
| 1.5 | wound-wait | generic/full | 28.3--40.0 | 1666.45 | 1664.94 | 250.9169 | 161 | 129 | 230 |
| 1.5 | wound-wait | compact/full | 88.0--156.3 | 7287.72 | 7280.85 | 47.1205 | 3,102 | 3,070 | 28 |
| 1.5 | wound-wait | compact/fast | 117.0--167.3 | 6461.04 | 6455.00 | n/a | 3,002 | 2,970 | 31 |

Compact CAS state: 3,280--19,824 bytes; immutable operation plans: 720,000--720,000 bytes. Transaction contexts and held pointers are additional and recorded in raw CSV.

At high skew the completion counts and run-to-run ranges remain material. Disabling metrics does not consistently improve committed throughput under contention. No claim that retries or hotspot collapse have been eliminated.
