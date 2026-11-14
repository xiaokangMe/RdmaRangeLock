# YCSB-E retry control comparison

Single-machine compact backends, same trace and unchanged 1000-attempt cap. Each cell is the median of three runs. All individual results and source snapshots are retained.

CONFIG: `{'runner': 'build-precision/rrl_workload_runner', 'algorithms': ['record-2pl', 'next-key', 'gap-lock', 'prior-key', 'hierarchical', 'exact-range', 'precision-lock'], 'policies': ['no-wait', 'wait-die', 'wound-wait'], 'thetas': [0.0, 1.5], 'retry_modes': ['off', 'adaptive'], 'rounds': 3, 'keys': 2097152, 'transactions': 5000, 'ops': 10, 'range': 32, 'threads': 32, 'seed': 8, 'warmup': 1, 'duration': 2, 'max_attempts': 1000, 'wait_timeout_ms': 100, 'hierarchy_bucket': 64, 'timeout': 90}`

Failures: 0; valid trials: 252.

## Theta 0

| Algorithm | Policy | Off TPS | Adaptive TPS | Ratio | Off exhausted | Adaptive exhausted | Off writes/s | Adaptive writes/s | Off unfinished | Adaptive unfinished |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| record-2pl | no-wait | 116,443.0 | 106,431.0 | 0.91x | 0 | 0 | 46,559.4 | 42,668.7 | 32 | 32 |
| record-2pl | wait-die | 115,286.0 | 109,665.0 | 0.95x | 0 | 0 | 46,051.6 | 43,915.8 | 30 | 31 |
| record-2pl | wound-wait | 121,181.0 | 102,478.0 | 0.85x | 0 | 0 | 48,486.6 | 40,781.0 | 31 | 29 |
| next-key | no-wait | 42,788.5 | 41,368.5 | 0.97x | 0 | 0 | 17,075.1 | 16,597.0 | 32 | 32 |
| next-key | wait-die | 41,559.9 | 41,809.5 | 1.01x | 0 | 0 | 16,603.3 | 16,667.5 | 32 | 31 |
| next-key | wound-wait | 40,451.1 | 41,143.0 | 1.02x | 0 | 0 | 16,228.8 | 16,485.6 | 32 | 32 |
| gap-lock | no-wait | 40,414.4 | 41,691.8 | 1.03x | 0 | 0 | 16,053.9 | 16,638.1 | 32 | 32 |
| gap-lock | wait-die | 42,547.4 | 41,964.5 | 0.99x | 0 | 0 | 16,986.8 | 16,637.8 | 32 | 32 |
| gap-lock | wound-wait | 41,207.2 | 39,384.3 | 0.96x | 0 | 0 | 16,485.6 | 15,757.3 | 31 | 32 |
| prior-key | no-wait | 40,180.3 | 42,275.5 | 1.05x | 0 | 0 | 16,116.7 | 17,011.9 | 32 | 32 |
| prior-key | wait-die | 41,761.8 | 42,709.5 | 1.02x | 0 | 0 | 16,696.5 | 17,035.6 | 32 | 31 |
| prior-key | wound-wait | 40,588.2 | 42,436.9 | 1.05x | 0 | 0 | 16,188.9 | 16,906.6 | 31 | 31 |
| hierarchical | no-wait | 225,617.0 | 233,222.0 | 1.03x | 0 | 0 | 90,320.4 | 93,423.3 | 30 | 31 |
| hierarchical | wait-die | 230,797.0 | 242,542.0 | 1.05x | 0 | 0 | 92,277.1 | 97,719.4 | 31 | 30 |
| hierarchical | wound-wait | 240,321.0 | 227,588.0 | 0.95x | 0 | 0 | 96,104.5 | 90,966.2 | 31 | 30 |
| exact-range | no-wait | 229,914.0 | 221,742.0 | 0.96x | 0 | 0 | 92,264.7 | 88,529.9 | 30 | 30 |
| exact-range | wait-die | 214,123.0 | 206,760.0 | 0.97x | 0 | 0 | 85,506.5 | 82,968.0 | 30 | 30 |
| exact-range | wound-wait | 202,783.0 | 199,593.0 | 0.98x | 0 | 0 | 81,158.7 | 80,059.2 | 31 | 32 |
| precision-lock | no-wait | 207,698.0 | 222,521.0 | 1.07x | 0 | 0 | 83,332.9 | 88,733.5 | 32 | 31 |
| precision-lock | wait-die | 199,043.0 | 229,410.0 | 1.15x | 0 | 0 | 79,646.9 | 91,550.1 | 30 | 31 |
| precision-lock | wound-wait | 212,564.0 | 212,112.0 | 1.00x | 0 | 0 | 85,243.8 | 84,912.9 | 28 | 32 |

## Theta 1.5

| Algorithm | Policy | Off TPS | Adaptive TPS | Ratio | Off exhausted | Adaptive exhausted | Off writes/s | Adaptive writes/s | Off unfinished | Adaptive unfinished |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| record-2pl | no-wait | 239.9 | 12,634.9 | 52.67x | 248 | 0 | 23.5 | 5,025.6 | 280 | 32 |
| record-2pl | wait-die | 257.9 | 9,964.7 | 38.64x | 251 | 0 | 26.5 | 3,981.1 | 283 | 32 |
| record-2pl | wound-wait | 1,175.1 | 23,036.6 | 19.60x | 292 | 0 | 477.8 | 9,198.0 | 324 | 31 |
| next-key | no-wait | 185.9 | 3,167.4 | 17.04x | 153 | 0 | 13.0 | 1,259.0 | 185 | 32 |
| next-key | wait-die | 186.4 | 2,700.6 | 14.49x | 160 | 0 | 11.5 | 1,068.5 | 192 | 32 |
| next-key | wound-wait | 1,227.0 | 8,518.6 | 6.94x | 233 | 0 | 488.8 | 3,413.4 | 265 | 32 |
| gap-lock | no-wait | 162.4 | 2,483.4 | 15.29x | 136 | 0 | 11.0 | 972.9 | 168 | 32 |
| gap-lock | wait-die | 169.4 | 2,334.6 | 13.78x | 158 | 0 | 10.5 | 914.4 | 190 | 32 |
| gap-lock | wound-wait | 986.1 | 7,923.9 | 8.04x | 306 | 0 | 399.3 | 3,167.6 | 338 | 32 |
| prior-key | no-wait | 164.9 | 2,845.5 | 17.25x | 150 | 0 | 10.5 | 1,124.4 | 182 | 32 |
| prior-key | wait-die | 169.9 | 2,686.2 | 15.81x | 152 | 0 | 13.0 | 1,065.5 | 184 | 32 |
| prior-key | wound-wait | 966.6 | 8,111.7 | 8.39x | 270 | 0 | 396.4 | 3,236.4 | 302 | 32 |
| hierarchical | no-wait | 124,404.0 | 62,605.2 | 0.50x | 41,104 | 0 | 37,193.6 | 24,950.5 | 41,136 | 32 |
| hierarchical | wait-die | 102,835.0 | 171,817.0 | 1.67x | 0 | 0 | 40,806.9 | 68,606.2 | 32 | 32 |
| hierarchical | wound-wait | 2,098.6 | 54,738.7 | 26.08x | 364 | 0 | 882.6 | 21,889.6 | 396 | 32 |
| exact-range | no-wait | 46,726.7 | 49,911.5 | 1.07x | 8,083 | 0 | 15,944.1 | 19,945.6 | 8,115 | 32 |
| exact-range | wait-die | 68,395.2 | 57,057.6 | 0.83x | 213 | 0 | 27,428.1 | 22,777.1 | 245 | 32 |
| exact-range | wound-wait | 67.5 | 37,803.2 | 560.38x | 2,094 | 0 | 56.0 | 15,128.2 | 2,125 | 32 |
| precision-lock | no-wait | 389.8 | 25,738.1 | 66.02x | 397 | 0 | 40.0 | 10,278.9 | 429 | 32 |
| precision-lock | wait-die | 436.8 | 19,006.0 | 43.51x | 444 | 0 | 38.0 | 7,579.0 | 476 | 32 |
| precision-lock | wound-wait | 132.4 | 40,683.2 | 307.18x | 1,933 | 0 | 77.5 | 16,267.3 | 1,965 | 32 |

## Attempt and delay costs

Ratios are calculated per trial, then the median is taken. Wait/backoff are sums across workers per committed transaction, not added wall time.

| Algorithm | Policy | Theta | Retry mode | TPS min--max | Aborts/commit | Retries/commit | Lock wait ms/commit | Retry delay ms/commit |
|---|---|---:|---|---:|---:|---:|---:|---:|
| record-2pl | no-wait | 0 | off | 114,762.0--125,014.0 | 0.66 | 0.66 | 0.0000 | 0.0000 |
| record-2pl | no-wait | 0 | adaptive | 103,379.0--124,743.0 | 0.01 | 0.01 | 0.0000 | 0.0003 |
| record-2pl | no-wait | 1.5 | off | 116.4--252.9 | 607.58 | 606.94 | 0.0000 | 0.0000 |
| record-2pl | no-wait | 1.5 | adaptive | 11,504.8--13,145.2 | 6.00 | 6.00 | 0.0000 | 2.2242 |
| record-2pl | wait-die | 0 | off | 109,519.0--124,983.0 | 0.33 | 0.33 | 0.0001 | 0.0000 |
| record-2pl | wait-die | 0 | adaptive | 100,556.0--125,665.0 | 0.01 | 0.01 | 0.0001 | 0.0002 |
| record-2pl | wait-die | 1.5 | off | 193.9--262.9 | 514.94 | 514.40 | 7.2330 | 0.0000 |
| record-2pl | wait-die | 1.5 | adaptive | 7,722.3--10,153.7 | 7.98 | 7.98 | 0.0882 | 2.6221 |
| record-2pl | wound-wait | 0 | off | 118,325.0--128,920.0 | 0.00 | 0.00 | 0.0005 | 0.0000 |
| record-2pl | wound-wait | 0 | adaptive | 100,670.0--111,391.0 | 0.00 | 0.00 | 0.0007 | 0.0000 |
| record-2pl | wound-wait | 1.5 | off | 806.2--1,666.4 | 380.40 | 380.23 | 2.7056 | 0.0000 |
| record-2pl | wound-wait | 1.5 | adaptive | 12,744.7--24,769.0 | 10.70 | 10.70 | 0.0977 | 0.9072 |
| next-key | no-wait | 0 | off | 41,539.7--42,857.6 | 0.02 | 0.02 | 0.0000 | 0.0000 |
| next-key | no-wait | 0 | adaptive | 39,230.4--41,655.0 | 0.01 | 0.01 | 0.0000 | 0.0002 |
| next-key | no-wait | 1.5 | off | 170.9--186.4 | 475.90 | 475.38 | 0.0000 | 0.0000 |
| next-key | no-wait | 1.5 | adaptive | 3,047.5--3,290.3 | 16.63 | 16.62 | 0.0000 | 8.0380 |
| next-key | wait-die | 0 | off | 39,255.7--41,779.7 | 0.02 | 0.02 | 0.0003 | 0.0000 |
| next-key | wait-die | 0 | adaptive | 39,541.1--41,870.5 | 0.01 | 0.01 | 0.0003 | 0.0002 |
| next-key | wait-die | 1.5 | off | 145.4--198.4 | 473.01 | 472.49 | 6.4667 | 0.0000 |
| next-key | wait-die | 1.5 | adaptive | 1,722.9--2,771.9 | 18.65 | 18.65 | 0.3595 | 8.6599 |
| next-key | wound-wait | 0 | off | 40,234.6--40,742.6 | 0.00 | 0.00 | 0.0016 | 0.0000 |
| next-key | wound-wait | 0 | adaptive | 40,386.7--42,764.7 | 0.00 | 0.00 | 0.0007 | 0.0000 |
| next-key | wound-wait | 1.5 | off | 1,064.1--1,263.5 | 316.86 | 316.76 | 2.9078 | 0.0000 |
| next-key | wound-wait | 1.5 | adaptive | 5,546.6--8,645.4 | 14.12 | 14.12 | 0.2916 | 2.4516 |
| gap-lock | no-wait | 0 | off | 39,703.3--42,927.1 | 0.03 | 0.03 | 0.0000 | 0.0000 |
| gap-lock | no-wait | 0 | adaptive | 38,946.0--42,110.1 | 0.01 | 0.01 | 0.0000 | 0.0002 |
| gap-lock | no-wait | 1.5 | off | 156.4--169.4 | 477.37 | 476.86 | 0.0000 | 0.0000 |
| gap-lock | no-wait | 1.5 | adaptive | 1,952.7--2,687.7 | 20.57 | 20.56 | 0.0000 | 10.1666 |
| gap-lock | wait-die | 0 | off | 35,777.0--42,558.6 | 0.04 | 0.04 | 0.0003 | 0.0000 |
| gap-lock | wait-die | 0 | adaptive | 40,991.8--42,639.5 | 0.01 | 0.01 | 0.0003 | 0.0003 |
| gap-lock | wait-die | 1.5 | off | 146.9--178.9 | 495.18 | 494.64 | 6.7106 | 0.0000 |
| gap-lock | wait-die | 1.5 | adaptive | 1,323.0--2,383.2 | 21.08 | 21.07 | 0.4263 | 10.0816 |
| gap-lock | wound-wait | 0 | off | 40,780.9--44,099.3 | 0.00 | 0.00 | 0.0009 | 0.0000 |
| gap-lock | wound-wait | 0 | adaptive | 38,758.3--41,300.8 | 0.00 | 0.00 | 0.0009 | 0.0000 |
| gap-lock | wound-wait | 1.5 | off | 850.2--993.6 | 398.93 | 398.76 | 3.1842 | 0.0000 |
| gap-lock | wound-wait | 1.5 | adaptive | 5,327.9--8,420.9 | 14.56 | 14.56 | 0.3179 | 2.6554 |
| prior-key | no-wait | 0 | off | 38,563.4--42,692.5 | 0.03 | 0.03 | 0.0000 | 0.0000 |
| prior-key | no-wait | 0 | adaptive | 36,413.3--43,092.6 | 0.01 | 0.01 | 0.0000 | 0.0004 |
| prior-key | no-wait | 1.5 | off | 156.9--172.9 | 481.87 | 481.34 | 0.0000 | 0.0000 |
| prior-key | no-wait | 1.5 | adaptive | 2,147.6--3,328.0 | 18.18 | 18.17 | 0.0000 | 8.8612 |
| prior-key | wait-die | 0 | off | 39,730.0--42,150.8 | 0.02 | 0.02 | 0.0003 | 0.0000 |
| prior-key | wait-die | 0 | adaptive | 40,498.2--44,319.5 | 0.01 | 0.01 | 0.0002 | 0.0003 |
| prior-key | wait-die | 1.5 | off | 154.9--181.4 | 477.82 | 477.30 | 7.2678 | 0.0000 |
| prior-key | wait-die | 1.5 | adaptive | 1,576.7--2,784.6 | 18.79 | 18.78 | 0.3662 | 8.7632 |
| prior-key | wound-wait | 0 | off | 39,354.6--44,270.1 | 0.00 | 0.00 | 0.0013 | 0.0000 |
| prior-key | wound-wait | 0 | adaptive | 38,203.7--45,471.7 | 0.00 | 0.00 | 0.0010 | 0.0000 |
| prior-key | wound-wait | 1.5 | off | 685.2--1,171.6 | 342.43 | 342.31 | 3.4964 | 0.0000 |
| prior-key | wound-wait | 1.5 | adaptive | 5,496.5--9,558.8 | 14.29 | 14.29 | 0.3057 | 2.5430 |
| hierarchical | no-wait | 0 | off | 196,252.0--235,977.0 | 3.44 | 3.44 | 0.0000 | 0.0000 |
| hierarchical | no-wait | 0 | adaptive | 228,236.0--255,387.0 | 0.37 | 0.37 | 0.0000 | 0.0040 |
| hierarchical | no-wait | 1.5 | off | 107,105.0--128,050.0 | 272.78 | 272.61 | 0.0000 | 0.0000 |
| hierarchical | no-wait | 1.5 | adaptive | 58,989.6--62,957.6 | 1.82 | 1.82 | 0.0000 | 0.4885 |
| hierarchical | wait-die | 0 | off | 216,986.0--266,644.0 | 2.26 | 2.26 | 0.0005 | 0.0000 |
| hierarchical | wait-die | 0 | adaptive | 201,342.0--291,036.0 | 0.18 | 0.18 | 0.0004 | 0.0040 |
| hierarchical | wait-die | 1.5 | off | 93,036.8--109,785.0 | 45.77 | 45.77 | 0.2145 | 0.0000 |
| hierarchical | wait-die | 1.5 | adaptive | 103,171.0--172,312.0 | 3.02 | 3.02 | 0.0431 | 0.1240 |
| hierarchical | wound-wait | 0 | off | 230,963.0--253,370.0 | 0.03 | 0.03 | 0.0050 | 0.0000 |
| hierarchical | wound-wait | 0 | adaptive | 217,463.0--258,655.0 | 0.01 | 0.01 | 0.0039 | 0.0001 |
| hierarchical | wound-wait | 1.5 | off | 1,790.7--2,916.2 | 244.78 | 244.68 | 12.3151 | 0.0000 |
| hierarchical | wound-wait | 1.5 | adaptive | 29,706.9--66,516.4 | 8.08 | 8.08 | 0.1191 | 0.3998 |
| exact-range | no-wait | 0 | off | 215,198.0--236,442.0 | 2.75 | 2.75 | 0.0000 | 0.0000 |
| exact-range | no-wait | 0 | adaptive | 220,875.0--229,947.0 | 0.15 | 0.15 | 0.0000 | 0.0036 |
| exact-range | no-wait | 1.5 | off | 46,235.3--52,862.4 | 111.09 | 111.00 | 0.0000 | 0.0000 |
| exact-range | no-wait | 1.5 | adaptive | 45,108.4--49,955.8 | 2.97 | 2.97 | 0.0000 | 0.6011 |
| exact-range | wait-die | 0 | off | 182,769.0--268,512.0 | 1.71 | 1.71 | 0.0001 | 0.0000 |
| exact-range | wait-die | 0 | adaptive | 184,309.0--233,143.0 | 0.10 | 0.10 | 0.0001 | 0.0024 |
| exact-range | wait-die | 1.5 | off | 65,204.6--73,709.7 | 32.08 | 32.08 | 0.3532 | 0.0000 |
| exact-range | wait-die | 1.5 | adaptive | 47,723.5--57,351.1 | 4.40 | 4.40 | 0.0789 | 0.4127 |
| exact-range | wound-wait | 0 | off | 192,989.0--216,418.0 | 0.00 | 0.00 | 0.0027 | 0.0000 |
| exact-range | wound-wait | 0 | adaptive | 198,381.0--205,269.0 | 0.00 | 0.00 | 0.0030 | 0.0000 |
| exact-range | wound-wait | 1.5 | off | 66.0--111.5 | 15684.46 | 15668.72 | 194.6385 | 0.0000 |
| exact-range | wound-wait | 1.5 | adaptive | 28,486.6--38,730.4 | 9.60 | 9.60 | 0.0875 | 0.6240 |
| precision-lock | no-wait | 0 | off | 195,258.0--259,325.0 | 1.21 | 1.21 | 0.0000 | 0.0000 |
| precision-lock | no-wait | 0 | adaptive | 209,904.0--226,187.0 | 0.10 | 0.10 | 0.0000 | 0.0018 |
| precision-lock | no-wait | 1.5 | off | 254.9--391.8 | 536.50 | 535.95 | 0.0000 | 0.0000 |
| precision-lock | no-wait | 1.5 | adaptive | 19,938.0--26,080.8 | 3.71 | 3.71 | 0.0000 | 1.1237 |
| precision-lock | wait-die | 0 | off | 194,778.0--222,109.0 | 0.77 | 0.77 | 0.0003 | 0.0000 |
| precision-lock | wait-die | 0 | adaptive | 228,054.0--242,176.0 | 0.08 | 0.08 | 0.0004 | 0.0014 |
| precision-lock | wait-die | 1.5 | off | 277.9--438.8 | 528.11 | 527.56 | 7.0595 | 0.0000 |
| precision-lock | wait-die | 1.5 | adaptive | 15,384.9--19,022.2 | 5.79 | 5.79 | 0.0440 | 1.3994 |
| precision-lock | wound-wait | 0 | off | 197,327.0--235,309.0 | 0.01 | 0.01 | 0.0029 | 0.0000 |
| precision-lock | wound-wait | 0 | adaptive | 200,423.0--241,817.0 | 0.00 | 0.00 | 0.0021 | 0.0000 |
| precision-lock | wound-wait | 1.5 | off | 127.0--163.4 | 7792.64 | 7785.23 | 46.1512 | 0.0000 |
| precision-lock | wound-wait | 1.5 | adaptive | 23,086.9--41,808.1 | 8.98 | 8.98 | 0.0503 | 0.5317 |

## Interpretation

At theta=1.5, retry-exhausted logical transactions across all trials, adaptive: 3; off: 181,984.
A failed transaction now releases all locks before randomized retry pacing. No-wait/wait-die/wound-wait acquisition rules, transaction ages, isolation and attempt caps are unchanged. No global serialization or predicted winners are introduced.
Compare committed transactions containing writes as well as total TPS. The old path can exhaust and discard difficult transactions before proceeding to later read-only transactions. Lower total TPS with fewer exhausted writers is a real tradeoff and is not hidden.
This is a short-window diagnostic with three repetitions, not a proof of starvation freedom or a guarantee for all workloads. Window-end unfinished transactions and run-to-run variability remain.
The YCSB-E variant uses fixed key-width ranges and a finite repeated trace. Insert labels can become overwrites after warmup; keys do not grow without bound.
record-2PL does not provide range phantom protection; algorithm throughput is not an isolation-equivalent ranking.
