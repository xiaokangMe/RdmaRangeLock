# 本地与共享数据 RoCE 正式矩阵（2026-11-04）

本说明替换旧的远端锁表矩阵运行方案。历史 CSV/报告保留，不覆盖、不混入新共享数据结果。本地与跨主机是两组独立实验：本地不使用 RDMA，跨主机的索引、记录和物理锁均为共享远端状态。不能把两组吞吐之差全部归因于锁算法。

## 运行入口

在 .66 项目目录执行，源码提交后再正式运行：

```bash
# 新原生运行时配置：4 协程、10 秒预热、10 秒测量、3 重复
python3 scripts/run_verified_matrices.py --config experiments/configs/formal_native_policies_10s.json
# 原维度配置仍可运行：默认 756 个本地 case，432 个跨主机 case
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
| 策略协调 | 同进程 coordinator | .66 被动 RDMA 事务状态表（native-rdma-state）；显式兼容配置可选 TCP |
| CSV 标记 | local-no-rdma / data_backend=local | rdma-cross-host-shared / data_backend=shared-rdma |

各组内部所有协议固定同一个索引实现、布局、order、初始化集合、负载和计时。原生跨主机数据/状态 QP 按执行上下文（worker × coroutine）隔离；锁 QP 默认按上下文配置，显式 rdma_connections 可覆盖。不同协议配置只改变锁计划。结构写批次使用全局排他 latch，是公开的基线瓶颈。

每个跨主机 case 启动独立的新数据服务、新锁服务和新 coordinator，结束后仅清理该 case 自己创建的进程。数据服务先按共同 trace 生成初始集合；客户端只验证指纹，不重新灌数。传输使用 RoCE GID 3。端口自动分配，完整命令落日志。不要把不同 case、协议、coordinator 的客户端接到同一个表，也不要单独重启其中一个服务继续测量。

默认维度：4 workload（YCSB-A/E、项目 Payment/New Order 子负载），10000 key，5000 条循环 trace，YCSB 每事务默认 10 个请求（--ops 可配；Payment 固定业务 trace，New Order 由 items 决定），Zipf 0.99，items 5，线程 1/2/4/8/16/32，YCSB-E range 8/32/128，hierarchy bucket 16/64/256，三次重复。它们不是完整 TPC-C。

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

## 原生运行时配置（2026-11-04 增补）

新入口默认 coordination_backend=native-rdma-state；coroutines 默认 1，warmup_seconds 默认 10。要复现新 4 协程设置使用 formal_native_policies_10s.json，结果记录完整解析后的配置，不能只根据旧配置文件缺省字段推断历史结果。

等价环境变量为 RRL_COORDINATION_BACKEND、RRL_COROUTINES、RRL_COROUTINE_STACK_BYTES、RRL_WARMUP_SECONDS，供 run_formal_matrix.sh 使用。每 case 的预热与正式事务以相同 worker/协程/协议运行，屏障排空后独立采集延迟和流量；预热错误仍令整轮失效。native 模式不启动 TCP coordinator；TCP 用于 QP/MR 建链交换、连接存活和故障/断连通知，不进行逐事务或逐锁授权。

状态服务默认 4096 个会话槽；同一会话的 attempt 按 generation 复用槽。物理锁槽容量保持原配置，max(worker) × coroutines 不得超过运行时/状态槽上限；正式执行之前必须通过相同最大并发、range、ops 和 seed 的容量门禁。lock_budget 对本地/TCP 元数据协调有效；原生授权容量由物理槽和会话槽单独约束，不把该数误作物理锁槽数。

原生检查会固定二进制并记录 SHA-256；新配置不能恢复或汇总旧源码/后端/预热/协程设置的 CSV。纯验证请使用 native_runtime_smoke.json 或 --preflight-only --smoke --keep-geometry，不把缩短时间的数据当成正式结果。源码映射与验收边界见 native-runtime-acceptance-2026-11-04.md。


恢复 CSV 还必须携带 campaign_config_sha256（全部解析配置的指纹）。即使源码相同，也拒绝 key_count、ops、zipf、duration 等配置变化；缺失指纹的历史结果不能拼接进新运行时。none 基线在恢复时归入首个 policy 槽，避免因报告为 none 而重复执行。


独立验证可加 --artifact-root DIR（在 DIR 下写 logs/results）和 --state-file FILE（独立状态快照）。CTest 的恢复集成测试使用临时目录和独立状态文件，不覆盖正在运行 campaign 的全局监控快照。


YCSB 事务长度与 Deneva 的 REQ_PER_QUERY 对齐，默认 10 个请求但始终可配置；一次 scan 可读取多条记录，不计为多条请求。默认 ycsb_unique_keys=1，对事务内请求起始 key 去重并保留生成顺序（Deneva 当前 KEY_ORDER=false）。配置、每 case 的服务端数据集/客户端 CLI、CSV 和完整配置指纹均保存该语义；RRL_YCSB_UNIQUE_KEYS=0 仅用于显式重复键消融/正确性验证，不能与默认结果混合。起始键唯一不代表扫描区间不相交。TPC-C 简化子负载独立于该参数。

原生物理锁服务启用 --native-fail-stop，断开 owner 若仍有锁或 gate，停止该 case，绝不清锁后重新授权；不单独重启服务接续测量。正常释放后的断开继续服务。该故障策略与旧兼容恢复路径分别验证。


两百万 key 域的完整维度配置为 formal_native_deneva_2m_10s.json，数据页/记录容量同步设为 131072/4194304。先用 `--mode cross-host --preflight-only --smoke --keep-geometry --validation-seconds 10` 验证最大并发/扫描几何；该命令只做容量检查，保留十秒预热/十秒窗口，仍标为非正式。小维度入口验证用 deneva_scale_2m_boundary.json。A 初始记录数等于 key 域大小；E 留有插入空位，实际行数见每 case 数据服务日志，不把二者都声称成满表。详细源码对照与验证记录见 deneva-gate-scale-warmup-2026-11-04.md。
