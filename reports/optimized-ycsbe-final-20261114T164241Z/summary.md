# Optimized local YCSB-E sweep

Configuration: `{'runner': 'build-precision/rrl_workload_runner', 'algorithms': ['record-2pl', 'next-key', 'gap-lock', 'prior-key', 'hierarchical', 'exact-range'], 'policies': ['no-wait', 'wait-die', 'wound-wait'], 'thetas': [0, 0.1, 0.3, 0.5, 0.7, 0.9, 1.1, 1.3, 1.5], 'keys': 2097152, 'transactions': 5000, 'ops': 10, 'range': 32, 'threads': 32, 'seed': 8, 'warmup': 10, 'duration': 10, 'max_attempts': 1000, 'wait_timeout_ms': 100, 'hierarchy_bucket': 64, 'retry_backoff': 'adaptive', 'latency': False, 'statistics': False, 'timeout': 180, 'shuffle_seed': 20261114}`

Each configuration runs once. Values are measurements, with no interpolation or repeat-based confidence intervals. All figures use the actual theta values as ticks.

Valid cases: 162. Missing cases: [].

Optional latency/row statistics are disabled in this performance sweep. Zero histogram or row fields do not mean no waiting or no reads. Core retry, backoff and completion counters remain enabled.

Record 2PL does not protect gaps or guarantee phantom avoidance. Compare its throughput with that semantic difference in mind. A fixed generated trace is replayed during both windows; inserts can become overwrites after their first successful commit. This is a closed replay experiment, not a growing-table insert benchmark.

The selected 162 measurements come from two explicitly identified source campaigns: Record/Hierarchy/Exact use the completed six-algorithm sweep; Next-key/Gap/Prior use the full 81-case rerun after scan-coverage reuse. All workload and measurement parameters match. No data points from the earlier classic implementation are retained in those three curves. The later code change is restricted to the classic interval execution branch. Each selected configuration has one measured trial; this is not a same-binary six-algorithm rerun.

| Algorithms | Source campaign |
|---|---|
| record-2pl, hierarchical, exact-range | optimized-ycsbe-20261114T145856Z |
| next-key, gap-lock, prior-key | optimized-ycsbe-20261114T161224Z |

## no-wait: committed TPS

| Theta | Record 2PL | Next-key | Gap | Prior-key | Hierarchy b64 | Exact-range |
|---|---:|---:|---:|---:|---:|---:|
| 0 | 103,447.0 | 59,264.5 | 41,183.4 | 54,698.4 | 273,538.0 | 236,889.0 |
| 0.1 | 148,171.0 | 52,786.8 | 39,476.1 | 55,038.1 | 276,032.0 | 261,666.0 |
| 0.3 | 101,080.0 | 49,075.6 | 38,490.8 | 55,622.8 | 261,554.0 | 236,918.0 |
| 0.5 | 126,304.0 | 53,727.1 | 38,751.2 | 55,696.3 | 265,138.0 | 276,829.0 |
| 0.7 | 118,119.0 | 44,766.2 | 36,262.7 | 45,226.3 | 245,940.0 | 229,470.0 |
| 0.9 | 68,026.6 | 28,250.3 | 23,081.3 | 30,971.2 | 79,023.6 | 100,127.0 |
| 1.1 | 34,491.0 | 18,677.6 | 13,662.1 | 18,950.0 | 113,221.0 | 50,924.7 |
| 1.3 | 24,154.3 | 15,709.5 | 11,838.4 | 15,921.5 | 124,816.0 | 49,390.7 |
| 1.5 | 19,890.2 | 15,800.7 | 11,762.9 | 16,775.8 | 132,680.0 | 49,670.4 |

## wait-die: committed TPS

| Theta | Record 2PL | Next-key | Gap | Prior-key | Hierarchy b64 | Exact-range |
|---|---:|---:|---:|---:|---:|---:|
| 0 | 138,246.0 | 56,685.4 | 40,542.7 | 54,269.0 | 289,086.0 | 229,290.0 |
| 0.1 | 139,687.0 | 55,706.0 | 42,945.8 | 55,625.0 | 271,815.0 | 268,703.0 |
| 0.3 | 122,366.0 | 52,215.3 | 37,119.1 | 56,061.6 | 280,442.0 | 258,644.0 |
| 0.5 | 128,282.0 | 54,921.9 | 39,602.9 | 57,464.7 | 262,453.0 | 262,521.0 |
| 0.7 | 111,604.0 | 49,123.1 | 35,208.5 | 48,378.4 | 245,994.0 | 232,539.0 |
| 0.9 | 67,355.8 | 24,764.3 | 18,423.5 | 24,083.6 | 156,836.0 | 145,422.0 |
| 1.1 | 18,800.8 | 9,664.3 | 7,685.8 | 9,576.8 | 138,393.0 | 84,953.6 |
| 1.3 | 12,082.6 | 6,606.2 | 4,699.6 | 6,726.4 | 170,343.0 | 58,196.1 |
| 1.5 | 10,467.4 | 5,780.5 | 4,244.2 | 5,708.9 | 115,565.0 | 61,444.5 |

## wound-wait: committed TPS

| Theta | Record 2PL | Next-key | Gap | Prior-key | Hierarchy b64 | Exact-range |
|---|---:|---:|---:|---:|---:|---:|
| 0 | 114,469.0 | 53,970.1 | 44,250.0 | 58,535.2 | 283,044.0 | 245,652.0 |
| 0.1 | 124,771.0 | 56,251.6 | 40,123.8 | 57,964.9 | 279,187.0 | 288,685.0 |
| 0.3 | 126,764.0 | 49,575.2 | 38,062.1 | 52,743.9 | 271,242.0 | 248,268.0 |
| 0.5 | 118,963.0 | 50,010.9 | 39,096.6 | 53,442.7 | 272,065.0 | 292,780.0 |
| 0.7 | 123,089.0 | 48,392.3 | 35,837.0 | 45,230.8 | 229,241.0 | 221,712.0 |
| 0.9 | 50,412.2 | 18,952.6 | 13,314.6 | 18,309.3 | 126,890.0 | 141,175.0 |
| 1.1 | 24,963.2 | 10,002.9 | 7,588.2 | 10,447.0 | 44,357.9 | 78,867.6 |
| 1.3 | 17,914.8 | 10,284.7 | 7,441.4 | 10,519.3 | 62,045.2 | 59,770.2 |
| 1.5 | 19,440.8 | 10,066.0 | 7,939.8 | 11,077.0 | 32,650.4 | 31,398.7 |

## Theta 1.5: completion and retries

| Algorithm | Policy | TPS | Write txns/s | Aborts/commit | Retries/commit | Exhausted | Unfinished | Max retries | Backoff ms/commit |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Record 2PL | no-wait | 19,890.2 | 7,933.8 | 1.845 | 1.845 | 0 | 32 | 193 | 1.493 |
| Record 2PL | wait-die | 10,467.4 | 4,179.9 | 7.580 | 7.580 | 0 | 30 | 34 | 2.502 |
| Record 2PL | wound-wait | 19,440.8 | 7,777.8 | 11.126 | 11.126 | 0 | 32 | 18 | 1.061 |
| Next-key | no-wait | 15,800.7 | 6,300.9 | 2.095 | 2.095 | 0 | 32 | 167 | 1.856 |
| Next-key | wait-die | 5,780.5 | 2,300.7 | 8.663 | 8.662 | 0 | 32 | 37 | 4.344 |
| Next-key | wound-wait | 10,066.0 | 4,026.2 | 13.080 | 13.079 | 0 | 32 | 22 | 1.963 |
| Gap | no-wait | 11,762.9 | 4,700.0 | 2.235 | 2.234 | 0 | 32 | 177 | 2.490 |
| Gap | wait-die | 4,244.2 | 1,693.2 | 9.149 | 9.148 | 0 | 32 | 38 | 5.953 |
| Gap | wound-wait | 7,939.8 | 3,174.2 | 14.089 | 14.089 | 0 | 32 | 24 | 2.458 |
| Prior-key | no-wait | 16,775.8 | 6,701.7 | 1.994 | 1.994 | 0 | 32 | 147 | 1.751 |
| Prior-key | wait-die | 5,708.9 | 2,276.1 | 8.700 | 8.700 | 0 | 32 | 38 | 4.403 |
| Prior-key | wound-wait | 11,077.0 | 4,433.6 | 12.681 | 12.681 | 0 | 32 | 21 | 1.774 |
| Hierarchy b64 | no-wait | 132,680.0 | 53,109.9 | 0.095 | 0.095 | 0 | 32 | 224 | 0.233 |
| Hierarchy b64 | wait-die | 115,565.0 | 46,171.6 | 3.111 | 3.111 | 0 | 32 | 16 | 0.151 |
| Hierarchy b64 | wound-wait | 32,650.4 | 13,061.5 | 9.560 | 9.560 | 0 | 32 | 15 | 0.633 |
| Exact-range | no-wait | 49,670.4 | 19,814.7 | 1.577 | 1.577 | 0 | 31 | 97 | 0.613 |
| Exact-range | wait-die | 61,444.5 | 24,525.3 | 4.240 | 4.240 | 0 | 32 | 22 | 0.381 |
| Exact-range | wound-wait | 31,398.7 | 12,559.8 | 10.105 | 10.105 | 0 | 32 | 16 | 0.736 |

## Scan reuse: before/after single-trial comparison

Both runs use 10s warmup and 10s measurement with optional metrics disabled. These ratios compare two single measurements, not a statistical speedup estimate. Mid-skew regressions are retained in scan_reuse_effect.csv.

| Algorithm | Policy | Theta | Before TPS | After TPS | Ratio |
|---|---|---:|---:|---:|---:|
| gap-lock | no-wait | 1.500 | 9603.470 | 11762.900 | 1.225 |
| gap-lock | wait-die | 1.500 | 3411.820 | 4244.210 | 1.244 |
| gap-lock | wound-wait | 1.500 | 5903.710 | 7939.800 | 1.345 |
| next-key | no-wait | 1.500 | 13772.700 | 15800.700 | 1.147 |
| next-key | wait-die | 1.500 | 5538.200 | 5780.480 | 1.044 |
| next-key | wound-wait | 1.500 | 8351.100 | 10066.000 | 1.205 |
| prior-key | no-wait | 1.500 | 13916.600 | 16775.800 | 1.205 |
| prior-key | wait-die | 1.500 | 4596.180 | 5708.860 | 1.242 |
| prior-key | wound-wait | 1.500 | 9266.250 | 11077.000 | 1.195 |
