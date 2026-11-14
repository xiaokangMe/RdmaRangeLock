# Optimized local YCSB-E sweep

Configuration: `{'runner': 'build-precision/rrl_workload_runner', 'algorithms': ['record-2pl', 'next-key', 'gap-lock', 'prior-key', 'hierarchical', 'exact-range'], 'policies': ['no-wait', 'wait-die', 'wound-wait'], 'thetas': [0, 0.1, 0.3, 0.5, 0.7, 0.9, 1.1, 1.3, 1.5], 'keys': 2097152, 'transactions': 5000, 'ops': 10, 'range': 32, 'threads': 32, 'seed': 8, 'warmup': 10, 'duration': 10, 'max_attempts': 1000, 'wait_timeout_ms': 100, 'hierarchy_bucket': 64, 'retry_backoff': 'adaptive', 'latency': False, 'statistics': False, 'timeout': 180, 'shuffle_seed': 20261114}`

Each configuration runs once. Values are measurements, with no interpolation or repeat-based confidence intervals. All figures use the actual theta values as ticks.

Valid cases: 162. Missing cases: [].

Optional latency/row statistics are disabled in this performance sweep. Zero histogram or row fields do not mean no waiting or no reads. Core retry, backoff and completion counters remain enabled.

Record 2PL does not protect gaps or guarantee phantom avoidance. Compare its throughput with that semantic difference in mind. A fixed generated trace is replayed during both windows; inserts can become overwrites after their first successful commit. This is a closed replay experiment, not a growing-table insert benchmark.

## no-wait: committed TPS

| Theta | Record 2PL | Next-key | Gap | Prior-key | Hierarchy b64 | Exact-range |
|---|---:|---:|---:|---:|---:|---:|
| 0 | 103,447.0 | 58,834.8 | 42,260.1 | 58,928.1 | 273,538.0 | 236,889.0 |
| 0.1 | 148,171.0 | 55,178.8 | 40,882.0 | 55,337.5 | 276,032.0 | 261,666.0 |
| 0.3 | 101,080.0 | 52,776.9 | 37,405.1 | 50,495.1 | 261,554.0 | 236,918.0 |
| 0.5 | 126,304.0 | 56,155.9 | 41,595.6 | 58,620.1 | 265,138.0 | 276,829.0 |
| 0.7 | 118,119.0 | 48,733.8 | 34,490.1 | 46,816.9 | 245,940.0 | 229,470.0 |
| 0.9 | 68,026.6 | 40,010.0 | 21,998.0 | 37,338.7 | 79,023.6 | 100,127.0 |
| 1.1 | 34,491.0 | 22,250.3 | 15,966.7 | 22,385.6 | 113,221.0 | 50,924.7 |
| 1.3 | 24,154.3 | 13,924.5 | 10,792.0 | 13,689.9 | 124,816.0 | 49,390.7 |
| 1.5 | 19,890.2 | 13,772.7 | 9,603.5 | 13,916.6 | 132,680.0 | 49,670.4 |

## wait-die: committed TPS

| Theta | Record 2PL | Next-key | Gap | Prior-key | Hierarchy b64 | Exact-range |
|---|---:|---:|---:|---:|---:|---:|
| 0 | 138,246.0 | 58,492.4 | 43,668.9 | 52,850.3 | 289,086.0 | 229,290.0 |
| 0.1 | 139,687.0 | 51,314.1 | 40,576.5 | 50,161.9 | 271,815.0 | 268,703.0 |
| 0.3 | 122,366.0 | 52,473.1 | 40,072.4 | 50,874.2 | 280,442.0 | 258,644.0 |
| 0.5 | 128,282.0 | 55,233.0 | 41,403.0 | 58,404.8 | 262,453.0 | 262,521.0 |
| 0.7 | 111,604.0 | 50,457.7 | 36,454.7 | 49,811.8 | 245,994.0 | 232,539.0 |
| 0.9 | 67,355.8 | 24,103.9 | 17,188.3 | 23,587.9 | 156,836.0 | 145,422.0 |
| 1.1 | 18,800.8 | 11,121.2 | 8,405.0 | 9,337.4 | 138,393.0 | 84,953.6 |
| 1.3 | 12,082.6 | 6,332.7 | 4,139.4 | 6,988.8 | 170,343.0 | 58,196.1 |
| 1.5 | 10,467.4 | 5,538.2 | 3,411.8 | 4,596.2 | 115,565.0 | 61,444.5 |

## wound-wait: committed TPS

| Theta | Record 2PL | Next-key | Gap | Prior-key | Hierarchy b64 | Exact-range |
|---|---:|---:|---:|---:|---:|---:|
| 0 | 114,469.0 | 60,403.7 | 42,042.7 | 58,039.3 | 283,044.0 | 245,652.0 |
| 0.1 | 124,771.0 | 53,704.7 | 40,784.7 | 53,327.8 | 279,187.0 | 288,685.0 |
| 0.3 | 126,764.0 | 54,301.7 | 38,219.3 | 51,163.8 | 271,242.0 | 248,268.0 |
| 0.5 | 118,963.0 | 52,842.9 | 41,515.8 | 54,371.4 | 272,065.0 | 292,780.0 |
| 0.7 | 123,089.0 | 47,442.9 | 35,891.6 | 48,924.0 | 229,241.0 | 221,712.0 |
| 0.9 | 50,412.2 | 19,224.2 | 15,069.8 | 18,869.4 | 126,890.0 | 141,175.0 |
| 1.1 | 24,963.2 | 9,552.0 | 7,377.9 | 9,752.1 | 44,357.9 | 78,867.6 |
| 1.3 | 17,914.8 | 11,103.3 | 6,074.0 | 9,192.6 | 62,045.2 | 59,770.2 |
| 1.5 | 19,440.8 | 8,351.1 | 5,903.7 | 9,266.2 | 32,650.4 | 31,398.7 |

## Theta 1.5: completion and retries

| Algorithm | Policy | TPS | Write txns/s | Aborts/commit | Retries/commit | Exhausted | Unfinished | Max retries | Backoff ms/commit |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Record 2PL | no-wait | 19,890.2 | 7,933.8 | 1.845 | 1.845 | 0 | 32 | 193 | 1.493 |
| Record 2PL | wait-die | 10,467.4 | 4,179.9 | 7.580 | 7.580 | 0 | 30 | 34 | 2.502 |
| Record 2PL | wound-wait | 19,440.8 | 7,777.8 | 11.126 | 11.126 | 0 | 32 | 18 | 1.061 |
| Next-key | no-wait | 13,772.7 | 5,491.3 | 2.053 | 2.052 | 0 | 32 | 177 | 2.140 |
| Next-key | wait-die | 5,538.2 | 2,207.7 | 8.892 | 8.891 | 0 | 32 | 37 | 4.606 |
| Next-key | wound-wait | 8,351.1 | 3,338.4 | 14.077 | 14.077 | 0 | 32 | 22 | 2.443 |
| Gap | no-wait | 9,603.5 | 3,837.8 | 2.349 | 2.348 | 0 | 32 | 183 | 3.095 |
| Gap | wait-die | 3,411.8 | 1,358.3 | 9.633 | 9.632 | 0 | 32 | 42 | 7.568 |
| Gap | wound-wait | 5,903.7 | 2,364.2 | 15.894 | 15.893 | 0 | 32 | 27 | 3.402 |
| Prior-key | no-wait | 13,916.6 | 5,551.6 | 2.024 | 2.024 | 0 | 32 | 149 | 2.120 |
| Prior-key | wait-die | 4,596.2 | 1,832.9 | 9.008 | 9.007 | 0 | 32 | 37 | 5.508 |
| Prior-key | wound-wait | 9,266.2 | 3,705.5 | 13.645 | 13.645 | 0 | 32 | 22 | 2.217 |
| Hierarchy b64 | no-wait | 132,680.0 | 53,109.9 | 0.095 | 0.095 | 0 | 32 | 224 | 0.233 |
| Hierarchy b64 | wait-die | 115,565.0 | 46,171.6 | 3.111 | 3.111 | 0 | 32 | 16 | 0.151 |
| Hierarchy b64 | wound-wait | 32,650.4 | 13,061.5 | 9.560 | 9.560 | 0 | 32 | 15 | 0.633 |
| Exact-range | no-wait | 49,670.4 | 19,814.7 | 1.577 | 1.577 | 0 | 31 | 97 | 0.613 |
| Exact-range | wait-die | 61,444.5 | 24,525.3 | 4.240 | 4.240 | 0 | 32 | 22 | 0.381 |
| Exact-range | wound-wait | 31,398.7 | 12,559.8 | 10.105 | 10.105 | 0 | 32 | 16 | 0.736 |
