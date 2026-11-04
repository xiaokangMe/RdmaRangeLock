# RdmaRangeLock

范围查询与范围锁的综述研究 + 最小可复现实现集。

本仓库同时承担两件事：

1. **一篇综述文章**：系统梳理范围查询在并发控制中的语义问题，以及 next-key lock、gap lock、层级锁、latch coupling 等方案的演化关系。
2. **一套最小实现**：用同一棵 B+ 树、同一套 workload，逐个叠加锁机制，得到可对比、可复现的实验数据。

核心主张：**综述不能只靠讲故事**。每一类锁机制都要有一份能跑的实现，用同一组 workload 测出差异，文章才有说服力。

---

## 一、六个实现版本（路线图）

| # | 名称 | 目录 | 核心机制 | 关键不变式 | 主要对比指标 |
|---|---|---|---|---|---|
| v1 | 单线程 B+树 | `src/index/` | 无并发控制 | 树结构始终有效 | 基线：吞吐、树高、点查/范围查延迟 |
| v2 | 并发 B+树 + Latch Coupling | `src/index/` | crabbing（分段加锁、父子 latch 交替） | latch 顺序自顶向下，无死锁 | 多核扩展性、latch 争用率 |
| v3 | B+树 + Record-Level Strict 2PL | `src/lock/` | 仅锁记录，不加 gap | 已提交读/可重复读语义 | 幻读发生率、并发度 |
| v4 | B+树 + Gap Lock | `src/lock/` | 记录锁 + 间隙锁 | 范围谓词稳定 | 锁数量、阻塞率、幻读是否消除 |
| v5 | B+树 + Next-Key Lock | `src/lock/` | 记录锁与间隙锁合并为一个区间锁 | 区间 (prev, curr] 互斥 | 与 v4 的锁开销/正确性差异 |
| v6 | B+树 + Next-Key Lock + Hierarchical Locking | `src/lock/` | 在 next-key 之上引入多粒度意向锁（IS/IX/S/X） | 意向锁沿树路径向上传播 | 大范围扫描的锁开销下降幅度 |

**递进关系**：v1→v2 解决"能不能并发"，v2→v3 解决"事务语义"，v3→v4→v5 解决"范围谓词的幻读"，v5→v6 解决"大范围锁的代价"。

这张表就是文章的骨架：每一行对应论文章节里的一节，每一节的结论都要有 v(N) 与 v(N-1) 的实测数据支撑。

---

## 二、目录约定

沿用本机既有系统类项目（deneva / Writeflex）的组织习惯：**按子系统分目录，每类协议独立文件，benchmark 与统计、画图脚本分离**。

```
RdmaRangeLock/
├── include/            公共头文件，与 src/ 镜像对应
│   ├── common/         基础类型、配置、日志、时间戳
│   ├── index/          B+树接口（页布局、节点、迭代器）
│   └── lock/           锁管理器接口（lock mode、lock table、hierarchy）
├── src/
│   ├── common/         工具实现
│   ├── index/          v1 单线程 B+树、v2 latch coupling
│   ├── lock/           v3 record 2PL、v4 gap lock、v5 next-key、v6 + hierarchical
│   └── txn/            事务骨架、调度器、workload 驱动（各版本共用）
├── benchmarks/         负载与数据
│   ├── schema/         表结构定义
│   └── workload/       点查 / 范围查 / 混合读写 / 幻读构造用例
├── experiments/        实验配置与产出
│   ├── configs/        每个实验一份配置（版本、并发度、workload 参数）
│   ├── results/        汇总后的 csv/json（原始大文件不入库）
│   └── logs/           运行日志
├── statistics/         运行时统计采集（锁等待、latch 争用、重试次数）
├── scripts/            构建、批量跑实验、出图
├── docs/
│   ├── survey/         综述正文（按章节拆分，便于逐章迭代）
│   ├── refs/           参考文献
│   └── notes/          阅读笔记、设计讨论、待验证猜想
├── figures/            成品图（脚本生成，不放手工截图）
├── test/               单元测试与正确性验证（幻读检测用例放这里）
└── thirdparty/         第三方依赖（若无则留空）
```

---

## 三、写作与实现的对应关系

文章的每一节都要能指回仓库里的具体文件：

- 讲 next-key lock 的定义 → `src/lock/v5_next_key_lock.cpp` 是它的可执行定义
- 说"gap lock 的锁数量是 next-key 的两倍" → `experiments/results/` 里有对应 csv
- 说"层级锁把大范围扫描的锁开销降下来" → `figures/` 里有 v5/v6 的对比图

**没有对应实现的论断，不写进文章。**

---

## 四、里程碑

- [x] M1 接口与区间锁表示：完成 deneva 风格类型、显式区间和 LockManager，Debug/Release 均通过
- [x] M2 v1 正确性：单线程 B+树通过插入/删除/范围扫描测试
- [x] M3 v2 并发：64 线程、65536 个 key 的 1 小时压力测试通过，无死锁和结构破坏
  压力测试命令：`scripts/run_v2_stress.sh [threads] [seconds] [keys_per_thread]`。
- [x] M4 v3~v5 语义：三个版本跑同一组幻读用例，结果可解释
- [x] M5 v6 层级锁：意向锁传播正确，大范围扫描锁数量下降可量化
- [ ] M6 综述初稿：分类学表格 + 六组对比数据齐备

---

## 五、工程约定

- **一个版本一次提交**，提交信息写明实现了哪个不变式
- 任何性能结论必须附 `experiments/configs/` 里的配置文件与 `results/` 里的原始数据
- 图必须由 `scripts/` 下的脚本生成，`figures/` 只存产物
- 原始数据集、编译产物、运行日志不入库（见 `.gitignore`）

---

代码层面的硬约束见 [CONSTRAINTS.md](CONSTRAINTS.md)；违反其中任何一条，对应提交直接回退。

## 六、共享数据 RDMA 基线（2026-11-04）

单边 RDMA 数据路径按 Deneva 的根/子节点/叶链偏移寻址方式接入：`.66` 数据服务的注册内存保存权威 B+ 树和 uint64 记录；`.67` 事务进程逐层 RDMA READ 查树、读取记录，提交时 RDMA WRITE 写回。所有 RDMA 锁协议共用这一条路径，不在客户端保留独立数据库。

`.66` 分别启动专用数据、物理锁表、时间戳协调器服务。旧兼容路径的协调面走 TCP；新的 native-rdma-state 路径把事务状态、数据和物理锁槽都放在注册内存，以单边 RDMA 访问。TCP 用于 QP/MR 建链交换、连接存活和故障/断连通知，不处理每次事务或锁授权请求。结构采用共享/排他 latch，写批次串行；删除为 tombstone，容量固定且可配置。当前是可验证的共享数据基线，不能据此宣称已达到 Sherman 等索引的扩展性。

```bash
cmake -S . -B build-shared -DCMAKE_BUILD_TYPE=Release
cmake --build build-shared -j4
# 全 CTest、独立进程/双机共享可见性与小窗口负载验证
python3 scripts/check_shared_rdma.py --build build-shared --cross-host
# 使用与正式矩阵完全相同的执行路径，缩小维度和时间做入口验证
python3 scripts/run_verified_matrices.py --smoke --build build-shared
```

本地正式矩阵使用 `BTreeLatch`，不使用 RDMA；跨主机正式矩阵使用 `BTreeRdma`。新入口每个远端 case 自动启动全新的数据/锁/协调服务，校验源码与二进制、初始数据指纹，先过容量预检，失败即停止。完整配置与运行方法见 [正式矩阵说明](docs/notes/formal-performance-matrix.md)。

旧 RDMA 图表只有远端锁表、客户端各自持有本地索引和记录，保留为锁服务工程微基准；不能当成共享数据库主实验。旧路径须显式 `--rdma-lock-only 1` 才允许运行，CSV 的 `data_backend=local`。新主实验必须为 `index=rdma,data_backend=shared-rdma`。

## 等待策略与延迟测量

本地和共享 RDMA 均支持 record/gap/next-key/hierarchy/predicate，冲突策略可选 `--wait-policy no-wait|wait-die|wound-wait`。同一个事务驱动负责锁计划、缓冲写入、读自己之前的写入、提交许可、批量应用与严格释放。事务 age 跨重试保持，attempt ID 每次更新。

显式策略模式输出 `metrics_schema=2`：成功 attempt 与含重试逻辑事务各自的 p50/p95/p99/p999，等待时间、重试次数、未完成/失败计数。所有 CSV 增加 `data_backend,data_rdma_operations,data_read_bytes,data_write_bytes,data_latch_wait_ns`；后四项只统计测量期间的数据路径，不含连接和最终结构校验。原 `rdma_*` 仍为物理锁路径计数，不把两类操作混淆。

具体边界、故障与正确性记录见 [迁移方案](docs/notes/deneva-shared-rdma-data-2026-11-03.md) 和 [共享数据验收记录](docs/notes/shared-rdma-acceptance-2026-11-04.md)。

## Deneva 式原生事务运行时

实现依据与逐文件来源见 [原生运行时移植说明](docs/notes/deneva-native-runtime-2026-11-04.md)。新增被动事务状态服务、每 worker 多个栈式协程、并发 warm-up、排空/测量屏障和独立流量快照。设备 context/PD 按进程共享，QP/CQ/缓冲仍按上下文隔离，避免大并发建链重复消耗设备 FD。

- 原生路径使用 --txn-state-port；旧 --coordinator-port 显式选择 TCP 兼容路径，二者互斥。TCP 兼容路径只支持每 worker 一个协程。
- --threads 是 OS worker 数，--coroutines 是每 worker 协程数；并发事务数不超过二者乘积（也不超过 trace 事务数）。
- --warmup 为每个 case 的并发预热秒数，--duration 为排空后独立测量秒数。预热数据保留，预热吞吐、延迟和 RDMA 流量不混入正式计数。
- 新增 CSV 字段包括 coroutines、active_contexts、warmup_seconds、warmup_committed、warmup_aborted、warmup_elapsed_ns、coroutine_switches_total、state_rdma_operations；coroutine_switches_total 含启动与预热切换，其余测量流量按阶段截取。硬错误计数保留整轮，包括预热错误。
- 每个原生状态服务限定一种等待策略；每 case 创建新服务，禁止把不同策略的事务放进同一个 wait-for graph。状态服务默认 4096 个会话槽，attempt 复用会话槽且带独立 generation；会话槽在服务生存期内不回收。
- 本地矩阵继续使用本地索引/本地锁；新正式配置为 experiments/configs/formal_native_policies_10s.json（4 协程、10 秒预热、10 秒测量、3 次重复）。原矩阵入口继续可用；配置与 CSV 明确记录协调后端，恢复/报告禁止混入旧后端、旧提交或旧预热配置。

验证入口（不启动完整正式矩阵）：

    cmake -S . -B build-shared -DCMAKE_BUILD_TYPE=Release
    cmake --build build-shared -j4
    python3 scripts/check_shared_rdma.py --build build-shared --cross-host --only contracts
    python3 scripts/check_native_runtime.py --build build-shared --cross-host
    python3 scripts/run_shared_matrices.py --config experiments/configs/native_runtime_smoke.json --mode both --smoke --no-build --keep-geometry

新运行时不等于完整 Deneva 数据库：现有 Payment/New Order 仍为简化 trace；hierarchical 仍为逻辑 root/bucket/record，不是真实叶页锁；共享索引写批次仍有全局结构 latch，年龄分配字也可能成为热点。相关性能结论须由新的正式矩阵支持。


YCSB 请求数遵循 Deneva 的可配置模型，默认 `REQ_PER_QUERY=10` 对应本项目 `--ops 10`；扫描长度由 `--range` 单独配置。默认事务内请求起始键去重且保持生成顺序（对应 Deneva `KEY_ORDER=false`）；区间可重叠。重复键专项验证显式使用 `--ycsb-unique-keys 0` / 配置 `ycsb_unique_keys: 0`，不能混作默认正式结果。TPC-C 子负载按其业务 trace 决定长度，`--ops` 不决定 Payment/New Order 操作数。详见 [Deneva 负载适配](docs/notes/deneva-ycsb-generation-2026-11-04.md)。

原生运行时的物理锁服务使用 `--native-fail-stop`：断开的 QP 若仍持有锁或 gate，立即停止该物理服务，不再清锁后授权；无残留的正常断开继续服务。此为保证失败实验不继续运行的策略，不提供跨故障恢复提交的能力。


两百万 key 配置：`experiments/configs/deneva_scale_2m.json` 用于单 case，`formal_native_deneva_2m_10s.json` 用于完整维度（尚未自动运行正式矩阵）。两者均配置 2,097,152 key 域、10 请求、10 秒预热与 10 秒测量，并同步增大数据页/记录容量。`deneva_scale_2m_boundary.json` 是小维度验证配置，执行 `python3 scripts/run_shared_matrices.py --config experiments/configs/deneva_scale_2m_boundary.json --mode both --smoke --keep-geometry --validation-seconds 10`，结果仍为非正式。规模、元数据 gate 与真实叶节点锁的区别见 [源码核查](docs/notes/deneva-gate-scale-warmup-2026-11-04.md)。
