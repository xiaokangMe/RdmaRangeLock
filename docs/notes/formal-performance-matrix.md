# 本地与共享数据 RoCE 正式矩阵（2026-11-04）

本说明替换旧的远端锁表矩阵运行方案。历史 CSV/报告保留，不覆盖、不混入新共享数据结果。本地与跨主机是两组独立实验：本地不使用 RDMA，跨主机的索引、记录和物理锁均为共享远端状态。不能把两组吞吐之差全部归因于锁算法。

## 运行入口

在 .66 项目目录执行，源码提交后再正式运行：

```bash
# 两套依次运行；默认 756 个本地 case，432 个跨主机 case
python3 scripts/run_verified_matrices.py --config experiments/configs/formal_shared_10s.json
# 单独选择一套
python3 scripts/run_shared_matrices.py --mode local
python3 scripts/run_shared_matrices.py --mode cross-host
# 保留原 shell 入口和主要 RRL_* 维度变量
scripts/run_formal_matrix.sh --mode local
scripts/run_formal_matrix.sh --mode cross-host
```

三个入口最终调用同一个 `run_shared_matrices.py`，不存在另外一条未经验证的正式路径。默认 Release 构建、每 case 至少十秒、每配置至少三次、seed 8/9/10；正式模式不接受未提交源码或跳过构建。会自动把本机 runner 二进制复制到 .67 的独立临时目录，核对 SHA256；不覆盖 .67 既有项目、二进制或实验。

```bash
# 同一路径的缩小入口检查，明确标为 smoke-not-formal
python3 scripts/run_verified_matrices.py --smoke
scripts/run_formal_matrix.sh --mode both --smoke
# 保留 10000 key / 5000 trace / 10 ops / 32 threads / range 128 的边界配置，
# 只缩短窗口与重复次数；它是定向检查，不是完整正式矩阵
python3 scripts/run_shared_matrices.py --config experiments/configs/shared_matrix_boundary_check.json \
  --smoke --keep-geometry --validation-seconds 10
# 只做容量预检，不启动完整矩阵
scripts/capacity_preflight.sh
```

`--smoke` 默认改为一秒、一重复、128 key、200 trace、4 ops、1/4 线程、range 8、bucket 16；`--keep-geometry` 保留配置中的负载/并发维度；`--validation-seconds` 可在 smoke 下设置 1..600 秒，产物仍标为非正式。

## 公平性和拓扑

| 项目 | 本地矩阵 | 跨主机矩阵 |
|---|---|---|
| 索引/记录 | .66 单个进程的 BTreeLatch | .66 数据服务注册内存内的 BTreeRdma |
| workload | .66 | .67，经 10.0.0.3 访问 .66 |
| 事务锁 | 同进程 LockManager | .66 专用 RemoteLockManager 锁表 |
| 策略协调 | 同进程 coordinator | .66 TCP coordinator |
| CSV 标记 | local-no-rdma / data_backend=local | rdma-cross-host-shared / data_backend=shared-rdma |

各组内部所有协议固定同一个索引实现、布局、order、初始化集合、负载和计时。跨主机数据 QP 为每 worker 独占；锁 QP 默认 min(threads,2)。不同协议配置只改变锁计划。结构写批次使用全局排他 latch，是公开的基线瓶颈。

每个跨主机 case 启动独立的新数据服务、新锁服务和新 coordinator，结束后仅清理该 case 自己创建的进程。数据服务先按共同 trace 生成初始集合；客户端只验证指纹，不重新灌数。传输使用 RoCE GID 3。端口自动分配，完整命令落日志。不要把不同 case、协议、coordinator 的客户端接到同一个表，也不要单独重启其中一个服务继续测量。

默认维度：4 workload（YCSB-A/E、项目 Payment/New Order 子负载），10000 key，5000 条循环 trace，每事务 10 ops（New Order 实际操作数由 items 决定），Zipf 0.99，items 5，线程 1/2/4/8/16/32，YCSB-E range 8/32/128，hierarchy bucket 16/64/256，三次重复。它们不是完整 TPC-C。

`wait_policies` 可设三策略的任意子集，所有协议统一 `max_attempts`。默认单个 no-wait，故默认 case 数仍为 756/432；策略维度扩大时实际 case 数自动计算，none 基线每个重复只执行一次。缓冲写入在 PrepareCommit 后应用，计时从所有 worker 缓冲准备完成后开始。

## 容量与数据门禁

跨主机测量前，按配置中最大的 range/线程/操作数，对每个 RDMA 协议、每个 hierarchy bucket、每个 seed 和策略运行预检；默认 12 点，每点十秒。数据页 8192、记录 262144、order 128，均可配置。物理表为 16 shard × 8192 slot；每 attempt metadata 上限 16384，不能把它当成物理容量。页/记录在写前对整个批次做保守容量检查，拒绝部分写入。

硬错误、非零 capacity/gate 中止、错误 CSV、计数不守恒、窗口不足或跨主机 `data_backend` 错误，会使流程失败并停止，不跳过失败 case。no-wait 高冲突下可能在短窗口没有提交；保留真实的 0 提交与 `progress_status=no-commits`，状态记录 no_progress_cases。零样本延迟不代表零延迟，不允许据此宣称尾延迟改善。

输出在 `experiments/results/<kind>-<mode>-<timestamp>.csv` 和同名 campaign JSON；每 case 的原始 stdout、stderr、配置、命令、服务日志，以及源码/二进制 SHA256 在 `experiments/logs/`。进度文件 `/tmp/rrl-shared-campaign-state.json` 不覆盖旧 campaign 的监控文件。源码、配置与实际 runner 二进制均被记录；禁止把 smoke、旧锁服务 CSV 与新正式数据拼接。

## 环境变量兼容

shell 入口保留 RRL_KEYS、TRANSACTIONS、OPERATIONS、ZIPF、TPCC_ITEMS、RUNS、THREADS、WORKLOADS、RANGE_LENGTHS、BUCKETS、LOCAL_CCS、RDMA_CCS；新增 RRL_WAIT_POLICIES、MAX_ATTEMPTS、TREE_ORDER、TREE_PAGES、TREE_RECORDS、SERVER_ROCE。保留 RRL_SSH_KEY、REMOTE_HOST、BUILD_DIR、RDMA_GID_INDEX。旧固定端口、覆盖远端源码目录和复用旧构建的参数已由独立临时目录、自动端口、二进制校验取代，推荐使用 JSON 配置。

旧 `run_rdma_smoke.sh`、`run_rdma_profile.py`、`run_hierarchy_ycsba.py` 仍保留为历史锁服务专项工具，内部显式传 --rdma-lock-only 1，不能生成新的共享数据主实验结论。


## 新结果绘图

使用明确的 campaign 文件，不自动寻找“最近的 CSV”，以免选中旧锁服务数据：

```bash
python3 scripts/report_shared_matrices.py --campaign experiments/results/formal-shared-data-<timestamp>.json \
  --out figures/shared-data-<timestamp>
```

报告逐项核对源码提交、结果类别、backend、case 维度、种子、计数守恒和错误字段，再输出 summary.csv、PNG 和 report.md。历史锁服务 campaign 会被拒绝；smoke 必须显式传 --allow-smoke，图上标记 NOT FORMAL PERFORMANCE。新旧报告工具各自保留，不修改历史图表。
