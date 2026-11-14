# Optimized local YCSB-E sweep

Configuration: `{'runner': 'build-precision/rrl_workload_runner', 'algorithms': ['next-key', 'gap-lock', 'prior-key'], 'policies': ['no-wait', 'wait-die', 'wound-wait'], 'thetas': [0, 0.1, 0.3, 0.5, 0.7, 0.9, 1.1, 1.3, 1.5], 'keys': 2097152, 'transactions': 5000, 'ops': 10, 'range': 32, 'threads': 32, 'seed': 8, 'warmup': 10, 'duration': 10, 'max_attempts': 1000, 'wait_timeout_ms': 100, 'hierarchy_bucket': 64, 'retry_backoff': 'adaptive', 'latency': False, 'statistics': False, 'timeout': 180, 'shuffle_seed': 20261114}`

Each configuration runs once. Values are measurements, with no interpolation or repeat-based confidence intervals. All figures use the actual theta values as ticks.

Valid cases: 81. Missing cases: [].

Optional latency/row statistics are disabled in this performance sweep. Zero histogram or row fields do not mean no waiting or no reads. Core retry, backoff and completion counters remain enabled.

Record 2PL does not protect gaps or guarantee phantom avoidance. Compare its throughput with that semantic difference in mind. A fixed generated trace is replayed during both windows; inserts can become overwrites after their first successful commit. This is a closed replay experiment, not a growing-table insert benchmark.

## no-wait: committed TPS

| Theta | Next-key | Gap | Prior-key |
|---|---:|---:|---:|
| 0 | 59,264.5 | 41,183.4 | 54,698.4 |
| 0.1 | 52,786.8 | 39,476.1 | 55,038.1 |
| 0.3 | 49,075.6 | 38,490.8 | 55,622.8 |
| 0.5 | 53,727.1 | 38,751.2 | 55,696.3 |
| 0.7 | 44,766.2 | 36,262.7 | 45,226.3 |
| 0.9 | 28,250.3 | 23,081.3 | 30,971.2 |
| 1.1 | 18,677.6 | 13,662.1 | 18,950.0 |
| 1.3 | 15,709.5 | 11,838.4 | 15,921.5 |
| 1.5 | 15,800.7 | 11,762.9 | 16,775.8 |

## wait-die: committed TPS

| Theta | Next-key | Gap | Prior-key |
|---|---:|---:|---:|
| 0 | 56,685.4 | 40,542.7 | 54,269.0 |
| 0.1 | 55,706.0 | 42,945.8 | 55,625.0 |
| 0.3 | 52,215.3 | 37,119.1 | 56,061.6 |
| 0.5 | 54,921.9 | 39,602.9 | 57,464.7 |
| 0.7 | 49,123.1 | 35,208.5 | 48,378.4 |
| 0.9 | 24,764.3 | 18,423.5 | 24,083.6 |
| 1.1 | 9,664.3 | 7,685.8 | 9,576.8 |
| 1.3 | 6,606.2 | 4,699.6 | 6,726.4 |
| 1.5 | 5,780.5 | 4,244.2 | 5,708.9 |

## wound-wait: committed TPS

| Theta | Next-key | Gap | Prior-key |
|---|---:|---:|---:|
| 0 | 53,970.1 | 44,250.0 | 58,535.2 |
| 0.1 | 56,251.6 | 40,123.8 | 57,964.9 |
| 0.3 | 49,575.2 | 38,062.1 | 52,743.9 |
| 0.5 | 50,010.9 | 39,096.6 | 53,442.7 |
| 0.7 | 48,392.3 | 35,837.0 | 45,230.8 |
| 0.9 | 18,952.6 | 13,314.6 | 18,309.3 |
| 1.1 | 10,002.9 | 7,588.2 | 10,447.0 |
| 1.3 | 10,284.7 | 7,441.4 | 10,519.3 |
| 1.5 | 10,066.0 | 7,939.8 | 11,077.0 |

## Theta 1.5: completion and retries

| Algorithm | Policy | TPS | Write txns/s | Aborts/commit | Retries/commit | Exhausted | Unfinished | Max retries | Backoff ms/commit |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Next-key | no-wait | 15,800.7 | 6,300.9 | 2.095 | 2.095 | 0 | 32 | 167 | 1.856 |
| Next-key | wait-die | 5,780.5 | 2,300.7 | 8.663 | 8.662 | 0 | 32 | 37 | 4.344 |
| Next-key | wound-wait | 10,066.0 | 4,026.2 | 13.080 | 13.079 | 0 | 32 | 22 | 1.963 |
| Gap | no-wait | 11,762.9 | 4,700.0 | 2.235 | 2.234 | 0 | 32 | 177 | 2.490 |
| Gap | wait-die | 4,244.2 | 1,693.2 | 9.149 | 9.148 | 0 | 32 | 38 | 5.953 |
| Gap | wound-wait | 7,939.8 | 3,174.2 | 14.089 | 14.089 | 0 | 32 | 24 | 2.458 |
| Prior-key | no-wait | 16,775.8 | 6,701.7 | 1.994 | 1.994 | 0 | 32 | 147 | 1.751 |
| Prior-key | wait-die | 5,708.9 | 2,276.1 | 8.700 | 8.700 | 0 | 32 | 38 | 4.403 |
| Prior-key | wound-wait | 11,077.0 | 4,433.6 | 12.681 | 12.681 | 0 | 32 | 21 | 1.774 |
