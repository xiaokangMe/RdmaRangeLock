# 正式性能实验矩阵

本文档描述 `scripts/run_formal_matrix.sh` 的实验计划。脚本只在显式传入 `--mode` 后运行；本次编写阶段没有启动任何实验。

## 实验目标

固定 v2 并发 B+树，把变量集中在范围并发控制和 RDMA 传输路径上，比较：

- 本地无锁数据通路：`none`；
- 本地记录级 Strict 2PL：`record-2pl`；
- 本地 Gap Lock：`gap-lock`；
- 本地 Next-Key Lock：`next-key`；
- 本地层级锁：`hierarchical`；
- RDMA Next-Key Lock：`rdma-next-key`；
- RDMA 层级锁：`rdma-hierarchical`。

v1 单线程树和 v2 latch coupling 的结构基线由已有测试和 `scripts/run_v2_stress.sh` 单独验证；本矩阵固定 `--index v2`，避免把索引结构变化混入锁协议对比。

## 默认矩阵

| 维度 | 默认值 |
|---|---|
| workload | `ycsb-a`, `ycsb-e`, `tpcc-payment`, `tpcc-new-order` |
| key 数 | 10000 |
| transaction 数 | 5000 |
| 每事务操作数 | 10 |
| YCSB-E range length | 8, 32, 128 |
| YCSB-A/TPCC range length | 0（不产生范围读） |
| YCSB Zipf | 0.99 |
| TPCC items/order | 5 |
| 线程数 | 1, 2, 4, 8, 16, 32 |
| 重复次数 | 3 |
| seed | 第 1/2/3 次为 8/9/10；同一 run 在各协议间保持一致 |
| 层级锁 bucket | 16, 64, 256 |
| 单 case 超时 | 900 秒 |

默认规模为 720 个本地 case 和 432 个跨主机 RDMA case。正式运行前建议先用环境变量缩小矩阵做一轮连通性检查。

## 运行位置和拓扑

脚本应在 `.66` 的项目根目录执行：

```bash
cd /home/dell/RdmaRangeLock
```

`--mode local` 运行本地 `none/record-2pl/gap-lock/next-key/hierarchical`；不需要 RDMA 服务端。

`--mode cross-host` 由 `.66` 启动锁服务端，并通过 SSH 在 `.67` 启动 workload runner。默认使用：

- `.66` server：`10.0.0.3`；
- `.67` client：`10.0.0.4`；
- `RRL_RDMA_GID_INDEX=3`；
- TCP/RDMA 服务端口：`19930`；
- `.67` 源码目录：`/home/dell/RdmaRangeLock`；
- `.67` 构建目录：`/tmp/rrl-formal-build`。

跨主机模式默认先把 `.66` 当前源码同步到 `.67`，再分别构建两端。同步会排除 `.git`、构建目录、实验结果和日志；不会执行实验。

## 使用方法

先做一个小矩阵验证脚本和机器环境：

```bash
RRL_RUNS=1 \
RRL_THREADS=1,2 \
RRL_WORKLOADS=ycsb-e \
RRL_RANGE_LENGTHS=32 \
RRL_TRANSACTIONS=100 \
scripts/run_formal_matrix.sh --mode local
```

跨主机小矩阵：

```bash
RRL_RUNS=1 \
RRL_THREADS=1,4 \
RRL_WORKLOADS=ycsb-e \
RRL_RANGE_LENGTHS=32 \
RRL_BUCKETS=64 \
RRL_TRANSACTIONS=100 \
scripts/run_formal_matrix.sh --mode cross-host
```

完整矩阵：

```bash
scripts/run_formal_matrix.sh --mode local
scripts/run_formal_matrix.sh --mode cross-host
```

常用覆盖变量：

- `RRL_KEYS`, `RRL_TRANSACTIONS`, `RRL_OPERATIONS`：负载规模；
- `RRL_THREADS`, `RRL_WORKLOADS`, `RRL_RANGE_LENGTHS`, `RRL_BUCKETS`：矩阵维度，逗号分隔；
- `RRL_RUNS`, `RRL_ZIPF`, `RRL_TPCC_ITEMS`：重复次数和 workload 参数；
- `RRL_RDMA_PORT`, `RRL_RDMA_GID_INDEX`：跨主机 RDMA 参数；
- `RRL_REMOTE_HOST`, `RRL_REMOTE_ROOT`, `RRL_REMOTE_BUILD_DIR`：`.67` 连接和目录；
- `RRL_SYNC_REMOTE=0`：使用 `.67` 已有源码，不同步；
- `RRL_RESULT_DIR`, `RRL_LOG_DIR`：结果和日志输出目录；
- `RRL_TIMEOUT_SECONDS`, `RRL_BUILD_JOBS`：超时和构建并行度。

## 输出格式

结果写入 `experiments/results/formal-<mode>-<timestamp>.csv`，日志写入 `experiments/logs/formal-<mode>-<timestamp>/`。CSV 在 runner 原有字段前增加：

```text
transport,server_host,client_host,run,seed,range,bucket,zipf
```

runner 字段包括 committed/aborted、attempted/committed throughput、lock conflicts、rows rechecked 等。结果目录已被 `.gitignore` 忽略，正式实验结果应另行归档并保留脚本版本、commit、机器信息和完整命令行。

## 统计和公平性约定

- 每个 case 都从新进程和新 trace 开始；RDMA 连接建立和索引预填充不计入 runner 的事务 elapsed time。
- 以 `throughput_committed_txn_s` 作为成功事务吞吐，同时报告 `throughput_attempt_txn_s`、committed、aborted 和 lock conflicts。
- 当前锁协议是 no-wait；`Abort` 表示冲突或容量失败，不能直接解释成阻塞时间。
- 本地与跨主机结果分开比较。跨主机模式还应记录 GID、网卡、链路状态、CPU 型号和 kernel/verbs 版本。
- 正式报告至少保留 3 次重复的中位数和离散度；对吞吐、abort ratio、lock acquire cost、rows rechecked 和 range length 分别作图。
- 结果出现异常时先查看对应 case 的 stderr 日志，再复现单 case；不要用 sleep 或重试掩盖连接、锁顺序或数据一致性错误。

## 预期后续分析

1. 本地 `record-2pl → gap-lock → next-key → hierarchical` 的锁数量、范围扫描成本和提交吞吐变化。
2. `next-key` 与 `rdma-next-key` 的跨传输层开销。
3. `hierarchical` 的 bucket=16/64/256 扫描和冲突变化。
4. YCSB-E 与 TPCC New-Order 的范围/热点差异。
5. 多线程扩展曲线和 no-wait abort ratio，单独标注 RDMA 单机与跨主机结果。
