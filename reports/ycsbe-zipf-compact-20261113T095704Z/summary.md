# YCSB-E Compact Lock Zipf Sweep

单机 `.66`，单次 campaign：32 个线程，`2^21 = 2,097,152` keys，5,000 条 trace 事务、每事务 10 个操作、range 32、seed 8；预热 10 秒、测量 10 秒。YCSB-E trace 使用 ordered Zipf；theta=0 对应均匀采样。全部 135 个 case 成功，未发生 runner 错误。本报告是单次探索性测量，不包含独立重复轮次或置信区间。

层级锁固定 bucket width=64、implicit root=1、padding=0。五种实现均使用 compact 锁表及 full metrics。吞吐是提交事务/秒；重试、中止、等待和未完成事务是各 theta 九个 case 的累积计数，`abort/commit`、`retry/commit` 以累计提交数归一化，wait ms/commit 以累计提交数归一化。

## Committed TPS

### no-wait

| Zipf theta | record-2pl | next-key | gap-lock | prior-key | hierarchical |
|---:|---:|---:|---:|---:|---:|
| 0 | 108,863 | 37,381 | 35,663 | 35,019 | 237,932 |
| 0.1 | 95,746 | 40,421 | 37,757 | 38,712 | 241,168 |
| 0.3 | 103,166 | 37,708 | 35,634 | 38,961 | 228,076 |
| 0.5 | 128,635 | 33,628 | 35,914 | 36,212 | 233,799 |
| 0.7 | 99,315 | 32,440 | 34,660 | 34,974 | 234,795 |
| 0.9 | 4,042 | 1,278 | 1,292 | 1,238 | 168,678 |
| 1.1 | 575 | 282 | 265 | 273 | 107,474 |
| 1.3 | 345 | 157 | 157 | 151 | 88,284 |
| 1.5 | 234 | 156 | 144 | 141 | 111,562 |

### wait-die

| Zipf theta | record-2pl | next-key | gap-lock | prior-key | hierarchical |
|---:|---:|---:|---:|---:|---:|
| 0 | 129,791 | 36,830 | 37,439 | 35,996 | 245,294 |
| 0.1 | 108,645 | 36,419 | 39,580 | 38,371 | 235,511 |
| 0.3 | 106,374 | 36,425 | 37,153 | 38,495 | 245,528 |
| 0.5 | 109,118 | 35,017 | 35,807 | 35,610 | 202,883 |
| 0.7 | 113,523 | 34,974 | 37,803 | 36,172 | 232,375 |
| 0.9 | 2,200 | 962 | 1,070 | 957 | 120,991 |
| 1.1 | 552 | 244 | 241 | 259 | 99,014 |
| 1.3 | 296 | 153 | 154 | 156 | 89,026 |
| 1.5 | 208 | 143 | 142 | 147 | 91,729 |

### wound-wait

| Zipf theta | record-2pl | next-key | gap-lock | prior-key | hierarchical |
|---:|---:|---:|---:|---:|---:|
| 0 | 98,147 | 34,427 | 37,270 | 35,764 | 260,014 |
| 0.1 | 102,047 | 38,479 | 36,518 | 39,151 | 254,403 |
| 0.3 | 96,404 | 38,463 | 39,066 | 39,200 | 192,367 |
| 0.5 | 109,322 | 35,886 | 34,977 | 36,244 | 25,099 |
| 0.7 | 98,637 | 36,917 | 38,096 | 37,898 | 21,229 |
| 0.9 | 26,447 | 12,548 | 12,690 | 12,847 | 5,868 |
| 1.1 | 3,723 | 3,978 | 3,212 | 3,873 | 2,514 |
| 1.3 | 1,238 | 2,152 | 1,538 | 2,076 | 2,088 |
| 1.5 | 1,275 | 1,385 | 1,012 | 1,264 | 3,563 |

## Conflict And Completion

| Algorithm | Policy | Mean TPS across theta | Aborts / commit | Retries / commit | Wait ms / commit | Retry exhausted | Deadline aborts | Unfinished |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| record-2pl | no-wait | 60,102 | 1.97 | 1.97 | 0.0000 | 6,582 | 284 | 6,866 |
| record-2pl | wait-die | 63,412 | 1.44 | 1.44 | 0.0198 | 5,810 | 1,247 | 6,096 |
| record-2pl | wound-wait | 59,693 | 2.62 | 2.62 | 0.0322 | 3,593 | 296 | 3,877 |
| next-key | no-wait | 20,383 | 1.72 | 1.72 | 0.0000 | 2,716 | 282 | 3,003 |
| next-key | wait-die | 20,130 | 1.67 | 1.67 | 0.0727 | 2,664 | 1,433 | 2,951 |
| next-key | wound-wait | 22,693 | 4.52 | 4.52 | 0.1055 | 1,242 | 330 | 1,530 |
| gap-lock | no-wait | 20,165 | 1.69 | 1.69 | 0.0000 | 2,636 | 279 | 2,920 |
| gap-lock | wait-die | 21,043 | 1.59 | 1.58 | 0.0452 | 2,615 | 961 | 2,900 |
| gap-lock | wound-wait | 22,709 | 4.29 | 4.29 | 0.0959 | 1,880 | 357 | 2,165 |
| prior-key | no-wait | 20,631 | 1.65 | 1.64 | 0.0000 | 2,632 | 273 | 2,917 |
| prior-key | wait-die | 20,685 | 1.64 | 1.64 | 0.0532 | 2,675 | 1,102 | 2,958 |
| prior-key | wound-wait | 23,146 | 4.29 | 4.29 | 0.1019 | 1,241 | 336 | 1,526 |
| hierarchical | no-wait | 183,530 | 62.87 | 62.83 | 0.0000 | 723,594 | 161 | 723,872 |
| hierarchical | wait-die | 173,595 | 11.69 | 11.69 | 0.0651 | 46 | 250 | 325 |
| hierarchical | wound-wait | 85,238 | 4.36 | 4.36 | 0.1837 | 12,651 | 938 | 12,929 |

## Interpretation Notes

- record-2PL is a record-lock baseline and does not provide phantom protection for range scans; it is not isolation-equivalent to next-key, gap, prior-key, or hierarchical bucket locking.
- The hierarchical no-wait results have high retry exhaustion and unfinished transaction counts in the more contended cases. Read its throughput beside the completion columns; high attempt throughput does not imply all logical transactions completed.
- theta=0.9 and above changes throughput sharply for several strategies. Since each point is a single run, confirm those changes with at least three independently repeated, order-balanced rounds before drawing a performance conclusion.
- This is the project runner's YCSB-E variant: 95% range-read / 5% insert request mix, fixed key-width scans, finite keys, and a repeated trace. Inserts after warmup may overwrite existing keys; this is not a growing-keyspace insert benchmark.
- Raw per-case commands, stdout/stderr, JSON, CSV, environment, branch, source hashes, and config snapshot are retained beside this report.
